#include "native/fight2d.hpp"

#include <algorithm>
#include <cmath>

namespace game2d {

namespace {

constexpr double kPi = 3.14159265358979323846;

double ease(double u) {
    u = std::clamp(u, 0.0, 1.0);
    return u * u * (3 - 2 * u);
}

double num(const Json& j, const char* k, double d) {
    if (!j.is_object() || !j.contains(k) || !j[k].is_number()) return d;
    return j[k].get<double>();
}

std::string str(const Json& j, const char* k) {
    if (!j.is_object() || !j.contains(k) || !j[k].is_string()) return "";
    return j[k].get<std::string>();
}

long long ident(const Json& j, const char* k) {
    if (!j.is_object() || !j.contains(k) || !j[k].is_number()) return -1;
    return j[k].get<long long>();
}

ui::Colour colourOf(const Json& j, const char* k, const char* d) {
    const std::string s = str(j, k);
    return pal::hex(s.empty() ? d : s.c_str());
}

char faceOf(double fx, double fy) {
    return std::abs(fx) >= std::abs(fy) ? (fx >= 0 ? 'E' : 'W') : (fy >= 0 ? 'S' : 'N');
}

} // namespace

void Fight2d::configure(const Json& data) { D_ = data.is_object() ? data : Json::object(); }

void Fight2d::clear() {
    shows_.clear();
    parts_.clear();
    floats_.clear();
}

double Fight2d::rnd(std::uint32_t& s) const {
    s = s * 1664525u + 1013904223u;
    return double(s >> 8) / double(1u << 24);
}

double Fight2d::phase(const char* k, double d) const { return num(D_.value("phase", Json::object()), k, d); }

double Fight2d::add(const Json& fight, double nowMs, long long me, const std::string& nameA, const std::string& nameB) {
    if (!fight.is_object()) return 0;
    const long long id = ident(fight, "id");
    for (const Show& s : shows_)
        if (s.id == id) return 0;
    Show s;
    s.id = id;
    s.a = ident(fight, "a");
    s.b = ident(fight, "b");
    s.mine = me >= 0 && (s.a == me || s.b == me);
    s.nameA = nameA;
    s.nameB = nameB;
    s.seed = std::uint32_t(id * 2654435761u + 7u);
    // the stance: face to face around the middle of where the two stood
    double ax = 0, ay = 0, bx = 0, by = 0;
    if (fight.contains("pos_a") && fight["pos_a"].is_array() && fight["pos_a"].size() >= 2) {
        ax = fight["pos_a"][0].get<double>();
        ay = fight["pos_a"][1].get<double>();
    }
    if (fight.contains("pos_b") && fight["pos_b"].is_array() && fight["pos_b"].size() >= 2) {
        bx = fight["pos_b"][0].get<double>();
        by = fight["pos_b"][1].get<double>();
    }
    double dx = bx - ax, dy = by - ay, d = std::hypot(dx, dy);
    if (d < 1e-3) dx = 1, dy = 0, d = 1;
    dx /= d, dy /= d;
    const double mx = (ax + bx) / 2, my = (ay + by) / 2, half = num(D_, "spacing_m", 1.15) / 2;
    Fighter A, B;
    A.x = mx - dx * half, A.y = my - dy * half, A.fx = dx, A.fy = dy, A.weapon = str(fight, "weapon_a");
    B.x = mx + dx * half, B.y = my + dy * half, B.fx = -dx, B.fy = -dy, B.weapon = str(fight, "weapon_b");
    s.f[s.a] = A;
    s.f[s.b] = B;
    // rounds -> exchanges: each attack / clinch / shot opens one; the rest rides on it
    const Json& ev = fight.contains("ev") && fight["ev"].is_array() ? fight["ev"] : Json::array();
    std::vector<std::vector<Exchange>> rounds;
    int lastR = -1000000;
    for (const Json& e : ev) {
        const int r = int(num(e, "r", 0));
        const std::string k = str(e, "k");
        if (k == "end") continue;
        if (r != lastR || rounds.empty()) {
            rounds.emplace_back();
            lastR = r;
        }
        auto& list = rounds.back();
        if (k == "attack" || k == "clinch" || k == "shoot" || k == "projectile_hit" || k == "projectile_miss") {
            Exchange x;
            x.kind = k == "attack" ? "attack" : (k == "clinch" ? "clinch" : "shoot");
            x.att = ident(e, "a");
            x.def = ident(e, "t");
            x.dir = str(e, "dir");
            x.zone = str(e, "zone");
            x.weapon = str(e, "weapon");
            x.riposte = num(e, "riposte", 0) > 0;
            x.stamina = num(e, "stamina", -1);
            x.move = str(e, "move");
            x.clinchOk = num(e, "ok", 0) > 0;
            if (k == "projectile_hit") x.out = "arrow_hit";
            if (k == "projectile_miss") x.out = "arrow_miss";
            list.push_back(std::move(x));
            continue;
        }
        if (list.empty()) {   // a state or a breath before any blow of the round
            Exchange x;
            x.kind = "other";
            x.att = ident(e, "a");
            list.push_back(std::move(x));
        }
        Exchange& x = list.back();
        if (k == "result") {
            x.out = str(e, "out");
            if (x.zone.empty()) x.zone = str(e, "zone");
        } else if (k == "wound") {
            x.sev = str(e, "sev");
            x.after.push_back(e);
        } else if (k != "guard")
            x.after.push_back(e);
    }
    const int n = int(rounds.size());
    s.rounds = n;
    double roundMs = std::max(num(D_, "min_round_ms", 900), num(fight, "round_ms", 1500) * num(D_, "round_ms_mult", 1.0));
    const int maxShown = std::max(1, int(num(D_, "max_rounds_shown", 40)));
    if (n > maxShown) roundMs = std::max(250.0, roundMs * maxShown / n);
    s.roundMs = roundMs;
    double t = num(D_, "intro_ms", 600);
    for (auto& list : rounds) {
        const double slot = roundMs / std::max<std::size_t>(1, list.size());
        for (auto& x : list) {
            x.t0 = t;
            x.t1 = t + slot;
            t += slot;
            s.ex.push_back(std::move(x));
        }
    }
    s.length = t + num(D_, "tail_ms", 1600);
    s.start = nowMs;
    shows_.push_back(std::move(s));
    while (shows_.size() > 12) shows_.pop_front();
    const Show& b = shows_.back();
    return b.mine && D_.value("hold_world_for_me", true) ? b.length : 0.0;
}

const Fight2d::Exchange* Fight2d::current(const Show& s, double t) const {
    for (const Exchange& e : s.ex)
        if (t >= e.t0 && t < e.t1) return &e;
    return nullptr;
}

bool Fight2d::active(double nowMs) const {
    for (const Show& s : shows_)
        if (nowMs < s.start + s.length) return true;
    return false;
}

bool Fight2d::mineActive(double nowMs) const {
    for (const Show& s : shows_)
        if (s.mine && nowMs < s.start + s.length) return true;
    return false;
}

void Fight2d::burst(Show& s, const std::string& effect, double x, double y, double nowMs, double mult) {
    const Json& fx = D_.value("effects", Json::object());
    if (!fx.contains(effect)) return;
    const Json& e = fx[effect];
    const std::string kind = str(e, "particles");
    const Json& P = D_.value("particles", Json::object());
    if (kind.empty() || !P.contains(kind)) return;
    const Json& p = P[kind];
    auto range = [&](const char* k, double a, double b) {
        if (p.contains(k) && p[k].is_array() && p[k].size() >= 2) return std::pair{p[k][0].get<double>(), p[k][1].get<double>()};
        return std::pair{a, b};
    };
    const auto [s0, s1] = range("speed", 1, 3);
    const auto [l0, l1] = range("life", 300, 700);
    const auto [z0, z1] = range("size", 1, 2);
    const Json cols = p.value("colours", Json::array({"#ffffff"}));
    const int count = int(std::round(num(e, "count", 6) * mult));
    for (int i = 0; i < count; ++i) {
        const double ang = rnd(s.seed) * 2 * kPi, sp = s0 + (s1 - s0) * rnd(s.seed);
        Particle q;
        q.x = x, q.y = y, q.z = 0.55 + 0.2 * rnd(s.seed);
        q.vx = std::cos(ang) * sp, q.vy = std::sin(ang) * sp * 0.6, q.vz = (0.3 + rnd(s.seed)) * sp * 0.6;
        q.born = nowMs, q.life = l0 + (l1 - l0) * rnd(s.seed), q.size = std::round(z0 + (z1 - z0) * rnd(s.seed));
        const std::size_t ci = cols.empty() ? 0 : std::size_t(rnd(s.seed) * double(cols.size())) % cols.size();
        q.c = cols.empty() ? ui::Colour(1, 1, 1, 1) : pal::hex(cols[ci].get<std::string>().c_str(), float(num(p, "alpha", 1.0)));
        q.glow = p.value("glow", false);
        q.stainUntil = kind == "blood" ? nowMs + num(p, "stain_ms", 0) : 0;
        q.vz -= 0;   // gravity in tick
        parts_.push_back(q);
    }
    (void)s;
}

void Fight2d::say(const std::string& effect, double x, double y, double nowMs, const std::string& text) {
    const Json& fx = D_.value("effects", Json::object());
    if (!fx.contains(effect)) return;
    const Json& e = fx[effect];
    const std::string key = text.empty() ? str(e, "text") : text;
    if (key.empty()) return;
    floats_.push_back({x, y, nowMs, key, colourOf(e, "text_colour", "#ffffff")});
}

void Fight2d::fire(Show& s, Exchange& e, double nowMs) {
    e.fired = true;
    const double t = nowMs - s.start;
    auto at = [&](long long id) -> Fighter* {
        auto it = s.f.find(id);
        return it == s.f.end() ? nullptr : &it->second;
    };
    Fighter* A = at(e.att);
    Fighter* B = at(e.def);
    if (A && e.stamina >= 0) A->stamina = e.stamina;
    if (A && e.riposte) say("riposte", A->x, A->y - 0.35, nowMs);
    const Json& fx = D_.value("effects", Json::object());
    if (B && (e.kind == "attack" || e.kind == "shoot") && !e.out.empty()) {
        const double hx = (A ? (A->x + B->x) / 2 : B->x), hy = (A ? (A->y + B->y) / 2 : B->y);
        const bool onBody = e.out == "hit" || e.out == "arrow_hit" || e.out == "dodge";
        const double px = onBody ? B->x : hx, py = onBody ? B->y : hy;
        const double mult = e.out == "hit" && !e.sev.empty() ? num(D_.value("wound_blood", Json::object()), e.sev.c_str(), 1.0)
                                                             : (e.out == "hit" ? 0.5 : 1.0);
        burst(s, e.out, px, py, nowMs, mult);
        if (e.out == "hit" || e.out == "arrow_hit") {
            const std::string zk = "ui2d.fight.zone." + e.zone;
            floats_.push_back({B->x, B->y, nowMs, "@hit:" + zk, colourOf(fx.value(e.out, Json::object()), "text_colour", "#ff6050")});
        } else
            say(e.out, px, py, nowMs);
        if (fx.contains(e.out) && fx[e.out].contains("flash")) {
            Fighter* who = (e.out == "hit" || e.out == "arrow_hit") ? B : B;
            who->flashAt = t;
            who->flashColour = colourOf(fx[e.out], "flash", "#ff3020");
        }
    }
    if (e.kind == "clinch" && A && B) {
        burst(s, "clinch", (A->x + B->x) / 2, (A->y + B->y) / 2, nowMs);
        say(e.clinchOk ? "clinch_ok" : "clinch", (A->x + B->x) / 2, (A->y + B->y) / 2 - 0.2, nowMs);
    }
    for (const Json& x : e.after) {
        const std::string k = str(x, "k");
        Fighter* W = at(ident(x, "a"));
        if (!W) continue;
        if (k == "wound") {
            ++W->wounds;
        } else if (k == "exhausted") {
            burst(s, "exhausted", W->x, W->y, nowMs);
            say("exhausted", W->x, W->y, nowMs);
        } else if (k == "wavering") {
            say("wavering", W->x, W->y, nowMs);
        } else if (k == "weapon_break") {
            burst(s, "weapon_break", W->x, W->y, nowMs);
            say("weapon_break", W->x, W->y, nowMs);
        } else if (k == "state") {
            const std::string st = str(x, "state");
            if (st.empty() || st == "fight" || st == W->state) continue;
            W->state = st;
            W->stateAt = t;
            burst(s, st, W->x, W->y, nowMs);
            say(st, W->x, W->y, nowMs);
        } else if (k == "riposte") {
            say("riposte", W->x, W->y - 0.35, nowMs);
        } else if (k == "close_in") {
            burst(s, "clinch", W->x, W->y, nowMs, 0.5);
        } else if (k == "get_up") {
            W->state = "fight";
            W->stateAt = t;
        }
    }
}

void Fight2d::tick(double nowMs, double dt) {
    for (Show& s : shows_) {
        const double t = nowMs - s.start;
        for (Exchange& e : s.ex) {
            if (e.fired) continue;
            const double hit = e.t0 + (e.t1 - e.t0) * phase("strike", 0.5);
            if (t >= hit) fire(s, e, nowMs);
            else break;
        }
    }
    while (!shows_.empty() && nowMs > shows_.front().start + shows_.front().length + 500) shows_.pop_front();
    const Json& P = D_.value("particles", Json::object());
    (void)P;
    for (Particle& p : parts_) {
        if (p.z <= 0) continue;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.z += p.vz * dt;
        p.vz -= 6.0 * dt * (p.glow ? 0.3 : 1.0);
        if (p.z <= 0) p.z = 0, p.vx = p.vy = 0;
    }
    parts_.erase(std::remove_if(parts_.begin(), parts_.end(),
                                [&](const Particle& p) {
                                    const bool alive = nowMs - p.born < p.life;
                                    return !alive && !(p.stainUntil > nowMs && p.z <= 0);
                                }),
                 parts_.end());
    const double life = num(D_.value("float_text", Json::object()), "life_ms", 1100);
    floats_.erase(std::remove_if(floats_.begin(), floats_.end(), [&](const Floater& f) { return nowMs - f.born > life; }),
                  floats_.end());
}

bool Fight2d::pose(long long id, double nowMs, FightPose& out) const {
    for (auto it = shows_.rbegin(); it != shows_.rend(); ++it) {
        const Show& s = *it;
        const double t = nowMs - s.start;
        if (t < 0 || t > s.length) continue;
        const auto fi = s.f.find(id);
        if (fi == s.f.end()) continue;
        const Fighter& F = fi->second;
        double x = F.x, y = F.y;
        const double sway = num(D_, "sway_m", 0.05) * std::sin(nowMs / std::max(1.0, num(D_, "sway_ms", 900)) * 2 * kPi + double(id % 7));
        // a little side to side in the stance
        x += -F.fy * sway;
        y += F.fx * sway;
        const Exchange* e = current(s, t);
        if (e && F.state == "fight") {
            const double u = (t - e->t0) / std::max(1.0, e->t1 - e->t0);
            const double W = phase("windup", 0.34), S = phase("strike", 0.5);
            if (e->kind == "attack" && e->att == id) {
                double k = 0;
                if (u < W) k = -num(D_, "lean_back_m", 0.12) * ease(u / W);
                else if (u < S) k = -num(D_, "lean_back_m", 0.12) + (num(D_, "lunge_m", 0.38) + num(D_, "lean_back_m", 0.12)) * ease((u - W) / (S - W));
                else k = num(D_, "lunge_m", 0.38) * (1 - ease((u - S) / (1 - S)));
                x += F.fx * k, y += F.fy * k;
            } else if (e->kind == "attack" && e->def == id) {
                if (e->out == "dodge" && u >= W) {
                    const double side = (s.seed + std::uint32_t(e->t0)) % 2 ? 1.0 : -1.0;
                    const double k = num(D_, "dodge_m", 0.5) * std::sin(kPi * std::clamp((u - W) / (1 - W), 0.0, 1.0));
                    x += -F.fy * k * side, y += F.fx * k * side;
                } else if (u >= S) {
                    const double back = e->out == "hit" ? num(D_, "recoil_m", 0.28) : num(D_, "push_m", 0.12);
                    const double k = back * (1 - ease((u - S) / (1 - S)));
                    x -= F.fx * k, y -= F.fy * k;
                }
            } else if (e->kind == "clinch" && (e->att == id || e->def == id)) {
                const double k = num(D_, "clinch_m", 0.42) * std::sin(kPi * std::clamp(u, 0.0, 1.0));
                x += F.fx * k, y += F.fy * k;
                x += 0.04 * std::sin(nowMs * 0.05 + double(id));
            }
        }
        out = FightPose{};
        out.face = faceOf(F.fx, F.fy);
        const double fallMs = num(D_, "fall_ms", 450);
        if (F.state == "ko" || F.state == "dead") out.lie = std::clamp((t - F.stateAt) / fallMs, 0.0, 1.0);
        else if (F.state == "yield") out.kneel = std::clamp((t - F.stateAt) / fallMs, 0.0, 1.0);
        else if (F.state == "flee") {
            const double k = num(D_, "flee_m", 4.0) * ease((t - F.stateAt) / (fallMs * 3));
            x -= F.fx * k, y -= F.fy * k;
            out.face = faceOf(-F.fx, -F.fy);
        }
        const double flashMs = num(D_, "flash_ms", 260);
        if (t - F.flashAt < flashMs) {
            out.flash = 1 - (t - F.flashAt) / flashMs;
            out.flashColour = F.flashColour;
        }
        out.x = x, out.y = y;
        return true;
    }
    return false;
}

void Fight2d::line(Ctx& c, double x0, double y0, double x1, double y1, float w, const ui::Colour& col) {
    const double d = std::hypot(x1 - x0, y1 - y0);
    const int n = std::max(1, int(std::ceil(d / std::max(1.0f, w * 0.7f))));
    for (int i = 0; i <= n; ++i) {
        const double k = double(i) / n;
        const float px = float(std::round(x0 + (x1 - x0) * k - w / 2)), py = float(std::round(y0 + (y1 - y0) * k - w / 2));
        c.fill(px, py, w, w, col);
    }
}

void Fight2d::drawWeapon(Ctx& c, const Show& s, const Exchange* e, long long who, double t, float sc) {
    const auto fi = s.f.find(who);
    if (fi == s.f.end()) return;
    const Fighter& F = fi->second;
    if (F.state != "fight") return;
    FightPose p;
    if (!pose(who, s.start + t, p)) return;
    const Json& W = D_.value("weapons", Json::object());
    const Json& w = W.contains(F.weapon) ? W[F.weapon] : (W.contains("fists") ? W["fists"] : Json::object());
    const double len = num(w, "len", 0.6);
    double ang = -35;   // the guard: raised, toward the other
    double extend = 1.0;
    bool swinging = false;
    if (e && e->kind == "attack" && e->att == who) {
        const double u = (t - e->t0) / std::max(1.0, e->t1 - e->t0);
        const double Wp = phase("windup", 0.34), S = phase("strike", 0.5);
        const Json& D = D_.value("directions", Json::object());
        const Json& dj = D.contains(e->dir) ? D[e->dir] : Json::object();
        const double from = num(dj, "from", -90), to = num(dj, "to", 30);
        const bool thrust = dj.value("thrust", false);
        if (u < Wp) {
            ang = -35 + (from - (-35)) * ease(u / Wp);
            if (thrust) extend = 1 - 0.35 * ease(u / Wp);
        } else if (u < S) {
            const double k = ease((u - Wp) / (S - Wp));
            ang = from + (to - from) * k;
            if (thrust) extend = 0.65 + (num(w, "thrust_only_mult", 1.3) - 0.65) * k;
            swinging = true;
        } else {
            const double k = ease((u - S) / (1 - S));
            ang = to + (-35 - to) * k;
            if (thrust) extend = num(w, "thrust_only_mult", 1.3) + (1 - num(w, "thrust_only_mult", 1.3)) * k;
        }
    } else if (e && e->kind == "attack" && e->def == who) {
        const double u = (t - e->t0) / std::max(1.0, e->t1 - e->t0);
        if ((e->out == "parry" || e->out == "block" || e->out == "block_shield") && u > 0.2)
            ang = -35 + (-75 + 35) * ease((u - 0.2) / 0.3) * (1 - ease((u - 0.7) / 0.3));
    }
    const double tp = c.tilePx();
    float hx, hy;
    c.toScreen(p.x + F.fx * 0.18, p.y + F.fy * 0.18, hx, hy);
    hy -= float(0.5 * tp);   // the hand at the chest
    const double base = std::atan2(F.fy, F.fx);
    const double a = base + ang * kPi / 180.0;
    const double L = len * extend * tp;
    const double ex = hx + std::cos(a) * L, ey = hy + std::sin(a) * L * 0.8;
    const float bw = float(num(w, "w", 1.2)) * sc;
    // a fading trail of the swing
    if (swinging) {
        const ui::Colour tc = colourOf(w, "blade", "#d8dde4");
        for (int i = 1; i <= 4; ++i) {
            const double aa = a - (std::signbit(ang) ? -1 : 1) * i * 0.12;
            line(c, hx + std::cos(aa) * L * 0.5, hy + std::sin(aa) * L * 0.4, hx + std::cos(aa) * L, hy + std::sin(aa) * L * 0.8, bw,
                 ui::Colour(tc.r, tc.g, tc.b, 0.35f / float(i)));
        }
    }
    const double grip = w.value("fist", false) ? 0.0 : 0.22;
    const double gx = hx + std::cos(a) * L * grip, gy = hy + std::sin(a) * L * grip * 0.8;
    line(c, hx, hy, gx, gy, bw, colourOf(w, "hilt", "#6a4a2a"));
    line(c, gx, gy, ex, ey, bw, colourOf(w, "blade", "#d8dde4"));
    if (w.contains("head")) line(c, ex - std::sin(a) * 1.5 * sc, ey + std::cos(a) * 1.5 * sc, ex + std::sin(a) * 1.5 * sc,
                                 ey - std::cos(a) * 1.5 * sc, bw * 1.6f, colourOf(w, "head", "#9aa0a8"));
}

void Fight2d::drawWorld(Ctx& c, double nowMs, float sc) {
    const double tp = c.tilePx();
    for (const Show& s : shows_) {
        const double t = nowMs - s.start;
        if (t < 0 || t > s.length) continue;
        const Exchange* e = current(s, t);
        drawWeapon(c, s, e, s.a, t, sc);
        drawWeapon(c, s, e, s.b, t, sc);
        // the bars over the fighters: stamina, and a pip a wound
        const Json& B = D_.value("bars", Json::object());
        for (const auto& [id, F] : s.f) {
            FightPose p;
            if (!pose(id, nowMs, p) || p.lie > 0.5) continue;
            float sx, sy;
            c.toScreen(p.x, p.y, sx, sy);
            const float w = float(num(B, "w", 16)) * sc, h = float(num(B, "h", 1.5)) * sc;
            const float x0 = sx - w / 2, y0 = sy - 22 * sc;
            c.fill(x0 - sc / 2, y0 - sc / 2, w + sc, h + sc, colourOf(B, "back", "#1a120a"));
            const double share = std::clamp(F.stamina / 100.0, 0.0, 1.0);
            c.fill(x0, y0, float(w * share), h, colourOf(B, "stamina", "#e8c040"));
            for (int i = 0; i < std::min(F.wounds, 8); ++i)
                c.fill(x0 + float(i) * 2 * sc, y0 + h + float(num(B, "gap", 1)) * sc, sc * 1.2f, sc * 1.2f, colourOf(B, "blood", "#c03020"));
        }
    }
    for (const Particle& p : parts_) {
        float sx, sy;
        c.toScreen(p.x, p.y, sx, sy);
        const double age = (nowMs - p.born) / std::max(1.0, p.life);
        ui::Colour col = p.c;
        if (p.z <= 0 && p.stainUntil > nowMs) col = ui::Colour(col.r * 0.6f, col.g * 0.6f, col.b * 0.6f, col.a * 0.7f);
        else col.a *= float(std::clamp(1.0 - age * age, 0.0, 1.0));
        const float z = float(p.z * tp), sz = float(p.size) * sc;
        if (p.glow) c.fill(sx - sz, sy - z - sz, sz * 2, sz * 2, ui::Colour(col.r, col.g, col.b, col.a * 0.3f));
        c.fill(float(std::round(sx - sz / 2)), float(std::round(sy - z - sz / 2)), sz, sz, col);
    }
    const Json& FT = D_.value("float_text", Json::object());
    const double life = num(FT, "life_ms", 1100), rise = num(FT, "rise_m", 0.9);
    for (const Floater& f : floats_) {
        const double k = (nowMs - f.born) / life;
        float sx, sy;
        c.toScreen(f.x, f.y, sx, sy);
        std::string text;
        if (f.text.rfind("@hit:", 0) == 0) text = c.L("ui2d.fight.hit", {{"zone", c.L(f.text.substr(5))}});
        else text = c.L(f.text);
        ui::Colour col = f.c;
        col.a = float(std::clamp(1.4 - k * 1.4, 0.0, 1.0));
        c.textC(sx, sy - float((1.25 + rise * k) * tp), text, col, float(num(FT, "px", 6.5)) * sc, true);
    }
}

void Fight2d::drawBanner(Ctx& c, double nowMs) {
    for (const Show& s : shows_) {
        if (!s.mine) continue;
        const double t = nowMs - s.start;
        if (t < 0 || t > s.length) continue;
        const Json& B = D_.value("banner", Json::object());
        const float U = c.u();
        const std::string title = c.L(str(B, "text").empty() ? "ui2d.fight.banner" : str(B, "text"), {{"a", s.nameA}, {"b", s.nameB}});
        int n = 0, i = 0;
        std::string lastId;
        for (const Exchange& e : s.ex) {
            ++i;
            if (t >= e.t0) n = i;
        }
        const int round = std::max(1, std::min(s.rounds, int(std::ceil(double(n) / std::max<std::size_t>(1, s.ex.size()) * s.rounds))));
        const std::string sub = c.L(str(B, "round").empty() ? "ui2d.fight.round" : str(B, "round"),
                                    {{"n", std::to_string(round)}, {"of", std::to_string(std::max(1, s.rounds))}});
        const float px = 15 * U, w = std::max(c.textW(title, px), c.textW(sub, 11 * U)) + 40 * U, h = 46 * U;
        const ui::Rect r{c.mapW() / 2 - w / 2, 54 * U, w, h};
        c.plate(r, true);
        c.textC(r.x + w / 2, r.y + 7 * U, title, colourOf(B, "colour", "#f0d890"), px, true);
        c.textC(r.x + w / 2, r.y + 27 * U, sub, pal::dim, 11 * U, true);
        return;
    }
}

} // namespace game2d
