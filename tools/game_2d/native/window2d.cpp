// AL-94: the 2D client in its own window (see window2d.hpp). A port of the
// page's map and play (tools/game_2d/web/game2d.js, play.js): ground chunks of
// 64x64 cells painted once into textures, things and pawns sorted by their
// foot, roofs lifted over the house one looks at, the walk's dots, names and
// what people say over their heads, night and rain; a side column of the
// households and the chosen one's card, a bar of the clock and the speeds; the
// player's person led by WASD / a click on the ground (A* over the cells),
// the act menu on RMB and the quick bar 1-8.
#include "native/window2d.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef near
#undef far
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <regex>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <SDL3/SDL.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <nlohmann/json.hpp>

#include "engine/ui/font.hpp"
#include "engine/ui/ui.hpp"
#include "native/atmos2d.hpp"
#include "native/ctx2d.hpp"
#include "native/pixels2d.hpp"
#include "sim/data_fs.hpp"

namespace game2d {

using Json = nlohmann::json;

void attachConsole(bool need) {
#ifdef _WIN32
    if (GetConsoleWindow()) return;
    // stdout already goes somewhere (a file, a pipe): keep it
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS) || (need && AllocConsole())) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        std::ios::sync_with_stdio();
    }
#else
    (void)need;
#endif
}

namespace {

constexpr int T = 16, CH = 64;
constexpr int kPage = 1024, kCell = 22;   // atlas pages, one picture a cell with a gap round it

double nowMs() { return double(SDL_GetTicks()); }

// LR-142: through the data layers (mods, the data folder beside the exe): a JSON file is the
// layered JSON, anything else the top layer's file.
std::string readText(const std::filesystem::path& p) {
    const std::string s = p.string();
    if (p.extension() == ".json" && lr::sim::data::exists(s)) {
        try {
            return lr::sim::data::readJson(s).dump();
        } catch (const std::exception& e) {
            std::cerr << "data: " << e.what() << "\n";
            return {};
        }
    }
    std::string out;
    lr::sim::data::readText(s, out);
    return out;
}

Json parseOr(const std::string& s, Json fallback) {
    try {
        return Json::parse(s);
    } catch (...) {
        return fallback;
    }
}

std::string str(const Json& j, const char* k) {
    const auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}
double num(const Json& j, const char* k, double d = 0) {
    const auto it = j.find(k);
    return it != j.end() && it->is_number() ? it->get<double>() : d;
}
// A list in `j` by reference (pointers into it stay good while `j` does); an empty one when there is none.
const Json& arr(const Json& j, const char* k) {
    static const Json empty = Json::array();
    const auto it = j.find(k);
    return it != j.end() && it->is_array() ? *it : empty;
}
bool flag(const Json& j, const char* k) {
    const auto it = j.find(k);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}
long long ident(const Json& j, const char* k) {
    const auto it = j.find(k);
    return it != j.end() && it->is_number() ? it->get<long long>() : -1;
}

ui::Colour col(Rgba c, float a = 1.0f) { return ui::Colour::rgb(int(c >> 16 & 0xff), int(c >> 8 & 0xff), int(c & 0xff), a); }
ui::Colour hexc(const char* h, float a = 1.0f) { return col(parseHex(h), a); }

// The frame's colours (tools/game_2d/web/style.css: "Резной рубеж").
const ui::Colour kWalnut = hexc("#3a2416"), kWalnutD = hexc("#24160c"), kBronze = hexc("#b08a4a"),
                 kBronzeL = hexc("#e0c080"), kLeather = hexc("#4a2e1c"), kLeatherD = hexc("#2e1c10"),
                 kParch = hexc("#e8d8b0"), kInk = hexc("#3a2414"), kGold = hexc("#e8c060"), kText = hexc("#efe2c4"),
                 kDim = hexc("#b8a07a"), kBad = hexc("#b83a28"), kMid = hexc("#c89a30"), kGood = hexc("#5a9a3a"),
                 kPlayer = hexc("#c03028"), kOut = hexc("#1a1410");

ui::Colour tone(double v) { return v < 0.3 ? kBad : v < 0.6 ? kMid : kGood; }

// --- the atlas: every small picture once on the card --------------------------
struct Sprite {
    SDL_Texture* tex = nullptr;
    SDL_FRect src{};
    explicit operator bool() const { return tex != nullptr; }
};

class Atlas {
public:
    void attach(SDL_Renderer* r) { ren_ = r; }
    void clear() {
        for (SDL_Texture* t : pages_) SDL_DestroyTexture(t);
        pages_.clear();
        map_.clear();
        next_ = 0;
    }
    const Sprite* find(const std::string& key) const {
        const auto it = map_.find(key);
        return it == map_.end() ? nullptr : &it->second;
    }
    Sprite put(const std::string& key, const Pixels& p) {
        Sprite s;
        if (p.size <= 0 || p.size > kCell - 2) {
            map_[key] = s;
            return s;
        }
        const int perRow = kPage / kCell, perPage = perRow * perRow;
        if (pages_.empty() || next_ >= perPage) {
            SDL_Texture* t = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, kPage, kPage);
            if (!t) return s;
            SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
            std::vector<Rgba> zero(std::size_t(kPage) * kPage, 0);
            SDL_UpdateTexture(t, nullptr, zero.data(), kPage * 4);
            pages_.push_back(t);
            next_ = 0;
        }
        const int cx = (next_ % perRow) * kCell + 1, cy = (next_ / perRow) * kCell + 1;
        ++next_;
        const SDL_Rect r{cx, cy, p.size, p.size};
        SDL_UpdateTexture(pages_.back(), &r, p.px.data(), p.size * 4);
        s.tex = pages_.back();
        s.src = {float(cx), float(cy), float(p.size), float(p.size)};
        map_[key] = s;
        return s;
    }

private:
    SDL_Renderer* ren_ = nullptr;
    std::vector<SDL_Texture*> pages_;
    std::unordered_map<std::string, Sprite> map_;
    int next_ = 0;
};

struct Chunk {
    Json j;
    std::string body;
    SDL_Texture* tex = nullptr;
    bool painted = false, glyph = false;
    double at = 0;
};

struct Vis {
    double x = 0, y = 0;
    char face = 'S';
    int bob = 0;
    bool moving = false, driven = false;
    double walked = 0;   // LR-139: metres walked, the step of the walk cycle
};

struct Bubble {
    std::string line;
    double until = 0;
    bool bark = false;
};

struct Slot {
    std::string kind, id;   // kind "act" | "goal"; empty: an empty slot
    bool target = false;
};

std::string utf8Cut(const std::string& s, std::size_t maxCodepoints) {
    std::size_t at = 0, n = 0;
    while (at < s.size() && n < maxCodepoints) {
        ui::nextCodepoint(s, at);
        ++n;
    }
    return at >= s.size() ? s : s.substr(0, at) + "…";
}

} // namespace

struct Window::Impl : Ctx {
    std::filesystem::path root;
    std::string err;
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    ui::Ui ui;
    std::shared_ptr<ui::FontFace> fontFace, fontBold;
    bool pixelFont = false;
    Atlas atlas;
    Atmos atmos;             // AL-110: light, shadows, weather
    Host* host = nullptr;
    float U = 1.0f;          // the interface's scale (the display's)
    int W = 1280, H = 720;   // the output in pixels
    float mouseK = 1.0f;     // window coordinates -> output pixels

    // --- the game as the routes tell it ---------------------------------------
    Json ST, MAP, RES, LOOK;
    std::map<std::string, std::string> UI;
    SpriteSet sprites;
    std::vector<std::pair<std::regex, std::string>> actsRe;
    std::unordered_set<std::string> hidden;
    std::unordered_map<std::string, Chunk> chunks;
    int chunkVersion = 0;
    std::vector<Json> people;
    std::unordered_map<long long, std::size_t> byId;
    std::unordered_map<long long, Vis> vis;
    Json clock = Json::object();
    bool paused = false;
    double tps = 2;
    long long selId = -1, hoverId = -1, me = -1;
    Json selCard;
    Json meCard;                       // LR-137: the led person while another is selected
    Json HUD;                          // content/sprites_2d/hud2d.json
    std::set<std::string> folded{"tier.safety", "tier.esteem", "tier.purpose", "tier.spirit", "tier.other"};
    std::string tipText;               // what the pointer is over in the HUD (shown in the bar)
    bool haveHover = false;
    double hoverX = 0, hoverY = 0;
    double camX = 0, camY = 0;
    int zoom = 2;
    bool follow = false, glyphs = false, altDown = false, fullscreen = false;
    std::string roofMode = "auto";
    double lastFrame = 0, lastPoll = -1e9, lastBar = -1e9;
    std::uint64_t dataGen = 0;   // LR-142: the data generation the sprites and the map rules were read at
    bool selectedMe = false;

    // --- leading ------------------------------------------------------------------
    std::vector<std::pair<double, double>> path;
    double lastSteer = 0, lastMove = 0;
    bool haveSteer = false;
    double steerX = 0, steerY = 0;
    double markerX = 0, markerY = 0, markerAt = -1e9;
    std::unordered_set<long long> walls;   // wall lines of the ground storey
    int wallsVersion = -1;
    std::array<Slot, 8> slots{};
    Json checked = Json::array();
    long long barFor = -2;

    // --- interface ------------------------------------------------------------------
    std::string toastText;
    double toastUntil = 0;
    std::unordered_map<long long, Bubble> bubbles;
    std::deque<std::pair<std::string, double>> lines;
    bool menuOpen = false;
    float menuX = 0, menuY = 0;
    std::string menuTitle;
    long long menuTarget = -1;
    Json menuActs = Json::array();
    ui::Input in;
    std::unique_ptr<Screens> screens;
    bool dragging = false;
    float dragX = 0, dragY = 0;
    double dragCamX = 0, dragCamY = 0;
    bool clickedWorld = false, rclickedWorld = false;
    float clickX = 0, clickY = 0;
    bool shift = false;

    // --- shots (--shot) --------------------------------------------------------------
    std::string shotPath;
    double shotAfter = 6.0;
    int selectIndex = -1;

    // ======================================================================
    std::string L(const std::string& k, const std::map<std::string, std::string>& args = {}) const override {
        const auto it = UI.find(k);
        std::string s = it == UI.end() ? k : it->second;
        for (const auto& [a, v] : args) {
            const std::string tag = "{" + a + "}";
            for (std::size_t p = s.find(tag); p != std::string::npos; p = s.find(tag, p + v.size())) s.replace(p, tag.size(), v);
        }
        return s;
    }
    Json get(const std::string& path) override {
        int code = 0;
        const std::string b = host->call("GET", path, "", &code);
        return code == 200 ? parseOr(b, Json()) : Json();
    }
    Json post(const std::string& path, const Json& body) override {
        int code = 0;
        const std::string b = host->call("POST", path, body.dump(), &code);
        return code == 200 ? parseOr(b, Json()) : Json();
    }
    Json cmd(const Json& body) override { return post("/api/cmd", body); }
    void toast(const std::string& t) override {
        if (t.empty()) return;
        toastText = t;
        toastUntil = nowMs() + 3200;
    }
    void cmdToast(const Json& body) override {
        const Json r = cmd(body);
        const std::string res = str(r, "result");
        if (!res.empty() && res != "ok") toast(str(r, "text").empty() ? res : str(r, "text"));
    }

    // --- what the screens use (ctx2d.hpp) ----------------------------------------------
    ui::Ui& gui() override { return ui; }
    ui::Input& input() override { return in; }
    float u() const override { return U; }
    float screenW() const override { return float(W); }
    float screenH() const override { return float(H); }
    std::string fit(const std::string& s, float width, float px) const override { return ui.fit(s, width, fs(px)); }
    bool spriteAt(const std::string& name, const Json& recolour, float x, float y, float size, float alpha) override {
        const Sprite sp = sprite(name, recolour);
        if (!sp) return false;
        blit(sp, x, y, size, size, alpha);
        return true;
    }
    void pawn(const Json& p, char side, float x, float y, float size) override {   // AL-136
        blit(pawnSprite(pawnSpec(p), side, 0), x, y, size, size);
    }
    void face(long long id, const std::string& name, float x, float y, float size) override {
        if (const Json* p = person(id)) {
            portrait(*p, x, y, size);
            return;
        }
        fill(x, y, size, size, kLeatherD);
        frame(x, y, size, size, kBronze, 1);
        std::size_t at = 0;
        ui::nextCodepoint(name, at);
        textC(x + size / 2, y + size / 2 - textH(size * 0.5f) / 2, name.substr(0, at), kGold, size * 0.5f);
    }
    const Json& statics() const override { return ST; }
    const Json& map2d() const override { return MAP; }
    const Json& craftLook() const override { return LOOK; }
    void refreshMap() override {
        for (auto& [k, c] : chunks) c.at = 0;
    }
    const Json& resources() const override { return RES; }
    void walkTo(double x, double y) override {
        if (me < 0 || !vis.count(me)) return;
        rebuildWalls();
        std::vector<std::pair<double, double>> found;
        if (findPath(vis[me].x, vis[me].y, x, y, found)) {
            path = found;
            markerX = std::floor(x) + 0.5, markerY = std::floor(y) + 0.5, markerAt = nowMs();
        } else toast(L("ui2d.no_way"));
    }
    const std::vector<Json>& peopleList() const override { return people; }
    long long meId() const override { return me; }
    long long selected() const override { return selId; }
    void lookAt(double x, double y) override {
        camX = x;
        camY = y;
        follow = false;
    }
    void setFollow(bool on) override { follow = on; }
    void refresh() override { poll(); }
    bool isPaused() const override { return paused; }
    void setPaused(bool on) override {
        paused = on;
        cmd({{"op", "pause"}, {"on", on}});
    }
    double camAtX() const override { return camX; }
    double camAtY() const override { return camY; }
    int zoomLevel() const override { return zoom; }
    void quitGame() override { post("/api/quit", {{"autosave", true}}); }
    // a new world in play: the map, the people, the camera start over
    void worldChanged(const Json& view) override {
        for (auto& [k, c] : chunks)
            if (c.tex) SDL_DestroyTexture(c.tex);
        chunks.clear();
        ++chunkVersion;
        people.clear();
        byId.clear();
        vis.clear();
        bubbles.clear();
        lines.clear();
        path.clear();
        selId = me = -1;
        selCard = Json();
        selectedMe = false;
        menuOpen = false;
        barFor = -2;
        UI.clear();
        actsRe.clear();
        hidden.clear();
        camX = camY = 0;
        loadStatic();
        if (view.is_object() && view.contains("x") && view["x"].is_number()) {
            camX = view["x"].get<double>();
            camY = view.value("y", camY);
        }
        if (view.is_object() && view.contains("zoom") && view["zoom"].is_number()) zoom = std::clamp(view["zoom"].get<int>(), 1, 3);
        poll();
    }

    // --- view math ---------------------------------------------------------------------
    float mapW() const override { return float(W) - sideW(); }
    float mapH() const override { return float(H) - barH(); }
    float sideW() const { return std::round(300 * U); }
    float barH() const { return std::round(32 * U); }
    double dpr() const { return std::max(1.0, std::round(double(U))); }
    double scale() const { return zoom * dpr(); }
    double tilePx() const override { return T * scale(); }
    void toScreen(double wx, double wy, float& sx, float& sy) const override {
        sx = float(std::round((wx - camX) * tilePx() + mapW() / 2));
        sy = float(std::round((wy - camY) * tilePx() + mapH() / 2));
    }
    void toWorld(double sx, double sy, double& wx, double& wy) const override {
        wx = (sx - mapW() / 2) / tilePx() + camX;
        wy = (sy - mapH() / 2) / tilePx() + camY;
    }
    const Json* person(long long id) const override {
        const auto it = byId.find(id);
        return it == byId.end() ? nullptr : &people[it->second];
    }

