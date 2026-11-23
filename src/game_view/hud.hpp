#pragma once
// The game's interface, style G of the design canvas "Altera UI и HUD"
// (boards HudG, Wheel, Sheet, Kin, Annals): leather and paper panels with gold
// rings and a wax seal, Kurale for titles and Nunito for the rest, portraits
// of the people as the bake renders them, rings for vitals and needs.
//
// Immediate mode over ui::Ui on the game's software canvas. Panels are
// painted on the CPU once per size (the CSS of the canvas, by distance to
// the rounded box) and cached as textures. Every text comes from content/loc
// through the host; the layout and the data mapping live here.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "engine/ui/ui.hpp"
#include "game_view/snapshot.hpp"
#include "sim/core.hpp"
#include "terrain_core/world_gen.hpp"

namespace altera::game {

// What the HUD asks of the game that owns it.
class HudHost {
public:
    virtual ~HudHost() = default;
    virtual std::string text(const std::string& key, const sim::Json& args = sim::Json::object()) const = 0;
    virtual std::string interactionName(const std::string& id) const = 0;
    virtual void command(const std::string& type, const sim::Json& args) = 0;   // a player command to the sim
    virtual void pause() = 0;
    virtual void speed(int n) = 0;
    virtual void select(EntityId id) = 0;
    virtual void wheelTarget(EntityId id) = 0;    // which relation the snapshot should carry
    // The look a person is drawn with and their tints (skin, hair, cloth).
    virtual std::string lookOf(const ActorSnap& a) const = 0;
    virtual void tintsOf(const ActorSnap& a, float skin[3], float hair[3], float cloth[3]) const = 0;
    virtual void familyColour(const std::string& family, float out[3]) const = 0;
    // Screen position over a person's head; false off screen.
    virtual bool overHead(const ActorSnap& a, float& x, float& y) const = 0;
    virtual double groundHeight(double simX, double simY) const = 0;   // for the minimap
    virtual double tickAlpha() const = 0;
    // Goals the player can suggest or order: (id, name).
    virtual std::vector<std::pair<std::string, std::string>> goals() const = 0;
    // The workshop's draft (process, slots, target): the sim forecasts it.
    virtual void craftDraft(const sim::Json& draft) { (void)draft; }
    // Screen position `up` metres over the ground at a sim point; false off screen.
    virtual bool groundToScreen(double simX, double simY, double up, float& x, float& y) const {
        (void)simX, (void)simY, (void)up, (void)x, (void)y;
        return false;
    }
    // A menu button (hud_menu.cpp): continue, new, open_load, open_save,
    // settings, resume, main_menu, quit, load {slot}, save {slot}, back,
    // apply {lang, fullscreen, autosave}. The game sets the panel.
    virtual void menuAction(const std::string& action, const sim::Json& args) { (void)action, (void)args; }
    // The control bar by the clock: how the player's person is led (0 keys,
    // 1 mouse, 2 free: Sims-like, orders queue up and it lives by itself when
    // left alone) and whether the camera rides on the selected person; the two
    // are independent.
    virtual void controlMode(int mode) { (void)mode; }
    virtual void cameraFollow(bool on) { (void)on; }
    // AL-56: the walls in the top view, as The Sims has them: 0 up, 1 cut away, 2 down.
    virtual void wallMode(int mode) { (void)mode; }
    // AL-64: where the camera looks, radians in the sim's plane (x east, y north), for the compass.
    virtual double cameraYaw() const { return 1.5707963; }
    // AL-67: an operation of the HUD on the sim thread: say_topic {id, target,
    // index}, set_name {id, forms}; and the case forms the language suggests
    // for a typed name ({nom, gen, dat, acc, ins, pre, g}).
    virtual void simOp(const std::string& op, const sim::Json& args) { (void)op, (void)args; }
    virtual sim::Json nameCases(const std::string& name, char sex) const {
        (void)sex;
        return sim::Json{{"nom", name}};
    }
};

class Hud {
public:
    bool init(SDL_Renderer* renderer, const std::filesystem::path& root, std::string* why);
    void shutdown();
    // The whole interface for this frame. `selected` may change (a portrait clicked).
    void draw(ui::Ui& ui, ui::Input& input, const Snapshot& s, EntityId selected, HudHost& host, float width,
              float height);

