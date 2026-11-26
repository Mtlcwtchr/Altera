# OBSOLETE: the 3D game client

`src/game_view` (the library `lr_game_view`, the executable `lr_game`) is obsolete
since 2026-11-26 and is **not built**. The game is played in the 2D client
(`lr_client2d`, `lr_game_2d`) on the life-sim `lr_sim`. Don't extend this code and don't
port features into it; it stays only for reference and may be deleted later.

Still built from here, because `terrain_lab` uses them for its loading window:

- `loading_screen.{hpp,cpp}`, `saves.{hpp,cpp}` (compiled into `terrain_lab`);
- `ui_theme.hpp` (the loading window's theme; tested by `tests/test_lr_ui_theme.cpp`).

Not built anymore: everything else here, and the tests `tests/test_lr_cutaway.cpp`,
`tests/test_lr_hotbar.cpp`, `tests/test_lr_locomotion.cpp`. `terrain_lab` lost its
`--game`, `--sim-seed`, `--season`, `--lore` and `--begin` options.
