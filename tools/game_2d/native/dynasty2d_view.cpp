// LR-145: the dynasty window (Y) of the 2D window: the house's head line, its members
// (alive, then the dead), the heir and the households, the house's chronicle. One size
// with the sheet and the colony (Win::big). Data: /api/dynasty (src/client2d/sheet2d).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include <SDL3/SDL.h>

#include "native/ctx2d.hpp"

namespace game2d {

namespace {

double now() { return double(SDL_GetTicks()); }
std::string D(Ctx& c, const std::string& k, const std::map<std::string, std::string>& a = {}) { return c.L("ui2d.dyn." + k, a); }
std::string num0(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.0f", v);
    return b;
}

// a box with a heading; its inside
ui::Rect section(Ctx& c, const ui::Rect& r, const std::string& title, const std::string& note = "") {
    const float U = c.u();
    c.well(r);
    c.text(r.x + 8 * U, r.y + 6 * U, title, pal::gold, 16 * U);
    if (!note.empty()) c.text(r.right() - 8 * U - c.textW(note, 16 * U), r.y + 6 * U, note, pal::dim, 16 * U);
    return {r.x + 6 * U, r.y + 30 * U, r.w - 12 * U, r.h - 36 * U};
}

std::string ageText(Ctx& c, const Json& m) { return c.L("ui2d.age", {{"n", std::to_string(int(jnum(m, "age")))}}); }

void render(Ctx& c, Screens& s, Win& w, const ui::Rect& body) {
    const float U = c.u(), p = c.pixel();
    ui::Ui& g = c.gui();
    const Json d = w.data.value("d", Json());
    if (!d.is_object()) {
        c.parchLines(body.x, body.y, body.w, {D(c, "none")}, 16 * U);
        return;
    }
    w.title = c.L("ui2d.screen.dynasty") + " · " + jstr(d, "name");
    const long long me = jid(d, "me"), heir = jid(d, "heir");
    std::vector<const Json*> alive, dead;
    for (const auto& m : jarr(d, "members")) (jflag(m, "alive") ? alive : dead).push_back(&m);
    auto byAge = [](const Json* a, const Json* b) { return jnum(*a, "age") > jnum(*b, "age"); };
    std::stable_sort(alive.begin(), alive.end(), byAge);
    std::stable_sort(dead.begin(), dead.end(), byAge);

    // --- the head line: the house's numbers ------------------------------------------------
    float y = body.y;
    {
        const float h = 34 * U;
        c.parchBox({body.x, y, body.w, h});
        c.icon("hud_crown", body.x + 8 * U, y + (h - 2 * c.iconSize()) / 2, 2 * c.iconSize());
        std::string line = D(c, "house", {{"name", jstr(d, "name")}});
        if (!jstr(d, "founder").empty()) line += " · " + D(c, "founder", {{"name", jstr(d, "founder")}});
        line += " · " + D(c, "count", {{"alive", jstr(d, "alive")}, {"grown", jstr(d, "grown")}, {"kids", jstr(d, "kids")}, {"dead", jstr(d, "dead")}});
        line += " · " + D(c, "purse", {{"n", num0(jnum(d, "purse"))}});
        line += " · " + D(c, "renown", {{"n", num0(jnum(d, "renown") * 100)}});
        c.text(body.x + 16 * U + 2 * c.iconSize(), y + (h - c.textH(16 * U)) / 2, c.fit(line, body.w - 30 * U - 2 * c.iconSize(), 16 * U), pal::ink, 16 * U);
        y += h + 8 * U;
    }
    const float gap = 10 * U, colH = body.bottom() - y;
    const float w1 = std::round(body.w * 0.42f), w2 = std::round(body.w * 0.26f), w3 = body.w - w1 - w2 - 2 * gap;

    // --- the members ---------------------------------------------------------------------
    {
        const ui::Rect in = section(c, {body.x, y, w1, colH}, D(c, "members"), D(c, "alive_n", {{"n", std::to_string(alive.size())}}));
        const ui::Rect sc = g.beginScroll(ui::widgetId("dyn.members"), in);
        float yy = sc.y;
        int idx = 0;
        auto row = [&](const Json& m, bool living) {
            const long long id = jid(m, "id");
            const float rh = 46 * U;
            const ui::Rect r{sc.x, yy, sc.w, rh};
            c.plate(r, id == me);
            const float port = rh - 4 * p - 4 * U;
            c.well({r.x + 2 * p + 2 * U, r.y + (rh - port) / 2, port, port});
            c.face(id, jstr(m, "name"), r.x + 3 * p + 2 * U, r.y + (rh - port) / 2 + p, port - 2 * p);
            const float tx = r.x + port + 4 * p + 8 * U;
            // the buttons: the sheet; a click on the row selects and looks
            const float bw = c.textW(D(c, "sheet"), 16 * U) + 16 * U;
            const ui::Rect br{r.right() - bw - 6 * U, r.y + (rh - 26 * U) / 2, bw, 26 * U};
            if (g.button(ui::widgetId("dyn.sheet", idx), br, D(c, "sheet"))) s.open("sheet", Json(id));
            std::string name = jstr(m, "name");
            std::string kin;
            for (const auto& k : jarr(m, "kin")) kin += (kin.empty() ? "" : ", ") + k.get<std::string>();
            if (id == me) kin = D(c, "you");
            const ui::Colour nc = !living ? pal::dim : id == me ? pal::gold : pal::text;
            float nx = tx;
            c.text(nx, r.y + 4 * U, c.fit(name, br.x - tx - 90 * U, 16 * U), nc, 16 * U);
            nx += std::min(c.textW(name, 16 * U), br.x - tx - 90 * U) + 8 * U;
            if (id == heir && living) {
                const std::string hs = D(c, "heir_tag");
                c.parchBox({nx, r.y + 4 * U, c.textW(hs, 16 * U) + 8 * U, c.textH(16 * U)});
                c.text(nx + 4 * U, r.y + 4 * U, hs, pal::ink, 16 * U);
            }
            std::string sub = kin.empty() ? "" : kin + " · ";
            sub += ageText(c, m);
            if (!jstr(m, "occ").empty()) sub += " · " + jstr(m, "occ");
            if (living) {
                const std::string doing = !jstr(m, "act").empty() ? jstr(m, "act") : jstr(m, "goal");
                if (!doing.empty()) sub += " · " + doing;
            }
            float sx = tx;
            if (living) sx += c.pips(tx, r.y + 26 * U + (c.textH(16 * U) - 5 * p) / 2, 6, jnum(m, "hp", 1), pal::bad) + 6 * U;
            c.text(sx, r.y + 24 * U, c.fit(sub, br.x - sx - 6 * U, 16 * U), pal::dim, 16 * U);
            if (living && g.selectable(ui::widgetId("dyn.row", idx), {r.x, r.y, br.x - r.x - 4 * U, rh}, false)) {
                c.select(id);
                if (const Json* pp = c.person(id)) c.lookAt(jnum(*pp, "x"), jnum(*pp, "y"));
            }
            ++idx;
            yy += rh + 4 * U;
        };
        for (const Json* m : alive) row(*m, true);
        if (!dead.empty()) {
            c.text(sc.x + 4 * U, yy + 4 * U, D(c, "dead_n", {{"n", std::to_string(dead.size())}}), pal::gold, 16 * U);
            yy += 28 * U;
            for (const Json* m : dead) row(*m, false);
        }
        g.endScroll(yy);
    }

    // --- the heir and the households ------------------------------------------------------
    {
        const float x = body.x + w1 + gap;
        const float hh = 150 * U;
        const ui::Rect in = section(c, {x, y, w2, hh}, D(c, "heir"));
        const Json* hm = nullptr;
        for (const Json* m : alive)
            if (jid(*m, "id") == heir) hm = m;
        if (!hm) c.text(in.x + 2 * U, in.y + 4 * U, D(c, "no_heir"), pal::dim, 16 * U);
        else {
            const float port = 72 * U;
            c.well({in.x, in.y, port, port});
            c.face(heir, jstr(*hm, "name"), in.x + p, in.y + p, port - 2 * p);
            const float tx = in.x + port + 8 * U, tw = in.right() - tx;
            c.text(tx, in.y + 2 * U, c.fit(jstr(*hm, "name"), tw, 16 * U), pal::gold, 16 * U);
            std::string kin;
            for (const auto& k : jarr(*hm, "kin")) kin += (kin.empty() ? "" : ", ") + k.get<std::string>();
            c.text(tx, in.y + 24 * U, c.fit((kin.empty() ? "" : kin + " · ") + ageText(c, *hm), tw, 16 * U), pal::dim, 16 * U);
            if (!jstr(*hm, "occ").empty()) c.text(tx, in.y + 46 * U, c.fit(jstr(*hm, "occ"), tw, 16 * U), pal::dim, 16 * U);
            c.text(in.x, in.y + port + 6 * U, c.fit(D(c, "heir_tip"), in.w, 16 * U), pal::dim.withAlpha(0.8f), 16 * U);
        }
        const float y2 = y + hh + gap;
        const ui::Rect in2 = section(c, {x, y2, w2, body.bottom() - y2}, D(c, "households"));
        float yy = in2.y;
        for (const auto& h : jarr(d, "households")) {
            int n = 0;
            for (const Json* m : alive)
                if (jstr(*m, "house") == jstr(h, "name")) ++n;
            std::vector<std::string> ls = {jstr(h, "name") + (jstr(h, "where").empty() ? "" : " · " + jstr(h, "where")),
                                           D(c, "house_n", {{"n", std::to_string(n)}})};
            yy += c.parchLines(in2.x, yy, in2.w, ls, 16 * U) + 6 * U;
            if (yy > in2.bottom() - 40 * U) break;
        }
        // couples of the house
        std::vector<std::string> pairs;
        for (const Json* m : alive) {
            const long long sp = jid(*m, "spouse");
            if (sp <= 0 || sp < jid(*m, "id")) continue;
            for (const Json* o : alive)
                if (jid(*o, "id") == sp) pairs.push_back(jstr(*m, "name") + " + " + jstr(*o, "name"));
        }
        if (!pairs.empty() && yy < in2.bottom() - 60 * U) {
            c.text(in2.x + 2 * U, yy + 2 * U, D(c, "couples"), pal::gold, 16 * U);
            yy += 24 * U;
            for (const auto& pr : pairs) {
                if (yy > in2.bottom() - 20 * U) break;
                c.text(in2.x + 4 * U, yy, c.fit(pr, in2.w - 8 * U, 16 * U), pal::text, 16 * U);
                yy += 20 * U;
            }
        }
    }

    // --- the house's chronicle -------------------------------------------------------------
    {
        const float x = body.x + w1 + w2 + 2 * gap;
        const ui::Rect in = section(c, {x, y, w3, colH}, D(c, "chronicle"));
        const ui::Rect sc = g.beginScroll(ui::widgetId("dyn.log"), in);
        float yy = sc.y;
        const Json& log = jarr(d, "log");
        if (log.empty()) {
            c.text(sc.x + 2 * U, yy, D(c, "quiet"), pal::dim, 16 * U);
            yy += 20 * U;
        }
        for (const auto& e : log) {
            const std::string day = c.L("ui2d.day", {{"n", std::to_string(int(jnum(e, "day")) + 1)}}) + " " + jstr(e, "t");
            c.text(sc.x + 2 * U, yy, day, pal::gold, 16 * U);
            yy += 18 * U;
            for (const auto& row : c.wrap(jstr(e, "text"), sc.w - 8 * U, 16 * U)) {
                c.text(sc.x + 6 * U, yy, row, pal::text, 16 * U);
                yy += 18 * U;
            }
            yy += 6 * U;
        }
        g.endScroll(yy);
    }
}

} // namespace

void openDynasty(Ctx& c, Screens& s) {
    auto w = std::make_unique<Win>();
    w->id = "dynasty";
    w->title = c.L("ui2d.screen.dynasty");
    w->big(c.u());
    w->data = {{"d", c.get("/api/dynasty")}, {"at", now()}};
    w->render = [&s](Ctx& c, Win& w, const ui::Rect& body) { render(c, s, w, body); };
    w->onPoll = [](Ctx& c, Win& w) {
        if (now() - jnum(w.data, "at") > 2500) {
            w.data["d"] = c.get("/api/dynasty");
            w.data["at"] = now();
        }
    };
    s.push(std::move(w));
}

} // namespace game2d
