// Controllers (CORE_SPEC §3). Step 2 placeholder: every character is driven by
// its AiController; the PlayerController and switching come in step 3.

#include "sim/ai.hpp"

namespace altera::sim {

void playerThink(World& w, Character& c) { aiThink(w, c); }

void installControl(World&) {}

} // namespace altera::sim
