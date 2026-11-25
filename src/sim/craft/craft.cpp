#include "sim/craft/craft.hpp"
#include "sim/craft/craft_magic.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

namespace lr::sim::craft {

namespace {

double clamp01(double v) { return std::min(1.0, std::max(0.0, v)); }

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    out.push_back(cur);
    return out;
}

std::uint64_t mix64(std::uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

const char* kMainRoles[] = {"blade", "head", "stave", "body", "shaft", "handle"};

const Item* mainPiece(const Item& it) {
    if (!it.isAssembly()) return &it;
    for (const char* r : kMainRoles)
        if (const Item* p = it.part(r)) return p;
    return &it.parts.front().item.front();
}

std::string roleOf(const CraftData& d, const Item& it) {
    if (it.isAssembly()) return "";
    const ShapeDef* s = d.shape(it.shape);
    return s ? s->role : "";
}

void collectFlaws(const Item& it, std::vector<const ItemFlaw*>& out) {
    for (const auto& f : it.flaws) out.push_back(&f);
    for (const auto& p : it.parts) collectFlaws(p.item.front(), out);
}

// Properties of a single piece: base + permanent state deltas are already in p;
// temporary states (hot) by heat level; flaw mods of the piece itself.
std::map<std::string, double> pieceProps(const CraftData& d, const Item& it, int heat);

struct PieceCtx : ExprCtx {
    const CraftData& d;
    const Item& it;
    const std::map<std::string, double>& p;
    int heat;
    PieceCtx(const CraftData& dd, const Item& i, const std::map<std::string, double>& pp, int h)
        : d(dd), it(i), p(pp), heat(h) {}
    double var(const std::string& n) const override {
        if (n == "heat") return heat;
        if (n == "len") return it.len;
        if (n == "thick") return it.thick;
        if (n == "q") return it.q;
        auto f = p.find(n);
        return f == p.end() ? 0.0 : f->second;
    }
    double sym(const std::string& fn, const std::string& a) const override {
        if (fn == "tag") return it.tags.count(a) ? 1.0 : 0.0;
        if (fn == "state") return it.states.count(a) ? 1.0 : 0.0;
        if (fn == "flaw") return it.hasFlaw(a) ? 1.0 : 0.0;
        if (fn == "eff") {
            auto e = it.effects.find(a);
            return e == it.effects.end() ? 0.0 : e->second;
        }
        return 0.0;
    }
};

std::map<std::string, double> pieceProps(const CraftData& d, const Item& it, int heat) {
    std::map<std::string, double> p = it.p;
    if (const MaterialDef* m = d.material(it.material))
        for (const auto& st : m->states) {
            if (!st.temp || st.when.empty()) continue;
            PieceCtx ctx(d, it, it.p, heat);
            if (st.when.eval(ctx) != 0.0)
                for (const auto& [k, v] : st.add) p[k] = clamp01(p[k] + v);
        }
    for (const auto& f : it.flaws)
        if (const FlawDef* fd = d.flaw(f.id))
            for (const auto& [k, v] : fd->mods)
                if (isProp(k) && p.count(k)) p[k] *= v;
    return p;
}

std::map<std::string, double> blendProps(const CraftData& d, const std::vector<std::map<std::string, double>>& ps,
                                         const std::vector<double>& mass) {
    std::map<std::string, double> out;
    double total = 0.0;
    for (double m : mass) total += m;
    if (total <= 0) total = 1.0;
    std::set<std::string> keys;
    for (const auto& p : ps)
        for (const auto& [k, v] : p) keys.insert(k);
    for (const auto& k : keys) {
        auto rIt = d.blend.find(k);
        std::string rule = rIt == d.blend.end() ? "avg_mass" : rIt->second;
        double acc = 0.0;
        bool any = false;
        for (size_t i = 0; i < ps.size(); ++i) {
            auto f = ps[i].find(k);
            if (f == ps[i].end()) continue;
            double v = f->second;
            if (rule == "avg_mass") acc += v * mass[i] / total;
            else if (rule == "max") acc = any ? std::max(acc, v) : v;
            else if (rule == "min") acc = any ? std::min(acc, v) : v;
            else if (rule == "sum_cap") acc += v;
            else if (rule == "product") acc = any ? acc * v : v;
            any = true;
        }
        if (rule == "sum_cap") acc = std::min(1.0, acc);
        if (k == "melt") out[k] = acc; // a level, not a share
        else out[k] = rule == "avg_mass" ? acc : clamp01(acc);
    }
    return out;
}

// The whole item: a single piece or an assembly blended over its parts.
struct ItemView {
    std::map<std::string, double> p;
    std::map<std::string, const Item*> role; // first part of each role
    std::map<std::string, std::map<std::string, double>> roleP;
};

ItemView viewOf(const CraftData& d, const Item& it, int heat) {
    ItemView v;
    if (!it.isAssembly()) {
        v.p = pieceProps(d, it, heat);
        return v;
    }
    std::vector<std::map<std::string, double>> ps;
    std::vector<double> ms;
    for (const auto& part : it.parts) {
        const Item& pi = part.item.front();
        ItemView sub = viewOf(d, pi, heat);
        ps.push_back(sub.p);
        ms.push_back(itemMass(d, pi));
        if (!part.role.empty() && !v.role.count(part.role)) {
            v.role[part.role] = &pi;
            v.roleP[part.role] = sub.p;
        }
    }
    v.p = blendProps(d, ps, ms);
    for (const auto& f : it.flaws)
        if (const FlawDef* fd = d.flaw(f.id))
            for (const auto& [k, mv] : fd->mods)
                if (isProp(k) && v.p.count(k)) v.p[k] *= mv;
    return v;
}

struct ItemCtx : ExprCtx {
    const CraftData& d;
    const Item& it;
    ItemView view;
    const std::map<std::string, double>* fn = nullptr;
    std::map<std::string, double> extra;
    ItemCtx(const CraftData& dd, const Item& i, int heat) : d(dd), it(i), view(viewOf(dd, i, heat)) {}

    double partVar(const Item& pi, const std::map<std::string, double>& pp, const std::string& k) const {
        if (k == "len") return pi.len;
        if (k == "thick") return pi.thick;
        if (k == "q") return pi.q;
        if (k == "mass") return itemMass(d, pi);
        auto f = pp.find(k);
        return f == pp.end() ? 0.0 : f->second;
    }
    double var(const std::string& n) const override {
        if (auto e = extra.find(n); e != extra.end()) return e->second;
        if (n == "q") return it.q;
        if (n == "len") return it.len;
        if (n == "thick") return it.thick;
        if (n == "joint") {
            double j = it.joint;
            for (const auto& f : it.flaws)
                if (const FlawDef* fd = d.flaw(f.id))
                    if (auto m = fd->mods.find("joint"); m != fd->mods.end()) j *= m->second;
            return j;
        }
        if (n == "solo") return it.isAssembly() ? 0.0 : 1.0;
        if (n == "liquid") return it.shape == "liquid" ? 1.0 : 0.0;
        if (n == "wear") return it.wear;
        if (n == "mass") return itemMass(d, it);
        // AL-117: magic in the thing
        if (n == "unstable") return instability(d, it);
        if (n == "aspects") return aspectTotal(it);
        if (n == "rune") return runePower(d, it);
        if (n == "inscribed") return it.inscribed ? 1.0 : 0.0;
        if (n == "resonance") return it.resonance;
        if (n == "deeds") return it.deeds;
        size_t dot = n.find('.');
        if (dot != std::string::npos) {
            std::string head = n.substr(0, dot), k = n.substr(dot + 1);
            if (head == "shape") {
                const ShapeDef* s = d.shape(it.shape);
                if (!s) return 0.0;
                auto f = s->v.find(k);
                return f == s->v.end() ? 0.0 : f->second;
            }
            if (head == "mat") {
                const MaterialDef* m = d.material(mainPiece(it)->material);
                if (!m) return 0.0;
                auto f = m->p.find(k);
                return f == m->p.end() ? 0.0 : f->second;
            }
            auto r = view.role.find(head);
            if (r == view.role.end()) return 0.0;
            return partVar(*r->second, view.roleP.at(head), k);
        }
        if (fn) {
            auto f = fn->find(n);
            if (f != fn->end()) return f->second;
        } else if (auto f = it.fn.find(n); f != it.fn.end()) {
            return f->second;
        }
        auto f = view.p.find(n);
        return f == view.p.end() ? 0.0 : f->second;
    }
    double sym(const std::string& f, const std::string& a) const override {
        if (f == "has" || f == "count") {
            int c = 0;
            for (const auto& p : it.parts)
                if (p.role == a) ++c;
            return f == "has" ? (c > 0 ? 1.0 : 0.0) : c;
        }
        if (f == "role") return (!it.isAssembly() && roleOf(d, it) == a) ? 1.0 : 0.0;
        if (f == "mass") {
            if (a.empty()) return itemMass(d, it);
            double m = 0.0;
            for (const auto& p : it.parts)
                if (p.role == a) m += itemMass(d, p.item.front());
            return m;
        }
        if (f == "flaw") {
            std::vector<const ItemFlaw*> fl;
            collectFlaws(it, fl);
            for (const auto* x : fl)
                if (x->id == a) return 1.0;
            return 0.0;
        }
        if (f == "state") {
            if (it.states.count(a)) return 1.0;
            return mainPiece(it)->states.count(a) ? 1.0 : 0.0;
        }
        if (f == "tag") return it.tags.count(a) ? 1.0 : 0.0;
        if (f == "eff") {
            auto e = it.effects.find(a);
            return e == it.effects.end() ? 0.0 : e->second;
        }
        return 0.0;
    }
};

// Quality of a thing (§6): a smoothed mean of its steps, held down by the
// weakest step (a chain is no stronger than its link). The batch quality of
// the raw material counts in the mean as one step, but not as a weakest link.
void settleQ(Item& it) {
    if (it.qSteps <= 0) return;
    double mean = it.qSum / it.qSteps;
    double weakest = it.qMin <= 1.0 ? it.qMin : mean;
    it.q = clamp01(0.7 * mean + 0.3 * weakest);
}

void addQ(Item& it, double q) {
    if (it.qSteps == 0) {
        it.qSum = it.q;
        it.qMin = 2.0; // no step yet
        it.qSteps = 1;
    }
    it.qSum += q;
    it.qSteps += 1;
    it.qMin = std::min(it.qMin, q);
    settleQ(it);
}

void mergeQ(Item& into, const Item& from) {
    if (into.qSteps == 0) into.qMin = 2.0;
    if (from.qSteps == 0) {
        into.qSum += from.q;
        into.qSteps += 1;
    } else {
        into.qSum += from.qSum;
        into.qSteps += from.qSteps;
        into.qMin = std::min(into.qMin, from.qMin);
    }
    settleQ(into);
}

void addFlaw(Item& it, const std::string& id) {
    if (!it.hasFlaw(id)) it.flaws.push_back({id, false});
}

void setMaterial(const CraftData& d, Item& it, const std::string& mat) {
    const MaterialDef* m = d.material(mat);
    if (!m) return;
    double moisture = it.p.count("moisture") ? it.p["moisture"] : -1.0;
    it.material = mat;
    it.p = m->p;
    if (moisture >= 0 && m->p.count("moisture")) it.p["moisture"] = std::min(m->p.at("moisture"), moisture);
    it.tags.insert(mat);
    for (const auto& t : m->tags) it.tags.insert(t);
    for (const auto& e : m->effects) it.effects[e.id] = std::max(it.effects[e.id], e.k * m->potency);
    it.states.clear();
}

void applyStates(const CraftData& d, Item& it, int heat) {
    if (it.isAssembly()) return;
    const MaterialDef* m = d.material(it.material);
    if (!m) return;
    for (const auto& st : m->states) {
        if (st.temp || st.when.empty() || it.states.count(st.id)) continue;
        PieceCtx ctx(d, it, it.p, heat);
        if (st.when.eval(ctx) == 0.0) continue;
        it.states.insert(st.id);
        for (const auto& [k, v] : st.add) it.p[k] = clamp01(it.p[k] + v);
    }
}

void enterState(const CraftData& d, Item& it, const std::string& state) {
    if (it.states.count(state)) return;
    it.states.insert(state);
    if (const MaterialDef* m = d.material(it.material))
        for (const auto& st : m->states)
            if (st.id == state)
                for (const auto& [k, v] : st.add) it.p[k] = clamp01(it.p[k] + v);
}

void cancelEffects(const CraftData& d, std::map<std::string, double>& e) {
    for (const auto& [a, b] : d.effCancel) {
        double x = e.count(a) ? e[a] : 0.0, y = e.count(b) ? e[b] : 0.0;
        double m = std::min(x, y);
        if (m <= 0) continue;
        e[a] = x - m;
        e[b] = y - m;
    }
    for (auto it = e.begin(); it != e.end();)
        it = it->second <= 1e-9 ? e.erase(it) : std::next(it);
}

std::string refOf(const Item& it) {
    if (!it.form.empty() && it.form.rfind("improvised_", 0) != 0) return "form:" + it.form;
    if (it.isAssembly()) return "shape:" + it.shape;
    return "mat:" + it.material;
}

struct StepEnv {
    int heat = 0;          // level in effect
    double even = 0.5;
    double hygiene = 1.0;
    bool noAir = false;
    double stationBonus = 0.0;
};

bool stationHasTag(const StationDef& s, const std::string& t) { return contains(s.tags, t); }

std::vector<const StationDef*> workingStations(const CraftData& d, const Workplace& wp) {
    std::set<std::string> tags;
    std::vector<const StationDef*> all;
    for (const auto& id : wp.stations)
        if (const StationDef* s = d.station(id)) {
            all.push_back(s);
            for (const auto& t : s->tags) tags.insert(t);
        }
    for (const auto& tl : wp.tools)
        for (const auto& t : tl.tags) tags.insert(t);
    std::vector<const StationDef*> out;
    for (const auto* s : all) {
        bool ok = true;
        for (const auto& n : s->needs)
            if (!tags.count(n)) ok = false;
        if (ok) out.push_back(s);
    }
    return out;
}

const std::vector<Item>* slotItems(const StepInput& in, const std::string& slot) {
    auto it = in.slots.find(slot);
    return it == in.slots.end() ? nullptr : &it->second;
}

bool slotMatches(const CraftData& d, const SlotDef& s, const Item& it, int heat) {
    bool anyShape = s.shapes.empty() || contains(s.shapes, "any");
    bool shapeOk = anyShape || contains(s.shapes, it.shape);
    bool tagOk = s.tags.empty();
    for (const auto& t : s.tags)
        if (it.tags.count(t)) tagOk = true;
    bool ok = s.either ? ((!s.shapes.empty() && shapeOk && !anyShape) || (!s.tags.empty() && tagOk))
                       : (shapeOk && tagOk);
    if (!ok) return false;
    if (!s.p.empty()) {
        ItemCtx ctx(d, it, heat);
        for (const auto& [k, c] : s.p)
            if (!c.ok(ctx.var(k))) return false;
    }
    return true;
}

std::string skillFor(const ProcessDef& p, const Item* work) {
    if (work)
        for (const auto& [tag, sk] : p.skillBy)
            if (work->tags.count(tag)) return sk;
    return p.skill;
}

// Heat in effect and the station figures for a step.
StepEnv envFor(const CraftData& d, const ProcessDef& p, const StepInput& in, const Workplace& wp) {
    StepEnv e;
    auto st = workingStations(d, wp);
    int maxHeat = 0;
    const StationDef* heatSrc = nullptr;
    for (const auto* s : st) {
        bool allowed = p.stations.empty() || p.op != ProcOp::Heat;
        for (const auto& t : p.stations)
            if (stationHasTag(*s, t)) allowed = true;
        if (allowed && s->tempMax > maxHeat) {
            maxHeat = s->tempMax;
            heatSrc = s;
        }
        bool counts = p.stations.empty();
        for (const auto& t : p.stations)
            if (stationHasTag(*s, t)) counts = true;
        if (counts) {
            e.stationBonus = std::max(e.stationBonus, s->qualityBonus);
            if (s->noAir) e.noAir = true;
            e.hygiene = std::min(e.hygiene, s->cleanDefault);
        }
    }
    e.heat = in.target.heat >= 0 ? std::min(in.target.heat, maxHeat) : maxHeat;
    if (p.op != ProcOp::Heat && p.heatMin <= 0) e.heat = 0;
    if (heatSrc) e.even = heatSrc->even;
    if (wp.hygiene >= 0) e.hygiene = wp.hygiene;
    if (contains(p.noAirModes, in.target.mode)) e.noAir = true;
    return e;
}

double toolQuality(const ProcessDef& p, const Workplace& wp, const CraftData& d) {
    if (p.tools.empty()) {
        double best = d.cfg.toolQualityBare;
        return best;
    }
    double best = -1.0;
    for (const auto& tl : wp.tools)
        for (const auto& t : p.tools)
            if (contains(tl.tags, t)) best = std::max(best, tl.q);
    return best < 0 ? 0.0 : best;
}

const Item* workPiece(const ProcessDef& p, const StepInput& in) {
    if (p.slots.empty()) return nullptr;
    const auto* v = slotItems(in, p.slots.front().name);
    return v && !v->empty() ? &v->front() : nullptr;
}

double traitMod(const CraftData& d, const Actor& a, const char* key) {
    double s = 0.0;
    for (const auto& t : a.traits) {
        auto it = d.cfg.traitMods.find(t);
        if (it == d.cfg.traitMods.end()) continue;
        auto k = it->second.find(key);
        if (k != it->second.end()) s += k->second;
    }
    return s;
}

struct Numbers {
    double d = 0, c = 0, m = 0;
    std::string skill;
};

Numbers numbersFor(const CraftData& dd, const ProcessDef& p, const StepInput& in, const Actor& a, const Workplace& wp,
                   const StepEnv& env) {
    Numbers n;
    const Item* work = workPiece(p, in);
    n.skill = skillFor(p, work);
    n.d = p.diffBase;
    if (work && !p.diffAdd.empty()) {
        ItemCtx ctx(dd, *work, env.heat);
        n.d += p.diffAdd.eval(ctx);
    }
    double skill = dd.cfg.novice;
    if (auto it = a.skills.find(n.skill); it != a.skills.end()) skill = it->second;
    double know = 0.0;
    int cnt = 0;
    for (const auto& [slot, items] : in.slots)
        for (const auto& it : items) {
            auto k = a.knowsMaterial.find(mainPiece(it)->material);
            know += k == a.knowsMaterial.end() ? 0.0 : k->second;
            ++cnt;
        }
    if (cnt) know /= cnt;
    // competence (§4.4.3): skill + tool + station + knowing the material + mood + having done it before;
    // the trait's quality modifier goes to the step quality, not here
    n.c = skill + dd.cfg.wTool * toolQuality(p, wp, dd) + env.stationBonus + dd.cfg.wKnow * know + 0.05 * a.mood +
          dd.cfg.repeatBonus * a.repeat;
    n.m = n.c - n.d;
    return n;
}

Outcome outcomeOf(const CraftData& d, const ProcessDef& p, double z) {
    if (z < d.cfg.tCatastrophe && p.risky) return Outcome::Catastrophe;
    if (z < d.cfg.tRuin) return Outcome::Ruined;
    if (z < d.cfg.tOk) return Outcome::Flawed;
    if (z < d.cfg.tGood) return Outcome::Ok;
    if (z < d.cfg.tMaster) return Outcome::Good;
    return Outcome::Masterwork;
}

// The deterministic part of a step: what the inputs become at quality qStep.
// `destroyed` gets the salvage material when a heat reaction destroys the item.
struct Applied {
    Item out;
    std::string destroyed;
    std::vector<std::string> fired; // reactions
};

void applyReacts(const CraftData& d, const ProcessDef& p, Item& it, const StepEnv& env,
                 const std::map<std::string, double>& massShare, std::vector<std::string>& fired, Roll* roll) {
    for (const auto& r : d.reacts) {
        if (r.atHeat > env.heat) continue;
        bool ok = true;
        for (const auto& w : r.when) {
            bool any = false;
            for (const auto& alt : split(w, '|')) {
                if (alt == "no_air") any = any || env.noAir;
                else if (alt == "no_yeast") any = any || !it.tags.count("yeast");
                else if (alt.rfind("eff:", 0) == 0) {
                    auto e = it.effects.find(alt.substr(4));
                    any = any || (e != it.effects.end() && e->second >= d.cfg.effectPresent);
                } else if (alt.rfind("heat", 0) == 0 && alt.size() > 4 && std::isdigit(static_cast<unsigned char>(alt[4]))) {
                    any = any || env.heat >= alt[4] - '0';
                } else {
                    any = any || it.tags.count(alt) > 0;
                }
            }
            if (!any) ok = false;
        }
        if (!ok) continue;
        ItemCtx ctx(d, it, env.heat);
        for (const auto& [k, c] : r.propConds)
            if (!c.ok(ctx.var(k))) ok = false;
        if (r.hasRatio) {
            auto s = massShare.find(r.ratioMat);
            if (s == massShare.end() || !r.ratio.ok(s->second)) ok = false;
        }
        if (!ok) continue;
        // a reaction happens once per item
        std::string mark = "react:" + r.id;
        if (it.tags.count(mark)) continue;
        it.tags.insert(mark);
        fired.push_back(r.id);
        if (!r.make.empty()) {
            if (r.replace) {
                std::set<std::string> keep = it.tags;
                std::map<std::string, double> keepEff = it.effects;
                setMaterial(d, it, r.make);
                it.tags.insert(keep.begin(), keep.end());
                for (const auto& [k, v] : keepEff) it.effects[k] = std::max(it.effects[k], v * 0.5);
                if (const MaterialDef* m = d.material(r.make)) it.shape = m->shape;
            } else if (const MaterialDef* m = d.material(r.make)) {
                it.tags.insert(r.make);
                for (const auto& t : m->tags) it.tags.insert(t);
                for (const auto& e : m->effects) it.effects[e.id] += e.k * m->potency * 0.5;
            }
        }
        for (const auto& [k, e] : r.p) {
            ItemCtx c2(d, it, env.heat);
            c2.extra["x"] = it.p.count(k) ? it.p[k] : 0.0;
            it.p[k] = clamp01(e.eval(c2));
        }
        if (!r.extract.empty()) it.extracted.insert(r.extract);
        if (r.riskTainted > 0 && roll && roll->next() < r.riskTainted) addFlaw(it, "tainted");
    }
    (void)p;
}

Item blendItems(const CraftData& d, const std::vector<const Item*>& ins, const Target& t,
                std::map<std::string, double>& massShare) {
    Item out;
    std::vector<std::map<std::string, double>> ps;
    std::vector<double> ms;
    double total = 0.0, liquid = 0.0, best = -1.0;
    for (const Item* it : ins) {
        double m = itemMass(d, *it);
        ps.push_back(viewOf(d, *it, 0).p);
        ms.push_back(m);
        total += m;
        if (it->shape == "liquid" || it->tags.count("liquid")) liquid += m;
        else if (m > best) {
            best = m;
            out.material = mainPiece(*it)->material;
        }
        out.tags.insert(it->tags.begin(), it->tags.end());
        out.extracted.insert(it->extracted.begin(), it->extracted.end());
        for (const auto& f : it->flaws)
            if (f.id == "tainted" || f.id == "rot" || f.id == "burnt" || f.id == "impure") addFlaw(out, f.id);
        massShare[mainPiece(*it)->material] += m;
        mergeQ(out, *it);
    }
    if (out.material.empty() && !ins.empty()) out.material = mainPiece(*ins.front())->material;
    if (total <= 0) total = 1.0;
    for (auto& [k, v] : massShare) v /= total;
    out.p = blendProps(d, ps, ms);
    // effects: concentration by mass share
    std::map<std::string, double> stacked, dominant;
    for (size_t i = 0; i < ins.size(); ++i)
        for (const auto& [e, k] : ins[i]->effects) {
            double c = k * ms[i] / total;
            if (contains(d.effDominant, e)) dominant[e] = std::max(dominant[e], c);
            else stacked[e] += c;
        }
    out.effects = stacked;
    for (const auto& [e, v] : dominant) out.effects[e] = v;
    cancelEffects(d, out.effects);
    if (liquid > 0) out.extracted.insert("water_cold");
    out.shape = !t.shape.empty() ? t.shape : (liquid / total > 0.5 ? "liquid" : "lump");
    out.len = 1;
    out.thick = 1;
    double dens = out.p.count("density") && out.p["density"] > 0 ? out.p["density"] : 0.5;
    out.frac = total / (dens * d.cfg.massUnit);
    if (out.qSteps == 0) out.q = 0.5;
    return out;
}

void setShape(const CraftData& d, Item& it, const std::string& shape, int len) {
    const ShapeDef* s = d.shape(shape);
    bool changed = shape != it.shape;
    it.shape = shape;
    if (!s) return;
    if (len > 0) it.len = std::min(std::max(len, s->lenMin), s->lenMax);
    else if (changed && (it.len < s->lenMin || it.len > s->lenMax)) it.len = s->lenDefault; // a blank keeps its length
    if (changed) it.thick = s->thick;
    it.frac = 1.0;
}

Applied applyStep(const CraftData& d, const ProcessDef& p, const StepInput& in, const StepEnv& env, double qStep,
                  Roll* roll) {
    Applied A;
    Item& out = A.out;
    std::map<std::string, double> massShare;
    const Item* work = workPiece(p, in);
    std::vector<const Item*> co; // all non-work inputs
    for (size_t i = 0; i < p.slots.size(); ++i) {
        const auto* v = slotItems(in, p.slots[i].name);
        if (!v) continue;
        for (size_t j = 0; j < v->size(); ++j)
            if (!(i == 0 && j == 0)) co.push_back(&(*v)[j]);
    }

    switch (p.op) {
    case ProcOp::Blend: {
        std::vector<const Item*> all;
        if (work) all.push_back(work);
        all.insert(all.end(), co.begin(), co.end());
        out = blendItems(d, all, in.target, massShare);
        break;
    }
    case ProcOp::Join: {
        out = Item{};
        std::vector<const Item*> pieces;
        const Item* binder = nullptr;
        for (const auto& sl : p.slots) {
            const auto* v = slotItems(in, sl.name);
            if (!v) continue;
            for (const auto& it : *v) {
                if (sl.name == "binder") binder = &it;
                else pieces.push_back(&it);
            }
        }
        auto addPart = [&](const Item& it, const std::string& forceRole) {
            if (it.isAssembly()) {
                for (const auto& pt : it.parts) out.parts.push_back(pt);
            } else {
                Item::Part pt;
                pt.role = forceRole.empty() ? roleOf(d, it) : forceRole;
                pt.item.push_back(it);
                out.parts.push_back(pt);
            }
            out.tags.insert(it.tags.begin(), it.tags.end());
            for (const auto& [e, k] : it.effects) out.effects[e] = std::max(out.effects[e], k);
            mergeQ(out, it);
        };
        for (const Item* it : pieces) addPart(*it, "");
        std::string bindRole;
        if (binder) {
            bindRole = roleOf(d, *binder);
            if (bindRole.empty()) bindRole = "binder";
            addPart(*binder, bindRole);
        }
        double bStrength = 0.0;
        if (binder) {
            auto bp = viewOf(d, *binder, 0).p;
            bStrength = std::max({bp["tacky"], bp["toughness"], bp["hardness"]});
            out.jointType = binder->tags.count("adhesive") ? "glue"
                            : (binder->shape == "nail" ? "nail" : (binder->shape == "rivet" ? "rivet" : "bind"));
        } else {
            out.jointType = "tang";
        }
        out.joint = clamp01(0.3 + 0.5 * qStep + 0.2 * bStrength);
        out.shape = "assembly";
        if (pieces.size() >= 2)
            for (const auto& r : p.assemble) {
                auto m = [&](const std::string& want, const Item* x) { return want == "any" || x->shape == want; };
                bool okAB = (m(r.a, pieces[0]) && m(r.b, pieces[1])) || (m(r.a, pieces[1]) && m(r.b, pieces[0]));
                if (okAB && (!r.binder || binder)) {
                    out.shape = r.to;
                    break;
                }
            }
        if (const ShapeDef* s = d.shape(out.shape)) {
            out.len = in.target.len > 0 ? std::min(std::max(in.target.len, s->lenMin), s->lenMax) : s->lenDefault;
            out.thick = s->thick;
        }
        if (!out.parts.empty()) out.material = mainPiece(out)->material;
        break;
    }
    case ProcOp::Inscribe: { // AL-117: a rune cut into the best conductor of the thing
        if (!work) break;
        out = *work;
        Carrier cr = carrierOf(d, out);
        out.inscribed = true;
        out.rune.q = qStep;
        out.rune.conductivity = cr.conductivity;
        out.rune.capacity = cr.capacity;
        // an ink (blood, silver dust, a tincture) lends its aspects to the cut
        for (const Item* c : co)
            for (const auto& a : aspectDoses(*c)) out.rune.q = clamp01(out.rune.q + 0.1 * a.power);
        out.tags.insert("inscribed");
        break;
    }
    case ProcOp::Bind: { // AL-117: the aspects of the reagents laid into the rune, up to the carrier's capacity
        if (!work) break;
        out = *work;
        std::map<std::string, double> add;
        double total = 0.0;
        for (const Item* c : co)
            for (const auto& a : aspectDoses(*c)) {
                add[runeKey(a.aspect, a.vec)] += a.power;
                total += a.power;
            }
        const double limit = out.rune.capacity * d.magic.bindCapShare;
        const double take = total > 0 ? std::min(1.0, limit / total) : 0.0;
        const double qual = (0.5 + 0.5 * out.rune.q) * (0.6 + 0.4 * qStep);
        for (const auto& [k, v] : add) out.rune.aspects[k] += v * take * qual;
        double sum = 0.0;
        for (const auto& [k, v] : out.rune.aspects) sum += v;
        if (sum > limit && sum > 0)
            for (auto& [k, v] : out.rune.aspects) v *= limit / sum;
        const std::string& m = in.target.mode;
        out.rune.trigger = !m.empty() ? m : (out.rune.trigger.empty() ? d.magic.defaultTrigger : out.rune.trigger);
        out.rune.charge = out.rune.capacity;
        out.tags.insert("enchanted");
        break;
    }
    case ProcOp::Shape:
    case ProcOp::Heat:
    case ProcOp::Soak: {
        if (!work) break;
        out = *work;
        // more pieces in the work slot merge into the first (weave, sew, link)
        const auto* wv = slotItems(in, p.slots.front().name);
        if (wv && wv->size() > 1 && p.op == ProcOp::Shape) {
            std::vector<const Item*> all;
            for (const auto& x : *wv) all.push_back(&x);
            std::map<std::string, double> ms2;
            Item merged = blendItems(d, all, Target{work->shape, 0, "", -1}, ms2);
            merged.len = work->len;
            merged.thick = std::min(5, work->thick + static_cast<int>(wv->size()) - 1);
            merged.frac = 1.0;
            merged.trace = work->trace;
            out = merged;
        }
        if (p.op == ProcOp::Soak) {
            for (const Item* c : co) {
                for (const auto& t : c->tags)
                    if (t != "liquid" && t != "water") out.tags.insert(t);
                for (const auto& [e, k] : c->effects) out.effects[e] += k * 0.3;
            }
        }
        if (p.op == ProcOp::Heat && !out.isAssembly()) {
            const MaterialDef* m = d.material(out.material);
            std::set<std::string> coTags = out.tags;
            for (const Item* c : co) coTags.insert(c->tags.begin(), c->tags.end());
            if (m) {
                std::vector<HeatReactDef> hs = m->heat; // copy: setMaterial may change m
                for (const auto& h : hs) {
                    if (env.heat < h.at) continue;
                    if (h.noAir && !env.noAir) continue;
                    if (!h.modes.empty() && !contains(h.modes, in.target.mode)) continue;
                    if (h.needsDry && out.p["moisture"] >= d.cfg.dryBelow) continue;
                    bool needs = true;
                    for (const auto& n : h.needs)
                        if (!coTags.count(n)) needs = false;
                    if (!needs) continue;
                    if (!h.when.empty()) {
                        PieceCtx ctx(d, out, out.p, env.heat);
                        if (h.when.eval(ctx) == 0.0) continue;
                    }
                    A.fired.push_back(h.id);
                    if (!h.result.empty()) {
                        A.destroyed = h.result;
                        break;
                    }
                    if (!h.to.empty()) {
                        setMaterial(d, out, h.to);
                        break;
                    }
                    if (!h.toState.empty()) {
                        out.states.insert(h.toState);
                        for (const auto& [k, v] : h.set) out.p[k] = v;
                    }
                }
            }
            if (out.shape == "liquid") out.extracted.insert("water_hot");
            if (!in.target.mode.empty()) out.tags.insert("mode:" + in.target.mode);
        }
        if (!in.target.shape.empty() && in.target.shape != out.shape) setShape(d, out, in.target.shape, in.target.len);
        else if (in.target.len > 0) setShape(d, out, out.shape, in.target.len);
        if (!p.toMaterial.empty()) setMaterial(d, out, p.toMaterial);
        break;
    }
    }

    if (!A.destroyed.empty()) return A;

    for (const auto& mk : p.marks) out.tags.insert(mk);
    if (!p.extract.empty()) out.extracted.insert(p.extract);
    for (const auto& [prop, e] : p.transform) {
        ItemCtx ctx(d, out, env.heat);
        ctx.extra["q"] = qStep;
        ctx.extra["heat"] = env.heat;
        ctx.extra["even"] = env.even;
        ctx.extra["hygiene"] = env.hygiene;
        if (prop == "wear") {
            ctx.extra["x"] = out.wear;
            out.wear = clamp01(e.eval(ctx));
            continue;
        }
        if (out.isAssembly()) continue;
        ctx.extra["x"] = out.p.count(prop) ? out.p[prop] : 0.0;
        out.p[prop] = prop == "melt" ? e.eval(ctx) : clamp01(e.eval(ctx));
    }
    if (!p.setState.empty() && !out.isAssembly()) enterState(d, out, p.setState);
    if (p.reacts || p.op == ProcOp::Blend) applyReacts(d, p, out, env, massShare, A.fired, roll);
    // AL-117: distilling, calcining, dissolving concentrate some effects and lose others;
    // a good step concentrates more
    if (!p.concentrate.empty())
        for (auto& [e, k] : out.effects) {
            double mult = -1.0;
            for (const auto& [pre, f] : p.concentrate)
                if (pre != "*" && e.rfind(pre, 0) == 0) mult = f;
            if (mult < 0)
                for (const auto& [pre, f] : p.concentrate)
                    if (pre == "*") mult = f;
            if (mult < 0) continue;
            if (mult > 1.0) mult = 1.0 + (mult - 1.0) * (0.5 + qStep);
            k = std::min(d.magic.aspectMax, k * mult);
        }
    if (!out.isAssembly())
        if (const MaterialDef* m = d.material(out.material))
            for (const auto& r : m->refine)
                if (r.process == p.id) {
                    PieceCtx ctx(d, out, out.p, env.heat);
                    if (r.when.empty() || r.when.eval(ctx) != 0.0) {
                        auto keepTags = out.tags;
                        setMaterial(d, out, r.to);
                        out.tags.insert(keepTags.begin(), keepTags.end());
                        break;
                    }
                }
    applyStates(d, out, env.heat);
    return A;
}

// The history of every input, in slot order; `refs` gets how each input is
// referred to by the new step: "step:<n>" (1-based, the output of step n of the
// merged history) for a made piece, "mat:<id>" / "form:<id>" for a gathered one.
std::vector<StepRec> mergedTrace(const ProcessDef& p, const StepInput& in,
                                 std::map<std::string, std::vector<std::string>>* refs) {
    std::vector<StepRec> t;
    for (const auto& sl : p.slots) {
        const auto* v = slotItems(in, sl.name);
        if (!v) continue;
        for (const auto& it : *v) {
            t.insert(t.end(), it.trace.begin(), it.trace.end());
            if (refs) (*refs)[sl.name].push_back(it.trace.empty() ? refOf(it) : "step:" + std::to_string(t.size()));
        }
    }
    return t;
}

StepRec recordStep(const ProcessDef& p, const StepInput& in, const StepEnv& env, double q,
                   const std::map<std::string, std::vector<std::string>>& refs) {
    StepRec r;
    r.process = p.id;
    r.slots = refs;
    r.shape = in.target.shape;
    r.len = in.target.len;
    r.mode = in.target.mode;
    r.heat = p.op == ProcOp::Heat ? env.heat : -1;
    r.q = q;
    return r;
}

double triCdf(double x, double m, double w) { // u1+u2-1 scaled by w, centred on m
    if (w <= 0) return x < m ? 0.0 : 1.0;
    double z = (x - m) / w;
    if (z <= -1) return 0.0;
    if (z >= 1) return 1.0;
    if (z < 0) return 0.5 * (z + 1) * (z + 1);
    return 1.0 - 0.5 * (1 - z) * (1 - z);
}

} // namespace

// ---------------------------------------------------------------------------

bool Item::hasFlaw(const std::string& id) const {
    for (const auto& f : flaws)
        if (f.id == id) return true;
    return false;
}

const Item* Item::part(const std::string& role) const {
    for (const auto& p : parts)
        if (p.role == role) return &p.item.front();
    return nullptr;
}

Item rawItem(const CraftData& d, const std::string& material, double q) {
    Item it;
    it.material = material;
    it.q = clamp01(q);
    const MaterialDef* m = d.material(material);
    if (!m) return it;
    it.p = m->p;
    it.shape = m->shape;
    it.len = m->len;
    it.thick = m->thick;
    it.tags.insert(material);
    for (const auto& t : m->tags) it.tags.insert(t);
    for (const auto& e : m->effects) it.effects[e.id] = e.k * m->potency;
    applyStates(d, it, 0);
    return it;
}

double itemMass(const CraftData& d, const Item& it) {
    if (it.isAssembly()) {
        double m = 0.0;
        for (const auto& p : it.parts) m += itemMass(d, p.item.front());
        return m;
    }
    auto f = it.p.find("density");
    double dens = f == it.p.end() || f->second <= 0 ? 0.5 : f->second;
    return dens * it.len * it.thick * it.frac * d.cfg.massUnit;
}

std::map<std::string, double> effectiveProps(const CraftData& d, const Item& it, int heat) {
    return viewOf(d, it, heat).p;
}

std::string qualityTier(const CraftData& d, double q) {
    if (q >= d.cfg.tierMaster) return "masterwork";
    if (q >= d.cfg.tierGood) return "good";
    if (q < d.cfg.tierCrude) return "crude";
    return "common";
}

namespace {
double tagSetWorth(const CraftData& d, const std::set<std::string>& tags) {
    double w = 1.0;
    for (const auto& t : tags) {
        auto f = d.cfg.tagWorth.find(t);
        if (f != d.cfg.tagWorth.end()) w *= f->second;
    }
    return w;
}

double flawsWorth(const CraftData& d, const Item& it) {
    double w = 1.0;
    for (const auto& f : it.flaws)
        if (const FlawDef* fd = d.flaw(f.id)) w *= fd->worth;
    return w;
}

// A piece or a part: what it is made of and how it is shaped, without the archetype.
double partWorth(const CraftData& d, const Item& it) {
    double w = 1.0;
    if (!it.isAssembly()) {
        if (const MaterialDef* m = d.material(it.material)) w *= m->worth;
        if (const ShapeDef* s = d.shape(it.shape)) w *= s->worth;
        w *= tagSetWorth(d, it.tags);
    } else {
        double total = 0.0, acc = 0.0, finest = 0.0;
        std::set<std::string> partTags;
        for (const auto& part : it.parts) {
            const Item& pi = part.item.front();
            const double m = std::max(1e-6, itemMass(d, pi)), pw = partWorth(d, pi);
            total += m;
            acc += pw * m;
            finest = std::max(finest, pw);
            partTags.insert(pi.tags.begin(), pi.tags.end());
        }
        const double avg = total > 0 ? acc / total : 1.0;
        const double sum = d.cfg.worthAvg + d.cfg.worthMax;
        w *= sum > 0 ? (d.cfg.worthAvg * avg + d.cfg.worthMax * finest) / sum : avg;
        if (const ShapeDef* s = d.shape(it.shape)) w *= s->worth;
        std::set<std::string> own; // marks the assembly got itself, not the ones of its parts
        for (const auto& t : it.tags)
            if (!partTags.count(t)) own.insert(t);
        w *= tagSetWorth(d, own);
    }
    return w * flawsWorth(d, it);
}
} // namespace

double tagsWorth(const CraftData& d, const std::vector<std::string>& tags) {
    return tagSetWorth(d, std::set<std::string>(tags.begin(), tags.end()));
}

double itemWorth(const CraftData& d, const Item& it) {
    double w = partWorth(d, it);
    if (const FormDef* f = d.form(it.form)) w *= f->worth;
    std::set<std::string> procs;
    for (const auto& st : it.trace) procs.insert(st.process);
    for (const auto& p : procs)
        if (const ProcessDef* pd = d.process(p)) w *= pd->worth;
    std::set<std::string> dtags(it.derivedTags.begin(), it.derivedTags.end());
    for (const auto& t : it.tags) dtags.erase(t);
    w *= tagSetWorth(d, dtags);
    // AL-117: an enchantment and a story raise the worth (an artifact = rare stuff + strong rune + history)
    w *= 1.0 + d.magic.runeWorth * runePower(d, it);
    w *= 1.0 + d.magic.resonanceWorth * it.resonance;
    return std::clamp(w, d.cfg.worthMin, d.cfg.worthCap);
}

namespace {
void recognizeBase(const CraftData& d, Item& it);
}

void recognize(const CraftData& d, Item& it) {
    recognizeBase(d, it);
    // AL-117: the rune does not change what the thing is; it adds to what it does
    if (it.rune.bound()) {
        it.derivedTags.push_back("enchanted");
        for (const auto& a : runeDoses(d, it))
            it.derived["rune." + runeKey(a.aspect, a.vec)] = a.power;
        it.derivedStr["rune_trigger"] = it.rune.trigger;
    }
    if (it.resonance > 0) it.derived["resonance"] = it.resonance;
}

namespace {
void recognizeBase(const CraftData& d, Item& it) {
    ItemCtx ctx(d, it, 0);
    std::map<std::string, double> fn;
    ctx.fn = &fn;
    std::vector<const ItemFlaw*> fl;
    collectFlaws(it, fl);
    for (const auto& f : d.functions) {
        double v = f.f.eval(ctx);
        for (const auto* x : fl)
            if (const FlawDef* fd = d.flaw(x->id))
                if (auto m = fd->mods.find(f.id); m != fd->mods.end()) v *= m->second;
        fn[f.id] = v;
    }
    it.fn = fn;

    const Item* main = mainPiece(it);
    const MaterialDef* mainMat = d.material(main->material);
    auto hasShape = [&](const std::vector<std::string>& want) {
        if (contains(want, it.shape)) return true;
        for (const auto& p : it.parts)
            if (contains(want, p.item.front().shape)) return true;
        return false;
    };
    const FormDef* best = nullptr;
    double bestScore = 0.0;
    size_t bestCount = 0;
    for (const auto& f : d.forms) {
        if (!f.shape.empty() && !hasShape(f.shape)) continue;
        if (!f.mat.empty()) {
            bool ok = false;
            for (const auto& m : f.mat)
                if (main->material == m || (mainMat && contains(mainMat->tags, m))) ok = true;
            if (!ok) continue;
        }
        bool ok = true;
        for (const auto& s : f.state)
            if (ctx.sym("state", s) == 0.0) ok = false;
        for (const auto& pr : f.profile) {
            bool any = false;
            for (const auto& alt : split(pr, '|'))
                if (it.tags.count(alt)) any = true;
            if (!any) ok = false;
        }
        for (const auto& ef : f.effects) {
            bool any = false;
            for (const auto& alt : split(ef, '|'))
                if (ctx.sym("eff", alt) >= d.cfg.effectPresent) any = true;
            if (!any) ok = false;
        }
        if (!ok) continue;
        double score = 1.0;
        for (const auto& n : f.needs) score *= n.c.closeness(n.expr.eval(ctx), d.cfg.falloff);
        size_t count = f.needs.size() + f.shape.size() + f.mat.size() + f.state.size() + f.profile.size() +
                       f.effects.size();
        if (score < d.cfg.tau) continue;
        if (!best || score > bestScore + 1e-9 || (std::fabs(score - bestScore) <= 1e-9 && count > bestCount)) {
            best = &f;
            bestScore = score;
            bestCount = count;
        }
    }
    it.form.clear();
    it.formScore = 0.0;
    it.kind.clear();
    it.derived.clear();
    it.derivedStr.clear();
    it.derivedTags.clear();
    if (best) {
        it.form = best->id;
        it.formScore = bestScore;
        it.kind = best->kind;
        it.derivedStr = best->deriveStr;
        it.derivedTags = best->deriveTags;
        for (const auto& [k, e] : best->derive) it.derived[k] = e.eval(ctx);
        return;
    }
    const ShapeDef* sd = d.shape(it.shape);
    bool finished = it.isAssembly() || (sd && sd->stage == "finished") ||
                    ((it.shape == "liquid" || it.tags.count("food")) && !it.trace.empty());
    if (!finished) return;
    const ImprovisedDef* pick = nullptr;
    double pv = d.cfg.improvisedMin;
    for (const auto& im : d.improvised) {
        if (im.fn == "any") continue;
        auto f = fn.find(im.fn);
        if (f != fn.end() && f->second >= pv) {
            pv = f->second;
            pick = &im;
        }
    }
    if (!pick)
        for (const auto& im : d.improvised)
            if (im.fn == "any") pick = &im;
    if (pick) it.form = pick->id;
}
} // namespace

Roll::Roll(std::uint64_t seed, std::uint64_t actor, std::uint64_t tick, std::uint64_t step)
    : state(mix64(seed ^ mix64(actor * 0x100000001B3ull ^ mix64(tick * 0x9E37ull ^ mix64(step + 0x51ull))))) {}

double Roll::next() {
    state = mix64(state);
    return static_cast<double>(state >> 11) * (1.0 / 9007199254740992.0);
}

const char* outcomeName(Outcome o) {
    switch (o) {
    case Outcome::NotStarted: return "not_started";
    case Outcome::Catastrophe: return "catastrophe";
    case Outcome::Ruined: return "ruined";
    case Outcome::Flawed: return "flawed";
    case Outcome::Ok: return "ok";
    case Outcome::Good: return "good";
    case Outcome::Masterwork: return "masterwork";
    }
    return "?";
}

Tool toolOf(const CraftData& d, const Item& it) {
    Tool t;
    Item copy = it;
    if (copy.fn.empty()) recognize(d, copy);
    t.tags = copy.derivedTags;
    for (const auto& tag : copy.tags) t.tags.push_back(tag);
    if (copy.form.empty() || copy.form.rfind("improvised_", 0) == 0) {
        if (copy.fn["edge"] >= 0.3 && copy.isAssembly()) t.tags.push_back("cutting");
        if (copy.fn["blunt"] >= 0.3) t.tags.push_back("striking");
    }
    auto p = viewOf(d, *mainPiece(copy), 0).p;
    t.hardness = p["hardness"];
    // a hard stone in the hand is a hammer (hammerstone); a flake of flint cuts
    if (!copy.isAssembly() && copy.tags.count("stone") && t.hardness >= 0.6) t.tags.push_back("striking");
    if (!copy.isAssembly() && copy.form == "" && copy.shape == "blade") t.tags.push_back("cutting");
    t.q = clamp01(copy.q * (1.0 - 0.5 * copy.wear));
    return t;
}

std::vector<Tool> bareTools() { return {Tool{{"hands"}, 0.2, 0.3}}; }

std::string checkStep(const CraftData& d, const StepInput& in, const Workplace& wp, std::string* arg) {
    auto fail = [&](const char* code, const std::string& a = "") {
        if (arg) *arg = a;
        return std::string(code);
    };
    const ProcessDef* p = d.process(in.process);
    if (!p) return fail("unknown_process", in.process);
    StepEnv env = envFor(d, *p, in, wp);
    int total = 0;
    for (const auto& sl : p->slots) {
        const auto* v = slotItems(in, sl.name);
        int n = v ? static_cast<int>(v->size()) : 0;
        total += n;
        if (n < sl.min) return fail("missing_input", sl.name);
        if (n > sl.max) return fail("too_many", sl.name);
        if (v)
            for (const auto& it : *v)
                if (!slotMatches(d, sl, it, std::max(env.heat, p->heatMin > 0 ? env.heat : 0)))
                    return fail("wrong_input", sl.name);
    }
    for (const auto& [slot, items] : in.slots) {
        bool known = false;
        for (const auto& sl : p->slots)
            if (sl.name == slot) known = true;
        if (!known && !items.empty()) return fail("unknown_slot", slot);
    }
    if (total > d.cfg.maxInputsPerStep + 2) return fail("too_many", "");
    if (!p->tools.empty()) {
        bool ok = false;
        for (const auto& tl : wp.tools)
            for (const auto& t : p->tools)
                if (contains(tl.tags, t)) ok = true;
        if (!ok) return fail("no_tool", p->tools.front());
        if (!p->toolHardness.empty()) {
            const Item* work = workPiece(*p, in);
            if (work) {
                ItemCtx ctx(d, *work, env.heat);
                double need = p->toolHardness.eval(ctx);
                bool hard = false;
                for (const auto& tl : wp.tools)
                    for (const auto& t : p->tools)
                        if (contains(tl.tags, t) && tl.hardness >= need) hard = true;
                if (!hard) return fail("tool_too_soft", p->tools.front());
            }
        }
    }
    auto st = workingStations(d, wp);
    if (!p->stations.empty()) {
        bool ok = false;
        for (const auto* s : st)
            for (const auto& t : p->stations)
                if (stationHasTag(*s, t)) ok = true;
        if (!ok) {
            for (const auto& id : wp.stations)
                if (const StationDef* s = d.station(id))
                    for (const auto& t : p->stations)
                        if (stationHasTag(*s, t) && !s->needs.empty()) return fail("station_needs", s->needs.front());
            return fail("no_station", p->stations.front());
        }
    }
    if (p->heatMin > 0 && env.heat < p->heatMin) return fail("too_cold", std::to_string(p->heatMin));
    if (!in.target.mode.empty() && !contains(p->modes, in.target.mode)) return fail("bad_mode", in.target.mode);
    const Item* work = workPiece(*p, in);
    if (work && !in.target.shape.empty() && in.target.shape != work->shape &&
        (p->op == ProcOp::Shape || p->op == ProcOp::Heat || p->op == ProcOp::Soak)) {
        auto m = p->shapeMap.find(work->shape);
        if (m == p->shapeMap.end() || !contains(m->second, in.target.shape)) return fail("bad_target", in.target.shape);
    }
    if (p->op == ProcOp::Shape && !p->shapeMap.empty() && work && in.target.shape.empty() &&
        !p->shapeMap.count(work->shape) && p->transform.empty() && p->setState.empty() && p->toMaterial.empty() &&
        p->fixes.empty())
        return fail("bad_target", work->shape);
    if (p->op == ProcOp::Inscribe && work) { // AL-117: the carrier must hold magic
        if (carrierOf(d, *work).conductivity + 1e-9 < d.magic.runeMinConductivity)
            return fail("carrier_cannot_hold", mainPiece(*work)->material);
    }
    if (p->op == ProcOp::Bind) {
        if (work && !work->inscribed) return fail("no_rune", "");
        bool any = false;
        for (const auto& sl : p->slots) {
            const auto* v = slotItems(in, sl.name);
            if (!v) continue;
            for (size_t j = 0; j < v->size(); ++j)
                if (&(*v)[j] != work && !aspectDoses((*v)[j]).empty()) any = true;
        }
        if (!any) return fail("no_aspect", "");
    }
    if (p->op == ProcOp::Join) {
        size_t parts = 0;
        for (const auto& [slot, items] : in.slots)
            for (const auto& it : items) parts += it.isAssembly() ? it.parts.size() : 1;
        if (static_cast<int>(parts) > d.cfg.maxPartsPerItem) return fail("too_many_parts", "");
    }
    return "";
}

StepResult runStep(const CraftData& d, const StepInput& in, const Actor& a, const Workplace& wp, std::uint64_t seed,
                   std::uint64_t tick, int stepNo) {
    StepResult R;
    R.reason = checkStep(d, in, wp, &R.reasonArg);
    if (!R.reason.empty()) return R;
    const ProcessDef& p = *d.process(in.process);
    StepEnv env = envFor(d, p, in, wp);
    Numbers n = numbersFor(d, p, in, a, wp, env);
    Roll roll(seed, a.id, tick, static_cast<std::uint64_t>(stepNo));
    double z = n.m + d.cfg.noise * (roll.next() + roll.next() - 1.0);
    R.outcome = outcomeOf(d, p, z);
    R.difficulty = n.d;
    R.competence = n.c;
    R.margin = n.m;
    // step quality: rises with the margin and saturates softly, so a master's
    // work still varies a little from piece to piece
    double q = 0.5 + 0.45 * std::tanh(2.0 * d.cfg.k * n.m) + d.cfg.noise * (roll.next() - 0.5);
    if (R.outcome == Outcome::Masterwork) q += d.cfg.stepQualityMasterBonus;
    if (R.outcome == Outcome::Flawed) q = std::min(q, 0.45);
    q += traitMod(d, a, "quality");
    R.qStep = clamp01(q);
    R.time = std::max(1, static_cast<int>(std::lround(p.time * (1.0 - traitMod(d, a, "speed")))));
    R.toolWear = p.tools.empty() ? 0.0 : p.toolWear * (1.5 - R.qStep);

    // risks
    double riskMul = std::max(0.0, 1.0 + traitMod(d, a, "risk"));
    for (const auto& r : p.risks) {
        bool fire = R.outcome == Outcome::Catastrophe || roll.next() < r.p * riskMul;
        if (fire) {
            R.risks.push_back(r.id);
            R.riskEffects.insert(R.riskEffects.end(), r.eff.begin(), r.eff.end());
        }
    }

    // experience (§4.4.7): reasonable challenges teach most; failures teach less
    double skillNow = d.cfg.novice;
    if (auto it = a.skills.find(n.skill); it != a.skills.end()) skillNow = it->second;
    double xp = d.cfg.xpRate * (1.0 - std::min(1.0, skillNow)) * (std::fabs(n.m) <= d.cfg.xpWindow ? 1.0 : 0.3);
    if (R.outcome == Outcome::Ruined || R.outcome == Outcome::Catastrophe) xp *= d.cfg.xpFailRatio;
    xp *= 1.0 + traitMod(d, a, "xp");
    R.skillXp[n.skill] = xp;
    for (const auto& [slot, items] : in.slots)
        for (const auto& it : items) {
            const std::string& m = mainPiece(it)->material;
            auto k = a.knowsMaterial.find(m);
            double know = k == a.knowsMaterial.end() ? 0.0 : k->second;
            R.knowGain[m] = std::max(R.knowGain[m], d.cfg.knowGain * (1.0 - know));
        }

    auto salvageAll = [&](const std::string& forced) {
        for (const auto& sl : p.slots) {
            const auto* v = slotItems(in, sl.name);
            if (!v) continue;
            for (const auto& it : *v) {
                if (it.shape == "liquid") continue;
                std::string mat = forced;
                if (mat.empty())
                    for (const auto& f : it.flaws)
                        if (const FlawDef* fd = d.flaw(f.id))
                            if (!fd->salvage.empty()) mat = fd->salvage;
                if (mat.empty())
                    if (const MaterialDef* md = d.material(mainPiece(it)->material)) mat = md->salvage;
                if (mat.empty()) mat = mainPiece(it)->material;
                Item s = rawItem(d, mat, std::min(it.q, 0.3));
                s.shape = "scrap";
                s.len = 1;
                s.thick = 1;
                double share = d.cfg.salvageMin + (d.cfg.salvageMax - d.cfg.salvageMin) * roll.next();
                double dens = s.p.count("density") && s.p["density"] > 0 ? s.p["density"] : 0.5;
                s.frac = std::max(0.05, share * itemMass(d, it) / (dens * d.cfg.massUnit));
                recognize(d, s);
                R.salvage.push_back(s);
            }
        }
    };

    if (R.outcome == Outcome::Ruined || R.outcome == Outcome::Catastrophe) {
        salvageAll("");
        return R;
    }

    Applied A = applyStep(d, p, in, env, R.qStep, &roll);
    if (!A.destroyed.empty()) {
        R.outcome = Outcome::Ruined;
        salvageAll(A.destroyed);
        return R;
    }
    Item& out = A.out;
    // AL-117: opposed aspects fight in the vessel; a poor step lets them blow it apart
    if (const double un = instability(d, out); un > d.magic.unstableSafe) {
        const double pb = clamp01((un - d.magic.unstableSafe) * d.magic.blastK * (1.2 - R.qStep) * riskMul);
        if (roll.next() < pb) {
            R.outcome = Outcome::Catastrophe;
            R.risks.push_back("blast");
            R.riskEffects.insert(R.riskEffects.end(), d.magic.blastEffects.begin(), d.magic.blastEffects.end());
            salvageAll("");
            return R;
        }
    }
    std::map<std::string, std::vector<std::string>> refs;
    std::vector<StepRec> trace = mergedTrace(p, in, &refs);
    addQ(out, R.qStep);

    // flaws
    std::vector<std::string> before;
    for (const auto& f : out.flaws) before.push_back(f.id);
    if (R.qStep >= 0.5)
        for (const auto& fx : p.fixes)
            out.flaws.erase(std::remove_if(out.flaws.begin(), out.flaws.end(),
                                           [&](const ItemFlaw& f) { return f.id == fx; }),
                            out.flaws.end());
    const FlawRule* likeliest = nullptr;
    double likeliestP = -1.0;
    for (const auto& fr : p.flaws) {
        ItemCtx ctx(d, out, env.heat);
        ctx.extra["q"] = R.qStep;
        ctx.extra["heat"] = env.heat;
        ctx.extra["even"] = env.even;
        ctx.extra["hygiene"] = env.hygiene;
        if (!fr.when.empty() && fr.when.eval(ctx) == 0.0) continue;
        double pr = clamp01(fr.p.eval(ctx) * riskMul);
        if (pr > likeliestP) {
            likeliestP = pr;
            likeliest = &fr;
        }
        if (roll.next() < pr) addFlaw(out, fr.id);
    }
    if (R.outcome == Outcome::Flawed && likeliest && out.flaws.size() == before.size()) addFlaw(out, likeliest->id);
    for (const auto& f : out.flaws)
        if (!contains(before, f.id)) R.newFlaws.push_back(f.id);
    for (auto& f : out.flaws)
        if (const FlawDef* fd = d.flaw(f.id))
            if (fd->visible) f.revealed = true;

    out.trace = trace;
    out.trace.push_back(recordStep(p, in, env, R.qStep, refs));
    out.maker = a.id;
    recognize(d, out);
    out.trace.back().outForm = out.form;
    out.trace.back().outShape = out.shape;
    out.trace.back().outMat = mainPiece(out)->material;
    R.out = out;
    R.hasOut = true;
    return R;
}

Preview previewStep(const CraftData& d, const StepInput& in, const Actor& a, const Workplace& wp) {
    Preview pv;
    pv.reason = checkStep(d, in, wp, &pv.reasonArg);
    if (!pv.reason.empty()) return pv;
    pv.canStart = true;
    const ProcessDef& p = *d.process(in.process);
    StepEnv env = envFor(d, p, in, wp);
    Numbers n = numbersFor(d, p, in, a, wp, env);
    pv.time = std::max(1, static_cast<int>(std::lround(p.time * (1.0 - traitMod(d, a, "speed")))));
    pv.pRuin = triCdf(d.cfg.tRuin, n.m, d.cfg.noise);
    pv.pFlaw = triCdf(d.cfg.tOk, n.m, d.cfg.noise) - pv.pRuin;
    pv.pGood = 1.0 - triCdf(d.cfg.tGood, n.m, d.cfg.noise);
    pv.expectedQ = clamp01(0.5 + 0.45 * std::tanh(2.0 * d.cfg.k * n.m) + traitMod(d, a, "quality"));
    double skill = d.cfg.novice;
    if (auto it = a.skills.find(n.skill); it != a.skills.end()) skill = it->second;
    double know = 0.0;
    int cnt = 0;
    for (const auto& [slot, items] : in.slots)
        for (const auto& it : items) {
            auto k = a.knowsMaterial.find(mainPiece(it)->material);
            know += k == a.knowsMaterial.end() ? 0.0 : k->second;
            ++cnt;
        }
    if (cnt) know /= cnt;
    pv.certainty = clamp01(0.6 * skill + 0.4 * know + 0.3 * a.repeat);
    // the blur (§9): a novice sees a wide spread and no name
    pv.qSpread = (1.0 - pv.certainty) * 0.4 + d.cfg.noise * d.cfg.k * 0.5;
    if (pv.certainty >= 0.35) {
        Applied A = applyStep(d, p, in, env, pv.expectedQ, nullptr);
        if (A.destroyed.empty()) {
            Item out = A.out;
            addQ(out, pv.expectedQ);
            std::map<std::string, std::vector<std::string>> refs;
            out.trace = mergedTrace(p, in, &refs);
            out.trace.push_back(recordStep(p, in, env, pv.expectedQ, refs));
            recognize(d, out);
            pv.likelyForm = out.form;
        }
    }
    return pv;
}

int inspectItem(const CraftData& d, Item& it, double skill, std::uint64_t seed, std::uint64_t tick, std::uint64_t actor) {
    (void)d;
    Roll roll(seed, actor, tick, 0xA11ull);
    int found = 0;
    std::function<void(Item&)> walk = [&](Item& x) {
        for (auto& f : x.flaws)
            if (!f.revealed && roll.next() < 0.25 + 0.65 * clamp01(skill)) {
                f.revealed = true;
                ++found;
            }
        for (auto& p : x.parts) walk(p.item.front());
    };
    walk(it);
    return found;
}

} // namespace lr::sim::craft
