// terrain_lab - the terrain core alone, on an empty scene, with numbers.
//
// Nothing of the game is linked: a window (or none), the device, the macro
// map, the clipmap and the patches. Two ways to run it:
//
//   interactive   terrain_lab [params] [--map]    fly with WASD/QE, RMB look,
//                                                 wheel = speed, F1 = debug views,
//                                                 M = strategic map view (WASD/drag pan,
//                                                 wheel zoom, RMB/QE rotate)
//                                                 G = another world, another seed (builds, then restarts the lab on it)
//   benchmark     terrain_lab --bench all --headless 1920x1080 --out DIR [params]
//
// The benchmark flies scripted paths at a fixed 1/60 s step - the movement a
// frame at sixty frames a second would make - and writes per-frame CSV and a
// summary JSON per scenario. With --sync (the default for --bench) every
// frame waits for the card, so a frame's time is its CPU and GPU cost together
// rather than a measure of the queue.
//
// Params: --world-km K  --spacing M  --clip N  --grid G  --levels L | --view-km V
//         --range F  --morph F  --heightmap PNG | --generate SIZE  --seed S
//         --height-range M  --sea F  --detail F
// Shots:  --shot PNG --at X,Y,AGL,YAWDEG,PITCHDEG (X, Y as fractions of the world)
// Water:  --water-skeleton PNG (red rivers, blue lakes: the main framework, painted)
//         --water-skeleton-out PNG (the framework used, over the relief, to edit)
//         --main-river-area KM2  --main-lakes N  --main-lake-area KM2 (generated framework)

#include "engine/render/device.hpp"
#include "engine/render/targets.hpp"
#include "terrain_core/biomes.hpp"
#include "terrain_core/drainage.hpp"
#include "terrain_core/dlss.hpp"
#include "terrain_core/erosion.hpp"
#include "terrain_core/foliage.hpp"
#include "terrain_core/grass.hpp"
#include "terrain_core/props.hpp"
#include "terrain_core/post.hpp"
#include "terrain_core/lighting.hpp"
#include "terrain_core/sky.hpp"
#include "terrain_core/water.hpp"
#include "engine/world_source/png_io.hpp"
#include "terrain_core/lod_select.hpp"
#include "terrain_core/macro_map.hpp"
#include "terrain_core/params.hpp"
#include "terrain_core/renderer.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#ifdef _WIN32
#include <process.h>
#endif
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace terrain_core;

namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

struct Camera {
    Vec3d eye;
    double yaw = 0.6, pitch = -0.12;     // pitch > 0 looks up
    double fovY = 60.0 * 3.14159265358979 / 180.0;
    double nearPlane = 0.25;

    void forward(double f[3]) const {
        f[0] = std::cos(pitch) * std::cos(yaw);
        f[1] = std::cos(pitch) * std::sin(yaw);
        f[2] = std::sin(pitch);
    }
    // Eye-relative, reversed depth with the far plane at infinity: depth is
    // near / distance, one at the near plane and falling towards nought.
    void viewProjection(float out[16], double aspect) const {
        double f[3];
        forward(f);
        double r[3] = {f[1], -f[0], 0};
        const double rl = std::hypot(r[0], r[1]);
        r[0] /= rl; r[1] /= rl;
        const double u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
        const double focal = 1.0 / std::tan(fovY * 0.5);
        for (int i = 0; i < 3; ++i) {
            out[0 + i] = float(r[i] * focal / aspect);
            out[4 + i] = float(u[i] * focal);
            out[8 + i] = 0.0f;
            out[12 + i] = float(f[i]);
        }
        out[3] = out[7] = out[15] = 0.0f;
        out[11] = float(nearPlane);
    }
};

struct Options {
    Params params;
    fs::path heightmap;
    fs::path erodeOut;        // --erode: weather the macro map, write it here, exit
    ErosionSettings erosion;
    // Modules: each can be left out from the command line or toggled live
    // (F2 ground materials, F3 trees, 1..9 a tree layer, F4 grass, F5 style).
    bool groundMaterials = true;
    bool surfaceMaps = true;      // the materials' normal and AO maps (F6)
    bool foliage = true;          // the trees module (trees and bushes)
    bool grass = true;            // the grass cover
    bool props = true;            // the mega structures (P)
    fs::path features;            // --features: <world>_features.json; empty: beside a <world>_final.r16 heightmap
    std::uint32_t post = 0;       // picture style: 0 off, 1 dark fantasy, 2 Frazetta, 3 Vallejo
    bool water = true;            // rivers, lakes, sea (F11); off also skips cutting the river beds
    bool dlss = false;            // NVIDIA DLSS Super Resolution, live toggle F12
    int dlssQuality = 0;          // 0 Quality, 1 Balanced, 2 Performance
    HydrologySettings hydrology;
    double clock = 0;             // --clock: the world's clock at the start, seconds (water, wind, clouds)
    fs::path waterPreview;        // --water-preview: the water map as a picture
    fs::path waterSkeleton;       // --water-skeleton: the main lakes and rivers, painted (red rivers, blue lakes)
    fs::path waterRules;          // --water-rules: runoff (R) and where deltas may form (G), from the world generator
    fs::path waterSkeletonOut;    // --water-skeleton-out: the framework used, as such a picture, to edit
    LightSettings light;          // time of day, clouds, wind; shadows/clouds/wind modules (F7 F8 F9, F10 time)
    std::string foliageLayers;   // comma list of layers to keep; empty: all
    float foliageMinPixels = 1.5f;
    DrainageSettings drainage;
    bool drainGenerated = false;  // run the generator's map through the drainage erosion (--drain-generator)
    fs::path biomes;          // 8-bit biome ids; empty: generated
    fs::path ground;          // biome table; empty: content/config/terrain_core/biomes.json
    std::uint32_t generateSize = 0;
    double viewKm = 0;
    std::uint32_t width = 1600, height = 900;
    bool headless = false;
    std::string bench;
    fs::path out = "terrain_lab_out";
    bool sync = false;
    std::string shot;
    double at[5] = {0.5, 0.5, 2.0, 35.0, -8.0};
    int debugLevels = 0;
    double startX = -1, startY = -1;    // fractions of the world; <0: auto (land near centre)
    bool map = false;                   // start in the strategic map view
    bool levelsGiven = false;
    bool seaGiven = false;
};

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--world-km") o.params.worldSize = std::atof(next()) * 1000.0;
        else if (a == "--spacing") o.params.spacing = std::atof(next());
        else if (a == "--clip") o.params.clipSize = std::uint32_t(std::atoi(next()));
        else if (a == "--grid") o.params.grid = std::uint32_t(std::atoi(next()));
        else if (a == "--levels") { o.params.levels = std::uint32_t(std::atoi(next())); o.levelsGiven = true; }
        else if (a == "--view-km") o.viewKm = std::atof(next());
        else if (a == "--range") o.params.rangeFactor = std::atof(next());
        else if (a == "--morph") o.params.morphStart = std::atof(next());
        else if (a == "--seed") o.params.seed = std::uint32_t(std::atoi(next()));
        else if (a == "--height-range") o.params.macroHeightRange = std::atof(next());
        else if (a == "--sea") { o.params.macroSeaLevel = std::atof(next()); o.seaGiven = true; }
        else if (a == "--detail") o.params.detailAmplitude = std::atof(next());
        else if (a == "--heightmap") o.heightmap = next();
        else if (a == "--dlss") o.dlss = true;
        else if (a == "--no-dlss") o.dlss = false;
        else if (a == "--dlss-quality") {
            const std::string mode = next();
            if (mode == "quality") o.dlssQuality = 0;
            else if (mode == "balanced") o.dlssQuality = 1;
            else if (mode == "performance") o.dlssQuality = 2;
            else { std::fprintf(stderr, "--dlss-quality expects quality, balanced or performance\n"); return false; }
        }
        else if (a == "--erode") o.erodeOut = next();
        else if (a == "--erode-scale") o.erosion.scale = std::uint32_t(std::atoi(next()));
        else if (a == "--erode-iterations") o.erosion.iterations = std::uint32_t(std::atoi(next()));
        else if (a == "--talus") o.erosion.talusDegrees = std::atof(next());
        else if (a == "--erode-detail") o.erosion.detail = std::atof(next());
        else if (a == "--drainage-iterations") o.drainage.iterations = std::uint32_t(std::atoi(next()));
        else if (a == "--drainage-res") o.drainage.resolution = std::uint32_t(std::atoi(next()));
        else if (a == "--drainage-k") o.drainage.incision = std::atof(next());
        else if (a == "--drainage-hold") o.drainage.hold = std::atof(next());
        else if (a == "--drainage-talus") o.drainage.talusDegrees = std::atof(next());
        else if (a == "--drain-generator") { o.drainGenerated = true; if (!o.drainage.iterations) o.drainage.iterations = 100; }
        else if (a == "--biomes") o.biomes = next();
        else if (a == "--ground") o.ground = next();
        else if (a == "--generate") o.generateSize = std::uint32_t(std::atoi(next()));
        else if (a == "--headless") {
            o.headless = true;
            std::sscanf(next(), "%ux%u", &o.width, &o.height);
        } else if (a == "--size") std::sscanf(next(), "%ux%u", &o.width, &o.height);
        else if (a == "--bench") { o.bench = next(); o.sync = true; }
        else if (a == "--out") o.out = next();
        else if (a == "--sync") o.sync = true;
        else if (a == "--no-sync") o.sync = false;
        else if (a == "--shot") o.shot = next();
        else if (a == "--at") std::sscanf(next(), "%lf,%lf,%lf,%lf,%lf", &o.at[0], &o.at[1], &o.at[2], &o.at[3], &o.at[4]);
        else if (a == "--start") std::sscanf(next(), "%lf,%lf", &o.startX, &o.startY);
        else if (a == "--debug-levels") o.debugLevels = 1;
        else if (a == "--debug") o.debugLevels = std::atoi(next());
        else if (a == "--map") o.map = true;
        else if (a == "--no-ground") o.groundMaterials = false;
        else if (a == "--no-surface") o.surfaceMaps = false;
        else if (a == "--no-foliage" || a == "--no-trees") o.foliage = false;
        else if (a == "--no-grass") o.grass = false;
        else if (a == "--no-props") o.props = false;
        else if (a == "--features") o.features = next();
        else if (a == "--no-water") o.water = false;
        else if (a == "--water-preview") o.waterPreview = next();
        else if (a == "--water-skeleton") o.waterSkeleton = next();
        else if (a == "--water-skeleton-out") o.waterSkeletonOut = next();
        else if (a == "--water-rules") o.waterRules = next();
        else if (a == "--main-river-area") o.hydrology.mainRiverArea = std::atof(next());
        else if (a == "--main-lakes") o.hydrology.mainLakes = std::uint32_t(std::atoi(next()));
        else if (a == "--main-lake-area") o.hydrology.mainLakeArea = std::atof(next());
        else if (a == "--stream-area") o.hydrology.streamArea = std::atof(next());
        else if (a == "--humidity") o.hydrology.humidity = std::atof(next());
        else if (a == "--time") o.light.timeOfDay = std::atof(next());
        else if (a == "--clock") o.clock = std::atof(next());
        else if (a == "--clouds") o.light.cloudCover = std::atof(next());
        else if (a == "--no-clouds") o.light.clouds = false;
        else if (a == "--no-shadows") o.light.shadows = false;
        else if (a == "--wind") o.light.wind = std::atof(next());
        else if (a == "--no-wind") o.light.windOn = false;
        else if (a == "--post") o.post = std::uint32_t(std::atoi(next())) % Post::kStyles;
        else if (a == "--foliage-layers") o.foliageLayers = next();
        else if (a == "--foliage-min-pixels") o.foliageMinPixels = float(std::atof(next()));
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return false; }
    }
    Params& p = o.params;
    // The generator's coast sits at about 0.11 of its range (MacroMap::generate);
    // an imported map is taken to have its sea at 0 unless told.
    if (!o.seaGiven && o.heightmap.empty()) p.macroSeaLevel = 0.11;
    // Flying it by hand, the levels go on until they cover the world, so the
    // strategic view can pull back over all of it. They cost little: each is
    // a ring of patches further out and one more layer of the clipmap. The
    // benchmark keeps the preset unless told otherwise.
    if (o.viewKm <= 0 && !o.levelsGiven && o.bench.empty() && o.shot.empty() && o.erodeOut.empty()) o.viewKm = p.worldSize * 1.5 / 1000.0;
    if (o.viewKm > 0) {
        p.levels = 1;
        while (p.viewDistance() < o.viewKm * 1000.0 && p.levels < 24) ++p.levels;
    }
    if ((p.clipSize & (p.clipSize - 1)) || (p.grid & (p.grid - 1)) || p.grid < 4) {
        std::fprintf(stderr, "--clip and --grid must be powers of two\n");
        return false;
    }
    return true;
}

