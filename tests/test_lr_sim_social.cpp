// Social chains (AL-40..AL-43): an act that asks the other's answer, the
// answer as the other's own act, the pair playing it out together (in place
// or at a bed), and an accepted invitation making the other follow - an NPC
// leader or the player's WASD - and join what the leader does.

#include <algorithm>
#include <map>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/social_chain.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {

const Content& realContent() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

World realWorld(std::uint64_t seed = 7) {
    static const Json cfg = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(realContent(), cfg, seed);
    installAi(w);
    return w;
}

Command cmdOf(const std::string& type, Json args, int player = 1) {
    Command c;
    c.source = "player:" + std::to_string(player);
    c.type = type;
    c.args = std::move(args);
    c.args["player"] = player;
    return c;
}

// Two adults of different households, not kin, put side by side far from the rest.
std::pair<Character*, Character*> pair(World& w, bool sameFamily = false) {
    Character* a = nullptr;
    Character* b = nullptr;
    for (auto& [id, c] : w.characters()) {
        if (c.isAnimal() || !c.body.alive || c.body.stage() != "adult") continue;
        if (!a) a = &c;
        else if ((c.status.family == a->status.family) == sameFamily && c.status.mother != a->id &&
                 c.status.father != a->id && a->status.mother != c.id && a->status.father != c.id &&
                 (c.status.mother == kNone || c.status.mother != a->status.mother)) {
            b = &c;
            break;
        }
    }
    return {a, b};
}

void fond(Character& of, const Character& toward, double v) {
    Relation& r = of.relations[toward.id];
    r.familiarity = 1.0;
    r.affinity = v;
    r.attraction = v;
    r.trust = v;
    r.enmity = v < 0 ? -v : 0.0;
}

void calm(Character& c) {
    c.ai.fsm = FsmState{};
    c.ai.goal = GoalInstance{};
    c.ai.plan = Plan{};
    for (auto& [k, v] : c.needs) v = 0.9;
}

bool hasMoodlet(const Character& c, const std::string& id, Tick now) { return c.hasMoodlet(id, now); }

bool traced(const Character& c, const std::string& code) {
    return std::any_of(c.ai.trace.begin(), c.ai.trace.end(), [&](const TraceEntry& e) { return e.code == code; });
}

// Steps the world until `done` or n ticks.
template <class F>
void runUntil(World& w, Tick n, F done) {
    for (Tick i = 0; i < n && !done(); ++i) w.step();
}

} // namespace

TEST(lr_sim_social_chains_load) {
    const Content& ct = realContent();
    const SocialRules* R = socialRules(ct);
    CHECK(R != nullptr);
    if (!R) return;
    CHECK(socialChain(ct, "kiss") != nullptr);
    CHECK(socialChain(ct, "embrace") != nullptr);
    const SocialChainDef* in = socialChain(ct, "intimacy");
    CHECK(in && in->place == "bed" && in->stages.size() == 3);
    const SocialChainDef* inv = socialChain(ct, "invite_follow");
    CHECK(inv && inv->follow);
    CHECK(socialChain(ct, "gather_berries") == nullptr);
    CHECK(ct.interaction("kiss") && ct.interaction("kiss")->targetsCharacter());
}

TEST(lr_sim_social_accept_chance_by_relation) {
    World w = realWorld();
    auto [a, b] = pair(w);
    CHECK(a && b);
    if (!a || !b) return;
    const InteractionDef& kiss = *w.content().interaction("kiss");
    const SocialChainDef& sc = *socialChain(w.content(), "kiss");
    fond(*b, *a, 0.9);
    const double loved = socialAcceptChance(w, *a, *b, kiss, sc, "");
    fond(*b, *a, -0.9);
    const double hated = socialAcceptChance(w, *a, *b, kiss, sc, "");
    CHECK(loved > 0.8);
    CHECK(hated < 0.05);
    // kin and children are never asked
    Character* child = nullptr;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.stage() == "child") child = &c;
    if (child) CHECK_EQ(socialBlock(w, *a, *child, sc), std::string("forbid:child"));
    b->status.mother = a->id;
    CHECK_EQ(socialBlock(w, *a, *b, sc), std::string("forbid:close_kin"));
    b->status.mother = kNone;
    // a stranger to the target
    b->relations[a->id].familiarity = 0.0;
    CHECK_EQ(socialBlock(w, *a, *b, sc), std::string("stranger"));
}

