// AL-136: the character's creation, after the backstory is picked (menu2d_view.cpp
// "whom to be" -> a newcomer): the people (race), the look (the pawn's layers),
// talents, flaws, nature and the rare magic gift for points, the attributes for
// points; the name, sex and age beside a preview of the pawn. The rules and the
// names come from /api/menu options.create (src/client2d/session2d creationJson,
// src/sim/charcreate, content/ai/character_creation.json); the sim checks the
// choice again on /api/begin {create: {...}}.
#include <algorithm>
#include <cmath>
#include <random>

#include "native/ctx2d.hpp"

namespace game2d {

namespace {

std::uint64_t wid(const std::string& s, int i = 0) { return ui::widgetId(s.c_str(), i); }

const Json& rules(const Win& w) { return w.data["R"]; }

const Json* raceRule(const Win& w) {
    for (const auto& r : jarr(rules(w), "races"))
        if (jstr(r, "id") == jstr(w.data, "race")) return &r;
    return nullptr;
}

bool hasTrait(const Win& w, const std::string& id) {
    for (const auto& t : w.data["traits"])
        if (t == id) return true;
    return false;
}

void dropTrait(Win& w, const std::string& id) {
    Json out = Json::array();
    for (const auto& t : w.data["traits"])
        if (t != id) out.push_back(t);
    w.data["traits"] = out;
}

bool exclusiveWith(const Win& w, const std::string& a, const std::string& b) {
    for (const auto& p : jarr(rules(w), "exclusive"))
        if (p.is_array() && p.size() == 2 && ((p[0] == a && p[1] == b) || (p[0] == b && p[1] == a))) return true;
    return false;
}

std::vector<std::string> backTraits(const Win& w) {
    std::vector<std::string> out;
    const Json B = rules(w).value("backstory", Json::object()).value(jstr(w.data, "back"), Json::object());
    for (const auto& t : jarr(B, "traits"))
        if (t.is_string()) out.push_back(t.get<std::string>());
    return out;
}

double backShift(const Win& w, const std::string& attr) {
    const Json B = rules(w).value("backstory", Json::object()).value(jstr(w.data, "back"), Json::object());
    return jnum(B.value("attrs", Json::object()), attr.c_str(), 0.0);
}

int costOf(const Win& w, const std::string& id) {
    if (id == jstr(rules(w).value("gift", Json::object()), "trait")) {
        const Json* r = raceRule(w);
        return r ? int(jnum(*r, "gift_cost", 6)) : 6;
    }
    for (const char* g : {"talents", "flaws"})
        for (const auto& t : jarr(rules(w), g))
            if (jstr(t, "id") == id) return int(jnum(t, "cost"));
    return 0;
}

int countIn(const Win& w, const char* group) {
    int n = 0;
    for (const auto& t : jarr(rules(w), group))
        if (hasTrait(w, jstr(t, "id"))) ++n;
    return n;
}

int spent(const Win& w) {
    int s = 0;
    for (const auto& t : w.data["traits"]) s += costOf(w, t.get<std::string>());
    const Json A = rules(w).value("attrs", Json::object());
    const double base = jnum(A, "base", 0.5), step = std::max(0.01, jnum(A, "step", 0.05));
    for (const auto& [k, v] : w.data["attrs"].items()) {
        const int st = int(std::lround((v.get<double>() - base) / step));
        s += st > 0 ? st * int(jnum(A, "cost", 1)) : st * int(jnum(A, "refund", 1));
    }
    return s;
}

const Json& lookOpts(const Win& w) {
    static const Json none = Json::object();
    const Json* r = raceRule(w);
    if (!r || !r->contains("look")) return none;
    const Json& L = (*r)["look"];
    const std::string sx = jstr(w.data, "sex");
    return L.contains(sx) ? L[sx] : none;
}

// keeps the look within what the kind and sex allow (the first, the skin's middle, by default)
void fixLook(Win& w) {
    const Json& o = lookOpts(w);
    Json look = Json::object();
    for (const auto& [k, vals] : o.items()) {
        if (!vals.is_array() || vals.empty()) continue;
        const std::string cur = jstr(w.data["look"], k.c_str());
        bool ok = false;
        for (const auto& v : vals) ok = ok || v == cur;
        if (ok) look[k] = cur;
        else if (k == "skin") look[k] = vals[vals.size() / 2];
        else if (k == "hair" && jstr(w.data, "sex") == "f" && vals.size() > 1) look[k] = vals[1];
        else look[k] = vals[0];
    }
    w.data["look"] = look;
}

void randomLook(Win& w) {
    static std::mt19937 rng{std::random_device{}()};
    const Json& o = lookOpts(w);
    Json look = Json::object();
    for (const auto& [k, vals] : o.items())
        if (vals.is_array() && !vals.empty()) look[k] = vals[rng() % vals.size()];
    w.data["look"] = look;
}

// the person the preview draws: the window's pawn rules over the chosen look
Json previewPerson(const Win& w) {
    Json worn = Json::array();
    for (const auto& b : jarr(w.data["M"].value("options", Json::object()), "backstories"))
        if (jstr(b, "id") == jstr(w.data, "back"))
            for (const auto& g : jarr(b, "gear")) worn.push_back(jstr(g, "id"));
    Json p = {{"id", -1}, {"age", jnum(w.data, "age", 24)}, {"sex", jstr(w.data, "sex")}, {"famId", 0}, {"worn", worn}, {"look", w.data["look"]}, {"alive", true}};
    if (const Json* r = raceRule(w))
        if (!jstr(*r, "skin").empty()) p["rskin"] = jstr(*r, "skin");
    if (const Json* r = raceRule(w))
        if (r->contains("pawn")) p["rlook"] = (*r)["pawn"];   // LR-139: the kind's ears, head, tusks...
    if (!jstr(w.data, "race").empty()) p["race"] = jstr(w.data, "race");   // LR-165: the people's 48x64 figure
    return p;
}

// real years of the kind for `human` years in the human measure (sim raceAgeDays)
int realYears(const Json* r, double human) {
    if (!r) return int(human);
    const double a = jnum(*r, "adult", 14), e = jnum(*r, "elder", 60);
    if (human <= 14) return int(a * human / 14.0);
    return int(a + (human - 14.0) * (e - a) / 46.0);
}

void swatch(Ctx& c, const ui::Rect& r, const std::string& hex, bool on) {
    c.fill(r.x, r.y, r.w, r.h, pal::hex(hex.c_str()));
    c.frame(r.x, r.y, r.w, r.h, on ? pal::gold : pal::walnutD, on ? 3 * c.u() : c.u());
}

// a row of word toggles; returns the next y
float wordRow(Ctx& c, Win& w, const std::string& key, const Json& vals, float x, float y, float width) {
    const float U = c.u();
    float xx = x;
    int i = 0;
    for (const auto& v : vals) {
        const std::string id = v.get<std::string>();
        const std::string label = c.L("ui2d.cc." + key + "." + id);
        const float bw = c.textW(label, 14 * U) + 26 * U;
        if (xx + bw > x + width) xx = x, y += 34 * U;
        if (c.gui().toggle(wid("cc.look." + key, i++), {xx, y, bw, 28 * U}, label, jstr(w.data["look"], key.c_str()) == id)) w.data["look"][key] = id;
        xx += bw + 6 * U;
    }
    return y + 36 * U;
}

float swatchRow(Ctx& c, Win& w, const std::string& key, const Json& vals, float x, float y, float width) {
    const float U = c.u(), s = 30 * U;
    float xx = x;
    int i = 0;
    for (const auto& v : vals) {
        const std::string id = v.get<std::string>();
        if (xx + s > x + width) xx = x, y += s + 6 * U;
        const ui::Rect r{xx, y, s, s};
        const bool on = jstr(w.data["look"], key.c_str()) == id;
        if (c.gui().selectable(wid("cc.sw." + key, i++), r, on)) w.data["look"][key] = id;
        swatch(c, r, id, on);
        xx += s + 6 * U;
    }
    return y + s + 12 * U;
}

void drawRace(Ctx& c, Win& w, const ui::Rect& area) {
    const float U = c.u();
    ui::Ui& g = c.gui();
    const Json& R = rules(w);
    c.text(area.x, area.y, c.L("ui2d.cc.race.hint", {{"culture", jstr(R.value("culture", Json::object()), "name")}}), pal::dim, 13 * U);
    const ui::Rect sr{area.x, area.y + 24 * U, area.w, area.h - 24 * U};
    const ui::Rect in = g.beginScroll(wid("cc.races"), sr);
    const float gap = 10 * U;
    const int cols = std::max(1, int((in.w + gap) / (300 * U + gap)));
    const float cw = (in.w - gap * (cols - 1)) / cols, ch = 188 * U;
    float bottom = in.y;
    int i = 0;
    for (const auto& r : jarr(R, "races")) {
        const float x = in.x + (i % cols) * (cw + gap), y = in.y + (i / cols) * (ch + gap);
        const ui::Rect box{x, y, cw, ch};
        const bool on = jstr(r, "id") == jstr(w.data, "race");
        if (g.selectable(wid("cc.race", i), box, on) && !on) {
            w.data["race"] = jstr(r, "id");
            if (jstr(r, "sexes").size() == 1) w.data["sex"] = jstr(r, "sexes"); // LR-166: nymphs, satyrs
            fixLook(w);
        }
        c.fill(box.x, box.y, box.w, box.h, on ? pal::leather : pal::leatherD.withAlpha(0.85f));
        c.frame(box.x, box.y, box.w, box.h, on ? pal::gold : pal::bronze, on ? 2 * U : U);
        if (!jstr(r, "skin").empty()) swatch(c, {box.right() - 26 * U, box.y + 8 * U, 18 * U, 18 * U}, jstr(r, "skin"), false);
        c.text(box.x + 8 * U, box.y + 6 * U, c.fit(jstr(r, "name"), box.w - 44 * U, 17 * U), on ? pal::gold : pal::text, 17 * U, true);
        const auto rows = c.wrap(jstr(r, "desc"), box.w - 16 * U, 12 * U);
        for (std::size_t k = 0; k < rows.size() && k < 3; ++k) c.text(box.x + 8 * U, box.y + 30 * U + k * 16 * U, rows[k], pal::dim, 12 * U);
        c.text(box.x + 8 * U, box.y + 82 * U,
               c.fit(c.L("ui2d.cc.race.life", {{"a", std::to_string(int(jnum(r, "adult")))}, {"e", std::to_string(int(jnum(r, "elder")))},
                                               {"l", std::to_string(int(jnum(r, "lifespan")))}}),
                     box.w - 16 * U, 12 * U),
               pal::text, 12 * U);
        std::vector<std::string> tags;
        int n = 0;
        for (const auto& a : jarr(r, "aptitudes"))
            if (jnum(a, "learn", 1) > 1.0 && n++ < 3) tags.push_back("+" + jstr(a, "name"));
        for (const auto& a : jarr(r, "affinity"))
            if (jnum(a, "v", 1) > 1.0 && n++ < 5) tags.push_back(c.L("ui2d.cc.race.aff", {{"name", jstr(a, "name")}}));
        c.chips(box.x + 8 * U, box.y + 102 * U, box.w - 16 * U, tags);
        c.text(box.x + 8 * U, box.bottom() - 42 * U, c.L("ui2d.cc.race.gift", {{"n", std::to_string(int(jnum(r, "gift_cost")))}}), pal::bronzeL, 12 * U);
        const double wel = jnum(r, "welcome", 1);
        const bool stranger = wel < jnum(R, "welcome_below", 0.6);
        c.text(box.x + 8 * U, box.bottom() - 22 * U, c.fit(c.L(stranger ? "ui2d.cc.race.stranger" : "ui2d.cc.race.own"), box.w - 16 * U, 12 * U),
               stranger ? pal::bad : pal::good, 12 * U);
        bottom = std::max(bottom, box.bottom() + gap);
        ++i;
    }
    g.endScroll(bottom);
}

void drawLook(Ctx& c, Win& w, const ui::Rect& area) {
    const float U = c.u();
    const Json& o = lookOpts(w);
    const ui::Rect in = c.gui().beginScroll(wid("cc.lookscroll"), area);
    float y = in.y;
    auto head = [&](const char* key) {
        c.text(in.x, y, c.L(std::string("ui2d.cc.look.") + key), pal::gold, 15 * U, true);
        y += 24 * U;
    };
    for (const char* k : {"body", "skin", "hair", "hair_colour", "beard", "cloth"}) {
        if (!o.contains(k) || !o[k].is_array() || o[k].empty()) continue;
        head(k);
        const std::string key = k;
        if (key == "skin" || key == "hair_colour" || key == "cloth") y = swatchRow(c, w, key, o[k], in.x, y, in.w);
        else y = wordRow(c, w, key, o[k], in.x, y, in.w);
    }
    if (c.gui().button(wid("cc.random"), {in.x, y + 4 * U, 200 * U, 34 * U}, c.L("ui2d.cc.random"))) randomLook(w);
    c.gui().endScroll(y + 50 * U);
}

void drawGifts(Ctx& c, Win& w, const ui::Rect& area) {
    const float U = c.u();
    ui::Ui& g = c.gui();
    const Json& R = rules(w);
    const Json mx = R.value("max", Json::object());
    const ui::Rect in = g.beginScroll(wid("cc.gifts"), {area.x, area.y, area.w, area.h - 40 * U});
    float y = in.y;
    std::string hover;
    const auto fixed = backTraits(w);
    // from the backstory
    if (!fixed.empty()) {
        c.text(in.x, y, c.L("ui2d.cc.fixed"), pal::gold, 15 * U, true);
        y += 24 * U;
        std::vector<std::string> names;
        for (const char* gr : {"talents", "flaws", "nature"})
            for (const auto& t : jarr(R, gr))
                if (std::find(fixed.begin(), fixed.end(), jstr(t, "id")) != fixed.end()) names.push_back(jstr(t, "name"));
        if (names.empty())
            for (const auto& f : fixed) names.push_back(f);
        y = c.chips(in.x, y, in.w, names) + 10 * U;
    }
    // the gift
    {
        const Json G = R.value("gift", Json::object());
        const std::string id = jstr(G, "trait");
        const bool on = hasTrait(w, id);
        const int cost = costOf(w, id);
        const ui::Rect box{in.x, y, in.w, 64 * U};
        const bool can = on || spent(w) + cost <= int(jnum(R, "points", 10));
        if (g.selectable(wid("cc.gift"), box, on) && can) {
            if (on) dropTrait(w, id);
            else w.data["traits"].push_back(id);
        }
        c.fill(box.x, box.y, box.w, box.h, on ? pal::hex("#3a2a5a") : pal::leatherD.withAlpha(0.85f));
        c.frame(box.x, box.y, box.w, box.h, on ? pal::gold : pal::bronze, on ? 2 * U : U);
        c.text(box.x + 10 * U, box.y + 6 * U, c.L("ui2d.cc.gift.title") + "  " + c.L("ui2d.cc.cost", {{"n", std::to_string(cost)}}),
               on ? pal::gold : (can ? pal::text : pal::dim), 16 * U, true);
        c.text(box.x + 10 * U, box.y + 30 * U, c.fit(c.L("ui2d.cc.gift.hint"), box.w - 20 * U, 12 * U), pal::dim, 12 * U);
        if (g.hovered(box)) hover = jstr(G, "desc");
        y += box.h + 14 * U;
    }
    auto group = [&](const char* gr, const char* title, int max, bool free) {
        const int have = countIn(w, gr);
        c.text(in.x, y, c.L(title, {{"n", std::to_string(max)}, {"have", std::to_string(have)}}), pal::gold, 15 * U, true);
        y += 26 * U;
        float x = in.x;
        int i = 0;
        for (const auto& t : jarr(R, gr)) {
            const std::string id = jstr(t, "id");
            if (std::find(fixed.begin(), fixed.end(), id) != fixed.end()) continue;
            const int cost = free ? 0 : int(jnum(t, "cost"));
            const std::string label = jstr(t, "name") + (free ? "" : (cost > 0 ? "  " + std::to_string(cost) : "  +" + std::to_string(-cost)));
            const float bw = c.textW(label, 13 * U) + 22 * U;
            if (x + bw > in.x + in.w) x = in.x, y += 32 * U;
            const bool on = hasTrait(w, id);
            bool can = on || have < max;
            if (!on && cost > 0 && spent(w) + cost > int(jnum(R, "points", 10))) can = false;
            if (!on) {
                for (const auto& o : w.data["traits"]) can = can && !exclusiveWith(w, o.get<std::string>(), id);
                for (const auto& o : fixed) can = can && !exclusiveWith(w, o, id);
            }
            const ui::Rect r{x, y, bw, 26 * U};
            if (g.toggle(wid(std::string("cc.tr.") + gr, i++), r, label, on, can)) {
                if (on) dropTrait(w, id);
                else w.data["traits"].push_back(id);
            }
            if (g.hovered(r)) hover = jstr(t, "name") + ": " + jstr(t, "desc");
            x += bw + 6 * U;
        }
        y += 40 * U;
    };
    group("talents", "ui2d.cc.talents", int(jnum(mx, "talents", 3)), false);
    group("flaws", "ui2d.cc.flaws", int(jnum(mx, "flaws", 3)), false);
    group("nature", "ui2d.cc.nature", int(jnum(mx, "nature", 3)), true);
    g.endScroll(y);
    c.parchLines(area.x, area.bottom() - 34 * U, area.w, {hover.empty() ? c.L("ui2d.cc.gifts.hint") : hover}, 13 * U);
}

void drawAttrs(Ctx& c, Win& w, const ui::Rect& area) {
    const float U = c.u();
    ui::Ui& g = c.gui();
    const Json A = rules(w).value("attrs", Json::object());
    const double base = jnum(A, "base", 0.5), lo = jnum(A, "min", 0.3), hi = jnum(A, "max", 0.8), step = jnum(A, "step", 0.05);
    const int points = int(jnum(rules(w), "points", 10));
    c.text(area.x, area.y, c.L("ui2d.cc.attrs.hint"), pal::dim, 13 * U);
    float y = area.y + 28 * U;
    int i = 0;
    for (const auto& a : jarr(A, "list")) {
        const std::string id = jstr(a, "id");
        double v = w.data["attrs"].contains(id) ? w.data["attrs"][id].get<double>() : base;
        const float rowH = 46 * U;
        c.text(area.x, y + 2 * U, jstr(a, "name"), pal::text, 15 * U, true);
        c.text(area.x, y + 22 * U, c.fit(jstr(a, "desc"), 300 * U, 11 * U), pal::dim, 11 * U);
        const float bx = area.x + 320 * U;
        if (g.button(wid("cc.a.minus", i), {bx, y + 6 * U, 30 * U, 30 * U}, "−", v - step >= lo - 1e-6)) v -= step;
        const ui::Rect bar{bx + 38 * U, y + 14 * U, std::max(80 * U, area.w - 320 * U - 160 * U), 14 * U};
        c.gauge(bar, v, v > base + 1e-6 ? pal::good : (v < base - 1e-6 ? pal::bad : pal::mid));
        const bool canUp = v + step <= hi + 1e-6 && spent(w) + int(jnum(A, "cost", 1)) <= points;
        if (g.button(wid("cc.a.plus", i), {bar.right() + 8 * U, y + 6 * U, 30 * U, 30 * U}, "+", canUp)) v += step;
        c.text(bar.right() + 46 * U, y + 10 * U, std::to_string(int(std::lround(v * 100))), pal::gold, 15 * U, true);
        const double sh = backShift(w, id);
        if (std::fabs(sh) > 1e-6)
            c.text(bar.right() + 82 * U, y + 12 * U,
                   c.L("ui2d.cc.attrs.back", {{"v", (sh > 0 ? "+" : "") + std::to_string(int(std::lround(sh * 100)))}}), sh > 0 ? pal::good : pal::bad,
                   12 * U);
        v = std::round(v / step) * step;
        if (std::fabs(v - base) < 1e-6) w.data["attrs"].erase(id);
        else w.data["attrs"][id] = v;
        y += rowH;
        ++i;
    }
}

void openCreateWin(Ctx& c, Screens& s, const std::string& back) {
    auto w = std::make_unique<Win>();
    w->id = "create";
    w->title = c.L("ui2d.cc.title");
    w->tabs = {{"race", c.L("ui2d.cc.tab.race")}, {"look", c.L("ui2d.cc.tab.look")}, {"gifts", c.L("ui2d.cc.tab.gifts")}, {"attrs", c.L("ui2d.cc.tab.attrs")}};
    w->tab = "race";
    w->width = 0.88f;
    w->height = 0.9f;
    const Json M = c.get("/api/menu");
    const Json R = M.value("options", Json::object()).value("create", Json::object());
    w->data = {{"M", M}, {"R", R}, {"back", back}, {"race", "human"}, {"sex", "m"}, {"name", ""}, {"traits", Json::array()}, {"attrs", Json::object()}, {"look", Json::object()}};
    w->data["age"] = jnum(R.value("age", Json::object()), "default", 24);
    if (!jarr(R, "races").empty()) w->data["race"] = jstr(jarr(R, "races")[0], "id");
    fixLook(*w);
    w->render = [&s](Ctx& c, Win& w, const ui::Rect& body) {
        const float U = c.u();
        ui::Ui& g = c.gui();
        const Json& R = rules(w);
        if (jarr(R, "races").empty()) {
            c.parchLines(body.x, body.y, body.w, {c.L("ui2d.cc.none")}, 14 * U);
            return;
        }
        const float bh = 42 * U, sideW = std::min(300 * U, body.w * 0.3f);
        // --- the left column: the pawn, name, sex, age, points
        const ui::Rect side{body.x, body.y, sideW, body.h - bh - 10 * U};
        c.fill(side.x, side.y, side.w, side.h, pal::leatherD.withAlpha(0.7f));
        c.frame(side.x, side.y, side.w, side.h, pal::bronze, U);
        const Json p = previewPerson(w);
        const float big = std::min(side.w - 40 * U, 160 * U);
        c.fill(side.x + (side.w - big) / 2, side.y + 10 * U, big, big, pal::walnutD);
        c.pawn(p, 'S', side.x + (side.w - big) / 2, side.y + 10 * U, big);
        const float sm = 48 * U;
        float px = side.x + (side.w - 3 * sm - 16 * U) / 2;
        for (char d : {'E', 'N', 'W'}) {
            c.pawn(p, d, px, side.y + big + 16 * U, sm);
            px += sm + 8 * U;
        }
        float y = side.y + big + sm + 26 * U;
        const float x = side.x + 10 * U, fw = side.w - 20 * U;
        c.text(x, y, c.L("ui2d.cc.name"), pal::gold, 14 * U, true);
        y += 20 * U;
        std::string name = jstr(w.data, "name");
        if (g.textField(wid("cc.name"), {x, y, fw, 28 * U}, name, c.L("ui2d.cc.name.hint"), 40)) w.data["name"] = name;
        y += 36 * U;
        const float hw = (fw - 6 * U) / 2;
        for (int k = 0; k < 2; ++k) {
            const std::string sx = k ? "f" : "m";
            const Json* rr = raceRule(w);
            const std::string only = rr ? jstr(*rr, "sexes") : std::string();
            if (only.size() == 1 && only != sx) continue; // LR-166: a one-sex kind
            if (g.toggle(wid("cc.sex", k), {x + k * (hw + 6 * U), y, hw, 28 * U}, c.L("ui2d.cc.sex." + sx), jstr(w.data, "sex") == sx) && jstr(w.data, "sex") != sx) {
                w.data["sex"] = sx;
                fixLook(w);
            }
        }
        y += 36 * U;
        const Json A = R.value("age", Json::object());
        std::int32_t age = std::int32_t(jnum(w.data, "age", 24));
        c.text(x, y + 5 * U, c.L("ui2d.cc.age"), pal::gold, 14 * U, true);
        if (g.stepper(wid("cc.age"), {x + fw - 120 * U, y, 120 * U, 28 * U}, age, std::int32_t(jnum(A, "min", 16)), std::int32_t(jnum(A, "max", 60))))
            w.data["age"] = age;
        y += 32 * U;
        c.text(x, y, c.fit(c.L("ui2d.cc.age.real", {{"n", std::to_string(realYears(raceRule(w), age))}}), fw, 12 * U), pal::dim, 12 * U);
        y += 24 * U;
        std::string backName = jstr(w.data, "back");
        for (const auto& b : jarr(w.data["M"].value("options", Json::object()), "backstories"))
            if (jstr(b, "id") == jstr(w.data, "back")) backName = jstr(b, "name");
        c.text(x, y, c.fit(c.L("ui2d.cc.backstory", {{"name", backName}}), fw, 13 * U), pal::text, 13 * U);
        y += 22 * U;
        if (const Json* r = raceRule(w)) {
            c.text(x, y, c.fit(c.L("ui2d.cc.race", {{"name", jstr(*r, "name")}}), fw, 13 * U), pal::text, 13 * U);
            y += 22 * U;
        }
        const int points = int(jnum(R, "points", 10)), left = points - spent(w);
        c.text(x, side.bottom() - 30 * U, c.L("ui2d.cc.points", {{"n", std::to_string(left)}, {"max", std::to_string(points)}}), left < 0 ? pal::bad : pal::gold,
               17 * U, true);
        // --- the tab
        const ui::Rect area{side.right() + 14 * U, body.y, body.right() - side.right() - 14 * U, body.h - bh - 10 * U};
        if (w.tab == "race") drawRace(c, w, area);
        else if (w.tab == "look") drawLook(c, w, area);
        else if (w.tab == "gifts") drawGifts(c, w, area);
        else drawAttrs(c, w, area);
        // --- the buttons
        const float bw = 200 * U, by = body.bottom() - bh;
        if (g.button(wid("cc.back"), {body.x, by, bw, bh}, c.L("ui2d.menu.back"))) {
            w.closing = true;
            s.open("pick");
        }
        const std::string go = c.L("ui2d.cc.start");
        const float gw = std::max(bw, c.textW(go, 15 * U) + 40 * U);
        if (g.button(wid("cc.go"), {body.right() - gw, by, gw, bh}, go, pal::gold, pal::ink, left >= 0)) {
            Json ch = {{"backstory", jstr(w.data, "back")}, {"race", jstr(w.data, "race")}, {"sex", jstr(w.data, "sex")},
                       {"age", jnum(w.data, "age", 24)}, {"traits", w.data["traits"]}, {"attrs", w.data["attrs"]}, {"look", w.data["look"]},
                       {"lang", c.L("ui2d.cc.lang")}};
            if (!jstr(w.data, "name").empty()) ch["name"] = jstr(w.data, "name");
            const Json r = c.post("/api/begin", {{"create", ch}});
            if (jstr(r, "result") != "ok") c.toast(jstr(r, "text").empty() ? jstr(r, "result") : jstr(r, "text"));
            else {
                w.closing = true;
                c.setFollow(true);
                c.refresh();
                if (c.meId() > 0) c.select(c.meId());
            }
        }
    };
    s.push(std::move(w));
}

} // namespace

void openCreate(Ctx& c, Screens& s, const std::string& backstory) { openCreateWin(c, s, backstory); }

} // namespace game2d
