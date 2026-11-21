// Backstories: parse, validate, apply (see backstory.hpp).

#include "sim/backstory.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

#include "sim/ai.hpp"
#include "sim/craft/craft_chain.hpp"
#include "sim/craft_sim.hpp"

namespace altera::sim {

namespace {

std::vector<std::string> strs(const Json& j, const char* key) {
    std::vector<std::string> out;
    if (j.contains(key) && j[key].is_array())
        for (const auto& v : j[key])
            if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

template <class T> std::map<std::string, T> numMap(const Json& j, const char* key) {
    std::map<std::string, T> out;
    if (j.contains(key) && j[key].is_object())
        for (auto it = j[key].begin(); it != j[key].end(); ++it)
            if (it.value().is_number()) out[it.key()] = it.value().get<T>();
    return out;
}

bool exclusive(const Content& c, const std::string& a, const std::string& b) {
    for (const auto& [x, y] : c.exclusiveTraits)
        if ((x == a && y == b) || (x == b && y == a)) return true;
    return false;
}

} // namespace

void parseBackstories(const Json& j, Content& c) {
    if (!j.is_object()) return;
    const Json cfg = j.value("config", Json::object());
    BackstoryConfig& k = c.backstoryCfg;
    k.otherSkillCap = cfg.value("other_skill_cap", k.otherSkillCap);
    k.attrSpread = cfg.value("attr_spread", k.attrSpread);
    if (cfg.contains("attr_clamp") && cfg["attr_clamp"].is_array() && cfg["attr_clamp"].size() == 2) {
        k.attrLo = cfg["attr_clamp"][0].get<double>();
        k.attrHi = cfg["attr_clamp"][1].get<double>();
    }
    if (cfg.contains("starts") && cfg["starts"].is_object())
        for (auto it = cfg["starts"].begin(); it != cfg["starts"].end(); ++it)
            k.starts[it.key()] = it.value().value("radius_m", 0.0);
    if (k.starts.empty()) k.starts["home"] = 0.0;
    for (const auto& e : j.value("backstories", Json::array())) {
        if (!e.is_object()) continue;
        BackstoryDef b;
        b.id = e.value("id", "");
        b.icon = e.value("icon", "");
        b.start = e.value("start", "home");
        if (e.contains("skills") && e["skills"].is_object())
            for (auto it = e["skills"].begin(); it != e["skills"].end(); ++it)
                if (it.value().is_number()) b.skills.emplace_back(it.key(), it.value().get<double>());
        const Json t = e.value("traits", Json::object());
        b.traitsAlways = strs(t, "always");
        b.traitPool = strs(t, "pool");
        b.traitPick = t.value("pick", 0);
        b.attrs = numMap<double>(e, "attrs");
        b.items = numMap<int>(e, "items");
        b.coins = e.value("coins", 0);
        b.recipes = strs(e, "recipes");
        for (const auto& m : e.value("made", Json::array()))
            if (m.is_object()) b.made.push_back({m.value("design", ""), m.value("q", 0.5)});
        b.rights = strs(e, "rights");
        b.reputation = numMap<double>(e, "reputation");
        if (e.contains("relations") && e["relations"].is_object())
            for (auto it = e["relations"].begin(); it != e["relations"].end(); ++it)
                b.relations[it.key()] = numMap<double>(e["relations"], it.key().c_str());
        c.backstories.push_back(std::move(b));
    }
}

void validateBackstories(const Content& c, ValidationReport& r) {
    std::set<std::string> seen;
    const auto& rel = relAxes();
    const auto& attrs = c.expansion.attributes;
    for (const BackstoryDef& b : c.backstories) {
        const std::string at = "backstories: " + (b.id.empty() ? std::string("<no id>") : b.id) + ": ";
        if (b.id.empty()) r.errors.push_back(at + "no id");
        else if (!seen.insert(b.id).second) r.errors.push_back(at + "duplicate id");
        if (b.icon.empty()) r.warnings.push_back(at + "no icon");
        if (b.skills.empty()) r.errors.push_back(at + "no skills");
        for (const auto& [k, v] : b.skills) {
            if (!c.knowledge.count(k)) r.errors.push_back(at + "unknown skill " + k);
            if (v < 0.0 || v > 1.0) r.errors.push_back(at + "skill " + k + " out of 0..1");
        }
        std::vector<std::string> all = b.traitsAlways;
        all.insert(all.end(), b.traitPool.begin(), b.traitPool.end());
        for (const auto& t : all)
            if (!c.traits.count(t)) r.errors.push_back(at + "unknown trait " + t);
        if (b.traitPick < 0 || b.traitPick > int(b.traitPool.size()))
            r.errors.push_back(at + "traits.pick exceeds the pool");
        for (std::size_t i = 0; i < b.traitsAlways.size(); ++i)
            for (std::size_t k = i + 1; k < b.traitsAlways.size(); ++k)
                if (exclusive(c, b.traitsAlways[i], b.traitsAlways[k]))
                    r.errors.push_back(at + "exclusive traits together: " + b.traitsAlways[i] + ", " + b.traitsAlways[k]);
        for (const auto& [a, v] : b.attrs) {
            if (c.expansion.loaded && std::find(attrs.begin(), attrs.end(), a) == attrs.end())
                r.errors.push_back(at + "unknown attribute " + a);
            if (std::fabs(v) > 0.5) r.errors.push_back(at + "attribute shift " + a + " beyond +-0.5");
        }
        for (const auto& [it, n] : b.items) {
            if (!c.items.count(it)) r.errors.push_back(at + "unknown item " + it);
            if (n <= 0) r.errors.push_back(at + "item " + it + " count <= 0");
        }
        if (b.coins < 0) r.errors.push_back(at + "coins < 0");
        if (b.coins > 0 && !c.items.count("coin")) r.errors.push_back(at + "coins but no item coin");
        for (const auto& d : b.recipes)
            if (c.craft.loaded && !c.craft.design(d)) r.errors.push_back(at + "unknown design " + d);
        for (const auto& m : b.made) {
            if (c.craft.loaded && !c.craft.design(m.design)) r.errors.push_back(at + "unknown made design " + m.design);
            if (m.q < 0.0 || m.q > 1.0) r.errors.push_back(at + "made " + m.design + " q out of 0..1");
        }
        if (!c.backstoryCfg.starts.count(b.start)) r.errors.push_back(at + "unknown start " + b.start);
        for (const auto& x : b.rights)
            if (std::find(c.config.rights.begin(), c.config.rights.end(), x) == c.config.rights.end())
                r.errors.push_back(at + "unknown right " + x);
        for (const auto& [ax, v] : b.reputation) {
            const auto& ra = c.config.reputationAxes;
            if (std::find(ra.begin(), ra.end(), ax) == ra.end()) r.errors.push_back(at + "unknown reputation axis " + ax);
            if (std::fabs(v) > 1.0) r.errors.push_back(at + "reputation " + ax + " beyond +-1");
        }
        for (const auto& [who, axes] : b.relations) {
            if (who != "kin" && who != "village") r.errors.push_back(at + "relations: unknown group " + who + " (kin|village)");
            for (const auto& [ax, v] : axes) {
                if (std::find(rel.begin(), rel.end(), ax) == rel.end()) r.errors.push_back(at + "relations: unknown axis " + ax);
                if (std::fabs(v) > 1.0) r.errors.push_back(at + "relations: " + ax + " beyond +-1");
            }
        }
    }
}

EntityId backstoryCandidate(const World& w) {
    std::set<EntityId> chiefs;
    for (const auto& [id, s] : w.settlements()) chiefs.insert(s.chief);
    EntityId head = kNone, adult = kNone;
    for (const auto& [id, c] : w.characters()) {
        if (c.isAnimal() || !c.body.alive || c.body.stage() != "adult" || c.controller.isPlayer()) continue;
        if (adult == kNone) adult = id;
        if (head == kNone && c.status.role == "head" && !chiefs.count(id)) head = id;
    }
    return head != kNone ? head : adult;
}

bool applyBackstory(World& w, Character& c, const BackstoryDef& b, std::string& why) {
    if (c.isAnimal()) return why = "animal", false;
    if (!c.body.alive) return why = "dead", false;
    if (c.body.stage() == "child") return why = "child", false;
    const Content& ct = w.content();
    const BackstoryConfig& k = ct.backstoryCfg;
    Rng rng = w.rng(c.id, "backstory:" + b.id);
    c.backstory = b.id;

    // skills: the backstory's own; what the person knew besides fades to the cap
    for (auto& [id, v] : c.skills) v = std::min(v, k.otherSkillCap);
    std::function<void(const std::string&)> teach = [&](const std::string& id) {
        if (c.knowledge.count(id)) return;
        auto d = ct.knowledge.find(id);
        if (d != ct.knowledge.end())
            for (const auto& req : d->second.requiresKnowledge) teach(req);
        c.knowledge.insert(id);
        if (!c.skills.count(id)) c.skills[id] = std::min(k.otherSkillCap, rng.range(0.1, 0.25));
    };
    for (const auto& [id, lvl] : b.skills) {
        teach(id);
        c.skills[id] = std::clamp(lvl + rng.range(-0.04, 0.04), 0.0, 1.0);
    }

    // traits: the fixed ones and `pick` from the pool, none exclusive with another
    c.traits = b.traitsAlways;
    std::vector<std::string> pool = b.traitPool;
    for (int n = 0; n < b.traitPick && !pool.empty();) {
        const std::size_t i = std::size_t(rng.irange(0, int(pool.size()) - 1));
        const std::string t = pool[i];
        pool.erase(pool.begin() + std::ptrdiff_t(i));
        bool ok = std::find(c.traits.begin(), c.traits.end(), t) == c.traits.end();
        for (const auto& have : c.traits) ok = ok && !exclusive(ct, have, t);
        if (ok) {
            c.traits.push_back(t);
            ++n;
        }
    }

    // attributes: the mean with a little spread, shifted by the backstory
    std::vector<std::string> attrs = ct.expansion.attributes;
    for (const auto& [a, v] : b.attrs)
        if (std::find(attrs.begin(), attrs.end(), a) == attrs.end()) attrs.push_back(a);
    for (const auto& a : attrs) {
        const auto s = b.attrs.find(a);
        const double shift = s == b.attrs.end() ? 0.0 : s->second;
        c.attrs[a] = std::clamp(0.5 + rng.range(-k.attrSpread, k.attrSpread) + shift, k.attrLo, k.attrHi);
    }

    // belongings and purse
    c.inventory.clear();
    for (const auto& [it, n] : b.items) c.inventory[it] += n;
    if (b.coins > 0) c.inventory["coin"] += b.coins;

    // recipes and own made things
    const auto& cd = ct.craft;
    if (cd.loaded) {
        for (const auto& d : b.recipes) giveDesign(w, c, d);
        for (const auto& m : b.made) {
            const craft::DesignDef* def = cd.design(m.design);
            if (!def) continue;
            craft::Actor maker;
            for (const auto& [pid, p] : cd.processes) maker.skills[p.skill] = std::max(0.3, m.q);
            craft::Workplace wp;
            for (const auto& [sid, st] : cd.stations) wp.stations.push_back(sid);
            craft::Tool tool;
            tool.tags = {"cutting", "saw", "striking", "tongs", "whetstone", "grit", "needle", "bellows"};
            tool.hardness = 0.9;
            tool.q = 0.6;
            wp.tools.push_back(tool);
            craft::Recipe r = craft::recipeFromDesign(cd, *def, 1.0, true);
            std::vector<craft::Item> pool2;
            craft::ChainOpts o;
            o.rawFromNothing = true;
            o.stopOnRisk = false;
            craft::ChainRun run = craft::runChain(cd, r.steps, pool2, maker, wp, rng.next(), 0, o);
            if (!run.done) continue;
            run.out.q = m.q;
            c.craft.items.push_back(run.out);
        }
    }

    // where they stand: round their home at the start's distance
    const double radius = k.starts.count(b.start) ? k.starts.at(b.start) : 0.0;
    if (radius > 0.0) {
        Vec2 at = c.pos;
        for (const auto& [id, s] : w.settlements())
            if (id == c.status.settlement) at = s.center;
        const double a = rng.range(0.0, 6.28318530718);
        c.pos = {at.x + std::cos(a) * radius, at.y + std::sin(a) * radius};
        const MapGrid& m = w.map();
        c.pos.x = std::clamp(c.pos.x, 5.0, m.sizeM - 5.0);
        c.pos.y = std::clamp(c.pos.y, 5.0, m.sizeM - 5.0);
    } else if (const WorldObject* h = w.object(c.home)) {
        // AL-28: "at home" is by the household's store, wherever the hours
        // before the start took the person (newGame runs the world first).
        const double a = rng.range(0.0, 6.28318530718);
        c.pos = {h->pos.x + std::cos(a) * 3.0, h->pos.y + std::sin(a) * 3.0};
        if (c.ai.fsm.state != "Idle") abortAction(w, c, "start.placed_home"); // no walk-back to the old errand
    }

    // rights, repute and how the others see them (both ways)
    for (const auto& x : b.rights) c.status.rights.insert(x);
    for (const auto& [ax, v] : b.reputation) c.reputation["reputation:" + ax] = std::clamp(v, -1.0, 1.0);
    for (auto& [id, o] : w.characters()) {
        if (id == c.id || o.isAnimal() || !o.body.alive) continue;
        const char* group = o.status.family != kNone && o.status.family == c.status.family ? "kin"
                            : o.status.settlement != kNone && o.status.settlement == c.status.settlement ? "village"
                                                                                                          : nullptr;
        if (!group) continue;
        const auto g = b.relations.find(group);
        if (g == b.relations.end()) continue;
        for (const auto& [ax, v] : g->second) {
            for (Relation* rl : {&c.relations[id], &o.relations[c.id]})
                if (double* p = rl->axis(ax)) *p = std::clamp(*p + v, -1.0, 1.0);
        }
    }
    return true;
}

bool handleBackstoryCommand(World& w, Command& cmd, int player) {
    if (cmd.type != "start_backstory") return false;
    const BackstoryDef* b = w.content().backstory(cmd.args.value("backstory", std::string()));
    if (!b) return cmd.result = "rejected:unknown_backstory", true;
    const EntityId who = cmd.args.value("id", kNone) != kNone ? cmd.args.value("id", kNone) : backstoryCandidate(w);
    Character* c = w.character(who);
    if (!c) return cmd.result = "rejected:nobody", true;
    std::string why;
    if (!applyBackstory(w, *c, *b, why) || !switchControl(w, player, who, why)) cmd.result = "rejected:" + why;
    return true;
}

} // namespace altera::sim
