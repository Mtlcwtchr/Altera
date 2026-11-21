// Localization tables, the placeholder formatter, the loc validator and the
// output-time assembly of trace / report text (doc/design/LOC_FORMAT.md).

#include "sim/dialogue_say.hpp"
#include "sim/loc.hpp"

#include "sim/village_life.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "sim/loc_gen.hpp"
#include "sim/world.hpp"

namespace altera::sim {

namespace {

const std::vector<std::string> kCases = {"nom", "gen", "dat", "acc", "ins", "pre"};
const std::vector<std::string> kGenders = {"m", "f", "n", "pl"};

bool isCase(const std::string& s) { return std::find(kCases.begin(), kCases.end(), s) != kCases.end(); }

std::string numberText(double v, const std::string& lang) {
    if (std::abs(v - std::round(v)) < 1e-9) return std::to_string(static_cast<long long>(std::llround(v)));
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (lang == "ru") std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

// Plain rendering of an argument in a case (nom when the form is absent).
std::string valueText(const Json& v, const std::string& gcase, const std::string& lang) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number()) return numberText(v.get<double>(), lang);
    if (v.is_boolean()) return v.get<bool>() ? "1" : "0";
    if (v.is_object()) {
        if (!gcase.empty() && v.contains(gcase) && v[gcase].is_string()) return v[gcase].get<std::string>();
        if (v.contains("nom") && v["nom"].is_string()) return v["nom"].get<std::string>();
        return "";
    }
    if (v.is_array()) {
        std::string out;
        for (const auto& x : v) {
            if (!out.empty()) out += ", ";
            out += valueText(x, gcase, lang);
        }
        return out;
    }
    return "";
}

// One value of a table: a pattern, a noun ({nom, ...}), a list of variants, or
// (LOC v0.2) everything under lex.<pos>.<id> and frame.<id>.
bool isLeaf(const std::string& prefix, const Json& j) {
    if (j.is_string() || j.is_array() || (j.is_object() && j.contains("nom"))) return true;
    const size_t dots = static_cast<size_t>(std::count(prefix.begin(), prefix.end(), '.'));
    return (prefix.rfind("lex.", 0) == 0 && dots == 2) || (prefix.rfind("frame.", 0) == 0 && dots == 1);
}

void flatten(const std::string& prefix, const Json& j, std::map<std::string, Json>& out, const std::string& file,
             std::vector<std::string>& warnings) {
    if (!prefix.empty() && isLeaf(prefix, j)) {
        if (prefix.empty()) return warnings.push_back(file + ": a bare value at the top level");
        if (!out.emplace(prefix, j).second) warnings.push_back(file + ": duplicate key '" + prefix + "'");
        return;
    }
    if (j.is_string() || j.is_array() || (j.is_object() && j.contains("nom")))
        return warnings.push_back(file + ": a bare value at the top level");
    if (!j.is_object()) return warnings.push_back(file + ": '" + prefix + "' must be a string, noun or object");
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key() == "_doc") continue;
        flatten(prefix.empty() ? it.key() : prefix + "." + it.key(), *it, out, file, warnings);
    }
}

Json readJsonFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + p.string());
    try {
        return Json::parse(f);
    } catch (const std::exception& e) {
        throw std::runtime_error(p.string() + ": " + e.what());
    }
}

