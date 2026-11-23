#pragma once
// AL-79: how a person moves on screen. The sim says where someone is and what
// they do; this turns it into a pose that moves well:
//
//  - a speed blend space (idle / walk / run / sprint) whose clips keep one step
//    phase (the left heel strikes at phase 0 in every clip), so walk and run
//    blend without the legs scissoring;
//  - crossfades between locomotion and act clips with eased weights; the
//    player's person fades and turns faster than the others (it answers the keys);
//  - start and stop by a speed spring (Skyrim has no start/stop clips): the
//    stride grows and dies away, the body leans into the push and back on the stop;
//  - turning: the body follows the way of travel by a spring, leans into the
//    curve, and on the spot turns at a limited rate with Skyrim's turn loop
//    blended in by the turn's speed;
//  - procedural layers on the clip pose: head / neck / chest look at someone
//    (clamped, eased), lean, breathing, foot IK on the ground (two-bone legs, the
//    pelvis lowered to the lower foot, the planted foot laid on the slope).
//
// Pure logic, the standard library only (no engine, no GPU): the skinned set
// (skinned.cpp) applies the rig operations, sim_game.cpp feeds the animator.
// Tests: tests/test_altera_locomotion.cpp. Parameters: content/animations/locomotion.json.

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace altera::game::loco {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = kPi / 180.0f;

// ---------------------------------------------------------------- small math
struct V3 {
    float x = 0, y = 0, z = 0;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 normalize(V3 a, V3 fallback = {0, 0, 1}) {
    const float l = length(a);
    return l > 1e-8f ? a * (1.0f / l) : fallback;
}

struct Q {
    float x = 0, y = 0, z = 0, w = 1;
};
inline Q operator*(Q a, Q b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Q axisAngle(V3 axis, float angle) {
    const V3 n = normalize(axis);
    const float s = std::sin(angle * 0.5f);
    return {n.x * s, n.y * s, n.z * s, std::cos(angle * 0.5f)};
}
inline V3 rotate(Q q, V3 v) {
    const V3 u{q.x, q.y, q.z};
    const V3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}
inline float angleOf(Q q) { return 2.0f * std::acos(std::clamp(std::fabs(q.w), 0.0f, 1.0f)); }
// The shortest rotation taking direction a onto direction b.
inline Q fromTo(V3 a, V3 b) {
    a = normalize(a), b = normalize(b);
    const float d = dot(a, b);
    if (d < -0.99999f) {   // opposite: any axis across a
        V3 ax = cross({1, 0, 0}, a);
        if (length(ax) < 1e-4f) ax = cross({0, 1, 0}, a);
        return axisAngle(ax, kPi);
    }
    const V3 c = cross(a, b);
    Q q{c.x, c.y, c.z, 1.0f + d};
    const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x / l, q.y / l, q.z / l, q.w / l};
}
// The same turn, `k` of the way (0 none, 1 whole).
inline Q scaled(Q q, float k) {
    const float a = angleOf(q);
    if (a < 1e-6f) return {};
    V3 ax{q.x, q.y, q.z};
    if (q.w < 0) ax = ax * -1.0f;
    return axisAngle(ax, a * k);
}
inline Q clampAngle(Q q, float maxAngle) {
    const float a = angleOf(q);
    return a > maxAngle && a > 1e-6f ? scaled(q, maxAngle / a) : q;
}

inline float wrapPi(float a) { return std::remainder(a, 2.0f * kPi); }
inline float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
inline float smooth01(float t) {
    t = clamp01(t);
    return t * t * (3 - 2 * t);
}
inline float frac(float v) { return v - std::floor(v); }

// Critically damped spring (half-life form): x eases to `target`, v is its speed.
inline void spring(float& x, float& v, float target, float halfLife, float dt) {
    const float h = 2.0f * 0.69314718f / std::max(halfLife, 1e-4f);   // half the damping
    const float j0 = x - target, j1 = v + j0 * h;
    const float e = std::exp(-h * dt);
    x = e * (j0 + j1 * dt) + target;
    v = e * (v - j1 * h * dt);
}
// The same on an angle: the short way round, the result kept in (-pi, pi].
inline void springAngle(float& a, float& v, float target, float halfLife, float dt) {
    float t = a + wrapPi(target - a);
    spring(a, v, t, halfLife, dt);
    a = wrapPi(a);
}
inline float damp(float x, float target, float halfLife, float dt) {
    return target + (x - target) * std::exp2(-dt / std::max(halfLife, 1e-4f));
}

// ---------------------------------------------------------------- config
struct Config {
    // blend space: the natural speeds (m/s) of the clips when they cannot be
    // measured from the feet (see analyseGait)
    float walkSpeed = 1.5f, runSpeed = 3.8f, sprintSpeed = 6.0f;
    bool measureSpeeds = true;
    float maxRate = 1.6f;   // over the fastest clip's speed the clip plays faster up to this
    float idleBelow = 0.08f;
    float speedHalfLife = 0.14f, playerSpeedHalfLife = 0.05f;
    float fadeS = 0.30f, playerFadeS = 0.15f;
    // turning
    float yawHalfLife = 0.16f, playerYawHalfLife = 0.06f;
    bool turnInPlace = true;
    float turnRateDeg = 220.0f, playerTurnRateDeg = 480.0f;   // on the spot, at most
    float turnClipRateDeg = 120.0f;   // the turn loop's own speed: its weight is the turn's share of it
    // lean
    bool lean = true;
    float leanTurn = 0.055f;          // rad per (rad/s x m/s)
    float leanTurnMaxDeg = 11.0f;
    float leanAccel = 0.035f;         // rad per m/s^2
    float leanAccelMaxDeg = 7.0f;
    float leanSlope = 0.45f;          // rad per unit rise along the way
    float leanSlopeMaxDeg = 8.0f;
    float leanHalfLife = 0.18f;
    float leanBodyShare = 0.55f;      // of the lean the whole body takes (about the feet), the rest the spine
    // look at
    bool look = true;
    float lookYawMaxDeg = 75.0f, lookPitchUpDeg = 25.0f, lookPitchDownDeg = 35.0f;
    float lookDropDeg = 115.0f;       // a target further round than this is let go
    float lookHalfLife = 0.16f, playerLookHalfLife = 0.10f;
    float lookRange = 10.0f;          // talk partner / aim at most this far
    float glanceRange = 6.0f;         // NPCs glance at the player's person this close
    std::array<float, 3> lookShare = {0.25f, 0.30f, 0.45f};   // chest, neck, head
    // foot IK
    bool footIk = true;
    float pelvisMaxDrop = 0.28f;
    float footAlignMaxDeg = 28.0f;
    float footLift = 0.12f;           // the foot this high over its rest height is not planted
    float maxSlope = 0.9f;
    // breathing / idle
    bool breath = true;
    float breathPeriod = 4.2f, breathDeg = 1.1f;
    float exertPeriod = 1.7f, exertDeg = 2.4f;
    float exertRise = 0.12f, exertFall = 0.05f;   // per second running / resting
    // reactions: NPCs turn to the player's person when called or come close
    bool react = true;
    float reactRange = 3.2f;
    // the rig (Skyrim names first, the UAL ones as a fallback)
    std::vector<std::string> pelvis{"NPC Pelvis [Pelv]", "pelvis"};
    std::vector<std::string> com{"NPC COM [COM ]", "pelvis"};
    std::vector<std::string> spine0{"NPC Spine [Spn0]", "spine_01"}, spine1{"NPC Spine1 [Spn1]", "spine_02"},
        spine2{"NPC Spine2 [Spn2]", "spine_03"}, neck{"NPC Neck [Neck]", "neck_01"}, head{"NPC Head [Head]", "Head"};
    std::vector<std::string> thighL{"NPC L Thigh [LThg]", "thigh_l"}, calfL{"NPC L Calf [LClf]", "calf_l"},
        footL{"NPC L Foot [Lft ]", "foot_l"};
    std::vector<std::string> thighR{"NPC R Thigh [RThg]", "thigh_r"}, calfR{"NPC R Calf [RClf]", "calf_r"},
        footR{"NPC R Foot [Rft ]", "foot_r"};
    // clips whose feet are not on the ground under the person (no IK, no look)
    std::vector<std::string> offFeet{"sit", "sleep", "bedroll", "death", "down", "kneel", "pray", "crouch",
                                     "swim", "lounge", "fall"};
};

// From the JSON (nlohmann or anything with contains / value / at / items):
// every key optional, unknown keys ignored.
template <class J> void readConfig(Config& c, const J& j) {
    auto f = [&](const char* k, float& v) { v = j.contains(k) ? float(j.at(k).template get<double>()) : v; };
    auto b = [&](const char* k, bool& v) { v = j.contains(k) ? j.at(k).template get<bool>() : v; };
    auto names = [&](const char* k, std::vector<std::string>& v) {
        if (j.contains(k)) v = j.at(k).template get<std::vector<std::string>>();
    };
    f("walk_speed", c.walkSpeed), f("run_speed", c.runSpeed), f("sprint_speed", c.sprintSpeed);
    b("measure_speeds", c.measureSpeeds), f("max_rate", c.maxRate), f("idle_below", c.idleBelow);
    f("speed_half_life", c.speedHalfLife), f("player_speed_half_life", c.playerSpeedHalfLife);
    f("fade_s", c.fadeS), f("player_fade_s", c.playerFadeS);
    f("yaw_half_life", c.yawHalfLife), f("player_yaw_half_life", c.playerYawHalfLife);
    b("turn_in_place", c.turnInPlace), f("turn_rate_deg", c.turnRateDeg), f("player_turn_rate_deg", c.playerTurnRateDeg);
    f("turn_clip_rate_deg", c.turnClipRateDeg);
    b("lean", c.lean), f("lean_turn", c.leanTurn), f("lean_turn_max_deg", c.leanTurnMaxDeg);
    f("lean_accel", c.leanAccel), f("lean_accel_max_deg", c.leanAccelMaxDeg), f("lean_slope", c.leanSlope);
    f("lean_slope_max_deg", c.leanSlopeMaxDeg), f("lean_half_life", c.leanHalfLife), f("lean_body_share", c.leanBodyShare);
    b("look", c.look), f("look_yaw_max_deg", c.lookYawMaxDeg), f("look_pitch_up_deg", c.lookPitchUpDeg);
    f("look_pitch_down_deg", c.lookPitchDownDeg), f("look_drop_deg", c.lookDropDeg);
    f("look_half_life", c.lookHalfLife), f("player_look_half_life", c.playerLookHalfLife);
    f("look_range", c.lookRange), f("glance_range", c.glanceRange);
    if (j.contains("look_share")) {
        const auto v = j.at("look_share").template get<std::vector<double>>();
        for (std::size_t i = 0; i < 3 && i < v.size(); ++i) c.lookShare[i] = float(v[i]);
    }
    b("foot_ik", c.footIk), f("pelvis_max_drop", c.pelvisMaxDrop), f("foot_align_max_deg", c.footAlignMaxDeg);
    f("foot_lift", c.footLift), f("max_slope", c.maxSlope);
    b("breath", c.breath), f("breath_period", c.breathPeriod), f("breath_deg", c.breathDeg);
    f("exert_period", c.exertPeriod), f("exert_deg", c.exertDeg), f("exert_rise", c.exertRise), f("exert_fall", c.exertFall);
    b("react", c.react), f("react_range", c.reactRange);
    names("pelvis_joint", c.pelvis), names("com_joint", c.com), names("spine0_joint", c.spine0);
    names("spine1_joint", c.spine1), names("spine2_joint", c.spine2), names("neck_joint", c.neck);
    names("head_joint", c.head), names("thigh_l_joint", c.thighL), names("calf_l_joint", c.calfL);
    names("foot_l_joint", c.footL), names("thigh_r_joint", c.thighR), names("calf_r_joint", c.calfR);
    names("foot_r_joint", c.footR), names("off_feet_clips", c.offFeet);
}

inline bool offFeet(const Config& c, const std::string& clipName) {
    for (const auto& s : c.offFeet)
        if (!s.empty() && clipName.find(s) != std::string::npos) return true;
    return false;
}

// ---------------------------------------------------------------- gait of a clip
// A looped locomotion clip, read from its feet: cycle length, the phase where
// the left heel strikes (the left foot furthest forward of the right), and the
// speed the stance foot slides back at (the speed the clip walks at, in place).
struct Gait {
    float cycle = 1;    // seconds
    float offset = 0;   // phase [0, 1) of the left heel strike
    float speed = 0;    // m/s, 0 when not measured
    bool ok = false;
};

// `left`, `right`: per frame, the feet in the figure's space with the clip's
// drift taken off; `forward`: the way the figure faces (horizontal).
inline Gait analyseGait(const std::vector<V3>& left, const std::vector<V3>& right, float fps, V3 forward) {
    Gait g;
    const std::size_t n = std::min(left.size(), right.size());
    if (n < 4 || fps <= 0) return g;
    g.cycle = float(n) / fps;
    forward = normalize({forward.x, forward.y, 0}, {0, -1, 0});
    std::size_t best = 0;
    float bestAhead = -1e9f;
    double slide = 0;
    int count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const float ahead = dot(left[i] - right[i], forward);
        if (ahead > bestAhead) bestAhead = ahead, best = i;
        // the lower foot stands: its backward speed is the travel speed
        const std::size_t a = (i + n - 1) % n, b = (i + 1) % n;
        const bool l = left[i].z <= right[i].z;
        const V3 d = l ? left[b] - left[a] : right[b] - right[a];
        const float v = -dot(d, forward) * fps * 0.5f;
        // a wrap of the loop that jumps (not a clean cycle) is not a step
        if (std::fabs(v) < 15.0f) slide += v, ++count;
    }
    g.offset = float(best) / float(n);
    g.speed = count ? float(slide / count) : 0.0f;
    g.ok = g.speed > 0.2f && g.speed < 12.0f;
    return g;
}

// ---------------------------------------------------------------- blend space
struct Node {
    int clip = -1;
    float speed = 0;   // natural speed; 0 for idle
    Gait gait;
};
struct Pick {
    int a = -1, b = -1;   // node indices; b weighs w, a weighs 1 - w
    float w = 0;
    float rate = 1;       // clip speed-up over the clips' own
};

// nodes[0] is idle, the rest locomotion clips by rising speed.
inline Pick blendSpace(const std::vector<Node>& nodes, float speed, float maxRate, float idleBelow) {
    Pick p;
    if (nodes.empty()) return p;
    p.a = 0;
    if (nodes.size() == 1 || speed <= idleBelow) return p;
    const std::size_t last = nodes.size() - 1;
    if (speed >= nodes[last].speed) {
        p.a = int(last);
        p.rate = std::min(maxRate, speed / std::max(0.1f, nodes[last].speed));
        return p;
    }
    for (std::size_t i = 0; i + 1 < nodes.size(); ++i) {
        const float s0 = nodes[i].speed, s1 = nodes[i + 1].speed;
        if (speed >= s0 && speed < s1) {
            p.a = int(i), p.b = int(i + 1);
            p.w = (speed - s0) / std::max(1e-3f, s1 - s0);
            return p;
        }
    }
    return p;
}

// Cycles a second the shared step phase advances at for this pick (0 standing).
inline float phaseRate(const std::vector<Node>& nodes, const Pick& p) {
    auto inv = [&](int i) { return i > 0 && i < int(nodes.size()) ? 1.0f / std::max(0.05f, nodes[std::size_t(i)].gait.cycle) : 0.0f; };
    if (p.a < 0) return 0;
    if (p.b < 0) return p.rate * inv(p.a);
    if (p.a == 0) return p.rate * inv(p.b);   // out of idle: the walk's own beat
    return p.rate * (inv(p.a) * (1 - p.w) + inv(p.b) * p.w);
}

// Where in a clip the shared phase is.
inline double clipTime(const Gait& g, float phase) { return double(frac(phase + g.offset)) * g.cycle; }

// ---------------------------------------------------------------- crossfade
// The eased weights of slots fading: each slot's linear weight w in [0, 1]; the
// shown weight is smoothstep(w), normalised over all slots.
inline void normaliseFade(const float* linear, int n, float* out) {
    float sum = 0;
    for (int i = 0; i < n; ++i) sum += out[i] = smooth01(linear[i]);
    if (sum <= 1e-6f) {
        for (int i = 0; i < n; ++i) out[i] = i == n - 1 ? 1.0f : 0.0f;
        return;
    }
    for (int i = 0; i < n; ++i) out[i] /= sum;
}

// ---------------------------------------------------------------- look at
struct Look {
    float yaw = 0, pitch = 0;   // relative to the body, clamped
    bool keep = false;          // false: let it go (behind, or no target)
};
// Target direction (world yaw, pitch up) seen from a body facing `bodyYaw`.
inline Look clampLook(const Config& c, float bodyYaw, float targetYaw, float targetPitch) {
    Look l;
    const float rel = wrapPi(targetYaw - bodyYaw);
    if (std::fabs(rel) > c.lookDropDeg * kDeg) return l;
    l.keep = true;
    l.yaw = std::clamp(rel, -c.lookYawMaxDeg * kDeg, c.lookYawMaxDeg * kDeg);
    l.pitch = std::clamp(targetPitch, -c.lookPitchDownDeg * kDeg, c.lookPitchUpDeg * kDeg);
    return l;
}

// ---------------------------------------------------------------- two-bone IK
struct TwoBone {
    Q upper, lower;   // world rotations: `upper` about the hip, then `lower` about the new knee
    V3 knee;          // the knee after `upper`
    bool reached = true;
};
// Hip a, knee b, ankle c; the ankle wanted at t. The knee keeps its plane (the
// current bend; `pole` when the leg is straight).
inline TwoBone solveTwoBone(V3 a, V3 b, V3 c, V3 t, V3 pole) {
    TwoBone r;
    const float l1 = length(b - a), l2 = length(c - b);
    const V3 at = t - a;
    float d = length(at);
    const float lo = std::fabs(l1 - l2) + 1e-4f, hi = l1 + l2 - 1e-4f;
    if (d > hi || d < lo) r.reached = false;
    d = std::clamp(d, lo, hi);
    const V3 dir = normalize(at, normalize(c - a));
    // the bend direction: the knee's offset off the hip-ankle line, else the pole
    const V3 ac = normalize(c - a, dir);
    V3 bend = (b - a) - ac * dot(b - a, ac);
    if (length(bend) < 1e-4f * (l1 + 1e-6f)) bend = pole;
    bend = bend - dir * dot(bend, dir);
    bend = normalize(bend, normalize(cross(dir, {1, 0, 0}), {0, 0, 1}));
    const float cosA = std::clamp((l1 * l1 + d * d - l2 * l2) / (2 * l1 * d), -1.0f, 1.0f);
    const float sinA = std::sqrt(std::max(0.0f, 1 - cosA * cosA));
    r.knee = a + dir * (l1 * cosA) + bend * (l1 * sinA);
    r.upper = fromTo(b - a, r.knee - a);
    const V3 c1 = a + rotate(r.upper, c - a);
    r.lower = fromTo(c1 - r.knee, (a + dir * d) - r.knee);
    return r;
}

// ---------------------------------------------------------------- ground
// The ground as a plane through the figure's feet: height gx * x + gy * y.
inline float planeHeight(float gx, float gy, V3 p) { return gx * p.x + gy * p.y; }
inline V3 planeNormal(float gx, float gy) { return normalize({-gx, -gy, 1}); }
// The world gradient in the figure's own frame (the figure turned by `angle`).
inline void gradientToModel(float gx, float gy, float angle, float& mx, float& my) {
    const float c = std::cos(angle), s = std::sin(angle);
    mx = c * gx + s * gy;
    my = -s * gx + c * gy;
}
// How far the body drops so the lower foot reaches its ground.
inline float pelvisDrop(float groundL, float groundR, float maxDrop) {
    return std::clamp(std::min({groundL, groundR, 0.0f}), -maxDrop, 0.0f);
}
inline float plantWeight(float ankleZ, float restZ, float lift) {
    return 1.0f - clamp01((ankleZ - restZ) / std::max(1e-3f, lift));
}

// ---------------------------------------------------------------- rig operations
// World matrices of a skeleton (row-major 4x4 per joint, translation in the
// last column), parents before children. Turn joint `j` and all under it by `r`
// about `pivot`.
inline void rotateSubtree(float* world, const std::vector<int>& parent, int j, Q r, V3 pivot,
                          std::vector<char>& scratch) {
    const int n = int(parent.size());
    if (j < 0 || j >= n) return;
    scratch.assign(std::size_t(n), 0);
    scratch[std::size_t(j)] = 1;
    // a rotation matrix of r
    const float x = r.x, y = r.y, z = r.z, w = r.w;
    const float R[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
                        2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                        2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)};
    for (int k = j; k < n; ++k) {
        if (k != j) {
            const int p = parent[std::size_t(k)];
            if (p < 0 || !scratch[std::size_t(p)]) continue;
            scratch[std::size_t(k)] = 1;
        }
        float* m = world + std::size_t(k) * 16;
        float o[12];
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 4; ++col)
                o[row * 4 + col] = R[row * 3] * m[col] + R[row * 3 + 1] * m[4 + col] + R[row * 3 + 2] * m[8 + col];
        // translation: about the pivot
        const V3 t{m[3] - pivot.x, m[7] - pivot.y, m[11] - pivot.z};
        o[3] = R[0] * t.x + R[1] * t.y + R[2] * t.z + pivot.x;
        o[7] = R[3] * t.x + R[4] * t.y + R[5] * t.z + pivot.y;
        o[11] = R[6] * t.x + R[7] * t.y + R[8] * t.z + pivot.z;
        std::copy(o, o + 12, m);
    }
}
inline V3 jointPos(const float* world, int j) {
    const float* m = world + std::size_t(j) * 16;
    return {m[3], m[7], m[11]};
}
// The pure rotation of a 3x3 (rows) with a uniform scale (and maybe a mirror
// shared by both frames): the turn taking frame `now` back onto frame `was`.
inline Q turnBetween(const float* was, const float* now) {
    // M = was * now^T / scale^2 (row-major 4x4 blocks)
    float M[9];
    float s2 = 0;
    for (int i = 0; i < 3; ++i) s2 += now[i] * now[i];
    s2 = s2 > 1e-12f ? s2 : 1.0f;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            M[r * 3 + c] = (was[r * 4] * now[c * 4] + was[r * 4 + 1] * now[c * 4 + 1] + was[r * 4 + 2] * now[c * 4 + 2]) / s2;
    Q q;
    const float tr = M[0] + M[4] + M[8];
    if (tr > 0) {
        const float k = std::sqrt(tr + 1) * 2;
        q = {(M[7] - M[5]) / k, (M[2] - M[6]) / k, (M[3] - M[1]) / k, 0.25f * k};
    } else if (M[0] > M[4] && M[0] > M[8]) {
        const float k = std::sqrt(1 + M[0] - M[4] - M[8]) * 2;
        q = {0.25f * k, (M[1] + M[3]) / k, (M[2] + M[6]) / k, (M[7] - M[5]) / k};
    } else if (M[4] > M[8]) {
        const float k = std::sqrt(1 + M[4] - M[0] - M[8]) * 2;
        q = {(M[1] + M[3]) / k, 0.25f * k, (M[5] + M[7]) / k, (M[2] - M[6]) / k};
    } else {
        const float k = std::sqrt(1 + M[8] - M[0] - M[4]) * 2;
        q = {(M[2] + M[6]) / k, (M[5] + M[7]) / k, 0.25f * k, (M[3] - M[1]) / k};
    }
    const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x / l, q.y / l, q.z / l, q.w / l};
}
inline void translateAll(float* world, int n, V3 d) {
    for (int k = 0; k < n; ++k) {
        float* m = world + std::size_t(k) * 16;
        m[3] += d.x, m[7] += d.y, m[11] += d.z;
    }
}

