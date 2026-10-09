#include "engine/pipeline/pass_clock.hpp"
#include "engine/pipeline/pass_name.hpp"
#include "engine/pipeline/calc_pipeline.hpp"

#include <chrono>

#include "engine/render/frame.hpp"
#include <cstdio>
#include "engine/pipeline/pass_name.hpp"
#include "engine/render/device.hpp"

namespace engine {

bool CalcPipeline::build(Device& device) {
    std::size_t index = 0;
    for (std::unique_ptr<CalcPass>& pass : passes_) {
        const std::string name = passName(*pass);
        device.beginStep(name, index++, passes_.size());
        const auto began = std::chrono::steady_clock::now();
        if (!pass->setup(device)) return false;
        const double took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
        if (took > 250.0) std::fprintf(stderr, "renderer: %s prepared in %.0f ms\n", name.c_str(), took);
    }
    return true;
}

bool CalcPipeline::run(Frame& frame) {
#if ASR_ENABLE_PROFILING
    passMillis_.clear();
#endif
    for (std::unique_ptr<CalcPass>& pass : passes_) {
        if (!pass->enabled) continue;
#if ASR_ENABLE_PROFILING
        const auto at = std::chrono::steady_clock::now();
#endif
        {
            const PassClock::Scope clock(PassClock::get().on() ? "calc " + passName(*pass) : std::string());
            pass->run(frame);
        }
#if ASR_ENABLE_PROFILING
        passMillis_.emplace_back(
                pass->id(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - at)
                        .count());
#endif
    }
    return true;
}

} // namespace engine
