// LR-169: the pantheon - Powers, liks, the world laws, prayer. See pantheon.hpp.
#include "sim/pantheon.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>

#include "sim/content.hpp"
#include "sim/culture.hpp"
#include "sim/data_fs.hpp"
#include "sim/magic_society.hpp"
#include "sim/world.hpp"

namespace lr::sim {

namespace fs = std::filesystem;

namespace {

std::map<std::string, double> dmap(const Json& j, const char* k) {
    std::map<std::string, double> out;
    if (!j.is_object() || !j.contains(k) || !j[k].is_object()) return out;
    for (auto it = j[k].begin(); it != j[k].end(); ++it)
        if (it.value().is_number()) out[it.key()] = it.value().get<double>();
    return out;
}

std::vector<std::string> strs(const Json& j, const char* k) {
    std::vector<std::string> out;
    if (!j.is_object() || !j.contains(k) || !j[k].is_array()) return out;
    for (const auto& s : j[k])
        if (s.is_string()) out.push_back(s.get<std::string>());
    return out;
}

double num(const Json& j, const char* k, double d) { return j.is_object() && j.contains(k) && j[k].is_number() ? j[k].get<double>() : d; }
int inum(const Json& j, const char* k, int d) { return static_cast<int>(num(j, k, d)); }
std::string str(const Json& j, const char* k, const std::string& d) {
    return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : d;
}

void loadNumbers(PantheonRules& r, const Json& j) {
    if (j.contains("belts") && j["belts"].is_array()) r.belts = strs(j, "belts");
    if (j.contains("culture_belts") && j["culture_belts"].is_object())
        for (auto it = j["culture_belts"].begin(); it != j["culture_belts"].end(); ++it)
            if (it->is_string()) r.cultureBelts[it.key()] = it->get<std::string>();
    const Json s = j.value("standing", Json::object());
    auto& S = r.standing;
    S.start = num(s, "start", S.start);
    S.gain = num(s, "gain", S.gain);
    S.decay = num(s, "decay", S.decay);
    S.riteGain = num(s, "rite_gain", S.riteGain);
    S.prayerGain = num(s, "prayer_gain", S.prayerGain);
    S.floor = num(s, "floor", S.floor);
    S.worshipNorm = std::max(1.0, num(s, "worship_norm", S.worshipNorm));
    const Json f = j.value("favor", Json::object());
    auto& F = r.favor;
    F.start = num(f, "start", F.start);
    F.riteGain = num(f, "rite_gain", F.riteGain);
    F.loveGain = num(f, "love_gain", F.loveGain);
    F.hateLoss = num(f, "hate_loss", F.hateLoss);
    F.sinLoss = num(f, "sin_loss", F.sinLoss);
    F.decay = num(f, "decay", F.decay);
    F.offeringGain = num(f, "offering_gain", F.offeringGain);
    F.wantedMult = num(f, "wanted_mult", F.wantedMult);
    F.moodletAt = num(f, "moodlet_at", F.moodletAt);
    F.wrathAt = num(f, "wrath_at", F.wrathAt);
    F.omenChance = num(f, "omen_chance", F.omenChance);
    F.identShare = num(f, "ident_share", F.identShare);
    F.favorMoodlet = str(f, "favor_moodlet", F.favorMoodlet);
    F.wrathMoodlet = str(f, "wrath_moodlet", F.wrathMoodlet);
    const Json i = j.value("identification", Json::object());
    auto& I = r.ident;
    I.base = num(i, "base", I.base);
    I.step = num(i, "step", I.step);
    I.at = num(i, "at", I.at);
    I.hospitalityMult = num(i, "hospitality_mult", I.hospitalityMult);
    I.affinity = num(i, "affinity", I.affinity);
    I.heresyPiety = num(i, "heresy_piety", I.heresyPiety);
    I.heresyTolerance = num(i, "heresy_tolerance", I.heresyTolerance);
    I.heresyChance = num(i, "heresy_chance", I.heresyChance);
    I.heresyEnmity = num(i, "heresy_enmity", I.heresyEnmity);
    I.heresyStep = num(i, "heresy_step", I.heresyStep);
    I.rivalryEveryDays = inum(i, "rivalry_every_days", I.rivalryEveryDays);
    I.heresyEveryDays = inum(i, "heresy_every_days", I.heresyEveryDays);
    I.heresyMoodlet = str(i, "heresy_moodlet", I.heresyMoodlet);
    const Json p = j.value("prayer", Json::object());
    auto& P = r.prayer;
    const Json base = p.value("base", Json::object());
    P.grant = num(base, "granted", P.grant);
    P.refuse = num(base, "refused", P.refuse);
    P.defer = num(base, "deferred", P.defer);
    P.silent = num(base, "silent", P.silent);
    const Json wt = p.value("weights", Json::object());
    P.wFavor = num(wt, "favor", P.wFavor);
    P.wWeight = num(wt, "weight", P.wWeight);
    P.wMercy = num(wt, "mercy", P.wMercy);
    P.wGenerosity = num(wt, "generosity", P.wGenerosity);
    P.wDomain = num(wt, "domain", P.wDomain);
    P.wBudget = num(wt, "budget", P.wBudget);
    P.wWrath = num(wt, "wrath", P.wWrath);
    P.wPride = num(wt, "pride", P.wPride);
    P.wAloof = num(wt, "aloof", P.wAloof);
    P.wCunning = num(wt, "cunning", P.wCunning);
    P.wDebt = num(wt, "debt", P.wDebt);
    P.noise = num(p, "noise", P.noise);
    P.budgetBase = num(p, "budget_base", P.budgetBase);
    P.budgetPerStanding = num(p, "budget_per_standing", P.budgetPerStanding);
    P.deferMinDays = inum(p, "defer_min_days", P.deferMinDays);
    P.deferMaxDays = std::max(P.deferMinDays, inum(p, "defer_max_days", P.deferMaxDays));
    P.debtSilent = num(p, "debt_silent", P.debtSilent);
    P.debtOverdue = num(p, "debt_overdue", P.debtOverdue);
    P.debtRepay = num(p, "debt_repay", P.debtRepay);
    P.debtDecay = num(p, "debt_decay", P.debtDecay);
    P.debtBreachAt = num(p, "debt_breach_at", P.debtBreachAt);
    P.strengthBase = num(p, "strength_base", P.strengthBase);
    P.strengthWeight = num(p, "strength_weight", P.strengthWeight);
    P.strengthDomain = num(p, "strength_domain", P.strengthDomain);
    P.grantedMoodlet = str(p, "granted_moodlet", P.grantedMoodlet);
    P.refusedMoodlet = str(p, "refused_moodlet", P.refusedMoodlet);
    const Json l = j.value("retribution", Json::object());
    auto& R = r.retribution;
    R.delayMinDays = inum(l, "delay_min_days", R.delayMinDays);
    R.delayMaxDays = std::max(R.delayMinDays, inum(l, "delay_max_days", R.delayMaxDays));
    R.curseDays = inum(l, "curse_days", R.curseDays);
    R.settlementWindowDays = inum(l, "settlement_window_days", R.settlementWindowDays);
    R.curseMood = num(l, "curse_mood", R.curseMood);
    R.pietyLoss = num(l, "piety_loss", R.pietyLoss);
    R.favorLoss = num(l, "favor_loss", R.favorLoss);
    R.communityTrust = num(l, "community_trust", R.communityTrust);
    R.healthLoss = num(l, "health_loss", R.healthLoss);
    R.witnessMult = num(l, "witness_mult", R.witnessMult);
    R.witnessCap = num(l, "witness_cap", R.witnessCap);
    R.sacredMult = num(l, "sacred_mult", R.sacredMult);
    R.swornMult = num(l, "sworn_mult", R.swornMult);
    R.settlementBreaches = inum(l, "settlement_breaches", R.settlementBreaches);
    R.curseMoodlet = str(l, "curse_moodlet", R.curseMoodlet);
    R.breachMemory = str(l, "breach_memory", R.breachMemory);
    R.curseMemory = str(l, "curse_memory", R.curseMemory);
    R.initiateMemory = str(l, "initiate_memory", R.initiateMemory);
    R.sacredRadius = num(l, "sacred_radius", R.sacredRadius);
    R.graveRadius = num(l, "grave_radius", R.graveRadius);
    if (l.contains("sacred_objects")) R.sacredObjects = strs(l, "sacred_objects");
    if (l.contains("grave_objects")) R.graveObjects = strs(l, "grave_objects");
    const Json g = l.value("god", Json::object());
    R.godStanding = num(g, "standing_loss", R.godStanding);
    R.bindAt = num(g, "bind_at", R.bindAt);
    R.bindDays = inum(g, "bind_days", R.bindDays);
    R.domainLoss = num(g, "domain_loss", R.domainLoss);
    if (j.contains("moodlets") && j["moodlets"].is_object())
        for (auto it = j["moodlets"].begin(); it != j["moodlets"].end(); ++it)
            r.moodlets[it.key()] = {num(*it, "value", 0.0), static_cast<Tick>(num(*it, "ttl", 144))};
}

std::unique_ptr<PantheonRules> loadRules(const fs::path& root) {
    auto r = std::make_unique<PantheonRules>();
    const fs::path dir = root / "pantheon";
    if (!data::exists(dir / "powers.json")) return r;
    try {
        if (data::exists(dir / "rules.json")) loadNumbers(*r, data::readJson((dir / "rules.json").string()));
        for (const Json& pj : data::readJson((dir / "powers.json").string()).value("powers", Json::array())) {
            PowerDef p;
            p.id = pj.value("id", std::string());
            if (p.id.empty()) continue;
            p.aspects = strs(pj, "aspects");
            p.aims = strs(pj, "aims");
            p.laws = strs(pj, "laws");
            p.values = dmap(pj, "values");
            if (!r->powers.count(p.id)) r->powerOrder.push_back(p.id);
            r->powers[p.id] = p;
        }
        for (const fs::path& fp : data::listPaths(dir / "liks")) {
            const Json bj = data::readJson(fp.string());
            const std::string belt = bj.value("belt", fp.stem().string());
            for (const Json& lj : bj.value("liks", Json::array())) {
                LikDef l;
                l.id = lj.value("id", std::string());
                if (l.id.empty()) continue;
                l.belt = lj.value("belt", belt);
                l.power = lj.value("power", std::string());
                l.also = strs(lj, "also");
                l.character = dmap(lj, "character");
                l.loves = strs(lj, "loves");
                l.hates = strs(lj, "hates");
                l.wants = strs(lj, "wants");
                l.epithets = strs(lj, "epithets");
                l.rivals = strs(lj, "rivals");
                l.allies = strs(lj, "allies");
                l.phase = lj.value("phase", 1);
                if (!r->liks.count(l.id)) r->likOrder.push_back(l.id);
                r->liks[l.id] = l;
            }
        }
        for (const fs::path& fp : data::listPaths(root / "laws")) {
            const Json lj = data::readJson(fp.string());
            LawDef d;
            d.id = lj.value("id", fp.stem().string());
            d.watchers = strs(lj, "watchers");
            d.weight = num(lj, "weight", d.weight);
            d.bindsGods = lj.value("binds_gods", true);
            for (const Json& fr : lj.value("facts", Json::array())) d.facts.push_back({strs(fr, "preds"), strs(fr, "when")});
            d.memories = strs(lj, "memories");
            if (!r->laws.count(d.id)) r->lawOrder.push_back(d.id);
            r->laws[d.id] = d;
        }
        if (r->belts.empty()) r->belts = {"mediterranean", "nile", "north", "forest", "steppe"};
        // the belts of the world (LR-166 content/world/belts.json) when there is such a file
        if (data::exists(root / "world" / "belts.json")) {
            const Json wb = data::readJson((root / "world" / "belts.json").string());
            std::vector<std::string> ids;
            const Json arr = wb.is_array() ? wb : wb.value("belts", Json::array());
            for (const Json& b : arr) {
                if (b.is_string()) ids.push_back(b.get<std::string>());
                else if (b.is_object() && b.contains("id")) ids.push_back(b["id"].get<std::string>());
            }
            for (const auto& b : ids)
                if (std::find(r->belts.begin(), r->belts.end(), b) == r->belts.end()) r->belts.push_back(b);
        }
        r->loaded = true;
    } catch (const std::exception& e) {
        r->errors.push_back(std::string("pantheon: ") + e.what());
    }
    return r;
}

bool person(const Character& c) { return !c.isAnimal(); }

Json& stateOf(World& w) {
    Json& s = w.moduleState("pantheon");
    if (!s.is_object()) s = Json::object();
    for (const char* k : {"gods", "favor", "ident", "curses", "seen"})
        if (!s.contains(k) || !s[k].is_object()) s[k] = Json::object();
    for (const char* k : {"pending", "breaches"})
        if (!s.contains(k) || !s[k].is_array()) s[k] = Json::array();
    if (!s.contains("facts") || !s["facts"].is_number_integer()) s["facts"] = 0;
    return s;
}
const Json* stateOf(const World& w) {
    const Json* s = w.moduleState("pantheon");
    return s && s->is_object() ? s : nullptr;
}

Json& godJson(World& w, const PantheonRules& r, const std::string& lik) {
    Json& g = stateOf(w)["gods"];
    if (!g.contains(lik)) g[lik] = {{"st", r.standing.start}, {"debt", 0.0}, {"dom", 1.0}, {"bound", -1}, {"wor", 0}};
    return g[lik];
}

std::string identKey(const std::string& a, const std::string& b) { return a < b ? a + "|" + b : b + "|" + a; }

std::string placeName(const World& w, EntityId sid) {
    auto it = w.settlements().find(sid);
    return it == w.settlements().end() ? std::string() : it->second.name;
}

EntityId settlementOf(const World& w, const Character& c) {
    if (c.status.settlement != kNone) return c.status.settlement;
    if (const Family* f = w.familyOf(c)) return f->settlement;
    return kNone;
}

void setMoodlet(World& w, Character& c, const PantheonRules& r, const std::string& id, double scale = 1.0) {
    if (id.empty()) return;
    double value = 0.0;
    Tick ttl = 144;
    if (auto it = r.moodlets.find(id); it != r.moodlets.end()) value = it->second.value, ttl = it->second.ttl;
    else if (auto md = w.content().moodlets.find(id); md != w.content().moodlets.end()) value = md->second.value, ttl = md->second.ttl;
    else return;
    const Tick until = w.now() + static_cast<Tick>(std::max(1.0, ttl * scale));
    auto it = std::find_if(c.moodlets.begin(), c.moodlets.end(), [&](const Moodlet& m) { return m.id == id; });
    if (it != c.moodlets.end()) it->until = std::max(it->until, until);
    else c.moodlets.push_back({id, value, until});
}

void rememberType(World& w, Character& c, const std::string& type, double valence, double salience) {
    if (type.empty()) return;
    MemoryEntry m;
    m.type = type;
    m.valence = valence;
    m.salience = salience;
    w.remember(c, m);
}

bool isKin(const World& w, const Character& a, const Character& b) {
    if (a.id == b.id) return false;
    if (a.status.spouse == b.id || b.status.spouse == a.id) return true;
    if (a.status.mother == b.id || a.status.father == b.id || b.status.mother == a.id || b.status.father == a.id) return true;
    if ((a.status.mother != kNone && a.status.mother == b.status.mother) || (a.status.father != kNone && a.status.father == b.status.father))
        return true;
    return a.status.family != kNone && a.status.family == b.status.family;
}

bool nearType(const World& w, Vec2 at, const std::vector<std::string>& types, double radius) {
    for (const auto& [id, o] : w.objects())
        if (std::find(types.begin(), types.end(), o.type) != types.end() && dist(o.pos, at) <= radius) return true;
    return false;
}

bool hasMemory(const Character& c, const std::string& type) {
    return std::any_of(c.memory.begin(), c.memory.end(), [&](const MemoryEntry& m) { return m.type == type; });
}

// the liks that watch a law: every lik whose Powers include a watcher
std::vector<std::string> watchersOf(const PantheonRules& r, const LawDef& law, const std::string& belt) {
    std::vector<std::string> out;
    for (const auto& id : r.likOrder) {
        const LikDef& l = r.liks.at(id);
        if (!belt.empty() && l.belt != belt) continue;
        for (const auto& p : law.watchers)
            if (l.hasPower(p)) {
                out.push_back(id);
                break;
            }
    }
    return out;
}

std::string personBelt(const World& w, const Character& c) { return cultureBelt(w.content(), cultureOf(w, c)); }

PrayerFulfillFn& fulfillHook() {
    static PrayerFulfillFn fn;
    return fn;
}

Json askJson(const PrayerAsk& p) {
    return {{"who", p.who}, {"faith", p.faith}, {"lik", p.lik}, {"aim", p.aim}, {"offering", p.offering}, {"rite", p.rite},
            {"place", p.place}, {"day", p.day}, {"weight", p.weight}};
}
PrayerAsk askFrom(const Json& j) {
    PrayerAsk p;
    p.who = j.value("who", kNone);
    p.faith = j.value("faith", std::string());
    p.lik = j.value("lik", std::string());
    p.aim = j.value("aim", std::string());
    p.offering = j.value("offering", std::string());
    p.rite = j.value("rite", std::string());
    p.place = j.value("place", kNone);
    p.day = j.value("day", -1);
    p.weight = j.value("weight", 1.0);
    return p;
}

} // namespace

// --- definitions ---------------------------------------------------------------------------
double LikDef::trait(const std::string& k, double d) const {
    auto it = character.find(k);
    return it == character.end() ? d : it->second;
}
bool LikDef::hasPower(const std::string& p) const { return power == p || std::find(also.begin(), also.end(), p) != also.end(); }

const PowerDef* PantheonRules::power(const std::string& id) const {
    auto it = powers.find(id);
    return it == powers.end() ? nullptr : &it->second;
}
const LikDef* PantheonRules::lik(const std::string& id) const {
    auto it = liks.find(id);
    return it == liks.end() ? nullptr : &it->second;
}
const LawDef* PantheonRules::law(const std::string& id) const {
    auto it = laws.find(id);
    return it == laws.end() ? nullptr : &it->second;
}

const PantheonRules& pantheonRules(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<PantheonRules>> cache;
    static const PantheonRules kNone;
    if (c.dir.empty()) return kNone;
    std::lock_guard<std::mutex> lock(mu);
    const std::string key = data::cacheKey(c.dir);
    auto it = cache.find(key);
    if (it == cache.end()) it = cache.emplace(key, loadRules(fs::path(c.dir).parent_path())).first;
    return *it->second;
}

// --- who worships whom ------------------------------------------------------------------------
std::string cultureBelt(const Content& c, const std::string& culture) {
    if (culture.empty()) return "";
    if (const Json* d = cultureData(c, culture)) {
        const std::string b = d->value("belt", std::string());
        if (!b.empty()) return b;
    }
    const PantheonRules& r = pantheonRules(c);
    auto it = r.cultureBelts.find(culture);
    return it == r.cultureBelts.end() ? std::string() : it->second;
}

namespace {
// the faith's liks JSON (religions/<id>.json "liks"): read through the faith rules' file
const Json* faithLiksJson(const Content& c, const std::string& faith) {
    static std::mutex mu;
    static std::map<std::string, std::map<std::string, Json>> cache;
    if (c.dir.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(mu);
    const std::string key = data::cacheKey(c.dir);
    auto it = cache.find(key);
    if (it == cache.end()) {
        std::map<std::string, Json> m;
        for (const fs::path& fp : data::listPaths(fs::path(c.dir).parent_path() / "culture" / "religions")) {
            try {
                const Json fj = data::readJson(fp.string());
                const std::string id = fj.value("id", fp.stem().string());
                m[id] = fj.contains("liks") && fj["liks"].is_object() ? fj["liks"] : Json::object();
            } catch (...) {
            }
        }
        it = cache.emplace(key, std::move(m)).first;
    }
    auto f = it->second.find(faith);
    return f == it->second.end() ? nullptr : &f->second;
}
} // namespace

std::vector<std::string> faithLiks(const Content& c, const std::string& faith, const std::string& belt) {
    const Json* l = faithLiksJson(c, faith);
    if (!l) return {};
    if (!belt.empty() && l->contains(belt) && (*l)[belt].is_array() && !(*l)[belt].empty()) return strs(*l, belt.c_str());
    return strs(*l, "*");
}

std::vector<std::string> faithAllLiks(const Content& c, const std::string& faith) {
    std::vector<std::string> out;
    const Json* l = faithLiksJson(c, faith);
    if (!l) return out;
    auto add = [&](const std::string& b) {
        for (const auto& x : strs(*l, b.c_str()))
            if (std::find(out.begin(), out.end(), x) == out.end()) out.push_back(x);
    };
    for (const auto& b : pantheonRules(c).belts) add(b);
    add("*");
    return out;
}

std::vector<std::string> godsOf(const World& w, const Character& c) {
    if (!person(c)) return {};
    return faithLiks(w.content(), faithOf(w, c), personBelt(w, c));
}

bool likServesAim(const Content& c, const std::string& lik, const std::string& aim) {
    const PantheonRules& r = pantheonRules(c);
    const LikDef* l = r.lik(lik);
    if (!l) return false;
    auto serves = [&](const std::string& p) {
        const PowerDef* pd = r.power(p);
        return pd && std::find(pd->aims.begin(), pd->aims.end(), aim) != pd->aims.end();
    };
    if (serves(l->power)) return true;
    return std::any_of(l->also.begin(), l->also.end(), serves);
}

std::string likForAim(const World& w, const std::string& faith, const std::string& aim, const std::string& belt) {
    const auto liks = faithLiks(w.content(), faith, belt);
    for (const auto& l : liks)
        if (likServesAim(w.content(), l, aim)) return l;
    return liks.empty() ? std::string() : liks.front();
}

std::string likForAim(const World& w, const Character& c, const std::string& faith, const std::string& aim) {
    return likForAim(w, faith.empty() ? faithOf(w, c) : faith, aim, personBelt(w, c));
}

// --- the gods' state -----------------------------------------------------------------------------
GodState godState(const World& w, const std::string& lik) {
    GodState g;
    g.standing = pantheonRules(w.content()).standing.start;
    const Json* s = stateOf(w);
    if (!s || !s->contains("gods") || !(*s)["gods"].contains(lik)) return g;
    const Json& j = (*s)["gods"][lik];
    g.standing = j.value("st", g.standing);
    g.debt = j.value("debt", 0.0);
    g.domain = j.value("dom", 1.0);
    g.boundUntil = j.value("bound", -1);
    g.worshippers = j.value("wor", 0);
    return g;
}

double domainShare(const World& w, const std::string& lik) {
    const PantheonRules& r = pantheonRules(w.content());
    const LikDef* l = r.lik(lik);
    if (!l) return 0.0;
    double mine = 0.0, sum = 0.0;
    for (const auto& [id, o] : r.liks) {
        if (o.power != l->power) continue;
        const GodState g = godState(w, id);
        const double v = std::max(0.0, g.standing) * std::max(0.0, g.domain);
        sum += v;
        if (id == lik) mine = v;
    }
    return sum > 0.0 ? mine / sum : 0.0;
}

double identification(const World& w, const std::string& a, const std::string& b) {
    if (a == b) return 1.0;
    const Json* s = stateOf(w);
    if (!s || !s->contains("ident")) return 0.0;
    return (*s)["ident"].value(identKey(a, b), 0.0);
}
bool identified(const World& w, const std::string& a, const std::string& b) {
    return a == b || identification(w, a, b) >= pantheonRules(w.content()).ident.at;
}

double favorOf(const World& w, EntityId who, const std::string& lik, bool shared) {
    const Json* s = stateOf(w);
    const PantheonRules& r = pantheonRules(w.content());
    if (!s || !s->contains("favor")) return r.favor.start;
    const std::string k = std::to_string(who);
    auto own = [&](const std::string& l) {
        const Json& f = (*s)["favor"];
        return f.contains(l) && f[l].contains(k) ? f[l][k].get<double>() : r.favor.start;
    };
    double v = own(lik);
    if (shared && s->contains("ident"))
        for (auto it = (*s)["ident"].begin(); it != (*s)["ident"].end(); ++it) {
            if (it->get<double>() < r.ident.at) continue;
            const std::string& key = it.key();
            const auto bar = key.find('|');
            const std::string a = key.substr(0, bar), b = key.substr(bar + 1);
            if (a == lik) v += own(b) * r.favor.identShare;
            else if (b == lik) v += own(a) * r.favor.identShare;
        }
    return std::clamp(v, -1.0, 1.0);
}

void addFavor(World& w, EntityId who, const std::string& lik, double d, const std::string&) {
    if (lik.empty() || who == kNone || d == 0.0) return;
    Json& f = stateOf(w)["favor"];
    const std::string k = std::to_string(who);
    const double was = f.contains(lik) && f[lik].contains(k) ? f[lik][k].get<double>() : pantheonRules(w.content()).favor.start;
    f[lik][k] = std::clamp(was + d, -1.0, 1.0);
}

// --- prayer ---------------------------------------------------------------------------------------
const char* answerName(DivineAnswerKind k) {
    switch (k) {
    case DivineAnswerKind::Granted: return "granted";
    case DivineAnswerKind::Refused: return "refused";
    case DivineAnswerKind::Deferred: return "deferred";
    default: return "silent";
    }
}

void setPrayerFulfill(PrayerFulfillFn fn) { fulfillHook() = std::move(fn); }

DivineAnswer answerPrayer(World& w, const PrayerAsk& ask) {
    const PantheonRules& r = pantheonRules(w.content());
    DivineAnswer a;
    if (!r.loaded) return a;
    Character* c = w.character(ask.who);
    PrayerAsk p = ask;
    if (p.day < 0) p.day = w.day();
    if (p.faith.empty() && c) p.faith = faithOf(w, *c);
    if (p.lik.empty()) p.lik = c ? likForAim(w, *c, p.faith, p.aim) : likForAim(w, p.faith, p.aim);
    a.lik = p.lik;
    const LikDef* l = r.lik(p.lik);
    if (!l) {
        a.reason = "pantheon.refuse.outside_domain";
        a.kind = DivineAnswerKind::Refused;
        return a;
    }
    Json& g = godJson(w, r, p.lik);
    const int today = w.day();
    const double weight = std::max(0.0, p.weight);
    // the offering pleases the god whatever the answer; one it wants more so
    if (p.byRule() && c) {
        double gain = r.favor.offeringGain * std::min(3.0, std::max(0.3, weight));
        if (!p.offering.empty() && std::find(l->wants.begin(), l->wants.end(), p.offering) != l->wants.end()) gain *= r.favor.wantedMult;
        addFavor(w, p.who, p.lik, gain, "offering");
    }
    g["st"] = std::min(1.0, g.value("st", r.standing.start) + r.standing.prayerGain * std::max(0.5, weight));
    const GodState gs = godState(w, p.lik);
    auto finish = [&](DivineAnswerKind k) {
        a.kind = k;
        Json params = {{"god", p.lik}, {"who", p.who}, {"aim", p.aim}};
        if (k == DivineAnswerKind::Refused) params["refusal"] = a.reason.substr(a.reason.rfind('.') + 1);
        w.chronicle("pantheon", c ? std::vector<EntityId>{c->id} : std::vector<EntityId>{}, std::string("chronicle.pantheon.") + answerName(k), params);
        if (c) {
            if (k == DivineAnswerKind::Granted) setMoodlet(w, *c, r, r.prayer.grantedMoodlet);
            else if (k == DivineAnswerKind::Refused) setMoodlet(w, *c, r, r.prayer.refusedMoodlet);
        }
        return a;
    };
    // a god in chains cannot answer: silence, and a plea made by the rule is owed
    if (gs.bound(today)) {
        a.reason = "pantheon.refuse.bound";
        if (p.byRule()) g["debt"] = g.value("debt", 0.0) + r.prayer.debtSilent * weight;
        return finish(DivineAnswerKind::Silent);
    }
    if (!likServesAim(w.content(), p.lik, p.aim)) {
        a.reason = "pantheon.refuse.outside_domain";
        return finish(DivineAnswerKind::Refused);
    }
    // one who broke a law this god's Powers watch is refused until the law is answered
    if (c) {
        const Json& br = stateOf(w)["breaches"];
        for (const Json& b : br) {
            if (b.value("doer", kNone) != c->id || b.value("done", false)) continue;
            const LawDef* law = r.law(b.value("law", std::string()));
            if (law && std::any_of(law->watchers.begin(), law->watchers.end(), [&](const std::string& pw) { return l->hasPower(pw); })) {
                a.reason = "pantheon.refuse.breaks_law";
                return finish(DivineAnswerKind::Refused);
            }
        }
    }
    // today's miracles: a budget by standing
    if (g.value("used_day", -1) != today) g["used_day"] = today, g["used"] = 0;
    const double budget = r.prayer.budgetBase + r.prayer.budgetPerStanding * gs.standing;
    const double used = g.value("used", 0);
    const double fav = favorOf(w, p.who, p.lik);
    const double share = domainShare(w, p.lik);
    const auto& P = r.prayer;
    Rng rng = w.rng(p.who, "pray_" + p.lik + "_" + p.aim);
    auto noise = [&]() { return (rng.uniform() * 2.0 - 1.0) * P.noise; };
    double sg = P.grant + P.wFavor * fav + P.wWeight * weight + P.wMercy * l->trait("mercy") + P.wGenerosity * l->trait("generosity") +
                P.wDomain * share - P.wBudget * std::max(0.0, used + 1.0 - budget) + noise();
    double sr = P.refuse + P.wWrath * l->trait("wrath") - P.wFavor * fav + P.wPride * l->trait("pride") * std::max(0.0, 1.0 - weight) + noise();
    double sd = P.defer + P.wAloof * l->trait("aloof") * 0.5 + P.wCunning * l->trait("cunning") + P.wBudget * std::max(0.0, used + 1.0 - budget) + noise();
    // silence: an aloof god may ignore a plea not made by the rule; the law makes it costly otherwise
    double ss = P.silent + P.wAloof * l->trait("aloof") - (p.byRule() ? P.wDebt * (1.0 + gs.debt) : 0.0) + noise();
    const double best = std::max({sg, sr, sd, ss});
    if (best == sg) {
        g["used"] = used + 1;
        a.strength = std::clamp(P.strengthBase + P.strengthWeight * std::min(2.0, weight) + P.strengthDomain * share, 0.05, 1.0);
        g["debt"] = std::max(0.0, g.value("debt", 0.0) - P.debtRepay * 0.5);
        return finish(DivineAnswerKind::Granted);
    }
    if (best == sd) {
        a.dueDay = today + P.deferMinDays + static_cast<int>(rng.uniform() * (P.deferMaxDays - P.deferMinDays + 1));
        a.dueDay = std::min(a.dueDay, today + P.deferMaxDays);
        a.strength = std::clamp(P.strengthBase + P.strengthWeight * std::min(2.0, weight) + P.strengthDomain * share, 0.05, 1.0);
        Json e = askJson(p);
        e["due"] = a.dueDay;
        e["strength"] = a.strength;
        stateOf(w)["pending"].push_back(e);
        return finish(DivineAnswerKind::Deferred);
    }
    if (best == sr) {
        a.reason = fav < 0.0 ? "pantheon.refuse.no_favor" : "pantheon.refuse.against_fate";
        return finish(DivineAnswerKind::Refused);
    }
    if (p.byRule()) g["debt"] = g.value("debt", 0.0) + P.debtSilent * weight;
    return finish(DivineAnswerKind::Silent);
}

void onRiteDone(World& w, EntityId who, const std::string& faith, const std::string&, double weight) {
    const PantheonRules& r = pantheonRules(w.content());
    if (!r.loaded) return;
    const Character* c = w.character(who);
    std::vector<std::string> liks = c ? faithLiks(w.content(), faith.empty() ? faithOf(w, *c) : faith, personBelt(w, *c))
                                      : faithLiks(w.content(), faith, "");
    if (liks.empty()) return;
    const double k = std::max(0.0, weight);
    for (size_t i = 0; i < liks.size(); ++i) {
        const double share = i == 0 ? 1.0 : 0.5;
        addFavor(w, who, liks[i], r.favor.riteGain * k * share, "rite");
        Json& g = godJson(w, r, liks[i]);
        g["st"] = std::min(1.0, g.value("st", r.standing.start) + r.standing.riteGain * k * share);
    }
}

// --- the world laws -----------------------------------------------------------------------------------
bool reportBreach(World& w, const Breach& b) {
    const PantheonRules& r = pantheonRules(w.content());
    const LawDef* law = r.law(b.law);
    if (!law) return false;
    if (!b.god.empty() && (!law->bindsGods || !r.lik(b.god))) return false;
    const auto& R = r.retribution;
    double weight = std::max(0.0, b.weight) * law->weight;
    weight *= 1.0 + std::min(R.witnessCap, R.witnessMult * static_cast<double>(b.witnesses.size()));
    if (b.sacred) weight *= 1.0 + R.sacredMult;
    if (!b.sworn.empty()) weight *= 1.0 + R.swornMult;
    Character* doer = w.character(b.doer);
    if (doer && doer->status.role != "member") weight *= 1.1; // a chief or an office holder answers more
    Rng rng = w.rng(b.doer == kNone ? 0 : b.doer, "breach_" + b.law + b.god);
    const int due = w.day() + R.delayMinDays + static_cast<int>(rng.uniform() * (R.delayMaxDays - R.delayMinDays + 1));
    Json e = {{"law", b.law}, {"doer", b.doer}, {"god", b.god}, {"victim", b.victim}, {"weight", weight}, {"day", w.day()},
              {"due", std::min(due, w.day() + R.delayMaxDays)}, {"done", false}, {"why", b.why}};
    if (doer) e["settlement"] = settlementOf(w, *doer);
    stateOf(w)["breaches"].push_back(e);
    if (doer) {
        rememberType(w, *doer, R.breachMemory, -0.4, 0.7);
        // the watchers turn their faces away now; the blow comes later
        for (const auto& lik : watchersOf(r, *law, personBelt(w, *doer))) addFavor(w, doer->id, lik, -R.favorLoss * 0.5 * weight, "breach");
        for (const auto& lik : godsOf(w, *doer)) {
            const LikDef* l = r.lik(lik);
            if (l && std::any_of(law->watchers.begin(), law->watchers.end(), [&](const std::string& p) { return l->hasPower(p); }))
                addFavor(w, doer->id, lik, -R.favorLoss * 0.5 * weight, "breach");
        }
        w.chronicle("pantheon", {doer->id}, "chronicle.pantheon.breach", {{"who", doer->id}, {"law", b.law}});
    } else if (!b.god.empty()) {
        w.chronicle("pantheon", {}, "chronicle.pantheon.god_breach", {{"god", b.god}, {"law", b.law}});
    }
    return true;
}

int openBreaches(const World& w, EntityId doer, const std::string& god) {
    const Json* s = stateOf(w);
    if (!s || !s->contains("breaches")) return 0;
    int n = 0;
    for (const Json& b : (*s)["breaches"]) {
        if (b.value("done", false)) continue;
        if (!god.empty() ? b.value("god", std::string()) == god : (doer != kNone && b.value("doer", kNone) == doer)) ++n;
    }
    return n;
}

Json breachLog(const World& w) {
    const Json* s = stateOf(w);
    return s && s->contains("breaches") ? (*s)["breaches"] : Json::array();
}

namespace {

bool factMatches(const World& w, const PantheonRules& r, const Fact& f, const LawDef::FactRule& fr) {
    if (std::find(fr.preds.begin(), fr.preds.end(), f.pred) == fr.preds.end()) return false;
    const Character* doer = w.character(f.role("doer"));
    const Character* victim = w.character(f.role("victim"));
    for (const auto& cond : fr.when) {
        if (cond == "victim_kin") {
            if (!doer || !victim || !isKin(w, *doer, *victim)) return false;
        } else if (cond == "guest_or_host") {
            // one of them is a stranger to the other's place: a guest wronged, or a host wronged by a guest
            if (!doer || !victim || victim->isAnimal()) return false;
            const EntityId ds = settlementOf(w, *doer), vs = settlementOf(w, *victim);
            if (ds == vs) return false;
        } else if (cond == "at_sacred") {
            if (!nearType(w, f.place, r.retribution.sacredObjects, r.retribution.sacredRadius)) return false;
        } else if (cond == "near_grave") {
            if (!nearType(w, f.place, r.retribution.graveObjects, r.retribution.graveRadius)) return false;
        } else if (cond == "initiate") {
            if (!doer || !hasMemory(*doer, r.retribution.initiateMemory)) return false;
        } else {
            return false; // an unknown condition never matches (the validator names it)
        }
    }
    return true;
}

void scanFacts(World& w, const PantheonRules& r) {
    Json& s = stateOf(w);
    const std::size_t from = static_cast<std::size_t>(std::max<std::int64_t>(0, s.value("facts", 0)));
    const auto& facts = w.facts();
    for (std::size_t i = from; i < facts.size(); ++i) {
        const Fact& f = facts[i];
        for (const auto& id : r.lawOrder) {
            const LawDef& law = r.laws.at(id);
            for (const auto& fr : law.facts)
                if (factMatches(w, r, f, fr)) {
                    Breach b;
                    b.law = id;
                    b.doer = f.role("doer");
                    b.victim = f.role("victim");
                    b.witnesses = f.witnesses;
                    b.sacred = nearType(w, f.place, r.retribution.sacredObjects, r.retribution.sacredRadius);
                    b.why = f.pred;
                    if (b.doer != kNone) reportBreach(w, b);
                    break;
                }
        }
    }
    s["facts"] = static_cast<std::int64_t>(facts.size());
}

void punishMortal(World& w, const PantheonRules& r, const LawDef& law, Character& c, double weight) {
    const auto& R = r.retribution;
    setMoodlet(w, c, r, R.curseMoodlet, std::max(0.5, weight));
    rememberType(w, c, R.curseMemory, -0.6, 0.8);
    c.piety = std::clamp((c.piety < 0 ? 0.45 : c.piety) - R.pietyLoss * weight, 0.0, 1.0);
    c.body.health = std::max(0.2, c.body.health - R.healthLoss * weight);
    for (const auto& lik : watchersOf(r, law, personBelt(w, c))) addFavor(w, c.id, lik, -R.favorLoss * 0.5 * weight, "retribution");
    // the community turns away (not a court: the court is its own level)
    const EntityId sid = settlementOf(w, c);
    if (sid != kNone)
        for (auto& [id, o] : w.characters())
            if (o.id != c.id && person(o) && o.body.alive && settlementOf(w, o) == sid) o.relations[c.id].trust += R.communityTrust * weight;
    w.chronicle("pantheon", {c.id}, "chronicle.pantheon.punished", {{"who", c.id}, {"law", law.id}});
}

void punishGod(World& w, const PantheonRules& r, const LawDef& law, const std::string& lik, double weight) {
    const auto& R = r.retribution;
    Json& g = godJson(w, r, lik);
    const int today = w.day();
    g["st"] = std::max(r.standing.floor, g.value("st", r.standing.start) * (1.0 - std::min(0.9, R.godStanding * weight)));
    w.chronicle("pantheon", {}, "chronicle.pantheon.god_punished", {{"god", lik}, {"law", law.id}, {"penalty", "standing"}});
    if (weight >= R.bindAt) {
        const int days = static_cast<int>(std::lround(R.bindDays * weight));
        g["bound"] = std::max(g.value("bound", -1), today + days);
        w.chronicle("pantheon", {}, "chronicle.pantheon.god_bound", {{"god", lik}, {"days", days}});
        // part of the domain goes to another lik of the same Power (the strongest)
        const LikDef* l = r.lik(lik);
        std::string to;
        double best = -1.0;
        if (l)
            for (const auto& [id, o] : r.liks)
                if (id != lik && o.power == l->power) {
                    const double st = godState(w, id).standing;
                    if (st > best) best = st, to = id;
                }
        if (!to.empty()) {
            const double lost = g.value("dom", 1.0) * R.domainLoss;
            g["dom"] = g.value("dom", 1.0) - lost;
            Json& t = godJson(w, r, to);
            t["dom"] = t.value("dom", 1.0) + lost;
            w.chronicle("pantheon", {}, "chronicle.pantheon.domain_lost", {{"god", lik}, {"god_to", to}});
        }
    }
    if (law.id == "exchange") g["debt"] = 0.0; // the debt is paid by the punishment
    g["breach_day"] = today;
}

void retribution(World& w, const PantheonRules& r) {
    Json& s = stateOf(w);
    const int today = w.day();
    std::map<EntityId, std::map<std::string, int>> bySettlement; // settlement -> law -> breaches in the window
    for (Json& b : s["breaches"]) {
        const std::string lawId = b.value("law", std::string());
        const LawDef* law = r.law(lawId);
        if (!law) continue;
        if (!b.value("done", false) && b.value("due", 0) <= today) {
            const double weight = b.value("weight", 1.0);
            if (const std::string god = b.value("god", std::string()); !god.empty()) punishGod(w, r, *law, god, weight);
            else if (Character* c = w.character(b.value("doer", kNone)); c && c->body.alive) punishMortal(w, r, *law, *c, weight);
            b["done"] = true;
        }
        if (b.value("day", 0) >= today - r.retribution.settlementWindowDays && b.contains("settlement") && b["settlement"].is_number_integer())
            ++bySettlement[b["settlement"].get<EntityId>()][lawId];
    }
    // a settlement that broke a law together is cursed whole
    for (const auto& [sid, laws] : bySettlement) {
        if (sid == kNone) continue;
        for (const auto& [lawId, n] : laws) {
            if (n < r.retribution.settlementBreaches) continue;
            const std::string key = std::to_string(sid);
            Json& cur = s["curses"];
            if (cur.contains(key) && cur[key].value("until", 0) > today) continue;
            cur[key] = {{"law", lawId}, {"until", today + r.retribution.curseDays}};
            for (auto& [id, c] : w.characters())
                if (person(c) && c.body.alive && settlementOf(w, c) == sid) setMoodlet(w, c, r, r.retribution.curseMoodlet);
            w.chronicle("pantheon", {}, "chronicle.pantheon.settlement_cursed", {{"place", placeName(w, sid)}, {"law", lawId}});
        }
    }
    // keep the log bounded: punished breaches older than a year go
    Json kept = Json::array();
    for (const Json& b : s["breaches"])
        if (!b.value("done", false) || b.value("day", 0) >= today - kDaysPerYear) kept.push_back(b);
    s["breaches"] = kept;
}

void scanMemories(World& w, const PantheonRules& r, const std::vector<Character*>& people, Tick from) {
    std::map<std::string, std::vector<std::string>> byMemory;
    for (const auto& id : r.lawOrder)
        for (const auto& m : r.laws.at(id).memories) byMemory[m].push_back(id);
    if (byMemory.empty()) return;
    for (Character* c : people) {
        std::set<std::string> done;
        for (const MemoryEntry& m : c->memory) {
            if (m.tick < from || m.source != "seen") continue;
            auto it = byMemory.find(m.type);
            if (it == byMemory.end()) continue;
            for (const auto& law : it->second) {
                if (!done.insert(law).second) continue;
                Breach b;
                b.law = law;
                b.doer = c->id;
                b.why = m.type;
                if (!m.who.empty()) b.victim = m.who.front();
                reportBreach(w, b);
            }
        }
    }
}

} // namespace

// --- the daily pass -------------------------------------------------------------------------------------
void pantheonDaily(World& w) {
    const PantheonRules& r = pantheonRules(w.content());
    if (!r.loaded) return;
    Json& s = stateOf(w);
    const int today = w.day();
    const Tick from = w.now() - kTicksPerDay;

    // 1. worship and the deeds of the day: standing, favour
    std::vector<Character*> people;
    std::map<std::string, double> worship;
    std::map<std::string, int> worshippers;
    std::map<EntityId, std::vector<Character*>> places;
    const CultureRules& cr = cultureRules(w.content());
    for (auto& [id, c] : w.characters()) {
        if (!person(c) || !c.body.alive) continue;
        people.push_back(&c);
        if (const EntityId sid = settlementOf(w, c); sid != kNone) places[sid].push_back(&c);
        const auto gods = godsOf(w, c);
        if (gods.empty()) continue;
        const double piety = pietyOf(w, c);
        std::set<std::string> deeds;
        for (const MemoryEntry& m : c.memory)
            if (m.tick >= from && m.source == "seen") deeds.insert(m.type);
        const FaithDef* fd = cr.faithDef(faithOf(w, c));
        for (size_t i = 0; i < gods.size(); ++i) {
            const std::string& g = gods[i];
            const double share = i == 0 ? 1.0 : 0.5 / static_cast<double>(gods.size() - 1);
            worship[g] += piety * share;
            ++worshippers[g];
            const LikDef* l = r.lik(g);
            if (!l) continue;
            double d = 0.0;
            for (const auto& m : deeds) {
                if (std::find(l->loves.begin(), l->loves.end(), m) != l->loves.end()) d += r.favor.loveGain;
                if (std::find(l->hates.begin(), l->hates.end(), m) != l->hates.end()) d -= r.favor.hateLoss;
                if (fd && fd->rites.count(m)) d += r.favor.riteGain * (i == 0 ? 1.0 : 0.5);
                if (fd && fd->sins.count(m) && fd->sins.at(m) > 0.0) d -= r.favor.sinLoss;
            }
            if (d != 0.0) addFavor(w, c.id, g, d, "deeds");
        }
    }
    for (const auto& id : r.likOrder) {
        Json& g = godJson(w, r, id);
        const double st = g.value("st", r.standing.start);
        const double gain = r.standing.gain * std::min(3.0, worship[id] / r.standing.worshipNorm);
        g["st"] = std::clamp(st + gain - r.standing.decay * st, r.standing.floor, 1.0);
        g["wor"] = worshippers[id];
        g["debt"] = std::max(0.0, g.value("debt", 0.0) - r.prayer.debtDecay);
        if (g.value("bound", -1) == today) w.chronicle("pantheon", {}, "chronicle.pantheon.god_freed", {{"god", id}});
    }

    // 2. favour fades toward nothing; strong favour or wrath shows (moodlets, omens)
    for (auto it = s["favor"].begin(); it != s["favor"].end(); ++it)
        for (auto f = it->begin(); f != it->end(); ++f) {
            const double v = f->get<double>();
            *f = v - v * r.favor.decay;
        }
    for (Character* c : people) {
        const auto gods = godsOf(w, *c);
        if (gods.empty()) continue;
        const double fv = favorOf(w, c->id, gods.front());
        const bool good = fv >= r.favor.moodletAt, bad = fv <= r.favor.wrathAt;
        if (!good && !bad) continue;
        setMoodlet(w, *c, r, good ? r.favor.favorMoodlet : r.favor.wrathMoodlet);
        if (w.rng(c->id, "omen").uniform() < r.favor.omenChance)
            w.chronicle("pantheon", {c->id}, good ? "chronicle.pantheon.omen_favor" : "chronicle.pantheon.omen_wrath", {{"god", gods.front()}, {"who", c->id}});
    }

    // 3. promises due: a god in chains or out of strength breaks them (a debt)
    Json keep = Json::array();
    for (const Json& e : s["pending"]) {
        if (e.value("due", today) > today) {
            keep.push_back(e);
            continue;
        }
        const PrayerAsk p = askFrom(e);
        Json& g = godJson(w, r, p.lik);
        const bool bound = g.value("bound", -1) > today;
        const Character* c = w.character(p.who);
        if (bound || !c || !c->body.alive) {
            g["debt"] = g.value("debt", 0.0) + r.prayer.debtOverdue * std::max(0.3, p.weight);
            w.chronicle("pantheon", {}, "chronicle.pantheon.overdue", {{"god", p.lik}, {"who", p.who}});
            continue;
        }
        if (fulfillHook()) fulfillHook()(w, p, e.value("strength", 0.5));
        g["debt"] = std::max(0.0, g.value("debt", 0.0) - r.prayer.debtRepay);
        w.chronicle("pantheon", {p.who}, "chronicle.pantheon.fulfilled", {{"god", p.lik}, {"who", p.who}, {"aim", p.aim}});
    }
    s["pending"] = keep;

    // 4. a god over its debt broke the law of Exchange (once per debt)
    for (const auto& id : r.likOrder) {
        Json& g = godJson(w, r, id);
        if (g.value("debt", 0.0) >= r.prayer.debtBreachAt && openBreaches(w, kNone, id) == 0) {
            Breach b;
            b.law = "exchange";
            b.god = id;
            b.weight = g.value("debt", 0.0) / std::max(0.01, r.prayer.debtBreachAt);
            b.why = "debt";
            reportBreach(w, b);
        }
    }

    // 5. the laws: the world's record of deeds, the doers' memories; then retribution
    scanFacts(w, r);
    scanMemories(w, r, people, from);
    retribution(w, r);

    // 6. identification and heresy where faiths meet
    for (auto& [sid, ppl] : places) {
        std::map<std::string, std::vector<Character*>> byFaith;
        for (Character* c : ppl) byFaith[faithOf(w, *c)].push_back(c);
        if (byFaith.size() < 2) continue;
        const auto vals = settlementValues(w, sid);
        const double hosp = vals.count("hospitality") ? vals.at("hospitality") : 0.0;
        for (auto a = byFaith.begin(); a != byFaith.end(); ++a)
            for (auto b = std::next(a); b != byFaith.end(); ++b) {
                const FaithDef* fa = cr.faithDef(a->first);
                const FaithDef* fb = cr.faithDef(b->first);
                if (!fa || !fb || a->second.empty() || b->second.empty()) continue;
                const auto la = godsOf(w, *a->second.front());
                const auto lb = godsOf(w, *b->second.front());
                for (const auto& x : la)
                    for (const auto& y : lb) {
                        if (x == y) continue;
                        const LikDef* lx = r.lik(x);
                        const LikDef* ly = r.lik(y);
                        if (!lx || !ly) continue;
                        const std::string key = identKey(x, y);
                        // rivals contend over a shared place (now and then)
                        if (std::find(lx->rivals.begin(), lx->rivals.end(), y) != lx->rivals.end()) {
                            const std::string rk = "rival|" + key + "|" + std::to_string(sid);
                            if (today - s["seen"].value(rk, -100000) >= r.ident.rivalryEveryDays) {
                                s["seen"][rk] = today;
                                w.chronicle("pantheon", {}, "chronicle.pantheon.rivalry", {{"god", x}, {"god2", y}, {"place", placeName(w, sid)}});
                            }
                        }
                        if (lx->power != ly->power) continue;
                        const double open = 0.5 * (fa->tolerance + fb->tolerance) * (1.0 + r.ident.hospitalityMult * hosp);
                        const double mix = std::min(a->second.size(), b->second.size()) / static_cast<double>(ppl.size());
                        const double chance = r.ident.base * std::max(0.0, open) * (0.5 + mix);
                        if (w.rng(sid, "ident_" + key).uniform() >= chance) continue;
                        // zealots of either side may reject it
                        std::vector<Character*> zealots;
                        for (auto* side : {&a->second, &b->second}) {
                            const FaithDef* f = side == &a->second ? fa : fb;
                            if (f->tolerance > r.ident.heresyTolerance) continue;
                            for (Character* c : *side)
                                if (pietyOf(w, *c) >= r.ident.heresyPiety) zealots.push_back(c);
                        }
                        Json& id = s["ident"];
                        const double was = id.value(key, 0.0);
                        if (!zealots.empty() && w.rng(sid, "heresy_" + key).uniform() < r.ident.heresyChance) {
                            id[key] = std::max(0.0, was - r.ident.heresyStep);
                            for (Character* z : zealots) {
                                rememberType(w, *z, "heresy_seen", -0.3, 0.5);
                                setMoodlet(w, *z, r, r.ident.heresyMoodlet);
                                const auto& others = std::find(a->second.begin(), a->second.end(), z) != a->second.end() ? b->second : a->second;
                                for (Character* o : others) z->relations[o->id].enmity += r.ident.heresyEnmity;
                            }
                            const std::string hk = "heresy|" + key + "|" + std::to_string(sid);
                            if (today - s["seen"].value(hk, -100000) >= r.ident.heresyEveryDays) {
                                s["seen"][hk] = today;
                                w.chronicle("pantheon", {}, "chronicle.pantheon.heresy", {{"god", x}, {"god2", y}, {"place", placeName(w, sid)}});
                            }
                            continue;
                        }
                        const double now = std::min(1.0, was + r.ident.step);
                        id[key] = now;
                        if (was < r.ident.at && now >= r.ident.at) {
                            w.chronicle("pantheon", {}, "chronicle.pantheon.identified", {{"god", x}, {"god2", y}, {"place", placeName(w, sid)}});
                            for (Character* c : a->second) rememberType(w, *c, "gods_matched", 0.2, 0.4);
                            for (Character* c : b->second) rememberType(w, *c, "gods_matched", 0.2, 0.4);
                            for (Character* c : a->second)
                                for (Character* o : b->second) {
                                    c->relations[o->id].affinity += r.ident.affinity;
                                    o->relations[c->id].affinity += r.ident.affinity;
                                }
                        }
                    }
            }
    }
}

// --- commands ----------------------------------------------------------------------------------------------
namespace {

bool handlePantheon(World& w, Command& cmd) {
    if (cmd.type != "pray" && cmd.type != "law_breach" && cmd.type != "set_favor") return false;
    const PantheonRules& r = pantheonRules(w.content());
    if (!r.loaded) {
        cmd.result = "rejected:no_pantheon";
        return true;
    }
    const EntityId id = cmd.args.value("id", kNone);
    Character* c = w.character(id);
    if (cmd.type == "pray") {
        if (!c || c->isAnimal()) {
            cmd.result = "rejected:no_such_character";
            return true;
        }
        PrayerAsk p;
        p.who = id;
        p.aim = cmd.args.value("aim", std::string());
        p.lik = cmd.args.value("lik", std::string());
        p.faith = cmd.args.value("faith", std::string());
        p.offering = cmd.args.value("offering", std::string());
        p.rite = cmd.args.value("rite", std::string());
        p.weight = cmd.args.value("weight", 1.0);
        const DivineAnswer a = answerPrayer(w, p);
        cmd.result = std::string("ok:") + answerName(a.kind) + ":" + a.lik;
        return true;
    }
    if (cmd.type == "set_favor") {
        const std::string lik = cmd.args.value("lik", std::string());
        if (!c || !r.lik(lik)) {
            cmd.result = "rejected:bad_args";
            return true;
        }
        const double v = std::clamp(cmd.args.value("favor", 0.0), -1.0, 1.0);
        stateOf(w)["favor"][lik][std::to_string(id)] = v;
        cmd.result = "ok";
        return true;
    }
    Breach b;
    b.law = cmd.args.value("law", std::string());
    b.doer = c ? id : kNone;
    b.god = cmd.args.value("god", std::string());
    b.victim = cmd.args.value("victim", kNone);
    b.weight = cmd.args.value("weight", 1.0);
    b.why = "command";
    if (b.doer == kNone && b.god.empty()) {
        cmd.result = "rejected:no_doer";
        return true;
    }
    cmd.result = reportBreach(w, b) ? "ok" : "rejected:unknown_law";
    return true;
}

} // namespace

void installPantheon(World& w) { w.addCommandHandler(handlePantheon); }

// --- checks -------------------------------------------------------------------------------------------------
void validatePantheon(const Content& c, ValidationReport& rep) {
    const PantheonRules& r = pantheonRules(c);
    for (const auto& e : r.errors) rep.errors.push_back(e);
    if (!r.loaded) return;
    const CultureRules& cr = cultureRules(c);
    std::set<std::string> values(cr.values.begin(), cr.values.end());
    std::set<std::string> aspects;
    const fs::path root = fs::path(c.dir).parent_path();
    if (data::exists(root / "magic" / "aspects.json")) {
        const Json a = data::readJson((root / "magic" / "aspects.json").string());
        const Json arr = a.is_array() ? a : a.value("aspects", Json::array());
        for (const Json& x : arr)
            if (x.is_object() && x.contains("id")) aspects.insert(x["id"].get<std::string>());
    }
    const std::set<std::string> belts(r.belts.begin(), r.belts.end());
    static const std::set<std::string> kConds = {"victim_kin", "guest_or_host", "at_sacred", "near_grave", "initiate"};
    static const std::set<std::string> kTraits = {"wrath", "mercy", "pride", "jealousy", "generosity", "cunning", "aloof"};
    for (const auto& [id, p] : r.powers) {
        for (const auto& a : p.aspects)
            if (!aspects.empty() && !aspects.count(a)) rep.errors.push_back("pantheon.powers." + id + ": unknown aspect " + a);
        for (const auto& l : p.laws)
            if (!r.law(l)) rep.errors.push_back("pantheon.powers." + id + ": unknown law " + l);
        for (const auto& [v, x] : p.values)
            if (!values.empty() && !values.count(v)) rep.errors.push_back("pantheon.powers." + id + ": unknown value " + v);
    }
    for (const auto& [id, l] : r.liks) {
        const std::string where = "pantheon.liks." + id;
        if (!r.power(l.power)) rep.errors.push_back(where + ": unknown power " + l.power);
        for (const auto& p : l.also)
            if (!r.power(p)) rep.errors.push_back(where + ": unknown power " + p);
        if (!belts.count(l.belt)) rep.errors.push_back(where + ": unknown belt " + l.belt);
        for (const auto& [k, v] : l.character) {
            if (!kTraits.count(k)) rep.errors.push_back(where + ": unknown character trait " + k);
            if (v < 0.0 || v > 1.0) rep.errors.push_back(where + ": character " + k + " out of 0..1");
        }
        for (const auto& o : l.rivals)
            if (!r.lik(o)) rep.errors.push_back(where + ": unknown rival " + o);
        for (const auto& o : l.allies)
            if (!r.lik(o)) rep.errors.push_back(where + ": unknown ally " + o);
    }
    for (const auto& [id, law] : r.laws) {
        for (const auto& p : law.watchers)
            if (!r.power(p)) rep.errors.push_back("laws." + id + ": unknown power " + p);
        for (const auto& fr : law.facts)
            for (const auto& w : fr.when)
                if (!kConds.count(w)) rep.errors.push_back("laws." + id + ": unknown condition " + w);
    }
    for (const char* need : {"hospitality", "oath", "exchange", "rest_dead", "suppliant", "kin_blood", "inviolable_will", "mystery"})
        if (!r.law(need)) rep.errors.push_back(std::string("laws: missing world law ") + need);
    // the faiths: power and liks
    for (const auto& [fid, f] : cr.faiths) {
        const Json* lj = faithLiksJson(c, fid);
        if (!lj || lj->empty()) {
            rep.errors.push_back("religions." + fid + ": no liks (LR-169)");
            continue;
        }
        for (auto it = lj->begin(); it != lj->end(); ++it) {
            if (it.key() != "*" && !belts.count(it.key())) rep.errors.push_back("religions." + fid + ": unknown belt " + it.key());
            for (const auto& x : it->is_array() ? *it : Json::array())
                if (!x.is_string() || !r.lik(x.get<std::string>())) rep.errors.push_back("religions." + fid + ": unknown lik " + x.dump());
        }
        if (faithLiks(c, fid, "").empty()) rep.errors.push_back("religions." + fid + ": no \"*\" liks (for a culture without a belt)");
    }
    for (const auto& [id, cd] : c.peoples.cultures) {
        const std::string b = cultureBelt(c, id);
        if (!b.empty() && !belts.count(b)) rep.errors.push_back("cultures." + id + ": unknown belt " + b);
    }
    for (const auto& m : {r.favor.favorMoodlet, r.favor.wrathMoodlet, r.prayer.grantedMoodlet, r.prayer.refusedMoodlet, r.retribution.curseMoodlet, r.ident.heresyMoodlet})
        if (!m.empty() && !r.moodlets.count(m) && !c.moodlets.count(m)) rep.errors.push_back("pantheon rules: unknown moodlet " + m);
}

std::vector<std::string> pantheonLocKeys(const Content& c) {
    std::vector<std::string> k;
    const PantheonRules& r = pantheonRules(c);
    if (!r.loaded) return k;
    for (const auto& b : r.belts) k.push_back("belt." + b + ".name");
    for (const auto& [id, p] : r.powers) {
        k.push_back("power." + id + ".name");
        k.push_back("power." + id + ".desc");
        for (const auto& a : p.aims) k.push_back("pantheon.aim." + a + ".name");
    }
    for (const auto& [id, l] : r.liks) {
        k.push_back("god." + id + ".name");
        for (const auto& e : l.epithets) k.push_back("god." + id + ".epithet." + e + ".name");
    }
    for (const auto& [id, l] : r.laws) {
        k.push_back("law." + id + ".name");
        k.push_back("law." + id + ".desc");
    }
    for (const auto& [id, m] : r.moodlets) k.push_back("moodlet." + id + ".name");
    for (const char* a : {"granted", "refused", "deferred", "silent"}) k.push_back(std::string("pantheon.answer.") + a + ".name");
    for (const char* a : {"breaks_law", "against_fate", "no_favor", "bound_elsewhere", "outside_domain", "bound"})
        k.push_back(std::string("pantheon.refuse.") + a + ".name");
    for (const char* a : {"standing", "bound", "domain"}) k.push_back(std::string("pantheon.penalty.") + a + ".name");
    for (const char* ck : {"granted", "refused", "deferred", "silent", "fulfilled", "overdue", "breach", "punished", "settlement_cursed",
                           "god_breach", "god_punished", "god_bound", "god_freed", "domain_lost", "identified", "heresy", "rivalry",
                           "omen_favor", "omen_wrath"})
        k.push_back(std::string("chronicle.pantheon.") + ck);
    for (const auto& [fid, f] : cultureRules(c).faiths) {
        k.push_back("faith." + fid + ".name");
        k.push_back("faith." + fid + ".desc");
    }
    return k;
}

} // namespace lr::sim
