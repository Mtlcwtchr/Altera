#pragma once
// Everything a world's environment is generated from, read and resolved once
// (doc/plan_procedural_environment_2026-10-03.md).
//
// The game hands over three things: its zone classifier (C++), a way to turn
// a model's name into the id its renderer knows, and a directory of JSON:
//
//   content/config/environment/
//     masks.json          the mask channels, at most eight, in GPU order
//     recipes/*.json      feature recipes (recipe.hpp)
//     cover.json          ground cover and secondary scatter by zone (cover.hpp)
//
// The catalogue resolves every name a recipe uses - zones, fields, mask
// channels, models - and says what it could not. An unresolved name is a
// problem, not a crash: the recipe still loads, and whatever named the
// missing thing is left out.
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/environment/cover.hpp"
#include "engine/environment/recipe.hpp"
#include "engine/environment/terrain_ops.hpp"
#include "engine/environment/zones.hpp"

namespace engine::environment {

inline constexpr std::size_t kMaskChannels = 8;

struct MaskChannel {
    std::string name;
    std::array<float, 3> debugColour{1, 1, 1};
    float fromZones = 0;   // reserved: default value where nothing writes
};

using ModelResolver = std::function<std::optional<std::uint32_t>(std::string_view name)>;

struct CatalogueProblem {
    std::string file;
    std::string what;
};

class Catalogue {
public:
    // `classifier` may be null (no zones: every zone rule fails to resolve);
    // `models` may be empty (every model is reported unknown and left out).
    static std::shared_ptr<Catalogue> load(const std::filesystem::path& dir, const ZoneClassifier* classifier,
                                           const ModelResolver& models, std::vector<CatalogueProblem>* problems);
    // The same from recipes already in memory: tests and tools.
    static std::shared_ptr<Catalogue> make(std::vector<FeatureRecipe> recipes, std::vector<MaskChannel> masks,
                                           CoverRules cover, const ZoneClassifier* classifier,
                                           const ModelResolver& models, std::vector<CatalogueProblem>* problems);

    [[nodiscard]] const std::vector<FeatureRecipe>& recipes() const { return recipes_; }
    [[nodiscard]] std::span<const CompiledOp> ops(std::uint32_t recipe) const { return ops_[recipe]; }
    [[nodiscard]] double reach(std::uint32_t recipe) const { return reach_[recipe]; }
    [[nodiscard]] std::optional<std::uint32_t> recipeId(std::string_view name) const;
    [[nodiscard]] const std::vector<MaskChannel>& masks() const { return masks_; }
    [[nodiscard]] std::optional<std::uint8_t> maskId(std::string_view name) const;
    [[nodiscard]] std::optional<ZoneTypeId> zoneId(std::string_view name) const;
    [[nodiscard]] std::span<const ZoneType> zones() const;
    [[nodiscard]] const CoverRules& cover() const { return cover_; }
    // The largest reach of any recipe: how far beyond a block to look for
    // instances that may touch it.
    [[nodiscard]] double maxReach() const { return maxReach_; }
    // Whether any recipe moves the ground. A world whose recipes only dress it
    // costs the height field nothing.
    [[nodiscard]] bool movesGround() const { return movesGround_; }
    // The widest any recipe moves the ground, metres, at its largest scale.
    // Samples further apart than this cannot draw any feature, so the height
    // field at that spacing does not ask for them (a level of detail leaves
    // out what it cannot carry, as it leaves out narrow valleys).
    [[nodiscard]] double widestGround() const { return widestGround_; }
    // The coarsest sample spacing at which features move the ground: the
    // widest feature, and never past 32 m. Pages coarser than that are the
    // distance - kilometres out, where a gully is a pixel - and planning the
    // features of every 512 m block they cover took a 64 m page from 5 ms to
    // half a second, and a fresh view minutes to load. The masks stop at the
    // same spacing (Environment::masks). Now 8 m (D193): the ground's shape
    // stops at coarser data, and a feature's own relief went with it - what
    // the features keep is their meshes, masks and scatter.
    [[nodiscard]] double groundStride() const { return std::min(widestGround_, 8.0); }
    // The recipes of one scale, in catalogue order.
    [[nodiscard]] const std::vector<std::uint32_t>& ofScale(FeatureScale s) const { return byScale_[int(s)]; }

private:
    void resolve(const ZoneClassifier* classifier, const ModelResolver& models, std::vector<CatalogueProblem>* problems);

    std::vector<FeatureRecipe> recipes_;
    std::vector<std::vector<CompiledOp>> ops_;
    std::vector<double> reach_;
    std::vector<MaskChannel> masks_;
    CoverRules cover_;
    const ZoneClassifier* classifier_ = nullptr;
    double maxReach_ = 0;
    double widestGround_ = 0;
    bool movesGround_ = false;
    std::array<std::vector<std::uint32_t>, 3> byScale_;
};

} // namespace engine::environment
