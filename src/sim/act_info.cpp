#include "sim/act_info.hpp"

#include "sim/ai.hpp"

namespace altera::sim {

bool shownCondition(const Condition& k) {
    // the owner rule and the household's own bookkeeping are not a player's worry
    return k.subject != "target_owner" && k.subject != "charity" && k.subject != "time" && k.subject != "herd";
}

ActInfo actInfo(const World& w, const Character& c, const InteractionDef& d, const WorldObject* obj,
                const Character* targetChar) {
    ActInfo a;
    a.id = d.id;
    a.duration = d.duration;
    a.can = canPerform(w, c, d, obj, &a.why, false, targetChar);
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
        if (d.targetsObject() && d.targetType == objectType && !d.parametric()) out.push_back(id);
    return out;
}

} // namespace altera::sim
