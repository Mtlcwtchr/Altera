#pragma once
// Village life (AL-39, content/ai/village_life.json).
//
// Once a day the village lives: unmarried grown-ups court the one they fancy
// (chat, compliments, flirting), propose and, betrothed, marry - the bride
// (by default) moves into the groom's household and takes its name. Spouses
// share a bed, a woman may conceive, carries the child for pregnancy_days and
// gives birth: the child joins the household and the father's dynasty (the
// mother's when the father is not her husband) and so stands in the line of
// heirs (heirOf in captivity.hpp). Old enmity and bad rumours turn into
// quarrels, people fall ill (and sometimes die of it or of old age), and
// strangers come to the village: one is taken in by a household, a family
// starts its own.
//
// Nothing is a scripted event: every step is an ordinary act of the catalog
// the person does themselves through the task queue, or a body's own course
// (pregnancy, illness, age). The player's person is never made to do any of
// it: no task is queued for them and nobody proposes to them or leads them to
// the altar - the player does those acts from the wheel, and the after-act
// hook gives them the same outcome.
//
// Deterministic (agentRng from the seed). Every event goes to the chronicle
// under chronicle.village.* (content/loc/<lang>/village_life.json).

#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sim/core.hpp"

namespace lr::sim {

class World;
struct Character;
struct Content;
struct InteractionDef;

struct IllnessKind {
    std::string id, moodlet;
    double weight = 1.0;
    std::array<int, 2> days = {4, 10};
    double healthPerDay = 0.05;
};

struct VillageLifeRules {
    bool loaded = false;
    // courtship
    int minAgeYears = 16, maxAgeGapYears = 15;
    bool sameSex = false;
    double courtDaily = 0.6;
    std::vector<std::string> courtActs = {"chat", "compliment", "flirt"};
    double flirtFromFamiliarity = 0.3;
    std::array<double, 2> spark = {0.0, 0.35};
    double interestMin = 0.25;
    std::string proposeAct = "propose_marriage";
    // propose when the suitor's interest and the other's feelings (rel back) are high enough
    double proposeInterest = 0.45, acceptAffinity = 0.3, acceptAttraction = 0.12;
    int refusedCooldownDays = 12;
    std::string wedAct = "marry";
    int weddingAfterDays = 7;
    std::string residence = "patrilocal"; // patrilocal | matrilocal
    bool takeFamilyName = true;
    double courtPriority = 0.55;
    Tick courtTtl = 144;
    // marriage
    std::string intimacyAct = "intimacy";
    double intimacyDaily = 0.3, marriagePriority = 0.5;
    Tick marriageTtl = 144;
    // conception and birth
    std::set<std::string> conceptionActs = {"intimacy"};
    double conceiveChance = 0.2;
    std::array<int, 2> fertileF = {16, 45}, fertileM = {16, 70};
    int fadeFromYears = 35;
    int pregnancyDays = 270, birthSpacingDays = 60;
    double startPregnantShare = 0.35, twins = 0.015, stillbirth = 0.04, motherDeath = 0.015;
    double traitInherit = 0.5;
    int traitsMax = 3;
    double attrNoise = 0.08;
    // quarrels
    struct QuarrelAct {
        std::string act;
        double from = 0.0;
    };
    std::vector<QuarrelAct> quarrelActs = {{"argue", 0.15}, {"insult", 0.6}};
    double quarrelDaily = 0.1, wEnmity = 1.0, wDistrust = 0.5, wDislike = 0.6;
    int quarrelCooldownDays = 10;
    double quarrelPriority = 0.5;
    Tick quarrelTtl = 108;
    // illness
    double illDaily = 0.0015, contagion = 0.02;
    std::map<std::string, double> illMult = {{"winter", 2.0}, {"autumn", 1.3}, {"child", 1.5},
                                             {"elder", 2.0}, {"hungry", 1.6}, {"cold", 1.6}};
    int immuneDays = 40;
    std::vector<IllnessKind> illnesses;
    // old age
    int oldFromYears = 60;
    double oldDailyAtFrom = 0.00005, oldDoubleYears = 7.0;
    // strangers
    double strangerDaily = 0.02, strangerNoSingles = 0.05; // the latter while singles of one sex have nobody to marry
    int strangerMaxPeople = 60;
    double strangerFoodDays = 30.0; // AL-82: a village with less food per head than this draws fewer wanderers (none at a third of it)
    double strangerCouple = 0.3, strangerChild = 0.4;
    std::array<int, 2> strangerAge = {17, 40}, loneAge = {17, 30};
    std::string takenInRole = "servant";
    std::map<std::string, int> householdKit = {{"grain", 25}, {"firewood", 6}};
    std::array<double, 2> strangerSkill = {0.2, 0.55};
    double arriveM = 140.0;
};

// village_life.json next to the content (cached per content dir); defaults without one.
const VillageLifeRules& villageLifeRules(const Content& c);
// Problems of village_life.json against the content (unknown acts, moodlets); empty = fine.
std::vector<std::string> checkVillageLife(const Content& c);
// Loc keys the module writes (chronicle.village.*, memory.*.text).
std::vector<std::string> villageLifeLocKeys();

// The daily pass (installAi's think hook, once a day).
void villageDaily(World& w);
// After every act (applyEffects): courtship, betrothal, wedding, conception, quarrels.
void villageAfterAct(World& w, Character& actor, const InteractionDef& d, Character* targetChar, bool success);
// Installs the "village_birth" event handler.
void installVillageLife(World& w);
// World generation: some married women of the start are already with child
// (so births come in the first months, not only after a year).
void seedVillageLife(World& w, std::uint64_t seed);

// Strangers come to `settlement` now (one taken in, or a family of their own); false: nowhere to go.
bool strangersArrive(World& w, EntityId settlement, bool family);
// The same, but the strangers are people who already live offstage (World::offstage(),
// AL-92): refugees from a hungry settlement walk onto the map as themselves. One
// person is taken in by a household; more found their own (the first man heads it,
// his wife and children with him). False: nowhere to go (nobody moved).
bool refugeesArrive(World& w, EntityId settlement, const std::vector<EntityId>& who);
// LR-121: one newcomer comes to `settlement` with a household and a house of their own
// (no fields yet: a store by the village); the new person's id, kNone when there is no such place.
EntityId newcomerArrives(World& w, EntityId settlement, char sex, int ageYears);
// The chance that a woman conceives at one act (fades after age_fade_from_years).
double conceiveChanceOf(const Character& woman, const VillageLifeRules& r);

// Queries
bool isPregnant(const World& w, const Character& c);
bool kinOrHousehold(const World& w, const Character& a, const Character& b);
// The one `c` courts now (kNone: nobody), as the daily pass picks them.
EntityId courtshipTarget(const World& w, const Character& c);

} // namespace lr::sim
