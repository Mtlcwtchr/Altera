# content/species — разумные виды (AL-114)

Дизайн: `gdd/setting/LIVELYREALMS_SETTING_v0.2.md` §3–4. Код: `src/sim/species.hpp`.

- `<id>.json` — вид: `body` (рост, масса, `sprite_px`, `skin`, ночное зрение), `life` (`adult_years`, `elder_years`, `lifespan_years`, `fertility`, `interbreed`: вид партнёра → вид ребёнка), `needs` (множители нужд), `aptitudes` (навык → `learn`/`cap`, `*` = все прочие), `affinity` (аспекты магии, читает AL-116), `traits_pool`, `resist`/`weak`, `relations_bias`, `culture_default`, `settlement_tag`, `gifted_chance`, `might` (сила в realm), `innate_spells`, `hoard`, `regen`, `sun_sleep`.
- `blend: [a, b]` — вид-смесь (полуэльф): числа усредняются, свои поля перекрывают.
- `cultures/<id>.json` — имена (ключи `name.<id>` в loc), стиль речи и построек, каким аспектам учат и что запрещено, нормы.
- `settle.json` — `default_race`, состав деревни карты (`map`) и шаблонов realm (`realm_templates`).

Возраст: правила, считающие годы (старость, плодовитость, брак), читают `Body::lifeYears()` — годы в человеческой мере (14 = взрослый, 60 = старый), поэтому новый вид не требует правок кода. Нежить — черта `undead` (AL-116): для неё нужды и старение вида не действуют. Тролли: `sun_sleep` → состояние `petrified_sleep` на свету без крыши.
