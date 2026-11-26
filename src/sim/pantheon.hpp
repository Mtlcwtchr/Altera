#pragma once
// LR-169: the pantheon - Powers, their liks (the gods of each belt), the world laws.
//
// Setting v0.3 (gdd/setting/LIVELYREALMS_SETTING_v0.3_MYTHIC_WORLD_PROPOSAL.md §4, §6):
//
//   power     an eternal domain of the world (Storm and Sky, Sea, the Underworld...):
//             magic aspects, prayer aims it may grant, world laws it watches, values
//   lik       one god of one belt, a face of a Power (Zeus, Thor, Perun, Tengri for Storm
//             and Sky): a character of its own (wrath, mercy, pride, jealousy, generosity,
//             cunning, aloof), loves and hates (memory types of mortal deeds), wanted
//             offerings, epithets, rivals and allies
//   faith     (LR-155, content/culture/religions) the cult of a group of liks per belt;
//             a person's gods = the liks of the faith for the belt of the person's culture
//
// Gods are entities of the sim under the same daily rules as everyone (no special case
// per god): each lik has a standing fed by worship (piety of its people, rites, offerings),
// a debt, a share of its Power's domain, favour toward each mortal, a ledger. A god acts
// only through favour and wrath, omens, answers to prayers, curses; never kills or raises
// by hand (the sandbox stays). State: World::moduleState("pantheon").
//
// The world laws (content/laws/*.json) bind everyone, gods included. A breach is found in
// World::facts (the legal tag of a deed + conditions) or in a doer's memories, or reported
// by another module (reportBreach); retribution comes later, from the Powers of Fate and
// Retribution: a mortal is cursed, loses favour and piety, the community turns away; a
// settlement where many broke a law is cursed whole; a god loses standing, may be bound
// (in chains for a time) and lose part of its domain to another lik of the same Power.
//
// Prayer (the exchange with the gods): a god must answer a prayer made by the rule
// (with an offering or a rite): grant, refuse with a reason, defer to a day; silence or a
// broken promise grows the god's debt, and a debt past the line is a breach of the law of
// Exchange, punished like any other. The god's choice is a utility score from data
// (content/pantheon/rules.json prayer) over its character, the petitioner's favour, the
// weight of the plea, the god's standing and today's spent miracles.
//
// Identification (interpretatio): where people of faiths whose liks share a Power live
// together, the two liks come to be held one Power under two names (a shared favour, a
// little affinity); zealots reject it as heresy (enmity, a moodlet). Rival liks contend.
//
// Data (all of it): content/pantheon/{rules,powers}.json, content/pantheon/liks/<belt>.json,
// content/laws/*.json, the faiths' power/liks (content/culture/religions), a culture's belt
// (content/species/cultures/<id>.json "belt"). Loc: content/loc/<lang>/pantheon.json.

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "sim/core.hpp"