fs::path findAssets() {
    for (fs::path at : {fs::current_path(), fs::path(SDL_GetBasePath())}) {
        for (int up = 0; up < 5 && !at.empty(); ++up, at = at.parent_path())
            if (fs::exists(at / "assets" / "shaders" / "terrain_core" / "terrain.hlsl")) return at / "assets" / "sprites";
    }
    return {};
}

double macroHeight(const Params& p, const MacroMap& macro, double x, double y) {
    return (double(macro.sample(x / p.worldSize, y / p.worldSize)) - p.macroSeaLevel) * p.macroHeightRange;
}

// Reads back the height under the eye from clipmap level 0, two frames late:
// enough to keep a walking camera on the ground without stalling for it.
class GroundProbe {
public:
    bool open(engine::Device& device) {
        device_ = &device;
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        info.size = 16;
        for (auto& b : ring_) if (!(b = SDL_CreateGPUTransferBuffer(device.handle(), &info))) return false;
        return true;
    }
    ~GroundProbe() { if (device_) for (auto* b : ring_) if (b) SDL_ReleaseGPUTransferBuffer(device_->handle(), b); }
    void record(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* clipmap, const Params& p, const Vec3d& eye) {
        const auto n = std::int64_t(p.clipSize);
        const auto tx = std::int64_t(std::floor(eye.x / p.spacing)), ty = std::int64_t(std::floor(eye.y / p.spacing));
        auto* copy = SDL_BeginGPUCopyPass(commands);
        SDL_GPUTextureRegion region{};
        region.texture = clipmap;
        region.x = std::uint32_t(((tx % n) + n) % n);
        region.y = std::uint32_t(((ty % n) + n) % n);
        region.layer = 0;
        region.w = region.h = region.d = 1;
        SDL_GPUTextureTransferInfo to{};
        to.transfer_buffer = ring_[frame_ % 3];
        SDL_DownloadFromGPUTexture(copy, &region, &to);
        SDL_EndGPUCopyPass(copy);
        written_[frame_ % 3] = true;
        ++frame_;
    }
    // The newest reading at least two frames old. Call after waitInFlight(1).
    bool read(double& height) {
        if (frame_ < 2 || !written_[(frame_ - 2) % 3]) return false;
        auto* b = ring_[(frame_ - 2) % 3];
        const auto* v = static_cast<const float*>(SDL_MapGPUTransferBuffer(device_->handle(), b, false));
        if (!v) return false;
        height = v[0];
        SDL_UnmapGPUTransferBuffer(device_->handle(), b);
        return std::isfinite(height);
    }
    void reset() { frame_ = 0; for (auto& w : written_) w = false; }

private:
    engine::Device* device_ = nullptr;
    SDL_GPUTransferBuffer* ring_[3] = {};
    bool written_[3] = {};
    std::uint64_t frame_ = 0;
};

struct FrameStats {
    double frameMs = 0, selectMs = 0, recordMs = 0, waitMs = 0;
    std::uint32_t patches = 0, triangles = 0, rects = 0, misses = 0, nodes = 0;
    std::uint64_t texels = 0;
    std::uint64_t foliageInstances = 0, foliageTriangles = 0, foliageCandidates = 0;
    std::uint64_t grassTufts = 0, grassCandidates = 0;
    std::uint64_t propsDrawn = 0, propsTriangles = 0;
    double propsCullMs = 0;
    double x = 0, y = 0, z = 0, ground = 0;
};

struct Lab {
    Options& options;
    engine::Device device;
    engine::Targets targets;
    engine::Targets dlssTargets;
    Dlss dlss;
    MacroMap macro;
    BiomeTable table;
    BiomeMap biomes;
    Renderer renderer;
    Foliage foliage;
    bool foliageReady = false;
    Grass grass;
    bool grassReady = false;
    Props props;
    bool propsReady = false;
    Post post;
    Post dlssComposite;
    bool postReady = false;
    std::uint64_t frames = 0;
    Lighting lighting;
    Water water;
    bool waterReady = false;
    WaterMap waterMap;
    std::vector<RiverSegment> riverSegments;
    Rivers rivers;   // the river graph on the GPU (empty with the water off)
    ShadowAtlas shadows;
    Sky sky;
    Clouds clouds;
    bool lightReady = false;
    double seconds = options.clock;   // the world's clock: wind, clouds, water
    Clock::time_point lastFrame = Clock::now();
    Clipmap clipmap{options.params};
    GroundProbe probe;
    Camera camera;
    double ground = 0;      // last known height under the eye
    bool groundKnown = false;
    bool eroded = false;      // --erode ran; nothing else to do

    explicit Lab(Options& o) : options(o) {}

