#include "game_view/loading_screen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "engine/render/device.hpp"
#include "engine/ui/canvas.hpp"
#include "engine/ui/font.hpp"
#include "engine/ui/ui.hpp"
#include "game_view/saves.hpp"
#include "game_view/ui_theme.hpp"

namespace lr::game {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

struct LoadingScreen::Impl {
    std::map<std::string, std::string> texts;
    std::shared_ptr<ui::FontFace> font, bold;
    UiTheme theme;   // AL-62: the player's interface style
    ui::Canvas canvas;
    ui::Ui ui;
    ui::Input input;
    engine::Texture picture;
    std::uint32_t pw = 0, ph = 0;
    SDL_GPUTransferBuffer* transfer = nullptr;
    std::string stage;
    float fraction = 0, nextFraction = 0;
    std::string shader;
    int compiled = 0, cached = 0;
    Clock::time_point start = Clock::now(), stageStart = Clock::now(), lastDraw{};
    bool drawing = false;
};

LoadingScreen::LoadingScreen() : impl_(std::make_unique<Impl>()) {}

LoadingScreen::~LoadingScreen() { finish(); }

void LoadingScreen::setup(const std::string& root, const std::string& lang) {
    for (const std::string& l : {std::string("ru"), lang}) {
        std::ifstream in(fs::path(root) / "content" / "loc" / l / "game_ui.json");
        const auto j = nlohmann::json::parse(in, nullptr, false);
        if (j.is_object())
            for (auto it = j.begin(); it != j.end(); ++it)
                if (it.value().is_string()) impl_->texts[it.key()] = it.value().get<std::string>();
    }
    impl_->theme = UiTheme::load(fs::path(root) / "content/ui/themes" / (loadSettings().uiStyle + ".json"));
    const fs::path fonts = fs::path(root) / "assets" / "fonts";
    const auto face = [&](const std::string& f) {
        return f.empty() ? std::shared_ptr<ui::FontFace>() : ui::FontFace::load(fonts / f);
    };
    impl_->font = face(impl_->theme.font("text"));
    impl_->bold = face(impl_->theme.font("title"));
    if (!impl_->font || !impl_->bold) {
        impl_->font = ui::FontFace::load(fonts / "NotoSans-Regular.ttf");
        impl_->bold = ui::FontFace::load(fonts / "NotoSans-Bold.ttf");
    }
}

std::string LoadingScreen::text(const std::string& key) const {
    const auto it = impl_->texts.find(key);
    return it != impl_->texts.end() ? it->second : key;
}

void LoadingScreen::attach(engine::Device& device, SDL_Window* window) {
    device_ = &device;
    window_ = window;
    device.onShaderProgress([this](const std::string& name, bool compiling) {
        if (compiling) {
            ++impl_->compiled;
            impl_->shader = name;
            std::printf("compiling shader %s\n", name.c_str());
        } else {
            ++impl_->cached;
        }
        pump();
    });
    device.onWait([this] { pump(); });
    pump();
}

void LoadingScreen::stage(const std::string& key, float fraction) {
    const double took = std::chrono::duration<double>(Clock::now() - impl_->stageStart).count();
    if (!impl_->stage.empty()) std::printf("loading: %s %.1f s\n", impl_->stage.c_str(), took);
    impl_->stage = key;
    impl_->fraction = fraction;
    impl_->stageStart = Clock::now();
    impl_->shader.clear();
    impl_->lastDraw = {};   // draw the new step at once
    pump();
}

void LoadingScreen::finish() {
    if (!device_) return;
    stage("", 1.0f);
    device_->onShaderProgress(nullptr);
    device_->onWait(nullptr);
    SDL_WaitForGPUIdle(device_->handle());
    if (impl_->transfer) SDL_ReleaseGPUTransferBuffer(device_->handle(), impl_->transfer);
    impl_->transfer = nullptr;
    impl_->picture = {};
    impl_->ui.shutdown();
    std::printf("loading: done in %.1f s (%d shaders compiled, %d from the cache)\n",
                std::chrono::duration<double>(Clock::now() - impl_->start).count(), impl_->compiled, impl_->cached);
    device_ = nullptr;
}

void LoadingScreen::pump() {
    if (!window_) return;
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) {
            std::printf("loading: closed by the player\n");
            std::fflush(stdout);
            std::_Exit(0);   // mid-open: nothing worth tearing down in order
        }
    if (!device_ || impl_->drawing) return;
    if (std::chrono::duration<double>(Clock::now() - impl_->lastDraw).count() < 0.05) return;
    impl_->drawing = true;
    draw();
    impl_->lastDraw = Clock::now();
    impl_->drawing = false;
}

