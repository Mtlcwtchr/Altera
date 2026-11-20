# crafting — крафт без рецептов (K1…K7)
Назначение: предметы собираются из материалов, процессов и форм по формулам свойств; книга рецептов — результат открытий, а не список.
Где: данные `content/ai/craft/` (черновик в `gdd/crafting/data/`): materials(50), processes(19), forms(47), functions, reacts(9), flaws(10), stations(20), crafting_config, loc_ru/craft.json. Генератор `tools/gen_k1.py`.
Запуск/проверка: `python tools/validate_content.py crafting` (ссылки процессов и изъянов, loc, достижимость форм через shape_map).
Состояние: сделан K1 (данные). Не сделано: интерпретатор craft-expr (K2), interactions крафта (K3), боевые экземпляры (K4), UI (K5), алхимия (K6), записи книги (K7).
Подводные камни:
- craft-expr пока описательный: строки в данных не исполняются, грамматику фиксирует K2.
- Станции станут объектами в objects.json: id не менять без правки ссылок.
- Новые материалы: ключ в `loc_ru/craft.json` (существительное с падежами), иначе ERR.
Спеки: `gdd/crafting/CRAFTING_DESIGN.md`, `gdd/crafting/BRIEF_PC_K1_K2.md`, `gdd/crafting/data/README_K1.md`; UI-доски `ui-crafting-boards` (в памяти проекта).
