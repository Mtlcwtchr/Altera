# Altera — схема данных ИИ (`content/ai/`), v0.2

Дополняет `ALTERA_AI_ARCHITECTURE_v0.1.md` и `ALTERA_CORE_SPEC_v0.1.md`. Это контракт между контентом и кодом
кора (`src/sim/`): код — интерпретатор, всё поведение задаётся этими файлами. Формат — JSON (UTF-8, без комментариев;
поле `"_doc"` в любом объекте разрешено и игнорируется). Идентификаторы — `snake_case` латиницей, уникальны в своём файле.

**Только ключи.** В данных и в коде нет отображаемого текста: имена, описания, реплики диалогов, тексты трассы и отчётов
лежат в `content/loc/<lang>/*.json` и собираются при выводе (формат — `LOC_FORMAT.md`). Каждый id контента обязан иметь
ключ в базовом языке (`item.<id>.name`, `interaction.<id>.name`, `trait.<id>.desc`, `dialogue.<template>.<variant>`, ...).

Загрузка: `altera::sim::Content::load(dir)`; валидатор `altera::sim::validate(content)` запускается при загрузке и в тестах
(`altera_sim_tests`), `altera_sim_runner --validate` печатает ошибки контента и локализации. Неизвестные поля —
предупреждение, не ошибка.

Изменения v0.2: префикс `t.` (цель-персонаж), параметры `$item/$skill/$place`, `chance`/`on_fail`/`risk`, `combat`,
`terms` сделок, эффекты `status_pair`, `household_*`, `commitment`, `death`, репутация, оси `rel` `enmity`/`respect`/
`attraction`, условие `moodlet:`, файл `dialogue.json`, локализация. Что из этого уже исполняется — §1.4.

## 0. Файлы

| Файл | Что | Слой |
|---|---|---|
| `items.json` | предметы (еда, материалы, инструменты) | мир |
| `objects.json` | типы объектов мира (куст, дерево, очаг, кровать, склад) | мир |
| `needs.json` | нужды: скорость падения, пороги; moodlets | вход Utility |
| `traits.json` | черты: множители целей, штрафы/допуски методов | Utility/HTN |
| `goals.json` | типы целей, соображения, кривые, гистерезис | 1 Utility |
| `htn.json` | составные задачи и методы | 2 HTN |
| `interactions.json` | **единый каталог взаимодействий** (= действия GOAP и команды игрока) | 3 GOAP / 4 FSM |
| `fsm.json` | иерархия состояний тела, прерываемость | 4 FSM |
| `dialogue.json` | речевые акты и шаблоны диалогов (тексты — `content/loc/<lang>/dialogue.json`) | соц. |
| `config.json` | глобальные параметры ИИ (top-K, гистерезис, лимиты GOAP, границы модификаторов, словари соц. слоя, обязательства) | все |

Тестовый мир и сценарии лежат отдельно: `content/sim/test_world.json`, `content/sim/scenarios/*.json` (§11).
Локализация — `content/loc/` (`LOC_FORMAT.md`).

## 1. Мини-язык условий и эффектов

Условия (`pre`, `interrupt`, `until`, предусловия методов, `pre` шаблонов и вариантов диалога) и эффекты (`eff`,
`on_fail`, `risk[].eff`, `combat.outcomes`, `terms.on_breach`, `commitments[].on_breach`) — строки. Одна строка = один факт.

**Условие:** `[!][t.]subject:key[op value]`. Без `op` — булево (истинно, если значение > 0). `!` — отрицание.
`op`: `>=`, `<=`, `>`, `<`, `==`, `!=`. `value` — число. `key` может быть параметром `$name` (§1.5).

### 1.1 Субъекты условий

