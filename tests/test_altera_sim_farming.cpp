// Fields (farming.cpp): sowing window, growth, ripening, shattering, the
// village's weather, and a whole year closing on the real content.

#include <algorithm>
#include <cmath>
#include <vector>
#include <cstdlib>
#include <iostream>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;

namespace {

const Content& real() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

Json worldCfg(const char* season) {
    Json j = loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    j["season"] = season;
    return j;
}

std::vector<WorldObject*> fields(World& w) {
    std::vector<WorldObject*> out;
    for (EntityId id : w.objectsOfType("field"))
        if (WorldObject* o = w.object(id)) out.push_back(o);
    return out;
}

int stockOf(const WorldObject& o, const char* k) {
    auto it = o.stock.find(k);
    return it == o.stock.end() ? 0 : it->second;
}

} // namespace

TEST(altera_sim_farm_fields_at_start) {
    // Winter start: every household has a field and its seed corn put by.
    World w = makeTestWorld(real(), worldCfg("winter"));
    const auto fs = fields(w);
    CHECK_EQ(fs.size(), w.families().size());
    int withSeed = 0;
    for (WorldObject* f : fs) {
        CHECK(stockOf(*f, "strips") > 0);
        CHECK_EQ(stockOf(*f, "sown"), 0);
        const WorldObject* store = w.object(w.families().at(f->ownerFamily).storage);
        if (store && store->stock.count("seed_grain") && store->stock.at("seed_grain") > 0) ++withSeed;
    }
    CHECK_EQ(withSeed, static_cast<int>(fs.size()));
    // Summer start: the spring sowing is done and the crop is growing.
    World s = makeTestWorld(real(), worldCfg("summer"));
    for (WorldObject* f : fields(s)) {
        CHECK_EQ(stockOf(*f, "sown"), stockOf(*f, "strips"));
        CHECK(stockOf(*f, "grown") > 0);
    }
}

TEST(altera_sim_farm_growth_ripening_shattering) {
    World w = makeTestWorld(real(), worldCfg("summer"));
    const CropDef& cd = real().objects.at("field").crop;
    WorldObject* f = fields(w).front();
    f->stock["sown"] = 5;
    f->stock["grown"] = cd.growDays - 1;
    f->stock["weeds"] = 0;
    f->stock["weed_load"] = 0;
    f->stock["ripe"] = 0;
    cropsDaily(w);
    CHECK_EQ(stockOf(*f, "sown"), 0);
    const int ripe = stockOf(*f, "ripe");
    // Sheaves = strips x seed x yield / grain per sheaf, inside the crop's range.
    const double lo = 5.0 * cd.seedPerStrip * cd.yieldLo * 0.45 / cd.grainPerSheaf;
    const double hi = 5.0 * cd.seedPerStrip * cd.yieldHi * 1.35 / cd.grainPerSheaf;
    CHECK(ripe >= static_cast<int>(lo) && ripe <= static_cast<int>(hi) + 1);
    // Standing past spoil_after it shatters; before that it waits.
    for (int d = 0; d < cd.spoilAfter; ++d) cropsDaily(w);
    CHECK_EQ(stockOf(*f, "ripe"), ripe);
    for (int d = 0; d < 20; ++d) cropsDaily(w);
    CHECK(stockOf(*f, "ripe") < ripe);
    // Tilled ground nobody sowed returns to weeds outside the window.
    f->stock["tilled"] = 3;
    cropsDaily(w); // summer: outside the spring window
    CHECK_EQ(stockOf(*f, "tilled"), 0);
}

TEST(altera_sim_farm_stress_costs_yield) {
    World w = makeTestWorld(real(), worldCfg("summer"));
    WorldObject* a = fields(w).at(0);
    const CropDef& cd = real().objects.at("field").crop;
    for (const char* k : {"weed_load", "water_stress", "frost"}) a->stock[k] = 0;
    const double clean = cropYield(w, *a);
    CHECK(clean >= cd.yieldLo && clean <= cd.yieldHi);
    a->stock["weed_load"] = 200;
    CHECK(cropYield(w, *a) < clean * 0.8);
    a->stock["weed_load"] = 0;
    a->stock["water_stress"] = cd.growDays * 500; // a dry season
    CHECK(cropYield(w, *a) < clean * 0.6);
    a->stock["water_stress"] = 0;
    a->stock["frost"] = static_cast<int>(cd.frostLoss);
    CHECK(cropYield(w, *a) < clean * 0.6);
    a->stock["frost"] = 0;
    a->stock["fertility"] = 200; // worn out
    CHECK(cropYield(w, *a) < clean);
}

