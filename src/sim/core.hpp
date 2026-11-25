#pragma once
// LivelyRealms simulation core: basic types shared by every module. Headless, no
// engine or renderer dependency; determinism comes from Rng seeded by hashes of
// (world seed, entity, tick, salt), never from iteration order or wall clock.

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lr::sim {

using Json = nlohmann::json;
using Tick = std::int64_t;
using EntityId = std::uint32_t;
constexpr EntityId kNone = 0;

// One tick is ten game minutes.
constexpr Tick kTicksPerDay = 144;
constexpr int kDaysPerYear = 360;
constexpr Tick kTicksPerYear = kTicksPerDay * kDaysPerYear;

inline std::uint64_t mix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

inline std::uint64_t hashCombine(std::uint64_t a, std::uint64_t b) {
    return mix64(a ^ (mix64(b) + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2)));
}

inline std::uint64_t hashStr(std::string_view s) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

// SplitMix64 stream. Small, fast, fully specified - the same numbers on every
// compiler, which std:: distributions do not promise.
struct Rng {
    std::uint64_t state = 0;
    explicit Rng(std::uint64_t seed = 0) : state(seed) {}
    std::uint64_t next() {
        state += 0x9e3779b97f4a7c15ull;
        std::uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    // [0, 1)
    double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
    double range(double lo, double hi) { return lo + (hi - lo) * uniform(); }
    // [lo, hi] inclusive
    int irange(int lo, int hi) {
        if (hi <= lo) return lo;
        return lo + static_cast<int>(next() % static_cast<std::uint64_t>(hi - lo + 1));
    }
    bool chance(double p) { return uniform() < p; }
};

// seed = hash(world_seed, agent_id, tick, salt) - AI_ARCHITECTURE §1.1.
inline Rng agentRng(std::uint64_t worldSeed, EntityId agent, Tick tick, std::string_view salt) {
    std::uint64_t h = hashCombine(worldSeed, agent);
    h = hashCombine(h, static_cast<std::uint64_t>(tick));
    h = hashCombine(h, hashStr(salt));
    return Rng(h);
}

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};
inline double dist(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }
inline void to_json(Json& j, const Vec2& v) { j = Json::array({v.x, v.y}); }
inline void from_json(const Json& j, Vec2& v) {
    v.x = j.at(0).get<double>();
    v.y = j.at(1).get<double>();
}

inline double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

} // namespace lr::sim
