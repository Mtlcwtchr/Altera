#pragma once
// News and things worth telling (AL-111): what people talk about.
//
// The truth is the world's chronicle (World::chronicleLog): every death,
// wedding, birth, raid, war, battle, verdict is a ChronicleEntry with a loc
// code and its params. A piece of news is such an entry; nothing new is made
// up. What this module adds is who knows it and how (content/ai/news.json):
//   - the people in it (ChronicleEntry::who) saw it;
//   - scope "family": their households heard it at once;
//   - scope "village": the whole settlement hears it within a few days
//     (each person on his own day, by a hash of the person and the entry);
//   - scope "realm": word from beyond the village reaches those who travel or
//     listen (traders, tavernkeepers, heads of households, the nobility, the
//     curious) within days, a few others later, the rest only by mouth;
//   - by mouth: a person told it (say act tell_news) remembers it as
//     MemoryEntry "heard_news" (object = entry index + 1, chain = who told it,
//     confidence falls with each retelling, far enough it is a rumour).
// Talk then has content: in a chat NPCs tell each other the news the other
// does not know (dialogueTopics brings it up), and the player can ask
// (askTopics): what's new, about yourself, about a person, about a place;
// answerAsk() answers from the answerer's knowledge, memory and feelings.
//
// Lines are rules (content/ai/dialogue_rules/news.json) over the facts this
// module gives (news.*, person.*, place.*, story.*) and loc keys news.* /
// say.news.* - the news itself is the chronicle text of the entry ({news},
// {news_lc} with a lower-case first letter). Deterministic, no RNG state.

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "sim/dialogue_rules.hpp"
#include "sim/dialogue_say.hpp"

