// AL-76: village buildings by purpose, from modules (sim/structure_plan).
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/building.hpp"
#include "sim/structure_plan.hpp"
#include "sim/village_plan.hpp"
#include "sim/world.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;

namespace {

const Content& realContent() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}
const StructureCatalog& catalog() {
    static const StructureCatalog c = parseStructures(realContent().structures);
    return c;
}

bool hasStation(const StructurePlan& s, const std::string& what) {
    for (const auto& t : s.stations)
        if (t.what == what) return true;
    return false;
}
const StructureVolume* volume(const StructurePlan& s, const std::string& role) {
    for (const auto& v : s.volumes)
        if (v.role == role) return &v;
    return nullptr;
}
bool inVolume(const StructureVolume& v, const LocalThing& t) { return t.i >= v.i0 && t.i < v.i1 && t.j >= v.j0 && t.j < v.j1; }

// The plan's modules on the world grid, as worldgen lays them, built.
PieceMap piecesOf(const StructurePlan& s, const StructureFrame& f) {
    PieceMap m;
    EntityId id = 1;
    for (const auto& lp : s.pieces) {
        BuildPiece p;
        p.id = id++;
        p.kind = lp.kind;
        f.cell(lp.i, lp.j, p.x, p.y);
        p.z = lp.z;
        p.rot = f.rot(lp.rot);
        p.built = true;
        m[p.id] = p;
    }
    return m;
}

void report(const std::string& what, const std::vector<std::string>& bad) {
    for (const auto& b : bad) std::printf("    %s: %s\n", what.c_str(), b.c_str());
}

}  // namespace

TEST(altera_sim_structures_smithy) {
    const StructureCatalog& cat = catalog();
    CHECK(cat.loaded());
    const StructureTypeDef* smithy = cat.find("smithy");
    CHECK(smithy != nullptr);
    if (!smithy) return;
    for (std::uint64_t seed = 1; seed <= 12; ++seed) {
        const StructurePlan s = planStructure(cat, realContent().build, *smithy, {5, seed});
        // a house with an open canopy beside it: the forge, the anvil, the tub and the grindstone under it
        CHECK(!s.volumes.empty() && s.volumes[0].role == "main");
        const StructureVolume* canopy = volume(s, "canopy");
        CHECK(canopy != nullptr);
        if (!canopy) continue;
        for (const char* st : {"forge", "anvil", "quench_tub", "grindstone"}) {
            CHECK(hasStation(s, st));
            for (const auto& t : s.stations)
                if (t.what == st) CHECK(inVolume(*canopy, t));
        }
        CHECK(hasStation(s, "hearth"));
        // the modules: walls, a door, a roof, posts under the canopy
        int walls = 0, doors = 0, roofs = 0, posts = 0;
        for (const auto& p : s.pieces) {
            const BuildPieceDef* d = realContent().build.find(p.kind);
            CHECK(d != nullptr);
            if (!d) continue;
            walls += d->category == "wall";
            doors += d->door;
            roofs += d->category == "roof";
            posts += d->category == "pillar" && p.i >= canopy->i0 - 1 && p.i <= canopy->i1 && p.j >= canopy->j0 - 1 && p.j <= canopy->j1;
        }
        CHECK(walls >= 10);
        CHECK(doors >= 2);   // the front door and the one into the canopy
        CHECK(roofs >= 9 + 4);
        CHECK(posts >= 4);
        const auto bad = checkStructure(realContent().build, s);
        report("smithy", bad);
        CHECK(bad.empty());
    }
}

