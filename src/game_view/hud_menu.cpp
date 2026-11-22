// The game's menus in style G: the main menu over the village, the pause
// menu (Esc), the save and load slots and the few settings. The buttons only
// tell the host (HudHost::menuAction); the game moves between the panels,
// saves and loads.

#include "game_view/hud.hpp"
#include "game_view/hud_style.hpp"

#include <algorithm>
#include <cmath>

namespace altera::game {
namespace {

using ui::Colour;
using ui::Rect;

using namespace hudroles;   // the palette's roles (hud_style.hpp, AL-62)

} // namespace

void Hud::resetWorld() {
    for (auto& [id, t] : portraits_)
        if (t) SDL_DestroyTexture(t);
    portraits_.clear();
    trends_.clear();
    toasts_.clear();
    seenChronicle_ = 0;
    selected_ = wheelTarget_ = kinPick_ = aim = sim::kNone;
    pin_ = hover_ = -1;
    pendingSlot_ = -1;
}

// A row of a menu column: a medallion with its icon and the line in Kurale.
bool Hud::menuButton(const Rect& r, const std::string& iconName, const std::string& s, bool enabled) {
    const bool hot = enabled && ui_->hovered(r);
    if (hot && hudPalette().theme.flag("textured")) ui_->roundRect(r, kGold.withAlpha(0.16f), r.h * 0.5f);
    const float d = r.h - 12, cx = r.x + 6 + d * 0.5f, cy = r.y + r.h * 0.5f;
    const bool edge = !hudPalette().theme.flag("textured");
    if (edge) {   // AL-66: a diamond, lit under the pointer
        if (hot) selRow(r);
        diamond(cx, cy, d * 0.74f, hot, false);
    } else {
        box(hot ? Skin::ButtonOn : Skin::Button, {cx - d * 0.5f, cy - d * 0.5f, d, d}, d * 0.5f);
    }
    const float is = std::round(d * 0.52f);
    const float a = enabled ? 1.0f : 0.35f;
    icon(iconName, {cx - is * 0.5f, cy - is * 0.5f, is, is}, (hot ? (edge ? kGoldPale : kDark) : kText).withAlpha(a));
    title(cx + d * 0.5f + 16, cy - 15, ui_->fit(s, r.right() - cx - d * 0.5f - 24, 22 / 15.f, true),
          (hot ? kGoldPale : kText).withAlpha(a), 22);
    ui_->claim(r);
    return enabled && in_->pressed && ui_->hovered(r);
}

void Hud::mainMenu(float W, float H) {
    // the village stays in sight; the left side darkens under the column
    ui_->rect({0, 0, W, H}, kRim.withAlpha(0.28f));
    for (int i = 0; i < 12; ++i) {
        const float x = W * 0.42f * float(i) / 12.f;
        ui_->rect({x, 0, W * 0.42f / 12.f + 1, H}, kRim.withAlpha(0.55f * (1.0f - float(i) / 12.f)));
    }
    ui_->claim({0, 0, W, H});
    const float x = std::max(48.0f, W * 0.07f), top = std::max(60.0f, H * 0.14f);
    const std::string name = host_->text("menu.title");
    title(x + 3, top + 4, name, kRim.withAlpha(0.9f), 84);
    title(x, top, name, kGoldPale, 84);
    label(x + 4, top + 100, host_->text("menu.tagline"), kText, 17, true);

    struct Item {
        const char* icon;
        const char* key;
        const char* action;
        bool enabled;
    };
    const Item items[] = {{"play", "menu.continue", "continue", canContinue},
                          {"a_plus", "menu.new", "new", true},
                          {"a_book", "menu.load", "open_load", canLoad},
                          {"a_hint", "menu.settings", "settings", true},
                          {"close", "menu.quit", "quit", true}};
    const float rowH = 62, colW = 380;
    const Rect C{x - 8, top + 150, colW, rowH * 5 + 20};
    box(Skin::Leather, C, 24);
    for (int i = 0; i < 5; ++i) {
        const Rect r{C.x + 10, C.y + 10 + rowH * float(i), C.w - 20, rowH - 4};
        if (menuButton(r, items[i].icon, host_->text(items[i].key), items[i].enabled))
            host_->menuAction(items[i].action, sim::Json::object());
    }
    if (!menuNote.empty()) label(C.x + 8, C.bottom() + 16, ui_->fit(menuNote, W - C.x - 40, 1.0f), kGold, 15, true);
    const std::string hint = host_->text("menu.hint");
    label(x, H - 44, hint, kText.withAlpha(0.6f), 13, true);
}

void Hud::pauseMenu(float W, float H) {
    dimmer(W, H, 0.6f);
    struct Item {
        const char* icon;
        const char* key;
        const char* action;
    };
    const Item items[] = {{"play", "menu.resume", "resume"},     {"a_ok", "menu.save", "open_save"},
                          {"a_book", "menu.load", "open_load"},  {"a_hint", "menu.settings", "settings"},
                          {"map", "menu.main_menu", "main_menu"}, {"close", "menu.quit", "quit"}};
    const int n = 6;
    const float rowH = 60, colW = 380;
    const Rect C{(W - colW) * 0.5f, std::max(110.0f, (H - rowH * n - 20) * 0.5f), colW, rowH * n + 20};
    const std::string head = host_->text("menu.pause");
    title((W - textW(head, 40, true)) * 0.5f, C.y - 66, head, kGoldPale, 40);
    box(Skin::Leather, C, 24);
    for (int i = 0; i < n; ++i) {
        const Rect r{C.x + 10, C.y + 10 + rowH * float(i), C.w - 20, rowH - 4};
        const bool enabled = std::string(items[i].action) != "open_load" || canLoad;
        if (menuButton(r, items[i].icon, host_->text(items[i].key), enabled))
            host_->menuAction(items[i].action, sim::Json::object());
    }
    if (!menuNote.empty()) {
        const std::string t = ui_->fit(menuNote, W - 80, 1.0f);
        label((W - textW(t, 15)) * 0.5f, C.bottom() + 18, t, kGold, 15, true);
    }
}

void Hud::slotsPanel(float W, float H) {
    dimmer(W, H, 0.66f);
    std::vector<const MenuSlot*> rows;
    for (const auto& m : menuSlots)
        if (!(slotsSave && m.slot == 0)) rows.push_back(&m);   // the autosave is not saved into by hand
    const float rowH = std::min(78.0f, (H - 260) / std::max(1.0f, float(rows.size())));
    const float w = std::min(860.0f, W - 80);
    const Rect P{(W - w) * 0.5f, std::max(40.0f, (H - (rowH * float(rows.size()) + 170)) * 0.5f), w,
                 rowH * float(rows.size()) + 170};
    box(Skin::Paper, P, 22);
    seal(P.right() - 44, P.y + 40);
    title(P.x + 36, P.y + 26, host_->text(slotsSave ? "menu.save.title" : "menu.load.title"), kInk, 34);
    float y = P.y + 84;
    ui_->line(P.x + 36, y, P.right() - 36, y, kInkSoft.withAlpha(0.35f));
    y += 8;
    for (const MenuSlot* m : rows) {
        const Rect r{P.x + 24, y, P.w - 48, rowH - 6};
        const bool usable = slotsSave || m->used;
        const bool hot = usable && ui_->hovered(r);
        const bool armed = slotsSave && pendingSlot_ == m->slot;
        if (armed) ui_->roundRect(r, kSeal.withAlpha(0.12f), 14);
        else if (hot) ui_->roundRect(r, kLine.withAlpha(0.12f), 14);
        const float d = std::min(52.0f, r.h - 10), cx = r.x + 12 + d * 0.5f, cy = r.y + r.h * 0.5f;
        box(m->used ? Skin::ButtonOn : Skin::Button, {cx - d * 0.5f, cy - d * 0.5f, d, d}, d * 0.5f);
        const std::string num = m->slot == 0 ? host_->text("menu.slot.auto_mark") : std::to_string(m->slot);
        title(cx - textW(num, 20, true) * 0.5f, cy - 13, num, m->used ? kDark : kText, 20);
        const float tx = cx + d * 0.5f + 16;
        const float whenW = m->used ? textW(m->when, 14) : 0;
        const float tw = r.right() - 16 - whenW - 20 - tx;
        if (m->used) {
            title(tx, cy - 25, ui_->fit(m->title, tw, 20 / 15.f, true), kInk, 20);
            label(tx, cy + 3, ui_->fit(armed ? host_->text("menu.slot.overwrite") : m->line, tw, 14 / 15.f),
                  armed ? kInkRed : kInkSoft, 14);
            label(r.right() - 16 - whenW, cy - 9, m->when, kInkSoft, 14);
        } else {
            const std::string e = host_->text(m->slot == 0 ? "menu.slot.auto_empty" : "menu.slot.empty");
            label(tx, cy - 10, e, kInkSoft.withAlpha(0.7f), 15);
        }
        ui_->claim(r);
        if (usable && in_->pressed && ui_->hovered(r)) {
            if (!slotsSave) host_->menuAction("load", {{"slot", m->slot}});
            else if (m->used && pendingSlot_ != m->slot) pendingSlot_ = m->slot;
            else {
                pendingSlot_ = -1;
                host_->menuAction("save", {{"slot", m->slot}});
            }
        }
        y += rowH;
    }
    if (!menuNote.empty()) label(P.x + 36, P.bottom() - 74, ui_->fit(menuNote, P.w - 72, 14 / 15.f), kInkRed, 14);
    const std::string back = host_->text("menu.back");
    const float bw = textW(back, 15) + 48;
    if (textButton({P.x + (P.w - bw) * 0.5f, P.bottom() - 52, bw, 34}, back, true)) {
        pendingSlot_ = -1;
        host_->menuAction("back", sim::Json::object());
    }
}

void Hud::settingsPanel(float W, float H) {
    dimmer(W, H, 0.66f);
    const float w = std::min(760.0f, W - 80), h = 470;
    const Rect P{(W - w) * 0.5f, (H - h) * 0.5f, w, h};
    box(Skin::Paper, P, 22);
    seal(P.right() - 44, P.y + 40);
    title(P.x + 36, P.y + 26, host_->text("menu.settings"), kInk, 34);
    ui_->line(P.x + 36, P.y + 84, P.right() - 36, P.y + 84, kInkSoft.withAlpha(0.35f));

    bool changed = false;
    float y = P.y + 106;
    const float lx = P.x + 36, cx0 = P.x + 36 + std::min(250.0f, P.w * 0.36f);
    auto row = [&](const std::string& name, const std::vector<std::pair<std::string, bool>>& chips) -> int {
        label(lx, y + 7, name, kInk, 16);
        float cx = cx0;
        int hit = -1;
        for (std::size_t i = 0; i < chips.size(); ++i) {
            const float cw = textW(chips[i].first, 15) + 30;
            if (textButton({cx, y, cw, 34}, chips[i].first, chips[i].second)) hit = int(i);
            cx += cw + 8;
        }
        y += 54;
        return hit;
    };
    const int l = row(host_->text("menu.settings.lang"), {{host_->text("menu.settings.lang.ru"), settingsLang == "ru"},
                                                          {host_->text("menu.settings.lang.en"), settingsLang == "en"}});
    if (l >= 0) {
        const std::string want = l == 0 ? "ru" : "en";
        changed = want != settingsLang;
        settingsLang = want;
    }
    const int f = row(host_->text("menu.settings.window"), {{host_->text("menu.settings.windowed"), !settingsFullscreen},
                                                            {host_->text("menu.settings.fullscreen"), settingsFullscreen}});
    if (f >= 0 && (f == 1) != settingsFullscreen) {
        settingsFullscreen = f == 1;
        changed = true;
    }
    static const int kAuto[] = {0, 5, 10, 15};
    std::vector<std::pair<std::string, bool>> ac;
    for (int m : kAuto)
        ac.push_back({m == 0 ? host_->text("menu.settings.off") : host_->text("menu.settings.minutes", {{"n", m}}),
                      settingsAutosave == m});
    const int a = row(host_->text("menu.settings.autosave"), ac);
    if (a >= 0 && kAuto[a] != settingsAutosave) {
        settingsAutosave = kAuto[a];
        changed = true;
    }
    // AL-62: the interface style (one theme for now) and its size
    std::vector<std::pair<std::string, bool>> sc;
    for (const auto& [id, nameKey] : themeList) sc.push_back({host_->text(nameKey), settingsStyle == id});
    if (sc.empty()) sc.push_back({host_->text("menu.settings.style.edge"), true});
    const int st = row(host_->text("menu.settings.style"), sc);
    if (st >= 0 && st < int(themeList.size()) && themeList[std::size_t(st)].first != settingsStyle) {
        settingsStyle = themeList[std::size_t(st)].first;
        changed = true;
    }
    static const int kScale[] = {80, 90, 100, 110, 120, 130};
    std::vector<std::pair<std::string, bool>> zc;
    for (int z : kScale) zc.push_back({host_->text("menu.settings.percent", {{"n", z}}), settingsScale == z});
    const int z = row(host_->text("menu.settings.ui_scale"), zc);
    if (z >= 0 && kScale[z] != settingsScale) {
        settingsScale = kScale[z];
        changed = true;
    }
    if (changed)
        host_->menuAction("apply", {{"lang", settingsLang}, {"fullscreen", settingsFullscreen}, {"autosave", settingsAutosave},
                                    {"style", settingsStyle}, {"ui_scale", settingsScale}});
    const std::string back = host_->text("menu.back");
    const float bw = textW(back, 15) + 48;
    if (textButton({P.x + (P.w - bw) * 0.5f, P.bottom() - 56, bw, 34}, back, true))
        host_->menuAction("back", sim::Json::object());
}

} // namespace altera::game
