#include "sim/craft_sim.hpp"
#include "sim/alchemy.hpp"
#include "sim/memory_budget.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "sim/ai.hpp"
#include "sim/rules.hpp"

namespace altera::sim {

namespace {

using namespace craft;

double clamp01(double v) { return std::min(1.0, std::max(0.0, v)); }

// tool tags a sim item gives: from the archetype that names it (forms[].item)
std::vector<std::string> itemToolTags(const CraftData& d, const std::string& item) {
    std::vector<std::string> tags;
    for (const auto& f : d.forms)
        if (f.item == item) tags.insert(tags.end(), f.deriveTags.begin(), f.deriveTags.end());
    return tags;
}

bool takeRef(World& w, Character& c, const std::string& ref, Item& out, std::vector<std::pair<std::string, int>>& inv,
             std::vector<int>& made) {
    const CraftData& d = w.content().craft;
    if (ref.rfind("inv:", 0) == 0) {
        const std::string item = ref.substr(4);
        auto def = w.content().items.find(item);
        if (def == w.content().items.end() || def->second.material.empty()) return false;
        int already = 0;
        for (const auto& [k, n] : inv)
            if (k == item) already += n;
        if (c.has(item) - already < 1) return false;
        inv.emplace_back(item, 1);
        out = rawItem(d, def->second.material, 0.5);
        return true;
    }
    if (ref.rfind("made:", 0) == 0) {
        int n = std::atoi(ref.c_str() + 5);
        if (n < 0 || n >= static_cast<int>(c.craft.items.size())) return false;
        if (std::find(made.begin(), made.end(), n) != made.end()) return false;
        made.push_back(n);
        out = c.craft.items[static_cast<size_t>(n)];
        return true;
    }
    return false;
}

std::string formItem(const CraftData& d, const Item& it) {
    const FormDef* f = d.form(it.form);
    return f ? f->item : std::string();
}

// Remove made things (by index, highest first) and inventory units.
void consume(Character& c, std::vector<int> made, const std::vector<std::pair<std::string, int>>& inv,
             const CraftData& d) {
    std::sort(made.rbegin(), made.rend());
    for (int i : made) {
        const std::string it = formItem(d, c.craft.items[static_cast<size_t>(i)]);
        if (!it.empty() && c.inventory[it] > 0) --c.inventory[it];
        c.craft.items.erase(c.craft.items.begin() + i);
    }
    for (const auto& [item, n] : inv) c.inventory[item] = std::max(0, c.inventory[item] - n);
}

int keep(Character& c, const Item& it, const CraftData& d, std::string* itemOut) {
    c.craft.items.push_back(it);
    const std::string item = formItem(d, it);
    if (!item.empty()) c.inventory[item] += 1;
    if (itemOut) *itemOut = item;
    return static_cast<int>(c.craft.items.size()) - 1;
}

void applyRisks(World& w, Character& c, const std::vector<std::string>& effs) {
    std::vector<Effect> list;
    for (const auto& s : effs) {
        try {
            list.push_back(parseEffect(s));
        } catch (...) {
        }
    }
    if (!list.empty()) applyEffectList(w, c, list, nullptr);
}

void learnFromStep(Character& c, const StepResult& r) {
    for (const auto& [sk, xp] : r.skillXp)
        if (!sk.empty()) c.skills[sk] = std::min(1.0, c.skills[sk] + xp);
    for (const auto& [m, g] : r.knowGain) c.craft.knowsMaterial[m] = std::min(1.0, c.craft.knowsMaterial[m] + g);
}

std::string signatureOf(const Item& it) {
    std::vector<RecipeStep> st = stepsOf(it);
    return recipeIdOf(st);
}

// A finished thing teaches the book (§17.2); an untried way may be a breakthrough (§18).
void afterSuccess(World& w, Character& c, Item& made, CraftOutcome& out) {
    const CraftData& d = w.content().craft;
    if (made.form.empty()) return; // an intermediate: nothing to remember yet
    // AL-83: a chain longer than the book holds is not written down
    if (static_cast<int>(stepsOf(made).size()) > memoryBudget(w.content()).maxRecipeSteps) return;
    const std::string sig = signatureOf(made);
    int& tried = c.craft.tried[sig];
    const bool known = c.craft.book.find(sig) != nullptr;
    double novelty = known ? 0.0 : 1.0 / (1.0 + tried);
    ++tried;
    const ProcessDef* last = made.trace.empty() ? nullptr : d.process(made.trace.back().process);
    double skill = last && c.skills.count(last->skill) ? c.skills.at(last->skill) : d.cfg.novice;
    const Tick cooldown = static_cast<Tick>(d.cfg.brCooldownDays) * kTicksPerDay;
    bool cooled = c.craft.lastBreakthrough < 0 || w.now() - c.craft.lastBreakthrough >= cooldown;
    if (novelty > 0 && cooled) {
        Rng r = w.rng(c.id, "craft:breakthrough:" + sig);
        if (r.uniform() < breakthroughChance(d, craftActor(w, c), skill, novelty)) {
            out.breakthrough = true;
            made.q = clamp01(made.q + d.cfg.brQBonus);
            c.craft.lastBreakthrough = w.now();
            if (w.content().moodlets.count("insight")) applyRisks(w, c, {"moodlet:insight"});
            MemoryEntry m;
            m.type = "breakthrough";
            m.who = {c.id};
            m.valence = 0.6;
            m.salience = 0.6;
            w.remember(c, m);
        }
    }
    Recipe& rec = learnFromItem(d, c.craft.book, made, w.seed(), c.id);
    if (out.breakthrough && std::find(rec.tags.begin(), rec.tags.end(), "breakthrough") == rec.tags.end())
        rec.tags.push_back("breakthrough");
    out.recipe = rec.id;
}

} // namespace

craft::Actor craftActor(const World& w, const Character& c) {
    craft::Actor a;
    a.id = c.id;
    a.skills = c.skills;
    a.knowsMaterial = c.craft.knowsMaterial;
    a.traits = c.traits;
    a.mood = std::max(-1.0, std::min(1.0, c.mood(w.now())));
    return a;
}

craft::Workplace craftWorkplace(const World& w, const Character& c, double radiusM) {
    const Content& ct = w.content();
    const CraftData& d = ct.craft;
    craft::Workplace wp;
    for (const auto& [type, od] : ct.objects) {
        if (od.station.empty()) continue;
        for (EntityId id : w.objectsOfType(type)) {
            const WorldObject* o = w.object(id);
            if (o && dist(o->pos, c.pos) <= radiusM) {
                wp.stations.push_back(od.station);
                break;
            }
        }
    }
    for (const auto& [item, n] : c.inventory) {
        if (n <= 0) continue;
        if (d.station(item)) wp.stations.push_back(item); // portable: a loom, a pot
        auto tags = itemToolTags(d, item);
        // a crafted instance of this item carries its real numbers below
        bool instance = false;
        for (const auto& it : c.craft.items)
            if (formItem(d, it) == item) instance = true;
        if (!tags.empty() && !instance) {
            craft::Tool t;
            t.tags = tags;
            t.hardness = 0.6;
            t.q = 0.5;
            wp.tools.push_back(t);
        }
        // a hard stone in hand is a hammerstone; sand is grit
        auto def = ct.items.find(item);
        if (def != ct.items.end() && !def->second.material.empty()) {
            Item raw = rawItem(d, def->second.material, 0.5);
            recognize(d, raw);
            craft::Tool t = toolOf(d, raw);
            if (std::find(t.tags.begin(), t.tags.end(), "striking") != t.tags.end() ||
                std::find(t.tags.begin(), t.tags.end(), "grit") != t.tags.end())
                wp.tools.push_back(t);
        }
    }
    for (const auto& it : c.craft.items)
        if (!it.derivedTags.empty() || it.kind == "tool") wp.tools.push_back(toolOf(d, it));
    auto bare = bareTools();
    wp.tools.insert(wp.tools.end(), bare.begin(), bare.end());
    std::sort(wp.stations.begin(), wp.stations.end());
    wp.stations.erase(std::unique(wp.stations.begin(), wp.stations.end()), wp.stations.end());
    return wp;
}

namespace {

bool buildInput(World& w, Character& c, const CraftCommand& cmd, StepInput& in,
                std::vector<std::pair<std::string, int>>& inv, std::vector<int>& made, std::string& bad) {
    in.process = cmd.process;
    in.target = cmd.target;
    for (const auto& [slot, refs] : cmd.slots)
        for (const auto& r : refs) {
            Item it;
            if (!takeRef(w, c, r, it, inv, made)) {
                bad = r;
                return false;
            }
            in.slots[slot].push_back(it);
        }
    return true;
}

} // namespace

craft::Preview craftPreview(const World& w, const Character& c, const CraftCommand& cmd) {
    StepInput in;
    std::vector<std::pair<std::string, int>> inv;
    std::vector<int> made;
    std::string bad;
    craft::Preview pv;
    if (!buildInput(const_cast<World&>(w), const_cast<Character&>(c), cmd, in, inv, made, bad)) {
        pv.reason = "missing_input";
        pv.reasonArg = bad;
        return pv;
    }
    return previewStep(w.content().craft, in, craftActor(w, c), craftWorkplace(w, c));
}

CraftOutcome craftAttempt(World& w, Character& c, const CraftCommand& cmd) {
    CraftOutcome out;
    const CraftData& d = w.content().craft;
    StepInput in;
    std::vector<std::pair<std::string, int>> inv;
    std::vector<int> made;
    std::string bad;
    if (!buildInput(w, c, cmd, in, inv, made, bad)) {
        out.reason = "missing_input";
        out.reasonArg = bad;
        return out;
    }
    const Workplace wp = craftWorkplace(w, c);
    const int stepNo = static_cast<int>(c.craft.tried.size());
    StepResult r = runStep(d, in, craftActor(w, c), wp, w.seed(), static_cast<std::uint64_t>(w.now()), stepNo);
    out.step = r;
    if (r.outcome == Outcome::NotStarted) {
        out.reason = r.reason;
        out.reasonArg = r.reasonArg;
        return out;
    }
    out.started = true;
    out.time = r.time;
    consume(c, made, inv, d);
    learnFromStep(c, r);
    applyRisks(w, c, r.riskEffects);
    for (const auto& s : r.salvage) keep(c, s, d, nullptr);
    if (r.hasOut) {
        Item it = r.out;
        afterSuccess(w, c, it, out);
        out.made = keep(c, it, d, &out.item);
    }
    return out;
}

namespace {

std::vector<Item> poolOf(const World& w, const Character& c, std::vector<std::string>& origin) {
    const Content& ct = w.content();
    std::vector<Item> pool;
    for (size_t i = 0; i < c.craft.items.size(); ++i) {
        pool.push_back(c.craft.items[i]);
        origin.push_back("made:" + std::to_string(i));
    }
    for (const auto& [item, n] : c.inventory) {
        auto def = ct.items.find(item);
        if (def == ct.items.end() || def->second.material.empty() || n <= 0) continue;
        for (int k = 0; k < std::min(n, 12); ++k) {
            pool.push_back(rawItem(ct.craft, def->second.material, 0.5));
            origin.push_back("inv:" + item);
        }
    }
    return pool;
}

} // namespace

craft::ChainPlan fastCraftPlan(const World& w, const Character& c, const std::string& recipeId) {
    craft::ChainPlan plan;
    const Recipe* r = c.craft.book.find(recipeId);
    if (!r) {
        plan.ok = false;
        plan.missing.push_back("recipe:" + recipeId);
        return plan;
    }
    std::vector<std::string> origin;
    std::vector<Item> pool = poolOf(w, c, origin);
    ChainOpts o;
    o.maxStages = w.content().craft.cfg.fastCraftMaxStages;
    plan = planChain(w.content().craft, r->steps, pool, craftActor(w, c), craftWorkplace(w, c), o);
    if (!r->master(w.content().craft.cfg)) plan.ok = false; // fast craft needs a fully known record
    return plan;
}

craft::ChainPlan designPlan(const World& w, const Character& c, const std::string& designId) {
    craft::ChainPlan plan;
    const CraftData& d = w.content().craft;
    const craft::DesignDef* def = d.design(designId);
    if (!def) {
        plan.ok = false;
        plan.missing.push_back("design:" + designId);
        return plan;
    }
    std::vector<std::string> origin;
    std::vector<Item> pool = poolOf(w, c, origin);
    ChainOpts o;
    o.maxStages = d.cfg.fastCraftMaxStages;
    const Recipe r = recipeFromDesign(d, *def, 0.0, true);
    return planChain(d, r.steps, pool, craftActor(w, c), craftWorkplace(w, c), o);
}

CraftOutcome fastCraft(World& w, Character& c, const std::string& recipeId, int count) {
    CraftOutcome out;
    const CraftData& d = w.content().craft;
    const Recipe* rp = c.craft.book.find(recipeId);
    if (!rp) {
        out.reason = "unknown_step";
        return out;
    }
    if (!rp->master(d.cfg)) {
        out.reason = "not_master";
        return out;
    }
    const std::vector<RecipeStep> steps = rp->steps;
    for (int n = 0; n < std::max(1, count); ++n) {
        std::vector<std::string> origin;
        std::vector<Item> pool = poolOf(w, c, origin);
        std::vector<Item> before = pool;
        ChainOpts o;
        o.maxStages = d.cfg.fastCraftMaxStages;
        Actor a = craftActor(w, c);
        a.repeat = std::min(1.0, rp->mastery);
        ChainRun run = runChain(d, steps, pool, a, craftWorkplace(w, c), w.seed(),
                                static_cast<std::uint64_t>(w.now()) + static_cast<std::uint64_t>(out.time), o);
        out.chain = run;
        out.time += run.time;
        if (!run.results.empty()) out.started = true;
        for (const auto& r : run.results) {
            learnFromStep(c, r);
            applyRisks(w, c, r.riskEffects);
        }
        // what left the pool was used: match the survivors back to their origin
        std::vector<bool> survived(before.size(), false);
        std::vector<bool> claimed(pool.size(), false);
        for (size_t i = 0; i < before.size(); ++i)
            for (size_t j = 0; j < pool.size(); ++j)
                if (!claimed[j] && pool[j].trace.size() == before[i].trace.size() &&
                    pool[j].material == before[i].material && pool[j].shape == before[i].shape &&
                    pool[j].q == before[i].q && pool[j].form == before[i].form) {
                    claimed[j] = survived[i] = true;
                    break;
                }
        std::vector<int> made;
        std::vector<std::pair<std::string, int>> inv;
        for (size_t i = 0; i < before.size(); ++i) {
            if (survived[i]) continue;
            if (origin[i].rfind("made:", 0) == 0) made.push_back(std::atoi(origin[i].c_str() + 5));
            else inv.emplace_back(origin[i].substr(4), 1);
        }
        consume(c, made, inv, d);
        for (size_t j = 0; j < pool.size(); ++j)
            if (!claimed[j]) keep(c, pool[j], d, nullptr); // salvage and leftovers
        if (!run.done) {
            out.reason = run.stop;
            out.reasonArg = run.stopArg;
            return out;
        }
        Item it = run.out;
        afterSuccess(w, c, it, out);
        out.made = keep(c, it, d, &out.item);
    }
    out.reason = "done";
    return out;
}

std::string giveDesign(const World& w, Character& c, const std::string& designId, double mastery) {
    const CraftData& d = w.content().craft;
    const DesignDef* ds = d.design(designId);
    if (!ds) return "";
    Recipe r = recipeFromDesign(d, *ds, mastery, mastery >= d.cfg.masterThreshold);
    if (!c.craft.book.find(r.id)) c.craft.book.recipes.push_back(r);
    return r.id;
}

int inspectMade(World& w, Character& c, int index) {
    if (index < 0 || index >= static_cast<int>(c.craft.items.size())) return -1;
    const CraftData& d = w.content().craft;
    double skill = d.cfg.novice;
    const Item& it = c.craft.items[static_cast<size_t>(index)];
    if (!it.trace.empty())
        if (const ProcessDef* p = d.process(it.trace.back().process))
            if (c.skills.count(p->skill)) skill = c.skills.at(p->skill);
    return inspectItem(d, c.craft.items[static_cast<size_t>(index)], skill, w.seed(), static_cast<std::uint64_t>(w.now()), c.id);
}

bool teachRecipe(World& w, Character& teacher, Character& pupil, const std::string& recipeId) {
    const CraftData& d = w.content().craft;
    const Recipe* r = teacher.craft.book.find(recipeId);
    if (!r || r->steps.empty()) return false;
    const ProcessDef* p = d.process(r->steps.back().step.process);
    const std::string skill = p ? p->skill : "";
    double ts = teacher.skills.count(skill) ? teacher.skills.at(skill) : d.cfg.novice;
    double ps = pupil.skills.count(skill) ? pupil.skills.at(skill) : d.cfg.novice;
    Recipe copy = craft::teachRecipe(d, *r, ts, ps);
    if (Recipe* had = pupil.craft.book.find(copy.id)) {
        had->mastery = std::max(had->mastery, copy.mastery);
        for (size_t i = 0; i < had->steps.size() && i < copy.steps.size(); ++i)
            had->steps[i].known = had->steps[i].known || copy.steps[i].known;
    } else {
        pupil.craft.book.recipes.push_back(copy);
    }
    return true;
}

CraftOutcome repairMade(World& w, Character& c, int index, const std::string& mode) {
    CraftCommand cmd;
    cmd.process = "repair";
    cmd.slots["work"] = {"made:" + std::to_string(index)};
    cmd.target.mode = mode;
    return craftAttempt(w, c, cmd);
}

void craftAct(World& w, Character& c, const InteractionDef& d, WorldObject* target, Character* targetChar,
              const Json& params) {
    (void)target;
    const std::string& kind = d.craft;
    if (kind.rfind("design:", 0) == 0) {
        const std::string id = giveDesign(w, c, kind.substr(7), w.content().craft.cfg.masterThreshold);
        if (Recipe* r = c.craft.book.find(id)) {
            r->successes = std::max(r->successes, w.content().craft.cfg.masterSuccesses);
            for (auto& s : r->steps) s.known = true;
        }
        fastCraft(w, c, id, 1);
        return;
    }
    if (kind == "recipe") {
        fastCraft(w, c, params.value("recipe", ""), params.value("count", 1));
        return;
    }
    if (kind == "attempt") {
        CraftCommand cmd;
        cmd.process = params.value("process", "");
        if (params.contains("slots"))
            for (auto it = params["slots"].begin(); it != params["slots"].end(); ++it)
                cmd.slots[it.key()] = it.value().get<std::vector<std::string>>();
        cmd.target.shape = params.value("shape", "");
        cmd.target.len = params.value("len", 0);
        cmd.target.mode = params.value("mode", "");
        cmd.target.heat = params.value("heat", -1);
        craftAttempt(w, c, cmd);
        return;
    }
    if (kind == "inspect") {
        inspectMade(w, c, params.value("item", -1));
        return;
    }
    if (kind == "teach" && targetChar) {
        teachRecipe(w, c, *targetChar, params.value("recipe", ""));
        return;
    }
    if (kind == "repair") repairMade(w, c, params.value("item", -1), params.value("mode", ""));
}

std::vector<InteractionDef> designInteractions(const Content& ct) {
    std::vector<InteractionDef> out;
    const CraftData& d = ct.craft;
    if (!d.loaded) return out;
    // what the world offers: object types that serve a step (its station tags, hot enough), tool tags by item
    auto servingTypes = [&](const ProcessDef& p, int heat) {
        std::set<std::string> out;
        for (const auto& [type, od] : ct.objects) {
            const StationDef* s = d.station(od.station);
            if (!s) continue;
            bool tagOk = p.stations.empty();
            for (const auto& t : p.stations)
                if (std::find(s->tags.begin(), s->tags.end(), t) != s->tags.end()) tagOk = true;
            if (tagOk && (heat <= 0 || s->tempMax >= heat)) out.insert(type);
        }
        return out;
    };
    // AL-88: a station that does not heat (the anvil) works with the heat of another one at
    // hand (the forge): the step then needs both, each its own place
    auto taggedTypes = [&](const ProcessDef& p) {
        std::set<std::string> out;
        for (const auto& [type, od] : ct.objects) {
            const StationDef* s = d.station(od.station);
            if (!s) continue;
            for (const auto& t : p.stations)
                if (std::find(s->tags.begin(), s->tags.end(), t) != s->tags.end()) out.insert(type);
        }
        return out;
    };
    auto hotTypes = [&](int heat) {
        std::set<std::string> out;
        for (const auto& [type, od] : ct.objects)
            if (const StationDef* s = d.station(od.station); s && s->tempMax >= heat) out.insert(type);
        return out;
    };
    // AL-74: a homestead object (one per household: the hearth, the house's table) stands a
    // few metres from the others, so one act may use several of them (craftWorkplace radius)
    auto homestead = [&](const std::string& type) {
        auto it = ct.objects.find(type);
        return it != ct.objects.end() && it->second.perFamily > 0;
    };
    auto rank = [&](const std::string& type) { // lower = the better place for the act
        auto it = ct.objects.find(type);
        if (it == ct.objects.end()) return 3;
        return it->second.perFamily > 0 ? 0 : it->second.perSettlement > 0 ? 1 : 2;
    };
    auto pick = [&](const std::set<std::string>& types) {
        std::string best;
        for (const auto& t : types)
            if (best.empty() || rank(t) < rank(best)) best = t;
        return best;
    };
    std::map<std::string, std::string> toolItem; // tool tag -> item
    for (const auto& f : d.forms)
        if (!f.item.empty() && ct.items.count(f.item))
            for (const auto& t : f.deriveTags)
                if (!toolItem.count(t)) toolItem[t] = f.item;
    // AL-88: raw things that work as tools by themselves (craftWorkplace): sand is grit,
    // a hard stone in hand is a hammerstone
    for (const auto& [id, it] : ct.items) {
        const MaterialDef* m = it.material.empty() ? nullptr : d.material(it.material);
        if (!m) continue;
        const bool grit = std::find(m->tags.begin(), m->tags.end(), "grit") != m->tags.end();
        const bool stone = std::find(m->tags.begin(), m->tags.end(), "stone") != m->tags.end();
        auto h = m->p.find("hardness");
        if (grit && !toolItem.count("grit")) toolItem["grit"] = id;
        if (stone && h != m->p.end() && h->second >= 0.6 && !toolItem.count("striking")) toolItem["striking"] = id;
    }
    std::map<std::string, std::string> portable; // station tag -> carried item that is that station
    for (const auto& [id, it] : ct.items)
        if (const StationDef* s = d.station(id))
            for (const auto& t : s->tags) portable[t] = id;
    // AL-88: the object types each room of rooms.json brings together (its needs and stations)
    std::vector<std::set<std::string>> workshopRooms;
    for (const auto& r : ct.roomsJson.value("rooms", Json::array())) {
        std::set<std::string> types;
        for (const auto& n : r.value("needs", Json::array()))
            for (const auto& t : n.value("any", Json::array()))
                if (t.is_string()) types.insert(t.get<std::string>());
        if (r.contains("furnish"))
            for (const auto& st : r["furnish"].value("stations", Json::array()))
                if (st.contains("object") && st["object"].is_string()) types.insert(st["object"].get<std::string>());
        if (types.size() > 1) workshopRooms.push_back(std::move(types));
    }
    // things an authored chore already makes at a place out of one's own stuff (brew_ale at the
    // hearth): the room rule leaves them to it (a theft or a bare-handed make_axe does not count)
    std::set<std::string> authored;
    for (const auto& [id, a] : ct.interactions) {
        if (a.target.rfind("object:", 0) != 0) continue;
        bool uses = false;
        for (const auto& e : a.eff) uses = uses || (e.subject == "has" && e.op == '-' && !e.onTarget);
        if (!uses) continue;
        for (const auto& e : a.eff)
            if (e.subject == "has" && e.op == '+' && !e.onTarget) authored.insert(e.key);
    }
    for (const auto& ds : d.designs) {
        const FormDef* f = d.form(ds.form);
        if (!f || f->item.empty() || !ct.items.count(f->item)) continue;
        std::map<std::string, int> need;  // item -> count
        std::set<std::string> tools, carried;
        std::vector<std::set<std::string>> places; // per step that needs a place: the object types serving it
        int heatPlace = -1;                         // index in places of the first heated step
        bool ok = true;
        int time = 0;
        std::string skill;
        std::vector<std::string> mainTags; // the design's first material: picks the skill by skill_by
        for (const auto& st : ds.steps)
            for (const auto& [slot, ins] : st.slots)
                for (const auto& in : ins)
                    if (mainTags.empty() && in.ref.rfind("mat:", 0) == 0)
                        if (const MaterialDef* m = d.material(in.ref.substr(4))) {
                            mainTags = m->tags;
                            mainTags.push_back(m->id);
                        }
        for (const auto& st : ds.steps) {
            const ProcessDef* p = d.process(st.process);
            if (!p) {
                ok = false;
                break;
            }
            time += p->time;
            skill = p->skill;
            for (const auto& [tag, sk] : p->skillBy)
                if (std::find(mainTags.begin(), mainTags.end(), tag) != mainTags.end()) {
                    skill = sk;
                    break;
                }
            for (const auto& [slot, ins] : st.slots)
                for (const auto& in : ins) {
                    if (in.ref.rfind("step:", 0) == 0) continue;
                    if (in.ref.rfind("mat:", 0) != 0) {
                        ok = false;
                        continue;
                    }
                    const MaterialDef* m = d.material(in.ref.substr(4));
                    if (!m || m->item.empty() || !ct.items.count(m->item)) ok = false;
                    else need[m->item] += std::max(1, in.count);
                }
            if (!p->tools.empty()) {
                std::string item;
                for (const auto& t : p->tools)
                    if (toolItem.count(t)) item = toolItem[t];
                if (item.empty()) ok = false;
                else tools.insert(item);
            }
            if (!p->stations.empty() || p->heatMin > 0) {
                int heat = std::max(p->heatMin, p->op == ProcOp::Heat ? st.heat : 0);
                std::string port;
                for (const auto& t : p->stations)
                    if (portable.count(t) && port.empty()) port = portable[t];
                if (!port.empty()) carried.insert(port);
                else {
                    std::set<std::string> types = servingTypes(*p, heat);
                    if (types.empty() && heat > 0 && !p->stations.empty()) {
                        std::set<std::string> tagged = taggedTypes(*p), hot = hotTypes(heat);
                        if (!tagged.empty() && !hot.empty()) {
                            if (heatPlace < 0) heatPlace = static_cast<int>(places.size());
                            places.push_back(std::move(hot));
                            types = std::move(tagged);
                            heat = 0;
                        }
                    }
                    if (types.empty()) ok = false;
                    if (heat > 0 && heatPlace < 0) heatPlace = static_cast<int>(places.size());
                    places.push_back(std::move(types));
                }
            }
        }
        // the place of the act: one object type serving every step, else a homestead object
        // (of the heated step if any) with every other step served at the homestead too
        std::string objType;
        if (ok && !places.empty()) {
            std::set<std::string> all = places.front();
            for (size_t i = 1; i < places.size(); ++i) {
                std::set<std::string> keep;
                for (const auto& t : all)
                    if (places[i].count(t)) keep.insert(t);
                all.swap(keep);
            }
            objType = pick(all);
            if (objType.empty()) {
                bool home = true;
                for (const auto& s : places) home = home && std::any_of(s.begin(), s.end(), homestead);
                if (home) {
                    std::set<std::string> first;
                    for (const auto& t : places[static_cast<size_t>(std::max(0, heatPlace))])
                        if (homestead(t)) first.insert(t);
                    objType = pick(first);
                }
            }
            // AL-88: a workshop room (rooms.json) groups stations within reach of each other
            // (smithy: forge, anvil, quench tub, grindstone): the act stands at the room's object
            // serving the heated step (else the first), when the room serves every step
            if (objType.empty() && !authored.count(f->item))
                for (const auto& room : workshopRooms) {
                    bool all = true;
                    for (const auto& st : places) {
                        bool any = false;
                        for (const auto& t : st) any = any || room.count(t);
                        all = all && any;
                    }
                    if (!all) continue;
                    std::set<std::string> at;
                    for (const auto& t : places[static_cast<size_t>(std::max(0, heatPlace))])
                        if (room.count(t)) at.insert(t);
                    objType = pick(at);
                    if (!objType.empty()) break;
                }
            if (objType.empty()) ok = false;
        }
        if (!ok) continue;
        InteractionDef a;
        a.id = "craft:" + ds.id;
        a.category = "craft";
        a.target = objType.empty() ? "self" : "object:" + objType;
        a.targetType = objType;
        const auto od = ct.objects.find(objType);
        const bool owned = od != ct.objects.end() && (od->second.owned || od->second.perFamily > 0);
        if (owned) a.pre.push_back(parseCondition("target_owner:family")); // one's own hearth, not a neighbour's
        if (owned && heatPlace >= 0 && ct.items.count("firewood")) a.pre.push_back(parseCondition("home:firewood>=1"));
        for (const auto& [item, n] : need) a.pre.push_back(parseCondition("has:" + item + ">=" + std::to_string(n)));
        for (const auto& t : tools) a.pre.push_back(parseCondition("has:" + t + ">=1"));
        for (const auto& t : carried) a.pre.push_back(parseCondition("has:" + t + ">=1"));
        if (!skill.empty() && ct.knowledge.count(skill)) a.pre.push_back(parseCondition("knows:" + skill));
        a.eff.push_back(parseEffect("has:" + f->item + "+1"));
        for (const auto& [item, n] : need) a.eff.push_back(parseEffect("has:" + item + "-" + std::to_string(n)));
        a.duration = std::max(1, time);
        a.cost = 1.5 * static_cast<double>(ds.steps.size()); // the plain chores stay cheaper
        a.fsm = "Work";
        a.goap = true;
        a.craft = "design:" + ds.id;
        out.push_back(a);
    }
    return out;
}

namespace {

void note(Character& c, const CraftOutcome& o) {
    c.craft.lastOutcome = o.started ? (o.chain.results.empty() ? outcomeName(o.step.outcome) : (o.reason.empty() ? "done" : o.reason))
                                    : o.reason;
    c.craft.lastArg = o.started ? o.reason : o.reasonArg;
    c.craft.lastMade = o.made;
    c.craft.lastBreakthrough_ = o.breakthrough;
    c.craft.lastRecipe = o.recipe;
    ++c.craft.lastSeq;
}

std::string resultOf(const CraftOutcome& o) {
    if (!o.started) return "rejected:" + o.reason + (o.reasonArg.empty() ? "" : ":" + o.reasonArg);
    return "ok:" + (o.chain.results.empty() ? std::string(outcomeName(o.step.outcome)) : o.reason) +
           (o.breakthrough ? ":breakthrough" : "");
}

bool handleCraft(World& w, Command& cmd) {
    const std::string& t = cmd.type;
    if (t.rfind("craft_", 0) != 0) return false;
    Character* c = cmd.args.contains("id") ? w.character(cmd.args.at("id").get<EntityId>()) : nullptr;
    if (!c || !c->body.alive || c->isAnimal()) {
        cmd.result = "rejected:no_such_character";
        return true;
    }
    if (!w.content().craft.loaded) {
        cmd.result = "rejected:no_crafting";
        return true;
    }
    const Json& a = cmd.args;
    if (t == "craft_attempt") {
        CraftCommand cc;
        cc.process = a.value("process", "");
        if (a.contains("slots"))
            for (auto it = a["slots"].begin(); it != a["slots"].end(); ++it)
                cc.slots[it.key()] = it.value().get<std::vector<std::string>>();
        cc.target.shape = a.value("shape", "");
        cc.target.len = a.value("len", 0);
        cc.target.mode = a.value("mode", "");
        cc.target.heat = a.value("heat", -1);
        CraftOutcome o = craftAttempt(w, *c, cc);
        note(*c, o);
        cmd.result = resultOf(o);
    } else if (t == "craft_fast") {
        CraftOutcome o = fastCraft(w, *c, a.value("recipe", ""), a.value("count", 1));
        note(*c, o);
        cmd.result = resultOf(o);
    } else if (t == "craft_inspect") {
        int n = inspectMade(w, *c, a.value("item", -1));
        cmd.result = n < 0 ? "rejected:no_such_item" : "ok:" + std::to_string(n);
    } else if (t == "craft_repair") {
        CraftOutcome o = repairMade(w, *c, a.value("item", -1), a.value("mode", ""));
        note(*c, o);
        cmd.result = resultOf(o);
    } else if (t == "craft_rename") {
        Recipe* r = c->craft.book.find(a.value("recipe", ""));
        if (!r) cmd.result = "rejected:no_such_recipe";
        else {
            r->nameUser = a.value("name", "");
            cmd.result = "ok";
        }
    } else {
        cmd.result = "rejected:unknown_command";
    }
    return true;
}

} // namespace

void installCrafting(World& w) {
    w.addCommandHandler(handleCraft);
    installAlchemy(w); // AL-117: alchemy_drink, alchemy_use_rune
}

std::string craftItemName(const Loc& loc, const std::string& lang, const craft::CraftData& d, const craft::Item& it) {
    const Item* main = &it;
    for (const char* r : {"blade", "head", "stave", "body", "shaft", "handle"})
        if (const Item* p = it.part(r)) {
            main = p;
            break;
        }
    if (it.isAssembly() && main == &it) main = &it.parts.front().item.front();
    const std::string matKey = "material." + main->material + ".made";
    if (!it.form.empty()) {
        const FormDef* f = d.form(it.form);
        const Json noun = loc.noun(lang, "form." + it.form + ".name", it.form);
        if (f && f->named && loc.has(lang, matKey)) return loc.text(lang, matKey, Json{{"form", noun}});
        return loc.text(lang, "form." + it.form + ".name");
    }
    const Json shape = loc.noun(lang, "shape." + it.shape + ".name", it.shape);
    if (it.shape == "liquid" || it.shape == "lump" || it.shape == "powder" || it.shape == "scrap" || !loc.has(lang, matKey))
        return loc.text(lang, "craft.name.raw",
                        Json{{"material", loc.noun(lang, "material." + main->material + ".name", main->material)},
                             {"shape", shape}});
    return loc.text(lang, matKey, Json{{"form", shape}});
}

} // namespace altera::sim
