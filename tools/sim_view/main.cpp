// altera_sim_view - watch and play the Altera life-sim in a browser.
//   altera_sim_view [--seed N] [--season spring|summer|autumn|winter] [--world FILE]
//                   [--content DIR] [--loc DIR] [--lang ru|en] [--port 8765] [--no-browser]
// One process: the simulation steps on the main thread between HTTP requests
// (no locks needed); the page (tools/sim_view/index.html, read from disk on
// every request, so it can be edited without a rebuild) polls /api/state and
// sends /api/cmd. Placeholder visuals until the sim moves into the game client.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "sim/ai.hpp"
#include "sim/loc.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;

namespace {

struct App {
    const Content* content = nullptr;
    const Loc* loc = nullptr;
    std::string lang = "ru";
    std::string pageFile;
    World* world = nullptr;
    bool paused = true;
    double tps = 6.0; // sim ticks per real second (1 tick = 10 game minutes)
    double lastStepMs = 0.0;
    std::string lastResult;
};

std::string nameOf(const App& a, const std::string& kind, const std::string& id) {
    if (id.empty()) return "";
    const std::string k = kind + "." + id + ".name";
    if (a.loc->has(a.lang, k)) return a.loc->text(a.lang, k);
    return id;
}

std::string clock(Tick t) {
    const int m = static_cast<int>(t % kTicksPerDay) * 10;
    char b[16];
    std::snprintf(b, sizeof b, "%02d:%02d", m / 60, m % 60);
    return b;
}

// The running action (interaction name); the FSM state goes to the page as an id.
std::string activityOf(const App& a, const Character& c) {
    if (!c.body.alive) return "";
    const FsmState& f = c.ai.fsm;
    if (!f.interaction.empty() && f.interaction != "goto") return nameOf(a, "interaction", f.interaction);
    if (f.interaction == "goto" && c.ai.plan.actionIndex + 1 < c.ai.plan.actions.size()) {
        const auto& next = c.ai.plan.actions[c.ai.plan.actionIndex + 1];
        if (next.interaction != "goto") return "-> " + nameOf(a, "interaction", next.interaction);
    }
    return "";
}

std::string goalOf(const App& a, const Character& c) {
    if (!c.ai.goal.valid()) return "";
    if (c.ai.goal.id == "player_action") return nameOf(a, "interaction", c.ai.goal.task);
    return nameOf(a, "goal", c.ai.goal.id);
}

Json staticJson(const App& a) {
    const World& w = *a.world;
    const MapGrid& m = w.map();
    Json j;
    j["size"] = m.sizeM;
    j["cell"] = m.cellM;
    j["w"] = m.w;
    j["h"] = m.h;
    j["biomes"] = m.biomeNames;
    Json bn = Json::array();
    for (const auto& b : m.biomeNames) bn.push_back(b);
    j["biome_names"] = bn;
    j["biome"] = m.biome;
    j["water"] = m.water;
    Json st = Json::array();
    for (const auto& [id, s] : w.settlements()) st.push_back({{"id", id}, {"name", s.name}, {"x", s.center.x}, {"y", s.center.y}});
    j["settlements"] = st;
    // Objects grouped by type; very common types (trees) are thinned to keep the page light.
    std::map<std::string, Json> byType;
    std::map<std::string, int> counts;
    for (const auto& [id, o] : w.objects()) ++counts[o.type];
    Json types = Json::object();
    for (const auto& [id, o] : w.objects()) {
        const int n = counts[o.type];
        const int stride = n > 3000 ? n / 3000 : 1;
        if (stride > 1 && (id % static_cast<EntityId>(stride)) != 0) continue;
        byType[o.type].push_back({id, std::round(o.pos.x), std::round(o.pos.y)});
    }
    for (auto& [t, list] : byType)
        types[t] = {{"name", nameOf(a, "object", t)}, {"count", counts[t]}, {"pts", list}};
    j["objects"] = types;
    Json goals = Json::array();
    for (const auto& g : w.content().goals) goals.push_back({{"id", g.id}, {"name", nameOf(a, "goal", g.id)}});
    j["goals"] = goals;
    return j;
}

Json characterBrief(const App& a, const Character& c) {
    const World& w = *a.world;
    const Family* f = w.familyOf(c);
    return {{"id", c.id},
            {"name", c.name},
            {"sp", c.species},
            {"spn", c.isAnimal() ? nameOf(a, "species", c.species) : ""},
            {"g", c.group},
            {"x", std::round(c.pos.x * 10) / 10},
            {"y", std::round(c.pos.y * 10) / 10},
            {"alive", c.body.alive},
            {"pl", c.controller.isPlayer()},
            {"sex", std::string(1, c.body.sex)},
            {"age", c.body.ageDays / kDaysPerYear},
            {"fam", f ? f->name : ""},
            {"hp", std::round(c.body.health * 100) / 100},
            {"wnd", c.wounds.size()},
            {"down", c.body.downUntil > w.now()},
            {"st", c.ai.fsm.state},
            {"act", activityOf(a, c)},
            {"goal", goalOf(a, c)}};
}

Json characterDetail(const App& a, const Character& c) {
    const World& w = *a.world;
    Json d = characterBrief(a, c);
    d["cause"] = c.body.causeOfDeath;
    d["fatigue"] = c.body.fatigue;
    d["blood"] = c.body.blood;
    d["mood"] = c.mood(w.now());
    d["stage"] = c.body.stage();
    d["role"] = c.status.role;
    d["free_will"] = c.controller.freeWill;
    Json needs = Json::array();
    for (const auto& [k, v] : c.needs) needs.push_back({{"id", k}, {"name", nameOf(a, "need", k)}, {"v", v}});
    d["needs"] = needs;
    Json inv = Json::array();
    for (const auto& [k, v] : c.inventory)
        if (v) inv.push_back({{"name", nameOf(a, "item", k)}, {"n", v}});
    d["inv"] = inv;
    Json traits = Json::array();
    for (const auto& t : c.traits) traits.push_back(nameOf(a, "trait", t));
    d["traits"] = traits;
    Json skills = Json::array();
    for (const auto& [k, v] : c.skills)
        if (v > 0.05) skills.push_back({{"name", std::string(k)}, {"v", v}});
    d["skills"] = skills;
    Json wounds = Json::array();
    for (const auto& x : c.wounds)
        wounds.push_back({{"zone", std::string(x.zone)}, {"sev", std::string(x.severity)},
                          {"left", x.daysLeft}, {"bleeding", x.bleeding}, {"infected", x.infected},
                          {"treat", std::string(x.treatment)}});
    d["wounds"] = wounds;
    Json inj = Json::array();
    for (const auto& x : c.injuries) inj.push_back(x.zone + ": " + x.kind);
    d["injuries"] = inj;
    Json moods = Json::array();
    for (const auto& m : c.moodlets)
        if (m.until > w.now()) moods.push_back({{"name", nameOf(a, "moodlet", m.id)}, {"v", m.value}});
    d["moodlets"] = moods;
    // Relations: the strongest few.
    std::vector<std::pair<double, EntityId>> rel;
    for (const auto& [id, r] : c.relations)
        rel.push_back({std::abs(r.affinity) + std::abs(r.enmity) + r.familiarity * 0.3 + std::abs(r.fear), id});
    std::sort(rel.rbegin(), rel.rend());
    Json rels = Json::array();
    for (size_t i = 0; i < rel.size() && i < 8; ++i) {
        const Character* o = w.character(rel[i].second);
        if (!o) continue;
        const Relation& r = c.relations.at(rel[i].second);
        rels.push_back({{"id", o->id}, {"name", o->isAnimal() ? nameOf(a, "species", o->species) : o->name},
                        {"aff", r.affinity}, {"trust", r.trust}, {"fear", r.fear}, {"enm", r.enmity},
                        {"fam", r.familiarity}});
    }
    d["relations"] = rels;
    Json mem = Json::array();
    for (auto it = c.memory.rbegin(); it != c.memory.rend() && mem.size() < 12; ++it) {
        std::string who;
        for (EntityId id : it->who)
            if (const Character* o = w.character(id)) who += (who.empty() ? "" : ", ") + o->name;
        const std::string key = "memory." + it->type + ".text";
        Json params = Json::object();
        if (!it->who.empty()) params["target"] = it->who.front();
        const std::string text = a.loc->has(a.lang, key) ? a.loc->text(a.lang, key, locArgs(*a.loc, a.lang, w, params, &c))
                                                         : it->type;
        mem.push_back({{"day", it->tick / kTicksPerDay}, {"t", clock(it->tick)}, {"text", text}, {"who", who},
                       {"val", it->valence}});
    }
    d["memory"] = mem;
    Json tr = Json::array();
    const size_t from = c.ai.trace.size() > 40 ? c.ai.trace.size() - 40 : 0;
    for (size_t i = c.ai.trace.size(); i-- > from;) {
        const TraceEntry& e = c.ai.trace[i];
        tr.push_back({{"day", e.tick / kTicksPerDay}, {"t", clock(e.tick)}, {"l", std::string(1, e.layer)},
                      {"text", traceText(*a.loc, a.lang, w, c, e)}});
    }
    d["trace"] = tr;
    return d;
}

Json stateJson(App& a, EntityId sel, size_t chronSince) {
    World& w = *a.world;
    Json j;
    j["tick"] = w.now();
    j["day"] = w.day();
    j["time"] = clock(w.now());
    j["night"] = w.isNight();
    j["season"] = w.season();
    j["paused"] = a.paused;
    j["tps"] = a.tps;
    j["step_ms"] = a.lastStepMs;
    j["last_result"] = a.lastResult;
    if (!w.settlements().empty()) {
        const DayWeather dw = weatherToday(w, w.settlements().begin()->first);
        j["weather"] = {{"t", std::round(dw.tempC * 10) / 10}, {"rain", std::round(dw.rainMm * 10) / 10}, {"wet", dw.wet}};
    }
    Json chars = Json::array();
    int people = 0, alive = 0;
    for (const auto& [id, c] : w.characters()) {
        chars.push_back(characterBrief(a, c));
        if (!c.isAnimal()) {
            ++people;
            alive += c.body.alive ? 1 : 0;
        }
    }
    j["chars"] = chars;
    j["people"] = people;
    j["alive"] = alive;
    const auto& log = w.chronicleLog();
    Json ch = Json::array();
    for (size_t i = std::min(chronSince, log.size()); i < log.size(); ++i) {
        const ChronicleEntry& e = log[i];
        std::string text = a.loc->has(a.lang, e.code) ? a.loc->text(a.lang, e.code, locArgs(*a.loc, a.lang, w, e.params))
                                                       : e.code + " " + e.params.dump();
        ch.push_back({{"day", e.tick / kTicksPerDay}, {"t", clock(e.tick)}, {"type", e.type}, {"who", e.who}, {"text", text}});
    }
    j["chron"] = ch;
    j["chron_n"] = log.size();
    if (const Character* c = w.character(sel)) j["sel"] = characterDetail(a, *c);
    return j;
}

std::string handleCmd(App& a, const Json& body) {
    const std::string op = body.value("op", std::string());
    World& w = *a.world;
    if (op == "pause") a.paused = body.value("on", !a.paused);
    else if (op == "speed") a.tps = std::clamp(body.value("tps", 6.0), 0.5, 2000.0);
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
        return a.lastResult;
    } else return "unknown op";
    return "ok";
}

