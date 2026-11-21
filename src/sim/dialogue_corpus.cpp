// Dialogue corpus and quality checks (AL-54). See dialogue_corpus.hpp.

#include "sim/dialogue_corpus.hpp"

#include <algorithm>
#include <set>
#include <sstream>

#include "sim/character.hpp"
#include "sim/dialogue_rules.hpp"
#include "sim/dialogue_say.hpp"
#include "sim/loc.hpp"
#include "sim/loc_gen.hpp"
#include "sim/world.hpp"

namespace altera::sim {

namespace {

// --- text helpers (UTF-8, ru + Latin) ------------------------------------------

// Lower case for ASCII and Cyrillic А-Я, Ё.
std::string lower(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const auto b = static_cast<unsigned char>(s[i]);
        if (b < 0x80) {
            o += static_cast<char>(std::tolower(b));
        } else if (b == 0xD0 && i + 1 < s.size()) {
            const auto n = static_cast<unsigned char>(s[i + 1]);
            if (n >= 0x90 && n <= 0x9F) o += static_cast<char>(0xD0), o += static_cast<char>(n + 0x20);
            else if (n >= 0xA0 && n <= 0xAF) o += static_cast<char>(0xD1), o += static_cast<char>(n - 0x20);
            else if (n == 0x81) o += static_cast<char>(0xD1), o += static_cast<char>(0x91);
            else o += s[i], o += s[i + 1];
            ++i;
        } else {
            o += s[i];
        }
    }
    return o;
}

bool isWordByte(unsigned char b) { return std::isalnum(b) || b >= 0x80 || b == '-'; }

// Words in lower case; punctuation, quotes, dashes and the ellipsis split.
std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    const std::string l = lower(s);
    for (size_t i = 0; i < l.size(); ++i) {
        const auto b = static_cast<unsigned char>(l[i]);
        // multi-byte punctuation: « » — – … (C2 AB, C2 BB, E2 80 94/93/A6)
        bool punct = false;
        if (b == 0xC2 && i + 1 < l.size() && (static_cast<unsigned char>(l[i + 1]) == 0xAB || static_cast<unsigned char>(l[i + 1]) == 0xBB)) {
            punct = true;
            i += 1;
        } else if (b == 0xE2 && i + 2 < l.size() && static_cast<unsigned char>(l[i + 1]) == 0x80) {
            punct = true;
            i += 2;
        }
        if (!punct && isWordByte(b)) {
            cur += l[i];
            continue;
        }
        if (!cur.empty()) {
            while (!cur.empty() && cur.back() == '-') cur.pop_back();
            while (!cur.empty() && cur.front() == '-') cur.erase(cur.begin());
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        }
    }
    while (!cur.empty() && cur.back() == '-') cur.pop_back();
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool hasWord(const std::vector<std::string>& ws, const std::string& w) {
    return std::find(ws.begin(), ws.end(), lower(w)) != ws.end();
}

// A form text may be several words ("у мельника"): consecutive words.
bool hasPhrase(const std::vector<std::string>& ws, const std::string& phrase) {
    const std::vector<std::string> p = words(phrase);
    if (p.empty() || p.size() > ws.size()) return false;
    for (size_t i = 0; i + p.size() <= ws.size(); ++i)
        if (std::equal(p.begin(), p.end(), ws.begin() + static_cast<std::ptrdiff_t>(i))) return true;
    return false;
}

bool hasCyrillic(const std::string& s) {
    for (unsigned char b : s)
        if (b == 0xD0 || b == 0xD1) return true;
    return false;
}

bool hasLatin(const std::string& s) {
    for (unsigned char b : s)
        if (std::isalpha(b)) return true;
    return false;
}

std::string str(const Json& j, const char* k) {
    return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string();
}

std::vector<std::string> strings(const Json& j) {
    std::vector<std::string> out;
    if (j.is_string()) out.push_back(j.get<std::string>());
    else if (j.is_array())
        for (const auto& x : j)
            if (x.is_string()) out.push_back(x.get<std::string>());
    return out;
}

// The past forms of a frame's verbs (with synonyms) by gender: m f n pl.
std::map<std::string, std::set<std::string>> framePastForms(const Loc& loc, const std::string& lang,
                                                            const std::string& pred) {
    std::map<std::string, std::set<std::string>> out;
    const Json* fr = loc.find(lang, "frame." + pred);
    if (!fr || !fr->is_object()) return out;
    std::vector<std::string> ids = strings(fr->value("verb", Json()));
    const bool syn = fr->value("syn", true);
    std::set<std::string> all;
    for (const auto& id : ids) {
        all.insert(id);
        if (!syn) continue;
        if (const Json* lx = loc.find(lang, "lex.verb." + id); lx && lx->is_object())
            for (const auto& s : strings(lx->value("syn", Json()))) all.insert(s);
    }
    for (const auto& id : all) {
        const Json* lx = loc.find(lang, "lex.verb." + id);
        if (!lx || !lx->is_object() || !lx->contains("past")) continue;
        const Json& p = (*lx)["past"];
        if (p.is_string()) {
            for (const char* g : {"m", "f", "n", "pl"}) out[g].insert(lower(p.get<std::string>()));
        } else if (p.is_object()) {
            for (auto it = p.begin(); it != p.end(); ++it)
                if (it->is_string()) out[it.key()].insert(lower(it->get<std::string>()));
        }
    }
    return out;
}

// Forms in the line that belong to another gender only (none when `want` is unknown).
std::vector<std::string> wrongForms(const std::map<std::string, std::set<std::string>>& forms,
                                    const std::string& want, const std::vector<std::string>& ws, bool* rightFound) {
    std::vector<std::string> bad;
    if (rightFound) *rightFound = false;
    auto wit = forms.find(want);
    if (wit == forms.end()) return bad;
    for (const auto& f : wit->second)
        if (hasPhrase(ws, f) && rightFound) *rightFound = true;
    for (const auto& [g, set] : forms) {
        if (g == want || g == "n") continue;
        for (const auto& f : set)
            if (!wit->second.count(f) && hasPhrase(ws, f)) bad.push_back(f + " (" + g + ")");
    }
    return bad;
}

// --- the corpus world -----------------------------------------------------------

struct Person {
    const char* name;
    char sex;
    int years;
    const char* status; // "" | noble | clergy
};
// Names of both sexes with the hard ones of the declension (Добрыня: m, -я; Игорь: soft m; Любовь-like f).
const Person kPeople[] = {
    {"Олег", 'm', 34, ""},      {"Брана", 'f', 27, ""},   {"Ждан", 'm', 52, ""},     {"Лада", 'f', 19, ""},
    {"Мирон", 'm', 67, ""},     {"Вера", 'f', 41, ""},    {"Добрыня", 'm', 45, "noble"}, {"Любава", 'f', 63, ""},
    {"Игорь", 'm', 23, "clergy"}, {"Марья", 'f', 38, "noble"}, {"Илья", 'm', 11, ""},  {"Агафья", 'f', 72, ""},
};

struct CorpusWorld {
    World w;
    std::vector<EntityId> people;
    std::vector<EntityId> families;
    CorpusWorld(const Content& c, std::uint64_t seed) : w(c, seed) {}
};

template <class T>
const T& pick(Rng& r, const std::vector<T>& v) {
    return v[static_cast<size_t>(r.irange(0, static_cast<int>(v.size()) - 1))];
}

// Values of criteria `key=v` across the rules (whole-line and part rules).
std::map<std::string, std::vector<std::string>> harvest(const DialogueGenData& d, const std::set<std::string>& keys) {
    std::map<std::string, std::set<std::string>> s;
    for (const auto& r : d.rules)
        for (const auto& c : r.when)
            if (c.op == Criterion::Op::Eq && keys.count(c.key))
                for (const auto& v : c.values) s[c.key].insert(v);
    std::map<std::string, std::vector<std::string>> out;
    for (auto& [k, v] : s) out[k] = std::vector<std::string>(v.begin(), v.end());
    return out;
}

bool ruleNeedsPred(const DialogueRuleDef& r) {
    for (const auto& c : r.when)
        if (c.key == "pred" || c.key == "pred.tag" || c.key == "has_content") return true;
    return false;
}

std::string ruleAct(const DialogueRuleDef& r) {
    for (const auto& c : r.when)
        if (c.key == "act" && c.op == Criterion::Op::Eq && !c.values.empty()) return c.values.front();
    return {};
}

std::string ruleSlotOf(const DialogueRuleDef& r) {
    for (const auto& c : r.when)
        if (c.key == "slot" && c.op == Criterion::Op::Eq && !c.values.empty()) return c.values.front();
    return {};
}

Ref charRef(EntityId id) {
    Ref r;
    r.kind = "char";
    r.id = id;
    return r;
}

struct Pools {
    std::vector<std::string> items;  // items with a loc noun
    std::vector<std::string> preds;  // predicates with roles
    std::vector<std::string> objects;  // object types with a loc noun
    bool game = false;
    std::vector<std::string> interactions, reasons; // v0.1 arguments of the ported lines
};

// A proposition about the corpus world: every declared role filled with some chance,
// the required ones always.
Prop makeProp(Rng& r, const DialogueGenData& d, const CorpusWorld& cw, const Pools& pools, const std::string& pred,
              EntityId speaker, EntityId listener, int depth = 0) {
    Prop p;
    p.pred = pred;
    const PredicateDef* def = d.predicate(pred);
    if (!def) return p;
    std::set<EntityId> used;
    auto person = [&](EntityId prefer) {
        if (prefer != kNone && !used.count(prefer)) return prefer;
        for (int t = 0; t < 20; ++t) {
            EntityId id = pick(r, cw.people);
            if (!used.count(id)) return id;
        }
        return pick(r, cw.people);
    };
    for (const auto& [role, kinds] : def->roles) {
        const bool required = std::find(def->required.begin(), def->required.end(), role) != def->required.end();
        if (!required && !r.chance(0.6)) continue;
        const std::string kind = pick(r, kinds);
        Ref ref;
        ref.kind = kind;
        if (kind == "char") {
            // the doer is now and then the listener ("ты украл") or the speaker ("я взял")
            EntityId prefer = kNone;
            if (role == "doer" || role == "speaker" || role == "thinker") {
                const double x = r.uniform();
                prefer = x < 0.2 ? listener : (x < 0.3 ? speaker : kNone);
            }
            ref.id = person(prefer);
            used.insert(ref.id);
        } else if (kind == "item") {
            if (pools.items.empty()) continue;
            ref.key = pick(r, pools.items);
        } else if (kind == "object") {
            if (pools.game && !pools.items.empty()) { // the game renders an object by its placed id: none here
                ref.kind = "item";
                ref.key = pick(r, pools.items);
                p.roles[role] = ref;
                continue;
            }
            if (pools.objects.empty()) continue;
            ref.key = pick(r, pools.objects); // a type: the corpus world has no placed objects
        } else if (kind == "family") {
            if (cw.families.empty()) continue;
            ref.id = pick(r, cw.families);
        } else if (kind == "text") {
            ref.key = "thing.something";
        } else if (kind == "prop") {
            if (depth > 0) continue;
            std::vector<std::string> inner;
            for (const auto& q : pools.preds)
                if (q != "say" && q != "think") inner.push_back(q);
            if (inner.empty()) continue;
            ref.sub.push_back(makeProp(r, d, cw, pools, pick(r, inner), speaker, listener, depth + 1));
        } else if (kind == "num") {
            ref.num = r.irange(1, 12);
        } else {
            continue;
        }
        p.roles[role] = ref;
    }
    // a victim and a victim family are one or the other
    // a victim or a victim family, as the memory table gives them (both at once is not said)
    if (p.roles.count("victim") && p.roles.count("victim_family")) p.roles.erase(r.chance(0.5) ? "victim_family" : "victim");
    p.neg = r.chance(0.08);
    const double t = r.uniform();
    // the sim says what happened (memory) or what is meant (plans); no present tense yet
    p.tense = t < 0.85 ? "past" : "future";
    if (p.tense == "future") p.modality = r.chance(0.5) ? "plan" : "fact";
    if (r.chance(0.3)) p.tick = static_cast<Tick>(r.irange(0, 20 * static_cast<int>(kTicksPerDay)));
    return p;
}

Epistemic makeEpi(Rng& r, const CorpusWorld& cw, EntityId speaker, EntityId listener) {
    static const std::vector<std::string> sources = {"seen", "seen", "told", "heard", "rumor", "rumor", "inferred", "invented"};
    Epistemic e;
    e.source = pick(r, sources);
    if (e.source == "seen") e.confidence = r.range(0.8, 1.0);
    else if (e.source == "told" || e.source == "heard") e.confidence = r.range(0.45, 0.9);
    else e.confidence = r.range(0.1, 0.85);
    if (e.source == "told" || e.source == "heard" || e.source == "rumor") {
        const int n = r.irange(e.source == "rumor" ? 0 : 1, static_cast<int>(Epistemic::kMaxChain));
        for (int i = 0; i < n; ++i) {
            EntityId id = pick(r, cw.people);
            if (id == speaker || id == listener) continue;
            e.chain.push_back(id);
        }
    }
    e.age = static_cast<Tick>(r.irange(0, 40 * static_cast<int>(kTicksPerDay)));
    return e;
}

Json personArg(const Loc& loc, const std::string& lang, const World& w, EntityId id, bool vy) {
    const Character* c = w.character(id);
    if (!c) return Json::object();
    Json n = loc.personNoun(lang, c->name, c->body.sex);
    n["id"] = id;
    if (vy) {
        n["reg"] = "vy";
        // an address word by sex: the language's "addr.lord" / "addr.lady" when present
        const std::string key = c->body.sex == 'm' ? "addr.lord" : "addr.lady";
        if (loc.has(lang, key)) n["addr"] = loc.noun(lang, key, "");
    }
    return n;
}

bool isNoble(const Character* c) {
    if (!c) return false;
    return std::find(c->traits.begin(), c->traits.end(), "noble") != c->traits.end();
}

Json refNoun(const Loc& loc, const std::string& lang, const CorpusWorld& cw, const Ref& ref, EntityId listener) {
    if (ref.kind == "char") {
        const Character* l = cw.w.character(listener);
        return personArg(loc, lang, cw.w, ref.id, ref.id == listener && isNoble(l));
    }
    if (ref.kind == "item") return loc.noun(lang, "item." + ref.key + ".name", ref.key);
    if (ref.kind == "object") return loc.noun(lang, "object." + ref.key + ".name", ref.key);
    if (ref.kind == "family") {
        auto it = cw.w.families().find(ref.id);
        if (it != cw.w.families().end() && !it->second.members.empty())
            if (const Character* h = cw.w.character(it->second.members.front()))
                return loc.personNoun(lang, h->name, h->body.sex);
        return Json{{"nom", "?"}};
    }
    if (ref.kind == "text") return loc.noun(lang, ref.key, ref.key);
    if (ref.kind == "num") return Json{{"nom", std::to_string(static_cast<long long>(ref.num))}};
    return Json{{"nom", ref.key}};
}

// What the content's verb must agree with: the doer's sex, pl for "вы".
// The role the frame's verb agrees with ({verb:agree=<role>}), when every clause variant agrees with the same one.
std::string agreeRole(const Loc& loc, const std::string& lang, const std::string& pred) {
    const Json* fr = loc.find(lang, "frame." + pred);
    if (!fr || !fr->is_object()) return {};
    std::string role;
    for (const auto& cl : strings(fr->value("clause", Json()))) {
        const size_t p = cl.find("agree=");
        if (p == std::string::npos) return {};
        size_t e = p + 6;
        while (e < cl.size() && (std::isalnum(static_cast<unsigned char>(cl[e])) || cl[e] == '_')) ++e;
        const std::string r = cl.substr(p + 6, e - p - 6);
        if (!role.empty() && r != role) return {};
        role = r;
    }
    return role;
}

// What the content's verb must agree with: the agreeing role's gender (a person's sex,
// pl for "вы", an item noun's g).
std::string verbAgreement(const Loc& loc, const std::string& lang, const CorpusWorld& cw, const Utterance& u) {
    if (!u.content || u.content->tense != "past") return {};
    if (u.content->pred == "say" || u.content->pred == "think") return {};
    const std::string role = agreeRole(loc, lang, u.content->pred);
    const Ref* d = role.empty() ? nullptr : u.content->role(role);
    if (!d) return {};
    if (d->kind == "item" || d->kind == "object") {
        const Json* e = loc.find(lang, d->kind + "." + d->key + ".name");
        const std::string g = e ? str(*e, "g") : std::string();
        return g == "m" || g == "f" || g == "n" ? g : std::string();
    }
    if (d->kind != "char") return {};
    const Character* c = cw.w.character(d->id);
    if (!c) return {};
    if (d->id == u.listener && isNoble(cw.w.character(u.listener))) return "pl";
    return c->body.sex == 'm' ? "m" : "f";
}

double* relAxis(Relation& r, const std::string& a) {
    if (a == "familiarity") return &r.familiarity;
    if (a == "trust") return &r.trust;
    if (a == "affinity") return &r.affinity;
    if (a == "fear") return &r.fear;
    if (a == "debt") return &r.debt;
    if (a == "enmity") return &r.enmity;
    if (a == "respect") return &r.respect;
    return nullptr;
}

// A number that holds the criterion (key<n, key>=n, ...).
double satisfying(const Criterion& c, double lo, double hi) {
    switch (c.op) {
    case Criterion::Op::Lt: return std::max(lo, c.number - 0.1);
    case Criterion::Op::Le: return std::max(lo, c.number - 0.05);
    case Criterion::Op::Gt: return std::min(hi, c.number + 0.1);
    case Criterion::Op::Ge: return std::min(hi, c.number + 0.05);
    case Criterion::Op::Not: return 0.0;
    default: return hi * 0.7;
    }
}

} // namespace

