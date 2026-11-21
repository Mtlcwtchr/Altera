// Dialogue generation contract (AL-46): JSON form of the meaning types,
// criteria, content formats and their validator. See dialogue_gen.hpp.

#include "sim/dialogue_gen.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>

#include "sim/loc.hpp"

namespace altera::sim {

namespace {

const std::set<std::string> kRefKinds = {"char", "item", "object", "family", "place", "text", "num", "prop"};
const std::set<std::string> kSources = {"seen", "heard", "rumor", "told", "inferred", "invented"};
const std::set<std::string> kModalities = {"fact", "plan", "wish", "must", "can", "guess"};
const std::set<std::string> kTenses = {"past", "present", "future"};
const std::set<std::string> kMemorySources = {"who0", "who1", "object", "object_owner", "self"};

bool fail(std::string* e, const std::string& m) {
    if (e) *e = m;
    return false;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> splitBar(const std::string& s) {
    std::vector<std::string> out;
    size_t from = 0;
    for (;;) {
        const size_t p = s.find('|', from);
        out.push_back(trim(s.substr(from, p == std::string::npos ? std::string::npos : p - from)));
        if (p == std::string::npos) break;
        from = p + 1;
    }
    return out;
}

bool validKey(const std::string& k) {
    if (k.empty()) return false;
    for (char ch : k)
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '.' || ch == '@')) return false;
    return true;
}

Json readJson(const std::filesystem::path& p, std::vector<std::string>& errors) {
    std::ifstream f(p);
    if (!f) return Json();
    try {
        return Json::parse(f);
    } catch (const std::exception& ex) {
        errors.push_back(p.filename().string() + ": " + ex.what());
        return Json();
    }
}

} // namespace

// --- equality --------------------------------------------------------------

bool Ref::operator==(const Ref& o) const {
    return kind == o.kind && id == o.id && key == o.key && num == o.num && sub == o.sub;
}
bool Prop::operator==(const Prop& o) const {
    return pred == o.pred && roles == o.roles && qty == o.qty && unit == o.unit && neg == o.neg &&
           modality == o.modality && tense == o.tense && tick == o.tick && fact == o.fact;
}
bool Epistemic::operator==(const Epistemic& o) const {
    return source == o.source && confidence == o.confidence && chain == o.chain && age == o.age;
}
bool Stance::operator==(const Stance& o) const {
    return valence == o.valence && emphasis == o.emphasis && politeness == o.politeness;
}
bool Utterance::operator==(const Utterance& o) const {
    return speaker == o.speaker && listener == o.listener && act == o.act && tone == o.tone &&
           intent == o.intent && content == o.content && epi == o.epi && stance == o.stance && seed == o.seed;
}

const Ref* Prop::role(const std::string& r) const {
    auto it = roles.find(r);
    return it == roles.end() ? nullptr : &it->second;
}

// --- JSON ------------------------------------------------------------------
// Compact: defaults are left out, so saves stay small and diffs readable.

Json toJson(const Ref& r) {
    Json j = {{"kind", r.kind}};
    if (r.id != kNone) j["id"] = r.id;
    if (!r.key.empty()) j["key"] = r.key;
    if (r.kind == "num") j["num"] = r.num;
    if (!r.sub.empty()) j["prop"] = toJson(r.sub.front());
    return j;
}

Json toJson(const Prop& p) {
    Json j = {{"pred", p.pred}};
    Json roles = Json::object();
    for (const auto& [k, v] : p.roles) roles[k] = toJson(v);
    j["roles"] = roles;
    if (p.qty != 0.0) j["qty"] = p.qty;
    if (!p.unit.empty()) j["unit"] = p.unit;
    if (p.neg) j["neg"] = true;
    if (p.modality != "fact") j["modality"] = p.modality;
    if (p.tense != "past") j["tense"] = p.tense;
    if (p.tick >= 0) j["tick"] = p.tick;
    if (p.fact != kNone) j["fact"] = p.fact;
    return j;
}

Json toJson(const Epistemic& e) {
    Json j = {{"source", e.source}, {"confidence", e.confidence}};
    if (!e.chain.empty()) j["chain"] = e.chain;
    if (e.age != 0) j["age"] = e.age;
    return j;
}

Json toJson(const Utterance& u) {
    Json j = {{"speaker", u.speaker}, {"listener", u.listener}, {"act", u.act}};
    if (!u.tone.empty()) j["tone"] = u.tone;
    if (!u.intent.empty()) j["intent"] = u.intent;
    if (u.content) j["content"] = toJson(*u.content);
    j["epi"] = toJson(u.epi);
    if (!(u.stance == Stance{}))
        j["stance"] = {{"valence", u.stance.valence}, {"emphasis", u.stance.emphasis},
                       {"politeness", u.stance.politeness}};
    if (u.seed) j["seed"] = u.seed;
    return j;
}

