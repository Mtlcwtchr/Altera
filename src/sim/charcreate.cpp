#include "sim/charcreate.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include "sim/ai.hpp"
#include "sim/backstory.hpp"
#include "sim/building.hpp"
#include "sim/loc.hpp"
#include "sim/species.hpp"

namespace lr::sim {

namespace {

const Json& cfgOf(const Content& c) { return c.charCreation; }

double jnum(const Json& j, const char* k, double d) { return j.is_object() && j.contains(k) && j[k].is_number() ? j[k].get<double>() : d; }

std::vector<std::string> jstrs(const Json& j, const char* k) {
    std::vector<std::string> out;
    if (j.is_object() && j.contains(k) && j[k].is_array())
        for (const auto& s : j[k])
            if (s.is_string()) out.push_back(s.get<std::string>());
    return out;
}

std::map<std::string, int> costs(const Json& cfg, const char* k) {
    std::map<std::string, int> out;
    if (cfg.contains(k) && cfg[k].is_object())
        for (auto it = cfg[k].begin(); it != cfg[k].end(); ++it)
            if (it.value().is_number()) out[it.key()] = it.value().get<int>();
    return out;
}

std::vector<std::string> playableRaces(const Content& c) {
    std::vector<std::string> out;
    for (const auto& r : jstrs(cfgOf(c), "races"))
        if (c.peoples.races.count(r)) out.push_back(r);
    if (out.empty()) out.push_back(c.peoples.defaultRace);
    return out;
}

std::string raceOrDefault(const Content& c, const std::string& r) { return r.empty() ? c.peoples.defaultRace : r; }

int giftCost(const Content& c, const std::string& race) {
    const Json g = cfgOf(c).value("gift", Json::object());
    const double base = jnum(g, "base", 6), baseChance = jnum(g, "base_chance", 0.05);
    const RaceDef* r = findRace(c, raceOrDefault(c, race));
    const double chance = r ? std::max(1e-3, r->giftedChance) : baseChance;
    const double v = std::round(base * std::sqrt(baseChance / chance));
    return int(std::clamp(v, jnum(g, "min", 1), jnum(g, "max", 99)));
}

std::string giftTrait(const Content& c) { return cfgOf(c).value("gift", Json::object()).value("trait", std::string("gifted")); }

bool exclusive(const Content& c, const std::string& a, const std::string& b) {
    for (const auto& [x, y] : c.exclusiveTraits)
        if ((x == a && y == b) || (x == b && y == a)) return true;
    return false;
}

std::vector<std::string> attrIds(const Content& c) {
    std::vector<std::string> a = c.expansion.attributes;
    if (a.empty()) a = {"strength", "agility", "dexterity", "endurance", "intellect", "perception", "willpower", "charisma"};
    return a;
}

// the look's options for a kind and sex: key -> allowed values
std::map<std::string, std::vector<std::string>> lookOptions(const Content& c, const std::string& race, char sex) {
    const Json L = cfgOf(c).value("look", Json::object());
    std::map<std::string, std::vector<std::string>> o;
    const Json body = L.value("body", Json::object());
    o["body"] = jstrs(body, sex == 'f' ? "f" : "m");
    o["hair"] = jstrs(L, "hair");
    o["hair_colour"] = jstrs(L, "hair_colours");
    o["cloth"] = jstrs(L, "cloth");
    if (sex != 'f') o["beard"] = jstrs(L, "beard");
    // the skin: the kind's own list, else shades of its one colour, else the human list
    const std::string rid = raceOrDefault(c, race);
    const Json skins = L.value("skins", Json::object());
    std::vector<std::string> sk = jstrs(skins, rid.c_str());
    if (sk.empty()) {
        const RaceDef* r = findRace(c, rid);
        if (r && !r->skin.empty() && r->skin.size() == 7 && r->skin[0] == '#') {
            const unsigned v = unsigned(std::stoul(r->skin.substr(1), nullptr, 16));
            for (const auto& sh : L.value("skin_shades", Json::array({1.0}))) {
                const double k = sh.is_number() ? sh.get<double>() : 1.0;
                auto ch = [&](int shift) { return int(std::clamp(std::round(((v >> shift) & 0xff) * k), 0.0, 255.0)); };
                char buf[8];
                std::snprintf(buf, sizeof buf, "#%02x%02x%02x", ch(16), ch(8), ch(0));
                sk.push_back(buf);
            }
        } else sk = jstrs(skins, c.peoples.defaultRace.c_str());
    }
    o["skin"] = sk;
    return o;
}

} // namespace

Json creationRules(const Content& c, const std::string& culture) {
    const Json& cfg = cfgOf(c);
    Json out = Json::object();
    out["points"] = int(jnum(cfg, "points", 10));
    out["age"] = cfg.value("age", Json{{"min", 16}, {"max", 60}, {"default", 24}});
    Json attrs = cfg.value("attrs", Json::object());
    attrs["ids"] = attrIds(c);
    out["attrs"] = attrs;
    out["max"] = cfg.value("max", Json::object());
    out["gift"] = {{"trait", giftTrait(c)}};
    Json races = Json::array();
    for (const auto& id : playableRaces(c)) {
        const RaceDef* r = findRace(c, id);
        Json e = {{"id", id}, {"gift_cost", giftCost(c, id)}, {"welcome", cultureWelcome(c.peoples, culture, id)}};
        if (r) {
            e["skin"] = r->skin;
            e["adult"] = r->adultYears;
            e["elder"] = r->elderYears;
            e["lifespan"] = r->lifespanYears;
            e["px"] = r->spritePx;
            if (!r->pawn.empty()) e["pawn"] = r->pawn;   // LR-139: ears, head, tusks... for the preview
            e["gifted_chance"] = r->giftedChance;
            // the kind's strongest and weakest aptitudes, for the card
            std::vector<std::pair<std::string, RaceAptitude>> ap(r->aptitudes.begin(), r->aptitudes.end());
            std::stable_sort(ap.begin(), ap.end(), [](const auto& a, const auto& b) { return a.second.learn > b.second.learn; });
            Json apj = Json::array();
            for (const auto& [sk, a] : ap)
                if (sk != "*") apj.push_back({{"skill", sk}, {"learn", a.learn}, {"cap", a.cap}});
            e["aptitudes"] = apj;
            e["affinity"] = r->affinity;
        }
        Json looks = Json::object();
        for (char sx : {'m', 'f'}) looks[std::string(1, sx)] = lookOptions(c, id, sx);
        e["look"] = looks;
        races.push_back(e);
    }
    out["races"] = races;
    Json talents = Json::array(), flaws = Json::array();
    for (const auto& [id, n] : costs(cfg, "talents")) talents.push_back({{"id", id}, {"cost", n}});
    for (const auto& [id, n] : costs(cfg, "flaws")) flaws.push_back({{"id", id}, {"cost", n}});
    out["talents"] = talents;
    out["flaws"] = flaws;
    out["nature"] = jstrs(cfg, "nature");
    Json ex = Json::array();
    for (const auto& [a, b] : c.exclusiveTraits) ex.push_back({a, b});
    out["exclusive"] = ex;
    return out;
}

int creationCost(const Content& c, const Json& ch, std::string* why) {
    auto fail = [&](const std::string& w) {
        if (why) *why = w;
        return -1;
    };
    const Json& cfg = cfgOf(c);
    if (!ch.is_object()) return fail("bad_choice");
    // the kind
    const std::string race = raceOrDefault(c, ch.value("race", std::string()));
    const auto pr = playableRaces(c);
    if (std::find(pr.begin(), pr.end(), race) == pr.end()) return fail("race_not_playable");
    const std::string sex = ch.value("sex", std::string("m"));
    if (sex != "m" && sex != "f") return fail("bad_sex");
    // the age (human measure)
    const Json age = cfg.value("age", Json::object());
    if (ch.contains("age")) {
        const double a = ch["age"].is_number() ? ch["age"].get<double>() : -1;
        if (a < jnum(age, "min", 16) || a > jnum(age, "max", 60)) return fail("bad_age");
    }
    // the backstory
    const std::string back = ch.value("backstory", std::string());
    const BackstoryDef* b = back.empty() ? nullptr : c.backstory(back);
    if (!back.empty() && !b) return fail("unknown_backstory");
    // the traits
    int spent = 0;
    const auto talents = costs(cfg, "talents"), flaws = costs(cfg, "flaws");
    const auto nature = jstrs(cfg, "nature");
    const Json mx = cfg.value("max", Json::object());
    const std::string gift = giftTrait(c);
    int nt = 0, nf = 0, nn = 0;
    std::vector<std::string> picked;
    if (ch.contains("traits") && !ch["traits"].is_array()) return fail("bad_traits");
    for (const auto& t : ch.value("traits", Json::array())) {
        if (!t.is_string()) return fail("bad_traits");
        const std::string id = t.get<std::string>();
        if (std::find(picked.begin(), picked.end(), id) != picked.end()) return fail("trait_twice:" + id);
        if (b && std::find(b->traitsAlways.begin(), b->traitsAlways.end(), id) != b->traitsAlways.end())
            return fail("trait_from_backstory:" + id);
        if (id == gift) spent += giftCost(c, race);
        else if (talents.count(id)) spent += talents.at(id), ++nt;
        else if (flaws.count(id)) spent += flaws.at(id), ++nf;
        else if (std::find(nature.begin(), nature.end(), id) != nature.end()) ++nn;
        else return fail("trait_not_offered:" + id);
        for (const auto& o : picked)
            if (exclusive(c, o, id)) return fail("traits_exclusive:" + o + "," + id);
        if (b)
            for (const auto& o : b->traitsAlways)
                if (exclusive(c, o, id)) return fail("traits_exclusive:" + o + "," + id);
        picked.push_back(id);
    }
    if (nt > int(jnum(mx, "talents", 3))) return fail("too_many_talents");
    if (nf > int(jnum(mx, "flaws", 3))) return fail("too_many_flaws");
    if (nn > int(jnum(mx, "nature", 3))) return fail("too_many_nature");
    // the attributes
    const Json A = cfg.value("attrs", Json::object());
    const double base = jnum(A, "base", 0.5), lo = jnum(A, "min", 0.3), hi = jnum(A, "max", 0.8), step = std::max(0.01, jnum(A, "step", 0.05));
    const int cost = int(jnum(A, "cost", 1)), refund = int(jnum(A, "refund", 1));
    const auto ids = attrIds(c);
    if (ch.contains("attrs") && !ch["attrs"].is_object()) return fail("bad_attrs");
    const Json attrsChosen = ch.value("attrs", Json::object());
    for (const auto& [k, v] : attrsChosen.items()) {
        if (std::find(ids.begin(), ids.end(), k) == ids.end()) return fail("unknown_attr:" + k);
        if (!v.is_number()) return fail("bad_attrs");
        const double x = v.get<double>();
        if (x < lo - 1e-6 || x > hi + 1e-6) return fail("attr_out_of_range:" + k);
        const int steps = int(std::lround((x - base) / step));
        spent += steps > 0 ? steps * cost : steps * refund;
    }
    // the look
    if (ch.contains("look")) {
        if (!ch["look"].is_object()) return fail("bad_look");
        const auto opts = lookOptions(c, race, sex[0]);
        for (const auto& [k, v] : ch["look"].items()) {
            const auto o = opts.find(k);
            if (o == opts.end() || !v.is_string()) return fail("bad_look:" + k);
            if (std::find(o->second.begin(), o->second.end(), v.get<std::string>()) == o->second.end()) return fail("bad_look:" + k);
        }
    }
    if (ch.contains("name") && (!ch["name"].is_string() || ch["name"].get<std::string>().size() > 64)) return fail("bad_name");
    const int points = int(jnum(cfg, "points", 10));
    if (spent > points) return fail("over_points");
    return spent;
}

EntityId creationCandidate(const World& w, char sex) {
    std::set<EntityId> chiefs;
    for (const auto& [id, s] : w.settlements()) chiefs.insert(s.chief);
    EntityId head = kNone, spouse = kNone, adult = kNone;
    for (const auto& [id, c] : w.characters()) {
        if (c.isAnimal() || !c.body.alive || c.body.stage() == "child" || c.controller.isPlayer() || c.body.sex != sex) continue;
        if (c.hasTrait("undead")) continue;
        // only the people of the map's village (the fully simulated one)
        const auto st = w.settlements().find(c.status.settlement);
        if (st == w.settlements().end() || !st->second.full()) continue;
        if (chiefs.count(id)) continue;
        if (adult == kNone) adult = id;
        if (head == kNone && c.status.role == "head") head = id;
        if (spouse == kNone && c.status.role == "spouse") spouse = id;
    }
    if (head != kNone) return head;
    if (spouse != kNone) return spouse;
    if (adult != kNone) return adult;
    return backstoryCandidate(w);
}

bool applyCreation(World& w, Character& c, const Json& ch, std::string& why) {
    const Content& ct = w.content();
    if (creationCost(ct, ch, &why) < 0) return false;
    const std::string race = raceOrDefault(ct, ch.value("race", std::string()));
    const std::string back = ch.value("backstory", std::string());
    const BackstoryDef* b = back.empty() ? nullptr : ct.backstory(back);

    // the household takes the kind (a household is of one kind, AL-136); its years in the
    // human measure stay as they were
    if (ct.peoples.loaded) {
        std::vector<EntityId> who{c.id};
        if (const Family* f = w.familyOf(c))
            for (EntityId m : f->members)
                if (m != c.id) who.push_back(m);
        for (EntityId id : who) {
            Character* p = w.character(id);
            if (!p || p->isAnimal() || !p->body.alive) continue;
            const double hy = p->body.lifeYears() + double(p->body.ageDays % kDaysPerYear) / kDaysPerYear;
            setRace(ct, *p, race);
            if (const RaceDef* r = findRace(ct, race)) p->body.ageDays = raceAgeDays(*r, hy);
        }
    }
    if (ch.contains("age") && ch["age"].is_number()) {
        if (const RaceDef* r = findRace(ct, race)) c.body.ageDays = raceAgeDays(*r, ch["age"].get<double>());
        else c.body.ageDays = int(ch["age"].get<double>() * kDaysPerYear);
    }

    // the backstory reshapes skills, things, place, rights and how the village sees them
    if (b && !applyBackstory(w, c, *b, why)) return false;

    // the traits: the backstory's fixed ones, then the choice (the gift only if chosen)
    std::vector<std::string> traits = b ? b->traitsAlways : std::vector<std::string>{};
    for (const auto& t : ch.value("traits", Json::array()))
        if (std::find(traits.begin(), traits.end(), t.get<std::string>()) == traits.end()) traits.push_back(t.get<std::string>());
    c.traits = traits;

    // the attributes: the chosen value, the backstory's shift on top
    const Json A = cfgOf(ct).value("attrs", Json::object());
    const double base = jnum(A, "base", 0.5);
    const Json chosen = ch.value("attrs", Json::object());
    for (const auto& a : attrIds(ct)) {
        double v = chosen.contains(a) ? chosen[a].get<double>() : base;
        if (b)
            if (auto s = b->attrs.find(a); s != b->attrs.end()) v += s->second;
        c.attrs[a] = std::clamp(v, ct.backstoryCfg.attrLo, ct.backstoryCfg.attrHi);
    }

    // the look and the name
    c.look.clear();
    const Json lookChosen = ch.value("look", Json::object());
    for (const auto& [k, v] : lookChosen.items()) c.look[k] = v.get<std::string>();
    std::string name = ch.value("name", std::string());
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    if (!name.empty()) {
        Json forms = Json::object();
        for (const char* k : {"nom", "gen", "dat", "acc", "ins", "pre"}) forms[k] = name;
        setOwnName(c, ch.value("lang", std::string("ru")), forms);
    }

    // a kind the village's culture takes badly: a stranger to the village (not to the household)
    const auto st = w.settlements().find(c.status.settlement);
    const std::string culture = st != w.settlements().end() ? st->second.culture : std::string();
    const double below = jnum(cfgOf(ct), "welcome_below", 0.6);
    const double welcome = cultureWelcome(ct.peoples, culture, race);
    if (below > 0.0 && welcome < below) {
        const double k = (below - welcome) / below;
        const Json rel = cfgOf(ct).value("outsider", Json::object()).value("relations", Json::object()).value("village", Json::object());
        for (auto& [id, o] : w.characters()) {
            if (id == c.id || o.isAnimal() || !o.body.alive || o.status.settlement != c.status.settlement) continue;
            if (o.status.family != kNone && o.status.family == c.status.family) continue;
            for (const auto& [ax, v] : rel.items()) {
                if (!v.is_number()) continue;
                for (Relation* rl : {&c.relations[id], &o.relations[c.id]})
                    if (double* p = rl->axis(ax)) *p = std::clamp(*p + v.get<double>() * k, -1.0, 1.0);
            }
        }
    }
    return true;
}

bool handleCreationCommand(World& w, Command& cmd, int player) {
    if (cmd.type != "create_character") return false;
    const std::string sex = cmd.args.value("sex", std::string("m"));
    const EntityId who = cmd.args.value("id", kNone) != kNone ? cmd.args.value("id", kNone) : creationCandidate(w, sex == "f" ? 'f' : 'm');
    Character* c = w.character(who);
    if (!c) return cmd.result = "rejected:nobody", true;
    if (c->body.sex != (sex == "f" ? 'f' : 'm')) return cmd.result = "rejected:sex", true;
    std::string why;
    if (!applyCreation(w, *c, cmd.args, why) || !switchControl(w, player, who, why)) cmd.result = "rejected:" + why;
    return true;
}

void validateCreation(const Content& c, ValidationReport& r) {
    const Json& cfg = cfgOf(c);
    if (!cfg.is_object() || cfg.empty()) return;
    for (const auto& id : jstrs(cfg, "races"))
        if (c.peoples.loaded && !c.peoples.races.count(id)) r.errors.push_back("character_creation: unknown race '" + id + "'");
    auto trait = [&](const std::string& where, const std::string& id) {
        if (!c.traits.empty() && !c.traits.count(id)) r.errors.push_back("character_creation " + where + ": unknown trait '" + id + "'");
    };
    for (const auto& [id, n] : costs(cfg, "talents")) {
        trait("talents", id);
        if (n <= 0) r.errors.push_back("character_creation talents: '" + id + "' must cost points");
    }
    for (const auto& [id, n] : costs(cfg, "flaws")) {
        trait("flaws", id);
        if (n >= 0) r.errors.push_back("character_creation flaws: '" + id + "' must give points back");
    }
    for (const auto& id : jstrs(cfg, "nature")) trait("nature", id);
    trait("gift", giftTrait(c));
    const Json A = cfg.value("attrs", Json::object());
    if (!(jnum(A, "min", 0.3) <= jnum(A, "base", 0.5) && jnum(A, "base", 0.5) <= jnum(A, "max", 0.8)))
        r.errors.push_back("character_creation attrs: need min <= base <= max");
}

} // namespace lr::sim