TEST(lr_sim_social_kiss_accepted_both_play_it) {
    World w = realWorld();
    auto [a, b] = pair(w);
    CHECK(a && b);
    if (!a || !b) return;
    const EntityId A = a->id, B = b->id;
    calm(*a);
    calm(*b);
    fond(*b, *a, 1.0);
    fond(*a, *b, 1.0);
    b->pos = {a->pos.x + 3.0, a->pos.y};
    std::string why;
    CHECK(beginSocial(w, *a, *b, *w.content().interaction("kiss"), why));
    CHECK_EQ(w.character(B)->ai.social.role, std::string("target"));
    CHECK_EQ(w.character(B)->ai.fsm.state, std::string("Talk")); // listens
    bool together = false;
    runUntil(w, 8, [&] {
        const Character& x = *w.character(A);
        const Character& y = *w.character(B);
        if (x.ai.social.phase == "stage" && y.ai.fsm.anim == "kiss" && x.ai.fsm.anim == "kiss" &&
            x.ai.fsm.state == "Interact" && y.ai.fsm.state == "Interact")
            together = true;
        return !x.ai.social.active();
    });
    const Character& x = *w.character(A);
    const Character& y = *w.character(B);
    CHECK(together);
    CHECK(traced(x, "social.accepted"));
    CHECK(traced(y, "social.said_yes"));
    CHECK(hasMoodlet(x, "kissed", w.now()));
    CHECK(hasMoodlet(y, "kissed", w.now()));
    CHECK(!y.ai.social.active());
    CHECK(dist(x.pos, y.pos) <= 3.0);
}

TEST(lr_sim_social_refusal_is_the_targets_act) {
    World w = realWorld();
    auto [a, b] = pair(w);
    CHECK(a && b);
    if (!a || !b) return;
    const EntityId A = a->id, B = b->id;
    calm(*a);
    calm(*b);
    fond(*a, *b, 1.0);
    fond(*b, *a, -1.0);
    b->pos = {a->pos.x + 3.0, a->pos.y};
    std::string why;
    CHECK(beginSocial(w, *a, *b, *w.content().interaction("embrace"), why));
    bool refuseShown = false;
    runUntil(w, 6, [&] {
        const Character& y = *w.character(B);
        if (y.ai.social.phase == "refused" && y.ai.fsm.anim == "social_refuse") refuseShown = true;
        return !w.character(A)->ai.social.active();
    });
    const Character& x = *w.character(A);
    CHECK(refuseShown);
    CHECK(traced(x, "social.refused"));
    CHECK(hasMoodlet(x, "rejected", w.now()));
    CHECK(std::any_of(x.memory.begin(), x.memory.end(),
                      [&](const MemoryEntry& m) { return m.type == "was_refused" && !m.who.empty() && m.who[0] == B; }));
    CHECK(!w.character(B)->ai.social.active());
    // An NPC does not ask again straight away.
    CHECK(!beginSocial(w, *w.character(A), *w.character(B), *w.content().interaction("embrace"), why));
    CHECK(why.find("recently") != std::string::npos);
}

TEST(lr_sim_social_player_order_and_player_answer) {
    World w = realWorld();
    auto [a, b] = pair(w);
    CHECK(a && b);
    if (!a || !b) return;
    const EntityId A = a->id, B = b->id;
    calm(*a);
    calm(*b);
    fond(*a, *b, 1.0);
    fond(*b, *a, 1.0);
    b->pos = {a->pos.x + 4.0, a->pos.y};
    // The player kisses B through an ordinary order: the chain runs and the order ends.
    CHECK_EQ(w.applyNow(cmdOf("take_control", {{"id", A}})).result, std::string("ok"));
    CHECK_EQ(w.applyNow(cmdOf("perform", {{"id", A}, {"interaction", "kiss"}, {"target", B}})).result,
             std::string("ok"));
    runUntil(w, 12, [&] { return hasMoodlet(*w.character(B), "kissed", w.now()); });
    CHECK(hasMoodlet(*w.character(B), "kissed", w.now()));
    runUntil(w, 4, [&] { return !w.character(A)->ai.goal.valid(); });
    CHECK(!w.character(A)->ai.goal.valid());
    // Now B (an NPC) asks the player's person: the player answers no.
    w.character(A)->moodlets.clear();
    // a neighbour who came up to talk to A meanwhile (AL-108: one walks up close) is sent off
    if (w.character(A)->ai.social.active()) endSocial(w, *w.character(A), "test");
    w.character(B)->ai.fsm = FsmState{};
    w.character(A)->ai.fsm = FsmState{};
    std::string why;
    CHECK(beginSocial(w, *w.character(B), *w.character(A), *w.content().interaction("embrace"), why));
    runUntil(w, 3, [&] { return w.character(A)->ai.social.phase == "answer"; });
    CHECK_EQ(w.character(A)->ai.social.phase, std::string("answer"));
    CHECK_EQ(w.applyNow(cmdOf("answer_social", {{"id", A}, {"accept", false}})).result, std::string("ok"));
    runUntil(w, 4, [&] { return !w.character(B)->ai.social.active(); });
    CHECK(traced(*w.character(B), "social.refused"));
    CHECK(hasMoodlet(*w.character(B), "rejected", w.now()));
}

