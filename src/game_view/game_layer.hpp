#pragma once
// What the world view (terrain_lab's Lab today, altera_world_view after the
// merge, doc engine_merge/MERGE_PLAN.md) asks of the game drawn into it: the
// SceneLayer of the plan, plus input and the camera. The view keeps its frame
// order; the game adds its things to the shared depth and fog, and its
// interface over the finished picture.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "terrain_core/enb.hpp"
#include "terrain_core/lighting.hpp"
#include "terrain_core/world_gen.hpp"

namespace engine { class Device; }
namespace terrain_core { class MacroMap; class BiomeMap; struct Params; struct RiverSegment; class Relief; }

namespace altera::game {

struct CameraRig {
    double eye[3] = {0, 0, 0};
    double yaw = 0, pitch = 0;   // pitch > 0 looks up
    double fovY = 1.0;
};

class GameLayer {
public:
    virtual ~GameLayer() = default;
    // Every SDL event, before the view's own keys. False: the player quits.
    virtual bool event(const SDL_Event& e) = 0;
    // Once a frame before drawing: moves the camera, sets the hour of the day.
    virtual void update(double dt, CameraRig& camera, double& timeOfDay) = 0;
    // Before any render pass of the frame (uploads). `viewProjection` as the
    // view draws with it (eye-relative, row-major, reversed depth).
    virtual void prepare(SDL_GPUCommandBuffer* commands, const double eye[3], const float viewProjection[16],
                         std::uint32_t width, std::uint32_t height) = 0;
    // Inside the frame's opaque pass, after the ground, rocks and plants.
    virtual void drawOpaque(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass,
                            const terrain_core::LightUniforms& light) = 0;
    // Over the finished frame (after post and PSX), before it goes to the screen.
    virtual void drawOverlay(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, std::uint32_t width,
                             std::uint32_t height) = 0;
    // True once after the game laid new pads on the ground (GameOptions::relief,
    // opened again): the view binds the relief and makes its clipmap again.
    virtual bool terrainChanged() { return false; }
    // AL-77: the game's warm lights (fires) near `eye`, eye-relative, for the
    // view's frame chain to add. None by default.
    virtual void lights(const double eye[3], std::vector<terrain_core::EnbLight>& out) { (void)eye; out.clear(); }
};

struct GameOptions {
    std::uint64_t seed = 7;
    std::string season;          // spring | summer | autumn | winter; empty: the world file's
    std::string lang = "ru";
    std::string root;            // the repository (content/, assets/)
    // The world's biome map: the sim's map (forest, marsh, water) is read
    // from it and the village is set in an open, dry, level place.
    const terrain_core::BiomeMap* biomes = nullptr;
    // The river graph (terrain_core/hydrology.hpp): rivers are water on the sim's map (fish, drinking).
    const std::vector<terrain_core::RiverSegment>* rivers = nullptr;
    // The view's fine relief (terrain_core/relief.hpp): the game lays its
    // pads on it (building sites, fields). None: the ground stays as it is.
    terrain_core::Relief* relief = nullptr;
    // AL-69: the world the view is on (kind "lore": the imported Altera), the
    // folder the worlds are kept in, and what to do once open: "new" (a new
    // game on this world), "load:N" (slot N), empty (the main menu)
    terrain_core::WorldSpec world;
    std::string worldsDir;
    std::string begin;
    std::string exe;             // this program, to start it again on another world
};

// The life-sim (altera_sim) on a region of the world centred on (x, y) metres.
std::unique_ptr<GameLayer> makeSimGame(engine::Device& device, SDL_Window* window,
                                       const terrain_core::MacroMap& macro, const terrain_core::Params& params,
                                       double centreX, double centreY, const GameOptions& options,
                                       std::string* why);

} // namespace altera::game
