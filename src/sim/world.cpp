#include "sim/world.hpp"

#include <algorithm>
#include <stdexcept>

namespace altera::sim {

namespace {
bool laterEvent(const Event& a, const Event& b) { return a.tick != b.tick ? a.tick > b.tick : a.seq > b.seq; }
const std::string kNoBiome = "none";
} // namespace

int MapGrid::cellIndex(Vec2 p) const {
    if (w <= 0 || h <= 0) return -1;
    const int cx = std::clamp(static_cast<int>(p.x / cellM), 0, w - 1);
    const int cy = std::clamp(static_cast<int>(p.y / cellM), 0, h - 1);
    return cy * w + cx;
}
const std::string& MapGrid::biomeAt(Vec2 p) const {
    const int i = cellIndex(p);
    if (i < 0 || biome.empty()) return kNoBiome;
    return biomeNames.at(biome[static_cast<size_t>(i)]);
}
bool MapGrid::waterAt(Vec2 p) const {
    const int i = cellIndex(p);
    return i >= 0 && !water.empty() && water[static_cast<size_t>(i)] != 0;
}

World::World(const Content& content, std::uint64_t seed) : content_(&content), seed_(seed) {}

bool World::isNight() const {
    const Tick t = tick_ % kTicksPerDay;
    return t < kTicksPerDay * 6 / 24 || t >= kTicksPerDay * 21 / 24; // 21:00 - 06:00
}

const std::string& World::season() const {
    static const std::string names[4] = {"spring", "summer", "autumn", "winter"};
    const int doy = (startDay_ + day()) % kDaysPerYear;
    return names[std::min(3, doy / (kDaysPerYear / 4))];
}

bool World::inSeason(const WorldObject& o) const {
    auto d = content_->objects.find(o.type);
    if (d == content_->objects.end() || d->second.seasons.empty()) return true;
    const std::string& s = season();
    return std::find(d->second.seasons.begin(), d->second.seasons.end(), s) != d->second.seasons.end();
}

Character& World::addCharacter(Character c) {
    if (c.id == kNone) c.id = newId();
    nextId_ = std::max(nextId_, c.id + 1);
    auto [it, ok] = chars_.emplace(c.id, std::move(c));
    if (!ok) throw std::runtime_error("duplicate character id");
    return it->second;
}

WorldObject& World::addObject(WorldObject o) {
    if (o.id == kNone) o.id = newId();
    nextId_ = std::max(nextId_, o.id + 1);
    auto [it, ok] = objects_.emplace(o.id, std::move(o));
    if (!ok) throw std::runtime_error("duplicate object id");
    indexObject(it->second);
    return it->second;
}

void World::indexObject(const WorldObject& o) {
    byType_[o.type].push_back(o.id);
    auto d = content_->objects.find(o.type);
    if (d != content_->objects.end() && d->second.shelter) roofs_.push_back(o.pos);
}

const std::vector<EntityId>& World::objectsOfType(const std::string& type) const {
    static const std::vector<EntityId> none;
    auto it = byType_.find(type);
    return it == byType_.end() ? none : it->second;
}

Character* World::character(EntityId id) {
    auto it = chars_.find(id);
    return it == chars_.end() ? nullptr : &it->second;
}
const Character* World::character(EntityId id) const {
    auto it = chars_.find(id);
    return it == chars_.end() ? nullptr : &it->second;
}
WorldObject* World::object(EntityId id) {
    auto it = objects_.find(id);
    return it == objects_.end() ? nullptr : &it->second;
}
const WorldObject* World::object(EntityId id) const {
    auto it = objects_.find(id);
    return it == objects_.end() ? nullptr : &it->second;
}
const Family* World::familyOf(const Character& c) const {
    auto it = families_.find(c.status.family);
    return it == families_.end() ? nullptr : &it->second;
}

void World::schedule(Event e) {
    e.seq = nextSeq_++;
    events_.push_back(std::move(e));
    std::push_heap(events_.begin(), events_.end(), laterEvent);
}

void World::chronicle(const std::string& type, std::vector<EntityId> who, std::string code, Json params) {
    chronicle_.push_back({tick_, type, std::move(who), std::move(code), std::move(params)});
}

std::uint64_t World::submit(Command cmd) {
    cmd.seq = nextCmdSeq_++;
    if (cmd.tick < tick_) cmd.tick = tick_;
    pending_.push_back(std::move(cmd));
    return pending_.back().seq;
}

void World::replay(const std::vector<Command>& log) {
    replay_ = log;
    replayNext_ = 0;
}

Command World::applyNow(Command cmd) {
    cmd.seq = nextCmdSeq_++;
    cmd.tick = tick_;
    applyCommand(cmd);
    return cmd;
}

void World::applyCommand(Command& cmd) {
    cmd.result.clear();
    bool handled = false;
    try {
        for (auto& h : handlers_) {
            if (h(*this, cmd)) {
                handled = true;
                break;
            }
        }
    } catch (const std::exception& e) {
        cmd.result = std::string("rejected:") + e.what();
        handled = true;
    }
    if (!handled) {
        // A few world-level commands that need no higher layer.
        if (cmd.type == "set_need") {
            Character* c = character(cmd.args.at("id").get<EntityId>());
            if (c) {
                c->needs[cmd.args.at("need").get<std::string>()] = clamp01(cmd.args.at("value").get<double>());
                cmd.result = "ok";
            } else {
                cmd.result = "rejected:no such character";
            }
        } else if (cmd.type == "set_stock") {
            WorldObject* o = object(cmd.args.at("id").get<EntityId>());
            if (o) {
                o->stock[cmd.args.at("item").get<std::string>()] = std::max(0, cmd.args.at("count").get<int>());
                cmd.result = "ok";
            } else {
                cmd.result = "rejected:no such object";
            }
        } else {
            cmd.result = "rejected:unknown command";
        }
    }
    if (cmd.result.empty()) cmd.result = "ok";
    commandLog_.push_back(cmd);
}

void World::processEvents() {
    while (!events_.empty() && events_.front().tick <= tick_) {
        std::pop_heap(events_.begin(), events_.end(), laterEvent);
        Event e = std::move(events_.back());
        events_.pop_back();
        handleEvent(e);
    }
}

void World::handleEvent(const Event& e) {
    if (e.type == "regrow") {
        WorldObject* o = object(e.obj);
        if (!o) return;
        auto defIt = content_->objects.find(o->type);
        if (defIt == content_->objects.end()) return;
        const ObjectDef& d = defIt->second;
        for (const auto& [item, n] : d.regrow) {
            const int cap = d.maxStock.count(item) ? d.maxStock.at(item) : n * 8;
            o->stock[item] = std::min(cap, o->stock[item] + n);
        }
        if (d.regrowEvery > 0) schedule({tick_ + d.regrowEvery, 0, "regrow", kNone, kNone, o->id, {}});
    } else if (e.type == "release_reservation") {
        WorldObject* o = object(e.obj);
        if (o && o->reservedBy == e.a && o->reservedUntil <= tick_) o->reservedBy = kNone;
    }
}

void World::remember(Character& c, MemoryEntry m) {
    m.tick = tick_;
    if (m.salience == 0.0) {
        auto it = content_->config.memoryTypes.find(m.type);
        m.salience = it == content_->config.memoryTypes.end() ? 0.1 : it->second;
    }
    c.memory.push_back(std::move(m));
    const size_t cap = static_cast<size_t>(std::max(8, content_->config.memoryCap));
    if (c.memory.size() > cap) {
        // Drop the least salient, oldest first among equals - deterministic.
        auto victim = std::min_element(c.memory.begin(), c.memory.end(), [](const MemoryEntry& a, const MemoryEntry& b) {
            return a.salience != b.salience ? a.salience < b.salience : a.tick < b.tick;
        });
        c.memory.erase(victim);
    }
}

bool World::sheltered(const Character& c, const std::vector<Vec2>& roofs) const {
    const double range = content_->config.interactionRangeM;
    for (const Vec2& p : roofs)
        if (dist(p, c.pos) <= range) return true;
    return false;
}

void World::updateBodies() {
    const bool night = isNight();
    const std::vector<Vec2>& roofs = roofs_;
    for (auto& [id, c] : chars_) {
        if (!c.body.alive) continue;
        if (tick_ % kTicksPerDay == 0 && tick_ > 0) ++c.body.ageDays;
        if (c.isAnimal()) continue; // animals.cpp drives their hunger
        const std::string& biome = map_.biomeAt(c.pos);
        bool starving = false;
        for (const auto& n : content_->needs) {
            double rate = n.decayPerDay / static_cast<double>(kTicksPerDay);
            if (night) rate *= n.nightMult;
            auto bm = n.biomeMult.find(biome);
            if (bm != n.biomeMult.end()) rate *= bm->second;
            auto sm = n.seasonMult.find(season());
            if (sm != n.seasonMult.end()) rate *= sm->second;
            if (n.shelteredMult != 1.0 && sheltered(c, roofs)) rate *= n.shelteredMult;
            for (const auto& [item, mult] : n.clothingMult)
                if (c.has(item) > 0) rate *= mult;
            for (const auto& t : c.traits) {
                auto td = content_->traits.find(t);
                if (td == content_->traits.end()) continue;
                auto nm = td->second.needMult.find(n.id);
                if (nm != td->second.needMult.end()) rate *= nm->second;
            }
            double& v = c.needs[n.id];
            v = clamp01(v - rate);
            if (n.id == "hunger" && v <= 0.0) starving = true;
        }
        // Moodlets expire.
        c.moodlets.erase(std::remove_if(c.moodlets.begin(), c.moodlets.end(),
                                        [&](const Moodlet& m) { return m.until <= tick_; }),
                         c.moodlets.end());
        // Starvation wears the body down; rest restores fatigue elsewhere.
        if (starving) {
            c.body.health = clamp01(c.body.health - 1.0 / (kTicksPerDay * 12.0));
            ++c.ai.stats.starvingTicks;
            if (tick_ % kTicksPerDay == 0) remember(c, {0, "starved", {}, kNone, -0.6, 0.0, "seen", 1.0});
        } else if (c.body.health < 1.0 && c.need("hunger") > 0.3 && c.wounds.empty()) {
            // (a wound gives its share of health back as it heals: wounds.cpp)
            c.body.health = clamp01(c.body.health + 1.0 / (kTicksPerDay * 20.0));
        }
        if (c.body.health <= 0.0) {
            c.body.alive = false;
            c.body.causeOfDeath = "starvation";
            chronicle("death", {c.id}, "chronicle.death", {{"who", c.id}, {"cause", "starvation"}});
        }
    }
}

void World::step() {
    // 1. Commands due this tick: a replayed log first (in log order), then
    //    newly submitted ones in submission order.
    while (replayNext_ < replay_.size() && replay_[replayNext_].tick <= tick_) {
        Command c = replay_[replayNext_++];
        nextCmdSeq_ = std::max(nextCmdSeq_, c.seq + 1);
        applyCommand(c);
    }
    std::vector<Command> due;
    auto keep = pending_.begin();
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        if (it->tick <= tick_) due.push_back(std::move(*it));
        else *keep++ = std::move(*it);
    }
    pending_.erase(keep, pending_.end());
    std::sort(due.begin(), due.end(), [](const Command& a, const Command& b) { return a.seq < b.seq; });
    for (auto& c : due) applyCommand(c);
    // 2. Scheduled events.
    processEvents();
    // 3. Bodies and needs.
    updateBodies();
    // 4. Minds (Utility -> HTN -> GOAP -> FSM), installed by the AI layer.
    if (think_) think_(*this);
    ++tick_;
}