bool fromJson(const Json& j, Ref& out, std::string* e) {
    out = Ref{};
    if (!j.is_object()) return fail(e, "ref: not an object");
    out.kind = j.value("kind", std::string());
    if (!kRefKinds.count(out.kind)) return fail(e, "ref: unknown kind '" + out.kind + "'");
    if (j.contains("id")) {
        if (!j["id"].is_number_unsigned() && !j["id"].is_number_integer()) return fail(e, "ref: id is not a number");
        out.id = j["id"].get<EntityId>();
    }
    if (j.contains("key")) {
        if (!j["key"].is_string()) return fail(e, "ref: key is not a string");
        out.key = j["key"].get<std::string>();
    }
    if (j.contains("num")) {
        if (!j["num"].is_number()) return fail(e, "ref: num is not a number");
        out.num = j["num"].get<double>();
    }
    if (out.kind == "prop") {
        if (!j.contains("prop")) return fail(e, "ref: kind prop without prop");
        Prop p;
        if (!fromJson(j["prop"], p, e)) return false;
        out.sub.push_back(std::move(p));
    }
    if ((out.kind == "char" || out.kind == "object" || out.kind == "family") && out.id == kNone)
        return fail(e, "ref: " + out.kind + " without id");
    if ((out.kind == "item" || out.kind == "place" || out.kind == "text") && out.key.empty())
        return fail(e, "ref: " + out.kind + " without key");
    return true;
}

bool fromJson(const Json& j, Prop& out, std::string* e) {
    out = Prop{};
    if (!j.is_object()) return fail(e, "prop: not an object");
    out.pred = j.value("pred", std::string());
    if (out.pred.empty()) return fail(e, "prop: no pred");
    if (j.contains("roles")) {
        if (!j["roles"].is_object()) return fail(e, "prop " + out.pred + ": roles is not an object");
        for (auto it = j["roles"].begin(); it != j["roles"].end(); ++it) {
            Ref r;
            std::string err;
            if (!fromJson(it.value(), r, &err)) return fail(e, "prop " + out.pred + "." + it.key() + ": " + err);
            out.roles.emplace(it.key(), std::move(r));
        }
    }
    try {
        out.qty = j.value("qty", 0.0);
        out.unit = j.value("unit", std::string());
        out.neg = j.value("neg", false);
        out.modality = j.value("modality", std::string("fact"));
        out.tense = j.value("tense", std::string("past"));
        out.tick = j.value("tick", static_cast<Tick>(-1));
        out.fact = j.value("fact", kNone);
    } catch (const std::exception& ex) {
        return fail(e, "prop " + out.pred + ": " + ex.what());
    }
    if (!kModalities.count(out.modality)) return fail(e, "prop " + out.pred + ": unknown modality " + out.modality);
    if (!kTenses.count(out.tense)) return fail(e, "prop " + out.pred + ": unknown tense " + out.tense);
    return true;
}

bool fromJson(const Json& j, Epistemic& out, std::string* e) {
    out = Epistemic{};
    if (!j.is_object()) return fail(e, "epi: not an object");
    try {
        out.source = j.value("source", std::string("seen"));
        out.confidence = j.value("confidence", 1.0);
        if (j.contains("chain")) out.chain = j["chain"].get<std::vector<EntityId>>();
        out.age = j.value("age", static_cast<Tick>(0));
    } catch (const std::exception& ex) {
        return fail(e, std::string("epi: ") + ex.what());
    }
    if (!kSources.count(out.source)) return fail(e, "epi: unknown source " + out.source);
    if (out.confidence < 0.0 || out.confidence > 1.0) return fail(e, "epi: confidence out of 0..1");
    if (out.chain.size() > Epistemic::kMaxChain) out.chain.resize(Epistemic::kMaxChain);
    return true;
}

