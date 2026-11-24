# altera_game_2d — 2D-клиент с видом сверху (AL-94, этап 1 AL-95)

Дизайн: `gdd/ui/client_2d/CLIENT_2D.md` (карта, спрайты, пешки) и `ui_h3/UI_H3.md` (рамка «Резной рубеж»).

- `src/client2d/` — чтение сима: мир → клетки 1 м (земля по биому, вода, дороги, площадь, поля; полы, стены/двери/окна на линиях клеток, комнаты, крыши), люди и карточка. Только чтение, приказы идут командами.
- `tools/game_2d/main.cpp` — процесс: сим шагает между HTTP-запросами (как altera_sim_view), Windows и Linux.
- `tools/game_2d/web/` — страница (canvas): тайлы 16×16, пешки 20×20 слоями, выбор, крыши, символьный режим. Читается с диска на каждый запрос: правится без пересборки.
- `content/sprites_2d/sprites.json` — спрайты сетками букв палитры (генератор `tools/content_gen/gen_sprites_2d.py`), `map2d.json` — что чем рисовать (земля, объекты, одежда, инструменты, значки дел), `resources.json` — пак ресурсов (AL-86).
- Строки страницы: `content/loc/<lang>/ui2d.json`.

Запуск: `altera_game_2d [--seed N] [--lang ru|en] [--port 8766] [--run-hours 7] [--paused]`, страница откроется сама (Windows).
Кадры: `node tools/game_2d/shots.js` → `terrain_lab_out/shots_for_user/`.
Тест: `altera_sim_tests client2d`.

Управление: ЛКМ выбрать, ПКМ/СКМ или WASD двигать, колесо или 1/2/3 зум, R крыши (авто/все/сняты), G символы, F следить, Пробел пауза, −/= скорость, Alt имена.

**Разговор и торг (AL-98):** `web/talk.js` + `web/talk.css`, сервер `src/client2d/talk2d.*`, маршруты `/api/talk?target=&tone=`, `POST /api/trade {target, give, take}`, команда `say_topic`. Открываются кнопками «Поговорить» / «Торговать» в ПКМ-меню на человеке или клавишей T по выбранному. Кадры: `node tools/game_2d/shots_talk.js`. Строки `content/loc/<lang>/ui2d_talk.json`.