void World::run(Tick ticks) {
    for (Tick i = 0; i < ticks; ++i) step();
}

// --- serialization ----------------------------------------------------------

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Family, id, name, dynasty, settlement, members, storage, hearth)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Dynasty, id, name, founder)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Settlement, id, name, center, families)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ChronicleEntry, tick, type, who, code, params)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MapGrid, sizeM, cellM, w, h, biomeNames, biome, water)

void to_json(Json& j, const WorldObject& o) {
    j = Json{{"id", o.id},       {"type", o.type},   {"pos", o.pos},
             {"owner", o.ownerFamily}, {"stock", o.stock}, {"reserved_by", o.reservedBy},
             {"reserved_until", o.reservedUntil}};
}
void from_json(const Json& j, WorldObject& o) {
    o.id = j.at("id").get<EntityId>();
    o.type = j.at("type").get<std::string>();
    o.pos = j.at("pos").get<Vec2>();
    o.ownerFamily = j.value("owner", kNone);
    o.stock = j.value("stock", std::map<std::string, int>{});
    o.reservedBy = j.value("reserved_by", kNone);
    o.reservedUntil = j.value("reserved_until", Tick{0});
}
void to_json(Json& j, const Command& c) {
    j = Json{{"tick", c.tick}, {"seq", c.seq}, {"source", c.source}, {"type", c.type}, {"args", c.args},
             {"result", c.result}};
}
void from_json(const Json& j, Command& c) {
    c.tick = j.value("tick", Tick{0});
    c.seq = j.value("seq", std::uint64_t{0});
    c.source = j.value("source", "");
    c.type = j.at("type").get<std::string>();
    c.args = j.value("args", Json::object());
    c.result = j.value("result", "");
}
void to_json(Json& j, const Event& e) {
    j = Json{{"tick", e.tick}, {"seq", e.seq}, {"type", e.type}, {"a", e.a}, {"b", e.b}, {"obj", e.obj},
             {"data", e.data}};
}
void from_json(const Json& j, Event& e) {
    e.tick = j.at("tick").get<Tick>();
    e.seq = j.at("seq").get<std::uint64_t>();
    e.type = j.at("type").get<std::string>();
    e.a = j.value("a", kNone);
    e.b = j.value("b", kNone);
    e.obj = j.value("obj", kNone);
    e.data = j.value("data", Json());
}

