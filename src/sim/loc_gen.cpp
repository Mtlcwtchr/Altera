// LOC v0.2 text generator (AL-47): sub-rules, predicate frames, verb / adjective
// agreement, pronouns, prepositions, address by register. See loc_gen.hpp and
// doc/design/LOC_V02.md.

#include "sim/loc_gen.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>

#include "sim/dialogue_gen.hpp"

namespace altera::sim {

namespace {

const std::vector<std::string> kCases = {"nom", "gen", "dat", "acc", "ins", "pre"};
constexpr int kMaxDepth = 6;

bool isCase(const std::string& s) { return std::find(kCases.begin(), kCases.end(), s) != kCases.end(); }

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == sep) {
            out.push_back(s.substr(a, i - a));
            a = i + 1;
        }
    return out;
}

int pluralForms(const std::string& lang) { return lang == "ru" ? 3 : 2; }

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

// --- UTF-8 ----------------------------------------------------------------------
std::u32string u32(const std::string& s) {
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
std::string u8(const std::u32string& s) {
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
char32_t lowerCp(char32_t c) {
    if (c >= U'А' && c <= U'Я') return c + 32;
    if (c == U'Ё') return U'ё';
    if (c >= U'A' && c <= U'Z') return c + 32;
    return c;
}
char32_t upperCp(char32_t c) {
    if (c >= U'а' && c <= U'я') return c - 32;
    if (c == U'ё') return U'Ё';
    if (c >= U'a' && c <= U'z') return c - 32;
    return c;
}
bool isLetter(char32_t c) {
    return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || (c >= 0x400 && c <= 0x4FF);
}
std::string lowerStr(const std::string& s) {
    std::u32string w = u32(s);
    for (auto& c : w) c = lowerCp(c);
    return u8(w);
}

// --- seeded choice ---------------------------------------------------------------
std::uint64_t mix(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
std::uint64_t hashStr(const std::string& s) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char ch : s) h = (h ^ ch) * 1099511628211ull;
    return h;
}

// --- placeholders ----------------------------------------------------------------
struct Ph {
    bool expand = false, optional = false, v2 = false;
    std::string name, gcase, form, tense, agree, prep, choice, count;
    bool pro = false, pron = false, ty = false, addr = false;
    std::vector<std::string> forms;   // g | plural
    std::vector<std::string> tyForms; // ty|a|b
};

bool parsePh(const std::string& body0, const std::string& lang, Ph& ph, std::string& err) {
    std::string body = body0;
    if (!body.empty() && body[0] == '>') {
        ph.expand = ph.v2 = true;
        body.erase(0, 1);
    }
    const size_t colon = body.find(':');
    ph.name = body.substr(0, colon);
    if (!ph.name.empty() && ph.name.back() == '?') {
        ph.optional = ph.v2 = true;
        ph.name.pop_back();
    }
    if (ph.name.empty()) return err = "empty placeholder name", false;
    for (char ch : ph.name)
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '.'))
            return err = "bad placeholder name '" + ph.name + "'", false;
    if (ph.name.find('.') != std::string::npos) ph.v2 = true;
    if (colon == std::string::npos) return true;
    std::string rest = body.substr(colon + 1);
    if (!rest.empty() && rest.back() == '?') {
        ph.optional = ph.v2 = true;
        rest.pop_back();
    }
    size_t pos = 0;
    while (true) {
        if (rest.compare(pos, 2, "g|") == 0 || rest.compare(pos, 7, "plural|") == 0) {
            std::vector<std::string> parts = split(rest.substr(pos), '|');
            ph.choice = parts.front();
            ph.forms.assign(parts.begin() + 1, parts.end());
            if (ph.choice == "g" && (ph.forms.size() < 2 || ph.forms.size() > 4))
                return err = "{" + ph.name + ":g|...} needs 2..4 forms (m|f[|n|pl])", false;
            if (ph.choice == "plural" && ph.forms.size() != static_cast<size_t>(pluralForms(lang)))
                return err = "{" + ph.name + ":plural|...} needs " + std::to_string(pluralForms(lang)) + " forms for " +
                             lang,
                       false;
            return true;
        }
        size_t end = rest.find(':', pos);
        if (end == std::string::npos) end = rest.size();
        const std::string seg = rest.substr(pos, end - pos);
        auto bad = [&] { return err = "unknown modifier '" + seg + "' in {" + body0 + "}", false; };
        if (isCase(seg)) {
            if (!ph.gcase.empty()) return err = "two cases in {" + body0 + "}", false;
            ph.gcase = seg;
        } else if (seg == "g" || seg == "plural") {
            return err = "{" + ph.name + ":" + seg + "|...} needs forms", false;
        } else if (seg == "clause" || seg == "nominal" || seg == "about") {
            ph.form = seg, ph.v2 = true;
        } else if (seg == "past" || seg == "present" || seg == "future" || seg == "inf") {
            ph.tense = seg, ph.v2 = true;
        } else if (seg == "pro" || seg == "pron") {
            ph.pro = true, ph.pron = seg == "pron", ph.v2 = true;
        } else if (seg == "ty" || seg.rfind("ty|", 0) == 0) {
            ph.ty = ph.v2 = true;
            if (seg.size() > 2) {
                std::vector<std::string> parts = split(seg, '|');
                ph.tyForms.assign(parts.begin() + 1, parts.end());
                if (ph.tyForms.size() != 2) return err = "{" + ph.name + ":ty|...} needs 2 forms (ty|vy)", false;
            }
        } else if (seg == "addr") {
            ph.addr = ph.v2 = true;
        } else if (seg.rfind("prep|", 0) == 0 && seg.size() > 5) {
            ph.prep = seg.substr(5), ph.v2 = true;
        } else if (seg.rfind("agree=", 0) == 0 && seg.size() > 6) {
            ph.agree = seg.substr(6), ph.v2 = true;
        } else if (seg.rfind("count=", 0) == 0 && seg.size() > 6) {
            ph.count = seg.substr(6), ph.v2 = true;
        } else {
            return bad();
        }
        if (end == rest.size()) return true;
        pos = end + 1;
    }
}

