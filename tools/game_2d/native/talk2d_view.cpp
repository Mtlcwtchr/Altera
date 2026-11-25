// The talk and trade windows in the 2D window (port of web/talk.js): a meeting
// of two, the player's person left, the other right.
//   talk  - how they feel about you (and how it moved in this talk), the lines
//           said and what they mutter aside, topics to bring up, the wheel's acts
//           by sphere with the chance in the tone (7 steps, mouse wheel), an ask
//           of theirs to answer.
//   trade - your pack and their goods with their own price and taste of each,
//           what you give / get, the scales, how they will take it, a counter
//           waiting, the last word of the deal.
// Data: /api/talk?target=&tone=, POST /api/trade {target, give, take}.
#include <cmath>
#include <regex>

#include "engine/ui/font.hpp"
#include "native/ctx2d.hpp"

namespace game2d {

namespace {

double now() { return double(SDL_GetTicks()); }

std::string pct(double v) { return std::to_string(int(std::lround(v * 100))) + "%"; }
std::string signed100(double v) {
    const int n = int(std::lround(std::abs(v) * 100));
    return (v > 0 ? "+" : v < 0 ? "-" : "") + std::to_string(n);
}

std::string itemIcon(const std::string& item) {
    static const std::vector<std::pair<std::regex, std::string>> icons = {
        {std::regex("coin|silver"), "it_coin"},
        {std::regex("bread|loaf|pie|cake|meal|porridge|stew|pottage"), "it_bread"},
        {std::regex("ale|mead|cider|beer|wine"), "it_ale"},
        {std::regex("sword|knife|blade|dagger|spear"), "it_sword"},
        {std::regex("apple|fruit|berr|^pear"), "it_apple"},
        {std::regex("herb|leaf|greens|flower"), "it_herb"},
        {std::regex("ingot|iron|bronze|copper|metal"), "it_ingot"},
        {std::regex("ore"), "it_ore"},
        {std::regex("wheat|grain|flour|seed|barley|oat|rye"), "it_wheat"},
        {std::regex("hammer|axe|tool|pick|saw|sickle"), "it_hammer"}};
    for (const auto& [re, sp] : icons)
        if (std::regex_search(item, re)) return sp;
    return "";
}

bool isPerson(Ctx& c, long long id) {
    const Json* p = c.person(id);
    return p && jstr(*p, "sp").empty();
}

bool canTalk(Ctx& c, long long id) {
    if (c.meId() < 0) {
        c.toast(c.L("ui2d.lead_first"));
        return false;
    }
    if (id < 0 || id == c.meId() || !isPerson(c, id)) {
        c.toast(c.L("ui2d.talk.no_person"));
        return false;
    }
    return true;
}

// a slot of a thing: the icon (or the first letter), the count, a mark of taste; true when clicked (1) / right-clicked (-1)
int slot(Ctx& c, const ui::Rect& r, const Json& g, int n, bool dim, std::uint64_t id, bool* hover = nullptr) {
    const float U = c.u();
    ui::Input& in = c.input();
    const double taste = jnum(g, "taste");
    const bool over = r.contains(in.mouseX, in.mouseY);
    c.fill(r.x, r.y, r.w, r.h, over ? pal::leather : pal::leatherD);
    c.frame(r.x, r.y, r.w, r.h, taste > 0 ? pal::good : taste < 0 ? pal::bad : pal::bronze.withAlpha(0.7f), taste != 0 ? 2 * U : U);
    const float a = dim ? 0.35f : 1.0f;
    const float ic = r.w * 0.5f;
    const std::string sp = itemIcon(jstr(g, "item"));
    if (sp.empty() || !c.spriteAt(sp, Json(), r.x + (r.w - ic) / 2, r.y + 4 * U, ic, a)) {
        const std::string name = jstr(g, "name").empty() ? jstr(g, "item") : jstr(g, "name");
        std::size_t at = 0;
        ui::nextCodepoint(name, at);
        c.textC(r.x + r.w / 2, r.y + 6 * U, name.substr(0, at), pal::gold.withAlpha(a), ic * 0.8f);
    }
    const std::string cnt = std::to_string(n);
    c.text(r.right() - c.textW(cnt, 12 * U) - 4 * U, r.y + 2 * U, cnt, pal::text.withAlpha(a), 12 * U, true);
    c.textC(r.x + r.w / 2, r.bottom() - 16 * U, c.fit(jstr(g, "name"), r.w - 4 * U, 11 * U), pal::text.withAlpha(a), 11 * U);
    c.gui().claim(r);
    if (hover) *hover = over;
    (void)id;
    if (over && in.pressed) return 1;
    if (over && in.rightPressed) return -1;
    return 0;
}

// --- the talk window ----------------------------------------------------------------------
void loadTalk(Ctx& c, Win& w) {
    const long long id = w.data["id"].get<long long>();
    std::string q = "/api/talk?target=" + std::to_string(id);
    if (!jstr(w.data, "tone").empty()) q += "&tone=" + jstr(w.data, "tone");
    const Json d = c.get(q);
    w.data["at"] = now();
    if (!d.is_object()) return;
    if (jflag(d, "ok")) {
        const Json them = d.value("them", Json::object());
        if (!w.data.contains("start") || jid(w.data["start"], "id") != jid(them, "id"))
            w.data["start"] = {{"id", jid(them, "id")}, {"rel", d.value("rel", Json::object())}};
        if (jstr(w.data, "tone").empty()) w.data["tone"] = jstr(d, "tone");
        bool has = false;
        for (const auto& s : jarr(d, "spheres")) has = has || jstr(s, "id") == jstr(w.data, "sphere");
        if (!has) w.data["sphere"] = jarr(d, "spheres").empty() ? "" : jstr(jarr(d, "spheres")[0], "id");
        if (!w.data.contains("d") || w.data["d"] != d) w.data["scrollDown"] = true;
    }
    w.data["d"] = d;
}

void talkTitle(Ctx& c, Win& w) {
    const Json& d = w.data["d"];
    std::string who;
    if (jflag(d, "ok")) who = jstr(d["them"], "name_ins");
    else if (const Json* p = c.person(w.data["id"].get<long long>())) who = jstr(*p, "name");
    w.title = c.L("ui2d.talk.title", {{"who", who}});
}

void renderTalk(Ctx& c, Screens& s, Win& w, const ui::Rect& body) {
    const float U = c.u();
    ui::Ui& g = c.gui();
    ui::Input& in = c.input();
    const Json d = w.data.value("d", Json());
    talkTitle(c, w);
    if (!d.is_object()) {
        c.parchLines(body.x, body.y, body.w, {c.L("ui2d.loading")}, 14 * U);
        return;
    }
    if (!jflag(d, "ok")) {
        c.parchLines(body.x, body.y, body.w, {c.L("ui2d.talk.dead")}, 14 * U);
        return;
    }
    const long long talkId = w.data["id"].get<long long>();
    const Json me = d.value("me", Json::object()), them = d.value("them", Json::object());
    const std::string meName = jstr(me, "name"), themName = jstr(them, "name");
    // the meeting: me | how they feel | them
    const float topH = 104 * U, pt = 96 * U;
    c.face(jid(me, "id"), meName, body.x, body.y, pt);
    c.text(body.x + pt + 8 * U, body.y + 20 * U, c.fit(meName, 200 * U, 17 * U), pal::gold, 17 * U, true);
    c.text(body.x + pt + 8 * U, body.y + 44 * U, c.fit(jstr(me, "role"), 200 * U, 13 * U), pal::dim, 13 * U);
    c.face(jid(them, "id"), themName, body.right() - pt, body.y, pt);
    const float rx = body.right() - pt - 8 * U;
    c.text(rx - c.textW(c.fit(themName, 200 * U, 17 * U), 17 * U), body.y + 20 * U, c.fit(themName, 200 * U, 17 * U), pal::gold, 17 * U, true);
    const std::string sub = jstr(them, "role"), vy = c.L(jflag(them, "vy") ? "ui2d.talk.vy" : "ui2d.talk.ty");
    c.text(rx - c.textW(c.fit(sub, 200 * U, 13 * U), 13 * U), body.y + 44 * U, c.fit(sub, 200 * U, 13 * U), pal::dim, 13 * U);
    c.text(rx - c.textW(vy, 12 * U), body.y + 64 * U, vy, pal::dim, 12 * U);
    {
        const float cx0 = body.x + pt + 220 * U, cx1 = rx - 220 * U, cw = cx1 - cx0;
        if (cw > 160 * U) {
            c.text(cx0, body.y, c.fit(c.L("ui2d.talk.rel", {{"who", jstr(them, "name_gen")}}), cw, 14 * U), pal::gold, 14 * U, true);
            static const char* AXES[] = {"affinity", "trust", "fear", "respect"};
            const Json rel = d.value("rel", Json::object()), names = d.value("rel_names", Json::object());
            const Json start = w.data.value("start", Json::object()).value("rel", Json::object());
            float y = body.y + 24 * U;
            for (const char* a : AXES) {
                const double v = jnum(rel, a), dv = v - jnum(start, a);
                const std::string nm = jstr(names, a).empty() ? a : jstr(names, a);
                c.text(cx0, y, c.fit(nm, 90 * U, 12 * U), pal::text, 12 * U);
                const ui::Rect bar{cx0 + 94 * U, y + 3 * U, cw - 94 * U - 40 * U, 10 * U};
                c.fill(bar.x, bar.y, bar.w, bar.h, pal::leatherD);
                const ui::Colour col = std::string(a) == "fear" ? pal::bad : std::string(a) == "trust" ? pal::hex("#4a8ab8")
                                                                         : std::string(a) == "respect" ? pal::gold : pal::good;
                const float mid = bar.x + bar.w / 2, len = float(std::abs(v)) * bar.w / 2;
                c.fill(v >= 0 ? mid : mid - len, bar.y, len, bar.h, col);
                c.fill(mid, bar.y - 2 * U, std::max(1.0f, U), bar.h + 4 * U, pal::bronzeL);
                c.frame(bar.x, bar.y, bar.w, bar.h, pal::out, 1);
                if (std::abs(dv) > 0.005) c.text(bar.right() + 6 * U, y, signed100(dv), dv > 0 ? pal::good : pal::bad, 12 * U);
                y += 18 * U;
            }
        }
    }
    float y = body.y + topH + 6 * U;
    // the lines said
    const float logH = std::max(90 * U, body.h * 0.26f);
    const ui::Rect logR{body.x, y, body.w, logH};
    c.parchBox(logR);
    {
        const std::uint64_t lid = ui::widgetId("talk.log");
        const ui::Rect li = g.beginScroll(lid, logR.inset(6 * U));
        float ly = li.y;
        const Json& lines = jarr(d, "lines");
        if (lines.empty()) {
            c.text(li.x, ly, c.L("ui2d.talk.log_empty"), pal::ink.withAlpha(0.6f), 13 * U);
            ly += 20 * U;
        }
        for (const auto& l : lines) {
            const bool mine = jid(l, "who") == jid(me, "id");
            const std::string head = jstr(l, "t") + "  " + (mine ? meName : themName) + ": ";
            const auto rows = c.wrap(head + jstr(l, "text"), li.w - 8 * U, 13 * U);
            for (std::size_t k = 0; k < rows.size(); ++k) {
                if (k == 0) {
                    c.text(li.x, ly, head, mine ? pal::hex("#2a4a98") : pal::hex("#8a2a1a"), 13 * U);
                    c.text(li.x + c.textW(head, 13 * U), ly, rows[0].size() > head.size() ? rows[0].substr(head.size()) : "", pal::ink, 13 * U);
                } else c.text(li.x, ly, rows[k], pal::ink, 13 * U);
                ly += c.textH(13 * U) + 5 * U;
            }
        }
        if (!jstr(d, "aside").empty()) {
            const auto rows = c.wrap(themName + " " + c.L("ui2d.talk.aside") + ": " + jstr(d, "aside"), li.w - 8 * U, 13 * U);
            for (const auto& r : rows) {
                c.text(li.x, ly, r, pal::ink.withAlpha(0.7f), 13 * U);
                ly += c.textH(13 * U) + 5 * U;
            }
        }
        g.endScroll(ly);
        if (jflag(w.data, "scrollDown")) {
            g.scrollTo(lid, 1e9f);
            w.data["scrollDown"] = false;
        }
    }
    y += logH + 8 * U;
    // an ask of theirs
    if (d.contains("ask") && d["ask"].is_object()) {
        const Json ask = d["ask"];
        const std::string t = c.L(jstr(ask, "tone").empty() ? "ui2d.talk.asks" : "ui2d.talk.asks_tone",
                                  {{"who", themName}, {"act", jstr(ask, "name")}, {"tone", jstr(ask, "tone")}});
        const ui::Rect r{body.x, y, body.w, 36 * U};
        c.fill(r.x, r.y, r.w, r.h, pal::leather);
        c.frame(r.x, r.y, r.w, r.h, pal::gold, U);
        c.text(r.x + 8 * U, r.y + 9 * U, c.fit(t, r.w - 300 * U, 14 * U), pal::text, 14 * U);
        for (int k = 0; k < 2; ++k) {
            const ui::Rect b{r.right() - (2 - k) * 140 * U, r.y + 3 * U, 132 * U, 30 * U};
            if (g.button(ui::widgetId("talk.ans", k), b, c.L(k == 0 ? "ui2d.talk.yes" : "ui2d.talk.no"))) {
                c.cmdToast({{"op", "cmd"}, {"type", "answer_social"}, {"args", {{"id", c.meId()}, {"accept", k == 0}}}});
                loadTalk(c, w);
            }
        }
        y += 42 * U;
    }
    // topics | acts
    const float gridH = body.bottom() - y;
    const float tw = std::round(body.w * 0.32f);
    const ui::Rect tr{body.x, y, tw, gridH};
    c.fill(tr.x, tr.y, tr.w, tr.h, pal::leather);
    c.text(tr.x + 8 * U, tr.y + 6 * U, c.L("ui2d.talk.topics"), pal::gold, 15 * U, true);
    {
        const ui::Rect ti = g.beginScroll(ui::widgetId("talk.topics"), {tr.x + 6 * U, tr.y + 30 * U, tr.w - 12 * U, tr.h - 36 * U});
        float ty = ti.y;
        const Json& topics = jarr(d, "topics");
        if (topics.empty()) {
            for (const auto& r : c.wrap(c.L("ui2d.talk.no_topics"), ti.w, 12 * U)) {
                c.text(ti.x, ty, r, pal::dim, 12 * U);
                ty += 18 * U;
            }
        }
        int k = 0;
        for (const auto& t : topics) {
            std::string label = jstr(t, "verb") + " " + jstr(t, "label");
            if (jflag(t, "rumor")) label += " [" + c.L("ui2d.talk.rumor") + "]";
            const auto rows = c.wrap(label, ti.w - 12 * U, 13 * U);
            const float h = rows.size() * (c.textH(13 * U) + 4 * U) + 8 * U;
            const ui::Rect r{ti.x, ty, ti.w, h};
            if (g.selectable(ui::widgetId("talk.topic", k++), r, false)) {
                const Json res = c.cmd({{"op", "say_topic"}, {"target", talkId}, {"index", (long long)jnum(t, "i")}});
                const std::string rr = jstr(res, "result");
                c.toast(rr == "ok" || rr.empty() ? c.L("ui2d.talk.said", {{"what", jstr(t, "label")}}) : (jstr(res, "text").empty() ? rr : jstr(res, "text")));
                loadTalk(c, w);
            }
            float ry = r.y + 4 * U;
            for (const auto& row : rows) {
                c.text(r.x + 6 * U, ry, row, pal::text, 13 * U);
                ry += c.textH(13 * U) + 4 * U;
            }
            ty += h + 3 * U;
        }
        g.endScroll(ty);
    }
    const ui::Rect ar{body.x + tw + 10 * U, y, body.w - tw - 10 * U, gridH};
    float ay = ar.y;
    // spheres
    {
        float x = ar.x;
        int k = 0;
        for (const auto& sp : jarr(d, "spheres")) {
            const float bw = c.textW(jstr(sp, "name"), 13 * U) + 22 * U;
            if (x + bw > ar.right()) break;
            if (g.toggle(ui::widgetId("talk.sphere", k++), {x, ay, bw, 28 * U}, jstr(sp, "name"), jstr(sp, "id") == jstr(w.data, "sphere")))
                w.data["sphere"] = jstr(sp, "id");
            x += bw + 4 * U;
        }
        ay += 34 * U;
    }
    const float bottomH = 40 * U + 38 * U;   // tone row + buttons
    {
        Json acts = Json::array();
        for (const auto& sp : jarr(d, "spheres"))
            if (jstr(sp, "id") == jstr(w.data, "sphere")) acts = sp.value("acts", Json::array());
        const ui::Rect gi = g.beginScroll(ui::widgetId("talk.acts"), {ar.x, ay, ar.w, ar.bottom() - ay - bottomH});
        const int cols = std::max(1, int(gi.w / (190 * U)));
        const float bw = (gi.w - (cols - 1) * 6 * U) / cols, bh = 46 * U;
        int k = 0;
        for (const auto& a : acts) {
            const ui::Rect r{gi.x + (k % cols) * (bw + 6 * U), gi.y + (k / cols) * (bh + 6 * U), bw, bh};
            const bool can = jflag(a, "can");
            const bool hit = g.selectable(ui::widgetId("talk.act", k), r, false);
            c.fill(r.x, r.y, r.w, r.h, can ? pal::leatherD : pal::leatherD.withAlpha(0.5f));
            c.frame(r.x, r.y, r.w, r.h, can ? pal::bronze : pal::bronze.withAlpha(0.35f), U);
            c.text(r.x + 6 * U, r.y + 4 * U, c.fit(jstr(a, "name"), r.w - 12 * U, 14 * U), can ? pal::text : pal::dim, 14 * U);
            std::string ch;
            if (!can) ch = jstr(a, "why");
            else if (jstr(a, "kind") == "certain" || !a.contains("chance") || !a["chance"].is_number()) ch = c.L("ui2d.talk.certain");
            else if (jstr(a, "kind") == "win") ch = c.L("ui2d.talk.win", {{"p", std::to_string(int(std::lround(jnum(a, "chance") * 100)))}});
            else ch = pct(jnum(a, "chance"));
            c.text(r.x + 6 * U, r.y + 25 * U, c.fit(ch, r.w - 12 * U, 12 * U), can ? pal::gold : pal::bad, 12 * U);
            if (hit && can) {
                const std::string id = jstr(a, "id");
                if (id.rfind("trade_", 0) == 0 && jflag(d, "trade")) openTalk(c, s, talkId, true);
                else {
                    const Json r2 = c.cmd({{"op", "cmd"}, {"type", "perform"},
                                           {"args", {{"id", c.meId()}, {"interaction", id}, {"target", talkId}, {"tone", jstr(w.data, "tone")}}}});
                    const std::string rr = jstr(r2, "result");
                    c.toast(rr.empty() || rr == "ok" ? jstr(a, "name") : (jstr(r2, "text").empty() ? rr : jstr(r2, "text")));
                    w.data["at"] = now() - 800;   // look again soon: the sim answers on its step
                }
            }
            ++k;
        }
        g.endScroll(gi.y + ((k + cols - 1) / cols) * (bh + 6 * U));
    }
    // the tone: 7 steps, the wheel over them
    {
        const float ty = ar.bottom() - bottomH + 4 * U;
        c.text(ar.x, ty + 6 * U, c.L("ui2d.talk.tone"), pal::gold, 14 * U, true);
        const Json& tones = jarr(d, "tones");
        const float sx = ar.x + 60 * U, sw = 28 * U;
        int cur = 0;
        for (std::size_t k = 0; k < tones.size(); ++k)
            if (jstr(tones[k], "id") == jstr(w.data, "tone")) cur = int(k);
        const ui::Rect strip{sx, ty, tones.size() * (sw + 3 * U), 28 * U};
        for (std::size_t k = 0; k < tones.size(); ++k) {
            const ui::Rect r{sx + k * (sw + 3 * U), ty, sw, 28 * U};
            const double t = tones.size() > 1 ? double(k) / double(tones.size() - 1) : 0.5;
            const ui::Colour col = pal::good.lerp(pal::bad, float(t));
            c.fill(r.x, r.y, r.w, r.h, int(k) == cur ? col : col.withAlpha(0.35f));
            c.frame(r.x, r.y, r.w, r.h, int(k) == cur ? pal::gold : pal::out, int(k) == cur ? 2 * U : 1);
            if (g.selectable(ui::widgetId("talk.tone", int(k)), r, int(k) == cur) && int(k) != cur) {
                w.data["tone"] = jstr(tones[k], "id");
                loadTalk(c, w);
            }
        }
        if (strip.contains(in.mouseX, in.mouseY) && in.wheel != 0 && !tones.empty()) {
            const int j = std::clamp(cur + (in.wheel < 0 ? 1 : -1), 0, int(tones.size()) - 1);
            in.wheel = 0;
            if (j != cur) {
                w.data["tone"] = jstr(tones[std::size_t(j)], "id");
                loadTalk(c, w);
            }
        }
        const std::string tn = cur < int(tones.size()) ? "«" + jstr(tones[std::size_t(cur)], "name") + "»" : "";
        c.text(strip.right() + 10 * U, ty + 6 * U, tn, pal::text, 14 * U);
        c.text(strip.right() + 20 * U + c.textW(tn, 14 * U), ty + 8 * U, c.L("ui2d.talk.tone_hint"), pal::dim, 12 * U);
        // buttons
        const float by = ar.bottom() - 34 * U, bw = 170 * U;
        if (jflag(d, "trade") && g.button(ui::widgetId("talk.trade"), {ar.x, by, bw, 32 * U}, c.L("ui2d.talk.trade"))) openTalk(c, s, talkId, true);
        if (g.button(ui::widgetId("talk.leave"), {ar.right() - bw, by, bw, 32 * U}, c.L("ui2d.talk.leave"))) w.closing = true;
    }
}

// --- the trade window ---------------------------------------------------------------------
void loadTrade(Ctx& c, Win& w) {
    const Json d = c.post("/api/trade", {{"target", w.data["id"]}, {"give", w.data["give"]}, {"take", w.data["take"]}});
    w.data["at"] = now();
    if (!d.is_object()) return;
    // what left the packs leaves the draft
    if (jflag(d, "ok"))
        for (const auto& [side, list] : {std::pair<const char*, const char*>{"give", "mine"}, {"take", "theirs"}}) {
            Json keep = Json::object();
            for (const auto& [item, n] : w.data[side].items()) {
                double have = 0;
                for (const auto& g : jarr(d, list))
                    if (jstr(g, "item") == item) have = jnum(g, "n");
                const int k = std::min(n.get<int>(), int(have));
                if (k > 0) keep[item] = k;
            }
            w.data[side] = keep;
        }
    w.data["d"] = d;
}

const Json* goodOf(const Json& d, const std::string& side, const std::string& item) {
    for (const auto& g : jarr(d, side == "give" ? "mine" : "theirs"))
        if (jstr(g, "item") == item) return &g;
    return nullptr;
}

void addTo(Ctx& c, Win& w, const std::string& side, const std::string& item, int k) {
    const Json& d = w.data["d"];
    const Json* g = goodOf(d, side, item);
    if (!g) return;
    const int was = w.data[side].contains(item) ? w.data[side][item].get<int>() : 0;
    const int n = std::clamp(was + k, 0, int(jnum(*g, "n")));
    if (n) w.data[side][item] = n;
    else w.data[side].erase(item);
    loadTrade(c, w);
}

double sumOf(const Json& d, const Json& draft, const std::string& side) {
    double s = 0;
    for (const auto& [item, n] : draft.items())
        if (const Json* g = goodOf(d, side, item)) s += jnum(*g, "worth") * n.get<int>();
    return s;
}

void renderTrade(Ctx& c, Win& w, const ui::Rect& body) {
    const float U = c.u();
    ui::Ui& g = c.gui();
    ui::Input& in = c.input();
    const Json d = w.data.value("d", Json());
    {
        std::string who;
        if (jflag(d, "ok")) who = jstr(d["them"], "name_ins");
        else if (const Json* p = c.person(w.data["id"].get<long long>())) who = jstr(*p, "name");
        w.title = c.L("ui2d.trade.title", {{"who", who}});
    }
    if (!d.is_object()) {
        c.parchLines(body.x, body.y, body.w, {c.L("ui2d.loading")}, 14 * U);
        return;
    }
    if (!jflag(d, "ok")) {
        c.parchLines(body.x, body.y, body.w, {c.L("ui2d.trade.no_trade")}, 14 * U);
        return;
    }
    const Json me = d.value("me", Json::object()), them = d.value("them", Json::object());
    Json& give = w.data["give"];
    Json& take = w.data["take"];
    const float packW = std::round(body.w * 0.28f), gap = 10 * U;
    const float slotS = 64 * U;
    std::string hoverPrice;
    auto pack = [&](const ui::Rect& r, const std::string& side, const Json& list, const Json& draft, long long who, const std::string& title, bool right) {
        c.fill(r.x, r.y, r.w, r.h, pal::leather);
        c.frame(r.x, r.y, r.w, r.h, pal::bronze, U);
        const float pt = 48 * U;
        if (right) {
            c.face(who, "", r.right() - pt - 6 * U, r.y + 6 * U, pt);
            c.text(r.x + 8 * U, r.y + 18 * U, c.fit(title, r.w - pt - 24 * U, 15 * U), pal::gold, 15 * U, true);
        } else {
            c.face(who, "", r.x + 6 * U, r.y + 6 * U, pt);
            c.text(r.x + pt + 14 * U, r.y + 18 * U, c.fit(title, r.w - pt - 24 * U, 15 * U), pal::gold, 15 * U, true);
        }
        float y = r.y + pt + 14 * U;
        if (right && !jstr(d, "role").empty()) {
            c.text(r.x + 8 * U, y, c.fit(jstr(d, "role"), r.w - 16 * U, 12 * U), pal::dim, 12 * U);
            y += 18 * U;
        }
        const float footH = right ? 70 * U : 26 * U;
        const ui::Rect gi = g.beginScroll(ui::widgetId(right ? "trade.theirs" : "trade.mine"), {r.x + 6 * U, y, r.w - 12 * U, r.bottom() - y - footH});
        const int cols = std::max(1, int((gi.w + 4 * U) / (slotS + 4 * U)));
        int k = 0;
        for (const auto& gd : list) {
            const std::string item = jstr(gd, "item");
            const int inDraft = draft.contains(item) ? draft[item].get<int>() : 0;
            const int left = int(jnum(gd, "n")) - inDraft;
            const ui::Rect sr{gi.x + (k % cols) * (slotS + 4 * U), gi.y + (k / cols) * (slotS + 4 * U), slotS, slotS};
            bool over = false;
            const int hit = slot(c, sr, gd, left, left <= 0, 0, &over);
            if (over) {
                hoverPrice = jstr(gd, "name") + ": " + c.L("ui2d.trade.price", {{"who", jstr(them, "name")}, {"v", [&] {
                                                                                     char b[16];
                                                                                     std::snprintf(b, sizeof b, "%.1f", jnum(gd, "worth"));
                                                                                     return std::string(b);
                                                                                 }()}});
                if (jnum(gd, "taste") != 0) hoverPrice += " · " + c.L(jnum(gd, "taste") > 0 ? "ui2d.trade.likes_mark" : "ui2d.trade.dislikes_mark");
            }
            if (hit != 0) addTo(c, w, side, item, hit * (in.shift ? 5 : 1));
            ++k;
        }
        g.endScroll(gi.y + ((k + cols - 1) / cols) * (slotS + 4 * U));
        float fy = r.bottom() - footH + 4 * U;
        const std::string coin = right ? c.L("ui2d.trade.their_coin", {{"n", jstr(d, "their_coin")}}) : c.L("ui2d.trade.coin", {{"n", jstr(d, "coin")}});
        c.text(r.x + 8 * U, fy, c.fit(coin, r.w - 16 * U, 12 * U), pal::dim, 12 * U);
        if (right) {
            auto joined = [](const Json& a) {
                std::string s;
                for (const auto& x : a)
                    if (x.is_string()) s += (s.empty() ? "" : ", ") + x.get<std::string>();
                return s;
            };
            if (!jarr(d, "likes").empty()) c.text(r.x + 8 * U, fy + 20 * U, c.fit(c.L("ui2d.trade.likes", {{"list", joined(jarr(d, "likes"))}}), r.w - 16 * U, 12 * U), pal::good, 12 * U);
            if (!jarr(d, "dislikes").empty()) c.text(r.x + 8 * U, fy + 40 * U, c.fit(c.L("ui2d.trade.dislikes", {{"list", joined(jarr(d, "dislikes"))}}), r.w - 16 * U, 12 * U), pal::bad, 12 * U);
        }
    };
    pack({body.x, body.y, packW, body.h}, "give", jarr(d, "mine"), give, jid(me, "id"), c.L("ui2d.trade.mine"), false);
    pack({body.right() - packW, body.y, packW, body.h}, "take", jarr(d, "theirs"), take, jid(them, "id"), c.L("ui2d.trade.theirs", {{"who", jstr(them, "name_gen")}}), true);
    // the deal in the middle
    const ui::Rect m{body.x + packW + gap, body.y, body.w - 2 * (packW + gap), body.h};
    float y = m.y;
    const float bw2 = (m.w - gap) / 2;
    for (int side = 0; side < 2; ++side) {
        const std::string sd = side == 0 ? "give" : "take";
        Json& draft = side == 0 ? give : take;
        const ui::Rect br{m.x + side * (bw2 + gap), y + 22 * U, bw2, slotS * 2 + 14 * U};
        c.text(br.x, y, c.L(side == 0 ? "ui2d.trade.give" : "ui2d.trade.take"), pal::gold, 15 * U, true);
        c.fill(br.x, br.y, br.w, br.h, pal::leatherD);
        c.frame(br.x, br.y, br.w, br.h, pal::bronze, U);
        if (draft.empty()) c.textC(br.x + br.w / 2, br.y + br.h / 2 - 8 * U, c.L("ui2d.trade.empty"), pal::dim, 13 * U);
        const int cols = std::max(1, int((br.w - 8 * U) / (slotS + 4 * U)));
        int k = 0;
        std::vector<std::pair<std::string, int>> items;
        for (const auto& [item, n] : draft.items()) items.push_back({item, n.get<int>()});
        for (const auto& [item, n] : items) {
            const Json* gd = goodOf(d, sd, item);
            if (!gd) continue;
            const ui::Rect sr{br.x + 4 * U + (k % cols) * (slotS + 4 * U), br.y + 4 * U + (k / cols) * (slotS + 4 * U), slotS, slotS};
            if (sr.bottom() > br.bottom()) break;
            const int hit = slot(c, sr, *gd, n, false, 0);
            if (hit != 0) addTo(c, w, sd, item, -(in.shift ? 5 : 1));
            ++k;
        }
    }
    y += 22 * U + slotS * 2 + 24 * U;
    // the scales: a beam on a post, tilted toward the heavier side
    const double given = sumOf(d, give, "give"), got = sumOf(d, take, "take");
    const double keep = jnum(d, "keep", 1) > 0 ? jnum(d, "keep", 1) : 1;
    const bool verdict = !jstr(d, "verdict").empty();
    const double bal = verdict ? jnum(d, "gain") - jnum(d, "loss") * keep : given - got;
    const bool any = !give.empty() || !take.empty();
    {
        const double tilt = any ? (bal >= 0 ? -1 : 1) * std::min(1.0, std::abs(bal) / std::max(1.0, given + got) * 2) : 0;
        const float cx = m.x + m.w / 2, top = y + 6 * U, len = std::min(m.w * 0.8f, 260 * U);
        const double a = tilt * 8 * 3.14159265 / 180;
        c.fill(cx - 3 * U, top, 6 * U, 70 * U, pal::bronze);
        c.fill(cx - 24 * U, top + 70 * U, 48 * U, 6 * U, pal::bronze);
        for (float t = -len / 2; t <= len / 2; t += U) {
            const float px = cx + t * float(std::cos(a)), py = top + 4 * U + t * float(std::sin(a));
            c.fill(px, py, std::max(1.0f, U), 4 * U, pal::bronzeL);
        }
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            const float t = s2 * len / 2;
            const float px = cx + t * float(std::cos(a)), py = top + 4 * U + t * float(std::sin(a));
            c.fill(px, py, std::max(1.0f, U), 22 * U, pal::bronze);
            c.fill(px - 22 * U, py + 22 * U, 44 * U, 4 * U, pal::bronze);
        }
        y += 90 * U;
    }
    std::string balText;
    char vb[32];
    std::snprintf(vb, sizeof vb, "%.1f", std::abs(bal));
    if (!any) balText = c.L("ui2d.trade.weigh");
    else if (std::abs(bal) < 0.5) balText = c.L("ui2d.trade.balance_even");
    else balText = c.L(bal > 0 ? "ui2d.trade.balance_theirs" : "ui2d.trade.balance_yours", {{"v", vb}});
    c.textC(m.x + m.w / 2, y, balText, pal::text, 15 * U, true);
    y += 26 * U;
    if (verdict) {
        const std::string v = jstr(d, "verdict");
        const double how = v == "accept" ? std::min(1.0, 0.6 + (jnum(d, "ratio") - 1) * 0.4) : v == "counter" ? 0.4 : 0.12;
        c.text(m.x, y, c.L("ui2d.trade.how"), pal::text, 13 * U);
        c.gauge({m.x + 100 * U, y + 3 * U, m.w - 260 * U, 10 * U}, how, pal::tone(how));
        c.text(m.right() - 150 * U, y, c.fit(c.L("ui2d.trade.verdict." + v), 150 * U, 13 * U), pal::gold, 13 * U);
        y += 22 * U;
        for (const auto& line : {jstr(d, "reason"), d.contains("their_counter") && d["their_counter"].is_object()
                                                         ? c.L("ui2d.trade.would_counter", {{"text", jstr(d["their_counter"], "text")}})
                                                         : std::string()}) {
            if (line.empty()) continue;
            for (const auto& r : c.wrap(line, m.w, 12 * U)) {
                c.text(m.x, y, r, pal::dim, 12 * U);
                y += 17 * U;
            }
        }
    }
    const bool counter = d.contains("counter") && d["counter"].is_object();
    if (counter) y += c.parchLines(m.x, y, m.w, {c.L("ui2d.trade.counter", {{"text", jstr(d["counter"], "text")}})}, 13 * U) + 6 * U;
    // buttons
    {
        const float bw = (m.w - 4 * 6 * U) / 5, bh = 32 * U, by = std::max(y + 4 * U, m.bottom() - 150 * U);
        const std::string mode = jstr(d, "mode");
        auto send = [&](const std::string& type, const Json& args, const std::string& ok) {
            Json a = args;
            a["id"] = c.meId();
            a["target"] = w.data["id"];
            const Json r = c.cmd({{"op", "cmd"}, {"type", type}, {"args", a}});
            const std::string rr = jstr(r, "result");
            if (rr.empty() || rr == "ok") {
                c.toast(ok);
                w.data["give"] = Json::object();
                w.data["take"] = Json::object();
            } else c.toast(jstr(r, "text").empty() ? rr : jstr(r, "text"));
            w.data["at"] = now() - 2100;
        };
        int k = 0;
        auto btn = [&](const char* key, bool on) {
            const bool hit = g.button(ui::widgetId("trade.btn", k), {m.x + k * (bw + 6 * U), by, bw, bh}, c.fit(c.L(key), bw - 8 * U, 13 * U), on);
            ++k;
            return hit;
        };
        if (btn("ui2d.trade.offer", any))
            send("trade", {{"mode", mode}, {"give", give}, {"take", take}}, c.L("ui2d.trade.sent", {{"mode", jstr(d, "mode_name").empty() ? mode : jstr(d, "mode_name")}}));
        if (btn("ui2d.trade.bargain", counter)) send("trade_accept", Json::object(), c.L("ui2d.trade.bargain"));
        if (btn("ui2d.trade.gift", !give.empty()))
            send("trade", {{"mode", "gift"}, {"give", give}, {"take", Json::object()}}, c.L("ui2d.trade.sent", {{"mode", c.L("ui2d.trade.gift")}}));
        if (btn("ui2d.trade.clear", true)) {
            w.data["give"] = Json::object();
            w.data["take"] = Json::object();
            loadTrade(c, w);
        }
        if (btn("ui2d.trade.cancel", true)) w.closing = true;
        y = by + bh + 8 * U;
    }
    const std::string info = hoverPrice.empty() ? c.L("ui2d.trade.hint") : hoverPrice;
    for (const auto& r : c.wrap(info, m.w, 12 * U)) {
        if (y > m.bottom() - 60 * U) break;
        c.text(m.x, y, r, hoverPrice.empty() ? pal::dim : pal::gold, 12 * U);
        y += 17 * U;
    }
    if (!jarr(d, "last").empty()) {
        std::vector<std::string> ls{c.L("ui2d.trade.last") + ":"};
        for (const auto& l : jarr(d, "last"))
            if (l.is_string()) ls.push_back(l.get<std::string>());
        if (y < m.bottom() - 40 * U) c.parchLines(m.x, y + 4 * U, m.w, ls, 12 * U);
    }
}

} // namespace

void openTalk(Ctx& c, Screens& s, long long target, bool trade) {
    if (target < 0) target = c.selected();
    if (!canTalk(c, target)) return;
    auto w = std::make_unique<Win>();
    w->width = 0.84f;
    w->height = 0.86f;
    if (!trade) {
        w->id = "talk";
        w->data = {{"id", target}, {"tone", ""}, {"sphere", ""}};
        loadTalk(c, *w);
        talkTitle(c, *w);
        w->render = [&s](Ctx& c, Win& w, const ui::Rect& body) { renderTalk(c, s, w, body); };
        w->onPoll = [](Ctx& c, Win& w) {
            if (now() - jnum(w.data, "at") > 1200) loadTalk(c, w);
        };
    } else {
        w->id = "trade";
        w->data = {{"id", target}, {"give", Json::object()}, {"take", Json::object()}};
        loadTrade(c, *w);
        w->render = [](Ctx& c, Win& w, const ui::Rect& body) { renderTrade(c, w, body); };
        w->onPoll = [](Ctx& c, Win& w) {
            if (now() - jnum(w.data, "at") > 2500) loadTrade(c, w);
        };
    }
    s.push(std::move(w));
}

} // namespace game2d
