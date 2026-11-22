// The primitives of the "Edge" style (AL-63, gdd/ui/four_styles/STYLE_3_SKYRIM_MOD.md):
// diamonds instead of circles (icons, badges, the medallion of a bar), thin
// bars with a gold contour, the selected person's bar with its medallion and
// filigree, frosted glass, the selected row (gold gradient, tick and bracket),
// key caps, and light spaced capitals for titles, tabs, names and buttons.
//
// Shapes are drawn as geometry (SDL_RenderGeometry), so they stay crisp at any
// interface scale; every colour comes from the theme's palette.

#include <algorithm>
#include <cmath>

#include "game_view/hud.hpp"
#include "game_view/hud_style.hpp"

namespace altera::game {
namespace {

using ui::Colour;
using ui::Rect;
using namespace hudroles;

SDL_FColor fc(const Colour& c) { return {c.r, c.g, c.b, c.a}; }

// UTF-8 to upper case for the scripts the game speaks (Latin, Latin-1, Cyrillic).
std::string upperUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out += char(c >= 'a' && c <= 'z' ? c - 32 : c);
            ++i;
            continue;
        }
        int n = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + std::size_t(n) > s.size()) n = 1;
        std::uint32_t cp = n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : n == 4 ? (c & 0x07) : c;
        for (int k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + std::size_t(k)]) & 0x3F);
        if (cp >= 0x430 && cp <= 0x44F) cp -= 0x20;                    // а..я
        else if (cp >= 0x450 && cp <= 0x45F) cp -= 0x50;               // ѐ..џ (ё)
        else if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) cp -= 0x20;   // à..þ
        if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out.append(s, i, std::size_t(n));
        }
        i += std::size_t(n);
    }
    return out;
}

// The code points of a UTF-8 string, each as its own string.
std::vector<std::string> glyphs(const std::string& s) {
    std::vector<std::string> g;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        g.push_back(s.substr(i, n));
        i += n;
    }
    return g;
}

} // namespace

std::string Hud::upper(const std::string& s) { return upperUtf8(s); }