bool fromJson(const Json& j, Utterance& out, std::string* e) {
    out = Utterance{};
    if (!j.is_object()) return fail(e, "utterance: not an object");
    try {
        out.speaker = j.value("speaker", kNone);
        out.listener = j.value("listener", kNone);
        out.act = j.value("act", std::string());
        out.tone = j.value("tone", std::string());
        out.intent = j.value("intent", std::string());
        out.seed = j.value("seed", static_cast<std::uint64_t>(0));
        if (j.contains("stance")) {
            const Json& s = j["stance"];
            out.stance.valence = s.value("valence", 0.0);
            out.stance.emphasis = s.value("emphasis", 0.0);
            out.stance.politeness = s.value("politeness", 0.5);
        }
    } catch (const std::exception& ex) {
        return fail(e, std::string("utterance: ") + ex.what());
    }
    if (out.act.empty()) return fail(e, "utterance: no act");
    if (j.contains("content")) {
        Prop p;
        if (!fromJson(j["content"], p, e)) return false;
        out.content = std::move(p);
    }
    if (j.contains("epi") && !fromJson(j["epi"], out.epi, e)) return false;
    return true;
}

// --- criteria --------------------------------------------------------------

bool Criterion::parse(const std::string& raw, Criterion& out, std::string* e) {
    out = Criterion{};
    const std::string s = trim(raw);
    out.text = s;
    if (s.empty()) return fail(e, "empty criterion");
    if (s[0] == '!') {
        out.op = Op::Not;
        out.key = trim(s.substr(1));
        return validKey(out.key) ? true : fail(e, "criterion '" + s + "': bad key");
    }
    static const std::vector<std::pair<std::string, Op>> ops = {
        {"!=", Op::Ne}, {"<=", Op::Le}, {">=", Op::Ge}, {"=", Op::Eq}, {"<", Op::Lt}, {">", Op::Gt}};
    size_t at = std::string::npos;
    std::string opText;
    for (size_t i = 0; i < s.size() && at == std::string::npos; ++i)
        for (const auto& [t, op] : ops)
            if (s.compare(i, t.size(), t) == 0) {
                at = i;
                opText = t;
                out.op = op;
                break;
            }
    if (at == std::string::npos) {
        out.op = Op::Has;
        out.key = s;
        return validKey(s) ? true : fail(e, "criterion '" + s + "': bad key");
    }
    out.key = trim(s.substr(0, at));
    const std::string value = trim(s.substr(at + opText.size()));
    if (!validKey(out.key)) return fail(e, "criterion '" + s + "': bad key");
    if (value.empty()) return fail(e, "criterion '" + s + "': no value");
    if (out.op == Op::Eq || out.op == Op::Ne) {
        out.values = splitBar(value);
        for (const auto& v : out.values)
            if (v.empty()) return fail(e, "criterion '" + s + "': empty alternative");
        return true;
    }
    try {
        size_t used = 0;
        out.number = std::stod(value, &used);
        if (used != value.size()) return fail(e, "criterion '" + s + "': not a number");
    } catch (...) {
        return fail(e, "criterion '" + s + "': not a number");
    }
    return true;
}

namespace {
std::string numText(double v) {
    if (std::floor(v) == v && std::fabs(v) < 1e15) return std::to_string(static_cast<long long>(v));
    return std::to_string(v);
}
} // namespace

bool Criterion::match(const ContextFacts& f) const {
    const auto s = f.str.find(key);
    const auto n = f.num.find(key);
    const auto l = f.list.find(key);
    const bool present = (s != f.str.end() && !s->second.empty()) || (n != f.num.end() && n->second != 0.0) ||
                         (l != f.list.end() && !l->second.empty());
    switch (op) {
    case Op::Has: return present;
    case Op::Not: return !present;
    case Op::Eq:
    case Op::Ne: {
        bool any = false;
        for (const auto& v : values) {
            if (s != f.str.end() && s->second == v) any = true;
            if (n != f.num.end() && numText(n->second) == v) any = true;
            if (l != f.list.end() && std::find(l->second.begin(), l->second.end(), v) != l->second.end()) any = true;
        }
        return op == Op::Eq ? any : !any;
    }
    default: break;
    }
    if (n == f.num.end()) return false; // a comparison needs a number
    const double v = n->second;
    switch (op) {
    case Op::Lt: return v < number;
    case Op::Le: return v <= number;
    case Op::Gt: return v > number;
    case Op::Ge: return v >= number;
    default: return false;
    }
}

// --- content ---------------------------------------------------------------

