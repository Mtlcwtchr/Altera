// The registry of truth at work: vouch, prove, the truth report (facts.hpp, AL-55).

#include "sim/facts.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

#include "sim/ai.hpp"
#include "sim/consequences.hpp"
#include "sim/justice.hpp"
#include "sim/world.hpp"

namespace fs = std::filesystem;

namespace altera::sim {

namespace {

double num(const Json& j, const char* k, double v) { return j.contains(k) && j[k].is_number() ? j[k].get<double>() : v; }

const Fact* factById(const World& w, EntityId id) {
    if (id == kNone || id > w.facts().size()) return nullptr;
    const Fact& f = w.facts()[id - 1]; // ids are positions + 1 (World::addFact)
    return f.id == id ? &f : nullptr;
}

// The latest deed on that object (or done to that household), settled or not.
const Fact* latestOn(const World& w, EntityId object, EntityId victimFamily) {
    for (auto it = w.facts().rbegin(); it != w.facts().rend(); ++it) {
        if (it->role("doer") == kNone) continue;
        if (object != kNone ? it->role("object") == object
                            : (victimFamily != kNone && it->role("victim_family") == victimFamily))
            return &*it;
    }
    return nullptr;
}

// Did `who` see it himself? (his memory of it, else the registry's list of witnesses)
bool sawIt(const Character& who, const Fact& f) {
    for (const auto& m : who.memory)
        if (m.fact == f.id && m.source == "seen" && m.type == "saw_crime") return true;
    return std::find(f.witnesses.begin(), f.witnesses.end(), who.id) != f.witnesses.end();
}

void rel(Character& from, EntityId to, double trust, double enmity) {
    if (from.id == to || to == kNone) return;
    Relation& r = from.relations[to];
    r.trust = std::clamp(r.trust + trust, -1.0, 1.0);
    r.enmity = std::clamp(r.enmity + enmity, -1.0, 1.0);
}

void honesty(World& w, Character& c, double dv) {
    double& v = c.reputation["reputation:honesty"];
    v = std::clamp(v + dv, -1.0, 1.0);
}

// The listener's beliefs blaming `blamed` (deed memories, not what he saw himself).
std::vector<size_t> blames(const Character& listener, EntityId blamed) {
    std::vector<size_t> out;
    for (size_t i = 0; i < listener.memory.size(); ++i) {
        const auto& m = listener.memory[i];
        if (isDeedMemory(m.type) && m.type != "saw_crime" && m.source != "refuted" && !m.who.empty() && m.who[0] == blamed)
            out.push_back(i);
    }
    return out;
}

// One belief shown false: doubt it (or forget it), think better of the blamed,
// worse of whoever spread it; learn who really did it, from whom.
void refute(World& w, Character& listener, size_t idx, const TruthCheck& t, EntityId by, const FactRules& R) {
    MemoryEntry& m = listener.memory[idx];
    const EntityId blamed = m.who[0];
    const EntityId object = m.object;
    const EntityId spreader = !m.chain.empty() ? m.chain.front() : (m.who.size() > 1 && m.type == "told_crime" ? m.who[1] : kNone);
    m.confidence *= R.refuteMult;
    m.source = "refuted"; // kept as a doubt, not refuted again
    const bool forget = m.confidence < R.dropBelow;
    if (forget) listener.memory.erase(listener.memory.begin() + static_cast<long>(idx));
    rel(listener, blamed, R.clearedTrust, R.clearedEnmity);
    if (spreader != blamed && spreader != by) rel(listener, spreader, R.spreaderTrust, 0.0);
    if (Character* b = w.character(blamed)) honesty(w, *b, R.clearedHonesty);
    if (t.realDoer != kNone && t.realDoer != listener.id) {
        bool knows = false;
        for (const auto& k : listener.memory)
            knows |= isDeedMemory(k.type) && !k.who.empty() && k.who[0] == t.realDoer && k.object == object;
        if (!knows) {
            MemoryEntry told;
            told.type = "told_crime";
            told.who = {t.realDoer, by};
            told.object = object;
            told.source = "told";
            told.confidence = R.witnessConfidence;
            told.valence = -0.4;
            told.chain = {by};
            told.fact = t.fact ? t.fact->id : kNone;
            w.remember(listener, std::move(told));
        }
    }
    // a case on it ends: the accused is cleared
    if (Character* b = w.character(blamed); b && casePending(*b, listener.id, object))
        closeCase(w, listener, *b, object, "cleared",
                  {{"witness", t.witness}, {"real_doer", t.realDoer}, {"by", by}});
}

} // namespace

const FactRules& factRules(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<FactRules>> cache;
    static const FactRules kDefaults;
    if (c.dir.empty()) return kDefaults;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(c.dir);
    if (it != cache.end()) return *it->second;
    auto r = std::make_unique<FactRules>();
    const fs::path p = fs::path(c.dir) / "consequences.json";
    if (fs::exists(p)) {
        std::ifstream f(p, std::ios::binary);
        const Json j = Json::parse(f).value("truth", Json::object());
        r->refuteMult = num(j, "refute_mult", r->refuteMult);
        r->dropBelow = num(j, "drop_below", r->dropBelow);
        r->clearedTrust = num(j, "cleared_trust", r->clearedTrust);
        r->clearedEnmity = num(j, "cleared_enmity", r->clearedEnmity);
        r->spreaderTrust = num(j, "spreader_trust", r->spreaderTrust);
        r->clearedHonesty = num(j, "cleared_honesty", r->clearedHonesty);
        r->witnessConfidence = num(j, "witness_confidence", r->witnessConfidence);
        r->lieTrust = num(j, "lie_trust", r->lieTrust);
        r->unprovenMult = num(j, "unproven_mult", r->unprovenMult);
        r->vouchAct = j.value("vouch_act", r->vouchAct);
        r->proveAct = j.value("prove_act", r->proveAct);
    }
    auto* out = r.get();
    cache[c.dir] = std::move(r);
    return *out;
}

TruthCheck checkDoer(const World& w, EntityId object, EntityId claimed, EntityId victimFamily, EntityId fact) {
    TruthCheck t;
    t.fact = factById(w, fact);
    if (!t.fact) t.fact = latestOn(w, object, victimFamily);
    if (!t.fact) {
        // nothing of the sort happened there (as far as the world knows): only an object can say so
        t.contradicted = object != kNone && w.object(object) != nullptr;
        return t;
    }
    t.realDoer = t.fact->role("doer");
    t.confirmed = t.realDoer == claimed;
    t.contradicted = !t.confirmed;
    for (EntityId id : trueWitnesses(w, *t.fact))
        if (id != claimed) {
            t.witness = id;
            break;
        }
    return t;
}

int vouch(World& w, Character& speaker, Character& listener) {
    const FactRules& R = factRules(w.content());
    int changed = 0;
    for (const auto& [id, c] : w.characters()) {
        (void)c;
        if (id == listener.id || id == speaker.id) continue;
        for (size_t k : [&] { auto v = blames(listener, id); std::reverse(v.begin(), v.end()); return v; }()) {
            const MemoryEntry& m = listener.memory[k];
            TruthCheck t = checkDoer(w, m.object, id, listener.status.family, m.fact);
            if (!t.fact || !t.contradicted || !sawIt(speaker, *t.fact)) continue; // he can only tell what he saw
            t.witness = speaker.id;
            const EntityId object = m.object;
            refute(w, listener, k, t, speaker.id, R);
            ++changed;
            w.chronicle("fact_vouched", {speaker.id, listener.id, id},
                        "chronicle.fact_vouched",
                        {{"witness", speaker.id}, {"listener", listener.id}, {"cleared", id}, {"real_doer", t.realDoer},
                         {"object", object}});
        }
    }
    trace(w, speaker, 'S', "deed.vouched", {{"listener", listener.id}, {"changed", changed}});
    return changed;
}

int proveInnocence(World& w, Character& accused, Character& listener) {
    const FactRules& R = factRules(w.content());
    int changed = 0;
    bool caught = false;
    auto idx = blames(listener, accused.id);
    std::reverse(idx.begin(), idx.end()); // erasing from the back keeps the rest in place
    for (size_t k : idx) {
        MemoryEntry& m = listener.memory[k];
        const TruthCheck t = checkDoer(w, m.object, accused.id, listener.status.family, m.fact);
        if (t.confirmed) {
            if (t.witness != kNone) { // a true witness: the lie is plain
                m.confidence = std::max(m.confidence, R.witnessConfidence);
                caught = true;
            } else {
                m.confidence *= R.unprovenMult; // his word against the rumour
            }
            continue;
        }
        if (!t.contradicted) continue;
        if (t.fact && t.witness == kNone) { // another did it, but nobody can say so
            m.confidence *= R.unprovenMult;
            continue;
        }
        const EntityId object = m.object;
        refute(w, listener, k, t, t.witness != kNone ? t.witness : accused.id, R);
        ++changed;
        w.chronicle("fact_proved", {accused.id, listener.id}, "chronicle.fact_proved",
                    {{"accused", accused.id}, {"listener", listener.id}, {"witness", t.witness},
                     {"real_doer", t.realDoer}, {"object", object}});
    }
    if (caught) {
        rel(listener, accused.id, R.lieTrust, 0.0);
        honesty(w, accused, -R.clearedHonesty);
        w.chronicle("lie_exposed", {accused.id, listener.id}, "chronicle.lie_exposed",
                    {{"liar", accused.id}, {"listener", listener.id}});
    }
    trace(w, accused, 'S', "deed.proved", {{"listener", listener.id}, {"changed", changed}, {"caught", caught}});
    return caught ? -1 : changed;
}

void factsAfterAct(World& w, Character& doer, const InteractionDef& d, Character* targetChar, bool success) {
    if (!success || !targetChar || targetChar->isAnimal() || targetChar->id == doer.id) return;
    const FactRules& R = factRules(w.content());
    const std::string& id = d.baseId.empty() ? d.id : d.baseId;
    if (id == R.vouchAct) vouch(w, doer, *targetChar);
    else if (id == R.proveAct) proveInnocence(w, doer, *targetChar);
}

Json truthReport(const World& w) {
    Json facts = Json::array();
    std::map<EntityId, Json> byFact;
    int unlinked = 0;
    for (const auto& [id, c] : w.characters())
        for (const auto& m : c.memory) {
            if (!isDeedMemory(m.type) && m.type != "noticed_missing") continue;
            if (m.fact == kNone) {
                unlinked += isDeedMemory(m.type);
                continue;
            }
            const Fact* f = factById(w, m.fact);
            if (!f) continue;
            Json& e = byFact[m.fact];
            if (e.is_null()) e = {{"true", 0}, {"wrong", 0}, {"by_type", Json::object()}};
            e["by_type"][m.type] = e["by_type"].value(m.type, 0) + 1;
            if (m.type == "noticed_missing") continue;
            const bool right = !m.who.empty() && m.who[0] == f->role("doer");
            e[right ? "true" : "wrong"] = e[right ? "true" : "wrong"].get<int>() + 1;
        }
    for (const auto& f : w.facts()) {
        Json e = {{"id", f.id}, {"pred", f.pred}, {"doer", f.role("doer")}, {"object", f.role("object")},
                  {"witnesses", f.witnesses.size()}, {"outcome", f.outcome}};
        if (auto it = byFact.find(f.id); it != byFact.end()) e["beliefs"] = it->second;
        facts.push_back(e);
    }
    return {{"facts", facts}, {"unlinked_beliefs", unlinked}};
}

} // namespace altera::sim
