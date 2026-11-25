// AL-79: the people's locomotion math (src/game_view/locomotion.hpp): blend
// space and step phase, crossfades, the animator's start / stop / turns / lean /
// look / breathing, the two-bone IK and the procedural pose on a toy skeleton.

#include <cmath>
#include <vector>

#include "framework.hpp"
#include "game_view/locomotion.hpp"
#include "sim/core.hpp"
#include "sim/worldgen.hpp"

using namespace lr::game::loco;

namespace {

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
bool near(V3 a, V3 b, float eps = 1e-3f) { return length(a - b) <= eps; }

std::vector<Node> nodes() {
    std::vector<Node> n(4);
    n[0].clip = 10;
    n[1].clip = 11, n[1].speed = 1.5f, n[1].gait = {1.1f, 0.25f, 1.5f, true};
    n[2].clip = 12, n[2].speed = 3.8f, n[2].gait = {0.7f, 0.6f, 3.8f, true};
    n[3].clip = 13, n[3].speed = 6.0f, n[3].gait = {0.55f, 0.1f, 6.0f, true};
    return n;
}

float weightOf(const Output& o, int clip) {
    float w = 0;
    for (int i = 0; i < o.count; ++i)
        if (o.layers[std::size_t(i)].clip == clip) w += o.layers[std::size_t(i)].weight;
    return w;
}
float total(const Output& o) {
    float w = 0;
    for (int i = 0; i < o.count; ++i) w += o.layers[std::size_t(i)].weight;
    return w;
}

// A toy person: root, com, pelvis, legs (knees a little forward), spine, neck, head.
struct Toy {
    std::vector<int> parent{-1, 0, 1, 2, 3, 4, 2, 6, 7, 1, 9, 10, 11, 12};
    std::vector<V3> pos{{0, 0, 0},      {0, 0, 1.0f},   {0, 0, 1.0f},  {0.1f, 0, 0.95f},  {0.1f, -0.06f, 0.52f},
                        {0.1f, 0, 0.08f}, {-0.1f, 0, 0.95f}, {-0.1f, -0.06f, 0.52f}, {-0.1f, 0, 0.08f},
                        {0, 0, 1.1f},   {0, 0, 1.25f},  {0, 0, 1.4f},  {0, 0, 1.55f},     {0, 0, 1.65f}};
    std::vector<float> world;
    Rig rig;
    Toy() {
        world.assign(parent.size() * 16, 0.0f);
        for (std::size_t j = 0; j < parent.size(); ++j) {
            float* m = &world[j * 16];
            m[0] = m[5] = m[10] = m[15] = 1;
            m[3] = pos[j].x, m[7] = pos[j].y, m[11] = pos[j].z;
        }
        rig.com = 1, rig.pelvis = 2, rig.thigh[0] = 3, rig.calf[0] = 4, rig.foot[0] = 5;
        rig.thigh[1] = 6, rig.calf[1] = 7, rig.foot[1] = 8;
        rig.spine0 = 9, rig.spine1 = 10, rig.spine2 = 11, rig.neck = 12, rig.head = 13;
        rig.footRestZ[0] = rig.footRestZ[1] = 0.08f;
        rig.ok = true;
    }
    V3 at(int j) const { return jointPos(world.data(), j); }
};

} // namespace

TEST(loco_spring_settles_without_overshoot) {
    float x = 0, v = 0, peak = 0;
    for (int i = 0; i < 200; ++i) {
        spring(x, v, 1.0f, 0.1f, 1.0f / 60);
        peak = std::max(peak, x);
    }
    CHECK(near(x, 1.0f, 1e-3f));
    CHECK(peak <= 1.0005f);
    // halfway after about one half-life (critically damped: a little later)
    x = 0, v = 0;
    for (int i = 0; i < 6; ++i) spring(x, v, 1.0f, 0.1f, 1.0f / 60);
    CHECK(x > 0.3f && x < 0.7f);
    float a = 3.0f, av = 0;   // the short way across +-pi
    for (int i = 0; i < 120; ++i) springAngle(a, av, -3.0f, 0.1f, 1.0f / 60);
    CHECK(near(a, -3.0f, 1e-2f));
}