DialogueGenData DialogueGenData::fromJson(const Json& preds, const std::vector<Json>& ruleFiles, const Json& noise) {
    DialogueGenData d;
    if (noise.is_object()) d.noise = noise;
    if (preds.is_object()) {
        for (const auto& p : preds.value("predicates", Json::array())) {
            if (!p.is_object() || !p.contains("id")) {
                d.loadErrors.push_back("predicates: entry without id");
                continue;
            }
            PredicateDef def;
            try {
                def.id = p["id"].get<std::string>();
                const Json roles = p.value("roles", Json::object());
                for (auto it = roles.begin(); it != roles.end(); ++it)
                    def.roles[it.key()] = it->is_array() ? it->get<std::vector<std::string>>()
                                                         : std::vector<std::string>{it->get<std::string>()};
                def.required = p.value("required", std::vector<std::string>{});
                def.tags = p.value("tags", std::vector<std::string>{});
                def.scale = p.value("scale", std::vector<std::string>{});
            } catch (const std::exception& ex) {
                d.loadErrors.push_back("predicates: " + def.id + ": " + ex.what());
                continue;
            }
            if (!d.predicates.emplace(def.id, def).second) d.loadErrors.push_back("predicates: duplicate " + def.id);
        }
        for (const auto& m : preds.value("memory", Json::array())) {
            MemoryPropMap mp;
            try {
                mp.memoryType = m.at("type").get<std::string>();
                mp.pred = m.at("pred").get<std::string>();
                mp.roles = m.value("roles", std::map<std::string, std::string>{});
                mp.modality = m.value("modality", std::string("fact"));
            } catch (const std::exception& ex) {
                d.loadErrors.push_back(std::string("predicates.memory: ") + ex.what());
                continue;
            }
            d.memory.push_back(std::move(mp));
        }
    }
    for (const auto& file : ruleFiles) {
        const std::string fname = file.value("_file", std::string("rules"));
        for (const auto& r : file.value("rules", Json::array())) {
            DialogueRuleDef def;
            def.file = fname;
            try {
                def.id = r.at("id").get<std::string>();
                def.say = r.value("say", std::string());
                def.weight = r.value("weight", 1.0);
                def.cooldown = r.value("cooldown", std::string());
                def.oncePer = r.value("once_per", std::string());
                for (const auto& w : r.value("when", Json::array())) {
                    Criterion c;
                    std::string err;
                    if (!Criterion::parse(w.get<std::string>(), c, &err)) {
                        d.loadErrors.push_back(fname + ": rule " + def.id + ": " + err);
                        continue;
                    }
                    def.when.push_back(std::move(c));
                }
            } catch (const std::exception& ex) {
                d.loadErrors.push_back(fname + ": rule " + def.id + ": " + ex.what());
                continue;
            }
            d.rules.push_back(std::move(def));
        }
    }
    return d;
}

DialogueGenData DialogueGenData::load(const std::string& contentDir) {
    namespace fs = std::filesystem;
    std::vector<std::string> errors;
    const fs::path dir(contentDir);
    Json preds = readJson(dir / "predicates.json", errors);
    Json noise = readJson(dir / "rumor_noise.json", errors);
    std::vector<Json> rules;
    const fs::path rdir = dir / "dialogue_rules";
    if (fs::is_directory(rdir)) {
        std::vector<fs::path> files;
        for (const auto& ent : fs::directory_iterator(rdir))
            if (ent.path().extension() == ".json") files.push_back(ent.path());
        std::sort(files.begin(), files.end());
        for (const auto& p : files) {
            Json j = readJson(p, errors);
            if (!j.is_object()) continue;
            j["_file"] = "dialogue_rules/" + p.filename().string();
            rules.push_back(std::move(j));
        }
    }
    DialogueGenData d = fromJson(preds, rules, noise);
    d.loadErrors.insert(d.loadErrors.begin(), errors.begin(), errors.end());
    return d;
}

const PredicateDef* DialogueGenData::predicate(const std::string& id) const {
    auto it = predicates.find(id);
    return it == predicates.end() ? nullptr : &it->second;
}

const MemoryPropMap* DialogueGenData::forMemory(const std::string& type) const {
    for (const auto& m : memory)
        if (m.memoryType == type) return &m;
    return nullptr;
}

const DialogueGenData& dialogueGenData(const Content& c) {
    static std::map<std::string, std::unique_ptr<DialogueGenData>> cache;
    auto it = cache.find(c.dir);
    if (it != cache.end()) return *it->second;
    auto d = std::make_unique<DialogueGenData>(c.dir.empty() ? DialogueGenData{} : DialogueGenData::load(c.dir));
    auto* out = d.get();
    cache[c.dir] = std::move(d);
    return *out;
}

namespace {
bool validCooldown(const std::string& s) {
    if (s.empty()) return true;
    const size_t c = s.find(':');
    if (c == std::string::npos) return false;
    const std::string scope = s.substr(0, c), dur = s.substr(c + 1);
    if (scope != "speaker" && scope != "listener" && scope != "pair") return false;
    if (dur.empty()) return false;
    size_t digits = 0;
    while (digits < dur.size() && std::isdigit(static_cast<unsigned char>(dur[digits]))) ++digits;
    if (digits == 0) return false;
    const std::string unit = dur.substr(digits);
    return unit.empty() || unit == "d" || unit == "h";
}
} // namespace

