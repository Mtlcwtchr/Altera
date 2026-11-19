#include "sim/aftermath.hpp"

#include <algorithm>
#include <cmath>

#include "sim/ai.hpp"
#include "sim/character.hpp"
#include "sim/combat_bridge.hpp"
#include "sim/content.hpp"
#include "sim/world.hpp"

namespace altera::sim {

namespace {

constexpr Tick kTicksPerHour = kTicksPerDay / 24;

std::array<double, 2> span2(const Json& j, const char* key) {
    std::array<double, 2> out{0.0, 0.0};
    if (j.contains(key) && j[key].is_array() && j[key].size() == 2) out = {j[key][0].get<double>(), j[key][1].get<double>()};
    return out;
}

bool contains(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

void killChar(World& w, Character& who, const std::string& cause) {
    if (!who.body.alive) return;
    who.body.alive = false;
    who.body.health = 0.0;
    who.body.causeOfDeath = cause;
    w.chronicle("death", {who.id}, "chronicle.death", {{"who", who.id}, {"cause", cause}});
}

bool isPredator(const World& w, const Character& c) {
    if (!c.isAnimal()) return false;
    const SpeciesDef* sp = w.content().speciesOf(c.species);
    return sp && sp->diet == "prey";
}

bool kin(const Character& a, const Character& b) {
    return (a.status.family != kNone && a.status.family == b.status.family) ||
           (a.status.dynasty != kNone && a.status.dynasty == b.status.dynasty);
}

void memo(World& w, Character& who, const std::string& type, EntityId other, double valence) {
    if (type.empty() || !who.body.alive) return;
    MemoryEntry m;
    m.type = type;
    if (other != kNone) m.who = {other};
    m.valence = valence;
    w.remember(who, std::move(m));
}

} // namespace

AftermathRules loadAftermath(const Json& j, const Content& c, std::vector<std::string>& err) {
    AftermathRules r;
    r.loserStates = j.value("loser_states", std::vector<std::string>{"ko", "yield"});
    for (const auto& s : r.loserStates)
        if (s != "ko" && s != "yield") err.push_back("aftermath.loser_states: ko | yield, not '" + s + "'");
    r.noise = j.value("noise", 0.0);
    r.chronicle = j.value("chronicle", r.chronicle);
    const auto axes = relAxes();
    for (const auto& o : j.value("options", Json::array())) {
        FinishOption op;
        op.id = o.value("id", std::string());
        const std::string where = "aftermath.options." + op.id;
        if (op.id.empty()) err.push_back("aftermath.options: an option without id");
        op.base = o.value("base", 0.0);
        for (const auto& t : o.value("terms", Json::array())) {
            FinishTerm ft;
            ft.add = t.value("add", 0.0);
            ft.mul = t.value("mul", 0.0);
            if (t.contains("trait")) {
                ft.kind = "trait";
                ft.key = t["trait"].get<std::string>();
                if (!c.traits.count(ft.key)) err.push_back(where + ": unknown trait '" + ft.key + "'");
            } else if (t.contains("rel")) {
                ft.kind = "rel";
                ft.key = t["rel"].get<std::string>();
                if (!contains(axes, ft.key)) err.push_back(where + ": unknown rel axis '" + ft.key + "'");
            } else if (t.contains("act_lethal")) {
                ft.kind = "act_lethal";
                ft.flag = t["act_lethal"].get<bool>();
            } else if (t.contains("loser") || t.contains("winner")) {
                ft.kind = t.contains("loser") ? "loser" : "winner";
                ft.key = t[ft.kind].get<std::string>();
                if (ft.key != "person" && ft.key != "animal") err.push_back(where + ": " + ft.kind + " is person | animal");
            } else if (t.contains("winner_predator")) {
                ft.kind = "winner_predator";
                ft.flag = t["winner_predator"].get<bool>();
            } else if (t.contains("kin")) {
                ft.kind = "kin";
                ft.flag = t["kin"].get<bool>();
            } else if (t.contains("loser_state")) {
                ft.kind = "loser_state";
                ft.key = t["loser_state"].get<std::string>();
            } else if (t.contains("witnesses_each")) {
                ft.kind = "witnesses_each";
                ft.add = t["witnesses_each"].get<double>();
                ft.cap = t.value("cap", -1e9);
            } else if (t.contains("right")) {
                ft.kind = "right";
                ft.key = t["right"].get<std::string>();
                if (!contains(c.config.rights, ft.key)) err.push_back(where + ": unknown right '" + ft.key + "'");
            } else if (t.contains("loser_verdict")) {
                ft.kind = "loser_verdict";
                ft.flag = t["loser_verdict"].get<bool>();
            } else {
                err.push_back(where + ": a term with no known condition");
                continue;
            }
            op.terms.push_back(std::move(ft));
        }
        for (const auto& e : o.value("eff", std::vector<std::string>{})) op.eff.push_back(parseEffect(e));
        op.kills = o.value("kills", false);
        op.playerLoser = o.value("player_loser", true);
        op.personWinner = o.value("person_winner", false);
        op.deathCause = o.value("death_cause", op.deathCause);
        op.deathCauseAnimalWinner = o.value("death_cause_animal_winner", op.deathCauseAnimalWinner);
        for (const auto* cause : {&op.deathCause, &op.deathCauseAnimalWinner})
            if (op.kills && !contains(c.config.deathCauses, *cause))
                err.push_back(where + ": death cause '" + *cause + "' is not in config.death_causes");
        const Json mem = o.value("memory", Json::object()), val = o.value("valence", Json::object());
        op.memWinner = mem.value("winner", std::string());
        op.memLoser = mem.value("loser", std::string());
        op.valWinner = val.value("winner", 0.0);
        op.valLoser = val.value("loser", 0.0);
        for (const auto* m : {&op.memWinner, &op.memLoser})
            if (!m->empty() && !c.config.memoryTypes.count(*m)) err.push_back(where + ": unknown memory type '" + *m + "'");
        op.koDownHours = span2(o, "ko_down_hours");
        r.options.push_back(std::move(op));
    }
    if (r.options.empty()) err.push_back("aftermath.options: empty");
    return r;
}

RescueRules loadRescue(const Json& j, const Content& c, std::vector<std::string>& err) {
    RescueRules r;
    for (const auto& o : j.value("rescuers", Json::array())) {
        RescuerDef d;
        d.id = o.value("id", std::string());
        const std::string where = "rescue.rescuers." + d.id;
        d.loc = o.value("loc", "rescue." + d.id);
        d.who = o.value("who", std::string("none"));
        if (d.who != "kin" && d.who != "holder" && d.who != "none") err.push_back(where + ".who: kin | holder | none");
        d.wake = o.value("wake", std::string("home"));
        if (d.wake != "home" && d.wake != "nearest_home") err.push_back(where + ".wake: home | nearest_home");
        d.treatment = o.value("treatment", std::string("none"));
        const Json wt = o.value("weight", Json::object());
        d.base = wt.value("base", 0.0);
        d.perKin = wt.value("per_kin_alive", 0.0);
        d.kinMax = wt.value("kin_max", 0);
        d.skill = wt.value("skill", std::string());
        d.skillMin = wt.value("skill_min", 0.0);
        d.perHolder = wt.value("per_holder", 0.0);
        d.holderMax = wt.value("holder_max", 0);
        d.nearHomeM = wt.value("near_home_m", 0.0);
        d.nearAdd = wt.value("near_add", 0.0);
        if (d.who == "holder" && d.skill.empty()) err.push_back(where + ": who=holder needs weight.skill");
        d.skipHours = span2(o, "skip_hours");
        if (d.skipHours[1] < d.skipHours[0]) err.push_back(where + ".skip_hours: [min, max]");
        d.lossShare = o.value("loss_share", 0.0);
        r.rescuers.push_back(std::move(d));
    }
    if (r.rescuers.empty()) err.push_back("rescue.rescuers: empty");
    const Json takes = j.value("winner_takes", Json::object());
    r.takesPerson = takes.value("person", 0.0);
    r.takesAnimal = takes.value("animal", 0.0);
    r.healthFloor = j.value("health_floor", r.healthFloor);
    r.bloodFloor = j.value("blood_floor", r.bloodFloor);
    const Json mem = j.value("memory", Json::object()), val = j.value("valence", Json::object());
    r.memRescued = mem.value("rescued", std::string());
    r.memRescuer = mem.value("rescuer", std::string());
    for (const auto* m : {&r.memRescued, &r.memRescuer})
        if (!m->empty() && !c.config.memoryTypes.count(*m)) err.push_back("rescue.memory: unknown memory type '" + *m + "'");
    r.valRescued = val.value("rescued", 0.0);
    r.valRescuer = val.value("rescuer", 0.0);
    r.rescuerAffinity = j.value("rescuer_affinity", 0.0);
    r.chronicle = j.value("chronicle", r.chronicle);
    return r;
}

std::vector<double> finishScores(const World& w, const AftermathRules& r, const Character& winner, const Character& loser,
                                 const InteractionDef& d, const std::string& loserState, std::size_t witnesses) {
    std::vector<double> out;
    auto rel = winner.relations.find(loser.id);
    for (const auto& op : r.options) {
        double s = op.base;
        for (const auto& t : op.terms) {
            if (t.kind == "trait") s += winner.hasTrait(t.key) ? t.add : 0.0;
            else if (t.kind == "rel") s += (rel == winner.relations.end() ? 0.0 : rel->second.get(t.key)) * t.mul;
            else if (t.kind == "act_lethal") s += d.combat.lethal == t.flag ? t.add : 0.0;
            else if (t.kind == "loser") s += (loser.isAnimal() == (t.key == "animal")) ? t.add : 0.0;
            else if (t.kind == "winner") s += (winner.isAnimal() == (t.key == "animal")) ? t.add : 0.0;
            else if (t.kind == "winner_predator") s += isPredator(w, winner) == t.flag ? t.add : 0.0;
            else if (t.kind == "kin") s += (!winner.isAnimal() && !loser.isAnimal() && kin(winner, loser)) == t.flag ? t.add : 0.0;
            else if (t.kind == "loser_state") s += loserState == t.key ? t.add : 0.0;
            else if (t.kind == "witnesses_each") s += std::max(t.cap, t.add * static_cast<double>(witnesses));
            else if (t.kind == "right") s += winner.status.rights.count(t.key) ? t.add : 0.0;
            else if (t.kind == "loser_verdict") s += (!loser.status.verdicts.empty()) == t.flag ? t.add : 0.0;
        }
        out.push_back(s);
    }
    return out;
}

std::string finishFight(World& w, Character& winner, Character& loser, const InteractionDef& d,
                        const std::string& loserState, const std::vector<EntityId>& witnesses) {
    const CombatBridge* b = combatBridge(w.content());
    if (!b || !winner.body.alive || !loser.body.alive) return {};
    const AftermathRules& R = b->aftermath;
    if (!contains(R.loserStates, loserState)) return {};
    auto allowed = [&](const FinishOption& op) {
        if (!op.playerLoser && loser.controller.isPlayer()) return false;
        if (op.personWinner && winner.isAnimal()) return false;
        return true;
    };
    const FinishOption* pick = nullptr;
    if (winner.controller.isPlayer() && !winner.controller.combatFinish.empty())
        for (const auto& op : R.options)
            if (op.id == winner.controller.combatFinish && allowed(op)) pick = &op;
    if (!pick) {
        const std::vector<double> sc = finishScores(w, R, winner, loser, d, loserState, witnesses.size());
        Rng rng = w.rng(winner.id, "finish:" + std::to_string(loser.id));
        double best = -1e18;
        for (std::size_t i = 0; i < R.options.size(); ++i) {
            const double s = sc[i] + R.noise * (2.0 * rng.uniform() - 1.0);
            if (allowed(R.options[i]) && s > best) {
                best = s;
                pick = &R.options[i];
            }
        }
    }
    if (!pick) return {};
    const FinishOption& op = *pick;
    applyEffectList(w, winner, op.eff, nullptr, &loser, &witnesses);
    memo(w, winner, op.memWinner, loser.id, op.valWinner);
    if (op.kills) killChar(w, loser, winner.isAnimal() ? op.deathCauseAnimalWinner : op.deathCause);
    else memo(w, loser, op.memLoser, winner.id, op.valLoser);
    if (loser.body.alive && loserState == "ko" && op.koDownHours[1] > 0.0) {
        Rng rng = w.rng(loser.id, "ko_down");
        loser.body.downUntil = w.now() + static_cast<Tick>(rng.range(op.koDownHours[0], op.koDownHours[1]) * kTicksPerHour);
        loser.ai.fsm.state = "Incapacitated";
    }
    w.chronicle("combat_finish", {winner.id, loser.id}, R.chronicle,
                {{"winner", winner.id}, {"loser", loser.id}, {"act", d.id}, {"decision", op.id}, {"loser_state", loserState}});
    if (loser.body.alive && loser.controller.isPlayer() && op.id != "bind") rescuePlayer(w, loser, &winner);
    return op.id;
}

std::string rescuePlayer(World& w, Character& player, Character* winner) {
    const CombatBridge* b = combatBridge(w.content());
    if (!b || !player.body.alive) return {};
    const RescueRules& R = b->rescue;
    const combat::CombatData& cd = *b->data;
    const Tick now = w.now();
    // Who could have found the body.
    auto kinOf = [&]() {
        std::vector<Character*> out;
        for (auto& [id, c] : w.characters())
            if (id != player.id && c.body.alive && !c.isAnimal() && c.body.downUntil <= now &&
                player.status.family != kNone && c.status.family == player.status.family)
                out.push_back(&c);
        std::sort(out.begin(), out.end(), [&](const Character* a, const Character* b2) {
            const double da = dist(a->pos, player.pos), db = dist(b2->pos, player.pos);
            return da != db ? da < db : a->id < b2->id;
        });
        return out;
    };
    auto holdersOf = [&](const RescuerDef& d) {
        std::vector<Character*> out;
        for (auto& [id, c] : w.characters()) {
            if (id == player.id || !c.body.alive || c.isAnimal() || c.body.downUntil > now) continue;
            if (player.status.settlement == kNone || c.status.settlement != player.status.settlement) continue;
            auto sk = c.skills.find(d.skill);
            if (sk != c.skills.end() && sk->second >= d.skillMin) out.push_back(&c);
        }
        std::sort(out.begin(), out.end(), [&](const Character* a, const Character* b2) {
            const double sa = a->skills.at(d.skill), sb = b2->skills.at(d.skill);
            return sa != sb ? sa > sb : a->id < b2->id;
        });
        return out;
    };
    const WorldObject* home = player.home != kNone ? w.object(player.home) : nullptr;
    std::vector<double> weight;
    double total = 0.0;
    for (const auto& d : R.rescuers) {
        double v = d.base;
        if (d.perKin != 0.0) v += d.perKin * static_cast<double>(std::min<std::size_t>(kinOf().size(), static_cast<std::size_t>(std::max(0, d.kinMax))));
        if (d.perHolder != 0.0 && !d.skill.empty())
            v += d.perHolder * static_cast<double>(std::min<std::size_t>(holdersOf(d).size(), static_cast<std::size_t>(std::max(0, d.holderMax))));
        if (d.nearHomeM > 0.0 && home && dist(home->pos, player.pos) < d.nearHomeM) v += d.nearAdd;
        v = std::max(0.0, v);
        weight.push_back(v);
        total += v;
    }
    Rng rng = w.rng(player.id, "rescue");
    std::size_t pick = 0;
    if (total > 0.0) {
        double x = rng.uniform() * total;
        for (pick = 0; pick + 1 < weight.size() && x >= weight[pick]; ++pick) x -= weight[pick];
    }
    const RescuerDef& d = R.rescuers[pick];
    Character* who = nullptr;
    if (d.who == "kin") {
        auto k = kinOf();
        if (!k.empty()) who = k.front();
    } else if (d.who == "holder") {
        auto h = holdersOf(d);
        if (!h.empty()) who = h.front();
    }
    // Things: the winner takes a share, some are lost on the way.
    const double takes = winner ? (winner->isAnimal() ? R.takesAnimal : R.takesPerson) : 0.0;
    int taken = 0, lost = 0;
    for (auto it = player.inventory.begin(); it != player.inventory.end();) {
        const int t = static_cast<int>(std::floor(it->second * takes));
        const int l = static_cast<int>(std::floor((it->second - t) * d.lossShare));
        if (t > 0 && winner && winner->body.alive) winner->inventory[it->first] += t;
        taken += t;
        lost += l;
        it->second -= t + l;
        it = it->second <= 0 ? player.inventory.erase(it) : std::next(it);
    }
    // Wakes up in a safe place, the wounds tended by the rescuer.
    Vec2 wake = player.pos;
    bool placed = false;
    if (d.wake == "home" && home) {
        wake = home->pos;
        placed = true;
    }
    if (!placed) {
        double best = 1e18;
        for (const auto& [fid, f] : w.families()) {
            const WorldObject* s = f.storage != kNone ? w.object(f.storage) : nullptr;
            if (!s) continue;
            const double dd = dist(s->pos, player.pos);
            if (dd < best) {
                best = dd;
                wake = s->pos;
                placed = true;
            }
        }
        if (!placed && home) wake = home->pos;
    }
    player.pos = wake;
    const double hours = rng.range(d.skipHours[0], d.skipHours[1]);
    player.body.downUntil = now + static_cast<Tick>(hours * kTicksPerHour);
    player.ai.fsm.state = "Incapacitated";
    player.body.health = std::max(player.body.health, R.healthFloor);
    player.body.blood = std::max(player.body.blood, std::min(R.bloodFloor, cd.bloodMax));
    const RecoveryRules& rec = b->recovery;
    for (Wound& wd : player.wounds) {
        if (rec.rankOf(d.treatment) > rec.rankOf(wd.treatment)) wd.treatment = d.treatment;
        wd.aided = true;
        wd.bleeding = false;
    }
    memo(w, player, R.memRescued, who ? who->id : kNone, R.valRescued);
    if (who) {
        memo(w, *who, R.memRescuer, player.id, R.valRescuer);
        player.relations[who->id].affinity = std::clamp(player.relations[who->id].affinity + R.rescuerAffinity, -1.0, 1.0);
    }
    w.chronicle("rescue", {player.id}, R.chronicle,
                {{"who", player.id}, {"rescuer", d.id}, {"loc", d.loc}, {"by", who ? who->id : kNone},
                 {"hours", static_cast<int>(hours)}, {"taken", taken}, {"lost", lost}});
    return d.id;
}

} // namespace altera::sim
