// Consequences of deeds (consequences.hpp, AL-31..AL-36).

#include "sim/consequences.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

#include "sim/dialogue_say.hpp"
#include "sim/ai.hpp"
#include "sim/justice.hpp"
#include "sim/dialogue_gen.hpp"
#include "sim/dialogue_memory.hpp"
#include "sim/rumor_noise.hpp"

namespace fs = std::filesystem;

namespace altera::sim {

namespace {

const std::set<std::string> kDeedMemories = {"saw_crime", "told_crime", "rumor_crime", "suspects"};

double clampRel(double v) { return std::clamp(v, -1.0, 1.0); }

void relTo(Character& from, EntityId to, double trust, double enmity) {
    if (from.id == to) return;
    Relation& r = from.relations[to];
    r.trust = clampRel(r.trust + trust);
    r.enmity = clampRel(r.enmity + enmity);
}

// The household wronged by a deed and the person, when it was done to one.
struct Victim {
    EntityId family = kNone;
    EntityId person = kNone;
};

Victim victimOf(const World& w, const Character& doer, const WorldObject* target, const Character* targetChar) {
    Victim v;
    if (target) v.family = target->ownerFamily;
    if (targetChar) {
        if (targetChar->isAnimal()) v.family = targetChar->status.family; // livestock
        else {
            v.person = targetChar->id;
            v.family = targetChar->status.family;
        }
    }
    if (v.family != kNone && v.family == doer.status.family) v.family = kNone; // one's own household is not wronged
    if (v.person == doer.id) v.person = kNone;
    return v;
}

std::vector<Character*> membersOf(World& w, EntityId family) {
    std::vector<Character*> out;
    if (family == kNone) return out;
    auto f = w.families().find(family);
    if (f == w.families().end()) return out;
    for (EntityId id : f->second.members)
        if (Character* m = w.character(id); m && m->body.alive && !m->isAnimal()) out.push_back(m);
    return out;
}

bool knowsOf(const Character& c, const std::string& type, EntityId doer, EntityId object) {
    for (const auto& m : c.memory)
        if ((type.empty() ? kDeedMemories.count(m.type) > 0 : m.type == type) && !m.who.empty() && m.who[0] == doer &&
            m.object == object)
            return true;
    return false;
}

void remember(World& w, Character& c, const std::string& type, EntityId doer, EntityId victim, EntityId object,
              const std::string& source, double confidence, double valence, std::vector<EntityId> chain = {},
              EntityId fact = kNone) {
    MemoryEntry m;
    m.chain = std::move(chain);
    m.fact = fact;
    m.type = type;
    m.who = {doer};
    if (victim != kNone) m.who.push_back(victim);
    m.object = object;
    m.source = source;
    m.confidence = confidence;
    m.valence = valence;
    w.remember(c, std::move(m));
}

// The doer's repute moves once per known deed (consequences.json reputation).
void stain(World& w, Character& doer, const ConsequenceRules& R, const std::vector<LegalTag>& tags) {
    const auto& axes = w.content().config.reputationAxes;
    for (const auto& t : tags) {
        auto it = R.reputation.find(t.tag);
        if (it == R.reputation.end()) continue;
        for (const auto& [axis, dv] : it->second) {
            if (std::find(axes.begin(), axes.end(), axis) == axes.end()) continue;
            double& v = doer.reputation["reputation:" + axis];
            v = std::clamp(v + dv * t.mult, -1.0, 1.0);
        }
    }
}

// The wronged know (saw it, were told, heard enough, worked it out): they
// trust the doer less, hate him more, and with enough certainty accuse.
void wronged(World& w, Character& doer, const std::vector<Character*>& household, double confidence,
             const ConsequenceRules& R, const std::string& why) {
    for (Character* m : household) relTo(*m, doer.id, R.victimTrust * confidence, R.victimEnmity * confidence);
    if (confidence < R.accuseAt || !w.content().interaction(R.accuseTask)) return;
    // the eldest adult speaks for the household (a player's person is asked, not made to)
    Character* speaker = nullptr;
    for (Character* m : household)
        if (m->body.stage() != "child" && !m->controller.isPlayer() && m->id != doer.id &&
            (!speaker || m->body.ageDays > speaker->body.ageDays))
            speaker = m;
    if (!speaker) return;
    for (const auto& q : speaker->ai.queue)
        if (q.task == R.accuseTask && q.target == doer.id) return;
    speaker->ai.queue.push_back(
        QueuedTask{R.accuseTask, doer.id, R.accusePriority, w.now(), R.accuseTtl, why, "consequence"});
    trace(w, *speaker, 'S', "deed.accuse_queued", {{"doer", doer.id}, {"confidence", confidence}, {"why", why}});
}

bool takesSomething(const InteractionDef& d) {
    for (const auto& e : d.eff) {
        if ((e.subject == "target_stock" || e.subject == "home") && e.signedDelta() < 0.0) return true;
        if (e.onTarget && (e.subject == "has" || e.subject == "group") && e.signedDelta() < 0.0) return true;
    }
    return false;
}

bool givesOnly(const InteractionDef& d) {
    bool gives = false;
    for (const auto& e : d.eff)
        if ((e.subject == "target_stock" || e.subject == "home") && e.signedDelta() > 0.0) gives = true;
    return gives && !takesSomething(d);
}

double num(const Json& j, const char* k, double v) { return j.contains(k) && j[k].is_number() ? j[k].get<double>() : v; }

} // namespace

bool isDeedMemory(const std::string& type) { return kDeedMemories.count(type) > 0; }

const ConsequenceRules& consequenceRules(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<ConsequenceRules>> cache;
    static const ConsequenceRules kDefaults;
    if (c.dir.empty()) return kDefaults;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(c.dir);
    if (it != cache.end()) return *it->second;
    auto r = std::make_unique<ConsequenceRules>();
    const fs::path p = fs::path(c.dir) / "consequences.json";
    if (fs::exists(p)) {
        std::ifstream f(p, std::ios::binary);
        const Json j = Json::parse(f);
        if (j.contains("reputation"))
            for (auto t = j["reputation"].begin(); t != j["reputation"].end(); ++t)
                if (t->is_object())
                    for (auto a = t->begin(); a != t->end(); ++a) r->reputation[t.key()][a.key()] = a->get<double>();
        if (j.contains("wrongs_owner")) r->wrongsOwner = j["wrongs_owner"].get<std::set<std::string>>();
        const Json wi = j.value("witness", Json::object());
        r->witnessTrust = num(wi, "trust", r->witnessTrust);
        r->witnessEnmity = num(wi, "enmity", r->witnessEnmity);
        r->tellConfidence = num(wi, "tell_confidence", r->tellConfidence);
        const Json vi = j.value("victim", Json::object());
        r->victimTrust = num(vi, "trust", r->victimTrust);
        r->victimEnmity = num(vi, "enmity", r->victimEnmity);
        r->accuseAt = num(vi, "accuse_at", r->accuseAt);
        r->accuseTask = vi.value("accuse_task", r->accuseTask);
        r->accusePriority = num(vi, "accuse_priority", r->accusePriority);
        r->accuseTtl = static_cast<Tick>(num(vi, "accuse_ttl", static_cast<double>(r->accuseTtl)));
        const Json tr = j.value("trail", Json::object());
        r->noticeDaily = num(tr, "notice_daily", r->noticeDaily);
        r->noticeAtHome = num(tr, "notice_at_home", r->noticeAtHome);
        r->atHomeM = num(tr, "at_home_m", r->atHomeM);
        r->trailDays = static_cast<int>(num(tr, "days", r->trailDays));
        r->nearbyM = num(tr, "nearby_m", r->nearbyM);
        const Json su = j.value("suspicion", Json::object());
        r->suspectNearby = num(su, "nearby", r->suspectNearby);
        r->suspectBadName = num(su, "bad_name", r->suspectBadName);
        r->suspectEnmity = num(su, "enmity", r->suspectEnmity);
        r->suspectPrior = num(su, "prior", r->suspectPrior);
        r->suspectMin = num(su, "min", r->suspectMin);
        const Json ru = j.value("rumor", Json::object());
        if (ru.contains("talk_acts")) r->talkActs = ru["talk_acts"].get<std::set<std::string>>();
        r->rumorDecay = num(ru, "decay", r->rumorDecay);
        r->rumorMin = num(ru, "min", r->rumorMin);
        r->rumorTrust = num(ru, "trust", r->rumorTrust);
    }
    auto* out = r.get();
    cache[c.dir] = std::move(r);
    return *out;
}

void addForeignVariants(Content& c) {
    std::vector<InteractionDef> made;
    for (const auto& [id, d] : c.interactions) {
        if (!d.baseId.empty() || id.find('@') != std::string::npos) continue;
        if (!d.targetsObject() && !d.targetsCreature()) continue;
        const bool asksOwn = std::any_of(d.pre.begin(), d.pre.end(), [](const Condition& k) {
            return k.subject == "target_owner" && !k.negate && (k.key == "family" || k.key == "self");
        });
        if (!asksOwn) continue;
        InteractionDef f = d;
        f.id = id + "@foreign";
        f.baseId = id;
        f.goap = false; // the planners keep their own steal_* acts; this is for orders
        f.pre.clear();
        for (Condition k : d.pre) {
            if (k.subject == "target_owner" || k.subject == "herd") continue; // the culling plan is the owner's
            if (k.subject == "home") {
                if (!d.targetsObject()) continue;
                k.subject = "target_stock"; // taken from (put into) their store, not one's own
                k.text = "target_stock:" + k.key;
            }
            f.pre.push_back(k);
        }
        for (Effect& e : f.eff)
            if (e.subject == "home" && d.targetsObject()) {
                e.subject = "target_stock";
                e.text = "target_stock:" + e.key;
            }
        for (Effect& e : f.onFail)
            if (e.subject == "home" && d.targetsObject()) e.subject = "target_stock";
        const char* tag = takesSomething(f) ? "theft" : givesOnly(f) ? nullptr : "trespass";
        if (tag && std::find(f.legal.begin(), f.legal.end(), tag) == f.legal.end()) f.legal.push_back(tag);
        if (tag && std::string(tag) == "theft") f.visibility = "secret";
        made.push_back(std::move(f));
    }
    for (auto& f : made) c.interactions.emplace(f.id, std::move(f));
}

// --- rumour (AL-35, AL-50) ---------------------------------------------------

namespace {

const NoiseParams& noiseParams(const Content& c, double decay) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<NoiseParams>> cache;
    std::lock_guard<std::mutex> lock(mu);
    auto& slot = cache[c.dir];
    if (!slot) slot = std::make_unique<NoiseParams>(NoiseParams::fromJson(dialogueGenData(c).noise, decay));
    return *slot;
}

const MemoryEntry* memoryOfFact(const Character& c, EntityId fact, const std::string& type = "") {
    if (fact == kNone) return nullptr;
    for (const auto& m : c.memory)
        if (m.fact == fact && (type.empty() ? kDeedMemories.count(m.type) > 0 : m.type == type)) return &m;
    return nullptr;
}

bool caughtLying(const Character& c, EntityId teller, EntityId fact) {
    for (const auto& m : c.memory)
        if (m.type == "caught_lying" && m.fact == fact && !m.who.empty() && m.who[0] == teller) return true;
    return false;
}

// Whom the teller would sooner believe guilty: those he hates or distrusts,
// and those with a bad name.
std::vector<std::pair<EntityId, double>> suspects(const World& w, const Character& teller, EntityId doer,
                                                  EntityId listener) {
    std::vector<std::pair<EntityId, double>> out;
    for (const auto& [id, rel] : teller.relations) {
        if (id == doer || id == teller.id || id == listener) continue;
        const Character* c = w.character(id);
        if (!c || !c->body.alive || c->isAnimal() || c->body.stage() == "child") continue;
        double weight = std::max(0.0, rel.enmity) + std::max(0.0, -rel.affinity) + std::max(0.0, -rel.trust);
        for (const auto& [axis, v] : c->reputation) weight += 0.5 * std::max(0.0, -v);
        if (weight > 0.0) out.emplace_back(id, weight);
    }
    return out;
}

} // namespace

