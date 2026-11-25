// The workshop (K) in the 2D window (port of web/craft.js): the crafting UI the
// user chose (gdd/ui/CRAFT_UI_VARIANTS.md: C + B with A, D as the helper).
//   C: the stations round the person, the heat cells (the wheel works the bellows)
//   B: the processes as a ring round the work piece, dim ones with the reason;
//      target shapes and ways as chips; the wheel over the piece sets the length
//   A: the bag's materials and made things go onto the piece's slots by a click
//      (a click on a token on the piece takes it off)
//   D: the helper - a wish, the designs that answer it, their chain, the step that
//      can start now laid on the bench.
// One main button: "Make" held (E or the mouse). Reads POST /api/workshop
// {draft, wish, texts}; acts with craft_attempt / craft_fast / craft_inspect / craft_repair.
#include <algorithm>
#include <cmath>

#include "engine/ui/font.hpp"
#include "native/ctx2d.hpp"

namespace game2d {

namespace {

double now() { return double(SDL_GetTicks()); }
int pct(double v) { return int(std::lround(std::clamp(v, 0.0, 1.0) * 100)); }
constexpr double kHoldMs = 700;

std::string firstLetters(const std::string& s, int n) {
    std::size_t at = 0;
    for (int i = 0; i < n && at < s.size(); ++i) ui::nextCodepoint(s, at);
    return s.substr(0, at);
}

struct Craft {
    Ctx& c;
    Win& w;
    const Json& ws() const { return w.data["ws"]; }
    Json& draft() { return w.data["draft"]; }
    std::string T(const std::string& k, const std::map<std::string, std::string>& a = {}) const {
        const Json& tx = w.data["tx"];
        std::string s = tx.contains(k) && tx[k].is_string() ? tx[k].get<std::string>() : k;
        for (const auto& [n, v] : a) {
            const std::string tag = "{" + n + "}";
            for (std::size_t p = s.find(tag); p != std::string::npos; p = s.find(tag, p + v.size())) s.replace(p, tag.size(), v);
        }
        return s;
    }
    std::string L(const std::string& k, const std::map<std::string, std::string>& a = {}) const { return c.L(k, a); }
    int heat() const { return w.data.value("heat", -1); }
    const Json* proc(const std::string& id) const {
        for (const auto& p : jarr(ws(), "processes"))
            if (jstr(p, "id") == id) return &p;
        return nullptr;
    }
    Json draftOut() {
        Json d = Json::object();
        const Json& dr = draft();
        if (jstr(dr, "process").empty()) return d;
        d["process"] = dr["process"];
        d["slots"] = dr.value("slots", Json::object());
        for (const char* k : {"shape", "mode"})
            if (!jstr(dr, k).empty()) d[k] = dr[k];
        if (jnum(dr, "len") > 0) d["len"] = dr["len"];
        const Json* p = proc(jstr(dr, "process"));
        if ((p && jnum(*p, "heat_min") > 0) || (heat() >= 0 && jnum(ws(), "heat_max") > 0)) d["heat"] = std::max(0, heat());
        return d;
    }
    void load(const Json& extra = Json::object()) {
        Json body = {{"draft", draftOut()}, {"wish", jstr(w.data, "wish")}, {"texts", w.data["tx"].empty()}};
        for (const auto& [k, v] : extra.items()) body[k] = v;
        const Json r = c.post("/api/workshop", body);
        w.data["at"] = now();
        if (!r.is_object()) return;
        if (r.contains("texts") && r["texts"].is_object()) w.data["tx"] = r["texts"];
        w.data["ws"] = r.value("ws", Json());
        const Json& s = ws();
        if (!s.is_object()) return;
        if (!w.data.contains("seq0")) w.data["seq0"] = s.contains("last") && s["last"].is_object() ? jnum(s["last"], "seq") : 0.0;
        const int hm = int(jnum(s, "heat_max"));
        if (heat() < 0 || heat() > hm) w.data["heat"] = hm;
        w.title = jarr(s, "stations").empty() ? L("ui2d.craft.at_none") : [&] {
            std::string list;
            for (const auto& st : jarr(s, "stations")) list += (list.empty() ? "" : " · ") + jstr(st, "name");
            return L("ui2d.craft.at", {{"list", list}});
        }();
    }
    void act(const std::string& type, Json args) {
        args["id"] = jid(ws(), "who");
        const Json r = c.cmd({{"op", "cmd"}, {"type", type}, {"args", args}});
        const std::string res = jstr(r, "result");
        if (!res.empty() && res != "ok" && res.rfind("ok", 0) != 0) c.toast(jstr(r, "text").empty() ? res : jstr(r, "text"));
        load();
    }
    const Json* thing(const std::string& ref) const {
        for (const char* k : {"materials", "made"})
            for (const auto& t : jarr(ws(), k))
                if (jstr(t, "ref") == ref) return &t;
        return nullptr;
    }
    // a thing's picture: its own sprite (craft2d.json forms / items, resources.json items), else a letter of its name
    void thingIcon(const Json& t, float x, float y, float size) {
        const Json& look = c.craftLook();
        const Json& res = c.resources();
        Json r;
        const std::string form = jstr(t, "form"), item = jstr(t, "item");
        if (look.contains("forms") && look["forms"].contains(form)) r = look["forms"][form];
        else if (look.contains("items") && look["items"].contains(item)) r = look["items"][item];
        else if (res.is_object() && res.contains("items") && res["items"].contains(item)) r = res["items"][item];
        else if (res.is_object() && res.contains("items") && res["items"].contains(form)) r = res["items"][form];
        if (r.is_object() && !jstr(r, "sprite").empty() && c.spriteAt(jstr(r, "sprite"), r.value("recolour", Json()), x, y, size)) return;
        std::string name = jstr(t, "name");
        if (name.empty()) name = !item.empty() ? item : (!form.empty() ? form : jstr(t, "shape"));
        c.fill(x, y, size, size, pal::walnutD);
        c.textC(x + size / 2, y + size * 0.12f, firstLetters(name, 1), pal::gold, size * 0.7f);
    }
    void stationIcon(const std::string& id, const std::string& name, float x, float y, float size) {
        const Json& look = c.craftLook();
        if (look.contains("stations") && look["stations"].contains(id) && c.spriteAt(look["stations"][id].get<std::string>(), Json(), x, y, size)) return;
        c.fill(x, y, size, size, pal::walnutD);
        c.textC(x + size / 2, y + size * 0.15f, firstLetters(name, 2), pal::gold, size * 0.5f);
    }
    ui::Colour procColour(const std::string& id) const {
        const Json& look = c.craftLook();
        if (look.contains("processes") && look["processes"].contains(id) && look["processes"][id].is_array() && look["processes"][id].size() >= 2) {
            const std::string l = look["processes"][id][1].get<std::string>();
            (void)l;
        }
        return pal::gold;
    }
    // a ref onto the piece: into `slot`, or the first free slot of the process (the first, when none)
    void putRef(const std::string& ref, const std::string& slot = "") {
        Json& dr = draft();
        if (jstr(dr, "process").empty()) {
            const Json* t = thing(ref);
            const Json* pick = nullptr;
            for (const auto& p : jarr(ws(), "processes"))
                if (!pick && jflag(p, "available") && t && p.value("targets", Json::object()).contains(jstr(*t, "shape"))) pick = &p;
            for (const auto& p : jarr(ws(), "processes"))
                if (!pick && jflag(p, "available")) pick = &p;
            if (!pick) return;
            dr = {{"process", jstr(*pick, "id")}, {"slots", Json::object()}};
        }
        const Json* p = proc(jstr(dr, "process"));
        std::vector<std::string> ids;
        if (p)
            for (const auto& s : jarr(*p, "slots")) ids.push_back(jstr(s, "id"));
        if (ids.empty()) ids.push_back("work");
        if (!dr.contains("slots") || !dr["slots"].is_object()) dr["slots"] = Json::object();
        std::string s = slot;
        if (s.empty())
            for (const auto& id : ids)
                if (s.empty() && (!dr["slots"].contains(id) || dr["slots"][id].empty())) s = id;
        if (s.empty()) s = ids[0];
        Json& list = dr["slots"][s];
        if (!list.is_array()) list = Json::array();
        int used = 0;
        for (const auto& [k, v] : dr["slots"].items())
            for (const auto& r : v) used += r == ref ? 1 : 0;
        const Json* t = thing(ref);
        if (ref.rfind("made:", 0) == 0 && std::find(list.begin(), list.end(), Json(ref)) != list.end()) return;
        if (t && jnum(*t, "n") > 0 && used >= int(jnum(*t, "n"))) return;
        list.push_back(ref);
        load();
    }
    void pickProcess(const std::string& id) {
        Json& dr = draft();
        if (jstr(dr, "process") == id) return;
        Json all = Json::array();
        for (const auto& [k, v] : dr.value("slots", Json::object()).items())
            for (const auto& r : v) all.push_back(r);
        dr = {{"process", id}, {"slots", Json::object()}};
        const Json* p = proc(id);
        if (p && !all.empty()) dr["slots"][jarr(*p, "slots").empty() ? "work" : jstr(jarr(*p, "slots")[0], "id")] = all;
        load();
    }
    void attempt() {
        const Json d = draftOut();
        if (jstr(d, "process").empty()) return;
        w.data["draft"] = Json::object();
        act("craft_attempt", d);
    }

