# content/ai/expansion

Content expansion v0.2 (source: project files `gdd/content_expansion/`): skills, traits, interests,
attributes, temperaments, items, objects, moodlets, memory types, interactions, HTN methods,
calendar and social position. Icons and models are stubs; the new interactions have no own logic yet
(they use the existing FSM states and effects).

How it loads (`src/sim/expansion.cpp`, called from `Content::load`):

- `items/objects/needs(moodlets)/traits/interactions/knowledge.json` are merged into the base files of
  `content/ai/` before parsing. **The base wins on a shared id** (it carries the working mechanics:
  field crops, hens, ale); for knowledge the expansion only fills the missing fields.
- `knowledge.json → annotations` adds `category`/`attrs` to the base skills; `exclusive` pairs are added.
- `config.json → memory_types` adds new memory types, `htn.json → add_methods` adds methods to
  existing tasks.
- Extended fields (trait `kind`, `skill_mult`, `attr_mod`, `interest_seeds`, ...; knowledge `category`,
  `attrs`) are kept in `Content::expansion` (`traitExt`, `knowledgeExt`) for the core to read later.
- `interests.json`, `attributes.json`, `temperaments.json`, `calendar.json`, `social_position.json` are
  reference books, parsed into `Content::expansion`.
- `validateExpansion` checks the references: skills, interactions, goals, HTN methods, traits,
  attributes, interest categories, ranges.

Loc: `content/loc/{ru,en}/expansion.json` (keys already present in other loc files were dropped there).

Check: `lr_sim_runner --content content/ai --loc content/loc --validate` prints an `expansion:` line.