struct Token {
    bool ph = false;
    std::string text;
    Ph p;
};

bool tokenize(const std::string& p, const std::string& lang, std::vector<Token>& out, std::string& err) {
    std::string buf;
    auto flush = [&] {
        if (!buf.empty()) out.push_back(Token{false, buf, {}});
        buf.clear();
    };
    for (size_t i = 0; i < p.size(); ++i) {
        const char ch = p[i];
        if (ch == '{' && i + 1 < p.size() && p[i + 1] == '{') {
            buf += '{';
            ++i;
        } else if (ch == '}' && i + 1 < p.size() && p[i + 1] == '}') {
            buf += '}';
            ++i;
        } else if (ch == '{') {
            const size_t end = p.find('}', i + 1);
            if (end == std::string::npos) return err = "unclosed '{'", false;
            const std::string body = p.substr(i + 1, end - i - 1);
            if (body.find('{') != std::string::npos) return err = "nested '{'", false;
            Token t;
            t.ph = true;
            if (!parsePh(body, lang, t.p, err)) return false;
            flush();
            out.push_back(std::move(t));
            i = end;
        } else if (ch == '}') {
            return err = "unmatched '}'", false;
        } else {
            buf += ch;
        }
    }
    flush();
    return true;
}

// --- values ------------------------------------------------------------------------
std::string str(const Json& j, const char* k, const std::string& def = "") {
    return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : def;
}
bool isLexVerb(const Json& v) { return v.is_object() && (v.contains("inf") || v.contains("base")) && !v.contains("nom"); }
bool isLexAdj(const Json& v) {
    return v.is_object() && !v.contains("nom") && (v.contains("m") || v.contains("pl") || str(v, "pos") == "adj");
}
bool isPlural(const Json& v) { return str(v, "num") == "pl" || str(v, "g") == "pl"; }
std::string genderKey(const Json& v) {
    if (isPlural(v)) return "pl";
    const std::string g = str(v, "g", "m");
    return g == "f" || g == "n" ? g : "m";
}
bool animate(const Json& v) {
    return v.is_object() && ((v.contains("anim") && v["anim"].is_boolean() && v["anim"].get<bool>()) || v.contains("id") ||
                             v.contains("pronoun"));
}
std::string regOf(const Json& v) { return str(v, "reg", "ty"); }
int personOf(const Json& v) { return v.is_object() && v.contains("p") && v["p"].is_number() ? v["p"].get<int>() : 3; }
std::string personNumber(const Json& v) {
    const int p = personOf(v);
    const bool pl = isPlural(v) || (p == 2 && regOf(v) == "vy");
    return std::to_string(p) + (pl ? "p" : "s");
}
std::string identity(const Json& v) {
    if (v.is_object() && v.contains("id") && v["id"].is_number()) return "#" + std::to_string(v["id"].get<long long>());
    if (v.is_object()) return str(v, "nom");
    if (v.is_string()) return v.get<std::string>();
    return "";
}

// Plain noun / value text in a case (nom when the form is absent; animate acc -> gen).
std::string nounForm(const Json& v, const std::string& gcase, bool plural, const std::string& lang) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number()) return numberText(v.get<double>(), lang);
    if (v.is_boolean()) return v.get<bool>() ? "1" : "0";
    if (v.is_array()) {
        std::string out;
        for (const auto& x : v) {
            if (!out.empty()) out += ", ";
            out += nounForm(x, gcase, plural, lang);
        }
        return out;
    }
    if (!v.is_object()) return "";
    const Json* src = &v;
    if (plural && v.contains("pl")) {
        if (v["pl"].is_string()) return v["pl"].get<std::string>();
        if (v["pl"].is_object()) src = &v["pl"];
    }
    if (!gcase.empty() && src->contains(gcase) && (*src)[gcase].is_string()) return (*src)[gcase].get<std::string>();
    if (gcase == "acc" && animate(v) && src->contains("gen") && (*src)["gen"].is_string())
        return (*src)["gen"].get<std::string>();
    return str(*src, "nom");
}

// A string or a lexeme form object: by person-number, then gender key, then 3s / m / first.
std::string formOf(const Json& f, const std::string& pn, const std::string& gk) {
    if (f.is_string()) return f.get<std::string>();
    if (!f.is_object()) return "";
    for (const std::string& k : {pn, gk, std::string("3s"), std::string("m")})
        if (f.contains(k) && f[k].is_string()) return f[k].get<std::string>();
    for (auto it = f.begin(); it != f.end(); ++it)
        if (it->is_string()) return it->get<std::string>();
    return "";
}

struct FrameCtx {
    const Json* frame = nullptr;
    const Json* prop = nullptr;
    std::string lang;
    std::string nominalCase;
    std::string verbId;
};

