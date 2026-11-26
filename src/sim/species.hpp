#pragma once
// Peoples (AL-114, gdd/setting/LIVELYREALMS_SETTING_v0.2.md §3-4): the sapient
// kinds - humans, elves, dwarves, orcs, goblins, ogres, trolls, half-bloods,
// dragons - as data (content/species/<id>.json) laid over the one Character.
//
// There is no `if (race == orc)` anywhere: a kind is only numbers, tags and
// abilities. Its life (when it grows up, when it grows old) is read through
// Body::lifeYears(), so every rule that counts years in human terms (old age,
// fertility, marriage, the levy) holds for an elf of 300 or an orc of 30 as it
// does for a man of 60. Needs, learning, the traits drawn, who can have
// children with whom, how strong a body is in the realm all come from the file.
//
// Character::species stays the animal kind (content/ai/species.json, animals.cpp);
// Character::race is this one ("" = the default kind, human). Monsters are
// animals in that sense (content/creatures/*.json join species.json), dragons
// and trolls are people with a race.
//
// Magic (AL-116) reads affinity, gifted_chance and innate_spells; undead is the
// trait `undead`, and the race's needs and ageing then do not apply.

#include <map>
#include <string>
#include <vector>

#include "sim/core.hpp"

namespace lr::sim {

class World;
struct Character;
struct Content;
struct ValidationReport;

struct RaceAptitude {
    double learn = 1.0; // multiplies practice gain
    double cap = 1.0;   // multiplies how far practice alone gets (the talent)
};

struct RaceDef {
    std::string id;
    std::vector<std::string> tags;
    std::vector<std::string> blend; // two parents: the numbers are their average (half_elf = human + elf)
    double sapience = 1.0;
    std::string glyph;
    // body
    double heightM = 1.72, massKg = 70.0, nightVision = 0.0, speedMult = 1.0, reachMult = 1.0;
    int spritePx = 20;
    std::string skin; // 2D skin colour "#rrggbb" ("" = the human range)
    // LR-139: how the 2D pawn shows the kind (body.pawn): ears, head, nose, brow, eyes,
    // tusks, build, hair, beard, skin_pattern -> a word the client draws ("" = the human way)
    std::map<std::string, std::string> pawn;
    // life
    int adultYears = 14, elderYears = 60, lifespanYears = 60;
    double fertility = 1.0, pregnancyMult = 1.0;
    std::map<std::string, std::string> interbreed; // other kind -> the child's kind (absent = no children)
    // mind and body
    std::map<std::string, double> needMult;
    std::map<std::string, double> statMods; // LR-141: stat -> shift of the kind's mean (0..1 scale, may pass 1)
    std::vector<std::string> diet;
    std::map<std::string, RaceAptitude> aptitudes; // skill -> (learn, cap); "*" = every other skill
    std::map<std::string, double> affinity;        // magic aspect -> multiplier (AL-116 reads it)
    std::map<std::string, double> traitsPool;      // trait -> weight multiplier at birth/generation
    std::map<std::string, double> resist, weak;    // physical: poison, cold, fire, iron, illness...
    std::map<std::string, double> relationsBias;   // other kind -> starting affinity
    std::string cultureDefault;
    std::string settlementTag; // the tag a settlement mostly of this kind gets (dwarven, elven...)
    double giftedChance = 0.05;
    double might = 0.0;        // realm strength one grown member adds beyond the levy (a dragon)
    Json innateSpells = Json::array(); // built-in spells (dragon breath), run by the magic core
    Json hoard = Json::object();       // a hoarder's lair: {start:[lo,hi], grow_per_year, tribute_share}
    Json variants = Json::object();
    struct Regen {
        double healthPerDay = 0.0;
        std::vector<std::string> blockedBy; // moodlets that stop it (burned)
    } regen;
    struct SunSleep {
        bool on = false;
        double lightAbove = 0.6, wakeBelow = 0.3;
        std::string state = "petrified_sleep";
    } sunSleep;
};

struct CultureDef {
    std::string id;
    std::string species;
    std::vector<std::string> namesM, namesF, family; // name ids (loc name.<id>); empty = the common pool
    std::string speech, buildStyle;
    std::vector<std::string> teachAspects, forbidAspects;
    Json norms = Json::object();
    // AL-136: who lives in this culture's settlements, by settlement kind (village, town,
    // keep, ...; "default" for any other): kind of people -> share. Empty = all `species`.
    std::map<std::string, std::map<std::string, double>> peoples;
    // How the culture takes people of other kinds who come to live among them:
    // kind -> 0..1 (1 = as one of their own, 0 = a stranger; absent = `welcomeOther`).
    std::map<std::string, double> welcome;
    double welcomeOther = 0.5;
    // LR-155: the whole file (faith, values, traditions, rites, food, desc: sim/culture.hpp reads them)
    Json data = Json::object();
};

struct SpeciesCatalog {
    bool loaded = false;
    std::string defaultRace = "human";
    std::map<std::string, RaceDef> races;
    std::map<std::string, CultureDef> cultures;
    std::map<std::string, double> mapMix;                                 // settle.json map (resolved)
    std::map<std::string, std::map<std::string, double>> templateMix;     // settle.json realm_templates (resolved)
    // AL-136: the culture of the map's village and of each realm template ("" = none named)
    std::string mapCulture;
    std::map<std::string, std::string> templateCulture;
    std::vector<std::string> errors;
};

// content/species (the directory) - races (*.json), cultures/*.json, settle.json.
SpeciesCatalog loadSpecies(const std::string& dir);
SpeciesCatalog parseSpecies(const std::vector<Json>& races, const std::vector<Json>& cultures, const Json& settle);
void validateSpecies(const Content& c, ValidationReport& r);
// content/creatures/*.json (monsters: the species.json format + sapience, loot, tags),
// _items.json and _interactions.json there join the catalog's JSON before parsing.
std::vector<std::string> mergeCreatures(const std::string& dir, Json& species, Json& items, Json& interactions);

// The kind of a person ("" -> the default); a built-in human when none is loaded.
const RaceDef& raceOf(const Content& ct, const Character& c);
const RaceDef* findRace(const Content& ct, const std::string& id);
std::string raceId(const Content& ct, const Character& c);
// Animals have their own rules; the undead (trait undead, AL-116) neither eat nor age by the race.
bool raceAppliesTo(const Character& c);

// Sets the kind and what the body takes from it (Body adult/elder years).
void setRace(const Content& ct, Character& c, const std::string& race);
// Days of age for a person of this kind who is `humanYears` old in human terms.
int raceAgeDays(const RaceDef& r, double humanYears);

double raceNeedMult(const Content& ct, const Character& c, const std::string& need);
double raceLearnMult(const Content& ct, const Character& c, const std::string& skill);
double raceCapMult(const Content& ct, const Character& c, const std::string& skill);
double raceTraitWeight(const RaceDef& r, const std::string& trait);
double raceConceiveMult(const Content& ct, const Character& c);
// The child's kind, "" when these two have none together.
std::string childRace(const Content& ct, const std::string& mother, const std::string& father);
// A name from the kind's culture, "" when it uses the common pool.
std::string cultureName(const Content& ct, const std::string& race, Rng& rng, bool female, bool withFamily);
// Starting relations between kinds (relations_bias), for two people meeting the first time.
double raceBias(const Content& ct, const std::string& from, const std::string& to);
// The tag a settlement of these people gets: a kind's settlement_tag when it is
// at least `major` of them, "mixed" when two kinds are each at least `minor`.
std::vector<std::string> peoplesTags(const Content& ct, const std::map<std::string, int>& count, double major = 0.7,
                                     double minor = 0.2);
// AL-136: the mix of kinds in a settlement of `culture` of kind `settlementKind`
// (the culture's peoples[kind], else peoples.default, else all of its species).
// Empty when the culture is unknown.
std::map<std::string, double> culturePeoples(const SpeciesCatalog& s, const std::string& culture,
                                             const std::string& settlementKind);
// How `culture` takes a newcomer of kind `race` (CultureDef::welcome), 1 for its own
// species and for an unknown culture.
double cultureWelcome(const SpeciesCatalog& s, const std::string& culture, const std::string& race);
// Draws a kind from a mix (settle.json), deterministic by the rng.
std::string drawRace(const std::map<std::string, double>& mix, Rng& rng, const std::string& fallback);

// What a new birth or a generated person takes from its kind (the body's years).
// The gift (trait gifted) is rolled by the magic core (AL-116) from giftedChance.
void raceAtBirth(World& w, Character& c);

// A built-in spell (dragon breath, basilisk gaze; innate_spells of the kind or the
// creature) fired by the magic core without mana or knowledge (castStored). Each
// entry may carry "chance" (default 1). `holder` may be null (a dragon offstage).
// Returns whether something was cast; false without magic content.
bool innateStrike(World& w, Character* holder, const Json& innateSpells, EntityId target, const Vec2* at,
                  const std::string& salt);

// --- trolls in the sun ------------------------------------------------------------
bool petrified(const Character& c);
void forcePetrify(World& w, Character& c); // Light+ at night (AL-116)
void wakePetrified(World& w, Character& c); // Shadow+ by day (AL-116)
// The light on a person now: World fields "light" when there are (AL-115), else
// day by the clock (1 by day, 0 at night) and 0 under a roof.
double lightOn(const World& w, const Character& c, bool underRoof);
// Every tick for each living person (World::updateBodies): sun sleep and
// regeneration. Returns false while the body is stone (its needs stay as they are).
bool raceBodyTick(World& w, Character& c, bool underRoof);

} // namespace lr::sim