// --- UTF-8 <-> code points (name declension works on letters) ---------------
std::u32string toU32(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp = c;
        int n = 0;
        if (c >= 0xF0) cp = c & 0x07, n = 3;
        else if (c >= 0xE0) cp = c & 0x0F, n = 2;
        else if (c >= 0xC0) cp = c & 0x1F, n = 1;
        ++i;
        for (int k = 0; k < n && i < s.size(); ++k, ++i) cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
        out.push_back(cp);
    }
    return out;
}
std::string toU8(const std::u32string& s) {
    std::string out;
    for (char32_t cp : s) {
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}
char32_t lowerRu(char32_t c) {
    if (c >= U'А' && c <= U'Я') return c + 32; // А..Я
    if (c == U'Ё') return U'ё';                 // Ё
    return c;
}

// One alternative of a rule's `match`: ".*" (anything), "[...]" (a class of the
// last letter) or a literal ending.
bool matchEnding(const std::u32string& word, const std::u32string& alt) {
    if (alt == U".*") return true;
    if (word.empty()) return false;
    if (alt.size() >= 2 && alt.front() == U'[' && alt.back() == U']') {
        const char32_t last = lowerRu(word.back());
        return alt.find(last, 1) < alt.size() - 1;
    }
    if (alt.size() > word.size()) return false;
    for (size_t i = 0; i < alt.size(); ++i)
        if (lowerRu(word[word.size() - alt.size() + i]) != alt[i]) return false;
    return true;
}
bool matchRule(const std::u32string& word, const std::string& match) {
    const std::u32string m = toU32(match);
    size_t from = 0;
    while (true) {
        const size_t bar = m.find(U'|', from);
        if (matchEnding(word, m.substr(from, bar == std::u32string::npos ? std::u32string::npos : bar - from)))
            return true;
        if (bar == std::u32string::npos) return false;
        from = bar + 1;
    }
}

// Case forms of one word by the grammar rules; nullopt-like empty when no rule.
Json declineWord(const Json& grammar, const std::string& word, char sex) {
    static const char* kCases[] = {"gen", "dat", "acc", "ins", "pre"};
    Json forms = Json::object();
    forms["nom"] = word;
    const std::u32string w = toU32(word);
    const std::string g = sex == 'm' ? "m" : "f";
    for (const auto& r : grammar.value("name_rules", Json::array())) {
        const std::string rg = r.value("gender", std::string("any"));
        if (rg != "any" && rg != g) continue;
        if (!matchRule(w, r.value("match", std::string()))) continue;
        const std::u32string end = toU32(r.value("end", std::string()));
        const std::u32string stem = w.size() >= end.size() ? w.substr(0, w.size() - end.size()) : w;
        const char32_t before = stem.empty() ? 0 : lowerRu(stem.back());
        static const std::u32string hushVelar = U"жшчщкгх", // жшчщкгх
            hushTs = U"жшчщц";                              // жшчщц
        Json add = r.value("add", Json::object());
        auto over = [&](const char* key, const std::u32string& set) {
            if (!r.contains(key) || !before || set.find(before) == std::u32string::npos) return;
            for (auto it = r[key].begin(); it != r[key].end(); ++it) add[it.key()] = *it;
        };
        // after_hush_or_velar looks at the stem, after_hush_or_ts at the last letter
        over("after_hush_or_velar", hushVelar);
        if (r.contains("after_hush_or_ts") && !w.empty() && hushTs.find(lowerRu(w.back())) != std::u32string::npos)
            for (auto it = r["after_hush_or_ts"].begin(); it != r["after_hush_or_ts"].end(); ++it) add[it.key()] = *it;
        if (add.empty()) return forms; // indeclinable
        for (const char* cs : kCases) {
            if (!add.contains(cs)) continue;
            std::string ending = add[cs].get<std::string>();
            const size_t bar = ending.find('|');
            if (bar != std::string::npos) ending = ending.substr(0, bar);
            forms[cs] = toU8(stem) + ending;
        }
        return forms;
    }
    return forms;
}

} // namespace

const Json* Loc::grammar(const std::string& lang) const {
    auto it = grammar_.find(lang);
    return it == grammar_.end() ? nullptr : &it->second;
}