    std::string panel = "none";   // none | sheet | wheel | kin | annals | build | colony | workshop
    // the workshop's draft (scripts may set it): process index, slot -> refs, target
    int craftProc = -1;
    std::map<std::string, std::vector<std::string>> craftSlots;
    std::string craftShape, craftMode, craftSlot;
    // the workshop scene (boards 10-15): heat step (-1: the most the stations
    // give), length (0: the shape's default), the helper's wish, the tab
    // (0 bench, 1 book, 2 things, 3 repair), the helper shown, E held this frame
    int craftHeat = -1, craftLen = 0, craftTab = 0;
    std::string craftWish;
    bool craftHelper = false, keyE = false;
    void closeCraftResult() { resultOpen_ = false; }   // tour scripts
    // The build mode (AL-9/10), filled by the game: the catalog of pieces
    // (index = the build.json piece), the tool, the piece picked, the state of
    // what is under the mouse, the household's plan. Buttons the bar presses
    // for the mouse alone (turn, storey up and down) count up here.
    struct BuildEntry {
        std::string id, name, cost, category;
    };
    std::vector<BuildEntry> buildCatalog;
    std::string buildMode = "room";   // room wall door window floor foundation roof gable pillar remove
    int buildPick = 0;
    std::string buildStatus, buildPlan;
    bool buildOk = true;
    int buildTurns = 0, buildStoreyUp = 0, buildStoreyDown = 0;
    static bool buildFits(const std::string& mode, const BuildEntry& e);
    void openWheel(EntityId target) {
        panel = "wheel";
        wheelTarget_ = target;
        sphere_ = 0;
        sub_ = 0;
    }
    // The act ring over a beast (a click on it): the sim's acts whose target is
    // its species, filled by the game (species -> interaction ids), drawn
    // round the point clicked.
    std::map<std::string, std::vector<std::string>> beastActs;
    // AL-62: pixels per point of the interface (the game sets it each frame);
    // openBeast / openThing take the screen's pixels.
    float scale = 1.0f;
    void openBeast(EntityId target, float x, float y) {
        panel = "beast";
        beastTarget_ = target;
        beastX_ = x / scale;
        beastY_ = y / scale;
        beastFresh_ = true;
    }
    // The act ring over a thing (a tree, deadwood, a bush): the acts the sim
    // checks for it (Snapshot::acts), each with what it needs and gives.
    void openThing(EntityId target, float x, float y) {
        panel = "thing";
        thingTarget_ = target;
        beastX_ = x / scale;
        beastY_ = y / scale;
        beastFresh_ = true;
    }
    // The target whose acts the sim should check (the ring or the wheel open on it).
    EntityId actTarget() const {
        if (panel == "wheel" || panel == "talk") return wheelTarget_;
        if (panel == "beast") return beastTarget_;
        if (panel == "thing") return thingTarget_;
        return sim::kNone;
    }
    // What the mouse is over in the world (the game fills it): the hint.
    EntityId hoverActor = sim::kNone, hoverThing = sim::kNone;
    void turnDrag() { dragTurned_ = !dragTurned_; }   // R in the inventory: the dragged thing turned
    EntityId aim = sim::kNone;   // the target the player clicked (marked, its name shown)
    // Filled by the game every frame: the control bar's state, the speed held
    // at 1 (walking by the keys), where a click sent the person (sim metres).
    int controlMode = 0;
    int wallMode = 1;   // AL-56: 0 up, 1 cut away, 2 down
    bool cameraFollow = false, speedLocked = false, hasPerson = false;
    std::string leadName;   // whom keys / mouse would take under control (nobody led)
    bool walkMark = false;
    double walkMarkX = 0, walkMarkY = 0;
    void setTone(int t) { tone_ = t; }
    // AL-67: the name screen for a person (panel "name"; scripts may open it), the talk topics (panel "talk")
    EntityId nameFor = sim::kNone;
    void openName(EntityId id) {
        panel = "name";
        nameFor = id;
        nameNom_.clear();
        nameTyped_.clear();
        nameCases_.clear();
        nameEdited_.clear();
    }
    void openTalk(EntityId target) {
        panel = "talk";
        wheelTarget_ = target;
    }   // the wheel's tone chip (tour scripts)
    void openSlot(int k) { pin_ = k; }   // open a status group's panel as the hover does (tour scripts; -1 closes)
    bool showThoughts = false;
    int socialKey = 0;   // AL-65, set by the game's keys: 1 Y (yes), -1 N (no), 2 X (let the followers go)
    // The new game's backstory screen (panel "backstory"), filled by the game
    // from content/ai/backstories.json, its loc and content/ui/backstory.json.
    struct BackstoryThing {
        std::string icon, name;
        int n = 1;
    };
    struct BackstoryCard {
        std::string id, icon, name, desc, life, start, startIcon;
        std::vector<std::pair<std::string, double>> skills;   // name, level 0..1
        std::vector<std::string> traits, pool, recipes, rights;
        int pick = 0, coins = 0;
        std::vector<std::pair<std::string, double>> attrs;    // name, shift
        std::vector<BackstoryThing> gear;
    };
    std::vector<BackstoryCard> backstories;
    int backstoryPick = 0;
    bool backstoryBegin = false;   // Enter or a script: begin with the picked one
    std::string coinIcon = "values";

