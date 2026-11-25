#pragma once
// World state: time, map stub, objects, characters, families, the event queue
// and the command log. Everything here is serializable; a snapshot plus the
// command log replays to the same state (CORE_SPEC §8).

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "sim/character.hpp"
#include "sim/content.hpp"
#include "sim/dialogue_rules.hpp"
#include "sim/world_fields.hpp"

namespace lr::sim {

struct WorldObject {
    EntityId id = kNone;
    std::string type;
    Vec2 pos;
    EntityId ownerFamily = kNone;
    std::map<std::string, int> stock;
    // AL-8: the way a planned building faces (radians, 0 = its front to -y, as
    // the game turns buildings); unplaced objects leave it to the view.
    double yaw = 0.0;
    bool faced = false;
    int storey = 0;   // AL-81: the storey of a building it stands on (0 = the ground)
    EntityId reservedBy = kNone;
    Tick reservedUntil = 0;
    bool reservedFor(EntityId who, Tick now) const { return reservedBy != kNone && reservedBy != who && now < reservedUntil; }
};

struct Family {
    EntityId id = kNone;
    std::string name;
    EntityId dynasty = kNone;
    EntityId settlement = kNone;
    std::vector<EntityId> members;
    EntityId storage = kNone;
    EntityId hearth = kNone;
};

struct Dynasty {
    EntityId id = kNone;
    std::string name;
    EntityId founder = kNone;
};

struct Settlement {
    EntityId id = kNone;
    std::string name;
    Vec2 center;
    std::vector<EntityId> families;
    // The colony's institution (GDD §2.2): "chief" - a head who drew people to
    // himself holds the mandate and its rights (config.rights); the rights he
    // hands on (appoint) are offices of others. kNone: nobody rules.
    std::string institution = "chief";
    EntityId chief = kNone;
    // AL-8: the planned village (village_plan.hpp): roads as polylines with
    // their widths, the square; empty for a world laid out the old way.
    std::vector<std::vector<Vec2>> roads;
    std::vector<double> roadWidth;
    Vec2 square;
    double squareR = 0.0;
    // AL-84: main | lane | out | path per road; where the main road comes in;
    // the land that is not buildings (fields, churchyard, pasture, the
    // village's wood, lots kept for new households).
    std::vector<std::string> roadKind;
    Vec2 entrance;
    struct Zone {
        std::string kind;
        Vec2 c;
        double yaw = 0.0, hw = 0.0, hd = 0.0;
    };
    std::vector<Zone> zones;
    // AL-76: the buildings the village was planned with (sim/structure_plan.hpp):
    // what each is for, whose, where its modules stand (World::pieces) and
    // what stands in it; the view draws the props.
    struct Prop {
        std::string piece;
        Vec2 pos;
        double yaw = 0.0;
    };
    // AL-81: a house the village was planned with; what it is for is its rooms' (World::rooms)
    struct Structure {
        EntityId family = kNone;
        std::string recipe;                 // AL-84: what the village planned it as
        int q = 0, gx = 0, gy = 0;          // the frame: quarters, origin cell
        int i0 = 0, j0 = 0, i1 = 0, j1 = 0; // the footprint in its own cells
        double h = 0.0;                     // the floor (the pieces' h)
        Vec2 door;                          // the main door, outside its sill
        std::vector<EntityId> objects;      // the sim's things it holds (house, hearth, forge...)
        std::vector<Prop> props;
    };
    std::vector<Structure> structures;

