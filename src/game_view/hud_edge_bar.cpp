// AL-80: the HUD of WoW and Skyrim on the Edge style
// (gdd/ui/hud_wow_skyrim/HUD_WOW_SKYRIM.md, agreed 23.11.2026): the quick
// bars 1-8 and Shift+1-8 in the middle at the bottom (three pages each, the
// fight's page by itself, the lock, dragging from the wheel, the bag, the
// sheet and between the slots), the act going on as a cast bar over them with
// the queue beside it, health and strength at the bars' sides and the spirit
// under them, the name line with the needs bottom left, and the talk over it
// all on a grey band: the lines said near the person and, while they talk,
// the topics and the talk acts in two columns with the tone.

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

SDL_FColor fcol(const Colour& c) { return {c.r, c.g, c.b, c.a}; }

const char* categoryIcon(const std::string& c) {
    static const std::pair<const char*, const char*> k[] = {
        {"social", "w_social"},   {"family", "w_family"},       {"economy", "w_economy"}, {"politics", "w_politics"},
        {"knowledge", "w_knowledge"}, {"faith", "w_faith"},     {"violence", "w_violence"}, {"medicine", "w_medicine"},
        {"household", "body"},    {"work", "c_hammer"},         {"gather", "m_plant"},    {"craft", "c_bench"},
        {"travel", "follow"},     {"crime", "hand"}};
    for (const auto& [id, ic] : k)
        if (c == id) return ic;
    return "wants";
}

const char* goalIcon(const std::string& g) {
    static const std::pair<const char*, const char*> k[] = {
        {"eat", "i_bread"},    {"drink", "c_drop"},     {"sleep", "body"},          {"rest", "body"},
        {"get_warm", "c_flame"}, {"socialize", "w_social"}, {"have_fun", "people"}, {"wash", "c_drop"},
        {"pray", "w_faith"},   {"stock_food", "i_food"}, {"tend_fields", "m_plant"}, {"stock_firewood", "i_wood"},
        {"care", "w_family"},  {"share_food", "i_bread"}, {"make_axe", "i_tool"},    {"make_clothes", "i_cloth"},
        {"build_home", "build"}, {"build", "build"},    {"stock_herbs", "i_herb"},   {"brew", "c_ferment"},
        {"tend_garden", "m_plant"}, {"tend_livestock", "m_food"}, {"offer_trade", "w_economy"},
        {"help_neighbours", "people"}, {"ask_help", "hand"}};
    for (const auto& [id, ic] : k)
        if (g == id) return ic;
    return "wants";
}

float wrapA(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

const char* kRoman[] = {"I", "II", "III"};

} // namespace

// A square with its corners cut: the slot of the quick bar. Glass darkening
// downwards, a contour (dashed for an empty slot).
void Hud::chamfer(const Rect& r, float cut, const Colour& fill, const Colour& edge, float lw, bool dashed) {
    const SDL_FPoint p[8] = {{r.x + cut, r.y},           {r.right() - cut, r.y},   {r.right(), r.y + cut},
                             {r.right(), r.bottom() - cut}, {r.right() - cut, r.bottom()}, {r.x + cut, r.bottom()},
                             {r.x, r.bottom() - cut},     {r.x, r.y + cut}};
    if (fill.a > 0) {
        SDL_Vertex v[9];
        const Colour low = fill.lerp(kRim, 0.45f);
        v[0] = {{r.x + r.w * 0.5f, r.y + r.h * 0.5f}, fcol(fill.lerp(low, 0.5f)), {0, 0}};
        for (int k = 0; k < 8; ++k) v[k + 1] = {p[k], fcol(fill.lerp(low, (p[k].y - r.y) / std::max(1.f, r.h))), {0, 0}};
        int idx[24];
        for (int k = 0; k < 8; ++k) idx[k * 3] = 0, idx[k * 3 + 1] = k + 1, idx[k * 3 + 2] = (k + 1) % 8 + 1;
        SDL_RenderGeometry(sdl_, nullptr, v, 9, idx, 24);
    }
    if (edge.a <= 0) return;
    for (int e = 0; e < 8; ++e) {
        const SDL_FPoint a = p[e], b = p[(e + 1) % 8];
        if (!dashed) {
            thickLine(a.x, a.y, b.x, b.y, lw, edge);
            continue;
        }
        const float len = std::hypot(b.x - a.x, b.y - a.y);
        for (float t = 0; t < len; t += 9) {
            const float t1 = std::min(len, t + 5);
            thickLine(a.x + (b.x - a.x) * t / len, a.y + (b.y - a.y) * t / len, a.x + (b.x - a.x) * t1 / len,
                      a.y + (b.y - a.y) * t1 / len, lw, edge);
        }
    }
}

std::string Hud::barIcon(const BarRef& r) const {
    if (r.kind == "goal") return goalIcon(r.id);
    if (r.kind == "item") return itemIcon(r.id);
    if (r.kind == "act") return categoryIcon(host_->actMeta(r.id).value("category", std::string()));
    return "";
}

std::string Hud::barName(const BarRef& r) const {
    if (r.kind == "act") return host_->interactionName(r.id);
    if (r.kind == "goal") {
        for (const auto& [id, name] : host_->goals())
            if (id == r.id) return name;
        return r.id;
    }
    if (r.kind == "item") {
        const std::string act = host_->itemAct(r.id);
        return act.empty() ? r.id : host_->interactionName(act);
    }
    return "";
}

