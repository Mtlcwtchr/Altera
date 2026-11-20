# combat — авто-бой по зонам и направлениям
Назначение: бой как в Kingdom Come (6 зон × 5 направлений), раны как Kenshi, клинч, дальний бой, износ; всё data-driven, игрок при поражении не умирает.
Где: данные `gdd/combat/data/` (combat_config, weapons, armor, shields, wounds, ranged_weapons, clinch_moves, animations, behaviors, clip_map, procedural, wear); в игре C++ ядро по `CORE_PORT_BRIEF.md` **[сверить]**.
Запуск: прототип `python gdd/combat/tools/combat_sim.py --n 400 --seed 1` (отчёт report/COMBAT_SIM_REPORT.md); `combat_trace.py` — события боя → сценарий анимаций.
Подводные камни:
- Числа только в JSON; код читает. Латы не пробиваются — только зазоры, дробящее передаёт силу.
- Нет защиты в PvP как отдельного режима; смерть только через казнь и подобное.
- Анимации: клипы ретаргетятся на риг Quaternius/UAL; кейфреймы чужих модов не брать.
Спеки: `gdd/combat/COMBAT_DESIGN.md`, `COMBAT_CORE_SPEC_v0.1.md`, `COMBAT_PRESENTATION.md`, `CLIP_CATALOG.md`.
