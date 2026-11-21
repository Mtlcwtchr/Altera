# LOC v0.2 — формат морфологии (AL-47, T1). Итоговый для T3/T5a/T5b

Надмножество LOC v0.1: всё старое работает как было. Любые `*.json` в `content/loc/<lang>/` грузятся (имена файлов
свободные: `lex_verbs.json`, `lex_adj.json`, `frames.json`, `wraps.json` …). Грамматика языка — `_grammar.json` (T1).

## 1. Значения ключей

| Что | Где | Форма |
|---|---|---|
| строка-шаблон | любой ключ | `"..."` |
| **варианты** подправила | любой ключ | **массив строк** `["вариант 1", "вариант 2"]`, выбор от сида |
| существительное | любой ключ | `{nom, gen, dat, acc, ins, pre, g, anim?, pl?}`; `anim: true` — одушевлённое; `pl: {nom..pre}` — мн. число |
| глагол ru | `lex.verb.<id>` | `{inf, pfv, past:{m,f,n,pl}, pres|fut:{1s,2s,3s,1p,2p,3p}, syn:[<id>…]}` |
| глагол en | `lex.verb.<id>` | `{base, past, 3s, ing, pp, syn:[…]}` |
| прилагательное ru | `lex.adj.<id>` | `{m|f|n|pl: {nom,gen,dat,acc,acc_an,ins,pre}}` (`acc_an` — вин. для одуш.; нет — берётся gen) |
| прилагательное en | `lex.adj.<id>` | строка или `{base}` |
| существительное-лексема | `lex.noun.<id>` | как существительное |
| фрейм предиката | `frame.<pred>` | см. §3 |

Всё под `lex.<pos>.<id>` и `frame.<id>` — один лист (внутрь не раскрывается). `syn` — **id других `lex.verb`**
(не слова: синоним тоже спрягается). Пустой `pres`/`fut` у ru-глагола: будущее несов. вида = «буду» + inf.

## 2. Плейсхолдеры (новое)

Модификаторы пишутся через `:` в любом порядке: `{who:pro:dat}`, `{x:pre:prep|о}`.

| Запись | Смысл |
|---|---|
| `{>key}` | раскрыть подправило `key` того же языка с теми же аргументами (строка или массив вариантов) |
| `{>key?}` | то же, но если в подправиле не хватает аргумента — пусто, без ошибки |
| `{x?}` | необязательный аргумент: нет — пусто |
| `{>prop}` = `{>prop:clause}`, `{>prop:nominal[:case]}`, `{>prop:about}` | реализовать аргумент-пропозицию через `frame.<pred>`; аргумент-строка вставляется как есть |
| `{verb:agree=doer}` | (во фрейме) глагол фрейма по роли: прош. — род/число, наст./буд. — лицо/число; время из пропозиции |
| `{verb:past:agree=doer}`, `{verb:inf}` | время/форма явно |
| `{verb.steal:agree=doer}` | конкретная лексема `lex.verb.steal` |
| `{adj.whole:agree=object:acc}` | прилагательное `lex.adj.whole` в роде/числе/одуш. аргумента и падеже |
| `{who:pro}` / `{who:pro:dat}` | первое упоминание в реплике — имя, дальше — местоимение (он/её/ему; после предлога «него»). `{who:pron}` — всегда местоимение |
| `{x:prep|о}` | предлог по правилу языка + слово: «о мельнике», «об Олеге», «обо мне»; en `a`→`an` |
| `{listener:ty}` / `{listener:ty:dat}` | ты/вы по регистру аргумента (`reg: "ty"|"vy"`), в падеже |
| `{listener:ty|знаешь|знаете}` | выбор по регистру |
| `{listener:addr}` | обращение (`addr` у аргумента: «господин», «брат»), нет — пусто |
| `{x:num|pl}`… | (не нужно) число берётся из `n` или `num:"pl"` аргумента: тогда формы `pl`, род `pl` |

