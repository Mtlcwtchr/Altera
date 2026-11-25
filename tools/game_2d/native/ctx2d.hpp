#pragma once
// What the 2D window's screens (menu, saves, talk, sheet, workshop, build,
// colony: tools/game_2d/native/*_view.cpp) use of the window: the routes, the
// strings, drawing in the "Резной рубеж" frame, the people and the camera.
// The window (window2d.cpp) is the one implementation.
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include "engine/ui/ui.hpp"

namespace game2d {

using Json = nlohmann::json;

// The frame's colours (tools/game_2d/web/style.css).
namespace pal {
ui::Colour hex(const char* h, float a = 1.0f);
extern const ui::Colour walnut, walnutD, bronze, bronzeL, leather, leatherD, parch, parchD, ink, gold, text, dim, bad, mid, good,
    player, out;
ui::Colour tone(double v);   // bad / mid / good
} // namespace pal

// Small readers of the routes' answers.
std::string jstr(const Json& j, const char* k);
double jnum(const Json& j, const char* k, double d = 0);
bool jflag(const Json& j, const char* k);
long long jid(const Json& j, const char* k);
const Json& jarr(const Json& j, const char* k);

class Ctx {
public:
    virtual ~Ctx() = default;
    // --- the game's routes and strings
    virtual std::string L(const std::string& k, const std::map<std::string, std::string>& args = {}) const = 0;
    virtual Json get(const std::string& path) = 0;
    virtual Json post(const std::string& path, const Json& body) = 0;
    virtual Json cmd(const Json& body) = 0;
    virtual void cmdToast(const Json& body) = 0;
    virtual void toast(const std::string& t) = 0;
    // --- the screen
    virtual ui::Ui& gui() = 0;
    virtual ui::Input& input() = 0;
    virtual float u() const = 0;          // the interface's scale
    virtual float screenW() const = 0;
    virtual float screenH() const = 0;
    virtual float mapW() const = 0;
    virtual float mapH() const = 0;
    virtual void fill(float x, float y, float w, float h, const ui::Colour& c) = 0;
    virtual void frame(float x, float y, float w, float h, const ui::Colour& c, float t) = 0;
    virtual void carved(const ui::Rect& r) = 0;
    virtual void parchBox(const ui::Rect& r) = 0;
    virtual void text(float x, float y, const std::string& s, const ui::Colour& c, float px, bool shadow = false) = 0;
    virtual void textC(float cx, float y, const std::string& s, const ui::Colour& c, float px, bool shadow = false) = 0;
    virtual float textW(const std::string& s, float px) const = 0;
    virtual float textH(float px) const = 0;
    virtual std::string fit(const std::string& s, float width, float px) const = 0;
    virtual std::vector<std::string> wrap(const std::string& s, float width, float px) const = 0;
    virtual float parchLines(float x, float y, float w, const std::vector<std::string>& ls, float px) = 0;
    virtual float chips(float x, float y, float w, const std::vector<std::string>& items) = 0;
    virtual void gauge(const ui::Rect& r, double v, const ui::Colour& c) = 0;
    // LR-137, the pixel skin: one art pixel in screen pixels, a leather plate (lit: gold edge),
    // a sunk well, a gauge of n pips (its width), a hud2d.json icon (9x9 art pixels; size 0: that)
    virtual float pixel() const = 0;
    virtual void plate(const ui::Rect& r, bool lit = false) = 0;
    virtual void well(const ui::Rect& r) = 0;
    virtual float pips(float x, float y, int n, double v, const ui::Colour& c) = 0;
    virtual void icon(const std::string& name, float x, float y, float size = 0) = 0;
    virtual float iconSize() const = 0;
    virtual const Json& hud() const = 0;   // content/sprites_2d/hud2d.json
    // a picture of the sprite set (content/sprites_2d/sprites.json), `size` pixels square; false: no such sprite
    virtual bool spriteAt(const std::string& name, const Json& recolour, float x, float y, float size, float alpha = 1.0f) = 0;
    // a pawn of the person (from the people the window knows), or a lettered stone
    virtual void face(long long id, const std::string& name, float x, float y, float size) = 0;
    // AL-136: a pawn drawn from a person-like JSON (sex, age, rskin, worn, look...) seen from `side` S|E|N|W
    virtual void pawn(const Json& person, char side, float x, float y, float size) = 0;
    // --- the game
    virtual const Json& statics() const = 0;   // /api/static
    virtual const Json& map2d() const = 0;     // content/sprites_2d/map2d.json
    virtual const Json& craftLook() const = 0; // content/sprites_2d/craft2d.json
    virtual const Json& resources() const = 0; // content/sprites_2d/resources.json
    virtual void walkTo(double x, double y) = 0;   // the led person walks there by the cells
    virtual void refreshMap() = 0;                 // the ground chunks ask again (the plan changed)
    virtual const Json* person(long long id) const = 0;
    virtual const std::vector<Json>& peopleList() const = 0;
    virtual long long meId() const = 0;
    virtual long long selected() const = 0;
    virtual void select(long long id) = 0;
    virtual void lookAt(double x, double y) = 0;
    virtual void setFollow(bool on) = 0;
    virtual void refresh() = 0;   // poll the state now
    virtual bool isPaused() const = 0;
    virtual void setPaused(bool on) = 0;
    virtual double camAtX() const = 0;
    virtual double camAtY() const = 0;
    virtual int zoomLevel() const = 0;
    // a new world in play (new game, a load): the map, the people, the camera start over
    virtual void worldChanged(const Json& view) = 0;
    virtual void quitGame() = 0;
    // the map in screen pixels
    virtual void toScreen(double wx, double wy, float& sx, float& sy) const = 0;
    virtual void toWorld(double sx, double sy, double& wx, double& wy) const = 0;
    virtual double tilePx() const = 0;
};

// The windows over the map (tools/game_2d/web/window.js): a carved frame over
// the darkened map, a title, tabs, a close button; Esc closes the top one.
struct Win {
    std::string id, title;
    std::vector<std::pair<std::string, std::string>> tabs;   // id, label
    std::string tab;
    float width = 0.62f, height = 0.78f;   // of the screen
    // LR-137: a window sized by its content: wantW its width in pixels, fitH its body's height
    // (render sets it; the next frame's box hugs it). 0: the fractions above.
    float wantW = 0, fitH = 0;
    // LR-143: the frame hugs what the window drew last frame (it is laid out at full height,
    // so the measure does not feed back); a long list still gets the full height
    bool autoFit = false;
    // LR-145: the big screens (sheet, dynasty, colony) are one size: this width and body height
    void big(float U) {
        wantW = 1180 * U;
        fitH = 648 * U;
    }
    std::function<void(Ctx&, Win&, const ui::Rect& body)> render;
    std::function<void(Ctx&, Win&)> onClose;
    std::function<void(Ctx&, Win&)> onPoll;   // with the state's poll
    std::function<bool(Ctx&, Win&, SDL_Scancode)> onKey;   // a key while it is on top
    bool closing = false;
    int frames = 0;   // drawn so far
    double polledAt = -1e9;
    Json data;   // the window's own state
};

class Screens {
public:
    explicit Screens(Ctx& c);
    ~Screens();
    // the screens by id: menu, new, pick, saves, chronicle, sheet, talk, trade, workshop, build, colony
    void open(const std::string& id, const Json& args = Json());
    bool isOpen(const std::string& id) const;
    bool any() const { return !stack_.empty(); }
    Win* top() { return stack_.empty() ? nullptr : stack_.back().get(); }
    Win& push(std::unique_ptr<Win> w);   // a window of a screen file
    void close(const std::string& id);
    void closeTop();
    void closeAll();
    // the frame: draws the windows (top last), true when they take the mouse
    void draw();
    void drawWorld();   // overlays on the map under the windows (build mode)
    // a key not typed into a field: true when a screen took it
    bool key(SDL_Scancode sc, SDL_Keymod mod);
    void polled();   // after each state poll
    // the pointer over the map at (wx, wy) this frame: true when a mode (build) takes it
    bool mapInput(double wx, double wy, const ui::Input& in, bool overUi);
    // the person menu's extra buttons (talk, trade) for a target person
    void personButtons(long long target, std::vector<std::pair<std::string, std::function<void()>>>& out);
    // the column's screen buttons
    std::vector<std::pair<std::string, std::string>> columnScreens() const;   // id, key letter
    void openScreen(const std::string& id);
    void start();   // the game's start: the main menu
    // run after the next frame is on the screen (a long call behind a "working" frame)
    void later(std::function<void()> f) { later_.push_back(std::move(f)); }
    struct Views;   // the screens' state, per file

private:
    Ctx& c_;
    std::vector<std::unique_ptr<Win>> stack_;
    std::unique_ptr<Views> v_;
    std::vector<std::function<void()>> later_;
    int laterFrames_ = 0;
    bool heldPause_ = false, holding_ = false;
    void hold();
    void releaseIfNone();
};

// The screen files register here (each defined in its own .cpp).
void openMenu(Ctx& c, Screens& s, const std::string& which, const Json& args);   // menu2d_view.cpp: menu, new, pick, saves
void openChronicle(Ctx& c, Screens& s);                                            // menu2d_view.cpp
void openCreate(Ctx& c, Screens& s, const std::string& backstory);                 // create2d_view.cpp (AL-136)
void openTalk(Ctx& c, Screens& s, long long target, bool trade);                   // talk2d_view.cpp
void openSheet(Ctx& c, Screens& s, long long id);                                  // sheet2d_view.cpp
void openWorkshop(Ctx& c, Screens& s);                                             // craft2d_view.cpp
void openColony(Ctx& c, Screens& s);                                               // build2d_view.cpp
void openDynasty(Ctx& c, Screens& s);                                              // dynasty2d_view.cpp (LR-145)
void openBuild(Ctx& c, Screens& s);                                                // build2d_view.cpp
bool buildActive(const Screens& s);
void buildDrawWorld(Ctx& c, Screens& s);
void buildPanel(Ctx& c, Screens& s);
bool buildMapInput(Ctx& c, Screens& s, double wx, double wy, const ui::Input& in, bool overUi);
bool buildKey(Ctx& c, Screens& s, SDL_Scancode sc);

} // namespace game2d