Json Loc::personNoun(const std::string& lang, const std::string& name, char sex) const {
    static const char* kAll[] = {"nom", "gen", "dat", "acc", "ins", "pre"};
    Json out = Json::object();
    for (const char* cs : kAll) out[cs] = "";
    out["g"] = sex == 'm' ? "m" : "f";
    const Json* gr = grammar(lang);
    size_t from = 0;
    bool first = true;
    while (from <= name.size()) {
        const size_t sp = name.find(' ', from);
        const std::string word = name.substr(from, sp == std::string::npos ? std::string::npos : sp - from);
        Json forms = gr ? declineWord(*gr, word, sex) : Json{{"nom", word}};
        for (const char* cs : kAll) {
            std::string f = forms.value(cs, word);
            if (const Json* ex = entry(lang, "name." + word + "." + cs); ex && ex->is_string()) f = ex->get<std::string>();
            std::string cur = out[cs].get<std::string>();
            out[cs] = first ? f : cur + " " + f;
        }
        first = false;
        if (sp == std::string::npos) break;
        from = sp + 1;
    }
    return out;
}

namespace {
Json characterNoun(const Loc& loc, const std::string& lang, const Character& c) {
    return loc.personNoun(lang, c.name, c.body.sex);
}
} // namespace

// --- tables -----------------------------------------------------------------

Loc Loc::fromJson(const std::string& base, const std::string& fallback,
                  const std::map<std::string, std::vector<Json>>& langs) {
    Loc l;
    l.base_ = base;
    l.fallback_ = fallback.empty() ? base : fallback;
    for (const auto& [lang, files] : langs) {
        auto& t = l.tables_[lang];
        for (size_t i = 0; i < files.size(); ++i) {
            Json f = files[i];
            if (f.is_object() && f.contains("_grammar")) { // tests: the language's _grammar.json inline
                l.grammar_[lang] = f["_grammar"];
                f.erase("_grammar");
            }
            flatten("", f, t, lang + "#" + std::to_string(i), l.loadWarnings);
        }
    }
    return l;
}

Loc Loc::load(const std::string& dir) {
    namespace fs = std::filesystem;
    const Json cfg = readJsonFile(fs::path(dir) / "loc.json");
    Loc l;
    l.base_ = cfg.value("base", std::string("ru"));
    l.fallback_ = cfg.value("fallback", l.base_);
    std::vector<std::string> langs = cfg.value("languages", std::vector<std::string>{l.base_});
    for (const auto& lang : langs) {
        auto& t = l.tables_[lang];
        const fs::path ld = fs::path(dir) / lang;
        if (!fs::is_directory(ld)) {
            l.loadWarnings.push_back("loc: no directory for language '" + lang + "'");
            continue;
        }
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(ld))
            if (e.is_regular_file() && e.path().extension() == ".json") files.push_back(e.path());
        std::sort(files.begin(), files.end()); // deterministic across file systems
        for (const auto& f : files) {
            const std::string fn = f.filename().string();
            if (!fn.empty() && fn[0] == '_') { // _grammar.json: rules, not keys
                if (fn == "_grammar.json") l.grammar_[lang] = readJsonFile(f);
                continue;
            }
            flatten("", readJsonFile(f), t, lang + "/" + fn, l.loadWarnings);
        }
    }
    return l;
}

std::vector<std::string> Loc::languages() const {
    std::vector<std::string> out;
    for (const auto& [k, v] : tables_) out.push_back(k);
    return out;
}

const Json* Loc::entry(const std::string& lang, const std::string& key) const {
    auto t = tables_.find(lang);
    if (t == tables_.end()) return nullptr;
    auto it = t->second.find(key);
    return it == t->second.end() ? nullptr : &it->second;
}

const Json* Loc::find(const std::string& lang, const std::string& key, std::string* from) const {
    for (const std::string* l : {&lang, &fallback_, &base_})
        if (const Json* e = entry(*l, key)) {
            if (from) *from = *l;
            return e;
        }
    return nullptr;
}

