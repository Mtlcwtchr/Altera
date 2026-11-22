// altera_world_gen - a procedural world (AL-69) into its folder without the game:
//
//   altera_world_gen [--spec JSON] [--out DIR] [--fresh]
//
// The folder DIR/<spec key>/ gets height.r16, biomes.png, preview.png,
// report.json (the numbers and the checks) and spec.json - the files the game
// opens the world from. --fresh makes them again even when they are there.

#include "terrain_core/world_gen.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    std::string specText = "{}";
    fs::path out = "terrain_lab_out/worlds";
    bool fresh = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--spec" && i + 1 < argc) specText = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--fresh") fresh = true;
        else {
            std::fprintf(stderr, "usage: altera_world_gen [--spec JSON] [--out DIR] [--fresh]\n");
            return 2;
        }
    }
    terrain_core::WorldSpec spec;
    std::string why;
    if (!terrain_core::parseWorldSpec(specText, spec, &why)) {
        std::fprintf(stderr, "%s\n", why.c_str());
        return 2;
    }
    if (fresh) {
        std::error_code ec;
        fs::remove_all(out / spec.key(), ec);
    }
    const auto start = std::chrono::steady_clock::now();
    const char* last = "";
    fs::path height, biomes;
    const bool ok = terrain_core::cachedWorld(spec, out, height, biomes, [&](const char* stage, float) {
        if (std::strcmp(stage, last) != 0) {
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::printf("%6.1f s  %s\n", s, stage);
            last = stage;
        }
    }, &why);
    if (!ok) {
        std::fprintf(stderr, "world: %s\n", why.c_str());
        return 1;
    }
    std::ifstream in(out / spec.key() / "report.json");
    std::stringstream ss;
    ss << in.rdbuf();
    std::printf("%s\n%s\n", (out / spec.key()).string().c_str(), ss.str().c_str());
    return ss.str().find("\"passed\": true") != std::string::npos ? 0 : 3;
}