TEST(loco_blend_space_weights) {
    const auto n = nodes();
    Pick p = blendSpace(n, 0.0f, 1.6f, 0.08f);
    CHECK_EQ(p.a, 0);
    CHECK_EQ(p.b, -1);
    p = blendSpace(n, 0.75f, 1.6f, 0.08f);   // half way out of idle into the walk
    CHECK_EQ(p.a, 0);
    CHECK_EQ(p.b, 1);
    CHECK(near(p.w, 0.5f));
    CHECK(near(p.rate, 1.0f));
    p = blendSpace(n, 1.5f, 1.6f, 0.08f);   // exactly the walk
    CHECK_EQ(p.a, 1);
    CHECK(near(p.w, 0.0f));
    p = blendSpace(n, 2.65f, 1.6f, 0.08f);   // walk / run mid way
    CHECK_EQ(p.a, 1);
    CHECK_EQ(p.b, 2);
    CHECK(near(p.w, 0.5f));
    p = blendSpace(n, 7.5f, 1.6f, 0.08f);   // over the sprint: it plays faster
    CHECK_EQ(p.a, 3);
    CHECK_EQ(p.b, -1);
    CHECK(near(p.rate, 1.25f));
    p = blendSpace(n, 50.0f, 1.6f, 0.08f);
    CHECK(near(p.rate, 1.6f));
}

TEST(loco_phase_keeps_the_left_heel_together) {
    const auto n = nodes();
    // walk-run half way: the beat between the two cycles
    const Pick p = blendSpace(n, 2.65f, 1.6f, 0.08f);
    CHECK(near(phaseRate(n, p), 0.5f / 1.1f + 0.5f / 0.7f));
    // at phase 0 both clips stand at their own left heel strike
    CHECK(near(float(clipTime(n[1].gait, 0.0f)), 0.25f * 1.1f));
    CHECK(near(float(clipTime(n[2].gait, 0.0f)), 0.6f * 0.7f));
    // half a cycle on: both at the right heel
    CHECK(near(float(clipTime(n[1].gait, 0.5f)), 0.75f * 1.1f));
    CHECK(near(float(clipTime(n[2].gait, 0.5f)), 0.1f * 0.7f));
    // standing: no beat
    CHECK(near(phaseRate(n, blendSpace(n, 0.0f, 1.6f, 0.08f)), 0.0f));
}

TEST(loco_gait_from_the_feet) {
    // a clip of 30 frames at 15 fps walking at 1.4 m/s; the left heel strikes at frame 9
    const float fps = 15, speed = 1.4f;
    const int n = 30;
    const float stride = speed * (float(n) / fps) / 2;   // two steps a cycle
    std::vector<V3> l(n), r(n);
    for (int i = 0; i < n; ++i) {
        const float ph = float(i - 9) / n;   // 0 at the left heel strike
        auto foot = [&](float q) {           // forward is -y: ahead = more negative y
            q = frac(q);
            V3 f;
            if (q < 0.5f) {   // stance: from ahead to behind at the travel speed
                f.y = -stride / 2 + stride * (q / 0.5f);
                f.z = 0.08f;
            } else {          // swing: back to ahead, lifted
                f.y = stride / 2 - stride * ((q - 0.5f) / 0.5f);
                f.z = 0.08f + 0.1f * std::sin((q - 0.5f) * 2 * kPi);
            }
            return f;
        };
        l[std::size_t(i)] = foot(ph);
        r[std::size_t(i)] = foot(ph + 0.5f);
        l[std::size_t(i)].x = 0.1f, r[std::size_t(i)].x = -0.1f;
    }
    const Gait g = analyseGait(l, r, fps, {0, -1, 0});
    CHECK(g.ok);
    CHECK(near(g.cycle, 2.0f));
    CHECK(near(g.offset, 9.0f / 30, 1.5f / 30));
    CHECK(near(g.speed, speed, 0.15f));
    // too short a clip: not measured
    CHECK(!analyseGait({{0, 0, 0}}, {{0, 0, 0}}, fps, {0, -1, 0}).ok);
}

TEST(loco_fade_weights_sum_to_one) {
    const float lin[3] = {0.2f, 0.9f, 0.5f};
    float out[3];
    normaliseFade(lin, 3, out);
    CHECK(near(out[0] + out[1] + out[2], 1.0f));
    CHECK(out[1] > out[2] && out[2] > out[0]);
    const float none[2] = {0, 0};
    normaliseFade(none, 2, out);
    CHECK(near(out[1], 1.0f));
    // eased: a fade's ends start and stop gently
    CHECK(smooth01(0.05f) < 0.05f);
    CHECK(smooth01(0.95f) > 0.95f);
}

