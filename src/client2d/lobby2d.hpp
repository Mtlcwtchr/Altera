#pragma once
// LR-160: the players of one running game (multiplayer, cross-play). The game
// is a server: the world steps on its own whether anyone is in or not; each
// client (the native window in the same process, a native window on another
// computer, a browser page) is a player with an id the sim knows
// (Controller::playerId, "player:<id>" commands). A client names itself by a
// token it keeps (the page in sessionStorage, the window in a file), so a
// reload or a short drop comes back as the same player.
//
// Shared things (pause, speed, a new game, saves) are the server's, the same
// for all, and every player has the same rights over them. Each player leads
// only a person of their own dynasty (sim mayLead, LR-138): one dynasty per
// player.
//
// A player gone quiet for `releaseSec` hands their person back to their own
// life (the AI); when the player is back, they take the same person again.
#include <string>
#include <vector>

#include "client2d/view2d.hpp"
#include "sim/world.hpp"

namespace lr::client2d {

struct NetPlayer {
    int id = 0;
    std::string token, name;
    std::string kind = "web";   // web | native | local (the host's own window)
    double joined = 0, lastSeen = -1e9;
    bool local = false;         // the window of the server's process: always in
    bool released = false;      // its person went back to free will while it was away
    sim::EntityId lastLed = sim::kNone;
};

class Lobby {
public:
    int maxPlayers = 8;
    double onlineSec = 15.0;    // seen this recently: in the game
    double releaseSec = 60.0;   // away this long: their person lives on their own

    // The host's own window: player 1, always in.
    int addLocal(const std::string& name, double now);
    // The player a request comes from (a new token joins; 0 when the game is full).
    // `joined`: a new player or one back after a drop.
    int touch(const std::string& token, const std::string& name, const std::string& kind, double now, bool* joined = nullptr);
    // The player left on purpose (the menu's Quit on a client): away from now on.
    void leave(int id, double now);
    const NetPlayer* find(int id) const;
    NetPlayer* find(int id);
    const std::vector<NetPlayer>& all() const { return players_; }
    bool online(const NetPlayer& p, double now) const { return p.local || now - p.lastSeen <= onlineSec; }
    int onlineCount(double now) const;

    // Once a second or so: a person of a player away too long is released to free will;
    // a player back takes their person again (when the sim still lets them). Lines for the log.
    std::vector<std::string> sweep(sim::World& w, double now);
    // A new world (new game, load): the persons of the old one mean nothing there.
    void worldChanged();

    // The players for the clients: id, name, kind, online, the person led and its name.
    sim::Json toJson(const sim::World& w, const ViewText& t, double now) const;
    // Kept in a save so the players get their persons back after the server restarts.
    sim::Json save() const;
    void load(const sim::Json& j, double now);

private:
    std::vector<NetPlayer> players_;
    int freeId() const;
};

// The person a player leads (kNone: they only watch).
sim::EntityId controlledBy(const sim::World& w, int player);
// A token as the client sent it, made safe (letters, digits, - and _, at most 64).
std::string cleanToken(const std::string& raw);
// A player's name as sent (trimmed, at most 32 bytes, no control characters).
std::string cleanName(const std::string& raw);

} // namespace lr::client2d