int CorpusReport::errors() const {
    return static_cast<int>(std::count_if(issues.begin(), issues.end(), [](const CorpusIssue& i) { return i.severity == "error"; }));
}
int CorpusReport::warnings() const {
    return static_cast<int>(std::count_if(issues.begin(), issues.end(), [](const CorpusIssue& i) { return i.severity == "warning"; }));
}

// --- generation ---------------------------------------------------------------------

std::vector<CorpusLine> generateDialogueCorpus(const Content& c, const Loc& loc, const CorpusOptions& o) {
    const std::string lang = o.lang.empty() ? loc.base() : o.lang;
    const DialogueGenData& d = dialogueGenData(c);
    installDialogueFacts(); // the memory facts (AL-38 recall rules), as the game's say() has them
    CorpusWorld cw(c, o.seed);
    Rng r(hashCombine(o.seed, hashStr("dialogue_corpus")));

    // people, families of two or three, a status trait on some
    // the traits the rules give a voice to, in turn; others of the content now and then
    std::vector<std::string> traitIds;
    for (const auto& [k, v] : harvest(d, {"speaker.trait", "listener.trait"}))
        for (const auto& t : v)
            if (std::find(traitIds.begin(), traitIds.end(), t) == traitIds.end()) traitIds.push_back(t);
    std::vector<std::string> otherTraits;
    for (const auto& [id, t] : c.traits)
        if (std::find(traitIds.begin(), traitIds.end(), id) == traitIds.end()) otherTraits.push_back(id);
    EntityId fam = kNone;
    int famLeft = 0;
    for (const Person& p : kPeople) {
        if (famLeft == 0) {
            Family f;
            f.id = cw.w.newId();
            f.name = p.name;
            cw.w.families()[f.id] = f;
            cw.families.push_back(f.id);
            fam = f.id;
            famLeft = r.irange(2, 3);
        }
        Character ch;
        ch.name = p.name;
        ch.body.sex = p.sex;
        ch.body.ageDays = p.years * kDaysPerYear;
        ch.status.family = fam;
        if (*p.status) ch.traits.push_back(p.status);
        if (!traitIds.empty()) // two traits each, in turn, so every trait has a voice in the corpus
            for (int k = 0; k < 2; ++k) {
                const std::string& t = traitIds[(cw.people.size() * 2 + static_cast<size_t>(k)) % traitIds.size()];
                if (std::find(ch.traits.begin(), ch.traits.end(), t) == ch.traits.end()) ch.traits.push_back(t);
            }
        if (!otherTraits.empty() && r.chance(0.5)) ch.traits.push_back(pick(r, otherTraits));
        const EntityId id = cw.w.addCharacter(ch).id;
        cw.people.push_back(id);
        for (const auto& a : c.config.reputationAxes)
            if (r.chance(0.4)) cw.w.character(id)->reputation[a] = r.range(-0.9, 0.9);
        cw.w.families()[fam].members.push_back(id);
        --famLeft;
    }
    // relations both ways and memories of misdeeds: AL-38 lines need them
    std::vector<std::string> memTypes;
    {
        std::set<std::string> mt;
        for (const auto& m : d.memory) mt.insert(m.memoryType);
        for (const auto& rule : d.rules)
            for (const auto& cr : rule.when)
                if (cr.key.rfind("memory.", 0) == 0 && cr.key.find('.', 7) == std::string::npos &&
                    cr.key != "memory.deeds" && cr.key != "memory.wariness" && cr.key != "memory.deed" &&
                    cr.key != "memory.kind")
                    mt.insert(cr.key.substr(7));
        memTypes.assign(mt.begin(), mt.end());
    }
    for (EntityId a : cw.people) {
        Character* ca = cw.w.character(a);
        for (EntityId b : cw.people) {
            if (a == b) continue;
            Relation& rel = ca->relations[b];
            rel.affinity = r.range(-0.8, 0.9);
            rel.familiarity = r.range(0.0, 1.0);
            rel.enmity = r.chance(0.25) ? r.range(0.2, 0.9) : 0.0;
            rel.trust = r.range(-0.5, 0.9);
            rel.respect = r.range(-0.5, 0.9);
            if (!memTypes.empty() && r.chance(0.2)) {
                static const std::vector<std::string> src = {"seen", "told", "rumor"};
                MemoryEntry m;
                m.tick = 0;
                m.type = pick(r, memTypes);
                m.who = {b};
                m.valence = -0.4;
                m.salience = 0.5;
                m.source = pick(r, src);
                m.confidence = m.source == "seen" ? 1.0 : r.range(0.3, 0.8);
                ca->memory.push_back(m);
            }
        }
    }

    Pools pools;
    pools.game = o.game;
    for (const auto& [id, it] : c.items)
        if (loc.has(lang, "item." + id + ".name")) pools.items.push_back(id);
    for (const auto& [id, p] : d.predicates)
        if (!p.roles.empty()) pools.preds.push_back(id);
    for (const auto& [id, ob] : c.objects)
        if (loc.has(lang, "object." + id + ".name")) pools.objects.push_back(id);
    for (const auto& [id, it] : c.interactions)
        if (loc.has(lang, "interaction." + id + ".name")) pools.interactions.push_back(id);
    for (const auto& k : loc.keys(lang))
        if (k.rfind("reason.", 0) == 0) {
            const Json* e = loc.find(lang, k);
            if (e && e->is_string() && e->get<std::string>().find('{') == std::string::npos) pools.reasons.push_back(k);
        }

    // what the rules ask for
    std::set<std::string> actsWithPred;
    for (const auto& rule : d.rules)
        if (ruleNeedsPred(rule)) actsWithPred.insert(ruleAct(rule));
    const auto callerVals = harvest(d, {"intent", "reason", "outcome", "mode", "topic.kind", "reply_to", "place",
                                        "commitment", "tone", "speaker.state"});
    // The parts of a line in order; one part of each group is said, the first that has a rule
    // (DIALOGUE_GEN §5.4, AL-51 slots: noise.open + tone.open|voice.open + line + voice.tail|tone.tail).
    std::vector<std::vector<std::string>> slots;
    {
        std::set<std::string> s;
        for (const auto& rule : d.rules)
            if (const std::string sl = ruleSlotOf(rule); !sl.empty()) s.insert(sl);
        auto group = [&](std::initializer_list<const char*> ks) {
            std::vector<std::string> g;
            for (const char* k : ks)
                if (s.erase(k)) g.push_back(k);
            if (!g.empty()) slots.push_back(g);
        };
        group({"noise.open"});
        group({"tone.open", "voice.open"});
        std::vector<std::string> tails;
        for (const auto& k : std::set<std::string>(s))
            if (k.find(".tail") == std::string::npos) slots.push_back({k}), s.erase(k);
        slots.push_back({""}); // the line itself
        group({"voice.tail", "tone.tail"});
        for (const auto& k : s) slots.push_back({k});
    }
    std::vector<std::string> acts = c.speechActs;
    if (acts.empty())
        for (const auto& rule : d.rules)
            if (const std::string a = ruleAct(rule); !a.empty() && std::find(acts.begin(), acts.end(), a) == acts.end())
                acts.push_back(a);
    std::vector<std::string> tones = {""};
    if (auto it = callerVals.find("tone"); it != callerVals.end())
        tones.insert(tones.end(), it->second.begin(), it->second.end());

    std::vector<const DialogueRuleDef*> lineRules;
    for (const auto& rule : d.rules)
        if (ruleSlotOf(rule).empty()) lineRules.push_back(&rule);
    std::sort(lineRules.begin(), lineRules.end(),
              [](const DialogueRuleDef* a, const DialogueRuleDef* b) { return a->id < b->id; });

    SaidMemory said;
    std::vector<CorpusLine> out;
    Tick now = kTicksPerDay * 30;
    EntityId sp = kNone, li = kNone;
    for (int i = 0; i < o.count; ++i) {
        if (i % std::max(1, o.run) == 0) {
            sp = pick(r, cw.people);
            do li = pick(r, cw.people);
            while (li == sp);
        }
        now += r.irange(1, 30);
        CorpusLine L;
        L.index = i;
        Utterance& u = L.u;
        u.speaker = sp;
        u.listener = li;
        u.act = acts.empty() ? "inform" : acts[static_cast<size_t>(i) % acts.size()];
        u.tone = pick(r, tones);
        u.seed = hashCombine(o.seed, static_cast<std::uint64_t>(i));
        // every other line aims at one rule (round robin): the moment gets what the rule asks
        // for, so the corpus reaches the specific rules and not only the fallbacks
        const DialogueRuleDef* target = (o.aim && !lineRules.empty() && i % 2 == 1)
                                            ? lineRules[static_cast<size_t>(i / 2) % lineRules.size()]
                                            : nullptr;
        L.aim = target ? target->id : std::string();
        std::string wantPred, wantTag, wantSource, wantModality;
        bool needContent = false;
        if (target) {
            if (const std::string a = ruleAct(*target); !a.empty()) u.act = a;
            for (const auto& cr : target->when) {
                const std::string v = cr.values.empty() ? std::string() : pick(r, cr.values);
                if (cr.key == "pred" || cr.key == "has_content") needContent = cr.op != Criterion::Op::Not;
                if (cr.key == "pred" && cr.op == Criterion::Op::Eq) wantPred = v;
                if (cr.key == "pred.tag" && cr.op == Criterion::Op::Eq) wantTag = v, needContent = true;
                if (cr.key == "epi.source" && cr.op == Criterion::Op::Eq) wantSource = v, needContent = true;
                if (cr.key == "modality" && cr.op == Criterion::Op::Eq) wantModality = v, needContent = true;
                if (cr.key == "tone" && cr.op == Criterion::Op::Eq) u.tone = v;
                Character* spc = cw.w.character(sp);
                Character* lic = cw.w.character(li);
                if (cr.key.rfind("rel.", 0) == 0 && cr.key.find('.', 4) == std::string::npos)
                    if (double* ax = relAxis(spc->relations[li], cr.key.substr(4))) *ax = satisfying(cr, -1.0, 1.0);
                if (cr.key.rfind("reputation.", 0) == 0 || cr.key.rfind("listener.reputation.", 0) == 0)
                    lic->reputation[cr.key.substr(cr.key.rfind('.') + 1)] = satisfying(cr, -1.0, 1.0);
                if (cr.key.rfind("memory.", 0) == 0 && cr.op != Criterion::Op::Not &&
                    cr.op != Criterion::Op::Lt && cr.op != Criterion::Op::Le) {
                    std::string type = cr.key.substr(7);
                    if (type.find('.') != std::string::npos || type == "deeds" || type == "wariness" || type == "kind")
                        type = type.rfind("kind", 0) == 0 ? "was_treated" : "saw_crime";
                    MemoryEntry m;
                    m.tick = now;
                    m.type = type;
                    m.who = {li};
                    m.valence = type == "was_treated" ? 0.4 : -0.4;
                    m.salience = 0.6;
                    m.source = "seen";
                    spc->memory.push_back(m);
                }
            }
            if (!wantTag.empty() && wantPred.empty())
                for (const auto& [id, pd] : d.predicates)
                    if (std::find(pd.tags.begin(), pd.tags.end(), wantTag) != pd.tags.end()) {
                        wantPred = id;
                        break;
                    }
        }
        const bool withContent = needContent || (!target && r.chance(actsWithPred.count(u.act) ? 0.8 : 0.15));
        if (withContent && !pools.preds.empty()) {
            u.content = makeProp(r, d, cw, pools, wantPred.empty() ? pick(r, pools.preds) : wantPred, sp, li);
            u.epi = makeEpi(r, cw, sp, li);
            if (!wantSource.empty()) u.epi.source = wantSource;
            if (!wantModality.empty()) u.content->modality = wantModality;
            if (target)
                for (const auto& cr : target->when) {
                    if (cr.key == "epi.conf") u.epi.confidence = satisfying(cr, 0.0, 1.0);
                    if (cr.key == "epi.hops" && u.epi.chain.empty() && cr.op != Criterion::Op::Not)
                        for (EntityId id : cw.people)
                            if (id != sp && id != li && u.epi.chain.size() < 2) u.epi.chain.push_back(id);
                    if ((cr.key == "listener.is_doer" || cr.key == "listener.is_victim" || cr.key == "speaker.is_victim" ||
                         cr.key == "speaker.is_doer") && cr.op == Criterion::Op::Has) {
                        const std::string role = cr.key.substr(cr.key.rfind("is_") + 3);
                        const PredicateDef* pd = d.predicate(u.content->pred);
                        if (!pd || !pd->roles.count(role)) continue;
                        u.content->roles[role] = charRef(cr.key.rfind("listener", 0) == 0 ? li : sp);
                        if (role == "victim") u.content->roles.erase("victim_family");
                    }
                }
        } else if (r.chance(0.3)) {
            u.epi = makeEpi(r, cw, sp, li);
        }
        u.stance.valence = r.range(-1.0, 1.0);
        u.stance.politeness = r.range(0.0, 1.0);

        ContextFacts f = dialogueFacts(d, &cw.w, &said, u);
        f.set("time", r.chance(0.3) ? std::string("night") : std::string("day"));
        for (const auto& [k, vals] : callerVals) {
            if (k == "tone") continue;
            if (k == "speaker.state") {
                if (r.chance(0.2)) {
                    const std::string s = pick(r, vals);
                    f.add("speaker.state", s);
                    L.states.push_back(s);
                }
                continue;
            }
            if (r.chance(k == "reason" && u.act == "refuse" ? 0.8 : 0.25)) {
                L.caller[k] = pick(r, vals);
                f.set(k, L.caller[k]);
            }
        }
        if (target)
            for (const auto& cr : target->when) {
                if (cr.op != Criterion::Op::Eq || cr.values.empty()) continue;
                if (cr.key == "speaker.state") {
                    f.add("speaker.state", cr.values.front());
                    L.states.push_back(cr.values.front());
                } else if (callerVals.count(cr.key) && cr.key != "tone") {
                    L.caller[cr.key] = cr.values.front();
                    f.set(cr.key, cr.values.front());
                } else if (cr.key == "time") {
                    f.set("time", cr.values.front());
                }
            }
        L.doerAgree = verbAgreement(loc, lang, cw, u);

        // the loc arguments
        const bool vy = isNoble(cw.w.character(li));
        Json args = Json::object();
        args["speaker"] = personArg(loc, lang, cw.w, sp, false);
        args["listener"] = personArg(loc, lang, cw.w, li, vy);
        args["self"] = args["speaker"]; // v0.1 lines (memory recall) call the speaker self
        L.listenerNoun = args["listener"];
        auto noun = [&](const Ref& ref) { return refNoun(loc, lang, cw, ref, li); };
        if (u.content) args["prop"] = propLocArg(*u.content, noun);
        if (!u.epi.chain.empty()) args["src"] = personArg(loc, lang, cw.w, u.epi.chain.front(), false);
        if (!pools.items.empty()) args["item"] = loc.noun(lang, "item." + pick(r, pools.items) + ".name", "");
        args["price"] = r.irange(2, 40);
        // the v0.1 arguments of the ported dialogue.<template> lines (topic, action, give, get, reason)
        if (!pools.items.empty()) {
            args["give"] = loc.noun(lang, "item." + pick(r, pools.items) + ".name", "");
            args["get"] = loc.noun(lang, "item." + pick(r, pools.items) + ".name", "");
        }
        if (!pools.interactions.empty())
            args["action"] = loc.noun(lang, "interaction." + pick(r, pools.interactions) + ".name", "");
        {
            std::string topicKey = "thing.something";
            if (u.content)
                if (const Json* fr = loc.find(lang, "frame." + u.content->pred); fr && fr->is_object() && fr->contains("noun"))
                    topicKey = str(*fr, "noun");
            args["topic"] = loc.noun(lang, topicKey, "");
        }
        if (!pools.reasons.empty()) args["reason"] = loc.text(lang, pick(r, pools.reasons));
        if (!c.needs.empty()) args["need"] = loc.noun(lang, "need." + pick(r, c.needs).id + ".name", "");
        args["person"] = personArg(loc, lang, cw.w, pick(r, cw.people), false);
        // a whole sentence (dialogue.lie.as_inform is "{text}")
        args["text"] = tidyLine((u.content ? realizeProp(loc, lang, args["prop"], "clause", "", args, u.seed)
                                           : loc.text(lang, "thing.something")) + ".", true);
        args["deadline"] = loc.noun(lang, "hud.daypart.evening", "");
        if (L.caller.count("reason") && L.caller["reason"] == "misdeed") {
            Prop deed = makeProp(r, d, cw, pools, d.predicate("theft") ? "theft" : pick(r, pools.preds), li, sp);
            deed.roles["doer"] = charRef(li);
            deed.tense = "past";
            deed.neg = false;
            args["reason"] = propLocArg(deed, noun);
        }

        std::vector<std::string> pieces;
        const std::string topic = dialogueTopic(u);
        const std::uint64_t lineSeed = hashCombine(hashCombine(u.seed, sp), static_cast<std::uint64_t>(now));
        for (const auto& grp0 : slots) {
            // which part of a group goes first (tone or voice) alternates by the line's seed
            std::vector<std::string> grp = grp0;
            if (grp.size() > 1 && (lineSeed & 1)) std::reverse(grp.begin(), grp.end());
            for (const auto& slot : grp) {
                if (!slot.empty() && !o.parts) break;
                SayContext ctx;
                ctx.speaker = sp;
                ctx.listener = li;
                ctx.topic = topic;
                ctx.now = now;
                ctx.seed = lineSeed;
                ctx.slot = slot;
                RuleChoice ch = chooseRule(d, f, &said, ctx);
                if (slot.empty()) {
                    for (const DialogueRuleDef* fr : ch.fitting) L.fitting.push_back(fr->id);
                    if (!ch.rule) break;
                    L.rule = ch.rule->id;
                    L.say = ch.rule->say;
                    L.repeat = ch.repeat;
                    if (!ch.repeat) said.record(ch.rule->id, sp, li, topic, now);
                } else {
                    if (!ch.rule || ch.repeat) continue; // a part on cooldown is left out
                    L.parts.push_back(ch.rule->id);
                    if (!ch.rule->cooldown.empty() || !ch.rule->oncePer.empty()) said.record(ch.rule->id, sp, li, topic, now);
                }
                if (slot.empty() && o.game) { // the line as the game renders it (dialogue_say utteranceText)
                    Utterance gu = u;
                    gu.seed = lineSeed;
                    if (L.caller.count("intent")) gu.intent = L.caller["intent"];
                    pieces.push_back(utteranceText(loc, lang, cw.w, gu, ch.rule->say));
                } else {
                    pieces.push_back(sayLoc(loc, lang, ch.rule->say, args, lineSeed, &L.errors));
                }
                break;
            }
            if (L.rule.empty() && grp0.size() == 1 && grp0[0].empty()) break;
        }
        std::set<std::string> keep;
        for (const char* k : {"speaker", "listener", "src", "person"})
            if (args.contains(k))
                for (auto it = args[k].begin(); it != args[k].end(); ++it)
                    if (it->is_string())
                        for (const auto& w : words(it->get<std::string>())) keep.insert(w);
        L.text = L.rule.empty() ? std::string() : joinDialogueParts(pieces, keep);
        out.push_back(std::move(L));
    }
    return out;
}