TEST(loco_look_is_clamped_and_let_go_behind) {
    Config c;
    Look l = clampLook(c, 0.0f, 30 * kDeg, 10 * kDeg);
    CHECK(l.keep);
    CHECK(near(l.yaw, 30 * kDeg));
    l = clampLook(c, 0.0f, 100 * kDeg, 60 * kDeg);   // far round and high: clamped
    CHECK(l.keep);
    CHECK(near(l.yaw, 75 * kDeg));
    CHECK(near(l.pitch, 25 * kDeg));
    l = clampLook(c, 0.0f, 0.0f, -80 * kDeg);
    CHECK(near(l.pitch, -35 * kDeg));
    CHECK(!clampLook(c, 0.0f, 170 * kDeg, 0).keep);   // behind
    // across +-pi: the body at 170 deg, the target at -170: 20 deg to the left
    l = clampLook(c, 170 * kDeg, -170 * kDeg, 0);
    CHECK(l.keep && near(l.yaw, 20 * kDeg));
}

TEST(loco_two_bone_ik_reaches_and_keeps_lengths) {
    const V3 a{0, 0, 1}, b{0, -0.03f, 0.55f}, c{0, 0, 0.1f};
    const float l1 = length(b - a), l2 = length(c - b);
    for (V3 t : {V3{0, 0, 0.3f}, V3{0.1f, -0.1f, 0.25f}, V3{0, 0.05f, 0.4f}}) {
        const TwoBone s = solveTwoBone(a, b, c, t, {0, -1, 0});
        CHECK(s.reached);
        CHECK(near(length(s.knee - a), l1));
        const V3 c1 = a + rotate(s.upper, c - a);
        const V3 c2 = s.knee + rotate(s.lower, c1 - s.knee);
        CHECK(near(c2, t));
        CHECK(near(length(c2 - s.knee), l2));
        CHECK(s.knee.y < 0);   // the knee still bends forward (-y)
    }
    // out of reach: the leg straight towards it
    const TwoBone far = solveTwoBone(a, b, c, {0, 0, -1}, {0, -1, 0});
    CHECK(!far.reached);
    const V3 c1 = a + rotate(far.upper, c - a);
    const V3 c2 = far.knee + rotate(far.lower, c1 - far.knee);
    CHECK(near(length(c2 - a), l1 + l2, 2e-3f));
    // straight leg: the pole decides the bend
    const TwoBone st = solveTwoBone({0, 0, 1}, {0, 0, 0.5f}, {0, 0, 0}, {0, 0, 0.2f}, {0, -1, 0});
    CHECK(st.knee.y < -0.1f);
}

TEST(loco_rotate_subtree_moves_children_only) {
    Toy t;
    std::vector<char> scratch;
    const V3 headBefore = t.at(t.rig.head), footBefore = t.at(t.rig.foot[0]);
    // turn the neck 90 degrees about up: the head is right above, stays; tilt instead
    rotateSubtree(t.world.data(), t.parent, t.rig.neck, axisAngle({1, 0, 0}, kPi / 2), t.at(t.rig.neck), scratch);
    const V3 h = t.at(t.rig.head);
    CHECK(near(h, {0, -0.1f, 1.55f}));   // 10 cm up the neck now points -y
    CHECK(near(t.at(t.rig.foot[0]), footBefore));
    CHECK(!near(h, headBefore));
}

TEST(loco_ground_plane_in_the_figure_frame) {
    // world gradient (0.2, 0): the ground rises to +x; a figure turned 90 degrees
    float mx, my;
    gradientToModel(0.2f, 0.0f, kPi / 2, mx, my);
    // model point p maps to world R(90) p: model (0, -1) -> world (1, 0): 0.2 higher
    CHECK(near(planeHeight(mx, my, {0, -1, 0}), 0.2f));
    CHECK(near(planeHeight(mx, my, {1, 0, 0}), 0.0f));
    CHECK(near(pelvisDrop(0.05f, -0.07f, 0.28f), -0.07f));
    CHECK(near(pelvisDrop(0.05f, 0.02f, 0.28f), 0.0f));   // both higher: the legs bend, the body stays
    CHECK(near(pelvisDrop(-0.5f, 0.0f, 0.28f), -0.28f));
    CHECK(near(plantWeight(0.08f, 0.08f, 0.12f), 1.0f));
    CHECK(near(plantWeight(0.20f, 0.08f, 0.12f), 0.0f));
}

