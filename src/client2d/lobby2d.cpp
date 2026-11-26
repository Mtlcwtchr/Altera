// LR-160: the players of one running game (see lobby2d.hpp).
#include "client2d/lobby2d.hpp"

#include <algorithm>

#include "sim/ai.hpp"
#include "sim/loc.hpp"

namespace lr::client2d {

using namespace lr::sim;

namespace {

std::string nameOf(const ViewText& t, const Character& c) {
    if (!t.loc) return c.name;
    const Json n = t.loc->personNoun(t.lang, c);
    return n.is_object() ? n.value("nom", c.name) : c.name;
}

Command playerCmd(int player, const std::string& type) {
    Command c;
    c.source = "player:" + std::to_string(player);
    c.type = type;
    c.args = {{"player", player}};
    return c;
}

} // namespace

EntityId controlledBy(const World& w, int player) {
    for (const auto& [id, c] : w.characters())
        if (c.controller.isPlayer() && c.controller.playerId == player) return id;
    return kNone;
}

std::string cleanToken(const std::string& raw) {
    std::string out;
    for (const char ch : raw) {
        if (out.size() >= 64) break;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') out += ch;
    }
    return out;
}

std::string cleanName(const std::string& raw) {
    std::string out;
    for (const char ch : raw)
        if (static_cast<unsigned char>(ch) >= 0x20 && ch != 0x7f) out += ch;
    const auto b = out.find_first_not_of(' ');
    if (b == std::string::npos) return "";
    out = out.substr(b, out.find_last_not_of(' ') - b + 1);
    if (out.size() > 32) {
        std::size_t cut = 32;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;   // not inside a UTF-8 letter
        out.resize(cut);
    }
    return out;
}

int Lobby::freeId() const {
    for (int id = 1;; ++id)
        if (!find(id)) return id;
}

const NetPlayer* Lobby::find(int id) const {
    for (const auto& p : players_)
        if (p.id == id) return &p;
    return nullptr;
}
NetPlayer* Lobby::find(int id) {
    for (auto& p : players_)
        if (p.id == id) return &p;
    return nullptr;
}

int Lobby::addLocal(const std::string& name, double now) {
    if (NetPlayer* p = find(1)) {
        p->local = true;
        p->kind = "local";
        p->lastSeen = now;
        if (!name.empty()) p->name = name;
        return 1;
    }
    NetPlayer p;
    p.id = 1;
    p.token = "local";
    p.name = name;
    p.kind = "local";
    p.local = true;
    p.joined = p.lastSeen = now;
    players_.insert(players_.begin(), p);
    return 1;
}

int Lobby::onlineCount(double now) const {
    int n = 0;
    for (const auto& p : players_) n += online(p, now) ? 1 : 0;
    return n;
}

int Lobby::touch(const std::string& rawToken, const std::string& rawName, const std::string& kind, double now, bool* joined) {
    if (joined) *joined = false;
    std::string token = cleanToken(rawToken);
    if (token.empty()) token = "anon";
    const std::string name = cleanName(rawName);
    for (auto& p : players_) {
        if (p.token != token) continue;
        if (!online(p, now) && joined) *joined = true;
        p.lastSeen = now;
        if (!name.empty()) p.name = name;
        if (!kind.empty() && !p.local) p.kind = kind;
        return p.id;
    }
    if (onlineCount(now) >= maxPlayers) return 0;
    NetPlayer p;
    p.id = freeId();
    p.token = token;
    p.name = name.empty() ? "#" + std::to_string(p.id) : name;
    p.kind = kind.empty() ? "web" : kind;
    p.joined = p.lastSeen = now;
    players_.push_back(p);
    if (joined) *joined = true;
    return p.id;
}

void Lobby::leave(int id, double now) {
    if (NetPlayer* p = find(id); p && !p->local) p->lastSeen = now - std::max(onlineSec, releaseSec) - 1.0;
}

std::vector<std::string> Lobby::sweep(World& w, double now) {
    std::vector<std::string> log;
    for (auto& p : players_) {
        const EntityId led = controlledBy(w, p.id);
        if (!p.local && now - p.lastSeen > releaseSec) {
            if (led != kNone) {
                // away too long: their person goes on with their own life
                w.applyNow(playerCmd(p.id, "release_control"));
                p.lastLed = led;
                p.released = true;
                log.push_back("player " + std::to_string(p.id) + " (" + p.name + ") away: their person lives on their own");
            }
            continue;
        }
        if (led != kNone) {
            p.lastLed = led;
            p.released = false;
            continue;
        }
        if (p.released && online(p, now) && p.lastLed != kNone) {
            // back: the same person again, if the sim lets them (alive, not taken by another)
            p.released = false;
            Command take = playerCmd(p.id, "take_control");
            take.args["id"] = p.lastLed;
            const Command r = w.applyNow(take);
            log.push_back("player " + std::to_string(p.id) + " (" + p.name + ") back: " + (r.result.empty() ? "ok" : r.result));
        }
    }
    return log;
}

void Lobby::worldChanged() {
    for (auto& p : players_) {
        p.lastLed = kNone;
        p.released = false;
    }
}

Json Lobby::toJson(const World& w, const ViewText& t, double now) const {
    Json out = Json::array();
    for (const auto& p : players_) {
        const bool on = online(p, now);
        const EntityId led = controlledBy(w, p.id);
        if (!on && led == kNone) continue;   // long gone: not listed
        Json j = {{"id", p.id}, {"name", p.name}, {"kind", p.kind}, {"online", on}, {"me", led}};
        if (const Character* c = led != kNone ? w.character(led) : nullptr) j["person"] = nameOf(t, *c);
        if (p.id == t.player) j["you"] = true;
        out.push_back(j);
    }
    return out;
}

Json Lobby::save() const {
    Json out = Json::array();
    for (const auto& p : players_)
        out.push_back({{"id", p.id}, {"token", p.local ? std::string("local") : p.token}, {"name", p.name}, {"kind", p.kind}});
    return out;
}

void Lobby::load(const Json& j, double now) {
    if (!j.is_array()) return;
    for (const auto& e : j) {
        if (!e.is_object()) continue;
        const int id = e.value("id", 0);
        const std::string token = cleanToken(e.value("token", std::string()));
        if (id <= 0 || token.empty()) continue;
        // a token known already keeps its id; an id taken by another token is not overwritten
        bool known = false;
        for (auto& p : players_) known = known || p.token == token || p.id == id;
        if (known) continue;
        NetPlayer p;
        p.id = id;
        p.token = token;
        p.name = cleanName(e.value("name", std::string()));
        p.kind = e.value("kind", std::string("web"));
        p.joined = now;
        p.lastSeen = now;   // the grace of a drop: they have releaseSec to come back
        players_.push_back(p);
    }
}

} // namespace lr::client2d
