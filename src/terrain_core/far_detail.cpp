#include "terrain_core/far_detail.hpp"
#include "terrain_core/foliage.hpp"

#include <nlohmann/json.hpp>

#include <cstring>
#include <fstream>

namespace terrain_core {

bool makeFarUniforms(const std::filesystem::path& file, const std::vector<FoliageLayer>& layers, std::uint32_t seed,
                     FarUniforms& out, std::string* why) {
    out = FarUniforms{};
    std::ifstream in(file);
    if (!in) {
        if (why) *why = "cannot open " + file.string();
        return false;
    }
    const auto config = nlohmann::json::parse(in, nullptr, false);
    if (config.is_discarded()) {
        if (why) *why = "bad json in " + file.string();
        return false;
    }
    out.range[0] = config.value("start_m", 150.0f);
    out.range[1] = config.value("full_m", 400.0f);
    out.range[2] = config.value("meadow_start_m", 100.0f);
    out.range[3] = config.value("meadow_full_m", 170.0f);
    out.strength[0] = config.value("erosion", 1.0f);
    out.strength[1] = config.value("strata", 0.6f);
    out.strength[2] = config.value("cavity", 1.0f);
    out.strength[3] = config.value("mottle", 1.0f);
    out.strength2[0] = config.value("meadow", 1.0f);
    out.strength2[1] = config.value("canopy", 1.0f);

    // The canopy: each named tree layer with the rules and seed foliage.cpp
    // spawns it by (Foliage::spawn: seed * 977 + layer index * 7919).
    std::uint32_t count = 0;
    if (config.contains("canopy_layers"))
        for (const auto& [name, colour] : config["canopy_layers"].items()) {
            if (count >= kFarCanopyLayers) break;
            for (std::size_t li = 0; li < layers.size(); ++li) {
                const FoliageLayer& layer = layers[li];
                if (layer.name != name) continue;
                for (int c = 0; c < 3; ++c) out.canopyColour[count][c] = colour.at(c).get<float>();
                out.canopyColour[count][3] = layer.minNormalZ;
                // A crown of radius about four tenths of the cell at the mean scale.
                out.canopyShape[count][0] = float(layer.spacing);
                out.canopyShape[count][1] = float(layer.spacing) * 0.4f * 0.5f * (layer.scaleMin + layer.scaleMax);
                const std::uint32_t layerSeed = seed * 977u + std::uint32_t(li) * 7919u;
                for (std::uint32_t b = 0; b < kMaxBiomes; ++b) {
                    for (int j = 0; j < 3; ++j) out.canopyRules[count][b][j] = layer.rules[b][j];
                    std::memcpy(&out.canopyRules[count][b][3], &layerSeed, 4);
                }
                ++count;
                break;
            }
        }
    out.strength2[2] = float(count);
    return true;
}

} // namespace terrain_core
