#pragma once
// LR-171: prayers and rites of the cults (setting v0.3 §6.5, §1.1).
//
// A prayer is an act with parameters: to whom (faith / lik), for what (aim), with what
// (offering), how (rite), where and when. The god answers it (the pantheon module
// registers the answer hook): granted, refused with a reason, deferred to a day, or
// silent (the god's debt grows). Without a hook a simple data-driven fallback answers,
// so the sim and the tests run on their own.
//
//   who prays   anyone who does a pray act (content/culture/prayers.json "acts"): the aim
//               is what the person needs most now (the sick in the house, a drought, a
//               grief, a foe, a road...), the offering the best thing carried; the player
//               names them with the command pray {id, aim, offering?, lik?, target?}
//   weight      base + piety + the place (shrine) + offering + a rite + a holy day
//   answer      hook or fallback; granted -> the aim's effects (people, the weather /
//               fields of the settlement) x strength; refused / silent -> moodlets
//
// Rites are data (content/culture/rites/*.json, one file per group of cults): who keeps
// them (faiths / liks), the kind (sacrifice, feast, procession, vigil, oath, pilgrimage,
// contest, funeral, ecstatic, mystery, trance...), when (a holy day, every N days, a
// season, an event: death, wedding, birth, illness, drought, war), the time of day, who
// takes part (the faith, the settlement, the household, the initiates, the mourners),
// the leader (clergy, the most pious, the head), the offering, the steps (acts at the
// shrine, the fire, the water, the grove...; the first whose place is near) and the
// effects (moodlets, memory, piety, the world's channels, a common prayer), the
// aftermath (hangover, jealousy, scandal...). The daily pass schedules them per
// settlement, the tick pass leads the steps (queued acts), attendance is counted when
// a person does a step act. Secret rites (mysteries) raise the person's grade.
//
// Intimacy in an ecstatic rite is for adults only (Body::lifeYears() >= min_life_years
// and never a child's stage) and by consent (both sides' affinity and attraction above
// the rite's thresholds); it is the common intimacy act, shown by the client as a pixel
// cloud with hearts (content/sprites_2d/rites2d.json). The validator refuses a rite
// with intimacy without adults_only and consent.
//
// The client reads riteOf (pawn: rite, step fx, attire, woohoo) and ritesJson.

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "sim/core.hpp"
#include "sim/rules.hpp"

