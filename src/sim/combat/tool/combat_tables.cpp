// altera_combat_tables: runs the same tables as gdd/combat/tools/combat_sim.py
// (report sections 1, 2, 3, 3b, 3c, 3d, 4, 5, 6) on the C++ core and, with
// --golden, compares every percentage against a report of the prototype.
//
//   altera_combat_tables [--n 300] [--seed 1] [--data content/combat]
//                        [--out report.md] [--golden COMBAT_SIM_REPORT.md] [--tol 5]
//   altera_combat_tables --log <seed> [--a sword:50] [--b sword:50:gambeson]   one duel's event log
//
// Exit code: 0, or 2 when --golden finds a cell outside the tolerance.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "sim/combat/combat.hpp"

using namespace altera::combat;

namespace {

struct Spec {
    double skill;
    std::string weapon;
    std::vector<std::string> armor;
    double know = -1.0;
    std::string shield;
};

FighterSpec fs(const Spec& s) { return {s.skill, s.weapon, s.armor, s.know, s.shield}; }

std::string pct(double x) {
    char b[16];
    std::snprintf(b, sizeof b, "%.0f%%", 100.0 * x);
    return b;
}

// Python's f"{x:.0f}" rounds half to even; so does printf with the default rounding mode.
double duelWin(const CombatData& d, const Spec& a, const Spec& b, int n, std::uint64_t seed, bool useReach = true,
               bool clinch = true) {
    Engine e(d, seed);
    int w = 0;
    for (int i = 0; i < n; ++i) {
        Fighter fa = e.make(fs(a), 1), fb = e.make(fs(b), 2);
        e.fight(fa, fb, useReach, clinch);
        if (fa.fighting() && !fb.fighting()) ++w;
    }
    return static_cast<double>(w) / n;
}

double rangedWin(const CombatData& d, const Spec& shooter, const Spec& target, const std::string& wn, double dist,
                 int n, std::uint64_t seed) {
    Engine e(d, seed);
    const int wi = d.rangedWeapon(wn);
    int w = 0;
    for (int i = 0; i < n; ++i) {
        Fighter a = e.make(fs(shooter), 1), t = e.make(fs(target), 2);
        e.rangedDuel(a, t, wi, dist);
        if (a.fighting() && !t.fighting()) ++w;
    }
    return static_cast<double>(w) / n;
}

struct GroupStats {
    double win, alive, dead, draw;
};

GroupStats groupRun(const CombatData& d, const std::vector<Spec>& sa, const std::vector<Spec>& sb, int n,
                    std::uint64_t seed) {
    Engine e(d, seed);
    int win = 0, draw = 0, dead = 0;
    std::vector<int> surv;
    for (int i = 0; i < n; ++i) {
        std::vector<Fighter> A, B;
        for (std::size_t k = 0; k < sa.size(); ++k) A.push_back(e.make(fs(sa[k]), static_cast<std::int64_t>(k), 0));
        for (std::size_t k = 0; k < sb.size(); ++k)
            B.push_back(e.make(fs(sb[k]), static_cast<std::int64_t>(100 + k), 1));
        const BattleResult r = e.battle(A, B);
        if (r.aliveA > 0 && r.aliveB == 0) ++win, surv.push_back(r.aliveA);
        else if (r.aliveA > 0 && r.aliveB > 0) ++draw;
        for (const auto& f : A) dead += f.state == State::Dead;
    }
    const double alive = surv.empty() ? 0.0 : std::accumulate(surv.begin(), surv.end(), 0.0) / surv.size();
    return {static_cast<double>(win) / n, alive, static_cast<double>(dead) / n / sa.size(),
            static_cast<double>(draw) / n};
}

std::string report(const CombatData& d, int n, std::uint64_t S) {
    std::ostringstream o;
    o << "# Combat core tables (C++)\n\nRuns per pair: " << n << ", seed " << S
      << ". Same tables as combat_sim.py; numbers from content/combat/*.json.\n\n";
    const std::vector<std::vector<std::string>> armors = {{}, {"gambeson"}, {"mail", "helmet"}, {"plate"}};

    o << "## 1. Skill (sword, no armour): row = skill A, column = skill B, % wins A\n\n| A\\B | 10 | 30 | 50 | 70 | 90 |\n|---|---|---|---|---|---|\n";
    for (int sa : {10, 30, 50, 70, 90}) {
        o << "| " << sa << " |";
        for (int sb : {10, 30, 50, 70, 90}) o << " " << pct(duelWin(d, {double(sa), "sword", {}}, {double(sb), "sword", {}}, n, S)) << " |";
        o << "\n";
    }

    o << "\n## 2. Weapon (with reach) vs sword in armour, skill 50: % wins A\n\n| weapon A \\ armour B | none | gambeson | mail+helmet | plate |\n|---|---|---|---|---|\n";
    for (const char* wp : {"sword", "axe", "mace", "spear", "dagger", "fists"}) {
        o << "| " << wp << " |";
        for (const auto& ar : armors) o << " " << pct(duelWin(d, {50, wp, {}}, {50, "sword", ar}, n, S)) << " |";
        o << "\n";
    }

    o << "\n## 3. Reach: no armour, skill 50; % wins A (B: sword)\n\n| A | with reach | without reach |\n|---|---|---|\n";
    for (const char* wp : {"spear", "axe", "mace", "dagger"})
        o << "| " << wp << " | " << pct(duelWin(d, {50, wp, {}}, {50, "sword", {}}, n, S)) << " | "
          << pct(duelWin(d, {50, wp, {}}, {50, "sword", {}}, n, S, false)) << " |\n";

    o << "\n## 3b. Clinch: % wins A vs sword skill 50. Columns: with clinch / without\n\n| A \\ armour B | none | gambeson | mail+helmet | plate |\n|---|---|---|---|---|\n";
    for (const char* wp : {"sword", "axe", "mace", "dagger", "fists", "spear"}) {
        o << "| " << wp << " |";
        for (const auto& ar : armors)
            o << " " << pct(duelWin(d, {50, wp, {}}, {50, "sword", ar}, n, S)) << " / "
              << pct(duelWin(d, {50, wp, {}}, {50, "sword", ar}, n, S, true, false)) << " |";
        o << "\n";
    }
    o << "\nDagger vs armoured swordsman (skill 50), by dagger skill:\n\n| dagger skill | vs plate (skill 50) | vs mail+helmet |\n|---|---|---|\n";
    for (int sk : {30, 50, 70, 90})
        o << "| " << sk << " | " << pct(duelWin(d, {double(sk), "dagger", {}}, {50, "sword", {"plate"}}, n, S)) << " | "
          << pct(duelWin(d, {double(sk), "dagger", {}}, {50, "sword", {"mail", "helmet"}}, n, S)) << " |\n";

    o << "\n## 3c. Ranged: % wins shooter (skill 50, dagger as backup) vs swordsman (50) from 30 m\n\n| weapon \\ armour | none | gambeson | mail+helmet | plate |\n|---|---|---|---|---|\n";
    for (const auto& w : d.ranged) {
        o << "| " << w.id << " |";
        for (const auto& ar : armors) o << " " << pct(rangedWin(d, {50, "dagger", {}}, {50, "sword", ar}, w.id, 30, n, S)) << " |";
        o << "\n";
    }
    o << "\nStart distance for longbow vs swordsman, no armour and gambeson:\n\n| distance, m | none | gambeson |\n|---|---|---|\n";
    for (int ds : {10, 20, 30, 50}) {
        o << "| " << ds << " |";
        for (const auto& ar : std::vector<std::vector<std::string>>{{}, {"gambeson"}})
            o << " " << pct(rangedWin(d, {50, "dagger", {}}, {50, "sword", ar}, "longbow", ds, n, S)) << " |";
        o << "\n";
    }

    o << "\n## 3d. Wear (sword 50 vs sword in armour: weapon A after the fight, breaks per 100, armour B)\n\n| armour B | weapon A | breaks /100 | armour B |\n|---|---|---|---|\n";
    for (const auto& ar : armors) {
        Engine e(d, S);
        double wc = 0, ac = 0;
        int br = 0;
        for (int i = 0; i < n; ++i) {
            Fighter a = e.make({50, "sword", {}}, 1), b = e.make({50, "sword", ar}, 2);
            e.fight(a, b);
            wc += a.weaponCond;
            br += a.broke;
            if (!ar.empty()) ac += std::accumulate(b.armorCond.begin(), b.armorCond.end(), 0.0) / b.armorCond.size();
        }
        std::string name;
        for (const auto& x : ar) name += (name.empty() ? "" : "+") + x;
        char b1[16], b2[16], b3[16];
        std::snprintf(b1, sizeof b1, "%.0f%%", wc / n * 100);
        std::snprintf(b2, sizeof b2, "%.0f", br * 100.0 / n);
        std::snprintf(b3, sizeof b3, "%.0f%%", ar.empty() ? 100.0 : ac / n * 100);
        o << "| " << (name.empty() ? "none" : name) << " | " << b1 << " | " << b2 << " | " << b3 << " |\n";
    }

    o << "\n## 4. Shields: sword+shield (A) vs sword (B), no armour, skill 50, % wins A\n\n";
    for (const char* sh : {"", "buckler", "round", "kite"})
        o << "- " << (*sh ? sh : "no shield") << ": " << pct(duelWin(d, {50, "sword", {}, 0.3, sh}, {50, "sword", {}}, n, S)) << "\n";

    o << "\n## 5. Knowledge of the foe (sword, skill 50): A 0.0/0.3/0.7 vs 0.3\n\n";
    for (double k : {0.0, 0.3, 0.7}) {
        char b[16];
        std::snprintf(b, sizeof b, "%.1f", k);
        o << "- know " << b << ": " << pct(duelWin(d, {50, "sword", {}, k}, {50, "sword", {}, 0.3}, n, S)) << "\n";
    }

    o << "\n## 6. Group battles (sword, gambeson; % wins A, A alive on a win, share of A dead, drawn)\n\n| side | wins A | alive A | dead | drawn |\n|---|---|---|---|---|\n";
    auto g = [](int c, double sk, std::vector<std::string> ar = {"gambeson"}, std::string w = "sword") {
        return std::vector<Spec>(c, Spec{sk, w, ar});
    };
    struct Case {
        const char* name;
        std::vector<Spec> a, b;
    };
    const std::vector<Case> cases = {{"1 vs 1 (50)", g(1, 50), g(1, 50)},
                                     {"2 vs 1 (50)", g(2, 50), g(1, 50)},
                                     {"3 vs 3 (50)", g(3, 50), g(3, 50)},
                                     {"3 masters (85) vs 6 novices (20)", g(3, 85), g(6, 20)},
                                     {"3 in plate (70) vs 8 unarmoured (35)", g(3, 70, {"plate"}), g(8, 35, {})},
                                     {"5 spears (45) vs 5 swords (45)", g(5, 45, {"gambeson"}, "spear"), g(5, 45)}};
    for (const auto& c : cases) {
        const GroupStats s = groupRun(d, c.a, c.b, std::max(60, n / 4), S);
        char b[128];
        std::snprintf(b, sizeof b, "| %s | %.0f%% | %.1f | %.0f%% | %.0f%% |\n", c.name, s.win * 100, s.alive, s.dead * 100,
                      s.draw * 100);
        o << b;
    }
    return o.str();
}

// --- golden comparison ------------------------------------------------------
// Each section is cut into its table rows / bullets; the percentages of row i
// of section k are compared with row i of section k of the other report.

std::vector<std::vector<std::vector<double>>> percents(const std::string& md) {
    std::vector<std::vector<std::vector<double>>> sec;
    std::istringstream in(md);
    std::string line;
    static const std::regex num(R"((\d+)%)");
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("## 7", 0) == 0) break; // treatment: not ported in this step
        if (line.rfind("## ", 0) == 0) {
            sec.emplace_back();
            continue;
        }
        if (sec.empty()) continue;
        std::vector<double> v;
        for (std::sregex_iterator it(line.begin(), line.end(), num), e; it != e; ++it) v.push_back(std::stod((*it)[1]));
        if (!v.empty()) sec.back().push_back(v);
    }
    return sec;
}

