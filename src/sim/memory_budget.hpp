#pragma once
// AL-83 memory budget: what one person may keep (memories, the recipe book, the
// craft novelty counters) and what the world keeps of talk (SaidMemory). The
// world has a total; each person gets total / people, clamped to [min, max], so
// the more people, the less each keeps. A daily pass trims to the budget:
// memories by salience (oldest first among equals), recipes by worth (masters,
// breakthroughs and given designs are kept), novelty counters by count.
// Data: content/ai/memory_budget.json (cached per content dir, defaults below).
//
// Save form (sharedMemoryJson / expandSharedMemory): what several people
// remember of the same event (tick, type, who, object, fact, goods, prop) is
// written once in "memory_events"; each person keeps only an index and what is
// his (valence, salience, source, confidence, chain).

#include <cstddef>

#include "sim/core.hpp"
#include "sim/craft/craft_data.hpp"

namespace lr::sim {

class World;
struct Content;
struct Character;

struct MemoryBudget {
    struct Pool {
        int total = 0;     // for the whole world
        int perMin = 0;    // a person never gets less
        int perMax = 0;    // ...nor more
        int per(int people) const;
    };
    Pool memories{40000, 40, 200};
    Pool recipes{4000, 16, 120};
    Pool tried{8000, 32, 240};
    Pool said{60000, 200, 2000};   // SaidMemory: per person x people = the world cap
    int saidKeepDays = 90;
    int maxRecipeSteps = 24;       // a longer chain is not written to the book at all
    static MemoryBudget fromJson(const Json& j);
};

const MemoryBudget& memoryBudget(const Content& c);

// Living people (not animals): the divisor of the totals.
int budgetPeople(const World& w);

struct BudgetTrim {
    std::size_t memories = 0, recipes = 0, tried = 0, said = 0;
};
// Trims one person to the per-person caps for `people`.
BudgetTrim trimToBudget(const MemoryBudget& b, const craft::CraftConfig& cfg, int people, Character& c);
// The daily pass over the whole world (installed by installAi).
BudgetTrim budgetDaily(World& w);

// Save form: the "characters" array with shared memory events pulled out.
Json sharedMemoryJson(Json characters, Json& eventsOut);
// Back: puts the shared parts into each memory (no-op without a table).
void expandSharedMemory(Json& characters, const Json& events);

} // namespace lr::sim
