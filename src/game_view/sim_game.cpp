// The life-sim in the world: altera_sim steps on a thread of its own, the
// world view draws its people, beasts and buildings as placeholder shapes on
// the real ground, and an immediate-mode HUD (engine ui, style G of the design
// canvas "Altera UI и HUD") shows the selected character, the clock, the
// chronicle and the player's orders.
//
// Every text on screen comes from content/loc (game.* keys in game_ui.json and
// the sim's own names), so this file holds no display strings.

#include "game_view/game_layer.hpp"
#include "game_view/skinned.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "engine/render/device.hpp"
#include "engine/world_source/png_io.hpp"
#include "engine/ui/canvas.hpp"
#include "engine/ui/font.hpp"
#include "engine/ui/ui.hpp"
#include "sim/ai.hpp"
#include "sim/loc.hpp"
#include "sim/worldgen.hpp"
#include "terrain_core/ground_height.hpp"
#include "terrain_core/macro_map.hpp"
#include "terrain_core/params.hpp"

namespace altera::game {
namespace {

namespace fs = std::filesystem;
using namespace altera::sim;
using Clock = std::chrono::steady_clock;
constexpr double kPi = 3.14159265358979;

// ---- what the sim thread hands the frame --------------------------------
struct ActorSnap {
    EntityId id = kNone;
    std::string name, species, family, state, act, goal, interaction;
    bool animal = false, alive = true, player = false, down = false, fighting = false;
    char sex = 'f';
    int age = 0, group = 1, wounds = 0;
    double px = 0, py = 0, x = 0, y = 0;   // sim metres, last tick and this one
    double hp = 1;
};

struct BuildingSnap {
    std::string type;
    double x = 0, y = 0;
    EntityId family = kNone;
};

struct Line {
    int day = 0;
    std::string time, text, kind;
    std::vector<EntityId> who;
};

struct Detail {
    EntityId id = kNone;
    std::vector<std::pair<std::string, double>> needs;
    double mood = 0, fatigue = 0, blood = 100;
    bool freeWill = true;
    std::vector<Line> trace;
    std::vector<std::string> inventory;
};

struct Snapshot {
    Tick tick = 0;
    Clock::time_point tickAt = Clock::now();
    double tickSeconds = 1.0;   // real seconds one tick takes at this speed
    std::string season;
    double tempC = 0, rain = 0;
    std::vector<ActorSnap> actors;
    std::vector<Line> chronicle;   // newest last, a bounded tail
    Detail detail;
    bool paused = false;
    int speed = 1;
    std::string result;
    std::string settlement;
};

std::string clockText(Tick t) {
    const int m = int(t % kTicksPerDay) * 10;
    char b[16];
    std::snprintf(b, sizeof b, "%02d:%02d", m / 60, m % 60);
    return b;
}

// Ticks per real second at each speed: 1 is a game minute a real second, as a
// Sims-like life sim plays; 4 is "as fast as the machine goes".
constexpr double kSpeeds[] = {0.0, 0.1, 0.6, 3.0, 40.0};

// ---- meshes ---------------------------------------------------------------
struct Vertex {
    float p[3], n[3], shade;
};
struct Instance {
    float placeYaw[4], scale[4], colour[4], colour2[4];
};
static_assert(sizeof(Instance) == 64);

struct MeshBuilder {
    std::vector<Vertex> v;
    void tri(const float a[3], const float b[3], const float c[3], float shade) {
        const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float n[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
        const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (float& x : n) x /= (l > 0 ? l : 1);
        for (const float* q : {a, b, c}) v.push_back({{q[0], q[1], q[2]}, {n[0], n[1], n[2]}, shade});
    }
    void quad(const float a[3], const float b[3], const float c[3], const float d[3], float shade) {
        tri(a, b, c, shade);
        tri(a, c, d, shade);
    }
    // A prism of `sides` from radius r0 at z0 to r1 at z1, capped.
    void frustum(int sides, float r0, float z0, float r1, float z1, float shade, float sx = 1, float sy = 1) {
        for (int i = 0; i < sides; ++i) {
            const float a0 = float(2 * kPi * i / sides), a1 = float(2 * kPi * (i + 1) / sides);
            const float b0[3] = {r0 * std::cos(a0) * sx, r0 * std::sin(a0) * sy, z0};
            const float b1[3] = {r0 * std::cos(a1) * sx, r0 * std::sin(a1) * sy, z0};
            const float t0[3] = {r1 * std::cos(a0) * sx, r1 * std::sin(a0) * sy, z1};
            const float t1[3] = {r1 * std::cos(a1) * sx, r1 * std::sin(a1) * sy, z1};
            quad(b0, b1, t1, t0, shade);
            const float top[3] = {0, 0, z1}, bottom[3] = {0, 0, z0};
            tri(t0, t1, top, shade);
            tri(b1, b0, bottom, shade);
        }
    }
    void box(float x0, float y0, float z0, float x1, float y1, float z1, float shade) {
        const float p[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0},
                               {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
        quad(p[0], p[3], p[2], p[1], shade);   // bottom
        quad(p[4], p[5], p[6], p[7], shade);   // top
        quad(p[0], p[1], p[5], p[4], shade);
        quad(p[1], p[2], p[6], p[5], shade);
        quad(p[2], p[3], p[7], p[6], shade);
        quad(p[3], p[0], p[4], p[7], shade);
    }
};

enum Mesh { kPerson, kBeast, kHouse, kBlock, kMeshes };
struct MeshRange {
    std::uint32_t first = 0, count = 0;
};

// ---- the game ---------------------------------------------------------------
class SimGame final : public GameLayer {
public:
    SimGame(engine::Device& device, SDL_Window* window, const terrain_core::MacroMap& macro,
            const terrain_core::Params& params)
        : device_(device), window_(window), ground_(macro, params), worldSize_(params.worldSize) {}

    ~SimGame() override {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        ui_.shutdown();
        if (uiTransfer_) SDL_ReleaseGPUTransferBuffer(device_.handle(), uiTransfer_);
        if (instTransfer_) SDL_ReleaseGPUTransferBuffer(device_.handle(), instTransfer_);
        if (shotTransfer_) SDL_ReleaseGPUTransferBuffer(device_.handle(), shotTransfer_);
    }

    bool open(double centreX, double centreY, const GameOptions& o, std::string* why) {
        opts_ = o;
        try {
            content_ = std::make_unique<Content>(Content::load(o.root + "/content/ai"));
            loc_ = std::make_unique<Loc>(Loc::load(o.root + "/content/loc"));
            Json cfg = loadJsonFile(o.root + "/content/sim/test_world.json");
            if (!o.season.empty()) {
                cfg.erase("start_day");
                cfg["season"] = o.season;
            }
            world_ = std::make_unique<World>(makeTestWorld(*content_, cfg, o.seed));
            installAi(*world_);
            // The world starts at midnight; the player arrives with the morning.
            world_->run(7 * kTicksPerDay / 24);
            for (const auto& [id, c] : world_->characters()) prevPos_[id] = c.pos;
        } catch (const std::exception& e) {
            if (why) *why = std::string("sim: ") + e.what();
            return false;
        }
        const MapGrid& m = world_->map();
        originX_ = std::clamp(centreX - m.sizeM * 0.5, 0.0, std::max(0.0, worldSize_ - m.sizeM));
        originY_ = std::clamp(centreY - m.sizeM * 0.5, 0.0, std::max(0.0, worldSize_ - m.sizeM));
        if (!world_->settlements().empty()) {
            const Settlement& s = world_->settlements().begin()->second;
            target_[0] = originX_ + s.center.x;
            target_[1] = originY_ + s.center.y;
        }
        for (const auto& [id, ob] : world_->objects()) {
            static const char* kShown[] = {"house", "storage", "hearth", "field", "garden", "shrine", "village_fire"};
            for (const char* t : kShown)
                if (ob.type == t) buildings_.push_back({ob.type, ob.pos.x, ob.pos.y, ob.ownerFamily});
        }
        for (const auto& g : content_->goals) {
            goalIds_.push_back(g.id);
            goalNames_.push_back(name("goal", g.id));
        }
        snapshotLocked();
        shown_ = snap_;
        for (const auto& a : shown_.actors)
            if (!a.animal && a.alive) {
                select(a.id);
                double wx, wy;
                actorWorld(a, 1.0, wx, wy);
                target_[0] = wx;
                target_[1] = wy;
                break;
            }
        if (!openGpu(why)) return false;
        loadScript();
        thread_ = std::thread([this] { simLoop(); });
        return true;
    }

    // ---- input ---------------------------------------------------------
    bool event(const SDL_Event& e) override {
        switch (e.type) {
        case SDL_EVENT_QUIT: return false;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_ESCAPE) return false;
            if (e.key.repeat) break;
            if (e.key.key == SDLK_SPACE) post({"pause"});
            if (e.key.key >= SDLK_1 && e.key.key <= SDLK_4) post({"speed", int(e.key.key - SDLK_1) + 1});
            if (e.key.key == SDLK_TAB) follow_ = !follow_;
            if (e.key.key == SDLK_H) showChronicle_ = !showChronicle_;
            if (e.key.key == SDLK_T) showThoughts_ = !showThoughts_;
            if (e.key.key == SDLK_F) follow_ = true;
            if (e.key.key == SDLK_F9) {
                char n[64];
                std::snprintf(n, sizeof n, "game_%03d.png", ++shotCounter_);
                shotWanted_ = (fs::path(opts_.root) / "terrain_lab_out/shots_for_user" / n).string();
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            input_.mouseX = e.motion.x;
            input_.mouseY = e.motion.y;
            if (rmb_) {
                orbitYaw_ -= e.motion.xrel * 0.005;
                orbitPitch_ = std::clamp(orbitPitch_ + e.motion.yrel * 0.004, 0.08, 1.45);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (e.button.button == SDL_BUTTON_LEFT) {
                input_.down = input_.pressed = true;
                input_.doubleClick = e.button.clicks >= 2;
                if (!overUi_) pickAt_ = {e.button.x, e.button.y, true};
            }
            if (e.button.button == SDL_BUTTON_RIGHT) {
                input_.rightPressed = input_.rightDown = true;
                if (!overUi_) rmb_ = true;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button == SDL_BUTTON_LEFT) {
                input_.down = false;
                input_.released = true;
            }
            if (e.button.button == SDL_BUTTON_RIGHT) {
                input_.rightDown = false;
                rmb_ = false;
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            input_.wheel += e.wheel.y;
            if (!overUi_) zoom_ = std::clamp(zoom_ * std::pow(0.85, e.wheel.y), 4.0, 6000.0);
            break;
        default: break;
        }
        return true;
    }

    // ---- camera and clock ---------------------------------------------------
    void update(double dt, CameraRig& cam, double& timeOfDay) override {
        finishShot();
        runScript(dt);
        {
            std::lock_guard<std::mutex> lock(snapMutex_);
            shown_ = snap_;
        }
        const double alpha = tickAlpha();
        // The hour of the day from the sim's clock, moving smoothly between ticks.
        const double ticks = double(shown_.tick % kTicksPerDay) + alpha;
        timeOfDay = ticks / double(kTicksPerDay) * 24.0;

        if (pickAt_.pending) {
            pickAt_.pending = false;
            const EntityId hit = pick(pickAt_.x, pickAt_.y);
            if (hit != kNone) select(hit);
        }
        const bool* keys = SDL_GetKeyboardState(nullptr);
        const ActorSnap* sel = actor(selected_);
        if (follow_ && sel) {
            double wx, wy;
            actorWorld(*sel, alpha, wx, wy);
            target_[0] += (wx - target_[0]) * std::min(1.0, dt * 6.0);
            target_[1] += (wy - target_[1]) * std::min(1.0, dt * 6.0);
        } else {
            const double v = zoom_ * dt * (keys[SDL_SCANCODE_LSHIFT] ? 3.0 : 1.0);
            const double c = std::cos(orbitYaw_), s = std::sin(orbitYaw_);
            if (keys[SDL_SCANCODE_W]) { target_[0] += c * v; target_[1] += s * v; follow_ = false; }
            if (keys[SDL_SCANCODE_S]) { target_[0] -= c * v; target_[1] -= s * v; follow_ = false; }
            if (keys[SDL_SCANCODE_D]) { target_[0] += s * v; target_[1] -= c * v; follow_ = false; }
            if (keys[SDL_SCANCODE_A]) { target_[0] -= s * v; target_[1] += c * v; follow_ = false; }
        }
        if (keys[SDL_SCANCODE_Q]) orbitYaw_ += 1.5 * dt;
        if (keys[SDL_SCANCODE_E]) orbitYaw_ -= 1.5 * dt;
        const double tz = groundAt(target_[0], target_[1]) + 1.2;
        cam.yaw = orbitYaw_;
        cam.pitch = -orbitPitch_;
        const double f[3] = {std::cos(cam.pitch) * std::cos(cam.yaw), std::cos(cam.pitch) * std::sin(cam.yaw),
                             std::sin(cam.pitch)};
        cam.eye[0] = target_[0] - f[0] * zoom_;
        cam.eye[1] = target_[1] - f[1] * zoom_;
        cam.eye[2] = std::max(tz - f[2] * zoom_, groundAt(cam.eye[0], cam.eye[1]) + 1.5);
    }

    // ---- drawing ------------------------------------------------------------
    void prepare(SDL_GPUCommandBuffer* commands, const double eye[3], const float vp[16], std::uint32_t width,
                 std::uint32_t height) override {
        std::memcpy(vp_, vp, sizeof vp_);
        std::memcpy(eye_, eye, sizeof eye_);
        viewW_ = width;
        viewH_ = height;
        buildInstances();
        people_.prepare(commands, figures_);
        if (instances_.empty() || !instBuffer_) return;
        const std::uint32_t bytes = std::uint32_t(instances_.size() * sizeof(Instance));
        void* map = SDL_MapGPUTransferBuffer(device_.handle(), instTransfer_, true);
        if (!map) return;
        std::memcpy(map, instances_.data(), bytes);
        SDL_UnmapGPUTransferBuffer(device_.handle(), instTransfer_);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        const SDL_GPUTransferBufferLocation from{instTransfer_, 0};
        const SDL_GPUBufferRegion to{instBuffer_.get(), 0, bytes};
        SDL_UploadToGPUBuffer(copy, &from, &to, true);
        SDL_EndGPUCopyPass(copy);
    }

    void drawOpaque(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass,
                    const terrain_core::LightUniforms& light) override {
        people_.draw(commands, pass, vp_, light);
        if (instances_.empty() || !actorsPipe_) return;
        SDL_BindGPUGraphicsPipeline(pass, actorsPipe_.get());
        const SDL_GPUBufferBinding bufs[2] = {{vertBuffer_.get(), 0}, {instBuffer_.get(), 0}};
        SDL_BindGPUVertexBuffers(pass, 0, bufs, 2);
        SDL_PushGPUVertexUniformData(commands, 0, vp_, sizeof vp_);
        SDL_PushGPUFragmentUniformData(commands, 0, &light, sizeof light);
        for (int m = 0; m < kMeshes; ++m)
            if (batch_[m].count)
                SDL_DrawGPUPrimitives(pass, meshes_[m].count, batch_[m].count, meshes_[m].first, batch_[m].first);
    }

    void drawOverlay(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, std::uint32_t width,
                     std::uint32_t height) override {
        if (!overlayPipe_) return;
        if (canvas_.wouldResize(int(width), int(height))) {
            ui_.shutdown();
            canvas_.size(int(width), int(height));
            ui_.init(canvas_.renderer());
            if (font_) ui_.setFont(font_, fontBold_, 15, 1.0f);
            styleTheme();
            uiTexture_ = {};
        }
        if (!canvas_) return;
        SDL_Renderer* sw = canvas_.renderer();
        SDL_SetRenderDrawBlendMode(sw, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(sw, 0, 0, 0, 0);
        SDL_RenderClear(sw);
        SDL_SetRenderDrawBlendMode(sw, SDL_BLENDMODE_BLEND);
        ui_.begin(input_, int(width), int(height));
        buildHud(float(width), float(height));
        ui_.end();
        overUi_ = input_.overUi;
        SDL_RenderPresent(sw);
        input_.pressed = input_.released = input_.rightPressed = input_.doubleClick = false;
        input_.wheel = 0;

        if (!uiTexture_) {
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = width;
            info.height = height;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            uiTexture_ = device_.makeTexture(info);
            if (uiTransfer_) SDL_ReleaseGPUTransferBuffer(device_.handle(), uiTransfer_);
            SDL_GPUTransferBufferCreateInfo t{};
            t.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            t.size = width * height * 4;
            uiTransfer_ = SDL_CreateGPUTransferBuffer(device_.handle(), &t);
            if (!uiTexture_ || !uiTransfer_) return;
        }
        const SDL_Surface* surf = canvas_.surface();
        auto* map = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(device_.handle(), uiTransfer_, true));
        if (!map) return;
        for (std::uint32_t y = 0; y < height; ++y)
            std::memcpy(map + std::size_t(y) * width * 4, static_cast<const std::uint8_t*>(surf->pixels) + std::size_t(y) * surf->pitch,
                        std::size_t(width) * 4);
        SDL_UnmapGPUTransferBuffer(device_.handle(), uiTransfer_);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = uiTransfer_;
        src.pixels_per_row = width;
        src.rows_per_layer = height;
        SDL_GPUTextureRegion dst{};
        dst.texture = uiTexture_.get();
        dst.w = width;
        dst.h = height;
        dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, true);
        SDL_EndGPUCopyPass(copy);

        SDL_GPUColorTargetInfo colour{};
        colour.texture = target;
        colour.load_op = SDL_GPU_LOADOP_LOAD;
        colour.store_op = SDL_GPU_STOREOP_STORE;
        auto* pass = SDL_BeginGPURenderPass(commands, &colour, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, overlayPipe_.get());
        const SDL_GPUTextureSamplerBinding pic{uiTexture_.get(), sampler_.get()};
        SDL_BindGPUFragmentSamplers(pass, 0, &pic, 1);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
        if (!shotWanted_.empty()) captureShot(commands, target, width, height);
    }

private:
    // ---- pictures of the game window (F9, and the scripted run) -------------
    // ALTERA_GAME_SCRIPT names a JSON list of steps, each at `at` seconds after
    // the world opened: camera (zoom, pitch, yawDeg), `select` (a person by
    // index among the living people), `speed`, `panel` (none|sheet|wheel|kin|annals),
    // `shot` (file under terrain_lab_out/shots_for_user), `quit`.
    void loadScript() {
        const char* path = std::getenv("ALTERA_GAME_SCRIPT");
        if (!path || !*path) return;
        try {
            script_ = loadJsonFile(path);
        } catch (const std::exception& e) {
            SDL_Log("altera_game: script %s: %s", path, e.what());
        }
    }
    void runScript(double dt) {
        if (!script_.is_array()) return;
        scriptClock_ += dt;
        while (scriptStep_ < script_.size() && scriptClock_ >= script_[scriptStep_].value("at", 0.0)) {
            const Json& st = script_[scriptStep_++];
            if (st.contains("zoom")) zoom_ = st["zoom"].get<double>();
            if (st.contains("pitch")) orbitPitch_ = st["pitch"].get<double>();
            if (st.contains("yawDeg")) orbitYaw_ = st["yawDeg"].get<double>() * kPi / 180.0;
            if (st.contains("speed")) post({"speed", st["speed"].get<int>()});
            if (st.contains("select")) {
                int k = st["select"].get<int>();
                for (const auto& a : shown_.actors)
                    if (!a.animal && a.alive && k-- == 0) {
                        select(a.id);
                        break;
                    }
            }
            if (st.contains("panel")) panel_ = st["panel"].get<std::string>();
            if (st.contains("shot"))
                shotWanted_ = (fs::path(opts_.root) / "terrain_lab_out/shots_for_user" / st["shot"].get<std::string>()).string();
            if (st.value("quit", false)) {
                SDL_Event q{};
                q.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&q);
            }
        }
    }
    void captureShot(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, std::uint32_t w, std::uint32_t h) {
        if (shotTransfer_) SDL_ReleaseGPUTransferBuffer(device_.handle(), shotTransfer_);
        SDL_GPUTransferBufferCreateInfo t{};
        t.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        t.size = w * h * 4;
        shotTransfer_ = SDL_CreateGPUTransferBuffer(device_.handle(), &t);
        if (!shotTransfer_) {
            shotWanted_.clear();
            return;
        }
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        SDL_GPUTextureRegion src{};
        src.texture = target;
        src.w = w;
        src.h = h;
        src.d = 1;
        SDL_GPUTextureTransferInfo dst{};
        dst.transfer_buffer = shotTransfer_;
        dst.pixels_per_row = w;
        dst.rows_per_layer = h;
        SDL_DownloadFromGPUTexture(copy, &src, &dst);
        SDL_EndGPUCopyPass(copy);
        shotPending_ = shotWanted_;
        shotW_ = w;
        shotH_ = h;
        shotWanted_.clear();
    }
    void finishShot() {
        if (shotPending_.empty() || !shotTransfer_) return;
        SDL_WaitForGPUIdle(device_.handle());
        const auto* px = static_cast<const std::uint8_t*>(SDL_MapGPUTransferBuffer(device_.handle(), shotTransfer_, false));
        if (px) {
            engine::world_source::Image img;
            img.width = shotW_;
            img.height = shotH_;
            img.channels = 3;
            img.allocate();
            // The view hands its own frame target (engine::Device::kColourFormat, RGBA).
            const bool bgra = engine::Device::kColourFormat == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
            for (std::size_t i = 0, n = std::size_t(shotW_) * shotH_; i < n; ++i) {
                img.bytes[i * 3 + 0] = px[i * 4 + (bgra ? 2 : 0)];
                img.bytes[i * 3 + 1] = px[i * 4 + 1];
                img.bytes[i * 3 + 2] = px[i * 4 + (bgra ? 0 : 2)];
            }
            SDL_UnmapGPUTransferBuffer(device_.handle(), shotTransfer_);
            std::error_code ec;
            fs::create_directories(fs::path(shotPending_).parent_path(), ec);
            std::string why;
            if (!engine::world_source::writePng(shotPending_, img, &why)) SDL_Log("altera_game: shot: %s", why.c_str());
            else SDL_Log("altera_game: saved %s", shotPending_.c_str());
        }
        shotPending_.clear();
    }

    struct Command {
        std::string op;
        int n = 0;
        std::string type;
        Json args = Json::object();
    };

    // ---- the sim thread -------------------------------------------------
    void post(Command c) {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        commands_.push_back(std::move(c));
    }

    void simLoop() {
        auto last = Clock::now();
        double owed = 0;
        auto lastSnap = Clock::now();
        while (!stop_) {
            bool changed = false;
            std::deque<Command> todo;
            {
                std::lock_guard<std::mutex> lock(cmdMutex_);
                todo.swap(commands_);
            }
            for (auto& c : todo) {
                changed = true;
                if (c.op == "pause") paused_ = !paused_;
                else if (c.op == "speed") {
                    speed_ = std::clamp(c.n, 1, 4);
                    paused_ = false;
                } else if (c.op == "cmd") {
                    altera::sim::Command cmd;
                    cmd.source = "player:1";
                    cmd.type = c.type;
                    cmd.args = c.args;
                    cmd.args["player"] = 1;
                    const auto r = world_->applyNow(cmd);
                    result_ = r.result;
                }
            }
            const auto now = Clock::now();
            const double dt = std::chrono::duration<double>(now - last).count();
            last = now;
            const double tps = kSpeeds[speed_];
            if (!paused_) owed = std::min(owed + dt * tps, 4.0);
            else owed = 0;
            int steps = 0;
            while (owed >= 1.0 && steps < 8) {
                for (const auto& [id, c] : world_->characters()) prevPos_[id] = c.pos;
                world_->step();
                owed -= 1.0;
                ++steps;
                changed = true;
                tickAt_ = Clock::now();
            }
            if (changed || selectedWanted_ != snapSelected_ ||
                std::chrono::duration<double>(Clock::now() - lastSnap).count() > 0.25) {
                snapshotLocked();
                lastSnap = Clock::now();
            }
            if (!steps) std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }

    std::string name(const std::string& kind, const std::string& id) const {
        if (id.empty()) return "";
        const std::string k = kind + "." + id + ".name";
        return loc_->has(opts_.lang, k) ? loc_->text(opts_.lang, k) : id;
    }
    std::string text(const std::string& key, const Json& args = Json::object()) const {
        return loc_->has(opts_.lang, key) ? loc_->text(opts_.lang, key, args) : key;
    }

    // Built on the sim thread (or before it starts): nothing else touches the world.
    void snapshotLocked() {
        const World& w = *world_;
        Snapshot s;
        s.tick = w.now();
        s.tickAt = tickAt_;
        s.tickSeconds = kSpeeds[speed_] > 0 ? 1.0 / kSpeeds[speed_] : 1.0;
        s.season = w.season();
        s.paused = paused_;
        s.speed = speed_;
        s.result = result_;
        if (!w.settlements().empty()) {
            s.settlement = w.settlements().begin()->second.name;
            const DayWeather dw = weatherToday(w, w.settlements().begin()->first);
            s.tempC = dw.tempC;
            s.rain = dw.rainMm;
        }
        s.actors.reserve(w.characters().size());
        for (const auto& [id, c] : w.characters()) {
            ActorSnap a;
            a.id = id;
            a.animal = c.isAnimal();
            a.name = a.animal ? name("species", c.species) : c.name;
            a.species = c.species;
            a.group = c.group;
            const Family* f = w.familyOf(c);
            a.family = f ? f->name : "";
            a.alive = c.body.alive;
            a.player = c.controller.isPlayer();
            a.down = c.body.downUntil > w.now();
            a.sex = c.body.sex;
            a.age = c.body.ageDays / kDaysPerYear;
            a.hp = c.body.health;
            a.wounds = int(c.wounds.size());
            a.state = c.ai.fsm.state;
            a.fighting = a.state == "Fight";
            a.interaction = c.ai.fsm.interaction;
            if (!c.ai.fsm.interaction.empty() && c.ai.fsm.interaction != "goto" && c.ai.fsm.interaction != "wait")
                a.act = name("interaction", c.ai.fsm.interaction);
            else if (c.ai.plan.actionIndex + 1 < c.ai.plan.actions.size() &&
                     c.ai.plan.actions[c.ai.plan.actionIndex + 1].interaction != "goto")
                a.act = name("interaction", c.ai.plan.actions[c.ai.plan.actionIndex + 1].interaction);
            if (c.ai.goal.valid())
                a.goal = c.ai.goal.id == "player_action" ? name("interaction", c.ai.goal.task) : name("goal", c.ai.goal.id);
            const auto pp = prevPos_.find(id);
            a.x = c.pos.x;
            a.y = c.pos.y;
            a.px = pp != prevPos_.end() ? pp->second.x : c.pos.x;
            a.py = pp != prevPos_.end() ? pp->second.y : c.pos.y;
            s.actors.push_back(std::move(a));
        }
        const auto& log = w.chronicleLog();
        for (size_t i = log.size() > 40 ? log.size() - 40 : 0; i < log.size(); ++i) {
            const ChronicleEntry& e = log[i];
            Line l;
            l.day = int(e.tick / kTicksPerDay) + 1;
            l.time = clockText(e.tick);
            l.kind = e.type;
            l.who = e.who;
            l.text = loc_->has(opts_.lang, e.code) ? loc_->text(opts_.lang, e.code, locArgs(*loc_, opts_.lang, w, e.params))
                                                   : e.code;
            s.chronicle.push_back(std::move(l));
        }
        const EntityId sel = selectedWanted_;
        snapSelected_ = sel;
        if (const Character* c = w.character(sel)) {
            Detail& d = s.detail;
            d.id = sel;
            for (const auto& [k, v] : c->needs) d.needs.push_back({name("need", k), v});
            d.mood = c->mood(w.now());
            d.fatigue = c->body.fatigue;
            d.blood = c->body.blood;
            d.freeWill = c->controller.freeWill;
            for (const auto& [k, v] : c->inventory)
                if (v > 0) d.inventory.push_back(name("item", k) + " " + std::to_string(v));
            const size_t from = c->ai.trace.size() > 14 ? c->ai.trace.size() - 14 : 0;
            for (size_t i = from; i < c->ai.trace.size(); ++i) {
                const TraceEntry& e = c->ai.trace[i];
                d.trace.push_back({int(e.tick / kTicksPerDay) + 1, clockText(e.tick), traceText(*loc_, opts_.lang, w, *c, e), "", {}});
            }
        }
        std::lock_guard<std::mutex> lock(snapMutex_);
        snap_ = std::move(s);
    }

    // ---- placement --------------------------------------------------------
    double groundAt(double x, double y) const { return std::max(0.0, double(ground_.at(x, y, 2.0))); }

    double tickAlpha() const {
        if (shown_.paused) return 1.0;
        const double since = std::chrono::duration<double>(Clock::now() - shown_.tickAt).count();
        return std::clamp(since / std::max(1e-3, shown_.tickSeconds), 0.0, 1.0);
    }

    void actorWorld(const ActorSnap& a, double alpha, double& x, double& y) const {
        x = originX_ + a.px + (a.x - a.px) * alpha;
        y = originY_ + a.py + (a.y - a.py) * alpha;
    }

    const ActorSnap* actor(EntityId id) const {
        for (const auto& a : shown_.actors)
            if (a.id == id) return &a;
        return nullptr;
    }

    void select(EntityId id) {
        selected_ = id;
        selectedWanted_ = id;
        follow_ = true;
    }

    // Eye-relative position to screen pixels; false behind the eye.
    bool project(double rx, double ry, double rz, float& sx, float& sy) const {
        const double cx = vp_[0] * rx + vp_[1] * ry + vp_[2] * rz + vp_[3];
        const double cy = vp_[4] * rx + vp_[5] * ry + vp_[6] * rz + vp_[7];
        const double cw = vp_[12] * rx + vp_[13] * ry + vp_[14] * rz + vp_[15];
        if (cw <= 0.05) return false;
        sx = float((cx / cw * 0.5 + 0.5) * viewW_);
        sy = float((0.5 - cy / cw * 0.5) * viewH_);
        return true;
    }

    EntityId pick(float mx, float my) const {
        EntityId best = kNone;
        float bd = 28.0f;
        const double alpha = tickAlpha();
        for (const auto& a : shown_.actors) {
            if (a.species == "fish_shoal") continue;
            double wx, wy;
            actorWorld(a, alpha, wx, wy);
            const double wz = groundAt(wx, wy) + (a.animal ? 0.5 : 1.0);
            float sx, sy;
            if (!project(wx - eye_[0], wy - eye_[1], wz - eye_[2], sx, sy)) continue;
            const float d = std::hypot(sx - mx, sy - my);
            if (d < bd) {
                bd = d;
                best = a.id;
            }
        }
        return best;
    }

    static void hsv(double h, double s, double v, float out[3]) {
        const double c = v * s, x = c * (1 - std::fabs(std::fmod(h / 60.0, 2) - 1)), m = v - c;
        double r = 0, g = 0, b = 0;
        if (h < 60) r = c, g = x;
        else if (h < 120) r = x, g = c;
        else if (h < 180) g = c, b = x;
        else if (h < 240) g = x, b = c;
        else if (h < 300) r = x, b = c;
        else r = c, b = x;
        out[0] = float(r + m), out[1] = float(g + m), out[2] = float(b + m);
    }
    void familyColour(const std::string& f, float out[3]) const {
        const double h = double(hashStr(f) % 360);
        hsv(h, 0.55, 0.62, out);
    }

    // What a person plays now, from what the sim says they do.
    int personClip(const ActorSnap& a, double speed, double& rate) const {
        rate = 1.0;
        auto st = [&](const char* s) { return people_.stateClip(s); };
        if (!a.alive) return st("death");
        if (a.down) return st("down");
        if (speed > 0.25) {
            if (a.state == "Flee" || speed > 5.5) {
                rate = std::clamp(speed / 6.0, 0.8, 1.6);
                return st("sprint");
            }
            if (speed > 2.6) {
                rate = std::clamp(speed / 3.5, 0.8, 1.6);
                return st("run");
            }
            rate = std::clamp(speed / 1.3, 0.6, 1.8);
            return st("walk");
        }
        if (!a.interaction.empty() && a.interaction != "goto" && a.interaction != "wait") {
            const int c = people_.interactionClip(a.interaction);
            if (c >= 0) return c;
        }
        static const std::pair<const char*, const char*> kByState[] = {
            {"Sleep", "sleep"}, {"Rest", "sit"}, {"Talk", "talk"}, {"Pray", "pray"}, {"Fight", "fight"},
            {"Pain", "pain"}, {"Eat", "eat"}, {"Work", "work"}, {"Act", "work"}, {"Cast", "cast"},
            {"Interact", "work"}, {"Flee", "sprint"}, {"Incapacitated", "down"}};
        for (const auto& [s, c] : kByState)
            if (a.state == s) {
                const int k = st(c);
                if (k >= 0) return k;
            }
        return st("idle");
    }

    Figure personFigure(const ActorSnap& a, double rx, double ry, double rz, double moveYaw, float highlight) {
        const double nowS = lastFrameS_;
        Anim& an = anims_[a.id];
        // Visual speed: the sim's step over the real time a tick takes.
        const double step = std::hypot(a.x - a.px, a.y - a.py);
        const double speed = shown_.paused ? 0.0 : step / std::max(0.05, shown_.tickSeconds);
        double rate = 1;
        const int want = personClip(a, speed, rate);
        if (want != an.clip) {
            an.from = an.clip;
            an.fromStart = an.start;
            an.fromRate = an.rate;
            an.clip = want;
            an.start = nowS;
            an.switched = nowS;
        }
        an.rate += (rate - an.rate) * std::min(1.0, frameDt_ * 4.0);
        // Turn towards the way of travel, not in a snap.
        const float target = float(moveYaw);
        if (!an.yawSet) {
            an.yaw = target;
            an.yawSet = true;
        }
        if (step > 0.05) {
            const float d = std::remainder(target - an.yaw, float(2 * kPi));
            an.yaw += d * float(std::min(1.0, frameDt_ * 8.0));
        }
        Figure f;
        const std::uint64_t h = std::uint64_t(a.id) * 0x9E3779B97F4A7C15ull;
        const auto& pool = a.sex == 'f' ? femaleLooks_ : maleLooks_;
        f.look = pool.empty() ? 0 : pool[std::size_t((h >> 17) % pool.size())];
        f.clip = an.clip;
        f.time = (nowS - an.start) * an.rate;
        if (an.from >= 0 && nowS - an.switched < 0.35) {
            f.fromClip = an.from;
            f.fromTime = (nowS - an.fromStart) * an.fromRate;
            f.fade = float(1.0 - (nowS - an.switched) / 0.35);
        }
        f.rel[0] = rx;
        f.rel[1] = ry;
        f.rel[2] = rz;
        f.yaw = an.yaw;
        f.scale = a.age < 16 ? float(0.55 + 0.028 * a.age) : 1.0f;
        f.highlight = highlight;
        personTints(a, f.skin, f.hair, f.cloth);
        return f;
    }

    // Skin from a small range of tones, hair from the family's line, cloth a
    // muted shade of the family's colour. Shared by the figure and its portrait.
    void personTints(const ActorSnap& a, float skin[3], float hair[3], float cloth[3]) const {
        static const float kSkin[][3] = {{1.00f, 0.92f, 0.86f}, {0.94f, 0.80f, 0.70f}, {0.82f, 0.66f, 0.54f},
                                         {0.66f, 0.50f, 0.40f}, {1.05f, 0.88f, 0.78f}};
        static const float kHair[][3] = {{0.32f, 0.22f, 0.14f}, {0.16f, 0.12f, 0.10f}, {0.55f, 0.38f, 0.20f},
                                         {0.70f, 0.55f, 0.32f}, {0.45f, 0.20f, 0.10f}, {0.08f, 0.07f, 0.07f}};
        const std::uint64_t h = std::uint64_t(a.id) * 0x9E3779B97F4A7C15ull;
        const std::uint64_t fh = hashStr(a.family);
        const auto& sk = kSkin[(fh + (h >> 29) % 2) % 5];
        const auto& hr = kHair[(fh >> 7) % 6];
        for (int k = 0; k < 3; ++k) {
            skin[k] = sk[k];
            hair[k] = a.age > 50 ? hr[k] * 0.4f + 0.45f : hr[k];
        }
        float fc[3];
        familyColour(a.family, fc);
        for (int k = 0; k < 3; ++k) cloth[k] = (0.62f + fc[k] * 0.55f) * (a.alive ? 1.0f : 0.6f);
    }

    void buildInstances() {
        instances_.clear();
        figures_.clear();
        const double nowS = std::chrono::duration<double>(Clock::now() - started_).count();
        frameDt_ = std::clamp(nowS - lastFrameS_, 0.0, 0.1);
        lastFrameS_ = nowS;
        std::vector<Instance> per[kMeshes];
        const double alpha = tickAlpha();
        const double range = std::max(1500.0, zoom_ * 4.0);
        for (const auto& a : shown_.actors) {
            if (a.species == "fish_shoal") continue;
            double wx, wy;
            actorWorld(a, alpha, wx, wy);
            const double rx = wx - eye_[0], ry = wy - eye_[1];
            if (rx * rx + ry * ry > range * range) continue;
            const double rz = groundAt(wx, wy) - eye_[2];
            double yaw = 0;
            const double mx = a.x - a.px, my = a.y - a.py;
            if (mx * mx + my * my > 0.01) yaw = std::atan2(my, mx);
            else yaw = double(a.id % 628) / 100.0;
            Instance in{};
            in.placeYaw[0] = float(rx), in.placeYaw[1] = float(ry), in.placeYaw[2] = float(rz), in.placeYaw[3] = float(yaw);
            const float hl = (a.id == selected_ ? 1.0f : 0.0f) + (a.player ? 0.6f : 0.0f);
            if (!a.animal && people_.loaded()) {
                if (figures_.size() < SkinnedSet::kMaxFigures) figures_.push_back(personFigure(a, rx, ry, rz, yaw, hl));
            } else if (!a.animal) {
                const float s = a.age < 14 ? 0.55f + 0.03f * float(a.age) : 1.0f;
                in.scale[0] = in.scale[1] = in.scale[2] = s;
                in.scale[3] = hl;
                familyColour(a.family, in.colour);
                if (!a.alive) in.colour[0] = in.colour[1] = in.colour[2] = 0.25f;
                in.colour2[0] = 0.86f, in.colour2[1] = 0.70f, in.colour2[2] = 0.56f;
                if (a.fighting) in.colour[0] = 0.85f, in.colour[1] = 0.2f, in.colour[2] = 0.15f;
                if (!a.alive || a.down) {   // lying on the ground
                    in.scale[2] = 0.25f * s;
                    in.scale[0] = in.scale[1] = 2.2f * s;
                }
                per[kPerson].push_back(in);
            } else {
                float s = 1.0f;
                float col[3] = {0.45f, 0.36f, 0.28f};
                const std::string& sp = a.species;
                if (sp == "hare") s = 0.35f, col[0] = 0.55f, col[1] = 0.50f, col[2] = 0.42f;
                else if (sp == "deer") s = 1.5f, col[0] = 0.55f, col[1] = 0.38f, col[2] = 0.22f;
                else if (sp == "boar") s = 1.0f, col[0] = 0.25f, col[1] = 0.20f, col[2] = 0.17f;
                else if (sp == "wolf") s = 1.0f, col[0] = 0.42f, col[1] = 0.42f, col[2] = 0.44f;
                else if (sp == "hens") s = 0.3f, col[0] = 0.85f, col[1] = 0.80f, col[2] = 0.70f;
                else if (sp == "goats") s = 0.8f, col[0] = 0.80f, col[1] = 0.76f, col[2] = 0.66f;
                else if (sp == "pig") s = 0.9f, col[0] = 0.85f, col[1] = 0.62f, col[2] = 0.58f;
                // A herd or a pack is several heads: a few beside the leader.
                const int heads = std::clamp(a.group, 1, 6);
                for (int k = 0; k < heads; ++k) {
                    Instance b = in;
                    if (k) {
                        const double ang = double(k) * 2.4 + double(a.id), r = 1.6 * s + 0.5 * k;
                        const double bx = wx + std::cos(ang) * r, by = wy + std::sin(ang) * r;
                        b.placeYaw[0] = float(bx - eye_[0]);
                        b.placeYaw[1] = float(by - eye_[1]);
                        b.placeYaw[2] = float(groundAt(bx, by) - eye_[2]);
                    }
                    b.scale[0] = b.scale[1] = b.scale[2] = s;
                    b.scale[3] = hl;
                    std::memcpy(b.colour, col, sizeof col);
                    b.colour2[0] = col[0] * 0.8f, b.colour2[1] = col[1] * 0.8f, b.colour2[2] = col[2] * 0.8f;
                    if (!a.alive) b.scale[2] = 0.3f * s;
                    per[kBeast].push_back(b);
                }
            }
        }
        for (const auto& b : buildings_) {
            const double wx = originX_ + b.x, wy = originY_ + b.y;
            const double rx = wx - eye_[0], ry = wy - eye_[1];
            if (rx * rx + ry * ry > range * range * 4) continue;
            Instance in{};
            in.placeYaw[0] = float(rx), in.placeYaw[1] = float(ry);
            in.placeYaw[2] = float(groundAt(wx, wy) - eye_[2] - 0.3);
            in.placeYaw[3] = float((hashStr(b.type) % 4) * 0.4);
            Mesh mesh = kBlock;
            auto set = [&](float sx, float sy, float sz, float r, float g, float bl, float r2, float g2, float b2) {
                in.scale[0] = sx, in.scale[1] = sy, in.scale[2] = sz;
                in.colour[0] = r, in.colour[1] = g, in.colour[2] = bl;
                in.colour2[0] = r2, in.colour2[1] = g2, in.colour2[2] = b2;
            };
            if (b.type == "house") mesh = kHouse, set(8, 6, 3.2f, 0.52f, 0.42f, 0.30f, 0.42f, 0.33f, 0.20f);
            else if (b.type == "storage") mesh = kHouse, set(4.5f, 4, 2.6f, 0.46f, 0.36f, 0.24f, 0.36f, 0.30f, 0.18f);
            else if (b.type == "hearth") set(1.4f, 1.4f, 0.8f, 0.30f, 0.29f, 0.27f, 0.30f, 0.29f, 0.27f);
            else if (b.type == "village_fire") set(2.2f, 2.2f, 0.9f, 0.85f, 0.42f, 0.12f, 0.85f, 0.42f, 0.12f);
            else if (b.type == "shrine") set(1.6f, 1.6f, 3.4f, 0.55f, 0.55f, 0.52f, 0.55f, 0.55f, 0.52f);
            else if (b.type == "field") set(40, 30, 0.5f, 0.50f, 0.42f, 0.22f, 0.50f, 0.42f, 0.22f);
            else if (b.type == "garden") set(12, 10, 0.5f, 0.28f, 0.40f, 0.18f, 0.28f, 0.40f, 0.18f);
            per[mesh].push_back(in);
        }
        for (int m = 0; m < kMeshes; ++m) {
            const std::size_t room = kMaxInstances - instances_.size();
            const std::size_t n = std::min(room, per[m].size());
            batch_[m] = {std::uint32_t(instances_.size()), std::uint32_t(n)};
            instances_.insert(instances_.end(), per[m].begin(), per[m].begin() + std::ptrdiff_t(n));
        }
    }

    bool openGpu(std::string* why) {
        MeshBuilder mb;
        auto range = [&](Mesh m, std::size_t start) {
            meshes_[m] = {std::uint32_t(start), std::uint32_t(mb.v.size() - start)};
        };
        std::size_t s = mb.v.size();
        mb.frustum(8, 0.24f, 0.0f, 0.17f, 1.38f, 0.0f, 1.0f, 0.75f);   // body
        mb.frustum(6, 0.13f, 1.40f, 0.10f, 1.74f, 1.0f);              // head
        range(kPerson, s);
        s = mb.v.size();
        mb.box(-0.5f, -0.2f, 0.35f, 0.5f, 0.2f, 0.75f, 0.0f);   // body
        mb.box(0.45f, -0.12f, 0.55f, 0.75f, 0.12f, 0.85f, 1.0f); // head
        for (float lx : {-0.4f, 0.35f})
            for (float ly : {-0.15f, 0.1f}) mb.box(lx, ly, 0.0f, lx + 0.07f, ly + 0.07f, 0.36f, 1.0f);
        range(kBeast, s);
        s = mb.v.size();
        mb.box(-0.5f, -0.5f, 0.0f, 0.5f, 0.5f, 1.0f, 0.0f);
        {   // a gable roof over the unit box
            const float a[3] = {-0.55f, -0.55f, 1.0f}, b[3] = {0.55f, -0.55f, 1.0f}, c[3] = {0.55f, 0.0f, 1.7f},
                        d[3] = {-0.55f, 0.0f, 1.7f}, e[3] = {0.55f, 0.55f, 1.0f}, f[3] = {-0.55f, 0.55f, 1.0f};
            mb.quad(a, b, c, d, 1.0f);
            mb.quad(e, f, d, c, 1.0f);
            mb.tri(b, e, c, 0.0f);
            mb.tri(f, a, d, 0.0f);
        }
        range(kHouse, s);
        s = mb.v.size();
        mb.box(-0.5f, -0.5f, 0.0f, 0.5f, 0.5f, 1.0f, 0.0f);
        range(kBlock, s);
        vertBuffer_ = device_.uploadBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, mb.v.data(), mb.v.size() * sizeof(Vertex));
        instBuffer_ = device_.makeBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, kMaxInstances * sizeof(Instance));
        SDL_GPUTransferBufferCreateInfo t{};
        t.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        t.size = std::uint32_t(kMaxInstances * sizeof(Instance));
        instTransfer_ = SDL_CreateGPUTransferBuffer(device_.handle(), &t);

        engine::PipelineWanted wanted;
        wanted.shaderFile = "altera_game/actors.hlsl";
        wanted.vertexEntry = "actorsVs";
        wanted.fragmentEntry = "actorsPs";
        wanted.depthClip = true;
        wanted.cull = SDL_GPU_CULLMODE_NONE;
        wanted.buffers = {{0, sizeof(Vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0},
                          {1, sizeof(Instance), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0}};
        wanted.attributes = {{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0},
                             {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 12},
                             {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, 24},
                             {3, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0},
                             {4, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 16},
                             {5, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 32},
                             {6, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 48}};
        actorsPipe_ = device_.makePipeline(wanted);

        engine::PipelineWanted over;
        over.shaderFile = "altera_game/overlay.hlsl";
        over.vertexEntry = "overlayVs";
        over.fragmentEntry = "overlayPs";
        over.depthTest = over.depthWrite = false;
        over.blend = true;
        over.colourFormats = {engine::Device::kColourFormat};
        overlayPipe_ = device_.makePipeline(over);
        SDL_GPUSamplerCreateInfo sampling{};
        sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_NEAREST;
        sampling.address_mode_u = sampling.address_mode_v = sampling.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler_ = device_.makeSampler(sampling);
        if (!vertBuffer_ || !instBuffer_ || !instTransfer_ || !actorsPipe_ || !overlayPipe_ || !sampler_) {
            if (why) *why = "game GPU objects: " + device_.error();
            return false;
        }
        {   // The people on the UAL rig; without the bake they stay placeholder shapes.
            const fs::path root(opts_.root);
            std::string whyPeople;
            if (!people_.load(device_, root / "assets/altera_game/people/people.askin",
                              root / "content/animations/humanoid_ual/baked/people.aanim",
                              root / "content/animations/humanoid_ual/baked/people_anim.json", &whyPeople))
                SDL_Log("altera_game: people stay placeholders: %s", whyPeople.c_str());
            for (int i = 0; i < people_.lookCount(); ++i)
                (people_.lookName(i)[0] == 'f' ? femaleLooks_ : maleLooks_).push_back(i);
        }
        const fs::path fonts = fs::path(opts_.root) / "assets" / "fonts";
        font_ = ui::FontFace::load(fonts / "NotoSans-Regular.ttf");
        fontBold_ = ui::FontFace::load(fonts / "NotoSans-Bold.ttf");
        return true;
    }

    // ---- the HUD (style G) ------------------------------------------------
    void styleTheme() {
        ui::Theme& t = ui_.theme();
        t.slot = ui::Colour::rgb(22, 29, 40, 0.94f);
        t.slotHot = ui::Colour::rgb(40, 50, 66, 0.96f);
        t.slotDown = ui::Colour::rgb(226, 197, 122);
        t.slotEdge = ui::Colour::rgb(168, 135, 62);
        t.label = ui::Colour::rgb(239, 230, 207);
        t.labelSoft = ui::Colour::rgb(176, 166, 140);
        t.accent = ui::Colour::rgb(226, 197, 122);
        t.paper = ui::Colour::rgb(239, 228, 200);
        t.paperEdge = ui::Colour::rgb(138, 111, 52);
        t.ink = ui::Colour::rgb(43, 38, 32);
        t.inkSoft = ui::Colour::rgb(94, 85, 70);
        t.gauge = ui::Colour::rgb(16, 22, 30, 0.85f);
    }

    const ui::Colour kNavy = ui::Colour::rgb(22, 29, 40, 0.93f);
    const ui::Colour kGold = ui::Colour::rgb(226, 197, 122);
    const ui::Colour kGoldPale = ui::Colour::rgb(243, 226, 168);
    const ui::Colour kGoldDark = ui::Colour::rgb(168, 135, 62);
    const ui::Colour kText = ui::Colour::rgb(239, 230, 207);
    const ui::Colour kSoft = ui::Colour::rgb(176, 166, 140);
    const ui::Colour kInk = ui::Colour::rgb(43, 38, 32);
    const ui::Colour kRed = ui::Colour::rgb(200, 64, 44);
    const ui::Colour kGreen = ui::Colour::rgb(79, 154, 76);
    const ui::Colour kAmber = ui::Colour::rgb(201, 150, 42);

    void pill(const ui::Rect& r) {
        ui_.roundRect(r, kNavy, r.h * 0.5f);
        ui_.border(r, kGoldPale, 1.5f, r.h * 0.5f);
        ui_.claim(r);
    }
    void leather(const ui::Rect& r) {
        ui_.roundRect(r, kNavy, 14);
        ui_.border(r, kGoldDark, 2.0f, 14);
        ui_.claim(r);
    }
    void paper(const ui::Rect& r) {
        ui_.roundRect(r, ui::Colour::rgb(239, 228, 200, 0.97f), 16);
        ui_.border(r, kGoldDark, 2.0f, 16);
        ui_.claim(r);
    }
    ui::Colour needColour(double v) const { return v < 0.25 ? kRed : v < 0.5 ? kAmber : kGreen; }

    std::string stateText(const ActorSnap& a) const {
        if (!a.alive) return text(a.sex == 'f' ? "game.dead_f" : "game.dead");
        if (a.down) return text("game.unconscious");
        std::string s = text("game.fsm." + a.state);
        if (!a.act.empty()) s += ": " + a.act;
        return s;
    }

    void buildHud(float W, float H) {
        const Snapshot& s = shown_;
        const ActorSnap* sel = actor(selected_);
        const double alpha = tickAlpha();

        // Names over the people near the camera.
        for (const auto& a : s.actors) {
            if (a.animal && a.id != selected_) continue;
            double wx, wy;
            actorWorld(a, alpha, wx, wy);
            const double rx = wx - eye_[0], ry = wy - eye_[1];
            const double d2 = rx * rx + ry * ry;
            if (d2 > 120.0 * 120.0 && a.id != selected_) continue;
            if (!a.alive && a.id != selected_) continue;
            float sx, sy;
            if (!project(rx, ry, groundAt(wx, wy) + 2.1 - eye_[2], sx, sy)) continue;
            const std::string label = a.player ? "* " + a.name : a.name;
            const float tw = ui_.textWidth(label, 0.9f);
            const ui::Colour c = a.id == selected_ ? kGoldPale : (a.alive ? kText : kSoft);
            ui_.rect({sx - tw * 0.5f - 5, sy - 10, tw + 10, 19}, ui::Colour::rgb(10, 14, 20, 0.55f));
            ui_.text(sx - tw * 0.5f, sy - 9, label, c, 0.9f);
        }

        // Top right: the clock and the place.
        {
            const ui::Rect r{W - 300, 18, 282, 58};
            leather(r);
            const int day = int(s.tick / kTicksPerDay) + 1;
            std::string line1 = text("game.day", {{"day", day}}) + ", " + clockText(s.tick);
            ui_.text(r.x + 16, r.y + 8, line1, kGoldPale, 1.15f, true);
            char t[32];
            std::snprintf(t, sizeof t, "%.0f", s.tempC);
            std::string line2 = s.settlement + " | " + text("game.season." + s.season) + " | " +
                                text("game.temp", {{"t", std::string(t)}});
            if (s.rain > 0.5) line2 += ", " + text("game.rain");
            ui_.text(r.x + 16, r.y + 33, ui_.fit(line2, r.w - 24), kSoft, 0.9f);
        }

        // Top left: the families and their people.
        {
            float y = 18;
            std::map<std::string, std::vector<const ActorSnap*>> fam;
            for (const auto& a : s.actors)
                if (!a.animal) fam[a.family].push_back(&a);
            for (auto& [f, list] : fam) {
                const float h = 28.0f + 24.0f * float(list.size());
                const ui::Rect r{18, y, 250, h};
                leather(r);
                float col[3];
                familyColour(f, col);
                ui_.text(r.x + 14, r.y + 6, f, ui::Colour(col[0], col[1], col[2]), 1.0f, true);
                float ly = r.y + 28;
                for (const ActorSnap* a : list) {
                    const ui::Rect row{r.x + 6, ly, r.w - 12, 23};
                    if (ui_.selectable(ui::widgetId("hud.person", int(a->id)), row, a->id == selected_)) select(a->id);
                    const ui::Colour c = !a->alive ? kSoft : (a->id == selected_ ? kGoldPale : kText);
                    std::string nm = (a->player ? "* " : "") + a->name;
                    ui_.text(row.x + 8, row.y + 3, ui_.fit(nm, 120, 0.9f), c, 0.9f);
                    ui_.text(row.x + 128, row.y + 4, ui_.fit(stateText(*a), 108, 0.8f), kSoft, 0.8f);
                    ly += 24;
                }
                y += h + 10;
                if (y > H * 0.55f) break;
            }
        }

        // Bottom right: pause and speed.
        {
            const ui::Rect r{W - 252, H - 62, 234, 44};
            pill(r);
            float x = r.x + 8;
            if (ui_.button(ui::widgetId("hud.pause"), {x, r.y + 6, 44, 32}, s.paused ? ">" : "||",
                           s.paused ? kGold : kNavy, s.paused ? kInk : kText))
                post({"pause"});
            x += 50;
            static const char* kLabels[] = {"", ">", ">>", ">>>", "MAX"};
            for (int k = 1; k <= 4; ++k) {
                const float w = k == 4 ? 50.0f : 36.0f;
                const bool on = !s.paused && s.speed == k;
                if (ui_.button(ui::widgetId("hud.speed", k), {x, r.y + 6, w, 32}, kLabels[k], on ? kGold : kNavy,
                               on ? kInk : kText))
                    post({"speed", k});
                x += w + 6;
            }
        }

        // Bottom left: the selected character.
        if (sel) {
            const Detail& d = s.detail;
            const bool person = !sel->animal;
            const float rows = person ? float(d.needs.size()) : 0.0f;
            const float h = 150 + rows * 22;
            const ui::Rect r{18, H - h - 18, 340, h};
            leather(r);
            float col[3];
            familyColour(sel->family, col);
            const ui::Rect face{r.x + 16, r.y + 16, 54, 54};
            ui_.roundRect(face, ui::Colour(col[0], col[1], col[2]), 27);
            ui_.border(face, kGoldPale, 2, 27);
            ui_.textCentred(face, sel->name.substr(0, 1), kInk, 1.6f, true);
            ui_.text(r.x + 82, r.y + 14, ui_.fit(sel->name, r.w - 96, 1.2f, true), kGoldPale, 1.2f, true);
            std::string sub = person ? sel->family + ", " + text("game.age", {{"n", sel->age}}) : "";
            if (sel->animal && sel->group > 1) sub = "x" + std::to_string(sel->group);
            ui_.text(r.x + 82, r.y + 40, ui_.fit(sub, r.w - 96, 0.85f), kSoft, 0.85f);
            float y = r.y + 80;
            ui_.text(r.x + 16, y, ui_.fit(stateText(*sel), r.w - 32), kText);
            y += 22;
            ui_.text(r.x + 16, y, ui_.fit(text("game.goal", {{"goal", sel->goal.empty() ? text("game.no_goal") : sel->goal}}), r.w - 32, 0.9f),
                     kSoft, 0.9f);
            y += 24;
            auto bar = [&](const std::string& label, double v, const ui::Colour& c) {
                ui_.text(r.x + 16, y, ui_.fit(label, 120, 0.85f), kText, 0.85f);
                ui_.gauge({r.x + 140, y + 5, r.w - 156, 9}, float(std::clamp(v, 0.0, 1.0)), c, ui::Colour::rgb(0, 0, 0, 0.45f));
                y += 22;
            };
            bar(text("game.health"), sel->hp, needColour(sel->hp));
            if (person) {
                for (const auto& [nm, v] : d.needs) bar(nm, v, needColour(v));
            }
            if (sel->wounds) ui_.text(r.x + 16, y, text("game.wounds", {{"n", sel->wounds}}), kRed, 0.85f);
        }

        // Bottom centre: the player's orders for the selected person.
        if (sel && !sel->animal && sel->alive) {
            const float w = 640;
            const ui::Rect r{(W - w) * 0.5f, H - 108, w, 90};
            leather(r);
            float x = r.x + 12;
            if (!sel->player) {
                if (ui_.button(ui::widgetId("hud.take"), {x, r.y + 10, 150, 32}, text("game.take_control"), kGold, kInk))
                    post({"cmd", 0, "take_control", {{"id", sel->id}}});
            } else {
                if (ui_.button(ui::widgetId("hud.release"), {x, r.y + 10, 150, 32}, text("game.release")))
                    post({"cmd", 0, "release_control", {{"id", sel->id}}});
            }
            x += 158;
            // The goal to order or hint: a button that opens the list upwards
            // (a list down from the bottom bar would leave the screen).
            const std::string chosen = goalChoice_ >= 0 && goalChoice_ < int(goalNames_.size())
                                           ? goalNames_[size_t(goalChoice_)] : "";
            if (ui_.button(ui::widgetId("hud.goal"), {x, r.y + 10, 250, 32}, ui_.fit(chosen, 230)))
                goalListOpen_ = !goalListOpen_;
            if (goalListOpen_) {
                const int n = int(goalNames_.size()), cols = 3, rows = (n + cols - 1) / cols;
                const float cw = 210, rh = 28;
                const ui::Rect list{(W - cw * cols) * 0.5f - 8, r.y - rows * rh - 24, cw * cols + 16, rows * rh + 16};
                leather(list);
                for (int i = 0; i < n; ++i) {
                    const ui::Rect cell{list.x + 8 + cw * float(i / rows), list.y + 8 + rh * float(i % rows), cw - 4, rh - 2};
                    if (ui_.selectable(ui::widgetId("hud.goal.item", i), cell, i == goalChoice_)) {
                        goalChoice_ = i;
                        goalListOpen_ = false;
                    }
                    ui_.text(cell.x + 6, cell.y + 4, ui_.fit(goalNames_[size_t(i)], cw - 16, 0.85f),
                             i == goalChoice_ ? kGoldPale : kText, 0.85f);
                }
            }
            x += 258;
            const std::string g = goalChoice_ >= 0 && goalChoice_ < int(goalIds_.size()) ? goalIds_[size_t(goalChoice_)] : "";
            if (sel->player) {
                if (ui_.button(ui::widgetId("hud.order"), {x, r.y + 10, 100, 32}, text("game.order"), kGold, kInk) && !g.empty())
                    post({"cmd", 0, "set_goal", {{"id", sel->id}, {"goal", g}}});
                x += 108;
                if (ui_.button(ui::widgetId("hud.cancel"), {x, r.y + 10, 100, 32}, text("game.cancel")))
                    post({"cmd", 0, "cancel_goal", {{"id", sel->id}}});
            }
            float x2 = r.x + 12 + 158;
            if (ui_.button(ui::widgetId("hud.want"), {x2, r.y + 50, 120, 30}, text("game.want")) && !g.empty())
                post({"cmd", 0, "hint", {{"id", sel->id}, {"goal", g}, {"delta", 0.4}, {"ttl", 144}}});
            if (ui_.button(ui::widgetId("hud.dontwant"), {x2 + 128, r.y + 50, 120, 30}, text("game.dont_want")) && !g.empty())
                post({"cmd", 0, "hint", {{"id", sel->id}, {"goal", g}, {"delta", -0.4}, {"ttl", 144}}});
            if (sel->player) {
                const bool fw = s.detail.freeWill;
                if (ui_.button(ui::widgetId("hud.freewill"), {x2 + 256, r.y + 50, 200, 30},
                               text(fw ? "game.free_will_on" : "game.free_will_off")))
                    post({"cmd", 0, "set_free_will", {{"id", sel->id}, {"on", !fw}}});
            }
            if (!s.result.empty() && s.result != "ok")
                ui_.text(r.x + 12, r.y + 56, ui_.fit(s.result, 150, 0.75f), kRed, 0.75f);
        }

        // Right: the chronicle on paper, the thoughts under it.
        float py = 90;
        if (showChronicle_) {
            const float w = 420, h = 250;
            const ui::Rect r{W - w - 18, py, w, h};
            paper(r);
            ui_.text(r.x + 18, r.y + 10, text("game.chronicle"), kInk, 1.15f, true);
            float y = r.y + h - 26;
            for (auto it = s.chronicle.rbegin(); it != s.chronicle.rend() && y > r.y + 38; ++it) {
                const std::string line = "d" + std::to_string(it->day) + " " + it->time + "  " + it->text;
                const ui::Colour c = it->kind == "death" ? ui::Colour::rgb(150, 40, 31)
                                                         : (it->kind.rfind("combat", 0) == 0 ? ui::Colour::rgb(140, 80, 20) : kInk);
                ui_.text(r.x + 16, y, ui_.fit(line, r.w - 32, 0.85f), c, 0.85f);
                y -= 20;
            }
            py += h + 10;
        }
        if (showThoughts_ && sel && !s.detail.trace.empty()) {
            const float w = 420, h = 300;
            const ui::Rect r{W - w - 18, py, w, h};
            leather(r);
            ui_.text(r.x + 16, r.y + 10, text("game.thoughts"), kGoldPale, 1.05f, true);
            float y = r.y + h - 24;
            for (auto it = s.detail.trace.rbegin(); it != s.detail.trace.rend() && y > r.y + 34; ++it) {
                ui_.text(r.x + 14, y, ui_.fit(it->time + "  " + it->text, r.w - 28, 0.8f), kText, 0.8f);
                y -= 19;
            }
        }
        // The keys, faint, along the top.
        ui_.text(W * 0.5f - ui_.textWidth(text("game.help"), 0.75f) * 0.5f, 6, text("game.help"),
                 ui::Colour::rgb(239, 230, 207, 0.55f), 0.75f);
        // Selection by name rows: the ui sets selected_ directly; keep the sim's detail in step.
        selectedWanted_ = selected_;
    }

    static constexpr std::size_t kMaxInstances = 8192;

    engine::Device& device_;
    SDL_Window* window_;
    Json script_;
    std::size_t scriptStep_ = 0;
    double scriptClock_ = 0;
    std::string shotWanted_, shotPending_, panel_ = "none";
    SDL_GPUTransferBuffer* shotTransfer_ = nullptr;
    std::uint32_t shotW_ = 0, shotH_ = 0;
    int shotCounter_ = 0;
    terrain_core::GroundHeight ground_;
    double worldSize_;
    GameOptions opts_;
    std::unique_ptr<Content> content_;
    std::unique_ptr<Loc> loc_;
    std::unique_ptr<World> world_;
    double originX_ = 0, originY_ = 0;
    std::vector<BuildingSnap> buildings_;
    std::vector<std::string> goalIds_, goalNames_;
    int goalChoice_ = 0;
    bool goalListOpen_ = false;

    // Sim thread state (only the sim thread touches these after open()).
    std::thread thread_;
    std::atomic<bool> stop_{false};
    bool paused_ = true;
    int speed_ = 2;
    std::string result_;
    Clock::time_point tickAt_ = Clock::now();
    std::map<EntityId, Vec2> prevPos_;
    std::atomic<EntityId> selectedWanted_{kNone};
    EntityId snapSelected_ = kNone;

    std::mutex cmdMutex_;
    std::deque<Command> commands_;
    std::mutex snapMutex_;
    Snapshot snap_;

    // Frame state (main thread).
    Snapshot shown_;
    EntityId selected_ = kNone;
    bool follow_ = false, showChronicle_ = true, showThoughts_ = false, rmb_ = false, overUi_ = false;
    struct PickAt {
        float x = 0, y = 0;
        bool pending = false;
    } pickAt_;
    double target_[2] = {0, 0};
    double zoom_ = 18.0, orbitYaw_ = 0.8, orbitPitch_ = 0.45;
    float vp_[16] = {};
    double eye_[3] = {};
    std::uint32_t viewW_ = 1, viewH_ = 1;

    std::vector<Instance> instances_;
    SkinnedSet people_;
    std::vector<int> maleLooks_, femaleLooks_;
    std::vector<Figure> figures_;
    struct Anim {
        int clip = -1, from = -1;
        double start = 0, fromStart = 0, switched = -10, rate = 1, fromRate = 1;
        float yaw = 0;
        bool yawSet = false;
    };
    std::map<EntityId, Anim> anims_;
    Clock::time_point started_ = Clock::now();
    double lastFrameS_ = 0, frameDt_ = 0;
    MeshRange meshes_[kMeshes], batch_[kMeshes];
    engine::Buffer vertBuffer_, instBuffer_;
    SDL_GPUTransferBuffer* instTransfer_ = nullptr;
    engine::GraphicsPipeline actorsPipe_, overlayPipe_;
    engine::Sampler sampler_;
    engine::Texture uiTexture_;
    SDL_GPUTransferBuffer* uiTransfer_ = nullptr;

    ui::Canvas canvas_;
    ui::Ui ui_;
    ui::Input input_;
    std::shared_ptr<ui::FontFace> font_, fontBold_;
};

} // namespace

std::unique_ptr<GameLayer> makeSimGame(engine::Device& device, SDL_Window* window, const terrain_core::MacroMap& macro,
                                       const terrain_core::Params& params, double centreX, double centreY,
                                       const GameOptions& options, std::string* why) {
    auto game = std::make_unique<SimGame>(device, window, macro, params);
    if (!game->open(centreX, centreY, options, why)) return nullptr;
    return game;
}

} // namespace altera::game
