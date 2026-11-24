// AL-86 resources pack: every crafting material has a source in the world, every
// starter design whose archetype the sim counts becomes an act NPCs can plan
// (craft:<design>), and the world generator places the new sources.

#include <iostream>
#include <set>
#include <string>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/content.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;

namespace {

const Content& resContent() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

// Materials still waiting for their source: the later stages of AL-86 shrink this list.
const std::set<std::string>& pendingMaterials() {
    static const std::set<std::string> s{"wheat_grain", // the split of the field into crops: farming
                                         "salt", "tin_ore", "silver", "gold"}; // salt springs and trade, AL-91
    return s;
}

bool designPending(const Content& c, const craft::DesignDef& ds) {
    for (const auto& st : ds.steps)
        for (const auto& [slot, ins] : st.slots)
            for (const auto& in : ins)
                if (in.ref.rfind("mat:", 0) == 0 && pendingMaterials().count(in.ref.substr(4))) return true;
    (void)c;
    return false;
}

// an authored chore at a place already makes it out of one's own stuff (brew_ale)
bool authoredAt(const Content& c, const std::string& item) {
    for (const auto& [id, a] : c.interactions) {
        if (a.target.rfind("object:", 0) != 0 || a.craft.rfind("design:", 0) == 0) continue;
        bool uses = false, gives = false;
        for (const auto& e : a.eff) {
            uses = uses || (e.subject == "has" && e.op == '-' && !e.onTarget);
            gives = gives || (e.subject == "has" && e.op == '+' && !e.onTarget && e.key == item);
        }
        if (uses && gives) return true;
    }
    return false;
}

} // namespace

TEST(altera_resources_every_material_has_a_source) {
    const Content& c = resContent();
    // what the world hands out: object stocks and the effects of acts
    std::set<std::string> given;
    for (const auto& [id, o] : c.objects)
        for (const auto& [item, n] : o.stock) given.insert(item);
    for (const auto& [id, a] : c.interactions)
        for (const auto& e : a.eff)
            if (e.subject == "has" && e.op == '+' && !e.onTarget) given.insert(e.key);
    // and what the travelling trader brings (tin, silver, gold come from afar)
    const Json trade = loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai/trade.json");
    for (const auto& [role, r] : trade.at("roles").items())
        if (r.contains("stock"))
            for (const auto& [item, n] : r.at("stock").items()) given.insert(item);
    int bad = 0;
    for (const auto& [id, m] : c.craft.materials) {
        if (!m.raw || pendingMaterials().count(id)) continue;
        if (m.item.empty() || !c.items.count(m.item)) {
            std::cerr << "    " << id << ": no item\n";
            ++bad;
        } else if (!given.count(m.item)) {
            std::cerr << "    " << id << ": nothing gives " << m.item << "\n";
            ++bad;
        }
    }
    CHECK(bad == 0);
}

TEST(altera_resources_designs_become_acts) {
    const Content& c = resContent();
    int bad = 0, acts = 0;
    for (const auto& ds : c.craft.designs) {
        const craft::FormDef* f = c.craft.form(ds.form);
        if (!f || f->item.empty()) {
            std::cerr << "    " << ds.id << ": the archetype counts as no item\n";
            ++bad;
            continue;
        }
        if (c.interactions.count("craft:" + ds.id)) {
            ++acts;
            continue;
        }
        if (designPending(c, ds)) continue;
        if (authoredAt(c, f->item)) continue; // brewed by the cuisine's own chores
        std::cerr << "    " << ds.id << ": no craft act\n";
        ++bad;
    }
    std::cerr << "    craft acts: " << acts << " of " << c.craft.designs.size() << "\n";
    CHECK(bad == 0);
}

TEST(altera_resources_world_places_the_sources) {
    static const Json cfg = loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(resContent(), cfg, 7);
    int species = 0;
    for (const char* t : {"oak_tree", "ash_tree", "pine_tree", "birch_tree", "linden_tree"})
        species += static_cast<int>(w.objectsOfType(t).size());
    CHECK(species > 0);
    CHECK(!w.objectsOfType("charcoal_clamp").empty());
    CHECK(!w.objectsOfType("lime_kiln").empty());
}