// A slot pressed (by the key or the mouse): the command for the sim, or the
// aim cursor first for an act on a person with nobody chosen.
void Hud::useSlot(const Snapshot& s, const BarRef& r, EntityId forced) {
    if (r.empty() || s.controlled == sim::kNone) return;
    EntityId chosen = forced;
    if (chosen == sim::kNone && r.aim != 3 && aim != sim::kNone && aim != s.controlled)
        if (const ActorSnap* t = actor(s, aim); t && t->alive && !t->animal) chosen = aim;
    BarRef use = r;
    if (forced != sim::kNone && use.aim == 3) use.aim = 2;   // the aim was given
    const std::string target = r.kind == "act" ? host_->actMeta(r.id).value("target", std::string()) : std::string();
    const std::string itemAct = r.kind == "item" ? host_->itemAct(r.id) : std::string();
    const BarUse u = barUse(use, target, itemAct, chosen);
    if (u.kind == BarUse::Aim) {
        aiming_ = true;
        aimRef_ = r;
        aimPicked_ = sim::kNone;
        return;
    }
    if (u.kind != BarUse::Command) return;
    sim::Json args = u.args;
    args["id"] = s.controlled;
    if (u.type == "perform" && tone_ >= 0 && tone_ < int(s.wheel.tones.size()) && target == "character")
        args["tone"] = s.wheel.tones[std::size_t(tone_)];
    host_->command(u.type, args);
}

// RMB on a slot: what its act is done to.
void Hud::aimMenu(const Snapshot& s) {
    (void)s;
    if (!aimMenu_.open) return;
    const float w = 250, rh = 30;
    const Rect P{aimMenu_.x - w * 0.5f, aimMenu_.y - 4 * rh - 26, w, 4 * rh + 16};
    glass(P, true);
    BarRef r = hotbars.get(s.controlled, aimMenu_.bar, aimMenu_.page, aimMenu_.slot);
    bool hit = false;
    for (int i = 0; i < 4; ++i) {
        const Rect row{P.x + 8, P.y + 8 + float(i) * rh, w - 16, rh};
        const bool hot = ui_->hovered(row);
        if (hot || r.aim == i) selRow(row);
        label(row.x + 14, row.y + 6, host_->text("hud.bar.aim." + std::to_string(i)), hot ? kGoldPale : kText, 14);
        if (clicked(row)) {
            r.aim = i;
            hotbars.set(s.controlled, aimMenu_.bar, aimMenu_.page, aimMenu_.slot, r);
            hit = true;
        }
    }
    ui_->claim(P);
    if (hit || (in_->pressed && !ui_->hovered(P)) || in_->escape) aimMenu_.open = false;
}