void passRumor(World& w, Character& from, Character& to, const ConsequenceRules& R) {
    const MemoryEntry* best = nullptr;
    for (const auto& m : from.memory) {
        if (!kDeedMemories.count(m.type) || m.who.empty() || m.who[0] == to.id || m.who[0] == from.id) continue;
        if (m.confidence * R.rumorDecay < R.rumorMin) continue;
        if (knowsOf(to, "", m.who[0], m.object)) continue;
        // the same deed, however it is told: known already, or this teller was caught on it
        if (const MemoryEntry* own = memoryOfFact(to, m.fact)) {
            if (own->source != "seen" || caughtLying(to, from.id, m.fact)) continue;
        }
        if (!best || m.salience * m.confidence > best->salience * best->confidence) best = &m;
    }
    if (!best) return;
    const MemoryEntry heard = *best;
    const Content& C = w.content();
    const DialogueGenData& D = dialogueGenData(C);
    const std::optional<Prop> told = propFromMemory(w, from, heard, D);

    // The meaning as the listener gets it: decayed, maybe distorted (AL-50).
    EntityId doerId = heard.who[0];
    EntityId object = heard.object;
    double conf = heard.confidence * R.rumorDecay;
    std::vector<EntityId> chain = {from.id};
    for (EntityId id : heard.chain)
        if (chain.size() < Epistemic::kMaxChain && id != to.id) chain.push_back(id);
    Json distorted;
    Json applied = Json::array();
    if (told) {
        NoiseContext ctx;
        ctx.teller = from.id;
        ctx.listener = to.id;
        ctx.tellerTraits = from.traits;
        if (auto it = from.relations.find(doerId); it != from.relations.end())
            ctx.tellerAffinityToDoer = it->second.affinity;
        ctx.agentCandidates = suspects(w, from, doerId, to.id);
        for (const auto& [id, item] : C.items)
            if (!item.tags.empty()) ctx.itemCategory[id] = item.tags.front();
        Rng rng = w.rng(to.id, "rumor_noise:" + std::to_string(from.id));
        const Epistemic epi = epistemicFromMemory(heard, w.now(), D.forMemory(heard.type));
        NoiseResult res = retell(D, noiseParams(C, R.rumorDecay), *told, epi, ctx, rng);
        // Somebody who saw it himself is not fooled, and trusts the teller less.
        if (const MemoryEntry* own = memoryOfFact(to, heard.fact); own && own->source == "seen") {
            const auto ownProp = propFromMemory(w, to, *own, D);
            const NoiseParams& P = noiseParams(C, R.rumorDecay);
            bool contra = false;
            if (ownProp && reconcile(P, res.prop, *ownProp, epistemicFromMemory(*own, w.now()), &contra)) {
                relTo(to, from.id, P.trustPenalty, 0.0);
                MemoryEntry m;
                m.type = "caught_lying";
                m.who = {from.id};
                m.fact = heard.fact;
                m.source = "seen";
                m.valence = -0.3;
                w.remember(to, std::move(m));
                trace(w, to, 'S', "deed.rumor_rejected", {{"from", from.id}, {"doer", doerId}});
            }
            return;
        }
        for (const auto& a : res.applied) applied.push_back(a);
        if (!res.applied.empty()) {
            distorted = toJson(res.prop);
            if (const Ref* d = res.prop.role("doer"); d && d->kind == "char") doerId = d->id;
            const Ref* o = res.prop.role("object");
            object = o && (o->kind == "object" || o->kind == "char") ? o->id : kNone;
        }
        conf = res.epi.confidence;
        chain = res.epi.chain;
        for (auto it = chain.begin(); it != chain.end();)
            it = *it == to.id ? chain.erase(it) : it + 1;
    }
    remember(w, to, "rumor_crime", doerId, heard.who.size() > 1 ? heard.who[1] : kNone, object, "rumor", conf, -0.2,
             std::move(chain), heard.fact);
    if (!distorted.is_null()) {
        // the entry just written (w.remember may have sorted or trimmed the memory)
        for (auto& m : to.memory)
            if (m.type == "rumor_crime" && m.fact == heard.fact && m.tick == w.now() && !m.who.empty() &&
                m.who[0] == doerId && m.prop.is_null()) {
                m.prop = distorted;
                break;
            }
    }
    relTo(to, doerId, R.rumorTrust * conf, 0.0);
    Json tp = {{"doer", doerId}, {"from", from.id}, {"confidence", conf}};
    if (!applied.empty()) tp["noise"] = applied;
    trace(w, to, 'S', "deed.rumor_heard", tp);
    // the wronged hear who did it (or who they are now told did it)
    Character* dc = w.character(doerId);
    if (!dc) return;
    if (const WorldObject* o = w.object(heard.object); o && o->ownerFamily != kNone && o->ownerFamily == to.status.family)
        wronged(w, *dc, membersOf(w, to.status.family), conf, R, "rumor");
}