    bool open(SDL_Window* window) {
        const fs::path assets = findAssets();
        if (assets.empty()) { std::fprintf(stderr, "assets/shaders/terrain_core not found\n"); return false; }
        // A DLSS-capable backend also allows F12 when starting with DLSS off.
        const char* driver = nullptr;
#if ASR_HAS_DLSS
        if (!SDL_GetHint(SDL_HINT_GPU_DRIVER)) driver = "direct3d12";
#endif
        bool ok = window ? device.open(window, assets, driver)
                         : device.openHeadless(options.width, options.height, assets, driver);
        if (!ok && driver) ok = window ? device.open(window, assets)
                                      : device.openHeadless(options.width, options.height, assets);
        if (!ok) { std::fprintf(stderr, "device: %s\n", device.error().c_str()); return false; }
        dlss.open(device);
        std::printf("%s\n", dlss.status().c_str());
        targets.sampledDepth(true);
        dlssTargets.sampledDepth(true);
        if (!targets.resize(device, options.width, options.height)) { std::fprintf(stderr, "targets\n"); return false; }

        const auto loadStart = Clock::now();
        std::string why;
        if (!options.heightmap.empty()) {
            if (!macro.load(options.heightmap, &why)) { std::fprintf(stderr, "heightmap: %s\n", why.c_str()); return false; }
        } else {
            const std::uint32_t size = options.generateSize ? options.generateSize : 2048;
            const fs::path cached = options.out / ("generated_" + std::to_string(size) + "_" +
                                                   std::to_string(options.params.seed) +
                                                   (options.drainGenerated ? "_drained" : "") + ".png");
            if (!fs::exists(cached) || !macro.load(cached, &why)) {
                macro.generate(size, options.params.seed);
                if (options.drainGenerated) {
                    DrainageTimings d;
                    erodeDrainage(macro, options.params, options.drainage, d);
                    std::printf("generator: drainage erosion %.1f s (%u floods %.0f ms, area %.0f ms, incision %.0f ms)\n",
                                d.totalMs / 1000, d.floods, d.floodMs, d.areaMs, d.incisionMs);
                }
                if (!macro.save(cached, &why)) std::fprintf(stderr, "could not cache %s: %s\n", cached.string().c_str(), why.c_str());
            }
        }
        std::printf("macro map %u^2 in %.0f ms (%.1f m/texel)\n", macro.size(), msSince(loadStart),
                    options.params.worldSize / macro.size());
        if (!options.erodeOut.empty()) return erodeAndSave();
        options.params.macroTexel = macro.texelMetres(options.params.worldSize);
        if (options.water) {
            // A painted framework: saturated red is a river, saturated blue a
            // lake; anything else (a grey relief under it, say) is ignored.
            WaterSkeleton painted;
            if (!options.waterSkeleton.empty()) {
                if (auto image = engine::world_source::readPng(options.waterSkeleton, &why); image && image->channels >= 3) {
                    painted.width = image->width;
                    painted.height = image->height;
                    painted.cells.assign(std::size_t(painted.width) * painted.height, 0);
                    const double top = image->maximum();
                    for (std::uint32_t y = 0; y < painted.height; ++y)
                        for (std::uint32_t x = 0; x < painted.width; ++x) {
                            const double r = image->at(x, y, 0) / top, g = image->at(x, y, 1) / top, b = image->at(x, y, 2) / top;
                            painted.cells[std::size_t(y) * painted.width + x] =
                                    r > 0.7 && g < 0.35 && b < 0.35 ? 1 : b > 0.7 && r < 0.35 ? 2 : 0;
                        }
                    options.hydrology.skeleton = &painted;
                    std::printf("water skeleton %ux%u from %s\n", painted.width, painted.height, options.waterSkeleton.string().c_str());
                } else {
                    std::fprintf(stderr, "water skeleton %s: %s\n", options.waterSkeleton.string().c_str(),
                                 image ? "needs an RGB picture" : why.c_str());
                }
            }
            WaterRules rulesImage;
            if (!options.waterRules.empty()) {
                if (auto image = engine::world_source::readPng(options.waterRules, &why); image && image->channels >= 2) {
                    rulesImage.width = image->width;
                    rulesImage.height = image->height;
                    const std::size_t count = std::size_t(image->width) * image->height;
                    rulesImage.runoff.resize(count);
                    rulesImage.delta.resize(count);
                    if (image->channels >= 3) rulesImage.absorb.resize(count);
                    const double top = image->maximum();
                    for (std::uint32_t y = 0; y < image->height; ++y)
                        for (std::uint32_t x = 0; x < image->width; ++x) {
                            const std::size_t at = std::size_t(y) * image->width + x;
                            rulesImage.runoff[at] = std::uint8_t(image->at(x, y, 0) / top * 255.0 + 0.5);
                            rulesImage.delta[at] = std::uint8_t(image->at(x, y, 1) / top * 255.0 + 0.5);
                            if (image->channels >= 3) rulesImage.absorb[at] = std::uint8_t(image->at(x, y, 2) / top * 255.0 + 0.5);
                        }
                    options.hydrology.rules = &rulesImage;
                    std::printf("water rules %ux%u from %s\n", rulesImage.width, rulesImage.height, options.waterRules.string().c_str());
                } else {
                    std::fprintf(stderr, "water rules %s: %s\n", options.waterRules.string().c_str(),
                                 image ? "needs an RGB picture" : why.c_str());
                }
            }
            Hydrology hydro = buildHydrology(macro, options.params, options.hydrology);
            options.hydrology.rules = nullptr;
            options.hydrology.skeleton = nullptr;
            std::printf("framework: %u main lakes, %u main river cells (%s)\n", hydro.mainLakeCount, hydro.mainRiverCells,
                        painted.cells.empty() ? "generated" : "painted");
            if (!options.waterSkeletonOut.empty()) {
                // The relief in grey, the framework over it: open it, paint,
                // and load it back with --water-skeleton.
                engine::world_source::Image picture;
                picture.width = picture.height = hydro.skeletonSize;
                picture.channels = 3;
                picture.bits = 8;
                picture.allocate();
                const std::uint32_t n = hydro.skeletonSize, step = macro.size() / n;
                for (std::uint32_t y = 0; y < n; ++y)
                    for (std::uint32_t x = 0; x < n; ++x) {
                        const double hm = double(macro.texels()[std::size_t(y * step) * macro.size() + x * step]) / 65535.0;
                        const double ground = (hm - options.params.macroSeaLevel) * options.params.macroHeightRange;
                        const auto grey = std::uint16_t(ground <= 0 ? 30 : std::clamp(70.0 + ground / 25.0, 70.0, 230.0));
                        const std::uint8_t kind = hydro.skeleton[std::size_t(y) * n + x];
                        const std::uint32_t py = n - 1 - y;
                        picture.set(x, py, 0, kind == 1 ? 255 : kind == 2 ? 20 : grey);
                        picture.set(x, py, 1, kind == 1 ? 0 : kind == 2 ? 60 : grey);
                        picture.set(x, py, 2, kind == 1 ? 0 : kind == 2 ? 255 : grey);
                    }
                if (engine::world_source::writePng(options.waterSkeletonOut, picture, &why))
                    std::printf("framework written to %s\n", options.waterSkeletonOut.string().c_str());
                else
                    std::fprintf(stderr, "framework: %s\n", why.c_str());
            }
            std::printf("hydrology: %u^2, %u lakes (%u closed, %u cells), river graph %u nodes, %u sources, %u mouths, %u deltas, "
                        "%zu segments, biggest %.0f km^2, %.0f ms\n", hydro.water.size, hydro.lakes, hydro.closedLakes,
                        hydro.water.lakeCells, hydro.nodes, hydro.sources, hydro.mouths, hydro.deltas, hydro.rivers.size(), hydro.biggestKm2,
                        hydro.ms);
            std::sort(hydro.deltaHeads.begin(), hydro.deltaHeads.end(), [](const auto& a, const auto& b) { return a[2] > b[2]; });
            for (std::size_t i = 0; i < std::min<std::size_t>(5, hydro.deltaHeads.size()); ++i)
                std::printf("  delta at %.4f,%.4f (%.0f km^2)\n", hydro.deltaHeads[i][0] / options.params.worldSize,
                            hydro.deltaHeads[i][1] / options.params.worldSize, hydro.deltaHeads[i][2]);
            if (hydro.falls)
                std::printf("  %u fall segments, biggest %.0f m at %.4f,%.4f\n", hydro.falls, hydro.biggestFall[2],
                            hydro.biggestFall[0] / options.params.worldSize, hydro.biggestFall[1] / options.params.worldSize);
            waterMap = std::move(hydro.water);
            riverSegments = std::move(hydro.rivers);
            if (!options.waterPreview.empty()) {
                // Land grey by height, the sea dark, lakes blue, rivers red
                // (brighter and wider the bigger); north up.
                engine::world_source::Image picture;
                picture.width = picture.height = waterMap.size;
                picture.channels = 3;
                picture.bits = 8;
                picture.allocate();
                const std::uint32_t n = waterMap.size, step = macro.size() / n;
                for (std::uint32_t y = 0; y < n; ++y)
                    for (std::uint32_t x = 0; x < n; ++x) {
                        const float* w = &waterMap.texels[(std::size_t(y) * n + x) * 4];
                        const double hm = double(macro.texels()[std::size_t(y * step) * macro.size() + x * step]) / 65535.0;
                        const double ground = (hm - options.params.macroSeaLevel) * options.params.macroHeightRange;
                        std::uint16_t r, g, b;
                        if (w[0] == 0.0f && ground <= 0) { r = 20; g = 40; b = 70; }
                        else if (w[0] > ground) { r = 40; g = 120; b = 255; }
                        else { const auto v = std::uint16_t(std::clamp(80.0 + ground / 30.0, 80.0, 235.0)); r = g = b = v; }
                        const std::uint32_t py = n - 1 - y;
                        picture.set(x, py, 0, r); picture.set(x, py, 1, g); picture.set(x, py, 2, b);
                    }
                const double cell = options.params.worldSize / n;
                for (const RiverSegment& seg : riverSegments) {
                    const double len = std::hypot(seg.x1 - seg.x0, seg.y1 - seg.y0);
                    const int steps = std::max(1, int(std::ceil(len / cell * 2)));
                    const int thick = seg.width0 > cell * 0.5 ? 1 : 0;
                    const auto red = std::uint16_t(120 + 135 * seg.flow);
                    for (int k = 0; k <= steps; ++k) {
                        const double t = double(k) / steps;
                        const int cx = int((seg.x0 + (seg.x1 - seg.x0) * t) / cell);
                        const int cy = int(n - 1) - int((seg.y0 + (seg.y1 - seg.y0) * t) / cell);
                        for (int oy = -thick; oy <= thick; ++oy)
                            for (int ox = -thick; ox <= thick; ++ox) {
                                const int px = cx + ox, py = cy + oy;
                                if (px < 0 || py < 0 || px >= int(n) || py >= int(n)) continue;
                                picture.set(px, py, 0, red); picture.set(px, py, 1, 30); picture.set(px, py, 2, 25);
                            }
                    }
                }
                engine::world_source::writePng(options.waterPreview, picture, &why);
            }
        }
        std::printf("detail from %.0f m wavelength down\n", options.params.topWavelength());

        const auto gpuStart = Clock::now();
        const auto biomeStart = Clock::now();
        const fs::path groundFile = options.ground.empty()
                ? assets.parent_path().parent_path() / "content" / "config" / "terrain_core" / "biomes.json"
                : options.ground;
        if (!table.load(groundFile, options.params, &why)) { std::fprintf(stderr, "ground: %s\n", why.c_str()); return false; }
        if (!options.biomes.empty()) {
            if (!biomes.load(options.biomes, &why)) { std::fprintf(stderr, "biomes: %s\n", why.c_str()); return false; }
        } else {
            biomes.generate(macro, options.params, 1024);
            biomes.save(options.out / ("generated_biomes_" + std::to_string(options.params.seed) + ".png"), &why);
        }
        std::printf("biome map %u^2 (%s) in %.0f ms, %zu materials\n", biomes.size(),
                    options.biomes.empty() ? "generated" : "imported", msSince(biomeStart), table.materialFiles().size());
        if (!rivers.open(device, riverSegments, options.params.worldSize, &why)) {
            std::fprintf(stderr, "rivers: %s\n", why.c_str());
            return false;
        }
        std::printf("river graph on the GPU: %.1f MB, %u tiles a side, longest tile list %u\n",
                    double(rivers.gpuBytes()) / (1 << 20), rivers.tilesPerSide(), rivers.longestList());
        riverSegments.clear();
        riverSegments.shrink_to_fit();
        renderer.setRivers(&rivers);
        if (!renderer.open(device, options.params, macro, table, biomes)) { std::fprintf(stderr, "renderer: %s\n", device.error().c_str()); return false; }
        {
            const auto foliageStart = Clock::now();
            const fs::path foliageFile = assets.parent_path().parent_path() / "content" / "config" / "terrain_core" / "foliage.json";
            foliageReady = foliage.open(device, options.params, foliageFile, renderer.ground(), &why);
            const fs::path spriteDir = assets.parent_path() / "terrain_core" / "grass";
            grassReady = grass.open(device, options.params, GrassSettings{}, spriteDir, &why);
            if (!grassReady) std::fprintf(stderr, "grass off: %s\n", why.c_str());
            postReady = post.open(device);
            if (postReady) dlssComposite.open(device);
            {
                // Opened even with the water off - one dry texel - since the
                // plants' spawns read the water map either way.
                if (!options.water) {
                    waterMap.size = 1;
                    waterMap.texels = {WaterMap::kNoWater, 0.0f, 0.0f, 0.0f};
                }
                waterReady = water.open(device, options.params, waterMap, &why);
                if (!waterReady) std::fprintf(stderr, "water off: %s\n", why.c_str());
                waterMap.texels.clear();
                waterMap.texels.shrink_to_fit();
            }
            lightReady = shadows.open(device) && sky.open(device) && clouds.open(device);
            if (!lightReady) std::fprintf(stderr, "light off: %s\n", device.error().c_str());
            if (!foliageReady) std::fprintf(stderr, "foliage off: %s\n", why.c_str());
            else {
                if (!options.foliageLayers.empty())
                    for (auto& layer : foliage.layers())
                        layer.enabled = ("," + options.foliageLayers + ",").find("," + layer.name + ",") != std::string::npos;
                std::printf("foliage: %zu layers, %u models, %.1f MB, in %.0f ms\n", foliage.layers().size(), foliage.modelCount(),
                            double(foliage.gpuBytes()) / (1 << 20), msSince(foliageStart));
            }
        }
        {
            // The mega structures: the world generator's feature list beside
            // its heightmap (tools/terrain_lab/world_features.py).
            fs::path features = options.features;
            const std::string hm = options.heightmap.string();
            const std::string tail = "_final.r16";
            if (features.empty() && hm.size() > tail.size() && hm.compare(hm.size() - tail.size(), tail.size(), tail) == 0)
                features = hm.substr(0, hm.size() - tail.size()) + "_features.json";
            if (!features.empty() && fs::exists(features)) {
                const auto propsStart = Clock::now();
                const fs::path root = assets.parent_path().parent_path();
                const fs::path groundFile = options.ground.empty()
                        ? root / "content" / "config" / "terrain_core" / "biomes.json" : options.ground;
                const Params& p = options.params;
                propsReady = props.open(device, features, assets.parent_path() / "terrain_core" / "props", groundFile,
                                        [&](double x, double y) { return macroHeight(p, macro, x, y); }, &why);
                std::error_code age;
                if (!options.heightmap.empty() &&
                    fs::last_write_time(features, age) < fs::last_write_time(options.heightmap, age) && !age)
                    std::fprintf(stderr, "props: %s is older than the heightmap - run tools/terrain_lab/world_features.py\n",
                                 features.string().c_str());
                if (!propsReady) std::fprintf(stderr, "props off: %s\n", why.c_str());
                else std::printf("props: %zu instances of %u models, %.1f MB, in %.0f ms\n", props.instanceCount(),
                                 props.modelCount(), double(props.gpuBytes()) / (1 << 20), msSince(propsStart));
            }
        }
        if (!probe.open(device)) return false;
        std::printf("renderer ready in %.0f ms (shaders), GPU %.1f MB\n", msSince(gpuStart),
                    double(renderer.gpuBytes()) / (1 << 20));
        const Params& p = options.params;
        std::printf("params: world %.0f km, spacing %.2f m, clip %u, grid %u, levels %u, range x%.2f -> view %.1f km\n",
                    p.worldSize / 1000, p.spacing, p.clipSize, p.grid, p.levels, p.rangeFactor, p.viewDistance() / 1000);
        return true;
    }

