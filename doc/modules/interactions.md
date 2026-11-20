# interactions — каталог взаимодействий и диалоги
Назначение: единый каталог действий (работа, соц., обмен, бой) и диалоги «речевой акт + тема»; реплики из шаблонов (BuiltinProvider) или от внешнего агента (только диалоги).
Где: данные `content/ai/interactions.json` (+ `part_c`, `part_x` из расширения), диалоги `dialogue.json`, loc `content/loc/<lang>/` (dialogue.*, interaction.*). Генераторы: `gdd/interactions/gen_*.py`, `build_dialogue_v02.py`.
Размер (на 20.11): ~204 взаимодействия с расширением, ключи диалогов dialogue.<шаблон>.<вариант>.
Запуск/проверка: `python tools/validate_content.py ai expansion loc`.
Подводные камни:
- Каждое взаимодействие нужно хоть кому-то из HTN/планов, иначе WARN «никто не ссылается».
- Падежи имён: существительные в loc как `{nom, gen, dat, acc, ins, pre, g}`; склонение имён по `name_declension_ru_proposal.json` (предложение).
- Диалог всегда начинает обычный ИИ; агент только правит веса/задачи.
Спеки: `gdd/interactions/INTERACTIONS_DIALOGUE_v0.1.md`, `LOCALIZATION_DIALOGUE_v0.1.md`, `gdd/LOC_FORMAT.md`.
