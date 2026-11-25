// AL-100: the character sheet of the 2D client (src/client2d/sheet2d): the five
// pages of one person read from a living village, with the names of the language.
#include <set>
#include <string>

#include "client2d/sheet2d.hpp"
#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;
namespace c2 = lr::client2d;

namespace {

const std::string kRoot = LR_SIM_SOURCE_DIR;

World& sheetVillage() {
    static const Content content = Content::load(kRoot + "/content/ai");
    static World w = [] {
        Json cfg = loadJsonFile(kRoot + "/content/sim/test_world.json");
        cfg["village_plan"] = true;
        World v = makeTestWorld(content, cfg, 11);
        installAi(v);
        for (int i = 0; i < int(kTicksPerDay); ++i) v.step();   // a day: memories, moodlets, relations
        return v;
    }();
    return w;
}

const Loc& sheetLoc() {
    static const Loc l = Loc::load(kRoot + "/content/loc");
    return l;
}

} // namespace

TEST(lr_client2d_sheet_pages) {
    World& w = sheetVillage();
    c2::ViewText t;
    t.loc = &sheetLoc();
    int people = 0, withRel = 0, withKin = 0, withMem = 0;
    for (const auto& [id, c] : w.characters()) {
        if (c.isAnimal()) continue;
        ++people;
        const Json d = c2::sheetJson(w, t, id);
        CHECK(d.is_object());
        for (const char* page : {"overview", "relations", "things", "skills", "chronicle"}) CHECK(d.contains(page));
        CHECK(!d["name"].get<std::string>().empty());
        CHECK(d["overview"]["needs"].is_array() && !d["overview"]["needs"].empty());
        for (const auto& n : d["overview"]["needs"]) CHECK(n["name"].get<std::string>().rfind("need.", 0) != 0);
        const std::set<std::string> groups{"family", "friends", "foes", "known"};
        if (!d["relations"].empty()) ++withRel;
        for (const auto& r : d["relations"]) {
            CHECK(groups.count(r["group"].get<std::string>()) == 1);
            CHECK(r["axes"].contains("affinity") && r["axes"].contains("trust"));
            if (!r["kin"].empty()) {
                ++withKin;
                // kin words are the language's, not the ids
                for (const auto& k : r["kin"]) CHECK(k.get<std::string>().find('.') == std::string::npos);
            }
        }
        for (const auto& s : d["skills"]["skills"]) CHECK(s["v"].get<double>() >= 0.0);
        for (const auto& wn : d["things"]["worn"]) CHECK(!wn["slotName"].get<std::string>().empty());
        if (!d["chronicle"]["mem"].empty()) ++withMem;
        for (const auto& m : d["chronicle"]["mem"]) {
            const std::string s = m["text"].get<std::string>();
            CHECK(!s.empty());
            CHECK(s.find("{self") == std::string::npos && s.find("{who") == std::string::npos);
        }
    }
    CHECK(people > 0);
    CHECK(withRel > 0);
    CHECK(withKin > 0);
    CHECK(withMem > 0);
    CHECK(c2::sheetJson(w, t, 999999999).is_null());
}

// Every ui2d.sheet.* key the page uses is in both languages.
TEST(lr_client2d_sheet_loc) {
    const Loc& l = sheetLoc();
    std::set<std::string> ru, en;
    for (const auto& k : l.keys("ru"))
        if (k.rfind("ui2d.sheet.", 0) == 0) ru.insert(k);
    for (const auto& k : l.keys("en"))
        if (k.rfind("ui2d.sheet.", 0) == 0) en.insert(k);
    CHECK(ru.size() > 40);
    CHECK(ru == en);
}
