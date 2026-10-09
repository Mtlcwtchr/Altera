#pragma once
// Offline erosion of the macro map, on the GPU.
//
// The source map is enlarged `scale` times (16k from 4k: 32 m a texel on a
// 524 km world), given the relief it is too coarse to hold, and weathered by
// thermal erosion: ground steeper than the angle of repose sheds height to
// its lower neighbours until it stands at that angle. What comes out is the
// same kind of macro map, written as raw 16-bit (.r16) - a 16k PNG would take
// longer to compress than the erosion takes to run.

#include "engine/render/device.hpp"
#include "terrain_core/macro_map.hpp"
#include "terrain_core/params.hpp"

#include <string>

namespace terrain_core {

struct ErosionSettings {
    std::uint32_t scale = 4;          // output texels per source texel (power of two)
    std::uint32_t iterations = 200;   // thermal passes
    double talusDegrees = 35.0;       // angle of repose
    double rate = 0.5;                // share of the excess moved per pass
    double detail = 1.0;              // strength of the ridged relief added in the mountains
};

struct ErosionTimings {
    double uploadMs = 0, upscaleMs = 0, thermalMs = 0, readbackMs = 0, convertMs = 0;
};

bool erode(engine::Device& device, const MacroMap& source, const Params& params, const ErosionSettings& settings,
           MacroMap& out, ErosionTimings& timings, std::string* why = nullptr);

} // namespace terrain_core
