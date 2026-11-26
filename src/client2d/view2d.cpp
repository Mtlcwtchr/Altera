#include "client2d/view2d.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

#include "client2d/grid2d.hpp"
#include "sim/ai.hpp"
#include "sim/need_tiers.hpp"
#include "sim/combat_bridge.hpp"
#include "sim/transport.hpp"

namespace lr::client2d {

using namespace lr::sim;

std::string ViewText::name(const std::string& kind, const std::string& id) const {
    if (id.empty() || !loc) return id;
    const std::string k = kind + "." + id + ".name";
    if (loc->has(lang, k)) return loc->text(lang, k);
    const std::string k2 = kind + "." + id;   // rooms: room.<type>
    if (loc->has(lang, k2)) return loc->text(lang, k2);
    return id;
}

namespace {

double r2(double v) { return std::round(v * 100) / 100; }

std::string clockText(Tick t) {
    const int m = static_cast<int>(t % kTicksPerDay) * 10;
    char b[16];
    std::snprintf(b, sizeof b, "%02d:%02d", m / 60, m % 60);
    return b;
}

std::string personName(const ViewText& t, const Character& c) {
    if (c.isAnimal()) return t.name("species", c.species);
    if (!t.loc) return c.name;
    const Json n = t.loc->personNoun(t.lang, c);
    return n.is_object() ? n.value("nom", c.name) : c.name;
}

// The act going on: the interaction, or where the walk leads.
std::string activityOf(const ViewText& t, const Character& c) {
    if (!c.body.alive) return "";
    const FsmState& f = c.ai.fsm;
    if (!f.interaction.empty() && f.interaction != "goto") return t.name("interaction", f.interaction);
    if (f.interaction == "goto" && c.ai.plan.actionIndex + 1 < c.ai.plan.actions.size()) {
        const auto& next = c.ai.plan.actions[c.ai.plan.actionIndex + 1];
        if (next.interaction != "goto") return t.name("interaction", next.interaction);
    }
    return "";
}

// The interaction the person is busy with, or the one the walk leads to (its icon).
std::string actId(const Character& c) {
    const FsmState& f = c.ai.fsm;
    if (f.interaction == "goto" && c.ai.plan.actionIndex + 1 < c.ai.plan.actions.size())
        return c.ai.plan.actions[c.ai.plan.actionIndex + 1].interaction;
    return f.interaction;
}

std::string goalOf(const ViewText& t, const Character& c) {
    if (!c.ai.goal.valid()) return "";
    if (c.ai.goal.id == "player_action") return t.name("interaction", c.ai.goal.task);
    return t.name("goal", c.ai.goal.id);
}

bool carries(const Character& c, const std::string& item) {
    const auto it = c.inventory.find(item);
    return it != c.inventory.end() && it->second > 0;
}

// AL-109: what the person holds in hand now: the tool of the act he is busy with (or
// walks to) — an item the act asks to have and does not use up (an axe to chop, a
// needle to sew), a tool its crafting processes ask for, the weapon of a fight —
// else a carried weapon (one with a spear walks with it). "" for empty hands.
std::string heldOf(const World& w, const Character& c) {
    if (!c.body.alive || c.ai.fsm.state == "Sleep") return "";
    const Content& ct = w.content();
    const CombatBridge* cb = combatBridge(ct);
    auto weapon = [&]() -> std::string {
        if (cb)
            for (const auto& item : cb->weaponItems)
                if (carries(c, item)) return item;
        for (const auto& it : c.craft.items)
            if (it.kind == "weapon" && it.wear < 1.0)
                if (const auto* f = ct.craft.form(it.form); f && !f->item.empty() && carries(c, f->item)) return f->item;
        return "";
    };
    const std::string id = actId(c);
    if (const InteractionDef* d = id.empty() ? nullptr : ct.interaction(id)) {
        if (d->combat.set) return weapon();
        for (const auto& k : d->pre) {
            if (k.negate || k.onTarget || k.subject != "has" || k.isParam()) continue;
            if (k.op != Condition::Op::Truthy && !(k.op == Condition::Op::Ge && k.value <= 1)) continue;
            bool used = false;
            for (const auto& e : d->eff)
                used |= e.subject == "has" && !e.onTarget && e.key == k.key && e.op == '-';
            if (!used && carries(c, k.key)) return k.key;
        }
        if (d->craft.rfind("design:", 0) == 0)
            if (const auto* ds = ct.craft.design(d->craft.substr(7)))
                for (const auto& st : ds->steps)
                    if (const auto* pr = ct.craft.process(st.process))
                        for (const auto& tag : pr->tools)
                            for (const auto& f : ct.craft.forms) {
                                if (f.item.empty() || !carries(c, f.item)) continue;
                                if (std::find(f.deriveTags.begin(), f.deriveTags.end(), tag) != f.deriveTags.end())
                                    return f.item;
                            }
    }
    return weapon();
}

} // namespace

// AL-114: the kind as the 2D client draws it: sprite size, skin, glyph, stone in the sun.
static void raceJson(const World& w, const Character& c, Json& p) {
    if (c.race.empty()) {   // LR-139: a human (the default kind) still has its hair and beard sets
        if (w.content().peoples.loaded && !c.isAnimal())
            if (const sim::RaceDef& r = sim::raceOf(w.content(), c); !r.pawn.empty()) p["rlook"] = r.pawn;
        return;
    }
    const sim::RaceDef& r = sim::raceOf(w.content(), c);
    p["race"] = c.race;
    p["rpx"] = r.spritePx;
    if (!r.skin.empty()) p["rskin"] = r.skin;
    if (!r.glyph.empty()) p["rg"] = r.glyph;
    if (!r.pawn.empty()) p["rlook"] = r.pawn;   // LR-139: ears, head, tusks, build...
    if (c.body.petrified) p["stone"] = true;
}

// LR-159: on a beast / in a vehicle: ride {id, sp, dy} or seat {veh, sp, dy, driver}, spd (walk multiplier);
// a ridden or harnessed beast says by whom (rider) or to what (team), so the client draws it with them.
static void transportJson(const World& w, const Character& c, Json& p) {
    const sim::TransportRules* R = sim::transportRules(w.content());
    if (!R) return;
    if (c.isAnimal()) {
        if (const EntityId r = sim::riderOf(w, c.id); r != kNone) p["rider"] = r;
        if (const EntityId v = sim::hitchedTo(w, c.id); v != kNone) p["team"] = v;
        return;
    }
    if (const EntityId b = sim::rideOf(w, c.id); b != kNone)
        if (const Character* a = w.character(b)) {
            const sim::MountDef* m = R->mount(a->species);
            p["ride"] = {{"id", b}, {"sp", a->species}, {"dy", m ? m->riderDyPx : 6}};
        }
    if (const EntityId v = sim::vehicleOf(w, c.id); v != kNone)
        if (const WorldObject* o = w.object(v))
            if (const sim::VehicleDef* d = R->vehicle(o->type); d && d->teamMax > 0)   // a handcart is pushed, not sat on
                p["seat"] = {{"veh", v}, {"sp", o->type}, {"dy", d->seatDyPx >= 0 ? d->seatDyPx : R->seatDyPx},
                             {"driver", sim::driverOf(w, v) == c.id}};
    const double s = sim::transportSpeedMult(w, c);
    if (std::abs(s - 1.0) > 1e-6) p["spd"] = r2(s);
}

// AL-136: the look chosen at the character's creation (the pawn's body, skin, hair...)
static void lookJson(const Character& c, Json& p) {
    if (!c.look.empty()) p["look"] = c.look;
}

Json peopleJson(const World& w, const ViewText& t, const Vec2* near, double beastsR) {
    Json out = Json::array();
    for (const auto& [id, c] : w.characters()) {
        if (near && c.isAnimal() && dist(c.pos, *near) > beastsR) continue;
        const Family* f = w.familyOf(c);
        Json p = {{"id", id},
                  {"name", personName(t, c)},
                  {"sp", c.species},
                  {"x", r2(c.pos.x)},
                  {"y", r2(c.pos.y)},
                  {"alive", c.body.alive},
                  {"down", c.body.downUntil > w.now()},
                  {"pl", c.controller.isPlayer()},
                  {"pid", c.controller.isPlayer() ? c.controller.playerId : 0},   // LR-160: whose person
                  {"sex", std::string(1, c.body.sex)},
                  {"cul", c.culture},   // LR-165: the 2D figure's dress by culture
                  {"age", c.body.ageDays / kDaysPerYear},
                  {"fam", f ? f->name : ""},
                  {"famId", f ? f->id : kNone},
                  {"st", c.ai.fsm.state},
                  {"ia", actId(c)},
                  {"act", activityOf(t, c)},
                  {"goal", goalOf(t, c)},
                  {"hp", r2(c.body.health)}};
        // how far the act is along (the cast bar): ticks done of done + left
        const FsmState& fs = c.ai.fsm;
        if (fs.state != "Idle" && fs.state != "Move" && fs.timer > 0 && w.now() >= fs.started) {
            const double done = double(w.now() - fs.started);
            p["prog"] = r2(done / (done + double(fs.timer)));
        }
        if (c.ai.fsm.interaction == "goto" || c.ai.fsm.state == "Move")
            p["dest"] = Json::array({r2(c.ai.fsm.dest.x), r2(c.ai.fsm.dest.y)});
        // LR-152: stamina (share of the cap), spent to the bottom (no run)
        if (c.body.staminaMax > 0 && c.body.stamina >= 0) {
            p["sta"] = r2(c.body.stamina / c.body.staminaMax);
            if (c.body.winded) p["wnd"] = true;
        }
        // AL-108: where the person looks (the act's target, the walk's way): the pawn's side
        p["fx"] = r2(c.facing.x);
        p["fy"] = r2(c.facing.y);
        if (!c.isAnimal()) {
            Json worn = Json::array();
            for (const auto& [slot, item] : c.worn) worn.push_back(item);
            p["worn"] = worn;
            p["held"] = heldOf(w, c);
            // AL-109: the colour of a dyed made thing worn (a blue tunic): item -> colour_* tag's colour
            Json dye = Json::object();
            for (const auto& [slot, item] : c.worn)
                for (const auto& it : c.craft.items) {
                    const auto* f = w.content().craft.form(it.form);
                    if (!f || f->item != item) continue;
                    std::function<void(const craft::Item&)> parts = [&](const craft::Item& x) {
                        for (const auto& tag : x.tags)
                            if (tag.rfind("colour_", 0) == 0) dye[item] = tag.substr(7);
                        for (const auto& pt : x.parts)
                            if (!pt.item.empty()) parts(pt.item[0]);
                    };
                    parts(it);
                    if (dye.contains(item)) break;
                }
            if (!dye.empty()) p["dye"] = dye;
            Json inv = Json::array();
            for (const auto& [item, n] : c.inventory)
                if (n > 0) inv.push_back(item);
            p["inv"] = inv;
            p["occ"] = c.occupation;
            p["role"] = c.status.role;
            raceJson(w, c, p); // AL-114
            lookJson(c, p);    // AL-136
        }
        transportJson(w, c, p); // LR-159
        out.push_back(std::move(p));
    }
    // LR-159: vehicles go in the list as beasts do (sp = the vehicle type, veh = true): they move
    if (const sim::TransportRules* R = sim::transportRules(w.content()))
        for (const auto& [id, o] : w.objects()) {
            if (!R->vehicle(o.type)) continue;
            if (near && dist(o.pos, *near) > beastsR) continue;
            Json v = {{"id", id}, {"name", t.name("object", o.type)}, {"sp", o.type}, {"veh", true},
                      {"x", r2(o.pos.x)}, {"y", r2(o.pos.y)}, {"alive", true}, {"famId", o.ownerFamily},
                      {"st", "Idle"}, {"ia", ""}};
            const sim::Json* st = w.moduleState("transport");
            double fx = 1.0, fy = 0.0;
            if (st && st->contains("veh") && (*st)["veh"].contains(std::to_string(id))) {
                const sim::Json& vs = (*st)["veh"][std::to_string(id)];
                fx = vs.value("fx", 1.0), fy = vs.value("fy", 0.0);
            }
            v["fx"] = r2(fx);
            v["fy"] = r2(fy);
            Json team = Json::array();
            for (EntityId b : sim::teamOf(w, id)) team.push_back(b);
            v["team"] = team;
            if (const EntityId d = sim::driverOf(w, id); d != kNone) {
                if (R->vehicle(o.type)->teamMax > 0) v["driver"] = d;   // drawn with the driver
                else v["pusher"] = d;                                  // a handcart draws itself, before the pusher
            }
            if (!o.stock.empty()) v["cargo"] = r2(sim::vehicleCargoKg(w, o) / std::max(1.0, sim::vehicleCapacityKg(w, o)));
            out.push_back(std::move(v));
        }
    return out;
}

Json personJson(const World& w, const ViewText& t, EntityId id) {
    const Character* cp = w.character(id);
    if (!cp) return nullptr;
    const Character& c = *cp;
    const Family* f = w.familyOf(c);
    Json d = {{"id", id},
              {"name", personName(t, c)},
              {"sp", c.species},
              {"spn", c.isAnimal() ? t.name("species", c.species) : (c.race.empty() ? "" : t.name("species", c.race))},
              {"alive", c.body.alive},
              {"cause", c.body.causeOfDeath},
              {"sex", std::string(1, c.body.sex)},
              {"age", c.body.ageDays / kDaysPerYear},
              {"fam", f ? f->name : ""},
              {"act", activityOf(t, c)},
              {"goal", goalOf(t, c)},
              {"hp", r2(c.body.health)},
              {"fatigue", r2(c.body.fatigue)},
              {"stamina", r2(std::max(0.0, c.body.stamina))},   // LR-152
              {"stamina_max", r2(c.body.staminaMax)},
              {"winded", c.body.winded},
              {"mood", r2(c.mood(w.now()))},
              {"role", c.status.role},
              {"occ", c.occupation},
              {"pl", c.controller.isPlayer()},
              {"pid", c.controller.isPlayer() ? c.controller.playerId : 0},   // LR-160: whose person
              {"lead", mayLead(w, t.player, c)}}; // LR-138: only one's own kin
    // LR-137: the names to show for the family and the trade (the raw ids stay above)
    if (f && t.loc) d["famn"] = t.loc->personNoun(t.lang, f->name, 'm').value("nom", f->name);
    if (!c.occupation.empty() && t.loc) {
        const std::string k = "news.trade." + c.occupation + (c.body.sex == 'f' ? ".f" : ".m");
        d["occn"] = t.loc->has(t.lang, k) ? t.loc->text(t.lang, k) : c.occupation;
    }
    Json needs, tiers;
    needsJson(w, t, c, needs, tiers, true);
    d["needs"] = needs;
    d["tiers"] = tiers; // LR-140
    // LR-137: what the pixel HUD shows of a person at a glance (the hero frame, the column)
    d["blood"] = r2(c.body.blood / 100.0);
    Json attrs = Json::array();
    for (const auto& [aid, v] : c.attrs) attrs.push_back({{"id", aid}, {"name", t.name("attr", aid)}, {"v", r2(v)}});
    d["attrs"] = attrs;
    Json moodlets = Json::array();
    const Tick now = w.now();
    for (const Moodlet& m : c.moodlets) {
        if (m.until != 0 && m.until <= now) continue;
        moodlets.push_back({{"id", m.id},
                            {"name", t.name("moodlet", m.id)},
                            {"v", r2(m.value)},
                            {"h", m.until == 0 ? -1.0 : std::round(double(m.until - now) / (kTicksPerDay / 24.0) * 10) / 10}});
    }
    std::stable_sort(moodlets.begin(), moodlets.end(),
                     [](const Json& a, const Json& b) { return std::abs(a["v"].get<double>()) > std::abs(b["v"].get<double>()); });
    d["moodlets"] = moodlets;
    Json skills = Json::array();
    for (const auto& [kid, v] : c.skills) skills.push_back({{"id", kid}, {"name", t.name("knowledge", kid)}, {"v", r2(v)}});
    std::stable_sort(skills.begin(), skills.end(), [](const Json& a, const Json& b) { return a["v"].get<double>() > b["v"].get<double>(); });
    d["nskills"] = skills.size();
    if (skills.size() > 6) skills.erase(skills.begin() + 6, skills.end());
    d["skills"] = skills;
    Json wounds = Json::array();
    for (const Wound& wd : c.wounds)
        wounds.push_back({{"zone", t.loc && t.loc->has(t.lang, "ui2d.sheet.zone." + wd.zone) ? t.loc->text(t.lang, "ui2d.sheet.zone." + wd.zone) : wd.zone},
                          {"sev", t.loc && t.loc->has(t.lang, "ui2d.sheet.sev." + wd.severity) ? t.loc->text(t.lang, "ui2d.sheet.sev." + wd.severity) : wd.severity},
                          {"bleeding", wd.bleeding}});
    d["wounds"] = wounds;
    Json plan = Json::array();
    for (std::size_t i = c.ai.plan.actionIndex; i < c.ai.plan.actions.size() && plan.size() < 4; ++i)
        if (c.ai.plan.actions[i].interaction != "goto") plan.push_back(t.name("interaction", c.ai.plan.actions[i].interaction));
    d["plan"] = plan;
    Json inv = Json::array();
    for (const auto& [item, n] : c.inventory)
        if (n > 0) inv.push_back({{"id", item}, {"name", t.name("item", item)}, {"n", n}});
    d["inv"] = inv;
    Json worn = Json::array();
    for (const auto& [slot, item] : c.worn) worn.push_back({{"slot", slot}, {"id", item}, {"name", t.name("item", item)}});
    d["worn"] = worn;
    Json traits = Json::array();
    for (const auto& tr : c.traits) traits.push_back(t.name("trait", tr));
    d["traits"] = traits;
    Json trace = Json::array();
    const std::size_t from = c.ai.trace.size() > 6 ? c.ai.trace.size() - 6 : 0;
    for (std::size_t i = c.ai.trace.size(); i-- > from;) {
        const TraceEntry& e = c.ai.trace[i];
        trace.push_back({{"t", clockText(e.tick)},
                         {"text", t.loc ? traceText(*t.loc, t.lang, w, c, e) : e.code}});
    }
    d["trace"] = trace;
    return d;
}

Json staticJson(const World& w, const ViewText& t) {
    const MapGrid& m = w.map();
    Json j;
    j["size"] = m.sizeM;
    j["ground"] = groundNames();
    Json st = Json::array();
    for (const auto& [id, s] : w.settlements())
        st.push_back({{"id", id}, {"name", s.name}, {"x", r2(s.center.x)}, {"y", r2(s.center.y)}, {"mode", s.mode}});
    j["settlements"] = st;
    Json rooms = Json::object();
    for (const auto& r : w.content().rooms.rules) rooms[r.id] = t.name("room", r.id);
    rooms[w.content().rooms.closedFallback] = t.name("room", w.content().rooms.closedFallback);
    rooms[w.content().rooms.openFallback] = t.name("room", w.content().rooms.openFallback);
    j["rooms"] = rooms;
    std::set<std::string> types;
    for (const auto& [id, o] : w.objects()) types.insert(o.type);
    Json objs = Json::object();
    for (const auto& ty : types) objs[ty] = t.name("object", ty);
    j["objects"] = objs;
    Json fams = Json::object();
    for (const auto& [id, f] : w.families()) fams[std::to_string(id)] = f.name;
    j["families"] = fams;
    Json ui = Json::object();
    if (t.loc)
        for (const auto& k : t.loc->keys(t.lang))
            if (k.rfind("ui2d.", 0) == 0) ui[k] = t.loc->text(t.lang, k);
    j["ui"] = ui;
    return j;
}

Json chronicleJson(const World& w, const ViewText& t, std::size_t since, std::size_t limit) {
    const auto& log = w.chronicleLog();
    Json list = Json::array();
    std::size_t from = std::min(since, log.size());
    if (log.size() - from > limit) from = log.size() - limit;
    for (std::size_t i = from; i < log.size(); ++i) {
        const ChronicleEntry& e = log[i];
        std::string text = e.code;
        if (t.loc && t.loc->has(t.lang, e.code)) text = t.loc->text(t.lang, e.code, locArgs(*t.loc, t.lang, w, e.params));
        list.push_back({{"i", i}, {"day", e.tick / kTicksPerDay}, {"t", clockText(e.tick)}, {"type", e.type}, {"who", e.who}, {"text", text}});
    }
    return {{"n", log.size()}, {"list", list}};
}

Json clockJson(const World& w) {
    Json j;
    j["tick"] = w.now();
    j["day"] = w.day();
    j["time"] = clockText(w.now());
    j["night"] = w.isNight();
    j["season"] = w.season();
    if (!w.settlements().empty()) {
        const DayWeather dw = weatherToday(w, w.settlements().begin()->first);
        j["weather"] = {{"t", std::round(dw.tempC * 10) / 10}, {"rain", std::round(dw.rainMm * 10) / 10}, {"wet", dw.wet}};
    }
    return j;
}

void needsJson(const World& w, const ViewText& t, const Character& c, Json& needs, Json& tiers, bool withCrit) {
    const Content& ct = w.content();
    needs = Json::array();
    tiers = Json::array();
    auto add = [&](const std::string& id, const NeedDef* nd) {
        auto it = c.needs.find(id);
        if (it == c.needs.end()) return;
        Json n = {{"id", id}, {"name", t.name("need", id)}, {"v", r2(it->second)}, {"tier", needTier(ct, id)},
                  {"derived", nd == nullptr}, {"care", r2(needCare(ct, c, id))}};
        if (withCrit && nd) n["crit"] = r2(nd->critical);
        needs.push_back(n);
    };
    for (const auto& nd : ct.needs) add(nd.id, &nd);
    if (!c.isAnimal())
        for (const auto& dn : ct.derivedNeeds) add(dn.id, nullptr);
    if (c.isAnimal()) return;
    for (const TierView& tv : needTiersOf(w, c)) {
        Json ids = Json::array();
        for (const auto& n : tv.needs) ids.push_back(n.id);
        tiers.push_back({{"id", tv.id}, {"name", t.name("need_tier", tv.id)}, {"order", tv.order}, {"sat", r2(tv.sat)}, {"needs", ids}});
    }
}

} // namespace lr::client2d