    // --erode: weather the loaded map, write it and a shaded preview beside it.
    bool erodeAndSave() {
        const ErosionSettings& e = options.erosion;
        std::printf("eroding: %u^2 -> %u^2 (%.1f m/texel), %u thermal passes at %.0f deg\n", macro.size(),
                    macro.size() * e.scale, options.params.worldSize / (macro.size() * e.scale), e.iterations,
                    e.talusDegrees);
        MacroMap out;
        ErosionTimings t;
        std::string why;
        const auto total = Clock::now();
        if (options.drainage.iterations > 0) {
            DrainageTimings d;
            erodeDrainage(macro, options.params, options.drainage, d);
            std::printf("drainage: %u iterations on %u^2: %.1f s (%u floods %.0f ms, area %.0f ms, incision %.0f ms)\n",
                        options.drainage.iterations, std::min(macro.size(), options.drainage.resolution), d.totalMs / 1000,
                        d.floods, d.floodMs, d.areaMs, d.incisionMs);
        }
        if (!erode(device, macro, options.params, e, out, t, &why)) { std::fprintf(stderr, "erosion: %s\n", why.c_str()); return false; }
        const auto saveStart = Clock::now();
        if (!out.save(options.erodeOut, &why)) { std::fprintf(stderr, "save: %s\n", why.c_str()); return false; }
        const double saveMs = msSince(saveStart);
        // A hillshade preview, 2048 a side.
        const std::uint32_t side = std::min<std::uint32_t>(2048, out.size()), step = out.size() / side;
        engine::world_source::Image preview;
        preview.width = preview.height = side;
        preview.channels = 1;
        preview.bits = 8;
        preview.allocate();
        const double texel = options.params.worldSize / out.size() * step;
        for (std::uint32_t y = 1; y + 1 < side; ++y)
            for (std::uint32_t x = 1; x + 1 < side; ++x) {
                auto at = [&](std::uint32_t a, std::uint32_t b) {
                    return double(out.texels()[std::size_t(b * step) * out.size() + a * step]) / 65535.0 * options.params.macroHeightRange;
                };
                const double dx = (at(x + 1, y) - at(x - 1, y)) / (2 * texel), dy = (at(x, y + 1) - at(x, y - 1)) / (2 * texel);
                const double shade = std::clamp((-dx * 0.6 - dy * 0.5 + 1.0) / std::sqrt(dx * dx + dy * dy + 1.0) * 0.75, 0.0, 1.0);
                preview.set(x, y, 0, std::uint16_t(shade * 255));
            }
        fs::path previewPath = options.erodeOut;
        previewPath.replace_extension(".preview.png");
        engine::world_source::writePng(previewPath, preview, &why);
        std::printf("erosion: upload %.0f ms, upscale %.0f ms, thermal %.0f ms (%.1f ms/pass), readback %.0f ms, convert %.0f ms, "
                    "save %.0f ms; total %.1f s\n", t.uploadMs, t.upscaleMs, t.thermalMs, t.thermalMs / std::max(1u, e.iterations),
                    t.readbackMs, t.convertMs, saveMs, msSince(total) / 1000.0);
        std::printf("wrote %s (%u^2) and %s\n", options.erodeOut.string().c_str(), out.size(), previewPath.string().c_str());
        eroded = true;
        return true;
    }