    // --- drawing primitives --------------------------------------------------------------
    void fill(float x, float y, float w, float h, const ui::Colour& c) override {
        SDL_SetRenderDrawColorFloat(ren, c.r, c.g, c.b, c.a);
        const SDL_FRect r{x, y, w, h};
        SDL_RenderFillRect(ren, &r);
        ui.ink({x, y, w, h});
    }
    void frame(float x, float y, float w, float h, const ui::Colour& c, float t) override {
        fill(x, y, w, t, c);
        fill(x, y + h - t, w, t, c);
        fill(x, y, t, h, c);
        fill(x + w - t, y, t, h, c);
    }
    void ellipse(float cx, float cy, float rx, float ry, const ui::Colour& c) {
        SDL_SetRenderDrawColorFloat(ren, c.r, c.g, c.b, c.a);
        for (int dy = int(-ry); dy <= int(ry); ++dy) {
            const float k = 1.0f - float(dy * dy) / (ry * ry);
            if (k < 0) continue;
            const float hw = rx * std::sqrt(k);
            const SDL_FRect r{cx - hw, cy + float(dy), hw * 2, 1};
            SDL_RenderFillRect(ren, &r);
        }
    }
    void ring(float cx, float cy, float rx, float ry, float t, const ui::Colour& c) {
        SDL_SetRenderDrawColorFloat(ren, c.r, c.g, c.b, c.a);
        const int n = 64;
        for (int i = 0; i < n; ++i) {
            const double a = i * 2 * 3.14159265358979 / n;
            const SDL_FRect r{float(cx + rx * std::cos(a) - t / 2), float(cy + ry * std::sin(a) - t / 2), t, t};
            SDL_RenderFillRect(ren, &r);
        }
    }
    // LR-137: the pixel skin. One art pixel is px() screen pixels (2 at 1080p): frames,
    // bevels, gauges and the 9x9 icons of hud2d.json are drawn on that grid.
    float px() const { return std::max(1.0f, std::round(2 * U)); }
    // specks of darker and lighter wood, fixed to the screen grid so they do not crawl
    void grain(const ui::Rect& r) {
        const float p = px(), cell = 8 * p;
        static const ui::Colour dark = hexc("#2c1a0f"), light = hexc("#46301e");
        for (float y = std::floor(r.y / cell) * cell; y < r.bottom(); y += cell)
            for (float x = std::floor(r.x / cell) * cell; x < r.right(); x += cell) {
                unsigned h = unsigned(int(x / p)) * 73856093u ^ unsigned(int(y / p)) * 19349663u;
                h ^= h >> 13;
                h *= 0x5bd1e995u;
                h ^= h >> 15;
                const float gx = x + float(h % 7) * p, gy = y + float((h >> 4) % 7) * p, gw = float(1 + (h >> 8) % 3) * p;
                if (gx < r.x || gy < r.y || gx + gw > r.right() || gy + p > r.bottom()) continue;
                fill(gx, gy, gw, p, (h >> 11) & 1 ? dark : light);
            }
    }
    // a carved frame: an outline, a bronze bevel lit from the top left, a dark seam, grained walnut
    void carved(const ui::Rect& r) override {
        const float p = px();
        fill(r.x, r.y, r.w, r.h, kOut);
        fill(r.x + p, r.y + p, r.w - 2 * p, r.h - 2 * p, kBronze);
        fill(r.x + p, r.y + p, r.w - 2 * p, p, kBronzeL);
        fill(r.x + p, r.y + p, p, r.h - 2 * p, kBronzeL);
        fill(r.x + 2 * p, r.y + 2 * p, r.w - 4 * p, r.h - 4 * p, kOut);
        fill(r.x + 3 * p, r.y + 3 * p, r.w - 6 * p, r.h - 6 * p, kWalnut);
        grain({r.x + 3 * p, r.y + 3 * p, r.w - 6 * p, r.h - 6 * p});
        ui.claim(r);
    }
    float rim() const { return 3 * px(); }   // how thick carved() is
    // a small plate (party frames, slots): an outline, leather with a one-pixel bevel
    float pixel() const override { return px(); }
    const Json& hud() const override { return HUD; }
    void plate(const ui::Rect& r, bool lit = false) override {
        const float p = px();
        fill(r.x, r.y, r.w, r.h, lit ? kGold : kOut);
        fill(r.x + p, r.y + p, r.w - 2 * p, r.h - 2 * p, kLeather);
        fill(r.x + p, r.y + p, r.w - 2 * p, p, kBronze.withAlpha(0.7f));
        fill(r.x + p, r.y + p, p, r.h - 2 * p, kBronze.withAlpha(0.7f));
        fill(r.x + 2 * p, r.y + 2 * p, r.w - 3 * p, r.h - 3 * p, kLeather);
        ui.claim(r);
    }
    // a sunk well (portraits, icon slots, fold rows): dark, shadowed at the top left
    void well(const ui::Rect& r) override {
        const float p = px();
        fill(r.x, r.y, r.w, r.h, kOut);
        fill(r.x + p, r.y + p, r.w - 2 * p, r.h - 2 * p, kLeatherD);
        fill(r.x + p, r.y + r.h - 2 * p, r.w - 2 * p, p, kLeather);
        fill(r.x + r.w - 2 * p, r.y + p, p, r.h - 2 * p, kLeather);
    }
    void parchBox(const ui::Rect& r) override {
        const float p = px();
        static const ui::Colour rimC = hexc("#c8b488");
        fill(r.x - p, r.y - p, r.w + 2 * p, r.h + 2 * p, kOut);
        fill(r.x, r.y, r.w, r.h, rimC);
        fill(r.x + p, r.y + p, r.w - 2 * p, r.h - 2 * p, kParch);
    }
    float fs(float px) const { return px / (15.0f * U); }   // a text scale for a size in pixels
    void text(float x, float y, const std::string& s, const ui::Colour& c, float px, bool shadow = false) override {
        if (shadow) ui.text(x + std::max(1.0f, U), y + std::max(1.0f, U), s, ui::Colour(0, 0, 0, c.a), fs(px));
        ui.text(x, y, s, c, fs(px));
    }
    float textW(const std::string& s, float px) const override { return ui.textWidth(s, fs(px)); }
    float textH(float px) const override { return ui.textHeight(fs(px)); }
    void textC(float cx, float y, const std::string& s, const ui::Colour& c, float px, bool shadow = false) override {
        text(cx - textW(s, px) / 2, y, s, c, px, shadow);
    }
    // a gauge in pips: an outline, segments of four art pixels, the filled ones with a
    // darker lower edge. A gauge narrower than two pips is one plain bar.
    void gauge(const ui::Rect& r, double v, const ui::Colour& c) override {
        const float p = px();
        const float h = std::max(r.h, 4 * p);
        const ui::Rect b{std::round(r.x), std::round(r.y + (r.h - h) / 2), std::round(r.w), std::round(h)};
        fill(b.x, b.y, b.w, b.h, kOut);
        const float iw = b.w - 2 * p, ih = b.h - 2 * p;
        if (iw <= 0 || ih <= 0) return;
        fill(b.x + p, b.y + p, iw, ih, kLeatherD);
        const int n = std::max(1, int(std::lround(iw / (4 * p))));
        const float seg = iw / float(n);
        const double vv = std::clamp(v, 0.0, 1.0);
        const int on = n >= 2 ? int(std::lround(vv * n)) : 0;
        const float fw = n >= 2 ? seg * float(on) : float(iw * vv);
        if (fw > 0) {
            fill(b.x + p, b.y + p, fw, ih, c);
            fill(b.x + p, b.y + p + ih - p, fw, p, ui::Colour(c.r * 0.6f, c.g * 0.6f, c.b * 0.6f, c.a));
            fill(b.x + p, b.y + p, fw, p, ui::Colour(std::min(1.0f, c.r * 1.25f), std::min(1.0f, c.g * 1.25f), std::min(1.0f, c.b * 1.25f), c.a));
        }
        if (n >= 2)
            for (int i = 1; i < n; ++i) fill(std::round(b.x + p + seg * float(i)) - p / 2, b.y + p, p, ih, kOut);
    }
    // a gauge of `n` pips at (x, y); its width
    float pips(float x, float y, int n, double v, const ui::Colour& c) override {
        const float p = px(), w = float(n) * 4 * p + 2 * p;
        gauge({x, y, w, 5 * p}, v, c);   // 5 art pixels: the bevels leave a lit middle
        return w;
    }
    // a hud2d.json icon (9x9 art pixels) at (x, y)
    float iconSize() const override { return 9 * px(); }
    void icon(const std::string& name, float x, float y, float size = 0) override {
        if (name.empty()) return;
        const float s = size > 0 ? size : iconSize();
        blit(sprite(name), std::round(x), std::round(y), s, s);
    }

    // --- pictures ------------------------------------------------------------------------
    Sprite sprite(const std::string& name, const Json& recolour = Json()) {
        std::string k = "s:" + name;
        std::map<char, char> rc;
        if (recolour.is_object())
            for (const auto& [a, b] : recolour.items())
                if (!a.empty() && b.is_string() && !b.get<std::string>().empty()) {
                    rc[a[0]] = b.get<std::string>()[0];
                    k += "|" + a + b.get<std::string>();
                }
        if (const Sprite* s = atlas.find(k)) return *s;
        return atlas.put(k, sprites.get(name, rc));
    }
    Sprite pawnSprite(const PawnSpec& spec, char side, int bob) {
        PawnPose pose;
        pose.bob = bob;
        return pawnSprite(spec, side, pose);
    }
    Sprite pawnSprite(const PawnSpec& spec, char side, const PawnPose& pose) {
        const std::string k = "p:" + spec.key() + side + pose.key();
        if (const Sprite* s = atlas.find(k)) return *s;
        return atlas.put(k, pawnPixels(spec, side, pose));
    }
    // LR-139: the frame of the pawn's walk, work or idle breath now
    PawnPose poseOf(const Json& p, const Vis& v) const {
        return pawnPose(v.moving, v.walked, str(p, "st") == "Work", nowMs(), ident(p, "id"));
    }
    // LR-139: a person's height against the kind's: each one a little taller or shorter
    static float heightK(const Json& p) {
        const std::uint32_t h = idHash(num(p, "id"), 11);
        return 0.94f + float(h % 13) * 0.01f;
    }
    Sprite beastSprite(const std::string& sp, char face) {
        const std::string k = std::string("b:") + sp + face;
        if (const Sprite* s = atlas.find(k)) return *s;
        return atlas.put(k, beastPixels(sp, face));
    }
    void blit(const Sprite& s, float x, float y, float w, float h, float alpha = 1.0f, double angle = 0) {
        if (!s) return;
        SDL_SetTextureAlphaModFloat(s.tex, alpha);
        ui.ink({x, y, w, h});
        const SDL_FRect d{x, y, w, h};
        if (angle != 0) SDL_RenderTextureRotated(ren, s.tex, &s.src, &d, angle, nullptr, SDL_FLIP_NONE);
        else SDL_RenderTexture(ren, s.tex, &s.src, &d);
        if (alpha != 1.0f) SDL_SetTextureAlphaModFloat(s.tex, 1.0f);
    }

    // --- pawns from what the sim says the person wears and holds ------------------------
    std::string famColour(const Json& p) const {
        if (p.contains("look") && p["look"].is_object() && !str(p["look"], "cloth").empty()) return str(p["look"], "cloth");   // AL-136
        const Json list = MAP.value("family_colours", Json::array({"#8a6a48"}));
        if (!list.is_array() || list.empty()) return "#8a6a48";
        return list[idHash(std::max(0.0, num(p, "famId")), 3) % list.size()].get<std::string>();
    }
    // LR-139: an entry of a list in pawns2d.json npc (the fallback when the data has none)
    static std::string npcPick(const char* key, std::uint32_t k, const char* fallback) {
        const Json& npc = pawnParts().contains("npc") ? pawnParts()["npc"] : Json::object();
        if (!npc.contains(key) || !npc[key].is_array() || npc[key].empty()) return fallback;
        const Json& v = npc[key][k % npc[key].size()];
        return v.is_string() ? v.get<std::string>() : fallback;
    }
    static double npcNum(const char* key, double fallback) {
        const Json& P = pawnParts();
        if (!P.contains("npc") || !P["npc"].contains(key) || !P["npc"][key].is_number()) return fallback;
        return P["npc"][key].get<double>();
    }
    PawnSpec pawnSpec(const Json& p) const {
        // how the people one meets look (pawns2d.json npc: bodies, skins, hair colours, beards, kerchiefs)
        const std::uint32_t h = idHash(num(p, "id"), 1);
        const double age = num(p, "age", 30);
        const bool child = age < npcNum("child_age", 13), f = str(p, "sex") == "f";
        PawnSpec spec;
        spec.child = child;
        spec.body = f ? npcPick("bodies_f", h, "fem") : npcPick("bodies_m", h, "norm");
        spec.skin = npcPick("skins", h >> 3, "light");
        if (!str(p, "rskin").empty()) spec.skin = str(p, "rskin");   // AL-114: the kind's skin
        if (flag(p, "stone")) spec.skin = npcPick("stone", 0, "#8c8c86");   // a troll asleep as stone
        // LR-139: the kind's look (species body.pawn): ears, head, nose, brow, eyes, tusks, build
        std::string beardRule, hairRule, hairSet, beardSet;
        if (p.contains("rlook") && p["rlook"].is_object()) {
            const Json& rl = p["rlook"];
            spec.ears = str(rl, "ears");
            spec.head = str(rl, "head");
            spec.nose = str(rl, "nose");
            spec.brow = str(rl, "brow");
            spec.eyes = str(rl, "eyes");
            spec.tusks = str(rl, "tusks");
            spec.build = child ? "" : str(rl, "build");
            spec.skinPat = str(rl, "skin_pattern");
            beardRule = str(rl, "beard");
            hairRule = str(rl, "hair");
            hairSet = str(rl, f ? "hair_f" : "hair_m");   // LR-139: the kind's cuts and beards
            beardSet = str(rl, "beards");
            if (child && spec.tusks == "yes") spec.tusks = "small";
        }
        std::string hc = npcPick("hair_colours", h >> 6, "#5a3a20");
        if (age >= npcNum("grey_age", 55)) hc = npcPick("grey", 0, "#b0b0a8");
        spec.hair = {f ? npcPick("hair_f", h >> 9, "long") : npcPick("hair_m", h >> 9, "short"), hc};
        const int beardIn = std::max(1, int(npcNum("beard_one_in", 3)));   // all but one in N grown men
        if (!f && !child && age >= npcNum("beard_age", 18) && ((h >> 12) % beardIn)) spec.beard = true;
        if (beardRule == "never") spec.beard = false;
        if (beardRule == "rare") spec.beard = spec.beard && (h >> 18) % std::max(1, int(npcNum("rare_one_in", 4))) == 0;
        if (beardRule == "long" && !f && !child) spec.beard = spec.longBeard = true;   // a dwarf's pride
        if (hairRule == "none") spec.hair.first = "none";
        if (hairRule == "tuft") spec.hair.first = "tuft";
        auto pick = [](const std::string& set, std::uint32_t k) {
            std::vector<std::string> v;
            std::istringstream in(set);
            for (std::string w; in >> w;) v.push_back(w);
            return v.empty() ? std::string() : v[k % v.size()];
        };
        const std::uint32_t h2 = idHash(num(p, "id"), 5);
        if (!child)
            if (const std::string cut = pick(hairSet, h2); !cut.empty()) spec.hair.first = cut;
        if (spec.beard) {
            const std::string bs = pick(beardSet, h2 >> 8);
            if (bs.empty() && beardRule != "long") spec.beard = false;   // a kind without beards
            else spec.beardStyle = bs;
        }
        // AL-136: the look chosen at the character's creation
        if (p.contains("look") && p["look"].is_object()) {
            const Json& lk = p["look"];
            if (!str(lk, "body").empty() && !child) spec.body = str(lk, "body");
            if (!str(lk, "skin").empty() && !flag(p, "stone")) spec.skin = str(lk, "skin");
            if (!str(lk, "hair").empty()) spec.hair.first = str(lk, "hair");
            if (!str(lk, "hair_colour").empty()) spec.hair.second = str(lk, "hair_colour");
            if (const std::string b = str(lk, "beard"); !b.empty()) {
                spec.beard = !f && !child && b != "no";
                spec.beardStyle = b == "yes" || b == "no" ? "" : b;   // LR-139: a style, or the old yes = full
                if (spec.beardStyle == "long") spec.longBeard = true;
                else spec.longBeard = false;
            }
        }
        std::vector<std::pair<std::string, std::string>> wear;
        std::set<std::string> kinds;
        const Json& W = MAP.contains("wear") ? MAP["wear"] : Json::object();
        if (p.contains("worn") && p["worn"].is_array())
            for (const auto& it : p["worn"]) {
                if (!it.is_string() || !W.contains(it.get<std::string>())) continue;
                const std::string id = it.get<std::string>();
                const Json& w = W[id];
                // AL-109: a woman's tunic is a gown, "family" is the household's colour, a dyed thing its dye's
                const std::string layer = f && w.contains("layer_f") ? str(w, "layer_f") : str(w, "layer");
                std::string c = str(w, "colour");
                if (c == "family") c = famColour(p);
                if (p.contains("dye") && p["dye"].is_object() && p["dye"].contains(id) && MAP.contains("dyes"))
                    c = MAP["dyes"].value(p["dye"][id].get<std::string>(), c);
                kinds.insert(layer);
                if (layer == "cloak") spec.cloak = c;
                else if (layer == "cap" || layer == "helmet" || layer == "hood") spec.hat = {layer, c};
                else wear.push_back({layer, c});
            }
        // LR-139: what the sim says is worn, nothing more: without a torso thing the pawn is bare
        spec.naked = !kinds.count("shirt") && !kinds.count("robe") && !kinds.count("mail") && !kinds.count("tabard");
        static const std::vector<std::string> order = {"shirt", "robe", "mail", "pants", "boots", "tabard", "apron", "belt", "bag", "pack"};
        auto rank = [&](const std::string& k) {
            const auto it = std::find(order.begin(), order.end(), k);
            return it == order.end() ? -1 : int(it - order.begin());
        };
        std::stable_sort(wear.begin(), wear.end(), [&](const auto& a, const auto& b) { return rank(a.first) < rank(b.first); });
        spec.wear = wear;
        if (spec.hat.first.empty() && f && !child && !spec.naked && ((h >> 15) % 100) < npcNum("kerchief_pct", 40))
            spec.hat = {"kerchief", npcPick("kerchief", 0, "#e8e0d0")};
        // AL-109: the sim says what is in hand now (the act's tool, else a carried weapon)
        const std::string held = str(p, "held");
        if (MAP.contains("tools") && !held.empty())
            for (const auto& pt : MAP["tools"])
                if (held.find(pt[0].get<std::string>()) != std::string::npos) {
                    spec.tool = pt[1].get<std::string>();
                    break;
                }
        return spec;
    }
    std::string actIcon(const Json& p) const {
        if (!flag(p, "alive")) return "";
        if (str(p, "st") == "Sleep") return "ic_sleep";
        const std::string ia = str(p, "ia");
        for (const auto& [re, sp] : actsRe)
            if (std::regex_search(ia, re)) return sp;
        return "";
    }
    const Json& thingLook(const std::string& type) const {
        static const Json none = Json::object();
        const Json& mo = MAP.contains("objects") ? MAP["objects"] : none;
        if (mo.contains(type)) return mo[type];
        if (RES.is_object() && RES.contains("objects") && RES["objects"].contains(type)) return RES["objects"][type];
        return mo.contains("_default") ? mo["_default"] : none;
    }

