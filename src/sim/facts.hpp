#pragma once
// The registry of truth at work (DIALOGUE_GEN §6.4, AL-55).
//
// World::facts keeps what really happened; memories point at it (fact).
// Nobody reads it directly. It shows through what it points at: one who
// really saw a deed can vouch for the wrongly blamed (vouch), the blamed can
// point to such a witness (prove_innocence), the thing itself can be found.
// A word with nothing true behind it - a lie, a rumour about a deed nobody
// did - falls apart the same way; a guilty man who claims innocence before a
// true witness is caught lying.

#include <string>

#include "sim/core.hpp"

namespace altera::sim {

class World;
struct Character;
struct Content;
struct InteractionDef;
struct Fact;

struct FactRules {
    double refuteMult = 0.2;       // a refuted belief keeps this much of its certainty
    double dropBelow = 0.15;       // ...and is forgotten below this
    double clearedTrust = 0.15;    // listener toward the cleared one
    double clearedEnmity = -0.15;
    double spreaderTrust = -0.2;   // listener toward the one he had it from
    double clearedHonesty = 0.04;  // the cleared one's name
    double witnessConfidence = 0.9; // what the witness tells of the real doer
    double lieTrust = -0.2;        // caught lying (claiming innocence before a true witness)
    double unprovenMult = 0.9;     // word against word: a little doubt
    std::string vouchAct = "vouch", proveAct = "prove_innocence";
};

const FactRules& factRules(const Content& c);

// What the truth says of "`claimed` did the deed on `object`" (the fact the
// memory names, else the latest open deed on that object, or on that household).
struct TruthCheck {
    const Fact* fact = nullptr;
    bool contradicted = false; // another did it (or nobody did anything)
    bool confirmed = false;    // he did it
    EntityId realDoer = kNone;
    EntityId witness = kNone;  // a living true witness who can say so (not `claimed`)
};
TruthCheck checkDoer(const World& w, EntityId object, EntityId claimed, EntityId victimFamily = kNone,
                     EntityId fact = kNone);

// `speaker`, who really saw a deed `listener` blames on someone else, tells
// him how it was. Returns how many of the listener's beliefs it changed.
int vouch(World& w, Character& speaker, Character& listener);
// `accused` shows `listener` he did not do what he is blamed for (through a
// true witness, or because nothing of the sort happened). Caught lying when
// he did it and a true witness exists. Returns beliefs changed (-1: caught).
int proveInnocence(World& w, Character& accused, Character& listener);

// The acts vouch / prove_innocence just done (afterDeed).
void factsAfterAct(World& w, Character& doer, const InteractionDef& d, Character* targetChar, bool success);

// QA: per fact, who believes what of it (truth vs rumour), and beliefs with no fact.
Json truthReport(const World& w);

} // namespace altera::sim
