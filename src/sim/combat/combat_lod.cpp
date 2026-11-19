#include "sim/combat/combat_lod.hpp"

#include <cmath>
#include <cstdio>

namespace altera::combat {

namespace {

std::string specKey(const FighterSpec& s, double skill) {
    std::string k = s.weapon + "|" + s.shield + "|";
    for (const auto& a : s.armor) k += a + ",";
    char buf[48];
    std::snprintf(buf, sizeof buf, "|%.3f|%.3f", skill, s.know);
    return k + buf;
}

std::uint64_t keyHash(const std::string& s) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

// Grid skill for one side: the lower or upper grid point, with the
// probability that makes the expectation equal to the real skill.
double gridSkill(double skill, double step, Rng& rng) {
    if (step <= 0.0) return skill;
    const double lo = std::floor(skill / step) * step;
    const double frac = (skill - lo) / step;
    return rng.random() < frac ? lo + step : lo;
}

} // namespace

std::size_t DuelTable::cells() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cells_.size();
}

const std::vector<DuelSample>& DuelTable::cell(const FighterSpec& a, const FighterSpec& b) {
    const std::string key = specKey(a, a.skill) + "#" + specKey(b, b.skill);
    std::lock_guard<std::mutex> lock(mu_);
    auto it = cells_.find(key);
    if (it != cells_.end()) return *it->second;
    auto samples = std::make_unique<std::vector<DuelSample>>();
    samples->reserve(static_cast<std::size_t>(cfg_.samples));
    Engine e(d_, combatSeed(0x10d5eedull, keyHash(key), 0));
    for (int i = 0; i < cfg_.samples; ++i) {
        Fighter fa = e.make(a, 0, 0), fb = e.make(b, 1, 1);
        DuelSample s;
        s.rounds = e.fight(fa, fb);
        s.a = fa.state;
        s.b = fb.state;
        s.woundsA = fa.wounds;
        s.zoneA = fa.woundZone;
        s.dmgA = fa.woundDmg;
        s.woundsB = fb.wounds;
        s.zoneB = fb.woundZone;
        s.dmgB = fb.woundDmg;
        s.bloodA = fa.blood;
        s.bloodB = fb.blood;
        samples->push_back(std::move(s));
    }
    return *(cells_[key] = std::move(samples));
}

const DuelSample& DuelTable::resolve(const FighterSpec& a, const FighterSpec& b, Rng& rng) {
    FighterSpec ga = a, gb = b;
    ga.skill = gridSkill(a.skill, cfg_.skillStep, rng);
    gb.skill = gridSkill(b.skill, cfg_.skillStep, rng);
    if (ga.know < 0) ga.know = d_.core.defaultKnow;
    if (gb.know < 0) gb.know = d_.core.defaultKnow;
    const std::vector<DuelSample>& c = cell(ga, gb);
    const std::size_t i = static_cast<std::size_t>(rng.next() % static_cast<std::uint64_t>(c.size()));
    return c[i];
}

void DuelTable::apply(const DuelSample& s, Fighter& a, Fighter& b) const {
    auto one = [&](Fighter& f, State st, const std::vector<int>& w, const std::vector<int>& z, const std::vector<double>& dmg,
                   double blood) {
        f.state = st;
        f.blood = blood;
        for (std::size_t i = 0; i < w.size(); ++i) {
            f.wounds.push_back(w[i]);
            f.woundZone.push_back(z[i]);
            f.woundDmg.push_back(dmg[i]);
            f.hp[z[i]] -= dmg[i];
        }
    };
    one(a, s.a, s.woundsA, s.zoneA, s.dmgA, s.bloodA);
    one(b, s.b, s.woundsB, s.zoneB, s.dmgB, s.bloodB);
}

} // namespace altera::combat
