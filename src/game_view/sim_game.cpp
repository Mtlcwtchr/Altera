// The life-sim in the world: altera_sim steps on a thread of its own, the
// world view draws its people, beasts and buildings as placeholder shapes on
// the real ground, and an immediate-mode HUD (engine ui, style G of the design
// canvas "Altera UI и HUD") shows the selected character, the clock, the
// chronicle and the player's orders.
//
// Every text on screen comes from content/loc (game.* keys in game_ui.json and
// the sim's own names), so this file holds no display strings.

#include "game_view/cutaway.hpp"
#include "game_view/game_layer.hpp"
#include "game_view/hud.hpp"
#include "game_view/hud_style.hpp"
#include "game_view/ui_theme.hpp"
#include "game_view/saves.hpp"
#include "game_view/skinned.hpp"
#include "game_view/snapshot.hpp"
#include "game_view/statics.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "engine/render/device.hpp"
#include "engine/world_source/png_io.hpp"
#include "engine/ui/canvas.hpp"
#include "engine/ui/font.hpp"
#include "engine/ui/ui.hpp"
#include "sim/act_info.hpp"
#include "sim/ai.hpp"
#include "sim/building.hpp"
#include "sim/collide.hpp"
#include "sim/craft_sim.hpp"
#include "sim/gear.hpp"
#include "sim/loc.hpp"
#include "sim/social_chain.hpp"
#include "sim/dialogue_say.hpp"
#include "sim/justice.hpp"
#include "sim/speech_feed.hpp"
#include "sim/tones.hpp"
#include "sim/worldgen.hpp"
#include "terrain_core/biomes.hpp"
#include "terrain_core/clearings.hpp"
#include "terrain_core/ground_height.hpp"
#include "terrain_core/hydrology.hpp"
#include "terrain_core/macro_map.hpp"
#include "terrain_core/params.hpp"
#include "terrain_core/relief.hpp"

namespace altera::game {
namespace {

namespace fs = std::filesystem;
using namespace altera::sim;
using Clock = std::chrono::steady_clock;
constexpr double kPi = 3.14159265358979;

struct BuildingSnap {
    std::string type;
    double x = 0, y = 0;
    EntityId family = kNone;
    float yaw = 0;          // AL-8: as the village plan faced it
    bool faced = false;
};

std::string clockText(Tick t) {
    const int m = int(t % kTicksPerDay) * 10;
    char b[16];
    std::snprintf(b, sizeof b, "%02d:%02d", m / 60, m % 60);
    return b;
}

// Ticks per real second at each speed: 1 is a game minute a real second, as a
// Sims-like life sim plays; 4 is "as fast as the machine goes".
constexpr double kSpeeds[] = {0.0, 0.1, 0.6, 3.0, 40.0};

// ---- meshes ---------------------------------------------------------------
struct Vertex {
    float p[3], n[3], shade;
};
struct Instance {
    float placeYaw[4], scale[4], colour[4], colour2[4];
};
static_assert(sizeof(Instance) == 64);

struct MeshBuilder {
    std::vector<Vertex> v;
    void tri(const float a[3], const float b[3], const float c[3], float shade) {
        const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float n[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
        const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (float& x : n) x /= (l > 0 ? l : 1);
        for (const float* q : {a, b, c}) v.push_back({{q[0], q[1], q[2]}, {n[0], n[1], n[2]}, shade});
    }
    void quad(const float a[3], const float b[3], const float c[3], const float d[3], float shade) {
        tri(a, b, c, shade);
        tri(a, c, d, shade);
    }
    // A prism of `sides` from radius r0 at z0 to r1 at z1, capped.
    void frustum(int sides, float r0, float z0, float r1, float z1, float shade, float sx = 1, float sy = 1) {
        for (int i = 0; i < sides; ++i) {
            const float a0 = float(2 * kPi * i / sides), a1 = float(2 * kPi * (i + 1) / sides);
            const float b0[3] = {r0 * std::cos(a0) * sx, r0 * std::sin(a0) * sy, z0};
            const float b1[3] = {r0 * std::cos(a1) * sx, r0 * std::sin(a1) * sy, z0};
            const float t0[3] = {r1 * std::cos(a0) * sx, r1 * std::sin(a0) * sy, z1};
            const float t1[3] = {r1 * std::cos(a1) * sx, r1 * std::sin(a1) * sy, z1};
            quad(b0, b1, t1, t0, shade);
            const float top[3] = {0, 0, z1}, bottom[3] = {0, 0, z0};
            tri(t0, t1, top, shade);
            tri(b1, b0, bottom, shade);
        }
    }
    void box(float x0, float y0, float z0, float x1, float y1, float z1, float shade) {
        const float p[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0},
                               {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
        quad(p[0], p[3], p[2], p[1], shade);   // bottom
        quad(p[4], p[5], p[6], p[7], shade);   // top
        quad(p[0], p[1], p[5], p[4], shade);
        quad(p[1], p[2], p[6], p[5], shade);
        quad(p[2], p[3], p[7], p[6], shade);
        quad(p[3], p[0], p[4], p[7], shade);
    }
};

// The things one works on (sim objects near the camera) are rough low-poly
// shapes too: a tree (trunk, two cones), deadwood (logs), a stony bank, a
// bush, a tuft (herbs, roots, mushrooms, reeds).
enum Mesh { kPerson, kBeast, kHouse, kBlock, kTree, kLog, kRock, kBush, kTuft, kMeshes };
struct MeshRange {
    std::uint32_t first = 0, count = 0;
};

// ---- the game ---------------------------------------------------------------
class SimGame final : public GameLayer, public HudHost {
public:
    SimGame(engine::Device& device, SDL_Window* window, const terrain_core::MacroMap& macro,
            const terrain_core::Params& params)
        : device_(device), window_(window), ground_(macro, params), worldSize_(params.worldSize), spacing_(params.spacing) {}

    ~SimGame() override {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        terrain_core::clearings().info[0] = 0;
        hud_.shutdown();
        ui_.shutdown();
        if (uiTransfer_) SDL_ReleaseGPUTransferBuffer(device_.handle(), uiTransfer_);
        if (instTransfer_) SDL_ReleaseGPUTransferBuffer(device_.handle(), instTransfer_);
        if (shotTransfer_) SDL_ReleaseGPUTransferBuffer(device_.handle(), shotTransfer_);
    }

    bool open(double centreX, double centreY, const GameOptions& o, std::string* why) {
        opts_ = o;
        relief_ = o.relief;
        try {
            content_ = std::make_unique<Content>(Content::load(o.root + "/content/ai"));
            loc_ = std::make_unique<Loc>(Loc::load(o.root + "/content/loc"));
            Json cfg = loadJsonFile(o.root + "/content/sim/test_world.json");
            if (!o.season.empty()) {
                cfg.erase("start_day");
                cfg["season"] = o.season;
            }
            if (o.biomes && o.biomes->size()) mapFromWorld(*o.biomes, cfg, centreX, centreY);
            settings_ = loadSettings();
            if (!settings_.lang.empty() && loc_->hasLanguage(settings_.lang)) opts_.lang = settings_.lang;
            control_ = settings_.control;
            leadMode_ = control_ == kFree ? kKeys : control_;
            follow_ = settings_.cameraFollow;
            wallMode_ = cutaway::Mode(settings_.wallMode);
            hud_.themeId = settings_.uiStyle;   // AL-62
            worldCfg_ = cfg;
            world_ = std::make_unique<World>(makeTestWorld(*content_, cfg, o.seed));
            installAi(*world_);
            // The world starts at midnight; the player arrives with the morning.
            world_->run(7 * kTicksPerDay / 24);
            for (const auto& [id, c] : world_->characters()) prevPos_[id] = c.pos;
        } catch (const std::exception& e) {
            if (why) *why = std::string("sim: ") + e.what();
            return false;
        }
        const MapGrid& m = world_->map();
        simSize_ = m.sizeM;
        // the act ring over beasts: every act whose target is a species
        for (const auto& [id, d] : content_->interactions)
            if (d.targetsCreature() && d.baseId.empty()) hud_.beastActs[d.target.substr(9)].push_back(id);
        originX_ = std::clamp(centreX - m.sizeM * 0.5, 0.0, std::max(0.0, worldSize_ - m.sizeM));
        originY_ = std::clamp(centreY - m.sizeM * 0.5, 0.0, std::max(0.0, worldSize_ - m.sizeM));
        refreshTexts();
        adoptWorld();
        if (!openGpu(why)) return false;
        setClearings();
        loadScript();
        // The game opens on the main menu (New game leads to the backstory
        // screen); tour scripts that speak of neither start in the world.
        std::string first = "menu";
        if (script_.is_array()) {
            first = "none";
            for (const auto& st : script_)
                if (st.contains("backstory") || st.contains("backstory_begin")) first = "backstory";
        }
        if (first == "backstory" && hud_.backstories.empty()) first = "none";
        hud_.panel = first;
        if (first == "menu") openMainMenu();
        if (settings_.fullscreen) SDL_SetWindowFullscreen(window_, true);
        thread_ = std::thread([this] { simLoop(); });
        // AL-69: this world is the one to open next time; and what the start was for
        if (!opts_.worldsDir.empty())
            terrain_core::writeWorldSpec(fs::path(opts_.worldsDir) / "current.json", opts_.world);
        if (opts_.begin == "new") menuQueue_.push_back({"new_here", Json::object()});
        else if (opts_.begin.rfind("load:", 0) == 0)
            menuQueue_.push_back({"load", {{"slot", std::atoi(opts_.begin.c_str() + 5)}}});
        return true;
    }

    // ---- the world the game shows: the first, a new one, one from a save ----
    // What the main thread keeps of the world's people and buildings, read
    // again from world_. The sim thread must not be running.
    void adoptWorld() {
        prevPos_.clear();
        speechFeed_.reset(); // AL-57: the new world's old talk stays unshown
        speechTail_.clear();
        jrTail_.clear();
        for (const auto& [id, c] : world_->characters()) prevPos_[id] = c.pos;
        buildings_.clear();
        roads_.clear();
        roadWidth_.clear();
        if (!world_->settlements().empty()) {
            const Settlement& s = world_->settlements().begin()->second;
            roads_ = s.roads;
            roadWidth_ = s.roadWidth;
            target_[0] = originX_ + s.center.x;
            target_[1] = originY_ + s.center.y;
            villageCentre_[0] = s.center.x;
            villageCentre_[1] = s.center.y;
        }
        for (const auto& [id, ob] : world_->objects()) {
            static const char* kShown[] = {"house", "storage", "hearth", "field", "garden", "shrine", "village_fire"};
            for (const char* t : kShown)
                if (ob.type == t) buildings_.push_back({ob.type, ob.pos.x, ob.pos.y, ob.ownerFamily, float(ob.yaw), ob.faced});
        }
        anims_.clear();
        beastAnims_.clear();
        avatar_ = Avatar{};
        hud_.resetWorld();
        selected_ = kNone;
        selectedWanted_ = kNone;
        wheelWanted_ = kNone;
        snapshotLocked();
        shown_ = snap_;
        clearedPieces_ = std::size_t(-1);   // the clearings follow the new buildings
        obstacleSig_ = std::size_t(-1);     // and so does what blocks a walker
        padSig_ = std::size_t(-1);          // and the ground under the buildings
        walkPath_.clear();
        idleS_ = 0;
        idleSent_ = false;
        for (const auto& a : shown_.actors)
            if (!a.animal && a.alive) {
                select(a.id);
                double wx, wy;
                actorWorld(a, 1.0, wx, wy);
                target_[0] = wx;
                target_[1] = wy;
                break;
            }
        freshTick_ = world_->now();
    }
    // Names the interface keeps (they follow the language).
    void refreshTexts() {
        hud_.buildCatalog.clear();
        for (const auto& d : content_->build.pieces) {
            std::string cost;
            for (const auto& [item, n] : d.cost) cost += (cost.empty() ? "" : ", ") + name("item", item) + " " + std::to_string(n);
            hud_.buildCatalog.push_back({d.id, text("hud.build.piece." + d.id), cost, d.category});
        }
        goalIds_.clear();
        goalNames_.clear();
        for (const auto& g : content_->goals) {
            goalIds_.push_back(g.id);
            goalNames_.push_back(name("goal", g.id));
        }
        backstoryCards();
    }
    void stopSim() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        stop_ = false;
    }
    void startSim() { thread_ = std::thread([this] { simLoop(); }); }
    // Untouched since it was made: New game needs no new world.
    bool worldFresh() const { return shown_.tick == freshTick_ && shown_.controlled == kNone; }

    // ---- the menus --------------------------------------------------------
    void menuAction(const std::string& action, const Json& args) override { menuQueue_.push_back({action, args}); }

    void openMainMenu() {
        hud_.panel = "menu";
        hud_.canLoad = latestSave() >= 0;
        hud_.canContinue = hud_.canLoad || !worldFresh();
        hud_.menuNote.clear();
        post({"hold"});
    }
    void openPause() {
        pauseHeld_ = !shown_.paused;
        post({"hold"});
        hud_.panel = "pause";
        hud_.canLoad = latestSave() >= 0;
        hud_.menuNote.clear();
    }
    void closePause() {
        hud_.panel = "none";
        if (pauseHeld_) post({"unhold"});
        pauseHeld_ = false;
    }
    void openSlots(bool save) {
        // AL-29: from the game (pause or any in-game panel) back goes to the game's pause
        if (hud_.panel == "menu") menuBack_ = "menu";
        else if (hud_.panel != "slots" && hud_.panel != "settings") menuBack_ = "pause";
        hud_.slotsSave = save;
        hud_.menuSlots.clear();
        for (const SaveMeta& m : listSaves()) {
            Hud::MenuSlot s;
            s.slot = m.slot;
            s.used = m.used;
            if (m.used) {
                s.title = m.person.empty() ? text("menu.slot.no_person") : m.person;
                if (!m.backstory.empty()) s.title += ", " + text("backstory." + m.backstory + ".name");
                const int hour = int((m.tick % kTicksPerDay) * 24 / kTicksPerDay);
                const int minute = int((m.tick % kTicksPerDay) * 24 * 60 / kTicksPerDay % 60);
                char hm[8];
                std::snprintf(hm, sizeof hm, "%02d:%02d", hour, minute);
                s.line = text("menu.slot.line", {{"day", m.day}, {"season", text("game.season." + m.season)},
                                                 {"time", std::string(hm)}, {"alive", m.alive}});
                if (m.slot == 0) s.line = text("menu.slot.auto") + " · " + s.line;
                const std::time_t t = std::time_t(m.savedAt);
                char when[32] = "";
                if (const std::tm* lt = std::localtime(&t)) std::strftime(when, sizeof when, "%d.%m.%Y  %H:%M", lt);
                s.when = when;
            }
            hud_.menuSlots.push_back(std::move(s));
        }
        hud_.menuNote.clear();
        hud_.panel = "slots";
    }
    void openSettings() {
        // AL-29: from the game (pause or any in-game panel) back goes to the game's pause
        if (hud_.panel == "menu") menuBack_ = "menu";
        else if (hud_.panel != "slots" && hud_.panel != "settings") menuBack_ = "pause";
        hud_.settingsLang = opts_.lang;
        hud_.settingsFullscreen = settings_.fullscreen;
        hud_.settingsAutosave = settings_.autosaveMin;
        hud_.settingsStyle = settings_.uiStyle;
        hud_.settingsScale = settings_.uiScale;
        hud_.menuNote.clear();
        hud_.panel = "settings";
    }
    void menuBack() {
        if (menuBack_ == "menu") openMainMenu();
        else {
            hud_.panel = "pause";
            hud_.canLoad = latestSave() >= 0;
        }
    }
    // Esc inside the menus: one step back.
    void menuEscape() {
        if (hud_.panel == "world") openMainMenu();
        else if (hud_.panel == "pause") closePause();
        else if (hud_.panel == "slots" || hud_.panel == "settings") menuBack();
    }

    // ---- AL-69: the world -------------------------------------------------
    // New game opens the world's making: size, seed, water, mountains,
    // climate, rivers, or the imported world of Altera. The same world again
    // starts at once; another one is made by the game started afresh on it
    // (the view's terrain, water and plants are built for one world).
    void openWorldPanel() {
        hud_.worldDraft = opts_.world;
        if (hud_.worldDraft.lore()) hud_.worldDraft = terrain_core::WorldSpec{};
        hud_.worldDraft.seed = std::uint32_t(SDL_GetTicksNS() % 900000u) + 1u;   // a new one each time
        hud_.worldSeedText = std::to_string(hud_.worldDraft.seed);
        hud_.menuNote.clear();
        menuBack_ = "menu";
        hud_.panel = "world";
    }
    void createWorld(const Json& a) {
        terrain_core::WorldSpec spec;
        std::string why;
        if (!terrain_core::parseWorldSpec(a.dump(), spec, &why)) {
            hud_.menuNote = text("menu.failed", {{"why", why}});
            return;
        }
        if (spec == opts_.world) {   // this very world: no restart
            newGame();
            return;
        }
        relaunch(spec, "new");
    }
    // The game again on `spec`'s world, then `begin` there (new | load:N).
    void relaunch(const terrain_core::WorldSpec& spec, const std::string& begin) {
        const fs::path next = fs::path(opts_.worldsDir.empty() ? "terrain_lab_out/worlds" : opts_.worldsDir) / "next.json";
        std::string why;
        if (!spec.lore() && !terrain_core::writeWorldSpec(next, spec, &why)) {
            hud_.menuNote = text("menu.failed", {{"why", why}});
            return;
        }
        int w = 1600, h = 900;
        SDL_GetWindowSize(window_, &w, &h);
        const std::string size = std::to_string(w) + "x" + std::to_string(h), file = next.string();
        std::vector<const char*> args{opts_.exe.c_str()};
        if (spec.lore()) args.push_back("--lore");
        else {
            args.push_back("--world-spec");
            args.push_back(file.c_str());
        }
        args.push_back("--begin");
        args.push_back(begin.c_str());
        args.push_back("--size");
        args.push_back(size.c_str());
        args.push_back(nullptr);
        SDL_Log("altera_game: restarting on world %s (%s)", spec.key().c_str(), begin.c_str());
        // a tour goes on in the new game with its next script, or none
        // (SDL's environment calls only: on Windows they and the C library's keep separate copies)
        if (SDL_getenv_unsafe("ALTERA_GAME_SCRIPT")) {
            const char* next = SDL_getenv_unsafe("ALTERA_GAME_SCRIPT_NEXT");
            const std::string nextScript = next ? next : "";
            if (nextScript.empty()) SDL_unsetenv_unsafe("ALTERA_GAME_SCRIPT");
            else SDL_setenv_unsafe("ALTERA_GAME_SCRIPT", nextScript.c_str(), 1);
            SDL_unsetenv_unsafe("ALTERA_GAME_SCRIPT_NEXT");
        }
        SDL_Process* child = opts_.exe.empty() ? nullptr : SDL_CreateProcess(args.data(), false);
        if (!child) {
            hud_.menuNote = text("menu.failed", {{"why", std::string(SDL_GetError())}});
            return;
        }
        SDL_DestroyProcess(child);   // forgets it; the new game runs on
        SDL_Event q{};
        q.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&q);
    }

    void newGame() {
        if (!worldFresh()) {
            stopSim();
            try {
                auto w = std::make_unique<World>(makeTestWorld(*content_, worldCfg_, opts_.seed));
                installAi(*w);
                w->run(7 * kTicksPerDay / 24);
                world_ = std::move(w);
            } catch (const std::exception& e) {
                SDL_Log("altera_game: new game: %s", e.what());
                hud_.menuNote = text("menu.failed", {{"why", std::string(e.what())}});
                startSim();
                return;
            }
            paused_ = true;
            adoptWorld();
            startSim();
        }
        hud_.menuNote.clear();
        if (hud_.backstories.empty()) {
            hud_.panel = "none";
            post({"unhold"});
        } else hud_.panel = "backstory";
    }

    bool saveSlot(int slot) {
        stopSim();
        Json save{{"format", "altera.save/1"},
                  {"game",
                   {{"origin_x", originX_}, {"origin_y", originY_}, {"target_x", target_[0]}, {"target_y", target_[1]},
                    {"zoom", zoom_}, {"yaw", orbitYaw_}, {"pitch", orbitPitch_}, {"speed", speed_},
                    {"seed", opts_.seed}, {"lang", opts_.lang},
                    {"world", Json::parse(terrain_core::worldSpecJson(opts_.world))}}},
                  {"world", world_->toJson()}};
        SaveMeta m;
        m.slot = slot;
        m.savedAt = std::int64_t(std::time(nullptr));
        m.tick = world_->now();
        m.day = world_->day() + 1;
        m.season = world_->season();
        for (const auto& [id, c] : world_->characters()) {
            if (c.body.alive) ++m.alive;
            if (c.controller.isPlayer()) {
                m.person = c.name;
                m.backstory = c.backstory;
            }
        }
        startSim();
        std::string why;
        if (!writeSave(slot, save, m, &why)) {
            SDL_Log("altera_game: save %d: %s", slot, why.c_str());
            hud_.menuNote = text("menu.failed", {{"why", why}});
            return false;
        }
        SDL_Log("altera_game: saved slot %d (%s)", slot, slotPath(slot).string().c_str());
        return true;
    }

    bool loadSlot(int slot) {
        Json save;
        try {
            save = readSave(slot);
        } catch (const std::exception& e) {
            SDL_Log("altera_game: load %d: %s", slot, e.what());
            hud_.menuNote = text("menu.failed", {{"why", std::string(e.what())}});
            return false;
        }
        {
            // AL-69: a save made on another world opens that world first
            // (saves from before worlds were chosen are of Altera's)
            terrain_core::WorldSpec saved;
            saved.kind = "lore";
            const Json g = save.value("game", Json::object());
            if (g.contains("world") && g["world"].is_object()) terrain_core::parseWorldSpec(g["world"].dump(), saved);
            if (!(saved == opts_.world)) {
                relaunch(saved, "load:" + std::to_string(slot));
                return true;
            }
        }
        stopSim();
        try {
            world_->loadJson(save.at("world"));
        } catch (const std::exception& e) {
            // a half-read world is no world: start a new one
            SDL_Log("altera_game: load %d: %s", slot, e.what());
            world_ = std::make_unique<World>(makeTestWorld(*content_, worldCfg_, opts_.seed));
            installAi(*world_);
            world_->run(7 * kTicksPerDay / 24);
            adoptWorld();
            startSim();
            hud_.menuNote = text("menu.failed", {{"why", std::string(e.what())}});
            return false;
        }
        const Json g = save.value("game", Json::object());
        originX_ = g.value("origin_x", originX_);
        originY_ = g.value("origin_y", originY_);
        adoptWorld();
        freshTick_ = Tick(-1);
        target_[0] = g.value("target_x", target_[0]);
        target_[1] = g.value("target_y", target_[1]);
        zoom_ = g.value("zoom", zoom_);
        orbitYaw_ = g.value("yaw", orbitYaw_);
        orbitPitch_ = g.value("pitch", orbitPitch_);
        speed_ = std::clamp(g.value("speed", speed_), 1, 4);
        paused_ = false;
        pauseHeld_ = false;
        startedAs_ = true;   // the player's person is selected once the snapshot shows them
        autosaveClock_ = 0;
        startSim();
        hud_.menuNote.clear();
        hud_.panel = "none";
        SDL_Log("altera_game: loaded slot %d", slot);
        return true;
    }

    void applySettings(const Json& a) {
        const std::string lang = a.value("lang", opts_.lang);
        if (lang != opts_.lang && loc_->hasLanguage(lang)) {
            stopSim();   // the sim thread reads the language for the snapshot's texts
            opts_.lang = lang;
            refreshTexts();
            startSim();
        }
        const bool fs = a.value("fullscreen", settings_.fullscreen);
        if (fs != settings_.fullscreen) SDL_SetWindowFullscreen(window_, fs);
        settings_.lang = opts_.lang;
        settings_.fullscreen = fs;
        settings_.autosaveMin = std::clamp(a.value("autosave", settings_.autosaveMin), 0, 120);
        settings_.uiScale = std::clamp(a.value("ui_scale", settings_.uiScale), 80, 130);
        const std::string style = a.value("style", settings_.uiStyle);
        if (style != settings_.uiStyle) {   // AL-62: another theme: the HUD and the faces again
            settings_.uiStyle = style;
            hud_.themeId = style;
            hud_.shutdown();
            std::string whyHud;
            if (!hud_.init(canvas_.renderer(), fs::path(opts_.root), &whyHud)) SDL_Log("altera_game: %s", whyHud.c_str());
            loadFonts();
            styleTheme();
        }
        storeSettings(settings_);
    }

    // The menu buttons of the last frame, and the autosave.
    void runMenus(double dt) {
        std::vector<std::pair<std::string, Json>> todo;
        todo.swap(menuQueue_);
        for (const auto& [act, args] : todo) {
            if (act == "continue") {
                // the game left for the main menu goes on; else the last save
                if (!worldFresh()) {
                    hud_.panel = "none";
                    post({"unhold"});
                } else if (const int n = latestSave(); n >= 0) loadSlot(n);
            } else if (act == "new") openWorldPanel();
            else if (act == "new_here") newGame();
            else if (act == "world_create") createWorld(args);
            else if (act == "open_load") openSlots(false);
            else if (act == "open_save") openSlots(true);
            else if (act == "settings") openSettings();
            else if (act == "resume") closePause();
            else if (act == "main_menu") {
                pauseHeld_ = false;
                openMainMenu();
            } else if (act == "quit") {
                SDL_Event q{};
                q.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&q);
            } else if (act == "load") loadSlot(args.value("slot", -1));
            else if (act == "save") {
                const int n = args.value("slot", 1);
                if (saveSlot(n)) {
                    menuBack();
                    hud_.menuNote = text("menu.saved", {{"n", n}});
                }
            } else if (act == "back") menuBack();
            else if (act == "apply") applySettings(args);
        }
        if (settings_.autosaveMin > 0 && hud_.panel == "none" && !shown_.paused && shown_.controlled != kNone) {
            autosaveClock_ += dt;
            if (autosaveClock_ >= settings_.autosaveMin * 60.0) {
                autosaveClock_ = 0;
                saveSlot(0);
            }
        }
    }

    // ---- the sim's map from the world ------------------------------------
    // The village goes to an open, dry, level place near where the world view
    // starts (a river valley or a plateau, a forest's edge, by water if there
    // is any), and the sim's grid of biomes and water is read from the world's
    // biome map round it, so the sim's forests, marshes and lakes are the ones
    // on screen.
    void mapFromWorld(const terrain_core::BiomeMap& bm, Json& cfg, double& centreX, double& centreY) {
        std::map<int, std::string> names;
        try {
            const Json t = loadJsonFile(opts_.root + "/content/config/terrain_core/biomes.json");
            const Json list = t.value("biomes", Json::array());
            for (const auto& b : list)
                if (b.is_object() && b.contains("id") && b.contains("name")) names[b["id"].get<int>()] = b["name"].get<std::string>();
        } catch (const std::exception& e) {
            SDL_Log("altera_game: biome names: %s", e.what());
        }
        const auto& ids = bm.ids();
        const double bs = double(bm.size());
        auto biomeName = [&](double x, double y) -> std::string {
            const int bx = std::clamp(int(x / worldSize_ * bs), 0, int(bs) - 1);
            const int by = std::clamp(int(y / worldSize_ * bs), 0, int(bs) - 1);
            const auto it = names.find(ids[std::size_t(by) * std::size_t(bs) + std::size_t(bx)]);
            return it == names.end() ? std::string() : it->second;
        };
        auto has = [](const std::string& n, std::initializer_list<const char*> keys) {
            for (const char* k : keys)
                if (n.find(k) != std::string::npos) return true;
            return false;
        };
        // Rivers of the world's graph wider than a brook, rasterised at 125 m:
        // they are water to the sim as the sea and lakes are (fish, drinking).
        const double rc = 125.0;
        const int rn = int(std::ceil(worldSize_ / rc));
        std::vector<std::uint8_t> river(std::size_t(rn) * std::size_t(rn), 0);
        if (opts_.rivers)
            for (const auto& seg : *opts_.rivers) {
                if (std::max(seg.width0, seg.width1) < 3.0f) continue;
                const double len = std::hypot(double(seg.x1 - seg.x0), double(seg.y1 - seg.y0));
                const int steps = std::max(1, int(len / 40.0));
                for (int k = 0; k <= steps; ++k) {
                    const double t = double(k) / steps;
                    const int cx = int((seg.x0 + (seg.x1 - seg.x0) * t) / rc), cy = int((seg.y0 + (seg.y1 - seg.y0) * t) / rc);
                    if (cx >= 0 && cy >= 0 && cx < rn && cy < rn) river[std::size_t(cy) * std::size_t(rn) + std::size_t(cx)] = 1;
                }
            }
        auto riverAt = [&](double x, double y) {
            const int cx = int(x / rc), cy = int(y / rc);
            return cx >= 0 && cy >= 0 && cx < rn && cy < rn && river[std::size_t(cy) * std::size_t(rn) + std::size_t(cx)];
        };
        auto wet = [&](const std::string& n, double h) { return h < 0.3 || n == "sea" || has(n, {"lake"}); };
        // the site
        double bestScore = -1e9, bx = centreX, by = centreY;
        for (double dy = -4000; dy <= 4000; dy += 125)
            for (double dx = -4000; dx <= 4000; dx += 125) {
                const double x = centreX + dx, y = centreY + dy;
                if (x < 2000 || y < 2000 || x > worldSize_ - 2000 || y > worldSize_ - 2000) continue;
                const double h = ground_.at(x, y, 2.0);
                const std::string n = biomeName(x, y);
                if (wet(n, h) || riverAt(x, y) || has(n, {"marsh", "mountain", "cliff", "shard", "scree", "volcan", "desert", "canyon_floor"})) continue;
                double slope = 0;
                for (int k = 0; k < 4; ++k) {
                    const double a = k * kPi * 0.5;
                    slope = std::max(slope, std::fabs(ground_.at(x + 30 * std::cos(a), y + 30 * std::sin(a), 2.0) - h) / 30.0);
                }
                if (slope > 0.12) continue;
                double open = has(n, {"river_valley"}) ? 1.0 : has(n, {"plateau"}) ? 0.8 : has(n, {"edge", "glade"}) ? 0.6
                            : has(n, {"steppe"}) ? 0.4 : has(n, {"forest"}) ? -0.2 : 0.2;
                // AL-7: water in sight of the houses (a lake, the sea, a river) counts most
                double water = 0;
                for (const double rr : {250.0, 450.0, 700.0}) {
                    for (int k = 0; k < 12 && water == 0; ++k) {
                        const double a = k * kPi / 6.0;
                        const double qx = x + rr * std::cos(a), qy = y + rr * std::sin(a);
                        if (wet(biomeName(qx, qy), ground_.at(qx, qy, 2.0)) || riverAt(qx, qy))
                            water = rr < 300 ? 1.6 : rr < 500 ? 1.1 : 0.5;
                    }
                    if (water > 0) break;
                }
                const double score = open + water - slope * 6.0 - std::hypot(dx, dy) / 5000.0;
                if (score > bestScore) bestScore = score, bx = x, by = y;
            }
        centreX = bx;
        centreY = by;
        // the grid, as the sim will lay it (worldgen: size_m, cell_m)
        const double size = std::clamp(cfg.value("size_m", 12000.0), 8000.0, 16000.0);
        const double cell = cfg.value("cell_m", 250.0);
        const int n = int(size / cell);
        const double ox = std::clamp(centreX - size * 0.5, 0.0, std::max(0.0, worldSize_ - size));
        const double oy = std::clamp(centreY - size * 0.5, 0.0, std::max(0.0, worldSize_ - size));
        Json mb = Json::array(), mw = Json::array();
        int wetCells = 0;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const double wx = ox + (x + 0.5) * cell, wy = oy + (y + 0.5) * cell;
                const std::string b = biomeName(wx, wy);
                const double h = ground_.at(wx, wy, 2.0);
                // a river anywhere in the sim's cell (sampled on the 125 m raster)
                bool riv = false;
                for (double sy = -0.375; sy <= 0.376 && !riv; sy += 0.25)
                    for (double sx = -0.375; sx <= 0.376 && !riv; sx += 0.25) riv = riverAt(wx + sx * cell, wy + sy * cell);
                const bool w = wet(b, h) || riv;
                mw.push_back(w ? 1 : 0);
                wetCells += w;
                mb.push_back(has(b, {"marsh"}) ? "marsh" : has(b, {"dark_forest"}) ? "forest" : "meadow");
            }
        cfg["map_biome"] = std::move(mb);
        cfg["map_water"] = std::move(mw);
        cfg["river"] = false;   // the water is the world's
        // AL-8: the ground round the village as the sim's planner reads it (sim/village_plan):
        // 1 km square at 5 m, heights in decimetres, water from the sea, lakes and the rivers' own widths.
        {
            const int sn = 201;
            const double sc = 5.0, sx0 = centreX - sc * (sn - 1) * 0.5, sy0 = centreY - sc * (sn - 1) * 0.5;
            Json hs = Json::array(), ws = Json::array();
            std::vector<std::uint8_t> wetCell(std::size_t(sn) * sn, 0);
            if (opts_.rivers)
                for (const auto& seg : *opts_.rivers) {
                    const double half = std::max(seg.width0, seg.width1) * 0.5 + 3.0;
                    const double vx = seg.x1 - seg.x0, vy = seg.y1 - seg.y0, l2 = vx * vx + vy * vy;
                    const int ax = int((std::min(seg.x0, seg.x1) - half - sx0) / sc) - 1,
                              bx = int((std::max(seg.x0, seg.x1) + half - sx0) / sc) + 1;
                    const int ay = int((std::min(seg.y0, seg.y1) - half - sy0) / sc) - 1,
                              by = int((std::max(seg.y0, seg.y1) + half - sy0) / sc) + 1;
                    for (int y = std::max(ay, 0); y <= std::min(by, sn - 1); ++y)
                        for (int x = std::max(ax, 0); x <= std::min(bx, sn - 1); ++x) {
                            const double px = sx0 + x * sc - seg.x0, py = sy0 + y * sc - seg.y0;
                            const double t = l2 > 0 ? std::clamp((px * vx + py * vy) / l2, 0.0, 1.0) : 0.0;
                            if (std::hypot(px - vx * t, py - vy * t) < half) wetCell[std::size_t(y) * sn + std::size_t(x)] = 1;
                        }
                }
            for (int y = 0; y < sn; ++y)
                for (int x = 0; x < sn; ++x) {
                    const double wx = sx0 + x * sc, wy = sy0 + y * sc;
                    const double h = ground_.at(wx, wy, 2.0);
                    hs.push_back(int(std::lround(h * 10.0)));
                    const bool w = wetCell[std::size_t(y) * sn + std::size_t(x)] || wet(biomeName(wx, wy), h);
                    ws.push_back(w ? 1 : 0);
                }
            cfg["village_site"] = Json{{"x0", sx0 - ox}, {"y0", sy0 - oy}, {"cell", sc}, {"n", sn},
                                       {"h_dm", std::move(hs)}, {"wet", std::move(ws)}};
            cfg["village_centre"] = Json::array({centreX - ox, centreY - oy});
        }
        SDL_Log("altera_game: village site %.0f, %.0f (%s), score %.2f, %d wet cells of %d", centreX, centreY,
                biomeName(centreX, centreY).c_str(), bestScore, wetCells, n * n);
    }

