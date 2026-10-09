# Sketchfab: кандидаты (downloadable, CC0 / CC-BY)

Скачать: `SKETCHFAB_TOKEN=... python tools/sketchfab_download.py doc/sketchfab_candidates.json`

## Обработка в стиле фэнтези 1960–80-х

Пайплайн: `tools/prepare_sketchfab.py` → meshoptimizer / Blender → `stylize_textures.py` → отдельная glTF-библиотека `assets/generated/sketchfab_fantasy_6080/`. Настройки категорий и бюджета текстур: `content/config/style/sketchfab_pipeline.json`; живописные пресеты и палитра берутся из существующих `texture_presets.json` и `reference.json`.

Повторить или продолжить обработку из корня проекта, PowerShell:

```powershell
.\.cache\sketchfab-pipeline\Scripts\python.exe tools/prepare_sketchfab.py --workers 3 --optimizer .cache/sketchfab-native-build/bin/RelWithDebInfo/model_simplify.exe
```

Готовые модели с теми же настройками пропускаются автоматически. `--only UID UID` выбирает конкретные модели; `--force` пересчитывает выбранные; `--no-previews` отключает пробные рендеры. Обработка использует только уже скачанное сырьё и не запускает загрузчик Sketchfab.

Результаты: `report.json` — статус каждой модели, треугольники, способ упрощения и ошибки; `before_after.png` — восемь сравнений при одинаковом освещении. В папке каждого ассета сохраняются glTF, текстуры, `source.json`, `license.txt`, метаданные изменений и логи. Масштаб исходника сохраняется; pivot переносится в нижний центр через родительский узел. Метрический масштаб следует проверить при размещении.

Статические поверхности упрощаются с сохранением UV и материалов. Совместимые с native-оптимайзером меши проходят проверку силуэта; модели с дополнительными атрибутами проходят Blender. Риги, анимации и морфы сохраняются. Чистые облака точек отмечаются отдельно: этот проход рассчитан на поверхностные меши. Регистрация в игровом каталоге и runtime LOD/импостеры — отдельный этап после отбора ассетов.

Для `Modular Lowpoly Medieval Environment` (10339 компонентов) в конфигурации сохранено исключение `asset_overrides`: авторская геометрия и раскладка остаются целыми, выполняется обработка материалов и экспорт. Индивидуальное упрощение всех компонентов этого набора превышало лимит времени и не подходит для сохранения его модульности.

Проверенный результат на 2026-10-04: **223 обработанных модели из 228 скачанных**, 5 чистых облаков точек отмечены отдельно, ошибок — 0. Геометрия: **26116576 → 13509269 треугольников (−48.3%)**. Проверены 2072 текстуры (все ≤1024 px), 223 буфера, лицензии, сохранение 27 анимированных и 18 ригованных ассетов, длительности анимаций. Итог проверки — `verification.json`; повторный запуск пропускает готовые результаты.

Превью: [до/после материалов и геометрии](../assets/generated/sketchfab_fantasy_6080/before_after.png), [тестовая сцена с обработанными ассетами](../assets/generated/sketchfab_fantasy_6080/fantasy_scene_preview.png). Сцена показывает обработанные материалы вместе со светом и атмосферой; её композиция не переносится автоматически в игру. Повторить рендер: `blender -b --python tools/blender/preview_sketchfab_fantasy.py`; атрибуции ассетов сцены сохранены в `scene_preview_sources.json`.

Локальное окружение Python: `.cache/sketchfab-pipeline` (NumPy, Pillow, SciPy). Blender: `C:\Program Files\Blender Foundation\Blender 5.2\blender.exe` (путь можно заменить через `--blender`). Отдельная сборка native-оптимайзера лежит в `.cache/sketchfab-native-build`, её CMake-проект и копия исходников meshoptimizer — в `.cache/sketchfab-native-source`; для пересборки: `cmake --build .cache/sketchfab-native-build --target model_simplify --config RelWithDebInfo`.

## architecture

