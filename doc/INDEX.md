# Индекс кода

Автогенерация: `python tools/gen_index.py` (не править руками). Одна строка на файл = его шапка. Карточки модулей: `doc/modules/`.

## src/
### src/engine/animation
- `animator.cpp` — The animator: layers of nodes over one skeleton, parameters in, a pose out,
- `animator.hpp` — The animator: layers of nodes over one skeleton, parameters in, a pose out,
- `ik.cpp` — Procedural corrections applied to a finished pose (Animator post-process):
- `ik.hpp` — Procedural corrections applied to a finished pose (Animator post-process):
- `motion.cpp` — What produces a pose, and the graph that combines them.
- `motion.hpp` — What produces a pose, and the graph that combines them.
- `pose.cpp` — Skeleton and pose: the representation every other part of the animator
- `pose.hpp` — Skeleton and pose: the representation every other part of the animator
- `procedural.cpp` — Procedural motions for a humanoid with the UE4 mannequin's bone names
- `procedural.hpp` — Procedural motions for a humanoid with the UE4 mannequin's bone names

### src/engine/biomes
- `category_field.cpp` — The categorical control layers over the world: four ids a 256 m sample -
- `category_field.hpp` — The categorical control layers over the world: four ids a 256 m sample -
- `detail_edits.cpp` — Hand edits to the details the terrain categories place
- `detail_edits.hpp` — Hand edits to the details the terrain categories place
- `patch_field.hpp` — One field of islands the whole picture is built on: where the moss stands and
- `registry.cpp` — Terrain categories and the biomes of the control layers
- `registry.hpp` — Terrain categories and the biomes of the control layers
- `shader_code.cpp` — The shader code and tables made from the terrain categories (registry.hpp).
- `shader_code.hpp` — The shader code and tables made from the terrain categories (registry.hpp).

### src/engine/camera
- `camera.cpp` — Pan and zoom. The camera is client state and never touches the simulation
- `camera.hpp` — Pan and zoom. The camera is client state and never touches the simulation
- `view_state.hpp`

### src/engine/core
- `binary.hpp` — Reading and writing the game's own binary form.
- `diagnostics.hpp` — Deliberately expands in the caller's scope so diagnostic declarations can
- `fixed.cpp` — Q32.32 fixed-point arithmetic.
- `fixed.hpp` — Q32.32 fixed-point arithmetic.
- `frame_jobs.hpp` — A few threads kept for the frame's own data-parallel loops (culling and
- `geometry.cpp` — Square grid geometry (see doc/DECISIONS.md D2, D20 and D44).
- `geometry.hpp` — Square grid geometry (see doc/DECISIONS.md D2, D20 and D44).
- `hash.hpp` — FNV-1a. Two uses: stable ids for content strings, and a running checksum of
- `ids.hpp` — Strongly typed handles. Mixing up a PersonId and an ItemStackId is the kind of
- `lane_hash.hpp` — A content key over large arrays: four independent multiply-xor lanes folded
- `parallel.hpp` — Independent work items on every core, for the loaders: thousands of PNG
- `profiler.cpp` — if !defined(_WIN32)
- `profiler.hpp` — A sampling profiler: what every thread of the process is doing, as it runs,
- `profiler_alloc.cpp` — The profiler's allocation hooks (engine/core/profiler.hpp): the global
- `progress.hpp` — What a long job on some thread is doing now, for whoever is waiting on it
- `publication.hpp`
- `rng.hpp` — Deterministic PRNG. Every stochastic decision in the simulation draws from a
- `time.cpp` — Game calendar.
- `time.hpp` — Game calendar.

### src/engine/ecs
- `commands.hpp` — A worker's write buffer, with no opinion about what a write is.
- `registry.hpp`
- `scheduler.hpp`
- `snapshot.hpp`

### src/engine/ecs/spatial
- `components.hpp`
- `index.hpp`

### src/engine/editor
- `main.cpp` — if ASR_ENGINE_EDITOR_GPU
- `module_panel.cpp`
- `module_panel.hpp`

