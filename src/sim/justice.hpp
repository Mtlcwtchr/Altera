#pragma once
// After an accusation (AL-37, content/ai/consequences.json "case").
//
// An accusation opens a case. The accused answers: gives back what was
// taken, pays it off, denies it or runs. A denial leaves the accuser to
// choose: let it go, quarrel, fight, or take it to the chief (judge_case).
// The judge weighs what is known (the accuser's certainty, witnesses, the
// accused's name, the judge's own ties), finds guilty or acquits and
// sentences: restitution and a fine, shame, custody (captivity) or exile.
// An acquittal is a false accusation and costs the accuser his name; a true
// witness of the deed can clear the accused and name the real doer.
//
// Every way a case ends is in the chronicle (chronicle.case_*). Choices are
// scored from data (traits, relations, guilt, evidence) with a small seeded
// noise, so a world replays the same. The case lives in the accused's memory
// (case_pending) and in "case" events of the world queue, so saves keep it.
// The truth is the world's record of the deed (World::facts, Fact):
// the judge never reads it, but those who really saw it testify from it, to
// prove guilt or to clear the accused and name the real doer.

#include <map>
#include <string>
#include <vector>

#include "sim/core.hpp"

namespace altera::sim {

class World;
struct Character;
struct Content;
struct InteractionDef;
struct Fact;

// kind: trait (key) | rel (key = axis, chooser toward the other; mul) |
// guilty | can_return | can_pay | has_judge | seen | kin (flag; add) |
// evidence (mul x certainty) | crimes (add per earlier deed, cap)
struct CaseTerm {
    std::string kind, key;
    bool flag = true;
    double add = 0.0, mul = 0.0, cap = 0.0;
    bool capped = false;
};

struct CaseOption {
    std::string id;
    double base = 0.0;
    std::vector<CaseTerm> terms;
};

struct CaseRules {
    double answerHours = 2.0, pursueHours = 4.0, courtWaitDays = 3.0;
    double noise = 0.1;
    std::map<std::string, int> worth;           // item -> value (paying off, fines)
    std::map<std::string, int> defaultWorth;    // tag -> value when nobody knows what was taken
    double compensateMult = 1.5;                // paying off costs more than giving back
    int fineValue = 3;
    std::vector<CaseOption> answers, pursuits, sentences;
    // court: guilty when the weighed evidence reaches guiltyAt
    double guiltyAt = 0.5, wCertainty = 0.5, wWitness = 0.2, witnessCap = 0.6, wBadName = 0.4, wPrior = 0.1,
           priorCap = 0.3, wJudgeAffinity = -0.3, wJudgeEnmity = 0.3, courtNoise = 0.15;
    double wCleared = 1.0; // a true witness names another as the doer
    std::string judgeTask = "judge_case", quarrelTask = "argue", fightTask = "brawl";
    double taskPriority = 0.7;
    Tick taskTtl = 288;
    double fleeM = 400.0, exileM = 500.0;
    std::map<std::string, std::map<std::string, double>> reputation; // outcome -> axis -> delta
    struct RelShift {
        double trust = 0.0, enmity = 0.0;
    };
    std::map<std::string, RelShift> accuserToAccused, accusedToAccuser;
};

// The "case" section of consequences.json (cached per content dir).
const CaseRules& caseRules(const Content& c);

// The truth of a misdeed goes into the world's record (afterDeed): doer,
// victim, what it took, who really saw it.
void keepTruth(World& w, Character& doer, const InteractionDef& d, EntityId victimFamily, EntityId victimPerson,
               EntityId object, const std::string& tag, const std::vector<EntityId>& witnesses);
// The open record of a deed on `object` (by `doer` when given; object kNone:
// any open wrong of `doer` to that household). nullptr when none.
const Fact* findDeed(const World& w, EntityId object, EntityId doer, EntityId victimFamily = kNone);
// Who can still bear witness to a record (alive, and not the doer).
std::vector<EntityId> trueWitnesses(const World& w, const Fact& d);
// An act just done: an accusation opens a case, a judge_case holds the court.
void caseAfterAct(World& w, Character& doer, const InteractionDef& d, Character* targetChar, bool success);
// Installs the "case" event handler.
void installJustice(World& w);

// Value of goods by the worth table (tests, trace).
int goodsWorth(const CaseRules& R, const std::map<std::string, int>& goods);
// Scores of options for a chooser (tests); no noise. ctx flags as in CaseTerm.
struct CaseCtx {
    bool guilty = false, canReturn = false, canPay = false, hasJudge = false, seen = false, kin = false;
    double evidence = 0.0;
    int crimes = 0;
};
double caseScore(const CaseOption& o, const Character& chooser, const Character& other, const CaseCtx& x);

} // namespace altera::sim