std::string joinDialogueParts(const std::vector<std::string>& pieces, const std::set<std::string>& names) {
    std::string joined;
    for (const auto& p0 : pieces) {
        std::string p = tidyLine(p0, false);
        if (p.empty()) continue;
        if (!joined.empty()) {
            const char e = joined.back();
            const bool ended = e == '.' || e == '!' || e == '?' || e == '"' ||
                               (joined.size() >= 3 && (joined.compare(joined.size() - 3, 3, "\xE2\x80\xA6") == 0 ||
                                                       joined.compare(joined.size() - 3, 3, "\xE2\x80\x94") == 0)) ||
                               (joined.size() >= 2 && joined.compare(joined.size() - 2, 2, "\xC2\xBB") == 0);
            if (!ended) { // the next piece goes on the sentence: its first word in lower case
                const size_t sp = p.find(' ');
                const std::string first = p.substr(0, sp);
                const std::vector<std::string> w = words(first);
                const bool englishI = first == "I" || first.rfind("I'", 0) == 0 || first.rfind("I\xE2\x80\x99", 0) == 0;
                if (!w.empty() && !names.count(w[0]) && !englishI) {
                    const size_t n = (static_cast<unsigned char>(p[0]) < 0x80) ? 1 : 2;
                    p = lower(p.substr(0, n)) + p.substr(n);
                }
            }
            joined += ' ';
        }
        joined += p;
    }
    return tidyLine(joined, true);
}

