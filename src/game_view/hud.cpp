#include "game_view/hud.hpp"
#include "game_view/hud_style.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include "engine/world_source/png_io.hpp"
#include "sim/worldgen.hpp"

namespace altera::game {
namespace {

namespace fs = std::filesystem;
using ui::Colour;
using ui::Rect;
constexpr float kPiF = 3.14159265f;

using namespace hudroles;   // the palette's roles (hud_style.hpp): colours from the theme (AL-62)

Colour barColour(double v) { return v < 0.35 ? kBad : v < 0.55 ? kMid : kGood; }
Colour ringColour(double v) { return v < 0.35 ? kRingBad : v < 0.55 ? kRingMid : kRingGood; }

struct Px {
    float r, g, b, a;
};
Px rgb(std::uint32_t hex, float a = 1) {
    return {float((hex >> 16) & 255) / 255.f, float((hex >> 8) & 255) / 255.f, float(hex & 255) / 255.f, a};
}
void over(Px& d, const Px& s) {
    const float a = s.a + d.a * (1 - s.a);
    if (a <= 1e-6f) {
        d = {0, 0, 0, 0};
        return;
    }
    d.r = (s.r * s.a + d.r * d.a * (1 - s.a)) / a;
    d.g = (s.g * s.a + d.g * d.a * (1 - s.a)) / a;
    d.b = (s.b * s.a + d.b * d.a * (1 - s.a)) / a;
    d.a = a;
}
// Signed distance to a rounded box centred at (cx, cy), half size (hx, hy).
float boxDist(float x, float y, float cx, float cy, float hx, float hy, float r) {
    const float qx = std::fabs(x - cx) - (hx - r), qy = std::fabs(y - cy) - (hy - r);
    const float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - r;
}
float cover(float d) { return std::clamp(0.5f - d, 0.f, 1.f); }
// Coverage of the band a < d <= b of a distance field, anti-aliased.
float band(float d, float a, float b) { return std::clamp(std::min(d - a, b - d) + 0.5f, 0.f, 1.f); }

bool loadImage(const fs::path& p, int& w, int& h, std::vector<std::uint8_t>& rgba) {
    std::string why;
    const auto img = engine::world_source::readPng(p, &why);
    if (!img || img->bits != 8) return false;
    w = int(img->width);
    h = int(img->height);
    rgba.assign(std::size_t(w) * h * 4, 255);
    for (std::size_t i = 0, n = std::size_t(w) * h; i < n; ++i) {
        const int c = img->channels;
        const std::uint8_t* s = img->bytes.data() + i * c;
        std::uint8_t* d = rgba.data() + i * 4;
        if (c >= 3) d[0] = s[0], d[1] = s[1], d[2] = s[2];
        else d[0] = d[1] = d[2] = s[0];
        d[3] = c == 4 ? s[3] : c == 2 ? s[1] : 255;
    }
    return true;
}

SDL_Texture* makeTexture(SDL_Renderer* r, int w, int h, const std::vector<Px>& px) {
    std::vector<std::uint8_t> b(std::size_t(w) * h * 4);
    for (std::size_t i = 0; i < px.size(); ++i) {
        b[i * 4 + 0] = std::uint8_t(std::clamp(px[i].r, 0.f, 1.f) * 255 + 0.5f);
        b[i * 4 + 1] = std::uint8_t(std::clamp(px[i].g, 0.f, 1.f) * 255 + 0.5f);
        b[i * 4 + 2] = std::uint8_t(std::clamp(px[i].b, 0.f, 1.f) * 255 + 0.5f);
        b[i * 4 + 3] = std::uint8_t(std::clamp(px[i].a, 0.f, 1.f) * 255 + 0.5f);
    }
    SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return nullptr;
    SDL_UpdateTexture(t, nullptr, b.data(), w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    return t;
}

constexpr int kMargin = 8, kShadow = 10;

} // namespace

// ---------------------------------------------------------------- setup
bool Hud::init(SDL_Renderer* renderer, const fs::path& root, std::string* why) {
    sdl_ = renderer;
    root_ = root;
    // AL-62: the look from content/ui/themes/<id>.json (a missing file: the Edge defaults)
    {
        std::string whyTheme;
        std::vector<std::string> skipped;
        const fs::path themes = root / "content/ui/themes";
        applyHudTheme(UiTheme::load(themes / (themeId + ".json"), &whyTheme, &skipped));
        if (!whyTheme.empty()) SDL_Log("hud: theme: %s", whyTheme.c_str());
        for (const auto& k : skipped) SDL_Log("hud: theme %s: %s is not a valid value, the default stays", themeId.c_str(), k.c_str());
        themeList.clear();
        for (const auto& id : UiTheme::list(themes)) themeList.push_back({id, UiTheme::load(themes / (id + ".json")).nameKey});
    }
    const fs::path ui = root / "assets/altera_game/ui";
    if (!loadImage(ui / "leather.png", leather_.w, leather_.h, leather_.rgba) ||
        !loadImage(ui / "paper.png", paper_.w, paper_.h, paper_.rgba)) {
        if (why) *why = "hud: cannot read the leather/paper tiles in " + ui.string();
        return false;
    }
    Image ic;
    if (loadImage(ui / "icons.png", ic.w, ic.h, ic.rgba)) {
        icons_ = SDL_CreateTexture(sdl_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, ic.w, ic.h);
        if (icons_) {
            SDL_UpdateTexture(icons_, nullptr, ic.rgba.data(), ic.w * 4);
            SDL_SetTextureBlendMode(icons_, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(icons_, SDL_SCALEMODE_LINEAR);
        }
    }
    Image pv;
    if (loadImage(ui / "build_previews.png", pv.w, pv.h, pv.rgba)) {
        previews_ = SDL_CreateTexture(sdl_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, pv.w, pv.h);
        if (previews_) {
            SDL_UpdateTexture(previews_, nullptr, pv.rgba.data(), pv.w * 4);
            SDL_SetTextureBlendMode(previews_, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(previews_, SDL_SCALEMODE_LINEAR);
        }
        try {
            const sim::Json j = sim::loadJsonFile((ui / "build_previews.json").string());
            previewCell_ = j.value("cell", 112);
            const sim::Json pieces = j.value("pieces", sim::Json::object());
            for (const auto& [k, v] : pieces.items()) previewIndex_[k] = v.get<int>();
        } catch (const std::exception&) {
        }
    }
    try {
        const sim::Json j = sim::loadJsonFile((ui / "icons.json").string());
        iconCell_ = j.value("cell", 48);
        const sim::Json icons = j.value("icons", sim::Json::object());
        for (const auto& [k, v] : icons.items()) iconIndex_[k] = v.get<int>();
        wheelSpec_ = sim::loadJsonFile((root / "content/ui/wheel.json").string());
        craftSpec_ = sim::loadJsonFile((root / "content/ui/craft.json").string());
    } catch (const std::exception& e) {
        if (why) *why = std::string("hud: ") + e.what();
        return false;
    }
    return true;
}

void Hud::shutdown() {
    for (auto& [k, t] : skins_) SDL_DestroyTexture(t);
    skins_.clear();
    for (auto& [k, t] : portraits_) SDL_DestroyTexture(t);
    portraits_.clear();
    if (icons_) SDL_DestroyTexture(icons_);
    icons_ = nullptr;
    if (previews_) SDL_DestroyTexture(previews_);
    previews_ = nullptr;
    if (minimap_) SDL_DestroyTexture(minimap_);
    minimap_ = nullptr;
    if (worldJob_.valid()) worldJob_.wait();
    if (worldTex_) SDL_DestroyTexture(worldTex_);
    worldTex_ = nullptr;
    worldShown_ = {};
}

// The palette and the theme it came from (hud_style.hpp, AL-62).
namespace {
void fillPalette(HudPalette& p, const UiTheme& t) {
    p.theme = t;
    const auto c = [&](const char* k) { return toColour(t.colour(k)); };
    p.panel = c("frost_solid");
    if (t.flag("blur")) p.panel = c("frost");
    p.card = c("card");
    p.tip = c("tip");
    p.gold = c("acc");
    p.goldPale = c("gold2");
    p.goldDark = c("gold_dark");
    p.rim = c("rim");
    p.text = c("ink");
    p.mute = c("mute");
    p.hot = c("hot");
    // writing on a card: the cards are glass like the panels, so the same bone and ash
    p.cardInk = c("ink");
    p.cardInkSoft = c("mute");
    p.cardLine = c("acc");
    p.cardRed = c("bad");
    p.good = c("good");
    p.mid = c("warn");
    p.bad = c("bad");
    p.ringGood = c("good");
    p.ringMid = c("warn");
    p.ringBad = c("bad");
    p.dark = c("on_gold");   // style G: dark ink on solid gold; Edge: light on a gold glaze
    p.seal = c("red");
    p.red = c("red");
    p.grn = c("grn");
    p.blu = c("blu");
    p.unk = c("unk");
    p.line = c("line");
    p.frame = c("frame");
    p.sel = c("sel");
    p.key = c("key");
    p.keyEdge = c("key_edge");
    p.dim = c("dim");
}
} // namespace

HudPalette& hudPalette() {
    static HudPalette p = [] {
        HudPalette q;
        fillPalette(q, UiTheme());
        return q;
    }();
    return p;
}

void applyHudTheme(const UiTheme& t) { fillPalette(hudPalette(), t); }

// ---------------------------------------------------------------- painting
namespace {
Px px(const Colour& c, float alpha = -1) { return {c.r, c.g, c.b, alpha < 0 ? c.a : alpha}; }
} // namespace

SDL_Texture* Hud::skin(Skin kind, int w, int h, float radius, std::uint32_t colour) {
    char key[96];
    std::snprintf(key, sizeof key, "%d:%d:%d:%.1f:%06x", int(kind), w, h, radius, colour);
    const auto it = skins_.find(key);
    if (it != skins_.end()) return it->second;
    const HudPalette& P = hudPalette();
    const bool textured = P.theme.flag("textured");
    const float line = std::max(1.0f, P.theme.size("line", 1.0f));
    const int W = w + 2 * kMargin, H = h + 2 * kMargin + kShadow;
    std::vector<Px> pxs(std::size_t(W) * H, Px{0, 0, 0, 0});
    const float cx = kMargin + w * 0.5f, cy = kMargin + h * 0.5f, hx = w * 0.5f, hy = h * 0.5f;
    const bool plate = kind == Skin::Leather || kind == Skin::Paper;
    // Glass panels and cards keep the theme's small corner; round buttons stay round.
    const bool chip = kind == Skin::Chip || kind == Skin::ChipOn;   // Edge: chips are square-cornered too (AL-66)
    const float rr = plate && !textured ? std::min(radius, P.theme.size("radius", 2.0f))
                   : chip && !textured  ? std::min(radius, P.theme.size("button_radius", 2.0f))
                                        : radius;
    const float r = std::min(rr, std::min(hx, hy));
    const bool paperLike = kind == Skin::Paper;
    const bool ringOnly = kind == Skin::Ring;
    const float outer = textured ? (kind == Skin::Leather ? 5.0f : paperLike ? 4.0f : ringOnly ? 6.5f
                                   : kind == Skin::Chip || kind == Skin::ChipOn ? 1.5f : 0.0f)
                                 : (ringOnly ? 4.5f : kind == Skin::Disc ? 0.0f : kind == Skin::Seal ? 1.5f : line);
    const float shadowA = (kind == Skin::Button || kind == Skin::ButtonOn || kind == Skin::Chip || kind == Skin::ChipOn) ? 0.0f
                        : kind == Skin::Seal ? 0.35f : textured ? 0.3f : 0.22f;
    const float shadowY = kind == Skin::Seal ? 3.0f : textured ? 9.0f : 6.0f;
    const Px gold = px(P.gold), goldPale = px(P.goldPale), goldDark = px(P.goldDark), rim = px(P.rim);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float fx = x + 0.5f, fy = y + 0.5f;
            const float d = boxDist(fx, fy, cx, cy, hx, hy, r);
            Px p{0, 0, 0, 0};
            if (shadowA > 0) {
                const float ds = boxDist(fx, fy - shadowY, cx, cy, hx, hy, r) - outer;
                p = Px{0, 0, 0, shadowA * cover(ds)};
            }
            // the body
            if (!ringOnly && d < 0.5f) {
                Px body{0, 0, 0, 0};
                const Image* tile = !textured ? nullptr : kind == Skin::Leather ? &leather_ : paperLike ? &paper_ : nullptr;
                if (kind == Skin::Leather) body = px(P.panel);
                else if (paperLike) body = px(P.card);
                else if (kind == Skin::Button) body = textured ? Px{0, 0, 0, 0.22f} : px(P.panel, 0.6f);
                else if (kind == Skin::ButtonOn) body = textured ? gold : px(P.gold, 0.30f);
                else if (kind == Skin::Seal) body = px(P.seal);
                else if (kind == Skin::Chip) body = textured ? px(P.cardInk, 0.08f) : px(P.text, 0.04f);
                else if (kind == Skin::ChipOn) body = textured ? px(P.dark) : px(P.gold, 0.24f);
                else if (kind == Skin::Disc) body = rgb(colour);
                if (tile && tile->w > 0) {
                    const std::uint8_t* t = tile->rgba.data() + (std::size_t(y % tile->h) * tile->w + x % tile->w) * 4;
                    over(body, Px{t[0] / 255.f, t[1] / 255.f, t[2] / 255.f, t[3] / 255.f});
                }
                const float inside = -d;
                if (kind == Skin::Leather && textured) {
                    if (boxDist(fx, fy - 2, cx, cy, hx, hy, r) > 0) over(body, Px{1, 1, 1, 0.10f});
                    if (boxDist(fx, fy + 3, cx, cy, hx, hy, r) > 0) over(body, Px{0, 0, 0, 0.40f});
                } else if (kind == Skin::Leather) {
                    // glass: a faint sheen at the top, the gold hairline down the left edge
                    over(body, Px{1, 1, 1, 0.035f * std::clamp(1.0f - (fy - (cy - hy)) / std::max(1.0f, hy), 0.f, 1.f)});
                    if (fx - (cx - hx) < line + 0.5f) over(body, px(P.gold, 0.55f));
                } else if (paperLike && textured) {
                    over(body, Px{gold.r, gold.g, gold.b, 0.38f * std::exp(-inside / 9.0f)});
                    if (inside < 2) over(body, Px{1, 1, 1, 0.45f});
                } else if (kind == Skin::Button && textured) {
                    if (boxDist(fx, fy - 2, cx, cy, hx, hy, r) > 0) over(body, Px{0, 0, 0, 0.45f});
                    if (boxDist(fx, fy + 1, cx, cy, hx, hy, r) > 0) over(body, Px{1, 1, 1, 0.10f});
                } else if (kind == Skin::ButtonOn && textured) {
                    if (boxDist(fx, fy - 2, cx, cy, hx, hy, r) > 0) over(body, px(P.hot));
                    if (boxDist(fx, fy + 3, cx, cy, hx, hy, r) > 0) over(body, goldDark);
                } else if (kind == Skin::Seal) {
                    Px lit = px(P.seal.lerp(Colour{1, 1, 1, 1}, 0.3f)), shade = px(P.seal.lerp(P.rim, 0.5f));
                    if (boxDist(fx, fy - 2, cx, cy, hx, hy, r) > 0) over(body, lit);
                    if (boxDist(fx, fy + 3, cx, cy, hx, hy, r) > 0) over(body, shade);
                    if (inside > 7 && inside < 8.5f) over(body, shade);
                } else if (kind == Skin::Chip && textured) {
                    if (inside < 1.5f) over(body, Px{gold.r, gold.g, gold.b, 0.5f});
                }
                body.a *= cover(d);
                over(p, body);
            }
            // the rings outside
            if (outer > 0 && d > -0.5f && d < outer + 0.5f) {
                Px ring{0, 0, 0, 0};
                if (!textured) {
                    if (ringOnly) {   // the double contour of a portrait: gold, a dark gap, gold faint
                        over(ring, Px{gold.r, gold.g, gold.b, band(d, 0, 1.5f)});
                        over(ring, Px{rim.r, rim.g, rim.b, 0.75f * band(d, 1.5f, 3.0f)});
                        over(ring, Px{gold.r, gold.g, gold.b, 0.5f * band(d, 3.0f, 4.5f)});
                    } else {
                        const float a = kind == Skin::ButtonOn ? 1.0f : kind == Skin::Seal ? 0.8f
                                      : kind == Skin::Leather ? 0.35f : P.frame.a;
                        const Px e = kind == Skin::ButtonOn ? goldPale : gold;
                        over(ring, Px{e.r, e.g, e.b, a * band(d, 0, outer)});
                    }
                } else if (kind == Skin::Leather) {
                    over(ring, Px{goldPale.r, goldPale.g, goldPale.b, band(d, 0, 1.5f)});
                    over(ring, Px{goldDark.r, goldDark.g, goldDark.b, band(d, 1.5f, 3.5f)});
                    over(ring, Px{rim.r, rim.g, rim.b, band(d, 3.5f, 5.0f)});
                } else if (paperLike) {
                    over(ring, Px{goldDark.r, goldDark.g, goldDark.b, band(d, 0, 1.5f)});
                    over(ring, Px{rim.r, rim.g, rim.b, band(d, 1.5f, 4.0f)});
                } else if (ringOnly) {
                    over(ring, Px{goldPale.r, goldPale.g, goldPale.b, band(d, 0, 2.0f)});
                    over(ring, Px{goldDark.r, goldDark.g, goldDark.b, band(d, 2.0f, 5.0f)});
                    over(ring, Px{rim.r, rim.g, rim.b, band(d, 5.0f, 6.5f)});
                } else {
                    const Px e = kind == Skin::ChipOn ? px(P.dark) : goldDark;
                    over(ring, Px{e.r, e.g, e.b, 0.5f * band(d, 0, 1.5f)});
                }
                over(p, ring);
            }
            pxs[std::size_t(y) * W + x] = p;
        }
    SDL_Texture* t = makeTexture(sdl_, W, H, pxs);
    skins_[key] = t;
    return t;
}

void Hud::box(Skin kind, const Rect& r, float radius, std::uint32_t colour) {
    SDL_Texture* t = skin(kind, int(std::round(r.w)), int(std::round(r.h)), radius, colour);
    if (!t) return;
    float tw, th;
    SDL_GetTextureSize(t, &tw, &th);
    ui_->image({std::round(r.x) - kMargin, std::round(r.y) - kMargin, tw, th}, t);
    ui_->claim(r);
}

void Hud::ring(float cx, float cy, float r) {
    const float d = std::round(r * 2);
    SDL_Texture* t = skin(Skin::Ring, int(d), int(d), r);
    if (!t) return;
    float tw, th;
    SDL_GetTextureSize(t, &tw, &th);
    ui_->image({std::round(cx - r) - kMargin, std::round(cy - r) - kMargin, tw, th}, t);
}

void Hud::arc(float cx, float cy, float r, float width, float startDeg, float sweepDeg, const Colour& c) {
    if (std::fabs(sweepDeg) < 0.2f) return;
    const int n = std::max(4, int(std::fabs(sweepDeg) / 4));
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    const SDL_FColor col{c.r, c.g, c.b, c.a};
    const float r0 = r - width * 0.5f, r1 = r + width * 0.5f;
    for (int k = 0; k <= n; ++k) {
        const float a = (startDeg + sweepDeg * float(k) / float(n)) * kPiF / 180.f;
        const float ca = std::cos(a), sa = std::sin(a);
        v.push_back({{cx + r0 * ca, cy + r0 * sa}, col, {0, 0}});
        v.push_back({{cx + r1 * ca, cy + r1 * sa}, col, {0, 0}});
        if (k) {
            const int b = 2 * k;
            idx.insert(idx.end(), {b - 2, b - 1, b, b - 1, b + 1, b});
        }
    }
    SDL_RenderGeometry(sdl_, nullptr, v.data(), int(v.size()), idx.data(), int(idx.size()));
    // round caps
    for (float e : {startDeg, startDeg + sweepDeg}) {
        const float a = e * kPiF / 180.f;
        disc(cx + r * std::cos(a), cy + r * std::sin(a), width * 0.5f, c);
    }
}

void Hud::disc(float cx, float cy, float r, const Colour& c) {
    const int n = std::clamp(int(r * 1.5f), 10, 48);
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    const SDL_FColor col{c.r, c.g, c.b, c.a};
    v.push_back({{cx, cy}, col, {0, 0}});
    for (int k = 0; k <= n; ++k) {
        const float a = 2 * kPiF * float(k) / float(n);
        v.push_back({{cx + r * std::cos(a), cy + r * std::sin(a)}, col, {0, 0}});
        if (k) idx.insert(idx.end(), {0, k, k + 1});
    }
    SDL_RenderGeometry(sdl_, nullptr, v.data(), int(v.size()), idx.data(), int(idx.size()));
}

void Hud::icon(const std::string& name, const Rect& r, const Colour& c) {
    const auto it = iconIndex_.find(name);
    if (it == iconIndex_.end() || !icons_) return;
    const SDL_FRect src{float(it->second * iconCell_), 0, float(iconCell_), float(iconCell_)};
    const SDL_FRect dst{std::round(r.x), std::round(r.y), r.w, r.h};
    SDL_SetTextureColorModFloat(icons_, c.r, c.g, c.b);
    SDL_SetTextureAlphaModFloat(icons_, c.a);
    SDL_RenderTexture(sdl_, icons_, &src, &dst);
}

// AL-66: in the Edge style a heading (the title face, 20 px and up) is set in
// light spaced capitals; its width is measured the same way, so what is
// centred or right-aligned by textW stays so.
namespace {
float headingSpacing(float px) { return px >= 28 ? hudPalette().theme.size("title_spacing", 0.14f) : 0.1f; }
bool spacedHeading(bool titleFace, float px) {
    return titleFace && px >= 20 && hudPalette().theme.flag("caps") && !hudPalette().theme.flag("textured");
}
} // namespace

float Hud::textW(const std::string& s, float px, bool titleFace) const {
    if (spacedHeading(titleFace, px)) return capsW(s, px, headingSpacing(px), true);
    return ui_->textWidth(s, px / 15.f, titleFace);
}

void Hud::title(float x, float y, const std::string& s, const Colour& c, float px) {
    if (spacedHeading(true, px)) return caps(x, y, s, c, px, headingSpacing(px), true);
    ui_->text(x, y, s, c, px / 15.f, true);
}

void Hud::label(float x, float y, const std::string& s, const Colour& c, float px, bool shadow) {
    if (shadow) ui_->text(x, y + 2, s, kRim.withAlpha(0.85f), px / 15.f);
    ui_->text(x, y, s, c, px / 15.f);
}

bool Hud::clicked(const Rect& r) {
    ui_->claim(r);
    return in_->pressed && ui_->hovered(r);
}

// A round icon button of style G; in the Edge style (AL-63) a diamond in the same box.
bool Hud::roundButton(const Rect& r, const std::string& iconName, bool on, const std::string& tip) {
    if (!hudPalette().theme.flag("textured"))
        return diamondButton(r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.w * 0.74f, iconName, on, tip);
    const bool hot = ui_->hovered(r);
    box(on ? Skin::ButtonOn : Skin::Button, r, r.h * 0.5f);
    if (hot && !on) disc(r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.w * 0.5f - 1, kGold.withAlpha(0.16f));
    const float s = std::round(r.w * 0.5f);
    icon(iconName, {r.x + (r.w - s) * 0.5f, r.y + (r.h - s) * 0.5f, s, s},
         on ? kDark : hot ? kHot : kText);
    if (hot && !tip.empty()) tipAt(r.x + r.w * 0.5f, r.bottom() + 10, tip);
    return clicked(r);
}

// A chip: `dark` is the chosen one. Edge: a 1 px gold contour, the chosen one glazed gold.
bool Hud::textButton(const Rect& r, const std::string& s, bool dark) {
    const bool hot = ui_->hovered(r);
    const bool edge = !hudPalette().theme.flag("textured");
    if (edge) {
        if (dark) gradRect(r, kGold.withAlpha(0.28f), kGold.withAlpha(0.12f), true);
        else ui_->rect(r, kText.withAlpha(0.03f));
        ui_->border(r, dark ? kGold : kGold.withAlpha(0.45f), 1.0f, 0);
        if (hot) ui_->border(r.inset(1), kGoldPale.withAlpha(0.5f), 1.0f, 0);
    } else {
        box(dark ? Skin::ChipOn : Skin::Chip, r, r.h * 0.5f);
        if (hot) ui_->roundRect(r, dark ? kText.withAlpha(0.08f) : kLine.withAlpha(0.12f), r.h * 0.5f);
    }
    const float w = textW(s, 15);
    ui_->text(r.x + (r.w - w) * 0.5f, r.y + (r.h - ui_->textHeight(1.0f)) * 0.5f, s,
              edge ? (dark ? kHot : hot ? kGoldPale : kText) : dark ? kText : kInk, 1.0f);
    return clicked(r);
}

// The wax seal of style G; Edge: a small double gold diamond, the panel's ornament.
void Hud::seal(float cx, float cy) {
    if (hudPalette().theme.flag("textured")) {
        box(Skin::Seal, {cx - 17, cy - 17, 34, 34}, 17);
        return;
    }
    diamond(cx, cy, 18, false, false);
    const Colour f = kGold.withAlpha(0.8f);
    diamond(cx, cy, 6, false, false, &f);
}

void Hud::dimmer(float W, float H, float a) {
    ui_->rect({0, 0, W, H}, kRim.withAlpha(a));
    ui_->claim({0, 0, W, H});
}

// ---------------------------------------------------------------- people
SDL_Texture* Hud::portraitOf(const ActorSnap& a) {
    const auto it = portraits_.find(a.id);
    if (it != portraits_.end()) return it->second;
    const std::string look = host_->lookOf(a);
    PortraitBase& base = portraitBases_[look];
    if (base.picture.w == 0 && !look.empty()) {
        const fs::path dir = root_ / "assets/altera_game/people";
        // AL-68: the Skyrim stand-ins' faces (tools/game_assets/skyrim/portraits.py) when baked
        const std::string stem = fs::exists(dir / ("portrait_" + look + "_sky.png")) ? look + "_sky" : look;
        loadImage(dir / ("portrait_" + stem + ".png"), base.picture.w, base.picture.h, base.picture.rgba);
        loadImage(dir / ("portrait_" + stem + "_id.png"), base.mask.w, base.mask.h, base.mask.rgba);
    }
    const int S = 128;
    const bool diamondCut = !hudPalette().theme.flag("textured");
    std::vector<Px> px(std::size_t(S) * S);
    float skinT[3], hairT[3], clothT[3], fam[3];
    host_->tintsOf(a, skinT, hairT, clothT);
    host_->familyColour(a.family, fam);
    for (int y = 0; y < S; ++y)
        for (int x = 0; x < S; ++x) {
            Px p{fam[0] * 0.75f, fam[1] * 0.75f, fam[2] * 0.75f, 1};
            if (base.picture.w == S && base.mask.w == S) {
                const std::uint8_t* c = base.picture.rgba.data() + (std::size_t(y) * S + x) * 4;
                const std::uint8_t* m = base.mask.rgba.data() + (std::size_t(y) * S + x) * 4;
                const float ms = m[0] / 255.f, mh = m[1] / 255.f, mc = m[2] / 255.f;
                const float rest = std::max(0.f, 1 - ms - mh - mc);
                Px q{c[0] / 255.f, c[1] / 255.f, c[2] / 255.f, c[3] / 255.f};
                q.r *= ms * skinT[0] + mh * hairT[0] + mc * clothT[0] + rest;
                q.g *= ms * skinT[1] + mh * hairT[1] + mc * clothT[1] + rest;
                q.b *= ms * skinT[2] + mh * hairT[2] + mc * clothT[2] + rest;
                over(p, q);
            }
            if (!a.alive) {
                const float l = p.r * 0.3f + p.g * 0.59f + p.b * 0.11f;
                p.r = p.g = p.b = l * 0.7f;
            }
            // a round portrait, or (Edge) one cut to a diamond
            const float d = diamondCut ? (std::fabs(x + 0.5f - S * 0.5f) + std::fabs(y + 0.5f - S * 0.5f) - S * 0.5f) * 0.7071f
                                       : std::hypot(x + 0.5f - S * 0.5f, y + 0.5f - S * 0.5f) - S * 0.5f;
            p.a *= cover(d);
            px[std::size_t(y) * S + x] = p;
        }
    SDL_Texture* t = makeTexture(sdl_, S, S, px);
    portraits_[a.id] = t;
    return t;
}

void Hud::portrait(const ActorSnap& a, float cx, float cy, float r) {
    SDL_Texture* t = portraitOf(a);
    if (t) ui_->image({cx - r, cy - r, r * 2, r * 2}, t);
}

const ActorSnap* Hud::actor(const Snapshot& s, EntityId id) const {
    for (const auto& a : s.actors)
        if (a.id == id) return &a;
    return nullptr;
}

std::string Hud::dayPart(Tick t) const {
    const double h = double(t % sim::kTicksPerDay) / double(sim::kTicksPerDay) * 24.0;
    const char* k = h < 4.5 ? "night" : h < 6.5 ? "dawn" : h < 11 ? "morning" : h < 17 ? "day" : h < 20 ? "evening"
                  : h < 22 ? "dusk" : "night";
    return host_->text(std::string("hud.daypart.") + k);
}

// The five tiers of needs, then wants, people and values.
static const char* kTierKeys[] = {"body", "safety", "belonging", "esteem", "calling"};
static const char* kSlotIcons[] = {"body", "safety", "belonging", "esteem", "calling", "wants", "people", "values"};

std::vector<Hud::Row> Hud::tierRows(const Snapshot& s, const ActorSnap& a, int tier) const {
    std::vector<Row> rows;
    const Detail& d = s.detail;
    auto need = [&](const char* id) {
        for (const auto& n : d.needs)
            if (n.id == id) rows.push_back({n.name, n.value, "", n.value < 0.35, n.id});
    };
    switch (tier) {
    case 0: need("hunger"); need("thirst"); need("energy"); break;
    case 1:
        rows.push_back({host_->text("hud.vital.health"), a.hp, "", a.hp < 0.35, "health"});
        need("warmth");
        need("hygiene");
        break;
    case 2: {
        need("social");
        double kin = 0;
        int n = 0;
        for (const auto& r : d.relations)
            if (!r.kin.empty()) kin += r.affinity, ++n;
        if (n) rows.push_back({host_->text("hud.people"), std::clamp(0.5 + kin / n * 0.5, 0.0, 1.0), "", false, "kin"});
        break;
    }
    case 3: {
        double rep = 0, resp = 0;
        for (const auto& [k, v] : d.reputation) rep += v;
        if (!d.reputation.empty()) rows.push_back({host_->text("hud.values"), std::clamp(0.5 + rep / d.reputation.size() * 0.5, 0.0, 1.0), "", false, "repute"});
        int n = 0;
        for (const auto& r : d.relations) resp += r.respect, ++n;
        if (n) rows.push_back({host_->text("hud.people"), std::clamp(0.5 + resp / n * 0.5, 0.0, 1.0), "", false, "respect"});
        break;
    }
    case 4: need("purpose"); need("fun"); break;
    case 5:
        for (std::size_t i = 0; i < d.wants.size() && i < 3; ++i) rows.push_back({d.wants[i], -1, "", false});
        if (d.wants.empty()) rows.push_back({host_->text("hud.sheet.no_wants"), -1, "", false});
        break;
    case 6:
        for (std::size_t i = 0; i < d.relations.size() && i < 3; ++i) {
            const RelSnap& r = d.relations[i];
            const std::string what = r.enmity > 0.3 ? "hates" : r.fear > 0.3 ? "fears" : r.affinity > 0.25 ? "likes"
                                   : r.affinity < -0.25 ? "dislikes" : r.trust > 0.3 ? "trust" : "knows";
            rows.push_back({r.name, -1, host_->text("hud.rel." + (r.kin.empty() ? what : r.kin)), r.affinity < -0.3});
        }
        if (d.relations.empty()) rows.push_back({host_->text("hud.rel.none"), -1, "", false});
        break;
    case 7:
        for (std::size_t i = 0; i < d.traits.size() && i < 3; ++i) rows.push_back({d.traits[i], -1, "", false});
        for (std::size_t i = 0; i < d.reputation.size() && rows.size() < 3; ++i)
            rows.push_back({d.reputation[i].first, std::clamp(0.5 + d.reputation[i].second * 0.5, 0.0, 1.0), "", false});
        break;
    }
    if (tier < 5)
        for (auto& r : rows)
            if (r.value >= 0 && !r.id.empty()) statusRow(s, a, r);
    return rows;
}

// A small trend arrow drawn with lines (the fonts have no arrow glyphs): -1 down, 0 level, 1 up.
void Hud::trendArrow(float cx, float cy, int dir, const Colour& c) {
    const float L = 6, H = 3.5f;
    auto ln = [&](float x0, float y0, float x1, float y1) { // two pixels thick
        ui_->line(x0, y0, x1, y1, c);
        ui_->line(x0 + 0.8f, y0 + 0.8f, x1 + 0.8f, y1 + 0.8f, c);
    };
    if (dir == 0) {
        ln(cx - L, cy, cx + L, cy);
        ln(cx + L, cy, cx + L - H, cy - H);
        ln(cx + L, cy, cx + L - H, cy + H);
        return;
    }
    const float s = dir < 0 ? 1.0f : -1.0f; // tip below for falling
    ln(cx, cy - L * s, cx, cy + L * s);
    ln(cx, cy + L * s, cx - H, cy + (L - H) * s);
    ln(cx, cy + L * s, cx + H, cy + (L - H) * s);
}

// One status of the hover panel: its trend from the last game hours, and, when it
// is low, why (from what the snapshot shows) and what helps (content/loc hud.status.*).
void Hud::statusRow(const Snapshot& s, const ActorSnap& a, Row& r) const {
    TrendSample& t = trends_[std::to_string(a.id) + ":" + r.id];
    const Tick hour = std::max<Tick>(1, sim::kTicksPerDay / 24);
    if (t.tick == 0 && !t.known && t.value == 0) t = {s.tick, r.value, 0, false};
    else if (s.tick < t.tick) t = {s.tick, r.value, 0, false};   // a reload
    else if (s.tick - t.tick >= hour) {
        const double rate = (r.value - t.value) / double(s.tick - t.tick) * double(sim::kTicksPerDay);
        t.rate = t.known ? t.rate * 0.6 + rate * 0.4 : rate;
        t.known = true, t.tick = s.tick, t.value = r.value;
    }
    if (t.known) r.arrow = t.rate < -0.04 ? -1 : t.rate > 0.04 ? 1 : 0;
    else r.arrow = r.value < 0.35 ? -1 : r.value < 0.55 ? 0 : 1;
    if (r.value >= 0.55) {
        r.desc = host_->text("hud.status.ok");
        return;
    }
    const Detail& d = s.detail;
    const std::string k = "hud.status." + r.id;
    std::string why = host_->text(k + ".why"), help = host_->text(k + ".help");
    const double h = double(s.tick % sim::kTicksPerDay) / double(sim::kTicksPerDay) * 24.0;
    if (r.id == "hunger") {
        static const char* kFood[] = {"bread", "loaf", "meal", "porridge", "berries", "roots", "mushrooms", "meat",
                                      "fish", "cheese", "honey", "eggs", "egg", "nuts", "apples", "greens"};
        const ItemSnap* food = nullptr;
        for (const auto& it : d.inventory)
            for (const char* f : kFood)
                if (it.id == f && it.count > 0 && !food) food = &it;
        if (food) help = host_->text(k + ".help_item", {{"item", food->name}});
        else why = host_->text(k + ".why_empty");
    } else if (r.id == "energy" && (h < 5 || h >= 22)) {
        why = host_->text(k + ".why_night");
    } else if (r.id == "health" && a.wounds > 0) {
        why = host_->text(k + ".why_wounds", {{"n", std::to_string(a.wounds)}});
    } else if (r.id == "warmth" && s.tempC < 8) {
        why = host_->text(k + ".why_cold", {{"t", std::to_string(int(std::lround(s.tempC)))}});
    }
    r.desc = host_->text("hud.status.help", {{"why", why}, {"help", help}});
}

double Hud::tierValue(const Snapshot& s, const ActorSnap& a, int tier) const {
    const auto rows = tierRows(s, a, tier);
    double lo = 1;
    int n = 0;
    for (const auto& r : rows)
        if (r.value >= 0) lo = std::min(lo, r.value), ++n;
    return n ? lo : 0.6;
}

// ---------------------------------------------------------------- the frame
void Hud::draw(ui::Ui& ui, ui::Input& input, const Snapshot& s, EntityId selected, HudHost& host, float W, float H) {
    ui_ = &ui;
    in_ = &input;
    host_ = &host;
    sdl_ = ui.renderer();
    selected_ = selected;
    clock_ = std::chrono::duration<double>(SnapClock::now() - started_).count();
    tipText_.clear();
    const ActorSnap* sel = actor(s, selected);
    // Portraits follow life and death: drop the dead's coloured one.
    for (const auto& a : s.actors)
        if (!a.alive && portraits_.count(a.id)) {
            static std::map<EntityId, bool> greyed;
            if (!greyed[a.id]) {
                SDL_DestroyTexture(portraits_[a.id]);
                portraits_.erase(a.id);
                greyed[a.id] = true;
            }
        }

    const bool edge = !hudPalette().theme.flag("textured");   // AL-64: the Edge HUD's layout
    if (panel == "none") names(s);
    if (panel == "none" && edge) edgeHud(s, sel, W, H);
    if (panel == "none" && !edge) {
        topMenu(W);
        minimap(s, W);
        toasts(s, W);
        journalBox(s, W, H);
        party(s, sel);
        if (sel && !sel->animal) selectedCorner(s, *sel, H);
        else if (sel) {
            // a beast: its name and state on a small leather plate
            const Rect r{36, H - 96, 300, 60};
            box(Skin::Leather, r, 18);
            title(r.x + 16, r.y + 8, sel->name + (sel->group > 1 ? "  x" + std::to_string(sel->group) : ""), kGoldPale, 20);
            label(r.x + 16, r.y + 34, host.text(sel->alive ? "game.fsm." + sel->state : "game.dead"), kText, 14);
        }
        quickBar(s, W, H);
        clockPill(s, W, H);
        controlPill(W, H);
    }
    if (panel == "none") {
        walkMarker();
        workFx(s);
        speechBubbles(s);
        feedbackFloats(s);
        hoverHint(s, W);
        socialAsk(s, W, H);   // AL-65
        if (showThoughts) thoughts(s, W);
        // A button of the top menu opened a panel: its click is spent. (The
        // build panel draws the top menu again, which took the same click as
        // a second press and shut the panel the frame it opened.)
        if (panel != "none") in_->pressed = false;
    }
    if (panel == "sheet" && sel && !sel->animal) sheet(s, *sel, W, H);
    else if (panel == "sheet") panel = "none";
    if (panel == "wheel") wheel(s, W, H);
    if (panel == "talk") talkPanel(s, W, H);   // AL-67
    if (panel == "name") namePanel(s, W, H);
    if (panel == "beast") {
        names(s);
        beast(s, W, H);
    }
    if (panel == "thing") {
        names(s);
        workFx(s);
        speechBubbles(s);
        feedbackFloats(s);
        thing(s, W, H);
    }
    if (panel == "kin") kin(s, sel, W, H);
    if (panel == "inventory") inventory(s, sel, W, H);
    if (panel == "annals") annals(s, W, H);
    if (panel == "build") {
        if (edge) {
            const float band = std::min(W, H * 16.f / 9.f), ox = (W - band) * 0.5f;
            edgeMenu(ox, band);
            edgeClock(s, ox, band, H);
        } else {
            topMenu(W);
            toasts(s, W);
            clockPill(s, W, H);
        }
        buildBar(s, W, H);
    }
    if (panel == "colony") colony(s, W, H);
    if (panel == "workshop") workshop(s, W, H);
    if (panel == "backstory") backstory(W, H);
    if (panel == "menu") mainMenu(W, H);
    if (panel == "pause") pauseMenu(W, H);
    if (panel == "slots") slotsPanel(W, H);
    if (panel == "settings") settingsPanel(W, H);
    if (panel == "world") worldPanel(W, H);   // AL-69
    if (!inMenu() && !s.result.empty() && s.result != "ok") {
        const std::string t = ui.fit(s.result, 600, 14 / 15.f);
        const float w = textW(t, 14) + 28;
        const Rect r{(W - w) * 0.5f, H - 150, w, 30};
        box(Skin::Paper, r, 15);
        label(r.x + 14, r.y + 6, t, kInkRed, 14);
    }
    drawTip(W, H);   // AL-63: every panel's tooltip, over everything
}

// Names over the people near the camera, a bubble over anyone in want.
void Hud::names(const Snapshot& s) {
    // Names do not lie over each other: the important first (the selected,
    // the target, the player's own, then the nearer - lower on the screen),
    // each later one lifted over those already placed, or left out when
    // there is no room for it within a few steps.
    struct Tag {
        const ActorSnap* a;
        float sx, sy;
        int rank;
    };
    std::vector<Tag> tags;
    for (const auto& a : s.actors) {
        if (a.animal && a.id != selected_ && a.id != aim) continue;
        if (!a.alive && a.id != selected_ && a.id != aim) continue;
        float sx, sy;
        if (!host_->overHead(a, sx, sy)) continue;
        tags.push_back({&a, sx, sy, a.id == selected_ ? 0 : a.id == aim ? 1 : a.player ? 2 : 3});
    }
    std::stable_sort(tags.begin(), tags.end(), [](const Tag& x, const Tag& y) {
        if (x.rank != y.rank) return x.rank < y.rank;
        return x.sy > y.sy;
    });
    std::vector<Rect> taken;
    auto clash = [&](const Rect& r) {
        for (const auto& t : taken)
            if (r.x < t.right() && t.x < r.right() && r.y < t.bottom() && t.y < r.bottom()) return true;
        return false;
    };
    for (const auto& tg : tags) {
        const ActorSnap& a = *tg.a;
        const float sx = tg.sx;
        float sy = tg.sy;
        // the clicked target: its name between two marks
        const std::string nm = a.id == aim && a.id != selected_ ? "> " + a.name + " <" : a.name;
        const float w = textW(nm, 15, true);
        const bool need = !a.animal && a.alive && a.lowNeed < 0.3 && !a.lowNeedId.empty();
        const float top = need ? 52.0f : 10.0f;   // the bubble stands over the name
        Rect r{sx - w * 0.5f - 4, sy - top, w + 8, top + 14};
        int lift = 0;
        while (clash(r) && lift < 4) {
            r.y -= r.h + 2;
            sy -= r.h + 2;
            ++lift;
        }
        if (clash(r) && tg.rank >= 3) continue;
        taken.push_back(r);
        const Colour c = a.id == selected_ || a.id == aim ? kGoldPale : a.player ? kGold : kText;
        ui_->text(sx - w * 0.5f, sy - 8 + 2, nm, kRim.withAlpha(0.9f), 1.0f, true);
        ui_->text(sx - w * 0.5f, sy - 8, nm, c, 1.0f, true);
        if (need) {
            const char* ic = a.lowNeedId == "hunger" || a.lowNeedId == "thirst" || a.lowNeedId == "energy" ? "body"
                           : a.lowNeedId == "warmth" || a.lowNeedId == "hygiene" ? "safety"
                           : a.lowNeedId == "social" ? "belonging" : "calling";
            const float bob = std::sin(float(clock_) * 2.5f + float(a.id)) * 2.0f;
            const Rect b{sx - 16, sy - 50 + bob, 32, 32};
            if (!hudPalette().theme.flag("textured")) {   // AL-66: a diamond with the need's level under it
                diamond(sx, b.y + 16, 24, false, false);
                ui_->rect({sx - 12, b.bottom() + 2, 24, 2}, kText.withAlpha(0.25f));
                ui_->rect({sx - 12, b.bottom() + 2, 24 * float(std::max(0.05, a.lowNeed)), 2}, kRingBad);
            } else {
                box(Skin::Leather, b, 16);
                arc(b.x + 16, b.y + 16, 13, 2.5f, -90, float(360 * std::max(0.05, a.lowNeed)), kRingBad);
            }
            icon(ic, {b.x + 7, b.y + 7, 18, 18}, kRingBad);
        }
    }
}

void Hud::topMenu(float W) {
    static const char* kMenu[][2] = {{"sheet", "hud.menu.sheet"}, {"dynasty", "hud.menu.dynasty"}, {"colony", "hud.menu.colony"},
                                     {"build", "hud.menu.build"}, {"annals", "hud.menu.annals"}, {"i_tool", "hud.menu.workshop"},
                                     {"map", "hud.menu.map"}};
    const int n = 7;
    const float w = n * 44 + (n - 1) * 4 + 8;
    const Rect r{(W - w) * 0.5f, 24, w, 52};
    box(Skin::Leather, r, 26);
    for (int i = 0; i < n; ++i) {
        const Rect b{r.x + 4 + i * 48.f, r.y + 4, 44, 44};
        const bool on = (i == 0 && panel == "sheet") || (i == 1 && panel == "kin") || (i == 2 && panel == "colony") ||
                        (i == 3 && panel == "build") || (i == 4 && panel == "annals") || (i == 5 && panel == "workshop");
        if (roundButton(b, kMenu[i][0], on, host_->text(kMenu[i][1]) + (i == 6 ? " · " + host_->text("hud.soon") : ""))) {
            if (i == 2) panel = panel == "colony" ? "none" : "colony";
            if (i == 3) panel = panel == "build" ? "none" : "build";
            if (i == 0) panel = panel == "sheet" ? "none" : "sheet";
            if (i == 1) panel = panel == "kin" ? "none" : "kin";
            if (i == 4) panel = panel == "annals" ? "none" : "annals";
            if (i == 5) panel = panel == "workshop" ? "none" : "workshop";
        }
    }
}

void Hud::minimap(const Snapshot& s, float W) {
    const float D = 140, x = W - 36 - D, y = 28;
    if (!minimap_ && s.mapSize > 0) {
        const int S = 140;
        std::vector<double> h(std::size_t(S) * S);
        double lo = 1e9, hi = -1e9;
        for (int j = 0; j < S; ++j)
            for (int i = 0; i < S; ++i) {
                const double v = host_->groundHeight((i + 0.5) / S * s.mapSize, (1.0 - (j + 0.5) / S) * s.mapSize);
                h[std::size_t(j) * S + i] = v;
                lo = std::min(lo, v), hi = std::max(hi, v);
            }
        std::vector<Px> px(std::size_t(S) * S);
        for (int j = 0; j < S; ++j)
            for (int i = 0; i < S; ++i) {
                const double v = h[std::size_t(j) * S + i];
                const double dx = h[std::size_t(j) * S + std::min(i + 1, S - 1)] - h[std::size_t(j) * S + std::max(i - 1, 0)];
                const double dy = h[std::size_t(std::min(j + 1, S - 1)) * S + i] - h[std::size_t(std::max(j - 1, 0)) * S + i];
                const float t = float((v - lo) / std::max(1.0, hi - lo));
                const float shade = std::clamp(1.0f - float(dx - dy) * 0.08f, 0.6f, 1.3f);
                Px p{(0.30f + 0.35f * t) * shade, (0.36f + 0.22f * t) * shade, (0.22f + 0.18f * t) * shade, 1};
                const float d = std::hypot(i + 0.5f - S * 0.5f, j + 0.5f - S * 0.5f) - S * 0.5f;
                p.a = cover(d);
                px[std::size_t(j) * S + i] = p;
            }
        minimap_ = makeTexture(sdl_, S, S, px);
    }
    if (minimap_) ui_->image({x, y, D, D}, minimap_);
    // people as dots, the selected one bright
    if (s.mapSize > 0)
        for (const auto& a : s.actors) {
            if (a.animal || !a.alive) continue;
            const float px = x + float(a.x / s.mapSize) * D, py = y + float(1.0 - a.y / s.mapSize) * D;
            if (std::hypot(px - x - D / 2, py - y - D / 2) > D / 2 - 3) continue;
            float c[3];
            host_->familyColour(a.family, c);
            disc(px, py, a.id == selected_ ? 4.0f : 2.5f, a.id == selected_ ? kGoldPale : Colour(c[0], c[1], c[2]));
        }
    ring(x + D / 2, y + D / 2, D / 2);
    ui_->claim({x, y, D, D});
    const std::string place = s.settlement;
    const float w = textW(place, 15, true) + 32;
    const Rect pill{x + D / 2 - w / 2, y + D - 12, w, 30};
    box(Skin::Leather, pill, 15);
    ui_->border(pill.inset(4), kGoldPale.withAlpha(0.45f), 1.0f, 11);
    title(pill.x + 16, pill.y + 5, place, kText, 15);
}

void Hud::toasts(const Snapshot& s, float W) {
    if (seenChronicle_ == 0) seenChronicle_ = s.chronicle.size();
    if (s.chronicle.size() > seenChronicle_) {
        for (std::size_t i = seenChronicle_; i < s.chronicle.size(); ++i) toasts_.push_back({s.chronicle[i].text, clock_});
        seenChronicle_ = s.chronicle.size();
    }
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [&](const Toast& t) { return clock_ - t.born > 9.0; }),
                  toasts_.end());
    while (toasts_.size() > 3) toasts_.erase(toasts_.begin());
    float y = 218;
    for (const auto& t : toasts_) {
        const float w = 340;
        const std::string line = ui_->fit(t.text, w - 28, 14 / 15.f);
        const Rect r{W - 36 - w, y, w, 34};
        box(Skin::Paper, r, 14);
        label(r.x + 14, r.y + 7, line, kInk, 14);
        y += 46;
    }
}

void Hud::party(const Snapshot& s, const ActorSnap* sel) {
    if (!sel || sel->animal) return;
    float y = 28;
    for (const auto& a : s.actors) {
        if (a.animal || a.family != sel->family || a.id == sel->id) continue;
        const Rect r{36, y, 52, 52};
        disc(r.x + 26, r.y + 26, 24, kNavy);
        arc(r.x + 26, r.y + 26, 24, 3, 0, 360, kInkSoft);
        portrait(a, r.x + 26, r.y + 26, 20);
        if (a.alive) arc(r.x + 26, r.y + 26, 24, 3, -90, float(360 * std::clamp(a.hp, 0.02, 1.0)), kBad);
        if (ui_->hovered(r)) {
            const std::string tip = a.name + " · " + host_->text(a.player ? "hud.kin.you" : (a.alive ? (a.sex == 'f' ? "hud.kin.self_f" : "hud.kin.self") : (a.sex == 'f' ? "hud.kin.dead_f" : "hud.kin.dead")));
            const float w = textW(tip, 14) + 20;
            box(Skin::Leather, {r.right() + 12, r.y + 12, w, 28}, 14);
            label(r.right() + 22, r.y + 17, tip, kText, 14);
        }
        if (clicked(r)) host_->select(a.id);
        y += 66;
        if (y > 520) break;
    }
}

void Hud::selectedCorner(const Snapshot& s, const ActorSnap& a, float H) {
    const float bx = 36, by = H - 30 - 140;
    // vitals ring and the big portrait
    box(Skin::Leather, {bx + 4, by + 4, 132, 132}, 66);
    const double spirit = std::clamp(0.5 + s.detail.mood * 0.5, 0.0, 1.0);
    const double strength = std::clamp(1.0 - s.detail.fatigue, 0.0, 1.0);
    const struct {
        double v;
        Colour c;
        float rot;
    } vit[] = {{a.hp, kBad, 95}, {strength, kRingGood, 215}, {spirit, kRingGood, 335}};
    for (const auto& v : vit) {
        arc(bx + 70, by + 70, 62, 6, v.rot, 109, kText.withAlpha(0.2f));
        arc(bx + 70, by + 70, 62, 6, v.rot, float(109 * std::clamp(v.v, 0.02, 1.0)), v.c);
    }
    portrait(a, bx + 70, by + 70, 52);
    arc(bx + 70, by + 70, 53, 2, 0, 360, kGoldPale);
    arc(bx + 70, by + 70, 55, 2.5f, 0, 360, kGoldDark);
    if (clicked({bx + 18, by + 18, 104, 104})) panel = panel == "sheet" ? "none" : "sheet";

    // name and mood
    const float cx = bx + 140 + 16;
    const int moodLevel = std::clamp(int((s.detail.mood + 1.0) * 2.5), 0, 4);
    const Colour mc = moodLevel >= 3 ? kRingGood : moodLevel == 2 ? kGold : kGold;
    const std::string full = a.name;
    ui_->text(cx + 8, H - 30 - 34 + 2, full, kRim.withAlpha(0.85f), 28 / 15.f, true);
    title(cx + 8, H - 30 - 34, full, kText, 28);
    label(cx + 8 + textW(full, 28, true) + 12, H - 30 - 26, host_->text("hud.mood." + std::to_string(moodLevel)), mc, 15, true);

    // the slots pill
    const Rect pill{cx, H - 30 - 34 - 10 - 52, 364, 52};
    box(Skin::Leather, pill, 26);
    int hovered = -1;
    for (int k = 0; k < 8; ++k) {
        const Rect b{pill.x + 4 + k * 45.0f, pill.y + 4, 44, 44};
        if (ui_->hovered(b)) hovered = k;
    }
    if (hovered >= 0) hover_ = hovered;
    else if (!ui_->hovered(pill)) hover_ = -1;
    const int shown = hover_ >= 0 ? hover_ : pin_;
    for (int k = 0; k < 8; ++k) {
        const Rect b{pill.x + 4 + k * 45.0f, pill.y + 4, 44, 44};
        const bool on = k == shown;
        const double v = k < 5 ? tierValue(s, a, k) : k == 6 ? 0.5 : 0.6;
        box(on ? Skin::ButtonOn : Skin::Button, b, 22);
        arc(b.x + 22, b.y + 22, 20, 3, -90, float(360 * std::clamp(v, 0.03, 1.0)), on ? kDark : ringColour(v));
        icon(kSlotIcons[k], {b.x + 12, b.y + 12, 20, 20}, on ? kDark : kText);
        if (clicked(b)) pin_ = pin_ == k ? -1 : k;
    }
    // the paper over the slots: the group opened into its statuses (value, trend, why, what helps)
    if (shown >= 0) slotCard(s, a, shown, pill.x, pill.y - 12, pill.x + 4 + shown * 45.0f + 22, 364);
}

// A status group opened into its statuses (value, trend, why, what helps): a
// card whose bottom is at `bottom`, `width` wide, with a caret at `caret`.
void Hud::slotCard(const Snapshot& s, const ActorSnap& a, int shown, float x, float bottom, float caret, float width) {
    static const char* kSlotNames[] = {"hud.tier.body", "hud.tier.safety", "hud.tier.belonging", "hud.tier.esteem",
                                       "hud.tier.calling", "hud.wants", "hud.people", "hud.values"};
    auto rows = tierRows(s, a, shown);
    const float tw = width - 40;
    std::vector<std::vector<std::string>> descLines(rows.size());
    double sum = 0, lo = 1;
    int bars = 0;
    float body = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        if (r.value >= 0) sum += r.value, lo = std::min(lo, r.value), ++bars;
        if (!r.desc.empty()) {
            // word wrap into at most two lines
            std::string line;
            std::size_t pos = 0;
            const std::string& t = r.desc;
            while (pos < t.size() && descLines[i].size() < 2) {
                std::size_t sp = t.find(' ', pos);
                const std::string word = t.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
                const std::string next = line.empty() ? word : line + " " + word;
                if (!line.empty() && textW(next, 13) > tw) {
                    descLines[i].push_back(line);
                    line = word;
                } else line = next;
                pos = sp == std::string::npos ? t.size() : sp + 1;
            }
            if (!line.empty() && descLines[i].size() < 2) descLines[i].push_back(line);
            else if (!line.empty()) descLines[i].back() = ui_->fit(descLines[i].back() + " " + line, tw, 13 / 15.f);
        }
        body += 28 + float(descLines[i].size()) * 17.0f + (descLines[i].empty() ? 0 : 4);
    }
    const bool groups = shown < 5;
    const float h = 16 + 30 + body + (groups ? 22 : 0) + 14;
    const Rect p{x, bottom - h, width, h};
    box(Skin::Paper, p, 18);
    seal(p.right() - 18 - 17 + 17, p.y + 5);
    std::string head = host_->text(kSlotNames[shown]);
    if (groups && bars) head += ": " + std::to_string(int(std::lround(sum / bars * 100)));
    title(p.x + 20, p.y + 14, head, kInk, 22);
    if (groups && bars) {
        const char* tk = lo < 0.35 ? "hud.status.trend.down" : lo < 0.55 ? "hud.status.trend.hold" : "hud.status.trend.up";
        const Colour tc = lo < 0.35 ? kInkRed : lo < 0.55 ? kGoldDark : kGood;
        const std::string tt = host_->text(tk);
        const float tx = p.right() - 20 - 34 - textW(tt, 14, true);
        title(tx, p.y + 20, tt, tc, 14);
        trendArrow(tx - 12, p.y + 29, lo < 0.35 ? -1 : lo < 0.55 ? 0 : 1, tc);
    }
    ui_->line(p.x + 20, p.y + 46, p.right() - 20, p.y + 46, kLine.withAlpha(0.45f));
    float y = p.y + 54;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        const Colour fg = r.alarm ? kInkRed : kInk;
        label(p.x + 20, y, ui_->fit(r.label, r.value >= 0 ? 150 : 200, 1.0f), fg, 15);
        if (r.value >= 0) {
            const Rect bar{p.right() - 20 - 54 - 10 - 110, y + 6, 110, 8};
            ui_->roundRect(bar, kInk.withAlpha(0.16f), 4);
            ui_->roundRect({bar.x, bar.y, std::max(8.f, float(bar.w * std::clamp(r.value, 0.0, 1.0))), bar.h}, barColour(r.value), 4);
            if (!r.id.empty()) {
                const std::string num = std::to_string(int(std::lround(std::clamp(r.value, 0.0, 1.0) * 100)));
                const Colour nc = r.value < 0.35 ? kInkRed : r.value < 0.55 ? kGoldDark : kGood;
                title(p.right() - 20 - 18 - textW(num, 13, true), y + 1, num, nc, 13);
                if (r.arrow >= -1 && r.arrow <= 1) trendArrow(p.right() - 20 - 7, y + 10, r.arrow, nc);
            }
        } else if (!r.note.empty()) {
            const float w = textW(r.note, 14);
            label(p.right() - 20 - w, y + 1, r.note, kInkSoft, 14);
        }
        y += 28;
        if (!descLines[i].empty()) {
            const Colour dc = r.value < 0.35 ? kInkRed : r.value < 0.55 ? kGoldDark : kInkSoft;
            y -= 6;
            for (const auto& l : descLines[i]) label(p.x + 20, y, l, dc, 13), y += 17;
            y += 10;
        }
    }
    if (groups) label(p.x + 20, y, ui_->fit(host_->text("hud.status.hint"), tw, 12 / 15.f), kGoldDark, 12);
    // the caret under the shown slot
    ui_->rect({caret - 7, p.bottom() - 2, 14, 6}, kText);
}

