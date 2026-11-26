// lr_sim_runner - headless runs of the LivelyRealms life-sim core.
//   lr_sim_runner [--content DIR] [--world FILE] [--seed N] [--days N]
//                     [--save FILE] [--report FILE] [--trace FILE] [--validate]
//                     [--loc DIR] [--lang ru|en]
//                     [--dialogue-corpus N [--corpus-out FILE(.txt|.json)] [--corpus-report FILE.json]
//                      [--corpus-game]]
//   --dialogue-corpus: N lines through rules + LOC v0.2 by --seed, quality checks
//   (AL-54, sim/dialogue_corpus.hpp); exit 1 when the checks find errors.
//   --corpus-game renders the lines as the game does now (utteranceText) instead of
//   with every argument the content may use.
// Trace and report carry codes + params; with a loc table their "text" fields
// are assembled in --lang (doc/design/LOC_FORMAT.md).

#include <filesystem>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>

#include "sim/dialogue_corpus.hpp"
#include "sim/data_fs.hpp"
#include "sim/dialogue_gen.hpp"
#include "sim/dialogue_rules.hpp"
#include "sim/ai.hpp"
#include "sim/loc.hpp"
#include "sim/loc_gen.hpp"
#include "sim/realm.hpp"
#include "sim/caravan.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {
// AL-93: one line of the village's food - stores, sacks, fields, gardens, the
// hungry and the dead by cause - every N days of a run.
void writeLedger(std::ostream& out, const World& w, bool header) {
    const char* items[] = {"grain", "seed_grain", "sheaf", "vegetables", "seed_veg", "meal", "bread",
                           "smoked_fish", "dried_meat", "meat", "fish", "cheese", "dried_berries", "hazelnuts", "roots"};
    const char* fieldKeys[] = {"strips", "tilled", "sown", "ripe"};
    if (header) {
        out << "day,year,doy,alive,children,hungry,starving,families,food_days_store,food_days_carried";
        for (const char* i : items) out << ",st_" << i;
        for (const char* t : {"field", "garden"})
            for (const char* k : fieldKeys) out << "," << t << "_" << k;
        out << ",dead,acts\n";
    }
    const Content& ct = w.content();
    const NeedDef* hn = ct.need("hunger");
    const double perDay = hn ? hn->decayPerDay : 0.55;
    int alive = 0, children = 0, hungry = 0, starving = 0;
    double carried = 0.0;
    std::map<std::string, int> dead;
    for (const auto& [id, c] : w.characters()) {
        if (c.isAnimal()) continue;
        if (!c.body.alive) { ++dead[c.body.causeOfDeath.empty() ? "?" : c.body.causeOfDeath]; continue; }
        ++alive;
        if (c.body.stage() == "child" || c.body.stage() == "infant") ++children;
        if (c.need("hunger") < 0.3) ++hungry;
        if (c.need("hunger") < 0.1) ++starving;
        for (const auto& [item, n] : c.inventory) {
            auto d = ct.items.find(item);
            if (d != ct.items.end()) carried += d->second.nutrition * n;
        }
    }
    std::map<std::string, long> st;
    double store = 0.0;
    int families = 0;
    for (const auto& [fid, f] : w.families()) {
        const WorldObject* h = w.object(f.storage);
        if (!h) continue;
        bool any = false;
        for (EntityId m : f.members)
            if (const Character* c = w.character(m); c && c->body.alive) any = true;
        if (any) ++families;
        for (const auto& [item, n] : h->stock) {
            st[item] += n;
            auto d = ct.items.find(item);
            if (d != ct.items.end()) store += d->second.nutrition * n;
        }
    }
    std::map<std::string, long> fk;
    for (const auto& [oid, o] : w.objects()) {
        if (o.type != "field" && o.type != "garden") continue;
        for (const char* k : fieldKeys) {
            auto it = o.stock.find(k);
            if (it != o.stock.end()) fk[o.type + "_" + k] += it->second;
        }
    }
    const int day = w.day();
    const int doy = (w.startDay() + day) % kDaysPerYear;
    const double eaters = std::max(1, alive) * perDay;
    out << day << "," << day / kDaysPerYear + 1 << "," << doy << "," << alive << "," << children << "," << hungry << ","
        << starving << "," << families << "," << store / eaters << "," << carried / eaters;
    for (const char* i : items) out << "," << st[i];
    for (const char* t : {"field", "garden"})
        for (const char* k : fieldKeys) out << "," << fk[std::string(t) + "_" + k];
    out << ",";
    bool first = true;
    for (const auto& [cause, n] : dead) out << (first ? "" : ";") << cause << ":" << n, first = false;
    // What was done with food since the last line (interactions that move grain,
    // seed and food), as id:count.
    static std::map<std::string, long> last;
    out << ",";
    first = true;
    for (const auto& [id, n] : interactionTally()) {
        const long d = n - last[id];
        if (d <= 0) continue;
        out << (first ? "" : ";") << id << ":" << d, first = false;
    }
    last = interactionTally();
    out << "\n";
    out.flush();
}
} // namespace

