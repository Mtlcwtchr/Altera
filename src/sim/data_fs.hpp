#pragma once
// The data layers (LR-142, doc/design/DATA_DRIVEN.md §2): every content file the sim
// and the 2D client read goes through here, so a built game picks up JSON dropped
// next to it without a rebuild.
//
//   base    the content/ folder: LR_DATA (env) > <exe>/data/content > <exe>/content >
//           content/ in a parent of the exe (a build tree in the repo) > the source tree
//   layers  mod folders over it, each mirroring content/ (mods/<name>/ai/items.json
//           overlays content/ai/items.json): <base>/../mods/* and <exe>/mods/*, in
//           mods/load_order.json order when there is one, else by name; LR_MODS adds
//           more (paths split by ';').
//
// A layer's file replaces the one below it, unless it is a JSON object with
// "_merge": true: then it is merged in (objects key by key, arrays of objects with an
// "id" by id - same id merges, new id joins, {"id": x, "_remove": true} drops it -
// other values replace). A directory lists the union of every layer's files.
//
// Paths outside the base are read as they are. generation() counts reloads; the
// modules' caches key on it, so after reload() everything reads fresh.

#include <cstdint>
#include <filesystem>
#include <ios>
#include <sstream>
#include <functional>
#include <string>
#include <vector>

#include "sim/core.hpp"

namespace lr::sim::data {

// Where content/ is for an executable at `exePath` (argv[0] or the real path); `fallback`
// is the source tree's content/ baked in at build time. Never empty.
std::string findContentRoot(const std::string& exePath, const std::string& fallback);
// Mod folders under the given roots (each a folder of mods), in load order.
std::vector<std::string> discoverMods(const std::vector<std::string>& modRoots);

// Sets the base and its layers (lowest first). Paths are made absolute. Clears the read log.
void mount(const std::string& contentRoot, const std::vector<std::string>& layers = {});
// mount() with findContentRoot + discoverMods + LR_MODS, as a game or tool starts.
// Returns the content root.
std::string mountDefault(const std::string& exePath, const std::string& fallback);
const std::string& contentRoot(); // empty until mounted
const std::vector<std::string>& layers();

// The real file a content path names: the top layer that has it, else the path itself.
std::string resolve(const std::string& path);
bool exists(const std::string& path);
bool isDir(const std::string& path);
// Text of the top layer's file; false when no layer has it.
bool readText(const std::string& path, std::string& out);
// The layered JSON (see above). Throws std::runtime_error naming the file on a parse
// error, and when no layer has the file.
Json readJson(const std::string& path);
// readJson, or `dflt` when no layer has the file (a parse error still throws).
Json readJsonOr(const std::string& path, Json dflt);
// File names (not paths) in `dir` over every layer, sorted; `ext` filters ("" = all files).
std::vector<std::string> listFiles(const std::string& dir, const std::string& ext = ".json");
std::vector<std::string> listDirs(const std::string& dir);

// The JSON merge of a "_merge" layer, exposed for tools and tests.
void mergeJson(Json& base, const Json& over);

// Hot reload: every file and directory read since mount() is watched. changed() lists the
// files whose stamp moved (edited, added or removed in any layer) since the last call;
// reload() bumps the generation so caches read fresh, and calls the listeners.
std::vector<std::string> changed();
std::uint64_t generation();
void reload();
// Back to an earlier generation (a reload whose new data did not load).
void restoreGeneration(std::uint64_t gen);
// A module's cache key for `dir`: dir + the generation.
std::string cacheKey(const std::string& dir);
// Called after reload() (the 2D client re-reads its JSON there). Returns an id.
int onReload(std::function<void()> fn);

// Drop-in for `std::ifstream f(path)` over a content JSON file: the layered JSON as
// text (so Json::parse(f), f >> j and `if (!f)` keep working). A layer that does not
// parse leaves the stream failed (the reader keeps its defaults or throws its own
// error), names the file in error() and on stderr; tools/validate_content.py reports it.
class InFile : public std::istringstream {
public:
    explicit InFile(const std::filesystem::path& p, std::ios::openmode = std::ios::in);
    bool is_open() const { return !fail(); }
    const std::string& error() const { return error_; } // a layer's parse error ("" = none)

private:
    std::string error_;
};
inline bool exists(const std::filesystem::path& p) { return exists(p.string()); }
inline bool isDir(const std::filesystem::path& p) { return isDir(p.string()); }
// The paths (dir / name) of listFiles(dir, ext), sorted by name.
inline std::vector<std::filesystem::path> listPaths(const std::filesystem::path& dir, const std::string& ext = ".json") {
    std::vector<std::filesystem::path> out;
    for (const auto& n : listFiles(dir.string(), ext)) out.push_back(dir / n);
    return out;
}

} // namespace lr::sim::data
