import sys, io, base64, json, html
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from PIL import Image
import tiles, nature, build, decor, transport, cliffs, icons, export, scenes
from export import to_rgba

src = {}
TL = tiles.all_tiles(); NA = nature.all_nature(); BU = build.all_pieces(); DE = decor.all_decor()
TR, META = transport.all_transport(); CL = cliffs.all_cliffs(); IC = icons.all_icons()
for d in (TL, NA, BU, DE, TR, CL, IC): src.update(d)
E = {k: export.entry(v) for k, v in src.items()}

def png(im):
    b = io.BytesIO(); im.save(b, 'PNG', optimize=True); return 'data:image/png;base64,' + base64.b64encode(b.getvalue()).decode()

def img(name, bg=None):
    px = to_rgba(E[name]); h, w = len(px), len(px[0])
    im = Image.new('RGBA', (w, h)); im.putdata([c for r in px for c in r])
    if bg is not None:
        g = Image.new('RGBA', (w, h))
        for y in range(0, h, 48):
            for x in range(0, w, 48): g.alpha_composite(bg, (x, y))
        g.alpha_composite(im); im = g
    return im

def tileimg(n):
    px = to_rgba(E[n]); im = Image.new('RGBA', (48, 48)); im.putdata([c for r in px for c in r]); return im

def tile2(n):  # 2x2 repeat to show the seam
    t = tileimg(n); im = Image.new('RGBA', (96, 96))
    for x in (0, 48):
        for y in (0, 48): im.alpha_composite(t, (x, y))
    return im

GR = tileimg('tl_grass'); DI = tileimg('tl_dirt'); SN = tileimg('tl_sand')
out = []
def fig(name, im, cap, scale=2, cls=''):
    out.append(f'<figure class="{cls}"><img src="{png(im)}" width="{im.width*scale}" height="{im.height*scale}" alt="{html.escape(cap)}"><figcaption>{html.escape(cap)}</figcaption></figure>')

def section(title, note, items, grid='g'):
    global out
    out.append(f'<section><h2>{html.escape(title)}</h2><p class="note">{html.escape(note)}</p><div class="{grid}">')
    for it in items: fig(*it)
    out.append('</div></section>')

# scenes
SC = [('med', 'Срединное море', 'Дом эллинов (стены из известняка с меандром, черепица), портик храма, алтарь, статуя, амфоры, треножник, кипарисы, олива, колесница, мощёная дорога, море.'),
      ('nile', 'Нил', 'Дом из сырцового кирпича с синей и золотой полосой, плоская крыша, пилон святилища, обелиск, сфинкс, пальмы, папирус, верблюд, папирусная лодка, ил поймы.'),
      ('forest', 'Средние леса', 'Славянская изба из бревна с резными красными точками, тёсовая крыша; кельтский дом из плетня с обмазкой под соломой; идол, колодец-журавль, священный дуб с лентами, береза, ель, телега.'),
      ('north', 'Север', 'Длинный дом из вертикальных досок под дёрном, гномья кладка с золотыми рунами и светом горна в окне, рунный камень, тур из камней, заснеженный обрыв, ладья, северный олень, сани.'),
      ('steppe', 'Восточная степь', 'Юрты (объектом, круглые), курган с каменным воином, балбал, шест с конским черепом и лентами, обо, кибитка, кони, ковыль.')]
out.append('<section><h2>Сцены по регионам</h2><p class="note">Всё в масштабе мира: 1 клетка = 1 м = 48×48 пикселей. Крыша срезана над южной половиной дома, чтобы видеть комнату (так окно показывает крыши). Пешек 48×64 рисует соседний тред, здесь их нет.</p><div class="scenes">')
for k, t, d in SC:
    im = dict(scenes.ALL)[k](E)
    out.append(f'<figure class="scene"><img src="{png(im)}" width="{im.width}" height="{im.height}" alt="{html.escape(t)}"><figcaption><b>{html.escape(t)}.</b> {html.escape(d)}</figcaption></figure>')
out.append('</div></section>')