std::string Loc::text(const std::string& lang, const std::string& key, const Json& args) const {
    std::string from;
    const Json* e = find(lang, key, &from);
    if (!e) return "[" + key + "]";
    std::string pattern;
    if (e->is_array()) {
        if (e->empty() || !(*e)[0].is_string()) return "";
        pattern = (*e)[0].get<std::string>(); // variants: the first one (sayLoc picks by seed)
    } else if (e->is_object()) {
        if (e->contains("inf") || e->contains("base")) return e->value("inf", e->value("base", std::string()));
        return valueText(*e, "nom", from);
    } else {
        pattern = e->get<std::string>();
    }
    bool v2 = false;
    const std::string out = renderLocPattern(this, from, pattern, args, 0, nullptr, &v2);
    return v2 ? tidyLine(out, false) : out;
}

Json Loc::noun(const std::string& lang, const std::string& key, const std::string& fallbackText) const {
    std::string from;
    const Json* e = find(lang, key, &from);
    if (!e) return Json{{"nom", fallbackText}};
    if (e->is_object()) return *e;
    return Json{{"nom", text(lang, key)}};
}

std::set<std::string> Loc::keys(const std::string& lang) const {
    std::set<std::string> out;
    auto t = tables_.find(lang);
    if (t != tables_.end())
        for (const auto& [k, v] : t->second) out.insert(k);
    return out;
}

// --- formatter --------------------------------------------------------------

std::string pluralCategory(const std::string& lang, double n) {
    const double a = std::abs(n);
    if (lang == "ru") {
        if (std::abs(a - std::round(a)) > 1e-9) return "few"; // 1,5 ягоды
        const long long i = std::llround(a);
        if (i % 10 == 1 && i % 100 != 11) return "one";
        if (i % 10 >= 2 && i % 10 <= 4 && (i % 100 < 12 || i % 100 > 14)) return "few";
        return "many";
    }
    return std::abs(a - 1.0) < 1e-9 ? "one" : "other";
}

std::string formatLoc(const std::string& pattern, const Json& args, const std::string& lang,
                      std::vector<std::string>* errors) {
    return renderLocPattern(nullptr, lang, pattern, args, 0, errors);
}

bool checkLocPattern(const std::string& pattern, const std::string& lang, std::string& error,
                     std::set<std::string>* names) {
    return locPatternNames(pattern, lang, error, names);
}

// --- required keys ------------------------------------------------------------

const std::vector<std::string>& traceCodes() {
    static const std::vector<std::string> v = {
        "control.confirmed",   "control.free_will_resume", "control.hint",      "control.order",
        "control.order_refused", "control.paused",        "control.queued",    "control.resumed",
        "control.suspended_dropped", "control.to_ai",     "control.to_player", "fsm.abort",
        "fsm.arrived",         "fsm.done",                 "fsm.enter",         "fsm.wait",
        "goal.done",           "goal.dropped",             "goal.failed",       "goap.fail",
        "goap.plan",           "goap.replan",              "htn.method_failed", "htn.no_method",
        "htn.plan",            "htn.replan",               "utility.choose",    "utility.interrupt",
        "dialog.accepted",     "dialog.refused",
        // AL-31 consequences
        "deed.accuse_queued",  "deed.rumor_heard",  "deed.rumor_rejected", "say.line",         "deed.loss_noticed", "deed.suspects",
        // AL-37 cases
        "deed.case_answer",    "deed.case_pursue",         "deed.case_to_judge", "deed.case_verdict",
        "act.outcome",         "control.steer_clamped"};
    return v;
}

const std::vector<std::string>& reasonCodes() {
    static const std::vector<std::string> v = {
        "pre",          "no_target",          "wrong_target",        "reserved",          "out_of_range",
        "dead",         "unknown",            "target",              "switched_to",       "done",
        "cancelled_by_player", "control.paused", "fsm.lost",         "fsm.unknown_action", "goto.dangling",
        "goto.unknown_action", "goto.no_target", "goap.unknown_action", "goap.no_target", "goap.no_chain",
        "goap.cannot",  "goap.budget",        "htn.unknown_task",    "htn.no_method",     "htn.depth",
        "no_such_character", "controlled_by_other", "nothing_controlled", "no_paused_plan", "cannot",
        "plan_failed", "goap.no_person", "refused", "season", "person", "no_person"};
    return v;
}

