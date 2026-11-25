// lr_craft_report: outcome tables of the starter designs by skill, and a
// 30-day village smith (gdd crafting v0.2 §15, K2 readiness). Markdown on stdout.
//   lr_craft_report [--n 60] [--seed 1]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "sim/craft/craft_chain.hpp"

using namespace lr::sim::craft;

namespace {

Workplace shop(const CraftData& d) {
    Workplace wp;
    for (const auto& [id, s] : d.stations) wp.stations.push_back(id);
    Tool t;
    t.tags = {"cutting", "saw", "striking", "tongs", "whetstone", "grit", "needle", "bellows"};
    t.hardness = 0.85;
    t.q = 0.6;
    wp.tools.push_back(t);
    return wp;
}

Actor actor(const CraftData& d, double skill, std::uint64_t id) {
    Actor a;
    a.id = id;
    for (const auto& [pid, p] : d.processes) {
        a.skills[p.skill] = skill;
        for (const auto& [tag, sk] : p.skillBy) a.skills[sk] = skill;
    }
    for (const auto& [m, md] : d.materials) a.knowsMaterial[m] = skill;
    return a;
}

bool flawed(const Item& it) {
    if (!it.flaws.empty()) return true;
    for (const auto& p : it.parts)
        if (flawed(p.item.front())) return true;
    return false;
}

} // namespace

int main(int argc, char** argv) {
    int n = 60;
    std::uint64_t seed = 1;
    for (int i = 1; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--n")) n = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--seed")) seed = std::strtoull(argv[++i], nullptr, 10);
    }
    const CraftData d = CraftData::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai/craft");
    if (!d.loaded || !d.errors.empty()) {
        std::fprintf(stderr, "craft content did not load (%zu errors)\n", d.errors.size());
        return 1;
    }
    Workplace wp = shop(d);
    const double skills[] = {0.1, 0.5, 0.9};
    std::printf("# Craft outcomes by skill (n=%d per cell, seed %llu)\n\n", n, static_cast<unsigned long long>(seed));
    std::printf("Every starter design run from gathered materials (q 0.5) in a full workshop, tools q 0.6. "
                "named = the archetype came out; impr = an improvised thing; other = an intermediate or another "
                "archetype; stop = the chain stopped (ruined step); q = mean quality of what came out; flaw = share "
                "with at least one flaw.\n\n");
    std::printf("| design | stages | skill | named %% | impr %% | other %% | stop %% | q | flaw %% |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|\n");
    std::map<double, std::vector<double>> namedBySkill;
    for (const auto& ds : d.designs) {
        Recipe r = recipeFromDesign(d, ds, 1.0, true);
        for (double sk : skills) {
            Actor a = actor(d, sk, 31);
            int named = 0, impr = 0, other = 0, stop = 0, fl = 0, done = 0;
            double q = 0.0;
            for (int i = 0; i < n; ++i) {
                std::vector<Item> pool;
                ChainOpts o;
                o.rawFromNothing = true;
                o.stopOnRisk = false;
                ChainRun run = runChain(d, r.steps, pool, a, wp, seed * 7919 + static_cast<std::uint64_t>(i), 500, o);
                if (!run.done) {
                    ++stop;
                    continue;
                }
                ++done;
                q += run.out.q;
                if (flawed(run.out)) ++fl;
                if (run.out.form == ds.form) ++named;
                else if (run.out.form.rfind("improvised_", 0) == 0) ++impr;
                else ++other;
            }
            namedBySkill[sk].push_back(100.0 * named / n);
            std::printf("| %s | %zu | %.1f | %.0f | %.0f | %.0f | %.0f | %.2f | %.0f |\n", ds.id.c_str(), ds.steps.size(),
                        sk, 100.0 * named / n, 100.0 * impr / n, 100.0 * other / n, 100.0 * stop / n,
                        done ? q / done : 0.0, done ? 100.0 * fl / done : 0.0);
        }
    }
    std::printf("\n## Summary\n\n| skill | mean named %% over designs |\n|---|---|\n");
    for (auto& [sk, v] : namedBySkill) {
        double s = 0;
        for (double x : v) s += x;
        std::printf("| %.1f | %.0f |\n", sk, v.empty() ? 0.0 : s / v.size());
    }

    // a village smith: starts at 0.3, makes a knife a day and a dagger every
    // other day for 30 days, learning from each step
    std::printf("\n## Village smith, 30 days\n\nStarts at smithing/carpentry 0.3, knows the materials a little "
                "(0.2), makes one knife and one dagger a day. Skill grows by the experience the resolver returns.\n\n");
    std::printf("| days | skill | knives named %% | daggers named %% | dagger q mean | dagger q min | dagger q max |\n|---|---|---|---|---|---|---|\n");
    Actor smith = actor(d, 0.3, 77);
    for (auto& [m, k] : smith.knowsMaterial) k = 0.2;
    const DesignDef* knife = d.design("knife");
    const DesignDef* dagger = d.design("dagger");
    Recipe rk = recipeFromDesign(d, *knife, 1.0, true), rd = recipeFromDesign(d, *dagger, 1.0, true);
    for (int week = 0; week < 5; ++week) {
        int kn = 0, dn = 0, days = week < 4 ? 7 : 2;
        double qs = 0, qmin = 1, qmax = 0;
        int qc = 0;
        for (int day = 0; day < days; ++day) {
            for (int which = 0; which < 2; ++which) {
                std::vector<Item> pool;
                ChainOpts o;
                o.rawFromNothing = true;
                o.stopOnRisk = false;
                std::uint64_t t = static_cast<std::uint64_t>(week * 7 + day) * 144 + which * 50;
                ChainRun run = runChain(d, which ? rd.steps : rk.steps, pool, smith, wp, seed, t, o);
                for (const auto& sr : run.results) {
                    for (const auto& [sk, xp] : sr.skillXp) smith.skills[sk] = std::min(1.0, smith.skills[sk] + xp);
                    for (const auto& [m, g] : sr.knowGain) smith.knowsMaterial[m] = std::min(1.0, smith.knowsMaterial[m] + g);
                }
                if (!run.done) continue;
                if (which == 0 && run.out.form == "knife") ++kn;
                if (which == 1) {
                    if (run.out.form == "dagger") ++dn;
                    qs += run.out.q;
                    qmin = std::min(qmin, run.out.q);
                    qmax = std::max(qmax, run.out.q);
                    ++qc;
                }
            }
        }
        std::printf("| %d-%d | %.2f | %.0f | %.0f | %.2f | %.2f | %.2f |\n", week * 7 + 1, week * 7 + days,
                    smith.skills["smithing"], 100.0 * kn / days, 100.0 * dn / days, qc ? qs / qc : 0.0, qc ? qmin : 0.0,
                    qc ? qmax : 0.0);
    }
    return 0;
}
