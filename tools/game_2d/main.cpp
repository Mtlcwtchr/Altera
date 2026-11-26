// lr_game_2d - the 2D client (gdd/ui/client_2d/CLIENT_2D.md): the LivelyRealms
// life-sim seen from above, tiles 16x16, people as layered pawns.
//   lr_game_2d [--seed N] [--season spring|summer|autumn|winter] [--world FILE]
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
#undef near  // minwindef.h defines near as an empty macro; the code names variables near
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
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>
#include <deque>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

#include "client2d/acts2d.hpp"
#ifdef LR_GAME2D_NATIVE
#include "native/window2d.hpp"
#endif
#include "client2d/craft2d.hpp"
#include "client2d/farm2d.hpp"
#include "client2d/relief2d.hpp"
#include "client2d/build2d.hpp"
#include "client2d/grid2d.hpp"
#include "client2d/sheet2d.hpp"
#include "client2d/culture2d.hpp"
#include "client2d/sky2d.hpp"
#include "client2d/talk2d.hpp"
#include "client2d/session2d.hpp"
#include "client2d/view2d.hpp"
#include "client2d/worldmap2d.hpp"
#include "client2d/lobby2d.hpp"
#include "net2d.hpp"
#include "sim/ai.hpp"
#include "sim/data_fs.hpp"
#include "sim/building.hpp"
#include "sim/loc.hpp"
#include "sim/social_chain.hpp"
#include "sim/speech_feed.hpp"
#include "sim/combat_show.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;
namespace c2 = lr::client2d;

namespace {

constexpr int kNetProto = 1;   // LR-160: the routes as clients of another build expect them

struct App {
    std::string root, webDir, spritesDir;
    World* world = nullptr;
    c2::ViewText text;
    c2::GroundRules ground = c2::GroundRules::defaults();
    c2::CropRules crops;   // AL-112: fields and gardens (content/sprites_2d/crops2d.json)
    c2::ReliefRules relief;   // LR-156: cliffs (content/sprites_2d/cliffs2d.json)
    bool paused = false;
    double tps = 2.0;   // sim ticks per real second (1 tick = 10 game minutes)
    double lastStepMs = 0.0;
    std::map<int, std::string> lastResult;   // LR-160: per player
    // LR-152: the combat state: while the led person's fight plays in the window the world waits
    std::chrono::steady_clock::time_point holdUntil{};
    c2::ActCatalog acts;        // AL-96: the act menu's catalog
    std::map<int, SpeechFeed> speech;   // AL-96: what people said (lines) and mutter (barks); LR-160: a feed per player
    c2::TalkCatalog talk;       // AL-98: the talk window's spheres and chances
    Json craftUi;               // AL-97: content/ui/craft.json (the helper's wishes)
    std::map<int, EntityId> colonyOf;   // AL-94: colony mode - the person handed to free will; the player plans as them (LR-160: per player)
    // LR-157: an NPC asks the player's person or talks with them in a talk the player leads:
    // the world waits (as a dialogue in an RPG) until it is answered or over
    bool holdTalks = true;      // off with --observe
    long long talkHeld = 0;     // the ask/talk the world was paused for
    bool talkPaused = false;    // the pause is the talk's own (lifted when it ends)
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    // AL-99: the session (new game, saves)
    const Content* content = nullptr;
    // LR-142: hot reload. Content and loc read again keep the old ones alive (a world, a cached
    // pointer may still hold them); the client JSON is read again in place.
    std::string contentDir, locDir;
    std::vector<std::unique_ptr<Content>> reloadedContent;
    std::vector<std::unique_ptr<Loc>> reloadedLoc;
    bool hotReload = true;
    std::chrono::steady_clock::time_point lastDataCheck{};
    Json baseCfg;               // the world file
    Json wheel;                 // content/ui/wheel.json
    Json sky;                   // AL-110: content/sprites_2d/sky2d.json
    std::unique_ptr<World> owned;
    c2::NewGame game;
    double runHours = 7.0;
    std::filesystem::path saveDir;
    int autosaveMin = 10;       // 0: off
    std::chrono::steady_clock::time_point lastAutosave = std::chrono::steady_clock::now();
    bool quit = false;
    // LR-160: multiplayer. The world is the server's; every client is a player of the lobby.
    c2::Lobby lobby;
    bool loopback = true;      // the request in hand comes from this computer (the window, a local page)
    bool menuPaused = false;   // the pause is a menu's hold (lifted when the menu closes)
    bool hasWindow = false;    // the host's own window is player 1
    int port = 8766;
    std::string bind = "0.0.0.0";
    std::deque<Json> events;   // shared acts (pause, speed, new game, load, joins) for the others' toasts
    long long eventSeq = 0;
    long long worldGen = 0;    // a new world in play (new game, load): every client starts over on it
    std::chrono::steady_clock::time_point lastSweep{};
};

double nowSec(const App& a) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - a.started).count(); }

// LR-160: a shared act of the player in hand (or of the server, player 0), for everyone's toast.
void shared(App& a, const std::string& what, const Json& extra = Json::object()) {
    const c2::NetPlayer* p = a.lobby.find(a.text.player);
    Json e = {{"seq", ++a.eventSeq}, {"by", a.text.player}, {"name", p ? p->name : std::string()}, {"what", what}, {"tps", a.tps}};
    for (const auto& [k, v] : extra.items()) e[k] = v;
    a.events.push_back(e);
    while (a.events.size() > 16) a.events.pop_front();
}

// AL-99: a new world in play: the act catalog and the talk feed start over.
void adopt(App& a, std::unique_ptr<World> w) {
    a.owned = std::move(w);
    a.world = a.owned.get();
    a.acts = c2::ActCatalog::make(*a.world, a.wheel);
    a.speech.clear();   // a feed's first poll only marks where the talk ends: old talk stays unshown
    a.lastResult.clear();
    a.colonyOf.clear();
    a.lobby.worldChanged();
    a.menuPaused = false;
    ++a.worldGen;
    a.lastAutosave = std::chrono::steady_clock::now();
}

