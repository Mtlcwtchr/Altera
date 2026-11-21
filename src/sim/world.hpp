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

namespace altera::sim {

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
    std::map<EntityId, WorldObject>& objects() { return objects_; }
    const std::map<EntityId, WorldObject>& objects() const { return objects_; }
    // Ids of every object of a type (index kept by addObject / loadJson).
    const std::vector<EntityId>& objectsOfType(const std::string& type) const;
    // Roof positions (objects.json shelter), kept with the index.
    const std::vector<Vec2>& roofs() const { return roofs_; }
    std::map<EntityId, Family>& families() { return families_; }
    const std::map<EntityId, Family>& families() const { return families_; }
    std::map<EntityId, Dynasty>& dynasties() { return dynasties_; }
    std::map<EntityId, Settlement>& settlements() { return settlements_; }
    const std::map<EntityId, Settlement>& settlements() const { return settlements_; }
    std::map<EntityId, BuildPiece>& pieces() { return pieces_; }
    const std::map<EntityId, BuildPiece>& pieces() const { return pieces_; }
    bool piecesDirty = true; // set when pieces change; building.cpp refreshes derived state
    bool removeObject(EntityId id);
    MapGrid& map() { return map_; }
    const MapGrid& map() const { return map_; }
    const Family* familyOf(const Character& c) const;

    // --- events ---------------------------------------------------------
    void schedule(Event e); // seq assigned here
    size_t pendingEvents() const { return events_.size(); }
    void chronicle(const std::string& type, std::vector<EntityId> who, std::string code, Json params = Json::object());
    const std::vector<ChronicleEntry>& chronicleLog() const { return chronicle_; }

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
    std::map<EntityId, Character> chars_;
    std::map<EntityId, WorldObject> objects_;
    std::map<std::string, std::vector<EntityId>> byType_;
    std::vector<Vec2> roofs_;
    void indexObject(const WorldObject& o);
    std::map<EntityId, Family> families_;
    std::map<EntityId, Dynasty> dynasties_;
    std::map<EntityId, Settlement> settlements_;
    std::map<EntityId, BuildPiece> pieces_;
    std::vector<Event> events_; // min-heap on (tick, seq)
    std::vector<Command> pending_;
    std::vector<Command> replay_;
    size_t replayNext_ = 0;
    std::vector<Command> commandLog_;
    std::vector<ChronicleEntry> chronicle_;
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

} // namespace altera::sim
