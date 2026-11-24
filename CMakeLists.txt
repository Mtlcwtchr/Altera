cmake_minimum_required(VERSION 3.24)
project(altera LANGUAGES CXX)

# ALTERA_SIM_ONLY - configure only the headless life-sim: altera_sim,
# altera_combat, altera_sim_runner, altera_combat_tables and their tests. No
# fetches, no engine, no SDL/D3D12, no shaders or assets, so a configure takes
# seconds and a build needs nothing but a C++20 compiler (Linux included).
# Used by the "sim" presets (CMakePresets.json, build dir cmake-build-sim) and
# tools/check_fast.bat. The shared cmake-build-windows stays the full build.
option(ALTERA_SIM_ONLY "Configure only the headless life-sim and its tests" OFF)

if(NOT ALTERA_SIM_ONLY)
include(FetchContent)

# EnTT is header-only. Keep it as a fetched, pinned dependency so the
# simulation can use an ECS without coupling itself to SDL or the renderer.
FetchContent_Declare(EnTT
        GIT_REPOSITORY https://github.com/skypjack/entt.git
        GIT_TAG v3.15.0
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(EnTT)

# zstd compresses the world's chunk files (engine/world_store). A pinned fetch
# like EnTT and SDL, built static and without its command-line programs: the
# engine needs the library, not a second copy of the zstd tool.
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG v1.5.7
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR build/cmake
        EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(zstd)

# zlib is used by engine/world_source/png_io for PNG inflate/deflate. Fetch a
# pinned static copy so Windows builds do not depend on a separately installed
# SDK package being present for FindZLIB.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG v1.3.1
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(zlib)
if(TARGET zlibstatic AND NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
elseif(TARGET zlib AND NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS zlib)
endif()

# meshoptimizer - offline only: tools/model_simplify reduces the multi-million
# triangle source plants and rocks (assets/models/polyhaven) to import budgets.
# Nothing in the engine links it.
FetchContent_Declare(meshoptimizer
        GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
        GIT_TAG v1.1
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(meshoptimizer)
endif() # NOT ALTERA_SIM_ONLY

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ASR_BUILD_CLIENT "Build the SDL3 graphical client" ON)
option(ASR_BUILD_ENGINE_EDITOR "Build the standalone engine module editor" ${ASR_BUILD_CLIENT})
option(ASR_ENGINE_EDITOR_GPU "Use the engine GPU viewport (OFF keeps a DXC-free CPU editor)" ON)
option(ASR_BUILD_TESTS "Build the test suite" ON)
option(ASR_TEST_GPU "Register tests requiring a window system and GPU with CTest" OFF)
option(ASR_ENABLE_PROFILING "Legacy default for ASR_ENABLE_DIAGNOSTICS" OFF)
option(ASR_ENABLE_DIAGNOSTICS "Compile metrics, profiling UI, traces and diagnostic readbacks" ${ASR_ENABLE_PROFILING})
# Retain old source guards/tool configurations; the new master switch wins.
set(ASR_ENABLE_PROFILING ${ASR_ENABLE_DIAGNOSTICS})
option(ASR_ENABLE_FPS "Compile the lightweight wall-clock FPS display" ON)
# The frame counter stands on its own, and used to not: it was ANDed with the
# diagnostics switch, so with diagnostics off - which is how this is normally
# built - the one number anybody actually wants while flying around was compiled
# out along with the profiler. It is two doubles and a counter; it does not need
# the machinery it was chained to.
add_compile_definitions(ASR_ENABLE_PROFILING=$<BOOL:${ASR_ENABLE_PROFILING}>
        ASR_ENABLE_DIAGNOSTICS=$<BOOL:${ASR_ENABLE_DIAGNOSTICS}>
        ASR_ENABLE_FPS=$<BOOL:${ASR_ENABLE_FPS}>)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Debug builds this engine at -Og, not -O0, and the reason is that -O0 does not
# produce a program anybody can look at.
#
# Nearly everything here is written to be free after inlining: every arithmetic
# operator on core::Fixed is a function, the surface a mesh is built from
# arrives as a std::function called millions of times a page, and the geometry
# is templates throughout. At -O0 none of that is folded, so the same frame
# costs several times what it costs when built to run - and the frame rate,
# which is the thing most often being judged, becomes a number about the
# compiler rather than about the game. A day went into "why is this thirty
# frames a second" that was only ever the profile.
#
# -Og keeps the debugger honest: it is the optimisation level clang documents
# for debugging, line tables stay usable and variables mostly survive. Asserts
# stay on, because NDEBUG is what a debug build is FOR - the slowness never was.
#
# Written only when it is still CMake's own default. Forcing it on every
# configure rewrites the cache every time, and a rewritten cache is a reason to
# reconfigure - which on this project means rebuilding the vendored shader
# compiler, half an hour of it, for a flag nobody changed.
if(CMAKE_CXX_FLAGS_DEBUG STREQUAL "-g")
    set(CMAKE_C_FLAGS_DEBUG "-Og -g" CACHE STRING "" FORCE)
    set(CMAKE_CXX_FLAGS_DEBUG "-Og -g" CACHE STRING "" FORCE)
endif()

add_library(asr_warnings INTERFACE)
target_compile_options(asr_warnings INTERFACE
        $<$<AND:$<CXX_COMPILER_ID:AppleClang,Clang,GNU>,$<CXX_COMPILER_FRONTEND_VARIANT:GNU>>:-Wall -Wextra -Wpedantic -Wno-unused-parameter>
        # clang-cl reads -Wall as -Weverything: a gigabyte of C++98-compat noise per build.
        $<$<AND:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_FRONTEND_VARIANT:MSVC>>:/W3 -Wno-unused-parameter>
        # MSVC: Allow multiple processes to write to the same .PDB file during parallel compilation
        $<$<CXX_COMPILER_ID:MSVC>:/FS>
        # CMake's MSVC-style RelWithDebInfo is /O2 /Ob1: only functions marked inline
        # get inlined. clang/gcc RelWithDebInfo (-O2 -g) inline freely; match that.
        $<$<AND:$<CONFIG:RelWithDebInfo>,$<CXX_COMPILER_FRONTEND_VARIANT:MSVC>>:/Ob2>)

# Vendored single-header JSON (content is declarative text data, GDD 11).
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${CMAKE_SOURCE_DIR}/third_party)

if(NOT ALTERA_SIM_ONLY)
# ---------------------------------------------------------------------------
# engine/core - fixed point, deterministic RNG, ids, calendar. No game concepts.
# ---------------------------------------------------------------------------
add_library(asr_core STATIC
        src/engine/core/fixed.cpp
        src/engine/core/geometry.cpp
        src/engine/core/time.cpp
        src/engine/core/profiler.cpp)
target_include_directories(asr_core PUBLIC src)
target_link_libraries(asr_core PRIVATE asr_warnings)
# Fixed divides __int128. clang-cl lowers that to __divti3, which the MSVC
# runtime does not have; it lives in clang's own builtins library.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    get_filename_component(ASR_CLANG_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    file(GLOB ASR_CLANG_BUILTINS "${ASR_CLANG_BIN}/../lib/clang/*/lib/windows/clang_rt.builtins-x86_64.lib")
    if(NOT ASR_CLANG_BUILTINS)
        message(FATAL_ERROR "clang_rt.builtins-x86_64.lib not found next to ${CMAKE_CXX_COMPILER}")
    endif()
    list(GET ASR_CLANG_BUILTINS 0 ASR_CLANG_BUILTINS)
    target_link_libraries(asr_core PUBLIC "${ASR_CLANG_BUILTINS}")
endif()
include(src/engine/modules.cmake)

# The entity system itself, and nothing else: registry, spatial cells, the
# snapshot boundary, deterministic batching, the worker command buffer. No game
# vocabulary, no rendering, no window. Header-only, so it is what it says it is.
add_library(asr_ecs INTERFACE)
target_include_directories(asr_ecs INTERFACE src)
target_link_libraries(asr_ecs INTERFACE asr_core EnTT::EnTT)

# Render systems that need no GPU: the ones that read entities and produce what
# a renderer will draw. Separate from asr_engine_render because deciding what a
# frame contains is not the same job as putting it on a card, and because this
# half has to build and be tested without a window.
add_library(asr_render_systems STATIC
        src/engine/render/systems/instance_gather.cpp
        src/engine/render/systems/level_select.cpp
        src/engine/render/systems/frustum_cull.cpp
        src/engine/render/systems/terrain_gather.cpp
        src/engine/render/systems/cluster_draws.cpp
        src/engine/render/systems/source_cluster_draws.cpp
        src/engine/render/systems/instance_hierarchy_select.cpp)
target_include_directories(asr_render_systems PUBLIC src)
target_link_libraries(asr_render_systems PUBLIC asr_ecs asr_geometry asr_representation PRIVATE asr_warnings)

# Reusable CPU geometry/reconstruction: no game, window or GPU dependency.
add_library(asr_geometry STATIC src/engine/geometry/smart_terrain_mesh.cpp)
target_include_directories(asr_geometry PUBLIC src)
target_link_libraries(asr_geometry PUBLIC asr_virtual_geometry PRIVATE asr_warnings)

# ---------------------------------------------------------------------------
# content - declarative definitions and their loader/validator.
# ---------------------------------------------------------------------------
add_library(asr_content STATIC
        src/game/content/defs.cpp
        src/game/content/content_db.cpp
        src/game/content/ground_materials.cpp)
target_include_directories(asr_content PUBLIC src)
target_link_libraries(asr_content PUBLIC asr_core PRIVATE nlohmann_json asr_warnings)

# ---------------------------------------------------------------------------
# generation - terrain, resources, hearth placement.
# ---------------------------------------------------------------------------
add_library(asr_generation STATIC
        src/game/generation/hybrid_terrain.cpp
        src/game/generation/terrain_foundation.cpp
        src/game/generation/local_map_gen.cpp
        src/game/generation/world_domain.cpp
        src/game/generation/world_scale_policy.cpp
        src/game/generation/world_macro.cpp
        src/game/generation/land_coverage.cpp
        src/game/generation/world_noise.cpp
        src/game/generation/world_layers.cpp
        src/game/generation/world_brush.cpp
        src/game/generation/world_layout.cpp
        src/game/generation/world_compose.cpp
        src/game/generation/world_sketch.cpp
        src/game/generation/world_import.cpp
        src/game/generation/world_procedural.cpp
        src/game/generation/world_map_gen.cpp)
target_include_directories(asr_generation PUBLIC src)
target_compile_definitions(asr_generation PRIVATE ASR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(asr_generation PUBLIC asr_core asr_content asr_terrain_generator asr_biomes asr_environment EnTT::EnTT
        PRIVATE asr_world_source nlohmann_json asr_warnings)

# ---------------------------------------------------------------------------
# world - the ground as geometry: the height field, chunk meshes, cliffs.
# Continuous world coordinates; chunks are storage and streaming, not borders.
# ---------------------------------------------------------------------------
add_library(asr_world STATIC
        src/game/world/world_builder.cpp
        src/game/world/surface_reconstruction.cpp
        src/game/world/scene_placement.cpp
        src/game/world/edit_layer.cpp
        src/game/world/terrain_brush.cpp
        src/game/world/height_field.cpp
        src/game/world/soil.cpp
        src/game/world/terrain_mesh.cpp
        src/game/world/terrain_grid.cpp
        src/game/world/terrain_lod.cpp
        src/game/world/terrain_config.cpp
        src/game/world/terrain_plan.cpp
        src/game/world/terrain_seams.cpp
        src/game/world/terrain_detail.cpp
        src/game/world/ring_mesh.cpp
        src/game/world/tile_mesh.cpp
        src/game/world/climate_field.cpp
        src/game/world/terrain_streaming/base_tile_baker.cpp
        src/game/world/terrain_streaming/baked_page_cache.cpp
        src/game/world/terrain_streaming/graph_carve.cpp
        src/game/world/terrain_streaming/hydrology_builder.cpp
        src/game/world/terrain_streaming/hydrology_selection.cpp
        src/game/world/terrain_streaming/page_ground.cpp
        src/game/world/terrain_streaming/page_store.cpp
        src/game/world/terrain_streaming/hydrology_cache.cpp
        src/game/world/terrain_streaming/hydrology_graph.cpp
        src/game/world/terrain_streaming/water_tile.cpp
        src/game/world/terrain_streaming/tile_cache.cpp
        src/game/world/terrain_streaming/worker_pool.cpp
        src/game/world/terrain_streaming/worker_runtime.cpp
        src/game/world/spatial.cpp
        src/game/world/navmesh.cpp
        src/game/world/macro.cpp
        src/game/environment/world_environment.cpp
        src/game/environment/natural_zones.cpp)
target_include_directories(asr_world PUBLIC src)
target_sources(asr_world PRIVATE src/game/world/scene_scatter.cpp)
target_sources(asr_world PRIVATE src/game/world/scene_entities.cpp)
target_sources(asr_world PRIVATE src/game/world/terrain_source.cpp)
target_sources(asr_world PRIVATE src/game/world/terrain_source_tree.cpp)
# The persistent delta: the world's history over its generated base, saved
# chunk by chunk under the world root (engine/world_store).
target_sources(asr_world PRIVATE src/game/world/world_delta.cpp src/game/world/world_delta_codec.cpp
        src/game/world/world_saves.cpp src/game/world/world_tools.cpp)
target_link_libraries(asr_world PUBLIC asr_core asr_generation asr_geometry asr_render_systems asr_environment
        PRIVATE nlohmann_json asr_warnings)
target_link_libraries(asr_world PUBLIC asr_world_store)

# ---------------------------------------------------------------------------
# simulation - the headless world. Knows nothing about rendering or input.
# game/work is a separate work pipeline; it reads world/ECS state but is not an
# ECS system and is not part of the actor tick scheduler.
# ---------------------------------------------------------------------------
add_library(asr_simulation STATIC
        src/game/ai/planner.cpp
        src/game/ecs/render/extraction.cpp
        src/game/simulation/world.cpp
        src/game/simulation/farming.cpp
        src/game/simulation/livestock.cpp
        src/game/simulation/pathfinder.cpp
        src/game/simulation/inventory.cpp
        src/game/simulation/needs.cpp
        src/game/work/planner.cpp
        src/game/work/execution.cpp
        src/game/simulation/population.cpp
        src/game/simulation/zones.cpp
        src/game/simulation/report.cpp
        src/game/simulation/savegame.cpp)
target_include_directories(asr_simulation PUBLIC src)
target_link_libraries(asr_simulation PUBLIC asr_core asr_content asr_generation asr_world EnTT::EnTT
        PRIVATE asr_warnings)

# ---------------------------------------------------------------------------
# sim_runner - headless scenario runs (GDD 11: the agent must be able to build,
# run a scenario, check invariants and get a report with no graphical client).
# ---------------------------------------------------------------------------
add_executable(sim_runner sim_runner/main.cpp)
target_link_libraries(sim_runner PRIVATE asr_simulation asr_warnings)
endif() # NOT ALTERA_SIM_ONLY

# ---------------------------------------------------------------------------
# altera_sim - the Altera life-sim core (doc/design/ALTERA_CORE_SPEC_v0.1.md):
# characters, 4-layer AI (Utility -> HTN -> GOAP -> FSM), controllers. Headless,
# deterministic from seed + command log; depends on nothing but the JSON header.
# ---------------------------------------------------------------------------
add_library(altera_sim STATIC
        src/sim/rules.cpp
        src/sim/content.cpp
        src/sim/expansion.cpp
        src/sim/backstory.cpp
        src/sim/trade.cpp
        src/sim/collide.cpp
        src/sim/act_info.cpp
        src/sim/consequences.cpp
        src/sim/dialogue_gen.cpp
        src/sim/dialogue_rules.cpp
        src/sim/rumor_noise.cpp
        src/sim/dialogue_say.cpp
        src/sim/speech_feed.cpp
        src/sim/justice.cpp
        src/sim/facts.cpp
        src/sim/village_life.cpp
        src/sim/realm.cpp
        src/sim/memory_budget.cpp
        src/sim/dialogue_memory.cpp
        src/sim/dialogue_corpus.cpp
        src/sim/gear.cpp
        src/sim/validate.cpp
        src/sim/character.cpp
        src/sim/world.cpp
        src/sim/worldgen.cpp
        src/sim/village_plan.cpp
        src/sim/structure_plan.cpp
        src/sim/rooms.cpp
        src/sim/ai_eval.cpp
        src/sim/animals.cpp
        src/sim/farming.cpp
        src/sim/weather.cpp
        src/sim/care.cpp
        src/sim/ai_utility.cpp
        src/sim/ai_htn.cpp
        src/sim/ai_goap.cpp
        src/sim/ai_mind.cpp
        src/sim/ai_control.cpp
        src/sim/loc.cpp
        src/sim/loc_gen.cpp
        src/sim/combat_bridge.cpp
        src/sim/wounds.cpp
        src/sim/aftermath.cpp
        src/sim/group_combat.cpp
        src/sim/captivity.cpp
        src/sim/tones.cpp
        src/sim/social_chain.cpp
        src/sim/ai_report.cpp
        src/sim/craft/craft_expr.cpp
        src/sim/craft/craft_data.cpp
        src/sim/craft/craft.cpp
        src/sim/craft/craft_chain.cpp
        src/sim/craft/craft_validate.cpp
        src/sim/craft/craft_state.cpp
        src/sim/craft_sim.cpp
        src/sim/building.cpp)
target_include_directories(altera_sim PUBLIC src)
target_link_libraries(altera_sim PUBLIC nlohmann_json altera_combat PRIVATE asr_warnings)

# altera_combat - combat core (gdd/combat/COMBAT_CORE_SPEC_v0.1.md): a port of
# the combat_sim.py prototype, numbers from content/combat/*.json, deterministic
# per seed, output is an event stream (no rendering).
add_library(altera_combat STATIC
        src/sim/combat/combat_data.cpp
        src/sim/combat/combat.cpp
        src/sim/combat/combat_anim.cpp
        src/sim/combat/combat_lod.cpp)
target_include_directories(altera_combat PUBLIC src)
target_link_libraries(altera_combat PUBLIC nlohmann_json PRIVATE asr_warnings)

add_executable(altera_combat_tables src/sim/combat/tool/combat_tables.cpp)
target_link_libraries(altera_combat_tables PRIVATE altera_combat asr_warnings)
target_compile_definitions(altera_combat_tables PRIVATE ALTERA_SIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

add_executable(altera_craft_report src/sim/craft/tool/craft_report.cpp)
target_link_libraries(altera_craft_report PRIVATE altera_sim asr_warnings)
target_compile_definitions(altera_craft_report PRIVATE ALTERA_SIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

add_executable(altera_sim_runner src/sim/runner/main.cpp)
target_link_libraries(altera_sim_runner PRIVATE altera_sim asr_warnings)
target_compile_definitions(altera_sim_runner PRIVATE ALTERA_SIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# altera_sim_view - watch and play the life-sim in a browser (local HTTP page,
# placeholder visuals until the sim moves into the game client).
if(WIN32)
    add_executable(altera_sim_view tools/sim_view/main.cpp)
    target_link_libraries(altera_sim_view PRIVATE altera_sim ws2_32 asr_warnings)
    target_compile_definitions(altera_sim_view PRIVATE ALTERA_SIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()

# AL-95: the 2D client (gdd/ui/client_2d): src/client2d reads the sim (snapshot ->
# 1 m cells, people), altera_game_2d serves the page tools/game_2d/web. Windows and Linux.
add_library(altera_client2d STATIC src/client2d/grid2d.cpp src/client2d/view2d.cpp src/client2d/acts2d.cpp src/client2d/talk2d.cpp)
target_include_directories(altera_client2d PUBLIC src)
target_link_libraries(altera_client2d PUBLIC altera_sim PRIVATE asr_warnings)
target_sources(altera_client2d PRIVATE src/client2d/sheet2d.cpp)   # AL-100: the character sheet
add_executable(altera_game_2d tools/game_2d/main.cpp)
target_link_libraries(altera_game_2d PRIVATE altera_client2d asr_warnings)
if(WIN32)
    target_link_libraries(altera_game_2d PRIVATE ws2_32)
endif()
target_compile_definitions(altera_game_2d PRIVATE ALTERA_SIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

if(NOT ALTERA_SIM_ONLY)
# ---------------------------------------------------------------------------
# tools - content validator.
# ---------------------------------------------------------------------------
# terrain_lint - looks for the marks the generator leaves on its own work:
# steps that should be slopes, patterns that line up with the axes, rivers drawn
# as dashed lines, levels of detail that disagree.
add_executable(terrain_lint tools/terrain_lint.cpp)
target_link_libraries(terrain_lint PRIVATE asr_world asr_warnings)

# world_root_inspect - what a world root holds on disk: chunks, revisions,
# blocks, journals, and whether each file is the one its manifest names.
add_executable(world_root_inspect tools/world_root_inspect.cpp)
target_link_libraries(world_root_inspect PRIVATE asr_world asr_warnings)

# world_load_probe - what opening a world costs, pass by pass: generation,
# the snapshot, the prebake of pages.
add_executable(worldtool tools/worldtool.cpp)
target_link_libraries(worldtool PRIVATE asr_world_source asr_biomes asr_warnings)

add_executable(world_load_probe tools/world_load_probe.cpp)
target_link_libraries(world_load_probe PRIVATE asr_world asr_generation asr_world_source asr_warnings)

add_executable(terrain_resolution_probe tools/terrain_resolution_probe.cpp)
target_link_libraries(terrain_resolution_probe PRIVATE asr_world nlohmann_json asr_warnings)

add_executable(terrain_subdivision_probe tools/terrain_subdivision_probe.cpp)
target_link_libraries(terrain_subdivision_probe PRIVATE asr_world nlohmann_json asr_warnings)

# lake_edge_probe - counts lake water that ends over lower ground (a lake
# hanging in the air) in the baked pages of every lake the graph holds.
add_executable(lake_edge_probe tools/lake_edge_probe.cpp)
target_link_libraries(lake_edge_probe PRIVATE asr_world asr_warnings)

# reach_probe - one river's course, head and banks, point by point.
add_executable(reach_probe tools/reach_probe.cpp)
target_link_libraries(reach_probe PRIVATE asr_world asr_warnings)

add_executable(terrain_feature_probe tools/terrain_feature_probe.cpp)
target_link_libraries(terrain_feature_probe PRIVATE asr_world nlohmann_json asr_warnings)

add_executable(hybrid_terrain_probe tools/hybrid_terrain_probe.cpp)
target_link_libraries(hybrid_terrain_probe PRIVATE asr_world nlohmann_json asr_warnings)

add_executable(mountain_shape_probe tools/mountain_shape_probe.cpp)
target_link_libraries(mountain_shape_probe PRIVATE asr_world nlohmann_json asr_warnings)

add_executable(world_scale_policy_probe tools/world_scale_policy_probe.cpp)
target_link_libraries(world_scale_policy_probe PRIVATE asr_generation nlohmann_json asr_warnings)

add_executable(world_scale_probe tools/world_scale_probe.cpp)
target_link_libraries(world_scale_probe PRIVATE asr_world asr_generation nlohmann_json asr_warnings)

# scene_model_clusters - the offline half of virtual geometry for props:
# reads the meshes prepare_scene_models.py wrote and puts a cluster DAG beside
# each one. See doc/reports/vg_step_3.
add_executable(scene_model_clusters tools/scene_model_clusters.cpp)
target_link_libraries(scene_model_clusters PRIVATE asr_geometry nlohmann_json asr_warnings)

# model_simplify - reduces the downloaded source models (trees, shrubs, ground
# plants, deadwood, rocks) to per-role triangle budgets: solid parts by edge
# collapse, scattered leaves/needles/blades by per-piece reduction and thinning
# with coverage-matched growth. See doc/DECISIONS.md D168.
# A build tree configured FETCHCONTENT_FULLY_DISCONNECTED without a populated
# meshoptimizer simply has no model_simplify; nothing else depends on it.
if(TARGET meshoptimizer)
    add_executable(model_simplify tools/model_simplify.cpp)
    target_link_libraries(model_simplify PRIVATE meshoptimizer nlohmann_json asr_warnings)
else()
    message(STATUS "meshoptimizer not populated: model_simplify is not built")
endif()

# impostor_bake - the impostor rasterizer of prepare_scene_models.py and the
# depth code of bake_impostor_depth.py, natively and byte-identical; driven by
# tools/bake_model_impostors.py. No dependencies.
add_executable(impostor_bake tools/impostor_bake.cpp)
target_link_libraries(impostor_bake PRIVATE asr_warnings)

add_executable(content_validator tools/content_validator.cpp)
target_link_libraries(content_validator PRIVATE asr_content asr_simulation asr_environment asr_world asr_warnings)

# environment_probe - what the procedural environment makes of a world:
# zones, features, masks, ground moved, and whether it is deterministic.
add_executable(feature_forms tools/feature_forms.cpp)
target_link_libraries(feature_forms PRIVATE asr_environment asr_warnings)

add_executable(environment_probe tools/environment_probe.cpp)
target_link_libraries(environment_probe PRIVATE asr_world asr_generation asr_environment asr_warnings)

# ---------------------------------------------------------------------------
# client - SDL3 top-down renderer. Optional; the simulation never links it.
# ---------------------------------------------------------------------------
if(ASR_BUILD_CLIENT OR ASR_BUILD_ENGINE_EDITOR)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG release-3.2.8
            GIT_SHALLOW TRUE
            EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(SDL3)

    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_TESTS OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_SAMPLES OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_AVIF OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_AVIF_SAVE OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_JXL OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_LIBTIFF OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_WEBP OFF CACHE BOOL "" FORCE)
    # Packed RG16 depth must bypass platform premultiplication/color management.
    set(SDLIMAGE_BACKEND_STB ON CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3_image
            GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
            GIT_TAG release-3.2.4
            GIT_SHALLOW TRUE
            EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(SDL3_image)

    add_library(asr_engine_gui STATIC src/engine/ui/ui.cpp src/engine/ui/font.cpp src/engine/editor/module_panel.cpp
            src/engine/ui/profiler_window.cpp)
    target_include_directories(asr_engine_gui PUBLIC src)
    # stb_truetype, vendored like the JSON reader (third_party/stb).
    target_include_directories(asr_engine_gui PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
    target_link_libraries(asr_engine_gui PUBLIC asr_core asr_representation asr_terrain_generator
            asr_virtual_geometry SDL3::SDL3-static PRIVATE asr_warnings)
    add_library(Campfire::Gui ALIAS asr_engine_gui)
    if(ASR_BUILD_CLIENT OR (ASR_BUILD_ENGINE_EDITOR AND ASR_ENGINE_EDITOR_GPU))
        include(src/engine/gpu_modules.cmake)
    endif()
    if(ASR_BUILD_ENGINE_EDITOR)
        add_executable(campfire_engine_editor src/engine/editor/main.cpp)
        target_link_libraries(campfire_engine_editor PRIVATE asr_engine_gui
                SDL3_image::SDL3_image-static nlohmann_json asr_warnings)
        target_compile_definitions(campfire_engine_editor PRIVATE ASR_ENGINE_EDITOR_GPU=$<BOOL:${ASR_ENGINE_EDITOR_GPU}>)
        if(ASR_ENGINE_EDITOR_GPU)
            target_link_libraries(campfire_engine_editor PRIVATE asr_engine_viewport)
        endif()
    endif()
endif()

if(ASR_BUILD_CLIENT)
    # The editor is its own program: it opens no world and ticks nothing, so it
    # starts in a second rather than after a continent has been generated.
    add_executable(altera_editor
            src/game/editor/main.cpp
            src/game/ui/editor.cpp)
    target_link_libraries(altera_editor PRIVATE asr_simulation asr_engine_gui nlohmann_json SDL3::SDL3-static
            SDL3_image::SDL3_image-static asr_warnings)

    # This game's passes. They know what ground is made of and what grass is; the
    # engine does not, which is the whole point of the split.
    add_library(asr_game_render STATIC
            src/game/render/world_renderer.cpp
            src/game/render/pass_ids.cpp
            src/game/render/grass_cull.cpp
            src/game/render/passes/terrain_pass.cpp
            src/game/render/passes/foliage_pass.cpp
            src/game/render/passes/far_trees_pass.cpp
            src/game/render/passes/water_pass.cpp
            src/game/render/passes/weather_pass.cpp
            src/game/render/passes/hold_pass.cpp
            src/game/render/passes/sketch_pass.cpp
            src/game/render/passes/highlight_pass.cpp
            src/game/render/passes/sky_pass.cpp src/game/render/passes/frustum_pass.cpp
            src/game/render/passes/menu_pass.cpp
            src/game/render/passes/sprite_pass.cpp
            src/game/client/explore_menu.cpp src/game/client/graphics_panel.cpp
            src/game/client/world_editor.cpp src/game/client/world_editor_biomes.cpp
            src/game/render/calc/terrain_stream.cpp
            src/game/render/calc/terrain_collect.cpp
            src/game/render/climate_textures.cpp
            src/game/render/biome_textures.cpp
            src/game/render/height_page_atlas.cpp
            src/game/render/height_page_stream.cpp
            src/game/render/gpu_terrain.cpp
            src/game/render/terrain_material_cache.cpp
            src/game/render/calc/sprite_queue.cpp)
    target_include_directories(asr_game_render PUBLIC src)
    target_sources(asr_game_render PRIVATE src/game/render/passes/scene_models_pass.cpp
            src/game/render/shadow_clipmap.cpp src/game/render/forest_proxy_cache.cpp
            src/game/render/forest_hierarchy.cpp src/game/render/graphics_settings.cpp
            src/game/render/terrain_look.cpp
            src/game/render/character_model.cpp src/game/render/passes/character_pass.cpp
            src/game/client/character_controller.cpp src/game/client/hero_camera.cpp)
    if(ASR_ENABLE_PROFILING)
        target_sources(asr_game_render PRIVATE src/game/render/world_renderer_diagnostics.cpp)
    endif()
    target_link_libraries(asr_game_render PUBLIC asr_engine_render asr_world asr_camera asr_animation asr_virtual_geometry_gpu
            asr_impostors_gpu asr_world_source PRIVATE nlohmann_json asr_warnings)

    add_executable(altera_client
            src/game/client/main.cpp
            src/engine/core/profiler_alloc.cpp
            src/game/client/renderer.cpp
            src/game/client/selection.cpp
            src/game/client/controls.cpp
            src/game/client/newgame_screen.cpp
            src/game/client/world_dials.cpp
            src/game/client/explorer.cpp
            src/game/client/explore_view.cpp
            src/game/client/explore_bench.cpp
            src/game/client/client_ui.cpp
            src/game/client/terrain_panel.cpp
            src/game/client/asset_browser.cpp
            src/game/client/client_app.cpp
            src/game/ui/hud.cpp
            src/game/ui/editor.cpp
            src/game/ui/overlay.cpp
            src/game/ui/info_panel.cpp)
    target_link_libraries(altera_client PRIVATE asr_simulation asr_world asr_game_render asr_engine_gui nlohmann_json
            SDL3::SDL3-static SDL3_image::SDL3_image-static
            SDL3_shadercross::SDL3_shadercross-static nlohmann_json asr_warnings)
    if(ASR_ENABLE_PROFILING)
        target_sources(altera_client PRIVATE src/game/client/explore_diagnostics.cpp)
    endif()

    # asr_mesh_lab - UE Nanite's bake against our cluster DAG on the same meshes,
    # rendered on the CPU into PNGs and a table. No window, no GPU. Input comes
    # from tools/ue_nanite_reference.py (headless Unreal).
    add_executable(asr_mesh_lab tools/mesh_lab.cpp)
    target_link_libraries(asr_mesh_lab PRIVATE asr_geometry nlohmann_json
            SDL3::SDL3-static SDL3_image::SDL3_image-static asr_warnings)

    # The terrain core (doc/rework/TERRAIN_REWORK_PLAN.md): macro map, GPU
    # clipmap, CDLOD patches. Depends on the device and nothing of the game.
    add_library(altera_terrain_core STATIC
            src/terrain_core/macro_map.cpp
            src/terrain_core/biomes.cpp
            src/terrain_core/erosion.cpp
            src/terrain_core/drainage.cpp
            src/terrain_core/terrain_gen.cpp
            src/terrain_core/world_gen.cpp
            src/terrain_core/foliage.cpp
            src/terrain_core/far_detail.cpp
            src/terrain_core/props.cpp
            src/terrain_core/grass.cpp
            src/terrain_core/post.cpp
            src/terrain_core/psx.cpp
            src/terrain_core/outline.cpp
            src/terrain_core/enb.cpp
            src/terrain_core/lighting.cpp
            src/terrain_core/sky.cpp
            src/terrain_core/hydrology.cpp
            src/terrain_core/ground_height.cpp
            src/terrain_core/water.cpp
            src/terrain_core/rivers.cpp
            src/terrain_core/relief.cpp
            src/terrain_core/lod_select.cpp
            src/terrain_core/renderer.cpp)
    target_include_directories(altera_terrain_core PUBLIC src)
    target_link_libraries(altera_terrain_core PUBLIC asr_engine_render asr_world_source PRIVATE nlohmann_json asr_warnings)

    # terrain_lab - the terrain core on an empty scene: fly it, or benchmark it.
    # altera_game_view - the life-sim drawn into the world view: its people,
    # beasts and buildings as a scene layer, the HUD, the player's orders
    # (src/game_view, doc engine_merge/MERGE_PLAN.md stage 4-6 fast path).
    add_library(altera_game_view STATIC src/game_view/sim_game.cpp src/game_view/loading_screen.cpp
                                       src/game_view/skinned.cpp src/game_view/statics.cpp src/game_view/hud.cpp
                                       src/game_view/hud_menu.cpp src/game_view/hud_edge.cpp
                                       src/game_view/hud_edge_hud.cpp src/game_view/hud_edge_bar.cpp src/game_view/hud_edge_social.cpp
                                       src/game_view/hud_edge_screens.cpp src/game_view/hud_edge_trade.cpp src/game_view/hud_world.cpp
                                       src/game_view/saves.cpp)
    target_include_directories(altera_game_view PUBLIC src)
    target_link_libraries(altera_game_view PUBLIC altera_terrain_core altera_sim asr_engine_gui asr_world_source PRIVATE asr_warnings)

    add_executable(terrain_lab tools/terrain_lab/main.cpp)
    target_link_libraries(terrain_lab PRIVATE altera_terrain_core altera_game_view asr_warnings)
    target_compile_definitions(terrain_lab PRIVATE _CRT_SECURE_NO_WARNINGS ALTERA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    altera_configure_dlss(altera_terrain_core terrain_lab)

    # altera_game - the game on the lab's world view: the life-sim with the
    # player's HUD from the start (terrain_lab --game, under its own name).
    # altera_world_gen - a procedural world (AL-69) into its folder, with its
    # picture and checks, without the game: altera_world_gen --spec '{...}' --out DIR
    add_executable(altera_world_gen tools/world_gen_cli.cpp)
    target_link_libraries(altera_world_gen PRIVATE altera_terrain_core asr_warnings)

    add_executable(altera_game tools/terrain_lab/main.cpp)
    target_link_libraries(altera_game PRIVATE altera_terrain_core altera_game_view asr_warnings)
    target_compile_definitions(altera_game PRIVATE _CRT_SECURE_NO_WARNINGS ALTERA_GAME_DEFAULT=1
            ALTERA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    if(ASR_DLSS_AVAILABLE)
        altera_copy_dlss_runtime(altera_game)
    endif()
    if(ASR_BUILD_TESTS AND ASR_DLSS_AVAILABLE)
        add_executable(asr_dlss_gpu_check tests/test_dlss_gpu.cpp)
        target_link_libraries(asr_dlss_gpu_check PRIVATE altera_terrain_core asr_warnings)
        altera_copy_dlss_runtime(asr_dlss_gpu_check)
    endif()

    add_executable(asr_shader_check tools/shader_check.cpp)
    target_link_libraries(asr_shader_check PRIVATE SDL3::SDL3-static
            SDL3_shadercross::SDL3_shadercross-static asr_warnings)

    # water_edge_probe - where the drawn water fails to meet the drawn ground,
    # measured on the packed pages the renderer receives.
    add_executable(water_edge_probe tools/water_edge_probe.cpp)
    target_link_libraries(water_edge_probe PRIVATE asr_game_render asr_world asr_warnings)

    # The GPU profiler (tools/gpu_profile.py): a Vulkan layer that times any
    # Vulkan program from outside it. Nothing in the engine knows it exists.
    # Vulkan headers come with SDL; Metal has Instruments instead.
    if(NOT APPLE)
        add_library(altera_gpu_layer SHARED tools/gpu_profiler/altera_gpu_layer.cpp)
        target_include_directories(altera_gpu_layer PRIVATE "${sdl3_SOURCE_DIR}/src/video/khronos")
        set_target_properties(altera_gpu_layer PROPERTIES PREFIX "")
        file(GENERATE OUTPUT "$<TARGET_FILE_DIR:altera_gpu_layer>/VK_LAYER_ALTERA_gpu_profile.json" CONTENT
"{
    \"file_format_version\": \"1.2.0\",
    \"layer\": {
        \"name\": \"VK_LAYER_ALTERA_gpu_profile\",
        \"type\": \"GLOBAL\",
        \"library_path\": \"./$<TARGET_FILE_NAME:altera_gpu_layer>\",
        \"api_version\": \"1.3.0\",
        \"implementation_version\": \"1\",
        \"description\": \"Altera GPU profile: timestamps around every piece of GPU work, named after its shaders\"
    }
}
")
    endif()

    # Public altera_* target names, with backward-compatible asr_* executable
    # shims for older scripts and local habits.
    function(altera_binary target legacy)
        if(UNIX)
            add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E rm -f "$<TARGET_FILE_DIR:${target}>/${legacy}"
                    COMMAND "${CMAKE_COMMAND}" -E create_symlink "$<TARGET_FILE_NAME:${target}>"
                            "$<TARGET_FILE_DIR:${target}>/${legacy}"
                    VERBATIM)
        else()
            add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${target}>"
                            "$<TARGET_FILE_DIR:${target}>/${legacy}${CMAKE_EXECUTABLE_SUFFIX}"
                    VERBATIM)
        endif()
    endfunction()
    altera_binary(altera_client asr_client)
    altera_binary(altera_editor asr_editor)

    # Windows has no rpath: the vendored shader compiler's DLLs must sit next to
    # the exe. dxil.dll is not linked, DXC loads it at runtime to sign DXIL.
    if(WIN32)
        add_custom_command(TARGET altera_client POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        $<TARGET_RUNTIME_DLLS:altera_client> "$<TARGET_FILE_DIR:altera_client>"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "$<TARGET_FILE_DIR:dxcompiler>/dxil.dll" "$<TARGET_FILE_DIR:altera_client>"
                COMMAND_EXPAND_LISTS
                VERBATIM)
    endif()

    option(ASR_PREPARE_SCENE_MODELS "Prepare instanced nature models and impostors offline" ON)
    if(ASR_PREPARE_SCENE_MODELS)
        set(ASR_SCENE_PYTHON "" CACHE FILEPATH "Python with tools/scene_models_requirements.txt installed")
        if(NOT ASR_SCENE_PYTHON)
            if(EXISTS "${CMAKE_SOURCE_DIR}/.cache/terrain-reference-tools/bin/python")
                set(ASR_SCENE_PYTHON "${CMAKE_SOURCE_DIR}/.cache/terrain-reference-tools/bin/python")
            else()
                find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
                set(ASR_SCENE_PYTHON "${Python3_EXECUTABLE}")
            endif()
        endif()
        set(SCENE_MODEL_ARCHIVE "${CMAKE_SOURCE_DIR}/assets/models/Stylized Nature MegaKit[Standard].zip")
        set(SCENE_MODEL_OUTPUT "${CMAKE_SOURCE_DIR}/assets/generated/scene_models")
        file(GLOB SCENE_UE_MARKER CONFIGURE_DEPENDS "${SCENE_MODEL_OUTPUT}/.ue-imported")
        if(SCENE_UE_MARKER)
            if(NOT EXISTS "${SCENE_MODEL_OUTPUT}/manifest.json")
                message(FATAL_ERROR "Activated UE resources have no manifest; rerun install_ue_assets.py")
            endif()
            add_custom_target(asr_scene_models DEPENDS "${SCENE_MODEL_OUTPUT}/manifest.json")
        else()
            add_custom_command(OUTPUT "${SCENE_MODEL_OUTPUT}/manifest.json"
                    COMMAND "${ASR_SCENE_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/prepare_scene_models.py"
                            --archive "${SCENE_MODEL_ARCHIVE}" --output "${SCENE_MODEL_OUTPUT}"
                    DEPENDS "${SCENE_MODEL_ARCHIVE}" "${CMAKE_SOURCE_DIR}/tools/prepare_scene_models.py"
                            "${CMAKE_SOURCE_DIR}/tools/mesh_lod.py"
                            "${CMAKE_SOURCE_DIR}/tools/scene_models_requirements.txt"
                    COMMENT "Preparing shared nature meshes and eight-view impostors (numpy/Pillow required)"
                    VERBATIM)
            add_custom_target(asr_scene_models DEPENDS "${SCENE_MODEL_OUTPUT}/manifest.json")
        endif()
        # The renderer consumes the cluster sidecars as content, not as an
        # optional developer experiment. Keep the offline DAG build behind the
        # mesh preparation step and use a stamp because the tool writes one
        # sidecar per model plus the two spatial grove shells.
        set(SCENE_CLUSTER_STAMP "${SCENE_MODEL_OUTPUT}/.cluster_sidecars.stamp")
        add_custom_command(OUTPUT "${SCENE_CLUSTER_STAMP}"
                COMMAND "$<TARGET_FILE:scene_model_clusters>" "${SCENE_MODEL_OUTPUT}"
                COMMAND "${CMAKE_COMMAND}" -E touch "${SCENE_CLUSTER_STAMP}"
                DEPENDS asr_scene_models scene_model_clusters "${SCENE_MODEL_OUTPUT}/manifest.json"
                COMMENT "Building per-model geometry DAGs and instance canopy sidecars"
                VERBATIM)
        add_custom_target(asr_scene_geometry DEPENDS "${SCENE_CLUSTER_STAMP}")
        add_dependencies(altera_client asr_scene_geometry)
    endif()
endif()
endif() # NOT ALTERA_SIM_ONLY

# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------
if(ASR_BUILD_TESTS)
    enable_testing()
    add_executable(altera_sim_tests tests/main.cpp tests/test_altera_sim.cpp tests/test_altera_sim_control.cpp
            tests/test_altera_sim_loc.cpp tests/test_altera_sim_loc_gen.cpp tests/test_altera_sim_content.cpp
            tests/test_altera_sim_farming.cpp tests/test_altera_sim_crafts.cpp
            tests/test_altera_sim_combat.cpp tests/test_altera_sim_captivity.cpp
            tests/test_altera_sim_craft.cpp tests/test_altera_sim_cuisine.cpp tests/test_altera_sim_worth.cpp tests/test_altera_sim_resources.cpp
            tests/test_altera_sim_build.cpp tests/test_altera_sim_backstory.cpp
            tests/test_altera_sim_collide.cpp tests/test_altera_sim_actinfo.cpp tests/test_altera_sim_player_acts.cpp
            tests/test_altera_sim_gear.cpp tests/test_altera_sim_village.cpp tests/test_altera_sim_structures.cpp tests/test_altera_sim_rooms.cpp
            tests/test_altera_sim_consequences.cpp tests/test_altera_sim_dialogue_gen.cpp
            tests/test_altera_sim_dialogue_rules.cpp
            tests/test_altera_sim_rumor_noise.cpp tests/test_altera_sim_rumor_spread.cpp tests/test_altera_sim_dialogue_say.cpp
            tests/test_altera_sim_justice.cpp tests/test_altera_sim_facts.cpp
            tests/test_altera_sim_speech_feed.cpp
            tests/test_altera_sim_village_life.cpp tests/test_altera_sim_memory_budget.cpp
            tests/test_altera_sim_realm.cpp
            tests/test_altera_sim_ecology.cpp
            tests/test_altera_sim_social.cpp
            tests/test_altera_sim_dialogue_memory.cpp tests/test_altera_sim_dialogue_lines.cpp
            tests/test_altera_sim_trade.cpp
            tests/test_altera_cutaway.cpp tests/test_altera_ui_theme.cpp tests/test_altera_locomotion.cpp tests/test_altera_hotbar.cpp
            tests/test_altera_sim_dialogue_corpus.cpp tests/test_altera_client2d.cpp
            tests/test_altera_client2d_talk.cpp)
    target_sources(altera_sim_tests PRIVATE tests/test_altera_client2d_sheet.cpp)   # AL-100
    target_include_directories(altera_sim_tests PRIVATE src tests)
    target_link_libraries(altera_sim_tests PRIVATE altera_sim altera_client2d asr_warnings)
    target_compile_definitions(altera_sim_tests PRIVATE ALTERA_SIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    # The suite is split by test name: the long multi-year worlds (cmake/test_labels.cmake)
    # run as altera_sim_slow, label "full"; the rest is altera_sim, label "fast".
    include(cmake/test_labels.cmake)
    altera_add_sim_tests(altera_sim_tests)
    add_executable(altera_combat_tests tests/main.cpp tests/test_altera_combat.cpp)
    target_include_directories(altera_combat_tests PRIVATE src tests)
    target_link_libraries(altera_combat_tests PRIVATE altera_combat asr_warnings)
    target_compile_definitions(altera_combat_tests PRIVATE ALTERA_SIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME altera_combat COMMAND altera_combat_tests)
    # C++ core vs the Python prototype's tables (n=3000, seed 1), +-5 pp per cell
    add_test(NAME altera_combat_golden COMMAND altera_combat_tables --n 3000 --seed 1
            --golden ${CMAKE_SOURCE_DIR}/tests/data/combat/golden_combat_sim_n3000_seed1.md --tol 5)
    if(ALTERA_SIM_ONLY)
        altera_label_tests()
        return()
    endif()
    add_executable(asr_dlss_math_tests tests/main.cpp tests/test_dlss_math.cpp)
    target_include_directories(asr_dlss_math_tests PRIVATE src tests)
    target_link_libraries(asr_dlss_math_tests PRIVATE asr_warnings)
    add_test(NAME dlss_math COMMAND asr_dlss_math_tests)
    if(TARGET asr_dlss_gpu_check AND ASR_TEST_GPU)
        add_test(NAME dlss_gpu COMMAND asr_dlss_gpu_check)
        set_tests_properties(dlss_gpu PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            SKIP_RETURN_CODE 77 TIMEOUT 120)
    endif()
    if(TARGET asr_engine_gui)
        # The interface layer on a software renderer: the face, typing, sliders.
        add_executable(asr_ui_tests tests/main.cpp tests/test_ui.cpp)
        target_link_libraries(asr_ui_tests PRIVATE asr_engine_gui asr_warnings)
        add_test(NAME ui COMMAND asr_ui_tests)
        set_tests_properties(ui PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    endif()
    if(TARGET asr_engine_viewport)
        add_executable(asr_engine_viewport_tests tests/main.cpp tests/test_dag_viewport_gpu.cpp)
        target_link_libraries(asr_engine_viewport_tests PRIVATE asr_engine_viewport asr_engine_gui asr_warnings)
        if(ASR_TEST_GPU)
            add_test(NAME engine_viewport_gpu COMMAND asr_engine_viewport_tests)
            set_tests_properties(engine_viewport_gpu PROPERTIES LABELS "gpu" TIMEOUT 60
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        endif()
    endif()
    add_executable(asr_engine_module_tests tests/main.cpp tests/test_engine_modules.cpp
            tests/test_recursive_impostor.cpp tests/test_impostor_hierarchy.cpp
            tests/test_camera.cpp tests/test_camera_matrix.cpp
            tests/test_plate_field.cpp tests/test_range_field.cpp tests/test_erosion_field.cpp
            tests/test_world_store.cpp tests/test_world_source.cpp tests/test_biomes.cpp)
    target_link_libraries(asr_engine_module_tests PRIVATE asr_representation asr_terrain_generator
            asr_virtual_geometry asr_impostors asr_world_store asr_world_source asr_biomes asr_warnings)
    add_test(NAME engine_modules COMMAND asr_engine_module_tests)
    add_executable(asr_animation_tests tests/main.cpp tests/test_animation.cpp)
    target_link_libraries(asr_animation_tests PRIVATE asr_animation asr_warnings)
    add_test(NAME animation COMMAND asr_animation_tests)
    set_tests_properties(animation PROPERTIES TIMEOUT 20)
    set_tests_properties(engine_modules PROPERTIES TIMEOUT 45)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME external_profiler COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/tests/test_external_profiler.py")
        set_tests_properties(external_profiler PROPERTIES TIMEOUT 30)
        execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import numpy; import PIL"
                RESULT_VARIABLE ASR_DEPTH_TEST_DEPS OUTPUT_QUIET ERROR_QUIET)
        if(ASR_DEPTH_TEST_DEPS EQUAL 0)
            add_test(NAME impostor_depth_baker COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/tests/test_impostor_depth.py")
            set_tests_properties(impostor_depth_baker PROPERTIES TIMEOUT 30)
        endif()
    endif()
    if(ASR_BUILD_CLIENT AND ASR_PREPARE_SCENE_MODELS)
        add_test(NAME ue_asset_import COMMAND "${ASR_SCENE_PYTHON}"
                "${CMAKE_SOURCE_DIR}/tests/test_prepare_ue_assets.py")
        set_tests_properties(ue_asset_import PROPERTIES TIMEOUT 60)
    endif()
    add_executable(asr_smart_mesh_tests tests/main.cpp tests/test_smart_mesh.cpp)
    target_link_libraries(asr_smart_mesh_tests PRIVATE asr_geometry asr_warnings)
    if(ASR_BUILD_CLIENT)
        target_compile_definitions(asr_smart_mesh_tests PRIVATE ASR_SMART_RENDER_TESTS=1)
        target_link_libraries(asr_smart_mesh_tests PRIVATE asr_engine_render)
    endif()
    target_sources(asr_smart_mesh_tests PRIVATE tests/test_cluster_dag.cpp
            tests/test_instance_hierarchy.cpp tests/test_page_residency.cpp)
    target_sources(asr_smart_mesh_tests PRIVATE
            src/engine/render/systems/instance_hierarchy_select.cpp)
    target_sources(asr_smart_mesh_tests PRIVATE tests/test_quad_mass.cpp)
    target_sources(asr_smart_mesh_tests PRIVATE tests/test_region_mass.cpp)
    add_test(NAME smart_mesh_components COMMAND asr_smart_mesh_tests)
    set_tests_properties(smart_mesh_components PROPERTIES TIMEOUT 30)
    add_executable(asr_world_system_tests tests/main.cpp tests/test_world_system.cpp
            tests/test_scene_entities.cpp tests/test_terrain_brush.cpp tests/test_world_delta.cpp
            tests/test_ground_edits.cpp tests/test_world_tools.cpp)
    target_link_libraries(asr_world_system_tests PRIVATE asr_world asr_warnings)
    add_test(NAME world_system COMMAND asr_world_system_tests)
    set_tests_properties(world_system PROPERTIES TIMEOUT 30)
    add_executable(asr_scene_view_tests tests/main.cpp tests/test_scene_view.cpp
            tests/test_camera_matrix.cpp tests/test_scene_scatter.cpp tests/test_ecology_shadow.cpp)
    target_link_libraries(asr_scene_view_tests PRIVATE asr_world asr_camera asr_warnings)
    add_test(NAME scene_view COMMAND asr_scene_view_tests)
    set_tests_properties(scene_view PROPERTIES TIMEOUT 30)
    # The inspector lives in the client, so its tests only exist when the client
    # is built. The simulation tests never depend on it.
    if(ASR_BUILD_CLIENT)
        add_library(asr_inspector STATIC src/game/client/selection.cpp src/game/ui/info_panel.cpp)
        target_include_directories(asr_inspector PUBLIC src)
        target_link_libraries(asr_inspector PUBLIC asr_simulation SDL3::SDL3-static PRIVATE asr_warnings)
        set(ASR_INSPECTOR_TESTS tests/test_inspector.cpp)
        set(ASR_INSPECTOR_LIBS asr_inspector)
        # Streaming integration without a window/GPU. Use the same Explorer
        # and worker implementation as the client, not a test-only scheduler.
        add_executable(asr_ring_tests
                tests/main.cpp
                tests/test_instance_arena.cpp
                tests/test_ring_stream.cpp
                tests/test_environment_menu.cpp
                tests/test_draw_distance.cpp
                tests/test_environment_workers.cpp
                tests/test_weather.cpp
                tests/test_water_bodies.cpp
                src/game/client/explore_menu.cpp src/game/client/graphics_panel.cpp
            src/game/client/world_editor.cpp src/game/client/world_editor_biomes.cpp
                src/game/client/world_dials.cpp
                src/game/client/explorer.cpp
                src/game/client/renderer.cpp
                src/game/client/controls.cpp
                src/game/client/selection.cpp)
        target_link_libraries(asr_ring_tests PRIVATE asr_simulation asr_world asr_game_render asr_engine_gui
                nlohmann_json SDL3::SDL3-static SDL3_image::SDL3_image-static asr_warnings)
        add_test(NAME ring_stream COMMAND asr_ring_tests)

        # Explicit GPU readback check, not a dependency of headless CI runs.
        add_executable(asr_height_page_gpu_tests
                tests/main.cpp
                tests/test_cluster_cull_gpu.cpp
                tests/test_indirect_draw_gpu.cpp
                tests/test_height_page_atlas_gpu.cpp
                tests/test_slope_shadow_gpu.cpp tests/test_impostor_cache_gpu.cpp)
        if(ASR_ENABLE_DIAGNOSTICS)
            target_sources(asr_height_page_gpu_tests PRIVATE tests/test_grass_cull_gpu.cpp)
        endif()
        target_link_libraries(asr_height_page_gpu_tests PRIVATE asr_game_render asr_warnings)
        if(ASR_TEST_GPU)
            add_test(NAME height_page_gpu COMMAND asr_height_page_gpu_tests)
            set_tests_properties(height_page_gpu PROPERTIES LABELS "gpu" TIMEOUT 60)
        endif()
    endif()
    add_executable(asr_tests
            tests/main.cpp
            tests/test_camera.cpp
            tests/test_camera_matrix.cpp
            tests/test_fixed.cpp
            tests/test_determinism.cpp
            tests/test_content.cpp
            tests/test_pathfinding.cpp
            tests/test_worldgen.cpp
            tests/test_hybrid_terrain.cpp
            tests/test_medicine.cpp
            tests/test_simulation.cpp
            tests/test_savegame.cpp
            tests/test_world_mesh.cpp
            tests/test_environment.cpp
            tests/test_water_queries.cpp
            tests/test_natural_basins.cpp
            tests/test_soil.cpp
            tests/test_world_spatial.cpp
            tests/test_world_nav.cpp
            tests/test_terrain_streaming.cpp
            tests/test_terrain_adaptive.cpp
            tests/test_terrain_refinement.cpp
            tests/test_terrain_plan.cpp
            tests/test_terrain_lod.cpp
            tests/test_terrain_height_pages.cpp
            tests/test_tile_mesh.cpp
            tests/test_engine_clock.cpp
            tests/test_ecs.cpp
            ${ASR_INSPECTOR_TESTS})
    target_link_libraries(asr_tests PRIVATE asr_simulation asr_world asr_camera nlohmann_json asr_warnings
            ${ASR_INSPECTOR_LIBS})
    target_sources(asr_tests PRIVATE tests/test_terrain_config.cpp)
    target_sources(asr_tests PRIVATE tests/test_world_layout.cpp tests/test_world_layers.cpp tests/test_world_compose.cpp
            tests/test_world_stages.cpp tests/test_world_import.cpp tests/test_world_procedural.cpp)
    target_link_libraries(asr_tests PRIVATE asr_world_source)
    target_sources(asr_tests PRIVATE tests/test_scene_scatter.cpp)
    target_sources(asr_tests PRIVATE tests/test_world_scale.cpp tests/test_land_coverage.cpp
            tests/test_terrain_source.cpp)
    add_test(NAME unit COMMAND asr_tests)

    # The procedural environment's mechanism alone: engine code, a synthetic
    # world, no game - small enough to build and run on its own.
    add_executable(asr_environment_tests tests/main.cpp tests/test_procedural_environment.cpp)
    target_include_directories(asr_environment_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    target_link_libraries(asr_environment_tests PRIVATE asr_environment asr_warnings)
    add_test(NAME environment COMMAND asr_environment_tests)

    add_executable(asr_terrain_tests
            tests/main.cpp
            tests/test_hybrid_terrain.cpp
            tests/test_terrain_adaptive.cpp
            tests/test_terrain_refinement.cpp
            tests/test_terrain_plan.cpp
            tests/test_terrain_lod.cpp
            tests/test_terrain_height_pages.cpp
            tests/test_terrain_cut.cpp
            tests/test_water_selection.cpp
            tests/test_tile_mesh.cpp)
    target_link_libraries(asr_terrain_tests PRIVATE asr_world nlohmann_json asr_warnings)
    target_sources(asr_terrain_tests PRIVATE tests/test_terrain_config.cpp)
    target_sources(asr_terrain_tests PRIVATE tests/test_scene_scatter.cpp)
    target_sources(asr_terrain_tests PRIVATE tests/test_terrain_source_tree.cpp)
    target_sources(asr_terrain_tests PRIVATE tests/test_plate_field.cpp)
    target_sources(asr_terrain_tests PRIVATE tests/test_range_field.cpp)
    target_sources(asr_terrain_tests PRIVATE tests/test_erosion_field.cpp)
    target_sources(asr_terrain_tests PRIVATE tests/test_terrain_foundation.cpp)
    target_sources(asr_terrain_tests PRIVATE tests/test_natural_basins.cpp)
    target_sources(asr_terrain_tests PRIVATE tests/test_world_scale.cpp tests/test_land_coverage.cpp
            tests/test_terrain_source.cpp)
    if(TARGET altera_terrain_core)
        add_executable(altera_terrain_core_tests tests/main.cpp tests/test_terrain_core.cpp tests/test_world_gen.cpp
                tests/test_terrain_core_enb.cpp)
        target_link_libraries(altera_terrain_core_tests PRIVATE altera_terrain_core asr_warnings)
        add_test(NAME terrain_core_unit COMMAND altera_terrain_core_tests)
    endif()
    add_test(NAME terrain_unit COMMAND asr_terrain_tests)
    set_tests_properties(terrain_unit PROPERTIES TIMEOUT 120)

    # Focused ECS suite. It deliberately excludes optional environment/render
    # tests so simulation component and scheduler changes can be validated while
    # those independent targets are being developed in parallel.
    add_executable(asr_ecs_tests
            tests/main.cpp
            tests/test_ecs.cpp)
    target_link_libraries(asr_ecs_tests PRIVATE asr_simulation asr_world nlohmann_json asr_warnings)
    add_test(NAME ecs_unit COMMAND asr_ecs_tests)

    # CPU render systems, on their own so they stay fast and windowless.
    add_executable(asr_render_system_tests
            tests/main.cpp
            tests/test_instance_gather.cpp
            tests/test_level_select.cpp
            tests/test_frustum_cull.cpp
            tests/test_horizon_cull.cpp
            tests/test_terrain_gather.cpp
            tests/test_cluster_draws.cpp
            tests/test_source_cluster_draws.cpp)
    target_link_libraries(asr_render_system_tests PRIVATE asr_render_systems asr_warnings)
    add_test(NAME render_systems COMMAND asr_render_system_tests)
endif()

# Every test gets a ctest label: "fast" (the sim's quick checks, the default
# layer) or "full" (everything else). See cmake/test_labels.cmake.
if(ASR_BUILD_TESTS)
    altera_label_tests()
endif()