// ---------------------------------------------------------------- the animator
// What a figure plays and how its procedural layers stand this frame.
struct Layer {
    int clip = -1;
    double time = 0;
    float weight = 0;
    float phase = -1;   // >= 0: a locomotion clip at this shared phase (its time from its gait)
};
constexpr int kMaxLayers = 8;

struct Output {
    std::array<Layer, kMaxLayers> layers{};
    int count = 0;
    float yaw = 0;               // the body's facing (world)
    float leanFwd = 0, leanLeft = 0;   // radians
    float lookYaw = 0, lookPitch = 0, lookWeight = 0;   // body relative
    float breath = 0;            // chest pitch, radians
    float feet = 0;              // weight of foot IK (the share of the pose that stands on its feet)
    float speed = 0;             // the eased speed shown
    float turnRate = 0;          // rad/s of the body
};

struct Inputs {
    float dt = 0;
    float speed = 0;             // how fast the sim moves them now, m/s
    float moveYaw = 0;           // the way of travel (when moving)
    bool face = false;           // turn to `faceYaw` when standing (talk partner, a call)
    float faceYaw = 0;
    int action = -1;             // an act clip, -1: locomotion (idle when standing)
    double actionRate = 1;
    bool actionOnFeet = true;    // the act stands on the person's feet (IK, look)
    bool player = false;         // the player's person: quick to answer
    bool look = false;
    float lookYaw = 0, lookPitch = 0;   // world direction to look along
    float slopeAlong = 0;        // ground rise per metre along the way faced
};