void Hud::quickBar(const Snapshot& s, float W, float H) {
    const float w = 6 * 48 + 5 * 6 + 12;
    const Rect r{(W - w) * 0.5f, H - 30 - 60, w, 60};
    box(Skin::Leather, r, 30);
    static const std::pair<const char*, const char*> kItemIcons[] = {
        {"knife", "i_knife"}, {"axe", "i_tool"}, {"spear", "i_knife"}, {"bread", "i_bread"}, {"meal", "i_food"},
        {"berries", "i_food"}, {"roots", "i_food"}, {"mushrooms", "i_food"}, {"meat", "i_food"}, {"firewood", "i_wood"},
        {"wood", "i_wood"}, {"torch", "i_torch"}, {"herbs", "i_herb"}, {"stone", "i_stone"}, {"cloth", "i_cloth"},
        {"thread", "i_cloth"}, {"remedy", "i_herb"}};
    for (int k = 0; k < 6; ++k) {
        const Rect b{r.x + 6 + k * 54.0f, r.y + 6, 48, 48};
        box(Skin::Button, b, 24);
        arc(b.x + 24, b.y + 24, 23, 1.5f, 0, 360, kGold.withAlpha(0.4f));
        if (k < int(s.detail.inventory.size())) {
            const ItemSnap& it = s.detail.inventory[std::size_t(k)];
            std::string ic = "i_tool";
            for (const auto& [id, name] : kItemIcons)
                if (it.id.find(id) != std::string::npos) ic = name;
            icon(ic, {b.x + 11, b.y + 11, 26, 26}, Colour(1, 1, 1));
            if (it.count > 1) {
                const std::string n = std::to_string(it.count);
                label(b.right() - 6 - textW(n, 12), b.bottom() - 18, n, kText, 12, true);
            }
            if (ui_->hovered(b)) {
                const float tw = textW(it.name, 14) + 20;
                box(Skin::Leather, {b.x + 24 - tw / 2, b.y - 40, tw, 28}, 14);
                label(b.x + 24 - tw / 2 + 10, b.y - 35, it.name, kText, 14);
            }
        }
        const std::string key = std::to_string(k + 1);
        label(b.right() - 10, b.bottom() - 16, "", kGold, 12);
        (void)key;
    }
}

