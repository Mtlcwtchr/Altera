#pragma once
// LR-171: prayers and rites of the cults (setting v0.3 §6.5, §1.1).
//
// A prayer is an act with parameters: to whom (faith / lik), for what (aim), with what
// (offering), how (rite), where and when. The god answers it (the pantheon module,
// LR pantheon thread, registers the answer hook): granted, refused with a reason,
// deferred to a day, or silent (the god's debt grows). Without a hook a simple
// data-driven fallback answers, so the sim and the tests run on their own.
//
// Rites are data (content/culture/rites/*.json): who keeps them (faiths / liks), the kind
// (sacrifice, feast, procession, vigil, oath, pilgrimage, contest, funeral, ecstatic,
// mystery, trance), the needs (place, time of day, season, offering, participants, attire),
// the steps (acts) and the effects (moodlets, memory, piety, cell fields), open or secret,
// grades of initiation. Intimacy in a rite is for adults only and by consent; the client
// shows it as a pixel cloud with hearts (content/sprites_2d).
//
// Aims and their effects when granted: content/culture/prayers.json.

#include <functional>
#include <string>

#include "sim/core.hpp"

namespace lr::sim {

class World;

struct Prayer {
    EntityId who = kNone;
    std::string faith;
    std::string lik;      // may be empty: the pantheon picks the lik of the faith for the aim
    std::string aim;      // heal, rain, harvest, safe_road, protect, revenge, love, forgive,
                          // victory, child, wisdom, good_hunt, safe_sea, rest_dead
    std::string offering; // item id or ""
    std::string rite;     // rite id or ""
    EntityId place = kNone;
    EntityId target = kNone; // whom the prayer is for (the sick one, the foe), kNone = the one who prays
    int day = 0;
    double weight = 0.0;  // the strength of the plea: offering x rite x place x piety, 0..~3
};

struct PrayerAnswer {
    enum Kind { Granted, Refused, Deferred, Silent } kind = Silent;
    std::string reason;  // loc key of a refusal (pantheon.refuse.*)
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

} // namespace lr::sim
