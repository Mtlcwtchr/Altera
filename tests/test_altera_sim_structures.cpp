// AL-76 / AL-81: village houses put together from rooms, from modules (sim/structure_plan, sim/rooms).
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/building.hpp"
#include "sim/rooms.hpp"
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
    static const StructureCatalog c = parseStructures(realContent().roomsJson);
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

namespace {
// The rooms a plan makes, read back by the rules from its modules and what stands in it.
RoomsResult roomsOf(const StructurePlan& s, const StructureFrame& f) {
    std::vector<RoomThing> things;
    EntityId id = 1000;
    for (const auto* list : {&s.stations, &s.decor, &s.beds})
        for (const auto& t : *list)
            if (!t.yard) things.push_back({id++, t.what, f.point(t.u, t.v), t.z});
    return detectRooms(realContent().build, realContent().rooms, piecesOf(s, f), things);
}
// The type read for the room holding a part's first cell.
std::string readAs(const RoomsResult& r, const StructurePart& p, const StructureFrame& f) {
    int x, y;
    f.cell(p.i0, p.j0, x, y);
    for (const auto& room : r.rooms)
        if (room.z == p.z)
            for (const auto& c : room.cells)
                if (c.first == x && c.second == y) return room.type;
    return "-";
}
}  // namespace

TEST(altera_sim_structures_smith) {
    const StructureCatalog& cat = catalog();
    CHECK(cat.loaded());
    const StructureTypeDef* smith = cat.find("smith");
    CHECK(smith != nullptr);
    if (!smith) return;
    for (std::uint64_t seed = 1; seed <= 12; ++seed) {
        const StructurePlan s = planStructure(cat, realContent().build, *smith, {5, seed});
        // a home with an open canopy beside it: the forge, the anvil, the tub and the grindstone under it
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
        CHECK(int(s.beds.size()) == 5);
        int doors = 0, posts = 0;
        for (const auto& p : s.pieces) {
            const BuildPieceDef* d = realContent().build.find(p.kind);
            CHECK(d != nullptr);
            if (!d) continue;
            doors += d->door;
            posts += d->category == "pillar" && p.i >= canopy->i0 - 1 && p.i <= canopy->i1 && p.j >= canopy->j0 - 1 && p.j <= canopy->j1;
        }
        CHECK(doors >= 2);   // the front door and the one into the canopy
        CHECK(posts >= 4);
        const auto bad = checkStructure(realContent().build, s);
        report("smith", bad);
        CHECK(bad.empty());
        // read back from what stands in it: a home and a smithy (open, under its roof), one building
        const StructureFrame f{int(seed % 4), 3, 9, realContent().build.cellM};
        const RoomsResult r = roomsOf(s, f);
        std::set<std::string> types;
        for (const auto& room : r.rooms) types.insert(room.type);
        CHECK(types.count("home") && types.count("smithy"));
        CHECK(r.buildings.size() == 1);
        if (!r.buildings.empty()) CHECK(r.buildings[0].purpose == "smithy");
        for (const auto& room : r.rooms)
            if (room.type == "smithy") CHECK(!room.enclosed);
    }
}

