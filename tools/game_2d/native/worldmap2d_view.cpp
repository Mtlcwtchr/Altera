// LR-153: the world map window (M) of the 2D window: the land around the realm's
// settlements in pixel cells, the realms' borders, roads, rivers, wars, the places
// of interest and where the led person is. Drag to pan, the wheel zooms, a click
// picks a settlement or a place; the right column tells about it and lists the
// realms and the places. One size with the sheet and the dynasty (Win::big).
// Data: /api/worldmap (src/client2d/worldmap2d); look: content/sprites_2d/worldmap2d.json.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

#include <SDL3/SDL.h>

#include "native/ctx2d.hpp"

namespace game2d {

namespace {

double now() { return double(SDL_GetTicks()); }
std::string W(Ctx& c, const std::string& k, const std::map<std::string, std::string>& a = {}) { return c.L("ui2d.wmap." + k, a); }
std::string num0(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.0f", v);
    return b;
}
std::uint32_t hash2(int i, int j) {
    std::uint32_t h = std::uint32_t(i) * 73856093u ^ std::uint32_t(j) * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    return h ^ (h >> 15);
}

// The window's state beyond the data: the view and the pick.
struct View {
    double cx = 0, cy = 0;   // km at the viewport's centre
    int zoom = 0;            // index into worldmap2d.json zooms
    bool borders = true, labels = true;
    bool dragging = false, moved = false;
    float dragX = 0, dragY = 0;
    double dragCx = 0, dragCy = 0;
    long long pick = -1;     // settlement id, or -(poi id) - 10 for a place
    std::string list = "domains";
    bool centred = false;
    // derived from the data
    std::vector<long long> owner;   // per cell: the domain whose land it is (-1: nobody, water)
    int grid = 0;
};

View& viewOf(Win& w) {
    static std::map<Win*, View> views;
    return views[&w];
}

const Json* settlement(const Json& d, long long id) {
    for (const auto& s : jarr(d, "settlements"))
        if (jid(s, "id") == id) return &s;
    return nullptr;
}
const Json* poi(const Json& d, long long id) {
    for (const auto& p : jarr(d, "pois"))
        if (jid(p, "id") == id) return &p;
    return nullptr;
}
// LR-163: a caravan's pick key (settlements >= 0, places -(id)-10, caravans below kCaravanKey)
constexpr long long kCaravanKey = -1000000;
const Json* caravan(const Json& d, long long key) {
    for (const auto& v : jarr(d, "caravans"))
        if (kCaravanKey - jid(v, "id") == key) return &v;
    return nullptr;
}
std::string caravanIcon(Ctx& c, const Json& v) {
    return c.worldLook().value("caravan_icons", Json::object()).value(jstr(v, "kind"), std::string("wm_caravan"));
}

const Json* domain(const Json& d, long long id) {
    for (const auto& x : jarr(d, "domains"))
        if (jid(x, "id") == id) return &x;
    return nullptr;
}
double kmx(const Json& o) { return o.contains("km") && o["km"].is_array() && o["km"].size() >= 2 ? o["km"][0].get<double>() : 0; }
double kmy(const Json& o) { return o.contains("km") && o["km"].is_array() && o["km"].size() >= 2 ? o["km"][1].get<double>() : 0; }

bool hasStr(const Json& arr, const std::string& v) {
    if (!arr.is_array()) return false;
    for (const auto& e : arr)
        if (e.is_string() && e.get<std::string>() == v) return true;
    return false;
}

std::string settlementIcon(Ctx& c, const Json& s) {
    for (const auto& r : c.worldLook().value("settlement_icons", Json::array())) {
        if (!jstr(r, "inst").empty() && jstr(r, "inst") != jstr(s, "inst_id")) continue;
        if (!jstr(r, "tag").empty() && !hasStr(s.value("tag_ids", Json::array()), jstr(r, "tag"))) continue;
        if (!jstr(r, "culture").empty() && jstr(r, "culture") != jstr(s, "culture_id")) continue;
        if (!jstr(r, "size").empty() && jstr(r, "size") != jstr(s, "size")) continue;
        return jstr(r, "icon");
    }
    return "wm_village";
}

ui::Colour domainColour(Ctx& c, const Json& d, long long dom, float a = 1.0f) {
    const Json& cols = c.worldLook().value("domain_colours", Json::array());
    int idx = 0;
    for (const auto& x : jarr(d, "domains")) {
        if (jid(x, "id") == dom) break;
        ++idx;
    }
    if (!cols.is_array() || cols.empty()) return pal::gold.withAlpha(a);
    return pal::hex(cols[size_t(idx) % cols.size()].get<std::string>().c_str(), a);
}

// whose land each cell is: the nearest settlement that reaches it (by its size), its domain
void deriveOwners(Ctx& c, const Json& d, View& v) {
    const int G = int(jnum(d, "grid"));
    v.grid = G;
    v.owner.assign(size_t(G * G), -1);
    if (G <= 0) return;
    const std::string ground = jstr(d, "ground");
    const double ox = d["origin"][0].get<double>(), oy = d["origin"][1].get<double>(), ck = jnum(d, "cell_km", 1);
    std::vector<int> water;
    int k = 0;
    for (const auto& kd : jarr(d, "kinds")) {
        if (jstr(kd, "id") == "sea" || jstr(kd, "id") == "lake") water.push_back(k);
        ++k;
    }
    const Json reach = c.worldLook().value("reach_km", Json::object());
    struct S {
        double x, y, r;
        long long dom;
    };
    std::vector<S> ss;
    for (const auto& s : jarr(d, "settlements")) {
        if (jflag(s, "ruined")) continue;
        ss.push_back({kmx(s), kmy(s), reach.value(jstr(s, "size"), 6.0), jid(s, "domain")});
    }
    for (int j = 0; j < G; ++j)
        for (int i = 0; i < G; ++i) {
            const size_t ci = size_t(j * G + i);
            if (ci < ground.size() && std::find(water.begin(), water.end(), ground[ci] - '0') != water.end()) continue;
            const double x = ox + (i + 0.5) * ck, y = oy + (j + 0.5) * ck;
            double best = 1e9;
            for (const S& s : ss) {
                const double dd = std::hypot(x - s.x, y - s.y) / s.r;   // reach-weighted: a town holds more land
                if (dd < 1.0 && dd < best) best = dd, v.owner[ci] = s.dom;
            }
        }
}

// a dotted or solid line of art pixels
void dotted(Ctx& c, float x0, float y0, float x1, float y1, const ui::Colour& col, float px, float gap, const ui::Rect& clip) {
    const float len = std::hypot(x1 - x0, y1 - y0);
    if (len < 1) return;
    const int n = int(len / gap);
    for (int i = 0; i <= n; ++i) {
        const float t = float(i) / std::max(1, n);
        const float x = x0 + (x1 - x0) * t, y = y0 + (y1 - y0) * t;
        if (!clip.contains(x, y)) continue;
        c.fill(std::round(x - px / 2), std::round(y - px / 2), px, px, col);
    }
}

void drawMap(Ctx& c, Win& w, View& v, const Json& d, const ui::Rect& R) {
    const float U = c.u(), p = c.pixel();
    const Json& look = c.worldLook();
    const Json zooms = look.value("zooms", Json::array({1.0, 1.6, 2.5, 4.0}));
    const int G = int(jnum(d, "grid"));
    const double ox = d["origin"][0].get<double>(), oy = d["origin"][1].get<double>(), ck = jnum(d, "cell_km", 1);
    const double z = zooms.is_array() && !zooms.empty() ? zooms[size_t(std::clamp(v.zoom, 0, int(zooms.size()) - 1))].get<double>() : 1.0;
    const double base = std::min(R.w, R.h) / (G * ck);    // px per km at zoom 1: the whole land fits
    const double ppk = base * z;
    auto sx = [&](double km) { return float(R.x + R.w / 2 + (km - v.cx) * ppk); };
    auto sy = [&](double km) { return float(R.y + R.h / 2 + (km - v.cy) * ppk); };
    auto kx = [&](float px) { return v.cx + (px - (R.x + R.w / 2)) / ppk; };
    auto ky = [&](float py) { return v.cy + (py - (R.y + R.h / 2)) / ppk; };
    ui::Input& in = c.input();
    const bool over = R.contains(in.mouseX, in.mouseY);

    // --- input: drag to pan, the wheel zooms about the pointer -----------------------------
    if (over && in.pressed) {
        v.dragging = true, v.moved = false;
        v.dragX = in.mouseX, v.dragY = in.mouseY, v.dragCx = v.cx, v.dragCy = v.cy;
    }
    if (v.dragging && in.down) {
        if (std::hypot(in.mouseX - v.dragX, in.mouseY - v.dragY) > 4 * U) v.moved = true;
        if (v.moved) v.cx = v.dragCx - (in.mouseX - v.dragX) / ppk, v.cy = v.dragCy - (in.mouseY - v.dragY) / ppk;
    }
    const bool click = v.dragging && in.released && !v.moved && over;
    if (in.released) v.dragging = false;
    if (over && in.wheel != 0 && zooms.is_array()) {
        const double ax = kx(in.mouseX), ay = ky(in.mouseY);
        v.zoom = std::clamp(v.zoom + (in.wheel > 0 ? 1 : -1), 0, int(zooms.size()) - 1);
        const double nppk = base * zooms[size_t(v.zoom)].get<double>();
        v.cx = ax - (in.mouseX - (R.x + R.w / 2)) / nppk;
        v.cy = ay - (in.mouseY - (R.y + R.h / 2)) / nppk;
        in.wheel = 0;
        return;   // drawn next frame at the new scale
    }
    const double half = G * ck / 2;
    v.cx = std::clamp(v.cx, ox, ox + 2 * half);
    v.cy = std::clamp(v.cy, oy, oy + 2 * half);

    // --- the land: runs of one kind per row, a glyph here and there --------------------------
    c.fill(R.x, R.y, R.w, R.h, pal::hex(look.value("fog_edge", std::string("#141010")).c_str()));
    const std::string ground = jstr(d, "ground"), river = jstr(d, "river");
    const Json& kinds = jarr(d, "kinds");
    const Json terr = look.value("terrain", Json::object());
    std::vector<ui::Colour> base_, dark_;
    std::vector<Json> glyph_;
    for (const auto& kd : kinds) {
        const Json t = terr.value(jstr(kd, "id"), Json::object());
        base_.push_back(pal::hex(t.value("c", std::string("#808080")).c_str()));
        dark_.push_back(pal::hex(t.value("d", std::string("#606060")).c_str()));
        glyph_.push_back(t.value("glyph", Json::array()));
    }
    const float cpx = float(ck * ppk);
    const int i0 = std::max(0, int(std::floor((kx(R.x) - ox) / ck))), i1 = std::min(G - 1, int(std::floor((kx(R.right()) - ox) / ck)));
    const int j0 = std::max(0, int(std::floor((ky(R.y) - oy) / ck))), j1 = std::min(G - 1, int(std::floor((ky(R.bottom()) - oy) / ck)));
    auto cellX = [&](int i) { return std::round(sx(ox + i * ck)); };
    auto cellY = [&](int j) { return std::round(sy(oy + j * ck)); };
    auto clipFill = [&](float x0, float y0, float x1, float y1, const ui::Colour& col) {
        x0 = std::max(x0, R.x), y0 = std::max(y0, R.y), x1 = std::min(x1, R.right()), y1 = std::min(y1, R.bottom());
        if (x1 > x0 && y1 > y0) c.fill(x0, y0, x1 - x0, y1 - y0, col);
    };
    const int nk = int(base_.size());
    for (int j = j0; j <= j1; ++j) {
        const float y0 = cellY(j), y1 = cellY(j + 1);
        int i = i0;
        while (i <= i1) {
            const int k = ground[size_t(j * G + i)] - '0';
            int e = i;
            while (e + 1 <= i1 && ground[size_t(j * G + e + 1)] - '0' == k) ++e;
            if (k >= 0 && k < nk) clipFill(cellX(i), y0, cellX(e + 1), y1, base_[size_t(k)]);
            i = e + 1;
        }
    }
    // the glyphs: 4x4 art pixels over every other cell, when a cell is big enough to hold them
    if (cpx >= 5 * p)
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                if (hash2(i, j) % 3) continue;
                const int k = ground[size_t(j * G + i)] - '0';
                if (k < 0 || k >= nk || !glyph_[size_t(k)].is_array()) continue;
                const float gp = std::max(p, std::floor(cpx / 5 / p) * p);
                const float gx = cellX(i) + std::round((cpx - 4 * gp) / 2), gy = cellY(j) + std::round((cpx - 4 * gp) / 2);
                int r = 0;
                for (const auto& row : glyph_[size_t(k)]) {
                    const std::string rs = row.is_string() ? row.get<std::string>() : std::string();
                    for (int q = 0; q < int(rs.size()) && q < 4; ++q) {
                        if (rs[size_t(q)] == '.') continue;
                        const ui::Colour col = rs[size_t(q)] == 'd' ? dark_[size_t(k)] : base_[size_t(k)].lerp(ui::Colour(1, 1, 1, 1), 0.14f);
                        clipFill(gx + q * gp, gy + r * gp, gx + (q + 1) * gp, gy + (r + 1) * gp, col);
                    }
                    ++r;
                }
            }
    // rivers
    const ui::Colour rc = pal::hex(look.value("river", std::string("#3c6c9c")).c_str());
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i)
            if (size_t(j * G + i) < river.size() && river[size_t(j * G + i)] == '1') {
                const float inset = std::max(0.0f, std::round(cpx * 0.25f));
                clipFill(cellX(i) + inset, cellY(j) + inset, cellX(i + 1) - inset, cellY(j + 1) - inset, rc);
                // join to a river neighbour
                if (i + 1 < G && river[size_t(j * G + i + 1)] == '1') clipFill(cellX(i + 1) - inset, cellY(j) + inset, cellX(i + 1) + inset, cellY(j + 1) - inset, rc);
                if (j + 1 < G && river[size_t((j + 1) * G + i)] == '1') clipFill(cellX(i) + inset, cellY(j + 1) - inset, cellX(i + 1) - inset, cellY(j + 1) + inset, rc);
            }

