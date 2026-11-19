#pragma once
// The loading screen: while the world opens (heightmap, water, shaders,
// plants, props, the sim), the window stays alive and says what is being done
// and how far along it is. Drawn with no shader at all (a picture made on the
// processor, blitted to the swapchain), so it works before any pipeline
// exists - including while the pipelines themselves compile.
//
// Long steps run on a worker thread (run()); the device's shader compiles do
// too (engine::Device::onWait). This thread pumps the window meanwhile.

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <SDL3/SDL.h>

namespace engine { class Device; }

namespace altera::game {

class LoadingScreen {
public:
    LoadingScreen();
    ~LoadingScreen();
    // The texts (content/loc/<lang>/game_ui.json, loading.* keys) and fonts.
    void setup(const std::string& root, const std::string& lang);
    // The device is open: from here on the screen is drawn and shader
    // compiles are reported and pumped.
    void attach(engine::Device& device, SDL_Window* window);
    // A new step (loading.<key> in the texts), `fraction` of the whole done.
    void stage(const std::string& key, float fraction);
    // Draws now and then, handles the window's events (closing it quits).
    void pump();
    // `work` on a worker thread while this one pumps; its result.
    template <class F>
    auto run(F&& work) -> decltype(work()) {
        if (!device_) return work();
        using R = decltype(work());
        std::atomic<bool> done{false};
        std::unique_ptr<R> out;
        std::thread worker([&] {
            out = std::make_unique<R>(work());
            done = true;
        });
        while (!done) {
            pump();
            SDL_Delay(15);
        }
        worker.join();
        return std::move(*out);
    }
    // Loading is over: the device forgets the screen.
    void finish();

private:
    void draw();
    std::string text(const std::string& key) const;

    struct Impl;
    std::unique_ptr<Impl> impl_;
    engine::Device* device_ = nullptr;
    SDL_Window* window_ = nullptr;
};

} // namespace altera::game
