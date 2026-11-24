// altera_game_2d - the 2D client (gdd/ui/client_2d/CLIENT_2D.md): the Altera
// life-sim seen from above, tiles 16x16, people as layered pawns.
//   altera_game_2d [--seed N] [--season spring|summer|autumn|winter] [--world FILE]
//                  [--content DIR] [--loc DIR] [--lang ru|en] [--port 8766]
//                  [--no-browser] [--run-hours H] [--paused] [--observe] [--autosave-min M]
// AL-99: the page opens on the main menu: a new game (seed, world size, season,
// then a person of a family or a newcomer with a backstory), saves and loads
// (src/client2d/session2d). The world on start is a game already begun, so
// Continue goes straight in.
// One process: the sim steps on the main thread between HTTP requests (no
// locks); the page (tools/game_2d/web/, read from disk on every request, so it
// can be edited without a rebuild) draws the map from /api/grid, polls
// /api/state and sends /api/cmd. It reads the sim only through src/client2d
// (snapshot -> cells, people) and acts on it only through commands.
// Builds on Windows and Linux (the cloud sim build: cmake --preset sim-linux).

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
using socket_t = SOCKET;
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <csignal>
using socket_t = int;
#define INVALID_SOCKET (-1)
#define CLOSESOCK ::close
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <iostream>
#include <sstream>
#include <string>

#include "client2d/acts2d.hpp"
#include "client2d/grid2d.hpp"
#include "client2d/sheet2d.hpp"
#include "client2d/talk2d.hpp"
#include "client2d/session2d.hpp"
#include "client2d/view2d.hpp"
#include "sim/ai.hpp"
#include "sim/building.hpp"
#include "sim/loc.hpp"
#include "sim/speech_feed.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;
namespace c2 = altera::client2d;

namespace {

struct App {
    std::string root, webDir, spritesDir;
    World* world = nullptr;
    c2::ViewText text;
    c2::GroundRules ground = c2::GroundRules::defaults();
    bool paused = false;
    double tps = 2.0;   // sim ticks per real second (1 tick = 10 game minutes)
    double lastStepMs = 0.0;
    std::string lastResult;
    c2::ActCatalog acts;        // AL-96: the act menu's catalog
    SpeechFeed speech;          // AL-96: what people said (lines) and mutter (barks)
    c2::TalkCatalog talk;       // AL-98: the talk window's spheres and chances
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    // AL-99: the session (new game, saves)
    const Content* content = nullptr;
    Json baseCfg;               // the world file
    Json wheel;                 // content/ui/wheel.json
    std::unique_ptr<World> owned;
    c2::NewGame game;
    double runHours = 7.0;
    std::filesystem::path saveDir;
    int autosaveMin = 10;       // 0: off
    std::chrono::steady_clock::time_point lastAutosave = std::chrono::steady_clock::now();
    bool quit = false;
};

// AL-99: a new world in play: the act catalog and the talk feed start over.
void adopt(App& a, std::unique_ptr<World> w) {
    a.owned = std::move(w);
    a.world = a.owned.get();
    a.acts = c2::ActCatalog::make(*a.world, a.wheel);
    a.speech = SpeechFeed{};
    a.speech.poll(*a.world, *a.text.loc, a.text.lang);   // old talk stays unshown
    a.lastResult.clear();
    a.lastAutosave = std::chrono::steady_clock::now();
}

// The player's person (player 1), kNone when the player only watches.
EntityId controlled(const World& w) {
    for (const auto& [id, c] : w.characters())
        if (c.controller.isPlayer() && c.controller.playerId == 1) return id;
    return kNone;
}

Json speechJson(App& a, const Vec2* near) {
    Json out = Json::array();
    auto push = [&](const SpeechEvent& e, bool bark) {
        out.push_back({{"who", e.speaker}, {"to", e.listener}, {"line", e.line}, {"full", e.full}, {"bark", bark}});
    };
    for (const auto& e : a.speech.poll(*a.world, *a.text.loc, a.text.lang)) push(e, false);
    if (near) {
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - a.started).count();
        for (const auto& e : a.speech.barks(*a.world, *a.text.loc, a.text.lang, near->x, near->y, 60.0, sec,
                                            controlled(*a.world)))
            push(e, true);
    }
    return out;
}