| Модель | Автор | Лиц. | Фейсы | Лайки |
|---|---|---|---|---|
| [Old church modeling - Interior Scene](https://sketchfab.com/3d-models/none-eb6cf543aa7d45e3acee49887ae3135c) | Aurélien Martel | BY | 306300 | 3100 |
| [Cloud Station](https://sketchfab.com/3d-models/none-26f81b24d83441ba88c7e80a52adbaaf) | Alexa Kruckenberg | BY | 18742 | 2391 |
| [stylised sky player home dioroma](https://sketchfab.com/3d-models/none-6597e6c9a5184f07a638ac33c08c2ad5) | Sander Vander Meiren | BY | 23379 | 2249 |
| [Tap](https://sketchfab.com/3d-models/none-9c94f0aca4bf4483800b80b6c035cf0e) | luyssport | BY | 46560 | 1575 |
| [Modular Lowpoly Medieval Environment](https://sketchfab.com/3d-models/none-5bf0a1562b7e401e9e6d7758ec54d09c) | Satendra Saraswat | BY | 562755 | 1467 |
| [Gislinge Viking Boat](https://sketchfab.com/3d-models/none-01098ad7973647a9b558f41d2ebc5193) | Opus Poly | BY | 29035 | 1222 |
| [Lightning Mage (Free Download)](https://sketchfab.com/3d-models/none-f89b591b51cb42c98f84606999e4ec89) | Adipriatna | BY | 86596 | 1059 |
| [The Dark Wraith](https://sketchfab.com/3d-models/none-392342e94ee94c1b823a367a8ddcf9ee) | davidkong | BY | 150293 | 995 |
| [Fantastic castle](https://sketchfab.com/3d-models/none-08a018e4e7b3486dbec7b0341be3aeb4) | lepkarepka | BY | 53724 | 955 |
| [Tol](https://sketchfab.com/3d-models/none-fd7c646f80da45c786ad6e42a2a66412) | luyssport | BY | 34883 | 897 |
| [Issum, The town on Capital Isle](https://sketchfab.com/3d-models/none-e433923a64d549fabb2d30635d643ab6) | Olee | BY | 193968 | 866 |
| [Armored Horse](https://sketchfab.com/3d-models/none-98c3f1c40a6b422dba76bf5403e0a3d8) | naminoff | BY | 44008 | 820 |
| [Village / Town Assets](https://sketchfab.com/3d-models/none-adf2e5a787804ffcaaa1ad6d541278d5) | imwolfgang | BY | 473769 | 764 |
| [Medieval Village](https://sketchfab.com/3d-models/none-0e7dd1fd2cd64f828b625021e30704d4) | iedalton | BY | 68146 | 711 |
| [Medieval Houses (3 Pack)](https://sketchfab.com/3d-models/none-38443304774c481e9498b3e810389e91) | Ajay Gawde | BY | 32351 | 689 |
| [Desert Western Village Town Modular Wall Pieces](https://sketchfab.com/3d-models/none-7dd263a780454eb4b91a2de0a4d6cf98) | Marcel_022 | BY | 288900 | 685 |
| [Medieval house | Generic Textures | Game ready](https://sketchfab.com/3d-models/none-3eb9e3b600264e2f99100fc43619497e) | by__Rx | BY | 8124 | 684 |
| [Twa](https://sketchfab.com/3d-models/none-52b771bb17c54ec985981e5ab119f5af) | luyssport | BY | 2824 | 669 |
| [Medieval house](https://sketchfab.com/3d-models/none-4ec56df1c24d422ea85d1cfbf21bbe8c) | Young_Wizard | BY | 1504 | 655 |
| [Medieval Tavern (2k vertices) - free game asset](https://sketchfab.com/3d-models/none-a06dfdc919334bfc95de720ad98f4295) | Andy Woodhead | BY | 3850 | 651 |
| [Medieval House 3](https://sketchfab.com/3d-models/none-cf179aa6dc0944f1974ce3f7812031a6) | Animau3D | BY | 697 | 644 |
| [Medieval Castle](https://sketchfab.com/3d-models/none-2964776c3847493ba908fd192e071ca3) | Uğur Yakışık | BY | 64014 | 629 |
| [Medieval House 1](https://sketchfab.com/3d-models/none-0bbda345359349ea95280f597c8a4bd4) | Animau3D | BY | 691 | 616 |
| [Castle village scene](https://sketchfab.com/3d-models/none-675b3afce21a4acaa1140645a5206db3) | BlackSpire | BY | 55058 | 610 |
| [Medieval house](https://sketchfab.com/3d-models/none-904038d693b945d4843decee87fb0eb5) | Vanuartw | BY | 83052 | 606 |
| [Low poly environment](https://sketchfab.com/3d-models/none-2e18c1baa9164093ad2e99e0a904363a) | BigPo | BY | 29369 | 588 |
| [WIP Fantasy Castle](https://sketchfab.com/3d-models/none-367debf3eef749d985109125c8c3da34) | Thomas Andris | BY | 320799 | 525 |
| [Silent Hill 3 cathedral](https://sketchfab.com/3d-models/none-032d086b265c492eac3f92eb427cb60e) | Katydid | BY | 39818 | 525 |
| [Door](https://sketchfab.com/3d-models/none-845565caed7644148b45d88c4e8987bb) | Rud_H | BY | 33130 | 507 |
| [Pack of old towers in ruins](https://sketchfab.com/3d-models/none-f213359c6cbb4c29bf8880764faa0fb8) | JB3D | BY | 73472 | 502 |
| [Medieval Watchtower House](https://sketchfab.com/3d-models/none-4099fd90094c400e987deb240078c38e) | Krystian Zem | BY | 10544 | 492 |
| [Desert Fortress Structures](https://sketchfab.com/3d-models/none-4c3c32e8319d4b87ab9bbf3fafeb8fb5) | Nicolai Kilstrup | BY | 87369 | 462 |
| [Windmill Stylized](https://sketchfab.com/3d-models/none-a7780e3667954bbf97bb619cb8a3e272) | _Alexandr | BY | 4321 | 457 |
| [little house (game ready)](https://sketchfab.com/3d-models/none-4911387d1ca74fce83e05722cb52129e) | XiaolingPan (Célinepan) | BY | 8498 | 434 |
| [Gothic City Palace - "Het Markiezenhof"](https://sketchfab.com/3d-models/none-7fd0bc0039fa40589dcc119c69000103) | 3DBuilder | BY | 8783 | 427 |
| [Jester](https://sketchfab.com/3d-models/none-cf3f5ac98ee049e78537cc45776f611a) | chol | BY | 5542 | 412 |
| [Low Poly Farm](https://sketchfab.com/3d-models/none-879d61d8dfc048548ee380cace6f79d3) | EdwiixGG | BY | 38000 | 370 |
| [War Damaged  Church](https://sketchfab.com/3d-models/none-c82833ce9bb644d9ae3ee5e1c8db7297) | local.yany | BY | 234391 | 360 |
| [Haunted Parish PS1/PS2 Environment | McPato](https://sketchfab.com/3d-models/none-37367bbd329d46669e95b42207972e00) | McPato | BY | 29606 | 351 |
| [Windmill near Water](https://sketchfab.com/3d-models/none-11523012f4424ff9acc9b9a3a7d46603) | Mr. The Rich | BY | 54990 | 340 |
| [Windmill](https://sketchfab.com/3d-models/none-0730705327e045bd8cb98a888bd0f954) | Yury Misiyuk | BY | 2947 | 334 |
| [Medieval Multi-Story Arch Bridge](https://sketchfab.com/3d-models/none-42befed15e8343488b3f3f127a6c9a5d) | Sereib | BY | 346984 | 332 |
| [Medieval mill](https://sketchfab.com/3d-models/none-080f516a0b6b402092c1314913341db8) | eexaw | BY | 11068 | 332 |
| [Windmill in Halikko, Kreivinmäki, Finland](https://sketchfab.com/3d-models/none-2a43c8bd492d4d11ad73e1d3f7ccb58a) | Lassi Kaukonen | BY | 572790 | 316 |
| [DYSON RINGS](https://sketchfab.com/3d-models/none-781280bf2a3f4e8b923220106832df10) | Portal.Studio | BY | 56808 | 314 |
| [Temple ruin aquarium decoration - Photoscan](https://sketchfab.com/3d-models/none-fad156666c33460a9074ceaa6ed6c93e) | DonCoasn | BY | 99992 | 312 |
| [Witch Tower and Lil Witch Boy](https://sketchfab.com/3d-models/none-aa80ea44899d402986c7c9148721bad5) | tinybaer | BY | 57228 | 306 |
| [Robin Hood in Sherwood Forest](https://sketchfab.com/3d-models/none-63dd568f5bc64e8694d5f4252924c99e) | lisa_shche | BY | 36022 | 290 |
| [Fortress Island](https://sketchfab.com/3d-models/none-56878754ffb24489b769dd3348d4d92f) | Laetitia Irata | BY | 89857 | 284 |
| [PSX Abandoned Church](https://sketchfab.com/3d-models/none-4947e2994961439a914550a436b7d9b0) | Shazly | BY | 458 | 266 |
| [Ruin Ancient Temple - Khmer Architecture A 05](https://sketchfab.com/3d-models/none-26378463180d4dc1848dc0d758850d66) | Mega 3D | BY | 5000 | 243 |
| [Church](https://sketchfab.com/3d-models/none-20595bb2a0ab4572bbe398514d55a39e) | local.yany | BY | 56618 | 225 |
| [Church Asset Pack](https://sketchfab.com/3d-models/none-e460517379e44313bf4a81817881cf11) | Caboose3d | BY | 3354 | 191 |
| [Fantasy eco city](https://sketchfab.com/3d-models/none-d6929fd7b2f641b985c9afd395e94f0f) | Skodvirr | BY | 551024 | 190 |
| [Old Ruined Temple](https://sketchfab.com/3d-models/none-bb2b2cc8db1f4fb49917b0eb8cb83977) | Muhammet Gündel | BY | 20026 | 183 |
| [Two Medieval Towers and an Arch Bridge](https://sketchfab.com/3d-models/none-34b6ec56138540ae9a88d3b1462156cc) | Sereib | BY | 273684 | 183 |
| [Arabic temple diorama](https://sketchfab.com/3d-models/none-01941ce469b041138a1c9a565e07b19a) | maheylah | BY | 7292 | 161 |
| [Security/Parking Booth - Game Asset](https://sketchfab.com/3d-models/none-4f557163e6ad4a63ad12c9182ceba0be) | AspectStudios | BY | 1998 | 126 |
| [Lowpoly Medieval Game Assets Set](https://sketchfab.com/3d-models/none-b6de2aa3b6c1490692f8fccfefcb25f7) | 3Dimentional | BY | 12611 | 116 |
| [CC0 ジャコウアゲハ 🦋 ♂ Chinese Windmill, B. alcinous](https://sketchfab.com/3d-models/none-86b0c5e135f64d139dd8b9f082e8cc2b) | ffish.asia / floraZia.com | CC0 | 270210 | 115 |
| [Fantasy City Buldings Street Parts Pack](https://sketchfab.com/3d-models/none-4b6cb28bd71d4698ab475d8665863b86) | Scol3D | BY | 89276 | 115 |
| [Temple 3D model](https://sketchfab.com/3d-models/none-12cc7b8392b441999aaf561849be0d8e) | Marco Zakaria | BY | 16696 | 114 |
| [Mossy Stone Arch Scene | Low-Poly](https://sketchfab.com/3d-models/none-363750ab46b34b579041fe90fb350e53) | kEam | BY | 18552 | 110 |
| [EPSILON TOWER](https://sketchfab.com/3d-models/none-005e694c69a84fb886323242c34069c8) | Portal.Studio | BY | 52823 | 102 |
| [Medieval Stone Bridge](https://sketchfab.com/3d-models/none-cddc91589bad41e3a26f9d5467acfa14) | Sereib | BY | 374400 | 96 |
| [Medieval bridge](https://sketchfab.com/3d-models/none-565a1cd4930d4560947b0e539cf6fed5) | Andrei CG | BY | 9030 | 85 |
| [Metal Gates](https://sketchfab.com/3d-models/none-1f1231cd53b24f6eb4762d750b3c8654) | Gunnar Correa | BY | 2092 | 73 |
| [Mausoleum stylized](https://sketchfab.com/3d-models/none-5d7521e81fd149ba9ecd6d241849b2a6) | Arhokis | BY | 1260 | 73 |
| [Modular Apocalyptic Water Settlement](https://sketchfab.com/3d-models/none-83f61cfa875a4c2eb78b485f98f14e9c) | emerson.davy | BY | 59404 | 70 |
| [Miniature High Fantasy City - preview version](https://sketchfab.com/3d-models/none-b7012b13c996403181891e757a23a85c) | Kai Xiang | BY | 157608 | 69 |
| [Ancient Fortress Asset Pack - Part 2](https://sketchfab.com/3d-models/none-43cab28dea5a47f69f8e30640cb9dd50) | EqualizerX | BY | 116775 | 68 |
| [Fantasy City Building 03](https://sketchfab.com/3d-models/none-f90e6771b5534f0490fb938bf527ff1b) | Scol3D | BY | 19868 | 68 |
| [STATION - A12](https://sketchfab.com/3d-models/none-1f511e5482bf481191544ebc132696bb) | Portal.Studio | BY | 4864 | 65 |
| [Renvylle Castle](https://sketchfab.com/3d-models/none-f08747faa13348d99a7f5bd2a824f2c2) | Galway3D_DH_AGe | CC0 | 362673 | 64 |
| [Ruin](https://sketchfab.com/3d-models/none-c64426b976a545e1b2ea2349f881babd) | Chloe Boswell | BY | 12816 | 63 |
| [Fantasy Room](https://sketchfab.com/3d-models/none-9882ab8a09614a9a97650e642fc10254) | local.yany | BY | 500850 | 63 |
| [Gate](https://sketchfab.com/3d-models/none-4aa24c937438468aa42c94f95a2b4e2b) | Bongblender | BY | 44872 | 57 |
| [Remains of the chapter-house ceiling](https://sketchfab.com/3d-models/none-ebc4f179cacd4f7290a2f1a5a3192d06) | Virtual Museums of Małopolska | CC0 | 30120 | 49 |
| [Galway City - Spanish Arch](https://sketchfab.com/3d-models/none-6f5b10dbb7744ee1a68564fe80055630) | Galway3D_DH_AGe | CC0 | 258389 | 44 |
| [Horse Aquamanile](https://sketchfab.com/3d-models/none-4bdfa94c45de45c88d9c15bf02adf4b8) | The Hunt Museum | CC0 | 367040 | 38 |
| [Small Grave, 4K PBR textures](https://sketchfab.com/3d-models/none-58408086fa3a422082d7200d823be4f2) | shamanek | BY | 30264 | 37 |
| [Grave Coffin, 4K PBR textures](https://sketchfab.com/3d-models/none-d12a82266c594cd59274a452768b3c2b) | shamanek | BY | 239734 | 35 |
| [CC0 ジャコウアゲハ 🦋 Chinese Windmill, Byasa alcinous](https://sketchfab.com/3d-models/none-c9b3d99fc1764e6cb4312b22eeb19a4e) | ffish.asia / floraZia.com | CC0 | 304558 | 34 |
| [Mausoleum stylized](https://sketchfab.com/3d-models/none-25b4e9dae56a46e6b528b849dcd2d0d7) | Arhokis | BY | 766 | 31 |
| [Realistic Rock Wall Structure](https://sketchfab.com/3d-models/none-a07119813c404350bd03db5c570271b8) | mohamedhussien | BY | 40000 | 23 |
| [Petra Treasure ¦ Al-Khazneh Photogrammétrie test](https://sketchfab.com/3d-models/none-3bd9120ff8854d6292a55892a4565269) | ThomasNicodeme Studio design 3D | BY | 200932 | 23 |
| [Stylized Cliff Rock Formation](https://sketchfab.com/3d-models/none-5df9d98eb6f14d2793d061126b242a9f) | mohamedhussien | BY | 40000 | 19 |
| [Half temple of Apollon (Portara)](https://sketchfab.com/3d-models/none-0d54b70cf8c842aaa7f7d6ac41d56cff) | Georgios Tsampounaris | BY | 334813 | 19 |
| [Mystic Forest Portal](https://sketchfab.com/3d-models/none-0dcbd6f7e1f14dd6bc82babe3260289d) | ARTEL_3D | BY | 499982 | 18 |
| [IndianMAN Geometre-100](https://sketchfab.com/3d-models/none-d2c762a2325547c5b30e51bdfd736188) | PatrickBiani | BY | 424869 | 18 |
| [Castillo de Montroi / Montroi Castle](https://sketchfab.com/3d-models/none-b91f93d6d4fd431eb4310038dca4b56c) | Riberana.es | CC0 | 560370 | 17 |
| [ancient arch 3d model](https://sketchfab.com/3d-models/none-828099acb95b4e1b9889034c60652df0) | grodan | BY | 16101 | 15 |
| [8 Hour Art Test](https://sketchfab.com/3d-models/none-8fecc3c682804f749d4e8ae9f4d6a49d) | Foolish Me Studios | BY | 27832 | 12 |
| [Vaulted Cloister Ruin Detail](https://sketchfab.com/3d-models/none-f7e507f0b0654579accf5dbf05745f7d) | mohamedhussien | BY | 39998 | 9 |
| [Browne Doorway](https://sketchfab.com/3d-models/none-33abdc420ce9411d99852e10bd52781b) | Galway3D_DH_AGe | CC0 | 421137 | 9 |
| [Stone-Arch-{✖}-'05'](https://sketchfab.com/3d-models/none-331513f2896c46cf9715c265f85bbe93) | -X-ScornGames | BY | 39929 | 8 |
| [Ancient Portal](https://sketchfab.com/3d-models/none-c8fd3c64c13e446d8acba7126fb29862) | DreamPolys | BY | 82231 | 8 |
| [Medieval Firepalce - Galway City](https://sketchfab.com/3d-models/none-3edbe61c816b4acca6913fb404639cf5) | Galway3D_DH_AGe | CC0 | 16156 | 7 |
| [Stylised cemetery](https://sketchfab.com/3d-models/none-0edb70136fbc47f9992761ffebaf66d7) | Piotr Pisiak | BY | 100100 | 7 |
| [Church of San Cataldo - Montenero](https://sketchfab.com/3d-models/none-97b9f0ae0b674e53b1a3802f0431af70) | Dimitar Ignatov | BY | 2340 | 6 |
| [monolith](https://sketchfab.com/3d-models/none-47ea90af1f9c4893b42f06cb790857f4) | barbodoji | BY | 125939 | 5 |
| [Baptisimal Font - Aughrim](https://sketchfab.com/3d-models/none-2dd9bc8172c6461797947cc7214db399) | Galway3D_DH_AGe | CC0 | 200000 | 4 |
| [Romanesque Column and Vault Ruin](https://sketchfab.com/3d-models/none-38a80966010b41b4b0ab52d39a433c5a) | mohamedhussien | BY | 39999 | 3 |
| [Critical Depth(1997): Portal Gate](https://sketchfab.com/3d-models/none-bc707a06f2d843bdb199055e69142ba7) | Zorg_Sinister | BY | 232 | 3 |
| [The Enchanted Portal](https://sketchfab.com/3d-models/none-e9d1f50ea2d4483a98e64a0b69ff82bd) | DreamPolys | BY | 97399 | 3 |
| [Belchrot - monsters of rock](https://sketchfab.com/3d-models/none-e913203590344d90b28803ced3cbe80e) | Brian Trepanier | BY | 403940 | 2 |
| [Ancient-{X}-Crypt-II](https://sketchfab.com/3d-models/none-bec550a500b941ab9806db909f19e3a7) | -X-ScornGames | BY | 142234 | 1 |

## surreal_landscape

| Модель | Автор | Лиц. | Фейсы | Лайки |
|---|---|---|---|---|
| [Lowpoly Rocks - Free Download](https://sketchfab.com/3d-models/none-c5da5675001f4bec9ecfbc8343453417) | Loïc Norgeot | BY | 12599 | 1764 |
| [Earthquakes - 2000 to 2019](https://sketchfab.com/3d-models/none-894ad84ceb8b444a91fbc05f20530bcd) | Loïc Norgeot | BY | 0 | 1669 |
| [Mushrooms](https://sketchfab.com/3d-models/none-a2552cfb13f84695b0d470bd5f19d559) | shindiego.ds | BY | 27081 | 756 |
| [Floating Island of the Potion Brewer](https://sketchfab.com/3d-models/none-555992efd5c74cf299f5a3ab4acd0a3b) | vilmariina | BY | 55545 | 705 |
| [Dead Trees Demo](https://sketchfab.com/3d-models/none-9bdd36641ef541adb51f0d01ddb07b2c) | Quadra3D | BY | 232924 | 666 |
| [Coniferous Forest Assets Pack](https://sketchfab.com/3d-models/none-240ce9cf0ad3474f809a59f9446e63cd) | Dari | BY | 58390 | 657 |
| [Various Forest Assets Pack](https://sketchfab.com/3d-models/none-cf138b8eb2d340cda643ed59f824989c) | Dari | BY | 130495 | 635 |
| [volcano](https://sketchfab.com/3d-models/none-136292fd63fc43a5b446d868fcaa7751) | gelmi.com.br | BY | 157203 | 557 |
| [HEDGEHOG ISLAND LABOLATORY Fan Art](https://sketchfab.com/3d-models/none-a036aa74dd234c07a3a2af19cff18dc6) | polish_farmer/ Eryk Stańko | BY | 66664 | 541 |
| [Icescape 1 Alien Moon Skybox](https://sketchfab.com/3d-models/none-bfc9a041814f4112b016904edfaad0c5) | Luis Vidal | BY | 960 | 528 |
| [city](https://sketchfab.com/3d-models/none-a694ac17a90a45f89f6b588de65fadc6) | abhayexe | BY | 409394 | 521 |
| [Old tree](https://sketchfab.com/3d-models/none-3cb4d59eb4844dc4802480e9ee53785e) | evolveduk | BY | 8107 | 457 |
| [Mt. Etna - Lidar survey](https://sketchfab.com/3d-models/none-b6f4551ffd3d4cab922234d10f69585a) | Loïc Norgeot | BY | 0 | 419 |
| [Star Wars - Mustafar Control Room](https://sketchfab.com/3d-models/none-1e5f5e8a5bc546e1a778fc94fb452bda) | lazae | BY | 135961 | 416 |
| [Mars - Olympus Mons](https://sketchfab.com/3d-models/none-53679864b41a43b0ae0459ce99d6adf0) | SebastianSosnowski | BY | 10662 | 412 |
| [Cabin On The Edge](https://sketchfab.com/3d-models/none-f37c87c9bf2d417d927f8859e057eacb) | Keppu | BY | 6144 | 404 |
| [Earthquakes - 2010 & 2011](https://sketchfab.com/3d-models/none-3a57cfbbcb1c45278c13bc66886dd6ee) | Loïc Norgeot | BY | 0 | 376 |
| [Interdimensional Floating Islands](https://sketchfab.com/3d-models/none-0742e636aa9a40b5865436511e3595cf) | Iso Game Lab | BY | 49195 | 369 |
| [Alien Terrain 05 HDRI](https://sketchfab.com/3d-models/none-7ba0da88576d4f4592beaaa0e9e3ff2d) | Giimann | BY | 4000 | 355 |
| [3 English Oak Set (Quercus Robur) *Updated](https://sketchfab.com/3d-models/none-750fac732a8c453e9dd748f1396275fa) | Yag1z | BY | 28379 | 340 |
| [Forest Skull](https://sketchfab.com/3d-models/none-ce4d33f9b9a3440f97aeec7ed9293b79) | FletchTech | BY | 94482 | 339 |
| [Mount Fuji Volcano - Japan](https://sketchfab.com/3d-models/none-bb83a647e5a8481d85a4bb33119d40a5) | Nate Loper  🗺️ ⛏🏺 | BY | 8192 | 333 |
| [Tōhoku earthquake - 2011](https://sketchfab.com/3d-models/none-bf7bac2c40d7417c8af31b1cce8a8a47) | Loïc Norgeot | BY | 196394 | 317 |
| [Stylized Tree](https://sketchfab.com/3d-models/none-2a00182b3ca4403da3f9e157184e4b42) | Daniel | BY | 166151 | 291 |
| [PSX Dead Tree Pack](https://sketchfab.com/3d-models/none-40510061c9814223b204f03941e4c589) | FlunkedPunk | BY | 1244 | 291 |
| [Volcano Island Lowpoly](https://sketchfab.com/3d-models/none-4a6591dc9fee40d8bfda8350683af9af) | Animateria | BY | 16406 | 283 |
| [World of metal](https://sketchfab.com/3d-models/none-577de9908be448a1bbe0a627f2b84099) | dark_igorek | BY | 573838 | 279 |
| [Mount Fuji Volcano Wide Area Model - Japan](https://sketchfab.com/3d-models/none-2bc48894d5c0496eb83d7b0eaf529499) | Nate Loper  🗺️ ⛏🏺 | BY | 8192 | 275 |
| [Snow Pine Tree and Bush Pack LOWPOLY](https://sketchfab.com/3d-models/none-7afc4ca23dd84adea90dc0ba8506c500) | EFX | BY | 1246 | 268 |
| [Hopi Buttes Volcano - Bidahochi Basin, Arizona](https://sketchfab.com/3d-models/none-1f6a01cc82a4464883c05e86d2fcd04f) | Nate Loper  🗺️ ⛏🏺 | BY | 8192 | 243 |
| [Stylized Mini Floating Island](https://sketchfab.com/3d-models/none-48ef6c69479541d3b4aed13ff7290ff8) | Tom Verbeeck | BY | 173115 | 237 |
| [Lush Alien landscape skybox](https://sketchfab.com/3d-models/none-7c2260faca5d473d9ccbe44319c73551) | Jungle Jim | BY | 560636 | 221 |
| [Floating Island Map](https://sketchfab.com/3d-models/none-0bb28f9be4214de28719e083944bb06e) | Bucks | BY | 65409 | 217 |
| [Dead Tree - PS1 Low Poly](https://sketchfab.com/3d-models/none-de2611ba107f4c3c8c89bf516d5d9b6b) | Wersaus33 | BY | 294 | 213 |
| [Hobbit House - Low poly](https://sketchfab.com/3d-models/none-5ce32873e252416aa7164aba2d84298b) | TST1818 | BY | 74120 | 196 |
| [Medieval Fantasy Tavern](https://sketchfab.com/3d-models/none-eea5d10fea01404587a1b5703784bb0b) | 3DBuilder | BY | 80622 | 196 |
| [Ice Cluster (free)](https://sketchfab.com/3d-models/none-4d2271f8bf7f400e9a5c8f10812a32de) | chrismartin1337 | BY | 1886 | 193 |
| [Low Poly Flying Island](https://sketchfab.com/3d-models/none-49c22c7d4f3249688a000fc526b84a76) | Mohamed Fsili | BY | 12799 | 185 |
| [FLOATING ISLAND](https://sketchfab.com/3d-models/none-cde28313f39c44dea06eb11c701c7837) | Viky_3D | BY | 60553 | 172 |
| [Low Poly Tree with twisting branches](https://sketchfab.com/3d-models/none-4e2589134f2442bcbdab51c1f306cd58) | purepoly | BY | 1126 | 165 |
| [Olive tree 2](https://sketchfab.com/3d-models/none-cde7b00c8b0e41b39b71842a2ed4a78b) | b_nealie | BY | 395339 | 142 |
| [Icescape 3 Alien Moon Skybox](https://sketchfab.com/3d-models/none-9a4001a45e334fe196c98463785b1ab7) | Luis Vidal | BY | 960 | 140 |
| [Canyon Cliff Face Low Poly](https://sketchfab.com/3d-models/none-df10be37cc064c6882c25dcc74a261f2) | TastefullBrick | BY | 524887 | 138 |
| [Alien: planet LV-426](https://sketchfab.com/3d-models/none-c564e88ae2314081946dacd85cf71db0) | Ignazio Pillitteri | BY | 190470 | 137 |
| [Tree Root Tentacle](https://sketchfab.com/3d-models/none-bf044450f89f409d973e9e8c770f51ee) | Alice Newhouse | BY | 133954 | 137 |
| [Ice Cluster2 (free)](https://sketchfab.com/3d-models/none-eb83bbed188e47bf9910b08e41e2956b) | chrismartin1337 | BY | 4417 | 130 |
| [XR Offal HYPEROBJEKT I / II Point Cloud](https://sketchfab.com/3d-models/none-3073c953606f41e2ac338c0100b20f2b) | MWintersberger | BY | 0 | 128 |
| [Azure Window - 3D scan before the collapse, 2016](https://sketchfab.com/3d-models/none-d1a80d182db148d3989e440244378b64) | Patrick | BY | 163970 | 122 |
| [Stalagmite Formation 1](https://sketchfab.com/3d-models/none-da1f7a9e2fe04549a03d479d938b3088) | Kallvin | BY | 3124 | 118 |
| [Cistell conillera, MEMGA](https://sketchfab.com/3d-models/none-3bc29ed214494478a493d55c941d6f43) | Giravolt | BY | 249741 | 117 |
| [Skybox Fantasy | View 2](https://sketchfab.com/3d-models/none-1a314b40bc7c448eb8b95282a1480631) | EdwS | BY | 3480 | 107 |
| [Landscape of an alien planet (Skybox)](https://sketchfab.com/3d-models/none-676c023d919344efbfa9876c85db7d11) | Jungle Jim | BY | 960 | 105 |
| [Tropical Modernity in Philippines](https://sketchfab.com/3d-models/none-9386e67c79c04e728a797a15ce237940) | arjamesart | BY | 24518 | 103 |
| [Geochelone carbonaria](https://sketchfab.com/3d-models/none-0bc47e68d77243efa50a22b881970626) | The Smithsonian Institution | CC0 | 99992 | 92 |
| [Arrival Transmutation at a Distance Point Cloud](https://sketchfab.com/3d-models/none-5e2bffa0ac2e463da2257ac0637814e5) | MWintersberger | BY | 0 | 84 |
| [Hengifoss waterfall](https://sketchfab.com/3d-models/none-f522842a45114d208361423730607c74) | matousekfoto | BY | 249492 | 83 |
| [Stalagmite Formation 2](https://sketchfab.com/3d-models/none-736323ad07334c49be26da6f44f0c609) | Kallvin | BY | 2570 | 77 |
| [Twisted birch](https://sketchfab.com/3d-models/none-6fdbf160d2ee4e058da7b16d4443f33c) | John Fino (tauricity) | BY | 104693 | 72 |
| [GA Free_351_Aetheris Feathertree Variant](https://sketchfab.com/3d-models/none-db7b28b1942e40d2bb85735183aa2219) | Galaxy Abundant | BY | 118394 | 71 |
| [Mystic stones of the Sky](https://sketchfab.com/3d-models/none-a3f798c9f9b74f16a1a700233400329a) | MadSpace | BY | 35508 | 67 |
| [Lowpoly Forest](https://sketchfab.com/3d-models/none-7a249aba33484437b385de17a228a814) | Jassi | BY | 10592 | 67 |
| [Dark Winged Demon Grim Reaper with Giant Scythe](https://sketchfab.com/3d-models/none-f72b0ce6dd374d68b3b44d02891fc354) | Pigcraft | BY | 35112 | 62 |
| [The Being of Forgotten Caverns](https://sketchfab.com/3d-models/none-27b62d17c2b24296aa5a6d9550622876) | dgregg | BY | 12711 | 54 |
| [GD53_AndreCoelho_MOD2_A3](https://sketchfab.com/3d-models/none-b42079534cd34778be211a818d7d7952) | alcbcoelho | BY | 17060 | 52 |
| [Crystal Wings (simple)](https://sketchfab.com/3d-models/none-5b437c0793f94aa2bf615f905946f65a) | kand8998 | BY | 240 | 52 |
| [Tree Golem](https://sketchfab.com/3d-models/none-c6ef3afc14914f039e4a8b72d10f3d46) | patromes | BY | 19845 | 50 |
| [MOMUS Park - CC0 Asset Pack](https://sketchfab.com/3d-models/none-5b40d57eed3741e598a8cc1e695289a0) | Polygonal Mind | BY | 110691 | 47 |
| [Crystal Cluster 2 (Free)](https://sketchfab.com/3d-models/none-b1bed3164820477894d4f76307a26a13) | chrismartin1337 | BY | 36746 | 46 |
| [Sunset Point - Bryce Canyon National Park, Utah](https://sketchfab.com/3d-models/none-c754bfb7473d40a1aaf15de1d9147d5e) | Nate Loper  🗺️ ⛏🏺 | BY | 8192 | 44 |
| [Crystal Cluster (Free)](https://sketchfab.com/3d-models/none-06d912aa58774a498ac95c90d5a2b614) | chrismartin1337 | BY | 40000 | 44 |
| [Pride Crystals](https://sketchfab.com/3d-models/none-6698444253e14a6f917ba1eb5a7124be) | Jazz Vincent | BY | 1574 | 43 |
| [DAE Bazaar Retake - Fish Shop](https://sketchfab.com/3d-models/none-fd94697f17c443759d20475c45409803) | Lander Vandermeeren | BY | 113533 | 38 |
| [Polygonal Alien Cave Nature Asset Package](https://sketchfab.com/3d-models/none-db7fe0becd974b158ff0b8a3013b7c69) | Aligned Games | BY | 99006 | 35 |
| [Stalacmite](https://sketchfab.com/3d-models/none-a2d7b14b15bf43918d0a9e93d4b4e64e) | Connor Hickey | BY | 171840 | 31 |
| [Natural Blob Rock Crystal](https://sketchfab.com/3d-models/none-46e8e2b59e674360871c9373a8b633ce) | riach | BY | 220876 | 27 |
| [Soberanes Canyon Trail, Ca - Lookout bench](https://sketchfab.com/3d-models/none-b738102fea9746fab02f201c34794c0d) | David Antalek | BY | 500192 | 26 |
| [Sand Hollow Spire](https://sketchfab.com/3d-models/none-11351b04aa0e445d9bd8cf9602b631ee) | GoochFauxRock | BY | 559665 | 24 |
| [Rock quartz formations](https://sketchfab.com/3d-models/none-50fbb4c8918e495691467c627749b1dd) | stuff scanner | BY | 526698 | 23 |
| [Monster Skull](https://sketchfab.com/3d-models/none-9359a801683e47bd9daabfd1a5f5c3a2) | FDU_oficial | BY | 51696 | 17 |
| [Cliff Village](https://sketchfab.com/3d-models/none-06c440f179ea499eae01a53ff90c95d4) | nico_hartl | BY | 452431 | 17 |
| [Kangaroo Skull - COMMENT AND LIKE IF USED THANKS](https://sketchfab.com/3d-models/none-557b96f731c747dfb89294ecdfef375d) | 3Danny | BY | 68969 | 15 |
| [Dinosaur Skeleton - Printable](https://sketchfab.com/3d-models/none-9803c283d60e4901b2da128e4e3fd5f8) | Ed | BY | 73928 | 14 |
| [Igneous rock: Basalt (A'a Lava Rock)](https://sketchfab.com/3d-models/none-cbbe1e1ae71b4e44acc6bf1b5245f1bb) | Digital Atlas of Ancient Life | CC0 | 363958 | 12 |
| [Super Mario - Mega Mushroom](https://sketchfab.com/3d-models/none-bde1aa32d5f047bd925c1893a8b0dec2) | kingston.shamily | BY | 1972 | 11 |
| [Skybox - Waterfall - Cliff - Skeleton Bridge](https://sketchfab.com/3d-models/none-8b65ce7ec41c4b3aa1cca5db4340f00c) | dannzjs | BY | 3968 | 11 |
| [Natural Rock Cliff Wall](https://sketchfab.com/3d-models/none-c544f4bfb77541988398f3ac6a5378f2) | mohamedhussien | BY | 39998 | 9 |
| [Ruined Cathedral Arch Interior](https://sketchfab.com/3d-models/none-03a85ab3c9614386addbf798a9075ee2) | mohamedhussien | BY | 39998 | 8 |
| [Cova del Tabac, pintures rupestres (Dstretch)](https://sketchfab.com/3d-models/none-6c240cbb1d7a4fe5a32a867e3038ef3d) | Giravolt | BY | 547219 | 8 |
| [Orirock Concentration](https://sketchfab.com/3d-models/none-c68c3d9d06d24851b9497412e30a31e9) | NoodleMania | BY | 62 | 8 |
| [Simple cave props (stalagmite variants)](https://sketchfab.com/3d-models/none-a8ca228ba7f2402e9557caa925b2e90f) | Satik64 | BY | 55616 | 8 |
| [West Maui Hawaii - Terrain Map](https://sketchfab.com/3d-models/none-6dd4c8db21f54108ae90f69edc30f553) | ArtIsInFormation | BY | 71247 | 8 |
| [Animated Violet Boom Conjures Floating Cube](https://sketchfab.com/3d-models/none-380ee68f7b1441e7862c0fd1e0767f68) | LasquetiSpice | BY | 109098 | 6 |
| [Stalagmite](https://sketchfab.com/3d-models/none-976ebf9b96ea4fdaa83746eed3272513) | DaniEve | BY | 4300 | 5 |
| [Spéléothème - Stalagtite en coupe](https://sketchfab.com/3d-models/none-ed9f281af08449eaa5feea2d3d32ff89) | Ecole Nationale Supérieure de Géologie | BY | 219556 | 5 |
| [Create a whimsical mushroom](https://sketchfab.com/3d-models/none-0554ff8df758478cb9493384d02f2abc) | Goveta | BY | 10702 | 4 |
| [Voxel climber standing in a cave](https://sketchfab.com/3d-models/none-40cf2c8c091d493a9053533d35265ce8) | martinedstrom | BY | 44424 | 4 |
| [Igenous rock: Basalt (Pahoehoe lava rock)](https://sketchfab.com/3d-models/none-9efcfa23b69240c699b9ee0bd6be6e05) | Digital Atlas of Ancient Life | CC0 | 310534 | 4 |
| [['-X-']═Rock_Arch═['Southside']](https://sketchfab.com/3d-models/none-eeb2888f84c5432b83cbf4c3333481ff) | -X-ScornGames | BY | 26734 | 3 |
| [Treehouse Giant tree](https://sketchfab.com/3d-models/none-85284749793d49168feeca86ca740953) | Pascal_vd_Veld | BY | 14785 | 3 |
| [Giant Mushroom Idle](https://sketchfab.com/3d-models/none-c4362fe272e14ac19881deb908c6dc63) | kirbinator505_ | BY | 4323 | 3 |
| [Pivot door, barn door, panel- SR Dev Canyon Mesa](https://sketchfab.com/3d-models/none-e03e705ba2934434acc8636dc3d03ccc) | ryanpendarvis | BY | 27346 | 3 |
| [The Firbolg Wild- Shaman-female](https://sketchfab.com/3d-models/none-e3e4eb92bb814c5b954a22f8354fa48f) | milakpro | BY | 8455 | 2 |
| [The Firbolg Wild- Shaman](https://sketchfab.com/3d-models/none-f7a30de4d8d348ef8f3b6b828cfd67ac) | milakpro | BY | 5133 | 2 |
| [Giant Mushroom](https://sketchfab.com/3d-models/none-3e6e0cbced27476ab5b9a4272cd304f0) | np-dev | BY | 267118 | 2 |
| [Mouse Wizard         A digital painting of a mo](https://sketchfab.com/3d-models/none-eb8b6a849e1d41cfa624f5190a07d724) | klrxyz | BY | 27758 | 1 |
| [Wizard Tower Diorama | 3D Modeling II](https://sketchfab.com/3d-models/none-c08de08209d74eb1a95c3a5a95a9267a) | LoganPomper | BY | 58878 | 1 |
| [5MT7405 Red-slipped Corrugated Sherd](https://sketchfab.com/3d-models/none-32a1f4d123ac4b289f8522e07ec3fc4c) | Canyons of the Ancients National Monument | BY | 75000 | 1 |
| [A Spectacular Journey on the Worlds Most Specta](https://sketchfab.com/3d-models/none-13dec77d3ef04ef2b9ccdf7b02bc111e) | klrxyz | BY | 1678 | 1 |
| [Giant Mushroom House - Voxel 32x32 (Anim)](https://sketchfab.com/3d-models/none-ebe265f769a7406dbf7574b8b92b5ca6) | Max3nce_3D | BY | 2896 | 0 |
| [Giant Mushroom - Voxel 32x32 (Animated)](https://sketchfab.com/3d-models/none-542b41fa787b4280a8fed783a42703c0) | Max3nce_3D | BY | 2460 | 0 |
| [Desert Map](https://sketchfab.com/3d-models/none-a2c47588ba7b404f9da8649c50ea08e6) | Mike2218 | BY | 345133 | 0 |

## flora

| Модель | Автор | Лиц. | Фейсы | Лайки |
|---|---|---|---|---|
| [Sci-Fi Plant Capsule](https://sketchfab.com/3d-models/none-bd2d05db1bf24143ac2030c57e1eb05e) | Berk Gedik | BY | 32562 | 1092 |
| [Pine tree](https://sketchfab.com/3d-models/none-e52769d653cd4e52a4acff3041961e65) | Andriy Shekh | BY | 2173 | 1008 |
| [Alien World Explorer](https://sketchfab.com/3d-models/none-f73af15ccc2849098df32f48133cba9b) | Sharon Kunne | BY | 71830 | 996 |
| [Autumn Nest](https://sketchfab.com/3d-models/none-9ff48b4830e2418f92e1b75afb160a52) | kennethtzh | BY | 110535 | 899 |
| [VR Alien Forest (with download)](https://sketchfab.com/3d-models/none-a82890cdeb9f4032aba416547711a3d2) | FDU_oficial | BY | 30752 | 787 |
| [Stylized Pine Tree Tree](https://sketchfab.com/3d-models/none-deadcadc915545a7b4701dbe6eb419e8) | Batuhan13 | BY | 939 | 573 |
| [Alien neuron network core](https://sketchfab.com/3d-models/none-9b1af8411cf94819af3c60fef1ab6c36) | 3dhdscan | BY | 301078 | 392 |
| [Alien Plant (Cydrakoniss)](https://sketchfab.com/3d-models/none-d940adfc452146efacc2cbb63fa2c01d) | Blake Stahlbaum | BY | 6007 | 391 |
| [Alien Plants kit](https://sketchfab.com/3d-models/none-5cd6f2d5b87c4c638694e187b1b2f5ba) | Jungle Jim | BY | 398594 | 380 |
| [Tentacle Plant](https://sketchfab.com/3d-models/none-8372c5e8dfca49a1ae1046e02f47e2d0) | meowskie | BY | 3308 | 335 |
| [Exotic Pink Leafed Plant](https://sketchfab.com/3d-models/none-868828fad0e348378c6be5397da0895c) | Nicola Hynes | BY | 3120 | 311 |
| [Planta Fantasía en flor](https://sketchfab.com/3d-models/none-d77c6b4064ad421e8195d7ff58cf331a) | violetaraffin | BY | 7640 | 292 |
| [Stylize Tree Lowpoly](https://sketchfab.com/3d-models/none-dcf20a5a86784331a34657e94442511f) | uday | BY | 1024 | 290 |
| [(FREE) Anime House](https://sketchfab.com/3d-models/none-d4740edd01a1436bbc3e2e4ec91f153e) | LowPolyBoy | BY | 34488 | 279 |
| [Aloe Plant Set](https://sketchfab.com/3d-models/none-f9b15dae5a194e41b27c220717594785) | LadyIReyna | BY | 57056 | 275 |
| [Alien Plant](https://sketchfab.com/3d-models/none-142addd0188c49e18e6d836fb9dfe813) | green364 | BY | 10056 | 271 |
| [Low Poly Pine](https://sketchfab.com/3d-models/none-107e3eab9dcb4e29a282c6709178752f) | Afloat Above the World | BY | 708 | 261 |
| [Pine Tree [Game-ready]](https://sketchfab.com/3d-models/none-dc3fbd9205cf4027a4455d1f415e0478) | Wenedi (^-^)/ | BY | 1650 | 257 |
| [Alien Plants](https://sketchfab.com/3d-models/none-853efc6c8e2a4eb59b88115f92297034) | Wenjing Huang | BY | 10184 | 248 |
| [Alien plant life](https://sketchfab.com/3d-models/none-eb1ecdbd89b342b18d5430f9f2a8aae7) | hannahwool | BY | 13674 | 238 |
| [Old Poplar tree stump -free](https://sketchfab.com/3d-models/none-604a5c25e3b84fa0ae520460112c9f92) | matousekfoto | BY | 78646 | 213 |
| [Rose Bush - Detailed](https://sketchfab.com/3d-models/none-f4d81ec952d04452a2f8421985875e56) | ramakarl | BY | 417128 | 197 |
| [Modular Vines](https://sketchfab.com/3d-models/none-eccf0e6839954dca8eabfb504d572394) | Somersby | BY | 63220 | 190 |
| [Hanging Pothos](https://sketchfab.com/3d-models/none-9b961b9aab1443a2a14b81de726e6d5f) | Chocolate Dogg | BY | 48136 | 180 |
| [Rose](https://sketchfab.com/3d-models/none-b5277921b85c43b2a6c1ee92bd63b3dc) | victorberdugo1 | BY | 7544 | 172 |
| [Pine Trees Pack | PS1 Low Poly](https://sketchfab.com/3d-models/none-347d7f6122af4bf684f752df2dd0eb5a) | Vova Truestory | BY | 353 | 171 |
| [Nature Vegetation Pack](https://sketchfab.com/3d-models/none-c257b444645542da867fce0cf221e52d) | AnderArts | BY | 46062 | 170 |
| [Painted wooden kitchen cabinet](https://sketchfab.com/3d-models/none-90005a6795244053a49c3eb525d0abb4) | Peter Nox | BY | 5600 | 165 |
| [Stylized Pine](https://sketchfab.com/3d-models/none-8a654cc7f8394416b8148f3de488453e) | BiscuitEater | BY | 3494 | 164 |
| [Hanging Plants Pack (LODs, game-ready)](https://sketchfab.com/3d-models/none-4adf9301069143ba8a855a451dc5b476) | LOLIPOP | BY | 154402 | 158 |
| [Antique Bookcase filled with items](https://sketchfab.com/3d-models/none-6a4913a8cb4d41ada53650bd48a842bc) | Peter Nox | BY | 16059 | 150 |
| [Handpainted Tree Pack](https://sketchfab.com/3d-models/none-d246b8b4439d44c6adc854ebc5ffc5ba) | rudolfs | BY | 30820 | 126 |
| [Stylized pine tree pack](https://sketchfab.com/3d-models/none-46ac3c1a4c3645b1984a97568db54c5d) | W_illness | BY | 93372 | 123 |
| [Pink Lotus Flower Cluster](https://sketchfab.com/3d-models/none-53d081e40cae4b5ebc66c6e06b5f19ef) | Galaxy Abundant | BY | 485838 | 88 |
| [GA Free_301_Pink Lotus Twin Bloom](https://sketchfab.com/3d-models/none-54cf6843761f49069e40bbd127ffb29f) | Galaxy Abundant | BY | 483814 | 73 |
| [⚡️Bramble Overload Set Pubg/Bgmi 3D Model⚡️](https://sketchfab.com/3d-models/none-5b366776c071484c8306413d4b0859fa) | YT-XTREMENINJA | BY | 200574 | 70 |
| [Arena_Side](https://sketchfab.com/3d-models/none-01c63acaceb340dd984df603287913f0) | Mordrenn | BY | 71042 | 61 |
| [withered trunk (thin)](https://sketchfab.com/3d-models/none-b4f59b5387404747858680e79e2670d7) | SibeYu | BY | 8503 | 56 |
| [Cabin of mushrooms](https://sketchfab.com/3d-models/none-3a4f5881a3294af29922f36be13253ba) | Rickerson | BY | 15405 | 56 |
| [Lady Seraphine, elegant aristocrat](https://sketchfab.com/3d-models/none-d168b630f4c2417d8b392ee37383cae3) | XRProfXR | BY | 379972 | 51 |
| [Wrecked](https://sketchfab.com/3d-models/none-dd6ee450adfc400c91c524e74ed28de9) | 3dhdscan | BY | 76888 | 51 |
| [Mega Maid 5km From Spaceballs 1987](https://sketchfab.com/3d-models/none-89b560195b9c408e8e2256e5e539d724) | Binkley-Spacetrucker | BY | 78751 | 48 |
| [Where a Forest Grows](https://sketchfab.com/3d-models/none-cf8f8f206617406caf76ffae2660fb0d) | Wren Mason-Blaug | BY | 51172 | 47 |
| [Tree Roots](https://sketchfab.com/3d-models/none-961841eefc0c4896a42648cc8d480482) | falk lochmann | BY | 5200 | 42 |
| [Mushroom Boss - game ready with animations](https://sketchfab.com/3d-models/none-ee611e4925e94a98baa4cfd3d89d3449) | iamjohnhult | BY | 11598 | 36 |
| [Vine lampshade Arabo](https://sketchfab.com/3d-models/none-41aee3ae45b34b1ca256e8eb9228d69b) | graphic.el | BY | 323918 | 34 |
| [Aesop's Fables #1 - The fox and the grapes](https://sketchfab.com/3d-models/none-b4d2632f58fc465cb7026bf49bb8d8af) | AlessandroFortuna | BY | 116005 | 31 |
| [Ball type penannular thistle brooch](https://sketchfab.com/3d-models/none-07b8053d04f8423b8ddbcf0f603bf790) | The Hunt Museum | BY | 50000 | 31 |
| [Black Myth: Wukong - Zhang Deng Yu Shi [Anim]](https://sketchfab.com/3d-models/none-578649f8f7614eb1a23088120a6b601a) | Haku Dragon | BY | 126862 | 23 |
| [Black Myth: Wukong - Zhang Deng Yu Shi [Static]](https://sketchfab.com/3d-models/none-ec3f4382c6604b81995072dc3a48e923) | Haku Dragon | BY | 126862 | 23 |
| [Stylized Plants Vines & Flowers – Hand Painted](https://sketchfab.com/3d-models/none-3988738c33094165aa6b9d0d885ad084) | Eduardo Vinicius | BY | 71 | 19 |
| [Cardboard Vine Platform](https://sketchfab.com/3d-models/none-10fc66b44e6a4f1f84c170a4acbd02ba) | Kyla Burgess | BY | 5836 | 15 |
| [Roblox DOORS | OUTDOORS : Bramble](https://sketchfab.com/3d-models/none-8dd8062673d54d9f9f22ecde5c11fb51) | Capi | BY | 11450 | 11 |
| [Chibi Lizard Lowpoly handpainted](https://sketchfab.com/3d-models/none-22697eb04fd746b39eced08ef957bc84) | UFRE Studio | BY | 15054 | 10 |
| [DOORS The Great Outdoors - Bramble](https://sketchfab.com/3d-models/none-fef9baccca68401d929988ec942a77f5) | Whateverdata55 | BY | 2216 | 10 |
| [Sir Rowan and Bramble](https://sketchfab.com/3d-models/none-9d57be2fce41449c840b10bd2f6b067f) | Brian Trepanier | BY | 176966 | 10 |
| [Rose Bush Scan](https://sketchfab.com/3d-models/none-a92d6bdc2a364f50b838f62a528fbf40) | giantbooley | BY | 31143 | 9 |
| [DOORS The Great Outdoors - Eyestalk](https://sketchfab.com/3d-models/none-6fda785f2c0f4c90909d55a91dfd470a) | Whateverdata55 | BY | 264 | 6 |
| [Bramble From The Outdoors!!](https://sketchfab.com/3d-models/none-7a467eaa36654bfa96ab9411a0b91ae7) | Mines30august | BY | 11450 | 6 |
| [Swamp Lvl1](https://sketchfab.com/3d-models/none-a5f593233e024599ab5990a5b3b1df06) | DannyBoy24 | BY | 2506 | 5 |
| [Title The Swamp Thing    A digital art of the Sw](https://sketchfab.com/3d-models/none-2ec1c4616b464044b1cd84505112a485) | klrxyz | BY | 40398 | 1 |
| ["Crown of Thorns" Earring](https://sketchfab.com/3d-models/none-c38495fae3664437ac51bdb3f77820b4) | kara.v.3d | BY | 328022 | 0 |

## creatures

| Модель | Автор | Лиц. | Фейсы | Лайки |
|---|---|---|---|---|
| [phoenix bird](https://sketchfab.com/3d-models/none-844ba0cf144a413ea92c779f18912042) | NORBERTO-3D | BY | 4064 | 5063 |
| [Lowpoly Animal Skulls - 1](https://sketchfab.com/3d-models/none-2938b8237531482d9750ce2717fbe099) | Loïc Norgeot | BY | 87497 | 3344 |
| [⛨ The Forgotten Knight ⛨](https://sketchfab.com/3d-models/none-d14eb14d83bd4e7ba7cbe443d76a10fd) | dark_igorek | BY | 223719 | 2520 |
| [Real-time Bones Demo: Phoenix Bird](https://sketchfab.com/3d-models/none-6a8d1fd20a1f46e988d9d06e028f75a0) | Sketchfab | BY | 4064 | 2147 |
| [Tarisland - Dragon (High Poly)](https://sketchfab.com/3d-models/none-ecf63885166c40e2bbbcdf11cd14e65f) | Doctor A. | BY | 40437 | 2146 |
| [Dragon katana "Oni koroshi"](https://sketchfab.com/3d-models/none-778d3dfe4cc549dd93c9bdc5ad78a420) | Sky_Hunter | BY | 79986 | 2028 |
| [Medieval Knight | Sculpture | Game ready](https://sketchfab.com/3d-models/none-6cdd055b4afa41eb9360dbbfe75c7f10) | by__Rx | BY | 41654 | 2014 |
| [Lowpoly PBR Knight Armour](https://sketchfab.com/3d-models/none-7e8485a272784e6cb3b5bda68ff319fa) | soidev | BY | 118365 | 1815 |
| [Chunky knight](https://sketchfab.com/3d-models/none-f1722ab650ad4d8dbe6fc4bf44e33d38) | thanhtp | BY | 3688 | 1720 |
| [1964.88 Armor for Man and Horse](https://sketchfab.com/3d-models/none-faca2ad557f64809b598c3a646a8804f) | Cleveland Museum of Art | CC0 | 119758 | 1618 |
| [Silver Dragonkin (Mir4)](https://sketchfab.com/3d-models/none-89ead4e87cdc4b70840f748383f0998f) | Doctor A. | BY | 22338 | 1603 |
| [One angery dragon boi](https://sketchfab.com/3d-models/none-e590e148d35e4e589c6aca7d5e87ab9e) | ricocilliers | BY | 68276 | 1513 |
| [Ultimate Monsters Pack](https://sketchfab.com/3d-models/none-fd72e114d119488da71fe3a16f216c4f) | quaternius | BY | 212178 | 1460 |
| [Knight Artorias and Great Grey Wolf Sif](https://sketchfab.com/3d-models/none-bdf57355f95a420fa0c5b3e924bea16d) | 9S | BY | 28765 | 1438 |
| [Animated Dragon Three Motion Loops](https://sketchfab.com/3d-models/none-eca98cf6cd084c1596cecf716e110c29) | LasquetiSpice | BY | 19544 | 1389 |
| [Female Noble Knight Armor](https://sketchfab.com/3d-models/none-5b40c2e4b40d48378d595268d2db69bb) | Meast | BY | 32319 | 1245 |
| [Demon Mask](https://sketchfab.com/3d-models/none-e8f1ddf1cd344a50bdb4a30beaff8333) | egorova.e | BY | 5686 | 1160 |
| [Sir Frog - Chrono Trigger](https://sketchfab.com/3d-models/none-0af0c15e947143be8fab274841764bf1) | Adrian Carter | BY | 167256 | 1135 |
| [Dragon Glass](https://sketchfab.com/3d-models/none-fe00cb0ecaca4e4595874b70de7e116b) | 5th Dimension | BY | 465824 | 1089 |
| [COD: Ghost's Wolf [+Puppy]](https://sketchfab.com/3d-models/none-2db0c6813c9b471e9e9eb2b4d8a01d08) | Puppies4life | BY | 14960 | 1076 |
| [CIBER WOLF By Oscar creativo](https://sketchfab.com/3d-models/none-540a6465144d481b9f42b878443ae860) | OSCAR CREATIVO | BY | 519402 | 1063 |
| [Demon Dragon Full Texture](https://sketchfab.com/3d-models/none-19035a72cdcb4abfa2c161de32823e6b) | endlessvoidmc | BY | 312688 | 1059 |
| [icy dragon](https://sketchfab.com/3d-models/none-2db9268227b943e6a41e88390f2875a6) | chengzijieczj | BY | 133666 | 1046 |
| [Wolf Mask](https://sketchfab.com/3d-models/none-ce144a94070d45be8cf344c0313266b7) | flioink | BY | 315760 | 966 |
| [Needle OC](https://sketchfab.com/3d-models/none-2d523b639c79407daff09ed23491e706) | CataRackta | BY | 62227 | 934 |
| [demon girl demi going to the beach](https://sketchfab.com/3d-models/none-d67ff51ed93f463897b89b6a093bb44e) | Jesus aponza | BY | 90246 | 910 |
| [Stelae knight](https://sketchfab.com/3d-models/none-0a795ae613f2496e955974ca37bf1e94) | Muru | BY | 20060 | 897 |
| [Wolf Guy](https://sketchfab.com/3d-models/none-ae247d7e082746d69b47cd617bd2fd32) | Juan_Puerta | BY | 12203 | 883 |
| [Warrior - game ready / free / fixed a bug](https://sketchfab.com/3d-models/none-1b862fe8b5d3449eb928eb49ff093c0e) | Alanik | BY | 92201 | 853 |
| [Whale of the veil - Stylized animated model](https://sketchfab.com/3d-models/none-7a54c3e8514647bf852f07aa27b0c203) | Meleagors | BY | 182028 | 847 |
| [Modular Dungeon Kit](https://sketchfab.com/3d-models/none-ecbeefd3212f447ba2f10f1986ad5769) | HenryBoadle | BY | 150616 | 820 |
| [DAE Diorama - Forest Loner & Racing](https://sketchfab.com/3d-models/none-d07f6f63202440739a95d80a0e93e9b6) | Julian | BY | 54937 | 820 |
| [Demon Hunter](https://sketchfab.com/3d-models/none-ff6a14371bc347d9a1526f5e64b29327) | patrickvdk | BY | 9862 | 817 |
| [Wolf Haircut (With Bones)](https://sketchfab.com/3d-models/none-3051c06f16504483a7815386cc0ff4a8) | zHairezt | BY | 11975 | 807 |
| [Zoey | Fortnite | Kpop Demon Hunters 3D Model](https://sketchfab.com/3d-models/none-aa3d9d519b34425ea5dd1456c4c67eaf) | Centrixe the Dodo | BY | 55781 | 785 |
| [free knight armor 2k](https://sketchfab.com/3d-models/none-8009434a1776469ea8cb3225c2d63dfb) | DEYMAR | BY | 26469 | 769 |
| [Old Fantasy Potions](https://sketchfab.com/3d-models/none-0b0ffbbb5c454bb7879c6f476d3c009b) | Muko_Art | BY | 2272 | 761 |
| [Pig Demon](https://sketchfab.com/3d-models/none-bba636efaee04b3ea988b2ae4487cfc1) | Lexington Dath | BY | 22924 | 747 |
| [Royal Apothecary Society Lab - *Annotated!*](https://sketchfab.com/3d-models/none-aba28e9a3e6d4d7d9f78f636670c7e56) | VideoJames | BY | 105061 | 730 |
| [Hydria apothecary vase](https://sketchfab.com/3d-models/none-7d6938c0c0b54b06a0210a982a73023e) | Virtual Museums of Małopolska | CC0 | 60002 | 730 |
| [Samurai Mask Sketch](https://sketchfab.com/3d-models/none-976564c01fb341e3b75fc4dcc4e96ba8) | Yaroslav.Kononenko | BY | 12116 | 719 |
| [Train Track - Modular Pack](https://sketchfab.com/3d-models/none-e662a834e5fb4b65ad4f8194f8af515b) | Digital Goblin | BY | 18816 | 695 |
| [Mastercraft Mithril Armor set for Beyond Skyrim2](https://sketchfab.com/3d-models/none-3ae9057e840e4da0bf05269865c29c91) | Royraj Vichaidit | BY | 18839 | 691 |
| [Fictional supercar - V12 Goblin](https://sketchfab.com/3d-models/none-0a20e49ad5774d778567cb5c3f345786) | ollitei | BY | 86623 | 685 |
| [🐉 Realistic Dragon Textures](https://sketchfab.com/3d-models/none-66803f2e8461486987d72d116fe2f714) | Jazz Vincent | BY | 19563 | 610 |
| [Loona - Helluva Boss - 3d printable Fanart](https://sketchfab.com/3d-models/none-e2b7c01407114a2797beed055821ea8d) | Printed Obsession | BY | 70414 | 609 |
| [Prowler Dragon Variant Rig](https://sketchfab.com/3d-models/none-7ee71aaf323d426bbbdf28d73d55bbd9) | DM-913 | BY | 19766 | 583 |
| [Low Poly Pine Tree](https://sketchfab.com/3d-models/none-99fb6a37547840e3a295689df032ba28) | Epic wolf studio | BY | 44884 | 581 |
| [Hell Wolf](https://sketchfab.com/3d-models/none-551f8788eb3f4b09b644ddf9b6f76db8) | Fubbi | BY | 4000 | 563 |
| [Humanity Reconstructed (Mir4)](https://sketchfab.com/3d-models/none-372b74b55c384aa682552ab556ace091) | Doctor A. | BY | 13134 | 547 |
| [LOTR Cave Troll - (for animating/rigging)](https://sketchfab.com/3d-models/none-20a50af723314c1dac04aad8c929aa1e) | Ole Gunnar Isager | BY | 17892 | 542 |
| [Ding censer with an openwork cover](https://sketchfab.com/3d-models/none-1034cd8e4df5430cb874f012415a43b0) | Virtual Museums of Małopolska | CC0 | 133000 | 541 |
| [Wolf Spider (Rigged) - (Rabidosa rabida)](https://sketchfab.com/3d-models/none-6392e4cfb64d407182fdad2cea9e0abe) | Dreaming In Alternation 27 | BY | 8458 | 539 |
| [Lesser Cyclops Variant Rig](https://sketchfab.com/3d-models/none-40d31731af1b46b59addd81ed5750db0) | DM-913 | BY | 18164 | 526 |
| [Statue](https://sketchfab.com/3d-models/none-ecd586875a8845cebb74aa9ea11756ed) | DJMaesen | BY | 1496 | 525 |
| [Shengding Food Vessel, around 575 BCE](https://sketchfab.com/3d-models/none-8f45c5ab58794f4faca0c242f89fdf22) | Minneapolis Institute of Art | CC0 | 64178 | 459 |
| [Old Magical Book](https://sketchfab.com/3d-models/none-326cf7653c7c4ec19d2672f5a7a33578) | Akiko.Tomiyoshi | BY | 41028 | 459 |
| [Lilypads](https://sketchfab.com/3d-models/none-9c55bc7d174a42878a47f31c2895a607) | sligocreatures | BY | 44313 | 428 |
| [Cyclops Rig](https://sketchfab.com/3d-models/none-e5cc86878c314f5bae6d7268bb7541d9) | DM-913 | BY | 11316 | 422 |
| [Roman officer - body armour](https://sketchfab.com/3d-models/none-f643a3129a9f4b15b9e5abe220020af7) | Andy Woodhead | BY | 37151 | 421 |
| [Scourger Rig](https://sketchfab.com/3d-models/none-deb9b27965064f85a7f482599f5a69a8) | DM-913 | BY | 81670 | 406 |
| [Gryphon Skeleton](https://sketchfab.com/3d-models/none-03a1e9900d6749429be0ccc1a6239541) | Got_Pizza | BY | 347782 | 401 |
| [Giant cockroach](https://sketchfab.com/3d-models/none-4b19ed8851f74c278eef835250a82896) | Drillimpact | BY | 576 | 400 |
| [Goblin](https://sketchfab.com/3d-models/none-2b753abc899648a5a16357be8a8db647) | keianhzo | BY | 53471 | 399 |
| [Modular Myan Inspired Dungeon](https://sketchfab.com/3d-models/none-10f3dc23c6dc4fd99083e3128d83ee2f) | R-LAB | BY | 23216 | 398 |
| [Undead Knight [Low-Poly]](https://sketchfab.com/3d-models/none-0453f93869af4776ac69d9ef896aa6a1) | SchrisArt | BY | 15656 | 394 |
| [Squigly- Skullgirls](https://sketchfab.com/3d-models/none-3d83c1f0deeb435385beeed2261c8846) | Phan21 | BY | 44913 | 392 |
| [Stone Griffin, Downing College, Cambridge](https://sketchfab.com/3d-models/none-d94638d6c6ad4119b868567cf8e9fe2b) | Thomas Flynn | BY | 120364 | 365 |
| [Wyvern Fishing](https://sketchfab.com/3d-models/none-06c8ebe3d34642e697755a516468315a) | Benoit Gagnier | BY | 60304 | 361 |
| [Agnaktor](https://sketchfab.com/3d-models/none-ea76eb689ca14ff49190cdc42216183d) | Mr. Bird | BY | 7892 | 358 |
| [LOTR Troll - (for animating/rigging)](https://sketchfab.com/3d-models/none-f4d777fd41d045fb8692f19f07b998fe) | Ole Gunnar Isager | BY | 17824 | 354 |
| [Magic Witch](https://sketchfab.com/3d-models/none-3b72325c2a284e2baa43838b74fad986) | Catarina Cerqueira | BY | 8140 | 353 |
| [Orc Tomahawk](https://sketchfab.com/3d-models/none-4f53aca64e8e4883ac20aff8b67d519b) | Tarik Takasu | BY | 4354 | 342 |
| [Book of Vile Darkness](https://sketchfab.com/3d-models/none-481ca7b5403745708373aa497aa06473) | DannyAg | BY | 34230 | 337 |
| [Mordor Troll - "Olog Hai" - Lord of the Rings](https://sketchfab.com/3d-models/none-7b6000538d9843a69d9b98fb32e4d62b) | Ole Gunnar Isager | BY | 17824 | 334 |
| [Cursed Undead Soldier Rig](https://sketchfab.com/3d-models/none-b2e39de76b834d978adee5716aec814d) | DM-913 | BY | 16743 | 334 |
| [Skeleton Dragon](https://sketchfab.com/3d-models/none-0ae92a30d2c242069acd61ed432f6c32) | LivindorCreation | BY | 7993 | 328 |
| [Possessed undead](https://sketchfab.com/3d-models/none-7394bdf26d664810b42b83153411cf74) | giantSwing | BY | 7675 | 324 |
| [Girl with clothes. Worker set](https://sketchfab.com/3d-models/none-4cf10e2dde6d4b12b6dccd25a4cf3b77) | cattleya | BY | 2469 | 322 |
| [Reaper](https://sketchfab.com/3d-models/none-1ea16626fced4a04ad33e6d5d564bef0) | 3DRT STUDIOS | BY | 21037 | 317 |
| [Winged Dragon, one of pair, 5th-4th century BCE](https://sketchfab.com/3d-models/none-b390b4046d6c46beb811c8c9e21014d4) | Minneapolis Institute of Art | CC0 | 63972 | 314 |
| [Galby [VRC model Free]](https://sketchfab.com/3d-models/none-bd15340d839542169b8f4e33045e7d5f) | Shortskirts | BY | 20024 | 311 |
| [Skeleton animated](https://sketchfab.com/3d-models/none-9210377c7a514cf6b48a31b9d3991ff3) | danielmclogan | BY | 6821 | 304 |
| [Cragle](https://sketchfab.com/3d-models/none-595defc6c81e40ddbca435b78e0ae9f7) | ChamberSu | BY | 21684 | 295 |
| [Godzilla](https://sketchfab.com/3d-models/none-ca862e19cad04f95af49263405ee04dd) | Pumpkin | BY | 61143 | 289 |
| [Gui (Chinese food vessel)](https://sketchfab.com/3d-models/none-81983aad41fe4981b06dd2f26d98213c) | Minneapolis Institute of Art | CC0 | 63936 | 285 |
| [Goblin Shredder Shield - Animated!](https://sketchfab.com/3d-models/none-a1c649875ef44f5cbbd2d2f51a9b3721) | VideoJames | BY | 1824 | 284 |
| [Zombie Warrior](https://sketchfab.com/3d-models/none-81d59334573e4089960ef5bffb5a827b) | tgalextg | BY | 5144 | 282 |
| [Minion](https://sketchfab.com/3d-models/none-8b24f1d553f643ee8f5f48197cb2f375) | DJMaesen | BY | 2618 | 279 |
| [Griffin Animated](https://sketchfab.com/3d-models/none-a8852f113416426bb06e6bba49a525a9) | VitSh | BY | 670 | 276 |
| [shortstack goblin girl](https://sketchfab.com/3d-models/none-142366ce5a8543f6a9f9383a520dc517) | slate | BY | 120684 | 259 |
| [Cirilha - The Keeper](https://sketchfab.com/3d-models/none-15aa0ba566cf4af5abd5b0971ba5df18) | Katharina Alexander | BY | 117410 | 255 |
| [Goblin Animations](https://sketchfab.com/3d-models/none-b9705b05dd6c47b29ec943bc096cbf3a) | codywellman | BY | 8274 | 255 |
| [Shotgun](https://sketchfab.com/3d-models/none-3c3ed272927c4f3d83d607b2acbf7aab) | Virtual Museums of Małopolska | CC0 | 326272 | 250 |
| [Shiva Nataraja, Lord of the Dance](https://sketchfab.com/3d-models/none-227d1524875042ce90176129ba98de6c) | Minneapolis Institute of Art | CC0 | 63994 | 249 |
| [Goblin Mess Hall](https://sketchfab.com/3d-models/none-3ccf9b0efc164d8ba2e71b8a239cf989) | ColorBots Studios | BY | 19272 | 243 |
| [Family Guy | Griffin's Living Room](https://sketchfab.com/3d-models/none-3172620a27b3486eb36feaaafdd16be6) | folderplug | BY | 37460 | 241 |
| [Rigged for UE4 - Roman - free](https://sketchfab.com/3d-models/none-a488272c15cc4aa5898c28e5141e939d) | Andy Woodhead | BY | 280624 | 239 |
| [Haunted Mansion Stretching Room](https://sketchfab.com/3d-models/none-84d56e0512a44e8bbbb8d8e80ff29a45) | pixeldog4ever | BY | 43356 | 234 |
| [Lidded ritual ewer (huo)](https://sketchfab.com/3d-models/none-5298a9e660df4666b90b2800126fdb67) | The Smithsonian Institution | CC0 | 256200 | 233 |
| [Shin Godzilla ( profile )](https://sketchfab.com/3d-models/none-e3d0aa25805d4ef7967cdcc4608610f0) | gabetke | BY | 597325 | 231 |
| [city building](https://sketchfab.com/3d-models/none-f38a12b260c042c39b4906494be9c26e) | Griffin M | BY | 328 | 231 |
| [Mega Scene](https://sketchfab.com/3d-models/none-b9909021fc8e467f9e34741db3f389e0) | VitSh | BY | 50312 | 227 |
| [Lidded ritual ewer (guang)](https://sketchfab.com/3d-models/none-c2898500387d40678d26d15d12809608) | The Smithsonian Institution | CC0 | 299989 | 226 |
| [1986.85 Jar with Dragon Design](https://sketchfab.com/3d-models/none-081f588b9ca24a0ea5ed79ee3d7bed47) | Cleveland Museum of Art | CC0 | 20412 | 212 |
| [Gargoyle #3DST27](https://sketchfab.com/3d-models/none-2581387ab6454f1787e50d5909b325aa) | alban | BY | 71817 | 210 |
| [Potion Bottle Pack - Hand-painted Props](https://sketchfab.com/3d-models/none-4eac61bd39354aed9a8b716dae7f25f7) | Breana Wright | BY | 946 | 210 |
| [Gargoyle](https://sketchfab.com/3d-models/none-b56d96d4c56749daab3cbeae5c001593) | vadimel | BY | 45494 | 207 |
| [Wizard Tower Animated](https://sketchfab.com/3d-models/none-cc777c6f112840bea2d4a2c612916906) | VitSh | BY | 6266 | 206 |
| [Rigged for UE4 - Saxon - free](https://sketchfab.com/3d-models/none-8b241785d74347f89f4d0f201c88600a) | Andy Woodhead | BY | 117850 | 205 |
| [Dragon skeleton low-poly](https://sketchfab.com/3d-models/none-195861853eba45e3a7d499a532cb93be) | mgsawwinhtike3002 | BY | 32755 | 203 |
| [Monster Truck Generator Dump](https://sketchfab.com/3d-models/none-b361ea54b9074974852aa25b7dd9f5ce) | Jorma Rysky | BY | 575519 | 202 |
| [Godzilla 2014](https://sketchfab.com/3d-models/none-6530bee86f5d4099a4ab7c6b85575f90) | ManySince910 | BY | 3376 | 197 |
| [Griffon statue](https://sketchfab.com/3d-models/none-2cd29b9448f6425e9008aa26aa9bc321) | Feivelyn | BY | 5097 | 197 |
| [Wyvern concept car - FREE](https://sketchfab.com/3d-models/none-969f16578168435d8f12391001fbd311) | NETRUNNER_pl | BY | 13466 | 195 |
| [The Queen of the Caverns](https://sketchfab.com/3d-models/none-006532548c5c49cfaeebe9b959ce9926) | Aligned Games | BY | 187649 | 191 |
| [1967.34 Plate With Winged Griffin](https://sketchfab.com/3d-models/none-5802382f24eb490caacbcb0e1de3fb22) | Cleveland Museum of Art | CC0 | 85000 | 186 |
| [Shiva Nataraja (after conservation), c. 1100 CE](https://sketchfab.com/3d-models/none-5bc151d954f74b238079726bf2205c23) | Minneapolis Institute of Art | CC0 | 64000 | 184 |
| [Stylized Potion](https://sketchfab.com/3d-models/none-78d411a585144c4f91ca51d876982ed7) | Moon Cube Studios | BY | 3124 | 182 |
| [Four-Legged-Frost-Predator.](https://sketchfab.com/3d-models/none-3ddc6f711201449cbc33617f430d1b20) | bensimulator2 | BY | 14564 | 181 |
| [Catfish Mech low-poly (animated)](https://sketchfab.com/3d-models/none-ad9bc16464744935b1ac9b7768a17474) | Jungle Jim | BY | 14650 | 179 |
| [Magic book set](https://sketchfab.com/3d-models/none-ee1747b4d426446ba5f14bcf2aec8400) | evildeer | BY | 6446 | 177 |
| [Gargoyle  (8.5K Tris)](https://sketchfab.com/3d-models/none-6ee63765c42642928c61510dac7b3df4) | adamvfc | BY | 8506 | 175 |
| [Fish fountain sprinkler](https://sketchfab.com/3d-models/none-9df20fc35c5a4cac904ebe9f36ee368d) | matousekfoto | BY | 230512 | 175 |
| [Zagmatorah - An Ancient Dragon](https://sketchfab.com/3d-models/none-19ad6f2e9d384159a6f686115aebf7a0) | Seth Santos | BY | 38378 | 171 |
| [Griffon statue in Tbilisi](https://sketchfab.com/3d-models/none-e9ce19f72e434d3494d364c59246307a) | Nik | BY | 97772 | 171 |
| [Crossbow Lady](https://sketchfab.com/3d-models/none-9b48ece66abf43219f23065e1636f18e) | MoonCat | BY | 37988 | 170 |
| [Verdant Gargoyle](https://sketchfab.com/3d-models/none-64018437728e4f82a4382b23afd237a1) | Spinnee | BY | 12482 | 155 |
| [Ding food vessel, 11th-10th century BCE](https://sketchfab.com/3d-models/none-6d3936d74ab942d48e9ddb985182eeea) | Minneapolis Institute of Art | CC0 | 64000 | 149 |
| [Tree Dragonfrog](https://sketchfab.com/3d-models/none-1be77f2648914b3aa73a31953b800980) | Daniel Reyes | BY | 24161 | 147 |
| [Fallen Book](https://sketchfab.com/3d-models/none-2678ba288c7f4131ab1509e0c27c1c7a) | jesseroberts | BY | 35072 | 146 |
| [Crystal Ball](https://sketchfab.com/3d-models/none-30618a3f909a4376af013a1d1de1cbba) | Randall_3D | BY | 56615 | 143 |
| [Wyvern](https://sketchfab.com/3d-models/none-06809e9220314dc1b118b9cd02d280b8) | Adrian Carter | BY | 124919 | 142 |
| [Gargoyle](https://sketchfab.com/3d-models/none-fe1e8b16f7c74f5ea2838eabe91ae92b) | Cat O. | BY | 14712 | 142 |
| [Halloween Haunted House](https://sketchfab.com/3d-models/none-402b277372424e75b4c7aec309889dda) | Karthik Naidu | BY | 18408 | 141 |
| [Gargoyle Statue (Photogrammetry)](https://sketchfab.com/3d-models/none-cb31805592fc4a4a99d9a294cbe50694) | pigfinite | BY | 62070 | 140 |
| [Fanghu Wine Vessel, 5th - 4th Century BCE](https://sketchfab.com/3d-models/none-e2e70b742453405fa901d59722d13736) | Minneapolis Institute of Art | CC0 | 64018 | 133 |
| [1942.206.c Ink Cake in Shape of Coiled Dragon](https://sketchfab.com/3d-models/none-15d18e208095465dbcc6912c54cf00ee) | Cleveland Museum of Art | CC0 | 40000 | 125 |
| [Stone Gargoyle](https://sketchfab.com/3d-models/none-43b36f75c57d43e0ac14010e0f84e7b3) | WakeKnight | BY | 25212 | 120 |
| [Ghost Raven](https://sketchfab.com/3d-models/none-0a5014dc066548f7b746ccfdbb4f2d9a) | Alex Sanches | BY | 1588 | 115 |
| [Eleodes - darkling beetle](https://sketchfab.com/3d-models/none-e1409dcd76b54c0a8a5dcecb37998316) | EvoBiomech | BY | 259482 | 114 |
| [Giant Stick Insect](https://sketchfab.com/3d-models/none-a6a0c0a7944f4cffad3b66de56bcebcd) | RISD Nature Lab | BY | 467358 | 109 |
| [Ram Architectural Detail](https://sketchfab.com/3d-models/none-089e194177a9493a8c5800d429b1fa7d) | Thomas Flynn | BY | 30000 | 105 |
| [Suzan](https://sketchfab.com/3d-models/none-1b6f09a25b934c28825d1363677a46da) | Blue Spirit | BY | 44438 | 91 |
| [FREE Eldritch horror Inspired Creature](https://sketchfab.com/3d-models/none-dedd097fb3ea4b388c7d7c71d5908d3c) | Axinovium | BY | 47379 | 91 |
| [crow](https://sketchfab.com/3d-models/none-e8e9af989b304efa96c173b4d29950d4) | paulincalmejane.lya | BY | 53419 | 88 |
| [Papilio cresphontes - Giant Swallowtail](https://sketchfab.com/3d-models/none-2671fc15b02642bfb20507c6e5f14d6b) | Frank McMains | BY | 242124 | 87 |
| [Le Epic Mech](https://sketchfab.com/3d-models/none-18feb3c805c44b98bfa62a9a00886ece) | Jungle Jim | BY | 56781 | 85 |
| [Skeleton Elite](https://sketchfab.com/3d-models/none-600f57d8c7ba43e3ba5f8be8f43b90d5) | Monhoo | BY | 118677 | 78 |
| [stone gate](https://sketchfab.com/3d-models/none-d88f01152b924d40908a61e17707aa88) | minaivalentin | BY | 6316 | 78 |
| [Dread Roach Rig](https://sketchfab.com/3d-models/none-2a83106e075042489250b5fe0e29e93c) | NO DONT EAT ME CASEOH (Ferris wheel) | BY | 48855 | 75 |
| [Wolf netsuke](https://sketchfab.com/3d-models/none-9eae8ee8f985439584aa063e5fe33f87) | Minneapolis Institute of Art | CC0 | 64000 | 73 |
| [Animated Peacock Moth](https://sketchfab.com/3d-models/none-1c3db0798f1c46be9764e99631cbb0a7) | Osian CG | BY | 61488 | 72 |
| [The Hooded Raven - Character](https://sketchfab.com/3d-models/none-9c1c4a8553fb48149ec6561ed6d800fb) | Mark Peters | BY | 344830 | 71 |
| [knife raven-3](https://sketchfab.com/3d-models/none-efe242b8e6934b75a15bc6af7e350676) | John Machine | BY | 1996 | 71 |
| [Low Poly Crow](https://sketchfab.com/3d-models/none-2e24c954890d4616adc990ba2fdf6c62) | ClintonAbbott.Art | BY | 6272 | 70 |
| [Johann Wolfgang von Goethe COVID-19](https://sketchfab.com/3d-models/none-99d6b24e3ccb40c8a86111f4f2acf855) | noe-3d.at | CC0 | 473404 | 69 |
| [Giant Weta / Wētāpunga](https://sketchfab.com/3d-models/none-b1059de0b5e847a7870fd97ecc3d0b3a) | Auckland Museum | BY | 57724 | 56 |
| [Free Skeleton Warrior Arrow](https://sketchfab.com/3d-models/none-2a57d9dd2966407c8b8fa753f76e3bdb) | Alien Haircut (NHI 377) | BY | 13131 | 48 |
| [CC0 キメンガニ 🦀 ♀ Demon-face Crab, Dorippe sinica](https://sketchfab.com/3d-models/none-86ea61d81d114e728033af5e27f8ef3e) | ffish.asia / floraZia.com | CC0 | 444276 | 44 |
| [Head of a Deva, 1181-1218 CE](https://sketchfab.com/3d-models/none-053ca510913d44cb9c9b3d4c0fc543a1) | Minneapolis Institute of Art | CC0 | 64000 | 41 |
| [CC0 キメンガニ 🦀 ♂ Demon-face Crab, Dorippe sinica](https://sketchfab.com/3d-models/none-fb8afe1c87e146aebe48faa75eeb9a7d) | ffish.asia / floraZia.com | CC0 | 480478 | 37 |
| [Raven Fracas](https://sketchfab.com/3d-models/none-d38d2a652f7a4420999cee647cfab030) | tamminen | BY | 9205 | 37 |
| [Ale for an Orc slayer!](https://sketchfab.com/3d-models/none-67a93745c5d14659ba862ce7a61df56b) | austeja | BY | 102614 | 37 |
| [🐦‍⬛ Tiny Bird](https://sketchfab.com/3d-models/none-63e5301d2a1647989196700cac11fd06) | Jazz Vincent | BY | 2650 | 36 |
| [Stylized Sword [Eye of the crow]](https://sketchfab.com/3d-models/none-74ff52c46c574eec9f82ec96800deb47) | zefgm | BY | 2998 | 35 |
| [Mountain Orge](https://sketchfab.com/3d-models/none-f18e6fb4a07b445095b5fcb5bec7fa7a) | SamThePie | BY | 17309 | 34 |
| [Troll bust (Nomad test 3)](https://sketchfab.com/3d-models/none-c074ee24fc0946c98fcf3158d5111495) | Orpind | BY | 190238 | 29 |
| [Murder Drones OC: Adam (Eldritch Form)](https://sketchfab.com/3d-models/none-53dba5e7b7f1470da5dae7676a5a918c) | bowseristheking | BY | 512266 | 29 |
| [Cave Troll](https://sketchfab.com/3d-models/none-ff197d0a91594be49b11b8eb0b1ffdde) | toro ardido modelos 3d | BY | 13707 | 28 |
| [Shoki, test scan 2](https://sketchfab.com/3d-models/none-ef37940ffbf7432ea4e4e8dc43b7cdf3) | Minneapolis Institute of Art | CC0 | 64000 | 27 |
| [Shrek 5 (Movie)](https://sketchfab.com/3d-models/none-83994aa492e8433c8c1dbe4f32dbbb92) | Artalasky | BY | 141907 | 24 |
| [Half of a twin capital with a griffin motif](https://sketchfab.com/3d-models/none-04045f7e2a874ea6a1d9e3a451ec78ad) | Virtual Museums of Małopolska | CC0 | 200000 | 23 |
| [Trough](https://sketchfab.com/3d-models/none-e70e546c4a0a42b98a8d4bc903fb20ea) | PropShop™ | BY | 4274 | 23 |
| [mountain Troll (myth)](https://sketchfab.com/3d-models/none-71ab09b11489411ea73c355c50e07894) | toro ardido modelos 3d | BY | 11142 | 21 |
| [Dark Young - Overlord (Rigged & Animated) .](https://sketchfab.com/3d-models/none-3c4975d1bf604c0f9fe91623634a4f5c) | Hank657 | BY | 29524 | 21 |
| [Black giant cicada (Cryptotympana facialis)](https://sketchfab.com/3d-models/none-f6c21330796549eca4f5f12aa8415d2f) | butterflybliss | BY | 31875 | 18 |
| [Cave troll (old)](https://sketchfab.com/3d-models/none-2748268f761b483c962c522a60686ebe) | toro ardido modelos 3d | BY | 21102 | 16 |
| [Troll Сannibal](https://sketchfab.com/3d-models/none-b70ae5e699434926a3ab8a97e08b8e9b) | Kazimir | BY | 990 | 16 |
| [olog-hai ( troll / ogre )](https://sketchfab.com/3d-models/none-458d55fb7b464b68a6bc450eaeb30ed9) | toro ardido modelos 3d | BY | 9929 | 15 |
| [spiky armored creature](https://sketchfab.com/3d-models/none-5394f37205d1417f9b949da3d0997b9b) | blaerov | BY | 40000 | 9 |
| [Ancient Bronze Cthulhu Artifact](https://sketchfab.com/3d-models/none-fc229b393319446dae3a8c69916c4828) | INGSOC1984 | BY | 8307 | 8 |
| [Cthulhu Stylized Sword 1](https://sketchfab.com/3d-models/none-83336e0b6f904c0e8afe4c1ce6ecf8bb) | UC-Rajat | BY | 16788 | 8 |
| [Myth Total War Model / Peasant Militia](https://sketchfab.com/3d-models/none-8fc26c7d593a469b9a1cdecba08b052c) | jsandor | BY | 19936 | 6 |
| [Cthulhu Stone Gargoyle - Eldritch Monolith](https://sketchfab.com/3d-models/none-813612f6d2cb496c8a8b46faeba6af3a) | INGSOC1984 | BY | 9720 | 4 |
| [DAE Diorama retake-Small farm](https://sketchfab.com/3d-models/none-781253e277654dd3835f02fb3ea1cf17) | marcu_andrei | BY | 383267 | 3 |
| [Decorated Egg - D8031](https://sketchfab.com/3d-models/none-a4d1db1dffcd4113949e6d1f53478524) | Institutul Național al Patrimoniului | BY | 18268 | 1 |
| [MOR-E 62 PIETA](https://sketchfab.com/3d-models/none-a0e28d338cc84b3fa814fef171b6d45f) | Muzeum im. Władysława Orkana w Rabce-Zdroju | BY | 459928 | 1 |

## props

| Модель | Автор | Лиц. | Фейсы | Лайки |
|---|---|---|---|---|
| [Free Pack - Rocks Stylized](https://sketchfab.com/3d-models/none-7c60b4d1b8ab4187965f30c5e0212fc0) | PolyOne Studio | BY | 3076 | 1700 |
| [Thor and the Midgard Serpent](https://sketchfab.com/3d-models/none-2ef4c45caa35450db1b876a7f94ff79d) | Mr. The Rich | BY | 44811 | 1619 |
| [Chevalier Sword](https://sketchfab.com/3d-models/none-b2662f2666a844e8a1bd0e7c4a7672d8) | rubenve | BY | 4002 | 1292 |
| [The tavern "Under the Falling Pigeon"](https://sketchfab.com/3d-models/none-f893c00f9fc34dde9e92870b3aea12f5) | Gnossiennes | BY | 412613 | 1230 |
| ["The Drunk Troll" Tavern](https://sketchfab.com/3d-models/none-6cdc0bdee2df457ebf9ac06a3a9910b8) | mrrobot | BY | 48728 | 1170 |
| [Jade Sword](https://sketchfab.com/3d-models/none-e17b04ace1f6446a9d7080ffd327fb4b) | Ole Gunnar Isager | BY | 8166 | 1149 |
| [medieval sword](https://sketchfab.com/3d-models/none-da574cba504e4b83a3293f3d3bd067fb) | LowSeb | BY | 862 | 1088 |
| [Roman Baths - free game asset](https://sketchfab.com/3d-models/none-ecc5e51b577a48b498dfcc87ee41f779) | Andy Woodhead | BY | 44016 | 925 |
| [Katanas](https://sketchfab.com/3d-models/none-faf3427097d84f69851ee507a2f8eff9) | LP Cupcake | BY | 22056 | 904 |
| [Throne](https://sketchfab.com/3d-models/none-bfc6d2989bb0411d9572733d35c3e6d1) | Nils Folker | BY | 11559 | 830 |
| [Fountain with three putts, Brno Lužánky](https://sketchfab.com/3d-models/none-430b9e5354484860b8b41505e9b63298) | 3dhdscan | BY | 89129 | 769 |
| [Toilet](https://sketchfab.com/3d-models/none-132a8ee2af3a40d39d270fbed3d3666c) | HippoStance | BY | 10492 | 712 |
| [Jon Snow](https://sketchfab.com/3d-models/none-2e94632116d442d7ab8882b6d493e74f) | Nephlim | BY | 994 | 712 |
| [Dungeon Gateway](https://sketchfab.com/3d-models/none-5a58a8821a5b47cda2710a8fe39751f7) | josevega | BY | 401805 | 652 |
| [Throne Room](https://sketchfab.com/3d-models/none-247c37e6a9694ae891398911876f0c14) | Uğur Yakışık | BY | 69420 | 633 |
| [Bar bar](https://sketchfab.com/3d-models/none-818254b878dd46dda5566d2ae88f84da) | anDDDres | BY | 227718 | 599 |
| [Low-poly medieval tavern](https://sketchfab.com/3d-models/none-0ceb0634c89349b6bec27d19228835e9) | Igor Rogov | BY | 61285 | 598 |
| [Temple Of Augustus Ruins](https://sketchfab.com/3d-models/none-a974316de2454609aa841790844e3571) | Abby Gancz | BY | 550135 | 589 |
| [Medieval Arcade](https://sketchfab.com/3d-models/none-58aee16f99934d6590a48ab178a0b852) | dark_igorek | BY | 247245 | 550 |
| [Environment Taverna Low Poly GameReady assets](https://sketchfab.com/3d-models/none-5249d045489941d981f4bc92a84b6877) | Kondyba | BY | 70537 | 540 |
| [Old Well with Hanging Bucket](https://sketchfab.com/3d-models/none-7fa29793eff14a7587ee0af15446a675) | Glowbox 3D | BY | 7286 | 518 |
| [Low poly bar table and booths set](https://sketchfab.com/3d-models/none-c2509f5213f84197ae00ab058877e5fc) | anDDDres | BY | 7620 | 515 |
| [Tavern. Day 5. FINAL[XYZ School DAILY CHALLENGE]](https://sketchfab.com/3d-models/none-c2f65086eac24c7c86fdb77ea8a418b4) | _SeF_ | BY | 195637 | 505 |
| [Medieval house h2 (Annielihta)](https://sketchfab.com/3d-models/none-5aacf0f8821d424bb31a2e6e7166eb4e) | Annielighta | BY | 80720 | 476 |
| [Medieval wooden cart](https://sketchfab.com/3d-models/none-c4ee19920e9b45aaa6ebe37839b1ad2a) | rakutin | BY | 11388 | 454 |
| [A Welcoming Sign](https://sketchfab.com/3d-models/none-10cc6cfc09db45d5b3ddb76ecd758ef5) | Raneman | BY | 14377 | 441 |
| [The Haunted Altar](https://sketchfab.com/3d-models/none-b4875b6700e24698b82a84a4fe3c9a93) | sdvtljkrgv | BY | 5079 | 434 |
| [Throne](https://sketchfab.com/3d-models/none-2f7ccdc939f34482b9c313c1a4c13ed6) | CrazyMoralFreak | BY | 121287 | 426 |
| [Set of daggers](https://sketchfab.com/3d-models/none-7fa10fcbb1f8401c97b3727e2af7406d) | Peter Nox | BY | 8329 | 411 |
| [Dragon Broadsword with Cyan Hilt](https://sketchfab.com/3d-models/none-b9b4a6cff67642dcb204ca0a146d421a) | DailyArt | BY | 66396 | 411 |
| [Sword](https://sketchfab.com/3d-models/none-39e4920c62a34b37a7b5b2f346210c8f) | vedoxox | BY | 10787 | 407 |
| [Crab Snap | DAE Weaponcraft Assignment](https://sketchfab.com/3d-models/none-a9c396e7e72c425ebcb4b9f5bcf599fa) | MeganVeldt | BY | 3544 | 405 |
| [Medieval Cart](https://sketchfab.com/3d-models/none-5dedd347d77943729475486f0d1d05e8) | Peter Nox | BY | 4165 | 394 |
| [Minas Tirith Throne Room Test v1](https://sketchfab.com/3d-models/none-485105dd3d9243c3a94e2e553a65e5a7) | Michael Ironstone | BY | 237144 | 392 |
| [Old Cart](https://sketchfab.com/3d-models/none-f536745fd82144eeb5cc4900a48e816e) | Lyskilde | BY | 17506 | 383 |
| [Old Sword](https://sketchfab.com/3d-models/none-54e2d050ff7c443d8a8d97cc804bd554) | KIFIR | BY | 141788 | 349 |
| [Gallows](https://sketchfab.com/3d-models/none-b496d60155cb4b2fb5864c3e2c68a0e2) | DJMaesen | BY | 3474 | 349 |
| [Rustic Fireplace](https://sketchfab.com/3d-models/none-555ad0c9171e4e4191ad7aea96ab44dd) | Loïc Norgeot | BY | 49378 | 348 |
| [Tombstone Familie Laurenz](https://sketchfab.com/3d-models/none-805187f02f8e424cb11b4f7c3a63a1c3) | 3dhdscan | BY | 72315 | 345 |
| [Stylized medieval cart with a lamp](https://sketchfab.com/3d-models/none-e2889f4cb4e64729b00e2cecf1425d30) | 3D_Kod | BY | 46794 | 341 |
| [Viking Ship](https://sketchfab.com/3d-models/none-beff0922dbd6415dbe7c4aafc2f51f12) | emelyarules | BY | 39006 | 323 |
| [Medieval Fayre - free game assets](https://sketchfab.com/3d-models/none-595624e5bd7543fe968bd8b2e3280196) | Andy Woodhead | BY | 65436 | 318 |
| [Stone Altar](https://sketchfab.com/3d-models/none-3d4f2edb18e4424cabb41b60aa160cb5) | TheoClarke | BY | 3132 | 316 |
| [Medieval wooden cart](https://sketchfab.com/3d-models/none-c64c942f09a14d15bb4440a9f75a0d9f) | rakutin | BY | 39270 | 309 |
| [Medieval Door](https://sketchfab.com/3d-models/none-4d8b766cc80344cdb95e3a0473e13bb9) | Krishna Bros | BY | 47503 | 302 |
| [Medieval: Random Weapons](https://sketchfab.com/3d-models/none-f019062c04504edda84ac60bd0e1d654) | Daniel | BY | 86671 | 293 |
| [Day1_tent](https://sketchfab.com/3d-models/none-f6da1cc219194a35a8cf1df904261a88) | Muru | BY | 741 | 293 |
| [Balista](https://sketchfab.com/3d-models/none-03022ca891e3440d815178003a466302) | Urizel | BY | 30262 | 284 |
| [Siege at night #MedievalRemixChallenge](https://sketchfab.com/3d-models/none-885f519b5a2e417c850736d3d9cd0ef3) | Dyson | BY | 531962 | 277 |
| [Moonbrand (Early 14th C Arming Sword)](https://sketchfab.com/3d-models/none-80fa91fc8c974fd0acd2a55cc513edec) | The Oakeshott Institute | BY | 44864 | 271 |
| [Top down low poly medieval camp](https://sketchfab.com/3d-models/none-65ccfee9ef124bd98f7c402c485e9049) | rakutin | BY | 117798 | 262 |
| [Knight Hospitaller - 3d printable](https://sketchfab.com/3d-models/none-7975ee5c47a94d35abf98070bfdd1ea8) | Andy Woodhead | BY | 349360 | 258 |
| [Rossbändiger](https://sketchfab.com/3d-models/none-dd74015d1eec43d0a01ada66a8e2ff87) | noe-3d.at | BY | 486920 | 257 |
| [Good Ol' Well](https://sketchfab.com/3d-models/none-9eadcb31e4b445c8978e791fcce548fe) | Mik Santos | BY | 27074 | 252 |
| [Stone well](https://sketchfab.com/3d-models/none-885951c7fc0646f4bb5b136475ee2f6c) | Lyskilde | BY | 7888 | 251 |
| [Siege Ballista](https://sketchfab.com/3d-models/none-8ca1e906cb10447e81aca3b78472f5f8) | Hjorskrim | BY | 17540 | 251 |
| [Tent](https://sketchfab.com/3d-models/none-441936f01497461cb226624da9a168db) | Arkikon | BY | 16588 | 220 |
| [Crossbow](https://sketchfab.com/3d-models/none-34cba117b05e4fed992f5700de9bb627) | Charlie Tinley | BY | 9033 | 220 |
| [Stylized Tombstones](https://sketchfab.com/3d-models/none-44b8e3ce8d0842b395d80c5b76bd5d7e) | George Hager | BY | 1268 | 216 |
| [Fluorescent Lamp/Light - 4096px²](https://sketchfab.com/3d-models/none-af6ccbb048594b848f2af232307cab60) | Mark Peters | BY | 81224 | 208 |
| [Old gallows](https://sketchfab.com/3d-models/none-e9d235ecfc974e44b1caa97b0980e157) | adam127 | BY | 624 | 207 |
| [Medieval knight](https://sketchfab.com/3d-models/none-1629b56db814401aaf7f3868f0577472) | 3dUVpro | BY | 30000 | 202 |
| [Halloween graveyard pack](https://sketchfab.com/3d-models/none-9d2ba5dfc9ec401b87e88fd121bc249c) | rudolfs | BY | 47619 | 197 |
| [Siege camp scene](https://sketchfab.com/3d-models/none-b8d6beb66c5a41c380269d51470e9f75) | BlackSpire | BY | 50525 | 195 |
| [Gallows - 28 MB](https://sketchfab.com/3d-models/none-8ec823c0cd7d49f19a39a55f3fdb0bab) | Mehdi Shahsavan | BY | 9846 | 191 |
| [Old Gallows](https://sketchfab.com/3d-models/none-3aeb58b4568a482ca524773452ff6e65) | gozdemrl | BY | 19548 | 189 |
| [O-zutsu Teppo | Feudal Japanese Hand Cannon |](https://sketchfab.com/3d-models/none-a8767054fa40471ea451ec651335c58c) | PeaPufferFish | BY | 12602 | 187 |
| [Old City](https://sketchfab.com/3d-models/none-c5c665646e6d44eeb43fe8ab2c868acc) | loriserio | BY | 409950 | 180 |
| [Goddess Sekhmet, 1320-656 BCE](https://sketchfab.com/3d-models/none-160b5a18fa994fcdb4bef96e5c1f1c4a) | Minneapolis Institute of Art | CC0 | 62032 | 178 |
| [Candles and Tombstones](https://sketchfab.com/3d-models/none-4606c83c372046cb92359a568e0e6d99) | N01516 | BY | 37284 | 177 |
| [Hound - Rainbow Six Siege Character Concept](https://sketchfab.com/3d-models/none-4906191118f742c581fcc782a1e8005c) | Jamie McFarlane | BY | 49194 | 177 |
| [Ballista](https://sketchfab.com/3d-models/none-ac83491459e944b6bf3257931c06f707) | iedalton | BY | 2836 | 173 |
| [Gallow 1](https://sketchfab.com/3d-models/none-8891d98ca9e14f22b6a35b1969916dc3) | sharetextures | BY | 19522 | 167 |
| [Hangman](https://sketchfab.com/3d-models/none-53cd38b7e02549caa262c5683b765202) | flewfatoo | BY | 416906 | 166 |
| [Chinese Junk Ship](https://sketchfab.com/3d-models/none-35b340bce9fb4e0680bc0116cebc35c9) | pinkycolada | BY | 205256 | 166 |
| [Gallows B](https://sketchfab.com/3d-models/none-7e3fb43ce01d4b8d89967be8375c2d04) | DJMaesen | BY | 3178 | 163 |
| [Exalted Beast](https://sketchfab.com/3d-models/none-e5e7b3c9fea944c18d235a15ab8d8483) | Spinnee | BY | 13020 | 161 |
| [Tombstones](https://sketchfab.com/3d-models/none-047c28dfd28f4f9eb630bfd42dfeb910) | Tiago Lopes | BY | 924 | 160 |
| [Ballista](https://sketchfab.com/3d-models/none-9f3c81860edb4bd595f613ccd6e01e8e) | Denis Moeller | BY | 4972 | 159 |
| [Low poly camp](https://sketchfab.com/3d-models/none-6f75ecd05f43418cbddbc6bd1e5fd692) | lukat33 | BY | 6612 | 147 |
| [Pirate Boat](https://sketchfab.com/3d-models/none-8152a12db067492b8179f86e02cf2cb9) | local.yany | BY | 39592 | 146 |
| [Modern Camping Tent](https://sketchfab.com/3d-models/none-3f624d58d582430595179bdf9a156be3) | Guy in a Poncho | BY | 35880 | 143 |
| [Stage](https://sketchfab.com/3d-models/none-57b58ad84f0e43a8986e567d72413345) | local.yany | BY | 5248 | 134 |
| [Winter Fantasy Assets Daggerfall Misc](https://sketchfab.com/3d-models/none-66f94552938747d7b54b49116ebcf980) | Anticipateants | BY | 59176 | 134 |
| [Crystal Lich Tombstone](https://sketchfab.com/3d-models/none-1b200ae8de834c5e9ac7d65db4cedc12) | Batuhan13 | BY | 901 | 134 |
| [Fountain - low-poly](https://sketchfab.com/3d-models/none-36bb9a6e62ab4b88b7154c41489b5f41) | timeframes | BY | 43981 | 132 |
| [Camel's Respite](https://sketchfab.com/3d-models/none-a7f8fba0bf4b41acb3bf77b8f9c8cd9e) | Baue Franco | BY | 24164 | 129 |
| [Rapier of D. João de Mascarenhas](https://sketchfab.com/3d-models/none-cd753effd9b0465b8ab84d6003fe7acd) | Morbase | Museu Virtual | CC0 | 154112 | 126 |
| [Gemstone Necklace](https://sketchfab.com/3d-models/none-c3ef2c8fad514b69bd3d7180b84cb25f) | randy.s.almeida | BY | 23184 | 126 |
| [Medieval Triple Gallows](https://sketchfab.com/3d-models/none-82541ecb325548dc9be062981df6887b) | Jungle Jim | BY | 7658 | 124 |
| [Stylized Tomb](https://sketchfab.com/3d-models/none-c5de58f39dee4e3fb5e8896b3a2f80c6) | Wah Marco | BY | 2528 | 123 |
| [Gravestone on Augsburg Westfriedhof graveyard](https://sketchfab.com/3d-models/none-6c13a74722d34f67a84c0d6b14f0e935) | Nexilion | BY | 127999 | 116 |
| [Beach Low Poly (Cartoon) FREE](https://sketchfab.com/3d-models/none-b60517a9b8124bd79244c010badcf224) | Mnydd | BY | 39542 | 116 |
| [Jungle Tent](https://sketchfab.com/3d-models/none-52332b0f55f24c739215342954e49f54) | SyntheticMN | BY | 958 | 107 |
| [Ship Wheel](https://sketchfab.com/3d-models/none-eda936bf1f0a4b2b8adb8548cb92f880) | demidrew | BY | 17796 | 105 |
| [SCI FI TRON (with astronaut rigged)](https://sketchfab.com/3d-models/none-f92905f918d848c284559623b6066beb) | Levion | BY | 365782 | 104 |
| [Camping Tent](https://sketchfab.com/3d-models/none-253c008a734e465982f8324a1aad0eac) | aniketranjan175 | BY | 266248 | 100 |
| [Votive stele - Buddha and two Bodhisattvas](https://sketchfab.com/3d-models/none-e97f95d5781a4359b5a744b0cf0328fa) | Minneapolis Institute of Art | CC0 | 64000 | 98 |
| [Medieval Gallows](https://sketchfab.com/3d-models/none-e8c6bfa83d6f4471b19bdee73a6ee997) | AspectStudios | BY | 70968 | 93 |
| [Western Prop 2 - Animated - Gallows](https://sketchfab.com/3d-models/none-92965281dda745bebd53668842645add) | Chad Green | BY | 28748 | 93 |
| [Tankard with a biblical scene](https://sketchfab.com/3d-models/none-d968b14919544a7e81dbe6802049faae) | Virtual Museums of Małopolska | CC0 | 240251 | 91 |
| [Fantasy Fishing Boat](https://sketchfab.com/3d-models/none-bf7d68cb9db5469a9895064de1499d68) | eb78 | BY | 54627 | 80 |
| [Hollow Knight_Statue Test](https://sketchfab.com/3d-models/none-7fb678c884f7491c96824f9ac45dea58) | Enrico Labarile | BY | 56211 | 75 |
| [Monstergarden](https://sketchfab.com/3d-models/none-9397658152d243ab9f22c619fad0293e) | iKnewt | BY | 16914 | 70 |
| [Gallow](https://sketchfab.com/3d-models/none-646fbf6dbb004c7595ac16c1bbdd60b0) | Jordi | BY | 3384 | 68 |
| [Low Poly Camp Fire Scene](https://sketchfab.com/3d-models/none-299a29f60a6b43c5b5bf33699498f128) | OscarWW | BY | 10900 | 67 |
| [Armored Maiden 3D improvement for Gmod](https://sketchfab.com/3d-models/none-1d52d5d76a274272a90b50dd58620e64) | JORNO NANA | BY | 48765 | 66 |
| [2024.140 Ceremonial Chair or Throne](https://sketchfab.com/3d-models/none-d72f5cf4c8e24daca0de504f342b8cf1) | Cleveland Museum of Art | CC0 | 255386 | 61 |
| [Boat](https://sketchfab.com/3d-models/none-60891d2c56e4448b9c1379bd7cb2b0e8) | Arsen.Krityan | BY | 246184 | 50 |
| [Hero Longboat](https://sketchfab.com/3d-models/none-47f4963f0aad4654b0513416360d3319) | local.yany | BY | 503996 | 44 |
| [Arkham Detective - Cage Chain Lock](https://sketchfab.com/3d-models/none-e91da059e4104b87af7bec5bbfa7269d) | stianb93 | BY | 12022 | 42 |
| [Hearth Cage Key Chain](https://sketchfab.com/3d-models/none-6612a18cd3c7417fb0637856bb43f574) | ArnieRosemary | BY | 16288 | 42 |
| [Iron Age stone lamp](https://sketchfab.com/3d-models/none-934bd78c490140198db542aee7f765fa) | The Hunt Museum | CC0 | 199146 | 36 |
| [Low-Poly Pirate Ship](https://sketchfab.com/3d-models/none-8f204fde27cf43aeacdaef550799591c) | Chenuka Wijesundara | BY | 11062 | 33 |
| [Piece of a Byzantine figurine](https://sketchfab.com/3d-models/none-99ab719e4c6c4c68b66bdcd70e502aa7) | Virtual Museums of Małopolska | CC0 | 49904 | 30 |
| [Gravestone of Abraham Aba](https://sketchfab.com/3d-models/none-6caf12e9427d45cc84d9ffbf4f4a22d7) | Virtual Museums of Małopolska | CC0 | 201728 | 30 |
| [Renaissance Hanging Banner](https://sketchfab.com/3d-models/none-81ce658d58494768b98a31e9bf1157d3) | Sunbox Games | BY | 14112 | 27 |
| [Chalice - Historic Environment Scotland](https://sketchfab.com/3d-models/none-74ca225e011a4cfd9336fb49e3f92dfc) | leepriestley | BY | 49632 | 21 |
| [Pack of 5 colored Flags](https://sketchfab.com/3d-models/none-aabfa0a089ae4df0a3830bf7a0acfca6) | 3000volt | BY | 9718 | 21 |
| [Figure Sculpture (Sedes Sapentia)](https://sketchfab.com/3d-models/none-819f51e0724e41119b1c126ce08f6396) | The Hunt Museum | CC0 | 235227 | 21 |
| [Bronze Dragon Incense Urn (Scaniverse)](https://sketchfab.com/3d-models/none-6a9f88e1779e44ec8cd265af071b02c7) | JFN | BY | 215275 | 17 |
| [Past Glory Buggy - PUBG MOBILE](https://sketchfab.com/3d-models/none-ba6d900b85e244f8ae485ba3be39502b) | Kenny3D89 | BY | 13492 | 17 |
| [A tombstone with the image of a “tree of life”](https://sketchfab.com/3d-models/none-d9ac279d0a9c424393579caf1f68843e) | Virtual Museums of Małopolska | CC0 | 73669 | 16 |
| [El Catrin 4 - t pose - Day of the Dead](https://sketchfab.com/3d-models/none-ea485d00b01842aba9a18d4007d789c4) | Brian Trepanier | BY | 410102 | 15 |
| [Evil Cultist Altar – Stylized 3D Game Prop](https://sketchfab.com/3d-models/none-cefcf8a1797f490a892faef537d48f9d) | jonas_hilschmann | BY | 498709 | 15 |
| [GA006-005001 (Megalithic Tomb) & GA006-005 CBG](https://sketchfab.com/3d-models/none-17a3d7ed11494847a832ff2bf61eaf0b) | Galway3D_DH_AGe | CC0 | 573047 | 15 |
| [Occult Lair](https://sketchfab.com/3d-models/none-a18c7067ef6c4ed6b8933411f52bcd02) | JayWoodo | BY | 16910 | 13 |
| [Game-Ready Stylized Cultist Shrine](https://sketchfab.com/3d-models/none-2f124b1684174120b35672dcd5a53bff) | celebornArt | BY | 3904 | 11 |
| [Figure Sculpture (Sedes Sapentia)](https://sketchfab.com/3d-models/none-c2a6d459a06440f7a5adf87227edefbb) | The Hunt Museum | CC0 | 124656 | 11 |
| [Ritual Altar - Game Asset](https://sketchfab.com/3d-models/none-cc4da902a27b4220bc6870ed48ad0e86) | ruspeloke | BY | 3068 | 10 |
| [Stony Brennan - Carved head](https://sketchfab.com/3d-models/none-af7811cb738b455bbf891a3a36012920) | Galway3D_DH_AGe | CC0 | 136150 | 9 |
| [Medieval banner](https://sketchfab.com/3d-models/none-0a87672db9ef4059ae559d25f199baf2) | ejtamovic | BY | 2468 | 9 |
| [Neolithic Ground and Polished Axehead](https://sketchfab.com/3d-models/none-d95445865b184fe6855886f836c93408) | The Hunt Museum | CC0 | 400200 | 7 |
| [Toorlectra- Sweathouse Internals](https://sketchfab.com/3d-models/none-55e7e1b04cba4cb38c94e89ed8c4c594) | Galway3D_DH_AGe | CC0 | 10585 | 7 |
| [Low Poly Red Flag - Wooden Pole Banner](https://sketchfab.com/3d-models/none-8d52d2408ef84046bc0a0f6102ec41be) | marishka1611 | BY | 296 | 5 |
| [A lion](https://sketchfab.com/3d-models/none-5bbf13c53e674bc9a722dbc9ca4f6aaa) | Virtual Museums of Małopolska | CC0 | 150000 | 5 |
| [Cross Street - Window Fragment 1](https://sketchfab.com/3d-models/none-d720852fd2c845ff9ddc9e08643a879c) | Galway3D_DH_AGe | CC0 | 87552 | 5 |
| [Thronendes Brettidol](https://sketchfab.com/3d-models/none-30a32f1ea6b844b8921a6d503e19b033) | Museum für Kunst und Gewerbe Hamburg | CC0 | 500000 | 4 |
| [KE037-022 - KNOCKGLASS MORE - Rock Art](https://sketchfab.com/3d-models/none-58e5a49255f34ceea69e84a6251fa5e1) | Kerry3D DH_Age | CC0 | 256729 | 4 |
| [Carnfree - Standstone 3](https://sketchfab.com/3d-models/none-a56b296833384dc4af411cedae5a0701) | Roscommon3D DH_AGE | CC0 | 535921 | 2 |
| [Grafsteen van Gumattius uit Dodewaard](https://sketchfab.com/3d-models/none-393410cdb6404797a2e9853ea85558bb) | The National Museum of Antiquities | CC0 | 599999 | 2 |
| [Grabstein mit kufischer Inschrift](https://sketchfab.com/3d-models/none-c4e81d468ed948fd850ac544fb280bd5) | Museum für Kunst und Gewerbe Hamburg | CC0 | 79999 | 1 |
| [KE036-058 - Bullan - FOILATRISNIG](https://sketchfab.com/3d-models/none-a4b02076a2af4d529e79be6fa61b6b88) | Kerry3D DH_Age | CC0 | 188167 | 1 |
| [Tank](https://sketchfab.com/3d-models/none-e696c9ff13fa49f1bc99a44500712d32) | Balsamic_Vin | BY | 393702 | 1 |
| [Metal Cage](https://sketchfab.com/3d-models/none-26202a089ff1482c819b67e98e7347c5) | parth8968 | BY | 1246 | 1 |
| [Cages and Chains](https://sketchfab.com/3d-models/none-5219ceda0c8b4fb684f1458cf29b47fb) | the1987redfox | BY | 19112 | 1 |
| [Shield, Banner, and Chair Props](https://sketchfab.com/3d-models/none-c30008193e384c5f97ecfe4fa1bd3a2f) | ljroberts | BY | 2432 | 0 |
| [Iron Cage](https://sketchfab.com/3d-models/none-ce6bfcc885c14f648fa3956f03ee86b5) | Emanon | BY | 231136 | 0 |