Json World::toJson() const {
    std::vector<Event> ev = events_;
    std::sort(ev.begin(), ev.end(), [](const Event& a, const Event& b) { return laterEvent(b, a); });
    Json chars = Json::array();
    for (const auto& [id, c] : chars_) chars.push_back(c);
    Json objs = Json::array();
    for (const auto& [id, o] : objects_) objs.push_back(o);
    Json fams = Json::array();
    for (const auto& [id, f] : families_) fams.push_back(f);
    Json dyns = Json::array();
    for (const auto& [id, d] : dynasties_) dyns.push_back(d);
    Json sets = Json::array();
    for (const auto& [id, s] : settlements_) sets.push_back(s);
    return Json{{"format", "altera.sim.world/1"},
                {"seed", seed_},
                {"tick", tick_},
                {"start_day", startDay_},
                {"next_seq", nextSeq_},
                {"next_cmd_seq", nextCmdSeq_},
                {"next_id", nextId_},
                {"map", map_},
                {"characters", chars},
                {"objects", objs},
                {"families", fams},
                {"dynasties", dyns},
                {"settlements", sets},
                {"events", ev},
                {"pending", pending_},
                {"command_log", commandLog_},
                {"chronicle", chronicle_}};
}

void World::loadJson(const Json& j) {
    if (j.value("format", "") != "altera.sim.world/1") throw std::runtime_error("not an altera.sim.world/1 snapshot");
    seed_ = j.at("seed").get<std::uint64_t>();
    tick_ = j.at("tick").get<Tick>();
    startDay_ = j.value("start_day", 0);
    nextSeq_ = j.at("next_seq").get<std::uint64_t>();
    nextCmdSeq_ = j.value("next_cmd_seq", std::uint64_t{1});
    replay_.clear();
    replayNext_ = 0;
    nextId_ = j.at("next_id").get<EntityId>();
    map_ = j.at("map").get<MapGrid>();
    chars_.clear();
    for (const auto& cj : j.at("characters")) {
        Character c = cj.get<Character>();
        chars_.emplace(c.id, std::move(c));
    }
    objects_.clear();
    byType_.clear();
    roofs_.clear();
    for (const auto& oj : j.at("objects")) {
        WorldObject o = oj.get<WorldObject>();
        auto [it, ok] = objects_.emplace(o.id, std::move(o));
        if (ok) indexObject(it->second);
    }
    families_.clear();
    for (const auto& fj : j.at("families")) {
        Family f = fj.get<Family>();
        families_.emplace(f.id, std::move(f));
    }
    dynasties_.clear();
    for (const auto& dj : j.at("dynasties")) {
        Dynasty d = dj.get<Dynasty>();
        dynasties_.emplace(d.id, std::move(d));
    }
    settlements_.clear();
    for (const auto& sj : j.at("settlements")) {
        Settlement s = sj.get<Settlement>();
        settlements_.emplace(s.id, std::move(s));
    }
    events_ = j.at("events").get<std::vector<Event>>();
    std::make_heap(events_.begin(), events_.end(), laterEvent);
    pending_ = j.at("pending").get<std::vector<Command>>();
    commandLog_ = j.at("command_log").get<std::vector<Command>>();
    chronicle_ = j.at("chronicle").get<std::vector<ChronicleEntry>>();
}

std::uint64_t World::stateHash() const { return hashStr(toJson().dump()); }

} // namespace altera::sim