const std::vector<std::string>& idleCodes() {
    static const std::vector<std::string> v = {"no_feasible_goal", "chose_idle", "no_goal", "player_paused_plan",
                                               "player_no_order"};
    return v;
}

std::vector<std::string> requiredLocKeys(const Content& c) {
    std::set<std::string> k;
    for (const auto& [id, d] : c.items) k.insert("item." + id + ".name");
    for (const auto& [id, d] : c.objects) k.insert("object." + id + ".name");
    for (const auto& n : c.needs) k.insert("need." + n.id + ".name");
    for (const auto& [id, t] : c.traits) {
        k.insert("trait." + id + ".name");
        k.insert("trait." + id + ".desc");
    }
    for (const auto& g : c.goals) k.insert("goal." + g.id + ".name");
    k.insert("goal.queued.name");        // code-level goals: a queued task,
    k.insert("goal.player_action.name"); // a direct player order
    for (const auto& [id, t] : c.tasks) k.insert("task." + id + ".name");
    for (const auto& [id, d] : c.interactions) {
        if (!d.baseId.empty()) continue; // <id>@foreign: named as the act it is made from
        k.insert("interaction." + id + ".name");
        for (const auto& r : d.risk) k.insert("risk." + r.id + ".name");
        for (const auto& s : d.speech) k.insert("speech." + s + ".name");
    }
    for (const auto& [id, m] : c.moodlets) k.insert("moodlet." + id + ".name");
    for (const auto& [id, s] : c.fsm) k.insert("fsm." + id + ".name");
    for (const auto& s : c.speechActs) k.insert("speech." + s + ".name");
    for (const auto& [id, t] : c.dialogue)
        for (const auto& v : t.variants) k.insert("dialogue." + id + "." + v.id);
    for (const auto& a : relAxes()) k.insert("rel." + a + ".name");
    for (const auto& a : c.config.reputationAxes) k.insert("reputation." + a + ".name");
    for (const auto& a : c.config.statusPairs) k.insert("status_pair." + a + ".name");
    for (const auto& a : c.config.householdRoles) k.insert("household_role." + a + ".name");
    for (const auto& a : c.config.deathCauses) k.insert("cause." + a + ".name");
    for (const auto& [id, cm] : c.config.commitments) k.insert("commitment." + id + ".name");
    for (const auto& t : traceCodes()) k.insert("trace." + t);
    for (const auto& r : reasonCodes()) k.insert("reason." + r);
    for (const auto& i : idleCodes()) k.insert("report.idle." + i);
    k.insert("chronicle.death");
    for (const char* ck : {"chronicle.deed_seen", "chronicle.loss_noticed", "chronicle.suspicion"}) k.insert(ck);
    for (const char* ck : {"chronicle.case_opened", "chronicle.case_returned", "chronicle.case_compensated",
                           "chronicle.case_denied", "chronicle.case_fled", "chronicle.case_let_go", "chronicle.case_quarrel",
                           "chronicle.case_fight", "chronicle.case_to_court", "chronicle.case_acquitted",
                           "chronicle.case_convicted", "chronicle.case_dropped", "chronicle.case_cleared",
                           "memory.case_pending.text", "memory.case_closed.text", "case.sentence.fine.name",
                           "case.sentence.shame.name", "case.sentence.custody.name", "case.sentence.exile.name"})
        k.insert(ck);
    for (const char* mk : {"memory.saw_crime.text", "memory.told_crime.text", "memory.rumor_crime.text",
                           "memory.suspects.text", "memory.noticed_missing.text"})
        k.insert(mk);
    for (const auto& vk : villageLifeLocKeys()) k.insert(vk); // AL-39
    if (c.craft.loaded)
        for (const auto& ck : craft::craftLocKeys(c.craft)) k.insert(ck);
    return {k.begin(), k.end()};
}

