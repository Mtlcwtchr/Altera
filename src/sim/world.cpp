#include "sim/world.hpp"
#include "sim/memory_budget.hpp"

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

bool World::removeObject(EntityId id) {
    auto it = objects_.find(id);
    if (it == objects_.end()) return false;
    auto& list = byType_[it->second.type];
    list.erase(std::remove(list.begin(), list.end(), id), list.end());
    objects_.erase(it);
    roofs_.clear();
    for (const auto& [oid, o] : objects_) {
        auto d = content_->objects.find(o.type);
        if (d != content_->objects.end() && d->second.shelter) roofs_.push_back(o.pos);
    }
    return true;
}

std::size_t World::removeObjects(const std::vector<EntityId>& ids) {
    std::size_t n = 0;
    for (EntityId id : ids) n += objects_.erase(id);
    byType_.clear();
    roofs_.clear();
    for (const auto& [oid, o] : objects_) indexObject(o);
    return n;
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
Character* World::person(EntityId id) {
    if (Character* c = character(id)) return c;
    auto it = offstage_.find(id);
    return it == offstage_.end() ? nullptr : &it->second;
}
const Character* World::person(EntityId id) const {
    if (const Character* c = character(id)) return c;
    auto it = offstage_.find(id);
    return it == offstage_.end() ? nullptr : &it->second;
}
Character* World::toOffstage(EntityId id) {
    auto node = chars_.extract(id);
    if (node.empty()) return nullptr;
    return &offstage_.insert(std::move(node)).position->second;
}
Character* World::toStage(EntityId id) {
    auto node = offstage_.extract(id);
    if (node.empty()) return nullptr;
    return &chars_.insert(std::move(node)).position->second;
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
    } else {
        for (const auto& h : eventHandlers_)
            if (h(*this, e)) break;
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
        // AL-114: the kind's body (a troll turns to stone in the sun: its needs wait)
        if (const RaceDef* rd = content_->raceOf(c.race); rd && (rd->sunSleep.on || rd->regen.healthPerDay > 0.0))
            if (!raceBodyTick(*this, c, rd->sunSleep.on && sheltered(c, roofs))) continue;
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
            if (!c.race.empty() && raceAppliesTo(c)) rate *= raceNeedMult(*content_, c, n.id); // AL-114
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
void to_json(Json& j, const Settlement& s) {
    j = Json{{"id", s.id}, {"name", s.name}, {"center", s.center}, {"families", s.families},
             {"institution", s.institution}, {"chief", s.chief}};
    if (!s.roads.empty()) {   // AL-8: only planned villages carry these
        j["roads"] = s.roads;
        j["road_width"] = s.roadWidth;
        j["square"] = s.square;
        j["square_r"] = s.squareR;
        j["road_kind"] = s.roadKind;   // AL-84
        j["entrance"] = s.entrance;
        Json zs = Json::array();
        for (const auto& z : s.zones) zs.push_back(Json{{"kind", z.kind}, {"rect", {z.c.x, z.c.y, z.yaw, z.hw, z.hd}}});
        j["zones"] = zs;
    }
    if (s.polity() || !s.full()) {   // AL-92: the realm's numbers
        j["polity"] = Json{{"mode", s.mode}, {"km", s.km}, {"pop", s.pop}, {"food", s.food}, {"wealth", s.wealth},
                           {"militia", s.militia}, {"morale", s.morale}, {"unrest", s.unrest}, {"land", s.land}, {"fields", s.fields},
                           {"crafts", s.crafts}, {"fort", s.fort}, {"heir", s.heir}, {"liege", s.liege},
                           {"tribute_to", s.tributeTo}, {"tribute", s.tribute}, {"house", s.house}, {"ruined", s.ruined},
                           {"founded", s.founded}, {"ruined_on", s.ruinedOn}, {"last_raid", s.lastRaid},
                           {"last_raided", s.lastRaided}, {"last_revolt", s.lastRevolt}, {"last_refugees", s.lastRefugees},
                           {"unrest_days", s.unrestDays}, {"size", s.size}, {"tags", s.tags}, {"income", s.income}};
    }
    if (!s.structures.empty()) {   // AL-76
        Json a = Json::array();
        for (const auto& t : s.structures) {
            Json props = Json::array();
            for (const auto& p : t.props) props.push_back(Json{{"piece", p.piece}, {"pos", p.pos}, {"yaw", p.yaw}});
            a.push_back(Json{{"family", t.family}, {"recipe", t.recipe}, {"frame", {t.q, t.gx, t.gy}},
                             {"cells", {t.i0, t.j0, t.i1, t.j1}}, {"h", t.h}, {"door", t.door},
                             {"objects", t.objects}, {"props", props}});
        }
        j["structures"] = a;
    }
    if (!s.peoples.empty()) j["peoples"] = s.peoples;
}
void from_json(const Json& j, Settlement& s) {
    const Settlement d;
    s.id = j.value("id", d.id);
    s.name = j.value("name", d.name);
    s.center = j.value("center", d.center);
    s.families = j.value("families", d.families);
    s.institution = j.value("institution", d.institution);
    s.chief = j.value("chief", d.chief);
    s.roads = j.value("roads", d.roads);
    s.roadWidth = j.value("road_width", d.roadWidth);
    s.square = j.value("square", d.square);
    s.squareR = j.value("square_r", d.squareR);
    s.roadKind = j.value("road_kind", d.roadKind);
    s.entrance = j.value("entrance", d.entrance);
    if (j.contains("polity")) {   // AL-92
        const Json& p = j["polity"];
        s.mode = p.value("mode", d.mode);
        s.km = p.value("km", d.km);
        s.pop = p.value("pop", d.pop), s.food = p.value("food", d.food), s.wealth = p.value("wealth", d.wealth);
        s.militia = p.value("militia", d.militia), s.morale = p.value("morale", d.morale), s.unrest = p.value("unrest", d.unrest);
        s.land = p.value("land", d.land), s.fields = p.value("fields", d.fields), s.crafts = p.value("crafts", d.crafts), s.fort = p.value("fort", d.fort);
        s.heir = p.value("heir", d.heir), s.liege = p.value("liege", d.liege), s.tributeTo = p.value("tribute_to", d.tributeTo);
        s.tribute = p.value("tribute", d.tribute), s.house = p.value("house", d.house), s.ruined = p.value("ruined", d.ruined);
        s.founded = p.value("founded", d.founded), s.ruinedOn = p.value("ruined_on", d.ruinedOn);
        s.lastRaid = p.value("last_raid", d.lastRaid), s.lastRaided = p.value("last_raided", d.lastRaided);
        s.lastRevolt = p.value("last_revolt", d.lastRevolt), s.lastRefugees = p.value("last_refugees", d.lastRefugees);
        s.unrestDays = p.value("unrest_days", d.unrestDays), s.size = p.value("size", d.size);
        s.tags = p.value("tags", d.tags), s.income = p.value("income", d.income);
    }
    s.peoples = j.value("peoples", std::map<std::string, double>{});
    s.zones.clear();
    for (const auto& z : j.value("zones", Json::array())) {
        const auto r = z.value("rect", std::vector<double>{});
        if (r.size() == 5) s.zones.push_back({z.value("kind", std::string()), {r[0], r[1]}, r[2], r[3], r[4]});
    }
    s.structures.clear();
    for (const auto& t : j.value("structures", Json::array())) {
        Settlement::Structure x;
        x.family = t.value("family", kNone);
        x.recipe = t.value("recipe", std::string());
        const auto fr = t.value("frame", std::vector<int>{0, 0, 0});
        if (fr.size() == 3) x.q = fr[0], x.gx = fr[1], x.gy = fr[2];
        const auto cl = t.value("cells", std::vector<int>{0, 0, 0, 0});
        if (cl.size() == 4) x.i0 = cl[0], x.j0 = cl[1], x.i1 = cl[2], x.j1 = cl[3];
        x.h = t.value("h", 0.0);
        x.door = t.value("door", Vec2{});
        x.objects = t.value("objects", std::vector<EntityId>{});
        for (const auto& p : t.value("props", Json::array()))
            x.props.push_back({p.value("piece", std::string()), p.value("pos", Vec2{}), p.value("yaw", 0.0)});
        s.structures.push_back(std::move(x));
    }
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ChronicleEntry, tick, type, who, code, params)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Fact, id, tick, pred, roles, place, witnesses, evidence, tags, goods,
                                                outcome)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MapGrid, sizeM, cellM, w, h, biomeNames, biome, water)

