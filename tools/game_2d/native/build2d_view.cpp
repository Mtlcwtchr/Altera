// The build mode (B) and the colony window (O) in the 2D window (port of
// web/build.js and colony.js).
//   Build: planning from above as RimWorld has it - a carved tool panel over the
//   map (room, wall, door, window, floor, roof, pillar, furniture, strike;
//   material, roof, storey, for the household or the settlement). A stroke is
//   dragged (or a point clicked); the server expands it into the sim's modules
//   and checks each (/api/build/preview); the ghost shows what goes (green) and
//   what not (red); letting go lays it into the plan (/api/build/place). The plan
//   stays on the map in and out of the mode (/api/build/plan).
//   Colony: who rules, households, what each one does, rooms and buildings, the
//   plan's sites and what they lack, the offices; the colony mode.
#include <algorithm>
#include <cmath>
#include <set>

#include "native/ctx2d.hpp"

namespace game2d {

namespace {

double now() { return double(SDL_GetTicks()); }

const char* TOOLS[] = {"room", "wall", "door", "window", "floor", "roof", "pillar", "thing", "remove"};
bool pointTool(const std::string& t) { return t == "door" || t == "window" || t == "pillar" || t == "thing"; }

struct Build {
    bool on = false;
    std::string tool = "room", material = "log", roof = "roof_thatch", thing = "thing_bed", thingGroup;
    bool roofed = true, forTown = false;
    int side = 0, rot = 0, z = 0;
    Json cat;   // /api/build/catalog
    Json plan = Json::array();
    double planAt = -1e9;
    bool stroking = false;
    double ax = 0, ay = 0, bx = 0, by = 0;
    bool hover = false;
    double hx = 0, hy = 0;
    Json preview;
    std::string previewKey;
    float panelBottom = 0;
} B;

double cellM() { return B.cat.is_object() ? jnum(B.cat, "cell_m", 2) : 2.0; }

void loadCatalog(Ctx& c) {
    if (!B.cat.is_object() || B.cat.empty()) B.cat = c.get("/api/build/catalog");
}

void loadPlan(Ctx& c, bool force) {
    if (!force && now() - B.planAt < 1500) return;
    B.planAt = now();
    const double tp = c.tilePx();
    const int wM = int(std::ceil(c.mapW() / tp)) + 8, hM = int(std::ceil(c.mapH() / tp)) + 8;
    const int x = int(std::floor(c.camAtX() - wM / 2.0)), y = int(std::floor(c.camAtY() - hM / 2.0));
    const Json r = c.get("/api/build/plan?x=" + std::to_string(x) + "&y=" + std::to_string(y) + "&w=" + std::to_string(wM) + "&h=" + std::to_string(hM));
    if (r.is_object()) B.plan = r.value("pieces", Json::array());
}

Json strokeBody(double ax, double ay, double bx, double by) {
    return {{"tool", B.tool}, {"a", {ax, ay}}, {"b", {bx, by}}, {"z", B.z}, {"rot", B.rot}, {"side", B.side}, {"material", B.material},
            {"roof", B.roof}, {"roofed", B.roofed}, {"thing", B.thing}, {"for", B.forTown ? "settlement" : "self"}};
}

void wantPreview(Ctx& c, double ax, double ay, double bx, double by) {
    const double cm = cellM();
    auto sn = [&](double v) {
        return B.tool == "thing" ? long(std::floor(v)) : pointTool(B.tool) ? long(std::lround(v * 4 / cm)) : long(std::floor(v / cm));
    };
    const Json q = strokeBody(ax, ay, bx, by);
    const std::string key = Json::array({B.tool, sn(ax), sn(ay), sn(bx), sn(by), B.z, B.rot, B.side, B.material, B.roof, B.roofed, B.thing, B.forTown}).dump();
    if (key == B.previewKey) return;
    B.previewKey = key;
    B.preview = c.post("/api/build/preview", q);
}

void repreview(Ctx& c) {
    B.previewKey.clear();
    if (B.stroking) wantPreview(c, B.ax, B.ay, B.bx, B.by);
    else if (B.hover && pointTool(B.tool)) wantPreview(c, B.hx, B.hy, B.hx, B.hy);
    else B.preview = Json();
}

std::string why(Ctx& c, const std::string& k) { return k.empty() ? "" : c.L("ui2d.build.why." + k); }

std::string costText(const Json& list) {
    std::string s;
    for (const auto& x : list) s += (s.empty() ? "" : ", ") + jstr(x, "name") + " " + jstr(x, "n");
    return s;
}

void place(Ctx& c, double ax, double ay, double bx, double by) {
    const Json r = c.post("/api/build/place", strokeBody(ax, ay, bx, by));
    B.preview = Json();
    B.previewKey.clear();
    loadPlan(c, true);
    c.refreshMap();
    const std::string n = jstr(r, "placed"), bad = jstr(r, "bad");
    if (B.tool == "remove") c.toast(c.L("ui2d.build.removed", {{"n", n}}));
    else if (jnum(r, "bad") > 0) c.toast(c.L("ui2d.build.placed_bad", {{"n", n}, {"bad", bad}, {"why", why(c, jstr(r, "why"))}}));
    else c.toast(c.L("ui2d.build.placed", {{"n", n}}));
}

void pick(Ctx& c, const std::string& tool) {
    B.tool = tool;
    B.preview = Json();
    B.previewKey.clear();
    repreview(c);
}

void turn(Ctx& c) {
    if (B.tool == "room") B.side = (B.side + 1) % 4;
    else B.rot = (B.rot + 1) % 4;
    repreview(c);
}

// --- drawing the plan and the ghost ---------------------------------------------------
void hline(Ctx& c, float x0, float y0, float x1, float y1, float t, const ui::Colour& col) {
    // axis-aligned (edges, grid): a rectangle of thickness t
    if (std::abs(y1 - y0) < 0.5f) c.fill(std::min(x0, x1), y0 - t / 2, std::abs(x1 - x0), t, col);
    else c.fill(x0 - t / 2, std::min(y0, y1), t, std::abs(y1 - y0), col);
}

void drawPiece(Ctx& c, const Json& p, const ui::Colour& col, float alpha, bool ghost) {
    const double cm = cellM(), tp = c.tilePx();
    const float sc = float(tp / 16);
    const int r = ((int(jnum(p, "rot")) % 4) + 4) % 4;
    const std::string slot = jstr(p, "slot");
    const double px = jnum(p, "x"), py = jnum(p, "y");
    if (slot == "thing") {
        float x, y;
        c.toScreen(jnum(p, "cx"), jnum(p, "cy"), x, y);
        std::string obj = jstr(p, "makes");
        if (obj.empty() && B.cat.is_object())
            for (const auto& q : jarr(B.cat, "pieces"))
                if (jstr(q, "id") == jstr(p, "kind")) obj = jstr(q, "makes");
        const Json objs = c.map2d().value("objects", Json::object());
        const Json look = objs.contains(obj) ? objs[obj] : objs.value("_default", Json::object());
        c.spriteAt(jstr(look, "sprite"), look.value("recolour", Json()), x - 8 * sc, y - 8 * sc, 16 * sc, alpha * (ghost ? 0.8f : 0.6f));
        c.frame(x - float(tp) / 2 + 1, y - float(tp) / 2 + 1, float(tp) - 2, float(tp) - 2, col.withAlpha(alpha), std::max(1.0f, sc));
        return;
    }
    if (slot == "cell" || slot == "roof") {
        float x, y;
        c.toScreen(px * cm, py * cm, x, y);
        const float w = float(cm * tp), h = w;
        if (slot == "cell") {
            c.fill(x, y, w, h, col.withAlpha(alpha * 0.28f));
            c.frame(x, y, w, h, col.withAlpha(alpha), std::max(1.0f, sc));
        } else {
            for (float k = 4 * sc; k < w; k += 6 * sc)
                if (r == 0 || r == 2) hline(c, x + k, y, x + k, y + h, std::max(1.0f, sc * 0.75f), col.withAlpha(alpha * 0.5f));
                else hline(c, x, y + k, x + w, y + k, std::max(1.0f, sc * 0.75f), col.withAlpha(alpha * 0.5f));
            const float t = 2 * sc;
            if (r == 0) hline(c, x, y + 1, x + w, y + 1, t, col.withAlpha(alpha));
            else if (r == 2) hline(c, x, y + h - 1, x + w, y + h - 1, t, col.withAlpha(alpha));
            else if (r == 1) hline(c, x + w - 1, y, x + w - 1, y + h, t, col.withAlpha(alpha));
            else hline(c, x + 1, y, x + 1, y + h, t, col.withAlpha(alpha));
        }
        return;
    }
    if (slot == "edge") {
        double x0 = px * cm, y0 = py * cm, x1 = (px + 1) * cm, y1 = py * cm;
        if (r == 1) x0 = (px + 1) * cm, y0 = py * cm, x1 = (px + 1) * cm, y1 = (py + 1) * cm;
        else if (r == 2) x0 = px * cm, y0 = (py + 1) * cm, x1 = (px + 1) * cm, y1 = (py + 1) * cm;
        else if (r == 3) x0 = px * cm, y0 = py * cm, x1 = px * cm, y1 = (py + 1) * cm;
        float sx0, sy0, sx1, sy1;
        c.toScreen(x0, y0, sx0, sy0);
        c.toScreen(x1, y1, sx1, sy1);
        hline(c, sx0, sy0, sx1, sy1, (jstr(p, "cat") == "gable" ? 2 : 4) * sc, col.withAlpha(alpha));
        const float mx = (sx0 + sx1) / 2, my = (sy0 + sy1) / 2;
        const std::string kind = jstr(p, "kind");
        if (jflag(p, "door") || kind.find("door") != std::string::npos) c.fill(mx - 3 * sc, my - 3 * sc, 6 * sc, 6 * sc, pal::out.withAlpha(alpha));
        else if (kind.find("window") != std::string::npos) c.fill(mx - 2 * sc, my - 2 * sc, 4 * sc, 4 * sc, pal::hex("#a8d0e8", alpha));
        return;
    }
    const int dx = (r == 1 || r == 2) ? 1 : 0, dy = r >= 2 ? 1 : 0;
    float x, y;
    c.toScreen((px + dx) * cm, (py + dy) * cm, x, y);
    c.fill(x - 3 * sc, y - 3 * sc, 6 * sc, 6 * sc, col.withAlpha(alpha));
}

// --- the colony window -----------------------------------------------------------------
std::string nameOf(Ctx& c, long long id) {
    const Json* p = c.person(id);
    return p ? jstr(*p, "name") : "#" + std::to_string(id);
}

// a person's row: portrait, name, what they do; true when clicked
float personRow(Ctx& c, long long id, float x, float y, float w, int idx) {
    const float U = c.u();
    const Json* p = c.person(id);
    if (!p) return 0;
    const ui::Rect r{x, y, w, 30 * U};
    if (c.gui().selectable(ui::widgetId("col.who", idx), r, false)) {
        c.select(id);
        c.lookAt(jnum(*p, "x"), jnum(*p, "y"));
    }
    c.face(id, jstr(*p, "name"), x + 2 * U, y + 2 * U, 26 * U);
    std::string doing = jstr(*p, "act").empty() ? (jstr(*p, "goal").empty() ? c.L("ui2d.colony.idle") : jstr(*p, "goal")) : jstr(*p, "act");
    c.text(x + 34 * U, y + 1 * U, c.fit(jstr(*p, "name") + (jflag(*p, "pl") ? " *" : ""), w - 40 * U, 13 * U), pal::ink, 13 * U);
    c.text(x + 34 * U, y + 16 * U, c.fit(c.L("ui2d.age", {{"n", std::to_string(int(jnum(*p, "age")))}}) + " · " + doing, w - 40 * U, 11 * U),
           pal::ink.withAlpha(0.7f), 11 * U);
    return 32 * U;
}

// cards of people in columns
void peopleCards(Ctx& c, const ui::Rect& body, const std::vector<std::pair<std::string, std::vector<long long>>>& groups, const char* scrollId) {
    const float U = c.u();
    ui::Ui& g = c.gui();
    const ui::Rect in = g.beginScroll(ui::widgetId(scrollId), body);
    const float gap = 10 * U;
    const int cols = std::max(1, int((in.w + gap) / (300 * U + gap)));
    const float cw = (in.w - gap * (cols - 1)) / cols;
    std::vector<float> colY(std::size_t(cols), in.y);
    int idx = 0;
    for (const auto& [title, ids] : groups) {
        const std::size_t col = std::size_t(std::min_element(colY.begin(), colY.end()) - colY.begin());
        const float x = in.x + col * (cw + gap), y = colY[col];
        const float h = 30 * U + ids.size() * 32 * U + 6 * U;
        c.parchBox({x, y, cw, h});
        c.text(x + 8 * U, y + 6 * U, c.fit(title, cw - 16 * U, 15 * U), pal::ink, 15 * U);
        float yy = y + 28 * U;
        for (long long id : ids) yy += personRow(c, id, x + 4 * U, yy, cw - 8 * U, idx++);
        colY[col] = y + h + gap;
    }
    g.endScroll(*std::max_element(colY.begin(), colY.end()));
}

void renderColony(Ctx& c, Screens& s, Win& w, const ui::Rect& body) {
    const float U = c.u();
    ui::Ui& g = c.gui();
    const Json d = w.data.value("d", Json());
    if (!d.is_object()) {
        c.parchLines(body.x, body.y, body.w, {c.L("ui2d.loading")}, 14 * U);
        return;
    }
    w.title = c.L("ui2d.screen.colony") + " · " + jstr(d, "name");
    if (w.tab == "families") {
        std::vector<std::pair<std::string, std::vector<long long>>> groups;
        for (const auto& f : jarr(d, "families")) {
            std::vector<long long> ids;
            for (const auto& m : jarr(f, "members")) ids.push_back(m.get<long long>());
            std::string t = jstr(f, "name");
            if (jnum(f, "planned") > 0) t += " · " + c.L("ui2d.colony.fam_sites", {{"n", jstr(f, "planned")}, {"ready", jstr(f, "ready")}});
            groups.push_back({t, ids});
        }
        peopleCards(c, body, groups, "col.fams");
        return;
    }
    if (w.tab == "people") {
        std::map<std::string, std::vector<long long>> by;
        for (const auto& f : jarr(d, "families"))
            for (const auto& m : jarr(f, "members")) {
                const Json* p = c.person(m.get<long long>());
                if (!p) continue;
                const std::string k = !jstr(*p, "act").empty() ? jstr(*p, "act") : !jstr(*p, "goal").empty() ? jstr(*p, "goal") : c.L("ui2d.colony.idle");
                by[k].push_back(m.get<long long>());
            }
        std::vector<std::pair<std::string, std::vector<long long>>> groups(by.begin(), by.end());
        std::stable_sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) { return a.second.size() > b.second.size(); });
        for (auto& [k, ids] : groups) k += "  x" + std::to_string(ids.size());
        peopleCards(c, body, groups, "col.people");
        return;
    }
    if (w.tab == "offices") {
        std::vector<std::pair<std::string, std::vector<long long>>> groups;
        for (const auto& o : jarr(d, "offices")) {
            std::vector<long long> ids;
            for (const auto& h : jarr(o, "holders")) ids.push_back(h.get<long long>());
            groups.push_back({c.L("ui2d.right." + jstr(o, "right")), ids});
        }
        peopleCards(c, body, groups, "col.offices");
        return;
    }
    if (w.tab == "sites") {
        const Json& sites = jarr(d, "sites");
        std::string lack;
        for (const auto& l : jarr(d, "lack")) lack += (lack.empty() ? "" : ", ") + jstr(l, "name") + " " + jstr(l, "n");
        std::vector<std::string> head = {c.L("ui2d.colony.sites", {{"n", jstr(d, "planned")}, {"ready", jstr(d, "ready")}})};
        if (!lack.empty()) head.push_back(c.L("ui2d.build.lack") + ": " + lack);
        if (sites.empty()) head = {c.L("ui2d.colony.no_sites")};
        float y = body.y + c.parchLines(body.x, body.y, body.w, head, 13 * U) + 8 * U;
        if (sites.empty()) return;
        std::map<long long, std::string> famName;
        for (const auto& f : jarr(d, "families")) famName[jid(f, "id")] = jstr(f, "name");
        struct Row {
            const Json* s;
            std::string state;
            int n = 0;
            std::map<std::string, int> lack;
        };
        std::map<std::string, Row> rows;
        for (const auto& st : sites) {
            const std::string state = jflag(st, "ready") ? "ready" : !jflag(st, "held") ? "wait_hold" : "wait_goods";
            const std::string k = jstr(st, "kind") + "|" + jstr(st, "fam") + "|" + jstr(st, "z") + "|" + state;
            Row& r = rows[k];
            if (!r.s) r.s = &st, r.state = state;
            ++r.n;
            for (const auto& l : jarr(st, "lack")) r.lack[jstr(l, "name")] += int(jnum(l, "n"));
        }
        std::vector<Row*> list;
        for (auto& [k, r] : rows) list.push_back(&r);
        auto order = [](const std::string& s2) { return s2 == "ready" ? 0 : s2 == "wait_goods" ? 1 : 2; };
        std::stable_sort(list.begin(), list.end(), [&](Row* a, Row* b) {
            const int ma = jstr(*a->s, "who") == "mine" ? 0 : 1, mb = jstr(*b->s, "who") == "mine" ? 0 : 1;
            if (ma != mb) return ma < mb;
            if (order(a->state) != order(b->state)) return order(a->state) < order(b->state);
            return jnum(*a->s, "z") < jnum(*b->s, "z");
        });
        const float c1 = body.x + 8 * U, c2 = body.x + body.w * 0.34f, c3 = body.x + body.w * 0.54f, c4 = body.x + body.w * 0.72f;
        for (auto [cx, k] : {std::pair<float, const char*>{c1, "what"}, {c2, "whose"}, {c3, "state"}, {c4, "lack"}})
            c.text(cx, y, c.L(std::string("ui2d.colony.col.") + k), pal::gold, 13 * U, true);
        y += 22 * U;
        const ui::Rect in = g.beginScroll(ui::widgetId("col.sites"), {body.x, y, body.w, body.bottom() - y});
        float yy = in.y;
        int i = 0;
        for (Row* r : list) {
            const Json& st = *r->s;
            std::string what = jstr(st, "kind");
            for (const auto& q : jarr(B.cat, "pieces"))
                if (jstr(q, "id") == what) what = jstr(q, "name");
            if (r->n > 1) what += " x" + std::to_string(r->n);
            if (jnum(st, "z") > 0) what += " · " + c.L("ui2d.build.storey", {{"n", jstr(st, "z")}});
            const std::string whose = jstr(st, "who") == "settlement" ? c.L("ui2d.build.for.town") : famName[jid(st, "fam")];
            std::string lk;
            for (const auto& [n, k] : r->lack) lk += (lk.empty() ? "" : ", ") + n + " " + std::to_string(k);
            const ui::Rect row{in.x, yy, in.w, 22 * U};
            if (g.selectable(ui::widgetId("col.site", i++), row, false)) {
                c.lookAt(jnum(st, "cx"), jnum(st, "cy"));
                w.closing = true;
            }
            c.text(c1, yy + 3 * U, c.fit(what, c2 - c1 - 6 * U, 13 * U), pal::text, 13 * U);
            c.text(c2, yy + 3 * U, c.fit(whose, c3 - c2 - 6 * U, 13 * U), pal::dim, 13 * U);
            c.text(c3, yy + 3 * U, c.fit(c.L("ui2d.colony.st." + r->state), c4 - c3 - 6 * U, 13 * U), r->state == "ready" ? pal::good : pal::mid, 13 * U);
            c.text(c4, yy + 3 * U, c.fit(lk, in.right() - c4 - 6 * U, 12 * U), pal::bad, 12 * U);
            yy += 23 * U;
        }
        g.endScroll(yy);
        return;
    }
    // LR-145: the overview fills the big window: the settlement and its measures | who does
    // what, the buildings and rooms | the plan and the households
    const float gap = 10 * U, cw = (body.w - 2 * gap) / 3;
    auto section = [&](float x, float y, float h, const std::string& title, const std::string& note) {
        c.well({x, y, cw, h});
        c.text(x + 8 * U, y + 6 * U, title, pal::gold, 16 * U);
        if (!note.empty()) c.text(x + cw - 8 * U - c.textW(note, 16 * U), y + 6 * U, note, pal::dim, 16 * U);
        return ui::Rect{x + 6 * U, y + 30 * U, cw - 12 * U, h - 36 * U};
    };
    auto stat = [&](const ui::Rect& in, float& y, const char* key, const std::string& val, double bar, bool goodHigh) {
        c.text(in.x + 2 * U, y, c.L(std::string("ui2d.colony.stat.") + key), pal::text, 16 * U);
        if (bar >= 0) {
            const double v = std::clamp(bar, 0.0, 1.0);
            c.pips(in.x + in.w * 0.5f, y + (c.textH(16 * U) - 5 * c.pixel()) / 2, 10, v, pal::tone(goodHigh ? v : 1 - v));
        }
        c.text(in.right() - 2 * U - c.textW(val, 16 * U), y, val, pal::gold, 16 * U);
        y += 22 * U;
    };
    {
        const float x = body.x;
        const ui::Rect in = section(x, body.y, body.h, c.L("ui2d.colony.settlement"), "");
        std::vector<std::string> ls = {c.L("ui2d.colony.chief") + ": " + (jid(d, "chief") > 0 ? nameOf(c, jid(d, "chief")) : c.L("ui2d.colony.nobody")),
                                       c.L("ui2d.colony.people", {{"n", jstr(d, "people")}, {"grown", jstr(d, "grown")}, {"kids", jstr(d, "kids")}}),
                                       c.L("ui2d.colony.households", {{"n", std::to_string(jarr(d, "families").size())}})};
        std::string rights;
        for (const auto& r : jarr(d, "my_rights")) rights += (rights.empty() ? "" : ", ") + c.L("ui2d.right." + r.get<std::string>());
        if (!rights.empty()) ls.push_back(c.L("ui2d.colony.my_rights") + ": " + rights);
        float y = in.y + c.parchLines(in.x, in.y, in.w, ls, 16 * U) + 10 * U;
        const Json st = d.value("stats", Json::object());
        if (st.is_object() && jnum(st, "pop") > 0) {   // read from the map at the first day's end
            const double pop = std::max(1.0, jnum(st, "pop", 1));
            stat(in, y, "food", c.L("ui2d.colony.days", {{"n", std::to_string(int(jnum(st, "food") / pop))}}), jnum(st, "food") / pop / 60.0, true);
            stat(in, y, "wealth", std::to_string(int(std::lround(jnum(st, "wealth")))), -1, true);
            stat(in, y, "militia", std::to_string(int(std::lround(jnum(st, "militia")))), -1, true);
            stat(in, y, "morale", std::to_string(int(std::lround(jnum(st, "morale") * 100))), jnum(st, "morale"), true);
            stat(in, y, "unrest", std::to_string(int(std::lround(jnum(st, "unrest") * 100))), jnum(st, "unrest"), false);
            stat(in, y, "fort", std::to_string(int(std::lround(jnum(st, "fort") * 100))), jnum(st, "fort"), true);
            stat(in, y, "crafts", std::to_string(int(std::lround(jnum(st, "crafts") * 100))), jnum(st, "crafts"), true);
            y += 8 * U;
        }
        const bool mode = jflag(d, "colony_mode");
        if (g.toggle(ui::widgetId("col.mode"), {in.x, y, in.w, 34 * U}, c.L(mode ? "ui2d.colony.mode_on" : "ui2d.colony.mode_off"), mode)) {
            w.data["d"] = c.post("/api/colony/mode", {{"on", !mode}});
            c.setFollow(false);
            c.refresh();
        }
        y += 40 * U;
        if (g.button(ui::widgetId("col.build"), {in.x, y, in.w, 34 * U}, c.L("ui2d.screen.build"))) {
            w.closing = true;
            s.open("build");
        }
        y += 42 * U;
        for (const auto& row : c.wrap(c.L("ui2d.colony.mode_tip"), in.w, 16 * U)) {
            if (y > in.bottom() - 18 * U) break;
            c.text(in.x, y, row, pal::dim, 16 * U);
            y += 18 * U;
        }
    }
    {
        const float x = body.x + cw + gap;
        // who does what now
        std::map<std::string, int> by;
        int n = 0;
        for (const auto& f : jarr(d, "families"))
            for (const auto& m : jarr(f, "members")) {
                const Json* p = c.person(m.get<long long>());
                if (!p) continue;
                ++n;
                by[!jstr(*p, "act").empty() ? jstr(*p, "act") : !jstr(*p, "goal").empty() ? jstr(*p, "goal") : c.L("ui2d.colony.idle")]++;
            }
        std::vector<std::pair<std::string, int>> acts(by.begin(), by.end());
        std::stable_sort(acts.begin(), acts.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        const float h1 = std::round(body.h * 0.5f);
        const ui::Rect in = section(x, body.y, h1, c.L("ui2d.colony.doing"), c.L("ui2d.colony.of_n", {{"n", std::to_string(n)}}));
        float y = in.y;
        for (const auto& [k, v] : acts) {
            if (y > in.bottom() - 20 * U) break;
            c.text(in.x + 2 * U, y, c.fit(k, in.w * 0.5f, 16 * U), pal::text, 16 * U);
            c.pips(in.x + in.w * 0.55f, y + (c.textH(16 * U) - 5 * c.pixel()) / 2, 8, n > 0 ? double(v) / n : 0, pal::bronzeL);
            const std::string vs = std::to_string(v);
            c.text(in.right() - 2 * U - c.textW(vs, 16 * U), y, vs, pal::gold, 16 * U);
            y += 21 * U;
        }
        const float y2 = body.y + h1 + gap;
        const ui::Rect in2 = section(x, y2, body.bottom() - y2, c.L("ui2d.colony.buildings"), "");
        std::vector<std::string> bs, rs;
        for (const auto& b : jarr(d, "buildings")) bs.push_back(jstr(b, "name") + " x" + jstr(b, "n"));
        for (const auto& b : jarr(d, "rooms")) rs.push_back(jstr(b, "name") + " x" + jstr(b, "n"));
        if (bs.empty()) bs.push_back(c.L("ui2d.colony.none"));
        if (rs.empty()) rs.push_back(c.L("ui2d.colony.none"));
        float yy = in2.y + c.chips(in2.x, in2.y, in2.w, bs) + 10 * U;
        c.text(in2.x + 2 * U, yy, c.L("ui2d.colony.rooms"), pal::gold, 16 * U);
        yy += 24 * U;
        c.chips(in2.x, yy, in2.w, rs);
    }
    {
        const float x = body.x + 2 * (cw + gap);
        const float h1 = std::round(body.h * 0.4f);
        const ui::Rect in = section(x, body.y, h1, c.L("ui2d.colony.plan"), "");
        std::vector<std::string> ls = {c.L("ui2d.colony.sites", {{"n", jstr(d, "planned")}, {"ready", jstr(d, "ready")}})};
        std::string lack;
        for (const auto& l : jarr(d, "lack")) lack += (lack.empty() ? "" : ", ") + jstr(l, "name") + " " + jstr(l, "n");
        if (!lack.empty()) ls.push_back(c.L("ui2d.build.lack") + ": " + lack);
        ls.push_back(c.L("ui2d.build.free"));
        c.parchLines(in.x, in.y, in.w, ls, 16 * U);
        const float y2 = body.y + h1 + gap;
        const ui::Rect in2 = section(x, y2, body.bottom() - y2, c.L("ui2d.colony.tab.families"), "");
        const ui::Rect sc = g.beginScroll(ui::widgetId("col.ov.fams"), in2);
        float yy = sc.y;
        for (const auto& f : jarr(d, "families")) {
            std::string t = jstr(f, "name") + " · " + c.L("ui2d.colony.fam_n", {{"n", std::to_string(jarr(f, "members").size())}});
            if (jnum(f, "planned") > 0) t += " · " + c.L("ui2d.colony.fam_sites", {{"n", jstr(f, "planned")}, {"ready", jstr(f, "ready")}});
            c.text(sc.x + 2 * U, yy, c.fit(t, sc.w - 4 * U, 16 * U), pal::text, 16 * U);
            yy += 21 * U;
        }
        g.endScroll(yy);
    }
}

} // namespace