Json rumorReport(const World& w) {
    const DialogueGenData& D = dialogueGenData(w.content());
    int memories = 0, falseDoer = 0, distorted = 0, facts = 0;
    double driftSum = 0.0;
    int maxHops = 0;
    for (const Fact& f : w.facts()) {
        if (!D.predicate(f.pred)) continue;
        ++facts;
        Prop truth;
        truth.pred = f.pred;
        for (const auto& [role, id] : f.roles) {
            if (id == kNone) continue;
            Ref r;
            r.kind = role == "victim_family" ? "family" : role == "object" ? "object" : "char";
            r.id = id;
            truth.roles[role] = r;
        }
        for (const auto& [cid, c] : w.characters())
            for (const auto& m : c.memory) {
                if (m.fact != f.id || !kDeedMemories.count(m.type)) continue;
                const auto p = propFromMemory(w, c, m, D);
                if (!p) continue;
                ++memories;
                if (!m.prop.is_null()) ++distorted;
                const Ref* d = p->role("doer");
                if (d && f.role("doer") != kNone && d->id != f.role("doer")) ++falseDoer;
                // compare only the roles the memory can know of
                Prop t = truth;
                for (auto it = t.roles.begin(); it != t.roles.end();)
                    it = (it->first == "doer" || p->roles.count(it->first)) ? std::next(it) : t.roles.erase(it);
                if (p->qty == 0.0) t.qty = 0.0;
                driftSum += drift(t, *p);
                maxHops = std::max(maxHops, static_cast<int>(m.chain.size()));
            }
    }
    return {{"facts", facts},
            {"memories", memories},
            {"distorted", distorted},
            {"false_doer", falseDoer},
            {"mean_drift", memories ? driftSum / memories : 0.0},
            {"max_hops", maxHops}};
}