ValidationReport validateLoc(const Content& c, const Loc& loc) {
    ValidationReport r;
    for (const auto& w : loc.loadWarnings) r.warnings.push_back(w);
    const std::string& base = loc.base();
    if (!loc.hasLanguage(base)) {
        r.errors.push_back("loc: base language '" + base + "' has no table");
        return r;
    }
    if (!loc.hasLanguage(loc.fallback())) r.errors.push_back("loc: fallback language '" + loc.fallback() + "' has no table");
    const std::vector<std::string> langs = loc.languages();
    for (const auto& key : requiredLocKeys(c)) {
        if (!loc.entry(base, key)) r.errors.push_back("loc[" + base + "]: missing key '" + key + "'");
        for (const auto& l : langs)
            if (l != base && !loc.entry(l, key)) r.warnings.push_back("loc[" + l + "]: missing translation '" + key + "'");
    }
    for (const auto& l : langs)
        for (const auto& key : loc.keys(l)) {
            const Json& e = *loc.entry(l, key);
            const std::string where = "loc[" + l + "] " + key;
            if (l != base && !loc.entry(base, key)) r.warnings.push_back(where + ": not in the base language");
            if (key.rfind("lex.", 0) == 0 || key.rfind("frame.", 0) == 0) continue; // validateLocGen
            if (e.is_object()) {
                for (auto it = e.begin(); it != e.end(); ++it) {
                    if (it.key() == "anim") {
                        if (!it->is_boolean()) r.errors.push_back(where + ": 'anim' must be true or false");
                    } else if (it.key() == "pl") {
                        if (it->is_object()) {
                            for (auto f = it->begin(); f != it->end(); ++f)
                                if (!isCase(f.key()) || !f->is_string())
                                    r.errors.push_back(where + ": bad plural form '" + f.key() + "'");
                        } else if (!it->is_string()) {
                            r.errors.push_back(where + ": 'pl' must be a string or case forms");
                        }
                    } else if (it.key() == "g") {
                        const std::string g = it->is_string() ? it->get<std::string>() : "";
                        if (std::find(kGenders.begin(), kGenders.end(), g) == kGenders.end())
                            r.errors.push_back(where + ": bad gender '" + g + "' (m f n pl)");
                    } else if (!isCase(it.key())) {
                        r.errors.push_back(where + ": unknown noun form '" + it.key() + "'");
                    } else if (!it->is_string()) {
                        r.errors.push_back(where + ": noun form '" + it.key() + "' must be a string");
                    }
                }
                continue;
            }
            if (e.is_array()) continue; // variants: validateLocGen
            std::string err;
            std::set<std::string> names;
            if (!checkLocPattern(e.get<std::string>(), l, err, &names)) {
                r.errors.push_back(where + ": " + err);
                continue;
            }
            if (l == base) continue;
            const Json* b = loc.entry(base, key);
            if (!b || !b->is_string()) continue;
            std::set<std::string> baseNames;
            std::string berr;
            checkLocPattern(b->get<std::string>(), base, berr, &baseNames);
            for (const auto& n : names)
                if (!baseNames.count(n)) r.warnings.push_back(where + ": placeholder {" + n + "} is not in the base text");
        }
    const ValidationReport g = validateLocGen(loc);
    r.errors.insert(r.errors.end(), g.errors.begin(), g.errors.end());
    r.warnings.insert(r.warnings.end(), g.warnings.begin(), g.warnings.end());
    return r;
}

// --- output-time assembly ------------------------------------------------------

std::string reasonText(const Loc& loc, const std::string& lang, const std::string& reason) {
    const size_t colon = reason.find(':');
    const std::string head = reason.substr(0, colon);
    const std::string rest = colon == std::string::npos ? "" : reason.substr(colon + 1);
    if (!loc.has(lang, "reason." + head)) return reason;
    std::string detail = rest;
    if (rest.find(':') != std::string::npos) {
        detail = reasonText(loc, lang, rest);
    } else if (!rest.empty()) {
        for (const char* kind : {"object", "item", "goal", "interaction", "need", "task"})
            if (loc.has(lang, std::string(kind) + "." + rest + ".name")) {
                detail = loc.text(lang, std::string(kind) + "." + rest + ".name");
                break;
            }
    }
    return loc.text(lang, "reason." + head, {{"detail", detail}});
}