// --- checks -----------------------------------------------------------------------

std::vector<std::string> lintDialogueLine(const std::string& text, const std::string& lang,
                                         const std::set<std::string>* names) {
    std::vector<std::string> out;
    if (text.empty()) return {"error: empty line"};
    // [missing.key]
    for (size_t p = text.find('['); p != std::string::npos; p = text.find('[', p + 1)) {
        const size_t q = text.find(']', p);
        if (q == std::string::npos) break;
        const std::string inner = text.substr(p + 1, q - p - 1);
        if (!inner.empty() && std::all_of(inner.begin(), inner.end(), [](unsigned char b) {
                return std::isalnum(b) || b == '.' || b == '_';
            }))
            out.push_back("error: missing key [" + inner + "]");
    }
    if (text.find('{') != std::string::npos || text.find('}') != std::string::npos)
        out.push_back("error: unrendered placeholder");
    if (text.find('|') != std::string::npos) out.push_back("error: stray '|'");
    for (size_t p = text.find('#'); p != std::string::npos; p = text.find('#', p + 1))
        if (p + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[p + 1]))) {
            out.push_back("error: an id in the text");
            break;
        }
    if (text.find("null") != std::string::npos) out.push_back("error: 'null' in the text");
    // style
    {   // a word twice with nothing between ("украл украл"); "Ха! Ха!", "Не, не" are said on purpose
        std::istringstream in(text);
        std::string prev, tok;
        while (in >> tok) {
            const std::vector<std::string> w = words(tok);
            const bool bare = !tok.empty() && isWordByte(static_cast<unsigned char>(tok.back()));
            if (!prev.empty() && w.size() == 1 && w[0] == prev) {
                out.push_back("warning: doubled word '" + prev + "'");
                break;
            }
            prev = (bare && w.size() == 1) ? w[0] : std::string();
        }
    }
    for (const char* bad : {",,", ",.", ".,", "!.", "?.", ":.", ",!", ",?", ":,", ";;", " ,", " .", "  "})
        if (text.find(bad) != std::string::npos) {
            out.push_back(std::string("warning: punctuation '") + bad + "'");
            break;
        }
    {
        const size_t dots = text.find("..");
        if (dots != std::string::npos && text.compare(dots, 3, "...") != 0)
            out.push_back("warning: punctuation '..'");
    }
    {
        std::string t = text;
        while (!t.empty() && (t.back() == ' ' || t.back() == '"')) t.pop_back();
        const bool end = !t.empty() && (t.back() == '.' || t.back() == '!' || t.back() == '?' ||
                                        (t.size() >= 3 && t.compare(t.size() - 3, 3, "\xE2\x80\xA6") == 0) ||
                                        (t.size() >= 2 && t.compare(t.size() - 2, 2, "\xC2\xBB") == 0));
        if (!end) out.push_back("warning: no final punctuation");
    }
    if (names) { // a capital inside a sentence that is not a name ("Прошу тебя, Вынуждена отказать")
        std::istringstream in(text);
        std::string prev, tok;
        bool first = true;
        while (in >> tok) {
            const auto b0 = static_cast<unsigned char>(tok[0]);
            const auto b1 = tok.size() > 1 ? static_cast<unsigned char>(tok[1]) : 0;
            const bool upper = std::isupper(b0) || (b0 == 0xD0 && ((b1 >= 0x90 && b1 <= 0xAF) || b1 == 0x81));
            bool sentenceEnd = first;
            if (!prev.empty()) {
                const char e = prev.back();
                sentenceEnd = sentenceEnd || e == '.' || e == '!' || e == '?' || e == '"' ||
                              (prev.size() >= 3 && (prev.compare(prev.size() - 3, 3, "\xE2\x80\xA6") == 0 ||
                                                    prev.compare(prev.size() - 3, 3, "\xE2\x80\x94") == 0)) ||
                              (prev.size() >= 2 && prev.compare(prev.size() - 2, 2, "\xC2\xBB") == 0);
            }
            if (upper && !sentenceEnd) {
                const std::vector<std::string> w = words(tok);
                if (!w.empty() && !names->count(w[0]) && w[0] != "я" && w[0] != "i" && w[0].rfind("i'", 0) != 0) {
                    out.push_back("warning: capital inside a sentence '" + tok + "'");
                    break;
                }
            }
            first = false;
            prev = tok;
        }
    }
    if (lang == "ru" && hasLatin(text)) out.push_back("warning: Latin letters in a ru line");
    if (lang == "en") { // the names of people may be Cyrillic, the words around them not
        bool cyr = false;
        for (const auto& w : words(text))
            if (hasCyrillic(w) && !(names && names->count(w))) cyr = true;
        if (cyr) out.push_back("warning: Cyrillic in an en line");
    }
    if (text.size() > 400) out.push_back("warning: a very long line");
    return out;
}