| subject | key | значение | кто проверяет |
|---|---|---|---|
| `has` | предмет | количество в инвентаре актёра | GOAP-факт |
| `at` | тип объекта | актёр стоит у объекта этого типа (булево) | GOAP-факт |
| `need` | нужда | 0..1, 1 = полностью удовлетворена | Utility/HTN |
| `knows` | знание | булево (`foraging`, `cooking`, ...) | HTN/GOAP |
| `skill` | навык | 0..1 | HTN |
| `trait` | черта | булево | HTN |
| `age` | `child`/`adult`/`elder` | булево | все |
| `body` | `health`, `fatigue` | 0..1 | все |
| `time` | `night`/`day` | булево | Utility |
| `known` | тип объекта | актёр знает свободный (не зарезервированный другим) объект этого типа; со `target_stock` — с нужным запасом | HTN/GOAP |
| `home` | предмет | количество на складе семьи актёра (`home:meal>=1`) | HTN |
| `target_stock` | предмет | запас в целевом объекте взаимодействия | GOAP (при выборе экземпляра) |
| `target_owner` | `self`/`family`/`other`/`none` | владелец целевого объекта | GOAP/HTN |
| `rel` | ось отношения (§1.6) | отношение актёр→цель (персонаж) | соц. |
| `moodlet` | moodlet | булево: moodlet действует сейчас | все |
| `reputation` | ось репутации (`config.reputation_axes`) | -1..1 | соц. |

### 1.2 Эффекты

**Эффект:** `[t.]subject:key<+|-|=>value` или `[t.]subject:key` (флаговые эффекты, без оператора).

| subject | Пример | Смысл |
|---|---|---|
| `has` | `has:berries+3` | инвентарь актёра (берётся из `target_stock`, если взаимодействие с объектом и указано `"from_target": true`) |
| `need` | `need:hunger+0.45` | нужда актёра, обрезка 0..1 |
| `target_stock` | `target_stock:berries-3` | запас целевого объекта (не уходит ниже 0) |
| `home` | `home:meal+1` | склад семьи |
| `skill` | `skill:foraging+0.01` | рост навыка |
| `body` | `body:fatigue+0.05` | тело (`health`, `fatigue`) |
| `moodlet` | `moodlet:ate_meal` | добавить moodlet из `needs.json/moodlets` (флаг) |
| `memory` | `memory:ate_alone` | запись в память актёра (тип, значимость из `config.json/memory_types`; флаг) |
| `rel` / `rel_back` | `rel:affinity+0.05` | отношение актёр→цель / цель→актёр (ось §1.6; нужна цель-персонаж) |
| `spawn` | `spawn:house` | создать объект типа у позиции актёра (владелец — семья актёра; флаг) |
| `reputation` | `reputation:honesty-0.1` | репутация актёра по оси, обрезка -1..1 |
| `status_pair` | `status_pair:betrothed`, `status_pair:married=0` | парный статус актёр↔цель (`config.status_pairs`): флаг или `=1` — установить, `=0` — снять |
| `household_join` | `household_join:spouse` | актёр входит в дом цели с ролью (`config.household_roles`; флаг) |
| `household_adopt` | `household_adopt:ward` | цель входит в дом актёра с ролью (флаг) |
| `household_leave` | `household_leave:quarrel` | актёр уходит из своего дома (ключ — причина; флаг) |
| `household_found` | `household_found:hearth` | актёр основывает новый дом (ключ — первый объект; флаг) |
| `commitment` | `commitment:repay_debt` | обязательство актёра перед целью (`config.commitments`: срок, последствия нарушения; флаг) |
| `death` | `t.death:combat` | смерть (причина из `config.death_causes`; запись `chronicle.death` в хронику; флаг) |
| `target_need` | `target_need:hunger+0.1` | устаревший синоним `t.need:` |

GOAP отслеживает только факты `has:*` и `at:*` актёра (сводка состояния). Остальные условия проверяются как
«статические» на момент планирования; эффекты на `need`, `skill`, `rel` и т. п. применяются исполнителем после
завершения действия.

### 1.3 Префикс `t.` — цель-персонаж

`t.` перед субъектом читает или меняет **целевого персонажа** взаимодействия (`target: character`) или слушателя в
диалоге, а не актёра.

- Условия: `t.need:`, `t.skill:`, `t.body:`, `t.age:`, `t.moodlet:`, а также `t.has:`, `t.trait:`, `t.knows:`,
  `t.reputation:`. Пример: `"pre": ["t.need:hunger<0.4", "!t.age:child"]`.
- Эффекты: `t.need:`, `t.skill:`, `t.body:`, `t.moodlet:`, `t.has:`, `t.reputation:`, `t.death:`.
  Пример: `"eff": ["t.skill:cooking+0.05", "t.moodlet:learned"]`.
