#pragma once
// Saved games and the game's settings on disk.
//
// A save is two files in the save folder: slot_<n>.json (the sim world as
// World::toJson has it, plus the game's own state: where the sim's map sits in
// the world, the camera, the speed) and slot_<n>.meta.json (a few lines for
// the slot list, so the list does not parse the worlds). Slot 0 is the
// autosave. The folder is ALTERA_SAVE_DIR when set, else the user's
// SDL pref path (%APPDATA%/Altera/Altera/saves on Windows); settings.json
// sits next to the saves folder.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "sim/core.hpp"

namespace altera::game {

constexpr int kSaveSlots = 6;   // manual slots 1..6; slot 0 is the autosave

struct SaveMeta {
    int slot = 0;
    bool used = false;
    std::int64_t savedAt = 0;   // unix seconds
    sim::Tick tick = 0;
    int day = 0;                // the day shown, from 1
    std::string season;         // spring | summer | autumn | winter
    std::string person;         // the player's person, or empty
    std::string backstory;      // backstory id, or empty
    int alive = 0;
};

struct GameSettings {
    std::string lang;           // empty: the command line's
    bool fullscreen = false;
    int autosaveMin = 10;       // 0: off
    int control = 0;            // how the person is led: 0 keys, 1 mouse, 2 free (Sims-like)
    bool cameraFollow = true;   // the camera rides on the selected person
    int wallMode = 1;           // AL-56: the walls in the top view, 0 up, 1 cut away, 2 down
    int idleAutonomyS = 20;     // free mode: idle seconds before the person lives by itself
};

std::filesystem::path saveDir();
std::filesystem::path slotPath(int slot);

// Every slot 0..kSaveSlots, used or not.
std::vector<SaveMeta> listSaves();
// The slot saved last, or -1.
int latestSave();

// Writes the save (through a temporary file, then a rename). False and `why` on failure.
bool writeSave(int slot, const sim::Json& save, const SaveMeta& meta, std::string* why);
// Reads slot_<n>.json. Throws on a missing or broken file.
sim::Json readSave(int slot);

GameSettings loadSettings();
void storeSettings(const GameSettings& s);

} // namespace altera::game