    // The menus (hud_menu.cpp): panels "menu" (the main menu), "pause",
    // "slots" (save or load) and "settings", filled by the game.
    struct MenuSlot {
        int slot = 0;
        bool used = false;
        std::string title, line, when;
    };
    std::vector<MenuSlot> menuSlots;
    bool slotsSave = false;          // the slot panel saves (else it loads)
    bool canContinue = false;        // a game is on, or a save exists
    bool canLoad = false;            // a save exists
    std::string menuNote;            // a line under the menu: saved, or what went wrong
    std::string settingsLang = "ru";
    bool settingsFullscreen = false;
    int settingsAutosave = 10;       // minutes, 0 off
    // AL-62: the interface style (a theme in content/ui/themes) and its size, 80..130 %
    std::string themeId = "edge";    // the theme init() loads
    std::vector<std::pair<std::string, std::string>> themeList;   // (id, loc key of its name)
    std::string settingsStyle = "edge";
    int settingsScale = 100;
    bool inMenu() const {
        return panel == "menu" || panel == "pause" || panel == "slots" || panel == "settings" || panel == "world";
    }
    // AL-69: the world being made (panel "world", hud_world.cpp); the seed as typed
    terrain_core::WorldSpec worldDraft;
    std::string worldSeedText;
    // A new world (a load or a new game): drop what belonged to the old one's people.
    void resetWorld();

private:
    struct Image {
        int w = 0, h = 0;
        std::vector<std::uint8_t> rgba;
    };
    enum class Skin { Leather, Paper, Ring, Button, ButtonOn, Seal, Chip, ChipOn, Disc };

    // painting
    SDL_Texture* skin(Skin kind, int w, int h, float radius, std::uint32_t colour = 0);
    void box(Skin kind, const ui::Rect& r, float radius, std::uint32_t colour = 0);
    void ring(float cx, float cy, float r);   // the gold ring round a portrait of radius r
    void arc(float cx, float cy, float r, float width, float startDeg, float sweepDeg, const ui::Colour& c);
    void disc(float cx, float cy, float r, const ui::Colour& c);
    void icon(const std::string& name, const ui::Rect& r, const ui::Colour& c);
    void portrait(const ActorSnap& a, float cx, float cy, float r);
    SDL_Texture* portraitOf(const ActorSnap& a);
    void title(float x, float y, const std::string& s, const ui::Colour& c, float px);
    void label(float x, float y, const std::string& s, const ui::Colour& c, float px = 15, bool shadow = false);
    bool clicked(const ui::Rect& r);
    bool roundButton(const ui::Rect& r, const std::string& iconName, bool on, const std::string& tip = "");
    bool textButton(const ui::Rect& r, const std::string& s, bool dark);
    void seal(float cx, float cy);
    float textW(const std::string& s, float px, bool titleFace = false) const;

