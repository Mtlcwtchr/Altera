// terrain_lab - the terrain core alone, on an empty scene, with numbers.
//
// Nothing of the game is linked: a window (or none), the device, the macro
// map, the clipmap and the patches. Two ways to run it:
//
//   interactive   terrain_lab [params] [--map]    fly with WASD/QE, RMB look,
//                                                 wheel = speed, F1 = debug views,
//                                                 M = strategic map view (WASD/drag pan,
//                                                 wheel zoom, RMB/QE rotate)
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

#include "engine/render/device.hpp"
#include "engine/render/targets.hpp"
#include "terrain_core/biomes.hpp"
#include "terrain_core/drainage.hpp"
#include "terrain_core/erosion.hpp"
#include "engine/world_source/png_io.hpp"
#include "terrain_core/lod_select.hpp"
#include "terrain_core/macro_map.hpp"
#include "terrain_core/params.hpp"
#include "terrain_core/renderer.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
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
    DrainageSettings drainage;
    bool drainGenerated = true;   // run the generator's map through the drainage erosion
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
        else if (a == "--raw-generator") o.drainGenerated = false;
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
    double x = 0, y = 0, z = 0, ground = 0;
};

struct Lab {
    Options& options;
    engine::Device device;
    engine::Targets targets;
    MacroMap macro;
    BiomeTable table;
    BiomeMap biomes;
    Renderer renderer;
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
        const bool ok = window ? device.open(window, assets)
                               : device.openHeadless(options.width, options.height, assets);
        if (!ok) { std::fprintf(stderr, "device: %s\n", device.error().c_str()); return false; }
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
        if (!renderer.open(device, options.params, macro, table, biomes)) { std::fprintf(stderr, "renderer: %s\n", device.error().c_str()); return false; }
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
        const double aspect = double(targets.width()) / double(targets.height());
        ViewUniforms view{};
        camera.viewProjection(view.viewProjection, aspect);
        const auto selectStart = Clock::now();
        const auto rects = clipmap.follow(camera.eye);
        const Selection selection = select(p, macro, clipmap, camera.eye, frustumFrom(view.viewProjection));
        s.selectMs = msSince(selectStart);

        const auto recordStart = Clock::now();
        auto* commands = SDL_AcquireGPUCommandBuffer(device.handle());
        renderer.upload(commands, selection);
        s.texels = renderer.generate(commands, rects);
        SDL_GPUTexture* swapchain = nullptr;
        Uint32 sw = 0, sh = 0;
        if (window && !SDL_WaitAndAcquireGPUSwapchainTexture(commands, window, &swapchain, &sw, &sh)) swapchain = nullptr;

        SDL_GPUColorTargetInfo colour{};
        colour.texture = targets.colour();
        colour.load_op = SDL_GPU_LOADOP_CLEAR;
        colour.store_op = SDL_GPU_STOREOP_STORE;
        colour.clear_color = {0.62f, 0.72f, 0.82f, 1};
        SDL_GPUDepthStencilTargetInfo depth{};
        depth.texture = targets.depth();
        depth.clear_depth = 0;
        depth.load_op = SDL_GPU_LOADOP_CLEAR;
        depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
        depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        auto* pass = SDL_BeginGPURenderPass(commands, &colour, 1, &depth);
        view.eyeZ = float(camera.eye.z);
        view.spacing = float(p.spacing);
        view.clipSize = p.clipSize;
        view.grid = p.grid;
        view.levels = p.levels;
        view.rangeFactor = float(p.rangeFactor);
        view.morphStart = float(p.morphStart);
        LookUniforms look{};
        const double sun[3] = {-0.45, -0.35, 0.6};
        const double sl = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
        for (int i = 0; i < 3; ++i) look.sunDirection[i] = float(sun[i] / sl);
        // Haze by height above the ground, not by the levels there are: from the
        // ground ~14 km as before, from a map view hundreds of kilometres.
        look.hazeDistance = float(14350.0 + 6.0 * std::max(0.0, camera.eye.z - ground));
        look.skyColour[0] = 0.62f; look.skyColour[1] = 0.72f; look.skyColour[2] = 0.82f;
        look.debugLevels = std::uint32_t(options.debugLevels);
        look.clipSize = p.clipSize;
        look.levels = p.levels;
        renderer.draw(commands, pass, view, look);
        SDL_EndGPURenderPass(pass);

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

    std::printf("%-9s %7s %7s %7s %7s %7s | %6s %6s | %7s %9s %9s %6s\n", "scenario", "mean", "p50", "p95", "p99", "max",
                "selCPU", "recCPU", "patches", "tris", "texels/f", "misses");
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
        out << "frame,frame_ms,select_ms,record_ms,wait_ms,patches,triangles,rects,texels,misses,nodes,x,y,z,ground\n";
        std::vector<double> ms, sel, rec;
        double texelSum = 0, texelMax = 0, patchMax = 0, triMax = 0, misses = 0;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const FrameStats& s = frames[i];
            std::snprintf(buf, sizeof buf, "%zu,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%llu,%u,%u,%.2f,%.2f,%.2f,%.2f\n", i, s.frameMs,
                          s.selectMs, s.recordMs, s.waitMs, s.patches, s.triangles, s.rects,
                          (unsigned long long)s.texels, s.misses, s.nodes, s.x, s.y, s.z, s.ground);
            out << buf;
            ms.push_back(s.frameMs);
            sel.push_back(s.selectMs);
            rec.push_back(s.recordMs);
            texelSum += double(s.texels);
            texelMax = std::max(texelMax, double(s.texels));
            patchMax = std::max(patchMax, double(s.patches));
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
        std::printf("%-9s %7.2f %7.2f %7.2f %7.2f %7.2f | %6.2f %6.2f | %7.0f %9.0f %9.0f %6.0f   cold %.1f ms\n", sc.name,
                    mean, percentile(ms, 0.5), percentile(ms, 0.95), percentile(ms, 0.99), percentile(ms, 1.0), selMean,
                    recMean, patchMax, triMax, texelSum / double(frames.size()), misses, coldMs);
        std::snprintf(buf, sizeof buf,
                      "%s    {\"name\": \"%s\", \"frames\": %zu, \"meanMs\": %.3f, \"p50Ms\": %.3f, \"p95Ms\": %.3f, "
                      "\"p99Ms\": %.3f, \"maxMs\": %.3f, \"selectMs\": %.3f, \"recordMs\": %.3f, \"coldMs\": %.3f, "
                      "\"patchesMax\": %.0f, \"trianglesMax\": %.0f, \"texelsPerFrame\": %.0f, \"texelsMax\": %.0f, "
                      "\"coverageMisses\": %.0f}",
                      first ? "" : ",\n", sc.name, frames.size(), mean, percentile(ms, 0.5), percentile(ms, 0.95),
                      percentile(ms, 0.99), percentile(ms, 1.0), selMean, recMean, coldMs, patchMax, triMax,
                      texelSum / double(frames.size()), texelMax, misses);
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
            char title[512];
            std::snprintf(title, sizeof title,
                          "terrain_lab | %.0f fps %.2f ms (sel %.2f rec %.2f) | patches %u tris %.2fM | gen %llu tx | %s %s",
                          1000.0 / mean, mean, s.selectMs, s.recordMs, s.patches, s.triangles / 1e6,
                          (unsigned long long)s.texels, where, s.misses ? "| COVERAGE MISS" : "");
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
