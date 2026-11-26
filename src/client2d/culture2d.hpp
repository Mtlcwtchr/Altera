#pragma once
// LR-155: the culture and faith window and the "inspect" sheet of the 2D client.
//
// /api/culture?id=<person>&faith=<faith id> (cultureJson): what the window shows - the
// person's own culture and faith, the culture (values, customs, rites, food, how it takes
// other peoples, magic it teaches and forbids), the settlement (its culture, the customs it
// holds with their strength and state, who lives there by culture and faith, its values),
// the faith (gods, values, rites and sins, holy days, burial, clergy, magic, zeal,
// tolerance, the people of it here) and the list of all faiths and cultures to browse.
//
// /api/inspect?id=<target> (inspectJson): a short sheet of another person with only what
// the viewer or the viewer's dynasty knows (content/culture/inspect.json): knowledge =
// the best familiarity among them (+ living in one settlement, kin know all); each field
// opens at its threshold; their memories of the person with who remembers.
#include "client2d/view2d.hpp"

namespace lr::client2d {

sim::Json cultureJson(const sim::World& w, const ViewText& t, sim::EntityId viewer, sim::EntityId person,
                      const std::string& faith = "", const std::string& culture = "");
sim::Json inspectJson(const sim::World& w, const ViewText& t, sim::EntityId viewer, sim::EntityId target);

} // namespace lr::client2d