    // --- the realms: a tint of the domain's colour and its border -------------------------------
    if (v.borders && int(v.owner.size()) == G * G) {
        for (int j = j0; j <= j1; ++j) {
            int i = i0;
            while (i <= i1) {
                const long long o = v.owner[size_t(j * G + i)];
                int e = i;
                while (e + 1 <= i1 && v.owner[size_t(j * G + e + 1)] == o) ++e;
                if (o >= 0) clipFill(cellX(i), cellY(j), cellX(e + 1), cellY(j + 1), domainColour(c, d, o, 0.16f));
                i = e + 1;
            }
        }
        const float bt = std::max(p, std::round(cpx * 0.18f));
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                const long long o = v.owner[size_t(j * G + i)];
                if (o < 0) continue;
                const ui::Colour col = domainColour(c, d, o, 0.85f);
                auto other = [&](int a, int b) { return a < 0 || b < 0 || a >= G || b >= G || v.owner[size_t(b * G + a)] != o; };
                if (other(i - 1, j)) clipFill(cellX(i), cellY(j), cellX(i) + bt, cellY(j + 1), col);
                if (other(i + 1, j)) clipFill(cellX(i + 1) - bt, cellY(j), cellX(i + 1), cellY(j + 1), col);
                if (other(i, j - 1)) clipFill(cellX(i), cellY(j), cellX(i + 1), cellY(j) + bt, col);
                if (other(i, j + 1)) clipFill(cellX(i), cellY(j + 1) - bt, cellX(i + 1), cellY(j + 1), col);
            }
    }

    // --- roads and wars ---------------------------------------------------------------------
    const ui::Colour road = pal::hex(look.value("road", std::string("#c8b078")).c_str());
    for (const auto& r : jarr(d, "roads")) {
        if (!r.is_array() || r.size() < 2) continue;
        const Json* a = settlement(d, r[0].get<long long>());
        const Json* b = settlement(d, r[1].get<long long>());
        if (a && b) dotted(c, sx(kmx(*a)), sy(kmy(*a)), sx(kmx(*b)), sy(kmy(*b)), road, p, 3 * p, R);
    }
    const ui::Colour war = pal::hex(look.value("war", std::string("#c03828")).c_str());
    for (const auto& r : jarr(d, "wars")) {
        const Json* a = settlement(d, jid(r, "a"));
        const Json* b = settlement(d, jid(r, "b"));
        if (a && b) dotted(c, sx(kmx(*a)), sy(kmy(*a)), sx(kmx(*b)), sy(kmy(*b)), war, 2 * p, 5 * p, R);
    }

    // --- the places, the settlements, the led person ------------------------------------------
    // icons in whole art pixels of the 16-pixel sprites, growing a little with the zoom
    const float iconPx = std::max(16.0f, std::round(std::clamp(float(22 * U * std::sqrt(z)), 20 * U, 44 * U) / 16) * 16);
    long long hover = 0;
    float hoverD = 1e9;
    auto near = [&](float x, float y, long long id) {
        if (!over) return;
        const float dd = std::hypot(in.mouseX - x, in.mouseY - y);
        if (dd < iconPx * 0.6f && dd < hoverD) hoverD = dd, hover = id;
    };
    const auto& poiIcons = look.value("poi_icons", Json::object());
    for (const auto& q : jarr(d, "pois")) {
        const float x = sx(kmx(q)), y = sy(kmy(q));
        if (!R.inset(iconPx / 2).contains(x, y)) continue;
        const float s = iconPx * 0.85f;
        const long long key = -jid(q, "id") - 10;
        if (v.pick == key) c.frame(x - s / 2 - p, y - s / 2 - p, s + 2 * p, s + 2 * p, pal::gold, p);
        c.spriteAt(poiIcons.value(jstr(q, "kind"), std::string("wm_ruins")), Json(), x - s / 2, y - s / 2, s);
        near(x, y, key);
        if (v.labels && (v.zoom >= 2 || v.pick == key)) c.textC(x, y + s / 2, jstr(q, "name"), pal::parch.withAlpha(0.85f), 12 * U, true);
    }
    const Json& me = d.value("me", Json::object());
    for (const auto& s : jarr(d, "settlements")) {
        const float x = sx(kmx(s)), y = sy(kmy(s));
        if (!R.inset(iconPx / 2).contains(x, y)) continue;
        const long long id = jid(s, "id");
        const float sz = jstr(s, "size") == "town" || jstr(s, "size") == "city" ? iconPx * 1.25f : iconPx;
        if (v.pick == id) c.frame(x - sz / 2 - p, y - sz / 2 - p, sz + 2 * p, sz + 2 * p, pal::gold, p);
        c.spriteAt(settlementIcon(c, s), Json(), x - sz / 2, y - sz / 2, sz, jflag(s, "ruined") ? 0.45f : 1.0f);
        if (jflag(s, "ruined")) c.spriteAt("wm_ruins", Json(), x - sz / 3, y - sz / 3, sz * 0.66f);
        if (jflag(s, "war")) c.spriteAt("wm_battle", Json(), x + sz / 4, y - sz / 2 - sz * 0.2f, sz * 0.5f);
        near(x, y, id);
        if (v.labels) {
            const std::string nm = jstr(s, "name");
            const ui::Colour col = jid(me, "at") == id ? pal::gold : pal::parch;
            c.textC(x, y + sz / 2, nm, col, 16 * U, true);
        }
    }
    // LR-163: who is on the road: a route to where they go, the wagon or the pack
    const ui::Colour route = pal::hex(look.value("caravan_route", std::string("#e0c890")).c_str(), 0.8f);
    for (const auto& cv : jarr(d, "caravans")) {
        const float x = sx(kmx(cv)), y = sy(kmy(cv));
        const long long key = kCaravanKey - jid(cv, "id");
        if (jstr(cv, "state") == "road")
            if (const Json* to = settlement(d, jid(cv, "to"))) dotted(c, x, y, sx(kmx(*to)), sy(kmy(*to)), route, p, 2 * p, R);
        if (!R.inset(iconPx / 2).contains(x, y)) continue;
        const float s = iconPx * 0.8f;
        const float ox2 = jstr(cv, "state") == "market" ? s * 0.6f : 0;   // at a market: beside the settlement
        if (v.pick == key) c.frame(x + ox2 - s / 2 - p, y - s / 2 - p, s + 2 * p, s + 2 * p, pal::gold, p);
        c.spriteAt(caravanIcon(c, cv), Json(), x + ox2 - s / 2, y - s / 2, s);
        near(x + ox2, y, key);
    }
    if (const Json* at = settlement(d, jid(me, "at"))) {
        const float x = sx(kmx(*at)), y = sy(kmy(*at));
        const float bob = std::round(std::sin(now() / 260.0) * 2) * p;
        if (R.inset(iconPx).contains(x, y)) c.spriteAt("wm_you", Json(), x - iconPx / 2, y - iconPx * 1.55f + bob, iconPx);
    }
    if (hover && !v.dragging) c.frame(in.mouseX - p, in.mouseY - p, 2 * p, 2 * p, pal::gold, p);
    if (click) v.pick = hover ? hover : v.pick;
    w.data["hover"] = hover;

    // --- the scale: 10 km ---------------------------------------------------------------------
    {
        const double kmBar = ppk * 10 < 60 * U ? 20 : 10;
        const float bw = float(kmBar * ppk), bx = R.x + 12 * U, by = R.bottom() - 18 * U;
        c.fill(bx, by, bw, 2 * p, pal::parch);
        c.fill(bx, by - 2 * p, p, 4 * p, pal::parch);
        c.fill(bx + bw - p, by - 2 * p, p, 4 * p, pal::parch);
        c.text(bx + bw + 6 * U, by - 8 * U, W(c, "scale", {{"n", num0(kmBar)}}), pal::parch, 12 * U, true);
    }
}