void Hud::clockPill(const Snapshot& s, float W, float H) {
    const int day = int(s.tick / sim::kTicksPerDay) + 1;
    const std::string line = host_->text("hud.dayline", {{"day", day}, {"part", dayPart(s.tick)}});
    const float tw = textW(line, 17, true);
    const float w = 6 + 36 + 6 + tw + 16 + 4 * 46 + 4;
    const Rect r{W - 36 - w, H - 30 - 52, w, 52};
    box(Skin::Leather, r, 26);
    // the dial: night blue, the day's share gold, turning with the clock
    const float cx = r.x + 6 + 18, cy = r.y + 26;
    disc(cx, cy, 16, kDark);
    const double f = double(s.tick % sim::kTicksPerDay) / double(sim::kTicksPerDay) + host_->tickAlpha() / double(sim::kTicksPerDay);
    arc(cx, cy, 8, 16, float(f * 360.0 - 180.0), 90, kGold);
    arc(cx, cy, 16, 1.5f, 0, 360, kGold);
    disc(cx, cy, 3, kRim);
    title(r.x + 48, r.y + 14, line, kText, 17);
    float x = r.x + 48 + tw + 16;
    if (roundButton({x, r.y + 4, 44, 44}, s.paused ? "play" : "pause", s.paused, host_->text("hud.pause"))) host_->pause();
    x += 46;
    for (int k = 1; k <= 3; ++k) {
        const Rect b{x, r.y + 4, 44, 44};
        const bool on = !s.paused && s.speed == k;
        // walking by the keys holds the clock at 1: the others are greyed out
        const bool locked = speedLocked && k != 1;
        box(on ? Skin::ButtonOn : Skin::Button, b, 22);
        const float iw = 9.0f * k + 2;
        for (int t = 0; t < k; ++t) {
            const float ox = b.x + 22 - iw * 0.5f + t * 9.0f;
            const SDL_FColor col = on       ? SDL_FColor{0.11f, 0.13f, 0.17f, 1}
                                   : locked ? SDL_FColor{0.94f, 0.9f, 0.81f, 0.28f}
                                            : SDL_FColor{0.94f, 0.9f, 0.81f, 1};
            const SDL_Vertex v[3] = {{{ox, b.y + 15}, col, {0, 0}}, {{ox + 10, b.y + 22}, col, {0, 0}}, {{ox, b.y + 29}, col, {0, 0}}};
            SDL_RenderGeometry(sdl_, nullptr, v, 3, nullptr, 0);
        }
        if (locked) {
            ui_->claim(b);
            if (ui_->hovered(b)) {
                const std::string tip = host_->text("hud.speed_locked");
                const float w2 = textW(tip, 14) + 20;
                box(Skin::Leather, {b.x + 22 - w2 / 2, b.y - 44, w2, 28}, 14);
                label(b.x + 22 - w2 / 2 + 10, b.y - 39, tip, kText, 14);
            }
            x += 46;
            continue;
        }
        if (ui_->hovered(b)) {
            const std::string tip = host_->text("hud.speed", {{"n", k}});
            const float w2 = textW(tip, 14) + 20;
            box(Skin::Leather, {b.x + 22 - w2 / 2, b.y - 44, w2, 28}, 14);
            label(b.x + 22 - w2 / 2 + 10, b.y - 39, tip, kText, 14);
        }
        if (clicked(b)) host_->speed(k);
        x += 46;
    }
}

// The control bar over the clock: three ways to lead the person (keys, mouse,
// free) and, apart from them, the camera riding on the selected person.
void Hud::controlPill(float W, float H) {
    static const char* kModes[3][2] = {{"ctl_keys", "hud.control.keys"}, {"ctl_mouse", "hud.control.mouse"},
                                       {"ctl_free", "hud.control.free"}};
    const float w = 6 + 3 * 46 + 14 + 46 + 14 + 46 + 4;
    const Rect r{W - 36 - w, H - 30 - 52 - 62, w, 52};
    box(Skin::Leather, r, 26);
    float x = r.x + 6;
    for (int k = 0; k < 3; ++k) {
        std::string tip = host_->text(kModes[k][1]);
        if (k == 2) tip = host_->text(hasPerson ? "hud.control.release" : "hud.control.free");
        else if (!hasPerson)
            tip += " - " + (leadName.empty() ? host_->text("hud.control.take_none")
                                             : host_->text("hud.control.take", {{"who", leadName}}));
        if (roundButton({x, r.y + 4, 44, 44}, kModes[k][0], controlMode == k, tip)) host_->controlMode(k);
        x += 46;
    }
    ui_->roundRect({x + 5, r.y + 12, 2, 28}, kGold.withAlpha(0.45f), 1);
    x += 14;
    const std::string camTip = host_->text(cameraFollow ? "hud.control.camera_on" : "hud.control.camera_off");
    if (roundButton({x, r.y + 4, 44, 44}, cameraFollow ? "cam_lock" : "cam_free", cameraFollow, camTip))
        host_->cameraFollow(!cameraFollow);
    // AL-56: the walls (Home goes round the same way: cut away -> down -> up)
    x += 46;
    ui_->roundRect({x + 5, r.y + 12, 2, 28}, kGold.withAlpha(0.45f), 1);
    x += 14;
    static const char* kWalls[3][2] = {{"walls_up", "hud.walls.up"}, {"walls_cut", "hud.walls.cut"},
                                       {"walls_down", "hud.walls.down"}};
    const int wm = std::clamp(wallMode, 0, 2);
    if (roundButton({x, r.y + 4, 44, 44}, kWalls[wm][0], wm != 0, host_->text(kWalls[wm][1])))
        host_->wallMode(wm == 1 ? 2 : wm == 2 ? 0 : 1);
}

// Where a click on the ground sent the person: a gold ring on the ground.
void Hud::walkMarker() {
    if (!walkMark) return;
    float x, y;
    if (!host_->groundToScreen(walkMarkX, walkMarkY, 0.05, x, y)) return;
    const float pulse = 0.5f + 0.5f * float(std::sin(clock_ * 5.0));
    arc(x, y, 10 + 3 * pulse, 2.0f, 0, 360, kGold.withAlpha(0.85f));
    disc(x, y, 3, kGoldPale);
}

void Hud::thoughts(const Snapshot& s, float W) {
    if (s.detail.trace.empty()) return;
    const float w = 420, h = 300;
    const Rect r{W - 36 - w, 380, w, h};
    box(Skin::Leather, r, 18);
    title(r.x + 18, r.y + 12, host_->text("hud.sheet.thoughts"), kGoldPale, 20);
    float y = r.bottom() - 26;
    for (auto it = s.detail.trace.rbegin(); it != s.detail.trace.rend() && y > r.y + 44; ++it) {
        label(r.x + 16, y, ui_->fit(it->time + "  " + it->text, r.w - 32, 13 / 15.f), kText, 13);
        y -= 20;
    }
}

// ---------------------------------------------------------------- the sheet
void Hud::sheet(const Snapshot& s, const ActorSnap& a, float W, float H) {
    dimmer(W, H, 0.62f);
    const float ox = std::round((W - 1300) * 0.5f), oy = std::round((H - 780) * 0.5f);
    box(Skin::Leather, {ox, oy, 1300, 780}, 30);
    static const char* kTabs[][2] = {{"values", "hud.values"}, {"wants", "hud.wants"}, {"people", "hud.people"},
                                     {"calling", "hud.skills"}, {"things", "hud.things"}};
    for (int k = 0; k < 5; ++k)
        if (roundButton({ox - 80, oy + 90 + k * 70.0f, 56, 56}, kTabs[k][0], k == sheetTab_, host_->text(kTabs[k][1]))) {
            if (k == 4) panel = "inventory";   // things: the inventory with its grids and the body
            else sheetTab_ = k;
        }
    if (roundButton({ox + 1274, oy - 20, 48, 48}, "close", false, host_->text("hud.close"))) panel = "none";

    // left page: who, traits, needs, feelings
    const Rect L{ox + 30, oy + 30, 610, 720};
    box(Skin::Paper, L, 18);
    portrait(a, L.x + 34 + 54, L.y + 30 + 54, 54);
    ring(L.x + 34 + 54, L.y + 30 + 54, 54);
    title(L.x + 34 + 128, L.y + 40, a.name, kInk, 34);
    std::string sub = a.family + " · " + host_->text("hud.kin.age", {{"n", a.age}});
    label(L.x + 34 + 128, L.y + 86, sub, kInkSoft, 15);
    const int moodLevel = std::clamp(int((s.detail.mood + 1.0) * 2.5), 0, 4);
    label(L.x + 34 + 128, L.y + 110, host_->text("hud.mood." + std::to_string(moodLevel)), moodLevel <= 1 ? kInkRed : kGood, 15);
    float x = L.x + 34, y = L.y + 158;
    for (const auto& t : s.detail.traits) {
        const float w = textW(t, 14) + 24;
        if (x + w > L.right() - 30) x = L.x + 34, y += 34;
        box(Skin::Chip, {x, y, w, 28}, 14);
        label(x + 12, y + 5, t, kInk, 14);
        x += w + 8;
    }
    y += 46;
    title(L.x + 34, y, host_->text("hud.sheet.needs"), kInk, 22);
    ui_->line(L.x + 34, y + 30, L.right() - 34, y + 30, kLine.withAlpha(0.45f));
    y += 44;
    for (int t = 0; t < 5; ++t) {
        const auto rows = tierRows(s, a, t);
        bool alarm = false;
        for (const auto& r : rows) alarm |= r.alarm;
        title(L.x + 34, y + 8, host_->text(std::string("hud.tier.") + kTierKeys[t]), alarm ? kInkRed : kInk, 18);
        float cx = L.x + 34 + 138;
        const float cw = (L.right() - 34 - cx - 28) / 3;
        for (std::size_t i = 0; i < rows.size() && i < 3; ++i) {
            const auto& r = rows[i];
            label(cx, y, ui_->fit(r.label, cw - 4, 14 / 15.f), r.alarm ? kInkRed : kInk, 14);
            const Rect bar{cx, y + 22, cw, 8};
            ui_->roundRect(bar, kInk.withAlpha(0.16f), 4);
            if (r.value >= 0) ui_->roundRect({bar.x, bar.y, std::max(8.f, float(bar.w * r.value)), 8}, barColour(r.value), 4);
            cx += cw + 14;
        }
        y += 54;
    }
    y = std::max(y + 10, L.bottom() - 120);
    title(L.x + 34, y, host_->text("hud.sheet.moodlets"), kInk, 18);
    y += 30;
    x = L.x + 34;
    for (const auto& [nm, v] : s.detail.moodlets) {
        const float w = textW(nm, 14) + 24;
        if (x + w > L.right() - 30) {
            x = L.x + 34;
            y += 34;
            if (y > L.bottom() - 34) break;
        }
        box(Skin::Chip, {x, y, w, 28}, 14);
        label(x + 12, y + 5, nm, v < 0 ? kInkRed : kGood, 14);
        x += w + 8;
    }

    // right page: the tab
    const Rect R{ox + 660, oy + 30, 610, 720};
    box(Skin::Paper, R, 18);
    seal(R.right() - 37, R.y + 5);
    title(R.x + 34, R.y + 30, host_->text(kTabs[sheetTab_][1]), kInk, 22);
    ui_->line(R.x + 34, R.y + 62, R.right() - 34, R.y + 62, kLine.withAlpha(0.45f));
    y = R.y + 80;
    const Detail& d = s.detail;
    if (sheetTab_ == 0) {   // values: reputation axes as a scale, traits below
        if (d.reputation.empty()) {
            label(R.x + 34, y, host_->text("hud.sheet.no_values"), kInkSoft, 15);
            y += 30;
        }
        for (const auto& [nm, v] : d.reputation) {
            label(R.x + 34, y, nm, kInk, 15);
            const Rect bar{R.x + 230, y + 7, 300, 8};
            ui_->roundRect(bar, kInk.withAlpha(0.16f), 4);
            const float m = bar.x + bar.w * float(std::clamp(0.5 + v * 0.5, 0.0, 1.0));
            box(Skin::ButtonOn, {m - 10, y + 1, 20, 20}, 10);
            y += 40;
        }
        y += 10;
        ui_->paragraph(R.x + 34, y, R.w - 68, host_->text("hud.sheet.values_note"), kInkSoft, 14 / 15.f);
    } else if (sheetTab_ == 1) {   // wants, and goals to suggest
        if (d.wants.empty()) label(R.x + 34, y, host_->text("hud.sheet.no_wants"), kInkSoft, 15);
        for (const auto& w : d.wants) {
            title(R.x + 34, y, ui_->fit(w, R.w - 68, 21 / 15.f, true), kInk, 21);
            y += 34;
            ui_->line(R.x + 34, y, R.right() - 34, y, kLine.withAlpha(0.25f));
            y += 12;
            if (y > R.bottom() - 120) break;
        }
        // goals to suggest ("want / don't want") or, for the one you lead, to order
        y = std::max(y + 6, R.y + 220);
        title(R.x + 34, y, host_->text("hud.sheet.goals"), kInk, 18);
        y += 34;
        const auto goals = host_->goals();
        float gx = R.x + 34;
        for (std::size_t i = 0; i < goals.size(); ++i) {
            const float w = textW(goals[i].second, 13) + 22;
            if (gx + w > R.right() - 34) {
                gx = R.x + 34;
                y += 34;
            }
            if (y > R.bottom() - 170) break;
            const Rect b{gx, y, w, 28};
            box(int(i) == goalPick_ ? Skin::ChipOn : Skin::Chip, b, 14);
            label(b.x + 11, b.y + 6, goals[i].second, int(i) == goalPick_ ? kText : kInk, 13);
            if (clicked(b)) goalPick_ = int(i);
            gx += w + 6;
        }
        if (goalPick_ >= 0 && goalPick_ < int(goals.size())) {
            const std::string& g = goals[std::size_t(goalPick_)].first;
            float bx = R.x + 34;
            const float by = R.bottom() - 150;
            const std::string tw = host_->text("game.want"), tn = host_->text("game.dont_want");
            if (textButton({bx, by, textW(tw, 15) + 40, 44}, tw, false))
                host_->command("hint", {{"id", a.id}, {"goal", g}, {"delta", 0.4}, {"ttl", 144}});
            bx += textW(tw, 15) + 50;
            if (textButton({bx, by, textW(tn, 15) + 40, 44}, tn, false))
                host_->command("hint", {{"id", a.id}, {"goal", g}, {"delta", -0.4}, {"ttl", 144}});
            bx += textW(tn, 15) + 50;
            if (a.player) {
                const std::string to = host_->text("game.order");
                if (textButton({bx, by, textW(to, 15) + 40, 44}, to, true)) host_->command("set_goal", {{"id", a.id}, {"goal", g}});
            }
        }
        ui_->paragraph(R.x + 34, R.bottom() - 70, R.w - 68, host_->text("hud.sheet.wants_note"), kInkSoft, 14 / 15.f);
    } else if (sheetTab_ == 2) {   // people
        if (d.relations.empty()) label(R.x + 34, y, host_->text("hud.rel.none"), kInkSoft, 15);
        for (const auto& r : d.relations) {
            const ActorSnap* o = actor(s, r.id);
            if (o) {
                portrait(*o, R.x + 34 + 24, y + 24, 24);
                arc(R.x + 58, y + 24, 25, 1.5f, 0, 360, kGoldPale);
                arc(R.x + 58, y + 24, 27, 2, 0, 360, kGoldDark);
            }
            title(R.x + 100, y + 2, r.name, kInk, 20);
            if (!r.kin.empty()) label(R.x + 100 + textW(r.name, 20, true) + 8, y + 6, "· " + host_->text("hud.rel." + r.kin), kInkSoft, 15);
            const Rect bar{R.x + 100, y + 32, 300, 8};
            ui_->roundRect(bar, kInk.withAlpha(0.16f), 4);
            const float mid = bar.x + bar.w * 0.5f, len = float(std::fabs(r.affinity)) * bar.w * 0.5f;
            ui_->roundRect({r.affinity >= 0 ? mid : mid - len, bar.y, std::max(4.f, len), 8}, r.affinity >= 0 ? kGood : kBad, 4);
            ui_->rect({mid - 1, bar.y - 3, 2, 14}, kGoldDark);
            const std::string what = r.enmity > 0.3 ? "hates" : r.fear > 0.3 ? "fears" : r.affinity > 0.25 ? "likes"
                                   : r.affinity < -0.25 ? "dislikes" : r.trust > 0.3 ? "trust" : "knows";
            const std::string t = host_->text("hud.rel." + what);
            label(R.right() - 34 - textW(t, 15), y + 26, t, kInkSoft, 15);
            if (o && clicked({R.x + 34, y, R.w - 68, 52})) host_->select(o->id);
            y += 64;
            if (y > R.bottom() - 60) break;
        }
    } else if (sheetTab_ == 3) {   // skills
        if (d.skills.empty()) label(R.x + 34, y, host_->text("hud.sheet.no_skills"), kInkSoft, 15);
        for (const auto& [nm, v] : d.skills) {
            label(R.x + 34, y, nm, kInk, 15);
            const Rect bar{R.x + 260, y + 7, 280, 8};
            ui_->roundRect(bar, kInk.withAlpha(0.16f), 4);
            ui_->roundRect({bar.x, bar.y, std::max(8.f, float(bar.w * std::clamp(v, 0.0, 1.0))), 8}, kGoldDark, 4);
            y += 34;
            if (y > R.bottom() - 40) break;
        }
    } else {   // things
        if (d.inventory.empty()) label(R.x + 34, y, host_->text("hud.sheet.no_things"), kInkSoft, 15);
        for (const auto& it : d.inventory) {
            label(R.x + 34, y, it.name, kInk, 15);
            const std::string n = "x" + std::to_string(it.count);
            label(R.right() - 34 - textW(n, 15), y, n, kInkSoft, 15);
            y += 30;
            if (y > R.bottom() - 40) break;
        }
    }
    if (in_->escape) panel = "none";
}

