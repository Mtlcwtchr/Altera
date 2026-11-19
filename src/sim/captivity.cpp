#include "sim/captivity.hpp"

#include <algorithm>
#include <cmath>

#include "sim/ai.hpp"
#include "sim/character.hpp"
#include "sim/combat_bridge.hpp"
#include "sim/world.hpp"

namespace altera::sim {

namespace {

constexpr Tick kTicksPerHour = kTicksPerDay / 24;

std::array<double, 2> span2(const Json& j, const char* key, std::array<double, 2> d) {
    if (j.contains(key) && j[key].is_array() && j[key].size() == 2) return {j[key][0].get<double>(), j[key][1].get<double>()};
    return d;
}

std::vector<FateOption> loadOptions(const Json& arr, const Content& c, const std::string& where, std::vector<std::string>& err) {
    std::vector<FateOption> out;
    for (const auto& o : arr) {
        FateOption op;
        op.id = o.value("id", std::string());
        op.base = o.value("base", 0.0);
        const std::string at = where + "." + op.id;
        for (const auto& t : o.value("terms", Json::array())) {
            FateTerm ft;
            static const char* kinds[] = {"trait", "rel", "kin", "right", "crimes", "offended", "can_pay", "captive_player",
                                          "captor_player"};
            for (const char* k : kinds)
                if (t.contains(k)) {
                    ft.kind = k;
                    const Json& v = t[k];
                    if (v.is_string()) ft.key = v.get<std::string>();
                    else if (v.is_boolean()) ft.flag = v.get<bool>();
                }
            if (ft.kind.empty()) err.push_back(at + ": a term without a known kind");
            ft.add = t.value("add", 0.0);
            ft.mul = t.value("mul", 0.0);
            if (t.contains("cap")) {
                ft.cap = t["cap"].get<double>();
                ft.capped = true;
            }
            if (ft.kind == "trait" && !c.traits.count(ft.key)) err.push_back(at + ": unknown trait '" + ft.key + "'");
            if (ft.kind == "right" && std::find(c.config.rights.begin(), c.config.rights.end(), ft.key) == c.config.rights.end())
                err.push_back(at + ": unknown right '" + ft.key + "'");
            op.terms.push_back(std::move(ft));
        }
        for (const auto& e : o.value("eff", std::vector<std::string>{})) op.eff.push_back(parseEffect(e));
        const Json mem = o.value("memory", Json::object()), val = o.value("valence", Json::object());
        op.memCaptor = mem.value("captor", std::string());
        op.memCaptive = mem.value("captive", std::string());
        op.memPayer = mem.value("payer", std::string());
        op.valCaptor = val.value("captor", 0.0);
        op.valCaptive = val.value("captive", 0.0);
        op.valPayer = val.value("payer", 0.0);
        for (const auto* m : {&op.memCaptor, &op.memCaptive, &op.memPayer})
            if (!m->empty() && !c.config.memoryTypes.count(*m)) err.push_back(at + ": unknown memory type '" + *m + "'");
        op.exileM = o.value("exile_m", 0.0);
        out.push_back(std::move(op));
    }
    return out;
}

const CaptivityRules* rules(const World& w) {
    const CombatBridge* b = combatBridge(w.content());
    return b && b->captivity.loaded ? &b->captivity : nullptr;
}

void memo(World& w, Character& who, const std::string& type, EntityId other, double valence) {
    if (type.empty() || !who.body.alive) return;
    MemoryEntry m;
    m.type = type;
    if (other != kNone) m.who = {other};
    m.valence = valence;
    w.remember(who, std::move(m));
}

bool kinOf(const Character& a, const Character& b) {
    return (a.status.family != kNone && a.status.family == b.status.family) ||
           (a.status.dynasty != kNone && a.status.dynasty == b.status.dynasty);
}

// saw_crime memories naming the captive among the people of the decider's settlement.
int crimesOf(const World& w, const Character& decider, const Character& captive) {
    int n = 0;
    for (const auto& [id, c] : w.characters()) {
        if (c.isAnimal() || !c.body.alive) continue;
        if (&c != &decider && (decider.status.settlement == kNone || c.status.settlement != decider.status.settlement)) continue;
        for (const auto& m : c.memory)
            if (m.type == "saw_crime" && !m.who.empty() && m.who.front() == captive.id) ++n;
    }
    return n;
}

bool offendedBy(const Character& decider, const Character& captive) {
    for (const auto& m : decider.memory)
        if (m.type == "was_insulted" && std::find(m.who.begin(), m.who.end(), captive.id) != m.who.end()) return true;
    return false;
}

// Who of the captive's family pays, and with what (item -> count), if they can.
struct Ransom {
    bool ok = false;
    std::vector<std::pair<Character*, std::map<std::string, int>>> fromPeople;
    std::map<std::string, int> fromStore;
    WorldObject* store = nullptr;
    Character* payer = nullptr;
};

Ransom ransomOf(World& w, const CaptivityRules& R, const Character& captive) {
    Ransom r;
    if (captive.status.family == kNone) return r;
    int value = 0;
    auto take = [&](std::map<std::string, int>& from, std::map<std::string, int>& into) {
        for (const auto& [item, v] : R.ransomGoods) {
            if (value >= R.ransomValue || v <= 0) break;
            auto it = from.find(item);
            if (it == from.end() || it->second <= 0) continue;
            const int need = (R.ransomValue - value + v - 1) / v;
            const int n = std::min(need, it->second);
            into[item] += n;
            value += n * v;
        }
    };
    auto fam = w.families().find(captive.status.family);
    if (fam == w.families().end()) return r;
    for (EntityId id : fam->second.members) {
        Character* m = w.character(id);
        if (!m || m->id == captive.id || !m->body.alive || m->captivity.held()) continue;
        std::map<std::string, int> part;
        take(m->inventory, part);
        if (!part.empty()) {
            r.fromPeople.push_back({m, part});
            if (!r.payer) r.payer = m;
        }
        if (value >= R.ransomValue) break;
    }
    if (value < R.ransomValue && fam->second.storage != kNone) {
        r.store = w.object(fam->second.storage);
        if (r.store) take(r.store->stock, r.fromStore);
    }
    if (!r.payer)
        for (EntityId id : fam->second.members)
            if (Character* m = w.character(id); m && m->id != captive.id && m->body.alive && !m->isAnimal()) {
                r.payer = m;
                break;
            }
    r.ok = value >= R.ransomValue;
    return r;
}

double score(const World& w, const FateOption& op, const Character& decider, const Character& captive, int crimes,
             bool canPay) {
    double s = op.base;
    auto rel = decider.relations.find(captive.id);
    for (const auto& t : op.terms) {
        double v = 0.0;
        if (t.kind == "trait") v = decider.hasTrait(t.key) ? t.add : 0.0;
        else if (t.kind == "rel") v = (rel == decider.relations.end() ? 0.0 : rel->second.get(t.key)) * t.mul;
        else if (t.kind == "kin") v = kinOf(decider, captive) == t.flag ? t.add : 0.0;
        else if (t.kind == "right") v = decider.status.rights.count(t.key) ? t.add : 0.0;
        else if (t.kind == "crimes") v = t.add * static_cast<double>(crimes);
        else if (t.kind == "offended") v = offendedBy(decider, captive) == t.flag ? t.add : 0.0;
        else if (t.kind == "can_pay") v = canPay == t.flag ? t.add : 0.0;
        else if (t.kind == "captive_player") v = captive.controller.isPlayer() == t.flag ? t.add : 0.0;
        else if (t.kind == "captor_player") v = decider.controller.isPlayer() == t.flag ? t.add : 0.0;
        if (t.capped) v = t.add >= 0.0 ? std::min(v, t.cap) : std::max(v, t.cap);
        s += v;
    }
    (void)w;
    return s;
}

const FateOption* pick(World& w, const CaptivityRules& R, const std::vector<FateOption>& opts, const Character& decider,
                       const Character& captive, const std::string& salt, bool canPay, bool allowKeep) {
    const int crimes = crimesOf(w, decider, captive);
    Rng rng = w.rng(decider.id, salt + std::to_string(captive.id));
    const FateOption* best = nullptr;
    double bs = -1e18;
    for (const auto& op : opts) {
        if (op.id == "keep" && !allowKeep) continue;
        if (op.id == "ransom" && !canPay) continue;
        const double s = score(w, op, decider, captive, crimes, canPay) + R.noise * (2.0 * rng.uniform() - 1.0);
        if (s > bs) {
            bs = s;
            best = &op;
        }
    }
    return best;
}

const FateOption* optionById(const std::vector<FateOption>& opts, const std::string& id) {
    for (const auto& o : opts)
        if (o.id == id) return &o;
    return nullptr;
}

void setPair(Character& a, Character& b, bool on) {
    auto one = [&](Character& x, Character& y) {
        if (on) x.status.pairs[y.id].insert("captor_captive");
        else {
            auto it = x.status.pairs.find(y.id);
            if (it != x.status.pairs.end()) {
                it->second.erase("captor_captive");
                if (it->second.empty()) x.status.pairs.erase(it);
            }
        }
    };
    one(a, b);
    one(b, a);
}

void die(World& w, Character& who, const std::string& cause) {
    if (!who.body.alive) return;
    who.body.alive = false;
    who.body.health = 0.0;
    who.body.causeOfDeath = cause;
    w.chronicle("death", {who.id}, "chronicle.death", {{"who", who.id}, {"cause", cause}});
}

// The judge of a trial: someone of the captor's settlement with the right to
// judge, else the eldest grown person there (custom), else the captor.
Character& judgeFor(World& w, Character& captor, const Character& captive) {
    Character* best = nullptr;
    Character* eldest = nullptr;
    const EntityId st = captor.status.settlement;
    for (auto& [id, c] : w.characters()) {
        if (c.isAnimal() || !c.body.alive || c.id == captive.id || c.captivity.held()) continue;
        if (st == kNone || c.status.settlement != st) continue;
        if (c.status.rights.count("judge") && (!best || c.body.ageDays > best->body.ageDays)) best = &c;
        if (c.body.stage() != "child" && (!eldest || c.body.ageDays > eldest->body.ageDays)) eldest = &c;
    }
    if (captor.status.rights.count("judge")) return captor;
    if (best) return *best;
    if (eldest) return *eldest;
    return captor;
}

void passControl(World& w, Character& dead) {
    if (!dead.controller.isPlayer()) return;
    const int player = dead.controller.playerId;
    const EntityId heir = heirOf(w, dead);
    const CaptivityRules* R = rules(w);
    dead.controller.kind = Controller::Kind::Ai;
    if (heir == kNone) {
        w.chronicle("succession", {dead.id}, (R ? R->succession : std::string("chronicle.succession")) + "_none",
                    {{"who", dead.id}, {"heir", kNone}});
        return;
    }
    std::string why;
    if (switchControl(w, player, heir, why))
        w.chronicle("succession", {dead.id, heir}, R ? R->succession : "chronicle.succession",
                    {{"who", dead.id}, {"heir", heir}});
}

} // namespace

CaptivityRules loadCaptivity(const Json& j, const Content& c, std::vector<std::string>& err) {
    CaptivityRules r;
    r.loaded = true;
    r.checkHours = j.value("check_hours", r.checkHours);
    r.decideHours = span2(j, "decide_hours", r.decideHours);
    if (r.decideHours[1] < r.decideHours[0]) err.push_back("captivity.decide_hours: [min, max]");
    r.followM = j.value("follow_m", r.followM);
    r.noise = j.value("noise", r.noise);
    r.maxKeeps = j.value("max_keeps", r.maxKeeps);
    const Json e = j.value("escape", Json::object());
    r.escBase = e.value("base", r.escBase);
    r.escSkill = e.value("skill", r.escSkill);
    r.escPerSkill = e.value("per_skill", r.escPerSkill);
    r.escAwayM = e.value("captor_away_m", r.escAwayM);
    r.escAwayMult = e.value("away_mult", r.escAwayMult);
    r.escNightMult = e.value("night_mult", r.escNightMult);
    r.escWoundedBelow = e.value("wounded_below", r.escWoundedBelow);
    r.escWoundedMult = e.value("wounded_mult", r.escWoundedMult);
    r.escMax = e.value("max", r.escMax);
    r.escRunM = e.value("run_m", r.escRunM);
    const Json rs = j.value("ransom", Json::object());
    r.ransomValue = rs.value("value", r.ransomValue);
    r.ransomGoods = rs.value("goods", std::map<std::string, int>{});
    for (const auto& [item, v] : r.ransomGoods)
        if (!c.items.count(item)) err.push_back("captivity.ransom.goods: unknown item '" + item + "'");
    r.fates = loadOptions(j.value("fates", Json::array()), c, "captivity.fates", err);
    r.verdicts = loadOptions(j.value("verdicts", Json::array()), c, "captivity.verdicts", err);
    for (const char* f : {"release", "ransom", "trial", "keep"})
        if (!optionById(r.fates, f)) err.push_back(std::string("captivity.fates: no '") + f + "'");
    for (const auto& v : r.verdicts)
        if (std::find(c.config.verdicts.begin(), c.config.verdicts.end(), v.id) == c.config.verdicts.end() && v.id != "acquit")
            err.push_back("captivity.verdicts: unknown verdict '" + v.id + "'");
    const Json ex = j.value("execution", Json::object());
    r.execCause = ex.value("cause", r.execCause);
    r.execWitnessM = ex.value("witness_radius_m", r.execWitnessM);
    r.execWitnessMem = ex.value("witness_memory", r.execWitnessMem);
    r.execExecutorMem = ex.value("executor_memory", r.execExecutorMem);
    if (std::find(c.config.deathCauses.begin(), c.config.deathCauses.end(), r.execCause) == c.config.deathCauses.end())
        err.push_back("captivity.execution.cause: unknown death cause '" + r.execCause + "'");
    const Json mem = j.value("memory", Json::object());
    r.memEscaped = mem.value("escaped", std::string());
    r.memCaptorEscaped = mem.value("captor_escaped", std::string());
    r.memFreed = mem.value("freed", std::string());
    r.memExiled = mem.value("exiled", std::string());
    r.memPunished = mem.value("punished", std::string());
    for (const auto* m : {&r.memEscaped, &r.memCaptorEscaped, &r.memFreed, &r.memExiled, &r.memPunished, &r.execWitnessMem,
                          &r.execExecutorMem})
        if (!m->empty() && !c.config.memoryTypes.count(*m)) err.push_back("captivity.memory: unknown memory type '" + *m + "'");
    r.chronicle = j.value("chronicle", r.chronicle);
    r.succession = j.value("succession", Json::object()).value("chronicle", r.succession);
    return r;
}

void takeCaptive(World& w, Character& captor, Character& captive) {
    if (!captive.body.alive || captive.isAnimal() || captor.isAnimal() || &captor == &captive) return;
    const CaptivityRules* R = rules(w);
    if (captive.captivity.held()) freeCaptive(w, captive, "taken_over");
    Captivity& k = captive.captivity;
    k.captor = captor.id;
    k.since = w.now();
    k.stage = "held";
    k.tries = 0;
    k.verdict.clear();
    const Tick hour = kTicksPerHour;
    Rng rng = w.rng(captive.id, "captive_hold");
    const double h = R ? rng.range(R->decideHours[0], R->decideHours[1]) : 12.0;
    k.decideAt = w.now() + static_cast<Tick>(h * static_cast<double>(hour));
    k.nextTry = w.now() + static_cast<Tick>((R ? R->checkHours : 2.0) * static_cast<double>(hour));
    setPair(captor, captive, true);
    captive.ai.fsm = FsmState{};
    captive.ai.fsm.state = "Incapacitated";
    captive.body.downUntil = std::max(captive.body.downUntil, w.now() + 1);
    w.chronicle("captivity", {captor.id, captive.id}, (R ? R->chronicle : std::string("chronicle.captivity")) + ".taken",
                {{"captor", captor.id}, {"captive", captive.id}});
}

void freeCaptive(World& w, Character& captive, const std::string& why) {
    if (!captive.captivity.held()) return;
    const CaptivityRules* R = rules(w);
    Character* captor = w.character(captive.captivity.captor);
    if (captor) setPair(*captor, captive, false);
    w.chronicle("captivity", {captive.id}, (R ? R->chronicle : std::string("chronicle.captivity")) + "." + why,
                {{"captor", captive.captivity.captor}, {"captive", captive.id}});
    captive.captivity = Captivity{};
    if (captive.ai.fsm.state == "Incapacitated" && captive.body.downUntil <= w.now() + 1) {
        captive.body.downUntil = 0;
        captive.ai.fsm.state = "Idle";
    }
}

std::vector<double> fateScores(const World& w, const std::vector<FateOption>& opts, const Character& decider,
                               const Character& captive) {
    std::vector<double> out;
    const int crimes = crimesOf(w, decider, captive);
    for (const auto& op : opts) out.push_back(score(w, op, decider, captive, crimes, true));
    return out;
}

void carryOutVerdict(World& w, Character& judge, Character& captive, const std::string& verdict) {
    const CaptivityRules* R = rules(w);
    if (!R || !captive.body.alive) return;
    const FateOption* v = optionById(R->verdicts, verdict);
    Character* captor = w.character(captive.captivity.captor);
    Character& executor = captor && captor->body.alive ? *captor : judge;
    if (verdict != "acquit") captive.status.verdicts.insert(verdict);
    captive.captivity.verdict = verdict;
    w.chronicle("verdict", {judge.id, captive.id}, R->chronicle + ".verdict_" + verdict,
                {{"judge", judge.id}, {"captive", captive.id}});
    if (v && !v->eff.empty()) applyEffectList(w, judge, v->eff, nullptr, &captive, nullptr);
    if (verdict == "execute") {
        // The real death (also of a player character: the heir takes over).
        memo(w, executor, R->execExecutorMem, captive.id, 0.0);
        for (auto& [id, c] : w.characters())
            if (!c.isAnimal() && c.body.alive && c.id != captive.id && c.id != executor.id &&
                dist(c.pos, captive.pos) <= R->execWitnessM)
                memo(w, c, R->execWitnessMem, captive.id, -0.3);
        if (captor) setPair(*captor, captive, false);
        captive.captivity = Captivity{};
        die(w, captive, R->execCause);
        passControl(w, captive);
        return;
    }
    if (verdict == "punish") memo(w, captive, R->memPunished, judge.id, -0.5);
    if (verdict == "exile") {
        memo(w, captive, R->memExiled, judge.id, -0.6);
        const EntityId st = captive.status.settlement;
        Vec2 centre = captive.pos;
        auto s = w.settlements().find(st);
        if (s != w.settlements().end()) centre = s->second.center;
        captive.status.settlement = kNone;
        Vec2 away{captive.pos.x - centre.x, captive.pos.y - centre.y};
        const double len = std::hypot(away.x, away.y);
        if (len < 1e-6) away = {1.0, 0.0};
        else away = {away.x / len, away.y / len};
        const double m = v ? v->exileM : 500.0;
        captive.pos = {centre.x + away.x * m, centre.y + away.y * m};
        captive.pos.x = std::clamp(captive.pos.x, 0.0, w.map().sizeM);
        captive.pos.y = std::clamp(captive.pos.y, 0.0, w.map().sizeM);
    }
    freeCaptive(w, captive, "sentence_" + verdict);
}

bool decideFate(World& w, Character& captor, Character& captive, const std::string& fate) {
    const CaptivityRules* R = rules(w);
    if (!R || !captive.captivity.held()) return false;
    const FateOption* op = optionById(R->fates, fate);
    if (!op) return false;
    if (fate == "keep") {
        Rng rng = w.rng(captive.id, "captive_keep");
        captive.captivity.decideAt =
            w.now() + static_cast<Tick>(rng.range(R->decideHours[0], R->decideHours[1]) * static_cast<double>(kTicksPerHour));
        ++captive.captivity.tries;
        return true;
    }
    if (fate == "ransom") {
        Ransom rs = ransomOf(w, *R, captive);
        if (!rs.ok) return false;
        for (auto& [who, items] : rs.fromPeople)
            for (const auto& [item, n] : items) {
                who->inventory[item] -= n;
                if (who->inventory[item] <= 0) who->inventory.erase(item);
                captor.inventory[item] += n;
            }
        if (rs.store)
            for (const auto& [item, n] : rs.fromStore) {
                rs.store->stock[item] = std::max(0, rs.store->stock[item] - n);
                captor.inventory[item] += n;
            }
        if (rs.payer) memo(w, *rs.payer, op->memPayer, captor.id, op->valPayer);
    }
    if (!op->eff.empty()) applyEffectList(w, captor, op->eff, nullptr, &captive, nullptr);
    memo(w, captor, op->memCaptor, captive.id, op->valCaptor);
    memo(w, captive, op->memCaptive, captor.id, op->valCaptive);
    if (fate == "trial") {
        captive.captivity.stage = "trial";
        w.chronicle("captivity", {captor.id, captive.id}, R->chronicle + ".trial", {{"captor", captor.id}, {"captive", captive.id}});
        Character& judge = judgeFor(w, captor, captive);
        const FateOption* v = pick(w, *R, R->verdicts, judge, captive, "verdict:", true, true);
        carryOutVerdict(w, judge, captive, v ? v->id : "acquit");
        return true;
    }
    freeCaptive(w, captive, fate);
    return true;
}

EntityId heirOf(const World& w, const Character& dead) {
    auto ok = [&](const Character* c) {
        return c && c->id != dead.id && c->body.alive && !c->isAnimal() && !c->controller.isPlayer();
    };
    // The heir named by the dead one (name_heir leaves memory heir_named with the heir).
    for (auto it = dead.memory.rbegin(); it != dead.memory.rend(); ++it)
        if (it->type == "heir_named" && !it->who.empty()) {
            const Character* h = w.character(it->who.front());
            if (ok(h)) return h->id;
        }
    auto eldest = [&](auto pred) -> EntityId {
        const Character* best = nullptr;
        for (const auto& [id, c] : w.characters())
            if (ok(&c) && pred(c) && (!best || c.body.ageDays > best->body.ageDays)) best = &c;
        return best ? best->id : kNone;
    };
    const auto grown = [](const Character& c) { return c.body.stage() != "child"; };
    EntityId h = eldest([&](const Character& c) {
        return grown(c) && (c.status.mother == dead.id || c.status.father == dead.id);
    });
    if (h != kNone) return h;
    if (const Character* sp = w.character(dead.status.spouse); ok(sp)) return sp->id;
    h = eldest([&](const Character& c) { return grown(c) && kinOf(c, dead); });
    if (h != kNone) return h;
    return eldest([&](const Character& c) { return kinOf(c, dead); });
}

void captivityTick(World& w) {
    const CaptivityRules* R = rules(w);
    // A dead player character (wounds, old age, execution) hands the player on.
    for (auto& [id, c] : w.characters())
        if (!c.body.alive && c.controller.isPlayer()) passControl(w, c);
    if (!R) return;
    const Tick now = w.now();
    for (auto& [id, c] : w.characters()) {
        if (!c.captivity.held() || !c.body.alive) continue;
        Character* captor = w.character(c.captivity.captor);
        // The captor is gone, dead or down, or let go of the pair (release_captive): free.
        if (!captor || !captor->body.alive || !c.status.hasPair(captor->id, "captor_captive")) {
            freeCaptive(w, c, "captor_gone");
            memo(w, c, R->memFreed, kNone, 0.3);
            continue;
        }
        // Led along: the mind stays off, the captive keeps a step behind.
        c.body.downUntil = std::max(c.body.downUntil, now + 2);
        c.ai.fsm.state = "Incapacitated";
        const double dd = dist(c.pos, captor->pos);
        // Off guard: the captor is down, asleep or far (left the captive behind).
        const bool captorAway = captor->body.downUntil > now || captor->ai.fsm.state == "Sleep" || dd > R->escAwayM;
        if (captor->body.downUntil <= now && dd > R->followM) {
            const double k = (dd - R->followM) / dd;
            c.pos = {c.pos.x + (captor->pos.x - c.pos.x) * k, c.pos.y + (captor->pos.y - c.pos.y) * k};
        }
        // Escape tries.
        if (now >= c.captivity.nextTry) {
            c.captivity.nextTry = now + static_cast<Tick>(R->checkHours * static_cast<double>(kTicksPerHour));
            auto sk = c.skills.find(R->escSkill);
            double p = R->escBase + R->escPerSkill * (sk == c.skills.end() ? 0.0 : sk->second);
            if (captorAway) p *= R->escAwayMult;
            if (w.isNight()) p *= R->escNightMult;
            if (c.body.health < R->escWoundedBelow) p *= R->escWoundedMult;
            p = std::min(p, R->escMax);
            Rng rng = w.rng(c.id, "escape");
            if (rng.uniform() < p) {
                Vec2 away{c.pos.x - captor->pos.x, c.pos.y - captor->pos.y};
                const double len = std::hypot(away.x, away.y);
                const double ang = rng.uniform() * 6.283185307;
                away = len > 1e-6 ? Vec2{away.x / len, away.y / len} : Vec2{std::cos(ang), std::sin(ang)};
                memo(w, c, R->memEscaped, captor->id, 0.4);
                memo(w, *captor, R->memCaptorEscaped, c.id, -0.3);
                captor->relations[c.id].enmity = std::clamp(captor->relations[c.id].enmity + 0.1, 0.0, 1.0);
                freeCaptive(w, c, "escaped");
                c.body.downUntil = 0;
                c.ai.fsm.state = "Idle";
                c.pos = {std::clamp(c.pos.x + away.x * R->escRunM, 0.0, w.map().sizeM),
                         std::clamp(c.pos.y + away.y * R->escRunM, 0.0, w.map().sizeM)};
                continue;
            }
        }
        // The captor's decision (a player captor decides with the wheel's acts).
        if (now >= c.captivity.decideAt && !captor->controller.isPlayer() && captor->body.downUntil <= now) {
            const Ransom rs = ransomOf(w, *R, c);
            const FateOption* f = pick(w, *R, R->fates, *captor, c, "fate:", rs.ok, c.captivity.tries < R->maxKeeps);
            if (!f || !decideFate(w, *captor, c, f->id)) decideFate(w, *captor, c, "release");
        }
    }
}

} // namespace altera::sim