Итог реплики: первая буква заглавная, двойные пробелы и пробел перед `,.!?:;` убираются (пустые `?`-части не оставляют дыр).

## 3. Фрейм `frame.<pred>`

```json
"frame.theft": {
  "verb": "steal",                       // id lex.verb или массив id (вариант от сида)
  "noun": "lex.noun.theft",              // голова номинализации (для :nominal / :about)
  "roles": { "doer": "nom", "object": "acc", "victim": "у+gen", "victim_family": "у+gen" },
  "clause":  ["{doer} {time?} {verb:agree=doer} {victim?} {object?}", "..."],
  "nominal": "{noun} {object:gen?} {victim?}",
  "about":   "{noun:pre:prep|о} {object:gen?}"   // необязательно: по умолчанию предлог+падеж из _grammar.about + nominal
}
```

- Роль-пропозиция (`content` у say/think) управляется формой: `"content": "clause"` (или `nominal`, `about`).
- Имена ролей — как в `content/ai/predicates.json` (doer, victim, victim_family, object, beneficiary, content…).
- Во фрейме `{role}` выводится **по управлению** (`"у+gen"` → «у мельника», `"acc"` → вин.); явный падеж
  `{role:gen}` — без предлога. `{role?}` — роль необязательна.
- Если роль — сам говорящий или слушатель (их `id` совпал с `speaker`/`listener`), она автоматически становится
  «я»/«ты|вы» в нужном падеже, и глагол согласуется по лицу: «это ты украл у меня муку».
- `neg: true` → «не» перед глаголом (ru), `did not`/`does not`/`will not` (en). `{noun}` в `nominal` — в падеже,
  запрошенном снаружи (`{>prop:nominal:acc}`).
- Доп. аргументы фрейма: `qty` (число), `unit` (существительное), `time` (строка/сущ., «ночью»), `place`.

## 4. Аргумент-пропозиция (что передаёт T3)

```json
{"pred": "theft", "tense": "past", "neg": false, "modality": "fact", "qty": 1,
 "unit": {...noun...}, "time": "ночью",
 "roles": {"doer": {"nom":"Олег", ..., "g":"m", "id": 12}, "object": {...noun...}, "victim": {...}}}
```

Узнаётся по полю `pred`. Персонажи — `personNoun` + `"id"`. В аргументах реплики `speaker`/`listener` тоже с `id`,
у `listener` — `reg` и `addr`. Хелпер в коде: `propLocArg(prop, refNoun)` (`src/sim/loc_gen.hpp`).

## 5. API (src/sim/loc_gen.hpp)

- `Loc::text(lang, key, args)` понимает весь v0.2 (сид 0).
- `sayLoc(loc, lang, key, args, seed, errors)` — реплика с сидом, заглавной буквой и чисткой пробелов.
- `realizeProp(loc, lang, propArg, form, case, args, seed)` — одна пропозиция.
- `validateLocGen(loc, predicates, roles)` — фрейм на каждый предикат в базовом языке (пока во всём базовом языке нет
  ни одного фрейма — предупреждение, потом ошибка), роли фрейма есть у предиката и с падежом, все `{>a.b}` и
  `{verb.x}` существуют, лексемы глаголов/прилагательных полные, `syn` ссылаются на `lex.verb`, вложенность ≤ 6 без
  циклов. Структуру проверяет и `validateLoc`; `altera_sim_runner --validate` — всё вместе.
- Грамматика: `_grammar.json` ru (`verb_model`, `pronouns`, `pronouns_after_prep`, `prep_words`, `prepositions`, `neg`,
  `future_aux`, `about`) и новый `content/loc/en/_grammar.json`. Новый язык = свой `_grammar.json`; модель спряжения
  пока `ru` или `en` (третья модель — правка кода).
- `{>key}` в `Loc::text` берёт первый вариант массива; сид — только в `sayLoc`/`realizeProp`.