ValidationReport validateDialogueGen(const DialogueGenData& d, const Content* c, const Loc* loc) {
    ValidationReport v;
    for (const auto& e : d.loadErrors) v.errors.push_back(e);
    for (const auto& [id, p] : d.predicates) {
        const std::string where = "predicates." + id;
        for (const auto& [role, kinds] : p.roles) {
            if (kinds.empty()) v.errors.push_back(where + "." + role + ": no kinds");
            for (const auto& k : kinds)
                if (!kRefKinds.count(k)) v.errors.push_back(where + "." + role + ": unknown kind " + k);
        }
        for (const auto& r : p.required)
            if (!p.roles.count(r)) v.errors.push_back(where + ": required role " + r + " is not declared");
        for (const auto& s : p.scale)
            if (!d.predicates.count(s)) v.errors.push_back(where + ": scale names unknown predicate " + s);
    }
    std::set<std::string> seenMem;
    for (const auto& m : d.memory) {
        const std::string where = "predicates.memory." + m.memoryType;
        if (!seenMem.insert(m.memoryType).second) v.errors.push_back(where + ": duplicate");
        const PredicateDef* p = d.predicate(m.pred);
        if (!p) {
            v.errors.push_back(where + ": unknown predicate " + m.pred);
            continue;
        }
        if (!kModalities.count(m.modality)) v.errors.push_back(where + ": unknown modality " + m.modality);
        for (const auto& [role, src] : m.roles) {
            if (!p->roles.count(role)) v.errors.push_back(where + ": predicate " + m.pred + " has no role " + role);
            if (!kMemorySources.count(src)) v.errors.push_back(where + "." + role + ": unknown source " + src);
        }
        for (const auto& r : p->required)
            if (!m.roles.count(r)) v.errors.push_back(where + ": required role " + r + " is not mapped");
    }
    std::set<std::string> ids;
    bool fallback = false;
    for (const auto& r : d.rules) {
        const std::string where = r.file + ": rule " + r.id;
        if (!ids.insert(r.id).second) v.errors.push_back(where + ": duplicate id");
        if (r.say.empty()) v.errors.push_back(where + ": no say");
        if (r.weight <= 0.0) v.errors.push_back(where + ": weight must be > 0");
        if (!validCooldown(r.cooldown)) v.errors.push_back(where + ": bad cooldown '" + r.cooldown + "'");
        if (!r.oncePer.empty() && r.oncePer != "listener" && r.oncePer != "topic" && r.oncePer != "listener+topic")
            v.errors.push_back(where + ": bad once_per '" + r.oncePer + "'");
        for (const auto& k : r.when)
            if (k.key == "pred" && k.op == Criterion::Op::Eq)
                for (const auto& val : k.values)
                    if (!d.predicates.count(val)) v.errors.push_back(where + ": unknown predicate " + val);
        if (c)
            for (const auto& k : r.when)
                if (k.key == "act" && k.op == Criterion::Op::Eq)
                    for (const auto& val : k.values)
                        if (std::find(c->speechActs.begin(), c->speechActs.end(), val) == c->speechActs.end()) v.warnings.push_back(where + ": unknown speech act " + val);
        if (loc && !r.say.empty() && !loc->entry(loc->base(), r.say))
            v.errors.push_back(where + ": loc key " + r.say + " missing in " + loc->base());
        if (r.when.size() == 1 && r.when[0].key == "act" && r.when[0].op == Criterion::Op::Eq) fallback = true;
    }
    if (!d.rules.empty() && !fallback)
        v.warnings.push_back("dialogue_rules: no fallback rule (a rule with only act=<id>)");
    return v;
}

bool checkProp(const DialogueGenData& d, const Prop& p, std::string* e) {
    const PredicateDef* def = d.predicate(p.pred);
    if (!def) return fail(e, "unknown predicate " + p.pred);
    for (const auto& r : def->required)
        if (!p.roles.count(r)) return fail(e, p.pred + ": no role " + r);
    for (const auto& [role, ref] : p.roles) {
        auto it = def->roles.find(role);
        if (it == def->roles.end()) return fail(e, p.pred + ": unknown role " + role);
        if (std::find(it->second.begin(), it->second.end(), ref.kind) == it->second.end())
            return fail(e, p.pred + "." + role + ": kind " + ref.kind + " not allowed");
        if (ref.kind == "prop" && !ref.sub.empty() && !checkProp(d, ref.sub.front(), e)) return false;
    }
    return true;
}

} // namespace altera::sim