- `rel:` / `rel_back:` всегда про пару актёр↔цель, префикс к ним не ставится.
- После `t.` — один субъект без точек (`t.a.b:x` — синтаксическая ошибка).
- Без цели-персонажа `t.`-условие ложно, `t.`-эффект пропускается; в данных валидатор считает это ошибкой (§12).

### 1.4 Статус интерпретатора

Парсер и валидатор знают весь язык; интерпретатор подключается поэтапно.

| Возможность | Парсер/валидатор | Исполнение |
|---|---|---|
| `t.` условия и эффекты, `rel`/`rel_back` (все оси), `reputation`, `moodlet`-условие, `death` | да | да (`evalSubject`, `applyEffects(w, c, def, object, targetChar)`) |
| `chance` / `on_fail` / `risk` | да | да (детерминированный бросок `rng(actor, tick, "chance:<id>")`, риски — `"risk:<id>:<risk>"`) |
| `params` (`$item`, `$skill`, `$place`) | да | подстановка `bindParams(def, {{"item", "berries"}})`; GOAP параметрические взаимодействия пока не планирует |
| `combat` | да | этап 2 (слой насилия): исход, урон, смерть |
| `terms` сделок | да | этап 2 (слой экономики и диалога) |
| `status_pair`, `household_*`, `commitment` | да | этап 2 (соц. слой); сейчас исполнитель их пропускает |
| `dialogue.json` | да (загрузка, валидация, ключи loc) | этап 2 (слой диалогов, шаг 4) |

### 1.5 Параметры `$name`

Взаимодействие может быть **параметрическим**: в `params` объявлены параметры, а в `pre`/`eff`/`on_fail`/`risk`/
`combat.outcomes`/`terms`/`chance.skill` вместо ключа стоит `$name`.

```json
{ "id": "give_item", "target": "character",
  "params": { "item": { "type": "item", "of": ["berries", "wood", "meal"] } },
  "pre": ["has:$item>=1"], "eff": ["has:$item-1", "t.has:$item+1", "rel_back:affinity+0.04"] }
```

- Объявление: `"params": { "<name>": "<type>" }` или `{ "<name>": { "type": "<type>", "of": [значения] } }`.
- Типы: `item` (id предмета), `skill` (навык/знание), `place` (тип объекта), `need`, `trait`.
- `of` — допустимые значения (пусто — любой id этого типа). Значение вне `of` — ошибка привязки.
- Тип параметра должен подходить субъекту: `has`/`home`/`target_stock` ← `item`; `skill`/`knows` ← `skill`;
  `at`/`known`/`spawn` ← `place`; `need` ← `need`; `trait` ← `trait`.
- Привязка: `bindParams(def, binding)` возвращает обычное (непараметрическое) взаимодействие. Её делает тот, кто
  выбирает взаимодействие: слой диалогов и команда игрока `perform` с аргументами (этап 2), позже — GOAP. По умолчанию параметрическое
  взаимодействие `goap: false`, в статическую проверку физической цепочки (§12) не входит.
- Параметр вне параметрического взаимодействия (в целях, HTN) — ошибка.

### 1.6 Оси отношений `rel`

`affinity` (симпатия), `trust` (доверие), `fear` (страх), `familiarity` (знакомство, 0..1), `debt` (долг, без
обрезки), `enmity` (вражда), `respect` (уважение), `attraction` (влечение). Все, кроме `familiarity` и `debt`, — -1..1.
Отношение направленное: A→B не равно B→A.

## 2. `items.json`

```json
{ "items": [
  { "id": "berries", "tags": ["food", "raw"], "nutrition": 0.12, "stack": 20 },
  { "id": "meal",    "tags": ["food", "cooked"], "nutrition": 0.45 },
  { "id": "axe",     "tags": ["tool"] }
]}
```
`tags` — свободные метки (условия вида `has:tag.food` — зарезервировано, пока не поддержано). Имя — `item.<id>.name`.

## 3. `objects.json`

```json
{ "objects": [
  { "id": "berry_bush", "stock": {"berries": 8}, "regrow": {"berries": 1}, "regrow_every": 72,
    "max_stock": {"berries": 8}, "biomes": ["meadow", "forest"], "per_km2": 6, "owned": false },
  { "id": "hearth", "settlement": true, "per_family": 1, "warmth": true },
  { "id": "bed", "settlement": true, "per_person": 1 },
  { "id": "storage", "settlement": true, "per_family": 1, "stock": {"meal": 2} }
]}
```
- `stock` — начальный запас; `regrow` + `regrow_every` (тики) — возобновление через очередь событий до `max_stock`.
- `biomes` + `per_km2` — природная расстановка генератором тестового мира; `settlement`/`per_family`/`per_person` — у домов.
- `owned` — у объекта есть владелец (семья). Взять из чужого — действие с юридическим тегом `theft`.
- Имя — `object.<id>.name` (существительное с падежами, `LOC_FORMAT.md` §2).