void LoadingScreen::draw() {
    Impl& m = *impl_;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    if (w <= 0 || h <= 0) return;
    if (m.canvas.wouldResize(w, h)) {
        m.ui.shutdown();
        m.canvas.size(w, h);
        m.ui.init(m.canvas.renderer());
        if (m.font) m.ui.setFont(m.font, m.bold, 16, 1.0f);
    }
    if (!m.canvas) return;
    const float W = float(w), H = float(h);
    SDL_Renderer* sw = m.canvas.renderer();
    SDL_SetRenderDrawBlendMode(sw, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sw, 12, 14, 16, 255);
    SDL_RenderClear(sw);
    SDL_SetRenderDrawBlendMode(sw, SDL_BLENDMODE_BLEND);
    m.ui.begin(m.input, w, h);
    const auto tc = [&](const char* k) {
        const ThemeColour c = m.theme.colour(k);
        return ui::Colour{c.r, c.g, c.b, c.a};
    };
    const ui::Colour gold = tc("acc"), pale = tc("gold2"), textC = tc("ink"), soft = tc("mute");
    // the name in light spaced capitals, one letter at a time
    const std::string title = "ALTERA";
    const float sp = m.ui.textHeight(3.0f) * 0.3f;
    float tw = -sp;
    for (char ch : title) tw += m.ui.textWidth(std::string(1, ch), 3.0f, true) + sp;
    float tx = W * 0.5f - tw * 0.5f;
    for (char ch : title) {
        const std::string c(1, ch);
        m.ui.text(tx, H * 0.32f, c, gold, 3.0f, true);
        tx += m.ui.textWidth(c, 3.0f, true) + sp;
    }
    const float bw = std::min(720.0f, W * 0.6f), bx = (W - bw) * 0.5f, by = H * 0.55f;
    const double elapsed = std::chrono::duration<double>(Clock::now() - m.start).count();
    const double inStage = std::chrono::duration<double>(Clock::now() - m.stageStart).count();
    // Creep towards the next step's share while a step runs, never past it.
    const float shown = m.fraction + float(1.0 - std::exp(-inStage / 20.0)) * 0.08f;
    // a thin bar with a gold hairline round it, and a second one a step out
    const float bh = std::max(4.0f, m.theme.size("bar", 6.0f));
    const ui::Rect bar{bx, by, bw, bh};
    m.ui.rect(bar, tc("rim").withAlpha(0.8f));
    m.ui.rect({bx, by, bw * std::clamp(shown, 0.0f, 1.0f), bh}, gold);
    m.ui.border(bar, gold, 1.0f, 0);
    m.ui.border({bx - 3, by - 3, bw + 6, bh + 6}, gold.withAlpha(0.35f), 1.0f, 0);
    const std::string step = m.stage.empty() ? "" : text("loading." + m.stage);
    m.ui.text(bx, by - 34, step, pale, 1.15f, true);
    char t[64];
    std::snprintf(t, sizeof t, "%.0f s", elapsed);
    m.ui.textRight(bx + bw, by - 30, t, soft, 0.95f);
    if (!m.shader.empty()) {
        char line[256];
        std::snprintf(line, sizeof line, "%s: %s (%d)", text("loading.compiling").c_str(), m.shader.c_str(), m.compiled);
        m.ui.text(bx, by + 26, m.ui.fit(line, bw), textC, 0.95f);
        m.ui.paragraph(bx, by + 52, bw, text("loading.compiling_hint"), soft, 0.85f);
    }
    m.ui.end();
    SDL_RenderPresent(sw);

    SDL_GPUDevice* gpu = device_->handle();
    if (!m.picture || m.pw != std::uint32_t(w) || m.ph != std::uint32_t(h)) {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        info.width = std::uint32_t(w);
        info.height = std::uint32_t(h);
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        m.picture = device_->makeTexture(info);
        if (m.transfer) SDL_ReleaseGPUTransferBuffer(gpu, m.transfer);
        SDL_GPUTransferBufferCreateInfo tb{};
        tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tb.size = std::uint32_t(w) * std::uint32_t(h) * 4;
        m.transfer = SDL_CreateGPUTransferBuffer(gpu, &tb);
        m.pw = std::uint32_t(w);
        m.ph = std::uint32_t(h);
        if (!m.picture || !m.transfer) return;
    }
    const SDL_Surface* surf = m.canvas.surface();
    auto* map = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(gpu, m.transfer, true));
    if (!map) return;
    for (int y = 0; y < h; ++y)
        std::memcpy(map + std::size_t(y) * std::size_t(w) * 4,
                    static_cast<const std::uint8_t*>(surf->pixels) + std::size_t(y) * std::size_t(surf->pitch),
                    std::size_t(w) * 4);
    SDL_UnmapGPUTransferBuffer(gpu, m.transfer);
    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(gpu);
    if (!commands) return;
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = m.transfer;
    src.pixels_per_row = std::uint32_t(w);
    src.rows_per_layer = std::uint32_t(h);
    SDL_GPUTextureRegion dst{};
    dst.texture = m.picture.get();
    dst.w = std::uint32_t(w);
    dst.h = std::uint32_t(h);
    dst.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, true);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUTexture* swapchain = nullptr;
    Uint32 sw2 = 0, sh2 = 0;
    if (SDL_WaitAndAcquireGPUSwapchainTexture(commands, window_, &swapchain, &sw2, &sh2) && swapchain) {
        SDL_GPUBlitInfo blit{};
        blit.source.texture = m.picture.get();
        blit.source.w = std::uint32_t(w);
        blit.source.h = std::uint32_t(h);
        blit.destination.texture = swapchain;
        blit.destination.w = sw2;
        blit.destination.h = sh2;
        blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
        blit.filter = SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(commands, &blit);
    }
    SDL_SubmitGPUCommandBuffer(commands);
}

} // namespace lr::game