// --- the build mode ---------------------------------------------------------------------
bool buildActive(const Screens&) { return B.on; }

void openBuild(Ctx& c, Screens& s) {
    B.on = !B.on;
    if (B.on) {
        s.closeAll();
        loadCatalog(c);
        loadPlan(c, true);
    } else {
        B.stroking = false;
        B.preview = Json();
        B.previewKey.clear();
    }
}

void buildDrawWorld(Ctx& c, Screens&) {
    if (!B.cat.is_object() || B.cat.empty()) {
        if (now() - B.planAt > 5000) {   // the plan shows on the map out of the mode too
            B.planAt = now();
            loadCatalog(c);
        }
        if (!B.cat.is_object() || B.cat.empty()) return;
    }
    loadPlan(c, false);
    const int z = B.on ? B.z : 0;
    for (const auto& p : B.plan) {
        if (!jflag(p, "built") && jstr(p, "cat") == "roof" && jnum(p, "z") > z) {
            drawPiece(c, p, pal::hex("#c89a60"), B.on ? 0.35f : 0.3f, false);
            continue;
        }
        if (jflag(p, "built") || int(jnum(p, "z")) != z) continue;
        if (!B.on && jstr(p, "slot") == "edge" && jnum(p, "z") == 0) continue;
        const std::string who = jstr(p, "who");
        const ui::Colour col = who == "mine" ? (jarr(p, "lack").empty() ? pal::hex("#9ad07a") : pal::hex("#e8c060"))
                               : (who == "settlement" || who == "common") ? pal::hex("#78b8e8") : pal::hex("#b0a090");
        drawPiece(c, p, col, B.on ? 0.9f : 0.55f, false);
    }
    if (!B.on) return;
    if (B.preview.is_object())
        for (const auto& p : jarr(B.preview, "pieces")) {
            if (int(jnum(p, "z")) != z && B.tool != "room" && B.tool != "roof") continue;
            const bool ok = jflag(p, "ok");
            const ui::Colour col = B.tool == "remove" ? (ok ? pal::hex("#e05040") : pal::hex("#806060")) : (ok ? pal::hex("#70e070") : pal::hex("#e05040"));
            drawPiece(c, p, col, int(jnum(p, "z")) == z ? 0.95f : 0.5f, true);
        }
    const double cm = cellM();
    const float U = c.u();
    if (B.stroking && !pointTool(B.tool)) {
        const double x0 = std::floor(std::min(B.ax, B.bx) / cm) * cm, y0 = std::floor(std::min(B.ay, B.by) / cm) * cm;
        const double x1 = (std::floor(std::max(B.ax, B.bx) / cm) + 1) * cm, y1 = (std::floor(std::max(B.ay, B.by) / cm) + 1) * cm;
        float sx, sy, ex, ey;
        c.toScreen(x0, y0, sx, sy);
        c.toScreen(x1, y1, ex, ey);
        c.frame(sx, sy, ex - sx, ey - sy, pal::hex("#f0d080"), std::max(1.0f, U));
        c.text(sx + 4, sy - 20 * U, std::to_string(int(std::lround((x1 - x0) / cm))) + "x" + std::to_string(int(std::lround((y1 - y0) / cm))),
               pal::hex("#f0d080"), 14 * U, true);
    }
    if (B.hover) {   // the cell grid round the pointer
        const double gc = B.tool == "thing" ? jnum(B.cat, "thing_m", 1) : cm;
        const double gx = std::floor(B.hx / gc) * gc, gy = std::floor(B.hy / gc) * gc;
        const ui::Colour gl = ui::Colour(240 / 255.f, 208 / 255.f, 128 / 255.f, 0.25f);
        for (int k = -4; k <= 5; ++k) {
            float ax, ay, bx, by;
            c.toScreen(gx + k * gc, gy - 4 * gc, ax, ay);
            c.toScreen(gx + k * gc, gy + 5 * gc, bx, by);
            hline(c, ax, ay, bx, by, 1, gl);
            c.toScreen(gx - 4 * gc, gy + k * gc, ax, ay);
            c.toScreen(gx + 5 * gc, gy + k * gc, bx, by);
            hline(c, ax, ay, bx, by, 1, gl);
        }
    }
}