// ---------------------------------------------------------------- the wheel
void Hud::wheel(const Snapshot& s, float W, float H) {
    const ActorSnap* t = actor(s, wheelTarget_);
    if (!t || t->animal) {
        panel = "none";
        return;
    }
    host_->wheelTarget(t->id);
    const bool edge = !hudPalette().theme.flag("textured");   // AL-65: diamonds, glass, capitals
    dimmer(W, H, 0.5f);
    const sim::Json spheres = wheelSpec_.value("spheres", sim::Json::array());
    const int n = int(spheres.size());
    if (!n) return;
    sphere_ = std::clamp(sphere_, 0, n - 1);
    const float k = std::min(W / 1600.f, H / 900.f);
    const float ox = (W - 1600 * k) * 0.5f, oy = (H - 900 * k) * 0.5f;
    auto X = [&](float v) { return ox + v * k; };
    auto Y = [&](float v) { return oy + v * k; };
    const float cx = X(360), cy = Y(450), R = 230 * k;
    if (roundButton({X(1540), Y(30), 48, 48}, "close", false, host_->text("hud.close")) || in_->escape) {
        panel = "none";
        return;
    }
    // the dashed orbit
    for (int d = 0; d < 72; d += 2) arc(cx, cy, R, 2, d * 5.0f, 2.0f, kGoldPale.withAlpha(0.35f));
    const sim::Json& sp = spheres[std::size_t(sphere_)];
    const sim::Json subs = sp.value("subs", sim::Json::array());
    const int m = int(subs.size());
    sub_ = std::clamp(sub_, 0, std::max(0, m - 1));
    const float y0 = Y(450) - (m * 72 - 16) * k * 0.5f;
    auto posOf = [&](int i, float rr) {
        const float a = -kPiF / 2 + i * 2 * kPiF / n;
        return std::pair<float, float>{cx + rr * std::cos(a), cy + rr * std::sin(a)};
    };
    // links: sphere -> subs -> paper
    {
        const auto [sx, sy] = posOf(sphere_, R);
        for (int i = 0; i < m; ++i) {
            const float ty = y0 + i * 72 * k + 28 * k;
            const bool on = i == sub_;
            std::vector<SDL_FPoint> pts;
            for (int q = 0; q <= 24; ++q) {
                const float u = q / 24.f, v = 1 - u;
                const float px = v * v * v * sx + 3 * v * v * u * (sx + 150 * k) + 3 * v * u * u * X(560) + u * u * u * X(700);
                const float py = v * v * v * sy + 3 * v * v * u * sy + 3 * v * u * u * ty + u * u * u * ty;
                pts.push_back({px, py});
            }
            for (std::size_t q = 1; q < pts.size(); ++q) {
                if (!on && q % 2) continue;
                const float w = on ? 4.0f : 2.5f;
                const Colour c = on ? kGoldPale : kGoldPale.withAlpha(0.55f);
                const float dx = pts[q].x - pts[q - 1].x, dy = pts[q].y - pts[q - 1].y, l = std::max(0.001f, std::hypot(dx, dy));
                const float nx = -dy / l * w * 0.5f, ny = dx / l * w * 0.5f;
                const SDL_FColor col{c.r, c.g, c.b, c.a};
                const SDL_Vertex v[4] = {{{pts[q - 1].x + nx, pts[q - 1].y + ny}, col, {0, 0}}, {{pts[q].x + nx, pts[q].y + ny}, col, {0, 0}},
                                         {{pts[q].x - nx, pts[q].y - ny}, col, {0, 0}}, {{pts[q - 1].x - nx, pts[q - 1].y - ny}, col, {0, 0}}};
                const int idx[6] = {0, 1, 2, 0, 2, 3};
                SDL_RenderGeometry(sdl_, nullptr, v, 4, idx, 6);
            }
        }
    }
    // the target in the middle
    if (edge) {
        diamond(cx, cy, 88 * k, false, true);
        portrait(*t, cx, cy, 55 * k);
        const float w = capsW(t->name, 17, 0.14f) + 36;
        glass({cx - w / 2, cy + 72 * k, w, 32}, true);
        caps(cx - w / 2 + 18, cy + 72 * k + 6, t->name, kText, 17, 0.14f);
    } else {
        portrait(*t, cx, cy, 62 * k);
        ring(cx, cy, 62 * k);
        const float w = textW(t->name, 19, true) + 32;
        box(Skin::Leather, {cx - w / 2, cy + 72 * k, w, 32}, 16);
        title(cx - w / 2 + 16, cy + 72 * k + 5, t->name, kText, 19);
    }
    // the spheres
    for (int i = 0; i < n; ++i) {
        const auto [px, py] = posOf(i, R);
        const std::string id = spheres[std::size_t(i)].value("id", std::string());
        const std::string nm = host_->text("hud.wheel.sphere." + id);
        const Rect b{px - 36 * k, py - 36 * k, 72 * k, 72 * k};
        const bool on = i == sphere_;
        if (edge) {
            diamond(px, py, 52 * k, on, true);
        } else {
            box(Skin::Leather, b, b.w * 0.5f);
            if (on) box(Skin::ButtonOn, b.inset(4), b.w * 0.5f - 4);
        }
        icon(spheres[std::size_t(i)].value("icon", std::string()), {px - 15 * k, py - 15 * k, 30 * k, 30 * k},
             on ? (edge ? kGoldPale : kDark) : kText);
        // the name under the sphere (out to the side it ran into the column of sub-directions)
        const float lx = px, ly = py + 36 * k + 22;
        if (edge) {
            const float w = capsW(nm, 15, 0.12f);
            caps(lx - w / 2, ly - 8, nm, on ? kGold : kText, 15, 0.12f, true, true);
        } else {
            const float w = textW(nm, 17, true);
            ui_->text(lx - w / 2, ly - 10 + 2, nm, kRim, 17 / 15.f, true);
            ui_->text(lx - w / 2, ly - 10, nm, on ? kGoldPale : kText, 17 / 15.f, true);
        }
        if (clicked(b)) {
            sphere_ = i;
            sub_ = 0;
        }
    }
    // the sub-directions
    for (int i = 0; i < m; ++i) {
        const std::string id = subs[std::size_t(i)].value("id", std::string());
        const Rect b{X(700), y0 + i * 72 * k, 230 * k, 56 * k};
        const bool on = i == sub_;
        if (edge) {
            glass(b, false);
            if (on) selRow(b);
            caps(b.x + 22, b.y + 8 * k, host_->text("hud.wheel.sub." + id), on ? kGold : kText, 15, 0.1f);
            label(b.x + 22, b.y + 31 * k, ui_->fit(host_->text("hud.wheel.sub." + id + ".d"), b.w - 30, 13 / 15.f),
                  kMute, 13);
        } else {
            box(on ? Skin::ButtonOn : Skin::Leather, b, b.h * 0.5f);
            if (on) ui_->border(b, kRim, 1.5f, b.h * 0.5f);
            title(b.x + 22, b.y + 7 * k, host_->text("hud.wheel.sub." + id), on ? kDark : kText, 18);
            label(b.x + 22, b.y + 31 * k, host_->text("hud.wheel.sub." + id + ".d"), on ? kDark.withAlpha(0.8f) : kText.withAlpha(0.8f), 13);
        }
        if (clicked(b)) sub_ = i;
    }
    // the acts on paper
    const Rect P{X(1010), Y(170), 540 * k, 560 * k};
    box(Skin::Paper, P, 18);
    seal(P.right() - 37, P.y + 5);
    const std::string subId = m ? subs[std::size_t(sub_)].value("id", std::string()) : std::string();
    if (edge) caps(P.x + 24, P.y + 24, host_->text("hud.wheel.sub." + subId), kGold, 20, 0.14f);
    else title(P.x + 24, P.y + 22, host_->text("hud.wheel.sub." + subId), kInk, 24);
    ui_->line(P.x + 24, P.y + 58, P.right() - 24, P.y + 58, kLine.withAlpha(0.45f));
    float y = P.y + 66;
    const bool canAct = s.controlled != sim::kNone && s.controlled != t->id;
    const std::string cat = sp.value("id", std::string());
    const double base = wheelSpec_.value("base_chance", sim::Json::object()).value(cat, 0.5);
    const RelSnap& rel = s.wheel.toActor;
    const ActInfoSnap* cardFor = nullptr;
    float cardY = 0;
    if (m) {
        const sim::Json acts = subs[std::size_t(sub_)].value("interactions", sim::Json::array());
        for (const auto& av : acts) {
            const std::string id = av.get<std::string>();
            if (y > P.bottom() - 120) break;
            const Rect row{P.x + 20, y, P.w - 40, 50};
            const bool hot = ui_->hovered(row) && canAct;
            if (hot && edge) selRow(row);
            else if (hot) ui_->roundRect(row, kLine.withAlpha(0.12f), 10);
            // The sim's own chance (sim/tones.hpp actChance) for this tone; the
            // old estimate from the relation only while the sim has not sent it yet.
            double chance = std::clamp(base + 0.3 * rel.affinity + 0.2 * rel.trust - 0.15 * rel.enmity, 0.02, 0.97);
            std::string kind;
            if (auto ch = s.wheel.chance.find(id); ch != s.wheel.chance.end() && !ch->second.empty()) {
                chance = ch->second[std::size_t(std::clamp(tone_, 0, int(ch->second.size()) - 1))];
                kind = s.wheel.kind.count(id) ? s.wheel.kind.at(id) : std::string();
            }
            const int pc = int(std::round(chance * 100));
            const std::string nm = host_->interactionName(id);
            label(row.x + 8, row.y + 6, ui_->fit(nm, row.w - 80, 1.0f), canAct ? kInk : kInkSoft, 15);
            const ActInfoSnap* info = actInfo(s, t->id, id);
            if (info && !info->can && !info->why.empty())
                label(row.x + 8, row.y + 28, ui_->fit(host_->text("hud.act.missing", {{"what", info->why}}), row.w - 80, 12 / 15.f), kInkRed, 12);
            else if (!kind.empty()) label(row.x + 8, row.y + 28, host_->text("hud.wheel.kind." + kind), kInkSoft, 12);
            if (info && ui_->hovered(row)) cardFor = info, cardY = row.y;
            const float ccx = row.right() - 26, ccy = row.y + 25;
            arc(ccx, ccy, 18, 4, 0, 360, kInk.withAlpha(0.16f));
            arc(ccx, ccy, 18, 4, -90, float(360 * chance), chance > 0.6 ? kGood : chance > 0.35 ? kMid : kBad);
            const std::string ps = std::to_string(pc);
            label(ccx - textW(ps, 13) / 2, ccy - 9, ps, kInk, 13);
            ui_->line(row.x, row.bottom() + 2, row.right(), row.bottom() + 2, kLine.withAlpha(0.25f));
            if (canAct && clicked(row)) {
                sim::Json args = {{"id", s.controlled}, {"interaction", id}, {"target", t->id}};
                if (tone_ >= 0 && tone_ < int(s.wheel.tones.size())) args["tone"] = s.wheel.tones[std::size_t(tone_)];
                host_->command("perform", args);
                panel = "none";
            }
            y += 54;
        }
    }
    y = P.bottom() - 104;
    label(P.x + 24, y, host_->text("hud.wheel.tone"), kInkSoft, 13);
    float x = P.x + 24;
    y += 22;
    for (int i = 0; i < 7; ++i) {
        const std::string tn = host_->text("hud.wheel.tone." + std::to_string(i));
        const float w = textW(tn, 14) + 28;
        if (x + w > P.right() - 20) x = P.x + 24, y += 40;
        const Rect b{x, y, w, 34};
        if (edge) {
            if (textButton(b, tn, i == tone_)) tone_ = i;
        } else {
            box(i == tone_ ? Skin::ChipOn : Skin::Chip, b, 17);
            label(b.x + 14, b.y + 8, tn, i == tone_ ? kText : kInk, 14);
            if (clicked(b)) tone_ = i;
        }
        x += w + 8;
    }
    // AL-67: the topics the person could bring up with the target (the menu without an agent)
    if (canAct && edge) {
        const std::string tl = host_->text("hud.talk.open");
        const float bw = capsW(tl, 15, 0.12f, false) + 40;
        if (edgeButton({P.x, P.y - 50, bw, 36}, tl)) {
            openTalk(t->id);
            return;
        }
    }
    if (cardFor) actCard(*cardFor, P.right() + 16 + 330 <= W - 12 ? P.right() + 16 : P.x - 346, cardY, W, H);
    const std::string note = !canAct ? host_->text(s.controlled == t->id ? "hud.wheel.self" : "hud.wheel.need_control")
                                     : host_->text(s.wheel.chance.empty() ? "hud.wheel.estimate" : "hud.wheel.real");
    label(P.x + 24, P.bottom() + 14, ui_->fit(note, P.w, 13 / 15.f), canAct ? kText.withAlpha(0.8f) : kRingBad, 13, true);
}

// The act menu over a beast or a thing, opened where it was clicked: a
// leather plate with the target's name and its state (or what it holds),
// the acts under it one row each - an act one cannot do now dimmed, its
// first unmet need on a line of its own - so nothing ever lies over
// anything else however many acts there are. Hovering a row shows what it
// needs and gives beside the plate; a click elsewhere or Esc closes it.
void Hud::actMenu(const Snapshot& s, EntityId target, const std::string& head, const std::vector<std::string>& sub,
                  const std::vector<std::string>& acts, float W, float H) {
    const bool fresh = beastFresh_;
    beastFresh_ = false;
    const bool canAct = s.controlled != sim::kNone;
    struct Row {
        std::string id, name, why;
        bool ok = true;
        const ActInfoSnap* info = nullptr;
        float h = 40;
    };
    std::vector<Row> rows;
    float w = std::max(260.0f, textW(head, 19, true) + 40);
    for (const auto& l : sub) w = std::max(w, textW(l, 13) + 40);
    for (const auto& id : acts) {
        Row r;
        r.id = id;
        r.info = actInfo(s, target, id);
        r.name = r.info ? r.info->name : host_->interactionName(id);
        r.ok = canAct && (!r.info || r.info->can);
        if (r.info && !r.info->can && !r.info->why.empty()) r.why = r.info->why, r.h = 58;
        w = std::max(w, textW(r.name, 15) + 60);
        if (!r.why.empty()) w = std::max(w, std::min(420.0f, textW(r.why, 12) + 60));
        rows.push_back(std::move(r));
    }
    w = std::min(w, 440.0f);
    float h = 18 + 30 + 20.0f * float(sub.size()) + 10;
    for (const auto& r : rows) h += r.h + 6;
    std::string note;
    if (rows.empty() || !canAct) {
        note = host_->text(!canAct ? "hud.wheel.need_control" : "hud.thing.nothing");
        w = std::max(w, std::min(440.0f, textW(note, 14) + 40));
        h += 34;
    }
    // beside the click, kept on the screen
    float x = beastX_ + 28;
    if (x + w > W - 12) x = beastX_ - 28 - w;
    x = std::clamp(x, 12.0f, std::max(12.0f, W - w - 12));
    const float y = std::clamp(beastY_ - h * 0.4f, 90.0f, std::max(90.0f, H - h - 110));
    const Rect P{x, y, w, h};
    // the point clicked, tied to the plate
    arc(beastX_, beastY_, 9, 2, 0, 360, kGold);
    ui_->line(beastX_ + (x > beastX_ ? 9 : -9), beastY_, x > beastX_ ? x : x + w, std::clamp(beastY_, y + 20, y + h - 20),
              kGoldPale.withAlpha(0.55f));
    box(Skin::Leather, P, 18);
    bool over = ui_->hovered(P);
    if (!hudPalette().theme.flag("textured")) caps(P.x + 18, P.y + 14, ui_->fit(head, w - 60, 19 / 15.f, true), kGold, 17, 0.12f);
    else title(P.x + 18, P.y + 12, ui_->fit(head, w - 36, 19 / 15.f, true), kGoldPale, 19);
    float yy = P.y + 46;
    for (const auto& l : sub) {
        label(P.x + 18, yy, ui_->fit(l, w - 36, 13 / 15.f), kText, 13);
        yy += 20;
    }
    yy += 8;
    const Row* hovered = nullptr;
    float hoveredY = 0;
    for (const auto& r : rows) {
        const Rect b{P.x + 12, yy, w - 24, 40};
        if (r.ok) {
            if (textButton(b, ui_->fit(r.name, b.w - 20, 1.0f), true)) {
                host_->command("perform", {{"id", s.controlled}, {"interaction", r.id}, {"target", target}});
                panel = "none";
                return;
            }
        } else {
            box(Skin::Chip, b, b.h * 0.5f);
            ui_->claim(b);
            const std::string nm = ui_->fit(r.name, b.w - 28, 1.0f);
            label(b.x + (b.w - textW(nm, 15)) * 0.5f, b.y + 11, nm, kInkSoft, 15);
        }
        if (!r.why.empty())
            label(b.x + 14, b.bottom() + 2, ui_->fit(host_->text("hud.act.missing", {{"what", r.why}}), b.w - 20, 12 / 15.f),
                  kRingBad, 12, true);
        if (ui_->hovered({b.x, b.y, b.w, r.h})) {
            hovered = &r;
            hoveredY = b.y;
        }
        yy += r.h + 6;
    }
    if (!note.empty()) {
        const Rect n{P.x + 12, yy, w - 24, 28};
        box(Skin::Paper, n, 14);
        label(n.x + 12, n.y + 5, ui_->fit(note, n.w - 24, 14 / 15.f), kInk, 14);
    }
    ui_->claim(P);
    if (hovered && hovered->info) {
        const float cx = P.right() + 12 + 330 <= W - 12 ? P.right() + 12 : P.x - 342;
        actCard(*hovered->info, cx, hoveredY, W, H);
    }
    if (!fresh && in_->pressed && !over) panel = "none";
}

void Hud::beast(const Snapshot& s, float W, float H) {
    const ActorSnap* t = actor(s, beastTarget_);
    if (!t || !t->animal || !t->alive || in_->escape) {
        panel = "none";
        return;
    }
    static const std::vector<std::string> kNoActs;
    const auto it = beastActs.find(t->species);
    const std::vector<std::string>& acts = it == beastActs.end() ? kNoActs : it->second;
    actMenu(s, t->id, t->name + (t->group > 1 ? "  x" + std::to_string(t->group) : ""),
            {host_->text("game.fsm." + t->state)}, acts, W, H);
}

void Hud::thing(const Snapshot& s, float W, float H) {
    const ThingSnap* t = nullptr;
    for (const auto& x : s.things)
        if (x.id == thingTarget_) t = &x;
    if (!t || in_->escape) {
        panel = "none";
        return;
    }
    std::vector<std::string> sub;
    for (std::size_t k = 0; k < t->stock.size() && k < 4; ++k)
        sub.push_back(t->stock[k].first + ": " + std::to_string(t->stock[k].second));
    if (t->busy) sub.push_back(host_->text("hud.hint.busy"));
    std::vector<std::string> acts;
    if (s.actsFor == t->id)
        for (const auto& a : s.acts) acts.push_back(a.id);
    actMenu(s, t->id, t->name, sub, acts, W, H);
}

const ActInfoSnap* Hud::actInfo(const Snapshot& s, EntityId target, const std::string& id) const {
    if (s.actsFor != target) return nullptr;
    for (const auto& a : s.acts)
        if (a.id == id) return &a;
    return nullptr;
}

// What an act needs (each met or not) and gives, and how long it takes.
void Hud::actCard(const ActInfoSnap& a, float x, float y, float W, float H) {
    const float w = 330;
    float h = 58 + 22.0f * float(a.needs.size()) + 22.0f * float(a.gains.size() + a.toTarget.size()) +
              (a.needs.empty() ? 0 : 26) + (a.gains.empty() && a.toTarget.empty() ? 0 : 26);
    if (x + w > W - 12) x = W - 12 - w;
    y = std::clamp(y, 12.0f, std::max(12.0f, H - h - 12));
    const Rect r{x, y, w, h};
    box(Skin::Paper, r, 14);
    if (!hudPalette().theme.flag("textured")) caps(r.x + 16, r.y + 12, ui_->fit(a.name, w - 100, 18 / 15.f), kGold, 15, 0.12f);
    else title(r.x + 16, r.y + 10, ui_->fit(a.name, w - 32, 18 / 15.f), kInk, 18);
    std::string len;
    if (a.minutes > 0)
        len = a.minutes >= 60 ? host_->text("hud.act.hours", {{"n", (a.minutes + 30) / 60}})
                              : host_->text("hud.act.minutes", {{"n", a.minutes}});
    if (!len.empty()) label(r.right() - 16 - textW(len, 13), r.y + 14, len, kInkSoft, 13);
    float yy = r.y + 40;
    if (!a.needs.empty()) {
        label(r.x + 16, yy, host_->text("hud.act.needs"), kInkSoft, 13);
        yy += 24;
        for (const auto& [t, ok] : a.needs) {
            icon(ok ? "a_ok" : "close", {r.x + 16, yy, 16, 16}, ok ? kGood : kInkRed);
            label(r.x + 38, yy, ui_->fit(t, w - 54, 14 / 15.f), ok ? kInk : kInkRed, 14);
            yy += 22;
        }
    }
    if (!a.gains.empty() || !a.toTarget.empty()) {
        label(r.x + 16, yy + 2, host_->text("hud.act.gives"), kInkSoft, 13);
        yy += 26;
        for (const auto& t : a.gains) {
            label(r.x + 38, yy, ui_->fit(t, w - 54, 14 / 15.f), kInk, 14);
            yy += 22;
        }
        for (const auto& t : a.toTarget) {
            label(r.x + 38, yy, ui_->fit(host_->text("hud.act.to_target", {{"what", t}}), w - 54, 14 / 15.f), kInk, 14);
            yy += 22;
        }
    }
}

// The hint over what the mouse is on: who it is and what they are busy
// with, or what a thing is and holds.
void Hud::hoverHint(const Snapshot& s, float W) {
    std::vector<std::string> lines;
    std::string head;
    if (hoverActor != sim::kNone) {
        const ActorSnap* a = actor(s, hoverActor);
        if (!a) return;
        head = a->name + (a->group > 1 ? "  x" + std::to_string(a->group) : "");
        if (!a->alive) lines.push_back(host_->text("game.dead"));
        else {
            if (!a->act.empty()) lines.push_back(host_->text("hud.hint.doing", {{"what", a->act}}));
            else lines.push_back(host_->text("game.fsm." + a->state));
            if (!a->goal.empty() && a->goal != a->act) lines.push_back(host_->text("hud.hint.goal", {{"what", a->goal}}));
            if (a->progress >= 0) lines.push_back(host_->text("hud.hint.progress", {{"n", int(std::round(a->progress * 100))}}));
        }
    } else if (hoverThing != sim::kNone) {
        const ThingSnap* t = nullptr;
        for (const auto& x : s.things)
            if (x.id == hoverThing) t = &x;
        if (!t) return;
        head = t->name;
        for (const auto& [k, v] : t->stock) lines.push_back(k + ": " + std::to_string(v));
        if (t->busy) lines.push_back(host_->text("hud.hint.busy"));
        lines.push_back(host_->text("hud.hint.click_thing"));
    } else {
        return;
    }
    float w = textW(head, 16, true) + 28;
    for (const auto& l : lines) w = std::max(w, textW(l, 13) + 28);
    const float h = 36 + 19.0f * float(lines.size());
    const float x = std::min(in_->mouseX + 18, W - w - 8), y = std::max(8.0f, in_->mouseY - h - 10);
    box(Skin::Leather, {x, y, w, h}, 12);
    title(x + 14, y + 8, head, kGoldPale, 16);
    float yy = y + 32;
    for (const auto& l : lines) {
        label(x + 14, yy, l, kText, 13);
        yy += 19;
    }
}

// What came of the person's doing, floating up over their head and fading;
// a bar over them while an act goes on.
void Hud::feedbackFloats(const Snapshot& s) {
    for (const auto& f : s.feedback)
        if (f.seq > floatSeq_) {
            floatSeq_ = f.seq;
            floats_.push_back({f.who, f.text, f.kind, clock_ + 0.75 * double(floats_.size() % 4)});
        }
    if (floats_.size() > 8) floats_.erase(floats_.begin(), floats_.end() - 8);
    const ActorSnap* me = actor(s, s.controlled);
    if (me && me->progress >= 0 && me->alive) {
        float x, y;
        if (host_->overHead(*me, x, y)) {
            const double p = std::clamp(me->progress + host_->tickAlpha() / std::max(1, me->actTicks), 0.0, 1.0);
            const Rect bar{x - 50, y - 66, 100, 10};   // over the need bubble (y - 50 .. y - 18)
            ui_->roundRect(bar, kRim.withAlpha(0.8f), 5);
            ui_->roundRect({bar.x + 2, bar.y + 2, float((bar.w - 4) * p), bar.h - 4}, kGold, 3);
            if (!me->act.empty()) {
                const std::string t = ui_->fit(me->act, 220, 13 / 15.f);
                label(x - textW(t, 13) / 2, bar.y - 20, t, kGoldPale, 13, true);
            }
        }
    }
    int stack = 0;
    for (auto it = floats_.begin(); it != floats_.end();) {
        const double age = clock_ - it->born;
        if (age > 3.2) {
            it = floats_.erase(it);
            continue;
        }
        if (age < 0) {
            ++it;
            continue;
        }
        const ActorSnap* a = actor(s, it->who);
        float x, y;
        if (a && host_->overHead(*a, x, y)) {
            const float rise = float(age) * 26.0f + float(stack) * 4.0f;
            const float alpha = float(std::clamp(1.0 - (age - 2.2), 0.0, 1.0));
            const Colour c = it->kind == 1 ? kRingGood.withAlpha(alpha) : it->kind == 2 ? kRingBad.withAlpha(alpha)
                                                                                              : kText.withAlpha(alpha);
            const float tw = textW(it->text, 16, true);
            if (const auto bt = bubbleTop_.find(it->who); bt != bubbleTop_.end()) y = std::min(y, bt->second + 100.0f);
            ui_->text(x - tw / 2, y - 112 - rise + 2, it->text, kRim.withAlpha(alpha * 0.85f), 16 / 15.f, true);
            ui_->text(x - tw / 2, y - 112 - rise, it->text, c, 16 / 15.f, true);
            ++stack;
        }
        ++it;
    }
}

// AL-57: what people say, on a slip of paper over the speaker, for a time that
// grows with the line; one slip per speaker (a new line takes the old one's
// place), lifted over the name, the need bubble and the act's bar, and over
// the slips of the others when they would lie on each other.
void Hud::speechBubbles(const Snapshot& s) {
    for (const auto& sp : s.speech)
        if (sp.seq > speechSeq_) {
            speechSeq_ = sp.seq;
            if (sp.line.empty()) continue;
            const double life = std::clamp(2.4 + 0.055 * double(sp.line.size()) / 2.0, 3.0, 7.0); // UTF-8 bytes / 2 ~ letters
            // AL-73: a talk's lines come in turn, one after the other, not all
            // at once (a tick says both sides' lines; a slow tick is seconds long)
            const double at = std::max(clock_, bubbleNext_);
            bubbleNext_ = at + std::min(2.2, life * 0.6);
            bubbleQueue_.push_back({sp.speaker, sp.line, at, life});
        }
    for (auto it = bubbleQueue_.begin(); it != bubbleQueue_.end();) {
        if (it->born > clock_) {
            ++it;
            continue;
        }
        const EntityId who = it->who;
        bubbles_.erase(std::remove_if(bubbles_.begin(), bubbles_.end(), [&](const Bubble& b) { return b.who == who; }),
                       bubbles_.end());
        bubbles_.push_back(*it);
        it = bubbleQueue_.erase(it);
    }
    if (bubbleQueue_.size() > 12) bubbleQueue_.erase(bubbleQueue_.begin(), bubbleQueue_.end() - 12);
    bubbles_.erase(std::remove_if(bubbles_.begin(), bubbles_.end(), [&](const Bubble& b) { return clock_ - b.born > b.life; }),
                   bubbles_.end());
    if (bubbles_.size() > 10) bubbles_.erase(bubbles_.begin(), bubbles_.end() - 10);
    bubbleTop_.clear();
    const float px = 14, lineH = 18, maxW = 260;
    auto wrap = [&](const std::string& t) {
        std::vector<std::string> lines;
        std::string line;
        std::size_t pos = 0;
        while (pos < t.size()) {
            const std::size_t sp = t.find(' ', pos);
            const std::string word = t.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
            const std::string next = line.empty() ? word : line + " " + word;
            if (!line.empty() && textW(next, px) > maxW) {
                lines.push_back(line);
                line = word;
            } else line = next;
            pos = sp == std::string::npos ? t.size() : sp + 1;
        }
        if (!line.empty()) lines.push_back(line);
        if (lines.size() > 3) {
            lines[2] = ui_->fit(lines[2] + " " + lines[3] + " ...", maxW, px / 15.f);
            lines.resize(3);
        }
        return lines;
    };
    std::vector<Rect> taken;
    auto clash = [&](const Rect& r) {
        for (const auto& t : taken)
            if (r.x < t.right() && t.x < r.right() && r.y < t.bottom() && t.y < r.bottom()) return true;
        return false;
    };
    // the newest on top: drawn last, placed first
    for (auto it = bubbles_.rbegin(); it != bubbles_.rend(); ++it) {
        const ActorSnap* a = actor(s, it->who);
        float x, y;
        if (!a || !a->alive || !host_->overHead(*a, x, y)) continue;
        const auto lines = wrap(it->text);
        if (lines.empty()) continue;
        float w = 0;
        for (const auto& l : lines) w = std::max(w, textW(l, px));
        const bool busy = a->id == s.controlled && a->progress >= 0;
        const float bottom = y - (busy ? 92.0f : 58.0f);
        Rect r{x - w * 0.5f - 12, bottom - (float(lines.size()) * lineH + 12), w + 24, float(lines.size()) * lineH + 12};
        for (int lift = 0; clash(r) && lift < 3; ++lift) r.y -= r.h + 6;
        taken.push_back(r);
        bubbleTop_[it->who] = r.y;
        const double age = clock_ - it->born;
        const float alpha = float(std::clamp(std::min(age / 0.18, (it->life - age) / 0.6), 0.0, 1.0));
        ui_->roundRect({r.x + 2, r.y + 3, r.w, r.h}, kRim.withAlpha(0.35f * alpha), 9);
        ui_->roundRect(r, Colour::rgb(238, 226, 198, 0.96f * alpha), 9);
        // the tail down to the speaker (only where the slip sits right over them)
        if (r.bottom() >= bottom - 1) {
            const Colour pc = Colour::rgb(238, 226, 198, 0.96f * alpha);
            ui_->roundRect({x - 5, r.bottom() - 2, 10, 5}, pc, 2);
            ui_->roundRect({x - 2.5f, r.bottom() + 2, 5, 4}, pc, 2);
        }
        const Colour ink = (a->player ? Colour::rgb(92, 58, 20) : kInk).withAlpha(alpha);
        float ty = r.y + 6;
        for (const auto& l : lines) {
            ui_->text(x - textW(l, px) * 0.5f, ty, l, ink, px / 15.f);
            ty += lineH;
        }
    }
}