TEST(altera_sim_structures_every_type) {
    const StructureCatalog& cat = catalog();
    const BuildCatalog& build = realContent().build;
    CHECK(cat.types.size() >= 8);
    std::set<std::string> annexKinds;
    for (const auto& type : cat.types)
        for (std::uint64_t seed = 1; seed <= 10; ++seed) {
            const StructurePlan s = planStructure(cat, build, type, {seed % 2 ? 4 : 7, seed * 7919});
            const auto bad = checkStructure(build, s);
            report(type.id + " seed " + std::to_string(seed), bad);
            CHECK(bad.empty());
            for (const auto& n : s.notes) std::printf("    %s seed %d note: %s\n", type.id.c_str(), int(seed), n.c_str());
            CHECK(s.notes.empty());
            // every station the type names (that the world has) stands in it
            for (const auto& st : type.stations)
                if (realContent().objects.count(st.what)) CHECK(hasStation(s, st.what));
            for (const auto& v : s.volumes) annexKinds.insert(v.role);
            // turned any way on the grid it still stands: no slot taken twice, every piece held
            for (int q = 0; q < 4; ++q) {
                StructureFrame f{q, 17, -5, build.cellM};
                PieceMap m = piecesOf(s, f);
                std::set<SlotKey> slots;
                for (const auto& [id, p] : m) CHECK(slots.insert(slotOf(*build.find(p.kind), p.x, p.y, p.z, p.rot)).second);
                computeStability(build, m);
                int weak = 0;
                for (const auto& [id, p] : m)
                    if (p.builtStability < build.minStability) {
                        if (weak++ < 3)
                            std::printf("    %s seed %d q %d: %s at %d,%d,%d rot %d stability %.2f\n", type.id.c_str(), int(seed), q,
                                        p.kind.c_str(), p.x, p.y, p.z, p.rot, p.builtStability);
                    }
                CHECK(weak == 0);
            }
            // the same seed, the same building
            const StructurePlan again = planStructure(cat, build, type, {seed % 2 ? 4 : 7, seed * 7919});
            CHECK(again.pieces.size() == s.pieces.size());
        }
    for (const char* k : {"main", "canopy", "shed", "porch"}) CHECK(annexKinds.count(k));
}

TEST(altera_sim_structures_frame) {
    // a cell and its rot turn together: a wall on the front edge of cell (0, 0)
    // faces the way the frame turns the front
    for (int q = 0; q < 4; ++q) {
        const StructureFrame f{q, 10, 20, 2.0};
        int x, y;
        f.cell(0, 0, x, y);
        BuildPieceDef wall{"w", "edge", "wall"};
        BuildPiece p;
        p.x = x, p.y = y, p.rot = f.rot(0);
        const Vec2 c = pieceCentre(BuildCatalog{}, wall, p);
        const Vec2 local = f.point(1.0, 0.0);   // the front edge's middle in the frame
        CHECK(dist(c, local) < 1e-9);
        CHECK(f.rot(4 + 1) == 4 + (1 + q) % 4);
    }
}

