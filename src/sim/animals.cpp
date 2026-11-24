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
//
// Livestock (species with domestic: true) is the same kind of character, owned
// by a household (status.family) and kept by its house (den): tame, out to
// graze by day in its grazing seasons, in by night, fed from its trough (its
// own inventory, filled by the household) when the grass is gone. Fed animals
// give their products (eggs, milk) into the same inventory, to be collected;
// they bear young once a year in their birth season and are culled down to
// what the household keeps over the stalled season.

#include <algorithm>
#include <cmath>

#include "sim/ai.hpp"
#include "sim/combat_bridge.hpp"
#include "sim/group_combat.hpp"

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

void fedOnKill(World& w, Character& pack);

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
        fedOnKill(w, wolf);
        w.chronicle("death", {victim.id}, "chronicle.death", {{"who", victim.id}, {"cause", "beast"}});
    }
}

// AL-30: a pack that has killed a person has eaten its fill: hunger full, and
// no more going at people for the species' sated_days_after_kill (one death,
// not a winter of them).
void fedOnKill(World& w, Character& pack) {
    pack.needs["hunger"] = 1.0;
    const SpeciesDef* s = w.content().speciesOf(pack.species);
    if (s && s->attackSatedDays > 0) pack.moodlets.push_back({"sated", 0.0, w.now() + s->attackSatedDays * kTicksPerDay});
}

// A pack that closes in on a person fights it through the combat core, all
// heads at once and whoever stands by (group.json); false = no combat data.
bool packFight(World& w, Character& pack, Character& victim) {
    const CombatBridge* b = combatBridge(w.content());
    if (!b || !b->group.enabled || !b->group.beastAttack) return false;
    const InteractionDef* d = w.content().interaction(b->group.beastAct);
    if (!d || !d->combat.set) return false;
    const Sides sides = gatherSides(w, pack, *d, victim);
    const GroupResolution r = resolveGroupCombat(w, sides, *d);
    if (!r.ran) return false;
    const std::vector<EntityId> wit = witnessesOf(w, pack, *d, &victim);
    applyGroupResult(w, sides, *d, r, nullptr, wit);
    // Win or lose, the pack backs off for a while; a kill feeds it.
    pack.moodlets.push_back({"alarmed", 0.0, w.now() + 12});
    if (!victim.body.alive) fedOnKill(w, pack);
    return true;
}

Character* nearestPrey(World& w, const Character& a, const SpeciesDef& s, double radius) {
    Character* best = nullptr;
    double bd = radius;
    for (auto& [id, o] : w.characters()) {
        if (!o.body.alive || o.species.empty() || o.id == a.id) continue;
        if (std::find(s.prey.begin(), s.prey.end(), o.species) == s.prey.end()) continue;
        if (const SpeciesDef* ps = w.content().speciesOf(o.species); ps && ps->domestic && a.need("hunger") > 0.15) {
            // A fed pack keeps away from the houses: livestock in the village is safe from it.
            bool village = false;
            for (const auto& [sid, st] : w.settlements())
                if (st.full() && dist(o.pos, st.center) < 300.0) village = true;
            if (village) continue;
        }
        const double d = dist(o.pos, a.pos);
        if (d <= bd) {
            bd = d;
            best = &o;
        }
    }
    return best;
}

int dayOfYear(const World& w) { return (w.startDay() + w.day()) % kDaysPerYear; }

bool inDays(int doy, int from, int to) {
    return from <= to ? (doy >= from && doy <= to) : (doy >= from || doy <= to);
}

std::string seasonOfDay(int doy) {
    static const char* const k[] = {"spring", "summer", "autumn", "winter"};
    return k[std::clamp(doy / (kDaysPerYear / 4), 0, 3)];
}

bool grazesOn(const SpeciesDef& s, int doy) {
    return std::find(s.grazeSeasons.begin(), s.grazeSeasons.end(), seasonOfDay(doy)) != s.grazeSeasons.end();
}

// Days from `doy` until the herd can graze again (0 when it grazes today).
int stalledDaysLeft(const SpeciesDef& s, int doy) {
    for (int d = 0; d < kDaysPerYear; ++d)
        if (grazesOn(s, (doy + d) % kDaysPerYear)) return d;
    return kDaysPerYear;
}

int stalledDaysPerYear(const SpeciesDef& s) {
    int n = 0;
    for (int d = 0; d < kDaysPerYear; ++d)
        if (!grazesOn(s, d)) ++n;
    return n;
}