std::string readFile(const std::string& p, bool* ok = nullptr) {
    std::ifstream f(p, std::ios::binary);
    if (ok) *ok = bool(f);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string queryParam(const std::string& path, const std::string& key) {
    const size_t q = path.find('?');
    if (q == std::string::npos) return "";
    std::stringstream ss(path.substr(q + 1));
    std::string kv;
    while (std::getline(ss, kv, '&')) {
        const size_t e = kv.find('=');
        if (e != std::string::npos && kv.substr(0, e) == key) return kv.substr(e + 1);
    }
    return "";
}

int intParam(const std::string& path, const std::string& key, int fallback) {
    const std::string v = queryParam(path, key);
    if (v.empty()) return fallback;
    try {
        return std::stoi(v);
    } catch (...) {
        return fallback;
    }
}

std::string mimeOf(const std::string& p) {
    auto ends = [&](const char* s) {
        const size_t n = std::strlen(s);
        return p.size() >= n && p.compare(p.size() - n, n, s) == 0;
    };
    if (ends(".html")) return "text/html; charset=utf-8";
    if (ends(".js")) return "text/javascript; charset=utf-8";
    if (ends(".css")) return "text/css; charset=utf-8";
    if (ends(".json")) return "application/json; charset=utf-8";
    if (ends(".svg")) return "image/svg+xml";
    if (ends(".png")) return "image/png";
    return "application/octet-stream";
}

Json stateJson(App& a, EntityId sel, const Vec2* near, double r) {
    Json j = c2::clockJson(*a.world);
    j["paused"] = a.paused;
    j["tps"] = a.tps;
    j["step_ms"] = a.lastStepMs;
    j["last_result"] = a.lastResult;
    j["people"] = c2::peopleJson(*a.world, a.text, near, r);
    j["me"] = controlled(*a.world);
    j["speech"] = speechJson(a, near);
    if (sel != kNone) j["sel"] = c2::personJson(*a.world, a.text, sel);
    return j;
}

std::string handleCmd(App& a, const Json& body) {
    const std::string op = body.value("op", std::string());
    World& w = *a.world;
    if (op == "pause") a.paused = body.value("on", !a.paused);
    else if (op == "speed") a.tps = std::clamp(body.value("tps", 2.0), 0.25, 600.0);
    else if (op == "step") {
        const int n = std::clamp(body.value("ticks", 1), 1, int(kTicksPerDay * 30));
        for (int i = 0; i < n; ++i) w.step();
    } else if (op == "cmd") {
        Command cmd;
        cmd.source = "player:1";
        cmd.type = body.value("type", std::string());
        cmd.args = body.value("args", Json::object());
        cmd.args["player"] = 1;
        const Command r = w.applyNow(cmd);
        a.lastResult = cmd.type + ": " + (r.result.empty() ? "ok" : r.result);
        return r.result == "ok" ? std::string() : r.result;
    } else if (op == "say_topic") {
        // AL-98: the player's person brings a topic of the talk window up
        const std::string why = c2::sayTopic(w, controlled(w), body.value("target", kNone), body.value("index", -1));
        a.lastResult = "say_topic: " + (why.empty() ? std::string("ok") : why);
        return why.empty() ? "ok" : "rejected:" + why;
    } else return "unknown op";
    return "ok";
}

// --- AL-99: the session ------------------------------------------------------
Json okJson(App& a, const std::string& r) { return {{"result", r}, {"text", r == "ok" ? "" : c2::resultText(a.text, r)}}; }

// The page's game state as a save keeps it: the new game's choices and the view.
Json gameJson(App& a, const Json& view) {
    Json g = a.game.toJson();
    g["tps"] = a.tps;
    if (view.is_object()) g["view"] = view;
    return g;
}

bool autosave(App& a) {
    std::string why;
    a.lastAutosave = std::chrono::steady_clock::now();
    if (controlled(*a.world) == kNone) return false;   // a game not begun is not saved
    if (!c2::writeSave2d(a.saveDir, 0, *a.world, gameJson(a, Json()), &why, &a.text)) {
        std::cerr << "autosave: " << why << "\n";
        return false;
    }
    return true;
}

void respond(socket_t s, int code, const std::string& type, const std::string& body);

// True when `path` was one of the session's routes (and is answered).
bool handleSession(App& a, socket_t s, const std::string& method, const std::string& path, const std::string& body) {
    auto route = [&](const char* p) { return path.rfind(p, 0) == 0; };
    auto reply = [&](const Json& j) { respond(s, 200, "application/json", j.dump()); };
    const Json q = method == "POST" ? Json::parse(body.empty() ? "{}" : body) : Json::object();
    if (route("/api/menu")) {
        reply({{"me", controlled(*a.world)},
               {"game", a.game.toJson()},
               {"paused", a.paused},
               {"options", c2::newGameOptions(*a.content, a.text, a.game)},
               {"saves", c2::listSaves2d(a.saveDir)},
               {"autosave_min", a.autosaveMin},
               {"save_dir", a.saveDir.string()}});
    } else if (route("/api/saves")) {
        reply(c2::listSaves2d(a.saveDir));
    } else if (route("/api/dynasties")) {
        reply({{"me", controlled(*a.world)}, {"families", c2::dynastiesJson(*a.world, a.text)}});
    } else if (method != "POST") {
        return false;
    } else if (route("/api/newgame")) {
        const c2::NewGame g = c2::NewGame::fromJson(q, a.game.seed);
        std::cout << "new game: seed " << g.seed << ", " << g.size << (g.season.empty() ? "" : ", " + g.season) << std::endl;
        try {
            adopt(a, c2::makeWorld2d(*a.content, a.baseCfg, g, a.runHours));
        } catch (const std::exception& e) {
            reply(okJson(a, std::string("rejected:") + e.what()));
            return true;
        }
        a.game = g;
        a.paused = true;   // the world waits while the player picks whom to be
        reply({{"result", "ok"}, {"game", g.toJson()}});
    } else if (route("/api/begin")) {
        const std::string r = c2::beginAs(*a.world, q);
        if (r == "ok") {
            a.paused = false;
            a.lastAutosave = std::chrono::steady_clock::now();
        }
        reply(okJson(a, r));
    } else if (route("/api/save")) {
        const int slot = q.value("slot", 1);
        std::string why;
        if (slot <= 0) why = "bad slot";   // slot 0 is the autosave's
        else if (c2::writeSave2d(a.saveDir, slot, *a.world, gameJson(a, q.value("view", Json())), &why, &a.text)) why.clear();
        else if (why.empty()) why = "cannot write";
        if (!why.empty()) std::cerr << "save " << slot << ": " << why << "\n";
        reply({{"result", why.empty() ? "ok" : "rejected:" + why}, {"text", why}, {"saves", c2::listSaves2d(a.saveDir)}});
    } else if (route("/api/load")) {
        const int slot = q.value("slot", 1);
        try {
            const Json save = c2::readSave2d(a.saveDir, slot);
            adopt(a, c2::loadWorld2d(*a.content, save));
            const Json g = save.value("game", Json::object());
            a.game = c2::NewGame::fromJson(g, a.game.seed);
            a.tps = std::clamp(g.value("tps", a.tps), 0.25, 600.0);
            a.paused = false;
            std::cout << "loaded slot " << slot << std::endl;
            reply({{"result", "ok"}, {"view", g.value("view", Json::object())}});
        } catch (const std::exception& e) {
            // the world in play stays as it was
            std::cerr << "load " << slot << ": " << e.what() << "\n";
            reply({{"result", std::string("rejected:") + e.what()}, {"text", e.what()}});
        }
    } else if (route("/api/delete")) {
        const int slot = q.value("slot", -1);
        reply({{"result", slot > 0 && c2::deleteSave2d(a.saveDir, slot) ? "ok" : "rejected:bad slot"},
               {"saves", c2::listSaves2d(a.saveDir)}});
    } else if (route("/api/quit")) {
        if (q.value("autosave", true)) autosave(a);
        a.quit = true;
        reply({{"result", "ok"}});
    } else return false;
    return true;
}

// --- tiny HTTP ---------------------------------------------------------------
void sendAll(socket_t s, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        const int n = int(send(s, data.data() + off, int(std::min<size_t>(data.size() - off, 1 << 20)), 0));
        if (n <= 0) return;
        off += size_t(n);
    }
}

