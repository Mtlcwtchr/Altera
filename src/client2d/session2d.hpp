#pragma once
// AL-99: the 2D client's game session: a new game (seed, world size, season),
// who the player starts as (a person of a living family or a newcomer with a
// backstory), and the saves. Like the rest of src/client2d it knows the sim
// only through World, its commands and World::toJson / loadJson.
//
// A save is two files in the 2D save folder (the 3D game's own folder is
// apart, its saves carry the terrain and the camera of the 3D view):
//   slot_<n>.json       {format: altera.save/1, client: "2d", game: {...}, world: World::toJson}
//   slot_<n>.meta.json  a few lines for the slot list (the list never parses worlds)
// Slot 0 is the autosave, 1..kSlots2d the player's. The folder is
// LR_SAVE_DIR/saves_2d when that is set, else %APPDATA%/LivelyRealms/LivelyRealms/saves_2d
// (Windows) or $XDG_DATA_HOME (~/.local/share)/LivelyRealms/LivelyRealms/saves_2d.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "client2d/view2d.hpp"
#include "sim/world.hpp"

namespace lr::client2d {

constexpr int kSlots2d = 6;

struct NewGame {
    std::uint64_t seed = 7;
    std::string size = "medium";   // small | medium | large (worldSizes())
    std::string season;            // spring | summer | autumn | winter; empty: the world file's
    static NewGame fromJson(const sim::Json& j, std::uint64_t fallbackSeed);
    sim::Json toJson() const { return {{"seed", seed}, {"size", size}, {"season", season}}; }
};

// The sizes the new-game screen offers: [{id, size_m, families, people: [lo, hi]}].
const sim::Json& worldSizes();
// The new-game screen's choices: sizes and seasons (with names), the backstories (cards), defaults.
sim::Json newGameOptions(const sim::Content& c, const ViewText& t, const NewGame& defaults);
// The world file's config with the new game's choices put in.
sim::Json worldConfig(const sim::Json& base, const NewGame& g);
// AL-136: the character creation screen's rules (sim/charcreate creationRules) with names:
// races (name, desc, aptitudes, affinity, look options, gift cost, welcome), talents,
// flaws, nature, gift, attrs.list, the village's culture, the backstories' shifts.
sim::Json creationJson(const sim::Content& c, const ViewText& t);
// A new world, its AI installed, run `runHours` (the world starts at midnight);
// nobody is led yet.
std::unique_ptr<sim::World> makeWorld2d(const sim::Content& c, const sim::Json& base, const NewGame& g, double runHours);

// Who can be taken at the start: the families of the settlements, each with its
// dynasty and people [{id, name, sex, age, role, adult, head, chief, pl}], the
// fully simulated settlement first.
sim::Json dynastiesJson(const sim::World& w, const ViewText& t);
// The player (player 1) begins: {id} leads that person; {backstory[, id]} a
// newcomer of that backstory (on that person, else the sim's candidate);
// {create: {backstory, race, sex, age, name, lang, traits, attrs, look}} a person made
// on the creation screen (AL-136, command create_character);
// {watch: true} leads nobody. "ok" or the sim's "rejected:..." reason.
std::string beginAs(sim::World& w, const sim::Json& args);

// --- saves -------------------------------------------------------------------
std::filesystem::path saveDir2d();
// Every slot 0..kSlots2d: {slot, used, saved_at, day, season, person, family, alive, seed, size}.
sim::Json listSaves2d(const std::filesystem::path& dir);
// Writes the world and the page's game state (seed, size, camera...). False + why on failure.
// `t` names the person in the list in the player's language (null: the sim's name).
bool writeSave2d(const std::filesystem::path& dir, int slot, const sim::World& w, const sim::Json& game, std::string* why,
                 const ViewText* t = nullptr);
// The save of a slot; throws when missing or not a save.
sim::Json readSave2d(const std::filesystem::path& dir, int slot);
// A world from a save (a fresh World with the AI, then World::loadJson).
std::unique_ptr<sim::World> loadWorld2d(const sim::Content& c, const sim::Json& save);
bool deleteSave2d(const std::filesystem::path& dir, int slot);

} // namespace lr::client2d
