// The main menu and what it leads to, in the window (port of web/menu.js):
// Continue, New game (seed, world size, season -> the world is made -> whom to
// be: a person of a family or a newcomer with a backstory, or only watch),
// Save, Load (slots 1-6 and the autosave 0), Quit. And the chronicle (J).
// Routes: /api/menu, /api/newgame, /api/dynasties, /api/begin, /api/save,
// /api/load, /api/delete, /api/quit, /api/chronicle.
#include <ctime>
#include <random>

#include "native/ctx2d.hpp"

namespace game2d {

namespace {

std::string randomSeed() {
    static std::mt19937 rng{std::random_device{}()};
    return std::to_string(rng() % 1000000000u);
}

std::string sizeName(const Json& M, const std::string& id) {
    const Json o = M.value("options", Json::object());   // kept: jarr of a temporary dangles
    for (const auto& s : jarr(o, "sizes"))
        if (jstr(s, "id") == id) return jstr(s, "name");
    return id;
}

std::string when(double sec) {
    if (sec <= 0) return "";
    const std::time_t t = std::time_t(sec);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%d.%m %H:%M", &tm);
    return b;
}

// a "working" body while a long call runs behind the next frame
bool busy(Ctx& c, Win& w, const ui::Rect& body) {
    const std::string b = jstr(w.data, "busy");
    if (b.empty()) return false;
    c.textC(body.x + body.w / 2, body.y + body.h / 2 - 20 * c.u(), b, pal::gold, 20 * c.u(), true);
    return true;
}

// a big menu button
bool big(Ctx& c, const char* id, int i, const ui::Rect& r, const std::string& label, bool on = true) {
    return c.gui().button(ui::widgetId(id, i), r, label, on);
}

void openMain(Ctx& c, Screens& s);
void openNew(Ctx& c, Screens& s);
void openPick(Ctx& c, Screens& s);
void openSaves(Ctx& c, Screens& s, const std::string& mode);

// --- the main menu -------------------------------------------------------------
void openMain(Ctx& c, Screens& s) {
    auto w = std::make_unique<Win>();
    w->id = "menu";
    w->title = c.L("ui2d.menu.title");
    w->width = 0.30f;
    w->height = 0.62f;
    w->autoFit = true;   // LR-143
    w->data = c.get("/api/menu");
    w->render = [&s](Ctx& c, Win& w, const ui::Rect& body) {
        const float U = c.u();
        const Json& M = w.data;
        const bool inGame = jid(M, "me") > 0;
        float y = body.y + 10 * U;
        c.textC(body.x + body.w / 2, y, c.L("ui2d.menu.crest"), pal::gold, 34 * U, true);
        y += 46 * U;
        const Json g = M.value("game", Json::object());
        c.textC(body.x + body.w / 2, y, c.L("ui2d.menu.world", {{"seed", jstr(g, "seed")}, {"size", sizeName(M, jstr(g, "size"))}}), pal::dim, 14 * U);
        y += 34 * U;
        bool anySave = false;
        for (const auto& sv : jarr(M, "saves")) anySave = anySave || jflag(sv, "used");
        const float bw = std::min(body.w, 320 * U), bx = body.x + (body.w - bw) / 2, bh = 42 * U;
        int i = 0;
        auto item = [&](const char* key, bool on) {
            const bool hit = big(c, "menu.item", i++, {bx, y, bw, bh}, c.L(std::string("ui2d.menu.") + key), on);
            y += bh + 8 * U;
            return hit;
        };
        if (item("continue", inGame)) w.closing = true;
        if (item("new", true)) {
            w.closing = true;
            openNew(c, s);
        }
        if (item("save", inGame)) {
            w.closing = true;
            openSaves(c, s, "save");
        }
        if (item("load", anySave)) {
            w.closing = true;
            openSaves(c, s, "load");
        }
        if (item("quit", true)) c.quitGame();
        c.textC(body.x + body.w / 2, y + 10 * U, c.L("ui2d.menu.keys"), pal::dim, 12 * U);   // LR-143: under the buttons, the window hugs them
    };
    s.push(std::move(w));
}

// --- new game ----------------------------------------------------------------------
void openNew(Ctx& c, Screens& s) {
    auto w = std::make_unique<Win>();
    w->id = "new";
    w->title = c.L("ui2d.ng.title");
    w->width = 0.56f;
    w->height = 0.72f;
    w->autoFit = true;   // LR-143
    w->data = {{"M", c.get("/api/menu")}, {"seed", randomSeed()}, {"season", ""}};
    w->data["size"] = jstr(w->data["M"].value("options", Json::object()).value("defaults", Json::object()), "size");
    w->render = [&s](Ctx& c, Win& w, const ui::Rect& body) {
        if (busy(c, w, body)) return;
        const float U = c.u();
        ui::Ui& g = c.gui();
        const Json o = w.data["M"].value("options", Json::object());
        float y = body.y;
        c.text(body.x, y, c.L("ui2d.ng.seed"), pal::gold, 16 * U, true);
        y += 26 * U;
        std::string seed = jstr(w.data, "seed");
        if (g.textField(ui::widgetId("ng.seed"), {body.x, y, std::min(360 * U, body.w - 60 * U), 30 * U}, seed, "", 40)) w.data["seed"] = seed;
        if (g.button(ui::widgetId("ng.dice"), {body.x + std::min(360 * U, body.w - 60 * U) + 8 * U, y, 44 * U, 30 * U}, "?")) w.data["seed"] = randomSeed();
        y += 36 * U;
        c.text(body.x, y, c.L("ui2d.ng.seed.hint"), pal::dim, 12 * U);
        y += 28 * U;
        c.text(body.x, y, c.L("ui2d.ng.size"), pal::gold, 16 * U, true);
        y += 26 * U;
        const Json& sizes = jarr(o, "sizes");
        const float cw = (body.w - 8 * U * (sizes.size() - 1)) / std::max<std::size_t>(1, sizes.size()), ch = 96 * U;
        int i = 0;
        for (const auto& sz : sizes) {
            const ui::Rect r{body.x + i * (cw + 8 * U), y, cw, ch};
            const bool on = jstr(sz, "id") == jstr(w.data, "size");
            if (g.selectable(ui::widgetId("ng.size", i), r, on)) w.data["size"] = jstr(sz, "id");
            c.fill(r.x, r.y, r.w, r.h, on ? pal::leather : pal::leatherD.withAlpha(0.8f));
            c.frame(r.x, r.y, r.w, r.h, on ? pal::gold : pal::bronze, on ? 2 * U : U);
            c.text(r.x + 10 * U, r.y + 8 * U, jstr(sz, "name"), on ? pal::gold : pal::text, 16 * U, true);
            c.text(r.x + 10 * U, r.y + 32 * U,
                   c.fit(c.L("ui2d.ng.size.km", {{"km", std::to_string(int(jnum(sz, "size_m") / 1000))}, {"f", std::to_string(int(jnum(sz, "families")))}}),
                         r.w - 20 * U, 13 * U),
                   pal::text, 13 * U);
            const auto rows = c.wrap(jstr(sz, "desc"), r.w - 20 * U, 12 * U);
            for (std::size_t k = 0; k < rows.size() && k < 2; ++k) c.text(r.x + 10 * U, r.y + 54 * U + k * 17 * U, rows[k], pal::dim, 12 * U);
            ++i;
        }
        y += ch + 16 * U;
        c.text(body.x, y, c.L("ui2d.ng.season"), pal::gold, 16 * U, true);
        y += 26 * U;
        Json seasons = Json::array({{{"id", ""}, {"name", c.L("ui2d.ng.season.any")}}});
        for (const auto& se : jarr(o, "seasons")) seasons.push_back(se);
        float x = body.x;
        i = 0;
        for (const auto& se : seasons) {
            const float bw = c.textW(jstr(se, "name"), 14 * U) + 28 * U;
            if (g.toggle(ui::widgetId("ng.season", i++), {x, y, bw, 30 * U}, jstr(se, "name"), jstr(se, "id") == jstr(w.data, "season")))
                w.data["season"] = jstr(se, "id");
            x += bw + 6 * U;
        }
        const float bw = 200 * U, bh = 42 * U, by = body.bottom() - bh;
        if (big(c, "ng.back", 0, {body.x, by, bw, bh}, c.L("ui2d.menu.back"))) {
            w.closing = true;
            openMain(c, s);
        }
        if (g.button(ui::widgetId("ng.make"), {body.right() - bw, by, bw, bh}, c.L("ui2d.ng.make"), pal::gold, pal::ink)) {
            w.data["busy"] = c.L("ui2d.ng.making");
            Win* wp = &w;
            s.later([&c, &s, wp] {
                std::string seed = jstr(wp->data, "seed");
                while (!seed.empty() && seed.back() == ' ') seed.pop_back();
                const Json r = c.post("/api/newgame", {{"seed", seed}, {"size", jstr(wp->data, "size")}, {"season", jstr(wp->data, "season")}});
                if (jstr(r, "result") != "ok") {
                    c.toast(jstr(r, "text").empty() ? jstr(r, "result") : jstr(r, "text"));
                    if (s.isOpen("new")) wp->data.erase("busy");
                    return;
                }
                wp->closing = true;
                c.worldChanged(Json::object());
                openPick(c, s);
            });
        }
    };
    s.push(std::move(w));
}

// --- whom to be --------------------------------------------------------------------
void begin(Ctx& c, Win& w, const Json& args) {
    const Json r = c.post("/api/begin", args);
    if (jstr(r, "result") != "ok") {
        c.toast(jstr(r, "text").empty() ? jstr(r, "result") : jstr(r, "text"));
        return;
    }
    w.closing = true;
    if (args.contains("id")) c.select(args["id"].get<long long>());
    c.setFollow(!args.contains("watch"));
    c.refresh();
}

void openPick(Ctx& c, Screens& s) {
    auto w = std::make_unique<Win>();
    w->id = "pick";
    w->title = c.L("ui2d.pick.title");
    w->tabs = {{"kin", c.L("ui2d.pick.kin")}, {"newcomer", c.L("ui2d.pick.newcomer")}};
    w->tab = "kin";
    w->width = 0.80f;
    w->height = 0.84f;
    w->autoFit = true;   // LR-143
    w->data = {{"M", c.get("/api/menu")}, {"fams", c.get("/api/dynasties").value("families", Json::array())}, {"id", -1}, {"back", ""}};
    // the head of the full village is offered first, as the game would pick
    for (const auto& f : w->data["fams"])
        for (const auto& p : jarr(f, "people"))
            if (w->data["id"].get<long long>() < 0 && jflag(f, "full") && jflag(p, "chief")) w->data["id"] = jid(p, "id");
    w->render = [&s](Ctx& c, Win& w, const ui::Rect& body) {
        const float U = c.u();
        ui::Ui& g = c.gui();
        const float bh = 42 * U;
        const ui::Rect area{body.x, body.y, body.w, body.h - bh - 10 * U};
        std::string who;
        const long long pid = w.data["id"].get<long long>();
        if (w.tab == "kin") {
            c.text(area.x, area.y, c.L("ui2d.pick.kin.hint"), pal::dim, 13 * U);
            const ui::Rect sr{area.x, area.y + 24 * U, area.w, area.h - 24 * U};
            const ui::Rect in = g.beginScroll(ui::widgetId("pick.kin"), sr);
            // families in columns of cards
            const float colW = 300 * U, gap = 10 * U;
            const int cols = std::max(1, int((in.w + gap) / (colW + gap)));
            const float cw = (in.w - gap * (cols - 1)) / cols;
            std::vector<float> colY(std::size_t(cols), in.y);
            int i = 0;
            for (const auto& f : w.data["fams"]) {
                std::size_t col = std::size_t(std::min_element(colY.begin(), colY.end()) - colY.begin());
                const float x = in.x + col * (cw + gap);
                float y = colY[col];
                const Json& people = jarr(f, "people");
                const float h = 48 * U + people.size() * 44 * U;
                c.fill(x, y, cw, h, pal::leather);
                c.frame(x, y, cw, h, pal::bronze, U);
                std::string head = jstr(f, "dynasty");
                if (head.find(jstr(f, "name")) == std::string::npos) head += "  " + jstr(f, "name");
                c.text(x + 8 * U, y + 6 * U, c.fit(head, cw - 16 * U, 16 * U), pal::gold, 16 * U, true);
                std::string sub = jstr(f, "settlement");
                if (!jflag(f, "full")) sub += " · " + c.L("ui2d.pick.far");
                c.text(x + 8 * U, y + 26 * U, c.fit(sub, cw - 16 * U, 12 * U), pal::dim, 12 * U);
                float yy = y + 46 * U;
                for (const auto& p : people) {
                    const long long id = jid(p, "id");
                    const bool adult = jflag(p, "adult");
                    const ui::Rect r{x + 4 * U, yy, cw - 8 * U, 40 * U};
                    if (g.selectable(ui::widgetId("pick.who", i++), r, id == pid) && adult) {
                        w.data["id"] = id;
                        c.select(id);
                        if (const Json* pp = c.person(id)) c.lookAt(jnum(*pp, "x"), jnum(*pp, "y"));
                    }
                    if (id == pid) c.frame(r.x, r.y, r.w, r.h, pal::gold, std::max(1.0f, U));
                    c.face(id, jstr(p, "name"), r.x + 2 * U, r.y + 2 * U, 36 * U);
                    c.text(r.x + 44 * U, r.y + 3 * U, c.fit(jstr(p, "name"), r.w - 50 * U, 14 * U), adult ? pal::text : pal::dim, 14 * U);
                    std::string tags = c.L("ui2d.pick.age", {{"n", std::to_string(int(jnum(p, "age")))}});
                    if (jflag(p, "chief")) tags += " · " + c.L("ui2d.pick.chief");
                    if (jflag(p, "head")) tags += " · " + c.L("ui2d.pick.head");
                    if (!adult) tags += " · " + c.L("ui2d.pick.child");
                    c.text(r.x + 44 * U, r.y + 21 * U, c.fit(tags, r.w - 50 * U, 12 * U), pal::dim, 12 * U);
                    if (id == pid) who = jstr(p, "name");
                    yy += 44 * U;
                }
                colY[col] = y + h + gap;
            }
            g.endScroll(*std::max_element(colY.begin(), colY.end()));
        } else {
            c.text(area.x, area.y, c.L("ui2d.pick.newcomer.hint"), pal::dim, 13 * U);
            const ui::Rect sr{area.x, area.y + 24 * U, area.w, area.h - 24 * U};
            const ui::Rect in = g.beginScroll(ui::widgetId("pick.backs"), sr);
            const float gap = 10 * U;
            const int cols = std::max(1, int((in.w + gap) / (340 * U + gap)));
            const float cw = (in.w - gap * (cols - 1)) / cols, ch = 150 * U;
            int i = 0;
            float bottom = in.y;
            const Json opts = w.data["M"].value("options", Json::object());
            for (const auto& b : jarr(opts, "backstories")) {
                const float x = in.x + (i % cols) * (cw + gap), y = in.y + (i / cols) * (ch + gap);
                const ui::Rect r{x, y, cw, ch};
                const bool on = jstr(b, "id") == jstr(w.data, "back");
                if (g.selectable(ui::widgetId("pick.back", i), r, on)) w.data["back"] = jstr(b, "id");
                if (on) who = jstr(b, "name");
                c.fill(r.x, r.y, r.w, r.h, on ? pal::leather : pal::leatherD.withAlpha(0.85f));
                c.frame(r.x, r.y, r.w, r.h, on ? pal::gold : pal::bronze, on ? 2 * U : U);
                c.text(r.x + 8 * U, r.y + 6 * U, c.fit(jstr(b, "name"), r.w - 16 * U, 16 * U), on ? pal::gold : pal::text, 16 * U, true);
                const auto rows = c.wrap(jstr(b, "desc"), r.w - 16 * U, 12 * U);
                for (std::size_t k = 0; k < rows.size() && k < 2; ++k) c.text(r.x + 8 * U, r.y + 30 * U + k * 16 * U, rows[k], pal::dim, 12 * U);
                std::vector<std::string> tags;
                for (const auto& sk : jarr(b, "skills")) tags.push_back(jstr(sk, "name") + " " + std::to_string(int(std::lround(jnum(sk, "v") * 100))));
                for (const auto& t : jarr(b, "traits"))
                    if (t.is_string()) tags.push_back(t.get<std::string>());
                c.chips(r.x + 8 * U, r.y + 66 * U, r.w - 16 * U, tags);
                std::string gear;
                for (const auto& gg : jarr(b, "gear")) gear += (gear.empty() ? "" : ", ") + jstr(gg, "name") + (jnum(gg, "n") > 1 ? " ×" + std::to_string(int(jnum(gg, "n"))) : "");
                if (jnum(b, "coins") > 0) gear += (gear.empty() ? "" : ", ") + c.L("ui2d.pick.coins", {{"n", std::to_string(int(jnum(b, "coins")))}});
                c.text(r.x + 8 * U, r.bottom() - 22 * U, c.fit(gear, r.w - 16 * U, 12 * U), pal::text, 12 * U);
                bottom = std::max(bottom, r.bottom() + gap);
                ++i;
            }
            g.endScroll(bottom);
        }
        const float bw = 260 * U, by = body.bottom() - bh;
        if (g.button(ui::widgetId("pick.watch"), {body.x, by, bw, bh}, c.L("ui2d.pick.watch"))) begin(c, w, {{"watch", true}});
        const std::string go = who.empty() ? c.L("ui2d.pick.none") : c.L(w.tab == "kin" ? "ui2d.pick.go" : "ui2d.pick.create", {{"who", who}});
        const float gw = std::max(bw, c.textW(go, 15 * U) + 40 * U);
        if (g.button(ui::widgetId("pick.go"), {body.right() - gw, by, gw, bh}, go, pal::gold, pal::ink, !who.empty())) {
            if (w.tab == "kin") begin(c, w, {{"id", pid}});
            else {
                // AL-136: a newcomer is made on the creation screen (race, look, talents, attributes)
                w.closing = true;
                openCreate(c, s, jstr(w.data, "back"));
            }
        }
    };
    s.push(std::move(w));
}

// --- save / load -------------------------------------------------------------------
void openSaves(Ctx& c, Screens& s, const std::string& mode) {
    auto w = std::make_unique<Win>();
    w->id = "saves";
    w->title = c.L(mode == "save" ? "ui2d.saves.save" : "ui2d.saves.load");
    w->tabs = {{"save", c.L("ui2d.menu.save")}, {"load", c.L("ui2d.menu.load")}};
    w->tab = mode;
    w->width = 0.56f;
    w->height = 0.74f;
    w->autoFit = true;   // LR-143
    w->data = {{"M", c.get("/api/menu")}, {"sure", ""}};
    w->render = [&s](Ctx& c, Win& w, const ui::Rect& body) {
        if (busy(c, w, body)) return;
        w.title = c.L(w.tab == "save" ? "ui2d.saves.save" : "ui2d.saves.load");
        const float U = c.u();
        ui::Ui& g = c.gui();
        Json& M = w.data["M"];
        const bool inGame = jid(M, "me") > 0;
        float y = body.y;
        const float rowH = 58 * U;
        int i = 0;
        std::string doWhat;
        int doSlot = -1;
        for (const auto& sv : jarr(M, "saves")) {
            const int slot = int(jnum(sv, "slot"));
            const bool used = jflag(sv, "used"), autoSlot = slot == 0;
            const ui::Rect r{body.x, y, body.w, rowH};
            c.parchBox(r);
            const std::string title = autoSlot ? c.L("ui2d.saves.auto") : c.L("ui2d.saves.slot", {{"n", std::to_string(slot)}});
            c.text(r.x + 10 * U, r.y + 6 * U, title, pal::ink, 15 * U, false);
            c.text(r.x + 10 * U, r.y + 30 * U, when(jnum(sv, "saved_at")), pal::ink.withAlpha(0.7f), 12 * U);
            const float wx = r.x + 150 * U, ww = r.w - 150 * U - 300 * U;
            if (used) {
                std::string head = jstr(sv, "person").empty() ? c.L("ui2d.saves.nobody") : jstr(sv, "person");
                if (!jstr(sv, "family").empty()) head += " · " + jstr(sv, "family");
                c.text(wx, r.y + 6 * U, c.fit(head, ww, 15 * U), pal::ink, 15 * U);
                const std::string sub = c.L("ui2d.saves.day", {{"n", jstr(sv, "day")}, {"season", c.L("ui2d.season." + jstr(sv, "season"))}}) + " · " +
                                        c.L("ui2d.saves.alive", {{"n", jstr(sv, "alive")}}) + " · " + sizeName(M, jstr(sv, "size")) + " · " +
                                        c.L("ui2d.saves.seed", {{"seed", jstr(sv, "seed")}});
                c.text(wx, r.y + 30 * U, c.fit(sub, ww, 12 * U), pal::ink.withAlpha(0.75f), 12 * U);
            } else c.text(wx, r.y + 18 * U, c.L("ui2d.saves.empty"), pal::ink.withAlpha(0.6f), 14 * U);
            float bx = r.right() - 10 * U;
            const std::string sure = jstr(w.data, "sure");
            const std::string tag = std::to_string(slot);
            if (used && !autoSlot) {
                const float bw = 90 * U;
                bx -= bw;
                if (g.button(ui::widgetId("saves.del", slot), {bx, r.y + 12 * U, bw, 34 * U}, sure == "del:" + tag ? c.L("ui2d.saves.sure") : c.L("ui2d.saves.delete")))
                    doWhat = "del", doSlot = slot;
                bx -= 6 * U;
            }
            if (w.tab == "save" && !autoSlot) {
                const float bw = 170 * U;
                bx -= bw;
                if (g.button(ui::widgetId("saves.save", slot), {bx, r.y + 12 * U, bw, 34 * U},
                             sure == "save:" + tag ? c.L("ui2d.saves.overwrite") : c.L("ui2d.saves.here"), inGame))
                    doWhat = "save", doSlot = slot;
            }
            if (w.tab == "load" && used) {
                const float bw = 170 * U;
                bx -= bw;
                if (g.button(ui::widgetId("saves.load", slot), {bx, r.y + 12 * U, bw, 34 * U}, c.L("ui2d.saves.open"))) doWhat = "load", doSlot = slot;
            }
            y += rowH + 8 * U;
            ++i;
        }
        const int amin = int(jnum(M, "autosave_min"));
        c.text(body.x, y + 6 * U, c.L(amin > 0 ? "ui2d.saves.autonote" : "ui2d.saves.autooff", {{"n", std::to_string(amin)}}), pal::dim, 13 * U);
        if (doWhat.empty()) return;
        // the second click does it (overwrite, delete)
        bool used = false;
        for (const auto& sv : jarr(M, "saves"))
            if (int(jnum(sv, "slot")) == doSlot) used = jflag(sv, "used");
        const std::string key = doWhat + ":" + std::to_string(doSlot);
        if ((doWhat == "del" || (doWhat == "save" && used)) && jstr(w.data, "sure") != key) {
            w.data["sure"] = key;
            return;
        }
        w.data["sure"] = "";
        if (doWhat == "save") {
            const Json r = c.post("/api/save", {{"slot", doSlot}, {"view", {{"x", std::lround(c.camAtX())}, {"y", std::lround(c.camAtY())}, {"zoom", c.zoomLevel()}}}});
            if (r.contains("saves")) M["saves"] = r["saves"];
            if (jstr(r, "result") == "ok") {
                c.toast(c.L("ui2d.saves.done", {{"n", std::to_string(doSlot)}}));
                w.closing = true;
            } else c.toast(c.L("ui2d.saves.failed", {{"why", jstr(r, "text").empty() ? jstr(r, "result") : jstr(r, "text")}}));
        } else if (doWhat == "del") {
            const Json r = c.post("/api/delete", {{"slot", doSlot}});
            if (r.contains("saves")) M["saves"] = r["saves"];
        } else if (doWhat == "load") {
            w.data["busy"] = c.L("ui2d.saves.loading");
            Win* wp = &w;
            s.later([&c, wp, doSlot] {
                const Json r = c.post("/api/load", {{"slot", doSlot}});
                if (jstr(r, "result") != "ok") {
                    c.toast(c.L("ui2d.saves.failed", {{"why", jstr(r, "text").empty() ? jstr(r, "result") : jstr(r, "text")}}));
                    wp->data.erase("busy");
                    return;
                }
                wp->closing = true;
                c.worldChanged(r.value("view", Json::object()));
                c.toast(c.L("ui2d.saves.loaded", {{"n", std::to_string(doSlot)}}));
            });
        }
    };
    s.push(std::move(w));
}

} // namespace

void openMenu(Ctx& c, Screens& s, const std::string& which, const Json& args) {
    if (which == "new") openNew(c, s);
    else if (which == "pick") openPick(c, s);
    else if (which == "saves") openSaves(c, s, args.is_string() ? args.get<std::string>() : "load");
    else openMain(c, s);
}

// --- the chronicle (J): what happened in the village, newest first ---------------
void openChronicle(Ctx& c, Screens& s) {
    auto w = std::make_unique<Win>();
    w->id = "chronicle";
    w->title = c.L("ui2d.screen.chronicle");
    w->width = 0.5f;
    w->height = 0.8f;
    w->autoFit = true;   // LR-143
    w->data = {{"since", 0}, {"list", Json::array()}};
    auto load = [](Ctx& c, Win& w) {
        const Json r = c.get("/api/chronicle?since=" + std::to_string(w.data["since"].get<long long>()));
        for (const auto& e : jarr(r, "list")) w.data["list"].push_back(e);
        while (w.data["list"].size() > 600) w.data["list"].erase(0);
        w.data["since"] = (long long)jnum(r, "n", double(w.data["since"].get<long long>()));
    };
    load(c, *w);
    w->onPoll = [load](Ctx& c, Win& w) {
        if (SDL_GetTicks() - w.polledAt > 3000) {
            w.polledAt = double(SDL_GetTicks());
            load(c, w);
        }
    };
    w->render = [](Ctx& c, Win& w, const ui::Rect& body) {
        const float U = c.u();
        ui::Ui& g = c.gui();
        const Json& list = w.data["list"];
        if (list.empty()) {
            c.parchLines(body.x, body.y, body.w, {c.L("ui2d.chronicle.empty")}, 14 * U);
            return;
        }
        const ui::Rect in = g.beginScroll(ui::widgetId("chron"), body);
        float y = in.y;
        long long day = -1;
        int i = 0;
        for (auto it = list.rbegin(); it != list.rend(); ++it) {
            const Json& e = *it;
            if ((long long)jnum(e, "day") != day) {
                day = (long long)jnum(e, "day");
                y += 6 * U;
                c.text(in.x, y, c.L("ui2d.day", {{"n", std::to_string(day + 1)}}), pal::gold, 15 * U, true);
                y += 24 * U;
            }
            const std::string line = jstr(e, "t") + "  " + jstr(e, "text");
            const auto rows = c.wrap(line, in.w - 16 * U, 13 * U);
            const float h = rows.size() * (c.textH(13 * U) + 5 * U) + 8 * U;
            const ui::Rect r{in.x, y, in.w, h};
            const Json& who = jarr(e, "who");
            if (g.selectable(ui::widgetId("chron.row", i++), r, false) && !who.empty() && who[0].is_number()) c.select(who[0].get<long long>());
            c.parchLines(in.x, y, in.w, {line}, 13 * U);
            y += h + 4 * U;
        }
        g.endScroll(y);
    };
    s.push(std::move(w));
}

} // namespace game2d