void to_json(Json& j, const WorldObject& o) {
    j = Json{{"id", o.id},       {"type", o.type},   {"pos", o.pos},
             {"owner", o.ownerFamily}, {"stock", o.stock}, {"reserved_by", o.reservedBy},
             {"reserved_until", o.reservedUntil}};
    if (o.faced) j["yaw"] = o.yaw;
    if (o.storey) j["storey"] = o.storey;   // AL-81
}
void from_json(const Json& j, WorldObject& o) {
    o.id = j.at("id").get<EntityId>();
    o.type = j.at("type").get<std::string>();
    o.pos = j.at("pos").get<Vec2>();
    o.ownerFamily = j.value("owner", kNone);
    o.stock = j.value("stock", std::map<std::string, int>{});
    o.reservedBy = j.value("reserved_by", kNone);
    o.reservedUntil = j.value("reserved_until", Tick{0});
    o.faced = j.contains("yaw");
    o.yaw = j.value("yaw", 0.0);
    o.storey = j.value("storey", 0);
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

void to_json(Json& j, const BuildPiece& p) {
    j = Json{{"id", p.id}, {"kind", p.kind}, {"x", p.x}, {"y", p.y}, {"z", p.z}, {"rot", p.rot}, {"h", p.h},
             {"built", p.built}, {"work", p.work}, {"family", p.family}, {"site", p.site}};
}
void from_json(const Json& j, BuildPiece& p) {
    p.id = j.at("id").get<EntityId>();
    p.kind = j.at("kind").get<std::string>();
    p.x = j.value("x", 0), p.y = j.value("y", 0), p.z = j.value("z", 0), p.rot = j.value("rot", 0);
    p.h = j.value("h", 0.0);
    p.built = j.value("built", false);
    p.work = j.value("work", 0);
    p.family = j.value("family", kNone);
    p.site = j.value("site", kNone);
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
    Json pcs = Json::array();
    for (const auto& [id, p] : pieces_) pcs.push_back(p);
    Json memEvents; // AL-83: what several people remember of one event is written once
    Json sharedChars = sharedMemoryJson(std::move(chars), memEvents);
    Json out = Json{{"format", "altera.sim.world/1"},
                {"seed", seed_},
                {"tick", tick_},
                {"start_day", startDay_},
                {"next_seq", nextSeq_},
                {"next_cmd_seq", nextCmdSeq_},
                {"next_id", nextId_},
                {"map", map_},
                {"characters", sharedChars},
                {"memory_events", memEvents},
                {"objects", objs},
                {"families", fams},
                {"dynasties", dyns},
                {"settlements", sets},
                {"pieces", pcs},
                {"events", ev},
                {"pending", pending_},
                {"command_log", commandLog_},
                {"chronicle", chronicle_},
                {"facts", facts_},
                {"said", altera::sim::toJson(said_)},
                {"modules", modules_}};
    if (!offstage_.empty()) { // AL-92
        Json off = Json::array();
        for (const auto& [id, c] : offstage_) off.push_back(c);
        out["offstage"] = std::move(off);
    }
    if (!fields_.empty() || !fields_.objectIntegrity.empty() || !fields_.pieceIntegrity.empty() || !fields_.weather.empty())
        out["fields"] = fields_.toJson(); // AL-115
    return out;
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
    Json charsJ = j.at("characters");
    if (j.contains("memory_events")) expandSharedMemory(charsJ, j["memory_events"]);
    for (const auto& cj : charsJ) {
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
    pieces_.clear();
    for (const auto& pj : j.value("pieces", Json::array())) {
        BuildPiece p = pj.get<BuildPiece>();
        pieces_.emplace(p.id, std::move(p));
    }
    piecesDirty = true;
    events_ = j.at("events").get<std::vector<Event>>();
    std::make_heap(events_.begin(), events_.end(), laterEvent);
    pending_ = j.at("pending").get<std::vector<Command>>();
    commandLog_ = j.at("command_log").get<std::vector<Command>>();
    chronicle_ = j.at("chronicle").get<std::vector<ChronicleEntry>>();
    facts_ = j.value("facts", std::vector<Fact>{});
    said_ = SaidMemory{};
    if (j.contains("said")) altera::sim::fromJson(j["said"], said_);
    offstage_.clear();
    for (const auto& cj : j.value("offstage", Json::array())) {
        Character c = cj.get<Character>();
        offstage_.emplace(c.id, std::move(c));
    }
    modules_.clear();
    if (j.contains("modules"))
        for (auto it = j["modules"].begin(); it != j["modules"].end(); ++it) modules_[it.key()] = *it;
    fields_ = WorldFields{}; // AL-115
    if (j.contains("fields")) fields_.fromJson(j["fields"]);
}

Fact& World::addFact(Fact f) {
    // own numbering, monotonic from 1 (0 = no fact): facts are never removed, an id is never reused
    f.id = static_cast<EntityId>(facts_.size() + 1);
    f.tick = tick_;
    facts_.push_back(std::move(f));
    return facts_.back();
}

std::uint64_t World::stateHash() const { return hashStr(toJson().dump()); }

} // namespace altera::sim