TEST(loco_feet_on_a_slope) {
    Config c;
    Toy t;
    std::vector<char> scratch;
    Procedural p;
    p.on = true, p.feet = 1;
    p.gx = 0.3f, p.gy = 0;   // the ground rises to +x (the left foot's side)
    applyProcedural(c, t.rig, t.parent, t.world.data(), p, 0, 0, scratch);
    const V3 l = t.at(t.rig.foot[0]), r = t.at(t.rig.foot[1]);
    CHECK(near(l.z, 0.08f + 0.03f, 2e-3f));   // each ankle at its ground (+-0.1 m x 0.3)
    CHECK(near(r.z, 0.08f - 0.03f, 2e-3f));
    CHECK(near(t.at(t.rig.pelvis).z, 1.0f - 0.03f, 1e-3f));   // the body down to the lower foot
    // the planted foot laid on the slope: its up turned towards the plane's normal
    const float* m = &t.world[std::size_t(t.rig.foot[0]) * 16];
    const V3 footUp{m[2], m[6], m[10]};
    CHECK(near(footUp, planeNormal(0.3f, 0), 1e-3f));
    // flat ground: nothing moves
    Toy flat;
    Procedural q;
    q.on = true, q.feet = 1;
    applyProcedural(c, flat.rig, flat.parent, flat.world.data(), q, 0, 0, scratch);
    Toy ref;
    for (std::size_t j = 0; j < ref.parent.size(); ++j) CHECK(near(flat.at(int(j)), ref.at(int(j)), 1e-4f));
    // switched off: the slope is ignored
    Config off;
    off.footIk = false;
    Toy t2;
    applyProcedural(off, t2.rig, t2.parent, t2.world.data(), p, 0, 0, scratch);
    CHECK(near(t2.at(t2.rig.foot[0]).z, 0.08f, 1e-4f));
}

TEST(loco_look_and_lean_on_the_toy) {
    Config c;
    Toy t;
    std::vector<char> scratch;
    Procedural p;
    p.on = true, p.feet = 1;
    p.lookYaw = 60 * kDeg, p.lookWeight = 1;
    applyProcedural(c, t.rig, t.parent, t.world.data(), p, 0, 0, scratch);
    // the head's forward (-y) turned 60 deg to the left (+x side)
    const float* m = &t.world[std::size_t(t.rig.head) * 16];
    const V3 fwd{-m[1], -m[5], -m[9]};
    CHECK(near(fwd, {std::sin(60 * kDeg), -std::cos(60 * kDeg), 0}, 2e-3f));
    CHECK(near(t.at(t.rig.foot[0]), {0.1f, 0, 0.08f}, 1e-4f));   // the legs untouched
    // lean left: the head goes to +x, the feet stay where they stood
    Toy l;
    Procedural q;
    q.on = true, q.feet = 1, q.leanLeft = 10 * kDeg;
    applyProcedural(c, l.rig, l.parent, l.world.data(), q, 0, 0, scratch);
    CHECK(l.at(l.rig.head).x > 0.1f);
    CHECK(near(l.at(l.rig.foot[0]), {0.1f, 0, 0.08f}, 2e-3f));
    CHECK(near(l.at(l.rig.foot[1]), {-0.1f, 0, 0.08f}, 2e-3f));
}