int roundChance(World& w, EntityId id, const std::string& salt, double x) {
    int n = static_cast<int>(x);
    if (w.rng(id, salt).uniform() < x - n) ++n;
    return n;
}

// Tame drivers: no flight from people; out to graze by day, in by night,
// kept in when there is nothing to graze.
void livestockThink(World& w, Character& a, const SpeciesDef& s) {
    double& hunger = a.needs["hunger"];
    const bool graze = grazesOn(s, dayOfYear(w));
    if (w.isNight() || !graze) {
        if (dist(a.pos, a.den) > 5.0) moveToward(a, a.den, s.speedMPerTick * 0.6);
        return;
    }
    if (hunger < 0.95 && inBiome(w, s, a.pos)) hunger = clamp01(hunger + 2.5 / static_cast<double>(kTicksPerDay));
    Rng r = w.rng(a.id, "graze");
    if (r.uniform() < 0.2) {
        const double ang = r.range(0.0, 2.0 * kPi);
        const double rad = r.range(0.0, std::max(20.0, s.territoryM));
        const Vec2 to = clampToMap(w, {a.den.x + std::cos(ang) * rad, a.den.y + std::sin(ang) * rad});
        if (inBiome(w, s, to)) moveToward(a, to, s.speedMPerTick * 0.5);
    }
}

void livestockDaily(World& w, Character& a, const SpeciesDef& s) {
    const int doy = dayOfYear(w);
    double& hunger = a.needs["hunger"];
    const int heads = std::max(0, a.group);
    // The trough: fodder when the grass is gone.
    if (!grazesOn(s, doy) && !s.fodder.empty() && s.fodderPerHead > 0.0 && heads > 0) {
        const double need = heads * s.fodderPerHead;
        auto it = a.inventory.find(s.fodder);
        const int have = it == a.inventory.end() ? 0 : it->second;
        // Fodder comes in whole units (a bundle of hay, a handful of grain):
        // a unit taken from the trough is eaten over the next days
        // (trough_credit, in units).
        double& credit = a.needs["trough_credit"];
        int taken = 0;
        while (credit < need && taken < have) {
            credit += 1.0;
            ++taken;
        }
        if (taken > 0) {
            if (have - taken <= 0) a.inventory.erase(s.fodder);
            else a.inventory[s.fodder] = have - taken;
        }
        const double fed = std::min(credit, need);
        credit -= fed;
        hunger = clamp01(hunger + s.hungerPerDay * 1.4 * fed / need);
    }
    // Products of fed animals in their season (left uncollected, they stop).
    if (hunger > 0.4)
        for (const auto& p : s.products) {
            if (!inDays(doy, p.from, p.to)) continue;
            const int cap = static_cast<int>(std::round(heads * p.capPerHead));
            auto it = a.inventory.find(p.item);
            const int have = it == a.inventory.end() ? 0 : it->second;
            const int n = std::min(cap, have + roundChance(w, a.id, "lay:" + p.item, heads * p.perHeadDay));
            if (n > 0) a.inventory[p.item] = n;
        }
    // Young once a year, in the birth season.
    if (s.breedFrom >= 0 && inDays(doy, s.breedFrom, s.breedTo) && hunger > 0.45 && heads >= 1 &&
        a.group < s.maxGroup && !a.hasMoodlet("bred", w.now())) {
        const int len = (s.breedTo - s.breedFrom + kDaysPerYear) % kDaysPerYear + 1;
        Rng r = w.rng(a.id, "breed");
        if (r.uniform() < 1.5 / len) {
            const int mothers = std::max(1, heads / 2);
            int young = 0;
            for (int m = 0; m < mothers; ++m) young += r.irange(s.broodLo, s.broodHi);
            a.group = std::min(s.maxGroup, a.group + young);
            a.moodlets.push_back({"bred", 0.0, w.now() + static_cast<Tick>(len + 30) * kTicksPerDay});
        }
    }
}

const SpeciesDef* tameSpecies(const World& w, const Character& a) {
    if (!a.isAnimal() || !a.body.alive) return nullptr;
    const SpeciesDef* s = w.content().speciesOf(a.species);
    return s && s->domestic ? s : nullptr;
}

int invOf(const Character& a, const std::string& item) {
    auto it = a.inventory.find(item);
    return it == a.inventory.end() ? 0 : it->second;
}