// ---------------------------------------------------------------- geometry
void Hud::quad(const SDL_FPoint p[4], const Colour c[4]) {
    SDL_Vertex v[4];
    for (int k = 0; k < 4; ++k) v[k] = {p[k], fc(c[k]), {0, 0}};
    const int idx[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(sdl_, nullptr, v, 4, idx, 6);
}

void Hud::thickLine(float x0, float y0, float x1, float y1, float w, const Colour& c) {
    const float dx = x1 - x0, dy = y1 - y0, l = std::max(1e-4f, std::hypot(dx, dy));
    const float nx = -dy / l * w * 0.5f, ny = dx / l * w * 0.5f;
    const SDL_FPoint p[4] = {{x0 + nx, y0 + ny}, {x1 + nx, y1 + ny}, {x1 - nx, y1 - ny}, {x0 - nx, y0 - ny}};
    const Colour cs[4] = {c, c, c, c};
    quad(p, cs);
}

void Hud::gradRect(const Rect& r, const Colour& left, const Colour& right, bool vertical) {
    const SDL_FPoint p[4] = {{r.x, r.y}, {r.right(), r.y}, {r.right(), r.bottom()}, {r.x, r.bottom()}};
    const Colour c[4] = {left, vertical ? left : right, right, vertical ? right : left};
    quad(p, c);
}

// A square stood on its corner, centred at (cx, cy), `size` across its sides
// (the bounding box is size * sqrt 2 wide). Glass inside, the gold contour,
// and for `twice` a second, fainter contour four pixels in.
void Hud::diamond(float cx, float cy, float size, bool lit, bool twice, const Colour* fill) {
    const float h = size * 0.7071f;
    const SDL_FPoint p[4] = {{cx, cy - h}, {cx + h, cy}, {cx, cy + h}, {cx - h, cy}};
    const HudPalette& P = hudPalette();
    if (fill) {
        const Colour c[4] = {*fill, *fill, *fill, *fill};
        quad(p, c);
    } else {
        // the glass darkens towards the bottom right, as the mockup's gradient
        const Colour a = Colour::rgb(40, 44, 48, 0.9f).lerp(P.panel, 0.4f), b = P.rim.withAlpha(0.92f);
        const Colour c[4] = {a, a.lerp(b, 0.5f), b, a.lerp(b, 0.5f)};
        quad(p, c);
    }
    if (lit) {   // the glow of a chosen icon
        for (int k = 1; k <= 4; ++k) {
            const float g = h + float(k) * 2.0f;
            const Colour gc = P.goldPale.withAlpha(0.16f / float(k));
            const SDL_FPoint q[4] = {{cx, cy - g}, {cx + g, cy}, {cx, cy + g}, {cx - g, cy}};
            for (int e = 0; e < 4; ++e) thickLine(q[e].x, q[e].y, q[(e + 1) % 4].x, q[(e + 1) % 4].y, 2.0f, gc);
        }
    }
    const Colour edge = lit ? P.goldPale : P.gold;
    const float lw = lit ? 2.0f : 1.5f;
    for (int e = 0; e < 4; ++e) thickLine(p[e].x, p[e].y, p[(e + 1) % 4].x, p[(e + 1) % 4].y, lw, edge);
    if (twice && h > 12) {
        const float g = h - 5.5f;
        const SDL_FPoint q[4] = {{cx, cy - g}, {cx + g, cy}, {cx, cy + g}, {cx - g, cy}};
        for (int e = 0; e < 4; ++e) thickLine(q[e].x, q[e].y, q[(e + 1) % 4].x, q[(e + 1) % 4].y, 1.0f, P.gold.withAlpha(0.5f));
    }
    ui_->claim({cx - h, cy - h, h * 2, h * 2});
}

// A diamond icon button: the hit area is the diamond's bounding box.
bool Hud::diamondButton(float cx, float cy, float size, const std::string& iconName, bool on, const std::string& tip,
                        bool dim) {
    const float h = size * 0.7071f;
    const Rect box{cx - h, cy - h, h * 2, h * 2};
    const bool over = ui_->hovered(box), hot = over && !dim;
    if (on) {
        const Colour f = kGold.withAlpha(0.25f);
        diamond(cx, cy, size, true, false, &f);
    } else {
        diamond(cx, cy, size, hot, false);
    }
    const float s = std::round(size * 0.52f);
    icon(iconName, {cx - s * 0.5f, cy - s * 0.5f, s, s},
         (on ? kHot : hot ? kGoldPale : kText).withAlpha(dim ? 0.35f : 1.0f));
    if (over && !tip.empty()) tipAt(cx, box.bottom() + 8, tip);
    return clicked(box);   // a dimmed one still answers (it says why it cannot)
}

// A meter: `fill` over the dark track, a gold hairline round it.
void Hud::frameBar(const Rect& r, float frac, const Colour& fill) {
    ui_->rect(r, kRim.withAlpha(0.6f));
    const float f = std::clamp(frac, 0.f, 1.f);
    if (f > 0) gradRect({r.x, r.y, r.w * f, r.h}, fill.lerp(Colour{1, 1, 1, 1}, 0.15f), fill.lerp(kRim, 0.25f), true);
    ui_->border(r.inset(-1), kGold.withAlpha(0.32f), 1.0f, 0);
}

// The selected person's bar, the "enemy bar" of the reference: a frame with a
// double contour, a diamond medallion with an icon at its centre and thin gold
// arcs of filigree over the medallion.
void Hud::filiBar(const Rect& r, float frac, const Colour& fill, const std::string& iconName) {
    // filigree: two arcs rising to the medallion from either side
    const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    for (int side = -1; side <= 1; side += 2) {
        const float x0 = cx + side * 150.f, x1 = cx + side * 30.f;
        float px = x0, py = r.y - 2;
        for (int k = 1; k <= 16; ++k) {
            const float t = float(k) / 16.f;
            const float x = x0 + (x1 - x0) * t, y = r.y - 2 - std::sin(t * 3.14159f * 0.5f) * 16.f;
            thickLine(px, py, x, y, 1.0f, kGold.withAlpha(0.7f));
            px = x, py = y;
        }
    }
    ui_->rect(r, Colour::rgb(0, 0, 0, 0.6f));
    const float f = std::clamp(frac, 0.f, 1.f);
    if (f > 0) gradRect({r.x, r.y, r.w * f, r.h}, fill.lerp(Colour{1, 1, 1, 1}, 0.2f), fill.lerp(kRim, 0.3f), true);
    ui_->border(r, kGold, 1.0f, 0);
    ui_->border(r.inset(-3), kRim.withAlpha(0.7f), 2.0f, 0);
    ui_->border(r.inset(-4), kGold.withAlpha(0.45f), 1.0f, 0);
    const Colour m = kRim.withAlpha(0.95f);
    diamond(cx, cy, 30, false, false, &m);
    if (!iconName.empty()) icon(iconName, {cx - 10, cy - 10, 20, 20}, kGoldPale);
    ui_->claim(r.inset(-6));
}

// Frosted glass: the theme's frost (or frost_solid where blur is off), a gold
// hairline down the left edge, `framed`: a 1 px gold frame round it.
void Hud::glass(const Rect& r, bool framed) {
    box(framed ? Skin::Paper : Skin::Leather, r, 2);
}

// The selected row of a list: a gold gradient from the left, a 3 px tick at
// the left edge and the bracket "[" set out past it.
void Hud::selRow(const Rect& r) {
    const HudPalette& P = hudPalette();
    gradRect({r.x, r.y, r.w * 0.8f, r.h}, P.sel, P.sel.withAlpha(0), false);
    ui_->rect({r.x, r.y, 3, r.h}, kGold);
    const float x = r.x - 10, y0 = r.y + std::min(8.f, r.h * 0.2f), y1 = r.bottom() - std::min(8.f, r.h * 0.2f);
    ui_->line(x, y0, x, y1, kGold);
    ui_->line(x, y0, x + 6, y0, kGold);
    ui_->line(x, y1, x + 6, y1, kGold);
}

// A key cap: a dark rounded plate with a light contour and the key's name.
float Hud::keyCap(float x, float y, const std::string& k, float px) {
    const HudPalette& P = hudPalette();
    const float w = std::max(px * 1.6f, textW(k, px) + 12), h = px * 1.6f;
    const Rect r{x, y, w, h};
    ui_->roundRect(r, P.key, P.theme.size("key_radius", 6.0f));
    ui_->border(r, P.keyEdge, 1.0f, P.theme.size("key_radius", 6.0f));
    label(x + (w - textW(k, px)) * 0.5f, y + (h - ui_->textHeight(px / 15.f)) * 0.5f, k, kText, px);
    return w;
}

// A vertical gold tick (the family list, the quest-list look).
void Hud::tick(float x, float y, float h, const Colour& c) { ui_->rect({x, y, 3, h}, c); }

// Light spaced capitals: `spacing` em between the letters (the theme's
// title_spacing when < 0). Titles use the title face (Jost Light).
float Hud::capsW(const std::string& s, float px, float spacing, bool titleFace) const {
    const float em = spacing < 0 ? hudPalette().theme.size("title_spacing", 0.14f) : spacing;
    const std::string u = hudPalette().theme.flag("caps") ? upperUtf8(s) : s;
    const auto g = glyphs(u);
    float w = 0;
    for (const auto& c : g) w += ui_->textWidth(c, px / 15.f, titleFace) + em * px;
    return g.empty() ? 0 : w - em * px;
}

void Hud::caps(float x, float y, const std::string& s, const Colour& c, float px, float spacing, bool titleFace,
               bool shadow) {
    const float em = spacing < 0 ? hudPalette().theme.size("title_spacing", 0.14f) : spacing;
    const std::string u = hudPalette().theme.flag("caps") ? upperUtf8(s) : s;
    for (const auto& g : glyphs(u)) {
        if (shadow) ui_->text(x, y + 1.5f, g, kRim.withAlpha(0.85f), px / 15.f, titleFace);
        ui_->text(x, y, g, c, px / 15.f, titleFace);
        x += ui_->textWidth(g, px / 15.f, titleFace) + em * px;
    }
}

// A text button of the Edge style: a 1 px gold contour, capitals; the main
// one with a gold glaze and a bright contour.
bool Hud::edgeButton(const Rect& r, const std::string& s, bool primary, bool enabled) {
    const bool hot = enabled && ui_->hovered(r);
    if (primary) gradRect(r, kGold.withAlpha(0.28f), kGold.withAlpha(0.12f), true);
    else ui_->rect(r, Colour{1, 1, 1, 0.03f});
    ui_->border(r, (primary ? kGold : kGold.withAlpha(0.45f)).withAlpha(enabled ? (primary ? 1.f : 0.45f) : 0.2f), 1.0f, 0);
    if (hot) ui_->border(r.inset(1), kGoldPale.withAlpha(0.6f), 1.0f, 0);
    const float px = primary ? 16.f : 15.f;
    const float sp = hudPalette().theme.size(primary ? "title_spacing" : "button_spacing", 0.12f);
    const float w = capsW(s, px, sp);
    caps(r.x + (r.w - w) * 0.5f, r.y + (r.h - ui_->textHeight(px / 15.f)) * 0.5f, s,
         (primary ? kHot : hot ? kGoldPale : kText).withAlpha(enabled ? 1.f : 0.35f), px, sp);
    return clicked(r) && enabled;
}

} // namespace altera::game