void afterDeed(World& w, Character& doer, const InteractionDef& d, WorldObject* target, Character* targetChar,
               bool success, const std::vector<LegalTag>& tags, const std::vector<EntityId>& witnesses) {
    const ConsequenceRules& R = consequenceRules(w.content());
    const Victim v = victimOf(w, doer, target, targetChar);
    const EntityId object = target ? target->id : (targetChar && targetChar->isAnimal() ? targetChar->id : kNone);
    Json outcome = {{"action", d.id}, {"success", success}, {"witnesses", static_cast<int>(witnesses.size())}};
    bool wrongs = false;
    for (const auto& t : tags) wrongs |= R.wrongsOwner.count(t.tag) > 0;

    // --- a misdeed (any legal tag): seen or unseen ---------------------------
    if (!tags.empty() && success) {
        outcome["tag"] = tags.front().tag;
        std::vector<Character*> household = membersOf(w, v.family);
        if (v.person != kNone && v.family == kNone)
            if (Character* p = w.character(v.person)) household.push_back(p);
        bool ownersSaw = false;
        int seen = 0;
        for (EntityId id : witnesses) {
            Character* wc = w.character(id);
            if (!wc || !wc->body.alive || wc->id == doer.id) continue;
            ++seen;
            const bool owner = std::find(household.begin(), household.end(), wc) != household.end();
            ownersSaw |= owner;
            relTo(*wc, doer.id, R.witnessTrust, R.witnessEnmity);
        }
        if (seen > 0) {
            stain(w, doer, R, tags);
            w.chronicle("deed_seen", {doer.id}, "chronicle.deed_seen",
                        {{"doer", doer.id}, {"tag", tags.front().tag}, {"witnesses", seen}, {"object", object}});
            // A bystander tells the wronged household what he saw.
            if (!ownersSaw && !household.empty()) {
                EntityId teller = kNone;
                for (EntityId id : witnesses)
                    if (id != doer.id && w.character(id)) {
                        teller = id;
                        break;
                    }
                for (Character* m : household)
                    if (!knowsOf(*m, "", doer.id, object))
                        remember(w, *m, "told_crime", doer.id, teller, object, "told", R.tellConfidence, -0.4,
                                 teller != kNone ? std::vector<EntityId>{teller} : std::vector<EntityId>{});
                wronged(w, doer, household, R.tellConfidence, R, "told");
            } else if (ownersSaw) {
                wronged(w, doer, household, 1.0, R, "seen");
            }
        }
        outcome["witnesses"] = seen;
        outcome["seen"] = seen > 0;
        // The truth of it goes into the world's record (proofs and clearing a name, AL-37).
        if (!household.empty() || v.family != kNone) keepTruth(w, doer, d, v.family, v.person, object, tags.front().tag, witnesses);
        // Nobody of the household saw it: a trail they may find (AL-34).
        if (wrongs && !ownersSaw && v.family != kNone) {
            Json nearby = Json::array();
            const Vec2 at = target ? target->pos : doer.pos;
            for (const auto& [id, o] : w.characters())
                if (!o.isAnimal() && o.body.alive && o.status.family != v.family && dist(o.pos, at) <= R.nearbyM)
                    nearby.push_back(id);
            Json data = {{"family", v.family}, {"object", object}, {"doer", doer.id}, {"tag", tags.front().tag},
                         {"nearby", nearby}, {"since", w.now()}, {"seen", seen}, {"x", at.x}, {"y", at.y}};
            w.schedule({w.now() + kTicksPerDay, 0, "deed_trail", doer.id, v.person, object, data});
        }
    }

    // --- talk carries word of misdeeds (AL-35) ------------------------------
    if (success && targetChar && !targetChar->isAnimal() && R.talkActs.count(d.baseId.empty() ? d.id : d.baseId)) {
        auto pass = [&](Character& from, Character& to) { passRumor(w, from, to, R); };
        pass(doer, *targetChar);
        pass(*targetChar, doer);
    }

    // --- an accusation opens a case, a judge_case holds the court (AL-37) ---
    caseAfterAct(w, doer, d, targetChar, success);

    // --- a failed social act gets the other side's reaction aloud (AL-36) --
    if (!success && targetChar && !targetChar->isAnimal() && targetChar->body.alive && targetChar->id != doer.id &&
        (d.category == "social" || !d.speech.empty())) {
        const SaidLine line = sayFailReaction(w, *targetChar, doer, d);
        if (line.ok()) outcome["said"] = line.key;
    }

    // --- no silent "ok" for the player's person (AL-36) --------------------
    if (doer.controller.isPlayer()) {
        if (v.family != kNone) outcome["owner"] = v.family;
        if (v.person != kNone) outcome["victim"] = v.person;
        trace(w, doer, 'S', "act.outcome", outcome);
    }
}

