#pragma once
// Validation of the crafting content against itself and the rest of the
// content (gdd crafting design v0.2 §11): references between files, shapes
// every archetype needs are producible, every name in a formula is known,
// every archetype has a starter design, materials have a source in the world.

namespace lr::sim {
struct Content;
struct ValidationReport;
} // namespace lr::sim

namespace lr::sim::craft {

void validateCraft(const Content& c, ValidationReport& r);

} // namespace lr::sim::craft