## 4. `needs.json`

```json
{ "needs": [
  { "id": "hunger", "decay_per_day": 0.55, "critical": 0.15, "start": [0.6, 0.9] },
  { "id": "energy", "decay_per_day": 0.9, "critical": 0.1, "night_mult": 1.0 },
  { "id": "warmth", "decay_per_day": 0.3, "critical": 0.15, "biome_mult": {"marsh": 1.6} }
],
  "moodlets": [ { "id": "ate_meal", "value": 0.15, "ttl": 72 }, { "id": "insulted", "value": -0.08, "ttl": 48 } ]
}
```
- `decay_per_day` — сколько теряется за игровые сутки (144 тика). `critical` — порог, ниже которого нужда прерывает
  текущую цель (см. `goals.json/interrupt`). `start` — диапазон стартового значения (детерминированно от seed).
- Черта может менять скорость: `traits.json/need_mult`.
- Имена — `need.<id>.name`, `moodlet.<id>.name`.

## 5. `traits.json`

```json
{ "traits": [
  { "id": "lazy", "goal_mult": {"gather_food": 0.7, "rest": 1.3}, "need_mult": {"energy": 1.2} },
  { "id": "honest", "legal_penalty": {"theft": 3.0} },
  { "id": "desperate_thief", "legal_penalty": {"theft": 0.2} },
  { "id": "stubborn", "hysteresis_mult": 1.6 },
  { "id": "gluttonous", "goal_mult": {"eat": 1.25} }
],
  "exclusive": [["honest", "desperate_thief"]]
}
```
- `goal_mult` — множитель итогового балла цели. `need_mult` — множитель скорости падения нужды.
- `legal_penalty` — множитель штрафа за юридический тег метода/взаимодействия (базовый штраф — `config.json/legal_base`).
  Закон не убирает вариант, а меняет его цену (GDD §1.2).
- `method_bias` — `{ "method_id": delta }`, сдвиг стоимости метода HTN (меньше — предпочтительнее).
- `hysteresis_mult` — множитель бонуса удержания текущей цели (упорство).
- `exclusive` — пары несовместимых черт (валидатор).
- Имя и описание — `trait.<id>.name`, `trait.<id>.desc`.

## 6. `goals.json` (слой 1, Utility)

```json
{ "goals": [
  { "id": "eat", "task": "obtain_food", "priority_class": "need",
    "aggregate": "product",
    "considerations": [
      { "input": "need:hunger", "curve": {"type": "inverse_logistic", "k": 10, "x0": 0.45}, "weight": 1.0 },
      { "input": "time:night", "curve": {"type": "linear", "m": -0.3, "b": 1.0} }
    ],
    "min_ticks": 3, "interrupt": "need:hunger<0.15", "cooldown_on_fail": 12 }
]}
```
- `task` — корневая задача HTN (`htn.json`). Цель без хотя бы одного выполнимого метода не выбирается (валидатор: хотя бы
  один метод вообще существует).
- `aggregate`: `product` (произведение с компенсацией по числу соображений, Dave Mark) или `sum` (взвешенная сумма / сумма весов).
- `considerations[].input` — источник значения: `need:<id>`, `time:night`, `body:health`, `body:fatigue`, `has:<item>`
  (нормировано `/norm`, по умолчанию 10), `home:<item>` (нормировано), `mood`, `const` (=1). `weight` — для `sum`; для
  `product` — показатель степени (значение^weight).
- `curve.type`: `linear` (m·x+b), `inverse` (1−x), `quadratic` (x^p, параметр `p`), `logistic` (1/(1+e^(−k(x−x0)))),
  `inverse_logistic` (1 − logistic), `step` (x ≥ `x0` → `hi` иначе `lo`), `const` (`v`). Результат обрезается в 0..1.
  Валидатор проверяет параметры: `k` ∈ [0.1, 50], `x0` ∈ [0,1], `p` ∈ [0.1, 8].