TEST(altera_sim_weather_follows_the_climate) {
    World w = makeTestWorld(real(), worldCfg("spring"));
    const EntityId st = w.settlements().begin()->first;
    const ClimateDef& cl = real().climate;
    CHECK(cl.loaded);
    double temp[4] = {0, 0, 0, 0}, rain[4] = {0, 0, 0, 0};
    int n[4] = {0, 0, 0, 0}, longestDry = 0, dry = 0;
    std::vector<double> years;
    double yearRain = 0.0;
    const int nYears = 40;
    for (int d = 0; d < nYears * kDaysPerYear; ++d) {
        const DayWeather x = weatherAt(w, st, d);
        CHECK_EQ(weatherAt(w, st, d).rainMm, x.rainMm); // a pure function of the day
        const int season = (d % kDaysPerYear) / (kDaysPerYear / 4);
        // The climate gives each season's middle; compare the middle month.
        const int fromMid = d % kDaysPerYear - (season * (kDaysPerYear / 4) + kDaysPerYear / 8);
        if (std::abs(fromMid) <= 15) {
            temp[season] += x.tempC;
            rain[season] += x.rainMm;
            ++n[season];
        }
        yearRain += x.rainMm;
        if ((d + 1) % kDaysPerYear == 0) {
            years.push_back(yearRain);
            yearRain = 0.0;
        }
        dry = x.wet ? 0 : dry + 1;
        if (season == 1) longestDry = std::max(longestDry, dry);
    }
    for (int s = 0; s < 4; ++s) {
        CHECK(std::abs(temp[s] / n[s] - cl.seasons[s].tempC) < 2.5);
        CHECK(std::abs(rain[s] / n[s] - cl.seasons[s].rainMm) < cl.seasons[s].rainMm * 0.4);
    }
    CHECK(temp[1] / n[1] > temp[3] / n[3] + 10.0);
    CHECK(longestDry >= 10); // summer droughts happen
    // Some years are wetter than others.
    double mean = 0.0, var = 0.0;
    for (double y : years) mean += y / years.size();
    for (double y : years) var += (y - mean) * (y - mean) / years.size();
    CHECK(std::sqrt(var) / mean > 0.08);
}

TEST(altera_sim_farm_harvests_differ_by_the_season) {
    // The same field sown on the same day every year, nothing else touching
    // it: what it gives depends on that year's weather alone.
    World w = makeTestWorld(real(), worldCfg("winter"));
    const CropDef& cd = real().objects.at("field").crop;
    WorldObject* f = fields(w).front();
    std::vector<double> ratios;
    int stressed = 0;
    for (int day = 0; day < 8 * kDaysPerYear; ++day) {
        w.run(kTicksPerDay);
        const int doy = (w.startDay() + w.day()) % kDaysPerYear;
        if (doy == cd.sowFrom + 10) {
            f->stock["ripe"] = 0;
            f->stock["sown"] = f->stock["strips"];
            f->stock["grown"] = 0;
            f->stock["fertility"] = 700;
        }
        if (f->stock["sown"] > 0 && f->stock["grown"] == cd.growDays - 1) {
            if (f->stock["water_stress"] > cd.growDays * 50) ++stressed;
            f->stock["weed_load"] = 0;
            f->stock["fertility"] = 700;
            ratios.push_back(cropYield(w, *f));
        }
        cropsDaily(w);
    }
    CHECK(ratios.size() >= 6);
    if (ratios.empty()) return;
    const auto [lo, hi] = std::minmax_element(ratios.begin(), ratios.end());
    CHECK(*hi > *lo * 1.15);
    CHECK(stressed >= 1);
}

TEST(altera_sim_farm_year_closes) {
    // From spring to the next spring: sown, reaped, threshed, seed put aside,
    // sown again - and nobody starves to death.
    World w = makeTestWorld(real(), worldCfg("spring"));
    installAi(w);
    w.run(400 * kTicksPerDay);
    int alive = 0, people = 0;
    for (const auto& [id, c] : w.characters()) {
        if (c.isAnimal()) continue;
        ++people;
        alive += c.body.alive ? 1 : 0;
    }
    CHECK_EQ(alive, people);
    int ripened = 0;
    for (const auto& e : w.chronicleLog())
        if (e.type == "crop_ripe") ++ripened;
    CHECK_EQ(ripened, static_cast<int>(w.families().size()));
    for (WorldObject* f : fields(w)) CHECK(stockOf(*f, "sown") > 0); // the second sowing
}
