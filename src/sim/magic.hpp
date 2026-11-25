#pragma once
// Magic of aspects (LivelyRealms, gdd/setting/LIVELYREALMS_SETTING_v0.2.md §4.4, §5; AL-116).
//
// A spell is not picked from a list: a caster puts it together from 1..3
// aspects (fire, water, life, death ... content/magic/aspects.json), each with a
// vector ('+' empower/create, '-' weaken/take, '~' shift/transfer), a form
// (touch, bolt, area, aura, rune, summon ... forms.json) and modifier steps
// (power, radius, silent ... modifiers.json). The cast is resolved like a
// crafting step: competence (skill, knowledge of the aspects, the gift, a
// focus) against difficulty gives a quality and a tier (catastrophe ... exemplary).
//
// The spell acts on the world through the same fields the rest of the sim
// reads (heat, moisture, fertility, integrity, light, aether, taint): the
// field store is AL-115's (sim/world_fields), reached through the sink hook
// below so that neither side needs the other to compile. On creatures it acts
// directly (aspects.json `body`): health, wounds, fear, sleep, mana, raising
// the dead. Keys this module does not know go to the body hooks (the troll's
// sun-sleep of AL-114, runes of AL-117) and are otherwise ignored.
//
// Mana is a separate scale (user 25.11). The gift (trait `gifted`, rare) is a
// talent: faster learning and a higher ceiling; everybody can learn.
//
// The undead are a state of a body, not a kind: the trait `undead` (needs stand
// still through the trait's need_mult), kept by the master's mana.
//
// Troubles (a close one died, a maiming, a kin's illness) open paths of
// stages (coping.json): most people grieve and let go; some ask around, find
// someone to speak with the dead, and a few go on to forbidden study, raise
// their dead and live with what follows (keep, exposure, fall, plague,
// redemption, lich). Nothing is scripted: weights from traits, bond, honour
// and knowledge decide, the world's ordinary rules do the rest.
//
// The state lives in World::moduleState("magic") (saved with the world).
// Deterministic (World::rng). Chronicle codes chronicle.magic.* (content/loc/<lang>/magic.json).

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sim/core.hpp"

namespace lr::sim {

class World;
struct Character;
struct Content;

// --- content ------------------------------------------------------------
struct AspectDef {
    std::string id, group;
    std::vector<std::string> tags, opposes, feeds;
    double baseCost = 1.0, difficulty = 0.3;
    bool enabled = true;
    Json world = Json::object(); // vector -> {field: expr}
    Json body = Json::object();  // vector -> {key: expr}
    bool hasTag(const std::string& t) const;
};

struct SpellFormDef {
    std::string id, reach = "touch"; // touch | ranged | area | self | rune | summon
    double costMult = 1.0, difficulty = 0.0, rangeM = 2.0, radiusM = 0.0;
    int time = 2;
};

struct ModifierDef {
    std::string id;
    double costPerStep = 0.5, diffPerStep = 0.05;
    int max = 3;
};

struct CastTier {
    std::string id;
    double below = 1.0, power = 1.0;
    bool backfire = false;
};

struct CopingNext {
    std::string to;
    double base = 1.0, knows = 1.0, unknown = 1.0, wBond = 0.0, wHonor = 0.0, wDeath = 0.0;
    std::map<std::string, double> traits;
};
struct CopingStage {
    std::string id, act;
    int minDays = 1;
    double daily = 0.3;
    bool quiet = false; // no chronicle line for this stage
    std::vector<CopingNext> next;
};
struct CopingTrouble {
    std::string id, start;
    double bondMin = 0.45;
    std::map<std::string, CopingStage> stages;
};

struct MagicRules {
    bool loaded = false;
    std::map<std::string, AspectDef> aspects;
    std::map<std::string, SpellFormDef> forms;
    std::map<std::string, ModifierDef> modifiers;
    std::map<std::string, CopingTrouble> troubles;
    std::string skill = "magic";
    // mana
    double manaBase = 0.6, manaPerSkill = 1.2, manaGifted = 0.8, manaRegen = 0.5, costScale = 0.25;
    // gift
    std::string giftTrait = "gifted";
    double giftChance = 0.05, giftLearn = 3.0, ceilGifted = 1.0, ceilPlain = 0.6, giftCompetence = 0.1;
    // knowledge
    double useMin = 0.2, solo = 0.5, full = 0.8, novicePenalty = 0.15;
    std::set<std::string> noviceForms = {"touch", "aura"};
    double gainCast = 0.012, gainTeach = 0.03, gainAwaken = 0.22, awakenDaily = 0.004, breakthrough = 0.03,
           breakthroughFrom = 0.5, skillGainCast = 0.006;
    // cast
    double k = 1.3, noise = 0.12, extraAspectDiff = 0.35, opposesCost = 1.6, opposesDiff = 0.15, aetherPerCost = 0.3,
           witnessM = 22.0;
    std::vector<CastTier> tiers;
    // death and the undead
    std::string deathAspect = "death", markTrait = "death_touched";
    int markAfter = 3;
    std::string undeadTrait = "undead", mindlessTrait = "mindless";
    double raiseMinPower = 0.8, wholeMindPower = 2.0, undeadHealth = 0.55, undeadDecay = 0.04, upkeepMana = 0.12,
           undeadWitnessM = 18.0, undeadFear = 0.35;
    int maxDeadDays = 60, unboundAfterDays = 2;
    // healing as a service
    std::string healAspect = "life";
    double healMinK = 0.5, healBelow = 0.75, healDaily = 0.6;
    // lich
    double lichDeathK = 0.9, lichDaily = 0.02;
    int lichRaisedMin = 4;

