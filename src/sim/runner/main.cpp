// altera_sim_runner - headless runs of the Altera life-sim core.
//   altera_sim_runner [--content DIR] [--world FILE] [--seed N] [--days N]
//                     [--save FILE] [--report FILE] [--trace FILE] [--validate]

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

#include "sim/ai.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;

int main(int argc, char** argv) {
    std::string root = ALTERA_SIM_SOURCE_DIR;
    std::string contentDir = root + "/content/ai";
    std::string worldFile = root + "/content/sim/test_world.json";
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
        else {
            std::cerr << "unknown argument " << argv[i] << "\n";
            return 2;
        }
    }
    try {
        const Content content = Content::load(contentDir);
        for (const auto& w : content.warnings) std::cerr << "warning: " << w << "\n";
        if (validateOnly) {
            std::cout << "content ok: " << content.goals.size() << " goals, " << content.tasks.size() << " tasks, "
                      << content.interactions.size() << " interactions\n";
            return 0;
        }
        World world = makeTestWorld(content, loadJsonFile(worldFile), seed);
        installAi(world);
        world.run(static_cast<Tick>(days) * kTicksPerDay);
        const Json report = aiReport(world);
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
                                     {"params", e.params}});
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