// the right column's card of the picked (or pointed at) thing
float card(Ctx& c, const Json& d, long long id, const ui::Rect& r) {
    const float U = c.u();
    float y = r.y;
    auto line = [&](const std::string& s, const ui::Colour& col) {
        for (const auto& row : c.wrap(s, r.w - 8 * U, 16 * U)) {
            c.text(r.x + 4 * U, y, row, col, 16 * U);
            y += 20 * U;
        }
    };
    if (id >= 0) {
        const Json* s = settlement(d, id);
        if (!s) return 0;
        const float ic = 48 * U;
        c.well({r.x, y, ic, ic});
        c.spriteAt(settlementIcon(c, *s), Json(), r.x + 4 * U, y + 4 * U, ic - 8 * U);
        c.text(r.x + ic + 8 * U, y + 2 * U, c.fit(jstr(*s, "name"), r.w - ic - 10 * U, 16 * U), pal::gold, 16 * U);
        std::string sub = jstr(*s, "size_name");
        if (!jflag(*s, "ruined")) sub += (sub.empty() ? "" : " · ") + W(c, "pop", {{"n", num0(jnum(*s, "pop"))}});
        else sub += " · " + W(c, "ruined");
        c.text(r.x + ic + 8 * U, y + 24 * U, c.fit(sub, r.w - ic - 10 * U, 16 * U), pal::dim, 16 * U);
        y += ic + 6 * U;
        if (const Json* dm = domain(d, jid(*s, "domain"))) {
            c.fill(r.x + 4 * U, y + 5 * U, 10 * U, 10 * U, domainColour(c, d, jid(*dm, "id")));
            c.text(r.x + 20 * U, y, c.fit(jstr(*dm, "name"), r.w - 24 * U, 16 * U), pal::text, 16 * U);
            y += 20 * U;
        }
        if (!jstr(*s, "ruler").empty()) line(W(c, "ruler", {{"name", jstr(*s, "ruler")}}) + " (" + jstr(*s, "inst") + ")", pal::text);
        if (!jstr(*s, "house").empty()) line(W(c, "house", {{"name", jstr(*s, "house")}}), pal::dim);
        if (!jstr(*s, "culture").empty()) line(W(c, "culture", {{"name", jstr(*s, "culture")}}), pal::dim);
        if (const Json* l = settlement(d, jid(*s, "liege")); l && jid(*s, "liege") > 0) line(W(c, "liege", {{"name", jstr(*l, "name")}}), pal::text);
        if (const Json* l = settlement(d, jid(*s, "tribute_to")); l && jid(*s, "tribute_to") > 0) line(W(c, "tribute", {{"name", jstr(*l, "name")}}), pal::mid);
        for (const auto& wr : jarr(d, "wars")) {
            const long long other = jid(wr, "a") == id ? jid(wr, "b") : jid(wr, "b") == id ? jid(wr, "a") : -1;
            if (other < 0) continue;
            if (const Json* o = settlement(d, other)) line(W(c, "war_with", {{"name", jstr(*o, "name")}}) + " · " + jstr(wr, "cause"), pal::bad);
        }
        for (const auto& q : jarr(d, "quests"))
            if (jid(q, "at") == id) line(W(c, "quest", {{"name", jstr(q, "name")}}), pal::good);
        std::string tags;
        for (const auto& t : jarr(*s, "tags")) tags += (tags.empty() ? "" : ", ") + t.get<std::string>();
        if (!tags.empty()) line(tags, pal::dim);
        if (jflag(*s, "here")) line(W(c, "you"), pal::gold);
    } else if (const Json* cv = caravan(d, id)) {
        const float ic = 48 * U;
        c.well({r.x, y, ic, ic});
        c.spriteAt(caravanIcon(c, *cv), Json(), r.x + 4 * U, y + 4 * U, ic - 8 * U);
        c.text(r.x + ic + 8 * U, y + 2 * U, c.fit(jstr(*cv, "kind_name"), r.w - ic - 10 * U, 16 * U), pal::gold, 16 * U);
        c.text(r.x + ic + 8 * U, y + 24 * U, c.fit(W(c, jstr(*cv, "state") == "market" ? "at_market" : "on_road"), r.w - ic - 10 * U, 16 * U), pal::dim, 16 * U);
        y += ic + 6 * U;
        if (!jstr(*cv, "leader").empty()) line(W(c, "leader", {{"name", jstr(*cv, "leader")}}), pal::text);
        const Json* from = settlement(d, jid(*cv, "from"));
        const Json* to = settlement(d, jid(*cv, "to"));
        if (from && to) line(W(c, "route", {{"from", jstr(*from, "name")}, {"to", jstr(*to, "name")}}), pal::text);
        line(W(c, "crew", {{"n", num0(jnum(*cv, "n"))}, {"g", num0(jnum(*cv, "guards"))}}), pal::dim);
        std::string cargo;
        for (const auto& g : jarr(*cv, "cargo")) cargo += (cargo.empty() ? "" : ", ") + g.get<std::string>();
        if (!cargo.empty()) line(W(c, "cargo", {{"list", cargo}}), pal::dim);
    } else {
        const Json* q = poi(d, -(id + 10));
        if (!q) return 0;
        const float ic = 48 * U;
        c.well({r.x, y, ic, ic});
        c.spriteAt(c.worldLook().value("poi_icons", Json::object()).value(jstr(*q, "kind"), std::string("wm_ruins")), Json(), r.x + 4 * U, y + 4 * U, ic - 8 * U);
        c.text(r.x + ic + 8 * U, y + 2 * U, c.fit(jstr(*q, "name"), r.w - ic - 10 * U, 16 * U), pal::gold, 16 * U);
        c.text(r.x + ic + 8 * U, y + 24 * U, c.fit(jstr(*q, "kind_name"), r.w - ic - 10 * U, 16 * U), pal::dim, 16 * U);
        y += ic + 6 * U;
        if (!jstr(*q, "near").empty()) line(jstr(*q, "near"), pal::text);
        if (jnum(*q, "day") > 0) line(W(c, "battle_day", {{"n", num0(jnum(*q, "day") + 1)}}), pal::dim);
    }
    return y - r.y;
}