namespace lr::sim {

struct Character;
struct Content;
class World;
struct ValidationReport;

struct PowerDef {
    std::string id;
    std::vector<std::string> aspects, aims, laws;
    std::map<std::string, double> values;
};

struct LikDef {
    std::string id, belt, power;
    std::vector<std::string> also; // further Powers: their domains are the lik's too
    std::map<std::string, double> character;
    std::vector<std::string> loves, hates, wants, epithets, rivals, allies;
    int phase = 1;
    double trait(const std::string& k, double d = 0.5) const;
    bool hasPower(const std::string& p) const;
};

struct LawDef {
    std::string id;
    std::vector<std::string> watchers; // Powers
    double weight = 1.0;
    bool bindsGods = true;
    struct FactRule {
        std::vector<std::string> preds, when;
    };
    std::vector<FactRule> facts;
    std::vector<std::string> memories;
};

struct PantheonRules {
    bool loaded = false;
    std::vector<std::string> belts;
    std::map<std::string, std::string> cultureBelts; // fallback culture -> belt
    std::vector<std::string> powerOrder, likOrder, lawOrder;
    std::map<std::string, PowerDef> powers;
    std::map<std::string, LikDef> liks;
    std::map<std::string, LawDef> laws;
    struct Standing {
        double start = 0.5, gain = 0.02, decay = 0.01, riteGain = 0.004, prayerGain = 0.002, floor = 0.02, worshipNorm = 20.0;
    } standing;
    struct Favor {
        double start = 0.0, riteGain = 0.03, loveGain = 0.02, hateLoss = 0.03, sinLoss = 0.02, decay = 0.02, offeringGain = 0.05, wantedMult = 2.0;
        double moodletAt = 0.35, wrathAt = -0.35, omenChance = 0.03, identShare = 0.5;
        std::string favorMoodlet = "divine_favor", wrathMoodlet = "divine_wrath";
    } favor;
    struct Ident {
        double base = 0.02, step = 0.08, at = 0.6, hospitalityMult = 1.0, affinity = 0.05;
        double heresyPiety = 0.7, heresyTolerance = 0.35, heresyChance = 0.3, heresyEnmity = 0.08, heresyStep = 0.04;
        int rivalryEveryDays = 60, heresyEveryDays = 30;
        std::string heresyMoodlet = "heresy_anger";
    } ident;
    struct Prayer {
        double grant = 0.4, refuse = 0.1, defer = 0.15, silent = -0.2;
        double wFavor = 0.6, wWeight = 0.25, wMercy = 0.3, wGenerosity = 0.2, wDomain = 0.3, wBudget = 0.6;
        double wWrath = 0.35, wPride = 0.15, wAloof = 0.35, wCunning = 0.15, wDebt = 0.4, noise = 0.1;
        double budgetBase = 1.0, budgetPerStanding = 6.0;
        int deferMinDays = 1, deferMaxDays = 5;
        double debtSilent = 0.25, debtOverdue = 0.4, debtRepay = 0.3, debtDecay = 0.01, debtBreachAt = 1.0;
        double strengthBase = 0.35, strengthWeight = 0.25, strengthDomain = 0.3;
        std::string grantedMoodlet = "prayer_granted", refusedMoodlet = "prayer_refused";
    } prayer;
    struct Retribution {
        int delayMinDays = 1, delayMaxDays = 6, curseDays = 10, settlementWindowDays = 30;
        double curseMood = -0.12, pietyLoss = 0.1, favorLoss = 0.25, communityTrust = -0.1, healthLoss = 0.03;
        double witnessMult = 0.1, witnessCap = 0.5, sacredMult = 0.5, swornMult = 0.5;
        int settlementBreaches = 3;
        std::string curseMoodlet = "cursed", breachMemory = "law_breach", curseMemory = "cursed", initiateMemory = "initiated";
        double sacredRadius = 6.0, graveRadius = 4.0;
        std::vector<std::string> sacredObjects = {"shrine", "altar", "hearth", "temple"}, graveObjects = {"grave", "barrow", "tomb"};
        // a god
        double godStanding = 0.25, bindAt = 1.2, domainLoss = 0.25;
        int bindDays = 30;
    } retribution;
    struct MoodletDef {
        double value = 0.0;
        Tick ttl = 144;
    };
    std::map<std::string, MoodletDef> moodlets;
    std::vector<std::string> errors;