Json locArgs(const Loc& loc, const std::string& lang, const World& w, const Json& params, const Character* self) {
    Json a = Json::object();
    if (self) a["self"] = characterNoun(loc, lang, *self);
    if (!params.is_object()) return a;
    auto ref = [&](const char* kind, const std::string& raw) {
        const std::string id = raw.substr(0, raw.find('@')); // <id>@foreign reads as the act itself
        return loc.noun(lang, std::string(kind) + "." + id + ".name", id);
    };
    for (auto it = params.begin(); it != params.end(); ++it) {
        const std::string& k = it.key();
        const Json& v = *it;
        if (v.is_string()) {
            const std::string s = v.get<std::string>();
            if (k == "goal" || k == "was" || k == "chosen") {
                if (s.rfind("queued:", 0) == 0) {
                    const std::string t = s.substr(7);
                    a[k] = loc.has(lang, "task." + t + ".name") ? ref("task", t) : ref("interaction", t);
                } else {
                    a[k] = ref("goal", s);
                }
            } else if (k == "task") {
                a[k] = loc.has(lang, "task." + s + ".name") ? ref("task", s) : ref("interaction", s);
            } else if (k == "action" || k == "interaction") {
                a[k] = s.empty() ? Json("") : ref("interaction", s);
            } else if (k == "state" || k == "root") {
                a[k] = ref("fsm", s);
            } else if (k == "need" || k == "item" || k == "cause") {
                a[k] = ref(k.c_str(), s);
            } else if (k == "to" || k == "object" || k == "target_type") {
                a[k] = ref("object", s);
            } else if (k == "tag") {
                a[k] = ref("legal", s);
            } else if (k == "why" || k == "for") {
                a[k] = reasonText(loc, lang, s);
            } else {
                a[k] = v;
            }
        } else if (v.is_number_integer() &&
                   (k == "who" || k == "speaker" || k == "listener" || k == "actor" || k == "target" || k == "a" ||
                    k == "b" || k == "doer" || k == "suspect" || k == "owner" || k == "victim" || k == "from" ||
                    k == "object")) {
            const EntityId id = v.get<EntityId>();
            if (const Character* c = w.character(id)) a[k] = characterNoun(loc, lang, *c);
            else if (const WorldObject* o = w.object(id)) a[k] = ref("object", o->type);
            else a[k] = v;
        } else {
            a[k] = v;
        }
    }
    return a;
}

std::string traceText(const Loc& loc, const std::string& lang, const World& w, const Character& c,
                      const TraceEntry& e) {
    if (e.code == "say.line") return lineText(loc, lang, w, e.params); // AL-53: the line from its meaning
    return loc.text(lang, "trace." + e.code, locArgs(loc, lang, w, e.params, &c));
}

void localizeReport(Json& report, const Loc& loc, const std::string& lang, const World& w) {
    Json& s = report["summary"];
    for (auto& e : s["why_idle"]) e["text"] = loc.text(lang, "report.idle." + e["code"].get<std::string>());
    for (auto& e : s["plan_failures"]) e["text"] = reasonText(loc, lang, e["code"].get<std::string>());
    for (auto& h : s["why_hunger"]) {
        Json t = Json::array();
        for (const auto& why : h["why"]) t.push_back(reasonText(loc, lang, why.get<std::string>()));
        h["why_text"] = t;
    }
    for (auto& d : s["deaths"])
        d["text"] = loc.text(lang, d.value("code", std::string()), locArgs(loc, lang, w, d.value("params", Json::object())));
    report["lang"] = lang;
}

} // namespace altera::sim
