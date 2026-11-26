// LR-162: the culture and faith window (V) and "inspect" of the 2D window, after the approved
// mockup (LR-155). Culture: the person's own ways, the culture card (values, customs, rites,
// food, how strangers are taken, magic) and all cultures to browse. Settlement: the customs
// the place holds (strength, state, trend, who kept them yesterday, carriers), who lives here
// by culture and faith, the place's values. Faith: the gods, pleasing deeds and sins, holy
// days, burial, clergy, zeal. All faiths: browse any. Data: /api/culture (src/client2d/culture2d).
// Inspect: a short sheet of another person with what this house knows (/api/inspect).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include <SDL3/SDL.h>

#include "native/ctx2d.hpp"

namespace game2d {

namespace {

double now() { return double(SDL_GetTicks()); }
std::string K(Ctx& c, const std::string& k, const std::map<std::string, std::string>& a = {}) { return c.L("ui2d.cult." + k, a); }
std::string I(Ctx& c, const std::string& k, const std::map<std::string, std::string>& a = {}) { return c.L("ui2d.insp." + k, a); }
std::string pct(double v) { return std::to_string(int(std::lround(v * 100))); }

std::string joined(const Json& a, const char* key = nullptr) {
    std::string s;
    if (!a.is_array()) return s;
    for (const auto& x : a) {
        const std::string v = key ? jstr(x, key) : (x.is_string() ? x.get<std::string>() : std::string());
        if (!v.empty()) s += (s.empty() ? "" : ", ") + v;
    }
    return s;
}

// a box with a heading; its inside
ui::Rect section(Ctx& c, const ui::Rect& r, const std::string& title, const std::string& note = "") {
    const float U = c.u();
    c.well(r);
    c.text(r.x + 8 * U, r.y + 6 * U, title, pal::gold, 16 * U);
    if (!note.empty()) c.text(r.right() - 8 * U - c.textW(note, 14 * U), r.y + 7 * U, note, pal::dim, 14 * U);
    return {r.x + 8 * U, r.y + 30 * U, r.w - 16 * U, r.h - 36 * U};
}

// wrapped lines of text; the height used
float para(Ctx& c, float x, float y, float w, const std::string& s, const ui::Colour& col, float px, int maxRows = 99) {
    const float lh = c.textH(px) + 4 * c.u();
    int n = 0;
    for (const auto& r : c.wrap(s, w, px)) {
        if (n >= maxRows) break;
        c.text(x, y + n * lh, r, col, px);
        ++n;
    }
    return n * lh;
}

// a diverging bar -1..1 about the middle: the value's name, the bar, the number
void valueBar(Ctx& c, float x, float y, float w, const Json& v, float extra = 0) {
    const float U = c.u(), p = c.pixel();
    const float lw = std::round(w * 0.42f), bx = x + lw, bw = w - lw - 40 * U, h = 5 * p;
    c.text(x, y, c.fit(jstr(v, "name"), lw - 6 * U, 14 * U), pal::text, 14 * U);
    const float by = y + (c.textH(14 * U) - h) / 2, mid = bx + bw / 2;
    c.fill(bx, by, bw, h, pal::out);
    c.fill(bx + p, by + p, bw - 2 * p, h - 2 * p, pal::leatherD);
    const double val = std::clamp(jnum(v, "v"), -1.0, 1.0);
    const float fw = float(std::abs(val)) * (bw / 2 - p);
    const ui::Colour col = val >= 0 ? pal::good : pal::bad;
    if (val >= 0) c.fill(mid, by + p, fw, h - 2 * p, col);
    else c.fill(mid - fw, by + p, fw, h - 2 * p, col);
    if (extra != 0) {   // what the customs add, lighter past the base
        const double tot = std::clamp(val + extra, -1.0, 1.0);
        const float ew = float(std::abs(tot)) * (bw / 2 - p);
        if (ew > fw) c.fill(tot >= 0 ? mid + fw : mid - ew, by + p, ew - fw, h - 2 * p, pal::gold.withAlpha(0.6f));
    }
    c.fill(mid, by - p, std::max(1.0f, p / 2), h + 2 * p, pal::bronzeL);
    char b[16];
    std::snprintf(b, sizeof b, "%+d", int(std::lround(jnum(v, "v") * 100)));
    c.text(bx + bw + 6 * U, y, b, val >= 0 ? pal::good : pal::bad, 14 * U);
}

// a bar 0..1 with a name and a number
void plainBar(Ctx& c, float x, float y, float w, const std::string& name, double v, const ui::Colour& col) {
    const float U = c.u(), lw = std::round(w * 0.5f);
    c.text(x, y, c.fit(name, lw - 6 * U, 14 * U), pal::text, 14 * U);
    c.gauge({x + lw, y + 2 * U, w - lw - 36 * U, c.textH(14 * U) - 4 * U}, v, col);
    c.text(x + w - 30 * U, y, pct(v), pal::dim, 14 * U);
}

void load(Ctx& c, Win& w) {
    std::string q = "/api/culture?id=" + std::to_string(jid(w.data, "id"));
    const std::string cp = jstr(w.data, "pickCulture"), fp = w.tab == "faiths" ? jstr(w.data, "pickFaith") : std::string();
    if (!cp.empty()) q += "&culture=" + cp;
    if (!fp.empty()) q += "&faith=" + fp;
    w.data["d"] = c.get(q);
    w.data["at"] = now();
    w.data["loaded"] = w.tab + "|" + cp + "|" + fp;
}

// --- the culture card -----------------------------------------------------------------------
void pageCulture(Ctx& c, Win& w, const Json& d, const ui::Rect& r) {
    const float U = c.u();
    ui::Ui& g = c.gui();
    const Json& pr = d.value("person", Json::object());
    const Json& cu = d.value("culture", Json::object());
    const float lw = 300 * U, gap = 10 * U;
    // the left column: the person and all cultures
    {
        const float ph = 128 * U;
        const ui::Rect in = section(c, {r.x, r.y, lw, ph}, jstr(pr, "name"));
        c.text(in.x, in.y, c.fit(jstr(pr, "cultureName") + " · " + jstr(pr, "speciesName"), in.w, 14 * U), pal::text, 14 * U);
        c.text(in.x, in.y + 20 * U, c.fit(jstr(pr, "faithName"), in.w, 14 * U), pal::text, 14 * U);
        const float py = in.y + 46 * U, pw = c.pixel() * 42;
        c.text(in.x, py, c.fit(K(c, "bond"), in.w - pw - 8 * U, 14 * U), pal::dim, 14 * U);
        c.pips(in.right() - pw, py + 3 * U, 10, jnum(pr, "bond"), pal::gold);
        c.text(in.x, py + 22 * U, c.fit(K(c, "piety"), in.w - pw - 8 * U, 14 * U), pal::dim, 14 * U);
        c.pips(in.right() - pw, py + 25 * U, 10, jnum(pr, "piety"), pal::good);
        const float y2 = r.y + ph + gap;
        const ui::Rect in2 = section(c, {r.x, y2, lw, r.bottom() - y2}, c.L("ui2d.cult.tab.culture"));
        const ui::Rect sc = g.beginScroll(ui::widgetId("cult.list"), in2);
        float yy = sc.y;
        int i = 0;
        for (const auto& x : jarr(d, "cultures")) {
            const std::string id = jstr(x, "id");
            const ui::Rect rr{sc.x, yy, sc.w, 26 * U};
            const bool on = id == jstr(cu, "id");
            if (g.selectable(ui::widgetId("cult.pick", i++), rr, on)) {
                w.data["pickCulture"] = id == jstr(pr, "culture") ? "" : id;
                load(c, w);
            }
            const ui::Colour col = id == jstr(pr, "culture") ? pal::gold : on ? pal::text : pal::dim;
            c.text(rr.x + 6 * U, rr.y + 5 * U, c.fit(jstr(x, "name"), rr.w - 12 * U, 14 * U), col, 14 * U);
            yy += 28 * U;
        }
        g.endScroll(yy);
    }
    const float x = r.x + lw + gap, rw = r.right() - x;
    if (!cu.is_object() || cu.empty()) return;
    // the head: name, people, faith, the description
    float y = r.y;
    {
        const std::string sub = jstr(cu, "speciesName") + " · " + jstr(cu, "faithName");
        const std::vector<std::string> rows = c.wrap(jstr(cu, "desc"), rw - 20 * U, 14 * U);
        const float lh = c.textH(14 * U) + 4 * U;
        const float h = 34 * U + std::min<std::size_t>(rows.size(), 3) * lh + 8 * U;
        c.parchBox({x, y, rw, h});
        c.text(x + 10 * U, y + 6 * U, jstr(cu, "name"), pal::ink, 20 * U);
        c.text(x + 20 * U + c.textW(jstr(cu, "name"), 20 * U), y + 10 * U, c.fit(sub, rw - 40 * U - c.textW(jstr(cu, "name"), 20 * U), 14 * U), pal::leather, 14 * U);
        for (std::size_t i = 0; i < rows.size() && i < 3; ++i) c.text(x + 10 * U, y + 34 * U + i * lh, rows[i], pal::ink, 14 * U);
        y += h + gap;
    }
    const float bottomH = 176 * U;
    const float midH = r.bottom() - y - bottomH - gap;
    const float vw = std::round(rw * 0.40f);
    {
        const ui::Rect in = section(c, {x, y, vw, midH}, K(c, "values"));
        float yy = in.y;
        for (const auto& v : jarr(cu, "values")) {
            if (yy > in.bottom() - 18 * U) break;
            valueBar(c, in.x, yy, in.w, v);
            yy += 22 * U;
        }
    }
    {
        const ui::Rect in = section(c, {x + vw + gap, y, rw - vw - gap, midH}, K(c, "traditions"));
        const ui::Rect sc = g.beginScroll(ui::widgetId("cult.trads"), in);
        float yy = sc.y;
        for (const auto& t : jarr(cu, "traditions")) {
            c.text(sc.x, yy, jstr(t, "name"), pal::text, 16 * U);
            const std::string kind = jstr(t, "kindName");
            if (!kind.empty()) {
                const float kx = sc.x + c.textW(jstr(t, "name"), 16 * U) + 10 * U;
                c.chips(kx, yy - 2 * U, sc.right() - kx, {kind});
            }
            yy += 24 * U;
            yy += para(c, sc.x + 6 * U, yy, sc.w - 12 * U, jstr(t, "desc"), pal::dim, 14 * U, 2) + 6 * U;
        }
        g.endScroll(yy);
    }
    // the three wells: rites and food, strangers, magic
    y = r.bottom() - bottomH;
    const float bw = (rw - 2 * gap) / 3;
    {
        const ui::Rect in = section(c, {x, y, bw, bottomH}, K(c, "rites"));
        float yy = in.y;
        for (const auto& rt : jarr(cu, "rites")) {
            c.text(in.x, yy, c.fit(jstr(rt, "kindName") + ": " + jstr(rt, "name"), in.w, 14 * U), pal::text, 14 * U);
            yy += 20 * U;
        }
        const Json& f = cu.value("food", Json::object());
        c.text(in.x, yy + 4 * U, K(c, "food"), pal::gold, 14 * U);
        yy += 24 * U;
        if (!jarr(f, "likes").empty()) yy += para(c, in.x, yy, in.w, K(c, "likes") + ": " + joined(jarr(f, "likes")), pal::good, 14 * U, 2);
        if (!jarr(f, "avoids").empty()) para(c, in.x, yy, in.w, K(c, "avoids") + ": " + joined(jarr(f, "avoids")), pal::bad, 14 * U, 2);
    }
    {
        const ui::Rect in = section(c, {x + bw + gap, y, bw, bottomH}, K(c, "welcome"));
        float yy = in.y;
        const float pw = c.pixel() * 22;
        for (const auto& wl : jarr(cu, "welcome")) {
            if (yy > in.bottom() - 18 * U) break;
            c.text(in.x, yy, c.fit(jstr(wl, "name"), in.w - pw - 8 * U, 14 * U), pal::text, 14 * U);
            c.pips(in.right() - pw, yy + 3 * U, 5, jnum(wl, "v"), pal::tone(jnum(wl, "v")));
            yy += 20 * U;
        }
    }
    {
        const ui::Rect in = section(c, {x + 2 * (bw + gap), y, bw, bottomH}, K(c, "magic"));
        float yy = in.y;
        if (!jarr(cu, "teach").empty()) yy += para(c, in.x, yy, in.w, K(c, "teach") + ": " + joined(jarr(cu, "teach")), pal::good, 14 * U, 2);
        if (!jarr(cu, "forbid").empty()) yy += para(c, in.x, yy, in.w, K(c, "forbid") + ": " + joined(jarr(cu, "forbid")), pal::bad, 14 * U, 2);
        yy += 4 * U;
        for (const auto& n : jarr(cu, "norms")) {
            if (yy > in.bottom() - 18 * U) break;
            c.text(in.x, yy, c.fit(jstr(n, "class") + ": " + jstr(n, "level"), in.w, 14 * U), pal::dim, 14 * U);
            yy += 20 * U;
        }
    }
}

// --- the settlement --------------------------------------------------------------------------
void pagePlace(Ctx& c, const Json& d, const ui::Rect& r) {
    const float U = c.u(), p = c.pixel();
    ui::Ui& g = c.gui();
    const Json& st = d.value("settlement", Json::object());
    if (!st.is_object() || st.empty()) {
        c.parchLines(r.x, r.y, r.w, {K(c, "none")}, 16 * U);
        return;
    }
    const float gap = 10 * U;
    float y = r.y;
    {
        const float h = 34 * U;
        c.parchBox({r.x, y, r.w, h});
        c.icon("hud_village", r.x + 8 * U, y + (h - 2 * c.iconSize()) / 2, 2 * c.iconSize());
        const std::string line = jstr(st, "name") + " · " + jstr(st, "cultureName") + " · " + K(c, "residents", {{"n", jstr(st, "residents")}});
        c.text(r.x + 16 * U + 2 * c.iconSize(), y + (h - c.textH(16 * U)) / 2, c.fit(line, r.w - 30 * U - 2 * c.iconSize(), 16 * U), pal::ink, 16 * U);
        y += h + gap;
    }
    const float rcw = 330 * U, lw = r.w - rcw - gap, colH = r.bottom() - y;
    // the customs table
    {
        const ui::Rect in = section(c, {r.x, y, lw, colH}, K(c, "traditions"));
        const double fixedT = jnum(st.value("thresholds", Json::object()), "fixed", 0.7), formT = jnum(st.value("thresholds", Json::object()), "forming", 0.25);
        const ui::Rect sc = g.beginScroll(ui::widgetId("cult.place"), in);
        float yy = sc.y;
        const float nameW = 190 * U, barW = 160 * U, statW = 120 * U;
        for (const auto& t : jarr(st, "traditions")) {
            const ui::Rect rr{sc.x, yy, sc.w, 46 * U};
            c.plate(rr, jstr(t, "status") == "fixed");
            c.text(rr.x + 8 * U, rr.y + 5 * U, c.fit(jstr(t, "name"), nameW - 10 * U, 16 * U), pal::text, 16 * U);
            c.text(rr.x + 8 * U, rr.y + 25 * U, c.fit(jstr(t, "kindName"), nameW - 10 * U, 13 * U), pal::dim, 13 * U);
            // strength with the marks of "forming" and "fixed"
            const float bx = rr.x + nameW, by = rr.y + 10 * U;
            const double s = jnum(t, "s");
            const ui::Colour col = s >= fixedT ? pal::good : s >= formT ? pal::mid : pal::bad;
            c.gauge({bx, by, barW, 6 * p}, s, col);
            for (double m : {formT, fixedT}) c.fill(bx + float(m) * barW, by - p, std::max(1.0f, p / 2), 8 * p, pal::text);
            c.text(bx, rr.y + 25 * U, pct(s) + "%", pal::dim, 13 * U);
            const float sx = bx + barW + 14 * U;
            const ui::Colour scol = jstr(t, "status") == "fixed" ? pal::good : jstr(t, "status") == "forming" ? pal::mid : pal::bad;
            c.text(sx, rr.y + 5 * U, c.fit(jstr(t, "statusName"), statW, 14 * U), scol, 14 * U);
            const bool up = jnum(t, "trend") > 0;
            c.text(sx, rr.y + 25 * U, c.fit(std::string(up ? "^ " : "v ") + K(c, up ? "trend.up" : "trend.down"), statW, 13 * U), up ? pal::good : pal::bad, 13 * U);
            const float ix = sx + statW + 10 * U, iw = rr.right() - ix - 6 * U;
            c.text(ix, rr.y + 5 * U, c.fit(K(c, "practised", {{"p", pct(jnum(t, "practised"))}}), iw, 13 * U), pal::text, 13 * U);
            c.text(ix, rr.y + 25 * U, c.fit(K(c, jflag(t, "carried") ? "carried" : "not_carried"), iw, 13 * U), jflag(t, "carried") ? pal::dim : pal::bad, 13 * U);
            yy += rr.h + 4 * U;
        }
        g.endScroll(yy);
    }
    // who lives here, the place's values
    const float x = r.x + lw + gap;
    const float ph = std::round(colH * 0.55f);
    {
        const ui::Rect in = section(c, {x, y, rcw, ph}, K(c, "people"));
        const ui::Rect sc = g.beginScroll(ui::widgetId("cult.people"), in);
        float yy = sc.y;
        const double total = std::max(1.0, jnum(st, "residents"));
        c.text(sc.x, yy, K(c, "by_culture"), pal::gold, 14 * U);
        yy += 20 * U;
        for (const auto& x2 : jarr(st, "cultures")) {
            plainBar(c, sc.x, yy, sc.w, jstr(x2, "name") + " " + jstr(x2, "n"), jnum(x2, "n") / total, pal::bronze);
            yy += 20 * U;
        }
        yy += 6 * U;
        c.text(sc.x, yy, K(c, "by_faith"), pal::gold, 14 * U);
        yy += 20 * U;
        for (const auto& f : jarr(st, "faiths")) {
            plainBar(c, sc.x, yy, sc.w, jstr(f, "name") + " " + jstr(f, "n"), jnum(f, "n") / total, pal::bronze);
            yy += 18 * U;
            const float pw = c.pixel() * 42;
            c.text(sc.x + 10 * U, yy, c.fit(K(c, "piety"), sc.w - pw - 20 * U, 13 * U), pal::dim, 13 * U);
            c.pips(sc.right() - pw, yy + 2 * U, 10, jnum(f, "piety"), pal::good);
            yy += 20 * U;
        }
        g.endScroll(yy);
    }
    {
        const float y2 = y + ph + gap;
        const ui::Rect in = section(c, {x, y2, rcw, r.bottom() - y2}, K(c, "values"));
        const ui::Rect sc = g.beginScroll(ui::widgetId("cult.pvals"), in);
        float yy = sc.y;
        for (const auto& v : jarr(st, "values")) {
            valueBar(c, sc.x, yy, sc.w, v);
            yy += 22 * U;
        }
        g.endScroll(yy);
    }
}

// --- a faith ---------------------------------------------------------------------------------
void faithCard(Ctx& c, const Json& f, const ui::Rect& r) {
    const float U = c.u();
    ui::Ui& g = c.gui();
    if (!f.is_object() || f.empty()) {
        c.parchLines(r.x, r.y, r.w, {K(c, "none")}, 16 * U);
        return;
    }
    const float gap = 10 * U;
    float y = r.y;
    {
        std::string sub = jstr(f, "kindName");
        if (jflag(f, "heresy")) sub += " · " + K(c, "heresy");
        if (f.contains("here")) sub += " · " + K(c, "here", {{"n", jstr(f, "here")}});
        const std::vector<std::string> rows = c.wrap(jstr(f, "desc"), r.w - 20 * U, 14 * U);
        const float lh = c.textH(14 * U) + 4 * U;
        const float h = 34 * U + std::min<std::size_t>(rows.size(), 2) * lh + 8 * U;
        c.parchBox({r.x, y, r.w, h});
        c.icon("hud_shrine", r.x + 8 * U, y + 6 * U, 2 * c.iconSize());
        const float nx = r.x + 16 * U + 2 * c.iconSize();
        c.text(nx, y + 6 * U, jstr(f, "name"), pal::ink, 20 * U);
        const float sx = nx + c.textW(jstr(f, "name"), 20 * U) + 12 * U;
        c.text(sx, y + 10 * U, c.fit(sub, r.right() - sx - 10 * U, 14 * U), jflag(f, "heresy") ? pal::bad : pal::leather, 14 * U);
        for (std::size_t i = 0; i < rows.size() && i < 2; ++i) c.text(r.x + 10 * U, y + 34 * U + i * lh, rows[i], pal::ink, 14 * U);
        y += h + gap;
    }
    const float rcw = std::min(340 * U, std::round(r.w * 0.34f)), lw = r.w - rcw - gap;
    // the gods: a grid of four
    const Json& gods = jarr(f, "deities");
    const int cols = 4;
    const int rowsN = std::max(1, int((gods.size() + cols - 1) / cols));
    const float gh = 30 * U + rowsN * 50 * U + 6 * U;
    {
        const ui::Rect in = section(c, {r.x, y, lw, gh}, K(c, "gods"));
        const float cw = (in.w - (cols - 1) * 6 * U) / cols;
        int i = 0;
        for (const auto& gd : gods) {
            const ui::Rect cr{in.x + (i % cols) * (cw + 6 * U), in.y + (i / cols) * 50 * U, cw, 44 * U};
            c.plate(cr);
            c.text(cr.x + 8 * U, cr.y + 5 * U, c.fit(jstr(gd, "name"), cr.w - 16 * U, 16 * U), pal::gold, 16 * U);
            c.text(cr.x + 8 * U, cr.y + 24 * U, c.fit(jstr(gd, "domainName"), cr.w - 16 * U, 13 * U), pal::dim, 13 * U);
            ++i;
        }
    }
    // pleasing deeds and sins
    {
        const float y2 = y + gh + gap, hw = (lw - gap) / 2, hh = r.bottom() - y2;
        double top = 0.01;
        for (const char* k : {"rites", "sins"})
            for (const auto& x : jarr(f, k)) top = std::max(top, jnum(x, "v"));
        auto deeds = [&](float x, const char* key, const std::string& title, const ui::Colour& col, const char* wid) {
            const ui::Rect in = section(c, {x, y2, hw, hh}, title);
            const ui::Rect sc = g.beginScroll(ui::widgetId(wid), in);
            float yy = sc.y;
            for (const auto& dd : jarr(f, key)) {
                plainBar(c, sc.x, yy, sc.w, jstr(dd, "name"), jnum(dd, "v") / top, col);
                yy += 22 * U;
            }
            g.endScroll(yy);
        };
        deeds(r.x, "rites", K(c, "rites_faith"), pal::good, "cult.deeds");
        deeds(r.x + hw + gap, "sins", K(c, "sins"), pal::bad, "cult.sins");
    }
    // the right column: holy days, then the faith's ways
    const float x = r.x + lw + gap;
    const float hdH = 30 * U + std::max<std::size_t>(1, jarr(f, "holy").size()) * 22 * U + 8 * U;
    {
        const ui::Rect in = section(c, {x, y, rcw, hdH}, K(c, "holy"));
        float yy = in.y;
        for (const auto& h : jarr(f, "holy")) {
            const int n = int(jnum(h, "in"));
            const std::string when = n == 0 ? K(c, "today") : K(c, "in_days", {{"n", std::to_string(n)}});
            const float ww = c.textW(when, 14 * U);
            c.text(in.x, yy, c.fit(jstr(h, "name"), in.w - ww - 10 * U, 14 * U), n == 0 ? pal::gold : pal::text, 14 * U);
            c.text(in.right() - ww, yy, when, n == 0 ? pal::gold : pal::dim, 14 * U);
            yy += 22 * U;
        }
    }
    {
        const float y2 = y + hdH + gap;
        const ui::Rect in = section(c, {x, y2, rcw, r.bottom() - y2}, c.L("ui2d.cult.tab.faith"));
        const ui::Rect sc = g.beginScroll(ui::widgetId("cult.ways"), in);
        float yy = sc.y;
        auto row = [&](const std::string& k, const std::string& v, const ui::Colour& col) {
            if (v.empty()) return;
            yy += para(c, sc.x, yy, sc.w, k + ": " + v, col, 14 * U, 3) + 2 * U;
        };
        row(K(c, "burial"), jstr(f, "burial"), pal::text);
        row(K(c, "clergy"), jstr(f, "clergy"), pal::text);
        row(K(c, "sacred"), joined(jarr(f, "sacred")), pal::text);
        row(K(c, "blessed"), joined(jarr(f, "blessed")), pal::good);
        row(K(c, "forbidden"), joined(jarr(f, "forbidden")), pal::bad);
        yy += 4 * U;
        const float pw = c.pixel() * 42;
        for (const auto& [k, col] : {std::pair<const char*, ui::Colour>{"zeal", pal::bad}, {"tolerance", pal::good}}) {
            c.text(sc.x, yy, c.fit(K(c, k), sc.w - pw - 8 * U, 14 * U), pal::dim, 14 * U);
            c.pips(sc.right() - pw, yy + 3 * U, 10, jnum(f, k), col);
            yy += 22 * U;
        }
        if (!jarr(f, "values").empty()) {
            c.text(sc.x, yy + 4 * U, K(c, "values"), pal::gold, 14 * U);
            yy += 26 * U;
            for (const auto& v : jarr(f, "values")) {
                valueBar(c, sc.x, yy, sc.w, v);
                yy += 22 * U;
            }
        }
        g.endScroll(yy);
    }
}

void pageFaiths(Ctx& c, Win& w, const Json& d, const ui::Rect& r) {
    const float U = c.u(), lw = 240 * U, gap = 10 * U;
    ui::Ui& g = c.gui();
    const Json& f = d.value("faith", Json::object());
    const std::string mine = jstr(d.value("person", Json::object()), "faith");
    const ui::Rect in = section(c, {r.x, r.y, lw, r.h}, c.L("ui2d.cult.tab.faiths"));
    const ui::Rect sc = g.beginScroll(ui::widgetId("cult.faiths"), in);
    float yy = sc.y;
    int i = 0;
    for (const auto& x : jarr(d, "faiths")) {
        const std::string id = jstr(x, "id");
        const ui::Rect rr{sc.x, yy, sc.w, 28 * U};
        if (g.selectable(ui::widgetId("cult.fpick", i++), rr, id == jstr(f, "id"))) {
            w.data["pickFaith"] = id;
            load(c, w);
        }
        c.text(rr.x + 6 * U, rr.y + 6 * U, c.fit(jstr(x, "name"), rr.w - 12 * U, 14 * U), id == mine ? pal::gold : pal::text, 14 * U);
        yy += 30 * U;
    }
    g.endScroll(yy);
    faithCard(c, f, {r.x + lw + gap, r.y, r.w - lw - gap, r.h});
}

void render(Ctx& c, Win& w, const ui::Rect& body) {
    const float U = c.u();
    const std::string want = w.tab + "|" + jstr(w.data, "pickCulture") + "|" + (w.tab == "faiths" ? jstr(w.data, "pickFaith") : std::string());
    if (jstr(w.data, "loaded") != want) load(c, w);
    const Json& d = w.data.value("d", Json());
    if (!d.is_object()) {
        c.parchLines(body.x, body.y, body.w, {K(c, "none")}, 16 * U);
        return;
    }
    if (w.tab == "place") pagePlace(c, d, body);
    else if (w.tab == "faith") faithCard(c, d.value("faith", Json::object()), body);
    else if (w.tab == "faiths") pageFaiths(c, w, d, body);
    else pageCulture(c, w, d, body);
}

// --- inspect ---------------------------------------------------------------------------------
std::string knowLabel(Ctx& c, const Json& d) {
    if (jflag(d, "kin")) return I(c, "know.kin");
    const double k = jnum(d, "know");
    return I(c, k >= 0.6 ? "know.well" : k >= 0.3 ? "know.some" : k >= 0.1 ? "know.little" : "know.none");
}

void renderInspect(Ctx& c, Screens& s, Win& w, const ui::Rect& body) {
    const float U = c.u();
    ui::Ui& g = c.gui();
    const Json& d = w.data.value("d", Json());
    if (!d.is_object()) {
        c.parchLines(body.x, body.y, body.w, {I(c, "know.none")}, 16 * U);
        return;
    }
    const long long id = jid(d, "id");
    w.title = I(c, "title") + " · " + jstr(d, "name");
    float y = body.y;
    // the head: portrait, name, what is seen, how well we know them
    {
        const float port = 84 * U;
        c.well({body.x, y, port, port});
        c.face(id, jstr(d, "name"), body.x + c.pixel(), y + c.pixel(), port - 2 * c.pixel());
        const float tx = body.x + port + 12 * U, tw = body.right() - tx;
        c.text(tx, y + 2 * U, c.fit(jstr(d, "name"), tw, 20 * U), jflag(d, "known") ? pal::gold : pal::dim, 20 * U);
        std::string seen = jstr(d, "speciesName");
        if (!jstr(d, "stage").empty()) seen += " · " + jstr(d, "stage");
        c.text(tx, y + 28 * U, c.fit(seen, tw, 14 * U), pal::text, 14 * U);
        if (!jstr(d, "act").empty()) c.text(tx, y + 46 * U, c.fit(jstr(d, "act"), tw, 14 * U), pal::dim, 14 * U);
        const float pw = c.pips(tx, y + 68 * U, 10, jnum(d, "know"), pal::gold);
        std::string kl = knowLabel(c, d);
        if (!jstr(d, "knower").empty()) kl += " · " + I(c, "via", {{"name", jstr(d, "knower")}});
        c.text(tx + pw + 8 * U, y + 64 * U, c.fit(kl, tw - pw - 8 * U, 14 * U), pal::dim, 14 * U);
        y += port + 10 * U;
    }
    const Json& f = d.value("fields", Json::object());
    // what is known; the hidden greyed
    {
        c.text(body.x, y, I(c, "facts"), pal::gold, 16 * U);
        y += 24 * U;
        const float kw = 130 * U;
        auto row = [&](const char* key, const std::string& value, bool shown, const ui::Colour& col = pal::text) {
            c.text(body.x + 6 * U, y, I(c, key), pal::dim, 14 * U);
            const std::string v = shown ? value : I(c, "hidden");
            y += std::max(20 * U, para(c, body.x + kw, y, body.w - kw, v, shown ? col : pal::dim.withAlpha(0.6f), 14 * U, 2));
        };
        row("age", f.contains("age") ? c.L("ui2d.age", {{"n", std::to_string(int(jnum(f, "age")))}}) : "", f.contains("age"));
        row("house", jstr(f, "house"), f.contains("house"));
        if (f.contains("home")) row("home", jstr(f, "home"), true);
        row("occupation", jstr(f, "occupation"), f.contains("occupation"));
        if (f.contains("culture")) {
            const Json& cu = f["culture"];
            row("culture", jstr(cu, "name") + (jflag(cu, "guess") ? " (" + I(c, "by_look") + ")" : ""), true);
        } else row("culture", "", false);
        if (f.contains("faith")) {
            const Json& fa = f["faith"];
            std::string v = jstr(fa, "name");
            if (fa.contains("piety")) v += " · " + K(c, "piety") + " " + pct(jnum(fa, "piety"));
            row("faith", v, true);
        } else row("faith", "", false);
        if (f.contains("mood")) row("mood", pct(jnum(f, "mood")), true, pal::tone(jnum(f, "mood")));
        else row("mood", "", false);
        row("traits", joined(jarr(f, "traits")), f.contains("traits"));
        std::string sk;
        for (const auto& x : jarr(f, "skills")) sk += (sk.empty() ? "" : ", ") + jstr(x, "name") + " " + pct(jnum(x, "v"));
        row("skills", sk, f.contains("skills"));
        std::string rel;
        for (const auto& x : jarr(f, "relations")) rel += (rel.empty() ? "" : ", ") + jstr(x, "name") + (jnum(x, "v") >= 0 ? " +" : " ") + pct(jnum(x, "v"));
        row("relations", rel, f.contains("relations"));
        y += 6 * U;
    }
    // our side: how we feel, what we share
    if (d.contains("mine") || !jarr(d, "common").empty()) {
        c.text(body.x, y, I(c, "mine"), pal::gold, 16 * U);
        y += 24 * U;
        if (d.contains("mine")) {
            const Json& m = d["mine"];
            const float hw = (body.w - 12 * U) / 2;
            int i = 0;
            for (const char* k : {"familiarity", "trust", "affinity", "respect"}) {
                const float bx = body.x + 6 * U + (i % 2) * (hw + 6 * U);
                const double v = jnum(m, k);
                plainBar(c, bx, y, hw, I(c, "rel." + std::string(k)), k == std::string("affinity") ? (v + 1) / 2 : v,
                         k == std::string("affinity") ? pal::tone((v + 1) / 2) : pal::bronze);
                if (i % 2) y += 22 * U;
                ++i;
            }
        }
        if (!jarr(d, "common").empty()) {
            std::vector<std::string> cs;
            for (const auto& x : jarr(d, "common")) cs.push_back(x.get<std::string>());
            c.text(body.x + 6 * U, y + 2 * U, I(c, "common"), pal::dim, 14 * U);
            y += c.chips(body.x + 130 * U, y, body.w - 130 * U, cs) + 6 * U;
        }
        y += 6 * U;
    }
    if (!jarr(d, "memories").empty()) {
        c.text(body.x, y, I(c, "memories"), pal::gold, 16 * U);
        y += 24 * U;
        for (const auto& m : jarr(d, "memories")) {
            const std::string day = c.L("ui2d.day", {{"n", std::to_string(int(jnum(m, "day")) + 1)}});
            const ui::Colour col = jnum(m, "v") > 0.05 ? pal::good : jnum(m, "v") < -0.05 ? pal::bad : pal::text;
            c.text(body.x + 6 * U, y, day, pal::dim, 13 * U);
            std::string t = jstr(m, "text");
            if (!jstr(m, "by").empty()) t += " (" + I(c, "by", {{"name", jstr(m, "by")}}) + ")";
            y += std::max(20 * U, para(c, body.x + 90 * U, y, body.w - 90 * U, t, col, 14 * U, 2)) + 2 * U;
        }
        y += 6 * U;
    }
    // the buttons
    const float bw = (body.w - 12 * U) / 3;
    const bool alive = jflag(d, "alive") && jstr(d, "sp").empty();
    if (alive && c.meId() >= 0 && id != c.meId() && g.button(ui::widgetId("insp.talk"), {body.x, y, bw, 32 * U}, I(c, "talk"))) {
        w.closing = true;
        s.open("talk", Json(id));
    }
    if (jstr(d, "sp").empty() && g.button(ui::widgetId("insp.cult"), {body.x + bw + 6 * U, y, bw, 32 * U}, c.L("ui2d.screen.culture"))) {
        w.closing = true;
        // the window shows the faith and culture we know; a stranger's own is not given away
        s.open("culture", Json(c.meId()));
        if (Win* t = s.top(); t && t->id == "culture") {
            const Json& fl = d.value("fields", Json::object());
            if (fl.contains("culture")) t->data["pickCulture"] = jstr(fl["culture"], "id");
            if (fl.contains("faith")) {
                t->data["pickFaith"] = jstr(fl["faith"], "id");
                t->tab = "faiths";
            }
        }
    }
    if (jflag(d, "kin") && g.button(ui::widgetId("insp.sheet"), {body.x + 2 * (bw + 6 * U), y, bw, 32 * U}, I(c, "sheet"))) {
        w.closing = true;
        s.open("sheet", Json(id));
    }
}

} // namespace

void openCulture(Ctx& c, Screens& s, long long id) {
    if (id < 0) {
        const Json* p = c.person(c.selected());
        id = p && jstr(*p, "sp").empty() ? c.selected() : c.meId();
    }
    auto w = std::make_unique<Win>();
    w->id = "culture";
    w->title = c.L("ui2d.screen.culture");
    for (const char* t : {"culture", "place", "faith", "faiths"}) w->tabs.push_back({t, c.L(std::string("ui2d.cult.tab.") + t)});
    w->tab = "culture";
    w->big(c.u());
    w->data = {{"id", id}, {"pickCulture", ""}, {"pickFaith", ""}, {"loaded", ""}};
    w->render = [](Ctx& c, Win& w, const ui::Rect& body) { render(c, w, body); };
    w->onPoll = [](Ctx& c, Win& w) {
        if (now() - jnum(w.data, "at") > 3000) load(c, w);
    };
    s.push(std::move(w));
}

void openInspect(Ctx& c, Screens& s, long long target) {
    if (target < 0) return;
    const Json d = c.get("/api/inspect?id=" + std::to_string(target));
    if (!d.is_object()) return;
    auto w = std::make_unique<Win>();
    w->id = "inspect";
    w->title = c.L("ui2d.insp.title");
    w->wantW = 600 * c.u();
    w->autoFit = true;
    w->data = {{"id", target}, {"d", d}, {"at", now()}};
    w->render = [&s](Ctx& c, Win& w, const ui::Rect& body) { renderInspect(c, s, w, body); };
    w->onPoll = [](Ctx& c, Win& w) {
        if (now() - jnum(w.data, "at") > 2500) {
            const Json d = c.get("/api/inspect?id=" + std::to_string(jid(w.data, "id")));
            if (d.is_object()) w.data["d"] = d;
            w.data["at"] = now();
        }
    };
    s.push(std::move(w));
}

} // namespace game2d
