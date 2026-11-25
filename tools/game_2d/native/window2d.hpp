#pragma once
// AL-94: the 2D client in its own window - no browser. SDL3 with its GPU
// renderer (Direct3D 12 on Windows): sprites from an atlas as quads, text with
// the engine's interface (src/engine/ui). The game behind it is the same
// process: every view is a request to the routes the page uses (Host::call,
// answered in place, no socket), so the window and the page (--web, kept as
// the debug client) show one game.
#include <functional>
#include <memory>
#include <string>

namespace game2d {

struct Host {
    // A route of tools/game_2d/main.cpp: method, path with query, body -> the answer's body.
    std::function<std::string(const std::string& method, const std::string& path, const std::string& body, int* code)> call;
    // The sim's steps owed (and the debug server's waiting requests), within this many milliseconds.
    std::function<void(int budgetMs)> pump;
    // The game asked to end (the menu's Quit).
    std::function<bool()> quitting;
};

class Window {
public:
    explicit Window(std::string root);   // the repo root: content/, assets/fonts
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // The window up, with the loading screen drawn. False when SDL or the renderer would not start.
    bool open();
    const std::string& error() const;
    // The game until the window closes or the game quits.
    void run(Host& host);
    // Starting failed: the reason on screen until the window is closed.
    void fail(const std::string& why);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// A window program has no console: the parent's (a terminal it was started
// from) is used when there is one; `need` makes one when there is none.
void attachConsole(bool need);

} // namespace game2d
