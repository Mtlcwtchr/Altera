// LR-171: prayers and rites of the cults. See rites.hpp.
#include "sim/rites.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <utility>

#include "sim/ai.hpp"
#include "sim/char_expr.hpp"
#include "sim/content.hpp"
#include "sim/culture.hpp"
#include "sim/data_fs.hpp"
#include "sim/expr.hpp"
#include "sim/magic_society.hpp"
#include "sim/world.hpp"
#include "sim/world_fields.hpp"

namespace lr::sim {

namespace fs = std::filesystem;

namespace {

PrayerAnswerFn& answerFn() {
    static PrayerAnswerFn f;
    return f;
}
RiteDoneFn& riteDoneFn() {
    static RiteDoneFn f;
    return f;
}

double num(const Json& j, const char* k, double d) {
    auto it = j.find(k);
    return it != j.end() && it->is_number() ? it->get<double>() : d;
}

// The object under a key (an empty one when missing): safe to iterate in a range-for.
const Json& sub(const Json& j, const char* k) {
    static const Json kEmpty = Json::object();
    auto it = j.find(k);
    return it != j.end() && it->is_object() ? *it : kEmpty;
}

std::vector<std::string> strs(const Json& j, const char* k) {
    std::vector<std::string> out;
    auto it = j.find(k);
    if (it == j.end()) return out;
    if (it->is_string()) out.push_back(it->get<std::string>());
    else if (it->is_array())
        for (const Json& s : *it)
            if (s.is_string()) out.push_back(s.get<std::string>());
    return out;
}

std::vector<Effect> effs(const Json& j, const char* k, const std::string& where, std::vector<std::string>& errors) {
    std::vector<Effect> out;
    for (const std::string& s : strs(j, k)) {
        try {
            out.push_back(parseEffect(s));
        } catch (const std::exception& e) {
            errors.push_back(where + ": bad effect '" + s + "': " + e.what());
        }
    }
    return out;
}

RiteDef parseRite(const Json& rj, const std::string& group, const std::vector<std::string>& faiths,
                  const std::vector<std::string>& liks, const std::string& file, std::vector<std::string>& errors) {
    RiteDef r;
    r.id = rj.value("id", std::string());
    r.group = group;
    r.file = file;
    r.kind = rj.value("kind", std::string("offering"));
    r.open = rj.value("open", true);
    r.secret = rj.value("secret", false);
    r.faiths = rj.contains("faiths") ? strs(rj, "faiths") : faiths;
    r.liks = rj.contains("liks") ? strs(rj, "liks") : liks;
    r.grades = strs(rj, "grades");
    const std::string where = "rites." + r.id;
    const Json when = rj.value("when", Json::object());
    if (when.contains("holy_day")) r.holyDay = when["holy_day"].is_string() ? when["holy_day"].get<std::string>() : "any";
    r.on = when.value("on", std::string());
    r.everyDays = static_cast<int>(num(when, "every_days", 0));
    r.offset = static_cast<int>(num(when, "offset", 0));
    if (when.contains("season") && when["season"].is_array() && when["season"].size() == 2) {
        r.seasonFrom = when["season"][0].get<int>();
        r.seasonTo = when["season"][1].get<int>();
    }
    r.chance = num(when, "chance", 1.0);
    r.time = rj.value("time", std::string("day"));
    const Json p = rj.value("participants", Json::object());
    r.who = p.value("who", r.who);
    r.age = p.value("age", r.age);
    r.minN = static_cast<int>(num(p, "min", r.minN));
    r.maxN = static_cast<int>(num(p, "max", r.maxN));
    r.radius = num(p, "radius_m", r.radius);
    r.leader = rj.value("leader", r.leader);
    if (rj.contains("offering")) {
        r.offerAny = strs(rj["offering"], "any");
        r.offerN = static_cast<int>(num(rj["offering"], "n", 1));
    }
    r.attire = rj.value("attire", std::string());
    for (const Json& s : rj.value("steps", Json::array())) {
        RiteStep st;
        st.all = strs(s, "all");
        st.leader = strs(s, "leader");
        st.ticks = std::max(1, static_cast<int>(num(s, "ticks", 2)));
        st.fx = s.value("fx", std::string());
        r.steps.push_back(std::move(st));
    }
    const Json e = rj.value("effects", Json::object());
    r.effAll = effs(e, "all", where, errors);
    r.effLeader = effs(e, "leader", where, errors);
    r.piety = num(e, "piety", 0.0);
    r.memory = strs(e, "memory");
    r.channels = e.value("channels", Json::object());
    r.aim = e.value("aim", std::string());
    for (const Json& a : rj.value("aftermath", Json::array())) {
        RiteDef::After af;
        af.p = num(a, "p", 0.0);
        af.who = a.value("who", std::string("any"));
        af.eff = effs(a, "eff", where + ".aftermath", errors);
        r.aftermath.push_back(std::move(af));
    }
    if (rj.contains("intimacy") && rj["intimacy"].is_object()) {
        const Json& i = rj["intimacy"];
        auto& I = r.intimacy;
        I.on = true;
        I.adultsOnly = i.value("adults_only", false);
        I.consent = i.value("consent", false);
        I.minLifeYears = static_cast<int>(num(i, "min_life_years", I.minLifeYears));
        I.share = num(i, "pairs_share", I.share);
        I.affinity = num(i, "affinity", I.affinity);
        I.attraction = num(i, "attraction", I.attraction);
        I.act = i.value("act", I.act);
        I.show = i.value("show", std::string());
    }
    return r;
}

std::unique_ptr<RitesRules> loadRules(const fs::path& dir) {
    auto r = std::make_unique<RitesRules>();
    const fs::path pp = dir / "prayers.json";
    if (!data::exists(pp.string())) return r;
    try {
        const Json j = data::readJson(pp.string());
        r->prayActs = strs(j, "acts");
        const Json wj = j.value("weight", Json::object());
        auto& W = r->weight;
        W.base = num(wj, "base", W.base);
        W.piety = num(wj, "piety", W.piety);
        W.rite = num(wj, "rite", W.rite);
        W.holyDay = num(wj, "holy_day", W.holyDay);
        W.max = num(wj, "max", W.max);
        for (const auto& [k, v] : sub(wj, "acts").items())
            if (v.is_number()) W.acts[k] = v.get<double>();
        for (const auto& [k, v] : sub(j, "offerings").items())
            if (v.is_number()) r->offerings[k] = v.get<double>();
        const Json fb = j.value("fallback", Json::object());
        auto& F = r->fallback;
        F.grantBase = num(fb, "grant_base", F.grantBase);
        F.grantPerWeight = num(fb, "grant_per_weight", F.grantPerWeight);
        F.grantMax = num(fb, "grant_max", F.grantMax);
        F.deferShare = num(fb, "defer_share", F.deferShare);
        if (fb.contains("defer_days") && fb["defer_days"].is_array() && fb["defer_days"].size() == 2) {
            F.deferMin = fb["defer_days"][0].get<int>();
            F.deferMax = fb["defer_days"][1].get<int>();
        }
        F.strengthBase = num(fb, "strength_base", F.strengthBase);
        F.strengthPerWeight = num(fb, "strength_per_weight", F.strengthPerWeight);
        for (const auto& [k, v] : sub(j, "answers").items()) {
            RitesRules::Answer a;
            a.moodlet = v.value("moodlet", std::string());
            a.memory = v.value("memory", std::string());
            a.chronicle = v.value("chronicle", std::string());
            a.piety = num(v, "piety", 0.0);
            r->answers[k] = a;
        }
        for (const Json& a : j.value("aims", Json::array())) {
            AimDef d;
            d.id = a.value("id", std::string());
            d.when = a.value("when", std::string("0"));
            d.pick = num(a, "pick", 1.0);
            d.forWhom = a.value("for", std::string());
            d.refuseIfKin = a.value("refuse_if_kin", false);
            const Json g = a.value("granted", Json::object());
            d.eff = effs(g, "eff", "prayers." + d.id, r->errors);
            d.targetEff = effs(g, "target_eff", "prayers." + d.id, r->errors);
            d.householdEff = effs(g, "household_eff", "prayers." + d.id, r->errors);
            d.channels = g.value("channels", Json::object());
            d.radius = num(g, "radius_m", d.radius);
            r->aims.push_back(std::move(d));
        }
        r->griefDays = static_cast<int>(num(j, "grief_days", r->griefDays));
        r->guiltTypes = strs(j, "guilt_types");
        r->foeTypes = strs(j, "foe_types");
        const fs::path rr = dir / "rite_rules.json";
        if (data::exists(rr.string())) {
            r->rules = data::readJson(rr.string());
            for (const auto& [k, v] : sub(r->rules, "time").items())
                if (v.is_number()) r->timeTick[k] = v.get<int>();
            r->clergy = strs(r->rules, "clergy");
            r->maxPerDay = static_cast<int>(num(r->rules, "max_per_day", r->maxPerDay));
            r->eventDays = static_cast<int>(num(r->rules, "event_days", r->eventDays));
            r->queuePriority = num(r->rules, "queue_priority", r->queuePriority);
            r->droughtBelow = num(r->rules, "drought_below", r->droughtBelow);
            for (const auto& [k, v] : sub(r->rules, "on").items()) {
                r->onTypes[k] = strs(v, "types");
                r->onCodes[k] = strs(v, "code_has");
            }
        }
        for (const fs::path& fp : data::listPaths(dir / "religions")) {
            const Json fj = data::readJson(fp.string());
            const std::string id = fj.value("id", fp.stem().string());
            auto& m = r->faithLiks[id];
            if (fj.contains("liks") && fj["liks"].is_object()) {
                for (const auto& [belt, v] : fj["liks"].items())
                    for (const Json& x : v)
                        if (x.is_string()) m[belt].insert(x.get<std::string>());
            } else
                for (const std::string& x : strs(fj, "liks")) m["*"].insert(x);
            for (const Json& g : fj.value("deities", Json::array()))
                if (g.is_object() && g.contains("id")) m["*"].insert(g["id"].get<std::string>());
        }
        for (const fs::path& fpp : data::listPaths(dir / "rites")) {
            const std::string fp = fpp.string();
            const Json fj = data::readJson(fp);
            const std::string group = fj.value("group", fs::path(fp).stem().string());
            const auto faiths = strs(fj, "faiths");
            const auto liks = strs(fj, "liks");
            for (const Json& rj : fj.value("rites", Json::array())) {
                RiteDef d = parseRite(rj, group, faiths, liks, fs::path(fp).filename().string(), r->errors);
                if (d.id.empty()) {
                    r->errors.push_back("rites/" + fs::path(fp).filename().string() + ": a rite without id");
                    continue;
                }
                if (r->rites.count(d.id)) {
                    r->errors.push_back("rites: duplicate rite " + d.id);
                    continue;
                }
                r->riteOrder.push_back(d.id);
                r->rites.emplace(d.id, std::move(d));
            }
        }
        r->loaded = true;
    } catch (const std::exception& e) {
        r->errors.push_back(std::string("rites: ") + e.what());
    }
    return r;
}

bool person(const Character* c) { return c && !c->isAnimal() && c->body.alive; }
bool child(const Character& c) { return c.body.stage() == "child"; }
int dayNow(const World& w) { return w.day(); }

void applyText(World& w, Character& c, const std::vector<Effect>& list, WorldObject* place, Character* other = nullptr) {
    if (!list.empty()) applyEffectList(w, c, list, place, other, nullptr);
}
void moodlet(World& w, Character& c, const std::string& id) {
    if (id.empty() || !w.content().moodlets.count(id)) return;
    applyText(w, c, {parseEffect("moodlet:" + id)}, nullptr);
}
void addPiety(World& w, Character& c, double d) {
    if (d == 0.0) return;
    c.piety = std::clamp(pietyOf(w, c) + d, 0.0, 1.0);
}
void remember(World& w, Character& c, const std::string& type, std::vector<EntityId> who, const std::string& good,
              double valence) {
    MemoryEntry m;
    m.type = type;
    m.who = std::move(who);
    if (!good.empty()) m.goods[good] = 1;
    m.valence = valence;
    w.remember(c, std::move(m));
}

bool recentMemory(const World& w, const Character& c, const std::vector<std::string>& types, int days,
                  EntityId* who = nullptr) {
    const Tick since = w.now() - static_cast<Tick>(days) * kTicksPerDay;
    for (auto it = c.memory.rbegin(); it != c.memory.rend(); ++it) {
        if (it->tick < since) break;
        if (std::find(types.begin(), types.end(), it->type) != types.end()) {
            if (who) *who = it->who.empty() ? kNone : it->who.front();
            return true;
        }
    }
    return false;
}

bool hasMoodlet(const World& w, const Character& c, const std::string& prefix) {
    for (const auto& m : c.moodlets)
        if (m.until > w.now() && m.id.rfind(prefix, 0) == 0) return true;
    return false;
}

const Settlement* settlementOf(const World& w, EntityId sid) {
    auto it = w.settlements().find(sid);
    return it == w.settlements().end() ? nullptr : &it->second;
}

std::vector<EntityId> householdOf(const World& w, const Character& c) {
    std::vector<EntityId> out;
    if (const Family* f = w.familyOf(c))
        for (EntityId m : f->members)
            if (person(w.character(m))) out.push_back(m);
    if (out.empty()) out.push_back(c.id);
    return out;
}

bool kin(const Character& a, const Character& b) {
    if (a.status.family != kNone && a.status.family == b.status.family) return true;
    if (a.status.spouse == b.id || a.status.mother == b.id || a.status.father == b.id) return true;
    if (b.status.mother == a.id || b.status.father == a.id) return true;
    return a.status.mother != kNone && (a.status.mother == b.status.mother || a.status.father == b.status.father);
}

bool drought(const World& w, EntityId sid) {
    const Settlement* s = settlementOf(w, sid);
    return s && fieldAt(w, Field::moisture, s->center) < ritesRules(w.content()).droughtBelow;
}

// The sickest of the household (health or an illness), kNone when all are well.
EntityId sickOf(const World& w, const Character& c) {
    EntityId best = kNone;
    double worst = 0.75;
    for (EntityId id : householdOf(w, c)) {
        const Character* m = w.character(id);
        if (!m) continue;
        double h = m->body.health;
        if (hasMoodlet(w, *m, "ill_") || hasMoodlet(w, *m, "sick")) h = std::min(h, 0.6);
        if (h < worst) worst = h, best = id;
    }
    return best;
}

struct AimArgs {
    std::map<std::string, double> v;
    EntityId sick = kNone, foe = kNone;
};

AimArgs aimArgs(const World& w, const Character& c) {
    const RitesRules& r = ritesRules(w.content());
    AimArgs a;
    a.sick = sickOf(w, c);
    a.v["sick"] = a.sick != kNone;
    a.v["grief"] = hasMoodlet(w, c, "grieving");
    a.v["drought"] = drought(w, c.status.settlement);
    a.v["war"] = recentMemory(w, c, {"war_declared", "raid", "attacked_by_beast"}, 20) || hasMoodlet(w, c, "alarmed");
    const Settlement* s = settlementOf(w, c.status.settlement);
    a.v["travel"] = (s && dist(c.pos, s->center) > 1500.0) || c.occupation == "trader" || c.occupation.rfind("caravan", 0) == 0;
    a.v["sea"] = c.skills.count("fishing") && c.skills.at("fishing") > 0.3;
    a.v["hunter"] = c.skills.count("hunting") && c.skills.at("hunting") > 0.3;
    a.v["foe"] = recentMemory(w, c, r.foeTypes, 30, &a.foe) && a.foe != kNone && a.foe != c.id;
    a.v["guilt"] = recentMemory(w, c, r.guiltTypes, 30) || hasMoodlet(w, c, "guilty");
    const int y = c.body.lifeYears();
    a.v["single"] = !child(c) && c.status.spouse == kNone && y >= 18 && y <= 45;
    bool kids = !c.status.children.empty();
    a.v["childless"] = c.status.spouse != kNone && !kids && y >= 18 && y <= 45;
    const int doy = dayOfYear(w);
    bool fields = false;
    if (c.status.family != kNone)
        for (EntityId id : w.objectsOfType("field"))
            if (const WorldObject* o = w.object(id); o && o->ownerFamily == c.status.family) fields = true;
    a.v["sown"] = fields && doy >= 60 && doy <= 230;
    return a;
}

std::map<std::string, Expr>& exprCache() {
    static std::map<std::string, Expr> m;
    return m;
}
double evalWhen(const std::string& src, const Character& c, const AimArgs& a) {
    static std::mutex mu;
    Expr e;
    {
        std::lock_guard<std::mutex> lock(mu);
        auto& cache = exprCache();
        auto it = cache.find(src);
        if (it == cache.end()) {
            std::string err;
            it = cache.emplace(src, Expr::compile(src, &err)).first;
        }
        e = it->second;
    }
    CharExprCtx ctx(c);
    ctx.args = a.v;
    return e.eval(ctx, 0.0);
}

// The settlement's rite state: moduleState("rites").
Json& state(World& w) {
    Json& s = w.moduleState("rites");
    if (!s.is_object()) s = Json::object();
    for (const char* k : {"active", "done", "deferred"})
        if (!s.contains(k)) s[k] = Json::array();
    for (const char* k : {"pending", "grades", "last", "answers"})
        if (!s.contains(k)) s[k] = Json::object();
    return s;
}
const Json* stateC(const World& w) { return w.moduleState("rites"); }

std::string bestOffering(const World& w, const Character& c) {
    const RitesRules& r = ritesRules(w.content());
    std::string best;
    double bw = 0.0;
    for (const auto& [item, n] : c.inventory) {
        if (n <= 0) continue;
        auto it = r.offerings.find(item);
        if (it == r.offerings.end()) continue;
        // food one needs is kept: only a spare piece is given
        if (n < 2 && w.content().items.count(item) && w.content().items.at(item).nutrition > 0.0) continue;
        if (it->second > bw) bw = it->second, best = item;
    }
    return best;
}

bool takeItem(Character& c, const std::string& item) {
    auto it = c.inventory.find(item);
    if (it == c.inventory.end() || it->second <= 0) return false;
    --it->second;
    return true;
}

std::string riteFaithOk(const Content& ct, const RiteDef& rd, const std::string& faith, const std::string& belt = "") {
    if (std::find(rd.faiths.begin(), rd.faiths.end(), faith) != rd.faiths.end()) return faith;
    const auto& fl = ritesRules(ct).faithLiks;
    auto f = fl.find(faith);
    if (f == fl.end()) return "";
    for (const auto& [b, liks] : f->second) {
        if (!belt.empty() && b != belt && b != "*") continue;
        for (const auto& l : rd.liks)
            if (liks.count(l)) return faith;
    }
    return "";
}

std::string beltOf(const World& w, EntityId sid) {
    const Json* cd = cultureData(w.content(), settlementCulture(w, sid));
    return cd ? cd->value("belt", std::string()) : std::string();
}

bool cultureForbids(const World& w, EntityId sid, const RiteDef& rd) {
    const Json* cd = cultureData(w.content(), settlementCulture(w, sid));
    if (!cd) return false;
    for (const std::string& k : strs(*cd, "rites_forbidden"))
        if (k == rd.id || k == rd.kind) return true;
    return false;
}

// The nearest object of the step's act type to `at` within `radius` (kNone: none).
EntityId placeFor(const World& w, const std::string& act, Vec2 at, double radius, std::string* chosen = nullptr) {
    const InteractionDef* d = w.content().interaction(act);
    if (!d) return kNone;
    if (!d->targetsObject()) return kNone;
    EntityId best = kNone;
    double bd = radius;
    for (EntityId id : w.objectsOfType(d->targetType)) {
        const WorldObject* o = w.object(id);
        if (!o) continue;
        const double dd = dist(o->pos, at);
        if (dd < bd) bd = dd, best = id;
    }
    if (best != kNone && chosen) *chosen = act;
    return best;
}

// Picks the act among `acts` whose place is near: {act, place}.
std::pair<std::string, EntityId> stepAct(const World& w, const std::vector<std::string>& acts, Vec2 at, double radius) {
    for (const auto& a : acts)
        if (EntityId p = placeFor(w, a, at, radius); p != kNone) return {a, p};
    return {"", kNone};
}

Tick riteStartTick(const RitesRules& r, const RiteDef& d, Tick dayStart) {
    auto it = r.timeTick.find(d.time);
    return dayStart + (it == r.timeTick.end() ? 66 : it->second);
}

struct EventMap {
    // settlement -> event -> people concerned (the dead, the wed, the newborn, the sick)
    std::map<EntityId, std::map<std::string, std::vector<EntityId>>> by;
};

EventMap recentEvents(const World& w, int days) {
    const RitesRules& r = ritesRules(w.content());
    EventMap m;
    const Tick since = w.now() - static_cast<Tick>(days) * kTicksPerDay;
    const auto& log = w.chronicleLog();
    for (auto it = log.rbegin(); it != log.rend(); ++it) {
        if (it->tick < since) break;
        for (const auto& [ev, types] : r.onTypes) {
            if (std::find(types.begin(), types.end(), it->type) == types.end()) continue;
            const auto& codes = r.onCodes.at(ev);
            if (!codes.empty() && std::none_of(codes.begin(), codes.end(),
                                               [&](const std::string& c) { return it->code.find(c) != std::string::npos; }))
                continue;
            for (EntityId id : it->who) {
                const Character* c = w.character(id);
                if (!c || c->isAnimal()) continue;
                EntityId sid = c->status.settlement;
                if (sid == kNone) continue;
                auto& v = m.by[sid][ev];
                if (std::find(v.begin(), v.end(), id) == v.end()) v.push_back(id);
            }
            // a war line without people concerns every settlement
            if (it->who.empty() && ev == "war")
                for (const auto& [sid, s] : w.settlements()) m.by[sid][ev];
        }
    }
    return m;
}

bool whenFits(const World& w, const RiteDef& d, const FaithDef* f, int doy, int day, EntityId sid,
              const EventMap& ev, std::string* why) {
    bool any = false, ok = true;
    if (!d.holyDay.empty()) {
        any = true;
        const std::string h = f ? holyDayOn(*f, doy) : std::string();
        ok = ok && !h.empty() && (d.holyDay == "any" || d.holyDay == h);
        if (why && ok) *why = "holy_day:" + h;
    }
    if (d.everyDays > 0) {
        any = true;
        ok = ok && (day % d.everyDays) == (d.offset % d.everyDays);
    }
    if (d.seasonFrom >= 0) {
        any = true;
        ok = ok && doy >= d.seasonFrom && doy <= d.seasonTo;
    }
    if (!d.on.empty()) {
        any = true;
        if (d.on == "drought") ok = ok && drought(w, sid);
        else {
            auto s = ev.by.find(sid);
            ok = ok && s != ev.by.end() && s->second.count(d.on);
        }
    }
    return any && ok;
}

// The leader: clergy of the faith, else the most pious adult of it.
EntityId pickLeader(const World& w, const RiteDef& d, const std::string& faith, const std::vector<Character*>& pool) {
    const RitesRules& r = ritesRules(w.content());
    const FaithDef* f = cultureRules(w.content()).faithDef(faith);
    Character* best = nullptr;
    double bs = -1.0;
    for (Character* c : pool) {
        if (child(*c) || c->controller.isPlayer()) continue;
        double s = pietyOf(w, *c);
        const bool cler = (f && !f->clergy.empty() && c->occupation == f->clergy) ||
                          std::find(r.clergy.begin(), r.clergy.end(), c->occupation) != r.clergy.end();
        if (d.leader == "clergy" && cler) s += 2.0;
        if (d.leader == "head" && c->status.role == "head") s += 2.0;
        if (s > bs) bs = s, best = c;
    }
    return best ? best->id : kNone;
}

bool offeringMatches(const Content& c, const std::string& want, const std::string& item) {
    if (want.rfind("tag:", 0) != 0) return want == item;
    auto it = c.items.find(item);
    if (it == c.items.end()) return false;
    const std::string t = want.substr(4);
    for (const auto& x : it->second.tags)
        if (x == t) return true;
    return false;
}

bool takeOffering(World& w, const RiteDef& d, EntityId leader, Json& rec) {
    if (d.offerAny.empty()) return true;
    int need = std::max(1, d.offerN);
    Character* L = w.character(leader);
    Json given = Json::object();
    auto take = [&](std::map<std::string, int>& stock) {
        for (const auto& want : d.offerAny) {
            // an item id, or "tag:<t>" (LR-168 tags offerings: offering, offering_libation...)
            for (auto& [item, n] : stock) {
                if (need <= 0) break;
                if (!offeringMatches(w.content(), want, item)) continue;
                while (need > 0 && n > 0) {
                    --n;
                    --need;
                    given[item] = given.value(item, 0) + 1;
                }
            }
        }
    };
    if (L) take(L->inventory);
    if (need > 0 && L)
        if (const Family* f = w.familyOf(*L))
            if (WorldObject* st = w.object(f->storage)) take(st->stock);
    if (need > 0)
        for (EntityId id : rec["people"].get<std::vector<EntityId>>()) {
            if (need <= 0) break;
            const Character* c = w.character(id);
            if (!c) continue;
            if (const Family* f = w.familyOf(*c))
                if (WorldObject* st = w.object(f->storage)) take(st->stock);
        }
    rec["offered"] = given;
    return need <= 0;
}

double riteWeight(const World& w, const RiteDef& d, const Json& rec, int attended) {
    const RitesRules& r = ritesRules(w.content());
    const Json W = r.rules.value("weight", Json::object());
    double v = num(W, "base", 0.8) + num(W, "per_attendee", 0.25) * std::log1p(std::max(0, attended));
    if (rec.value("holy", false)) v += num(W, "holy_day", 0.4);
    const Json off = rec.value("offered", Json::object());
    if (!d.offerAny.empty()) {
        if (off.empty()) v *= num(W, "no_offering", 0.5);
        else {
            double s = 0.0;
            int n = 0;
            for (const auto& [item, k] : off.items()) {
                auto it = r.offerings.find(item);
                s += (it == r.offerings.end() ? 0.3 : it->second) * k.get<int>();
                n += k.get<int>();
            }
            v += s / std::max(1, n) * std::min(2.0, 0.5 + 0.25 * n);
        }
    }
    return std::min(r.weight.max, v);
}

void chron(World& w, const std::string& code, std::vector<EntityId> who, Json params) {
    w.chronicle("rite", std::move(who), "chronicle." + code, std::move(params));
}

// Gives a step: queues the act for every participant who is not the player.
void queueStep(World& w, Json& rec, int step) {
    const RitesRules& r = ritesRules(w.content());
    const RiteDef* d = r.rite(rec["rite"].get<std::string>());
    if (!d || step >= static_cast<int>(d->steps.size())) return;
    const RiteStep& st = d->steps[step];
    const Settlement* s = settlementOf(w, rec["settlement"].get<EntityId>());
    const Vec2 at = s ? s->center : Vec2{};
    const EntityId leader = rec["leader"].get<EntityId>();
    auto [allAct, allPlace] = stepAct(w, st.all, at, d->radius * 2);
    auto [ldAct, ldPlace] = st.leader.empty() ? std::make_pair(allAct, allPlace) : stepAct(w, st.leader, at, d->radius * 2);
    if (ldAct.empty()) ldAct = allAct, ldPlace = allPlace;
    rec["step"] = step;
    rec["act"] = allAct;
    rec["leader_act"] = ldAct;
    rec["place"] = allPlace;
    rec["fx"] = st.fx;
    const Tick ttl = static_cast<Tick>(st.ticks) + 18;
    for (EntityId id : rec["people"].get<std::vector<EntityId>>()) {
        Character* c = w.character(id);
        if (!person(c) || c->controller.isPlayer()) continue;
        const bool lead = id == leader;
        const std::string& act = lead ? ldAct : allAct;
        const EntityId place = lead ? ldPlace : allPlace;
        if (act.empty() || place == kNone) continue;
        // the earlier step's act, if still waiting, gives way
        auto& q = c->ai.queue;
        q.erase(std::remove_if(q.begin(), q.end(), [](const QueuedTask& t) { return t.source == "rites"; }), q.end());
        q.push_back(QueuedTask{act, place, r.queuePriority + (lead ? 0.05 : 0.0), w.now(), ttl, "rite:" + d->id, "rites"});
    }
    // §1.1: intimacy in an ecstatic step is only between adults and only by consent
    if (d->intimacy.on && allAct == "rite_revel") {
        const auto& I = d->intimacy;
        std::vector<Character*> adults;
        for (EntityId id : rec["people"].get<std::vector<EntityId>>()) {
            Character* c = w.character(id);
            if (person(c) && !child(*c) && c->body.lifeYears() >= I.minLifeYears && !c->controller.isPlayer())
                adults.push_back(c);
        }
        Rng rng = w.rng(leader, "rite:intimacy:" + d->id);
        std::set<EntityId> used;
        Json pairs = Json::array();
        const int maxPairs = static_cast<int>(std::floor(I.share * adults.size() / 2.0 + rng.uniform()));
        for (Character* a : adults) {
            if (static_cast<int>(pairs.size()) >= maxPairs) break;
            if (used.count(a->id)) continue;
            Character* best = nullptr;
            double bs = 0.0;
            for (Character* b : adults) {
                if (b == a || used.count(b->id)) continue;
                auto ra = a->relations.find(b->id);
                auto rb = b->relations.find(a->id);
                if (ra == a->relations.end() || rb == b->relations.end()) continue;
                // both want it: the consent of both sides
                if (ra->second.affinity < I.affinity || rb->second.affinity < I.affinity) continue;
                if (ra->second.attraction < I.attraction || rb->second.attraction < I.attraction) continue;
                const double s = ra->second.attraction + rb->second.attraction;
                if (s > bs) bs = s, best = b;
            }
            if (!best) continue;
            used.insert(a->id);
            used.insert(best->id);
            pairs.push_back(Json::array({a->id, best->id}));
            a->ai.queue.push_back(QueuedTask{I.act, best->id, r.queuePriority - 0.05, w.now(), ttl + 6,
                                             "rite:" + d->id, "rites"});
        }
        rec["pairs"] = pairs;
    }
}

void finishRite(World& w, Json& rec) {
    const RitesRules& r = ritesRules(w.content());
    const RiteDef* d = r.rite(rec["rite"].get<std::string>());
    if (!d) return;
    const EntityId leader = rec["leader"].get<EntityId>();
    const std::string faith = rec.value("faith", std::string());
    std::vector<EntityId> came;
    for (const auto& [k, v] : rec["attended"].items())
        if (v.get<int>() > 0) came.push_back(static_cast<EntityId>(std::stoul(k)));
    std::sort(came.begin(), came.end());
    const double weight = riteWeight(w, *d, rec, static_cast<int>(came.size()));
    rec["weight"] = weight;
    rec["came"] = came;
    WorldObject* place = rec.value("place", kNone) != kNone ? w.object(rec["place"].get<EntityId>()) : nullptr;
    Json& st = state(w);
    for (EntityId id : came) {
        Character* c = w.character(id);
        if (!person(c)) continue;
        applyText(w, *c, d->effAll, place);
        if (id == leader) applyText(w, *c, d->effLeader, place);
        addPiety(w, *c, d->piety);
        for (const auto& m : d->memory) remember(w, *c, m, {leader}, "rite:" + d->id, 0.3);
        if (d->secret && !d->grades.empty()) {
            Json& g = st["grades"][std::to_string(id)];
            if (!g.is_object()) g = Json::object();
            const int was = g.value(d->id, -1);
            const int now = std::min(static_cast<int>(d->grades.size()) - 1, was + 1);
            g[d->id] = now;
            if (now > was && now == static_cast<int>(d->grades.size()) - 1)
                chron(w, "rites.grade.top", {id}, {{"who", id}, {"rite", d->id}});
        }
        if (riteDoneFn()) riteDoneFn()(w, id, faith, d->id, weight);
    }
    if (!came.empty() && place && d->channels.is_object() && !d->channels.empty())
        applyChannels(w, place->pos, 300.0, d->channels, weight * 0.5);
    // aftermath
    std::set<EntityId> intimates;
    for (const Json& p : rec.value("pairs", Json::array()))
        for (const Json& x : p) intimates.insert(x.get<EntityId>());
    Rng rng = w.rng(leader, "rite:after:" + d->id);
    for (const auto& af : d->aftermath) {
        std::vector<std::pair<EntityId, EntityId>> who; // the one, the other (the intimate partner)
        if (af.who == "leader") who.push_back({leader, kNone});
        else if (af.who == "intimate") {
            for (EntityId id : intimates) who.push_back({id, kNone});
        } else if (af.who == "spouse_of_intimate") {
            for (const Json& p : rec.value("pairs", Json::array()))
                for (int k = 0; k < 2; ++k) {
                    const Character* c = w.character(p[k].get<EntityId>());
                    const EntityId other = p[1 - k].get<EntityId>();
                    if (c && c->status.spouse != kNone && c->status.spouse != other) who.push_back({c->status.spouse, c->id});
                }
        } else
            for (EntityId id : came) who.push_back({id, kNone});
        for (auto [id, other] : who) {
            Character* c = w.character(id);
            if (!person(c) || !rng.chance(af.p)) continue;
            applyText(w, *c, af.eff, place, other != kNone ? w.character(other) : nullptr);
        }
    }
    chron(w, "rites.done", {leader}, {{"a", leader}, {"rite", d->id}, {"n", static_cast<int>(came.size())}});
    // the common prayer of the rite
    if (!d->aim.empty() && !came.empty()) {
        Prayer p;
        p.who = leader;
        p.faith = faith;
        p.aim = d->aim;
        p.rite = d->id;
        p.place = place ? place->id : kNone;
        p.day = dayNow(w);
        p.weight = weight;
        if (Character* L = w.character(leader)) {
            if (d->aim == "heal") p.target = sickOf(w, *L);
            offerPrayer(w, p);
        }
    }
}

void fallbackAnswer(World& w, const Prayer& p, PrayerAnswer& a) {
    const RitesRules& r = ritesRules(w.content());
    const auto& F = r.fallback;
    Rng rng = w.rng(p.who, "prayer:" + p.aim);
    const double grant = std::clamp(F.grantBase + F.grantPerWeight * p.weight, 0.0, F.grantMax);
    const double roll = rng.uniform();
    if (roll < grant) {
        a.kind = PrayerAnswer::Granted;
        a.strength = std::clamp(F.strengthBase + F.strengthPerWeight * p.weight, 0.0, 1.0);
    } else if (roll < grant + (1.0 - grant) * F.deferShare) {
        a.kind = PrayerAnswer::Deferred;
        a.dueDay = dayNow(w) + rng.irange(F.deferMin, F.deferMax);
        a.strength = std::clamp(F.strengthBase + F.strengthPerWeight * p.weight, 0.0, 1.0);
    } else {
        a.kind = PrayerAnswer::Silent;
    }
}

const char* kindName(PrayerAnswer::Kind k) {
    switch (k) {
    case PrayerAnswer::Granted: return "granted";
    case PrayerAnswer::Refused: return "refused";
    case PrayerAnswer::Deferred: return "deferred";
    default: return "silent";
    }
}

Json prayerJson(const Prayer& p) {
    return {{"who", p.who}, {"faith", p.faith}, {"lik", p.lik}, {"aim", p.aim}, {"offering", p.offering},
            {"rite", p.rite}, {"place", p.place}, {"target", p.target}, {"day", p.day}, {"weight", p.weight}};
}
Prayer prayerFrom(const Json& j) {
    Prayer p;
    p.who = j.value("who", kNone);
    p.faith = j.value("faith", std::string());
    p.lik = j.value("lik", std::string());
    p.aim = j.value("aim", std::string());
    p.offering = j.value("offering", std::string());
    p.rite = j.value("rite", std::string());
    p.place = j.value("place", kNone);
    p.target = j.value("target", kNone);
    p.day = j.value("day", 0);
    p.weight = j.value("weight", 0.0);
    return p;
}

std::vector<Character*> poolOf(World& w, EntityId sid, const RiteDef& d, const std::string& faith, const EventMap& ev,
                               Rng& rng) {
    std::vector<Character*> all;
    for (auto& [id, c] : w.characters())
        if (person(&c) && c.status.settlement == sid && !c.captivity.held()) all.push_back(&c);
    std::vector<Character*> out;
    std::set<EntityId> concerned;
    if (!d.on.empty())
        if (auto s = ev.by.find(sid); s != ev.by.end())
            if (auto e = s->second.find(d.on); e != s->second.end()) concerned.insert(e->second.begin(), e->second.end());
    std::set<EntityId> families;
    if (d.who == "household" || d.who == "mourners") {
        for (EntityId id : concerned)
            if (const Character* c = w.character(id)) families.insert(c->status.family);
        if (families.empty() && !all.empty()) {
            // a household rite with no event: one household by turn
            std::vector<EntityId> fams;
            for (Character* c : all)
                if (c->status.family != kNone && std::find(fams.begin(), fams.end(), c->status.family) == fams.end())
                    fams.push_back(c->status.family);
            if (!fams.empty()) families.insert(fams[rng.irange(0, static_cast<int>(fams.size()) - 1)]);
        }
    }
    const Json& grades = state(w)["grades"];
    for (Character* c : all) {
        if (d.age == "adult" && child(*c)) continue;
        if (d.who == "faith" && faithOf(w, *c) != faith) continue;
        if (d.who == "initiates") {
            if (faithOf(w, *c) != faith) continue;
            // the initiates and those pious enough to be called to the first grade
            const std::string k = std::to_string(c->id);
            const bool init = grades.contains(k) && grades[k].contains(d.id);
            if (!init && pietyOf(w, *c) < 0.55) continue;
        }
        if (d.who == "household" && !families.count(c->status.family)) continue;
        if (d.who == "mourners") {
            bool near = families.count(c->status.family) > 0;
            for (EntityId dead : concerned)
                if (const Character* x = w.character(dead))
                    if (auto it = c->relations.find(dead); it != c->relations.end() && it->second.affinity > 0.2) near = true;
            if (!near && faithOf(w, *c) != faith) continue;
        }
        out.push_back(c);
    }
    // the most pious first, cut to the max
    std::stable_sort(out.begin(), out.end(), [&](Character* a, Character* b) { return pietyOf(w, *a) > pietyOf(w, *b); });
    if (static_cast<int>(out.size()) > d.maxN) out.resize(d.maxN);
    return out;
}

std::string scheduleRite(World& w, EntityId sid, const RiteDef& d, const std::string& faith, Tick at, EntityId leader,
                         const EventMap& ev, bool holy) {
    const Settlement* s = settlementOf(w, sid);
    if (!s) return "no_settlement";
    if (d.steps.empty()) return "no_steps";
    if (cultureForbids(w, sid, d)) return "forbidden";
    // the first step needs its place near
    if (stepAct(w, d.steps.front().all, s->center, d.radius * 2).first.empty()) return "no_place";
    Rng rng = w.rng(sid, "rite:pool:" + d.id);
    auto pool = poolOf(w, sid, d, faith, ev, rng);
    if (static_cast<int>(pool.size()) < d.minN) return "too_few";
    if (leader == kNone) leader = pickLeader(w, d, faith, pool);
    if (leader == kNone) return "no_leader";
    Json rec = {{"rite", d.id}, {"settlement", sid}, {"faith", faith}, {"leader", leader}, {"start", at},
                {"step", -1}, {"holy", holy}, {"attended", Json::object()}, {"kind", d.kind}, {"attire", d.attire}};
    Json people = Json::array();
    for (Character* c : pool) people.push_back(c->id);
    if (std::find(people.begin(), people.end(), Json(leader)) == people.end()) people.push_back(leader);
    rec["people"] = people;
    takeOffering(w, d, leader, rec);
    state(w)["active"].push_back(std::move(rec));
    return "";
}

} // namespace

// --- rules ---------------------------------------------------------------------------------
int RiteDef::totalTicks() const {
    int t = 0;
    for (const auto& s : steps) t += s.ticks;
    return t;
}
const AimDef* RitesRules::aim(const std::string& id) const {
    for (const auto& a : aims)
        if (a.id == id) return &a;
    return nullptr;
}
const RiteDef* RitesRules::rite(const std::string& id) const {
    auto it = rites.find(id);
    return it == rites.end() ? nullptr : &it->second;
}

const RitesRules& ritesRules(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<RitesRules>> cache;
    static const RitesRules kNoRules;
    if (c.dir.empty()) return kNoRules;
    std::lock_guard<std::mutex> lock(mu);
    const std::string key = data::cacheKey(c.dir);
    auto it = cache.find(key);
    if (it == cache.end()) it = cache.emplace(key, loadRules(fs::path(c.dir).parent_path() / "culture")).first;
    return *it->second;
}

void setPrayerAnswerHook(PrayerAnswerFn f) { answerFn() = std::move(f); }
void setRiteDoneHook(RiteDoneFn f) { riteDoneFn() = std::move(f); }

// --- prayers -----------------------------------------------------------------------------
std::string prayerAimOf(World& w, const Character& c, EntityId* forWhom) {
    const RitesRules& r = ritesRules(w.content());
    const AimArgs a = aimArgs(w, c);
    std::vector<std::pair<const AimDef*, double>> opts;
    double total = 0.0;
    for (const auto& d : r.aims) {
        const double v = evalWhen(d.when, c, a);
        if (v <= 0.0) continue;
        opts.push_back({&d, v * d.pick});
        total += v * d.pick;
    }
    if (opts.empty() || total <= 0.0) return "thanks";
    Rng rng = w.rng(c.id, "prayer:aim");
    double x = rng.uniform() * total;
    const AimDef* pick = opts.back().first;
    for (auto& [d, v] : opts) {
        if ((x -= v) <= 0.0) {
            pick = d;
            break;
        }
    }
    if (forWhom) *forWhom = pick->forWhom == "sick" ? a.sick : pick->forWhom == "foe" ? a.foe : kNone;
    return pick->id;
}

double prayerWeight(const World& w, const Character& c, const std::string& act, const std::string& offering,
                    const std::string& rite) {
    const RitesRules& r = ritesRules(w.content());
    double v = r.weight.base + r.weight.piety * pietyOf(w, c);
    if (auto it = r.weight.acts.find(act); it != r.weight.acts.end()) v += it->second;
    if (auto it = r.offerings.find(offering); !offering.empty() && it != r.offerings.end()) v += it->second;
    if (!rite.empty()) v += r.weight.rite;
    if (const FaithDef* f = cultureRules(w.content()).faithDef(faithOf(w, c)))
        if (!holyDayOn(*f, dayOfYear(w)).empty()) v += r.weight.holyDay;
    return std::min(r.weight.max, v);
}

void fulfillPrayer(World& w, const Prayer& p, double strength) {
    const RitesRules& r = ritesRules(w.content());
    const AimDef* a = r.aim(p.aim);
    Character* c = w.character(p.who);
    if (!a || !c) return;
    strength = std::clamp(strength, 0.0, 1.0);
    // a weak answer gives the effects with the chance of its strength
    Rng rng = w.rng(p.who, "prayer:fulfil:" + p.aim);
    const bool full = rng.uniform() < std::max(0.35, strength);
    if (full) {
        if (c->body.alive) applyText(w, *c, a->eff, w.object(p.place));
        if (Character* t = p.target != kNone ? w.character(p.target) : c; t && t->body.alive && !a->targetEff.empty()) {
            // the effects on another: applied as that person's own (moodlets, body)
            applyText(w, *t, a->targetEff, nullptr);
        }
        for (EntityId id : householdOf(w, *c))
            if (Character* m = w.character(id); m && id != c->id) applyText(w, *m, a->householdEff, nullptr);
    }
    if (a->channels.is_object() && !a->channels.empty())
        if (const Settlement* s = settlementOf(w, c->status.settlement))
            applyChannels(w, s->center, a->radius, a->channels, strength);
}

PrayerAnswer offerPrayer(World& w, Prayer p) {
    const RitesRules& r = ritesRules(w.content());
    PrayerAnswer a;
    Character* c = w.character(p.who);
    if (!r.loaded || !c) return a;
    if (p.faith.empty()) p.faith = faithOf(w, *c);
    if (p.day == 0) p.day = dayNow(w);
    const AimDef* aim = r.aim(p.aim);
    // rules the world keeps before any god: revenge on one's own kin is refused
    if (aim && aim->refuseIfKin && p.target != kNone)
        if (const Character* t = w.character(p.target); t && kin(*c, *t)) {
            a.kind = PrayerAnswer::Refused;
            a.reason = "rites.refuse.kin";
        }
    if (a.reason.empty()) {
        if (answerFn()) a = answerFn()(w, p);
        else fallbackAnswer(w, p, a);
    }
    const std::string kind = kindName(a.kind);
    Json& st = state(w);
    st["answers"][std::to_string(p.who)] = {{"aim", p.aim}, {"answer", kind}, {"reason", a.reason}, {"day", p.day},
                                            {"tick", w.now()}, {"weight", p.weight}, {"lik", p.lik}};
    const auto ans = r.answers.find(kind);
    if (ans != r.answers.end()) {
        moodlet(w, *c, ans->second.moodlet);
        addPiety(w, *c, ans->second.piety);
        if (!ans->second.memory.empty()) remember(w, *c, ans->second.memory, {}, "aim:" + p.aim, 0.4);
        if (!ans->second.chronicle.empty())
            chron(w, ans->second.chronicle, {p.who}, {{"who", p.who}, {"aim", p.aim}, {"why", a.reason}});
    }
    if (a.kind == PrayerAnswer::Granted) fulfillPrayer(w, p, a.strength);
    if (a.kind == PrayerAnswer::Deferred && !answerFn()) {
        Json d = prayerJson(p);
        d["due"] = a.dueDay;
        d["strength"] = a.strength;
        st["deferred"].push_back(d);
    }
    return a;
}

// --- rites ---------------------------------------------------------------------------------
std::vector<const RiteDef*> ritesOfFaith(const Content& c, const std::string& faith, const std::string& belt) {
    std::vector<const RiteDef*> out;
    const RitesRules& r = ritesRules(c);
    for (const auto& id : r.riteOrder) {
        const RiteDef& d = r.rites.at(id);
        if (!riteFaithOk(c, d, faith, belt).empty()) out.push_back(&d);
    }
    return out;
}

std::string startRite(World& w, EntityId settlement, const std::string& rite, Tick at, EntityId leader) {
    const RitesRules& r = ritesRules(w.content());
    const RiteDef* d = r.rite(rite);
    if (!d) return "unknown_rite";
    // the faith: the leader's, else the settlement's most common that keeps it
    std::string faith;
    if (const Character* L = w.character(leader)) faith = faithOf(w, *L);
    if (faith.empty() || riteFaithOk(w.content(), *d, faith).empty()) {
        faith.clear();
        int best = 0;
        for (const auto& [f, n] : settlementBelief(w, settlement).faiths)
            if (!riteFaithOk(w.content(), *d, f).empty() && n > best) best = n, faith = f;
    }
    if (faith.empty()) return "no_faith";
    const EventMap ev = recentEvents(w, r.eventDays);
    return scheduleRite(w, settlement, *d, faith, at < 0 ? w.now() : at, leader, ev, false);
}

Json riteOf(const World& w, EntityId who) {
    const Json* st = stateC(w);
    if (!st || !st->contains("active")) return nullptr;
    for (const Json& rec : (*st)["active"]) {
        if (rec.value("step", -1) < 0) continue;
        const auto& ppl = rec["people"];
        if (std::find(ppl.begin(), ppl.end(), Json(who)) == ppl.end()) continue;
        Json o = {{"rite", rec["rite"]}, {"kind", rec["kind"]}, {"step", rec["step"]}, {"fx", rec.value("fx", "")},
                  {"attire", rec.value("attire", "")}, {"leader", rec["leader"].get<EntityId>() == who}};
        for (const Json& p : rec.value("pairs", Json::array()))
            if (p[0] == who || p[1] == who) o["pair"] = p[0] == who ? p[1] : p[0];
        return o;
    }
    return nullptr;
}

Json ritesJson(const World& w, EntityId settlement) {
    Json out = {{"active", Json::array()}, {"done", Json::array()}};
    const Json* st = stateC(w);
    if (!st) return out;
    for (const char* k : {"active", "done"})
        if (st->contains(k))
            for (const Json& rec : (*st)[k]) {
                if (settlement != kNone && rec.value("settlement", kNone) != settlement) continue;
                Json o = rec;
                o.erase("attended");
                if (const WorldObject* p = w.object(rec.value("place", kNone)))
                    o["at"] = Json::array({p->pos.x, p->pos.y});
                out[k].push_back(std::move(o));
            }
    return out;
}

int riteGrade(const World& w, EntityId who, const std::string& rite) {
    const Json* st = stateC(w);
    if (!st || !st->contains("grades")) return -1;
    const std::string k = std::to_string(who);
    if (!(*st)["grades"].contains(k) || !(*st)["grades"][k].is_object()) return -1;
    return (*st)["grades"][k].value(rite, -1);
}

// --- passes ---------------------------------------------------------------------------------
void ritesDaily(World& w) {
    const RitesRules& r = ritesRules(w.content());
    if (!r.loaded) return;
    Json& st = state(w);
    const int day = dayNow(w);
    // the fallback's deferred prayers come due
    Json keep = Json::array();
    for (Json& d : st["deferred"]) {
        if (d.value("due", 0) > day) {
            keep.push_back(d);
            continue;
        }
        const Prayer p = prayerFrom(d);
        fulfillPrayer(w, p, d.value("strength", 0.5));
        if (Character* c = w.character(p.who); c && c->body.alive) {
            const auto ans = r.answers.find("granted");
            if (ans != r.answers.end()) moodlet(w, *c, ans->second.moodlet);
            chron(w, "rites.prayer.came_true", {p.who}, {{"who", p.who}, {"aim", p.aim}});
        }
    }
    st["deferred"] = keep;
    // the rites of today, per settlement
    const int doy = dayOfYear(w);
    const Tick dayStart = w.now() - (w.now() % kTicksPerDay);
    const EventMap ev = recentEvents(w, r.eventDays);
    const CultureRules& cr = cultureRules(w.content());
    for (const auto& [sid, s] : w.settlements()) {
        const SettlementBelief b = settlementBelief(w, sid);
        if (b.residents <= 0) continue;
        int big = 0;
        std::vector<std::pair<std::string, int>> faiths(b.faiths.begin(), b.faiths.end());
        std::stable_sort(faiths.begin(), faiths.end(), [](auto& x, auto& y) { return x.second > y.second; });
        std::set<std::string> started;
        for (const auto& [faith, n] : faiths) {
            const FaithDef* f = cr.faithDef(faith);
            for (const RiteDef* d : ritesOfFaith(w.content(), faith, beltOf(w, sid))) {
                if (started.count(d->id)) continue;
                const bool household = d->who == "household" || d->who == "mourners";
                if (!household && big >= r.maxPerDay) continue;
                std::string why;
                if (!whenFits(w, *d, f, doy, day, sid, ev, &why)) continue;
                // an event rite once per event: not again while the same event is recent
                const std::string lastKey = std::to_string(sid) + ":" + d->id;
                const int last = st["last"].value(lastKey, -1000);
                if (!d->on.empty() && day - last <= r.eventDays) continue;
                if (d->chance < 1.0 && !w.rng(sid, "rite:chance:" + d->id).chance(d->chance)) continue;
                const Tick at = std::max(w.now(), riteStartTick(r, *d, dayStart));
                if (!scheduleRite(w, sid, *d, faith, at, kNone, ev, !d->holyDay.empty()).empty()) continue;
                st["last"][lastKey] = day;
                started.insert(d->id);
                if (!household) ++big;
            }
        }
    }
}

void ritesTick(World& w) {
    const RitesRules& r = ritesRules(w.content());
    if (!r.loaded) return;
    Json* stp = static_cast<const World&>(w).moduleState("rites") ? &w.moduleState("rites") : nullptr;
    if (!stp || !stp->contains("active") || (*stp)["active"].empty()) return;
    Json& st = *stp;
    Json keep = Json::array();
    const int keepDone = static_cast<int>(num(r.rules, "keep_finished", 6));
    for (Json& rec : st["active"]) {
        const RiteDef* d = r.rite(rec.value("rite", std::string()));
        if (!d) continue;
        const Tick start = rec["start"].get<Tick>();
        if (w.now() < start) {
            keep.push_back(std::move(rec));
            continue;
        }
        const Tick el = w.now() - start;
        int step = 0;
        Tick acc = 0;
        while (step < static_cast<int>(d->steps.size()) && el >= acc + d->steps[step].ticks + 6) acc += d->steps[step].ticks + 6, ++step;
        if (step >= static_cast<int>(d->steps.size())) {
            finishRite(w, rec);
            rec.erase("people");
            rec["end"] = w.now();
            st["done"].push_back(std::move(rec));
            while (static_cast<int>(st["done"].size()) > keepDone) st["done"].erase(st["done"].begin());
            continue;
        }
        if (rec.value("step", -1) != step) queueStep(w, rec, step);
        keep.push_back(std::move(rec));
    }
    st["active"] = keep;
}

void ritesAfterAct(World& w, Character& c, const InteractionDef& d, WorldObject* target, Character*, bool success) {
    const RitesRules& r = ritesRules(w.content());
    if (!r.loaded || !success || c.isAnimal()) return;
    // a pray act prays: the person's own aim, or the one named by the command
    if (std::find(r.prayActs.begin(), r.prayActs.end(), d.id) != r.prayActs.end()) {
        Json& st = state(w);
        const std::string k = std::to_string(c.id);
        Prayer p;
        p.who = c.id;
        p.place = target ? target->id : kNone;
        if (st["pending"].contains(k)) {
            const Json pj = st["pending"][k];
            st["pending"].erase(k);
            p.aim = pj.value("aim", std::string());
            p.lik = pj.value("lik", std::string());
            p.faith = pj.value("faith", std::string());
            p.offering = pj.value("offering", std::string());
            p.target = pj.value("target", kNone);
        }
        if (p.aim.empty() || !r.aim(p.aim)) p.aim = prayerAimOf(w, c, &p.target);
        if (p.offering.empty()) p.offering = bestOffering(w, c);
        if (!p.offering.empty() && !takeItem(c, p.offering)) p.offering.clear();
        p.weight = prayerWeight(w, c, d.id, p.offering, "");
        offerPrayer(w, p);
        return;
    }
    // a rite step act counts the person in the rite going on at that place
    if (d.id.rfind("rite_", 0) != 0) return;
    Json* stp = static_cast<const World&>(w).moduleState("rites") ? &w.moduleState("rites") : nullptr;
    if (!stp || !stp->contains("active")) return;
    for (Json& rec : (*stp)["active"]) {
        if (rec.value("step", -1) < 0) continue;
        if (rec.value("act", std::string()) != d.id && rec.value("leader_act", std::string()) != d.id) continue;
        auto& ppl = rec["people"];
        const bool in = std::find(ppl.begin(), ppl.end(), Json(c.id)) != ppl.end();
        // an open rite takes in whoever joins it at its place (the player too)
        if (!in) {
            const RiteDef* rd = r.rite(rec["rite"].get<std::string>());
            if (!rd || !rd->open || (rd->age == "adult" && child(c))) continue;
            ppl.push_back(c.id);
        }
        const std::string k = std::to_string(c.id);
        rec["attended"][k] = rec["attended"].value(k, 0) + 1;
        return;
    }
}

namespace {
bool handleRites(World& w, Command& cmd) {
    if (cmd.type != "pray" && cmd.type != "start_rite") return false;
    const RitesRules& r = ritesRules(w.content());
    if (cmd.type == "start_rite") {
        const EntityId sid = cmd.args.value("settlement", kNone);
        const std::string res = startRite(w, sid, cmd.args.value("rite", std::string()), -1, cmd.args.value("leader", kNone));
        cmd.result = res.empty() ? "ok" : "rejected:" + res;
        return true;
    }
    Character* c = cmd.args.contains("id") ? w.character(cmd.args.at("id").get<EntityId>()) : nullptr;
    if (!person(c)) {
        cmd.result = "rejected:no_such_character";
        return true;
    }
    const std::string aim = cmd.args.value("aim", std::string());
    if (!aim.empty() && !r.aim(aim)) {
        cmd.result = "rejected:unknown_aim";
        return true;
    }
    const std::string off = cmd.args.value("offering", std::string());
    if (!off.empty() && (!c->inventory.count(off) || c->inventory.at(off) <= 0)) {
        cmd.result = "rejected:no_offering";
        return true;
    }
    Json pj = {{"aim", aim}, {"lik", cmd.args.value("lik", std::string())}, {"faith", cmd.args.value("faith", std::string())},
               {"offering", off}, {"target", cmd.args.value("target", kNone)}};
    if (cmd.args.value("now", false)) {
        // prays here and now (a quick prayer without walking to a shrine)
        Prayer p;
        p.who = c->id;
        p.aim = aim.empty() ? prayerAimOf(w, *c, &p.target) : aim;
        if (!aim.empty()) p.target = pj["target"].get<EntityId>();
        p.lik = pj["lik"];
        p.faith = pj["faith"];
        p.offering = off;
        if (!off.empty()) takeItem(*c, off);
        p.weight = prayerWeight(w, *c, "pray_alone", off, "");
        const PrayerAnswer a = offerPrayer(w, p);
        cmd.result = std::string("ok:") + kindName(a.kind);
        return true;
    }
    state(w)["pending"][std::to_string(c->id)] = pj;
    cmd.result = "ok";
    return true;
}
} // namespace

void installRites(World& w) { w.addCommandHandler(handleRites); }

void validateRites(const Content& c, ValidationReport& rep) {
    const RitesRules& r = ritesRules(c);
    for (const auto& e : r.errors) rep.errors.push_back(e);
    if (!r.loaded) return;
    for (const auto& a : r.prayActs)
        if (!c.interaction(a)) rep.errors.push_back("prayers: unknown pray act " + a);
    for (const auto& [k, v] : r.offerings)
        if (!c.items.count(k)) rep.warnings.push_back("prayers: offering " + k + " is no item (yet)");
    auto checkMood = [&](const std::vector<Effect>& list, const std::string& where) {
        for (const auto& e : list)
            if (e.subject == "moodlet" && !c.moodlets.count(e.key)) rep.errors.push_back(where + ": unknown moodlet " + e.key);
    };
    for (const auto& a : r.aims) {
        std::string err;
        Expr::compile(a.when, &err);
        if (!err.empty()) rep.errors.push_back("prayers." + a.id + ": when: " + err);
        checkMood(a.eff, "prayers." + a.id);
        checkMood(a.targetEff, "prayers." + a.id);
        checkMood(a.householdEff, "prayers." + a.id);
    }
    for (const auto& [k, a] : r.answers)
        if (!a.moodlet.empty() && !c.moodlets.count(a.moodlet)) rep.errors.push_back("prayers.answers." + k + ": unknown moodlet " + a.moodlet);
    // the liks named by the rites are the pantheon's (content/pantheon/liks/*.json), when it is there
    std::set<std::string> known;
    for (const fs::path& fp : data::listPaths(fs::path(c.dir).parent_path() / "pantheon" / "liks")) {
        const Json fj = data::readJson(fp.string());
        for (const Json& l : fj.value("liks", Json::array()))
            if (l.is_object() && l.contains("id")) known.insert(l["id"].get<std::string>());
    }
    if (!known.empty())
        for (const auto& id : r.riteOrder)
            for (const auto& l : r.rites.at(id).liks)
                if (!known.count(l)) rep.errors.push_back("rites." + id + ": unknown lik " + l);
    static const std::set<std::string> who = {"faith", "settlement", "household", "initiates", "mourners"};
    static const std::set<std::string> leaders = {"clergy", "pious", "head"};
    static const std::set<std::string> afterWho = {"any", "intimate", "spouse_of_intimate", "leader"};
    for (const auto& id : r.riteOrder) {
        const RiteDef& d = r.rites.at(id);
        const std::string where = "rites." + id;
        if (d.steps.empty()) rep.errors.push_back(where + ": no steps");
        if (d.holyDay.empty() && d.on.empty() && d.everyDays <= 0 && d.seasonFrom < 0)
            rep.errors.push_back(where + ": no when (holy_day, every_days, season or on)");
        if (!d.on.empty() && d.on != "drought" && !r.onTypes.count(d.on)) rep.errors.push_back(where + ": unknown event " + d.on);
        if (!r.timeTick.count(d.time)) rep.errors.push_back(where + ": unknown time " + d.time);
        if (!who.count(d.who)) rep.errors.push_back(where + ": unknown participants.who " + d.who);
        if (!leaders.count(d.leader)) rep.errors.push_back(where + ": unknown leader " + d.leader);
        if (!d.aim.empty() && !r.aim(d.aim)) rep.errors.push_back(where + ": unknown aim " + d.aim);
        if (d.faiths.empty() && d.liks.empty()) rep.errors.push_back(where + ": kept by no faith and no lik");
        if (d.secret && d.grades.empty()) rep.errors.push_back(where + ": a secret rite without grades");
        for (const auto& s : d.steps)
            for (const auto* acts : {&s.all, &s.leader})
                for (const auto& a : *acts) {
                    const InteractionDef* ia = c.interaction(a);
                    if (!ia) rep.errors.push_back(where + ": unknown step act " + a);
                    else if (!ia->targetsObject()) rep.errors.push_back(where + ": step act " + a + " has no place (object target)");
                }
        checkMood(d.effAll, where);
        checkMood(d.effLeader, where);
        for (const auto& af : d.aftermath) {
            checkMood(af.eff, where + ".aftermath");
            if (!afterWho.count(af.who)) rep.errors.push_back(where + ".aftermath: unknown who " + af.who);
        }
        bool anyOffer = d.offerAny.empty();
        for (const auto& o : d.offerAny) {
            if (o.rfind("tag:", 0) == 0) {
                for (const auto& [id, it] : c.items) anyOffer |= offeringMatches(c, o, id);
            } else anyOffer |= c.items.count(o) > 0;
        }
        if (!anyOffer) rep.errors.push_back(where + ": no offering is a known item");
        // §1.1: intimacy in a rite only between adults and only by consent
        if (d.intimacy.on) {
            if (!d.intimacy.adultsOnly) rep.errors.push_back(where + ": intimacy without adults_only (§1.1)");
            if (!d.intimacy.consent) rep.errors.push_back(where + ": intimacy without consent (§1.1)");
            if (d.intimacy.minLifeYears < 18) rep.errors.push_back(where + ": intimacy min_life_years under 18 (§1.1)");
            if (d.age != "adult") rep.errors.push_back(where + ": a rite with intimacy must be participants.age adult (§1.1)");
            if (d.intimacy.affinity <= 0.0 || d.intimacy.attraction <= 0.0)
                rep.errors.push_back(where + ": intimacy without a consent threshold (affinity, attraction) (§1.1)");
            if (!c.interaction(d.intimacy.act)) rep.errors.push_back(where + ": unknown intimacy act " + d.intimacy.act);
        }
    }
    // §1.1: every act of intimacy or courtship asks both sides to be no child
    std::set<std::string> adultActs;
    for (const auto& a : strs(r.rules, "adult_acts")) adultActs.insert(a);
    for (const auto& [id, d] : r.rites)
        if (d.intimacy.on) adultActs.insert(d.intimacy.act);
    for (const auto& [id, ia] : c.interactions) {
        if (!adultActs.count(id)) continue;
        bool self = false, other = false;
        for (const auto& k : ia.pre) {
            if (k.subject != "age") continue;
            const bool okSelf = (!k.negate && k.key == "adult") || (k.negate && k.key == "child");
            if (okSelf && !k.onTarget) self = true;
            if (okSelf && k.onTarget) other = true;
        }
        if (!self || (ia.targetsCharacter() && !other))
            rep.errors.push_back("interactions." + id + ": intimacy without an adult check on both sides (§1.1)");
    }
}

} // namespace lr::sim