    void place(double fx, double fy, double agl) {
        camera.eye.x = fx * options.params.worldSize;
        camera.eye.y = fy * options.params.worldSize;
        ground = std::max(0.0, macroHeight(options.params, macro, camera.eye.x, camera.eye.y));
        groundKnown = false;
        probe.reset();
        camera.eye.z = ground + agl;
    }

    // Somewhere on land near the middle of the world, for the default start.
    void findLand(double& fx, double& fy) {
        for (int ring = 0; ring < 64; ++ring)
            for (int k = 0; k < 16; ++k) {
                const double a = k * 0.3926991 + ring * 0.7, r = ring * 0.006;
                const double x = 0.5 + r * std::cos(a), y = 0.5 + r * std::sin(a);
                const double h = macroHeight(options.params, macro, x * options.params.worldSize, y * options.params.worldSize);
                if (h > 40 && h < 0.4 * options.params.macroHeightRange) { fx = x; fy = y; return; }
            }
        fx = fy = 0.5;
    }

    // One frame: follow, select, generate, draw, (present). Returns its numbers.
    FrameStats frame(SDL_Window* window, double keepAgl /* < 0: free camera */) {
        FrameStats s;
        const auto start = Clock::now();
        device.waitInFlight(1);
        double probed = 0;
        if (probe.read(probed)) { ground = probed; groundKnown = true; }
        if (keepAgl >= 0) camera.eye.z = ground + keepAgl;

        const Params& p = options.params;
        int outputWidth = int(options.width), outputHeight = int(options.height);
        if (window) SDL_GetWindowSizeInPixels(window, &outputWidth, &outputHeight);
        outputWidth = std::max(1, outputWidth); outputHeight = std::max(1, outputHeight);
        if (!targets.resize(device, Uint32(outputWidth), Uint32(outputHeight))) return s;
        bool useDlss = dlss.prepare(options.dlss, Uint32(outputWidth), Uint32(outputHeight), options.dlssQuality);
        if (useDlss && !dlssTargets.resize(device, dlss.renderWidth(), dlss.renderHeight())) {
            options.dlss = false;
            dlss.prepare(false, Uint32(outputWidth), Uint32(outputHeight), options.dlssQuality);
            useDlss = false;
        }
        auto& sceneTargets = useDlss ? dlssTargets : targets;
        renderer.setTextureScale(useDlss ? float(dlss.renderWidth()) / float(outputWidth) : 1.0f);
        const double aspect = double(targets.width()) / double(targets.height());
        ViewUniforms view{};
        camera.viewProjection(view.viewProjection, aspect);
        float unjittered[16];
        std::memcpy(unjittered, view.viewProjection, sizeof unjittered);
        const auto selectStart = Clock::now();
        const auto rects = clipmap.follow(camera.eye);
        const Selection selection = select(p, macro, clipmap, camera.eye, frustumFrom(view.viewProjection));
        if (useDlss) dlss.jitter(view.viewProjection);
        s.selectMs = msSince(selectStart);

        // The world's clock: a fixed sixtieth a frame when measuring (so a run is
        // the same run every time), the real time otherwise.
        const double dt = options.sync ? 1.0 / 60.0 : std::min(0.1, msSince(lastFrame) / 1000.0);
        lastFrame = Clock::now();
        seconds += dt;
        double forward[3];
        camera.forward(forward);
        lighting.update(options.light, camera.eye, forward, camera.fovY, aspect, seconds);
        const LightUniforms& light = lighting.uniforms();
        view.eyeZ = float(camera.eye.z);
        view.spacing = float(p.spacing);
        view.clipSize = p.clipSize;
        view.grid = p.grid;
        view.levels = p.levels;
        view.rangeFactor = float(p.rangeFactor);
        view.morphStart = float(p.morphStart);
        PlantView plants{};
        std::memcpy(plants.viewProjection, view.viewProjection, sizeof(plants.viewProjection));
        plants.wind[0] = light.windDirection[0];
        plants.wind[1] = light.windDirection[1];
        plants.wind[2] = light.wind;
        plants.wind[3] = light.time;
        plants.eyeWrapped[0] = float(std::fmod(camera.eye.x, 4096.0));
        plants.eyeWrapped[1] = float(std::fmod(camera.eye.y, 4096.0));
        const bool shadowsOn = lightReady && options.light.shadows;

        const auto recordStart = Clock::now();
        auto* commands = SDL_AcquireGPUCommandBuffer(device.handle());
        renderer.upload(commands, selection);
        if (shadowsOn) {
            // The shadow casters: every patch near enough, whichever way the eye looks.
            Params near = p;
            near.levels = std::min<std::uint32_t>(p.levels, 7);
            renderer.uploadShadow(commands, select(near, macro, clipmap, camera.eye, Frustum{}));
        }
        s.texels = renderer.generate(commands, rects);
        if (foliageReady && options.foliage && waterReady) {   // the spawn reads the water map
            FoliageFrame f;
            f.eye = camera.eye;
            f.frustum = frustumFrom(unjittered);
            std::memcpy(f.viewProjection, view.viewProjection, sizeof(f.viewProjection));
            f.pixelScale = float(double(targets.height()) * 0.5 / std::tan(camera.fovY * 0.5));
            f.minPixels = options.foliageMinPixels;
            foliage.spawn(commands, f, renderer.clipmap(), renderer.biomeMap(), renderer.biomeSize(), water.map(),
                          water.mapSize(), rivers);
            s.foliageInstances = foliage.drawnInstances();
            s.foliageTriangles = foliage.drawnTriangles();
            s.foliageCandidates = foliage.candidates();
        }
        if (grassReady && options.grass && waterReady) {
            GrassFrame g;
            g.eye = camera.eye;
            g.frustum = frustumFrom(unjittered);
            g.pixelScale = float(double(targets.height()) * 0.5 / std::tan(camera.fovY * 0.5));
            for (int i = 0; i < 3; ++i) {
                g.sunDirection[i] = light.sun[i];
                g.skyColour[i] = light.zenith[i];
            }
            GroundUniforms ground = renderer.ground();
            grass.spawn(commands, g, renderer.clipmap(), renderer.biomeMap(), renderer.materials(),
                        renderer.materialNormals(), renderer.materialProperties(), water.map(), water.mapSize(), rivers,
                        ground, light);
            s.grassTufts = grass.tufts();
            s.grassCandidates = grass.candidates();
        }
        if (propsReady && options.props) {
            PropsFrame f;
            f.eye = camera.eye;
            f.frustum = frustumFrom(unjittered);
            f.pixelScale = float(double(targets.height()) * 0.5 / std::tan(camera.fovY * 0.5));
            f.shadowRange = shadowsOn ? double(light.cascadeFar[2]) : 0.0;
            f.detailBuilds = options.shot.empty() ? 1 : 64;   // a still picture: every near cliff refined at once
            props.prepare(commands, f);
            s.propsDrawn = props.drawn();
            s.propsTriangles = props.trianglesDrawn();
            s.propsCullMs = props.cullMs();
        }
        if (shadowsOn) {
            // The sun's shadow atlas: the ground and the trees, three cascades.
            auto* shadowPass = shadows.begin(commands);
            for (std::uint32_t c = 0; c < Lighting::kCascades; ++c) {
                shadows.viewport(shadowPass, c);
                ViewUniforms lightView = view;
                std::memcpy(lightView.viewProjection, lighting.cascadeMatrix(c), sizeof(lightView.viewProjection));
                renderer.drawShadow(commands, shadowPass, lightView);
                if (foliageReady && options.foliage) {
                    PlantView lightPlants = plants;
                    std::memcpy(lightPlants.viewProjection, lighting.cascadeMatrix(c), sizeof(lightPlants.viewProjection));
                    foliage.drawShadow(commands, shadowPass, lightPlants);
                }
                if (propsReady && options.props) {
                    PlantView lightPlants = plants;
                    std::memcpy(lightPlants.viewProjection, lighting.cascadeMatrix(c), sizeof(lightPlants.viewProjection));
                    props.drawShadow(commands, shadowPass, lightPlants);
                }
            }
            SDL_EndGPURenderPass(shadowPass);
        }
        SDL_GPUTexture* swapchain = nullptr;
        Uint32 sw = 0, sh = 0;
        if (window && !SDL_WaitAndAcquireGPUSwapchainTexture(commands, window, &swapchain, &sw, &sh)) swapchain = nullptr;

        SDL_GPUColorTargetInfo colour{};
        colour.texture = sceneTargets.colour();
        colour.load_op = SDL_GPU_LOADOP_CLEAR;
        colour.store_op = SDL_GPU_STOREOP_STORE;
        colour.clear_color = {light.horizon[0], light.horizon[1], light.horizon[2], 1};
        SDL_GPUDepthStencilTargetInfo depth{};
        depth.texture = sceneTargets.depth();
        depth.clear_depth = 0;
        depth.load_op = SDL_GPU_LOADOP_CLEAR;
        depth.store_op = SDL_GPU_STOREOP_STORE;   // the clouds read it
        depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        auto* pass = SDL_BeginGPURenderPass(commands, &colour, 1, &depth);
        RayCamera rays = rayCamera(forward, camera.fovY, aspect);
        if (useDlss) {
            const auto jitter = dlss.jitterOffset();
            for (int i = 0; i < 3; ++i)
                rays.forward[i] += 2 * jitter[0] / float(sceneTargets.width()) * rays.right[i]
                                 - 2 * jitter[1] / float(sceneTargets.height()) * rays.up[i];
        }
        if (lightReady) sky.draw(commands, pass, light, rays);
        LookUniforms look{};
        look.debugLevels = std::uint32_t(options.debugLevels);
        look.clipSize = p.clipSize;
        look.levels = p.levels;
        look.flags = (options.groundMaterials ? 0u : kLookPlainGround) | (options.surfaceMaps ? 0u : kLookNoSurfaceMaps);
        renderer.draw(commands, pass, view, look, light, shadows.texture(), shadows.sampler());
        if (propsReady && options.props) {
            PropsLook rock;
            rock.tileMetres = float(std::exp2(renderer.ground().tileLog2) * p.spacing);
            rock.textureScale = renderer.ground().textureScale;
            rock.mossLayer = props.mossLayer();
            props.draw(commands, pass, plants, light, rock, renderer.materials(), renderer.materialNormals(),
                       renderer.materialProperties(), shadows.texture(), shadows.sampler());
        }
        if (foliageReady && options.foliage) foliage.draw(commands, pass, plants, light, shadows.texture(), shadows.sampler());
        if (grassReady && options.grass) grass.draw(commands, pass, plants, light, shadows.texture(), shadows.sampler());
        if (waterReady && options.water)
            water.draw(commands, pass, renderer, view, light, shadows.texture(), shadows.sampler(), camera.eye, float(seconds));
        SDL_EndGPURenderPass(pass);

        const bool cloudsDrawn = lightReady && clouds.march(commands, sceneTargets, light, rays, float(camera.nearPlane),
                                                            std::uint32_t(frames));
        if (useDlss) {
            // Composite clouds before DLSS; grade and grain at display resolution.
            if (postReady && cloudsDrawn) dlssComposite.apply(commands, sceneTargets, 0, float(seconds), clouds.texture());
            const double eye[3] = {camera.eye.x, camera.eye.y, camera.eye.z};
            auto* upscaled = dlss.evaluate(commands, sceneTargets.resolved(), sceneTargets.depth(), unjittered, eye, float(dt * 1000));
            SDL_GPUBlitInfo upscale{};
            upscale.source.texture = upscaled ? upscaled : sceneTargets.resolved();
            upscale.source.w = upscaled ? targets.width() : sceneTargets.width();
            upscale.source.h = upscaled ? targets.height() : sceneTargets.height();
            upscale.destination.texture = targets.colour();
            upscale.destination.w = targets.width(); upscale.destination.h = targets.height();
            upscale.load_op = SDL_GPU_LOADOP_DONT_CARE;
            upscale.filter = SDL_GPU_FILTER_LINEAR;
            SDL_BlitGPUTexture(commands, &upscale);
            if (!upscaled) options.dlss = false;
            if (postReady) post.apply(commands, targets, options.post, float(seconds));
        } else if (postReady) post.apply(commands, targets, options.post, float(seconds), cloudsDrawn ? clouds.texture() : nullptr);
        ++frames;
        probe.record(commands, renderer.clipmap(), p, camera.eye);
        if (swapchain) {
            SDL_GPUBlitInfo blit{};
            blit.source.texture = targets.resolved();
            blit.source.w = targets.width();
            blit.source.h = targets.height();
            blit.destination.texture = swapchain;
            blit.destination.w = sw;
            blit.destination.h = sh;
            blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
            blit.filter = SDL_GPU_FILTER_LINEAR;
            SDL_BlitGPUTexture(commands, &blit);
        }
        device.submitFrame(commands);
        s.recordMs = msSince(recordStart);
        if (options.sync) {
            const auto waitStart = Clock::now();
            device.waitInFlight(0);
            s.waitMs = msSince(waitStart);
        }
        s.frameMs = msSince(start);
        s.patches = selection.patches;
        s.triangles = renderer.trianglesDrawn();
        s.rects = std::uint32_t(rects.size());
        s.misses = selection.coverageMisses;
        s.nodes = selection.nodesVisited;
        s.x = camera.eye.x; s.y = camera.eye.y; s.z = camera.eye.z; s.ground = ground;
        return s;
    }
};