### src/engine/environment
- `asset_meta.cpp` — What every imported asset must carry (doc/plan_procedural_environment_2026-10-03.md, part I;
- `asset_meta.hpp` — What every imported asset must carry (doc/plan_procedural_environment_2026-10-03.md, part I;
- `catalogue.cpp` — Everything a world's environment is generated from, read and resolved once
- `catalogue.hpp` — Everything a world's environment is generated from, read and resolved once
- `cover.cpp` — Ground cover and secondary scatter by zone
- `cover.hpp` — Ground cover and secondary scatter by zone
- `drainage.cpp` — Historical drainage (doc/plan_procedural_environment_2026-10-03.md, part D).
- `drainage.hpp` — Historical drainage (doc/plan_procedural_environment_2026-10-03.md, part D).
- `environment.cpp` — One world's procedural environment, put together
- `environment.hpp` — One world's procedural environment, put together
- `feature_layer.cpp` — The ground's change from features, as the height field asks for it
- `feature_layer.hpp` — The ground's change from features, as the height field asks for it
- `feature_mesh.cpp` — Feature meshes (doc/plan_procedural_environment_2026-10-03.md, part F).
- `feature_mesh.hpp` — Feature meshes (doc/plan_procedural_environment_2026-10-03.md, part F).
- `fields.cpp` — The continuous fields an environment is read from
- `fields.hpp` — The continuous fields an environment is read from
- `masks.cpp` — Influence masks over a page (doc/plan_procedural_environment_2026-10-03.md, part G).
- `masks.hpp` — Influence masks over a page (doc/plan_procedural_environment_2026-10-03.md, part G).
- `painted_layer.cpp` — A painted layer of the world (doc/plan_procedural_environment_2026-10-03.md):
- `painted_layer.hpp` — A painted layer of the world (doc/plan_procedural_environment_2026-10-03.md):
- `planner.cpp` — Where features go (doc/plan_procedural_environment_2026-10-03.md, part C).
- `planner.hpp` — Where features go (doc/plan_procedural_environment_2026-10-03.md, part C).
- `random.hpp` — Hashes and noise the environment is placed with. Every number is a pure
- `recipe.hpp` — Feature recipes (doc/plan_procedural_environment_2026-10-03.md, part C).
- `recipe_json.cpp`
- `scatter.cpp` — Scatter primitives (doc/plan_procedural_environment_2026-10-03.md, part E).
- `scatter.hpp` — Scatter primitives (doc/plan_procedural_environment_2026-10-03.md, part E).
- `style.cpp` — Style tables (doc/plan_procedural_environment_2026-10-03.md, part H).
- `style.hpp` — Style tables (doc/plan_procedural_environment_2026-10-03.md, part H).
- `terrain_ops.cpp` — What a feature does to the ground (doc/plan_procedural_environment_2026-10-03.md, part C).
- `terrain_ops.hpp` — What a feature does to the ground (doc/plan_procedural_environment_2026-10-03.md, part C).
- `zones.cpp` — Environment zones (doc/plan_procedural_environment_2026-10-03.md, part B).
- `zones.hpp` — Environment zones (doc/plan_procedural_environment_2026-10-03.md, part B).

### src/engine/geometry
- `cluster_asset.cpp` — The cluster DAG as bytes, and back.
- `cluster_asset.hpp` — The cluster DAG as bytes, and back.
- `cluster_dag.cpp` — Building the cluster DAG that virtual geometry selects from.
- `cluster_dag.hpp` — Building the cluster DAG that virtual geometry selects from.
- `cluster_morph.cpp`
- `cluster_morph.hpp`
- `instance_hierarchy.cpp` — A hierarchy over placed mesh instances.
- `instance_hierarchy.hpp` — A hierarchy over placed mesh instances.
- `quad_mass.cpp` — Turning a mass of alpha cards into one surface that can be retopologised.
- `quad_mass.hpp` — Turning a mass of alpha cards into one surface that can be retopologised.
- `region_mass.cpp` — One region of placed objects, merged into geometry of its own.
- `region_mass.hpp` — One region of placed objects, merged into geometry of its own.
- `smart_mesh.hpp`
- `smart_sprite_mesh.hpp`
- `smart_terrain_mesh.cpp`
- `smart_terrain_mesh.hpp`

### src/engine
- `gpu_modules.cmake` — GPU modules are engine infrastructure, not a side effect of building the game.
- `modules.cmake` — Reusable CPU modules. None may depend on game/, content, SDL or a window.

### src/engine/pipeline
- `calc_pipeline.cpp` — Work that is not drawing.
- `calc_pipeline.hpp` — Work that is not drawing.
- `clock.hpp` — How many whole steps of the world fit in the time that has passed.
- `ids.hpp` — Who a pass is, which stage it belongs to, and which pipeline runs it.
- `pass.hpp` — A pass: one thing that happens in a frame.
- `pass_clock.hpp` — ASR_PASS_TIMES=1: every pass's CPU time, averaged and printed to stderr
- `pass_name.hpp` — A pass's name for a loading screen and a log - "SceneModels" for
- `pipeline.hpp` — A pipeline: one phase of a frame, made of passes of one kind.
- `profile.hpp` — What the last second cost.
- `runner.cpp` — The one thing that knows what a frame is.
- `runner.hpp` — The one thing that knows what a frame is.

### src/engine/render
- `components.hpp` — What an entity says about being drawn, and nothing about how.
- `dag_viewport.cpp`
- `dag_viewport.hpp`
- `device.cpp` — The graphics device, and the few things only it can do.
- `device.hpp` — The graphics device, and the few things only it can do.
- `draw_arguments.hpp` — The five numbers a card reads to issue one draw.
- `draw_queue.cpp` — Everything that is going to be drawn this frame, and the order to draw it in.
- `draw_queue.hpp` — Everything that is going to be drawn this frame, and the order to draw it in.
- `frame.hpp` — What every pipeline and every pass is told about the frame it is working on.
- `hemisphere_impostor.hpp`
- `impostor_cache.cpp`
- `impostor_cache.hpp`
- `impostor_depth.hpp`
- `impostor_gpu_cache.cpp`
- `impostor_gpu_cache.hpp`
- `impostor_hierarchy.cpp` — A persistent spatial hierarchy of recursively baked impostors.
- `impostor_hierarchy.hpp` — A persistent spatial hierarchy of recursively baked impostors.
- `level_of_detail.hpp` — How much geometry a thing is worth, as one rule in one place.
- `material.hpp`
- `mesh.hpp`
- `mesh_renderer.hpp`
- `recursive_impostor.cpp`
- `recursive_impostor.hpp`
- `render_pipeline.cpp` — Drawing, centralised.
- `render_pipeline.hpp` — Drawing, centralised.
- `representation_selector.cpp`
- `representation_selector.hpp`
- `targets.cpp` — Where a frame is drawn before it reaches the window.
- `targets.hpp` — Where a frame is drawn before it reaches the window.

### src/engine/render/geometry
- `cluster_cull.cpp` — The selection step of the virtual geometry layer, on the card.
- `cluster_cull.hpp` — The selection step of the virtual geometry layer, on the card.
- `hiz_pyramid.cpp`
- `hiz_pyramid.hpp`
- `instance_hierarchy_cull.cpp` — GPU cut for the hierarchy around placed objects.
- `instance_hierarchy_cull.hpp` — GPU cut for the hierarchy around placed objects.
- `instance_root_cull.cpp` — GPU producer for instance-hierarchy replacement roots.
- `instance_root_cull.hpp` — GPU producer for instance-hierarchy replacement roots.
- `instanced.cpp` — A shape on the card, drawn once or a hundred thousand times.
- `instanced.hpp` — A shape on the card, drawn once or a hundred thousand times.
- `instances.cpp` — One buffer for every instance drawn in a frame.
- `instances.hpp` — One buffer for every instance drawn in a frame.
- `mesh_cache.cpp` — Immutable geometry on the card, kept by key.
- `mesh_cache.hpp` — Immutable geometry on the card, kept by key.
- `mesh_root_cull.cpp` — GPU expansion of mesh roots into world-space cluster candidates.
- `mesh_root_cull.hpp` — GPU expansion of mesh roots into world-space cluster candidates.
- `page_residency.cpp`
- `page_residency.hpp`
- `range_pool.hpp` — Byte ranges handed out of a set of large pages, and handed back.

### src/engine/render/passes
- `grade_pass.cpp` — The picture's last word: the grade over the finished world.
- `grade_pass.hpp` — The picture's last word: the grade over the finished world.

### src/engine/render/systems
- `cluster_draws.cpp` — Turning what a frame decided to draw into the arguments of one indirect draw.
- `cluster_draws.hpp` — Turning what a frame decided to draw into the arguments of one indirect draw.
- `frustum_cull.cpp` — Dropping what the camera cannot see, between the gather and the selection.
- `frustum_cull.hpp` — Dropping what the camera cannot see, between the gather and the selection.
- `horizon_cull.hpp` — Dropping what the ground is standing in front of.
- `instance_gather.cpp` — The system that turns entities into what a renderer draws.
- `instance_gather.hpp` — The system that turns entities into what a renderer draws.
- `instance_hierarchy_select.cpp`
- `instance_hierarchy_select.hpp`
- `level_select.cpp` — Choosing how much geometry an instance is worth, and splitting a frame by it.
- `level_select.hpp` — Choosing how much geometry an instance is worth, and splitting a frame by it.
- `source_cluster_draws.cpp`
- `source_cluster_draws.hpp`
- `terrain_gather.cpp` — The system that turns a cut of the ground into what a frame draws.
- `terrain_gather.hpp` — The system that turns a cut of the ground into what a frame draws.

### src/engine/terrain
- `erosion_field.cpp` — Erosion, split into the two different things it is.
- `erosion_field.hpp` — Erosion, split into the two different things it is.
- `field_noise.hpp` — The noise the procedural terrain is built out of, in one place.
- `mountain_shape.cpp`
- `mountain_shape.hpp`
- `plate_field.cpp` — Plates as a function of where you are, not as a map of the world.
- `plate_field.hpp` — Plates as a function of where you are, not as a map of the world.
- `range_field.cpp` — A mountain range built on a plate boundary, as a function of where you are.
- `range_field.hpp` — A mountain range built on a plate boundary, as a function of where you are.

### src/engine/ui
- `canvas.hpp` — A picture of the interface, drawn on the processor: a surface the size of
- `font.cpp` — if defined(__clang__)
- `font.hpp` — A TrueType face for the interface, and its glyphs baked for one renderer.
- `profiler_window.cpp` — The sampling profiler's window (engine/core/profiler.hpp), a window of its
- `profiler_window.hpp` — The sampling profiler's window (engine/core/profiler.hpp), a window of its
- `ui.cpp` — The interface layer: drawing, input and widgets.
- `ui.hpp` — The interface layer: drawing, input and widgets.

### src/engine/world_source
- `bake.cpp` — What is worked out from a WorldSource once and kept beside it, never in it
- `bake.hpp` — What is worked out from a WorldSource once and kept beside it, never in it
- `example_package.cpp` — A small synthetic authoring package: an island skeleton heightmap (R16), a
- `example_package.hpp` — A small synthetic authoring package: an island skeleton heightmap (R16), a
- `package_vectors.cpp` — The vector files of an authoring package (world_authoring_import_export_spec
- `package_vectors.hpp` — The vector files of an authoring package (world_authoring_import_export_spec
- `png_io.cpp` — PNG, eight and sixteen bits a channel, both ways.
- `png_io.hpp` — PNG, eight and sixteen bits a channel, both ways.
- `schema.cpp` — What an authoring package and a WorldSource say their layers are
- `schema.hpp` — What an authoring package and a WorldSource say their layers are
- `transfer.cpp` — Import and export between an authoring package and a WorldSource
- `transfer.hpp` — Import and export between an authoring package and a WorldSource
- `world_source.cpp` — The canonical authored source of a world (world_authoring_import_export_spec
- `world_source.hpp` — The canonical authored source of a world (world_authoring_import_export_spec

### src/engine/world_store
- `atomic_file.cpp` — if defined(_WIN32)
- `atomic_file.hpp` — Replacing a file so that nobody ever sees half of it.
- `chunk_file.cpp` — One chunk file: a header, a directory of typed blocks, and the blocks.
- `chunk_file.hpp` — One chunk file: a header, a directory of typed blocks, and the blocks.
- `chunk_key.cpp` — Where a piece of the world is kept.
- `chunk_key.hpp` — Where a piece of the world is kept.
- `codec.cpp` — Compression of a chunk's blocks, and the hash that says a block is intact.
- `codec.hpp` — Compression of a chunk's blocks, and the hash that says a block is intact.
- `world_root.cpp` — A world on disk.
- `world_root.hpp` — A world on disk.

### src/game/ai
- `components.hpp`
- `intents.hpp`
- `planner.cpp`
- `planner.hpp`

### src/game/client
- `asset_browser.cpp` — A window for choosing what the ground is made of from what the build has:
- `asset_browser.hpp` — A window for choosing what the ground is made of from what the build has:
- `camera.hpp` — Compatibility only: projection and camera controls are an engine module.
- `character_controller.cpp` — The player's character on the ground: WASD relative to where the camera
- `character_controller.hpp` — The player's character on the ground: WASD relative to where the camera
- `client_app.cpp` — The game client: main menu, the saved worlds, and a world to explore or
- `client_app.hpp` — The game client: main menu, the saved worlds, and a world to explore or
- `client_ui.cpp` — The game client's interface: every screen it has, drawn with ui::Ui.
- `client_ui.hpp` — The game client's interface: every screen it has, drawn with ui::Ui.
- `controls.cpp` — Camera control and picking: the two things that make a strategy game feel like
- `controls.hpp` — Camera control and picking: the two things that make a strategy game feel like
- `explore_bench.cpp` — A scripted camera and a frame-time report: `asr_client --explore --bench`.
- `explore_bench.hpp` — A scripted camera and a frame-time report: `asr_client --explore --bench`.
- `explore_diagnostics.cpp` — if ASR_ENABLE_PROFILING
- `explore_diagnostics.hpp` — if ASR_ENABLE_PROFILING
- `explore_menu.cpp` — The explorer's own world menu.
- `explore_menu.hpp` — The explorer's own world menu.
- `explore_view.cpp` — Client controls/menu adapter. World ownership and rendering live in game subsystems.
- `explore_view.hpp` — Client controls/menu adapter. World ownership and rendering live in game subsystems.
- `explorer.cpp` — Looking at the world.
- `explorer.hpp` — Looking at the world.
- `graphics_panel.cpp` — The graphics settings window of the explorer: tabs, dropdowns, checkboxes and
- `graphics_panel.hpp` — The graphics settings window of the explorer: tabs, dropdowns, checkboxes and
- `hero_camera.cpp` — The player's character and the camera that goes with it, for any view that
- `hero_camera.hpp` — The player's character and the camera that goes with it, for any view that
- `main.cpp` — Graphical client.
- `newgame_screen.cpp` — The screen the game opens on: choose a world, look at it, and start.
- `newgame_screen.hpp` — The screen the game opens on: choose a world, look at it, and start.
- `renderer.cpp` — Top-down tile renderer. Reads the world, never writes to it.
- `renderer.hpp` — Top-down tile renderer. Reads the world, never writes to it.
- `selection.cpp` — What the player currently has selected, and how a click turns into one.
- `selection.hpp` — What the player currently has selected, and how a click turns into one.
- `terrain_panel.cpp` — The Ground tab: how the control maps' categories paint the ground, edited
- `terrain_panel.hpp` — The Ground tab: how the control maps' categories paint the ground, edited
- `wall_tiling.hpp`
- `world_dials.cpp` — The generator's dials, with names on them.
- `world_dials.hpp` — The generator's dials, with names on them.
- `world_editor.cpp` — The world editor: the explorer's Edit mode.
- `world_editor.hpp` — The world editor: the explorer's Edit mode.
- `world_editor_biomes.cpp` — The world editor's terrain categories and details (engine/biomes,
- `world_inspection.hpp`

### src/game/content
- `content_db.cpp` — Loads every JSON file under content/ into immutable tables and resolves the
- `content_db.hpp` — Loads every JSON file under content/ into immutable tables and resolves the
- `defs.cpp` — Declarative content definitions (GDD 11: "content is described by declarative
- `defs.hpp` — Declarative content definitions (GDD 11: "content is described by declarative
- `ground_materials.cpp` — What the ground is made of, as data.
- `ground_materials.hpp` — What the ground is made of, as data.

### src/game/ecs
- `commands.hpp`
- `entity.hpp`
- `registry.hpp` — Compatibility vocabulary: the ECS core belongs to the engine; this binds it
- `scheduler.hpp` — Compatibility vocabulary: batching and deterministic command merging belong
- `snapshot.hpp` — Compatibility vocabulary: the ECS core belongs to the engine.

### src/game/ecs/construction
- `components.hpp`

### src/game/ecs/inventory
- `components.hpp`

### src/game/ecs/jobs
- `components.hpp`

### src/game/ecs/life
- `components.hpp`

### src/game/ecs/needs
- `components.hpp`

### src/game/ecs/render
- `components.hpp`
- `extraction.cpp`
- `extraction.hpp`

### src/game/ecs/resources
- `components.hpp`

### src/game/ecs/social
- `components.hpp`

### src/game/ecs/spatial
- `components.hpp` — Compatibility vocabulary: the ECS core belongs to the engine.
- `index.hpp` — Compatibility vocabulary: the ECS core belongs to the engine.

### src/game/editor
- `main.cpp` — The editor, on its own.

### src/game/environment
- `natural_zones.cpp` — This game's natural zones (doc/plan_procedural_environment_2026-10-03.md, part J):
- `natural_zones.hpp` — This game's natural zones (doc/plan_procedural_environment_2026-10-03.md, part J):
- `world_environment.cpp` — This game's procedural environment (doc/plan_procedural_environment_2026-10-03.md):
- `world_environment.hpp` — This game's procedural environment (doc/plan_procedural_environment_2026-10-03.md):

### src/game/generation
- `cell_field.hpp` — A value per macro cell of a world, kept only where it is not the sea's.
- `erosion_field.hpp`
- `field_noise.hpp`
- `hybrid_terrain.cpp`
- `hybrid_terrain.hpp`
- `land_coverage.cpp` — What a rectangle of the world may contain, before anything expensive is built.
- `land_coverage.hpp` — What a rectangle of the world may contain, before anything expensive is built.
- `local_map_gen.cpp` — Local map generation.
- `local_map_gen.hpp` — Local map generation.
- `mountain_shape.hpp`
- `plate_field.hpp`
- `range_field.hpp`
- `terrain_foundation.cpp`
- `terrain_foundation.hpp`
- `world_brush.cpp` — Brushes: what paints the layers (world_layers.hpp).
- `world_brush.hpp` — Brushes: what paints the layers (world_layers.hpp).
- `world_compose.cpp` — A world made of its generations (world_layout.hpp), put together.
- `world_compose.hpp` — A world made of its generations (world_layout.hpp), put together.
- `world_domain.cpp`
- `world_domain.hpp`
- `world_import.cpp` — The authored world a layout is built on: the height skeleton and control
- `world_import.hpp` — The authored world a layout is built on: the height skeleton and control
- `world_layers.cpp` — Layers: the world's maps, each at its own resolution, painted with brushes.
- `world_layers.hpp` — Layers: the world's maps, each at its own resolution, painted with brushes.
- `world_layout.cpp` — A world as a grid of regions, each generated on its own terms.
- `world_layout.hpp` — A world as a grid of regions, each generated on its own terms.
- `world_macro.cpp` — Bounded global composition for the scale-aware generator.
- `world_macro.hpp` — Bounded global composition for the scale-aware generator.
- `world_map_gen.cpp` — The world above the local map.
- `world_map_gen.hpp` — The world above the local map.
- `world_noise.cpp` — The noise the world generator's passes are made of.
- `world_noise.hpp` — The noise the world generator's passes are made of.
- `world_procedural.cpp` — Ground made by the generator instead of drawn somewhere else: the other way
- `world_procedural.hpp` — Ground made by the generator instead of drawn somewhere else: the other way
- `world_scale_policy.cpp`
- `world_scale_policy.hpp`
- `world_sketch.cpp` — The sketch of a coast: what the continents layer says of the regions still
- `world_sketch.hpp` — The sketch of a coast: what the continents layer says of the regions still

### src/game/render
- `biome_textures.cpp` — The terrain categories on the card (engine/biomes, terrain_biomes.hlsli):
- `biome_textures.hpp` — The terrain categories on the card (engine/biomes, terrain_biomes.hlsli):
- `character_model.cpp` — A skinned character: the files tools/pack_character.py writes, posed and
- `character_model.hpp` — A skinned character: the files tools/pack_character.py writes, posed and
- `climate_textures.cpp` — The world's climate on the card: three pictures, uploaded once, read by
- `climate_textures.hpp` — The world's climate on the card: three pictures, uploaded once, read by
- `forest_hierarchy.cpp` — if defined(__APPLE__)
- `forest_hierarchy.hpp` — The far forest: 128 / 512 / 2048 m nodes of the engine impostor hierarchy,
- `forest_proxy_cache.cpp`
- `forest_proxy_cache.hpp`
- `gpu_terrain.cpp` — The active GPU terrain path: pages + a quadtree cut + one immutable grid.
- `gpu_terrain.hpp` — The active GPU terrain path: pages + a quadtree cut + one immutable grid.
- `graphics_settings.cpp` — Every graphics option the explorer exposes, in one value.
- `graphics_settings.hpp` — Every graphics option the explorer exposes, in one value.
- `grass_cull.cpp`
- `grass_cull.hpp`
- `height_page_atlas.cpp` — One R16 atlas per data level. Uploads consume immutable PageStore results;
- `height_page_atlas.hpp` — One R16 atlas per data level. Uploads consume immutable PageStore results;
- `height_page_stream.cpp`
- `height_page_stream.hpp`
- `inspection_data.hpp` — The mode-specific inspection payload shares existing GPU attributes. Natural
- `pass_ids.cpp` — What this game's passes and pipelines are called.
- `pass_ids.hpp` — What this game's passes and pipelines are called.
- `presentation.hpp`
- `shadow_clipmap.cpp`
- `shadow_clipmap.hpp`
- `sprite_data.hpp` — A sprite, as the card sees it: one quad and a list of where to put copies of
- `terrain_data.hpp` — What the ground looks like once it is on the card.
- `terrain_look.cpp` — How the ground's textures are laid, for every material at once
- `terrain_look.hpp` — How the ground's textures are laid, for every material at once
- `terrain_material_cache.cpp` — The ground's material, baked: what the terrain shader works out per pixel -
- `terrain_material_cache.hpp` — The ground's material, baked: what the terrain shader works out per pixel -
- `world_materials.hpp`
- `world_renderer.cpp`
- `world_renderer.hpp`
- `world_renderer_diagnostics.cpp`

### src/game/render/calc
- `sprite_queue.cpp` — Everything the game wants drawn as a card this frame.
- `sprite_queue.hpp` — Everything the game wants drawn as a card this frame.
- `terrain_collect.cpp` — Collects page-backed terrain blocks, not copies of CPU terrain vertices.
- `terrain_collect.hpp` — Collects page-backed terrain blocks, not copies of CPU terrain vertices.
- `terrain_stream.cpp` — Keeping the ground in memory around the view.
- `terrain_stream.hpp` — Keeping the ground in memory around the view.

### src/game/render/passes
- `character_pass.cpp` — The player's character: one skinned model (CharacterModel), a level of
- `character_pass.hpp` — The player's character: one skinned model (CharacterModel), a level of
- `far_trees_pass.cpp` — Trees from the edge of the placed objects to the horizon (far_trees.hlsl).
- `far_trees_pass.hpp` — Trees from the edge of the placed objects to the horizon (far_trees.hlsl).
- `foliage_pass.cpp` — if ASR_ENABLE_DIAGNOSTICS
- `foliage_pass.hpp` — Grass, drawn as cards standing on the ground.
- `frustum_pass.cpp`
- `frustum_pass.hpp`
- `highlight_pass.cpp` — What the editor has picked out of the world, drawn over it as lines: the
- `highlight_pass.hpp` — What the editor has picked out of the world, drawn over it as lines: the
- `hold_pass.cpp` — The last picture of the world that was showing, held over the one that
- `hold_pass.hpp` — The last picture of the world that was showing, held over the one that
- `menu_pass.cpp` — Pictures drawn on the processor, put on the screen by the card.
- `menu_pass.hpp` — Pictures drawn on the processor, put on the screen by the card.
- `scene_models_pass.cpp`
- `scene_models_pass.hpp`
- `sketch_pass.cpp` — The sketch of a coast (generation/world_sketch.hpp), extruded: a slab out of
- `sketch_pass.hpp` — The sketch of a coast (generation/world_sketch.hpp), extruded: a slab out of
- `sky_pass.cpp`
- `sky_pass.hpp`
- `sprite_pass.cpp` — Cards in the world: pawns, trees, barrels, markers, decals.
- `sprite_pass.hpp` — Cards in the world: pawns, trees, barrels, markers, decals.
- `terrain_pass.cpp` — The ground.
- `terrain_pass.hpp` — The ground.
- `water_pass.cpp` — The water, over the ground it stands on.
- `water_pass.hpp` — The water, over the ground it stands on.
- `weather_pass.cpp`
- `weather_pass.hpp`

### src/game/simulation
- `ecs.hpp` — The ECS seam for simulation and presentation data.
- `farming.cpp` — Sown fields (GDD 14: the grain chain). A tile inside a farm area can be broken,
- `farming.hpp` — Sown fields (GDD 14: the grain chain). A tile inside a farm area can be broken,
- `inventory.cpp` — Queries and mutations over physical goods. Everything here works in terms of
- `inventory.hpp` — Queries and mutations over physical goods. Everything here works in terms of
- `livestock.cpp` — The flock (GDD 7). Animals age, graze, give wool and milk, breed and die. They
- `livestock.hpp` — The flock (GDD 7). Animals age, graze, give wool and milk, breed and die. They
- `needs.cpp` — Bodily states. GDD 7: these are separate axes, not one wellbeing bar, and an
- `needs.hpp` — Bodily states. GDD 7: these are separate axes, not one wellbeing bar, and an
- `pathfinder.cpp` — A* over the tile grid. Bodies move continuously between tile centres, but they
- `pathfinder.hpp` — A* over the tile grid. Bodies move continuously between tile centres, but they
- `population.cpp` — Demography: ageing, life stages, pairing, conception, birth and natural death,
- `population.hpp` — Demography: ageing, life stages, pairing, conception, birth and natural death,
- `report.cpp` — Turns a finished headless run into the plain-text report GDD 14 asks for: why
- `report.hpp` — Turns a finished headless run into the plain-text report GDD 14 asks for: why
- `savegame.cpp` — Saving and loading, in the game's own binary form.
- `savegame.hpp` — Saving and loading, in the game's own binary form.
- `world.cpp` — The whole simulated world. Headless by construction: nothing in this header or
- `world.hpp` — The whole simulated world. Headless by construction: nothing in this header or
- `zones.cpp` — Areas (GDD 9). Two kinds of area exist and they are not the same thing:
- `zones.hpp` — Areas (GDD 9). Two kinds of area exist and they are not the same thing:

### src/game/ui
- `editor.cpp` — The editor: a way to look at the game's own data and to tune how it looks,
- `editor.hpp` — The editor: a way to look at the game's own data and to tune how it looks,
- `hud.cpp` — The head-up display: what the player reads and touches while the game runs.
- `hud.hpp` — The head-up display: what the player reads and touches while the game runs.
- `info_panel.cpp` — The inspector. Everything the player clicks gets a panel explaining what it is,
- `info_panel.hpp` — The inspector. Everything the player clicks gets a panel explaining what it is,
- `overlay.cpp` — The HUD. GDD 9 requires the interface to keep two things visibly apart: the
- `overlay.hpp` — The HUD. GDD 9 requires the interface to keep two things visibly apart: the

### src/game/work
- `execution.cpp` — Advances whatever each pawn is currently doing by one tick: walking a path,
- `execution.hpp` — Advances whatever each pawn is currently doing by one tick: walking a path,
- `planner.cpp` — Autonomous work selection (GDD 2.1, 2.3, 6).
- `planner.hpp` — Autonomous work selection (GDD 2.1, 2.3, 6).

### src/game/world
- `climate_field.cpp` — The climate of the whole world, at 256-512 metres, held once.
- `climate_field.hpp` — The climate of the whole world, at 256-512 metres, held once.
- `coords.hpp` — Where things are in a world that has no edges.
- `ecology.hpp`
- `edit_layer.cpp` — What a person changed about the ground, kept apart from what the generator
- `edit_layer.hpp` — What a person changed about the ground, kept apart from what the generator
- `environment.hpp` — Terrain potential and mutable soil are separate. No renderer-owned fertility.
- `environment_models.gen.hpp` — Generated by tools/prepare_environment_models.py from content/config/environment_models.json:
- `flood_preview.hpp` — Bounded connected-water scenario, NOT a probability or a fluid simulation.
- `foliage_catalog.hpp`
- `height_field.cpp` — The shape of the ground, as a field rather than as a map.
- `height_field.hpp` — The shape of the ground, as a field rather than as a map.
- `inspection.hpp` — Immutable requests/results for per-ring inspection work. No renderer or UI references.
- `macro.cpp` — The world's large structures, at play scale.
- `macro.hpp` — The world's large structures, at play scale.
- `navmesh.cpp` — Where a body can walk, and how it gets there.
- `navmesh.hpp` — Where a body can walk, and how it gets there.
- `navmesh_shape.hpp` — Turning a set of cells into a mesh.
- `object_id.hpp` — A procedural object's name, worked out rather than stored.
- `ring_mesh.cpp` — What is left of the annulus: the foliage it scattered, and the height bounds
- `ring_mesh.hpp` — What is left of the annulus: the foliage it scattered, and the height bounds
- `scene_entities.cpp` — Turning a scatter into entities, once, when the scatter changes.
- `scene_entities.hpp` — Turning a scatter into entities, once, when the scatter changes.
- `scene_placement.cpp`
- `scene_placement.hpp`
- `scene_scatter.cpp`
- `scene_scatter.hpp`
- `scene_view.hpp`
- `shadow_field.hpp`
- `soil.cpp`
- `soil.hpp`
- `spatial.cpp` — Finding things in the world.
- `spatial.hpp` — Finding things in the world.
- `surface_reconstruction.cpp`
- `surface_reconstruction.hpp`
- `terrain_adaptive.cpp` — Compatibility vocabulary for the world's sampling/planning policies.
- `terrain_adaptive.hpp` — Compatibility vocabulary for the world's sampling/planning policies.
- `terrain_brush.cpp` — The tools a person shapes ground with.
- `terrain_brush.hpp` — The tools a person shapes ground with.
- `terrain_config.cpp` — CPU-only tuning. A view carries a value snapshot, never a mutable file watcher.
- `terrain_config.hpp` — CPU-only tuning. A view carries a value snapshot, never a mutable file watcher.
- `terrain_cut.hpp` — A non-overlapping quadtree cut. Preload requests never become draw nodes.
- `terrain_detail.cpp`
- `terrain_detail.hpp`
- `terrain_grid.cpp` — The only terrain topology kept by the renderer. Every terrain node instances
- `terrain_grid.hpp` — The only terrain topology kept by the renderer. Every terrain node instances
- `terrain_height_pages.hpp` — Addressing shared by the uploader and the future page-backed vertex path.
- `terrain_lod.cpp` — Runtime terrain topology selection. Geometry is an immutable grid; changing
- `terrain_lod.hpp` — Runtime terrain topology selection. Geometry is an immutable grid; changing
- `terrain_mesh.cpp` — The surface of the world as a triangle mesh, built a chunk at a time.
- `terrain_mesh.hpp` — The surface of the world as a triangle mesh, built a chunk at a time.
- `terrain_plan.cpp` — CPU-only terrain planning. No Camera, SDL objects or mutable atlas references
- `terrain_plan.hpp` — CPU-only terrain planning. No Camera, SDL objects or mutable atlas references
- `terrain_progress.hpp` — Read-only snapshots. Percentages count pages/target tiles, never elapsed time.
- `terrain_residency.hpp` — Fixed-budget residency for runtime terrain detail pages. Page contents live in
- `terrain_seams.cpp`
- `terrain_source.cpp` — Surface pages produced on demand from the bounded composition.
- `terrain_source.hpp` — Surface pages produced on demand from the bounded composition.
- `terrain_source_tree.cpp` — The sparse refinement tree the generator's data actually lives in.
- `terrain_source_tree.hpp` — The sparse refinement tree the generator's data actually lives in.
- `terrain_view.hpp`
- `tile_mesh.cpp` — The ground as square tiles, addressed by where they are in the world.
- `tile_mesh.hpp` — The ground as square tiles, addressed by where they are in the world.
- `weather.hpp`
- `weather_temperature.hpp`
- `world_builder.cpp`
- `world_builder.hpp`
- `world_delta.cpp` — The persistent delta: everything that has happened to a world.
- `world_delta.hpp` — The persistent delta: everything that has happened to a world.
- `world_delta_codec.cpp` — What the persistent delta is made of, and how it is written down.
- `world_delta_codec.hpp` — What the persistent delta is made of, and how it is written down.
- `world_saves.cpp` — The worlds on disk: what the host screen lists, creates and deletes.
- `world_saves.hpp` — The worlds on disk: what the host screen lists, creates and deletes.
- `world_system.hpp`
- `world_tools.cpp` — What the edit tools do to a world, as steps that can be taken back.
- `world_tools.hpp` — What the edit tools do to a world, as steps that can be taken back.

### src/game/world/terrain_streaming
- `baked_page_cache.cpp`
- `baked_page_cache.hpp`
- `base_tile.hpp`
- `base_tile_baker.cpp` — Baking H_sim into square, world-aligned pages.
- `base_tile_baker.hpp` — Baking H_sim into square, world-aligned pages.
- `cache_bytes.hpp` — The byte primitives every terrain cache record is written with.
- `graph_carve.cpp` — Cutting the ground to the water, from the graph.
- `graph_carve.hpp` — Cutting the ground to the water, from the graph.
- `hydrology_builder.cpp` — Turning the macro map's drainage flags into a network that can be stored.
- `hydrology_builder.hpp`
- `hydrology_cache.cpp` — The persistent river/lake graph on disk.
- `hydrology_cache.hpp` — The persistent river/lake graph on disk.
- `hydrology_graph.cpp`
- `hydrology_graph.hpp`
- `hydrology_selection.cpp` — Choosing a piece of the water to edit, with what depends on it.
- `hydrology_selection.hpp` — Choosing a piece of the water to edit, with what depends on it.
- `page_ground.cpp` — Reading the ground off a baked page.
- `page_ground.hpp` — Reading the ground off a baked page.
- `page_store.cpp` — The pages a worker can ask for by key.
- `page_store.hpp` — The pages a worker can ask for by key.
- `tile_cache.cpp`
- `tile_cache.hpp`
- `tile_layout.hpp`
- `water_tile.cpp` — The water over one page, derived rather than sampled.
- `water_tile.hpp` — The water over one page, derived rather than sampled.
- `worker_pool.cpp`
- `worker_pool.hpp`
- `worker_runtime.cpp`
- `worker_runtime.hpp`

### src/game_view
- `game_layer.hpp` — What the world view (terrain_lab's Lab today, altera_world_view after the
- `hud.cpp` — The game's interface, style G of the design canvas "Altera UI и HUD"
- `hud.hpp` — The game's interface, style G of the design canvas "Altera UI и HUD"
- `loading_screen.cpp` — The loading screen: while the world opens (heightmap, water, shaders,
- `loading_screen.hpp` — The loading screen: while the world opens (heightmap, water, shaders,
- `sim_game.cpp` — The life-sim in the world: altera_sim steps on a thread of its own, the
- `skinned.cpp` — Skinned figures for the game: the people (and, with their own files, the
- `skinned.hpp` — Skinned figures for the game: the people (and, with their own files, the
- `snapshot.hpp` — What the sim thread hands the frame (src/game_view/sim_game.cpp builds it
- `statics.cpp` — The village's buildings and works as static meshes: one bake
- `statics.hpp` — The village's buildings and works as static meshes: one bake

### src/sim
- `aftermath.cpp` — After a fight (COMBAT_DESIGN §7-8, §17; CORE_SPEC §6, §11):
- `aftermath.hpp` — After a fight (COMBAT_DESIGN §7-8, §17; CORE_SPEC §6, §11):
- `ai.hpp` — The four-layer mind (AI_ARCHITECTURE_v0.1): Utility -> HTN -> GOAP -> FSM.
- `ai_control.cpp` — Controllers (CORE_SPEC §2-3). A Character is one type for the player and for
- `ai_eval.cpp` — Condition evaluation and the interaction executor shared by every layer and
- `ai_goap.cpp` — Layer 3 - GOAP: "out of which physical actions?" A* over a small state
- `ai_htn.cpp` — Layer 2 - HTN: "how, in broad strokes?" A goal's root task is decomposed by
- `ai_mind.cpp` — The mind loop: Utility selection (hysteresis, interrupts, weighted top-K),
- `ai_report.cpp` — "Why" report (AI_ARCHITECTURE §8): per character and aggregated - why
- `ai_utility.cpp` — Layer 1 - Utility: "what do I want now?" Scores every goal type from its
- `animals.cpp` — Animals and monsters: characters of the same core (Character with a
- `building.cpp` — Building from modules and the colony's mandate (sim/building.hpp).
- `building.hpp` — Building from modules (content/ai/build.json, gdd/blueprints/
- `captivity.cpp` — A captive's fate (COMBAT_DESIGN §7, §17; content/combat/captivity.json):
- `captivity.hpp` — A captive's fate (COMBAT_DESIGN §7, §17; content/combat/captivity.json):
- `care.cpp` — Care (care.json): who depends on others - infants, children, the old, the
- `character.cpp` — Character - one data type for the player's people and every NPC
- `character.hpp` — Character - one data type for the player's people and every NPC
- `combat_bridge.cpp` — altera_sim <-> combat core (src/sim/combat). An interaction with a `combat`
- `combat_bridge.hpp` — altera_sim <-> combat core (src/sim/combat). An interaction with a `combat`
- `content.cpp` — Declarative content (content/ai/*.json, doc/design/AI_DATA_SCHEMA.md). Code is
- `content.hpp` — Declarative content (content/ai/*.json, doc/design/AI_DATA_SCHEMA.md). Code is
- `core.hpp` — Altera simulation core: basic types shared by every module. Headless, no
- `farming.cpp` — Fields (objects.json `crop`): tilling and sowing in a window of the year,
- `group_combat.cpp` — Group fights (COMBAT_DESIGN §9, CORE_SPEC §6): when an act with a combat
- `group_combat.hpp` — Group fights (COMBAT_DESIGN §9, CORE_SPEC §6): when an act with a combat
- `loc.cpp` — Localization tables, the placeholder formatter, the loc validator and the
- `loc.hpp` — Localization (doc/design/LOC_FORMAT.md). Data and code carry keys only;
- `rules.cpp` — The condition/effect mini-language of AI_DATA_SCHEMA.md §1.
- `rules.hpp` — The condition/effect mini-language of AI_DATA_SCHEMA.md §1.
- `tones.cpp` — The tone of an act (content/ai/tones.json): the player picks it on the
- `tones.hpp` — The tone of an act (content/ai/tones.json): the player picks it on the
- `validate.cpp` — Content validators (AI_DATA_SCHEMA.md §12, AI_ARCHITECTURE §10).
- `weather.cpp` — Daily weather of a settlement (climate.json), a pure function of the world
- `world.cpp` — World state: time, map stub, objects, characters, families, the event queue
- `world.hpp` — World state: time, map stub, objects, characters, families, the event queue
- `worldgen.cpp` — The small test world of AI_ARCHITECTURE §11.1: a stub map (8-16 km), 2-3
- `worldgen.hpp` — The small test world of AI_ARCHITECTURE §11.1: a stub map (8-16 km), 2-3
- `wounds.cpp` — Wounds in the character profile (Kenshi-like, COMBAT_DESIGN §6, CORE_SPEC §6):
- `wounds.hpp` — Wounds in the character profile (Kenshi-like, COMBAT_DESIGN §6, CORE_SPEC §6):

### src/sim/combat
- `combat.cpp` — Combat core: round loop of melee duels, clinch, ranged approach, group
- `combat.hpp` — Combat core: round loop of melee duels, clinch, ranged approach, group
- `combat_anim.cpp` — Event stream -> animation cues (COMBAT_PRESENTATION.md §2-3). No rendering:
- `combat_anim.hpp` — Event stream -> animation cues (COMBAT_PRESENTATION.md §2-3). No rendering:
- `combat_data.cpp` — Combat data (content/combat/*.json) parsed into flat tables. Every number of
- `combat_data.hpp` — Combat data (content/combat/*.json) parsed into flat tables. Every number of
- `combat_lod.cpp` — Statistical (LOD) resolution of duels (COMBAT_CORE_SPEC §7): far from the
- `combat_lod.hpp` — Statistical (LOD) resolution of duels (COMBAT_CORE_SPEC §7): far from the
- `combat_rng.hpp` — Combat PRNG: xoshiro256** seeded through splitmix64. One generator per

### src/sim/combat/tool
- `combat_tables.cpp` — altera_combat_tables: runs the same tables as gdd/combat/tools/combat_sim.py

### src/sim/runner
- `main.cpp` — altera_sim_runner - headless runs of the Altera life-sim core.

### src/terrain_core
- `biomes.cpp` — What the ground is made of: the biome map over the world and, per biome,
- `biomes.hpp` — What the ground is made of: the biome map over the world and, per biome,
- `clearings.hpp` — Clearings: discs where plants do not grow (the game's village, its yards
- `dlss.cpp` — if ASR_HAS_DLSS
- `dlss.hpp`
- `dlss_math.hpp`
- `drainage.cpp` — Large-scale hydraulic erosion: valleys cut by the water a whole catchment
- `drainage.hpp` — Large-scale hydraulic erosion: valleys cut by the water a whole catchment
- `erosion.cpp` — Offline erosion of the macro map, on the GPU.
- `erosion.hpp` — Offline erosion of the macro map, on the GPU.
- `far_detail.cpp` — The far ground's painted detail (far_detail.hlsli): gullies, strata,
- `far_detail.hpp` — The far ground's painted detail (far_detail.hlsli): gullies, strata,
- `foliage.cpp` — Foliage on the GPU (stage 3 of doc/rework/TERRAIN_REWORK_PLAN.md).
- `foliage.hpp` — Foliage on the GPU (stage 3 of doc/rework/TERRAIN_REWORK_PLAN.md).
- `grass.cpp` — The grass cover: solid grass wherever the ground is grass, in the ground's
- `grass.hpp` — The grass cover: solid grass wherever the ground is grass, in the ground's
- `ground_height.cpp` — The ground's height on the CPU, as the GPU makes it: terrainHeight() in
- `ground_height.hpp` — The ground's height on the CPU, as the GPU makes it: terrainHeight() in
- `hydrology.cpp` — Rivers and lakes, worked out once from the macro map when a world loads.
- `hydrology.hpp` — Rivers and lakes, worked out once from the macro map when a world loads.
- `lighting.cpp` — Light for the terrain core: the sun and sky by time of day, height fog,
- `lighting.hpp` — Light for the terrain core: the sun and sky by time of day, height fog,
- `lod_select.cpp` — Which pieces of ground are drawn at which level, and which texels of the
- `lod_select.hpp` — Which pieces of ground are drawn at which level, and which texels of the
- `macro_map.cpp` — The macro height map: one 16-bit picture stretched over the whole world.
- `macro_map.hpp` — The macro height map: one 16-bit picture stretched over the whole world.
- `params.hpp` — The few numbers the whole terrain core is built from.
- `post.cpp` — Picture styles over the finished frame: dark fantasy, Frazetta, Vallejo
- `post.hpp` — Picture styles over the finished frame: dark fantasy, Frazetta, Vallejo
- `props.cpp` — Mega structures: the mesh part of the hybrid terrain
- `props.hpp` — Mega structures: the mesh part of the hybrid terrain
- `psx.cpp` — The PSX look as the very last pass over the frame (assets/shaders/terrain_core/psx.hlsl):
- `psx.hpp` — The PSX look as the very last pass over the frame (assets/shaders/terrain_core/psx.hlsl):
- `relief.cpp` — Fine relief: small windows of the ground at a metre or two a texel, for
- `relief.hpp` — Fine relief: small windows of the ground at a metre or two a texel, for
- `renderer.cpp` — The terrain core on the GPU: the macro map, the clipmap it generates from it,
- `renderer.hpp` — The terrain core on the GPU: the macro map, the clipmap it generates from it,
- `rivers.cpp` — The river graph's segments on the GPU (hydrology.hpp), with a grid of
- `rivers.hpp` — The river graph's segments on the GPU (hydrology.hpp), with a grid of
- `sky.cpp` — The sky (a full-screen pass drawn first) and the volumetric clouds (a
- `sky.hpp` — The sky (a full-screen pass drawn first) and the volumetric clouds (a
- `water.cpp` — Water on the GPU: the sea, lakes and rivers of the water map (hydrology.hpp)
- `water.hpp` — Water on the GPU: the sea, lakes and rivers of the water map (hydrology.hpp)

## sim_runner/
### sim_runner
- `main.cpp` — Headless scenario runner (GDD 11).

## tests/
### tests
- `framework.hpp` — A test harness small enough to not be a dependency. Registration happens at
- `main.cpp`
- `support.hpp` — Shared setup: every simulation test needs the real content tree, loaded once.
- `test_altera_combat.cpp` — Combat core (src/sim/combat): data validator, determinism of the event log,
- `test_altera_sim.cpp` — Altera life-sim core (src/sim): world skeleton, determinism, persistence.
- `test_altera_sim_build.cpp` — Building from modules (content/ai/build.json, sim/building.hpp) and the
- `test_altera_sim_captivity.cpp` — Group fights (group.json), the captive's fate and executions
- `test_altera_sim_combat.cpp` — altera_sim <-> combat core: an act with a combat block (FSM Fight) is played
- `test_altera_sim_content.cpp` — Altera life-sim core on the real content (content/ai, parts B and C) and the
- `test_altera_sim_control.cpp` — Altera life-sim core: controllers, Sims-style switching, player/NPC equality.
- `test_altera_sim_crafts.cpp` — Household crafts on the real content: livestock (animals.cpp domestic
- `test_altera_sim_farming.cpp` — Fields (farming.cpp): sowing window, growth, ripening, shattering, the
- `test_altera_sim_loc.cpp` — Altera life-sim core: localization (LOC_FORMAT.md) and the mini-language
- `test_animation.cpp` — The animator (engine/animation): pose maths, blend spaces, the state
- `test_biomes.cpp`
- `test_camera.cpp`
- `test_camera_matrix.cpp` — The projection, as a matrix, against the projection as it was written out.
- `test_cluster_cull_gpu.cpp`
- `test_cluster_dag.cpp`
- `test_cluster_draws.cpp`
- `test_content.cpp`
- `test_dag_viewport_gpu.cpp`
- `test_determinism.cpp`
- `test_dlss_gpu.cpp`
- `test_dlss_math.cpp`
- `test_draw_distance.cpp`
- `test_ecology_shadow.cpp`
- `test_ecs.cpp`
- `test_engine_clock.cpp` — The fixed step, which is the only clock the world is allowed to have.
- `test_engine_modules.cpp`
- `test_environment.cpp`
- `test_environment_menu.cpp`
- `test_environment_workers.cpp`
- `test_erosion_field.cpp`
- `test_external_profiler.py` — External profiler tests: no engine, Xcode, target process or GPU required.
- `test_fixed.cpp`
- `test_frustum_cull.cpp`
- `test_grass_cull_gpu.cpp`
- `test_ground_edits.cpp` — Ground dug into a world is the ground that is drawn (D149).
- `test_height_page_atlas_gpu.cpp`
- `test_horizon_cull.cpp`
- `test_hybrid_terrain.cpp`
- `test_impostor_cache_gpu.cpp`
- `test_impostor_depth.py`
- `test_impostor_hierarchy.cpp`
- `test_indirect_draw_gpu.cpp` — Proving the draw path before anything is built on it.
- `test_inspector.cpp`
- `test_instance_arena.cpp` — if defined(__unix__) || defined(__APPLE__)
- `test_instance_gather.cpp`
- `test_instance_hierarchy.cpp`
- `test_land_coverage.cpp`
- `test_level_select.cpp`
- `test_marsh_islands.py` — Run with python -m unittest discover -s tests -p test_marsh_islands.py.
- `test_medicine.cpp` — Medicine and burial (D98, D99).
- `test_natural_basins.cpp`
- `test_page_residency.cpp`
- `test_pathfinding.cpp`
- `test_plate_field.cpp`
- `test_prepare_ue_assets.py` — CPU-only regression tests for staged UE import; no Unreal/GPU needed.
- `test_procedural_environment.cpp` — The procedural environment's mechanism (doc/plan_procedural_environment_2026-10-03.md):
- `test_quad_mass.cpp`
- `test_range_field.cpp`
- `test_recursive_impostor.cpp`
- `test_region_mass.cpp`
- `test_ring_stream.cpp`
- `test_savegame.cpp`
- `test_scene_entities.cpp`
- `test_scene_scatter.cpp`
- `test_scene_view.cpp`
- `test_simulation.cpp`
- `test_slope_shadow_gpu.cpp`
- `test_smart_mesh.cpp` — if ASR_SMART_RENDER_TESTS
- `test_soil.cpp`
- `test_source_cluster_draws.cpp`
- `test_terrain_adaptive.cpp`
- `test_terrain_brush.cpp`
- `test_terrain_config.cpp`
- `test_terrain_core.cpp`
- `test_terrain_cut.cpp`
- `test_terrain_foundation.cpp`
- `test_terrain_gather.cpp`
- `test_terrain_height_pages.cpp`
- `test_terrain_lod.cpp`
- `test_terrain_plan.cpp`
- `test_terrain_refinement.cpp`
- `test_terrain_source.cpp`
- `test_terrain_source_tree.cpp`
- `test_terrain_streaming.cpp`
- `test_tile_mesh.cpp`
- `test_ui.cpp` — The interface layer with a real face: text measured and drawn in points,
- `test_water_bodies.cpp`
- `test_water_queries.cpp`
- `test_water_selection.cpp` — Picking a piece of the water picks what drains into it, and nothing it
- `test_weather.cpp`
- `test_world_compose.cpp`
- `test_world_delta.cpp`
- `test_world_import.cpp` — An imported skeleton (world_import.hpp) is the ground of the region it is in.
- `test_world_layers.cpp`
- `test_world_layout.cpp`
- `test_world_mesh.cpp` — The world as geometry: the height field, the chunk meshes cut out of it, and
- `test_world_nav.cpp` — Walking the world: polygons of walkable ground, portals between them, and
- `test_world_procedural.cpp` — The procedural way into the world's source (world_procedural.hpp): the
- `test_world_scale.cpp`
- `test_world_source.cpp`
- `test_world_spatial.cpp` — Finding things in the world: the tree over static geometry, the index over
- `test_world_stages.cpp` — The authoring pipeline (doc/authoring_pipeline_2026-09-30.md): a region made
- `test_world_store.cpp`
- `test_world_system.cpp`
- `test_world_tools.cpp` — Saved worlds on disk, and the edit tools' steps and their undo.
- `test_worldgen.cpp` — The country above the map (D84, D88).

## tools/
### tools
- `altera_profiler.py` — Altera external profiler: Instruments capture without engine instrumentation.
- `apply_terrain_materials_to_ground.py` — Bridge new terrain library materials into legacy ground textures.
- `assemble_controls.py` — Control maps for an assembled world, read off hand-drawn region maps
- `assemble_heightmap.py` — Lays several height pictures out on one world and writes it as an
- `assemble_tree_crown.py` — Bake bounded crossed-card branch prototypes at authored Nanite assembly transforms.
- `bake_character_impostor.py` — Eight-view impostor of a packed character (tools/pack_character.py), in its rest pose.
- `bake_foam_residue.py` — Bake the PHX residue layer only; existing water normals/foam stay untouched.
- `bake_impostor_depth.py` — Add optional depth impostors to an existing SCM2 catalogue, without reimporting meshes.
- `bake_materials.py` — Bakes the ground materials into single textures the renderer can use.
- `bake_model_impostors.py` — bake_model_impostors - impostors for the simplified source models, in the
- `bake_water.py` — Bakes the water surface into the three layers the water shader reads.
- `biome_layout.py` — Lays the biomes of a world over a height map, by their requirements, and
- `build_fairytale_package.py` — Builds the Fairytale world's authoring package from its painted heightmap.
- `build_pawn_rig.py` — Builds the pawn rig out of assets/sprites/source/pawn_layers.png.
- `capture_grass_culling.py` — Same-process GPU/direct/GPU grass A/B/A; no FPS claim or desktop capture.
- `capture_scene_models.py` — Capture bounded real-client runs for terrain-decoration regression checks.
- `capture_terrain_continuation.py` — Capture terrain publication morphs and distant erosion; requires diagnostics.
- `capture_vegetation_cover.py` — Real-client vegetation regression captures, including an isolated grass A/B.
- `capture_vegetation_lods.py` — Capture view-wide vegetation LODs, cold far views, panning and open biomes.
- `character_import_blender.py` — Blender (headless) half of the character import: FBX in, glTF binary out.
- `check_fast.bat` — check_fast.bat - the quick check layer: build the sim-only tree (cmake-build-sim,
- `compare_tree_shells.py` — Isolated, equal-frame tree/LOD comparisons (CPU, not game screenshots).
- `content_validator.cpp` — Content validator (GDD 7, 11).
- `discover_itch.py` — List itch.io game-asset pages from free listings: discover_itch.py tag-3d/tag-fantasy [pages]
- `environment_probe.cpp` — environment_probe - what the procedural environment makes of a world, and
- `ethnos_table.py` — Write doc/ETHNOS_CONTENT.md: what each people is made of.
- `export_ue_assets.py` — Export the used, already-cut nature LOD0 and terrain textures without rendering.
- `export_ue_foliage.py` — Export extra foliage from the AncientSettlement UE project: glTF geometry + colour/opacity textures.
- `export_ue_sky.py` — Export the sky source textures from the AncientSettlement UE project.
- `export_ue_terrain.py` — Export extra ground surface textures from the AncientSettlement UE project.
- `extract_sprites.py` — Cuts the game's sprites out of the source art sheets.
- `feature_forms.cpp` — feature_forms - procedural feature meshes as source assets
- `fetch_itch.py` — Download every free upload of an itch.io page (no login; skips paid/PWYW-with-minimum pages).
- `fetch_models.py` — fetch_models - brings down the source scene models (trees, shrubs, ground
- `fetch_terrain.py` — fetch_terrain - brings down the source textures the terrain is made of.
- `fetch_terrain_references.py` — Download reference DEMs; decode Terrarium BEFORE resampling, never screen gray.
- `gen_index.py` — Генератор doc/INDEX.md: одна строка на файл кода = первая содержательная строка шапки (комментарий/докстринг).
- `gpu_profile.py` — gpu_profile - where the GPU spends a frame, for any Vulkan program, from outside it.
- `height_from_colour_map.py` — A height sheet for a region that has only a flat coloured map: blue sea and
- `hemisphere_impostor.py` — Hemisphere layout shared by asset metadata and the engine's view mapping.
- `hybrid_terrain_probe.cpp`
- `hydrology_probe.cpp`
- `impostor_bake.cpp` — impostor_bake - the impostor rasterizer of tools/prepare_scene_models.py
- `install_ue_assets.py` — Validate and activate a prepared UE bundle, retaining the previous resources.
- `lake_edge_probe.cpp` — Lakes hanging in the air, counted.
- `look_ablation.sh` — What each graphics feature costs: the same scripted bench with one thing
- `look_grid.py` — A picture as a small text grid, for reading a shot without a screen.
- `look_hues.py` — Hue histogram of the lower part of shots (saturated pixels only).
- `look_metrics.py` — Numbers for a picture, for comparing looks without eyes on the screen.
- `look_palette.py` — Palette summary of shots: overall luma/contrast, the ground's mean hue and
- `look_shots.sh` — A fixed set of views for judging the look: on foot in a wood, on foot in a
- `make_flower_cards.py` — Flower cards of real species, composed from the simplified flower models.
- `make_foliage_cards.py` — Ground flora cards beyond the six imported grass views: the meadow flowers,
- `mesh_lab.cpp` — asr_mesh_lab - side by side: what Unreal's Nanite bake makes of a mesh, and
- `mesh_lod.py` — Discrete level-of-detail chains for the imported nature meshes.
- `model_simplify.cpp` — model_simplify - one source model (glTF 2.0 with an external .bin, the way
- `mountain_shape_probe.cpp`
- `pack_character.py` — The engine half of the character import: a skinned .glb in, the engine's files out.
- `pack_terrain.py` — pack_terrain - turns the downloaded source maps into what the game loads.
- `ppm_to_png.py` — PPM -> PNG, with no dependencies. sim_runner writes the world map as a PPM
- `prepare_environment_models.py` — Append the selected environment visuals to the existing scene catalogue.
- `prepare_environment_textures.py` — Prepare downloaded moss/debris through the existing terrain map packer.
- `prepare_scene_models.py` — Offline import of the selected static glTF 2 models; eight relightable impostors.
- `prepare_sketchfab.py` — Downloaded Sketchfab surfaces -> optimized, painted 1960s-80s fantasy library.
- `prepare_sky.py` — Turn the UE sky export (tools/export_ue_sky.py) into runtime sky assets.
- `prepare_ue_assets.py` — Prepare an exported UE nature/terrain set in a NEW staging directory.
- `prepare_ue_foliage.py` — prepare_ue_foliage - the raw UE foliage export (tools/export_ue_foliage.py)
- `reach_probe.cpp` — Prints a reach's course: where it runs, its head, and the ground either
- `rebuild_prop_lods.py` — Rebuild the distance chain of the solid scene props and orient every model.
- `repack_ue_terrain.py` — Re-pack the installed UE terrain materials at a higher runtime resolution.
- `ripple_bisect.py` — Which shader term is the ripple. Measured, one term at a time.
- `ripple_shape.py` — What the ripple actually looks like, as numbers.
- `ripple_stability.py` — Does the ground stay still when the camera moves?
- `scene_model_clusters.cpp` — Builds the cluster DAG sidecar for every scene model that has solid geometry.
- `shader_check.cpp` — Offline HLSL -> SPIR-V -> Metal validation. No SDL video, window or GPU device.
- `shot_diff.py` — Pixel comparison of two shot directories: share of differing pixels, max and mean |delta|.
- `simplify_models.py` — simplify_models - the downloaded source models reduced to import budgets.
- `sketchfab_download.py` — Download Sketchfab models listed in a candidates JSON using YOUR API token.
- `sketchfab_search.py` — Collect downloadable CC0 / CC-BY Sketchfab models for Altera -> doc/sketchfab_candidates.{json,md}
- `sprite_grid.py` — Finds the variant grid in a source art sheet.
- `stage_ue_terrain.py` — Stage the UE terrain exports as ordinary material sources.
- `style_reference.py` — What a folder of style references is made of, as numbers a look can use.
- `stylize_textures.py` — Force source textures through one look (doc/plan_procedural_environment_2026-10-03.md, part I).
- `summarize_xctrace.py` — Summarize exported xctrace time-profile XML, including interned element refs.
- `terrain_feature_probe.cpp` — Headless measurement of the production planner; no GPU/FPS claims.
- `terrain_lint.cpp` — Looks for the marks a generator leaves on its own work.
- `terrain_resolution_probe.cpp` — Offline source-resolution experiment. Does not change runtime terrain policy.
- `terrain_set_polyhaven.py` — The Poly Haven ground set the renderer's layers are built from (sixteen in
- `terrain_shimmer.py` — How much of a picture is pixel-scale noise rather than shape.
- `terrain_subdivision_probe.cpp` — Measures tessellation, not whether an erosion reference has been reproduced.
- `ue_assembly_export.py` — Export small assembly parts + transforms, never high-resolution tree trunks.
- `ue_assembly_format.py` — Parser for Unreal's exported assembly structs (data only; no eval).
- `ue_asset_inventory.py` — Read-only UE commandlet inventory for the nature/terrain migration.
- `ue_asset_policy.py` — CPU-only transfer limits matching AncientSettlement/Tools/Editor/cut_assets.py.
- `ue_asset_session.py` — Bounded, read-only UE import session: keep startup across export corrections.
- `ue_commandlet.sh` — Runs one UE Python commandlet: niced, with a wall-clock limit, log to $LOG.
- `ue_material_graph.py` — Read-only material graph diagnostics for CPU conversion of used UE materials.
- `ue_nanite_reference.py` — Unreal editor script: export what Nanite's bake makes of a few reference meshes.
- `ue_terrain_sources.py` — List large ground-like textures in the UE project (read-only, no loading).
- `ue_tree_assembly.py` — Read Nanite assembly metadata without copying high-resolution trunk geometry.
- `validate_content.py` — Единый валидатор контента Altera (облако и ПК, только stdlib).
- `verify_vegetation_trace.py` — Validate the step-4 forest --trace route: continuous pan, zoom and LOD fallback.
- `water_edge_probe.cpp` — Where the drawn water does not meet the drawn ground, counted.
- `world_load_probe.cpp` — What opening a world costs, pass by pass: the world made from a preset over
- `world_root_inspect.cpp` — world_root_inspect - what a world root on disk holds, chunk by chunk.
- `world_scale_policy_probe.cpp`
- `world_scale_probe.cpp` — What the bounded composition actually costs and contains, at several world
- `worldtool.cpp` — worldtool: the authoring package <-> WorldSource bridge

### tools/anim_retarget
- `asf_amc.py` — CMU ASF/AMC reader for Blender: builds an armature with one action, resampled.
- `bindings.py` — Which retargeted clips serve which game keys.
- `bone_maps.py` — Bone maps onto the Quaternius Universal Animation Library rig (UE-style names).
- `build_jobs.py` — Build the job list for retarget_ual.py from the unpacked sources.
- `build_manifest.py` — Write the clip manifest and the gap report for the retargeted humanoid clips.
- `preview_sheet.py` — Contact sheet of clips on the UAL mannequin (Workbench, front 3/4 view).
- `probe_skeleton.py` — Print armature bones (name, parent, world head/tail, rest dir) of an FBX/GLB/BVH.
- `retarget_ual.py` — Batch retarget onto the Quaternius UAL rig and export one GLB per clip.
- `verify_glb.py` — Re-import exported clips and check them against the UAL rig.

### tools/blender
- `asr_blender.py` — Engine-side Blender toolkit: shapes made monumental and weathered, textured
- `dead_trees.py` — Dead trees for the stele's glade (doc/megaliths/A2_CARVED_STELE.md): bare,
- `export_foliage.py` — Blender (background) side of tools/terrain_lab/build_foliage.py.
- `export_sketchfab_models.py` — Re-export picks from the processed Sketchfab library for the scene catalogue.
- `megalith_a2_carved_stele.py` — Magic megalith A-2: the carved stele (doc/BIOME_PLAN.md, kind A:
- `megalith_a3_desert_gate.py` — Magic megalith A-3: the gate of the golden desert (doc/BIOME_PLAN.md 3.4, kind A:
- `megalith_a4_canyon_arch.py` — Magic megalith A-4: the arch-bridge of the canyon (kind A: blockout -> sculpt -> material,
- `megalith_a_monolith.py` — Magic megalith A-1: the giant monolith slabs (doc/BIOME_PLAN.md, kind A:
- `megaliths.py` — This game's megaliths, for the 1960s-80s fantasy look (doc/references/artstyle):
- `prepare_sketchfab_asset.py` — Preserve imported materials, UVs and rigs while preparing painted Sketchfab assets.
- `preview_sketchfab_fantasy.py` — A small review scene made from the actual converted Sketchfab meshes.
- `stylize_mesh.py` — Any source mesh made to sit in the painted world (fantasy_80s_art_asset_style_spec
- `terrain_props.py` — The terrain core's mega structures (the mesh part of the hybrid terrain,

### tools/canyon_lab
- `analyze.py` — Stage 2 of the canyon sandbox: read the heights a run made and say what
- `blender_view.py` — The canyon sandbox's scene with its meshes, rendered by Blender without a
- `canyon_lab.py` — Canyon sandbox (user 15.10): the canyon's relief generated on a small patch
- `decor.py` — Small decor of the canyon (user 15.10; v2 after "не натурально"): the
- `fine_relief.py` — The canyon's ground as the game would hold it (user 15.10: a 2 m cell where
- `gen.py` — The canyon's relief on a small patch (user 15.10: a biome worked out in a
- `noise.py` — Small fast fields for the canyon sandbox: smooth noise (a random grid
- `paths.py` — Ways up and across the canyon (user 15.10): natural stone bridges between
- `place.py` — Stage 3 of the canyon sandbox: the meshes put on the sites the analysis
- `render.py` — Pictures of the sandbox's heights: hillshade, slope, a coloured map with

### tools/game_assets
- `bake_animals.py` — Bake the sim's beasts: Quaternius Ultimate Animated Animals (glTF) and Farm Animals
- `bake_buildings.py` — Bake the village's buildings and works for the game: blueprints assembled from the
- `bake_people.py` — Bake the game's people: Quaternius base heads + fantasy outfits + hair on the UAL rig,
- `kit_fe.py` — Kit fantasy_european: assemble a blueprint (2 m cells, walls on cell edges, roof parts)
- `make_icons.py` — Rasterise the HUD's line icons (the stroke paths of the design canvas "Altera UI и HUD",
- `proc_beasts.py` — Procedural low-poly beasts for bake_animals.py: hens and fish, built in an

### tools/gpu_profiler
- `altera_gpu_layer.cpp` — altera_gpu_layer - a Vulkan layer that times the GPU work of any Vulkan

### tools/sim_view
- `main.cpp` — altera_sim_view - watch and play the Altera life-sim in a browser.

### tools/terrain_lab
- `build_foliage.py` — build_foliage - the terrain core's foliage models, from new_assets.
- `build_grass.py` — build_grass - the grass cover's sprites, from new_assets/kenney_foliageSprites.zip.
- `build_materials.py` — build_materials - the terrain core's ground pictures, from new_assets/Free.zip.
- `build_world.py` — Builds the generated world, start to finish, into terrain_lab_out/world_*.
- `canyon_features.py` — The canyon's meshes in the world (user 15.10): the sandbox's analysis and
- `canyon_relief.py` — The canyon steppe from the sandbox (tools/canyon_lab, user 15.10: "Да,
- `import_megalith.py` — import_megalith - a magic megalith made in Blender (doc/megaliths/) into the
- `import_old_world.py` — import_old_world - the old assembled world (doc/OldWorld/assembled) as a
- `main.cpp` — terrain_lab - the terrain core alone, on an empty scene, with numbers.
- `make_crust.py` — Cracked orange clay crust for the canyon floors (doc/BIOME_PLAN.md 3.3): a seamless
- `marsh_hill_mesh.py` — marsh_hill_mesh - a terraced hill of the great marshes as a mesh: the
- `marsh_islands.py` — marsh_islands - the layout of the great marshes (doc/BIOME_PLAN.md 3.1,
- `regenerate.py` — Another world by the same rules, another seed - started from the lab with G.
- `reshape_v5.py` — Reshape the v5 heightmap so water can do what it should.
- `stele_glade.py` — stele_glade - the glade the carved stele A-2 stands in
- `steppe_checks.py` — The canyon steppe's own checks (doc/BIOME_PLAN.md 3.3, common ones in 1.5),
- `steppe_ground.py` — The canyon steppe's and the stone desert's ground (doc/BIOME_PLAN.md 3.3).
- `steppe_relief.py` — The canyon steppe's relief cut into mesas (doc/BIOME_PLAN.md 3.3, the
- `sweep.py` — sweep - run terrain_lab's benchmark over a matrix of parameters, write a report.
- `world_buildings.py` — world_buildings - built structures (doc/BIOMES_RU.md): the desert's buried
- `world_canyon_arch.py` — world_canyon_arch - the canyon's chain of portals A-4 in the world
- `world_cliffs.py` — world_cliffs - the cliff walls of the hybrid terrain (doc/rework/MESH_TERRAIN_PLAN.md).
- `world_features.py` — world_features - where the mega structures of a generated world stand (the
- `world_gate.py` — world_gate - the desert gate A-3 in the world (doc/megaliths/A3_DESERT_GATE.md),
- `world_gen.py` — World generator driven by rules (doc/rework/WORLD_GENERATOR_PLAN.md).
- `world_megaliths.py` — world_megaliths - the megaliths (doc/rework/MESH_TERRAIN_PLAN.md): strange,
- `world_stele.py` — world_stele - the carved stele A-2 and its fallen pieces in the world
- `world_terrain_parts.py` — world_terrain_parts - the large parts of the hybrid terrain that are
- `world_zones.py` — Where things go: the continent's shape is read, covered with patches of natural

## content/ (данные)
- `content/ai/` — 18 json
- `content/animals/` — 6 json
- `content/animations/` — 2 json
- `content/buildings/` — 3 json
- `content/combat/` — 15 json
- `content/config/` — 58 json
- `content/crops/` — 1 json
- `content/ethnos/` — 4 json
- `content/items/` — 6 json
- `content/knowledge/` — 1 json
- `content/loc/` — 22 json
- `content/presets/` — 30 json
- `content/recipes/` — 8 json
- `content/resources/` — 3 json
- `content/sim/` — 1 json
- `content/traits/` — 1 json
- `content/ui/` — 1 json
