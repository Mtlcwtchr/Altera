// Combat core (src/sim/combat): data validator, determinism of the event log,
// armour formula, a few balance anchors of the prototype report.

#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "framework.hpp"
#include "sim/combat/combat.hpp"
#include "sim/combat/combat_anim.hpp"

using namespace altera::combat;

namespace {

const std::string kDir = std::string(ALTERA_SIM_SOURCE_DIR) + "/content/combat";

const CombatData& data() {
    static const CombatData d = CombatData::load(kDir);
    return d;
}

std::string duelLog(std::uint64_t seed, const FighterSpec& a, const FighterSpec& b) {
    std::string log;
    EventStream ev;
    ev.subscribe([&](const std::vector<Event>& batch) {
        for (const auto& e : batch) log += eventLine(data(), e) + "\n";
    });
    Engine e(data(), seed, &ev);
    Fighter fa = e.make(a, 1), fb = e.make(b, 2);
    e.fight(fa, fb);
    return log;
}

double winShare(const FighterSpec& a, const FighterSpec& b, int n, bool clinch = true) {
    Engine e(data(), 7);
    int w = 0;
    for (int i = 0; i < n; ++i) {
        Fighter fa = e.make(a, 1), fb = e.make(b, 2);
        e.fight(fa, fb, true, clinch);
        w += fa.fighting() && !fb.fighting();
    }
    return double(w) / n;
}

// Loads the real files, lets the test break one value, expects a DataError naming it.
bool rejects(const std::function<void(std::map<std::string, OJson>&)>& breakIt, const std::string& needle) {
    auto files = CombatData::readFiles(kDir);
    breakIt(files);
    try {
        CombatData::fromJson(files);
    } catch (const DataError& e) {
        if (std::string(e.what()).find(needle) != std::string::npos) return true;
        std::cerr << "    error text lacks '" << needle << "': " << e.what() << "\n";
        return false;
    }
    std::cerr << "    no error for '" << needle << "'\n";
    return false;
}

} // namespace

TEST(altera_combat_data_loads) {
    const CombatData& d = data();
    CHECK_EQ(d.zones.size(), std::size_t(6));
    CHECK(d.hasWeapon("sword") && d.hasWeapon("fists"));
    CHECK_EQ(d.moves.size(), std::size_t(4));
    CHECK_EQ(d.ranged.size(), std::size_t(6));
    CHECK(d.armors[d.armor("plate")].hasGap);
    CHECK(!d.armors[d.armor("gambeson")].hasGap);
    CHECK_EQ(d.core.unarmed, d.weapon("fists"));
}

TEST(altera_combat_validator_rejects_unknown_keys) {
    CHECK(rejects([](auto& f) { f["armor"]["mail"]["cover"].push_back("tail"); }, "unknown zone 'tail'"));
    CHECK(rejects([](auto& f) { f["weapons"]["sword"]["dir_type"]["spin"] = "slash"; }, "unknown direction 'spin'"));
    CHECK(rejects([](auto& f) { f["weapons"]["axe"]["types"]["fire"] = OJson::array({1, 2}); }, "unknown damage type 'fire'"));
    CHECK(rejects([](auto& f) { f["clinch_moves"]["share"]["halberd"] = 0.1; }, "unknown weapon 'halberd'"));
    CHECK(rejects([](auto& f) { f["clinch_moves"]["moves"]["headbutt"] = f["clinch_moves"]["moves"]["disarm"]; },
                  "unknown clinch move"));
    CHECK(rejects([](auto& f) { f["wear"]["armor"]["durability"]["brigandine"] = 300; }, "unknown armor 'brigandine'"));
    CHECK(rejects([](auto& f) { f["combat_config"]["direction_counter"]["left"][0] = "guard_up"; }, "unknown guard 'guard_up'"));
    CHECK(rejects([](auto& f) { f["combat_config"]["hit"].erase("base"); }, "missing 'base'"));
    CHECK(rejects([](auto& f) { f["weapons"]["mace"]["sped"] = 1.0; }, "unknown key 'sped'"));
    CHECK(rejects([](auto& f) { f["wounds"]["severity"][1]["bleed"] = "gush"; }, "unknown bleed rate 'gush'"));
    CHECK(rejects([](auto& f) { f["ranged_weapons"]["weapons"]["sling"]["dtype"] = "acid"; }, "unknown damage type 'acid'"));
    CHECK(rejects([](auto& f) { f["combat_config"]["core"]["unarmed_weapon"] = "claws"; }, "unknown weapon 'claws'"));
    CHECK(rejects([](auto& f) { f.erase("shields"); }, "missing file shields.json"));
    CHECK(rejects([](auto& f) { f["weapons"]["dagger"]["reach"] = "short"; }, "must be a number"));
}