// The person of the player this request is from (a.text.player), kNone when they only watch.
EntityId controlled(const App& a) { return c2::controlledBy(*a.world, a.text.player); }

// Who plans and runs the colony: the led person, or in colony mode the one handed to free will.
EntityId planner(const App& a) {
    const EntityId me = controlled(a);
    if (me != kNone) return me;
    const auto it = a.colonyOf.find(a.text.player);
    const Character* c = it != a.colonyOf.end() ? a.world->character(it->second) : nullptr;
    return c && c->body.alive ? it->second : kNone;
}

// LR-157: the ask or the led talk of the player's person (0: none), one key for both phases
long long talkKey(const World& w, int player) {
    const Character* me = w.character(c2::controlledBy(w, player));
    if (!me) return 0;
    const EntityId with = lr::sim::pendingInvite(*me) != kNone ? lr::sim::pendingInvite(*me) : lr::sim::ledTalkWith(*me);
    return with == kNone ? 0 : (long long)(with) * 1000003LL + (long long)me->ai.social.since + 1;
}

// Pauses the world for a new ask/talk, lifts the pause once it is over; true when it paused now.
bool holdForTalk(App& a) {
    if (!a.holdTalks || !a.world) return false;
    // LR-160: the world waits for a talk only for a player alone in it; with others in, it goes on
    const double now = nowSec(a);
    if (a.lobby.onlineCount(now) > 1) {
        if (a.talkPaused) a.paused = a.talkPaused = false;
        a.talkHeld = 0;
        return false;
    }
    int solo = 1;
    for (const auto& p : a.lobby.all())
        if (a.lobby.online(p, now)) solo = p.id;
    const long long k = talkKey(*a.world, solo);
    if (k != 0 && k != a.talkHeld) {
        a.talkHeld = k;
        if (!a.paused) a.paused = a.talkPaused = true;
        return a.talkPaused;
    }
    if (k == 0 && a.talkHeld != 0) {
        a.talkHeld = 0;
        if (a.talkPaused) a.paused = a.talkPaused = false;
    }
    return false;
}

Json speechJson(App& a, const Vec2* near) {
    Json out = Json::array();
    auto push = [&](const SpeechEvent& e, bool bark) {
        out.push_back({{"who", e.speaker}, {"to", e.listener}, {"line", e.line}, {"full", e.full}, {"bark", bark}});
    };
    for (const auto& e : a.speech[a.text.player].poll(*a.world, *a.text.loc, a.text.lang)) push(e, false);
    if (near) {
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - a.started).count();
        for (const auto& e : a.speech[a.text.player].barks(*a.world, *a.text.loc, a.text.lang, near->x, near->y, 60.0, sec,
                                            controlled(a)))
            push(e, true);
    }
    return out;
}

