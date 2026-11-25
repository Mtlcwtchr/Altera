// A captive's fate (COMBAT_DESIGN §7, §17; content/combat/captivity.json):
// held by the captor after a fight (aftermath "bind") or an arrest
// (restrain), tries to escape, and is released, ransomed by the family or
// tried; a trial may end in an execution - the only real death of a player,
// after which the dynasty's heir takes over (succession).
#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>

#include "sim/core.hpp"
#include "sim/rules.hpp"

namespace lr::sim {

class World;
struct Character;
struct Content;

struct FateTerm {
    std::string kind; // trait | rel | kin | right | crimes | offended | can_pay | captive_player | captor_player
    std::string key;
    bool flag = true;
    double add = 0.0, mul = 0.0, cap = 0.0;
    bool capped = false;
};

struct FateOption {
    std::string id;
    double base = 0.0;
    std::vector<FateTerm> terms;
    std::vector<Effect> eff;
    std::string memCaptor, memCaptive, memPayer;
    double valCaptor = 0.0, valCaptive = 0.0, valPayer = 0.0;
    double exileM = 0.0;
};

struct CaptivityRules {
    bool loaded = false;
    double checkHours = 2.0;
    std::array<double, 2> decideHours{8.0, 30.0};
    double followM = 2.0, noise = 0.15;
    int maxKeeps = 2;
    // escape
    double escBase = 0.03, escPerSkill = 0.1, escAwayM = 40.0, escAwayMult = 3.0, escNightMult = 1.5;
    double escWoundedBelow = 0.4, escWoundedMult = 0.4, escMax = 0.4, escRunM = 60.0;
    std::string escSkill = "hunting";
    // ransom
    int ransomValue = 6;
    std::map<std::string, int> ransomGoods;
    std::vector<FateOption> fates, verdicts;
    std::string execCause = "execution", execWitnessMem = "saw_execution", execExecutorMem = "executed";
    double execWitnessM = 80.0;
    std::string memEscaped, memCaptorEscaped, memFreed, memExiled, memPunished;
    std::string chronicle = "chronicle.captivity", succession = "chronicle.succession";
};

CaptivityRules loadCaptivity(const Json& j, const Content& c, std::vector<std::string>& err);

// `captor` takes `captive`: the captive follows, the mind stops, the fate
// waits for the captor's decision.
void takeCaptive(World& w, Character& captor, Character& captive);
// Lets the captive go (no decision, no memory beyond `why` in the chronicle).
void freeCaptive(World& w, Character& captive, const std::string& why);

// Scores of the fates / verdicts for this decider (tests, trace); no noise.
std::vector<double> fateScores(const World& w, const std::vector<FateOption>& opts, const Character& decider,
                               const Character& captive);

// Applies a fate now ("release", "ransom", "trial", "keep"); returns false
// when the fate does not apply (ransom nobody can pay). Trial picks the
// judge and the verdict and carries it out.
bool decideFate(World& w, Character& captor, Character& captive, const std::string& fate);
// The verdict of `judge` over `captive` carried out ("execute", "punish", "exile", "acquit").
void carryOutVerdict(World& w, Character& judge, Character& captive, const std::string& verdict);

// Every tick: captives follow, try to escape, captors decide; a dead player
// character hands the player to the dynasty's heir.
void captivityTick(World& w);

// The heir a dead (or executed) character's player gets: the named heir,
// the eldest grown child, the spouse, the eldest grown kin, any kin.
EntityId heirOf(const World& w, const Character& dead);

} // namespace lr::sim