double herdOf(const World& w, const Character& a, const SpeciesDef& s, const std::string& key) {
    const int doy = dayOfYear(w);
    const int heads = std::max(0, a.group);
    if (key == "heads") return heads;
    if (key == "trough") {
        if (grazesOn(s, doy) || s.fodder.empty()) return 0.0;
        return std::max(0.0, std::ceil(heads * s.fodderPerHead * 3.0) - invOf(a, s.fodder)); // three days' worth
    }
    if (key == "cull") {
        int over = 0;
        if (s.cullFrom >= 0 && inDays(doy, s.cullFrom, s.cullTo)) over = heads - std::max(1, s.keep);
        // Not enough fodder put by for the rest of the stalled season: the
        // ones it cannot carry go to the larder before they starve.
        if (!grazesOn(s, doy) && !s.fodder.empty() && s.fodderPerHead > 0.0) {
            double have = invOf(a, s.fodder);
            if (auto f = w.families().find(a.status.family); f != w.families().end())
                if (const WorldObject* st = w.object(f->second.storage)) {
                    auto it = st->stock.find(s.fodder);
                    if (it != st->stock.end()) have += it->second;
                }
            const int carry = static_cast<int>(have / (s.fodderPerHead * std::max(1, stalledDaysLeft(s, doy))));
            if (carry < heads) over = std::max(over, heads - std::max(1, carry));
            if (a.need("hunger") < 0.25 && invOf(a, s.fodder) == 0) over = std::max(over, heads > 1 ? 1 : 0);
        }
        return std::max(0, over);
    }
    if (key == "fodder_need") {
        if (s.fodder.empty() || s.fodderPerHead <= 0.0) return 0.0;
        const int kept = s.keep > 0 ? std::min(heads, s.keep) : heads;
        const int days = grazesOn(s, doy) ? stalledDaysPerYear(s) : stalledDaysLeft(s, doy);
        return kept * s.fodderPerHead * days - invOf(a, s.fodder);
    }
    for (const auto& p : s.products)
        if (p.item == key) return invOf(a, key);
    return 0.0;
}

} // namespace

namespace {

// "<fodder>_need" (hay_need, acorns_need): the fodder_need of one fodder.
// "<species>_trough" (goats_trough): the trough of one kind of beast.
std::string speciesOfTroughKey(const std::string& key) {
    const std::string suf = "_trough";
    if (key.size() <= suf.size() || key.compare(key.size() - suf.size(), suf.size(), suf) != 0) return {};
    return key.substr(0, key.size() - suf.size());
}

std::string fodderOfKey(const std::string& key) {
    const std::string suf = "_need";
    if (key.size() <= suf.size() || key.compare(key.size() - suf.size(), suf.size(), suf) != 0) return {};
    return key.substr(0, key.size() - suf.size());
}

} // namespace

bool isHerdKey(const Content& ct, const std::string& key) {
    if (key == "work" || key == "trough" || key == "fodder_need" || key == "cull" || key == "heads") return true;
    const std::string fodder = fodderOfKey(key);
    const std::string troughOf = speciesOfTroughKey(key);
    if (!troughOf.empty()) {
        const auto sp = ct.species.find(troughOf);
        return sp != ct.species.end() && sp->second.domestic;
    }
    for (const auto& [id, s] : ct.species) {
        if (!fodder.empty() && s.fodder == fodder) return true;
        for (const auto& p : s.products)
            if (p.item == key) return true;
    }
    return false;
}