void Hud::edgeBars(const Snapshot& s, const ActorSnap* sel, float W, float H) {
    const float band = std::min(W, H * 16.f / 9.f), ox = (W - band) * 0.5f;
    const bool small = H < 820 || band < 1500;
    const ActorSnap* me = actor(s, s.controlled);
    // whose bars: the person led; with nobody led, the selected one's (shown, not used)
    const ActorSnap* who = me ? me : (sel && !sel->animal ? sel : nullptr);
    if (!who) {
        keyQueue_.clear();
        barPress_ = {};
        aiming_ = false;
        barTop_ = H - 40;
        return;
    }
    const std::uint64_t id = who->id;
    if (pageSteps_) {
        hotbars.turnPage(id, 0, pageSteps_);
        pageSteps_ = 0;
    }
    Hotbars::Layout& L = hotbars.of(id);
    const bool fight = who->fighting;
    const bool two = settingsBar2;
    const float c = small ? 52.f : 68.f, gap = small ? 5.f : 6.f, cut = c * 0.2f;
    const float bw = 8 * c + 7 * gap;
    const float x0 = std::round(ox + band * 0.5f - bw * 0.5f);
    const float y1 = H - (small ? 30.f : 40.f) - c;
    const float y2 = y1 - c - (small ? 10.f : 14.f);
    barTop_ = two ? y2 : y1;
    const bool canUse = me != nullptr;
    const auto pageOf = [&](int b) { return b == 0 && fight ? Hotbars::kFightPage : L.page[b]; };

    // the person chosen for acts on a person, and what the sim should check
    EntityId chosen = sim::kNone;
    if (aim != sim::kNone && aim != s.controlled)
        if (const ActorSnap* t = actor(s, aim); t && t->alive && !t->animal) chosen = aim;
    std::vector<std::string> check;
    for (int b = 0; b < (two ? 2 : 1); ++b)
        for (int k = 0; k < Hotbars::kSlots; ++k) {
            const BarRef& r = L.slot[b][pageOf(b)][k];
            if (r.kind == "act") check.push_back(r.id);
            else if (r.kind == "item")
                if (std::string ia = host_->itemAct(r.id); !ia.empty()) check.push_back(ia);
        }
    host_->barCheck(check, chosen);

    // the keys; in a talk 1-8 pick the reply
    for (const auto& [b, k] : keyQueue_) {
        if (talkOpen_ && b == 0) {
            talkKey_ = k;
            continue;
        }
        if ((b == 1 && !two) || !canUse || !Hotbars::valid(b, 0, k)) continue;
        slotFlash_[std::to_string(b) + ":" + std::to_string(k)] = clock_;
        useSlot(s, L.slot[b][pageOf(b)][k]);
    }
    keyQueue_.clear();
    // the aim: a person clicked
    if (aiming_ && aimPicked_ != sim::kNone) {
        useSlot(s, aimRef_, aimPicked_);
        aiming_ = false;
        aimPicked_ = sim::kNone;
    }

    struct Hit {
        int b, k;
        Rect r;
    };
    std::vector<Hit> hits;
    const float alpha = talkOpen_ ? 0.4f : 1.0f;
    const HudPalette& P = hudPalette();
    for (int b = 0; b < (two ? 2 : 1); ++b) {
        const float y = b == 0 ? y1 : y2;
        const int page = pageOf(b);
        // the rails: gold lines over and under the row, diamonds at their ends
        ui_->line(x0 - 8, y - 5, x0 + bw + 8, y - 5, kGold.withAlpha(0.55f * alpha));
        ui_->line(x0 - 8, y + c + 5, x0 + bw + 8, y + c + 5, kGold.withAlpha(0.55f * alpha));
        const Colour dg = kGold.withAlpha(0.9f * alpha);
        diamond(x0 - 12, y + c * 0.5f, 9, false, false, &dg);
        diamond(x0 + bw + 12, y + c * 0.5f, 9, false, false, &dg);
        // the page: I-III with arrows (the fight's page is named)
        {
            const float px = x0 + bw + 30, cy = y + c * 0.5f;
            const std::string pn = page == Hotbars::kFightPage ? host_->text("hud.bar.fight") : kRoman[page];
            const float pw = capsW(pn, 14, 0.1f, true);
            const Rect la{px, cy - 12, 16, 24}, ra{px + 22 + pw + 6, cy - 12, 16, 24};
            const bool fightPage = page == Hotbars::kFightPage;
            for (int side = 0; side < 2; ++side) {
                const Rect a = side ? ra : la;
                const bool hot = ui_->hovered(a) && !fightPage;
                const Colour col = (hot ? kGoldPale : kGold).withAlpha((fightPage ? 0.3f : 0.9f) * alpha);
                const float ax = a.x + a.w * 0.5f;
                const SDL_FPoint tp[4] = {{side ? ax - 4 : ax + 4, cy - 7}, {side ? ax + 5 : ax - 5, cy},
                                          {side ? ax - 4 : ax + 4, cy + 7}, {side ? ax - 4 : ax + 4, cy + 7}};
                const Colour tc[4] = {col, col, col, col};
                quad(tp, tc);
                if (clicked(a) && !fightPage) hotbars.turnPage(id, b, side ? 1 : -1);
            }
            caps(px + 22, cy - ui_->textHeight(14 / 15.f) * 0.5f, pn, kText.withAlpha(alpha), 14, 0.1f, true, true);
            if (ui_->hovered({px, cy - 12, ra.right() - px, 24}))
                tipAt(px + 30, y - 40, host_->text(fightPage ? "hud.bar.fight_tip" : "hud.bar.page_tip"));
        }
        for (int k = 0; k < Hotbars::kSlots; ++k) {
            const Rect r{x0 + float(k) * (c + gap), y, c, c};
            hits.push_back({b, k, r});
            const BarRef& ref = L.slot[b][page][k];
            const bool hot = ui_->hovered(r) && !talkOpen_;
            const std::string keyName = std::to_string(k + 1);
            if (ref.empty()) {
                chamfer(r, cut, Colour::rgb(0, 0, 0, 0.22f * alpha), kGold.withAlpha((hot ? 0.6f : 0.32f) * alpha), 1.0f, true);
                label(r.x + 6, r.y + 3, keyName, kMute.withAlpha(0.7f * alpha), 12, true);
                if (b == 1) ui_->rect({r.x + 6, r.y + 18, 8, 1.5f}, kMute.withAlpha(0.7f * alpha));
            } else {
                // what the slot can do now
                const std::string act = ref.kind == "act" ? ref.id : ref.kind == "item" ? host_->itemAct(ref.id) : "";
                const ActInfoSnap* info = nullptr;
                if (!act.empty() && s.barFor == chosen)
                    for (const auto& a : s.barActs)
                        if (a.id == act) info = &a;
                int count = -1;
                if (ref.kind == "item" && s.detail.id == id) {
                    count = 0;
                    for (const auto& it : s.detail.inventory)
                        if (it.id == ref.id) count += it.count;
                }
                const std::string target = ref.kind == "act" ? host_->actMeta(ref.id).value("target", std::string()) : "";
                bool noTarget = false;
                if (target == "character" && ref.aim != 1) {
                    if (chosen == sim::kNone) noTarget = true;
                    else if (const ActorSnap* t = actor(s, chosen); t && std::hypot(t->x - who->x, t->y - who->y) > 25.0)
                        noTarget = true;
                }
                const bool off = !canUse || (info && !info->can) || count == 0 || (ref.kind == "item" && act.empty());
                const bool running = (!act.empty() && who->interaction == act) || (ref.kind == "goal" && who->goal == ref.id);
                const auto fl = slotFlash_.find(std::to_string(b) + ":" + std::to_string(k));
                const double since = fl == slotFlash_.end() ? 1e9 : clock_ - fl->second;
                const bool pressed = since < 0.25 || (barPress_.on && barPress_.key == "slot" && barPress_.bar == b &&
                                                       barPress_.slot == k && !barPress_.dragging);
                if (hot || pressed) chamfer(r.inset(-3), cut + 1, Colour{0, 0, 0, 0}, kGoldPale.withAlpha(0.28f * alpha), 3.0f);
                const Colour edge = running ? P.grn : (hot || pressed) ? kGoldPale : kGold;
                chamfer(r, cut, Colour::rgb(46, 50, 54, 0.88f * alpha), edge.withAlpha(alpha), running || hot ? 2.0f : 1.5f);
                const float is = std::round(c * 0.56f);
                icon(barIcon(ref), {r.x + (c - is) * 0.5f, r.y + (c - is) * 0.5f + 2, is, is},
                     (pressed ? kHot : kText).withAlpha((off ? 0.35f : 1.f) * alpha));
                if (off) {
                    chamfer(r.inset(1), cut, Colour::rgb(0, 0, 0, 0.35f * alpha), Colour{0, 0, 0, 0}, 0);
                    // the red lock
                    const float lx = r.right() - 16, ly = r.bottom() - 15;
                    ui_->rect({lx, ly + 4, 10, 8}, kBad.withAlpha(alpha));
                    ui_->line(lx + 2, ly + 4, lx + 2, ly, kBad.withAlpha(alpha));
                    ui_->line(lx + 8, ly + 4, lx + 8, ly, kBad.withAlpha(alpha));
                    ui_->line(lx + 2, ly, lx + 8, ly, kBad.withAlpha(alpha));
                }
                // "just done": the sectors darken away, with the time left
                if (since < 1.5) {
                    const float f = float(1.0 - since / 1.5);
                    const float cx = r.x + c * 0.5f, cy = r.y + c * 0.5f, rr = c * 0.46f;
                    const int n = std::max(2, int(24 * f));
                    std::vector<SDL_Vertex> v;
                    v.push_back({{cx, cy}, fcol(Colour::rgb(0, 0, 0, 0.45f)), {0, 0}});
                    for (int t = 0; t <= n; ++t) {
                        const float a = -kPi * 0.5f + 2 * kPi * (1 - f) + 2 * kPi * f * float(t) / float(n);
                        v.push_back({{cx + std::cos(a) * rr, cy + std::sin(a) * rr}, fcol(Colour::rgb(0, 0, 0, 0.45f)), {0, 0}});
                    }
                    std::vector<int> idx;
                    for (int t = 1; t < int(v.size()) - 1; ++t) idx.insert(idx.end(), {0, t, t + 1});
                    SDL_RenderGeometry(sdl_, nullptr, v.data(), int(v.size()), idx.data(), int(idx.size()));
                }
                label(r.x + 6, r.y + 3, keyName, (noTarget ? kBad : kText).withAlpha(alpha), 13, true);
                if (b == 1) ui_->rect({r.x + 6, r.y + 19, 8, 1.5f}, (noTarget ? kBad : kGold).withAlpha(alpha));
                if (count >= 0) {
                    const std::string n = std::to_string(count);
                    label(r.right() - 6 - textW(n, 12), r.bottom() - 18, n, kText.withAlpha(alpha), 12, true);
                }
                if (hot && !barPress_.dragging) {
                    std::string tip = barName(ref);
                    if (info && !info->can && !info->why.empty()) tip += " · " + info->why;
                    else if (ref.kind == "item" && act.empty()) tip += " · " + host_->text("hud.bar.no_use");
                    else if (count == 0) tip += " · " + host_->text("hud.bar.none_left");
                    else if (noTarget) tip += " · " + host_->text(chosen == sim::kNone ? "hud.bar.no_target" : "hud.bar.far");
                    if (!canUse) tip += " · " + host_->text("hud.wheel.need_control");
                    tipAt(r.x + c * 0.5f, r.y - 44, tip);
                }
                if (hot && in_->rightPressed && canUse) aimMenu_ = {true, b, page, k, r.x + c * 0.5f, r.y};
                if (clicked(r) && !talkOpen_) {
                    if (!hotbars.locked || in_->shift) barPress_ = {ref, "slot", b, page, k, in_->mouseX, in_->mouseY, true, false};
                    else if (canUse) {
                        slotFlash_[std::to_string(b) + ":" + std::to_string(k)] = clock_;
                        useSlot(s, ref);
                    }
                }
            }
            ui_->claim(r);
        }
    }
    // the lock, left of the first row
    {
        const float lx = x0 - 46, ly = y1 + c * 0.5f - 12;
        const Rect hit{lx - 6, ly - 6, 30, 36};
        const bool hot = ui_->hovered(hit);
        const Colour col = (hot ? kGoldPale : hotbars.locked ? kGold : kMute).withAlpha(alpha);
        ui_->rect({lx, ly + 9, 18, 14}, col.withAlpha(0.25f * alpha));
        ui_->border({lx, ly + 9, 18, 14}, col, 1.5f, 2);
        const float sx = hotbars.locked ? lx + 4 : lx + 9;   // the shackle swings open
        ui_->line(sx, ly + 9, sx, ly + 2, col);
        ui_->line(sx + 10, ly + (hotbars.locked ? 9.f : 4.f), sx + 10, ly + 2, col);
        ui_->line(sx, ly + 2, sx + 10, ly + 2, col);
        if (hot) tipAt(lx + 9, y1 - 44, host_->text(hotbars.locked ? "hud.bar.locked" : "hud.bar.unlocked"));
        if (clicked(hit)) hotbars.locked = !hotbars.locked;
    }
    const auto slotUnder = [&](float x, float y) -> const Hit* {
        for (const auto& h : hits)
            if (x >= h.r.x && x < h.r.right() && y >= h.r.y && y < h.r.bottom()) return &h;
        return nullptr;
    };

    // a drag onto the bars: from a slot (swap; off the bars: cleared) or from the wheel, the sheet
    if (barPress_.on) {
        const bool mayDrag = !hotbars.locked || in_->shift;
        if (!barPress_.dragging && in_->down && mayDrag &&
            std::hypot(in_->mouseX - barPress_.x, in_->mouseY - barPress_.y) > 6)
            barPress_.dragging = true;
        if (barPress_.dragging) {
            const Rect g{in_->mouseX - c * 0.4f, in_->mouseY - c * 0.4f, c * 0.8f, c * 0.8f};
            chamfer(g, cut * 0.8f, Colour::rgb(46, 50, 54, 0.8f), kGoldPale, 1.5f);
            const float is = std::round(c * 0.46f);
            icon(barIcon(barPress_.ref), {g.x + (g.w - is) * 0.5f, g.y + (g.h - is) * 0.5f, is, is}, kText);
            if (const Hit* h = slotUnder(in_->mouseX, in_->mouseY)) chamfer(h->r.inset(-3), cut + 1, Colour{0, 0, 0, 0}, kGoldPale, 2.0f);
        }
        if (in_->released || !in_->down) {
            const Hit* h = slotUnder(in_->mouseX, in_->mouseY);
            if (barPress_.dragging) {
                if (h && barPress_.key == "slot")
                    hotbars.swap(id, barPress_.bar, barPress_.page, barPress_.slot, h->b, pageOf(h->b), h->k);
                else if (h) hotbars.set(id, h->b, pageOf(h->b), h->k, barPress_.ref);
                else if (barPress_.key == "slot") hotbars.set(id, barPress_.bar, barPress_.page, barPress_.slot, {});
            } else if (barPress_.key == "slot" && h && h->b == barPress_.bar && h->k == barPress_.slot && canUse) {
                slotFlash_[std::to_string(h->b) + ":" + std::to_string(h->k)] = clock_;
                useSlot(s, barPress_.ref);
            }
            barPress_ = {};
        }
    }
    // a thing of the bag dropped on a slot
    if (!invDragWas_.empty() && in_->released && (!hotbars.locked || in_->shift))
        if (const Hit* h = slotUnder(in_->mouseX, in_->mouseY)) hotbars.set(id, h->b, pageOf(h->b), h->k, {"item", invDragWas_, 0});
    aimMenu(s);

    // over the bars: the act going on, the vitals at the sides, the spirit under them
    if (panel == "none" && !who->animal) {
        if (me) edgeCast(s, *me, ox + band * 0.5f, barTop_ - 14 - 22);
        edgeSideVitals(s, *who, x0 - 66, x0 + bw + 112, y1 + c * 0.5f, y1 + c + 10);
    }
    // the aim cursor: a ring at the mouse and what to do
    if (aiming_) {
        const float mx = in_->mouseX, my = in_->mouseY;
        arc(mx, my, 14, 2, 0, 360, kGoldPale);
        ui_->line(mx - 22, my, mx - 8, my, kGoldPale);
        ui_->line(mx + 8, my, mx + 22, my, kGoldPale);
        ui_->line(mx, my - 22, mx, my - 8, kGoldPale);
        ui_->line(mx, my + 8, mx, my + 22, kGoldPale);
        const std::string t = host_->text("hud.bar.aim_hint", {{"act", barName(aimRef_)}});
        caps(mx + 26, my + 14, t, kGoldPale, 13, 0.1f, false, true);
    }
}