// ---- benchmark ------------------------------------------------------------

struct Scenario {
    const char* name;
    int frames;
    double speed;        // m/s along the heading
    double agl;          // height above ground; < 0: climb from 2 m to -agl
    bool teleport;       // jump somewhere else every second
};

const Scenario kScenarios[] = {
        {"idle", 240, 0, 2, false},
        {"walk", 600, 5, 1.8, false},
        {"ride", 600, 20, 2.5, false},
        {"fly", 600, 300, 150, false},
        {"jet", 600, 3000, 1500, false},
        {"climb", 600, 0, -20000, false},
        {"teleport", 600, 0, 2, true},
};

double percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, std::size_t(q * double(v.size())))];
}

int runBench(Lab& lab, SDL_Window* window) {
    Options& o = lab.options;
    std::error_code ignored;
    fs::create_directories(o.out, ignored);
    double sx = o.startX, sy = o.startY;
    if (sx < 0 || sy < 0) lab.findLand(sx, sy);
    const Params& p = o.params;

    std::string summary = "{\n  \"params\": {";
    char buf[1024];
    std::snprintf(buf, sizeof buf,
                  "\"worldKm\": %.1f, \"spacing\": %.3f, \"clip\": %u, \"grid\": %u, \"levels\": %u, "
                  "\"range\": %.2f, \"morph\": %.2f, \"viewKm\": %.2f, \"clipmapMB\": %.1f, \"gpuMB\": %.1f, "
                  "\"width\": %u, \"height\": %u, \"start\": [%.4f, %.4f]},\n  \"scenarios\": [\n",
                  p.worldSize / 1000, p.spacing, p.clipSize, p.grid, p.levels, p.rangeFactor, p.morphStart,
                  p.viewDistance() / 1000, double(p.clipmapBytes()) / (1 << 20), double(lab.renderer.gpuBytes()) / (1 << 20),
                  lab.targets.width(), lab.targets.height(), sx, sy);
    summary += buf;

    std::printf("%-9s %7s %7s %7s %7s %7s | %6s %6s | %7s %9s %9s %6s | %8s %9s\n", "scenario", "mean", "p50", "p95", "p99", "max",
                "selCPU", "recCPU", "patches", "tris", "texels/f", "misses", "fol.inst", "fol.tris");
    bool first = true;
    for (const Scenario& sc : kScenarios) {
        if (o.bench != "all" && o.bench != sc.name) continue;
        lab.place(sx, sy, sc.agl >= 0 ? sc.agl : 2.0);
        lab.clipmap.invalidate();
        lab.camera.yaw = 0.6;
        lab.camera.pitch = sc.agl > 100 ? -0.25 : -0.08;
        // Warm-up: the first full generation and the readback of the ground,
        // measured separately (it is what a teleport costs).
        FrameStats warm = lab.frame(window, sc.agl >= 0 ? sc.agl : 2.0);
        const double coldMs = warm.frameMs;
        for (int i = 0; i < 3; ++i) lab.frame(window, sc.agl >= 0 ? sc.agl : 2.0);

        std::vector<FrameStats> frames;
        frames.reserve(sc.frames);
        std::uint32_t rng = 12345;
        const double dt = 1.0 / 60.0;
        for (int f = 0; f < sc.frames; ++f) {
            if (window) {
                SDL_Event e;
                while (SDL_PollEvent(&e)) if (e.type == SDL_EVENT_QUIT) return 1;
            }
            double f3[3];
            lab.camera.forward(f3);
            const double h = std::hypot(f3[0], f3[1]);
            lab.camera.eye.x += sc.speed * dt * f3[0] / h;
            lab.camera.eye.y += sc.speed * dt * f3[1] / h;
            double agl = sc.agl;
            if (sc.agl < 0) agl = 2.0 * std::pow(-sc.agl / 2.0, double(f) / double(sc.frames - 1));
            if (sc.teleport && f % 60 == 59) {
                rng = rng * 1664525u + 1013904223u;
                const double tx = 0.1 + 0.8 * double(rng >> 8) / double(1u << 24);
                rng = rng * 1664525u + 1013904223u;
                const double ty = 0.1 + 0.8 * double(rng >> 8) / double(1u << 24);
                lab.place(tx, ty, 2.0);
            }
            frames.push_back(lab.frame(window, agl));
        }

        const fs::path csv = o.out / (std::string(sc.name) + ".csv");
        std::ofstream out(csv);
        out << "frame,frame_ms,select_ms,record_ms,wait_ms,patches,triangles,rects,texels,misses,nodes,x,y,z,ground,foliage_instances,foliage_triangles,foliage_candidates\n";
        std::vector<double> ms, sel, rec;
        double texelSum = 0, texelMax = 0, patchMax = 0, triMax = 0, misses = 0, foliageMax = 0, foliageTriMax = 0;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const FrameStats& s = frames[i];
            std::snprintf(buf, sizeof buf, "%zu,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%llu,%u,%u,%.2f,%.2f,%.2f,%.2f,%llu,%llu,%llu\n", i, s.frameMs,
                          s.selectMs, s.recordMs, s.waitMs, s.patches, s.triangles, s.rects,
                          (unsigned long long)s.texels, s.misses, s.nodes, s.x, s.y, s.z, s.ground,
                          (unsigned long long)s.foliageInstances, (unsigned long long)s.foliageTriangles,
                          (unsigned long long)s.foliageCandidates);
            out << buf;
            ms.push_back(s.frameMs);
            sel.push_back(s.selectMs);
            rec.push_back(s.recordMs);
            texelSum += double(s.texels);
            texelMax = std::max(texelMax, double(s.texels));
            patchMax = std::max(patchMax, double(s.patches));
            foliageMax = std::max(foliageMax, double(s.foliageInstances));
            foliageTriMax = std::max(foliageTriMax, double(s.foliageTriangles));
            triMax = std::max(triMax, double(s.triangles));
            misses += s.misses;
        }
        double mean = 0;
        for (double v : ms) mean += v;
        mean /= double(ms.size());
        double selMean = 0, recMean = 0;
        for (double v : sel) selMean += v;
        for (double v : rec) recMean += v;
        selMean /= double(sel.size());
        recMean /= double(rec.size());
        std::printf("%-9s %7.2f %7.2f %7.2f %7.2f %7.2f | %6.2f %6.2f | %7.0f %9.0f %9.0f %6.0f | %8.0f %9.0f   cold %.1f ms\n", sc.name,
                    mean, percentile(ms, 0.5), percentile(ms, 0.95), percentile(ms, 0.99), percentile(ms, 1.0), selMean,
                    recMean, patchMax, triMax, texelSum / double(frames.size()), misses, foliageMax, foliageTriMax, coldMs);
        std::snprintf(buf, sizeof buf,
                      "%s    {\"name\": \"%s\", \"frames\": %zu, \"meanMs\": %.3f, \"p50Ms\": %.3f, \"p95Ms\": %.3f, "
                      "\"p99Ms\": %.3f, \"maxMs\": %.3f, \"selectMs\": %.3f, \"recordMs\": %.3f, \"coldMs\": %.3f, "
                      "\"patchesMax\": %.0f, \"trianglesMax\": %.0f, \"texelsPerFrame\": %.0f, \"texelsMax\": %.0f, "
                      "\"coverageMisses\": %.0f, \"foliageInstancesMax\": %.0f, \"foliageTrianglesMax\": %.0f, "
                      "\"dlssActive\": %s, \"dlssQuality\": %d, \"renderWidth\": %u, \"renderHeight\": %u}",
                      first ? "" : ",\n", sc.name, frames.size(), mean, percentile(ms, 0.5), percentile(ms, 0.95),
                      percentile(ms, 0.99), percentile(ms, 1.0), selMean, recMean, coldMs, patchMax, triMax,
                      texelSum / double(frames.size()), texelMax, misses, foliageMax, foliageTriMax,
                      lab.dlss.active() ? "true" : "false", o.dlssQuality,
                      lab.dlss.active() ? lab.dlss.renderWidth() : lab.targets.width(),
                      lab.dlss.active() ? lab.dlss.renderHeight() : lab.targets.height());
        summary += buf;
        first = false;
    }
    summary += "\n  ]\n}\n";
    std::ofstream(o.out / "summary.json") << summary;
    return 0;
}