struct Ctx {
    const Loc* loc = nullptr;
    std::string reqLang;
    std::uint64_t seed = 0;
    std::vector<std::string>* errors = nullptr;
    bool missing = false;
    bool v2 = false;
    int depth = 0;
    std::vector<std::string> stack;
    std::map<std::string, int> mentions;
    unsigned picks = 0;
    std::vector<FrameCtx> frames;
    std::string out;
    void note(const std::string& s) {
        if (errors) errors->push_back(s);
    }
    const Json* grammar(const std::string& lang) const { return loc ? loc->grammar(lang) : nullptr; }
    size_t pick(const std::string& key, size_t n) {
        if (n <= 1) return 0;
        return static_cast<size_t>(mix(seed ^ hashStr(key) ^ mix(++picks)) % n);
    }
    const Json* lookup(const std::string& key, std::string* from) const {
        return loc ? loc->find(reqLang, key, from) : nullptr;
    }
};

void renderPattern(Ctx& c, const std::string& lang, const std::string& pattern, const Json& args);

const Json& gram(const Ctx& c, const std::string& lang, const char* key) {
    static const Json empty = Json::object();
    const Json* g = c.grammar(lang);
    if (!g || !g->contains(key)) return empty;
    return (*g)[key];
}
std::string verbModel(const Ctx& c, const std::string& lang) {
    const Json* g = c.grammar(lang);
    return g ? str(*g, "verb_model", lang == "ru" ? "ru" : "en") : (lang == "ru" ? "ru" : "en");
}

// A pronoun as a noun object (grammar "pronouns".<key>), or null when the language has none.
Json pronounNoun(const Ctx& c, const std::string& lang, const std::string& key) {
    const Json& p = gram(c, lang, "pronouns");
    if (!p.contains(key) || !p[key].is_object()) return nullptr;
    Json n = p[key];
    n["pronoun"] = key;
    return n;
}
std::string thirdKey(const Json& v) {
    const std::string g = genderKey(v);
    return g == "pl" ? "3p" : "3" + g;
}

bool lastWordIsPrep(const Ctx& c, const std::string& lang) {
    const Json& words = gram(c, lang, "prep_words");
    if (!words.is_array()) return false;
    size_t e = c.out.size();
    while (e > 0 && c.out[e - 1] == ' ') --e;
    size_t b = c.out.rfind(' ', e == 0 ? 0 : e - 1);
    b = b == std::string::npos ? 0 : b + 1;
    if (b >= e) return false;
    const std::string w = lowerStr(c.out.substr(b, e - b));
    for (const auto& x : words)
        if (x.is_string() && x.get<std::string>() == w) return true;
    return false;
}

std::string prepForm(const Ctx& c, const std::string& lang, const std::string& prep, const std::string& word) {
    const Json& rules = gram(c, lang, "prepositions");
    if (!rules.contains(prep) || !rules[prep].is_array()) return prep;
    std::string first = word.substr(0, word.find(' '));
    first = lowerStr(first);
    const std::u32string fw = u32(first);
    for (const auto& r : rules[prep]) {
        const std::string form = str(r, "form", prep);
        if (r.contains("words"))
            for (const auto& w : r["words"])
                if (w.is_string() && w.get<std::string>() == first) return form;
        if (r.contains("starts") && !fw.empty()) {
            const std::u32string s = u32(str(r, "starts"));
            if (s.find(fw.front()) != std::u32string::npos) return form;
        }
        if (r.contains("prefixes"))
            for (const auto& w : r["prefixes"])
                if (w.is_string() && first.rfind(w.get<std::string>(), 0) == 0) return form;
    }
    return prep;
}

// The speaker / listener inside a proposition becomes я / ты|вы.
Json personSubst(const Ctx& c, const std::string& lang, const Json& v, const Json& args) {
    if (!v.is_object() || !v.contains("id") || !args.is_object()) return v;
    auto same = [&](const char* who) {
        return args.contains(who) && args[who].is_object() && args[who].contains("id") && args[who]["id"] == v["id"];
    };
    std::string key;
    int p = 0;
    if (same("speaker")) key = "1s", p = 1;
    else if (same("listener")) {
        const std::string reg = args["listener"].is_object() ? str(args["listener"], "reg", regOf(v)) : regOf(v);
        key = reg == "vy" ? "2p" : "2s", p = 2;
    } else {
        return v;
    }
    Json n = pronounNoun(c, lang, key);
    if (n.is_null()) return v;
    n["g"] = str(v, "g", "m");
    n["p"] = p;
    n["id"] = v["id"];
    if (key == "2p") n["reg"] = "vy", n["num"] = "pl";
    return n;
}

struct Val {
    Json v;
    bool found = false, role = false, lexeme = false;
    std::string govCase, govPrep;
};

void parseGov(const std::string& spec, std::string& gcase, std::string& gprep) {
    const size_t plus = spec.find('+');
    if (plus == std::string::npos) gcase = spec;
    else gprep = spec.substr(0, plus), gcase = spec.substr(plus + 1);
}

Val resolve(Ctx& c, const std::string& lang, const std::string& name, const Json& args) {
    Val r;
    if (!c.frames.empty()) {
        const FrameCtx& f = c.frames.back();
        const Json& prop = *f.prop;
        if (name == "verb" && !f.verbId.empty()) {
            if (const Json* e = c.lookup("lex.verb." + f.verbId, nullptr)) r.v = *e, r.found = r.lexeme = true;
            else c.note("missing lexeme 'lex.verb." + f.verbId + "'");
            return r;
        }
        if (name == "noun") {
            const std::string key = str(*f.frame, "noun");
            if (const Json* e = key.empty() ? nullptr : c.lookup(key, nullptr)) {
                r.v = *e, r.found = r.role = true, r.govCase = f.nominalCase;
                return r;
            }
        }
        if (prop.contains("roles") && prop["roles"].contains(name)) {
            r.v = personSubst(c, lang, prop["roles"][name], args);
            r.found = r.role = true;
            const Json& gov = f.frame->value("roles", Json::object());
            if (gov.contains(name) && gov[name].is_string()) parseGov(gov[name].get<std::string>(), r.govCase, r.govPrep);
            return r;
        }
        if (name != "roles" && name != "pred" && prop.contains(name) && !prop[name].is_null()) {
            r.v = prop[name], r.found = true;
            return r;
        }
    }
    if (args.is_object() && args.contains(name)) {
        r.v = args[name], r.found = true;
        return r;
    }
    if (name.find('.') != std::string::npos)
        if (const Json* e = c.lookup("lex." + name, nullptr)) r.v = *e, r.found = r.lexeme = true;
    return r;
}

