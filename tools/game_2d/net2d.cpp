// LR-160: see net2d.hpp.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
using sock_t = SOCKET;
#define NET_CLOSE closesocket
#define NET_BAD INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using sock_t = int;
#define NET_CLOSE ::close
#define NET_BAD (-1)
#endif

#include "net2d.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <random>
#include <sstream>

#include <nlohmann/json.hpp>

namespace net2d {

bool parseAddr(const std::string& text, Addr& out, int defaultPort) {
    std::string t = text;
    auto trim = [](std::string& v) {
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '/')) v.pop_back();
    };
    trim(t);
    if (t.rfind("http://", 0) == 0) t = t.substr(7);
    if (const size_t sl = t.find('/'); sl != std::string::npos) t = t.substr(0, sl);
    if (t.empty()) return false;
    out = Addr{};
    out.port = defaultPort;
    const size_t colon = t.rfind(':');
    if (colon != std::string::npos) {
        try {
            out.port = std::stoi(t.substr(colon + 1));
        } catch (...) {
            return false;
        }
        t = t.substr(0, colon);
    }
    if (t.empty() || out.port <= 0 || out.port > 65535) return false;
    for (const char ch : t)
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '.' || ch == '-' || ch == '_')) return false;
    out.host = t;
    return true;
}

void initSockets() {
#ifdef _WIN32
    static bool done = false;
    if (done) return;
    WSADATA wsa;
    done = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#endif
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += ch;
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string httpCall(const Addr& a, const std::string& method, const std::string& path, const std::string& body,
                     const std::vector<std::pair<std::string, std::string>>& headers, int* code, int timeoutMs, std::string* err) {
    initSockets();
    if (code) *code = 0;
    auto fail = [&](const std::string& why) {
        if (err) *err = why;
        return std::string();
    };
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(a.host.c_str(), std::to_string(a.port).c_str(), &hints, &res) != 0 || !res) return fail("no such host " + a.host);
    sock_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == NET_BAD) {
        freeaddrinfo(res);
        return fail("no socket");
    }
#ifdef _WIN32
    DWORD to = DWORD(timeoutMs);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
#else
    timeval to{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);
#endif
    const int rc = connect(s, res->ai_addr, int(res->ai_addrlen));
    freeaddrinfo(res);
    if (rc != 0) {
        NET_CLOSE(s);
        return fail("cannot connect to " + a.host + ":" + std::to_string(a.port));
    }
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + a.host + ":" + std::to_string(a.port) +
                      "\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    for (const auto& [k, v] : headers) req += k + ": " + v + "\r\n";
    req += "\r\n" + body;
    for (size_t off = 0; off < req.size();) {
        const int n = int(send(s, req.data() + off, int(req.size() - off), 0));
        if (n <= 0) {
            NET_CLOSE(s);
            return fail("send failed");
        }
        off += size_t(n);
    }
    std::string resp;
    char buf[16384];
    for (;;) {
        const int n = int(recv(s, buf, sizeof buf, 0));
        if (n <= 0) break;
        resp.append(buf, size_t(n));
    }
    NET_CLOSE(s);
    const size_t he = resp.find("\r\n\r\n");
    if (he == std::string::npos) return fail("no answer");
    int st = 0;
    std::istringstream first(resp.substr(0, resp.find("\r\n")));
    std::string ver;
    first >> ver >> st;
    if (code) *code = st;
    return resp.substr(he + 4);
}

std::vector<std::string> localAddresses() {
    initSockets();
    std::vector<std::string> out;
    auto add = [&](const std::string& ip) {
        if (ip.empty() || ip.rfind("127.", 0) == 0 || ip.rfind("169.254.", 0) == 0) return;
        if (std::find(out.begin(), out.end(), ip) == out.end()) out.push_back(ip);
    };
#ifdef _WIN32
    char name[256] = {};
    if (gethostname(name, sizeof name) == 0) {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        if (getaddrinfo(name, nullptr, &hints, &res) == 0) {
            for (addrinfo* p = res; p; p = p->ai_next) {
                char b[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(p->ai_addr)->sin_addr, b, sizeof b);
                add(b);
            }
            freeaddrinfo(res);
        }
    }
#else
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) == 0) {
        for (ifaddrs* p = ifs; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || (p->ifa_flags & IFF_LOOPBACK)) continue;
            char b[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr, b, sizeof b);
            add(b);
        }
        freeifaddrs(ifs);
    }