TEST(altera_sim_structures_village) {
    Json cfg = loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    cfg["village_plan"] = true;
    for (int families : {3, 7}) {
        cfg["families"] = families;
        World w = makeTestWorld(realContent(), cfg, 7 + families);
        const Settlement& st = w.settlements().begin()->second;
        std::map<std::string, int> types;
        for (const auto& s : st.structures) ++types[s.type];
        std::printf("    families %d:", families);
        for (const auto& [t, n] : types) std::printf(" %s %d", t.c_str(), n);
        std::printf("\n");
        CHECK(types.size() >= (families >= 7 ? 7u : 4u));
        CHECK(types.count("smithy") == 1);
        CHECK(types.count("chapel") == 1);
        if (families >= 3) CHECK(types.count("tavern") == 1);
        int homes = 0;
        for (const auto& s : st.structures) homes += s.family != kNone;
        CHECK(homes == families);
        int houses = 0;
        for (const auto& [id, o] : w.objects()) houses += o.type == "house";
        CHECK(houses == families);
        // the modules stand on the grid as built pieces, every one held up
        CHECK(w.pieces().size() > std::size_t(families) * 40);
        PieceMap pm = w.pieces();
        computeStability(realContent().build, pm);
        int weak = 0;
        for (const auto& [id, p] : pm) weak += p.builtStability < realContent().build.minStability;
        CHECK(weak == 0);
        // the footprints keep apart, every door opens onto open ground
        const double c = realContent().build.cellM;
        std::vector<PlanRect> rects;
        for (const auto& s : st.structures) {
            const StructureFrame f{s.q, s.gx, s.gy, c};
            PlanRect r;
            r.c = f.point((s.i0 + s.i1) * 0.5 * c, (s.j0 + s.j1) * 0.5 * c);
            r.yaw = s.q * 1.5707963267948966;
            r.hw = (s.i1 - s.i0) * c * 0.5 - 0.05;
            r.hd = (s.j1 - s.j0) * c * 0.5 - 0.05;
            rects.push_back(r);
        }
        for (std::size_t a = 0; a < rects.size(); ++a) {
            for (std::size_t b = a + 1; b < rects.size(); ++b) CHECK(!rects[a].overlaps(rects[b], 1.0));
            for (std::size_t b = 0; b < rects.size(); ++b) {
                if (b == a) continue;   // its own porch may stand before it
                PlanRect door{st.structures[a].door, 0, 0.3, 0.3};
                CHECK(!rects[b].overlaps(door, 0.0));
            }
        }
        // the smithy's forge and anvil stand in it, owned by the smith's household, whose head knows smithing
        for (const auto& s : st.structures) {
            if (s.type != "smithy") continue;
            std::set<std::string> held;
            for (EntityId id : s.objects)
                if (const WorldObject* o = w.object(id)) {
                    held.insert(o->type);
                    CHECK(o->ownerFamily == s.family);
                    CHECK(o->faced);
                }
            CHECK(held.count("forge") && held.count("anvil") && held.count("house") && held.count("hearth"));
            bool smith = false;
            for (const auto& [id, ch] : w.characters())
                if (ch.status.family == s.family && ch.status.role == "head") smith = ch.knowledge.count("smithing") > 0;
            CHECK(smith);
        }
        // the zones: the tavern and the chapel by the square, the barn out at the edge
        auto farOut = [&](const std::string& type) {
            for (std::size_t k = 0; k < st.structures.size(); ++k)
                if (st.structures[k].type == type) return dist(rects[k].c, st.square);
            return -1.0;
        };
        if (types.count("barn") && types.count("tavern")) CHECK(farOut("barn") > farOut("tavern"));
        if (types.count("barn") && types.count("chapel")) CHECK(farOut("barn") > farOut("chapel"));
        // saved and loaded, the buildings stay
        World again = makeTestWorld(realContent(), cfg, 7 + families);
        again.loadJson(w.toJson());
        const Settlement& back = again.settlements().begin()->second;
        CHECK(back.structures.size() == st.structures.size());
        if (!back.structures.empty()) {
            CHECK(back.structures[0].props.size() == st.structures[0].props.size());
            CHECK(back.structures[0].objects == st.structures[0].objects);
            CHECK(back.structures[0].gx == st.structures[0].gx);
        }
    }
}

TEST(altera_sim_structures_chapel_tavern) {
    const StructureCatalog& cat = catalog();
    const BuildCatalog& build = realContent().build;
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        // the chapel: a gable front, the aisle free from the door to the altar at the back
        const StructurePlan c = planStructure(cat, build, *cat.find("chapel"), {4, seed});
        const StructureVolume& m = c.volumes[0];
        bool altar = false;
        for (const auto& t : c.stations)
            if (t.what == "shrine") altar = t.i == (m.i1 - m.i0) / 2 && t.j == m.j1 - 1;
        CHECK(altar);
        for (const auto& t : c.decor) CHECK(t.i != (m.i1 - m.i0) / 2);
        // the tavern: two storeys, rooms upstairs, the hall with its hearth, counter and tables
        const StructurePlan t = planStructure(cat, build, *cat.find("tavern"), {5, seed});
        CHECK(t.volumes[0].storeys == 2);
        int upperFloors = 0, upperDoors = 0;
        for (const auto& p : t.pieces) {
            upperFloors += p.kind == "floor_plank" && p.z == 1;
            upperDoors += build.find(p.kind)->door && p.z == 1;
        }
        CHECK(upperFloors == (t.volumes[0].i1 - t.volumes[0].i0) * (t.volumes[0].j1 - t.volumes[0].j0));
        CHECK(upperDoors >= 2);
        int counters = 0, tables = 0;
        for (const auto& d : t.decor) counters += d.what == "counter", tables += d.what == "table_bench";
        CHECK(counters >= 1 && tables >= 2);
        CHECK(hasStation(t, "hearth") && hasStation(t, "brewing_vat"));
    }
}
