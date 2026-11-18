// altera_sim_runner - headless runs of the Altera life-sim core.
//   altera_sim_runner [--content DIR] [--world FILE] [--seed N] [--days N]
//                     [--save FILE] [--report FILE] [--trace FILE] [--validate]
//                     [--loc DIR] [--lang ru|en]
// Trace and report carry codes + params; with a loc table their "text" fields
// are assembled in --lang (doc/design/LOC_FORMAT.md).

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

#include "sim/ai.hpp"
#include "sim/loc.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;

int main(int argc, char** argv) {
    std::string root = ALTERA_SIM_SOURCE_DIR;
    std::string contentDir = root + "/content/ai";
    std::string worldFile = root + "/content/sim/test_world.json";
    std::string locDir = root + "/content/loc";
    std::string lang = "ru";
    std::string saveFile, reportFile, traceFile;
    std::uint64_t seed = 0;
    int days = 3;
    bool validateOnly = false;
    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << argv[i] << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (!std::strcmp(argv[i], "--content")) contentDir = next();
        else if (!std::strcmp(argv[i], "--world")) worldFile = next();
        else if (!std::strcmp(argv[i], "--seed")) seed = std::stoull(next());
        else if (!std::strcmp(argv[i], "--days")) days = std::stoi(next());
        else if (!std::strcmp(argv[i], "--save")) saveFile = next();
        else if (!std::strcmp(argv[i], "--report")) reportFile = next();
        else if (!std::strcmp(argv[i], "--trace")) traceFile = next();
        else if (!std::strcmp(argv[i], "--validate")) validateOnly = true;
        else if (!std::strcmp(argv[i], "--loc")) locDir = next();
        else if (!std::strcmp(argv[i], "--lang")) lang = next();
        else {
            std::cerr << "unknown argument " << argv[i] << "\n";
            return 2;
        }
    }
    try {
        const Content content = Content::load(contentDir);
        for (const auto& w : content.warnings) std::cerr << "warning: " << w << "\n";
        const Loc loc = Loc::load(locDir);
        const ValidationReport lv = validateLoc(content, loc);
        for (const auto& w : lv.warnings) std::cerr << "warning: " << w << "\n";
        for (const auto& e : lv.errors) std::cerr << "error: " << e << "\n";
        if (!lv.ok()) return 1;
        if (!loc.hasLanguage(lang))
            std::cerr << "warning: no loc table for '" << lang << "', using " << loc.fallback() << "\n";
        if (validateOnly) {
            std::cout << "content ok: " << content.goals.size() << " goals, " << content.tasks.size() << " tasks, "
                      << content.interactions.size() << " interactions, " << content.dialogue.size()
                      << " dialogue templates; loc ok: " << requiredLocKeys(content).size() << " keys, base "
                      << loc.base() << "\n";
            return 0;
        }
        World world = makeTestWorld(content, loadJsonFile(worldFile), seed);
        installAi(world);
        world.run(static_cast<Tick>(days) * kTicksPerDay);
        Json report = aiReport(world);
        localizeReport(report, loc, lang, world);
        std::cout << "alive " << report["alive"] << "/" << report["population"] << "\n"
                  << "why_hunger: " << report["summary"]["why_hunger"].dump() << "\n"
                  << "why_idle: " << report["summary"]["why_idle"].dump() << "\n"
                  << "plan_failures: " << report["summary"]["plan_failures"].dump() << "\n";
        if (!reportFile.empty()) std::ofstream(reportFile) << report.dump(1);
        if (!traceFile.empty()) {
            Json t = Json::object();
            for (const auto& [id, c] : world.characters()) {
                Json lines = Json::array();
                for (const auto& e : c.ai.trace)
                    lines.push_back({{"tick", e.tick}, {"layer", std::string(1, e.layer)}, {"code", e.code},
                                     {"params", e.params}, {"text", traceText(loc, lang, world, c, e)}});
                t[std::to_string(id) + " " + c.name] = lines;
            }
            std::ofstream(traceFile) << t.dump(1);
        }
        std::cout << "seed " << world.seed() << ", day " << world.day() << ", " << world.characters().size()
                  << " characters, hash " << std::hex << world.stateHash() << std::dec << "\n";
        if (!saveFile.empty()) {
            std::ofstream(saveFile) << world.toJson().dump(1);
            std::cout << "saved " << saveFile << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
