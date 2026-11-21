// Memory -> meaning (AL-49, AL-38). See dialogue_memory.hpp.

#include "sim/dialogue_memory.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

#include "sim/character.hpp"
#include "sim/loc.hpp"
#include "sim/world.hpp"

namespace altera::sim {

namespace {

double num(const Json& j, const char* k, double d) {
    auto it = j.find(k);
    return it != j.end() && it->is_number() ? it->get<double>() : d;
}

// A role source of the memory table -> the Ref it names, if known.
std::optional<Ref> sourceRef(const World& w, const Character& self, const MemoryEntry& m, const std::string& src) {
    auto charRef = [](EntityId id) {
        Ref r;
        r.kind = "char";
        r.id = id;
        return r;
    };
    if (const size_t bar = src.find('|'); bar != std::string::npos) { // "goods|object": the first known
        if (auto r = sourceRef(w, self, m, src.substr(0, bar))) return r;
        return sourceRef(w, self, m, src.substr(bar + 1));
    }
    if (src == "self") return charRef(self.id);
    if (src == "goods") { // AL-58: what was taken, the most of it first (ties by name)
        const std::pair<const std::string, int>* best = nullptr;
        for (const auto& g : m.goods)
            if (g.second > 0 && (!best || g.second > best->second)) best = &g;
        if (!best) return std::nullopt;
        Ref r;
        r.kind = "item";
        r.key = best->first;
        return r;
    }
    if (src == "who0" || src == "who1") {
        const size_t i = src == "who0" ? 0 : 1;
        if (m.who.size() <= i || m.who[i] == kNone) return std::nullopt;
        return charRef(m.who[i]);
    }
    if (m.object == kNone) return std::nullopt;
    const WorldObject* o = w.object(m.object);
    if (src == "object") {
        Ref r;
        if (o) {
            r.kind = "object";
            r.id = o->id;
            r.key = o->type;
        } else if (w.character(m.object)) {
            r.kind = "char"; // a beast taken
            r.id = m.object;
        } else {
            return std::nullopt;
        }
        return r;
    }
    if (src == "object_owner") {
        if (!o || o->ownerFamily == kNone) return std::nullopt;
        Ref r;
        r.kind = "family";
        r.id = o->ownerFamily;
        return r;
    }
    return std::nullopt;
}

EntityId refId(const Prop& p, const char* role, const char* kind) {
    const Ref* r = p.role(role);
    return r && r->kind == kind ? r->id : kNone;
}

bool hasTags(const DialogueGenData& d, const std::string& pred) {
    const PredicateDef* def = d.predicate(pred);
    return def && !def->tags.empty();
}

} // namespace

RecallRules RecallRules::fromJson(const Json& j) {
    RecallRules r;
    if (!j.is_object()) return r;
    if (auto it = j.find("severity"); it != j.end() && it->is_object())
        for (auto s = it->begin(); s != it->end(); ++s)
            if (s->is_number()) r.severity[s.key()] = s->get<double>();
    r.severityDefault = num(j, "severity_default", r.severityDefault);
    r.mineMult = num(j, "mine_mult", r.mineMult);
    r.halfLifeDays = num(j, "half_life_days", r.halfLifeDays);
    r.minWeight = num(j, "min_weight", r.minWeight);
    r.guessMult = num(j, "guess_mult", r.guessMult);
    r.consentWariness = num(j, "consent_wariness", r.consentWariness);
    r.priceWariness = num(j, "price_wariness", r.priceWariness);
    return r;
}

const RecallRules& recallRules(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<RecallRules>> cache;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(c.dir);
    if (it != cache.end()) return *it->second;
    auto r = std::make_unique<RecallRules>();
    if (!c.dir.empty()) {
        std::ifstream f(std::filesystem::path(c.dir) / "dialogue_recall.json");
        if (f) {
            try {
                *r = RecallRules::fromJson(Json::parse(f));
            } catch (const std::exception&) {
                // a broken file keeps the defaults; validate_content reports it
            }
        }
    }
    auto* out = r.get();
    cache[c.dir] = std::move(r);
    return *out;
}

std::optional<Prop> propFromMemory(const World& w, const Character& self, const MemoryEntry& m,
                                   const DialogueGenData& d) {
    // AL-50: a distorted rumour carries its own meaning
    if (m.prop.is_object()) {
        Prop p;
        if (fromJson(m.prop, p) && checkProp(d, p)) {
            p.fact = m.fact;
            return p;
        }
    }
    const MemoryPropMap* map = d.forMemory(m.type);
    if (!map) return std::nullopt;
    Prop p;
    p.pred = map->pred;
    p.modality = map->modality;
    p.tense = "past";
    p.tick = m.tick;
    p.fact = m.fact;
    for (const auto& [role, src] : map->roles)
        if (auto r = sourceRef(w, self, m, src)) p.roles[role] = *r;
    if (!checkProp(d, p)) return std::nullopt;
    return p;
}

Epistemic epistemicFromMemory(const MemoryEntry& m, Tick now, const MemoryPropMap* map) {
    Epistemic e;
    e.source = m.source.empty() ? "seen" : m.source;
    if (map && map->modality == "guess" && e.source == "seen") e.source = "inferred";
    e.confidence = std::clamp(m.confidence, 0.0, 1.0);
    e.chain = m.chain;
    if (e.chain.size() > Epistemic::kMaxChain) e.chain.resize(Epistemic::kMaxChain);
    e.age = std::max<Tick>(0, now - m.tick);
    return e;
}

std::string topicOf(const Prop& p) {
    if (p.fact != kNone) return "fact:" + std::to_string(p.fact);
    const Ref* doer = p.role("doer");
    const Ref* obj = p.role("object");
    const std::string what = !obj ? "0" : obj->id != kNone ? std::to_string(obj->id) : obj->key;
    return p.pred + ":" + std::to_string(doer ? doer->id : kNone) + ":" + what;
}

std::vector<Recollection> recall(const World& w, const Character& self, EntityId about, const DialogueGenData& d,
                                 const RecallRules& r) {
    std::vector<Recollection> out;
    std::map<std::string, size_t> byTopic;
    for (size_t i = 0; i < self.memory.size(); ++i) {
        const MemoryEntry& m = self.memory[i];
        auto prop = propFromMemory(w, self, m, d);
        if (!prop) continue;
        const EntityId doer = refId(*prop, "doer", "char");
        if (doer == kNone || doer == self.id) continue;
        if (about != kNone && doer != about) continue;
        Recollection rc;
        const bool deed = hasTags(d, prop->pred);
        if (deed) rc.kind = "deed";
        else if (m.valence > 0.0) rc.kind = "kindness";
        else continue;
        const MemoryPropMap* map = d.forMemory(m.type);
        rc.memory = &m;
        rc.epi = epistemicFromMemory(m, w.now(), map);
        // mine: the rememberer or his household on the receiving end
        const EntityId victim = refId(*prop, "victim", "char");
        const EntityId benef = refId(*prop, "beneficiary", "char");
        const EntityId fam = refId(*prop, "victim_family", "family");
        rc.mine = victim == self.id || benef == self.id ||
                  (fam != kNone && fam == self.status.family);
        double sev = 0.5;
        if (deed) {
            sev = r.severityDefault;
            for (const auto& t : d.predicate(prop->pred)->tags) {
                auto it = r.severity.find(t);
                sev = std::max(sev, it == r.severity.end() ? r.severityDefault : it->second);
            }
        } else {
            sev = std::clamp(m.valence, 0.0, 1.0);
        }
        const double days = static_cast<double>(rc.epi.age) / static_cast<double>(kTicksPerDay);
        const double fresh = r.halfLifeDays > 0.0 ? std::exp2(-days / r.halfLifeDays) : 1.0;
        rc.weight = sev * rc.epi.confidence * fresh * (rc.mine ? r.mineMult : 1.0) *
                    (prop->modality == "guess" ? r.guessMult : 1.0);
        if (rc.weight < r.minWeight) continue;
        rc.topic = topicOf(*prop);
        rc.prop = std::move(*prop);
        auto [it, fresh_topic] = byTopic.emplace(rc.topic, out.size());
        if (fresh_topic) out.push_back(std::move(rc));
        else if (rc.weight > out[it->second].weight) out[it->second] = std::move(rc);
    }
    std::stable_sort(out.begin(), out.end(), [](const Recollection& a, const Recollection& b) {
        if (a.weight != b.weight) return a.weight > b.weight;
        return a.memory->tick > b.memory->tick;
    });
    return out;
}

std::vector<Recollection> recall(const World& w, const Character& self, EntityId about) {
    return recall(w, self, about, dialogueGenData(w.content()), recallRules(w.content()));
}

double deedWariness(const World& w, const Character& self, EntityId other) {
    if (other == kNone || other == self.id) return 0.0;
    // cheap: most people remember nothing of most others
    if (std::none_of(self.memory.begin(), self.memory.end(),
                     [&](const MemoryEntry& m) { return !m.who.empty() && m.who[0] == other; }))
        return 0.0;
    double keep = 1.0;
    for (const auto& rc : recall(w, self, other))
        if (rc.kind == "deed") keep *= 1.0 - std::clamp(rc.weight, 0.0, 1.0);
    return 1.0 - keep;
}

double priceFactor(const World& w, const Character& seller, EntityId buyer) {
    return 1.0 + recallRules(w.content()).priceWariness * deedWariness(w, seller, buyer);
}

void addMemoryFacts(const World& w, const Character& speaker, const Character& listener, ContextFacts& out) {
    for (const auto& axis : w.content().config.reputationAxes) {
        auto it = listener.reputation.find("reputation:" + axis);
        out.set("listener.reputation." + axis, it == listener.reputation.end() ? 0.0 : it->second);
    }
    const auto& d = dialogueGenData(w.content());
    const auto all = recall(w, speaker, listener.id, d, recallRules(w.content()));
    const Recollection* deed = nullptr;
    const Recollection* kind = nullptr;
    int deeds = 0;
    double keep = 1.0;
    for (const auto& rc : all) {
        if (rc.kind == "deed") {
            ++deeds;
            keep *= 1.0 - std::clamp(rc.weight, 0.0, 1.0);
            if (!deed) deed = &rc;
        } else if (!kind) {
            kind = &rc;
        }
    }
    if (deed) {
        out.set("memory.deed", deed->prop.pred);
        for (const auto& t : d.predicate(deed->prop.pred)->tags) out.add("memory.deed.tag", t);
        out.set("memory.deed.conf", deed->epi.confidence);
        out.set("memory.deed.source", deed->epi.source);
        out.set("memory.deed.hops", static_cast<double>(deed->epi.hops()));
        out.set("memory.deed.days", static_cast<double>(deed->epi.age / kTicksPerDay));
        out.set("memory.deed.modality", deed->prop.modality);
        out.set("memory.deed.topic", deed->topic);
        if (deed->mine) out.set("memory.deed.mine", 1.0);
        out.set("memory.deeds", static_cast<double>(deeds));
        out.set("memory.wariness", 1.0 - keep);
    }
    if (kind) {
        out.set("memory.kind", kind->prop.pred);
        out.set("memory.kind.conf", kind->epi.confidence);
        if (kind->mine) out.set("memory.kind.mine", 1.0);
    }
}

Utterance recallUtterance(const World& w, const Character& speaker, const Character& listener,
                          const std::string& act, const Recollection& r, std::uint64_t seed) {
    (void)w;
    Utterance u;
    u.speaker = speaker.id;
    u.listener = listener.id;
    u.act = act;
    u.content = r.prop;
    // to the doer's face it is "you did", told in the present of the talk
    u.epi = r.epi;
    u.stance.valence = r.kind == "deed" ? -std::min(1.0, r.weight) : std::min(1.0, r.weight);
    u.stance.emphasis = r.mine ? 0.8 : 0.4;
    u.seed = seed;
    return u;
}

Json propParams(const World& w, const Prop& p) {
    Json j = Json::object();
    for (const auto& [role, ref] : p.roles) {
        if (ref.kind == "char" || ref.kind == "object") j[role] = ref.id;
        else if (ref.kind == "item") j[role == "object" ? "item" : role] = ref.key;
        else if (ref.kind == "num") j[role] = ref.num;
    }
    if (const Ref* doer = p.role("doer"); doer && doer->kind == "char") j["who"] = doer->id;
    if (const Ref* v = p.role("victim"); v && v->kind == "char") {
        j["owner"] = v->id;
    } else if (const Ref* f = p.role("victim_family"); f && f->kind == "family") {
        if (const Family* fam = [&]() -> const Family* {
                auto it = w.families().find(f->id);
                return it == w.families().end() ? nullptr : &it->second;
            }();
            fam && !fam->members.empty()) {
            for (EntityId id : fam->members)
                if (const Character* c = w.character(id); c && c->body.alive && !c->isAnimal()) {
                    j["owner"] = id;
                    break;
                }
        }
    }
    if (const PredicateDef* def = dialogueGenData(w.content()).predicate(p.pred); def && !def->tags.empty())
        j["tag"] = def->tags.front();
    if (p.qty != 0.0) j["qty"] = p.qty;
    return j;
}

std::string renderUtterance(const Loc& loc, const std::string& lang, const World& w, const Utterance& u,
                            const std::string& sayKey) {
    Json params = u.content ? propParams(w, *u.content) : Json::object();
    if (u.speaker != kNone) params["speaker"] = u.speaker;
    if (u.listener != kNone) params["listener"] = u.listener;
    if (!u.epi.chain.empty()) params["from"] = u.epi.chain.front();
    const Character* self = w.character(u.speaker);
    return loc.text(lang, sayKey, locArgs(loc, lang, w, params, self));
}

} // namespace altera::sim