- `base` — множитель класса цели (по умолчанию 1). `priority_class`: `need` / `work` / `social` / `idle` — для отчётов.
- `min_ticks` — минимальное время жизни цели. `interrupt` — условие, при котором эта цель прерывает любую текущую.
- `cooldown_on_fail` — тиков штрафа после провала цели (её балл ×`config.fail_penalty`).
- Имя — `goal.<id>.name`; цели уровня кода `queued` и `player_action` тоже имеют ключи.

## 7. `htn.json` (слой 2, HTN)

```json
{ "tasks": [
  { "id": "obtain_food", "methods": [
    { "id": "eat_carried", "pre": ["has:meal>=1"], "cost": 1, "steps": [ {"do": "eat_meal"} ] },
    { "id": "eat_stored", "pre": ["home:meal>=1"], "cost": 3,
      "steps": [ {"achieve": ["has:meal>=1"]}, {"do": "eat_meal"} ] },
    { "id": "cook_then_eat", "pre": ["knows:cooking", "known:hearth"], "cost": 6,
      "steps": [ {"achieve": ["has:meal>=1"], "allow": ["cook_meal", "gather_berries"]}, {"do": "eat_meal"} ] },
    { "id": "forage", "pre": ["knows:foraging", "known:berry_bush"], "cost": 5,
      "steps": [ {"achieve": ["has:berries>=2"]}, {"do": "eat_berries"} ] },
    { "id": "steal_food", "pre": ["known:storage"], "cost": 4, "legal": ["theft"],
      "steps": [ {"achieve": ["has:meal>=1"], "allow": ["steal_meal"]}, {"do": "eat_meal"} ] }
  ]}
]}
```
- Метод: `pre` (условия §1), `cost` (база), `legal` (теги → штраф по `traits.legal_penalty` и `config.legal_base`), `steps`.
- Шаг: `{"do": "<interaction>"}` — выполнить взаимодействие (GOAP добавит переход к цели, если у взаимодействия есть
  `target`), `{"achieve": [условия has/at]}` — подцель для GOAP; `allow` — белый список взаимодействий для этой подцели
  (по умолчанию — все с `"goap": true` без юридических тегов), `{"task": "<id>"}` — вложенная составная задача
  (глубина ≤ `config.htn_max_depth`).
- Выбор метода: минимальная оценка `cost + trait.method_bias + legal_penalty + fail_penalty`. Отброшенные методы и причины
  пишутся в трассу. После `config.method_max_fails` провалов подряд метод штрафуется на `config.method_fail_ticks`.
- Имя задачи — `task.<id>.name`.

## 8. `interactions.json` (единый каталог, GOAP-действия)

Одна запись описывает взаимодействие, доступное **любому персонажу** (игрок и NPC вызывают одну функцию). GOAP строит
цепочки из записей с `"goap": true`.