TEST(lr_sim_social_intimacy_goes_to_bed) {
    World w = realWorld();
    auto [a, b] = pair(w, false);
    CHECK(a && b);
    if (!a || !b) return;
    const EntityId A = a->id, B = b->id;
    calm(*a);
    calm(*b);
    fond(*a, *b, 1.0);
    fond(*b, *a, 1.0);
    b->status.spouse = A;
    a->status.spouse = B;
    b->pos = {a->pos.x + 3.0, a->pos.y};
    std::string why;
    CHECK(beginSocial(w, *a, *b, *w.content().interaction("intimacy"), why));
    EntityId bed = kNone;
    bool inBed = false;
    runUntil(w, 40, [&] {
        const Character& x = *w.character(A);
        const Character& y = *w.character(B);
        if (x.ai.social.place != kNone) bed = x.ai.social.place;
        if (x.ai.social.phase == "stage" && x.ai.fsm.anim == "intimacy" && y.ai.fsm.anim == "intimacy") inBed = true;
        return !x.ai.social.active();
    });
    const Character& x = *w.character(A);
    const Character& y = *w.character(B);
    CHECK(traced(x, "social.approach"));
    CHECK(bed != kNone);
    CHECK(inBed);
    if (bed != kNone) {
        const WorldObject& o = *w.object(bed);
        CHECK_EQ(o.type, std::string("bed"));
        CHECK(o.ownerFamily == x.status.family || o.ownerFamily == y.status.family);
        CHECK(dist(x.pos, o.pos) <= 3.0);
        CHECK(dist(y.pos, o.pos) <= 3.0);
        CHECK(o.reservedBy != A);
    }
    CHECK(std::any_of(x.memory.begin(), x.memory.end(), [](const MemoryEntry& m) { return m.type == "intimacy"; }));
}

TEST(lr_sim_social_follow_the_player_wasd) {
    World w = realWorld();
    auto [a, b] = pair(w);
    CHECK(a && b);
    if (!a || !b) return;
    const EntityId A = a->id, B = b->id;
    calm(*a);
    calm(*b);
    fond(*b, *a, 1.0);
    b->pos = {a->pos.x + 3.0, a->pos.y};
    CHECK_EQ(w.applyNow(cmdOf("take_control", {{"id", A}})).result, std::string("ok"));
    std::string why;
    CHECK(beginSocial(w, *w.character(A), *w.character(B), *w.content().interaction("invite_follow"), why));
    runUntil(w, 6, [&] { return w.character(B)->ai.social.role == "follower"; });
    CHECK_EQ(w.character(B)->ai.social.role, std::string("follower"));
    CHECK(!w.character(A)->ai.social.active());
    // WASD: twenty steps of 20 m; B keeps a step behind after each.
    const double followM = socialRules(w.content())->followM;
    double worst = 0.0;
    for (int i = 0; i < 20; ++i) {
        const Vec2 p = w.character(A)->pos;
        CHECK_EQ(w.applyNow(cmdOf("steer", {{"id", A}, {"x", p.x + 20.0}, {"y", p.y}})).result, std::string("ok"));
        worst = std::max(worst, dist(w.character(A)->pos, w.character(B)->pos));
    }
    CHECK(worst <= followM + 1e-6);
    // The world goes on: B still follows and does not wander off to its own plan.
    for (int i = 0; i < 6; ++i) w.step();
    CHECK_EQ(w.character(B)->ai.social.role, std::string("follower"));
    CHECK(dist(w.character(A)->pos, w.character(B)->pos) <= followM + 1e-6);
    // Let go: B's own mind takes over.
    CHECK_EQ(w.applyNow(cmdOf("dismiss_follower", {{"id", A}})).result, std::string("ok"));
    CHECK(!w.character(B)->ai.social.active());
    CHECK_EQ(w.applyNow(cmdOf("dismiss_follower", {{"id", A}})).result, std::string("rejected:no_followers"));
}

