#include "client2d/session2d.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <stdexcept>
#include <system_error>

#include "sim/ai.hpp"
#include "sim/building.hpp"
#include "sim/charcreate.hpp"
#include "sim/species.hpp"
#include "sim/worldgen.hpp"

namespace lr::client2d {
using namespace lr::sim;
namespace fs = std::filesystem;

namespace {

const char* kSeasons[] = {"spring", "summer", "autumn", "winter"};

std::string personName(const ViewText& t, const Character& c) {
    if (!t.loc) return c.name;
    const Json n = t.loc->personNoun(t.lang, c);
    return n.is_object() ? n.value("nom", c.name) : c.name;
}

std::string uiText(const ViewText& t, const std::string& key) {
    return t.loc && t.loc->has(t.lang, key) ? t.loc->text(t.lang, key) : key;
}

bool writeFile(const fs::path& path, const std::string& text, std::string* why) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (f) f.write(text.data(), std::streamsize(text.size()));
        if (!f) {
            if (why) *why = "cannot write " + tmp.string();
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        if (why) *why = "cannot replace " + path.string() + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

Json readJson(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + path.string());
    return Json::parse(f);
}

fs::path utf8Path(const char* s) { return fs::path(reinterpret_cast<const char8_t*>(s)); }

fs::path slotFile(const fs::path& dir, int slot) { return dir / ("slot_" + std::to_string(slot) + ".json"); }
fs::path metaFile(const fs::path& dir, int slot) { return dir / ("slot_" + std::to_string(slot) + ".meta.json"); }

bool validSlot(int slot) { return slot >= 0 && slot <= kSlots2d; }

} // namespace

NewGame NewGame::fromJson(const Json& j, std::uint64_t fallbackSeed) {
    NewGame g;
    g.seed = fallbackSeed;
    if (j.contains("seed")) {
        const Json& s = j["seed"];
        if (s.is_number_unsigned() || s.is_number_integer()) g.seed = s.get<std::uint64_t>();
        else if (s.is_string() && !s.get<std::string>().empty()) {
            // digits are the seed itself; any other word is hashed (a seed can be a name)
            const std::string str = s.get<std::string>();
            if (std::all_of(str.begin(), str.end(), [](unsigned char ch) { return std::isdigit(ch); }) && str.size() < 19)
                g.seed = std::stoull(str);
            else g.seed = hashStr(str);
        }
    }
    const std::string size = j.value("size", g.size);
    for (const auto& s : worldSizes())
        if (s["id"] == size) g.size = size;
    const std::string season = j.value("season", std::string());
    for (const char* s : kSeasons)
        if (season == s) g.season = season;
    return g;
}

const Json& worldSizes() {
    // the sim's map is 8..16 km across (worldgen clamps size_m); the village grows with it
    static const Json sizes = Json::array({
        {{"id", "small"}, {"size_m", 8000}, {"families", 3}, {"people", {3, 5}}},
        {{"id", "medium"}, {"size_m", 12000}, {"families", 4}, {"people", {4, 6}}},
        {{"id", "large"}, {"size_m", 16000}, {"families", 6}, {"people", {4, 7}}},
    });
    return sizes;
}

Json newGameOptions(const Content& c, const ViewText& t, const NewGame& d) {
    Json sizes = Json::array();
    for (const auto& s : worldSizes()) {
        const std::string id = s["id"];
        Json e = s;
        e["name"] = uiText(t, "ui2d.ng.size." + id);
        e["desc"] = uiText(t, "ui2d.ng.size." + id + ".desc");
        sizes.push_back(e);
    }
    Json seasons = Json::array();
    for (const char* s : kSeasons) seasons.push_back({{"id", s}, {"name", uiText(t, std::string("ui2d.season.") + s)}});
    Json backs = Json::array();
    for (const BackstoryDef& b : c.backstories) {
        Json skills = Json::array();
        for (const auto& [k, lvl] : b.skills) skills.push_back({{"name", t.name("knowledge", k)}, {"v", lvl}});
        Json traits = Json::array();
        for (const auto& tr : b.traitsAlways) traits.push_back(t.name("trait", tr));
        Json gear = Json::array();
        for (const auto& [it, n] : b.items) gear.push_back({{"id", it}, {"name", t.name("item", it)}, {"n", n}});
        backs.push_back({{"id", b.id},
                         {"icon", b.icon},
                         {"name", uiText(t, "backstory." + b.id + ".name")},
                         {"desc", uiText(t, "backstory." + b.id + ".desc")},
                         {"start", b.start},
                         {"skills", skills},
                         {"traits", traits},
                         {"gear", gear},
                         {"coins", b.coins}});
    }
    return {{"sizes", sizes}, {"seasons", seasons}, {"backstories", backs}, {"defaults", d.toJson()}, {"create", creationJson(c, t)}};
}

Json creationJson(const Content& c, const ViewText& t) {
    // AL-136: the rules of sim/charcreate with the names in the player's language
    const std::string culture = c.peoples.mapCulture;
    Json r = creationRules(c, culture);
    auto desc = [&](const std::string& key) { return t.loc && t.loc->has(t.lang, key) ? t.loc->text(t.lang, key) : std::string(); };
    r["culture"] = {{"id", culture}, {"name", t.name("culture", culture)}};
    for (auto& e : r["races"]) {
        const std::string id = e["id"];
        e["name"] = t.name("species", id);
        e["desc"] = desc("species." + id + ".desc");
        for (auto& a : e["aptitudes"]) a["name"] = t.name("knowledge", a["skill"].get<std::string>());
        Json aff = Json::array();
        const Json affinity = e.value("affinity", Json::object());
        for (const auto& [asp, v] : affinity.items())
            aff.push_back({{"id", asp}, {"name", t.name("aspect", asp)}, {"v", v}});
        e["affinity"] = aff;
    }
    auto names = [&](Json& list) {
        for (auto& x : list) {
            const std::string id = x["id"];
            x["name"] = t.name("trait", id);
            x["desc"] = desc("trait." + id + ".desc");
        }
    };
    names(r["talents"]);
    names(r["flaws"]);
    Json nature = Json::array();
    for (const auto& id : r["nature"]) nature.push_back({{"id", id}, {"name", t.name("trait", id.get<std::string>())}, {"desc", desc("trait." + id.get<std::string>() + ".desc")}});
    r["nature"] = nature;
    const std::string gift = r["gift"]["trait"];
    r["gift"]["name"] = t.name("trait", gift);
    r["gift"]["desc"] = desc("trait." + gift + ".desc");
    Json attrs = Json::array();
    for (const auto& id : r["attrs"]["ids"]) {
        const std::string a = id;
        attrs.push_back({{"id", a}, {"name", t.name("attr", a)}, {"desc", desc("attr." + a + ".desc")}});
    }
    r["attrs"]["list"] = attrs;
    // the backstories' attribute shifts and fixed traits by id (the screen shows them over the choice)
    Json backs = Json::object();
    for (const BackstoryDef& b : c.backstories) backs[b.id] = {{"attrs", b.attrs}, {"traits", b.traitsAlways}};
    r["backstory"] = backs;
    return r;
}

Json worldConfig(const Json& base, const NewGame& g) {
    Json cfg = base;
    for (const auto& s : worldSizes())
        if (s["id"] == g.size) {
            cfg["size_m"] = s["size_m"];
            cfg["families"] = s["families"];
            cfg["people_per_family"] = s["people"];
        }
    if (!g.season.empty()) {
        cfg.erase("start_day");
        cfg["season"] = g.season;
    }
    cfg["seed"] = g.seed;
    if (!cfg.contains("village_site")) cfg["village_plan"] = true;   // roads, lots, houses of pieces
    return cfg;
}

std::unique_ptr<World> makeWorld2d(const Content& c, const Json& base, const NewGame& g, double runHours) {
    auto w = std::make_unique<World>(makeTestWorld(c, worldConfig(base, g), g.seed));
    installAi(*w);
    if (runHours > 0) w->run(Tick(runHours * kTicksPerDay / 24));
    return w;
}

Json dynastiesJson(const World& w, const ViewText& t) {
    // World has no const view of the dynasties; this only reads them
    auto& dyn = const_cast<World&>(w).dynasties();
    std::vector<const Settlement*> order;
    for (const auto& [id, s] : w.settlements()) order.push_back(&s);
    std::stable_sort(order.begin(), order.end(), [](const Settlement* a, const Settlement* b) { return a->full() && !b->full(); });
    Json out = Json::array();
    for (const Settlement* s : order) {
        const EntityId chief = mandateHolder(w, s->id);
        for (EntityId fid : s->families) {
            const auto fit = w.families().find(fid);
            if (fit == w.families().end()) continue;
            const Family& f = fit->second;
            const auto dit = dyn.find(f.dynasty);
            Json people = Json::array();
            for (EntityId pid : f.members) {
                const Character* c = w.character(pid);
                if (!c || c->isAnimal() || !c->body.alive) continue;
                people.push_back({{"id", pid},
                                  {"name", personName(t, *c)},
                                  {"sex", std::string(1, c->body.sex)},
                                  {"age", c->body.ageDays / kDaysPerYear},
                                  {"role", c->status.role},
                                  {"occ", c->occupation},
                                  {"adult", c->body.stage() != "child"},
                                  {"head", c->status.role == "head"},
                                  {"chief", pid == chief},
                                  {"pl", c->controller.isPlayer()}});
            }
            if (people.empty()) continue;
            out.push_back({{"id", fid},
                           {"name", f.name},
                           {"dynasty", dit != dyn.end() ? dit->second.name : f.name},
                           {"settlement", s->name},
                           {"full", s->full()},
                           {"people", people}});
        }
    }
    return out;
}

std::string beginAs(World& w, const Json& args, int player) {
    Command cmd;
    cmd.source = "player:" + std::to_string(player);
    if (args.value("watch", false)) {
        // nobody led: release whoever the player holds
        for (auto& [id, c] : w.characters())
            if (c.controller.isPlayer() && c.controller.playerId == player) {
                cmd.type = "release_control";
                cmd.args = {{"player", player}};
                const Command r = w.applyNow(cmd);
                return r.result.empty() ? "ok" : r.result;
            }
        return "ok";
    }
    // LR-138: the start of a game: whom to be is chosen anew, and with it the house
    forgetHouse(w, player);
    const std::string back = args.value("backstory", std::string());
    if (args.contains("create") && args["create"].is_object()) {
        // AL-136: a character made on the creation screen
        cmd.type = "create_character";
        cmd.args = args["create"];
        cmd.args["player"] = player;
    } else if (!back.empty()) {
        cmd.type = "start_backstory";
        cmd.args = {{"backstory", back}, {"player", player}};
        if (args.value("id", kNone) != kNone) cmd.args["id"] = args["id"];
    } else {
        const EntityId id = args.value("id", kNone);
        if (id == kNone) return "rejected:nobody";
        cmd.type = "take_control";
        cmd.args = {{"id", id}, {"player", player}};
    }
    const Command r = w.applyNow(cmd);
    return r.result.empty() ? "ok" : r.result;
}

// --- saves -------------------------------------------------------------------
fs::path saveDir2d() {
    if (const char* d = std::getenv("LR_SAVE_DIR"); d && *d) return utf8Path(d) / "saves_2d";
#ifdef _WIN32
    if (const char* a = std::getenv("APPDATA"); a && *a) return fs::path(a) / "LivelyRealms" / "LivelyRealms" / "saves_2d";
#else
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) return utf8Path(x) / "LivelyRealms" / "LivelyRealms" / "saves_2d";
    if (const char* h = std::getenv("HOME"); h && *h) return utf8Path(h) / ".local" / "share" / "LivelyRealms" / "LivelyRealms" / "saves_2d";
#endif
    return fs::current_path() / "saves_2d";
}

