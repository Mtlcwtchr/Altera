// After an accusation: answer, pursuit, court, sentence (justice.hpp, AL-37).

#include "sim/justice.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

#include "sim/ai.hpp"
#include "sim/captivity.hpp"
#include "sim/consequences.hpp"
#include "sim/facts.hpp"
#include "sim/world.hpp"

namespace fs = std::filesystem;

namespace lr::sim {

namespace {

constexpr const char* kPending = "case_pending"; // on the accused: who = {accuser, judge}, source = stage
constexpr const char* kClosed = "case_closed";   // on both: who = {the other}, source = outcome
constexpr double kSalience = 0.8;

double clampRel(double v) { return std::clamp(v, -1.0, 1.0); }

void relShift(Character& from, EntityId to, const CaseRules::RelShift& s) {
    if (from.id == to) return;
    Relation& r = from.relations[to];
    r.trust = clampRel(r.trust + s.trust);
    r.enmity = clampRel(r.enmity + s.enmity);
}

void relOutcome(Character& accuser, Character& accused, const CaseRules& R, const std::string& outcome) {
    if (auto it = R.accuserToAccused.find(outcome); it != R.accuserToAccused.end()) relShift(accuser, accused.id, it->second);
    if (auto it = R.accusedToAccuser.find(outcome); it != R.accusedToAccuser.end()) relShift(accused, accuser.id, it->second);
}

void repute(World& w, Character& c, const CaseRules& R, const std::string& outcome) {
    auto it = R.reputation.find(outcome);
    if (it == R.reputation.end()) return;
    const auto& axes = w.content().config.reputationAxes;
    for (const auto& [axis, dv] : it->second) {
        if (std::find(axes.begin(), axes.end(), axis) == axes.end()) continue;
        double& v = c.reputation["reputation:" + axis];
        v = std::clamp(v + dv, -1.0, 1.0);
    }
}

void note(World& w, Character& c, const std::string& type, std::vector<EntityId> who, EntityId object,
          const std::string& source, double confidence, double valence) {
    MemoryEntry m;
    m.type = type;
    m.who = std::move(who);
    m.object = object;
    m.source = source;
    m.confidence = confidence;
    m.valence = valence;
    m.salience = kSalience;
    w.remember(c, std::move(m));
}

MemoryEntry* pendingOf(Character& accused, EntityId accuser, EntityId object) {
    for (auto& m : accused.memory)
        if (m.type == kPending && !m.who.empty() && m.who[0] == accuser && m.object == object) return &m;
    return nullptr;
}

// Earlier deeds of the accused known to `judge` (or settled cases), apart from this one.
int crimesOf(const Character& judge, const Character& accused, EntityId object) {
    std::set<EntityId> objects;
    int settled = 0;
    for (const auto& m : judge.memory)
        if (isDeedMemory(m.type) && !m.who.empty() && m.who[0] == accused.id && m.object != object) objects.insert(m.object);
    for (const auto& m : accused.memory)
        if (m.type == kClosed && m.object != object &&
            (m.source == "returned" || m.source == "compensated" || m.source == "fled" || m.source == "convicted"))
            ++settled;
    return static_cast<int>(objects.size()) + settled;
}

bool kinOf(const Character& a, const Character& b) {
    return a.status.family != kNone && a.status.family == b.status.family;
}

double badName(const Character& c) {
    auto it = c.reputation.find("reputation:honesty");
    return it == c.reputation.end() ? 0.0 : std::max(0.0, -it->second);
}

WorldObject* storeOf(World& w, const Character& c) {
    auto f = w.families().find(c.status.family);
    if (f == w.families().end()) return nullptr;
    return w.object(f->second.storage);
}

// What `c` can reach of an item: on him and in his household's store.
int reach(World& w, const Character& c, const std::string& item) {
    int n = c.has(item);
    if (const WorldObject* s = storeOf(w, c)) {
        auto it = s->stock.find(item);
        if (it != s->stock.end()) n += std::max(0, it->second);
    }
    return n;
}

// Takes n of an item from `c` (inventory first, then the store); returns how many.
int takeFrom(World& w, Character& c, const std::string& item, int n) {
    int got = 0;
    auto inv = c.inventory.find(item);
    if (inv != c.inventory.end()) {
        const int k = std::min(n, inv->second);
        inv->second -= k;
        if (inv->second <= 0) c.inventory.erase(inv);
        got += k;
    }
    if (got < n)
        if (WorldObject* s = storeOf(w, c)) {
            auto it = s->stock.find(item);
            if (it != s->stock.end()) {
                const int k = std::min(n - got, std::max(0, it->second));
                it->second -= k;
                got += k;
            }
        }
    return got;
}

void giveTo(World& w, Character& c, const std::map<std::string, int>& goods) {
    WorldObject* s = storeOf(w, c);
    for (const auto& [item, n] : goods) {
        if (n <= 0) continue;
        if (s) s->stock[item] += n;
        else c.inventory[item] += n;
    }
}

bool holds(World& w, const Character& c, const std::map<std::string, int>& goods) {
    if (goods.empty()) return false;
    for (const auto& [item, n] : goods)
        if (reach(w, c, item) < n) return false;
    return true;
}

// Goods worth `value` that `c` could pay (by the worth table, in its order); empty when short.
std::map<std::string, int> payment(World& w, const Character& c, const CaseRules& R, int value) {
    std::map<std::string, int> out;
    if (value <= 0) return out;
    int got = 0;
    // the cheapest goods first (food before tools), then by name
    std::vector<std::pair<std::string, int>> order(R.worth.begin(), R.worth.end());
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    for (const auto& [item, v] : order) {
        if (got >= value) break;
        if (v <= 0) continue;
        const int have = reach(w, c, item);
        if (have <= 0) continue;
        const int need = (value - got + v - 1) / v;
        const int n = std::min(need, have);
        out[item] = n;
        got += n * v;
    }
    if (got < value) out.clear();
    return out;
}

// Moves goods from one person (and household) to another's; returns what moved.
std::map<std::string, int> hand(World& w, Character& from, Character& to, const std::map<std::string, int>& goods) {
    std::map<std::string, int> moved;
    for (const auto& [item, n] : goods)
        if (const int k = takeFrom(w, from, item, n); k > 0) moved[item] = k;
    giveTo(w, to, moved);
    return moved;
}

Json goodsJson(const std::map<std::string, int>& g) {
    Json j = Json::object();
    for (const auto& [k, v] : g) j[k] = v;
    return j;
}

int lossWorth(const CaseRules& R, const std::map<std::string, int>& goods, const std::string& tag) {
    const int v = goodsWorth(R, goods);
    if (v > 0) return v;
    auto it = R.defaultWorth.find(tag);
    return it == R.defaultWorth.end() ? 2 : it->second;
}

// A person sent (or going) away from his settlement.
void leave(World& w, Character& c, double m) {
    Vec2 centre = c.pos;
    auto s = w.settlements().find(c.status.settlement);
    if (s != w.settlements().end()) centre = s->second.center;
    c.status.settlement = kNone;
    Vec2 away{c.pos.x - centre.x, c.pos.y - centre.y};
    const double len = std::hypot(away.x, away.y);
    away = len < 1e-6 ? Vec2{1.0, 0.0} : Vec2{away.x / len, away.y / len};
    c.pos = {std::clamp(centre.x + away.x * m, 0.0, w.map().sizeM), std::clamp(centre.y + away.y * m, 0.0, w.map().sizeM)};
    c.ai.queue.clear();
}

// Who judges a case: the chief, else one with the right to judge, else the
// eldest grown person of the settlement; never a party but the chief himself.
Character* judgeOf(World& w, const Character& accuser, const Character& accused) {
    const EntityId st = accuser.status.settlement;
    if (st == kNone) return nullptr;
    if (auto s = w.settlements().find(st); s != w.settlements().end() && s->second.chief != kNone &&
                                           s->second.chief != accused.id)
        if (Character* ch = w.character(s->second.chief); ch && ch->body.alive && !ch->captivity.held()) return ch;
    Character* right = nullptr;
    Character* eldest = nullptr;
    for (auto& [id, c] : w.characters()) {
        if (c.isAnimal() || !c.body.alive || c.captivity.held() || c.status.settlement != st) continue;
        if (c.id == accused.id || c.id == accuser.id || c.body.stage() == "child") continue;
        if (c.status.rights.count("judge") && (!right || c.body.ageDays > right->body.ageDays)) right = &c;
        if (!eldest || c.body.ageDays > eldest->body.ageDays) eldest = &c;
    }
    return right ? right : eldest;
}

const CaseOption* pick(World& w, const CaseRules& R, const std::vector<CaseOption>& opts, const Character& chooser,
                       const Character& other, const CaseCtx& x, const std::string& salt) {
    const CaseOption* best = nullptr;
    double top = -1e18;
    for (const auto& o : opts) {
        Rng r = w.rng(chooser.id, salt + o.id);
        const double s = caseScore(o, chooser, other, x) + R.noise * r.uniform();
        if (s > top) {
            top = s;
            best = &o;
        }
    }
    return best;
}

void queueTask(World& w, Character& who, const std::string& task, EntityId target, const CaseRules& R,
               const std::string& why) {
    if (!w.content().interaction(task)) return;
    for (const auto& q : who.ai.queue)
        if (q.task == task && q.target == target) return;
    who.ai.queue.push_back(QueuedTask{task, target, R.taskPriority, w.now(), R.taskTtl, why, "consequence"});
}

// The case ends: the pending mark goes, both remember how it ended, the chronicle says it.
void close(World& w, Character& accuser, Character& accused, EntityId object, const std::string& outcome,
           Json params = Json::object()) {
    accused.memory.erase(std::remove_if(accused.memory.begin(), accused.memory.end(),
                                        [&](const MemoryEntry& m) {
                                            return m.type == kPending && !m.who.empty() && m.who[0] == accuser.id &&
                                                   m.object == object;
                                        }),
                         accused.memory.end());
    const bool good = outcome == "returned" || outcome == "compensated";
    note(w, accused, kClosed, {accuser.id}, object, outcome, 1.0, good ? -0.1 : -0.4);
    note(w, accuser, kClosed, {accused.id}, object, outcome, 1.0, good ? 0.2 : -0.3);
    params["accuser"] = accuser.id;
    params["accused"] = accused.id;
    params["object"] = object;
    w.chronicle("case_" + outcome, {accuser.id, accused.id}, "chronicle.case_" + outcome, params);
}

void schedule(World& w, const std::string& stage, EntityId accuser, EntityId accused, EntityId object, Json data,
              double hours) {
    data["stage"] = stage;
    const Tick dt = std::max<Tick>(1, static_cast<Tick>(std::llround(hours * kTicksPerDay / 24.0)));
    w.schedule({w.now() + dt, 0, "case", accuser, accused, object, data});
}

struct Facts {
    bool guilty = false;
    EntityId record = kNone; // the accused's own deed
    std::map<std::string, int> goods;
    std::string tag = "theft";
};

// The truth (World::facts): the accused knows it of himself, nobody else reads it.
Facts factsOf(World& w, const Character& accuser, const Character& accused, EntityId object) {
    Facts f;
    if (const Fact* d = findDeed(w, object, accused.id, accuser.status.family)) {
        f.guilty = true;
        f.record = d->id;
        f.goods = d->goods;
        if (!d->pred.empty()) f.tag = d->pred;
    }
    return f;
}

void settle(World& w, EntityId record, const std::string& outcome) {
    for (auto& d : w.facts())
        if (d.id == record) d.outcome = outcome;
}

// The accused answers (stage "answer").
void answer(World& w, Character& accuser, Character& accused, EntityId object, const Json& data) {
    const CaseRules& R = caseRules(w.content());
    const Facts f = factsOf(w, accuser, accused, object);
    const int worth = lossWorth(R, f.goods, f.tag);
    const int payOff = static_cast<int>(std::ceil(worth * R.compensateMult));
    CaseCtx x;
    x.guilty = f.guilty;
    x.canReturn = f.guilty && holds(w, accused, f.goods);
    const std::map<std::string, int> pay = payment(w, accused, R, payOff);
    x.canPay = !pay.empty();
    x.seen = data.value("seen", false);
    x.evidence = data.value("confidence", 0.5);
    x.kin = kinOf(accuser, accused);
    x.crimes = crimesOf(accuser, accused, object);
    const CaseOption* o = pick(w, R, R.answers, accused, accuser, x, "case_answer:" + std::to_string(object) + ":");
    const std::string a = o ? o->id : "deny";
    trace(w, accused, 'S', "deed.case_answer", {{"answer", a}, {"accuser", accuser.id}, {"guilty", f.guilty}});
    if (a == "return" && x.canReturn) {
        const auto moved = hand(w, accused, accuser, f.goods);
        repute(w, accused, R, "returned");
        settle(w, f.record, "returned");
        relOutcome(accuser, accused, R, "returned");
        close(w, accuser, accused, object, "returned", {{"goods", goodsJson(moved)}});
    } else if ((a == "compensate" || a == "return") && x.canPay) {
        const auto moved = hand(w, accused, accuser, pay);
        repute(w, accused, R, "compensated");
        settle(w, f.record, "compensated");
        relOutcome(accuser, accused, R, "compensated");
        close(w, accuser, accused, object, "compensated", {{"goods", goodsJson(moved)}, {"value", payOff}});
    } else if (a == "flee") {
        repute(w, accused, R, "fled");
        settle(w, f.record, "fled");
        relOutcome(accuser, accused, R, "fled");
        close(w, accuser, accused, object, "fled");
        leave(w, accused, R.fleeM);
    } else {
        // innocent, and someone really saw who did it: he speaks up, the accused points to him (AL-55)
        if (!f.guilty) {
            const TruthCheck t = checkDoer(w, object, accused.id, accuser.status.family);
            if (t.contradicted && t.witness != kNone) {
                if (Character* wi = w.character(t.witness); wi && !wi->controller.isPlayer())
                    queueTask(w, *wi, R.vouchTask, accuser.id, R, "case");
                if (!accused.controller.isPlayer()) queueTask(w, accused, R.proveTask, accuser.id, R, "case");
            }
        }
        relOutcome(accuser, accused, R, "denied");
        w.chronicle("case_denied", {accused.id, accuser.id}, "chronicle.case_denied",
                    {{"accuser", accuser.id}, {"accused", accused.id}, {"object", object}});
        schedule(w, "pursue", accuser.id, accused.id, object, data, R.pursueHours);
    }
}

// The accuser, denied, chooses what next (stage "pursue").
void pursue(World& w, Character& accuser, Character& accused, EntityId object, Json data) {
    const CaseRules& R = caseRules(w.content());
    Character* judge = judgeOf(w, accuser, accused);
    CaseCtx x;
    x.hasJudge = judge != nullptr;
    x.seen = data.value("seen", false);
    x.evidence = data.value("confidence", 0.5);
    x.kin = kinOf(accuser, accused);
    x.crimes = crimesOf(accuser, accused, object);
    // a player's person is not made to quarrel or fight: it lets it go or goes to the judge
    std::vector<CaseOption> opts;
    for (const auto& o : R.pursuits)
        if (!accuser.controller.isPlayer() || o.id == "let_go" || o.id == "court") opts.push_back(o);
    const CaseOption* o = pick(w, R, opts, accuser, accused, x, "case_pursue:" + std::to_string(object) + ":");
    std::string p = o ? o->id : "let_go";
    if (p == "court" && !judge) p = "let_go";
    trace(w, accuser, 'S', "deed.case_pursue", {{"choice", p}, {"accused", accused.id}});
    if (p == "court") {
        if (MemoryEntry* m = pendingOf(accused, accuser.id, object)) {
            m->source = "court";
            m->who.resize(1);
            m->who.push_back(judge->id);
        }
        w.chronicle("case_to_court", {accuser.id, accused.id, judge->id}, "chronicle.case_to_court",
                    {{"accuser", accuser.id}, {"accused", accused.id}, {"judge", judge->id}, {"object", object}});
        if (judge->controller.isPlayer()) {
            // a player's person is asked, not made to: the court sits on its own if he does not
            trace(w, *judge, 'S', "deed.case_to_judge", {{"accuser", accuser.id}, {"accused", accused.id}});
        } else {
            queueTask(w, *judge, R.judgeTask, accused.id, R, "case");
        }
        data["judge"] = judge->id;
        schedule(w, "court", accuser.id, accused.id, object, data, R.courtWaitDays * 24.0);
        return;
    }
    if (p == "quarrel") queueTask(w, accuser, R.quarrelTask, accused.id, R, "case");
    if (p == "fight") queueTask(w, accuser, R.fightTask, accused.id, R, "case");
    relOutcome(accuser, accused, R, p);
    close(w, accuser, accused, object, p);
}

// The court (judge_case done, or the day it sits regardless).
void court(World& w, Character& judge, Character& accuser, Character& accused, EntityId object, double certainty,
           bool seen) {
    const CaseRules& R = caseRules(w.content());
    MemoryEntry* pend = pendingOf(accused, accuser.id, object);
    if (!pend || pend->source != "court") return; // already settled
    // Those who really saw it testify (the world's record): against the
    // accused when he did it, for him - naming the real doer - when another did.
    const Facts f = factsOf(w, accuser, accused, object);
    int witnesses = 0;
    if (f.guilty)
        for (const Fact& d : w.facts())
            if (d.id == f.record)
                for (EntityId id : trueWitnesses(w, d)) witnesses += id != accuser.id;
    EntityId realDoer = kNone;
    EntityId clearedBy = kNone;
    if (!f.guilty && object != kNone)
        if (const Fact* other = findDeed(w, object, kNone, accuser.status.family); other && other->role("doer") != accused.id)
            for (EntityId id : trueWitnesses(w, *other))
                if (id != accused.id) {
                    realDoer = other->role("doer");
                    clearedBy = id;
                    break;
                }
    const int crimes = crimesOf(judge, accused, object);
    double e = R.wCertainty * certainty + std::min(R.witnessCap, R.wWitness * witnesses) + R.wBadName * badName(accused) +
               std::min(R.priorCap, R.wPrior * crimes);
    if (clearedBy != kNone) e -= R.wCleared;
    // the thing itself found with him (searching him and his house): only what can be told apart
    bool found = false;
    if (f.guilty && goodsWorth(R, f.goods) >= R.foundMinWorth && holds(w, accused, f.goods)) {
        e += R.wFound;
        found = true;
    }
    if (auto it = judge.relations.find(accused.id); it != judge.relations.end())
        e += R.wJudgeAffinity * it->second.affinity + R.wJudgeEnmity * std::max(0.0, it->second.enmity);
    if (judge.id == accused.id) e = 0.0;
    Rng r = w.rng(judge.id, "case_court:" + std::to_string(object));
    e += R.courtNoise * (r.uniform() - 0.5);
    note(w, judge, "judged", {accused.id, accuser.id}, object, "court", 1.0, 0.0);
    trace(w, judge, 'S', "deed.case_verdict",
          {{"accused", accused.id}, {"evidence", e}, {"witnesses", witnesses}, {"found", found}, {"right", (e >= R.guiltyAt) == f.guilty}});
    if (e < R.guiltyAt) {
        repute(w, accused, R, "acquitted");
        repute(w, accuser, R, "false_accusation");
        relOutcome(accuser, accused, R, "acquitted");
        Json params = {{"judge", judge.id}};
        if (clearedBy != kNone) {
            params["real_doer"] = realDoer;
            params["cleared_by"] = clearedBy;
        }
        close(w, accuser, accused, object, "acquitted", params);
        // a witness named the real doer: the wronged now know who it was
        if (realDoer != kNone)
            if (Character* rd = w.character(realDoer)) {
                note(w, accuser, "told_crime", {realDoer, clearedBy}, object, "told", 0.9, -0.4);
                w.chronicle("case_cleared", {accused.id, clearedBy, realDoer}, "chronicle.case_cleared",
                            {{"accused", accused.id}, {"witness", clearedBy}, {"real_doer", realDoer}, {"object", object}});
                relShift(accuser, rd->id, {-0.2, 0.15});
            }
        return;
    }
    // Guilty: what was taken goes back (or its worth), then the sentence.
    const int worth = lossWorth(R, f.goods, f.tag);
    std::map<std::string, int> back;
    if (f.guilty && holds(w, accused, f.goods)) back = hand(w, accused, accuser, f.goods);
    else back = hand(w, accused, accuser, payment(w, accused, R, worth));
    const std::map<std::string, int> fine = payment(w, accused, R, R.fineValue);
    CaseCtx x;
    x.guilty = true;
    x.canPay = !fine.empty();
    x.seen = seen;
    x.evidence = e;
    x.kin = kinOf(judge, accused);
    x.crimes = crimes;
    const CaseOption* o = pick(w, R, R.sentences, judge, accused, x, "case_sentence:" + std::to_string(object) + ":");
    std::string s = o ? o->id : "shame";
    if (s == "fine" && fine.empty()) s = "shame";
    if (s == "custody" && (judge.id == accused.id || accused.captivity.held())) s = "shame";
    repute(w, accused, R, "convicted");
    repute(w, accused, R, s);
    relOutcome(accuser, accused, R, "convicted");
    std::map<std::string, int> paid;
    if (s == "fine") paid = hand(w, accused, accuser, fine);
    accused.status.verdicts.insert("case_" + s);
    settle(w, f.record, "convicted");
    close(w, accuser, accused, object, "convicted",
          {{"judge", judge.id}, {"sentence", s}, {"goods", goodsJson(back)}, {"fine", goodsJson(paid)}});
    if (Character* j = &judge; j->id != accused.id) relShift(accused, j->id, {0.0, 0.1});
    if (s == "exile") leave(w, accused, R.exileM);
    if (s == "custody") takeCaptive(w, judge, accused);
}

std::vector<CaseOption> loadOptions(const Json& arr) {
    std::vector<CaseOption> out;
    for (const auto& o : arr) {
        CaseOption op;
        op.id = o.value("id", std::string());
        op.base = o.value("base", 0.0);
        for (const auto& t : o.value("terms", Json::array())) {
            CaseTerm ct;
            for (const char* k : {"trait", "rel", "guilty", "can_return", "can_pay", "has_judge", "seen", "kin", "evidence",
                                  "crimes"})
                if (t.contains(k)) {
                    ct.kind = k;
                    if (t[k].is_string()) ct.key = t[k].get<std::string>();
                    else if (t[k].is_boolean()) ct.flag = t[k].get<bool>();
                    break;
                }
            ct.add = t.value("add", 0.0);
            ct.mul = t.value("mul", 0.0);
            if (t.contains("cap")) {
                ct.cap = t["cap"].get<double>();
                ct.capped = true;
            }
            op.terms.push_back(ct);
        }
        out.push_back(std::move(op));
    }
    return out;
}

double num(const Json& j, const char* k, double v) { return j.contains(k) && j[k].is_number() ? j[k].get<double>() : v; }

std::map<std::string, CaseRules::RelShift> loadRel(const Json& j) {
    std::map<std::string, CaseRules::RelShift> out;
    for (auto it = j.begin(); it != j.end(); ++it)
        if (it->is_object()) out[it.key()] = {num(*it, "trust", 0.0), num(*it, "enmity", 0.0)};
    return out;
}

} // namespace

int goodsWorth(const CaseRules& R, const std::map<std::string, int>& goods) {
    int v = 0;
    for (const auto& [item, n] : goods) {
        auto it = R.worth.find(item);
        v += n * (it == R.worth.end() ? 1 : it->second);
    }
    return v;
}

double caseScore(const CaseOption& o, const Character& chooser, const Character& other, const CaseCtx& x) {
    double s = o.base;
    for (const auto& t : o.terms) {
        if (t.kind == "trait") {
            if (chooser.hasTrait(t.key)) s += t.add;
        } else if (t.kind == "rel") {
            auto it = chooser.relations.find(other.id);
            if (it != chooser.relations.end()) s += t.mul * it->second.get(t.key);
        } else if (t.kind == "evidence") {
            s += t.mul * x.evidence + t.add;
        } else if (t.kind == "crimes") {
            double v = t.add * x.crimes;
            if (t.capped) v = t.add >= 0.0 ? std::min(v, t.cap) : std::max(v, t.cap);
            s += v;
        } else {
            const bool v = t.kind == "guilty"       ? x.guilty
                           : t.kind == "can_return" ? x.canReturn
                           : t.kind == "can_pay"    ? x.canPay
                           : t.kind == "has_judge"  ? x.hasJudge
                           : t.kind == "seen"       ? x.seen
                           : t.kind == "kin"        ? x.kin
                                                    : false;
            if (v == t.flag) s += t.add;
        }
    }
    return s;
}

const CaseRules& caseRules(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<CaseRules>> cache;
    static const CaseRules kDefaults;
    if (c.dir.empty()) return kDefaults;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(c.dir);
    if (it != cache.end()) return *it->second;
    auto r = std::make_unique<CaseRules>();
    const fs::path p = fs::path(c.dir) / "consequences.json";
    if (fs::exists(p)) {
        std::ifstream f(p, std::ios::binary);
        const Json root = Json::parse(f);
        const Json j = root.value("case", Json::object());
        const Json t = j.value("timing", Json::object());
        r->answerHours = num(t, "answer_h", r->answerHours);
        r->pursueHours = num(t, "pursue_h", r->pursueHours);
        r->courtWaitDays = num(t, "court_wait_days", r->courtWaitDays);
        r->noise = num(j, "noise", r->noise);
        if (j.contains("worth"))
            for (auto w = j["worth"].begin(); w != j["worth"].end(); ++w)
                if (w->is_number()) r->worth[w.key()] = w->get<int>();
        if (j.contains("default_worth"))
            for (auto w = j["default_worth"].begin(); w != j["default_worth"].end(); ++w)
                if (w->is_number()) r->defaultWorth[w.key()] = w->get<int>();
        r->compensateMult = num(j, "compensate_mult", r->compensateMult);
        r->fineValue = static_cast<int>(num(j, "fine_value", r->fineValue));
        r->answers = loadOptions(j.value("answers", Json::array()));
        r->pursuits = loadOptions(j.value("pursuits", Json::array()));
        r->sentences = loadOptions(j.value("sentences", Json::array()));
        const Json co = j.value("court", Json::object());
        r->guiltyAt = num(co, "guilty_at", r->guiltyAt);
        r->wCertainty = num(co, "certainty", r->wCertainty);
        r->wWitness = num(co, "witness", r->wWitness);
        r->witnessCap = num(co, "witness_cap", r->witnessCap);
        r->wBadName = num(co, "bad_name", r->wBadName);
        r->wPrior = num(co, "prior", r->wPrior);
        r->priorCap = num(co, "prior_cap", r->priorCap);
        r->wJudgeAffinity = num(co, "judge_affinity", r->wJudgeAffinity);
        r->wJudgeEnmity = num(co, "judge_enmity", r->wJudgeEnmity);
        r->courtNoise = num(co, "noise", r->courtNoise);
        r->wCleared = num(co, "cleared", r->wCleared);
        r->wFound = num(co, "found", r->wFound);
        r->foundMinWorth = static_cast<int>(num(co, "found_min_worth", r->foundMinWorth));
        const Json ta = j.value("tasks", Json::object());
        r->judgeTask = ta.value("judge", r->judgeTask);
        r->quarrelTask = ta.value("quarrel", r->quarrelTask);
        r->fightTask = ta.value("fight", r->fightTask);
        r->vouchTask = ta.value("vouch", r->vouchTask);
        r->proveTask = ta.value("prove", r->proveTask);
        r->taskPriority = num(ta, "priority", r->taskPriority);
        r->taskTtl = static_cast<Tick>(num(ta, "ttl", static_cast<double>(r->taskTtl)));
        r->fleeM = num(j, "flee_m", r->fleeM);
        r->exileM = num(j, "exile_m", r->exileM);
        if (j.contains("reputation"))
            for (auto o = j["reputation"].begin(); o != j["reputation"].end(); ++o)
                if (o->is_object())
                    for (auto a = o->begin(); a != o->end(); ++a) r->reputation[o.key()][a.key()] = a->get<double>();
        const Json rel = j.value("rel", Json::object());
        r->accuserToAccused = loadRel(rel.value("accuser", Json::object()));
        r->accusedToAccuser = loadRel(rel.value("accused", Json::object()));
    }
    auto* out = r.get();
    cache[c.dir] = std::move(r);
    return *out;
}

EntityId keepTruth(World& w, Character& doer, const InteractionDef& d, EntityId victimFamily, EntityId victimPerson,
               EntityId object, const std::string& tag, const std::vector<EntityId>& witnesses) {
    Fact f;
    f.pred = tag;
    f.roles["doer"] = doer.id;
    if (victimPerson != kNone) f.roles["victim"] = victimPerson;
    if (victimFamily != kNone) f.roles["victim_family"] = victimFamily;
    if (object != kNone) f.roles["object"] = object;
    if (const WorldObject* o = w.object(object)) f.place = o->pos;
    else f.place = doer.pos;
    f.tags = {"deed", d.baseId.empty() ? d.id : d.baseId};
    for (const auto& e : d.eff)
        if (e.subject == "target_stock" && e.signedDelta() < 0.0 && !e.isParam())
            f.goods[e.key] += std::max(1, static_cast<int>(std::lround(-e.signedDelta())));
    for (EntityId id : witnesses)
        if (id != doer.id) {
            f.witnesses.push_back(id);
            f.evidence.push_back({{"kind", "seen"}, {"who", id}});
        }
    // where the thing went: with the doer (found by searching him or his house)
    for (const auto& [item, n] : f.goods) f.evidence.push_back({{"kind", "goods"}, {"item", item}, {"n", n}, {"holder", doer.id}});
    return w.addFact(std::move(f)).id;
}

const Fact* findDeed(const World& w, EntityId object, EntityId doer, EntityId victimFamily) {
    // the latest open misdeed
    for (auto it = w.facts().rbegin(); it != w.facts().rend(); ++it) {
        const Fact& f = *it;
        if (!f.outcome.empty() || f.role("doer") == kNone) continue;
        if (doer != kNone && f.role("doer") != doer) continue;
        if (object != kNone ? f.role("object") == object
                            : (victimFamily != kNone && f.role("victim_family") == victimFamily))
            return &f;
    }
    return nullptr;
}

std::vector<EntityId> trueWitnesses(const World& w, const Fact& f) {
    std::vector<EntityId> out;
    for (EntityId id : f.witnesses)
        if (const Character* c = w.character(id); c && c->body.alive && id != f.role("doer")) out.push_back(id);
    return out;
}

void caseAfterAct(World& w, Character& doer, const InteractionDef& d, Character* targetChar, bool success) {
    if (!targetChar || targetChar->isAnimal() || targetChar->id == doer.id || !success) return;
    const std::string& id = d.baseId.empty() ? d.id : d.baseId;
    const CaseRules& R = caseRules(w.content());
    if (id == consequenceRules(w.content()).accuseTask) {
        Character& accused = *targetChar;
        // what the accuser holds against him: the surest word of a deed
        // (his own, else what his household knows: one speaks for all)
        const MemoryEntry* best = nullptr;
        auto scan = [&](const Character& c) {
            for (const auto& m : c.memory)
                if (isDeedMemory(m.type) && !m.who.empty() && m.who[0] == accused.id &&
                    (!best || m.confidence > best->confidence))
                    best = &m;
        };
        scan(doer);
        if (!best)
            if (auto fam = w.families().find(doer.status.family); fam != w.families().end())
                for (EntityId id : fam->second.members)
                    if (const Character* m = w.character(id); m && m->id != doer.id) scan(*m);
        const EntityId object = best ? best->object : kNone;
        const double conf = best ? best->confidence : 0.5;
        const bool seen = best && best->type == "saw_crime";
        const EntityId fact = best ? best->fact : kNone;
        if (pendingOf(accused, doer.id, object)) return;
        for (const auto& m : accused.memory) // settled already
            if (m.type == kClosed && !m.who.empty() && m.who[0] == doer.id && m.object == object && object != kNone) return;
        note(w, accused, kPending, {doer.id}, object, "answer", conf, -0.4);
        if (MemoryEntry* p = pendingOf(accused, doer.id, object)) p->fact = fact;
        w.chronicle("case_opened", {doer.id, accused.id}, "chronicle.case_opened",
                    {{"accuser", doer.id}, {"accused", accused.id}, {"object", object}, {"confidence", conf}});
        schedule(w, "answer", doer.id, accused.id, object, {{"confidence", conf}, {"seen", seen}}, R.answerHours);
        return;
    }
    if (id == R.judgeTask) {
        Character& accused = *targetChar;
        for (const auto& m : std::vector<MemoryEntry>(accused.memory))
            if (m.type == kPending && m.source == "court" && m.who.size() > 1 && m.who[1] == doer.id)
                if (Character* accuser = w.character(m.who[0]); accuser && accuser->body.alive)
                    court(w, doer, *accuser, accused, m.object, m.confidence, false);
    }
}

void closeCase(World& w, Character& accuser, Character& accused, EntityId object, const std::string& outcome,
               Json params) {
    if (!pendingOf(accused, accuser.id, object)) return;
    close(w, accuser, accused, object, outcome, std::move(params));
}

bool casePending(Character& accused, EntityId accuser, EntityId object) {
    return pendingOf(accused, accuser, object) != nullptr;
}

void installJustice(World& w) {
    w.addEventHandler([](World& world, const Event& e) {
        if (e.type != "case") return false;
        Character* accuser = world.character(e.a);
        Character* accused = world.character(e.b);
        if (!accuser || !accused) return true;
        if (!accused->body.alive || !accuser->body.alive) {
            if (accused) close(world, *accuser, *accused, e.obj, "dropped");
            return true;
        }
        if (!pendingOf(*accused, accuser->id, e.obj)) return true;
        const std::string stage = e.data.value("stage", std::string());
        if (accused->captivity.held() && stage != "court") { // the captor's fate decides now
            close(world, *accuser, *accused, e.obj, "dropped");
            return true;
        }
        if (stage == "answer") answer(world, *accuser, *accused, e.obj, e.data);
        else if (stage == "pursue") pursue(world, *accuser, *accused, e.obj, e.data);
        else if (stage == "court") {
            Character* judge = world.character(e.data.value("judge", kNone));
            if (!judge || !judge->body.alive) judge = judgeOf(world, *accuser, *accused);
            if (!judge) {
                close(world, *accuser, *accused, e.obj, "dropped");
                return true;
            }
            court(world, *judge, *accuser, *accused, e.obj, e.data.value("confidence", 0.5), e.data.value("seen", false));
        }
        return true;
    });
}

} // namespace lr::sim