TN = {'tl_grass':'трава','tl_grass2':'трава 2','tl_forest':'лесная подстилка','tl_marsh':'болото','tl_water':'вода','tl_sand':'песок','tl_dirt':'земля','tl_road':'дорога','tl_field':'пашня','tl_floor':'дощатый пол','tl_roof':'солома',
      'tl_med_grass':'сухая трава с тимьяном','tl_med_scrub':'каменистая земля','tl_med_paving':'мощение','tl_med_mosaic':'мозаика с меандром','tl_med_sea':'море','tl_med_roof':'черепица',
      'tl_nile_silt':'ил поймы','tl_nile_sand':'пустыня','tl_nile_floor':'сырцовый кирпич','tl_nile_river':'Нил','tl_nile_roof':'плоская крыша','tl_nile_reed_roof':'тростник',
      'tl_wood_moss':'мох и листва','tl_wood_meadow':'лесной луг','tl_wood_floor':'половицы','tl_wood_roof':'тёс',
      'tl_north_tundra':'тундра','tl_north_snow':'снег','tl_north_rock':'скала','tl_north_floor':'широкие доски','tl_north_sea':'северное море','tl_north_roof':'дёрн',
      'tl_steppe_grass':'ковыль','tl_steppe_dry':'сухая степь','tl_steppe_felt':'войлочный ковёр','tl_steppe_roof':'войлок юрты'}
section('Тайлы земли, полов и крыш', 'Каждый тайл показан 2×2, чтобы был виден стык. У частых видов земли по три варианта с общей кромкой: их можно перемешивать и поворачивать без шва.', [(n, tile2(n), TN.get(n, n)) for n in TL if not n[-2:] in ('_2', '_3')])

NN = {'ob_tree':'дуб','ob_tree_birch':'берёза','ob_pine':'ель','ob_pine_snow':'ель в снегу','ob_cypress':'кипарис','ob_olive':'олива','ob_palm':'финиковая пальма','ob_willow':'ива','ob_dead_tree':'сухое дерево','ob_sacred_tree':'священный дуб','ob_bush':'куст','ob_bush_berry':'ягодный куст','ob_rock':'камень','ob_rock_moss':'камень во мху','ob_rock_sand':'песчаник','ob_reeds':'камыш','ob_papyrus':'папирус'}
section('Деревья, кусты, камни', 'Дерево 3–4 м в высоту, ель 4 м. Свет сверху слева, мягкая тень на земле.', [(n, img(n, GR), NN.get(n, n), 1) for n in NA], 'g wide')

STN = {'hellenic':'эллинский','labyrinth':'минойский (лабиринт)','cyclopean':'циклопическая кладка','grove':'живая изгородь рощи','nilotic':'нильский сырец','sylvan':'эльфийское живое дерево','celtic':'кельтский плетень с обмазкой','slavic':'славянский сруб','norse':'северные доски','dwarven':'гномий гранит','cave':'пещерный камень','steppe':'войлочная стенка','lair':'логово'}
rows = []
for st in build.STYLES:
    W = Image.new('RGBA', (48 * 5 + 16, 48))
    for i, k in enumerate(('_h', '_door_h', '_window_h', '_v', '_post')):
        W.alpha_composite(DI, (i * 52, 0)); W.alpha_composite(img('wl_' + st + k), (i * 52, 0))
    rows.append((st, W, STN[st] + ' (' + st + ')'))
section('Модульные стены по стилю постройки', 'Стиль = build_style культуры. В ряду: стена вдоль x (видна верхушка и южная грань), дверь, окно, стена вдоль y, угловой столб. Толщина 25 см.', rows, 'g walls')

DN = {'ob_bed':'кровать','ob_table':'стол','ob_chest':'сундук','ob_barrel':'бочка','ob_sack':'мешок','ob_hearth':'очаг','ob_anvil':'наковальня','ob_well':'колодец','ob_stall':'прилавок','ob_block':'колода','ob_firewood':'поленница','ob_log':'бревно','ob_pen':'изгородь','ob_grave':'могила','ob_rubble':'обломки','ob_wreck':'остов','ob_shrine':'домашний алтарь','ob_frame':'рама','ob_well_mud':'колодец (Нил)','ob_well_marble':'колодец (мрамор)','ob_well_log':'колодец (сруб)','ob_grave_stone':'могила с камнем','ob_stall_blue':'прилавок 2',
      'st_forge':'горн','st_oven':'печь','st_campfire':'костёр','st_bench':'верстак','st_loom':'ткацкий станок','st_wheel':'гончарный круг','st_tub':'чан','st_pot':'котёл','st_jar':'пифос','st_rack':'сушилка','st_mortar':'ступа','st_quern':'жернов','st_grind':'точило','st_pit':'дубильная яма'}
