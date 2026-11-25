#include "sim/craft/craft_validate.hpp"
#include "sim/craft/craft_magic.hpp"

#include <set>

#include "sim/content.hpp"

namespace lr::sim::craft {

namespace {

struct Known {
    std::set<std::string> tags;    // material ids + tags + process marks + reaction marks
    std::set<std::string> roles;
    std::set<std::string> shapeVals;
    std::set<std::string> states;
    std::set<std::string> effects;
    std::set<std::string> toolTags;
    std::set<std::string> stationTags;
};

Known known(const CraftData& d) {
    Known k;
    for (const auto& [id, m] : d.materials) {
        k.tags.insert(id);
        k.tags.insert(m.tags.begin(), m.tags.end());
        k.toolTags.insert(m.tags.begin(), m.tags.end());
        for (const auto& s : m.states) k.states.insert(s.id);
        for (const auto& h : m.heat)
            if (!h.toState.empty()) k.states.insert(h.toState);
        for (const auto& e : m.effects) k.effects.insert(e.id);
    }
    for (const auto& id : d.processOrder) {
        const ProcessDef& p = d.processes.at(id);
        k.tags.insert(p.marks.begin(), p.marks.end());
        if (!p.setState.empty()) k.states.insert(p.setState);
    }
    k.tags.insert("fermented");
    for (const auto& [id, s] : d.shapes) {
        if (!s.role.empty()) k.roles.insert(s.role);
        for (const auto& [v, x] : s.v) k.shapeVals.insert(v);
    }
    k.roles.insert("binder");
    for (const auto& f : d.forms) k.toolTags.insert(f.deriveTags.begin(), f.deriveTags.end());
    k.toolTags.insert({"cutting", "striking", "hands"}); // functional tags toolOf() gives
    for (const auto& [id, s] : d.stations) k.stationTags.insert(s.tags.begin(), s.tags.end());
    for (const auto& e : d.effStack) k.effects.insert(e);
    for (const auto& e : d.effDominant) k.effects.insert(e);
    return k;
}

// AL-117: an aspect effect (aspect.<id>[.minus|.shift]) of a known aspect is known even when no
// material carries it yet; without content/magic every well-formed one is.
bool knownAspect(const CraftData& d, const std::string& effect) {
    std::string a;
    char v = '+';
    if (!parseAspectEffect(effect, a, v)) return false;
    return d.aspects.empty() || d.aspect(a) != nullptr;
}

void checkExpr(const CraftData& d, const Known& k, const Expr& e, const std::set<std::string>& extra,
               const std::string& where, std::vector<std::string>& err) {
    static const std::set<std::string> vars{"q", "len", "thick", "joint", "solo", "liquid", "wear", "mass", "x",
                                            "heat", "even", "hygiene",
                                            // AL-117: magic in the thing
                                            "unstable", "aspects", "rune", "inscribed", "resonance", "deeds"};
    for (const auto& n : e.names()) {
        if (vars.count(n) || extra.count(n) || isProp(n) || d.isFunction(n)) continue;
        size_t dot = n.find('.');
        if (dot != std::string::npos) {
            std::string head = n.substr(0, dot), tail = n.substr(dot + 1);
            if (head == "shape" && k.shapeVals.count(tail)) continue;
            if (head == "mat" && isProp(tail)) continue;
            if (k.roles.count(head) && (isProp(tail) || tail == "len" || tail == "thick" || tail == "q" || tail == "mass"))
                continue;
        }
        err.push_back(where + ": unknown name '" + n + "' in '" + e.source() + "'");
    }
    for (const auto& s : e.symbols()) {
        size_t p = s.find('(');
        std::string fn = s.substr(0, p), arg = s.substr(p + 1, s.size() - p - 2);
        bool ok = true;
        if (fn == "has" || fn == "count" || fn == "role") ok = k.roles.count(arg) > 0;
        else if (fn == "mass") ok = arg.empty() || k.roles.count(arg) > 0;
        else if (fn == "flaw") ok = d.flaw(arg) != nullptr;
        else if (fn == "state") ok = k.states.count(arg) > 0 || arg == "glazed";
        else if (fn == "tag") ok = k.tags.count(arg) > 0;
        else if (fn == "eff") ok = k.effects.count(arg) > 0 || knownAspect(d, arg);
        if (!ok) err.push_back(where + ": unknown argument in " + s + " ('" + e.source() + "')");
    }
}

} // namespace

void validateCraft(const Content& c, ValidationReport& r) {
    const CraftData& d = c.craft;
    if (!d.loaded) return;
    auto E = [&](const std::string& s) { r.errors.push_back("craft: " + s); };
    auto W = [&](const std::string& s) { r.warnings.push_back("craft: " + s); };
    for (const auto& e : d.errors) E(e);
    for (const auto& w : d.warnings) W(w);
    const Known k = known(d);
    std::vector<std::string> xerr;

    // materials
    for (const auto& [id, m] : d.materials) {
        const std::string w = "material " + id;
        if (!d.shape(m.shape)) E(w + ": unknown shape " + m.shape);
        if (!m.salvage.empty() && !d.material(m.salvage)) E(w + ": unknown salvage material " + m.salvage);
        if (!m.item.empty() && !c.items.count(m.item)) E(w + ": unknown item " + m.item);
        for (const auto& h : m.heat) {
            if (!h.to.empty() && !d.material(h.to)) E(w + ".heat." + h.id + ": unknown material " + h.to);
            if (!h.result.empty() && !d.material(h.result)) E(w + ".heat." + h.id + ": unknown material " + h.result);
            if (h.at < 0 || h.at > 4) E(w + ".heat." + h.id + ": level out of 0..4");
            for (const auto& n : h.needs)
                if (!k.tags.count(n)) E(w + ".heat." + h.id + ": needs unknown tag " + n);
            checkExpr(d, k, h.when, {}, w + ".heat." + h.id, xerr);
        }
        for (const auto& f : m.refine) {
            if (!d.process(f.process)) E(w + ".refine: unknown process " + f.process);
            if (!d.material(f.to)) E(w + ".refine: unknown material " + f.to);
            checkExpr(d, k, f.when, {}, w + ".refine", xerr);
        }
        for (const auto& s : m.states) checkExpr(d, k, s.when, {}, w + ".states." + s.id, xerr);
        if (m.raw && m.item.empty())
            W(w + ": raw material with no source in the world (no item gathers it yet)");
    }
    for (const auto& [id, it] : c.items)
        if (!it.material.empty() && !d.material(it.material)) E("item " + id + ": unknown material " + it.material);
    for (const auto& [id, o] : c.objects)
        if (!o.station.empty() && !d.station(o.station)) E("object " + id + ": unknown station " + o.station);

    // shapes producible: raw forms, every shape_map target, assembly results, blends
    std::set<std::string> producible{"liquid", "lump", "assembly", "scrap"};
    for (const auto& [id, m] : d.materials) producible.insert(m.shape);
    for (const auto& pid : d.processOrder) {
        const ProcessDef& p = d.processes.at(pid);
        const std::string w = "process " + pid;
        if (p.skill.empty() || !c.knowledge.count(p.skill)) E(w + ": unknown skill '" + p.skill + "' (knowledge.json)");
        for (const auto& [tag, sk] : p.skillBy) {
            if (!c.knowledge.count(sk)) E(w + ".skill_by: unknown skill " + sk);
            if (!k.tags.count(tag)) E(w + ".skill_by: unknown tag " + tag);
        }
        for (const auto& t : p.tools)
            if (!k.toolTags.count(t)) E(w + ": no tool has the tag " + t);
        for (const auto& t : p.stations)
            if (!k.stationTags.count(t)) E(w + ": no station has the tag " + t);
        if (p.heatMin < 0 || p.heatMin > 4) E(w + ": heat_min out of 0..4");
        for (const auto& sl : p.slots) {
            for (const auto& s : sl.shapes)
                if (s != "any" && !d.shape(s)) E(w + ".inputs." + sl.name + ": unknown shape " + s);
            for (const auto& t : sl.tags)
                if (!k.tags.count(t)) E(w + ".inputs." + sl.name + ": unknown tag " + t);
            if (sl.min > sl.max) E(w + ".inputs." + sl.name + ": min > max");
        }
        for (const auto& [from, tos] : p.shapeMap) {
            if (!d.shape(from)) E(w + ".shape_map: unknown shape " + from);
            for (const auto& t : tos) {
                if (!d.shape(t)) E(w + ".shape_map: unknown shape " + t);
                producible.insert(t);
            }
        }
        for (const auto& a : p.assemble) {
            for (const auto& s : {a.a, a.b, a.to})
                if (s != "any" && !d.shape(s)) E(w + ".assemble: unknown shape " + s);
            producible.insert(a.to);
        }
        if (!p.toMaterial.empty() && !d.material(p.toMaterial)) E(w + ": unknown material " + p.toMaterial);
        for (const auto& f : p.flaws) {
            if (!d.flaw(f.id)) E(w + ".flaws: unknown flaw " + f.id);
            checkExpr(d, k, f.p, {}, w + ".flaws." + f.id, xerr);
            checkExpr(d, k, f.when, {}, w + ".flaws." + f.id, xerr);
        }
        for (const auto& f : p.fixes)
            if (!d.flaw(f)) E(w + ".fixes: unknown flaw " + f);
        for (const auto& [prop, e] : p.transform) {
            if (!isProp(prop) && prop != "wear") E(w + ".transform: unknown property " + prop);
            checkExpr(d, k, e, {}, w + ".transform." + prop, xerr);
        }
        checkExpr(d, k, p.diffAdd, {}, w + ".difficulty", xerr);
        checkExpr(d, k, p.toolHardness, {}, w + ".requires", xerr);
        for (const auto& m : p.noAirModes)
            if (std::find(p.modes.begin(), p.modes.end(), m) == p.modes.end()) E(w + ": no_air mode not in modes: " + m);
    }
    for (const auto& [id, s] : d.shapes)
        if (!producible.count(id)) W("shape " + id + ": no process makes it");

    // functions, forms
    for (const auto& f : d.functions) checkExpr(d, k, f.f, {}, "function " + f.id, xerr);
    for (const auto& [prop, rule] : d.blend)
        if (!isProp(prop)) E("blend: unknown property " + prop);
    for (const auto& f : d.forms) {
        const std::string w = "form " + f.id;
        for (const auto& s : f.shape) {
            if (!d.shape(s)) E(w + ": unknown shape " + s);
            else if (!producible.count(s)) E(w + ": shape " + s + " is made by no process (unreachable)");
        }
        for (const auto& m : f.mat)
            if (!k.tags.count(m)) E(w + ": unknown material or tag " + m);
        for (const auto& s : f.state)
            if (!k.states.count(s)) E(w + ": unknown state " + s);
        for (const auto& pr : f.profile)
            for (size_t b = 0, e; b <= pr.size(); b = e + 1) {
                e = pr.find('|', b);
                if (e == std::string::npos) e = pr.size();
                std::string t = pr.substr(b, e - b);
                if (!k.tags.count(t)) E(w + ": profile names unknown tag " + t);
            }
        for (const auto& ef : f.effects)
            for (size_t b = 0, e; b <= ef.size(); b = e + 1) {
                e = ef.find('|', b);
                if (e == std::string::npos) e = ef.size();
                if (!k.effects.count(ef.substr(b, e - b)) && !knownAspect(d, ef.substr(b, e - b)))
                    E(w + ": unknown effect " + ef.substr(b, e - b));
            }
        for (const auto& n : f.needs) checkExpr(d, k, n.expr, {}, w + ".needs", xerr);
        for (const auto& [key, e] : f.derive) checkExpr(d, k, e, {}, w + ".derive." + key, xerr);
        if (!f.item.empty() && !c.items.count(f.item)) E(w + ": unknown item " + f.item);
        if (!d.design(f.id)) E(w + ": no starter design (designs.json): its reachability is not shown");
    }
    for (const auto& im : d.improvised)
        if (im.fn != "any" && !d.isFunction(im.fn)) E("improvised " + im.id + ": unknown function " + im.fn);

    // reactions
    for (const auto& rc : d.reacts) {
        const std::string w = "react " + rc.id;
        if (!rc.make.empty() && !d.material(rc.make)) E(w + ": unknown material " + rc.make);
        for (const auto& t : rc.when)
            for (size_t b = 0, e; b <= t.size(); b = e + 1) {
                e = t.find('|', b);
                if (e == std::string::npos) e = t.size();
                std::string a = t.substr(b, e - b);
                if (a == "no_air" || a == "no_yeast") continue;
                if (a.rfind("eff:", 0) == 0) {
                    if (!k.effects.count(a.substr(4)) && !knownAspect(d, a.substr(4))) E(w + ": unknown effect " + a);
                    continue;
                }
                if (a.rfind("heat", 0) == 0 && a.size() == 5) continue;
                if (!k.tags.count(a)) E(w + ": unknown tag " + a);
            }
        for (const auto& [prop, cn] : rc.propConds)
            if (!isProp(prop)) E(w + ": unknown property " + prop);
        if (rc.hasRatio && !d.material(rc.ratioMat)) E(w + ": ratio names unknown material " + rc.ratioMat);
    }

    // flaws, stations, designs
    for (const auto& [id, f] : d.flaws) {
        if (!f.fix.empty() && !d.process(f.fix) && f.fix != "rework") E("flaw " + id + ": unknown fix process " + f.fix);
        if (!f.salvage.empty() && !d.material(f.salvage)) E("flaw " + id + ": unknown salvage material " + f.salvage);
        for (const auto& [key, m] : f.mods)
            if (!isProp(key) && !d.isFunction(key) && key != "joint") E("flaw " + id + ": mods unknown name " + key);
    }
    for (const auto& ds : d.designs) {
        const std::string w = "design " + ds.id;
        if (!d.form(ds.form)) E(w + ": unknown form " + ds.form);
        for (size_t i = 0; i < ds.steps.size(); ++i) {
            const DesignStep& st = ds.steps[i];
            const ProcessDef* p = d.process(st.process);
            if (!p) {
                E(w + ": step " + std::to_string(i + 1) + " unknown process " + st.process);
                continue;
            }
            if (!st.shape.empty() && !d.shape(st.shape)) E(w + ": step " + std::to_string(i + 1) + " unknown shape " + st.shape);
            if (!st.mode.empty() && std::find(p->modes.begin(), p->modes.end(), st.mode) == p->modes.end())
                E(w + ": step " + std::to_string(i + 1) + " mode " + st.mode + " not offered by " + st.process);
            for (const auto& [slot, ins] : st.slots) {
                bool okSlot = false;
                for (const auto& sl : p->slots)
                    if (sl.name == slot) okSlot = true;
                if (!okSlot) E(w + ": step " + std::to_string(i + 1) + " unknown slot " + slot);
                for (const auto& in : ins) {
                    const std::string& ref = in.ref;
                    bool ok = true;
                    if (ref.rfind("mat:", 0) == 0) ok = d.material(ref.substr(4)) != nullptr;
                    else if (ref.rfind("form:", 0) == 0) ok = d.form(ref.substr(5)) != nullptr;
                    else if (ref.rfind("shape:", 0) == 0) ok = d.shape(ref.substr(6)) != nullptr;
                    else if (ref.rfind("tag:", 0) == 0) ok = k.tags.count(ref.substr(4)) > 0;
                    else if (ref.rfind("step:", 0) == 0) {
                        int n = std::atoi(ref.c_str() + 5);
                        ok = n >= 1 && n <= static_cast<int>(i);
                    } else ok = false;
                    if (!ok) E(w + ": step " + std::to_string(i + 1) + " bad ref " + ref);
                }
            }
        }
    }
    for (const auto& [t, mods] : d.cfg.traitMods)
        if (!c.traits.count(t)) W("crafting_config.traits: unknown trait " + t);
    for (const auto& x : xerr) E(x);
}

} // namespace lr::sim::craft
