#include "sim/character.hpp"

#include <algorithm>

namespace altera::sim {

bool Character::hasTrait(const std::string& t) const { return std::find(traits.begin(), traits.end(), t) != traits.end(); }

int Character::has(const std::string& item) const {
    auto it = inventory.find(item);
    return it == inventory.end() ? 0 : it->second;
}

double Character::need(const std::string& n) const {
    auto it = needs.find(n);
    return it == needs.end() ? 1.0 : it->second;
}

bool Character::hasMoodlet(const std::string& id, Tick now) const {
    for (const auto& m : moodlets)
        if (m.id == id && m.until > now) return true;
    return false;
}

double* Relation::axis(const std::string& a) {
    if (a == "familiarity") return &familiarity;
    if (a == "trust") return &trust;
    if (a == "affinity") return &affinity;
    if (a == "fear") return &fear;
    if (a == "debt") return &debt;
    if (a == "enmity") return &enmity;
    if (a == "respect") return &respect;
    if (a == "attraction") return &attraction;
    return nullptr;
}

double Relation::get(const std::string& a) const {
    double* p = const_cast<Relation*>(this)->axis(a);
    return p ? *p : 0.0;
}

double Character::mood(Tick now) const {
    double m = 0.0;
    for (const auto& md : moodlets)
        if (md.until > now) m += md.value;
    double needSum = 0.0;
    for (const auto& [k, v] : needs) needSum += v;
    if (!needs.empty()) m += (needSum / static_cast<double>(needs.size()) - 0.5) * 0.6;
    return std::clamp(m, -1.0, 1.0);
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Body, health, fatigue, ageDays, sex, alive, causeOfDeath, blood,
                                                downUntil)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Wound, zone, severity, dmg, healthDebt, daysTotal, daysLeft, treatment,
                                                bleeding, aided, infectionRolled, infected, since, feverAt)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Injury, zone, kind, since)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Moodlet, id, value, until)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MemoryEntry, tick, type, who, object, valence, salience, source,
                                                confidence, chain, fact, goods, prop)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Relation, familiarity, trust, affinity, fear, debt, enmity, respect,
                                                attraction)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Status, family, dynasty, settlement, role, spouse, mother, father,
                                                children, pairs, rights, verdicts)

bool Status::hasPair(EntityId other, const std::string& id) const {
    if (id == "married" && spouse != kNone && spouse == other) return true;
    auto it = pairs.find(other);
    return it != pairs.end() && it->second.count(id) > 0;
}
bool Status::hasPairAny(const std::string& id) const {
    if (id == "married" && spouse != kNone) return true;
    for (const auto& [o, s] : pairs)
        if (s.count(id)) return true;
    return false;
}
NLOHMANN_JSON_SERIALIZE_ENUM(Controller::Kind, {{Controller::Kind::Ai, "ai"}, {Controller::Kind::Player, "player"}})
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Controller, kind, playerId, freeWill, since, idleSince,
                                                combatFinish, tone)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Captivity, captor, since, decideAt, nextTry, stage, verdict, tries)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(GoalInstance, id, source, score, started, task, target)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PlanStep, kind, ref, achieve, allow, method, params)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PlanAction, interaction, target, targetType, cost)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Plan, methods, steps, stepIndex, actions, actionIndex, stepReplans,
                                                htnReplans, waits)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FsmState, state, interaction, target, dest, timer, started, anim)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SocialLink, chain, role, partner, phase, stage, until, since, expires,
                                                place, tone, result)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WeightMod, target, delta, created, ttl, decay, reason, source)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(QueuedTask, task, target, priority, created, ttl, reason, source)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TraceEntry, tick, layer, code, params)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MethodFail, count, until)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(AiStats, goalTicks, failReasons, idleTicks, starvingTicks, replans,
                                                goalSwitches, planFailures, idleReasons, lastGoalFail, methodUse)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(AiState, goal, plan, fsm, suspended, suspendedGoal, suspendedPlan, mods,
                                                queue, methodFails, goalCooldown, trace, lastFailReason, nextThink,
                                                stats, social, followAfter)

void to_json(Json& j, const Character& c) {
    j = Json{{"id", c.id},
             {"name", c.name},
             {"body", c.body},
             {"needs", c.needs},
             {"moodlets", c.moodlets},
             {"traits", c.traits},
             {"skills", c.skills},
             {"knowledge", c.knowledge},
             {"known_objects", c.knownObjects},
             {"memory", c.memory},
             {"relations", c.relations},
             {"reputation", c.reputation},
             {"status", c.status},
             {"inventory", c.inventory},
             {"wounds", c.wounds},
             {"injuries", c.injuries},
             {"pos", c.pos},
             {"home", c.home},
             {"controller", c.controller},
             {"ai", c.ai}};
    if (!c.species.empty()) {
        j["species"] = c.species;
        j["group"] = c.group;
        j["den"] = c.den;
        j["starve_days"] = c.starveDays;
    }
    if (c.captivity.held()) j["captivity"] = c.captivity;
    if (!c.craft.empty()) j["craft"] = c.craft;
    if (!c.attrs.empty()) j["attrs"] = c.attrs;
    if (!c.backstory.empty()) j["backstory"] = c.backstory;
    if (!c.worn.empty()) j["worn"] = c.worn;
    if (!c.gearPins.empty()) {
        Json p = Json::object();
        for (const auto& [k, v] : c.gearPins) p[k] = {{"grid", v.grid}, {"x", v.x}, {"y", v.y}, {"turned", v.turned}};
        j["gear_pins"] = p;
    }
}

void from_json(const Json& j, Character& c) {
    c = Character{};
    c.id = j.at("id").get<EntityId>();
    c.name = j.value("name", "");
    c.body = j.value("body", Body{});
    c.needs = j.value("needs", std::map<std::string, double>{});
    c.moodlets = j.value("moodlets", std::vector<Moodlet>{});
    c.traits = j.value("traits", std::vector<std::string>{});
    c.skills = j.value("skills", std::map<std::string, double>{});
    c.attrs = j.value("attrs", std::map<std::string, double>{});
    c.backstory = j.value("backstory", std::string());
    c.worn = j.value("worn", std::map<std::string, std::string>{});
    if (j.contains("gear_pins"))
        for (const auto& [k, v] : j["gear_pins"].items())
            c.gearPins[k] = {v.value("grid", std::string()), v.value("x", 0), v.value("y", 0), v.value("turned", false)};
    c.knowledge = j.value("knowledge", std::set<std::string>{});
    c.knownObjects = j.value("known_objects", std::set<EntityId>{});
    c.memory = j.value("memory", std::vector<MemoryEntry>{});
    c.relations = j.value("relations", std::map<EntityId, Relation>{});
    c.reputation = j.value("reputation", std::map<std::string, double>{});
    c.status = j.value("status", Status{});
    c.inventory = j.value("inventory", std::map<std::string, int>{});
    c.wounds = j.value("wounds", std::vector<Wound>{});
    c.injuries = j.value("injuries", std::vector<Injury>{});
    c.pos = j.value("pos", Vec2{});
    c.home = j.value("home", kNone);
    c.controller = j.value("controller", Controller{});
    c.ai = j.value("ai", AiState{});
    c.species = j.value("species", std::string());
    c.group = j.value("group", 1);
    c.den = j.value("den", Vec2{});
    c.starveDays = j.value("starve_days", 0);
    c.captivity = j.value("captivity", Captivity{});
    c.craft = j.value("craft", craft::CraftState{});
}

} // namespace altera::sim
