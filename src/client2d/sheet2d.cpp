#include "client2d/sheet2d.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include "sim/ai.hpp"
#include "sim/captivity.hpp"
#include "sim/culture.hpp"
#include "sim/gear.hpp"
#include "sim/trade.hpp"

namespace lr::client2d {

using namespace lr::sim;

namespace {

double r2(double v) { return std::round(v * 100) / 100; }

std::string hhmm(Tick t) {
    const int m = static_cast<int>(t % kTicksPerDay) * 10;
    char b[16];
    std::snprintf(b, sizeof b, "%02d:%02d", m / 60, m % 60);
    return b;
}

// a key's text when the language has it, else `fallback`
std::string textOr(const ViewText& t, const std::string& key, const std::string& fallback) {
    if (t.loc && t.loc->has(t.lang, key)) return t.loc->text(t.lang, key);
    return fallback;
}

std::string nameOf(const ViewText& t, const Character& c) {
    if (c.isAnimal()) return t.name("species", c.species);
    if (!t.loc) return c.name;
    const Json n = t.loc->personNoun(t.lang, c);
    return n.is_object() ? n.value("nom", c.name) : c.name;
}

std::string goalName(const ViewText& t, const Character& c) {
    if (!c.ai.goal.valid()) return "";
    if (c.ai.goal.id == "player_action") return t.name("interaction", c.ai.goal.task);
    return t.name("goal", c.ai.goal.id);
}

std::string actName(const ViewText& t, const Character& c) {
    if (!c.body.alive) return "";
    const std::string& ia = c.ai.fsm.interaction;
    if (!ia.empty() && ia != "goto") return t.name("interaction", ia);
    return "";
}

// How `other` is kin to `c` (ui2d.sheet.kin.* keys): spouse, mother, father, child, sibling.
std::vector<std::string> kinOf(const World& w, const Character& c, const Character& o) {
    std::vector<std::string> k;
    const Status& s = c.status;
    if (s.spouse == o.id) k.push_back("spouse");
    if (s.mother == o.id) k.push_back("mother");
    if (s.father == o.id) k.push_back("father");
    if (std::find(s.children.begin(), s.children.end(), o.id) != s.children.end() || o.status.mother == c.id ||
        o.status.father == c.id)
        k.push_back("child");
    const bool sameMother = s.mother != kNone && s.mother == o.status.mother;
    const bool sameFather = s.father != kNone && s.father == o.status.father;
    if (o.id != c.id && (sameMother || sameFather)) k.push_back("sibling");
    if (k.empty() && s.family != kNone && s.family == o.status.family) k.push_back("household");
    (void)w;
    return k;
}

Json axesJson(const Relation& r) {
    Json a = Json::object();
    for (const char* ax : {"familiarity", "trust", "affinity", "fear", "debt", "enmity", "respect", "attraction"})
        a[ax] = r2(r.get(ax));
    return a;
}

// all | family | friends | foes | known (the page's filters)
std::string groupOf(const std::vector<std::string>& kin, const Relation& r) {
    if (!kin.empty()) return "family";
    if (r.enmity >= 0.3 || r.affinity <= -0.3 || r.fear >= 0.5) return "foes";
    if (r.affinity >= 0.3 || (r.trust >= 0.4 && r.affinity >= 0.15)) return "friends";
    return "known";
}

Json memJson(const World& w, const ViewText& t, const Character& c, const MemoryEntry& m) {
    return {{"day", m.tick / kTicksPerDay},
            {"t", hhmm(m.tick)},
            {"text", memoryText(w, t, c, m)},
            {"type", m.type},
            {"v", r2(m.valence)},
            {"sal", r2(m.salience)},
            {"src", m.source},
            {"conf", r2(m.confidence)},
            {"who", m.who}};
}

} // namespace

std::string memoryText(const World& w, const ViewText& t, const Character& self, const MemoryEntry& m) {
    const std::string key = "memory." + m.type + ".text";
    if (!t.loc || !t.loc->has(t.lang, key)) return t.name("memory", m.type);
    Json p = Json::object();
    if (!m.who.empty()) {
        p["who"] = m.who[0];
        p["target"] = m.who.size() > 1 ? m.who[1] : m.who[0];
    }
    if (m.object != kNone) p["object"] = m.object;
    if (!m.goods.empty()) p["item"] = m.goods.begin()->first;
    return t.loc->text(t.lang, key, locArgs(*t.loc, t.lang, w, p, &self));
}

Json sheetJson(const World& w, const ViewText& t, EntityId id) {
    const Character* cp = w.character(id);
    if (!cp) return nullptr;
    const Character& c = *cp;
    const Content& ct = w.content();
    const Tick now = w.now();
    const Family* fam = w.familyOf(c);

    Json d = {{"id", id},
              {"name", nameOf(t, c)},
              {"sp", c.species},
              {"alive", c.body.alive},
              {"cause", c.body.causeOfDeath.empty() ? "" : t.name("cause", c.body.causeOfDeath)},
              {"sex", std::string(1, c.body.sex)},
              {"age", c.body.ageDays / kDaysPerYear},
              {"stage", textOr(t, "life_stage." + c.body.stage() + ".name", c.body.stage())},
              {"fam", !fam ? "" : t.loc ? t.loc->personNoun(t.lang, fam->name, 'm').value("nom", fam->name) : fam->name},   // LR-137: shown
              {"house", c.standing.house},
              {"role", textOr(t, "ui2d.sheet.role." + c.status.role + "." + std::string(1, c.body.sex),
                              textOr(t, "household_role." + c.status.role + ".name", c.status.role))},
              {"standing", c.standing.role.empty() ? "" : textOr(t, "ui2d.sheet.standing." + c.standing.role, c.standing.role)},
              {"life", textOr(t, "ui2d.sheet.life." + c.standing.life, c.standing.life)},
              {"occ", c.occupation.empty() ? "" : textOr(t, "ui2d.sheet.occ." + c.occupation, c.occupation)},
              {"backstory", c.backstory.empty() ? "" : t.name("backstory", c.backstory)},
              {"pl", c.controller.isPlayer()},
              {"lead", mayLead(w, 1, c)}}; // LR-138: only one's own kin
    if (!c.isAnimal()) {   // LR-155: the person's culture and faith (the window: /api/culture)
        const std::string cu = cultureOf(w, c), fa = faithOf(w, c);
        d["belief"] = {{"culture", cu}, {"cultureName", t.name("culture", cu)}, {"bond", r2(cultureBondOf(w, c))},
                       {"faith", fa}, {"faithName", t.name("faith", fa)}, {"piety", r2(pietyOf(w, c))}};
    }

    // --- overview -----------------------------------------------------------------
    Json ov;
    ov["hp"] = r2(c.body.health);
    ov["fatigue"] = r2(c.body.fatigue);
    ov["blood"] = r2(c.body.blood / 100.0);
    ov["mood"] = r2(c.mood(now));
    Json moodlets = Json::array();
    for (const Moodlet& m : c.moodlets) {
        if (m.until != 0 && m.until <= now) continue;
        moodlets.push_back({{"id", m.id},
                            {"name", t.name("moodlet", m.id)},
                            {"v", r2(m.value)},
                            {"h", m.until == 0 ? -1.0 : std::round(double(m.until - now) / (kTicksPerDay / 24.0) * 10) / 10}});
    }
    std::sort(moodlets.begin(), moodlets.end(), [](const Json& a, const Json& b) { return a["v"].get<double>() > b["v"].get<double>(); });
    ov["moodlets"] = moodlets;
    Json needs, tiers;
    needsJson(w, t, c, needs, tiers, true); // LR-140: all needs with their tier
    ov["needs"] = needs;
    ov["tiers"] = tiers;
    ov["goal"] = goalName(t, c);
    ov["act"] = actName(t, c);
    Json plan = Json::array();
    for (std::size_t i = 0; i < c.ai.plan.actions.size(); ++i) {
        const PlanAction& a = c.ai.plan.actions[i];
        if (i < c.ai.plan.actionIndex) continue;
        if (a.interaction == "goto") continue;
        plan.push_back({{"name", t.name("interaction", a.interaction)}, {"now", i == c.ai.plan.actionIndex}});
        if (plan.size() >= 6) break;
    }
    ov["plan"] = plan;
    Json wounds = Json::array();
    for (const Wound& wd : c.wounds)
        wounds.push_back({{"zone", textOr(t, "ui2d.sheet.zone." + wd.zone, wd.zone)},
                          {"sev", textOr(t, "ui2d.sheet.sev." + wd.severity, wd.severity)},
                          {"days", std::round(wd.daysLeft * 10) / 10},
                          {"bleeding", wd.bleeding},
                          {"infected", wd.infected}});
    ov["wounds"] = wounds;
    Json injuries = Json::array();
    for (const Injury& in : c.injuries)
        injuries.push_back({{"zone", textOr(t, "ui2d.sheet.zone." + in.zone, in.zone)}, {"kind", textOr(t, "ui2d.sheet.injury." + in.kind, in.kind)}});
    ov["injuries"] = injuries;
    ov["captive"] = c.captivity.held();
    d["overview"] = ov;

    // --- relations ------------------------------------------------------------------
    // everyone the person has a relation to, and the kin even without one
    std::set<EntityId> others;
    for (const auto& [oid, r] : c.relations) others.insert(oid);
    for (EntityId k : {c.status.spouse, c.status.mother, c.status.father})
        if (k != kNone) others.insert(k);
    for (EntityId k : c.status.children) others.insert(k);
    if (c.status.family != kNone)
        for (const auto& [oid, o] : w.characters())
            if (!o.isAnimal() && o.status.family == c.status.family) others.insert(oid);
    others.erase(id);
    Json rels = Json::array();
    for (EntityId oid : others) {
        const Character* o = w.character(oid);
        if (!o || o->isAnimal()) continue;
        static const Relation kNoRel{};
        auto rit = c.relations.find(oid);
        const Relation& r = rit == c.relations.end() ? kNoRel : rit->second;
        const auto kin = kinOf(w, c, *o);
        Json kinNames = Json::array();
        for (const auto& k : kin) kinNames.push_back(textOr(t, "ui2d.sheet.kin." + k + "." + std::string(1, o->body.sex), k));
        Json pairs = Json::array();
        if (auto pit = c.status.pairs.find(oid); pit != c.status.pairs.end())
            for (const auto& p : pit->second) pairs.push_back(t.name("status_pair", p));
        Json traits = Json::array();
        for (const auto& tr : o->traits) traits.push_back(t.name("trait", tr));
        Json mem = Json::array();
        for (std::size_t i = c.memory.size(); i-- > 0 && mem.size() < 8;) {
            const MemoryEntry& m = c.memory[i];
            if (std::find(m.who.begin(), m.who.end(), oid) != m.who.end()) mem.push_back(memJson(w, t, c, m));
        }
        const Family* of = w.familyOf(*o);
        rels.push_back({{"id", oid},
                        {"name", nameOf(t, *o)},
                        {"fam", of ? of->name : ""},
                        {"alive", o->body.alive},
                        {"age", o->body.ageDays / kDaysPerYear},
                        {"sex", std::string(1, o->body.sex)},
                        {"kin", kinNames},
                        {"pairs", pairs},
                        {"group", groupOf(kin, r)},
                        {"axes", axesJson(r)},
                        {"traits", traits},
                        {"mem", mem},
                        {"act", actName(t, *o)},
                        {"goal", goalName(t, *o)}});
    }
    // kin first, then by how well they are known
    std::stable_sort(rels.begin(), rels.end(), [](const Json& a, const Json& b) {
        const bool ka = a["group"] == "family", kb = b["group"] == "family";
        if (ka != kb) return ka;
        return a["axes"]["familiarity"].get<double>() > b["axes"]["familiarity"].get<double>();
    });
    d["relations"] = rels;

    // --- things -------------------------------------------------------------------------
    Json th;
    const GearRules* G = gearRules(ct);
    Json worn = Json::array();
    std::map<std::string, int> wornN;
    for (const auto& [sl, item] : c.worn) {
        const auto colon = sl.find(':');
        const std::string slot = sl.substr(0, colon), layer = colon == std::string::npos ? "" : sl.substr(colon + 1);
        worn.push_back({{"slot", slot},
                        {"slotName", textOr(t, "hud.gear.slot." + slot, slot)},
                        {"layer", layer},
                        {"layerName", layer.empty() ? "" : textOr(t, "hud.gear.layer." + layer, layer)},
                        {"id", item},
                        {"name", t.name("item", item)},
                        {"kg", G ? r2(G->item(item).kg) : 0.0}});
        ++wornN[item];
    }
    th["worn"] = worn;
    Json slots = Json::array();   // every body place, worn or not (the doll)
    if (G)
        for (const GearSlot& gs : G->slots) {
            Json layers = Json::array();   // LR-145: inner to outer, for the sheet's layer grid
            for (const auto& l : gs.layers) layers.push_back({{"id", l}, {"name", textOr(t, "hud.gear.layer." + l, l)}});
            slots.push_back({{"id", gs.id}, {"name", textOr(t, "hud.gear.slot." + gs.id, gs.id)}, {"layers", layers}});
        }
    th["slots"] = slots;
    Json carried = Json::array();
    for (const auto& [item, n] : c.inventory) {
        const int left = n - wornN[item];
        if (left <= 0) continue;
        carried.push_back({{"id", item},
                           {"name", t.name("item", item)},
                           {"n", left},
                           {"kg", G ? r2(G->item(item).kg) : 0.0},
                           {"worth", r2(baseValue(ct, item) * itemWorthMult(ct, c, item))}});
    }
    th["carried"] = carried;
    // LR-145: what is carried where: the hands' grid, each worn container's (belt, satchel,
    // pack) and what is hauled on the shoulder, as sim/gear arrange() lays them out
    Json carry = Json::array();
    if (G) {
        const GearLayout L = arrange(*G, c);
        for (const GearGrid& g : L.grids) {
            const bool hands = g.id == "hands";
            const std::string slot = hands ? "hands" : G->item(g.id).slot;
            Json items = Json::array();
            int used = 0;
            double kg = 0;
            std::map<std::string, int> n;
            std::vector<std::string> order;
            for (const GearSpot& sp : L.spots)
                if (sp.grid == g.id) {
                    used += sp.w * sp.h;
                    if (!n.count(sp.item)) order.push_back(sp.item);
                    n[sp.item] += sp.count;
                }
            for (const auto& it : order) {
                kg += G->item(it).kg * n[it];
                items.push_back({{"id", it}, {"name", t.name("item", it)}, {"n", n[it]}});
            }
            carry.push_back({{"id", g.id},
                             {"slot", slot},
                             {"name", hands ? textOr(t, "hud.gear.hands", "hands") : t.name("item", g.id)},
                             {"cells", g.w * g.h},
                             {"used", used},
                             {"kg", r2(kg)},
                             {"items", items}});
        }
        if (!L.hauled.empty()) {
            Json items = Json::array();
            double kg = 0;
            for (const auto& [it, k] : L.hauled) {
                kg += G->item(it).kg * k;
                items.push_back({{"id", it}, {"name", t.name("item", it)}, {"n", k}});
            }
            carry.push_back({{"id", "haul"}, {"slot", "haul"}, {"name", textOr(t, "hud.gear.haul", "on the shoulder")}, {"cells", 0}, {"used", 0},
                             {"kg", r2(kg)}, {"items", items}});
        }
        if (!L.overflow.empty()) {
            Json items = Json::array();
            for (const auto& [it, k] : L.overflow) items.push_back({{"id", it}, {"name", t.name("item", it)}, {"n", k}});
            carry.push_back({{"id", "overflow"}, {"slot", "overflow"}, {"name", textOr(t, "hud.gear.overflow", "no room")}, {"cells", 0},
                             {"used", 0}, {"kg", 0.0}, {"items", items}});
        }
    }
    th["carry"] = carry;
    th["kg"] = G ? r2(loadKg(*G, c)) : 0.0;
    th["maxKg"] = G ? r2(maxLoadKg(*G, c)) : 0.0;
    d["things"] = th;

    // --- skills and traits ----------------------------------------------------------------
    Json sk;
    Json skills = Json::array();
    for (const auto& [kid, v] : c.skills) {
        std::string cat;
        if (auto it = ct.expansion.knowledgeExt.find(kid); it != ct.expansion.knowledgeExt.end() && it->second.is_object())
            cat = it->second.value("category", "");
        skills.push_back({{"id", kid},
                          {"name", t.name("knowledge", kid)},
                          {"cat", cat},
                          {"catName", cat.empty() ? "" : t.name("skill_category", cat)},
                          {"v", r2(v)}});
    }
    std::stable_sort(skills.begin(), skills.end(), [](const Json& a, const Json& b) { return a["v"].get<double>() > b["v"].get<double>(); });
    sk["skills"] = skills;
    Json traits = Json::array();
    for (const auto& tr : c.traits)
        traits.push_back({{"id", tr}, {"name", t.name("trait", tr)}, {"desc", textOr(t, "trait." + tr + ".desc", "")}});
    sk["traits"] = traits;
    Json attrs = Json::array();
    for (const auto& [aid, v] : c.attrs)
        attrs.push_back({{"id", aid}, {"name", t.name("attr", aid)}, {"desc", textOr(t, "attr." + aid + ".desc", "")}, {"v", r2(v)}});
    // LR-137: the ones never rolled (the first villagers) at the mean the sim reads for them
    for (const auto& aid : ct.expansion.attributes)
        if (!c.attrs.count(aid))
            attrs.push_back({{"id", aid}, {"name", t.name("attr", aid)}, {"desc", textOr(t, "attr." + aid + ".desc", "")}, {"v", 0.5}, {"base", true}});
    sk["attrs"] = attrs;
    Json knows = Json::array();
    for (const auto& k : c.knowledge)
        if (!c.skills.count(k)) knows.push_back({{"id", k}, {"name", t.name("knowledge", k)}});
    sk["knows"] = knows;
    Json rep = Json::array();
    for (const auto& [ax, v] : c.reputation) rep.push_back({{"id", ax}, {"name", t.name("reputation", ax)}, {"v", r2(v)}});
    sk["rep"] = rep;
    Json values = Json::array();
    const Standing& s = c.standing;
    // the axes are the person's own only once rolled (else the realm reads them from the traits)
    if (s.drawn)
        for (const auto& [ax, v] : std::vector<std::pair<std::string, double>>{
             {"martial", s.martial}, {"ambition", s.ambition}, {"greed", s.greed}, {"honor", s.honor}, {"wanderlust", s.wanderlust}})
            values.push_back({{"id", ax}, {"name", textOr(t, "ui2d.sheet.value." + ax, ax)}, {"v", r2(v)}});
    sk["values"] = values;
    sk["renown"] = r2(s.renown);
    sk["purse"] = r2(s.purse);
    d["skills"] = sk;

    // --- chronicle: what the person remembers, and what the village saw of them ---------------------
    Json ch;
    Json mem = Json::array();
    for (std::size_t i = c.memory.size(); i-- > 0 && mem.size() < 120;) mem.push_back(memJson(w, t, c, c.memory[i]));
    ch["mem"] = mem;
    Json log = Json::array();
    const auto& L = w.chronicleLog();
    for (std::size_t i = L.size(); i-- > 0 && log.size() < 80;) {
        const ChronicleEntry& e = L[i];
        if (std::find(e.who.begin(), e.who.end(), id) == e.who.end()) continue;
        std::string text = e.code;
        if (t.loc && t.loc->has(t.lang, e.code)) text = t.loc->text(t.lang, e.code, locArgs(*t.loc, t.lang, w, e.params));
        log.push_back({{"day", e.tick / kTicksPerDay}, {"t", hhmm(e.tick)}, {"text", text}});
    }
    ch["log"] = log;
    d["chronicle"] = ch;
    return d;
}


// LR-145: the dynasty window: the viewer's dynasty, its households, every member (alive and
// dead) with how they are kin to the viewer, the heir (sim heirOf) and the house's chronicle.
Json dynastyJson(const World& w, const ViewText& t, EntityId viewer) {
    const Character* me = w.character(viewer);
    if (!me) return nullptr;
    const Family* mf = w.familyOf(*me);
    if (!mf) return nullptr;
    const EntityId dyn = mf->dynasty;
    std::set<EntityId> fams;
    for (const auto& [fid, f] : w.families())
        if (fid == mf->id || (dyn != kNone && f.dynasty == dyn)) fams.insert(fid);
    Json d = {{"me", viewer}, {"name", mf->name}};
    if (dyn != kNone)
        if (auto it = w.dynasties().find(dyn); it != w.dynasties().end()) {
            if (!it->second.name.empty()) d["name"] = it->second.name;
            if (const Character* fo = w.character(it->second.founder)) d["founder"] = nameOf(t, *fo);
        }
    Json households = Json::array();
    for (EntityId fid : fams) {
        const Family& f = w.families().at(fid);
        auto st = w.settlements().find(f.settlement);
        households.push_back({{"id", fid}, {"name", f.name}, {"where", st == w.settlements().end() ? "" : st->second.name}});
    }
    d["households"] = households;
    Json members = Json::array();
    int alive = 0, dead = 0, grown = 0, kids = 0;
    double purse = 0, renown = 0;
    std::set<EntityId> ids;
    for (const auto& [id, c] : w.characters())
        if (!c.isAnimal() && fams.count(c.status.family)) ids.insert(id);
    for (EntityId id : ids) {
        const Character& c = *w.character(id);
        const bool child = c.body.stage() == "child";
        if (c.body.alive) {
            ++alive;
            (child ? kids : grown)++;
            purse += c.standing.purse;
            renown = std::max(renown, c.standing.renown);
        } else ++dead;
        Json kin = Json::array();
        if (id != viewer)
            for (const auto& k : kinOf(w, *me, c)) kin.push_back(textOr(t, "ui2d.sheet.kin." + k + "." + std::string(1, c.body.sex), k));
        const Family* f = w.familyOf(c);
        members.push_back({{"id", id},
                           {"name", nameOf(t, c)},
                           {"alive", c.body.alive},
                           {"age", r2(c.body.ageDays / kDaysPerYear)},
                           {"sex", std::string(1, c.body.sex)},
                           {"child", child},
                           {"house", f ? f->name : ""},
                           {"occ", c.occupation.empty() ? "" : textOr(t, "ui2d.sheet.occ." + c.occupation, c.occupation)},
                           {"act", actName(t, c)},
                           {"goal", goalName(t, c)},
                           {"hp", r2(c.body.health)},
                           {"pl", c.controller.isPlayer()},
                           {"spouse", c.status.spouse},
                           {"mother", c.status.mother},
                           {"father", c.status.father},
                           {"kin", kin}});
    }
    d["members"] = members;
    d["alive"] = alive;
    d["dead"] = dead;
    d["grown"] = grown;
    d["kids"] = kids;
    d["purse"] = r2(purse);
    d["renown"] = r2(renown);
    const EntityId heir = heirOf(w, *me);
    d["heir"] = heir;
    // the house's chronicle: what the village saw of its people
    Json log = Json::array();
    const auto& L = w.chronicleLog();
    for (std::size_t i = L.size(); i-- > 0 && log.size() < 60;) {
        const ChronicleEntry& e = L[i];
        bool ours = false;
        for (EntityId x : e.who) ours = ours || ids.count(x);
        if (!ours) continue;
        std::string text = e.code;
        if (t.loc && t.loc->has(t.lang, e.code)) text = t.loc->text(t.lang, e.code, locArgs(*t.loc, t.lang, w, e.params));
        log.push_back({{"day", e.tick / kTicksPerDay}, {"t", hhmm(e.tick)}, {"text", text}});
    }
    d["log"] = log;
    return d;
}

} // namespace lr::client2d