    // ======================================================================
    // start
    bool open() {
        SDL_SetMainReady();
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d12,direct3d11,opengl,software");
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
            err = SDL_GetError();
            return false;
        }
        win = SDL_CreateWindow("Lively Realms", 1600, 900, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
        if (!win) {
            err = SDL_GetError();
            return false;
        }
        ren = SDL_CreateRenderer(win, nullptr);
        if (!ren) {
            err = SDL_GetError();
            return false;
        }
        SDL_SetRenderVSync(ren, 1);
        std::cout << "renderer: " << SDL_GetRendererName(ren) << std::endl;
        atlas.attach(ren);
        atmos.attach(ren);
        ui.init(ren);
        // LR-137: a pixel font (Tiny5, OFL, full Cyrillic) to go with the pixel frames; Kurale when it is missing.
        // One weight: the pixel face reads bold already, titles differ by colour and size.
        fontFace = ui::FontFace::load(root / "assets/fonts/Tiny5-Regular.ttf");
        pixelFont = fontFace != nullptr;
        if (!fontFace) fontFace = ui::FontFace::load(root / "assets/fonts/Kurale-Regular.ttf");
        fontBold = fontFace;
        if (const char* sz = SDL_getenv("LR_2D_SIZE")) {   // shots: the window's size, "1920x1080"
            int ww = 0, wh = 0;
            if (std::sscanf(sz, "%dx%d", &ww, &wh) == 2 && ww > 0 && wh > 0) SDL_SetWindowSize(win, ww, wh);
        }
        measure();
        SDL_MaximizeWindow(win);
        SDL_ShowWindow(win);
        loadingFrame("");
        return true;
    }
    void measure() {
        SDL_GetRenderOutputSize(ren, &W, &H);
        int ww = 1, wh = 1;
        SDL_GetWindowSize(win, &ww, &wh);
        mouseK = ww > 0 ? float(W) / float(ww) : 1.0f;
        U = std::clamp(SDL_GetWindowDisplayScale(win), 1.0f, 3.0f);
        if (fontFace) ui.setFont(fontFace, fontBold ? fontBold : fontFace, 15.0f * U, 1.0f);
        skin();
    }
    // LR-137: the engine's buttons, toggles and scroll bars in the pixel frame's colours
    void skin() {
        ui::Theme& t = ui.theme();
        t.pixel = px();
        t.fontGrid = pixelFont ? 8 : 0;   // Tiny5: an 8-pixel em
        t.bevelLight = kBronzeL.withAlpha(0.55f);
        t.bevelDark = kOut;
        t.slot = kLeather;
        t.slotHot = hexc("#5a3a24");
        t.slotDown = kLeatherD;
        t.slotEdge = kOut;
        t.accent = kGold;
        t.label = kText;
        t.labelSoft = kDim;
        t.paper = kParch;
        t.paperEdge = kOut;
        t.ink = kInk;
    }
    void loadingFrame(const std::string& why) {
        for (int i = 0; i < 2; ++i) {
            SDL_PumpEvents();
            measure();
            SDL_SetRenderDrawColorFloat(ren, 0.07f, 0.04f, 0.02f, 1);
            SDL_RenderClear(ren);
            ui::Input none;
            ui.begin(none, W, H);
            textC(W / 2.0f, H / 2.0f - 60 * U, "LivelyRealms", kGold, 40 * U, true);
            if (why.empty()) textC(W / 2.0f, H / 2.0f + 10 * U, "…", kText, 22 * U, true);
            else ui.paragraph(W / 2.0f - 300 * U, H / 2.0f, 600 * U, why, kBad, fs(16 * U));
            ui.end();
            SDL_RenderPresent(ren);
        }
    }
    void fail(const std::string& why) {
        if (!ren) return;
        for (;;) {
            SDL_Event e;
            while (SDL_PollEvent(&e))
                if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
                    (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE))
                    return;
            loadingFrame(why);
            SDL_Delay(30);
        }
    }

    void loadStatic() {
        ST = get("/api/static");
        if (!ST.is_object()) ST = Json::object();
        const Json strings = ST.value("ui", Json::object());   // kept: items() of a temporary dangles
        for (const auto& [k, v] : strings.items())
            if (v.is_string()) UI[k] = v.get<std::string>();
        const auto dir = root / "content/sprites_2d";
        sprites.load(parseOr(readText(dir / "sprites.json"), Json::object()));
        MAP = parseOr(readText(dir / "map2d.json"), Json::object());
        loadPawnParts();   // LR-139: hair, beards, ears, builds... as data
        RES = parseOr(readText(dir / "resources.json"), Json());
        LOOK = parseOr(readText(dir / "craft2d.json"), Json::object());
        atmos.configure(parseOr(readText(dir / "sky2d.json"), Json::object()));
        sprites.merge(LOOK);
        sprites.merge(parseOr(readText(dir / "crops2d.json"), Json::object()));   // AL-112: fields and gardens
        HUD = parseOr(readText(dir / "hud2d.json"), Json::object());   // LR-137: the pixel HUD
        sprites.merge(HUD);
        for (const auto& a : MAP.value("acts", Json::array()))
            if (a.is_array() && a.size() >= 2) try {
                    actsRe.push_back({std::regex(a[0].get<std::string>(), std::regex::ECMAScript), a[1].get<std::string>()});
                } catch (...) {
                }
        for (const auto& h : MAP.value("hidden", Json::array()))
            if (h.is_string()) hidden.insert(h.get<std::string>());
        SDL_SetWindowTitle(win, L("ui2d.title").c_str());
        std::cout << "window: static " << ST.size() << " keys, " << UI.size() << " strings, map " << MAP.size() << " keys" << std::endl;
        for (const auto& s : ST.value("settlements", Json::array()))
            if (str(s, "mode") == "full" || camX == 0) {
                camX = num(s, "x");
                camY = num(s, "y");
                if (str(s, "mode") == "full") break;
            }
        slots = {Slot{"act", "chat", true}, Slot{"goal", "eat"}, Slot{"goal", "drink"}, Slot{"goal", "rest"}, Slot{}, Slot{}, Slot{}, Slot{}};
    }

    // LR-142: the content was reloaded (a JSON changed on disk): read the sprites, the map rules
    // and the HUD again, keep the camera and the quick bar, repaint everything.
    void reloadStatic() {
        const double cx = camX, cy = camY;
        const auto keepSlots = slots;
        UI.clear();
        actsRe.clear();
        hidden.clear();
        loadStatic();
        camX = cx;
        camY = cy;
        slots = keepSlots;
        atlas.clear();
        for (auto& [k, c] : chunks) c.painted = false;
        std::cout << "window: data reloaded" << std::endl;
    }

    // --- ground chunks --------------------------------------------------------------------
    std::string tileSprite(const std::string& g, int v) const {
        const Json tiles = MAP.value("tiles", Json::object());
        if (tiles.contains(g) && tiles[g].is_array() && !tiles[g].empty()) return tiles[g][std::size_t(v) % tiles[g].size()].get<std::string>();
        return "tl_grass";
    }
    void paintChunk(Chunk& c) {
        if (!c.tex) {
            c.tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, CH * T, CH * T);
            if (!c.tex) return;
            SDL_SetTextureScaleMode(c.tex, SDL_SCALEMODE_NEAREST);
            SDL_SetTextureBlendMode(c.tex, SDL_BLENDMODE_BLEND);
        }
        SDL_Texture* was = SDL_GetRenderTarget(ren);
        SDL_SetRenderTarget(ren, c.tex);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 0);
        SDL_RenderClear(ren);
        const Json& j = c.j;
        const int w = int(num(j, "w")), h = int(num(j, "h")), x0 = int(num(j, "x0")), y0 = int(num(j, "y0"));
        const std::string ground = str(j, "ground"), var = str(j, "var");
        const Json names = ST.value("ground", Json::array());
        const Json gg = MAP.value("glyph_ground", Json::object());
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const std::size_t i = std::size_t(y * w + x);
                const int gi = i < ground.size() ? ground[i] - 97 : 0;
                int v = 0;
                if (i * 2 + 1 < var.size()) v = std::stoi(var.substr(i * 2, 2), nullptr, 16);
                const std::string gname = gi >= 0 && gi < int(names.size()) ? names[std::size_t(gi)].get<std::string>() : "grass";
                if (glyphs) {
                    fill(float(x * T), float(y * T), T, T, hexc("#100c08"));
                    const std::string cl = gg.contains(gname) ? gg[gname][1].get<std::string>() : "l";
                    fill(float(x * T + 6), float(y * T + 6), 4, 4, col(sprites.colour(cl), 0.55f));
                    continue;
                }
                const Sprite sp = sprite(tileSprite(gname, v));
                const bool turn = gname == "grass" || gname == "meadow" || gname == "forest" || gname == "marsh" || gname == "sand";
                const int rot = turn ? (v >> 4) & 3 : 0;
                blit(sp, float(x * T), float(y * T), T, T, 1.0f, rot * 90.0);
            }
        // AL-112: fields and gardens over the ground (one char per cell, '.' none)
        const std::string crop = str(j, "crop");
        const Json cropTiles = j.value("crop_tiles", Json::array());
        if (!crop.empty())
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const std::size_t i = std::size_t(y * w + x);
                    if (i >= crop.size() || crop[i] == '.') continue;
                    const std::size_t k = std::size_t(crop[i] - '0');
                    if (k >= cropTiles.size() || !cropTiles[k].is_array() || cropTiles[k].size() < 2) continue;
                    const std::string name = cropTiles[k][0].get<std::string>();
                    const int rot = cropTiles[k][1].get<int>();
                    if (glyphs) {
                        if (name.find("path") != std::string::npos) continue;
                        char cl = 'l';
                        if (name.find("over") != std::string::npos) cl = 'T';
                        else if (name.find("ripe") != std::string::npos) cl = 'y';
                        else if (name.find("tilled") != std::string::npos || name.find("sown") != std::string::npos) cl = 'n';
                        else if (name.find("fallow") != std::string::npos || name.find("bare") != std::string::npos ||
                                 name.find("stubble") != std::string::npos) cl = 't';
                        fill(float(x * T), float(y * T), T, T, hexc("#100c08"));
                        fill(float(x * T + 5), float(y * T + 5), 6, 6, col(sprites.colour(cl), 0.7f));
                        continue;
                    }
                    blit(sprite(name), float(x * T), float(y * T), T, T, 1.0f, rot * 90.0);
                }
        // walls, doors, windows of the ground storey on the cell lines (4 px strips)
        auto pc = [&](char l) { return col(sprites.colour(l)); };
        for (const auto& e : arr(j, "edges")) {
            if (num(e, "z") != 0 || str(e, "kind") == "gable") continue;
            const std::string piece = str(e, "piece"), kind = str(e, "kind");
            char a = 'n', b = 'N';
            if (piece.find("daub") != std::string::npos) a = 'w', b = 'e';
            else if (piece.find("stone") != std::string::npos) a = 'W', b = 'M';
            const float lx = float((num(e, "x") - x0) * T), ly = float((num(e, "y") - y0) * T), len = float(num(e, "len") * T);
            const bool hz = str(e, "axis") == "h";
            const float rx = hz ? lx : lx - 2, ry = hz ? ly - 2 : ly, rw = hz ? len : 4, rh = hz ? 4 : len;
            const float al = flag(e, "built") ? 1.0f : 0.45f;
            if (glyphs) {
                fill(rx, ry, rw, rh, col(sprites.colour(kind == "door" ? 'o' : 'w'), al));
                continue;
            }
            fill(rx - 1, ry - 1, rw + 2, rh + 2, kOut.withAlpha(al));
            fill(rx, ry, rw, rh, pc(a).withAlpha(al));
            for (float k = 3; k < len; k += 6)
                if (hz) fill(lx + k, ly - 2, 1, 4, pc(b).withAlpha(al));
                else fill(lx - 2, ly + k, 4, 1, pc(b).withAlpha(al));
            if (kind == "door") {
                if (hz) {
                    fill(lx + 6, ly - 2, len - 12, 4, pc('n').withAlpha(al));
                    fill(lx + 6, ly - 2, 1, 4, kOut.withAlpha(al));
                    fill(lx + len - 7, ly - 2, 1, 4, kOut.withAlpha(al));
                    fill(lx + 8, ly - 1, len - 16, 2, pc('t').withAlpha(al));
                } else {
                    fill(lx - 2, ly + 6, 4, len - 12, pc('n').withAlpha(al));
                    fill(lx - 2, ly + 6, 4, 1, kOut.withAlpha(al));
                    fill(lx - 2, ly + len - 7, 4, 1, kOut.withAlpha(al));
                    fill(lx - 1, ly + 8, 2, len - 16, pc('t').withAlpha(al));
                }
            } else if (kind == "window") {
                if (hz) fill(lx + 10, ly - 1, len - 20, 2, pc('U').withAlpha(al));
                else fill(lx - 1, ly + 10, 2, len - 20, pc('U').withAlpha(al));
            }
        }
        SDL_SetRenderTarget(ren, was);
        c.painted = true;
        c.glyph = glyphs;
    }
    void requestChunk(int cx, int cy) {
        const std::string k = std::to_string(cx) + "," + std::to_string(cy);
        int code = 0;
        const std::string b = host->call("GET", "/api/grid?x=" + std::to_string(cx * CH) + "&y=" + std::to_string(cy * CH) +
                                                    "&w=" + std::to_string(CH) + "&h=" + std::to_string(CH),
                                         "", &code);
        if (code != 200) return;
        Chunk& c = chunks[k];
        c.at = nowMs();
        if (c.painted && c.body == b) return;
        c.body = b;
        c.j = parseOr(b, Json::object());
        c.painted = false;
        ++chunkVersion;
    }
    std::vector<std::pair<int, int>> visibleChunks() const {
        double x0, y0, x1, y1;
        toWorld(0, 0, x0, y0);
        toWorld(mapW(), mapH(), x1, y1);
        const double size = num(ST, "size", 12000);
        std::vector<std::pair<int, int>> out;
        for (int cy = int(std::floor(y0 / CH)); cy <= int(std::floor(y1 / CH)); ++cy)
            for (int cx = int(std::floor(x0 / CH)); cx <= int(std::floor(x1 / CH)); ++cx)
                if (cx >= 0 && cy >= 0 && cx * CH < size && cy * CH < size) out.push_back({cx, cy});
        return out;
    }
    void forgetFarChunks() {
        if (chunks.size() < 72) return;
        std::vector<std::pair<double, std::string>> far;
        for (const auto& [k, c] : chunks) {
            const double cx = num(c.j, "x0") + CH / 2.0, cy = num(c.j, "y0") + CH / 2.0;
            far.push_back({std::hypot(cx - camX, cy - camY), k});
        }
        std::sort(far.begin(), far.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (std::size_t i = 0; i < far.size() && chunks.size() > 48; ++i) {
            if (chunks[far[i].second].tex) SDL_DestroyTexture(chunks[far[i].second].tex);
            chunks.erase(far[i].second);
            ++chunkVersion;
        }
    }

    // --- polling -----------------------------------------------------------------------------
    std::set<std::string> shownOutcomes;   // LR-125
    bool outcomesPrimed = false;
    void poll() {
        lastPoll = nowMs();
        const int r = int(std::ceil(std::hypot(mapW(), mapH()) / tilePx())) + 30;
        std::string q = "/api/state?vx=" + std::to_string(int(std::lround(camX))) + "&vy=" + std::to_string(int(std::lround(camY))) +
                        "&vr=" + std::to_string(r);
        if (selId >= 0) q += "&sel=" + std::to_string(selId);
        const Json s = get(q);
        if (!s.is_object()) return;
        clock = s;
        paused = flag(s, "paused");
        tps = num(s, "tps", 2);
        const long long m = s.contains("me") && s["me"].is_number() && s["me"].get<long long>() > 0 ? s["me"].get<long long>() : -1;
        if (m >= 0 && m != me) follow = true;
        me = m;
        people.clear();
        byId.clear();
        for (const auto& p : s.value("people", Json::array())) {
            byId[ident(p, "id")] = people.size();
            people.push_back(p);
        }
        for (auto it = vis.begin(); it != vis.end();)
            if (!byId.count(it->first)) it = vis.erase(it);
            else ++it;
        selCard = s.contains("sel") ? s["sel"] : Json();
        meCard = s.contains("me_card") ? s["me_card"] : Json();
        // LR-125: how the acts of the led person came out, each told once
        for (const auto& o : s.value("outcomes", Json::array())) {
            const std::string k = str(o, "key");
            if (k.empty() || shownOutcomes.count(k)) continue;
            shownOutcomes.insert(k);
            if (outcomesPrimed) toast(str(o, "text"));
        }
        outcomesPrimed = true;
        if (screens) screens->polled();
        if (selId >= 0 && !byId.count(selId)) selId = -1;
        if (me != barFor) {
            barFor = me;
            checkBar();
        }
        if (me >= 0 && !selectedMe && selId < 0) {
            selectedMe = true;
            selId = me;
        }
        const double t = nowMs();
        for (const auto& e : s.value("speech", Json::array())) {
            const bool bark = flag(e, "bark");
            bubbles[ident(e, "who")] = {str(e, "line"), t + (bark ? 3500 : 5000), bark};
            if (!bark) {
                lines.push_back({str(e, "full"), t});
                if (lines.size() > 7) lines.pop_front();
            }
        }
    }
    void select(long long id) override {
        selId = id;
        selCard = Json();
        poll();
    }
    void checkBar() {
        lastBar = nowMs();
        if (me < 0) return;
        Json list = Json::array();
        for (const Slot& s : slots) list.push_back({{"kind", s.kind}, {"id", s.id}, {"target", s.target}});
        const Json c = post("/api/bar", {{"slots", list}, {"target", selId >= 0 && selId != me ? selId : 0}});
        if (c.is_array()) checked = c;
    }

    // --- smoothing: pawns slide over the cells toward where the sim puts them --------------
    void stepVis(double dt) {
        for (const Json& p : people) {
            const long long id = ident(p, "id");
            const double px = num(p, "x"), py = num(p, "y");
            auto [it, fresh] = vis.try_emplace(id);
            Vis& v = it->second;
            if (fresh) {
                v.x = px;
                v.y = py;
            }
            // AL-108: standing still, one looks where the sim turned one (at the act's target)
            const bool faced = p.contains("fx");
            const double fx = num(p, "fx"), fy = num(p, "fy");
            auto lookSim = [&] {
                if (faced && (fx || fy)) v.face = std::abs(fx) > std::abs(fy) ? (fx > 0 ? 'E' : 'W') : (fy > 0 ? 'S' : 'N');
            };
            if (v.driven) {
                if (!v.moving) lookSim();
                continue;
            }
            const double dx = px - v.x, dy = py - v.y, d = std::hypot(dx, dy);
            if (d > 80) {
                v.x = px, v.y = py, v.moving = false;
                continue;
            }
            if (d < 0.02) {
                v.moving = false;
                lookSim();
                continue;
            }
            const double speed = std::max(3.5, d * 1.6), s = std::min(d, speed * dt);
            const int before = int(std::floor(v.x)) + int(std::floor(v.y));
            v.x += dx / d * s;
            v.y += dy / d * s;
            v.walked += s;
            v.moving = true;
            if (int(std::floor(v.x)) + int(std::floor(v.y)) != before) v.bob ^= 1;
            v.face = std::abs(dx) > std::abs(dy) ? (dx > 0 ? 'E' : 'W') : (dy > 0 ? 'S' : 'N');
        }
    }

    // --- the map as the walk sees it (play.js) ---------------------------------------------
    const Chunk* chunkAt(double x, double y) const {
        const auto it = chunks.find(std::to_string(int(std::floor(x / CH))) + "," + std::to_string(int(std::floor(y / CH))));
        return it == chunks.end() ? nullptr : &it->second;
    }
    std::string groundAt(double x, double y) const {
        const Chunk* c = chunkAt(x, y);
        if (!c) return "";
        const Json& j = c->j;
        const int w = int(num(j, "w"));
        const long i = long(std::floor(y) - num(j, "y0")) * w + long(std::floor(x) - num(j, "x0"));
        const std::string g = str(j, "ground");
        if (i < 0 || i >= long(g.size())) return "";
        const int gi = g[std::size_t(i)] - 97;
        const Json names = ST.value("ground", Json::array());
        return gi >= 0 && gi < int(names.size()) ? names[std::size_t(gi)].get<std::string>() : "";
    }
    static long long wallKey(char axis, int x, int y) { return (long long)(axis == 'h') << 62 | (long long)(x & 0xfffffff) << 30 | (y & 0x3fffffff); }
    void rebuildWalls() {
        if (wallsVersion == chunkVersion) return;
        wallsVersion = chunkVersion;
        walls.clear();
        for (const auto& [k, c] : chunks)
            for (const auto& e : arr(c.j, "edges")) {
                if (num(e, "z") != 0 || str(e, "kind") == "door" || !flag(e, "built")) continue;
                const int ex = int(num(e, "x")), ey = int(num(e, "y")), len = int(num(e, "len"));
                const bool hz = str(e, "axis") == "h";
                for (int i = 0; i < len; ++i) walls.insert(hz ? wallKey('h', ex + i, ey) : wallKey('v', ex, ey + i));
            }
    }
    bool passCell(double x, double y) const {
        const std::string g = groundAt(x, y);
        return !g.empty() && g != "water";
    }
    bool stepOk(int ax, int ay, int bx, int by) const {
        if (!passCell(bx + 0.5, by + 0.5)) return false;
        if (bx != ax) return !walls.count(wallKey('v', std::max(ax, bx), ay));
        if (by != ay) return !walls.count(wallKey('h', ax, std::max(ay, by)));
        return true;
    }
    bool moveOk(double ax, double ay, double bx, double by) const {
        const int cx0 = int(std::floor(ax)), cy0 = int(std::floor(ay)), cx1 = int(std::floor(bx)), cy1 = int(std::floor(by));
        if (cx0 == cx1 && cy0 == cy1) return true;
        if (cx0 != cx1 && cy0 != cy1)
            return (stepOk(cx0, cy0, cx1, cy0) && stepOk(cx1, cy0, cx1, cy1)) || (stepOk(cx0, cy0, cx0, cy1) && stepOk(cx0, cy1, cx1, cy1));
        return stepOk(cx0, cy0, cx1, cy1);
    }
    bool sight(double ax, double ay, double bx, double by) const {
        const double d = std::hypot(bx - ax, by - ay);
        const int n = int(std::ceil(d * 3));
        double px = ax, py = ay;
        for (int i = 1; i <= n; ++i) {
            const double x = ax + (bx - ax) * i / n, y = ay + (by - ay) * i / n;
            if (!moveOk(px, py, x, y)) return false;
            px = x, py = y;
        }
        return true;
    }
    bool findPath(double fsx, double fsy, double ftx, double fty, std::vector<std::pair<double, double>>& out) const {
        const int sx = int(std::floor(fsx)), sy = int(std::floor(fsy)), tx = int(std::floor(ftx)), ty = int(std::floor(fty));
        if (!passCell(tx + 0.5, ty + 0.5)) return false;
        auto key = [](int x, int y) { return (long long)x * 100003 + y; };
        using Node = std::tuple<int, int, int, int>;   // f, g, x, y
        std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
        std::unordered_map<long long, std::pair<int, int>> came;
        std::unordered_map<long long, int> cost;
        open.push({std::abs(tx - sx) + std::abs(ty - sy), 0, sx, sy});
        cost[key(sx, sy)] = 0;
        int n = 0;
        while (!open.empty() && n++ < 40000) {
            const auto [f, g, x, y] = open.top();
            open.pop();
            (void)f;
            if (x == tx && y == ty) {
                std::vector<std::pair<double, double>> pts;
                int cx = x, cy = y;
                while (came.count(key(cx, cy))) {
                    pts.push_back({cx + 0.5, cy + 0.5});
                    const auto pr = came.at(key(cx, cy));
                    cx = pr.first, cy = pr.second;
                }
                std::reverse(pts.begin(), pts.end());
                // drop the cells a straight walk passes anyway
                out.clear();
                double ax = sx + 0.5, ay = sy + 0.5;
                std::size_t i = 0;
                while (i < pts.size()) {
                    std::size_t j = pts.size() - 1;
                    while (j > i && !sight(ax, ay, pts[j].first, pts[j].second)) --j;
                    out.push_back(pts[j]);
                    ax = pts[j].first, ay = pts[j].second;
                    i = j + 1;
                }
                return true;
            }
            const auto ci = cost.find(key(x, y));
            if (ci != cost.end() && g > ci->second) continue;
            const int d4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (auto& d : d4) {
                const int nx = x + d[0], ny = y + d[1];
                if (!stepOk(x, y, nx, ny)) continue;
                const int ng = g + 1;
                const auto nk = key(nx, ny);
                const auto old = cost.find(nk);
                if (old != cost.end() && ng >= old->second) continue;
                cost[nk] = ng;
                came[nk] = {x, y};
                open.push({ng + std::abs(tx - nx) + std::abs(ty - ny), ng, nx, ny});
            }
        }
        return false;
    }

    // --- walking the player's person (play.js) ------------------------------------------------
    void walkMe(double dt) {
        if (me < 0) return;
        const auto it = vis.find(me);
        if (it == vis.end()) return;
        Vis& v = it->second;
        rebuildWalls();
        const bool* k = SDL_GetKeyboardState(nullptr);
        const bool typing = ui.typing();
        double dx = 0, dy = 0;
        if (!typing) {
            dx = (k[SDL_SCANCODE_D] ? 1 : 0) - (k[SDL_SCANCODE_A] ? 1 : 0);
            dy = (k[SDL_SCANCODE_S] ? 1 : 0) - (k[SDL_SCANCODE_W] ? 1 : 0);
        }
        const double t = nowMs();
        const bool run = k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT];
        if (dx || dy) path.clear();
        if (!dx && !dy && !path.empty()) {
            const auto [px, py] = path.front();
            const double d = std::hypot(px - v.x, py - v.y);
            if (d < 0.15) path.erase(path.begin());
            else dx = (px - v.x) / d, dy = (py - v.y) / d;
        }
        if (dx || dy) {
            const double len = std::hypot(dx, dy), sp = (run ? 7.5 : 4.2) * dt;
            double s = sp;
            if (!path.empty()) s = std::min(sp, std::hypot(path.front().first - v.x, path.front().second - v.y));
            const double nx = v.x + dx / len * s, ny = v.y + dy / len * s;
            const int before = int(std::floor(v.x)) + int(std::floor(v.y));
            const double wx = v.x, wy = v.y;
            if (moveOk(v.x, v.y, nx, ny)) v.x = nx, v.y = ny;
            else if (moveOk(v.x, v.y, nx, v.y)) v.x = nx;
            else if (moveOk(v.x, v.y, v.x, ny)) v.y = ny;
            else path.clear();
            v.walked += std::hypot(v.x - wx, v.y - wy);
            if (int(std::floor(v.x)) + int(std::floor(v.y)) != before) v.bob ^= 1;
            v.face = std::abs(dx) > std::abs(dy) ? (dx > 0 ? 'E' : 'W') : (dy > 0 ? 'S' : 'N');
            v.moving = true;
            v.driven = true;
            lastMove = t;
            follow = true;
        } else if (v.driven) v.moving = false;
        if (v.driven && t - lastSteer > 120 && (!haveSteer || std::hypot(steerX - v.x, steerY - v.y) > 0.05)) {
            lastSteer = t;
            haveSteer = true;
            steerX = v.x, steerY = v.y;
            const Json r = cmd({{"op", "cmd"}, {"type", "steer"}, {"args", {{"id", me}, {"x", v.x}, {"y", v.y}, {"run", run}}}});
            const std::string res = str(r, "result");
            if (!res.empty() && res != "ok") toast(str(r, "text").empty() ? res : str(r, "text"));
        }
        if (v.driven && t - lastMove > 700 && t - lastSteer > 300) v.driven = false;
    }

    // --- picking ---------------------------------------------------------------------------------
    const Json* pickPerson(double wx, double wy) const {
        const Json* best = nullptr;
        double bd = 1.1;
        for (const Json& p : people) {
            const auto it = vis.find(ident(p, "id"));
            if (it == vis.end()) continue;
            const double d = std::hypot(it->second.x - wx, (it->second.y - 0.5) - wy);
            if (d < bd || (d == bd && str(p, "sp").empty())) bd = d, best = &p;
        }
        return best;
    }
    const Json* thingAt(double wx, double wy) const {
        const Json* best = nullptr;
        double bd = 0.9;
        for (const auto& [k, c] : chunks)
            for (const auto& t : arr(c.j, "things")) {
                if (!t.is_array() || t.size() < 4 || hidden.count(t[1].get<std::string>())) continue;
                const double d = std::hypot(t[2].get<double>() - wx, t[3].get<double>() - wy);
                if (d < bd) bd = d, best = &t;
            }
        return best;
    }
    std::vector<long long> openBuildings() const {
        std::vector<long long> open;
        auto mark = [&](double x, double y) {
            for (const auto& [k, c] : chunks)
                for (const auto& r : arr(c.j, "rooms"))
                    if (x >= num(r, "x0") && x < num(r, "x1") && y >= num(r, "y0") && y < num(r, "y1"))
                        open.push_back(ident(r, "building"));
        };
        if (selId >= 0 && vis.count(selId)) mark(vis.at(selId).x, vis.at(selId).y);
        if (haveHover) mark(hoverX, hoverY);
        return open;
    }

    // ======================================================================
    // the map
    void drawMap() {
        const double sc = scale(), tp = tilePx();
        fill(0, 0, float(W), float(H), glyphs ? hexc("#100c08") : hexc("#1a2a12"));
        const auto vc = visibleChunks();
        int asked = 0;
        const double t = nowMs();
        for (const auto& [cx, cy] : vc) {
            const std::string k = std::to_string(cx) + "," + std::to_string(cy);
            auto it = chunks.find(k);
            if (it == chunks.end()) {
                if (asked++ < 3) requestChunk(cx, cy);
                continue;
            }
            Chunk& c = it->second;
            if (t - c.at > 8000 && asked < 3) {
                ++asked;
                requestChunk(cx, cy);   // things change: felled trees, new pieces
            }
            if (!c.painted || c.glyph != glyphs) paintChunk(c);
            if (!c.tex) continue;
            float sx, sy;
            toScreen(num(c.j, "x0"), num(c.j, "y0"), sx, sy);
            const SDL_FRect d{sx, sy, float(num(c.j, "w") * tp), float(num(c.j, "h") * tp)};
            const SDL_FRect s{0, 0, float(num(c.j, "w") * T), float(num(c.j, "h") * T)};
            SDL_RenderTexture(ren, c.tex, &s, &d);
        }
        forgetFarChunks();
        // drawables sorted by their foot: things, pawns
        double wx0, wy0, wx1, wy1;
        toWorld(-tp * 2, -tp * 2, wx0, wy0);
        toWorld(mapW() + tp * 2, mapH() + tp * 2, wx1, wy1);
        struct Item {
            double y;
            const Json* thing;
            const Json* person;
        };
        std::vector<Item> items;
        const Json thin = MAP.value("thin", Json::object());
        for (const auto& [cx, cy] : vc) {
            const auto it = chunks.find(std::to_string(cx) + "," + std::to_string(cy));
            if (it == chunks.end()) continue;
            const Json& j = it->second.j;
            const double x0 = num(j, "x0"), y0 = num(j, "y0"), w = num(j, "w"), h = num(j, "h");
            for (const auto& th : arr(j, "things")) {
                if (!th.is_array() || th.size() < 5) continue;
                const std::string type = th[1].get<std::string>();
                const double x = th[2].get<double>(), y = th[3].get<double>();
                if (th[4].get<double>() > 0 || hidden.count(type)) continue;
                if (x < x0 || x >= x0 + w || y < y0 || y >= y0 + h) continue;
                if (x < wx0 || x > wx1 || y < wy0 || y > wy1) continue;
                if (thin.contains(type) && thin[type].get<int>() > 1 && th[0].get<long long>() % thin[type].get<int>()) continue;
                items.push_back({y, &th, nullptr});
            }
        }
        for (const Json& p : people) {
            const auto it = vis.find(ident(p, "id"));
            if (it == vis.end() || it->second.x < wx0 || it->second.x > wx1 || it->second.y < wy0 || it->second.y > wy1) continue;
            items.push_back({it->second.y, nullptr, &p});
        }
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.y < b.y; });
        // AL-110: the ground's weather, the shadows, what shines (before the drawables cover the ground)
        std::vector<const Json*> things;
        std::vector<const Json*> folk;
        for (const Item& it : items) (it.thing ? things : folk).push_back(it.thing ? it.thing : it.person);
        guard("atmos", [&] { atmosBegin(things, folk, sc); });
        for (const Item& it : items)
            if (it.thing) guard("thing", [&] { drawThing(*it.thing, sc); });
            else guard("pawn", [&] { drawPawn(*it.person, vis[ident(*it.person, "id")], sc); });
        guard("drawRoofs", [&] { drawRoofs(tp); });
        guard("atmosLight", [&] { atmosEnd(sc); });
        guard("build", [&] { screens->drawWorld(); });
        // LR-137: the room names are in the place plate (drawPlace), not over the map
        guard("drawPlay", [&] { drawPlay(sc); });
        guard("drawSelection", [&] { drawSelection(sc); });
        guard("drawOverheads", [&] { drawOverheads(sc); });
        guard("drawBubbles", [&] { drawBubbles(sc); });
        if (!atmos.enabled) guard("drawNight", [&] { drawNight(); });
    }
    void drawThing(const Json& th, double sc) {
        const std::string type = th[1].get<std::string>();
        const Json& look = thingLook(type);
        float sx, sy;
        toScreen(th[2].get<double>(), th[3].get<double>(), sx, sy);
        if (glyphs) {
            textC(sx, sy - float(7 * sc), str(look, "glyph").empty() ? "?" : str(look, "glyph"), col(sprites.colour(str(look, "colour").empty() ? "w" : str(look, "colour"))), float(13 * sc));
            return;
        }
        const Sprite sp = sprite(str(look, "sprite"), look.value("recolour", Json()));
        if (!sp) return;
        const std::string sn = str(look, "sprite");
        static const std::regex tree("tree|grove|pine");
        const bool isTree = std::regex_search(type, tree) || sn == "ob_tree" || sn == "ob_pine";
        const float s = float(sc);
        if (isTree) {
            ellipse(sx, sy - s, 6 * s, 2.5f * s, ui::Colour(0, 0, 0, 0.22f));
            blit(sp, sx - 8 * s, sy - 15 * s, 16 * s, 16 * s);
        } else blit(sp, sx - 8 * s, sy - 8 * s, 16 * s, 16 * s);
    }
    void drawPawn(const Json& p, const Vis& v, double scd) {
        float sx, sy;
        toScreen(v.x, v.y, sx, sy);
        const float sc = float(scd);
        const long long id = ident(p, "id");
        const std::string sp = str(p, "sp");
        const bool alive = flag(p, "alive");
        if (glyphs) {
            std::string g = alive ? (str(p, "rg").empty() ? "@" : str(p, "rg")) : "%";   // AL-114: the kind's glyph
            if (!sp.empty()) {
                std::size_t at = 0;
                const std::string nm = str(p, "name").empty() ? sp : str(p, "name");
                ui::nextCodepoint(nm, at);
                g = nm.substr(0, at);
            }
            const ui::Colour c = !sp.empty() ? col(sprites.colour('t')) : (id == selId ? hexc("#ffe080") : hexc(famColour(p).c_str()));
            textC(sx, sy - 7 * sc, g, c, 14 * sc);
            return;
        }
        ellipse(sx, sy + sc, (sp.empty() ? 5 : 4) * sc, 2 * sc, ui::Colour(0, 0, 0, 0.25f));
        const std::string st = str(p, "st");
        const bool lying = !alive || flag(p, "down") || st == "Sleep" || st == "Incapacitated";
        if (!sp.empty()) {
            blit(beastSprite(sp, v.face == 'W' ? 'W' : 'E'), sx - 8 * sc, sy - 11 * sc, 16 * sc, 16 * sc);
            return;
        }
        const Sprite img = lying ? pawnSprite(pawnSpec(p), 'S', 0) : pawnSprite(pawnSpec(p), v.face, poseOf(p, v));
        // AL-114: a kind's size (a goblin 16, an ogre or a troll 32 px against the 20 of a man); LR-139: and one's own
        const float k = (num(p, "rpx", 0) > 0 ? float(num(p, "rpx", 20)) / float(kPawn) : 1.0f) * heightK(p);
        if (lying) {
            const float cx = sx, cy = sy - 3 * sc;
            blit(img, cx - 10 * sc * k, cy - 10 * sc * k, kPawn * sc * k, kPawn * sc * k, alive ? 1.0f : 0.6f, -90);
        } else blit(img, sx - 10 * sc * k, sy - 18 * sc * k, kPawn * sc * k, kPawn * sc * k);
    }
    void drawRoofs(double tp) {
        if (roofMode == "none") return;
        const auto open = roofMode == "auto" ? openBuildings() : std::vector<long long>{};
        const Sprite roof = sprite("tl_roof");
        std::unordered_set<long long> seen;
        for (const auto& [k, c] : chunks)
            for (const auto& r : arr(c.j, "roofs")) {
                const long long b = ident(r, "building");
                if (b >= 0 && std::find(open.begin(), open.end(), b) != open.end()) continue;
                const long long key = (long long)num(r, "x") * 100003 + (long long)num(r, "y");
                if (!seen.insert(key).second) continue;
                float sx, sy;
                toScreen(num(r, "x"), num(r, "y"), sx, sy);
                const float len = float(num(r, "len")), t = float(tp);
                if (sx > mapW() || sy > mapH() || sx + len * t < 0 || sy + len * t < 0) continue;
                if (glyphs) {
                    fill(sx, sy, len * t, len * t, hexc("#a82820", 0.55f));
                    continue;
                }
                for (int dy = 0; dy < int(len); ++dy)
                    for (int dx = 0; dx < int(len); ++dx) blit(roof, sx + dx * t, sy + dy * t, t, t);
                atmos.snowOn(sx, sy, len * t, len * t);   // AL-110
            }
    }
    void drawRoomLabels() {
        if (zoom < 2) return;
        const bool none = roofMode == "none";
        const auto open = roofMode == "auto" ? openBuildings() : std::vector<long long>{};
        std::unordered_set<long long> done;
        const Json rooms = ST.value("rooms", Json::object());
        const float s = float(scale());
        for (const auto& [k, c] : chunks)
            for (const auto& r : arr(c.j, "rooms")) {
                if (num(r, "z") != 0 || !done.insert(ident(r, "id")).second) continue;
                if (!none && std::find(open.begin(), open.end(), ident(r, "building")) == open.end()) continue;
                float sx, sy;
                toScreen((num(r, "x0") + num(r, "x1")) / 2, num(r, "y0") + 0.55, sx, sy);
                const std::string type = str(r, "type");
                const std::string name = rooms.contains(type) ? rooms[type].get<std::string>() : type;
                const float px = std::round(5.5f * s);
                const float w = textW(name, px) + 8 * float(dpr());
                fill(sx - w / 2, sy - 5 * s, w, 10 * s, hexc("#e8d8b0", 0.88f));
                textC(sx, sy - textH(px) / 2, name, hexc("#3a2414"), px);
            }
    }
    void drawSelection(double scd) {
        const Json* p = person(selId);
        if (!p || !vis.count(selId)) return;
        const Vis& v = vis.at(selId);
        const float sc = float(scd);
        float sx, sy;
        toScreen(v.x, v.y, sx, sy);
        if (p->contains("dest") && (*p)["dest"].is_array() && (*p)["dest"].size() >= 2) {
            const double dx = (*p)["dest"][0].get<double>(), dy = (*p)["dest"][1].get<double>(), d = std::hypot(dx - v.x, dy - v.y);
            for (int k = 1; k < d && k < 400; ++k) {
                float px, py;
                toScreen(v.x + (dx - v.x) * k / d, v.y + (dy - v.y) * k / d, px, py);
                fill(px - sc, py - sc, 2 * sc, 2 * sc, kGold);
            }
            float ex, ey;
            toScreen(dx, dy, ex, ey);
            frame(ex - 4 * sc, ey - 4 * sc, 8 * sc, 8 * sc, kGold, sc);
        }
        const float x0 = sx - 11 * sc, y0 = sy - 19 * sc, w = 22 * sc, h = 22 * sc, l = 5 * sc;
        for (int pass = 0; pass < 2; ++pass) {
            const float off = pass == 0 ? sc : 0;
            const ui::Colour c = pass == 0 ? kOut : kGold;
            fill(x0 + off, y0 + off, l, sc, c);
            fill(x0 + off, y0 + off, sc, l, c);
            fill(x0 + w - l + off, y0 + off, l, sc, c);
            fill(x0 + w - sc + off, y0 + off, sc, l, c);
            fill(x0 + off, y0 + h - sc + off, l, sc, c);
            fill(x0 + off, y0 + h - l + off, sc, l, c);
            fill(x0 + w - l + off, y0 + h - sc + off, l, sc, c);
            fill(x0 + w - sc + off, y0 + h - l + off, sc, l, c);
        }
    }
    void drawOverheads(double scd) {
        const float sc = float(scd);
        for (const Json& p : people) {
            if (!str(p, "sp").empty()) continue;
            const long long id = ident(p, "id");
            const auto it = vis.find(id);
            if (it == vis.end()) continue;
            float sx, sy;
            toScreen(it->second.x, it->second.y, sx, sy);
            if (sx < -50 || sy < -50 || sx > mapW() + 50 || sy > mapH() + 50) continue;
            const bool sel = id == selId, hov = id == hoverId;
            const std::string icon = actIcon(p);
            const bool showIcon = !icon.empty() && !glyphs && (zoom >= 2 || sel || hov);
            const float s = std::max(1.0f, std::floor(sc / 2));
            if (showIcon) {
                const float box = 18 * s, bx = sx - box / 2, by = sy - 20 * sc - box - 2 * s;
                fill(bx - s, by - s, box + 2 * s, box + 2 * s, kOut);
                fill(bx, by, box, box, kParch);
                blit(sprite(icon), bx + s, by + s, 16 * s, 16 * s);
            }
            if (altDown || sel || hov) {
                const float px = std::round(6 * std::max(2.0f, sc));
                const float y = sy - 20 * sc - (showIcon ? 20 * s + 2 : 0) - textH(px) - 2;
                const std::string name = str(p, "name");
                textC(sx + 1, y + 1, name, ui::Colour(0, 0, 0, 1), px);
                textC(sx, y, name, sel ? kGold : kText, px);
            }
        }
    }
    void drawBubbles(double scd) {
        const float sc = float(scd);
        const double t = nowMs();
        const float px = std::round(5.5f * std::max(2.0f, sc));
        for (auto it = bubbles.begin(); it != bubbles.end();) {
            if (t > it->second.until) {
                it = bubbles.erase(it);
                continue;
            }
            const auto pv = vis.find(it->first);
            if (pv == vis.end()) {
                ++it;
                continue;
            }
            float sx, sy;
            toScreen(pv->second.x, pv->second.y, sx, sy);
            const std::string text = utf8Cut(it->second.line, 58);
            const float w = textW(text, px) + 10 * float(dpr()), h = 9 * std::max(2.0f, sc) * 0.75f, y = sy - 26 * sc;
            const float a = it->second.bark ? 0.75f : 0.95f;
            fill(sx - w / 2 - 1, y - h - 1, w + 2, h + 2, kOut.withAlpha(a));
            fill(sx - w / 2, y - h, w, h, (it->second.bark ? hexc("#d8ccb0") : kParch).withAlpha(a));
            textC(sx, y - h + (h - textH(px)) / 2, text, kInk.withAlpha(a), px);
            ++it;
        }
    }
    void drawPlay(double scd) {
        const float sc = float(scd);
        const double t = nowMs();
        if (t - markerAt < 1500 && !path.empty()) {
            float x, y;
            toScreen(markerX, markerY, x, y);
            const float r = float((4 + (t - markerAt) / 300) * sc);
            frame(x - r, y - r, 2 * r, 2 * r, kGold, sc);
        }
        if (!path.empty() && me >= 0)
            for (const auto& [x, y] : path) {
                float sx, sy;
                toScreen(x, y, sx, sy);
                fill(sx - sc, sy - sc, 2 * sc, 2 * sc, kGold.withAlpha(0.7f));
            }
        if (me >= 0 && vis.count(me)) {
            float sx, sy;
            toScreen(vis[me].x, vis[me].y, sx, sy);
            ring(sx, sy + sc, 7 * sc, 3 * sc, std::max(1.0f, sc * 0.75f), kPlayer);
        }
    }
    // ======================================================================
    // AL-110: light, shadows and weather (native/atmos2d): what casts, what shines
    struct WLight {
        double x = 0, y = 0, r = 4, rgb[3] = {1, 0.7, 0.4}, i = 1, flicker = 0;
        std::uint64_t key = 0;
        bool smoke = false;
    };
    struct RoomBox {
        double x0, y0, x1, y1;
        long long building;
        int awake = 0, all = 0;
    };
    std::vector<WLight> wlights;
    std::vector<RoomBox> roomBoxes;
    std::vector<std::array<float, 5>> panes;          // lit windows: screen x, y, w, h, strength
    std::vector<std::array<double, 3>> torches;        // world x, y, key
    std::vector<std::array<double, 3>> fxFlames;       // the sim's fires: world x, y, strength
    double atmosLast = 0;

    int roomAt(double x, double y) const {
        for (std::size_t i = 0; i < roomBoxes.size(); ++i)
            if (x >= roomBoxes[i].x0 && x < roomBoxes[i].x1 && y >= roomBoxes[i].y0 && y < roomBoxes[i].y1) return int(i);
        return -1;
    }
    static void rgbOf(const Json& l, double out[3]) {
        const Json c = l.value("rgb", Json::array());
        if (c.is_array() && c.size() >= 3)
            for (int k = 0; k < 3; ++k) out[k] = c[k].get<double>();
    }
    // The pawn as drawPawn draws it standing (false: lying, a glyph).
    bool pawnLook(const Json& p, const Vis& v, double scd, Sprite& img, SDL_FRect& dst, float& foot, bool& beast) {
        const std::string st = str(p, "st");
        if (glyphs || !flag(p, "alive") || flag(p, "down") || st == "Sleep" || st == "Incapacitated") return false;
        float sx, sy;
        toScreen(v.x, v.y, sx, sy);
        const float sc = float(scd);
        const std::string sp = str(p, "sp");
        beast = !sp.empty();
        if (beast) {
            img = beastSprite(sp, v.face == 'W' ? 'W' : 'E');
            dst = {sx - 8 * sc, sy - 11 * sc, 16 * sc, 16 * sc};
            foot = sy + 4 * sc;
        } else {
            img = pawnSprite(pawnSpec(p), v.face, poseOf(p, v));
            const float k = (num(p, "rpx", 0) > 0 ? float(num(p, "rpx", 20)) / float(kPawn) : 1.0f) * heightK(p);
            dst = {sx - 10 * sc * k, sy - 18 * sc * k, kPawn * sc * k, kPawn * sc * k};
            foot = sy + 1 * sc;
        }
        return bool(img);
    }

    void atmosBegin(const std::vector<const Json*>& things, const std::vector<const Json*>& folk, double sc) {
        atmos.enabled = !glyphs;
        const double t = nowMs();
        const double dt = atmosLast > 0 ? std::min(0.25, (t - atmosLast) / 1000) : 0.0;
        atmosLast = t;
        const double frac = paused ? 0.0 : std::clamp((t - lastPoll) / 1000.0 * tps, 0.0, 1.0);
        atmos.update(clock.value("sky", Json()), clock.value("sky_next", Json()), frac, dt, t / 1000);
        atmos.frame(mapW(), mapH(), camX, camY, tilePx(), float(sc));
        wlights.clear(), panes.clear(), torches.clear(), roomBoxes.clear(), fxFlames.clear();
        if (!atmos.enabled) return;
        const SkyNow& k = atmos.sky();
        const double tp = tilePx(), hour = num(clock.value("sky", Json::object()), "hour", 12);

        // the ground storey's rooms and who is in them
        std::unordered_set<int> seenRoom;
        for (const auto& [key, c] : chunks)
            for (const auto& r : arr(c.j, "rooms"))
                if (num(r, "z") == 0 && seenRoom.insert(int(ident(r, "id"))).second)
                    roomBoxes.push_back({num(r, "x0"), num(r, "y0"), num(r, "x1"), num(r, "y1"), ident(r, "building")});
        std::vector<int> where(folk.size(), -1);
        for (std::size_t i = 0; i < folk.size(); ++i) {
            const Json& p = *folk[i];
            if (!str(p, "sp").empty() || !flag(p, "alive")) continue;
            const auto v = vis.find(ident(p, "id"));
            if (v == vis.end()) continue;
            where[i] = roomAt(v->second.x, v->second.y);
            if (where[i] < 0) continue;
            RoomBox& rb = roomBoxes[std::size_t(where[i])];
            ++rb.all;
            if (str(p, "st") != "Sleep") ++rb.awake;
        }
        std::unordered_map<long long, std::pair<int, int>> houses;   // building -> awake, all
        for (const RoomBox& rb : roomBoxes) {
            auto& hh = houses[rb.building];
            hh.first += rb.awake, hh.second += rb.all;
        }
        const auto open = roofMode == "auto" ? openBuildings() : std::vector<long long>{};
        auto roofed = [&](long long b) {   // its roof drawn: what is inside does not shine out
            return b >= 0 && roofMode != "none" && std::find(open.begin(), open.end(), b) == open.end();
        };
        auto workerNear = [&](double x, double y, double r, bool working) {
            for (const Json* p : folk) {
                if (!str(*p, "sp").empty() || !flag(*p, "alive")) continue;
                const auto v = vis.find(ident(*p, "id"));
                if (v == vis.end() || std::hypot(v->second.x - x, v->second.y - y) > r) continue;
                if (!working || str(*p, "st") == "Work") return true;
            }
            return false;
        };

        // fires, ovens, forges, shrines
        for (const Json* th : things) {
            const std::string type = (*th)[1].get<std::string>();
            const Json* L = atmos.lightOf(type);
            if (!L) continue;
            const double x = (*th)[2].get<double>(), y = (*th)[3].get<double>();
            const std::string when = str(*L, "when");
            double on = 1;
            if (when == "hearth") {
                const bool meal = (hour >= 5 && hour < 9) || (hour >= 11.5 && hour < 13.5) || (hour >= 17 && hour < 23.5);
                on = meal || workerNear(x, y, 2.5, true) ? 1.0 : 0.3;   // embers between meals
            } else if (when == "evening") on = hour >= 17.5 || hour < 1 || workerNear(x, y, 3, false) ? 1.0 : 0.0;
            else if (when == "worked") on = workerNear(x, y, 2.5, true) ? 1.0 : 0.15;
            if (on <= 0) continue;
            const int room = roomAt(x, y);
            const bool hidden = room >= 0 && roofed(roomBoxes[std::size_t(room)].building);
            WLight l;
            l.x = x, l.y = y, l.r = num(*L, "r", 5), l.i = hidden ? 0.0 : num(*L, "i", 1) * on, l.flicker = num(*L, "flicker", 0.2);
            rgbOf(*L, l.rgb);
            l.key = std::uint64_t((*th)[0].get<long long>());
            l.smoke = on >= 0.9 && atmos.smokes(type);   // under a roof: out of the chimney
            wlights.push_back(l);
        }
        // the windows of a house someone is in: lit while they are up, and in the evening
        if (const Json* W = atmos.lightOf("window"); W && k.lights > 0.05) {
            std::unordered_set<long long> done;
            for (const auto& [key, c] : chunks)
                for (const auto& e : arr(c.j, "edges")) {
                    if (num(e, "z") != 0 || str(e, "kind") != "window" || !flag(e, "built")) continue;
                    const double x = num(e, "x"), y = num(e, "y"), len = num(e, "len");
                    const bool hz = str(e, "axis") == "h";
                    if (!done.insert((long long)(x * 2) * 1000003 + (long long)(y * 2) * 7 + hz).second) continue;
                    const double cx = hz ? x + len / 2 : x, cy = hz ? y : y + len / 2;
                    // the room behind it; the light falls out the other way
                    int room = roomAt(hz ? cx : cx - 0.5, hz ? cy - 0.5 : cy);
                    double out = 1.0;
                    if (room < 0) room = roomAt(hz ? cx : cx + 0.5, hz ? cy + 0.5 : cy), out = -1.0;
                    if (room < 0) continue;
                    if (roomAt(hz ? cx : cx + 0.5 * out, hz ? cy + 0.5 * out : cy) >= 0) out = 0;   // between two rooms
                    const auto hh = houses[roomBoxes[std::size_t(room)].building];
                    const bool lit = hh.first > 0 || (hh.second > 0 && hour >= 16 && hour < 22.5);
                    if (!lit) continue;
                    WLight l;
                    l.x = cx + (hz ? 0 : out * 1.2), l.y = cy + (hz ? out * 1.2 : 0);
                    l.r = num(*W, "r", 3.8), l.i = num(*W, "i", 0.7), l.flicker = num(*W, "flicker", 0.08);
                    rgbOf(*W, l.rgb);
                    l.key = std::uint64_t(x * 131 + y * 7919);
                    wlights.push_back(l);
                    float sx, sy;
                    const double m = 10.0 / T, th = 2.0 / T;
                    toScreen(hz ? x + m : x - th / 2, hz ? y - th / 2 : y + m, sx, sy);
                    panes.push_back({sx, sy, float((hz ? len - 2 * m : th) * tp), float((hz ? th : len - 2 * m) * tp), float(l.i)});
                }
            // a candle in the rooms one sees into
            for (const RoomBox& rb : roomBoxes)
                if (rb.awake > 0 && !roofed(rb.building)) {
                    WLight l;
                    l.x = (rb.x0 + rb.x1) / 2, l.y = (rb.y0 + rb.y1) / 2;
                    l.r = 1.5 + 0.6 * std::max(rb.x1 - rb.x0, rb.y1 - rb.y0), l.i = 0.55, l.flicker = 0.12;
                    rgbOf(*W, l.rgb);
                    l.key = std::uint64_t(l.x * 31 + l.y * 17);
                    wlights.push_back(l);
                }
        }
        // a torch in the hand of whoever is out at night
        if (const Json* Tl = atmos.lightOf("torch"); Tl && k.lights > 0.45)
            for (std::size_t i = 0; i < folk.size(); ++i) {
                const Json& p = *folk[i];
                if (where[i] >= 0 || !str(p, "sp").empty() || !flag(p, "alive") || flag(p, "down") || str(p, "st") == "Sleep") continue;
                const auto v = vis.find(ident(p, "id"));
                if (v == vis.end()) continue;
                WLight l;
                l.x = v->second.x + 0.35, l.y = v->second.y - 0.4;
                l.r = num(*Tl, "r", 5), l.i = num(*Tl, "i", 0.85) * std::min(1.0, (k.lights - 0.45) * 4), l.flicker = num(*Tl, "flicker", 0.3);
                rgbOf(*Tl, l.rgb);
                l.key = std::uint64_t(ident(p, "id"));
                wlights.push_back(l);
                torches.push_back({v->second.x, v->second.y, double(ident(p, "id"))});
            }
        // lights the sim gives (fire, magic: [{x, y, r, rgb, i, flicker}]) when it does
        // AL-115: the sim's own fire, magic, steam (/api/state world_fx, metres of the world)
        const Json wfx = clock.value("world_fx", Json::object());
        fxFlames.clear();
        for (const auto& f : wfx.value("fx", Json::array())) {
            if (!f.is_object()) continue;
            const std::string kind = str(f, "kind");
            const double x = num(f, "x"), y = num(f, "y"), st = std::clamp(num(f, "strength", 1), 0.0, 1.5);
            const std::uint64_t key = std::uint64_t(x * 977 + y * 31) ^ 0x5bd1e995u;
            if (kind == "flame") {
                WLight l;
                l.x = x, l.y = y, l.r = 3 + 4 * st, l.i = std::min(1.0, 0.6 + 0.4 * st), l.flicker = 0.45, l.key = key;
                l.rgb[0] = 1.0, l.rgb[1] = 0.55, l.rgb[2] = 0.2;
                wlights.push_back(l);
                fxFlames.push_back({x, y, st});
                atmos.addSmoke(x, y - 0.3, key, st, 2);
            } else if (kind == "steam") atmos.addSmoke(x, y, key, st, 1);
            else if (kind == "smoke") atmos.addSmoke(x, y, key, st, 0);
        }
        for (const auto& sl : wfx.value("lights", clock.value("lights", Json::array()))) {
            if (!sl.is_object()) continue;
            WLight l;
            l.x = num(sl, "x"), l.y = num(sl, "y"), l.r = num(sl, "r", 4), l.i = num(sl, "i", 1), l.flicker = num(sl, "flicker", 0.2);
            rgbOf(sl, l.rgb);
            l.key = std::uint64_t(l.x * 977 + l.y * 31);
            wlights.push_back(l);
        }

        // the ground: wet, snow; then the sun's shadows of things, people and houses
        atmos.drawGround();
        atmos.beginShadows();
        const float s = float(sc);
        static const std::regex treeRe("tree|grove|pine");
        for (const Json* th : things) {
            const std::string type = (*th)[1].get<std::string>();
            const Json& look = thingLook(type);
            const std::string sn = str(look, "sprite");
            const bool isTree = std::regex_search(type, treeRe) || sn == "ob_tree" || sn == "ob_pine";
            const double hgt = atmos.heightOf(type, atmos.heightOf(isTree ? "tree" : "thing", 1.0));
            if (hgt <= 0.05) continue;
            const Sprite sp = sprite(sn, look.value("recolour", Json()));
            if (!sp) continue;
            float sx, sy;
            toScreen((*th)[2].get<double>(), (*th)[3].get<double>(), sx, sy);
            const SDL_FRect d = isTree ? SDL_FRect{sx - 8 * s, sy - 15 * s, 16 * s, 16 * s} : SDL_FRect{sx - 8 * s, sy - 8 * s, 16 * s, 16 * s};
            atmos.shadowSprite(sp.tex, sp.src, d, isTree ? sy : sy + 6 * s, float(k.shadowX * hgt * tp), float(k.shadowY * hgt * tp));
        }
        for (const Json* p : folk) {
            const auto v = vis.find(ident(*p, "id"));
            if (v == vis.end()) continue;
            Sprite img;
            SDL_FRect d;
            float foot;
            bool beast;
            if (!pawnLook(*p, v->second, sc, img, d, foot, beast)) continue;
            const double hgt = atmos.heightOf(beast ? "beast" : "pawn", beast ? 1.0 : 1.7);
            atmos.shadowSprite(img.tex, img.src, d, foot, float(k.shadowX * hgt * tp), float(k.shadowY * hgt * tp));
        }
        {
            std::unordered_set<long long> seen;
            const double hgt = atmos.heightOf("roof", 3.2);
            for (const auto& [key, c] : chunks)
                for (const auto& r : arr(c.j, "roofs")) {   // z: the roof's step up to the ridge
                    const long long rk = (long long)num(r, "x") * 100003 + (long long)num(r, "y");
                    if (!seen.insert(rk).second) continue;
                    atmos.shadowBox(num(r, "x"), num(r, "y"), num(r, "x") + num(r, "len"), num(r, "y") + num(r, "len"),
                                    hgt + 0.9 * std::max(0.0, num(r, "z") - 1));
                }
        }
        atmos.endShadows();
        // at night the people by a fire cast a shadow away from it
        if (k.lights > 0.3)
            for (const Json* p : folk) {
                const auto v = vis.find(ident(*p, "id"));
                if (v == vis.end()) continue;
                const WLight* best = nullptr;
                double bd = 1e9;
                for (const WLight& l : wlights) {
                    if (l.i < 0.3 || l.key == std::uint64_t(ident(*p, "id"))) continue;
                    const double d = std::hypot(v->second.x - l.x, v->second.y - l.y);
                    if (d < l.r * 0.9 && d > 0.3 && d < bd) bd = d, best = &l;
                }
                if (!best) continue;
                Sprite img;
                SDL_FRect d;
                float foot;
                bool beast;
                if (!pawnLook(*p, v->second, sc, img, d, foot, beast)) continue;
                const double dx = (v->second.x - best->x) / bd, dy = (v->second.y - best->y) / bd;
                const double len = (beast ? 1.0 : 1.7) * tp * (0.35 + 0.9 * bd / best->r);
                const float a = float(0.5 * k.lights * best->i * (1 - bd / (best->r * 0.9)));
                atmos.fireShadow(img.tex, img.src, d, foot, float(dx * len), float(dy * len), a);
            }
    }

    void atmosEnd(double sc) {
        if (!atmos.enabled) return;
        const SkyNow& k = atmos.sky();
        for (const WLight& l : wlights) {
            if (l.smoke) atmos.addSmoke(l.x, l.y - 0.3, l.key, 1.0);
            if (l.i > 0.01) atmos.addLight(l.x, l.y, l.r, l.rgb, l.i, l.flicker, l.key);
        }
        atmos.drawAir();
        atmos.drawLight();
        // what shines by itself: the panes of lit windows, the flames of torches
        for (const auto& p : panes) atmos.glow(p[0], p[1], p[2], p[3], 1.0, 0.8, 0.45, p[4] * k.lights);
        const float s = float(sc);
        for (const auto& tch : torches) {
            float sx, sy;
            toScreen(tch[0], tch[1], sx, sy);
            const double f = atmos.flick(std::uint64_t(tch[2]), 0.5);
            atmos.glow(sx + 5 * s, sy - 13 * s, 1.5f * s, 1 * s, 0.45, 0.3, 0.18, 1.0);              // the stick
            atmos.glow(sx + 5 * s, sy - float(15 + f) * s, 1.5f * s, float(2 + f) * s, 1.0, 0.62, 0.2, 0.95);   // the flame
            atmos.glow(sx + 5.25f * s, sy - 14.5f * s, 1 * s, 1 * s, 1.0, 0.95, 0.6, 1.0);
        }
        for (const auto& f : fxFlames) {   // tongues of the sim's fires
            float sx, sy;
            toScreen(f[0], f[1], sx, sy);
            const std::uint64_t key = std::uint64_t(f[0] * 977 + f[1] * 31);
            for (int k = 0; k < 3; ++k) {
                const double fl = atmos.flick(key + k * 131, 0.8);
                const float w = float((2.5 + f[2] * 2) * s), hgt = float((4 + f[2] * 5) * fl * s), ox = float((k - 1) * 3) * s;
                atmos.glow(sx + ox - w / 2, sy - hgt, w, hgt, 1.0, 0.45 + 0.15 * k, 0.12, 0.85);
                atmos.glow(sx + ox - w / 4, sy - hgt * 0.6f, w / 2, hgt * 0.6f, 1.0, 0.9, 0.5, 0.9);
            }
        }
        atmos.drawPrecip();
    }

    void drawNight() {
        const std::string tm = str(clock, "time");
        int hh = 12, mm = 0;
        if (const auto colon = tm.find(':'); colon != std::string::npos) hh = std::atoi(tm.c_str()), mm = std::atoi(tm.c_str() + colon + 1);
        const double h = hh + mm / 60.0;
        double a = 0;
        if (h >= 21 || h < 4.5) a = 0.55;
        else if (h >= 19.5) a = 0.55 * (h - 19.5) / 1.5;
        else if (h < 6) a = 0.55 * (6 - h) / 1.5;
        if (a > 0) fill(0, 0, mapW(), mapH(), ui::Colour::rgb(14, 18, 48, float(a)));
        const Json w = clock.value("weather", Json::object());
        if (num(w, "rain") > 0.5 && !glyphs) {
            SDL_SetRenderDrawColorFloat(ren, 170 / 255.f, 190 / 255.f, 220 / 255.f, 0.35f);
            const double t = nowMs() / 40;
            const float d = float(dpr());
            for (int i = 0; i < 160; ++i) {
                const float x = float(std::fmod(idHash(i, 9) % std::max(1, int(mapW())) + t * 3, mapW()));
                const float y = float(std::fmod(idHash(i, 5) % std::max(1, int(mapH())) + t * 9, mapH()));
                SDL_RenderLine(ren, x, y, x - 4 * d, y + 10 * d);
            }
        }
    }

    // ======================================================================
    // the interface
    void portrait(const Json& p, float x, float y, float size) {
        fill(x, y, size, size, kLeatherD);
        if (!str(p, "sp").empty()) {
            blit(beastSprite(str(p, "sp"), 'E'), x + size * 0.1f, y + size * 0.1f, size * 0.8f, size * 0.8f);
            return;
        }
        blit(pawnSprite(pawnSpec(p), 'S', 0), x, y, size, size);
    }
    std::string actOf(const Json& p) const {
        if (!str(p, "act").empty()) return str(p, "act");
        if (!str(p, "goal").empty()) return str(p, "goal");
        return L("ui2d.idle");
    }
    // --- LR-137: the pixel HUD ---------------------------------------------------------------
    // where (wx, wy) is: the settlement near it, then the room there or the ground
    std::string placeOf(double wx, double wy) const {
        std::string town;
        double bd = 350.0 * 350.0;
        for (const auto& s : ST.value("settlements", Json::array())) {
            const double dx = num(s, "x") - wx, dy = num(s, "y") - wy, d2 = dx * dx + dy * dy;
            if (d2 < bd) bd = d2, town = str(s, "name");
        }
        const int cx = int(std::floor(wx)), cy = int(std::floor(wy));
        std::string where;
        const Json rooms = ST.value("rooms", Json::object());
        for (const auto& [k, c] : chunks) {
            const int x0 = int(num(c.j, "x0")), y0 = int(num(c.j, "y0")), w = int(num(c.j, "w")), h = int(num(c.j, "h"));
            if (cx < x0 || cy < y0 || cx >= x0 + w || cy >= y0 + h) continue;
            for (const auto& r : arr(c.j, "rooms"))
                if (num(r, "z") == 0 && cx >= num(r, "x0") && cx < num(r, "x1") && cy >= num(r, "y0") && cy < num(r, "y1")) {
                    const std::string ty = str(r, "type");
                    where = rooms.contains(ty) && rooms[ty].is_string() ? rooms[ty].get<std::string>() : ty;
                }
            if (where.empty()) {
                const std::string g = str(c.j, "ground");
                const std::size_t i = std::size_t(cy - y0) * std::size_t(w) + std::size_t(cx - x0);
                const Json names = ST.value("ground", Json::array());
                if (i < g.size()) {
                    const int gi = g[i] - 'a';
                    if (gi >= 0 && gi < int(names.size()) && names[std::size_t(gi)].is_string())
                        where = L("ui2d.place." + names[std::size_t(gi)].get<std::string>());
                }
            }
            break;
        }
        if (town.empty()) town = L("ui2d.place.wild");
        return where.empty() ? town : town + "  >  " + where;
    }
    std::string moodletIcon(const Json& m) const {
        const std::string id = str(m, "id");
        for (const auto& e : HUD.value("moodlets", Json::array()))
            if (e.is_array() && e.size() >= 2) try {
                    if (std::regex_search(id, std::regex(e[0].get<std::string>()))) return e[1].get<std::string>();
                } catch (...) {
                }
        return HUD.value(num(m, "v") >= 0 ? "moodlet_good" : "moodlet_bad", "");
    }
    std::string hoursText(double h) const {
        if (h < 0) return L("ui2d.hud.lasting");
        return L("ui2d.hud.hours", {{"n", std::to_string(int(std::ceil(h)))}});
    }
    std::string signedPct(double v) const { return (v >= 0 ? "+" : "-") + std::to_string(int(std::lround(std::abs(v) * 100))); }
    void tip(const ui::Rect& r, const std::string& t) {
        if (r.contains(in.mouseX, in.mouseY)) tipText = t;
    }
    // the led person's frame (WoW's player frame): the portrait, the name and health / strength
    // in pips, no taller than the portrait (LR-145: what ails them is in the column, the rest in the sheet)
    float heroW() const { return 250 * U; }   // the hero frame's width
    float drawHero(float x, float y) {
        const Json& d = selId == me ? selCard : meCard;
        const Json* pp = person(me);
        if (!pp || !d.is_object()) return 0;
        const float p = px(), r0 = rim() + 2 * U, w = heroW(), port = 56 * U, rowH = 18 * U;
        const float h = 2 * r0 + port;
        carved({x, y, w, h});
        const float ix = x + r0, iy = y + r0;
        well({ix, iy, port, port});
        portrait(*pp, ix + p, iy + p, port - 2 * p);
        const float tx = ix + port + 8 * U, tw = x + w - r0 - tx;
        text(tx, iy, ui.fit(str(d, "name"), tw, fs(16 * U)), kGold, 16 * U, true);
        const double hp = num(d, "hp", 1), st = 1 - num(d, "fatigue");
        struct Row {
            const char* key;
            double v;
            ui::Colour c;
        };
        const Row rows[] = {{"health", hp, kBad}, {"strength", st, kMid}};
        float ry = iy + port - 2 * rowH;
        for (const Row& r : rows) {
            const std::string ic = HUD["vitals"].value(r.key, "");
            icon(ic, tx, ry + (rowH - iconSize()) / 2);
            const float gw = pips(tx + iconSize() + 4 * U, ry + (rowH - 5 * p) / 2, 10, r.v, r.c);
            const std::string n = std::to_string(int(std::lround(r.v * 100)));
            text(tx + iconSize() + 8 * U + gw, ry + 2 * U, n, kText, 13 * U);
            tip({tx, ry, tw, rowH}, L(std::string("ui2d.hud.") + r.key) + ": " + n);
            ry += rowH;
        }
        ui.claim({x, y, w, h});
        return h;
    }
    // the selected other person (WoW's target frame)
    void drawTarget(float x, float y) {
        if (selId < 0 || selId == me || !selCard.is_object()) return;
        const Json* pp = person(selId);
        if (!pp) return;
        const float p = px(), r0 = rim() + 2 * U, w = 250 * U, port = 40 * U, h = 2 * r0 + port;
        carved({x, y, w, h});
        well({x + r0, y + r0, port, port});
        portrait(*pp, x + r0 + p, y + r0 + p, port - 2 * p);
        const float tx = x + r0 + port + 6 * U, tw = x + w - r0 - tx;
        text(tx, y + r0, ui.fit(str(selCard, "name"), tw, fs(13 * U)), kText, 13 * U, true);
        icon(HUD["vitals"].value("health", ""), tx, y + r0 + 16 * U);
        pips(tx + iconSize() + 3 * U, y + r0 + 16 * U + (iconSize() - 5 * p) / 2, 10, num(selCard, "hp", 1), kBad);
        text(tx, y + r0 + 30 * U, ui.fit(actOf(selCard), tw, fs(11 * U)), kDim, 11 * U);
    }
    // the led person's dynasty as party frames; a click selects and looks
    void drawParty(float x, float y) {
        const Json* mp = person(me);
        if (!mp) return;
        const std::string fam = str(*mp, "fam");
        std::vector<const Json*> kin;
        for (const Json& p : people)
            if (ident(p, "id") != me && str(p, "sp").empty() && flag(p, "alive") && !fam.empty() && str(p, "fam") == fam) kin.push_back(&p);
        if (kin.empty()) return;
        std::stable_sort(kin.begin(), kin.end(), [](const Json* a, const Json* b) { return num(*a, "age") > num(*b, "age"); });
        const Json& card = selId == me ? selCard : meCard;
        const std::string famn = card.is_object() && !str(card, "famn").empty() ? str(card, "famn") : fam;
        text(x + 2 * U, y, L("ui2d.hud.dynasty", {{"name", famn}}), kGold, 12 * U, true);
        y += 18 * U;
        const float p = px(), w = heroW(), h = 38 * U, port = 28 * U;
        int i = 0;
        for (const Json* k : kin) {
            if (i++ >= 7 || y + h > mapH() - 80 * U) break;
            const long long id = ident(*k, "id");
            const ui::Rect r{x, y, w, h};
            plate(r, id == selId);
            well({x + 2 * p + 2 * U, y + (h - port) / 2, port, port});
            portrait(*k, x + 3 * p + 2 * U, y + (h - port) / 2 + p, port - 2 * p);
            const float tx = x + 2 * p + port + 7 * U, tw = x + w - 2 * p - tx;
            text(tx, y + 3 * U, ui.fit(str(*k, "name"), tw, fs(12 * U)), kText, 12 * U);
            const float gw = pips(tx, y + 22 * U, 8, num(*k, "hp", 1), kBad);
            text(tx + gw + 5 * U, y + 19 * U, ui.fit(actOf(*k), tw - gw - 5 * U, fs(10 * U)), kDim, 10 * U);
            if (r.contains(in.mouseX, in.mouseY)) {
                tipText = str(*k, "name") + " · " + actOf(*k);
                if (in.pressed && !screens->any()) {
                    select(id);
                    if (vis.count(id)) camX = vis[id].x, camY = vis[id].y;
                }
            }
            y += h + 3 * U;
        }
    }
    // where the led person (or the camera) is: settlement > room or ground
    float drawPlace(float x, float y) {
        double wx = camX, wy = camY;
        if (const Json* mp = person(me)) {
            wx = num(*mp, "x"), wy = num(*mp, "y");
            if (vis.count(me)) wx = vis[me].x, wy = vis[me].y;
        }
        const std::string s = placeOf(wx, wy);
        const float h = 24 * U, w = textW(s, 13 * U) + iconSize() + 20 * U;
        parchBox({x, y, w, h});
        icon("hud_pin", x + 5 * U, y + (h - iconSize()) / 2);
        text(x + iconSize() + 11 * U, y + (h - textH(13 * U)) / 2, s, kInk, 13 * U);
        ui.claim({x, y, w, h});
        return h;
    }

    // a fold row of the column: an arrow, a gold label, a note on the right; a click opens / shuts it
    bool foldRow(const std::string& id, float x, float& y, float w, const std::string& label, const std::string& note) {
        const float h = 20 * U;
        const bool open = !folded.count(id);
        const ui::Rect r{x, y, w, h};
        well(r);
        const bool over = r.contains(in.mouseX, in.mouseY);
        text(x + 5 * U, y + 3 * U, open ? "v" : ">", kGold, 12 * U);
        text(x + 18 * U, y + 3 * U, label, over ? kText : kGold, 13 * U);
        if (!note.empty()) text(x + w - 5 * U - textW(note, 11 * U), y + 4 * U, note, kDim, 11 * U);
        if (over && in.pressed && !screens->any()) {
            if (open) folded.insert(id);
            else folded.erase(id);
        }
        y += h + 3 * U;
        return open;
    }
    void drawSide() {
        const ui::Rect side{mapW(), 0, sideW(), mapH()};
        carved(side);
        const float r0 = rim() + 3 * U, x = side.x + r0, w = side.w - 2 * r0;
        // LR-145: the screens as icon buttons (hud2d.json "screens", the key letter in the corner,
        // the name in the hint), the menu apart on the right with its own label
        const float bh = 42 * U, gap = 3 * U, mw = 58 * U;
        std::vector<std::pair<std::string, std::string>> tabs;
        std::string menuKey;
        for (const auto& sk : screens->columnScreens())
            if (sk.first == "menu") menuKey = sk.second;
            else tabs.push_back(sk);
        const float bw = std::floor((w - mw - 8 * U - gap * float(tabs.size() - 1)) / float(tabs.size()));
        const Json scrIcons = HUD.value("screens", Json::object());
        for (size_t i = 0; i < tabs.size(); ++i) {
            const auto& [id, k] = tabs[i];
            const ui::Rect r{x + float(i) * (bw + gap), r0, bw, bh};
            if (ui.toggle(ui::widgetId("side.screen", int(i)), r, "", screens->isOpen(id))) screens->openScreen(id);
            const float is = 2 * iconSize();
            icon(scrIcons.value(id, ""), r.x + (r.w - is) / 2, r.y + (r.h - is) / 2 + 5 * U, is);
            text(r.x + 3 * U, r.y + 1 * U, k, kInk, 10 * U);
            tip(r, L("ui2d.screen." + id) + " (" + k + ")");
        }
        {
            const ui::Rect r{x + w - mw, r0, mw, bh};
            if (ui.button(ui::widgetId("side.menu"), r, "", hexc("#b8683a"), kInk)) screens->openScreen("menu");
            const float p = px(), lw = 11 * p, lx = r.x + (r.w - lw) / 2;
            for (int i = 0; i < 3; ++i) fill(lx, r.y + 7 * U + float(i) * 3 * p, lw, p, kInk);
            const std::string ml = L("ui2d.screen.menu");
            text(r.x + (r.w - textW(ml, 11 * U)) / 2, r.y + r.h - textH(11 * U) - 3 * U, ml, kInk, 11 * U);
            tip(r, ml + " (" + menuKey + ")");
        }
        const float cy = r0 + bh + 8 * U;
        drawCard({x, cy, w, side.bottom() - cy - r0});
    }
    // words over as many lines as they need inside `width`
    std::vector<std::string> wrap(const std::string& s, float width, float px) const override {
        std::vector<std::string> out;
        std::string line, word;
        std::istringstream ws(s);
        while (ws >> word) {
            const std::string tried = line.empty() ? word : line + " " + word;
            if (!line.empty() && textW(tried, px) > width) {
                out.push_back(line);
                line = word;
            } else line = tried;
        }
        if (!line.empty() || out.empty()) out.push_back(line);
        for (auto& l : out) l = ui.fit(l, width, fs(px));
        return out;
    }
    float parchLines(float x, float y, float w, const std::vector<std::string>& ls, float px) override {
        std::vector<std::string> rows;
        for (const auto& l : ls)
            for (auto& r : wrap(l, w - 16 * U, px)) rows.push_back(r);
        const float lh = textH(px) + 5 * U, h = rows.size() * lh + 8 * U;
        parchBox({x, y, w, h});
        float yy = y + 5 * U;
        for (const auto& r : rows) {
            text(x + 8 * U, yy, r, kInk, px);
            yy += lh;
        }
        return h;
    }
    float chips(float x, float y, float w, const std::vector<std::string>& items) override {
        float cx = x, cy = y;
        const float px = 13 * U, h = textH(px) + 8 * U;
        for (const auto& it : items) {
            const float cw = std::min(w, textW(it, px) + 12 * U);
            if (cx + cw > x + w && cx > x) cx = x, cy += h + 4 * U;
            fill(cx, cy, cw, h, kLeatherD);
            frame(cx, cy, cw, h, kBronze.withAlpha(0.6f), 1);
            text(cx + 6 * U, cy + 4 * U, ui.fit(it, cw - 12 * U, fs(px)), kText, px);
            cx += cw + 4 * U;
        }
        return cy + h - y;
    }
    // the column's card of the selected person: who and what now, the needs by tier
    // (hud2d.json tiers, each folds) and the thoughts. The rest is in the sheet (C).
    void drawCard(const ui::Rect& r) {
        const Json& d = selCard;
        const Json* p = person(selId);
        if (!d.is_object() || !p) {
            parchLines(r.x, r.y, r.w, {L("ui2d.nobody")}, 13 * U);
            return;
        }
        const ui::Rect c = ui.beginScroll(ui::widgetId("side.card"), r);
        float y = c.y;
        const float w = c.w - 2 * U, x = c.x, pp = px();
        const bool human = str(*p, "sp").empty();
        // who: one line, the name and lead / release (the portrait and the rest are in the
        // frames over the map and in the sheet, LR-145)
        float bw = 0;
        // LR-138: one leads only one's own kin
        if (human && flag(d, "alive") && (ident(d, "id") == me || !d.contains("lead") || flag(d, "lead"))) {
            const long long id = ident(d, "id");
            const std::string lbl = L(id == me ? "ui2d.release" : "ui2d.lead");
            bw = textW(lbl, 12 * U) + 16 * U;
            if (ui.button(ui::widgetId("card.lead"), {x + w - bw, y, bw, 20 * U}, lbl)) {
                if (id == me) cmdToast({{"op", "cmd"}, {"type", "release_control"}, {"args", Json::object()}});
                else cmdToast({{"op", "cmd"}, {"type", "take_control"}, {"args", {{"id", id}}}});
                poll();
            }
        }
        text(x + 2 * U, y + 2 * U, ui.fit(str(d, "name"), w - bw - 8 * U, fs(14 * U)), kGold, 14 * U, true);
        y += 24 * U;
        if (!flag(d, "alive")) {
            std::string dead = L("ui2d.dead");
            if (!str(d, "cause").empty()) dead += ": " + str(d, "cause");
            y += parchLines(x, y, w, {dead}, 12 * U) + 5 * U;
        }
        // now: the act and what it is for, one parchment line
        {
            std::string now = L("ui2d.doing") + ": " + (str(d, "act").empty() ? L("ui2d.idle") : str(d, "act"));
            if (!str(d, "goal").empty()) now += "  >  " + str(d, "goal");
            const float h = 20 * U;
            parchBox({x, y, w, h});
            text(x + 5 * U, y + (h - textH(12 * U)) / 2, ui.fit(now, w - 10 * U, fs(12 * U)), kInk, 12 * U);
            tip({x, y, w, h}, now);
            y += h + 5 * U;
        }
        // LR-145: the condition as chips: wounds, bleeding, worn out (was in the hero frame)
        {
            std::vector<std::pair<std::string, ui::Colour>> cond;
            bool bleeding = false;
            for (const auto& wd : arr(d, "wounds")) {
                std::string t = str(wd, "sev");
                if (!str(wd, "zone").empty()) t += (t.empty() ? "" : ", ") + str(wd, "zone");
                if (!t.empty()) cond.push_back({t, hexc("#e06048")});
                bleeding = bleeding || flag(wd, "bleeding");
            }
            if (bleeding) cond.push_back({L("ui2d.hud.bleeding"), hexc("#e06048")});
            if (num(d, "fatigue") > 0.7) cond.push_back({L("ui2d.hud.tired"), kGold});
            float cx = x;
            const float px = 11 * U, ch = textH(px) + 6 * U;
            for (const auto& [t, col] : cond) {
                const float cw = std::min(w, textW(t, px) + 10 * U);
                if (cx + cw > x + w && cx > x) cx = x, y += ch + 3 * U;
                well({cx, y, cw, ch});
                text(cx + 5 * U, y + 3 * U, ui.fit(t, cw - 10 * U, fs(px)), col, px);
                cx += cw + 3 * U;
            }
            if (!cond.empty()) y += ch + 6 * U;
        }
        // needs by tier: the tier's row shows its worst need, opened it lists them
        const Json& needs = arr(d, "needs");
        if (!needs.empty()) {
            text(x + 2 * U, y, L("ui2d.needs"), kGold, 13 * U, true);
            y += 19 * U;
            std::set<std::string> placed;
            // a need that matters little to this person (care, LR-140: grace of a non-believer) is dimmed
            auto careless = [](const Json& n) { return n.contains("care") && n["care"].is_number() && n["care"].get<double>() < 0.3; };
            auto tierRows = [&](const std::string& tid, const std::string& title, double sat,
                                const std::vector<std::pair<const Json*, std::string>>& list) {
                if (list.empty()) return;
                double worst = 1;
                for (const auto& [n, ic] : list)
                    if (!careless(*n)) worst = std::min(worst, num(*n, "v"));
                if (sat >= 0) worst = sat;   // the sim's own measure of the tier
                // a fold row with the worst need's pips inside
                const float h = 20 * U;
                const bool open = !folded.count("tier." + tid);
                const ui::Rect rr{x, y, w, h};
                well(rr);
                const bool over = rr.contains(in.mouseX, in.mouseY);
                text(x + 5 * U, y + 3 * U, open ? "v" : ">", kGold, 12 * U);
                text(x + 18 * U, y + 3 * U, title, over ? kText : kGold, 13 * U);
                const std::string wn = std::to_string(int(std::lround(worst * 100)));
                text(x + w - 5 * U - textW(wn, 11 * U), y + 4 * U, wn, kDim, 11 * U);
                pips(x + w - 40 * U - 5 * 4 * pp, y + (h - 4 * pp) / 2, 5, worst, tone(worst));
                if (over && in.pressed && !screens->any()) {
                    if (open) folded.insert("tier." + tid);
                    else folded.erase("tier." + tid);
                }
                y += h + 2 * U;
                if (!open) return;
                for (const auto& [n, ic] : list) {
                    const double v = num(*n, "v");
                    const float rh = 19 * U;
                    icon(ic, x + 12 * U, y + (rh - iconSize()) / 2);
                    const std::string vn = std::to_string(int(std::lround(v * 100)));
                    const ui::Colour nc = careless(*n) ? kDim : v < 0.6 ? kGold : kText;
                    text(x + 12 * U + iconSize() + 5 * U, y + 3 * U, ui.fit(str(*n, "name"), w - 120 * U, fs(12 * U)), nc, 12 * U);
                    pips(x + w - 40 * U - 5 * 4 * pp, y + (rh - 4 * pp) / 2, 5, v, tone(v));
                    text(x + w - 5 * U - textW(vn, 11 * U), y + 4 * U, vn, kDim, 11 * U);
                    y += rh;
                }
                y += 3 * U;
            };
            // the icons by need id (hud2d.json)
            std::map<std::string, std::string> needIcon;
            for (const auto& t : HUD.value("tiers", Json::array()))
                for (const auto& e : arr(t, "needs"))
                    if (e.is_array() && e.size() >= 2) needIcon[e[0].get<std::string>()] = e[1].get<std::string>();
            auto iconOf = [&](const std::string& id) {
                const auto it = needIcon.find(id);
                return it != needIcon.end() ? it->second : HUD.value("other_need", "");
            };
            auto take = [&](const std::string& id, std::vector<std::pair<const Json*, std::string>>& list) {
                for (const auto& n : needs)
                    if (str(n, "id") == id && !placed.count(id)) {
                        list.push_back({&n, iconOf(id)});
                        placed.insert(id);
                    }
            };
            if (const Json& tiers = arr(d, "tiers"); !tiers.empty()) {
                // LR-140: the sim's tiers, in the pyramid's order, with their names and fill
                for (const auto& t : tiers) {
                    std::vector<std::pair<const Json*, std::string>> list;
                    for (const auto& id : arr(t, "needs"))
                        if (id.is_string()) take(id.get<std::string>(), list);
                    tierRows(str(t, "id"), str(t, "name"), t.contains("sat") && t["sat"].is_number() ? t["sat"].get<double>() : -1, list);
                }
            } else
                for (const auto& t : HUD.value("tiers", Json::array())) {   // an older sim: the tiers of hud2d.json
                    std::vector<std::pair<const Json*, std::string>> list;
                    for (const auto& e : arr(t, "needs"))
                        if (e.is_array() && !e.empty()) take(e[0].get<std::string>(), list);
                    tierRows(str(t, "id"), L("ui2d.tier." + str(t, "id")), -1, list);
                }
            std::vector<std::pair<const Json*, std::string>> rest;
            for (const auto& n : needs)
                if (!placed.count(str(n, "id"))) rest.push_back({&n, iconOf(str(n, "id"))});
            tierRows("other", L("ui2d.tier.other"), -1, rest);
            y += 3 * U;
        }
        // thoughts
        const Json& moods = arr(d, "moodlets");
        if (!moods.empty()) {
            double sum = 0;
            for (const auto& m : moods) sum += num(m, "v");
            if (foldRow("thoughts", x, y, w, L("ui2d.hud.thoughts"), signedPct(sum))) {
                for (const auto& m : moods) {
                    const float rh = 19 * U;
                    const double v = num(m, "v");
                    icon(moodletIcon(m), x + 4 * U, y + (rh - iconSize()) / 2);
                    const std::string vs = signedPct(v);
                    text(x + iconSize() + 8 * U, y + 3 * U, vs, v >= 0 ? hexc("#8ac85a") : hexc("#e06048"), 12 * U);
                    const std::string hrs = hoursText(num(m, "h", -1));
                    text(x + iconSize() + 40 * U, y + 3 * U, ui.fit(str(m, "name"), w - iconSize() - 48 * U - textW(hrs, 11 * U), fs(12 * U)), kText, 12 * U);
                    text(x + w - textW(hrs, 11 * U), y + 4 * U, hrs, kDim, 11 * U);
                    y += rh;
                }
                y += 3 * U;
            }
        }
        ui.endScroll(y);
    }
    void drawBar() {
        const ui::Rect b{0, mapH(), float(W), barH()};
        carved(b);
        const float pad = rim() + 2 * U, h = b.h - 2 * pad, y = b.y + pad;
        float x = b.right() - pad;
        auto btn = [&](const char* id, const std::string& label, float w, bool on) {
            x -= w;
            const bool hit = ui.toggle(ui::widgetId(id), {x, y, w, h}, label, on);
            x -= 3 * U;
            return hit;
        };
        if (btn("bar.follow", L("ui2d.follow"), 84 * U, follow)) follow = !follow && (selId >= 0 || me >= 0);
        if (btn("bar.mode", L(glyphs ? "ui2d.mode.glyphs" : "ui2d.mode.sprites"), 84 * U, false)) glyphs = !glyphs;
        if (btn("bar.roofs", L("ui2d.roofs." + roofMode), 104 * U, false)) cycleRoofs();
        if (btn("bar.zoom", L("ui2d.zoom", {{"n", std::to_string(zoom)}}), 64 * U, false)) zoom = zoom % 3 + 1;
        if (btn("bar.faster", "+", 24 * U, false)) setSpeed(1);
        {
            const std::string sp = paused ? L("ui2d.paused") : L("ui2d.speed", {{"n", tpsText()}});
            const float w = textW(sp, 12 * U) + 12 * U;
            x -= w;
            ui.textCentred({x, y, w, h}, sp, kText, fs(12 * U));
            x -= 3 * U;
        }
        if (btn("bar.slower", "-", 24 * U, false)) setSpeed(-1);
        if (btn("bar.pause", paused ? ">" : "||", 30 * U, paused)) togglePause();
        // the clock and the hint on parchment
        std::string clk;
        if (clock.contains("day")) {
            clk = L("ui2d.day", {{"n", std::to_string(int(num(clock, "day")) + 1)}}) + " · " + L("ui2d.season." + str(clock, "season")) + " · " +
                  str(clock, "time");
            const Json w = clock.value("weather", Json::object());
            if (w.is_object() && w.contains("t"))
                clk += " · " + std::to_string(int(std::lround(num(w, "t")))) + "° " + (num(w, "rain") > 0.5 ? L("ui2d.rain") : L("ui2d.dry"));
        }
        const float cw = textW(clk, 12 * U) + 16 * U;
        x -= cw + 2 * U;
        parchBox({x, y, cw, h});
        text(x + 8 * U, y + (h - textH(12 * U)) / 2, clk, kInk, 12 * U);
        x -= 8 * U;
        const ui::Rect hr{b.x + pad + 2 * U, y, x - b.x - pad - 2 * U, h};
        text(hr.x, y + (h - textH(12 * U)) / 2, ui.fit(hintText(), hr.w, fs(12 * U)), kText, 12 * U);
    }
    std::string tpsText() const {
        char b[32];
        std::snprintf(b, sizeof b, tps == std::floor(tps) ? "%.0f" : "%.1f", tps);
        return b;
    }
    std::string hintText() const {
        if (!tipText.empty()) return tipText;
        if (const Json* p = person(hoverId)) {
            std::string s = L("ui2d.hint.person", {{"name", str(*p, "name")}, {"act", actOf(*p)}});
            if (p->contains("dest") && (*p)["dest"].is_array() && (*p)["dest"].size() >= 2)
                s += " · " + L("ui2d.hint.walk", {{"m", std::to_string(int(std::lround(std::hypot((*p)["dest"][0].get<double>() - num(*p, "x"),
                                                                                                  (*p)["dest"][1].get<double>() - num(*p, "y")))))}});
            return s;
        }
        if (haveHover) {
            const Json* thing = nullptr;
            const Json* room = nullptr;
            for (const auto& [k, c] : chunks) {
                for (const auto& t : arr(c.j, "things"))
                    if (t.is_array() && t.size() >= 4 && !hidden.count(t[1].get<std::string>()) && std::abs(t[2].get<double>() - hoverX) < 0.6 &&
                        std::abs(t[3].get<double>() - hoverY) < 0.6)
                        thing = &t;
                for (const auto& r : arr(c.j, "rooms"))
                    if (num(r, "z") == 0 && hoverX >= num(r, "x0") && hoverX < num(r, "x1") && hoverY >= num(r, "y0") && hoverY < num(r, "y1")) room = &r;
            }
            std::string s;
            const Json objs = ST.value("objects", Json::object()), rooms = ST.value("rooms", Json::object());
            if (thing) {
                const std::string ty = (*thing)[1].get<std::string>();
                s = L("ui2d.hint.thing", {{"name", objs.contains(ty) ? objs[ty].get<std::string>() : ty}});
            }
            if (room) {
                const std::string ty = str(*room, "type");
                if (!s.empty()) s += " · ";
                s += L("ui2d.hint.room", {{"room", rooms.contains(ty) ? rooms[ty].get<std::string>() : ty}});
            }
            if (!s.empty()) return s;
        }
        return L("ui2d.hint.keys");
    }
    void drawOverlay() {
        tipText.clear();
        const double t = nowMs();
        // toast
        if (t < toastUntil && !toastText.empty()) {
            const float px = 15 * U, w = textW(toastText, px) + 28 * U, h = textH(px) + 12 * U;
            const float x = mapW() / 2 - w / 2, y = 18 * U;
            fill(x, y, w, h, kLeather);
            frame(x, y, w, h, kBronze, std::max(1.0f, 2 * U));
            textC(mapW() / 2, y + 6 * U, toastText, kGold, px);
        }
        // what people said, bottom left
        {
            float y = mapH() - (me >= 0 ? 70 : 16) * U;   // over the quick bar when it is there
            const float px = 14 * U;
            for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
                if (t - it->second > 40000) continue;
                y -= textH(px) + 6 * U;
                const float a = t - it->second > 15000 ? 0.55f : 1.0f;
                const std::string s = ui.fit(it->first, mapW() * 0.45f, fs(px));
                fill(12 * U, y - 2 * U, textW(s, px) + 16 * U, textH(px) + 4 * U, ui::Colour(0, 0, 0, 0.45f * a));
                text(20 * U, y, s, kText.withAlpha(a), px, true);
            }
        }
        // LR-137: the hero's frame, the target, where, the dynasty (top left of the map)
        {
            const float x0 = 8 * U, y0 = 8 * U;
            const float hh = drawHero(x0, y0);
            const float px0 = x0 + (hh > 0 ? heroW() + 8 * U : 0);
            const float ph = drawPlace(px0, y0);
            drawTarget(px0, y0 + ph + 6 * U);
            drawParty(x0, y0 + (hh > 0 ? hh + 8 * U : ph + 8 * U));
        }
        // the dock: the act going on and the quick bar (icons of map2d.json acts, names in the hint)
        if (me >= 0) {
            const float p = px(), sw = 44 * U, gap = 2 * U, total = 8 * sw + 7 * gap, r0 = rim();
            const float x0 = std::round(mapW() / 2 - total / 2), y0 = std::round(mapH() - sh_dock() - 8 * U + r0);
            carved({x0 - r0 - 2 * U, y0 - r0 - 2 * U, total + 2 * r0 + 4 * U, sw + 2 * r0 + 4 * U});
            const Json* mp = person(me);
            if (mp && mp->contains("prog") && (*mp)["prog"].is_number() && !str(*mp, "act").empty()) {
                const float gw = 50 * 4 * p / 2;
                const float cy = y0 - r0 - 12 * U - 4 * p;
                const std::string act = str(*mp, "act");
                text(mapW() / 2 - gw / 2 - textW(act, 13 * U) - 8 * U, cy - 4 * U, act, kGold, 13 * U, true);
                gauge({mapW() / 2 - gw / 2, cy, gw, 4 * p}, num(*mp, "prog"), kGold);
            }
            for (int i = 0; i < 8; ++i) {
                const ui::Rect r{x0 + i * (sw + gap), y0, sw, sw};
                const Slot& s = slots[std::size_t(i)];
                const Json c = i < int(checked.size()) ? checked[std::size_t(i)] : Json();
                const bool off = !s.id.empty() && c.is_object() && !flag(c, "can");
                well(r);
                if (off) frame(r.x, r.y, r.w, r.h, kBad.withAlpha(0.7f), p);
                ui.claim(r);
                if (!s.id.empty()) {
                    const std::string name = c.is_object() && !str(c, "name").empty() ? str(c, "name") : s.id;
                    std::string ic;
                    for (const auto& [re, sp] : actsRe)
                        if (std::regex_search(s.id, re)) {
                            ic = sp;
                            break;
                        }
                    if (!ic.empty()) blit(sprite(ic), r.x + (sw - 32 * U) / 2, r.y + (sw - 32 * U) / 2 + 2 * U, 32 * U, 32 * U, off ? 0.45f : 1.0f);
                    else {
                        auto rows = wrap(name, r.w - 8 * U, 10 * U);
                        for (std::size_t k = 0; k < rows.size() && k < 2; ++k)
                            text(r.x + 4 * U, r.y + 14 * U + k * (textH(10 * U) + 1 * U), rows[k], off ? kDim : kText, 10 * U);
                    }
                    tip(r, std::to_string(i + 1) + ": " + name + (off && !str(c, "why").empty() ? " (" + str(c, "why") + ")" : ""));
                }
                text(r.x + 3 * U, r.y + 1 * U, std::to_string(i + 1), kGold, 11 * U, true);
                if (r.contains(in.mouseX, in.mouseY) && !screens->any()) {
                    if (in.pressed) useSlot(i);
                    if (in.rightPressed) {
                        slots[std::size_t(i)] = Slot{};
                        checkBar();
                    }
                }
            }
        }
        drawMenu();
    }
    float sh_dock() const { return 44 * U; }
    void drawMenu() {
        if (!menuOpen) return;
        const float w = 340 * U, rowH = 40 * U, pad = 8 * U;
        std::size_t n = menuActs.size();
        const bool withPerson = menuTarget >= 0 && person(menuTarget) && str(*person(menuTarget), "sp").empty();
        const float extra = withPerson ? 38 * U : 0;   // talk / trade over the acts
        float h = 34 * U + extra + std::max<std::size_t>(n, 1) * (rowH + 2 * U) + pad;
        h = std::min(h, mapH() - 20 * U);
        float x = std::min(menuX + 14 * U, mapW() - w - 10 * U), y = std::max(10 * U, std::min(menuY - 20 * U, mapH() - h - 10 * U));
        const ui::Rect box{x, y, w, h};
        fill(box.x, box.y, box.w, box.h, kLeather);
        frame(box.x, box.y, box.w, box.h, kBronze, std::max(1.0f, 2 * U));
        ui.claim(box);
        text(x + pad, y + 6 * U, ui.fit(menuTitle, w - 2 * pad, fs(16 * U)), kGold, 16 * U, true);
        if (withPerson) {
            const float bw = (w - 2 * pad - 6 * U) / 2;
            if (ui.button(ui::widgetId("menu.talk"), {x + pad, y + 32 * U, bw, 30 * U}, L("ui2d.talk.open"))) {
                menuOpen = false;
                screens->open("talk", Json(menuTarget));
                return;
            }
            if (ui.button(ui::widgetId("menu.trade"), {x + pad + bw + 6 * U, y + 32 * U, bw, 30 * U}, L("ui2d.talk.trade"))) {
                menuOpen = false;
                screens->open("trade", Json(menuTarget));
                return;
            }
        }
        const ui::Rect listR{x + pad, y + 32 * U + extra, w - 2 * pad, h - 36 * U - extra};
        const ui::Rect c = ui.beginScroll(ui::widgetId("menu.acts"), listR);
        float yy = c.y;
        if (menuActs.empty()) {
            parchLines(c.x, yy, c.w, {L("ui2d.no_acts")}, 14 * U);
            yy += 30 * U;
        }
        int i = 0;
        std::string chosen;
        Json expand;   // LR-123: a parametric act's choices replace the list
        Json direct;   // LR-122: a sim command of its own (wear / take_off)
        for (const auto& a : menuActs) {
            const bool can = flag(a, "can");
            const ui::Rect r{c.x, yy, c.w, rowH};
            const bool hit = ui.selectable(ui::widgetId("menu.act", i++), r, false);
            const float mins = float(num(a, "minutes"));
            const std::string tm = mins >= 60 ? L("ui2d.hours", {{"n", std::to_string(int(std::lround(mins / 60)))}})
                                              : L("ui2d.minutes", {{"n", std::to_string(int(mins))}});
            text(r.x + 6 * U, r.y + 3 * U, ui.fit(str(a, "name"), r.w - 90 * U, fs(14 * U)), can ? kText : kDim, 14 * U);
            text(r.right() - textW(tm, 12 * U) - 30 * U, r.y + 4 * U, tm, kDim, 12 * U);
            // pin to the quick bar
            const ui::Rect pin{r.right() - 24 * U, r.y + 2 * U, 22 * U, 18 * U};
            text(pin.x + 4 * U, pin.y, "+", kGold, 15 * U);
            std::string sub;
            for (const auto& g : a.value("gains", Json::array())) sub += (sub.empty() ? "" : ", ") + g.get<std::string>();
            for (const auto& g : a.value("to_target", Json::array())) sub += (sub.empty() ? "→ " : ", → ") + g.get<std::string>();
            if (!can && !str(a, "why").empty()) sub = str(a, "why");
            for (const auto& g : a.value("legal", Json::array())) sub += (sub.empty() ? "" : " · ") + g.get<std::string>();
            text(r.x + 6 * U, r.y + 21 * U, ui.fit(sub, r.w - 12 * U, fs(11 * U)), !can ? kBad : kDim, 11 * U);
            if (hit) {
                if (pin.contains(in.mouseX, in.mouseY)) pinToBar(Slot{"act", str(a, "id"), menuTarget >= 0 && person(menuTarget) && str(*person(menuTarget), "sp").empty()});
                else if (can && a.contains("choices") && a["choices"].is_array()) expand = a;
                else if (can && !str(a, "cmd").empty()) direct = a;   // LR-122: put on / take off
                else if (can) chosen = str(a, "id");
            }
            yy += rowH + 2 * U;
        }
        ui.endScroll(yy);
        if (direct.is_object()) {
            menuOpen = false;
            cmdToast({{"op", "cmd"}, {"type", str(direct, "cmd")}, {"args", {{"id", me}, {"item", str(direct, "item")}}}});
            poll();
            return;
        }
        if (expand.is_object()) {
            Json list = Json::array();
            for (Json ch : expand["choices"]) {
                ch["minutes"] = expand.value("minutes", 0);
                ch["to_target"] = Json::array();
                ch["legal"] = Json::array();
                list.push_back(ch);
            }
            menuActs = list;
            menuTitle = str(expand, "name");
            return;
        }
        if (!chosen.empty()) {
            menuOpen = false;
            perform(chosen, menuTarget);
        }
        if (in.pressed && !box.contains(in.mouseX, in.mouseY)) menuOpen = false;
    }
    void openMenu(long long target, const std::string& title, float sx, float sy) {
        if (me < 0) {
            toast(L("ui2d.lead_first"));
            return;
        }
        Json acts = get("/api/acts" + (target >= 0 ? "?target=" + std::to_string(target) : std::string()));
        if (!acts.is_array()) acts = Json::array();
        std::stable_sort(acts.begin(), acts.end(), [](const Json& a, const Json& b) { return flag(a, "can") > flag(b, "can"); });
        menuActs = acts;
        menuTarget = target;
        menuTitle = title;
        menuX = sx, menuY = sy;
        menuOpen = true;
    }
    void perform(const std::string& id, long long target) {
        path.clear();
        if (vis.count(me)) vis[me].driven = false;
        Json args = {{"id", me}, {"interaction", id}};
        if (target >= 0) args["target"] = target;
        cmdToast({{"op", "cmd"}, {"type", "perform"}, {"args", args}});
        poll();
    }
    void pinToBar(const Slot& s) {
        for (std::size_t i = 0; i < slots.size(); ++i)
            if (slots[i].id.empty()) {
                slots[i] = s;
                checkBar();
                toast(L("ui2d.pinned", {{"n", std::to_string(i + 1)}}));
                return;
            }
        toast(L("ui2d.bar_full"));
    }
    void useSlot(int i) {
        const Slot& s = slots[std::size_t(i)];
        if (s.id.empty() || me < 0) return;
        const Json c = i < int(checked.size()) ? checked[std::size_t(i)] : Json();
        if (c.is_object() && !flag(c, "can")) {
            toast(str(c, "why").empty() ? L("ui2d.cannot") : str(c, "why"));
            return;
        }
        if (s.kind == "goal") {
            path.clear();
            cmdToast({{"op", "cmd"}, {"type", "set_goal"}, {"args", {{"id", me}, {"goal", s.id}}}});
        } else perform(s.id, c.is_object() && flag(c, "needs_target") ? selId : -1);
    }

    // --- controls ------------------------------------------------------------------------
    static constexpr double SPEEDS[] = {0.5, 1, 2, 4, 8, 16, 48};
    void setSpeed(int dir) {
        int i = 0;
        while (i < 7 && SPEEDS[i] < tps) ++i;
        if (i >= 7) i = 6;
        i = std::clamp(i + dir, 0, 6);
        tps = SPEEDS[i];
        cmd({{"op", "speed"}, {"tps", tps}});
    }
    void togglePause() {
        paused = !paused;
        cmd({{"op", "pause"}, {"on", paused}});
    }
    void cycleRoofs() { roofMode = roofMode == "auto" ? "all" : roofMode == "all" ? "none" : "auto"; }
    void toggleFullscreen() {
        fullscreen = !fullscreen;
        SDL_SetWindowFullscreen(win, fullscreen);
    }
    bool key(SDL_Scancode sc) {
        if (ui.typing()) return false;
        if (sc == SDL_SCANCODE_ESCAPE && menuOpen) {
            menuOpen = false;
            return true;
        }
        if (sc == SDL_SCANCODE_F11) {
            toggleFullscreen();
            return true;
        }
        if (screens && screens->key(sc, SDL_GetModState())) {
            menuOpen = false;
            return true;
        }
        const bool leading = me >= 0;
        if (leading && sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_8 && !altDown) {
            useSlot(int(sc - SDL_SCANCODE_1));
            return true;
        }
        switch (sc) {
        case SDL_SCANCODE_X:
            if (leading) {
                path.clear();
                cmd({{"op", "cmd"}, {"type", "cancel_goal"}, {"args", {{"id", me}}}});
                poll();
            }
            return true;
        case SDL_SCANCODE_SPACE: togglePause(); return true;
        case SDL_SCANCODE_Z: zoom = zoom % 3 + 1; return true;
        case SDL_SCANCODE_R: cycleRoofs(); return true;
        case SDL_SCANCODE_G: glyphs = !glyphs; return true;
        case SDL_SCANCODE_F: follow = !follow && (selId >= 0 || me >= 0); return true;
        case SDL_SCANCODE_F11: toggleFullscreen(); return true;
        case SDL_SCANCODE_ESCAPE:
            if (menuOpen) menuOpen = false;
            else select(-1);
            return true;
        case SDL_SCANCODE_MINUS:
        case SDL_SCANCODE_KP_MINUS: setSpeed(-1); return true;
        case SDL_SCANCODE_EQUALS:
        case SDL_SCANCODE_KP_PLUS: setSpeed(1); return true;
        default: return false;
        }
    }

    // LR-139: pawns2d.json is read at the start and again whenever it changes on disk, so
    // a new cut or a kind's ears show at once, without a rebuild or a restart.
    std::filesystem::file_time_type partsTime{};
    double partsChecked = 0;
    void loadPawnParts() {
        const auto path = root / "content/sprites_2d/pawns2d.json";
        std::error_code ec;
        partsTime = std::filesystem::last_write_time(path, ec);
        setPawnParts(parseOr(readText(path), Json::object()));
    }
    void watchPawnParts() {
        if (nowMs() - partsChecked < 1000) return;
        partsChecked = nowMs();
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(root / "content/sprites_2d/pawns2d.json", ec);
        if (ec || t == partsTime) return;
        loadPawnParts();
        atlas.clear();   // every sprite is drawn again on demand
    }

    // --- one frame -------------------------------------------------------------------------
    bool events() {
        watchPawnParts();
        in.pressed = in.released = in.rightPressed = in.doubleClick = false;
        in.wheel = 0;
        in.text.clear();
        in.backspace = in.enter = in.escape = in.tab = false;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED: return false;
            case SDL_EVENT_RENDER_TARGETS_RESET:
            case SDL_EVENT_RENDER_DEVICE_RESET:
                for (auto& [k, c] : chunks) c.painted = false;
                if (e.type == SDL_EVENT_RENDER_DEVICE_RESET) {
                    for (auto& [k, c] : chunks) c.tex = nullptr;
                    atlas.clear();
                    atmos.release();
                    ui.shutdown();
                    ui.init(ren);
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST: altDown = false; break;
            case SDL_EVENT_KEY_DOWN: {
                const SDL_Scancode sc = e.key.scancode;
                if (sc == SDL_SCANCODE_LALT || sc == SDL_SCANCODE_RALT) altDown = true;
                if (sc == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)) {
                    toggleFullscreen();
                    break;
                }
                if (sc == SDL_SCANCODE_BACKSPACE) in.backspace = true;
                if (sc == SDL_SCANCODE_RETURN) in.enter = true;
                if (sc == SDL_SCANCODE_ESCAPE) in.escape = true;
                if (!e.key.repeat) key(sc);
                break;
            }
            case SDL_EVENT_KEY_UP:
                if (e.key.scancode == SDL_SCANCODE_LALT || e.key.scancode == SDL_SCANCODE_RALT) altDown = false;
                break;
            case SDL_EVENT_TEXT_INPUT: in.text += e.text.text; break;
            case SDL_EVENT_MOUSE_MOTION:
                in.mouseX = e.motion.x * mouseK;
                in.mouseY = e.motion.y * mouseK;
                if (dragging) {
                    camX = dragCamX - (in.mouseX - dragX) / tilePx();
                    camY = dragCamY - (in.mouseY - dragY) / tilePx();
                    follow = false;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                const float mx = e.button.x * mouseK, my = e.button.y * mouseK;
                shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
                if (e.button.button == SDL_BUTTON_LEFT) {
                    in.down = true;
                    in.pressed = true;
                    in.doubleClick = e.button.clicks >= 2;
                    clickedWorld = true;
                    clickX = mx, clickY = my;
                } else if (e.button.button == SDL_BUTTON_RIGHT) {
                    in.rightDown = true;
                    in.rightPressed = true;
                    if (shift) startDrag(mx, my);
                    else rclickedWorld = true, clickX = mx, clickY = my;
                } else if (e.button.button == SDL_BUTTON_MIDDLE) {
                    in.middleDown = true;
                    startDrag(mx, my);
                }
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button == SDL_BUTTON_LEFT) in.down = false, in.released = true;
                if (e.button.button == SDL_BUTTON_RIGHT) in.rightDown = false, dragging = false;
                if (e.button.button == SDL_BUTTON_MIDDLE) in.middleDown = false, dragging = false;
                break;
            case SDL_EVENT_MOUSE_WHEEL: in.wheel += e.wheel.y; break;
            default: break;
            }
        }
        const SDL_Keymod mod = SDL_GetModState();
        in.shift = (mod & SDL_KMOD_SHIFT) != 0;
        in.ctrl = (mod & SDL_KMOD_CTRL) != 0;
        return true;
    }
    void startDrag(float mx, float my) {
        dragging = true;
        dragX = mx, dragY = my;
        dragCamX = camX, dragCamY = camY;
    }
    void worldInput() {
        const bool overUi = in.overUi || in.mouseX >= mapW() || in.mouseY >= mapH();
        {
            double wx, wy;
            toWorld(in.mouseX, in.mouseY, wx, wy);
            if (screens->mapInput(wx, wy, in, overUi || dragging)) {   // the build mode has the pointer
                if (!overUi && in.wheel != 0) {
                    double bx, by, ax, ay;
                    toWorld(in.mouseX, in.mouseY, bx, by);
                    zoom = std::clamp(zoom + (in.wheel > 0 ? 1 : -1), 1, 3);
                    toWorld(in.mouseX, in.mouseY, ax, ay);
                    camX += bx - ax;
                    camY += by - ay;
                }
                clickedWorld = rclickedWorld = false;
                haveHover = false;
                return;
            }
        }
        if (!overUi && !dragging) {
            toWorld(in.mouseX, in.mouseY, hoverX, hoverY);
            haveHover = true;
            const Json* p = pickPerson(hoverX, hoverY);
            hoverId = p ? ident(*p, "id") : -1;
        } else {
            haveHover = false;
            hoverId = -1;
        }
        if (!overUi && in.wheel != 0) {
            double bx, by, ax, ay;
            toWorld(in.mouseX, in.mouseY, bx, by);
            zoom = std::clamp(zoom + (in.wheel > 0 ? 1 : -1), 1, 3);
            toWorld(in.mouseX, in.mouseY, ax, ay);
            camX += bx - ax;
            camY += by - ay;
        }
        if (clickedWorld && !overUi && !menuOpen) {
            double wx, wy;
            toWorld(clickX, clickY, wx, wy);
            const Json* p = pickPerson(wx, wy);
            bool done = false;
            if (me >= 0 && !shift && !p && vis.count(me)) {
                rebuildWalls();
                std::vector<std::pair<double, double>> found;
                if (findPath(vis[me].x, vis[me].y, wx, wy, found)) {
                    path = found;
                    markerX = std::floor(wx) + 0.5, markerY = std::floor(wy) + 0.5, markerAt = nowMs();
                } else toast(L("ui2d.no_way"));
                done = true;
            }
            if (!done) select(p ? ident(*p, "id") : -1);
        }
        if (rclickedWorld && !overUi) {
            double wx, wy;
            toWorld(clickX, clickY, wx, wy);
            if (const Json* p = pickPerson(wx, wy)) {
                const long long id = ident(*p, "id");
                openMenu(id == me ? -1 : id, id == me ? L("ui2d.self") : str(*p, "name"), clickX, clickY);
            } else if (const Json* t = thingAt(wx, wy)) {
                const std::string ty = (*t)[1].get<std::string>();
                const Json objs = ST.value("objects", Json::object());
                openMenu((*t)[0].get<long long>(), objs.contains(ty) ? objs[ty].get<std::string>() : ty, clickX, clickY);
            } else menuOpen = false;
        }
        clickedWorld = rclickedWorld = false;
    }
    void camera(double dt) {
        const bool* k = SDL_GetKeyboardState(nullptr);
        if (ui.typing()) return;
        const double pan = 18 * dt * (k[SDL_SCANCODE_LSHIFT] ? 3 : 1) / std::max(1.0, zoom * 0.6) * 3;
        const bool wasd = me < 0;
        if ((wasd && k[SDL_SCANCODE_A]) || k[SDL_SCANCODE_LEFT]) camX -= pan, follow = false;
        if ((wasd && k[SDL_SCANCODE_D]) || k[SDL_SCANCODE_RIGHT]) camX += pan, follow = false;
        if ((wasd && k[SDL_SCANCODE_W]) || k[SDL_SCANCODE_UP]) camY -= pan, follow = false;
        if ((wasd && k[SDL_SCANCODE_S]) || k[SDL_SCANCODE_DOWN]) camY += pan, follow = false;
        const long long fid = follow ? (me >= 0 ? me : selId) : -1;
        if (fid >= 0 && vis.count(fid)) {
            const Vis& v = vis[fid];
            camX += (v.x - camX) * std::min(1.0, dt * 6);
            camY += (v.y - camY) * std::min(1.0, dt * 6);
        }
    }
    void shotScript(double elapsed) {
        if (selectIndex >= 0 && selId < 0 && !people.empty()) {
            std::vector<const Json*> humans;
            for (const Json& p : people)
                if (str(p, "sp").empty() && flag(p, "alive")) humans.push_back(&p);
            if (!humans.empty()) {
                const Json* p = humans[std::min(humans.size() - 1, std::size_t(selectIndex))];
                selId = ident(*p, "id");
                camX = num(*p, "x"), camY = num(*p, "y");
                follow = true;
                selectIndex = -1;
            }
        }
        (void)elapsed;
    }

    // A broken answer costs that part of one frame, not the game: said once a second at most.
    double lastComplaint = -1e9;
    template <class F>
    void guard(const char* what, F&& f) {
        try {
            f();
        } catch (const std::exception& e) {
            if (nowMs() - lastComplaint > 1000) {
                lastComplaint = nowMs();
                std::cerr << "window: " << what << ": " << e.what() << std::endl;
            }
        }
    }

    void run(Host& h) {
        host = &h;
        loadingFrame("");
        loadStatic();
        dataGen = lr::sim::data::generation();
        if (const char* s = SDL_getenv("LR_2D_SHOT")) shotPath = s;
        if (const char* s = SDL_getenv("LR_2D_SHOT_AFTER")) shotAfter = std::atof(s);
        if (const char* s = SDL_getenv("LR_2D_SELECT")) selectIndex = std::atoi(s);
        if (const char* s = SDL_getenv("LR_2D_ZOOM")) zoom = std::clamp(std::atoi(s), 1, 3);
        SDL_StartTextInput(win);
        screens = std::make_unique<Screens>(*this);
        poll();
        if (!SDL_getenv("LR_2D_SHOT")) screens->start();
        if (const char* o = SDL_getenv("LR_2D_OPEN")) {   // a shot of a screen: LR_2D_OPEN=menu|new|pick|saves|chronicle|...
            const char* arg = SDL_getenv("LR_2D_OPEN_ARG");
            Json a = arg ? Json(std::string(arg)) : Json();
            if (arg && std::string(arg) == "near" && person(me)) {   // the nearest person to one's own
                double best = 1e18;
                const double mx = num(*person(me), "x"), my = num(*person(me), "y");
                for (const Json& p : people)
                    if (str(p, "sp").empty() && flag(p, "alive") && ident(p, "id") != me) {
                        const double d = std::hypot(num(p, "x") - mx, num(p, "y") - my);
                        if (d < best) best = d, a = ident(p, "id");
                    }
            }
            screens->open(o, a);
            if (const char* t = SDL_getenv("LR_2D_TAB"); t && screens->top()) screens->top()->tab = t;
        }
        const double start = nowMs();
        lastFrame = start;
        while (!host->quitting()) {
            if (!events()) {
                post("/api/quit", {{"autosave", true}});
                break;
            }
            measure();
            const double t = nowMs(), dt = std::min(0.1, (t - lastFrame) / 1000);
            lastFrame = t;
            host->pump(8);
            if (t - lastPoll > 250) guard("poll", [&] { poll(); });
            if (t - lastBar > 1200) guard("bar", [&] { checkBar(); });
            if (lr::sim::data::generation() != dataGen) {   // LR-142: hot reload
                dataGen = lr::sim::data::generation();
                guard("reload", [&] { reloadStatic(); });
            }
            camera(dt);
            stepVis(dt);
            guard("walk", [&] { walkMe(dt); });
            shotScript((t - start) / 1000);
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            ui.begin(in, W, H);
            guard("map", [&] { drawMap(); });
            guard("overlay", [&] { drawOverlay(); });
            guard("side", [&] { drawSide(); });
            guard("bar", [&] { drawBar(); });
            guard("screens", [&] { screens->draw(); });
            ui.end();
            guard("input", [&] { worldInput(); });
            if (!shotPath.empty() && (t - start) / 1000 > shotAfter) {
                if (SDL_Surface* s = SDL_RenderReadPixels(ren, nullptr)) {
                    if (!IMG_SavePNG(s, shotPath.c_str())) std::cerr << "shot: " << SDL_GetError() << "\n";
                    else std::cout << "shot " << shotPath << std::endl;
                    SDL_DestroySurface(s);
                }
                shotPath.clear();
                if (const char* q = SDL_getenv("LR_2D_SHOT_QUIT"); q && *q == '1') {
                    post("/api/quit", {{"autosave", false}});
                }
            }
            SDL_RenderPresent(ren);
        }
        SDL_StopTextInput(win);
    }

    ~Impl() {
        for (auto& [k, c] : chunks)
            if (c.tex) SDL_DestroyTexture(c.tex);
        atlas.clear();
        atmos.release();
        ui.shutdown();
        if (ren) SDL_DestroyRenderer(ren);
        if (win) SDL_DestroyWindow(win);
        SDL_Quit();
    }
};

Window::Window(std::string root) : impl_(std::make_unique<Impl>()) { impl_->root = std::move(root); }
Window::~Window() = default;
bool Window::open() { return impl_->open(); }
const std::string& Window::error() const { return impl_->err; }
void Window::run(Host& host) { impl_->run(host); }
void Window::fail(const std::string& why) { impl_->fail(why); }

} // namespace game2d
