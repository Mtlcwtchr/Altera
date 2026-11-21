// Rumour noise (AL-50): operators over the meaning of a retold rumour.

#include "sim/rumor_noise.hpp"

#include <algorithm>
#include <cmath>

namespace altera::sim {

namespace {

const std::vector<std::string> kOrder = {"attribute_shift", "drop_detail", "vague_theme", "exaggerate",
                                         "minimize",        "swap_agent",  "swap_victim", "invert"};

// Roles in order of how much they matter to the story (drift weights).
const std::map<std::string, double> kRoleWeight = {
    {"doer", 3.0}, {"victim", 2.0}, {"victim_family", 1.5}, {"object", 1.0}, {"beneficiary", 2.0},
    {"speaker", 1.0}, {"thinker", 1.0}, {"content", 3.0}};

double chanceOf(const NoiseParams::Op& op, const Epistemic& epi, double conf, const NoiseContext& ctx) {
    if (conf >= op.belowConf) return 0.0;
    if (epi.hops() < op.minHops) return 0.0;
    double c = op.base * (1.0 + op.perHop * epi.hops());
    for (const auto& t : ctx.tellerTraits) {
        auto it = op.trait.find(t);
        if (it != op.trait.end()) c *= it->second;
    }
    return std::clamp(c, 0.0, 1.0);
}

EntityId pickWeighted(const std::vector<std::pair<EntityId, double>>& cand, EntityId exclude, Rng& rng) {
    double total = 0.0;
    for (const auto& [id, w] : cand)
        if (id != exclude && w > 0.0) total += w;
    if (total <= 0.0) return kNone;
    double r = rng.uniform() * total;
    for (const auto& [id, w] : cand) {
        if (id == exclude || w <= 0.0) continue;
        r -= w;
        if (r < 0.0) return id;
    }
    for (auto it = cand.rbegin(); it != cand.rend(); ++it)
        if (it->first != exclude && it->second > 0.0) return it->first;
    return kNone;
}

// Move pred along its scale by `step` (+1 stronger, -1 weaker); false if it cannot.
bool shiftScale(const DialogueGenData& d, Prop& p, int step) {
    const PredicateDef* def = d.predicate(p.pred);
    if (!def || def->scale.empty()) return false;
    auto it = std::find(def->scale.begin(), def->scale.end(), p.pred);
    if (it == def->scale.end()) return false;
    const long i = static_cast<long>(it - def->scale.begin()) + step;
    if (i < 0 || i >= static_cast<long>(def->scale.size())) return false;
    const std::string to = def->scale[static_cast<size_t>(i)];
    const PredicateDef* target = d.predicate(to);
    if (!target) return false;
    // the roles must still fit the stronger/weaker predicate
    for (const auto& [role, ref] : p.roles) {
        auto r = target->roles.find(role);
        if (r == target->roles.end() || std::find(r->second.begin(), r->second.end(), ref.kind) == r->second.end())
            return false;
    }
    for (const auto& req : target->required)
        if (!p.roles.count(req)) return false;
    p.pred = to;
    return true;
}

Ref textRef(const std::string& key) {
    Ref r;
    r.kind = "text";
    r.key = key;
    return r;
}

bool roleAllows(const DialogueGenData& d, const std::string& pred, const std::string& role, const std::string& kind) {
    const PredicateDef* def = d.predicate(pred);
    if (!def) return true; // unknown predicate: nothing to check against
    auto it = def->roles.find(role);
    return it != def->roles.end() && std::find(it->second.begin(), it->second.end(), kind) != it->second.end();
}

bool apply(const std::string& id, const DialogueGenData& d, const NoiseParams::Op& op, Prop& p, Epistemic& out,
           const Epistemic& in, const NoiseContext& ctx, Rng& rng) {
    if (id == "attribute_shift") {
        // the far end of the chain is forgotten: "Brana saw it" -> "someone saw it"
        if (in.chain.empty()) return false;
        out.chain.pop_back();
        return true;
    }
    if (id == "drop_detail") {
        std::vector<std::string> can;
        for (const auto& r : op.roles) {
            if (r == "tick" && p.tick >= 0) can.push_back(r);
            else if (r == "qty" && p.qty != 0.0) can.push_back(r);
            else if (r != "tick" && r != "qty" && p.roles.count(r)) {
                const PredicateDef* def = d.predicate(p.pred);
                const bool required = def && std::find(def->required.begin(), def->required.end(), r) != def->required.end();
                if (!required) can.push_back(r);
            }
        }
        if (can.empty()) return false;
        const std::string r = can[static_cast<size_t>(rng.irange(0, static_cast<int>(can.size()) - 1))];
        if (r == "tick") p.tick = -1;
        else if (r == "qty") {
            p.qty = 0.0;
            p.unit.clear();
        } else p.roles.erase(r);
        return true;
    }
    if (id == "vague_theme") {
        auto it = p.roles.find("object");
        if (it == p.roles.end()) return false;
        Ref& o = it->second;
        Ref to;
        if (o.kind == "item") {
            auto c = ctx.itemCategory.find(o.key);
            to = textRef(c != ctx.itemCategory.end() ? "category." + c->second : "thing.something");
        } else if (o.kind == "object" || (o.kind == "text" && o.key.rfind("category.", 0) == 0)) {
            to = textRef("thing.something");
        } else {
            return false; // already "something", or a beast
        }
        if (!roleAllows(d, p.pred, "object", "text")) return false;
        o = to;
        p.qty = 0.0;
        p.unit.clear();
        return true;
    }
    if (id == "exaggerate" || id == "minimize") {
        const int step = id == "exaggerate" ? 1 : -1;
        if (id == "minimize" && ctx.tellerAffinityToDoer <= op.relOver) return false;
        bool changed = shiftScale(d, p, step);
        if (p.qty > 0.0) {
            p.qty = step > 0 ? std::ceil(p.qty * 2.0) : std::max(1.0, std::floor(p.qty / 2.0));
            changed = true;
        }
        return changed;
    }
    if (id == "swap_agent" || id == "swap_victim") {
        const std::string role = id == "swap_agent" ? "doer" : "victim";
        auto it = p.roles.find(role);
        if (it == p.roles.end() || it->second.kind != "char") return false;
        const auto& cand = id == "swap_agent" ? ctx.agentCandidates : ctx.victimCandidates;
        const EntityId to = pickWeighted(cand, it->second.id, rng);
        if (to == kNone) return false;
        // never makes the teller or listener the one they talk about by accident
        if (to == ctx.teller || to == ctx.listener) return false;
        it->second.id = to;
        return true;
    }
    if (id == "invert") {
        auto a = p.roles.find("doer");
        auto b = p.roles.find("victim");
        if (a == p.roles.end() || b == p.roles.end() || a->second.kind != "char" || b->second.kind != "char")
            return false;
        std::swap(a->second, b->second);
        return true;
    }
    return false;
}

} // namespace

NoiseParams NoiseParams::fromJson(const Json& j, double decay) {
    NoiseParams p;
    p.decay = decay;
    if (!j.is_object()) return p;
    const Json ops = j.value("operators", Json::object());
    for (auto it = ops.begin(); it != ops.end(); ++it) {
        const Json& o = it.value();
        if (!o.is_object()) continue;
        Op op;
        op.base = o.value("base", 0.0);
        op.perHop = o.value("per_hop", 0.0);
        op.belowConf = o.value("below_conf", 1.0);
        op.minHops = o.value("min_hops", 0);
        op.confDelta = o.value("conf_delta", 0.0);
        op.relOver = o.value("rel_affinity_doer_over", 2.0);
        if (o.contains("trait") && o["trait"].is_object())
            for (auto t = o["trait"].begin(); t != o["trait"].end(); ++t)
                if (t->is_number()) op.trait[t.key()] = t->get<double>();
        if (o.contains("roles") && o["roles"].is_array())
            for (const auto& r : o["roles"])
                if (r.is_string()) op.roles.push_back(r.get<std::string>());
        p.ops[it.key()] = op;
    }
    const Json own = j.value("check_own_memory", Json::object());
    p.discardIfSeen = own.value("discard_if_seen", p.discardIfSeen);
    p.trustPenalty = own.value("trust_penalty", p.trustPenalty);
    p.surface = j.value("surface", Json::object()).value("enabled", p.surface);
    return p;
}

const NoiseParams::Op* NoiseParams::op(const std::string& id) const {
    auto it = ops.find(id);
    return it == ops.end() ? nullptr : &it->second;
}

NoiseResult retell(const DialogueGenData& d, const NoiseParams& p, const Prop& heard, const Epistemic& epi,
                   const NoiseContext& ctx, Rng& rng) {
    NoiseResult r;
    r.prop = heard;
    r.epi = epi;
    r.epi.source = "rumor";
    r.epi.confidence = std::clamp(epi.confidence * p.decay, 0.0, 1.0);
    for (const auto& id : kOrder) {
        const NoiseParams::Op* op = p.op(id);
        if (!op) continue;
        const double c = chanceOf(*op, epi, r.epi.confidence, ctx);
        // one roll per operator, always drawn, so a change of one parameter
        // does not reshuffle what the others do
        const double roll = rng.uniform();
        if (c <= 0.0 || roll >= c) continue;
        if (!apply(id, d, *op, r.prop, r.epi, epi, ctx, rng)) continue;
        r.epi.confidence = std::clamp(r.epi.confidence + op->confDelta, 0.0, 1.0);
        r.applied.push_back(id);
    }
    // the teller becomes the nearest link
    if (ctx.teller != kNone) {
        r.epi.chain.insert(r.epi.chain.begin(), ctx.teller);
        if (r.epi.chain.size() > Epistemic::kMaxChain) r.epi.chain.resize(Epistemic::kMaxChain);
    }
    r.prop.fact = heard.fact; // the truth reference survives every distortion
    return r;
}

bool reconcile(const NoiseParams& p, const Prop& heard, const Prop& own, const Epistemic& ownEpi, bool* contradicts) {
    const bool same = heard.pred == own.pred && heard.roles == own.roles;
    if (contradicts) *contradicts = !same;
    if (same) return false;
    return p.discardIfSeen && ownEpi.source == "seen";
}

namespace {
double roleDrift(const Prop& truth, const Prop& rumour) {
    double total = 0.0, off = 0.0;
    auto weight = [](const std::string& role) { return kRoleWeight.count(role) ? kRoleWeight.at(role) : 0.5; };
    for (const auto& [role, ref] : truth.roles) {
        total += weight(role);
        const Ref* got = rumour.role(role);
        if (!got || !(*got == ref)) off += weight(role);
    }
    for (const auto& [role, ref] : rumour.roles)
        if (!truth.roles.count(role)) {
            total += weight(role);
            off += weight(role);
        }
    if (truth.qty != rumour.qty) {
        total += 0.5;
        off += 0.5;
    }
    return total > 0.0 ? off / total : 0.0;
}
} // namespace

double drift(const Prop& truth, const Prop& rumour) {
    const double roles = roleDrift(truth, rumour);
    // another predicate (a step along the scale) is half the story changed already
    return truth.pred == rumour.pred ? roles : 0.5 + 0.5 * roles;
}

} // namespace altera::sim
