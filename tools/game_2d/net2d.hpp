#pragma once
// LR-160: the network side of lr_game_2d - a tiny HTTP client (a window that
// plays the game of another computer: --connect), this computer's addresses
// (what the others type to join), and a probe of a server for tests.
#include <chrono>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace net2d {

struct Addr {
    std::string host;
    int port = 8766;
};

// "host", "host:port", "http://host:port/" -> Addr. False when it names nothing.
bool parseAddr(const std::string& text, Addr& out, int defaultPort = 8766);

// Sockets ready (WSAStartup on Windows); safe to call more than once.
void initSockets();

// One request, the connection closed after it. Body of the answer; *code 0 when it did not connect.
std::string httpCall(const Addr& a, const std::string& method, const std::string& path, const std::string& body,
                     const std::vector<std::pair<std::string, std::string>>& headers, int* code, int timeoutMs = 4000,
                     std::string* err = nullptr);

// %xx for a header or a query value (non-ASCII names go through as UTF-8 bytes).
std::string urlEncode(const std::string& s);

// The IPv4 addresses of this computer others can reach (no loopback); empty when none is known.
std::vector<std::string> localAddresses();

// A fresh client token (random hex).
std::string newToken();

// The client of a remote game: every call carries the player's token and name.
class Remote {
public:
    Remote(Addr a, std::string token, std::string name, std::string kind = "native");
    std::string call(const std::string& method, const std::string& path, const std::string& body, int* code);
    // The server is a LivelyRealms game of a protocol we speak. False + why otherwise.
    bool hello(std::string* why, int proto);
    const Addr& addr() const { return addr_; }
    int failures() const { return failures_; }

private:
    Addr addr_;
    std::vector<std::pair<std::string, std::string>> headers_;
    int failures_ = 0;   // calls in a row that did not connect
    std::chrono::steady_clock::time_point lastTry_{};
};

// --probe host:port[,name]: join as a client, print the server's hello and the shared state
// (players, pause, speed, the clock) as one JSON line. 0 on success.
int probe(const std::string& spec, std::ostream& out);

} // namespace net2d
