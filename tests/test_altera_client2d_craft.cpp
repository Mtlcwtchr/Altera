// AL-97: the workshop of the 2D client (src/client2d/craft2d): what the page
// reads of a person's crafting, the helper's drafts and the commands it sends.
#include <string>

#include "client2d/craft2d.hpp"
#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/craft_sim.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;
namespace c2 = altera::client2d;

namespace {

const std::string kRoot = ALTERA_SIM_SOURCE_DIR;

struct Shop {
    Content content = Content::load(kRoot + "/content/ai");
    Loc loc = Loc::load(kRoot + "/content/loc");
    Json ui = loadJsonFile(kRoot + "/content/ui/craft.json");
    World world;
    c2::ViewText text;
    EntityId who = kNone;
    Shop() : world([this] {
        Json cfg = loadJsonFile(kRoot + "/content/sim/test_world.json");
        cfg["village_plan"] = true;
        return makeTestWorld(content, cfg, 7);
    }()) {
        installAi(world);
        text.loc = &loc;
        text.lang = "ru";
        for (const auto& [id, c] : world.characters())
            if (!c.isAnimal() && c.body.alive && c.body.stage() == "adult") {
                who = id;
                break;
            }
    }
    Json ws(const c2::WorkshopQuery& q = {}) { return c2::workshopJson(world, text, ui, who, q); }
    std::string run(const std::string& type, Json args) {
        Command cmd;
        cmd.source = "player:1";
        cmd.type = type;
        args["id"] = who;
        cmd.args = args;
        return world.applyNow(cmd).result;
    }
};

Shop& shop() {
    static Shop s;
    return s;
}

} // namespace

TEST(altera_client2d_workshop_json) {
    Shop& s = shop();
    CHECK(s.who != kNone);
    CHECK(c2::workshopJson(s.world, s.text, s.ui, kNone, {}).is_null());
    const Json j = s.ws();
    CHECK(j.is_object());
    CHECK(j["processes"].size() >= 10);
    for (const auto& p : j["processes"]) {
        CHECK(!p["name"].get<std::string>().empty());
        CHECK(p["name"].get<std::string>().rfind("process.", 0) != 0);   // a name, never a key
        if (!p["available"].get<bool>()) CHECK(!p["need"].get<std::string>().empty());
    }
    CHECK_EQ(j["wishes"].size(), s.ui["wishes"].size());
    for (const auto& w : j["wishes"]) CHECK(w["name"].get<std::string>().rfind("hud.", 0) != 0);
    const Json t = c2::workshopTexts(s.text);
    CHECK(t.contains("hud.craft.hold"));
    CHECK(t.contains("craft.tier.good"));
}

TEST(altera_client2d_workshop_kit_and_book) {
    Shop& s = shop();
    Character& c = *s.world.character(s.who);
    c2::craftKit(s.world, c);
    const Json j = s.ws();
    CHECK(j["materials"].size() >= 5);
    for (const auto& m : j["materials"]) {
        CHECK(m["ref"].get<std::string>().rfind("inv:", 0) == 0);
        CHECK(m["n"].get<int>() > 0);
    }
    CHECK(j["made"].size() >= 3);
    CHECK(j["recipes"].size() >= 5);
    // a mastered starter recipe made from memory, as the book's button sends it
    std::string rid;
    for (const auto& r : j["recipes"])
        if (r["master"].get<bool>() && r["can"].get<bool>()) {
            rid = r["id"];
            break;
        }
    CHECK(!rid.empty());
    const int seq = c.craft.lastSeq;
    s.run("craft_fast", {{"recipe", rid}, {"count", 1}});
    CHECK(c.craft.lastSeq > seq);
    const Json after = s.ws();
    CHECK(after["last"].is_object());
    CHECK(!after["last"]["head"].get<std::string>().empty());
}

TEST(altera_client2d_workshop_helper_draft) {
    Shop& s = shop();
    Character& c = *s.world.character(s.who);
    if (c.inventory["fiber_raw"] <= 0) c2::craftKit(s.world, c);
    // D: a wish gives candidates; one with a step to do now carries a draft the
    // page lays on the bench; the forecast reads it and craft_attempt runs it
    bool drafted = false;
    for (const auto& w : s.ui["wishes"]) {
        c2::WorkshopQuery q;
        q.wish = w["id"];
        const Json j = s.ws(q);
        CHECK(j["candidates"].size() <= size_t(s.ui.value("helper_max", 3)));
        for (const auto& cd : j["candidates"]) {
            CHECK(cd["fit"].get<double>() >= 0.0 && cd["fit"].get<double>() <= 1.0);
            CHECK(!cd["steps"].empty());
            if (!cd.contains("draft") || drafted) continue;
            c2::WorkshopQuery dq;
            dq.draft = cd["draft"];
            const Json f = s.ws(dq)["forecast"];
            CHECK(f.is_object());
            if (!f["can"].get<bool>()) {
                CHECK(!f["why"].get<std::string>().empty());
                continue;
            }
            CHECK(!f["text"].get<std::string>().empty());
            const int seq = c.craft.lastSeq;
            Json args = cd["draft"];
            s.run("craft_attempt", args);
            CHECK(c.craft.lastSeq > seq);
            drafted = true;
        }
    }
    CHECK(drafted);
}