class Animator {
public:
    // nodes: idle, walk, run, sprint (missing ones left out); turn loops may be -1.
    void setup(std::vector<Node> nodes, int turnLeft, int turnRight) {
        nodes_ = std::move(nodes);
        turnL_ = turnLeft, turnR_ = turnRight;
    }
    bool started() const { return started_; }
    float yaw() const { return yaw_; }

    Output update(const Config& c, const Inputs& in) {
        Output o;
        const float dt = std::clamp(in.dt, 0.0f, 0.1f);
        if (!started_) {
            started_ = true;
            yaw_ = in.face ? in.faceYaw : in.moveYaw;
            speed_ = in.speed;
            slots_.clear();
            slots_.push_back({in.action, 0.0, float(in.actionRate), 1.0f, in.actionOnFeet});
            prevYaw_ = yaw_;
        }
        // ---- speed (start / stop) and acceleration
        const float prevSpeed = speed_;
        spring(speed_, speedVel_, std::max(0.0f, in.speed), in.player ? c.playerSpeedHalfLife : c.speedHalfLife, dt);
        speed_ = std::max(0.0f, speed_);
        const float accel = dt > 0 ? (speed_ - prevSpeed) / dt : 0.0f;
        accel_ = damp(accel_, accel, 0.08f, dt);
        // ---- facing
        const bool moving = in.speed > c.idleBelow && in.action < 0;
        const float turnRate = (in.player ? c.playerTurnRateDeg : c.turnRateDeg) * kDeg;
        if (moving) {
            springAngle(yaw_, yawVel_, in.moveYaw, in.player ? c.playerYawHalfLife : c.yawHalfLife, dt);
        } else if (in.face) {
            // on the spot: eased, but no faster than a person turns
            float before = yaw_;
            springAngle(yaw_, yawVel_, in.faceYaw, (in.player ? c.playerYawHalfLife : c.yawHalfLife) * 1.6f, dt);
            const float step = wrapPi(yaw_ - before), lim = turnRate * dt;
            if (std::fabs(step) > lim) {
                yaw_ = wrapPi(before + std::copysign(lim, step));
                yawVel_ = std::copysign(turnRate, step);
            }
        } else {
            yawVel_ = damp(yawVel_, 0.0f, 0.1f, dt);
        }
        const float yawRate = dt > 0 ? wrapPi(yaw_ - prevYaw_) / dt : 0.0f;
        prevYaw_ = yaw_;
        yawRate_ = damp(yawRate_, yawRate, 0.06f, dt);
        // ---- slots: locomotion (-1) or an act, faded
        const int want = in.action;
        Slot* target = nullptr;
        for (auto& s : slots_)
            if (s.clip == want) target = &s;
        if (!target) {
            if (slots_.size() >= 3) slots_.erase(slots_.begin());   // the oldest, furthest faded
            slots_.push_back({want, 0.0, float(in.actionRate), 0.0f, in.actionOnFeet});
            target = &slots_.back();
            if (want >= 0) target->time = 0;
        }
        const float fade = std::max(0.02f, in.player ? c.playerFadeS : c.fadeS);
        target->rate = float(in.actionRate);
        target->onFeet = in.actionOnFeet;
        for (auto& s : slots_) s.w = clamp01(s.w + (&s == target ? dt : -dt) / fade);
        slots_.erase(std::remove_if(slots_.begin(), slots_.end(), [&](const Slot& s) { return s.w <= 0 && &s != target; }),
                     slots_.end());
        float lin[3], shown[3];
        const int ns = int(slots_.size());
        for (int i = 0; i < ns; ++i) lin[i] = slots_[std::size_t(i)].w;
        normaliseFade(lin, ns, shown);
        // ---- locomotion blend at the shared phase
        const float locoSpeed = moving || in.action < 0 ? speed_ : 0.0f;
        const Pick pk = blendSpace(nodes_, locoSpeed, c.maxRate, c.idleBelow);
        const float pr = phaseRate(nodes_, pk);
        if (pr <= 0) phase_ = 0;   // the next start begins on the left heel
        phase_ = frac(phase_ + pr * dt);
        idleTime_ += dt;
        // turn on the spot: the turn loop, weighted by the share of its own speed
        float turnW = 0;
        if (c.turnInPlace && !moving && speed_ < 0.4f) turnW = clamp01(std::fabs(yawRate_) / (c.turnClipRateDeg * kDeg));
        turnW_ = damp(turnW_, turnW, 0.08f, dt);
        float feet = 0;
        for (int i = 0; i < ns; ++i) {
            Slot& s = slots_[std::size_t(i)];
            const float sw = shown[i];
            if (s.clip >= 0) {
                s.time += double(dt) * s.rate;
                push(o, {s.clip, s.time, sw, -1});
                if (s.onFeet) feet += sw;
                continue;
            }
            feet += sw;
            auto node = [&](int k, float w) {
                if (k < 0 || w <= 1e-4f) return;
                const Node& nd = nodes_[std::size_t(k)];
                if (k == 0) push(o, {nd.clip, idleTime_, w, -1});
                else push(o, {nd.clip, clipTime(nd.gait, phase_), w, phase_});
            };
            const float tw = (turnL_ >= 0 || turnR_ >= 0) && pk.a == 0 ? turnW_ : 0.0f;
            node(pk.a, sw * (pk.b < 0 ? 1.0f : 1.0f - pk.w) * (1 - tw));
            node(pk.b, sw * pk.w);
            if (tw > 1e-3f) {
                const int tc = yawRate_ > 0 ? (turnL_ >= 0 ? turnL_ : turnR_) : (turnR_ >= 0 ? turnR_ : turnL_);
                turnTime_ += double(dt) * std::max(0.6f, std::fabs(yawRate_) / (c.turnClipRateDeg * kDeg));
                push(o, {tc, turnTime_, sw * (pk.b < 0 ? 1.0f : 1.0f - pk.w) * tw, -1});
            }
        }
        o.feet = clamp01(feet);
        // ---- lean: into the curve, into the push, up the slope
        float fwd = 0, left = 0;
        if (c.lean) {
            const float movingW = clamp01(speed_ / std::max(0.1f, c.walkSpeed));
            left = std::clamp(yawRate_ * speed_ * c.leanTurn, -c.leanTurnMaxDeg * kDeg, c.leanTurnMaxDeg * kDeg);
            fwd = std::clamp(accel_ * c.leanAccel, -c.leanAccelMaxDeg * kDeg, c.leanAccelMaxDeg * kDeg) +
                  movingW * std::clamp(in.slopeAlong * c.leanSlope, -c.leanSlopeMaxDeg * kDeg, c.leanSlopeMaxDeg * kDeg);
            fwd *= o.feet, left *= o.feet;
        }
        spring(leanF_, leanFV_, fwd, c.leanHalfLife, dt);
        spring(leanL_, leanLV_, left, c.leanHalfLife, dt);
        o.leanFwd = leanF_, o.leanLeft = leanL_;
        // ---- look
        float lw = 0, ly = 0, lp = 0;
        if (c.look && in.look) {
            const Look l = clampLook(c, yaw_, in.lookYaw, in.lookPitch);
            if (l.keep) {
                lw = o.feet;
                // running, the head stays more on the way ahead
                const float runW = clamp01((speed_ - c.walkSpeed) / std::max(0.1f, c.runSpeed - c.walkSpeed));
                lw *= 1.0f - 0.6f * runW;
                ly = l.yaw, lp = l.pitch;
            }
        }
        const float lh = in.player ? c.playerLookHalfLife : c.lookHalfLife;
        if (lw > 0) {
            springAngle(lookY_, lookYV_, ly, lh, dt);
            spring(lookP_, lookPV_, lp, lh, dt);
        }
        lookW_ = damp(lookW_, lw, lh, dt);
        o.lookYaw = lookY_, o.lookPitch = lookP_, o.lookWeight = lookW_;
        // ---- breathing: slow at rest, quick and deep after a run
        const bool hard = speed_ > 0.8f * c.runSpeed;
        exert_ = clamp01(exert_ + (hard ? c.exertRise : -c.exertFall) * dt);
        const float period = c.breathPeriod + (c.exertPeriod - c.breathPeriod) * exert_;
        breathPhase_ = frac(breathPhase_ + dt / std::max(0.3f, period));
        if (c.breath) {
            const float amp = (c.breathDeg + (c.exertDeg - c.breathDeg) * exert_) * kDeg;
            const float rest = 1.0f - clamp01(speed_ / std::max(0.1f, c.walkSpeed));
            o.breath = amp * std::sin(2 * kPi * breathPhase_) * rest * o.feet;
        }
        o.yaw = yaw_;
        o.speed = speed_;
        o.turnRate = yawRate_;
        return o;
    }

private:
    struct Slot {
        int clip = -1;
        double time = 0;
        float rate = 1;
        float w = 0;
        bool onFeet = true;
    };
    static void push(Output& o, Layer l) {
        if (o.count < kMaxLayers && l.clip >= 0 && l.weight > 1e-4f) o.layers[std::size_t(o.count++)] = l;
    }
    std::vector<Node> nodes_;
    int turnL_ = -1, turnR_ = -1;
    bool started_ = false;
    std::vector<Slot> slots_;
    float speed_ = 0, speedVel_ = 0, accel_ = 0;
    float yaw_ = 0, yawVel_ = 0, prevYaw_ = 0, yawRate_ = 0;
    float phase_ = 0;
    double idleTime_ = 0, turnTime_ = 0;
    float turnW_ = 0;
    float leanF_ = 0, leanFV_ = 0, leanL_ = 0, leanLV_ = 0;
    float lookY_ = 0, lookYV_ = 0, lookP_ = 0, lookPV_ = 0, lookW_ = 0;
    float exert_ = 0, breathPhase_ = 0;
};

