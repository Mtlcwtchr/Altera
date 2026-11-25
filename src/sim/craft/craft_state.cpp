#include "sim/craft/craft_state.hpp"

namespace altera::sim::craft {

void to_json(Json& j, const ItemFlaw& f) { j = Json{{"id", f.id}, {"revealed", f.revealed}}; }
void from_json(const Json& j, ItemFlaw& f) {
    f.id = j.value("id", "");
    f.revealed = j.value("revealed", false);
}

void to_json(Json& j, const StepRec& s) {
    j = Json{{"process", s.process}, {"slots", s.slots}, {"q", s.q}};
    if (!s.shape.empty()) j["shape"] = s.shape;
    if (!s.mode.empty()) j["mode"] = s.mode;
    if (s.len) j["len"] = s.len;
    if (s.heat >= 0) j["heat"] = s.heat;
    if (!s.outForm.empty()) j["out_form"] = s.outForm;
    if (!s.outShape.empty()) j["out_shape"] = s.outShape;
    if (!s.outMat.empty()) j["out_mat"] = s.outMat;
}
void from_json(const Json& j, StepRec& s) {
    s.process = j.value("process", "");
    s.slots = j.value("slots", std::map<std::string, std::vector<std::string>>{});
    s.q = j.value("q", 0.0);
    s.shape = j.value("shape", "");
    s.mode = j.value("mode", "");
    s.len = j.value("len", 0);
    s.heat = j.value("heat", -1);
    s.outForm = j.value("out_form", "");
    s.outShape = j.value("out_shape", "");
    s.outMat = j.value("out_mat", "");
}

void to_json(Json& j, const Item& it) {
    j = Json{{"material", it.material}, {"p", it.p},       {"shape", it.shape}, {"len", it.len},
             {"thick", it.thick},      {"q", it.q},       {"q_steps", it.qSteps}, {"q_sum", it.qSum},
             {"q_min", it.qMin},       {"tags", it.tags}, {"trace", it.trace}};
    if (it.frac != 1.0) j["frac"] = it.frac;
    if (!it.flaws.empty()) j["flaws"] = it.flaws;
    if (!it.states.empty()) j["states"] = it.states;
    if (!it.effects.empty()) j["effects"] = it.effects;
    if (!it.extracted.empty()) j["extracted"] = it.extracted;
    if (!it.parts.empty()) {
        Json parts = Json::array();
        for (const auto& p : it.parts) parts.push_back(Json{{"role", p.role}, {"item", p.item.front()}});
        j["parts"] = parts;
        j["joint"] = it.joint;
        j["joint_type"] = it.jointType;
    }
    if (it.wear > 0) j["wear"] = it.wear;
    if (it.maker) j["maker"] = it.maker;
    if (it.inscribed) { // AL-117
        j["rune"] = Json{{"q", it.rune.q},           {"conductivity", it.rune.conductivity},
                         {"capacity", it.rune.capacity}, {"aspects", it.rune.aspects},
                         {"trigger", it.rune.trigger},   {"charge", it.rune.charge}};
    }
    if (it.deeds) {
        j["deeds"] = it.deeds;
        j["resonance"] = it.resonance;
        j["history"] = it.history;
    }
    // recognition is recomputed on load (recognize()); the form is kept for lookups
    if (!it.form.empty()) j["form"] = it.form;
}
void from_json(const Json& j, Item& it) {
    it = Item{};
    it.material = j.value("material", "");
    it.p = j.value("p", std::map<std::string, double>{});
    it.shape = j.value("shape", "lump");
    it.len = j.value("len", 1);
    it.thick = j.value("thick", 1);
    it.frac = j.value("frac", 1.0);
    it.q = j.value("q", 0.5);
    it.qSteps = j.value("q_steps", 0);
    it.qSum = j.value("q_sum", 0.0);
    it.qMin = j.value("q_min", 2.0);
    it.flaws = j.value("flaws", std::vector<ItemFlaw>{});
    it.states = j.value("states", std::set<std::string>{});
    it.tags = j.value("tags", std::set<std::string>{});
    it.effects = j.value("effects", std::map<std::string, double>{});
    it.extracted = j.value("extracted", std::set<std::string>{});
    for (const auto& p : j.value("parts", Json::array())) {
        Item::Part pt;
        pt.role = p.value("role", "");
        pt.item.push_back(p.at("item").get<Item>());
        it.parts.push_back(pt);
    }
    it.joint = j.value("joint", 1.0);
    it.jointType = j.value("joint_type", "");
    it.wear = j.value("wear", 0.0);
    if (j.contains("rune")) {
        const Json& r = j["rune"];
        it.inscribed = true;
        it.rune.q = r.value("q", 0.0);
        it.rune.conductivity = r.value("conductivity", 0.0);
        it.rune.capacity = r.value("capacity", 0.0);
        it.rune.aspects = r.value("aspects", std::map<std::string, double>{});
        it.rune.trigger = r.value("trigger", "");
        it.rune.charge = r.value("charge", 0.0);
    }
    it.deeds = j.value("deeds", 0);
    it.resonance = j.value("resonance", 0.0);
    it.history = j.value("history", std::vector<std::string>{});
    it.maker = j.value("maker", std::uint64_t{0});
    it.trace = j.value("trace", std::vector<StepRec>{});
    it.form = j.value("form", "");
}

void to_json(Json& j, const Recipe& r) {
    Json steps = Json::array();
    for (const auto& s : r.steps) {
        Json st{{"process", s.step.process}, {"known", s.known}};
        Json slots = Json::object();
        for (const auto& [slot, ins] : s.step.slots) {
            Json a = Json::array();
            for (const auto& in : ins) a.push_back(in.count == 1 ? Json(in.ref) : Json{{"ref", in.ref}, {"n", in.count}});
            slots[slot] = a;
        }
        st["slots"] = slots;
        if (!s.step.shape.empty()) st["shape"] = s.step.shape;
        if (s.step.len) st["len"] = s.step.len;
        if (!s.step.mode.empty()) st["mode"] = s.step.mode;
        if (s.step.heat >= 0) st["heat"] = s.step.heat;
        if (!s.outForm.empty()) st["out_form"] = s.outForm;
        if (!s.outShape.empty()) st["out_shape"] = s.outShape;
        if (!s.outMat.empty()) st["out_mat"] = s.outMat;
        steps.push_back(st);
    }
    j = Json{{"recipe_id", r.id},  {"name_key", r.nameKey}, {"name_user", r.nameUser}, {"form", r.form},
             {"steps", steps},     {"mastery", r.mastery},  {"successes", r.successes}, {"best_q", r.bestQ},
             {"notes", r.notes},   {"tags", r.tags}};
}
void from_json(const Json& j, Recipe& r) {
    r = Recipe{};
    r.id = j.value("recipe_id", "");
    r.nameKey = j.value("name_key", "");
    r.nameUser = j.value("name_user", "");
    r.form = j.value("form", "");
    r.mastery = j.value("mastery", 0.0);
    r.successes = j.value("successes", 0);
    r.bestQ = j.value("best_q", 0.0);
    r.notes = j.value("notes", std::vector<std::string>{});
    r.tags = j.value("tags", std::vector<std::string>{});
    for (const auto& st : j.value("steps", Json::array())) {
        RecipeStep s;
        s.step.process = st.value("process", "");
        s.known = st.value("known", false);
        s.step.shape = st.value("shape", "");
        s.step.len = st.value("len", 0);
        s.step.mode = st.value("mode", "");
        s.step.heat = st.value("heat", -1);
        s.outForm = st.value("out_form", "");
        s.outShape = st.value("out_shape", "");
        s.outMat = st.value("out_mat", "");
        if (st.contains("slots"))
            for (auto it = st["slots"].begin(); it != st["slots"].end(); ++it)
                for (const auto& x : it.value()) {
                    if (x.is_string()) s.step.slots[it.key()].push_back({x.get<std::string>(), 1});
                    else s.step.slots[it.key()].push_back({x.value("ref", ""), x.value("n", 1)});
                }
        r.steps.push_back(s);
    }
}

void to_json(Json& j, const CraftState& s) {
    j = Json{{"items", s.items}, {"recipes", s.book.recipes}, {"knows_material", s.knowsMaterial},
             {"last_breakthrough", s.lastBreakthrough}, {"tried", s.tried}};
}
void from_json(const Json& j, CraftState& s) {
    s = CraftState{};
    s.items = j.value("items", std::vector<Item>{});
    s.book.recipes = j.value("recipes", std::vector<Recipe>{});
    s.knowsMaterial = j.value("knows_material", std::map<std::string, double>{});
    s.lastBreakthrough = j.value("last_breakthrough", std::int64_t{-1});
    s.tried = j.value("tried", std::map<std::string, int>{});
}

} // namespace altera::sim::craft
