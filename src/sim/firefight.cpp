// AL-115: fighting fires (sim/firefight.hpp).

#include "sim/firefight.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "sim/building.hpp"
#include "sim/world.hpp"
#include "sim/world_fields.hpp"
#include "sim/world_reactions.hpp"

namespace lr::sim {

namespace {

// Where it burns now: burning cells of the active chunks and burning things.
const std::vector<Vec2>& firePoints(World& w) {
    WorldFields& wf = w.fields();
    if (wf.firePtsTick == static_cast<std::int64_t>(w.now())) return wf.firePts;
    wf.firePtsTick = static_cast<std::int64_t>(w.now());
    std::vector<Vec2>& out = wf.firePts;
    out.clear();
    const int n = fieldsConfig(w).chunk;
    for (const auto& [key, c] : wf.chunks) {
        if (!c.active) continue;
        for (int idx = 0; idx < n * n; ++idx)
            if (c.fire[idx] > 0) out.push_back({c.cx * n + idx % n + 0.5, c.cy * n + idx / n + 0.5});
    }
    for (const auto& [id, f] : wf.burningObjects)
        if (const WorldObject* o = w.object(id)) out.push_back(o->pos);
    for (const auto& [id, f] : wf.burningPieces)
        if (auto it = w.pieces().find(id); it != w.pieces().end()) {
            if (const BuildPieceDef* d = w.content().build.find(it->second.kind)) out.push_back(pieceCentre(w.content().build, *d, it->second));
        }
    return out;
}

bool nearest(const std::vector<Vec2>& pts, Vec2 at, double maxM, Vec2& best) {
    double bd = maxM;
    bool found = false;
    for (const Vec2& p : pts) {
        const double d = dist(p, at);
        if (d <= bd) {
            bd = d;
            best = p;
            found = true;
        }
    }
    return found;
}

bool waterNear(const World& w, const FirefightRule& R, Vec2 at) {
    for (const auto& [id, o] : w.objects())
        if (std::find(R.waterObjects.begin(), R.waterObjects.end(), o.type) != R.waterObjects.end() && dist(o.pos, at) <= R.waterM) return true;
    if (w.map().w <= 0) return false;
    // a river or a pond: a coarse look round (every 10 m)
    const double s = 10.0;
    for (double dy = -R.waterM; dy <= R.waterM; dy += s)
        for (double dx = -R.waterM; dx <= R.waterM; dx += s) {
            if (dx * dx + dy * dy > R.waterM * R.waterM) continue;
            const Vec2 p{at.x + dx, at.y + dy};
            if (p.x < 0 || p.y < 0 || p.x >= w.map().sizeM || p.y >= w.map().sizeM) continue;
            if (w.map().waterAt(p)) return true;
        }
    return false;
}

bool able(const World& w, const Character& c) {
    return c.body.alive && !c.isAnimal() && !c.controller.isPlayer() && !c.captivity.held() && w.now() >= c.body.downUntil &&
           c.body.stage() == "adult" && !c.ai.social.active() && c.body.health > 0.4;
}

} // namespace

void firefightTick(World& w) {
    WorldFields& wf = w.fields();
    const FirefightRule& R = fieldsConfig(w).firefight;
    if (R.crew <= 0) return;
    const bool burning = !wf.burningObjects.empty() || !wf.burningPieces.empty() || anyActiveChunk(w);
    if (!burning && wf.fireAlarm.empty() && wf.firefighters.empty()) return;
    static const std::vector<Vec2> none;
    const std::vector<Vec2>& fires = burning ? firePoints(w) : none;
    // the dead and the hurt drop out
    for (auto it = wf.firefighters.begin(); it != wf.firefighters.end();) {
        const Character* c = w.character(it->first);
        it = !c || !c->body.alive || c->body.health <= 0.25 || c->captivity.held() ? wf.firefighters.erase(it) : std::next(it);
    }
    for (const auto& [sid, s] : w.settlements()) {
        if (!s.full()) continue;
        Vec2 f;
        const bool near = !fires.empty() && nearest(fires, s.center, R.alarmM, f);
        if (!near) {
            if (wf.fireAlarm.erase(sid)) {
                for (auto it = wf.firefighters.begin(); it != wf.firefighters.end();)
                    it = it->second == sid ? wf.firefighters.erase(it) : std::next(it);
                w.chronicle("fire", {}, "chronicle.fire_out", {{"settlement", s.name}});
            }
            continue;
        }
        if (wf.fireAlarm.insert(sid).second) w.chronicle("fire", {}, "chronicle.fire_alarm", {{"settlement", s.name}, {"x", f.x}, {"y", f.y}});
        int have = 0;
        for (const auto& [pid, at] : wf.firefighters) have += at == sid;
        if (have >= R.crew) continue;
        // the nearest able grown-ups of the village come running
        std::vector<std::pair<double, EntityId>> cand;
        for (const auto& [pid, c] : w.characters()) {
            if (c.status.settlement != sid || wf.firefighters.count(pid) || !able(w, c)) continue;
            const double d = dist(c.pos, f);
            if (d <= R.recruitM) cand.push_back({d, pid});
        }
        std::sort(cand.begin(), cand.end());
        for (std::size_t i = 0; i < cand.size() && have < R.crew; ++i, ++have) wf.firefighters[cand[i].second] = sid;
    }
}

bool firefighting(World& w, Character& c) {
    WorldFields& wf = w.fields();
    auto it = wf.firefighters.find(c.id);
    if (it == wf.firefighters.end()) return false;
    const FirefightRule& R = fieldsConfig(w).firefight;
    const std::vector<Vec2>& fires = firePoints(w);
    Vec2 f;
    if (!c.body.alive || fires.empty() || !nearest(fires, c.pos, R.recruitM + R.alarmM, f)) {
        wf.firefighters.erase(it);
        return false;
    }
    const double d = dist(c.pos, f);
    if (d > R.reachM) {
        // run there, stopping short of the flames
        const Vec2 to{f.x + (c.pos.x - f.x) * (R.reachM * 0.8) / d, f.y + (c.pos.y - f.y) * (R.reachM * 0.8) / d};
        const double run = w.content().config.walkMPerTick * stepMoveFactor(w, c.pos, to, w.content().config.walkMPerTick);
        const double go = std::min(run, dist(c.pos, to));
        if (go >= dist(c.pos, to)) c.pos = to;
        else {
            c.pos.x += (to.x - c.pos.x) * go / dist(c.pos, to);
            c.pos.y += (to.y - c.pos.y) * go / dist(c.pos, to);
        }
        if (dist(c.pos, f) > R.reachM) return true;
    }
    const double k = waterNear(w, R, f) ? 1.0 : R.noWaterMult;
    douse(w, f, R.reachM, static_cast<int>(std::lround(R.douse * k)));
    c.body.fatigue = std::min(1.0, c.body.fatigue + R.fatigue);
    return true;
}

} // namespace lr::sim
