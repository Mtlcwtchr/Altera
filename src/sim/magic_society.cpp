// Magic in society: laws and witch trials, guilds, paid services, magic at
// work, the undead's following and wandering, magic quests in the realm (AL-118).
// See magic_society.hpp.

#include "sim/magic_society.hpp"
#include "sim/data_fs.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

#include "sim/content.hpp"
#include "sim/magic.hpp"
#include "sim/realm.hpp"
#include "sim/species.hpp"
#include "sim/world.hpp"
#include "sim/world_fields.hpp"

namespace lr::sim {

namespace fs = std::filesystem;

namespace {

constexpr const char* kState = "magic_society";
constexpr Tick kHour = kTicksPerDay / 24;
const std::vector<std::string> kClasses = {"magic", "witchcraft", "necromancy"}; // mild -> grave

Json readJson(const fs::path& p) {
    data::InFile f(p);
    if (!f) return Json();
    return Json::parse(f, nullptr, false);
}

double num(const Json& j, const char* k, double d) {
    return j.is_object() && j.contains(k) && j[k].is_number() ? j[k].get<double>() : d;
}

void loadRules(SocietyRules& r, const fs::path& file) {
    const Json j = readJson(file);
    if (!j.is_object()) return;
    r.loaded = true;
    const Json n = j.value("norms", Json::object());
    const Json lv = n.value("levels", Json::object());
    for (auto it = lv.begin(); it != lv.end(); ++it) r.levels[it.key()] = it.value().get<double>();
    r.normDefault = n.value("default", r.normDefault);
    const Json cbt = n.value("class_by_tag", Json::object());
    for (auto it = cbt.begin(); it != cbt.end(); ++it) r.classByTag[it.key()] = it.value().get<std::string>();
    const Json jt = n.value("judge_traits", Json::object());
    for (auto it = jt.begin(); it != jt.end(); ++it) r.judgeTraits[it.key()] = it.value().get<double>();
    const Json is = n.value("institution", Json::object());
    for (auto it = is.begin(); it != is.end(); ++it) r.institutionShift[it.key()] = it.value().get<double>();
    r.giftedJudge = num(n, "gifted_judge", r.giftedJudge);

    const Json t = j.value("trial", Json::object());
    const Json mc = t.value("memory_class", Json::object());
    for (auto it = mc.begin(); it != mc.end(); ++it) r.memoryClass[it.key()] = it.value().get<std::string>();
    const Json mw = t.value("memory_weight", Json::object());
    for (auto it = mw.begin(); it != mw.end(); ++it) r.memoryWeight[it.key()] = it.value().get<double>();
    r.accuseAt = num(t, "accuse_at", r.accuseAt);
    r.memoryDays = num(t, "memory_days", r.memoryDays);
    r.retrialDays = num(t, "retrial_days", r.retrialDays);
    r.fearMult = num(t, "fear_mult", r.fearMult);
    r.fleeAt = num(t, "flee_at", r.fleeAt);
    r.noise = num(t, "noise", r.noise);
    r.fineCoin = num(t, "fine_coin", r.fineCoin);
    r.exileM = num(t, "exile_m", r.exileM);
    for (const auto& o : t.value("options", Json::array())) {
        TrialOption op;
        op.id = o.value("id", "");
        op.base = num(o, "base", 0.0);
        for (const auto& tm : o.value("terms", Json::array()))
            op.terms.push_back({tm.value("kind", ""), tm.value("key", ""), num(tm, "add", 0.0), num(tm, "mul", 0.0)});
        r.options.push_back(op);
    }

    const Json g = j.value("guild", Json::object());
    r.guildMages = static_cast<int>(num(g, "mages", r.guildMages));
    r.guildKnowledge = num(g, "knowledge", r.guildKnowledge);
    if (g.contains("sizes")) r.guildSizes = g["sizes"].get<std::set<std::string>>();
    r.guildKm = num(g, "km", r.guildKm);
    r.guildHealFee = num(g, "heal_fee", r.guildHealFee);
    r.guildHealDaily = num(g, "heal_daily", r.guildHealDaily);

    const Json sv = j.value("service", Json::object());
    r.healFee = num(sv, "heal_fee", r.healFee);
    r.workFee = num(sv, "work_fee", r.workFee);

    const Json wk = j.value("work", Json::object());
    r.workDaily = num(wk, "daily", r.workDaily);
    r.manaReserve = num(wk, "mana_reserve", r.manaReserve);
    r.riskWeight = num(wk, "risk_weight", r.riskWeight);
    r.lawWeight = num(wk, "law_weight", r.lawWeight);
    r.costWeight = num(wk, "cost_weight", r.costWeight);
    r.workMin = num(wk, "min", r.workMin);
    r.workPriority = num(wk, "priority", r.workPriority);
    r.workTtl = static_cast<Tick>(num(wk, "ttl_ticks", static_cast<double>(r.workTtl)));
    for (const auto& s : wk.value("spells", Json::array())) {
        WorkSpell ws;
        ws.id = s.value("id", "");
        ws.object = s.value("object", "");
        ws.aspect = s.value("aspect", "");
        const std::string v = s.value("vector", "+");
        ws.vector = v.empty() ? '+' : v[0];
        ws.form = s.value("form", ws.form);
        ws.power = static_cast<int>(num(s, "power", 1));
        ws.when = s.value("when", "");
        ws.below = num(s, "below", 0.0);
        ws.value = num(s, "value", ws.value);
        r.work.push_back(ws);
    }

    const Json u = j.value("undead", Json::object());
    r.followM = num(u, "follow_m", r.followM);
    r.wanderM = num(u, "wander_m", r.wanderM);
    r.wanderSpeed = num(u, "wander_speed", r.wanderSpeed);
    r.biteM = num(u, "bite_m", r.biteM);
    r.biteHealth = num(u, "bite_health", r.biteHealth);
    r.biteFear = num(u, "bite_fear", r.biteFear);

    const Json rl = j.value("realm", Json::object());
    r.questEvery = static_cast<int>(num(rl, "quest_every_days", r.questEvery));
    r.necropolisShare = num(rl, "necropolis_share", r.necropolisShare);
}

Json& state(World& w) {
    Json& s = w.moduleState(kState);
    if (!s.is_object()) s = Json::object();
    for (const char* k : {"trials", "people", "guild_heal"})
        if (!s.contains(k)) s[k] = Json::object();
    return s;
}
const Json* stateC(const World& w) { return w.moduleState(kState); }
std::string idKey(EntityId id) { return std::to_string(id); }

int classRank(const std::string& cls) {
    auto it = std::find(kClasses.begin(), kClasses.end(), cls);
    return it == kClasses.end() ? 0 : static_cast<int>(it - kClasses.begin());
}

bool person(const Character& c) { return !c.isAnimal() && c.body.alive && !isUndead(c); }

const Settlement* settlementOf(const World& w, EntityId id) {
    auto it = w.settlements().find(id);
    return it == w.settlements().end() ? nullptr : &it->second;
}

// The one who rules or judges: the chief, else the eldest grown person living there.
const Character* rulerOf(const World& w, EntityId sid, EntityId except = kNone) {
    const Settlement* s = settlementOf(w, sid);
    if (!s) return nullptr;
    if (const Character* ch = w.person(s->chief); ch && ch->body.alive && ch->id != except) return ch;
    const Character* eldest = nullptr;
    for (const auto& [id, c] : w.characters())
        if (person(c) && c.status.settlement == sid && id != except && c.body.stage() != "child" &&
            (!eldest || c.body.ageDays > eldest->body.ageDays))
            eldest = &c;
    return eldest;
}

double levelOf(const SocietyRules& r, const std::string& name) {
    auto it = r.levels.find(name);
    return it == r.levels.end() ? 0.0 : it->second;
}

const CultureDef* cultureDef(const Content& ct, const std::string& id) {
    auto it = ct.peoples.cultures.find(id);
    return it == ct.peoples.cultures.end() ? nullptr : &it->second;
}

// The bend of a judge and an institution on every level.
double normShift(const World& w, EntityId sid) {
    const SocietyRules& r = societyRules(w.content());
    double shift = 0.0;
    if (const Settlement* s = settlementOf(w, sid))
        if (auto it = r.institutionShift.find(s->institution); it != r.institutionShift.end()) shift += it->second;
    if (const Character* j = rulerOf(w, sid)) {
        for (const auto& [t, v] : r.judgeTraits)
            if (j->hasTrait(t)) shift += v;
        if (isGifted(w, *j)) shift += r.giftedJudge;
    }
    return shift;
}

// How much the settlement minds one aspect: its class, or a crime when the culture forbids it.
double aspectNorm(const World& w, EntityId sid, const std::string& aspect) {
    const SocietyRules& r = societyRules(w.content());
    double base = settlementNorm(w, sid, magicClass(w.content(), aspect));
    if (const CultureDef* cd = cultureDef(w.content(), settlementCulture(w, sid)))
        if (std::find(cd->forbidAspects.begin(), cd->forbidAspects.end(), aspect) != cd->forbidAspects.end()) {
            const double crime = levelOf(r, "crime") + normShift(w, sid);
            base = std::max(base, std::clamp(crime, 0.0, 1.0));
        }
    return base;
}

void chron(World& w, const std::string& code, std::vector<EntityId> who, Json params) {
    w.chronicle("magic", std::move(who), "chronicle.magic." + code, std::move(params));
}

double bondOf(const Character& a, const Character& b) {
    double bond = 0.0;
    if (a.status.spouse == b.id || b.status.spouse == a.id) bond = 0.9;
    if (b.status.mother == a.id || b.status.father == a.id || a.status.mother == b.id || a.status.father == b.id)
        bond = std::max(bond, 0.85);
    if (a.status.family != kNone && a.status.family == b.status.family) bond = std::max(bond, 0.6);
    return bond;
}

void leaveSettlement(World& w, Character& c, double m) {
    Vec2 centre = c.pos;
    if (const Settlement* s = settlementOf(w, c.status.settlement)) centre = s->center;
    c.status.settlement = kNone;
    Vec2 away{c.pos.x - centre.x, c.pos.y - centre.y};
    const double len = std::hypot(away.x, away.y);
    away = len < 1e-6 ? Vec2{1.0, 0.0} : Vec2{away.x / len, away.y / len};
    c.pos = {std::clamp(centre.x + away.x * m, 0.0, w.map().sizeM), std::clamp(centre.y + away.y * m, 0.0, w.map().sizeM)};
    c.ai.queue.clear();
}

// The undead the person keeps (magic state "undead": master).
std::vector<Character*> servantsOf(World& w, EntityId master) {
    std::vector<Character*> out;
    for (auto& [id, c] : w.characters())
        if (c.body.alive && isUndead(c) && undeadMaster(w, c) == master) out.push_back(&c);
    return out;
}

Spell workCast(const WorkSpell& ws) {
    Spell s;
    s.parts.push_back({ws.aspect, ws.vector});
    s.form = ws.form;
    if (ws.power > 1) s.mods["power"] = ws.power;
    return s;
}

// The need of a work spell at an object 0..1.
double workNeed(const World& w, const WorkSpell& ws, const WorldObject& o) {
    auto st = [&](const char* k, int d) {
        auto it = o.stock.find(k);
        return it == o.stock.end() ? d : it->second;
    };
    if (ws.when == "hungry_field") {
        if (st("sown", 0) <= 0) return 0.0;
        const double f = st("fertility", 700);
        return f < ws.below ? std::clamp((ws.below - f) / std::max(1.0, ws.below) * 2.5, 0.0, 1.0) : 0.0;
    }
    if (ws.when == "dry_field") {
        if (st("sown", 0) <= 0) return 0.0;
        const double cap = std::max(1, st("capacity", 100));
        const double m = st("moisture", static_cast<int>(cap * 0.7));
        const double lim = ws.below * cap;
        return m < lim ? std::clamp(1.0 - m / std::max(1.0, lim), 0.0, 1.0) : 0.0;
    }
    if (ws.when == "cracked") {
        const int mx = objectIntegrityMax(w, o);
        const double k = mx > 0 ? static_cast<double>(std::max(0, objectIntegrity(w, o))) / mx : integrityAt(w, o.pos);
        return k < ws.below ? std::clamp((ws.below - k) / std::max(0.01, ws.below), 0.0, 1.0) : 0.0;
    }
    if (ws.when == "tainted") {
        const double t = fieldAt(w, Field::taint, o.pos);
        return t > ws.below ? std::clamp((t - ws.below) / std::max(0.01, 1.0 - ws.below) * 2.0, 0.0, 1.0) : 0.0;
    }
    return 0.0;
}

} // namespace

// --- content ---------------------------------------------------------------------------

const WorkSpell* SocietyRules::workSpell(const std::string& id) const {
    for (const auto& ws : work)
        if (ws.id == id) return &ws;
    return nullptr;
}

const SocietyRules& societyRules(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<SocietyRules>> cache;
    static const SocietyRules kNoRules;
    if (c.dir.empty()) return kNoRules;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(data::cacheKey(c.dir));
    if (it != cache.end()) return *it->second;
    auto r = std::make_unique<SocietyRules>();
    loadRules(*r, fs::path(c.dir).parent_path() / "magic" / "society.json");
    return *cache.emplace(data::cacheKey(c.dir), std::move(r)).first->second;
}

std::vector<std::string> checkSociety(const Content& c) {
    std::vector<std::string> err;
    const SocietyRules& r = societyRules(c);
    if (!r.loaded) return {"society.json: not loaded"};
    const MagicRules& m = magicRules(c);
    for (const auto& [tag, cls] : r.classByTag)
        if (classRank(cls) == 0 && cls != "magic") err.push_back("society.json: class_by_tag." + tag + " unknown class " + cls);
    for (const auto& [t, v] : r.judgeTraits)
        if (!c.traits.count(t)) err.push_back("society.json: judge_traits '" + t + "' is not a trait");
    for (const auto& [mem, cls] : r.memoryClass) {
        if (classRank(cls) == 0 && cls != "magic") err.push_back("society.json: memory_class." + mem + " unknown class");
        if (!r.memoryWeight.count(mem)) err.push_back("society.json: memory_weight has no '" + mem + "'");
    }
    std::set<std::string> ids;
    for (const auto& o : r.options) {
        ids.insert(o.id);
        for (const auto& t : o.terms)
            if (t.kind == "trait" && !c.traits.count(t.key)) err.push_back("society.json: trial " + o.id + " trait '" + t.key + "' unknown");
    }
    for (const char* need : {"tolerate", "fine", "shame", "exile", "burn"})
        if (!ids.count(need)) err.push_back(std::string("society.json: trial option '") + need + "' is missing");
    for (const auto& ws : r.work) {
        if (!c.interaction(ws.id)) err.push_back("society.json: work spell '" + ws.id + "' is not a catalog act");
        if (!c.objects.count(ws.object)) err.push_back("society.json: work spell '" + ws.id + "' object '" + ws.object + "' unknown");
        if (m.loaded && !m.aspect(ws.aspect)) err.push_back("society.json: work spell '" + ws.id + "' aspect '" + ws.aspect + "' unknown");
        if (m.loaded && !m.form(ws.form)) err.push_back("society.json: work spell '" + ws.id + "' form '" + ws.form + "' unknown");
        if (ws.when != "hungry_field" && ws.when != "dry_field" && ws.when != "cracked" && ws.when != "tainted")
            err.push_back("society.json: work spell '" + ws.id + "' when '" + ws.when + "' unknown");
    }
    for (const auto& [id, cd] : c.peoples.cultures)
        for (auto it = cd.norms.begin(); it != cd.norms.end(); ++it)
            if (classRank(it.key()) > 0 || it.key() == "magic")
                if (!it.value().is_string() || !r.levels.count(it.value().get<std::string>()))
                    err.push_back("culture " + id + ": norm '" + it.key() + "' has an unknown level");
    return err;
}

std::vector<std::string> societyLocKeys(const Content& c) {
    std::vector<std::string> k;
    const SocietyRules& r = societyRules(c);
    for (const auto& o : r.options) k.push_back("chronicle.magic.trial." + o.id);
    for (const char* s : {"ruler", "fled"}) k.push_back(std::string("chronicle.magic.trial.") + s);
    for (const char* s : {"guild_founded", "guild_heal", "necropolis", "paid"})
        k.push_back(std::string("chronicle.magic.society.") + s);
    for (const auto& ws : r.work) k.push_back("interaction." + ws.id + ".name");
    for (const char* q : {"haunting", "necromancer_hunt", "dragon_slay", "artifact_hunt"})
        k.push_back(std::string("realm.quest.") + q + ".name");
    k.push_back("cause.burned_as_witch.name");
    return k;
}

// --- norms -------------------------------------------------------------------------------

std::string magicClass(const Content& c, const std::string& aspect) {
    const SocietyRules& r = societyRules(c);
    const AspectDef* a = magicRules(c).aspect(aspect);
    std::string best = "magic";
    if (!a) return best;
    for (const auto& t : a->tags)
        if (auto it = r.classByTag.find(t); it != r.classByTag.end() && classRank(it->second) > classRank(best))
            best = it->second;
    return best;
}

std::string settlementCulture(const World& w, EntityId sid) {
    // AL-136: the settlement's own culture (worldgen / realm templates), else its ruler's kind's
    if (const auto st = w.settlements().find(sid); st != w.settlements().end() && !st->second.culture.empty())
        return st->second.culture;
    const Character* ruler = rulerOf(w, sid);
    if (!ruler) return "";
    const RaceDef& rd = raceOf(w.content(), *ruler);
    return rd.cultureDefault;
}

double settlementNorm(const World& w, EntityId sid, const std::string& cls) {
    const SocietyRules& r = societyRules(w.content());
    if (!r.loaded || sid == kNone) return 0.0;
    std::string name = r.normDefault;
    if (const CultureDef* cd = cultureDef(w.content(), settlementCulture(w, sid)))
        if (cd->norms.is_object() && cd->norms.contains(cls) && cd->norms[cls].is_string()) name = cd->norms[cls].get<std::string>();
    const double base = levelOf(r, name);
    // a bend works on what is minded at all; a settlement that allows a kind of magic does not start to mind it
    if (base <= 0.0) return 0.0;
    return std::clamp(base + normShift(w, sid), 0.0, 1.0);
}

// --- witch trials ----------------------------------------------------------------------------

namespace {

struct Evidence {
    double weight = 0.0;
    double norm = 0.0; // the gravest norm broken that was seen
    std::string cls = "magic";
    int witnesses = 0;
};

Evidence evidenceOf(const World& w, const Character& caster) {
    const SocietyRules& r = societyRules(w.content());
    Evidence e;
    const EntityId sid = caster.status.settlement;
    if (!r.loaded || sid == kNone) return e;
    const Tick since = w.now() - static_cast<Tick>(r.memoryDays * kTicksPerDay);
    for (const auto& [id, o] : w.characters()) {
        if (!person(o) || id == caster.id || o.status.settlement != sid) continue;
        double mine = 0.0;
        for (const auto& m : o.memory) {
            if (m.who.empty() || m.who.front() != caster.id || m.tick < since) continue;
            auto mc = r.memoryClass.find(m.type);
            if (mc == r.memoryClass.end()) continue;
            std::string aspect;
            for (const auto& [g, n] : m.goods)
                if (g.rfind("aspect:", 0) == 0) aspect = g.substr(7);
            const double lvl = aspect.empty() ? settlementNorm(w, sid, mc->second) : aspectNorm(w, sid, aspect);
            if (lvl <= 0.0) continue;
            const std::string cls = aspect.empty() ? mc->second : magicClass(w.content(), aspect);
            if (lvl > e.norm || (lvl == e.norm && classRank(cls) > classRank(e.cls))) e.norm = lvl, e.cls = cls;
            double fear = 0.0;
            if (auto rel = o.relations.find(caster.id); rel != o.relations.end()) fear = rel->second.fear;
            auto wt = r.memoryWeight.find(m.type);
            mine += (wt == r.memoryWeight.end() ? 0.3 : wt->second) * m.confidence * lvl * (1.0 + r.fearMult * fear);
        }
        if (mine > 0.0) {
            ++e.witnesses;
            e.weight += std::min(1.0, mine);
        }
    }
    return e;
}

int lastTrial(const World& w, EntityId id) {
    const Json* s = stateC(w);
    if (!s || !s->contains("trials") || !(*s)["trials"].contains(idKey(id))) return -100000;
    return (*s)["trials"][idKey(id)].value("day", -100000);
}

double usefulness(const World& w, const Character& c) {
    const Json* s = stateC(w);
    double u = 0.0;
    if (s && s->contains("people") && (*s)["people"].contains(idKey(c.id))) {
        const Json& p = (*s)["people"][idKey(c.id)];
        u += 0.15 * p.value("heals", 0) + 0.08 * p.value("work", 0);
    }
    const MagicRules& m = magicRules(w.content());
    if (aspectKnowledge(w, c, m.healAspect) >= m.healMinK) u += 0.3;
    return std::clamp(u, 0.0, 1.0);
}

double scoreOption(const TrialOption& o, const Character& judge, const Character& accused, const World& w,
                   const Evidence& e) {
    double s = o.base;
    const double kin = bondOf(judge, accused) >= 0.6 ? 1.0 : 0.0;
    double aff = 0.0;
    if (auto it = judge.relations.find(accused.id); it != judge.relations.end()) aff = it->second.affinity;
    const MagicRules& mr = magicRules(w.content());
    const bool giftedJudge = isGifted(w, judge) || aspectKnowledge(w, judge, mr.healAspect) > 0.0;
    for (const auto& t : o.terms) {
        if (t.kind == "norm") s += t.mul * e.norm + t.add;
        else if (t.kind == "evidence") s += t.mul * e.weight + t.add;
        else if (t.kind == "useful") s += t.mul * usefulness(w, accused) + t.add;
        else if (t.kind == "affinity") s += t.mul * aff + t.add;
        else if (t.kind == "kin") s += kin * t.add;
        else if (t.kind == "trait") s += judge.hasTrait(t.key) ? t.add : 0.0;
        else if (t.kind == "gifted_judge") s += giftedJudge ? t.add : 0.0;
        else if (t.kind == "dark") s += e.cls != "magic" ? t.add : 0.0;
    }
    return s;
}

const TrialOption* verdictFor(World& w, const Character& judge, const Character& accused, const Evidence& e, Rng& rng,
                              std::map<std::string, double>* scores = nullptr) {
    const SocietyRules& r = societyRules(w.content());
    const TrialOption* best = nullptr;
    double bs = -1e18;
    for (const auto& o : r.options) {
        const double s = scoreOption(o, judge, accused, w, e) + r.noise * (2.0 * rng.uniform() - 1.0);
        if (scores) (*scores)[o.id] = s;
        if (s > bs) bs = s, best = &o;
    }
    return best;
}

} // namespace

double accusationWeight(const World& w, const Character& caster) { return evidenceOf(w, caster).weight; }

std::string holdTrial(World& w, Character& accused) {
    const SocietyRules& r = societyRules(w.content());
    if (!r.loaded) return "";
    const EntityId sid = accused.status.settlement;
    Json& s = state(w);
    const Settlement* st = settlementOf(w, sid);
    if (!st) return "";
    const Evidence e = evidenceOf(w, accused);
    if (st->chief == accused.id) {
        // the one who would judge is the mage: people whisper, nobody judges
        s["trials"][idKey(accused.id)] = {{"day", w.day()}, {"verdict", "ruler"}, {"class", e.cls}};
        chron(w, "trial.ruler", {accused.id}, {{"who", accused.id}});
        return "ruler";
    }
    const Character* jc = rulerOf(w, sid, accused.id);
    Character* judge = jc ? w.character(jc->id) : nullptr;
    if (!judge) return "";
    Rng rng = w.rng(accused.id, "magic_trial");
    const TrialOption* o = verdictFor(w, *judge, accused, e, rng);
    std::string v = o ? o->id : "tolerate";
    s["trials"][idKey(accused.id)] = {{"day", w.day()}, {"verdict", v}, {"class", e.cls}, {"judge", judge->id},
                                      {"settlement", sid}};
    const Json params = {{"who", accused.id}, {"judge", judge->id}, {"class", e.cls}};
    if (v == "fine") {
        const double paid = payForMagic(w, accused, *judge, r.fineCoin);
        if (paid <= 0.0) v = "shame";
        accused.standing.honor = std::max(0.0, accused.standing.honor - 0.02);
    }
    if (v == "shame") {
        accused.standing.honor = std::max(0.0, accused.standing.honor - 0.08);
        for (auto& [id, o2] : w.characters())
            if (person(o2) && o2.status.settlement == sid && id != accused.id)
                o2.relations[accused.id].trust = std::max(-1.0, o2.relations[accused.id].trust - 0.1);
    }
    s["trials"][idKey(accused.id)]["verdict"] = v;
    chron(w, "trial." + v, {accused.id, judge->id}, params);
    if (v == "exile") {
        std::vector<Character*> keep = servantsOf(w, accused.id);
        leaveSettlement(w, accused, r.exileM);
        for (Character* u : keep) u->pos = accused.pos, u->status.settlement = kNone;
        accused.relations[judge->id].enmity = std::clamp(accused.relations[judge->id].enmity + 0.5, 0.0, 1.0);
    } else if (v == "burn") {
        accused.body.alive = false;
        accused.body.health = 0.0;
        accused.body.causeOfDeath = "burned_as_witch";
        fieldAdd(w, Field::heat, accused.pos, 2.0, 80.0);
        // the master's death leaves the undead unbound (magic undeadDaily)
    } else if (v == "tolerate") {
        // a settled matter: the fear of the neighbours eases a little
        for (auto& [id, o2] : w.characters())
            if (person(o2) && o2.status.settlement == sid && id != accused.id)
                o2.relations[accused.id].fear = std::max(0.0, o2.relations[accused.id].fear - 0.05);
    }
    return v;
}

// --- services ------------------------------------------------------------------------------

double payForMagic(World& w, Character& payer, Character& to, double fee) {
    if (fee <= 0.0 || payer.id == to.id) return 0.0;
    double left = fee;
    auto take = [&](std::map<std::string, int>& stock, const std::string& item, double rate) {
        auto it = stock.find(item);
        if (it == stock.end() || it->second <= 0 || left <= 1e-9) return 0;
        const int n = std::min(it->second, static_cast<int>(std::ceil(left / rate)));
        it->second -= n;
        left -= n * rate;
        return n;
    };
    std::map<std::string, int> got;
    got["coin"] += take(payer.inventory, "coin", 1.0);
    WorldObject* store = nullptr;
    if (auto f = w.families().find(payer.status.family); f != w.families().end()) store = w.object(f->second.storage);
    if (store) {
        got["coin"] += take(store->stock, "coin", 1.0);
        got["grain"] += take(store->stock, "grain", 0.5);
    }
    WorldObject* into = nullptr;
    if (auto f = w.families().find(to.status.family); f != w.families().end()) into = w.object(f->second.storage);
    for (const auto& [item, n] : got) {
        if (n <= 0) continue;
        if (item == "coin" || !into) to.inventory[item] += n;
        else into->stock[item] += n;
    }
    const double paid = fee - std::max(0.0, left);
    if (paid > 0.0) {
        to.relations[payer.id].affinity = std::clamp(to.relations[payer.id].affinity + 0.03, -1.0, 1.0);
        payer.relations[to.id].respect = std::clamp(payer.relations[to.id].respect + 0.03, -1.0, 1.0);
    }
    return paid;
}

namespace {

void countService(World& w, const Character& c, const char* what) {
    Json& p = state(w)["people"][idKey(c.id)];
    if (!p.is_object()) p = Json::object();
    p[what] = p.value(what, 0) + 1;
}

// A household's head (or any grown member) to pay for what was done for it.
Character* payerOf(World& w, EntityId family) {
    auto f = w.families().find(family);
    if (f == w.families().end()) return nullptr;
    Character* best = nullptr;
    for (EntityId id : f->second.members)
        if (Character* c = w.character(id); c && person(*c) && c->body.stage() != "child")
            if (!best || c->status.role == "head") best = c;
    return best;
}

} // namespace

double workSpellValue(const World& w, const Character& c, const WorkSpell& ws, const WorldObject& o) {
    const SocietyRules& r = societyRules(w.content());
    const MagicRules& m = magicRules(w.content());
    if (!m.loaded || !r.loaded) return 0.0;
    const double need = workNeed(w, ws, o);
    if (need <= 0.0) return 0.0;
    if (aspectKnowledge(w, c, ws.aspect) < m.useMin) return 0.0;
    const SpellPreview pv = previewSpell(w, c, workCast(ws));
    if (!pv.ok) return 0.0;
    const double mx = std::max(0.1, manaMax(w, c));
    if (pv.manaNow - pv.cost < r.manaReserve * mx) return 0.0; // keeps a reserve for what may come
    double risk = 0.0;
    for (const auto& t : m.tiers)
        if (t.backfire || t.power <= 0.0)
            if (auto it = pv.tierChance.find(t.id); it != pv.tierChance.end()) risk += it->second * (t.backfire ? 1.0 : 0.4);
    const double law = aspectNorm(w, c.status.settlement, ws.aspect);
    double v = ws.value * need - r.costWeight * pv.cost / mx - r.riskWeight * risk - r.lawWeight * law;
    if (o.ownerFamily != kNone && o.ownerFamily != c.status.family) v *= 0.6; // another's work, for a fee
    return v;
}

void magicAfterAct(World& w, Character& c, const InteractionDef& d, WorldObject* target, Character*, bool success) {
    const SocietyRules& r = societyRules(w.content());
    if (!r.loaded || !success || !target) return;
    const WorkSpell* ws = r.workSpell(d.id);
    if (!ws) return;
    Spell s = workCast(*ws);
    // under a law against it a careful mage keeps quiet
    if (aspectNorm(w, c.status.settlement, ws->aspect) >= 0.5 && magicRules(w.content()).modifiers.count("silent"))
        s.mods["silent"] = 1;
    CastTarget t;
    t.hasPos = true;
    t.at = target->pos;
    const CastOutcome o = castSpell(w, c, s, t);
    if (!o.cast || o.backfire || o.power <= 0.0) return;
    countService(w, c, "work");
    if (target->ownerFamily != kNone && target->ownerFamily != c.status.family)
        if (Character* payer = payerOf(w, target->ownerFamily)) {
            const double paid = payForMagic(w, *payer, c, r.workFee);
            if (paid > 0.0)
                chron(w, "society.paid", {c.id, payer->id}, {{"a", c.id}, {"b", payer->id}, {"n", static_cast<int>(std::round(paid))}});
        }
}

namespace {

void workDaily(World& w) {
    const SocietyRules& r = societyRules(w.content());
    if (r.work.empty()) return;
    for (auto& [id, c] : w.characters()) {
        if (!person(c) || c.controller.isPlayer() || c.body.stage() == "child" || c.status.settlement == kNone) continue;
        if (c.captivity.held()) continue;
        Rng rng = w.rng(id, "magic_work");
        if (!rng.chance(r.workDaily)) continue;
        bool any = false;
        for (const auto& ws : r.work) any |= aspectKnowledge(w, c, ws.aspect) >= magicRules(w.content()).useMin;
        if (!any) continue;
        const WorkSpell* best = nullptr;
        EntityId at = kNone;
        double bv = r.workMin;
        for (const auto& ws : r.work) {
            if (!w.content().interaction(ws.id)) continue;
            for (EntityId oid : w.objectsOfType(ws.object)) {
                const WorldObject* o = w.object(oid);
                if (!o) continue;
                // the household's own, or another's of the same settlement
                if (o->ownerFamily != kNone && o->ownerFamily != c.status.family) {
                    auto f = w.families().find(o->ownerFamily);
                    if (f == w.families().end() || f->second.settlement != c.status.settlement) continue;
                }
                if (o->ownerFamily == kNone && dist(o->pos, c.pos) > 600.0) continue;
                const double v = workSpellValue(w, c, ws, *o);
                if (v > bv) bv = v, best = &ws, at = oid;
            }
        }
        if (!best) continue;
        bool queued = false;
        for (const auto& q : c.ai.queue) queued |= q.task == best->id;
        if (!queued)
            c.ai.queue.push_back(QueuedTask{best->id, at, r.workPriority, w.now(), r.workTtl, "magic.work", "magic"});
    }
}

// Trials: who has been seen at forbidden or feared magic enough is brought to judgement.
void trialsDaily(World& w) {
    const SocietyRules& r = societyRules(w.content());
    std::vector<EntityId> accused;
    for (const auto& [id, c] : w.characters()) {
        if (!person(c) || c.status.settlement == kNone || c.controller.isPlayer()) continue;
        if (w.day() - lastTrial(w, id) < r.retrialDays) continue;
        if (accusationWeight(w, c) >= r.accuseAt) accused.push_back(id);
    }
    for (EntityId id : accused) {
        Character* c = w.character(id);
        if (!c || !c->body.alive) continue;
        const Settlement* st = settlementOf(w, c->status.settlement);
        // one who fears the stake may run before the trial
        if (st && st->chief != id) {
            const Character* jc = rulerOf(w, c->status.settlement, id);
            if (jc) {
                const Evidence e = evidenceOf(w, *c);
                Rng look = w.rng(id, "magic_trial_fear");
                std::map<std::string, double> sc;
                verdictFor(w, *jc, *c, e, look, &sc);
                double top = -1e18;
                for (const auto& [k, v] : sc) top = std::max(top, v);
                const double dread = std::max(sc["burn"], sc["exile"]) >= top - 0.05 ? 1.0 : 0.0;
                Rng rng = w.rng(id, "magic_flee");
                if (dread > 0.0 && rng.chance(r.fleeAt) && !c->hasTrait("stubborn")) {
                    Json& s = state(w);
                    s["trials"][idKey(id)] = {{"day", w.day()}, {"verdict", "fled"}, {"class", e.cls},
                                              {"settlement", c->status.settlement}};
                    chron(w, "trial.fled", {id}, {{"who", id}, {"class", e.cls}});
                    std::vector<Character*> keep = servantsOf(w, id);
                    leaveSettlement(w, *c, r.exileM);
                    for (Character* u : keep) u->pos = c->pos, u->status.settlement = kNone;
                    continue;
                }
            }
        }
        holdTrial(w, *c);
    }
}

// Those who see the walking dead about their master know whose they are.
void undeadWitnessDaily(World& w) {
    const MagicRules& mr = magicRules(w.content());
    for (auto& [uid, u] : w.characters()) {
        if (!u.body.alive || !isUndead(u)) continue;
        const EntityId master = undeadMaster(w, u);
        if (master == kNone) continue;
        const Character* mc = w.character(master);
        if (!mc || !mc->body.alive || dist(mc->pos, u.pos) > 30.0) continue;
        for (auto& [id, o] : w.characters()) {
            if (!person(o) || id == master || dist(o.pos, u.pos) > mr.undeadWitnessM) continue;
            bool knows = false;
            for (const auto& m : o.memory)
                knows |= m.type == "saw_necromancy" && !m.who.empty() && m.who.front() == master &&
                         w.now() - m.tick < kTicksPerDay * 10;
            if (knows) continue;
            MemoryEntry m{0, "saw_necromancy", {master, uid}, kNone, -0.6, 0.0, "seen", 1.0};
            m.goods["aspect:" + mr.deathAspect] = 1;
            w.remember(o, std::move(m));
        }
    }
}

// The guilds: where mages gather, or a town that does not mind magic.
void guildsDaily(World& w) {
    const SocietyRules& r = societyRules(w.content());
    const MagicRules& m = magicRules(w.content());
    for (auto& [sid, p] : w.settlements()) {
        if ((w.day() + static_cast<int>(sid)) % 30 != 0 || p.ruined) continue;
        bool guild = false;
        if (p.full()) {
            int mages = 0;
            for (const auto& [id, c] : w.characters()) {
                if (!person(c) || c.status.settlement != sid) continue;
                const MagicSelf ms = magicOf(w, c);
                for (const auto& [a, k] : ms.aspects)
                    if (k >= r.guildKnowledge && m.aspect(a) && !m.aspect(a)->hasTag("forbidden")) {
                        ++mages;
                        break;
                    }
            }
            guild = mages >= r.guildMages;
        } else {
            guild = r.guildSizes.count(p.size) && settlementNorm(w, sid, "magic") < 0.5;
        }
        const bool had = p.tagged("mage_guild");
        if (guild) p.tags.insert("mage_guild");
        else p.tags.erase("mage_guild");
        if (guild && !had) chron(w, "society.guild_founded", {}, {{"pol", sid}});
    }
}

// A guild near the village sends a healer to those who look for one and have none at home.
void guildHealDaily(World& w) {
    const SocietyRules& r = societyRules(w.content());
    const MagicRules& m = magicRules(w.content());
    const Json* ms = static_cast<const World&>(w).moduleState("magic");
    if (!ms || !ms->contains("coping")) return;
    std::vector<std::pair<EntityId, EntityId>> asks; // seeker, patient
    for (auto it = (*ms)["coping"].begin(); it != (*ms)["coping"].end(); ++it) {
        if (it.value().value("stage", "") != "seek_healer") continue;
        const EntityId seeker = static_cast<EntityId>(std::stoul(it.key()));
        const EntityId patient = it.value().value("trouble", "") == "maimed" ? seeker : it.value().value("about", kNone);
        asks.emplace_back(seeker, patient);
    }
    for (const auto& [sk, pt] : asks) {
        Character* seeker = w.character(sk);
        Character* patient = w.character(pt);
        if (!seeker || !patient || !patient->body.alive) continue;
        const Settlement* home = settlementOf(w, seeker->status.settlement);
        if (!home) continue;
        bool local = false;
        for (const auto& [id, c] : w.characters())
            local |= person(c) && c.status.settlement == home->id && aspectKnowledge(w, c, m.healAspect) >= m.healMinK;
        if (local) continue;
        const Settlement* guild = nullptr;
        for (const auto& [sid, p] : w.settlements())
            if (p.tagged("mage_guild") && !p.ruined && std::hypot(p.km.x - home->km.x, p.km.y - home->km.y) <= r.guildKm)
                guild = &p;
        if (!guild) continue;
        Json& gh = state(w)["guild_heal"];
        if (gh.value(idKey(pt), -1) == w.day()) continue;
        Rng rng = w.rng(pt, "magic_guild_heal");
        if (!rng.chance(r.guildHealDaily)) continue;
        // the fee goes to the guild (off the map); what the household can give
        Character* payer = seeker;
        double left = r.guildHealFee;
        auto take = [&](std::map<std::string, int>& st, const std::string& item, double rate) {
            auto i = st.find(item);
            if (i == st.end() || i->second <= 0 || left <= 0) return;
            const int n = std::min(i->second, static_cast<int>(std::ceil(left / rate)));
            i->second -= n;
            left -= n * rate;
        };
        take(payer->inventory, "coin", 1.0);
        if (auto f = w.families().find(payer->status.family); f != w.families().end())
            if (WorldObject* o = w.object(f->second.storage)) take(o->stock, "coin", 1.0), take(o->stock, "grain", 0.5);
        if (left >= r.guildHealFee) continue; // nothing to pay with
        gh[idKey(pt)] = w.day();
        applyAspectsTo(w, *patient, {{m.healAspect, '+', 1.0}}, kNone);
        chron(w, "society.guild_heal", {sk, pt}, {{"a", sk}, {"b", pt}, {"pol", guild->id}});
    }
}

} // namespace

// --- the undead ---------------------------------------------------------------------------------

bool undeadHeld(const World& w, const Character& c) {
    if (!c.body.alive || !isUndead(c) || c.controller.isPlayer()) return false;
    return c.hasTrait(magicRules(w.content()).mindlessTrait) && societyRules(w.content()).loaded;
}

void undeadTick(World& w) {
    const SocietyRules& r = societyRules(w.content());
    if (!r.loaded) return;
    const double walk = w.content().config.walkMPerTick;
    for (auto& [id, u] : w.characters()) {
        if (!undeadHeld(w, u) || u.captivity.held()) continue;
        const EntityId mid = undeadMaster(w, u);
        Character* master = mid == kNone ? nullptr : w.character(mid);
        Vec2 to = u.pos;
        double keep = r.followM, speed = walk;
        Character* prey = nullptr;
        if (master && master->body.alive) {
            to = master->pos;
            u.status.settlement = master->status.settlement;
        } else {
            // unbound: to the nearest of the living
            double best = r.wanderM;
            for (auto& [oid, o] : w.characters())
                if (person(o) && dist(o.pos, u.pos) < best) best = dist(o.pos, u.pos), prey = &o;
            if (!prey) {
                u.ai.fsm.state = "Idle";
                continue;
            }
            to = prey->pos;
            keep = r.biteM * 0.8;
            speed = walk * r.wanderSpeed;
        }
        const double dd = dist(u.pos, to);
        if (dd > keep) {
            const double go = std::min(dd - keep, speed);
            u.pos.x += (to.x - u.pos.x) * go / dd;
            u.pos.y += (to.y - u.pos.y) * go / dd;
            u.ai.fsm.state = "Move";
            u.ai.fsm.dest = to;
        } else {
            u.ai.fsm.state = "Idle";
        }
        if (prey && dist(u.pos, prey->pos) <= r.biteM && w.now() % kHour == 0) {
            prey->body.health = std::max(0.0, prey->body.health - r.biteHealth);
            prey->relations[u.id].fear = std::clamp(prey->relations[u.id].fear + r.biteFear, 0.0, 1.0);
            bool fresh = false;
            for (const auto& m : prey->memory)
                fresh |= m.type == "attacked_by_undead" && !m.who.empty() && m.who.front() == u.id && w.now() - m.tick < kTicksPerDay;
            if (!fresh) w.remember(*prey, MemoryEntry{0, "attacked_by_undead", {u.id}, kNone, -0.6, 0.0, "seen", 1.0});
            if (prey->body.health <= 0.0) {
                prey->body.alive = false;
                prey->body.causeOfDeath = "undead";
            }
        }
    }
}

// --- the realm ---------------------------------------------------------------------------------

namespace {

void realmMagic(World& w, RealmState& rs) {
    const SocietyRules& r = societyRules(w.content());
    if (!r.loaded || r.questEvery <= 0) return;
    const int day = w.day();
    for (auto& [sid, p] : w.settlements()) {
        if (p.ruined || (day + static_cast<int>(sid)) % r.questEvery != 0) continue;
        auto open = [&](const std::string& kind, EntityId target) {
            for (const auto& q : rs.quests)
                if (q.status == "open" && q.kind == kind && q.polity == sid && (target == kNone || q.target == target)) return true;
            return false;
        };
        if (p.full()) {
            // the walking dead about the village, without a master or with a forbidden one
            int living = 0, dead = 0;
            Character* worst = nullptr;
            for (auto& [id, c] : w.characters()) {
                if (c.isAnimal() || !c.body.alive) continue;
                const bool near = dist(c.pos, p.center) < 800.0;
                if (!isUndead(c)) {
                    living += c.status.settlement == sid;
                    continue;
                }
                if (!near && c.status.settlement != sid) continue;
                ++dead;
                const EntityId master = undeadMaster(w, c);
                const bool forbidden = master == kNone || settlementNorm(w, sid, "necromancy") >= 0.5;
                if (forbidden && !worst) worst = &c;
            }
            const bool necro = living > 0 && dead >= std::max(1.0, r.necropolisShare * living);
            if (necro && !p.tagged("necropolis")) chron(w, "society.necropolis", {}, {{"pol", sid}});
            if (necro) p.tags.insert("necropolis");
            else p.tags.erase("necropolis");
            if (worst && !open("haunting", kNone) && realmQuestOn(w.content(), "haunting"))
                realmPostQuest(w, rs, "haunting", sid, p.chief, worst->id, -1.0);
        }
        // those who fled or were driven out for necromancy are hunted by those who judged them
        if (const Json* s = stateC(w); s && s->contains("trials") && realmQuestOn(w.content(), "necromancer_hunt"))
            for (auto it = (*s)["trials"].begin(); it != (*s)["trials"].end(); ++it) {
                const Json& t = it.value();
                if (t.value("settlement", kNone) != sid || t.value("class", "") != "necromancy") continue;
                const std::string v = t.value("verdict", "");
                if (v != "fled" && v != "exile") continue;
                if (day - t.value("day", 0) > 360) continue;
                const EntityId who = static_cast<EntityId>(std::stoul(it.key()));
                const Character* c = w.person(who);
                if (!c || !c->body.alive || open("necromancer_hunt", who)) continue;
                bool posted = false;
                for (const auto& q : rs.quests) posted |= q.kind == "necromancer_hunt" && q.target == who;
                if (!posted) realmPostQuest(w, rs, "necromancer_hunt", sid, p.chief, who, -1.0);
            }
        // a dragon that takes tribute or raids is hunted for its hoard
        if (!p.tagged("dragon_lair") && realmQuestOn(w.content(), "dragon_slay")) {
            EntityId lair = kNone;
            if (const Settlement* t = settlementOf(w, p.tributeTo); t && t->tagged("dragon_lair") && !t->ruined) lair = t->id;
            if (lair == kNone)
                for (const auto& [k, view] : rs.views) {
                    const auto gt = k.find('>');
                    if (gt == std::string::npos || k.substr(0, gt) != std::to_string(sid)) continue;
                    const EntityId other = static_cast<EntityId>(std::stoul(k.substr(gt + 1)));
                    const Settlement* t = settlementOf(w, other);
                    if (!t || !t->tagged("dragon_lair") || t->ruined) continue;
                    for (const auto& g : view.grievances)
                        if (g.cause == "raid" && day - g.day < 360) lair = other;
                }
            if (lair != kNone && !open("dragon_slay", lair)) realmPostQuest(w, rs, "dragon_slay", sid, p.chief, lair, -1.0);
        }
        // a guild wants things of power found
        if (p.tagged("mage_guild") && realmQuestOn(w.content(), "artifact_hunt") && !open("artifact_hunt", kNone)) {
            Rng rng = w.rng(sid, "magic_artifact_hunt");
            if (rng.chance(0.08)) realmPostQuest(w, rs, "artifact_hunt", sid, p.chief, kNone, -1.0);
        }
    }
}

bool societyCommand(World& w, Command& cmd) {
    const std::string& t = cmd.type;
    if (t != "witch_trial" && t != "work_spell") return false;
    Character* c = cmd.args.contains("id") ? w.character(cmd.args.at("id").get<EntityId>()) : nullptr;
    if (!c || !c->body.alive) {
        cmd.result = "rejected:no_such_character";
        return true;
    }
    if (t == "witch_trial") {
        const std::string v = holdTrial(w, *c);
        cmd.result = v.empty() ? "rejected:no_judge" : "ok:" + v;
    } else {
        const SocietyRules& r = societyRules(w.content());
        const WorkSpell* ws = r.workSpell(cmd.args.value("spell", ""));
        const WorldObject* o = w.object(cmd.args.value("at", kNone));
        if (!ws || !o) {
            cmd.result = "rejected:no_work";
            return true;
        }
        c->ai.queue.push_back(QueuedTask{ws->id, o->id, r.workPriority + 0.3, w.now(), r.workTtl, "magic.work", "magic"});
        cmd.result = "ok";
    }
    return true;
}

} // namespace

void societyDaily(World& w) {
    const SocietyRules& r = societyRules(w.content());
    if (!r.loaded || !magicRules(w.content()).loaded) return;
    undeadWitnessDaily(w);
    trialsDaily(w);
    guildsDaily(w);
    guildHealDaily(w);
    workDaily(w);
}

void installMagicSociety(World& w) {
    w.addCommandHandler(societyCommand);
    static const bool once = [] {
        addRealmHook(realmMagic);
        setMagicServiceHook([](World& world, Character& provider, Character& client, const std::string& what) {
            const SocietyRules& r = societyRules(world.content());
            if (!r.loaded) return;
            countService(world, provider, what == "heal" ? "heals" : "work");
            if (provider.status.family == client.status.family) return; // one's own are not charged
            Character* payer = payerOf(world, client.status.family);
            if (!payer) payer = &client;
            const double paid = payForMagic(world, *payer, provider, what == "heal" ? r.healFee : r.workFee);
            if (paid > 0.0)
                world.chronicle("magic", {provider.id, payer->id}, "chronicle.magic.society.paid",
                                {{"a", provider.id}, {"b", payer->id}, {"n", static_cast<int>(std::round(paid))}});
        });
        return true;
    }();
    (void)once;
}

} // namespace lr::sim