TEST(lr_sim_social_follower_joins_the_leader) {
    World w = realWorld();
    auto [a, b] = pair(w);
    CHECK(a && b);
    if (!a || !b) return;
    calm(*a);
    calm(*b);
    // An accepted invitation by an NPC leader.
    b->ai.social = SocialLink{};
    b->ai.social.chain = "invite_follow";
    b->ai.social.role = "follower";
    b->ai.social.partner = a->id;
    b->ai.social.phase = "follow";
    b->ai.social.expires = w.now() + 30;
    b->pos = {a->pos.x + 2.0, a->pos.y};
    // The leader sits down to rest; the follower rests too.
    const InteractionDef& rest = *w.content().interaction("rest");
    a->ai.fsm = FsmState{rest.fsm, rest.id, kNone, a->pos, rest.duration, w.now()};
    a->ai.nextThink = w.now() + 100;
    std::string why;
    CHECK(tickSocial(w, *b, why) == SocialStep::Running);
    CHECK_EQ(b->ai.social.phase, std::string("join"));
    CHECK_EQ(b->ai.fsm.interaction, std::string("rest"));
    CHECK(hasMoodlet(*b, "good_company", w.now()));
    // The invitation runs out: B goes back to its own life.
    b->ai.social.expires = w.now();
    b->ai.social.phase = "follow";
    CHECK(tickSocial(w, *b, why) == SocialStep::Running);
    CHECK(!b->ai.social.active());
    CHECK_EQ(why, std::string("invitation_over"));
}

TEST(lr_sim_social_target_steered_away_breaks_it) {
    World w = realWorld();
    auto [a, b] = pair(w);
    CHECK(a && b);
    if (!a || !b) return;
    const EntityId A = a->id, B = b->id;
    calm(*a);
    calm(*b);
    fond(*b, *a, 1.0);
    b->pos = {a->pos.x + 3.0, a->pos.y};
    CHECK_EQ(w.applyNow(cmdOf("take_control", {{"id", B}})).result, std::string("ok"));
    std::string why;
    CHECK(beginSocial(w, *w.character(A), *w.character(B), *w.content().interaction("embrace"), why));
    const Vec2 p = w.character(B)->pos;
    CHECK_EQ(w.applyNow(cmdOf("steer", {{"id", B}, {"x", p.x + 10.0}, {"y", p.y}})).result, std::string("ok"));
    CHECK(!w.character(B)->ai.social.active());
    CHECK_EQ(w.character(A)->ai.social.phase, std::string("broken"));
    w.step();
    CHECK(!w.character(A)->ai.social.active());
}

TEST(lr_sim_social_link_saved) {
    World w = realWorld();
    auto [a, b] = pair(w);
    CHECK(a && b);
    if (!a || !b) return;
    calm(*a);
    calm(*b);
    fond(*b, *a, 1.0);
    b->pos = {a->pos.x + 3.0, a->pos.y};
    std::string why;
    CHECK(beginSocial(w, *a, *b, *w.content().interaction("kiss"), why));
    Json j = *b;
    Character back = j.get<Character>();
    CHECK_EQ(back.ai.social.chain, std::string("kiss"));
    CHECK_EQ(back.ai.social.role, std::string("target"));
    CHECK(back.ai.social.partner == a->id);
    CHECK_EQ(back.ai.fsm.anim, b->ai.fsm.anim);
}