    // the Edge style's primitives (hud_edge.cpp, AL-63)
    static std::string upper(const std::string& s);
    void quad(const SDL_FPoint p[4], const ui::Colour c[4]);
    void thickLine(float x0, float y0, float x1, float y1, float w, const ui::Colour& c);
    void gradRect(const ui::Rect& r, const ui::Colour& a, const ui::Colour& b, bool vertical);
    // a diamond of side `size` at (cx, cy): glass (or `fill`), the gold contour, lit: glowing, twice: a second contour
    void diamond(float cx, float cy, float size, bool lit = false, bool twice = false, const ui::Colour* fill = nullptr);
    bool diamondButton(float cx, float cy, float size, const std::string& iconName, bool on, const std::string& tip = "",
                       bool dim = false);
    void frameBar(const ui::Rect& r, float frac, const ui::Colour& fill);                   // a thin meter, gold contour
    void filiBar(const ui::Rect& r, float frac, const ui::Colour& fill, const std::string& iconName);   // medallion and filigree
    void glass(const ui::Rect& r, bool framed = false);
    void selRow(const ui::Rect& r);                                                         // gradient, tick, bracket
    float keyCap(float x, float y, const std::string& key, float px = 13);                  // returns its width
    void tick(float x, float y, float h, const ui::Colour& c);
    float capsW(const std::string& s, float px, float spacing = -1, bool titleFace = true) const;
    void caps(float x, float y, const std::string& s, const ui::Colour& c, float px, float spacing = -1,
              bool titleFace = true, bool shadow = false);
    bool edgeButton(const ui::Rect& r, const std::string& s, bool primary = false, bool enabled = true);

