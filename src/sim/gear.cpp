#include "sim/gear.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>

#include "sim/character.hpp"
#include "sim/content.hpp"
#include "sim/world.hpp"

namespace lr::sim {

namespace fs = std::filesystem;

const GearRules* gearRules(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<GearRules>> cache;
    if (c.dir.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(c.dir);
    if (it != cache.end()) return it->second.get();
    std::unique_ptr<GearRules> r;
    const fs::path p = fs::path(c.dir) / "gear.json";
    if (fs::exists(p)) {
        r = std::make_unique<GearRules>();
        std::ifstream f(p, std::ios::binary);
        const Json j = Json::parse(f);
        const Json carry = j.value("carry", Json::object());
        r->comfortKg = carry.value("comfort_kg", r->comfortKg);
        r->maxKg = carry.value("max_kg", r->maxKg);
        r->strengthAttr = carry.value("strength_attr", r->strengthAttr);
        r->perAttrKg = carry.value("per_attr_kg", r->perAttrKg);
        const Json fat = j.value("fatigue", Json::object());
        r->loadFatigue = fat.value("load_mult", r->loadFatigue);
        r->walkPerKm = fat.value("walk_per_km", r->walkPerKm);
        r->runPerKm = fat.value("run_per_km", r->runPerKm);
        r->climbPer100m = fat.value("climb_per_100m", r->climbPer100m);
        const Json hands = j.value("hands", Json::array({2, 3}));
        r->handsW = hands.at(0).get<int>();
        r->handsH = hands.at(1).get<int>();
        for (const auto& s : j.value("slots", Json::array())) {
            GearSlot g;
            g.id = s.at("id").get<std::string>();
            g.layers = s.value("layers", std::vector<std::string>{});
            g.bulk = s.value("bulk", 1);
            r->slots.push_back(std::move(g));
        }
        auto readItem = [](const Json& v, GearItem& g) {
            g.kg = v.value("kg", g.kg);
            g.haul = v.value("haul", g.haul);
            if (v.contains("size")) {
                g.w = v["size"].at(0).get<int>();
                g.h = v["size"].at(1).get<int>();
            }
            if (v.contains("wear")) {
                const Json& w = v["wear"];
                g.slot = w.value("slot", std::string());
                g.layer = w.value("layer", std::string());
                g.bulk = w.value("bulk", 1);
            }
            if (v.contains("grid")) {
                g.gridW = v["grid"].at(0).get<int>();
                g.gridH = v["grid"].at(1).get<int>();
            }
            g.warmth = v.value("warmth", 0.0);
        };
        readItem(j.value("default", Json::object()), r->fallback);
        const Json base = j.value("base_clothes", Json::object());
        for (const auto& [who, set] : base.items())
            if (set.is_object())
                for (const auto& [item, n] : set.items())
                    if (n.is_number_integer()) r->baseClothes[who][item] = n.get<int>();
        // stack sizes from items.json (how many small things share a cell)
        if (const fs::path ip = fs::path(c.dir) / "items.json"; fs::exists(ip)) {
            std::ifstream fi(ip, std::ios::binary);
            const Json items = Json::parse(fi).value("items", Json::array());
            for (const auto& v : items)
                if (v.contains("id")) r->stacks[v["id"].get<std::string>()] = v.value("stack", 1);
        }
        const Json itemsJ = j.value("items", Json::object());   // a local: items() of a temporary dangles
        for (const auto& [k, v] : itemsJ.items()) {
            GearItem g = r->fallback;
            g.slot.clear(), g.layer.clear(), g.bulk = 0, g.gridW = g.gridH = 0;
            readItem(v, g);
            if (!g.slot.empty()) {
                const GearSlot* s = nullptr;
                for (const auto& x : r->slots)
                    if (x.id == g.slot) s = &x;
                if (!s) throw std::runtime_error("gear.json: " + k + " is worn on an unknown slot " + g.slot);
                if (std::find(s->layers.begin(), s->layers.end(), g.layer) == s->layers.end())
                    throw std::runtime_error("gear.json: " + k + " is worn on an unknown layer " + g.layer);
            }
            r->items[k] = g;
        }
    }
    return (cache[c.dir] = std::move(r)).get();
}

double loadKg(const GearRules& R, const Character& c) {
    double kg = 0;
    for (const auto& [k, n] : c.inventory)
        if (n > 0) kg += R.item(k).kg * n;
    return kg;
}

double maxLoadKg(const GearRules& R, const Character& c) {
    const auto it = c.attrs.find(R.strengthAttr);
    const double a = it == c.attrs.end() ? 0.5 : it->second;
    return std::max(R.comfortKg + 2.0, R.maxKg + (a - 0.5) * 2.0 * R.perAttrKg);
}

double loadFatigueMult(const GearRules& R, const Character& c) {
    const double kg = loadKg(R, c), mx = maxLoadKg(R, c);
    if (kg <= R.comfortKg) return 1.0;
    return 1.0 + R.loadFatigue * std::clamp((kg - R.comfortKg) / std::max(1.0, mx - R.comfortKg), 0.0, 1.5);
}

int wornCount(const Character& c, const std::string& item) {
    int n = 0;
    for (const auto& [k, v] : c.worn) n += v == item;
    return n;
}

namespace {

// The grids one has: the hands, then every worn container in slot order.
std::vector<GearGrid> gridsOf(const GearRules& R, const Character& c) {
    std::vector<GearGrid> out{{"hands", R.handsW, R.handsH}};
    for (const auto& s : R.slots)
        for (const auto& l : s.layers) {
            const auto it = c.worn.find(s.id + ":" + l);
            if (it == c.worn.end()) continue;
            const GearItem& g = R.item(it->second);
            if (g.gridW > 0 && g.gridH > 0) out.push_back({it->second, g.gridW, g.gridH});
        }
    return out;
}

int stackOf(const GearRules& R, const std::string& id) {
    const auto it = R.stacks.find(id);
    return it == R.stacks.end() ? 1 : std::max(1, it->second);
}

} // namespace

GearLayout arrange(const GearRules& R, const Character& c, const std::map<std::string, int>* extra) {
    GearLayout L;
    L.grids = gridsOf(R, c);
    std::map<std::string, std::vector<std::vector<bool>>> used;
    for (const auto& g : L.grids) used[g.id].assign(std::size_t(g.h), std::vector<bool>(std::size_t(g.w), false));
    auto fits = [&](const GearGrid& g, int x, int y, int w, int h) {
        if (x < 0 || y < 0 || x + w > g.w || y + h > g.h) return false;
        for (int j = y; j < y + h; ++j)
            for (int i = x; i < x + w; ++i)
                if (used[g.id][std::size_t(j)][std::size_t(i)]) return false;
        return true;
    };
    auto take = [&](const GearGrid& g, int x, int y, int w, int h) {
        for (int j = y; j < y + h; ++j)
            for (int i = x; i < x + w; ++i) used[g.id][std::size_t(j)][std::size_t(i)] = true;
    };
    // what is carried (not worn): stacks, biggest first
    std::map<std::string, int> carried = c.inventory;
    if (extra)
        for (const auto& [k, n] : *extra) carried[k] += n;
    struct Piece {
        std::string item;
        int count, w, h;
    };
    std::vector<Piece> pieces;
    for (const auto& [k, n0] : carried) {
        const int n = n0 - wornCount(c, k);
        if (n <= 0) continue;
        const GearItem& g = R.item(k);
        if (g.haul) { // AL-73: on the shoulder, not in the hands or bags
            L.hauled[k] += n;
            continue;
        }
        // Things a stack holds per its cells: small things (a cell) stack in
        // the item's stack size, big ones one a spot.
        const int per = g.w * g.h == 1 ? stackOf(R, k) : 1;
        for (int left = n; left > 0; left -= per) pieces.push_back({k, std::min(per, left), g.w, g.h});
    }
    std::stable_sort(pieces.begin(), pieces.end(), [&](const Piece& a, const Piece& b) {
        const bool pa = c.gearPins.count(a.item) > 0, pb = c.gearPins.count(b.item) > 0;
        if (pa != pb) return pa;
        return a.w * a.h > b.w * b.h;
    });
    std::map<std::string, bool> pinUsed;
    for (const auto& p : pieces) {
        bool placed = false;
        // where the player put it, the first stack of it
        if (const auto pin = c.gearPins.find(p.item); pin != c.gearPins.end() && !pinUsed[p.item]) {
            for (const auto& g : L.grids) {
                if (g.id != pin->second.grid) continue;
                const int w = pin->second.turned ? p.h : p.w, h = pin->second.turned ? p.w : p.h;
                if (fits(g, pin->second.x, pin->second.y, w, h)) {
                    take(g, pin->second.x, pin->second.y, w, h);
                    L.spots.push_back({p.item, p.count, g.id, pin->second.x, pin->second.y, w, h, pin->second.turned});
                    placed = true;
                }
            }
            pinUsed[p.item] = true;
        }
        for (const auto& g : L.grids) {
            for (int turn = 0; turn < 2 && !placed; ++turn) {
                const int w = turn ? p.h : p.w, h = turn ? p.w : p.h;
                if (turn && w == h) break;
                for (int y = 0; y + h <= g.h && !placed; ++y)
                    for (int x = 0; x + w <= g.w && !placed; ++x)
                        if (fits(g, x, y, w, h)) {
                            take(g, x, y, w, h);
                            L.spots.push_back({p.item, p.count, g.id, x, y, w, h, turn == 1});
                            placed = true;
                        }
            }
            if (placed) break;
        }
        if (!placed) L.overflow[p.item] += p.count;
    }
    return L;
}

std::string canWear(const GearRules& R, const Character& c, const std::string& item) {
    const GearItem& g = R.item(item);
    if (g.slot.empty()) return "not_wearable";
    const auto inv = c.inventory.find(item);
    if (inv == c.inventory.end() || inv->second - wornCount(c, item) <= 0) return "not_carried";
    const auto on = c.worn.find(g.slot + ":" + g.layer);
    if (on != c.worn.end()) return "layer_taken:" + on->second;
    const GearSlot* s = R.slot(g.slot);
    int bulk = g.bulk;
    for (const auto& l : s->layers)
        if (const auto it = c.worn.find(s->id + ":" + l); it != c.worn.end()) bulk += R.item(it->second).bulk;
    if (bulk > s->bulk) return "too_bulky";
    return "";
}

std::string canTake(const GearRules& R, const Character& c, const std::map<std::string, int>& gains) {
    double kg = loadKg(R, c), add = 0;
    for (const auto& [k, n] : gains) add += R.item(k).kg * n;
    if (add > 0 && kg + add > maxLoadKg(R, c) + 1e-6) return "too_heavy";
    // what already finds no room is not this act's fault: only more of it is
    auto lost = [](const GearLayout& L) {
        int n = 0;
        for (const auto& [k, v] : L.overflow) n += v;
        return n;
    };
    if (lost(arrange(R, c, &gains)) > lost(arrange(R, c))) return "no_room";
    return "";
}

void pruneGear(Character& c) {
    for (auto it = c.worn.begin(); it != c.worn.end();) {
        const auto inv = c.inventory.find(it->second);
        const int have = inv == c.inventory.end() ? 0 : inv->second;
        if (wornCount(c, it->second) > have) it = c.worn.erase(it);
        else ++it;
    }
    for (auto it = c.gearPins.begin(); it != c.gearPins.end();) {
        if (!c.inventory.count(it->first)) it = c.gearPins.erase(it);
        else ++it;
    }
}

void wearAll(const GearRules& R, Character& c) {
    // the heaviest first: armour before the shirt would be the wrong way, so by layer order
    for (const auto& s : R.slots)
        for (const auto& l : s.layers)
            for (const auto& [k, n] : c.inventory) {
                const GearItem& g = R.item(k);
                if (g.slot != s.id || g.layer != l || n <= 0) continue;
                if (canWear(R, c, k).empty()) c.worn[g.slot + ":" + g.layer] = k;
            }
}

void dressTick(World& w) {
    constexpr Tick kHour = kTicksPerDay / 24;
    if (w.now() % kHour != 0) return;
    const GearRules* R = gearRules(w.content());
    if (!R) return;
    const bool start = w.now() == 0;
    for (auto& [id, c] : w.characters()) {
        if (start && c.body.alive && !c.isAnimal()) giveBaseClothes(w, c);   // LR-139: nobody starts naked
        if (!c.body.alive || c.isAnimal() || c.inventory.empty()) continue;
        if (c.controller.isPlayer() && !start) continue;   // the player dresses his person himself
        wearAll(*R, c);
    }
}

void giveBaseClothes(World& w, Character& c) {
    if (c.isAnimal()) return;
    const GearRules* R = gearRules(w.content());
    if (!R || R->baseClothes.empty()) return;
    const std::string who = c.body.stage() == "child" ? "child" : (c.body.sex == 'f' ? "f" : "m");
    const auto set = R->baseClothes.find(who);
    if (set == R->baseClothes.end()) return;
    for (const auto& [item, n] : set->second) {
        const GearItem& g = R->item(item);
        if (g.slot.empty() || n <= 0) continue;
        if (c.worn.count(g.slot + ":" + g.layer)) continue;   // already dressed there
        bool has = false;                                      // or carries something for that layer
        for (const auto& [k, m] : c.inventory)
            if (m > 0 && R->item(k).slot == g.slot && R->item(k).layer == g.layer) has = true;
        if (!has) c.inventory[item] += n;
    }
    wearAll(*R, c);
}

namespace {

bool handle(World& w, Command& cmd) {
    const std::string& t = cmd.type;
    if (t != "wear" && t != "take_off" && t != "gear_pin" && t != "wear_all") return false;
    Character* c = w.character(cmd.args.at("id").get<EntityId>());
    if (!c) throw std::runtime_error("no such character");
    const GearRules* R = gearRules(w.content());
    if (!R) return cmd.result = "rejected:no_gear_rules", true;
    if (t == "wear_all") {   // put on what one carries that can be worn (a new person's start)
        wearAll(*R, *c);
        return true;
    }
    const std::string item = cmd.args.at("item").get<std::string>();
    if (t == "wear") {
        const std::string why = canWear(*R, *c, item);
        if (!why.empty()) return cmd.result = "rejected:" + why, true;
        const GearItem& g = R->item(item);
        c->worn[g.slot + ":" + g.layer] = item;
        c->gearPins.erase(item);
        return true;
    }
    if (t == "take_off") {
        for (auto it = c->worn.begin(); it != c->worn.end(); ++it)
            if (it->second == item) {
                // a bag that comes off: what was in it must find room elsewhere
                Character after = *c;
                after.worn.erase(after.worn.find(it->first));
                if (!arrange(*R, after).overflow.empty()) return cmd.result = "rejected:no_room", true;
                c->worn.erase(it);
                return true;
            }
        return cmd.result = "rejected:not_worn", true;
    }
    // gear_pin
    if (cmd.args.value("clear", false)) {
        c->gearPins.erase(item);
        return true;
    }
    if (!c->inventory.count(item)) return cmd.result = "rejected:not_carried", true;
    Character::GearPin pin{cmd.args.at("grid").get<std::string>(), cmd.args.at("x").get<int>(), cmd.args.at("y").get<int>(),
                           cmd.args.value("turned", false)};
    Character trial = *c;
    trial.gearPins[item] = pin;
    // the pin holds only where the thing fits (else it is left where it was)
    const GearLayout L = arrange(*R, trial);
    bool there = false;
    for (const auto& s : L.spots)
        there |= s.item == item && s.grid == pin.grid && s.x == pin.x && s.y == pin.y;
    if (!there) return cmd.result = "rejected:no_room", true;
    c->gearPins[item] = pin;
    return true;
}

} // namespace

void installGear(World& w) { w.addCommandHandler(handle); }

} // namespace lr::sim