ui::Rect section(Ctx& c, const ui::Rect& r, const std::string& title) {
    const float U = c.u();
    c.well(r);
    c.text(r.x + 8 * U, r.y + 6 * U, title, pal::gold, 16 * U);
    return {r.x + 6 * U, r.y + 30 * U, r.w - 12 * U, r.h - 36 * U};
}

void centreOn(View& v, const Json& o) { v.cx = kmx(o), v.cy = kmy(o); }

void render(Ctx& c, Win& w, const ui::Rect& body) {
    const float U = c.u();
    ui::Ui& g = c.gui();
    const Json& d = w.data["d"];
    if (!d.is_object() || jnum(d, "grid") <= 0) {
        c.parchLines(body.x, body.y, body.w, {W(c, "none")}, 16 * U);
        return;
    }
    View& v = viewOf(w);
    if (v.grid != int(jnum(d, "grid")) || jflag(w.data, "fresh")) {
        deriveOwners(c, d, v);
        w.data["fresh"] = false;
    }
    if (!v.centred) {
        if (const Json* at = settlement(d, jid(d.value("me", Json::object()), "at"))) centreOn(v, *at), v.pick = jid(*at, "id");
        else v.cx = d["origin"][0].get<double>() + jnum(d, "grid") * jnum(d, "cell_km") / 2, v.cy = d["origin"][1].get<double>() + jnum(d, "grid") * jnum(d, "cell_km") / 2;
        v.zoom = 1;
        if (w.tab == "places" || w.tab == "caravans") v.list = w.tab;   // LR_2D_TAB=places|caravans: a shot of a list
        if (w.tab == "near") v.zoom = 3;            // LR_2D_TAB=near: the land close up
        v.centred = true;
    }
    const float colW = 330 * U, gap = 10 * U;
    const ui::Rect mapR{body.x, body.y, body.w - colW - gap, body.h};
    c.well(mapR);
    const ui::Rect inner = mapR.inset(3 * c.pixel());
    drawMap(c, w, v, d, inner);

    // the map's buttons: zoom, centre, borders, labels
    {
        float x = inner.x + 8 * U;
        const float y = inner.y + 8 * U, h = 28 * U;
        auto btn = [&](const char* id, const std::string& label, bool on, bool toggle) {
            const float bw = c.textW(label, 16 * U) + 16 * U;
            const bool hit = toggle ? g.toggle(ui::widgetId(id), {x, y, bw, h}, label, on) : g.button(ui::widgetId(id), {x, y, bw, h}, label);
            x += bw + 4 * U;
            return hit;
        };
        const int zn = int(c.worldLook().value("zooms", Json::array({1, 2, 3, 4})).size());
        if (btn("wm.zin", "+", false, false)) v.zoom = std::min(zn - 1, v.zoom + 1);
        if (btn("wm.zout", "-", false, false)) v.zoom = std::max(0, v.zoom - 1);
        if (btn("wm.me", W(c, "center"), false, false))
            if (const Json* at = settlement(d, jid(d.value("me", Json::object()), "at"))) centreOn(v, *at);
        if (btn("wm.borders", W(c, "borders"), v.borders, true)) v.borders = !v.borders;
        if (btn("wm.labels", W(c, "labels"), v.labels, true)) v.labels = !v.labels;
    }

    // --- the right column: the card, then the realms or the places ----------------------------------
    const float x = body.right() - colW;
    const long long hover = w.data.value("hover", 0LL);
    const long long show = hover ? hover : v.pick;
    const float cardH = 250 * U;
    {
        const ui::Rect in = section(c, {x, body.y, colW, cardH}, W(c, "legend"));
        if (!show || card(c, d, show, in) <= 0) c.parchLines(in.x, in.y, in.w, {W(c, "pick")}, 16 * U);
    }
    const float ty = body.y + cardH + gap;
    {
        const float tw = (colW - 8 * U) / 3;
        if (g.tab(ui::widgetId("wm.tab", 0), {x, ty, tw, 28 * U}, W(c, "domains"), v.list == "domains")) v.list = "domains";
        if (g.tab(ui::widgetId("wm.tab", 1), {x + tw + 4 * U, ty, tw, 28 * U}, W(c, "places"), v.list == "places")) v.list = "places";
        if (g.tab(ui::widgetId("wm.tab", 2), {x + 2 * (tw + 4 * U), ty, tw, 28 * U}, W(c, "caravans"), v.list == "caravans")) v.list = "caravans";
    }
    const ui::Rect lr{x, ty + 32 * U, colW, body.bottom() - ty - 32 * U};
    c.well(lr);
    const ui::Rect sc = g.beginScroll(ui::widgetId("wm.list"), lr.inset(6 * U));
    float yy = sc.y;
    int idx = 0;
    if (v.list == "domains") {
        // the realms with their settlements, the biggest first
        std::vector<const Json*> doms;
        for (const auto& dm : jarr(d, "domains")) doms.push_back(&dm);
        std::stable_sort(doms.begin(), doms.end(), [](const Json* a, const Json* b) { return jnum(*a, "n") > jnum(*b, "n"); });
        for (const Json* dm : doms) {
            c.fill(sc.x + 2 * U, yy + 5 * U, 10 * U, 10 * U, domainColour(c, d, jid(*dm, "id")));
            c.text(sc.x + 18 * U, yy, c.fit(jstr(*dm, "name"), sc.w - 20 * U, 16 * U), pal::gold, 16 * U);
            yy += 20 * U;
            c.text(sc.x + 18 * U, yy, c.fit(jstr(*dm, "title_name") + " · " + W(c, "members", {{"n", num0(jnum(*dm, "n"))}}), sc.w - 20 * U, 16 * U), pal::dim, 16 * U);
            yy += 20 * U;
            for (const auto& m : jarr(*dm, "members")) {
                const Json* s = settlement(d, m.get<long long>());
                if (!s) continue;
                const ui::Rect row{sc.x + 14 * U, yy, sc.w - 14 * U, 22 * U};
                if (g.selectable(ui::widgetId("wm.srow", idx++), row, v.pick == jid(*s, "id"))) v.pick = jid(*s, "id"), centreOn(v, *s);
                c.spriteAt(settlementIcon(c, *s), Json(), row.x + 2 * U, row.y + 1 * U, 20 * U);
                c.text(row.x + 26 * U, row.y + 2 * U, c.fit(jstr(*s, "name") + " · " + jstr(*s, "size_name"), row.w - 30 * U, 16 * U),
                       jflag(*s, "here") ? pal::gold : pal::text, 16 * U);
                yy += 24 * U;
            }
            yy += 6 * U;
        }
    } else if (v.list == "caravans") {
        if (jarr(d, "caravans").empty()) c.text(sc.x + 2 * U, yy, W(c, "no_caravans"), pal::dim, 16 * U), yy += 20 * U;
        for (const auto& cv : jarr(d, "caravans")) {
            const ui::Rect row{sc.x, yy, sc.w, 40 * U};
            const long long key = kCaravanKey - jid(cv, "id");
            if (g.selectable(ui::widgetId("wm.crow", idx++), row, v.pick == key)) v.pick = key, centreOn(v, cv);
            c.spriteAt(caravanIcon(c, cv), Json(), row.x + 2 * U, row.y + 4 * U, 32 * U);
            std::string where;
            const Json* from = settlement(d, jid(cv, "from"));
            const Json* to = settlement(d, jid(cv, "to"));
            if (from && to) where = W(c, "route", {{"from", jstr(*from, "name")}, {"to", jstr(*to, "name")}});
            c.text(row.x + 40 * U, row.y + 1 * U, c.fit(jstr(cv, "kind_name") + (jstr(cv, "leader").empty() ? "" : " · " + jstr(cv, "leader")), row.w - 44 * U, 16 * U), pal::text, 16 * U);
            c.text(row.x + 40 * U, row.y + 20 * U, c.fit(where, row.w - 44 * U, 12 * U), pal::dim, 12 * U);
            yy += 44 * U;
        }
    } else {
        for (const auto& q : jarr(d, "pois")) {
            const ui::Rect row{sc.x, yy, sc.w, 40 * U};
            const long long key = -jid(q, "id") - 10;
            if (g.selectable(ui::widgetId("wm.prow", idx++), row, v.pick == key)) v.pick = key, centreOn(v, q);
            c.spriteAt(c.worldLook().value("poi_icons", Json::object()).value(jstr(q, "kind"), std::string("wm_ruins")), Json(), row.x + 2 * U, row.y + 4 * U, 32 * U);
            c.text(row.x + 40 * U, row.y + 1 * U, c.fit(jstr(q, "name"), row.w - 44 * U, 16 * U), pal::text, 16 * U);
            c.text(row.x + 40 * U, row.y + 20 * U, c.fit(jstr(q, "kind_name") + " · " + jstr(q, "near"), row.w - 44 * U, 12 * U), pal::dim, 12 * U);
            yy += 44 * U;
        }
    }
    g.endScroll(yy);
}

} // namespace

void openWorldMap(Ctx& c, Screens& s) {
    auto w = std::make_unique<Win>();
    w->id = "map";
    w->title = c.L("ui2d.wmap.title");
    w->big(c.u());
    w->data = {{"d", c.get("/api/worldmap")}, {"at", now()}, {"fresh", true}, {"hover", 0}};
    w->render = [](Ctx& c, Win& w, const ui::Rect& body) { render(c, w, body); };
    w->onPoll = [](Ctx& c, Win& w) {
        if (now() - jnum(w.data, "at") > 5000) {   // the realm moves by days: a slow poll
            w.data["d"] = c.get("/api/worldmap");
            w.data["at"] = now();
            w.data["fresh"] = true;
        }
    };
    w->onClose = [](Ctx&, Win& w) { viewOf(w) = View{}; };
    (void)s;
    s.push(std::move(w));
}

} // namespace game2d