#endif
    return out;
}

std::string newToken() {
    std::random_device rd;
    std::mt19937_64 rng((std::uint64_t(rd()) << 32) ^ rd());
    static const char* hex = "0123456789abcdef";
    std::string t;
    for (int i = 0; i < 24; ++i) t += hex[rng() & 15];
    return t;
}

Remote::Remote(Addr a, std::string token, std::string name, std::string kind) : addr_(std::move(a)) {
    headers_ = {{"X-LR-Player", token}, {"X-LR-Client", kind}};
    if (!name.empty()) headers_.push_back({"X-LR-Name", urlEncode(name)});
}

std::string Remote::call(const std::string& method, const std::string& path, const std::string& body, int* code) {
    // the server gone: one try a second, not a frozen window waiting out every call
    const auto now = std::chrono::steady_clock::now();
    if (failures_ >= 3 && now - lastTry_ < std::chrono::seconds(1)) {
        if (code) *code = 0;
        return "";
    }
    lastTry_ = now;
    int c = 0;
    std::string out = httpCall(addr_, method, path, body, headers_, &c);
    failures_ = c == 0 ? failures_ + 1 : 0;
    if (code) *code = c;
    return out;
}

bool Remote::hello(std::string* why, int proto) {
    int code = 0;
    std::string err;
    const std::string b = httpCall(addr_, "GET", "/api/hello", "", headers_, &code, 4000, &err);
    auto no = [&](const std::string& r) {
        if (why) *why = r;
        return false;
    };
    if (code == 0) return no(err);
    if (code == 503) return no("full");
    if (code != 200) return no("answer " + std::to_string(code));
    const auto j = nlohmann::json::parse(b, nullptr, false);
    if (!j.is_object() || j.value("game", std::string()) != "LivelyRealms") return no("not a LivelyRealms game");
    if (j.value("proto", 0) != proto) return no("another version of the game (protocol " + std::to_string(j.value("proto", 0)) + ")");
    return true;
}

int probe(const std::string& spec, std::ostream& out) {
    std::string addrText = spec, name = "probe";
    if (const size_t c = spec.find(','); c != std::string::npos) {
        addrText = spec.substr(0, c);
        name = spec.substr(c + 1);
    }
    Addr a;
    if (!parseAddr(addrText, a)) {
        out << R"({"ok":false,"why":"bad address"})" << std::endl;
        return 2;
    }
    Remote r(a, "probe-" + newToken(), name, "native");
    std::string why;
    nlohmann::json res = {{"ok", false}, {"addr", a.host + ":" + std::to_string(a.port)}};
    if (!r.hello(&why, 1)) {
        res["why"] = why;
        out << res.dump() << std::endl;
        return 1;
    }
    int code = 0;
    const auto hello = nlohmann::json::parse(r.call("GET", "/api/hello", "", &code), nullptr, false);
    const auto st = nlohmann::json::parse(r.call("GET", "/api/state", "", &code), nullptr, false);
    res["ok"] = code == 200 && st.is_object();
    res["player"] = hello.value("player", 0);
    if (st.is_object()) {
        res["paused"] = st.value("paused", false);
        res["tps"] = st.value("tps", 0.0);
        res["tick"] = st.value("tick", 0);
        res["people"] = st.contains("people") ? st["people"].size() : 0;
        res["players"] = st.contains("net") ? st["net"].value("players", nlohmann::json::array()) : nlohmann::json::array();
    }
    r.call("POST", "/api/quit", "{}", &code);   // leave at once
    out << res.dump() << std::endl;
    return res["ok"].get<bool>() ? 0 : 1;
}

} // namespace net2d