    // ---- input ---------------------------------------------------------
    bool event(const SDL_Event& e) override {
        switch (e.type) {
        case SDL_EVENT_QUIT: return false;
        case SDL_EVENT_TEXT_INPUT:   // AL-67: typing into a text field of the HUD
            if (ui_.typing()) input_.text += e.text.text;
            break;
        case SDL_EVENT_KEY_DOWN:
            if (ui_.typing()) {   // the keys edit the field, they are not commands
                if (e.key.key == SDLK_BACKSPACE) input_.backspace = true;
                if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) input_.enter = true;
                if (e.key.key == SDLK_ESCAPE) input_.escape = true;
                if (e.key.key == SDLK_TAB) input_.tab = true;
                break;
            }
            // AL-29: F11 / Alt+Enter toggle the full screen anywhere
            if (!e.key.repeat && (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)))) {
                settings_.fullscreen = !settings_.fullscreen;
                if (window_) SDL_SetWindowFullscreen(window_, settings_.fullscreen);
                hud_.settingsFullscreen = settings_.fullscreen;
                break;
            }
            if (hud_.inMenu()) {   // the menus: only Esc, one step back
                if (e.key.key == SDLK_ESCAPE && !e.key.repeat) menuEscape();
                break;
            }
            if (hud_.panel == "backstory") {   // the first screen: only its own keys
                if (e.key.key == SDLK_ESCAPE && !e.key.repeat) {
                    openMainMenu();
                    break;
                }
                if (e.key.key == SDLK_UP || e.key.key == SDLK_LEFT) --hud_.backstoryPick;
                if (e.key.key == SDLK_DOWN || e.key.key == SDLK_RIGHT) ++hud_.backstoryPick;
                if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) hud_.backstoryBegin = true;
                break;
            }
            if (e.key.key == SDLK_ESCAPE) {
                if (hud_.panel != "none") {
                    hud_.panel = "none";
                    break;
                }
                if (!e.key.repeat) openPause();
                break;
            }
            if (e.key.repeat) break;
            noteInput();
            if (e.key.key == SDLK_SPACE) post({"pause"});
            if (e.key.key >= SDLK_1 && e.key.key <= SDLK_4 && !speedLocked()) post({"speed", int(e.key.key - SDLK_1) + 1});
            if (e.key.key == SDLK_TAB) cameraFollow(!follow_);
            if (e.key.key == SDLK_V) controlMode((control_ + 1) % 3);
            if (e.key.key == SDLK_H) hud_.panel = hud_.panel == "annals" ? "none" : "annals";
            if (e.key.key == SDLK_C) hud_.panel = hud_.panel == "sheet" ? "none" : "sheet";
            if (e.key.key == SDLK_K) hud_.panel = hud_.panel == "kin" ? "none" : "kin";
            if (e.key.key == SDLK_T) hud_.showThoughts = !hud_.showThoughts;
            if (e.key.key == SDLK_B) hud_.panel = hud_.panel == "build" ? "none" : "build";
            if (e.key.key == SDLK_O) hud_.panel = hud_.panel == "colony" ? "none" : "colony";
            if (e.key.key == SDLK_J) hud_.panel = hud_.panel == "workshop" ? "none" : "workshop";
            if (e.key.key == SDLK_I) hud_.panel = hud_.panel == "inventory" ? "none" : "inventory";
            if (e.key.key == SDLK_R && hud_.panel == "inventory") hud_.turnDrag();
            if (hud_.panel == "build") {
                if (e.key.key == SDLK_R) buildRot_ = (buildRot_ + 1) % 4;
                if (e.key.key == SDLK_PAGEUP) ++buildBump_;
                if (e.key.key == SDLK_PAGEDOWN) buildBump_ = std::max(0, buildBump_ - 1);
                if (e.key.key == SDLK_X || e.key.key == SDLK_DELETE) removeWanted_ = true;
            }
            if (e.key.key == SDLK_F) cameraFollow(true);
            // AL-65: Y / N answer an NPC's ask, X lets the followers go (outside the build mode)
            if (!ui_.typing() && hud_.panel == "none") {
                if (e.key.key == SDLK_Y) hud_.socialKey = 1;
                if (e.key.key == SDLK_N) hud_.socialKey = -1;
                if (e.key.key == SDLK_X) hud_.socialKey = 2;
            }
            if (e.key.key == SDLK_HOME) wallMode(int(cutaway::nextMode(wallMode_)));
            if (e.key.key == SDLK_F9) {
                char n[64];
                std::snprintf(n, sizeof n, "game_%03d.png", ++shotCounter_);
                shotWanted_ = (fs::path(opts_.root) / "terrain_lab_out/shots_for_user" / n).string();
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            input_.mouseX = e.motion.x;
            input_.mouseY = e.motion.y;
            if (rmb_) {
                orbitYaw_ -= e.motion.xrel * 0.005;
                orbitPitch_ = std::clamp(orbitPitch_ + e.motion.yrel * 0.004, 0.08, 1.45);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            noteInput();
            if (e.button.button == SDL_BUTTON_LEFT) {
                input_.down = input_.pressed = true;
                input_.doubleClick = e.button.clicks >= 2;
                if (!overUi_) pickAt_ = {e.button.x, e.button.y, true};
            }
            if (e.button.button == SDL_BUTTON_RIGHT) {
                input_.rightPressed = input_.rightDown = true;
                if (!overUi_) rmb_ = true;
                rmbAt_[0] = e.button.x;
                rmbAt_[1] = e.button.y;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button == SDL_BUTTON_LEFT) {
                input_.down = false;
                input_.released = true;
            }
            if (e.button.button == SDL_BUTTON_RIGHT) {
                input_.rightDown = false;
                // A right click (not a drag of the camera) on a person: the wheel.
                if (rmb_ && std::hypot(e.button.x - rmbAt_[0], e.button.y - rmbAt_[1]) < 5) wheelAt_ = {e.button.x, e.button.y, true};
                rmb_ = false;
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            input_.wheel += e.wheel.y;
            if (!overUi_) zoom_ = std::clamp(zoom_ * std::pow(0.85, e.wheel.y), 4.0, 6000.0);
            break;
        default: break;
        }
        return true;
    }

    // ---- camera and clock ---------------------------------------------------
    void update(double dt, CameraRig& cam, double& timeOfDay) override {
        finishShot();
        runScript(dt);
        {
            std::lock_guard<std::mutex> lock(snapMutex_);
            shown_ = snap_;
        }
        runMenus(dt);
        if (hud_.panel == "menu") orbitYaw_ += 0.05 * dt;   // the village turns slowly behind the main menu
        const double alpha = tickAlpha();
        // The hour of the day from the sim's clock, moving smoothly between ticks.
        const double ticks = double(shown_.tick % kTicksPerDay) + alpha;
        timeOfDay = ticks / double(kTicksPerDay) * 24.0;

        if (startedAs_ && shown_.controlled != kNone) {
            startedAs_ = false;
            select(shown_.controlled);
        }
        // a person newly in the player's hands puts on what they carry to wear
        if (shown_.controlled != kNone && shown_.controlled != woreAllFor_) {
            woreAllFor_ = shown_.controlled;
            post({"cmd", 0, "wear_all", {{"id", shown_.controlled}}});
        }
        if (wheelAt_.pending) {
            wheelAt_.pending = false;
            const EntityId hit = pick(wheelAt_.x, wheelAt_.y);
            if (hit != kNone && hit != shown_.controlled) openActions(hit, wheelAt_.x, wheelAt_.y);
        }
        if (pickAt_.pending) {
            pickAt_.pending = false;
            if (hud_.panel == "build") dragStart_ = true;   // AL-9: a press starts a drag, the release lays it
            else {
                // A click picks a target and opens what can be done to it; it
                // never moves the control (that is the Dynasty panel's "Lead").
                // Without a person of one's own (before the backstory) it only
                // shows whom it hit, as before.
                const EntityId hit = pick(pickAt_.x, pickAt_.y);
                if (shown_.controlled == kNone) {
                    if (hit != kNone) select(hit);
                } else if (hit == kNone) {
                    hud_.aim = kNone;
                    // a thing to work on: its act ring; else, in the mouse and
                    // free modes, a click on the ground walks the person there
                    const EntityId thing = pickThing(pickAt_.x, pickAt_.y);
                    double gx, gy;
                    if (thing != kNone) hud_.openThing(thing, pickAt_.x, pickAt_.y);
                    else if (control_ != kKeys && groundUnder(pickAt_.x, pickAt_.y, gx, gy)) walkTo(gx, gy);
                } else if (hit == shown_.controlled) {
                    hud_.aim = kNone;
                    select(hit);
                } else {
                    openActions(hit, pickAt_.x, pickAt_.y);
                }
            }
        }
        if (hud_.panel == "build") updateBuild();
        else hasPreview_ = false;
        if (shown_.pieces.size() != clearedPieces_) {
            clearedPieces_ = shown_.pieces.size();
            setClearings();
        }
        layPads();
        rebuildObstacles();
        static const bool kNoKeys[SDL_SCANCODE_COUNT] = {};
        const bool* keys = hud_.inMenu() ? kNoKeys : SDL_GetKeyboardState(nullptr);
        holdSpeed();
        const bool walking = driveAvatar(dt, keys, alpha);
        updateIdle(dt, walking);
        pendingActStep();
        const ActorSnap* sel = actor(selected_);
        // The keys walk the person only in the keys mode (and only with a
        // person of one's own); otherwise they move the camera, which lets go
        // of the person it rode on.
        const bool keysWalk = control_ == kKeys && avatarCanWalk() && hud_.panel == "none";
        if (follow_ && walking && (selected_ == avatar_.id || !sel)) {
            // the camera stays on the walker, without the follow's lag
            target_[0] = avatar_.x;
            target_[1] = avatar_.y;
        } else if (follow_ && sel) {
            double wx, wy;
            actorWorld(*sel, alpha, wx, wy);
            target_[0] += (wx - target_[0]) * std::min(1.0, dt * 6.0);
            target_[1] += (wy - target_[1]) * std::min(1.0, dt * 6.0);
        }
        if (!keysWalk) {
            const double v = zoom_ * dt * (keys[SDL_SCANCODE_LSHIFT] ? 3.0 : 1.0);
            const double c = std::cos(orbitYaw_), s = std::sin(orbitYaw_);
            double mx = 0, my = 0;
            if (keys[SDL_SCANCODE_W]) mx += c * v, my += s * v;
            if (keys[SDL_SCANCODE_S]) mx -= c * v, my -= s * v;
            if (keys[SDL_SCANCODE_D]) mx += s * v, my -= c * v;
            if (keys[SDL_SCANCODE_A]) mx -= s * v, my += c * v;
            if (mx != 0 || my != 0) {
                target_[0] += mx;
                target_[1] += my;
                if (follow_) cameraFollow(false);
            }
        }
        syncControl();
        hud_.controlMode = control_;
        hud_.cameraFollow = follow_;
        hud_.wallMode = int(wallMode_);
        {
            const ActorSnap* c = actor(leadCandidate());
            hud_.leadName = c ? c->name : "";
        }
        hud_.speedLocked = speedLocked();
        hud_.hasPerson = shown_.controlled != kNone;
        viewX_ = target_[0] - originX_;
        viewY_ = target_[1] - originY_;
        actsWanted_ = hud_.actTarget();
        // the hint: what the mouse is over in the world
        hud_.hoverActor = hud_.hoverThing = kNone;
        if (hud_.panel == "none" && !overUi_ && !rmb_) {
            hud_.hoverActor = pick(input_.mouseX, input_.mouseY);
            if (hud_.hoverActor == kNone) hud_.hoverThing = pickThing(input_.mouseX, input_.mouseY);
        }
        hud_.walkMark = !walkPath_.empty();
        if (hud_.walkMark) {
            hud_.walkMarkX = walkPath_.back().first - originX_;
            hud_.walkMarkY = walkPath_.back().second - originY_;
        }
        if (keys[SDL_SCANCODE_Q]) orbitYaw_ += 1.5 * dt;
        // in the workshop E is "make" (held), not the camera
        hud_.keyE = keys[SDL_SCANCODE_E] && hud_.panel == "workshop";
        if (keys[SDL_SCANCODE_E] && hud_.panel != "workshop") orbitYaw_ -= 1.5 * dt;
        const double tz = groundAt(target_[0], target_[1]) + 1.2;
        cam.yaw = orbitYaw_;
        cam.pitch = -orbitPitch_;
        const double f[3] = {std::cos(cam.pitch) * std::cos(cam.yaw), std::cos(cam.pitch) * std::sin(cam.yaw),
                             std::sin(cam.pitch)};
        cam.eye[0] = target_[0] - f[0] * zoom_;
        cam.eye[1] = target_[1] - f[1] * zoom_;
        cam.eye[2] = std::max(tz - f[2] * zoom_, groundAt(cam.eye[0], cam.eye[1]) + 1.5);
    }

    // ---- direct control ------------------------------------------------------
    // WASD walk the player's person on this side at a walker's pace (Shift:
    // running), relative to the camera; the sim hears where they are ten times
    // a second (`steer`), which also drops what they were doing. Out of the
    // world map's water and the sim's map nobody walks. True while walking
    // (keys held), the camera then rides on the walker.
    bool driveAvatar(double dt, const bool* keys, double alpha) {
        const ActorSnap* me = actor(shown_.controlled);
        const double nowS = std::chrono::duration<double>(Clock::now() - started_).count();
        if (!me || !me->alive || me->down || me->captive || me->fighting) {
            avatar_.driven = false;
            avatar_.id = me ? me->id : kNone;
            avatar_.speed = 0;
            return false;
        }
        if (avatar_.id != me->id) {
            avatar_ = Avatar{};
            avatar_.id = me->id;
        }
        if (shown_.paused) {   // the clock stands: so does the person (a click's way waits)
            avatar_.speed = 0;
            if (scriptWalk_[2] > 0) scriptWalk_[2] -= dt;
            return false;
        }
        double fwd = 0, side = 0;
        if (control_ == kKeys && hud_.panel == "none" && !script_.is_array()) {
            if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) fwd += 1;
            if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]) fwd -= 1;
            if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) side += 1;
            if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) side -= 1;
        }
        if (scriptWalk_[2] > 0) {   // tour scripts: walk {fwd, side} for a while
            fwd = scriptWalk_[0];
            side = scriptWalk_[1];
            scriptWalk_[2] -= dt;
        }
        double dx = 0, dy = 0;
        const bool keyed = fwd != 0 || side != 0;
        if (keyed) {
            walkPath_.clear();   // the keys take over from a click's way
            pendingAct_ = false;
            const double c = std::cos(orbitYaw_), s = std::sin(orbitYaw_);
            dx = c * fwd + s * side;
            dy = s * fwd - c * side;
        } else if (!walkPath_.empty()) {
            // a click's way: on to the next corner of it
            if (!avatar_.driven) actorWorld(*me, alpha, avatar_.x, avatar_.y);
            while (!walkPath_.empty() &&
                   std::hypot(walkPath_.front().first - avatar_.x, walkPath_.front().second - avatar_.y) < 0.25)
                walkPath_.erase(walkPath_.begin());
            if (!walkPath_.empty()) {
                dx = walkPath_.front().first - avatar_.x;
                dy = walkPath_.front().second - avatar_.y;
            }
        }
        const bool input = dx != 0 || dy != 0;
        if (input) {
            if (!avatar_.driven) {
                actorWorld(*me, alpha, avatar_.x, avatar_.y);
                obstacles_.pushOut(avatar_.x, avatar_.y, kWalkerRadius);   // the sim may have them indoors
                avatar_.driven = true;
            }
            if (selected_ != me->id) select(me->id);
            const double l = std::hypot(dx, dy);
            dx /= l;
            dy /= l;
            const bool run = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT] || scriptRun_ ||
                             (!keyed && walkRun_);
            // a heavy load slows the walker; over the most one can carry, no running
            double slow = 1.0;
            const GearSnap& gs = shown_.detail.gear;
            if (gs.on && shown_.detail.id == me->id && gs.kg > gs.comfortKg)
                slow = gs.kg > gs.maxKg ? 0.55 : 1.0 - 0.25 * (gs.kg - gs.comfortKg) / std::max(1.0, gs.maxKg - gs.comfortKg);
            const bool canRun = !(gs.on && shown_.detail.id == me->id && gs.kg > gs.maxKg);
            avatarRuns_ = run && canRun;
            // the clock's speed hurries the walker too (1x is a real walk)
            static constexpr double kWalkScale[] = {0.0, 1.0, 1.6, 2.6, 4.0};
            const double v = (avatarRuns_ ? 5.2 : 1.9) * slow * kWalkScale[std::clamp(shown_.speed, 1, 4)];
            double step = v * dt;
            if (!keyed && !walkPath_.empty())   // do not overshoot the corner
                step = std::min(step, std::hypot(walkPath_.front().first - avatar_.x, walkPath_.front().second - avatar_.y));
            // Houses, walls, posts, steep and wet ground and the sim map's edge
            // block the walker; along them it slides.
            const double ox = avatar_.x, oy = avatar_.y;
            obstacles_.slide(avatar_.x, avatar_.y, dx * step, dy * step, kWalkerRadius);
            bumpActors(me->id, ox, oy);
            const double went = std::hypot(avatar_.x - ox, avatar_.y - oy);
            avatar_.speed = dt > 0 ? std::min(v, went / dt) : 0.0;
            if (went > 1e-4) avatar_.yaw = std::atan2(avatar_.y - oy, avatar_.x - ox);
            else avatar_.yaw = std::atan2(dy, dx);
            if (!keyed) {
                // a click's way that has stuck (something new in the way): give it up
                if (went < step * 0.2) walkStuck_ += dt;
                else walkStuck_ = 0;
                if (walkStuck_ > 0.8) {
                    walkPath_.clear();
                    walkStuck_ = 0;
                }
            }
            avatar_.lastInput = nowS;
        } else {
            avatar_.speed = 0;
        }
        if (!avatar_.driven) return false;
        // tell the sim, at most ten times a second and once more when the keys let go
        const bool moved = std::hypot(avatar_.x - avatar_.sentX, avatar_.y - avatar_.sentY) > 0.02;
        if (moved && (nowS - avatar_.lastSent > 0.1 || !input)) {
            const double z = groundAt(avatar_.x, avatar_.y);
            post({"steer", 0, "", {{"id", me->id}, {"x", avatar_.x - originX_}, {"y", avatar_.y - originY_},
                                   {"run", avatarRuns_}, {"climb", std::max(0.0, z - sentZ_)}}});
            sentZ_ = z;
            avatar_.sentX = avatar_.x;
            avatar_.sentY = avatar_.y;
            avatar_.lastSent = nowS;
        }
        // Let go: back to the snapshots once the sim stands them where we left
        // them (or, after a while, wherever the sim has them).
        if (!input && nowS - avatar_.lastInput > 0.3) {
            const double sx = originX_ + me->x, sy = originY_ + me->y;
            if (std::hypot(sx - avatar_.x, sy - avatar_.y) < 0.05 || nowS - avatar_.lastInput > 2.0) avatar_.driven = false;
        }
        return input;
    }

    // People and beasts are in the way too: the walker is pushed off them
    // (never into a house: then it stays where it was).
    void bumpActors(EntityId self, double ox, double oy) {
        const double alpha = tickAlpha();
        for (const auto& a : shown_.actors) {
            if (a.id == self || !a.alive || a.species == "fish_shoal") continue;
            double ax, ay;
            actorWorld(a, alpha, ax, ay);
            const double r = kWalkerRadius + (a.animal ? (a.species == "hens" || a.species == "hare" ? 0.15 : 0.45) : kWalkerRadius);
            const double dx = avatar_.x - ax, dy = avatar_.y - ay, d = std::hypot(dx, dy);
            if (d >= r) continue;
            if (d < 1e-4) {
                avatar_.x = ox, avatar_.y = oy;
                continue;
            }
            const double nx = ax + dx / d * r, ny = ay + dy / d * r;
            if (!obstacles_.blocked(nx, ny, kWalkerRadius)) avatar_.x = nx, avatar_.y = ny;
            else avatar_.x = ox, avatar_.y = oy;
        }
    }

    // ---- control modes ---------------------------------------------------------
    // keys: WASD walk the person (the clock held at 1, the walk is real time);
    // mouse: a click on the ground walks them there round what is in the way,
    // WASD move the camera; free (Sims-like): the same clicks, the wheel's acts
    // queue up instead of replacing what goes on, and left alone for
    // idleAutonomyS seconds the person goes back to their own life. The camera
    // riding on the selected person is apart from all three (Tab, the bar).
    bool avatarCanWalk() const {
        const ActorSnap* me = actor(shown_.controlled);
        return me && me->alive && !me->down && !me->captive && !me->fighting;
    }
    // (tour scripts keep their own clock)
    // (the clock is never held now: pause and speed act on the walker as on everyone)
    bool speedLocked() const { return false; }
    void holdSpeed() {
        if (!speedLocked() || shown_.speed == 1) return;
        const double nowS = std::chrono::duration<double>(Clock::now() - started_).count();
        if (nowS - speedPostedS_ < 0.5) return;
        speedPostedS_ = nowS;
        post({"speed", 1});
    }
    void noteInput() {
        idleS_ = 0;
        idleSent_ = false;
    }
    void updateIdle(double dt, bool walking) {
        if (control_ != kMouse || shown_.controlled == kNone || hud_.inMenu() || shown_.paused) return;
        if (walking) {
            idleS_ = 0;
            return;
        }
        idleS_ += dt;
        if (!idleSent_ && idleS_ >= double(settings_.idleAutonomyS)) {
            idleSent_ = true;
            post({"cmd", 0, "set_free_will", {{"id", shown_.controlled}, {"on", true}, {"now", true}}});
        }
    }
    // free: nobody is led (the person goes back to their own life, the camera
    // lets go); keys / mouse with nobody led take the selected person (or the
    // one led last) under control.
    void controlMode(int mode) override {
        mode = std::clamp(mode, 0, 2);
        noteInput();
        if (mode == kFree) {
            if (shown_.controlled != kNone) {
                lastLed_ = shown_.controlled;
                post({"cmd", 0, "release_control", Json::object()});
            }
            walkPath_.clear();
            avatar_.driven = false;
            if (follow_) cameraFollow(false);
        } else {
            leadMode_ = mode;
            if (shown_.controlled == kNone && !worldFresh()) {
                const EntityId who = leadCandidate();
                if (who != kNone) post({"cmd", 0, "take_control", {{"id", who}}});
            }
        }
        if (mode == control_) return;
        control_ = mode;
        if (control_ == kKeys) walkPath_.clear();
        settings_.control = control_;
        storeSettings(settings_);
    }
    // whom keys / mouse take under control: the selected person, else the one led last
    EntityId leadCandidate() const {
        for (EntityId id : {selected_, lastLed_}) {
            const ActorSnap* a = actor(id);
            if (a && !a->animal && a->alive) return id;
        }
        return kNone;
    }
    // The control bar's state follows the sim: nobody led is the free mode,
    // someone taken (the Dynasty panel's Lead, the backstory) the mode led with last.
    void syncControl() {
        if (shown_.controlled != prevControlled_) {
            if (shown_.controlled == kNone && prevControlled_ != kNone) {
                lastLed_ = prevControlled_;
                if (control_ != kFree) control_ = kFree;
            } else if (shown_.controlled != kNone && control_ == kFree) {
                control_ = leadMode_;
            }
            prevControlled_ = shown_.controlled;
        }
    }
    void cameraFollow(bool on) override {
        follow_ = on;
        if (on && selected_ == kNone && shown_.controlled != kNone) select(shown_.controlled);
        if (settings_.cameraFollow != on) {
            settings_.cameraFollow = on;
            storeSettings(settings_);
        }
    }
    void wallMode(int mode) override {
        wallMode_ = cutaway::Mode(std::clamp(mode, 0, 2));
        if (settings_.wallMode != int(wallMode_)) {
            settings_.wallMode = int(wallMode_);
            storeSettings(settings_);
        }
    }
    // A click on the ground (mouse and free modes): the way there round the
    // houses, walked by the avatar as the keys would; a double click runs.
    // Where a target stands (world metres): a thing or a person / beast.
    bool targetWorld(EntityId id, double& x, double& y) const {
        for (const auto& t : shown_.things)
            if (t.id == id) {
                x = originX_ + t.x;
                y = originY_ + t.y;
                return true;
            }
        if (const ActorSnap* a = actor(id)) {
            actorWorld(*a, tickAlpha(), x, y);
            return true;
        }
        return false;
    }
    // A way to stand next to the target (not into it); false when near enough or no way.
    bool approach(EntityId target) {
        const ActorSnap* me = actor(shown_.controlled);
        if (!me || !avatarCanWalk() || shown_.paused) return false;
        double tx, ty, sx, sy;
        if (!targetWorld(target, tx, ty)) return false;
        if (avatar_.driven && avatar_.id == me->id) sx = avatar_.x, sy = avatar_.y;
        else actorWorld(*me, tickAlpha(), sx, sy);
        const double d = std::hypot(tx - sx, ty - sy);
        if (d < 2.5 || d > 300.0) return false;
        // the free spot nearest the target, from the person's side
        double gx = tx + (sx - tx) / d * 1.3, gy = ty + (sy - ty) / d * 1.3;
        obstacles_.pushOut(gx, gy, kWalkerRadius);
        obstacles_.pushOut(sx, sy, kWalkerRadius);
        std::vector<collide::Point> way;
        if (!obstacles_.path(sx, sy, gx, gy, kWalkerRadius, way) || way.empty()) return false;
        walkPath_ = std::move(way);
        walkRun_ = false;
        walkStuck_ = 0;
        return true;
    }
    // The act waiting for the walk to end: sent on arrival, dropped when the keys take over.
    void pendingActStep() {
        if (!pendingAct_) return;
        if (walkPath_.empty() && !shown_.paused) {
            pendingAct_ = false;
            sendCommand(pendingType_, pendingArgs_);
        }
    }
    void walkTo(double gx, double gy) {
        const ActorSnap* me = actor(shown_.controlled);
        if (!me || !avatarCanWalk()) return;
        double sx, sy;
        if (avatar_.driven && avatar_.id == me->id) sx = avatar_.x, sy = avatar_.y;
        else actorWorld(*me, tickAlpha(), sx, sy);
        obstacles_.pushOut(sx, sy, kWalkerRadius);
        // AL-29: too far or no way there - said, not silently ignored
        if (std::hypot(gx - sx, gy - sy) > 600.0) {
            result_ = "rejected:walk_too_far";
            return;
        }
        std::vector<collide::Point> way;
        if (!obstacles_.path(sx, sy, gx, gy, kWalkerRadius, way)) {
            result_ = "rejected:no_path";
            return;
        }
        walkPath_ = std::move(way);
        walkRun_ = input_.doubleClick;
        walkStuck_ = 0;
        noteInput();
    }
    // The ground under a screen point (world metres): the eye's ray marched to the terrain.
    bool groundUnder(float mx, float my, double& gx, double& gy) const {
        double inv[16];
        if (!invert4(vp_, inv)) return false;
        const double nx = double(mx) / double(viewW_) * 2.0 - 1.0, ny = 1.0 - double(my) / double(viewH_) * 2.0;
        double p[4];
        for (int r = 0; r < 4; ++r) p[r] = inv[r * 4 + 0] * nx + inv[r * 4 + 1] * ny + inv[r * 4 + 2] * 0.5 + inv[r * 4 + 3];
        if (std::fabs(p[3]) < 1e-12) return false;
        double d[3] = {p[0] / p[3], p[1] / p[3], p[2] / p[3]};
        const double l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l < 1e-9) return false;
        for (double& v : d) v /= l;
        // the eye looks along its yaw and pitch: a ray behind it is turned round
        const double f[3] = {std::cos(orbitPitch_) * std::cos(orbitYaw_), std::cos(orbitPitch_) * std::sin(orbitYaw_),
                             -std::sin(orbitPitch_)};
        if (d[0] * f[0] + d[1] * f[1] + d[2] * f[2] < 0)
            for (double& v : d) v = -v;
        auto above = [&](double t) {
            const double x = eye_[0] + d[0] * t, y = eye_[1] + d[1] * t;
            return eye_[2] + d[2] * t > groundAt(x, y);
        };
        double t0 = 0, t = 0.5;
        while (t < 3000.0 && above(t)) {
            t0 = t;
            t += std::max(0.25, t * 0.02);
        }
        if (t >= 3000.0) return false;
        for (int k = 0; k < 10; ++k) {
            const double m = (t0 + t) * 0.5;
            if (above(m)) t0 = m;
            else t = m;
        }
        gx = eye_[0] + d[0] * t;
        gy = eye_[1] + d[1] * t;
        return true;
    }
    static bool invert4(const float* m, double* out) {
        double a[16];
        for (int k = 0; k < 16; ++k) a[k] = m[k];
        double inv[16];
        inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
        inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
        inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
        inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
        inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
        inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
        inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
        inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
        inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
        inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
        inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
        inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
        inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
        inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
        inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
        inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
        const double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
        if (std::fabs(det) < 1e-30) return false;
        for (int k = 0; k < 16; ++k) out[k] = inv[k] / det;
        return true;
    }

    // What blocks a walker, built again when the village changes: the
    // settlement's buildings as they are drawn (turned boxes), the built
    // ground-floor walls and posts of the households' own building, the fires;
    // the ground: dry, not steeper than ~42 degrees, inside the sim's map.
    // Low growth and ground cover one walks through.
    static bool walkThrough(const std::string& ty) {
        static const std::set<std::string> k = {"berry_bush", "root_patch", "mushroom_glade", "reed_bed", "nettle_patch",
                                                "herb_patch", "wild_greens_patch", "wild_grass", "hay_meadow", "game_trail",
                                                "fish_pool", "corpse", "hazel_thicket", "campfire"};
        return k.count(ty) > 0;
    }
    void rebuildObstacles() {
        std::size_t sig = buildings_.size() * 7919 + shown_.pieces.size() * 131 + (village_.loaded() ? 1 : 0);
        for (const auto& [id, p] : shown_.pieces) sig += p.built ? std::size_t(id) * 2654435761u : 0;
        for (const auto& t : shown_.things) sig += std::size_t(t.id) * 40503u + 17;
        if (sig == obstacleSig_) return;
        obstacleSig_ = sig;
        obstacles_.clear();
        // trunks, piles of stone and logs (the bushes and patches one walks through)
        for (const auto& t : shown_.things) {
            const double wx = originX_ + t.x, wy = originY_ + t.y;
            const std::uint64_t h = std::uint64_t(t.id) * 0x9E3779B97F4A7C15ull;
            if (t.type == "tree" || t.type == "bee_tree") obstacles_.add(collide::Circle{wx, wy, 0.28});
            else if (t.type == "oak_grove") obstacles_.add(collide::Circle{wx, wy, 0.4});
            else if (t.type == "fruit_tree") obstacles_.add(collide::Circle{wx, wy, 0.2});
            else if (t.type == "riverbank") obstacles_.add(collide::Circle{wx, wy, 0.9});
            else if (t.type == "deadwood")
                obstacles_.add(collide::Box{wx, wy, 1.15, 0.3, double((h >> 8) % 628) / 100.0});
            else if (const int pp = propPiece(t.type); pp >= 0 && !walkThrough(t.type)) {
                // a stand-in model (AL-11): its footprint as drawn (yaw and size as buildInstances has them)
                float hx = 0, hy = 0;
                props_.halfSize(pp, hx, hy);
                const double var = 0.85 + 0.3 * double((h >> 20) % 100) / 100.0;
                if (hx > 0.15 && hy > 0.15)
                    obstacles_.add(collide::Box{wx, wy, hx * var * 0.8, hy * var * 0.8, double((h >> 8) % 628) / 100.0});
            }
        }
        for (const auto& b0 : buildings_) {
            BuildingSnap b;
            float yaw = 0;
            int mesh = -1;
            if (!buildingPose(b0, b, yaw, mesh)) continue;
            const double wx = originX_ + b.x, wy = originY_ + b.y;
            if (b.type == "field" || b.type == "garden") continue;
            if (b.type == "hearth") {
                obstacles_.add(collide::Circle{wx, wy, 0.6});
                continue;
            }
            if (b.type == "village_fire") {
                obstacles_.add(collide::Circle{wx, wy, 1.1});
                continue;
            }
            float hx = 0, hy = 0;
            if (mesh >= 0) houseSet().halfSize(mesh, hx, hy);
            if (hx <= 0 || hy <= 0) {   // the placeholder blocks (no kit bake)
                hx = b.type == "house" ? 4.0f : b.type == "storage" ? 2.25f : 0.8f;
                hy = b.type == "house" ? 3.0f : b.type == "storage" ? 2.0f : 0.8f;
            }
            obstacles_.add(collide::Box{wx, wy, double(hx) * 0.95, double(hy) * 0.95, double(yaw)});
        }
        const BuildCatalog& cat = content_->build;
        for (const auto& [id, p] : shown_.pieces) {
            if (!p.built || p.z != 0) continue;
            const BuildPieceDef* d = cat.find(p.kind);
            if (!d) continue;
            const Vec2 c = pieceCentre(cat, *d, p);
            const double wx = originX_ + c.x, wy = originY_ + c.y;
            if (d->slot == "edge" && !d->door && (d->category == "wall" || d->category == "gable")) {
                const int r = ((p.rot % 4) + 4) % 4;
                const double dx = r == 1 ? 1 : r == 3 ? -1 : 0, dy = r == 0 ? -1 : r == 2 ? 1 : 0;
                const double ax = -dy * cat.cellM * 0.5, ay = dx * cat.cellM * 0.5;   // along the edge
                obstacles_.add(collide::Wall{wx - ax, wy - ay, wx + ax, wy + ay, 0.18});
            } else if (d->slot == "corner" || d->category == "pillar") {
                obstacles_.add(collide::Circle{wx, wy, 0.22});
            }
        }
        obstacles_.walkable = [this](double x, double y) {
            if (x < originX_ + 1 || y < originY_ + 1 || x > originX_ + simSize_ - 1 || y > originY_ + simSize_ - 1)
                return false;
            if (landAt(x, y) <= 0.3) return false;   // water
            const double e = 0.6;
            const double gx = landAt(x + e, y) - landAt(x - e, y);
            const double gy = landAt(x, y + e) - landAt(x, y - e);
            return std::hypot(gx, gy) / (2 * e) < 0.9;   // tan 42 degrees
        };
    }

    // A click on someone else: they become the target, and what can be done
    // to them opens - the wheel for a person, the act ring for a beast.
    void openActions(EntityId hit, float x, float y) {
        const ActorSnap* t = actor(hit);
        if (!t) return;
        hud_.aim = hit;
        if (!t->alive) return;
        if (t->animal) hud_.openBeast(hit, x, y);
        else {
            hud_.openWheel(hit);
            wheelWanted_ = hit;
        }
    }

    // ---- drawing ------------------------------------------------------------
    void prepare(SDL_GPUCommandBuffer* commands, const double eye[3], const float vp[16], std::uint32_t width,
                 std::uint32_t height) override {
        std::memcpy(vp_, vp, sizeof vp_);
        std::memcpy(eye_, eye, sizeof eye_);
        viewW_ = width;
        viewH_ = height;
        buildInstances();
        people_.prepare(commands, figures_);
        village_.prepare(commands, placed_);
        props_.prepare(commands, propsPlaced_);
        for (auto& [k, set] : beasts_) set->prepare(commands, beastFigures_[k]);
        if (instances_.empty() || !instBuffer_) return;
        const std::uint32_t bytes = std::uint32_t(instances_.size() * sizeof(Instance));
        void* map = SDL_MapGPUTransferBuffer(device_.handle(), instTransfer_, true);
        if (!map) return;
        std::memcpy(map, instances_.data(), bytes);
        SDL_UnmapGPUTransferBuffer(device_.handle(), instTransfer_);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        const SDL_GPUTransferBufferLocation from{instTransfer_, 0};
        const SDL_GPUBufferRegion to{instBuffer_.get(), 0, bytes};
        SDL_UploadToGPUBuffer(copy, &from, &to, true);
        SDL_EndGPUCopyPass(copy);
    }

    void drawOpaque(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass,
                    const terrain_core::LightUniforms& light) override {
        village_.draw(commands, pass, vp_, light);
        props_.draw(commands, pass, vp_, light);
        people_.draw(commands, pass, vp_, light);
        for (auto& [k, set] : beasts_) set->draw(commands, pass, vp_, light);
        if (instances_.empty() || !actorsPipe_) return;
        SDL_BindGPUGraphicsPipeline(pass, actorsPipe_.get());
        const SDL_GPUBufferBinding bufs[2] = {{vertBuffer_.get(), 0}, {instBuffer_.get(), 0}};
        SDL_BindGPUVertexBuffers(pass, 0, bufs, 2);
        SDL_PushGPUVertexUniformData(commands, 0, vp_, sizeof vp_);
        SDL_PushGPUFragmentUniformData(commands, 0, &light, sizeof light);
        for (int m = 0; m < kMeshes; ++m)
            if (batch_[m].count)
                SDL_DrawGPUPrimitives(pass, meshes_[m].count, batch_[m].count, meshes_[m].first, batch_[m].first);
    }

    void drawOverlay(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, std::uint32_t width,
                     std::uint32_t height) override {
        if (!overlayPipe_) return;
        if (canvas_.wouldResize(int(width), int(height))) {
            ui_.shutdown();
            canvas_.size(int(width), int(height));
            hud_.shutdown();
            ui_.init(canvas_.renderer());
            uiScale_ = 0;   // the fonts are set again below, at the scale
            std::string whyHud;
            if (!hud_.init(canvas_.renderer(), fs::path(opts_.root), &whyHud)) SDL_Log("altera_game: %s", whyHud.c_str());
            loadFonts();   // the theme init() read names them
            styleTheme();
            uiTexture_ = {};
        }
        if (!canvas_) return;
        SDL_Renderer* sw = canvas_.renderer();
        SDL_SetRenderDrawBlendMode(sw, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(sw, 0, 0, 0, 0);
        SDL_RenderClear(sw);
        SDL_SetRenderDrawBlendMode(sw, SDL_BLENDMODE_BLEND);
        // AL-62: the interface is laid out in points, uiScale_ pixels each (the
        // player's size times what the screen's height asks for); the writing
        // is baked at the pixels it lands on, so it stays sharp.
        const float s = uiScaleFor(settings_.uiScale, float(height));
        if (s != uiScale_ && font_) ui_.setFont(font_, fontBold_, 15, s);
        uiScale_ = s;
        hud_.scale = s;
        SDL_SetRenderScale(sw, s, s);
        ui::Input in = input_;
        in.mouseX /= s;
        in.mouseY /= s;
        ui_.begin(in, int(float(width) / s), int(float(height) / s));
        buildHud(float(width) / s, float(height) / s);
        ui_.end();
        SDL_SetRenderScale(sw, 1, 1);
        in.mouseX = input_.mouseX;
        in.mouseY = input_.mouseY;
        input_ = in;   // what the interface took (the click, the keys) goes back
        overUi_ = input_.overUi;
        SDL_RenderPresent(sw);
        input_.pressed = input_.released = input_.rightPressed = input_.doubleClick = false;
        input_.wheel = 0;
        input_.text.clear();
        input_.backspace = input_.enter = input_.escape = input_.tab = false;
        // the window takes typed text only while a field has the keyboard
        if (window_ && ui_.typing() != SDL_TextInputActive(window_)) {
            if (ui_.typing()) SDL_StartTextInput(window_);
            else SDL_StopTextInput(window_);
        }

        if (!uiTexture_) {
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = width;
            info.height = height;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            uiTexture_ = device_.makeTexture(info);
            if (uiTransfer_) SDL_ReleaseGPUTransferBuffer(device_.handle(), uiTransfer_);
            SDL_GPUTransferBufferCreateInfo t{};
            t.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            t.size = width * height * 4;
            uiTransfer_ = SDL_CreateGPUTransferBuffer(device_.handle(), &t);
            if (!uiTexture_ || !uiTransfer_) return;
        }
        const SDL_Surface* surf = canvas_.surface();
        auto* map = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(device_.handle(), uiTransfer_, true));
        if (!map) return;
        for (std::uint32_t y = 0; y < height; ++y)
            std::memcpy(map + std::size_t(y) * width * 4, static_cast<const std::uint8_t*>(surf->pixels) + std::size_t(y) * surf->pitch,
                        std::size_t(width) * 4);
        SDL_UnmapGPUTransferBuffer(device_.handle(), uiTransfer_);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = uiTransfer_;
        src.pixels_per_row = width;
        src.rows_per_layer = height;
        SDL_GPUTextureRegion dst{};
        dst.texture = uiTexture_.get();
        dst.w = width;
        dst.h = height;
        dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, true);
        SDL_EndGPUCopyPass(copy);

        SDL_GPUColorTargetInfo colour{};
        colour.texture = target;
        colour.load_op = SDL_GPU_LOADOP_LOAD;
        colour.store_op = SDL_GPU_STOREOP_STORE;
        auto* pass = SDL_BeginGPURenderPass(commands, &colour, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, overlayPipe_.get());
        const SDL_GPUTextureSamplerBinding pic{uiTexture_.get(), sampler_.get()};
        SDL_BindGPUFragmentSamplers(pass, 0, &pic, 1);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
        if (!shotWanted_.empty()) captureShot(commands, target, width, height);
    }

