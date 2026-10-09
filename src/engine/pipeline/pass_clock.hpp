#pragma once
// ASR_PASS_TIMES=1: every pass's CPU time, averaged and printed to stderr
// every 120 frames. Compiled in every build (unlike the diagnostics build's
// profiler) so a slow release frame can be taken apart where it is slow.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace engine {

class PassClock {
public:
    static PassClock& get() { static PassClock clock; return clock; }
    bool on() const { return on_; }
    void add(const std::string& name, double ms) { auto& e = totals_[name]; e.first += ms; e.second = std::max(e.second, ms); }
    // Once a frame, after everything.
    void frameDone() {
        if (!on_ || ++frames_ < 120) return;
        std::fprintf(stderr, "pass-times (%d frames, CPU ms: mean / max)\n", frames_);
        for (const auto& [name, e] : totals_)
            if (e.first / frames_ >= 0.05) std::fprintf(stderr, "  %-28s %8.3f %8.3f\n", name.c_str(), e.first / frames_, e.second);
        totals_.clear();
        frames_ = 0;
    }
    struct Scope {
        std::string name;
        std::chrono::steady_clock::time_point at;
        bool on;
        explicit Scope(std::string n) : name(std::move(n)), on(get().on()) { if (on) at = std::chrono::steady_clock::now(); }
        ~Scope() { if (on) get().add(name, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - at).count()); }
    };

private:
    PassClock() : on_(std::getenv("ASR_PASS_TIMES") != nullptr) {}
    bool on_;
    int frames_ = 0;
    std::map<std::string, std::pair<double, double>> totals_;
};

} // namespace engine