// LR-142: content files come through the data layers (mods, the data folder beside the exe);
// a JSON file is the layered JSON ("_merge" layers merged), anything else the top layer's file.
std::string readFile(const std::string& p, bool* ok = nullptr) {
    const bool json = p.size() > 5 && p.compare(p.size() - 5, 5, ".json") == 0;
    if (json && data::exists(p)) {
        try {
            const std::string out = data::readJson(p).dump();
            if (ok) *ok = true;
            return out;
        } catch (const std::exception& e) {
            std::cerr << "data: " << e.what() << "\n";
            if (ok) *ok = false;
            return {};
        }
    }
    std::string out;
    const bool have = data::readText(p, out);
    if (ok) *ok = have;
    return out;
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

Json stateJson(App& a, EntityId sel, const Vec2* near, double r, std::uint64_t fsSince = 0) {
    Json j = c2::clockJson(*a.world);
    j["paused"] = a.paused;
    j["tps"] = a.tps;
    j["world_gen"] = a.worldGen;   // LR-160
    // AL-110: the sky of this tick and the next (the window blends between them)
    static const char* forced = std::getenv("LR_2D_WEATHER");   // frames: clear|cloudy|rain|storm|snow|sleet|fog|snowy
    const std::string wx = forced ? forced : "";
    j["sky"] = c2::skyJson(c2::withWeather(c2::skyAt(*a.world, a.sky, a.world->now()), a.sky, wx));
    j["sky_next"] = c2::skyJson(c2::withWeather(c2::skyAt(*a.world, a.sky, a.world->now() + 1), a.sky, wx));
    j["step_ms"] = a.lastStepMs;
    j["last_result"] = a.lastResult[a.text.player];
    j["people"] = c2::peopleJson(*a.world, a.text, near, r);
    j["me"] = controlled(a);
    j["speech"] = speechJson(a, near);
    if (sel != kNone) j["sel"] = c2::personJson(*a.world, a.text, sel);
    // LR-137: the hero frame shows the led person even while another one is selected
    if (const EntityId m = controlled(a); m != kNone && m != sel) j["me_card"] = c2::personJson(*a.world, a.text, m);
    // LR-157: who asks the player's person (the window opens the talk with them), who talks with them
    if (const Character* mc = a.world->character(controlled(a))) {
        if (const EntityId who = lr::sim::pendingInvite(*mc); who != kNone) {
            const lr::sim::SocialLink& l = mc->ai.social;
            j["invite"] = {{"who", who}, {"act", l.chain}, {"since", l.since}};
        }
        if (const EntityId with = lr::sim::ledTalkWith(*mc); with != kNone) j["talk_with"] = with;
    }
    // AL-115: fire, smoke and magic as point lights / fx for the 2D renderer (src/sim/world_fields.hpp)
    j["world_fx"] = lr::sim::fieldsViewJson(*a.world, near, r);
    // LR-160: who plays, who you are, what the others did to the shared things
    j["net"] = {{"you", a.text.player}, {"players", a.lobby.toJson(*a.world, a.text, nowSec(a))}, {"events", Json(a.events)},
                {"menu_pause", a.menuPaused}};
    // LR-152: fights played in full since the client's last one (their rounds, for the combat state)
    j["fights"] = lr::sim::fightsSince(*a.world, fsSince, near ? near->x : 0, near ? near->y : 0, near ? r : 0);
    j["holding"] = std::chrono::steady_clock::now() < a.holdUntil;
    return j;
}

std::string handleCmd(App& a, const Json& body) {
    const std::string op = body.value("op", std::string());
    World& w = *a.world;
    const int pl = a.text.player;
    if (op == "pause") {
        // LR-160: the pause is everyone's. A menu's pause (menu: true) holds the world only for a
        // player alone in it; with others in, opening the menu does not stop their game.
        const bool on = body.value("on", !a.paused);
        if (body.value("menu", false)) {
            if (on && !a.paused && a.lobby.onlineCount(nowSec(a)) <= 1) a.paused = a.menuPaused = true;
            else if (!on && a.menuPaused) a.paused = a.menuPaused = false;
        } else {
            a.paused = on;
            a.menuPaused = false;
            a.talkPaused = false;
        }
        if (!body.value("menu", false)) shared(a, on ? "pause" : "resume");
    } else if (op == "speed") {
        a.tps = std::clamp(body.value("tps", 2.0), 0.25, 600.0);
        shared(a, "speed");
    } else if (op == "hold") {
        // LR-152: the window plays the led person's fight; the world waits that long (alone in it only:
        // with others in, their game goes on, as with the menu's pause)
        const double ms = std::clamp(body.value("ms", 0.0), 0.0, 120000.0);
        if (a.lobby.onlineCount(nowSec(a)) <= 1)
            a.holdUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(long(ms));
    } else if (op == "step") {
        const int n = std::clamp(body.value("ticks", 1), 1, int(kTicksPerDay * 30));
        for (int i = 0; i < n; ++i) w.step();
    } else if (op == "cmd") {
        Command cmd;
        cmd.source = "player:" + std::to_string(pl);
        cmd.type = body.value("type", std::string());
        cmd.args = body.value("args", Json::object());
        cmd.args["player"] = pl;   // a client acts only as itself
        const Command r = w.applyNow(cmd);
        a.lastResult[pl] = cmd.type + ": " + (r.result.empty() ? "ok" : r.result);
        return r.result == "ok" ? std::string() : r.result;
    } else if (op == "say_topic") {
        // AL-98: the player's person brings a topic of the talk window up
        const std::string why = c2::sayTopic(w, controlled(a), body.value("target", kNone), body.value("index", -1));
        a.lastResult[pl] = "say_topic: " + (why.empty() ? std::string("ok") : why);
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
    g["players"] = a.lobby.save();   // LR-160: who led whom, for after a restart
    if (view.is_object()) g["view"] = view;
    return g;
}

bool autosave(App& a) {
    std::string why;
    a.lastAutosave = std::chrono::steady_clock::now();
    // a game not begun (nobody has led anyone yet) is not saved
    if (!a.world->moduleState("player_house")) {
        bool any = false;
        for (const auto& [id, c] : a.world->characters()) any = any || c.controller.isPlayer();
        if (!any) return false;
    }
    if (!c2::writeSave2d(a.saveDir, 0, *a.world, gameJson(a, Json()), &why, &a.text)) {
        std::cerr << "autosave: " << why << "\n";
        return false;
    }
    return true;
}

// The answer to one request, sent over the socket by serveClient or handed to
// the native window (tools/game_2d/native) as it is.
struct Reply {
    int code = 200;
    std::string type = "text/plain", body;
};
void respond(Reply& s, int code, const std::string& type, const std::string& body);

// True when `path` was one of the session's routes (and is answered).
bool handleSession(App& a, Reply& s,const std::string& method, const std::string& path, const std::string& body) {
    auto route = [&](const char* p) { return path.rfind(p, 0) == 0; };
    auto reply = [&](const Json& j) { respond(s, 200, "application/json", j.dump()); };
    const Json q = method == "POST" ? Json::parse(body.empty() ? "{}" : body) : Json::object();
    if (route("/api/hello")) {
        // LR-160: a client checks it reached a LivelyRealms game and learns who it is
        const c2::NetPlayer* p = a.lobby.find(a.text.player);
        reply({{"game", "LivelyRealms"}, {"proto", kNetProto}, {"player", a.text.player}, {"name", p ? p->name : ""}});
    } else if (route("/api/net")) {
        // LR-160: the network window: where others join, who is in
        Json addrs = Json::array();
        for (const auto& ip : net2d::localAddresses()) addrs.push_back(ip + ":" + std::to_string(a.port));
        reply({{"port", a.port}, {"bind", a.bind}, {"addrs", addrs}, {"you", a.text.player}, {"max", a.lobby.maxPlayers},
               {"players", a.lobby.toJson(*a.world, a.text, nowSec(a))}});
    } else if (route("/api/menu")) {
        reply({{"me", controlled(a)},
               {"players_online", a.lobby.onlineCount(nowSec(a))},
               {"game", a.game.toJson()},
               {"paused", a.paused},
               {"options", c2::newGameOptions(*a.content, a.text, a.game)},
               {"saves", c2::listSaves2d(a.saveDir)},
               {"autosave_min", a.autosaveMin},
               {"save_dir", a.saveDir.string()}});
    } else if (route("/api/saves")) {
        reply(c2::listSaves2d(a.saveDir));
    } else if (route("/api/dynasties")) {
        reply({{"me", controlled(a)}, {"families", c2::dynastiesJson(*a.world, a.text)}});
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
        shared(a, "newgame");
        reply({{"result", "ok"}, {"game", g.toJson()}});
    } else if (route("/api/begin")) {
        const std::string r = c2::beginAs(*a.world, q, a.text.player);
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
            a.lobby.load(g.value("players", Json::array()), nowSec(a));   // LR-160: their persons wait for them
            shared(a, "load", {{"slot", slot}});
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
        // LR-160: the game is the server's: only this computer ends it; another client only leaves
        if (!a.loopback || a.text.player != 1) {
            a.lobby.leave(a.text.player, nowSec(a));
            std::cout << "player " << a.text.player << " left" << std::endl;
            reply({{"result", "ok"}, {"left", true}});
            return true;
        }
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

void respond(Reply& s, int code, const std::string& type, const std::string& body) {
    s.code = code;
    s.type = type;
    s.body = body;
}

void sendReply(socket_t s, const Reply& r) {
    const std::string h = "HTTP/1.1 " + std::to_string(r.code) + (r.code == 200 ? " OK" : " Error") +
                          "\r\nContent-Type: " + r.type + "\r\nCache-Control: no-store\r\nContent-Length: " +
                          std::to_string(r.body.size()) + "\r\nConnection: close\r\n\r\n";
    sendAll(s, h + r.body);
}

// A file under `dir` by the request's rest of path; no way out of it.
void serveFile(Reply& s,const std::string& dir, std::string rest) {
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

void dispatch(App& a, Reply& s, const std::string& method, const std::string& path, const std::string& body);

// A header's value (name in lower case), "" when absent.
std::string headerOf(const std::string& head, const std::string& lowerHead, const std::string& name) {
    const size_t at = lowerHead.find("\r\n" + name + ":");
    if (at == std::string::npos) return "";
    const size_t b = at + 2 + name.size() + 1, e = head.find("\r\n", b);
    std::string v = head.substr(b, e == std::string::npos ? std::string::npos : e - b);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    return v;
}

// %xx and + of a query value
std::string urlDecode(const std::string& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '+') out += ' ';
        else if (v[i] == '%' && i + 2 < v.size()) {
            out += char(std::strtol(v.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else out += v[i];
    }
    return out;
}

// LR-160: who a request is from. The window of this process is player 1 (Host::call); a
// socket's client names itself by its token (header X-LR-Player or ?lrp=), a new token joins.
// False when the game is full.
bool identify(App& a, const std::string& token, const std::string& name, const std::string& kind, bool loopback) {
    a.loopback = loopback;
    bool joined = false;
    const int id = a.lobby.touch("n" + token, name, kind, nowSec(a), &joined);   // "n": a socket's token never names the window
    if (id <= 0) return false;
    a.text.player = id;
    if (joined) {
        const c2::NetPlayer* p = a.lobby.find(id);
        std::cout << "player " << id << " (" << (p ? p->name : "") << ", " << kind << ") joined" << std::endl;
        shared(a, "join");
    }
    return true;
}

void serveClient(App& a, socket_t s, bool loopback) {
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
                if (cl != std::string::npos) contentLen = std::min<size_t>(std::stoul(lower.substr(cl + 15)), 16u << 20);
            }
        }
        if (headerEnd != std::string::npos && req.size() >= headerEnd + 4 + contentLen) break;
        if (req.size() > (17u << 20)) return;   // no request is this big
    }
    if (headerEnd == std::string::npos) return;
    std::istringstream line(req.substr(0, req.find("\r\n")));
    std::string method, path;
    line >> method >> path;
    const std::string body = req.substr(headerEnd + 4);
    const std::string head = req.substr(0, headerEnd);
    std::string lower = head;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    std::string token = headerOf(head, lower, "x-lr-player"), name = urlDecode(headerOf(head, lower, "x-lr-name"));
    std::string kind = headerOf(head, lower, "x-lr-client");
    if (token.empty()) token = queryParam(path, "lrp");
    if (name.empty()) name = urlDecode(queryParam(path, "lrn"));
    Reply rep;
    // files of the page are anyone's; the routes are a player's
    const bool api = path.rfind("/api/", 0) == 0;
    if (api && !identify(a, token, name, kind.empty() ? "web" : kind, loopback)) {
        respond(rep, 503, "application/json", Json{{"result", "rejected:full"}, {"max", a.lobby.maxPlayers}}.dump());
    } else dispatch(a, rep, method, path, body);
    sendReply(s, rep);
    a.text.player = a.hasWindow ? 1 : a.text.player;
    a.loopback = true;
}

// One request of the page or of the native window (tools/game_2d/native): the
// same routes either way, so both clients see one game.
void dispatch(App& a, Reply& s, const std::string& method, const std::string& path, const std::string& body) {
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
            c2::Grid2d grid = c2::buildGrid(*a.world, a.ground, x, y, w, h);
            c2::paintCrops(*a.world, a.crops, grid);   // AL-112
            c2::paintRelief(*a.world, a.relief, grid); // LR-156
            respond(s, 200, "application/json", c2::toJson(grid).dump());
        } else if (path.rfind("/api/fields", 0) == 0) {
            // AL-115: one field of the cells as bytes for an overlay (?f=heat&x=&y=&w=&h=)
            lr::sim::Field f = lr::sim::Field::heat;
            lr::sim::fieldFromName(queryParam(path, "f"), f);
            respond(s, 200, "application/json",
                    lr::sim::fieldRasterJson(*a.world, f, intParam(path, "x", 0), intParam(path, "y", 0), intParam(path, "w", 64),
                                                 intParam(path, "h", 64))
                        .dump());
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
            const int fs = intParam(path, "fs", -1);   // LR-152: fights after this id (-1: none, only the newest id)
            Json st = stateJson(a, id, vr > 0 ? &near : nullptr, vr, fs < 0 ? ~std::uint64_t{0} : std::uint64_t(fs));
            st["fight_last"] = a.world->client.nextFight - 1;
            respond(s, 200, "application/json", st.dump());
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
        } else if (path.rfind("/api/dynasty", 0) == 0) {
            // LR-145: the dynasty window (src/client2d/sheet2d dynastyJson)
            respond(s, 200, "application/json", c2::dynastyJson(*a.world, a.text, controlled(a)).dump());
        } else if (path.rfind("/api/worldmap", 0) == 0) {
            // LR-153: the world map window (src/client2d/worldmap2d)
            respond(s, 200, "application/json", c2::worldMapJson(*a.world, a.text, controlled(a)).dump());
        } else if (path.rfind("/api/culture", 0) == 0) {
            // LR-155: the culture and faith window (src/client2d/culture2d)
            respond(s, 200, "application/json",
                    c2::cultureJson(*a.world, a.text, controlled(a), EntityId(std::max(0, intParam(path, "id", 0))),
                                    queryParam(path, "faith"), queryParam(path, "culture"))
                        .dump());
        } else if (path.rfind("/api/inspect", 0) == 0) {
            // LR-155: "inspect" - what the led person and the dynasty know of someone
            respond(s, 200, "application/json",
                    c2::inspectJson(*a.world, a.text, controlled(a), EntityId(std::max(0, intParam(path, "id", 0)))).dump());
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
            respond(s, 200, "application/json", c2::actsJson(*a.world, a.text, a.acts, controlled(a), target).dump());
        } else if (path.rfind("/api/bar", 0) == 0 && method == "POST") {
            // AL-96: the quick bar's slots checked for the player's person
            const Json q = Json::parse(body.empty() ? "{}" : body);
            respond(s, 200, "application/json",
                    c2::barJson(*a.world, a.text, controlled(a), q.value("target", kNone), q.value("slots", Json::array()))
                        .dump());
        } else if (path.rfind("/api/build/catalog", 0) == 0) {
            // AL-94: the build mode (tools/game_2d/web/build.js): the palette
            respond(s, 200, "application/json", c2::catalogJson(*a.world, a.text).dump());
        } else if (path.rfind("/api/build/plan", 0) == 0) {
            const int x = intParam(path, "x", 0), y = intParam(path, "y", 0);
            const int w = std::clamp(intParam(path, "w", 128), 1, 512), h = std::clamp(intParam(path, "h", 128), 1, 512);
            respond(s, 200, "application/json", c2::planJson(*a.world, a.text, planner(a), x, y, w, h).dump());
        } else if ((path.rfind("/api/build/preview", 0) == 0 || path.rfind("/api/build/place", 0) == 0) && method == "POST") {
            // a tool's stroke: checked as the sim checks it; place lays it into the plan
            const c2::Stroke st = c2::Stroke::fromJson(Json::parse(body.empty() ? "{}" : body));
            const c2::Planner who{planner(a), st.forSettlement, a.text.player};
            const bool place = path.rfind("/api/build/place", 0) == 0;
            respond(s, 200, "application/json",
                    (place ? c2::placeJson(*a.world, a.text, who, st) : c2::previewJson(*a.world, a.text, who, st)).dump());
        } else if (path.rfind("/api/colony", 0) == 0) {
            // AL-94: the colony window (tools/game_2d/web/colony.js)
            if (path.rfind("/api/colony/mode", 0) == 0 && method == "POST") {
                // colony mode: one's person lives on their own, the player runs the settlement
                const int pl = a.text.player;
                const auto it = a.colonyOf.find(pl);
                const EntityId held = it != a.colonyOf.end() ? it->second : kNone;
                const bool on = Json::parse(body.empty() ? "{}" : body).value("on", held == kNone);
                Command c;
                c.source = "player:" + std::to_string(pl);
                c.args = {{"player", pl}};
                if (on && held == kNone && controlled(a) != kNone) {
                    a.colonyOf[pl] = controlled(a);
                    c.type = "release_control";
                    a.world->applyNow(c);
                } else if (!on && held != kNone) {
                    c.type = "take_control";
                    c.args["id"] = held;
                    a.colonyOf.erase(pl);
                    a.world->applyNow(c);
                }
            }
            Json j = c2::colonyJson(*a.world, a.text, planner(a));
            if (j.is_object()) j["colony_mode"] = a.colonyOf.count(a.text.player) > 0;
            respond(s, 200, "application/json", j.dump());
        } else if (path.rfind("/api/talk", 0) == 0) {
            // AL-98: the talk window with a person
            EntityId target = kNone;
            try {
                target = EntityId(std::stoull(queryParam(path, "target")));
            } catch (...) {
            }
            respond(s, 200, "application/json",
                    c2::talkJson(*a.world, a.text, a.acts, a.talk, controlled(a), target, queryParam(path, "tone")).dump());
        } else if (path.rfind("/api/trade", 0) == 0 && method == "POST") {
            // AL-98: the trade window, the draft {target, give, take} weighed
            const Json q = Json::parse(body.empty() ? "{}" : body);
            respond(s, 200, "application/json",
                    c2::tradeJson(*a.world, a.text, controlled(a), q.value("target", kNone),
                                  q.value("give", Json::object()), q.value("take", Json::object()))
                        .dump());
        } else if (path.rfind("/api/workshop", 0) == 0 && method == "POST") {
            // AL-97: the workshop of the player's person (or `id`): {draft, wish, kit, texts}
            const Json q = Json::parse(body.empty() ? "{}" : body);
            EntityId who = q.value("id", controlled(a));
            if (q.value("kit", false))
                if (Character* c = a.world->character(who)) c2::craftKit(*a.world, *c);
            c2::WorkshopQuery wq;
            wq.draft = q.value("draft", Json::object());
            wq.wish = q.value("wish", std::string());
            Json r = {{"ws", c2::workshopJson(*a.world, a.text, a.craftUi, who, wq)}};
            if (q.value("texts", false)) r["texts"] = c2::workshopTexts(a.text);
            respond(s, 200, "application/json", r.dump());
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

// --- LR-160: one window, another computer's game ------------------------------------
// This program again, as a window of the game at `to` (empty: its own game again). The language stays.
bool relaunch(int argc, char** argv, const std::string& to, const std::string& name) {
    std::vector<std::string> args;
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--lang") || !std::strcmp(argv[i], "--root") || !std::strcmp(argv[i], "--port")) {
            args.push_back(argv[i]);
            args.push_back(argv[++i]);
        }
    if (!to.empty()) args.insert(args.end(), {"--connect", to});
    if (!name.empty()) args.insert(args.end(), {"--name", name});
#ifdef _WIN32
    auto wide = [](const std::string& u) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, nullptr, 0);
        std::wstring w(size_t(std::max(n, 1)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, w.data(), n);
        w.resize(size_t(std::max(n - 1, 0)));
        return w;
    };
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
    for (const auto& a : args) cmd += L" \"" + wide(a) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    (void)args;
    return false;   // no window in the Linux build
#endif
}

// The token this computer's window is known by on any server (kept beside the saves).
std::string windowToken() {
    const std::filesystem::path f = c2::saveDir2d().parent_path() / "net_token.txt";
    std::string t;
    if (std::ifstream in(f); in) std::getline(in, t);
    t = c2::cleanToken(t);
    if (t.size() >= 12) return t;
    t = net2d::newToken();
    std::error_code ec;
    std::filesystem::create_directories(f.parent_path(), ec);
    std::ofstream(f) << t << "\n";
    return t;
}

std::string defaultName() {
    for (const char* k : {"LR_PLAYER_NAME", "USERNAME", "USER"})
        if (const char* v = std::getenv(k); v && *v) return c2::cleanName(v);
    return "";
}

// The 2D client's own JSON (content/sprites_2d, content/ui), at start and on a hot reload.
void loadClientData(App& app) {
    bool haveRules = false;
    const std::string rulesText = readFile(app.spritesDir + "/map2d.json", &haveRules);
    app.ground = haveRules ? c2::GroundRules::fromJson(Json::parse(rulesText).value("ground", Json::object()))
                           : c2::GroundRules::defaults();
    bool haveSky = false;
    const std::string skyText = readFile(app.spritesDir + "/sky2d.json", &haveSky);
    app.sky = haveSky ? Json::parse(skyText) : Json::object();
    bool haveCrops = false;
    const std::string cropsText = readFile(app.spritesDir + "/crops2d.json", &haveCrops);
    app.crops = haveCrops ? c2::CropRules::fromJson(Json::parse(cropsText).value("plots", Json::object())) : c2::CropRules{};
    bool haveCliffs = false;
    const std::string cliffsText = readFile(app.spritesDir + "/cliffs2d.json", &haveCliffs);
    app.relief = haveCliffs ? c2::ReliefRules::fromJson(Json::parse(cliffsText).value("rules", Json::object())) : c2::ReliefRules{};
    bool haveWheel = false;
    const std::string wheel = readFile(app.root + "/content/ui/wheel.json", &haveWheel);
    bool haveCraftUi = false;
    const std::string craftUi = readFile(app.root + "/content/ui/craft.json", &haveCraftUi);
    app.craftUi = haveCraftUi ? Json::parse(craftUi) : Json::object();
    app.talk = c2::TalkCatalog::make(haveWheel ? Json::parse(wheel) : Json());
    app.wheel = haveWheel ? Json::parse(wheel) : Json();
}

// LR-142: a content file changed on disk (any layer): read the content, the loc and the client
// JSON again and put them under the running world. A file that does not load keeps the old data.
void reloadData(App& app, const std::vector<std::string>& why) {
    std::cout << "data: " << why.size() << " file(s) changed (" << why.front() << (why.size() > 1 ? ", ..." : "")
              << "), reloading" << std::endl;
    const std::uint64_t gen = data::generation();
    data::reload(); // the modules' caches key on the generation: they read fresh from here on
    try {
        auto fresh = std::make_unique<Content>(Content::load(app.contentDir));
        auto loc = std::make_unique<Loc>(Loc::load(app.locDir));
        loadClientData(app);
        if (!fresh->warnings.empty())
            std::cout << "data: " << fresh->warnings.size() << " content warning(s), as at the start" << std::endl;
        if (app.world) app.world->rebindContent(*fresh);
        app.content = fresh.get();
        app.text.loc = loc.get();
        if (!loc->hasLanguage(app.text.lang)) app.text.lang = loc->base();
        app.reloadedContent.push_back(std::move(fresh));
        app.reloadedLoc.push_back(std::move(loc));
        if (app.world) app.acts = c2::ActCatalog::make(*app.world, app.wheel);
        std::cout << "data: reloaded (generation " << data::generation() << ", " << data::layers().size()
                  << " mod layer(s))" << std::endl;
    } catch (const std::exception& e) {
        data::restoreGeneration(gen);
        std::cerr << "data: reload failed, the old data stays: " << e.what() << std::endl;
    }
}

// Once a second: did a watched content file change?
void checkData(App& app) {
    if (!app.hotReload) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - app.lastDataCheck < std::chrono::seconds(1)) return;
    app.lastDataCheck = now;
    const std::vector<std::string> ch = data::changed();
    if (!ch.empty()) reloadData(app, ch);
}

int main(int argc, char** argv) {
    App app;
    // LR-142: content/ beside the exe (data/content, content) or above it (a build tree), else the
    // source tree; mods/ folders over it (sim/data_fs.hpp). --root still names a tree outright.
    app.root = std::filesystem::path(data::mountDefault(argc > 0 ? argv[0] : "", std::string(LR_SIM_SOURCE_DIR) + "/content"))
                   .parent_path()
                   .generic_string();
    if (const char* h = std::getenv("LR_HOT_RELOAD"); h && *h == '0') app.hotReload = false;
    std::string contentDir, worldFile, locDir, season, lang = "ru";
    std::uint64_t seed = 7;
    int port = 8766;
    double& runHours = app.runHours;   // the world starts at midnight; the view opens in the morning
    bool openBrowser = true;
    bool web = false;       // --web: the page in the browser (the debug client); --no-browser: the server alone
    bool observe = false;   // --observe: watch only, nobody is led
    bool server = false;    // LR-160 --server: the world alone, no window, no browser (others join it)
    std::string connect;    // LR-160 --connect host[:port]: this window plays the game of another computer
    std::string playerName; // LR-160 --name: how the others see this player
    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << argv[i] << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (!std::strcmp(argv[i], "--root")) {
            app.root = next();
            data::mount(app.root + "/content", data::layers());
        }
        else if (!std::strcmp(argv[i], "--no-hot-reload")) app.hotReload = false;
        else if (!std::strcmp(argv[i], "--content")) contentDir = next();
        else if (!std::strcmp(argv[i], "--world")) worldFile = next();
        else if (!std::strcmp(argv[i], "--seed")) seed = std::stoull(next());
        else if (!std::strcmp(argv[i], "--season")) season = next();
        else if (!std::strcmp(argv[i], "--loc")) locDir = next();
        else if (!std::strcmp(argv[i], "--lang")) lang = next();
        else if (!std::strcmp(argv[i], "--port")) port = std::stoi(next());
        else if (!std::strcmp(argv[i], "--probe")) return net2d::probe(next(), std::cout);   // LR-160: a client's view of a server
        else if (!std::strcmp(argv[i], "--run-hours")) runHours = std::stod(next());
        else if (!std::strcmp(argv[i], "--paused")) app.paused = true;
        else if (!std::strcmp(argv[i], "--no-browser")) openBrowser = false, web = true;
        else if (!std::strcmp(argv[i], "--web")) web = true;
        else if (!std::strcmp(argv[i], "--observe")) observe = true, app.holdTalks = false;
        else if (!std::strcmp(argv[i], "--autosave-min")) app.autosaveMin = std::max(0, std::stoi(next()));
        else if (!std::strcmp(argv[i], "--bind")) app.bind = next();
        else if (!std::strcmp(argv[i], "--server")) server = true, observe = true, openBrowser = false, web = true;
        else if (!std::strcmp(argv[i], "--connect")) connect = next();
        else if (!std::strcmp(argv[i], "--name")) playerName = next();
        else if (!std::strcmp(argv[i], "--max-players")) app.lobby.maxPlayers = std::clamp(std::stoi(next()), 1, 16);
        else if (!std::strcmp(argv[i], "--release-sec")) app.lobby.releaseSec = std::max(1.0, std::stod(next()));
        else {
            std::cerr << "unknown argument " << argv[i] << "\n";
            return 2;
        }
    }
    if (contentDir.empty()) contentDir = app.root + "/content/ai";
    if (worldFile.empty()) worldFile = app.root + "/content/sim/test_world.json";
    if (locDir.empty()) locDir = app.root + "/content/loc";
    app.webDir = app.root + "/tools/game_2d/web";
    if (!std::filesystem::is_directory(app.webDir)) app.webDir = std::string(LR_SIM_SOURCE_DIR) + "/tools/game_2d/web"; // debug page
    app.contentDir = contentDir;
    app.locDir = locDir;
    std::cout << "data: " << data::contentRoot() << (data::layers().empty() ? "" : " + mods:") << std::endl;
    for (const auto& l : data::layers()) std::cout << "  " << l << std::endl;
    app.spritesDir = app.root + "/content/sprites_2d";
#ifdef LR_GAME2D_NATIVE
    // the game's own window (tools/game_2d/native), up before the world is made
    game2d::attachConsole(web);
    std::unique_ptr<game2d::Window> window;
    if (!web) {
        window = std::make_unique<game2d::Window>(app.root);
        if (!window->open()) {
            std::cerr << "no window: " << window->error() << " - the page instead (--web)\n";
            window.reset();
            web = true;
        }
    }
#else
    web = true;
#endif
    if (playerName.empty()) playerName = defaultName();
    if (!connect.empty()) {
        // LR-160: a window of another computer's game; the world steps there, this only draws and asks
        net2d::Addr to;
        if (!net2d::parseAddr(connect, to)) {
            std::cerr << "--connect: bad address " << connect << "\n";
            return 2;
        }
#ifdef LR_GAME2D_NATIVE
        if (!window) {
            std::cerr << "--connect needs the window; a browser joins at http://" << to.host << ":" << to.port << "/\n";
            return 2;
        }
        net2d::Remote remote(to, windowToken(), playerName, "native");
        std::string why;
        if (!remote.hello(&why, kNetProto)) {
            window->fail("LivelyRealms: " + to.host + ":" + std::to_string(to.port) + " - " + why);
            return 1;
        }
        std::cout << "joined " << to.host << ":" << to.port << std::endl;
        bool quit = false;
        game2d::Host host;
        host.remote = to.host + ":" + std::to_string(to.port);
        host.call = [&](const std::string& method, const std::string& path, const std::string& body, int* code) {
            const std::string out = remote.call(method, path, body, code);
            if (path.rfind("/api/quit", 0) == 0) quit = true;   // the server only lets us go; the window closes
            return out;
        };
        host.pump = [](int) {};   // nothing steps here
        host.quitting = [&] { return quit; };
        host.connect = [&](const std::string& addr) {
            if (!relaunch(argc, argv, addr, playerName)) return false;
            int code = 0;
            remote.call("POST", "/api/quit", "{}", &code);
            quit = true;
            return true;
        };
        window->run(host);
        if (!quit) {
            int code = 0;
            remote.call("POST", "/api/quit", "{}", &code);
        }
        return 0;
#else
        std::cerr << "--connect needs the window build; a browser joins at http://" << to.host << ":" << to.port << "/\n";
        return 2;
#endif
    }
    try {
        std::cout << "loading content..." << std::endl;
        const Content content = Content::load(contentDir);
        const Loc loc = Loc::load(locDir);
        app.text.loc = &loc;
        app.text.lang = loc.hasLanguage(lang) ? lang : loc.base();
        app.content = &content;
        app.baseCfg = loadJsonFile(worldFile);
        app.saveDir = c2::saveDir2d();
        loadClientData(app);
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
        // AL-96: the player leads the head of the village (the mandate holder), as the 3D game starts.
        // LR-160: player 1 - the host's window, else the first client to join; a --server starts with nobody led.
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
        app.port = port;
        app.hasWindow = false;
#ifdef LR_GAME2D_NATIVE
        app.hasWindow = window != nullptr;
#endif
        if (app.hasWindow) app.lobby.addLocal(playerName, nowSec(app));

#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) throw std::runtime_error("WSAStartup failed");
#else
        std::signal(SIGPIPE, SIG_IGN);
#endif
        // the HTTP server: the game's server (LR-160: every client but the host's window joins
        // through it, on the local network by default - --bind 127.0.0.1 keeps it to this computer).
        // Beside the window a port in use is no failure: the window plays alone then.
        socket_t ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<unsigned short>(port));
        if (inet_pton(AF_INET, app.bind.c_str(), &addr.sin_addr) != 1) throw std::runtime_error("bad --bind address " + app.bind);
        int yes = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
        if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(ls, 64) != 0) {
            if (web) throw std::runtime_error("cannot listen on " + app.bind + ":" + std::to_string(port) + " (already running?)");
            CLOSESOCK(ls);
            ls = INVALID_SOCKET;
        }
        const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";
        std::cout << "LivelyRealms 2D: " << (ls != INVALID_SOCKET ? url : std::string("no server")) << "  ("
                  << world0.characters().size() << " characters; Ctrl+C to quit)" << std::endl;
        if (ls != INVALID_SOCKET && app.bind != "127.0.0.1")
            for (const auto& ip : net2d::localAddresses())
                std::cout << "  others join: http://" << ip << ":" << port << "/  or  lr_game_2d --connect " << ip << ":" << port
                          << std::endl;
