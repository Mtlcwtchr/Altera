#pragma once
// Building from modules (content/ai/build.json, gdd/blueprints/
// ALTERA_BLUEPRINTS_SPEC_v0.2 §1-2, §4.7) and the colony's mandate (GDD §2.2).
//
// A household plans pieces on a 3D grid (cells of cell_m, storeys of
// storey_m). A planned piece is a build_site object in the world: its stock is
// what still has to be brought. People bring the materials (deliver_to_site)
// and put the piece up (construct_piece) once it is supplied and what holds it
// is standing: support is Valheim-like, stability = best support x carry, the
// ground gives 1. Everything here is deterministic and lives in World::pieces.
//
// The same placement rules serve the game's build mode (it checks a preview
// against a copy of the pieces), so the player gets nothing the sim refuses.

#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "sim/world.hpp"

namespace altera::sim {

// A slot of the grid: layer f (cell floor: foundation, floor), r (a cell's
// volume: roof), h / v (an edge along x / along y, at its lower-left grid
// point), p (a corner, a grid point); a, b grid coordinates; z storey.
struct SlotKey {
    char layer = 'f';
    int a = 0, b = 0, z = 0;
    bool operator<(const SlotKey& o) const { return std::tie(layer, a, b, z) < std::tie(o.layer, o.a, o.b, o.z); }
    bool operator==(const SlotKey& o) const { return layer == o.layer && a == o.a && b == o.b && z == o.z; }
};
using PieceMap = std::map<EntityId, BuildPiece>;

SlotKey slotOf(const BuildPieceDef& d, int x, int y, int z, int rot);
// The piece's centre on the ground plan (sim metres): cell centre, edge middle or corner.
Vec2 pieceCentre(const BuildCatalog& cat, const BuildPieceDef& d, const BuildPiece& p);
// Stability of every piece (planned and built) and of the built ones alone.
void computeStability(const BuildCatalog& cat, PieceMap& pieces);
// "" when the piece may go on the plan, else the reason: unknown_kind,
// bad_storey, foundation_ground (a footing off the ground), floor_upper (a floor on the ground), occupied, unsupported. `stability` receives what it would have.
std::string checkPlacement(const BuildCatalog& cat, const PieceMap& pieces, const BuildPiece& p,
                           double* stability = nullptr);
// The lowest storey from `from` up where the piece fits (-1: none).
int autoStorey(const BuildCatalog& cat, const PieceMap& pieces, BuildPiece p, int from = 0);
// The ground-floor height of the structure the piece would join (a piece
// that holds it or one beside it). False: it stands alone.
bool joinHeight(const BuildCatalog& cat, const PieceMap& pieces, const BuildPiece& p, double& h);
// A planned piece: what its site still lacks (item -> count).
std::map<std::string, int> missingOf(const World& w, const BuildPiece& p);

// Sim hooks.
void refreshPieces(World& w);   // derived stability, when pieces changed
double siteValue(const World& w, const Character& c, const WorldObject* site, const std::string& key);
double buildValue(const World& w, const Character& c, const std::string& key);
void siteEffect(World& w, Character& c, WorldObject& site, const std::string& key);
// Per tick: finished sites leave the world, a dead chief's mandate passes on.
void buildingTick(World& w);
// Commands: place_piece, remove_piece, grant_right, revoke_right.
void installBuilding(World& w);

// The colony (GDD §2.2): who holds the mandate of the settlement, and who
// would judge a captive now (the right to judge, else the eldest by custom).
EntityId mandateHolder(const World& w, EntityId settlement);
EntityId judgeNow(const World& w, EntityId settlement);
// The worldgen's start: the head of the eldest household becomes the chief
// with every right of config.rights ("lore start with rights", GDD §2.2).
void grantStartMandate(World& w, EntityId settlement, EntityId chief);

} // namespace altera::sim