// --- tiny HTTP ---------------------------------------------------------------
std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void sendAll(SOCKET s, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        const int n = send(s, data.data() + off, static_cast<int>(std::min<size_t>(data.size() - off, 1 << 20)), 0);
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

void respond(SOCKET s, int code, const std::string& type, const std::string& body) {
    std::string h = "HTTP/1.1 " + std::to_string(code) + (code == 200 ? " OK" : " Error") +
                    "\r\nContent-Type: " + type + "\r\nCache-Control: no-store\r\nContent-Length: " +
                    std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    sendAll(s, h + body);
}

std::string queryParam(const std::string& path, const std::string& key) {
    const size_t q = path.find('?');
    if (q == std::string::npos) return "";
    std::string qs = path.substr(q + 1);
    std::stringstream ss(qs);
    std::string kv;
    while (std::getline(ss, kv, '&')) {
        const size_t e = kv.find('=');
        if (e != std::string::npos && kv.substr(0, e) == key) return kv.substr(e + 1);
    }
    return "";
}

void serveClient(App& a, SOCKET s) {
    std::string req;
    char buf[8192];
    size_t headerEnd = std::string::npos;
    size_t contentLen = 0;
    for (;;) {
        const int n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) break;
        req.append(buf, static_cast<size_t>(n));
        if (headerEnd == std::string::npos) {
            headerEnd = req.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                std::string lower = req.substr(0, headerEnd);
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
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
        if (path == "/" || path.rfind("/index.html", 0) == 0) {
            const std::string page = readFile(a.pageFile);
            if (page.empty()) respond(s, 404, "text/plain", "page not found: " + a.pageFile);
            else respond(s, 200, "text/html; charset=utf-8", page);
        } else if (path.rfind("/api/static", 0) == 0) {
            respond(s, 200, "application/json; charset=utf-8", staticJson(a).dump());
        } else if (path.rfind("/api/state", 0) == 0) {
            const std::string sel = queryParam(path, "sel"), since = queryParam(path, "since");
            const EntityId id = sel.empty() ? kNone : static_cast<EntityId>(std::stoul(sel));
            const size_t from = since.empty() ? 0 : std::stoul(since);
            respond(s, 200, "application/json; charset=utf-8", stateJson(a, id, from).dump(-1, ' ', false, Json::error_handler_t::replace));
        } else if (path.rfind("/api/cmd", 0) == 0 && method == "POST") {
            const std::string r = handleCmd(a, Json::parse(body));
            respond(s, 200, "application/json; charset=utf-8", Json{{"result", r}}.dump());
        } else {
            respond(s, 404, "text/plain", "not found");
        }
    } catch (const std::exception& e) {
        a.lastResult = std::string("error: ") + e.what();
        respond(s, 500, "application/json", Json{{"result", a.lastResult}}.dump());
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string root = ALTERA_SIM_SOURCE_DIR;
    std::string contentDir = root + "/content/ai";
    std::string worldFile = root + "/content/sim/test_world.json";
    std::string locDir = root + "/content/loc";
    std::string season;
    App app;
    app.pageFile = root + "/tools/sim_view/index.html";
    std::uint64_t seed = 7;
    int port = 8765;
    bool openBrowser = true;
    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << argv[i] << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (!std::strcmp(argv[i], "--content")) contentDir = next();
        else if (!std::strcmp(argv[i], "--world")) worldFile = next();
        else if (!std::strcmp(argv[i], "--seed")) seed = std::stoull(next());
        else if (!std::strcmp(argv[i], "--season")) season = next();
        else if (!std::strcmp(argv[i], "--loc")) locDir = next();
        else if (!std::strcmp(argv[i], "--lang")) app.lang = next();
        else if (!std::strcmp(argv[i], "--port")) port = std::stoi(next());
        else if (!std::strcmp(argv[i], "--page")) app.pageFile = next();
        else if (!std::strcmp(argv[i], "--no-browser")) openBrowser = false;
        else {
            std::cerr << "unknown argument " << argv[i] << "\n";
            return 2;
        }
    }
    try {
        std::cout << "loading content..." << std::endl;
        const Content content = Content::load(contentDir);
        const Loc loc = Loc::load(locDir);
        Json cfg = loadJsonFile(worldFile);
        if (!season.empty()) {
            cfg.erase("start_day");
            cfg["season"] = season;
        }
        std::cout << "generating world, seed " << seed << "..." << std::endl;
        World world = makeTestWorld(content, cfg, seed);
        installAi(world);
        app.content = &content;
        app.loc = &loc;
        app.world = &world;

        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) throw std::runtime_error("WSAStartup failed");
        SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        BOOL yes = TRUE;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
        if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(ls, 16) != 0)
            throw std::runtime_error("cannot listen on 127.0.0.1:" + std::to_string(port) + " (already running?)");
        const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";
        std::cout << "Altera sim view: " << url << "  (" << world.characters().size()
                  << " characters; Ctrl+C to quit)" << std::endl;
        if (openBrowser) ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

        using Clock = std::chrono::steady_clock;
        auto lastTick = Clock::now();
        double owed = 0.0;
        for (;;) {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(ls, &rd);
            timeval tv{0, 2000};
            if (select(0, &rd, nullptr, nullptr, &tv) > 0) {
                SOCKET cs = accept(ls, nullptr, nullptr);
                if (cs != INVALID_SOCKET) {
                    DWORD to = 2000;
                    setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
                    serveClient(app, cs);
                    shutdown(cs, SD_SEND);
                    closesocket(cs);
                }
            }
            const auto now = Clock::now();
            const double dt = std::chrono::duration<double>(now - lastTick).count();
            lastTick = now;
            if (app.paused) {
                owed = 0.0;
                continue;
            }
            owed = std::min(owed + dt * app.tps, 2000.0);
            // Step while owed, but give the page a turn at least every ~100 ms.
            const auto budgetEnd = Clock::now() + std::chrono::milliseconds(100);
            while (owed >= 1.0 && Clock::now() < budgetEnd) {
                const auto t0 = Clock::now();
                world.step();
                app.lastStepMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                owed -= 1.0;
            }
            if (owed > 1.0 && Clock::now() >= budgetEnd) owed = std::min(owed, 1.0 + app.tps * 0.1); // fall behind gracefully
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
