// Combat PRNG: xoshiro256** seeded through splitmix64. One generator per
// combat, no global state: the same seed gives the same draws on every
// platform (only integer ops; doubles are built from the top 53 bits).
// The helpers mirror the Python prototype's random.Random calls
// (random, uniform, choice, choices, shuffle) by meaning, not by stream.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace lr::combat {

class Rng {
public:
    explicit Rng(std::uint64_t seed = 0) { reseed(seed); }

    void reseed(std::uint64_t seed) {
        std::uint64_t x = seed;
        for (auto& w : s_) w = splitmix(x);
    }

    std::uint64_t next() {
        const std::uint64_t r = rotl(s_[1] * 5, 7) * 9;
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return r;
    }

    // [0, 1)
    double random() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
    double uniform(double a, double b) { return a + (b - a) * random(); }

    // Unbiased integer in [0, n), n > 0 (modulo with rejection of the short tail).
    std::size_t below(std::size_t n) {
        const std::uint64_t bound = static_cast<std::uint64_t>(n);
        const std::uint64_t lim = UINT64_MAX - UINT64_MAX % bound;
        std::uint64_t x = next();
        while (x >= lim) x = next();
        return static_cast<std::size_t>(x % bound);
    }

    // Index drawn by relative weights (Python random.choices: cumulative + bisect_right).
    template <class W>
    std::size_t weighted(const W& weights) {
        double total = 0.0;
        for (double w : weights) total += w;
        const double x = random() * total;
        double cum = 0.0;
        std::size_t i = 0, last = 0;
        for (double w : weights) {
            cum += w;
            if (w > 0.0) last = i;
            if (x < cum) return i;
            ++i;
        }
        return last; // rounding at the very top
    }

    template <class T>
    void shuffle(std::vector<T>& v) {
        for (std::size_t i = v.size(); i > 1; --i) std::swap(v[i - 1], v[below(i)]);
    }

private:
    static std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    static std::uint64_t splitmix(std::uint64_t& x) {
        std::uint64_t z = (x += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    std::uint64_t s_[4]{};
};

// Seed of one combat from the world seed, the combat id and the tick it starts.
inline std::uint64_t combatSeed(std::uint64_t worldSeed, std::uint64_t combatId, std::uint64_t tick) {
    std::uint64_t x = worldSeed ^ (combatId * 0x9E3779B97F4A7C15ull) ^ (tick * 0xD1B54A32D192ED03ull);
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ull;
    return x ^ (x >> 33);
}

} // namespace lr::combat
