#include "sim/combat_bridge.hpp"
#include "sim/data_fs.hpp"

#include <algorithm>
#include <filesystem>
#include <set>
#include <fstream>
#include <mutex>

#include "sim/character.hpp"
#include "sim/gear.hpp"
#include "sim/world.hpp"

namespace lr::sim {

namespace fs = std::filesystem;

const CombatBridge* combatBridge(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<CombatBridge>> cache; // by content dir; null = off
    if (c.dir.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(data::cacheKey(c.dir));
    if (it != cache.end()) return it->second.get();
    std::unique_ptr<CombatBridge> b;
    const fs::path dir = fs::path(c.dir) / ".." / "combat";
    if (data::exists(dir / "combat_config.json") && data::exists(dir / "sim_bridge.json")) {
        b = std::make_unique<CombatBridge>();
        b->data = std::make_shared<combat::CombatData>(combat::CombatData::load(dir.string()));
        data::InFile f(dir / "sim_bridge.json", std::ios::binary);
        const Json j = Json::parse(f);
        const auto& d = *b->data;
        std::vector<std::string> err;
        b->skillScale = j.value("skill_scale", b->skillScale);
        b->defaultSkill = j.value("default_skill", b->defaultSkill);
        b->healthMinAfter = j.value("health_min_after", b->healthMinAfter);
        b->weaponItems = j.value("weapon_items", std::vector<std::string>{});
        b->weaponOfItem = j.value("weapon_of_item", std::map<std::string, std::string>{});
        b->armorOfItem = j.value("armor_of_item", std::map<std::string, std::string>{});
        for (const auto& f : c.craft.forms)
            if (!f.item.empty()) b->itemOfForm[f.id] = f.item;
        if (const GearRules* R = gearRules(c))
            for (const auto& [id, g] : R->items)
                if (!g.slot.empty()) b->wearable.insert(id);
        if (j.contains("animal")) {
            b->animalWeapon = j["animal"].value("weapon", b->animalWeapon);
            b->animalSkillKey = j["animal"].value("skill_key", b->animalSkillKey);
            b->animalDefaultSkill = j["animal"].value("default_skill", b->animalDefaultSkill);
        }
        b->memory = j.value("memory", std::map<std::string, std::string>{});
        b->memoryValence = j.value("memory_valence", std::map<std::string, double>{});
        b->chronicle = j.value("chronicle", b->chronicle);
        if (j.contains("lod")) {
            const Json& l = j["lod"];
            b->lodEnabled = l.value("enabled", false);
            b->lodFullRadiusM = l.value("full_radius_m", b->lodFullRadiusM);
            combat::LodConfig lc;
            lc.samples = l.value("samples", lc.samples);
            lc.skillStep = l.value("skill_step", lc.skillStep);
            if (lc.samples < 1) err.push_back("sim_bridge.lod.samples: at least 1");
            b->lod = std::make_shared<combat::DuelTable>(*b->data, lc);
        }
        for (const auto& [item, w] : b->weaponOfItem) {
            if (!d.hasWeapon(w)) err.push_back("sim_bridge.weapon_of_item." + item + ": unknown weapon '" + w + "'");
            if (!c.items.count(item)) err.push_back("sim_bridge.weapon_of_item: unknown item '" + item + "'");
        }
        for (const auto& item : b->weaponItems)
            if (!b->weaponOfItem.count(item)) err.push_back("sim_bridge.weapon_items: '" + item + "' has no weapon_of_item");
        for (const auto& [item, a] : b->armorOfItem) {
            if (!d.armorIx.count(a)) err.push_back("sim_bridge.armor_of_item." + item + ": unknown armor '" + a + "'");
            if (!c.items.count(item)) err.push_back("sim_bridge.armor_of_item: unknown item '" + item + "'");
        }
        if (!d.hasWeapon(b->animalWeapon)) err.push_back("sim_bridge.animal.weapon: unknown weapon '" + b->animalWeapon + "'");
        for (const char* o : {"win", "lose", "draw", "flee"})
            if (!b->memory.count(o)) err.push_back(std::string("sim_bridge.memory: no type for '") + o + "'");
        if (data::exists(dir / "recovery.json")) {
            data::InFile rf(dir / "recovery.json", std::ios::binary);
            b->recovery = loadRecovery(Json::parse(rf), d, c, err);
        } else {
            err.push_back("recovery.json: missing next to sim_bridge.json");
        }
        for (const char* f : {"aftermath.json", "rescue.json"}) {
            if (!data::exists(dir / f)) {
                err.push_back(std::string(f) + ": missing next to sim_bridge.json");
                continue;
            }
            data::InFile af(dir / f, std::ios::binary);
            const Json aj = Json::parse(af);
            if (std::string(f) == "aftermath.json") b->aftermath = loadAftermath(aj, c, err);
            else b->rescue = loadRescue(aj, c, err);
        }
        for (const char* f : {"group.json", "captivity.json"}) {
            if (!data::exists(dir / f)) continue; // optional: without them, duels only / no captivity follow-up
            data::InFile gf(dir / f, std::ios::binary);
            const Json gj = Json::parse(gf);
            if (std::string(f) == "group.json") b->group = loadGroupRules(gj, c, err);
            else b->captivity = loadCaptivity(gj, c, err);
        }
        for (const auto& rr : b->rescue.rescuers)
            if (!d.treatIx.count(rr.treatment))
                err.push_back("rescue.rescuers." + rr.id + ".treatment: unknown treatment '" + rr.treatment + "'");
        if (!err.empty()) throw combat::DataError(err);
    }
    return (cache[data::cacheKey(c.dir)] = std::move(b)).get();
}

combat::FighterSpec fighterOf(const CombatBridge& b, const Character& c, const std::string& skillKey) {
    combat::FighterSpec s;
    s.armor.clear();
    s.shield.clear();
    if (c.isAnimal()) {
        auto it = c.skills.find(b.animalSkillKey);
        s.skill = b.skillScale * (it == c.skills.end() ? b.animalDefaultSkill : it->second);
        s.weapon = b.animalWeapon;
        return s;
    }
    auto it = c.skills.find(skillKey);
    s.skill = b.skillScale * (it == c.skills.end() ? b.defaultSkill : it->second);
    s.weapon = b.data->weapons[b.data->core.unarmed].id;
    for (const auto& item : b.weaponItems) {
        auto inv = c.inventory.find(item);
        if (inv != c.inventory.end() && inv->second > 0) {
            s.weapon = b.weaponOfItem.at(item);
            break;
        }
    }
    for (const auto& [item, armor] : b.armorOfItem) {
        auto inv = c.inventory.find(item);
        if (inv == c.inventory.end() || inv->second <= 0) continue;
        if (b.wearable.count(item) && wornCount(c, item) == 0) continue;   // in the bag, not on the body
        s.armor.push_back(armor);
    }
    applyMadeGear(b, c, s);
    return s;
}

void wearMadeWeapon(Character& c, double wear) {
    if (wear <= 0.0) return;
    craft::Item* best = nullptr;
    double bestDmg = 0.0;
    for (auto& it : c.craft.items) {
        if (it.kind != "weapon" || it.wear >= 1.0) continue;
        auto dm = it.derived.find("damage_mult");
        double dmg = dm == it.derived.end() ? 1.0 : dm->second;
        if (!best || dmg > bestDmg) {
            best = &it;
            bestDmg = dmg;
        }
    }
    if (best) best->wear = std::min(1.0, best->wear + wear);
}

void applyMadeGear(const CombatBridge& b, const Character& c, combat::FighterSpec& s) {
    // Made things (crafting derive, gdd crafting §10): the best carried weapon
    // replaces the item-based one with its own numbers; each made armour layer
    // the data knows is worn with its quality and gaps. Worn-out things do not count.
    const craft::Item* best = nullptr;
    double bestDmg = 0.0;
    for (const auto& it : c.craft.items) {
        if (it.kind != "weapon" || it.wear >= 1.0) continue;
        auto w = it.derivedStr.find("weapon_id");
        if (w == it.derivedStr.end() || !b.data->hasWeapon(w->second)) continue;
        auto dm = it.derived.find("damage_mult");
        double dmg = dm == it.derived.end() ? 1.0 : dm->second;
        if (!best || dmg > bestDmg) {
            best = &it;
            bestDmg = dmg;
        }
    }
    if (best) {
        const std::string& wid = best->derivedStr.at("weapon_id");
        s.weapon = wid;
        s.damageMult = bestDmg * (1.0 - 0.5 * best->wear);
        auto du = best->derived.find("durability");
        const double base = b.data->weapons[b.data->weapon(wid)].durability;
        if (du != best->derived.end() && base > 0) s.durabilityMult = std::max(0.1, du->second / base);
    }
    std::set<std::string> layers;
    for (const auto& it : c.craft.items) {
        if (it.kind != "armor" || it.wear >= 1.0) continue;
        if (const auto fi = b.itemOfForm.find(it.form);
            fi != b.itemOfForm.end() && b.wearable.count(fi->second) && wornCount(c, fi->second) == 0)
            continue;   // AL-109: carried, not worn
        auto l = it.derivedStr.find("layer");
        if (l == it.derivedStr.end() || !b.data->armorIx.count(l->second) || !layers.insert(l->second).second) continue;
        auto have = std::find(s.armor.begin(), s.armor.end(), l->second);
        size_t i = static_cast<size_t>(have - s.armor.begin());
        if (have == s.armor.end()) {
            s.armor.push_back(l->second);
            i = s.armor.size() - 1;
        }
        s.armorEff.resize(s.armor.size(), 1.0);
        s.armorGap.resize(s.armor.size(), 0.0);
        s.armorEff[i] = std::clamp((0.6 + 0.6 * it.q) * (1.0 - 0.5 * it.wear), 0.3, 1.2);
        auto g = it.derived.find("gap");
        if (g != it.derived.end()) s.armorGap[i] = std::max(0.0, g->second);
    }
}

namespace {

// Share of health this fight took: zone hp lost since the start (old wounds
// already count in body.health), over the whole body.
double healthLeft(const combat::CombatData& d, const combat::Fighter& f, const std::vector<double>& hpStart) {
    double lost = 0.0, total = 0.0;
    for (std::size_t z = 0; z < d.zoneHp.size(); ++z) {
        total += d.zoneHp[z];
        lost += std::max(0.0, std::min(hpStart[z], hpStart[z] - f.hp[z]));
    }
    return total > 0 ? 1.0 - lost / total : 1.0;
}

} // namespace

bool combatNeedsFullMode(const World& w, const CombatBridge& b, const Character& actor, const Character& target) {
    if (!b.lodEnabled || !b.lod) return true;
    for (const Character* c : {&actor, &target}) {
        if (c->controller.isPlayer() || !c->wounds.empty() || !c->injuries.empty()) return true;
        if (c->body.blood < b.data->bloodMax) return true;
    }
    for (const auto& [id, c] : w.characters())
        if (c.body.alive && c.controller.isPlayer() &&
            (dist(c.pos, actor.pos) <= b.lodFullRadiusM || dist(c.pos, target.pos) <= b.lodFullRadiusM))
            return true;
    return false;
}

CombatResolution resolveCombat(const World& w, const Character& actor, const InteractionDef& d, const Character& target,
                               combat::EventStream* events, const std::string& mode) {
    CombatResolution r;
    const CombatBridge* b = combatBridge(w.content());
    if (!b) return r;
    const combat::CombatData& cd = *b->data;
    const std::uint64_t pair = (static_cast<std::uint64_t>(actor.id) << 32) ^ static_cast<std::uint64_t>(target.id);
    combat::Engine e(cd, combat::combatSeed(w.seed(), pair, static_cast<std::uint64_t>(w.now())), events);
    combat::Fighter a = e.make(fighterOf(*b, actor, d.combat.attackSkill), static_cast<std::int64_t>(actor.id), 0);
    combat::Fighter t = e.make(fighterOf(*b, target, d.combat.defenseSkill), static_cast<std::int64_t>(target.id), 1);
    carryWounds(cd, b->recovery, actor, a);
    carryWounds(cd, b->recovery, target, t);
    const std::vector<double> hpA = a.hp, hpT = t.hp;
    r.ran = true;
    const bool full = mode == "full" || (mode != "stat" && (events || combatNeedsFullMode(w, *b, actor, target)));
    if (full || !b->lod) {
        r.rounds = e.fight(a, t);
    } else {
        // Statistical resolution: a sample of the same core's results for this match-up.
        combat::Rng rng(combat::combatSeed(w.seed(), pair ^ 0x5747u, static_cast<std::uint64_t>(w.now())));
        const combat::DuelSample& s = b->lod->resolve(fighterOf(*b, actor, d.combat.attackSkill),
                                                      fighterOf(*b, target, d.combat.defenseSkill), rng);
        b->lod->apply(s, a, t);
        r.rounds = s.rounds;
        r.mode = "stat";
    }
    r.actorState = combat::stateId(a.state);
    r.targetState = combat::stateId(t.state);
    if (a.fighting() && !t.fighting()) r.outcome = "win";
    else if (a.state == combat::State::Flee) r.outcome = "flee";
    else if (!a.fighting() && t.fighting()) r.outcome = "lose";
    else r.outcome = "draw";
    r.actorDead = a.state == combat::State::Dead;
    r.targetDead = t.state == combat::State::Dead;
    r.actorHealth = healthLeft(cd, a, hpA);
    r.targetHealth = healthLeft(cd, t, hpT);
    r.actorHits = woundHits(a);
    r.targetHits = woundHits(t);
    r.actorBlood = a.blood;
    r.targetBlood = t.blood;
    for (int s : a.wounds) r.actorWounds.push_back(cd.severity[s].id);
    for (int s : t.wounds) r.targetWounds.push_back(cd.severity[s].id);
    r.actorWeapon = cd.weapons[a.weapon].id;
    r.targetWeapon = cd.weapons[t.weapon].id;
    r.actorWeaponBroke = a.broke > 0;
    r.targetWeaponBroke = t.broke > 0;
    r.actorWeaponWear = a.broke > 0 ? 1.0 : 1.0 - a.weaponCond;
    r.targetWeaponWear = t.broke > 0 ? 1.0 : 1.0 - t.weaponCond;
    return r;
}

} // namespace lr::sim
