#pragma once
// LR-152: the combat state of the 2D window. The sim plays a fight round by round
// (src/sim/combat) and the state route hands over its events (/api/state "fights",
// sim/combat_show.hpp); this plays them back over the map: the fighters face off,
// each exchange is a wind-up, a swing along its direction (overhead, left, right,
// under, thrust), the defender's answer (block, parry with sparks, dodge, a hit with
// blood and a red flash), ripostes, clinches, winded breaths, falls. Every number,
// colour and text key is in content/sprites_2d/combat2d.json (hot reload).
// Drawing goes through the window's Ctx: pixel squares in art pixels, no textures.
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "native/ctx2d.hpp"

namespace game2d {

struct FightPose {
    double x = 0, y = 0;
    char face = 'S';
    double lie = 0;        // 0 standing .. 1 lying (a fall)
    double kneel = 0;      // yield
    double flash = 0;      // 0..1 of the hit flash
    ui::Colour flashColour{1, 0.2f, 0.1f, 1};
};

class Fight2d {
public:
    void configure(const Json& data);   // content/sprites_2d/combat2d.json
    // A fight from the state route; `me` the led person. Returns the ms the world should
    // wait (the led person's fight, hold_world_for_me), else 0.
    double add(const Json& fight, double nowMs, long long me, const std::string& nameA, const std::string& nameB);
    void tick(double nowMs, double dt);
    // The fighter's pose while its fight plays (false: not in a fight now).
    bool pose(long long id, double nowMs, FightPose& out) const;
    bool active(double nowMs) const;
    bool mineActive(double nowMs) const;
    // Over the pawns: weapons, sparks, blood, the floating words, the bars above the fighters.
    void drawWorld(Ctx& c, double nowMs, float sc);
    // The banner of the led person's fight (top centre of the map).
    void drawBanner(Ctx& c, double nowMs);
    void clear();

private:
    struct Exchange {
        double t0 = 0, t1 = 0;      // ms from the show's start
        long long att = -1, def = -1;
        std::string kind = "attack"; // attack | clinch | shoot | other
        std::string dir, zone, out, weapon, sev, move;
        bool riposte = false, clinchOk = false, fired = false;
        double stamina = -1;
        std::vector<Json> after;    // exhausted, wavering, state, weapon_break, wound, ... (at the hit frame)
    };
    struct Fighter {
        double x = 0, y = 0;   // stance spot
        double fx = 1, fy = 0; // toward the other
        std::string weapon;
        std::string state = "fight";
        double stateAt = -1;   // ms of the show when the state changed
        double stamina = 100;  // last seen
        int wounds = 0;
        double flashAt = -1e9;
        ui::Colour flashColour{1, 0.2f, 0.1f, 1};
    };
    struct Show {
        long long id = 0, a = -1, b = -1;
        double start = 0, length = 0;
        double roundMs = 1500;
        int rounds = 0;
        bool mine = false;
        std::string nameA, nameB;
        std::vector<Exchange> ex;
        std::map<long long, Fighter> f;
        std::uint32_t seed = 1;
    };
    struct Particle {
        double x, y, z, vx, vy, vz, born, life, size;
        ui::Colour c;
        bool glow;
        double stainUntil;
    };
    struct Floater {
        double x, y, born;
        std::string text;
        ui::Colour c;
    };

    void fire(Show& s, Exchange& e, double nowMs);
    void burst(Show& s, const std::string& effect, double x, double y, double nowMs, double mult = 1.0);
    void say(const std::string& effect, double x, double y, double nowMs, const std::string& text = "");
    const Exchange* current(const Show& s, double t) const;
    void drawWeapon(Ctx& c, const Show& s, const Exchange* e, long long who, double t, float sc);
    void line(Ctx& c, double x0, double y0, double x1, double y1, float w, const ui::Colour& col);
    double rnd(std::uint32_t& s) const;
    double phase(const char* k, double d) const;

    Json D_ = Json::object();
    Ctx* textCtx_ = nullptr;
    std::deque<Show> shows_;
    std::vector<Particle> parts_;
    std::vector<Floater> floats_;
    std::uint32_t rng_ = 12345;
};

} // namespace game2d