TEST(loco_animator_starts_and_stops_smoothly) {
    Config c;
    Animator npc, pl;
    npc.setup(nodes(), 20, 21);
    pl.setup(nodes(), 20, 21);
    Inputs in;
    in.dt = 1.0f / 60;
    npc.update(c, in);
    pl.update(c, in);
    in.speed = 1.5f;
    Output on = npc.update(c, in), op;
    in.player = true;
    op = pl.update(c, in);
    // one frame in: neither walks at full speed, the player's person is further on
    CHECK(on.speed < 1.0f);
    CHECK(op.speed > on.speed);
    in.player = false;
    float prevLean = 0;
    for (int i = 0; i < 6; ++i) on = npc.update(c, in);
    prevLean = on.leanFwd;
    CHECK(prevLean > 0);   // the push: leaning into it
    for (int i = 0; i < 60; ++i) on = npc.update(c, in);
    CHECK(near(on.speed, 1.5f, 0.01f));
    CHECK(near(weightOf(on, 11), 1.0f, 0.02f));   // all walk
    CHECK(near(total(on), 1.0f, 1e-3f));
    // stop: the walk fades out through the slower blend, leaning back
    in.speed = 0;
    on = npc.update(c, in);
    for (int i = 0; i < 4; ++i) on = npc.update(c, in);
    CHECK(on.leanFwd < prevLean);
    CHECK(weightOf(on, 11) > 0.0f && weightOf(on, 10) > 0.0f);
    for (int i = 0; i < 90; ++i) on = npc.update(c, in);
    CHECK(near(weightOf(on, 10), 1.0f, 1e-3f));
}

TEST(loco_animator_crossfades_acts) {
    Config c;
    Animator a;
    a.setup(nodes(), -1, -1);
    Inputs in;
    in.dt = 1.0f / 60;
    a.update(c, in);
    in.action = 40;
    Output o = a.update(c, in);
    CHECK(weightOf(o, 40) > 0.0f && weightOf(o, 40) < 0.1f);
    CHECK(near(total(o), 1.0f, 1e-3f));
    for (int i = 0; i < 9; ++i) o = a.update(c, in);   // ~half the fade
    CHECK(weightOf(o, 40) > 0.3f && weightOf(o, 40) < 0.7f);
    CHECK(near(total(o), 1.0f, 1e-3f));
    // a second act while the first still fades in: three slots, still one in sum
    in.action = 41;
    in.actionOnFeet = false;   // sitting down: the feet leave the IK
    o = a.update(c, in);
    CHECK(weightOf(o, 10) > 0 && weightOf(o, 40) > 0 && weightOf(o, 41) > 0);
    CHECK(near(total(o), 1.0f, 1e-3f));
    for (int i = 0; i < 60; ++i) o = a.update(c, in);
    CHECK(near(weightOf(o, 41), 1.0f, 1e-3f));
    CHECK(near(o.feet, 0.0f, 1e-3f));
    // the act's own clock runs from its start
    for (int i = 0; i < o.count; ++i)
        if (o.layers[std::size_t(i)].clip == 41) CHECK(near(float(o.layers[std::size_t(i)].time), 61.0f / 60, 1e-3f));
}

TEST(loco_animator_turns_on_the_spot_at_a_limited_rate) {
    Config c;
    Animator a;
    a.setup(nodes(), 20, 21);
    Inputs in;
    in.dt = 1.0f / 60;
    in.face = true, in.faceYaw = 0;
    a.update(c, in);
    in.faceYaw = kPi * 0.9f;   // round to the left
    float maxRate = 0, turnW = 0;
    Output o;
    for (int i = 0; i < 30; ++i) {
        o = a.update(c, in);
        maxRate = std::max(maxRate, o.turnRate);
        turnW = std::max(turnW, weightOf(o, 20));
    }
    CHECK(maxRate <= c.turnRateDeg * kDeg * 1.01f);
    CHECK(maxRate > 1.0f);
    CHECK(turnW > 0.3f);              // the left turn loop played
    CHECK(near(weightOf(o, 21), 0));  // not the right one
    for (int i = 0; i < 120; ++i) o = a.update(c, in);
    CHECK(near(o.yaw, kPi * 0.9f, 0.02f));
    CHECK(weightOf(o, 20) < 0.05f);   // turned: the loop has faded
    // without the turn: no loop
    Config off;
    off.turnInPlace = false;
    Animator b;
    b.setup(nodes(), 20, 21);
    in.faceYaw = 0;
    b.update(off, in);
    in.faceYaw = 2.0f;
    for (int i = 0; i < 30; ++i) o = b.update(off, in);
    CHECK(near(weightOf(o, 20), 0.0f));
}

