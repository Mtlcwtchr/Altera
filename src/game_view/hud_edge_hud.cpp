// The HUD of the "Edge" style (AL-64): the layout of the mockup's style 3
// (gdd/ui/four_styles, mockups app.js hudMOD), authored for 1920 x 1080
// points: the compass on top, the family down the left, the menu diamonds and
// the "heard nearby" tracker on the right, the selected person's portrait,
// bars and needs at the bottom left, the target's bar with its medallion at
// the bottom centre, the day, speed and key prompts at the bottom right. On a
// screen wider than 16:9 the HUD keeps to the 16:9 band in the middle; on a
// small one (under 820 points high) it is compact.

#include <algorithm>
#include <cmath>

#include "game_view/hud.hpp"
#include "game_view/hud_style.hpp"

namespace lr::game {
namespace {

using ui::Colour;
using ui::Rect;
using namespace hudroles;
constexpr float kPi = 3.14159265f;

float wrapAngle(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

} // namespace

void Hud::edgeHud(const Snapshot& s, const ActorSnap* sel, float W, float H) {
    const float band = std::min(W, H * 16.f / 9.f);   // the 16:9 band of an ultra-wide screen
    const float ox = (W - band) * 0.5f;
    compact_ = H < 820 || band < 1500;
    edgeCompass(s, sel, ox, band);
    edgeMenu(ox, band);
    edgeFamily(s, sel, ox);
    followerChip(s, ox + 60 + 300, familyBottom_ + 4);   // AL-65
    edgeTracker(s, ox, band);
    caseCard(s, ox + band - 60 - 480, compact_ ? 170.f : 262.f, 480);   // AL-67
    if (sel && !sel->animal) {
        edgeNameplate(s, *sel, ox, H);   // AL-80: no portrait; the bars, the cast and the vitals: edgeBars
    } else if (sel) {
        const Rect r{ox + 60, H - 110, 320, 62};
        glass(r, true);
        caps(r.x + 18, r.y + 10, sel->name + (sel->group > 1 ? "  x" + std::to_string(sel->group) : ""), kGold, 18,
             0.14f);
        label(r.x + 18, r.y + 36, host_->text(sel->alive ? "game.fsm." + sel->state : "game.dead"), kText, 14);
    }
    edgeTarget(s, sel, ox, band, H);
    edgeClock(s, ox, band, H);
    edgeSkillToast(s, sel, ox, band);
}

// The compass band: the cardinal points as the camera faces, markers for the
// target, the family and the village; over it how far the current act's
// target is and what it is.
void Hud::edgeCompass(const Snapshot& s, const ActorSnap* sel, float ox, float band) {
    const float menuW = 7 * 54.f + 56;
    const float cw = std::clamp(band - 2 * (menuW + 40), 420.f, 800.f);
    const float cx = ox + band * 0.5f, x0 = cx - cw * 0.5f, y = compact_ ? 44.f : 52.f;
    if (sel && !sel->animal) {
        double tx = 0, ty = 0;
        bool has = false;
        if (sel->actTarget != sim::kNone) {
            if (const ActorSnap* t = actor(s, sel->actTarget)) tx = t->x, ty = t->y, has = true;
            for (const auto& th : s.things)
                if (th.id == sel->actTarget) tx = th.x, ty = th.y, has = true;
        }
        const std::string what = !sel->interaction.empty() ? host_->interactionName(sel->interaction)
                                                           : host_->text("game.fsm." + sel->state);
        std::string dist;
        if (has) dist = host_->text("hud.edge.metres", {{"n", int(std::lround(std::hypot(tx - sel->x, ty - sel->y)))}});
        const float px = 15, sp = 0.12f;
        const float dw = dist.empty() ? 0 : capsW(dist + " · ", px, sp, false);
        const std::string fitted = ui_->fit(what, cw - dw - 60, px / 15.f);
        const float lw = dw + capsW(fitted, px, sp, false);
        float lx = cx - lw * 0.5f;
        if (!dist.empty()) {
            caps(lx, y - 26, dist, kGold, px, sp, false, true);
            lx += capsW(dist, px, sp, false);
            caps(lx, y - 26, " · ", kText, px, sp, false, true);
            lx += capsW(" · ", px, sp, false);
        }
        caps(lx, y - 26, fitted, kText.lerp(kMute, 0.15f), px, sp, false, true);
    }
    // the band: dark in the middle, fading to the ends, a light rule above and below
    const float bh = 40;
    gradRect({x0, y, cw * 0.18f, bh}, kRim.withAlpha(0), kRim.withAlpha(0.55f), false);
    ui_->rect({x0 + cw * 0.18f, y, cw * 0.64f, bh}, kRim.withAlpha(0.55f));
    gradRect({x0 + cw * 0.82f, y, cw * 0.18f, bh}, kRim.withAlpha(0.55f), kRim.withAlpha(0), false);
    ui_->line(x0, y, x0 + cw, y, kText.withAlpha(0.35f));
    ui_->line(x0, y + bh, x0 + cw, y + bh, kText.withAlpha(0.35f));
    ui_->claim({x0, y, cw, bh});
    const float yaw = float(host_->cameraYaw());
    // a bearing (radians, x east, y north) to a place on the band; false more than 90 degrees aside
    const auto place = [&](float bearing, float& bx) {
        const float rel = wrapAngle(bearing - yaw);
        if (std::fabs(rel) > kPi * 0.5f) return false;
        bx = cx - rel / (kPi * 0.5f) * (cw * 0.5f - 30);
        return true;
    };
    static const char* kPoints[] = {"E", "NE", "N", "NW", "W", "SW", "S", "SE"};
    for (int k = 0; k < 16; ++k) {
        float bx;
        if (!place(float(k) * kPi / 8.f, bx)) continue;
        const float fade = 1.0f - std::fabs(bx - cx) / (cw * 0.5f) * 0.6f;
        if (k % 2) {
            disc(bx, y + bh * 0.5f, 1.5f, kText.withAlpha(0.45f * fade));
            continue;
        }
        const std::string p = host_->text(std::string("hud.edge.compass.") + kPoints[k / 2]);
        const float pw = capsW(p, 17, 0.1f, true);
        caps(bx - pw * 0.5f, y + (bh - ui_->textHeight(17 / 15.f)) * 0.5f, p, kText.withAlpha(fade), 17, 0.1f, true);
    }
    const auto marker = [&](double mx, double my, const char* ic, const Colour& c, bool dia) {
        const float b = float(std::atan2(my - sel->y, mx - sel->x));
        float bx;
        if (!place(b, bx)) return;
        if (dia) {
            const Colour f = c;
            diamond(bx, y + 12, 9, false, false, &f);
        } else {
            icon(ic, {bx - 10, y + 2, 20, 20}, c);
        }
    };
    if (sel) {
        if (s.mapSize > 0) marker(s.centre[0], s.centre[1], "build", kGoldPale, false);
        for (const auto& a : s.actors)
            if (!a.animal && a.alive && a.family == sel->family && a.id != sel->id) marker(a.x, a.y, "people", kText, false);
        if (aim != sim::kNone)
            if (const ActorSnap* t = actor(s, aim)) marker(t->x, t->y, "", kGold, true);
        if (sel->actTarget != sim::kNone && sel->actTarget != aim) {
            if (const ActorSnap* t = actor(s, sel->actTarget)) marker(t->x, t->y, "", kGold, true);
            for (const auto& th : s.things)
                if (th.id == sel->actTarget) marker(th.x, th.y, "", kGold, true);
        }
    }
    // the caret under the middle
    const SDL_FPoint tp[4] = {{cx - 5, y + bh + 4}, {cx + 5, y + bh + 4}, {cx, y + bh + 10}, {cx, y + bh + 10}};
    const Colour tc[4] = {kGold, kGold, kGold, kGold};
    quad(tp, tc);
}

// The menu as diamonds along the top right.
void Hud::edgeMenu(float ox, float band) {
    static const char* kMenu[][2] = {{"sheet", "hud.menu.sheet"}, {"dynasty", "hud.menu.dynasty"}, {"colony", "hud.menu.colony"},
                                     {"build", "hud.menu.build"}, {"annals", "hud.menu.annals"}, {"i_tool", "hud.menu.workshop"},
                                     {"map", "hud.menu.map"}};
    static const char* kPanel[] = {"sheet", "kin", "colony", "build", "annals", "workshop", ""};
    const float step = 54, d = 31;
    const float right = ox + band - 56, cy = compact_ ? 50.f : 62.f;
    for (int i = 0; i < 7; ++i) {
        const float cx = right - float(6 - i) * step - d * 0.71f;
        const bool on = *kPanel[i] && panel == kPanel[i];
        const std::string tip = host_->text(kMenu[i][1]) + (i == 6 ? " · " + host_->text("hud.soon") : "");
        if (diamondButton(cx, cy, d, kMenu[i][0], on, tip) && *kPanel[i]) panel = on ? "none" : kPanel[i];
    }
}

// The family as a list read from the right, a gold tick by every name (its
// height: how the person fares, red in trouble), as the quest list of the reference.
void Hud::edgeFamily(const Snapshot& s, const ActorSnap* sel, float ox) {
    familyBottom_ = compact_ ? 30.f : 40.f;
    if (!sel || sel->animal) return;
    std::vector<const ActorSnap*> fam{sel};
    for (const auto& a : s.actors)
        if (!a.animal && a.family == sel->family && a.id != sel->id) fam.push_back(&a);
    const float right = ox + 60 + 300;
    float y = familyBottom_;
    const std::size_t most = compact_ ? 4 : 6;
    for (std::size_t i = 0; i < fam.size() && i < most; ++i) {
        const ActorSnap& a = *fam[i];
        const bool me = a.id == sel->id;
        const bool trouble = a.alive && (a.lowNeed < 0.25 || a.hp < 0.35);
        std::string who;
        if (!a.alive) who = host_->text(a.sex == 'f' ? "hud.kin.dead_f" : "hud.kin.dead");
        else if (me) who = host_->text(a.player ? "hud.kin.you" : "hud.edge.selected");
        else {
            for (const auto& r : s.detail.relations)
                if (r.id == a.id && !r.kin.empty()) who = host_->text("hud.rel." + r.kin);
            if (a.player) who += (who.empty() ? "" : " · ") + host_->text("hud.kin.you");
        }
        if (trouble && a.hp < 0.35) who += (who.empty() ? "" : " · ") + host_->text("hud.edge.hurt");
        else if (trouble && !a.lowNeedId.empty())
            who += (who.empty() ? "" : " · ") + host_->text("hud.edge.low", {{"need", host_->text("need." + a.lowNeedId + ".name")}});
        const float th = 18 + float(std::clamp(a.alive ? std::min(a.lowNeed, a.hp) : 0.0, 0.0, 1.0)) * 20;
        const float nameW = capsW(a.name, 16, 0.12f, false);
        const float bangW = trouble ? capsW(" !", 16, 0.12f, false) : 0;
        const float nx = right - 14 - nameW - bangW;
        const Rect hit{right - 300, y - 2, 300, 40};
        const bool hot = ui_->hovered(hit);
        caps(nx, y, a.name, (me ? kGold : hot ? kGoldPale : kText).withAlpha(a.alive ? 1.f : 0.5f), 16, 0.12f, false, true);
        if (trouble) caps(nx + nameW, y, " !", kBad, 16, 0.12f, false, true);
        const std::string sub = ui_->fit(who, 280, 12 / 15.f);
        label(right - 14 - textW(sub, 12), y + 21, sub, kMute, 12, true);
        tick(right - 3, y + 2, th, trouble ? kBad : kGold);
        if (clicked(hit) && !me) host_->select(a.id);
        y += 44;
    }
    familyBottom_ = y;
}

// "Heard nearby": the latest lines of the chronicle as a tracker under the menu.
void Hud::edgeTracker(const Snapshot& s, float ox, float band) {
    if (s.chronicle.empty()) return;
    const float w = 480, x = ox + band - 60 - w, y = compact_ ? 92.f : 118.f;
    ui_->line(x + 8, y + 10, x + 2, y + 16, kGold);
    ui_->line(x + 2, y + 16, x + 8, y + 22, kGold);
    const Colour g = kGold;
    diamond(x + 18, y + 16, 8, false, false, &g);
    caps(x + 32, y + 4, host_->text("hud.edge.heard"), kGold, 18, 0.14f, true, true);
    const int n = compact_ ? 2 : 4;
    float ly = y + 36;
    int k = 0;
    for (auto it = s.chronicle.rbegin(); it != s.chronicle.rend() && k < n; ++it, ++k) {
        const float a = 1.0f - float(k) * 0.18f;
        diamond(x + 36, ly + 9, 7, false, false);
        label(x + 50, ly, ui_->fit(it->text, w - 54, 14 / 15.f), kText.withAlpha(a), 14, true);
        ly += 24;
    }
}

// The bar of whom the player aims at (or points to), under the compass as
// the enemy bar of Skyrim (AL-80): the name and who they are, how they feel
// towards the person; the bar (health in a fight, else the liking) with a
// diamond medallion carrying what they do now, named under it.
void Hud::edgeTarget(const Snapshot& s, const ActorSnap* sel, float ox, float band, float H) {
    (void)H;
    EntityId id = aim != sim::kNone ? aim : hoverActor;
    if (sel && id == sel->id) id = sim::kNone;
    const ActorSnap* t = id != sim::kNone ? actor(s, id) : nullptr;
    if (!t) return;
    const float cx = ox + band * 0.5f;
    const float top = (compact_ ? 44.f : 52.f) + 40 + 22;
    const float w = compact_ ? 360.f : 460.f;
    std::string who;
    const RelSnap* rel = nullptr;
    for (const auto& r : s.detail.relations)
        if (r.id == t->id) {
            rel = &r;
            if (!r.kin.empty()) who = host_->text("hud.rel." + r.kin);
        }
    if (who.empty() && !t->animal && !t->role.empty()) who = t->role;
    const float nw = capsW(t->name, 17, 0.12f, false);
    const float sw = who.empty() ? 0 : textW(" · " + who, 14);
    const float x = cx - (nw + sw) * 0.5f;
    caps(x, top, t->name, kText, 17, 0.12f, false, true);
    if (!who.empty()) label(x + nw, top + 2, " · " + who, kMute, 14, true);
    std::string feel;
    if (rel) feel = host_->text("hud.edge.affinity", {{"n", int(std::lround(rel->affinity * 100))}});
    else if (!t->animal) feel = host_->text("hud.bar.stranger");
    if (!feel.empty()) label(cx - textW(feel, 12) * 0.5f, top + 24, feel, kMute, 12, true);
    const bool fight = t->fighting || (sel && sel->fighting);
    const Rect r{cx - w * 0.5f, top + 50, w, 12};
    const std::string medal = !t->interaction.empty() ? barIcon({"act", t->interaction, 0}) : (t->animal ? "hand" : "w_social");
    if (fight || t->animal) filiBar(r, float(t->alive ? t->hp : 0), hudPalette().red, medal);
    else filiBar(r, rel ? float(std::clamp((rel->affinity + 1) * 0.5, 0.0, 1.0)) : 0.5f, hudPalette().grn, medal);
    const std::string doing = !t->alive ? host_->text("game.dead")
                            : !t->interaction.empty() ? host_->interactionName(t->interaction)
                                                      : host_->text("game.fsm." + t->state);
    const std::string d = ui_->fit(doing, w, 12 / 15.f);
    label(cx - textW(d, 12) * 0.5f, r.bottom() + 14, d, kText.withAlpha(0.85f), 12, true);
}

// Bottom right: the day, the season and weather; pause and speeds as diamonds;
// the key prompts V (how the person is led), Tab (the camera), Home (the walls),
// each also a button.
void Hud::edgeClock(const Snapshot& s, float ox, float band, float H) {
    const float right = ox + band - 60;
    const int day = int(s.tick / sim::kTicksPerDay) + 1;
    const std::string line = host_->text("hud.dayline", {{"day", day}, {"part", dayPart(s.tick)}});
    std::string weather = host_->text("hud.edge.season." + (s.season.empty() ? std::string("summer") : s.season));
    weather += " · " + std::to_string(int(std::lround(s.tempC))) + "°";
    weather += " · " + host_->text(s.rain > 0.3 ? "hud.edge.rain" : "hud.edge.dry");
    const float wy = H - 56 - 120;
    const float ww = textW(weather, 13);
    label(right - ww, wy + 4, weather, kMute, 13, true);
    const float lw = capsW(line, 17, 0.12f, false);
    caps(right - ww - 14 - lw, wy, line, kText, 17, 0.12f, false, true);

    // pause and the speeds, right to left
    const float d = 28, h = d * 0.7071f, cy = H - 56 - 62;
    float cx = right - h;
    for (int k = 3; k >= 0; --k) {
        const bool on = k == 0 ? s.paused : (!s.paused && s.speed == k);
        const bool locked = speedLocked && k > 1;
        const std::string tip = k == 0 ? host_->text("hud.pause")
                              : locked ? host_->text("hud.speed_locked")
                                       : host_->text("hud.speed", {{"n", k}});
        if (k == 0) {
            if (diamondButton(cx, cy, d, s.paused ? "play" : "pause", on, tip)) host_->pause();
        } else {
            const Rect box{cx - h, cy - h, h * 2, h * 2};
            const bool hot = ui_->hovered(box) && !locked;
            if (on) {
                const Colour f = kGold.withAlpha(0.25f);
                diamond(cx, cy, d, true, false, &f);
            } else {
                diamond(cx, cy, d, hot, false);
            }
            const Colour c = (on ? kHot : kText).withAlpha(locked ? 0.3f : 1.f);
            const float iw = 6.0f * float(k) + 1;
            for (int t = 0; t < k; ++t) {
                const float tx = cx - iw * 0.5f + float(t) * 6.0f;
                const SDL_FPoint p[4] = {{tx, cy - 5}, {tx + 7, cy}, {tx, cy + 5}, {tx, cy + 5}};
                const Colour cs[4] = {c, c, c, c};
                quad(p, cs);
            }
            if (ui_->hovered(box)) tipAt(cx, box.bottom() + 6, tip);
            if (clicked(box) && !locked) host_->speed(k);
        }
        cx -= h * 2 + 10;
    }

    // the key prompts, right to left
    const float ky = H - 56 - 22;
    float kx = right;
    const auto prompt = [&](const std::string& key, const std::string& text, const std::string& tip) {
        const float tw = textW(text, 14);
        const float kw = std::max(13 * 1.6f, textW(key, 13) + 12);
        const float x = kx - tw - 8 - kw;
        const Rect hit{x, ky - 2, kw + 8 + tw, 26};
        const bool hot = ui_->hovered(hit);
        keyCap(x, ky, key, 13);
        label(x + kw + 8, ky + 2, text, hot ? kGoldPale : kText, 14, true);
        if (hot) tipAt(hit.x + hit.w * 0.5f, ky - 40, tip);
        kx = x - 18;
        return clicked(hit);
    };
    static const char* kWalls[3] = {"hud.edge.walls.up", "hud.edge.walls.cut", "hud.edge.walls.down"};
    static const char* kWallTips[3] = {"hud.walls.up", "hud.walls.cut", "hud.walls.down"};
    const int wm = std::clamp(wallMode, 0, 2);
    if (prompt("Home", host_->text(kWalls[wm]), host_->text(kWallTips[wm]))) host_->wallMode(wm == 1 ? 2 : wm == 2 ? 0 : 1);
    if (prompt("Tab", host_->text("hud.edge.camera"),
               host_->text(cameraFollow ? "hud.control.camera_on" : "hud.control.camera_off")))
        host_->cameraFollow(!cameraFollow);
    static const char* kModes[3] = {"hud.edge.mode.keys", "hud.edge.mode.mouse", "hud.edge.mode.free"};
    static const char* kModeTips[3] = {"hud.control.keys", "hud.control.mouse", "hud.control.free"};
    const int cm = std::clamp(controlMode, 0, 2);
    std::string modeTip = host_->text(kModeTips[cm]);
    if (!hasPerson)
        modeTip += " - " + (leadName.empty() ? host_->text("hud.control.take_none")
                                             : host_->text("hud.control.take", {{"who", leadName}}));
    if (prompt("V", host_->text(kModes[cm]), modeTip)) host_->controlMode((cm + 1) % 3);
}

// A skill grew a step (a tenth): the "level up" band with the step in a diamond.
void Hud::edgeSkillToast(const Snapshot& s, const ActorSnap* sel, float ox, float band) {
    if (sel && s.detail.id == sel->id) {
        if (skillWho_ != sel->id) {
            skillWho_ = sel->id;
            skillStep_.clear();
            for (const auto& [k, v] : s.detail.skills) skillStep_[k] = int(v * 10);
        } else {
            for (const auto& [k, v] : s.detail.skills) {
                const int step = int(v * 10);
                const auto it = skillStep_.find(k);
                if (it != skillStep_.end() && step > it->second) skillToasts_.push_back({k, step, clock_});
                skillStep_[k] = step;
            }
        }
    }
    skillToasts_.erase(std::remove_if(skillToasts_.begin(), skillToasts_.end(),
                                      [&](const SkillToast& t) { return clock_ - t.born > 5.0; }),
                       skillToasts_.end());
    if (skillToasts_.empty()) return;
    const SkillToast& t = skillToasts_.front();
    const float w = 460, h = 76, x = ox + band * 0.5f - w * 0.5f, y = compact_ ? 196.f : 236.f;
    const float fade = float(std::clamp(std::min((clock_ - t.born) / 0.15, (5.0 - (clock_ - t.born)) / 0.5), 0.0, 1.0));
    gradRect({x, y, w * 0.2f, h}, kRim.withAlpha(0), kRim.withAlpha(0.8f * fade), false);
    ui_->rect({x + w * 0.2f, y, w * 0.6f, h}, kRim.withAlpha(0.8f * fade));
    gradRect({x + w * 0.8f, y, w * 0.2f, h}, kRim.withAlpha(0.8f * fade), kRim.withAlpha(0), false);
    ui_->line(x, y, x + w, y, kGold.withAlpha(0.5f * fade));
    ui_->line(x, y + h, x + w, y + h, kGold.withAlpha(0.5f * fade));
    diamond(x + 70, y + h * 0.5f, 44, false, true);
    const std::string n = std::to_string(t.step);
    label(x + 70 - textW(n, 22) * 0.5f, y + h * 0.5f - ui_->textHeight(22 / 15.f) * 0.5f, n, kText.withAlpha(fade), 22);
    const std::string head = host_->text("hud.edge.skill_up", {{"skill", t.skill}});
    caps(x + 118, y + 12, ui_->fit(head, w - 140, 22 / 15.f, true), kText.withAlpha(fade), 22, 0.2f, true, true);
    label(x + 118, y + 46, host_->text("hud.edge.skill_step", {{"n", t.step}}), kMute.withAlpha(fade), 13, true);
}

} // namespace lr::game