int compare(const std::string& mine, const std::string& golden, double tol, std::ostream& out) {
    const auto a = percents(mine), b = percents(golden);
    int bad = 0, cells = 0;
    double worst = 0.0, sum = 0.0;
    const std::size_t ns = std::min(a.size(), b.size());
    for (std::size_t s = 0; s < ns; ++s) {
        // section 3d (index 5) carries condition percents, sections 6 (index 8) dead/draw; all compared alike
        for (std::size_t r = 0; r < std::min(a[s].size(), b[s].size()); ++r)
            for (std::size_t c = 0; c < std::min(a[s][r].size(), b[s][r].size()); ++c) {
                const double dlt = std::abs(a[s][r][c] - b[s][r][c]);
                ++cells;
                sum += dlt;
                worst = std::max(worst, dlt);
                if (dlt > tol) {
                    ++bad;
                    out << "  section " << s + 1 << " row " << r + 1 << " col " << c + 1 << ": C++ " << a[s][r][c]
                        << "% vs golden " << b[s][r][c] << "% (diff " << dlt << ")\n";
                }
            }
    }
    char buf[160];
    std::snprintf(buf, sizeof buf, "golden: %d cells, %d outside +-%.0f pp, mean |diff| %.2f pp, worst %.0f pp\n", cells,
                  bad, tol, cells ? sum / cells : 0.0, worst);
    out << buf;
    return bad;
}

