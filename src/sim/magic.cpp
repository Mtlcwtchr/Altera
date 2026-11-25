// Magic of aspects, the undead and the paths out of grief (AL-116). See magic.hpp.

#include "sim/magic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

#include "sim/content.hpp"
#include "sim/world.hpp"
#include "sim/world_fields.hpp"
#include "sim/species.hpp"

namespace lr::sim {

namespace fs = std::filesystem;

namespace {

constexpr const char* kState = "magic";

// --- hooks ------------------------------------------------------------------
MagicWorldSink& worldSink() {
    static MagicWorldSink s;
    return s;
}
MagicFieldReader& fieldReader() {
    static MagicFieldReader r;
    return r;
}
MagicAffinity& affinityFn() {
    static MagicAffinity f;
    return f;
}
MagicGiftChance& giftChanceFn() {
    static MagicGiftChance f;
    return f;
}
MagicServiceHook& serviceFn() {
    static MagicServiceHook f;
    return f;
}
std::vector<MagicBodyHook>& bodyHooks() {
    static std::vector<MagicBodyHook> h;
    return h;
}

// --- small helpers ------------------------------------------------------------
Json readJson(const fs::path& p) {
    if (!fs::exists(p)) return Json();
    std::ifstream f(p, std::ios::binary);
    return Json::parse(f);
}

std::vector<std::string> strs(const Json& j, const char* key) {
    std::vector<std::string> out;
    if (j.contains(key) && j[key].is_array())
        for (const auto& s : j[key]) out.push_back(s.get<std::string>());
    return out;
}

// "+2*power", "-0.3*power", "0.5*power", "1" -> value at `power`.
double evalExpr(const Json& e, double power) {
    if (e.is_number()) return e.get<double>();
    if (!e.is_string()) return 0.0;
    std::string s = e.get<std::string>();
    s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
    bool byPower = false;
    if (auto at = s.find("*power"); at != std::string::npos) {
        byPower = true;
        s = s.substr(0, at);
    }
    const double v = std::strtod(s.c_str(), nullptr);
    return byPower ? v * power : v;
}

std::string vecKey(char v) { return std::string(1, v); }

void addMoodlet(World& w, Character& c, const std::string& id) {
    auto md = w.content().moodlets.find(id);
    if (md == w.content().moodlets.end()) return;
    const Tick until = w.now() + md->second.ttl;
    for (auto& m : c.moodlets)
        if (m.id == id) {
            m.until = std::max(m.until, until);
            return;
        }
    c.moodlets.push_back({id, md->second.value, until});
}

void addTrait(Character& c, const std::string& t) {
    if (!c.hasTrait(t)) c.traits.push_back(t);
}
void dropTrait(Character& c, const std::string& t) {
    c.traits.erase(std::remove(c.traits.begin(), c.traits.end(), t), c.traits.end());
}

bool isPerson(const Character& c) { return !c.isAnimal(); }

int today(const World& w) { return static_cast<int>(w.now() / kTicksPerDay); }

Json& state(World& w) {
    Json& s = w.moduleState(kState);
    if (!s.is_object()) s = Json::object();
    for (const char* k : {"people", "undead", "coping", "dead_seen"})
        if (!s.contains(k)) s[k] = Json::object();
    return s;
}
const Json* stateC(const World& w) { return w.moduleState(kState); }

std::string idKey(EntityId id) { return std::to_string(id); }

void loadRules(MagicRules& r, const fs::path& dir) {
    const Json asp = readJson(dir / "aspects.json");
    if (!asp.is_object()) return;
    r.loaded = true;
    for (const auto& a : asp.value("aspects", Json::array())) {
        AspectDef d;
        d.id = a.value("id", "");
        d.group = a.value("group", "");
        d.tags = strs(a, "tags");
        d.opposes = strs(a, "opposes");
        d.feeds = strs(a, "feeds");
        d.baseCost = a.value("base_cost", 1.0);
        d.difficulty = a.value("difficulty", 0.3);
        d.enabled = a.value("enabled", true);
        d.world = a.value("world_channels", Json::object());
        d.body = a.value("body", Json::object());
        r.aspects[d.id] = d;
    }
    const Json fm = readJson(dir / "forms.json");
    for (const auto& f : fm.is_object() ? fm.value("forms", Json::array()) : Json::array()) {
        SpellFormDef d;
        d.id = f.value("id", "");
        d.reach = f.value("reach", "touch");
        d.costMult = f.value("cost_mult", 1.0);
        d.difficulty = f.value("difficulty", 0.0);
        d.rangeM = f.value("range_m", 2.0);
        d.radiusM = f.value("radius_m", 0.0);
        d.time = f.value("time", 2);
        r.forms[d.id] = d;
    }
    const Json md = readJson(dir / "modifiers.json");
    for (const auto& m : md.is_object() ? md.value("modifiers", Json::array()) : Json::array()) {
        ModifierDef d;
        d.id = m.value("id", "");
        d.costPerStep = m.value("cost_per_step", 0.5);
        d.diffPerStep = m.value("diff_per_step", 0.05);
        d.max = m.value("max", 3);
        r.modifiers[d.id] = d;
    }
    const Json cfg = readJson(dir / "magic_config.json");
    if (cfg.is_object()) {
        r.skill = cfg.value("skill", r.skill);
        const Json mana = cfg.value("mana", Json::object());
        r.manaBase = mana.value("base_max", r.manaBase);
        r.manaPerSkill = mana.value("per_skill", r.manaPerSkill);
        r.manaGifted = mana.value("gifted_bonus", r.manaGifted);
        r.manaRegen = mana.value("regen_per_day", r.manaRegen);
        r.costScale = mana.value("cost_scale", r.costScale);
        const Json g = cfg.value("gift", Json::object());
        r.giftTrait = g.value("trait", r.giftTrait);
        r.giftChance = g.value("chance", r.giftChance);
        r.giftLearn = g.value("learn_mult", r.giftLearn);
        r.ceilGifted = g.value("ceiling_gifted", r.ceilGifted);
        r.ceilPlain = g.value("ceiling_plain", r.ceilPlain);
        r.giftCompetence = g.value("competence_bonus", r.giftCompetence);
        const Json kn = cfg.value("knowledge", Json::object());
        r.useMin = kn.value("use_min", r.useMin);
        r.solo = kn.value("solo", r.solo);
        r.full = kn.value("full", r.full);
        r.novicePenalty = kn.value("novice_penalty", r.novicePenalty);
        if (kn.contains("novice_forms")) {
            r.noviceForms.clear();
            for (const auto& s : kn["novice_forms"]) r.noviceForms.insert(s.get<std::string>());
        }
        r.gainCast = kn.value("gain_cast", r.gainCast);
        r.gainTeach = kn.value("gain_teach", r.gainTeach);
        r.gainAwaken = kn.value("gain_awaken", r.gainAwaken);
        r.awakenDaily = kn.value("awaken_daily", r.awakenDaily);
        r.breakthrough = kn.value("breakthrough", r.breakthrough);
        r.breakthroughFrom = kn.value("breakthrough_from", r.breakthroughFrom);
        r.skillGainCast = kn.value("skill_gain_cast", r.skillGainCast);
        const Json cast = cfg.value("cast", Json::object());
        r.k = cast.value("k", r.k);
        r.noise = cast.value("noise", r.noise);
        r.extraAspectDiff = cast.value("extra_aspect_diff", r.extraAspectDiff);
        r.opposesCost = cast.value("opposes_cost", r.opposesCost);
        r.opposesDiff = cast.value("opposes_diff", r.opposesDiff);
        r.aetherPerCost = cast.value("aether_per_cost", r.aetherPerCost);
        r.witnessM = cast.value("witness_m", r.witnessM);
        for (const auto& t : cast.value("tiers", Json::array()))
            r.tiers.push_back({t.value("id", ""), t.value("below", 1.0), t.value("power", 1.0), t.value("backfire", false)});
        const Json de = cfg.value("death", Json::object());
        r.deathAspect = de.value("aspect", r.deathAspect);
        r.markTrait = de.value("mark_trait", r.markTrait);
        r.markAfter = de.value("mark_after", r.markAfter);
        const Json un = cfg.value("undead", Json::object());
        r.undeadTrait = un.value("trait", r.undeadTrait);
        r.mindlessTrait = un.value("mindless_trait", r.mindlessTrait);
        r.raiseMinPower = un.value("raise_min_power", r.raiseMinPower);
        r.wholeMindPower = un.value("whole_mind_power", r.wholeMindPower);
        r.maxDeadDays = un.value("max_dead_days", r.maxDeadDays);
        r.undeadHealth = un.value("health_start", r.undeadHealth);
        r.undeadDecay = un.value("decay_per_day", r.undeadDecay);
        r.upkeepMana = un.value("upkeep_mana", r.upkeepMana);
        r.unboundAfterDays = un.value("unbound_after_days", r.unboundAfterDays);
        r.undeadWitnessM = un.value("witness_m", r.undeadWitnessM);
        r.undeadFear = un.value("fear", r.undeadFear);
        const Json sv = cfg.value("service", Json::object());
        r.healAspect = sv.value("heal_aspect", r.healAspect);
        r.healMinK = sv.value("heal_min_k", r.healMinK);
        r.healBelow = sv.value("heal_health_below", r.healBelow);
        r.healDaily = sv.value("daily", r.healDaily);
        const Json li = cfg.value("lich", Json::object());
        r.lichDeathK = li.value("death_k", r.lichDeathK);
        r.lichRaisedMin = li.value("raised_min", r.lichRaisedMin);
        r.lichDaily = li.value("chance_daily", r.lichDaily);
    }
    if (r.tiers.empty())
        r.tiers = {{"catastrophe", 0.12, 0.6, true}, {"fail", 0.3, 0.0, false}, {"normal", 0.7, 1.0, false},
                   {"good", 0.9, 1.25, false}, {"exemplary", 9.0, 1.5, false}};
    const Json cp = readJson(dir / "coping.json");
    const Json troubles_it = cp.is_object() ? cp.value("troubles", Json::object()) : Json::object();
        for (auto it : troubles_it.items()) {
            CopingTrouble t;
            t.id = it.key();
            t.start = it.value().value("start", "");
            t.bondMin = it.value().value("bond_min", 0.45);
            const Json stages = it.value().value("stages", Json::object());
            for (auto st : stages.items()) {
                CopingStage s;
                s.id = st.key();
                s.act = st.value().value("do", s.id);
                s.minDays = st.value().value("min_days", 1);
                s.daily = st.value().value("daily", 0.3);
                s.quiet = st.value().value("quiet", false);
                for (const auto& n : st.value().value("next", Json::array())) {
                    CopingNext x;
                    x.to = n.value("to", "");
                    x.base = n.value("base", 1.0);
                    x.knows = n.value("knows", 1.0);
                    x.unknown = n.value("unknown", 1.0);
                    x.wBond = n.value("w_bond", 0.0);
                    x.wHonor = n.value("w_honor", 0.0);
                    x.wDeath = n.value("w_death", 0.0);
                    const Json traits_tr = n.value("traits", Json::object());
                    for (auto tr : traits_tr.items())
                        x.traits[tr.key()] = tr.value().get<double>();
                    s.next.push_back(x);
                }
                t.stages[s.id] = s;
            }
            r.troubles[t.id] = t;
        }
}

const char* kBodyKeys[] = {"health", "cure_illness", "heal_wounds", "burn", "cold", "wet", "fire_out", "thirst", "push", "pull",
                           "fear", "calm", "charm", "confuse", "sleep", "fatigue", "wake", "mana", "dispel",
                           "poison", "cure_poison", "bleed", "undead_harm", "undead_heal", "raise", "speak_dead",
                           "lay_rest", "death_touch", "blink", "health_transfer", "strength", "breath"};

bool knownBodyKey(const std::string& k) {
    for (const char* b : kBodyKeys)
        if (k == b) return true;
    return false;
}

} // namespace

// --- content ------------------------------------------------------------------
bool AspectDef::hasTag(const std::string& t) const { return std::find(tags.begin(), tags.end(), t) != tags.end(); }

const AspectDef* MagicRules::aspect(const std::string& id) const {
    auto it = aspects.find(id);
    return it == aspects.end() ? nullptr : &it->second;
}
const SpellFormDef* MagicRules::form(const std::string& id) const {
    auto it = forms.find(id);
    return it == forms.end() ? nullptr : &it->second;
}

const MagicRules& magicRules(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<MagicRules>> cache;
    static const MagicRules kNone;
    if (c.dir.empty()) return kNone;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(c.dir);
    if (it != cache.end()) return *it->second;
    auto r = std::make_unique<MagicRules>();
    loadRules(*r, fs::path(c.dir).parent_path() / "magic");
    return *cache.emplace(c.dir, std::move(r)).first->second;
}

std::vector<std::string> checkMagic(const Content& c) {
    std::vector<std::string> err;
    const MagicRules& r = magicRules(c);
    if (!r.loaded) return err;
    for (const auto& [id, a] : r.aspects) {
        for (const auto& o : a.opposes)
            if (!r.aspect(o)) err.push_back("aspects." + id + ".opposes: unknown aspect '" + o + "'");
        for (const auto& o : a.feeds)
            if (!r.aspect(o)) err.push_back("aspects." + id + ".feeds: unknown aspect '" + o + "'");
        for (const char* v : {"+", "-", "~"}) {
            if (!a.body.contains(v)) err.push_back("aspects." + id + ".body: no vector '" + v + "'");
            if (!a.world.contains(v)) err.push_back("aspects." + id + ".world_channels: no vector '" + v + "'");
        }
    }
    for (const auto& [tid, t] : r.troubles) {
        if (!t.stages.count(t.start)) err.push_back("coping." + tid + ".start: unknown stage '" + t.start + "'");
        for (const auto& [sid, s] : t.stages)
            for (const auto& n : s.next)
                if (!t.stages.count(n.to)) err.push_back("coping." + tid + "." + sid + ": unknown next '" + n.to + "'");
    }
    for (const char* tr : {"gifted", "undead", "mindless", "death_touched", "lich"})
        if (!c.traits.count(tr)) err.push_back(std::string("traits: no '") + tr + "' for magic");
    for (const char* m : {"spoke_with_dead", "dread_dark_magic", "healed_by_magic", "obsessed_with_return", "grieving"})
        if (!c.moodlets.count(m)) err.push_back(std::string("moodlets: no '") + m + "' for magic");
    return err;
}

std::vector<std::string> magicLocKeys(const Content& c) {
    std::vector<std::string> k = {"chronicle.magic.awaken", "chronicle.magic.learn", "chronicle.magic.teach",
                                  "chronicle.magic.breakthrough", "chronicle.magic.cast", "chronicle.magic.backfire",
                                  "chronicle.magic.raise", "chronicle.magic.undead_undone", "chronicle.magic.unbound",
                                  "chronicle.magic.heal", "chronicle.magic.lich", "chronicle.magic.death_mark",
                                  "chronicle.magic.too_late"};
    const MagicRules& r = magicRules(c);
    for (const auto& [tid, t] : r.troubles)
        for (const auto& [sid, s] : t.stages) k.push_back("chronicle.magic.coping." + tid + "." + sid);
    for (const char* code : {"no_magic", "unknown_aspect", "unknown_form", "disabled_aspect", "too_many_aspects",
                             "not_known", "novice_form", "no_mana", "no_target", "out_of_reach", "undead_caster"})
        k.push_back(std::string("magic.reason.") + code);
    for (const auto& [id, a] : r.aspects) {
        k.push_back("aspect." + id + ".name");
        k.push_back("aspect." + id + ".desc");
    }
    for (const auto& [id, f] : r.forms) k.push_back("spell.form." + id + ".name");
    for (const auto& [id, m] : r.modifiers) k.push_back("spell.mod." + id + ".name");
    return k;
}

// --- spells ---------------------------------------------------------------------
int Spell::mod(const std::string& m, int def) const {
    auto it = mods.find(m);
    return it == mods.end() ? def : it->second;
}

std::string Spell::key() const {
    std::vector<std::string> p;
    for (const auto& s : parts) p.push_back(s.aspect + s.vector);
    std::sort(p.begin(), p.end());
    std::string k;
    for (const auto& s : p) k += s + ",";
    k += "|" + form + "|";
    for (const auto& [m, v] : mods)
        if (v) k += m + std::to_string(v) + ",";
    return k;
}

Json spellToJson(const Spell& s) {
    Json a = Json::array();
    for (const auto& p : s.parts) a.push_back(Json::array({p.aspect, vecKey(p.vector)}));
    Json j = {{"aspects", a}, {"form", s.form}, {"mods", s.mods}};
    if (!s.name.empty()) j["name"] = s.name;
    return j;
}

Spell spellFromJson(const Json& j) {
    Spell s;
    for (const auto& a : j.value("aspects", Json::array())) {
        SpellPart p;
        if (a.is_array() && !a.empty()) {
            p.aspect = a.at(0).get<std::string>();
            const std::string v = a.size() > 1 ? a.at(1).get<std::string>() : "+";
            p.vector = v.empty() ? '+' : v[0];
        } else if (a.is_string()) {
            p.aspect = a.get<std::string>();
        }
        s.parts.push_back(p);
    }
    s.form = j.value("form", "touch");
    const Json mods_it = j.value("mods", Json::object());
    for (auto it : mods_it.items())
        s.mods[it.key()] = it.value().get<int>();
    s.name = j.value("name", "");
    return s;
}

// --- a person's magic -----------------------------------------------------------
MagicSelf magicOf(const World& w, const Character& c) {
    MagicSelf m;
    const Json* s = stateC(w);
    if (!s || !s->contains("people")) return m;
    const Json& people = (*s)["people"];
    auto it = people.find(idKey(c.id));
    if (it == people.end()) return m;
    const Json& j = *it;
    m.rolled = j.value("rolled", false);
    m.mana = j.value("mana", 0.0);
    m.manaAt = j.value("mana_at", Tick(0));
    m.aware = j.value("aware", false);
    m.deathUses = j.value("death_uses", 0);
    m.raised = j.value("raised", 0);
    const Json aspects_a = j.value("aspects", Json::object());
    for (auto a : aspects_a.items())
        m.aspects[a.key()] = a.value().get<double>();
    for (const auto& b : j.value("book", Json::array()))
        m.book.push_back({spellFromJson(b.value("spell", Json::object())), b.value("mastery", 0.0), b.value("casts", 0)});
    return m;
}

void setMagicOf(World& w, const Character& c, const MagicSelf& m) {
    Json j = {{"rolled", m.rolled}, {"mana", m.mana}, {"mana_at", m.manaAt}, {"aware", m.aware}};
    if (!m.aspects.empty()) j["aspects"] = m.aspects;
    if (m.deathUses) j["death_uses"] = m.deathUses;
    if (m.raised) j["raised"] = m.raised;
    if (!m.book.empty()) {
        Json b = Json::array();
        for (const auto& e : m.book) b.push_back({{"spell", spellToJson(e.spell)}, {"mastery", e.mastery}, {"casts", e.casts}});
        j["book"] = b;
    }
    Json& rec = state(w)["people"][idKey(c.id)];
    if (!rec.is_object()) rec = Json::object();
    for (const char* k : {"aspects", "death_uses", "raised", "book"}) rec.erase(k); // written anew below when set
    for (auto& [k, v] : j.items()) rec[k] = v; // other keys of the record (healed_day, maimed_seen) stay
}

bool isGifted(const World& w, const Character& c) { return c.hasTrait(magicRules(w.content()).giftTrait); }
bool isUndead(const Character& c) { return c.hasTrait("undead"); }

double manaMax(const World& w, const Character& c) {
    const MagicRules& r = magicRules(w.content());
    auto sk = c.skills.find(r.skill);
    const double skill = sk == c.skills.end() ? 0.0 : sk->second;
    return r.manaBase + r.manaPerSkill * skill + (isGifted(w, c) ? r.manaGifted : 0.0);
}

namespace {
double manaAt(const World& w, const Character& c, const MagicSelf& m) {
    const MagicRules& r = magicRules(w.content());
    const double mx = manaMax(w, c);
    if (!m.rolled) return mx; // nobody has spent anything yet
    const double days = static_cast<double>(w.now() - m.manaAt) / static_cast<double>(kTicksPerDay);
    return std::min(mx, m.mana + std::max(0.0, days) * r.manaRegen * mx);
}

void spendMana(World& w, const Character& c, MagicSelf& m, double amount) {
    m.mana = std::max(0.0, manaAt(w, c, m) - amount);
    m.manaAt = w.now();
    m.rolled = true;
}

double talent(const World& w, const Character& c, const std::string& aspect) {
    const MagicRules& r = magicRules(w.content());
    double t = isGifted(w, c) ? r.giftLearn : 1.0;
    if (affinityFn()) t *= std::max(0.0, affinityFn()(w, c, aspect));
    return t;
}

double ceilingOf(const World& w, const Character& c) {
    const MagicRules& r = magicRules(w.content());
    return isGifted(w, c) ? r.ceilGifted : r.ceilPlain;
}

double skillOf(const World& w, const Character& c) {
    auto it = c.skills.find(magicRules(w.content()).skill);
    return it == c.skills.end() ? 0.0 : it->second;
}

std::vector<Character*> peopleNear(World& w, Vec2 at, double radius, EntityId except = kNone) {
    std::vector<Character*> out;
    for (auto& [id, o] : w.characters())
        if (o.body.alive && id != except && dist(o.pos, at) <= radius) out.push_back(&o);
    return out;
}

double bondOf(const Character& a, const Character& b) {
    double bond = 0.0;
    if (a.status.spouse == b.id) bond = 0.9;
    if (b.status.mother == a.id || b.status.father == a.id) bond = std::max(bond, 0.95);
    if (a.status.mother == b.id || a.status.father == b.id) bond = std::max(bond, 0.8);
    if (a.status.mother != kNone && a.status.mother == b.status.mother) bond = std::max(bond, 0.6);
    if (auto it = a.relations.find(b.id); it != a.relations.end()) bond = std::max(bond, it->second.affinity);
    return bond;
}

void witness(World& w, const Character& caster, const AspectDef* main, bool silent, double radius) {
    if (silent) return;
    const bool dark = main && (main->hasTag("dark") || main->hasTag("forbidden"));
    for (Character* o : peopleNear(w, caster.pos, radius, caster.id)) {
        if (!isPerson(*o) || isUndead(*o)) continue;
        // AL-118: what was seen (the aspect) goes with the memory: the law judges by it
        const bool forbidden = main && main->hasTag("forbidden");
        MemoryEntry seen{0, forbidden ? "saw_necromancy" : dark ? "saw_dark_magic" : "saw_magic", {caster.id}, kNone,
                         dark ? -0.4 : 0.1, 0.0, "seen", 1.0};
        if (main) seen.goods["aspect:" + main->id] = 1;
        w.remember(*o, std::move(seen));
        MagicSelf om = magicOf(w, *o);
        if (!om.aware) {
            om.aware = true;
            om.rolled = om.rolled || false;
            setMagicOf(w, *o, om);
        }
        if (dark) {
            addMoodlet(w, *o, "dread_dark_magic");
            o->relations[caster.id].fear = clamp01(o->relations[caster.id].fear + 0.15);
        }
    }
}

void markDeath(World& w, Character& c, MagicSelf& m) {
    const MagicRules& r = magicRules(w.content());
    ++m.deathUses;
    if (m.deathUses >= r.markAfter && !c.hasTrait(r.markTrait)) {
        addTrait(c, r.markTrait);
        w.chronicle("magic", {c.id}, "chronicle.magic.death_mark", {{"who", c.id}});
    }
}

// One body key on one creature. Returns true when something happened.
bool applyBodyKey(World& w, Character& t, const std::string& key, double v, Character* src, bool& raisedOut,
                  EntityId& raisedId) {
    const MagicRules& r = magicRules(w.content());
    const bool undead = isUndead(t);
    if (!t.body.alive && key != "raise" && key != "speak_dead") return false;
    if (key == "health") {
        if (undead) return false; // life and death work the other way on the undead: undead_harm/undead_heal
        t.body.health = clamp01(t.body.health + v);
        if (v > 0) addMoodlet(w, t, "healed_by_magic");
    } else if (key == "cure_illness") {
        if (undead || v < 0.5) return false;
        auto ill = std::find_if(t.moodlets.begin(), t.moodlets.end(),
                                [&](const Moodlet& m) { return m.id.rfind("ill_", 0) == 0 && m.until > w.now(); });
        if (ill == t.moodlets.end()) return false;
        t.moodlets.erase(ill);
    } else if (key == "heal_wounds") {
        if (undead) return false;
        for (auto& wd : t.wounds) {
            wd.daysLeft *= std::max(0.0, 1.0 - v);
            wd.bleeding = false;
        }
        if (v >= 1.0 && !t.injuries.empty()) t.injuries.pop_back(); // an exemplary healing takes back a maiming
    } else if (key == "burn" || key == "cold") {
        if (undead && key == "cold") return false;
        t.body.health = clamp01(t.body.health - 0.04 * v);
    } else if (key == "bleed") {
        if (undead) return false;
        t.body.blood = std::max(0.0, t.body.blood - 15.0 * v);
    } else if (key == "poison") {
        if (undead) return false;
        t.body.health = clamp01(t.body.health - 0.05 * v);
    } else if (key == "thirst") {
        if (undead) return false;
        t.needs["thirst"] = clamp01(t.need("thirst") - v);
    } else if (key == "fatigue") {
        if (undead) return false;
        t.body.fatigue = clamp01(t.body.fatigue + v);
    } else if (key == "sleep") {
        if (undead) return false;
        t.body.downUntil = std::max(t.body.downUntil, w.now() + static_cast<Tick>(6.0 * v));
    } else if (key == "wake") {
        t.body.downUntil = std::min(t.body.downUntil, w.now());
    } else if (key == "fear" || key == "confuse") {
        if (src) t.relations[src->id].fear = clamp01(t.relations[src->id].fear + v);
    } else if (key == "calm") {
        if (src) t.relations[src->id].fear = clamp01(t.relations[src->id].fear - v);
    } else if (key == "charm") {
        if (src) {
            auto& rel = t.relations[src->id];
            rel.affinity = std::clamp(rel.affinity + v, -1.0, 1.0);
        }
    } else if (key == "mana" || key == "dispel") {
        MagicSelf m = magicOf(w, t);
        const double now = manaAt(w, t, m);
        m.mana = std::clamp(now + (key == "dispel" ? -std::abs(v) * 0.25 : v), 0.0, manaMax(w, t));
        m.manaAt = w.now();
        m.rolled = true;
        setMagicOf(w, t, m);
    } else if (key == "undead_harm") {
        if (!undead) return false;
        t.body.health = clamp01(t.body.health - v);
        if (t.body.health <= 0.0) {
            t.body.alive = false;
            t.body.causeOfDeath = "undone";
            w.chronicle("magic", {t.id}, "chronicle.magic.undead_undone", {{"who", t.id}});
        }
    } else if (key == "undead_heal") {
        if (!undead) return false;
        t.body.health = clamp01(t.body.health + v);
    } else if (key == "lay_rest") {
        if (!undead) return false;
        t.body.alive = false;
        t.body.causeOfDeath = "laid_to_rest";
        w.chronicle("magic", {t.id}, "chronicle.magic.undead_undone", {{"who", t.id}});
    } else if (key == "raise") {
        if (t.body.alive || !src) return false;
        if (raiseUndead(w, *src, t, v)) {
            raisedOut = true;
            raisedId = t.id;
        }
    } else if (key == "speak_dead") {
        if (t.body.alive) return false;
        if (src) addMoodlet(w, *src, "spoke_with_dead");
    } else if (key == "push" || key == "pull" || key == "blink") {
        if (!src) return false;
        const double dx = t.pos.x - src->pos.x, dy = t.pos.y - src->pos.y, d = std::max(0.01, std::hypot(dx, dy));
        const double s = key == "pull" ? -std::min(v, d - 0.5) : v;
        if (key == "blink") return false; // the caster's own jump: handled by the caster's side
        t.pos = {t.pos.x + dx / d * s, t.pos.y + dy / d * s};
    } else if (key == "health_transfer") {
        if (!src || undead) return false;
        const double take = std::min(v, t.body.health);
        t.body.health = clamp01(t.body.health - take);
        src->body.health = clamp01(src->body.health + take);
    } else if (key == "death_touch") {
        // the caster's mark is counted by the cast
    } else if (key == "strength" || key == "breath" || key == "wet" || key == "fire_out" || key == "cure_poison") {
        // states the combat/fields layers read; nothing to keep in the body yet
    } else {
        for (auto& h : bodyHooks())
            if (h(w, t, key, v, src ? src->id : kNone)) return true;
        return false;
    }
    (void)r;
    return true;
}

// The body block of one aspect vector on one creature.
void applyBody(World& w, Character& t, const AspectDef& a, char vec, double power, Character* src, bool& raised,
               EntityId& raisedId) {
    const std::string v = vecKey(vec);
    if (!a.body.contains(v)) return;
    for (auto it = a.body[v].begin(); it != a.body[v].end(); ++it)
        applyBodyKey(w, t, it.key(), evalExpr(it.value(), power), src, raised, raisedId);
}

void applyWorld(World& w, const AspectDef& a, char vec, Vec2 at, double radius, double power) {
    if (!worldSink()) return;
    const std::string v = vecKey(vec);
    if (a.world.contains(v) && !a.world[v].empty()) worldSink()(w, at, radius, a.world[v], power);
}

struct Costing {
    std::string reason;
    double cost = 0.0, difficulty = 0.0, competence = 0.0;
    bool novice = false;
};

Costing costSpell(const World& w, const Character& c, const Spell& s, const MagicSelf& m, bool needKnowledge) {
    const MagicRules& r = magicRules(w.content());
    Costing k;
    if (!r.loaded) {
        k.reason = "no_magic";
        return k;
    }
    if (s.parts.empty() || s.parts.size() > 3) {
        k.reason = s.parts.empty() ? "unknown_aspect" : "too_many_aspects";
        return k;
    }
    const SpellFormDef* f = r.form(s.form);
    if (!f) {
        k.reason = "unknown_form";
        return k;
    }
    double cost = 0.0, diff = 0.0, know = 0.0, minKnow = 1.0;
    for (size_t i = 0; i < s.parts.size(); ++i) {
        const AspectDef* a = r.aspect(s.parts[i].aspect);
        if (!a) {
            k.reason = "unknown_aspect";
            return k;
        }
        if (!a->enabled) {
            k.reason = "disabled_aspect";
            return k;
        }
        cost += a->baseCost;
        diff += a->difficulty * (i == 0 ? 1.0 : r.extraAspectDiff);
        auto kn = m.aspects.find(a->id);
        const double kv = kn == m.aspects.end() ? 0.0 : kn->second;
        know += kv;
        minKnow = std::min(minKnow, kv);
        for (size_t j = 0; j < s.parts.size(); ++j)
            if (j != i && std::find(a->opposes.begin(), a->opposes.end(), s.parts[j].aspect) != a->opposes.end()) {
                cost *= std::sqrt(r.opposesCost);
                diff += r.opposesDiff * 0.5;
            }
    }
    double mult = f->costMult;
    diff += f->difficulty;
    for (const auto& [mid, steps] : s.mods) {
        auto md = r.modifiers.find(mid);
        if (md == r.modifiers.end() || steps <= 0) continue;
        const int st = std::min(steps, md->second.max);
        if (mid == "power" && st <= 1) continue; // power 1 is the default, free
        const int extra = mid == "power" ? st - 1 : st;
        mult += md->second.costPerStep * extra;
        diff += md->second.diffPerStep * extra;
    }
    k.cost = cost * mult * r.costScale;
    know /= static_cast<double>(s.parts.size());
    if (needKnowledge) {
        if (minKnow < r.useMin) {
            k.reason = "not_known";
            return k;
        }
        if (minKnow < r.solo) {
            k.novice = true;
            if (!r.noviceForms.count(s.form) || s.mod("power", 1) > 1) {
                k.reason = "novice_form";
                return k;
            }
            diff += r.novicePenalty;
        }
    }
    k.difficulty = diff;
    k.competence = skillOf(w, c) * 0.5 + know * 0.6 + (isGifted(w, c) ? r.giftCompetence : 0.0);
    return k;
}

const CastTier& tierOf(const MagicRules& r, double q) {
    for (const auto& t : r.tiers)
        if (q < t.below) return t;
    return r.tiers.back();
}

Character* targetOf(World& w, const CastTarget& t) { return t.who == kNone ? nullptr : w.character(t.who); }

// The shared body of a cast: `caster` may be null (a carrier fired by nobody).
CastOutcome resolve(World& w, Character* caster, const Spell& s, const CastTarget& t, double power, bool checkReach,
                    const std::string& tierId) {
    const MagicRules& r = magicRules(w.content());
    CastOutcome o;
    const SpellFormDef* f = r.form(s.form);
    Character* tc = targetOf(w, t);
    Vec2 origin = caster ? caster->pos : (tc ? tc->pos : t.at);
    Vec2 at = t.hasPos ? t.at : (tc ? tc->pos : origin);
    if (f->reach == "self" || f->reach == "rune") at = origin;
    if (f->reach == "touch" || f->reach == "ranged") {
        if (!tc && f->reach != "ranged") tc = caster;
        if (tc) at = tc->pos;
    }
    if (checkReach && caster && f->reach != "self") {
        const double reach = f->reach == "touch" ? f->rangeM : std::max(f->rangeM, 1.0);
        if (f->reach != "rune" && dist(caster->pos, at) > reach + 0.01 && !(f->reach == "area" && f->rangeM == 0.0)) {
            o.reason = "out_of_reach";
            return o;
        }
    }
    if (f->reach == "area" && f->rangeM == 0.0) at = origin; // a wave goes out from the caster
    const double radius = f->radiusM + s.mod("radius", 0);
    o.cast = true;
    o.tier = tierId;
    o.power = power;
    // who it touches
    std::vector<Character*> hit;
    if (f->reach == "self") hit.push_back(caster ? caster : tc);
    else if (f->reach == "area" || f->reach == "summon") {
        for (auto& [id, c] : w.characters())
            if (dist(c.pos, at) <= radius && (caster == nullptr || id != caster->id || f->rangeM > 0.0)) hit.push_back(&c);
        // a dead body under the spell still counts for raise/speak_dead
    } else if (tc) hit.push_back(tc);
    if (power > 0.0)
        for (const auto& p : s.parts) {
            const AspectDef* a = r.aspect(p.aspect);
            for (Character* h : hit)
                if (h) {
                    bool raised = false;
                    EntityId rid = kNone;
                    applyBody(w, *h, *a, p.vector, power, caster, raised, rid);
                    if (raised) o.raised = rid;
                    if (std::find(o.hit.begin(), o.hit.end(), h->id) == o.hit.end()) o.hit.push_back(h->id);
                }
            applyWorld(w, *a, p.vector, at, std::max(radius, 0.5), power);
        }
    return o;
}

} // namespace

double manaNow(const World& w, const Character& c) { return manaAt(w, c, magicOf(w, c)); }

double aspectKnowledge(const World& w, const Character& c, const std::string& aspect) {
    const MagicSelf m = magicOf(w, c);
    auto it = m.aspects.find(aspect);
    return it == m.aspects.end() ? 0.0 : it->second;
}

double learnAspect(World& w, Character& c, const std::string& aspect, double amount, const std::string& how) {
    const MagicRules& r = magicRules(w.content());
    const AspectDef* a = r.aspect(aspect);
    if (!a || !a->enabled || amount <= 0.0) return 0.0;
    MagicSelf m = magicOf(w, c);
    const double was = m.aspects.count(aspect) ? m.aspects[aspect] : 0.0;
    const double ceil = ceilingOf(w, c);
    if (was >= ceil) return 0.0;
    const double gain = std::min(ceil - was, amount * talent(w, c, aspect) * (1.0 - was * 0.5));
    m.aspects[aspect] = was + gain;
    m.aware = true;
    if (!m.rolled) {
        m.rolled = true;
        m.mana = manaMax(w, c);
        m.manaAt = w.now();
    }
    setMagicOf(w, c, m);
    if (was < r.useMin && was + gain >= r.useMin && how != "awaken")
        w.chronicle("magic", {c.id}, "chronicle.magic.learn", {{"who", c.id}, {"aspect", aspect}, {"how", how}});
    return gain;
}

bool teachAspect(World& w, Character& teacher, Character& pupil, const std::string& aspect) {
    const MagicRules& r = magicRules(w.content());
    const double tk = aspectKnowledge(w, teacher, aspect);
    if (tk < r.solo || aspectKnowledge(w, pupil, aspect) >= tk) return false;
    const double g = learnAspect(w, pupil, aspect, r.gainTeach * (0.5 + tk), "teach");
    if (g <= 0.0) return false;
    w.chronicle("magic", {teacher.id, pupil.id}, "chronicle.magic.teach",
                {{"a", teacher.id}, {"b", pupil.id}, {"aspect", aspect}});
    return true;
}

int saveSpell(World& w, Character& c, const Spell& s) {
    MagicSelf m = magicOf(w, c);
    const std::string k = s.key();
    for (size_t i = 0; i < m.book.size(); ++i)
        if (m.book[i].spell.key() == k) {
            if (!s.name.empty()) m.book[i].spell.name = s.name;
            setMagicOf(w, c, m);
            return static_cast<int>(i);
        }
    m.book.push_back({s, 0.0, 0});
    setMagicOf(w, c, m);
    return static_cast<int>(m.book.size()) - 1;
}

SpellPreview previewSpell(const World& w, const Character& caster, const Spell& s) {
    const MagicRules& r = magicRules(w.content());
    SpellPreview p;
    const MagicSelf m = magicOf(w, caster);
    const Costing k = costSpell(w, caster, s, m, true);
    p.manaNow = manaAt(w, caster, m);
    p.cost = k.cost;
    p.difficulty = k.difficulty;
    p.competence = k.competence;
    if (!k.reason.empty()) {
        p.reason = k.reason;
        return p;
    }
    // the share of each tier over the noise (uniform in [-noise, noise])
    const double mean = 0.5 + r.k * (k.competence - k.difficulty);
    const int n = 50;
    for (int i = 0; i < n; ++i) {
        const double q = mean + r.noise * (2.0 * (i + 0.5) / n - 1.0);
        p.tierChance[tierOf(r, q).id] += 1.0 / n;
    }
    p.ok = p.manaNow >= k.cost;
    if (!p.ok) p.reason = "no_mana";
    return p;
}

CastOutcome castSpell(World& w, Character& caster, const Spell& s, const CastTarget& t) {
    const MagicRules& r = magicRules(w.content());
    CastOutcome o;
    if (!caster.body.alive) {
        o.reason = "no_target";
        return o;
    }
    MagicSelf m = magicOf(w, caster);
    const Costing k = costSpell(w, caster, s, m, true);
    if (!k.reason.empty()) {
        o.reason = k.reason;
        return o;
    }
    const double mana = manaAt(w, caster, m);
    if (mana + 1e-9 < k.cost) {
        o.reason = "no_mana";
        return o;
    }
    const SpellFormDef* f = r.form(s.form);
    if ((f->reach == "touch" || f->reach == "ranged") && t.who == kNone && !t.hasPos && f->reach == "ranged") {
        o.reason = "no_target";
        return o;
    }
    Rng rng = w.rng(caster.id, "magic_cast:" + s.key());
    const double q = 0.5 + r.k * (k.competence - k.difficulty) + r.noise * (2.0 * rng.uniform() - 1.0);
    const CastTier& tier = tierOf(r, q);
    const double power = std::max(1, s.mod("power", 1)) * tier.power;
    CastOutcome res;
    if (tier.backfire) {
        // the spell turns on its maker: the same aspects, on the caster's own body
        CastTarget self;
        self.who = caster.id;
        Spell onSelf = s;
        onSelf.form = "aura";
        res = resolve(w, &caster, onSelf, self, power, false, tier.id);
        res.backfire = true;
        w.chronicle("magic", {caster.id}, "chronicle.magic.backfire", {{"who", caster.id}, {"spell", spellToJson(s)}});
    } else {
        res = resolve(w, &caster, s, t, power, true, tier.id);
        if (!res.cast) return res;
    }
    m = magicOf(w, caster); // the cast may have changed it (raising counts)
    spendMana(w, caster, m, k.cost);
    res.cost = k.cost;
    res.quality = q;
    // the aether of the place remembers the cast
    if (worldSink()) worldSink()(w, caster.pos, 2.0, Json{{"aether", "+" + std::to_string(r.aetherPerCost * k.cost) + "*power"}}, 1.0);
    // learning from a reasonably hard cast
    const double margin = k.competence - k.difficulty;
    const double hard = std::exp(-margin * margin / 0.08);
    caster.skills[r.skill] = clamp01(skillOf(w, caster) + r.skillGainCast * (0.4 + hard));
    bool death = false;
    for (const auto& p : s.parts) {
        const double was = m.aspects[p.aspect];
        const double ceil = ceilingOf(w, caster);
        if (was < ceil)
            m.aspects[p.aspect] = std::min(ceil, was + r.gainCast * talent(w, caster, p.aspect) * (0.4 + hard) * (1.0 - was));
        if (p.aspect == r.deathAspect) death = true;
    }
    if (death) markDeath(w, caster, m);
    // a breakthrough into a neighbouring aspect
    for (const auto& p : s.parts) {
        const AspectDef* a = r.aspect(p.aspect);
        if (!a || m.aspects[p.aspect] < r.breakthroughFrom) continue;
        for (const auto& fd : a->feeds) {
            const AspectDef* n = r.aspect(fd);
            if (!n || !n->enabled || m.aspects.count(fd)) continue;
            if (rng.chance(r.breakthrough * talent(w, caster, fd))) {
                m.aspects[fd] = std::min(ceilingOf(w, caster), r.gainAwaken * 0.5 * talent(w, caster, fd));
                res.breakthrough = true;
                w.chronicle("magic", {caster.id}, "chronicle.magic.breakthrough",
                            {{"who", caster.id}, {"from_aspect", p.aspect}, {"aspect", fd}});
            }
        }
    }
    // the book: a cast spell is kept, mastery grows
    {
        const std::string key = s.key();
        bool found = false;
        for (auto& b : m.book)
            if (b.spell.key() == key) {
                ++b.casts;
                b.mastery = clamp01(b.mastery + 0.05 * (0.5 + hard));
                found = true;
            }
        if (!found && caster.controller.isPlayer()) m.book.push_back({s, 0.05, 1});
    }
    m.aware = true;
    setMagicOf(w, caster, m);
    const AspectDef* main = r.aspect(s.parts.front().aspect);
    witness(w, caster, main, s.mod("silent", 0) > 0, r.witnessM);
    return res;
}

CastOutcome castStored(World& w, Character* holder, const Spell& s, const CastTarget& t, double power) {
    const MagicRules& r = magicRules(w.content());
    CastOutcome o;
    if (!r.loaded || s.parts.empty() || !r.form(s.form)) {
        o.reason = r.loaded ? "unknown_form" : "no_magic";
        return o;
    }
    for (const auto& p : s.parts)
        if (!r.aspect(p.aspect)) {
            o.reason = "unknown_aspect";
            return o;
        }
    o = resolve(w, holder, s, t, power, false, "stored");
    if (holder) witness(w, *holder, r.aspect(s.parts.front().aspect), s.mod("silent", 0) > 0, r.witnessM);
    return o;
}

void applyAspectsTo(World& w, Character& target, const std::vector<AspectDose>& doses, EntityId source) {
    const MagicRules& r = magicRules(w.content());
    Character* src = source == kNone ? nullptr : w.character(source);
    for (const auto& d : doses) {
        const AspectDef* a = r.aspect(d.aspect);
        if (!a || d.power <= 0.0) continue;
        bool raised = false;
        EntityId rid = kNone;
        applyBody(w, target, *a, d.vector, d.power, src ? src : &target, raised, rid);
    }
}

bool raiseUndead(World& w, Character& necro, Character& corpse, double power) {
    const MagicRules& r = magicRules(w.content());
    if (corpse.body.alive || power < r.raiseMinPower) return false;
    Json& s = state(w);
    const std::string key = idKey(corpse.id);
    const int seen = s["dead_seen"].contains(key) ? s["dead_seen"][key].get<int>() : today(w);
    const int deadDays = today(w) - seen;
    if (deadDays > r.maxDeadDays) {
        w.chronicle("magic", {necro.id, corpse.id}, "chronicle.magic.too_late", {{"a", necro.id}, {"b", corpse.id}});
        return false;
    }
    corpse.body.alive = true;
    corpse.body.health = clamp01(r.undeadHealth * std::min(1.5, power));
    corpse.body.blood = std::max(corpse.body.blood, 30.0);
    corpse.body.downUntil = 0;
    for (auto& wd : corpse.wounds) wd.bleeding = false;
    for (auto& [n, v] : corpse.needs) v = 1.0;
    addTrait(corpse, r.undeadTrait);
    std::string mind = "echo";
    if (power < r.wholeMindPower || deadDays > 30 || corpse.isAnimal()) {
        mind = "mindless";
        addTrait(corpse, r.mindlessTrait);
    } else if (power >= r.wholeMindPower && deadDays <= 3) {
        mind = "whole";
    }
    s["undead"][key] = {{"master", necro.id}, {"since", today(w)}, {"mind", mind}, {"orphan_days", 0}};
    auto& rel = corpse.relations[necro.id];
    rel.fear = clamp01(rel.fear + 0.3);
    rel.respect = clamp01(rel.respect + 0.3);
    MagicSelf m = magicOf(w, necro);
    ++m.raised;
    setMagicOf(w, necro, m);
    w.chronicle("magic", {necro.id, corpse.id}, "chronicle.magic.raise",
                {{"a", necro.id}, {"b", corpse.id}, {"mind", mind}});
    return true;
}

EntityId undeadMaster(const World& w, const Character& c) {
    const Json* s = stateC(w);
    if (!s || !isUndead(c) || !s->contains("undead")) return kNone;
    auto it = (*s)["undead"].find(idKey(c.id));
    if (it == (*s)["undead"].end()) return kNone;
    return it->value("master", kNone);
}

void setMagicWorldSink(MagicWorldSink sink) { worldSink() = std::move(sink); }
void setMagicFieldReader(MagicFieldReader reader) { fieldReader() = std::move(reader); }
double magicFieldAt(const World& w, const std::string& field, Vec2 at) {
    return fieldReader() ? fieldReader()(w, field, at) : 0.0;
}
void setMagicAffinity(MagicAffinity f) { affinityFn() = std::move(f); }
void setMagicGiftChance(MagicGiftChance f) { giftChanceFn() = std::move(f); }
void addMagicBodyHook(MagicBodyHook h) { bodyHooks().push_back(std::move(h)); }
void setMagicServiceHook(MagicServiceHook f) { serviceFn() = std::move(f); }

// --- the daily pass ------------------------------------------------------------------
namespace {

Spell simple(const std::string& aspect, char vec, const std::string& form, int power = 1) {
    Spell s;
    s.parts.push_back({aspect, vec});
    s.form = form;
    if (power > 1) s.mods["power"] = power;
    return s;
}

// A healer's cast at a distance of a day's errand: the healer goes to the patient.
bool healOnce(World& w, Character& healer, Character& patient) {
    const MagicRules& r = magicRules(w.content());
    Json& rec = state(w)["people"][idKey(patient.id)];
    if (rec.is_object() && rec.value("healed_day", -1) == today(w)) return false; // one healing a day is enough
    if (!rec.is_object()) rec = Json::object();
    rec["healed_day"] = today(w);
    const Vec2 was = healer.pos;
    healer.pos = patient.pos; // the round: the healer comes to the patient
    CastTarget t;
    t.who = patient.id;
    const CastOutcome o = castSpell(w, healer, simple(r.healAspect, '+', "touch"), t);
    healer.pos = was;
    if (o.cast && o.power > 0.0 && !o.backfire) {
        w.chronicle("magic", {healer.id, patient.id}, "chronicle.magic.heal", {{"a", healer.id}, {"b", patient.id}});
        if (serviceFn()) serviceFn()(w, healer, patient, "heal"); // AL-118: a healer's work is paid
        return true;
    }
    return false;
}

bool needsHealing(const World& w, const Character& c) {
    const MagicRules& r = magicRules(w.content());
    if (!c.body.alive || isUndead(c) || c.isAnimal()) return false;
    if (c.body.health < r.healBelow || !c.wounds.empty()) return true;
    for (const auto& m : c.moodlets)
        if (m.id.rfind("ill_", 0) == 0 && m.until > w.now()) return true;
    return false;
}

Character* findHealer(World& w, const Character& near, EntityId except = kNone) {
    const MagicRules& r = magicRules(w.content());
    Character* best = nullptr;
    double bestK = 0.0;
    for (auto& [id, c] : w.characters()) {
        if (!c.body.alive || c.isAnimal() || isUndead(c) || id == except) continue;
        if (c.status.settlement != near.status.settlement && near.status.settlement != kNone) continue;
        const double k = aspectKnowledge(w, c, r.healAspect);
        if (k >= r.healMinK && k > bestK && manaNow(w, c) > 0.1) {
            best = &c;
            bestK = k;
        }
    }
    return best;
}

void rollGift(World& w, Character& c, MagicSelf& m) {
    const MagicRules& r = magicRules(w.content());
    m.rolled = true;
    m.mana = manaMax(w, c);
    m.manaAt = w.now();
    if (c.isAnimal()) return;
    double chance = giftChanceFn() ? giftChanceFn()(w, c) : r.giftChance;
    Rng rng = w.rng(c.id, "magic_gift");
    if (!c.hasTrait(r.giftTrait) && rng.chance(chance)) addTrait(c, r.giftTrait);
    m.aware = c.hasTrait(r.giftTrait) || rng.chance(0.3);
}

// The aspect a gifted person wakes into, by what the person lives through.
std::string awakenAspect(World& w, const Character& c, Rng& rng) {
    const MagicRules& r = magicRules(w.content());
    if (c.hasMoodlet("grieving", w.now())) return rng.chance(0.5) ? "death" : "life";
    if (!c.wounds.empty() || !c.injuries.empty()) return rng.chance(0.5) ? "blood" : "life";
    std::vector<std::pair<std::string, double>> pool;
    double total = 0.0;
    for (const auto& [id, a] : r.aspects) {
        if (!a.enabled || a.hasTag("forbidden") || a.group == "force" || a.group == "special") continue;
        const double wgt = affinityFn() ? std::max(0.0, affinityFn()(w, c, id)) : 1.0;
        pool.emplace_back(id, wgt);
        total += wgt;
    }
    double x = rng.uniform() * total;
    for (const auto& [id, wgt] : pool) {
        x -= wgt;
        if (x <= 0.0) return id;
    }
    return pool.empty() ? std::string() : pool.back().first;
}

void undeadDaily(World& w) {
    const MagicRules& r = magicRules(w.content());
    Json& s = state(w);
    std::vector<std::string> gone;
    for (auto it = s["undead"].begin(); it != s["undead"].end(); ++it) {
        Character* u = w.character(static_cast<EntityId>(std::stoul(it.key())));
        if (!u || !u->body.alive || !isUndead(*u)) {
            gone.push_back(it.key());
            continue;
        }
        Json& rec = it.value();
        const EntityId mid = rec.value("master", kNone);
        Character* master = mid == kNone ? nullptr : w.character(mid);
        bool kept = false;
        if (master && master->body.alive) {
            MagicSelf m = magicOf(w, *master);
            const double mana = manaAt(w, *master, m);
            if (mana >= r.upkeepMana) {
                spendMana(w, *master, m, r.upkeepMana);
                setMagicOf(w, *master, m);
                kept = true;
            }
            rec["orphan_days"] = 0;
        } else if (mid != kNone) {
            const int od = rec.value("orphan_days", 0) + 1;
            rec["orphan_days"] = od;
            if (od >= r.unboundAfterDays) {
                rec["master"] = kNone;
                w.chronicle("magic", {u->id}, "chronicle.magic.unbound", {{"who", u->id}});
            }
        }
        // a dead body does not mend by itself: the ordinary rest and food regain is taken back
        if (rec.contains("hp")) u->body.health = std::min(u->body.health, rec["hp"].get<double>());
        if (!kept) {
            u->body.health = clamp01(u->body.health - r.undeadDecay);
            if (worldSink()) worldSink()(w, u->pos, 1.5, Json{{"taint", "+0.3*power"}}, 1.0);
        }
        rec["hp"] = u->body.health;
        if (u->body.health <= 0.0) {
            u->body.alive = false;
            u->body.causeOfDeath = "undone";
            w.chronicle("magic", {u->id}, "chronicle.magic.undead_undone", {{"who", u->id}});
            gone.push_back(it.key());
            continue;
        }
        // the living who see the walking dead
        for (Character* o : peopleNear(w, u->pos, r.undeadWitnessM, u->id)) {
            if (o->isAnimal() || isUndead(*o) || o->id == mid) continue;
            addMoodlet(w, *o, "dread_dark_magic");
            o->relations[u->id].fear = clamp01(o->relations[u->id].fear + r.undeadFear * 0.2);
            if (master) o->relations[master->id].fear = clamp01(o->relations[master->id].fear + 0.02);
        }
    }
    for (const auto& k : gone) s["undead"].erase(k);
}

// --- troubles and their paths ---------------------------------------------------------
double weightOf(const World& w, const Character& c, const CopingNext& n, double bond, const MagicSelf& m) {
    double x = n.base;
    for (const auto& [t, mult] : n.traits)
        if (c.hasTrait(t)) x *= mult;
    x *= m.aware ? n.knows : n.unknown;
    x *= std::max(0.0, 1.0 + n.wBond * bond);
    x *= std::max(0.0, 1.0 + n.wHonor * (c.standing.honor - 0.5));
    auto dk = m.aspects.find(magicRules(w.content()).deathAspect);
    x *= std::max(0.0, 1.0 + n.wDeath * (dk == m.aspects.end() ? 0.0 : dk->second));
    return std::max(0.0, x);
}

void copingChronicle(World& w, const std::string& trouble, const std::string& stage, EntityId a, EntityId b) {
    const MagicRules& r = magicRules(w.content());
    auto t = r.troubles.find(trouble);
    if (t == r.troubles.end()) return;
    auto st = t->second.stages.find(stage);
    if (st != t->second.stages.end() && st->second.quiet) return;
    w.chronicle("magic", {a, b}, "chronicle.magic.coping." + trouble + "." + stage, {{"a", a}, {"b", b}});
}

void startTrouble(World& w, Character& c, const std::string& trouble, EntityId about, double bond) {
    const MagicRules& r = magicRules(w.content());
    auto t = r.troubles.find(trouble);
    if (t == r.troubles.end()) return;
    Json& s = state(w);
    const std::string key = idKey(c.id);
    if (s["coping"].contains(key)) return; // one path at a time
    s["coping"][key] = {{"trouble", trouble}, {"about", about}, {"stage", t->second.start}, {"since", today(w)},
                        {"bond", bond}};
    copingChronicle(w, trouble, t->second.start, c.id, about);
}

Character* freshCorpseNear(World& w, const Character& c, EntityId except, double radius) {
    const MagicRules& r = magicRules(w.content());
    const Json& seen = state(w)["dead_seen"];
    Character* best = nullptr;
    for (auto& [id, o] : w.characters()) {
        if (o.body.alive || id == except || isUndead(o)) continue;
        if (o.isAnimal()) continue;
        if (dist(o.pos, c.pos) > radius && o.status.settlement != c.status.settlement) continue;
        const std::string k = idKey(id);
        const int day = seen.contains(k) ? seen[k].get<int>() : today(w);
        if (today(w) - day > r.maxDeadDays) continue;
        best = &o;
        break;
    }
    return best;
}

// What a person does on a stage of a path; returns false to stay on the stage.
bool doStage(World& w, Character& c, const std::string& trouble, const std::string& act, Character* about, Rng& rng) {
    const MagicRules& r = magicRules(w.content());
    MagicSelf m = magicOf(w, c);
    if (act == "grieve") {
        addMoodlet(w, c, "grieving");
        return true;
    }
    if (act == "ask_around") {
        if (trouble == "kin_death") addMoodlet(w, c, "obsessed_with_return");
        if (!m.aware && rng.chance(0.25)) {
            m.aware = true;
            setMagicOf(w, c, m);
        }
        return true;
    }
    if (act == "seek_healer") {
        Character* patient = trouble == "maimed" ? &c : about;
        if (!patient || !patient->body.alive) return true;
        Character* healer = findHealer(w, *patient, kNone);
        if (healer && healer->id != patient->id) healOnce(w, *healer, *patient);
        else if (healer == patient && healer) healOnce(w, *healer, *patient);
        return true;
    }
    if (act == "speak_dead") {
        // someone who knows Death~ well enough, or the person themselves
        Character* medium = nullptr;
        for (auto& [id, o] : w.characters())
            if (o.body.alive && !o.isAnimal() && aspectKnowledge(w, o, r.deathAspect) >= r.useMin &&
                (o.status.settlement == c.status.settlement || id == c.id)) {
                medium = &o;
                break;
            }
        if (medium) {
            addMoodlet(w, c, "spoke_with_dead");
            c.moodlets.erase(std::remove_if(c.moodlets.begin(), c.moodlets.end(),
                                            [](const Moodlet& x) { return x.id == "obsessed_with_return"; }),
                             c.moodlets.end());
            if (medium->id != c.id) {
                MagicSelf mm = magicOf(w, *medium);
                markDeath(w, *medium, mm);
                setMagicOf(w, *medium, mm);
            }
        }
        return true;
    }
    if (act == "study_forbidden") {
        addMoodlet(w, c, "obsessed_with_return");
        learnAspect(w, c, r.deathAspect, 0.012, "forbidden_study");
        return aspectKnowledge(w, c, r.deathAspect) >= r.useMin; // the stage ends only once the first spell can be held
    }
    if (act == "experiment") {
        learnAspect(w, c, r.deathAspect, 0.01, "experiment");
        if (Character* corpse = freshCorpseNear(w, c, about ? about->id : kNone, 60.0)) {
            const Vec2 was = c.pos;
            c.pos = corpse->pos; // a night at the graves
            CastTarget t;
            t.who = corpse->id;
            castSpell(w, c, simple(r.deathAspect, '~', "touch"), t);
            c.pos = was;
        } else {
            MagicSelf mm = magicOf(w, c);
            markDeath(w, c, mm);
            setMagicOf(w, c, mm);
        }
        return true;
    }
    if (act == "raise") {
        if (!about) return true;
        if (about->body.alive) return isUndead(*about); // already walks
        const Vec2 was = c.pos;
        c.pos = about->pos;
        CastTarget t;
        t.who = about->id;
        const int power = aspectKnowledge(w, c, r.deathAspect) >= r.solo ? 2 : 1;
        const CastOutcome o = castSpell(w, c, simple(r.deathAspect, '~', "touch", power), t);
        c.pos = was;
        if (o.raised == about->id) return true;
        // too far gone: the bones will not answer
        const std::string k = idKey(about->id);
        const Json& seen = state(w)["dead_seen"];
        if (seen.contains(k) && today(w) - seen[k].get<int>() > r.maxDeadDays) {
            Json& s = state(w)["coping"][idKey(c.id)];
            s["stage"] = "let_go";
            return true;
        }
        return false;
    }
    if (act == "keep") {
        if (about && about->body.alive && isUndead(*about)) {
            // the household lives with it
            for (auto& [id, o] : w.characters())
                if (o.body.alive && !o.isAnimal() && !isUndead(o) && id != c.id && o.status.family == c.status.family) {
                    addMoodlet(w, o, "dread_dark_magic");
                    o.relations[c.id].fear = clamp01(o.relations[c.id].fear + 0.01);
                }
        }
        return true;
    }
    if (act == "exposure") {
        for (auto& [id, o] : w.characters())
            if (o.body.alive && !o.isAnimal() && !isUndead(o) && id != c.id && o.status.settlement == c.status.settlement) {
                bool knows = false;
                for (const auto& mem : o.memory)
                    if (mem.type == "necromancy" && !mem.who.empty() && mem.who.front() == c.id) knows = true;
                if (!knows && rng.chance(0.35)) {
                    w.remember(o, MemoryEntry{0, "necromancy", {c.id}, kNone, -0.7, 0.0, "rumor", 0.7});
                    o.relations[c.id].fear = clamp01(o.relations[c.id].fear + 0.1);
                    o.relations[c.id].trust = std::max(-1.0, o.relations[c.id].trust - 0.15);
                }
            }
        return true;
    }
    if (act == "fall") {
        learnAspect(w, c, r.deathAspect, 0.01, "fall");
        if (rng.chance(0.15))
            if (Character* corpse = freshCorpseNear(w, c, kNone, 80.0)) {
                const Vec2 was = c.pos;
                c.pos = corpse->pos;
                CastTarget t;
                t.who = corpse->id;
                castSpell(w, c, simple(r.deathAspect, '~', "touch", 2), t);
                c.pos = was;
            }
        c.standing.honor = std::max(0.0, c.standing.honor - 0.005);
        return true;
    }
    if (act == "plague") {
        Json& s = state(w);
        for (auto it = s["undead"].begin(); it != s["undead"].end(); ++it)
            if (it.value().value("master", kNone) == c.id && rng.chance(0.3)) {
                it.value()["master"] = kNone;
                w.chronicle("magic", {static_cast<EntityId>(std::stoul(it.key()))}, "chronicle.magic.unbound",
                            {{"who", static_cast<EntityId>(std::stoul(it.key()))}});
            }
        return true;
    }
    if (act == "redeem") {
        Json& s = state(w);
        for (auto it = s["undead"].begin(); it != s["undead"].end(); ++it)
            if (it.value().value("master", kNone) == c.id)
                if (Character* u = w.character(static_cast<EntityId>(std::stoul(it.key())))) {
                    u->body.alive = false;
                    u->body.causeOfDeath = "laid_to_rest";
                }
        c.moodlets.erase(std::remove_if(c.moodlets.begin(), c.moodlets.end(),
                                        [](const Moodlet& x) { return x.id == "obsessed_with_return"; }),
                         c.moodlets.end());
        c.standing.honor = std::min(1.0, c.standing.honor + 0.05);
        return true;
    }
    if (act == "lich") {
        if (aspectKnowledge(w, c, r.deathAspect) >= r.lichDeathK && m.raised >= r.lichRaisedMin) {
            addTrait(c, "lich");
            w.chronicle("magic", {c.id}, "chronicle.magic.lich", {{"who", c.id}});
            return true;
        }
        Json& s = state(w)["coping"][idKey(c.id)];
        s["stage"] = "fall"; // not yet: back to the slow fall
        return false;
    }
    if (act == "let_go") return true;
    return true;
}

void copingDaily(World& w) {
    const MagicRules& r = magicRules(w.content());
    Json& s = state(w);
    // new deaths: the close ones of the dead start a path
    std::vector<Character*> fresh;
    for (auto& [id, c] : w.characters()) {
        if (c.body.alive || c.isAnimal()) continue;
        const std::string k = idKey(id);
        if (s["dead_seen"].contains(k)) continue;
        s["dead_seen"][k] = today(w);
        if (!isUndead(c)) fresh.push_back(&c);
    }
    if (r.troubles.count("kin_death"))
        for (Character* dead : fresh)
            for (auto& [id, c] : w.characters()) {
                if (!c.body.alive || c.isAnimal() || isUndead(c)) continue;
                const double bond = bondOf(c, *dead);
                if (bond >= r.troubles.at("kin_death").bondMin) startTrouble(w, c, "kin_death", dead->id, bond);
            }
    // maimings and a kin's illness
    for (auto& [id, c] : w.characters()) {
        if (!c.body.alive || c.isAnimal() || isUndead(c) || c.controller.isPlayer()) continue;
        if (!c.injuries.empty() && r.troubles.count("maimed")) {
            const std::string flag = "maimed_seen";
            const std::string k = idKey(id);
            const int n = static_cast<int>(c.injuries.size());
            if (s["people"].contains(k) ? s["people"][k].value(flag, 0) < n : true) {
                startTrouble(w, c, "maimed", id, 1.0);
                s["people"][k][flag] = n;
            }
        }
    }
    if (r.troubles.count("ill_kin"))
        for (auto& [id, sick] : w.characters()) {
            if (!sick.body.alive || sick.isAnimal() || !needsHealing(w, sick)) continue;
            bool ill = false;
            for (const auto& m : sick.moodlets) ill |= m.id.rfind("ill_", 0) == 0 && m.until > w.now();
            if (!ill) continue;
            bool sought = false; // one of the kin is enough to go for a healer
            for (auto it = s["coping"].begin(); it != s["coping"].end(); ++it)
                sought |= it.value().value("trouble", "") == "ill_kin" && it.value().value("about", kNone) == id;
            if (sought) continue;
            for (auto& [oid, c] : w.characters())
                if (oid != id && c.body.alive && !c.isAnimal() && !isUndead(c) && !c.controller.isPlayer()) {
                    const double bond = bondOf(c, sick);
                    if (bond >= r.troubles.at("ill_kin").bondMin && !s["coping"].contains(idKey(oid))) {
                        startTrouble(w, c, "ill_kin", id, bond);
                        break;
                    }
                }
        }
    // walk the paths
    std::vector<std::string> done;
    std::vector<std::string> keys;
    for (auto it = s["coping"].begin(); it != s["coping"].end(); ++it) keys.push_back(it.key());
    for (const auto& key : keys) {
        Json& p = s["coping"][key];
        Character* c = w.character(static_cast<EntityId>(std::stoul(key)));
        if (!c || !c->body.alive || c->controller.isPlayer()) {
            done.push_back(key);
            continue;
        }
        const std::string trouble = p.value("trouble", "");
        auto tt = r.troubles.find(trouble);
        if (tt == r.troubles.end()) {
            done.push_back(key);
            continue;
        }
        std::string stage = p.value("stage", "");
        auto st = tt->second.stages.find(stage);
        if (st == tt->second.stages.end()) {
            done.push_back(key);
            continue;
        }
        Character* about = w.character(p.value("about", kNone));
        Rng rng = w.rng(c->id, "magic_coping");
        const bool ready = doStage(w, *c, trouble, st->second.act, about, rng);
        // doStage may have moved the stage itself (raise too late, lich not yet)
        const std::string after = s["coping"][key].value("stage", "");
        if (after != stage) {
            s["coping"][key]["since"] = today(w);
            copingChronicle(w, trouble, after, c->id, p.value("about", kNone));
            continue;
        }
        if (st->second.next.empty()) {
            done.push_back(key);
            continue;
        }
        if (!ready || today(w) - p.value("since", 0) < st->second.minDays || !rng.chance(st->second.daily)) continue;
        const MagicSelf m = magicOf(w, *c);
        const double bond = p.value("bond", 0.5);
        double total = 0.0;
        std::vector<double> ws;
        for (const auto& n : st->second.next) {
            ws.push_back(weightOf(w, *c, n, bond, m));
            total += ws.back();
        }
        if (total <= 0.0) continue;
        double x = rng.uniform() * total;
        std::string to = st->second.next.back().to;
        for (size_t i = 0; i < ws.size(); ++i) {
            x -= ws[i];
            if (x <= 0.0) {
                to = st->second.next[i].to;
                break;
            }
        }
        p["stage"] = to;
        p["since"] = today(w);
        copingChronicle(w, trouble, to, c->id, p.value("about", kNone));
    }
    for (const auto& k : done) s["coping"].erase(k);
}

bool handleMagic(World& w, Command& cmd) {
    const std::string& t = cmd.type;
    if (t.rfind("magic_", 0) != 0) return false;
    Character* c = cmd.args.contains("id") ? w.character(cmd.args.at("id").get<EntityId>()) : nullptr;
    if (!c || !c->body.alive || c->isAnimal()) {
        cmd.result = "rejected:no_such_character";
        return true;
    }
    if (!magicRules(w.content()).loaded) {
        cmd.result = "rejected:no_magic";
        return true;
    }
    const Json& a = cmd.args;
    if (t == "magic_cast") {
        CastTarget tg;
        tg.who = a.value("target", kNone);
        if (a.contains("at")) {
            tg.at = a["at"].get<Vec2>();
            tg.hasPos = true;
        }
        const CastOutcome o = castSpell(w, *c, spellFromJson(a.value("spell", Json::object())), tg);
        cmd.result = o.cast ? "ok:" + o.tier : "rejected:" + o.reason;
    } else if (t == "magic_save") {
        Spell s = spellFromJson(a.value("spell", Json::object()));
        if (a.contains("name")) s.name = a["name"].get<std::string>();
        cmd.result = "ok:" + std::to_string(saveSpell(w, *c, s));
    } else if (t == "magic_teach") {
        Character* p = w.character(a.value("pupil", kNone));
        cmd.result = p && teachAspect(w, *c, *p, a.value("aspect", "")) ? "ok" : "rejected:cannot_teach";
    } else if (t == "magic_learn") {
        const double g = learnAspect(w, *c, a.value("aspect", ""), a.value("amount", 0.1), a.value("how", "command"));
        cmd.result = g > 0.0 ? "ok" : "rejected:cannot_learn";
    } else {
        cmd.result = "rejected:unknown_command";
    }
    return true;
}

} // namespace

void magicDaily(World& w) {
    const MagicRules& r = magicRules(w.content());
    if (!r.loaded) return;
    // the gift of new people, awakening, teaching within the household
    for (auto& [id, c] : w.characters()) {
        if (!c.body.alive || c.isAnimal() || isUndead(c)) continue;
        MagicSelf m = magicOf(w, c);
        if (!m.rolled) {
            rollGift(w, c, m);
            setMagicOf(w, c, m);
        }
        if (isGifted(w, c) && m.aspects.empty()) {
            Rng rng = w.rng(c.id, "magic_awaken");
            double ch = r.awakenDaily;
            if (c.hasMoodlet("grieving", w.now()) || !c.wounds.empty()) ch *= 5.0;
            if (rng.chance(ch)) {
                const std::string a = awakenAspect(w, c, rng);
                if (!a.empty()) {
                    learnAspect(w, c, a, r.gainAwaken, "awaken");
                    w.chronicle("magic", {c.id}, "chronicle.magic.awaken", {{"who", c.id}, {"aspect", a}});
                }
            }
        }
    }
    for (auto& [id, c] : w.characters()) {
        if (!c.body.alive || c.isAnimal() || isUndead(c) || c.controller.isPlayer()) continue;
        const MagicSelf m = magicOf(w, c);
        for (const auto& [aspect, k] : m.aspects) {
            if (k < r.solo) continue;
            const AspectDef* a = r.aspect(aspect);
            if (!a || a->hasTag("forbidden")) continue; // the dark arts are not taught at the hearth
            Rng rng = w.rng(c.id, "magic_teach:" + aspect);
            if (!rng.chance(0.05)) continue;
            Character* pupil = nullptr;
            for (auto& [oid, o] : w.characters())
                if (oid != id && o.body.alive && !o.isAnimal() && !isUndead(o) && o.status.family == c.status.family &&
                    o.body.stage() != "child" && aspectKnowledge(w, o, aspect) < k) {
                    if (!pupil || (isGifted(w, o) && !isGifted(w, *pupil))) pupil = &o;
                }
            if (pupil) teachAspect(w, c, *pupil, aspect);
        }
    }
    // healers' rounds: magic as a service
    for (auto& [id, c] : w.characters()) {
        if (!c.body.alive || c.isAnimal() || isUndead(c) || c.controller.isPlayer()) continue;
        if (aspectKnowledge(w, c, r.healAspect) < r.healMinK) continue;
        Rng rng = w.rng(c.id, "magic_heal");
        if (!rng.chance(r.healDaily)) continue;
        Character* worst = nullptr;
        for (auto& [oid, o] : w.characters())
            if (needsHealing(w, o) && o.status.settlement == c.status.settlement && (!worst || o.body.health < worst->body.health))
                worst = &o;
        if (worst && previewSpell(w, c, simple(r.healAspect, '+', "touch")).ok) healOnce(w, c, *worst);
    }
    undeadDaily(w);
    copingDaily(w);
}

void installMagic(World& w) {
    w.addCommandHandler(handleMagic);
    // AL-115's field store is the default world of a spell
    if (!worldSink())
        worldSink() = [](World& world, Vec2 at, double radiusM, const Json& channels, double power) {
            applyChannels(world, at, radiusM, channels, power);
        };
    // AL-114's kinds: talent by aspect, the gift's chance, the troll's sun-sleep
    if (!affinityFn())
        affinityFn() = [](const World& world, const Character& c, const std::string& aspect) {
            const auto& aff = raceOf(world.content(), c).affinity;
            auto it = aff.find(aspect);
            return it == aff.end() ? 1.0 : it->second;
        };
    if (!giftChanceFn())
        giftChanceFn() = [](const World& world, const Character& c) { return raceOf(world.content(), c).giftedChance; };
    static const bool trollHook = [] {
        addMagicBodyHook([](World& world, Character& t, const std::string& key, double amount, EntityId) {
            if (amount <= 0.0) return false;
            if ((key == "troll_sun" || key == "troll_wake") && !raceOf(world.content(), t).sunSleep.on)
                return true; // only the kinds that sleep in stone
            if (key == "troll_sun") forcePetrify(world, t);
            else if (key == "troll_wake") wakePetrified(world, t);
            else return false;
            return true;
        });
        return true;
    }();
    (void)trollHook;
    if (!fieldReader())
        fieldReader() = [](const World& world, const std::string& field, Vec2 at) {
            Field f;
            return fieldFromName(field, f) ? fieldAt(world, f, at) : 0.0;
        };
}

} // namespace lr::sim
