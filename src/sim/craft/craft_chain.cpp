#include "sim/craft/craft_chain.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <sstream>

namespace lr::sim::craft {

namespace {

double clamp01(double v) { return std::min(1.0, std::max(0.0, v)); }

std::uint64_t fnv(const std::string& s) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

bool isStepRef(const std::string& r, int& n) {
    if (r.rfind("step:", 0) != 0) return false;
    n = std::atoi(r.c_str() + 5);
    return n > 0;
}

bool rawOf(const CraftData& d, const Item& it, const std::string& mat) {
    if (it.isAssembly() || it.material != mat) return false;
    const MaterialDef* m = d.material(mat);
    return m && it.shape == m->shape && it.trace.empty();
}

bool matchesRef(const CraftData& d, const Item& it, const std::string& ref) {
    if (ref.rfind("mat:", 0) == 0) return rawOf(d, it, ref.substr(4));
    if (ref.rfind("form:", 0) == 0) return it.form == ref.substr(5);
    if (ref.rfind("shape:", 0) == 0) return it.shape == ref.substr(6);
    if (ref.rfind("tag:", 0) == 0) return it.tags.count(ref.substr(4)) > 0;
    return false;
}

bool matchesOut(const Item& it, const RecipeStep& s) {
    if (!s.outForm.empty() && s.outForm.rfind("improvised_", 0) != 0) return it.form == s.outForm;
    if (s.outShape.empty()) return false;
    if (it.trace.empty() || it.shape != s.outShape || it.material != s.outMat) return false;
    // AL-83: a stored thing stands for an intermediate stage only when that stage
    // made it: a finished nut bread is lump + flour too, but it is not the dough
    // (taking it re-baked the same bread forever, one step longer each time)
    if (!it.form.empty() && it.form.rfind("improvised_", 0) != 0) return false;
    if (!s.outForm.empty() && it.form != s.outForm) return false;
    return it.trace.back().process == s.step.process;
}

} // namespace

int Recipe::knownCount() const {
    int n = 0;
    for (const auto& s : steps)
        if (s.known) ++n;
    return n;
}

Recipe* RecipeBook::find(const std::string& id) {
    for (auto& r : recipes)
        if (r.id == id) return &r;
    return nullptr;
}
const Recipe* RecipeBook::find(const std::string& id) const {
    for (const auto& r : recipes)
        if (r.id == id) return &r;
    return nullptr;
}
Recipe* RecipeBook::byForm(const std::string& form) {
    Recipe* best = nullptr;
    for (auto& r : recipes)
        if (r.form == form && (!best || r.mastery > best->mastery)) best = &r;
    return best;
}

std::vector<RecipeStep> stepsOf(const Item& it) {
    std::vector<RecipeStep> out;
    for (const auto& t : it.trace) {
        RecipeStep s;
        s.step.process = t.process;
        for (const auto& [slot, refs] : t.slots) {
            // the same raw ref repeated becomes one input with a count
            std::vector<DesignInput> ins;
            for (const auto& r : refs) {
                if (!ins.empty() && ins.back().ref == r && r.rfind("step:", 0) != 0) ++ins.back().count;
                else ins.push_back({r, 1});
            }
            s.step.slots[slot] = ins;
        }
        s.step.shape = t.shape;
        s.step.len = t.len;
        s.step.mode = t.mode;
        s.step.heat = t.heat;
        s.outForm = t.outForm;
        s.outShape = t.outShape;
        s.outMat = t.outMat;
        out.push_back(s);
    }
    return out;
}

std::string recipeIdOf(const std::vector<RecipeStep>& steps) {
    std::ostringstream os;
    for (const auto& s : steps) {
        os << s.step.process << "|" << s.step.shape << "|" << s.step.len << "|" << s.step.mode << ";";
        for (const auto& [slot, ins] : s.step.slots) {
            os << slot << "=";
            for (const auto& i : ins) os << i.ref << "x" << i.count << ",";
        }
        os << "/";
    }
    std::ostringstream id;
    id << "r_" << std::hex << (fnv(os.str()) & 0xffffffffull);
    return id.str();
}

Recipe& learnFromItem(const CraftData& d, RecipeBook& book, const Item& made, std::uint64_t seed,
                      std::uint64_t actor) {
    std::vector<RecipeStep> steps = stepsOf(made);
    std::string id = recipeIdOf(steps);
    Roll roll(seed, actor, fnv(id), 0xB00Cull);
    Recipe* r = book.find(id);
    double gain = d.cfg.masteryPerSuccess * (0.5 + made.q);
    if (!r) {
        Recipe n;
        n.id = id;
        n.steps = steps;
        n.form = made.form;
        if (!made.form.empty() && made.form.rfind("improvised_", 0) != 0) n.nameKey = "form." + made.form + ".name";
        // §17.2: the first success leaves a fragmentary record; the last stage
        // is always remembered, about a third of the others
        if (!n.steps.empty()) n.steps.back().known = true;
        int want = std::max(0, static_cast<int>(n.steps.size()) / 3);
        for (size_t i = 0; i + 1 < n.steps.size() && want > 0; ++i)
            if (roll.next() < 0.5) {
                n.steps[i].known = true;
                --want;
            }
        book.recipes.push_back(n);
        r = &book.recipes.back();
        r->mastery = clamp01(gain * 0.5);
    } else {
        r->mastery = clamp01(r->mastery + gain);
        for (int k = 0; k < d.cfg.fragmentsPerSuccess; ++k) {
            std::vector<size_t> unknown;
            for (size_t i = 0; i < r->steps.size(); ++i)
                if (!r->steps[i].known) unknown.push_back(i);
            if (unknown.empty()) break;
            r->steps[unknown[static_cast<size_t>(roll.next() * unknown.size()) % unknown.size()]].known = true;
        }
        if (!made.form.empty()) r->form = made.form;
    }
    r->successes += 1;
    r->bestQ = std::max(r->bestQ, made.q);
    if (r->master(d.cfg))
        for (auto& s : r->steps) s.known = true;
    return *r;
}

Recipe teachRecipe(const CraftData& d, const Recipe& r, double teacherSkill, double pupilSkill) {
    Recipe n = r;
    n.mastery = std::min(r.mastery, clamp01(teacherSkill * d.cfg.teachShare));
    n.successes = 0;
    n.bestQ = 0.0;
    n.notes.clear();
    size_t known = static_cast<size_t>(std::ceil(n.steps.size() * clamp01(0.3 + 0.5 * pupilSkill)));
    for (size_t i = 0; i < n.steps.size(); ++i) n.steps[i].known = r.steps[i].known && (i < known || i + 1 == n.steps.size());
    return n;
}

Recipe recipeFromDesign(const CraftData& d, const DesignDef& ds, double mastery, bool allKnown) {
    Recipe r;
    for (const auto& s : ds.steps) {
        RecipeStep rs;
        rs.step = s;
        rs.known = allKnown;
        r.steps.push_back(rs);
    }
    if (!r.steps.empty()) {
        r.steps.back().outForm = ds.form;
        r.steps.back().known = true;
    }
    // what each stage gives (a stored one can replace it in fast craft): a dry
    // run by a master in a full workshop
    {
        Workplace wp;
        for (const auto& [id, s] : d.stations) wp.stations.push_back(id);
        Tool t;
        for (const auto& f : d.forms) t.tags.insert(t.tags.end(), f.deriveTags.begin(), f.deriveTags.end());
        t.tags.insert(t.tags.end(), {"cutting", "striking", "grit", "bellows"});
        t.hardness = 1.0;
        t.q = 0.8;
        wp.tools.push_back(t);
        Actor a;
        for (const auto& [pid, p] : d.processes) {
            a.skills[p.skill] = 1.0;
            for (const auto& [tag, sk] : p.skillBy) a.skills[sk] = 1.0;
        }
        std::vector<Item> none;
        ChainOpts o;
        o.rawFromNothing = true;
        o.stopOnRisk = false;
        o.maxStages = 1000;
        ChainRun run = runChain(d, r.steps, none, a, wp, 0x5EEDull, 0, o);
        for (size_t i = 0; i < run.results.size() && i < run.ran.size(); ++i) {
            const StepResult& sr = run.results[i];
            if (!sr.hasOut || sr.out.trace.empty()) continue;
            RecipeStep& rs = r.steps[static_cast<size_t>(run.ran[i])];
            if (rs.outForm.empty()) rs.outForm = sr.out.trace.back().outForm;
            rs.outShape = sr.out.trace.back().outShape;
            rs.outMat = sr.out.trace.back().outMat;
        }
    }
    r.id = recipeIdOf(r.steps);
    r.form = ds.form;
    if (d.form(ds.form)) r.nameKey = "form." + ds.form + ".name";
    r.mastery = clamp01(mastery);
    r.successes = allKnown ? d.cfg.masterSuccesses : 0;
    return r;
}

namespace {

struct Resolver {
    const CraftData& d;
    const std::vector<RecipeStep>& steps;
    const std::vector<Item>& pool;
    std::vector<bool> taken;
    std::vector<int> need;      // 1 = run, 2 = from the store
    std::vector<int> fromPool;  // pool index for stage k when need == 2
    Resolver(const CraftData& dd, const std::vector<RecipeStep>& s, const std::vector<Item>& p)
        : d(dd), steps(s), pool(p), taken(p.size(), false), need(s.size(), 0), fromPool(s.size(), -1) {}