namespace lr::sim {

class World;
struct Character;
struct Content;
struct InteractionDef;
struct WorldObject;
struct ValidationReport;

struct Prayer {
    EntityId who = kNone;
    std::string faith;
    std::string lik;      // may be empty: the pantheon picks the lik of the faith for the aim
    std::string aim;      // heal, rain, harvest, safe_road, protect, revenge, love, forgive,
                          // victory, child, wisdom, good_hunt, safe_sea, rest_dead, thanks
    std::string offering; // item id or ""
    std::string rite;     // rite id or ""
    EntityId place = kNone;
    EntityId target = kNone; // whom the prayer is for (the sick one, the foe), kNone = the one who prays
    int day = 0;
    double weight = 0.0;  // the strength of the plea: offering x rite x place x piety, 0..~3
};

struct PrayerAnswer {
    enum Kind { Granted, Refused, Deferred, Silent } kind = Silent;
    std::string reason;  // loc key of a refusal (pantheon.refuse.*, rites.refuse.*)
    int dueDay = 0;      // for Deferred: the day the god answers
    double strength = 0; // 0..1: how fully a granted prayer is fulfilled
};

using PrayerAnswerFn = std::function<PrayerAnswer(World&, const Prayer&)>;
// The pantheon registers its answer (the god's choice, favor, debt, standing, the laws).
void setPrayerAnswerHook(PrayerAnswerFn f);
// Fulfils the aim of a prayer (the effects of prayers.json, scaled by strength); the
// pantheon calls it when a deferred prayer comes due.
void fulfillPrayer(World& w, const Prayer& p, double strength);

using RiteDoneFn = std::function<void(World&, EntityId who, const std::string& faith, const std::string& rite,
                                      double weight)>;
// Called for every participant when a rite is done (the pantheon raises favor / standing).
void setRiteDoneHook(RiteDoneFn f);

// --- data -------------------------------------------------------------------------------
struct AimDef {
    std::string id, when, forWhom; // forWhom: "" (self) | sick | foe
    double pick = 1.0;
    bool refuseIfKin = false;
    std::vector<Effect> eff, targetEff, householdEff;
    Json channels = Json::object();
    double radius = 400.0;
};

struct RiteStep {
    std::vector<std::string> all, leader; // acts, the first whose place is near
    int ticks = 2;
    std::string fx;
};

struct RiteDef {
    std::string id, group, kind, file;
    bool open = true, secret = false;
    std::vector<std::string> faiths, liks, grades;
    // when
    std::string holyDay, on; // holy day id or "any"; event
    int everyDays = 0, offset = 0, seasonFrom = -1, seasonTo = -1;
    double chance = 1.0;
    std::string time = "day";
    // who
    std::string who = "faith", age = "any", leader = "clergy";
    int minN = 2, maxN = 40;
    double radius = 900.0;
    std::vector<std::string> offerAny;
    int offerN = 0;
    std::string attire;
    std::vector<RiteStep> steps;
    // effects
    std::vector<Effect> effAll, effLeader;
    double piety = 0.0;
    std::vector<std::string> memory;
    Json channels = Json::object();
    std::string aim;
    struct After {
        double p = 0.0;
        std::string who = "any"; // any | intimate | spouse_of_intimate | leader
        std::vector<Effect> eff;
    };
    std::vector<After> aftermath;
    struct Intimacy {
        bool on = false, adultsOnly = false, consent = false;
        int minLifeYears = 18;
        double share = 0.25, affinity = 0.3, attraction = 0.25;
        std::string act = "intimacy", show;
    } intimacy;
    int totalTicks() const;
};

struct RitesRules {
    bool loaded = false;
    std::vector<std::string> prayActs;
    struct Weight {
        double base = 0.4, piety = 0.8, rite = 0.6, holyDay = 0.3, max = 3.0;
        std::map<std::string, double> acts;
    } weight;
    std::map<std::string, double> offerings;
    struct Fallback {
        double grantBase = 0.12, grantPerWeight = 0.22, grantMax = 0.85, deferShare = 0.45;
        int deferMin = 1, deferMax = 5;
        double strengthBase = 0.5, strengthPerWeight = 0.2;
    } fallback;
    struct Answer {
        std::string moodlet, memory, chronicle;
        double piety = 0.0;
    };
    std::map<std::string, Answer> answers; // granted | refused | deferred | silent
    std::vector<AimDef> aims;
    int griefDays = 30;
    std::vector<std::string> guiltTypes, foeTypes;
    // the rites
    std::vector<std::string> riteOrder;
    std::map<std::string, RiteDef> rites;
    Json rules = Json::object(); // rite_rules.json as is
    std::map<std::string, int> timeTick; // dawn | day | dusk | night -> tick of the day
    std::vector<std::string> clergy;     // occupations that lead rites
    int maxPerDay = 2, eventDays = 2;
    double queuePriority = 0.9, droughtBelow = 0.25;
    std::map<std::string, std::vector<std::string>> onTypes, onCodes; // event -> chronicle types / code parts
    std::vector<std::string> errors;

    const AimDef* aim(const std::string& id) const;
    const RiteDef* rite(const std::string& id) const;
};

const RitesRules& ritesRules(const Content& c);

// --- prayers ------------------------------------------------------------------------------
// The aim a person would pray for now ("thanks" when nothing presses), and for whom.
std::string prayerAimOf(World& w, const Character& c, EntityId* forWhom = nullptr);
// Builds, answers and applies a prayer (the answer is also stored for the client).
PrayerAnswer offerPrayer(World& w, Prayer p);
// The weight of a plea of `c` at `place` with `offering` (an item id, "" none).
double prayerWeight(const World& w, const Character& c, const std::string& act, const std::string& offering,
                    const std::string& rite);

// --- rites --------------------------------------------------------------------------------
// The rites the faith keeps (by faith id or by the liks of its deities).
std::vector<const RiteDef*> ritesOfFaith(const Content& c, const std::string& faith);
// Starts a rite in a settlement now (or at `at`); "" = ok, else a rejection code.
std::string startRite(World& w, EntityId settlement, const std::string& rite, Tick at = -1, EntityId leader = kNone);
// What the person is doing in a rite now (null when none): {rite, kind, step, fx, attire,
// leader, woohoo}.
Json riteOf(const World& w, EntityId who);
// The active and the last finished rites (the client's list): settlement kNone = all.
Json ritesJson(const World& w, EntityId settlement = kNone);
// The secret grade of a person in a rite's mystery (-1: not initiated).
int riteGrade(const World& w, EntityId who, const std::string& rite);

// --- passes -------------------------------------------------------------------------------
void ritesDaily(World& w);
void ritesTick(World& w);
// After every act (ai_eval applyEffects): a pray act prays, a rite step act counts.
void ritesAfterAct(World& w, Character& c, const InteractionDef& d, WorldObject* target, Character* targetChar,
                   bool success);
// Commands: pray {id, aim?, faith?, lik?, offering?, target?, now?}, start_rite {settlement, rite, leader?}.
void installRites(World& w);
void validateRites(const Content& c, ValidationReport& r);

} // namespace lr::sim
