# terrain_core — рендер и генерация мира (ядро лабы)
Назначение: клипмап + CDLOD рельеф, биомы, вода/реки, фолиаж/трава, пропсы (мегалиты, клифы, мосты), небо, облака, тени, FarDetail, Post (стили 0-3), DLSS, CPU-высота как на GPU.
Где: `src/terrain_core/` (библиотека `altera_terrain_core`), лаба `tools/terrain_lab/` (`terrain_lab.exe`, структура `Lab` в main.cpp ~1400 строк), шейдеры `*.hlsl`.
Запуск/проверка: закрыть terrain_lab.exe → `cmake --build cmake-build-windows` → кадры в `terrain_lab_out\shots_for_user\`. Только Windows/D3D12, в облаке не проверяется.
Подводные камни:
- Порядок кадра: клипмап → фолиаж/трава/пропсы → тени (3 каскада) → sky/ground/props/foliage/grass/water → облака → DLSS → Post.
- Правка одного .hlsl пересобирает PSO долго; не трогать шейдеры без нужды.
- Историю каньона, маршей и мегалитов (с 15.10) см. `process/archive/terrain_history.md` в файлах проекта.
- Не читать main.cpp целиком: искать по `doc/INDEX.md`.
Спеки: `engine_merge/MERGE_PLAN.md`, `BIOME_PLAN.md`, `MESH_TERRAIN_PLAN.md`, `marshes/MARSHES_REQUIREMENTS.md`.