    // --- the bench ------------------------------------------------------------------------
    void bench(const ui::Rect& r) {
        const float U = c.u();
        const float lw = std::round(r.w * 0.25f), rw = std::round(r.w * 0.24f), gap = 10 * U;
        left({r.x, r.y, lw, r.h});
        ring({r.x + lw + gap, r.y, r.w - lw - rw - 2 * gap, r.h});
        right({r.right() - rw, r.y, rw, r.h});
    }
    void left(const ui::Rect& r) {
        const float U = c.u();
        ui::Ui& g = c.gui();
        ui::Input& in = c.input();
        const Json& s = ws();
        float y = r.y;
        c.text(r.x, y, L("ui2d.craft.stations"), pal::gold, 15 * U, true);
        y += 22 * U;
        const Json& st = jarr(s, "stations");
        const float boxH = std::max(80 * U, std::min(170 * U, 60 * U * float((st.size() + 2) / 3)));
        c.fill(r.x, y, r.w, boxH, pal::leatherD.withAlpha(0.85f));
        c.frame(r.x, y, r.w, boxH, pal::bronze.withAlpha(0.6f), U);
        if (st.empty()) {
            float yy = y + 6 * U;
            for (const auto& row : c.wrap(L("ui2d.craft.no_station"), r.w - 12 * U, 12 * U)) {
                c.text(r.x + 6 * U, yy, row, pal::dim, 12 * U);
                yy += 17 * U;
            }
        }
        int i = 0;
        const float cw = (r.w - 8 * U) / 3;
        bool overHot = false;
        for (const auto& sn : st) {
            const float sx = r.x + 4 * U + (i % 3) * cw, sy = y + 4 * U + (i / 3) * 60 * U;
            if (sy + 56 * U > y + boxH) break;
            stationIcon(jstr(sn, "id"), jstr(sn, "name"), sx + (cw - 36 * U) / 2, sy, 36 * U);
            c.textC(sx + cw / 2, sy + 38 * U, c.fit(jstr(sn, "name"), cw - 4 * U, 11 * U), pal::text, 11 * U);
            if (jnum(sn, "temp_max") > 0 && ui::Rect{sx, sy, cw, 56 * U}.contains(in.mouseX, in.mouseY)) overHot = true;
            ++i;
        }
        y += boxH + 8 * U;
        // the heat gauge
        const int hm = int(jnum(s, "heat_max"));
        c.text(r.x, y, L("ui2d.craft.heat"), pal::gold, 14 * U, true);
        if (hm > 0) {
            const float hx = r.x + 50 * U, cellW = std::min(22 * U, (r.w - 110 * U) / hm);
            const ui::Rect cells{hx, y, cellW * hm, 18 * U};
            for (int k = 1; k <= hm; ++k) {
                const ui::Rect cr{hx + (k - 1) * cellW, y, cellW - 2 * U, 18 * U};
                const double t = double(k) / hm;
                const ui::Colour on = t > 0.66 ? pal::hex("#f8e060") : t > 0.33 ? pal::hex("#f08020") : pal::hex("#a03a10");
                c.fill(cr.x, cr.y, cr.w, cr.h, k <= heat() ? on : pal::walnutD);
                c.frame(cr.x, cr.y, cr.w, cr.h, pal::out, 1);
                if (g.selectable(ui::widgetId("ws.heat", k), cr, false)) {
                    w.data["heat"] = heat() == k ? k - 1 : k;
                    load();
                }
            }
            c.text(cells.right() + 6 * U, y, std::to_string(heat()) + " / " + std::to_string(hm), pal::dim, 12 * U);
            if ((cells.contains(in.mouseX, in.mouseY) || overHot) && in.wheel != 0) {
                const int n = std::clamp(heat() + (in.wheel > 0 ? 1 : -1), 0, hm);
                in.wheel = 0;
                if (n != heat()) {
                    w.data["heat"] = n;
                    load();
                }
            }
        } else c.text(r.x + 50 * U, y + 2 * U, L("ui2d.craft.heat_none"), pal::dim, 12 * U);
        y += 28 * U;
        // the last outcome (only what happened since the window opened)
        if (s.contains("last") && s["last"].is_object() && jnum(s["last"], "seq") > jnum(w.data, "seq0")) {
            const Json& l = s["last"];
            std::vector<std::string> ls = {L("ui2d.craft.result") + ": " + jstr(l, "head") + (jstr(l, "what").empty() ? "" : ": " + jstr(l, "what"))};
            if (!jstr(l, "tier").empty()) ls.push_back(jstr(l, "tier"));
            std::string links;
            for (const auto& k : jarr(l, "links")) links += (links.empty() ? "" : " · ") + jstr(k, "name") + " " + std::to_string(pct(jnum(k, "q")));
            if (!links.empty()) ls.push_back(links);
            if (!jstr(l, "weak").empty()) ls.push_back(jstr(l, "weak"));
            std::string flaws;
            for (const auto& f : jarr(l, "flaws"))
                if (f.is_string()) flaws += (flaws.empty() ? "" : ", ") + f.get<std::string>();
            if (!flaws.empty()) ls.push_back(flaws);
            if (jflag(l, "breakthrough")) ls.push_back(T("hud.craft.breakthrough") + (jstr(l, "recipe_name").empty() ? "" : " · " + jstr(l, "recipe_name")));
            y += c.parchLines(r.x, y, r.w, ls, 12 * U) + 8 * U;
        }
        // the short book: mastered first; a mastered one is made from memory on a click
        c.text(r.x, y, T("hud.craft.book"), pal::gold, 14 * U, true);
        y += 22 * U;
        Json recipes = jarr(s, "recipes");
        if (recipes.empty()) {
            for (const auto& row : c.wrap(T("hud.craft.book_empty"), r.w, 12 * U)) {
                c.text(r.x, y, row, pal::dim, 12 * U);
                y += 17 * U;
            }
            return;
        }
        std::stable_sort(recipes.begin(), recipes.end(), [](const Json& a, const Json& b) {
            if (jflag(a, "master") != jflag(b, "master")) return jflag(a, "master");
            return jnum(a, "mastery") > jnum(b, "mastery");
        });
        int k = 0;
        for (const auto& rc : recipes) {
            if (k >= 8 || y > r.bottom() - 24 * U) break;
            const bool go = jflag(rc, "master") && jflag(rc, "can");
            const ui::Rect row{r.x, y, r.w, 24 * U};
            if (g.selectable(ui::widgetId("ws.short", k), row, false) && go) act("craft_fast", {{"recipe", jstr(rc, "id")}, {"count", 1}});
            thingIcon({{"item", jstr(rc, "item")}, {"form", jstr(rc, "form")}, {"shape", "lump"}, {"name", jstr(rc, "name")}}, r.x + 2 * U, y + 2 * U, 20 * U);
            const std::string tag = jflag(rc, "master") ? L("ui2d.craft.book_master")
                                                        : L("ui2d.craft.book_fragment", {{"known", jstr(rc, "known")}, {"total", jstr(rc, "total")}});
            c.text(r.x + 28 * U, y + 4 * U, c.fit(jstr(rc, "name"), r.w - 130 * U, 13 * U), go ? pal::gold : pal::text, 13 * U);
            c.text(r.right() - c.textW(tag, 11 * U), y + 5 * U, tag, pal::dim, 11 * U);
            y += 26 * U;
            ++k;
        }
    }
    void ring(const ui::Rect& r) {
        const float U = c.u();
        ui::Ui& g = c.gui();
        ui::Input& in = c.input();
        const Json& s = ws();
        c.fill(r.x, r.y, r.w, r.h, pal::leatherD.withAlpha(0.6f));
        c.frame(r.x, r.y, r.w, r.h, pal::bronze.withAlpha(0.5f), U);
        const float bagH = 120 * U, chipsH = 64 * U;
        const float ringH = r.h - bagH - chipsH;
        const float cx = r.x + r.w / 2, cy = r.y + ringH / 2, R = std::min(r.w, ringH) / 2 - 34 * U;
        for (int k = 0; k < 120; ++k) {
            if (k % 3 == 2) continue;
            const double a = k * 2 * 3.14159265 / 120;
            c.fill(cx + R * float(std::cos(a)) - U, cy + R * float(std::sin(a)) - U, 2.5f * U, 2.5f * U, pal::bronze);
        }
        const Json& procs = jarr(s, "processes");
        const std::string cur = jstr(draft(), "process");
        int i = 0;
        std::string tip;
        for (const auto& p : procs) {
            const double a = double(i) / std::max<std::size_t>(1, procs.size()) * 2 * 3.14159265 - 3.14159265 / 2;
            const float bs = 46 * U;
            const ui::Rect b{cx + R * float(std::cos(a)) - bs / 2, cy + R * float(std::sin(a)) - bs / 2, bs, bs};
            const bool avail = jflag(p, "available"), on = jstr(p, "id") == cur;
            if (g.selectable(ui::widgetId("ws.proc", i), b, on)) pickProcess(jstr(p, "id"));
            c.fill(b.x, b.y, b.w, b.h, on ? pal::leather : pal::walnutD);
            c.frame(b.x, b.y, b.w, b.h, on ? pal::gold : avail ? pal::bronze : pal::bronze.withAlpha(0.3f), on ? 2 * U : U);
            c.textC(b.x + b.w / 2, b.y + 12 * U, firstLetters(jstr(p, "name"), 3), avail ? pal::text : pal::dim, 15 * U);
            if (b.contains(in.mouseX, in.mouseY))
                tip = T("hud.craft.proc_tip", {{"name", jstr(p, "name")}, {"time", jstr(p, "time")}}) + (avail ? "" : " · " + jstr(p, "need"));
            ++i;
        }
        // the piece in the middle: the chosen process's slots
        const float ps = std::min(184 * U, R * 1.15f);
        const ui::Rect piece{cx - ps / 2, cy - ps / 2, ps, ps};
        c.fill(piece.x, piece.y, piece.w, piece.h, ui::Colour(0, 0, 0, 0.3f));
        c.frame(piece.x, piece.y, piece.w, piece.h, pal::bronze.withAlpha(0.7f), 2 * U);
        const Json* p = proc(cur);
        if (!p) {
            float yy = piece.y + ps / 2 - 20 * U;
            for (const auto& row : c.wrap(L("ui2d.craft.pick"), ps - 16 * U, 13 * U)) {
                c.textC(cx, yy, row, pal::dim, 13 * U);
                yy += 18 * U;
            }
        } else {
            c.textC(cx, piece.y + 6 * U, c.fit(jstr(*p, "name"), ps - 8 * U, 14 * U), pal::gold, 14 * U, true);
            Json slots = jarr(*p, "slots");
            if (slots.empty()) slots = Json::array({{{"id", "work"}, {"name", T("hud.craft.slot.work")}}});
            float yy = piece.y + 28 * U;
            const float sh = std::min(52 * U, (ps - 34 * U) / slots.size());
            int si = 0;
            for (const auto& sl : slots) {
                const std::string sid = jstr(sl, "id");
                Json& refs = draft()["slots"][sid];
                if (!refs.is_array()) refs = Json::array();
                c.text(piece.x + 6 * U, yy, c.fit(jstr(sl, "name"), ps - 12 * U, 11 * U), pal::dim, 11 * U);
                float tx = piece.x + 6 * U;
                int removeAt = -1;
                for (std::size_t k = 0; k < refs.size(); ++k) {
                    const ui::Rect tr{tx, yy + 15 * U, sh - 18 * U, sh - 18 * U};
                    if (tr.right() > piece.right() - 4 * U) break;
                    const Json* t = thing(refs[k].get<std::string>());
                    thingIcon(t ? *t : Json{{"shape", "lump"}, {"name", "?"}}, tr.x, tr.y, tr.w);
                    if (g.selectable(ui::widgetId("ws.tok", si * 100 + int(k)), tr, false)) removeAt = int(k);
                    tx += tr.w + 3 * U;
                }
                if (refs.empty()) c.text(piece.x + 6 * U, yy + 18 * U, L("ui2d.craft.piece_empty"), pal::dim.withAlpha(0.6f), 12 * U);
                if (removeAt >= 0) {
                    refs.erase(std::size_t(removeAt));
                    load();
                    return;
                }
                yy += sh;
                ++si;
            }
            if (piece.contains(in.mouseX, in.mouseY) && in.wheel != 0) {   // the wheel over the piece: its length
                draft()["len"] = std::clamp(int(jnum(draft(), "len")) + (in.wheel > 0 ? 1 : -1), 0, 12);
                in.wheel = 0;
                load();
                return;
            }
            if (piece.contains(in.mouseX, in.mouseY))
                tip = jnum(draft(), "len") > 0 ? T("hud.craft.len_tip", {{"n", jstr(draft(), "len")}}) : T("hud.craft.len_default");
        }
        // target shapes and ways
        float y = r.y + ringH + 4 * U;
        float x = r.x + 8 * U;
        if (p) {
            std::string first;
            for (const auto& [k, v] : draft().value("slots", Json::object()).items())
                if (first.empty() && v.is_array() && !v.empty()) first = v[0].get<std::string>();
            const Json* t = first.empty() ? nullptr : thing(first);
            const Json shapes = t ? p->value("targets", Json::object()).value(jstr(*t, "shape"), Json::array()) : Json::array();
            int k = 0;
            auto chip = [&](const std::string& label, bool on, int id) {
                const float bw = c.textW(label, 12 * U) + 18 * U;
                if (x + bw > r.right() - 8 * U) x = r.x + 8 * U, y += 28 * U;
                const bool hit = g.toggle(ui::widgetId("ws.chip", id), {x, y, bw, 24 * U}, label, on);
                x += bw + 4 * U;
                return hit;
            };
            if (!shapes.empty()) {
                c.text(x, y + 4 * U, L("ui2d.craft.target"), pal::gold, 12 * U);
                x += c.textW(L("ui2d.craft.target"), 12 * U) + 8 * U;
                for (const auto& sh : shapes)
                    if (chip(jstr(sh, "name"), jstr(draft(), "shape") == jstr(sh, "id"), k++)) {
                        draft()["shape"] = jstr(draft(), "shape") == jstr(sh, "id") ? "" : jstr(sh, "id");
                        load();
                        return;
                    }
            }
            if (!jarr(*p, "modes").empty()) {
                c.text(x, y + 4 * U, L("ui2d.craft.mode"), pal::gold, 12 * U);
                x += c.textW(L("ui2d.craft.mode"), 12 * U) + 8 * U;
                for (const auto& m : jarr(*p, "modes"))
                    if (chip(jstr(m, "name"), jstr(draft(), "mode") == jstr(m, "id"), 100 + k++)) {
                        draft()["mode"] = jstr(draft(), "mode") == jstr(m, "id") ? "" : jstr(m, "id");
                        load();
                        return;
                    }
            }
            if (jnum(draft(), "len") > 0) c.text(x, y + 4 * U, L("ui2d.craft.len") + " " + jstr(draft(), "len"), pal::dim, 12 * U);
        }
        // the bag
        const float by = r.bottom() - bagH;
        c.text(r.x + 8 * U, by, tip.empty() ? L("ui2d.craft.bag") : tip, tip.empty() ? pal::gold : pal::text, 13 * U, tip.empty());
        std::map<std::string, int> used;
        for (const auto& [k, v] : draft().value("slots", Json::object()).items())
            for (const auto& rr : v) ++used[rr.get<std::string>()];
        Json all = Json::array();
        for (const char* k : {"materials", "made"})
            for (const auto& t : jarr(s, k)) all.push_back(t);
        if (all.empty()) c.text(r.x + 8 * U, by + 26 * U, L("ui2d.craft.bag_empty"), pal::dim, 12 * U);
        const ui::Rect bag = g.beginScroll(ui::widgetId("ws.bag"), {r.x + 6 * U, by + 22 * U, r.w - 12 * U, bagH - 26 * U});
        const float ts = 44 * U;
        const int cols = std::max(1, int((bag.w + 4 * U) / (ts + 4 * U)));
        int k = 0;
        for (const auto& t : all) {
            const std::string ref = jstr(t, "ref");
            const int left = (jnum(t, "n") > 0 ? int(jnum(t, "n")) : 1) - used[ref];
            const ui::Rect tr{bag.x + (k % cols) * (ts + 4 * U), bag.y + (k / cols) * (ts + 4 * U), ts, ts};
            const bool hit = g.selectable(ui::widgetId("ws.bagtok", k), tr, false);
            c.fill(tr.x, tr.y, tr.w, tr.h, ref.rfind("made:", 0) == 0 ? pal::leather : pal::walnutD);
            c.frame(tr.x, tr.y, tr.w, tr.h, pal::bronze.withAlpha(left > 0 ? 0.8f : 0.25f), U);
            thingIcon(t, tr.x + 6 * U, tr.y + 4 * U, ts - 12 * U);
            if (jnum(t, "n") > 1) c.text(tr.right() - c.textW(std::to_string(left), 11 * U) - 3 * U, tr.y + 1 * U, std::to_string(left), pal::text, 11 * U, true);
            if (t.contains("know") && t["know"].is_number()) c.fill(tr.x + 2 * U, tr.bottom() - 4 * U, (tr.w - 4 * U) * float(std::clamp(jnum(t, "know"), 0.0, 1.0)), 2 * U, pal::gold);
            if (left <= 0) c.fill(tr.x, tr.y, tr.w, tr.h, ui::Colour(0, 0, 0, 0.5f));
            if (tr.contains(in.mouseX, in.mouseY)) {
                std::string tt = jstr(t, "name");
                if (t.contains("know") && t["know"].is_number()) tt += " · " + L("ui2d.craft.know", {{"n", std::to_string(pct(jnum(t, "know")))}});
                if (!jstr(t, "tier").empty()) tt += " · " + jstr(t, "tier");
                c.fill(r.x + 8 * U, by - 2 * U, r.w - 16 * U, 20 * U, pal::leatherD);
                c.text(r.x + 8 * U, by, c.fit(tt, r.w - 16 * U, 13 * U), pal::text, 13 * U);
            }
            if (hit && left > 0) {
                g.endScroll(tr.bottom());
                putRef(ref);
                return;
            }
            ++k;
        }
        g.endScroll(bag.y + ((k + cols - 1) / cols) * (ts + 4 * U));
    }
    void right(const ui::Rect& r) {
        const float U = c.u();
        ui::Ui& g = c.gui();
        ui::Input& in = c.input();
        const Json f = ws().value("forecast", Json());
        std::vector<std::string> ls;
        bool can = false;
        if (!f.is_object()) ls.push_back(T("hud.craft.sheet_none"));
        else if (!jflag(f, "can")) ls.push_back(jstr(f, "why").empty() ? "…" : jstr(f, "why"));
        else {
            can = true;
            const bool sure = jnum(f, "certainty") >= 0.5;
            ls.push_back(L("ui2d.craft.q") + ": " + (jstr(f, "likely").empty() ? T("hud.craft.unknown_form") : jstr(f, "likely")) + ", " + jstr(f, "tier"));
            ls.push_back(L("ui2d.craft.ruin") + ": " + (sure ? std::to_string(pct(jnum(f, "ruin"))) + "%" : "~" + std::to_string(int(std::lround(pct(jnum(f, "ruin")) / 10.0) * 10)) + "%"));
            ls.push_back(L("ui2d.craft.flaw") + ": " + (sure ? std::to_string(pct(jnum(f, "flaw"))) + "%" : "?"));
            ls.push_back(L("ui2d.craft.time") + ": " + L("ui2d.craft.minutes", {{"n", std::to_string(int(jnum(f, "time") * 10))}}));
            ls.push_back(L(sure ? "ui2d.craft.sure" : "ui2d.craft.vague"));
        }
        c.text(r.x, r.y, L("ui2d.craft.forecast"), pal::gold, 15 * U, true);
        float y = r.y + 24 * U;
        y += c.parchLines(r.x, y, r.w, ls, 13 * U) + 6 * U;
        if (can) {   // the quality band
            const ui::Rect q{r.x, y, r.w, 10 * U};
            c.fill(q.x, q.y, q.w, q.h, pal::leatherD);
            const double lo = std::clamp(jnum(f, "q") - jnum(f, "spread"), 0.0, 1.0), wd = std::max(0.02, 2 * jnum(f, "spread"));
            c.fill(q.x + q.w * float(lo), q.y, q.w * float(std::min(wd, 1 - lo)), q.h, pal::gold);
            c.frame(q.x, q.y, q.w, q.h, pal::out, 1);
            y += 18 * U;
        }
        // clear | make (held)
        const float bh = 44 * U;
        if (g.button(ui::widgetId("ws.clear"), {r.x, y, 44 * U, bh}, "X")) {
            w.data["draft"] = Json::object();
            load();
        }
        const ui::Rect mk{r.x + 50 * U, y, r.w - 50 * U, bh};
        const bool keyE = SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_E] && !g.typing();
        if (can && mk.contains(in.mouseX, in.mouseY) && in.pressed) w.data["holdAt"] = now();
        if (can && keyE && jnum(w.data, "holdAt") <= 0) w.data["holdAt"] = now();
        const bool holding = jnum(w.data, "holdAt") > 0 && (keyE || (in.down && mk.contains(in.mouseX, in.mouseY)));
        if (!holding) w.data["holdAt"] = 0.0;
        const double k = holding ? std::min(1.0, (now() - jnum(w.data, "holdAt")) / kHoldMs) : 0.0;
        c.fill(mk.x, mk.y, mk.w, mk.h, can ? pal::leather : pal::walnutD);
        c.fill(mk.x, mk.y, mk.w * float(k), mk.h, pal::gold.withAlpha(0.6f));
        c.frame(mk.x, mk.y, mk.w, mk.h, can ? pal::gold : pal::bronze.withAlpha(0.4f), 2 * U);
        c.textC(mk.x + mk.w / 2, mk.y + (mk.h - c.textH(15 * U)) / 2, L("ui2d.craft.hold"), can ? pal::text : pal::dim, 15 * U);
        g.claim(mk);
        if (holding && k >= 1) {
            w.data["holdAt"] = -1.0;   // not again until let go
            attempt();
        }
        if (!keyE && !in.down && jnum(w.data, "holdAt") < 0) w.data["holdAt"] = 0.0;
    }

