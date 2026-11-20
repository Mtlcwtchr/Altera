# loc — локализация
Назначение: в данных и коде только ключи; тексты в `content/loc/<lang>/*.json`; ru = base, en = перевод, fallback ru.
Где: `src/sim/loc.hpp` (`Loc`, `formatLoc`, `validateLoc`, `traceText`, `localizeReport`); данные `content/loc/{ru,en}/` (names, social, dialogue, trace, expansion, motivation, history).
Формат: вложенные объекты → ключи через точку; существительное `{nom,gen,dat,acc,ins,pre,g}`; плейсхолдеры `{x}`, `{x:dat}`, `{x:g|рад|рада}`, `{n:plural|ягода|ягоды|ягод}`, литералы `{{ }}`.
Проверка: `python tools/validate_content.py loc`: ERR — ключ есть в en, но не в ru; разные плейсхолдеры ru/en (род `:g|` не считается); непарные скобки. WARN — нет перевода en. У history своя грамматика (`_grammar.json`): расхождения только WARN.
Подводные камни:
- Плейсхолдер выводит только слово; число пишется отдельно.
- Ключ в двух файлах одного языка: побеждает первый (WARN).
- Не копировать фразы из чужих игр; тексты писать самим.
Спеки: `gdd/LOC_FORMAT.md` (копия `doc/design/LOC_FORMAT.md`), `gdd/interactions/LOCALIZATION_DIALOGUE_v0.1.md`.
