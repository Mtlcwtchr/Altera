#pragma once
// Magic in society (LivelyRealms §5.9, §5.10, §9; AL-118).
//
// The magic core (sim/magic.hpp, AL-116) knows spells, mana, the gift and the
// undead. This module is how people live with it, by the same rules for
// everybody (content/magic/society.json):
//
//  - Laws. What a settlement thinks of each kind of magic is its norm: the
//    culture of whoever rules it (content/species/cultures: forbid_aspects,
//    norms {necromancy, witchcraft}), bent by the ruler's own traits and gift
//    and by the institution (a monastery is stricter). Casting is seen
//    (sim/magic witness: saw_magic / saw_dark_magic / saw_necromancy, rumours
//    of the coping paths). When what the neighbours saw weighs enough against
//    the norm, the one who rules judges a witch: tolerate, fine, shame, exile
//    or the stake, scored from data (the norm, the evidence, the judge's ties
//    and nature, how useful the mage is). One who fears the verdict may run.
//  - Guilds. A settlement with enough mages (or a town that is not against
//    magic) gets the tag `mage_guild`; a guild near the village sends healers
//    for a fee to those who ask around, and posts work for wanderers.
//  - Magic as a service. A healer's cast and a mage's work on another
//    household's field are paid (coin, else grain) by those who got them.
//  - Work. People who know an aspect use it at their ordinary work when it
//    pays better than the cost and the risk: Life on a hungry field, Water on a
//    dry one, Stone/Earth on a cracked wall, Life against a poisoned well. The
//    choice is queued as a catalog act (cast_grow, cast_rain, cast_mend,
//    cast_cleanse; the AI weighs it like any other) and the spell is cast when
//    the act is done at the place. Under a law against that kind of magic a
//    careful mage casts it silent, or not at all.
//  - The undead. Mindless undead follow their master a few steps behind (the
//    mind stays off); without one they wander to the living, frighten and
//    hurt them (the plague of §5.10).
//  - The realm (AL-85): magic quests are on (realm.json disabled_tags no
//    longer hold "magic"). Unbound undead about the village or a forbidden
//    necromancer post `haunting` / `necromancer_hunt`; a dragon that raids or
//    takes tribute draws a `dragon_slay`; a guild posts `artifact_hunt`.
//    A settlement where the undead outnumber the living is tagged `necropolis`.
//
// Deterministic (World::rng by id and salt). State in moduleState("magic_society").
// Text: chronicle.magic.trial.*, chronicle.magic.society.* (content/loc/<lang>/magic.json).

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
struct WorldObject;
struct Settlement;

// --- content (content/magic/society.json) ------------------------------------------
struct TrialTerm {
    std::string kind, key; // norm | evidence | useful | affinity | kin | trait | gifted_judge | dark
    double add = 0.0, mul = 0.0;
};
struct TrialOption {
    std::string id; // tolerate | fine | shame | exile | burn
    double base = 0.0;
    std::vector<TrialTerm> terms;
};
struct WorkSpell {
    std::string id;     // the catalog act (cast_grow ...)
    std::string object; // the object type it is done at
    std::string aspect;
    char vector = '+';
    std::string form = "area";
    int power = 1;
    std::string when;   // hungry_field | dry_field | cracked | tainted
    double below = 0.0; // the measure under which there is work (permille, share, 0..1)
    double value = 0.5; // what it is worth when the need is full
};
struct SocietyRules {
    bool loaded = false;
    // norms: a class of magic -> how much a settlement minds it 0..1
    std::map<std::string, double> levels;   // allowed, frowned, suspect, crime -> 0..1
    std::string normDefault = "allowed";
    std::map<std::string, std::string> classByTag; // aspect tag -> class (forbidden -> necromancy, dark -> witchcraft)
    std::map<std::string, double> judgeTraits;    // a judge's trait -> shift of every level
    std::map<std::string, double> institutionShift;
    double giftedJudge = -0.3;
    // witnesses and accusation
    std::map<std::string, std::string> memoryClass; // memory type -> class
    std::map<std::string, double> memoryWeight;     // memory type -> weight of one
    double accuseAt = 1.2, memoryDays = 60.0, retrialDays = 120.0, fearMult = 0.6;
    double fleeAt = 0.55; // the chance-weighted fear of the stake at which one runs
    double noise = 0.12;
    std::vector<TrialOption> options;
    double fineCoin = 6.0, exileM = 500.0;
    // guilds
    int guildMages = 3;
    double guildKnowledge = 0.5;
    std::set<std::string> guildSizes = {"town", "city"};
    double guildKm = 40.0, guildHealFee = 6.0, guildHealDaily = 0.25;
    // services
    double healFee = 2.0, workFee = 3.0;
    // work spells
    std::vector<WorkSpell> work;
    double workDaily = 0.5, manaReserve = 0.25, riskWeight = 1.0, lawWeight = 1.2, costWeight = 0.6, workMin = 0.15;
    double workPriority = 0.62;
    Tick workTtl = 144;
    // the undead
    double followM = 2.5, wanderM = 60.0, wanderSpeed = 0.6, biteM = 1.6, biteHealth = 0.02, biteFear = 0.15;
    // realm
    int questEvery = 10;
    double necropolisShare = 0.5;
    const WorkSpell* workSpell(const std::string& id) const;
};
const SocietyRules& societyRules(const Content& c);
std::vector<std::string> checkSociety(const Content& c);
std::vector<std::string> societyLocKeys(const Content& c);

// --- norms -----------------------------------------------------------------------
// The class of magic an aspect belongs to (magic, witchcraft, necromancy).
std::string magicClass(const Content& c, const std::string& aspect);
// How much the settlement minds that class 0..1 (0 allowed .. 1 crime).
double settlementNorm(const World& w, EntityId settlement, const std::string& cls);
// The culture whose norms rule a settlement ("" = none known).
std::string settlementCulture(const World& w, EntityId settlement);

// --- witch trials ------------------------------------------------------------------
// Weighs what the settlement saw of `caster` against its norms; > accuseAt brings a trial.
double accusationWeight(const World& w, const Character& caster);
// Judges now (also the debug command witch_trial {id}); returns the verdict, "" when there is nobody to judge.
std::string holdTrial(World& w, Character& accused);

// --- work and services ---------------------------------------------------------------
// The value of a work spell for `c` at `o` net of its cost, risk and the law (<= 0: not worth it).
double workSpellValue(const World& w, const Character& c, const WorkSpell& ws, const WorldObject& o);
// After a catalog act (ai_eval applyEffects): a cast_* act casts its spell at the place.
void magicAfterAct(World& w, Character& c, const InteractionDef& d, WorldObject* target, Character* targetChar,
                   bool success);
// Pays `fee` (coin, else grain) from `payer`'s household to `to`; returns what was paid.
double payForMagic(World& w, Character& payer, Character& to, double fee);

// --- the undead ----------------------------------------------------------------------
// Debug/scenario command: work_spell {id, spell, at} queues a work spell.
// Mindless undead whose mind stays off this tick (they follow or wander); thinkCharacter skips them.
bool undeadHeld(const World& w, const Character& c);
void undeadTick(World& w);

// --- daily, install --------------------------------------------------------------------
void societyDaily(World& w);
void installMagicSociety(World& w);

} // namespace lr::sim