double herdValue(const World& w, const Character& c, const Character* target, const std::string& key) {
    if (target) {
        const SpeciesDef* s = tameSpecies(w, *target);
        if (!s || target->status.family != c.status.family || key == "work") return 0.0;
        return herdOf(w, *target, *s, key);
    }
    if (c.status.family == kNone) return 0.0;
    const WorldObject* home = w.object(c.home);
    auto homeHas = [&](const std::string& item) {
        if (!home) return 0;
        auto it = home->stock.find(item);
        return it == home->stock.end() ? 0 : it->second;
    };
    const int doy = dayOfYear(w);
    const std::string onlyFodder = key == "fodder_need" ? std::string() : fodderOfKey(key);
    const bool needKey = key == "fodder_need" || !onlyFodder.empty();
    double sum = 0.0, cull = 0.0, product = 0.0;
    std::map<std::string, double> needByFodder;
    bool troughFillable = false;
    for (const auto& [id, a] : w.characters()) {
        const SpeciesDef* s = tameSpecies(w, a);
        if (!s || a.status.family != c.status.family) continue;
        if (needKey) {
            if (onlyFodder.empty() || s->fodder == onlyFodder) needByFodder[s->fodder] += herdOf(w, a, *s, "fodder_need");
            continue;
        }
        if (key != "work") {
            const std::string troughOf = speciesOfTroughKey(key);
            if (troughOf.empty()) sum += herdOf(w, a, *s, key);
            else if (a.species == troughOf) sum += herdOf(w, a, *s, "trough");
            continue;
        }
        if (herdOf(w, a, *s, "trough") > 0.0 && homeHas(s->fodder) >= 1) troughFillable = true;
        cull += herdOf(w, a, *s, "cull");
        for (const auto& p : s->products) product += herdOf(w, a, *s, p.item);
        if (grazesOn(*s, doy)) needByFodder[s->fodder] += herdOf(w, a, *s, "fodder_need");
    }
    double fodderNeed = 0.0;
    for (const auto& [item, n] : needByFodder) fodderNeed += std::max(0.0, n - homeHas(item));
    if (needKey) return fodderNeed;
    if (key != "work") return sum;
    // How pressing the chores are today, 0..1 (utility input herd:work).
    if (troughFillable) return 0.9;    // hungry beasts in the byre
    if (product >= 3.0) return 0.6;    // eggs and milk will not keep
    if (cull > 0.0) return 0.55;       // the slaughter season
    if (fodderNeed > 0.0) return 0.45; // hay and mast for the winter
    return 0.0;
}

void spawnLivestock(World& w, std::uint64_t seed) {
    for (const auto& [sid, s] : w.content().species) {
        if (!s.domestic || s.perFamilyHi <= 0) continue;
        for (const auto& [fid, fam] : w.families()) {
            const WorldObject* store = w.object(fam.storage);
            if (!store) continue;
            Rng r(hashCombine(seed, hashCombine(static_cast<std::uint64_t>(fid), hashStr("livestock:" + sid))));
            if (!r.chance(s.perFamilyChance)) continue;
            const int heads = r.irange(s.perFamilyLo, s.perFamilyHi);
            if (heads <= 0) continue;
            Character a;
            a.species = sid;
            a.name = sid;
            a.group = heads;
            a.den = {store->pos.x + r.range(-6.0, 6.0), store->pos.y + r.range(-6.0, 6.0)};
            a.pos = a.den;
            a.status.family = fid;
            a.needs["hunger"] = r.range(0.6, 0.9);
            a.body.ageDays = 2 * kDaysPerYear;
            a.body.sex = 'f';
            w.addCharacter(std::move(a));
            // Last year's fodder in the barn (stand-in until history generation):
            // the hay and mast put by for the stalled season - none yet before
            // the making season, half in it, all of it after; in the stalled
            // season what is left of it.
            auto fd = w.content().items.find(s.fodder);
            if (s.fodder.empty() || s.fodderPerHead <= 0.0 || fd == w.content().items.end() || fd->second.nutrition > 0.0)
                continue;
            const int doy = w.startDay() % kDaysPerYear;
            // Heads still to be culled this season are not fed through the winter.
            const bool culling = s.cullFrom >= 0 && (grazesOn(s, doy) || inDays(doy, s.cullFrom, s.cullTo));
            const int kept = s.keep > 0 && culling ? std::min(heads, s.keep) : heads;
            double days = stalledDaysLeft(s, doy);
            if (days <= 0.0) {
                const std::string season = seasonOfDay(doy);
                const double made = season == "spring" ? 0.0 : (season == "summer" ? 0.5 : 1.0);
                days = made * stalledDaysPerYear(s);
            }
            const int put = static_cast<int>(std::round(kept * s.fodderPerHead * days * r.range(0.8, 1.15)));
            if (put > 0)
                if (WorldObject* st = w.object(fam.storage)) st->stock[s.fodder] += put;
        }
    }
}

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
    if (s.domestic) return livestockThink(w, a, s);

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
                            !a.hasMoodlet("sated", now) &&
                            w.rng(a.id, "dare").uniform() < s.aggression;
        if (attack) {
            moveToward(a, person->pos, s.speedMPerTick);
            if (dist(a.pos, person->pos) <= 5.0) {
                Character& victim = *w.character(person->id);
                if (!packFight(w, a, victim)) bite(w, a, victim, s);
            }
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
        if (s.domestic) {
            livestockDaily(w, a, s);
            continue;
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
                if (st.full() && dist(a.den, st.center) < 2500.0) {
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
                        if (st.full() && dist(a.pos, st.center) < 400.0 && s.habitat != "water") nearVillage = true;
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
