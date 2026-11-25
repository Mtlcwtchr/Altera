// Dialogue generation, layer 2: context facts, rule choice, memory of what was said (AL-48).
// Design: doc/design/DIALOGUE_GEN.md §4; contract: sim/dialogue_gen.hpp.

#include "sim/dialogue_rules.hpp"
#include "sim/data_fs.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>

#include "sim/world.hpp"

namespace lr::sim {

namespace {

bool fail(std::string* e, const std::string& m) {
    if (e) *e = m;
    return false;
}

std::string refText(const Ref& r) {
    if (r.kind == "char" || r.kind == "object" || r.kind == "family") return r.kind + ":" + std::to_string(r.id);
    if (r.kind == "num") {
        std::ostringstream o;
        o << r.num;
        return "num:" + o.str();
    }
    if (r.kind == "prop") return "prop";
    return r.kind + ":" + r.key;
}

std::string ageBand(int years) {
    if (years < 14) return "child";
    if (years < 25) return "young";
    if (years < 60) return "adult";
    return "old";
}

const char* const kEstates[] = {"noble", "clergy", "burgher", "commoner"};

// Filled by the built-in sources. A key with a trailing '.' is a prefix.
const std::vector<std::string>& builtinFactKeys() {
    static const std::vector<std::string> k = {
        "act", "tone", "intent", "has_content", "pred", "pred.tag", "neg", "modality", "tense", "qty", "unit", "fact",
        "role.", "epi.source", "epi.conf", "epi.hops", "epi.age_days", "topic.age_days", "stance.valence",
        "stance.emphasis", "stance.politeness", "speaker.", "listener.", "rel.", "rel_back.", "memory.", "reputation.",
        "time", "season", "said.", "said_to.", "heard.topic", "slot", "outcome", "reason", "place",
        "mode", "topic.kind", "commitment", "reply_to"};
    return k;
}

// Subkeys of speaker. / listener.
const std::set<std::string>& personKeys() {
    static const std::set<std::string> k = {"trait", "age",   "age_years", "stage",    "sex",    "role",   "species",
                                            "status", "state", "is_player", "mood",    "rights", "verdict"};
    return k;
}

struct Provider {
    FactProvider fn;
    std::vector<std::string> prefixes;
};
std::map<std::string, Provider>& providers() {
    static std::map<std::string, Provider> p;
    return p;
}

bool matchesKey(const std::string& key, const std::string& pattern) {
    if (!pattern.empty() && pattern.back() == '.') return key.compare(0, pattern.size(), pattern) == 0 && key.size() > pattern.size();
    return key == pattern || key.compare(0, pattern.size() + 1, pattern + ".") == 0;
}

// A rule is a part rule when it names a slot; returns that slot.
std::string ruleSlot(const DialogueRuleDef& r) {
    for (const auto& c : r.when)
        if (c.key == "slot" && c.op == Criterion::Op::Eq && !c.values.empty()) return c.values.front();
    return {};
}

bool isFallback(const DialogueRuleDef& r) {
    return r.when.size() == 1 && r.when[0].key == "act" && r.when[0].op == Criterion::Op::Eq;
}

} // namespace

// --- durations, topics ----------------------------------------------------------

Tick parseDialogueDuration(const std::string& s) {
    if (s.empty()) return -1;
    size_t digits = 0;
    while (digits < s.size() && std::isdigit(static_cast<unsigned char>(s[digits]))) ++digits;
    if (digits == 0 || digits > 9) return -1;
    const Tick n = std::stoll(s.substr(0, digits));
    const std::string unit = s.substr(digits);
    if (unit.empty()) return n;
    if (unit == "d") return n * kTicksPerDay;
    if (unit == "h") return n * (kTicksPerDay / 24);
    return -1;
}

std::string dialogueTopic(const Utterance& u) {
    if (!u.content) return {};
    const Prop& p = *u.content;
    if (p.fact != kNone) return "fact:" + std::to_string(p.fact);
    std::string t = "pred:" + p.pred;
    for (const auto& [role, ref] : p.roles) t += "|" + role + "=" + refText(ref);
    return t;
}

// --- memory of what was said ----------------------------------------------------

bool SaidRecord::operator==(const SaidRecord& o) const {
    return rule == o.rule && speaker == o.speaker && listener == o.listener && topic == o.topic && tick == o.tick;
}
bool SaidMemory::operator==(const SaidMemory& o) const { return log == o.log; }

void SaidMemory::record(const std::string& rule, EntityId speaker, EntityId listener, const std::string& topic, Tick tick) {
    log.push_back(SaidRecord{rule, speaker, listener, topic, tick});
}

bool SaidMemory::blocked(const DialogueRuleDef& r, EntityId speaker, EntityId listener, const std::string& topic,
                         Tick now) const {
    if (r.cooldown.empty() && r.oncePer.empty()) return false;
    std::string scope;
    Tick dur = -1;
    if (!r.cooldown.empty()) {
        const size_t c = r.cooldown.find(':');
        if (c != std::string::npos) {
            scope = r.cooldown.substr(0, c);
            dur = parseDialogueDuration(r.cooldown.substr(c + 1));
        }
    }
    const bool byListener = r.oncePer == "listener" || r.oncePer == "listener+topic";
    const bool byTopic = r.oncePer == "topic" || r.oncePer == "listener+topic";
    for (auto it = log.rbegin(); it != log.rend(); ++it) {
        const SaidRecord& s = *it;
        if (s.rule != r.id) continue;
        if (dur >= 0 && now - s.tick < dur) {
            if (scope == "speaker" && s.speaker == speaker) return true;
            if (scope == "listener" && s.listener == listener) return true;
            if (scope == "pair" && s.speaker == speaker && s.listener == listener) return true;
        }
        if (!r.oncePer.empty() && s.speaker == speaker && (!byListener || s.listener == listener) &&
            (!byTopic || s.topic == topic))
            return true;
    }
    return false;
}

int SaidMemory::count(const std::string& rule, EntityId speaker, EntityId listener) const {
    int n = 0;
    for (const auto& s : log)
        if (s.rule == rule && s.speaker == speaker && (listener == kNone || s.listener == listener)) ++n;
    return n;
}

int SaidMemory::topicCount(EntityId speaker, EntityId listener, const std::string& topic) const {
    if (topic.empty()) return 0;
    int n = 0;
    for (const auto& s : log)
        if (s.topic == topic && s.listener == listener && (speaker == kNone || s.speaker == speaker)) ++n;
    return n;
}

void SaidMemory::prune(Tick now, Tick keep, size_t cap) {
    log.erase(std::remove_if(log.begin(), log.end(), [&](const SaidRecord& s) { return now - s.tick > keep; }),
              log.end());
    if (log.size() > cap) log.erase(log.begin(), log.begin() + static_cast<std::ptrdiff_t>(log.size() - cap));
}

Json toJson(const SaidMemory& m) {
    Json a = Json::array();
    for (const auto& s : m.log) {
        Json r = {{"rule", s.rule}, {"s", s.speaker}, {"l", s.listener}, {"t", s.tick}};
        if (!s.topic.empty()) r["topic"] = s.topic;
        a.push_back(std::move(r));
    }
    return Json{{"said", a}};
}

bool fromJson(const Json& j, SaidMemory& out, std::string* e) {
    out = SaidMemory{};
    if (!j.is_object()) return fail(e, "said memory: not an object");
    const Json a = j.value("said", Json::array());
    if (!a.is_array()) return fail(e, "said memory: 'said' is not a list");
    try {
        for (const auto& r : a) {
            SaidRecord s;
            s.rule = r.at("rule").get<std::string>();
            s.speaker = r.value("s", kNone);
            s.listener = r.value("l", kNone);
            s.tick = r.value("t", Tick{0});
            s.topic = r.value("topic", std::string());
            out.log.push_back(std::move(s));
        }
    } catch (const std::exception& ex) {
        out = SaidMemory{};
        return fail(e, std::string("said memory: ") + ex.what());
    }
    return true;
}

// --- states ---------------------------------------------------------------------

namespace {
std::vector<Json> readRuleFiles(const std::string& contentDir) {
    namespace fs = std::filesystem;
    std::vector<Json> files;
    const fs::path rdir = fs::path(contentDir) / "dialogue_rules";
    if (contentDir.empty() || !data::isDir(rdir)) return files;
    std::vector<fs::path> paths = data::listPaths(rdir);
    for (const auto& p : paths) {
        data::InFile in(p);
        Json j = Json::parse(in, nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue; // load() reports broken files
        j["_file"] = "dialogue_rules/" + p.filename().string();
        files.push_back(std::move(j));
    }
    return files;
}
} // namespace

std::vector<DialogueStateDef> parseDialogueStates(const std::vector<Json>& ruleFiles, std::vector<std::string>* errors) {
    std::map<std::string, DialogueStateDef> byId;
    for (const auto& f : ruleFiles) {
        if (!f.is_object() || !f.contains("states")) continue;
        const std::string fname = f.value("_file", std::string("rules"));
        const Json& st = f["states"];
        if (!st.is_object()) {
            if (errors) errors->push_back(fname + ": states is not an object");
            continue;
        }
        for (auto it = st.begin(); it != st.end(); ++it) {
            if (it.key().rfind("_", 0) == 0) continue;
            DialogueStateDef d;
            d.id = it.key();
            try {
                d.moodlets = it->value("moodlets", std::vector<std::string>{});
                d.fatigueGe = it->value("fatigue_ge", -1.0);
                d.moodLe = it->value("mood_le", 2.0);
            } catch (const std::exception& ex) {
                if (errors) errors->push_back(fname + ": state " + d.id + ": " + ex.what());
                continue;
            }
            if (byId.count(d.id) && errors) errors->push_back(fname + ": state " + d.id + " is defined twice");
            byId[d.id] = std::move(d);
        }
    }
    std::vector<DialogueStateDef> out;
    for (auto& [id, d] : byId) out.push_back(std::move(d));
    return out;
}

const std::vector<DialogueStateDef>& dialogueStates(const std::string& contentDir) {
    static std::map<std::string, std::unique_ptr<std::vector<DialogueStateDef>>> cache;
    auto it = cache.find(contentDir);
    if (it != cache.end()) return *it->second;
    const std::vector<Json> files = readRuleFiles(contentDir);
    auto v = std::make_unique<std::vector<DialogueStateDef>>(parseDialogueStates(files));
    auto* out = v.get();
    cache[contentDir] = std::move(v);
    return *out;
}

// --- context facts -----------------------------------------------------------------

void addUtteranceFacts(ContextFacts& f, const DialogueGenData& d, const Utterance& u) {
    if (!u.act.empty()) f.set("act", u.act);
    if (!u.tone.empty()) f.set("tone", u.tone);
    if (!u.intent.empty()) f.set("intent", u.intent);
    f.set("stance.valence", u.stance.valence);
    f.set("stance.emphasis", u.stance.emphasis);
    f.set("stance.politeness", u.stance.politeness);
    if (!u.content) return;
    const Prop& p = *u.content;
    f.set("has_content", 1.0);
    f.set("pred", p.pred);
    if (const PredicateDef* def = d.predicate(p.pred))
        for (const auto& t : def->tags) f.add("pred.tag", t);
    if (p.neg) f.set("neg", 1.0);
    f.set("modality", p.modality);
    f.set("tense", p.tense);
    if (p.qty != 0.0) f.set("qty", p.qty);
    if (!p.unit.empty()) f.set("unit", p.unit);
    if (p.fact != kNone) f.set("fact", 1.0);
    for (const auto& [role, ref] : p.roles) {
        f.set("role." + role, ref.kind);
        if (!ref.key.empty()) f.set("role." + role + ".key", ref.key);
        if (ref.kind == "char" && ref.id != kNone) {
            if (ref.id == u.speaker) f.set("speaker.is_" + role, 1.0);
            if (ref.id == u.listener) f.set("listener.is_" + role, 1.0);
        }
    }
    f.set("epi.source", u.epi.source);
    f.set("epi.conf", u.epi.confidence);
    f.set("epi.hops", static_cast<double>(u.epi.hops()));
    const double ageDays = static_cast<double>(u.epi.age) / static_cast<double>(kTicksPerDay);
    f.set("epi.age_days", ageDays);
    f.set("topic.age_days", ageDays);
}

namespace {

void personFacts(ContextFacts& f, const World& w, const Character& c, const std::string& p, const Utterance& u) {
    const Tick now = w.now();
    for (const auto& t : c.traits) f.add(p + ".trait", t);
    const int years = c.body.ageDays / kDaysPerYear;
    f.set(p + ".age", ageBand(years));
    f.set(p + ".age_years", static_cast<double>(years));
    f.set(p + ".stage", c.body.stage());
    f.set(p + ".sex", std::string(1, c.body.sex));
    f.set(p + ".role", c.status.role);
    if (!c.species.empty()) f.set(p + ".species", c.species);
    std::string estate = "commoner";
    for (const char* e : kEstates)
        if (c.hasTrait(e)) {
            estate = e;
            break;
        }
    f.set(p + ".status", estate);
    if (c.controller.isPlayer()) f.set(p + ".is_player", 1.0);
    const double mood = c.mood(now);
    f.set(p + ".mood", mood);
    for (const auto& s : dialogueStates(w.content().dir)) {
        bool on = false;
        for (const auto& m : s.moodlets)
            if (c.hasMoodlet(m, now)) on = true;
        if (s.fatigueGe >= 0.0 && c.body.fatigue >= s.fatigueGe) on = true;
        if (s.moodLe <= 1.0 && mood <= s.moodLe) on = true;
        if (on) f.add(p + ".state", s.id);
    }
    for (const auto& [axis, v] : c.reputation) f.set(p + ".reputation." + axis, v);
    for (const auto& r : c.status.rights) f.add(p + ".rights", r);
    for (const auto& v : c.status.verdicts) f.add(p + ".verdict", v);
    if (u.content && c.status.family != kNone)
        for (const auto& [role, ref] : u.content->roles)
            if (ref.kind == "family" && ref.id == c.status.family) f.set(p + ".is_" + role, 1.0);
}

} // namespace

void addWorldFacts(ContextFacts& f, const World& w, const Utterance& u) {
    f.set("time", w.isNight() ? "night" : "day");
    f.set("season", w.season());
    const Character* s = w.character(u.speaker);
    const Character* l = w.character(u.listener);
    if (s) personFacts(f, w, *s, "speaker", u);
    if (l) {
        personFacts(f, w, *l, "listener", u);
        for (const auto& [axis, v] : l->reputation) f.set("reputation." + axis, v);
    }
    if (!s || !l) return;
    if (auto it = s->relations.find(l->id); it != s->relations.end())
        for (const auto& axis : relAxes()) f.set("rel." + axis, it->second.get(axis));
    if (auto it = l->relations.find(s->id); it != l->relations.end())
        for (const auto& axis : relAxes()) f.set("rel_back." + axis, it->second.get(axis));
    if (s->status.spouse == l->id) f.add("rel.kin", "spouse");
    if (s->status.mother == l->id || s->status.father == l->id) f.add("rel.kin", "parent");
    if (l->status.mother == s->id || l->status.father == s->id) f.add("rel.kin", "child");
    if ((s->status.mother != kNone && s->status.mother == l->status.mother) ||
        (s->status.father != kNone && s->status.father == l->status.father))
        f.add("rel.kin", "sibling");
    if (s->status.family != kNone && s->status.family == l->status.family) f.add("rel.kin", "family");
    if (auto it = s->status.pairs.find(l->id); it != s->status.pairs.end())
        for (const auto& pr : it->second) f.add("rel.pair", pr);
    std::map<std::string, double> mem;
    for (const auto& m : s->memory)
        if (!m.who.empty() && m.who[0] == l->id) mem[m.type] += 1.0;
    for (const auto& [type, n] : mem) f.set("memory." + type, n);
}

void addSaidFacts(ContextFacts& f, const SaidMemory& said, const Utterance& u) {
    std::map<std::string, double> by, to;
    for (const auto& s : said.log) {
        if (s.speaker != u.speaker) continue;
        by[s.rule] += 1.0;
        if (s.listener == u.listener) to[s.rule] += 1.0;
    }
    for (const auto& [r, n] : by) f.set("said." + r, n);
    for (const auto& [r, n] : to) f.set("said_to." + r, n);
    const std::string topic = dialogueTopic(u);
    if (topic.empty()) return;
    if (int n = said.topicCount(u.speaker, u.listener, topic)) f.set("said.topic", static_cast<double>(n));
    if (int n = said.topicCount(kNone, u.listener, topic)) f.set("heard.topic", static_cast<double>(n));
}

void registerFactProvider(const std::string& name, FactProvider fn, std::vector<std::string> prefixes) {
    providers()[name] = Provider{std::move(fn), std::move(prefixes)};
}

void unregisterFactProvider(const std::string& name) { providers().erase(name); }

void addProviderFacts(ContextFacts& f, const World& w, const Utterance& u) {
    for (const auto& [name, p] : providers())
        if (p.fn) p.fn(f, w, u);
}

ContextFacts dialogueFacts(const DialogueGenData& d, const World* w, const SaidMemory* said, const Utterance& u) {
    ContextFacts f;
    addUtteranceFacts(f, d, u);
    if (w) addWorldFacts(f, *w, u);
    if (said) addSaidFacts(f, *said, u);
    if (w) addProviderFacts(f, *w, u);
    return f;
}

bool knownDialogueFact(const std::string& key) {
    for (const auto& k : builtinFactKeys())
        if (matchesKey(key, k)) {
            if (k == "speaker." || k == "listener.") {
                const std::string sub = key.substr(k.size());
                const std::string head = sub.substr(0, sub.find('.'));
                if (personKeys().count(head) || head == "reputation" || sub.rfind("is_", 0) == 0) return true;
                continue;
            }
            return true;
        }
    for (const auto& [name, p] : providers())
        for (const auto& k : p.prefixes)
            if (matchesKey(key, k)) return true;
    return false;
}

// --- choosing ----------------------------------------------------------------------

RuleChoice chooseRule(const DialogueGenData& d, const ContextFacts& facts, const SaidMemory* said, const SayContext& ctx) {
    ContextFacts slotted;
    const ContextFacts* f = &facts;
    if (!ctx.slot.empty()) {
        slotted = facts;
        slotted.set("slot", ctx.slot);
        f = &slotted;
    }
    RuleChoice out;
    for (const auto& r : d.rules) {
        const std::string slot = ruleSlot(r);
        if (slot != ctx.slot) continue;
        bool all = true;
        for (const auto& c : r.when)
            if (!c.match(*f)) {
                all = false;
                break;
            }
        if (all) out.fitting.push_back(&r);
    }
    std::stable_sort(out.fitting.begin(), out.fitting.end(), [](const DialogueRuleDef* a, const DialogueRuleDef* b) {
        if (a->specificity() != b->specificity()) return a->specificity() > b->specificity();
        return a->id < b->id;
    });
    if (out.fitting.empty()) return out;
    std::vector<const DialogueRuleDef*> open;
    for (const auto* r : out.fitting) {
        if (said && said->blocked(*r, ctx.speaker, ctx.listener, ctx.topic, ctx.now))
            out.blocked.push_back(r->id);
        else
            open.push_back(r);
    }
    const std::vector<const DialogueRuleDef*>& pool = open.empty() ? out.fitting : open;
    out.repeat = open.empty();
    const int best = pool.front()->specificity();
    std::vector<const DialogueRuleDef*> top;
    for (const auto* r : pool)
        if (r->specificity() == best) top.push_back(r);
    double total = 0.0;
    for (const auto* r : top) {
        total += std::max(0.0, r->weight);
        out.tied.push_back(r->id);
    }
    out.specificity = best;
    out.rule = top.front();
    if (top.size() > 1 && total > 0.0) {
        Rng rng(hashCombine(hashCombine(ctx.seed, hashStr("dialogue_rule")), hashStr(ctx.slot)));
        double x = rng.uniform() * total;
        for (const auto* r : top) {
            x -= std::max(0.0, r->weight);
            if (x < 0.0) {
                out.rule = r;
                break;
            }
        }
    }
    return out;
}

namespace {
std::uint64_t lineSeed(const Utterance& u, Tick now) {
    return hashCombine(hashCombine(u.seed, u.speaker), static_cast<std::uint64_t>(now));
}
} // namespace

RuleChoice chooseLine(const DialogueGenData& d, const World& w, const Utterance& u, SaidMemory* said,
                      bool recordRepeat, ContextFacts* factsOut) {
    ContextFacts f = dialogueFacts(d, &w, said, u);
    SayContext ctx;
    ctx.speaker = u.speaker;
    ctx.listener = u.listener;
    ctx.topic = dialogueTopic(u);
    ctx.now = w.now();
    ctx.seed = lineSeed(u, w.now());
    RuleChoice c = chooseRule(d, f, said, ctx);
    if (said && c.rule && (!c.repeat || recordRepeat)) said->record(c.rule->id, u.speaker, u.listener, ctx.topic, ctx.now);
    if (factsOut) *factsOut = std::move(f);
    return c;
}

RuleChoice chooseSlot(const DialogueGenData& d, const ContextFacts& lineFacts, const Utterance& u,
                      const std::string& slot, Tick now, SaidMemory* said) {
    SayContext ctx;
    ctx.speaker = u.speaker;
    ctx.listener = u.listener;
    ctx.topic = dialogueTopic(u);
    ctx.now = now;
    ctx.seed = lineSeed(u, now);
    ctx.slot = slot;
    RuleChoice c = chooseRule(d, lineFacts, said, ctx);
    if (said && c.rule && !c.repeat && (!c.rule->cooldown.empty() || !c.rule->oncePer.empty()))
        said->record(c.rule->id, u.speaker, u.listener, ctx.topic, now);
    return c;
}

std::string explainChoice(const RuleChoice& c, const ContextFacts& f) {
    std::ostringstream o;
    o << "facts:\n";
    for (const auto& [k, v] : f.str) o << "  " << k << " = " << v << "\n";
    for (const auto& [k, v] : f.num) o << "  " << k << " = " << v << "\n";
    for (const auto& [k, v] : f.list) {
        o << "  " << k << " = [";
        for (size_t i = 0; i < v.size(); ++i) o << (i ? ", " : "") << v[i];
        o << "]\n";
    }
    o << "fitting:\n";
    for (const auto* r : c.fitting) {
        o << "  " << r->id << " (" << r->specificity() << ")";
        if (std::find(c.blocked.begin(), c.blocked.end(), r->id) != c.blocked.end()) o << " blocked";
        if (r == c.rule) o << " <- " << r->say;
        o << "\n";
    }
    if (!c.rule) o << "no rule\n";
    if (c.repeat) o << "repeat: every fitting rule was blocked\n";
    return o.str();
}

// --- checks ----------------------------------------------------------------------------

ValidationReport validateDialogueRules(const DialogueGenData& d, const Content* c) {
    ValidationReport v;
    std::set<std::string> acts, fallbacks;
    std::map<std::string, std::string> seen; // criteria+slot+say -> rule id
    for (const auto& r : d.rules) {
        const std::string where = r.file + ": rule " + r.id;
        for (const auto& k : r.when)
            if (!knownDialogueFact(k.key)) v.warnings.push_back(where + ": no fact source fills '" + k.key + "'");
        const std::string slot = ruleSlot(r);
        if (slot.empty())
            for (const auto& k : r.when)
                if (k.key == "act" && k.op == Criterion::Op::Eq)
                    for (const auto& a : k.values) acts.insert(a);
        if (isFallback(r)) {
            if (!r.cooldown.empty() || !r.oncePer.empty())
                v.warnings.push_back(where + ": a fallback with cooldown/once_per may leave the act without a line");
            else
                for (const auto& a : r.when[0].values) fallbacks.insert(a);
        }
        std::vector<std::string> crit;
        for (const auto& k : r.when) crit.push_back(k.text);
        std::sort(crit.begin(), crit.end());
        std::string sig = r.say + "#";
        for (const auto& s : crit) sig += s + ";";
        if (auto [it, fresh] = seen.emplace(sig, r.id); !fresh)
            v.warnings.push_back(where + ": same criteria and say as " + it->second);
    }
    for (const auto& a : acts)
        if (!fallbacks.count(a)) v.warnings.push_back("dialogue_rules: act " + a + " has no fallback rule (only act=" + a + ")");
    if (c) {
        std::vector<std::string> errs;
        const std::vector<DialogueStateDef> states = parseDialogueStates(readRuleFiles(c->dir), &errs);
        for (const auto& e : errs) v.errors.push_back(e);
        std::set<std::string> moodlets;
        for (const auto& [id, m] : c->moodlets) moodlets.insert(id);
        for (const auto& s : states)
            for (const auto& m : s.moodlets)
                if (!moodlets.empty() && !moodlets.count(m))
                    v.warnings.push_back("dialogue_rules: state " + s.id + ": unknown moodlet " + m);
    }
    return v;
}

} // namespace lr::sim