Spec parseSpec(const std::string& s) { // weapon:skill[:armor+armor]
    Spec r{50, "sword", {}};
    std::vector<std::string> p;
    std::stringstream ss(s);
    std::string t;
    while (std::getline(ss, t, ':')) p.push_back(t);
    if (!p.empty()) r.weapon = p[0];
    if (p.size() > 1) r.skill = std::stod(p[1]);
    if (p.size() > 2) {
        std::stringstream as(p[2]);
        while (std::getline(as, t, '+'))
            if (!t.empty()) r.armor.push_back(t);
    }
    return r;
}

} // namespace

int main(int argc, char** argv) {
    int n = 300;
    std::uint64_t seed = 1;
    std::string dataDir = std::string(ALTERA_SIM_SOURCE_DIR) + "/content/combat", out, golden;
    double tol = 5.0;
    long long logSeed = -1;
    std::string sa = "sword:50", sb = "sword:50";
    bool duelOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--n") n = std::atoi(next().c_str());
        else if (a == "--seed") seed = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--data") dataDir = next();
        else if (a == "--out") out = next();
        else if (a == "--golden") golden = next();
        else if (a == "--tol") tol = std::atof(next().c_str());
        else if (a == "--log") logSeed = std::atoll(next().c_str());
        else if (a == "--a") sa = next();
        else if (a == "--b") sb = next();
        else if (a == "--duel") duelOnly = true;
        else {
            std::cerr << "unknown argument " << a << "\n";
            return 1;
        }
    }
    CombatData d;
    try {
        d = CombatData::load(dataDir);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    if (duelOnly) { // --duel: a/b/none counts of one pairing (for checks against the prototype)
        Engine e(d, seed);
        int w[3] = {0, 0, 0};
        double rounds = 0;
        for (int i = 0; i < n; ++i) {
            Fighter a = e.make(fs(parseSpec(sa)), 1), b = e.make(fs(parseSpec(sb)), 2);
            rounds += e.fight(a, b);
            ++w[a.fighting() && !b.fighting() ? 0 : b.fighting() && !a.fighting() ? 1 : 2];
        }
        std::printf("a %.4f b %.4f none %.4f rounds %.2f\n", double(w[0]) / n, double(w[1]) / n, double(w[2]) / n,
                    rounds / n);
        return 0;
    }
    if (logSeed >= 0) {
        EventStream ev;
        ev.subscribe([&](const std::vector<Event>& batch) {
            for (const auto& e : batch) std::cout << eventLine(d, e) << "\n";
        });
        Engine e(d, static_cast<std::uint64_t>(logSeed), &ev);
        Fighter a = e.make(fs(parseSpec(sa)), 1), b = e.make(fs(parseSpec(sb)), 2);
        e.fight(a, b);
        return 0;
    }
    const std::string md = report(d, n, seed);
    std::cout << md;
    if (!out.empty()) std::ofstream(out, std::ios::binary) << md;
    if (!golden.empty()) {
        std::ifstream g(golden, std::ios::binary);
        if (!g) {
            std::cerr << "cannot open " << golden << "\n";
            return 1;
        }
        std::stringstream gs;
        gs << g.rdbuf();
        std::cout << "\n";
        return compare(md, gs.str(), tol, std::cout) ? 2 : 0;
    }
    return 0;
}