// The act going on as a cast bar: its name, how long is left, its progress;
// the queue as diamonds right of it. RMB cancels (the act, or a queued one).
void Hud::edgeCast(const Snapshot& s, const ActorSnap& me, float cx, float y) {
    const bool idle = me.interaction.empty() && (me.progress < 0 || me.state == "Idle");
    std::vector<std::pair<int, std::string>> queue;   // index in the sim's queue, name
    if (s.detail.id == me.id)
        for (std::size_t i = s.detail.wantsGoal ? 1 : 0; i < s.detail.wants.size() && queue.size() < 3; ++i)
            queue.push_back({int(i) - (s.detail.wantsGoal ? 1 : 0), s.detail.wants[i]});
    if (idle && queue.empty()) return;
    const float w = compact_ ? 340.f : 440.f, h = 22;
    const Rect r{cx - w * 0.5f, y, w, h};
    if (!idle) {
        std::string what = !me.interaction.empty() ? host_->interactionName(me.interaction) : host_->text("game.fsm." + me.state);
        if (me.progress >= 0 && me.actTicks > 0) {
            const int left = int(std::ceil((1.0 - std::clamp(me.progress, 0.0, 1.0)) * me.actTicks * (24.0 * 60.0 / double(sim::kTicksPerDay))));
            what += " · " + host_->text("hud.bar.left", {{"n", std::max(1, left)}});
        }
        gradRect({r.x - 30, r.y, 30, h}, kRim.withAlpha(0), kRim.withAlpha(0.6f), false);
        ui_->rect(r, kRim.withAlpha(0.6f));
        gradRect({r.right(), r.y, 30, h}, kRim.withAlpha(0.6f), kRim.withAlpha(0), false);
        if (me.progress >= 0)
            gradRect({r.x, r.y + h - 4, r.w * float(std::clamp(me.progress, 0.0, 1.0)), 4}, kGold.withAlpha(0.9f), kGoldPale, false);
        ui_->line(r.x, r.y, r.right(), r.y, kGold.withAlpha(0.45f));
        ui_->line(r.x, r.bottom(), r.right(), r.bottom(), kGold.withAlpha(0.45f));
        const std::string t = ui_->fit(what, w - 20, 14 / 15.f);
        label(cx - textW(t, 14) * 0.5f, r.y + 2, t, kText, 14, true);
        if (ui_->hovered(r)) {
            tipAt(cx, r.y - 40, host_->text("hud.bar.cancel"));
            if (in_->rightPressed) host_->command("cancel_goal", {{"id", me.id}});
        }
        ui_->claim(r);
    }
    float qx = r.right() + 40;
    for (const auto& [i, name] : queue) {
        const float d = 24, hh = d * 0.7071f;
        const Rect box{qx - hh, r.y + h * 0.5f - hh, hh * 2, hh * 2};
        const bool hot = ui_->hovered(box);
        diamond(qx, r.y + h * 0.5f, d, hot, false);
        icon("wants", {qx - 8, r.y + h * 0.5f - 8, 16, 16}, kText.withAlpha(0.8f));
        if (hot) {
            tipAt(qx, r.y - 40, name + " · " + host_->text("hud.bar.unqueue"));
            if (in_->rightPressed) host_->command("unqueue", {{"id", me.id}, {"index", i}});
        }
        qx += 36;
    }
}

