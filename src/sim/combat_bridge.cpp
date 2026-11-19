#include "sim/combat_bridge.hpp"

#include <filesystem>
#include <fstream>
#include <mutex>

#include "sim/character.hpp"
#include "sim/world.hpp"

namespace altera::sim {

namespace fs = std::filesystem;

const CombatBridge* combatBridge(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<CombatBridge>> cache; // by content dir; null = off
    if (c.dir.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(c.dir);
    if (it != cache.end()) return it->second.get();
    std::unique_ptr<CombatBridge> b;
    const fs::path dir = fs::path(c.dir) / ".." / "combat";
    if (fs::exists(dir / "combat_config.json") && fs::exists(dir / "sim_bridge.json")) {
        b = std::make_unique<CombatBridge>();
        b->data = std::make_shared<combat::CombatData>(combat::CombatData::load(dir.string()));
        std::ifstream f(dir / "sim_bridge.json", std::ios::binary);
        const Json j = Json::parse(f);
        const auto& d = *b->data;
        std::vector<std::string> err;
        b->skillScale = j.value("skill_scale", b->skillScale);
        b->defaultSkill = j.value("default_skill", b->defaultSkill);
        b->healthMinAfter = j.value("health_min_after", b->healthMinAfter);
        b->weaponItems = j.value("weapon_items", std::vector<std::string>{});
        b->weaponOfItem = j.value("weapon_of_item", std::map<std::string, std::string>{});
        b->armorOfItem = j.value("armor_of_item", std::map<std::string, std::string>{});
        if (j.contains("animal")) {
            b->animalWeapon = j["animal"].value("weapon", b->animalWeapon);
            b->animalSkillKey = j["animal"].value("skill_key", b->animalSkillKey);
            b->animalDefaultSkill = j["animal"].value("default_skill", b->animalDefaultSkill);
        }
        b->memory = j.value("memory", std::map<std::string, std::string>{});
        b->memoryValence = j.value("memory_valence", std::map<std::string, double>{});
        b->chronicle = j.value("chronicle", b->chronicle);
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
        if (fs::exists(dir / "recovery.json")) {
            std::ifstream rf(dir / "recovery.json", std::ios::binary);
            b->recovery = loadRecovery(Json::parse(rf), d, c, err);
        } else {
            err.push_back("recovery.json: missing next to sim_bridge.json");
        }
        if (!err.empty()) throw combat::DataError(err);
    }
    return (cache[c.dir] = std::move(b)).get();
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
        if (inv != c.inventory.end() && inv->second > 0) s.armor.push_back(armor);
    }
    return s;
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

CombatResolution resolveCombat(const World& w, const Character& actor, const InteractionDef& d, const Character& target,
                               combat::EventStream* events) {
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
    r.rounds = e.fight(a, t);
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
    return r;
}

} // namespace altera::sim
