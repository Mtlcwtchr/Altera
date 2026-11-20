// Layer 3 - GOAP: "out of which physical actions?" A* over a small state
// summary (inventory counts + which kind of object the character stands at),
// with the catalog interactions as actions and implicit goto-<type> moves whose
// cost is the walking distance. Depth and node budgets bound the search;
// reservations keep two people off one bush.

#include <algorithm>
#include <map>
#include <queue>
#include <set>

#include "sim/ai.hpp"

namespace altera::sim {

namespace {

struct Node {
    std::map<std::string, int> has;
    std::string at;
    Vec2 pos;
    double g = 0.0;
    double f = 0.0;
    int depth = 0;
    int parent = -1;
    PlanAction act;
    std::uint64_t order = 0;
};

std::string keyOf(const Node& n) {
    std::string k = n.at + "|";
    for (const auto& [item, cnt] : n.has)
        if (cnt > 0) k += item + "=" + std::to_string(cnt) + ";";
    return k;
}

bool satisfied(const Node& n, const Condition& k) {
    if (k.subject == "at") return k.test(n.at == k.key ? 1.0 : 0.0);
    auto it = n.has.find(k.key);
    return k.test(it == n.has.end() ? 0.0 : it->second);
}

bool staticOk(const World& w, const Character& c, const InteractionDef& d, std::string* why,
              const Character* person = nullptr) {
    // A creature act is judged against the herd it would go to (t.has:eggs,
    // herd:cull of that herd).
    const Character* beast = nullptr;
    if (!person && d.targetsCreature())
        beast = w.character(findCreature(w, d.targetType, c.pos, w.content().config.trackRadiusM, c.status.family));
    const EvalCtx x{w, c, nullptr, person ? person : beast};
    for (const auto& k : d.pre) {
        if (goapFact(k) || k.subject == "target_stock" || k.subject == "target_owner" || k.subject == "site") continue;
        if (!evalCondition(x, k)) {
            if (why) *why = d.id + ":" + k.text;
            return false;
        }
    }
    return true;
}

} // namespace

GoapResult planStep(const World& w, const Character& c, const PlanStep& step) {
    const Content& ct = w.content();
    const AiConfig& cfg = ct.config;
    GoapResult res;

    // Goal facts and the final action.
    std::vector<Condition> goal;
    EntityId personId = kNone;
    const InteractionDef* finalAction = nullptr;
    if (step.kind == "do") {
        finalAction = ct.interaction(step.ref);
        if (!finalAction) {
            res.why = "goap.unknown_action:" + step.ref;
            return res;
        }
        std::string why;
        if (finalAction->targetsCharacter()) {
            std::string pw;
            personId = findPerson(w, c, *finalAction, c.ai.goal.target, &pw);
            if (personId == kNone) {
                res.why = "goap.no_person:" + finalAction->id + ":" + pw;
                return res;
            }
        }
        if (!staticOk(w, c, *finalAction, &why, w.character(personId))) {
            res.why = "goap.cannot:" + why;
            return res;
        }
        for (const auto& k : finalAction->pre)
            if ((k.subject == "has" && !k.onTarget)) goal.push_back(k);
        if (finalAction->targetsObject() || finalAction->targetsCreature() || finalAction->targetsCharacter()) {
            Condition at;
            at.subject = "at";
            at.key = finalAction->targetType;
            at.text = "at:" + at.key;
            goal.push_back(at);
        }
    } else {
        for (const auto& t : step.achieve) goal.push_back(parseCondition(t));
    }

    // Allowed actions: the step's whitelist, else every lawful GOAP action.
    std::vector<const InteractionDef*> allowed;
    std::string skipWhy;
    auto consider = [&](const InteractionDef* d) {
        if (!d) return;
        std::string why;
        if (!staticOk(w, c, *d, &why)) {
            skipWhy = why;
            return;
        }
        allowed.push_back(d);
    };
    if (!step.allow.empty()) {
        for (const auto& a : step.allow) consider(ct.interaction(a));
    } else {
        for (const auto& [id, d] : ct.interactions)
            if (d.goap && d.legal.empty()) consider(&d);
    }
    // Relevance pruning (README_B §6): keep only actions that produce an item
    // the goal needs, or an item a kept action needs (backward closure). With
    // ~150 catalog entries an unpruned search burns its node budget on
    // unrelated crafts.
    {
        std::set<std::string> need;
        for (const auto& k : goal)
            if ((k.subject == "has" && !k.onTarget)) need.insert(k.key);
        if (finalAction)
            for (const auto& k : finalAction->pre)
                if ((k.subject == "has" && !k.onTarget)) need.insert(k.key);
        std::vector<bool> keep(allowed.size(), false);
        // An at:<type> goal keeps one action of that type so the type is located.
        for (const auto& k : goal)
            if (k.subject == "at")
                for (size_t i = 0; i < allowed.size(); ++i)
                    if (allowed[i]->targetType == k.key) {
                        keep[i] = true;
                        break;
                    }
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t i = 0; i < allowed.size(); ++i) {
                if (keep[i]) continue;
                bool produces = false;
                for (const auto& e : allowed[i]->eff)
                    if (e.subject == "has" && !e.onTarget && e.signedDelta() > 0.0 && need.count(e.key)) produces = true;
                if (!produces) continue;
                keep[i] = true;
                changed = true;
                for (const auto& k : allowed[i]->pre)
                    if ((k.subject == "has" && !k.onTarget)) need.insert(k.key);
            }
        }
        std::vector<const InteractionDef*> kept;
        for (size_t i = 0; i < allowed.size(); ++i)
            if (keep[i]) kept.push_back(allowed[i]);
        allowed.swap(kept);
    }
    // Representative instance per object type (nearest valid for the first
    // interaction needing it), and the types we can actually reach.
    std::map<std::string, Vec2> typePos;
    std::map<std::string, std::string> typeWhy;
    auto locate = [&](const InteractionDef* d) {
        if (!d || !(d->targetsObject() || d->targetsCreature()) || typePos.count(d->targetType)) return;
        std::string why;
        const EntityId t = findTarget(w, c, *d, c.pos, &why, true);
        Vec2 p;
        if (t != kNone && targetPos(w, t, p)) typePos[d->targetType] = p;
        else typeWhy[d->targetType] = why;
    };
    for (const auto* d : allowed) locate(d);
    locate(finalAction);
    if (personId != kNone) {
        const Character* pc = w.character(personId);
        typePos["person"] = pc->pos;
    }
    if (finalAction && (finalAction->targetsObject() || finalAction->targetsCreature()) &&
        !typePos.count(finalAction->targetType)) {
        res.why = "goap.no_target:" + typeWhy[finalAction->targetType];
        return res;
    }

    // Start node.
    Node start;
    for (const auto& [item, n] : c.inventory) start.has[item] = n;
    start.pos = c.pos;
    for (const auto& [type, p] : typePos)
        if (dist(p, c.pos) <= cfg.interactionRangeM) {
            start.at = type;
            start.pos = p;
            break;
        }

    auto isGoal = [&](const Node& n) {
        for (const auto& k : goal)
            if (!satisfied(n, k)) return false;
        return true;
    };
    auto h = [&](const Node& n) {
        int u = 0;
        for (const auto& k : goal) u += satisfied(n, k) ? 0 : 1;
        return static_cast<double>(u);
    };

    std::vector<Node> nodes;
    nodes.reserve(256);
    auto cmp = [&](int a, int b) {
        const Node& x = nodes[static_cast<size_t>(a)];
        const Node& y = nodes[static_cast<size_t>(b)];
        return x.f != y.f ? x.f > y.f : x.order > y.order;
    };
    std::priority_queue<int, std::vector<int>, decltype(cmp)> open(cmp);
    std::map<std::string, double> best;
    std::uint64_t order = 0;
    start.f = h(start);
    start.order = order++;
    nodes.push_back(start);
    open.push(0);
    best[keyOf(start)] = 0.0;
    int found = -1;
    while (!open.empty()) {
        const int cur = open.top();
        open.pop();
        const Node n = nodes[static_cast<size_t>(cur)];
        if (best.count(keyOf(n)) && best[keyOf(n)] < n.g - 1e-9) continue;
        if (isGoal(n)) {
            found = cur;
            break;
        }
        if (n.depth >= cfg.goapMaxDepth) continue;
        if (static_cast<int>(nodes.size()) >= cfg.goapMaxNodes) {
            res.why = "goap.budget";
            break;
        }
        auto push = [&](Node m) {
            m.parent = cur;
            m.depth = n.depth + 1;
            m.f = m.g + h(m);
            m.order = order++;
            const std::string k = keyOf(m);
            auto it = best.find(k);
            if (it != best.end() && it->second <= m.g + 1e-9) return;
            best[k] = m.g;
            nodes.push_back(std::move(m));
            open.push(static_cast<int>(nodes.size()) - 1);
        };
        // Moves.
        for (const auto& [type, p] : typePos) {
            if (type == n.at) continue;
            Node m = n;
            m.at = type;
            m.pos = p;
            const double walk = dist(n.pos, p) / std::max(1.0, cfg.walkMPerTick);
            m.g = n.g + walk + 0.5;
            m.act = PlanAction{"goto", kNone, type, walk + 0.5};
            push(std::move(m));
        }
        // Interactions.
        for (const auto* d : allowed) {
            if ((d->targetsObject() || d->targetsCreature()) && d->targetType != n.at) continue;
            bool ok = true;
            for (const auto& k : d->pre)
                if ((k.subject == "has" && !k.onTarget) && !satisfied(n, k)) ok = false;
            if (!ok) continue;
            Node m = n;
            bool progress = false;
            for (const auto& e : d->eff) {
                if (e.subject != "has" || e.onTarget) continue; // t.has: the target's, not the plan's
                int& v = m.has[e.key];
                v = e.op == '=' ? static_cast<int>(e.value) : v + static_cast<int>(e.signedDelta());
                if (v < 0) ok = false;
                progress = true;
            }
            if (!ok || !progress) continue;
            m.g = n.g + d->cost;
            m.act = PlanAction{d->id, kNone, d->targetType, d->cost};
            push(std::move(m));
        }
    }
    res.nodes = static_cast<int>(nodes.size());
    if (found < 0) {
        if (res.why.empty()) {
            std::string missing;
            for (const auto& k : goal)
                if (!satisfied(start, k)) missing = k.text;
            res.why = "goap.no_chain:" + missing;
            if (!typeWhy.empty()) res.why += " (" + typeWhy.begin()->second + ")";
            else if (!skipWhy.empty()) res.why += " (" + skipWhy + ")";
        }
        return res;
    }
    std::vector<PlanAction> rev;
    for (int i = found; i > 0; i = nodes[static_cast<size_t>(i)].parent) rev.push_back(nodes[static_cast<size_t>(i)].act);
    res.actions.assign(rev.rbegin(), rev.rend());
    res.cost = nodes[static_cast<size_t>(found)].g;
    if (finalAction) {
        res.actions.push_back(PlanAction{finalAction->id, personId, finalAction->targetType, finalAction->cost});
        if (personId != kNone)
            for (auto& a : res.actions)
                if (a.interaction == "goto" && a.targetType == "person") a.target = personId;
        res.cost += finalAction->cost;
    }
    res.ok = true;
    return res;
}

} // namespace altera::sim
