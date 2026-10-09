#include "engine/render/device.hpp"
#include "engine/render/targets.hpp"
#include "terrain_core/dlss.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    int failed = 0;
    {
        engine::Device device;
        if (!device.openHeadless(640, 360, "assets/sprites", "direct3d12")) return 2;
        terrain_core::Dlss dlss;
        if (!dlss.open(device)) { std::fprintf(stderr, "SKIP: %s\n", dlss.status().c_str()); return 77; }
        engine::Targets input;
        input.sampledDepth(true);
        for (int quality = 0; quality < 3; ++quality) {
            const unsigned width = quality == 1 ? 800 : 640, height = quality == 1 ? 450 : 360;
            if (!dlss.prepare(true, width, height, quality) || !dlss.active() || dlss.renderWidth() >= width) return 4;
            if (!input.resize(device, dlss.renderWidth(), dlss.renderHeight())) return 5;
            auto* commands = SDL_AcquireGPUCommandBuffer(device.handle());
            SDL_GPUColorTargetInfo colour{};
            colour.texture = input.colour(); colour.load_op = SDL_GPU_LOADOP_CLEAR; colour.store_op = SDL_GPU_STOREOP_STORE;
            colour.clear_color = {0.2f, 0.4f, 0.6f, 1};
            SDL_GPUDepthStencilTargetInfo depth{};
            depth.texture = input.depth(); depth.load_op = SDL_GPU_LOADOP_CLEAR; depth.store_op = SDL_GPU_STOREOP_STORE;
            depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE; depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            SDL_EndGPURenderPass(SDL_BeginGPURenderPass(commands, &colour, 1, &depth));
            const float original[16]{1,0,0,0, 0,1,0,0, 0,0,0,0.25f, 0,0,1,0};
            float jittered[16]; std::copy_n(original, 16, jittered);
            const double eye[3]{};
            dlss.jitter(jittered);
            auto* output = dlss.evaluate(commands, input.colour(), input.depth(), original, eye, 16.6667f);
            if (!output || !device.submitFrame(commands)) return 6;
            SDL_WaitForGPUIdle(device.handle());
            std::vector<unsigned char> bytes(width * height * 4);
            if (!device.readTexture(output, bytes.data(), width, height)) return 7;
            const auto pixel = (height / 2 * width + width / 2) * 4;
            for (int c = 0; c < 3; ++c) {
                const int expected = 51 * (c + 1);
                if (std::abs(int(bytes[pixel + c]) - expected) > 12) ++failed;
            }
            std::printf("mode %d: %s, centre RGB %u %u %u\n", quality, dlss.status().c_str(), bytes[pixel], bytes[pixel+1], bytes[pixel+2]);
            if (dlss.prepare(false, width, height, quality) || dlss.active()) return 8;
            dlss.resetHistory();
            std::puts("disabled: native rendering; history reset");
        }
    }
    SDL_Quit();
    std::printf("toggle, resize, all quality modes and GPU output: %s\n", failed ? "FAIL" : "PASS");
    return failed ? 9 : 0;
}