namespace lr::sim {

class World;
class Loc;
struct Character;
struct ChronicleEntry;
struct Content;

struct NewsKind {
    std::string kind = "none"; // death, wedding, raid, war, ... (news.kind of the lines)
    std::string mood = "neutral"; // sad | glad | alarm | spicy | neutral
    std::string scope = "village"; // who | family | village | realm
    double interest = 0.0;      // 0 = not news
    std::vector<std::string> tags;
};

struct NewsRules {
    int maxAgeDays = 45;        // older than this is not news any more
    int storyAgeDays = 7200;    // a person's own past he still tells about
    double halfLifeDays = 12.0;
    double villageDelay0 = 0.0, villageDelay1 = 2.0;
    double realmDelay0 = 1.0, realmDelay1 = 10.0;
    double realmReach = 0.3;    // share of the others who hear realm news by themselves
    std::set<std::string> earlyOccupations, earlyRoles, earlyTraits, earlyLife;
    double seenConf = 1.0, villageConf = 0.9, realmConf = 0.7, toldConf = 0.9, rumorBelow = 0.6;
    double mineMult = 1.4;
    size_t maxHeard = 40;
    double declineAffinity = -0.45, declineEnmity = 0.5;
    std::set<std::string> disabledTags;
    std::map<std::string, NewsKind> types, codes, prefixes;
    // The kind of an entry: codes, then the longest prefix, then the type; null = not news.
    const NewsKind* of(const ChronicleEntry& e) const;
    static NewsRules fromJson(const Json& j);
};

// content/ai/news.json (cached per content dir; defaults without it: no news).
const NewsRules& newsRules(const Content& c);

struct NewsItem {
    size_t index = 0;           // in World::chronicleLog()
    NewsKind kind;
    Epistemic epi;
    double weight = 0.0;        // interest x freshness x sureness (x mine)
    bool mine = false;          // the knower or his kin is in it
};

// Whether `c` knows chronicle entry `index`, and how (seen, heard, told, rumor).
std::optional<Epistemic> newsKnown(const World& w, const Character& c, size_t index);
// The news `c` knows, freshest and weightiest first (maxAgeDays < 0: the rules' max).
std::vector<NewsItem> knownNews(const World& w, const Character& c, size_t max = 8, int maxAgeDays = -1);
// What `teller` knows and `hearer` does not, worth telling, strongest first.
std::vector<NewsItem> newsToTell(const World& w, const Character& teller, const Character& hearer, size_t max = 3);
// What `teller` knows about a person or a settlement (`about` in the entry), news first.
std::vector<NewsItem> newsAbout(const World& w, const Character& teller, EntityId about, size_t max = 2);
// The person's own past he tells about (entries with him in them, any age within story_age_days).
std::vector<NewsItem> lifeStory(const World& w, const Character& c, size_t max = 2);
// The goal input news:fresh (goals.json share_news): how much `c` has to tell, the weight of
// the strongest news of the last days he did not take part in himself (0 = nothing).
double freshNewsWeight(const World& w, const Character& c);
// Whether `id` takes part in entry `e` (who, or a person/place param).
bool newsInvolves(const ChronicleEntry& e, EntityId id);

// `hearer` is told entry `index` by `teller` who knows it as `tellerEpi`.
// False when the hearer knew it already.
bool learnNews(World& w, Character& hearer, const Character& teller, size_t index, const Epistemic& tellerEpi);

// The argument of a line about entry `index` (Utterance::args["news"]).
Ref newsRef(size_t index);
// The entry a Ref points to (nullopt: not a news ref, or out of range).
std::optional<size_t> newsIndex(const World& w, const Ref& r);

// The chronicle text of an entry (lowerFirst: for the middle of a sentence,
// unless it starts with a name).
std::string newsText(const Loc& loc, const std::string& lang, const World& w, size_t index, bool lowerFirst = false);

// say(): an Utterance that told news teaches the listener (called for every said line).
void noteSaid(World& w, const Utterance& u, const std::string& rule);
// utteranceArgs(): {news}, {news_lc}, {person_trade}, {place}, {place_dir}, {place_size},
// {place_km}, {ruler}, {self_trade}, {years}, {children} of the lines.
void addNewsArgs(const Loc& loc, const std::string& lang, const World& w, const Utterance& u, Json& args);
// Registers the facts news.*, person.*, place.*, story.* (say() calls it once).
void installNewsFacts();

// --- the player's questions ------------------------------------------------------
// What `asker` can ask `answerer`: ask_news, ask_self, ask_person {person}, ask_place {place}.
std::vector<MenuTopic> askTopics(const World& w, const Character& asker, const Character& answerer, size_t maxPeople = 3,
                                 size_t maxPlaces = 3);
// The news `speaker` can tell `listener` as menu topics (act tell_news, args news).
std::vector<MenuTopic> newsTopics(const World& w, const Character& speaker, const Character& listener, size_t max = 3);
// `asker` asks (his line) and `answerer` answers in one to three lines. Returns how
// many lines the answerer said.
int askAndAnswer(World& w, Character& asker, Character& answerer, const MenuTopic& ask);
// A beat of a chat with nothing new to tell (social_chain talkBeat): `talk` chat | gossip -
// the fresh news both know is talked over (discuss_news, the other answers react_news);
// story - the teller tells of his own past (tell_story). `turn` picks among them. False:
// nothing of the kind, the beat goes on with small talk.
bool talkOverNews(World& w, Character& teller, Character& hearer, const std::string& talk, int turn);
// Whether `answerer` will not talk to `asker` (too hostile).
bool declinesToAnswer(const World& w, const Character& answerer, const Character& asker);

} // namespace lr::sim

namespace lr::sim {
// --- the dialogue corpus and line tests (dialogue_corpus.cpp, test_lr_sim_dialogue_lines) ---
// Whether a fact key is this module's (news*, person*, place*, story.*).
bool newsFactKey(const std::string& key);
// For a corpus moment of `act` aimed at `target` (may be null): the facts the act's
// lines rest on, set so the target's criteria on this module's keys hold.
void sampleNewsFacts(const std::string& act, const DialogueRuleDef* target, ContextFacts& f);
// The loc arguments of this module's lines for a sample moment: {news} is a real
// chronicle line about args.person / args.src, {place} a sample settlement, ...
void sampleNewsArgs(const Loc& loc, const std::string& lang, Json& args);
} // namespace lr::sim