TEST(lr_sim_social_village_lives_with_chains) {
    // Ten days of the ordinary world with chains on: nobody is left holding a
    // half-done chain, nobody follows a dead leader, the pairs end.
    World w = realWorld(11);
    int made = 0; // a few sociable people invite others along on their own
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.stage() == "adult" && !c.hasTrait("sociable") && !c.hasTrait("loner") && made < 4) {
            c.traits.push_back("sociable");
            ++made;
        }
    std::map<std::string, int> seen; // chain:role ticks
    for (Tick t = 0; t < kTicksPerDay * 10; ++t) {
        w.step();
        for (const auto& [id, c] : w.characters())
            if (c.ai.social.active()) {
                ++seen[c.ai.social.chain + ":" + c.ai.social.role];
                ++seen["phase:" + c.ai.social.phase];
            }
    }
    CHECK(seen["invite_follow:follower"] > 0); // NPCs invite and follow each other on their own
    CHECK(seen["phase:join"] > 0);              // ...and join what the leader does
    int stale = 0;
    for (const auto& [id, c] : w.characters()) {
        const SocialLink& l = c.ai.social;
        if (!l.active()) continue;
        const Character* p = w.character(l.partner);
        if (!p || !p->body.alive) ++stale;
        else if (l.role == "target" && p->ai.social.partner != id) ++stale;
    }
    CHECK_EQ(stale, 0);
}

// LR-157: an NPC who opens a talk with the player's person asks first (the 2D client
// opens the talk window); the player's yes makes it a talk the player leads, a no or
// silence ends it - the person's own mind never answers for the player.
TEST(lr_sim_social_npc_talk_invites_the_player) {
    World w = realWorld();
    auto [a, b] = pair(w);
    CHECK(a && b);
    if (!a || !b) return;
    const EntityId A = a->id, B = b->id;
    calm(*a);
    calm(*b);
    fond(*a, *b, 0.6);
    fond(*b, *a, 0.6);
    b->pos = {a->pos.x + 2.0, a->pos.y};
    CHECK_EQ(w.applyNow(cmdOf("take_control", {{"id", A}})).result, std::string("ok"));
    const InteractionDef* chat = w.content().interaction("chat");
    const SocialChainDef* sc = socialChain(w.content(), "chat");
    CHECK(chat && sc && sc->talkOnly);
    if (!chat || !sc) return;
    auto ask = [&] {
        if (w.character(A)->ai.social.active()) endSocial(w, *w.character(A), "test");
        w.character(A)->ai.fsm = FsmState{};
        w.character(B)->ai.fsm = FsmState{};
        w.character(B)->memory.clear();
        std::string why;
        CHECK(beginSocial(w, *w.character(B), *w.character(A), *chat, why));
        runUntil(w, 3, [&] { return pendingInvite(*w.character(A)) == B; });
        CHECK_EQ(pendingInvite(*w.character(A)), B);
    };
    // 1. Asked: it waits for the player well past the old answer time, nobody answers for them.
    ask();
    w.run(socialRules(w.content())->answerTicks + 4);
    CHECK_EQ(pendingInvite(*w.character(A)), B);
    // 2. Yes: the talk is the player's to lead; B waits while the player keeps it going.
    CHECK_EQ(w.applyNow(cmdOf("answer_social", {{"id", A}, {"accept", true}})).result, std::string("ok"));
    CHECK_EQ(ledTalkWith(*w.character(A)), B);
    CHECK_EQ(w.character(B)->ai.social.phase, std::string("led"));
    const Tick talk = socialRules(w.content())->playerTalkTicks;
    w.run(talk / 2);
    keepLedTalk(w, *w.character(A));
    w.run(talk / 2 + 2);
    CHECK_EQ(ledTalkWith(*w.character(A)), B);   // kept going by the player
    CHECK(!traced(*w.character(B), "social.stage")); // no talk that plays by itself
    // The player closes the talk: goodbyes, both free.
    CHECK_EQ(w.applyNow(cmdOf("end_talk", {{"id", A}})).result, std::string("ok"));
    runUntil(w, 3, [&] { return !w.character(B)->ai.social.active(); });
    CHECK(ledTalkWith(*w.character(A)) == kNone);
    CHECK(traced(*w.character(B), "social.done"));
    // 3. No: refused.
    ask();
    CHECK_EQ(w.applyNow(cmdOf("answer_social", {{"id", A}, {"accept", false}})).result, std::string("ok"));
    runUntil(w, 4, [&] { return !w.character(B)->ai.social.active(); });
    CHECK(traced(*w.character(B), "social.refused"));
    // 4. Silence: the asker gives up, no talk.
    ask();
    w.run(socialRules(w.content())->playerAnswerTicks + 2);
    CHECK(traced(*w.character(B), "social.unanswered"));
    CHECK(w.character(A)->ai.social.partner != B || !w.character(A)->ai.social.active());
}