int runShot(Lab& lab, SDL_Window* window) {
    Options& o = lab.options;
    lab.place(o.at[0], o.at[1], o.at[2]);
    lab.camera.yaw = o.at[3] * 3.14159265358979 / 180.0;
    lab.camera.pitch = o.at[4] * 3.14159265358979 / 180.0;
    for (int i = 0; i < 4; ++i) lab.frame(window, o.at[2]);
    lab.device.waitInFlight(0);
    if (lab.propsReady)   // detailMs: the upload of the last refined cliff (the refining is off the frame)
        std::printf("props: %u drawn, %.2f M triangles, %u cliff tiles refined (last %.1f ms)\n", lab.props.drawn(),
                    double(lab.props.trianglesDrawn()) / 1e6, lab.props.detailDrawn(), lab.props.detailMs());
    if (!engine::saveTargetAsPng(lab.device, lab.targets, o.shot)) {
        std::fprintf(stderr, "could not save %s\n", o.shot.c_str());
        return 1;
    }
    std::printf("shot %s (eye %.1f %.1f %.1f, ground %.1f)\n", o.shot.c_str(), lab.camera.eye.x, lab.camera.eye.y,
                lab.camera.eye.z, lab.ground);
    return 0;
}

// Strategic map camera: looks down at a point on the ground from a distance,
// pans over the world, zooms from a few dozen metres to the whole world.
struct MapCamera {
    double x = 0, y = 0;                    // the point looked at
    double distance = 3000;                 // eye to that point, metres
    double yaw = 1.5707963, pitch = 0.95;   // pitch: radians below the horizon

    void apply(Camera& camera, double groundAtTarget) const {
        camera.yaw = yaw;
        camera.pitch = -pitch;
        double f[3];
        camera.forward(f);
        camera.eye.x = x - f[0] * distance;
        camera.eye.y = y - f[1] * distance;
        camera.eye.z = groundAtTarget - f[2] * distance;
    }
};

