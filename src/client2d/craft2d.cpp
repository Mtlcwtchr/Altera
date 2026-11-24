// AL-97: the workshop of the 2D client (craft2d.hpp). A port of the 3D game's
// workshop snapshot (src/game_view/sim_game.cpp craftSnap / craftCandidates)
// to JSON, so the page can draw it in the "Резной рубеж" style.
#include "client2d/craft2d.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "sim/craft_sim.hpp"

using namespace altera::sim;

namespace altera::client2d {

namespace {

std::string tx(const ViewText& t, const std::string& key, const Json& args = Json::object()) {
    if (!t.loc || !t.loc->has(t.lang, key)) return key;
    return t.loc->text(t.lang, key, args);
}

double r3(double v) { return std::round(v * 1000.0) / 1000.0; }

std::string itemName(const ViewText& t, const std::string& id) {
    return t.name("item", id.substr(0, id.find('@')));
}

// "mat:<id>", "form:<id>", "shape:<id>", "station:<id>", "tag:<t>" as a name to show
std::string refName(const World& w, const ViewText& t, const std::string& ref) {
    const auto colon = ref.find(':');
    if (colon == std::string::npos) return ref;
    const std::string kind = ref.substr(0, colon), id = ref.substr(colon + 1);
    if (kind == "mat") {
        if (t.loc && t.loc->has(t.lang, "material." + id + ".name")) return tx(t, "material." + id + ".name");
        for (const auto& [item, def] : w.content().items)
            if (def.material == id) return itemName(t, item);
        return id;
    }
    if (kind == "form" || kind == "shape" || kind == "station") return tx(t, kind + "." + id + ".name");
    return id;
}

std::string recipeName(const ViewText& t, const craft::Recipe& r) {
    return !r.nameUser.empty() ? r.nameUser : !r.nameKey.empty() ? tx(t, r.nameKey) : tx(t, "hud.craft.unnamed");
}

bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

// The nearest object in the world that is station `st` (the helper walks there).
const WorldObject* nearestStation(const World& w, const std::string& st, const Vec2& from) {
    const WorldObject* best = nullptr;
    double bd = 1e18;
    for (const auto& [type, od] : w.content().objects) {
        if (od.station != st) continue;
        for (EntityId id : w.objectsOfType(type))
            if (const WorldObject* o = w.object(id)) {
                const double dd = dist(o->pos, from);
                if (dd < bd) bd = dd, best = o;
            }
    }
    return best;
}

Json stationsJson(const World& w, const ViewText& t, const Character& c, const craft::Workplace& wp, int& heatMax) {
    const auto& d = w.content().craft;
    Json out = Json::array();
    heatMax = 0;
    for (const auto& st : wp.stations) {
        const craft::StationDef* sd = d.station(st);
        if (!sd) continue;
        Json s = {{"id", st}, {"name", tx(t, "station." + st + ".name")}, {"temp_max", sd->tempMax}, {"placed", false}};
        // where it stands (C: its gauge hangs over it on the map); a carried one has no place
        double best = 1e18;
        for (const auto& [type, od] : w.content().objects) {
            if (od.station != st) continue;
            for (EntityId id : w.objectsOfType(type))
                if (const WorldObject* o = w.object(id)) {
                    const double dd = dist(o->pos, c.pos);
                    if (dd <= 12.0 && dd < best) {
                        best = dd;
                        s["placed"] = true;
                        s["x"] = r3(o->pos.x);
                        s["y"] = r3(o->pos.y);
                        s["object"] = type;
                    }
                }
        }
        heatMax = std::max(heatMax, sd->tempMax);
        out.push_back(std::move(s));
    }
    return out;
}

Json materialsJson(const World& w, const ViewText& t, const Character& c) {
    const auto& d = w.content().craft;
    Json out = Json::array();
    for (const auto& [item, n] : c.inventory) {
        auto def = w.content().items.find(item);
        if (n <= 0 || def == w.content().items.end() || def->second.material.empty()) continue;
        const craft::MaterialDef* m = d.material(def->second.material);
        const auto kn = c.craft.knowsMaterial.find(def->second.material);
        Json tags = Json::array();
        if (m)
            for (const auto& tg : m->tags) tags.push_back(tg);
        out.push_back({{"ref", "inv:" + item},
                       {"item", item},
                       {"name", itemName(t, item)},
                       {"mat", def->second.material},
                       {"shape", m ? m->shape : std::string("lump")},
                       {"tags", tags},
                       {"n", n},
                       {"know", r3(kn == c.craft.knowsMaterial.end() ? 0.0 : kn->second)}});
    }
    return out;
}

Json madeJson(const World& w, const ViewText& t, const Character& c) {
    const auto& d = w.content().craft;
    Json out = Json::array();
    for (size_t i = 0; i < c.craft.items.size(); ++i) {
        const craft::Item& it = c.craft.items[i];
        Json flaws = Json::array();
        for (const auto& f : it.flaws)
            if (f.revealed) flaws.push_back(tx(t, "flaw." + f.id + ".name"));
        std::string formItem;
        if (const craft::FormDef* f = d.form(it.form)) formItem = f->item;
        out.push_back({{"ref", "made:" + std::to_string(i)},
                       {"i", i},
                       {"name", t.loc ? craftItemName(*t.loc, t.lang, d, it) : it.form},
                       {"shape", it.shape},
                       {"form", it.form},
                       {"item", formItem},
                       {"mat", it.material},
                       {"kind", it.kind},
                       {"q", r3(it.q)},
                       {"tier", tx(t, "craft.tier." + craft::qualityTier(d, it.q))},
                       {"wear", r3(it.wear)},
                       {"flaws", flaws},
                       {"hidden_flaws", int(std::count_if(it.flaws.begin(), it.flaws.end(),
                                                          [](const craft::ItemFlaw& f) { return !f.revealed; })) > 0},
                       {"steps", it.trace.size()}});
    }
    return out;
}

Json processesJson(const World& w, const ViewText& t, const craft::Workplace& wp, int heatMax) {
    const auto& d = w.content().craft;
    Json out = Json::array();
    for (const auto& pid : d.processOrder) {
        const craft::ProcessDef& p = d.processes.at(pid);
        Json ps = {{"id", pid}, {"name", tx(t, "process." + pid + ".name")}, {"heat_min", p.heatMin}, {"time", p.time},
                   {"skill", p.skill}, {"available", true}, {"need", ""}};
        ps["stations"] = p.stations;
        ps["tools"] = p.tools;
        if (!p.stations.empty()) {
            bool any = false;
            std::string need;
            for (const auto& s : p.stations) {
                if (has(wp.stations, s)) any = true;
                need += (need.empty() ? "" : " / ") + tx(t, "station." + s + ".name");
            }
            if (!any) {
                ps["available"] = false;
                ps["need"] = need;
            }
        }
        if (ps["available"].get<bool>() && p.heatMin > 0 && heatMax < p.heatMin) {
            ps["available"] = false;
            ps["need"] = tx(t, "hud.craft.need_heat", {{"n", p.heatMin}});
        }
        Json slots = Json::array();
        for (const auto& sl : p.slots) slots.push_back({{"id", sl.name}, {"name", tx(t, "hud.craft.slot." + sl.name)}});
        ps["slots"] = slots;
        Json modes = Json::array();
        for (const auto& m : p.modes) modes.push_back({{"id", m}, {"name", tx(t, "process.mode." + m + ".name")}});
        ps["modes"] = modes;
        Json targets = Json::object();
        for (const auto& [from, tos] : p.shapeMap) {
            Json list = Json::array();
            for (const auto& to : tos) list.push_back({{"id", to}, {"name", tx(t, "shape." + to + ".name")}});
            targets[from] = list;
        }
        ps["targets"] = targets;
        out.push_back(std::move(ps));
    }
    return out;
}

Json recipesJson(const World& w, const ViewText& t, const Character& c) {
    const auto& d = w.content().craft;
    Json out = Json::array();
    for (const auto& r : c.craft.book.recipes) {
        Json steps = Json::array();
        for (const auto& st : r.steps)
            steps.push_back({{"process", st.step.process}, {"name", tx(t, "process." + st.step.process + ".name")}, {"known", st.known}});
        const craft::ChainPlan plan = fastCraftPlan(w, c, r.id);
        Json missing = Json::array();
        for (const auto& m : plan.missing) missing.push_back(refName(w, t, m));
        for (const auto& s : plan.needStations) missing.push_back(tx(t, "station." + s + ".name"));
        std::string formItem;
        if (const craft::FormDef* f = d.form(r.form)) formItem = f->item;
        out.push_back({{"id", r.id},
                       {"name", recipeName(t, r)},
                       {"form", r.form},
                       {"item", formItem},
                       {"mastery", r3(r.mastery)},
                       {"best_q", r3(r.bestQ)},
                       {"known", r.knownCount()},
                       {"total", r.steps.size()},
                       {"master", r.master(d.cfg)},
                       {"successes", r.successes},
                       {"steps", steps},
                       {"stages", plan.stages},
                       {"time", plan.time},
                       {"expected_q", r3(plan.expectedQ)},
                       {"can", plan.ok && plan.missing.empty() && plan.needStations.empty()},
                       {"missing", missing}});
    }
    return out;
}

Json forecastJson(const World& w, const ViewText& t, const Character& c, const Json& draft) {
    const auto& d = w.content().craft;
    CraftCommand cmd;
    cmd.process = draft.value("process", "");
    if (draft.contains("slots") && draft["slots"].is_object())
        for (auto it = draft["slots"].begin(); it != draft["slots"].end(); ++it)
            if (it.value().is_array()) cmd.slots[it.key()] = it.value().get<std::vector<std::string>>();
    cmd.target.shape = draft.value("shape", "");
    cmd.target.mode = draft.value("mode", "");
    cmd.target.heat = draft.value("heat", -1);
    cmd.target.len = draft.value("len", 0);
    const craft::Preview pv = craftPreview(w, c, cmd);
    Json f = {{"can", pv.canStart},       {"q", r3(pv.expectedQ)},   {"spread", r3(pv.qSpread)},
              {"ruin", r3(pv.pRuin)},     {"flaw", r3(pv.pFlaw)},    {"good", r3(pv.pGood)},
              {"certainty", r3(pv.certainty)}, {"time", pv.time},
              {"likely", pv.likelyForm.empty() ? std::string() : tx(t, "form." + pv.likelyForm + ".name")},
              {"likely_form", pv.likelyForm},
              {"tier", tx(t, "craft.tier." + craft::qualityTier(d, pv.expectedQ))}};
    if (pv.canStart) {
        const std::string likely = pv.likelyForm.empty() ? tx(t, "craft.unknown")
                                                         : tx(t, "craft.likely", {{"form", tx(t, "form." + pv.likelyForm + ".name")}});
        f["text"] = tx(t, pv.certainty >= 0.5 ? "hud.craft.forecast" : "hud.craft.forecast_vague",
                       {{"tier", f["tier"]}, {"ruin", int(std::lround(pv.pRuin * 100))},
                        {"flaw", int(std::lround(pv.pFlaw * 100))}, {"time", pv.time}, {"likely", likely}});
    } else {
        Json args = {{"slot", tx(t, "hud.craft.slot." + pv.reasonArg)}, {"tool", pv.reasonArg}, {"station", pv.reasonArg},
                     {"need", pv.reasonArg}, {"level", pv.reasonArg}, {"mode", pv.reasonArg},
                     {"shape", tx(t, "shape." + pv.reasonArg + ".name")}};
        if (d.station(pv.reasonArg)) args["station"] = tx(t, "station." + pv.reasonArg + ".name");
        f["reason"] = pv.reason;
        f["why"] = pv.reason.empty() ? std::string() : tx(t, "craft.reason." + pv.reason, args);
    }
    return f;
}

Json lastJson(const World& w, const ViewText& t, const Character& c) {
    const auto& d = w.content().craft;
    if (c.craft.lastOutcome.empty()) return nullptr;
    const auto& L = t.lang;
    std::string what;
    if (c.craft.lastMade >= 0 && c.craft.lastMade < int(c.craft.items.size()) && t.loc)
        what = craftItemName(*t.loc, L, d, c.craft.items[size_t(c.craft.lastMade)]);
    const std::string& code = c.craft.lastOutcome;
    const bool isOutcome = t.loc && t.loc->has(L, "craft.outcome." + code);
    const std::string head = isOutcome ? tx(t, "craft.outcome." + code)
                             : t.loc && t.loc->has(L, "craft.stop." + code)
                                 ? tx(t, "craft.stop." + code,
                                      {{"what", c.craft.lastArg}, {"why", c.craft.lastArg}, {"stages", c.craft.lastArg}})
                                 : tx(t, "craft.stop.other", {{"why", code}});
    Json j = {{"seq", c.craft.lastSeq},  {"code", code},   {"head", head},
              {"what", what},            {"made", c.craft.lastMade},
              {"started", c.craft.lastMade >= 0 || isOutcome},
              {"breakthrough", c.craft.lastBreakthrough_}, {"recipe", c.craft.lastRecipe}};
    if (const craft::Recipe* r = c.craft.book.find(c.craft.lastRecipe)) j["recipe_name"] = recipeName(t, *r);
    if (c.craft.lastMade >= 0 && c.craft.lastMade < int(c.craft.items.size())) {
        const craft::Item& it = c.craft.items[size_t(c.craft.lastMade)];
        j["q"] = r3(it.q);
        j["tier"] = tx(t, "craft.tier." + craft::qualityTier(d, it.q));
        Json links = Json::array();
        double weakQ = 2.0;
        std::string weak;
        for (const auto& s : it.trace) {
            const std::string n = tx(t, "process." + s.process + ".name");
            links.push_back({{"process", s.process}, {"name", n}, {"q", r3(s.q)}});
            if (s.q < weakQ) weakQ = s.q, weak = n;
        }
        j["links"] = links;
        if (links.size() > 1) j["weak"] = tx(t, "hud.craft.weak", {{"name", weak}});
        Json flaws = Json::array();
        for (const auto& f : it.flaws)
            if (f.revealed) flaws.push_back(tx(t, "flaw." + f.id + ".name"));
        j["flaws"] = flaws;
    }
    return j;
}

// The novice helper (D; board 10b, gdd crafting §17): the designs whose form
// answers the wish, how well they fit what the person has, the stations along
// their chain and the first step that can be done now, as a draft.
Json candidatesJson(const World& w, const ViewText& t, const Json& ui, const Character& c, const craft::Workplace& wp,
                    const std::string& wishId) {
    const auto& d = w.content().craft;
    Json wish;
    for (const auto& x : ui.value("wishes", Json::array()))
        if (x.value("id", "") == wishId) wish = x;
    if (wish.is_null()) return Json::array();
    const auto needs = wish.value("needs", std::vector<std::string>{});
    const auto cats = wish.value("categories", std::vector<std::string>{});
    auto invOf = [&](const std::string& mat) -> std::string {
        for (const auto& [item, n] : c.inventory) {
            if (n <= 0) continue;
            auto def = w.content().items.find(item);
            if (def != w.content().items.end() && def->second.material == mat) return item;
        }
        return "";
    };
    struct Cand {
        Json j;
        double fit;
    };
    std::vector<Cand> all;
    for (const auto& ds : d.designs) {
        const craft::FormDef* f = d.form(ds.form);
        if (!f) continue;
        bool match = has(cats, f->category);
        for (const auto& n : f->needs) {
            const std::string k = n.key.substr(0, n.key.find('.'));
            if (has(needs, n.key) || has(needs, k)) match = true;
        }
        if (!match) continue;
        const craft::ChainPlan plan = designPlan(w, c, ds.id);
        double fit = std::clamp(plan.expectedQ, 0.0, 1.0);
        if (!plan.needStations.empty()) fit *= 0.6;
        fit *= std::pow(0.75, double(std::min<size_t>(plan.missing.size(), 4)));
        Json missing = Json::array();
        for (const auto& m : plan.missing) missing.push_back(refName(w, t, m));
        for (const auto& s : plan.needStations) missing.push_back(tx(t, "station." + s + ".name"));
        Json cd = {{"id", ds.id},       {"form", ds.form},       {"name", tx(t, "form." + ds.form + ".name")},
                   {"item", f->item},   {"shape", f->shape.empty() ? std::string("lump") : f->shape.front()},
                   {"fit", r3(fit)},    {"missing", missing},    {"next", -1}};
        bool finished = false;
        for (const auto& it : c.craft.items)
            if (it.form == ds.form) finished = true;
        cd["finished"] = finished;
        Json steps = Json::array();
        int next = -1;
        for (size_t i = 0; i < ds.steps.size(); ++i) {
            const craft::DesignStep& st = ds.steps[i];
            std::string at;
            if (const craft::ProcessDef* p = d.process(st.process)) {
                for (const auto& s : p->stations)
                    if (has(wp.stations, s)) {
                        at = s;
                        break;
                    }
                if (at.empty() && !p->stations.empty()) at = p->stations.front();
            }
            Json sj = {{"process", st.process}, {"name", tx(t, "process." + st.process + ".name")}, {"station", at},
                       {"station_name", at.empty() ? std::string() : tx(t, "station." + at + ".name")},
                       {"here", at.empty() || has(wp.stations, at)}};
            if (!sj["here"].get<bool>())
                if (const WorldObject* o = nearestStation(w, at, c.pos)) sj["x"] = r3(o->pos.x), sj["y"] = r3(o->pos.y);
            steps.push_back(std::move(sj));
            if (finished || next >= 0) continue;
            bool made = false;   // already made? (a stored piece of this step's shape)
            if (!st.shape.empty())
                for (const auto& it : c.craft.items)
                    if (it.shape == st.shape) made = true;
            if (made) continue;
            Json slots = Json::object();
            bool ok = true;
            std::vector<int> taken;
            for (const auto& [slot, ins] : st.slots) {
                Json refs = Json::array();
                for (const auto& in : ins) {
                    const auto colon = in.ref.find(':');
                    const std::string kind = in.ref.substr(0, colon), id = in.ref.substr(colon + 1);
                    if (kind == "mat") {
                        const std::string item = invOf(id);
                        if (item.empty()) ok = false;
                        for (int k = 0; k < in.count && ok; ++k) refs.push_back("inv:" + item);
                    } else {
                        std::string wantShape, wantForm;   // a made piece: step:<n> (1-based), form:<id>, shape:<id>
                        if (kind == "step") {
                            const int n = std::atoi(id.c_str()) - 1;
                            if (n >= 0 && n < int(ds.steps.size())) wantShape = ds.steps[size_t(n)].shape;
                        } else if (kind == "form") wantForm = id;
                        else if (kind == "shape") wantShape = id;
                        int found = -1;
                        for (size_t m = 0; m < c.craft.items.size() && found < 0; ++m) {
                            if (std::find(taken.begin(), taken.end(), int(m)) != taken.end()) continue;
                            const craft::Item& it = c.craft.items[m];
                            if ((!wantShape.empty() && it.shape == wantShape) || (!wantForm.empty() && it.form == wantForm))
                                found = int(m);
                        }
                        if (found < 0) ok = false;
                        else {
                            taken.push_back(found);
                            refs.push_back("made:" + std::to_string(found));
                        }
                    }
                }
                slots[slot] = refs;
            }
            if (!ok) continue;
            next = int(i);
            Json draft = {{"process", st.process}, {"slots", slots}};
            if (!st.shape.empty()) draft["shape"] = st.shape;
            if (!st.mode.empty()) draft["mode"] = st.mode;
            if (st.len > 0) draft["len"] = st.len;
            if (st.heat >= 0) draft["heat"] = st.heat;
            cd["draft"] = draft;
        }
        cd["next"] = next;
        cd["steps"] = steps;
        all.push_back({std::move(cd), fit});
    }
    std::stable_sort(all.begin(), all.end(), [](const Cand& a, const Cand& b) { return a.fit > b.fit; });
    const size_t keep = size_t(std::max(1, ui.value("helper_max", 3)));
    Json out = Json::array();
    for (size_t i = 0; i < all.size() && i < keep; ++i) out.push_back(std::move(all[i].j));
    return out;
}

} // namespace

Json workshopJson(const World& w, const ViewText& t, const Json& ui, EntityId who, const WorkshopQuery& q) {
    const Character* c = w.character(who);
    if (!c || !c->body.alive || c->isAnimal() || !w.content().craft.loaded) return nullptr;
    const craft::Workplace wp = craftWorkplace(w, *c);
    int heatMax = 0;
    Json j;
    j["who"] = who;
    j["name"] = c->name;
    j["stations"] = stationsJson(w, t, *c, wp, heatMax);
    j["heat_max"] = heatMax;
    j["materials"] = materialsJson(w, t, *c);
    j["made"] = madeJson(w, t, *c);
    j["processes"] = processesJson(w, t, wp, heatMax);
    j["recipes"] = recipesJson(w, t, *c);
    if (!q.draft.value("process", std::string()).empty()) j["forecast"] = forecastJson(w, t, *c, q.draft);
    j["last"] = lastJson(w, t, *c);
    Json wishes = Json::array();
    for (const auto& x : ui.value("wishes", Json::array())) {
        const std::string id = x.value("id", "");
        wishes.push_back({{"id", id}, {"name", tx(t, "hud.craft.wish." + id)}});
    }
    j["wishes"] = wishes;
    j["wish"] = q.wish;
    j["candidates"] = q.wish.empty() ? Json::array() : candidatesJson(w, t, ui, *c, wp, q.wish);
    return j;
}

Json workshopTexts(const ViewText& t) {
    Json out = Json::object();
    if (!t.loc) return out;
    for (const auto& k : t.loc->keys(t.lang))
        if (k.rfind("hud.craft.", 0) == 0 || k.rfind("craft.tier.", 0) == 0 || k.rfind("craft.outcome.", 0) == 0)
            out[k] = t.loc->text(t.lang, k);
    return out;
}

void craftKit(World& w, Character& c) {
    // the 3D game's tour kit (sim_game.cpp craftKit): gathered materials, a knife,
    // a hammer, a needle and a sword made by a master, starter recipes of a few crafts
    for (const char* it : {"fiber_raw", "log", "stone", "clay", "hide", "meat", "bast_fiber", "branch", "grain"})
        c.inventory[it] += 6;
    const auto& d = w.content().craft;
    if (!d.loaded) return;
    craft::Actor master;
    for (const auto& [pid, p] : d.processes) master.skills[p.skill] = 0.95;
    craft::Workplace wp;
    for (const auto& [id, st] : d.stations) wp.stations.push_back(id);
    craft::Tool tool;
    tool.tags = {"cutting", "saw", "striking", "tongs", "whetstone", "grit", "needle", "bellows"};
    tool.hardness = 0.9;
    tool.q = 0.8;
    wp.tools.push_back(tool);
    for (const char* ds : {"knife", "hammer_tool", "bone_needle", "sword"})
        if (const craft::DesignDef* def = d.design(ds)) {
            craft::Recipe r = craft::recipeFromDesign(d, *def, 1.0, true);
            std::vector<craft::Item> pool;
            craft::ChainOpts o;
            o.rawFromNothing = true;
            o.stopOnRisk = false;
            craft::ChainRun run = craft::runChain(d, r.steps, pool, master, wp, 7, 0, o);
            if (run.done) c.craft.items.push_back(run.out);
        }
    for (const char* ds : {"cord", "thread", "roast_meat", "clay_pot", "plank", "knife"}) giveDesign(w, c, ds);
    for (const char* it : {"honey", "milk", "eggs", "apples", "berries", "vegetables", "fish", "herbs", "cheese", "bone"})
        c.inventory[it] += 4;
    c.inventory["water"] += 12;
    for (const char* s : {"toolmaking", "pottery", "spinning", "cooking"}) c.skills[s] = std::max(c.skills[s], 0.5);
    c.skills["carpentry"] = std::max(c.skills["carpentry"], 0.4);
}

} // namespace altera::client2d
