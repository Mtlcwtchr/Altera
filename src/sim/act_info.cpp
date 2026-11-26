#include "sim/act_info.hpp"

#include <algorithm>

#include "sim/ai.hpp"

namespace lr::sim {

bool shownCondition(const Condition& k) {
    // the owner rule and the household's own bookkeeping are not a player's worry
    return k.subject != "target_owner" && k.subject != "charity" && k.subject != "time" && k.subject != "herd";
}

ActInfo actInfo(const World& w, const Character& c, const InteractionDef& base, const WorldObject* obj,
                const Character* targetChar) {
    ActInfo a;
    a.id = base.id; // the order names the act; the sim does it as the foreign variant
    const InteractionDef* f =
        foreignVariant(w, c, base, obj ? obj->id : (targetChar && targetChar->isAnimal() ? targetChar->id : kNone));
    const InteractionDef& d = f ? *f : base;
    if (f) a.legal = f->legal;
    // LR-127: what the act is in law here (lynching without a sentence, tyranny without the right...)
    for (const LegalTag& t : legalTags(w, c, d, obj, targetChar))
        if (std::find(a.legal.begin(), a.legal.end(), t.tag) == a.legal.end()) a.legal.push_back(t.tag);
    a.duration = d.duration;
    a.can = canPerform(w, c, d, obj, &a.why, false, targetChar);
    // LR-130: the menu is the player's order: it says "can't carry" as the order would
    if (a.can && c.controller.isPlayer())
        if (std::string g = gearRefusal(w, c, d); !g.empty()) a.can = false, a.why = g;
    const EvalCtx x{w, c, obj, targetChar};
    for (const Condition& k : d.pre) {
        if (k.isParam() || !shownCondition(k)) continue;
        if (k.onTarget && !targetChar) continue;
        ActReq r;
        r.cond = k;
        r.ok = evalCondition(x, k);
        a.pre.push_back(std::move(r));
    }
    for (const Effect& e : d.eff) {
        if (e.isParam() || e.onWitnesses || e.subject == "target_stock") continue;
        if (e.onTarget) a.onTarget.push_back(e);
        else a.gains.push_back(e);
    }
    return a;
}

std::vector<std::string> actsOnObject(const Content& content, const std::string& objectType) {
    std::vector<std::string> out;
    for (const auto& [id, d] : content.interactions)
        if (d.targetsObject() && d.targetType == objectType && !d.parametric() && d.baseId.empty()) out.push_back(id);
    return out;
}

} // namespace lr::sim
