# Altera - working rules for Claude

Altera: a life-sim (src/sim) on the Campfire engine (src/engine) with a terrain
renderer (src/terrain_core) and the game client/lab (tools/terrain_lab, src/game_view).
Several threads work in one working copy and in worktrees (D:/AlteraGame/altera-wt/*).

## Check layers (cheapest first; stop at the first one that answers the question)

1. **Compile only what you touched**: `tools\check_fast.bat build` (sim-only tree).
2. **Fast layer, default**: `tools\check_fast.bat` - builds cmake-build-sim and runs the
   ctest label `fast` (the sim's quick tests, combat, combat golden). About a minute.
3. **One test by name**: `tools\check_fast.bat -R altera_sim` or the test exe with a
   name substring: `cmake-build-sim\altera_sim_tests.exe farm_ripen`.
   `--exclude=<substring>` skips tests. `tools\check_fast.bat rerun` reruns only the failed ones.
4. **Full layer, only on request**: `tools\check_fast.bat full` (sim incl. multi-year worlds,
   label `full`), and `ctest --test-dir cmake-build-windows` for the whole engine.
5. **Frames/screenshots, only on request**: terrain_lab / altera_game in cmake-build-windows,
   shots go to `terrain_lab_out\shots_for_user\`. Don't open/analyse them unless asked.

check_fast prints only the outcome and the first 20 errors; full logs are in
`cmake-build-sim\check_build.log` and `check_test.log`. Read the log by grep, not whole.

## Build trees

| Dir | What | How |
|-----|------|-----|
| `cmake-build-sim` | sim only (`ALTERA_SIM_ONLY=ON`): altera_sim, altera_combat, altera_sim_runner, altera_combat_tables, altera_sim_tests, altera_combat_tests, altera_sim_view. No fetches, engine, D3D12, shaders, assets. One per worktree. | `cmake --preset sim`, `cmake --build --preset sim`, `ctest --preset sim-fast` |
| `cmake-build-windows` | full client/lab/editor (CLion, preset `windows` in CMakeUserPresets.json). Shared, heavy: one build at a time. | `cmake --build cmake-build-windows --target <target>` |
| Linux/cloud | the same sim-only build | `cmake --preset sim-linux && cmake --build --preset sim-linux && ctest --preset sim-linux-fast` |

- Build only the targets you need (`--target`), never the whole full tree without a reason.
- **Close terrain_lab.exe / altera_game.exe before building** cmake-build-windows, or the
  link fails with permission denied on the exe and DLLs.
- Outside CLion the shell needs the MSVC env for clang-cl (check_fast.bat calls VsDevCmd
  itself); cmake/ninja/ctest come from `C:\Program Files\JetBrains\CLion*\bin\`.
- A new sim .cpp goes into `altera_sim` in CMakeLists.txt; a new sim test file into
  `altera_sim_tests`. A slow sim test (> ~5 s) goes into `ALTERA_SIM_SLOW_TESTS` in
  `cmake/test_labels.cmake` so it moves to the full layer.
- src/sim includes only the standard library and nlohmann/json: keep it that way
  (no windows.h, no engine headers) so it builds on Linux.

## Directory ownership

| Area | Owner / thread | Notes |
|------|----------------|-------|
| `src/sim`, `content/ai`, `content/*` sim data, `tests/test_altera_*` | sim / AI / crafting threads | headless, deterministic |
| `src/sim/combat`, `content/combat` | combat | golden: tests/data/combat |
| `src/terrain_core`, `assets/shaders/terrain_core`, `content/config/terrain_core` | terrain / biomes threads | checked in Terrain lab |
| `tools/terrain_lab`, `src/game_view` | game client / lab | heavy build |
| `src/engine`, `src/game` | engine (Campfire) | rarely touched |
| `CMakeLists.txt`, `CMakePresets.json`, `cmake/`, `tools/check_fast.bat`, `CLAUDE.md` | build setup | keep edits small, own hunks only |
| `doc/`, `assets/source` | user | don't edit without a request |

## Rules

- **Commit locally, don't push** unless the user explicitly asks in this thread.
  Never force-push, `reset --hard`, `clean -fd`, `rm -rf`, `del /s`.
- **Don't touch others' uncommitted edits**: `git status` shows many modified files from
  parallel threads. Stage only your own files/hunks (`git add <file>`, or `git apply --cached`
  of your own patch when a file also carries someone else's edit). Never revert, stash or
  reformat what isn't yours.
- **Don't read files over ~500 lines whole**: find the spot (`grep -n`, graphify) and read
  that range. Bound shell output (`| head`, `| tail`); long logs go to a file.
- Full test suite, full-tree builds and frames/screenshots only when the user asks.
- Answer in Russian; times in UTC+2 or none.
- Buildings (towers, castles) are postponed; natural structures only for now.

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

- For codebase questions, first run `graphify query "<question>"` when graphify-out/graph.json exists. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