// the tool panel over the map (top left); true when the pointer is over it
void buildPanel(Ctx& c, Screens& s) {
    if (!B.on || !B.cat.is_object()) return;
    const float U = c.u();
    ui::Ui& g = c.gui();
    const float pw = std::min(c.mapW() - 20 * U, 600 * U), x0 = 10 * U, y0 = 10 * U, pad = 8 * U;
    // height grows with what is shown; drawn after measuring by a first pass is overkill: estimate
    float h = B.panelBottom > y0 ? B.panelBottom - y0 : 4 * 34 * U + 130 * U;   // as tall as last frame's contents
    const ui::Rect box{x0, y0, pw, h};
    c.carved(box);
    float x = x0 + pad, y = y0 + pad;
    c.text(x, y + 4 * U, c.L("ui2d.screen.build"), pal::gold, 18 * U, true);
    float hx = x + c.textW(c.L("ui2d.screen.build"), 18 * U) + 16 * U;
    if (g.toggle(ui::widgetId("bb.for", 0), {hx, y, 110 * U, 28 * U}, c.L("ui2d.build.for.self"), !B.forTown)) B.forTown = false, repreview(c);
    if (g.toggle(ui::widgetId("bb.for", 1), {hx + 114 * U, y, 120 * U, 28 * U}, c.L("ui2d.build.for.town"), B.forTown)) B.forTown = true, repreview(c);
    if (g.button(ui::widgetId("bb.close"), {box.right() - pad - 30 * U, y, 30 * U, 28 * U}, "X")) openBuild(c, s);
    y += 34 * U;
    // tools
    {
        const float bw = (pw - 2 * pad - 8 * 4 * U) / 9;
        for (int i = 0; i < 9; ++i)
            if (g.toggle(ui::widgetId("bb.tool", i), {x + i * (bw + 4 * U), y, bw, 30 * U}, c.fit(c.L(std::string("ui2d.build.tool.") + TOOLS[i]), bw - 6 * U, 12 * U),
                         B.tool == TOOLS[i]))
                pick(c, TOOLS[i]);
        y += 36 * U;
    }
    // material, roof
    {
        float bx = x;
        auto btn = [&](const std::string& label, bool on, int id) {
            const float bw = c.textW(label, 12 * U) + 18 * U;
            const bool hit = g.toggle(ui::widgetId("bb.row", id), {bx, y, bw, 28 * U}, label, on);
            bx += bw + 4 * U;
            return hit;
        };
        const std::string t = B.tool;
        if (t == "room" || t == "wall" || t == "door" || t == "window" || t == "roof")
            for (const char* m : {"log", "daub", "stone"})
                if (btn(c.L(std::string("ui2d.build.mat.") + m), B.material == m, int(m[0]))) B.material = m, repreview(c);
        if (t == "room" || t == "roof") {
            bx += 8 * U;
            int k = 0;
            for (const auto& p : jarr(B.cat, "pieces"))
                if (jstr(p, "cat") == "roof" && btn(jstr(p, "name"), B.roof == jstr(p, "id"), 200 + k++)) B.roof = jstr(p, "id"), repreview(c);
        }
        if (t == "room" && btn(c.L(B.roofed ? "ui2d.build.roofed" : "ui2d.build.open"), B.roofed, 300)) B.roofed = !B.roofed, repreview(c);
        y += 34 * U;
    }
    // storey, turn, colony
    {
        c.text(x, y + 6 * U, c.L("ui2d.build.storey", {{"n", std::to_string(B.z)}}), pal::text, 14 * U);
        float bx = x + 90 * U;
        if (g.button(ui::widgetId("bb.zdown"), {bx, y, 30 * U, 28 * U}, "-")) B.z = std::max(0, B.z - 1), repreview(c);
        if (g.button(ui::widgetId("bb.zup"), {bx + 34 * U, y, 30 * U, 28 * U}, "+")) B.z = std::min(4, B.z + 1), repreview(c);
        bx += 76 * U;
        if (B.tool == "room" || B.tool == "thing") {
            static const char* ARROWS[] = {"^", ">", "v", "<"};
            const std::string label = c.L("ui2d.build.turn") + " " + ARROWS[B.tool == "room" ? B.side : B.rot];
            const float bw = c.textW(label, 13 * U) + 20 * U;
            if (g.button(ui::widgetId("bb.turn"), {bx, y, bw, 28 * U}, label)) turn(c);
            bx += bw + 6 * U;
        }
        if (g.button(ui::widgetId("bb.colony"), {bx, y, 120 * U, 28 * U}, c.L("ui2d.screen.colony"))) s.open("colony");
        y += 34 * U;
    }
    // furniture by the rooms it makes
    if (B.tool == "thing") {
        std::map<std::string, const Json*> byObj;
        for (const auto& p : jarr(B.cat, "pieces"))
            if (jstr(p, "slot") == "thing") byObj[jstr(p, "makes")] = &p;
        std::set<std::string> used;
        float bx = x;
        int gi = 0;
        std::vector<std::string> openList;
        auto groupBtn = [&](const std::string& id, const std::string& name) {
            const float bw = c.textW(name, 12 * U) + 16 * U;
            if (bx + bw > box.right() - pad) bx = x, y += 30 * U;
            if (g.toggle(ui::widgetId("bb.group", gi++), {bx, y, bw, 26 * U}, name, B.thingGroup == id)) B.thingGroup = B.thingGroup == id ? "" : id;
            bx += bw + 4 * U;
        };
        for (const auto& r : jarr(B.cat, "rooms")) {
            std::vector<std::string> list;
            for (const auto& n : jarr(r, "needs"))
                for (const auto& o : jarr(n, "any"))
                    if (byObj.count(o.get<std::string>()) && std::find(list.begin(), list.end(), o.get<std::string>()) == list.end()) list.push_back(o.get<std::string>());
            if (list.empty()) continue;
            groupBtn(jstr(r, "id"), jstr(r, "name"));
            if (B.thingGroup == jstr(r, "id")) openList = list;
            for (const auto& o : list) used.insert(o);
        }
        std::vector<std::string> rest;
        for (const auto& [o, p] : byObj)
            if (!used.count(o)) rest.push_back(o);
        if (!rest.empty()) {
            groupBtn("_other", c.L("ui2d.build.other"));
            if (B.thingGroup == "_other") openList = rest;
        }
        y += 32 * U;
        bx = x;
        int ti = 0;
        const Json objs = c.map2d().value("objects", Json::object());
        for (const auto& o : openList) {
            const Json& p = *byObj[o];
            const float bw = 136 * U;
            if (bx + bw > box.right() - pad) bx = x, y += 34 * U;
            const ui::Rect br{bx, y, bw, 30 * U};
            const bool on = B.thing == jstr(p, "id");
            if (g.selectable(ui::widgetId("bb.thing", ti++), br, on)) B.thing = jstr(p, "id"), pick(c, "thing");
            c.fill(br.x, br.y, br.w, br.h, on ? pal::leather : pal::walnutD);
            c.frame(br.x, br.y, br.w, br.h, on ? pal::gold : pal::bronze.withAlpha(0.6f), U);
            const Json look = objs.contains(o) ? objs[o] : objs.value("_default", Json::object());
            c.spriteAt(jstr(look, "sprite"), look.value("recolour", Json()), br.x + 3 * U, br.y + 3 * U, 24 * U);
            c.text(br.x + 30 * U, br.y + 7 * U, c.fit(jstr(p, "name"), bw - 34 * U, 12 * U), pal::text, 12 * U);
            bx += bw + 4 * U;
        }
        y += 36 * U;
        for (const auto& p : jarr(B.cat, "pieces"))
            if (jstr(p, "id") == B.thing) {
                c.text(x, y, c.fit(c.L("ui2d.build.thing_now", {{"what", jstr(p, "name")}}) + " · " + costText(jarr(p, "cost")), pw - 2 * pad, 12 * U), pal::dim, 12 * U);
                y += 20 * U;
            }
    }
    // the tip, what the stroke would do, what the plan waits for
    std::vector<std::string> info = {c.L("ui2d.build.tip." + B.tool)};
    if (B.preview.is_object() && B.preview.contains("pieces")) {
        if (B.tool == "remove") {
            std::string s2 = c.L("ui2d.build.will_remove", {{"n", jstr(B.preview, "ok")}});
            if (jnum(B.preview, "bad") > 0) s2 += " · " + c.L("ui2d.build.not_yours", {{"n", jstr(B.preview, "bad")}});
            info.push_back(s2);
        } else {
            info.push_back(jnum(B.preview, "bad") > 0
                               ? c.L("ui2d.build.can_bad", {{"n", jstr(B.preview, "ok")}, {"bad", jstr(B.preview, "bad")}, {"why", why(c, jstr(B.preview, "why"))}})
                               : c.L("ui2d.build.can", {{"n", jstr(B.preview, "ok")}}));
            if (!jarr(B.preview, "cost").empty()) info.push_back(c.L("ui2d.build.will_bring") + ": " + costText(jarr(B.preview, "cost")));
        }
    } else info.push_back(c.L("ui2d.build.free"));
    {
        int n = 0, ready = 0;
        std::map<std::string, int> lack;
        for (const auto& p : B.plan) {
            const std::string who = jstr(p, "who");
            if (jflag(p, "built") || (who != "mine" && who != "settlement")) continue;
            ++n;
            if (jarr(p, "lack").empty() && jflag(p, "ready")) ++ready;
            for (const auto& l : jarr(p, "lack")) lack[jstr(l, "name")] += int(jnum(l, "n"));
        }
        if (n > 0) {
            std::string s2 = c.L("ui2d.build.sites", {{"n", std::to_string(n)}, {"ready", std::to_string(ready)}});
            if (!lack.empty()) {
                s2 += " · " + c.L("ui2d.build.lack") + ": ";
                bool first = true;
                for (const auto& [k, v] : lack) s2 += (first ? "" : ", ") + k + " " + std::to_string(v), first = false;
            }
            info.push_back(s2);
        }
    }
    y += c.parchLines(x, y, pw - 2 * pad, info, 12 * U) + pad;
    B.panelBottom = y;
    g.claim({x0, y0, pw, y - y0});
}

