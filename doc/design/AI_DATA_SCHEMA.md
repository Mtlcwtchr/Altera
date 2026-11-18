# Altera — схема данных ИИ (`content/ai/`), v0.1

Дополняет `ALTERA_AI_ARCHITECTURE_v0.1.md` и `ALTERA_CORE_SPEC_v0.1.md`. Это контракт между контентом и кодом
кора (`src/sim/`): код — интерпретатор, всё поведение задаётся этими файлами. Формат — JSON (UTF-8, без комментариев;
поле `"_doc"` в любом объекте разрешено и игнорируется). Идентификаторы — `snake_case` латиницей, уникальны в своём файле.

Загрузка: `altera::sim::Content::load(dir)`; валидатор `altera::sim::validate(content)` запускается при загрузке и в тестах
(`altera_sim_tests`), `altera_sim_runner --validate` печатает ошибки. Неизвестные поля — предупреждение, не ошибка.

## 0. Файлы

| Файл | Что | Слой |
|---|---|---|
| `items.json` | предметы (еда, материалы, инструменты) | мир |
| `objects.json` | типы объектов мира (куст, дерево, очаг, кровать, склад) | мир |
| `needs.json` | нужды: скорость падения, пороги | вход Utility |
| `traits.json` | черты: множители целей, штрафы/допуски методов | Utility/HTN |
| `goals.json` | типы целей, соображения, кривые, гистерезис | 1 Utility |
| `htn.json` | составные задачи и методы | 2 HTN |
| `interactions.json` | **единый каталог взаимодействий** (= действия GOAP и команды игрока) | 3 GOAP / 4 FSM |
| `fsm.json` | иерархия состояний тела, прерываемость | 4 FSM |
| `config.json` | глобальные параметры ИИ (top-K, гистерезис, лимиты GOAP, границы модификаторов) | все |

Тестовый мир и сценарии лежат отдельно: `content/sim/test_world.json`, `content/sim/scenarios/*.json` (§9).

## 1. Мини-язык условий и эффектов

Условия (`pre`, `interrupt`, предусловия методов) и эффекты (`eff`) — строки. Одна строка = один факт.

**Условие:** `[!]subject:key[op value]`. Без `op` — булево (истинно, если значение > 0). `!` — отрицание.
`op`: `>=`, `<=`, `>`, `<`, `==`, `!=`. `value` — число.

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
| `rel` | `affinity`/`trust`/`fear`/`familiarity` | отношение актёр→цель (персонаж), -1..1 | соц. |

**Эффект:** `subject:key<+|-|=>value` или `subject:key` (для `moodlet`, `memory`, `spawn`).

| subject | Пример | Смысл |
|---|---|---|
| `has` | `has:berries+3` | инвентарь актёра (берётся из `target_stock`, если взаимодействие с объектом и указано `"from_target": true`) |
| `need` | `need:hunger+0.45` | нужда актёра, обрезка 0..1 |
| `target_stock` | `target_stock:berries-3` | запас целевого объекта (не уходит ниже 0) |
| `home` | `home:meal+1` | склад семьи |
| `skill` | `skill:foraging+0.01` | рост навыка |
| `body` | `body:fatigue+0.05` | тело |
| `moodlet` | `moodlet:ate_meal` | добавить moodlet из `needs.json/moodlets` |
| `memory` | `memory:ate_alone` | запись в память актёра (тип, значимость из `config.json/memory_types`) |
| `rel` / `rel_back` | `rel:affinity+0.05` | отношение актёр→цель / цель→актёр (соц. взаимодействия) |
| `spawn` | `spawn:house` | создать объект типа у позиции актёра (владелец — семья актёра) |

GOAP отслеживает только факты `has:*` и `at:*` (сводка состояния). Остальные условия проверяются как «статические» на момент
планирования; эффекты на `need`, `skill`, `rel` и т. п. применяются исполнителем после завершения действия.

## 2. `items.json`

```json
{ "items": [
  { "id": "berries", "tags": ["food", "raw"], "nutrition": 0.12, "stack": 20 },
  { "id": "meal",    "tags": ["food", "cooked"], "nutrition": 0.45 },
  { "id": "axe",     "tags": ["tool"] }
]}
```
`tags` — свободные метки (условия вида `has:tag.food` — зарезервировано, пока не поддержано).

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

## 4. `needs.json`

```json
{ "needs": [
  { "id": "hunger", "decay_per_day": 0.55, "critical": 0.15, "start": [0.6, 0.9] },
  { "id": "energy", "decay_per_day": 0.9, "critical": 0.1, "night_mult": 1.0 },
  { "id": "warmth", "decay_per_day": 0.3, "critical": 0.15, "biome_mult": {"marsh": 1.6} },
  { "id": "social", "decay_per_day": 0.35, "critical": 0.05 }
],
  "moodlets": [ { "id": "ate_meal", "value": 0.15, "ttl": 72 }, { "id": "starving", "value": -0.4, "ttl": 24 } ]
}
```
- `decay_per_day` — сколько теряется за игровые сутки (144 тика). `critical` — порог, ниже которого нужда прерывает
  текущую цель (см. `goals.json/interrupt`). `start` — диапазон стартового значения (детерминированно от seed).