int main(int argc, char** argv) {
    // LR-142: content/ beside the exe or the source tree, with mods/ over it (sim/data_fs.hpp)
    std::string root = std::filesystem::path(lr::sim::data::mountDefault(argc > 0 ? argv[0] : "",
                                                                         std::string(LR_SIM_SOURCE_DIR) + "/content"))
                           .parent_path()
                           .generic_string();
    std::string contentDir = root + "/content/ai";
    std::string worldFile = root + "/content/sim/test_world.json";
    std::string locDir = root + "/content/loc";
    std::string lang = "ru";
    std::string saveFile, reportFile, traceFile, chronicleFile, chronicleFilter, ledgerFile;
    int ledgerEvery = 5;
    bool realmOnly = false;
    std::uint64_t seed = 0;
    int days = 3;
    bool validateOnly = false;
    int corpusCount = 0;
    std::string corpusOut, corpusReport;
    bool corpusGame = false;
    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << argv[i] << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (!std::strcmp(argv[i], "--content")) contentDir = next();
        else if (!std::strcmp(argv[i], "--world")) worldFile = next();
        else if (!std::strcmp(argv[i], "--seed")) seed = std::stoull(next());
        else if (!std::strcmp(argv[i], "--days")) days = std::stoi(next());
        else if (!std::strcmp(argv[i], "--save")) saveFile = next();
        else if (!std::strcmp(argv[i], "--report")) reportFile = next();
        else if (!std::strcmp(argv[i], "--trace")) traceFile = next();
        else if (!std::strcmp(argv[i], "--realm-only")) realmOnly = true;          // AL-85: the region's history alone (the village fed, no minds)
        else if (!std::strcmp(argv[i], "--chronicle")) chronicleFile = next();       // AL-85: the chronicle as text
        else if (!std::strcmp(argv[i], "--chronicle-only")) chronicleFilter = next(); // a code prefix, e.g. chronicle.realm.
        else if (!std::strcmp(argv[i], "--validate")) validateOnly = true;
        else if (!std::strcmp(argv[i], "--ledger")) ledgerFile = next();            // AL-93: the village's food, day by day (csv)
        else if (!std::strcmp(argv[i], "--ledger-every")) ledgerEvery = std::stoi(next());
        else if (!std::strcmp(argv[i], "--loc")) locDir = next();
        else if (!std::strcmp(argv[i], "--lang")) lang = next();
        else if (!std::strcmp(argv[i], "--dialogue-corpus")) corpusCount = std::stoi(next());
        else if (!std::strcmp(argv[i], "--corpus-out")) corpusOut = next();
        else if (!std::strcmp(argv[i], "--corpus-report")) corpusReport = next();
        else if (!std::strcmp(argv[i], "--corpus-game")) corpusGame = true;
        else {
            std::cerr << "unknown argument " << argv[i] << "\n";
            return 2;
        }
    }
    try {
        const Content content = Content::load(contentDir);
        for (const auto& w : content.warnings) std::cerr << "warning: " << w << "\n";
        const Loc loc = Loc::load(locDir);
        const ValidationReport lv = validateLoc(content, loc);
        for (const auto& w : lv.warnings) std::cerr << "warning: " << w << "\n";
        for (const auto& e : lv.errors) std::cerr << "error: " << e << "\n";
        if (!lv.ok()) return 1;
        const ValidationReport dv = validateDialogueGen(dialogueGenData(content), &content, &loc);
        for (const auto& w : dv.warnings) std::cerr << "warning: " << w << "\n";
        for (const auto& e : dv.errors) std::cerr << "error: " << e << "\n";
        if (!dv.ok()) return 1;
        const ValidationReport rv = validateDialogueRules(dialogueGenData(content), &content);
        for (const auto& w : rv.warnings) std::cerr << "warning: " << w << "\n";
        for (const auto& e : rv.errors) std::cerr << "error: " << e << "\n";
        if (!rv.ok()) return 1;
        {   // LOC v0.2 (AL-47): a frame per predicate, frame roles known to the predicate
            std::vector<std::string> preds;
            std::map<std::string, std::vector<std::string>> roles;
            for (const auto& [id, pd] : dialogueGenData(content).predicates) {
                preds.push_back(id);
                for (const auto& [rn, kinds] : pd.roles) roles[id].push_back(rn);
            }
            const ValidationReport fv = validateLocGen(loc, preds, roles);
            const std::set<std::string> said(lv.warnings.begin(), lv.warnings.end()); // validateLoc ran the structure part
            for (const auto& w : fv.warnings)
                if (!said.count(w)) std::cerr << "warning: " << w << "\n";
            for (const auto& e : fv.errors) std::cerr << "error: " << e << "\n";
            if (!fv.ok()) return 1;
        }
        if (!loc.hasLanguage(lang))
            std::cerr << "warning: no loc table for '" << lang << "', using " << loc.fallback() << "\n";
        if (corpusCount > 0) { // AL-54: the corpus of lines and its quality checks
            CorpusOptions co;
            co.count = corpusCount;
            co.seed = seed ? seed : 1;
            co.lang = loc.hasLanguage(lang) ? lang : loc.fallback();
            co.game = corpusGame;
            const std::vector<CorpusLine> lines = generateDialogueCorpus(content, loc, co);
            const CorpusReport rep = checkDialogueCorpus(lines, content, loc, co.lang, co.run);
            if (!corpusOut.empty()) {
                const bool json = corpusOut.size() > 5 && corpusOut.compare(corpusOut.size() - 5, 5, ".json") == 0;
                std::ofstream(corpusOut, std::ios::binary) << (json ? corpusJson(lines).dump(1) : corpusText(lines));
            }
            if (!corpusReport.empty()) std::ofstream(corpusReport, std::ios::binary) << toJson(rep).dump(1);
            std::cout << reportText(rep);
            return rep.errors() ? 1 : 0;
        }
        if (validateOnly) {
            std::cout << "content ok: " << content.goals.size() << " goals, " << content.tasks.size() << " tasks, "
                      << content.interactions.size() << " interactions, " << content.dialogue.size()
                      << " dialogue templates; loc ok: " << requiredLocKeys(content).size() << " keys, base "
                      << loc.base() << "\n";
            if (content.expansion.loaded)
                std::cout << "expansion: +" << content.expansion.added << " entries, " << content.expansion.kept.size()
                          << " ids kept from the base, " << content.knowledge.size() << " skills, "
                          << content.traits.size() << " traits, " << content.expansion.interests.size()
                          << " interests, " << content.expansion.attributes.size() << " attributes, "
                          << content.expansion.temperaments.size() << " temperaments\n";
            return 0;
        }
        World world = makeTestWorld(content, loadJsonFile(worldFile), seed);
        if (realmOnly) {
            installRealm(world);
            installCaravans(world); // LR-154
            for (int d = 0; d < days; ++d) {
                for (auto& [id, c] : world.characters())
                    if (!c.isAnimal() && c.body.alive) c.needs["hunger"] = 1.0, c.needs["thirst"] = 1.0;
                world.run(kTicksPerDay);
                realmDaily(world);
            }
        } else if (!ledgerFile.empty()) {
            installAi(world);
            std::ofstream led(ledgerFile);
            for (int d = 0; d < days; ++d) {
                world.run(kTicksPerDay);
                if (d % std::max(1, ledgerEvery) == 0 || d + 1 == days) writeLedger(led, world, d == 0);
            }
        } else {
            installAi(world);
            world.run(static_cast<Tick>(days) * kTicksPerDay);
        }
        Json report = aiReport(world);
        localizeReport(report, loc, lang, world);
        std::cout << "alive " << report["alive"] << "/" << report["population"] << "\n"
                  << "why_hunger: " << report["summary"]["why_hunger"].dump() << "\n"
                  << "why_idle: " << report["summary"]["why_idle"].dump() << "\n"
                  << "plan_failures: " << report["summary"]["plan_failures"].dump() << "\n";
        if (!reportFile.empty()) std::ofstream(reportFile) << report.dump(1);
        if (!chronicleFile.empty()) {
            std::ofstream out(chronicleFile);
            for (const auto& e : world.chronicleLog()) {
                if (!chronicleFilter.empty() && e.code.rfind(chronicleFilter, 0) != 0) continue;
                const int d = static_cast<int>(e.tick / kTicksPerDay);
                out << "y" << (d / kDaysPerYear + 1) << " d" << (d % kDaysPerYear + 1) << "\t"
                    << loc.text(lang, e.code, locArgs(loc, lang, world, e.params)) << "\n";
            }
        }
        if (!traceFile.empty()) {
            Json t = Json::object();
            for (const auto& [id, c] : world.characters()) {
                Json lines = Json::array();
                for (const auto& e : c.ai.trace)
                    lines.push_back({{"tick", e.tick}, {"layer", std::string(1, e.layer)}, {"code", e.code},
                                     {"params", e.params}, {"text", traceText(loc, lang, world, c, e)}});
                t[std::to_string(id) + " " + c.name] = lines;
            }
            std::ofstream(traceFile) << t.dump(1);
        }
        std::cout << "seed " << world.seed() << ", day " << world.day() << ", " << world.characters().size()
                  << " characters, hash " << std::hex << world.stateHash() << std::dec << "\n";
        if (!saveFile.empty()) {
            std::ofstream(saveFile) << world.toJson().dump(1);
            std::cout << "saved " << saveFile << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
