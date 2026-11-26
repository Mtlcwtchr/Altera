// Talking over remembered moments (LR-158). See memory_talk.hpp.

#include "sim/memory_talk.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <mutex>

#include "sim/ai.hpp"
#include "sim/data_fs.hpp"
#include "sim/loc.hpp"
#include "sim/world.hpp"

namespace lr::sim {

namespace {

const Ref* arg(const Utterance& u, const char* k) {
    auto it = u.args.find(k);
    return it == u.args.end() ? nullptr : &it->second;
}

double daysAgo(const World& w, Tick t) { return static_cast<double>(std::max<Tick>(0, w.now() - t)) / kTicksPerDay; }

std::map<std::string, double> numMap(const Json& j) {
    std::map<std::string, double> out;
    if (!j.is_object()) return out;
    for (auto it = j.begin(); it != j.end(); ++it)
        if (it->is_number()) out[it.key()] = it->get<double>();
    return out;
}

// The other person in the moment (who[0], a person who is not the owner), or null.
const Character* withOf(const World& w, const Character& owner, const MemoryEntry& m) {
    if (m.who.empty()) return nullptr;
    const Character* c = w.character(m.who.front());
    if (!c || c->isAnimal() || c->id == owner.id) return nullptr;
    return c;
}

// The first letter in upper case (ASCII and Cyrillic), for a part that opens a sentence.
std::string upperFirst(std::string s) {
    if (s.empty()) return s;
    const unsigned char c0 = static_cast<unsigned char>(s[0]);
    if (c0 < 0x80) {
        s[0] = static_cast<char>(std::toupper(c0));
        return s;
    }
    if (s.size() < 2) return s;
    const unsigned char c1 = static_cast<unsigned char>(s[1]);
    if (c0 == 0xD0 && c1 >= 0xB0 && c1 <= 0xBF) s[1] = static_cast<char>(c1 - 0x20); // а..п
    else if (c0 == 0xD1 && c1 >= 0x80 && c1 <= 0x8F) {                              // р..я
        s[0] = static_cast<char>(0xD0);
        s[1] = static_cast<char>(c1 + 0x20);
    } else if (c0 == 0xD1 && c1 == 0x91) { // ё
        s[0] = static_cast<char>(0xD0);
        s[1] = static_cast<char>(0x81);
    }
    return s;
}

std::string str(const Json& j) { return j.is_string() ? j.get<std::string>() : std::string(); }

void addUpper(Json& args) {
    args["moment_up"] = upperFirst(str(args["moment"]));
    args["moment_we_up"] = upperFirst(str(args["moment_we"]));
    args["when_up"] = upperFirst(str(args["when"]));
}

void applyDeltas(Character& from, EntityId to, const std::map<std::string, double>& d) {
    if (d.empty()) return;
    Relation& r = from.relations[to];
    for (const auto& [axis, v] : d)
        if (double* p = r.axis(axis)) *p = std::clamp(*p + v, -1.0, 1.0);
}

} // namespace

// --- rules -----------------------------------------------------------------------------

const MomentKind* MomentRules::of(const std::string& type) const {
    auto it = types.find(type);
    return it == types.end() || it->second.interest <= 0.0 ? nullptr : &it->second;
}

MomentRules MomentRules::fromJson(const Json& j) {
    MomentRules r;
    r.maxAgeDays = j.value("max_age_days", r.maxAgeDays);
    r.halfLifeDays = j.value("half_life_days", r.halfLifeDays);
    r.minWeight = j.value("min_weight", r.minWeight);
    r.sharedMult = j.value("shared_mult", r.sharedMult);
    r.sharedWindowDays = j.value("shared_window_days", r.sharedWindowDays);
    r.maxMoments = j.value("max_moments", r.maxMoments);
    for (const auto& b : j.value("when", Json::array()))
        if (b.is_array() && b.size() == 2 && b[0].is_number() && b[1].is_string())
            r.when.emplace_back(b[0].get<double>(), b[1].get<std::string>());
    std::sort(r.when.begin(), r.when.end());
    r.whenBeyond = j.value("when_beyond", r.whenBeyond);
    const Json after = j.value("after", Json::object());
    r.afterTold = numMap(after.value("told", Json::object()));
    r.afterShared = numMap(after.value("shared", Json::object()));
    const Json talks = j.value("talks", Json::object());
    for (auto it = talks.begin(); it != talks.end(); ++it) {
        if (!it->is_object()) continue;
        MomentTalkTurn t;
        t.firstEvery = it->value("first_every", 0);
        t.onlyShared = it->value("only_shared", false);
        r.talks[it.key()] = t;
    }
    const Json types = j.value("types", Json::object());
    for (auto it = types.begin(); it != types.end(); ++it) {
        if (!it->is_object()) continue;
        MomentKind k;
        k.kind = it->value("kind", it.key());
        k.mood = it->value("mood", k.mood);
        k.interest = it->value("interest", 0.0);
        k.shareable = it->value("shareable", true);
        k.sharedBy = it->value("shared_by", k.sharedBy);
        r.types[it.key()] = k;
    }
    return r;
}

const MomentRules& momentRules(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<MomentRules>> cache;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(data::cacheKey(c.dir));
    if (it != cache.end()) return *it->second;
    auto r = std::make_unique<MomentRules>();
    if (!c.dir.empty()) {
        data::InFile f(std::filesystem::path(c.dir) / "memory_talk.json");
        if (f) {
            try {
                *r = MomentRules::fromJson(Json::parse(f));
            } catch (const std::exception&) {
                // a broken file means no moments; validate_content reports it
            }
        }
    }
    auto* out = r.get();
    cache[data::cacheKey(c.dir)] = std::move(r);
    return *out;
}

// --- moments -----------------------------------------------------------------------------

std::string momentTopic(const MemoryEntry& m) { return "moment:" + m.type + ":" + std::to_string(m.tick); }

Ref momentRef(const MemoryEntry& m) { return Ref{"moment", static_cast<EntityId>(m.tick + 1), m.type, 0.0, {}}; }

const MemoryEntry* momentMemory(const Character& owner, const Ref& r) {
    if (r.kind != "moment" || r.id == kNone) return nullptr;
    const Tick t = static_cast<Tick>(r.id - 1);
    for (const auto& m : owner.memory)
        if (m.tick == t && m.type == r.key) return &m;
    return nullptr;
}

bool sharedMoment(const World& w, const Character& c, const MemoryEntry& m, const Character& other) {
    if (other.id == c.id) return false;
    const MomentRules& R = momentRules(w.content());
    const MomentKind* k = R.of(m.type);
    if (!k || !k->shareable) return false;
    if (std::find(m.who.begin(), m.who.end(), other.id) != m.who.end()) return true;
    const bool kinOf = c.status.spouse == other.id || other.status.spouse == c.id ||
                       (c.status.family != kNone && c.status.family == other.status.family);
    if (k->sharedBy != "day" && !kinOf) return false;
    const Tick window = static_cast<Tick>(R.sharedWindowDays * kTicksPerDay);
    for (const auto& o : other.memory)
        if (o.type == m.type && o.source == "seen" && std::llabs(static_cast<long long>(o.tick) - static_cast<long long>(m.tick)) <= window)
            return true;
    return false;
}

std::vector<Moment> momentsToTell(const World& w, const Character& c, const Character* other, bool includeTold) {
    const MomentRules& R = momentRules(w.content());
    std::vector<Moment> out;
    if (R.types.empty() || !c.body.alive || c.isAnimal()) return out;
    std::map<std::string, size_t> byType; // one per type: the strongest
    for (const auto& m : c.memory) {
        if (m.source != "seen") continue;
        const MomentKind* k = R.of(m.type);
        if (!k) continue;
        const double age = daysAgo(w, m.tick);
        if (age > R.maxAgeDays) continue;
        Moment mo;
        mo.memory = &m;
        mo.kind = *k;
        mo.topic = momentTopic(m);
        mo.shared = other && sharedMoment(w, c, m, *other);
        const double fresh = std::pow(0.5, age / std::max(1.0, R.halfLifeDays));
        mo.weight = k->interest * std::max(0.1, m.salience) * fresh * (mo.shared ? R.sharedMult : 1.0);
        if (mo.weight < R.minWeight) continue;
        if (other && !includeTold && w.said().topicCount(c.id, other->id, mo.topic) > 0) continue;
        auto it = byType.find(m.type);
        if (it != byType.end()) {
            if (out[it->second].weight >= mo.weight) continue;
            out[it->second] = mo;
            continue;
        }
        byType[m.type] = out.size();
        out.push_back(mo);
    }
    std::stable_sort(out.begin(), out.end(), [](const Moment& a, const Moment& b) {
        if (a.weight != b.weight) return a.weight > b.weight;
        return a.memory->tick > b.memory->tick;
    });
    if (out.size() > R.maxMoments) out.resize(R.maxMoments);
    return out;
}

bool momentsFirst(const World& w, const std::string& talk, int turn) {
    const MomentRules& R = momentRules(w.content());
    auto it = R.talks.find(talk);
    if (it == R.talks.end() || it->second.firstEvery <= 0) return false;
    const int n = it->second.firstEvery;
    return std::max(0, turn) % n == n - 1;
}

namespace {

// The teller says the moment, the hearer answers; the told record and the deltas follow.
bool tellMoment(World& w, Character& teller, Character& hearer, const Moment& mo, const std::string& intent) {
    const MomentRules& R = momentRules(w.content());
    std::map<std::string, Ref> args;
    args["moment"] = momentRef(*mo.memory);
    const std::string topic = mo.topic;
    const bool shared = mo.shared;
    const std::string type = mo.memory->type;
    const char* act = shared ? "reminisce" : "recall_moment";
    if (!say(w, teller, hearer, act, std::nullopt, Epistemic{}, "", intent, args).ok()) return false;
    w.said().record("memory_talk", teller.id, hearer.id, topic, w.now());
    if (shared) w.said().record("memory_talk", hearer.id, teller.id, topic, w.now());
    say(w, hearer, teller, shared ? "react_reminisce" : "react_moment", std::nullopt, Epistemic{}, "", intent, args);
    applyDeltas(hearer, teller.id, R.afterTold);
    if (shared) {
        applyDeltas(hearer, teller.id, R.afterShared);
        applyDeltas(teller, hearer.id, R.afterShared);
    }
    trace(w, teller, 'D', "memory.told", {{"type", type}, {"to", hearer.id}, {"shared", shared}});
    return true;
}

} // namespace

bool talkOverMoments(World& w, Character& teller, Character& hearer, const std::string& talk, int turn) {
    if (teller.id == hearer.id || hearer.isAnimal() || !hearer.body.alive) return false;
    const MomentRules& R = momentRules(w.content());
    auto t = R.talks.find(talk);
    if (t == R.talks.end()) return false;
    std::vector<Moment> moments = momentsToTell(w, teller, &hearer);
    if (t->second.onlyShared)
        moments.erase(std::remove_if(moments.begin(), moments.end(), [](const Moment& m) { return !m.shared; }), moments.end());
    if (moments.empty()) return false;
    // the strongest first; a later turn of the same talk may pick the next one
    const Moment& mo = moments[static_cast<size_t>(std::max(0, turn)) % std::min<size_t>(moments.size(), 2)];
    return tellMoment(w, teller, hearer, mo, talk);
}

int answerMemory(World& w, Character& answerer, Character& asker) {
    const auto moments = momentsToTell(w, answerer, &asker);
    if (moments.empty())
        return say(w, answerer, asker, "no_moment", std::nullopt, Epistemic{}, "", "ask_memory").ok() ? 1 : 0;
    const size_t before = answerer.ai.trace.size();
    if (!tellMoment(w, answerer, asker, moments.front(), "ask_memory")) return 0;
    int n = 0;
    for (size_t i = before; i < answerer.ai.trace.size(); ++i)
        n += answerer.ai.trace[i].code == "say.line" && answerer.ai.trace[i].params.value("speaker", kNone) == answerer.id;
    return std::max(1, n);
}

// --- lines ------------------------------------------------------------------------------

void addMomentArgs(const Loc& loc, const std::string& lang, const World& w, const Utterance& u, Json& args) {
    const Ref* r = arg(u, "moment");
    if (!r || r->kind != "moment") { // a line that may ask for these without a moment: nothing raw is left
        if (!args.contains("moment") || !args["moment"].is_string()) args["moment"] = loc.text(lang, "memory.moment.any", args);
        if (!args.contains("moment_we")) args["moment_we"] = loc.text(lang, "memory.shared.any", args);
        if (!args.contains("when")) args["when"] = loc.text(lang, "memory.when.long");
        if (!args.contains("with")) args["with"] = loc.noun(lang, "memory.somebody", "");
        addUpper(args);
        return;
    }
    // whose moment: the teller's (recall, reminisce), else the one who told it (the reactions)
    const Character* owner = nullptr;
    const MemoryEntry* m = nullptr;
    for (EntityId id : {u.speaker, u.listener})
        if (const Character* c = w.character(id); c && !m)
            if ((m = momentMemory(*c, *r))) owner = c;
    const std::string type = r->key;
    // {moment} in the owner's voice; the reactions speak of the listener's moment
    Json own = args;
    if (owner && owner->id == u.listener && args.contains("listener")) own["self"] = own["speaker"] = args["listener"];
    args["moment"] = loc.has(lang, "memory.moment." + type) ? loc.text(lang, "memory.moment." + type, own)
                                                            : loc.text(lang, "memory.moment.any", own);
    args["moment_we"] = loc.has(lang, "memory.shared." + type) ? loc.text(lang, "memory.shared." + type, own)
                                                               : loc.text(lang, "memory.shared.any", own);
    std::string bucket;
    if (m) {
        const MomentRules& R = momentRules(w.content());
        const double d = daysAgo(w, m->tick);
        bucket = R.whenBeyond;
        for (const auto& [upTo, b] : R.when)
            if (d <= upTo) {
                bucket = b;
                break;
            }
    }
    args["when"] = loc.text(lang, "memory.when." + (bucket.empty() ? std::string("long") : bucket));
    if (owner && m)
        if (const Character* c = withOf(w, *owner, *m)) args["with"] = loc.personNoun(lang, *c);
    if (!args.contains("with")) args["with"] = loc.noun(lang, "memory.somebody", "");
    addUpper(args);
}

void installMomentFacts() {
    registerFactProvider(
        "moment",
        [](ContextFacts& f, const World& w, const Utterance& u) {
            const Ref* r = arg(u, "moment");
            if (!r || r->kind != "moment") return;
            const Character* owner = nullptr;
            const MemoryEntry* m = nullptr;
            for (EntityId id : {u.speaker, u.listener})
                if (const Character* c = w.character(id); c && !m)
                    if ((m = momentMemory(*c, *r))) owner = c;
            f.set("moment", 1.0);
            f.set("moment.type", r->key);
            const MomentRules& R = momentRules(w.content());
            if (const MomentKind* k = R.of(r->key)) {
                f.set("moment.kind", k->kind);
                f.set("moment.mood", k->mood);
            }
            if (!m || !owner) return;
            f.set("moment.days", daysAgo(w, m->tick));
            if (owner->id == u.listener) f.set("moment.listeners", 1.0); // the reply: the moment is the other's
            const Character* other = w.character(owner->id == u.speaker ? u.listener : u.speaker);
            if (other && sharedMoment(w, *owner, *m, *other)) f.set("moment.shared", 1.0);
            if (const Character* c = withOf(w, *owner, *m)) {
                if (c->id == u.speaker || c->id == u.listener) f.set("moment.with_us", 1.0);
                else f.set("moment.with", 1.0);
                if (!c->body.alive) f.set("moment.with_dead", 1.0);
            }
            // the one who answers keeps a moment of the same kind himself
            if (other && owner->id == u.listener) {
                const Character* me = w.character(u.speaker);
                const MomentKind* k = R.of(r->key);
                if (me && k)
                    for (const auto& o : me->memory) {
                        if (o.source != "seen" || daysAgo(w, o.tick) > R.maxAgeDays) continue;
                        const MomentKind* ok = R.of(o.type);
                        if (ok && ok->kind == k->kind && !sharedMoment(w, *owner, *m, *me)) {
                            f.set("moment.same", 1.0);
                            break;
                        }
                    }
            }
        },
        {"moment"});
}

namespace {
[[maybe_unused]] const bool kMomentFactsInstalled = (installMomentFacts(), true);
} // namespace

bool momentFactKey(const std::string& k) { return k == "moment" || k.rfind("moment.", 0) == 0; }

void sampleMomentFacts(const std::string& act, const DialogueRuleDef* target, ContextFacts& f) {
    static const std::set<std::string> acts = {"recall_moment", "reminisce", "react_moment", "react_reminisce"};
    if (acts.count(act)) {
        f.set("moment", 1.0);
        f.set("moment.type", "attacked_by_beast");
        f.set("moment.kind", "beast");
        f.set("moment.mood", "alarm");
        f.set("moment.days", 3.0);
    }
    if (!target) return;
    for (const auto& c : target->when) {
        if (!momentFactKey(c.key)) continue;
        switch (c.op) {
        case Criterion::Op::Eq:
            if (!c.values.empty()) f.set(c.key, c.values.front());
            break;
        case Criterion::Op::Has: f.set(c.key, 1.0); break;
        case Criterion::Op::Gt: f.set(c.key, c.number + 0.1); break;
        case Criterion::Op::Ge: f.set(c.key, c.number); break;
        case Criterion::Op::Lt: f.set(c.key, c.number - 0.1); break;
        case Criterion::Op::Le: f.set(c.key, c.number); break;
        case Criterion::Op::Not:
            f.str.erase(c.key);
            f.num.erase(c.key);
            f.list.erase(c.key);
            break;
        default: break;
        }
    }
}

void sampleMomentArgs(const Loc& loc, const std::string& lang, Json& args) {
    const Json self = args.value("self", args.value("speaker", Json()));
    Json a = args;
    a["self"] = a["speaker"] = self;
    args["moment"] = loc.text(lang, "memory.moment.attacked_by_beast", a);
    args["moment_we"] = loc.text(lang, "memory.shared.raid", a);
    args["when"] = loc.text(lang, "memory.when.days");
    args["with"] = args.value("person", args.value("listener", Json()));
    addUpper(args);
}

} // namespace lr::sim