// Health left of the bars, strength right of them, the spirit as a line
// under them; full and still ones fade away after three seconds (Skyrim),
// unless the settings keep them.
void Hud::edgeSideVitals(const Snapshot& s, const ActorSnap& a, float leftEnd, float rightStart, float midY, float underY) {
    const HudPalette& P = hudPalette();
    const double spirit = s.detail.id == a.id ? std::clamp(0.5 + s.detail.mood * 0.5, 0.0, 1.0) : 0.5;
    const double strength = s.detail.id == a.id ? std::clamp(1.0 - s.detail.fatigue, 0.0, 1.0) : 1.0;
    const double v[3] = {a.hp, strength, spirit};
    const float L = compact_ ? 150.f : 220.f;
    const Rect rs[3] = {{leftEnd - L, midY - 4, L, 8}, {rightStart, midY - 4, L, 8}, {(leftEnd + rightStart) * 0.5f - 180, underY, 360, 3}};
    const Colour cs[3] = {P.red, P.grn, P.blu};
    const char* tips[3] = {"hud.vital.health", "hud.edge.strength", "hud.edge.spirit"};
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(v[i] - vitalSeen_[i]) > 0.004) vitalSeen_[i] = v[i], vitalAt_[i] = clock_;
        float al = 1.0f;
        if (!settingsVitals && v[i] >= 0.98) al = float(std::clamp(1.0 - (clock_ - vitalAt_[i] - 3.0) / 0.6, 0.0, 1.0));
        const Rect& r = rs[i];
        if (ui_->hovered({r.x, r.y - 6, r.w, r.h + 12})) {
            al = 1.0f;
            tipAt(r.x + r.w * 0.5f, r.y - 40, host_->text(tips[i]) + " " + std::to_string(int(std::lround(v[i] * 100))));
        }
        if (al <= 0.01f) continue;
        const float f = float(std::clamp(v[i], 0.0, 1.0));
        ui_->rect(r, kRim.withAlpha(0.6f * al));
        if (i == 0) {   // health drains towards the bars: it fills from the right
            gradRect({r.right() - r.w * f, r.y, r.w * f, r.h}, cs[i].lerp(Colour{1, 1, 1, 1}, 0.15f).withAlpha(al),
                     cs[i].lerp(kRim, 0.25f).withAlpha(al), true);
        } else {
            gradRect({r.x, r.y, r.w * f, r.h}, cs[i].lerp(Colour{1, 1, 1, 1}, 0.15f).withAlpha(al), cs[i].lerp(kRim, 0.25f).withAlpha(al), true);
        }
        if (i < 2) ui_->border(r.inset(-1), kGold.withAlpha(0.32f * al), 1.0f, 0);
    }
}

