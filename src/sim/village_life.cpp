// Village life (village_life.hpp, AL-39).

#include "sim/village_life.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

#include "sim/ai.hpp"
#include "sim/captivity.hpp"
#include "sim/worldgen.hpp"

namespace fs = std::filesystem;

namespace altera::sim {

namespace {

const char* kSource = "village_life";

double num(const Json& j, const char* k, double v) { return j.contains(k) && j[k].is_number() ? j[k].get<double>() : v; }
int inum(const Json& j, const char* k, int v) { return static_cast<int>(num(j, k, v)); }
std::array<int, 2> ipair(const Json& j, const char* k, std::array<int, 2> v) {
    if (j.contains(k) && j[k].is_array() && j[k].size() == 2) return {j[k][0].get<int>(), j[k][1].get<int>()};
    return v;
}
std::array<double, 2> dpair(const Json& j, const char* k, std::array<double, 2> v) {
    if (j.contains(k) && j[k].is_array() && j[k].size() == 2) return {j[k][0].get<double>(), j[k][1].get<double>()};
    return v;
}
Tick hoursTtl(const Json& j, Tick v) { return j.contains("ttl_hours") ? static_cast<Tick>(num(j, "ttl_hours", 24) * kTicksPerDay / 24) : v; }

void loadRules(VillageLifeRules& r, const Json& j) {
    r.loaded = true;
    const Json co = j.value("courtship", Json::object());
    r.minAgeYears = inum(co, "min_age_years", r.minAgeYears);
    r.maxAgeGapYears = inum(co, "max_age_gap_years", r.maxAgeGapYears);
    r.sameSex = co.value("same_sex", r.sameSex);
    r.courtDaily = num(co, "daily_chance", r.courtDaily);
    if (co.contains("acts")) r.courtActs = co["acts"].get<std::vector<std::string>>();
    r.flirtFromFamiliarity = num(co, "flirt_from_familiarity", r.flirtFromFamiliarity);
    r.spark = dpair(co, "spark", r.spark);
    r.interestMin = num(co, "interest_min", r.interestMin);
    r.proposeAct = co.value("propose_act", r.proposeAct);
    r.proposeInterest = num(co, "propose_interest", r.proposeInterest);
    r.acceptAffinity = num(co, "accept_affinity", r.acceptAffinity);
    r.acceptAttraction = num(co, "accept_attraction", r.acceptAttraction);
    r.refusedCooldownDays = inum(co, "refused_cooldown_days", r.refusedCooldownDays);
    r.wedAct = co.value("wed_act", r.wedAct);
    r.weddingAfterDays = inum(co, "wedding_after_days", r.weddingAfterDays);
    r.residence = co.value("residence", r.residence);
    r.takeFamilyName = co.value("take_family_name", r.takeFamilyName);
    r.courtPriority = num(co, "priority", r.courtPriority);
    r.courtTtl = hoursTtl(co, r.courtTtl);
    const Json ma = j.value("marriage", Json::object());
    r.intimacyAct = ma.value("intimacy_act", r.intimacyAct);
    r.intimacyDaily = num(ma, "intimacy_daily", r.intimacyDaily);
    r.marriagePriority = num(ma, "priority", r.marriagePriority);
    r.marriageTtl = hoursTtl(ma, r.marriageTtl);
    const Json cn = j.value("conception", Json::object());
    if (cn.contains("acts")) r.conceptionActs = cn["acts"].get<std::set<std::string>>();
    r.conceiveChance = num(cn, "chance", r.conceiveChance);
    r.fertileF = ipair(cn, "fertile_f_years", r.fertileF);
    r.fertileM = ipair(cn, "fertile_m_years", r.fertileM);
    r.fadeFromYears = inum(cn, "age_fade_from_years", r.fadeFromYears);
    r.pregnancyDays = std::max(1, inum(cn, "pregnancy_days", r.pregnancyDays));
    r.birthSpacingDays = inum(cn, "birth_spacing_days", r.birthSpacingDays);
    r.startPregnantShare = num(cn, "start_pregnant_share", r.startPregnantShare);
    r.twins = num(cn, "twins", r.twins);
    r.stillbirth = num(cn, "stillbirth", r.stillbirth);
    r.motherDeath = num(cn, "mother_death", r.motherDeath);
    r.traitInherit = num(cn, "trait_inherit", r.traitInherit);
    r.traitsMax = inum(cn, "traits_max", r.traitsMax);
    r.attrNoise = num(cn, "attr_noise", r.attrNoise);
    const Json qu = j.value("quarrels", Json::object());
    if (qu.contains("acts")) {
        r.quarrelActs.clear();
        for (const auto& a : qu["acts"]) r.quarrelActs.push_back({a.value("act", std::string()), num(a, "from", 0.0)});
        std::sort(r.quarrelActs.begin(), r.quarrelActs.end(),
                  [](const auto& a, const auto& b) { return a.from < b.from; });
    }
    r.quarrelDaily = num(qu, "daily_chance", r.quarrelDaily);
    r.wEnmity = num(qu, "enmity", r.wEnmity);
    r.wDistrust = num(qu, "distrust", r.wDistrust);
    r.wDislike = num(qu, "dislike", r.wDislike);
    r.quarrelCooldownDays = inum(qu, "cooldown_days", r.quarrelCooldownDays);
    r.quarrelPriority = num(qu, "priority", r.quarrelPriority);
    r.quarrelTtl = hoursTtl(qu, r.quarrelTtl);
    const Json il = j.value("illness", Json::object());
    r.illDaily = num(il, "daily_base", r.illDaily);
    r.contagion = num(il, "contagion_household", r.contagion);
    if (il.contains("mult")) r.illMult = il["mult"].get<std::map<std::string, double>>();
    r.immuneDays = inum(il, "immune_days", r.immuneDays);
    for (const auto& k : il.value("kinds", Json::array())) {
        IllnessKind x;
        x.id = k.value("id", std::string());
        x.moodlet = k.value("moodlet", "ill_" + x.id);
        x.weight = num(k, "weight", x.weight);
        x.days = ipair(k, "days", x.days);
        x.healthPerDay = num(k, "health_per_day", x.healthPerDay);
        r.illnesses.push_back(x);
    }
    const Json oa = j.value("old_age", Json::object());
    r.oldFromYears = inum(oa, "from_years", r.oldFromYears);
    r.oldDailyAtFrom = num(oa, "daily_at_from", r.oldDailyAtFrom);
    r.oldDoubleYears = std::max(0.5, num(oa, "double_every_years", r.oldDoubleYears));
    const Json st = j.value("strangers", Json::object());
    r.strangerDaily = num(st, "daily_chance", r.strangerDaily);
    r.strangerNoSingles = num(st, "daily_no_singles", r.strangerNoSingles);
    r.loneAge = ipair(st, "lone_age_years", r.loneAge);
    r.strangerMaxPeople = inum(st, "max_people", r.strangerMaxPeople);
    r.strangerFoodDays = num(st, "food_days", r.strangerFoodDays);
    r.strangerCouple = num(st, "couple", r.strangerCouple);
    r.strangerChild = num(st, "child_with_couple", r.strangerChild);
    r.strangerAge = ipair(st, "age_years", r.strangerAge);
    r.takenInRole = st.value("taken_in_role", r.takenInRole);
    if (st.contains("household_kit")) r.householdKit = st["household_kit"].get<std::map<std::string, int>>();
    r.strangerSkill = dpair(st, "skill_range", r.strangerSkill);
    r.arriveM = num(st, "arrive_m", r.arriveM);
}

const VillageLifeRules& R(const World& w) { return villageLifeRules(w.content()); }

int years(const Character& c) { return c.body.ageDays / kDaysPerYear; }
bool person(const Character* c) { return c && c->body.alive && !c->isAnimal(); }
bool idleBody(const Character& c) { return !c.captivity.held(); }

const Character* aliveSpouse(const World& w, const Character& c) {
    const Character* s = c.status.spouse == kNone ? nullptr : w.character(c.status.spouse);
    return person(s) ? s : nullptr;
}

EntityId betrothedTo(const Character& c) {
    for (const auto& [other, ids] : c.status.pairs)
        if (ids.count("betrothed")) return other;
    return kNone;
}

void setPair(World& w, Character& a, Character& b, const std::string& id, bool on) {
    auto one = [&](Character& x, Character& y) {
        if (on) x.status.pairs[y.id].insert(id);
        else if (auto it = x.status.pairs.find(y.id); it != x.status.pairs.end()) {
            it->second.erase(id);
            if (it->second.empty()) x.status.pairs.erase(it);
        }
        if (id == "married") x.status.spouse = on ? y.id : (x.status.spouse == y.id ? kNone : x.status.spouse);
    };
    one(a, b);
    one(b, a);
    (void)w;
}

void memo(World& w, Character& c, const std::string& type, EntityId about, double valence, double salience = 0.0) {
    MemoryEntry m;
    m.type = type;
    if (about != kNone) m.who = {about};
    m.valence = valence;
    m.salience = salience;
    w.remember(c, std::move(m));
}

// The last tick `c` remembers `type` (about `who` when set); -1: never.
Tick lastMemory(const Character& c, const std::string& type, EntityId who = kNone) {
    Tick t = -1;
    for (const auto& m : c.memory)
        if (m.type == type && (who == kNone || (!m.who.empty() && m.who[0] == who))) t = std::max(t, m.tick);
    return t;
}

void moodlet(World& w, Character& c, const std::string& id, Tick until = -1) {
    auto md = w.content().moodlets.find(id);
    const double value = md == w.content().moodlets.end() ? 0.0 : md->second.value;
    if (until < 0) until = w.now() + (md == w.content().moodlets.end() ? 72 : md->second.ttl);
    for (auto& m : c.moodlets)
        if (m.id == id) {
            m.until = std::max(m.until, until);
            return;
        }
    c.moodlets.push_back({id, value, until});
}

void dropMoodlet(Character& c, const std::string& id) {
    c.moodlets.erase(std::remove_if(c.moodlets.begin(), c.moodlets.end(), [&](const Moodlet& m) { return m.id == id; }),
                     c.moodlets.end());
}

void die(World& w, Character& c, const std::string& cause) {
    if (!c.body.alive) return;
    c.body.alive = false;
    c.body.health = 0.0;
    c.body.causeOfDeath = cause;
    w.chronicle("death", {c.id}, "chronicle.death", {{"who", c.id}, {"cause", cause}});
}

bool queued(const Character& c, const std::string& act, EntityId target) {
    for (const auto& q : c.ai.queue)
        if (q.task == act && q.target == target) return true;
    return false;
}

bool queuedByUs(const Character& c, const InteractionDef& d, EntityId target) {
    const std::string& base = d.baseId.empty() ? d.id : d.baseId;
    for (const auto& q : c.ai.queue)
        if ((q.task == d.id || q.task == base) && q.target == target && q.source == kSource) return true;
    return false;
}

// A person's own life: the player's person is asked, never made to.
bool queueAct(World& w, Character& c, const std::string& act, EntityId target, double prio, Tick ttl,
              const std::string& why) {
    if (c.controller.isPlayer() || !w.content().interaction(act) || queued(c, act, target)) return false;
    c.ai.queue.push_back(QueuedTask{act, target, prio, w.now(), ttl, why, kSource});
    return true;
}

double spark(const World& w, EntityId a, EntityId b, const VillageLifeRules& r) {
    Rng rr = agentRng(w.seed(), std::min(a, b), static_cast<Tick>(std::max(a, b)), "spark");
    return rr.range(r.spark[0], r.spark[1]);
}

// Could `c` marry at all now (age, alive, free).
bool marriageable(const World& w, const Character& c, const VillageLifeRules& r) {
    return person(&c) && years(c) >= r.minAgeYears && !aliveSpouse(w, c) && idleBody(c);
}

bool fertile(const Character& c, const VillageLifeRules& r) {
    const int y = years(c);
    const auto& f = c.body.sex == 'f' ? r.fertileF : r.fertileM;
    return person(&c) && y >= f[0] && y <= f[1];
}

void moveToFamily(World& w, Character& c, EntityId famId, const std::string& role) {
    if (c.status.family == famId) return;
    if (auto old = w.families().find(c.status.family); old != w.families().end()) {
        auto& m = old->second.members;
        m.erase(std::remove(m.begin(), m.end(), c.id), m.end());
    }
    auto nf = w.families().find(famId);
    if (nf == w.families().end()) return;
    nf->second.members.push_back(c.id);
    c.status.family = famId;
    c.status.role = role;
    c.status.settlement = nf->second.settlement;
    c.home = nf->second.storage;
    for (EntityId id : nf->second.members) {
        if (id == c.id) continue;
        Character* m = w.character(id);
        if (!m) continue;
        Relation& a = c.relations[id];
        Relation& b = m->relations[c.id];
        a.familiarity = std::max(a.familiarity, 0.6);
        b.familiarity = std::max(b.familiarity, 0.6);
        a.affinity = std::max(a.affinity, 0.2);
        b.affinity = std::max(b.affinity, 0.2);
    }
    for (const auto& [oid, o] : w.objects())
        if (o.ownerFamily == famId) c.knownObjects.insert(oid);
}

std::string withFamilyName(const std::string& name, const std::string& family) {
    const auto sp = name.find(' ');
    return (sp == std::string::npos ? name : name.substr(0, sp)) + " " + family;
}

// --- courtship and marriage --------------------------------------------------

double interest(const World& w, const Character& a, const Character& b, const VillageLifeRules& r) {
    auto it = a.relations.find(b.id);
    const double att = it == a.relations.end() ? 0.0 : it->second.attraction;
    const double aff = it == a.relations.end() ? 0.0 : it->second.affinity;
    return att + 0.5 * aff + spark(w, a.id, b.id, r);
}

bool suitable(const World& w, const Character& a, const Character& b, const VillageLifeRules& r) {
    if (a.id == b.id || !marriageable(w, b, r) || betrothedTo(b) != kNone) return false;
    if (!r.sameSex && a.body.sex == b.body.sex) return false;
    if (std::abs(a.body.ageDays - b.body.ageDays) > r.maxAgeGapYears * kDaysPerYear) return false;
    if (a.status.settlement != b.status.settlement) return false;
    if (kinOrHousehold(w, a, b)) return false;
    const Tick refused = lastMemory(a, "proposal_refused", b.id);
    if (refused >= 0 && w.now() - refused < static_cast<Tick>(r.refusedCooldownDays) * kTicksPerDay) return false;
    return true;
}

// The one of a couple who leads them to the wedding: an AI groom, else an AI bride.
const Character* weddingLeader(const Character& a, const Character& b) {
    const Character* first = a.body.sex == 'm' ? &a : (b.body.sex == 'm' ? &b : (a.id < b.id ? &a : &b));
    const Character* second = first == &a ? &b : &a;
    if (!first->controller.isPlayer() && !second->controller.isPlayer()) return first;
    return nullptr; // a player's person goes to the altar of their own will
}

void courtDaily(World& w, Character& c, const VillageLifeRules& r) {
    if (c.controller.isPlayer() || !marriageable(w, c, r)) return;
    Rng rng = w.rng(c.id, "village:court");
    if (const EntityId bid = betrothedTo(c); bid != kNone) {
        Character* b = w.character(bid);
        if (!person(b)) {
            if (b) setPair(w, c, *b, "betrothed", false);
            return;
        }
        const Tick since = lastMemory(c, "betrothed", bid);
        if (since >= 0 && w.now() - since < static_cast<Tick>(r.weddingAfterDays) * kTicksPerDay) return;
        if (weddingLeader(c, *b) == &c)
            queueAct(w, c, r.wedAct, bid, r.courtPriority + 0.15, r.courtTtl, "village.wedding");
        return;
    }
    const EntityId tid = courtshipTarget(w, c);
    if (tid == kNone) return;
    const Character& t = *w.character(tid);
    const Relation& to = c.relations[tid];
    auto back = t.relations.find(c.id);
    const double backAff = back == t.relations.end() ? 0.0 : back->second.affinity;
    const double backAtt = back == t.relations.end() ? 0.0 : back->second.attraction;
    if (interest(w, c, t, r) >= r.proposeInterest && backAff >= r.acceptAffinity && backAtt >= r.acceptAttraction &&
        !t.controller.isPlayer()) {
        queueAct(w, c, r.proposeAct, tid, r.courtPriority + 0.1, r.courtTtl, "village.propose");
        return;
    }
    if (!rng.chance(r.courtDaily) || r.courtActs.empty()) return;
    std::string act = r.courtActs[static_cast<size_t>(rng.irange(0, static_cast<int>(r.courtActs.size()) - 1))];
    if (to.familiarity < r.flirtFromFamiliarity) act = r.courtActs.front();
    queueAct(w, c, act, tid, r.courtPriority, r.courtTtl, "village.court");
}

void marriedDaily(World& w, Character& c, const VillageLifeRules& r) {
    const Character* s = aliveSpouse(w, c);
    if (!s || c.controller.isPlayer() || s->controller.isPlayer()) return;
    // one of the pair (the wife) decides for the night; the husband acts
    const Character& wife = c.body.sex == 'f' ? c : *s;
    const Character& husband = c.body.sex == 'f' ? *s : c;
    if (wife.id != c.id || wife.body.sex != 'f' || husband.body.sex != 'm') return;
    if (!fertile(wife, r) || !fertile(husband, r) || isPregnant(w, wife)) return;
    Rng rng = w.rng(c.id, "village:intimacy");
    if (!rng.chance(r.intimacyDaily)) return;
    queueAct(w, *w.character(husband.id), r.intimacyAct, wife.id, r.marriagePriority, r.marriageTtl, "village.spouses");
}

void betroth(World& w, Character& a, Character& b) {
    setPair(w, a, b, "betrothed", true);
    memo(w, a, "betrothed", b.id, 0.6, 0.8);
    memo(w, b, "betrothed", a.id, 0.6, 0.8);
    moodlet(w, a, "in_love");
    moodlet(w, b, "in_love");
    w.chronicle("betrothal", {a.id, b.id}, "chronicle.village.betrothal", {{"a", a.id}, {"b", b.id}});
}

void wed(World& w, Character& a, Character& b, const VillageLifeRules& r) {
    setPair(w, a, b, "betrothed", false);
    if (!a.status.hasPair(b.id, "married")) setPair(w, a, b, "married", true);
    // who moves: the bride (patrilocal) or the groom (matrilocal); one
    // without a household always goes to the other's
    Character* mover = &b;
    if (a.body.sex != b.body.sex) {
        const bool aWoman = a.body.sex == 'f';
        const bool brideMoves = r.residence != "matrilocal";
        mover = (aWoman == brideMoves) ? &a : &b;
    }
    Character* stayer = mover == &a ? &b : &a;
    if (!w.families().count(stayer->status.family)) std::swap(mover, stayer);
    const EntityId fam = stayer->status.family;
    if (w.families().count(fam) && mover->status.family != fam) {
        moveToFamily(w, *mover, fam, stayer->status.role == "head" ? "spouse" : "kin");
        if (r.takeFamilyName) mover->name = withFamilyName(mover->name, w.families()[fam].name);
    }
    memo(w, a, "married_to", b.id, 0.7, 0.9);
    memo(w, b, "married_to", a.id, 0.7, 0.9);
    moodlet(w, a, "in_love");
    moodlet(w, b, "in_love");
    w.chronicle("wedding", {a.id, b.id}, "chronicle.village.wedding",
                {{"a", a.id}, {"b", b.id}, {"family", fam}, {"moved", mover->id}});
}

// --- conception and birth ----------------------------------------------------

void startPregnancy(World& w, Character& mother, EntityId father, Tick due) {
    moodlet(w, mother, "pregnant", due + 1);
    memo(w, mother, "with_child", father, 0.4, 0.8);
    Event e;
    e.tick = due;
    e.type = "village_birth";
    e.a = mother.id;
    e.b = father;
    w.schedule(e);
}

bool canConceive(const World& w, const Character& woman, const Character& man, const VillageLifeRules& r) {
    if (woman.body.sex != 'f' || man.body.sex != 'm' || !fertile(woman, r) || !fertile(man, r)) return false;
    if (isPregnant(w, woman)) return false;
    const Tick born = lastMemory(woman, "gave_birth");
    return born < 0 || w.now() - born >= static_cast<Tick>(r.birthSpacingDays) * kTicksPerDay;
}

void tryConceive(World& w, Character& a, Character& b, const VillageLifeRules& r) {
    Character& woman = a.body.sex == 'f' ? a : b;
    Character& man = a.body.sex == 'f' ? b : a;
    if (&woman == &man || !canConceive(w, woman, man, r)) return;
    Rng rng = w.rng(woman.id, "village:conceive");
    if (!rng.chance(conceiveChanceOf(woman, r))) return;
    startPregnancy(w, woman, man.id, w.now() + static_cast<Tick>(r.pregnancyDays) * kTicksPerDay);
    const bool wed = woman.status.spouse == man.id;
    w.chronicle("pregnancy", {woman.id, man.id}, "chronicle.village.pregnancy",
                {{"mother", woman.id}, {"father", man.id}, {"wedlock", wed}});
}

std::vector<std::string> inheritTraits(Rng& rng, const Content& ct, const Character* m, const Character* f,
                                       const VillageLifeRules& r) {
    std::vector<std::string> out;
    for (const Character* p : {m, f}) {
        if (!p) continue;
        for (const auto& t : p->traits) {
            if (static_cast<int>(out.size()) >= r.traitsMax) break;
            if (std::find(out.begin(), out.end(), t) != out.end() || !rng.chance(r.traitInherit)) continue;
            bool clash = false;
            for (const auto& [x, y] : ct.exclusiveTraits)
                for (const auto& o : out) clash |= (x == t && y == o) || (y == t && x == o);
            if (!clash) out.push_back(t);
        }
    }
    return out;
}

EntityId bear(World& w, Character& mother, Character* father, Rng& rng, const VillageLifeRules& r) {
    const Content& ct = w.content();
    Character k;
    k.id = w.newId();
    k.body.sex = rng.chance(0.5) ? 'f' : 'm';
    k.body.ageDays = 0;
    const auto fam = w.families().find(mother.status.family);
    const std::string famName = fam != w.families().end() ? fam->second.name : std::string();
    k.name = randomPersonName(rng, k.body.sex == 'f', &ct) + (famName.empty() ? "" : " " + famName);
    k.traits = inheritTraits(rng, ct, &mother, father, r);
    for (const auto& n : ct.needs) k.needs[n.id] = n.startHi;
    for (const auto& [a, v] : mother.attrs) {
        const double fv = father && father->attrs.count(a) ? father->attrs.at(a) : v;
        k.attrs[a] = clamp01(0.5 * (v + fv) + rng.range(-r.attrNoise, r.attrNoise));
    }
    const bool wedlock = father && mother.status.spouse == father->id;
    k.status.family = mother.status.family;
    k.status.dynasty = wedlock && father->status.dynasty != kNone ? father->status.dynasty : mother.status.dynasty;
    k.status.settlement = mother.status.settlement;
    k.status.role = "child";
    k.status.mother = mother.id;
    k.status.father = father ? father->id : kNone;
    k.pos = mother.pos;
    k.home = mother.home;
    k.knownObjects = mother.knownObjects;
    Character& child = w.addCharacter(std::move(k));
    const EntityId cid = child.id;
    if (fam != w.families().end()) fam->second.members.push_back(cid);
    // (pointers into the map stay valid: std::map nodes do not move)
    Character& c = *w.character(cid);
    for (EntityId id : fam != w.families().end() ? fam->second.members : std::vector<EntityId>{mother.id}) {
        if (id == cid) continue;
        Character* m = w.character(id);
        if (!m) continue;
        const bool parent = id == mother.id || (father && id == father->id);
        Relation& to = m->relations[cid];
        Relation& from = c.relations[id];
        to.familiarity = from.familiarity = 1.0;
        to.affinity = parent ? 0.85 : 0.5;
        from.affinity = parent ? 0.8 : 0.5;
        to.trust = from.trust = parent ? 0.8 : 0.5;
    }
    if (father && father->status.family != c.status.family) {
        Relation& to = father->relations[cid];
        to.familiarity = 0.5;
        to.affinity = 0.5;
    }
    mother.status.children.push_back(cid);
    if (father) father->status.children.push_back(cid);
    return cid;
}

bool onBirth(World& w, const Event& e) {
    if (e.type != "village_birth") return false;
    const VillageLifeRules& r = R(w);
    Character* mother = w.character(e.a);
    if (!mother) return true;
    dropMoodlet(*mother, "pregnant");
    if (!mother->body.alive) return true; // the child died with her
    Character* father = e.b == kNone ? nullptr : w.character(e.b);
    Rng rng = w.rng(mother->id, "village:birth");
    memo(w, *mother, "gave_birth", kNone, 0.5, 0.8);
    if (rng.chance(r.stillbirth)) {
        moodlet(w, *mother, "grieving");
        if (father && father->body.alive) moodlet(w, *father, "grieving");
        w.chronicle("stillbirth", {mother->id}, "chronicle.village.stillbirth",
                    {{"mother", mother->id}, {"father", e.b}});
    } else {
        const int n = rng.chance(r.twins) ? 2 : 1;
        for (int i = 0; i < n; ++i) {
            const EntityId cid = bear(w, *mother, father, rng, r);
            mother = w.character(e.a);
            father = e.b == kNone ? nullptr : w.character(e.b);
            const Character* child = w.character(cid);
            memo(w, *mother, "child_born", cid, 0.8, 0.9);
            moodlet(w, *mother, "new_parent");
            if (father && father->body.alive) {
                memo(w, *father, "child_born", cid, 0.8, 0.9);
                moodlet(w, *father, "new_parent");
            }
            w.chronicle("birth", {cid, mother->id, e.b}, "chronicle.village.birth",
                        {{"child", cid}, {"mother", mother->id}, {"father", e.b}, {"sex", std::string(1, child->body.sex)},
                         {"dynasty", child->status.dynasty}, {"twin", n > 1}});
        }
    }
    if (rng.chance(r.motherDeath)) die(w, *mother, "childbirth");
    return true;
}

// --- deaths, widows, the head of a household ---------------------------------

void widowsAndHeads(World& w) {
    for (auto& [id, c] : w.characters()) {
        if (!person(&c) || c.status.spouse == kNone) continue;
        Character* s = w.character(c.status.spouse);
        if (s && s->body.alive) continue;
        const EntityId dead = c.status.spouse;
        if (s) setPair(w, c, *s, "married", false);
        c.status.spouse = kNone;
        memo(w, c, "widowed", dead, -0.7, 0.9);
        moodlet(w, c, "grieving");
    }
    for (auto& [fid, f] : w.families()) {
        const Character* deadHead = nullptr;
        bool hasHead = false;
        for (EntityId id : f.members) {
            const Character* m = w.character(id);
            if (!m || m->isAnimal() || m->status.role != "head") continue;
            if (m->body.alive) hasHead = true;
            else if (!deadHead) deadHead = m;
        }
        if (hasHead) continue;
        Character* heir = nullptr;
        if (deadHead)
            if (Character* h = w.character(heirOf(w, *deadHead)); h && h->status.family == fid) heir = h;
        if (!heir)
            for (EntityId id : f.members) {
                Character* m = w.character(id);
                if (person(m) && m->body.stage() != "child" && (!heir || m->body.ageDays > heir->body.ageDays)) heir = m;
            }
        if (!heir) continue;
        heir->status.role = "head";
        if (deadHead) {
            // the dead keep their place in the record, not the headship
            if (Character* d = w.character(deadHead->id)) d->status.role = "kin";
        }
        w.chronicle("new_head", {heir->id}, "chronicle.village.new_head",
                    {{"who", heir->id}, {"family", fid}, {"after", deadHead ? deadHead->id : kNone}});
    }
}

// --- illness and old age -----------------------------------------------------

const IllnessKind* illnessOf(const World& w, const Character& c, const VillageLifeRules& r) {
    for (const auto& k : r.illnesses)
        if (c.hasMoodlet(k.moodlet, w.now())) return &k;
    return nullptr;
}

void bodyDaily(World& w, Character& c, const VillageLifeRules& r, const std::map<EntityId, int>& sickIn) {
    if (!person(&c)) return;
    const std::string stage = c.body.stage();
    if (const IllnessKind* k = illnessOf(w, c, r)) {
        const double mult = stage == "adult" ? 1.0 : 1.3;
        c.body.health = clamp01(c.body.health - k->healthPerDay * mult);
        if (c.body.health <= 0.0) die(w, c, "illness");
        return;
    }
    // old age
    if (years(c) >= r.oldFromYears) {
        const double p = r.oldDailyAtFrom * std::pow(2.0, (years(c) - r.oldFromYears) / r.oldDoubleYears);
        Rng ro = w.rng(c.id, "village:old_age");
        if (ro.chance(p)) return die(w, c, "old_age");
    }
    if (r.illnesses.empty()) return;
    const Tick last = lastMemory(c, "fell_ill");
    if (last >= 0 && w.now() - last < static_cast<Tick>(r.immuneDays) * kTicksPerDay) return;
    auto m = [&](const char* k) {
        auto it = r.illMult.find(k);
        return it == r.illMult.end() ? 1.0 : it->second;
    };
    double p = r.illDaily * m(w.season().c_str());
    if (stage == "child") p *= m("child");
    if (stage == "elder") p *= m("elder");
    if (c.need("hunger") < 0.25) p *= m("hungry");
    if (c.need("warmth") < 0.3) p *= m("cold");
    if (auto it = sickIn.find(c.status.family); it != sickIn.end()) p += r.contagion * it->second;
    Rng rng = w.rng(c.id, "village:illness");
    if (!rng.chance(p)) return;
    double total = 0.0;
    for (const auto& k : r.illnesses) total += std::max(0.0, k.weight);
    double x = rng.uniform() * total;
    const IllnessKind* kind = &r.illnesses.back();
    for (const auto& k : r.illnesses)
        if ((x -= std::max(0.0, k.weight)) <= 0.0) {
            kind = &k;
            break;
        }
    const int days = rng.irange(kind->days[0], kind->days[1]);
    moodlet(w, c, kind->moodlet, w.now() + static_cast<Tick>(days) * kTicksPerDay);
    memo(w, c, "fell_ill", kNone, -0.4, 0.5);
    w.chronicle("illness", {c.id}, "chronicle.village.illness", {{"who", c.id}, {"illness", kind->id}, {"days", days}});
}

// --- quarrels ----------------------------------------------------------------

void quarrelDaily(World& w, Character& c, const VillageLifeRules& r) {
    if (c.controller.isPlayer() || !person(&c) || c.body.stage() == "child" || r.quarrelActs.empty()) return;
    EntityId worst = kNone;
    double grudge = 0.0;
    for (const auto& [oid, rel] : c.relations) {
        const Character* o = w.character(oid);
        if (!person(o) || o->status.settlement != c.status.settlement || o->body.stage() == "child") continue;
        const double g = r.wEnmity * rel.enmity + r.wDistrust * std::max(0.0, -rel.trust) +
                         r.wDislike * std::max(0.0, -rel.affinity);
        if (g > grudge) grudge = g, worst = oid;
    }
    if (worst == kNone || grudge < r.quarrelActs.front().from) return;
    const Tick last = lastMemory(c, "quarrel", worst);
    if (last >= 0 && w.now() - last < static_cast<Tick>(r.quarrelCooldownDays) * kTicksPerDay) return;
    Rng rng = w.rng(c.id, "village:quarrel");
    if (!rng.chance(r.quarrelDaily * std::min(1.0, grudge))) return;
    std::string act = r.quarrelActs.front().act;
    for (const auto& q : r.quarrelActs)
        if (grudge >= q.from) act = q.act;
    queueAct(w, c, act, worst, r.quarrelPriority, r.quarrelTtl, "village.quarrel");
}

// --- strangers -----------------------------------------------------------------

Character makeStranger(World& w, Rng& rng, const Settlement& st, char sex, int ageYears, const std::string& surname,
                       const VillageLifeRules& r) {
    const Content& ct = w.content();
    Character c;
    c.id = w.newId();
    c.body.sex = sex;
    c.body.ageDays = ageYears * kDaysPerYear + rng.irange(0, kDaysPerYear - 1);
    c.name = randomPersonName(rng, sex == 'f', &ct) + " " + surname;
    std::vector<std::string> pool;
    for (const auto& [id, t] : ct.traits) pool.push_back(id);
    for (int i = 0; i < 2 && !pool.empty(); ++i) {
        const std::string& t = pool[static_cast<size_t>(rng.irange(0, static_cast<int>(pool.size()) - 1))];
        bool clash = std::find(c.traits.begin(), c.traits.end(), t) != c.traits.end();
        for (const auto& [x, y] : ct.exclusiveTraits)
            for (const auto& o : c.traits) clash |= (x == t && y == o) || (y == t && x == o);
        if (!clash) c.traits.push_back(t);
    }
    for (const auto& n : ct.needs) c.needs[n.id] = rng.range(n.startLo, n.startHi);
    if (c.body.stage() != "child")
        for (const auto& k : ct.householdMust) {
            c.knowledge.insert(k);
            c.skills[k] = rng.range(r.strangerSkill[0], r.strangerSkill[1]);
        }
    // AL-82: what everyone of their age knows (knowledge.json start rules, as
    // worldgen gives the first villagers) - a newcomer who cannot farm leaves his
    // seed corn in the sack and starves by his own field.
    {
        const int age = c.body.ageDays / kDaysPerYear;
        for (const auto& [kid, k] : ct.knowledge) {
            const bool knows = (k.start == "all_from_child" && age >= 5) || (k.start == "all_from_youth" && age >= 12) ||
                               (k.start == "all_adults" && age >= 14) ||
                               (age >= 14 && k.startChanceAdult > 0.0 && rng.chance(k.startChanceAdult));
            if (!knows || c.knowledge.count(kid)) continue;
            bool ok = true;
            for (const auto& req : k.requiresKnowledge) ok = ok && (c.knowledge.count(req) || ct.knowledge.count(req) == 0 ||
                                                                    ct.knowledge.at(req).start.rfind("all_", 0) == 0);
            if (!ok) continue;
            c.knowledge.insert(kid);
            if (!c.skills.count(kid)) c.skills[kid] = rng.range(r.strangerSkill[0], r.strangerSkill[1]);
        }
    }
    c.status.settlement = st.id;
    const double ang = rng.range(0.0, 6.283185307179586);
    c.pos = {st.center.x + std::cos(ang) * r.arriveM, st.center.y + std::sin(ang) * r.arriveM};
    for (const auto& [oid, o] : w.objects())
        if (dist(o.pos, st.center) <= 1500.0 && !o.ownerFamily) c.knownObjects.insert(oid);
    return c;
}

void meetVillage(World& w, Character& s, const Settlement& st) {
    for (auto& [id, o] : w.characters()) {
        if (!person(&o) || id == s.id || o.status.settlement != st.id) continue;
        if (o.status.family == s.status.family && s.status.family != kNone) continue;
        Relation& to = o.relations[s.id];
        Relation& from = s.relations[id];
        to.familiarity = std::max(to.familiarity, 0.05);
        from.familiarity = std::max(from.familiarity, 0.05);
        to.trust = std::min(to.trust, -0.1);
        if (o.body.stage() != "child") memo(w, o, "stranger_arrived", s.id, 0.0, 0.3);
    }
}

bool arrive(World& w, Settlement& st, Rng& rng, bool family, const VillageLifeRules& r, char lonely = 0) {
    const EntityId sid = st.id;
    const std::string surname = randomFamilyName(rng, &w.content());
    if (!family) {
        // a lone wanderer: the household with most food to spare takes them in
        EntityId best = kNone;
        double bestDays = -1.0;
        for (EntityId fid : st.families) {
            auto f = w.families().find(fid);
            if (f == w.families().end()) continue;
            const Character* head = nullptr;
            for (EntityId m : f->second.members)
                if (const Character* x = w.character(m); person(x)) head = x;
            if (!head) continue;
            const double d = homeFoodDays(w, *head);
            if (d > bestDays) bestDays = d, best = fid;
        }
        if (best == kNone) return false;
        // a single of the sex the village lacks (a match for its lonely ones), else either
        const char sex = lonely ? lonely : (rng.chance(0.5) ? 'f' : 'm');
        Character c = makeStranger(w, rng, st, sex, rng.irange(r.loneAge[0], r.loneAge[1]), surname, r);
        const EntityId id = w.addCharacter(std::move(c)).id;
        Character& s = *w.character(id);
        s.status.dynasty = kNone;
        moveToFamily(w, s, best, r.takenInRole);
        meetVillage(w, s, st);
        w.chronicle("stranger", {id}, "chronicle.village.stranger_taken_in", {{"who", id}, {"family", best}});
        return true;
    }
    // a family: their own household by the village
    Family fam;
    fam.id = w.newId();
    fam.name = surname;
    fam.settlement = sid;
    Dynasty dyn;
    dyn.id = w.newId();
    dyn.name = "House " + surname;
    fam.dynasty = dyn.id;
    const double ang = rng.range(0.0, 6.283185307179586);
    WorldObject store;
    store.type = "storage";
    store.pos = {st.center.x + std::cos(ang) * r.arriveM * 0.7, st.center.y + std::sin(ang) * r.arriveM * 0.7};
    store.ownerFamily = fam.id;
    if (auto def = w.content().objects.find("storage"); def != w.content().objects.end()) store.stock = def->second.stock;
    for (const auto& [item, n] : r.householdKit) store.stock[item] += n;
    fam.storage = w.addObject(store).id;
    // AL-82: and a hearth by it - without one the grain they brought cannot be
    // cooked (cook_* acts on the household's own hearth), and they starve on a full sack.
    WorldObject hearth;
    hearth.type = "hearth";
    hearth.pos = {store.pos.x + 3.0, store.pos.y};
    hearth.ownerFamily = fam.id;
    if (auto def = w.content().objects.find("hearth"); def != w.content().objects.end()) hearth.stock = def->second.stock;
    fam.hearth = w.addObject(hearth).id;
    w.families()[fam.id] = fam;
    w.dynasties()[dyn.id] = dyn;
    st.families.push_back(fam.id);
    Character hm = makeStranger(w, rng, st, 'm', rng.irange(r.strangerAge[0] + 3, r.strangerAge[1] + 5), surname, r);
    Character hf = makeStranger(w, rng, st, 'f', rng.irange(r.strangerAge[0], r.strangerAge[1]), surname, r);
    const EntityId mid = w.addCharacter(std::move(hm)).id;
    const EntityId wid = w.addCharacter(std::move(hf)).id;
    std::vector<EntityId> who = {mid, wid};
    if (rng.chance(r.strangerChild)) {
        Character k = makeStranger(w, rng, st, rng.chance(0.5) ? 'f' : 'm', rng.irange(2, 12), surname, r);
        k.status.mother = wid;
        k.status.father = mid;
        who.push_back(w.addCharacter(std::move(k)).id);
    }
    for (EntityId id : who) {
        Character& s = *w.character(id);
        s.status.dynasty = dyn.id;
        moveToFamily(w, s, fam.id, id == mid ? "head" : (id == wid ? "spouse" : "child"));
        for (EntityId o : who)
            if (o != id) {
                Relation& rel = s.relations[o];
                rel.familiarity = 1.0;
                rel.affinity = std::max(rel.affinity, 0.6);
                rel.trust = std::max(rel.trust, 0.6);
            }
    }
    Character& m = *w.character(mid);
    Character& f = *w.character(wid);
    setPair(w, m, f, "married", true);
    if (who.size() > 2) {
        m.status.children.push_back(who[2]);
        f.status.children.push_back(who[2]);
    }
    w.dynasties()[dyn.id].founder = mid;
    for (EntityId id : who) meetVillage(w, *w.character(id), st);
    openFields(w, fam.id, store.pos, w.seed()); // AL-82: land to live on, like the others
    w.chronicle("stranger", who, "chronicle.village.strangers_settled",
                {{"who", mid}, {"spouse", wid}, {"family", fam.id}, {"people", static_cast<int>(who.size())}});
    return true;
}

void strangersDaily(World& w, const VillageLifeRules& r) {
    for (auto& [sid, st] : w.settlements()) {
        if (!st.full()) continue; // AL-92
        int people = 0, singleM = 0, singleF = 0;
        double foodDays = 0.0;
        for (const auto& [id, c] : w.characters())
            if (person(&c) && c.status.settlement == sid) {
                ++people;
                foodDays += homeFoodDays(w, c);
                if (marriageable(w, c, r) && years(c) <= r.oldFromYears) ++(c.body.sex == 'm' ? singleM : singleF);
            }
        if (people == 0 || people >= r.strangerMaxPeople) continue;
        // a village with nobody to marry draws wanderers sooner
        const bool lacking = singleM == 0 || singleF == 0;
        Rng rng = w.rng(sid, "village:strangers");
        // AL-82: word of a hungry winter travels - wanderers keep away from a village short of food.
        const double fed = r.strangerFoodDays > 0.0
                               ? std::clamp((foodDays / people / r.strangerFoodDays - 1.0 / 3.0) * 1.5, 0.0, 1.0)
                               : 1.0;
        if (!rng.chance((lacking ? r.strangerNoSingles : r.strangerDaily) * fed)) continue;
        const char lonely = singleM > 0 && singleF == 0 ? 'f' : (singleF > 0 && singleM == 0 ? 'm' : 0);
        arrive(w, st, rng, !lonely && rng.chance(r.strangerCouple), r, lonely);
    }
}

} // namespace

const VillageLifeRules& villageLifeRules(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<VillageLifeRules>> cache;
    static const VillageLifeRules kDefaults;
    if (c.dir.empty()) return kDefaults;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(c.dir);
    if (it != cache.end()) return *it->second;
    auto r = std::make_unique<VillageLifeRules>();
    const fs::path p = fs::path(c.dir) / "village_life.json";
    if (fs::exists(p)) {
        std::ifstream f(p, std::ios::binary);
        loadRules(*r, Json::parse(f));
    }
    return *cache.emplace(c.dir, std::move(r)).first->second;
}

std::vector<std::string> checkVillageLife(const Content& c) {
    std::vector<std::string> err;
    const VillageLifeRules& r = villageLifeRules(c);
    auto act = [&](const std::string& a, const char* where) {
        if (!c.interaction(a)) err.push_back(std::string("village_life.") + where + ": unknown act '" + a + "'");
    };
    for (const auto& a : r.courtActs) act(a, "courtship.acts");
    act(r.proposeAct, "courtship.propose_act");
    act(r.wedAct, "courtship.wed_act");
    act(r.intimacyAct, "marriage.intimacy_act");
    for (const auto& a : r.conceptionActs) act(a, "conception.acts");
    for (const auto& q : r.quarrelActs) act(q.act, "quarrels.acts");
    for (const auto& k : r.illnesses)
        if (!c.moodlets.count(k.moodlet)) err.push_back("village_life.illness: unknown moodlet '" + k.moodlet + "'");
    for (const char* m : {"pregnant", "in_love", "new_parent", "grieving", "rejected"})
        if (!c.moodlets.count(m)) err.push_back(std::string("village_life: moodlet '") + m + "' missing in needs.json");
    if (r.residence != "patrilocal" && r.residence != "matrilocal")
        err.push_back("village_life.courtship.residence: patrilocal | matrilocal");
    if (std::find(c.config.deathCauses.begin(), c.config.deathCauses.end(), "childbirth") == c.config.deathCauses.end())
        err.push_back("village_life: config.death_causes lacks 'childbirth'");
    return err;
}

std::vector<std::string> villageLifeLocKeys() {
    std::vector<std::string> k;
    for (const char* e : {"courtship", "betrothal", "proposal_refused", "wedding", "pregnancy", "birth", "stillbirth",
                          "new_head", "illness", "quarrel", "stranger_taken_in", "strangers_settled"})
        k.push_back(std::string("chronicle.village.") + e);
    for (const char* m : {"courting", "betrothed", "proposal_refused", "married_to", "with_child", "gave_birth",
                          "child_born", "widowed", "fell_ill", "quarrel", "stranger_arrived"})
        k.push_back(std::string("memory.") + m + ".text");
    for (const char* i : {"fever", "flux", "cough"}) k.push_back(std::string("illness.") + i + ".name");
    return k;
}

bool isPregnant(const World& w, const Character& c) { return c.hasMoodlet("pregnant", w.now()); }

bool kinOrHousehold(const World& w, const Character& a, const Character& b) {
    if (a.status.family != kNone && a.status.family == b.status.family) return true;
    auto parent = [](const Character& x, const Character& y) {
        return x.status.mother == y.id || x.status.father == y.id;
    };
    if (parent(a, b) || parent(b, a)) return true;
    auto shared = [](EntityId x, EntityId y) { return x != kNone && x == y; };
    if (shared(a.status.mother, b.status.mother) || shared(a.status.father, b.status.father)) return true;
    // grandparents and grandchildren
    for (EntityId p : {a.status.mother, a.status.father})
        if (const Character* pc = p == kNone ? nullptr : w.character(p); pc && parent(*pc, b)) return true;
    for (EntityId p : {b.status.mother, b.status.father})
        if (const Character* pc = p == kNone ? nullptr : w.character(p); pc && parent(*pc, a)) return true;
    return false;
}

EntityId courtshipTarget(const World& w, const Character& c) {
    const VillageLifeRules& r = R(w);
    if (!marriageable(w, c, r)) return kNone;
    if (const EntityId b = betrothedTo(c); b != kNone) return b;
    EntityId best = kNone;
    double score = r.interestMin;
    for (const auto& [id, o] : w.characters()) {
        if (o.isAnimal() || !suitable(w, c, o, r)) continue;
        const double s = interest(w, c, o, r);
        if (s > score) score = s, best = id;
    }
    return best;
}

void villageDaily(World& w) {
    const VillageLifeRules& r = R(w);
    if (!r.loaded) return;
    widowsAndHeads(w);
    std::map<EntityId, int> sickIn;
    for (const auto& [id, c] : w.characters())
        if (person(&c) && illnessOf(w, c, r)) ++sickIn[c.status.family];
    std::vector<EntityId> ids;
    for (const auto& [id, c] : w.characters())
        if (person(&c)) ids.push_back(id);
    for (EntityId id : ids) {
        Character& c = *w.character(id);
        bodyDaily(w, c, r, sickIn);
        if (!c.body.alive || c.body.downUntil > w.now()) continue;
        courtDaily(w, c, r);
        marriedDaily(w, c, r);
        quarrelDaily(w, c, r);
    }
    strangersDaily(w, r);
}

void villageAfterAct(World& w, Character& actor, const InteractionDef& d, Character* t, bool success) {
    const VillageLifeRules& r = R(w);
    if (!r.loaded || !t || t->isAnimal() || t->id == actor.id) return;
    const std::string& id = d.baseId.empty() ? d.id : d.baseId;
    const bool ours = queuedByUs(actor, d, t->id);
    if (id == r.proposeAct) {
        if (success && marriageable(w, actor, r) && marriageable(w, *t, r) && betrothedTo(actor) == kNone &&
            betrothedTo(*t) == kNone && !kinOrHousehold(w, actor, *t)) {
            betroth(w, actor, *t);
        } else if (!success) {
            memo(w, actor, "proposal_refused", t->id, -0.5, 0.6);
            moodlet(w, actor, "rejected");
            w.chronicle("proposal_refused", {actor.id, t->id}, "chronicle.village.proposal_refused",
                        {{"a", actor.id}, {"b", t->id}});
        }
        return;
    }
    if (id == r.wedAct) {
        if (success && t->body.alive && actor.body.alive) wed(w, actor, *t, r);
        return;
    }
    if (success && r.conceptionActs.count(id)) tryConceive(w, actor, *t, r);
    if (!ours || !success) return;
    if (std::find(r.courtActs.begin(), r.courtActs.end(), id) != r.courtActs.end()) {
        if (lastMemory(actor, "courting", t->id) < 0) {
            memo(w, actor, "courting", t->id, 0.4, 0.6);
            // the first sign of interest: what draws them shows in the relation
            Relation& back = t->relations[actor.id];
            back.attraction = std::clamp(back.attraction + 0.5 * spark(w, actor.id, t->id, r), -1.0, 1.0);
            w.chronicle("courtship", {actor.id, t->id}, "chronicle.village.courtship", {{"a", actor.id}, {"b", t->id}});
        }
        Relation& to = actor.relations[t->id];
        to.attraction = std::clamp(to.attraction + 0.03, -1.0, 1.0);
        return;
    }
    for (const auto& q : r.quarrelActs)
        if (q.act == id) {
            memo(w, actor, "quarrel", t->id, -0.4, 0.4);
            memo(w, *t, "quarrel", actor.id, -0.4, 0.4);
            w.chronicle("quarrel", {actor.id, t->id}, "chronicle.village.quarrel",
                        {{"a", actor.id}, {"b", t->id}, {"act", id}});
            return;
        }
}

double conceiveChanceOf(const Character& woman, const VillageLifeRules& r) {
    const int y = years(woman);
    if (y <= r.fadeFromYears) return r.conceiveChance;
    const double span = std::max(1, r.fertileF[1] - r.fadeFromYears);
    return r.conceiveChance * std::max(0.15, 1.0 - 0.85 * (y - r.fadeFromYears) / span);
}

bool refugeesArrive(World& w, EntityId settlement, const std::vector<EntityId>& whoIn) {
    auto it = w.settlements().find(settlement);
    if (it == w.settlements().end() || whoIn.empty() || !it->second.full()) return false;
    Settlement& st = it->second;
    const VillageLifeRules& r = R(w);
    for (EntityId id : whoIn)
        if (!w.offstage().count(id)) return false;
    Rng rng = w.rng(settlement, "village:refugees");
    // the household with most food to spare takes a lone one in
    EntityId best = kNone;
    if (whoIn.size() == 1) {
        double bestDays = -1.0;
        for (EntityId fid : st.families) {
            auto f = w.families().find(fid);
            if (f == w.families().end()) continue;
            const Character* head = nullptr;
            for (EntityId m : f->second.members)
                if (const Character* x = w.character(m); person(x)) head = x;
            if (!head) continue;
            const double d = homeFoodDays(w, *head);
            if (d > bestDays) bestDays = d, best = fid;
        }
        if (best == kNone) return false;
    }
    // onto the map: the same people, at the edge, knowing what a stranger knows
    std::vector<EntityId> who;
    for (EntityId id : whoIn) {
        Character* c = w.toStage(id);
        if (!c) continue;
        const Content& ct = w.content();
        for (const auto& n : ct.needs)
            if (!c->needs.count(n.id)) c->needs[n.id] = rng.range(n.startLo, n.startHi);
        if (c->body.stage() != "child")
            for (const auto& k : ct.householdMust)
                if (!c->knowledge.count(k)) {
                    c->knowledge.insert(k);
                    c->skills[k] = rng.range(r.strangerSkill[0], r.strangerSkill[1]);
                }
        c->status.settlement = st.id;
        c->status.family = kNone;
        c->ai = AiState{};
        const double ang = rng.range(0.0, 6.283185307179586);
        c->pos = {st.center.x + std::cos(ang) * r.arriveM, st.center.y + std::sin(ang) * r.arriveM};
        for (const auto& [oid, o] : w.objects())
            if (dist(o.pos, st.center) <= 1500.0 && !o.ownerFamily) c->knownObjects.insert(oid);
        who.push_back(id);
    }
    if (who.empty()) return false;
    if (who.size() == 1) {
        Character& s = *w.character(who[0]);
        s.status.dynasty = kNone;
        moveToFamily(w, s, best, r.takenInRole);
        meetVillage(w, s, st);
        w.chronicle("stranger", {s.id}, "chronicle.village.stranger_taken_in", {{"who", s.id}, {"family", best}});
        return true;
    }
    const Character& head0 = *w.character(who[0]);
    const std::string surname = head0.name.find(' ') != std::string::npos ? head0.name.substr(head0.name.rfind(' ') + 1)
                                                                          : randomFamilyName(rng, &w.content());
    Family fam;
    fam.id = w.newId();
    fam.name = surname;
    fam.settlement = st.id;
    Dynasty dyn;
    dyn.id = w.newId();
    dyn.name = "House " + surname;
    fam.dynasty = dyn.id;
    const double ang = rng.range(0.0, 6.283185307179586);
    WorldObject store;
    store.type = "storage";
    store.pos = {st.center.x + std::cos(ang) * r.arriveM * 0.7, st.center.y + std::sin(ang) * r.arriveM * 0.7};
    store.ownerFamily = fam.id;
    if (auto def = w.content().objects.find("storage"); def != w.content().objects.end()) store.stock = def->second.stock;
    for (const auto& [item, n] : r.householdKit) store.stock[item] += n;
    fam.storage = w.addObject(store).id;
    w.families()[fam.id] = fam;
    w.dynasties()[dyn.id] = dyn;
    st.families.push_back(fam.id);
    const EntityId headId = who[0];
    const EntityId spouseId = w.character(headId)->status.spouse;
    for (EntityId id : who) {
        Character& s = *w.character(id);
        s.status.dynasty = dyn.id;
        moveToFamily(w, s, fam.id, id == headId ? "head" : (id == spouseId ? "spouse" : "child"));
        for (EntityId o : who)
            if (o != id) {
                Relation& rel = s.relations[o];
                rel.familiarity = 1.0;
                rel.affinity = std::max(rel.affinity, 0.6);
                rel.trust = std::max(rel.trust, 0.6);
            }
    }
    if (Character* sp = w.character(spouseId)) setPair(w, *w.character(headId), *sp, "married", true);
    w.dynasties()[dyn.id].founder = headId;
    for (EntityId id : who) meetVillage(w, *w.character(id), st);
    w.chronicle("stranger", who, "chronicle.village.strangers_settled",
                {{"who", headId}, {"spouse", spouseId}, {"family", fam.id}, {"people", static_cast<int>(who.size())}});
    return true;
}

bool strangersArrive(World& w, EntityId settlement, bool family) {
    auto it = w.settlements().find(settlement);
    if (it == w.settlements().end()) return false;
    Rng rng = w.rng(settlement, "village:strangers_now");
    return arrive(w, it->second, rng, family, R(w));
}

void installVillageLife(World& w) {
    w.addEventHandler([](World& world, const Event& e) { return onBirth(world, e); });
}

void seedVillageLife(World& w, std::uint64_t seed) {
    const VillageLifeRules& r = R(w);
    if (!r.loaded) return;
    std::vector<EntityId> wives;
    for (const auto& [id, c] : w.characters())
        if (person(&c) && c.body.sex == 'f' && c.status.spouse != kNone) wives.push_back(id);
    for (EntityId id : wives) {
        Character& c = *w.character(id);
        Character* h = w.character(c.status.spouse);
        if (!h || !canConceive(w, c, *h, r)) continue;
        Rng rng = agentRng(seed, id, 0, "village:start_pregnant");
        if (!rng.chance(r.startPregnantShare)) continue;
        const Tick due = w.now() + static_cast<Tick>(rng.irange(5, r.pregnancyDays)) * kTicksPerDay;
        startPregnancy(w, c, h->id, due);
    }
}

} // namespace altera::sim