```json
{ "interactions": [
  { "id": "gather_berries", "category": "work", "target": "object:berry_bush",
    "pre": ["knows:foraging", "target_stock:berries>=1"],
    "eff": ["has:berries+2", "target_stock:berries-2", "skill:foraging+0.01", "body:fatigue+0.02"],
    "from_target": true, "duration": 3, "cost": 3, "fsm": "Work", "reserve": true, "goap": true,
    "legal": [], "visibility": "public", "social": null, "speech": ["offer", "request"] }
]}
```
| поле | обяз. | смысл |
|---|---|---|
| `id` | да | уникальный идентификатор; имя — `interaction.<id>.name` |
| `category` | да | `household` / `work` / `social` / `economy` / `violence` / `family` / `knowledge` / `faith` / `medicine` / `travel` / `politics` (CORE §4.1) |
| `target` | да | `self`, `object:<type>`, `character` |
| `pre` | нет | условия §1 (включая `target_stock`, `target_owner`, `rel`, `t.*`) |
| `eff` | нет | эффекты §1 |
| `duration` | да | тиков исполнения в FSM (≥ 1) |
| `cost` | да | стоимость для GOAP (время/усталость/риск) |
| `fsm` | да | состояние FSM из `fsm.json` (`Work`, `Eat`, `Sleep`, `Interact`, `Talk`, `Fight`, ...) |
| `reserve` | нет | резервировать целевой объект на время плана (по умолчанию `false`) |
| `goap` | нет | доступно планировщику (по умолчанию `true` для `object`/`self` без `params`) |
| `legal` | нет | юридические теги (`theft`, `assault`, ...): меняют цену, не убирают вариант |
| `visibility` | нет | `public` / `private` / `secret` — кто может стать свидетелем |
| `social` | нет | смысл для отношений: `{ "meaning": "gift", "witness_affinity": -0.1 }` |
| `speech` | нет | речевые акты (`dialogue.json/speech_acts`), которыми взаимодействие можно предложить/попросить/пообещать в диалоге |
| `continuous` | нет | длится, пока не выполнено `until` (например, сон до `need:energy>=0.95`), с лимитом `duration` |
| `until` | нет | условие окончания для `continuous` |
| `params` | нет | параметры `$name` (§1.5) |
| `chance` | нет | вероятность успеха: число или `{ "base", "skill", "per_skill", "min", "max" }`; p = clamp(base + per_skill·skill, min, max). Не для `continuous` |
| `on_fail` | нет | эффекты вместо `eff`, если бросок `chance` не удался |
| `risk` | нет | побочные риски `[{ "id", "p", "eff" }]`: каждый бросается отдельно после действия (успешного или нет); имя — `risk.<id>.name` |
| `combat` | нет | бой (только `target: character`): `{ "attack_skill", "defense_skill", "damage": [lo, hi], "lethal", "outcomes": { "win"/"lose"/"draw"/"flee": [эффекты] } }` |
| `terms` | нет | условия сделки (только `target: character`): `{ "give": {предмет: n}, "take": {предмет: n}, "due": тики, "commitment", "on_breach": [эффекты] }` |

Пример социальных взаимодействий с новыми полями:

```json
{ "id": "teach_skill", "category": "knowledge", "target": "character",
  "params": { "skill": { "type": "skill", "of": ["foraging", "cooking"] } },
  "pre": ["skill:$skill>=0.5", "t.skill:$skill<0.5", "!t.age:elder"],
  "chance": { "base": 0.4, "skill": "$skill", "per_skill": 0.5 },
  "eff": ["t.skill:$skill+0.05", "t.moodlet:learned", "rel_back:respect+0.05"],
  "on_fail": ["rel_back:respect-0.01"], "duration": 6, "cost": 3, "fsm": "Talk", "speech": ["offer", "request"] },
{ "id": "brawl", "category": "violence", "target": "character", "pre": ["rel:enmity>=0.5", "!t.age:child"],
  "combat": { "damage": [0.05, 0.25], "lethal": false,
              "outcomes": { "win": ["moodlet:won_fight", "reputation:valor+0.03", "t.moodlet:wounded"],
                            "lose": ["moodlet:wounded", "t.moodlet:won_fight"], "flee": ["reputation:valor-0.02"] } },
  "risk": [ { "id": "broken_bone", "p": 0.05, "eff": ["body:health-0.2"] } ],
  "eff": ["rel_back:enmity+0.1", "rel_back:fear+0.05", "memory:fought"],
  "duration": 3, "cost": 5, "fsm": "Fight", "legal": ["assault"] },
{ "id": "lend_item", "category": "economy", "target": "character",
  "params": { "item": { "type": "item", "of": ["berries", "wood", "meal"] } },
  "pre": ["has:$item>=1", "rel:trust>=0.2"],
  "terms": { "give": {"$item": 1}, "take": {"$item": 1}, "due": 288, "commitment": "repay_debt",
             "on_breach": ["rel:trust-0.3", "t.reputation:honesty-0.1"] },
  "duration": 1, "cost": 1, "fsm": "Talk", "speech": ["offer", "promise"] }
```

- `chance`: один детерминированный бросок на (актёр, тик, взаимодействие). Успех — `eff`, неудача — `on_fail`.
- `combat`: сила стороны — её навык (`attack_skill` у актёра, `defense_skill` у цели); проигравший теряет здоровье в
  диапазоне `damage`; `lethal` — проигравший с нулевым здоровьем умирает (причина `combat`); затем эффекты исхода
  (`win`/`lose` — с точки зрения актёра, `t.` — про цель). Исполнение — этап 2.