    // AL-92 (sim/realm.hpp): every settlement of the world is this one thing, the
    // map's village and the ones beyond it alike. What it is (size, tags) is
    // classified from its numbers and history; `mode` says only how it is
    // computed: "full" - on the map, its people act and its stores are real;
    // "stat" - beyond the player's focus, counted by the same rules.
    std::string mode = "full";
    Vec2 km;                         // on the region map; the first village of the map is at (0, 0)
    double pop = 0.0, food = 0.0, wealth = 0.0, militia = 0.0;  // full: read from the map each day
    double morale = 0.6, unrest = 0.1;
    double land = 1.0;               // what its fields give a head in a year (soil, work)
    double fields = 0.0;             // how many heads its fields feed at full yield (0 = no limit); more hands add little
    double crafts = 0.0;             // share of its people in crafts and trade, 0..1
    double fort = 0.0;               // walls and a keep, 0..1
    EntityId heir = kNone, liege = kNone, tributeTo = kNone;
    double tribute = 0.0;            // paid each month
    std::string house;               // the ruling family
    bool ruined = false;
    int founded = 0, ruinedOn = -1;
    int lastRaid = -9999, lastRaided = -9999, lastRevolt = -9999, lastRefugees = -9999, unrestDays = 0;
    std::string size;                // hamlet | village | town | city (classified)
    std::set<std::string> tags;      // raiders, outlaw, cult, fortified, farming, trade, vassal, tributary, ruined
    std::map<std::string, double> income; // the last year's income by source: fields | raids | trade | crafts
    std::map<std::string, double> peoples; // AL-114: the kinds newcomers here are drawn from (settle.json); empty = the default
    std::string culture;                   // AL-136: content/species/cultures id ("" = none named)
    bool full() const { return mode == "full"; }
    bool tagged(const std::string& t) const { return tags.count(t) > 0; }
    bool polity() const { return !size.empty(); } // the realm has read it at least once
};

// One module of a building on the household's plan or standing (build.json,
// BLUEPRINTS_SPEC §2.6). Grid cell (x, y) of cell_m, storey z of storey_m;
// rot 0..3 turns the piece to face -y, +x, +y, -x (a gable 4..7: mirrored).
// h is the height of the structure's ground floor (metres, for the view).
struct BuildPiece {
    EntityId id = kNone;
    std::string kind;
    int x = 0, y = 0, z = 0, rot = 0;
    double h = 0.0;
    bool built = false;
    int work = 0;              // construct acts done
    EntityId family = kNone;
    EntityId site = kNone;     // the build_site object while planned
    // derived (sim/building.cpp refreshPieces), not saved
    double stability = 0.0;      // counting planned pieces
    double builtStability = 0.0; // counting built pieces only: can it go up now
};

// AL-81: a room - cells of one storey closed by walls and doors (or open
// under a roof) - and what it is by what stands in it (rooms.json rules).
struct Room {
    int id = 0;
    std::string type;                         // the rule that fits, or the fallback
    int z = 0;                                // storey
    bool enclosed = false;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;       // bounds in grid cells [x0, x1) x [y0, y1)
    std::vector<std::pair<int, int>> cells;
    std::vector<EntityId> things;             // the objects the rules read that stand in it
    int building = -1;                        // index in World::roomBuildings
};
// A building: rooms on touching footings; what it is for = its highest room.
struct RoomBuilding {
    int id = 0;
    std::string purpose;                      // a room type
    std::vector<int> rooms;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;       // bounds in grid cells
};

// A coarse grid: biome per cell, water flags. A stub of the real world's
// interfaces (world -> poi/resources -> settlements, AI_ARCHITECTURE §11.1).
struct MapGrid {
    double sizeM = 12000.0;
    double cellM = 250.0;
    int w = 0, h = 0;
    std::vector<std::string> biomeNames;
    std::vector<std::uint8_t> biome;
    std::vector<std::uint8_t> water;
    int cellIndex(Vec2 p) const;
    const std::string& biomeAt(Vec2 p) const;
    bool waterAt(Vec2 p) const;
};

struct Event {
    Tick tick = 0;
    std::uint64_t seq = 0;
    std::string type;
    EntityId a = kNone;
    EntityId b = kNone;
    EntityId obj = kNone;
    Json data;
};

// An intention from outside the simulation (player, agent, scenario). The only
// way anything but the simulation itself changes the world.
struct Command {
    Tick tick = 0;
    std::uint64_t seq = 0;
    std::string source; // player:<id> | agent:<name> | scenario | system
    std::string type;
    Json args = Json::object();
    std::string result; // filled when applied: ok | rejected:<reason>
};

struct ChronicleEntry {
    Tick tick = 0;
    std::string type;
    std::vector<EntityId> who;
    std::string code;              // loc key (chronicle.<...>), text assembled at output
    Json params = Json::object();
};

// The registry of truth (DIALOGUE_GEN §6.4, AL-37): what really happened,
// kept by the world whatever anyone remembers, believes or forgets; saved and
// replayed. Characters never read it: proofs, courts and clearing a name go
// through what it points at (those who really saw it, the evidence).
struct Fact {
    EntityId id = kNone;
    Tick tick = 0;
    std::string pred;                       // what happened: a legal tag (theft, trespass, ...) for a misdeed
    std::map<std::string, EntityId> roles;  // true roles: doer, victim, victim_family, object
    Vec2 place;
    std::vector<EntityId> witnesses;        // who really saw it
    std::vector<Json> evidence;             // traces, things, confessions (filled by later work)
    std::vector<std::string> tags;
    std::map<std::string, int> goods;       // what changed hands
    std::string outcome;                    // how it was settled; empty = open
    EntityId role(const std::string& r) const {
        auto it = roles.find(r);
        return it == roles.end() ? kNone : it->second;
    }
};

class World {
public:
    explicit World(const Content& content, std::uint64_t seed = 1);