TEST(loco_animator_leans_into_curves) {
    Config c;
    Animator a;
    a.setup(nodes(), -1, -1);
    Inputs in;
    in.dt = 1.0f / 60;
    in.speed = 3.8f;
    Output o;
    for (int i = 0; i < 60; ++i) o = a.update(c, in);   // running straight along +x
    CHECK(std::fabs(o.leanLeft) < 1e-3f);
    for (int i = 0; i < 40; ++i) {   // a curve to the left (yaw rising)
        in.moveYaw += 1.5f / 60;
        o = a.update(c, in);
    }
    CHECK(o.leanLeft > 3 * kDeg);
    CHECK(o.leanLeft <= c.leanTurnMaxDeg * kDeg + 1e-4f);
    // uphill: forward
    in.slopeAlong = 0.3f;
    for (int i = 0; i < 60; ++i) o = a.update(c, in);
    CHECK(o.leanFwd > 2 * kDeg);
    // off
    Config off;
    off.lean = false;
    Animator b;
    b.setup(nodes(), -1, -1);
    for (int i = 0; i < 60; ++i) {
        in.moveYaw += 1.5f / 60;
        o = b.update(off, in);
    }
    CHECK(near(o.leanLeft, 0) && near(o.leanFwd, 0));
}

TEST(loco_animator_looks_and_breathes) {
    Config c;
    Animator a;
    a.setup(nodes(), -1, -1);
    Inputs in;
    in.dt = 1.0f / 60;
    in.look = true, in.lookYaw = 50 * kDeg, in.lookPitch = 0.1f;
    Output o = a.update(c, in);
    CHECK(o.lookWeight < 0.2f);   // eased in, not snapped
    float breathMax = 0;
    for (int i = 0; i < 300; ++i) {
        o = a.update(c, in);
        breathMax = std::max(breathMax, std::fabs(o.breath));
    }
    CHECK(near(o.lookWeight, 1.0f, 0.01f));
    CHECK(near(o.lookYaw, 50 * kDeg, 0.01f));
    CHECK(breathMax > 0.5f * c.breathDeg * kDeg && breathMax <= c.breathDeg * kDeg * 1.01f);
    // behind them: let go
    in.lookYaw = kPi;
    for (int i = 0; i < 120; ++i) o = a.update(c, in);
    CHECK(o.lookWeight < 0.01f);
    // walking: no breathing sway
    in.speed = 1.5f, in.moveYaw = 0;
    for (int i = 0; i < 120; ++i) o = a.update(c, in);
    CHECK(std::fabs(o.breath) < 1e-4f);
}

TEST(loco_config_reads_the_content_file) {
    Config c;
    const auto j = lr::sim::loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/animations/locomotion.json");
    readConfig(c, j);
    CHECK(c.look && c.footIk && c.lean && c.breath && c.turnInPlace && c.react);
    CHECK(near(c.lookShare[0] + c.lookShare[1] + c.lookShare[2], 1.0f));
    CHECK(c.playerFadeS < c.fadeS);
    CHECK(c.playerSpeedHalfLife < c.speedHalfLife);
    const auto off = lr::sim::Json::parse(R"({"foot_ik": false, "look_yaw_max_deg": 40, "thigh_l_joint": ["x"]})");
    readConfig(c, off);
    CHECK(!c.footIk);
    CHECK(near(c.lookYawMaxDeg, 40));
    CHECK_EQ(c.thighL.size(), std::size_t(1));
    CHECK(offFeet(c, "sky__idlesit_crosslegged"));
    CHECK(!offFeet(c, "sky__mt_walkforward"));
    // rig by names
    Config d;
    const std::vector<std::string> names{"NPC COM [COM ]", "NPC Pelvis [Pelv]", "NPC L Thigh [LThg]", "NPC L Calf [LClf]",
                                         "NPC L Foot [Lft ]", "NPC R Thigh [RThg]", "NPC R Calf [RClf]",
                                         "NPC R Foot [Rft ]", "NPC Spine [Spn0]", "NPC Spine1 [Spn1]",
                                         "NPC Spine2 [Spn2]", "NPC Neck [Neck]", "NPC Head [Head]"};
    auto find = [&](const std::string& n) {
        for (std::size_t i = 0; i < names.size(); ++i)
            if (names[i] == n) return int(i);
        return -1;
    };
    const Rig r = resolveRig(d, find);
    CHECK(r.ok);
    CHECK_EQ(r.foot[1], 7);
    CHECK(!resolveRig(d, [](const std::string&) { return -1; }).ok);
}
