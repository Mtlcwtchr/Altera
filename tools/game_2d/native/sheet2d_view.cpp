// The character sheet (C) in the 2D window (port of web/character.js): a big
// window, < portrait > to page through the people (also , and .), five pages:
// Overview, Relations, Things, Skills & traits, Chronicle.
// Data: /api/sheet?id= (src/client2d/sheet2d.cpp), refreshed while open.
#include <algorithm>
#include <cmath>
#include <regex>

#include "native/ctx2d.hpp"

namespace game2d {

namespace {

double now() { return double(SDL_GetTicks()); }
int pct(double v) { return int(std::lround(std::clamp(v, 0.0, 1.0) * 100)); }

// the people one pages through: the humans, by household then age
std::vector<long long> ring(Ctx& c) {
    std::vector<const Json*> hs;
    for (const Json& p : c.peopleList())
        if (jstr(p, "sp").empty()) hs.push_back(&p);
    std::stable_sort(hs.begin(), hs.end(), [](const Json* a, const Json* b) {
        const std::string fa = jstr(*a, "fam"), fb = jstr(*b, "fam");
        if (fa != fb) return fa < fb;
        return jnum(*a, "age") > jnum(*b, "age");
    });
    std::vector<long long> out;
    for (const Json* p : hs) out.push_back(jid(*p, "id"));
    return out;
}

void load(Ctx& c, Win& w) {
    const long long id = w.data["id"].get<long long>();
    w.data["at"] = now();
    const Json d = c.get("/api/sheet?id=" + std::to_string(id));
    if (!d.is_object()) return;
    // a need's trend: compared with the last read
    Json& prev = w.data["prevNeeds"];
    Json& trend = w.data["trend"];
    for (const auto& n : jarr(d.value("overview", Json::object()), "needs")) {
        const std::string nid = jstr(n, "id");
        const double v = jnum(n, "v");
        if (prev.contains(nid) && std::abs(v - prev[nid].get<double>()) >= 0.005) trend[nid] = v > prev[nid].get<double>() ? 1 : -1;
        prev[nid] = v;
    }
    w.data["d"] = d;
}

void setPerson(Ctx& c, Win& w, long long id) {
    if (w.data.value("id", -1LL) == id) return;
    w.data["id"] = id;
    w.data["d"] = Json();
    w.data["prevNeeds"] = Json::object();
    w.data["trend"] = Json::object();
    w.data["relSel"] = -1;
    w.data["thing"] = "";
    load(c, w);
}

void step(Ctx& c, Win& w, int dir) {
    const auto r = ring(c);
    if (r.empty()) return;
    const auto it = std::find(r.begin(), r.end(), w.data["id"].get<long long>());
    const int i = it == r.end() ? 0 : int(it - r.begin());
    setPerson(c, w, r[std::size_t((i + dir + int(r.size())) % int(r.size()))]);
}

// A column that flows down a scrolled box: headings, bars, lines, chips.
struct Flow {
    Ctx& c;
    float x, y, w;
    float U;
    void head(const std::string& h) {
        y += 4 * U;
        c.text(x, y, h, pal::gold, 16 * U, true);
        c.fill(x, y + 19 * U, w, c.pixel(), pal::bronze.withAlpha(0.6f));
        y += 26 * U;
    }
    // LR-137: an icon, the name, ten pips, the number on the right
    void stat(const std::string& icon, const std::string& name, double v, const ui::Colour& col, const std::string& right) {
        const float is = c.iconSize(), rowH = std::max(is, 20 * U), p = c.pixel();
        const float pw = 10 * 4 * p + 2 * p, nw = 34 * U;
        if (!icon.empty()) c.icon(icon, x, y + (rowH - is) / 2);
        const float tx = x + is + 6 * U;
        c.text(tx, y + (rowH - c.textH(16 * U)) / 2, c.fit(name, w - (tx - x) - pw - nw - 6 * U, 16 * U), pal::text, 16 * U);
        c.pips(x + w - nw - pw, y + (rowH - 5 * p) / 2, 10, v, col);
        c.text(x + w - c.textW(right, 16 * U), y + (rowH - c.textH(16 * U)) / 2, right, pal::dim, 16 * U);
        y += rowH + 4 * U;
    }
    void bar(const std::string& name, double v, const ui::Colour& col, const std::string& right, bool signedBar = false) {
        const float px = 13 * U, lw = std::min(140 * U, w * 0.42f);
        c.text(x, y, c.fit(name, lw - 4 * U, px), pal::text, px);
        const ui::Rect r{x + lw, y + 3 * U, w - lw - 46 * U, 10 * U};
        if (signedBar) {
            c.fill(r.x, r.y, r.w, r.h, pal::leatherD);
            const float mid = r.x + r.w / 2, len = float(std::min(1.0, std::abs(v))) * r.w / 2;
            c.fill(v >= 0 ? mid : mid - len, r.y, len, r.h, v >= 0 ? pal::good : pal::bad);
            c.frame(r.x, r.y, r.w, r.h, pal::out, 1);
        } else c.gauge(r, v, col);
        c.text(x + w - c.textW(right, px), y, right, pal::dim, px);
        y += 19 * U;
    }
    void lines(const std::vector<std::string>& ls, float px = 13) { y += c.parchLines(x, y, w, ls, px * U) + 6 * U; }
    void chips(const std::vector<std::string>& items) {
        if (!items.empty()) y += c.chips(x, y, w, items) + 6 * U;
    }
};

// an inset box with a scrolled flow inside; draw fills it
template <class F>
void box(Ctx& c, const ui::Rect& r, const char* id, int index, F&& draw) {
    const float U = c.u();
    c.well(r);
    ui::Ui& g = c.gui();
    const ui::Rect in = g.beginScroll(ui::widgetId(id, index), r.inset(8 * U));
    Flow f{c, in.x, in.y, in.w, U};
    draw(f);
    g.endScroll(f.y);
}

std::string S(Ctx& c, const std::string& k, const std::map<std::string, std::string>& a = {}) { return c.L("ui2d.sheet." + k, a); }

std::string memLine(Ctx& c, const Json& m) {
    std::vector<std::string> tail;
    if (jstr(m, "src") != "seen" && !jstr(m, "src").empty()) tail.push_back(S(c, "src." + jstr(m, "src")));
    if (jnum(m, "conf", 1) < 0.99) tail.push_back(S(c, "believes", {{"n", std::to_string(pct(jnum(m, "conf", 1)))}}));
    const double v = jnum(m, "v");
    std::string s = std::string(v > 0.05 ? "+ " : v < -0.05 ? "- " : "· ") + c.L("ui2d.day", {{"n", std::to_string(int(jnum(m, "day")) + 1)}}) + " " +
                    jstr(m, "t") + "  " + jstr(m, "text");
    if (!tail.empty()) {
        s += " (";
        for (std::size_t i = 0; i < tail.size(); ++i) s += (i ? ", " : "") + tail[i];
        s += ")";
    }
    return s;
}

// --- the pages ------------------------------------------------------------------------------
// LR-137: the character page in the spirit of WoW's: the paper doll with the worn things
// on both sides and the hands below, then attributes and condition, then skills, traits
// and what the person is up to. Needs and thoughts are in the column next to the map.
constexpr float kPageH = 420;   // the page's height (in U): the window hugs it

// a thing's picture: craft2d.json items, resources.json items, else the first letter
void thingIcon(Ctx& c, const std::string& item, const std::string& name, float x, float y, float size) {
    for (const Json* look : {&c.craftLook(), &c.resources()})
        if (look->is_object() && look->contains("items") && (*look)["items"].contains(item)) {
            const Json& r = (*look)["items"][item];
            if (r.is_object() && c.spriteAt(jstr(r, "sprite"), r.value("recolour", Json()), x, y, size)) return;
        }
    if (name.empty()) return;
    std::size_t n = 1;
    while (n < name.size() && (static_cast<unsigned char>(name[n]) & 0xC0) == 0x80) ++n;
    c.textC(x + size / 2, y + (size - c.textH(16 * c.u())) / 2, name.substr(0, n), pal::gold, 16 * c.u());
}

// a carried thing for a hand: the first whose id matches
const Json* inHand(const Json& carried, const char* re) {
    const std::regex rx(re);
    for (const auto& x : carried)
        if (std::regex_search(jstr(x, "id"), rx)) return &x;
    return nullptr;
}

void pageOverview(Ctx& c, Win& w, const Json& d, const ui::Rect& r) {
    (void)w;
    const float U = c.u(), p = c.pixel(), is = c.iconSize();
    const Json o = d.value("overview", Json::object());
    const Json th = d.value("things", Json::object());
    const Json sk = d.value("skills", Json::object());
    const Json& hud = c.hud();

    // --- the doll ---------------------------------------------------------------------------
    const float slotW = 170 * U, slotH = 64 * U, gap = 10 * U, figW = 150 * U;
    const float dw = 2 * slotW + figW + 4 * gap;
    const ui::Rect dr{r.x, r.y, dw, r.h};
    c.well(dr);
    c.text(dr.x + 10 * U, dr.y + 8 * U, S(c, "worn"), pal::gold, 16 * U);
    static const struct {
        const char* slot;
        int col, row;
    } PLACES[] = {{"head", 0, 0}, {"shoulder", 0, 1}, {"back", 0, 2}, {"torso", 0, 3}, {"hands", 1, 0}, {"waist", 1, 1}, {"legs", 1, 2}, {"feet", 1, 3}};
    const float top = dr.y + 34 * U;
    for (const auto& pl : PLACES) {
        std::vector<const Json*> items;
        for (const auto& x : jarr(th, "worn"))
            if (jstr(x, "slot") == pl.slot) items.push_back(&x);
        std::string slotName = pl.slot;
        for (const auto& sd : jarr(th, "slots"))
            if (jstr(sd, "id") == pl.slot) slotName = jstr(sd, "name");
        const bool left = pl.col == 0;
        const ui::Rect sr{left ? dr.x + gap : dr.right() - gap - slotW, top + pl.row * (slotH + gap), slotW, slotH};
        c.plate(sr, !items.empty());
        const float ws = slotH - 4 * p;
        const ui::Rect wr{left ? sr.x + 2 * p : sr.right() - 2 * p - ws, sr.y + 2 * p, ws, ws};
        c.well(wr);
        // the outermost layer shows; the rest is counted
        const Json* shown = items.empty() ? nullptr : items.back();
        if (shown) thingIcon(c, jstr(*shown, "id"), jstr(*shown, "name"), wr.x + 2 * p, wr.y + 2 * p, ws - 4 * p);
        const float tx = left ? wr.right() + 6 * U : sr.x + 6 * U, tw = sr.w - ws - 2 * p - 12 * U;
        auto line = [&](float y, const std::string& s, const ui::Colour& col) {
            const std::string f = c.fit(s, tw, 16 * U);
            c.text(left ? tx : wr.x - 6 * U - c.textW(f, 16 * U), y, f, col, 16 * U);
        };
        line(sr.y + slotH / 2 - 20 * U, slotName, pal::dim);
        std::string what = shown ? jstr(*shown, "name") : S(c, "layer_empty");
        if (items.size() > 1) what += " +" + std::to_string(items.size() - 1);
        line(sr.y + slotH / 2 + 2 * U, what, shown ? pal::text : pal::dim.withAlpha(0.6f));
    }
    // the figure between the columns
    const float fx = dr.x + slotW + 2 * gap, fy = top, fh = 4 * slotH + 3 * gap;
    c.well({fx, fy, figW, fh});
    const float fig = std::min(figW - 8 * p, fh - 8 * p);
    c.face(jid(d, "id"), jstr(d, "name"), fx + (figW - fig) / 2, fy + (fh - fig) / 2, fig);
    // the hands below: a weapon and a tool from what is carried
    const Json& carried = jarr(th, "carried");
    const Json* weapon = inHand(carried, "sword|knife|blade|dagger|spear|bow|mace|club");
    const Json* tool = inHand(carried, "hammer|axe|pick|saw|sickle|scythe|hoe|tongs|chisel|rod|net");
    const float hy = top + fh + gap;
    auto hand = [&](float x, const char* lbl, const Json* t) {
        const ui::Rect hr{x, hy, slotW, slotH};
        c.plate(hr, t != nullptr);
        const float ws = slotH - 4 * p;
        const ui::Rect wr{hr.x + 2 * p, hr.y + 2 * p, ws, ws};
        c.well(wr);
        if (t) thingIcon(c, jstr(*t, "id"), jstr(*t, "name"), wr.x + 2 * p, wr.y + 2 * p, ws - 4 * p);
        c.text(wr.right() + 6 * U, hr.y + slotH / 2 - 20 * U, S(c, lbl), pal::dim, 16 * U);
        c.text(wr.right() + 6 * U, hr.y + slotH / 2 + 2 * U, c.fit(t ? jstr(*t, "name") : S(c, "empty_hand"), hr.right() - wr.right() - 12 * U, 16 * U),
               t ? pal::text : pal::dim.withAlpha(0.6f), 16 * U);
    };
    hand(dr.x + gap, "weapon", weapon);
    hand(dr.right() - gap - slotW, "tool", tool);
    // the load and the purse under the figure
    {
        const double kg = jnum(th, "kg"), maxKg = jnum(th, "maxKg"), ld = maxKg > 0 ? kg / maxKg : 0;
        char a[16], b[16];
        std::snprintf(a, sizeof a, "%.1f", kg);
        std::snprintf(b, sizeof b, "%.0f", maxKg);
        const float y = hy + 4 * U;
        c.icon("hud_bag", fx, y);
        c.pips(fx + is + 4 * U, y + (is - 5 * p) / 2, 10, ld, ld > 0.8 ? pal::bad : ld > 0.45 ? pal::mid : pal::good);
        const std::string kgs = std::string(a) + "/" + b;
        c.textC(fx + figW / 2, y + is + 4 * U, kgs, pal::dim, 16 * U);
    }

    // --- attributes and condition ----------------------------------------------------------
    const float cx = dr.right() + 10 * U, cw = (r.right() - cx - 10 * U) / 2;
    box(c, {cx, r.y, cw, r.h}, "cs.ov", 0, [&](Flow& f) {
        f.head(S(c, "attrs_title"));
        std::map<std::string, std::string> attrIcon;
        for (const auto& a : jarr(hud, "attrs")) attrIcon[jstr(a, "id")] = jstr(a, "icon");
        for (const auto& a : jarr(sk, "attrs")) {
            const double v = jnum(a, "v");
            const int lvl = std::clamp(int(std::lround(v * 10)), 1, 10);
            f.stat(attrIcon[jstr(a, "id")], jstr(a, "name"), lvl / 10.0, pal::gold, std::to_string(lvl));
        }
        f.head(S(c, "state"));
        const Json& wounds = jarr(o, "wounds");
        const double hp = jnum(o, "hp", 1), st = 1 - jnum(o, "fatigue"), bl = jnum(o, "blood", 1), mood = jnum(o, "mood");
        const Json vit = hud.value("vitals", Json::object());
        f.stat(jstr(vit, "health"), S(c, "health"), hp, pal::bad, std::to_string(pct(hp)));
        f.stat(jstr(vit, "strength"), S(c, "strength"), st, pal::mid, std::to_string(pct(st)));
        f.stat(jstr(vit, "blood"), S(c, "blood"), bl, pal::hex("#a0282a"), std::to_string(pct(bl)));
        f.stat(jstr(vit, mood < -0.1 ? "mood_low" : "mood"), S(c, "mood"), (mood + 1) / 2, mood >= 0 ? pal::good : pal::bad,
               (mood >= 0 ? "+" : "") + std::to_string(int(std::lround(mood * 100))));
        if (!wounds.empty() || !jarr(o, "injuries").empty()) {
            f.head(S(c, "wounds"));
            std::vector<std::string> ls;
            for (const auto& x : wounds) {
                std::string s = jstr(x, "sev") + ": " + jstr(x, "zone");
                if (jflag(x, "bleeding")) s += ", " + S(c, "bleeding");
                if (jflag(x, "infected")) s += ", " + S(c, "infected");
                s += " (" + S(c, "heals", {{"n", jstr(x, "days")}}) + ")";
                ls.push_back(s);
            }
            for (const auto& x : jarr(o, "injuries")) ls.push_back(jstr(x, "kind") + " (" + jstr(x, "zone") + ")");
            f.lines(ls, 16);
        }
    });

    // --- skills, traits, what now ------------------------------------------------------------
    box(c, {cx + cw + 10 * U, r.y, cw, r.h}, "cs.ov", 1, [&](Flow& f) {
        f.head(S(c, "skills"));
        const Json& skills = jarr(sk, "skills");
        if (skills.empty()) f.lines({S(c, "none")}, 16);
        int n = 0;
        for (const auto& k : skills) {
            if (n++ >= 8) break;
            f.stat("hud_skill", jstr(k, "name"), jnum(k, "v"), pal::bronzeL, std::to_string(pct(jnum(k, "v"))));
        }
        if (skills.size() > 8) {
            c.text(f.x, f.y, S(c, "more", {{"n", std::to_string(skills.size() - 8)}}) + "  (" + S(c, "tab.skills") + ")", pal::dim, 16 * U);
            f.y += 20 * U;
        }
        if (!jarr(sk, "traits").empty()) {
            f.head(S(c, "traits"));
            std::vector<std::string> ts;
            for (const auto& t : jarr(sk, "traits")) ts.push_back(jstr(t, "name"));
            f.chips(ts);
        }
        f.head(S(c, "doing"));
        std::vector<std::string> doing = {jstr(o, "act").empty() ? c.L("ui2d.idle") : jstr(o, "act")};
        if (!jstr(o, "goal").empty()) doing.push_back(S(c, "wants") + ": " + jstr(o, "goal"));
        int i = 1;
        for (const auto& a : jarr(o, "plan"))
            if (i <= 4) doing.push_back(std::to_string(i++) + ". " + jstr(a, "name"));
        f.lines(doing, 16);
    });
}

void pageRelations(Ctx& c, Screens& s, Win& w, const Json& d, const ui::Rect& r) {
    (void)s;
    const float U = c.u();
    ui::Ui& g = c.gui();
    static const char* F[] = {"all", "family", "friends", "foes", "known"};
    const Json& rels = jarr(d, "relations");
    std::string filter = w.data.value("relFilter", std::string("all"));
    float x = r.x;
    for (int i = 0; i < 5; ++i) {
        int n = 0;
        for (const auto& rr : rels) n += (std::string(F[i]) == "all" || jstr(rr, "group") == F[i]) ? 1 : 0;
        const std::string label = S(c, std::string("filter.") + F[i]) + " " + std::to_string(n);
        const float bw = c.textW(label, 13 * U) + 22 * U;
        if (g.toggle(ui::widgetId("cs.filter", i), {x, r.y, bw, 28 * U}, label, filter == F[i])) {
            w.data["relFilter"] = F[i];
            w.data["relSel"] = -1;
            filter = F[i];
        }
        x += bw + 4 * U;
    }
    std::vector<const Json*> list;
    for (const auto& rr : rels)
        if (filter == "all" || jstr(rr, "group") == filter) list.push_back(&rr);
    long long sel = w.data.value("relSel", -1LL);
    bool selIn = false;
    for (const auto& rr : rels) selIn = selIn || jid(rr, "id") == sel;
    if (!selIn) sel = list.empty() ? -1 : jid(*list[0], "id");
    w.data["relSel"] = sel;
    const float top = r.y + 36 * U, tw = r.w * 0.55f, h = r.bottom() - top;
    // the table
    {
        const ui::Rect tr{r.x, top, tw, h};
        c.fill(tr.x, tr.y, tr.w, tr.h, pal::leatherD.withAlpha(0.85f));
        c.frame(tr.x, tr.y, tr.w, tr.h, pal::bronze.withAlpha(0.6f), U);
        const float cn = tr.x + 8 * U, cwho = tr.x + tw * 0.36f, ca = tr.x + tw * 0.72f, ct = tr.x + tw * 0.82f, cf = tr.x + tw * 0.92f;
        c.text(cn, tr.y + 6 * U, S(c, "col.name"), pal::gold, 13 * U, true);
        c.text(cwho, tr.y + 6 * U, S(c, "col.who"), pal::gold, 13 * U, true);
        c.text(ca, tr.y + 6 * U, c.fit(S(c, "axis.affinity"), tw * 0.1f, 12 * U), pal::gold, 12 * U);
        c.text(ct, tr.y + 6 * U, c.fit(S(c, "axis.trust"), tw * 0.1f, 12 * U), pal::gold, 12 * U);
        c.text(cf, tr.y + 6 * U, c.fit(S(c, "axis.fear"), tw * 0.08f, 12 * U), pal::gold, 12 * U);
        const ui::Rect in = g.beginScroll(ui::widgetId("cs.reltable"), {tr.x + 2 * U, tr.y + 28 * U, tr.w - 4 * U, tr.h - 30 * U});
        float y = in.y;
        if (list.empty()) {
            c.text(cn, y + 4 * U, S(c, "no_one"), pal::dim, 13 * U);
            y += 24 * U;
        }
        int i = 0;
        for (const Json* rr : list) {
            const long long id = jid(*rr, "id");
            const ui::Rect row{in.x, y, in.w, 24 * U};
            const bool click = g.selectable(ui::widgetId("cs.relrow", i++), row, id == sel);
            if (click) {
                if (c.input().doubleClick) setPerson(c, w, id);
                else w.data["relSel"] = id;
            }
            if (id == sel) c.frame(row.x, row.y, row.w, row.h, pal::gold, 1);
            std::string who;
            for (const auto& k : jarr(*rr, "kin")) who += (who.empty() ? "" : ", ") + k.get<std::string>();
            for (const auto& k : jarr(*rr, "pairs")) who += (who.empty() ? "" : ", ") + k.get<std::string>();
            if (who.empty()) who = S(c, "who." + jstr(*rr, "group"));
            const ui::Colour tc = jflag(*rr, "alive") ? pal::text : pal::dim;
            c.text(cn, y + 4 * U, c.fit(jstr(*rr, "name"), cwho - cn - 6 * U, 13 * U), tc, 13 * U);
            c.text(cwho, y + 4 * U, c.fit(who, ca - cwho - 6 * U, 12 * U), pal::dim, 12 * U);
            const Json ax = rr->value("axes", Json::object());
            auto cell = [&](float cx, double v, bool sgn) {
                const ui::Colour col = sgn ? (v > 0.05 ? pal::good : v < -0.05 ? pal::bad : pal::text) : (v > 0.3 ? pal::bad : pal::text);
                c.text(cx, y + 4 * U, std::to_string(int(std::lround(v * 100))), col, 13 * U);
            };
            cell(ca, jnum(ax, "affinity"), true);
            cell(ct, jnum(ax, "trust"), true);
            cell(cf, jnum(ax, "fear"), false);
            y += 25 * U;
        }
        g.endScroll(y);
    }
    // the chosen one
    const Json* rs = nullptr;
    for (const auto& rr : rels)
        if (jid(rr, "id") == sel) rs = &rr;
    box(c, {r.x + tw + 10 * U, top, r.w - tw - 10 * U, h}, "cs.relone", 0, [&](Flow& f) {
        if (!rs) {
            f.lines({S(c, "no_one")});
            return;
        }
        const Json& rr = *rs;
        std::string who;
        for (const auto& k : jarr(rr, "kin")) who += (who.empty() ? "" : ", ") + k.get<std::string>();
        for (const auto& k : jarr(rr, "pairs")) who += (who.empty() ? "" : ", ") + k.get<std::string>();
        if (who.empty()) who = S(c, "stranger");
        c.face(jid(rr, "id"), jstr(rr, "name"), f.x, f.y, 56 * U);
        c.text(f.x + 64 * U, f.y + 2 * U, c.fit(jstr(rr, "name"), f.w - 64 * U, 17 * U), pal::gold, 17 * U, true);
        c.text(f.x + 64 * U, f.y + 24 * U, c.fit(S(c, "rel_to", {{"pron", S(c, std::string("pron.") + (jstr(rr, "sex") == "f" ? "f" : "m"))}, {"what", who}}), f.w - 64 * U, 12 * U),
               pal::dim, 12 * U);
        std::string sub = jstr(rr, "fam");
        sub += (sub.empty() ? "" : " · ") + c.L("ui2d.age", {{"n", jstr(rr, "age")}});
        c.text(f.x + 64 * U, f.y + 40 * U, c.fit(sub, f.w - 64 * U, 12 * U), pal::dim, 12 * U);
        f.y += 62 * U;
        std::vector<std::string> traits;
        for (const auto& t : jarr(rr, "traits"))
            if (t.is_string()) traits.push_back(t.get<std::string>());
        f.chips(traits);
        f.head(S(c, "axes"));
        const Json ax = rr.value("axes", Json::object());
        for (const char* a : {"familiarity", "affinity", "trust", "respect", "fear", "enmity", "attraction", "debt"}) {
            const double v = jnum(ax, a);
            const std::string an(a);
            if (an == "familiarity" || an == "affinity" || an == "trust" || std::abs(v) >= 0.01) {
                const bool sgn = an == "trust" || an == "affinity" || an == "respect" || an == "debt";
                f.bar(S(c, "axis." + an), v, pal::bronze, std::to_string(int(std::lround(v * 100))), sgn);
            }
        }
        f.head(S(c, jstr(rr, "sex") == "f" ? "remembers_f" : "remembers"));
        std::vector<std::string> mem;
        for (const auto& m : jarr(rr, "mem")) mem.push_back(memLine(c, m));
        if (mem.empty()) mem.push_back(S(c, "none"));
        f.lines(mem, 12);
        if (jflag(rr, "alive") && (!jstr(rr, "act").empty() || !jstr(rr, "goal").empty())) {
            f.head(S(c, "busy"));
            std::vector<std::string> b = {jstr(rr, "act").empty() ? c.L("ui2d.idle") : jstr(rr, "act")};
            if (!jstr(rr, "goal").empty()) b.push_back(S(c, "wants") + ": " + jstr(rr, "goal"));
            f.lines(b);
        }
        if (c.gui().button(ui::widgetId("cs.osheet"), {f.x, f.y, std::min(f.w, 200 * U), 30 * U}, S(c, jstr(rr, "sex") == "f" ? "her_sheet" : "his_sheet")))
            setPerson(c, w, jid(rr, "id"));
        f.y += 36 * U;
    });
}

void pageThings(Ctx& c, Win& w, const Json& d, const ui::Rect& r) {
    const float U = c.u();
    ui::Ui& g = c.gui();
    const Json t = d.value("things", Json::object());
    const std::string selThing = w.data.value("thing", std::string());
    // the paper doll: the figure, four places a side
    const float dw = std::min(r.w * 0.55f, 640 * U);
    const ui::Rect dr{r.x, r.y, dw, r.h};
    c.fill(dr.x, dr.y, dr.w, dr.h, pal::leatherD.withAlpha(0.85f));
    c.frame(dr.x, dr.y, dr.w, dr.h, pal::bronze.withAlpha(0.6f), U);
    c.text(dr.x + 8 * U, dr.y + 6 * U, S(c, "worn"), pal::gold, 15 * U, true);
    const float fig = std::min(dr.w * 0.34f, 200 * U);
    c.face(jid(d, "id"), jstr(d, "name"), dr.x + (dr.w - fig) / 2, dr.y + 40 * U, fig);
    static const struct {
        const char* slot;
        int col, row;
    } PLACES[] = {{"head", 0, 0}, {"shoulder", 0, 1}, {"hands", 0, 2}, {"legs", 0, 3}, {"back", 1, 0}, {"torso", 1, 1}, {"waist", 1, 2}, {"feet", 1, 3}};
    const float pw = (dr.w - fig) / 2 - 16 * U, ph = (dr.h - 50 * U) / 4 - 6 * U;
    int idx = 0;
    for (const auto& pl : PLACES) {
        std::vector<const Json*> items;
        for (const auto& x : jarr(t, "worn"))
            if (jstr(x, "slot") == pl.slot) items.push_back(&x);
        std::string slotName;
        for (const auto& sd : jarr(t, "slots"))
            if (jstr(sd, "id") == pl.slot) slotName = jstr(sd, "name");
        if (slotName.empty() && items.empty()) continue;
        if (slotName.empty()) slotName = jstr(*items[0], "slotName");
        const ui::Rect pr{pl.col == 0 ? dr.x + 8 * U : dr.right() - 8 * U - pw, dr.y + 40 * U + pl.row * (ph + 6 * U), pw, ph};
        c.fill(pr.x, pr.y, pr.w, pr.h, items.empty() ? pal::walnutD.withAlpha(0.6f) : pal::leather);
        c.frame(pr.x, pr.y, pr.w, pr.h, items.empty() ? pal::bronze.withAlpha(0.35f) : pal::bronze, U);
        c.text(pr.x + 6 * U, pr.y + 4 * U, c.fit(slotName, pr.w - 12 * U, 12 * U), pal::dim, 12 * U);
        float y = pr.y + 22 * U;
        if (items.empty()) c.text(pr.x + 6 * U, y, S(c, "layer_empty"), pal::dim.withAlpha(0.6f), 12 * U);
        for (const Json* x : items) {
            if (y > pr.bottom() - 16 * U) break;
            const ui::Rect ir{pr.x + 2 * U, y - 2 * U, pr.w - 4 * U, 18 * U};
            if (g.selectable(ui::widgetId("cs.worn", idx++), ir, jstr(*x, "id") == selThing))
                w.data["thing"] = jstr(*x, "id") == selThing ? "" : jstr(*x, "id");
            c.text(pr.x + 6 * U, y, c.fit(jstr(*x, "name") + "  (" + jstr(*x, "layerName") + ")", pr.w - 12 * U, 13 * U), pal::text, 13 * U);
            y += 18 * U;
        }
    }
    // what is carried
    box(c, {r.x + dw + 10 * U, r.y, r.w - dw - 10 * U, r.h}, "cs.carried", 0, [&](Flow& f) {
        f.head(S(c, "carried"));
        const double kg = jnum(t, "kg"), maxKg = jnum(t, "maxKg"), ld = maxKg > 0 ? kg / maxKg : 0;
        char a[16], b[16];
        std::snprintf(a, sizeof a, "%.1f", kg);
        std::snprintf(b, sizeof b, "%.0f", maxKg);
        f.bar(S(c, "load", {{"kg", a}, {"max", b}}), ld, ld > 0.8 ? pal::bad : ld > 0.45 ? pal::mid : pal::good, std::to_string(pct(ld)));
        const Json& carried = jarr(t, "carried");
        if (carried.empty()) f.lines({S(c, "none")});
        const float cw = 120 * U, chh = 44 * U;
        const int cols = std::max(1, int((f.w + 4 * U) / (cw + 4 * U)));
        int i = 0;
        for (const auto& x : carried) {
            const ui::Rect cr{f.x + (i % cols) * (cw + 4 * U), f.y + (i / cols) * (chh + 4 * U), cw, chh};
            const bool on = jstr(x, "id") == selThing;
            if (g.selectable(ui::widgetId("cs.carry", i), cr, on)) w.data["thing"] = on ? "" : jstr(x, "id");
            c.fill(cr.x, cr.y, cr.w, cr.h, on ? pal::leather : pal::walnutD.withAlpha(0.7f));
            c.frame(cr.x, cr.y, cr.w, cr.h, on ? pal::gold : pal::bronze.withAlpha(0.6f), U);
            const auto rows = c.wrap(jstr(x, "name"), cr.w - 10 * U, 12 * U);
            for (std::size_t k = 0; k < rows.size() && k < 2; ++k) c.text(cr.x + 5 * U, cr.y + 4 * U + k * 16 * U, rows[k], pal::text, 12 * U);
            if (jnum(x, "n") > 1) {
                const std::string n = "x" + std::to_string(int(jnum(x, "n")));
                c.text(cr.right() - c.textW(n, 12 * U) - 4 * U, cr.bottom() - 17 * U, n, pal::gold, 12 * U);
            }
            ++i;
        }
        f.y += ((i + cols - 1) / cols) * (chh + 4 * U) + 8 * U;
        const Json* sel = nullptr;
        for (const auto& x : carried)
            if (jstr(x, "id") == selThing) sel = &x;
        for (const auto& x : jarr(t, "worn"))
            if (jstr(x, "id") == selThing) sel = &x;
        if (sel) {
            std::string l = jstr(*sel, "name") + (jnum(*sel, "n") > 1 ? " x" + std::to_string(int(jnum(*sel, "n"))) : "");
            char k[16];
            std::snprintf(k, sizeof k, "%.1f", jnum(*sel, "kg"));
            std::string l2 = S(c, "kg", {{"kg", k}});
            if (sel->contains("worth") && (*sel)["worth"].is_number()) l2 += " · " + S(c, "worth", {{"n", jstr(*sel, "worth")}});
            if (!jstr(*sel, "layerName").empty()) l2 += " · " + jstr(*sel, "slotName") + ", " + jstr(*sel, "layerName");
            f.lines({l, l2});
        }
    });
}

void pageSkills(Ctx& c, const Json& d, const ui::Rect& r) {
    const float U = c.u();
    const Json s = d.value("skills", Json::object());
    const float cw = (r.w - 2 * 10 * U) / 3;
    box(c, {r.x, r.y, cw, r.h}, "cs.sk", 0, [&](Flow& f) {
        f.head(S(c, "skills"));
        const Json& skills = jarr(s, "skills");
        if (skills.empty()) f.lines({S(c, "none")});
        std::vector<std::string> cats;
        for (const auto& k : skills) {
            const std::string cn = jstr(k, "catName").empty() ? S(c, "other") : jstr(k, "catName");
            if (std::find(cats.begin(), cats.end(), cn) == cats.end()) cats.push_back(cn);
        }
        for (const auto& cn : cats) {
            c.text(f.x, f.y, cn, pal::dim, 12 * U);
            f.y += 18 * U;
            for (const auto& k : skills)
                if ((jstr(k, "catName").empty() ? S(c, "other") : jstr(k, "catName")) == cn) f.bar(jstr(k, "name"), jnum(k, "v"), pal::gold, std::to_string(pct(jnum(k, "v"))));
        }
        if (!jarr(s, "knows").empty()) {
            f.head(S(c, "knows"));
            std::vector<std::string> ks;
            for (const auto& k : jarr(s, "knows")) ks.push_back(jstr(k, "name"));
            f.chips(ks);
        }
    });
    box(c, {r.x + cw + 10 * U, r.y, cw, r.h}, "cs.sk", 1, [&](Flow& f) {
        f.head(S(c, "traits"));
        std::vector<std::string> ts;
        for (const auto& t : jarr(s, "traits")) ts.push_back(jstr(t, "name") + (jstr(t, "desc").empty() ? "" : " — " + jstr(t, "desc")));
        if (ts.empty()) ts.push_back(S(c, "none"));
        f.lines(ts);
        if (!jarr(s, "attrs").empty()) {
            f.head(S(c, "attrs"));
            for (const auto& a : jarr(s, "attrs")) f.bar(jstr(a, "name"), jnum(a, "v"), pal::bronze, std::to_string(pct(jnum(a, "v"))));
        }
    });
    box(c, {r.x + 2 * (cw + 10 * U), r.y, cw, r.h}, "cs.sk", 2, [&](Flow& f) {
        f.head(S(c, jstr(d, "sex") == "f" ? "who_is_f" : "who_is"));
        std::vector<std::string> ls;
        auto line = [&](const char* k, const std::string& v) {
            if (!v.empty()) ls.push_back(S(c, k) + ": " + v);
        };
        line("role_lbl", jstr(d, "role"));
        line("house", jstr(d, "house").empty() ? jstr(d, "fam") : jstr(d, "house"));
        line("occ_lbl", jstr(d, "occ"));
        line("backstory", jstr(d, "backstory"));
        line("standing_lbl", jstr(d, "standing"));
        line("life_lbl", jstr(d, "life"));
        if (jnum(s, "renown") != 0) {
            char b[16];
            std::snprintf(b, sizeof b, "%.2f", jnum(s, "renown"));
            line("renown", b);
        }
        if (jnum(s, "purse") != 0) line("purse", jstr(s, "purse"));
        if (ls.empty()) ls.push_back(S(c, "none"));
        f.lines(ls);
        if (!jarr(s, "values").empty()) {
            f.head(S(c, "values"));
            for (const auto& v : jarr(s, "values")) f.bar(jstr(v, "name"), jnum(v, "v"), pal::bronze, std::to_string(int(std::lround(jnum(v, "v") * 100))));
        }
        if (!jarr(s, "rep").empty()) {
            f.head(S(c, "rep"));
            for (const auto& v : jarr(s, "rep")) f.bar(jstr(v, "name"), jnum(v, "v"), pal::bronze, std::to_string(int(std::lround(jnum(v, "v") * 100))), true);
        }
    });
}

void pageChronicle(Ctx& c, const Json& d, const ui::Rect& r) {
    const float U = c.u();
    const Json ch = d.value("chronicle", Json::object());
    const float cw = (r.w - 10 * U) / 2;
    box(c, {r.x, r.y, cw, r.h}, "cs.ch", 0, [&](Flow& f) {
        f.head(S(c, "mem"));
        std::vector<std::string> ls;
        for (const auto& m : jarr(ch, "mem")) ls.push_back(memLine(c, m));
        if (ls.empty()) ls.push_back(S(c, "empty"));
        f.lines(ls, 12);
    });
    box(c, {r.x + cw + 10 * U, r.y, cw, r.h}, "cs.ch", 1, [&](Flow& f) {
        f.head(S(c, jstr(d, "sex") == "f" ? "log_f" : "log"));
        const Json& log = jarr(ch, "log");
        if (log.empty()) f.lines({S(c, "empty")});
        long long day = -1;
        std::vector<std::string> ls;
        for (const auto& e : log) {
            if ((long long)jnum(e, "day") != day) {
                if (!ls.empty()) f.lines(ls, 12), ls.clear();
                day = (long long)jnum(e, "day");
                c.text(f.x, f.y, c.L("ui2d.day", {{"n", std::to_string(day + 1)}}), pal::dim, 13 * U, true);
                f.y += 20 * U;
            }
            ls.push_back(jstr(e, "t") + "  " + jstr(e, "text"));
        }
        if (!ls.empty()) f.lines(ls, 12);
    });
}

void render(Ctx& c, Screens& s, Win& w, const ui::Rect& body) {
    const float U = c.u();
    ui::Ui& g = c.gui();
    const Json d = w.data.value("d", Json());
    if (!d.is_object() || jid(d, "id") != w.data["id"].get<long long>()) {
        c.parchLines(body.x, body.y, body.w, {c.L("ui2d.loading")}, 14 * U);
        return;
    }
    // the head: < portrait name > and the buttons
    const float hh = 80 * U;
    if (g.button(ui::widgetId("cs.prev"), {body.x, body.y + 20 * U, 36 * U, 36 * U}, "<")) step(c, w, -1);
    c.face(jid(d, "id"), jstr(d, "name"), body.x + 44 * U, body.y + 2 * U, 72 * U);
    if (g.button(ui::widgetId("cs.next"), {body.x + 124 * U, body.y + 20 * U, 36 * U, 36 * U}, ">")) step(c, w, 1);
    const float nx = body.x + 172 * U;
    c.text(nx, body.y + 6 * U, jstr(d, "name"), pal::gold, 22 * U, true);
    std::string sub;
    for (const std::string& part : {jstr(d, "role"), jstr(d, "occ"), jstr(d, "standing"), c.L("ui2d.age", {{"n", jstr(d, "age")}}), jstr(d, "fam")})
        if (!part.empty()) sub += (sub.empty() ? "" : " · ") + part;
    const float bwAll = 3 * 150 * U;
    c.text(nx, body.y + 36 * U, c.fit(sub, body.w - (nx - body.x) - bwAll, 13 * U), pal::dim, 13 * U);
    const bool alive = jflag(d, "alive");
    std::string state;
    if (!alive) state = c.L("ui2d.dead") + (jstr(d, "cause").empty() ? "" : ": " + jstr(d, "cause"));
    if (jflag(d.value("overview", Json::object()), "captive")) state += (state.empty() ? "" : " · ") + S(c, "captive");
    if (!state.empty()) c.text(nx, body.y + 56 * U, state, pal::bad, 13 * U);
    const long long id = jid(d, "id");
    const bool mine = id == c.meId();
    float bx = body.right();
    auto btn = [&](const char* wid, const std::string& label) {
        bx -= 144 * U;
        const bool hit = g.button(ui::widgetId(wid), {bx, body.y + 20 * U, 140 * U, 34 * U}, label);
        bx -= 6 * U;
        return hit;
    };
    if (btn("cs.map", S(c, "to_map"))) {
        c.select(id);
        if (const Json* p = c.person(id)) c.lookAt(jnum(*p, "x"), jnum(*p, "y"));
        w.closing = true;
    }
    // LR-138: one leads only one's own kin
    const bool canLead = mine || !d.contains("lead") || jflag(d, "lead");
    if (alive && canLead && btn("cs.lead", S(c, mine ? "release" : "lead"))) {
        if (mine) c.cmdToast({{"op", "cmd"}, {"type", "release_control"}, {"args", Json::object()}});
        else c.cmdToast({{"op", "cmd"}, {"type", "take_control"}, {"args", {{"id", id}}}});
        c.refresh();
        load(c, w);
    }
    if (alive && c.meId() >= 0 && !mine && btn("cs.talk", S(c, "talk"))) {
        w.closing = true;
        openTalk(c, s, id, false);
    }
    const ui::Rect page{body.x, body.y + hh + 8 * U, body.w, body.h - hh - 8 * U};
    if (w.tab == "relations") pageRelations(c, s, w, d, page);
    else if (w.tab == "things") pageThings(c, w, d, page);
    else if (w.tab == "skills") pageSkills(c, d, page);
    else if (w.tab == "chronicle") pageChronicle(c, d, page);
    else pageOverview(c, w, d, page);
}

} // namespace

void openSheet(Ctx& c, Screens& s, long long id) {
    if (id < 0) {
        const Json* p = c.person(c.selected());
        id = p && jstr(*p, "sp").empty() ? c.selected() : c.meId();
    }
    if (id < 0) {
        const auto r = ring(c);
        if (r.empty()) return;
        id = r[0];
    }
    auto w = std::make_unique<Win>();
    w->id = "sheet";
    w->title = c.L("ui2d.screen.sheet");
    for (const char* t : {"overview", "relations", "things", "skills", "chronicle"}) w->tabs.push_back({t, S(c, std::string("tab.") + t)});
    w->tab = "overview";
    w->width = 0.9f;
    w->height = 0.9f;
    w->wantW = 1180 * c.u();
    w->fitH = (80 + 8 + kPageH) * c.u();   // the head and the page: every tab has the doll's height
    w->data = {{"id", -1}, {"relFilter", "all"}};
    setPerson(c, *w, id);
    w->render = [&s](Ctx& c, Win& w, const ui::Rect& body) { render(c, s, w, body); };
    w->onPoll = [](Ctx& c, Win& w) {
        if (now() - jnum(w.data, "at") > 1500) load(c, w);
    };
    w->onKey = [](Ctx& c, Win& w, SDL_Scancode sc) {
        if (sc == SDL_SCANCODE_COMMA) step(c, w, -1);
        else if (sc == SDL_SCANCODE_PERIOD) step(c, w, 1);
        else return false;
        return true;
    };
    s.push(std::move(w));
}

} // namespace game2d