std::string verbForm(Ctx& c, const std::string& lang, const Json& lex, const std::string& tense0, const Json& agree,
                     bool neg) {
    const std::string pn = personNumber(agree), gk = genderKey(agree);
    const std::string tense = tense0.empty() ? "past" : tense0;
    if (verbModel(c, lang) == "ru") {
        const std::string inf = str(lex, "inf");
        std::string f;
        if (tense == "inf") f = inf;
        else if (tense == "past") f = formOf(lex.value("past", Json()), pn, gk);
        else if (tense == "present") f = formOf(lex.value("pres", Json()), pn, gk);
        else if (lex.contains("fut")) f = formOf(lex["fut"], pn, gk);
        else f = formOf(gram(c, lang, "future_aux"), pn, gk) + " " + inf;
        if (f.empty() || f == " ") {
            c.note("verb '" + inf + "' has no " + tense + " form");
            f = inf;
        }
        if (neg) { // no _grammar.json (a test table) must not crash
            const Json& ng = gram(c, lang, "neg");
            f = (ng.is_string() ? ng.get<std::string>() : std::string("не")) + " " + f;
        }
        return f;
    }
    // en-like: base, past, 3s; pres / past may be objects by person-number (be: am / is / are).
    const std::string base = str(lex, "base", str(lex, "inf"));
    const bool third = pn == "3s";
    if (tense == "inf") return base;
    if (tense == "past") {
        if (neg) return "did not " + base;
        return lex.contains("past") ? formOf(lex["past"], pn, gk) : base;
    }
    if (tense == "present") {
        if (neg) return std::string(third ? "does not " : "do not ") + base;
        if (lex.contains("pres")) return formOf(lex["pres"], pn, gk);
        return third ? str(lex, "3s", base + "s") : base;
    }
    return std::string(neg ? "will not " : "will ") + base;
}

std::string adjForm(Ctx& c, const std::string& lang, const Json& lex, const Json& agree, const std::string& gcase0) {
    (void)c;
    (void)lang;
    if (lex.is_string()) return lex.get<std::string>();
    if (lex.contains("base")) return str(lex, "base");
    const std::string gk = genderKey(agree);
    const std::string gcase = gcase0.empty() ? "nom" : gcase0;
    const Json& f = lex.contains(gk) ? lex[gk] : lex.value("m", Json());
    if (f.is_string()) return f.get<std::string>();
    if (!f.is_object()) return "";
    if (gcase == "acc" && animate(agree) && (gk == "m" || gk == "pl")) return str(f, "acc_an", str(f, "gen", str(f, "acc")));
    return str(f, gcase.c_str(), str(f, "nom"));
}

std::string countForm(const Json& v, double n, const std::string& gcase, const std::string& lang) {
    if (lang == "ru") {
        const std::string cat = pluralCategory(lang, n);
        if (gcase.empty() || gcase == "nom" || gcase == "acc") {
            if (cat == "one") return nounForm(v, gcase.empty() ? "nom" : gcase, false, lang);
            if (cat == "few") return nounForm(v, "gen", false, lang);
            return nounForm(v, "gen", true, lang);
        }
        return nounForm(v, gcase, cat != "one", lang);
    }
    return nounForm(v, gcase, std::abs(n - 1.0) > 1e-9, lang);
}

void realize(Ctx& c, const Json& prop, const std::string& form, const std::string& gcase, const Json& args);