#ifdef _WIN32
        if (web && openBrowser) ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
        (void)openBrowser;
#endif
        (void)server;
        using Clock = std::chrono::steady_clock;
        auto lastTick = Clock::now();
        double owed = 0.0;
        // One request off the socket, answered (it closes after).
        auto serveOne = [&](socket_t cs, const sockaddr_in& peer) {
#ifdef _WIN32
            DWORD to = 2000;
            setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
#else
            timeval to{2, 0};
            setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
#endif
            const bool loop = (ntohl(peer.sin_addr.s_addr) >> 24) == 127;
            serveClient(app, cs, loop);
#ifdef _WIN32
            shutdown(cs, SD_SEND);
#else
            shutdown(cs, SHUT_WR);
#endif
            CLOSESOCK(cs);
        };
        // One round: the requests waiting (up to `waitUs` for the first; LR-160: then all that are
        // ready, at most 48, so 8 clients polling do not queue behind the window's frames), then the
        // sim's steps owed, within `budgetMs` of real time.
        auto pump = [&](long waitUs, int budgetMs) {
            if (ls != INVALID_SOCKET) {
                for (int n = 0; n < 48; ++n) {
                    fd_set rd;
                    FD_ZERO(&rd);
                    FD_SET(ls, &rd);
                    timeval tv{0, n == 0 ? waitUs : 0};
                    if (select(int(ls) + 1, &rd, nullptr, nullptr, &tv) <= 0) break;
                    sockaddr_in peer{};
#ifdef _WIN32
                    int plen = sizeof peer;
#else
                    socklen_t plen = sizeof peer;
#endif
                    socket_t cs = accept(ls, reinterpret_cast<sockaddr*>(&peer), &plen);
                    if (cs == INVALID_SOCKET) break;
                    serveOne(cs, peer);
                }
            }
            checkData(app); // LR-142: hot reload of the content
            const auto now = Clock::now();
            // LR-160: players away too long hand their persons to free will, the ones back take them again
            if (now - app.lastSweep > std::chrono::seconds(1)) {
                app.lastSweep = now;
                if (app.hasWindow) app.lobby.addLocal("", nowSec(app));
                for (const auto& line : app.lobby.sweep(*app.world, nowSec(app))) std::cout << line << std::endl;
            }
            const double dt = std::chrono::duration<double>(now - lastTick).count();
            lastTick = now;
            if (app.autosaveMin > 0 && now - app.lastAutosave > std::chrono::minutes(app.autosaveMin)) autosave(app);
            holdForTalk(app);   // LR-157
            // LR-152: the client's clock for the led body's stamina, the fights it watches
            app.world->client.recordFights = true;
            app.world->client.realSecPerTick = app.tps > 0 ? 1.0 / app.tps : 0.0;
            if (app.paused || now < app.holdUntil) {
                owed = 0.0;
                return;
            }
            owed = std::min(owed + dt * app.tps, 2000.0);
            const auto budgetEnd = Clock::now() + std::chrono::milliseconds(budgetMs);
            while (owed >= 1.0 && Clock::now() < budgetEnd) {
                const auto t0 = Clock::now();
                app.world->step();
                app.lastStepMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                owed -= 1.0;
                if (holdForTalk(app)) {   // LR-157: an NPC asks the player's person: the world waits
                    owed = 0.0;
                    break;
                }
            }
            if (owed > 1.0 && Clock::now() >= budgetEnd) owed = std::min(owed, 1.0 + app.tps * 0.1);
        };
#ifdef LR_GAME2D_NATIVE
        if (window) {
            game2d::Host host;
            host.call = [&](const std::string& method, const std::string& path, const std::string& body, int* code) {
                Reply r;
                app.text.player = 1;   // LR-160: the host's own window
                app.loopback = true;
                dispatch(app, r, method, path, body);
                if (code) *code = r.code;
                return r.body;
            };
            host.pump = [&](int budgetMs) { pump(0, budgetMs); };
            host.quitting = [&] { return app.quit; };
            host.connect = [&](const std::string& to) {
                // LR-160: the window goes over to another computer's game: this one saved, the exe again with --connect
                if (!relaunch(argc, argv, to, playerName)) return false;
                autosave(app);
                app.quit = true;
                return true;
            };
            window->run(host);
            if (!app.quit) autosave(app);   // the window closed some other way than the menu's Quit
            app.quit = true;
        }
#endif
        while (!app.quit) pump(2000, 100);
        std::cout << "quit" << std::endl;
        if (ls != INVALID_SOCKET) CLOSESOCK(ls);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
#ifdef LR_GAME2D_NATIVE
        if (window) window->fail(e.what());
#endif
        return 1;
    }
    return 0;
}