- Черта может менять скорость: `traits.json/need_mult`.

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
| `id` | да | уникальный идентификатор |
| `category` | да | `household` / `work` / `social` / `economy` / `violence` / `family` / `knowledge` / `faith` / `medicine` / `travel` / `politics` (CORE §4.1) |
| `target` | да | `self`, `object:<type>`, `character` |
| `pre` | нет | условия §1 (включая `target_stock`, `target_owner`, `rel`) |
| `eff` | нет | эффекты §1 |
| `duration` | да | тиков исполнения в FSM (≥ 1) |
| `cost` | да | стоимость для GOAP (время/усталость/риск) |
| `fsm` | да | состояние FSM из `fsm.json` (`Work`, `Eat`, `Sleep`, `Interact`, `Talk`, `Fight`, ...) |
| `reserve` | нет | резервировать целевой объект на время плана (по умолчанию `false`) |
| `goap` | нет | доступно планировщику (по умолчанию `true` для `object`/`self`) |
| `legal` | нет | юридические теги (`theft`, `assault`, ...): меняют цену, не убирают вариант |
| `visibility` | нет | `public` / `private` / `secret` — кто может стать свидетелем |
| `social` | нет | смысл для отношений: `{ "meaning": "gift", "witness_affinity": -0.1 }` |
| `speech` | нет | речевые акты, которыми взаимодействие можно предложить/попросить/пообещать в диалоге |
| `continuous` | нет | длится, пока не выполнено `until` (например, сон до `need:energy>=0.95`), с лимитом `duration` |
| `until` | нет | условие окончания для `continuous` |

Переход к объекту (`goto`) не пишется в каталоге: GOAP строит его сам для каждого `target: object:<type>`; стоимость —
расстояние до ближайшего известного свободного экземпляра / скорость (`config.walk_m_per_tick`).

## 9. `fsm.json` (слой 4)

```json
{ "states": [
  { "id": "Idle" }, { "id": "Move", "interruptible": true },
  { "id": "Act", "interruptible": true },
  { "id": "Work", "parent": "Act" }, { "id": "Eat", "parent": "Act" }, { "id": "Interact", "parent": "Act" },
  { "id": "Rest", "interruptible": true }, { "id": "Sleep", "parent": "Rest", "interruptible": false, "wake_on": ["threat"] }
]}
```
- `parent` — иерархия (подсостояние наследует прерываемость родителя, если не указано своё).
- `interruptible` — можно ли прервать критичной нуждой; `wake_on` — какие события прерывают непрерываемое.
- Headless-исполнение: состояние + таймер (`duration` взаимодействия, для `Move` — расстояние/скорость).

## 10. `config.json`

```json
{ "top_k": 3, "hysteresis_bonus": 0.15, "switch_margin": 0.1, "fail_penalty": 0.3,
  "htn_max_depth": 4, "method_max_fails": 3, "method_fail_ticks": 72,
  "goap_max_depth": 10, "goap_max_nodes": 2000, "reserve_ticks": 36,
  "walk_m_per_tick": 800, "interaction_range_m": 15,
  "legal_base": 4.0, "trace_size": 64,
  "modifier_bounds": { "max_abs_delta": 0.5, "max_ttl": 288 },
  "memory_types": { "starved": 0.6, "ate_meal": 0.05 } }
```

## 11. Тестовый мир и сценарии (`content/sim/`)

`test_world.json`: `seed`, `size_m` (8000..16000), `cell_m`, `biomes` (`[{ "id": "forest", "warmth": 0.9 }, ...]`),
`river`, `families` (число), `people_per_family` ([min, max]), `traits_pool`, `knowledge_start`.
Сценарий: `{ "id", "world": "test_world.json", "overrides": {...}, "days": N, "commands": [ { "tick", "type", ... } ],
"expect": { ... } }` — команды идут в журнал команд и исполняются так же, как от игрока.

## 12. Правила валидатора

Ошибки (загрузка падает): ссылки на несуществующие id (цель→задача, метод→взаимодействие/задача, взаимодействие→тип объекта /
предмет / состояние FSM, черта/нужда в условиях); синтаксис условий/эффектов; параметры кривых вне диапазона;
`duration < 1`; цикл вложенных задач HTN; цель без методов; подцель `achieve`, недостижимая из **пустого старта** через
GOAP-действия (без инвентаря, но с объектами мира) — проверка физической цепочки «ничто из воздуха»; предмет, который
нигде не производится и нигде не лежит в стартовом запасе, но требуется в `pre`; взаимоисключающие черты.
Предупреждения: неизвестные поля; взаимодействие, на которое никто не ссылается; цель с балл ≡ 0 на всём диапазоне.