void renderValue(Ctx& c, const std::string& lang, const Ph& ph, const Val& val, const Json& args, bool afterPrep) {
    const Json& v = val.v;
    const std::string gcase = !ph.gcase.empty() ? ph.gcase : val.govCase;
    if (ph.choice == "g") {
        const std::string g = genderKey(v);
        size_t i = g == "f" ? 1 : g == "n" ? 2 : g == "pl" ? 3 : 0;
        if (!v.is_object() || (!v.contains("g") && !v.contains("num"))) i = 0;
        c.out += ph.forms[i < ph.forms.size() ? i : 0];
        return;
    }
    if (ph.choice == "plural") {
        double n = 0.0;
        if (v.is_number()) n = v.get<double>();
        else if (v.is_object() && v.contains("n") && v["n"].is_number()) n = v["n"].get<double>();
        else c.note("'" + ph.name + "' is not a number for plural");
        const std::string cat = pluralCategory(lang, n);
        size_t i = lang == "ru" ? (cat == "one" ? 0 : cat == "few" ? 1 : 2) : (cat == "one" ? 0 : 1);
        c.out += ph.forms[std::min(i, ph.forms.size() - 1)];
        return;
    }
    if (ph.ty && !ph.tyForms.empty()) {
        c.out += ph.tyForms[regOf(v) == "vy" ? 1 : 0];
        return;
    }
    if (isPropArg(v)) { // a prop role may be governed by a form: "content": "clause"
        const bool govForm = val.govCase == "clause" || val.govCase == "nominal" || val.govCase == "about";
        const std::string form = !ph.form.empty() ? ph.form : govForm ? val.govCase : "clause";
        return realize(c, v, form, isCase(gcase) ? gcase : "", args);
    }
    if (val.lexeme && isLexVerb(v)) {
        Json agree = Json::object();
        if (!ph.agree.empty()) {
            Val a = resolve(c, lang, ph.agree, args);
            if (!a.found) c.note("agree: no argument '" + ph.agree + "'");
            else agree = personSubst(c, lang, a.v, args); // the listener agrees as 2nd person
        }
        std::string tense = ph.tense;
        bool neg = false;
        if (!c.frames.empty()) {
            const Json& p = *c.frames.back().prop;
            if (tense.empty()) tense = str(p, "tense", "past");
            if (ph.name == "verb") neg = p.value("neg", false);
        }
        c.out += verbForm(c, lang, v, tense, agree, neg);
        return;
    }
    if (val.lexeme && isLexAdj(v)) {
        Json agree = Json::object();
        if (!ph.agree.empty()) {
            Val a = resolve(c, lang, ph.agree, args);
            if (!a.found) c.note("agree: no argument '" + ph.agree + "'");
            else agree = a.v;
        }
        c.out += adjForm(c, lang, v, agree, gcase);
        return;
    }
    if (ph.ty) {
        Json p = pronounNoun(c, lang, regOf(v) == "vy" ? "2p" : "2s");
        c.out += p.is_null() ? nounForm(v, gcase, false, lang) : nounForm(p, gcase, false, lang);
        return;
    }
    if (ph.addr) {
        if (v.is_object() && v.contains("addr")) c.out += nounForm(v["addr"], gcase.empty() ? "nom" : gcase, false, lang);
        return;
    }
    if (!ph.count.empty()) {
        Val nv = resolve(c, lang, ph.count, args);
        double n = 1.0;
        if (nv.found && nv.v.is_number()) n = nv.v.get<double>();
        else c.note("count: '" + ph.count + "' is not a number");
        c.out += countForm(v, n, gcase, lang);
        return;
    }
    const std::string id = identity(v);
    const bool already = !id.empty() && c.mentions.count(id) && c.mentions[id] > 0;
    if (!id.empty()) ++c.mentions[id];
    const bool isPronoun = v.is_object() && v.contains("pronoun");
    if (ph.pro && !isPronoun && (ph.pron || already)) {
        const std::string key = thirdKey(v);
        Json p = pronounNoun(c, lang, key);
        if (!p.is_null()) {
            const Json& ap = gram(c, lang, "pronouns_after_prep");
            const std::string cs = gcase.empty() ? "nom" : gcase;
            if (afterPrep && ap.contains(key) && ap[key].contains(cs) && ap[key][cs].is_string()) {
                c.out += ap[key][cs].get<std::string>();
                return;
            }
            c.out += nounForm(p, cs, false, lang);
            return;
        }
    }
    c.out += nounForm(v, gcase, isPlural(v) && !isPronoun && v.is_object() && v.contains("pl"), lang);
}

// Runs `body` with optional semantics: on a missing argument the output and
// mentions are rolled back and errors dropped.
template <class F>
void guarded(Ctx& c, bool optional, F body) {
    const size_t mark = c.out.size();
    const bool savedMissing = c.missing;
    auto* savedErrors = c.errors;
    std::vector<std::string> local;
    const auto savedMentions = c.mentions;
    c.missing = false;
    if (optional) c.errors = &local;
    body();
    const bool miss = c.missing;
    c.errors = savedErrors;
    if (optional) {
        if (miss) {
            c.out.resize(mark);
            c.mentions = savedMentions;
        } else if (c.errors) {
            for (auto& e : local) c.errors->push_back(e);
        }
        c.missing = savedMissing;
    } else {
        c.missing = savedMissing || miss;
    }
}

void expandKey(Ctx& c, const std::string& key, const Json& args) {
    std::string from;
    const Json* e = c.lookup(key, &from);
    if (!e) {
        c.note("missing rule '" + key + "'");
        c.out += "[" + key + "]";
        c.missing = true;
        return;
    }
    std::string pattern;
    if (e->is_string()) pattern = e->get<std::string>();
    else if (e->is_array() && !e->empty()) {
        const Json& x = (*e)[c.pick(key, e->size())];
        pattern = x.is_string() ? x.get<std::string>() : "";
    } else {
        c.note("rule '" + key + "' is not a pattern");
        return;
    }
    if (c.depth >= kMaxDepth || std::find(c.stack.begin(), c.stack.end(), key) != c.stack.end()) {
        c.note("rule '" + key + "': expansion too deep or cyclic");
        return;
    }
    ++c.depth;
    c.stack.push_back(key);
    renderPattern(c, from, pattern, args);
    c.stack.pop_back();
    --c.depth;
}

void handle(Ctx& c, const std::string& lang, const Ph& ph, const Json& args) {
    if (ph.v2) c.v2 = true;
    guarded(c, ph.optional, [&] {
        Val val = resolve(c, lang, ph.name, args);
        if (ph.expand && !(val.found && !val.lexeme)) {
            if (!c.loc) {
                c.note("sub-rule {>" + ph.name + "} needs a Loc");
                c.missing = true;
                return;
            }
            return expandKey(c, ph.name, args);
        }
        if (ph.expand && val.v.is_string()) {
            c.out += val.v.get<std::string>();
            return;
        }
        if (!val.found) {
            c.missing = true;
            if (!ph.optional) {
                c.note("missing argument '" + ph.name + "'");
                c.out += "{" + ph.name + "}";
            }
            return;
        }
        if (val.v.is_null()) {
            c.missing = true;
            return;
        }
        const std::string prep = !ph.prep.empty() ? ph.prep : (ph.gcase.empty() ? val.govPrep : "");
        const size_t mark = c.out.size();
        renderValue(c, lang, ph, val, args, !prep.empty() || lastWordIsPrep(c, lang));
        if (!prep.empty() && c.out.size() > mark) {
            const std::string word = c.out.substr(mark);
            c.out.insert(mark, prepForm(c, lang, prep, word) + " ");
        }
    });
}