Json listSaves2d(const fs::path& dir) {
    Json out = Json::array();
    for (int s = 0; s <= kSlots2d; ++s) {
        Json m = {{"slot", s}, {"used", false}};
        std::error_code ec;
        if (fs::exists(slotFile(dir, s), ec) && fs::exists(metaFile(dir, s), ec)) {
            try {
                const Json j = readJson(metaFile(dir, s));
                for (const char* k : {"saved_at", "tick", "day", "season", "person", "family", "alive", "seed", "size", "backstory"})
                    if (j.contains(k)) m[k] = j[k];
                m["used"] = true;
            } catch (const std::exception& e) {
                m["error"] = e.what();
            }
        }
        out.push_back(m);
    }
    return out;
}

bool writeSave2d(const fs::path& dir, int slot, const World& w, const Json& game, std::string* why, const ViewText* t) {
    const int player = t ? t->player : 1;   // LR-160: the person of the one who saves
    if (!validSlot(slot)) {
        if (why) *why = "bad slot";
        return false;
    }
    Json meta = {{"format", "altera.save.meta/1"},
                 {"client", "2d"},
                 {"slot", slot},
                 {"saved_at", std::int64_t(std::time(nullptr))},
                 {"tick", w.now()},
                 {"day", w.day() + 1},
                 {"season", w.season()},
                 {"seed", game.value("seed", Json(nullptr))},
                 {"size", game.value("size", std::string())}};
    int alive = 0;
    for (const auto& [id, c] : w.characters()) {
        if (c.isAnimal()) continue;
        if (c.body.alive) ++alive;
        if (c.controller.isPlayer() && c.controller.playerId == player) {
            meta["person"] = t ? personName(*t, c) : c.name;
            meta["backstory"] = c.backstory;
            if (const Family* f = w.familyOf(c)) meta["family"] = f->name;
        }
    }
    meta["alive"] = alive;
    const Json save = {{"format", "altera.save/1"}, {"client", "2d"}, {"game", game}, {"world", w.toJson()}};
    // the world first: a meta file without its world is never listed
    return writeFile(slotFile(dir, slot), save.dump(), why) && writeFile(metaFile(dir, slot), meta.dump(1), why);
}

Json readSave2d(const fs::path& dir, int slot) {
    if (!validSlot(slot)) throw std::runtime_error("bad slot");
    Json j = readJson(slotFile(dir, slot));
    if (j.value("format", "") != "altera.save/1" || !j.contains("world")) throw std::runtime_error("not an altera.save/1 file");
    return j;
}

std::unique_ptr<World> loadWorld2d(const Content& c, const Json& save) {
    const Json& wj = save.at("world");
    auto w = std::make_unique<World>(c, wj.value("seed", std::uint64_t{1}));
    installAi(*w);
    w->loadJson(wj);
    return w;
}

bool deleteSave2d(const fs::path& dir, int slot) {
    if (!validSlot(slot)) return false;
    std::error_code ec;
    const bool a = fs::remove(slotFile(dir, slot), ec);
    fs::remove(metaFile(dir, slot), ec);
    return a;
}

} // namespace lr::client2d
