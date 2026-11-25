#pragma once
// AL-115 (LIVELYREALMS_SETTING_v0.2 §6.1-6.2, M2): the fields of the world's
// cells. Every 1x1 m cell of the map (one 16x16 tile of the 2D client) carries
// seven bytes - heat, moisture, fertility, integrity, light, aether, taint -
// that magic, fire, rain and time write and the other systems read. Nobody
// special-cases a spell: an aspect's world_channels (content/magic/aspects.json)
// say what it writes, and fire, rot, growth and the beasts read the numbers.
//
// The map is large and nearly all of it is at rest, so the cells are stored in
// 32x32 chunks made only where something wrote; a chunk eases back to the
// cell's baseline (the biome, the season, today's weather) and is dropped when
// nothing in it differs any more. Only chunks with something going on are
// stepped each tick (diffusion, relaxation, fire); scarred ones (a burnt wood
// growing back) are stepped once a day.
//
// Fire (§6.2): a cell burns while it has fuel (grass, undergrowth by biome) and
// is hot and dry enough; it heats its neighbours (more downwind) and the
// things standing on it. Things (WorldObject, built BuildPiece) have a material
// and an integrity (content/world/fields.json): fire and the integrity channel
// wear it down; at 0 the thing is gone and leaves debris (deadwood, rubble)
// that people gather like any other, and a building's unsupported pieces fall.
//
// In the statistical zone beyond the map nothing here runs: what burns there
// is counted by the realm (sim/realm.cpp) in shares, by the same numbers.
//
// Units: a field byte is 0..255. heat: zero_c + c_per_step * byte (degrees C);
// light: an addition to what the 2D client draws for the sun (0 = none);
// integrity: 255 at rest, lower = damage pressing on what stands there this
// tick (the integrity of a thing itself lives on the thing).

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "sim/core.hpp"

namespace altera::sim {

class World;
struct WorldObject;

enum class Field : std::uint8_t { heat, moisture, fertility, integrity, light, aether, taint, count };
constexpr int kFieldCount = static_cast<int>(Field::count);
const char* fieldName(Field f);
bool fieldFromName(const std::string& id, Field& out);

// --- content/world/fields.json ------------------------------------------------
struct FieldRule {
    double unit = 20.0;        // bytes per unit of a channel expression ("+2*power" = 2 units)
    double diffuse = 0.0;      // share of the difference to the 4 neighbours' mean taken each tick
    double relax = 0.0;        // share of the difference to the baseline given back each tick
    double relaxPerDay = 0.0;  // bytes a day back toward the baseline (slow fields: fertility, taint)
    int base = 0;              // baseline where nothing else says
};
struct BiomeFields {
    int moisture = 90, fertility = 120, fuel = 120, aether = 0;
};
struct FieldMaterial {
    std::string id;
    double flammable = 0.0;    // 0 = does not burn; the share of a burning cell's heat it catches
    int integrity = 100;       // full
    int burnPerTick = 4;       // integrity a burning thing of it loses a tick
    int fireHeat = 200;        // heat byte it burns at
    std::string debrisType;    // the object it leaves (deadwood, rubble); empty = nothing
    std::map<std::string, int> debris;       // stock of what it leaves when broken
    std::map<std::string, int> burntDebris;  // ... and when burnt down
    double rustable = 0.0;     // takes the "rust" channel (metal)
};
struct ThingRule {
    std::string material;
    int integrity = -1;        // -1: the material's
    double radiusM = 0.5;      // the cells it stands on
    bool keepsStockAsDebris = false; // a tree: its logs stay as its debris
};
struct FireRule {
    int igniteHeat = 80;        // a cell with fuel catches at this heat ...
    int wetStops = 170;         // ... unless its moisture is at least this
    int fuelPerTick = 6;        // what a burning cell eats a tick at full flame
    int grow = 50;              // flame added a tick while there is fuel
    int flameHeat = 210;        // heat a full flame holds its cell at
    double spread = 0.22;       // share of the flame's heat pushed to each neighbour
    double windCarry = 0.08;    // extra share downwind per m/s of wind
    int light = 180;            // light of a full flame
    int dryPerTick = 12;        // moisture a flame drives out of its cell
    int ashFertility = 35;      // fertility a burnt-out cell gains (then it relaxes back)
    int fuelRegrowPerDay = 2;   // a burnt cell's fuel comes back this fast
    double burnHealth = 0.06;   // health a full flame takes a tick from someone standing in it
    double rainPerMm = 20.0;    // flame a mm of the day's rain puts out each tick of that day
    int smokeMin = 40;          // flame from which a cell smokes (2D fx)
};
struct FieldsConfig {
    int chunk = 32;
    double zeroC = -40.0, cPerStep = 4.0;
    std::array<FieldRule, kFieldCount> rules{};
    std::map<std::string, BiomeFields> biomes;
    BiomeFields biomeDefault;
    int waterMoisture = 255;
    double rainMoisturePerMm = 6.0;     // moisture a mm of the day's rain adds to the baseline
    FireRule fire;
    std::map<std::string, FieldMaterial> materials;
    std::map<std::string, std::string> pieceMaterials;   // build.json material -> materials id
    std::map<std::string, ThingRule> things;             // object type -> rule
    ThingRule thingDefault;                               // objects not listed (empty material = untouched)
    std::vector<std::string> untouched;                   // object types fire and damage leave alone
    static FieldsConfig defaults();
    static FieldsConfig fromJson(const Json& j, std::vector<std::string>* warnings = nullptr);
    int heatOfC(double c) const;
    double cOfHeat(int b) const { return zeroC + cPerStep * b; }
    const FieldMaterial* materialOfObject(const std::string& type, ThingRule* rule = nullptr) const;
    const FieldMaterial* materialOfPiece(const std::string& buildMaterial) const;
};
// The rules of a world (read once per Content from <content>/../world/fields.json).
const FieldsConfig& fieldsConfig(const World& w);

// --- the state ------------------------------------------------------------------
struct FieldChunk {
    int cx = 0, cy = 0;
    std::array<std::vector<std::uint8_t>, kFieldCount> v;   // chunk*chunk each
    std::vector<std::uint8_t> fuel, fire, burnt;            // what can burn; the flame; burnt-out marks
    bool active = true;     // stepped every tick; false: scarred only, stepped daily
    int quiet = 0;          // ticks with nothing to do
};
// The objects / pieces burning now (a thing's flame lives with the fields, not the thing).
struct ThingFire {
    int flame = 0;
};

struct WorldFields {
    std::map<std::int64_t, FieldChunk> chunks;
    std::map<EntityId, ThingFire> burningObjects, burningPieces;
    // the thing integrity that is not full (absent = full), objects and pieces
    std::map<EntityId, int> objectIntegrity, pieceIntegrity;
    // AL-115 M4: local weather pushed by magic, per settlement: rain (mm/day) and sun (heat bytes), easing off daily
    struct WeatherPush {
        double rainMm = 0.0, heat = 0.0, wind = 0.0;
    };
    std::map<EntityId, WeatherPush> weather;
    // derived, not saved
    int ambientDay = -1;
    int ambientHeat = 14;
    double rainMmToday = 0.0, windFromDeg = 270.0, windMs = 3.0;
    std::uint64_t indexSig = 0;
    std::map<std::int64_t, std::vector<EntityId>> objectsByChunk, piecesByChunk;