void renderPattern(Ctx& c, const std::string& lang, const std::string& pattern, const Json& args) {
    std::vector<Token> toks;
    std::string err;
    if (!tokenize(pattern, lang, toks, err)) {
        c.note("pattern: " + err);
        c.out += pattern;
        return;
    }
    for (const auto& t : toks) {
        if (!t.ph) c.out += t.text;
        else handle(c, lang, t.p, args);
    }
}

void realize(Ctx& c, const Json& prop, const std::string& form, const std::string& gcase, const Json& args) {
    c.v2 = true;
    const std::string pred = str(prop, "pred");
    std::string from;
    const Json* frame = c.lookup("frame." + pred, &from);
    if (!frame || !frame->is_object()) {
        c.note("no frame for '" + pred + "'");
        c.out += "[frame." + pred + "]";
        c.missing = true;
        return;
    }
    if (c.depth >= kMaxDepth) {
        c.note("frame '" + pred + "': expansion too deep");
        return;
    }
    FrameCtx f;
    f.frame = frame;
    f.prop = &prop;
    f.lang = from;
    f.nominalCase = gcase;
    // the verb: one of frame.verb (string or array), then one of it and its synonyms
    if (frame->contains("verb")) {
        const Json& vb = (*frame)["verb"];
        std::vector<std::string> ids;
        if (vb.is_string()) ids.push_back(vb.get<std::string>());
        else if (vb.is_array())
            for (const auto& x : vb)
                if (x.is_string()) ids.push_back(x.get<std::string>());
        if (!ids.empty()) {
            std::string id = ids[c.pick("frame." + pred + ".verb", ids.size())];
            if (frame->value("syn", true))
                if (const Json* lx = c.lookup("lex.verb." + id, nullptr); lx && lx->is_object() && lx->contains("syn")) {
                    std::vector<std::string> all{id};
                    for (const auto& s : (*lx)["syn"])
                        if (s.is_string()) all.push_back(s.get<std::string>());
                    id = all[c.pick("lex.verb." + id + ".syn", all.size())];
                }
            f.verbId = id;
        }
    }
    std::string pattern;
    const Json* pf = frame->contains(form) ? &(*frame)[form] : nullptr;
    if (pf && pf->is_array() && !pf->empty()) {
        const Json& x = (*pf)[c.pick("frame." + pred + "." + form, pf->size())];
        if (x.is_string()) pattern = x.get<std::string>();
    } else if (pf && pf->is_string()) {
        pattern = pf->get<std::string>();
    }
    ++c.depth;
    if (pattern.empty() && form == "about" && (frame->contains("nominal"))) {
        // default: the grammar's "about" preposition + case + the nominal form
        const Json& ab = gram(c, from, "about");
        const std::string prep = str(ab, "prep"), cs = str(ab, "case", "pre");
        const size_t mark = c.out.size();
        --c.depth;
        realize(c, prop, "nominal", cs, args);
        ++c.depth;
        if (!prep.empty() && c.out.size() > mark) c.out.insert(mark, prepForm(c, from, prep, c.out.substr(mark)) + " ");
    } else if (pattern.empty()) {
        c.note("frame '" + pred + "' has no '" + form + "'");
        c.missing = true;
    } else {
        c.frames.push_back(f);
        renderPattern(c, from, pattern, args);
        c.frames.pop_back();
    }
    --c.depth;
}

} // namespace

bool isPropArg(const Json& v) { return v.is_object() && v.contains("pred") && v["pred"].is_string(); }

std::string renderLocPattern(const Loc* loc, const std::string& lang, const std::string& pattern, const Json& args,
                             std::uint64_t seed, std::vector<std::string>* errors, bool* usedV2) {
    Ctx c;
    c.loc = loc;
    c.reqLang = lang;
    c.seed = seed;
    c.errors = errors;
    renderPattern(c, lang, pattern, args);
    if (usedV2) *usedV2 = c.v2;
    return c.out;
}

bool locPatternNames(const std::string& pattern, const std::string& lang, std::string& error,
                     std::set<std::string>* names) {
    std::vector<Token> toks;
    if (!tokenize(pattern, lang, toks, error)) return false;
    if (names)
        for (const auto& t : toks)
            if (t.ph && t.p.name.find('.') == std::string::npos) names->insert(t.p.name);
    return true;
}

