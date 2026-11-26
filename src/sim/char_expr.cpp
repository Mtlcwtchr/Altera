#include "sim/char_expr.hpp"

#include <algorithm>

#include "sim/character.hpp"
#include "sim/content.hpp"

namespace lr::sim {

namespace {

double read(const Character& c, const Character* other, const std::string& n, bool& known) {
    known = true;
    auto pick = [&](const std::map<std::string, double>& m, size_t skip) {
        auto it = m.find(n.substr(skip));
        return it == m.end() ? 0.0 : it->second;
    };
    if (n.rfind("attr.", 0) == 0) return pick(c.attrsNow.empty() ? c.attrs : c.attrsNow, 5); // LR-141: the current value
    if (n.rfind("skill.", 0) == 0) return pick(c.skills, 6);
    if (n.rfind("need.", 0) == 0) return pick(c.needs, 5);
    if (n.rfind("rep.", 0) == 0) return pick(c.reputation, 4);
    if (n.rfind("trait.", 0) == 0)
        return std::find(c.traits.begin(), c.traits.end(), n.substr(6)) != c.traits.end() ? 1.0 : 0.0;
    if (n.rfind("rel.", 0) == 0) {
        if (!other) return 0.0;
        auto it = c.relations.find(other->id);
        return it == c.relations.end() ? 0.0 : it->second.get(n.substr(4));
    }
    if (n == "age") return c.body.ageDays / 365.0;
    // LR-141: the base of a stat, a moodlet held (1/0), the body
    if (n.rfind("attr_base.", 0) == 0) {
        auto it = c.attrs.find(n.substr(10));
        return it == c.attrs.end() ? 0.5 : it->second;
    }
    if (n.rfind("moodlet.", 0) == 0) {
        const std::string id = n.substr(8);
        for (const auto& m : c.moodlets)
            if (m.id == id) return 1.0;
        return 0.0;
    }
    if (n == "body.health") return c.body.health;
    if (n == "body.fatigue") return c.body.fatigue;
    // LR-152: blood 0..1 (combat units over 100), open wounds, the stamina's share of its cap
    if (n == "body.blood") return c.body.blood / 100.0;
    if (n == "body.wounds") return static_cast<double>(c.wounds.size());
    if (n == "body.stamina") return c.body.staminaMax > 0 ? std::max(0.0, c.body.stamina) / c.body.staminaMax : 1.0;
    known = false;
    return 0.0;
}

const bool kPrefixes = [] {
    for (const char* p : {"attr.", "skill.", "trait.", "need.", "rep.", "rel.", "t.", "age", "attr_base.", "moodlet.", "body."})
        registerExprPrefix(p);
    return true;
}();

} // namespace

double CharExprCtx::var(const std::string& name) const {
    bool known = false;
    if (name.rfind("t.", 0) == 0) {
        if (!other) return 0.0;
        const double v = read(*other, &self, name.substr(2), known);
        if (known) return v;
    } else if (name.rfind("arg.", 0) == 0) {
        auto it = args.find(name.substr(4));
        return it == args.end() ? 0.0 : it->second;
    } else {
        const double v = read(self, other, name, known);
        if (known) return v;
    }
    double v = 0.0;
    if (extra && extra(name, v)) return v;
    return 0.0;
}

bool charExprNameKnown(const Content& c, const std::string& name0, std::string* why) {
    std::string n = name0.rfind("t.", 0) == 0 ? name0.substr(2) : name0;
    auto bad = [&](const std::string& what) {
        if (why) *why = "unknown " + what + " in '" + name0 + "'";
        return false;
    };
    auto has = [](const std::vector<std::string>& v, const std::string& x) {
        return std::find(v.begin(), v.end(), x) != v.end();
    };
    if (n.rfind("attr.", 0) == 0) {
        const std::string id = n.substr(5);
        if (c.expansion.attributes.empty() || has(c.expansion.attributes, id)) return true;
        return bad("attribute '" + id + "'");
    }
    if (n.rfind("skill.", 0) == 0) {
        const std::string id = n.substr(6);
        if (c.knowledge.count(id)) return true;
        return bad("skill '" + id + "'");
    }
    if (n.rfind("trait.", 0) == 0) {
        const std::string id = n.substr(6);
        if (c.traits.count(id)) return true;
        return bad("trait '" + id + "'");
    }
    if (n.rfind("need.", 0) == 0) {
        const std::string id = n.substr(5);
        if (c.need(id)) return true;
        return bad("need '" + id + "'");
    }
    if (n.rfind("rel.", 0) == 0) {
        const std::string id = n.substr(4);
        if (has(relAxes(), id)) return true;
        return bad("relation axis '" + id + "'");
    }
    if (n.rfind("moodlet.", 0) == 0) {
        if (c.moodlets.count(n.substr(8))) return true;
        return bad("moodlet '" + n.substr(8) + "'");
    }
    if (n.rfind("attr_base.", 0) == 0) {
        const std::string id = n.substr(10);
        if (c.expansion.attributes.empty() || has(c.expansion.attributes, id)) return true;
        return bad("attribute '" + id + "'");
    }
    if (n == "body.health" || n == "body.fatigue" || n == "body.blood" || n == "body.wounds" || n == "body.stamina")
        return true;
    if (n.rfind("body.", 0) == 0) return bad("body value");
    if (n.rfind("rep.", 0) == 0 || n == "age" || n.rfind("arg.", 0) == 0) return true;
    if (why) why->clear();
    return false; // not ours: the registered prefixes decide
}

} // namespace lr::sim
