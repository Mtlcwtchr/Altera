#include "sim/combat_show.hpp"

#include <cmath>

#include "sim/combat_bridge.hpp"
#include "sim/world.hpp"

namespace lr::sim {

void recordFight(World& w, const Character& actor, const Character& target, const std::string& act,
                 const CombatResolution& cr, std::size_t keep) {
    if (!w.client.recordFights || cr.eventLines.empty()) return;
    Json ev = Json::array();
    for (const auto& line : cr.eventLines) {
        Json e = Json::parse(line, nullptr, false);
        if (!e.is_discarded()) ev.push_back(std::move(e));
    }
    double roundMs = 1500.0;
    if (const CombatBridge* b = combatBridge(w.content()); b && b->data) roundMs = b->data->roundSeconds * 1000.0;
    Json f = {{"id", w.client.nextFight++},
              {"tick", w.now()},
              {"act", act},
              {"a", actor.id},
              {"b", target.id},
              {"pos_a", {actor.pos.x, actor.pos.y}},
              {"pos_b", {target.pos.x, target.pos.y}},
              {"weapon_a", cr.actorWeapon},
              {"weapon_b", cr.targetWeapon},
              {"outcome", cr.outcome},
              {"a_state", cr.actorState},
              {"b_state", cr.targetState},
              {"rounds", cr.rounds},
              {"round_ms", roundMs},
              {"ev", std::move(ev)}};
    w.client.fights.push_back(std::move(f));
    while (w.client.fights.size() > keep) w.client.fights.pop_front();
}

Json fightsSince(const World& w, std::uint64_t sinceId, double x, double y, double radius) {
    Json out = Json::array();
    for (const Json& f : w.client.fights) {
        if (f.value("id", std::uint64_t{0}) <= sinceId) continue;
        if (radius > 0) {
            const auto& p = f["pos_a"];
            if (std::hypot(p[0].get<double>() - x, p[1].get<double>() - y) > radius) continue;
        }
        out.push_back(f);
    }
    return out;
}

} // namespace lr::sim