    void want(int k, bool final) {
        if (k < 0 || k >= static_cast<int>(steps.size()) || need[k]) return;
        if (!final)
            for (size_t i = 0; i < pool.size(); ++i)
                if (!taken[i] && matchesOut(pool[i], steps[k])) {
                    taken[i] = true;
                    need[k] = 2;
                    fromPool[k] = static_cast<int>(i);
                    return;
                }
        need[k] = 1;
        for (const auto& [slot, ins] : steps[k].step.slots)
            for (const auto& in : ins) {
                int n = 0;
                if (isStepRef(in.ref, n)) want(n - 1, false);
            }
    }
    int stages() const {
        int c = 0;
        for (int v : need)
            if (v == 1) ++c;
        return c;
    }
};

} // namespace

ChainRun runChain(const CraftData& d, const std::vector<RecipeStep>& steps, std::vector<Item>& pool, const Actor& a,
                  const Workplace& wp, std::uint64_t seed, std::uint64_t tick, const ChainOpts& opt) {
    ChainRun run;
    if (steps.empty()) {
        run.stop = "unknown_step";
        return run;
    }
    Resolver res(d, steps, pool);
    res.want(static_cast<int>(steps.size()) - 1, true);
    if (res.stages() > opt.maxStages) {
        run.stop = "too_long";
        run.stopArg = std::to_string(res.stages());
        return run;
    }
    std::vector<bool> taken = res.taken;
    std::vector<Item> produced(steps.size());
    std::vector<bool> have(steps.size(), false);
    for (size_t k = 0; k < steps.size(); ++k)
        if (res.need[k] == 2) {
            produced[k] = pool[res.fromPool[k]];
            have[k] = true;
            run.skipped.push_back(static_cast<int>(k));
        }
    std::vector<bool> used(steps.size(), false);

    auto finish = [&](bool ok) {
        // what was taken leaves the pool; made but unused things go back to it
        std::vector<Item> rest;
        for (size_t i = 0; i < pool.size(); ++i)
            if (!taken[i]) rest.push_back(pool[i]);
        for (size_t k = 0; k < steps.size(); ++k)
            if (have[k] && !used[k] && !(ok && k + 1 == steps.size())) {
                rest.push_back(produced[k]);
                run.leftovers.push_back(produced[k]);
            }
        pool = rest;
    };

    int stepNo = 0;
    for (size_t k = 0; k < steps.size(); ++k) {
        if (res.need[k] != 1) continue;
        const RecipeStep& rs = steps[k];
        StepInput in;
        in.process = rs.step.process;
        in.target.shape = rs.step.shape;
        in.target.len = rs.step.len;
        in.target.mode = rs.step.mode;
        in.target.heat = rs.step.heat;
        std::string missing;
        std::vector<size_t> mine;  // pool items this step took
        std::vector<int> myRefs;   // stages whose output it took
        for (const auto& [slot, ins] : rs.step.slots)
            for (const auto& di : ins)
                for (int c = 0; c < std::max(1, di.count); ++c) {
                    int n = 0;
                    if (isStepRef(di.ref, n)) {
                        if (n - 1 >= 0 && n - 1 < static_cast<int>(steps.size()) && have[n - 1] && !used[n - 1]) {
                            in.slots[slot].push_back(produced[n - 1]);
                            used[n - 1] = true;
                            myRefs.push_back(n - 1);
                        } else {
                            missing = di.ref;
                        }
                        continue;
                    }
                    bool found = false;
                    for (size_t i = 0; i < pool.size() && !found; ++i)
                        if (!taken[i] && matchesRef(d, pool[i], di.ref)) {
                            taken[i] = true;
                            mine.push_back(i);
                            in.slots[slot].push_back(pool[i]);
                            found = true;
                        }
                    if (!found && opt.rawFromNothing && di.ref.rfind("mat:", 0) == 0 && d.material(di.ref.substr(4))) {
                        in.slots[slot].push_back(rawItem(d, di.ref.substr(4), opt.rawQ));
                        found = true;
                    }
                    if (!found) missing = di.ref;
                }
        auto giveBack = [&]() { // nothing of this step was consumed
            for (size_t i : mine) taken[i] = false;
            for (int j : myRefs) used[j] = false;
        };
        if (!missing.empty()) {
            run.stop = "missing";
            run.stopArg = missing;
            run.stopStep = static_cast<int>(k);
            giveBack();
            finish(false);
            return run;
        }
        Actor act = a;
        if (!rs.known) act.repeat -= opt.fillUnknownPenalty / std::max(0.01, d.cfg.repeatBonus);
        StepResult r = runStep(d, in, act, wp, seed, tick + static_cast<std::uint64_t>(run.time), stepNo++);
        run.time += r.time;
        run.ran.push_back(static_cast<int>(k));
        run.results.push_back(r);
        if (r.outcome == Outcome::NotStarted) {
            run.stop = "cannot_start";
            run.stopArg = r.reason + (r.reasonArg.empty() ? "" : ":" + r.reasonArg);
            run.stopStep = static_cast<int>(k);
            run.time -= r.time;
            giveBack();
            finish(false);
            return run;
        }
        if (r.outcome == Outcome::Ruined || r.outcome == Outcome::Catastrophe) {
            run.stop = "ruined";
            run.stopStep = static_cast<int>(k);
            for (const auto& sv : r.salvage) run.leftovers.push_back(sv);
            finish(false);
            for (const auto& sv : r.salvage) pool.push_back(sv);
            return run;
        }
        produced[k] = r.out;
        have[k] = true;
        if (opt.stopOnRisk && !r.risks.empty() && k + 1 < steps.size()) {
            run.stop = "hurt";
            run.stopArg = r.risks.front();
            run.stopStep = static_cast<int>(k);
            finish(false);
            return run;
        }
    }
    run.done = true;
    run.out = produced.back();
    finish(true);
    return run;
}

ChainPlan planChain(const CraftData& d, const std::vector<RecipeStep>& steps, const std::vector<Item>& pool,
                    const Actor& a, const Workplace& wp, const ChainOpts& opt) {
    ChainPlan plan;
    if (steps.empty()) {
        plan.ok = false;
        return plan;
    }
    Resolver res(d, steps, pool);
    res.want(static_cast<int>(steps.size()) - 1, true);
    plan.stages = res.stages();
    if (plan.stages > opt.maxStages) plan.ok = false;
    std::vector<bool> taken = res.taken;
    std::set<std::string> toolTags, stTags;
    for (const auto& t : wp.tools) toolTags.insert(t.tags.begin(), t.tags.end());
    for (const auto& id : wp.stations)
        if (const StationDef* s = d.station(id)) stTags.insert(s->tags.begin(), s->tags.end());
    for (size_t k = 0; k < steps.size(); ++k) {
        if (res.need[k] != 1) continue;
        const ProcessDef* p = d.process(steps[k].step.process);
        if (!p) {
            plan.ok = false;
            plan.missing.push_back("process:" + steps[k].step.process);
            continue;
        }
        plan.time += p->time;
        if (!p->tools.empty()) {
            bool ok = false;
            for (const auto& t : p->tools)
                if (toolTags.count(t)) ok = true;
            if (!ok) plan.needTools.push_back(p->tools.front());
        }
        if (!p->stations.empty()) {
            bool ok = false;
            for (const auto& t : p->stations)
                if (stTags.count(t)) ok = true;
            if (!ok) plan.needStations.push_back(p->stations.front());
        }
        for (const auto& [slot, ins] : steps[k].step.slots)
            for (const auto& di : ins) {
                int n = 0;
                if (isStepRef(di.ref, n)) continue;
                for (int c = 0; c < std::max(1, di.count); ++c) {
                    bool found = false;
                    for (size_t i = 0; i < pool.size() && !found; ++i)
                        if (!taken[i] && matchesRef(d, pool[i], di.ref)) {
                            taken[i] = true;
                            found = true;
                        }
                    if (!found && !opt.rawFromNothing) plan.missing.push_back(di.ref);
                }
            }
    }
    if (!plan.missing.empty() || !plan.needTools.empty() || !plan.needStations.empty()) plan.ok = false;
    // expected quality: a dry run with gathered stand-ins, averaged over a few seeds
    double sum = 0.0;
    int n = 0;
    for (std::uint64_t s = 1; s <= 3; ++s) {
        std::vector<Item> copy = pool;
        ChainOpts o = opt;
        o.rawFromNothing = true;
        o.stopOnRisk = false;
        ChainRun r = runChain(d, steps, copy, a, wp, 0xD2A7ull * s, 0, o);
        sum += r.done ? r.out.q : 0.0;
        ++n;
    }
    plan.expectedQ = n ? sum / n : 0.0;
    return plan;
}

double breakthroughChance(const CraftData& d, const Actor& a, double skill, double novelty) {
    double base = d.cfg.brNovice + (d.cfg.brExpert - d.cfg.brNovice) * clamp01(skill);
    double trait = 0.0;
    for (const auto& t : a.traits) {
        auto it = d.cfg.traitMods.find(t);
        if (it == d.cfg.traitMods.end()) continue;
        auto k = it->second.find("breakthrough");
        if (k != it->second.end()) trait += k->second;
    }
    double p = base * std::max(0.0, 1.0 + trait) * (1.0 + (d.cfg.brNoveltyBoost - 1.0) * clamp01(novelty));
    return std::min(d.cfg.brCap, p);
}

} // namespace lr::sim::craft