// AL-57: the journal over the clock - what the player's person said and heard,
// what came of their doing, with the sim's time. A click on its head folds it;
// folded, the head counts what came since.
void Hud::journalBox(const Snapshot& s, float W, float H) {
    const float w = 360, x = W - 36 - w, bottom = H - 30 - 52 - 62 - 12;
    const int rows = journalOpen_ ? int(std::min<std::size_t>(s.journal.size(), 6)) : 0;
    const float headH = 28, rowH = 19;
    const float h = headH + (journalOpen_ ? std::max(1, rows) * rowH + 10 : 0);
    const Rect r{x, bottom - h, w, h};
    box(Skin::Leather, r, 14);
    ui_->claim(r);
    const Rect head{r.x, r.y, r.w, headH};
    const int newest = s.journal.empty() ? 0 : s.journal.back().seq;
    if (journalOpen_) journalSeen_ = newest;
    int fresh = 0;
    for (const auto& j : s.journal) fresh += j.seq > journalSeen_;
    std::string t = host_->text("hud.journal.title");
    if (!journalOpen_ && fresh > 0) t += "  +" + std::to_string(fresh);
    title(r.x + 14, r.y + 5, t, ui_->hovered(head) ? kGoldPale : kGold, 16);
    label(r.right() - 26, r.y + 6, journalOpen_ ? "-" : "+", kGoldPale, 15);
    if (clicked(head)) journalOpen_ = !journalOpen_;
    if (!journalOpen_) return;
    float y = r.y + headH + 2;
    if (s.journal.empty()) {
        label(r.x + 14, y, host_->text("hud.journal.empty"), kText.withAlpha(0.6f), 13);
        return;
    }
    for (std::size_t i = s.journal.size() - std::size_t(rows); i < s.journal.size(); ++i) {
        const JournalSnap& j = s.journal[i];
        label(r.x + 14, y, j.time, kText.withAlpha(0.55f), 12);
        const Colour c = j.kind == 1 ? Colour::rgb(170, 225, 120) : j.kind == 2 ? Colour::rgb(240, 120, 100)
                       : j.kind == 3 ? kGoldPale : kText;
        const float tx = r.x + 62;
        label(tx, y, ui_->fit(j.text, r.right() - 12 - tx, 13 / 15.f), c, 13);
        y += rowH;
    }
}

std::string Hud::itemIcon(const std::string& id) const {
    // AL-74: by id or tag (content/ui/craft.json); an icon the atlas lacks yet falls back to the name match
    if (auto it = itemIcons.find(id); it != itemIcons.end() && iconIndex_.count(it->second)) return it->second;
    static const std::pair<const char*, const char*> kIcons[] = {
        {"knife", "i_knife"}, {"blade", "i_knife"}, {"axe", "i_tool"}, {"spear", "i_knife"}, {"bread", "i_bread"},
        {"meal", "i_food"}, {"berries", "i_food"}, {"roots", "i_food"}, {"mushrooms", "i_food"}, {"meat", "i_food"},
        {"fish", "i_food"}, {"apples", "i_food"}, {"cheese", "i_food"}, {"vegetables", "i_food"}, {"grain", "i_food"},
        {"firewood", "i_wood"}, {"log", "i_wood"}, {"branch", "i_wood"}, {"wood", "i_wood"}, {"torch", "i_torch"},
        {"herbs", "i_herb"}, {"remedy", "i_herb"}, {"stone", "i_stone"}, {"clay", "i_stone"}, {"cloth", "i_cloth"},
        {"thread", "i_cloth"}, {"tunic", "i_cloth"}, {"cloak", "i_cloth"}, {"gambeson", "i_cloth"},
        {"trousers", "i_cloth"}, {"cap", "i_cloth"}, {"armour", "safety"}, {"mail", "safety"}, {"helmet", "safety"},
        {"belt", "things"}, {"satchel", "things"}, {"backpack", "things"}, {"basket", "things"}, {"boots", "things"},
        {"coin", "w_economy"}};
    for (const auto& [k, v] : kIcons)
        if (id.find(k) != std::string::npos) return v;
    return "i_tool";
}

// The inventory: the body on the left by its slots and layers (what is on,
// what each layer costs of the slot), the grids on the right - the hands and
// every worn bag - with each thing in its place, as many cells as it takes.
// Drag a thing to another place (R turns it); right click, or a click on a
// free layer of the body, puts it on; a click on a worn thing takes it off.
// Under it the load: kilograms of what a body can bear, and what it costs.
void Hud::inventory(const Snapshot& s, const ActorSnap* sel, float W, float H) {
    if (!sel || sel->animal || s.detail.id != sel->id) {
        panel = "none";
        return;
    }
    const GearSnap& g = s.detail.gear;
    dimmer(W, H, 0.62f);
    const float PW = std::min(1360.0f, W - 40), PH = std::min(820.0f, H - 40);
    const float ox = std::round((W - PW) * 0.5f), oy = std::round((H - PH) * 0.5f);
    box(Skin::Leather, {ox, oy, PW, PH}, 30);
    if (roundButton({ox + PW - 26, oy - 20, 48, 48}, "close", false, host_->text("hud.close")) || in_->escape) {
        panel = "none";
        dragItem_.clear();
        return;
    }
    title(ox + 34, oy + 22, host_->text("hud.gear.title", {{"name", sel->name}}), kGoldPale, 26);
    const bool mine = s.controlled == sel->id;
    const EntityId who = sel->id;
    if (!g.on) {
        label(ox + 34, oy + 70, host_->text("hud.gear.none"), kText, 15);
        return;
    }
    // ---- the body
    const Rect B{ox + 24, oy + 70, std::min(580.0f, PW * 0.44f), PH - 180};
    box(Skin::Paper, B, 18);
    title(B.x + 20, B.y + 14, host_->text("hud.gear.body"), kInk, 20);
    float y = B.y + 52;
    std::string hintTip;
    for (const auto& slot : g.slots) {
        const float rowH = 54;
        if (y + rowH > B.bottom() - 8) break;
        label(B.x + 18, y + 6, slot.name, kInk, 15);
        const std::string cost = std::to_string(slot.used) + "/" + std::to_string(slot.bulk);
        label(B.x + 18, y + 28, cost, slot.used > slot.bulk ? kInkRed : kInkSoft, 12);
        float x = B.x + 104;
        const float lw = std::min(150.0f, (B.right() - 14 - x) / std::max<std::size_t>(1, slot.layers.size()) - 6);
        for (const auto& l : slot.layers) {
            const Rect c{x, y, lw, rowH - 6};
            const bool hot = ui_->hovered(c);
            box(l.item.empty() ? Skin::Chip : Skin::ChipOn, c, 10);
            if (l.item.empty()) {
                label(c.x + 8, c.y + 15, ui_->fit(l.name, lw - 14, 12 / 15.f), kInkSoft, 12);
                // a dragged thing dropped on a free layer: put it on
                if (hot && !dragItem_.empty() && in_->released && mine) {
                    host_->command("wear", {{"id", who}, {"item", dragItem_}});
                    dragItem_.clear();
                }
            } else {
                icon(itemIcon(l.item), {c.x + 6, c.y + 6, 20, 20}, kText);
                label(c.x + 28, c.y + 6, ui_->fit(l.itemName, lw - 32, 12 / 15.f), kText, 12);
                label(c.x + 8, c.y + 28, ui_->fit(l.name, lw - 14, 11 / 15.f), kText.withAlpha(0.7f), 11);
                if (hot) hintTip = host_->text("hud.gear.take_off", {{"what", l.itemName}});
                if (hot && in_->pressed && mine && dragItem_.empty()) host_->command("take_off", {{"id", who}, {"item", l.item}});
            }
            ui_->claim(c);
            x += lw + 6;
        }
        y += rowH;
    }
    // ---- the grids
    const float cell = 44;
    const Rect G{B.right() + 20, oy + 70, ox + PW - 24 - (B.right() + 20), PH - 180};
    box(Skin::Paper, G, 18);
    float gx = G.x + 20, gy = G.y + 16, rowMax = 0;
    struct Placed {
        const GearSnap::Grid* grid;
        float x, y;
    };
    std::vector<Placed> where;
    for (const auto& gr : g.grids) {
        const float w = gr.w * cell, h = gr.h * cell + 26;
        if (gx + w > G.right() - 16) {
            gx = G.x + 20;
            gy += rowMax + 14;
            rowMax = 0;
        }
        title(gx, gy, gr.name, kInk, 16);
        where.push_back({&gr, gx, gy + 24});
        for (int j = 0; j < gr.h; ++j)
            for (int i = 0; i < gr.w; ++i)
                ui_->roundRect({gx + i * cell + 1, gy + 24 + j * cell + 1, cell - 2, cell - 2}, kInk.withAlpha(0.14f), 4);
        gx += w + 22;
        rowMax = std::max(rowMax, h);
    }
    // the things in their places
    const GearSnap::Spot* hovered = nullptr;
    for (const auto& sp : g.spots) {
        const Placed* pl = nullptr;
        for (const auto& p : where)
            if (p.grid->id == sp.grid) pl = &p;
        if (!pl) continue;
        const Rect r{pl->x + sp.x * cell + 2, pl->y + sp.y * cell + 2, sp.w * cell - 4, sp.h * cell - 4};
        const bool dragged = dragItem_ == sp.item && dragFrom_ == sp.grid;
        ui_->roundRect(r, dragged ? kLine.withAlpha(0.15f) : kRim.withAlpha(0.88f), 6);
        const float is = std::min(r.w, r.h) * 0.55f;
        icon(itemIcon(sp.item), {r.x + (r.w - is) * 0.5f, r.y + (r.h - is) * 0.5f - (sp.h > 1 ? 6 : 0), is, is},
             kText.withAlpha(dragged ? 0.4f : 1.0f));
        if (sp.w * sp.h > 1) label(r.x + 5, r.bottom() - 17, ui_->fit(sp.name, r.w - 8, 11 / 15.f), kText, 11);
        if (sp.count > 1) {
            const std::string n = std::to_string(sp.count);
            label(r.right() - 5 - textW(n, 12), r.y + 3, n, kGoldPale, 12, true);
        }
        if (ui_->hovered(r)) {
            hovered = &sp;
            if (in_->pressed && mine && dragItem_.empty()) {
                dragItem_ = sp.item;
                dragFrom_ = sp.grid;
                dragW_ = sp.turned ? sp.h : sp.w;
                dragH_ = sp.turned ? sp.w : sp.h;
                dragTurned_ = sp.turned;
            }
            if (in_->rightPressed && mine && sp.wearable) host_->command("wear", {{"id", who}, {"item", sp.item}});
        }
        ui_->claim(r);
    }
    if (hovered && dragItem_.empty()) {
        char kg[32];
        std::snprintf(kg, sizeof kg, "%.1f", hovered->kg);
        hintTip = hovered->name + (hovered->count > 1 ? " x" + std::to_string(hovered->count) : "") + "  " +
                  host_->text("hud.gear.kg", {{"kg", std::string(kg)}}) +
                  (hovered->wearable ? "  " + host_->text("hud.gear.wear_hint") : "");
    }
    // the dragged thing follows the mouse; let go over a cell: there it goes
    if (!dragItem_.empty()) {
        const int w = dragTurned_ ? dragH_ : dragW_, h = dragTurned_ ? dragW_ : dragH_;
        // the size as it was picked up is (dragW_, dragH_) in its old turn; R swaps
        const Rect ghost{in_->mouseX - cell * 0.5f, in_->mouseY - cell * 0.5f, w * cell - 4, h * cell - 4};
        ui_->roundRect(ghost, kGold.withAlpha(0.35f), 6);
        icon(itemIcon(dragItem_), {ghost.x + 6, ghost.y + 6, 26, 26}, kText);
        if (in_->released) {
            for (const auto& p : where) {
                const int cx = int(std::floor((in_->mouseX - p.x) / cell)), cy = int(std::floor((in_->mouseY - p.y) / cell));
                if (cx < 0 || cy < 0 || cx >= p.grid->w || cy >= p.grid->h) continue;
                host_->command("gear_pin", {{"id", who}, {"item", dragItem_}, {"grid", p.grid->id}, {"x", cx}, {"y", cy},
                                            {"turned", dragTurned_}});
            }
            dragItem_.clear();
        }
    }
    // ---- the load
    const Rect L{ox + 24, oy + PH - 96, PW - 48, 70};
    box(Skin::Paper, L, 16);
    char line[160];
    std::snprintf(line, sizeof line, "%.1f / %.0f", g.kg, g.maxKg);
    title(L.x + 18, L.y + 10, host_->text("hud.gear.load", {{"kg", std::string(line)}}), kInk, 18);
    const Rect bar{L.x + 18, L.y + 40, L.w * 0.5f, 14};
    ui_->roundRect(bar, kInk.withAlpha(0.18f), 7);
    const float f = float(std::clamp(g.kg / std::max(1.0, g.maxKg), 0.0, 1.0));
    const Colour fill = g.kg > g.maxKg ? kInkRed : g.kg > g.comfortKg ? kMid : kGood;
    ui_->roundRect({bar.x, bar.y, bar.w * f, bar.h}, fill, 7);
    const float cm = float(std::clamp(g.comfortKg / std::max(1.0, g.maxKg), 0.0, 1.0));
    ui_->line(bar.x + bar.w * cm, bar.y - 3, bar.x + bar.w * cm, bar.bottom() + 3, kInk);
    std::string state = host_->text("hud.gear.light");
    if (g.kg > g.maxKg) state = host_->text("hud.gear.over");
    else if (g.tireMult > 1.001) {
        char m[16];
        std::snprintf(m, sizeof m, "%.2f", g.tireMult);
        state = host_->text("hud.gear.heavy", {{"x", std::string(m)}});
    }
    label(bar.right() + 18, L.y + 38, ui_->fit(state, L.right() - bar.right() - 30, 14 / 15.f), g.kg > g.maxKg ? kInkRed : kInk, 14);
    if (!g.overflow.empty()) {
        std::string t;
        for (const auto& [n, c] : g.overflow) t += (t.empty() ? "" : ", ") + n + (c > 1 ? " x" + std::to_string(c) : "");
        label(G.x + 20, G.bottom() - 28, ui_->fit(host_->text("hud.gear.no_room", {{"what", t}}), G.w - 40, 13 / 15.f), kInkRed, 13);
    }
    if (!g.hauled.empty()) {   // AL-73: a log on the shoulder, outside the grids
        std::string t;
        for (const auto& [n, c] : g.hauled) t += (t.empty() ? "" : ", ") + n + (c > 1 ? " x" + std::to_string(c) : "");
        label(G.x + 20, G.bottom() - (g.overflow.empty() ? 28 : 48), ui_->fit(host_->text("hud.gear.hauled", {{"what", t}}), G.w - 40, 13 / 15.f), kInk, 13);
    }
    if (!mine) label(ox + 34, oy + PH - 20, host_->text("hud.gear.not_yours"), kText.withAlpha(0.8f), 13, true);
    else label(ox + 34, oy + PH - 20, host_->text("hud.gear.keys"), kText.withAlpha(0.7f), 13, true);
    if (!hintTip.empty()) {
        const float tw = textW(hintTip, 14) + 24;
        const Rect t{std::min(in_->mouseX + 16, W - tw - 8), in_->mouseY + 18, tw, 28};
        box(Skin::Leather, t, 14);
        label(t.x + 12, t.y + 5, hintTip, kText, 14);
    }
    if (in_->released && !dragItem_.empty()) dragItem_.clear();
}

// Simple signs of work: chips and dust flying off what someone works on,
// in the thing's colour (wood, stone, leaves).
void Hud::workFx(const Snapshot& s) {
    for (const auto& a : s.actors) {
        if (!a.alive || a.progress < 0 || a.actTarget == sim::kNone) continue;
        const ThingSnap* t = nullptr;
        for (const auto& x : s.things)
            if (x.id == a.actTarget) t = &x;
        if (!t) continue;
        float sx, sy;
        if (!host_->groundToScreen(t->x, t->y, 0.8, sx, sy)) continue;
        Colour c = kGoldDark;
        if (t->type == "riverbank") c = kMute;
        else if (t->type.find("bush") != std::string::npos || t->type.find("patch") != std::string::npos ||
                 t->type.find("glade") != std::string::npos || t->type == "reed_bed" || t->type == "hay_meadow")
            c = kGood;
        for (int k = 0; k < 7; ++k) {
            const double ph = std::fmod(clock_ * 1.3 + k * 0.37 + double(a.id % 7) * 0.11, 1.0);
            const double ang = double(k) * 2.4 + double(a.id);
            const float px = sx + float(std::cos(ang) * 40.0 * ph);
            const float py = sy - float(60.0 * ph - 80.0 * ph * ph);
            const float r = 2.5f + 1.5f * float(k % 2);
            ui_->roundRect({px - r, py - r, r * 2, r * 2}, c.withAlpha(float(1.0 - ph)), 1);
        }
    }
}

// ---------------------------------------------------------------- the house
void Hud::kin(const Snapshot& s, const ActorSnap* sel, float W, float H) {
    if (!sel || sel->animal) {
        for (const auto& a : s.actors)
            if (!a.animal) {
                sel = &a;
                break;
            }
        if (!sel) {
            panel = "none";
            return;
        }
    }
    dimmer(W, H, 0.6f);
    if (roundButton({W - 84, 30, 48, 48}, "close", false, host_->text("hud.close")) || in_->escape) {
        panel = "none";
        return;
    }
    std::vector<const ActorSnap*> fam;
    for (const auto& a : s.actors)
        if (!a.animal && a.family == sel->family) fam.push_back(&a);
    // generations: how many parents up stay in the house
    std::map<EntityId, int> gen;
    std::function<int(const ActorSnap*, int)> depth = [&](const ActorSnap* a, int guard) -> int {
        if (guard > 6) return 0;
        int d = 0;
        for (EntityId p : {a->mother, a->father})
            for (const ActorSnap* q : fam)
                if (q->id == p) d = std::max(d, depth(q, guard + 1) + 1);
        return d;
    };
    int maxGen = 0;
    for (const ActorSnap* a : fam) maxGen = std::max(maxGen, gen[a->id] = depth(a, 0));
    std::vector<std::vector<const ActorSnap*>> rows(std::size_t(maxGen + 1));
    for (const ActorSnap* a : fam) rows[std::size_t(gen[a->id])].push_back(a);
    for (auto& row : rows)   // spouses side by side, elders first
        std::stable_sort(row.begin(), row.end(), [](const ActorSnap* x, const ActorSnap* y) { return x->age > y->age; });
    // crest pill
    float col[3];
    host_->familyColour(sel->family, col);
    {
        const std::string t1 = host_->text("hud.kin.house", {{"name", sel->family}});
        const std::string t2 = host_->text("hud.kin.members", {{"n", int(fam.size())}});
        const float w = std::max(textW(t1, 22, true), textW(t2, 13)) + 90;
        const Rect r{36, 28, w, 56};
        box(Skin::Leather, r, 28);
        disc(r.x + 30, r.y + 28, 21, Colour(col[0], col[1], col[2]));
        arc(r.x + 30, r.y + 28, 21, 1.5f, 0, 360, kGoldPale);
        icon("dynasty", {r.x + 18, r.y + 16, 24, 24}, kGoldPale);
        title(r.x + 62, r.y + 6, t1, kText, 22);
        label(r.x + 62, r.y + 32, t2, kText.withAlpha(0.8f), 13);
    }
    // the tree
    const float left = 120, right = W - 620;
    std::map<EntityId, std::pair<float, float>> at;
    for (std::size_t g = 0; g < rows.size(); ++g) {
        const auto& row = rows[g];
        const float y = 200 + g * std::min(260.0f, (H - 300) / std::max<float>(1, float(rows.size())));
        for (std::size_t i = 0; i < row.size(); ++i) {
            const float x = left + (right - left) * (i + 0.5f) / float(row.size());
            at[row[i]->id] = {x, y};
        }
    }
    // lines: parent to child as elbows, spouses joined
    auto seg = [&](float x0, float y0, float x1, float y1) {
        ui_->rect({std::min(x0, x1) - 3.5f, std::min(y0, y1) - 3.5f, std::fabs(x1 - x0) + 7, std::fabs(y1 - y0) + 7}, kRim);
        const float len = std::max(std::fabs(x1 - x0), std::fabs(y1 - y0));
        for (float t = 0; t < len; t += 11) {
            const float u = t / std::max(1.f, len);
            disc(x0 + (x1 - x0) * u, y0 + (y1 - y0) * u, 1.5f, kGoldPale);
        }
    };
    for (const ActorSnap* a : fam) {
        const auto me = at[a->id];
        for (EntityId p : {a->mother, a->father}) {
            const auto it = at.find(p);
            if (it == at.end()) continue;
            const float midY = (it->second.second + me.second) * 0.5f;
            seg(it->second.first, it->second.second + 44, it->second.first, midY);
            seg(it->second.first, midY, me.first, midY);
            seg(me.first, midY, me.first, me.second - 44);
            break;
        }
        const auto sp = at.find(a->spouse);
        if (sp != at.end() && a->id < a->spouse) seg(me.first + 44, me.second, sp->second.first - 44, sp->second.second);
    }
    if (kinPick_ == sim::kNone || !actor(s, kinPick_)) kinPick_ = sel->id;
    for (const ActorSnap* a : fam) {
        const auto [x, y] = at[a->id];
        const Colour rc = a->player ? kGoldPale : !a->alive ? kInkSoft : kMute;
        portrait(*a, x, y, 44);
        arc(x, y, 45, 2, 0, 360, rc);
        arc(x, y, 47.5f, 3, 0, 360, kGoldDark);
        if (a->id == kinPick_) arc(x, y, 52, 3, 0, 360, kGoldPale);
        const std::string ctl = host_->text(a->player ? "hud.kin.you" : !a->alive ? (a->sex == 'f' ? "hud.kin.dead_f" : "hud.kin.dead")
                                                                                  : (a->sex == 'f' ? "hud.kin.self_f" : "hud.kin.self"));
        const float w1 = textW(a->name, 18, true), w2 = textW(ctl, 13);
        ui_->text(x - w1 / 2, y + 54 + 2, a->name, kRim, 18 / 15.f, true);
        ui_->text(x - w1 / 2, y + 54, a->name, kText, 18 / 15.f, true);
        label(x - w2 / 2, y + 78, ctl, rc, 13, true);
        if (clicked({x - 44, y - 44, 88, 88})) kinPick_ = a->id;
    }
    // the paper on the right
    const ActorSnap* k = actor(s, kinPick_);
    if (!k) return;
    const Rect P{W - 560, (H - 520) * 0.5f, 500, 520};
    box(Skin::Paper, P, 18);
    seal(P.right() - 37, P.y + 5);
    title(P.x + 28, P.y + 26, k->name, kInk, 30);
    label(P.x + 28, P.y + 68, k->family + " · " + host_->text("hud.kin.age", {{"n", k->age}}), kInkSoft, 15);
    std::string doing = host_->text(k->alive ? "game.fsm." + k->state : "game.dead");
    if (!k->act.empty()) doing += ": " + k->act;
    label(P.x + 28, P.y + 98, host_->text("hud.kin.now") + ": " + doing, kInk, 15);
    const std::string ctl = host_->text(k->player ? "hud.kin.you" : !k->alive ? (k->sex == 'f' ? "hud.kin.dead_f" : "hud.kin.dead")
                                                                              : (k->sex == 'f' ? "hud.kin.self_f" : "hud.kin.self"));
    label(P.x + 28, P.y + 124, host_->text("hud.kin.led") + ": " + ctl, k->player ? kGoldDark : kInk, 15);
    float x = P.x + 28;
    if (k->alive && !k->player) {
        const std::string t = host_->text("hud.kin.lead");
        const float w = textW(t, 15) + 48;
        if (textButton({x, P.y + 160, w, 48}, t, true)) host_->command("take_control", {{"id", k->id}});
        x += w + 10;
    } else if (k->player) {
        const std::string t = host_->text("hud.kin.release");
        const float w = textW(t, 15) + 48;
        if (textButton({x, P.y + 160, w, 48}, t, true)) host_->command("release_control", {{"id", k->id}});
        x += w + 10;
    }
    {
        const std::string t = host_->text("hud.kin.open_sheet");
        const float w = textW(t, 15) + 40;
        if (textButton({x, P.y + 160, w, 48}, t, false)) {
            host_->select(k->id);
            panel = "sheet";
        }
    }
    ui_->paragraph(P.x + 28, P.y + 222, P.w - 56, host_->text("hud.kin.note"), kInkSoft, 13 / 15.f);
    title(P.x + 28, P.y + 270, host_->text("hud.kin.legacy"), kInk, 22);
    ui_->line(P.x + 28, P.y + 302, P.right() - 28, P.y + 302, kLine.withAlpha(0.45f));
    float y = P.y + 316;
    for (const ActorSnap* a : fam) {
        if (y > P.bottom() - 30) break;
        label(P.x + 28, y, a->name, kInk, 15);
        const std::string st = host_->text(a->alive ? "game.fsm." + a->state : (a->sex == 'f' ? "game.dead_f" : "game.dead"));
        label(P.x + 200, y, ui_->fit(st, P.w - 228, 14 / 15.f), kInkSoft, 14);
        y += 26;
    }
}