void installConsequences(World& w) {
    installJustice(w);
    w.addEventHandler([](World& world, const Event& e) {
        if (e.type != "deed_trail") return false;
        const ConsequenceRules& R = consequenceRules(world.content());
        const EntityId family = e.data.value("family", kNone);
        std::vector<Character*> household = membersOf(world, family);
        Character* doer = world.character(e.a);
        if (household.empty() || !doer) return true;
        const Tick since = e.data.value("since", Tick{0});
        const Vec2 at{e.data.value("x", 0.0), e.data.value("y", 0.0)};
        // Does one of them find it today?
        Character* finder = nullptr;
        double best = 1e18;
        for (Character* m : household) {
            const double dd = dist(m->pos, at);
            if (dd < best) {
                best = dd;
                finder = m;
            }
        }
        const double p = best <= R.atHomeM ? R.noticeAtHome : R.noticeDaily;
        Rng r = agentRng(world.seed(), finder->id, world.now(), "deed_trail:" + std::to_string(e.obj));
        if (r.uniform() >= p) {
            if (world.now() - since < static_cast<Tick>(R.trailDays) * kTicksPerDay)
                world.schedule({world.now() + kTicksPerDay, 0, e.type, e.a, e.b, e.obj, e.data});
            return true;
        }
        // Found: remembered by the household, written down, a suspect sought.
        for (Character* m : household)
            remember(world, *m, "noticed_missing", kNone, kNone, e.obj, m == finder ? "seen" : "told", 1.0, -0.3);
        world.chronicle("loss_noticed", {finder->id}, "chronicle.loss_noticed",
                        {{"owner", finder->id}, {"object", e.obj}, {"tag", e.data.value("tag", "")}});
        trace(world, *finder, 'S', "deed.loss_noticed", {{"object", e.obj}, {"tag", e.data.value("tag", "")}});
        std::vector<EntityId> nearby = e.data.value("nearby", std::vector<EntityId>{});
        std::map<EntityId, double> score;
        const double about = nearby.empty() ? 0.0 : R.suspectNearby * std::min(1.0, 2.0 / nearby.size());
        for (EntityId id : nearby) score[id] += about;
        for (const auto& [id, o] : world.characters()) {
            if (o.isAnimal() || !o.body.alive || o.status.family == family) continue;
            if (o.status.settlement != finder->status.settlement && !score.count(id)) continue;
            auto rep = o.reputation.find("reputation:honesty");
            const double badName = rep == o.reputation.end() ? 0.0 : std::max(0.0, -rep->second);
            double s = R.suspectBadName * badName;
            if (auto rel = finder->relations.find(id); rel != finder->relations.end())
                s += R.suspectEnmity * std::max(0.0, rel->second.enmity);
            double prior = 0.0;
            for (const auto& m : finder->memory)
                if (kDeedMemories.count(m.type) && !m.who.empty() && m.who[0] == id) prior = std::max(prior, m.confidence);
            s += R.suspectPrior * prior;
            if (s > 0.0) score[id] += s;
        }
        EntityId suspect = kNone;
        double top = 0.0;
        for (const auto& [id, s] : score) {
            Rng j = agentRng(world.seed(), id, world.now(), "suspect:" + std::to_string(e.obj));
            const double sj = s + 0.05 * j.uniform();
            if (sj > top) {
                top = sj;
                suspect = id;
            }
        }
        if (suspect == kNone || top < R.suspectMin) return true;
        const double conf = std::min(1.0, top);
        Character* sc = world.character(suspect);
        for (Character* m : household)
            remember(world, *m, "suspects", suspect, kNone, e.obj, m == finder ? "seen" : "told", conf, -0.3);
        world.chronicle("suspicion", {finder->id, suspect}, "chronicle.suspicion",
                        {{"owner", finder->id}, {"suspect", suspect}, {"object", e.obj}, {"confidence", conf}});
        trace(world, *finder, 'S', "deed.suspects", {{"suspect", suspect}, {"confidence", conf},
                                                      {"right", suspect == e.a}});
        if (sc) wronged(world, *sc, household, conf, R, "suspicion");
        return true;
    });
}

} // namespace altera::sim