// ---------------------------------------------------------------- procedural pose
// The joints the layers act on, found by name (Config lists the candidates).
struct Rig {
    int com = -1, pelvis = -1, spine0 = -1, spine1 = -1, spine2 = -1, neck = -1, head = -1;
    int thigh[2] = {-1, -1}, calf[2] = {-1, -1}, foot[2] = {-1, -1};   // left, right
    float footRestZ[2] = {0.08f, 0.08f};   // the ankles' height standing in the rest pose
    // the bake's frame: z up, the figure faces -y, its left is +x
    V3 up{0, 0, 1}, forward{0, -1, 0}, left{1, 0, 0};
    bool ok = false;
};
template <class Find> Rig resolveRig(const Config& c, Find find) {
    auto one = [&](const std::vector<std::string>& names) {
        for (const auto& n : names)
            if (const int j = find(n); j >= 0) return j;
        return -1;
    };
    Rig r;
    r.com = one(c.com), r.pelvis = one(c.pelvis), r.spine0 = one(c.spine0), r.spine1 = one(c.spine1);
    r.spine2 = one(c.spine2), r.neck = one(c.neck), r.head = one(c.head);
    r.thigh[0] = one(c.thighL), r.calf[0] = one(c.calfL), r.foot[0] = one(c.footL);
    r.thigh[1] = one(c.thighR), r.calf[1] = one(c.calfR), r.foot[1] = one(c.footR);
    r.ok = r.com >= 0 && r.pelvis >= 0 && r.spine0 >= 0 && r.spine1 >= 0 && r.spine2 >= 0 && r.neck >= 0 &&
           r.head >= 0;
    for (int i = 0; i < 2; ++i) r.ok = r.ok && r.thigh[i] >= 0 && r.calf[i] >= 0 && r.foot[i] >= 0;
    return r;
}