- `terms`: `give` — что актёр отдаёт цели, `take` — что получает (ключи — предметы или `$параметры`). `due` = 0 — обмен
  на месте; `due` > 0 — отложенная часть становится обязательством `commitment` (§10) со сроком; нарушение применяет
  `on_breach` сделки и `on_breach` обязательства. Исполнение — этап 2.

Переход к объекту (`goto`) не пишется в каталоге: GOAP строит его сам для каждого `target: object:<type>`; стоимость —
расстояние до ближайшего известного свободного экземпляра / скорость (`config.walk_m_per_tick`).

## 9. `fsm.json` (слой 4)

```json
{ "states": [
  { "id": "Idle" }, { "id": "Move", "interruptible": true },
  { "id": "Act", "interruptible": true },
  { "id": "Work", "parent": "Act" }, { "id": "Eat", "parent": "Act" }, { "id": "Interact", "parent": "Act" },
  { "id": "Talk", "parent": "Act" }, { "id": "Fight", "parent": "Act", "interruptible": false },
  { "id": "Rest", "interruptible": true }, { "id": "Sleep", "parent": "Rest", "interruptible": false, "wake_on": ["threat"] }
]}
```
- `parent` — иерархия (подсостояние наследует прерываемость родителя, если не указано своё).
- `interruptible` — можно ли прервать критичной нуждой; `wake_on` — какие события прерывают непрерываемое.
- Headless-исполнение: состояние + таймер (`duration` взаимодействия, для `Move` — расстояние/скорость).
- Имя состояния — `fsm.<id>.name`.

## 9a. `dialogue.json` (речевые акты и диалоги)

```json
{ "speech_acts": ["greet", "smalltalk", "offer", "request", "promise", "threaten", "insult", "gossip", "propose",
                  "comfort", "agree", "refuse"],
  "templates": [
    { "id": "greet", "speech": "greet",
      "eff": ["rel:familiarity+0.02", "rel_back:familiarity+0.02"],
      "variants": [ { "id": "warm", "pre": ["rel:affinity>=0.4"] }, { "id": "plain" },
                    { "id": "cold", "pre": ["rel:enmity>=0.3"] } ],
      "replies": ["greet", "smalltalk"] },
    { "id": "propose_marriage", "speech": "propose",
      "pre": ["rel:attraction>=0.5", "rel:respect>=0.2", "!age:child", "!t.age:child"],
      "eff": ["status_pair:betrothed", "commitment:wedding"],
      "variants": [ "plain", { "id": "shy", "pre": ["rel:familiarity<0.5"] } ],
      "replies": ["agree", "refuse"] }
  ]
}
```

- `speech_acts` — словарь речевых актов. Им пользуются `interactions[].speech` и шаблоны; имя — `speech.<act>.name`.
- Шаблон: `id`, `speech` (речевой акт), `pre` (условия §1: актёр — говорящий, `t.` — слушатель, `rel` — говорящий→
  слушатель), `eff` (эффекты при произнесении), `variants`, `replies` (шаблоны, которыми слушатель может ответить).
- Вариант: строка-id или `{ "id", "pre", "weight" }`. Выбирается среди вариантов с выполненными `pre` по весу.
- **Текста в `dialogue.json` нет.** Реплика варианта — ключ `dialogue.<template>.<variant>` в
  `content/loc/<lang>/dialogue.json`, с плейсхолдерами `{speaker}`, `{listener:dat}`, `{speaker:g|рад|рада}`,
  `{qty:plural|ягода|ягоды|ягод}` (`LOC_FORMAT.md`).

## 10. `config.json`

```json
{ "top_k": 3, "hysteresis_bonus": 0.15, "switch_margin": 0.1, "fail_penalty": 0.3,
  "htn_max_depth": 4, "method_max_fails": 3, "method_fail_ticks": 72,
  "goap_max_depth": 10, "goap_max_nodes": 2000, "reserve_ticks": 36,
  "walk_m_per_tick": 800, "interaction_range_m": 15,
  "legal_base": 4.0, "trace_size": 64,
  "player_idle_resume_ticks": 18, "player_goal_hold_ticks": 36, "player_goal_hold_bonus": 0.3,
  "hint_decay_ticks": 72, "memory_cap": 200,
  "modifier_bounds": { "max_abs_delta": 0.5, "max_ttl": 288 },
  "memory_types": { "starved": 0.6, "ate_meal": 0.05 },
  "reputation_axes": ["honesty", "valor", "piety", "generosity", "notoriety"],
  "status_pairs": ["betrothed", "married", "master_apprentice", "lord_vassal", "sworn_kin"],
  "household_roles": ["head", "spouse", "child", "kin", "servant", "ward"],
  "death_causes": ["starvation", "combat", "illness", "old_age", "execution"],
  "commitments": {
    "wedding": { "due": 1008, "on_breach": ["rel_back:trust-0.4", "rel_back:enmity+0.3", "reputation:honesty-0.1"] },
    "repay_debt": { "due": 432, "on_breach": ["rel_back:trust-0.3", "reputation:honesty-0.15"] } } }
```

