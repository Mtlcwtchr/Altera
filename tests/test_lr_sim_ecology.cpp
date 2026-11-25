// AL-82: the wild beasts hold their numbers for years. Without people the
// predators and their prey settle into a balance instead of eating the land
// empty; with the last heads hidden and groups coming in across the edge of
// the map no species dies out for good.

#include <cstdio>
#include <cstdlib>
#include <map>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {

const Content& ecoContent() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

const Json& ecoWorldCfg() {
    static const Json j = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    return j;
}

std::map<std::string, int> headsBySpecies(const World& w) {
    std::map<std::string, int> n;
    for (const auto& [id, c] : w.characters())
        if (c.isAnimal() && c.body.alive) n[c.species] += c.group;
    return n;
}

// Only the beasts: the animal drivers per tick and the daily pass.
void runBeasts(World& w, int days, bool print) {
    for (int d = 0; d < days; ++d) {
        for (Tick t = 0; t < kTicksPerDay; ++t) {
            w.step();
            if (w.now() % kTicksPerDay == 0) animalsDaily(w);
            AnimalTick tick;
            for (const auto& [id, c] : w.characters())
                if (c.body.alive && c.isAnimal())
                    if (const SpeciesDef* sp = w.content().speciesOf(c.species); sp && sp->diet == "prey")
                        tick.predators.push_back(&c);
            for (auto& [id, c] : w.characters())
                if (c.isAnimal()) animalThink(w, c, tick);
        }
        if (print && (d + 1) % 90 == 0) {
            std::printf("  day %4d:", d + 1);
            for (const auto& [s, n] : headsBySpecies(w)) std::printf(" %s=%d", s.c_str(), n);
            std::printf("\n");
        }
    }
}

} // namespace

TEST(lr_sim_ecology_holds_without_people) {
    World w = makeTestWorld(ecoContent(), ecoWorldCfg());
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal()) c.body.alive = false;
    const auto start = headsBySpecies(w);
    const char* env = std::getenv("LR_ECO_DAYS");
    const int days = env ? std::atoi(env) : 3 * kDaysPerYear;
    runBeasts(w, days, env != nullptr);
    const auto end = headsBySpecies(w);
    for (const char* s : {"deer", "hare", "boar", "wolf"}) {
        const int a = start.count(s) ? start.at(s) : 0;
        const int b = end.count(s) ? end.at(s) : 0;
        std::printf("  %s: %d -> %d\n", s, a, b);
        CHECK(b > 0);
        // Prey holds at least a third of its start; predators keep at least one pack.
        if (std::string(s) != "wolf") CHECK(b * 3 >= a);
        else CHECK(b >= 3);
    }
}