// ---------------------------------------------------------------- the annals
void Hud::annals(const Snapshot& s, float W, float H) {
    dimmer(W, H, 0.62f);
    const float ox = std::round((W - 1300) * 0.5f), oy = std::round((H - 780) * 0.5f);
    box(Skin::Leather, {ox, oy, 1300, 780}, 30);
    if (roundButton({ox + 1274, oy - 20, 48, 48}, "close", false, host_->text("hud.close")) || in_->escape) {
        panel = "none";
        return;
    }
    {   // the switch
        const std::string a = host_->text("hud.annals.house"), b = host_->text("hud.annals.world");
        const float wa = textW(a, 14) + 36, wb = textW(b, 14) + 36;
        const Rect r{W * 0.5f - (wa + wb + 12) * 0.5f, oy - 26, wa + wb + 12, 52};
        box(Skin::Leather, r, 26);
        const Rect ra{r.x + 4, r.y + 4, wa, 44}, rb{r.x + 8 + wa, r.y + 4, wb, 44};
        if (annalsTab_ == 0) box(Skin::ButtonOn, ra, 22);
        else box(Skin::ButtonOn, rb, 22);
        label(ra.x + 18, ra.y + 13, a, annalsTab_ == 0 ? kDark : kText, 14);
        label(rb.x + 18, rb.y + 13, b, annalsTab_ == 1 ? kDark : kText, 14);
        if (clicked(ra)) annalsTab_ = 0;
        if (clicked(rb)) annalsTab_ = 1;
    }
    const Rect P{ox + 30, oy + 40, 1240, 710};
    box(Skin::Paper, P, 18);
    if (annalsTab_ == 1) {
        title(P.x + 38, P.y + 30, host_->text("hud.annals.world_title"), kInk, 32);
        ui_->line(P.x + 38, P.y + 74, P.right() - 38, P.y + 74, kLine.withAlpha(0.45f));
        ui_->paragraph(P.x + 38, P.y + 100, P.w - 76, host_->text("hud.annals.world_soon"), kInkSoft, 1.0f);
        return;
    }
    title(P.x + 38, P.y + 30, host_->text("hud.annals.title", {{"name", s.settlement}}), kInk, 32);
    static const char* kFilters[] = {"all", "family", "people", "power", "signs"};
    float fx = P.right() - 38;
    for (int i = 4; i >= 0; --i) {
        const std::string t = host_->text(std::string("hud.annals.") + kFilters[i]);
        const float w = textW(t, 14) + 28;
        fx -= w;
        const Rect b{fx, P.y + 30, w, 40};
        box(i == annalsFilter_ ? Skin::ChipOn : Skin::Chip, b, 20);
        label(b.x + 14, b.y + 11, t, i == annalsFilter_ ? kText : kInk, 14);
        if (clicked(b)) annalsFilter_ = i;
        fx -= 8;
    }
    ui_->line(P.x + 38, P.y + 82, P.right() - 38, P.y + 82, kLine.withAlpha(0.45f));
    auto kindOf = [](const std::string& k) {
        if (k.find("birth") != std::string::npos || k.find("death") != std::string::npos || k.find("marr") != std::string::npos ||
            k.find("family") != std::string::npos || k.find("year") != std::string::npos)
            return 1;
        if (k.find("law") != std::string::npos || k.find("council") != std::string::npos || k.find("office") != std::string::npos ||
            k.find("power") != std::string::npos || k.find("exile") != std::string::npos)
            return 3;
        if (k.find("faith") != std::string::npos || k.find("rite") != std::string::npos || k.find("sign") != std::string::npos ||
            k.find("omen") != std::string::npos)
            return 4;
        return 2;
    };
    static const std::uint32_t kKindColours[] = {0x2f6088, 0x2c6b4a, 0x8e3a2e, 0x2f6088, 0x57407f};
    float col = 0, y = P.y + 100;
    int shown = 0;
    for (auto it = s.chronicle.rbegin(); it != s.chronicle.rend(); ++it) {
        const int kind = kindOf(it->kind);
        if (annalsFilter_ && kind != annalsFilter_) continue;
        const float x = P.x + 38 + col * 600;
        const std::uint32_t c = kKindColours[kind];
        const Colour cc = Colour::rgb(int(c >> 16), int((c >> 8) & 255), int(c & 255));
        disc(x + 29, y + 29, 29, cc);
        arc(x + 29, y + 29, 29, 1.5f, 0, 360, kGoldDark);
        const ActorSnap* who = it->who.empty() ? nullptr : actor(s, it->who.front());
        if (who && !who->animal) portrait(*who, x + 29, y + 29, 26);
        else {
            const std::string ini = it->text.substr(0, it->text.size() && (unsigned char)it->text[0] >= 0xC0 ? 2 : 1);
            title(x + 29 - textW(ini, 30, true) / 2, y + 10, ini, kHot, 30);
        }
        label(x + 78, y, host_->text("hud.annals.day", {{"day", it->day}}) + " · " + it->time, kInkSoft, 13);
        const float h = ui_->paragraph(x + 78, y + 20, 500, it->text, kInk, 18 / 15.f);
        y += std::max(78.f, h + 40);
        ++shown;
        if (y > P.bottom() - 90) {
            if (col > 0) break;
            col = 1;
            y = P.y + 100;
        }
    }
    if (!shown) label(P.x + 38, P.y + 110, host_->text("hud.annals.empty"), kInkSoft, 15);
}


// The build mode (sim/building.hpp): the palette of modules on a leather bar,
// the state of the piece under the mouse and of the household's plan. The
// world takes the clicks; the game draws the preview.
bool Hud::buildFits(const std::string& mode, const BuildEntry& e) {
    const bool door = e.id.rfind("wall_door", 0) == 0, window = e.id.rfind("wall_window", 0) == 0;
    if (mode == "room" || mode == "wall") return e.category == "wall" && !door && !window;
    if (mode == "door") return door;
    if (mode == "window") return window;
    if (mode == "remove") return false;
    return e.category == mode;
}

// AL-10: the build catalog, Sims-like: a row of tools on top, the pieces of
// the tool as cards with their picture, name and cost, and on the right the
// piece picked, what the mouse is over, the plan and the buttons for turning
// and storeys. Ink on paper, so it reads in any light.
void Hud::buildBar(const Snapshot& s, float W, float H) {
    (void)s;
    if (buildCatalog.empty()) return;
    static const char* kModes[] = {"room", "wall", "door", "window", "floor", "foundation", "roof", "gable", "pillar", "remove"};
    const float pw = std::min(W - 60, 1320.0f), ph = 232;
    const Rect P{(W - pw) * 0.5f, H - 24 - ph, pw, ph};
    box(Skin::Paper, P, 18);
    ui_->claim(P);
    // the tools
    const int nm = int(sizeof kModes / sizeof *kModes);
    const float infoW = std::min(330.0f, pw * 0.3f);
    const float tabsW = pw - infoW - 36;
    const float tw = std::min(118.0f, (tabsW - (nm - 1) * 5) / nm);
    for (int i = 0; i < nm; ++i) {
        const Rect t{P.x + 14 + i * (tw + 5), P.y + 12, tw, 32};
        const bool on = buildMode == kModes[i];
        if (textButton(t, ui_->fit(host_->text(std::string("hud.build.mode.") + kModes[i]), tw - 12, 1.0f), on)) {
            buildMode = kModes[i];
            if (buildPick < 0 || buildPick >= int(buildCatalog.size()) ||
                !buildFits(buildMode, buildCatalog[std::size_t(buildPick)]))
                for (std::size_t k = 0; k < buildCatalog.size(); ++k)
                    if (buildFits(buildMode, buildCatalog[k])) {
                        buildPick = int(k);
                        break;
                    }
        }
    }
    // the cards of the tool
    const float cw = 118, chh = 160, top = P.y + 54;
    float x = P.x + 14;
    int shown = 0;
    for (std::size_t k = 0; k < buildCatalog.size(); ++k) {
        const BuildEntry& e = buildCatalog[k];
        if (!buildFits(buildMode, e)) continue;
        if (x + cw > P.x + 14 + tabsW) break;
        const Rect card{x, top, cw, chh};
        const bool on = int(k) == buildPick;
        ui_->roundRect(card, on ? kGold.withAlpha(0.55f) : kLine.withAlpha(ui_->hovered(card) ? 0.16f : 0.08f), 12);
        if (on) ui_->roundRect({card.x + 3, card.y + 3, card.w - 6, card.h - 6}, kHot.withAlpha(0.55f), 10);
        const auto it = previewIndex_.find(e.id);
        if (previews_ && it != previewIndex_.end()) {
            const SDL_FRect src{float(it->second * previewCell_), 0, float(previewCell_), float(previewCell_)};
            const SDL_FRect dst{std::round(card.x + 9), std::round(card.y + 6), 100, 100};
            SDL_SetTextureColorModFloat(previews_, 1, 1, 1);
            SDL_SetTextureAlphaModFloat(previews_, 1);
            SDL_RenderTexture(sdl_, previews_, &src, &dst);
        }
        const std::string nameFit = ui_->fit(e.name, cw - 12, 14 / 15.f);
        label(card.x + (cw - textW(nameFit, 14)) * 0.5f, card.y + 110, nameFit, kInk, 14);
        const std::string costFit = ui_->fit(e.cost, cw - 12, 12 / 15.f);
        label(card.x + (cw - textW(costFit, 12)) * 0.5f, card.y + 132, costFit, kInkSoft, 12);
        if (clicked(card)) buildPick = int(k);
        x += cw + 8;
        ++shown;
    }
    if (shown == 0) label(P.x + 20, top + 10, host_->text("hud.build.tip." + buildMode), kInkSoft, 15);
    // the piece, the cursor, the plan, the buttons
    const Rect I{P.right() - infoW - 14, P.y + 12, infoW, ph - 24};
    ui_->roundRect(I, kLine.withAlpha(0.10f), 12);
    float y = I.y + 10;
    const BuildEntry* pick =
        buildPick >= 0 && buildPick < int(buildCatalog.size()) ? &buildCatalog[std::size_t(buildPick)] : nullptr;
    std::string head = pick ? pick->name : "";
    if (buildMode == "remove") head = host_->text("hud.build.mode.remove");
    else if (buildMode == "room" && pick) head = host_->text("hud.build.mode.room") + ": " + pick->name;
    title(I.x + 14, y, ui_->fit(head, I.w - 28, 20 / 15.f, true), kInk, 20);
    y += 30;
    if (pick && buildMode != "remove") {
        label(I.x + 14, y, ui_->fit(pick->cost, I.w - 28, 14 / 15.f), kInkSoft, 14);
        y += 22;
    }
    label(I.x + 14, y, ui_->fit(host_->text("hud.build.tip." + buildMode), I.w - 28, 13 / 15.f), kInkSoft, 13);
    y += 22;
    if (!buildStatus.empty()) {
        label(I.x + 14, y, ui_->fit(buildStatus, I.w - 28, 14 / 15.f), buildOk ? kGood : kInkRed, 14);
        y += 22;
    }
    if (!buildPlan.empty()) label(I.x + 14, y, ui_->fit(buildPlan, I.w - 28, 13 / 15.f), kInk, 13);
    const float bw = (I.w - 28 - 12) / 3;
    const float by = I.bottom() - 40;
    if (textButton({I.x + 14, by, bw, 30}, ui_->fit(host_->text("hud.build.turn"), bw - 10, 1.0f), false)) ++buildTurns;
    if (textButton({I.x + 20 + bw, by, bw, 30}, ui_->fit(host_->text("hud.build.storey_up"), bw - 10, 1.0f), false))
        ++buildStoreyUp;
    if (textButton({I.x + 26 + 2 * bw, by, bw, 30}, ui_->fit(host_->text("hud.build.storey_down"), bw - 10, 1.0f), false))
        ++buildStoreyDown;
    if (roundButton({W - 84, 30, 48, 48}, "close", false, host_->text("hud.close")) || in_->escape) panel = "none";
}

// The colony (GDD §2.2): its institution, the chief who holds the mandate,
// who judges now, and the rights of every grown person. With the mandate the
// player hands rights out and takes them back; the sim judges by them at once.
void Hud::colony(const Snapshot& s, float W, float H) {
    dimmer(W, H, 0.6f);
    if (roundButton({W - 84, 30, 48, 48}, "close", false, host_->text("hud.close")) || in_->escape) {
        panel = "none";
        return;
    }
    const ColonySnap& c = s.colony;
    const float pw = std::min(W - 120, 1100.0f), ph = std::min(H - 120, 760.0f);
    const Rect P{(W - pw) * 0.5f, (H - ph) * 0.5f, pw, ph};
    box(Skin::Paper, P, 22);
    title(P.x + 32, P.y + 22, host_->text("hud.colony.title", {{"name", s.settlement}}), kInk, 30);
    float y = P.y + 70;
    label(P.x + 32, y, host_->text("hud.colony.institution." + (c.institution.empty() ? std::string("none") : c.institution)), kInkSoft, 15);
    y += 26;
    label(P.x + 32, y,
          c.chief == sim::kNone ? host_->text("hud.colony.no_chief")
                                : host_->text("hud.colony.chief", {{"name", c.chiefName}, {"family", c.chiefFamily}}),
          kInk, 16);
    y += 26;
    if (c.judge != sim::kNone)
        label(P.x + 32, y,
              host_->text(c.judgeByRight ? "hud.colony.judge_right" : "hud.colony.judge_custom", {{"name", c.judgeName}}),
              kInk, 16);
    y += 30;
    seal(P.right() - 60, P.y + 52);
    label(P.x + 32, y, host_->text(c.mandate ? "hud.colony.mandate_yes" : "hud.colony.mandate_no", {{"family", c.chiefFamily}}),
          c.mandate ? kGood : kInkRed, 15);
    y += 36;
    // the table: name, house, age, the rights
    const float colName = P.x + 32, colFam = P.x + 300, colAge = P.x + 470, colR = P.x + 530;
    const float chipW = std::min(130.0f, (P.right() - 32 - colR) / std::max<float>(1, float(c.rights.size())) - 8);
    label(colName, y, host_->text("hud.colony.col.name"), kInkSoft, 13);
    label(colFam, y, host_->text("hud.colony.col.house"), kInkSoft, 13);
    label(colAge, y, host_->text("hud.colony.col.age"), kInkSoft, 13);
    for (std::size_t k = 0; k < c.rights.size(); ++k)
        label(colR + k * (chipW + 8) + 8, y, host_->text("hud.right." + c.rights[k]), kInkSoft, 13);
    y += 24;
    const float rowH = 34;
    const int rows = int((P.bottom() - 60 - y) / rowH);
    int shown = 0;
    for (const ColonyMember& m : c.members) {
        if (shown >= rows) break;
        ++shown;
        if (shown % 2 == 0) ui_->roundRect({P.x + 20, y - 4, pw - 40, rowH}, kLine.withAlpha(0.07f), 8);
        label(colName, y + 4, ui_->fit(m.name, colFam - colName - 12, 15 / 15.f) + (m.chief ? "  \xE2\x99\x9B" : ""),
              m.chief ? kInkRed : kInk, 15);
        label(colFam, y + 4, ui_->fit(m.family, colAge - colFam - 12, 15 / 15.f), kInk, 15);
        label(colAge, y + 4, std::to_string(m.age), kInk, 15);
        for (std::size_t k = 0; k < c.rights.size(); ++k) {
            const std::string& r = c.rights[k];
            const bool has = std::find(m.rights.begin(), m.rights.end(), r) != m.rights.end();
            const Rect b{colR + k * (chipW + 8), y, chipW, 26};
            const std::string t = host_->text(has ? "hud.colony.has" : "hud.colony.not");
            if (c.mandate && !m.chief) {
                if (textButton(b, t, has))
                    host_->command(has ? "revoke_right" : "grant_right", {{"id", c.actor}, {"to", m.id}, {"right", r}});
            } else {
                box(has ? Skin::ChipOn : Skin::Chip, b, 13);
                const float tw = textW(t, 13);
                label(b.x + (b.w - tw) * 0.5f, b.y + 4, t, has ? kText : kInkSoft, 13);
            }
        }
        y += rowH;
    }
    if (shown < int(c.members.size()))
        label(colName, y + 4, host_->text("hud.colony.more", {{"n", int(c.members.size()) - shown}}), kInkSoft, 13);
    label(P.x + 32, P.bottom() - 40, ui_->fit(host_->text("hud.colony.note"), pw - 64, 13 / 15.f), kInkSoft, 13);
}

// ---------------------------------------------------------------- the workshop
// Boards 10-15 of the design canvas (style G; gdd/ui/CRAFT_UI_VARIANTS.md,
// the decision of 20.11): the work happens at the stations in the world (C),
// the processes lie on a ring round the work piece (B), the heat is set on the
// hearth's gauge by the mouse wheel, materials are dragged from the bag onto a
// station or the piece (A), and the novice helper answers a wish (D). No text
// buttons: icons, their words in the tooltip; "make" is held (E, or the mouse
// on the piece). Data: CraftSnap (sim_game.cpp), icons: content/ui/craft.json.
namespace {
constexpr float kHoldSeconds = 0.8f;
const Colour kHeat = kMid;
const Colour kScene = kRim.withAlpha(0.72f);
} // namespace

std::string Hud::craftIconOf(const char* map, const std::string& id, const char* fallback) const {
    if (craftSpec_.is_object() && craftSpec_.contains(map) && craftSpec_[map].contains(id))
        return craftSpec_[map][id].get<std::string>();
    return fallback;
}

// The tooltip of what is under the mouse: drawn last, over everything.
// The tooltip asked for this frame (tipAt), over everything: glass with a gold frame.
void Hud::drawTip(float W, float H) {
    if (tipText_.empty()) return;
    const float w = std::min(textW(tipText_, 14) + 26, 460.f);
    const float x = std::clamp(tipX_ - w * 0.5f, 8.f, W - w - 8);
    const float yy = std::clamp(tipY_, 8.f, H - 40);
    const Rect t{x, yy, w, 32};
    box(Skin::Paper, t, 3);
    label(t.x + 13, t.y + 7, ui_->fit(tipText_, w - 26, 14 / 15.f), kInk, 14);
    tipText_.clear();
}

void Hud::tipAt(float cx, float y, const std::string& tip) {
    tipText_ = tip;
    tipX_ = cx;
    tipY_ = y;
}

bool Hud::iconButton(const Rect& r, const std::string& iconName, bool on, const std::string& tip, bool dim) {
    if (!hudPalette().theme.flag("textured"))   // AL-66: a diamond in the same box
        return diamondButton(r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.w * 0.74f, iconName, on, tip, dim);
    const bool hot = ui_->hovered(r);
    const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    box(on ? Skin::ButtonOn : Skin::Button, r, r.h * 0.5f);
    if (hot && !on) disc(cx, cy, r.w * 0.5f - 1, kGold.withAlpha(0.16f));
    if (dim) disc(cx, cy, r.w * 0.5f - 1, kRim.withAlpha(0.5f));
    const float s = std::round(r.w * 0.48f);
    Colour c = on ? kDark : hot ? kHot : kText;
    if (dim) c = c.withAlpha(0.45f);
    icon(iconName, {cx - s * 0.5f, cy - s * 0.5f, s, s}, c);
    if (hot) tipAt(cx, r.bottom() + 10, tip);
    return clicked(r);
}

sim::Json Hud::craftDraftJson(const Snapshot& s) const {
    sim::Json d = sim::Json::object();
    const CraftSnap& c = s.craft;
    if (!craftWish.empty()) d["wish"] = craftWish;
    if (craftProc < 0 || craftProc >= int(c.processes.size())) return d;
    d["process"] = c.processes[size_t(craftProc)].id;
    sim::Json slots = sim::Json::object();
    for (const auto& [slot, refs] : craftSlots) slots[slot] = refs;
    d["slots"] = slots;
    if (!craftShape.empty()) d["shape"] = craftShape;
    if (!craftMode.empty()) d["mode"] = craftMode;
    if (craftHeat >= 0) d["heat"] = craftHeat;
    if (craftLen > 0) d["len"] = craftLen;
    return d;
}

void Hud::craftAttempt(const Snapshot& s) {
    const CraftSnap& c = s.craft;
    if (craftProc < 0 || craftProc >= int(c.processes.size()) || !c.canStart) return;
    sim::Json a = craftDraftJson(s);
    a.erase("wish");
    a["id"] = c.actor;
    lastDraft_ = a;
    host_->command("craft_attempt", a);
    craftSlots.clear();
    craftShape.clear();
    const auto& pr = c.processes[size_t(craftProc)];
    craftSlot = pr.slots.empty() ? "" : pr.slots.front().first;
}

void Hud::workshop(const Snapshot& s, float W, float H) {
    craftDt_ = float(std::clamp(clock_ - craftClock_, 0.0, 0.1));
    craftClock_ = clock_;
    tipText_.clear();
    const CraftSnap& c = s.craft;
    if (resultShown_ < 0) resultShown_ = c.result.seq;   // what happened before the window opened is old news
    if (c.result.seq != resultShown_) {
        resultShown_ = c.result.seq;
        resultOpen_ = !c.result.code.empty();
        renameText_ = c.result.recipeName;
    }
    dimmer(W, H, craftTab == 0 && !resultOpen_ ? 0.22f : 0.62f);

    // the title plate and the tabs (boards 10-15: the same row of icons)
    const ActorSnap* who = actor(s, c.actor);
    {
        const std::string t = host_->text("hud.craft.title", {{"name", who ? who->name : std::string()}});
        const float w = textW(t, 22, true) + 40;
        box(Skin::ButtonOn, {36, 30, w, 46}, 23);
        title(56, 40, t, kDark, 22);
    }
    static const char* kTabIcons[] = {"c_anvil", "a_book", "things", "c_repair"};
    static const char* kTabTips[] = {"hud.craft.tab.bench", "hud.craft.tab.book", "hud.craft.tab.things", "hud.craft.tab.repair"};
    const float tx = W - 36 - 6 * 62 + 10;
    for (int i = 0; i < 4; ++i)
        if (iconButton({tx + i * 62.f, 28, 52, 52}, kTabIcons[i], craftTab == i, host_->text(kTabTips[i]))) {
            craftTab = i;
            resultOpen_ = false;
        }
    if (iconButton({tx + 4 * 62.f, 28, 52, 52}, "a_hint", craftHelper, host_->text("hud.craft.helper"))) {
        craftHelper = !craftHelper;
        craftTab = 0;
        if (!craftHelper) craftWish.clear();
    }
    if (resultOpen_ && in_->escape && !ui_->typing()) {
        resultOpen_ = false;
        in_->escape = false;
    }
    if (iconButton({tx + 5 * 62.f, 28, 52, 52}, "close", false, host_->text("hud.close")) || (in_->escape && !ui_->typing())) {
        panel = "none";
        dragRef_.clear();
        resultOpen_ = false;
        return;
    }
    if (c.actor == sim::kNone) {
        box(Skin::Paper, {W * 0.5f - 200, H * 0.5f - 30, 400, 60}, 20);
        const float w = textW(host_->text("hud.craft.nobody"), 16);
        label(W * 0.5f - w * 0.5f, H * 0.5f - 10, host_->text("hud.craft.nobody"), kInkRed, 16);
        return;
    }

    if (resultOpen_) craftResultCard(s, W, H);
    else if (craftTab == 0) {
        craftBench(s, W, H);
        if (craftHelper) craftHelperView(s, W, H);
    } else if (craftTab == 1) craftBook(s, W, H);
    else if (craftTab == 2) craftThings(s, W, H);
    else craftRepair(s, W, H);

    // the draft goes to the sim for its forecast when it changed
    {
        const sim::Json d = craftDraftJson(s);
        const std::string ds = d.dump();
        if (ds != sentDraft_) {
            sentDraft_ = ds;
            host_->craftDraft(d);
        }
    }
    // a carried material follows the mouse; let go anywhere else, it goes back
    if (!dragRef_.empty()) {
        std::string ic = "s_lump";
        for (const auto& m : c.materials)
            if (m.ref == dragRef_) ic = m.icon;
        for (const auto& m : c.made)
            if (m.ref == dragRef_) ic = m.icon;
        disc(in_->mouseX, in_->mouseY, 26, kRim.withAlpha(0.85f));
        arc(in_->mouseX, in_->mouseY, 26, 2.5f, 0, 360, kGoldPale);
        icon(ic, {in_->mouseX - 14, in_->mouseY - 14, 28, 28}, kGoldPale);
        if (in_->released) dragRef_.clear();
    }
    drawTip(W, H);
}

