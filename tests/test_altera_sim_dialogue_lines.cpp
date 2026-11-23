// Shipped dialogue lines (AL-51 ru, AL-52 en): every say.* / slot.* / part.* key
// and every predicate frame renders in every language with typical arguments,
// several seeds, without errors or leftover {placeholders} / [missing keys], and
// ru and en carry the same set of generator keys. ALTERA_DUMP_LINES=<file>
// writes every rendered variant there for reading by eye.

#include <cstdlib>
#include <fstream>

#include "framework.hpp"
#include "sim/loc.hpp"
#include "sim/loc_gen.hpp"

using namespace altera::sim;

#define CHECK_MSG(cond, msg)                                         \
    do {                                                             \
        if (!(cond)) ::testing::fail(__FILE__, __LINE__, (msg));     \
    } while (0)

namespace {

const Loc& shippedLoc() {
    static const Loc l = Loc::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/loc");
    return l;
}

bool generatorKey(const std::string& k) {
    for (const char* p : {"say.", "slot.", "part.epi.", "frame.", "lex.", "category.", "thing."})
        if (k.rfind(p, 0) == 0) return true;
    return false;
}

Json person(const Loc& loc, const std::string& lang, const std::string& name, char sex, int id) {
    Json n = loc.personNoun(lang, name, sex);
    n["id"] = id;
    return n;
}

struct Cast {
    Json oleg, brana, miller, yarik, item, theft, murder, help, lose;
};

Cast cast(const Loc& loc, const std::string& lang) {
    Cast c;
    c.oleg = person(loc, lang, "Oleg", 'm', 1);
    c.brana = person(loc, lang, "Brana", 'f', 2);
    c.miller = person(loc, lang, "Vlas", 'm', 3);
    c.yarik = person(loc, lang, "Yarik", 'm', 4);
    c.item = lang == "ru" ? Json{{"nom", "мука"}, {"gen", "муки"}, {"dat", "муке"}, {"acc", "муку"}, {"ins", "мукой"},
                                 {"pre", "муке"}, {"g", "f"}}
                          : Json("flour");
    c.theft = {{"pred", "theft"}, {"tense", "past"}, {"roles", {{"doer", c.yarik}, {"object", c.item}, {"victim", c.miller}}}};
    c.murder = {{"pred", "murder"}, {"tense", "past"}, {"roles", {{"doer", c.yarik}, {"victim", c.miller}}}};
    c.help = {{"pred", "help"}, {"tense", "past"}, {"roles", {{"doer", c.brana}, {"beneficiary", c.miller}}}};
    c.lose = {{"pred", "lose"}, {"tense", "past"}, {"roles", {{"object", c.item}, {"victim", c.miller}}}};
    return c;
}

bool clean(const std::string& s) {
    return s.find('{') == std::string::npos && s.find('}') == std::string::npos && s.find('[') == std::string::npos;
}

} // namespace

TEST(altera_dialogue_lines_ru_en_same_keys) {
    const Loc& loc = shippedLoc();
    std::set<std::string> ru, en;
    for (const auto& k : loc.keys("ru"))
        if (generatorKey(k)) ru.insert(k);
    for (const auto& k : loc.keys("en"))
        if (generatorKey(k)) en.insert(k);
    for (const auto& k : ru)
        if (!en.count(k)) CHECK_MSG(false, "no en for " + k);
    for (const auto& k : en)
        if (!ru.count(k)) CHECK_MSG(false, "en key not in ru: " + k);
}

TEST(altera_dialogue_lines_render_all) {
    const Loc& loc = shippedLoc();
    const char* dumpPath = std::getenv("ALTERA_DUMP_LINES");
    std::ofstream dump;
    if (dumpPath) dump.open(dumpPath);
    for (const std::string lang : {"ru", "en"}) {
        const Cast c = cast(loc, lang);
        for (const auto& key : loc.keys(lang)) {
            if (key.rfind("say.", 0) != 0 && key.rfind("slot.", 0) != 0 && key.rfind("part.epi.", 0) != 0) continue;
            const bool loss = key.find("loss") != std::string::npos;
            const Json prop = loss ? c.lose : key.find("grave") != std::string::npos ? c.murder
                              : key.find("thank") != std::string::npos || key.find("praise") != std::string::npos ? c.help
                                                                                                                 : c.theft;
            Json listener = c.brana;
            if (key.find("noble") != std::string::npos) listener["reg"] = "vy";
            const Json args = {{"speaker", c.oleg}, {"listener", listener}, {"src", c.miller}, {"prop", prop},
                               {"reason", c.theft}, {"item", c.item}, {"price", 12},
                               {"give", c.item}, {"get", c.item}}; // AL-75: the two sides of a deal
            for (std::uint64_t seed = 0; seed < 4; ++seed) {
                std::vector<std::string> errs;
                const std::string t = sayLoc(loc, lang, key, args, seed, &errs);
                CHECK_MSG(errs.empty(), lang + " " + key + ": " + (errs.empty() ? "" : errs.front()));
                CHECK_MSG(clean(t), lang + " " + key + ": " + t);
                if (dump) dump << lang << " " << key << " #" << seed << ": " << t << "\n";
            }
        }
        // every frame in every form, with the speaker as doer too (I / me)
        for (const auto& key : loc.keys(lang)) {
            if (key.rfind("frame.", 0) != 0) continue;
            const std::string pred = key.substr(6);
            Json roles = {{"doer", c.oleg}, {"victim", c.miller}, {"object", c.item}, {"beneficiary", c.miller},
                          {"thinker", c.oleg}, {"speaker", c.brana}, {"listener", c.miller}, {"content", c.theft}};
            if (pred == "lose") roles.erase("doer");
            for (const char* tense : {"past", "future"}) // ru perfectives have no present
                for (const bool neg : {false, true}) {
                    const Json p = {{"pred", pred}, {"tense", tense}, {"neg", neg}, {"roles", roles}};
                    for (const char* form : {"clause", "nominal", "about"})
                        for (std::uint64_t seed = 0; seed < 3; ++seed) {
                            std::vector<std::string> errs;
                            const std::string t = realizeProp(loc, lang, p, form, "", {{"speaker", c.oleg}}, seed, &errs);
                            CHECK_MSG(errs.empty(), lang + " " + key + " " + form + ": " + (errs.empty() ? "" : errs.front()));
                            CHECK_MSG(clean(t), lang + " " + key + " " + form + ": " + t);
                            if (dump && seed == 0)
                                dump << lang << " " << key << " " << form << " " << tense << (neg ? " neg" : "") << ": " << t
                                     << "\n";
                        }
                }
        }
    }
}
