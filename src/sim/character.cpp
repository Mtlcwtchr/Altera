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

double Character::mood(Tick now) const {
    double m = 0.0;
    for (const auto& md : moodlets)
        if (md.until > now) m += md.value;
    double needSum = 0.0;
    for (const auto& [k, v] : needs) needSum += v;
    if (!needs.empty()) m += (needSum / static_cast<double>(needs.size()) - 0.5) * 0.6;
    return std::clamp(m, -1.0, 1.0);
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Body, health, fatigue, ageDays, sex, alive, causeOfDeath)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Moodlet, id, value, until)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MemoryEntry, tick, type, who, object, valence, salience, source,
                                                confidence)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Relation, familiarity, trust, affinity, fear, debt)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Status, family, dynasty, settlement, role, spouse, mother, father,
                                                children)
NLOHMANN_JSON_SERIALIZE_ENUM(Controller::Kind, {{Controller::Kind::Ai, "ai"}, {Controller::Kind::Player, "player"}})
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Controller, kind, playerId, freeWill, since)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(GoalInstance, id, source, score, started, task, target)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PlanStep, kind, ref, achieve, allow, method)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PlanAction, interaction, target, targetType, cost)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Plan, methods, steps, stepIndex, actions, actionIndex)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FsmState, state, interaction, target, dest, timer, started)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WeightMod, target, delta, created, ttl, decay, reason, source)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(QueuedTask, task, target, priority, created, ttl, reason, source)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TraceEntry, tick, layer, text)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MethodFail, count, until)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(AiStats, goalTicks, failReasons, idleTicks, starvingTicks, replans,
                                                goalSwitches, planFailures, idleReasons)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(AiState, goal, plan, fsm, suspended, suspendedGoal, suspendedPlan, mods,
                                                queue, methodFails, goalCooldown, trace, lastFailReason, nextThink,
                                                stats)

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
             {"status", c.status},
             {"inventory", c.inventory},
             {"pos", c.pos},
             {"home", c.home},
             {"controller", c.controller},
             {"ai", c.ai}};
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
    c.knowledge = j.value("knowledge", std::set<std::string>{});
    c.knownObjects = j.value("known_objects", std::set<EntityId>{});
    c.memory = j.value("memory", std::vector<MemoryEntry>{});
    c.relations = j.value("relations", std::map<EntityId, Relation>{});
    c.status = j.value("status", Status{});
    c.inventory = j.value("inventory", std::map<std::string, int>{});
    c.pos = j.value("pos", Vec2{});
    c.home = j.value("home", kNone);
    c.controller = j.value("controller", Controller{});
    c.ai = j.value("ai", AiState{});
}

} // namespace altera::sim