// Board 10: the bench in the world.
void Hud::craftBench(const Snapshot& s, float W, float H) {
    const CraftSnap& c = s.craft;
    const float px = W * 0.5f, py = H * 0.46f;
    const float R = std::clamp(H * 0.19f, 120.f, 170.f);
    const bool hasProc = craftProc >= 0 && craftProc < int(c.processes.size());
    const CraftProcessSnap* pr = hasProc ? &c.processes[size_t(craftProc)] : nullptr;

    auto pickProcess = [&](int i) {
        if (craftProc == i) return;
        craftProc = i;
        craftSlots.clear();
        craftShape.clear();
        craftMode.clear();
        craftLen = 0;
        const auto& p = c.processes[size_t(i)];
        craftSlot = p.slots.empty() ? "" : p.slots.front().first;
    };
    auto addToSlot = [&](const std::string& ref, const std::string& slotWanted) {
        if (craftProc < 0 || craftProc >= int(c.processes.size())) return;
        const auto& p = c.processes[size_t(craftProc)];
        std::string slot = slotWanted.empty() ? craftSlot : slotWanted;
        if (slot.empty() && !p.slots.empty()) slot = p.slots.front().first;
        auto& v = craftSlots[slot];
        if (ref.rfind("made:", 0) == 0 && std::find(v.begin(), v.end(), ref) != v.end()) return;
        v.push_back(ref);
        craftSlot = slot;
        for (const auto& [sl, nm] : p.slots)
            if (craftSlots[sl].empty()) {
                craftSlot = sl;
                break;
            }
    };
    auto thingOf = [&](const std::string& ref) -> const CraftThing* {
        for (const auto& m : c.materials)
            if (m.ref == ref) return &m;
        for (const auto& m : c.made)
            if (m.ref == ref) return &m;
        return nullptr;
    };
    const bool dropping = !dragRef_.empty() && in_->released;

    // ---- the stations: over their objects when the camera sees them, else
    // round the bench (heat on the left as on board 10, the rest on the right)
    stationPos_.clear();
    std::vector<std::pair<float, float>> used;
    int left = 0, right = 0;
    for (const auto& st : c.stationsAt) {
        float sx = 0, sy = 0;
        bool ok = st.placed && host_->groundToScreen(st.x, st.y, 1.4, sx, sy);
        if (ok && std::hypot(sx - px, sy - py) < R + 90) ok = false;
        if (ok && (sy < 120 || sy > H - 190 || sx < 80 || sx > W - 80)) ok = false;
        if (!ok) {
            if (st.tempMax > 0) {
                sx = px - R - 230 - float(left / 3) * 120;
                sy = py - 110 + float(left % 3) * 125;
                ++left;
            } else {
                sx = px + R + 210 + float(right / 3) * 120;
                sy = py - 110 + float(right % 3) * 125;
                ++right;
            }
        }
        for (int guard = 0; guard < 6; ++guard) {
            bool clash = false;
            for (const auto& [ux, uy] : used)
                if (std::hypot(ux - sx, uy - sy) < 110) clash = true;
            if (!clash) break;
            sy += 115;
        }
        sx = std::clamp(sx, 70.f, W - 70);
        sy = std::clamp(sy, 140.f, H - 200);
        used.push_back({sx, sy});
        stationPos_[st.id] = {sx, sy};
    }
    for (const auto& st : c.stationsAt) {
        const auto [sx, sy] = stationPos_[st.id];
        const Rect hit{sx - 52, sy - 52, 104, 104};
        const bool hot = ui_->hovered(hit);
        ui_->claim(hit);
        const bool usedNow = pr && std::find(pr->stations.begin(), pr->stations.end(), st.id) != pr->stations.end();
        disc(sx, sy, 40, kScene);
        arc(sx, sy, 40, 2, 0, 360, usedNow || hot ? kGoldPale : kGoldPale.withAlpha(0.45f));
        if (!dragRef_.empty()) arc(sx, sy, 46, 3, 0, 360, kGold.withAlpha(0.5f + 0.3f * std::sin(float(clock_) * 6)));
        const std::string ic = craftIconOf("station_icons", st.id, "c_bench");
        icon(ic, {sx - 20, sy - 20, 40, 40}, st.tempMax > 0 ? kRingBad : kGoldPale);
        std::string tip = host_->text("hud.craft.station_tip", {{"name", st.name}});
        if (st.tempMax > 0) {
            // the heat gauge (manometer) over the hearth: the mouse wheel turns it
            const int heat = craftHeat < 0 ? st.tempMax : std::min(craftHeat, st.tempMax);
            arc(sx, sy, 54, 9, 135, 270, kRim.withAlpha(0.45f));
            arc(sx, sy, 54, 9, 135, 270.f * float(heat) / float(std::max(1, st.tempMax)), kHeat);
            for (int k = 1; k < st.tempMax; ++k) {
                const float a = (135.f + 270.f * float(k) / float(st.tempMax)) * kPiF / 180.f;
                ui_->line(sx + std::cos(a) * 49, sy + std::sin(a) * 49, sx + std::cos(a) * 59, sy + std::sin(a) * 59,
                          kRim);
            }
            tip = host_->text("hud.craft.heat_tip", {{"name", st.name}, {"n", heat}, {"max", st.tempMax}});
            if (hot && in_->wheel != 0) {
                craftHeat = std::clamp(heat + (in_->wheel > 0 ? 1 : -1), 0, st.tempMax);
                in_->wheel = 0;
            }
        }
        if (hot) tipAt(sx, sy + 64, tip);
        // a material let go over a station: the station's work takes it
        if (hot && (dropping || clicked(hit))) {
            int best = -1;
            if (pr && usedNow) best = craftProc;
            for (size_t i = 0; i < c.processes.size() && best < 0; ++i) {
                const auto& p = c.processes[i];
                if (p.available && std::find(p.stations.begin(), p.stations.end(), st.id) != p.stations.end()) best = int(i);
            }
            if (best >= 0) {
                pickProcess(best);
                if (dropping) addToSlot(dragRef_, "");
            }
            if (dropping) dragRef_.clear();
        }
    }

    // ---- the ring of processes round the piece (board 10, variant B)
    std::vector<int> order;
    for (size_t i = 0; i < c.processes.size(); ++i)
        if (c.processes[i].available && c.processes[i].id != "repair") order.push_back(int(i));
    for (size_t i = 0; i < c.processes.size(); ++i)
        if (!c.processes[i].available && c.processes[i].id != "repair") order.push_back(int(i));
    const int cap = std::max(6, int(2 * kPiF * R / 62));
    if (int(order.size()) > cap) order.resize(size_t(cap));
    if (hasProc && std::find(order.begin(), order.end(), craftProc) == order.end()) order.back() = craftProc;
    disc(px, py, R + 34, kRim.withAlpha(0.30f));
    arc(px, py, R + 34, 2, 0, 360, kGoldPale.withAlpha(0.3f));
    for (size_t k = 0; k < order.size(); ++k) {
        const auto& p = c.processes[size_t(order[k])];
        const float a = (-90.f + 360.f * float(k) / float(order.size())) * kPiF / 180.f;
        const float bx = px + std::cos(a) * R, by = py + std::sin(a) * R;
        std::string tip = host_->text("hud.craft.proc_tip", {{"name", p.name}, {"time", p.time}});
        if (p.heatMin > 0) tip += " · " + host_->text("hud.craft.need_heat", {{"n", p.heatMin}});
        if (!p.available) tip += " · " + p.need;
        if (iconButton({bx - 27, by - 27, 54, 54}, craftIconOf("process_icons", p.id, "c_hammer"), order[k] == craftProc,
                       tip, !p.available))
            pickProcess(order[k]);
    }

    // ---- the piece in the middle, the slots below it, the targets above
    const Rect piece{px - 50, py - 50, 100, 100};
    const bool pieceHot = ui_->hovered(piece);
    ui_->claim(piece);
    std::string pieceIcon = pr ? craftIconOf("process_icons", pr->id, "c_hammer") : "c_anvil";
    if (pr && !pr->slots.empty()) {
        const auto it = craftSlots.find(pr->slots.front().first);
        if (it != craftSlots.end() && !it->second.empty())
            if (const CraftThing* t = thingOf(it->second.front())) pieceIcon = t->icon;
    }
    disc(px, py, 46, kRim.withAlpha(0.85f));
    arc(px, py, 46, 2, 0, 360, kGoldPale);
    icon(pieceIcon, {px - 24, py - 24, 48, 48}, kGoldPale);
    // "make" is held: E, or the left button on the piece
    const bool holding = (keyE && !ui_->typing()) || (in_->down && pieceHot && dragRef_.empty());
    if (holding && !holdLatch_ && c.canStart && hasProc) {
        holdT_ += craftDt_;
        if (holdT_ >= kHoldSeconds) {
            craftAttempt(s);
            holdT_ = 0;
            holdLatch_ = true;
        }
    } else if (!holding) {
        holdT_ = std::max(0.f, holdT_ - craftDt_ * 3);
        holdLatch_ = false;
    }
    arc(px, py, 58, 8, 0, 360, kRim.withAlpha(0.5f));
    if (holdT_ > 0) arc(px, py, 58, 8, -90, 360.f * holdT_ / kHoldSeconds, kGold);
    else if (c.canStart && hasProc)
        arc(px, py, 58, 8, -90, 360.f * float(std::clamp(c.expectedQ, 0.05, 1.0)), kGold.withAlpha(0.35f));
    {   // the key cap under the piece
        const Rect kc{px - 15, py + 64, 30, 26};
        box(c.canStart && hasProc ? Skin::ButtonOn : Skin::Button, kc, 9);
        const float w = textW("E", 15, true);
        ui_->text(kc.x + (kc.w - w) * 0.5f, kc.y + 4, "E", c.canStart && hasProc ? kDark : kText.withAlpha(0.5f), 1.0f, true);
    }
    if (pieceHot)
        tipAt(px, py + 96, !hasProc ? host_->text("hud.craft.pick_process")
                           : c.canStart ? host_->text("hud.craft.hold")
                                        : (c.why.empty() ? host_->text("hud.craft.slot_hint") : c.why));
    if (dropping && pieceHot && hasProc) {
        addToSlot(dragRef_, "");
        dragRef_.clear();
    }
    if (pr) {
        // slots on an inner arc under the piece
        const size_t n = pr->slots.size();
        for (size_t k = 0; k < n; ++k) {
            const auto& [slot, slotName] = pr->slots[k];
            const float a = (90.f + (float(k) - float(n - 1) * 0.5f) * 34.f) * kPiF / 180.f;
            const float sx = px + std::cos(a) * (R - 64), sy = py + std::sin(a) * (R - 64) + 26;
            const Rect sr{sx - 22, sy - 22, 44, 44};
            const bool hot = ui_->hovered(sr);
            auto& v = craftSlots[slot];
            disc(sx, sy, 22, kRim.withAlpha(0.85f));
            arc(sx, sy, 22, craftSlot == slot ? 3.f : 1.5f, 0, 360, craftSlot == slot ? kGold : kGoldPale.withAlpha(0.5f));
            std::string list;
            if (!v.empty()) {
                const CraftThing* t = thingOf(v.front());
                icon(t ? t->icon : "s_lump", {sx - 12, sy - 12, 24, 24}, kText);
                if (v.size() > 1) {
                    const std::string nn = "x" + std::to_string(v.size());
                    const float w = textW(nn, 12) + 10;
                    box(Skin::ChipOn, {sx + 10, sy - 26, w, 18}, 9);
                    label(sx + 15, sy - 25, nn, kText, 12);
                }
                for (const auto& r : v) {
                    const CraftThing* t2 = thingOf(r);
                    list += (list.empty() ? "" : ", ") + (t2 ? t2->name : r);
                }
            }
            if (hot) {
                tipAt(sx, sy + 30, v.empty() ? host_->text("hud.craft.slot_empty", {{"slot", slotName}})
                                             : host_->text("hud.craft.slot_tip", {{"slot", slotName}, {"list", list}}));
                if (dropping) {
                    addToSlot(dragRef_, slot);
                    dragRef_.clear();
                }
            }
            if (clicked(sr)) {
                if (craftSlot == slot && !v.empty()) v.pop_back();
                craftSlot = slot;
            }
        }
        // targets (what shape to make) on the upper inner arc
        std::string from;
        if (!pr->slots.empty()) {
            const auto it = craftSlots.find(pr->slots.front().first);
            if (it != craftSlots.end() && !it->second.empty())
                if (const CraftThing* t = thingOf(it->second.front())) from = t->shape;
        }
        const auto tg = pr->targets.find(from);
        if (tg != pr->targets.end()) {
            const size_t m = tg->second.size();
            for (size_t k = 0; k < m; ++k) {
                const auto& [shape, nm] = tg->second[k];
                const float a = (-90.f + (float(k) - float(m - 1) * 0.5f) * 30.f) * kPiF / 180.f;
                const float sx = px + std::cos(a) * (R - 66), sy = py + std::sin(a) * (R - 66) - 18;
                if (iconButton({sx - 19, sy - 19, 38, 38}, craftIconOf("shape_icons", shape, "s_lump"), craftShape == shape,
                               host_->text("hud.craft.target_tip", {{"name", nm}})))
                    craftShape = craftShape == shape ? "" : shape;
            }
        }
        // modes on the left inner side, the length knob on the right
        for (size_t k = 0; k < pr->modes.size(); ++k) {
            const auto& [mode, nm] = pr->modes[k];
            const float a = (180.f + (float(k) - float(pr->modes.size() - 1) * 0.5f) * 30.f) * kPiF / 180.f;
            const float sx = px + std::cos(a) * (R - 62), sy = py + std::sin(a) * (R - 62);
            if (iconButton({sx - 17, sy - 17, 34, 34}, "k_finish", craftMode == mode, host_->text("hud.craft.mode_tip", {{"name", nm}})))
                craftMode = craftMode == mode ? "" : mode;
        }
        if (!craftShape.empty()) {
            const float sx = px + (R - 62), sy = py;
            const Rect kr{sx - 18, sy - 18, 36, 36};
            const bool hot = ui_->hovered(kr);
            arc(sx, sy, 24, 4, 135, 270, kRim.withAlpha(0.45f));
            if (craftLen > 0) arc(sx, sy, 24, 4, 135, 270.f * float(craftLen) / 5.f, kGold);
            iconButton(kr, "k_len", craftLen > 0, craftLen > 0 ? host_->text("hud.craft.len_tip", {{"n", craftLen}})
                                                              : host_->text("hud.craft.len_default"));
            if (hot && in_->wheel != 0) {
                craftLen = std::clamp(craftLen + (in_->wheel > 0 ? 1 : -1), 0, 5);
                in_->wheel = 0;
            }
        }
        // clear the draft: a small icon at the piece's side
        if (iconButton({px + 58, py + 50, 32, 32}, "a_clear", false, host_->text("hud.craft.clear"))) {
            craftSlots.clear();
            craftShape.clear();
            craftMode.clear();
            craftLen = 0;
        }
    }

    // ---- the forecast as a sheet pinned on the wall (top right)
    {
        const Rect sh{W - 360, 104, 316, 168};
        box(Skin::Paper, sh, 18);
        disc(sh.x + sh.w * 0.5f, sh.y + 2, 10, kSeal);
        if (!hasProc) {
            ui_->paragraph(sh.x + 20, sh.y + 26, sh.w - 40, host_->text("hud.craft.sheet_none"), kInkSoft, 14 / 15.f);
        } else if (!c.canStart) {
            title(sh.x + 20, sh.y + 20, pr->name, kInk, 20);
            ui_->paragraph(sh.x + 20, sh.y + 54, sh.w - 40, c.why, kInkRed, 14 / 15.f);
        } else {
            const bool vague = c.certainty < 0.5;
            const std::string form = c.likely.empty() ? host_->text("hud.craft.unknown_form") : c.likely + (vague ? "?" : "");
            icon(c.likely.empty() || craftShape.empty() ? pieceIcon : craftIconOf("shape_icons", craftShape, "s_lump"),
                 {sh.x + 18, sh.y + 24, 56, 56}, kLine.withAlpha(vague ? 0.55f : 1.f));
            title(sh.x + 88, sh.y + 22, ui_->fit(form, sh.w - 108, 20 / 15.f, true), kInk, 20);
            // quality: a bar, the spread hatched when the person is unsure
            const Rect bar{sh.x + 88, sh.y + 56, sh.w - 110, 12};
            ui_->roundRect(bar, kInk.withAlpha(0.18f), 6);
            const double lo = std::clamp(c.expectedQ - c.qSpread, 0.0, 1.0), hi = std::clamp(c.expectedQ + c.qSpread, 0.0, 1.0);
            const Rect rg{bar.x + float(lo) * bar.w, bar.y, std::max(6.f, float(hi - lo) * bar.w), bar.h};
            ui_->roundRect(rg, barColour(c.expectedQ).withAlpha(vague ? 0.55f : 0.9f), 6);
            if (vague)
                for (float hx = rg.x - 12; hx < rg.right(); hx += 6)
                    ui_->line(std::max(hx, rg.x), rg.bottom() - std::max(0.f, rg.x - hx), std::min(hx + 12, rg.right()),
                              rg.y + std::max(0.f, hx + 12 - rg.right()), kInk.withAlpha(0.35f));
            else ui_->rect({bar.x + float(c.expectedQ) * bar.w - 1, bar.y - 3, 2, bar.h + 6}, kInk);
            label(bar.x, bar.bottom() + 6, ui_->fit(c.tier, bar.w, 13 / 15.f), kInkSoft, 13);
            float lx = sh.x + 20;
            const float ly = sh.y + 118;
            auto stat = [&](const char* ic, const std::string& t, const Colour& col) {
                icon(ic, {lx, ly, 22, 22}, col);
                label(lx + 26, ly + 2, t, col, 14);
                lx += 26 + textW(t, 14) + 18;
            };
            stat("a_trash", vague ? "?" : std::to_string(int(std::lround(c.pRuin * 100))) + "%", c.pRuin > 0.3 ? kInkRed : kInkSoft);
            stat("w_cover", vague ? "?" : std::to_string(int(std::lround(c.pFlaw * 100))) + "%", c.pFlaw > 0.3 ? kInkRed : kInkSoft);
            stat("pause", host_->text("hud.craft.sheet_time", {{"time", c.time}}), kInkSoft);
        }
    }

    // ---- the bag at the bottom: materials, then the made things
    {
        std::vector<const CraftThing*> bag;
        for (const auto& m : c.materials) bag.push_back(&m);
        for (const auto& m : c.made) bag.push_back(&m);
        const float tok = 62;
        const int fit = std::max(1, int((W - 760) / tok));
        const int shown = std::min<int>(fit, int(bag.size()));
        bagOff_ = std::clamp(bagOff_, 0, std::max(0, int(bag.size()) - shown));
        const float bw = std::max(240.f, shown * tok + 24);
        const Rect B{px - bw * 0.5f, H - 100, bw, 74};
        box(Skin::Leather, B, 37);
        if (ui_->hovered(B) && in_->wheel != 0) {
            bagOff_ = std::clamp(bagOff_ - (in_->wheel > 0 ? 1 : -1), 0, std::max(0, int(bag.size()) - shown));
            in_->wheel = 0;
        }
        if (bag.empty()) {
            const std::string t = host_->text("hud.craft.empty");
            label(B.x + (B.w - textW(t, 14)) * 0.5f, B.y + 28, t, kText, 14);
        }
        for (int k = 0; k < shown; ++k) {
            const CraftThing& t = *bag[size_t(k + bagOff_)];
            const Rect r{B.x + 12 + k * tok + 3, B.y + 10, 54, 54};
            const bool hot = ui_->hovered(r);
            const bool made = t.ref.rfind("made:", 0) == 0;
            box(Skin::Button, r, 27);
            if (hot) disc(r.x + 27, r.y + 27, 26, kGold.withAlpha(0.16f));
            if (made) arc(r.x + 27, r.y + 27, 25, 3, -90, float(360 * std::clamp(t.q, 0.05, 1.0)), ringColour(t.q));
            icon(t.icon, {r.x + 14, r.y + 14, 26, 26}, t.flawed ? kRingBad : kText);
            if (t.count > 1) {
                const std::string nn = "x" + std::to_string(t.count);
                const float w = textW(nn, 12) + 10;
                box(Skin::ChipOn, {r.right() - w + 6, r.y - 6, w, 18}, 9);
                label(r.right() - w + 11, r.y - 5, nn, kText, 12);
            }
            if (hot) tipAt(r.x + 27, B.y - 40, t.name + (t.note.empty() ? "" : " · " + t.note));
            if (clicked(r)) {
                dragRef_ = t.ref;
                dragX_ = in_->mouseX;
                dragY_ = in_->mouseY;
            }
            // a click without a drag puts it on the piece
            if (hot && in_->released && dragRef_ == t.ref && std::hypot(in_->mouseX - dragX_, in_->mouseY - dragY_) < 6) {
                addToSlot(t.ref, "");
                dragRef_.clear();
            }
        }
    }
}

// Board 10b: the novice helper. A wish (icons, bottom left), the designs that
// answer it with "fits N%" (left), the chain of the chosen one numbered at
// its stations in the scene; what is missing carries a red "!".
void Hud::craftHelperView(const Snapshot& s, float W, float H) {
    (void)W;
    const CraftSnap& c = s.craft;
    const sim::Json wishes = craftSpec_.is_object() ? craftSpec_.value("wishes", sim::Json::array()) : sim::Json::array();
    const Rect P{36, H - 236, 2 * 18 + 4 * 62.f, 136};
    box(Skin::Leather, P, 30);
    int k = 0;
    for (const auto& w : wishes) {
        const std::string id = w.value("id", "");
        const Rect r{P.x + 18 + (k % 4) * 62.f, P.y + 14 + (k / 4) * 60.f, 52, 52};
        if (iconButton(r, w.value("icon", "a_hint"), craftWish == id, host_->text("hud.craft.wish." + id)))
            craftWish = craftWish == id ? "" : id;
        ++k;
    }
    if (craftWish.empty() || c.wish != craftWish) return;
    // the candidates
    float y = 110;
    int hoverC = -1;
    for (size_t i = 0; i < c.candidates.size(); ++i) {
        const CraftCandidate& cd = c.candidates[i];
        const Rect r{36, y, 300, 74};
        const bool hot = ui_->hovered(r);
        if (hot) hoverC = int(i);
        box(Skin::Leather, r, 22);
        if (i == 0 || hot) arc(r.x + 37, r.y + 37, 30, 3, 0, 360, kGoldPale);
        disc(r.x + 37, r.y + 37, 28, kRim.withAlpha(0.7f));
        icon(cd.icon, {r.x + 19, r.y + 19, 36, 36}, i == 0 ? kGoldPale : kText.withAlpha(0.75f));
        title(r.x + 78, r.y + 12, ui_->fit(cd.name, r.w - 120, 18 / 15.f, true), kText, 18);
        const Rect bar{r.x + 78, r.y + 44, 150, 9};
        ui_->roundRect(bar, kRim.withAlpha(0.45f), 4);
        ui_->roundRect({bar.x, bar.y, std::max(4.f, bar.w * float(cd.fit)), bar.h}, barColour(cd.fit), 4);
        if (!cd.missing.empty()) {
            const Rect m{r.right() - 40, r.y + 22, 30, 30};
            disc(m.x + 15, m.y + 15, 15, kBad);
            ui_->text(m.x + 12, m.y + 6, "!", kText, 1.0f, true);
        }
        std::string tip = host_->text("hud.craft.fit", {{"n", int(std::lround(cd.fit * 100))}});
        if (!cd.missing.empty()) {
            std::string list;
            for (const auto& m : cd.missing) list += (list.empty() ? "" : ", ") + m;
            tip += " · " + host_->text("hud.craft.missing", {{"list", list}});
        }
        if (cd.next < 0) tip += " · " + host_->text("hud.craft.no_next");
        if (hot) tipAt(r.x + r.w * 0.5f, r.bottom() + 8, tip);
        if (clicked(r) && cd.next >= 0 && cd.draft.is_object()) {
            // the step the person can do now goes onto the bench
            const sim::Json& d = cd.draft;
            for (size_t p = 0; p < c.processes.size(); ++p)
                if (c.processes[p].id == d.value("process", "")) craftProc = int(p);
            craftSlots.clear();
            if (d.contains("slots"))
                for (auto it = d["slots"].begin(); it != d["slots"].end(); ++it)
                    craftSlots[it.key()] = it.value().get<std::vector<std::string>>();
            craftShape = d.value("shape", "");
            craftMode = d.value("mode", "");
            craftLen = d.value("len", 0);
            craftHeat = d.value("heat", -1);
            if (craftProc >= 0 && !c.processes[size_t(craftProc)].slots.empty())
                craftSlot = c.processes[size_t(craftProc)].slots.front().first;
        }
        y += 84;
    }
    // the chain of the chosen candidate, numbered at its stations
    const int pick = hoverC >= 0 ? hoverC : 0;
    if (pick >= int(c.candidates.size())) return;
    const CraftCandidate& cd = c.candidates[size_t(pick)];
    std::map<std::string, int> atStation;
    float lastX = -1, lastY = -1;
    for (size_t i = 0; i < cd.steps.size(); ++i) {
        const auto& [proc, st] = cd.steps[i];
        float sx = W * 0.5f, sy = H * 0.46f;
        const auto it = stationPos_.find(st);
        if (it != stationPos_.end()) sx = it->second.first, sy = it->second.second;
        const int dup = atStation[st]++;
        sx += 44 + dup * 34.f;
        sy += 40;
        if (lastX >= 0)
            for (int d = 0; d < 24; d += 2) {
                const float t0 = d / 24.f, t1 = (d + 1) / 24.f;
                ui_->line(lastX + (sx - lastX) * t0, lastY + (sy - lastY) * t0, lastX + (sx - lastX) * t1,
                          lastY + (sy - lastY) * t1, kGoldPale.withAlpha(0.8f));
            }
        lastX = sx, lastY = sy;
        const bool now = int(i) == cd.next;
        disc(sx, sy, 17, now ? kGold : kRim.withAlpha(0.85f));
        arc(sx, sy, 17, 2, 0, 360, kGoldPale);
        const std::string n = std::to_string(i + 1);
        ui_->text(sx - textW(n, 15, true) * 0.5f, sy - 9, n, now ? kDark : kGoldPale, 1.0f, true);
        const Rect hit{sx - 17, sy - 17, 34, 34};
        if (ui_->hovered(hit)) {
            std::string pname = proc;
            for (const auto& p : c.processes)
                if (p.id == proc) pname = p.name;
            tipAt(sx, sy + 22, n + ". " + pname);
        }
    }
}