    bool empty() const { return chunks.empty() && burningObjects.empty() && burningPieces.empty(); }
    Json toJson() const;
    void fromJson(const Json& j);
};

// --- reading -----------------------------------------------------------------------
// The value of a field at a point (its baseline where nothing wrote).
int fieldByte(const World& w, Field f, Vec2 at);
// The same as 0..1 (heat in degrees C via heatC).
double fieldAt(const World& w, Field f, Vec2 at);
double heatC(const World& w, Vec2 at);
int fireAt(const World& w, Vec2 at);          // the flame of the cell 0..255
bool burnable(const World& w, Vec2 at);       // has fuel and is not too wet
// What stands on a cell at its weakest: 0..1 of full (1 = nothing damaged or nothing there).
double integrityAt(const World& w, Vec2 at);
int objectIntegrity(const World& w, const WorldObject& o);       // -1: indestructible
int objectIntegrityMax(const World& w, const WorldObject& o);

// --- writing -----------------------------------------------------------------------
// Adds `amount` bytes to a field in a disc (full at the centre, half at the rim).
// Integrity takes damage (amount < 0) or mends (amount > 0) what stands there.
void fieldAdd(World& w, Field f, Vec2 at, double radiusM, double amount);
// A magic aspect's channels for one vector (aspects.json world_channels["+"|"-"|"~"]):
// {"heat": "+2*power", ...} in units of FieldRule::unit; special keys: transfer
// (draws the named field from the ring round the disc into it), rain/sun/wind/storm
// (the settlement's local weather), rust (metal things), growth (crops, M4).
// Keys it does not know are returned (raise_ground, shape_*, push, lure: other systems').
std::vector<std::string> applyChannels(World& w, Vec2 at, double radiusM, const Json& channels, double power);
// Sets a cell burning (a torch, a lightning strike, a spell).
bool ignite(World& w, Vec2 at, int flame = 200);
// Damage to one thing (an axe, a ram): integrity points; true if it broke.
bool damageObject(World& w, EntityId obj, int dmg, const std::string& cause);
bool damagePiece(World& w, EntityId piece, int dmg, const std::string& cause);
// Evaluates "+2*power" / "-0.3*power" / "power" / "1.5".
double channelValue(const Json& expr, double power);

// --- time -----------------------------------------------------------------------------
void fieldsTick(World& w);    // every tick (installAi's think hook)
void installFields(World& w); // commands: field_add, ignite, damage, apply_channels

// --- the view (2D client) -----------------------------------------------------------
// Point sources for the 2D renderer round `near` (r metres; r <= 0: all):
// {"lights": [{x,y,r,rgb,i,flicker}], "fx": [{x,y,kind,strength}], "fires": n}
Json fieldsViewJson(const World& w, const Vec2* near, double r);
// One field as a raster of bytes for an overlay: {x,y,w,h,field,data:[...]}.
Json fieldRasterJson(const World& w, Field f, int x0, int y0, int wCells, int hCells);

} // namespace altera::sim