int runInteractive(Lab& lab, SDL_Window* window) {
    const Params& p = lab.options.params;
    double sx = lab.options.startX, sy = lab.options.startY;
    if (sx < 0 || sy < 0) lab.findLand(sx, sy);
    lab.place(sx, sy, 2.0);
    double speed = 10.0, agl = 2.0;
    bool looking = false, dragging = false, map = lab.options.map;
    MapCamera strategic;
    strategic.x = lab.camera.eye.x;
    strategic.y = lab.camera.eye.y;
    if (map) strategic.distance = p.worldSize * 0.6;
    auto last = Clock::now(), titleAt = Clock::now();
    std::vector<double> window_ms;
    FrameStats s;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) return 0;
            if (e.type == SDL_EVENT_KEY_DOWN) {
                if (e.key.key == SDLK_ESCAPE) return 0;
                if (e.key.key == SDLK_F1) lab.options.debugLevels = (lab.options.debugLevels + 1) % 6;
                if (e.key.key == SDLK_F2) lab.options.groundMaterials = !lab.options.groundMaterials;
                if (e.key.key == SDLK_F3) lab.options.foliage = !lab.options.foliage;
                if (e.key.key == SDLK_F4) lab.options.grass = !lab.options.grass;
                if (e.key.key == SDLK_P) lab.options.props = !lab.options.props;
                if (e.key.key == SDLK_F5) lab.options.post = (lab.options.post + 1) % Post::kStyles;
                if (e.key.key == SDLK_F6) lab.options.surfaceMaps = !lab.options.surfaceMaps;
                if (e.key.key == SDLK_F7) lab.options.light.shadows = !lab.options.light.shadows;
                if (e.key.key == SDLK_F8) lab.options.light.clouds = !lab.options.light.clouds;
                if (e.key.key == SDLK_F9) lab.options.light.windOn = !lab.options.light.windOn;
                if (e.key.key == SDLK_F11 && lab.waterReady) lab.options.water = !lab.options.water;
                if (e.key.key == SDLK_F12 && !e.key.repeat) {
                    lab.options.dlss = !lab.options.dlss;
                    lab.dlss.resetHistory();
                    SDL_Log("DLSS requested %s: %s", lab.options.dlss ? "on" : "off", lab.dlss.status().c_str());
                }
                if (e.key.key == SDLK_F10) {
                    const double times[] = {7.0, 9.5, 13.0, 16.5, 18.8, 20.2};
                    std::size_t next = 0;
                    while (next < 6 && times[next] <= lab.options.light.timeOfDay + 0.01) ++next;
                    lab.options.light.timeOfDay = times[next % 6];
                }
                if (e.key.key >= SDLK_1 && e.key.key <= SDLK_9 && lab.foliageReady) {
                    const std::size_t i = std::size_t(e.key.key - SDLK_1);
                    if (i < lab.foliage.layers().size()) lab.foliage.layers()[i].enabled = !lab.foliage.layers()[i].enabled;
                }
                if (e.key.key == SDLK_G && !e.key.repeat) {
                    // Another world by the same rules, another seed: built in a console
                    // window of its own, then this lab is replaced by one on the new world.
                    std::system(("start \"world\" python tools/terrain_lab/regenerate.py --pid " + std::to_string(int(_getpid())) + "").c_str());
                    SDL_SetWindowTitle(window, "terrain_lab - building a new world (see the console window)...");
                }
                if (e.key.key == SDLK_M) {
                    map = !map;
                    if (map) {
                        // Look at the ground a little ahead of where the eye stood.
                        double f[3];
                        lab.camera.forward(f);
                        const double fh = std::hypot(f[0], f[1]);
                        strategic.yaw = std::atan2(f[1], f[0]);
                        strategic.distance = std::max(300.0, agl * 4.0);
                        strategic.x = lab.camera.eye.x + f[0] / fh * strategic.distance * 0.5;
                        strategic.y = lab.camera.eye.y + f[1] / fh * strategic.distance * 0.5;
                    } else {
                        // Drop to the ground at the point that was looked at.
                        lab.camera.eye.x = strategic.x;
                        lab.camera.eye.y = strategic.y;
                        lab.camera.yaw = strategic.yaw;
                        lab.camera.pitch = -0.1;
                        agl = 2.0;
                    }
                }
            }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_RIGHT) {
                looking = true;
                SDL_SetWindowRelativeMouseMode(window, true);
            }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_RIGHT) {
                looking = false;
                SDL_SetWindowRelativeMouseMode(window, false);
            }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_MIDDLE))
                dragging = true;
            if (e.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_MIDDLE))
                dragging = false;
            if (e.type == SDL_EVENT_MOUSE_MOTION) {
                if (map && looking) {
                    strategic.yaw -= e.motion.xrel * 0.004;
                    strategic.pitch = std::clamp(strategic.pitch + e.motion.yrel * 0.004, 0.2, 1.5);
                } else if (map && dragging) {
                    // Grab the ground: a pixel is about distance / window height.
                    const double perPixel = strategic.distance * 1.15 / double(lab.targets.height());
                    const double c = std::cos(strategic.yaw), sn = std::sin(strategic.yaw);
                    strategic.x -= (e.motion.xrel * sn - e.motion.yrel * c) * perPixel;
                    strategic.y -= (-e.motion.xrel * c - e.motion.yrel * sn) * perPixel;
                } else if (looking) {
                    lab.camera.yaw -= e.motion.xrel * 0.003;
                    lab.camera.pitch = std::clamp(lab.camera.pitch - e.motion.yrel * 0.003, -1.5, 1.5);
                }
            }
            if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                if (map)
                    strategic.distance = std::clamp(strategic.distance * std::pow(0.8, e.wheel.y), 30.0, p.worldSize * 1.2);
                else
                    speed = std::clamp(speed * std::pow(1.5, e.wheel.y), 1.0, 50000.0);
            }
        }
        const double dt = std::min(0.1, std::chrono::duration<double>(Clock::now() - last).count());
        last = Clock::now();
        const bool* keys = SDL_GetKeyboardState(nullptr);
        if (map) {
            // Pan at about a screen a second, whatever the zoom.
            const double v = strategic.distance * dt * (keys[SDL_SCANCODE_LSHIFT] ? 4.0 : 1.0);
            const double c = std::cos(strategic.yaw), sn = std::sin(strategic.yaw);
            if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) { strategic.x += c * v; strategic.y += sn * v; }
            if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]) { strategic.x -= c * v; strategic.y -= sn * v; }
            if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) { strategic.x += sn * v; strategic.y -= c * v; }
            if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) { strategic.x -= sn * v; strategic.y += c * v; }
            if (keys[SDL_SCANCODE_Q]) strategic.yaw += 1.5 * dt;
            if (keys[SDL_SCANCODE_E]) strategic.yaw -= 1.5 * dt;
            strategic.x = std::clamp(strategic.x, 0.0, p.worldSize);
            strategic.y = std::clamp(strategic.y, 0.0, p.worldSize);
            strategic.apply(lab.camera, std::max(0.0, macroHeight(p, lab.macro, strategic.x, strategic.y)));
            // Never below the ground under the eye (detail the macro map does not know).
            lab.camera.eye.z = std::max(lab.camera.eye.z, lab.ground + 5.0);
            s = lab.frame(window, -1.0);
        } else {
            double f[3];
            lab.camera.forward(f);
            const double fh = std::hypot(f[0], f[1]);
            const double fx = f[0] / fh, fy = f[1] / fh;
            const double v = speed * dt * (keys[SDL_SCANCODE_LSHIFT] ? 10.0 : 1.0);
            double mx = 0, my = 0;
            if (keys[SDL_SCANCODE_W]) { mx += fx; my += fy; }
            if (keys[SDL_SCANCODE_S]) { mx -= fx; my -= fy; }
            if (keys[SDL_SCANCODE_D]) { mx += fy; my -= fx; }
            if (keys[SDL_SCANCODE_A]) { mx -= fy; my += fx; }
            lab.camera.eye.x = std::clamp(lab.camera.eye.x + mx * v, 0.0, p.worldSize);
            lab.camera.eye.y = std::clamp(lab.camera.eye.y + my * v, 0.0, p.worldSize);
            if (keys[SDL_SCANCODE_E]) agl += v;
            if (keys[SDL_SCANCODE_Q]) agl = std::max(1.0, agl - v);
            s = lab.frame(window, agl);
        }
        window_ms.push_back(s.frameMs);
        if (msSince(titleAt) > 500) {
            double mean = 0;
            for (double x : window_ms) mean += x;
            mean /= double(window_ms.size());
            char where[160];
            if (map)
                std::snprintf(where, sizeof where, "MAP x %.1f y %.1f km, zoom %.1f km (M: walk)",
                              strategic.x / 1000, strategic.y / 1000, strategic.distance / 1000);
            else
                std::snprintf(where, sizeof where, "x %.0f y %.0f agl %.0f, speed %.0f m/s (M: map)", s.x, s.y, agl, speed);
            // Which modules are on: G ground materials, F foliage and its layers (1..9).
            std::string modules = std::string(lab.options.groundMaterials ? "G" : "g") + (lab.options.surfaceMaps ? "S" : "s") +
                                  (lab.options.grass ? " Gr" : " gr") + (lab.options.light.shadows ? " Sh" : " sh") +
                                  (lab.options.light.clouds ? " Cl" : " cl") + (lab.options.light.windOn ? " W" : " w") +
                                  (lab.options.foliage ? " T:" : " t:");
            if (lab.foliageReady)
                for (std::size_t i = 0; i < lab.foliage.layers().size(); ++i)
                    modules += lab.foliage.layers()[i].enabled ? char('1' + i) : '-';
            modules += " | " + lab.dlss.status();
            char title[640];
            std::snprintf(title, sizeof title,
                          "terrain_lab | %.0f fps %.2f ms | %02d:%02d | grass %llu | trees %llu %.2fM tris | [%s] style %s | %s %s",
                          1000.0 / mean, mean, int(lab.options.light.timeOfDay),
                          int(std::fmod(lab.options.light.timeOfDay, 1.0) * 60.0), (unsigned long long)s.grassTufts,
                          (unsigned long long)s.foliageInstances,
                          double(s.foliageTriangles) / 1e6, modules.c_str(), Post::name(lab.options.post), where,
                          s.misses ? "| COVERAGE MISS" : "");
            SDL_SetWindowTitle(window, title);
            window_ms.clear();
            titleAt = Clock::now();
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Options options;
    if (!parse(argc, argv, options)) return 2;
    SDL_Window* window = nullptr;
    // The GPU device wants the video subsystem even with no window to draw in.
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    if (!options.headless) {
        window = SDL_CreateWindow("terrain_lab", int(options.width), int(options.height), 0);
        if (!window) { std::fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    }
    int code = 0;
    {
        Lab lab(options);
        if (!lab.open(window)) return 1;
        if (lab.eroded) return 0;
        if (!options.shot.empty()) code = runShot(lab, window);
        else if (!options.bench.empty()) code = runBench(lab, window);
        else if (window) code = runInteractive(lab, window);
        else std::fprintf(stderr, "headless needs --bench or --shot\n");
        SDL_WaitForGPUIdle(lab.device.handle());
    }
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return code;
}
