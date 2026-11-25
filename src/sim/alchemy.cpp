#include "sim/alchemy.hpp"

#include <algorithm>
#include <cmath>

#include "sim/ai.hpp"
#include "sim/magic.hpp"
#include "sim/rules.hpp"

namespace lr::sim {

namespace {

using CDose = craft::AspectDose;
using craft::Item;


struct PowerCtx : craft::ExprCtx {
    double power = 0.0;
    double var(const std::string& n) const override { return n == "power" ? power : 0.0; }
    double sym(const std::string&, const std::string&) const override { return 0.0; }
};

double evalField(const std::string& src, double power) {
    std::string s = src;
    while (!s.empty() && (s.front() == '+' || s.front() == ' ')) s.erase(s.begin());
    std::string err;
    craft::Expr e = craft::Expr::compile(s, &err);
    if (!err.empty()) return 0.0;
    PowerCtx ctx;
    ctx.power = power;
    return e.eval(ctx);
}

void giveMoodlet(World& w, Character& c, const std::string& id) {
    if (!w.content().moodlets.count(id)) return;
    try {
        applyEffectList(w, c, {parseEffect("moodlet:" + id)}, nullptr);
    } catch (...) {
    }
}

void hurtBy(World& w, Character& c, const std::vector<std::string>& effs) {
    std::vector<Effect> list;
    for (const auto& s : effs) {
        try {
            list.push_back(parseEffect(s));
        } catch (...) {
        }
    }
    if (!list.empty()) applyEffectList(w, c, list, nullptr);
}

void dropMade(Character& c, int index, const craft::CraftData& d) {
    const Item& it = c.craft.items[static_cast<size_t>(index)];
    if (const craft::FormDef* f = d.form(it.form); f && !f->item.empty()) {
        auto inv = c.inventory.find(f->item);
        if (inv != c.inventory.end() && inv->second > 0) --inv->second;
    }
    c.craft.items.erase(c.craft.items.begin() + index);
}

// the self vectors: what a rune of touch/word does to its holder
bool selfVector(const CDose& a) {
    static const char* good[] = {"life+", "essence+", "earth+", "metal+", "blood+", "sleep-", "mind+", "shadow+", "poison-"};
    const std::string k = a.aspect + a.vec;
    for (const char* g : good)
        if (k == g) return true;
    return false;
}

} // namespace

AspectUse applyAspectDoses(World& w, Character& c, const std::vector<CDose>& doses) {
    AspectUse u;
    u.ok = true;
    u.doses = doses;
    // the magic core (AL-116): the doses act as a spell on self, without mana or knowledge
    if (magicRules(w.content()).loaded) {
        std::vector<lr::sim::AspectDose> md;
        for (const auto& a : doses) md.push_back({a.aspect, a.vec, a.power});
        const double h0 = c.body.health, f0 = c.body.fatigue;
        lr::sim::applyAspectsTo(w, c, md, c.id);
        u.health = c.body.health - h0;
        u.fatigue = c.body.fatigue - f0;
        return u;
    }
    // without content/magic: the body fields the life-sim has
    const craft::CraftData& d = w.content().craft;
    for (const auto& a : doses) {
        const craft::AspectInfo* ai = d.aspect(a.aspect);
        if (!ai || !ai->enabled) continue;
        auto v = ai->body.find(std::string(1, a.vec));
        if (v == ai->body.end()) continue;
        for (const auto& [field, expr] : v->second) {
            const double x = evalField(expr, a.power);
            if (field == "health") u.health += x;
            else if (field == "fatigue") u.fatigue += x;
            else {
                u.fields.push_back(field + ":" + std::to_string(x));
                giveMoodlet(w, c, "potion_" + field);
            }
        }
    }
    if (u.health != 0.0) c.body.health = clamp01(c.body.health + u.health);
    if (u.fatigue != 0.0) c.body.fatigue = clamp01(c.body.fatigue + u.fatigue);
    if (c.body.health <= 0.0 && c.body.alive) hurtBy(w, c, {"death:potion"});
    return u;
}

bool isDrinkable(const Item& it) {
    if (it.shape != "liquid") return false;
    const auto h = it.derived.find("heal");
    return !craft::aspectDoses(it).empty() || (h != it.derived.end() && h->second > 0.0) || it.kind == "consumable";
}

AspectUse drinkPotion(World& w, Character& c, int index) {
    AspectUse u;
    if (index < 0 || index >= static_cast<int>(c.craft.items.size())) {
        u.reason = "no_such_item";
        return u;
    }
    const craft::CraftData& d = w.content().craft;
    Item it = c.craft.items[static_cast<size_t>(index)];
    std::vector<CDose> doses = craft::aspectDoses(it);
    const double heal = it.derived.count("heal") ? it.derived.at("heal") : 0.0;
    if (!isDrinkable(it)) {
        u.reason = "not_a_potion";
        return u;
    }
    // an unstable flask may burst in the hand instead
    const double un = craft::instability(d, it);
    if (un > d.magic.unstableSafe) {
        Rng r = w.rng(c.id, "alchemy:drink:" + std::to_string(w.now()));
        if (r.uniform() < clamp01((un - d.magic.unstableSafe) * d.magic.blastK)) {
            dropMade(c, index, d);
            hurtBy(w, c, d.magic.blastEffects);
            u.reason = "blast";
            u.blast = true;
            u.consumed = true;
            return u;
        }
    }
    dropMade(c, index, d);
    u = applyAspectDoses(w, c, doses);
    if (heal > 0.0) { // the herbal remedies of K6: a draught eases the body a little
        const double h = 0.1 * heal * (0.5 + it.q);
        c.body.health = clamp01(c.body.health + h);
        u.health += h;
    }
    u.consumed = true;
    giveMoodlet(w, c, "drank_potion");
    return u;
}

AspectUse useRune(World& w, Character& c, int index, bool onSelf) {
    AspectUse u;
    if (index < 0 || index >= static_cast<int>(c.craft.items.size())) {
        u.reason = "no_such_item";
        return u;
    }
    const craft::CraftData& d = w.content().craft;
    Item& it = c.craft.items[static_cast<size_t>(index)];
    if (!it.rune.bound()) {
        u.reason = "not_enchanted";
        return u;
    }
    const bool scroll = it.tags.count("scroll") || it.form == "scroll";
    std::vector<CDose> doses = scroll ? craft::runeDoses(d, it) : craft::spendRune(d, it);
    if (doses.empty()) {
        u.reason = "no_charge";
        return u;
    }
    if (scroll) {
        dropMade(c, index, d);
        u.consumed = true;
    } else {
        craft::recognize(d, it);
    }
    if (!onSelf) {
        u.ok = true;
        u.doses = doses;
        return u;
    }
    {
        std::vector<CDose> self;
        for (const auto& a : doses)
            if (selfVector(a)) self.push_back(a);
        AspectUse a = applyAspectDoses(w, c, self);
        u.health = a.health;
        u.fatigue = a.fatigue;
        u.fields = a.fields;
    }
    u.ok = true;
    u.doses = doses;
    return u;
}

void rechargeRunes(Character& c, double share) {
    for (auto& it : c.craft.items)
        if (it.rune.bound()) craft::rechargeRune(it, share * it.rune.capacity);
}

void noteWeaponDeed(World& w, Character& c, const std::string& what) {
    const craft::CraftData& d = w.content().craft;
    Item* best = nullptr;
    double bestDmg = 0.0;
    for (auto& it : c.craft.items) { // the same pick as wearMadeWeapon (combat_bridge)
        if (it.kind != "weapon" || it.wear >= 1.0) continue;
        auto dm = it.derived.find("damage_mult");
        double dmg = dm == it.derived.end() ? 1.0 : dm->second;
        if (!best || dmg > bestDmg) {
            best = &it;
            bestDmg = dmg;
        }
    }
    if (!best) return;
    craft::noteDeed(d, *best, what);
    craft::recognize(d, *best);
}

static Spell runeSpell(const std::vector<CDose>& doses, const std::string& form) {
    Spell s;
    s.form = form;
    for (const auto& a : doses) {
        if (s.parts.size() >= 3) break;
        s.parts.push_back({a.aspect, a.vec});
    }
    return s;
}

AspectUse fireRune(World& w, Character& holder, int index, const CastTarget& at) {
    AspectUse u = useRune(w, holder, index, false);
    if (!u.ok || !magicRules(w.content()).loaded) return u;
    double power = 0.0;
    for (const auto& a : u.doses) power += a.power;
    const std::string form = at.who != kNone ? "touch" : (at.hasPos ? "area" : "aura");
    CastTarget t = at;
    if (t.who == kNone && !t.hasPos) t.who = holder.id;
    lr::sim::castStored(w, &holder, runeSpell(u.doses, form), t, power);
    return u;
}

void fireHitRunes(World& w, Character& holder, Character& foe) {
    if (!holder.body.alive) return;
    int best = -1;
    double bestDmg = 0.0;
    for (size_t i = 0; i < holder.craft.items.size(); ++i) { // the same pick as wearMadeWeapon
        const Item& it = holder.craft.items[i];
        if (it.kind != "weapon" || it.wear >= 1.0) continue;
        auto dm = it.derived.find("damage_mult");
        double dmg = dm == it.derived.end() ? 1.0 : dm->second;
        if (best < 0 || dmg > bestDmg) {
            best = static_cast<int>(i);
            bestDmg = dmg;
        }
    }
    if (best < 0) return;
    const Item& it = holder.craft.items[static_cast<size_t>(best)];
    if (!it.rune.bound() || (it.rune.trigger != "on_hit" && it.rune.trigger != "constant")) return;
    CastTarget t;
    t.who = foe.id;
    fireRune(w, holder, best, t);
}

namespace {

// bond+ on a person feeds the runes they carry, bond- drains them (aspects.json body bind/unbind)
void installBondHook() {
    static bool done = false;
    if (done) return;
    done = true;
    addMagicBodyHook([](World&, Character& target, const std::string& key, double amount, EntityId) {
        if (key == "bind") {
            rechargeRunes(target, 0.25 * std::max(0.0, amount));
            return true;
        }
        if (key == "unbind") {
            for (auto& it : target.craft.items)
                if (it.rune.bound()) it.rune.charge = std::max(0.0, it.rune.charge - 0.5 * std::max(0.0, amount) * it.rune.capacity);
            return true;
        }
        return false;
    });
}

bool handleAlchemy(World& w, Command& cmd) {
    const std::string& t = cmd.type;
    if (t != "alchemy_drink" && t != "alchemy_use_rune") return false;
    Character* c = cmd.args.contains("id") ? w.character(cmd.args.at("id").get<EntityId>()) : nullptr;
    if (!c || !c->body.alive || c->isAnimal()) {
        cmd.result = "rejected:no_such_character";
        return true;
    }
    const int index = cmd.args.value("item", -1);
    AspectUse u;
    if (t == "alchemy_drink") u = drinkPotion(w, *c, index);
    else if (cmd.args.contains("target") || cmd.args.contains("at")) { // at a creature or a place: through the magic core
        CastTarget at;
        if (cmd.args.contains("target")) at.who = cmd.args.at("target").get<EntityId>();
        if (cmd.args.contains("at") && cmd.args["at"].is_array() && cmd.args["at"].size() == 2) {
            at.hasPos = true;
            at.at = Vec2{cmd.args["at"][0].get<double>(), cmd.args["at"][1].get<double>()};
        }
        u = fireRune(w, *c, index, at);
    } else u = useRune(w, *c, index, cmd.args.value("self", true));
    if (!u.ok) {
        cmd.result = "rejected:" + u.reason;
        return true;
    }
    std::string r = "ok";
    for (const auto& a : u.doses) r += ":" + a.aspect + a.vec;
    cmd.result = r;
    return true;
}

} // namespace

void installAlchemy(World& w) {
    w.addCommandHandler(handleAlchemy);
    installBondHook();
}

} // namespace lr::sim