    // --- the book, things, the helper ---------------------------------------------------------
    void book(const ui::Rect& r) {
        const float U = c.u();
        ui::Ui& g = c.gui();
        const Json& recipes = jarr(ws(), "recipes");
        if (recipes.empty()) {
            c.parchLines(r.x, r.y, r.w, {T("hud.craft.book_empty")}, 14 * U);
            return;
        }
        const ui::Rect in = g.beginScroll(ui::widgetId("ws.book"), r);
        float y = in.y;
        int k = 0;
        for (const auto& rc : recipes) {
            const ui::Rect row{in.x, y, in.w, 64 * U};
            c.parchBox(row);
            thingIcon({{"item", jstr(rc, "item")}, {"form", jstr(rc, "form")}, {"shape", "lump"}, {"name", jstr(rc, "name")}}, row.x + 8 * U, row.y + 12 * U, 40 * U);
            const std::string tag = jflag(rc, "master") ? L("ui2d.craft.book_master")
                                                        : L("ui2d.craft.book_fragment", {{"known", jstr(rc, "known")}, {"total", jstr(rc, "total")}});
            c.text(row.x + 58 * U, row.y + 6 * U, jstr(rc, "name") + "  (" + tag + ")", pal::ink, 15 * U);
            std::string steps;
            int n = 1;
            for (const auto& st : jarr(rc, "steps")) steps += (steps.empty() ? "" : " > ") + std::to_string(n++) + ". " + (jflag(st, "known") ? jstr(st, "name") : "?");
            c.text(row.x + 58 * U, row.y + 26 * U, c.fit(steps, row.w - 300 * U, 12 * U), pal::ink.withAlpha(0.8f), 12 * U);
            std::string sub = L("ui2d.craft.mastery", {{"n", std::to_string(pct(jnum(rc, "mastery")))}}) + " · " + T("hud.craft.successes", {{"n", jstr(rc, "successes")}});
            std::string miss;
            for (const auto& m : jarr(rc, "missing"))
                if (m.is_string()) miss += (miss.empty() ? "" : ", ") + m.get<std::string>();
            if (!miss.empty()) sub += " · " + L("ui2d.craft.missing", {{"list", miss}});
            c.text(row.x + 58 * U, row.y + 44 * U, c.fit(sub, row.w - 300 * U, 12 * U), miss.empty() ? pal::ink.withAlpha(0.7f) : pal::bad, 12 * U);
            const bool go = jflag(rc, "master") && jflag(rc, "can");
            if (g.button(ui::widgetId("ws.fast", k), {row.right() - 220 * U, row.y + 14 * U, 210 * U, 36 * U}, L("ui2d.craft.from_memory"), go)) {
                act("craft_fast", {{"recipe", jstr(rc, "id")}, {"count", 1}});
                w.tab = "bench";
            }
            y += 70 * U;
            ++k;
        }
        g.endScroll(y);
    }
    void things(const ui::Rect& r) {
        const float U = c.u();
        ui::Ui& g = c.gui();
        const Json& made = jarr(ws(), "made");
        if (made.empty()) {
            c.parchLines(r.x, r.y, r.w, {T("hud.craft.things_empty")}, 14 * U);
            return;
        }
        const ui::Rect in = g.beginScroll(ui::widgetId("ws.things"), r);
        float y = in.y;
        int k = 0;
        for (const auto& t : made) {
            const ui::Rect row{in.x, y, in.w, 60 * U};
            c.parchBox(row);
            thingIcon(t, row.x + 8 * U, row.y + 10 * U, 40 * U);
            c.text(row.x + 58 * U, row.y + 6 * U, jstr(t, "name") + "  (" + jstr(t, "tier") + ")", pal::ink, 15 * U);
            std::vector<std::string> notes;
            if (jnum(t, "wear") > 0.05) notes.push_back(T("hud.craft.worn", {{"n", std::to_string(pct(jnum(t, "wear")))}}));
            std::string flaws;
            for (const auto& f : jarr(t, "flaws"))
                if (f.is_string()) flaws += (flaws.empty() ? "" : ", ") + f.get<std::string>();
            if (!flaws.empty()) notes.push_back(flaws);
            if (jflag(t, "hidden_flaws")) notes.push_back(L("ui2d.craft.hidden"));
            std::string nt;
            for (const auto& n : notes) nt += (nt.empty() ? "" : " · ") + n;
            c.text(row.x + 58 * U, row.y + 26 * U, c.fit(nt, row.w - 420 * U, 12 * U), flaws.empty() ? pal::ink.withAlpha(0.7f) : pal::bad, 12 * U);
            c.gauge({row.x + 58 * U, row.y + 44 * U, std::min(240 * U, row.w - 420 * U), 8 * U}, 1 - jnum(t, "wear"), pal::good);
            const float bw = 120 * U;
            float bx = row.right() - 3 * (bw + 6 * U);
            if (g.button(ui::widgetId("ws.inspect", k), {bx, row.y + 12 * U, bw, 34 * U}, L("ui2d.craft.inspect"))) act("craft_inspect", {{"item", t["i"]}});
            bx += bw + 6 * U;
            const bool rep = jnum(t, "wear") > 0.05 || !jarr(t, "flaws").empty();
            if (g.button(ui::widgetId("ws.repair", k), {bx, row.y + 12 * U, bw, 34 * U}, L("ui2d.craft.repair"), rep)) act("craft_repair", {{"item", t["i"]}});
            bx += bw + 6 * U;
            if (g.button(ui::widgetId("ws.tobench", k), {bx, row.y + 12 * U, bw, 34 * U}, c.fit(L("ui2d.craft.to_bench"), bw - 8 * U, 13 * U))) {
                w.tab = "bench";
                g.endScroll(y + 66 * U);
                putRef(jstr(t, "ref"));
                return;
            }
            y += 66 * U;
            ++k;
        }
        g.endScroll(y);
    }
    void helper(const ui::Rect& r) {
        const float U = c.u();
        ui::Ui& g = c.gui();
        c.text(r.x, r.y, L("ui2d.craft.wish"), pal::gold, 16 * U, true);
        float x = r.x, y = r.y + 26 * U;
        int k = 0;
        for (const auto& wsh : jarr(ws(), "wishes")) {
            const float bw = c.textW(jstr(wsh, "name"), 13 * U) + 24 * U;
            if (g.toggle(ui::widgetId("ws.wish", k++), {x, y, bw, 30 * U}, jstr(wsh, "name"), jstr(w.data, "wish") == jstr(wsh, "id"))) {
                w.data["wish"] = jstr(w.data, "wish") == jstr(wsh, "id") ? "" : jstr(wsh, "id");
                load();
                return;
            }
            x += bw + 4 * U;
        }
        y += 40 * U;
        const ui::Rect in = g.beginScroll(ui::widgetId("ws.cands"), {r.x, y, r.w, r.bottom() - y});
        float yy = in.y;
        k = 0;
        for (const auto& cd : jarr(ws(), "candidates")) {
            const ui::Rect row{in.x, yy, in.w, 84 * U};
            c.parchBox(row);
            thingIcon(cd, row.x + 8 * U, row.y + 14 * U, 48 * U);
            const std::string tag = jflag(cd, "finished") ? L("ui2d.craft.done") : L("ui2d.craft.fit", {{"n", std::to_string(pct(jnum(cd, "fit")))}});
            c.text(row.x + 66 * U, row.y + 6 * U, jstr(cd, "name") + "  (" + tag + ")", pal::ink, 15 * U);
            c.gauge({row.x + 66 * U, row.y + 28 * U, std::min(220 * U, row.w - 340 * U), 8 * U}, jnum(cd, "fit"), pal::gold);
            // the chain along the stations
            float sx = row.x + 66 * U;
            int si = 0;
            const int next = int(jnum(cd, "next", -1));
            for (const auto& st : jarr(cd, "steps")) {
                std::string label = std::to_string(si + 1) + ". " + jstr(st, "name");
                if (!jstr(st, "station_name").empty()) label += " (" + jstr(st, "station_name") + ")";
                const float lw = c.textW(label, 12 * U) + 14 * U;
                if (sx + lw > row.right() - 270 * U) break;
                const ui::Rect sr{sx, row.y + 44 * U, lw, 24 * U};
                const bool here = jflag(st, "here");
                c.fill(sr.x, sr.y, sr.w, sr.h, si == next ? pal::gold.withAlpha(0.35f) : pal::parchD);
                c.frame(sr.x, sr.y, sr.w, sr.h, here ? pal::ink : pal::bad, 1);
                c.text(sr.x + 7 * U, sr.y + 4 * U, label, pal::ink, 12 * U);
                if (!here && st.contains("x") && st["x"].is_number() && g.selectable(ui::widgetId("ws.go", k * 100 + si), sr, false)) {
                    g.endScroll(yy + 90 * U);
                    w.closing = true;
                    c.walkTo(jnum(st, "x"), jnum(st, "y"));
                    return;
                }
                sx += lw + 4 * U;
                ++si;
            }
            std::string miss;
            for (const auto& m : jarr(cd, "missing"))
                if (m.is_string()) miss += (miss.empty() ? "" : ", ") + m.get<std::string>();
            if (!miss.empty()) c.text(row.x + 66 * U, row.y + 70 * U - 2 * U, c.fit(T("hud.craft.missing", {{"list", miss}}), row.w - 340 * U, 11 * U), pal::bad, 11 * U);
            const bool has = cd.contains("draft") && cd["draft"].is_object();
            if (g.button(ui::widgetId("ws.lay", k), {row.right() - 250 * U, row.y + 24 * U, 240 * U, 36 * U},
                         c.fit(has ? L("ui2d.craft.lay") : L("ui2d.craft.no_next"), 232 * U, 13 * U), has)) {
                w.data["draft"] = cd["draft"];
                if (cd["draft"].contains("heat")) w.data["heat"] = std::min(int(jnum(ws(), "heat_max")), int(jnum(cd["draft"], "heat")));
                w.tab = "bench";
                g.endScroll(yy + 90 * U);
                load();
                return;
            }
            yy += 90 * U;
            ++k;
        }
        g.endScroll(yy);
    }
};

} // namespace

