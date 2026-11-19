# Анимации на риге Quaternius UAL: покрытие и пробелы

Собрано клипов: **1100** (ошибок выгрузки: 0). По источникам: cmu 262, iglesias 358, mixamo 365, motifect 31, ual 84.

Файлы: `content/animations/humanoid_ual/clips/<pack>/<id>.glb`, список и привязки: `manifest.json`. Подбор клипов к ключам сделан по названиям и выборочным превью; перед использованием посмотреть в просмотрщике.

## Бой (clip_map.json / animations.json)
Ключей: 116. Покрыто клипом: 36, клип плюс процедурная доработка: 72, пробелов: 8.

Пробелы (клипа нет): `bandage_arm`, `bandage_torso`, `bind_hands`, `carry_shoulder`, `exec_kneel_stab`, `feint_eye_dart`, `tourniquet_leg`, `wipe_blade`.

Звериные ключи (`atk_beast_bite`, `atk_beast_bite_low`, `atk_beast_claw_l`, `atk_beast_claw_r`, `atk_beast_pounce`, `death_beast_roll`, `growl_low`, `rise_roar`, `stance_beast`) относятся к ригу животных Quaternius, в эту работу не входят.

## Взаимодействия (interactions.json)
Ключей: 149. Покрыто клипом: 146, клип плюс процедурная доработка: 0, пробелов: 3.

Пробелы (клипа нет): `intimacy`, `release_captive`, `execute`.

## Что закрыто только приблизительно
Ключи ниже получили клип, но он лишь близок по смыслу и требует доработки позой или предметом:

- Ремёсла (`knap_blade`, `make_axe`, `make_spear`, `make_needle`, `make_spindle`, `make_digging_stick`): база Iglesias HammeringGround и CMU slicing/chopping, нужен предмет в руке и поза над верстаком.
- Прядение, ткачество, шитьё (`spin_thread`, `weave_cloth`, `sew_*`, `twist_cord`): CMU 79_05/80_04 sewing (стоя); для сидячего варианта соединить с `ual1__sitting_idle_loop` (ноги) по рецепту GAP_ANIMATIONS_HOWTO.
- Готовка (`cook_*`, `brew_remedy`): CMU mixing batter / hand mixing dough, стоя за столом; у костра нужен вариант на корточках.
- Парные сцены (`comfort`, `help_with_task`, `restrain`, `give_food`): CMU 18–23, обе роли A и B в одном дубле (поле `pair` в манифесте), синхронизировать по кадрам и выставить актёров по исходной съёмке.
- Обогрев у огня (`warm_at_*`): CMU cold/freezing (зябнет), а не руки к огню.
- Суд, совет, законы (`judge_case`, `propose_law`, `call_council`, `vote`): общие жесты речи.

## Чего нет ни в одном источнике (делать вручную или процедурно)
- Добивание на коленях (`exec_kneel_stab`, `execute`), связывание рук (`bind_hands`) и освобождение (`release_captive`).
- Перевязка и жгут (`bandage_arm`, `bandage_torso`, `tourniquet_leg`), перенос раненого на плече (`carry_shoulder`).
- Финт глазами (`feint_eye_dart`), протирка клинка (`wipe_blade`).
- Интимная сцена (`intimacy`), парный поцелуй.
- Ковка у наковальни стоя, свежевание туши, помешивание котла у костра (есть только близкие базы).

## Качество ретаргета
- Конечности (плечо, предплечье, бедро, голень) совпадают с источником по направлению; ключицы, кисти, стопы и пальцы переносятся поворотом от покоя без подгонки (у ригов разное строение).
- Таз ставится по нижней лодыжке клипа на высоту покоя UAL (поле `ground_shift_m`); у CMU поправка 3–4 см.
- Пальцы есть у Mixamo и Motifect; у Iglesias и CMU кисть без пальцев (пальцы в позе покоя).
- Перемещение по миру лежит на кости pelvis (`root_motion_m`), кость root неподвижна. Для движка с root motion перенести xy таза в root при импорте.

Клипы с `loop` в имени, но со скачком на шве больше 8°: `igl_crafting__farmingwithplow01_r_loop_f` (21.66°).

## Лицензии
- Quaternius UAL: CC0.
- Mixamo: только внутри игры, не распространять клипы отдельными файлами или паками.
- Kevin Iglesias FREE: коммерческое использование разрешено, сами паки не перепродавать и не раздавать.
- Motifect Daily Life: свободно в проектах, без перепродажи набором.
- CMU: по странице базы свободно для любых целей; перед релизом подтвердить у авторов (решение проекта).
- Поэтому GLB лежат вне git (папка clips в .gitignore) и пересобираются скриптом из исходников в Загрузках.

Неиспользованных в привязках клипов: 748 (остаются в манифесте как запас).

Клипов по пакам: cmu 262, igl_basic 116, igl_crafting 52, igl_dance 26, igl_melee 80, igl_spell 84, mot 31, mx_adventure 22, mx_axe 47, mx_drunk 12, mx_farming 25, mx_gestures 15, mx_great_sword 51, mx_injured 20, mx_longbow 39, mx_magic 56, mx_single 29, mx_sword_shield 49, ual1 42, ual2 42.
