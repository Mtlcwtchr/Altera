#pragma once
// Skinned figures for the game: the people (and, with their own files, the
// beasts) baked by tools/game_assets from the Quaternius UAL rig and the
// retargeted clips. A set is one bake: its looks share one joint list and one
// texture atlas; its clips are per-joint basis rotations and offsets
// (Blender's pose basis), turned into joint matrices here on the CPU each frame
// and skinned on the GPU (assets/shaders/altera_game/skinned.hlsl).

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "engine/render/device.hpp"
#include "terrain_core/lighting.hpp"

namespace altera::game {

struct Figure {
    int look = 0;
    // What it plays: clip at time, optionally faded into from `fromClip` at
    // `fromTime` (weight of the old one `fade`, 1 at the switch, 0 when done).
    int clip = 0;
    double time = 0;
    int fromClip = -1;
    double fromTime = 0;
    float fade = 0;
    double rel[3] = {0, 0, 0};   // eye-relative position of the feet
    float yaw = 0;               // facing, radians, 0 = +x
    float scale = 1;
    float highlight = 0;
    float skin[3] = {1, 1, 1}, hair[3] = {1, 1, 1}, cloth[3] = {1, 1, 1};
};

class SkinnedSet {
public:
    // `anim` and `animJson` may be absent (a set with only its rest pose).
    bool load(engine::Device& device, const std::filesystem::path& mesh, const std::filesystem::path& anim,
              const std::filesystem::path& animJson, std::string* why);
    bool loaded() const { return pipeline_ ? true : false; }

    int look(const std::string& name) const;
    int lookCount() const { return int(looks_.size()); }
    const std::string& lookName(int i) const { return looks_[std::size_t(i)].name; }
    int clip(const std::string& name) const;
    int stateClip(const std::string& state) const;            // -1 when none
    int interactionClip(const std::string& interaction) const; // -1 when none
    double clipSeconds(int clip) const;
    bool clipHolds(int clip) const;   // plays once and stays on its last frame

    // Before the frame's passes: poses every figure and uploads.
    void prepare(SDL_GPUCommandBuffer* commands, const std::vector<Figure>& figures);
    void draw(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const float viewProjection[16],
              const terrain_core::LightUniforms& light);

    static constexpr std::size_t kMaxFigures = 96;

private:
    struct Joint {
        int parent = -1;
        std::string name;
    };
    struct Bone {
        float local[16], invRest[16];
    };
    struct Look {
        std::string name;
        std::uint32_t skeleton = 0, firstIndex = 0, indexCount = 0;
        float height = 0;
    };
    struct Clip {
        std::string name;
        std::uint32_t frames = 0, loop = 0, first = 0;
    };
    struct Pose {
        std::vector<float> q, t;   // per joint: x y z w / x y z
        float anchor[2] = {0, 0};
    };
    void sample(int clip, double time, Pose& out) const;
    void palette(const Figure& f, float* rows) const;   // joints x 12 floats

    std::vector<Joint> joints_;
    std::vector<std::vector<Bone>> skeletons_;
    std::vector<Look> looks_;
    std::vector<Clip> clips_;
    std::vector<float> frames_;   // per frame: joints x 7, then anchor x 2
    float fps_ = 15;
    std::map<std::string, int> clipIndex_, stateClips_, interactionClips_;

    engine::Device* device_ = nullptr;
    engine::Buffer vertices_, indices_, palette_, instances_;
    SDL_GPUTransferBuffer* paletteTransfer_ = nullptr;
    SDL_GPUTransferBuffer* instanceTransfer_ = nullptr;
    engine::Texture atlas_;
    engine::Sampler sampler_;
    engine::GraphicsPipeline pipeline_;
    std::vector<int> drawLooks_;   // look of each instance prepared this frame

public:
    SkinnedSet() = default;
    SkinnedSet(const SkinnedSet&) = delete;
    SkinnedSet& operator=(const SkinnedSet&) = delete;
    ~SkinnedSet();
};

// Upload RGBA8 pixels (rows top-down) to a new sampled texture; one copy pass
// on a command buffer of its own.
engine::Texture uploadRgba(engine::Device& device, const std::uint8_t* rgba, std::uint32_t width,
                           std::uint32_t height);

} // namespace altera::game