void openWorkshop(Ctx& c, Screens& s) {
    if (c.meId() < 0) {
        c.toast(c.L("ui2d.craft.nobody"));
        return;
    }
    auto w = std::make_unique<Win>();
    w->id = "workshop";
    w->title = c.L("ui2d.screen.workshop");
    for (const char* t : {"bench", "book", "things", "helper"}) w->tabs.push_back({t, c.L(std::string("ui2d.craft.tab.") + t)});
    w->tab = "bench";
    w->width = 0.92f;
    w->height = 0.9f;
    w->data = {{"draft", Json::object()}, {"wish", ""}, {"heat", -1}, {"tx", Json::object()}, {"holdAt", 0.0}};
    Craft{c, *w}.load(SDL_getenv("ALTERA_2D_KIT") ? Json{{"kit", true}} : Json::object());
    w->render = [](Ctx& c, Win& w, const ui::Rect& body) {
        Craft cr{c, w};
        if (!cr.ws().is_object()) {
            c.parchLines(body.x, body.y, body.w, {c.L("ui2d.craft.nobody")}, 14 * c.u());
            return;
        }
        if (w.tab == "book") cr.book(body);
        else if (w.tab == "things") cr.things(body);
        else if (w.tab == "helper") cr.helper(body);
        else cr.bench(body);
    };
    w->onPoll = [](Ctx& c, Win& w) {
        if (now() - jnum(w.data, "at") > 1500 && jnum(w.data, "holdAt") == 0) Craft{c, w}.load();
    };
    w->onKey = [](Ctx&, Win&, SDL_Scancode sc) { return sc == SDL_SCANCODE_E; };   // E is the held Make
    s.push(std::move(w));
}

} // namespace game2d