std::string tidyLine(const std::string& s, bool capitalize) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const char ch = s[i];
        if (ch == ' ') {
            if (r.empty() || r.back() == ' ') continue;
            // skip a space before punctuation
            size_t j = i;
            while (j < s.size() && s[j] == ' ') ++j;
            if (j < s.size() && std::strchr(",.!?:;)", s[j])) continue;
            if (j + 1 < s.size() && static_cast<unsigned char>(s[j]) == 0xC2 &&
                static_cast<unsigned char>(s[j + 1]) == 0xBB) // »
                continue;
            if (r.size() >= 2 && static_cast<unsigned char>(r[r.size() - 2]) == 0xC2 &&
                static_cast<unsigned char>(r.back()) == 0xAB) // «
                continue;
            if (r.back() == '(') continue;
        }
        r += ch;
    }
    while (!r.empty() && r.back() == ' ') r.pop_back();
    if (capitalize) { // the first letter of the line and of every sentence after . ! ?
        std::u32string w = u32(r);
        bool start = true;
        for (size_t i = 0; i < w.size(); ++i) {
            char32_t& cp = w[i];
            if (start && isLetter(cp)) {
                cp = upperCp(cp);
                start = false;
            } else if ((cp == U'.' || cp == U'!' || cp == U'?') && i + 1 < w.size() && w[i + 1] == U' ') {
                start = true;
            } else if (isLetter(cp) || (cp >= U'0' && cp <= U'9')) {
                start = false;
            }
        }
        r = u8(w);
    }
    return r;
}

std::string sayLoc(const Loc& loc, const std::string& lang, const std::string& key, const Json& args,
                   std::uint64_t seed, std::vector<std::string>* errors) {
    Ctx c;
    c.loc = &loc;
    c.reqLang = lang;
    c.seed = seed;
    c.errors = errors;
    c.stack.push_back(key);
    std::string from;
    const Json* e = loc.find(lang, key, &from);
    if (!e) {
        if (errors) errors->push_back("missing key '" + key + "'");
        return "[" + key + "]";
    }
    if (e->is_string()) renderPattern(c, from, e->get<std::string>(), args);
    else if (e->is_array() && !e->empty() && (*e)[c.pick(key, e->size())].is_string())
        renderPattern(c, from, (*e)[c.pick(key, e->size())].get<std::string>(), args);
    else c.out = nounForm(*e, "nom", false, from);
    return tidyLine(c.out, true);
}

std::string realizeProp(const Loc& loc, const std::string& lang, const Json& prop, const std::string& form,
                        const std::string& gcase, const Json& args, std::uint64_t seed,
                        std::vector<std::string>* errors) {
    Ctx c;
    c.loc = &loc;
    c.reqLang = lang;
    c.seed = seed;
    c.errors = errors;
    realize(c, prop, form, gcase, args);
    return tidyLine(c.out, false);
}

Json propLocArg(const Prop& p, const std::function<Json(const Ref&)>& refNoun,
                const std::function<Json(const std::string&)>& unitNoun) {
    Json j = Json::object();
    j["pred"] = p.pred;
    j["tense"] = p.tense;
    j["neg"] = p.neg;
    j["modality"] = p.modality;
    if (p.qty > 0.0) j["qty"] = p.qty;
    if (!p.unit.empty() && unitNoun) j["unit"] = unitNoun(p.unit);
    Json roles = Json::object();
    for (const auto& [name, ref] : p.roles) {
        if (ref.kind == "prop" && !ref.sub.empty()) roles[name] = propLocArg(ref.sub.front(), refNoun, unitNoun);
        else roles[name] = refNoun ? refNoun(ref) : Json(ref.key);
    }
    j["roles"] = roles;
    return j;
}

// --- validator ---------------------------------------------------------------------

namespace {

void collectRefs(const std::string& pattern, const std::string& lang, std::vector<Ph>& phs, std::string& err, bool& ok) {
    std::vector<Token> toks;
    ok = tokenize(pattern, lang, toks, err);
    if (!ok) return;
    for (auto& t : toks)
        if (t.ph) phs.push_back(t.p);
}

std::vector<std::string> patternsOf(const Json& e) {
    std::vector<std::string> out;
    if (e.is_string()) out.push_back(e.get<std::string>());
    else if (e.is_array())
        for (const auto& x : e)
            if (x.is_string()) out.push_back(x.get<std::string>());
    return out;
}

} // namespace