    const Content& content() const { return *content_; }
    std::uint64_t seed() const { return seed_; }
    Tick now() const { return tick_; }
    int day() const { return static_cast<int>(tick_ / kTicksPerDay); }
    bool isNight() const;
    // Seasons: 90 days each from `startDay` (day of year the world starts on).
    // spring | summer | autumn | winter.
    const std::string& season() const;
    void setStartDay(int dayOfYear) { startDay_ = ((dayOfYear % kDaysPerYear) + kDaysPerYear) % kDaysPerYear; }
    int startDay() const { return startDay_; }
    // Out-of-season objects (objects.json `season`) give nothing and are not
    // offered to planners.
    bool inSeason(const WorldObject& o) const;

    // --- entities -------------------------------------------------------
    EntityId newId() { return nextId_++; }
    Character& addCharacter(Character c);
    WorldObject& addObject(WorldObject o);
    Character* character(EntityId id);
    const Character* character(EntityId id) const;
    WorldObject* object(EntityId id);
    const WorldObject* object(EntityId id) const;
    std::map<EntityId, Character>& characters() { return chars_; }
    const std::map<EntityId, Character>& characters() const { return chars_; }
    // AL-92: the people beyond the player's focus - the same Characters, counted by
    // the realm (sim/realm.cpp) instead of acting on the map. Moving between the two
    // keeps the object where it is (node handles), so pointers stay good.
    std::map<EntityId, Character>& offstage() { return offstage_; }
    const std::map<EntityId, Character>& offstage() const { return offstage_; }
    Character* person(EntityId id);              // on the map or off it
    const Character* person(EntityId id) const;
    Character* toOffstage(EntityId id);           // null: not on the map
    Character* toStage(EntityId id);              // null: not offstage
    std::map<EntityId, WorldObject>& objects() { return objects_; }
    const std::map<EntityId, WorldObject>& objects() const { return objects_; }
    // Ids of every object of a type (index kept by addObject / loadJson).
    const std::vector<EntityId>& objectsOfType(const std::string& type) const;
    // Roof positions (objects.json shelter), kept with the index.
    const std::vector<Vec2>& roofs() const { return roofs_; }
    std::map<EntityId, Family>& families() { return families_; }
    const std::map<EntityId, Family>& families() const { return families_; }
    std::map<EntityId, Dynasty>& dynasties() { return dynasties_; }
    const std::map<EntityId, Dynasty>& dynasties() const { return dynasties_; }
    std::map<EntityId, Settlement>& settlements() { return settlements_; }
    const std::map<EntityId, Settlement>& settlements() const { return settlements_; }
    std::map<EntityId, BuildPiece>& pieces() { return pieces_; }
    const std::map<EntityId, BuildPiece>& pieces() const { return pieces_; }
    bool piecesDirty = true; // set when pieces change; building.cpp refreshes derived state
    // AL-81: the rooms the built pieces close (sim/rooms.hpp), derived, not saved
    std::vector<Room>& rooms() { return rooms_; }
    const std::vector<Room>& rooms() const { return rooms_; }
    std::vector<RoomBuilding>& roomBuildings() { return roomBuildings_; }
    const std::vector<RoomBuilding>& roomBuildings() const { return roomBuildings_; }
    // LR-140 (need_tiers.cpp): who finished how many builds, counted from the
    // chronicle as it grows (not saved: worked out again after a load).
    struct NeedCache {
        std::size_t scanned = 0;
        std::map<EntityId, int> built;
    } needCache;
    bool roomsDirty = true;          // set when pieces or the things in rooms change
    std::uint64_t roomsSig = 0;      // what the rooms were read from last
    bool removeObject(EntityId id);
    // AL-78: many at once (the game's forest replaces the generated trees): one pass, not one a thing
    std::size_t removeObjects(const std::vector<EntityId>& ids);
    MapGrid& map() { return map_; }
    const MapGrid& map() const { return map_; }
    // AL-115: the fields of the map's cells, fire and the integrity of things (sim/world_fields.hpp)
    WorldFields& fields() { return fields_; }
    const WorldFields& fields() const { return fields_; }
    const Family* familyOf(const Character& c) const;