    const AspectDef* aspect(const std::string& id) const;
    const SpellFormDef* form(const std::string& id) const;
};

// content/magic/*.json next to the content dir (cached per dir); not loaded without them.
const MagicRules& magicRules(const Content& c);
// Problems of the magic data (unknown aspects in opposes/feeds, unknown stages); empty = fine.
std::vector<std::string> checkMagic(const Content& c);
// Loc keys the module writes (chronicle.magic.*, magic.reason.*, aspect names).
std::vector<std::string> magicLocKeys(const Content& c);

// --- a spell ------------------------------------------------------------
struct SpellPart {
    std::string aspect;
    char vector = '+'; // + - ~
};
struct Spell {
    std::vector<SpellPart> parts;
    std::string form = "touch";
    std::map<std::string, int> mods; // modifier -> steps
    std::string name;                // a name the caster gave ("" = none)
    int mod(const std::string& m, int def = 0) const;
    std::string key() const;         // the same spell = the same key, whatever its name
};
// {"aspects": [["fire","+"],["air","+"]], "form": "wave", "mods": {"power": 2}, "name": "..."}
Json spellToJson(const Spell& s);
Spell spellFromJson(const Json& j);

// --- a person's magic (saved in moduleState("magic").people[id]) ---------
struct BookSpell {
    Spell spell;
    double mastery = 0.0;
    int casts = 0;
};
struct MagicSelf {
    bool rolled = false; // the gift was rolled
    double mana = 0.0;
    Tick manaAt = 0;
    std::map<std::string, double> aspects; // knowledge 0..1
    bool aware = false;                    // knows that magic exists
    std::vector<BookSpell> book;
    int deathUses = 0, raised = 0;
};
MagicSelf magicOf(const World& w, const Character& c);
void setMagicOf(World& w, const Character& c, const MagicSelf& m);

bool isGifted(const World& w, const Character& c);
bool isUndead(const Character& c);
double manaMax(const World& w, const Character& c);
double manaNow(const World& w, const Character& c);
double aspectKnowledge(const World& w, const Character& c, const std::string& aspect);
// Knowledge grows by `amount` times the talent (gift, race affinity), up to
// the ceiling; returns the gain. `how` goes to the chronicle on a first opening.
double learnAspect(World& w, Character& c, const std::string& aspect, double amount, const std::string& how);
bool teachAspect(World& w, Character& teacher, Character& pupil, const std::string& aspect);
// Into the book (same key = the same entry, renamed); returns the book index.
int saveSpell(World& w, Character& c, const Spell& s);

// --- casting --------------------------------------------------------------
struct CastTarget {
    EntityId who = kNone; // a creature (alive or a corpse)
    bool hasPos = false;
    Vec2 at;
};
struct SpellPreview {
    bool ok = false;
    std::string reason; // magic.reason.<code>
    double cost = 0.0, difficulty = 0.0, competence = 0.0, manaNow = 0.0;
    std::map<std::string, double> tierChance; // tier -> share
};
struct CastOutcome {
    bool cast = false;
    std::string reason;
    std::string tier;
    double cost = 0.0, quality = 0.0, power = 0.0;
    std::vector<EntityId> hit;
    EntityId raised = kNone;
    bool backfire = false, breakthrough = false;
};
SpellPreview previewSpell(const World& w, const Character& caster, const Spell& s);
CastOutcome castSpell(World& w, Character& caster, const Spell& s, const CastTarget& t);
// A spell fired from a carrier (a rune, a scroll, a dragon's breath): no mana,
// no knowledge, fixed power (AL-117, AL-114). `holder` may be null.
CastOutcome castStored(World& w, Character* holder, const Spell& s, const CastTarget& t, double power);
// What a potion does on the one who drinks it (AL-117 §7): the aspects act as a
// spell on self, without mana or knowledge.
struct AspectDose {
    std::string aspect;
    char vector = '+';
    double power = 1.0;
};
void applyAspectsTo(World& w, Character& target, const std::vector<AspectDose>& doses, EntityId source);

// Raising a corpse (Death~): returns false when the body is too far gone or the power is short.
bool raiseUndead(World& w, Character& necromancer, Character& corpse, double power);
// The undead's master, kNone when unbound or not undead.
EntityId undeadMaster(const World& w, const Character& c);

// --- hooks to other modules ---------------------------------------------
// Fields of the world cell (AL-115): write `channels` ({field: expr}) at `at` within `radiusM` with `power`.
using MagicWorldSink = std::function<void(World&, Vec2 at, double radiusM, const Json& channels, double power)>;
void setMagicWorldSink(MagicWorldSink sink);
// Read a field (light, aether ...) at a point; unset = 0.
using MagicFieldReader = std::function<double(const World&, const std::string& field, Vec2 at)>;
void setMagicFieldReader(MagicFieldReader reader);
double magicFieldAt(const World& w, const std::string& field, Vec2 at);
// Talent by kind (AL-114 race affinity): a multiplier of learning one aspect; unset = 1.
using MagicAffinity = std::function<double(const World&, const Character&, const std::string& aspect)>;
void setMagicAffinity(MagicAffinity f);
// The gift's chance at the first look at a person (AL-114 gifted_chance); unset = magic_config.
using MagicGiftChance = std::function<double(const World&, const Character&)>;
void setMagicGiftChance(MagicGiftChance f);
// Body keys this module does not know (troll_sun, troll_wake, bind ...): true = handled.
using MagicBodyHook = std::function<bool(World&, Character& target, const std::string& key, double amount, EntityId source)>;
void addMagicBodyHook(MagicBodyHook h);
// A service done for another (a healer's round): AL-118 charges for it; unset = free.
using MagicServiceHook = std::function<void(World&, Character& provider, Character& client, const std::string& what)>;
void setMagicServiceHook(MagicServiceHook f);

// --- the daily pass --------------------------------------------------------
// Gift rolls for new people, mana, awakening of the gifted, healers' rounds,
// the undead's upkeep and decay, troubles and their paths (coping.json).
void magicDaily(World& w);
// Commands: magic_cast {id, spell, target?, at?}, magic_save {id, spell, name},
// magic_teach {id, pupil, aspect}, magic_learn {id, aspect, amount} (scenario/debug).
void installMagic(World& w);

} // namespace lr::sim