std::vector<CorpusIssue> checkFrameAgreement(const Content& c, const Loc& loc, const std::string& lang0, int* renders) {
    const std::string lang = lang0.empty() ? loc.base() : lang0;
    const DialogueGenData& d = dialogueGenData(c);
    std::vector<CorpusIssue> out;
    int n = 0;
    auto issue = [&](const std::string& sev, const std::string& msg) { out.push_back({sev, "agreement", -1, msg}); };

    // items of each gender with a noun in this language
    std::map<std::string, std::string> itemByG;
    for (const auto& [id, it] : c.items) {
        const Json* e = loc.find(lang, "item." + id + ".name");
        if (!e || !e->is_object()) continue;
        const std::string g = str(*e, "g");
        if (!g.empty() && !itemByG.count(g)) itemByG[g] = id;
    }
    if (itemByG.empty())
        for (const auto& [id, it] : c.items)
            if (loc.has(lang, "item." + id + ".name")) {
                itemByG["?"] = id;
                break;
            }
    struct Name {
        const char* name;
        char sex;
    };
    const Name names[] = {{"Олег", 'm'}, {"Добрыня", 'm'}, {"Игорь", 'm'}, {"Брана", 'f'}, {"Любава", 'f'}, {"Марья", 'f'}};

    for (const auto& [pred, def] : d.predicates) {
        const Json* fr = loc.find(lang, "frame." + pred);
        if (!fr || !fr->is_object()) continue;
        auto dk = def.roles.find("doer");
        if (dk == def.roles.end() || std::find(dk->second.begin(), dk->second.end(), "char") == dk->second.end())
            continue;
        const auto forms = framePastForms(loc, lang, pred);
        const Json gov = fr->value("roles", Json::object());
        const std::vector<std::string> clauses = strings(fr->value("clause", Json()));
        // roles every clause variant names: those must show in their government case
        std::set<std::string> inAll;
        for (const auto& [role, kinds] : def.roles) {
            bool all = !clauses.empty();
            for (const auto& cl : clauses)
                if (cl.find("{" + role) == std::string::npos) all = false;
            if (all) inAll.insert(role);
        }
        int k = 0;
        for (const Name& doer : names) {
            const Name& other = names[(static_cast<size_t>(k) + 3) % 6];
            const std::string item = itemByG.empty() ? std::string() : std::next(itemByG.begin(), k % static_cast<int>(itemByG.size()))->second;
            ++k;
            Json doerN = loc.personNoun(lang, doer.name, doer.sex);
            doerN["id"] = 9001;
            Json otherN = loc.personNoun(lang, other.name, other.sex);
            otherN["id"] = 9002;
            Json roles = Json::object();
            std::map<std::string, Json> filled;
            for (const auto& [role, kinds] : def.roles) {
                if (role == "doer") filled[role] = doerN;
                else if (std::find(kinds.begin(), kinds.end(), "char") != kinds.end() && role != "object") filled[role] = otherN;
                else if (!item.empty() && (std::find(kinds.begin(), kinds.end(), "item") != kinds.end() ||
                                           std::find(kinds.begin(), kinds.end(), "object") != kinds.end()))
                    filled[role] = loc.noun(lang, "item." + item + ".name", item);
                else if (std::find(kinds.begin(), kinds.end(), "family") != kinds.end()) filled[role] = otherN;
                else if (std::find(kinds.begin(), kinds.end(), "char") != kinds.end()) filled[role] = otherN;
            }
            // victim and victim_family are one or the other
            if (filled.count("victim") && filled.count("victim_family")) filled.erase("victim_family");
            for (auto& [role, v] : filled) roles[role] = v;
            const std::string want = doer.sex == 'm' ? "m" : "f";
            for (int neg = 0; neg < 2; ++neg)
                for (std::uint64_t seed = 0; seed < 4; ++seed) {
                    Json prop = {{"pred", pred}, {"tense", "past"}, {"neg", neg == 1}, {"modality", "fact"}, {"roles", roles}};
                    std::vector<std::string> errs;
                    const std::string where = "frame." + pred + " (" + doer.name + (item.empty() ? "" : ", " + item) +
                                              (neg ? ", neg" : "") + ", seed " + std::to_string(seed) + ")";
                    const std::string text = realizeProp(loc, lang, prop, "clause", "", Json::object(), seed, &errs);
                    ++n;
                    for (const auto& e : errs) issue("error", where + ": " + e);
                    for (const auto& l : lintDialogueLine(text + ".", lang))
                        if (l.rfind("error:", 0) == 0) issue("error", where + ": " + l.substr(7) + ": " + text);
                    const std::vector<std::string> ws = words(text);
                    if (!hasPhrase(ws, str(doerN, "nom")))
                        issue("error", where + ": the doer is not in nom: " + text);
                    bool right = false;
                    const auto bad = wrongForms(forms, want, ws, &right);
                    // a negated verb may take another form (en "did not steal"); only the wrong gender counts then
                    if (!forms.empty() && !right && !neg) issue("error", where + ": no verb form for " + want + ": " + text);
                    for (const auto& b : bad) issue("error", where + ": verb of the other gender " + b + ": " + text);
                    for (const auto& role : inAll) {
                        if (role == "doer" || !filled.count(role)) continue;
                        const std::string g = str(gov, role.c_str());
                        if (g.empty() || g == "nom") continue;
                        const size_t plus = g.find('+');
                        const std::string prep = plus == std::string::npos ? "" : g.substr(0, plus);
                        const std::string gcase = plus == std::string::npos ? g : g.substr(plus + 1);
                        const Json& rn = filled[role];
                        std::string form = str(rn, gcase.c_str());
                        // accusative of an animate noun is its genitive
                        if (gcase == "acc" && form.empty()) form = str(rn, "nom");
                        if (form.empty()) continue;
                        const bool ok = prep.empty() ? hasPhrase(ws, form)
                                                     : (hasPhrase(ws, prep + " " + form) || hasPhrase(ws, prep + "о " + form) ||
                                                        hasPhrase(ws, prep + "б " + form));
                        if (!ok) issue("error", where + ": role " + role + " not in " + g + " ('" + (prep.empty() ? "" : prep + " ") + form + "'): " + text);
                    }
                }
            // the speaker as the doer: я + the speaker's gender
            {
                Json args = {{"speaker", doerN}, {"listener", otherN}};
                Json prop = {{"pred", pred}, {"tense", "past"}, {"neg", false}, {"modality", "fact"}, {"roles", roles}};
                std::vector<std::string> errs;
                const std::string text = realizeProp(loc, lang, prop, "clause", "", args, 0, &errs);
                ++n;
                const std::vector<std::string> ws = words(text);
                const std::string where = "frame." + pred + " (speaker " + doer.name + " as doer)";
                for (const auto& e : errs) issue("error", where + ": " + e);
                const Json* gr = loc.grammar(lang);
                const bool pronouns = gr && gr->contains("pronouns") && (*gr)["pronouns"].contains("1s");
                if (pronouns && !hasWord(ws, str((*gr)["pronouns"]["1s"], "nom").empty()
                                                 ? std::string("я")
                                                 : str((*gr)["pronouns"]["1s"], "nom")))
                    issue("error", where + ": no 1st person pronoun: " + text);
                for (const auto& b : wrongForms(forms, want, ws, nullptr))
                    issue("error", where + ": verb of the other gender " + b + ": " + text);
            }
            // nominal and about render clean
            for (const char* form : {"nominal", "about"}) {
                if (!fr->contains(form)) continue;
                Json prop = {{"pred", pred}, {"tense", "past"}, {"neg", false}, {"modality", "fact"}, {"roles", roles}};
                std::vector<std::string> errs;
                const std::string text = realizeProp(loc, lang, prop, form, "", Json::object(), 0, &errs);
                ++n;
                const std::string where = "frame." + pred + ":" + form + " (" + doer.name + ")";
                for (const auto& e : errs) issue("error", where + ": " + e);
                for (const auto& l : lintDialogueLine(text + ".", lang))
                    if (l.rfind("error:", 0) == 0) issue("error", where + ": " + l.substr(7) + ": " + text);
            }
        }
    }
    if (renders) *renders = n;
    return out;
}