- `reputation_axes`, `status_pairs`, `household_roles`, `death_causes` — словари для `reputation:`, `status_pair:`,
  `household_join/adopt:`, `death:`; у каждого значения есть ключ локализации (`reputation.<id>.name`,
  `status_pair.<id>.name`, `household_role.<id>.name`, `cause.<id>.name`). Без поля действуют значения выше.
- `commitments` — типы обязательств: срок `due` (тики, > 0) и `on_breach` (эффекты; `t.`/`rel_back` — тот, перед кем
  обязались); имя — `commitment.<id>.name`.

## 11. Тестовый мир и сценарии (`content/sim/`)

`test_world.json`: `seed`, `size_m` (8000..16000), `cell_m`, `biomes` (`[{ "id": "forest", "warmth": 0.9 }, ...]`),
`river`, `families` (число), `people_per_family` ([min, max]), `traits_pool`, `knowledge_start`.
Сценарий: `{ "id", "world": "test_world.json", "overrides": {...}, "days": N, "commands": [ { "tick", "type", ... } ],
"expect": { ... } }` — команды идут в журнал команд и исполняются так же, как от игрока.

## 12. Правила валидатора

Ошибки (загрузка падает): ссылки на несуществующие id (цель→задача, метод→взаимодействие/задача, взаимодействие→тип объекта /
предмет / состояние FSM, черта/нужда/moodlet в условиях); синтаксис условий/эффектов; параметры кривых вне диапазона;
`duration < 1`; цикл вложенных задач HTN; цель без методов; подцель `achieve`, недостижимая из **пустого старта** через
GOAP-действия (без инвентаря, но с объектами мира; параметрические и `t.`-факты не учитываются) — проверка физической
цепочки «ничто из воздуха»; предмет, который нигде не производится и нигде не лежит в стартовом запасе, но требуется в
`pre`; взаимоисключающие черты.

v0.2, ошибки:
- неизвестная ось `rel`/`reputation`, пара статусов, роль в доме, обязательство, причина смерти; ключ `body` не
  `health`/`fatigue`;
- `t.`-субъект, не поддерживающий цель; `t.`, `rel`, `rel_back`, `status_pair`, `household_join/adopt`, `commitment`
  без цели-персонажа; неверная форма флагового эффекта (`status_pair` — флаг или `=0`/`=1`);
- параметр `$x` вне параметрического взаимодействия, необъявленный, не того типа или со значением `of` неизвестного id;
  дубликат параметра, неизвестный тип параметра;
- `chance` у `continuous`, `chance`/`risk.p` вне [0, 1], риск без `id` или `eff`, дубликат риска;
- `combat`/`terms` не у `target: character`; урон не `[lo ≤ hi]` в [0, 1]; неизвестный исход боя; `lethal` без
  `combat` в `death_causes`; `terms` с неизвестным предметом, количеством ≤ 0, пустые, `due > 0` без `commitment`;
- неизвестный речевой акт (во взаимодействии и шаблоне), шаблон без вариантов, дубликат варианта, отрицательный вес,
  ответ на неизвестный шаблон, обязательство с `due ≤ 0`.

Локализация проверяется отдельно (`validateLoc`, `LOC_FORMAT.md` §6): ключ из данных обязан быть в базовом языке
(ошибка), недостающий перевод — предупреждение.

Предупреждения: неизвестные поля; взаимодействие, на которое никто не ссылается (кроме `target: character` — их вызывают
диалог и игрок); цель с балл ≡ 0 на всём диапазоне; `on_fail` без `chance`; неиспользуемый параметр; параметрическое
взаимодействие с `goap: true` (GOAP их пока не планирует).
