#include "client2d/talk2d.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "sim/dialogue_say.hpp"
#include "sim/social_chain.hpp"
#include "sim/tones.hpp"
#include "sim/trade.hpp"

namespace altera::client2d {

using namespace altera::sim;

namespace {

double r2(double v) { return std::round(v * 100) / 100; }

std::string text(const ViewText& t, const std::string& key, const Json& args = Json::object()) {
    return t.loc && t.loc->has(t.lang, key) ? t.loc->text(t.lang, key, args) : "";
}

std::string clockOf(Tick tick) {
    const int minutes = int((tick % kTicksPerDay) * (24 * 60) / kTicksPerDay);
    char b[16];
    std::snprintf(b, sizeof b, "%02d:%02d", minutes / 60, minutes % 60);
    return b;
}

// What the person is in the village, said short: the trade role, else the head of a household.
std::string roleText(const ViewText& t, const Character& c) {
    if (!c.occupation.empty())
        if (std::string s = text(t, "trade.role." + c.occupation); !s.empty()) return s;
    if (!c.status.role.empty())
        if (std::string s = text(t, "ui2d.role." + c.status.role); !s.empty()) return s;
    return "";
}

// The name in the page's language with the cases a window's captions need
// ("Разговор с Томасом", "Отношение Томаса к вам").
Json sideJson(const World& w, const ViewText& t, const Character& c, const Character& other) {
    const Json n = t.loc ? t.loc->personNoun(t.lang, c) : Json();
    auto form = [&](const char* k) { return n.is_object() ? n.value(k, c.name) : c.name; };
    return {{"id", c.id},          {"name", form("nom")},        {"name_ins", form("ins")}, {"name_gen", form("gen")},
            {"role", roleText(t, c)}, {"vy", addressedVy(w, other.id, c.id)}};
}

std::string goodsText(const ViewText& t, const Goods& g) {
    std::string out;
    for (const auto& [it, n] : g) out += (out.empty() ? "" : ", ") + t.name("item", it) + " ×" + std::to_string(n);
    return out;
}

} // namespace

TalkCatalog TalkCatalog::make(const Json& wheel) {
    TalkCatalog c;
    if (!wheel.is_object()) return c;
    for (const auto& sp : wheel.value("spheres", Json::array())) {
        Sphere s;
        s.id = sp.value("id", std::string());
        for (const auto& sub : sp.value("subs", Json::array()))
            for (const auto& id : sub.value("interactions", Json::array())) s.acts.push_back(id.get<std::string>());
        if (!s.acts.empty()) c.spheres.push_back(std::move(s));
    }
    return c;
}

Json talkJson(World& w, const ViewText& t, ActCatalog& acts, TalkCatalog& cat, EntityId meId, EntityId targetId,
              const std::string& toneIn) {
    Character* me = w.character(meId);
    Character* them = w.character(targetId);
    if (!me || !them || me == them || them->isAnimal() || !t.loc) return {{"ok", false}};
    Json j = {{"ok", true}, {"me", sideJson(w, t, *me, *them)}, {"them", sideJson(w, t, *them, *me)}};
    j["alive"] = them->body.alive;

    // how they feel about the player's person
    Json rel = Json::object();
    const auto ri = them->relations.find(me->id);
    for (const char* axis : {"affinity", "trust", "fear", "respect", "familiarity", "enmity"})
        rel[axis] = ri == them->relations.end() ? 0.0 : r2(ri->second.get(axis));
    j["rel"] = rel;
    j["rel_names"] = Json::object();
    for (const auto& [axis, v] : rel.items()) j["rel_names"][axis] = text(t, "hud.axis." + axis);

    // the lines of the two, oldest first (each is in my trace once: mine as the speaker's copy, theirs as the listener's)
    std::vector<Json> lines;
    for (auto it = me->ai.trace.rbegin(); it != me->ai.trace.rend() && lines.size() < 14; ++it) {
        if (it->code != "say.line") continue;
        const EntityId sp = it->params.value("speaker", kNone), li = it->params.value("listener", kNone);
        if (!((sp == me->id && li == them->id) || (sp == them->id && li == me->id))) continue;
        std::string line;
        Utterance u;
        if (it->params.contains("utt") && fromJson(it->params["utt"], u))
            line = utteranceText(*t.loc, t.lang, w, u, it->params.value("key", std::string()));
        if (line.empty()) line = lineText(*t.loc, t.lang, w, it->params);
        if (line.empty()) continue;
        lines.push_back({{"t", clockOf(it->tick)}, {"tick", it->tick}, {"who", sp}, {"text", line}});
    }
    std::reverse(lines.begin(), lines.end());
    j["lines"] = lines;
    // what they mutter aside (presentation only, not recorded)
    if (them->body.alive) {
        const SaidLine b = chooseBark(w, *them);
        if (b.ok()) j["aside"] = utteranceText(*t.loc, t.lang, w, b.utterance, b.key, b.pre, b.post);
    }

    // the topics to bring up
    Json topics = Json::array();
    int i = 0;
    for (const auto& tp : dialogueTopics(w, *me, *them)) {
        topics.push_back({{"i", i++},
                          {"act", tp.act},
                          {"verb", text(t, "hud.talk.act." + tp.act)},
                          {"label", menuLabel(*t.loc, t.lang, w, *me, *them, tp)},
                          {"rumor", tp.epi.source == "rumor" || tp.epi.source == "heard" || tp.epi.hops() > 0},
                          {"sure", r2(tp.epi.confidence)}});
    }
    j["topics"] = topics;

    // tones
    const ToneRules* tr = toneRules(w.content());
    std::string tone = toneIn.empty() ? me->controller.tone : toneIn;
    Json tones = Json::array();
    if (tr) {
        if (!tr->tones.count(tone)) tone = tr->order.empty() ? "" : tr->order.front();
        for (const auto& id : tr->order) tones.push_back({{"id", id}, {"name", t.name("tone", id)}});
    } else tone.clear();
    j["tones"] = tones;
    j["tone"] = tone;

    // the acts of the wheel by sphere: can / why from the act check, the chance in this tone
    const Json all = actsJson(w, t, acts, me->id, them->id);
    std::map<std::string, const Json*> byId;
    for (const auto& a : all) byId[a.value("id", std::string())] = &a;
    const std::string ck = std::to_string(me->id) + ":" + std::to_string(them->id) + ":" + tone;
    TalkCatalog::Cached& cc = cat.cache[ck];
    if (w.now() - cc.at >= 6 || !cc.chances.is_object()) {
        cc.chances = Json::object();
        cc.at = w.now();
        if (them->body.alive)
            for (const auto& sp : cat.spheres)
                for (const auto& id : sp.acts) {
                    const InteractionDef* d = w.content().interaction(id);
                    if (!d) continue;
                    const ActChance ac = actChance(w, *me, *d, them, tone);
                    cc.chances[id] = {r2(ac.p), ac.kind};
                }
    }
    Json spheres = Json::array();
    for (const auto& sp : cat.spheres) {
        Json list = Json::array();
        for (const auto& id : sp.acts) {
            auto it = byId.find(id);
            if (it == byId.end()) continue;
            Json a = *it->second;
            if (cc.chances.contains(id)) {
                a["chance"] = cc.chances[id][0];
                a["kind"] = cc.chances[id][1];
            }
            list.push_back(std::move(a));
        }
        if (!list.empty()) spheres.push_back({{"id", sp.id}, {"name", text(t, "hud.wheel.sphere." + sp.id)}, {"acts", list}});
    }
    j["spheres"] = spheres;

    // they ask something and wait for the answer
    const SocialLink& l = me->ai.social;
    if (l.active() && l.role == "target" && l.phase == "answer" && l.partner == them->id)
        j["ask"] = {{"act", l.chain},
                    {"name", t.name("interaction", l.chain)},
                    {"tone", l.tone.empty() ? "" : t.name("tone", l.tone)},
                    {"left", std::max<Tick>(0, l.until - w.now())}};
    j["trade"] = tradeRules(w.content()) != nullptr;
    return j;
}

std::string offerMode(const Content& C, const Goods& give, const Goods& take) {
    auto money = [&](const std::string& it) {
        auto d = C.items.find(it);
        return d != C.items.end() && std::find(d->second.tags.begin(), d->second.tags.end(), "money") != d->second.tags.end();
    };
    bool giveCoin = !give.empty(), takeCoin = !take.empty();
    for (const auto& [it, n] : give) giveCoin = giveCoin && money(it);
    for (const auto& [it, n] : take) takeCoin = takeCoin && money(it);
    if (take.empty()) return "gift";
    if (giveCoin) return "buy";
    if (takeCoin) return "sell";
    return "barter";
}

Json tradeJson(const World& w, const ViewText& t, EntityId meId, EntityId targetId, const Json& giveJ, const Json& takeJ) {
    const Character* me = w.character(meId);
    const Character* them = w.character(targetId);
    if (!me || !them || me == them || them->isAnimal() || !t.loc || !tradeRules(w.content())) return {{"ok", false}};
    const Content& C = w.content();
    Json j = {{"ok", true}, {"me", sideJson(w, t, *me, *them)}, {"them", sideJson(w, t, *them, *me)}};
    j["role"] = them->occupation.empty() ? "" : text(t, "trade.role." + them->occupation);
    auto money = [&](const std::string& it) {
        auto d = C.items.find(it);
        return d != C.items.end() && std::find(d->second.tags.begin(), d->second.tags.end(), "money") != d->second.tags.end();
    };
    int coin = 0, theirCoin = 0;
    // each thing as the other side weighs it: the personal price (their worth of one) and their taste
    auto goods = [&](const Character& holder, int& coins) {
        Json out = Json::array();
        for (const auto& [it, n] : holder.inventory) {
            const int k = spare(holder, it);
            if (k <= 0) continue;
            if (money(it)) coins += k;
            const double tv = itemTaste(w, *them, it);
            out.push_back({{"item", it},
                           {"name", t.name("item", it)},
                           {"n", k},
                           {"worth", r2(worthTo(w, *them, it, holder))},
                           {"taste", tv > 0.2 ? 1 : (tv < -0.2 ? -1 : 0)},
                           {"money", money(it)}});
        }
        return out;
    };
    j["mine"] = goods(*me, coin);
    j["theirs"] = goods(*them, theirCoin);
    j["coin"] = coin;
    j["their_coin"] = theirCoin;
    Json likes = Json::array(), dislikes = Json::array();
    for (const auto& [cat, v] : tasteOf(w, *them)) {
        const std::string n = text(t, "trade.cat." + cat);
        if (n.empty()) continue;
        if (v > 0.2) likes.push_back(n);
        else if (v < -0.2) dislikes.push_back(n);
    }
    j["likes"] = likes;
    j["dislikes"] = dislikes;

    Offer o;
    o.give = goodsFrom(C, giveJ.is_object() ? giveJ : Json::object());
    o.take = goodsFrom(C, takeJ.is_object() ? takeJ : Json::object());
    o.mode = offerMode(C, o.give, o.take);
    j["mode"] = o.mode;
    j["mode_name"] = text(t, "hud.trade.mode." + o.mode);
    j["give"] = goodsJson(o.give);
    j["take"] = goodsJson(o.take);
    if (!o.give.empty() || !o.take.empty()) {
        const DealEval e = weighDeal(w, *me, *them, o);
        j["verdict"] = e.verdict;
        j["reason"] = e.reason.empty() ? "" : text(t, "reason.trade_" + e.reason);
        j["gain"] = r2(e.gain);
        j["loss"] = r2(e.loss);
        j["keep"] = r2(e.keep);
        j["ratio"] = r2(e.ratio);
        if (e.verdict == "counter")
            j["their_counter"] = {{"give", goodsJson(e.counter.give)},
                                  {"take", goodsJson(e.counter.take)},
                                  {"text", text(t, "hud.trade.counter_offer",
                                                {{"give", goodsText(t, e.counter.give)}, {"take", goodsText(t, e.counter.take)}})}};
    }
    if (const Json* pc = pendingCounter(w, *them, me->id)) {
        const Goods g = goodsFrom(C, pc->value("give", Json::object())), tk = goodsFrom(C, pc->value("take", Json::object()));
        j["counter"] = {{"give", goodsJson(g)},
                        {"take", goodsJson(tk)},
                        {"text", text(t, "hud.trade.counter_offer", {{"give", goodsText(t, g)}, {"take", goodsText(t, tk)}})}};
    }
    Json last = Json::array();
    for (auto it = me->ai.trace.rbegin(); it != me->ai.trace.rend() && last.size() < 4; ++it)
        if (it->code.rfind("trade.", 0) == 0 && it->code != "trade.weigh")
            last.insert(last.begin(), traceText(*t.loc, t.lang, w, *me, *it));
    j["last"] = last;
    return j;
}

std::string sayTopic(World& w, EntityId meId, EntityId targetId, int index) {
    Character* me = w.character(meId);
    Character* them = w.character(targetId);
    if (!me || !them || me == them) return "no_person";
    const auto topics = dialogueTopics(w, *me, *them);
    if (index < 0 || index >= int(topics.size())) return "no_topic";
    const MenuTopic& tp = topics[std::size_t(index)];
    say(w, *me, *them, tp.act, tp.prop, tp.epi);
    return "";
}

} // namespace altera::client2d