// Bottom left, the portrait gone: "NAME · mood · whom they lead" and under it
// the status groups as diamonds, as before; a click on the name opens the sheet.
void Hud::edgeNameplate(const Snapshot& s, const ActorSnap& a, float ox, float H) {
    const float tx = ox + 64, ny = H - 150;
    const int moodLevel = std::clamp(int((s.detail.mood + 1.0) * 2.5), 0, 4);
    const std::string shownName = ui_->fit(a.name, 300, 20 / 15.f, true);
    const float nw = capsW(shownName, 20, 0.14f);
    const Rect nameHit{tx, ny - 4, nw, 30};
    const bool hot = ui_->hovered(nameHit);
    caps(tx, ny, shownName, hot ? kGoldPale : kText, 20, 0.14f, true, true);
    if (hot) tipAt(tx + nw * 0.5f, ny + 34, host_->text("hud.bar.open_sheet"));
    if (clicked(nameHit)) panel = panel == "sheet" ? "none" : "sheet";
    float x = tx + nw;
    std::string rest = " · " + host_->text("hud.mood." + std::to_string(moodLevel));
    if (!s.social.followers.empty() && a.id == s.controlled) {
        std::string names;
        for (std::size_t i = 0; i < s.social.followers.size() && i < 2; ++i)
            names += (i ? ", " : "") + s.social.followers[i].second;
        if (s.social.followers.size() > 2) names += " +" + std::to_string(s.social.followers.size() - 2);
        rest += " · " + host_->text("hud.bar.leads", {{"who", names}});
    }
    caps(x, ny + 6, ui_->fit(rest, 420, 12 / 15.f), kGold, 12, 0.1f, false, true);
    x += capsW(ui_->fit(rest, 420, 12 / 15.f), 12, 0.1f, false);
    // AL-67: the player's own person can be named, with the cases
    if (a.player || a.id == s.controlled)
        if (diamondButton(x + 22, ny + 12, 20, "a_rename", false, host_->text("hud.name.open"))) openName(a.id);

    // the status groups: five needs, then wants, people and values a little apart
    const HudPalette& P = hudPalette();
    const float row = H - 82;
    float sx = ox + 64 + 36;
    int hovered = -1;
    float cxs[8], ds[8];
    for (int k = 0; k < 8; ++k) {
        ds[k] = k < 5 ? 50.f : 40.f;
        cxs[k] = sx;
        sx += k == 4 ? 82.f : k < 5 ? 66.f : 56.f;
        const float h = ds[k] * 0.7071f;
        if (ui_->hovered({cxs[k] - h, row - h, h * 2, h * 2})) hovered = k;
    }
    static const char* kIcons[] = {"body", "safety", "belonging", "esteem", "calling", "wants", "people", "values"};
    if (hovered >= 0) hover_ = hovered;
    else if (!ui_->hovered({ox + 60, row - 40, sx - ox - 60, 80})) hover_ = -1;
    const int shown = hover_ >= 0 ? hover_ : pin_;
    for (int k = 0; k < 8; ++k) {
        const float d = ds[k], h = d * 0.7071f, cx = cxs[k];
        const bool on = k == shown;
        const double v = k < 5 ? tierValue(s, a, k) : k == 6 ? 0.5 : 0.6;
        diamond(cx, row, d, on, false);
        icon(kIcons[k], {cx - d * 0.24f, row - d * 0.24f, d * 0.48f, d * 0.48f}, on ? kGoldPale : kText);
        if (k < 5) {
            const Colour lc = v < 0.35 ? P.red : v < 0.55 ? P.mid : kGold;
            const Rect strip{cx - 22, row + h + 6, 44, 3};
            ui_->rect(strip, kText.withAlpha(0.2f));
            ui_->rect({strip.x, strip.y, strip.w * float(std::clamp(v, 0.0, 1.0)), strip.h}, lc);
        }
        if (clicked({cx - h, row - h, h * 2, h * 2})) pin_ = pin_ == k ? -1 : k;
    }
    if (shown >= 0) slotCard(s, a, shown, ox + 60, ny - 14, cxs[shown], 410);
}

