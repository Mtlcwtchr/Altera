# sim — ядро симуляции (headless)
Назначение: Character (один тип для игрока и NPC), время, события, сериализация, seed, журнал; без движка и D3D12. Детерминизм от seed + команд.
Где: `src/sim/` (библиотека `altera_sim`), раннер `sim_runner/` (`altera_sim_runner`), тесты `altera_sim_tests`, просмотр `altera_sim_view`. **[сверить]** точный состав файлов — `doc/INDEX.md`.
Состав кора: world, agent, control (AiController/PlayerController), interaction, dialogue, society, economy, knowledge, history, agentapi, tools.
Запуск: собрать цели `altera_sim_tests`, `altera_sim_runner`; `altera_sim_runner --days 3`. Быстрый путь без клиента; общий слот cmake-build-windows нужен только для клиента/лабы.
Linux (облако, проверено 20.11): `cmake --preset sim-linux && cmake --build --preset sim-linux -j && ctest --preset sim-linux-fast` — сборка ~1,5 мин, 3 теста fast ~14 с. Нужен CMake ≥3.30 (`$<CXX_COMPILER_FRONTEND_VARIANT>`); на 3.28 ставить `pip install cmake`.
Подводные камни:
- Любое решение/случайность только через сид-генератор мира; без `rand()`/времени системы.
- Код и данные хранят только ключи локализации, тексты не пишем (см. loc).
- Игрок = обычный Character с PlayerController; не плодить особых веток «если игрок».
- Не тянуть Windows-заголовки в `src/sim`: библиотека должна собираться на Linux.
Данные: `content/ai/*.json` (схема AI_DATA_SCHEMA), проверка `validate_content.py ai`.
Спеки: `gdd/ALTERA_CORE_SPEC_v0.1.md`, `gdd/ALTERA_GDD_v1.0_draft.md`, `gdd/WORK_PLAN.md`, `gdd/AI_DATA_SCHEMA.md`.