ValidationReport validateLocGen(const Loc& loc, const std::vector<std::string>& predicates,
                                const std::map<std::string, std::vector<std::string>>& roles) {
    ValidationReport r;
    const std::string base = loc.base();
    for (const auto& lang : loc.languages()) {
        const bool isBase = lang == base;
        auto problem = [&](const std::string& s) { (isBase ? r.errors : r.warnings).push_back(s); };
        const Json* g = loc.grammar(lang);
        const std::string model = g ? str(*g, "verb_model", lang == "ru" ? "ru" : "en") : (lang == "ru" ? "ru" : "en");
        std::map<std::string, std::vector<std::string>> graph; // sub-rule references
        for (const auto& key : loc.keys(lang)) {
            const Json& e = *loc.entry(lang, key);
            const std::string where = "loc[" + lang + "] " + key;
            std::vector<std::string> pats;
            std::set<std::string> allowedNames;
            const bool isFrame = key.rfind("frame.", 0) == 0;
            if (key.rfind("lex.verb.", 0) == 0) {
                if (!e.is_object()) {
                    r.errors.push_back(where + ": a verb must be an object");
                    continue;
                }
                if (model == "ru") {
                    if (!e.contains("inf")) r.errors.push_back(where + ": verb without 'inf'");
                    if (!e.contains("past")) r.errors.push_back(where + ": verb without 'past'");
                    else if (e["past"].is_object())
                        for (const char* k : {"m", "f", "n", "pl"})
                            if (!e["past"].contains(k)) r.errors.push_back(where + ": past has no '" + k + "'");
                    for (const char* t : {"pres", "fut"})
                        if (e.contains(t) && e[t].is_object())
                            for (const char* k : {"1s", "2s", "3s", "1p", "2p", "3p"})
                                if (!e[t].contains(k)) r.errors.push_back(where + ": " + t + " has no '" + k + "'");
                } else if (!e.contains("base")) {
                    r.errors.push_back(where + ": verb without 'base'");
                }
                if (e.contains("syn")) {
                    if (!e["syn"].is_array()) r.errors.push_back(where + ": syn must be a list of lex.verb ids");
                    else
                        for (const auto& s : e["syn"])
                            if (!s.is_string() || !loc.find(lang, "lex.verb." + s.get<std::string>()))
                                problem(where + ": synonym '" + (s.is_string() ? s.get<std::string>() : "?") +
                                        "' is not a lex.verb");
                }
                continue;
            }
            if (key.rfind("lex.adj.", 0) == 0) {
                if (e.is_string()) continue;
                if (!e.is_object()) {
                    r.errors.push_back(where + ": an adjective must be a string or an object");
                    continue;
                }
                if (e.contains("base")) continue;
                for (const char* gk : {"m", "f", "n", "pl"})
                    if (!e.contains(gk)) r.errors.push_back(where + ": adjective has no '" + gk + "' forms");
                    else if (e[gk].is_object() && !e[gk].contains("nom"))
                        r.errors.push_back(where + ": adjective '" + gk + "' has no 'nom'");
                continue;
            }
            if (isFrame) {
                if (!e.is_object()) {
                    r.errors.push_back(where + ": a frame must be an object");
                    continue;
                }
                if (!e.contains("clause")) problem(where + ": frame without 'clause'");
                const Json& vb = e.value("verb", Json());
                for (const auto& v : patternsOf(vb))
                    if (!loc.find(lang, "lex.verb." + v)) problem(where + ": verb '" + v + "' is not a lex.verb");
                if (e.contains("noun") && (!e["noun"].is_string() || !loc.find(lang, e["noun"].get<std::string>())))
                    problem(where + ": noun '" + str(e, "noun") + "' not found");
                const Json& rl = e.value("roles", Json::object());
                if (!rl.is_object()) r.errors.push_back(where + ": roles must be an object");
                else
                    for (auto it = rl.begin(); it != rl.end(); ++it) {
                        std::string cs, pr;
                        if (it->is_string()) parseGov(it->get<std::string>(), cs, pr);
                        if (!isCase(cs) && cs != "clause" && cs != "nominal" && cs != "about")
                            r.errors.push_back(where + ": role '" + it.key() + "' has no case");
                        allowedNames.insert(it.key());
                    }
                const std::string pred = key.substr(6);
                const auto pr = roles.find(pred);
                if (pr != roles.end())
                    for (const auto& n : allowedNames)
                        if (std::find(pr->second.begin(), pr->second.end(), n) == pr->second.end())
                            problem(where + ": role '" + n + "' is not a role of predicate '" + pred + "'");
                if (pr != roles.end())
                    for (const auto& n : pr->second) allowedNames.insert(n);
                for (const char* x : {"clause", "nominal", "about"})
                    if (e.contains(x)) {
                        if (!e[x].is_string() && !e[x].is_array())
                            r.errors.push_back(where + ": '" + x + "' must be a pattern or a list of patterns");
                        for (auto& p : patternsOf(e[x])) pats.push_back(p);
                    }
            } else if (e.is_string() || e.is_array()) {
                if (e.is_array())
                    for (const auto& x : e)
                        if (!x.is_string()) r.errors.push_back(where + ": a variant must be a string");
                pats = patternsOf(e);
            } else {
                continue; // nouns: validateLoc
            }
            for (const auto& p : pats) {
                std::vector<Ph> phs;
                std::string err;
                bool ok = true;
                collectRefs(p, lang, phs, err, ok);
                if (!ok) {
                    r.errors.push_back(where + ": " + err);
                    continue;
                }
                for (const auto& ph : phs) {
                    if (ph.expand && ph.name.find('.') != std::string::npos) {
                        if (!loc.find(lang, ph.name)) problem(where + ": sub-rule '" + ph.name + "' not found");
                        else graph[key].push_back(ph.name);
                    } else if (!ph.expand && ph.name.find('.') != std::string::npos &&
                               !loc.find(lang, "lex." + ph.name)) {
                        problem(where + ": lexeme 'lex." + ph.name + "' not found");
                    }
                    if (isFrame && !ph.agree.empty() && !allowedNames.count(ph.agree) && ph.agree != "speaker" &&
                        ph.agree != "listener")
                        problem(where + ": agree with unknown role '" + ph.agree + "'");
                }
            }
        }
        // cycles and depth of sub-rule references
        std::map<std::string, int> state; // 1 = on stack, 2 = done
        std::map<std::string, int> height;
        std::function<int(const std::string&)> dfs = [&](const std::string& k) -> int {
            if (state[k] == 1) {
                r.errors.push_back("loc[" + lang + "] " + k + ": sub-rule cycle");
                return 0;
            }
            if (state[k] == 2) return height[k];
            state[k] = 1;
            int h = 0;
            for (const auto& n : graph[k]) h = std::max(h, 1 + dfs(n));
            state[k] = 2;
            return height[k] = h;
        };
        for (const auto& [k, v] : graph)
            if (dfs(k) > kMaxDepth) r.errors.push_back("loc[" + lang + "] " + k + ": sub-rules nest deeper than 6");
    }
    // Until the base language has any frame (content not written yet) a missing frame is a warning.
    bool anyFrame = false;
    for (const auto& k : loc.keys(base)) anyFrame = anyFrame || k.rfind("frame.", 0) == 0;
    for (const auto& p : predicates)
        if (!loc.entry(base, "frame." + p))
            (anyFrame ? r.errors : r.warnings).push_back("loc[" + base + "]: no frame for predicate '" + p + "'");
    return r;
}

} // namespace altera::sim
