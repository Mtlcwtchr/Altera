#include "engine/core/frame_jobs.hpp"
#include "engine/render/systems/level_select.hpp"

#include <algorithm>
#include <cmath>

namespace engine::render {
namespace {
double row(const float r[4], const float p[3]) {
    return double(r[0]) * p[0] + double(r[1]) * p[1] + double(r[2]) * p[2] + double(r[3]);
}
}

double ScreenScale::pixelsPerMetreAt(const float position[3], double& depth) const {
    depth = row(rowW, position);
    if (focal <= 0) return scale;                 // orthographic: one scale everywhere
    return focal / std::max(depth, 1e-3);
}

SelectedLevels selectLevels(const GatheredInstances& gathered,
                            std::span<const MeshDescription> assets,
                            const ScreenScale& screen) {
    SelectedLevels result;

    // One pass to decide, then one stable regroup. Deciding and emitting in the
    // same pass would put an instance in whichever batch it happened to be
    // visited from, and the batches would stop being contiguous runs.
    //
    // The deciding on the frame's threads (FrameJobs), a level per instance;
    // the regroup a counting sort by level inside each (mesh, material) run,
    // in source order - the same order the stable sort by (mesh, material,
    // level, source) it replaces gave, without sorting a hundred thousand.
    constexpr std::uint32_t kBehind = ~0u;
    std::vector<std::uint32_t> levels(gathered.instances.size(), kBehind);
    FrameJobs::get().run(gathered.batches.size(), 8, [&](std::size_t b0, std::size_t b1, std::size_t) {
        for (std::size_t b = b0; b < b1; ++b) {
            const InstanceBatch& batch = gathered.batches[b];
            const MeshDescription* description =
                    batch.mesh < assets.size() ? &assets[batch.mesh] : nullptr;
            for (std::uint32_t i = 0; i < batch.count; ++i) {
                const std::uint32_t at = batch.first + i;
                const GatheredInstance& instance = gathered.instances[at];
                // Measured at the model's middle, not at its feet. The cull uses
                // the same point, and a frame that disagreed about which one it
                // meant would size a thing by a depth it was not culled at.
                const float centre[3]{instance.position[0], instance.position[1],
                                      instance.position[2] +
                                              float((description ? description->bounds.rise : 0) *
                                                    instance.scale)};
                double depth = 0;
                const double perMetre = screen.pixelsPerMetreAt(centre, depth);
                if (screen.focal > 0 && depth <= 0) continue;
                std::uint32_t level = 0;
                if (description && !description->errors.empty()) {
                    // The instance's own size on screen: the model's extent
                    // grown by this instance's scale.
                    const double pixels = description->extent * instance.scale * perMetre;
                    level = std::uint32_t(levelFor(description->errors, pixels, description->extent,
                                                   screen.allowance));
                    // Never finer than what is actually there: the coarser
                    // page it has is the honest answer.
                    if (description->resident)
                        level = std::min<std::uint32_t>(level, std::uint32_t(description->resident - 1));
                }
                levels[at] = level;
            }
        }
    });

    std::vector<std::uint32_t> order(gathered.batches.size());
    for (std::uint32_t b = 0; b < order.size(); ++b) order[b] = b;
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        const InstanceBatch &x = gathered.batches[a], &y = gathered.batches[b];
        if (x.mesh != y.mesh) return x.mesh < y.mesh;
        if (x.material != y.material) return x.material < y.material;
        return x.first < y.first;
    });
    result.instances.reserve(gathered.instances.size());
    result.owners.reserve(gathered.instances.size());
    std::vector<std::uint32_t> counts;
    for (std::size_t r = 0; r < order.size();) {
        const InstanceBatch& head = gathered.batches[order[r]];
        std::size_t end = r;
        while (end < order.size() && gathered.batches[order[end]].mesh == head.mesh &&
               gathered.batches[order[end]].material == head.material)
            ++end;
        counts.assign(1, 0);
        for (std::size_t k = r; k < end; ++k) {
            const InstanceBatch& batch = gathered.batches[order[k]];
            for (std::uint32_t i = 0; i < batch.count; ++i) {
                const std::uint32_t level = levels[batch.first + i];
                if (level == kBehind) { ++result.behind; continue; }
                if (level >= counts.size()) counts.resize(level + 1, 0);
                ++counts[level];
            }
        }
        for (std::uint32_t level = 0; level < counts.size(); ++level) {
            if (!counts[level]) continue;
            result.batches.push_back({head.mesh, head.material, level,
                                      std::uint32_t(result.instances.size()), counts[level]});
            for (std::size_t k = r; k < end; ++k) {
                const InstanceBatch& batch = gathered.batches[order[k]];
                for (std::uint32_t i = 0; i < batch.count; ++i) {
                    const std::uint32_t at = batch.first + i;
                    if (levels[at] != level) continue;
                    result.instances.push_back(gathered.instances[at]);
                    result.owners.push_back(gathered.owners[at]);
                }
            }
        }
        r = end;
    }
    return result;
}

} // namespace engine::render
