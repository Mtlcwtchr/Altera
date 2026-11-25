#include "sim/craft/craft_magic.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

namespace altera::sim::craft {

namespace {
double clamp01(double v) { return std::min(1.0, std::max(0.0, v)); }
const char* suffixOf(char vec) { return vec == '-' ? ".minus" : (vec == '~' ? ".shift" : ""); }
} // namespace

bool parseRuneKey(const std::string& key, std::string& aspect, char& vec) {
    vec = '+';
    aspect = key;
    auto dot = key.find('.');
    if (dot != std::string::npos) {
        const std::string v = key.substr(dot + 1);
        aspect = key.substr(0, dot);
        if (v == "minus") vec = '-';
        else if (v == "shift") vec = '~';
        else return false;
    }
    return !aspect.empty();
}

bool parseAspectEffect(const std::string& effect, std::string& aspect, char& vec) {
    static const std::string head = "aspect.";
    if (effect.rfind(head, 0) != 0) return false;
    return parseRuneKey(effect.substr(head.size()), aspect, vec);
}

std::string runeKey(const std::string& aspect, char vec) { return aspect + suffixOf(vec); }
std::string aspectEffectId(const std::string& aspect, char vec) { return "aspect." + runeKey(aspect, vec); }

std::vector<AspectDose> aspectDoses(const Item& it) {
    std::vector<AspectDose> out;
    for (const auto& [e, k] : it.effects) {
        AspectDose a;
        if (k > 0 && parseAspectEffect(e, a.aspect, a.vec)) {
            a.power = k;
            out.push_back(a);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const AspectDose& a, const AspectDose& b) { return a.power > b.power; });
    return out;
}

double aspectTotal(const Item& it) {
    double s = 0.0;
    for (const auto& a : aspectDoses(it)) s += a.power;
    return s;
}

double instabilityOf(const CraftData& d, const std::vector<AspectDose>& doses) {
    auto opposed = [&](const AspectDose& a, const AspectDose& b) {
        if (a.aspect == b.aspect) return a.vec != b.vec && a.vec != '~' && b.vec != '~';
        auto has = [&](const AspectDose& x, const AspectDose& y) {
            const AspectInfo* ai = d.aspect(x.aspect);
            return ai && std::find(ai->opposes.begin(), ai->opposes.end(), y.aspect) != ai->opposes.end();
        };
        // fire+ against water+ fights; fire+ against water- (drying) goes along
        return (has(a, b) || has(b, a)) && a.vec == b.vec;
    };
    double s = 0.0;
    for (size_t i = 0; i < doses.size(); ++i)
        for (size_t j = i + 1; j < doses.size(); ++j)
            if (opposed(doses[i], doses[j])) s += 2.0 * std::min(doses[i].power, doses[j].power);
    return clamp01(s);
}

double instability(const CraftData& d, const Item& it) {
    double s = instabilityOf(d, aspectDoses(it));
    if (it.rune.bound()) {
        std::vector<AspectDose> r;
        for (const auto& [k, v] : it.rune.aspects) {
            AspectDose a;
            if (parseRuneKey(k, a.aspect, a.vec)) {
                a.power = v;
                r.push_back(a);
            }
        }
        s = std::max(s, instabilityOf(d, r));
    }
    return s;
}

Carrier carrierOf(const CraftData& d, const Item& it) {
    Carrier best;
    bool any = false;
    std::function<void(const Item&)> walk = [&](const Item& x) {
        if (x.isAssembly()) {
            for (const auto& p : x.parts) walk(p.item.front());
            return;
        }
        auto p = effectiveProps(d, x, 0);
        double c = p.count("conductivity") ? p["conductivity"] : 0.0;
        double m = p.count("mana_capacity") ? p["mana_capacity"] : 0.0;
        if (!any || c > best.conductivity + 1e-9 || (std::fabs(c - best.conductivity) <= 1e-9 && m > best.capacity)) {
            best = {c, m, x.material};
            any = true;
        }
    };
    walk(it);
    return best;
}

double runePower(const CraftData& d, const Item& it) {
    double s = 0.0;
    for (const auto& [k, v] : it.rune.aspects) s += v;
    return s * (1.0 + d.magic.resonanceRune * it.resonance);
}

std::vector<AspectDose> runeDoses(const CraftData& d, const Item& it) {
    std::vector<AspectDose> out;
    const double mult = 1.0 + d.magic.resonanceRune * it.resonance;
    for (const auto& [k, v] : it.rune.aspects) {
        AspectDose a;
        if (!parseRuneKey(k, a.aspect, a.vec)) continue;
        a.power = v * mult;
        out.push_back(a);
    }
    std::stable_sort(out.begin(), out.end(), [](const AspectDose& a, const AspectDose& b) { return a.power > b.power; });
    return out;
}

std::vector<AspectDose> spendRune(const CraftData& d, Item& it) {
    if (!it.rune.bound() || it.rune.capacity <= 0) return {};
    const double cost = d.magic.chargePerUse * it.rune.capacity;
    if (it.rune.charge + 1e-9 < cost) return {};
    it.rune.charge = std::max(0.0, it.rune.charge - cost);
    return runeDoses(d, it);
}

void rechargeRune(Item& it, double amount) {
    if (!it.rune.bound()) return;
    it.rune.charge = std::min(it.rune.capacity, it.rune.charge + std::max(0.0, amount));
}

double noteDeed(const CraftData& d, Item& it, const std::string& what) {
    ++it.deeds;
    it.history.push_back(what);
    if (it.history.size() > 16) it.history.erase(it.history.begin());
    it.resonance = 1.0 - std::exp(-static_cast<double>(it.deeds) / d.magic.resonanceDeeds);
    return it.resonance;
}

} // namespace altera::sim::craft