section('Мебель и рабочие места', 'Одна клетка, вид 3/4: видна крышка и южная грань.', [(n, img(n, DI), DN.get(n, n)) for n in DE if n.startswith(('ob_', 'st_'))])
DCN = {'dc_med_temple':'портик храма','dc_med_altar':'алтарь','dc_med_statue':'статуя бога','dc_med_statue_bronze':'бронзовая статуя','dc_med_herm':'герма','dc_med_tripod':'треножник','dc_med_amphora':'амфора','dc_med_horns':'рога посвящения (Крит)',
       'dc_nile_obelisk':'обелиск','dc_nile_sphinx':'сфинкс','dc_nile_naos':'пилон святилища','dc_nile_column':'колонна-лотос',
       'dc_wood_idol':'идол','dc_wood_stone':'камень с огамом','dc_wood_dolmen':'дольмен','dc_wood_well_crane':'колодец-журавль',
       'dc_north_runestone':'рунный камень','dc_north_cairn':'тур из камней','dc_north_forge_shrine':'гномье святилище горна',
       'dc_steppe_balbal':'балбал','dc_steppe_kurgan':'курган','dc_steppe_totem':'шест с конским черепом','dc_steppe_ovoo':'обо','dc_steppe_yurt':'юрта'}
section('Святилища и памятники по регионам', 'Крупные вещи стоят на нескольких клетках: портик и обелиск 4 м, статуя 2 м.', [(n, img(n, SN if 'nile' in n else (GR)), DCN.get(n, n), 1) for n in DE if n.startswith('dc_')], 'g wide')

TRN = {'tr_horse':'конь','tr_pony':'пони','tr_donkey':'осёл','tr_mule':'мул','tr_ox':'вол','tr_war_wolf':'боевой волк','tr_riding_lizard':'ездовой ящер','tr_camel':'верблюд','tr_reindeer':'северный олень','tr_handcart':'тачка','tr_cart':'телега','tr_wagon':'фургон','tr_coach':'карета','tr_chariot':'колесница','tr_kibitka':'кибитка','tr_sled':'сани','tr_longship':'ладья','tr_galley':'галера','tr_reedboat':'папирусная лодка'}
section('Транспорт', 'Вид сбоку, мордой на восток (на запад зеркалится), по два кадра шага или поворота колёс. Здесь первый кадр.', [(n, img(n, GR), TRN.get(n, n), 1) for n in TR if not n.endswith('_1')], 'g wide')

CN = {'stone':'камень','mossy':'камень во мху','earth':'земля','sandstone':'песчаник','snow':'снег','limestone':'известняк (Срединное море)','basalt':'базальт (Север)'}
crow = []
for st in cliffs.STYLES:
    W = Image.new('RGBA', (52 * 6, 48))
    for i, k in enumerate(('_n_0', '_n_1', '_s_0', '_e_0', '_c_ne', '_ramp_n')):
        W.alpha_composite(GR, (i * 52, 0)); W.alpha_composite(img('cf_' + st + k), (i * 52, 0))
    crow.append((st, W, CN[st]))
section('Обрывы рельефа', 'В ряду: обрыв на север (2 варианта, стыкуются), бровка юга, бок, угол, ступени дороги. Две новые породы: известняк и базальт.', crow, 'g walls')

section('Иконки предметов', '24×24, тот же свет и цветной контур. Имена прежние, ссылки в данных не меняются.', [(n, img(n), n[3:], 3) for n in IC], 'g icons')

body = '\n'.join(out)
page = open('' + __import__('os').path.join(__import__('os').path.dirname(__import__('os').path.abspath(__file__)), 'review_tpl.html') + '').read().replace('<!--BODY-->', body)
open((sys.argv[1] if len(sys.argv) > 1 else 'assets_lr167.html'), 'w').write(page)
print(len(page) // 1024, 'KB')