TEST(altera_sim_structures_every_recipe) {
    const StructureCatalog& cat = catalog();
    const BuildCatalog& build = realContent().build;
    CHECK(cat.types.size() >= 8);
    std::set<std::string> annexKinds, roomsMade;
    for (const auto& type : cat.types)
        for (std::uint64_t seed = 1; seed <= 10; ++seed) {
            const StructurePlan s = planStructure(cat, build, type, {seed % 2 ? 4 : 7, seed * 7919});
            const auto bad = checkStructure(build, s);
            report(type.id + " seed " + std::to_string(seed), bad);
            CHECK(bad.empty());
            for (const auto& n : s.notes) std::printf("    %s seed %d note: %s\n", type.id.c_str(), int(seed), n.c_str());
            CHECK(s.notes.empty());
            for (const auto& v : s.volumes) annexKinds.insert(v.role);
            // every room it was put together from reads back as that room
            const StructureFrame f0{int(seed % 4), -4, 11, build.cellM};
            const RoomsResult rr = roomsOf(s, f0);
            for (const auto& p : s.parts) {
                if (p.room.empty()) continue;
                const std::string got = readAs(rr, p, f0);
                if (got != p.room)
                    std::printf("    %s seed %d: the %s (%s, z %d) reads as %s\n", type.id.c_str(), int(seed), p.room.c_str(),
                                p.at.c_str(), p.z, got.c_str());
                CHECK(got == p.room);
                roomsMade.insert(p.room);
            }
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
            // the same seed, the same house
            const StructurePlan again = planStructure(cat, build, type, {seed % 2 ? 4 : 7, seed * 7919});
            CHECK(again.pieces.size() == s.pieces.size());
        }
    for (const char* k : {"main", "canopy", "shed", "porch"}) CHECK(annexKinds.count(k));
    for (const char* k : {"home", "smithy", "tavern_hall", "kitchen", "bedroom", "bakery", "chapel", "granary", "byre", "storeroom",
                          "brewery", "weaving", "tannery", "pottery", "carpentry"})
        CHECK(roomsMade.count(k));
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
        // no building types: the rooms are read from the world, the buildings' purposes from them
        std::map<std::string, int> rooms, purposes;
        for (const auto& r : w.rooms()) ++rooms[r.type];
        for (const auto& b : w.roomBuildings()) ++purposes[b.purpose];
        std::printf("    families %d rooms:", families);
        for (const auto& [t, n] : rooms) std::printf(" %s %d", t.c_str(), n);
        for (const auto& r : w.rooms())
            if (r.type == "room") {   // what a bare room holds (a room nothing furnished)
                std::printf("\n    (a bare room: z %d, %d cells, %s:", r.z, int(r.cells.size()), r.enclosed ? "closed" : "open");
                for (EntityId id : r.things) std::printf(" %s", w.object(id) ? w.object(id)->type.c_str() : "?");
                std::printf(")");
                const double cx = (r.cells[0].first + 0.5) * 2.0, cy = (r.cells[0].second + 0.5) * 2.0;
                for (const auto& [id, o] : w.objects())
                    if (dist(o.pos, {cx, cy}) < 3.0) std::printf(" [near: %s %.1f]", o.type.c_str(), dist(o.pos, {cx, cy}));
            }
        std::printf("\n    buildings:");
        for (const auto& [t, n] : purposes) std::printf(" %s %d", t.c_str(), n);
        std::printf("\n");
        CHECK(int(w.roomBuildings().size()) == int(st.structures.size()));
        CHECK(rooms.count("home") && rooms.count("smithy") && rooms.count("chapel"));
        CHECK(purposes.count("smithy") == 1 && purposes.count("chapel") == 1);
        if (families >= 3) CHECK(rooms.count("tavern_hall") && rooms.count("kitchen") && rooms.count("bedroom"));
        CHECK(purposes.size() >= (families >= 7 ? 7u : 4u));
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
        // the smithy room's forge and anvil belong to the smith's household, whose head knows smithing
        for (const auto& r : w.rooms()) {
            if (r.type != "smithy") continue;
            EntityId fam = kNone;
            for (EntityId id : r.things)
                if (const WorldObject* o = w.object(id)) {
                    if (o->type == "forge") fam = o->ownerFamily;
                    CHECK(o->faced);
                }
            CHECK(fam != kNone);
            bool smith = false;
            for (const auto& [id, ch] : w.characters())
                if (ch.status.family == fam && ch.status.role == "head") smith = ch.knowledge.count("smithing") > 0;
            CHECK(smith);
        }
        // the upper storey's beds: their bedrooms
        for (const auto& [id, o] : w.objects())
            if (o.type == "bed" && o.storey > 0) {
                const Room* r = roomAt(w, o.pos, o.storey);
                CHECK(r && r->type == "bedroom");
            }
        // the zones: the tavern and the chapel by the square, the granary out at the edge
        auto farOut = [&](const std::string& purpose) {
            for (const auto& b : w.roomBuildings())
                if (b.purpose == purpose) return dist({(b.x0 + b.x1) * 0.5 * c, (b.y0 + b.y1) * 0.5 * c}, st.square);
            return -1.0;
        };
        if (purposes.count("granary") && purposes.count("tavern_hall")) CHECK(farOut("granary") > farOut("tavern_hall"));
        if (purposes.count("granary") && purposes.count("chapel")) CHECK(farOut("granary") > farOut("chapel"));
        // saved and loaded, the houses stay and the rooms read the same
        World again = makeTestWorld(realContent(), cfg, 7 + families);
        again.loadJson(w.toJson());
        refreshRooms(again);
        const Settlement& back = again.settlements().begin()->second;
        CHECK(back.structures.size() == st.structures.size());
        CHECK(again.rooms().size() == w.rooms().size());
        if (!back.structures.empty()) {
            CHECK(back.structures[0].objects == st.structures[0].objects);
            CHECK(back.structures[0].gx == st.structures[0].gx);
        }
    }
}

TEST(altera_sim_structures_chapel_inn) {
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
        // the inn: two storeys, bedrooms upstairs, the hall with its counter and tables, a kitchen behind a partition
        const StructurePlan t = planStructure(cat, build, *cat.find("innkeeper"), {5, seed});
        CHECK(t.volumes[0].storeys == 2);
        int upperFloors = 0, upperDoors = 0, upperBeds = 0;
        for (const auto& p : t.pieces) {
            upperFloors += p.kind == "floor_plank" && p.z == 1;
            upperDoors += build.find(p.kind)->door && p.z == 1;
        }
        for (const auto& b : t.beds) upperBeds += b.z == 1;
        CHECK(upperFloors == (t.volumes[0].i1 - t.volumes[0].i0) * (t.volumes[0].j1 - t.volumes[0].j0));
        CHECK(upperDoors >= 2);
        CHECK(upperBeds == 5);
        int counters = 0, tables = 0;
        for (const auto& d : t.decor) counters += d.what == "counter", tables += d.what == "table_bench";
        CHECK(counters >= 1 && tables >= 2);
        CHECK(hasStation(t, "brewing_vat") && hasStation(t, "cook_pot"));
    }
}