CorpusReport checkDialogueCorpus(const std::vector<CorpusLine>& lines, const Content& c, const Loc& loc,
                                 const std::string& lang0, int run) {
    const std::string lang = lang0.empty() ? loc.base() : lang0;
    const DialogueGenData& d = dialogueGenData(c);
    CorpusReport rep;
    rep.lines = static_cast<int>(lines.size());
    std::set<std::string> texts, bigrams;
    std::map<std::string, std::set<std::string>> perActTexts;
    size_t bigramTotal = 0;
    std::set<std::string> rulesFired, partsFired, sayUsed, rulesFit;
    std::map<std::string, std::map<std::string, std::set<std::string>>> verbForms; // pred -> forms
    auto formsOf = [&](const std::string& pred) -> const std::map<std::string, std::set<std::string>>& {
        auto it = verbForms.find(pred);
        if (it == verbForms.end()) it = verbForms.emplace(pred, framePastForms(loc, lang, pred)).first;
        return it->second;
    };
    // only a line that renders the content ({>prop...}) is checked against the content's verb
    std::map<std::string, bool> propKeys;
    auto saysProp = [&](const std::string& key) {
        auto it = propKeys.find(key);
        if (it != propKeys.end()) return it->second;
        bool yes = false;
        if (const Json* e = loc.find(lang, key))
            for (const auto& pat : strings(*e)) yes = yes || pat.find("{>prop") != std::string::npos;
        return propKeys[key] = yes;
    };
    // the names of the corpus people in every case, for the capital check
    std::set<std::string> names;
    for (const Person& p : kPeople) {
        const Json n = loc.personNoun(lang, p.name, p.sex);
        for (auto it = n.begin(); it != n.end(); ++it)
            if (it->is_string())
                for (const auto& w : words(it->get<std::string>())) names.insert(w);
    }
    // words the authors capitalize inside a sentence themselves (Бог, Господи): not an error of gluing
    for (const auto& k : loc.keys(lang)) {
        const Json* e = loc.entry(lang, k);
        if (!e) continue;
        for (const auto& pat : strings(*e)) {
            std::istringstream in(pat);
            std::string prev, tok;
            while (in >> tok) {
                const auto b0 = static_cast<unsigned char>(tok[0]);
                const auto b1 = tok.size() > 1 ? static_cast<unsigned char>(tok[1]) : 0;
                const bool upper = std::isupper(b0) || (b0 == 0xD0 && ((b1 >= 0x90 && b1 <= 0xAF) || b1 == 0x81));
                const char pe = prev.empty() ? '.' : prev.back();
                if (upper && pe != '.' && pe != '!' && pe != '?' && pe != '}')
                    if (const auto w = words(tok); !w.empty()) names.insert(w[0]);
                prev = tok;
            }
        }
    }
    // a run of talk: the same pair in consecutive lines
    std::map<std::string, int> runTexts;
    EntityId runS = kNone, runL = kNone;
    int runStart = 0;
    for (const auto& L : lines) {
        auto add = [&](const std::string& sev, const std::string& check, const std::string& msg) {
            rep.issues.push_back({sev, check, L.index, msg + " | " + L.rule + " -> " + L.say + " | " + L.text});
        };
        if (L.u.speaker != runS || L.u.listener != runL || L.index - runStart >= run) {
            runTexts.clear();
            runS = L.u.speaker;
            runL = L.u.listener;
            runStart = L.index;
        }
        if (L.rule.empty()) {
            add("error", "broken", "no rule for act " + L.u.act);
            continue;
        }
        rulesFired.insert(L.rule);
        rulesFit.insert(L.fitting.begin(), L.fitting.end());
        sayUsed.insert(L.say);
        for (const auto& p : L.parts) partsFired.insert(p);
        ++rep.perAct[L.u.act];
        perActTexts[L.u.act].insert(L.text);
        texts.insert(L.text);
        for (const auto& e : L.errors) add("error", "broken", "render: " + e);
        for (const auto& l : lintDialogueLine(L.text, lang, &names)) {
            const bool err = l.rfind("error:", 0) == 0;
            add(err ? "error" : "warning", err ? "broken" : "style", l.substr(err ? 7 : 9));
        }
        const std::vector<std::string> ws = words(L.text);
        { // the listener named twice ("Эх, Лада, пойдём ..., Лада?")
            const Json ln = L.listenerNoun;
            const std::string nom = lower(str(ln, "nom"));
            if (!nom.empty() && std::count(ws.begin(), ws.end(), nom) >= 2)
                add("warning", "style", "the listener named twice");
        }
        {   // two colons in one sentence: an opener ending in ':' before a line with its own ':'
            int colons = 0;
            for (char ch : L.text) {
                if (ch == '.' || ch == '!' || ch == '?') colons = 0;
                else if (ch == ':' && ++colons == 2) {
                    add("warning", "style", "two colons in one sentence");
                    break;
                }
            }
        }
        for (size_t i = 1; i < ws.size(); ++i) bigrams.insert(ws[i - 1] + " " + ws[i]);
        bigramTotal += ws.empty() ? 0 : ws.size() - 1;
        if (!L.doerAgree.empty() && L.u.content && saysProp(L.say)) {
            const auto& forms = formsOf(L.u.content->pred);
            if (!forms.empty()) {
                ++rep.agreementChecked;
                for (const auto& b : wrongForms(forms, L.doerAgree, ws, nullptr))
                    add("error", "agreement", "verb " + b + ", the doer wants " + L.doerAgree);
            }
        }
        if (!L.repeat && ++runTexts[L.text] == 2) add("warning", "repeat", "said twice to the same listener in a row");
    }
    rep.distinct = static_cast<int>(texts.size());
    rep.dupShare = rep.lines ? 1.0 - static_cast<double>(rep.distinct) / rep.lines : 0.0;
    rep.distinct2 = bigramTotal ? static_cast<double>(bigrams.size()) / static_cast<double>(bigramTotal) : 0.0;
    for (const auto& [a, s] : perActTexts) rep.distinctPerAct[a] = static_cast<int>(s.size());
    for (const auto& rule : d.rules) {
        if (ruleSlotOf(rule).empty()) {
            ++rep.rulesTotal;
            if (rulesFired.count(rule.id)) ++rep.rulesFired;
            else (rulesFit.count(rule.id) ? rep.rulesShadowed : rep.rulesNever).push_back(rule.id);
        } else {
            ++rep.partsTotal;
            if (partsFired.count(rule.id)) ++rep.partsFired;
            else rep.partsNever.push_back(rule.id);
        }
    }
    for (const auto& k : loc.keys(lang)) {
        if (k.rfind("say.", 0) != 0) continue;
        ++rep.sayKeysTotal;
        if (sayUsed.count(k)) ++rep.sayKeysUsed;
        else rep.sayNever.push_back(k);
    }
    int renders = 0;
    for (auto& i : checkFrameAgreement(c, loc, lang, &renders)) rep.issues.push_back(std::move(i));
    rep.frameChecks = renders;
    return rep;
}