// Board 12: the recipe book. Records on the left; the chosen one's mastery,
// its stages as icons (the unknown ones blurred), the fast craft.
void Hud::craftBook(const Snapshot& s, float W, float H) {
    const CraftSnap& c = s.craft;
    const float pw = std::min(W - 80, 1100.0f), ph = std::min(H - 140, 660.0f);
    const Rect P{(W - pw) * 0.5f, 100 + (H - 140 - ph) * 0.5f, pw, ph};
    box(Skin::Paper, P, 22);
    title(P.x + 32, P.y + 22, host_->text("hud.craft.tab.book"), kInk, 28);
    seal(P.right() - 52, P.y + 44);
    if (c.recipes.empty()) {
        ui_->paragraph(P.x + 32, P.y + 80, pw - 64, host_->text("hud.craft.book_empty"), kInkSoft, 15 / 15.f);
        return;
    }
    bookPick_ = std::clamp(bookPick_, 0, int(c.recipes.size()) - 1);
    const Rect L{P.x + 24, P.y + 76, 330, P.h - 100};
    float y = L.y;
    for (size_t i = 0; i < c.recipes.size(); ++i) {
        const auto& r = c.recipes[i];
        if (y + 58 > L.bottom()) break;
        const Rect row{L.x, y, L.w, 52};
        const bool on = int(i) == bookPick_;
        ui_->roundRect(row, on ? kGold.withAlpha(0.35f) : ui_->hovered(row) ? kLine.withAlpha(0.10f)
                                                                                     : kLine.withAlpha(0.05f), 12);
        label(row.x + 14, row.y + 6, ui_->fit(r.name, row.w - 28, 15 / 15.f), kInk, 15);
        const Rect bar{row.x + 14, row.y + 34, row.w * 0.55f, 7};
        ui_->roundRect(bar, kLine.withAlpha(0.18f), 3);
        ui_->roundRect({bar.x, bar.y, std::max(4.f, float(bar.w * r.mastery)), bar.h}, r.master ? kGood : kMid, 3);
        if (r.master) icon("esteem", {row.right() - 34, row.y + 14, 22, 22}, kGoldDark);
        if (clicked(row)) {
            bookPick_ = int(i);
            renaming_ = false;
            bookCount_ = 1;
        }
        y += 58;
    }
    const auto& r = c.recipes[size_t(bookPick_)];
    const Rect D{L.right() + 30, P.y + 76, P.right() - L.right() - 60, P.h - 100};
    if (renaming_) {
        ui_->textField(ui::widgetId("craft.rename"), {D.x, D.y, D.w - 70, 40}, renameText_, r.name, 60);
        if (iconButton({D.right() - 52, D.y - 4, 48, 48}, "a_ok", true, host_->text("hud.craft.save_name")) || in_->enter) {
            if (!renameText_.empty()) host_->command("craft_rename", {{"id", c.actor}, {"recipe", r.id}, {"name", renameText_}});
            renaming_ = false;
            ui_->unfocus();
        }
    } else {
        title(D.x, D.y, ui_->fit(r.name, D.w - 70, 26 / 15.f, true), kInk, 26);
        if (iconButton({D.right() - 52, D.y - 4, 48, 48}, "a_rename", false, host_->text("hud.craft.rename"))) {
            renaming_ = true;
            renameText_ = r.name;
            ui_->focus(ui::widgetId("craft.rename"));
        }
    }
    float dy = D.y + 60;
    auto bar = [&](const std::string& name, double v, const std::string& right) {
        label(D.x, dy, name, kInkSoft, 13);
        const Rect b{D.x + 190, dy + 4, D.w - 300, 10};
        ui_->roundRect(b, kLine.withAlpha(0.18f), 5);
        ui_->roundRect({b.x, b.y, std::max(4.f, float(b.w * std::clamp(v, 0.0, 1.0))), b.h}, barColour(v), 5);
        label(b.right() + 12, dy, right, kInk, 13);
        dy += 30;
    };
    bar(host_->text("hud.craft.mastery"), r.mastery, std::to_string(int(std::lround(r.mastery * 100))) + "%");
    bar(host_->text("hud.craft.best"), r.bestQ, host_->text("hud.craft.successes", {{"n", r.successes}}));
    dy += 8;
    label(D.x, dy, host_->text("hud.craft.stages"), kInkSoft, 13);
    dy += 24;
    float sx = D.x;
    for (size_t k = 0; k < r.steps.size(); ++k) {
        if (sx + 46 > D.right()) {
            sx = D.x;
            dy += 54;
        }
        const bool known = k < r.stepKnown.size() && r.stepKnown[k];
        std::string pname = r.steps[k];
        for (const auto& p : c.processes)
            if (p.id == r.steps[k]) pname = p.name;
        const Rect b{sx, dy, 44, 44};
        disc(b.x + 22, b.y + 22, 22, kInk.withAlpha(known ? 0.12f : 0.06f));
        arc(b.x + 22, b.y + 22, 21, 1.5f, 0, 360, kLine.withAlpha(known ? 0.8f : 0.35f));
        icon(craftIconOf("process_icons", r.steps[k], "c_hammer"), {b.x + 10, b.y + 10, 24, 24},
             kInk.withAlpha(known ? 1.f : 0.3f));
        if (!known) ui_->text(b.x + 30, b.y - 2, "?", kInkRed, 1.0f, true);
        if (ui_->hovered(b)) tipAt(b.x + 22, b.bottom() + 6, pname + (known ? "" : " · " + host_->text("hud.craft.step_unknown")));
        if (k + 1 < r.steps.size()) ui_->line(b.right() + 1, b.y + 22, b.right() + 9, b.y + 22, kLine.withAlpha(0.6f));
        sx += 54;
    }
    dy += 70;
    // the fast craft: count, time, expected quality, what is missing, "make"
    label(D.x, dy, host_->text("hud.craft.fast"), kInkSoft, 13);
    dy += 26;
    if (iconButton({D.x, dy, 44, 44}, "a_minus", false, host_->text("hud.craft.less"))) bookCount_ = std::max(1, bookCount_ - 1);
    {
        const std::string n = std::to_string(bookCount_);
        title(D.x + 66 - textW(n, 26, true) * 0.5f, dy + 6, n, kInk, 26);
    }
    if (iconButton({D.x + 88, dy, 44, 44}, "a_plus", false, host_->text("hud.craft.more"))) bookCount_ = std::min(20, bookCount_ + 1);
    label(D.x + 150, dy + 4, host_->text("hud.craft.fast_time", {{"time", r.time * bookCount_}, {"stages", r.stages}}), kInk, 14);
    label(D.x + 150, dy + 24, host_->text("hud.craft.expected", {{"n", int(std::lround(r.expectedQ * 100))}}), kInkSoft, 13);
    dy += 62;
    if (!r.missing.empty()) {
        std::string list;
        for (const auto& m : r.missing) list += (list.empty() ? "" : ", ") + m;
        dy += ui_->paragraph(D.x, dy, D.w, host_->text("hud.craft.missing", {{"list", list}}), kInkRed, 14 / 15.f) + 8;
    } else if (!r.master) {
        dy += ui_->paragraph(D.x, dy, D.w, host_->text("hud.craft.not_master"), kInkSoft, 14 / 15.f) + 8;
    }
    const bool can = r.master && r.missing.empty();
    const Rect go{D.x, std::min(dy + 6, D.bottom() - 64), 64, 64};
    if (iconButton(go, "c_hammer", can, host_->text("hud.craft.make_memory"), !can) && can)
        host_->command("craft_fast", {{"id", c.actor}, {"recipe", r.id}, {"count", bookCount_}});
}

// Board 14: the made things. A row of tokens; the chosen one opens a small
// ring of actions round it (inspect, repair, onto the bench).
void Hud::craftThings(const Snapshot& s, float W, float H) {
    const CraftSnap& c = s.craft;
    const float pw = std::min(W - 80, 1100.0f), ph = std::min(H - 140, 560.0f);
    const Rect P{(W - pw) * 0.5f, 100 + (H - 140 - ph) * 0.5f, pw, ph};
    box(Skin::Paper, P, 22);
    title(P.x + 32, P.y + 22, host_->text("hud.craft.tab.things"), kInk, 28);
    seal(P.right() - 52, P.y + 44);
    if (c.made.empty()) {
        label(P.x + 32, P.y + 86, host_->text("hud.craft.things_empty"), kInkSoft, 15);
        return;
    }
    const float tok = 96;
    const int cols = std::max(1, int((P.w - 64) / tok));
    for (size_t i = 0; i < c.made.size(); ++i) {
        const CraftThing& t = c.made[i];
        const float cx = P.x + 32 + (float(i % size_t(cols)) + 0.5f) * tok, cy = P.y + 120 + float(i / size_t(cols)) * (tok + 20);
        if (cy + 50 > P.bottom()) break;
        const Rect r{cx - 34, cy - 34, 68, 68};
        const bool hot = ui_->hovered(r);
        disc(cx, cy, 34, kInk.withAlpha(hot || thingPick_ == int(i) ? 0.2f : 0.1f));
        arc(cx, cy, 33, 4, -90, float(360 * std::clamp(t.q, 0.05, 1.0)), barColour(t.q));
        if (t.wear > 0.05) arc(cx, cy, 26, 3, -90, float(360 * t.wear), kInkRed.withAlpha(0.7f));
        icon(t.icon, {cx - 18, cy - 18, 36, 36}, t.flawed ? kInkRed : kInk);
        if (hot) tipAt(cx, cy + 44, t.name + (t.note.empty() ? "" : " · " + t.note));
        if (clicked(r)) thingPick_ = thingPick_ == int(i) ? -1 : int(i);
    }
    if (thingPick_ >= 0 && thingPick_ < int(c.made.size())) {
        const size_t i = size_t(thingPick_);
        const float cx = P.x + 32 + (float(i % size_t(cols)) + 0.5f) * tok, cy = P.y + 120 + float(i / size_t(cols)) * (tok + 20);
        const int idx = std::atoi(c.made[i].ref.c_str() + 5);
        struct Act {
            const char* ic;
            const char* tip;
        };
        static const Act acts[] = {{"a_inspect", "hud.craft.inspect"}, {"c_repair", "hud.craft.repair_go"}, {"hand", "hud.craft.to_bench"}};
        for (int k = 0; k < 3; ++k) {
            const float a = (-150.f + k * 60.f) * kPiF / 180.f;
            const float bx = cx + std::cos(a) * 62, by = cy + std::sin(a) * 62;
            if (iconButton({bx - 22, by - 22, 44, 44}, acts[k].ic, false, host_->text(acts[k].tip))) {
                if (k == 0) host_->command("craft_inspect", {{"id", c.actor}, {"item", idx}});
                if (k == 1) {
                    repairPick_ = idx;
                    craftTab = 3;
                }
                if (k == 2) {
                    craftTab = 0;
                    if (craftProc >= 0 && craftProc < int(c.processes.size())) {
                        std::string slot = craftSlot.empty() && !c.processes[size_t(craftProc)].slots.empty()
                                               ? c.processes[size_t(craftProc)].slots.front().first
                                               : craftSlot;
                        craftSlots[slot].push_back(c.made[i].ref);
                    }
                }
                thingPick_ = -1;
                break;
            }
        }
    }
}

// Board 15: repair and sharpening. What is worn or flawed; the chosen one's
// wear and flaws; the repair's ways as icons.
void Hud::craftRepair(const Snapshot& s, float W, float H) {
    const CraftSnap& c = s.craft;
    const float pw = std::min(W - 80, 1000.0f), ph = std::min(H - 140, 560.0f);
    const Rect P{(W - pw) * 0.5f, 100 + (H - 140 - ph) * 0.5f, pw, ph};
    box(Skin::Paper, P, 22);
    title(P.x + 32, P.y + 22, host_->text("hud.craft.tab.repair"), kInk, 28);
    seal(P.right() - 52, P.y + 44);
    std::vector<std::pair<int, const CraftThing*>> list;
    for (const auto& t : c.made)
        if (t.wear > 0.01 || t.flawed) list.push_back({std::atoi(t.ref.c_str() + 5), &t});
    if (list.empty()) {
        label(P.x + 32, P.y + 86, host_->text("hud.craft.repair_empty"), kInkSoft, 15);
        return;
    }
    bool found = false;
    for (const auto& [idx, t] : list) found |= idx == repairPick_;
    if (!found) repairPick_ = list.front().first;
    float x = P.x + 32;
    const float y = P.y + 80;
    for (const auto& [idx, t] : list) {
        if (x + 68 > P.right() - 32) break;
        const Rect r{x, y, 60, 60};
        disc(x + 30, y + 30, 30, kInk.withAlpha(idx == repairPick_ ? 0.22f : 0.08f));
        arc(x + 30, y + 30, 29, 3, -90, float(360 * std::clamp(1.0 - t->wear, 0.05, 1.0)), barColour(1.0 - t->wear));
        icon(t->icon, {x + 14, y + 14, 32, 32}, t->flawed ? kInkRed : kInk);
        if (ui_->hovered(r)) tipAt(x + 30, y + 66, t->name);
        if (clicked(r)) repairPick_ = idx;
        x += 72;
    }
    const CraftThing* t = nullptr;
    for (const auto& [idx, tt] : list)
        if (idx == repairPick_) t = tt;
    if (!t) return;
    float dy = y + 100;
    title(P.x + 32, dy, ui_->fit(t->name, pw - 64, 24 / 15.f, true), kInk, 24);
    dy += 44;
    label(P.x + 32, dy, host_->text("hud.craft.wear"), kInkSoft, 13);
    const Rect b{P.x + 200, dy + 4, 300, 10};
    ui_->roundRect(b, kLine.withAlpha(0.18f), 5);
    ui_->roundRect({b.x, b.y, std::max(4.f, float(b.w * t->wear)), b.h}, kBad, 5);
    label(b.right() + 12, dy, std::to_string(int(std::lround(t->wear * 100))) + "%", kInk, 13);
    dy += 34;
    label(P.x + 32, dy, host_->text("hud.craft.flaws"), kInkSoft, 13);
    ui_->paragraph(P.x + 200, dy, pw - 260, t->flawed ? t->note : host_->text("hud.craft.no_flaws"), t->flawed ? kInkRed : kInk,
                   14 / 15.f);
    dy += 60;
    // the repair's ways (process modes); none: one way
    const CraftProcessSnap* rp = nullptr;
    for (const auto& p : c.processes)
        if (p.id == "repair") rp = &p;
    std::vector<std::pair<std::string, std::string>> ways;
    if (rp) ways = rp->modes;
    if (ways.empty()) ways.push_back({"", host_->text("hud.craft.repair_go")});
    float wx = P.x + 32;
    for (const auto& [mode, nm] : ways) {
        std::string tip = nm;
        if (rp && !rp->available) tip += " · " + rp->need;
        if (iconButton({wx, dy, 60, 60}, mode.empty() ? "c_repair" : "k_finish", true, tip, rp && !rp->available))
            host_->command("craft_repair", {{"id", c.actor}, {"item", repairPick_}, {"mode", mode}});
        wx += 74;
    }
}

// Boards 11 and 13: what came out of the attempt (quality, the chain's links,
// flaws) and, after a breakthrough, the new way with its name to write down.
void Hud::craftResultCard(const Snapshot& s, float W, float H) {
    const CraftSnap& c = s.craft;
    const CraftResultSnap& r = c.result;
    const float pw = std::min(W - 80, 760.0f), ph = std::min(H - 160, 520.0f);
    const Rect P{(W - pw) * 0.5f, 110 + (H - 160 - ph) * 0.5f, pw, ph};
    const float cx = P.x + pw * 0.5f;
    if (r.breakthrough)   // the rays of the insight behind the card
        for (int k = 0; k < 16; ++k) {
            const float a = k * kPiF / 8.f;
            for (float d = pw * 0.45f; d < pw * 0.9f; d += 26)
                ui_->line(cx + std::cos(a) * d, P.y + ph * 0.5f + std::sin(a) * d, cx + std::cos(a) * (d + 8),
                          P.y + ph * 0.5f + std::sin(a) * (d + 8), kGoldPale.withAlpha(0.55f));
        }
    box(Skin::Paper, P, 22);
    const ActorSnap* who = actor(s, c.actor);
    {
        const std::string kicker = host_->text(r.breakthrough ? "hud.craft.insight" : "hud.craft.result");
        label(cx - textW(kicker, 13) * 0.5f, P.y + 22, kicker, kLine, 13);
        const std::string head = r.breakthrough ? host_->text("hud.craft.insight_lead", {{"name", who ? who->name : std::string()}}) : r.head;
        title(cx - textW(head, 30, true) * 0.5f, P.y + 44, ui_->fit(head, pw - 60, 30 / 15.f, true), kInk, 30);
        if (!r.what.empty()) label(cx - textW(r.what, 16) * 0.5f, P.y + 86, ui_->fit(r.what, pw - 60, 16 / 15.f), kInkSoft, 16);
    }
    float y = P.y + 124;
    if (r.q >= 0) {
        // quality as a ring, the chain's links as bars, the weakest one marked
        const float qx = P.x + 90, qy = y + 60;
        disc(qx, qy, 50, kInk.withAlpha(0.08f));
        arc(qx, qy, 46, 9, -90, float(360 * std::clamp(r.q, 0.03, 1.0)), barColour(r.q));
        const std::string qs = std::to_string(int(std::lround(r.q * 100)));
        title(qx - textW(qs, 26, true) * 0.5f, qy - 16, qs, kInk, 26);
        label(qx - textW(r.tier, 13) * 0.5f, qy + 58, r.tier, kInkSoft, 13);
        float ly = y;
        int weak = -1;
        for (size_t i = 0; i < r.links.size(); ++i)
            if (weak < 0 || r.links[i].second < r.links[size_t(weak)].second) weak = int(i);
        for (size_t i = 0; i < r.links.size() && ly < y + 170; ++i) {
            const auto& [nm, q] = r.links[i];
            label(P.x + 180, ly, ui_->fit(nm, 170, 13 / 15.f), int(i) == weak ? kInkRed : kInk, 13);
            const Rect b{P.x + 360, ly + 4, pw - 420, 9};
            ui_->roundRect(b, kLine.withAlpha(0.18f), 4);
            ui_->roundRect({b.x, b.y, std::max(4.f, float(b.w * std::clamp(q, 0.0, 1.0))), b.h}, barColour(q), 4);
            ly += 24;
        }
        if (weak >= 0 && r.links.size() > 1)
            label(P.x + 180, ly + 4, ui_->fit(host_->text("hud.craft.weak", {{"name", r.links[size_t(weak)].first}}), pw - 220, 13 / 15.f),
                  kInkSoft, 13);
        y += 200;
    }
    if (!r.flaws.empty()) {
        float fx = P.x + 40;
        for (const auto& f : r.flaws) {
            const float w = textW(f, 14) + 24;
            box(Skin::Chip, {fx, y, w, 28}, 14);
            label(fx + 12, y + 5, f, kInkRed, 14);
            fx += w + 8;
        }
        y += 40;
    }
    if (r.breakthrough && !r.recipe.empty()) {
        label(P.x + 40, y, host_->text("hud.craft.name_way"), kInkSoft, 13);
        ui_->textField(ui::widgetId("craft.insight"), {P.x + 40, y + 20, pw - 160, 40}, renameText_, r.recipeName, 60);
        if (iconButton({P.right() - 100, y + 16, 48, 48}, "a_book", true, host_->text("hud.craft.save_name"))) {
            if (!renameText_.empty()) host_->command("craft_rename", {{"id", c.actor}, {"recipe", r.recipe}, {"name", renameText_}});
            ui_->unfocus();
            resultOpen_ = false;
        }
    }
    // the actions: take (close), repeat, inspect
    const float by = P.bottom() - 82;
    if (iconButton({cx - 110, by, 60, 60}, "a_ok", true, host_->text("hud.craft.take"))) resultOpen_ = false;
    if (iconButton({cx - 30, by, 60, 60}, "a_repeat", false, host_->text("hud.craft.repeat"), lastDraft_.is_null())) {
        if (!lastDraft_.is_null()) host_->command("craft_attempt", lastDraft_);
        resultOpen_ = false;
    }
    if (iconButton({cx + 50, by, 60, 60}, "a_inspect", false, host_->text("hud.craft.inspect"), r.made < 0) && r.made >= 0)
        host_->command("craft_inspect", {{"id", c.actor}, {"item", r.made}});
}

// ---------------------------------------------------------------- the backstory
// The new game's first screen (style G): who the player was before. The
// backstories stand as medallions on a leather column; the picked one opens
// on a paper sheet with its skills as bars, traits as chips, the build as
// rising and falling marks, belongings as icon tiles, the purse and where the
// life begins. No text buttons: "begin" is the gold seal, "at random" the
// small ring beside it, their words in the tooltip. Enter begins, the arrows
// turn the column.
void Hud::backstory(float W, float H) {
    if (backstories.empty()) {
        panel = "none";
        return;
    }
    tipText_.clear();
    const int n = int(backstories.size());
    backstoryPick = ((backstoryPick % n) + n) % n;
    dimmer(W, H, 0.72f);
    const std::string head = host_->text("hud.backstory.title");
    title((W - textW(head, 40, true)) * 0.5f, 34, head, kGoldPale, 40);
    const std::string sub = host_->text("hud.backstory.subtitle");
    label((W - textW(sub, 16)) * 0.5f, 88, sub, kText, 16, true);

    // the column of medallions
    const float top = 132, colW = std::min(420.0f, W * 0.27f);
    const float rowH = std::min(92.0f, (H - top - 150) / float(n));
    const float total = colW + 28 + std::min(1000.0f, W - 80 - colW - 28);
    const Rect C{std::max(40.0f, (W - total) * 0.5f), top, colW, rowH * float(n) + 24};
    box(Skin::Leather, C, 22);
    for (int i = 0; i < n; ++i) {
        const BackstoryCard& b = backstories[size_t(i)];
        const Rect row{C.x + 10, C.y + 12 + rowH * float(i), C.w - 20, rowH - 4};
        const bool on = i == backstoryPick, hot = ui_->hovered(row);
        if (on) ui_->roundRect(row, kGold.withAlpha(0.20f), 16);
        else if (hot) ui_->roundRect(row, kGold.withAlpha(0.08f), 16);
        const float d = std::min(64.0f, rowH - 14);
        const float cx = row.x + 12 + d * 0.5f, cy = row.y + row.h * 0.5f;
        box(on ? Skin::ButtonOn : Skin::Button, {cx - d * 0.5f, cy - d * 0.5f, d, d}, d * 0.5f);
        const float s = std::round(d * 0.52f);
        icon(b.icon, {cx - s * 0.5f, cy - s * 0.5f, s, s}, on ? kDark : kText);
        const float tx = cx + d * 0.5f + 14, tw = row.right() - tx - 8;
        title(tx, cy - 24, ui_->fit(b.name, tw, 21 / 15.f, true), on ? kGoldPale : kText, 21);
        label(tx, cy + 4, ui_->fit(b.desc, tw, 13 / 15.f), on ? kText : kText.withAlpha(0.7f), 13);
        if (clicked(row)) backstoryPick = i;
    }

    // the sheet of the picked one
    const BackstoryCard& b = backstories[size_t(backstoryPick)];
    const float px = C.right() + 28;
    const Rect P{px, top, std::min(W - px - 40, 1000.0f), C.h};
    box(Skin::Paper, P, 22);
    seal(P.right() - 44, P.y + 40);
    const float big = 104, bx = P.x + 36 + big * 0.5f, by = P.y + 30 + big * 0.5f;
    disc(bx, by, big * 0.5f, kNavy);
    ring(bx, by, big * 0.5f);
    icon(b.icon, {bx - 30, by - 30, 60, 60}, kGoldPale);
    const float tx = P.x + 36 + big + 26, tw = P.right() - tx - 90;
    title(tx, P.y + 30, b.name, kInk, 34);
    label(tx, P.y + 76, ui_->fit(b.desc, tw, 16 / 15.f), kInkRed, 16);
    float y = P.y + 30 + big + 18;
    y += ui_->paragraph(P.x + 36, y, P.w - 72, b.life, kInk, 15 / 15.f) + 18;
    ui_->line(P.x + 36, y, P.right() - 36, y, kInkSoft.withAlpha(0.35f));
    y += 16;

    // left: skills as bars
    const float lx = P.x + 36, lw = (P.w - 72) * 0.48f, rx = lx + lw + 36, rw = P.right() - 36 - rx;
    float ly = y;
    title(lx, ly, host_->text("hud.backstory.skills"), kInkSoft, 17);
    ly += 30;
    const float barX = lx + lw * 0.55f, barW = lw - (barX - lx);
    const float skillRowH = std::clamp((P.bottom() - 170 - ly) / std::max(1.0f, float(b.skills.size())), 18.0f, 26.0f);
    for (const auto& [name, lvl] : b.skills) {
        label(lx, ly, ui_->fit(name, barX - lx - 10, 14 / 15.f), kInk, 14);
        const Rect bar{barX, ly + 5, barW, 9};
        ui_->roundRect(bar, kLine.withAlpha(0.18f), 4.5f);
        ui_->roundRect({bar.x, bar.y, std::max(9.0f, bar.w * float(lvl)), bar.h}, lvl >= 0.5 ? kGoldDark : kMid, 4.5f);
        ly += skillRowH;
    }

    // right: traits as chips, the pool dimmer, the build's marks, rights
    float ry = y;
    title(rx, ry, host_->text("hud.backstory.traits"), kInkSoft, 17);
    ry += 30;
    auto chips = [&](const std::vector<std::string>& list, bool dark, float& cy) {
        float cxp = rx;
        for (const auto& t : list) {
            const float w = textW(t, 13) + 22;
            if (cxp + w > rx + rw) {
                cxp = rx;
                cy += 30;
            }
            box(dark ? Skin::ChipOn : Skin::Chip, {cxp, cy, w, 24}, 12);
            label(cxp + 11, cy + 3, t, dark ? kText : kInkSoft, 13);
            cxp += w + 6;
        }
        cy += 32;
    };
    chips(b.traits, true, ry);
    if (b.pick > 0) {
        label(rx, ry, host_->text("hud.backstory.traits_pool", {{"n", b.pick}}), kInkSoft, 13);
        ry += 22;
        chips(b.pool, false, ry);
    }
    if (!b.attrs.empty()) {
        title(rx, ry + 4, host_->text("hud.backstory.attrs"), kInkSoft, 17);
        ry += 34;
        float ax = rx;
        for (const auto& [name, shift] : b.attrs) {
            const float w = textW(name, 13) + 34;
            if (ax + w > rx + rw) {
                ax = rx;
                ry += 26;
            }
            trendArrow(ax + 8, ry + 10, shift > 0 ? 1 : -1, shift > 0 ? kGood : kBad);
            label(ax + 20, ry + 2, name, shift > 0 ? kGood : kInkRed, 13);
            ax += w + 6;
        }
        ry += 30;
    }
    if (!b.rights.empty()) {
        title(rx, ry + 4, host_->text("hud.backstory.rights"), kInkSoft, 17);
        ry += 34;
        chips(b.rights, true, ry);
    }

    // the bottom band: belongings, purse, start
    const float gy = P.bottom() - 132;
    ui_->line(P.x + 36, gy - 12, P.right() - 36, gy - 12, kInkSoft.withAlpha(0.35f));
    title(P.x + 36, gy, host_->text("hud.backstory.gear"), kInkSoft, 17);
    float gx = P.x + 36;
    const float tile = 50, ty = gy + 30;
    auto tileAt = [&](const std::string& ic, const std::string& tip, int count) {
        const Rect t{gx, ty, tile, tile};
        box(Skin::Disc, t, 12, 0x2b3444);
        icon(ic, {t.x + 9, t.y + 9, 32, 32}, kText);
        if (count > 1) {
            const std::string c = std::to_string(count);
            const float w = textW(c, 12) + 10;
            ui_->roundRect({t.right() - w + 4, t.bottom() - 14, w, 18}, kInkRed, 9);
            label(t.right() - w + 9, t.bottom() - 13, c, kGoldPale, 12);
        }
        if (ui_->hovered(t)) tipAt(t.x + tile * 0.5f, t.y - 44, tip);
        gx += tile + 8;
    };
    const float gearEnd = P.right() - 330;
    for (const auto& g : b.gear) {
        if (gx + tile > gearEnd) break;
        tileAt(g.icon, g.name, g.n);
    }
    if (b.coins > 0 && gx + tile <= gearEnd) tileAt(coinIcon, host_->text("hud.backstory.coins", {{"n", b.coins}}), b.coins);
    if (!b.recipes.empty()) {
        std::string r;
        for (const auto& x : b.recipes) r += (r.empty() ? "" : ", ") + x;
        label(P.x + 36, ty + tile + 12,
              ui_->fit(host_->text("hud.backstory.recipes") + ": " + r, gearEnd - P.x - 40, 13 / 15.f), kInkSoft, 13);
    }
    const float sx = P.right() - 310;
    title(sx, gy, host_->text("hud.backstory.start"), kInkSoft, 17);
    box(Skin::Disc, {sx, ty, tile, tile}, 12, 0x2b3444);
    icon(b.startIcon, {sx + 9, ty + 9, 32, 32}, kText);
    ui_->paragraph(sx + tile + 12, ty + 4, P.right() - 36 - (sx + tile + 12), b.start, kInk, 14 / 15.f);

    // begin (the gold seal) and at random
    const float by2 = C.bottom() + 22, bcx = P.x + P.w * 0.5f;
    if (iconButton({bcx - 42, by2, 84, 84}, "a_ok", true, host_->text("hud.backstory.begin")) || in_->enter)
        backstoryBegin = true;
    if (iconButton({bcx + 62, by2 + 16, 52, 52}, "a_repeat", false, host_->text("hud.backstory.random")))
        backstoryPick = int((std::uint64_t(clock_ * 1000.0) * 2654435761ull) % std::uint64_t(n));
    if (backstoryBegin) {
        backstoryBegin = false;
        host_->command("start_backstory", {{"backstory", b.id}});
        panel = "none";
    }
    drawTip(W, H);
}

} // namespace altera::game