    // --- events ---------------------------------------------------------
    void schedule(Event e); // seq assigned here
    size_t pendingEvents() const { return events_.size(); }
    void chronicle(const std::string& type, std::vector<EntityId> who, std::string code, Json params = Json::object());
    const std::vector<ChronicleEntry>& chronicleLog() const { return chronicle_; }
    // --- the registry of truth (Fact) ----------------------------------
    Fact& addFact(Fact f); // id and tick assigned here
    std::vector<Fact>& facts() { return facts_; }
    const std::vector<Fact>& facts() const { return facts_; }
    // What was said, for cooldown / once_per of dialogue rules (AL-53); saved.
    SaidMemory& said() { return said_; }
    const SaidMemory& said() const { return said_; }
    // Saved state of a higher layer by key (AL-85: "realm"); null JSON until written.
    Json& moduleState(const std::string& key) { return modules_[key]; }
    const Json* moduleState(const std::string& key) const {
        auto it = modules_.find(key);
        return it == modules_.end() ? nullptr : &it->second;
    }

    // --- commands -------------------------------------------------------
    // Queued for the start of the next step (or `cmd.tick` if later). Returns
    // the assigned sequence number.
    std::uint64_t submit(Command cmd);
    const std::vector<Command>& commandLog() const { return commandLog_; }
    // Applied immediately (inside the current tick), and logged. Used by the
    // in-process agent API so a reply can carry the result.
    Command applyNow(Command cmd);
    // Deterministic replay: commands from a log are re-applied at their ticks,
    // in log order, keeping their sequence numbers.
    void replay(const std::vector<Command>& log);

    // Hooks set by higher layers (AI, controllers). The world core knows only
    // their contract. Commands unknown to every handler are rejected.
    using CommandHandler = std::function<bool(World&, Command&)>;
    void addCommandHandler(CommandHandler h) { handlers_.push_back(std::move(h)); }
    // Event types the core does not know go to these (true = handled).
    using EventHandler = std::function<bool(World&, const Event&)>;
    void addEventHandler(EventHandler h) { eventHandlers_.push_back(std::move(h)); }
    using TickHook = std::function<void(World&)>;
    void setThinkHook(TickHook h) { think_ = std::move(h); }

    // --- time -----------------------------------------------------------
    void step();
    void run(Tick ticks);

    // --- persistence ----------------------------------------------------
    Json toJson() const;
    void loadJson(const Json& j); // replaces all state; content stays
    std::uint64_t stateHash() const;

    // Personal randomness, independent of iteration order.
    Rng rng(EntityId who, std::string_view salt) const { return agentRng(seed_, who, tick_, salt); }

    // Memory helper (caps the list, keeps the most salient).
    void remember(Character& c, MemoryEntry m);

private:
    void applyCommand(Command& cmd);
    void processEvents();
    void handleEvent(const Event& e);
    void updateBodies();
    bool sheltered(const Character& c, const std::vector<Vec2>& roofs) const;

    const Content* content_;
    std::uint64_t seed_;
    Tick tick_ = 0;
    int startDay_ = 0;
    std::uint64_t nextSeq_ = 1;    // events
    std::uint64_t nextCmdSeq_ = 1; // commands (own counter: replay must not depend on event traffic)
    EntityId nextId_ = 1;
    MapGrid map_;
    WorldFields fields_;
    std::map<EntityId, Character> chars_;
    std::map<EntityId, WorldObject> objects_;
    std::map<std::string, std::vector<EntityId>> byType_;
    std::vector<Vec2> roofs_;
    void indexObject(const WorldObject& o);
    std::map<EntityId, Family> families_;
    std::map<EntityId, Dynasty> dynasties_;
    std::map<EntityId, Settlement> settlements_;
    std::map<EntityId, BuildPiece> pieces_;
    std::vector<Room> rooms_;
    std::vector<RoomBuilding> roomBuildings_;
    std::vector<Event> events_; // min-heap on (tick, seq)
    std::vector<Command> pending_;
    std::vector<Command> replay_;
    size_t replayNext_ = 0;
    std::vector<Command> commandLog_;
    std::vector<ChronicleEntry> chronicle_;
    std::vector<Fact> facts_;
    SaidMemory said_;
    std::map<std::string, Json> modules_;
    std::map<EntityId, Character> offstage_;
    std::vector<CommandHandler> handlers_;
    std::vector<EventHandler> eventHandlers_;
    TickHook think_;
};

// Serialization of the plain structures.
void to_json(Json& j, const WorldObject& o);
void from_json(const Json& j, WorldObject& o);
void to_json(Json& j, const Command& c);
void from_json(const Json& j, Command& c);
void to_json(Json& j, const Event& e);
void from_json(const Json& j, Event& e);
void to_json(Json& j, const BuildPiece& p);
void from_json(const Json& j, BuildPiece& p);

} // namespace lr::sim