// --- output -----------------------------------------------------------------------

std::string corpusText(const std::vector<CorpusLine>& lines) {
    std::ostringstream o;
    char buf[32];
    for (const auto& L : lines) {
        o << "#" << L.index << " " << L.u.act;
        if (!L.u.tone.empty()) o << "/" << L.u.tone;
        if (L.u.content) {
            std::snprintf(buf, sizeof buf, "%.2f", L.u.epi.confidence);
            o << " " << L.u.content->pred << (L.u.content->neg ? "!" : "") << " " << L.u.epi.source << " " << buf;
            if (L.u.epi.hops()) o << " h" << L.u.epi.hops();
        }
        for (const auto& [k, v] : L.caller) o << " " << k << "=" << v;
        for (const auto& s : L.states) o << " state=" << s;
        o << " | " << (L.rule.empty() ? "-" : L.rule) << (L.repeat ? " (repeat)" : "");
        for (const auto& p : L.parts) o << " +" << p;
        o << " | " << L.text << "\n";
    }
    return o.str();
}

Json corpusJson(const std::vector<CorpusLine>& lines) {
    Json a = Json::array();
    for (const auto& L : lines) {
        Json j = {{"i", L.index}, {"utterance", toJson(L.u)}, {"rule", L.rule}, {"say", L.say},
                  {"repeat", L.repeat}, {"parts", L.parts}, {"text", L.text}};
        if (!L.caller.empty()) j["caller"] = L.caller;
        if (!L.states.empty()) j["states"] = L.states;
        if (!L.errors.empty()) j["errors"] = L.errors;
        a.push_back(std::move(j));
    }
    return a;
}