bool buildMapInput(Ctx& c, Screens&, double wx, double wy, const ui::Input& in, bool overUi) {
    if (!B.on) return false;
    if (!overUi) {
        B.hover = true;
        B.hx = wx, B.hy = wy;
        if (B.stroking) {
            B.bx = wx, B.by = wy;
            wantPreview(c, B.ax, B.ay, wx, wy);
        } else if (pointTool(B.tool)) wantPreview(c, wx, wy, wx, wy);
        else if (!B.preview.is_null()) B.preview = Json(), B.previewKey.clear();
    } else B.hover = false;
    if (in.pressed && !overUi) {
        if (pointTool(B.tool)) place(c, wx, wy, wx, wy);
        else {
            B.stroking = true;
            B.ax = B.bx = wx, B.ay = B.by = wy;
            B.previewKey.clear();
            wantPreview(c, wx, wy, wx, wy);
        }
    }
    if (in.released && B.stroking) {
        B.stroking = false;
        place(c, B.ax, B.ay, B.bx, B.by);
    }
    if (in.rightPressed && !overUi && !in.shift) {
        if (B.stroking) {
            B.stroking = false;
            B.preview = Json();
            B.previewKey.clear();
        } else B.on = false;
    }
    return true;
}

bool buildKey(Ctx& c, Screens& s, SDL_Scancode sc) {
    if (!B.on) return false;
    if (sc == SDL_SCANCODE_ESCAPE) {
        if (B.stroking) B.stroking = false, B.preview = Json();
        else openBuild(c, s);
        return true;
    }
    if (sc == SDL_SCANCODE_R) return turn(c), true;
    if (sc == SDL_SCANCODE_PAGEUP || sc == SDL_SCANCODE_RIGHTBRACKET) return B.z = std::min(4, B.z + 1), repreview(c), true;
    if (sc == SDL_SCANCODE_PAGEDOWN || sc == SDL_SCANCODE_LEFTBRACKET) return B.z = std::max(0, B.z - 1), repreview(c), true;
    if (sc == SDL_SCANCODE_T) return B.forTown = !B.forTown, repreview(c), true;
    if (sc == SDL_SCANCODE_X) return pick(c, "remove"), true;
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return pick(c, TOOLS[sc - SDL_SCANCODE_1]), true;
    return false;
}

void openColony(Ctx& c, Screens& s) {
    auto w = std::make_unique<Win>();
    w->id = "colony";
    w->title = c.L("ui2d.screen.colony");
    for (const char* t : {"overview", "families", "people", "sites", "offices"}) w->tabs.push_back({t, c.L(std::string("ui2d.colony.tab.") + t)});
    w->tab = "overview";
    w->big(c.u());   // LR-145: one size with the sheet and the dynasty
    loadCatalog(c);
    w->data = {{"d", c.get("/api/colony")}, {"at", now()}};
    w->render = [&s](Ctx& c, Win& w, const ui::Rect& body) { renderColony(c, s, w, body); };
    w->onPoll = [](Ctx& c, Win& w) {
        if (now() - jnum(w.data, "at") > 2500) {
            w.data["d"] = c.get("/api/colony");
            w.data["at"] = now();
        }
    };
    s.push(std::move(w));
}

} // namespace game2d