private:
    // ---- pictures of the game window (F9, and the scripted run) -------------
    // ALTERA_GAME_SCRIPT names a JSON list of steps, each at `at` seconds after
    // the world opened: camera (zoom, pitch, yawDeg), `select` (a person by
    // index among the living people), `speed`, `panel` (none|sheet|wheel|kin|annals),
    // `menu`, `save`, `load` (the menus and the save slots),
    // `shot` (file under terrain_lab_out/shots_for_user), `quit`.
    void loadScript() {
        const char* path = std::getenv("ALTERA_GAME_SCRIPT");
        if (!path || !*path) return;
        try {
            script_ = loadJsonFile(path);
        } catch (const std::exception& e) {
            SDL_Log("altera_game: script %s: %s", path, e.what());
        }
    }
    void runScript(double dt) {
        if (!script_.is_array()) return;
        scriptClock_ += dt;
        while (scriptStep_ < script_.size() && scriptClock_ >= script_[scriptStep_].value("at", 0.0)) {
            const Json& st = script_[scriptStep_++];
            if (st.contains("zoom")) zoom_ = st["zoom"].get<double>();
            if (st.contains("pitch")) orbitPitch_ = st["pitch"].get<double>();
            if (st.contains("yawDeg")) orbitYaw_ = st["yawDeg"].get<double>() * kPi / 180.0;
            if (st.contains("speed")) post({"speed", st["speed"].get<int>()});
            // direct control: walk [forward, side, seconds] as WASD would, run: Shift held;
            // aim_beast / aim_person: a click on the nearest such (opens its menu)
            if (st.contains("walk")) {
                scriptWalk_[0] = st["walk"][0].get<double>();
                scriptWalk_[1] = st["walk"][1].get<double>();
                scriptWalk_[2] = st["walk"][2].get<double>();
            }
            if (st.contains("run")) scriptRun_ = st["run"].get<bool>();
            // AL-70 shots: "anim" plays a state or interaction clip on the led person ("" = off)
            if (st.contains("anim")) scriptAnim_ = st["anim"].get<std::string>();
            if (st.contains("highlight")) scriptHighlight_ = st["highlight"].get<bool>();
            // checks: face_house turns the camera (and so WASD) to the nearest
            // house as drawn; probe writes where the person is and what stops
            // them to terrain_lab_out/probe.jsonl
            if (st.value("face_house", false)) faceNearestHouse();
            if (st.contains("probe")) probe(st["probe"].get<std::string>());
            if (st.contains("pause")) post({st["pause"].get<bool>() ? "hold" : "unhold"});
            // take_sex "m" | "f": the nearest grown person of that sex under control
            if (st.contains("take_sex")) {
                const char want = st["take_sex"].get<std::string>()[0];
                double px, py;
                personAt(px, py);
                const ActorSnap* best = nullptr;
                double bd = 1e18;
                for (const auto& a : shown_.actors) {
                    if (a.animal || !a.alive || a.age < 16 || a.sex != want || a.id == shown_.controlled) continue;
                    double ax, ay;
                    actorWorld(a, tickAlpha(), ax, ay);
                    const double d = std::hypot(ax - px, ay - py);
                    if (d < bd) bd = d, best = &a;
                }
                if (best) {
                    if (!st.value("look_only", false)) post({"cmd", 0, "take_control", {{"id", best->id}}});
                    select(best->id);
                }
            }
            // any sim command as the player: {"sim": "release_control", "args": {...}}
            if (st.contains("sim")) post({"cmd", 0, st["sim"].get<std::string>(), st.value("args", Json::object())});
            // the control bar: control 0 keys | 1 mouse | 2 free, camera_follow,
            // walk_to [dx, dy] metres from the person (as a click on the ground)
            if (st.contains("control")) controlMode(st["control"].get<int>());
            if (st.contains("camera_follow")) cameraFollow(st["camera_follow"].get<bool>());
            // AL-56: walls 0 up | 1 cut away | 2 down
            if (st.contains("walls")) wallMode(st["walls"].get<int>());
            if (st.contains("walk_to"))
                if (const ActorSnap* me = actor(shown_.controlled))
                    walkTo(originX_ + me->x + st["walk_to"][0].get<double>(), originY_ + me->y + st["walk_to"][1].get<double>());
            // aim_thing: the act ring on the nearest such thing (tree, deadwood, riverbank, any)
            if (st.contains("aim_thing"))
                if (const ActorSnap* me = actor(shown_.controlled)) {
                    const std::string ty = st["aim_thing"].get<std::string>();
                    const ThingSnap* best = nullptr;
                    double bd = 1e18;
                    for (const auto& t : shown_.things) {
                        if (ty != "any" && t.type != ty) continue;
                        const double d = std::hypot(t.x - me->x, t.y - me->y);
                        if (d < bd) bd = d, best = &t;
                    }
                    if (best) {
                        // AL-29: look at the thing from the person's side, back
                        // and from above, not from inside its trunk or crown
                        target_[0] = originX_ + (best->x + me->x) * 0.5;
                        target_[1] = originY_ + (best->y + me->y) * 0.5;
                        zoom_ = std::max(zoom_, 14.0 + 0.5 * bd);
                        orbitPitch_ = std::max(orbitPitch_, 0.6);
                        orbitYaw_ = std::atan2(best->y - me->y, best->x - me->x);
                        if (follow_) cameraFollow(false);
                        hud_.openThing(best->id, float(viewW_) * 0.5f, float(viewH_) * 0.45f);
                    }
                }
            if (st.contains("aim_beast") || st.contains("aim_person")) {
                const ActorSnap* me = actor(shown_.controlled);
                const std::string sp = st.value("aim_beast", std::string());
                const ActorSnap* best = nullptr;
                double bd = 1e18;
                for (const auto& a : shown_.actors) {
                    if (!a.alive || !me || a.id == me->id || a.animal != st.contains("aim_beast")) continue;
                    if (!sp.empty() && sp != "any" && a.species != sp) continue;
                    const double d = std::hypot(a.x - me->x, a.y - me->y);
                    if (d < bd) bd = d, best = &a;
                }
                if (best) {
                    double wx, wy;
                    actorWorld(*best, 1.0, wx, wy);
                    float sx = float(viewW_) * 0.5f, sy = float(viewH_) * 0.5f;
                    project(wx - eye_[0], wy - eye_[1], groundAt(wx, wy) + 0.8 - eye_[2], sx, sy);
                    openActions(best->id, sx, sy);
                }
            }
            if (st.contains("select")) {
                int k = st["select"].get<int>();
                for (const auto& a : shown_.actors)
                    if (!a.animal && a.alive && k-- == 0) {
                        select(a.id);
                        break;
                    }
            }
            if (st.contains("beast")) {
                const std::string sp = st["beast"].get<std::string>();
                for (const auto& a : shown_.actors)
                    if (a.animal && a.alive && a.species == sp) {
                        select(a.id);
                        break;
                    }
            }
            // the menus: `menu` runs a button (continue, new, open_load,
            // open_save, settings, resume, main_menu), `save` / `load` a slot
            if (st.contains("menu")) {
                const std::string m = st["menu"].get<std::string>();
                if (m == "main_menu") openMainMenu();
                else if (m == "pause") openPause();
                else menuAction(m, Json::object());
            }
            // AL-69: world_panel {spec}: the making of a world open on that
            // spec; world {spec}: create it (the game restarts on it, with
            // ALTERA_GAME_SCRIPT_NEXT as its script)
            if (st.contains("world_panel")) {
                openWorldPanel();
                if (st["world_panel"].is_object()) {
                    terrain_core::parseWorldSpec(st["world_panel"].dump(), hud_.worldDraft);
                    hud_.worldSeedText = std::to_string(hud_.worldDraft.seed);
                }
            }
            if (st.contains("next_script")) SDL_setenv_unsafe("ALTERA_GAME_SCRIPT_NEXT", st["next_script"].get<std::string>().c_str(), 1);
            if (st.contains("world") && st["world"].is_object()) menuAction("world_create", st["world"]);
            if (st.contains("save")) saveSlot(st["save"].get<int>());
            if (st.contains("load")) loadSlot(st["load"].get<int>());
            if (st.contains("panel")) {
                hud_.panel = st["panel"].get<std::string>();
                if (hud_.panel == "wheel") {
                    // the wheel on the nearest other person
                    for (const auto& a : shown_.actors)
                        if (!a.animal && a.alive && a.id != selected_) {
                            hud_.openWheel(a.id);
                            wheelWanted_ = a.id;
                            break;
                        }
                }
            }
            if (st.contains("backstory")) {
                const std::string id = st["backstory"].get<std::string>();
                for (std::size_t i = 0; i < hud_.backstories.size(); ++i)
                    if (hud_.backstories[i].id == id) hud_.backstoryPick = int(i);
            }
            if (st.value("backstory_begin", false)) hud_.backstoryBegin = true;
            if (st.contains("tone")) hud_.setTone(st["tone"].get<int>());
            if (st.contains("slot")) hud_.openSlot(st["slot"].get<int>());
            // building and the colony
            if (st.value("take_chief", false) && shown_.colony.chief != kNone)
                post({"cmd", 0, "take_control", {{"id", shown_.colony.chief}}});
            if (st.contains("mouse")) {
                input_.mouseX = st["mouse"][0].get<float>();
                input_.mouseY = st["mouse"][1].get<float>();
            }
            if (st.contains("pick")) hud_.buildPick = st["pick"].get<int>();
            if (st.contains("build_mode")) hud_.buildMode = st["build_mode"].get<std::string>();
            if (st.contains("build_storey")) buildBump_ = st["build_storey"].get<int>();
            // AL-9: a drag of the build tool between two points beside the builder, laid at once
            if (st.contains("drag") && shown_.builder != kNone && content_ && !content_->build.pieces.empty())
                if (const ActorSnap* b = actor(shown_.builder)) {
                    const Json g = st["drag"];
                    buildPick_ = std::clamp(hud_.buildPick, 0, int(content_->build.pieces.size()) - 1);
                    buildBatch(b->x + g.at(0).get<double>(), b->y + g.at(1).get<double>(), b->x + g.at(2).get<double>(),
                               b->y + g.at(3).get<double>());
                    int bad = 0;
                    for (const auto& w : batchWhy_) bad += !w.empty();
                    SDL_Log("altera_game: script drag %s: %zu pieces, %d refused, %zu removed", hud_.buildMode.c_str(),
                            laid_.size(), bad, batchRemove_.size());
                    commitBatch();
                }
            // the workshop: craft_kit gives the person materials, tools and the
            // starter recipes; craft_draft fills the attempt; craft_try runs it;
            // craft_same runs the first mastered recipe
            {
                const EntityId who = shown_.craft.actor != kNone ? shown_.craft.actor : selected_;
                if (st.value("craft_kit", false) && who != kNone) post({"craft_kit", 0, "", {{"id", who}}});
                if (st.contains("craft_draft")) {
                    const Json& d = st["craft_draft"];
                    hud_.craftProc = -1;
                    for (size_t i = 0; i < shown_.craft.processes.size(); ++i)
                        if (shown_.craft.processes[i].id == d.value("process", "")) hud_.craftProc = int(i);
                    hud_.craftSlots.clear();
                    if (d.contains("slots"))
                        for (auto it = d["slots"].begin(); it != d["slots"].end(); ++it)
                            hud_.craftSlots[it.key()] = it.value().get<std::vector<std::string>>();
                    hud_.craftShape = d.value("shape", "");
                    hud_.craftMode = d.value("mode", "");
                    hud_.craftHeat = d.value("heat", -1);
                    hud_.craftLen = d.value("len", 0);
                    if (!hud_.craftSlots.empty()) hud_.craftSlot = hud_.craftSlots.begin()->first;
                }
                // the workshop's tabs and helper (boards 10-15)
                if (st.contains("craft_tab")) hud_.craftTab = st["craft_tab"].get<int>();
                if (st.contains("craft_wish")) {
                    hud_.craftWish = st["craft_wish"].get<std::string>();
                    hud_.craftHelper = !hud_.craftWish.empty();
                }
                if (st.contains("craft_helper")) hud_.craftHelper = st["craft_helper"].get<bool>();
                if (st.value("craft_close", false)) hud_.closeCraftResult();
                if (st.value("craft_try", false) && who != kNone && hud_.craftProc >= 0 &&
                    hud_.craftProc < int(shown_.craft.processes.size())) {
                    Json a = Json::object();
                    a["id"] = who;
                    a["process"] = shown_.craft.processes[size_t(hud_.craftProc)].id;
                    Json slots = Json::object();
                    for (const auto& [k, v] : hud_.craftSlots) slots[k] = v;
                    a["slots"] = slots;
                    if (!hud_.craftShape.empty()) a["shape"] = hud_.craftShape;
                    if (!hud_.craftMode.empty()) a["mode"] = hud_.craftMode;
                    if (hud_.craftHeat >= 0) a["heat"] = hud_.craftHeat;
                    if (hud_.craftLen > 0) a["len"] = hud_.craftLen;
                    post({"cmd", 0, "craft_attempt", a});
                    hud_.craftSlots.clear();
                }
                if (st.value("craft_same", false) && who != kNone)
                    for (const auto& r : shown_.craft.recipes)
                        if (r.master) {
                            post({"cmd", 0, "craft_fast", {{"id", who}, {"recipe", r.id}, {"count", 1}}});
                            break;
                        }
            }
            if (st.contains("rot")) buildRot_ = st["rot"].get<int>();
            if (st.value("give", false) && shown_.builder != kNone) post({"give", 0, "", {{"id", shown_.builder}}});
            if (st.contains("plan") && shown_.builder != kNone)
                if (const ActorSnap* b = actor(shown_.builder)) {
                    const double dx = st.value("dx", 9.0), dy = st.value("dy", 3.0);
                    planHouse(b->x + dx, b->y + dy);
                    target_[0] = originX_ + b->x + dx + 2;
                    target_[1] = originY_ + b->y + dy + 2;
                    follow_ = false;
                }
            if (st.contains("look_at_build") && !shown_.pieces.empty()) {
                const BuildPiece& p0 = shown_.pieces.begin()->second;
                target_[0] = originX_ + (p0.x + 1) * content_->build.cellM;
                target_[1] = originY_ + (p0.y + 1) * content_->build.cellM;
                follow_ = false;
            }
            if (st.contains("take")) {
                int k = st["take"].get<int>();
                for (const auto& a : shown_.actors)
                    if (!a.animal && a.alive && k-- == 0) {
                        post({"cmd", 0, "take_control", {{"id", a.id}}});
                        break;
                    }
            }
            {
                std::string counts;
                for (const auto& [k, v] : beastFigures_) counts += k + "=" + std::to_string(v.size()) + " ";
                int shoals = 0;
                double nearest = 1e18;
                for (const auto& a : shown_.actors)
                    if (a.species == "fish_shoal" && a.alive) {
                        ++shoals;
                        nearest = std::min(nearest, std::hypot(originX_ + a.x - eye_[0], originY_ + a.y - eye_[1]));
                    }
                counts += "shoals=" + std::to_string(shoals) + " nearest=" + std::to_string(int(nearest)) + " ";
                int builtN = 0;
                for (const auto& [id, p] : shown_.pieces) builtN += p.built;
                counts += "pieces=" + std::to_string(shown_.pieces.size()) + " built=" + std::to_string(builtN) + " result=" +
                          shown_.result + " ";
                SDL_Log("altera_game: step %zu people=%zu %s", scriptStep_, figures_.size(), counts.c_str());
            }
            if (st.contains("shot"))
                shotWanted_ = (fs::path(opts_.root) / "terrain_lab_out/shots_for_user" / st["shot"].get<std::string>()).string();
            if (st.value("quit", false)) {
                SDL_Event q{};
                q.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&q);
            }
        }
    }
    void captureShot(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, std::uint32_t w, std::uint32_t h) {
        if (shotTransfer_) SDL_ReleaseGPUTransferBuffer(device_.handle(), shotTransfer_);
        SDL_GPUTransferBufferCreateInfo t{};
        t.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        t.size = w * h * 4;
        shotTransfer_ = SDL_CreateGPUTransferBuffer(device_.handle(), &t);
        if (!shotTransfer_) {
            shotWanted_.clear();
            return;
        }
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        SDL_GPUTextureRegion src{};
        src.texture = target;
        src.w = w;
        src.h = h;
        src.d = 1;
        SDL_GPUTextureTransferInfo dst{};
        dst.transfer_buffer = shotTransfer_;
        dst.pixels_per_row = w;
        dst.rows_per_layer = h;
        SDL_DownloadFromGPUTexture(copy, &src, &dst);
        SDL_EndGPUCopyPass(copy);
        shotPending_ = shotWanted_;
        shotW_ = w;
        shotH_ = h;
        shotWanted_.clear();
    }
    void finishShot() {
        if (shotPending_.empty() || !shotTransfer_) return;
        SDL_WaitForGPUIdle(device_.handle());
        const auto* px = static_cast<const std::uint8_t*>(SDL_MapGPUTransferBuffer(device_.handle(), shotTransfer_, false));
        if (px) {
            engine::world_source::Image img;
            img.width = shotW_;
            img.height = shotH_;
            img.channels = 3;
            img.allocate();
            // The view hands its own frame target (engine::Device::kColourFormat, RGBA).
            const bool bgra = engine::Device::kColourFormat == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
            for (std::size_t i = 0, n = std::size_t(shotW_) * shotH_; i < n; ++i) {
                img.bytes[i * 3 + 0] = px[i * 4 + (bgra ? 2 : 0)];
                img.bytes[i * 3 + 1] = px[i * 4 + 1];
                img.bytes[i * 3 + 2] = px[i * 4 + (bgra ? 0 : 2)];
            }
            SDL_UnmapGPUTransferBuffer(device_.handle(), shotTransfer_);
            std::error_code ec;
            fs::create_directories(fs::path(shotPending_).parent_path(), ec);
            std::string why;
            if (!engine::world_source::writePng(shotPending_, img, &why)) SDL_Log("altera_game: shot: %s", why.c_str());
            else SDL_Log("altera_game: saved %s", shotPending_.c_str());
        }
        shotPending_.clear();
    }

    struct Command {
        std::string op;
        int n = 0;
        std::string type;
        Json args = Json::object();
    };

    // ---- the sim thread -------------------------------------------------
    void post(Command c) {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        commands_.push_back(std::move(c));
    }

    void simLoop() {
        auto last = Clock::now();
        double owed = 0;
        auto lastSnap = Clock::now();
        while (!stop_) {
            bool changed = false;
            std::deque<Command> todo;
            {
                std::lock_guard<std::mutex> lock(cmdMutex_);
                todo.swap(commands_);
            }
            for (auto& c : todo) try {
                changed = true;
                if (c.op == "pause") paused_ = !paused_;
                else if (c.op == "hold") paused_ = true;     // the menus
                else if (c.op == "unhold") paused_ = false;
                else if (c.op == "speed") {
                    speed_ = std::clamp(c.n, 1, 4);
                    paused_ = false;
                } else if (c.op == "give") {
                    // tour scripts: the builder's household gets what a house takes
                    if (Character* b = world_->character(c.args.value("id", kNone)))
                        for (const char* it : {"stone", "log", "branch", "clay", "thatch"})
                            b->inventory[it] += std::string(it) == "thatch" ? 12 : 40;
                } else if (c.op == "craft_draft") {
                    craftDraft_ = c.args;
                } else if (c.op == "steer") {
                    // direct control: no result line for each step, and no slide
                    // back between ticks (the step is where the person stands now)
                    altera::sim::Command cmd;
                    cmd.source = "player:1";
                    cmd.type = "steer";
                    cmd.args = c.args;
                    cmd.args["player"] = 1;
                    const auto r = world_->applyNow(cmd);
                    if (r.result != "ok") result_ = r.result;
                    if (const Character* me = world_->character(c.args.value("id", kNone))) prevPos_[me->id] = me->pos;
                } else if (c.op == "say_topic") {
                    // AL-67: the player's person brings a topic up (the menu without an agent)
                    Character* me = world_->character(c.args.value("id", kNone));
                    Character* t = world_->character(c.args.value("target", kNone));
                    const int k = c.args.value("index", -1);
                    if (me && t && k >= 0) {
                        const auto topics = dialogueTopics(*world_, *me, *t);
                        if (k < int(topics.size())) {
                            const MenuTopic& tp = topics[std::size_t(k)];
                            say(*world_, *me, *t, tp.act, tp.prop, tp.epi);
                            result_ = "ok";
                        }
                    }
                } else if (c.op == "set_name") {
                    // AL-67: a name the player gave, with its case forms (sim/loc.hpp setOwnName)
                    if (Character* me = world_->character(c.args.value("id", kNone))) {
                        setOwnName(*me, opts_.lang, c.args.value("forms", Json::object()));
                        result_ = "ok";
                    }
                } else if (c.op == "craft_kit") {
                    if (Character* b = world_->character(c.args.value("id", kNone))) craftKit(*b);
                } else if (c.op == "cmd") {
                    altera::sim::Command cmd;
                    cmd.source = "player:1";
                    cmd.type = c.type;
                    cmd.args = c.args;
                    cmd.args["player"] = 1;
                    const auto r = world_->applyNow(cmd);
                    result_ = r.result;
                    if (c.type == "start_backstory" && r.result.rfind("rejected", 0) != 0) {
                        paused_ = false;
                        startedAs_ = true;
                    }
                }
            } catch (const std::exception& e) {
                // a bad command is reported, not fatal
                SDL_Log("altera_game: command %s/%s failed: %s", c.op.c_str(), c.type.c_str(), e.what());
                result_ = std::string("rejected:") + e.what();
            }
            const auto now = Clock::now();
            const double dt = std::chrono::duration<double>(now - last).count();
            last = now;
            const double tps = kSpeeds[speed_];
            if (!paused_) owed = std::min(owed + dt * tps, 4.0);
            else owed = 0;
            int steps = 0;
            while (owed >= 1.0 && steps < 8) {
                for (const auto& [id, c] : world_->characters()) prevPos_[id] = c.pos;
                try {
                    world_->step();
                } catch (const std::exception& e) {
                    // a broken tick must not take the game down: say what and stop the clock
                    SDL_Log("altera_game: sim tick %lld failed: %s", (long long)world_->now(), e.what());
                    result_ = std::string("sim_error:") + e.what();
                    paused_ = true;
                    owed = 0;
                    break;
                }
                owed -= 1.0;
                ++steps;
                changed = true;
                tickAt_ = Clock::now();
            }
            if (changed || selectedWanted_ != snapSelected_ ||
                std::chrono::duration<double>(Clock::now() - lastSnap).count() > 0.25) {
                try {
                    snapshotLocked();
                } catch (const std::exception& e) {
                    SDL_Log("altera_game: snapshot failed: %s", e.what());
                }
                lastSnap = Clock::now();
            }
            if (!steps) std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }

    // ---- the backstory screen ----
    void backstoryCards() {
        Json ui = Json::object();
        try {
            ui = loadJsonFile(opts_.root + "/content/ui/backstory.json");
        } catch (const std::exception& e) {
            SDL_Log("altera_game: backstory ui: %s", e.what());
        }
        const Json itemIcons = ui.value("item_icons", Json::object());
        const std::string itemDefault = ui.value("item_icon_default", std::string("things"));
        const Json startIcons = ui.value("start_icons", Json::object());
        if (itemIcons.contains("coin")) hud_.coinIcon = itemIcons["coin"].get<std::string>();
        hud_.backstories.clear();
        for (const BackstoryDef& b : content_->backstories) {
            Hud::BackstoryCard c;
            c.id = b.id;
            c.icon = b.icon;
            c.name = text("backstory." + b.id + ".name");
            c.desc = text("backstory." + b.id + ".desc");
            c.life = text("backstory." + b.id + ".life");
            c.start = text("hud.backstory.start." + b.start);
            c.startIcon = startIcons.value(b.start, std::string("map"));
            for (const auto& [k, lvl] : b.skills) c.skills.push_back({name("knowledge", k), lvl});
            for (const auto& t : b.traitsAlways) c.traits.push_back(name("trait", t));
            for (const auto& t : b.traitPool) c.pool.push_back(name("trait", t));
            c.pick = b.traitPick;
            for (const auto& [a, v] : b.attrs)
                if (std::fabs(v) >= 0.03) c.attrs.push_back({text("attr." + a + ".name"), v});
            for (const auto& [it, n] : b.items) c.gear.push_back({itemIcons.value(it, itemDefault), name("item", it), n});
            for (const auto& m : b.made) c.gear.push_back({itemIcons.value(m.design, itemDefault), name("form", m.design), 1});
            c.coins = b.coins;
            for (const auto& d : b.recipes) c.recipes.push_back(name("form", d));
            for (const auto& r : b.rights) c.rights.push_back(text("hud.right." + r));
            hud_.backstories.push_back(std::move(c));
        }
    }

    // ---- the workshop (gdd crafting §9) ----
    void craftKit(Character& c) {
        // a tour kit: gathered materials, a knife, a hammer and a needle made by
        // a master, and the starter recipes of a few crafts
        for (const char* it : {"fiber_raw", "log", "stone", "clay", "hide", "meat", "bast_fiber", "branch", "grain"})
            c.inventory[it] += 6;
        const auto& d = world_->content().craft;
        if (!d.loaded) return;
        craft::Actor master;
        for (const auto& [pid, p] : d.processes) master.skills[p.skill] = 0.95;
        craft::Workplace wp;
        for (const auto& [id, st] : d.stations) wp.stations.push_back(id);
        craft::Tool t;
        t.tags = {"cutting", "saw", "striking", "tongs", "whetstone", "grit", "needle", "bellows"};
        t.hardness = 0.9;
        t.q = 0.8;
        wp.tools.push_back(t);
        for (const char* ds : {"knife", "hammer_tool", "bone_needle", "sword"})
            if (const craft::DesignDef* def = d.design(ds)) {
                craft::Recipe r = craft::recipeFromDesign(d, *def, 1.0, true);
                std::vector<craft::Item> pool;
                craft::ChainOpts o;
                o.rawFromNothing = true;
                o.stopOnRisk = false;
                craft::ChainRun run = craft::runChain(d, r.steps, pool, master, wp, 7, 0, o);
                if (run.done) c.craft.items.push_back(run.out);
            }
        for (const char* ds : {"cord", "thread", "roast_meat", "clay_pot", "plank", "knife"}) giveDesign(*world_, c, ds);
        c.skills["spinning"] = std::max(c.skills["spinning"], 0.5);
        c.skills["carpentry"] = std::max(c.skills["carpentry"], 0.4);
        c.skills["cooking"] = std::max(c.skills["cooking"], 0.5);
    }

    void craftSnap(const World& w, const Character& c, CraftSnap& cs) const {
        const auto& d = w.content().craft;
        if (!d.loaded) return;
        cs.actor = c.id;
        const std::string& L = opts_.lang;
        for (const auto& [item, n] : c.inventory) {
            auto def = w.content().items.find(item);
            if (n <= 0 || def == w.content().items.end() || def->second.material.empty()) continue;
            const craft::MaterialDef* m = d.material(def->second.material);
            CraftThing t{"inv:" + item, name("item", item), m ? m->shape : "", "", n, -1};
            t.icon = craftIcon("shape_icons", t.shape, "s_lump");
            if (m)
                for (const auto& tag : m->tags)
                    if (craftUi()["material_icons"].contains(tag)) {
                        t.icon = craftUi()["material_icons"][tag].get<std::string>();
                        break;
                    }
            cs.materials.push_back(std::move(t));
        }
        for (size_t i = 0; i < c.craft.items.size(); ++i) {
            const craft::Item& it = c.craft.items[i];
            std::string note = text("craft.tier." + craft::qualityTier(d, it.q));
            bool flawed = false;
            for (const auto& f : it.flaws)
                if (f.revealed) {
                    note += ", " + text("flaw." + f.id + ".name") + "!";
                    flawed = true;
                }
            if (it.wear > 0.05) note += ", " + text("hud.craft.worn", {{"n", int(it.wear * 100)}});
            CraftThing t{"made:" + std::to_string(i), craftItemName(*loc_, L, d, it), it.shape, note, 1, it.q};
            t.icon = craftIcon("shape_icons", it.shape, "s_lump");
            t.wear = it.wear;
            t.flawed = flawed;
            cs.made.push_back(std::move(t));
        }
        const craft::Workplace wp = craftWorkplace(w, c);
        for (const auto& st : wp.stations)
            if (d.station(st)) cs.stations.push_back(text("station." + st + ".name"));
        // where the stations stand (board 10: their gauges hang over them in the world)
        for (const auto& st : wp.stations) {
            const craft::StationDef* sd = d.station(st);
            if (!sd) continue;
            CraftStationSnap ss;
            ss.id = st;
            ss.name = text("station." + st + ".name");
            ss.tempMax = sd->tempMax;
            double best = 1e18;
            for (const auto& [type, od] : w.content().objects) {
                if (od.station != st) continue;
                for (EntityId id : w.objectsOfType(type))
                    if (const WorldObject* o = w.object(id)) {
                        const double dd = dist(o->pos, c.pos);
                        if (dd <= 12.0 && dd < best) {
                            best = dd;
                            ss.placed = true;
                            ss.x = o->pos.x;
                            ss.y = o->pos.y;
                        }
                    }
            }
            cs.heatMax = std::max(cs.heatMax, sd->tempMax);
            cs.stationsAt.push_back(std::move(ss));
        }
        for (const auto& pid : d.processOrder) {
            const craft::ProcessDef& p = d.processes.at(pid);
            CraftProcessSnap ps;
            ps.id = pid;
            ps.name = text("process." + pid + ".name");
            ps.stations = p.stations;
            ps.heatMin = p.heatMin;
            ps.time = p.time;
            if (!p.stations.empty()) {
                bool any = false;
                for (const auto& s : p.stations)
                    if (std::find(wp.stations.begin(), wp.stations.end(), s) != wp.stations.end()) any = true;
                if (!any) {
                    ps.available = false;
                    for (const auto& s : p.stations)
                        ps.need += (ps.need.empty() ? "" : " / ") + text("station." + s + ".name");
                }
            }
            if (ps.available && p.heatMin > 0 && cs.heatMax < p.heatMin) {
                ps.available = false;
                ps.need = text("hud.craft.need_heat", {{"n", p.heatMin}});
            }
            for (const auto& sl : p.slots) ps.slots.push_back({sl.name, text("hud.craft.slot." + sl.name)});
            for (const auto& m : p.modes) ps.modes.push_back({m, text("process.mode." + m + ".name")});
            for (const auto& [from, tos] : p.shapeMap)
                for (const auto& to : tos) ps.targets[from].push_back({to, text("shape." + to + ".name")});
            cs.processes.push_back(std::move(ps));
        }
        for (const auto& r : c.craft.book.recipes) {
            CraftRecipeSnap rs;
            rs.id = r.id;
            rs.name = !r.nameUser.empty() ? r.nameUser : !r.nameKey.empty() ? text(r.nameKey) : text("hud.craft.unnamed");
            rs.mastery = r.mastery;
            rs.bestQ = r.bestQ;
            rs.known = r.knownCount();
            rs.total = int(r.steps.size());
            rs.master = r.master(d.cfg);
            rs.successes = r.successes;
            for (const auto& st : r.steps) {
                rs.steps.push_back(st.step.process);
                rs.stepKnown.push_back(st.known);
            }
            const craft::ChainPlan plan = fastCraftPlan(w, c, r.id);
            rs.stages = plan.stages;
            rs.time = plan.time;
            rs.expectedQ = plan.expectedQ;
            for (const auto& m : plan.missing) rs.missing.push_back(craftRefName(m));
            for (const auto& s : plan.needStations) rs.missing.push_back(text("station." + s + ".name"));
            cs.recipes.push_back(std::move(rs));
        }
        // the draft's forecast (§9): blurred for a novice
        if (craftDraft_.contains("process")) {
            CraftCommand cmd;
            cmd.process = craftDraft_.value("process", "");
            if (craftDraft_.contains("slots"))
                for (auto it = craftDraft_["slots"].begin(); it != craftDraft_["slots"].end(); ++it)
                    cmd.slots[it.key()] = it.value().get<std::vector<std::string>>();
            cmd.target.shape = craftDraft_.value("shape", "");
            cmd.target.mode = craftDraft_.value("mode", "");
            cmd.target.heat = craftDraft_.value("heat", -1);
            cmd.target.len = craftDraft_.value("len", 0);
            const craft::Preview pv = craftPreview(w, c, cmd);
            cs.canStart = pv.canStart;
            cs.expectedQ = pv.expectedQ;
            cs.qSpread = pv.qSpread;
            cs.pRuin = pv.pRuin;
            cs.pFlaw = pv.pFlaw;
            cs.certainty = pv.certainty;
            cs.time = pv.time;
            cs.likely = pv.likelyForm.empty() ? "" : text("form." + pv.likelyForm + ".name");
            cs.tier = text("craft.tier." + craft::qualityTier(d, pv.expectedQ));
            if (pv.canStart) {
                const std::string tier = text("craft.tier." + craft::qualityTier(d, pv.expectedQ));
                const std::string likely = pv.likelyForm.empty()
                                               ? text("craft.unknown")
                                               : text("craft.likely", {{"form", text("form." + pv.likelyForm + ".name")}});
                cs.forecast = text(pv.certainty >= 0.5 ? "hud.craft.forecast" : "hud.craft.forecast_vague",
                                   {{"tier", tier}, {"ruin", int(std::lround(pv.pRuin * 100))},
                                    {"flaw", int(std::lround(pv.pFlaw * 100))}, {"time", pv.time}, {"likely", likely}});
            } else {
                Json args = {{"slot", text("hud.craft.slot." + pv.reasonArg)}, {"tool", pv.reasonArg}, {"station", pv.reasonArg},
                             {"need", pv.reasonArg}, {"level", pv.reasonArg}, {"mode", pv.reasonArg},
                             {"shape", text("shape." + pv.reasonArg + ".name")}};
                if (d.station(pv.reasonArg)) args["station"] = text("station." + pv.reasonArg + ".name");
                cs.why = text("craft.reason." + pv.reason, args);
            }
        }
        if (!c.craft.lastOutcome.empty()) {
            std::string what;
            if (c.craft.lastMade >= 0 && c.craft.lastMade < int(c.craft.items.size()))
                what = craftItemName(*loc_, L, d, c.craft.items[size_t(c.craft.lastMade)]);
            const std::string head = loc_->has(L, "craft.outcome." + c.craft.lastOutcome)
                                         ? text("craft.outcome." + c.craft.lastOutcome)
                                         : loc_->has(L, "craft.stop." + c.craft.lastOutcome)
                                         ? text("craft.stop." + c.craft.lastOutcome, {{"what", c.craft.lastArg}, {"why", c.craft.lastArg},
                                                                                      {"stages", c.craft.lastArg}})
                                         : text("craft.stop.other", {{"why", c.craft.lastOutcome}}); // AL-27: never a raw key
            cs.last = what.empty() ? head : text("hud.craft.last", {{"outcome", head}, {"what", what}});
            if (c.craft.lastBreakthrough_) cs.last += "  " + text("hud.craft.breakthrough");
            // the result card (board 11) and the insight (board 13)
            CraftResultSnap& rs = cs.result;
            rs.seq = c.craft.lastSeq;
            rs.code = c.craft.lastOutcome;
            rs.head = head;
            rs.what = what;
            rs.made = c.craft.lastMade;
            rs.started = c.craft.lastMade >= 0 || loc_->has(L, "craft.outcome." + c.craft.lastOutcome);
            rs.breakthrough = c.craft.lastBreakthrough_;
            rs.recipe = c.craft.lastRecipe;
            if (const craft::Recipe* r = c.craft.book.find(rs.recipe))
                rs.recipeName = !r->nameUser.empty() ? r->nameUser : !r->nameKey.empty() ? text(r->nameKey) : text("hud.craft.unnamed");
            if (rs.made >= 0 && rs.made < int(c.craft.items.size())) {
                const craft::Item& it = c.craft.items[size_t(rs.made)];
                rs.q = it.q;
                rs.tier = text("craft.tier." + craft::qualityTier(d, it.q));
                for (const auto& t : it.trace) rs.links.push_back({text("process." + t.process + ".name"), t.q});
                for (const auto& f : it.flaws)
                    if (f.revealed) rs.flaws.push_back(text("flaw." + f.id + ".name"));
            }
        }
        craftCandidates(w, c, wp, cs);
    }

    // ---- the workshop's look (content/ui/craft.json) and the novice helper ----
    const Json& craftUi() const {
        if (craftUi_.is_null()) {
            try {
                craftUi_ = sim::loadJsonFile((fs::path(opts_.root) / "content/ui/craft.json").string());
            } catch (const std::exception&) {
                craftUi_ = Json::object();
            }
        }
        return craftUi_;
    }
    std::string craftIcon(const char* map, const std::string& id, const char* fallback) const {
        const Json& u = craftUi();
        if (u.contains(map) && u[map].contains(id)) return u[map][id].get<std::string>();
        return fallback;
    }
    // "mat:<id>", "form:<id>", "shape:<id>", "tag:<t>" as a name to show
    std::string craftRefName(const std::string& ref) const {
        const auto colon = ref.find(':');
        if (colon == std::string::npos) return ref;
        const std::string kind = ref.substr(0, colon), id = ref.substr(colon + 1);
        const std::string& L = opts_.lang;
        if (kind == "mat") {
            if (loc_->has(L, "material." + id + ".name")) return text("material." + id + ".name");
            for (const auto& [item, def] : world_->content().items)
                if (def.material == id) return name("item", item);
            return id;
        }
        if (kind == "form" || kind == "shape" || kind == "station") return text(kind + "." + id + ".name");
        return id;
    }

    // The novice helper (board 10b, gdd crafting §17): the designs whose form
    // answers the wish, how well they fit what the person has, the stations
    // along their chain and the step that can be done now as a draft.
    void craftCandidates(const World& w, const Character& c, const craft::Workplace& wp, CraftSnap& cs) const {
        const auto& d = w.content().craft;
        cs.wish = craftDraft_.value("wish", "");
        if (cs.wish.empty()) return;
        const Json& u = craftUi();
        Json wish;
        for (const auto& x : u.value("wishes", Json::array()))
            if (x.value("id", "") == cs.wish) wish = x;
        if (wish.is_null()) return;
        const auto needs = wish.value("needs", std::vector<std::string>{});
        const auto cats = wish.value("categories", std::vector<std::string>{});
        auto has = [&](const std::vector<std::string>& v, const std::string& s) {
            return std::find(v.begin(), v.end(), s) != v.end();
        };
        // what the person has, for resolving a step's inputs
        auto invOf = [&](const std::string& mat) -> std::string {
            for (const auto& [item, n] : c.inventory) {
                if (n <= 0) continue;
                auto def = w.content().items.find(item);
                if (def != w.content().items.end() && def->second.material == mat) return item;
            }
            return "";
        };
        std::vector<CraftCandidate> all;
        for (const auto& ds : d.designs) {
            const craft::FormDef* f = d.form(ds.form);
            if (!f) continue;
            bool match = has(cats, f->category);
            for (const auto& n : f->needs) {
                const std::string k = n.key.substr(0, n.key.find('.'));
                if (has(needs, n.key) || has(needs, k)) match = true;
            }
            if (!match) continue;
            CraftCandidate cd;
            cd.id = ds.id;
            cd.name = text("form." + ds.form + ".name");
            cd.icon = f->shape.empty() ? "s_lump" : craftIcon("shape_icons", f->shape.front(), "s_lump");
            const craft::ChainPlan plan = designPlan(w, c, ds.id);
            double fit = std::clamp(plan.expectedQ, 0.0, 1.0);
            if (!plan.needStations.empty()) fit *= 0.6;
            fit *= std::pow(0.75, double(std::min<size_t>(plan.missing.size(), 4)));
            cd.fit = fit;
            for (const auto& m : plan.missing) cd.missing.push_back(craftRefName(m));
            // the chain along the stations; the first step the person can do now
            std::vector<int> usedMade;
            bool finished = false;
            for (const auto& it : c.craft.items)
                if (it.form == ds.form) finished = true;
            for (size_t i = 0; i < ds.steps.size(); ++i) {
                const craft::DesignStep& st = ds.steps[i];
                std::string at;
                if (const craft::ProcessDef* p = d.process(st.process)) {
                    for (const auto& s : p->stations)
                        if (std::find(wp.stations.begin(), wp.stations.end(), s) != wp.stations.end()) {
                            at = s;
                            break;
                        }
                    if (at.empty() && !p->stations.empty()) at = p->stations.front();
                }
                cd.steps.push_back({st.process, at});
                if (finished || cd.next >= 0) continue;
                // already made? (a stored piece of this step's shape)
                bool made = false;
                if (!st.shape.empty())
                    for (const auto& it : c.craft.items)
                        if (it.shape == st.shape) made = true;
                if (made) continue;
                Json slots = Json::object();
                bool ok = true;
                std::vector<int> taken;
                for (const auto& [slot, ins] : st.slots) {
                    Json refs = Json::array();
                    for (const auto& in : ins) {
                        const auto colon = in.ref.find(':');
                        const std::string kind = in.ref.substr(0, colon), id = in.ref.substr(colon + 1);
                        if (kind == "mat") {
                            const std::string item = invOf(id);
                            if (item.empty()) ok = false;
                            for (int k = 0; k < in.count && ok; ++k) refs.push_back("inv:" + item);
                        } else {
                            // a made piece: step:<n> (1-based), form:<id>, shape:<id>
                            std::string wantShape, wantForm;
                            if (kind == "step") {
                                const int n = std::atoi(id.c_str()) - 1;
                                if (n >= 0 && n < int(ds.steps.size())) wantShape = ds.steps[size_t(n)].shape;
                            } else if (kind == "form") wantForm = id;
                            else if (kind == "shape") wantShape = id;
                            int found = -1;
                            for (size_t m = 0; m < c.craft.items.size() && found < 0; ++m) {
                                if (std::find(taken.begin(), taken.end(), int(m)) != taken.end()) continue;
                                const craft::Item& it = c.craft.items[m];
                                if ((!wantShape.empty() && it.shape == wantShape) || (!wantForm.empty() && it.form == wantForm))
                                    found = int(m);
                            }
                            if (found < 0) ok = false;
                            else {
                                taken.push_back(found);
                                refs.push_back("made:" + std::to_string(found));
                            }
                        }
                    }
                    slots[slot] = refs;
                }
                if (!ok) continue;
                cd.next = int(i);
                cd.draft = {{"process", st.process}, {"slots", slots}};
                if (!st.shape.empty()) cd.draft["shape"] = st.shape;
                if (!st.mode.empty()) cd.draft["mode"] = st.mode;
                if (st.len > 0) cd.draft["len"] = st.len;
                if (st.heat >= 0) cd.draft["heat"] = st.heat;
            }
            all.push_back(std::move(cd));
        }
        std::stable_sort(all.begin(), all.end(), [](const CraftCandidate& a, const CraftCandidate& b) { return a.fit > b.fit; });
        const size_t keep = size_t(std::max(1, u.value("helper_max", 3)));
        if (all.size() > keep) all.resize(keep);
        cs.candidates = std::move(all);
    }

    std::string name(const std::string& kind, const std::string& id) const {
        if (id.empty()) return "";
        const std::string base = id.substr(0, id.find('@')); // <id>@foreign (AL-32) reads as the act
        const std::string k = kind + "." + base + ".name";
        return loc_->has(opts_.lang, k) ? loc_->text(opts_.lang, k) : base;
    }
    std::string text(const std::string& key, const Json& args = Json::object()) const override {
        return loc_->has(opts_.lang, key) ? loc_->text(opts_.lang, key, args) : key;
    }

    // Built on the sim thread (or before it starts): nothing else touches the world.
    void snapshotLocked() {
        const World& w = *world_;
        Snapshot s;
        s.tick = w.now();
        s.tickAt = tickAt_;
        s.tickSeconds = kSpeeds[speed_] > 0 ? 1.0 / kSpeeds[speed_] : 1.0;
        s.season = w.season();
        s.paused = paused_;
        s.speed = speed_;
        // AL-27: the sim's answer in words, never a bare "rejected:<code>"
        s.result = result_;
        if (result_.rfind("rejected:", 0) == 0) {
            const std::string code = result_.substr(9);
            const std::string t = reasonText(*loc_, opts_.lang, code);
            s.result = t != code ? text("hud.result.rejected", {{"why", t}}) : text("hud.result.unknown", {{"code", code}});
        }
        if (!w.settlements().empty()) {
            s.settlement = w.settlements().begin()->second.name;
            const DayWeather dw = weatherToday(w, w.settlements().begin()->first);
            s.tempC = dw.tempC;
            s.rain = dw.rainMm;
        }
        s.actors.reserve(w.characters().size());
        for (const auto& [id, c] : w.characters()) {
            ActorSnap a;
            a.id = id;
            a.animal = c.isAnimal();
            a.name = a.animal ? name("species", c.species) : c.name;
            a.species = c.species;
            a.group = c.group;
            const Family* f = w.familyOf(c);
            a.family = f ? f->name : "";
            a.alive = c.body.alive;
            a.player = c.controller.isPlayer();
            a.down = c.body.downUntil > w.now();
            a.sex = c.body.sex;
            a.age = c.body.ageDays / kDaysPerYear;
            a.hp = c.body.health;
            a.wounds = int(c.wounds.size());
            a.state = c.ai.fsm.state;
            a.fighting = a.state == "Fight";
            a.captive = c.captivity.held();
            if (a.captive) a.down = false; // led along on foot, not lying
            a.interaction = c.ai.fsm.interaction;
            a.anim = c.ai.fsm.anim;
            a.role = c.status.role;
            a.spouse = c.status.spouse;
            a.mother = c.status.mother;
            a.father = c.status.father;
            for (const auto& [k, v] : c.needs)
                if (v < a.lowNeed) {
                    a.lowNeed = v;
                    a.lowNeedId = k;
                }
            if (!c.ai.fsm.interaction.empty() && c.ai.fsm.interaction != "goto" && c.ai.fsm.interaction != "wait")
                a.act = name("interaction", c.ai.fsm.interaction);
            else if (c.ai.plan.actionIndex + 1 < c.ai.plan.actions.size() &&
                     c.ai.plan.actions[c.ai.plan.actionIndex + 1].interaction != "goto")
                a.act = name("interaction", c.ai.plan.actions[c.ai.plan.actionIndex + 1].interaction);
            if (c.ai.goal.valid())
                a.goal = c.ai.goal.id == "player_action" ? name("interaction", c.ai.goal.task) : name("goal", c.ai.goal.id);
            if (c.ai.fsm.state != "Idle" && c.ai.fsm.state != "Move" && !c.ai.fsm.interaction.empty()) {
                a.actTarget = c.ai.fsm.target;
                if (const InteractionDef* d = w.content().interaction(c.ai.fsm.interaction); d && d->duration > 1) {
                    a.actTicks = int(d->duration);
                    a.progress = std::clamp(double(w.now() - c.ai.fsm.started) / double(d->duration), 0.0, 1.0);
                }
            }
            const auto pp = prevPos_.find(id);
            a.x = c.pos.x;
            a.y = c.pos.y;
            a.px = pp != prevPos_.end() ? pp->second.x : c.pos.x;
            a.py = pp != prevPos_.end() ? pp->second.y : c.pos.y;
            s.actors.push_back(std::move(a));
        }
        const auto& log = w.chronicleLog();
        for (size_t i = log.size() > 40 ? log.size() - 40 : 0; i < log.size(); ++i) {
            const ChronicleEntry& e = log[i];
            Line l;
            l.day = int(e.tick / kTicksPerDay) + 1;
            l.time = clockText(e.tick);
            l.kind = e.type;
            l.who = e.who;
            l.text = loc_->has(opts_.lang, e.code) ? loc_->text(opts_.lang, e.code, locArgs(*loc_, opts_.lang, w, e.params))
                                                   : e.code;
            s.chronicle.push_back(std::move(l));
        }
        const EntityId sel = selectedWanted_;
        snapSelected_ = sel;
        if (const Character* c = w.character(sel)) {
            Detail& d = s.detail;
            d.id = sel;
            for (const auto& [k, v] : c->needs) d.needs.push_back({k, name("need", k), v});
            d.mood = c->mood(w.now());
            d.fatigue = c->body.fatigue;
            d.blood = c->body.blood;
            d.freeWill = c->controller.freeWill;
            for (const auto& [k, v] : c->inventory)
                if (v > 0) d.inventory.push_back({k, name("item", k), v});
            gearSnap(w, *c, d.gear);
            for (const auto& t : c->traits) d.traits.push_back(name("trait", t));
            for (const auto& m : c->moodlets)
                if (m.until > w.now()) d.moodlets.push_back({name("moodlet", m.id), m.value});
            for (const auto& [k, v] : c->skills) d.skills.push_back({name("skill", k), v});
            std::sort(d.skills.begin(), d.skills.end(), [](auto& x, auto& y) { return x.second > y.second; });
            for (const auto& [k, v] : c->reputation) // keys are "reputation:<axis>" (AL-27)
                d.reputation.push_back({name("reputation", k.rfind("reputation:", 0) == 0 ? k.substr(11) : k), v});
            for (const auto& [oid, r] : c->relations) {
                const Character* o = w.character(oid);
                if (!o || o->isAnimal()) continue;
                RelSnap rs;
                rs.id = oid;
                rs.name = o->name;
                rs.kin = oid == c->status.spouse ? "spouse" : oid == c->status.mother ? "mother"
                       : oid == c->status.father ? "father"
                       : std::find(c->status.children.begin(), c->status.children.end(), oid) != c->status.children.end() ? "child"
                       : "";
                rs.affinity = r.affinity, rs.trust = r.trust, rs.fear = r.fear, rs.enmity = r.enmity;
                rs.respect = r.respect, rs.familiarity = r.familiarity;
                d.relations.push_back(rs);
            }
            std::sort(d.relations.begin(), d.relations.end(), [](const RelSnap& x, const RelSnap& y) {
                const double a = (x.kin.empty() ? 0 : 2) + std::fabs(x.affinity) + x.familiarity;
                const double b = (y.kin.empty() ? 0 : 2) + std::fabs(y.affinity) + y.familiarity;
                return a > b;
            });
            if (c->ai.goal.valid())
                d.wants.push_back(c->ai.goal.id == "player_action" ? name("interaction", c->ai.goal.task) : name("goal", c->ai.goal.id));
            for (const auto& q : c->ai.queue) d.wants.push_back(name("interaction", q.task));
            const size_t from = c->ai.trace.size() > 14 ? c->ai.trace.size() - 14 : 0;
            for (size_t i = from; i < c->ai.trace.size(); ++i) {
                const TraceEntry& e = c->ai.trace[i];
                d.trace.push_back({int(e.tick / kTicksPerDay) + 1, clockText(e.tick), traceText(*loc_, opts_.lang, w, *c, e), "", {}});
            }
        }
        // The wheel: how its target feels about the player's own character.
        for (const auto& [id, c] : w.characters())
            if (c.controller.isPlayer()) s.controlled = id;
        s.wheel.target = wheelWanted_;
        s.wheel.actor = s.controlled;
        if (const Character* t = w.character(wheelWanted_)) {
            const auto it = t->relations.find(s.controlled);
            if (it != t->relations.end()) {
                s.wheel.toActor.affinity = it->second.affinity, s.wheel.toActor.trust = it->second.trust;
                s.wheel.toActor.fear = it->second.fear, s.wheel.toActor.enmity = it->second.enmity;
                s.wheel.toActor.respect = it->second.respect, s.wheel.toActor.familiarity = it->second.familiarity;
            }
        }
        wheelChances(s.wheel);
        socialSnap(w, s);
        talkSnap(w, s);
        thingsSnap(w, s);
        actsSnap(w, s);
        feedbackSnap(w, s);
        speechSnap(w, s);
        s.centre[0] = villageCentre_[0];
        s.centre[1] = villageCentre_[1];
        s.mapSize = w.map().sizeM;
        // building: the pieces, what the planned ones lack, who builds
        s.pieces = w.pieces();
        for (const auto& [id, p] : w.pieces())
            if (!p.built) s.missing[id] = missingOf(w, p);
        const EntityId by = s.controlled != kNone ? s.controlled : sel;
        const Character* bc = w.character(by);
        if (bc && !bc->isAnimal() && bc->body.alive) {
            s.builder = by;
            s.builderFamily = bc->status.family;
        }
        if (bc && !bc->isAnimal() && bc->body.alive) craftSnap(w, *bc, s.craft);
        // the colony (GDD §2.2)
        if (!w.settlements().empty()) {
            const Settlement& st = w.settlements().begin()->second;
            ColonySnap& cs = s.colony;
            cs.settlement = st.id;
            cs.institution = st.institution;
            cs.chief = st.chief;
            cs.rights = w.content().config.rights;
            const Character* chief = w.character(st.chief);
            if (chief) {
                cs.chiefName = chief->name;
                const Family* f = w.familyOf(*chief);
                cs.chiefFamily = f ? f->name : "";
            }
            cs.judge = judgeNow(w, st.id);
            if (const Character* j = w.character(cs.judge)) {
                cs.judgeName = j->name;
                cs.judgeByRight = j->status.rights.count("judge") > 0;
            }
            cs.mandate = chief && bc && chief->body.alive && chief->status.dynasty == bc->status.dynasty;
            cs.actor = cs.mandate ? chief->id : kNone;
            for (const auto& [id, c] : w.characters()) {
                if (c.isAnimal() || !c.body.alive || c.status.settlement != st.id || c.body.stage() == "child") continue;
                ColonyMember m;
                m.id = id;
                m.name = c.name;
                const Family* f = w.familyOf(c);
                m.family = f ? f->name : "";
                m.age = c.body.ageDays / kDaysPerYear;
                m.chief = id == st.chief;
                for (const auto& r : cs.rights)
                    if (c.status.rights.count(r)) m.rights.push_back(r);
                cs.members.push_back(std::move(m));
            }
            std::sort(cs.members.begin(), cs.members.end(), [](const ColonyMember& a, const ColonyMember& b) {
                if (a.chief != b.chief) return a.chief;
                if (a.family != b.family) return a.family < b.family;
                return a.age > b.age;
            });
        }
        std::lock_guard<std::mutex> lock(snapMutex_);
        snap_ = std::move(s);
    }

    // What the selected person carries and wears, for the inventory (sim/gear.hpp).
    void gearSnap(const World& w, const Character& c, GearSnap& g) {
        const GearRules* R = gearRules(w.content());
        if (!R || c.isAnimal()) return;
        g.on = true;
        g.kg = loadKg(*R, c);
        g.maxKg = maxLoadKg(*R, c);
        g.comfortKg = R->comfortKg;
        g.tireMult = loadFatigueMult(*R, c);
        const GearLayout L = arrange(*R, c);
        for (const auto& gr : L.grids)
            g.grids.push_back({gr.id, gr.id == "hands" ? text("hud.gear.hands") : name("item", gr.id), gr.w, gr.h});
        for (const auto& s : L.spots) {
            GearSnap::Spot o;
            o.item = s.item;
            o.name = name("item", s.item);
            o.grid = s.grid;
            o.count = s.count;
            o.x = s.x, o.y = s.y, o.w = s.w, o.h = s.h;
            o.turned = s.turned;
            o.kg = R->item(s.item).kg * s.count;
            o.wearable = !R->item(s.item).slot.empty();
            g.spots.push_back(std::move(o));
        }
        for (const auto& s : R->slots) {
            GearSnap::Slot o;
            o.id = s.id;
            o.name = text("hud.gear.slot." + s.id);
            o.bulk = s.bulk;
            for (const auto& l : s.layers) {
                GearSnap::Layer y;
                y.id = l;
                y.name = text("hud.gear.layer." + l);
                if (const auto it = c.worn.find(s.id + ":" + l); it != c.worn.end()) {
                    y.item = it->second;
                    y.itemName = name("item", it->second);
                    y.bulk = R->item(it->second).bulk;
                    o.used += y.bulk;
                }
                o.layers.push_back(std::move(y));
            }
            g.slots.push_back(std::move(o));
        }
        for (const auto& [k, n] : L.overflow) g.overflow.push_back({name("item", k), n});
    }

    // ---- things, acts, feedback (sim thread) ------------------------------------
    // Things one can work on, near the camera and near the player's person.
    void thingsSnap(const World& w, Snapshot& s) {
        static const std::set<std::string> kSkip = {"house", "storage", "hearth", "field", "garden", "shrine",
                                                    "village_fire", "bed", "build_site", "roof_piece", "water"};
        const double vx = viewX_, vy = viewY_;
        Vec2 me{1e18, 1e18};
        for (const auto& [id, c] : w.characters())
            if (c.controller.isPlayer()) me = c.pos;
        const double R2 = 140.0 * 140.0;
        for (const auto& [id, o] : w.objects()) {
            if (kSkip.count(o.type)) continue;
            const double d1 = (o.pos.x - vx) * (o.pos.x - vx) + (o.pos.y - vy) * (o.pos.y - vy);
            const double d2 = (o.pos.x - me.x) * (o.pos.x - me.x) + (o.pos.y - me.y) * (o.pos.y - me.y);
            if (d1 > R2 && d2 > R2) continue;
            if (s.things.size() >= 700) break;
            ThingSnap t;
            t.id = id;
            t.type = o.type;
            t.name = name("object", o.type);
            t.x = o.pos.x;
            t.y = o.pos.y;
            for (const auto& [k, v] : o.stock)
                if (v > 0) t.stock.push_back({name("item", k), v});
            t.busy = o.reservedFor(kNone, w.now()) && o.reservedBy != kNone;
            s.things.push_back(std::move(t));
        }
    }

    // A precondition and an effect in the player's words (content/loc hud.req.* / hud.gain.*).
    std::string opText(Condition::Op op) const {
        switch (op) {
        case Condition::Op::Ge: return ">=";
        case Condition::Op::Le: return "<=";
        case Condition::Op::Gt: return ">";
        case Condition::Op::Lt: return "<";
        case Condition::Op::Eq: return "=";
        case Condition::Op::Ne: return "!=";
        default: return "";
        }
    }
    static std::string num(double v) {
        char b[32];
        if (std::fabs(v - std::round(v)) < 1e-6) std::snprintf(b, sizeof b, "%d", int(std::round(v)));
        else std::snprintf(b, sizeof b, "%.2g", v);
        return b;
    }
    std::string describeCondition(const Condition& k) const {
        const std::string who = k.onTarget ? text("hud.req.target") + " " : "";
        const bool counted = k.op == Condition::Op::Ge || k.op == Condition::Op::Gt;
        std::string out;
        if (k.subject == "has") {
            out = text("hud.req.has", {{"item", name("item", k.key)}});
            if (counted && k.value > 1) out += " x" + num(k.value);
        } else if (k.subject == "knows") {
            out = text("hud.req.knows", {{"skill", name("skill", k.key)}});
        } else if (k.subject == "skill") {
            out = text("hud.req.skill", {{"skill", name("skill", k.key)}, {"op", opText(k.op)}, {"v", num(k.value * 100)}});
        } else if (k.subject == "target_stock") {
            out = text("hud.req.stock", {{"item", name("item", k.key)}, {"op", opText(k.op)}, {"v", num(k.value)}});
        } else if (k.subject == "age") {
            out = text("hud.req.age", {{"op", opText(k.op)}, {"v", num(k.value)}});
        } else if (k.subject == "need") {
            out = text("hud.req.need", {{"need", name("need", k.key)}, {"op", opText(k.op)}, {"v", num(k.value * 100)}});
        } else if (k.subject == "home") {
            out = text("hud.req.home", {{"what", name("item", k.key)}});
        } else if (k.subject == "rel") {
            out = text("hud.req.rel", {{"axis", text("hud.axis." + k.key)}, {"op", opText(k.op)}, {"v", num(k.value)}});
        } else {
            out = k.text;
        }
        if (k.negate) out = text("hud.req.not", {{"what", out}});
        return who + out;
    }
    // canPerform's reason code (ai_eval.cpp) in the player's words.
    std::string whyText(const std::string& why, const InteractionDef& d, const WorldObject* obj,
                        const Character* tc) const {
        if (why.empty()) return "";
        const std::size_t colon = why.find(':');
        const std::string head = why.substr(0, colon), rest = colon == std::string::npos ? "" : why.substr(colon + 1);
        const std::string what = obj ? name("object", obj->type) : tc ? tc->name : name("object", rest);
        if (head == "pre") {
            for (const Condition& k : d.pre)
                if (k.text == rest) return conditionWhy(k, obj);
            return "";
        }
        if (head == "reserved") return text("hud.why.reserved", {{"what", what}});
        if (head == "season") return text("hud.why.season", {{"what", what}});
        if (head == "depleted") return text("hud.why.depleted", {{"what", what}});
        if (head == "out_of_range") return text("hud.why.far", {{"what", what}});
        if (head == "gear") return text(rest == "too_heavy" ? "hud.why.too_heavy" : "hud.why.no_room");
        if (head == "dead") return text("hud.why.dead");
        if (head == "no_person") return text("hud.why.no_person");
        if (head == "no_target" || head == "wrong_target") return text("hud.why.no_target", {{"what", name("object", rest)}});
        return "";
    }
    // A failed precondition, the hidden ones (owner, time, herd) included.
    std::string conditionWhy(const Condition& k, const WorldObject* obj) const {
        if (k.subject == "target_owner") {
            if (k.key == "family" || k.key == "self") return text("hud.why.not_ours");
            if (k.key == "other") return text("hud.why.ours");
            return text("hud.why.owner");
        }
        if (k.subject == "time") return text(k.negate ? "hud.why.not_at_night" : k.key == "night" ? "hud.why.night_only" : "hud.why.time");
        if (k.subject == "herd") return text("hud.why.herd");
        if (k.subject == "charity") return text("hud.why.charity");
        if (k.subject == "target_stock") {
            const std::string what = obj ? name("object", obj->type) : "";
            if (k.op == Condition::Op::Ge || k.op == Condition::Op::Gt)
                return text("hud.why.stock_low", {{"what", what}, {"item", name("item", k.key)}, {"v", num(k.value)}});
            return text("hud.why.stock_full", {{"what", what}, {"item", name("item", k.key)}});
        }
        return describeCondition(k);
    }
    std::string describeEffect(const Effect& e) const {
        const std::string sign = e.op == '-' ? "-" : "+";
        if (e.subject == "has") return sign + num(e.value) + " " + name("item", e.key);
        if (e.subject == "skill") return text(e.op == '-' ? "hud.gain.skill_down" : "hud.gain.skill", {{"skill", name("skill", e.key)}});
        if (e.subject == "need") return text(e.op == '-' ? "hud.gain.need_down" : "hud.gain.need", {{"need", name("need", e.key)}});
        if (e.subject == "body" && e.key == "fatigue") return text(e.op == '-' ? "hud.gain.rest" : "hud.gain.tired");
        if (e.subject == "body" && e.key == "health") return text(e.op == '-' ? "hud.gain.hurt" : "hud.gain.heal");
        if (e.subject == "moodlet") return text("hud.gain.moodlet", {{"what", name("moodlet", e.key)}});
        if (e.subject == "rel" || e.subject == "rel_back")
            return text(e.signedDelta() >= 0 ? "hud.gain.rel_up" : "hud.gain.rel_down", {{"axis", text("hud.axis." + e.key)}});
        if (e.subject == "reputation") return text("hud.gain.reputation", {{"what", name("reputation", e.key)}, {"sign", sign}});
        if (e.subject == "spawn") return text("hud.gain.spawn", {{"what", name("object", e.key)}});
        return "";
    }

    // The acts of the target the act ring / the wheel is open on, checked for
    // the player's person (sim/act_info.hpp).
    void actsSnap(const World& w, Snapshot& s) {
        const EntityId tid = actsWanted_;
        s.actsFor = tid;
        if (tid == kNone) return;
        const Character* me = nullptr;
        for (const auto& [id, c] : w.characters())
            if (c.controller.isPlayer()) me = &c;
        if (!me) return;
        const WorldObject* obj = w.object(tid);
        const Character* tc = obj ? nullptr : w.character(tid);
        if (!obj && !tc) return;
        const std::vector<std::string>* ids = nullptr;
        static const std::vector<std::string> kNone_;
        if (obj) {
            auto it = objActs_.find(obj->type);
            if (it == objActs_.end()) it = objActs_.emplace(obj->type, actsOnObject(w.content(), obj->type)).first;
            ids = &it->second;
        } else if (tc->isAnimal()) {
            const auto it = hud_.beastActs.find(tc->species);
            ids = it == hud_.beastActs.end() ? &kNone_ : &it->second;
        } else {
            ids = &wheelActs_;
        }
        for (const auto& id : *ids) {
            const InteractionDef* d = w.content().interaction(id);
            if (!d) continue;
            const ActInfo a = actInfo(w, *me, *d, obj, tc);
            ActInfoSnap o;
            o.id = id;
            o.name = name("interaction", id);
            o.can = a.can;
            for (const auto& r : a.pre) {
                o.needs.push_back({describeCondition(r.cond), r.ok});
                if (!r.ok && o.why.empty()) o.why = o.needs.back().first;
            }
            // the reason the sim gives first (canPerform), in words; the shown
            // need that fails otherwise; never a bare "cannot now"
            if (!a.can) {
                if (std::string t = whyText(a.why, *d, obj, tc); !t.empty()) o.why = t;
                if (o.why.empty()) o.why = text("hud.why.unknown", {{"code", a.why}});
            }
            for (const auto& e : a.gains)
                if (std::string g = describeEffect(e); !g.empty()) o.gains.push_back(g);
            for (const auto& e : a.onTarget)
                if (std::string g = describeEffect(e); !g.empty()) o.toTarget.push_back(g);
            // AL-32: another household's: allowed, but named for what it is
            for (const auto& t : a.legal) o.needs.push_back({text("act.outcome.crime", {{"tag", name("legal", t)}}), false});
            o.minutes = int(a.duration) * int(24 * 60 / kTicksPerDay);
            s.acts.push_back(std::move(o));
        }
    }

    // What came of the player's person's doing since the last snapshot: things
    // gained or spent, acts done or broken off, refusals.
    void feedbackSnap(const World& w, Snapshot& s) {
        const Character* me = nullptr;
        for (const auto& [id, c] : w.characters())
            if (c.controller.isPlayer()) me = &c;
        if (!me) {
            fbFor_ = kNone;
            s.feedback = fbTail_;
            return;
        }
        auto push = [&](std::string t, int kind) {
            if (t.empty()) return;
            journal(t, kind);
            fbTail_.push_back({++fbSeq_, me->id, std::move(t), kind});
            if (fbTail_.size() > 12) fbTail_.erase(fbTail_.begin());
        };
        if (fbFor_ != me->id) {   // a new person: start from what they have now
            fbFor_ = me->id;
            fbInv_ = me->inventory;
            fbTick_ = me->ai.trace.empty() ? w.now() : me->ai.trace.back().tick;
            fbAtTick_ = 0;
            for (const auto& e : me->ai.trace) fbAtTick_ += e.tick == fbTick_;
        } else {
            std::set<std::string> keys;
            for (const auto& [k, v] : fbInv_) keys.insert(k);
            for (const auto& [k, v] : me->inventory) keys.insert(k);
            for (const auto& k : keys) {
                const int was = fbInv_.count(k) ? fbInv_.at(k) : 0, now = me->inventory.count(k) ? me->inventory.at(k) : 0;
                if (now > was) push("+" + std::to_string(now - was) + " " + name("item", k), 1);
                else if (now < was) push("-" + std::to_string(was - now) + " " + name("item", k), 0);
            }
            fbInv_ = me->inventory;
            int at = 0;
            Tick last = fbTick_;
            int lastN = 0;
            for (const auto& e : me->ai.trace) {
                if (e.tick < fbTick_) continue;
                if (e.tick == fbTick_ && ++at <= fbAtTick_) continue;
                // (some traces carry the plan's step number as "action", not an act)
                const auto ai = e.params.find("action");
                const std::string act = ai != e.params.end() && ai->is_string() ? ai->get<std::string>() : std::string();
                if (e.code == "fsm.done" && act != "goto" && act != "wait")
                    push(text("hud.fb.done", {{"act", name("interaction", act)}}), 1);
                else if (e.code == "fsm.abort" && act != "goto" && act != "wait" && !act.empty())
                    push(text("hud.fb.abort", {{"act", name("interaction", act)}}), 2);
                else if (e.code == "control.order_refused" || e.code == "goap.fail" || e.code == "htn.no_method" ||
                         e.code == "dialog.refused")
                    push(traceText(*loc_, opts_.lang, w, *me, e), 2);
                else if (e.code == "dialog.accepted")
                    push(traceText(*loc_, opts_.lang, w, *me, e), 1);
                else if (e.code == "act.outcome") { // AL-36: what came of it, never a bare "ok"
                    if (!e.params.value("success", true)) push(text("act.outcome.failed"), 2);
                    if (e.params.contains("tag")) {
                        push(text("act.outcome.crime", {{"tag", name("legal", e.params["tag"].get<std::string>())}}), 2);
                        const int n = e.params.value("witnesses", 0);
                        push(n > 0 ? text("act.outcome.seen", {{"n", n}}) : text("act.outcome.unseen"), n > 0 ? 2 : 0);
                    }
                }
            }
            if (!me->ai.trace.empty()) {
                last = me->ai.trace.back().tick;
                for (const auto& e : me->ai.trace) lastN += e.tick == last;
                fbTick_ = last;
                fbAtTick_ = lastN;
            }
        }
        s.feedback = fbTail_;
    }

    // AL-57: a row of the player's journal, stamped with the sim's clock.
    void journal(const std::string& t, int kind) {
        if (t.empty()) return;
        jrTail_.push_back({++jrSeq_, clockText(world_->now()), t, kind});
        if (jrTail_.size() > 40) jrTail_.erase(jrTail_.begin());
    }

    // AL-57 (the client side of AL-36 / AL-53): what people said since the
    // last snapshot (say.line of the sim, sim/speech_feed.hpp) - a bubble over
    // each speaker near the camera, and in the journal what the player's
    // person said and heard.
    void speechSnap(const World& w, Snapshot& s) {
        EntityId me = kNone;
        for (const auto& [id, c] : w.characters())
            if (c.controller.isPlayer()) me = id;
        const double vx = viewX_, vy = viewY_;
        for (auto& e : speechFeed_.poll(w, *loc_, opts_.lang)) {
            const bool mine = me != kNone && (e.speaker == me || e.listener == me);
            if (mine) journal(e.full, 3);
            const Character* c = w.character(e.speaker);
            const double dx = c ? c->pos.x - vx : 1e9, dy = c ? c->pos.y - vy : 1e9;
            if (!mine && dx * dx + dy * dy > 120.0 * 120.0) continue;
            speechTail_.push_back({++speechSeq_, e.speaker, e.listener, std::move(e.line)});
            if (speechTail_.size() > 16) speechTail_.erase(speechTail_.begin());
        }
        // and what the people near the camera mutter to themselves (barks:
        // shown, never written to the world), paced by the clock on the wall
        if (!paused_) {
            const double now = std::chrono::duration<double>(Clock::now() - barkEpoch_).count();
            for (auto& e : speechFeed_.barks(w, *loc_, opts_.lang, vx, vy, 45.0, now, me)) {
                speechTail_.push_back({++speechSeq_, e.speaker, e.listener, std::move(e.line)});
                if (speechTail_.size() > 16) speechTail_.erase(speechTail_.begin());
            }
        }
        s.speech = speechTail_;
        s.journal = jrTail_;
    }

    // The sim's chance of every act of the wheel, per tone (sim/tones.hpp):
    // recomputed when the pair changes, else every few ticks (a group fight's
    // chance draws battles, not for every frame).
    void wheelChances(WheelSnap& ws) {
        Character* actor = world_->character(ws.actor);
        Character* target = world_->character(ws.target);
        if (!wheelOpen_ || !actor || !target || actor == target || !actor->body.alive || !target->body.alive) {
            if (!wheelOpen_) wheelCacheFor_[1] = kNone; // recompute when the wheel opens again
            else wheelCache_ = {};
            return;
        }
        const bool fresh = wheelCacheFor_[0] == ws.actor && wheelCacheFor_[1] == ws.target &&
                           world_->now() - wheelCacheTick_ < 6;
        if (!fresh) {
            WheelSnap c;
            const sim::ToneRules* tr = sim::toneRules(world_->content());
            if (tr) c.tones = tr->order;
            if (c.tones.empty()) c.tones = {""};
            if (wheelActs_.empty()) {
                const Json spec = sim::loadJsonFile((fs::path(opts_.root) / "content/ui/wheel.json").string());
                const Json spheres = spec.value("spheres", Json::array());
                for (const auto& sp : spheres) {
                    const Json subs = sp.value("subs", Json::array());
                    for (const auto& sub : subs)
                        for (const auto& id : sub.value("interactions", Json::array())) wheelActs_.push_back(id.get<std::string>());
                }
            }
            for (const auto& id : wheelActs_) {
                const sim::InteractionDef* d = world_->content().interaction(id);
                if (!d) continue;
                std::vector<float> per;
                std::string kind;
                for (const auto& tone : c.tones) {
                    const sim::ActChance ac = sim::actChance(*world_, *actor, *d, target, tone);
                    per.push_back(float(ac.p));
                    kind = ac.kind;
                    if (ac.kind == "win" || ac.kind == "certain") break; // the tone does not move these
                }
                while (per.size() < c.tones.size()) per.push_back(per.back());
                c.chance[id] = std::move(per);
                c.kind[id] = kind;
            }
            wheelCache_ = std::move(c);
            wheelCacheFor_[0] = ws.actor;
            wheelCacheFor_[1] = ws.target;
            wheelCacheTick_ = world_->now();
        }
        ws.tones = wheelCache_.tones;
        ws.chance = wheelCache_.chance;
        ws.kind = wheelCache_.kind;
    }

    // ---- building (sim/building.hpp) ------------------------------------------
    // The ground under the mouse: a ray from the eye through the pixel, marched
    // against the terrain. World metres.
    bool groundRay(float mx, float my, double& hx, double& hy) const {
        double m[16], inv[16];
        for (int i = 0; i < 16; ++i) m[i] = vp_[i];
        if (!invert4(m, inv)) return false;
        const double nx = double(mx) / viewW_ * 2.0 - 1.0, ny = 1.0 - double(my) / viewH_ * 2.0;
        double q[4];
        for (int r = 0; r < 4; ++r) q[r] = inv[r * 4 + 0] * nx + inv[r * 4 + 1] * ny + inv[r * 4 + 2] * 0.5 + inv[r * 4 + 3];
        if (std::fabs(q[3]) < 1e-12) return false;
        double d[3] = {q[0] / q[3], q[1] / q[3], q[2] / q[3]};
        const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len < 1e-9) return false;
        for (double& v : d) v /= len;
        auto above = [&](double t) { return t * d[2] - (groundAt(eye_[0] + t * d[0], eye_[1] + t * d[1]) - eye_[2]); };
        double t0 = 0.0, t = 0.5;
        while (t < 4000.0 && above(t) > 0) {
            t0 = t;
            t += std::max(0.25, t * 0.02);
        }
        if (t >= 4000.0) return false;
        for (int k = 0; k < 24; ++k) {
            const double mid = 0.5 * (t0 + t);
            (above(mid) > 0 ? t0 : t) = mid;
        }
        hx = eye_[0] + t * d[0];
        hy = eye_[1] + t * d[1];
        return true;
    }
    static bool invert4(const double* a, double* out) {
        double m[4][8];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 8; ++c) m[r][c] = c < 4 ? a[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
        for (int c = 0; c < 4; ++c) {
            int piv = c;
            for (int r = c + 1; r < 4; ++r)
                if (std::fabs(m[r][c]) > std::fabs(m[piv][c])) piv = r;
            if (std::fabs(m[piv][c]) < 1e-15) return false;
            for (int k = 0; k < 8; ++k) std::swap(m[c][k], m[piv][k]);
            const double f = m[c][c];
            for (int k = 0; k < 8; ++k) m[c][k] /= f;
            for (int r = 0; r < 4; ++r) {
                if (r == c) continue;
                const double g = m[r][c];
                for (int k = 0; k < 8; ++k) m[r][k] -= g * m[c][k];
            }
        }
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) out[r * 4 + c] = m[r][c + 4];
        return true;
    }

    // A piece of the palette at a sim point: the slot nearest the point, the
    // lowest storey it fits from the bump up, the structure's ground floor.
    BuildPiece pieceAt(const BuildPieceDef& d, double sx, double sy, int turn, int bump, std::string& why) const {
        const BuildCatalog& cat = content_->build;
        const double c = cat.cellM;
        BuildPiece p;
        p.kind = d.id;
        p.x = int(std::floor(sx / c));
        p.y = int(std::floor(sy / c));
        const double fx = sx - p.x * c, fy = sy - p.y * c;
        if (d.slot == "edge") {
            const double ds[4] = {fy, c - fx, c - fy, fx};   // -y, +x, +y, -x
            p.rot = int(std::min_element(ds, ds + 4) - ds);
            if (turn & 1) {   // the same edge, the other face outwards
                const int dx = p.rot == 1 ? 1 : p.rot == 3 ? -1 : 0, dy = p.rot == 0 ? -1 : p.rot == 2 ? 1 : 0;
                p.x += dx, p.y += dy;
                p.rot = (p.rot + 2) % 4;
            }
            if (d.mirror && (turn & 2)) p.rot += 4;
        } else if (d.slot == "corner") {
            p.rot = fx < c / 2 ? (fy < c / 2 ? 0 : 3) : (fy < c / 2 ? 1 : 2);
        } else if (d.slot == "roof") {
            p.rot = turn % 4;
        }
        const int z = autoStorey(cat, shown_.pieces, p, bump);
        p.z = z < 0 ? bump : z;
        if (d.category == "foundation") p.z = 0;
        if (d.category == "floor") p.z = std::max(1, p.z);
        why = checkPlacement(cat, shown_.pieces, p);
        // the ground under a cell: the stone plinth goes 1.4 m down
        const double wx = originX_ + (p.x + 0.5) * c, wy = originY_ + (p.y + 0.5) * c;
        double gmin = 1e9, gmax = -1e9;
        for (int k = 0; k < 5; ++k) {
            const double ox = k == 4 ? 0 : (k & 1 ? 0.95 : -0.95), oy = k == 4 ? 0 : (k & 2 ? 0.95 : -0.95);
            const double g = groundAt(wx + ox, wy + oy);
            gmin = std::min(gmin, g), gmax = std::max(gmax, g);
        }
        double h = 0;
        if (joinHeight(cat, shown_.pieces, p, h)) p.h = h;
        else p.h = gmax + 0.05;
        if (d.category == "foundation" && why.empty() && (p.h - gmin > 1.3 || gmax > p.h + 0.6)) why = "ground_steep";
        return p;
    }

    // ---- AL-9: building as in the Sims ------------------------------------
    // A tool lays a batch of pieces for a drag from a to b (sim points): a
    // room (footings, walls round it, corner posts), a run of walls along a
    // grid line, a floor or footings over a rectangle, a roof with its gables
    // over one, a door or a window put in place of a wall, removal over a
    // rectangle; a click is a drag of one cell. Each piece is checked against
    // the plan with the batch's earlier pieces in it, so a room's walls stand
    // on its own new footings; on release the sim gets them in that order.
    void addToBatch(PieceMap& plan, EntityId& fake, const std::string& kind, int x, int y, int rot, int z, bool exactZ) {
        const BuildCatalog& cat = content_->build;
        const BuildPieceDef* d = cat.find(kind);
        if (!d) return;
        BuildPiece p;
        p.kind = kind;
        p.x = x;
        p.y = y;
        p.rot = rot;
        p.family = shown_.builderFamily;
        if (d->category == "foundation") p.z = 0;
        else if (exactZ) p.z = std::max(d->category == "floor" ? 1 : 0, z);
        else {
            const int az = autoStorey(cat, plan, p, std::max(d->category == "floor" ? 1 : 0, z));
            p.z = az < 0 ? z : az;
        }
        std::string why = checkPlacement(cat, plan, p);
        const double c = cat.cellM;
        const double wx = originX_ + (p.x + 0.5) * c, wy = originY_ + (p.y + 0.5) * c;
        double gmin = 1e9, gmax = -1e9;
        for (int k = 0; k < 5; ++k) {
            const double ox = k == 4 ? 0 : (k & 1 ? 0.95 : -0.95), oy = k == 4 ? 0 : (k & 2 ? 0.95 : -0.95);
            const double g = groundAt(wx + ox, wy + oy);
            gmin = std::min(gmin, g), gmax = std::max(gmax, g);
        }
        double h = 0;
        if (joinHeight(cat, plan, p, h)) p.h = h;
        else p.h = gmax + 0.05;
        // the ground is levelled under footings (layPads), so only a real cliff stops one
        if (d->category == "foundation" && why.empty() && (p.h - gmin > 3.0 || gmax > p.h + 0.6)) why = "ground_steep";
        if (why.empty()) {
            p.id = ++fake;
            plan[p.id] = p;
        }
        laid_.push_back(p);
        batchWhy_.push_back(why);
    }

    // The way a gable leans at an end of a roof (see planHouse): its mirrored
    // twin on one half of the end.
    static int gableRot(int endRot, bool lowerHalf) {
        switch (endRot) {
        case 3: return lowerHalf ? 3 + 4 : 3;   // west end, ridge along x
        case 1: return lowerHalf ? 1 : 1 + 4;   // east end
        case 0: return lowerHalf ? 0 + 4 : 0;   // south end, ridge along y (lower half = +x)
        default: return lowerHalf ? 2 : 2 + 4;  // north end
        }
    }

    void buildBatch(double ax, double ay, double bx, double by) {
        const BuildCatalog& cat = content_->build;
        laid_.clear();
        batchWhy_.clear();
        batchRemove_.clear();
        const std::string mode = hud_.buildMode;
        const BuildPieceDef& d = cat.pieces[std::size_t(buildPick_)];
        const double c = cat.cellM;
        PieceMap plan = shown_.pieces;
        EntityId fake = 0xF0000000u;
        const int cx0 = int(std::floor(std::min(ax, bx) / c)), cx1 = int(std::floor(std::max(ax, bx) / c));
        const int cy0 = int(std::floor(std::min(ay, by) / c)), cy1 = int(std::floor(std::max(ay, by) / c));
        const bool drag = cx0 != cx1 || cy0 != cy1;
        auto single = [&](const BuildPieceDef& def) {
            std::string why;
            BuildPiece p = pieceAt(def, bx, by, buildRot_, buildBump_, why);
            addToBatch(plan, fake, def.id, p.x, p.y, p.rot, p.z, true);
        };
        if (mode == "remove") {
            for (const auto& [id, p] : shown_.pieces) {
                const BuildPieceDef* pd = cat.find(p.kind);
                if (!pd || p.family != shown_.builderFamily) continue;
                const Vec2 pc = pieceCentre(cat, *pd, p);
                if (pc.x >= cx0 * c - 0.3 && pc.x <= (cx1 + 1) * c + 0.3 && pc.y >= cy0 * c - 0.3 && pc.y <= (cy1 + 1) * c + 0.3)
                    batchRemove_.push_back(id);
            }
            return;
        }
        if (mode == "door" || mode == "window") {
            // in place of the wall on that edge (any wall of the household)
            std::string why;
            const BuildPiece p = pieceAt(d, bx, by, buildRot_, buildBump_, why);
            BuildPiece probe = p;
            for (int z = 0; z <= 4; ++z) {
                probe.z = z;
                for (const auto& [id, o] : shown_.pieces) {
                    const BuildPieceDef* od = cat.find(o.kind);
                    if (!od || od->category != "wall" || o.family != shown_.builderFamily || o.z != z) continue;
                    if (checkPlacement(cat, PieceMap{{id, o}}, probe) == "occupied") {
                        batchRemove_.push_back(id);
                        plan.erase(id);
                        addToBatch(plan, fake, d.id, o.x, o.y, o.rot, o.z, true);
                        laid_.back().h = o.h;
                        return;
                    }
                }
            }
            addToBatch(plan, fake, d.id, p.x, p.y, p.rot, p.z, true);
            return;
        }
        if (mode == "gable" || mode == "pillar" || (mode == "wall" && !drag)) {
            single(d);
            return;
        }
        if (mode == "wall") {
            // a run along the grid line the drag follows most
            const int ia = int(std::lround(ax / c)), ja = int(std::lround(ay / c));
            const int ib = int(std::lround(bx / c)), jb = int(std::lround(by / c));
            const bool flip = buildRot_ & 1;
            if (std::abs(ib - ia) >= std::abs(jb - ja)) {
                for (int i = std::min(ia, ib); i < std::max(ia, ib); ++i)
                    addToBatch(plan, fake, d.id, i, flip ? ja - 1 : ja, flip ? 2 : 0, buildBump_, false);
            } else {
                for (int j = std::min(ja, jb); j < std::max(ja, jb); ++j)
                    addToBatch(plan, fake, d.id, flip ? ia - 1 : ia, j, flip ? 1 : 3, buildBump_, false);
            }
            if (laid_.empty()) single(d);
            return;
        }
        if (mode == "floor" || mode == "foundation") {
            for (int y = cy0; y <= cy1; ++y)
                for (int x = cx0; x <= cx1; ++x) addToBatch(plan, fake, d.id, x, y, 0, std::max(1, buildBump_), false);
            return;
        }
        if (mode == "room") {
            // footings under it all, walls round it facing out, posts at the corners
            if (buildBump_ == 0)
                for (int y = cy0; y <= cy1; ++y)
                    for (int x = cx0; x <= cx1; ++x) addToBatch(plan, fake, "foundation_stone", x, y, 0, 0, true);
            else
                for (int y = cy0; y <= cy1; ++y)
                    for (int x = cx0; x <= cx1; ++x) addToBatch(plan, fake, "floor_plank", x, y, 0, buildBump_, true);
            const int z = buildBump_;
            for (int x = cx0; x <= cx1; ++x) {
                addToBatch(plan, fake, d.id, x, cy0, 0, z, true);
                addToBatch(plan, fake, d.id, x, cy1, 2, z, true);
            }
            for (int y = cy0; y <= cy1; ++y) {
                addToBatch(plan, fake, d.id, cx0, y, 3, z, true);
                addToBatch(plan, fake, d.id, cx1, y, 1, z, true);
            }
            addToBatch(plan, fake, "pillar_log", cx0, cy0, 0, z, true);
            addToBatch(plan, fake, "pillar_log", cx1, cy0, 1, z, true);
            addToBatch(plan, fake, "pillar_log", cx1, cy1, 2, z, true);
            addToBatch(plan, fake, "pillar_log", cx0, cy1, 3, z, true);
            return;
        }
        if (mode == "roof") {
            // the ridge along the longer side; each row a storey higher toward
            // it; at the ends gables under the slope and walls below them
            const int zb = std::max(1, buildBump_ + 1);
            const int W = cx1 - cx0 + 1, D = cy1 - cy0 + 1;
            const std::string gable = lastGable_.empty() ? "gable_daub" : lastGable_;
            const std::string wall = lastWall_.empty() ? "wall_daub" : lastWall_;
            const bool alongX = W >= D;
            const int rows = alongX ? D : W, len = alongX ? W : D;
            for (int r = 0; r < rows; ++r) {
                const int k = std::min(r, rows - 1 - r);
                const bool lower = r < rows / 2 || (rows % 2 && r == rows / 2);
                const int rot = alongX ? (lower ? 0 : 2) : (lower ? 3 : 1);
                for (int t = 0; t < len; ++t) {
                    const int x = alongX ? cx0 + t : cx0 + r, y = alongX ? cy0 + r : cy0 + t;
                    addToBatch(plan, fake, d.id, x, y, rot, zb + k, true);
                }
                for (int end = 0; end < 2; ++end) {
                    const int x = alongX ? (end ? cx1 : cx0) : cx0 + r, y = alongX ? cy0 + r : (end ? cy1 : cy0);
                    const int endRot = alongX ? (end ? 1 : 3) : (end ? 2 : 0);
                    const bool half = alongX ? lower : !lower;   // ridge along y: the +x half is the "lower" one
                    for (int s = 0; s < k; ++s) addToBatch(plan, fake, wall, x, y, endRot, zb + s, true);
                    addToBatch(plan, fake, gable, x, y, gableRot(endRot, half), zb + k, true);
                }
            }
            return;
        }
        single(d);
    }

    void updateBuild() {
        const BuildCatalog& cat = content_->build;
        hasPreview_ = false;
        if (cat.pieces.empty()) return;
        // the buttons of the bar do what the keys do
        buildRot_ = (buildRot_ + hud_.buildTurns) % 4;
        buildBump_ = std::max(0, buildBump_ + hud_.buildStoreyUp - hud_.buildStoreyDown);
        hud_.buildTurns = hud_.buildStoreyUp = hud_.buildStoreyDown = 0;
        buildPick_ = std::clamp(hud_.buildPick, 0, int(cat.pieces.size()) - 1);
        const BuildPieceDef& d = cat.pieces[std::size_t(buildPick_)];
        if (d.category == "gable") lastGable_ = d.id;
        if (d.category == "wall" && d.id.rfind("wall_door", 0) != 0 && d.id.rfind("wall_window", 0) != 0) lastWall_ = d.id;
        double hx = 0, hy = 0;
        const bool hit = !overUi_ && groundRay(input_.mouseX, input_.mouseY, hx, hy);
        if (dragStart_ && hit) {
            dragOn_ = true;
            dragA_[0] = hx - originX_;
            dragA_[1] = hy - originY_;
        }
        dragStart_ = false;
        if (hit) {
            const double bx = hx - originX_, by = hy - originY_;
            buildBatch(dragOn_ ? dragA_[0] : bx, dragOn_ ? dragA_[1] : by, bx, by);
            hasPreview_ = true;
        } else if (!dragOn_) {
            laid_.clear();
            batchWhy_.clear();
            batchRemove_.clear();
        }
        int ok = 0, bad = 0, z = 0;
        std::string firstWhy;
        for (std::size_t i = 0; i < laid_.size(); ++i) {
            if (batchWhy_[i].empty()) ++ok, z = std::max(z, laid_[i].z);
            else if (++bad == 1) firstWhy = batchWhy_[i];
        }
        // AL-26: a plan past the household's piece limit is not laid in part:
        // the whole drag is refused, and the preview says so before release.
        {
            int own = 0;
            for (const auto& [id, p] : shown_.pieces) own += p.family == shown_.builderFamily;
            if (ok > 0 && own + ok > content_->build.maxPerFamily) {
                for (auto& wy : batchWhy_)
                    if (wy.empty()) wy = "too_many";
                bad = int(laid_.size());
                ok = 0;
                firstWhy = "too_many";
            }
        }
        if (shown_.builder == kNone) firstWhy = "no_builder", bad = std::max(bad, 1), ok = 0;
        hud_.buildOk = bad == 0;
        if (!hit && !dragOn_) hud_.buildStatus = "";
        else if (hud_.buildMode == "remove")
            hud_.buildStatus = text("hud.build.remove_n", {{"n", int(batchRemove_.size())}});
        else if (bad == 0 && ok == 1) hud_.buildStatus = text("hud.build.ok", {{"storey", z + 1}});
        else if (bad == 0) hud_.buildStatus = text("hud.build.batch_ok", {{"n", ok}});
        else
            hud_.buildStatus = text("hud.build.batch_bad", {{"n", ok}, {"bad", bad}}) + ": " +
                               text("hud.build.why." + firstWhy, {{"max", content_->build.maxPerFamily}});
        // what the family's plan still lacks
        std::map<std::string, int> lack;
        int planned = 0, built = 0;
        for (const auto& [id, p] : shown_.pieces) {
            if (p.family != shown_.builderFamily) continue;
            if (p.built) {
                ++built;
                continue;
            }
            ++planned;
            auto it = shown_.missing.find(id);
            if (it != shown_.missing.end())
                for (const auto& [k, v] : it->second) lack[k] += v;
        }
        std::string need;
        for (const auto& [k, v] : lack) need += (need.empty() ? "" : ", ") + name("item", k) + " " + std::to_string(v);
        hud_.buildPlan = text("hud.build.plan", {{"planned", planned}, {"built", built}}) +
                         (need.empty() ? "" : "  \xC2\xB7  " + text("hud.build.lack", {{"list", need}}));
        // the release lays the batch (the whole of it that can go: what cannot is left out)
        const bool release = dragOn_ && (!input_.down || input_.released);
        if (release || placeWanted_) {
            dragOn_ = false;
            placeWanted_ = false;
            commitBatch();
        }
        if (removeWanted_) {
            removeWanted_ = false;
            if (!hit) return;
            EntityId best = kNone;
            double bd = 1.6;
            for (const auto& [id, p] : shown_.pieces) {
                const BuildPieceDef* pd = cat.find(p.kind);
                if (!pd || p.family != shown_.builderFamily) continue;
                const Vec2 c = pieceCentre(cat, *pd, p);
                // the highest piece near the cursor
                const double dd = std::hypot(originX_ + c.x - hx, originY_ + c.y - hy) - p.z * 0.25;
                if (dd < bd) best = id, bd = dd;
            }
            if (best != kNone) post({"cmd", 0, "remove_piece", {{"id", shown_.builder}, {"piece", best}}});
        }
    }

    void commitBatch() {
        if (shown_.builder == kNone) return;
        for (EntityId id : batchRemove_) post({"cmd", 0, "remove_piece", {{"id", shown_.builder}, {"piece", id}}});
        for (std::size_t i = 0; i < laid_.size(); ++i) {
            if (!batchWhy_[i].empty()) continue;
            const BuildPiece& p = laid_[i];
            post({"cmd", 0, "place_piece",
                  {{"id", shown_.builder}, {"kind", p.kind}, {"x", p.x}, {"y", p.y}, {"z", p.z}, {"rot", p.rot}, {"h", p.h}}});
        }
        laid_.clear();
        batchWhy_.clear();
        batchRemove_.clear();
    }

    // A ready-made little house for the tour scripts: 2 x 2 cells at a sim
    // point - stone footing, log and daub walls with a door, gables and a
    // steep thatch roof, corner posts.
    void planHouse(double ox, double oy) {
        const BuildCatalog& cat = content_->build;
        const int x0 = int(std::floor(ox / cat.cellM)), y0 = int(std::floor(oy / cat.cellM));
        double gmax = -1e9;
        for (double dx = 0; dx <= 4; dx += 1)
            for (double dy = 0; dy <= 4; dy += 1)
                gmax = std::max(gmax, groundAt(originX_ + x0 * cat.cellM + dx, originY_ + y0 * cat.cellM + dy));
        const double h = gmax + 0.05;
        auto put = [&](const char* kind, int x, int y, int z, int rot) {
            post({"cmd", 0, "place_piece",
                  {{"id", shown_.builder}, {"kind", kind}, {"x", x0 + x}, {"y", y0 + y}, {"z", z}, {"rot", rot}, {"h", h}}});
        };
        for (int x = 0; x < 2; ++x)
            for (int y = 0; y < 2; ++y) put("foundation_stone", x, y, 0, 0);
        put("wall_door_log", 0, 0, 0, 0);
        put("wall_window_log", 1, 0, 0, 0);
        put("wall_log", 0, 1, 0, 2);
        put("wall_log", 1, 1, 0, 2);
        put("wall_daub", 0, 0, 0, 3);
        put("wall_window_daub", 0, 1, 0, 3);
        put("wall_daub", 1, 0, 0, 1);
        put("wall_daub", 1, 1, 0, 1);
        put("pillar_log", 0, 0, 0, 0);
        put("pillar_log", 1, 0, 0, 1);
        put("pillar_log", 1, 1, 0, 2);
        put("pillar_log", 0, 1, 0, 3);
        // the ridge runs along x between the rows: gables on the west and east ends
        put("gable_daub", 0, 0, 1, 3 + 4);
        put("gable_daub", 0, 1, 1, 3);
        put("gable_daub", 1, 0, 1, 1);
        put("gable_daub", 1, 1, 1, 1 + 4);
        for (int x = 0; x < 2; ++x) {
            put("roof_thatch", x, 0, 1, 0);
            put("roof_thatch", x, 1, 1, 2);
        }
    }

    // ---- AL-56: the top view's cutaway (game_view/cutaway.hpp) -----------------
    // A kit piece as the cutaway sees it: its kind, its box in the world, a wall's normal.
    void cutPiece(const BuildPieceDef& d, const BuildPiece& p, int mesh, double wx, double wy, bool props,
                  std::size_t index) {
        const BuildCatalog& cat = content_->build;
        float mn[3] = {0, 0, 0}, mx[3] = {0, 0, cat.storeyM > 0 ? float(cat.storeyM) : 3.0f};
        village_.bounds(mesh, mn, mx);
        cutaway::Piece c;
        c.key = std::uint64_t(p.id);
        c.family = std::uint64_t(p.family) + 1;
        c.x = wx, c.y = wy;
        const double base = p.h + p.z * cat.storeyM;
        c.z0 = base + mn[2], c.z1 = base + mx[2];
        const double half = cat.cellM * 0.5;
        if (d.category == "wall" || d.category == "gable") {
            const int r = ((p.rot % 4) + 4) % 4;
            c.kind = cutaway::Kind::Wall;
            c.nx = r == 1 ? 1 : r == 3 ? -1 : 0, c.ny = r == 0 ? -1 : r == 2 ? 1 : 0;
            c.hx = half, c.hy = 0.15;
        } else if (d.category == "pillar" || d.slot == "corner") {
            c.kind = cutaway::Kind::Pillar;
            c.hx = c.hy = 0.15;
        } else {
            c.kind = d.category == "roof" ? cutaway::Kind::Roof : cutaway::Kind::Floor;
            c.hx = c.hy = half;
        }
        cutPieces_.push_back(c);
        cutRefs_.push_back({props, index});
    }
    // What the view is about: the person led, the one selected, the mouse on
    // the ground (the first sets the storey looked at); none: where the camera looks.
    std::vector<cutaway::Focus> cutFoci() const {
        std::vector<cutaway::Focus> foci;
        auto person = [&](EntityId id) {
            const ActorSnap* a = actor(id);
            if (!a || !a->alive) return;
            double x, y;
            actorWorld(*a, tickAlpha(), x, y);
            if (avatar_.driven && a->id == avatar_.id) x = avatar_.x, y = avatar_.y;
            foci.push_back({x, y, groundAt(x, y), true});
        };
        person(shown_.controlled);
        if (selected_ != shown_.controlled) person(selected_);
        double gx, gy;
        if (!overUi_ && !hud_.inMenu() && viewW_ > 0 && groundUnder(input_.mouseX, input_.mouseY, gx, gy))
            foci.push_back({gx, gy, groundAt(gx, gy), false});
        if (foci.empty()) foci.push_back({target_[0], target_[1], groundAt(target_[0], target_[1]), false});
        return foci;
    }
    void applyCutaway() {
        if (cutPieces_.empty()) return;
        cutaway_.update(wallMode_, cutPieces_, eye_, cutFoci(), frameDt_);
        const auto& looks = cutaway_.looks();
        for (std::size_t i = 0; i < cutRefs_.size() && i < looks.size(); ++i) {
            std::vector<Placed>& v = cutRefs_[i].props ? propsPlaced_ : placed_;
            if (cutRefs_[i].index >= v.size()) continue;
            v[cutRefs_[i].index].vis = looks[i].vis;
            v[cutRefs_[i].index].keep = looks[i].keep;
        }
    }

    // The pieces as the kit bake has them: planned ones pale, the preview
    // green (or red where it cannot go).
    void placePieces(double range) {
        const BuildCatalog& cat = content_->build;
        // cut: the cutaway may take it (the build preview always shows)
        auto add = [&](const BuildPiece& p, const float tint[3], float hl, bool cut) {
            const BuildPieceDef* d = cat.find(p.kind);
            if (!d) return;
            const std::string mesh = d->mirror && p.rot >= 4 ? d->mesh + "_m" : d->mesh;
            auto it = pieceMesh_.find(mesh);
            if (it == pieceMesh_.end()) it = pieceMesh_.emplace(mesh, village_.piece(mesh)).first;
            if (it->second < 0) return;
            const Vec2 c = pieceCentre(cat, *d, p);
            const double wx = originX_ + c.x, wy = originY_ + c.y;
            const double rx = wx - eye_[0], ry = wy - eye_[1];
            if (rx * rx + ry * ry > range * range * 4) return;
            Placed pl;
            pl.piece = it->second;
            pl.rel[0] = rx;
            pl.rel[1] = ry;
            pl.rel[2] = p.h + p.z * cat.storeyM - eye_[2];
            pl.yaw = float((p.rot % 4) * kPi * 0.5);
            pl.highlight = hl;
            for (int k = 0; k < 3; ++k) pl.tint[k] = tint[k];
            placed_.push_back(pl);
            if (cut) cutPiece(*d, p, it->second, wx, wy, false, placed_.size() - 1);
        };
        static const float kBuilt[3] = {1, 1, 1}, kPlanned[3] = {0.55f, 0.62f, 0.78f}, kOk[3] = {0.7f, 1.35f, 0.7f},
                           kBad[3] = {1.5f, 0.45f, 0.4f};
        for (const auto& [id, p] : shown_.pieces) {
            const bool going = hud_.panel == "build" &&
                               std::find(batchRemove_.begin(), batchRemove_.end(), id) != batchRemove_.end();
            add(p, going ? kBad : p.built ? kBuilt : kPlanned, going ? 0.9f : p.built ? 0.0f : 0.25f, true);
        }
        if (hud_.panel == "build" && (hasPreview_ || dragOn_))
            for (std::size_t i = 0; i < laid_.size(); ++i) add(laid_[i], batchWhy_[i].empty() ? kOk : kBad, 0.9f, false);
    }

    // ---- placement --------------------------------------------------------
    double groundAt(double x, double y) const { return std::max(0.0, landAt(x, y)); }
    // The land as the player sees it: the terrain with the game's pads (building sites, fields) on it.
    double landAt(double x, double y) const {
        const float g = ground_.at(x, y, 2.0);
        return relief_ && padsOn_ ? double(relief_->overPads(x, y, g)) : double(g);
    }

    // ---- the ground under the village ------------------------------------------
    // Every settlement building stands on a level site cut or filled to the
    // lowest ground under it, its approaches eased back into the land over a
    // few metres; a field or a garden is levelled gently (the furrows are
    // drawn on it). Laid as pads on the view's fine relief (terrain_core/relief.hpp),
    // which the clipmap then stands on - the ground, the grass, the shore.
    void layPads() {
        if (!relief_ || !houseSet().loaded()) return;
        // AL-9: the household's footings count too (the ground is levelled under them)
        std::size_t footings = 0;
        for (const auto& [id, pc] : shown_.pieces)
            if (pc.z == 0 && pc.kind.rfind("foundation", 0) == 0) footings = footings * 31 + std::size_t(pc.x * 7919 + pc.y) + 1;
        const std::size_t sig = buildings_.size() * 2654435761u + 1 + footings * 40503u;
        if (sig == padSig_) return;
        padSig_ = sig;
        std::vector<terrain_core::Relief::Pad> pads;
        auto rawAt = [&](double x, double y) { return double(ground_.at(x, y, 2.0)); };
        for (const auto& b0 : buildings_) {
            BuildingSnap b;
            float yaw = 0;
            int mesh = -1;
            if (!buildingPose(b0, b, yaw, mesh)) continue;
            const bool soil = b.type == "field" || b.type == "garden";
            if (b.type == "hearth" || b.type == "village_fire") continue;
            float hx = 0, hy = 0;
            if (mesh >= 0) houseSet().halfSize(mesh, hx, hy);
            if (hx <= 0 || hy <= 0) {
                hx = b.type == "field" ? 20.0f : b.type == "garden" ? 6.0f : b.type == "house" ? 4.0f : 2.25f;
                hy = b.type == "field" ? 15.0f : b.type == "garden" ? 5.0f : b.type == "house" ? 3.0f : 2.0f;
            }
            const double wx = originX_ + b.x, wy = originY_ + b.y;
            const double c = std::cos(yaw), s = std::sin(yaw);
            // the site's height: the lowest ground under a house (as it is
            // placed), the mean under a field
            double base = 1e30, sum = 0;
            int n = 0;
            for (int j = -2; j <= 2; ++j)
                for (int i = -2; i <= 2; ++i) {
                    const double lx = hx * i / 2.0, ly = hy * j / 2.0;
                    const double g = rawAt(wx + c * lx - s * ly, wy + s * lx + c * ly);
                    base = std::min(base, g);
                    sum += g;
                    ++n;
                }
            if (soil) base = sum / n;
            if (base <= 0.4) continue;   // at the water: leave it be
            const double pad = soil ? 1.0 : 1.2, ease = soil ? 3.0 : 5.0, full = soil ? 0.75 : 1.0;
            const double reach = std::hypot(hx, hy) + pad + ease + 1.0;
            terrain_core::Relief::Pad p;
            p.texel = soil ? 1.0f : 0.5f;
            p.base = float(base);
            p.x0 = wx - reach;
            p.y0 = wy - reach;
            p.width = p.height = std::uint32_t(std::ceil(2 * reach / p.texel));
            const std::size_t cells = std::size_t(p.width) * p.height;
            p.rise.assign(cells, 0.0f);
            p.weight.assign(cells, 0.0f);
            for (std::uint32_t j = 0; j < p.height; ++j)
                for (std::uint32_t i = 0; i < p.width; ++i) {
                    const double x = p.x0 + (i + 0.5) * p.texel - wx, y = p.y0 + (j + 0.5) * p.texel - wy;
                    const double lx = c * x + s * y, ly = -s * x + c * y;
                    // outside the footprint grown by `pad`: how far
                    const double ex = std::max(0.0, std::fabs(lx) - hx - pad), ey = std::max(0.0, std::fabs(ly) - hy - pad);
                    const double d = std::hypot(ex, ey);
                    const double t = std::clamp(1.0 - d / ease, 0.0, 1.0);
                    p.weight[std::size_t(j) * p.width + i] = float(full * t * t * (3 - 2 * t));
                }
            pads.push_back(std::move(p));
        }
        // AL-9: a level site under every structure the household builds: its
        // footings grouped by touching cells, the ground brought to just under
        // the floor across them and eased back into the land round them
        {
            const double cm = content_->build.cellM;
            std::map<std::pair<int, int>, double> cells;
            for (const auto& [id, pc] : shown_.pieces)
                if (pc.z == 0 && pc.kind.rfind("foundation", 0) == 0) cells[{pc.x, pc.y}] = pc.h;
            std::set<std::pair<int, int>> seen;
            for (const auto& [start, h0] : cells) {
                if (seen.count(start)) continue;
                std::vector<std::pair<int, int>> stack{start}, group;
                seen.insert(start);
                while (!stack.empty()) {
                    const auto q = stack.back();
                    stack.pop_back();
                    group.push_back(q);
                    for (const auto& n : {std::pair{q.first + 1, q.second}, std::pair{q.first - 1, q.second},
                                          std::pair{q.first, q.second + 1}, std::pair{q.first, q.second - 1}})
                        if (cells.count(n) && seen.insert(n).second) stack.push_back(n);
                }
                int x0 = 1 << 30, x1 = -(1 << 30), y0 = 1 << 30, y1 = -(1 << 30);
                double floorH = 1e30;
                for (const auto& q : group) {
                    x0 = std::min(x0, q.first), x1 = std::max(x1, q.first);
                    y0 = std::min(y0, q.second), y1 = std::max(y1, q.second);
                    floorH = std::min(floorH, cells[q]);
                }
                const double hx = (x1 - x0 + 1) * cm * 0.5, hy = (y1 - y0 + 1) * cm * 0.5;
                const double wx = originX_ + (x0 * cm + hx), wy = originY_ + (y0 * cm + hy);
                const double pad = 0.5, ease = 4.0;
                const double reach = std::hypot(hx, hy) + pad + ease + 1.0;
                terrain_core::Relief::Pad pd;
                pd.texel = 0.5f;
                pd.base = float(floorH - 0.06);
                pd.x0 = wx - reach;
                pd.y0 = wy - reach;
                pd.width = pd.height = std::uint32_t(std::ceil(2 * reach / pd.texel));
                const std::size_t n = std::size_t(pd.width) * pd.height;
                pd.rise.assign(n, 0.0f);
                pd.weight.assign(n, 0.0f);
                for (std::uint32_t j = 0; j < pd.height; ++j)
                    for (std::uint32_t i = 0; i < pd.width; ++i) {
                        const double lx = pd.x0 + (i + 0.5) * pd.texel - wx, ly = pd.y0 + (j + 0.5) * pd.texel - wy;
                        const double ex = std::max(0.0, std::fabs(lx) - hx - pad), ey = std::max(0.0, std::fabs(ly) - hy - pad);
                        const double t = std::clamp(1.0 - std::hypot(ex, ey) / ease, 0.0, 1.0);
                        pd.weight[std::size_t(j) * pd.width + i] = float(t * t * (3 - 2 * t));
                    }
                pads.push_back(std::move(pd));
            }
        }
        if (!relief_->setPads(pads, spacing_)) {
            SDL_Log("altera_game: pads: the world's relief is too big to lay pads beside it");
            return;
        }
        std::string why;
        if (!relief_->open(device_, &why)) {
            SDL_Log("altera_game: pads: %s", why.c_str());
            return;
        }
        padsOn_ = true;
        terrainChanged_ = true;
        obstacleSig_ = std::size_t(-1);   // the slopes changed
        SDL_Log("altera_game: %zu building and field pads laid on the ground", pads.size());
    }
    bool terrainChanged() override {
        const bool was = terrainChanged_;
        terrainChanged_ = false;
        return was;
    }

    void actorWorld(const ActorSnap& a, double alpha, double& x, double& y) const {
        if (avatar_.driven && a.id == avatar_.id) {
            x = avatar_.x;
            y = avatar_.y;
            return;
        }
        x = originX_ + a.px + (a.x - a.px) * alpha;
        y = originY_ + a.py + (a.y - a.py) * alpha;
        // The sim walks straight between two ticks: a walker on the way is
        // pushed out of the houses and walls it would cut through (one standing
        // still where the sim put them, as indoors asleep, stays).
        if (!a.animal && (a.x - a.px) * (a.x - a.px) + (a.y - a.py) * (a.y - a.py) > 1e-4)
            obstacles_.pushOut(x, y, kWalkerRadius);
    }

    const ActorSnap* actor(EntityId id) const {
        for (const auto& a : shown_.actors)
            if (a.id == id) return &a;
        return nullptr;
    }


    // Eye-relative position to screen pixels; false behind the eye.
    bool project(double rx, double ry, double rz, float& sx, float& sy) const {
        const double cx = vp_[0] * rx + vp_[1] * ry + vp_[2] * rz + vp_[3];
        const double cy = vp_[4] * rx + vp_[5] * ry + vp_[6] * rz + vp_[7];
        const double cw = vp_[12] * rx + vp_[13] * ry + vp_[14] * rz + vp_[15];
        if (cw <= 0.05) return false;
        sx = float((cx / cw * 0.5 + 0.5) * viewW_);
        sy = float((0.5 - cy / cw * 0.5) * viewH_);
        return true;
    }

    // Distance on screen from a point to the segment between two projected points.
    static float segDist(float px, float py, float ax, float ay, float bx, float by) {
        const float vx = bx - ax, vy = by - ay, l2 = vx * vx + vy * vy;
        const float t = l2 > 1e-6f ? std::clamp(((px - ax) * vx + (py - ay) * vy) / l2, 0.0f, 1.0f) : 0.0f;
        return std::hypot(px - (ax + vx * t), py - (ay + vy * t));
    }
    // Who is under the mouse: the whole figure from feet to head counts, with
    // a margin that grows as they come nearer; the nearest to the eye wins a tie.
    EntityId pick(float mx, float my) const {
        EntityId best = kNone;
        float bd = 1e9f;
        const double alpha = tickAlpha();
        for (const auto& a : shown_.actors) {
            if (a.species == "fish_shoal") continue;
            double wx, wy;
            actorWorld(a, alpha, wx, wy);
            const double gz = groundAt(wx, wy), h = a.animal ? 1.0 : (a.age < 14 ? 1.1 : 1.75);
            float fx, fy, hx, hy;
            if (!project(wx - eye_[0], wy - eye_[1], gz + 0.1 - eye_[2], fx, fy)) continue;
            if (!project(wx - eye_[0], wy - eye_[1], gz + h - eye_[2], hx, hy)) continue;
            const float tall = std::hypot(hx - fx, hy - fy);
            const float margin = std::clamp(tall * 0.35f, 14.0f, 60.0f);
            const float d = segDist(mx, my, fx, fy, hx, hy);
            const double far = std::hypot(wx - eye_[0], wy - eye_[1]);
            const float score = d / margin + float(far) * 1e-4f;
            if (d < margin && score < bd) {
                bd = score;
                best = a.id;
            }
        }
        return best;
    }
    // A thing to work on under the mouse (a tree's trunk and crown, a bush, a pile).
    EntityId pickThing(float mx, float my) const {
        EntityId best = kNone;
        float bd = 1e9f;
        for (const auto& t : shown_.things) {
            const double wx = originX_ + t.x, wy = originY_ + t.y;
            const double gz = groundAt(wx, wy), h = thingHeight(t.type);
            float fx, fy, hx, hy;
            if (!project(wx - eye_[0], wy - eye_[1], gz + 0.05 - eye_[2], fx, fy)) continue;
            if (!project(wx - eye_[0], wy - eye_[1], gz + h - eye_[2], hx, hy)) continue;
            const float tall = std::hypot(hx - fx, hy - fy);
            const float margin = std::clamp(tall * 0.4f, 16.0f, 90.0f);
            const float d = segDist(mx, my, fx, fy, hx, hy);
            const float score = d / margin + float(std::hypot(wx - eye_[0], wy - eye_[1])) * 1e-4f;
            if (d < margin && score < bd) {
                bd = score;
                best = t.id;
            }
        }
        return best;
    }
    static double thingHeight(const std::string& type) {
        if (type == "tree" || type == "bee_tree") return 6.0;
        if (type == "oak_grove") return 7.5;
        if (type == "fruit_tree") return 4.0;
        if (type == "hazel_thicket") return 1.8;
        if (type == "berry_bush") return 1.0;
        if (type == "reed_bed") return 1.6;
        if (type == "riverbank" || type == "deadwood") return 0.6;
        return 0.6;
    }

    static void hsv(double h, double s, double v, float out[3]) {
        const double c = v * s, x = c * (1 - std::fabs(std::fmod(h / 60.0, 2) - 1)), m = v - c;
        double r = 0, g = 0, b = 0;
        if (h < 60) r = c, g = x;
        else if (h < 120) r = x, g = c;
        else if (h < 180) g = c, b = x;
        else if (h < 240) g = x, b = c;
        else if (h < 300) r = x, b = c;
        else r = c, b = x;
        out[0] = float(r + m), out[1] = float(g + m), out[2] = float(b + m);
    }

    int lookIndex(const ActorSnap& a) const {
        const std::uint64_t h = std::uint64_t(a.id) * 0x9E3779B97F4A7C15ull;
        const auto& pool = a.sex == 'f' ? femaleLooks_ : maleLooks_;
        return pool.empty() ? -1 : pool[std::size_t((h >> 17) % pool.size())];
    }

    // What a person plays now, from what the sim says they do.
    int personClip(const ActorSnap& a, double speed, double& rate) const {
        rate = 1.0;
        auto st = [&](const char* s) { return people_.stateClip(s); };
        if (!scriptAnim_.empty() && a.id == avatar_.id) {
            const int c = people_.interactionClip(scriptAnim_);
            const int k = c >= 0 ? c : people_.stateClip(scriptAnim_);
            if (k >= 0) return k;
        }
        if (!a.alive) return st("death");
        if (a.down) return st("down");
        if (speed > 0.25) {
            if (a.state == "Flee" || speed > 5.5) {
                rate = std::clamp(speed / 6.0, 0.8, 1.6);
                return st("sprint");
            }
            if (speed > 2.6) {
                rate = std::clamp(speed / 3.5, 0.8, 1.6);
                return st("run");
            }
            rate = std::clamp(speed / 1.3, 0.6, 1.8);
            return st("walk");
        }
        // AL-40: a joint social act names the clip of each side (kiss, embrace, refuse...).
        if (!a.anim.empty()) {
            const int c = people_.interactionClip(a.anim);
            if (c >= 0) return c;
        }
        if (!a.interaction.empty() && a.interaction != "goto" && a.interaction != "wait") {
            const int c = people_.interactionClip(a.interaction);
            if (c >= 0) return c;
        }
        static const std::pair<const char*, const char*> kByState[] = {
            {"Sleep", "sleep"}, {"Rest", "sit"}, {"Talk", "talk"}, {"Pray", "pray"}, {"Fight", "fight"},
            {"Pain", "pain"}, {"Eat", "eat"}, {"Work", "work"}, {"Act", "work"}, {"Cast", "cast"},
            {"Interact", "work"}, {"Flee", "sprint"}, {"Incapacitated", "down"}};
        for (const auto& [s, c] : kByState)
            if (a.state == s) {
                const int k = st(c);
                if (k >= 0) return k;
            }
        return st("idle");
    }

    Figure personFigure(const ActorSnap& a, double rx, double ry, double rz, double moveYaw, float highlight) {
        const double nowS = lastFrameS_;
        Anim& an = anims_[a.id];
        // Visual speed: the sim's step over the real time a tick takes (the
        // walker's own pace while the player drives them).
        const bool walker = avatar_.driven && a.id == avatar_.id;
        const double step = walker ? (avatar_.speed > 0 ? 1.0 : 0.0) : std::hypot(a.x - a.px, a.y - a.py);
        const double speed = walker ? avatar_.speed : shown_.paused ? 0.0 : step / std::max(0.05, shown_.tickSeconds);
        double rate = 1;
        const int want = personClip(a, speed, rate);
        if (want != an.clip) {
            an.from = an.clip;
            an.fromStart = an.start;
            an.fromRate = an.rate;
            an.clip = want;
            an.start = nowS;
            an.switched = nowS;
        }
        an.rate += (rate - an.rate) * std::min(1.0, frameDt_ * 4.0);
        // Turn towards the way of travel, not in a snap.
        const float target = float(moveYaw);
        if (!an.yawSet) {
            an.yaw = target;
            an.yawSet = true;
        }
        if (step > 0.05) {
            const float d = std::remainder(target - an.yaw, float(2 * kPi));
            an.yaw += d * float(std::min(1.0, frameDt_ * 8.0));
        }
        Figure f;
        f.look = std::max(0, lookIndex(a));
        f.clip = an.clip;
        f.time = (nowS - an.start) * an.rate;
        if (an.from >= 0 && nowS - an.switched < 0.35) {
            f.fromClip = an.from;
            f.fromTime = (nowS - an.fromStart) * an.fromRate;
            f.fade = float(1.0 - (nowS - an.switched) / 0.35);
        }
        f.rel[0] = rx;
        f.rel[1] = ry;
        f.rel[2] = rz;
        f.yaw = an.yaw;
        f.scale = a.age < 16 ? float(0.55 + 0.028 * a.age) : 1.0f;
        f.highlight = highlight;
        personTints(a, f.skin, f.hair, f.cloth);
        return f;
    }

    Figure beastFigure(const SkinnedSet& set, const ActorSnap& a, int head, double rx, double ry, double rz,
                       double moveYaw, float highlight) {
        const double nowS = lastFrameS_;
        Anim& an = beastAnims_[{a.id, head}];
        const double step = std::hypot(a.x - a.px, a.y - a.py);
        const double speed = shown_.paused ? 0.0 : step / std::max(0.05, shown_.tickSeconds);
        double rate = 1;
        int want;
        auto st = [&](const char* s) { return set.stateClip(s); };
        if (!a.alive) want = st("death");
        else if (a.fighting) want = st("attack");
        else if (speed > 0.25) {
            const bool fast = speed > 3.0 || a.state == "Flee";
            want = st(fast ? "run" : "walk");
            rate = std::clamp(speed / (fast ? 6.0 : 1.4), 0.6, 1.8);
        } else if (a.state == "Eat" || (a.id + head) % 3 == 1) want = st("eat");
        else want = st((a.id + head) % 2 ? "idle2" : "idle");
        if (want < 0) want = st("idle");
        if (want != an.clip) {
            an.from = an.clip;
            an.fromStart = an.start;
            an.fromRate = an.rate;
            an.clip = want;
            an.start = nowS - double((a.id * 7 + head * 13) % 50) * 0.1;   // herds out of step
            an.switched = nowS;
        }
        an.rate += (rate - an.rate) * std::min(1.0, frameDt_ * 4.0);
        if (!an.yawSet) {
            an.yaw = float(moveYaw) + float(head) * 0.7f;
            an.yawSet = true;
        }
        if (step > 0.05) an.yaw += std::remainder(float(moveYaw) - an.yaw, float(2 * kPi)) * float(std::min(1.0, frameDt_ * 6.0));
        Figure f;
        f.clip = an.clip;
        f.time = (nowS - an.start) * an.rate;
        if (an.from >= 0 && nowS - an.switched < 0.35) {
            f.fromClip = an.from;
            f.fromTime = (nowS - an.fromStart) * an.fromRate;
            f.fade = float(1.0 - (nowS - an.switched) / 0.35);
        }
        f.rel[0] = rx;
        f.rel[1] = ry;
        f.rel[2] = rz;
        f.yaw = an.yaw;
        f.scale = head ? 0.85f + 0.05f * float((a.id + head) % 4) : 1.0f;
        f.highlight = head ? 0.0f : highlight;
        return f;
    }

    // Skin from a small range of tones, hair from the family's line, cloth a
    // muted shade of the family's colour. Shared by the figure and its portrait.
    void personTints(const ActorSnap& a, float skin[3], float hair[3], float cloth[3]) const {
        // AL-71: fair European skin by default (the skin textures are baked to a light skin
        // colour, see tools/game_assets/skyrim/actors.py SKIN_ALBEDO): small shifts round it,
        // rosier, paler, a touch of sun, no dark tones.
        static const float kSkin[][3] = {{1.00f, 1.00f, 1.00f}, {1.04f, 0.98f, 0.96f}, {0.97f, 0.93f, 0.90f},
                                         {1.02f, 0.95f, 0.91f}, {0.93f, 0.87f, 0.82f}};
        static const float kHair[][3] = {{0.32f, 0.22f, 0.14f}, {0.16f, 0.12f, 0.10f}, {0.55f, 0.38f, 0.20f},
                                         {0.70f, 0.55f, 0.32f}, {0.45f, 0.20f, 0.10f}, {0.08f, 0.07f, 0.07f}};
        const std::uint64_t h = std::uint64_t(a.id) * 0x9E3779B97F4A7C15ull;
        const std::uint64_t fh = hashStr(a.family);
        const auto& sk = kSkin[(fh + (h >> 29) % 2) % 5];
        const auto& hr = kHair[(fh >> 7) % 6];
        for (int k = 0; k < 3; ++k) {
            skin[k] = sk[k];
            hair[k] = a.age > 50 ? hr[k] * 0.4f + 0.45f : hr[k];
        }
        float fc[3];
        familyColour(a.family, fc);
        for (int k = 0; k < 3; ++k) cloth[k] = (0.62f + fc[k] * 0.55f) * (a.alive ? 1.0f : 0.6f);
    }

    // No forest through the houses: the village is an open place with yards
    // round every building (the foliage and grass passes read these discs).
    void setClearings() {
        auto& c = terrain_core::clearings();
        int n = 0;
        auto add = [&](double x, double y, double inner, double outer) {
            if (n >= 32) return;
            c.discs[n][0] = float(originX_ + x), c.discs[n][1] = float(originY_ + y);
            c.discs[n][2] = float(inner), c.discs[n][3] = float(outer);
            ++n;
        };
        double far = 0;
        for (const auto& b : buildings_)
            far = std::max(far, std::hypot(b.x - villageCentre_[0], b.y - villageCentre_[1]));
        add(villageCentre_[0], villageCentre_[1], std::max(12.0, far * 0.7), far + 18.0);
        for (const auto& b : buildings_) {
            double r = 5.0;
            if (b.type == "field") r = 24.0;
            else if (b.type == "garden") r = 9.0;
            else if (b.type == "house") r = 7.0;
            else if (b.type == "shrine") r = 4.5;
            if (std::hypot(b.x - villageCentre_[0], b.y - villageCentre_[1]) + r < far * 0.7) continue;
            add(b.x, b.y, r, r + 8.0);
        }
        // the household's own building: a yard round every 8 m of footing
        std::set<std::pair<int, int>> yards;
        for (const auto& [id, p] : shown_.pieces) {
            if (p.z != 0 || p.kind.rfind("foundation", 0) != 0) continue;
            const double x = (p.x + 0.5) * content_->build.cellM, y = (p.y + 0.5) * content_->build.cellM;
            if (yards.insert({int(std::floor(x / 8.0)), int(std::floor(y / 8.0))}).second) add(x, y, 4.0, 9.0);
        }
        c.info[0] = float(n);
    }

    // The settlement's buildings from the kit bake: houses vary by family,
    // doors turned to the village's centre, set on the lowest ground under them.
    // Where a settlement building stands as drawn: works the sim set inside or
    // against a house step out (a hearth inside is the house's own chimney:
    // false), houses vary by family, doors turn to the village's centre.
    // `mesh`: the kit bake's piece (-1 without one).
    // The person's place (as drawn) and the nearest house as drawn: its box.
    bool nearestHouse(double px, double py, double& hx0, double& hy0, double& hx, double& hy, float& yaw) const {
        double bd = 1e18;
        for (const auto& b0 : buildings_) {
            if (b0.type != "house") continue;
            BuildingSnap b;
            float yw = 0;
            int mesh = -1;
            if (!buildingPose(b0, b, yw, mesh)) continue;
            const double wx = originX_ + b.x, wy = originY_ + b.y, d = std::hypot(wx - px, wy - py);
            if (d >= bd) continue;
            bd = d;
            hx0 = wx, hy0 = wy, yaw = yw;
            float a = 0, c = 0;
            if (mesh >= 0) houseSet().halfSize(mesh, a, c);
            if (a <= 0 || c <= 0) a = 4.0f, c = 3.0f;
            hx = a, hy = c;
        }
        return bd < 1e17;
    }
    void personAt(double& x, double& y) const {
        x = y = 0;
        if (const ActorSnap* me = actor(shown_.controlled)) actorWorld(*me, tickAlpha(), x, y);
    }
    void faceNearestHouse() {
        double px, py, cx, cy, hx, hy;
        float yaw;
        personAt(px, py);
        if (nearestHouse(px, py, cx, cy, hx, hy, yaw)) orbitYaw_ = std::atan2(cy - py, cx - px);
    }
    void probe(const std::string& label) {
        double px, py, cx = 0, cy = 0, hx = 0, hy = 0;
        float yaw = 0;
        personAt(px, py);
        const bool any = nearestHouse(px, py, cx, cy, hx, hy, yaw);
        // inside the house's drawn footprint (its local frame)
        const double c = std::cos(yaw), s = std::sin(yaw), lx = (px - cx) * c + (py - cy) * s,
                     ly = -(px - cx) * s + (py - cy) * c;
        const bool inside = any && std::fabs(lx) < hx * 0.9 && std::fabs(ly) < hy * 0.9;
        const ActorSnap* me = actor(shown_.controlled);
        Json j = {{"label", label},      {"clock", scriptClock_},      {"tick", shown_.tick},
                  {"paused", shown_.paused}, {"speed", shown_.speed},   {"control", control_},
                  {"controlled", shown_.controlled}, {"driven", avatar_.driven}, {"x", px - originX_},
                  {"y", py - originY_}, {"sim_x", me ? me->x : 0.0}, {"sim_y", me ? me->y : 0.0},
                  {"obstacles", obstacles_.size()}, {"house_d", std::hypot(cx - px, cy - py)},
                  {"house_half", {hx, hy}}, {"inside_house", inside},
                  {"blocked_here", obstacles_.blocked(px, py, 0.05)}};
        const fs::path out = fs::path(opts_.root) / "terrain_lab_out/probe.jsonl";
        if (FILE* f = std::fopen(out.string().c_str(), "ab")) {
            const std::string line = j.dump() + "\n";
            std::fwrite(line.data(), 1, line.size(), f);
            std::fclose(f);
        }
    }

    bool buildingPose(const BuildingSnap& b0, BuildingSnap& b, float& yaw, int& mesh) const {
        b = b0;
        if (b0.faced) {   // AL-8: placed by the village plan; only a hearth inside a house stays unseen
            if (b.type == "hearth")
                for (const auto& h : buildings_)
                    if (h.type == "house" && std::hypot(b.x - h.x, b.y - h.y) < 4.5) return false;
        } else if (b.type != "house" && b.type != "field" && b.type != "garden")
            for (const auto& h : buildings_) {
                if (h.type != "house") continue;
                const double dx = b.x - h.x, dy = b.y - h.y, d = std::hypot(dx, dy);
                const double need = b.type == "hearth" ? 6.0 : 8.5;
                if (d >= need) continue;
                if (b.type == "hearth" && d < 4.5) return false;
                const double ux = d > 0.1 ? dx / d : 1.0, uy = d > 0.1 ? dy / d : 0.0;
                b.x = h.x + ux * need;
                b.y = h.y + uy * need;
            }
        std::string piece = b.type;
        const std::uint64_t h = hashStr(b.type + std::to_string(int(b.x)) + "," + std::to_string(int(b.y)));
        if (b.type == "house") {
            static const char* kHouses[] = {"house_daub", "house_stone", "house_log"};
            piece = kHouses[(h >> 9) % 3];
        }
        mesh = houseSet().loaded() ? houseSet().piece(piece) : -1;
        // the centre as the sim had it when the world opened (the sim thread owns the world now)
        const double dx = villageCentre_[0] - b.x, dy = villageCentre_[1] - b.y;
        yaw = (dx * dx + dy * dy > 1.0) ? float(std::atan2(dx, -dy)) : float((h % 4) * kPi * 0.5);
        if (b.type == "field" || b.type == "garden") yaw = float(std::round(yaw / (kPi * 0.5)) * kPi * 0.5);
        if (b0.faced) yaw = b0.yaw;
        return true;
    }

    // AL-68: one textured ground tile (props "road_tile" / "soil_tile", a unit
    // slab with its top at the origin) lying on the ground at x, y.
    void groundTile(int piece, double x, double y, float yaw, float size, float r, float g, float b) {
        if (propsPlaced_.size() >= StaticSet::kMaxPlaced - 1024) return;   // keep room for the buildings
        Placed pl;
        pl.piece = piece;
        pl.rel[0] = x - eye_[0];
        pl.rel[1] = y - eye_[1];
        pl.rel[2] = groundAt(x, y) + 0.02 - eye_[2];
        pl.yaw = yaw;
        pl.scale = size;
        pl.tint[0] = r, pl.tint[1] = g, pl.tint[2] = b;
        propsPlaced_.push_back(pl);
    }

    // Fields and gardens dug into the ground: the levelled pad (layPads) laid
    // with furrows of turned earth, two metres a tile along the field's rows,
    // dark and light by turns, a garden's beds greener; each tile sits on the
    // ground under it.
    void soilTiles(std::vector<Instance>& out) {
        const int soilPiece = propPiece("soil_tile");
        for (const auto& b0 : buildings_) {
            if (b0.type != "field" && b0.type != "garden") continue;
            BuildingSnap b;
            float yaw = 0;
            int mesh = -1;
            if (!buildingPose(b0, b, yaw, mesh)) continue;
            const double wx = originX_ + b.x, wy = originY_ + b.y;
            if (std::hypot(wx - eye_[0], wy - eye_[1]) > 900.0) continue;
            float hx = 0, hy = 0;
            if (mesh >= 0) houseSet().halfSize(mesh, hx, hy);
            const bool garden = b.type == "garden";
            if (hx <= 0 || hy <= 0) hx = garden ? 6.0f : 20.0f, hy = garden ? 5.0f : 15.0f;
            const double t = 2.0, c = std::cos(yaw), s = std::sin(yaw);
            const int nx = std::max(1, int(std::round(2 * hx / t))), ny = std::max(1, int(std::round(2 * hy / t)));
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i) {
                    if (out.size() >= kMaxInstances / 2) return;
                    const double lx = -hx + (i + 0.5) * t, ly = -hy + (j + 0.5) * t;
                    const double x = wx + c * lx - s * ly, y = wy + s * lx + c * ly;
                    Instance in{};
                    in.placeYaw[0] = float(x - eye_[0]);
                    in.placeYaw[1] = float(y - eye_[1]);
                    in.placeYaw[2] = float(groundAt(x, y) - 0.04 - eye_[2]);
                    in.placeYaw[3] = yaw;
                    in.scale[0] = in.scale[1] = float(t) * 1.02f;
                    in.scale[2] = 0.09f;
                    const bool ridge = j % 2 == 0;
                    const float n = 0.94f + 0.12f * float((std::uint64_t(i * 73856093) ^ std::uint64_t(j * 19349663)) % 100) / 100.0f;
                    float r = ridge ? 0.42f : 0.29f, g = ridge ? 0.31f : 0.21f, bl = ridge ? 0.19f : 0.13f;
                    if (garden && ridge) r = 0.30f, g = 0.38f, bl = 0.17f;
                    in.colour[0] = in.colour2[0] = r * n;
                    in.colour[1] = in.colour2[1] = g * n;
                    in.colour[2] = in.colour2[2] = bl * n;
                    if (soilPiece >= 0) {   // AL-68: the textured tile, shaded like the block
                        const float k = (ridge ? 1.0f : 0.72f) * n;
                        groundTile(soilPiece, x, y, yaw, float(t) * 1.02f,
                                   garden && ridge ? 0.85f * k : k, garden && ridge ? 1.05f * k : k,
                                   garden && ridge ? 0.75f * k : k);
                        continue;
                    }
                    out.push_back(in);
                }
        }
    }

    // AL-8: the village's roads as beaten earth, tiles of 2 m along each
    // polyline laid on the ground, a little paler at the edges.
    void roadTiles(std::vector<Instance>& out) {
        const int roadPiece = propPiece("road_tile");
        for (std::size_t r = 0; r < roads_.size(); ++r) {
            const auto& line = roads_[r];
            const double width = r < roadWidth_.size() ? roadWidth_[r] : 3.0;
            const int across = std::max(1, int(std::round(width / 2.0)));
            for (std::size_t i = 0; i + 1 < line.size(); ++i) {
                const double ax = originX_ + line[i].x, ay = originY_ + line[i].y;
                const double bx = originX_ + line[i + 1].x, by = originY_ + line[i + 1].y;
                const double len = std::hypot(bx - ax, by - ay);
                if (len < 0.1 || std::hypot(ax - eye_[0], ay - eye_[1]) > 900.0) continue;
                const double ux = (bx - ax) / len, uy = (by - ay) / len;
                const float yaw = float(std::atan2(ux, -uy));
                const int steps = std::max(1, int(std::round(len / 2.0)));
                for (int s = 0; s < steps; ++s)
                    for (int k = 0; k < across; ++k) {
                        if (out.size() >= kMaxInstances / 2) return;
                        const double t = (s + 0.5) * len / steps, off = (k - (across - 1) * 0.5) * width / across;
                        const double x = ax + ux * t - uy * off, y = ay + uy * t + ux * off;
                        Instance in{};
                        in.placeYaw[0] = float(x - eye_[0]);
                        in.placeYaw[1] = float(y - eye_[1]);
                        in.placeYaw[2] = float(groundAt(x, y) - 0.05 - eye_[2]);
                        in.placeYaw[3] = yaw;
                        in.scale[0] = float(width / across) * 1.04f;
                        in.scale[1] = float(len / steps) * 1.04f;
                        in.scale[2] = 0.09f;
                        const bool edge = across > 1 && (k == 0 || k == across - 1);
                        const float n = 0.93f + 0.14f * float((std::uint64_t(i * 2654435761u) ^ std::uint64_t(s * 40503u + k)) % 100) / 100.0f;
                        in.colour[0] = in.colour2[0] = (edge ? 0.47f : 0.40f) * n;
                        in.colour[1] = in.colour2[1] = (edge ? 0.40f : 0.33f) * n;
                        in.colour[2] = in.colour2[2] = (edge ? 0.30f : 0.24f) * n;
                        if (roadPiece >= 0) {   // AL-68: the textured tile, square, turned by quarters
                            const float shade = (edge ? 1.12f : 1.0f) * n;
                            const float quarter = float((std::uint64_t(i * 2654435761u) ^ std::uint64_t(s * 97u + k * 31u)) % 4) * 1.5707963f;
                            groundTile(roadPiece, x, y, yaw + quarter,
                                       float(std::max(width / across, len / steps)) * 1.04f, shade, shade, shade);
                            continue;
                        }
                        out.push_back(in);
                    }
            }
        }
    }

    void placeBuildings(double range) {
        for (const auto& b0 : buildings_) {
            BuildingSnap b;
            float yaw = 0;
            int id = -1;
            if (!buildingPose(b0, b, yaw, id) || id < 0) continue;
            if (b.type == "field" || b.type == "garden") continue;   // dug into the ground instead (soilTiles)
            const double wx = originX_ + b.x, wy = originY_ + b.y;
            const double rx = wx - eye_[0], ry = wy - eye_[1];
            if (rx * rx + ry * ry > range * range * 4) continue;
            const std::uint64_t h = hashStr(b.type + std::to_string(int(b.x)) + "," + std::to_string(int(b.y)));
            Placed pl;
            pl.piece = id;
            pl.yaw = yaw;
            float hx, hy;
            houseSet().halfSize(id, hx, hy);
            double z = groundAt(wx, wy);
            const double r = std::min<double>(std::max(hx, hy), 6.0) * 0.8;
            for (int k = 0; k < 4; ++k) z = std::min(z, groundAt(wx + r * std::cos(k * kPi * 0.5 + 0.7), wy + r * std::sin(k * kPi * 0.5 + 0.7)));
            pl.rel[0] = rx;
            pl.rel[1] = ry;
            pl.rel[2] = z - eye_[2];
            const float t = 0.92f + 0.16f * float((h >> 21) % 100) / 100.0f;
            pl.tint[0] = pl.tint[1] = pl.tint[2] = t;
            housePlaced().push_back(pl);
            // AL-56: a baked building is cut as one (to the sill)
            float mn[3], mx[3];
            if (b.type != "hearth" && b.type != "village_fire" && houseSet().bounds(id, mn, mx)) {
                cutaway::Piece c;
                c.key = h | (1ull << 63);
                c.kind = cutaway::Kind::Whole;
                c.x = wx, c.y = wy, c.z0 = z + mn[2], c.z1 = z + mx[2];
                c.hx = std::max(0.5f, hx), c.hy = std::max(0.5f, hy), c.yaw = yaw;
                cutPieces_.push_back(c);
                cutRefs_.push_back({&housePlaced() == &propsPlaced_, housePlaced().size() - 1});
            }
        }
    }

    void buildInstances() {
        instances_.clear();
        placed_.clear();
        propsPlaced_.clear();
        figures_.clear();
        for (auto& [k, v] : beastFigures_) v.clear();
        const double nowS = std::chrono::duration<double>(Clock::now() - started_).count();
        frameDt_ = std::clamp(nowS - lastFrameS_, 0.0, 0.1);
        lastFrameS_ = nowS;
        std::vector<Instance> per[kMeshes];
        const double alpha = tickAlpha();
        const double range = std::max(1500.0, zoom_ * 4.0);
        for (const auto& a : shown_.actors) {
            const bool fish = a.species == "fish_shoal";
            if (fish && !beasts_.count("fish_shoal")) continue;
            double wx, wy;
            actorWorld(a, alpha, wx, wy);
            const double rx = wx - eye_[0], ry = wy - eye_[1];
            if (rx * rx + ry * ry > range * range) continue;
            const double rz = groundAt(wx, wy) - eye_[2];
            double yaw = 0;
            const double mx = a.x - a.px, my = a.y - a.py;
            if (avatar_.driven && a.id == avatar_.id) yaw = avatar_.yaw;
            else if (mx * mx + my * my > 0.01) yaw = std::atan2(my, mx);
            else yaw = double(a.id % 628) / 100.0;
            Instance in{};
            in.placeYaw[0] = float(rx), in.placeYaw[1] = float(ry), in.placeYaw[2] = float(rz), in.placeYaw[3] = float(yaw);
            const float hl = !scriptHighlight_ ? 0.0f
                             : (a.id == selected_ || a.id == hud_.aim ? 1.0f : 0.0f) + (a.player ? 0.6f : 0.0f);
            if (!a.animal && people_.loaded()) {
                if (figures_.size() < SkinnedSet::kMaxFigures) figures_.push_back(personFigure(a, rx, ry, rz, yaw, hl));
            } else if (!a.animal) {
                const float s = a.age < 14 ? 0.55f + 0.03f * float(a.age) : 1.0f;
                in.scale[0] = in.scale[1] = in.scale[2] = s;
                in.scale[3] = hl;
                familyColour(a.family, in.colour);
                if (!a.alive) in.colour[0] = in.colour[1] = in.colour[2] = 0.25f;
                in.colour2[0] = 0.86f, in.colour2[1] = 0.70f, in.colour2[2] = 0.56f;
                if (a.fighting) in.colour[0] = 0.85f, in.colour[1] = 0.2f, in.colour[2] = 0.15f;
                if (!a.alive || a.down) {   // lying on the ground
                    in.scale[2] = 0.25f * s;
                    in.scale[0] = in.scale[1] = 2.2f * s;
                }
                per[kPerson].push_back(in);
            } else {
                float s = 1.0f;
                float col[3] = {0.45f, 0.36f, 0.28f};
                const std::string& sp = a.species;
                if (sp == "hare") s = 0.35f, col[0] = 0.55f, col[1] = 0.50f, col[2] = 0.42f;
                else if (sp == "deer") s = 1.5f, col[0] = 0.55f, col[1] = 0.38f, col[2] = 0.22f;
                else if (sp == "boar") s = 1.0f, col[0] = 0.25f, col[1] = 0.20f, col[2] = 0.17f;
                else if (sp == "wolf") s = 1.0f, col[0] = 0.42f, col[1] = 0.42f, col[2] = 0.44f;
                else if (sp == "hens") s = 0.3f, col[0] = 0.85f, col[1] = 0.80f, col[2] = 0.70f;
                else if (sp == "goats") s = 0.8f, col[0] = 0.80f, col[1] = 0.76f, col[2] = 0.66f;
                else if (sp == "pig") s = 0.9f, col[0] = 0.85f, col[1] = 0.62f, col[2] = 0.58f;
                // A herd or a pack is several heads: a few beside the leader.
                const int heads = std::clamp(a.group, 1, 6);
                const auto bs = beasts_.find(sp);
                if (bs != beasts_.end()) {
                    auto& list = beastFigures_[sp];
                    for (int k = 0; k < heads && list.size() < SkinnedSet::kMaxFigures; ++k) {
                        double bx = wx, by = wy;
                        if (k) {
                            const double ang = double(k) * 2.4 + double(a.id), r = (fish ? 0.5 + 0.3 * k : 1.2 + 0.6 * k);
                            bx += std::cos(ang) * r;
                            by += std::sin(ang) * r;
                        }
                        // A shoal swims under the surface (below the sea line, or over a stream's bed).
                        double bz = groundAt(bx, by);
                        if (fish) {
                            const double raw = double(ground_.at(bx, by, 2.0));
                            bz = raw < 0.0 ? std::max(raw + 0.3, -0.5) - 0.1 * double(k % 3) : raw + 0.2;
                        }
                        list.push_back(beastFigure(*bs->second, a, k, bx - eye_[0], by - eye_[1], bz - eye_[2], yaw, hl));
                    }
                    continue;
                }
                for (int k = 0; k < heads; ++k) {
                    Instance b = in;
                    if (k) {
                        const double ang = double(k) * 2.4 + double(a.id), r = 1.6 * s + 0.5 * k;
                        const double bx = wx + std::cos(ang) * r, by = wy + std::sin(ang) * r;
                        b.placeYaw[0] = float(bx - eye_[0]);
                        b.placeYaw[1] = float(by - eye_[1]);
                        b.placeYaw[2] = float(groundAt(bx, by) - eye_[2]);
                    }
                    b.scale[0] = b.scale[1] = b.scale[2] = s;
                    b.scale[3] = hl;
                    std::memcpy(b.colour, col, sizeof col);
                    b.colour2[0] = col[0] * 0.8f, b.colour2[1] = col[1] * 0.8f, b.colour2[2] = col[2] * 0.8f;
                    if (!a.alive) b.scale[2] = 0.3f * s;
                    per[kBeast].push_back(b);
                }
            }
        }
        for (const auto& t : shown_.things) {
            const double wx = originX_ + t.x, wy = originY_ + t.y;
            const double rx = wx - eye_[0], ry = wy - eye_[1];
            if (rx * rx + ry * ry > 260.0 * 260.0) continue;
            Instance in{};
            const std::uint64_t h = std::uint64_t(t.id) * 0x9E3779B97F4A7C15ull;
            const float var = 0.85f + 0.3f * float((h >> 20) % 100) / 100.0f;
            in.placeYaw[0] = float(rx), in.placeYaw[1] = float(ry);
            in.placeYaw[2] = float(groundAt(wx, wy) - 0.05 - eye_[2]);
            in.placeYaw[3] = float(double((h >> 8) % 628) / 100.0);
            in.scale[0] = in.scale[1] = in.scale[2] = var;
            in.scale[3] = (t.id == hud_.hoverThing || (hud_.panel == "thing" && t.id == hud_.actTarget())) ? 1.0f : 0.0f;
            auto col = [&](float r, float g, float b, float r2, float g2, float b2) {
                in.colour[0] = r, in.colour[1] = g, in.colour[2] = b;
                in.colour2[0] = r2, in.colour2[1] = g2, in.colour2[2] = b2;
            };
            const std::string& ty = t.type;
            if (const int pp = propPiece(ty); pp >= 0) {
                Placed pl;
                pl.piece = pp;
                pl.rel[0] = rx;
                pl.rel[1] = ry;
                pl.rel[2] = groundAt(wx, wy) - 0.03 - eye_[2];
                pl.yaw = in.placeYaw[3];
                pl.scale = var;
                pl.highlight = in.scale[3];
                propsPlaced_.push_back(pl);
                continue;
            }
            Mesh m = kTuft;
            if (ty == "tree") m = kTree, col(0.36f, 0.26f, 0.17f, 0.17f, 0.30f, 0.14f);
            else if (ty == "bee_tree") m = kTree, col(0.33f, 0.24f, 0.16f, 0.24f, 0.31f, 0.13f);
            else if (ty == "oak_grove") m = kTree, col(0.30f, 0.23f, 0.16f, 0.24f, 0.35f, 0.13f), in.scale[0] *= 1.35f, in.scale[1] *= 1.35f, in.scale[2] *= 1.2f;
            else if (ty == "fruit_tree") m = kTree, col(0.38f, 0.28f, 0.19f, 0.30f, 0.42f, 0.17f), in.scale[0] *= 0.7f, in.scale[1] *= 0.7f, in.scale[2] *= 0.65f;
            else if (ty == "deadwood") m = kLog, col(0.42f, 0.32f, 0.22f, 0.34f, 0.27f, 0.19f);
            else if (ty == "riverbank") m = kRock, col(0.46f, 0.44f, 0.41f, 0.56f, 0.53f, 0.49f), in.scale[0] *= 1.3f, in.scale[1] *= 1.3f;
            else if (ty == "berry_bush") m = kBush, col(0.20f, 0.33f, 0.15f, 0.48f, 0.12f, 0.17f);
            else if (ty == "hazel_thicket") m = kBush, col(0.24f, 0.34f, 0.14f, 0.40f, 0.33f, 0.18f), in.scale[0] *= 1.6f, in.scale[1] *= 1.6f, in.scale[2] *= 1.7f;
            else if (ty == "root_patch") col(0.33f, 0.28f, 0.17f, 0.30f, 0.42f, 0.19f);
            else if (ty == "mushroom_glade") col(0.82f, 0.78f, 0.70f, 0.58f, 0.24f, 0.14f);
            else if (ty == "reed_bed") col(0.42f, 0.46f, 0.24f, 0.58f, 0.54f, 0.32f), in.scale[2] *= 2.8f;
            else if (ty == "nettle_patch") col(0.18f, 0.37f, 0.14f, 0.23f, 0.43f, 0.17f), in.scale[0] *= 1.4f, in.scale[1] *= 1.4f;
            else if (ty == "herb_patch") col(0.27f, 0.42f, 0.19f, 0.62f, 0.52f, 0.72f);
            else if (ty == "wild_greens_patch") col(0.28f, 0.47f, 0.19f, 0.38f, 0.53f, 0.24f);
            else if (ty == "hay_meadow") col(0.58f, 0.53f, 0.29f, 0.68f, 0.60f, 0.34f), in.scale[0] *= 3.0f, in.scale[1] *= 3.0f;
            else m = kBlock, col(0.40f, 0.34f, 0.28f, 0.40f, 0.34f, 0.28f), in.scale[0] *= 0.8f, in.scale[1] *= 0.8f, in.scale[2] *= 0.6f;
            per[m].push_back(in);
        }
        soilTiles(per[kBlock]);
        roadTiles(per[kBlock]);
        cutPieces_.clear();
        cutRefs_.clear();
        if (houseSet().loaded()) placeBuildings(range);
        if (village_.loaded()) placePieces(range);
        applyCutaway();
        for (const auto& b : buildings_) {
            if (houseSet().loaded()) break;
            const double wx = originX_ + b.x, wy = originY_ + b.y;
            const double rx = wx - eye_[0], ry = wy - eye_[1];
            if (rx * rx + ry * ry > range * range * 4) continue;
            Instance in{};
            in.placeYaw[0] = float(rx), in.placeYaw[1] = float(ry);
            in.placeYaw[2] = float(groundAt(wx, wy) - eye_[2] - 0.3);
            in.placeYaw[3] = float((hashStr(b.type) % 4) * 0.4);
            Mesh mesh = kBlock;
            auto set = [&](float sx, float sy, float sz, float r, float g, float bl, float r2, float g2, float b2) {
                in.scale[0] = sx, in.scale[1] = sy, in.scale[2] = sz;
                in.colour[0] = r, in.colour[1] = g, in.colour[2] = bl;
                in.colour2[0] = r2, in.colour2[1] = g2, in.colour2[2] = b2;
            };
            if (b.type == "house") mesh = kHouse, set(8, 6, 3.2f, 0.52f, 0.42f, 0.30f, 0.42f, 0.33f, 0.20f);
            else if (b.type == "storage") mesh = kHouse, set(4.5f, 4, 2.6f, 0.46f, 0.36f, 0.24f, 0.36f, 0.30f, 0.18f);
            else if (b.type == "hearth") set(1.4f, 1.4f, 0.8f, 0.30f, 0.29f, 0.27f, 0.30f, 0.29f, 0.27f);
            else if (b.type == "village_fire") set(2.2f, 2.2f, 0.9f, 0.85f, 0.42f, 0.12f, 0.85f, 0.42f, 0.12f);
            else if (b.type == "shrine") set(1.6f, 1.6f, 3.4f, 0.55f, 0.55f, 0.52f, 0.55f, 0.55f, 0.52f);
            else if (b.type == "field" || b.type == "garden") continue;   // soilTiles
            per[mesh].push_back(in);
        }
        for (int m = 0; m < kMeshes; ++m) {
            const std::size_t room = kMaxInstances - instances_.size();
            const std::size_t n = std::min(room, per[m].size());
            batch_[m] = {std::uint32_t(instances_.size()), std::uint32_t(n)};
            instances_.insert(instances_.end(), per[m].begin(), per[m].begin() + std::ptrdiff_t(n));
        }
    }

    bool openGpu(std::string* why) {
        MeshBuilder mb;
        auto range = [&](Mesh m, std::size_t start) {
            meshes_[m] = {std::uint32_t(start), std::uint32_t(mb.v.size() - start)};
        };
        std::size_t s = mb.v.size();
        mb.frustum(8, 0.24f, 0.0f, 0.17f, 1.38f, 0.0f, 1.0f, 0.75f);   // body
        mb.frustum(6, 0.13f, 1.40f, 0.10f, 1.74f, 1.0f);              // head
        range(kPerson, s);
        s = mb.v.size();
        mb.box(-0.5f, -0.2f, 0.35f, 0.5f, 0.2f, 0.75f, 0.0f);   // body
        mb.box(0.45f, -0.12f, 0.55f, 0.75f, 0.12f, 0.85f, 1.0f); // head
        for (float lx : {-0.4f, 0.35f})
            for (float ly : {-0.15f, 0.1f}) mb.box(lx, ly, 0.0f, lx + 0.07f, ly + 0.07f, 0.36f, 1.0f);
        range(kBeast, s);
        s = mb.v.size();
        mb.box(-0.5f, -0.5f, 0.0f, 0.5f, 0.5f, 1.0f, 0.0f);
        {   // a gable roof over the unit box
            const float a[3] = {-0.55f, -0.55f, 1.0f}, b[3] = {0.55f, -0.55f, 1.0f}, c[3] = {0.55f, 0.0f, 1.7f},
                        d[3] = {-0.55f, 0.0f, 1.7f}, e[3] = {0.55f, 0.55f, 1.0f}, f[3] = {-0.55f, 0.55f, 1.0f};
            mb.quad(a, b, c, d, 1.0f);
            mb.quad(e, f, d, c, 1.0f);
            mb.tri(b, e, c, 0.0f);
            mb.tri(f, a, d, 0.0f);
        }
        range(kHouse, s);
        s = mb.v.size();
        mb.box(-0.5f, -0.5f, 0.0f, 0.5f, 0.5f, 1.0f, 0.0f);
        range(kBlock, s);
        s = mb.v.size();
        mb.frustum(6, 0.24f, 0.0f, 0.15f, 2.4f, 0.0f);    // trunk
        mb.frustum(7, 1.45f, 1.9f, 0.10f, 4.6f, 1.0f);    // crown, low
        mb.frustum(7, 1.05f, 3.5f, 0.04f, 6.2f, 1.0f);    // crown, top
        range(kTree, s);
        s = mb.v.size();
        mb.box(-1.25f, -0.32f, 0.0f, 1.15f, -0.02f, 0.3f, 0.0f);
        mb.box(-0.95f, 0.02f, 0.0f, 1.05f, 0.30f, 0.27f, 1.0f);
        mb.box(-0.55f, -0.18f, 0.28f, 0.75f, 0.12f, 0.52f, 0.0f);
        range(kLog, s);
        s = mb.v.size();
        mb.box(-0.65f, -0.55f, 0.0f, 0.45f, 0.40f, 0.55f, 0.0f);
        mb.box(0.25f, -0.25f, 0.0f, 0.95f, 0.45f, 0.38f, 1.0f);
        mb.box(-0.95f, 0.15f, 0.0f, -0.35f, 0.75f, 0.30f, 1.0f);
        mb.box(-0.2f, -0.95f, 0.0f, 0.3f, -0.45f, 0.22f, 0.0f);
        range(kRock, s);
        s = mb.v.size();
        mb.frustum(7, 0.75f, 0.0f, 0.62f, 0.55f, 0.0f);
        mb.frustum(7, 0.62f, 0.55f, 0.12f, 1.05f, 1.0f);
        range(kBush, s);
        s = mb.v.size();
        mb.frustum(5, 0.45f, 0.0f, 0.28f, 0.28f, 0.0f);
        mb.frustum(5, 0.28f, 0.28f, 0.03f, 0.6f, 1.0f);
        range(kTuft, s);
        vertBuffer_ = device_.uploadBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, mb.v.data(), mb.v.size() * sizeof(Vertex));
        instBuffer_ = device_.makeBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, kMaxInstances * sizeof(Instance));
        SDL_GPUTransferBufferCreateInfo t{};
        t.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        t.size = std::uint32_t(kMaxInstances * sizeof(Instance));
        instTransfer_ = SDL_CreateGPUTransferBuffer(device_.handle(), &t);

        engine::PipelineWanted wanted;
        wanted.shaderFile = "altera_game/actors.hlsl";
        wanted.vertexEntry = "actorsVs";
        wanted.fragmentEntry = "actorsPs";
        wanted.depthClip = true;
        wanted.cull = SDL_GPU_CULLMODE_NONE;
        wanted.buffers = {{0, sizeof(Vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0},
                          {1, sizeof(Instance), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0}};
        wanted.attributes = {{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0},
                             {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 12},
                             {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, 24},
                             {3, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0},
                             {4, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 16},
                             {5, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 32},
                             {6, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 48}};
        actorsPipe_ = device_.makePipeline(wanted);

        engine::PipelineWanted over;
        over.shaderFile = "altera_game/overlay.hlsl";
        over.vertexEntry = "overlayVs";
        over.fragmentEntry = "overlayPs";
        over.depthTest = over.depthWrite = false;
        over.blend = true;
        over.colourFormats = {engine::Device::kColourFormat};
        overlayPipe_ = device_.makePipeline(over);
        SDL_GPUSamplerCreateInfo sampling{};
        sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_NEAREST;
        sampling.address_mode_u = sampling.address_mode_v = sampling.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler_ = device_.makeSampler(sampling);
        if (!vertBuffer_ || !instBuffer_ || !instTransfer_ || !actorsPipe_ || !overlayPipe_ || !sampler_) {
            if (why) *why = "game GPU objects: " + device_.error();
            return false;
        }
        {   // The people on the UAL rig; without the bake they stay placeholder shapes.
            const fs::path root(opts_.root);
            std::string whyPeople;
            // AL-11: the Skyrim stand-ins on the same rig (tools/game_assets/skyrim/actors.py), when baked.
            // AL-70: they are on Skyrim's own skeleton now, with Skyrim's clips (skyrim/people_native.py).
            const fs::path peopleSky = root / "assets/altera_game/people/people_sky.askin";
            const fs::path animSky = root / "assets/altera_game/people/people_sky.aanim";
            const bool sky = fs::exists(peopleSky) && fs::exists(animSky);
            if (!people_.load(device_, sky ? peopleSky : root / "assets/altera_game/people/people.askin",
                              sky ? animSky : root / "content/animations/humanoid_ual/baked/people.aanim",
                              sky ? root / "assets/altera_game/people/people_sky_anim.json"
                                  : root / "content/animations/humanoid_ual/baked/people_anim.json",
                              &whyPeople))
                SDL_Log("altera_game: people stay placeholders: %s", whyPeople.c_str());
            for (int i = 0; i < people_.lookCount(); ++i)
                (people_.lookName(i)[0] == 'f' ? femaleLooks_ : maleLooks_).push_back(i);
            std::string whyVillage;
            // AL-11: the kit with Skyrim textures (tools/game_assets/skyrim/world_tex.py), when baked.
            const fs::path villageSky = root / "assets/altera_game/buildings/buildings_sky.astm";
            if (!village_.load(device_, fs::exists(villageSky) ? villageSky : root / "assets/altera_game/buildings/buildings.astm",
                               &whyVillage))
                SDL_Log("altera_game: buildings stay placeholders: %s", whyVillage.c_str());
            // AL-11: dev-only stand-ins from the local Skyrim (tools/game_assets/skyrim/convert.py):
            // buildings and every sim object get a real model; absent, the kit and the blocks stay.
            const fs::path propsFile = root / "assets/altera_game/props/props.astm";
            if (fs::exists(propsFile)) {
                std::string whyProps;
                if (!props_.load(device_, propsFile, &whyProps)) SDL_Log("altera_game: no props: %s", whyProps.c_str());
            }
            // Beasts with a bake of their own (hens and fish: tools/game_assets/proc_beasts.py).
            for (const char* sp : {"deer", "wolf", "hare", "boar", "pig", "goats", "hens", "fish_shoal"}) {
                const fs::path base = root / "assets/altera_game/beasts" / sp;
                auto set = std::make_unique<SkinnedSet>();
                std::string whyBeast;
                const fs::path sky(base.string() + "_sky.askin");
                if (set->load(device_, fs::exists(sky) ? sky : fs::path(base.string() + ".askin"),
                              fs::path(base.string() + ".aanim"),
                              fs::path(base.string() + "_anim.json"), &whyBeast))
                    beasts_[sp] = std::move(set);
                else
                    SDL_Log("altera_game: %s stays a placeholder: %s", sp, whyBeast.c_str());
            }
        }
        loadFonts();
        return true;
    }

    // AL-62: the faces the theme names (content/ui/themes/<id>.json fonts: text
    // and title, the "bold" face of ui::Ui), then its fallbacks, then Noto Sans.
    void loadFonts() {
        const fs::path fonts = fs::path(opts_.root) / "assets" / "fonts";
        const UiTheme& th = hudPalette().theme;
        const auto face = [&](const std::string& file) {
            return file.empty() ? std::shared_ptr<ui::FontFace>() : ui::FontFace::load(fonts / file);
        };
        font_ = face(th.font("text"));
        fontBold_ = face(th.font("title"));
        if (!font_ || !fontBold_) {
            font_ = face(th.font("text_fallback"));
            fontBold_ = face(th.font("title_fallback"));
        }
        if (!font_ || !fontBold_) {
            font_ = ui::FontFace::load(fonts / "NotoSans-Regular.ttf");
            fontBold_ = ui::FontFace::load(fonts / "NotoSans-Bold.ttf");
        }
        uiScale_ = 0;
    }

    // ---- the HUD (style G) ------------------------------------------------
    // The engine widgets (text fields, sliders) in the HUD's theme (AL-62).
    void styleTheme() {
        const HudPalette& p = hudPalette();
        ui::Theme& t = ui_.theme();
        t.slot = p.panel;
        t.slotHot = p.panel.lerp(p.gold, 0.15f);
        t.slotDown = p.gold;
        t.slotEdge = p.frame;
        t.label = p.text;
        t.labelSoft = p.mute;
        t.accent = p.gold;
        t.paper = p.card;
        t.paperEdge = p.frame;
        t.ink = p.cardInk;
        t.inkSoft = p.cardInkSoft;
        t.gauge = p.rim.withAlpha(0.85f);
        t.radius = p.theme.size("radius", 2.0f);
    }

    void buildHud(float W, float H) {
        hud_.draw(ui_, input_, shown_, selected_, *this, W, H);
        wheelOpen_ = hud_.panel == "wheel";
        // Selection by portraits and names: the hud calls select(); keep the sim's detail in step.
        selectedWanted_ = selected_;
    }

    // ---- HudHost ------------------------------------------------------------
    std::string interactionName(const std::string& id) const override { return name("interaction", id); }
    void command(const std::string& type, const Json& args) override {
        noteInput();
        // An act on something away from the person: they first walk there
        // themselves, round the houses (the sim would cut straight through),
        // and the act is sent when they stand by it.
        if (type == "perform" && !pendingAct_ && args.contains("target") && args["target"].is_number_integer() &&
            approach(args["target"].get<EntityId>())) {
            pendingAct_ = true;
            pendingType_ = type;
            pendingArgs_ = args;
            return;
        }
        sendCommand(type, args);
    }
    // AL-67: the topics the player's person could bring up with the wheel's
    // target, and the cases opened against the person.
    void talkSnap(const World& w, Snapshot& s) const {
        const Character* me = w.character(s.controlled);
        if (!me) return;
        if (const Character* t = w.character(wheelWanted_); t && t->id != me->id && !t->isAnimal()) {
            s.topicsFor = t->id;
            for (const auto& tp : dialogueTopics(w, *me, *t))
                s.topics.push_back({tp.act, menuLabel(*loc_, opts_.lang, w, *me, *t, tp)});
        }
        for (const auto& m : me->memory) {
            if (m.type != "case_pending" || m.who.empty()) continue;
            CaseSnap c;
            c.accuser = m.who[0];
            if (const Character* a = w.character(m.who[0])) c.accuserName = a->name;
            if (m.who.size() > 1)
                if (const Character* j = w.character(m.who[1])) c.judgeName = j->name;
            c.stage = m.source;
            if (const Character* o = w.character(m.object)) c.what = o->name;
            c.day = int(m.tick / kTicksPerDay) + 1;
            s.cases.push_back(std::move(c));
        }
        if (!s.cases.empty())
            for (const auto& a : caseRules(w.content()).answers) s.caseAnswers.push_back(a.id);
    }

    // AL-65: an NPC's ask the player's person has to answer, and its followers.
    void socialSnap(const World& w, Snapshot& s) const {
        const Character* me = w.character(s.controlled);
        if (!me) return;
        const SocialLink& l = me->ai.social;
        if (l.active() && l.role == "target" && l.phase == "answer") {
            s.social.askFrom = l.partner;
            if (const Character* a = w.character(l.partner)) s.social.askName = a->name;
            s.social.askAct = l.chain;
            s.social.askActName = name("interaction", l.chain);
            s.social.askTone = l.tone;
            s.social.askTicksLeft = int(std::max<Tick>(0, l.until - w.now()));
            s.social.askTicks = int(std::max<Tick>(1, l.until - l.since));
        }
        for (EntityId id : followersOf(w, me->id))
            if (const Character* f = w.character(id)) s.social.followers.push_back({id, f->name});
    }

    void sendCommand(const std::string& type, const Json& args) {
        // Free mode: an act from the wheel joins the person's queue (done in
        // turn, as in The Sims) instead of replacing what goes on.
        if (control_ == kMouse && type == "perform" && args.contains("interaction") && args.contains("id")) {
            Json q{{"id", args["id"]}, {"task", args["interaction"]}, {"reason", "player_queue"}};
            if (args.contains("target")) q["target"] = args["target"];
            post({"cmd", 0, "queue_task", q});
            return;
        }
        post({"cmd", 0, type, args});
    }
    void pause() override { post({"pause"}); }
    void speed(int n) override {
        if (speedLocked() && n != 1) return;   // walking by the keys holds the clock at 1
        post({"speed", n});
    }
    // Selecting a person shows them (the camera goes there); riding on them
    // is the camera button's (Tab), apart from the choice.
    void select(EntityId id) override {
        selected_ = id;
        selectedWanted_ = id;
        if (const ActorSnap* a = actor(id)) {
            double wx, wy;
            actorWorld(*a, tickAlpha(), wx, wy);
            if (!follow_) {
                target_[0] = wx;
                target_[1] = wy;
            }
        }
    }
    void wheelTarget(EntityId id) override { wheelWanted_ = id; }
    void craftDraft(const Json& draft) override { post({"craft_draft", 0, "", draft}); }
    std::vector<std::pair<std::string, std::string>> goals() const override {
        std::vector<std::pair<std::string, std::string>> out;
        for (std::size_t i = 0; i < goalIds_.size(); ++i) out.push_back({goalIds_[i], goalNames_[i]});
        return out;
    }
    std::string lookOf(const ActorSnap& a) const override {
        const int k = lookIndex(a);
        return k >= 0 && k < people_.lookCount() ? people_.lookName(k) : std::string();
    }
    void tintsOf(const ActorSnap& a, float skin[3], float hair[3], float cloth[3]) const override {
        personTints(a, skin, hair, cloth);
    }
    void familyColour(const std::string& f, float out[3]) const override {
        const double h = double(hashStr(f) % 360);
        hsv(h, 0.55, 0.62, out);
    }
    bool overHead(const ActorSnap& a, float& x, float& y) const override {
        double wx, wy;
        actorWorld(a, tickAlpha(), wx, wy);
        const double rx = wx - eye_[0], ry = wy - eye_[1];
        if (rx * rx + ry * ry > 90.0 * 90.0 && a.id != selected_ && a.id != hud_.aim) return false;
        const double h = a.animal ? 1.4 : (a.age < 16 ? 0.55 + 0.028 * a.age : 1.0) * 2.05;
        const bool on = project(rx, ry, groundAt(wx, wy) + h - eye_[2], x, y) && x > -50 && y > -50 &&
                        x < viewW_ + 50 && y < viewH_ + 50;
        toUi(x, y);   // AL-62: the HUD is in points
        return on;
    }
    double cameraYaw() const override { return orbitYaw_; }
    // AL-67: the HUD's own operations on the sim (a topic brought up, a name given).
    void simOp(const std::string& op, const Json& args) override {
        noteInput();
        post({op, 0, "", args});
    }
    // The case forms the language's rules suggest for a typed name (prefill of the name screen).
    Json nameCases(const std::string& n, char sex) const override { return loc_->personNoun(opts_.lang, n, sex); }
    void toUi(float& x, float& y) const {
        const float s = uiScale_ > 0 ? uiScale_ : 1.0f;
        x /= s;
        y /= s;
    }
    double groundHeight(double x, double y) const override { return groundAt(originX_ + x, originY_ + y); }
    bool groundToScreen(double simX, double simY, double up, float& x, float& y) const override {
        const double wx = originX_ + simX, wy = originY_ + simY;
        const double rx = wx - eye_[0], ry = wy - eye_[1];
        if (rx * rx + ry * ry > 120.0 * 120.0) return false;
        const bool on = project(rx, ry, groundAt(wx, wy) + up - eye_[2], x, y) && x > 0 && y > 0 && x < viewW_ &&
                        y < viewH_;
        toUi(x, y);
        return on;
    }
    double tickAlpha() const override {
        if (shown_.paused) return 1.0;
        const double since = std::chrono::duration<double>(Clock::now() - shown_.tickAt).count();
        return std::clamp(since / std::max(1e-3, shown_.tickSeconds), 0.0, 1.0);
    }

    static constexpr std::size_t kMaxInstances = 8192;

    engine::Device& device_;
    SDL_Window* window_;
    Json script_;
    Json craftDraft_ = Json::object();   // the workshop's attempt as the HUD has it (sim thread)
    mutable Json craftUi_;               // content/ui/craft.json, read on first use
    std::size_t scriptStep_ = 0;
    double scriptClock_ = 0;
    std::string shotWanted_, shotPending_, panel_ = "none";
    SDL_GPUTransferBuffer* shotTransfer_ = nullptr;
    std::uint32_t shotW_ = 0, shotH_ = 0;
    int shotCounter_ = 0;
    terrain_core::GroundHeight ground_;
    double worldSize_;
    double spacing_ = 1.0;                 // the clipmap's finest spacing (the pads' corners snap to it)
    terrain_core::Relief* relief_ = nullptr;   // the view's fine relief, the game's pads on it
    bool padsOn_ = false, terrainChanged_ = false;
    std::size_t padSig_ = std::size_t(-1);
    GameOptions opts_;
    Json worldCfg_;                      // the sim world's config (New game makes the world again from it)
    GameSettings settings_;
    Tick freshTick_ = 0;                 // the tick the world was made at
    std::vector<std::pair<std::string, Json>> menuQueue_;   // menu buttons, run at the next update
    std::string menuBack_ = "menu";      // where the slots and the settings go back to
    bool pauseHeld_ = false;             // the pause menu stopped the clock (and lets it go)
    double autosaveClock_ = 0;
    std::unique_ptr<Content> content_;
    std::unique_ptr<Loc> loc_;
    std::unique_ptr<World> world_;
    double originX_ = 0, originY_ = 0;
    double villageCentre_[2] = {0, 0};
    std::vector<std::vector<Vec2>> roads_;   // AL-8: the village plan's roads (sim coordinates)
    std::vector<double> roadWidth_;
    std::vector<BuildingSnap> buildings_;
    std::vector<std::string> goalIds_, goalNames_;
    int goalChoice_ = 0;
    bool goalListOpen_ = false;

    // Sim thread state (only the sim thread touches these after open()).
    std::thread thread_;
    std::atomic<bool> stop_{false};
    bool paused_ = true;
    int speed_ = 2;
    std::string result_;
    Clock::time_point tickAt_ = Clock::now();
    std::map<EntityId, Vec2> prevPos_;
    std::atomic<EntityId> selectedWanted_{kNone};
    std::atomic<EntityId> wheelWanted_{kNone};
    std::atomic<bool> wheelOpen_{false};
    std::atomic<bool> startedAs_{false};   // a backstory was applied: select the player's person
    WheelSnap wheelCache_;
    EntityId wheelCacheFor_[2] = {kNone, kNone};
    Tick wheelCacheTick_ = 0;
    std::vector<std::string> wheelActs_;
    EntityId snapSelected_ = kNone;
    // things, acts, feedback: the main thread says where it looks and what
    // ring is open; the rest is the sim thread's
    std::atomic<double> viewX_{0}, viewY_{0};
    std::atomic<EntityId> actsWanted_{kNone};
    std::map<std::string, std::vector<std::string>> objActs_;
    EntityId fbFor_ = kNone;
    std::map<std::string, int> fbInv_;
    Tick fbTick_ = 0;
    int fbAtTick_ = 0, fbSeq_ = 0;
    std::vector<FeedbackSnap> fbTail_;
    SpeechFeed speechFeed_;   // AL-57
    Clock::time_point barkEpoch_ = Clock::now();
    int speechSeq_ = 0, jrSeq_ = 0;
    std::vector<SpeechSnap> speechTail_;
    std::vector<JournalSnap> jrTail_;

    std::mutex cmdMutex_;
    std::deque<Command> commands_;
    std::mutex snapMutex_;
    Snapshot snap_;

    // Frame state (main thread).
    Snapshot shown_;
    EntityId selected_ = kNone;
    bool follow_ = false, showChronicle_ = true, showThoughts_ = false, rmb_ = false, overUi_ = false;
    struct PickAt {
        float x = 0, y = 0;
        bool pending = false;
    } pickAt_, wheelAt_;
    float rmbAt_[2] = {0, 0};
    Hud hud_;
    double target_[2] = {0, 0};
    // Direct control: the player's person walked by WASD on this side in real
    // time (world metres), sent to the sim as `steer`; drawn from here while
    // driven, from the snapshots otherwise.
    struct Avatar {
        EntityId id = kNone;
        bool driven = false;
        double x = 0, y = 0, yaw = 0, speed = 0;
        double lastInput = -1, lastSent = -1, sentX = 0, sentY = 0;
    } avatar_;
    // How the person is led (settings: control) and what blocks a walker.
    static constexpr int kKeys = 0, kMouse = 1, kFree = 2;
    EntityId lastLed_ = kNone, prevControlled_ = kNone;
    bool pendingAct_ = false;
    std::string pendingType_;
    Json pendingArgs_;
    int leadMode_ = kKeys;
    static constexpr double kWalkerRadius = 0.3;
    int control_ = kKeys;
    EntityId woreAllFor_ = kNone;
    bool avatarRuns_ = false;
    double sentZ_ = 0;   // the ground's height where the sim last heard the walker was (climbing tires)
    collide::Obstacles obstacles_;
    std::size_t obstacleSig_ = std::size_t(-1);
    std::vector<collide::Point> walkPath_;   // a click's way, world metres, the goal last
    bool walkRun_ = false;
    double walkStuck_ = 0;
    double idleS_ = 0, speedPostedS_ = -1;   // free mode: real seconds without the player's hand
    bool idleSent_ = false;
    double simSize_ = 0;   // the sim map's side, metres
    double scriptWalk_[3] = {0, 0, 0};
    bool scriptRun_ = false;
    std::string scriptAnim_;
    bool scriptHighlight_ = true;   // AL-70 shots: "highlight": false draws people unlit by selection
    double zoom_ = 18.0, orbitYaw_ = 0.8, orbitPitch_ = 0.45;
    float vp_[16] = {};
    double eye_[3] = {};
    std::uint32_t viewW_ = 1, viewH_ = 1;

    std::vector<Instance> instances_;
    SkinnedSet people_;
    std::vector<int> maleLooks_, femaleLooks_;
    std::vector<Figure> figures_;
    struct Anim {
        int clip = -1, from = -1;
        double start = 0, fromStart = 0, switched = -10, rate = 1, fromRate = 1;
        float yaw = 0;
        bool yawSet = false;
    };
    std::map<EntityId, Anim> anims_;
    std::map<std::string, std::unique_ptr<SkinnedSet>> beasts_;
    StaticSet village_;
    StaticSet props_;                 // AL-11 stand-ins (buildings + objects), when baked
    std::vector<Placed> propsPlaced_;
    // The set the settlement's buildings come from: the stand-ins when baked, else the kit.
    const StaticSet& houseSet() const { return props_.loaded() && props_.piece("house_daub") >= 0 ? props_ : village_; }
    std::vector<Placed>& housePlaced() { return &houseSet() == &props_ ? propsPlaced_ : placed_; }
    int propPiece(const std::string& type) const {
        if (!props_.loaded()) return -1;
        auto it = propPiece_.find(type);
        if (it == propPiece_.end()) it = propPiece_.emplace(type, props_.piece(type)).first;
        return it->second;
    }
    mutable std::map<std::string, int> propPiece_;
    std::vector<Placed> placed_;
    // AL-56: the cutaway's pieces this frame and where each one's Placed is
    struct CutRef {
        bool props = false;
        std::size_t index = 0;
    };
    cutaway::Cutaway cutaway_;
    cutaway::Mode wallMode_ = cutaway::Mode::Cut;
    std::vector<cutaway::Piece> cutPieces_;
    std::vector<CutRef> cutRefs_;
    std::map<std::string, int> pieceMesh_;
    int buildPick_ = 0, buildRot_ = 0, buildBump_ = 0;
    bool hasPreview_ = false, placeWanted_ = false, removeWanted_ = false;
    BuildPiece buildPreview_;
    // AL-9: the drag of a build tool and what it lays
    bool dragStart_ = false, dragOn_ = false;
    double dragA_[2] = {0, 0};
    std::vector<BuildPiece> laid_;
    std::vector<std::string> batchWhy_;
    std::vector<EntityId> batchRemove_;
    std::string lastGable_, lastWall_;
    std::string buildWhy_;
    std::size_t clearedPieces_ = 0;
    std::map<std::string, std::vector<Figure>> beastFigures_;
    std::map<std::pair<EntityId, int>, Anim> beastAnims_;
    Clock::time_point started_ = Clock::now();
    double lastFrameS_ = 0, frameDt_ = 0;
    MeshRange meshes_[kMeshes], batch_[kMeshes];
    engine::Buffer vertBuffer_, instBuffer_;
    SDL_GPUTransferBuffer* instTransfer_ = nullptr;
    engine::GraphicsPipeline actorsPipe_, overlayPipe_;
    engine::Sampler sampler_;
    engine::Texture uiTexture_;
    SDL_GPUTransferBuffer* uiTransfer_ = nullptr;

    ui::Canvas canvas_;
    ui::Ui ui_;
    ui::Input input_;
    std::shared_ptr<ui::FontFace> font_, fontBold_;
    float uiScale_ = 0;   // AL-62: pixels per point of the interface, as set last (0: set it again)
};

} // namespace

std::unique_ptr<GameLayer> makeSimGame(engine::Device& device, SDL_Window* window, const terrain_core::MacroMap& macro,
                                       const terrain_core::Params& params, double centreX, double centreY,
                                       const GameOptions& options, std::string* why) {
    auto game = std::make_unique<SimGame>(device, window, macro, params);
    if (!game->open(centreX, centreY, options, why)) return nullptr;
    return game;
}

} // namespace altera::game