    const PowerDef* power(const std::string& id) const;
    const LikDef* lik(const std::string& id) const;
    const LawDef* law(const std::string& id) const;
};

// The rules (cached per data generation); `loaded` false without content/pantheon.
const PantheonRules& pantheonRules(const Content& c);

// --- who worships whom -----------------------------------------------------------------
// The belt of a culture: its "belt" field, else rules.json culture_belts, else "".
std::string cultureBelt(const Content& c, const std::string& culture);
// The liks of a faith for a belt ("*" when the belt has none listed); patron first.
std::vector<std::string> faithLiks(const Content& c, const std::string& faith, const std::string& belt);
// All the liks of a faith over every belt, in order, once each.
std::vector<std::string> faithAllLiks(const Content& c, const std::string& faith);
// The gods of a person (the faith's liks for the belt of the person's culture).
std::vector<std::string> godsOf(const World& w, const Character& c);
// Whether a lik's Powers may grant an aim, and the lik of a faith to pray to for it
// (the first of the belt's liks that serves the aim, else the patron; "" = none).
bool likServesAim(const Content& c, const std::string& lik, const std::string& aim);
std::string likForAim(const World& w, const std::string& faith, const std::string& aim, const std::string& belt = "");
std::string likForAim(const World& w, const Character& c, const std::string& faith, const std::string& aim);

// --- the gods' state -------------------------------------------------------------------
struct GodState {
    double standing = 0.0, debt = 0.0, domain = 1.0; // domain: a share multiplier (lost domain < 1)
    int boundUntil = -1;                              // in chains until this day
    int worshippers = 0;
    bool bound(int day) const { return boundUntil > day; }
};
GodState godState(const World& w, const std::string& lik);
// The lik's share of its Power's domain among the Power's liks (by standing and domain).
double domainShare(const World& w, const std::string& lik);
// Favour of a god toward a mortal (-1..1); `shared` adds the favour of liks identified with it.
double favorOf(const World& w, EntityId who, const std::string& lik, bool shared = true);
void addFavor(World& w, EntityId who, const std::string& lik, double d, const std::string& why = "");
// How far two liks are held one Power (0..1) and whether it passed the line.
double identification(const World& w, const std::string& a, const std::string& b);
bool identified(const World& w, const std::string& a, const std::string& b);

// --- prayer: the exchange with the gods --------------------------------------------------
struct PrayerAsk {
    EntityId who = kNone;
    std::string faith, lik, aim, offering, rite; // lik "" = the faith's lik for the aim
    EntityId place = kNone;
    int day = -1;        // -1 = today
    double weight = 1.0; // the plea's strength: offering x rite x place x piety (0..~3)
    bool byRule() const { return !offering.empty() || !rite.empty(); } // obliges an answer
};
enum class DivineAnswerKind { Granted, Refused, Deferred, Silent };
struct DivineAnswer {
    DivineAnswerKind kind = DivineAnswerKind::Silent;
    std::string lik;    // who answered
    std::string reason; // refused: pantheon.refuse.<id>
    int dueDay = -1;    // deferred: the day it comes
    double strength = 0.0;
};
const char* answerName(DivineAnswerKind k); // granted | refused | deferred | silent
// The god decides and the world records it (favour for the offering, standing, debt,
// a deferred promise, the chronicle, the petitioner's moodlet). Effects of a grant are the
// caller's (the rites module); a deferred grant is fulfilled later through the hook below.
DivineAnswer answerPrayer(World& w, const PrayerAsk& p);
// Who applies a deferred grant when its day comes (the rites module registers it).
using PrayerFulfillFn = std::function<void(World&, const PrayerAsk&, double strength)>;
void setPrayerFulfill(PrayerFulfillFn fn);
// A rite done for a faith (the rites module may call it; memories of rites count anyway).
void onRiteDone(World& w, EntityId who, const std::string& faith, const std::string& rite, double weight);

// --- the world laws ----------------------------------------------------------------------
struct Breach {
    std::string law;
    EntityId doer = kNone; // a mortal, or kNone with `god`
    std::string god;       // a lik that broke it
    EntityId victim = kNone;
    double weight = 1.0;   // before the law's own weight
    std::vector<EntityId> witnesses;
    std::string sworn;     // a Power or lik sworn by (oath): it is a witness itself
    bool sacred = false;   // at a holy place
    std::string why;
};
// Record a breach; retribution follows in a few days. Returns false for an unknown law.
bool reportBreach(World& w, const Breach& b);
// Open (not yet punished) breaches of a mortal or a god ("" god = by doer).
int openBreaches(const World& w, EntityId doer, const std::string& god = "");
// The breaches the world recorded (open and punished), newest last.
Json breachLog(const World& w);

// --- the daily pass, commands, checks ----------------------------------------------------
void pantheonDaily(World& w);
// Commands: pray {id, aim, lik?, faith?, offering?, rite?, weight?},
// law_breach {law, id? | god?, weight?, victim?}, set_favor {id, lik, favor}.
void installPantheon(World& w);
void validatePantheon(const Content& c, ValidationReport& r);
std::vector<std::string> pantheonLocKeys(const Content& c);

} // namespace lr::sim
