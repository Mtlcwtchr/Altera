// LR-171: prayers and rites of the cults. See rites.hpp.
#include "sim/rites.hpp"

#include <utility>

#include "sim/world.hpp"

namespace lr::sim {

namespace {
PrayerAnswerFn& answerFn() {
    static PrayerAnswerFn f;
    return f;
}
RiteDoneFn& riteDoneFn() {
    static RiteDoneFn f;
    return f;
}
} // namespace

void setPrayerAnswerHook(PrayerAnswerFn f) { answerFn() = std::move(f); }
void setRiteDoneHook(RiteDoneFn f) { riteDoneFn() = std::move(f); }

void fulfillPrayer(World&, const Prayer&, double) {}

} // namespace lr::sim