TEST(altera_combat_same_seed_same_log) {
    const FighterSpec a{50, "dagger", {}}, b{50, "sword", {"mail", "helmet"}};
    const std::string l1 = duelLog(12345, a, b), l2 = duelLog(12345, a, b), l3 = duelLog(12346, a, b);
    CHECK(!l1.empty());
    CHECK(l1 == l2);
    CHECK(l1 != l3);
    CHECK(l1.find("\"k\":\"attack\"") != std::string::npos);
    CHECK(l1.find("\"k\":\"end\"") != std::string::npos);
}

TEST(altera_combat_ranged_and_battle_are_deterministic) {
    auto run = [](std::uint64_t seed) {
        std::string log;
        EventStream ev;
        ev.subscribe([&](const std::vector<Event>& b) {
            for (const auto& e : b) log += eventLine(data(), e) + "\n";
        });
        Engine e(data(), seed, &ev);
        Fighter s = e.make({50, "dagger", {}}, 1), t = e.make({50, "sword", {"gambeson"}}, 2);
        e.rangedDuel(s, t, data().rangedWeapon("longbow"), 30);
        std::vector<Fighter> A, B;
        for (int i = 0; i < 3; ++i) A.push_back(e.make({45, "spear", {"gambeson"}}, 10 + i, 0));
        for (int i = 0; i < 3; ++i) B.push_back(e.make({45, "sword", {"gambeson"}}, 20 + i, 1));
        e.battle(A, B);
        return log;
    };
    const std::string a = run(99), b = run(99);
    CHECK(a == b);
    CHECK(a.find("projectile_") != std::string::npos);
}

TEST(altera_combat_armor_formula) {
    const CombatData& d = data();
    Engine e(d, 1);
    const Fighter plate = e.make({50, "sword", {"plate"}}), none = e.make({50, "sword", {}});
    const int torso = d.zone("torso"), slash = d.dtypeIx.at("slash"), blunt = d.dtypeIx.at("blunt");
    CHECK(std::abs(e.absorb(none, torso, slash)) < 1e-12);
    // plate: pen 1 -> through = k_impact * trans = 0.1 * 0.8, absorbed 0.92
    CHECK(std::abs(e.absorb(plate, torso, slash) - 0.92) < 1e-9);
    // blunt: through = trans 0.75
    CHECK(std::abs(e.absorb(plate, torso, blunt) - 0.25) < 1e-9);
    // gap_stab ignores jointed layers
    CHECK(std::abs(e.absorb(plate, torso, slash, nullptr, 0.0, true)) < 1e-12);
}

TEST(altera_combat_balance_anchors) {
    // Loose anchors (n=300, wide margin): the tight check is altera_combat_tables --golden.
    const double even = winShare({50, "sword", {}}, {50, "sword", {}}, 300);
    CHECK(even > 0.40 && even < 0.62);
    const double master = winShare({90, "sword", {}}, {10, "sword", {}}, 300);
    CHECK(master > 0.95);
    const double daggerPlateClinch = winShare({70, "dagger", {}}, {50, "sword", {"plate"}}, 300);
    const double daggerPlateNo = winShare({70, "dagger", {}}, {50, "sword", {"plate"}}, 300, false);
    CHECK(daggerPlateClinch > daggerPlateNo + 0.15);
}

TEST(altera_combat_events_bind_to_ual_clips) {
    const nlohmann::json manifest =
        nlohmann::json::parse(std::ifstream(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/animations/humanoid_ual/manifest.json"));
    AnimBinder bind(data(), manifest);
    std::vector<AnimCue> cues;
    EventStream ev;
    ev.subscribe([&](const std::vector<Event>& b) {
        auto c = bind.bind(b);
        cues.insert(cues.end(), c.begin(), c.end());
    });
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        Engine e(data(), seed, &ev);
        Fighter a = e.make({60, seed % 2 ? "dagger" : "mace", {}}, 1), b = e.make({50, "sword", {"mail", "helmet"}}, 2);
        e.fight(a, b);
        Fighter s = e.make({50, "dagger", {}}, 3), t = e.make({50, "sword", {}}, 4);
        e.rangedDuel(s, t, data().rangedWeapon("crossbow"), 30);
    }
    int attacks = 0, withClip = 0;
    double last = -1;
    for (const auto& c : cues) {
        if (c.source == EventKind::Attack) {
            ++attacks;
            CHECK(!c.tier.empty());
            CHECK(c.speedMult > 0.3 && c.speedMult < 3.0);
        }
        withClip += !c.clip.empty();
        CHECK(c.startMs >= 0);
        (void)last;
    }
    CHECK(attacks > 100);
    CHECK(withClip * 10 >= static_cast<int>(cues.size()) * 9); // >= 90% of cues have a clip on the UAL rig
    for (const auto& k : bind.unboundEvents()) // only the gaps animations.json names as null
        CHECK(k == "get_up" || k == "shoot" || k == "projectile_miss");
    for (const auto& k : bind.missingClips()) std::cerr << "    no UAL clip for '" << k << "'\n";
}