void respond(socket_t s, int code, const std::string& type, const std::string& body) {
    const std::string h = "HTTP/1.1 " + std::to_string(code) + (code == 200 ? " OK" : " Error") +
                          "\r\nContent-Type: " + type + "\r\nCache-Control: no-store\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    sendAll(s, h + body);
}

// A file under `dir` by the request's rest of path; no way out of it.
void serveFile(socket_t s, const std::string& dir, std::string rest) {
    const size_t q = rest.find('?');
    if (q != std::string::npos) rest = rest.substr(0, q);
    if (rest.empty() || rest.find("..") != std::string::npos || rest.find('\\') != std::string::npos) {
        respond(s, 404, "text/plain", "not found");
        return;
    }
    bool ok = false;
    const std::string body = readFile(dir + "/" + rest, &ok);
    if (!ok) respond(s, 404, "text/plain", "not found: " + rest);
    else respond(s, 200, mimeOf(rest), body);
}

void serveClient(App& a, socket_t s) {
    std::string req;
    char buf[8192];
    size_t headerEnd = std::string::npos, contentLen = 0;
    for (;;) {
        const int n = int(recv(s, buf, sizeof buf, 0));
        if (n <= 0) break;
        req.append(buf, size_t(n));
        if (headerEnd == std::string::npos) {
            headerEnd = req.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                std::string lower = req.substr(0, headerEnd);
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char ch) { return char(std::tolower(ch)); });
                const size_t cl = lower.find("content-length:");
                if (cl != std::string::npos) contentLen = std::stoul(lower.substr(cl + 15));
            }
        }
        if (headerEnd != std::string::npos && req.size() >= headerEnd + 4 + contentLen) break;
    }
    if (headerEnd == std::string::npos) return;
    std::istringstream line(req.substr(0, req.find("\r\n")));
    std::string method, path;
    line >> method >> path;
    const std::string body = req.substr(headerEnd + 4);
    try {
        if (path == "/" || path.rfind("/?", 0) == 0) {
            serveFile(s, a.webDir, "index.html");
        } else if (path.rfind("/web/", 0) == 0) {
            serveFile(s, a.webDir, path.substr(5));
        } else if (path.rfind("/sprites/", 0) == 0) {
            serveFile(s, a.spritesDir, path.substr(9));
        } else if (path.rfind("/api/static", 0) == 0) {
            respond(s, 200, "application/json", c2::staticJson(*a.world, a.text).dump());
        } else if (path.rfind("/api/grid", 0) == 0) {
            const int x = intParam(path, "x", 0), y = intParam(path, "y", 0);
            const int w = std::clamp(intParam(path, "w", 64), 1, 256), h = std::clamp(intParam(path, "h", 64), 1, 256);
            respond(s, 200, "application/json", c2::toJson(c2::buildGrid(*a.world, a.ground, x, y, w, h)).dump());
        } else if (path.rfind("/api/state", 0) == 0) {
            const std::string sel = queryParam(path, "sel");
            EntityId id = kNone;
            if (!sel.empty()) try {
                    id = EntityId(std::stoull(sel));
                } catch (...) {
                }
            // beasts only round the view (vx, vy, vr metres); people always
            Vec2 near{double(intParam(path, "vx", 0)), double(intParam(path, "vy", 0))};
            const int vr = intParam(path, "vr", 0);
            respond(s, 200, "application/json", stateJson(a, id, vr > 0 ? &near : nullptr, vr).dump());
        } else if (path.rfind("/api/cmd", 0) == 0 && method == "POST") {
            std::string r;
            try {
                r = handleCmd(a, Json::parse(body.empty() ? "{}" : body));
            } catch (const std::exception& e) {
                r = std::string("rejected:") + e.what();
            }
            respond(s, 200, "application/json", Json{{"result", r}, {"text", c2::resultText(a.text, r)}}.dump());
        } else if (path.rfind("/api/chronicle", 0) == 0) {
            respond(s, 200, "application/json",
                    c2::chronicleJson(*a.world, a.text, std::size_t(std::max(0, intParam(path, "since", 0)))).dump());
        } else if (path.rfind("/api/sheet", 0) == 0) {
            // AL-100: the character sheet (src/client2d/sheet2d)
            respond(s, 200, "application/json", c2::sheetJson(*a.world, a.text, EntityId(std::max(0, intParam(path, "id", 0)))).dump());
        } else if (path.rfind("/api/acts", 0) == 0) {
            // AL-96: the act menu of the player's person on a thing / person / beast (none: on oneself)
            const std::string tq = queryParam(path, "target");
            EntityId target = kNone;
            if (!tq.empty()) try {
                    target = EntityId(std::stoull(tq));
                } catch (...) {
                }
            respond(s, 200, "application/json", c2::actsJson(*a.world, a.text, a.acts, controlled(*a.world), target).dump());
        } else if (path.rfind("/api/bar", 0) == 0 && method == "POST") {
            // AL-96: the quick bar's slots checked for the player's person
            const Json q = Json::parse(body.empty() ? "{}" : body);
            respond(s, 200, "application/json",
                    c2::barJson(*a.world, a.text, controlled(*a.world), q.value("target", kNone), q.value("slots", Json::array()))
                        .dump());
        } else if (path.rfind("/api/talk", 0) == 0) {
            // AL-98: the talk window with a person
            EntityId target = kNone;
            try {
                target = EntityId(std::stoull(queryParam(path, "target")));
            } catch (...) {
            }
            respond(s, 200, "application/json",
                    c2::talkJson(*a.world, a.text, a.acts, a.talk, controlled(*a.world), target, queryParam(path, "tone")).dump());
        } else if (path.rfind("/api/trade", 0) == 0 && method == "POST") {
            // AL-98: the trade window, the draft {target, give, take} weighed
            const Json q = Json::parse(body.empty() ? "{}" : body);
            respond(s, 200, "application/json",
                    c2::tradeJson(*a.world, a.text, controlled(*a.world), q.value("target", kNone),
                                  q.value("give", Json::object()), q.value("take", Json::object()))
                        .dump());
        } else if (path.rfind("/api/", 0) == 0 && handleSession(a, s, method, path, body)) {
            // AL-99: menu, new game, saves
        } else {
            respond(s, 404, "text/plain", "not found");
        }
    } catch (const std::exception& e) {
        respond(s, 500, "text/plain", e.what());
    }
}

} // namespace