    // parts of the screen
    void names(const Snapshot& s);
    void topMenu(float W);
    void minimap(const Snapshot& s, float W);
    void party(const Snapshot& s, const ActorSnap* sel);
    void selectedCorner(const Snapshot& s, const ActorSnap& a, float H);
    void slotCard(const Snapshot& s, const ActorSnap& a, int shown, float x, float bottom, float caret, float width);
    // the Edge HUD (hud_edge_hud.cpp, AL-64)
    void edgeHud(const Snapshot& s, const ActorSnap* sel, float W, float H);
    void edgeCompass(const Snapshot& s, const ActorSnap* sel, float ox, float band);
    void edgeMenu(float ox, float band);
    void edgeFamily(const Snapshot& s, const ActorSnap* sel, float ox);
    void edgeTracker(const Snapshot& s, float ox, float band);
    void edgeVitals(const Snapshot& s, const ActorSnap& a, float ox, float H);
    void edgeQueue(const Snapshot& s, float ox, float H);
    void edgeTarget(const Snapshot& s, const ActorSnap* sel, float ox, float band, float H);
    void edgeClock(const Snapshot& s, float ox, float band, float H);
    void edgeSkillToast(const Snapshot& s, const ActorSnap* sel, float ox, float band);
    // AL-65: an NPC's ask (yes / no, its time running out) and the followers' chip
    void socialAsk(const Snapshot& s, float W, float H);
    void followerChip(const Snapshot& s, float x, float y);
    // AL-67: the name screen with its cases, the talk topics, the case card
    void namePanel(const Snapshot& s, float W, float H);
    void talkPanel(const Snapshot& s, float W, float H);
    void caseCard(const Snapshot& s, float x, float y, float w);
    std::string nameNom_, nameTyped_;
    std::map<std::string, std::string> nameCases_;
    std::map<std::string, bool> nameEdited_;
    bool compact_ = false;       // a small screen: fewer lines, tighter rows
    float familyBottom_ = 40;    // where the family list ended (the follower chip goes under it)
    struct SkillToast {
        std::string skill;
        int step = 0;
        double born = 0;
    };
    std::vector<SkillToast> skillToasts_;
    std::map<std::string, int> skillStep_;
    EntityId skillWho_ = sim::kNone;
    void quickBar(const Snapshot& s, float W, float H);
    void clockPill(const Snapshot& s, float W, float H);
    void controlPill(float W, float H);
    void walkMarker();
    void toasts(const Snapshot& s, float W);
    void thoughts(const Snapshot& s, float W);
    void sheet(const Snapshot& s, const ActorSnap& a, float W, float H);
    void wheel(const Snapshot& s, float W, float H);
    void beast(const Snapshot& s, float W, float H);
    void thing(const Snapshot& s, float W, float H);
    void actMenu(const Snapshot& s, EntityId target, const std::string& head, const std::vector<std::string>& sub,
                 const std::vector<std::string>& acts, float W, float H);
    // What an act needs and gives (the sim's check), on a paper card at x, y.
    void actCard(const ActInfoSnap& a, float x, float y, float W, float H);
    const ActInfoSnap* actInfo(const Snapshot& s, EntityId target, const std::string& id) const;
    void hoverHint(const Snapshot& s, float W);
    void feedbackFloats(const Snapshot& s);
    void speechBubbles(const Snapshot& s);              // AL-57: lines over the speakers
    void journalBox(const Snapshot& s, float W, float H); // AL-57: the player's journal
    void workFx(const Snapshot& s);
    void kin(const Snapshot& s, const ActorSnap* a, float W, float H);
    void annals(const Snapshot& s, float W, float H);
    void buildBar(const Snapshot& s, float W, float H);
    void colony(const Snapshot& s, float W, float H);
    void workshop(const Snapshot& s, float W, float H);
    void inventory(const Snapshot& s, const ActorSnap* sel, float W, float H);
    std::string itemIcon(const std::string& id) const;
    // the thing dragged in the inventory: what, its size, turned
    std::string dragItem_, dragFrom_;
    int dragW_ = 1, dragH_ = 1;
    bool dragTurned_ = false;
    void backstory(float W, float H);
    // the menus (hud_menu.cpp)
    void mainMenu(float W, float H);
    void pauseMenu(float W, float H);
    void slotsPanel(float W, float H);
    void settingsPanel(float W, float H);
    // AL-69: the making of a new world, with its map drawn on a worker thread
    void worldPanel(float W, float H);
    struct WorldPreview {
        std::string key;
        std::vector<std::uint8_t> rgba;
        int size = 0;
        double land = 0, peak = 0;
        bool passed = false;
    };
    std::future<WorldPreview> worldJob_;
    std::string worldJobKey_;
    WorldPreview worldShown_;
    SDL_Texture* worldTex_ = nullptr;
    bool menuButton(const ui::Rect& r, const std::string& iconName, const std::string& s, bool enabled = true);
    int pendingSlot_ = -1;           // a used slot clicked once in the save panel: the next click overwrites
    sim::Json craftDraftJson(const Snapshot& s) const;
    std::string sentDraft_;
    // the workshop's parts (hud.cpp, after the design canvas boards 10-15)
    bool iconButton(const ui::Rect& r, const std::string& iconName, bool on, const std::string& tip, bool dim = false);
    void tipAt(float cx, float y, const std::string& tip);
    void drawTip(float W, float H);
    void craftBench(const Snapshot& s, float W, float H);
    void craftHelperView(const Snapshot& s, float W, float H);
    void craftBook(const Snapshot& s, float W, float H);
    void craftThings(const Snapshot& s, float W, float H);
    void craftRepair(const Snapshot& s, float W, float H);
    void craftResultCard(const Snapshot& s, float W, float H);
    void craftAttempt(const Snapshot& s);
    std::string craftIconOf(const char* map, const std::string& id, const char* fallback) const;
    sim::Json craftSpec_;
    std::string dragRef_;          // a material carried from the bag
    float holdT_ = 0;              // how long "make" has been held
    int resultShown_ = -1, bookPick_ = 0, bookCount_ = 1, thingPick_ = -1, repairPick_ = -1, bagOff_ = 0;
    bool resultOpen_ = false, renaming_ = false, holdLatch_ = false;
    std::string renameText_, tipText_;
    float tipX_ = 0, tipY_ = 0, dragX_ = 0, dragY_ = 0;
    double craftClock_ = 0;
    float craftDt_ = 0;
    std::map<std::string, std::pair<float, float>> stationPos_;   // where each station is drawn this frame
    sim::Json lastDraft_;          // the last attempt, for "repeat"
    void dimmer(float W, float H, float a);

