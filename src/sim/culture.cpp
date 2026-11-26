// LR-155: cultures and faiths of people and settlements. See culture.hpp.
#include "sim/culture.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>

#include "sim/content.hpp"
#include "sim/data_fs.hpp"
#include "sim/magic_society.hpp"
#include "sim/species.hpp"
#include "sim/world.hpp"

namespace lr::sim {

namespace fs = std::filesystem;

namespace {

std::map<std::string, double> dmap(const Json& j, const char* k) {
    std::map<std::string, double> out;
    if (!j.is_object() || !j.contains(k) || !j[k].is_object()) return out;
    for (auto it = j[k].begin(); it != j[k].end(); ++it)
        if (it.value().is_number()) out[it.key()] = it.value().get<double>();
    return out;
}

std::vector<std::string> strs(const Json& j, const char* k) {
    std::vector<std::string> out;
    if (!j.is_object() || !j.contains(k) || !j[k].is_array()) return out;
    for (const auto& s : j[k])
        if (s.is_string()) out.push_back(s.get<std::string>());
    return out;
}

double num(const Json& j, const char* k, double d) { return j.is_object() && j.contains(k) && j[k].is_number() ? j[k].get<double>() : d; }

std::unique_ptr<CultureRules> loadRules(const fs::path& dir) {
    auto r = std::make_unique<CultureRules>();
    const fs::path rp = dir / "rules.json";
    if (!data::exists(rp)) return r;
    try {
        const Json j = data::readJson(rp.string());
        const Json p = j.value("person", Json::object());
        auto& P = r->person;
        P.inherit = p.value("inherit", P.inherit);
        P.bondStart = num(p, "bond_start", P.bondStart);
        P.driftAdult = num(p, "drift_adult", P.driftAdult);
        P.driftChild = num(p, "drift_child", P.driftChild);
        P.driftFloor = num(p, "drift_floor_mult", P.driftFloor);
        P.grow = num(p, "grow", P.grow);
        P.switchBelow = num(p, "switch", P.switchBelow);
        P.afterSwitch = num(p, "after_switch", P.afterSwitch);
        const Json t = j.value("tradition", Json::object());
        auto& T = r->tradition;
        T.start = num(t, "start", T.start);
        T.gain = num(t, "gain", T.gain);
        T.normShare = std::max(0.01, num(t, "norm_share", T.normShare));
        T.fade = num(t, "fade", T.fade);
        T.fixed = num(t, "fixed", T.fixed);
        T.forming = num(t, "forming", T.forming);
        T.lost = num(t, "lost", T.lost);
        T.seed = num(t, "seed", T.seed);
        T.weightTtlDays = static_cast<int>(num(t, "weight_ttl_days", T.weightTtlDays));
        const Json f = j.value("faith", Json::object());
        auto& F = r->faith;
        F.basePiety = num(f, "base_piety", F.basePiety);
        F.relax = num(f, "relax", F.relax);
        F.convertBelow = num(f, "convert_below", F.convertBelow);
        F.convertBase = num(f, "convert_base", F.convertBase);
        F.keep = num(f, "keep", F.keep);
        F.pull = dmap(f, "pull");
        F.friendAffinity = num(f, "friend_affinity", F.friendAffinity);
        F.pietyAfterConvert = num(f, "piety_after_convert", F.pietyAfterConvert);
        F.weightsScale = num(f, "weights_scale", F.weightsScale);
        F.holyDayMoodlet = f.value("holy_day_moodlet", F.holyDayMoodlet);
        F.traitBase = dmap(f, "trait_base");
        if (data::exists(dir / "values.json")) r->values = strs(data::readJson((dir / "values.json").string()), "values");
        if (data::exists(dir / "traditions.json"))
            for (const Json& tj : data::readJson((dir / "traditions.json").string()).value("traditions", Json::array())) {
                TraditionDef d;
                d.id = tj.value("id", std::string());
                if (d.id.empty()) continue;
                d.kind = tj.value("kind", std::string("custom"));
                d.practice = strs(tj, "practice");
                d.goals = dmap(tj, "goals");
                d.values = dmap(tj, "values");
                d.moodlet = tj.value("moodlet", std::string());
                if (!r->traditions.count(d.id)) r->traditionOrder.push_back(d.id);
                r->traditions[d.id] = d;
            }
        for (const fs::path& fp : data::listPaths(dir / "religions")) {
            const Json fj = data::readJson(fp.string());
            FaithDef d;
            d.id = fj.value("id", fp.stem().string());
            d.kind = fj.value("kind", std::string("pantheon"));
            d.heresy = fj.value("heresy", false);
            for (const Json& g : fj.value("deities", Json::array()))
                if (g.is_object()) d.deities.push_back({g.value("id", std::string()), g.value("domain", std::string())});
            d.values = dmap(fj, "values");
            d.rites = dmap(fj, "rites");
            d.sins = dmap(fj, "sins");
            d.goals = dmap(fj, "goals");
            d.drawnBy = dmap(fj, "drawn_by");
            for (const Json& h : fj.value("holy_days", Json::array()))
                if (h.is_object()) d.holyDays.push_back({h.value("id", std::string()), h.value("day", 0)});
            d.burial = fj.value("burial", std::string());
            d.clergy = fj.value("clergy", std::string());
            d.sacred = strs(fj, "sacred");
            const Json m = fj.value("magic", Json::object());
            d.blessed = strs(m, "blessed");
            d.forbidden = strs(m, "forbidden");
            d.zeal = num(fj, "zeal", d.zeal);
            d.tolerance = num(fj, "tolerance", d.tolerance);
            r->faiths[d.id] = d;
        }
        if (data::exists(dir / "inspect.json")) r->inspect = data::readJson((dir / "inspect.json").string());
        r->loaded = true;
    } catch (const std::exception& e) {
        r->errors.push_back(std::string("culture: ") + e.what());
    }
    return r;
}

bool person(const Character& c) { return !c.isAnimal(); }

EntityId settlementOfPerson(const World& w, const Character& c) {
    if (c.status.settlement != kNone) return c.status.settlement;
    if (const Family* f = w.familyOf(c)) return f->settlement;
    return c.standing.at;
}

std::string raceCulture(const World& w, const Character& c) {
    const RaceDef& rd = raceOf(w.content(), c);
    if (!rd.cultureDefault.empty()) return rd.cultureDefault;
    return w.content().peoples.mapCulture;
}

const Character* parentFor(const World& w, const Character& c, const std::string& rule) {
    const Character* m = w.character(c.status.mother);
    const Character* f = w.character(c.status.father);
    if (rule == "father") return f ? f : m;
    return m ? m : f;
}

// what the daily pass gives a person with nothing stored (depth guards a loop of parents)
std::string deriveCulture(const World& w, const Character& c, int depth) {
    if (!c.culture.empty()) return c.culture;
    const CultureRules& r = cultureRules(w.content());
    if (depth < 4 && r.person.inherit != "settlement")
        if (const Character* p = parentFor(w, c, r.person.inherit)) {
            const std::string pc = deriveCulture(w, *p, depth + 1);
            if (!pc.empty()) return pc;
        }
    const EntityId sid = settlementOfPerson(w, c);
    if (sid != kNone) {
        const std::string sc = settlementCulture(w, sid);
        // a newcomer of another kind keeps the kind's ways unless the place's culture is of his kind
        const std::string rc = raceCulture(w, c);
        if (!sc.empty()) {
            const CultureDef* cd = nullptr;
            if (auto it = w.content().peoples.cultures.find(sc); it != w.content().peoples.cultures.end()) cd = &it->second;
            const RaceDef& rd = raceOf(w.content(), c);
            // a culture's peoples (or its species) are its own: they grew up in it
            bool own = !cd || cd->species.empty() || cd->species == rd.id || rc.empty();
            if (cd && !own)
                for (const auto& [kind, mix] : cd->peoples)
                    if (mix.count(rd.id)) own = true;
            if (own) return sc;
        }
        if (!rc.empty()) return rc;
        return sc;
    }
    return raceCulture(w, c);
}

std::string deriveFaith(const World& w, const Character& c, int depth) {
    if (!c.faith.empty()) return c.faith;
    const CultureRules& r = cultureRules(w.content());
    if (depth < 4 && r.person.inherit != "settlement")
        if (const Character* p = parentFor(w, c, r.person.inherit)) {
            const std::string pf = deriveFaith(w, *p, depth + 1);
            if (!pf.empty()) return pf;
        }
    return cultureFaith(w.content(), deriveCulture(w, c, 0));
}

double basePiety(const CultureRules& r, const Character& c) {
    double b = r.faith.basePiety;
    for (const auto& [t, v] : r.faith.traitBase)
        if (c.hasTrait(t)) b = v;
    return b;
}

// the memories a person made since `from` (the last day)
template <class F>
void recent(const Character& c, Tick from, F&& f) {
    for (const MemoryEntry& m : c.memory)
        if (m.tick >= from && m.source == "seen") f(m);
}

Json& stateOf(World& w) {
    Json& s = w.moduleState("culture");
    if (!s.is_object()) s = Json::object();
    if (!s.contains("trad") || !s["trad"].is_object()) s["trad"] = Json::object();
    return s;
}

void setMoodlet(World& w, Character& c, const std::string& id) {
    if (id.empty()) return;
    auto md = w.content().moodlets.find(id);
    if (md == w.content().moodlets.end()) return;
    const Tick until = w.now() + md->second.ttl;
    auto it = std::find_if(c.moodlets.begin(), c.moodlets.end(), [&](const Moodlet& m) { return m.id == id; });
    if (it != c.moodlets.end()) it->until = until;
    else c.moodlets.push_back({id, md->second.value, until});
}

void convert(World& w, Character& c, const std::string& to, const std::string& why) {
    const CultureRules& r = cultureRules(w.content());
    const std::string from = c.faith;
    c.faith = to;
    c.piety = r.faith.pietyAfterConvert;
    MemoryEntry m;
    m.type = "converted";
    m.valence = 0.3;
    m.salience = 0.6;
    w.remember(c, m);
    w.chronicle("culture", {c.id}, "chronicle.culture.converted", {{"who", c.id}, {"faith", to}, {"from_faith", from}, {"how", why}});
}

} // namespace

const TraditionDef* CultureRules::trad(const std::string& id) const {
    auto it = traditions.find(id);
    return it == traditions.end() ? nullptr : &it->second;
}
const FaithDef* CultureRules::faithDef(const std::string& id) const {
    auto it = faiths.find(id);
    return it == faiths.end() ? nullptr : &it->second;
}

const CultureRules& cultureRules(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<CultureRules>> cache;
    static const CultureRules kNone;
    if (c.dir.empty()) return kNone;
    std::lock_guard<std::mutex> lock(mu);
    const std::string key = data::cacheKey(c.dir);
    auto it = cache.find(key);
    if (it == cache.end()) it = cache.emplace(key, loadRules(fs::path(c.dir).parent_path() / "culture")).first;
    return *it->second;
}

const Json* cultureData(const Content& ct, const std::string& culture) {
    auto it = ct.peoples.cultures.find(culture);
    if (it == ct.peoples.cultures.end() || !it->second.data.is_object()) return nullptr;
    return &it->second.data;
}

std::string cultureFaith(const Content& ct, const std::string& culture) {
    const Json* d = cultureData(ct, culture);
    return d ? d->value("faith", std::string()) : std::string();
}

std::string cultureOf(const World& w, const Character& c) { return person(c) ? deriveCulture(w, c, 0) : std::string(); }
std::string faithOf(const World& w, const Character& c) { return person(c) ? deriveFaith(w, c, 0) : std::string(); }
double pietyOf(const World& w, const Character& c) { return c.piety >= 0 ? c.piety : basePiety(cultureRules(w.content()), c); }
double cultureBondOf(const World& w, const Character& c) {
    return c.cultureBond >= 0 ? c.cultureBond : cultureRules(w.content()).person.bondStart;
}

std::string TraditionState::status(const CultureRules& r) const {
    if (strength >= r.tradition.fixed) return "fixed";
    if (strength >= r.tradition.forming) return "forming";
    return "fading";
}

std::map<std::string, TraditionState> settlementTraditions(const World& w, EntityId sid) {
    std::map<std::string, TraditionState> out;
    const Json* s = w.moduleState("culture");
    if (s && s->is_object() && s->contains("trad")) {
        const std::string key = std::to_string(sid);
        if ((*s)["trad"].contains(key)) {
            for (auto it = (*s)["trad"][key].begin(); it != (*s)["trad"][key].end(); ++it)
                out[it.key()] = {it.value().value("s", 0.0), it.value().value("since", 0), it.value().value("p", 0.0)};
            return out;
        }
    }
    // not yet passed a day: what it would start with
    const CultureRules& r = cultureRules(w.content());
    if (const Json* cd = cultureData(w.content(), settlementCulture(w, sid)))
        for (const std::string& t : strs(*cd, "traditions"))
            if (r.trad(t)) out[t] = {r.tradition.start, 0, 0.0};
    return out;
}

std::map<std::string, double> settlementValues(const World& w, EntityId sid) {
    const CultureRules& r = cultureRules(w.content());
    std::map<std::string, double> v;
    if (const Json* cd = cultureData(w.content(), settlementCulture(w, sid))) v = dmap(*cd, "values");
    for (const auto& [id, st] : settlementTraditions(w, sid))
        if (const TraditionDef* t = r.trad(id))
            for (const auto& [k, d] : t->values) v[k] += d * st.strength;
    for (auto& [k, x] : v) x = std::clamp(x, -1.0, 1.0);
    return v;
}

SettlementBelief settlementBelief(const World& w, EntityId sid) {
    SettlementBelief b;
    std::map<std::string, double> sum;
    for (const auto& [id, c] : w.characters()) {
        if (!person(c) || !c.body.alive || settlementOfPerson(w, c) != sid) continue;
        ++b.residents;
        ++b.cultures[cultureOf(w, c)];
        const std::string f = faithOf(w, c);
        ++b.faiths[f];
        sum[f] += pietyOf(w, c);
    }
    for (const auto& [f, n] : b.faiths) b.piety[f] = n > 0 ? sum[f] / n : 0.0;
    return b;
}

int dayOfYear(const World& w) { return ((w.startDay() + w.day()) % kDaysPerYear + kDaysPerYear) % kDaysPerYear; }

std::string holyDayOn(const FaithDef& f, int day) {
    for (const auto& h : f.holyDays)
        if (h.day == day) return h.id;
    return "";
}

void cultureDaily(World& w) {
    const CultureRules& r = cultureRules(w.content());
    if (!r.loaded) return;
    const Tick from = w.now() - kTicksPerDay;
    const int today = w.day(), doy = dayOfYear(w);
    Json& state = stateOf(w);

    // 1. everyone gets what is theirs (first pass after birth or a load of an old save)
    for (auto& [id, c] : w.characters()) {
        if (!person(c) || !c.body.alive) continue;
        if (c.culture.empty()) c.culture = deriveCulture(w, c, 0);
        if (c.faith.empty()) c.faith = deriveFaith(w, c, 0);
        if (c.piety < 0) c.piety = basePiety(r, c);
        if (c.cultureBond < 0) c.cultureBond = r.person.bondStart;
    }

    // 2. who lives where; who practised what today
    struct Place {
        std::vector<Character*> people;
        std::map<std::string, std::set<EntityId>> practised; // tradition -> who kept it today
        std::map<std::string, int> faiths;
    };
    std::map<EntityId, Place> places;
    std::map<std::string, std::vector<std::string>> byMemory; // memory type -> traditions it keeps
    for (const auto& id : r.traditionOrder)
        for (const auto& m : r.traditions.at(id).practice) byMemory[m].push_back(id);
    for (auto& [id, c] : w.characters()) {
        if (!person(c) || !c.body.alive) continue;
        const EntityId sid = settlementOfPerson(w, c);
        if (sid == kNone) continue;
        Place& p = places[sid];
        p.people.push_back(&c);
        ++p.faiths[c.faith];
        recent(c, from, [&](const MemoryEntry& m) {
            if (auto it = byMemory.find(m.type); it != byMemory.end())
                for (const auto& t : it->second) p.practised[t].insert(c.id);
        });
    }

    // 3. the settlements' traditions
    for (auto& [sid, p] : places) {
        if (p.people.empty()) continue;
        const std::string key = std::to_string(sid);
        Json& tr = state["trad"];
        if (!tr.contains(key)) {
            tr[key] = Json::object();
            if (const Json* cd = cultureData(w.content(), settlementCulture(w, sid)))
                for (const std::string& t : strs(*cd, "traditions"))
                    if (r.trad(t)) tr[key][t] = {{"s", r.tradition.start}, {"since", today}, {"p", 0.0}};
        }
        Json& mine = tr[key];
        const double n = static_cast<double>(p.people.size());
        // carriers: whose culture knows the custom (a newcomer brings his people's ways)
        std::map<std::string, int> carriers;
        for (Character* c : p.people)
            if (const Json* cd = cultureData(w.content(), c->culture))
                for (const std::string& t : strs(*cd, "traditions")) ++carriers[t];
        for (const auto& id : r.traditionOrder) {
            const auto pit = p.practised.find(id);
            const double share = pit == p.practised.end() ? 0.0 : pit->second.size() / n;
            if (mine.contains(id)) {
                Json& e = mine[id];
                const double was = e.value("s", 0.0);
                const double s = std::clamp(was + r.tradition.gain * std::min(1.0, share / r.tradition.normShare) - r.tradition.fade, 0.0, 1.0);
                e["s"] = s;
                e["p"] = share;
                if (was < r.tradition.fixed && s >= r.tradition.fixed)
                    w.chronicle("culture", {}, "chronicle.culture.fixed", {{"trad", id}, {"place", w.settlements().count(sid) ? w.settlements().at(sid).name : ""}});
                if (s < r.tradition.lost) {
                    w.chronicle("culture", {}, "chronicle.culture.lost", {{"trad", id}, {"place", w.settlements().count(sid) ? w.settlements().at(sid).name : ""}});
                    mine.erase(id);
                }
            } else if (share > 0.0) {
                // takes root: brought by its carriers, or born of the practice itself (slower)
                const double seed = r.tradition.seed * std::min(1.0, share / r.tradition.normShare) * (carriers.count(id) ? 1.0 : 0.5);
                if (seed >= r.tradition.lost) {
                    mine[id] = {{"s", seed}, {"since", today}, {"p", share}};
                    w.chronicle("culture", {}, "chronicle.culture.rooted", {{"trad", id}, {"place", w.settlements().count(sid) ? w.settlements().at(sid).name : ""}});
                }
            }
        }
    }

    // 4. people: bond, piety, conversion, the weights, moodlets
    for (auto& [sid, p] : places) {
        const std::string sc = settlementCulture(w, sid);
        const auto held = settlementTraditions(w, sid);
        std::string major;
        int majorN = 0;
        for (const auto& [f, k] : p.faiths)
            if (k > majorN) major = f, majorN = k;
        const CultureDef* scd = nullptr;
        if (auto it = w.content().peoples.cultures.find(sc); it != w.content().peoples.cultures.end()) scd = &it->second;
        for (Character* cp : p.people) {
            Character& c = *cp;
            // the bond to one's culture
            if (!sc.empty() && c.culture != sc) {
                double welcome = scd ? scd->welcomeOther : 0.5;
                if (scd)
                    if (auto it = scd->welcome.find(raceOf(w.content(), c).id); it != scd->welcome.end()) welcome = it->second;
                const double rate = (c.body.stage() == "child" ? r.person.driftChild : r.person.driftAdult) * std::max(r.person.driftFloor, welcome);
                c.cultureBond = std::max(0.0, c.cultureBond - rate);
                if (c.cultureBond < r.person.switchBelow) {
                    const std::string was = c.culture;
                    c.culture = sc;
                    c.cultureBond = r.person.afterSwitch;
                    MemoryEntry m;
                    m.type = "adopted_ways";
                    m.valence = 0.1;
                    m.salience = 0.5;
                    w.remember(c, m);
                    w.chronicle("culture", {c.id}, "chronicle.culture.adopted", {{"who", c.id}, {"culture", sc}, {"from_culture", was}});
                }
            } else c.cultureBond = std::min(1.0, c.cultureBond + r.person.grow);

            // piety: deeds of the faith, sins, the base it relaxes to
            const FaithDef* fd = r.faithDef(c.faith);
            std::set<std::string> deeds;
            recent(c, from, [&](const MemoryEntry& m) { deeds.insert(m.type); });
            if (fd) {
                for (const auto& d : deeds) {
                    if (auto it = fd->rites.find(d); it != fd->rites.end()) c.piety += it->second;
                    if (auto it = fd->sins.find(d); it != fd->sins.end()) c.piety -= it->second;
                }
                if (!holyDayOn(*fd, doy).empty()) setMoodlet(w, c, r.faith.holyDayMoodlet);
            }
            const double base = basePiety(r, c);
            c.piety += (base - c.piety) * r.faith.relax;
            c.piety = std::clamp(c.piety, 0.0, 1.0);

            // a faith that calls by a deed (grief -> the forbidden -> the Pale Mourners)
            bool changed = false;
            for (const auto& [fid, f] : r.faiths) {
                if (fid == c.faith || f.drawnBy.empty()) continue;
                for (const auto& [mem, chance] : f.drawnBy)
                    if (deeds.count(mem) && w.rng(c.id, "faith_call_" + fid).uniform() < chance) {
                        convert(w, c, fid, "called");
                        changed = true;
                        break;
                    }
                if (changed) break;
            }
            // the pull of the people around
            if (!changed && c.piety < r.faith.convertBelow && c.body.stage() != "child") {
                std::map<std::string, double> pull;
                auto add = [&](const std::string& f, const char* tie) {
                    if (f.empty() || f == c.faith) return;
                    const FaithDef* o = r.faithDef(f);
                    auto it = r.faith.pull.find(tie);
                    if (o && it != r.faith.pull.end()) pull[f] += it->second * o->zeal;
                };
                if (const Character* sp = w.character(c.status.spouse); sp && sp->body.alive) add(sp->faith, "spouse");
                if (const Family* fam = w.familyOf(c))
                    for (EntityId m : fam->members)
                        if (const Character* o = w.character(m); o && o->id != c.id && o->body.alive && o->id != c.status.spouse) add(o->faith, "household");
                if (majorN * 2 > static_cast<int>(p.people.size())) add(major, "settlement");
                for (const auto& [oid, rel] : c.relations)
                    if (rel.affinity >= r.faith.friendAffinity)
                        if (const Character* o = w.character(oid); o && o->body.alive) add(o->faith, "friend");
                std::string best;
                double bestPull = 0;
                for (const auto& [f, v] : pull)
                    if (v > bestPull) best = f, bestPull = v;
                if (!best.empty()) {
                    const double own = fd ? fd->tolerance : 1.0;
                    const double chance = r.faith.convertBase * (1.0 - c.piety) * bestPull * (1.0 - r.faith.keep * (1.0 - own));
                    if (w.rng(c.id, "faith_pull").uniform() < chance) {
                        convert(w, c, best, "pull");
                        fd = r.faithDef(c.faith);
                    }
                }
            }

            // behaviour: the place's held traditions and one's faith bend the goals
            std::map<std::string, double> wgt;
            std::set<std::string> mine;
            if (const Json* cd = cultureData(w.content(), c.culture))
                for (const std::string& t : strs(*cd, "traditions")) mine.insert(t);
            for (const auto& [tid, st] : held)
                if (const TraditionDef* t = r.trad(tid)) {
                    const double k = st.strength * (mine.count(tid) ? 1.0 : 0.5);
                    for (const auto& [g, d] : t->goals) wgt[g] += d * k;
                    if (!t->moodlet.empty() && st.strength >= r.tradition.fixed)
                        for (const auto& m : t->practice)
                            if (deeds.count(m)) {
                                setMoodlet(w, c, t->moodlet);
                                break;
                            }
                }
            if (fd)
                for (const auto& [g, d] : fd->goals) wgt[g] += d * c.piety * r.faith.weightsScale;
            auto& mods = c.ai.mods;
            mods.erase(std::remove_if(mods.begin(), mods.end(), [](const WeightMod& m) { return m.source == "culture"; }), mods.end());
            for (const auto& [g, d] : wgt) {
                if (std::abs(d) < 1e-4) continue;
                WeightMod m;
                m.target = "goal:" + g;
                m.delta = d;
                m.created = w.now();
                m.ttl = static_cast<Tick>(std::max(1, r.tradition.weightTtlDays)) * kTicksPerDay + 1;
                m.decay = "none";
                m.reason = "culture";
                m.source = "culture";
                mods.push_back(m);
            }
        }
    }
}

namespace {

bool handleCulture(World& w, Command& cmd) {
    if (cmd.type != "set_faith" && cmd.type != "set_culture") return false;
    Character* c = cmd.args.contains("id") ? w.character(cmd.args.at("id").get<EntityId>()) : nullptr;
    if (!c || c->isAnimal()) {
        cmd.result = "rejected:no_such_character";
        return true;
    }
    const CultureRules& r = cultureRules(w.content());
    if (cmd.type == "set_faith") {
        const std::string f = cmd.args.value("faith", std::string());
        if (!r.faithDef(f)) {
            cmd.result = "rejected:unknown_faith";
            return true;
        }
        c->faith = f;
        c->piety = std::clamp(cmd.args.value("piety", pietyOf(w, *c)), 0.0, 1.0);
    } else {
        const std::string k = cmd.args.value("culture", std::string());
        if (!w.content().peoples.cultures.count(k)) {
            cmd.result = "rejected:unknown_culture";
            return true;
        }
        c->culture = k;
        c->cultureBond = std::clamp(cmd.args.value("bond", cultureBondOf(w, *c)), 0.0, 1.0);
    }
    cmd.result = "ok";
    return true;
}

} // namespace

void installCulture(World& w) { w.addCommandHandler(handleCulture); }

void validateCulture(const Content& c, ValidationReport& rep) {
    const CultureRules& r = cultureRules(c);
    for (const auto& e : r.errors) rep.errors.push_back(e);
    if (!r.loaded) return;
    const std::set<std::string> values(r.values.begin(), r.values.end());
    auto checkValues = [&](const std::map<std::string, double>& v, const std::string& where) {
        for (const auto& [k, x] : v)
            if (!values.count(k)) rep.errors.push_back(where + ": unknown value " + k);
    };
    auto checkGoals = [&](const std::map<std::string, double>& g, const std::string& where) {
        for (const auto& [k, x] : g)
            if (std::none_of(c.goals.begin(), c.goals.end(), [&](const GoalDef& d) { return d.id == k; }))
                rep.errors.push_back(where + ": unknown goal " + k);
    };
    for (const auto& [id, t] : r.traditions) {
        checkValues(t.values, "traditions." + id);
        checkGoals(t.goals, "traditions." + id);
        if (!t.moodlet.empty() && !c.moodlets.count(t.moodlet)) rep.errors.push_back("traditions." + id + ": unknown moodlet " + t.moodlet);
    }
    for (const auto& [id, f] : r.faiths) {
        checkValues(f.values, "religions." + id);
        checkGoals(f.goals, "religions." + id);
        for (const auto& h : f.holyDays)
            if (h.day < 0 || h.day >= kDaysPerYear) rep.errors.push_back("religions." + id + ": holy day " + h.id + " out of the year");
    }
    if (!r.faith.holyDayMoodlet.empty() && !c.moodlets.count(r.faith.holyDayMoodlet))
        rep.errors.push_back("culture rules: unknown moodlet " + r.faith.holyDayMoodlet);
    for (const auto& [id, cd] : c.peoples.cultures) {
        const Json& d = cd.data;
        const std::string f = d.value("faith", std::string());
        if (!f.empty() && !r.faithDef(f)) rep.errors.push_back("cultures." + id + ": unknown faith " + f);
        checkValues(dmap(d, "values"), "cultures." + id);
        for (const auto& t : strs(d, "traditions"))
            if (!r.trad(t)) rep.errors.push_back("cultures." + id + ": unknown tradition " + t);
    }
}

} // namespace lr::sim
