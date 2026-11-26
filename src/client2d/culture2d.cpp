#include "client2d/culture2d.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "sim/culture.hpp"
#include "sim/magic_society.hpp"
#include "sim/species.hpp"

namespace lr::client2d {

using namespace lr::sim;

namespace {

double r2(double v) { return std::round(v * 100) / 100; }

std::string textOr(const ViewText& t, const std::string& key, const std::string& fallback) {
    if (t.loc && t.loc->has(t.lang, key)) return t.loc->text(t.lang, key);
    return fallback;
}

std::string personName(const ViewText& t, const Character& c) {
    if (!t.loc) return c.name;
    const Json n = t.loc->personNoun(t.lang, c);
    return n.is_object() ? n.value("nom", c.name) : c.name;
}

EntityId homeOf(const World& w, const Character& c) {
    if (c.status.settlement != kNone) return c.status.settlement;
    if (const Family* f = w.familyOf(c)) return f->settlement;
    return c.standing.at;
}

std::vector<std::string> strs(const Json& j, const char* k) {
    std::vector<std::string> out;
    if (j.is_object() && j.contains(k) && j[k].is_array())
        for (const auto& s : j[k])
            if (s.is_string()) out.push_back(s.get<std::string>());
    return out;
}

// values in the order of values.json, then any other
Json valuesJson(const ViewText& t, const CultureRules& r, const std::map<std::string, double>& v) {
    Json out = Json::array();
    std::set<std::string> done;
    auto one = [&](const std::string& id, double x) {
        out.push_back({{"id", id}, {"name", t.name("culture.value", id)}, {"desc", textOr(t, "culture.value." + id + ".desc", "")}, {"v", r2(x)}});
        done.insert(id);
    };
    for (const auto& id : r.values)
        if (auto it = v.find(id); it != v.end()) one(id, it->second);
    for (const auto& [id, x] : v)
        if (!done.count(id)) one(id, x);
    return out;
}

Json tradJson(const ViewText& t, const CultureRules& r, const std::string& id) {
    const TraditionDef* d = r.trad(id);
    Json j = {{"id", id}, {"name", t.name("culture.trad", id)}, {"desc", textOr(t, "culture.trad." + id + ".desc", "")}};
    if (d) {
        j["kind"] = d->kind;
        j["kindName"] = t.name("culture.trad_kind", d->kind);
        Json pr = Json::array();
        for (const auto& m : d->practice) pr.push_back(t.name("culture.deed", m));
        j["practice"] = pr;
    }
    return j;
}

Json cultureCard(const World& w, const ViewText& t, const CultureRules& r, const std::string& id) {
    const Content& ct = w.content();
    const Json* d = cultureData(ct, id);
    Json j = {{"id", id}, {"name", t.name("culture", id)}, {"desc", textOr(t, "culture." + id + ".desc", "")}};
    if (!d) return j;
    const CultureDef& cd = ct.peoples.cultures.at(id);
    j["species"] = cd.species;
    j["speciesName"] = t.name("species", cd.species);
    j["faith"] = d->value("faith", std::string());
    j["faithName"] = t.name("faith", j["faith"].get<std::string>());
    std::map<std::string, double> vals;
    if (d->contains("values") && (*d)["values"].is_object())
        for (auto it = (*d)["values"].begin(); it != (*d)["values"].end(); ++it)
            if (it.value().is_number()) vals[it.key()] = it.value().get<double>();
    j["values"] = valuesJson(t, r, vals);
    Json trads = Json::array();
    for (const auto& tid : strs(*d, "traditions")) trads.push_back(tradJson(t, r, tid));
    j["traditions"] = trads;
    Json rites = Json::array();
    if (d->contains("rites") && (*d)["rites"].is_object())
        for (const char* k : {"coming_of_age", "marriage", "burial"})
            if ((*d)["rites"].contains(k) && (*d)["rites"][k].is_string()) {
                const std::string v = (*d)["rites"][k].get<std::string>();
                const std::string name = std::string(k) == "burial" ? t.name("faith.burial", v) : t.name("culture.rite", v);
                rites.push_back({{"kind", k}, {"kindName", t.name("culture.rite_kind", k)}, {"id", v}, {"name", name}});
            }
    j["rites"] = rites;
    Json food = Json::object();
    if (d->contains("food") && (*d)["food"].is_object())
        for (const char* k : {"likes", "avoids"}) {
            Json a = Json::array();
            for (const auto& f : strs((*d)["food"], k)) a.push_back(t.name("culture.food", f));
            food[k] = a;
        }
    j["food"] = food;
    // how it takes other peoples, warmest first
    Json wel = Json::array();
    for (const auto& [race, v] : cd.welcome) wel.push_back({{"id", race}, {"name", t.name("species", race)}, {"v", r2(v)}});
    std::stable_sort(wel.begin(), wel.end(), [](const Json& a, const Json& b) { return a["v"].get<double>() > b["v"].get<double>(); });
    j["welcome"] = wel;
    j["welcomeOther"] = r2(cd.welcomeOther);
    Json teach = Json::array(), forbid = Json::array(), norms = Json::array();
    for (const auto& a : cd.teachAspects) teach.push_back(t.name("aspect", a));
    for (const auto& a : cd.forbidAspects) forbid.push_back(t.name("aspect", a));
    if (cd.norms.is_object())
        for (auto it = cd.norms.begin(); it != cd.norms.end(); ++it)
            if (it.value().is_string())
                norms.push_back({{"class", t.name("culture.mclass", it.key())}, {"level", t.name("culture.norm", it.value().get<std::string>())}});
    j["teach"] = teach;
    j["forbid"] = forbid;
    j["norms"] = norms;
    return j;
}

Json faithCard(const World& w, const ViewText& t, const CultureRules& r, const std::string& id) {
    const FaithDef* f = r.faithDef(id);
    Json j = {{"id", id}, {"name", t.name("faith", id)}, {"desc", textOr(t, "faith." + id + ".desc", "")}};
    if (!f) return j;
    j["kind"] = f->kind;
    j["kindName"] = t.name("faith.kind", f->kind);
    j["heresy"] = f->heresy;
    Json gods = Json::array();
    for (const auto& g : f->deities)
        gods.push_back({{"id", g.id}, {"name", textOr(t, "faith." + id + ".god." + g.id + ".name", g.id)}, {"domain", g.domain}, {"domainName", t.name("aspect", g.domain)}});
    j["deities"] = gods;
    j["values"] = valuesJson(t, r, f->values);
    auto deeds = [&](const std::map<std::string, double>& m) {
        Json a = Json::array();
        for (const auto& [mem, v] : m)
            if (v > 0) a.push_back({{"id", mem}, {"name", t.name("culture.deed", mem)}, {"v", r2(v)}});
        std::stable_sort(a.begin(), a.end(), [](const Json& x, const Json& y) { return x["v"].get<double>() > y["v"].get<double>(); });
        return a;
    };
    j["rites"] = deeds(f->rites);
    j["sins"] = deeds(f->sins);
    const int doy = dayOfYear(w);
    Json days = Json::array();
    for (const auto& h : f->holyDays) {
        const int in = ((h.day - doy) % kDaysPerYear + kDaysPerYear) % kDaysPerYear;
        days.push_back({{"id", h.id}, {"name", textOr(t, "faith." + id + ".day." + h.id + ".name", h.id)}, {"day", h.day}, {"in", in}});
    }
    std::stable_sort(days.begin(), days.end(), [](const Json& a, const Json& b) { return a["in"].get<int>() < b["in"].get<int>(); });
    j["holy"] = days;
    j["burial"] = t.name("faith.burial", f->burial);
    j["clergy"] = t.name("faith.clergy", f->clergy);
    Json sacred = Json::array(), blessed = Json::array(), forbidden = Json::array();
    for (const auto& s : f->sacred) sacred.push_back(t.name("object", s));
    for (const auto& a : f->blessed) blessed.push_back(t.name("aspect", a));
    for (const auto& a : f->forbidden) forbidden.push_back(t.name("aspect", a));
    j["sacred"] = sacred;
    j["blessed"] = blessed;
    j["forbidden"] = forbidden;
    j["zeal"] = r2(f->zeal);
    j["tolerance"] = r2(f->tolerance);
    return j;
}

} // namespace

Json cultureJson(const World& w, const ViewText& t, EntityId viewer, EntityId pid, const std::string& faithPick,
                 const std::string& culturePick) {
    const CultureRules& r = cultureRules(w.content());
    if (!r.loaded) return nullptr;
    const Character* p = w.character(pid != kNone ? pid : viewer);
    if (!p) p = w.character(viewer);
    Json d = Json::object();
    std::string culture = culturePick, faith = faithPick;
    EntityId sid = kNone;
    if (p && !p->isAnimal()) {
        const std::string pc = cultureOf(w, *p), pf = faithOf(w, *p);
        d["person"] = {{"id", p->id},
                       {"name", personName(t, *p)},
                       {"culture", pc},
                       {"cultureName", t.name("culture", pc)},
                       {"bond", r2(cultureBondOf(w, *p))},
                       {"faith", pf},
                       {"faithName", t.name("faith", pf)},
                       {"piety", r2(pietyOf(w, *p))},
                       {"species", raceOf(w.content(), *p).id},
                       {"speciesName", t.name("species", raceOf(w.content(), *p).id)}};
        if (culture.empty()) culture = pc;
        if (faith.empty()) faith = pf;
        sid = homeOf(w, *p);
    }
    if (sid != kNone && w.settlements().count(sid)) {
        const Settlement& st = w.settlements().at(sid);
        const std::string sc = settlementCulture(w, sid);
        const SettlementBelief b = settlementBelief(w, sid);
        Json s = {{"id", sid}, {"name", st.name}, {"culture", sc}, {"cultureName", t.name("culture", sc)}, {"residents", b.residents}};
        Json cults = Json::array(), faiths = Json::array(), trads = Json::array();
        for (const auto& [id, n] : b.cultures) cults.push_back({{"id", id}, {"name", t.name("culture", id)}, {"n", n}});
        for (const auto& [id, n] : b.faiths)
            faiths.push_back({{"id", id}, {"name", t.name("faith", id)}, {"n", n}, {"piety", r2(b.piety.count(id) ? b.piety.at(id) : 0.0)}});
        auto byN = [](const Json& a, const Json& x) { return a["n"].get<int>() > x["n"].get<int>(); };
        std::stable_sort(cults.begin(), cults.end(), byN);
        std::stable_sort(faiths.begin(), faiths.end(), byN);
        // carriers: the customs the residents' cultures know
        std::set<std::string> carried;
        for (const auto& [cid, n] : b.cultures)
            if (const Json* cd = cultureData(w.content(), cid))
                for (const auto& tid : strs(*cd, "traditions")) carried.insert(tid);
        for (const auto& [tid, ts] : settlementTraditions(w, sid)) {
            Json tj = tradJson(t, r, tid);
            tj["s"] = r2(ts.strength);
            tj["status"] = ts.status(r);
            tj["statusName"] = textOr(t, "ui2d.cult.status." + ts.status(r), ts.status(r));
            tj["practised"] = r2(ts.practised);
            tj["since"] = ts.since;
            tj["trend"] = ts.practised * r.tradition.gain / r.tradition.normShare > r.tradition.fade ? 1 : -1;
            tj["carried"] = carried.count(tid) > 0;
            trads.push_back(tj);
        }
        std::stable_sort(trads.begin(), trads.end(), [](const Json& a, const Json& x) { return a["s"].get<double>() > x["s"].get<double>(); });
        s["cultures"] = cults;
        s["faiths"] = faiths;
        s["traditions"] = trads;
        s["values"] = valuesJson(t, r, settlementValues(w, sid));
        s["thresholds"] = {{"fixed", r.tradition.fixed}, {"forming", r.tradition.forming}};
        d["settlement"] = s;
        if (culture.empty()) culture = sc;
    }
    if (!culture.empty()) d["culture"] = cultureCard(w, t, r, culture);
    if (faith.empty() && !culture.empty()) faith = cultureFaith(w.content(), culture);
    if (!faith.empty()) {
        Json fj = faithCard(w, t, r, faith);
        if (d.contains("settlement"))
            for (const auto& f : d["settlement"]["faiths"])
                if (f["id"] == faith) fj["here"] = f["n"];
        d["faith"] = fj;
    }
    Json all = Json::array(), cultures = Json::array();
    for (const auto& [id, f] : r.faiths)
        if (!f.heresy || id == faith) all.push_back({{"id", id}, {"name", t.name("faith", id)}});
    for (const auto& [id, cd] : w.content().peoples.cultures) cultures.push_back({{"id", id}, {"name", t.name("culture", id)}});
    d["faiths"] = all;
    d["cultures"] = cultures;
    d["day"] = dayOfYear(w);
    return d;
}

Json inspectJson(const World& w, const ViewText& t, EntityId viewer, EntityId target) {
    const Character* me = w.character(viewer);
    const Character* o = w.character(target);
    if (!o) return nullptr;
    const CultureRules& r = cultureRules(w.content());
    const Json& ins = r.inspect;
    auto th = [&](const char* f) {
        if (ins.contains("fields") && ins["fields"].contains(f) && ins["fields"][f].is_number()) return ins["fields"][f].get<double>();
        return 0.0;
    };
    // who knows: the viewer and the viewer's dynasty (its households)
    std::vector<const Character*> knowers;
    if (me) {
        const Family* mf = w.familyOf(*me);
        const EntityId dyn = mf ? mf->dynasty : kNone;
        for (const auto& [id, c] : w.characters()) {
            if (c.isAnimal() || !c.body.alive) continue;
            const Family* f = w.familyOf(c);
            if (id == viewer || (mf && f && (f->id == mf->id || (dyn != kNone && f->dynasty == dyn)))) knowers.push_back(&c);
        }
    }
    double know = 0.0;
    const Character* best = nullptr;
    bool kin = false;
    const double sameS = ins.value("same_settlement", 0.25);
    for (const Character* k : knowers) {
        double v = 0.0;
        if (k->id == target) kin = true;
        if (auto it = k->relations.find(target); it != k->relations.end()) v = it->second.familiarity;
        if (homeOf(w, *k) != kNone && homeOf(w, *k) == homeOf(w, *o)) v += sameS;
        if (v > know) know = v, best = k;
    }
    for (const Character* k : knowers)
        if (k->status.family != kNone && k->status.family == o->status.family) kin = true;
    if (kin) know = std::max(know, ins.value("kin", 1.0));
    know = std::min(1.0, know);
    auto open = [&](const char* f) { return know >= th(f); };

    Json d = {{"id", target}, {"know", r2(know)}, {"kin", kin}, {"alive", o->body.alive}, {"sp", o->species}};
    if (best && best->id != viewer) d["knower"] = personName(t, *best);
    // seen: kind, sex, rough age, what he does, how he looks hurt
    const RaceDef& rd = raceOf(w.content(), *o);
    d["sex"] = std::string(1, o->body.sex);
    d["speciesName"] = o->isAnimal() ? t.name("species", o->species) : t.name("species", rd.id);
    d["stage"] = textOr(t, "life_stage." + o->body.stage() + ".name", o->body.stage());
    d["hp"] = r2(o->body.health);
    if (o->ai.fsm.interaction.size() && o->ai.fsm.interaction != "goto") d["act"] = t.name("interaction", o->ai.fsm.interaction);
    d["name"] = open("name") ? personName(t, *o) : textOr(t, "ui2d.insp.stranger." + std::string(1, o->body.sex), "?");
    d["known"] = open("name");
    if (o->isAnimal()) return d;
    Json fields = Json::object();
    if (open("age")) fields["age"] = o->body.ageDays / kDaysPerYear;
    if (open("house")) {
        if (const Family* f = w.familyOf(*o)) fields["house"] = t.loc ? t.loc->personNoun(t.lang, f->name, 'm').value("nom", f->name) : f->name;
        if (auto st = w.settlements().find(homeOf(w, *o)); st != w.settlements().end()) fields["home"] = st->second.name;
    }
    if (open("occupation")) {
        std::string role = textOr(t, "ui2d.sheet.role." + o->status.role + "." + std::string(1, o->body.sex), textOr(t, "household_role." + o->status.role + ".name", ""));
        if (!o->occupation.empty()) role += (role.empty() ? "" : " · ") + textOr(t, "ui2d.sheet.occ." + o->occupation, o->occupation);
        if (!role.empty()) fields["occupation"] = role;
    }
    const std::string oc = cultureOf(w, *o);
    if (open("culture")) fields["culture"] = {{"id", oc}, {"name", t.name("culture", oc)}};
    else if (ins.value("culture_by_look", false) && !rd.cultureDefault.empty())
        fields["culture"] = {{"id", rd.cultureDefault}, {"name", t.name("culture", rd.cultureDefault)}, {"guess", true}};
    if (open("faith")) {
        const std::string of = faithOf(w, *o);
        Json fj = {{"id", of}, {"name", t.name("faith", of)}};
        if (open("piety")) fj["piety"] = r2(pietyOf(w, *o));
        fields["faith"] = fj;
    }
    if (open("mood")) fields["mood"] = r2(o->mood(w.now()));
    if (open("traits")) {
        Json a = Json::array();
        for (const auto& tr : o->traits) a.push_back(t.name("trait", tr));
        fields["traits"] = a;
    }
    if (open("skills")) {
        std::vector<std::pair<double, std::string>> sk;
        for (const auto& [k, v] : o->skills) sk.push_back({v, k});
        std::sort(sk.rbegin(), sk.rend());
        Json a = Json::array();
        for (std::size_t i = 0; i < sk.size() && i < std::size_t(ins.value("skills_top", 3)); ++i)
            a.push_back({{"name", t.name("knowledge", sk[i].second)}, {"v", r2(sk[i].first)}});
        fields["skills"] = a;
    }
    if (open("relations")) {
        std::vector<std::pair<double, EntityId>> rs;
        for (const auto& [oid, rel] : o->relations)
            if (const Character* x = w.character(oid); x && !x->isAnimal() && oid != viewer) rs.push_back({std::abs(rel.affinity), oid});
        std::sort(rs.rbegin(), rs.rend());
        Json a = Json::array();
        for (std::size_t i = 0; i < rs.size() && i < std::size_t(ins.value("relations_top", 4)); ++i) {
            const double af = o->relations.at(rs[i].second).affinity;
            a.push_back({{"id", rs[i].second}, {"name", personName(t, *w.character(rs[i].second))}, {"v", r2(af)}});
        }
        fields["relations"] = a;
    }
    d["fields"] = fields;
    // what the viewer feels, and how the other is to the viewer as far as it shows
    if (me && me->id != target) {
        if (auto it = me->relations.find(target); it != me->relations.end())
            d["mine"] = {{"familiarity", r2(it->second.familiarity)}, {"trust", r2(it->second.trust)}, {"affinity", r2(it->second.affinity)}, {"respect", r2(it->second.respect)}, {"fear", r2(it->second.fear)}};
        // shared ground: the same culture, the same faith (as far as known)
        Json common = Json::array();
        if (open("culture") && cultureOf(w, *me) == oc) common.push_back(textOr(t, "ui2d.insp.same_culture", "same culture"));
        if (open("faith") && faithOf(w, *me) == faithOf(w, *o)) common.push_back(textOr(t, "ui2d.insp.same_faith", "same faith"));
        d["common"] = common;
    }
    // what they remember of the person, newest first
    struct Mem {
        Tick tick;
        const Character* who;
        const MemoryEntry* m;
    };
    std::vector<Mem> mems;
    for (const Character* k : knowers)
        for (const MemoryEntry& m : k->memory)
            if (std::find(m.who.begin(), m.who.end(), target) != m.who.end()) mems.push_back({m.tick, k, &m});
    std::stable_sort(mems.begin(), mems.end(), [](const Mem& a, const Mem& b) { return a.tick > b.tick; });
    Json ma = Json::array();
    std::set<std::string> told;   // one line per thing remembered (a day's talk is one memory)
    const std::size_t cap = std::size_t(ins.value("memories", 6));
    for (std::size_t i = 0; i < mems.size() && ma.size() < cap; ++i) {
        const Mem& m = mems[i];
        std::string text = t.name("memory", m.m->type);
        const std::string key = "memory." + m.m->type + ".text";
        if (t.loc && t.loc->has(t.lang, key)) {
            Json p = Json::object();
            if (!m.m->who.empty()) {
                p["who"] = m.m->who[0];
                p["target"] = m.m->who.size() > 1 ? m.m->who[1] : m.m->who[0];
            }
            if (m.m->object != kNone) p["object"] = m.m->object;
            if (!m.m->goods.empty()) p["item"] = m.m->goods.begin()->first;
            text = t.loc->text(t.lang, key, locArgs(*t.loc, t.lang, w, p, m.who));
        }
        if (!told.insert(text).second) continue;
        ma.push_back({{"day", m.tick / kTicksPerDay}, {"text", text}, {"v", r2(m.m->valence)}, {"src", m.m->source}, {"by", m.who->id == viewer ? "" : personName(t, *m.who)}});
    }
    d["memories"] = ma;
    return d;
}

} // namespace lr::client2d
