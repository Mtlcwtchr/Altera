#include "sim/tones.hpp"
#include "sim/data_fs.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>

#include "sim/ai.hpp"
#include "sim/character.hpp"
#include "sim/content.hpp"
#include "sim/group_combat.hpp"
#include "sim/social_chain.hpp"
#include "sim/world.hpp"

namespace lr::sim {

namespace fs = std::filesystem;

const ToneRules* toneRules(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<ToneRules>> cache;
    if (c.dir.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(data::cacheKey(c.dir));
    if (it != cache.end()) return it->second.get();
    std::unique_ptr<ToneRules> r;
    const fs::path p = fs::path(c.dir) / "tones.json";
    if (data::exists(p)) {
        r = std::make_unique<ToneRules>();
        data::InFile f(p, std::ios::binary);
        const Json j = Json::parse(f);
        r->order = j.value("order", std::vector<std::string>{});
        const Json rc = j.value("reception", Json::object());
        const Json base = rc.value("base", Json::object()); // a local: items() of a temporary dangles
        for (auto& [k, v] : base.items())
            if (v.is_number()) r->receptionBase[k] = v.get<double>();
        r->receptionRel = rc.value("rel", std::map<std::string, double>{});
        r->kinAdd = rc.value("kin_add", r->kinAdd);
        r->moodMult = rc.value("mood_mult", r->moodMult);
        r->recMin = rc.value("min", r->recMin);
        r->recMax = rc.value("max", r->recMax);
        r->skipMeanings = rc.value("skip_meanings", std::vector<std::string>{});
        for (const auto& e : rc.value("on_fail", std::vector<std::string>{})) r->receptionOnFail.push_back(parseEffect(e));
        const Json tones = j.value("tones", Json::object());
        for (auto it2 = tones.begin(); it2 != tones.end(); ++it2) {
            ToneDef t;
            const Json& tj = it2.value();
            t.chance = tj.value("chance", std::map<std::string, double>{});
            t.rel = tj.value("rel", std::map<std::string, double>{});
            for (const auto& e : tj.value("eff", std::vector<std::string>{})) t.eff.push_back(parseEffect(e));
            for (const auto& e : tj.value("on_fail", std::vector<std::string>{})) t.onFail.push_back(parseEffect(e));
            r->tones[it2.key()] = std::move(t);
        }
        for (const auto& o : r->order)
            if (!r->tones.count(o)) throw std::runtime_error("tones.json: order names unknown tone '" + o + "'");
    }
    return (cache[data::cacheKey(c.dir)] = std::move(r)).get();
}

std::string toneOf(const World& w, const Character& actor, const Character* target) {
    if (!target || target == &actor || target->isAnimal() || !actor.controller.isPlayer()) return {};
    const ToneRules* R = toneRules(w.content());
    if (!R || !R->tones.count(actor.controller.tone)) return {};
    return actor.controller.tone;
}

namespace {

std::string meaningOf(const InteractionDef& d) {
    return d.social.is_object() ? d.social.value("meaning", std::string()) : std::string();
}

double relOf(const Character& from, const Character& to, const std::string& axis) {
    auto it = from.relations.find(to.id);
    return it == from.relations.end() ? 0.0 : it->second.get(axis);
}

} // namespace

double toneShift(const ToneRules& R, const std::string& tone, const InteractionDef& d, const Character& actor,
                 const Character& target) {
    auto t = R.tones.find(tone);
    if (t == R.tones.end()) return 0.0;
    const std::string m = meaningOf(d);
    double s = 0.0;
    auto c = t->second.chance.find(m);
    if (c == t->second.chance.end()) c = t->second.chance.find("_");
    if (c != t->second.chance.end()) s += c->second;
    for (const auto& [axis, k] : t->second.rel) s += k * relOf(target, actor, axis);
    return s;
}

bool receptionRolls(const ToneRules& R, const InteractionDef& d) {
    if (d.chance.set || d.consent == "target" || d.combat.set || !d.targetsCharacter()) return false;
    if (!R.receptionBase.count(d.category)) return false;
    return std::find(R.skipMeanings.begin(), R.skipMeanings.end(), meaningOf(d)) == R.skipMeanings.end();
}

double receptionChance(const World& w, const ToneRules& R, const Character& actor, const InteractionDef& d,
                       const Character& target, const std::string& tone) {
    double p = R.receptionBase.at(d.category);
    for (const auto& [axis, k] : R.receptionRel) p += k * relOf(target, actor, axis);
    if ((actor.status.family != kNone && actor.status.family == target.status.family) ||
        (actor.status.dynasty != kNone && actor.status.dynasty == target.status.dynasty))
        p += R.kinAdd;
    p += R.moodMult * target.mood(w.now());
    p += toneShift(R, tone, d, actor, target);
    return std::clamp(p, R.recMin, R.recMax);
}

ActChance actChance(World& w, Character& actor, const InteractionDef& d, Character* target, const std::string& tone) {
    ActChance a;
    const ToneRules* R = toneRules(w.content());
    if (d.combat.set && target) {
        a.kind = "win";
        a.p = winChance(w, actor, d, *target);
        return a;
    }
    // AL-40: an act with a social chain - the other's answer.
    if (target && !target->isAnimal())
        if (const SocialChainDef* sc = socialChain(w.content(), d.id); sc && !sc->byDefault) { // AL-73: a talk heard as before
            a.kind = "consent";
            a.p = socialBlock(w, actor, *target, *sc).empty() ? socialAcceptChance(w, actor, *target, d, *sc, tone) : 0.0;
            return a;
        }
    if (d.chance.set) {
        a.kind = "chance";
        a.p = chanceOf(w, actor, d, target);
        if (R && target && !tone.empty()) a.p = std::clamp(a.p + toneShift(*R, tone, d, actor, *target), d.chance.min, d.chance.max);
        return a;
    }
    if (target && d.consent == "target") {
        a.kind = "consent";
        a.p = consentChance(w, actor, *target, d, tone);
        return a;
    }
    if (R && target && !tone.empty() && receptionRolls(*R, d)) {
        a.kind = "reception";
        a.p = receptionChance(w, *R, actor, d, *target, tone);
    }
    return a;
}

} // namespace lr::sim