// AL-78 / AL-80: what is said round the person and what came of their doing,
// over the bars on a grey band blurred at its sides and top (no frame): three
// lines at most, the newest at the bottom, the older paler (100/72/45 %), a
// line living 6 s and 60 ms a letter; the name in gold capitals, the person's
// own in a cold grey-blue; a small diamond glows over whoever's line is shown
// (stuck to the screen's edge with an arrow when they are out of sight). While
// the person talks with someone the band grows: their name and how they feel,
// the last lines, the topics and the talk acts in two columns (keys 1-8), the
// tone as seven diamonds (the mouse wheel). Esc closes it; the world goes on.
void Hud::talkStrip(const Snapshot& s, float W, float H) {
    const ActorSnap* me = actor(s, s.controlled);
    const auto lifeOf = [](const std::string& t) { return 6.0 + 0.06 * double(t.size()) / 1.6; };
    // the lines: in turn (the speech queue), kept when they concern the person
    for (const auto& sp : s.speech)
        if (sp.seq > speechSeq_) {
            speechSeq_ = sp.seq;
            if (sp.line.empty()) continue;
            const double life = lifeOf(sp.line);
            const double at = std::max(clock_, bubbleNext_);
            bubbleNext_ = at + std::min(2.2, life * 0.35);
            bubbleQueue_.push_back(Bubble{sp.speaker, sp.line, at, life});
            if (me && (sp.speaker == me->id || sp.listener == me->id)) {
                const EntityId other = sp.speaker == me->id ? sp.listener : sp.speaker;
                if (other != me->id && other != sim::kNone) stripPartner_ = other, stripPartnerAt_ = clock_;
                if (sp.speaker == me->id) talkClosed_ = sim::kNone;   // the person speaks again: the talk opens
            }
        }
    for (auto it = bubbleQueue_.begin(); it != bubbleQueue_.end();) {
        if (it->born > clock_) {
            ++it;
            continue;
        }
        const ActorSnap* a = actor(s, it->who);
        bool near = me && a;
        if (near && a->id != me->id) near = std::hypot(a->x - me->x, a->y - me->y) < 45.0;
        if (a && near) strip_.push_back({a->name, it->text, 3, a->id == s.controlled, clock_, a->id, it->life});
        it = bubbleQueue_.erase(it);
    }
    if (bubbleQueue_.size() > 12) bubbleQueue_.erase(bubbleQueue_.begin(), bubbleQueue_.end() - 12);
    // the news: what came of the person's doing
    for (const auto& f : s.feedback)
        if (f.seq > stripFbSeq_) {
            stripFbSeq_ = f.seq;
            strip_.push_back({"", f.text, f.kind, false, clock_, sim::kNone, lifeOf(f.text)});
        }
    if (strip_.size() > 24) strip_.erase(strip_.begin(), strip_.end() - 24);
    // the talk: while it goes on, and a little after
    const ActorSnap* partner = actor(s, stripPartner_);
    const bool talking = me && partner && partner->alive && !partner->animal && partner->id != talkClosed_ &&
                         (me->faces == partner->id || clock_ - stripPartnerAt_ < 12.0) &&
                         std::hypot(partner->x - me->x, partner->y - me->y) < 12.0;
    if (talking && !talkOpen_) talkPick_ = 0;
    talkOpen_ = talking;
    if (talking) host_->wheelTarget(partner->id);   // its topics and its feelings come with the snapshot
    // what is shown: the newest lines still alive (in a talk the last four)
    const std::size_t most = talking ? 4 : (compact_ ? 2 : 3);
    std::vector<const StripRow*> rows;
    for (auto it = strip_.rbegin(); it != strip_.rend() && rows.size() < most; ++it)
        if (clock_ - it->born < (talking ? 60.0 : it->life)) rows.push_back(&*it);
    if (rows.empty() && !talking) {
        talkKey_ = -1;
        return;
    }
    // the replies: the topics (say_topic), then the talk acts
    struct Reply {
        std::string label;
        bool say = false;
        int index = 0;
        std::string act;
    };
    std::vector<Reply> replies;
    if (talking) {
        if (s.topicsFor == partner->id)
            for (std::size_t i = 0; i < s.topics.size() && replies.size() < 4; ++i)
                replies.push_back({s.topics[i].label, true, int(i), ""});
        static const char* kActs[] = {"chat", "joke", "tell_story", "gossip", "compliment", "insult"};
        for (const char* a : kActs)
            if (replies.size() < 8) replies.push_back({host_->interactionName(a), false, 0, a});
    }
    const float w = std::min(1000.f, W - 80), rowH = 24;
    const float headH = talking ? 64.f : 0.f;
    const float replyH = talking ? 34.f * float((replies.size() + 1) / 2) + 12 : 0.f;
    const float toneH = talking ? 40.f : 0.f;
    float h = 22 + float(rows.size()) * rowH + headH + replyH + toneH + 8;
    h = std::min(h, H * 0.45f);
    const float bottom = (barTop_ > 0 ? barTop_ : H - 130) - 14 - 22 - 12;
    const Rect r{(W - w) * 0.5f, bottom - h, w, h};
    // the grey band: clear at the top, darker towards the bars, blurred at the sides
    const int bands = 16;
    const float side = std::min(140.f, w * 0.16f);
    for (int k = 0; k < bands; ++k) {
        const float t = float(k) / float(bands - 1);
        const Colour g = Colour::rgb(34, 34, 37, (talking ? 0.2f : 0.08f) + 0.55f * t);
        const Rect b{r.x, r.y + r.h * float(k) / float(bands), r.w, r.h / float(bands) + 1};
        gradRect({b.x, b.y, side, b.h}, g.withAlpha(0), g, false);
        ui_->rect({b.x + side, b.y, b.w - 2 * side, b.h}, g);
        gradRect({b.right() - side, b.y, side, b.h}, g, g.withAlpha(0), false);
    }
    if (talking) ui_->claim(r);
    const float tx = r.x + side * 0.6f, tw = r.w - side * 1.2f;
    float y = r.y + 14;
    if (talking) {
        // the head: whom the person talks with, and how they feel towards the person
        caps(tx, y, partner->name, kGold, 20, 0.14f, true, true);
        std::string who;
        for (const auto& rel : s.detail.relations)
            if (rel.id == partner->id && !rel.kin.empty()) who = host_->text("hud.rel." + rel.kin);
        if (!who.empty()) label(tx + capsW(partner->name, 20, 0.14f) + 10, y + 4, "· " + who, kMute, 13, true);
        if (s.wheel.target == partner->id) {
            const HudPalette& P = hudPalette();
            const struct {
                const char* key;
                double v;
                Colour c;
            } m[] = {{"hud.axis.affinity", (s.wheel.toActor.affinity + 1) * 0.5, P.grn},
                     {"hud.axis.trust", (s.wheel.toActor.trust + 1) * 0.5, P.blu},
                     {"hud.axis.fear", s.wheel.toActor.fear, P.red}};
            float mx = r.right() - side * 0.6f - 3 * 150;
            for (const auto& q : m) {
                label(mx, y - 2, host_->text(q.key), kMute, 12, true);
                frameBar({mx, y + 18, 130, 5}, float(std::clamp(q.v, 0.0, 1.0)), q.c);
                mx += 150;
            }
        }
        y += headH - 14;
        ui_->line(tx, y - 8, tx + tw, y - 8, kGold.withAlpha(0.3f));
    }
    // the lines, the oldest first
    static const float kFade[] = {1.0f, 0.72f, 0.45f, 0.35f};
    for (std::size_t i = rows.size(); i-- > 0;) {
        const StripRow& row = *rows[i];
        const float age = float(clock_ - row.born);
        const float alpha = kFade[std::min<std::size_t>(i, 3)] * std::clamp(age / 0.25f, 0.0f, 1.0f) *
                            (talking ? 1.f : std::clamp(float(row.life) - age, 0.0f, 0.6f) / 0.6f);
        float x = tx;
        if (row.kind == 3) {
            const Colour nc = row.mine ? Colour::rgb(150, 172, 192) : kGold;
            caps(x, y + 2, row.who, nc.withAlpha(alpha), 13, 0.12f, false, true);
            x += capsW(row.who, 13, 0.12f, false) + 12;
            // murmurs and whispers (a line in brackets or with dots) are quieter
            const bool soft = !row.text.empty() && (row.text.front() == '(' || row.text.rfind("...") == row.text.size() - 3);
            label(x, y, ui_->fit(row.text, tx + tw - x, 15 / 15.f), (soft ? kMute : kText).withAlpha(alpha), 15, true);
            // the speaker's diamond, over their head or at the screen's edge
            if (const ActorSnap* a = actor(s, row.speaker); a && i < 2 && age < row.life) {
                float hx, hy;
                if (host_->overHead(*a, hx, hy)) {
                    const Colour f = kGoldPale.withAlpha(0.9f * alpha);
                    diamond(hx, hy - 18, 14, true, false, &f);
                } else if (me) {
                    const float rel = wrapA(float(std::atan2(a->y - me->y, a->x - me->x)) - float(host_->cameraYaw()));
                    const float ex = rel > 0 ? 34.f : W - 34.f, ey = H * 0.42f;
                    const Colour f = kGoldPale.withAlpha(0.9f * alpha);
                    diamond(ex, ey, 14, true, false, &f);
                    const float d = rel > 0 ? -1.f : 1.f;
                    const SDL_FPoint p[4] = {{ex + d * 14, ey - 7}, {ex + d * 24, ey}, {ex + d * 14, ey + 7}, {ex + d * 14, ey + 7}};
                    const Colour pc[4] = {f, f, f, f};
                    quad(p, pc);
                }
            }
        } else {
            const HudPalette& P = hudPalette();
            const Colour c = row.kind == 1 ? P.grn.lerp(kText, 0.3f) : row.kind == 2 ? P.red.lerp(kText, 0.25f) : kMute;
            label(x, y + 1, ui_->fit(row.text, tw, 14 / 15.f), c.withAlpha(alpha), 14, true);
        }
        y += rowH;
    }
    if (!talking) {
        talkKey_ = -1;
        return;
    }
    // the replies in two columns, capitals; keys 1-8, the mouse
    y += 6;
    const bool canAct = s.controlled != sim::kNone && s.controlled != partner->id;
    const float colW = (tw - 20) * 0.5f;
    if (ui_->hovered(r) && in_->wheel != 0 && !in_->shift)
        tone_ = std::clamp(tone_ + (in_->wheel > 0 ? -1 : 1), 0, 6);
    for (std::size_t i = 0; i < replies.size(); ++i) {
        const Rect row{tx + float(i % 2) * (colW + 20) + 14, y + float(i / 2) * 34, colW - 14, 30};
        const bool hot = ui_->hovered(row);
        if (hot) talkPick_ = int(i);
        const bool picked = int(i) == talkPick_;
        if (picked) selRow(row);
        const float kw = keyCap(row.x + 8, row.y + 4, std::to_string(i + 1), 12);
        caps(row.x + 16 + kw, row.y + 6, ui_->fit(replies[i].label, row.w - kw - 26, 14 / 15.f), picked ? kGoldPale : kText, 14,
             0.08f, false, true);
        if ((clicked(row) || talkKey_ == int(i)) && canAct) {
            if (replies[i].say)
                host_->simOp("say_topic", {{"id", s.controlled}, {"target", partner->id}, {"index", replies[i].index}});
            else {
                sim::Json args{{"id", s.controlled}, {"interaction", replies[i].act}, {"target", partner->id}};
                if (tone_ >= 0 && tone_ < int(s.wheel.tones.size())) args["tone"] = s.wheel.tones[std::size_t(tone_)];
                host_->command("perform", args);
            }
        }
    }
    talkKey_ = -1;
    y += replyH - 6;
    // the tone: seven diamonds, the chosen one lit, its name beside them
    float dx = tx + 20;
    for (int i = 0; i < 7; ++i) {
        const float d = 18, hh = d * 0.7071f;
        const Rect box{dx - hh, y + 14 - hh, hh * 2, hh * 2};
        if (i == tone_) {
            const Colour f = kGold.withAlpha(0.35f);
            diamond(dx, y + 14, d, true, false, &f);
        } else {
            diamond(dx, y + 14, d, ui_->hovered(box), false);
        }
        if (clicked(box)) tone_ = i;
        dx += 30;
    }
    caps(dx + 6, y + 6, host_->text("hud.wheel.tone") + ": " + host_->text("hud.wheel.tone." + std::to_string(tone_)), kMute, 12,
         0.1f, false, true);
    const std::string esc = host_->text("hud.bar.talk_close");
    const float ew = textW(esc, 12);
    label(tx + tw - ew, y + 8, esc, kMute, 12, true);
    const float kw = keyCap(tx + tw - ew - 44, y + 4, "Esc", 11);
    (void)kw;
}

} // namespace lr::game
