#pragma once
// LR-155: cultures and faiths of people and settlements.
//
// The scheme of doc/ancient_settlement_rts_gdd_v0.1.md (§2.4 "culture is expressed
// materially", §6 "knowledge, traditions and discoveries", §7 "social and cultural
// needs"): a custom first lives in people, it grows by being practised and becomes a
// settlement's own tradition, it fades when nobody keeps it; a faith is a person's own
// need, shown by deeds. Nothing is scripted - the same daily rules for every person and
// settlement (the player's dynasty is not special):
//
//   person      culture + bond to it: in a settlement of another culture the bond
//               thaws (faster for children, faster where newcomers are welcome) until
//               the person takes the place's ways; children take the mother's (rules)
//   faith       faith + piety: rites (memories of deeds) raise it, sins lower it, it
//               relaxes to the base; a person of little piety may take the faith of
//               spouse, household, settlement, friends; some faiths call by a deed
//               (drawn_by: grief -> necromancy -> the Pale Mourners)
//   settlement  traditions with a strength: the share of residents who practised the
//               custom today raises it, every day lowers it; fixed / forming / fading;
//               a custom practised by newcomers takes root, a lost one leaves
//   behaviour   held traditions and the faith bend the residents' goals (WeightMod,
//               source "culture"); keeping a fixed custom gives a moodlet; holy days
//
// Data (all of it): content/culture/{rules,values,traditions,inspect}.json,
// content/culture/religions/*.json, content/species/cultures/*.json (faith, values,
// traditions, rites, food, desc). The settlements' state: World::moduleState("culture").
// Chronicle codes chronicle.culture.* (content/loc/<lang>/culture.json).

#include <map>
#include <string>
#include <vector>

#include "sim/core.hpp"

namespace lr::sim {

struct Character;
struct Content;
class World;
struct ValidationReport;

struct TraditionDef {
    std::string id, kind;                 // rite | custom | festival | law | craft
    std::vector<std::string> practice;    // memory types that keep it
    std::map<std::string, double> goals;  // goal id -> weight shift (x strength)
    std::map<std::string, double> values; // value id -> shift of the settlement's values (x strength)
    std::string moodlet;                  // for who keeps it while it is fixed
};

struct FaithDef {
    std::string id, kind; // pantheon | ancestors | animism | spirits | cult | self
    bool heresy = false;
    struct Deity {
        std::string id, domain;
    };
    std::vector<Deity> deities; // LR-169: built from "liks" (every belt, once each; domain = the lik's Power)
    std::string power;                                    // LR-169: the faith's main Power (content/pantheon)
    std::map<std::string, std::vector<std::string>> liks; // LR-169: belt ("*" = any) -> liks, patron first
    std::map<std::string, double> values, rites, sins, goals, drawnBy;
    struct HolyDay {
        std::string id;
        int day = 0; // of the year
    };
    std::vector<HolyDay> holyDays;
    std::string burial, clergy;
    std::vector<std::string> sacred, blessed, forbidden;
    double zeal = 0.2, tolerance = 0.5;
};

struct CultureRules {
    bool loaded = false;
    std::vector<std::string> values;
    std::vector<std::string> traditionOrder;
    std::map<std::string, TraditionDef> traditions;
    std::map<std::string, FaithDef> faiths;
    struct Person {
        std::string inherit = "mother";
        double bondStart = 0.8, driftAdult = 0.0025, driftChild = 0.012, driftFloor = 0.35, grow = 0.002;
        double switchBelow = 0.15, afterSwitch = 0.45;
    } person;
    struct Tradition {
        double start = 0.85, gain = 0.06, normShare = 0.25, fade = 0.004, fixed = 0.7, forming = 0.25, lost = 0.03, seed = 0.08;
        int weightTtlDays = 1;
    } tradition;
    struct Faith {
        double basePiety = 0.45, relax = 0.01, convertBelow = 0.35, convertBase = 0.04, keep = 0.5;
        std::map<std::string, double> pull; // spouse | household | settlement | friend
        double friendAffinity = 0.4, pietyAfterConvert = 0.5, weightsScale = 1.0;
        std::string holyDayMoodlet = "holy_day";
        std::map<std::string, double> traitBase; // trait -> base piety instead of basePiety
    } faith;
    Json inspect = Json::object(); // inspect.json as is (the client reads it)
    std::vector<std::string> errors;

    const TraditionDef* trad(const std::string& id) const;
    const FaithDef* faithDef(const std::string& id) const;
};

// The rules of the content (cached per data generation); `loaded` false without content/culture.
const CultureRules& cultureRules(const Content& c);

// --- a person -------------------------------------------------------------------------
// The person's culture: the stored one, else what the daily pass would give (parents,
// the settlement, the kind's default). "" for animals.
std::string cultureOf(const World& w, const Character& c);
std::string faithOf(const World& w, const Character& c);
double pietyOf(const World& w, const Character& c);
double cultureBondOf(const World& w, const Character& c);
// The faith a culture keeps (its "faith" field), "" = none.
std::string cultureFaith(const Content& ct, const std::string& culture);
// The culture's own JSON (content/species/cultures/<id>.json), null when unknown.
const Json* cultureData(const Content& ct, const std::string& culture);

// --- a settlement ---------------------------------------------------------------------
struct TraditionState {
    double strength = 0.0;
    int since = 0;        // the day it took root (or the world began with it)
    double practised = 0; // the share of residents who kept it on the last day
    std::string status(const CultureRules& r) const; // fixed | forming | fading
};
std::map<std::string, TraditionState> settlementTraditions(const World& w, EntityId settlement);
// The settlement's values: its culture's, bent by the traditions it holds (-1..1).
std::map<std::string, double> settlementValues(const World& w, EntityId settlement);
// Who lives there by culture and by faith (alive people of the settlement's households).
struct SettlementBelief {
    int residents = 0;
    std::map<std::string, int> cultures, faiths;
    std::map<std::string, double> piety; // faith -> mean piety of its people there
};
SettlementBelief settlementBelief(const World& w, EntityId settlement);

// Day of the year (0..359) the world is on, and the holy day of a faith on it ("" = none).
int dayOfYear(const World& w);
std::string holyDayOn(const FaithDef& f, int day);

// --- the daily pass and commands --------------------------------------------------------
void cultureDaily(World& w);
// Commands: set_faith {id, faith, piety?}, set_culture {id, culture, bond?} (scenario / debug).
void installCulture(World& w);
void validateCulture(const Content& c, ValidationReport& r);

} // namespace lr::sim
