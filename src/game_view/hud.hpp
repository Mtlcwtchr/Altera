#pragma once
// The game's interface, style G of the design canvas "Altera UI и HUD"
// (boards HudG, Wheel, Sheet, Kin, Annals): leather and paper panels with gold
// rings and a wax seal, Kurale for titles and Nunito for the rest, portraits
// of the people as the bake renders them, rings for vitals and needs.
//
// Immediate mode over ui::Ui on the game's software canvas. Panels are
// painted on the CPU once per size (the CSS of the canvas, by distance to
// the rounded box) and cached as textures. Every text comes from content/loc
// through the host; the layout and the data mapping live here.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "engine/ui/ui.hpp"
#include "game_view/snapshot.hpp"
#include "sim/core.hpp"

namespace altera::game {

// What the HUD asks of the game that owns it.
class HudHost {
public:
    virtual ~HudHost() = default;
    virtual std::string text(const std::string& key, const sim::Json& args = sim::Json::object()) const = 0;
    virtual std::string interactionName(const std::string& id) const = 0;
    virtual void command(const std::string& type, const sim::Json& args) = 0;   // a player command to the sim
    virtual void pause() = 0;
    virtual void speed(int n) = 0;
    virtual void select(EntityId id) = 0;
    virtual void wheelTarget(EntityId id) = 0;    // which relation the snapshot should carry
    // The look a person is drawn with and their tints (skin, hair, cloth).
    virtual std::string lookOf(const ActorSnap& a) const = 0;
    virtual void tintsOf(const ActorSnap& a, float skin[3], float hair[3], float cloth[3]) const = 0;
    virtual void familyColour(const std::string& family, float out[3]) const = 0;
    // Screen position over a person's head; false off screen.
    virtual bool overHead(const ActorSnap& a, float& x, float& y) const = 0;
    virtual double groundHeight(double simX, double simY) const = 0;   // for the minimap
    virtual double tickAlpha() const = 0;
    // Goals the player can suggest or order: (id, name).
    virtual std::vector<std::pair<std::string, std::string>> goals() const = 0;
};

class Hud {
public:
    bool init(SDL_Renderer* renderer, const std::filesystem::path& root, std::string* why);
    void shutdown();
    // The whole interface for this frame. `selected` may change (a portrait clicked).
    void draw(ui::Ui& ui, ui::Input& input, const Snapshot& s, EntityId selected, HudHost& host, float width,
              float height);

    std::string panel = "none";   // none | sheet | wheel | kin | annals
    void openWheel(EntityId target) {
        panel = "wheel";
        wheelTarget_ = target;
        sphere_ = 0;
        sub_ = 0;
    }
    void setTone(int t) { tone_ = t; }   // the wheel's tone chip (tour scripts)
    bool showThoughts = false;

private:
    struct Image {
        int w = 0, h = 0;
        std::vector<std::uint8_t> rgba;
    };
    enum class Skin { Leather, Paper, Ring, Button, ButtonOn, Seal, Chip, ChipOn, Disc };

    // painting
    SDL_Texture* skin(Skin kind, int w, int h, float radius, std::uint32_t colour = 0);
    void box(Skin kind, const ui::Rect& r, float radius, std::uint32_t colour = 0);
    void ring(float cx, float cy, float r);   // the gold ring round a portrait of radius r
    void arc(float cx, float cy, float r, float width, float startDeg, float sweepDeg, const ui::Colour& c);
    void disc(float cx, float cy, float r, const ui::Colour& c);
    void icon(const std::string& name, const ui::Rect& r, const ui::Colour& c);
    void portrait(const ActorSnap& a, float cx, float cy, float r);
    SDL_Texture* portraitOf(const ActorSnap& a);
    void title(float x, float y, const std::string& s, const ui::Colour& c, float px);
    void label(float x, float y, const std::string& s, const ui::Colour& c, float px = 15, bool shadow = false);
    bool clicked(const ui::Rect& r);
    bool roundButton(const ui::Rect& r, const std::string& iconName, bool on, const std::string& tip = "");
    bool textButton(const ui::Rect& r, const std::string& s, bool dark);
    void seal(float cx, float cy);
    float textW(const std::string& s, float px, bool titleFace = false) const;

    // parts of the screen
    void names(const Snapshot& s);
    void topMenu(float W);
    void minimap(const Snapshot& s, float W);
    void party(const Snapshot& s, const ActorSnap* sel);
    void selectedCorner(const Snapshot& s, const ActorSnap& a, float H);
    void quickBar(const Snapshot& s, float W, float H);
    void clockPill(const Snapshot& s, float W, float H);
    void toasts(const Snapshot& s, float W);
    void thoughts(const Snapshot& s, float W);
    void sheet(const Snapshot& s, const ActorSnap& a, float W, float H);
    void wheel(const Snapshot& s, float W, float H);
    void kin(const Snapshot& s, const ActorSnap* a, float W, float H);
    void annals(const Snapshot& s, float W, float H);
    void dimmer(float W, float H, float a);

    struct Row {
        std::string label;
        double value = -1;   // 0..1, or -1: no bar
        std::string note;
        bool alarm = false;
    };
    std::vector<Row> tierRows(const Snapshot& s, const ActorSnap& a, int tier) const;
    double tierValue(const Snapshot& s, const ActorSnap& a, int tier) const;
    const ActorSnap* actor(const Snapshot& s, EntityId id) const;
    std::string dayPart(Tick t) const;

    ui::Ui* ui_ = nullptr;
    ui::Input* in_ = nullptr;
    HudHost* host_ = nullptr;
    SDL_Renderer* sdl_ = nullptr;
    std::filesystem::path root_;
    Image leather_, paper_;
    SDL_Texture* icons_ = nullptr;
    int iconCell_ = 48;
    std::map<std::string, int> iconIndex_;
    std::map<std::string, SDL_Texture*> skins_;
    struct PortraitBase {
        Image picture, mask;
    };
    std::map<std::string, PortraitBase> portraitBases_;
    std::map<EntityId, SDL_Texture*> portraits_;
    SDL_Texture* minimap_ = nullptr;
    sim::Json wheelSpec_;
    EntityId selected_ = sim::kNone, wheelTarget_ = sim::kNone;
    int goalPick_ = -1;
    int pin_ = 0, hover_ = -1, sheetTab_ = 0, sphere_ = 0, sub_ = 0, tone_ = 1, annalsTab_ = 0, annalsFilter_ = 0;
    EntityId kinPick_ = sim::kNone;
    std::size_t seenChronicle_ = 0;
    struct Toast {
        std::string text;
        double born = 0;
    };
    std::vector<Toast> toasts_;
    double clock_ = 0;
    SnapClock::time_point started_ = SnapClock::now();
    std::uint64_t idSeed_ = 0;
};

} // namespace altera::game
