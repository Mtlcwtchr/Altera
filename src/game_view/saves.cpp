#include "game_view/saves.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <system_error>

#include <SDL3/SDL.h>

namespace fs = std::filesystem;

namespace altera::game {
namespace {

fs::path baseDir() {
    if (const char* d = std::getenv("ALTERA_SAVE_DIR"); d && *d) return fs::u8path(d);
    if (char* p = SDL_GetPrefPath("Altera", "Altera")) {
        fs::path r = fs::u8path(p);
        SDL_free(p);
        return r;
    }
    return fs::current_path() / "saves";
}

fs::path metaPath(int slot) { return saveDir() / ("slot_" + std::to_string(slot) + ".meta.json"); }

bool writeFile(const fs::path& path, const std::string& text, std::string* why) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            if (why) *why = "cannot write " + tmp.string();
            return false;
        }
        f.write(text.data(), std::streamsize(text.size()));
        if (!f) {
            if (why) *why = "cannot write " + tmp.string();
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        if (why) *why = "cannot replace " + path.string() + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

sim::Json readJson(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + path.string());
    return sim::Json::parse(f);
}

} // namespace

fs::path saveDir() { return baseDir() / "saves"; }

fs::path slotPath(int slot) { return saveDir() / ("slot_" + std::to_string(slot) + ".json"); }

std::vector<SaveMeta> listSaves() {
    std::vector<SaveMeta> out;
    for (int s = 0; s <= kSaveSlots; ++s) {
        SaveMeta m;
        m.slot = s;
        std::error_code ec;
        if (fs::exists(slotPath(s), ec) && fs::exists(metaPath(s), ec)) {
            try {
                const sim::Json j = readJson(metaPath(s));
                m.used = true;
                m.savedAt = j.value("saved_at", std::int64_t{0});
                m.tick = j.value("tick", sim::Tick{0});
                m.day = j.value("day", 0);
                m.season = j.value("season", "");
                m.person = j.value("person", "");
                m.backstory = j.value("backstory", "");
                m.alive = j.value("alive", 0);
            } catch (const std::exception& e) {
                SDL_Log("altera_game: save slot %d: %s", s, e.what());
            }
        }
        out.push_back(m);
    }
    return out;
}

int latestSave() {
    int best = -1;
    std::int64_t at = -1;
    for (const auto& m : listSaves())
        if (m.used && m.savedAt > at) {
            at = m.savedAt;
            best = m.slot;
        }
    return best;
}

bool writeSave(int slot, const sim::Json& save, const SaveMeta& meta, std::string* why) {
    const sim::Json mj{{"format", "altera.save.meta/1"}, {"slot", slot},          {"saved_at", meta.savedAt},
                       {"tick", meta.tick}, {"day", meta.day}, {"season", meta.season}, {"person", meta.person},
                       {"backstory", meta.backstory},    {"alive", meta.alive}};
    // the world first: a meta file without its world is never listed
    return writeFile(slotPath(slot), save.dump(), why) && writeFile(metaPath(slot), mj.dump(1), why);
}

sim::Json readSave(int slot) {
    sim::Json j = readJson(slotPath(slot));
    if (j.value("format", "") != "altera.save/1") throw std::runtime_error("not an altera.save/1 file");
    return j;
}

GameSettings loadSettings() {
    GameSettings s;
    try {
        const sim::Json j = readJson(baseDir() / "settings.json");
        s.lang = j.value("lang", "");
        s.fullscreen = j.value("fullscreen", false);
        s.autosaveMin = j.value("autosave_min", 10);
        // AL-29: the settings offer off / 5 / 10 / 15 min: an odd value snaps to the nearest
        if (s.autosaveMin > 0) s.autosaveMin = s.autosaveMin <= 7 ? 5 : (s.autosaveMin <= 12 ? 10 : 15);
        s.control = std::clamp(j.value("control", 0), 0, 2);
        s.cameraFollow = j.value("camera_follow", true);
        s.idleAutonomyS = std::clamp(j.value("idle_autonomy_s", 20), 3, 600);
    } catch (const std::exception&) {
    }
    return s;
}

void storeSettings(const GameSettings& s) {
    const sim::Json j{{"lang", s.lang}, {"fullscreen", s.fullscreen}, {"autosave_min", s.autosaveMin},
                      {"control", s.control}, {"camera_follow", s.cameraFollow}, {"idle_autonomy_s", s.idleAutonomyS}};
    std::string why;
    if (!writeFile(baseDir() / "settings.json", j.dump(1), &why)) SDL_Log("altera_game: settings: %s", why.c_str());
}

} // namespace altera::game