    struct Row {
        std::string label;
        double value = -1;   // 0..1, or -1: no bar
        std::string note;
        bool alarm = false;
        std::string id;      // status id (need id, health, kin, repute, respect): trend, cause and help
        int arrow = 2;       // -1 falling, 0 holding, 1 rising, 2 none
        std::string desc;    // the line under the bar: why it is low and what helps, or "fine"
    };
    // Trend of every status of the selected person, sampled each game hour.
    struct TrendSample {
        Tick tick = 0;
        double value = 0, rate = 0; // rate per day
        bool known = false;
    };
    mutable std::map<std::string, TrendSample> trends_;
    void statusRow(const Snapshot& s, const ActorSnap& a, Row& r) const;
    void trendArrow(float cx, float cy, int dir, const ui::Colour& c);
    std::vector<Row> tierRows(const Snapshot& s, const ActorSnap& a, int tier) const;
    double tierValue(const Snapshot& s, const ActorSnap& a, int tier) const;
    const ActorSnap* actor(const Snapshot& s, EntityId id) const;
    std::string dayPart(Tick t) const;

    ui::Ui* ui_ = nullptr;
    ui::Input* in_ = nullptr;
    HudHost* host_ = nullptr;
    SDL_Renderer* sdl_ = nullptr;
    std::filesystem::path root_;
    Image leather_, paper_;
    SDL_Texture* icons_ = nullptr;
    SDL_Texture* previews_ = nullptr;   // AL-10: build_previews.png (tools/game_assets/make_build_previews.py)
    int previewCell_ = 112;
    std::map<std::string, int> previewIndex_;
    int iconCell_ = 48;
    std::map<std::string, int> iconIndex_;
    std::map<std::string, SDL_Texture*> skins_;
    struct PortraitBase {
        Image picture, mask;
    };
    std::map<std::string, PortraitBase> portraitBases_;
    std::map<EntityId, SDL_Texture*> portraits_;
    SDL_Texture* minimap_ = nullptr;
    sim::Json wheelSpec_;
    EntityId selected_ = sim::kNone, wheelTarget_ = sim::kNone, beastTarget_ = sim::kNone, thingTarget_ = sim::kNone;
    struct Float {
        EntityId who = sim::kNone;
        std::string text;
        int kind = 0;
        double born = 0;
    };
    std::vector<Float> floats_;
    int floatSeq_ = 0;
    struct Bubble {
        EntityId who = sim::kNone;
        std::string text;
        double born = 0, life = 0;
    };
    std::vector<Bubble> bubbles_;            // AL-57: one per speaker, the newest line
    std::vector<Bubble> bubbleQueue_;        // AL-73: lines waiting their turn (born = when shown)
    double bubbleNext_ = 0;                  // AL-73: the earliest a next line of a talk shows
    int speechSeq_ = 0;
    std::map<EntityId, float> bubbleTop_;    // this frame: where a speaker's bubble ends (screen y)
    bool journalOpen_ = true;
    int journalSeen_ = 0;                    // the newest row shown while open
    float beastX_ = 0, beastY_ = 0;
    bool beastFresh_ = false;   // opened by this frame's click: that click does not close it
    int goalPick_ = -1;
    int pin_ = -1, hover_ = -1, sheetTab_ = 0, sphere_ = 0, sub_ = 0, tone_ = 1, annalsTab_ = 0, annalsFilter_ = 0;
    EntityId kinPick_ = sim::kNone;
    std::size_t seenChronicle_ = 0;
    struct Toast {
        std::string text;
        double born = 0;
    };
    std::vector<Toast> toasts_;
    double clock_ = 0;
    SnapClock::time_point started_ = SnapClock::now();
    std::uint64_t idSeed_ = 0;
};

} // namespace altera::game