Json toJson(const CorpusReport& r) {
    Json issues = Json::array();
    for (const auto& i : r.issues)
        issues.push_back({{"severity", i.severity}, {"check", i.check}, {"line", i.line}, {"message", i.message}});
    return {{"lines", r.lines},
            {"distinct", r.distinct},
            {"dup_share", r.dupShare},
            {"distinct2", r.distinct2},
            {"per_act", r.perAct},
            {"distinct_per_act", r.distinctPerAct},
            {"rules", {{"total", r.rulesTotal}, {"fired", r.rulesFired}, {"never_fit", r.rulesNever}, {"shadowed", r.rulesShadowed}}},
            {"parts", {{"total", r.partsTotal}, {"fired", r.partsFired}, {"never", r.partsNever}}},
            {"say_keys", {{"total", r.sayKeysTotal}, {"used", r.sayKeysUsed}, {"never", r.sayNever}}},
            {"agreement_checked", r.agreementChecked},
            {"frame_checks", r.frameChecks},
            {"errors", r.errors()},
            {"warnings", r.warnings()},
            {"issues", issues}};
}

std::string reportText(const CorpusReport& r, size_t maxIssues) {
    std::ostringstream o;
    char buf[64];
    o << "lines " << r.lines << ", distinct " << r.distinct;
    std::snprintf(buf, sizeof buf, " (dup %.1f%%), distinct bigrams %.2f\n", 100.0 * r.dupShare, r.distinct2);
    o << buf;
    o << "rules fired " << r.rulesFired << "/" << r.rulesTotal << ", parts " << r.partsFired << "/" << r.partsTotal
      << ", say keys used " << r.sayKeysUsed << "/" << r.sayKeysTotal << "\n";
    o << "agreement: " << r.agreementChecked << " corpus lines, " << r.frameChecks << " frame renders\n";
    o << "per act (lines/distinct):";
    for (const auto& [a, n] : r.perAct) o << " " << a << " " << n << "/" << r.distinctPerAct.at(a);
    o << "\n";
    std::map<std::string, int> byCheck;
    for (const auto& i : r.issues) ++byCheck[i.severity + " " + i.check];
    o << "errors " << r.errors() << ", warnings " << r.warnings();
    for (const auto& [k, n] : byCheck) o << "; " << k << " " << n;
    o << "\n";
    size_t shown = 0;
    for (const char* sev : {"error", "warning"})
        for (const auto& i : r.issues) {
            if (i.severity != sev) continue;
            if (shown++ >= maxIssues) break;
            o << "  " << i.severity << " [" << i.check << "]" << (i.line >= 0 ? " #" + std::to_string(i.line) : "")
              << " " << i.message << "\n";
        }
    if (r.issues.size() > maxIssues) o << "  ... " << r.issues.size() - maxIssues << " more\n";
    auto list = [&](const char* what, const std::vector<std::string>& v) {
        if (v.empty()) return;
        o << what << " (" << v.size() << "):";
        for (size_t i = 0; i < v.size() && i < 40; ++i) o << " " << v[i];
        o << (v.size() > 40 ? " ..." : "") << "\n";
    };
    list("rules that never fit a moment", r.rulesNever);
    list("rules that fit but always lost to a more specific one", r.rulesShadowed);
    list("part rules never said", r.partsNever);
    list("say keys never used", r.sayNever);
    return o.str();
}

} // namespace altera::sim
