// LR-155: the culture and faith window (/api/culture) and "inspect" (/api/inspect) of the
// 2D client (src/client2d/culture2d), read from a living village in the language's names.
#include <string>

#include "client2d/culture2d.hpp"
#include "client2d/sheet2d.hpp"
#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/culture.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;
namespace c2 = lr::client2d;

namespace {

const std::string kRoot = LR_SIM_SOURCE_DIR;

World& village() {
    static const Content content = Content::load(kRoot + "/content/ai");
    static World w = [] {
        Json cfg = loadJsonFile(kRoot + "/content/sim/test_world.json");
        World v = makeTestWorld(content, cfg, 13);
        installAi(v);
        v.run(kTicksPerDay + 2);   // a day: the culture pass has run once
        return v;
    }();
    return w;
}

const Loc& loc() {
    static const Loc l = Loc::load(kRoot + "/content/loc");
    return l;
}

EntityId someone(const World& w, EntityId notThis = kNone) {
    for (const auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && id != notThis) return id;
    return kNone;
}

} // namespace

TEST(lr_client2d_culture_window) {
    World& w = village();
    c2::ViewText t{&loc(), "ru"};
    const EntityId me = someone(w);
    const Json d = c2::cultureJson(w, t, me, me);
    CHECK(d.is_object());
    CHECK(d.contains("person") && d["person"]["faithName"].get<std::string>() != d["person"]["faith"].get<std::string>());
    CHECK(d.contains("culture") && !d["culture"]["values"].empty() && !d["culture"]["traditions"].empty());
    CHECK(d["culture"]["rites"].size() == 3);
    CHECK(d.contains("settlement") && d["settlement"]["residents"].get<int>() > 0);
    CHECK(!d["settlement"]["traditions"].empty());
    CHECK(d["settlement"]["traditions"][0].contains("status"));
    CHECK(d.contains("faith") && !d["faith"]["deities"].empty() && !d["faith"]["holy"].empty());
    // names, not ids
    CHECK(d["faith"]["name"].get<std::string>() != d["faith"]["id"].get<std::string>());
    CHECK(d["faith"]["deities"][0]["name"].get<std::string>() != d["faith"]["deities"][0]["id"].get<std::string>());
    // another faith picked to browse
    const Json o = c2::cultureJson(w, t, me, me, "green_covenant", "sylvan");
    CHECK_EQ(o["faith"]["id"].get<std::string>(), std::string("green_covenant"));
    CHECK_EQ(o["culture"]["id"].get<std::string>(), std::string("sylvan"));
    // the sheet shows the person's culture and faith
    const Json s = c2::sheetJson(w, t, me);
    CHECK(s.contains("belief") && !s["belief"]["faithName"].get<std::string>().empty());
}

TEST(lr_client2d_inspect_by_knowledge) {
    World& w = village();
    c2::ViewText t{&loc(), "ru"};
    const EntityId me = someone(w);
    Character& m = *w.character(me);
    const Family* mf = w.familyOf(m);
    // someone of another household and dynasty
    EntityId other = kNone;
    for (const auto& [id, c] : w.characters()) {
        const Family* f = w.familyOf(c);
        if (c.isAnimal() || !c.body.alive || !f || !mf || f->id == mf->id || (mf->dynasty != kNone && f->dynasty == mf->dynasty)) continue;
        other = id;
        break;
    }
    CHECK(other != kNone);
    if (other == kNone) return;
    // nobody of the dynasty knows him: a neighbour seen about the village
    for (auto& [id, c] : w.characters()) c.relations.erase(other);
    Json a = c2::inspectJson(w, t, me, other);
    CHECK(a.is_object());
    CHECK(!a["kin"].get<bool>());
    CHECK(a["fields"].contains("culture"));
    CHECK(!a["fields"].contains("faith"));
    CHECK(!a["fields"].contains("traits"));
    // well known: faith with piety, traits, skills
    m.relations[other].familiarity = 0.9;
    Json b = c2::inspectJson(w, t, me, other);
    CHECK(b["known"].get<bool>());
    CHECK(b["fields"].contains("faith") && b["fields"]["faith"].contains("piety"));
    CHECK(b["fields"].contains("traits") && b["fields"].contains("skills"));
    CHECK(b.contains("memories") && b.contains("mine"));
    // what a kinsman knows, the dynasty knows
    m.relations.erase(other);
    for (auto& [id, c] : w.characters())
        if (id != me && w.familyOf(c) == mf) {
            c.relations[other].familiarity = 0.9;
            break;
        }
    Json k = c2::inspectJson(w, t, me, other);
    CHECK(k["fields"].contains("faith"));
    CHECK(c2::inspectJson(w, t, me, 999999999).is_null());
}