int main(int argc, char** argv) {
    App app;
    app.root = ALTERA_SIM_SOURCE_DIR;
    std::string contentDir, worldFile, locDir, season, lang = "ru";
    std::uint64_t seed = 7;
    int port = 8766;
    double& runHours = app.runHours;   // the world starts at midnight; the view opens in the morning
    bool openBrowser = true;
    bool observe = false;   // --observe: watch only, nobody is led
    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << argv[i] << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (!std::strcmp(argv[i], "--root")) app.root = next();
        else if (!std::strcmp(argv[i], "--content")) contentDir = next();
        else if (!std::strcmp(argv[i], "--world")) worldFile = next();
        else if (!std::strcmp(argv[i], "--seed")) seed = std::stoull(next());
        else if (!std::strcmp(argv[i], "--season")) season = next();
        else if (!std::strcmp(argv[i], "--loc")) locDir = next();
        else if (!std::strcmp(argv[i], "--lang")) lang = next();
        else if (!std::strcmp(argv[i], "--port")) port = std::stoi(next());
        else if (!std::strcmp(argv[i], "--run-hours")) runHours = std::stod(next());
        else if (!std::strcmp(argv[i], "--paused")) app.paused = true;
        else if (!std::strcmp(argv[i], "--no-browser")) openBrowser = false;
        else if (!std::strcmp(argv[i], "--observe")) observe = true;
        else if (!std::strcmp(argv[i], "--autosave-min")) app.autosaveMin = std::max(0, std::stoi(next()));
        else {
            std::cerr << "unknown argument " << argv[i] << "\n";
            return 2;
        }
    }
    if (contentDir.empty()) contentDir = app.root + "/content/ai";
    if (worldFile.empty()) worldFile = app.root + "/content/sim/test_world.json";
    if (locDir.empty()) locDir = app.root + "/content/loc";
    app.webDir = app.root + "/tools/game_2d/web";
    app.spritesDir = app.root + "/content/sprites_2d";
    try {
        std::cout << "loading content..." << std::endl;
        const Content content = Content::load(contentDir);
        const Loc loc = Loc::load(locDir);
        app.text.loc = &loc;
        app.text.lang = loc.hasLanguage(lang) ? lang : loc.base();
        bool haveRules = false;
        const std::string rulesText = readFile(app.spritesDir + "/map2d.json", &haveRules);
        if (haveRules) app.ground = c2::GroundRules::fromJson(Json::parse(rulesText).value("ground", Json::object()));
        app.content = &content;
        app.baseCfg = loadJsonFile(worldFile);
        app.saveDir = c2::saveDir2d();
        bool haveWheel = false;
        const std::string wheel = readFile(app.root + "/content/ui/wheel.json", &haveWheel);
        app.talk = c2::TalkCatalog::make(haveWheel ? Json::parse(wheel) : Json());
        app.wheel = haveWheel ? Json::parse(wheel) : Json();
        app.game.seed = seed;
        if (app.baseCfg.contains("size_m"))   // the world file's own size stays unless a size is picked
            app.game.size = app.baseCfg.value("size_m", 12000.0) <= 9000 ? "small" : app.baseCfg.value("size_m", 12000.0) >= 15000 ? "large" : "medium";
        app.game.season = season;
        std::cout << "generating world, seed " << seed << "..." << std::endl;
        {
            Json cfg = app.baseCfg;
            if (!cfg.contains("village_site")) cfg["village_plan"] = true;   // roads, lots, houses of pieces
            if (!season.empty()) {
                cfg.erase("start_day");
                cfg["season"] = season;
            }
            auto w = std::make_unique<World>(makeTestWorld(content, cfg, seed));
            installAi(*w);
            w->run(Tick(runHours * kTicksPerDay / 24));
            adopt(app, std::move(w));
        }
        World& world0 = *app.world;
        // AL-96: the player leads the head of the village (the mandate holder), as the 3D game starts
        if (!observe)
            for (const auto& [sid, st] : world0.settlements()) {
                if (st.mode != "full") continue;
                EntityId who = mandateHolder(world0, sid);
                if (who == kNone && !st.families.empty())
                    if (const auto* f = &world0.families().at(st.families.front()); !f->members.empty()) who = f->members.front();
                if (who != kNone) {
                    Command take;
                    take.source = "player:1";
                    take.type = "take_control";
                    take.args = {{"id", who}, {"player", 1}};
                    world0.applyNow(take);
                }
                break;
            }
        app.speech.poll(world0, loc, app.text.lang);   // old talk stays unshown

#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) throw std::runtime_error("WSAStartup failed");
#else
        std::signal(SIGPIPE, SIG_IGN);
#endif
        socket_t ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<unsigned short>(port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        int yes = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
        if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(ls, 16) != 0)
            throw std::runtime_error("cannot listen on 127.0.0.1:" + std::to_string(port) + " (already running?)");
        const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";
        std::cout << "Altera 2D: " << url << "  (" << world0.characters().size() << " characters; Ctrl+C to quit)"
                  << std::endl;
#ifdef _WIN32
        if (openBrowser) ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
        (void)openBrowser;
#endif
        using Clock = std::chrono::steady_clock;
        auto lastTick = Clock::now();
        double owed = 0.0;
        while (!app.quit) {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(ls, &rd);
            timeval tv{0, 2000};
            if (select(int(ls) + 1, &rd, nullptr, nullptr, &tv) > 0) {
                socket_t cs = accept(ls, nullptr, nullptr);
                if (cs != INVALID_SOCKET) {
#ifdef _WIN32
                    DWORD to = 2000;
                    setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
#else
                    timeval to{2, 0};
                    setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
#endif
                    serveClient(app, cs);
#ifdef _WIN32
                    shutdown(cs, SD_SEND);
#else
                    shutdown(cs, SHUT_WR);
#endif
                    CLOSESOCK(cs);
                }
            }
            const auto now = Clock::now();
            const double dt = std::chrono::duration<double>(now - lastTick).count();
            lastTick = now;
            if (app.autosaveMin > 0 && now - app.lastAutosave > std::chrono::minutes(app.autosaveMin)) autosave(app);
            if (app.paused) {
                owed = 0.0;
                continue;
            }
            owed = std::min(owed + dt * app.tps, 2000.0);
            const auto budgetEnd = Clock::now() + std::chrono::milliseconds(100);
            while (owed >= 1.0 && Clock::now() < budgetEnd) {
                const auto t0 = Clock::now();
                app.world->step();
                app.lastStepMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                owed -= 1.0;
            }
            if (owed > 1.0 && Clock::now() >= budgetEnd) owed = std::min(owed, 1.0 + app.tps * 0.1);
        }
        std::cout << "quit" << std::endl;
        CLOSESOCK(ls);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
