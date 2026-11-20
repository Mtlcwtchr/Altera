#pragma once
// Validation of the crafting content against itself and the rest of the
// content (gdd crafting design v0.2 §11): references between files, shapes
// every archetype needs are producible, every name in a formula is known,
// every archetype has a starter design, materials have a source in the world.

namespace altera::sim {
struct Content;
struct ValidationReport;
} // namespace altera::sim

namespace altera::sim::craft {

void validateCraft(const Content& c, ValidationReport& r);

} // namespace altera::sim::craft