// What the figure's procedural layers do this frame (from the animator's Output
// and the ground).
struct Procedural {
    bool on = false;
    float leanFwd = 0, leanLeft = 0;
    float lookYaw = 0, lookPitch = 0, lookWeight = 0;
    float breath = 0;
    float feet = 0;          // share of the pose standing on its feet (IK weight)
    float gx = 0, gy = 0;    // the ground's gradient in the figure's frame
};

// On the clip's world matrices (FK done, before skinning): lean, breathing,
// look, then the legs onto the ground. `anchor`: the clip's drift the palette
// takes off (the ground is under the figure's origin).
inline void applyProcedural(const Config& c, const Rig& r, const std::vector<int>& parent, float* world,
                            const Procedural& p, float anchorX, float anchorY, std::vector<char>& scratch) {
    if (!p.on || !r.ok) return;
    const int n = int(parent.size());
    const bool ik = c.footIk && p.feet > 0.01f;
    float gx = p.gx, gy = p.gy;
    const float gl = std::hypot(gx, gy);
    if (gl > c.maxSlope) gx *= c.maxSlope / gl, gy *= c.maxSlope / gl;
    // where the clip puts the ankles, and the ground under them
    V3 target[2];
    float ground[2] = {0, 0}, plant[2] = {0, 0};
    float footWas[2][16];   // the clip's foot frames: the feet keep them through lean and IK
    for (int i = 0; i < 2; ++i) {
        std::copy(world + std::size_t(r.foot[i]) * 16, world + std::size_t(r.foot[i]) * 16 + 16, footWas[i]);
        const V3 a = jointPos(world, r.foot[i]);
        ground[i] = ik ? planeHeight(gx, gy, {a.x - anchorX, a.y - anchorY, 0}) * p.feet : 0.0f;
        target[i] = a + V3{0, 0, ground[i]};
        plant[i] = plantWeight(a.z, r.footRestZ[i], c.footLift);
    }
    // lean: the whole body about the ground under the pelvis (the feet put back
    // by the IK), the rest bent at the waist
    const V3 L = r.forward * p.leanFwd + r.left * p.leanLeft;
    const float ang = length(L);
    if (ang > 1e-5f) {
        const V3 axis = cross(r.up, L * (1.0f / ang));
        const float body = ik ? std::clamp(c.leanBodyShare, 0.0f, 1.0f) : 0.0f;
        const V3 pel = jointPos(world, r.pelvis);
        if (body > 0) rotateSubtree(world, parent, r.com, axisAngle(axis, ang * body), {pel.x, pel.y, 0}, scratch);
        rotateSubtree(world, parent, r.spine0, axisAngle(axis, ang * (1 - body)), jointPos(world, r.spine0), scratch);
    }
    // breathing: the chest rises and settles
    if (std::fabs(p.breath) > 1e-6f)
        rotateSubtree(world, parent, r.spine1, axisAngle(r.left, -p.breath), jointPos(world, r.spine1), scratch);
    // look: chest, neck and head share the turn; the nod about the turned right axis
    if (p.lookWeight > 1e-3f) {
        const int chain[3] = {r.spine2, r.neck, r.head};
        float turned = 0;
        for (int k = 0; k < 3; ++k) {
            const float s = c.lookShare[std::size_t(k)] * p.lookWeight;
            const V3 piv = jointPos(world, chain[k]);
            turned += p.lookYaw * s;
            const Q yawQ = axisAngle(r.up, p.lookYaw * s);
            const V3 right = rotate(axisAngle(r.up, turned), r.left * -1.0f);
            const Q pitchQ = axisAngle(right, p.lookPitch * s);
            rotateSubtree(world, parent, chain[k], pitchQ * yawQ, piv, scratch);
        }
    }
    if (!ik) return;
    // the body down to the lower foot's ground, then each leg onto its own
    const float drop = pelvisDrop(ground[0], ground[1], c.pelvisMaxDrop);
    translateAll(world, n, {0, 0, drop});
    const V3 normal = planeNormal(gx, gy);
    const Q tilt = clampAngle(fromTo(r.up, normal), c.footAlignMaxDeg * kDeg);
    for (int i = 0; i < 2; ++i) {
        const V3 a = jointPos(world, r.thigh[i]), b = jointPos(world, r.calf[i]), f = jointPos(world, r.foot[i]);
        const TwoBone s = solveTwoBone(a, b, f, target[i], r.forward);
        rotateSubtree(world, parent, r.thigh[i], s.upper, a, scratch);
        rotateSubtree(world, parent, r.calf[i], s.lower, s.knee, scratch);
        const V3 ankle = jointPos(world, r.foot[i]);
        const Q back = turnBetween(footWas[i], world + std::size_t(r.foot[i]) * 16);
        const float k = plant[i] * p.feet;
        rotateSubtree(world, parent, r.foot[i], k > 1e-3f ? scaled(tilt, k) * back : back, ankle, scratch);
    }
}

} // namespace altera::game::loco
