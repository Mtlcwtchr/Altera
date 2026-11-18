// Animals and monsters: characters of the same core (Character with a
// species) whose mind is a handful of reactive drivers instead of the
// four-layer human AI. No scripts and no storyteller: hunger, fear, prey,
// breeding and migration move them, and whoever walks into their range meets
// them (the user's rule: chance comes out of the simulation itself).
//
// Drivers, in order, once per tick:
//   threat   - a person within notice range: flee (or, for an aggressive and
//              hungry group, close in and attack a lone person);
//   alarm    - a spooked group (moodlet alarmed) keeps running from the
//              nearest person;
//   feed     - grazers eat where their biome is; predators hunt prey groups;
//   wander   - otherwise drift around the territory centre (den);
// and once per day: breeding, starvation, migration of the den.

#include <algorithm>
#include <cmath>

#include "sim/ai.hpp"

namespace altera::sim {

namespace {

constexpr double kPi = 3.14159265358979323846;

bool inBiome(const World& w, const SpeciesDef& s, Vec2 p) {
    if (s.habitat == "water") return w.map().waterAt(p);
    if (w.map().waterAt(p)) return false;
    const std::string& b = w.map().biomeAt(p);
    return std::find(s.biomes.begin(), s.biomes.end(), b) != s.biomes.end();
}

void moveToward(Character& c, Vec2 to, double speed) {
    const double d = dist(c.pos, to);
    if (d < 1e-6) return;
    const double k = std::min(1.0, speed / d);
    c.pos.x += (to.x - c.pos.x) * k;
    c.pos.y += (to.y - c.pos.y) * k;
}

Vec2 clampToMap(const World& w, Vec2 p) {
    const double s = w.map().sizeM;
    return {std::clamp(p.x, 1.0, s - 1.0), std::clamp(p.y, 1.0, s - 1.0)};
}

const Character* nearestPerson(const AnimalTick& t, const Character& a, double radius) {
    const Character* best = nullptr;
    double bd = radius;
    for (const Character* op : t.people) {
        const Character& o = *op;
        if (!o.body.alive) continue;
        const double d = dist(o.pos, a.pos);
        if (d <= bd) {
            bd = d;
            best = &o;
        }
    }
    return best;
}

int peopleWithin(const AnimalTick& t, Vec2 p, double radius) {
    int n = 0;
    for (const Character* o : t.people)
        if (o->body.alive && dist(o->pos, p) <= radius) ++n;
    return n;
}

void bite(World& w, Character& wolf, Character& victim, const SpeciesDef& s) {
    Rng r = w.rng(wolf.id, "bite");
    if (r.uniform() >= s.attackChance) {
        w.remember(victim, MemoryEntry{0, "saw_beast", {wolf.id}, kNone, -0.3, 0.0, "seen", 1.0});
        return;
    }
    const double dmg = r.range(s.attackLo, s.attackHi);
    victim.body.health = clamp01(victim.body.health - dmg);
    w.remember(victim, MemoryEntry{0, "attacked_by_beast", {wolf.id}, kNone, -0.7, 0.0, "seen", 1.0});
    auto md = w.content().moodlets.find("mauled");
    if (md != w.content().moodlets.end())
        victim.moodlets.push_back({"mauled", md->second.value, w.now() + md->second.ttl});
    // The beast backs off after a bite; a fed one leaves.
    wolf.needs["hunger"] = clamp01(wolf.need("hunger") + 0.05);
    wolf.moodlets.push_back({"alarmed", 0.0, w.now() + 12});
    if (victim.body.health <= 0.0 && victim.body.alive) {
        victim.body.alive = false;
        victim.body.causeOfDeath = "beast";
        w.chronicle("death", {victim.id}, "chronicle.death", {{"who", victim.id}, {"cause", "beast"}});
    }
}

Character* nearestPrey(World& w, const Character& a, const SpeciesDef& s, double radius) {
    Character* best = nullptr;
    double bd = radius;
    for (auto& [id, o] : w.characters()) {
        if (!o.body.alive || o.species.empty() || o.id == a.id) continue;
        if (std::find(s.prey.begin(), s.prey.end(), o.species) == s.prey.end()) continue;
        const double d = dist(o.pos, a.pos);
        if (d <= bd) {
            bd = d;
            best = &o;
        }
    }
    return best;
}

} // namespace

void loseHeads(World& w, Character& herd, int n, const std::string& cause) {
    herd.group -= n;
    if (herd.group <= 0 && herd.body.alive) {
        herd.group = 0;
        herd.body.alive = false;
        herd.body.causeOfDeath = cause;
    }
    (void)w;
}

void animalThink(World& w, Character& a, const AnimalTick& tick) {
    if (!a.body.alive) return;
    const SpeciesDef* sp = w.content().speciesOf(a.species);
    if (!sp) return;
    const SpeciesDef& s = *sp;
    const Tick now = w.now();
    // Hunger: per tick.
    double& hunger = a.needs["hunger"];
    hunger = clamp01(hunger - s.hungerPerDay / static_cast<double>(kTicksPerDay));
    if (s.speedMPerTick <= 0.0) return; // fish: no movement drivers

    const bool alarmed = a.hasMoodlet("alarmed", now);
    // --- threat ---------------------------------------------------------------
    const Character* person = nearestPerson(tick, a, alarmed ? s.fleeM : s.noticeM);
    // A hunter stalking this very group (walking up to it or in the middle of
    // the hunt) is noticed only by chance, less the better he is.
    if (person && !alarmed && person->ai.fsm.target == a.id) {
        auto sk = person->skills.find("hunting");
        const double skill = sk == person->skills.end() ? 0.0 : sk->second;
        if (w.rng(a.id, "notice:" + std::to_string(person->id)).uniform() >= 0.12 * (1.0 - 0.7 * skill)) person = nullptr;
    }
    if (person) {
        const bool hungry = hunger < 0.3;
        const bool lone = peopleWithin(tick, person->pos, 60.0) <= 1;
        const bool attack = s.aggression > 0.0 && hungry && lone && a.group >= 3 && !alarmed &&
                            w.rng(a.id, "dare").uniform() < s.aggression;
        if (attack) {
            moveToward(a, person->pos, s.speedMPerTick);
            if (dist(a.pos, person->pos) <= 5.0) bite(w, a, *w.character(person->id), s);
            return;
        }
        // Flee: away from the person, back toward the den when cornered at the edge.
        Vec2 away{a.pos.x + (a.pos.x - person->pos.x), a.pos.y + (a.pos.y - person->pos.y)};
        if (dist(away, a.pos) < 1e-6) away.x += 1.0;
        Vec2 to{a.pos.x + (away.x - a.pos.x) * 10.0, a.pos.y + (away.y - a.pos.y) * 10.0};
        to = clampToMap(w, to);
        moveToward(a, to, s.speedMPerTick * 1.3);
        if (!alarmed) a.moodlets.push_back({"alarmed", 0.0, now + 6});
        return;
    }
    // Grazers also run from predators that hunt them.
    if (s.diet != "prey") {
        const Character* hunter = nullptr;
        double bd = s.noticeM * 2.0;
        for (const Character* op : tick.predators) {
            const Character& o = *op;
            if (!o.body.alive || o.id == a.id) continue;
            const SpeciesDef* os = w.content().speciesOf(o.species);
            if (!os || std::find(os->prey.begin(), os->prey.end(), a.species) == os->prey.end()) continue;
            const double d = dist(o.pos, a.pos);
            if (d <= bd) {
                bd = d;
                hunter = &o;
            }
        }
        if (hunter) {
            Vec2 to{a.pos.x + (a.pos.x - hunter->pos.x) * 5.0, a.pos.y + (a.pos.y - hunter->pos.y) * 5.0};
            moveToward(a, clampToMap(w, to), s.speedMPerTick * 1.2);
            return;
        }
    }
    // --- feed -----------------------------------------------------------------
    if (hunger < 0.7) {
        if (s.diet == "prey") {
            Character* prey = nearestPrey(w, a, s, s.territoryM);
            if (prey) {
                moveToward(a, prey->pos, s.speedMPerTick);
                if (dist(a.pos, prey->pos) <= 10.0) {
                    Rng r = w.rng(a.id, "kill");
                    if (r.uniform() < 0.25) {
                        loseHeads(w, *prey, 1, "prey");
                        hunger = clamp01(hunger + 0.6);
                    }
                    prey->moodlets.push_back({"alarmed", 0.0, now + 24});
                }
                return;
            }
        } else if (inBiome(w, s, a.pos)) {
            hunger = clamp01(hunger + 2.5 / static_cast<double>(kTicksPerDay)); // grazing restores it
            return;
        }
    }
    // --- wander around the den ------------------------------------------------
    Rng r = w.rng(a.id, "wander");
    if (r.uniform() < 0.3) {
        const double ang = r.range(0.0, 2.0 * kPi);
        const double rad = r.range(0.0, std::max(50.0, s.territoryM * 0.5));
        Vec2 to = clampToMap(w, {a.den.x + std::cos(ang) * rad, a.den.y + std::sin(ang) * rad});
        if (inBiome(w, s, to)) moveToward(a, to, s.speedMPerTick * 0.5);
    }
}

void animalsDaily(World& w) {
    std::vector<Character> born;
    for (auto& [id, a] : w.characters()) {
        if (a.species.empty() || !a.body.alive) continue;
        const SpeciesDef* sp = w.content().speciesOf(a.species);
        if (!sp) continue;
        const SpeciesDef& s = *sp;
        Rng r = w.rng(a.id, "daily");
        // Starvation: a starving group loses heads.
        if (s.hungerPerDay > 0.0 && a.need("hunger") <= 0.05) {
            if (++a.starveDays >= s.starveDays) {
                loseHeads(w, a, 1, "starvation");
                a.starveDays = 0;
            }
        } else {
            a.starveDays = 0;
        }
        // Fish: the shoal regrows every day toward its carrying size (logistic).
        if (s.habitat == "water") {
            const double g = a.group;
            const double grow = std::max(1.0, g * 0.12 * (1.0 - g / std::max(1, s.maxGroup)));
            if (a.group < s.maxGroup) a.group = std::min(s.maxGroup, a.group + static_cast<int>(std::round(grow)));
            continue;
        }
        // Breeding: fed groups grow, full groups split off a young group nearby.
        // Predators breed only when well fed (prey is plentiful).
        const double fedEnough = s.diet == "prey" ? 0.65 : 0.45;
        if (s.breedEveryDays > 0 && (s.hungerPerDay <= 0.0 || a.need("hunger") > fedEnough) &&
            r.uniform() < 1.0 / s.breedEveryDays) {
            // Large broods (fish) grow by a share of the group, beasts by one head.
            const int brood = s.habitat == "water" ? std::max(1, a.group / 5) : 1;
            if (a.group < s.maxGroup) a.group = std::min(s.maxGroup, a.group + brood);
            else if (a.group >= 4 && [&] {
                         // Carrying capacity: no new group where the land is already taken.
                         const double r2 = std::max(400.0, s.territoryM * 1.5);
                         int around = 0;
                         for (const auto& [oid, o] : w.characters())
                             if (o.body.alive && o.species == a.species && oid != a.id && dist(o.den, a.den) < r2) ++around;
                         return around < 2;
                     }()) {
                Character y = a;
                y.id = kNone;
                y.group = a.group / 2;
                a.group -= y.group;
                const double ang = r.range(0.0, 2.0 * kPi);
                const double rad = std::max(300.0, s.territoryM);
                y.den = clampToMap(w, {a.den.x + std::cos(ang) * rad, a.den.y + std::sin(ang) * rad});
                y.pos = y.den;
                y.memory.clear();
                y.moodlets.clear();
                if (inBiome(w, s, y.den)) born.push_back(std::move(y));
            }
        }
        // Migration: a hungry grazer moves its den toward better ground; predators follow prey.
        if (s.hungerPerDay > 0.0 && a.need("hunger") < 0.25 && r.uniform() < 0.5) {
            const double ang = r.range(0.0, 2.0 * kPi);
            const Vec2 to = clampToMap(w, {a.den.x + std::cos(ang) * 1500.0, a.den.y + std::sin(ang) * 1500.0});
            if (inBiome(w, s, to)) a.den = to;
        }
        // Being hunted near the village pushes the den away (the herd learns).
        if (a.hasMoodlet("alarmed", w.now()) && r.uniform() < 0.2) {
            for (const auto& [sid, st] : w.settlements())
                if (dist(a.den, st.center) < 2500.0) {
                    Vec2 away{a.den.x + (a.den.x - st.center.x) * 0.3, a.den.y + (a.den.y - st.center.y) * 0.3};
                    away = clampToMap(w, away);
                    if (inBiome(w, s, away)) a.den = away;
                }
        }
    }
    for (auto& y : born) w.addCharacter(std::move(y));
}

void spawnAnimals(World& w, std::uint64_t seed) {
    const Content& ct = w.content();
    const MapGrid& m = w.map();
    const double cellKm2 = (m.cellM / 1000.0) * (m.cellM / 1000.0);
    for (const auto& [id, s] : ct.species) {
        if (s.perKm2 <= 0.0) continue;
        for (int y = 0; y < m.h; ++y)
            for (int x = 0; x < m.w; ++x) {
                const Vec2 p{(x + 0.5) * m.cellM, (y + 0.5) * m.cellM};
                if (!inBiome(w, s, p)) continue;
                Rng cr(hashCombine(hashCombine(seed, hashStr("species:" + id)), static_cast<std::uint64_t>(y * m.w + x)));
                // per_km2 counts groups; keep them off the settlement itself.
                const double expect = s.perKm2 * cellKm2;
                int n = static_cast<int>(expect);
                if (cr.chance(expect - n)) ++n;
                for (int k = 0; k < n; ++k) {
                    Character a;
                    a.species = id;
                    a.name = id;
                    a.group = cr.irange(s.groupLo, s.groupHi);
                    a.pos = {(x + cr.uniform()) * m.cellM, (y + cr.uniform()) * m.cellM};
                    bool nearVillage = false;
                    for (const auto& [sid, st] : w.settlements())
                        if (dist(a.pos, st.center) < 400.0 && s.habitat != "water") nearVillage = true;
                    if (nearVillage) continue;
                    a.den = a.pos;
                    a.needs["hunger"] = cr.range(0.5, 0.9);
                    a.body.ageDays = 2 * kDaysPerYear;
                    a.body.sex = 'f';
                    w.addCharacter(std::move(a));
                }
            }
    }
}

} // namespace altera::sim
