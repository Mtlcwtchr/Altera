// AL-69: the making of a new world, in style Edge - the size, the seed, the
// water, the mountains, the climate and the rivers, with the world's map
// beside them (the generator itself at a small size, on a worker thread, so
// the picture is the world that will be made), or the imported world of
// Altera. The button tells the game (HudHost::menuAction "world_create").

#include "game_view/hud.hpp"
#include "game_view/hud_style.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace altera::game {
namespace {

using ui::Colour;
using ui::Rect;
using namespace hudroles;

constexpr int kPreview = 256;   // the map's size, pixels and heights a side

} // namespace

void Hud::worldPanel(float W, float H) {
    terrain_core::WorldSpec& d = worldDraft;
    // the map: a new one is asked for whenever the choice changed and none is being drawn
    if (worldJob_.valid() && worldJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        worldShown_ = worldJob_.get();
        if (worldTex_) SDL_DestroyTexture(worldTex_);
        worldTex_ = SDL_CreateTexture(sdl_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, worldShown_.size, worldShown_.size);
        if (worldTex_) {
            SDL_UpdateTexture(worldTex_, nullptr, worldShown_.rgba.data(), worldShown_.size * 4);
            SDL_SetTextureScaleMode(worldTex_, SDL_SCALEMODE_LINEAR);
        }
    }
    const bool lore = d.lore();
    const std::string key = d.key();
    if (!lore && !worldJob_.valid() && worldShown_.key != key) {
        const terrain_core::WorldSpec spec = d;
        worldJob_ = std::async(std::launch::async, [spec, key] {
            WorldPreview p;
            p.key = key;
            const terrain_core::GeneratedWorld w = terrain_core::generateWorld(spec, kPreview);
            p.rgba = terrain_core::worldPicture(w, kPreview);
            p.size = kPreview;
            p.land = w.landShare;
            p.peak = w.peakMetres;
            p.passed = w.passed();
            return p;
        });
    }

    dimmer(W, H, 0.62f);
    const float w = std::min(1080.f, W - 60), h = std::min(640.f, H - 60);
    const Rect P{(W - w) * 0.5f, (H - h) * 0.5f, w, h};
    glass(P, true);
    seal(P.right() - 30, P.y + 30);
    caps(P.x + 36, P.y + 28, host_->text("menu.world.title"), kGold, 26, 0.14f);
    label(P.x + 36, P.y + 66, ui_->fit(host_->text("menu.world.hint"), P.w - 72, 13 / 15.f), kMute, 13);

    // the choices, left
    const float lx = P.x + 36, mapSide = std::min(h - 200, w * 0.42f), cx0 = lx + 150;
    const float colRight = P.right() - 36 - mapSide - 30;
    float y = P.y + 104;
    auto row = [&](const std::string& name, const std::vector<std::string>& chips, int on) -> int {
        label(lx, y + 8, name, kText, 15);
        float cx = cx0;
        int hit = -1;
        for (std::size_t i = 0; i < chips.size(); ++i) {
            const float cw = std::max(64.f, textW(chips[i], 15) + 26);
            if (cx + cw > colRight) break;
            if (textButton({cx, y, cw, 34}, chips[i], int(i) == on)) hit = int(i);
            cx += cw + 6;
        }
        y += 48;
        return hit;
    };
    const int kind = row(host_->text("menu.world.kind"),
                         {host_->text("menu.world.kind.procedural"), host_->text("menu.world.kind.lore")}, lore ? 1 : 0);
    if (kind == 0 && lore) d.kind = "procedural";
    if (kind == 1) d.kind = "lore";
    if (!d.lore()) {
        static const int kSizes[] = {8, 12, 16, 24};
        std::vector<std::string> sizes;
        int sizeOn = 1;
        for (int i = 0; i < 4; ++i) {
            sizes.push_back(host_->text("menu.world.km", {{"n", kSizes[i]}}));
            if (kSizes[i] == d.sizeKm) sizeOn = i;
        }
        if (const int r = row(host_->text("menu.world.size"), sizes, sizeOn); r >= 0) d.sizeKm = kSizes[r];
        auto three = [&](const char* name, const char* a, const char* b, const char* c, int& v) {
            if (const int r = row(host_->text(name), {host_->text(a), host_->text(b), host_->text(c)}, v); r >= 0) v = r;
        };
        three("menu.world.water", "menu.world.water.inland", "menu.world.water.coast", "menu.world.water.islands", d.water);
        three("menu.world.mountains", "menu.world.less", "menu.world.some", "menu.world.more", d.mountains);
        three("menu.world.climate", "menu.world.climate.cold", "menu.world.climate.temperate", "menu.world.climate.warm",
              d.climate);
        three("menu.world.rivers", "menu.world.less", "menu.world.some", "menu.world.more", d.rivers);
        // the seed: typed (digits only) or thrown
        label(lx, y + 8, host_->text("menu.world.seed"), kText, 15);
        const float fw = std::min(170.f, colRight - cx0 - 140);
        ui_->textField(ui::widgetId("world.seed"), {cx0, y, fw, 34}, worldSeedText, "1", 9);
        worldSeedText.erase(std::remove_if(worldSeedText.begin(), worldSeedText.end(), [](char c) { return c < '0' || c > '9'; }),
                            worldSeedText.end());
        if (!worldSeedText.empty()) d.seed = std::uint32_t(std::min(999999999ull, std::stoull(worldSeedText)));
        const std::string dice = host_->text("menu.world.random");
        if (textButton({cx0 + fw + 8, y, textW(dice, 15) + 26, 34}, dice, false)) {
            d.seed = std::uint32_t((d.seed * 2654435761ull + SDL_GetTicksNS()) % 900000u) + 1u;
            worldSeedText = std::to_string(d.seed);
        }
        y += 48;
    } else {
        y += 8;
        label(lx, y, ui_->fit(host_->text("menu.world.lore_note"), colRight - lx, 14 / 15.f), kMute, 14);
    }

    // the map, right
    const Rect M{P.right() - 36 - mapSide, P.y + 104, mapSide, mapSide};
    ui_->rect(M, kRim.withAlpha(0.6f));
    if (!lore && worldTex_ && worldShown_.key == key) ui_->image(M, worldTex_);
    else if (!lore && worldTex_) {
        ui_->image(M, worldTex_);
        ui_->rect(M, kRim.withAlpha(0.45f));   // the last map, dimmed while the new one is drawn
    }
    ui_->border(M, kGold.withAlpha(0.7f), 1.0f, 0);
    std::string under;
    if (lore) under = host_->text("menu.world.lore_map");
    else if (worldShown_.key != key || !worldTex_) under = host_->text("menu.world.drawing");
    else
        under = host_->text("menu.world.facts", {{"land", int(std::lround(worldShown_.land * 100))},
                                                 {"peak", int(std::lround(worldShown_.peak / 10.0) * 10)},
                                                 {"km", d.sizeKm}});
    label(M.x, M.bottom() + 10, ui_->fit(under, M.w, 13 / 15.f), kMute, 13);
    if (!lore && worldShown_.key == key && !worldShown_.passed)
        label(M.x, M.bottom() + 30, ui_->fit(host_->text("menu.world.weak"), M.w, 13 / 15.f), kGold, 13);

    if (!menuNote.empty()) label(lx, P.bottom() - 96, ui_->fit(menuNote, colRight - lx, 14 / 15.f), kGold, 14);
    const float bw = 200, by = P.bottom() - 58;
    if (edgeButton({lx, by, bw, 38}, host_->text("menu.world.create"), true)) {
        sim::Json spec = sim::Json::parse(terrain_core::worldSpecJson(d));
        host_->menuAction("world_create", spec);
    }
    if (edgeButton({lx + bw + 16, by, 160, 38}, host_->text("menu.back"))) host_->menuAction("back", sim::Json::object());
    ui_->claim(P);
}

} // namespace altera::game
