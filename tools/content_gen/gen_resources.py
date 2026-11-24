#!/usr/bin/env python3
"""Resources pack (epic AL-86): world sources, raw items and their crafting links.

Source of truth for content/ai/resources/*.json and content/loc/{ru,en}/resources.json
(keys of this pack), plus small idempotent links into content/ai/craft
(material.item, form.item, new materials). Run from the repo root:

    python tools/content_gen/gen_resources.py

Stages: AL-88 wood/stone/iron, AL-89 animals and light, AL-90 plants, AL-91 salt, stores, leather, goods. Later stages append to the tables below.
"""
import json
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
P = lambda *a: os.path.join(ROOT, *a)


def load(p):
    with open(p, encoding="utf-8") as f:
        return json.load(f)


def save(p, d):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(json.dumps(d, ensure_ascii=False, indent=1) + "\n")


def ru(nom, gen, dat, acc, ins, pre, g):
    return {"nom": nom, "gen": gen, "dat": dat, "acc": acc, "ins": ins, "pre": pre, "g": g}


# ---------------------------------------------------------------- materials
# New crafting materials (content/ai/craft/materials.json); item = what the world gives.
NEW_MATERIALS = [
    {"id": "yew_wood", "tags": ["wood", "hard_wood", "springy", "poisonous", "reagent"],
     "p": {"hardness": 0.55, "toughness": 0.75, "density": 0.65, "flex": 0.75, "flammable": 0.5, "porous": 0.25,
           "moisture": 0.3, "purity": 0.85, "toxic": 0.2},
     "shape": "stave", "len": 4, "item": "yew_stave", "worth": 1.8,
     "heat": [{"id": "char", "at": 2, "no_air": True, "to": "charcoal", "modes": ["char"]}]},
    {"id": "linden_wood", "tags": ["wood", "soft_wood", "carving"],
     "p": {"hardness": 0.3, "toughness": 0.45, "density": 0.45, "flex": 0.3, "flammable": 0.55, "porous": 0.35,
           "moisture": 0.35, "purity": 0.85, "workable": 0.7},
     "shape": "log", "len": 2, "thick": 2, "item": "linden_block", "worth": 1.0,
     "heat": [{"id": "char", "at": 2, "no_air": True, "to": "charcoal", "modes": ["char"]}]},
    {"id": "linden_bast", "tags": ["fiber", "plant_fiber", "bast"],
     "p": {"fiber": 0.6, "flex": 0.7, "toughness": 0.45, "moisture": 0.3, "purity": 0.8},
     "shape": "strip", "item": "linden_bast", "worth": 0.7},
    {"id": "oak_bark", "tags": ["bark", "tannin"],
     "p": {"fiber": 0.3, "toughness": 0.3, "moisture": 0.3, "purity": 0.6, "flammable": 0.5},
     "shape": "strip", "item": "oak_bark", "worth": 0.6, "effects": [{"id": "tannin", "k": 0.6}]},
    {"id": "peat", "tags": ["fuel", "earth"],
     "p": {"flammable": 0.7, "density": 0.3, "moisture": 0.5, "purity": 0.4, "porous": 0.6},
     "shape": "lump", "item": "peat", "worth": 0.4},
]
# Existing materials that get a source in the world (material -> item).
MATERIAL_ITEMS = {
    "charcoal": "charcoal", "wool_raw": "wool", "flint": "flint", "limestone": "limestone",
    "quicklime": "quicklime", "sand": "sand", "bog_iron_ore": "bog_ore", "copper_ore": "copper_ore",
    "tin_ore": "tin_ore", "resin": "resin", "birch_bark": "birch_bark", "wood_ash": "wood_ash",
    "ash_wood": "ash_stave", "pine_wood": "pine_log",
}
# Archetypes the sim counts as an item (form -> item); existing items are linked as they are.
FORM_ITEMS = {
    "dagger": "dagger", "sword": "sword", "mace": "mace", "club": "club", "short_bow": "short_bow",
    "gambeson": "gambeson", "leather_armor": "leather_armour", "mail_shirt": "mail_shirt", "helmet": "helmet",
    "boots": "boots", "hammer_tool": "hammer", "tongs": "tongs", "saw": "saw", "whetstone": "whetstone",
    "wooden_bucket": "bucket", "barrel": "keg", "basket": "basket", "pouch": "pouch", "rope": "rope",
    "fishing_net": "fishing_net", "leather_sheet": "leather", "plank": "plank", "beam": "beam", "brick": "brick",
    "roof_tile": "roof_tile", "mortar_lime": "lime_mortar", "poison": "poison", "tincture": "tincture",
    "soap": "soap", "candle": "candle", "torch": "torch",
}

# ---------------------------------------------------------------- items
# id, material, tags, stack, value, kg, size, ru noun (6 cases + g), en
ITEMS = [
    # raw from the world
    ("flint", "flint", ["stone", "craft_raw"], 30, 0.3, 1.0, [1, 1], ru("кремень", "кремня", "кремню", "кремень", "кремнем", "кремне", "m"), "flint"),
    ("limestone", "limestone", ["stone", "craft_raw"], 20, 0.3, 4.0, [1, 2], ru("известняк", "известняка", "известняку", "известняк", "известняком", "известняке", "m"), "limestone"),
    ("quicklime", "quicklime", ["mineral", "building"], 20, 1.0, 2.0, [1, 1], ru("известь", "извести", "извести", "известь", "известью", "извести", "f"), "quicklime"),
    ("sand", "sand", ["earth", "craft_raw"], 30, 0.1, 2.0, [1, 1], ru("песок", "песка", "песку", "песок", "песком", "песке", "m"), "sand"),
    ("bog_ore", "bog_iron_ore", ["ore", "craft_raw"], 20, 0.8, 3.0, [1, 1], ru("болотная руда", "болотной руды", "болотной руде", "болотную руду", "болотной рудой", "болотной руде", "f"), "bog iron ore"),
    ("copper_ore", "copper_ore", ["ore", "craft_raw"], 20, 1.5, 3.0, [1, 1], ru("медная руда", "медной руды", "медной руде", "медную руду", "медной рудой", "медной руде", "f"), "copper ore"),
    ("tin_ore", "tin_ore", ["ore", "craft_raw", "trade_good"], 20, 3.0, 3.0, [1, 1], ru("оловянная руда", "оловянной руды", "оловянной руде", "оловянную руду", "оловянной рудой", "оловянной руде", "f"), "tin ore"),
    ("peat", "peat", ["fuel"], 20, 0.2, 2.0, [1, 1], ru("торф", "торфа", "торфу", "торф", "торфом", "торфе", "m"), "peat"),
    ("resin", "resin", ["craft_raw", "adhesive"], 20, 0.6, 0.3, [1, 1], ru("смола", "смолы", "смоле", "смолу", "смолой", "смоле", "f"), "resin"),
    ("birch_bark", "birch_bark", ["bark", "craft_raw", "tinder"], 30, 0.2, 0.2, [1, 1], ru("береста", "бересты", "бересте", "бересту", "берестой", "бересте", "f"), "birch bark"),
    ("oak_bark", "oak_bark", ["bark", "craft_raw"], 30, 0.2, 0.4, [1, 1], ru("дубовая кора", "дубовой коры", "дубовой коре", "дубовую кору", "дубовой корой", "дубовой коре", "f"), "oak bark"),
    ("wood_ash", "wood_ash", ["craft_raw", "alkali"], 30, 0.1, 0.3, [1, 1], ru("зола", "золы", "золе", "золу", "золой", "золе", "f"), "wood ash"),
    ("ash_stave", "ash_wood", ["wood", "craft_raw"], 10, 0.8, 2.0, [1, 3], ru("ясеневая жердь", "ясеневой жерди", "ясеневой жерди", "ясеневую жердь", "ясеневой жердью", "ясеневой жерди", "f"), "ash stave"),
    ("yew_stave", "yew_wood", ["wood", "craft_raw", "valuable"], 10, 2.5, 2.0, [1, 3], ru("тисовая заготовка", "тисовой заготовки", "тисовой заготовке", "тисовую заготовку", "тисовой заготовкой", "тисовой заготовке", "f"), "yew stave"),
    ("pine_log", "pine_wood", ["wood", "heavy"], 6, 0.6, 12.0, [2, 3], ru("сосновое бревно", "соснового бревна", "сосновому бревну", "сосновое бревно", "сосновым бревном", "сосновом бревне", "n"), "pine log"),
    ("linden_block", "linden_wood", ["wood", "craft_raw"], 10, 0.6, 3.0, [1, 2], ru("липовая чурка", "липовой чурки", "липовой чурке", "липовую чурку", "липовой чуркой", "липовой чурке", "f"), "linden block"),
    ("linden_bast", "linden_bast", ["fiber", "craft_raw"], 30, 0.2, 0.2, [1, 1], ru("липовый луб", "липового луба", "липовому лубу", "липовый луб", "липовым лубом", "липовом лубе", "m"), "linden bast"),
    # made things the archetypes count as (the forms of content/ai/craft)
    ("dagger", None, ["tool", "weapon"], 1, 8, 0.4, [1, 2], ru("кинжал", "кинжала", "кинжалу", "кинжал", "кинжалом", "кинжале", "m"), "dagger"),
    ("sword", None, ["weapon"], 1, 20, 1.3, [1, 4], ru("меч", "меча", "мечу", "меч", "мечом", "мече", "m"), "sword"),
    ("mace", None, ["weapon"], 1, 12, 1.6, [1, 3], ru("булава", "булавы", "булаве", "булаву", "булавой", "булаве", "f"), "mace"),
    ("club", None, ["weapon"], 1, 1, 1.2, [1, 3], ru("дубина", "дубины", "дубине", "дубину", "дубиной", "дубине", "f"), "club"),
    ("short_bow", None, ["weapon", "ranged"], 1, 8, 0.8, [1, 4], ru("лук", "лука", "луку", "лук", "луком", "луке", "m"), "bow"),
    ("hammer", None, ["tool"], 1, 6, 1.0, [1, 2], ru("молот", "молота", "молоту", "молот", "молотом", "молоте", "m"), "hammer"),
    ("tongs", None, ["tool"], 1, 5, 1.0, [1, 2], ru("клещи", "клещей", "клещам", "клещи", "клещами", "клещах", "pl"), "tongs"),
    ("saw", None, ["tool"], 1, 8, 0.8, [1, 3], ru("пила", "пилы", "пиле", "пилу", "пилой", "пиле", "f"), "saw"),
    ("whetstone", None, ["tool"], 1, 2, 0.4, [1, 1], ru("точило", "точила", "точилу", "точило", "точилом", "точиле", "n"), "whetstone"),
    ("bucket", None, ["container"], 1, 2, 1.0, [2, 2], ru("ведро", "ведра", "ведру", "ведро", "ведром", "ведре", "n"), "bucket"),
    ("keg", None, ["container"], 1, 4, 6.0, [2, 2], ru("бочонок", "бочонка", "бочонку", "бочонок", "бочонком", "бочонке", "m"), "keg"),
    ("basket", None, ["container"], 1, 1, 0.5, [2, 2], ru("корзина", "корзины", "корзине", "корзину", "корзиной", "корзине", "f"), "basket"),
    ("pouch", None, ["container"], 1, 2, 0.2, [1, 1], ru("кошель", "кошеля", "кошелю", "кошель", "кошелем", "кошеле", "m"), "pouch"),
    ("rope", None, ["binding"], 10, 1, 0.5, [1, 1], ru("верёвка", "верёвки", "верёвке", "верёвку", "верёвкой", "верёвке", "f"), "rope"),
    ("fishing_net", None, ["tool"], 1, 4, 1.5, [2, 2], ru("сеть", "сети", "сети", "сеть", "сетью", "сети", "f"), "fishing net"),
    ("leather", None, ["textile", "craft_raw"], 10, 3, 1.0, [2, 2], ru("кожа", "кожи", "коже", "кожу", "кожей", "коже", "f"), "leather"),
    ("plank", None, ["building", "wood"], 10, 1, 4.0, [1, 3], ru("доска", "доски", "доске", "доску", "доской", "доске", "f"), "plank"),
    ("beam", None, ["building", "wood", "heavy"], 4, 2, 12.0, [1, 4], ru("балка", "балки", "балке", "балку", "балкой", "балке", "f"), "beam"),
    ("brick", None, ["building"], 20, 0.5, 2.5, [1, 1], ru("кирпич", "кирпича", "кирпичу", "кирпич", "кирпичом", "кирпиче", "m"), "brick"),
    ("roof_tile", None, ["building"], 20, 0.5, 2.0, [1, 1], ru("черепица", "черепицы", "черепице", "черепицу", "черепицей", "черепице", "f"), "roof tile"),
    ("lime_mortar", None, ["building"], 10, 1, 3.0, [1, 1], ru("известковый раствор", "известкового раствора", "известковому раствору", "известковый раствор", "известковым раствором", "известковом растворе", "m"), "lime mortar"),
    ("poison", None, ["poison"], 5, 6, 0.2, [1, 1], ru("яд", "яда", "яду", "яд", "ядом", "яде", "m"), "poison"),
    ("tincture", None, ["medicine"], 5, 4, 0.3, [1, 1], ru("настойка", "настойки", "настойке", "настойку", "настойкой", "настойке", "f"), "tincture"),
    ("soap", None, ["household"], 10, 2, 0.3, [1, 1], ru("мыло", "мыла", "мылу", "мыло", "мылом", "мыле", "n"), "soap"),
    ("torch", None, ["light"], 5, 0.5, 0.6, [1, 2], ru("факел", "факела", "факелу", "факел", "факелом", "факеле", "m"), "torch"),
]

# Items of other files that this pack annotates (fields fill in only where missing).
ANNOTATE = [
    {"id": "charcoal", "material": "charcoal", "value": 0.5},
    {"id": "wool", "material": "wool_raw", "value": 1},
]

# ---------------------------------------------------------------- objects
# The sources in the world. Trees are species beside the generic `tree` (which the 3D
# client draws and fells, AL-78): they give `log` for building plus what only they give.
def obj(id_, stock, biomes=None, per_km2=0.0, regrow=None, every=0, stand=0.8, season=None, **kw):
    o = {"id": id_, "stand_m": stand, "stock": stock}
    if regrow:
        o["regrow"] = regrow
        o["regrow_every"] = every
        o["max_stock"] = dict(stock)
    if biomes:
        o["biomes"] = biomes
        o["per_km2"] = per_km2
    o["owned"] = kw.pop("owned", False)
    if season:
        o["season"] = season
    o.update(kw)
    return o


OBJECTS = [
    obj("oak_tree", {"log": 5, "oak_bark": 4}, ["forest"], 50, _doc="дуб: брёвна и дубильная кора"),
    obj("ash_tree", {"log": 3, "ash_stave": 3}, ["forest", "meadow"], 25, _doc="ясень: жерди для древков, рукоятей и луков"),
    obj("pine_tree", {"pine_log": 4, "resin": 3}, ["forest"], 70, regrow={"resin": 1}, every=288,
        _doc="сосна: дешёвые брёвна, смола подсочкой (растёт снова)"),
    obj("birch_tree", {"log": 3, "birch_bark": 3}, ["forest", "meadow"], 50, regrow={"birch_bark": 1}, every=432,
        _doc="берёза: береста (дёготь, туеса, растопка)"),
    obj("willow_stand", {"branch": 10, "bast_fiber": 4}, ["river", "marsh"], 30, regrow={"branch": 3, "bast_fiber": 1},
        every=144, stand=0.7, _doc="ивняк: лоза и ивовая кора, не рубится, а режется"),
    obj("yew_tree", {"log": 1, "yew_stave": 2}, ["forest"], 2, _doc="тис: редкий, лучший лук"),
    obj("linden_tree", {"log": 3, "linden_block": 2, "linden_bast": 4}, ["forest"], 20, regrow={"linden_bast": 1},
        every=432, _doc="липа: мягкая древесина для резьбы, луб для верёвок и лаптей"),
    obj("flint_scree", {"flint": 16}, ["meadow", "forest"], 2, regrow={"flint": 2}, every=288, stand=0.5,
        _doc="кремнёвая осыпь"),
    obj("limestone_outcrop", {"limestone": 30}, ["meadow", "forest"], 1.5, regrow={"limestone": 2}, every=432,
        stand=0.8, _doc="выход известняка"),
    obj("sand_bar", {"sand": 30}, ["river"], 3, regrow={"sand": 4}, every=144, stand=0.5, _doc="песчаная отмель"),
    obj("bog_ore_bed", {"bog_ore": 12}, ["marsh", "river"], 2, regrow={"bog_ore": 1}, every=720, stand=0.6,
        _doc="залежь болотной руды, копится годами"),
    obj("peat_bog", {"peat": 40}, ["marsh"], 2, regrow={"peat": 1}, every=720, stand=0.6, _doc="торфяник"),
    obj("copper_vein", {"copper_ore": 15}, ["forest", "meadow"], 0.3, stand=0.8, _doc="медная жила, редкая"),
    obj("charcoal_clamp", {}, stand=1.2, settlement=True, per_settlement=1, own_seed=True,
        _doc="угольная яма на краю деревни: дрова/брёвна -> древесный уголь"),
    obj("lime_kiln", {}, stand=1.2, settlement=True, per_settlement=1, own_seed=True,
        _doc="известковая печь: известняк + дрова -> известь"),
]


# ---------------------------------------------------------------- interactions
def work(id_, target, pre, eff, dur, cost=None, risk=None, reserve=True, ru_name="", en_name=""):
    a = {"id": id_, "category": "work", "target": "object:" + target, "pre": pre, "eff": eff,
         "from_target": True, "duration": dur, "cost": cost if cost is not None else dur, "fsm": "Work",
         "legal": [], "visibility": "public"}
    if reserve:
        a["reserve"] = True
    if risk:
        a["risk"] = risk
    a["_name"] = (ru_name, en_name)
    return a


AXE_SLIP = [{"id": "axe_slip", "p": 0.01, "eff": ["body:health-0.15"]}]


def fell(tree, gives, n, ru_name, en_name, extra=()):
    pre = ["knows:woodcutting", "has:axe", f"target_stock:{gives}>={n}"]
    eff = [f"has:{gives}+{n}", f"target_stock:{gives}-{n}", "skill:woodcutting+0.01", "body:fatigue+0.12", *extra]
    return work(f"fell_{tree}", tree, pre, eff, 9, 10, AXE_SLIP, ru_name=ru_name, en_name=en_name)


def take(id_, target, item, n, dur, tool=None, knows=None, skill=None, fatigue=0.03, ru_name="", en_name="", extra=()):
    pre = []
    if knows:
        pre.append(f"knows:{knows}")
    if tool:
        pre.append(f"has:{tool}")
    pre.append(f"target_stock:{item}>={n}")
    eff = [f"has:{item}+{n}", f"target_stock:{item}-{n}", f"body:fatigue+{fatigue}"]
    if skill:
        eff.insert(2, f"skill:{skill}+0.01")
    eff += list(extra)
    return work(id_, target, pre, eff, dur, ru_name=ru_name, en_name=en_name)


INTERACTIONS = [
    fell("oak_tree", "log", 2, "Рубить дуб", "Fell the oak"),
    take("strip_oak_bark", "oak_tree", "oak_bark", 2, 3, tool="stone_blade", ru_name="Драть дубовую кору", en_name="Strip oak bark"),
    fell("ash_tree", "log", 2, "Рубить ясень", "Fell the ash"),
    take("cut_ash_staves", "ash_tree", "ash_stave", 1, 5, tool="axe", skill="woodcutting", fatigue=0.06,
         ru_name="Нарубить ясеневых жердей", en_name="Cut ash staves"),
    fell("pine_tree", "pine_log", 2, "Рубить сосну", "Fell the pine"),
    take("tap_pine_resin", "pine_tree", "resin", 1, 3, tool="stone_blade", skill="foraging",
         ru_name="Собрать смолу", en_name="Tap pine resin"),
    fell("birch_tree", "log", 2, "Рубить берёзу", "Fell the birch"),
    take("peel_birch_bark", "birch_tree", "birch_bark", 1, 2, tool="stone_blade", skill="foraging",
         ru_name="Снять бересту", en_name="Peel birch bark"),
    take("cut_withies", "willow_stand", "branch", 3, 3, tool="stone_blade", skill="basketry",
         ru_name="Нарезать лозы", en_name="Cut withies"),
    take("strip_willow_bark", "willow_stand", "bast_fiber", 1, 2, tool="stone_blade", skill="herbalism",
         ru_name="Снять ивовую кору", en_name="Strip willow bark"),
    take("cut_yew_staves", "yew_tree", "yew_stave", 1, 6, tool="axe", skill="woodcutting", fatigue=0.06,
         ru_name="Вырубить тисовую заготовку", en_name="Cut a yew stave"),
    fell("linden_tree", "log", 2, "Рубить липу", "Fell the linden"),
    take("cut_linden_blocks", "linden_tree", "linden_block", 1, 4, tool="axe", skill="woodcutting", fatigue=0.05,
         ru_name="Нарубить липовых чурок", en_name="Cut linden blocks"),
    take("strip_linden_bast", "linden_tree", "linden_bast", 2, 3, tool="stone_blade", skill="foraging",
         ru_name="Надрать луба", en_name="Strip linden bast"),
    take("gather_flint", "flint_scree", "flint", 3, 3, skill="knapping", ru_name="Набрать кремня", en_name="Gather flint"),
    take("quarry_limestone", "limestone_outcrop", "limestone", 2, 8, knows="mining", skill="mining", fatigue=0.1,
         ru_name="Ломать известняк", en_name="Quarry limestone"),
    take("dig_sand", "sand_bar", "sand", 4, 3, ru_name="Накопать песка", en_name="Dig sand"),
    take("dig_bog_ore", "bog_ore_bed", "bog_ore", 2, 8, skill="mining", fatigue=0.12,
         ru_name="Копать болотную руду", en_name="Dig bog ore"),
    take("cut_peat", "peat_bog", "peat", 4, 6, fatigue=0.1, ru_name="Резать торф", en_name="Cut peat"),
    take("mine_copper", "copper_vein", "copper_ore", 2, 10, knows="mining", skill="mining", fatigue=0.14,
         ru_name="Добывать медную руду", en_name="Mine copper ore"),
    work("burn_charcoal", "charcoal_clamp", ["has:log>=2"],
         ["has:log-2", "has:charcoal+6", "skill:charcoal_burning+0.015", "body:fatigue+0.1"], 16, 12,
         ru_name="Выжечь уголь", en_name="Burn charcoal"),
    work("burn_charcoal_firewood", "charcoal_clamp", ["has:firewood>=6"],
         ["has:firewood-6", "has:charcoal+4", "skill:charcoal_burning+0.01", "body:fatigue+0.08"], 14, 12,
         ru_name="Выжечь уголь из дров", en_name="Burn charcoal from firewood"),
    work("burn_lime", "lime_kiln", ["has:limestone>=3", "has:firewood>=3"],
         ["has:limestone-3", "has:firewood-3", "has:quicklime+3", "skill:masonry+0.01", "body:fatigue+0.08"], 12, 10,
         ru_name="Обжечь известь", en_name="Burn lime"),
    work("collect_ash", "hearth", ["has:firewood>=1", "target_owner:family"],
         ["has:firewood-1", "has:wood_ash+2"], 1, 1, reserve=False,
         ru_name="Выгрести золу", en_name="Rake out the ash"),
]

OBJECT_NAMES = {  # ru noun, en
    "oak_tree": (ru("дуб", "дуба", "дубу", "дуб", "дубом", "дубе", "m"), "oak"),
    "ash_tree": (ru("ясень", "ясеня", "ясеню", "ясень", "ясенем", "ясене", "m"), "ash tree"),
    "pine_tree": (ru("сосна", "сосны", "сосне", "сосну", "сосной", "сосне", "f"), "pine"),
    "birch_tree": (ru("берёза", "берёзы", "берёзе", "берёзу", "берёзой", "берёзе", "f"), "birch"),
    "willow_stand": (ru("ивняк", "ивняка", "ивняку", "ивняк", "ивняком", "ивняке", "m"), "willow stand"),
    "yew_tree": (ru("тис", "тиса", "тису", "тис", "тисом", "тисе", "m"), "yew"),
    "linden_tree": (ru("липа", "липы", "липе", "липу", "липой", "липе", "f"), "linden"),
    "flint_scree": (ru("кремнёвая осыпь", "кремнёвой осыпи", "кремнёвой осыпи", "кремнёвую осыпь", "кремнёвой осыпью", "кремнёвой осыпи", "f"), "flint scree"),
    "limestone_outcrop": (ru("выход известняка", "выхода известняка", "выходу известняка", "выход известняка", "выходом известняка", "выходе известняка", "m"), "limestone outcrop"),
    "sand_bar": (ru("песчаная отмель", "песчаной отмели", "песчаной отмели", "песчаную отмель", "песчаной отмелью", "песчаной отмели", "f"), "sand bar"),
    "bog_ore_bed": (ru("залежь руды", "залежи руды", "залежи руды", "залежь руды", "залежью руды", "залежи руды", "f"), "bog ore bed"),
    "peat_bog": (ru("торфяник", "торфяника", "торфянику", "торфяник", "торфяником", "торфянике", "m"), "peat bog"),
    "copper_vein": (ru("медная жила", "медной жилы", "медной жиле", "медную жилу", "медной жилой", "медной жиле", "f"), "copper vein"),
    "charcoal_clamp": (ru("угольная яма", "угольной ямы", "угольной яме", "угольную яму", "угольной ямой", "угольной яме", "f"), "charcoal clamp"),
    "lime_kiln": (ru("известковая печь", "известковой печи", "известковой печи", "известковую печь", "известковой печью", "известковой печи", "f"), "lime kiln"),
}
MATERIAL_NAMES = {  # material: (ru nom, g, ru made, en, en made)
    "yew_wood": ("тис", "m", "тисов{form:g|ый|ая|ое|ые} {form}", "yew", "yew {form}"),
    "linden_wood": ("липа", "f", "липов{form:g|ый|ая|ое|ые} {form}", "linden", "linden {form}"),
    "linden_bast": ("луб", "m", "лубян{form:g|ой|ая|ое|ые} {form}", "bast", "bast {form}"),
    "oak_bark": ("дубовая кора", "f", "{form} из дубовой коры", "oak bark", "oak bark {form}"),
    "peat": ("торф", "m", "торфян{form:g|ой|ая|ое|ые} {form}", "peat", "peat {form}"),
}


# ================================================================ AL-89: animals and light
# Butchery gives fat and sinew, a goat its horns, a stag its antlers, a hen its feathers;
# hives give wax. Tallow is rendered from fat at the hearth (a heat step of the designs).
NEW_MATERIALS += [
    {"id": "tallow", "tags": ["fat", "animal"],
     "p": {"flammable": 0.75, "workable": 0.55, "tacky": 0.2, "waterproof": 0.6, "purity": 0.7, "melt": 1},
     "shape": "lump", "raw": False, "worth": 0.9},
    {"id": "feather", "tags": ["feather", "animal"],
     "p": {"fiber": 0.3, "flex": 0.6, "density": 0.05, "warmth": 0.7, "purity": 0.7, "flammable": 0.6},
     "shape": "fiber_raw", "item": "feathers", "worth": 0.5},
]
MATERIAL_ITEMS.update({"animal_fat": "animal_fat", "sinew": "sinew", "beeswax": "beeswax"})
# rendering: fat gently roasted at the hearth turns to tallow (a material transform)
MATERIAL_HEAT = {"animal_fat": [{"id": "roast", "at": 1, "to": "tallow"}]}
FORM_ITEMS.update({})
ITEMS += [
    ("animal_fat", "animal_fat", ["animal", "craft_raw", "fat"], 20, 0.4, 0.3, [1, 1], ru("жир", "жира", "жиру", "жир", "жиром", "жире", "m"), "animal fat"),
    ("sinew", "sinew", ["animal", "craft_raw", "fiber"], 30, 0.4, 0.1, [1, 1], ru("жилы", "жил", "жилам", "жилы", "жилами", "жилах", "pl"), "sinew"),
    ("beeswax", "beeswax", ["craft_raw", "wax", "trade_good"], 20, 1.5, 0.3, [1, 1], ru("воск", "воска", "воску", "воск", "воском", "воске", "m"), "beeswax"),
    ("feathers", "feather", ["animal", "craft_raw"], 40, 0.1, 0.05, [1, 1], ru("перья", "перьев", "перьям", "перья", "перьями", "перьях", "pl"), "feathers"),
    ("arrows", None, ["weapon", "ranged", "ammo"], 20, 0.5, 0.05, [1, 3], ru("стрела", "стрелы", "стреле", "стрелу", "стрелой", "стреле", "f"), "arrow"),
    ("longbow", None, ["weapon", "ranged"], 1, 14, 1.0, [1, 4], ru("длинный лук", "длинного лука", "длинному луку", "длинный лук", "длинным луком", "длинном луке", "m"), "longbow"),
]
# new archetypes (one form per line in forms.json)
NEW_FORMS = {
    "arrows": '{"id": "arrows", "item": "arrows", "category": "ranged", "named": false, "needs": {"shape": ["shaft"], "has(blade)": "1", "edge": ">=0.25", "reach": "2", "profile": ["feather"]},\n   "derive": {"kind": "ammo", "weapon_id": "bow", "pen": "edge*q", "durability": "3*(0.5+q)"}}',
    "longbow": '{"id": "longbow", "item": "longbow", "worth": 1.3, "category": "ranged", "needs": {"shape": ["bow_stave"], "has(string)": "1", "stave.len": "4", "flexion": ">=0.7", "bind": ">=0.3"},\n   "derive": {"kind": "ranged", "weapon_id": "bow", "power": "0.7*stave.flex*string.toughness*(0.5+q)", "durability": "100*stave.toughness*(0.5+q)"}}',
}
NEW_DESIGNS = [
    {"id": "tallow_candle", "form": "candle", "steps": [
        {"process": "heat", "mode": "roast", "heat": 1, "slots": {"work": ["mat:animal_fat"]}},
        {"process": "mould", "shape": "candle_raw", "slots": {"work": ["step:1"]}},
        {"process": "twist", "shape": "strand", "slots": {"work": ["mat:flax_fiber"]}},
        {"process": "join", "slots": {"a": ["step:2"], "b": ["step:3"]}}]},
    {"id": "arrows", "form": "arrows", "steps": [
        {"process": "knap", "shape": "blade", "len": 1, "slots": {"work": ["mat:flint"]}},
        {"process": "carve", "shape": "shaft", "len": 3, "slots": {"work": ["mat:ash_wood"]}},
        {"process": "join", "slots": {"a": ["step:2"], "b": ["step:1"], "binder": ["mat:resin"]}},
        {"process": "join", "slots": {"a": ["step:3"], "b": ["mat:feather"]}}]},
    {"id": "longbow", "form": "longbow", "steps": [
        {"process": "carve", "shape": "bow_stave", "len": 4, "slots": {"work": ["mat:yew_wood"]}},
        {"process": "twist", "shape": "strand", "slots": {"work": ["mat:sinew"]}},
        {"process": "twist", "shape": "cord", "slots": {"work": ["step:2"]}},
        {"process": "join", "slots": {"a": ["step:1"], "b": ["step:3"]}}]},
]
# what the base acts also give (content/ai/interactions.json; appended once)
BASE_EFFECTS = {
    "butcher_carcass": ["has:animal_fat+1", "has:sinew+1"],
    "slaughter_goat": ["has:horn+2"],
    "hunt_game": ["has:antler+1"],
    "kill_hen": ["has:feathers+4"],
    "kill_hen_for_pot": ["has:feathers+4"],
    "gather_honey": ["has:beeswax+1"],
}
OBJECTS += [
    obj("beehive", {"honey": 4, "beeswax": 2}, regrow={"honey": 1, "beeswax": 1}, every=288, stand=0.8,
        settlement=True, per_settlement=2, own_seed=True,
        _doc="улей-колода у домов: мёд и воск (расставляет тред поселений)"),
]
INTERACTIONS += [
    work("harvest_hive", "beehive", ["has:firewood>=1", "target_stock:honey>=2", "target_stock:beeswax>=1"],
         ["has:honey+2", "has:beeswax+1", "target_stock:honey-2", "target_stock:beeswax-1", "has:firewood-1",
          "skill:husbandry+0.01", "body:fatigue+0.03"], 3,
         risk=[{"id": "bee_stings", "p": 0.05, "eff": ["body:health-0.03"]}],
         ru_name="Подрезать соты", en_name="Harvest the hive"),
]
FORM_NAMES = {  # form: (ru nom, g, en)
    "arrows": ("стрелы", "pl", "arrows"),
    "longbow": ("длинный лук", "m", "longbow"),
}
DESIGN_NAMES = {  # design: (ru, en) of the act craft:<design>
    "arrows": ("стрелы", "arrows"), "longbow": ("длинный лук", "longbow"), "tallow_candle": ("сальная свеча", "tallow candle"),
}
OBJECT_NAMES["beehive"] = (ru("улей", "улья", "улью", "улей", "ульем", "улье", "m"), "beehive")
MATERIAL_NAMES.update({
    "tallow": ("сало", "n", "сальн{form:g|ый|ая|ое|ые} {form}", "tallow", "tallow {form}"),
    "feather": ("перо", "n", "{form} из перьев", "feather", "feather {form}"),
})


# ================================================================ AL-90: plants
# Wild hemp and flax, dye plants (blue woad, red madder, yellow weld: a dye bath soaks its
# colour and the `dyed` tag into cloth), herbs and poisons each their own item and patch.
# Magic is not here yet: poppy, hemlock, hellebore and yew carry the tag `reagent`.
NEW_MATERIALS += [
    {"id": "hemp_fiber", "tags": ["fiber", "plant_fiber", "bast"],
     "p": {"fiber": 0.85, "flex": 0.55, "toughness": 0.75, "flammable": 0.6, "moisture": 0.3, "purity": 0.75, "warmth": 0.25},
     "shape": "fiber_raw", "item": "hemp_fiber", "worth": 0.9},
    {"id": "woad", "tags": ["herb", "dye", "dyed", "colour_blue"], "p": {"moisture": 0.6, "purity": 0.7},
     "shape": "lump", "item": "woad", "worth": 1.2},
    {"id": "madder", "tags": ["herb", "dye", "dyed", "colour_red"], "p": {"moisture": 0.5, "purity": 0.7},
     "shape": "lump", "item": "madder", "worth": 1.4},
    {"id": "weld", "tags": ["herb", "dye", "dyed", "colour_yellow"], "p": {"moisture": 0.6, "purity": 0.7},
     "shape": "lump", "item": "weld", "worth": 1.0},
    {"id": "hellebore", "tags": ["herb", "poisonous", "reagent"], "p": {"toxic": 0.7, "moisture": 0.6, "purity": 0.8},
     "shape": "lump", "item": "hellebore", "effects": [{"id": "poison", "k": 0.6}, {"id": "bitter", "k": 0.5}], "worth": 1.2},
    {"id": "hops", "tags": ["herb", "aromatic"], "p": {"moisture": 0.5, "purity": 0.8},
     "shape": "lump", "item": "hops", "effects": [{"id": "preserves", "k": 0.5}, {"id": "bitter", "k": 0.4}], "worth": 0.9},
    {"id": "linden_flower", "tags": ["herb", "aromatic"], "p": {"moisture": 0.5, "purity": 0.85},
     "shape": "lump", "item": "linden_flower", "effects": [{"id": "fresh", "k": 0.4}, {"id": "warm", "k": 0.2}], "worth": 0.8},
]
MATERIAL_ITEMS.update({"poppy": "poppy", "hemlock": "hemlock", "garlic": "garlic", "nettle": "nettle"})
MATERIAL_TAGS = {"poppy": ["reagent"], "hemlock": ["reagent"]}  # appended once
ITEMS += [
    ("hemp_fiber", "hemp_fiber", ["fiber", "craft_raw"], 30, 0.3, 0.2, [1, 1], ru("пенька", "пеньки", "пеньке", "пеньку", "пенькой", "пеньке", "f"), "hemp fibre"),
    ("woad", "woad", ["plant", "craft_raw", "dye"], 20, 0.5, 0.2, [1, 1], ru("вайда", "вайды", "вайде", "вайду", "вайдой", "вайде", "f"), "woad"),
    ("madder", "madder", ["plant", "craft_raw", "dye"], 20, 0.6, 0.2, [1, 1], ru("марена", "марены", "марене", "марену", "мареной", "марене", "f"), "madder"),
    ("weld", "weld", ["plant", "craft_raw", "dye"], 20, 0.4, 0.2, [1, 1], ru("резеда", "резеды", "резеде", "резеду", "резедой", "резеде", "f"), "weld"),
    ("poppy", "poppy", ["plant", "herb"], 20, 0.8, 0.1, [1, 1], ru("мак", "мака", "маку", "мак", "маком", "маке", "m"), "poppy"),
    ("hemlock", "hemlock", ["plant", "herb", "poisonous"], 20, 0.6, 0.1, [1, 1], ru("болиголов", "болиголова", "болиголову", "болиголов", "болиголовом", "болиголове", "m"), "hemlock"),
    ("hellebore", "hellebore", ["plant", "herb", "poisonous"], 20, 0.6, 0.1, [1, 1], ru("чемерица", "чемерицы", "чемерице", "чемерицу", "чемерицей", "чемерице", "f"), "hellebore"),
    ("garlic", "garlic", ["plant", "herb", "food"], 20, 0.3, 0.1, [1, 1], ru("черемша", "черемши", "черемше", "черемшу", "черемшой", "черемше", "f"), "wild garlic"),
    ("hops", "hops", ["plant", "herb"], 20, 0.4, 0.1, [1, 1], ru("хмель", "хмеля", "хмелю", "хмель", "хмелем", "хмеле", "m"), "hops"),
    ("linden_flower", "linden_flower", ["plant", "herb"], 20, 0.3, 0.05, [1, 1], ru("липовый цвет", "липового цвета", "липовому цвету", "липовый цвет", "липовым цветом", "липовом цвете", "m"), "linden blossom"),
    ("nettle", "nettle", ["plant", "herb"], 20, 0.1, 0.1, [1, 1], ru("крапива", "крапивы", "крапиве", "крапиву", "крапивой", "крапиве", "f"), "nettle"),
]
OBJECTS += [
    obj("wild_hemp", {"hemp_fiber": 10, "fiber_raw": 6}, ["meadow", "river"], 6, regrow={"hemp_fiber": 2, "fiber_raw": 1},
        every=288, stand=0.7, _doc="дикая конопля и лён: пенька для верёвок и сетей, волокно на ткань"),
    obj("dye_meadow", {"woad": 4, "madder": 3, "weld": 4}, ["meadow"], 4, regrow={"woad": 1, "madder": 1, "weld": 1},
        every=288, stand=0.6, _doc="красильные травы: вайда (синее), марена (красное), резеда (жёлтое)"),
    obj("poppy_patch", {"poppy": 5}, ["meadow"], 3, regrow={"poppy": 1}, every=288, stand=0.6, _doc="мак: снотворное и боль"),
    obj("hemlock_stand", {"hemlock": 4}, ["river", "marsh"], 2, regrow={"hemlock": 1}, every=288, stand=0.6,
        _doc="болиголов у воды: яд"),
    obj("hellebore_patch", {"hellebore": 3}, ["marsh", "forest"], 1.5, regrow={"hellebore": 1}, every=432, stand=0.6,
        _doc="чемерица: яд и рвотное; лекарь или отравитель, судят по поступку"),
    obj("ramsons", {"garlic": 6}, ["forest"], 6, regrow={"garlic": 2}, every=144, stand=0.6,
        _doc="черемша: дикий чеснок, от заражения ран и в еду"),
    obj("hop_vine", {"hops": 5}, ["forest", "river"], 3, regrow={"hops": 1}, every=288, stand=0.7,
        _doc="хмель на опушке: эль дольше не киснет"),
]
# the linden flowers in summer (an object of this pack: its stock is rewritten here)
for _o in OBJECTS:
    if _o["id"] == "linden_tree":
        _o["stock"]["linden_flower"] = 3
        _o["regrow"]["linden_flower"] = 1
        _o["max_stock"]["linden_flower"] = 3
INTERACTIONS += [
    take("cut_hemp", "wild_hemp", "hemp_fiber", 3, 3, tool="stone_blade", skill="fiber_working",
         ru_name="Нарезать конопли", en_name="Cut hemp"),
    take("pull_wild_flax", "wild_hemp", "fiber_raw", 2, 3, skill="fiber_working", ru_name="Надёргать льна", en_name="Pull wild flax"),
    take("gather_woad", "dye_meadow", "woad", 2, 2, skill="herbalism", ru_name="Нарвать вайды", en_name="Gather woad"),
    take("dig_madder", "dye_meadow", "madder", 1, 3, skill="herbalism", fatigue=0.05, ru_name="Накопать марены", en_name="Dig madder root"),
    take("gather_weld", "dye_meadow", "weld", 2, 2, skill="herbalism", ru_name="Нарвать резеды", en_name="Gather weld"),
    take("gather_poppy", "poppy_patch", "poppy", 2, 2, knows="herbalism", skill="herbalism", ru_name="Собрать мак", en_name="Gather poppies"),
    take("gather_hemlock", "hemlock_stand", "hemlock", 1, 2, knows="herbalism", skill="herbalism",
         ru_name="Срезать болиголов", en_name="Cut hemlock"),
    take("dig_hellebore", "hellebore_patch", "hellebore", 1, 3, knows="herbalism", skill="herbalism",
         ru_name="Выкопать чемерицу", en_name="Dig hellebore"),
    take("gather_ramsons", "ramsons", "garlic", 2, 2, skill="foraging", ru_name="Нарвать черемши", en_name="Gather wild garlic"),
    take("pick_hops", "hop_vine", "hops", 2, 2, skill="foraging", ru_name="Собрать хмель", en_name="Pick hops"),
    take("pick_linden_flowers", "linden_tree", "linden_flower", 1, 2, skill="herbalism",
         ru_name="Собрать липовый цвет", en_name="Pick linden blossom"),
    work("pick_nettle_leaves", "nettle_patch", ["target_stock:fiber_raw>=2"],
         ["has:nettle+2", "target_stock:fiber_raw-2", "skill:herbalism+0.005", "moodlet:stung"], 2,
         ru_name="Нарвать крапивы", en_name="Pick nettles"),
]
def _dyed(base_steps, dye):
    n = len(base_steps)
    return base_steps + [
        {"process": "mix", "mode": "stir", "slots": {"a": [f"mat:{dye}"], "b": [{"ref": "mat:water", "n": 2}]}},
        {"process": "soak", "slots": {"work": [f"step:{n}"], "liquid": [f"step:{n + 1}"]}}]
_TUNIC = [{"process": "twist", "shape": "strand", "slots": {"work": ["mat:flax_fiber"]}},
          {"process": "weave", "shape": "cloth", "slots": {"work": ["step:1"]}},
          {"process": "twist", "shape": "strand", "slots": {"work": ["mat:flax_fiber"]}},
          {"process": "sew", "shape": "garment_raw", "len": 2, "slots": {"work": ["step:2"], "thread": ["step:3"]}}]
_CLOAK = [{"process": "twist", "shape": "strand", "slots": {"work": ["mat:wool_raw"]}},
          {"process": "weave", "shape": "cloth", "slots": {"work": ["step:1"]}},
          {"process": "twist", "shape": "strand", "slots": {"work": ["mat:flax_fiber"]}},
          {"process": "sew", "shape": "garment_raw", "len": 3, "slots": {"work": ["step:2"], "thread": ["step:3"]}}]
_CLOTH = [{"process": "twist", "shape": "strand", "slots": {"work": ["mat:flax_fiber"]}},
          {"process": "weave", "shape": "cloth", "slots": {"work": ["step:1"]}}]
NEW_DESIGNS += [
    {"id": "blue_tunic", "form": "tunic", "steps": _dyed(_TUNIC, "woad")},
    {"id": "red_cloak", "form": "cloak", "steps": _dyed(_CLOAK, "madder")},
    {"id": "yellow_cloth", "form": "cloth", "steps": _dyed(_CLOTH, "weld")},
    {"id": "hemp_rope", "form": "rope", "steps": [
        {"process": "twist", "shape": "strand", "slots": {"work": ["mat:hemp_fiber"]}},
        {"process": "twist", "shape": "cord", "slots": {"work": ["step:1"]}},
        {"process": "twist", "shape": "rope", "len": 3, "slots": {"work": ["step:2"]}}]},
    {"id": "hellebore_poison", "form": "poison", "steps": [
        {"process": "mix", "mode": "stir", "slots": {"a": ["mat:hellebore"], "b": [{"ref": "mat:water", "n": 2}]}}]},
    {"id": "linden_tea", "form": "herbal_tea", "steps": [
        {"process": "mix", "mode": "stir", "slots": {"a": ["mat:linden_flower"], "b": [{"ref": "mat:water", "n": 2}]}},
        {"process": "heat", "mode": "boil", "heat": 1, "slots": {"work": ["step:1"]}}]},
]
DESIGN_NAMES.update({
    "blue_tunic": ("синяя рубаха", "blue tunic"), "red_cloak": ("красный плащ", "red cloak"),
    "yellow_cloth": ("жёлтое полотно", "yellow cloth"), "hemp_rope": ("пеньковая верёвка", "hemp rope"),
    "hellebore_poison": ("яд из чемерицы", "hellebore poison"), "linden_tea": ("липовый чай", "linden tea"),
})
OBJECT_NAMES.update({
    "wild_hemp": (ru("конопляник", "конопляника", "коноплянику", "конопляник", "конопляником", "коноплянике", "m"), "wild hemp"),
    "dye_meadow": (ru("красильный луг", "красильного луга", "красильному лугу", "красильный луг", "красильным лугом", "красильном лугу", "m"), "dye meadow"),
    "poppy_patch": (ru("маковая поляна", "маковой поляны", "маковой поляне", "маковую поляну", "маковой поляной", "маковой поляне", "f"), "poppy patch"),
    "hemlock_stand": (ru("заросли болиголова", "зарослей болиголова", "зарослям болиголова", "заросли болиголова", "зарослями болиголова", "зарослях болиголова", "pl"), "hemlock stand"),
    "hellebore_patch": (ru("чемерица", "чемерицы", "чемерице", "чемерицу", "чемерицей", "чемерице", "f"), "hellebore patch"),
    "ramsons": (ru("черемша", "черемши", "черемше", "черемшу", "черемшой", "черемше", "f"), "wild garlic"),
    "hop_vine": (ru("хмельник", "хмельника", "хмельнику", "хмельник", "хмельником", "хмельнике", "m"), "hop vine"),
})
MATERIAL_NAMES.update({
    "hemp_fiber": ("пенька", "f", "пеньков{form:g|ый|ая|ое|ые} {form}", "hemp", "hemp {form}"),
    "woad": ("вайда", "f", "{form}, крашенн{form:g|ый|ая|ое|ые} вайдой", "woad", "woad-dyed {form}"),
    "madder": ("марена", "f", "{form}, крашенн{form:g|ый|ая|ое|ые} мареной", "madder", "madder-dyed {form}"),
    "weld": ("резеда", "f", "{form}, крашенн{form:g|ый|ая|ое|ые} резедой", "weld", "weld-dyed {form}"),
    "hellebore": ("чемерица", "f", "{form} из чемерицы", "hellebore", "hellebore {form}"),
    "hops": ("хмель", "m", "хмелев{form:g|ой|ая|ое|ые} {form}", "hops", "hop {form}"),
    "linden_flower": ("липовый цвет", "m", "{form} из липового цвета", "linden blossom", "linden {form}"),
})


# ================================================================ AL-91: salt, stores, leather, home, goods
# A rare salt spring gives brine, boiled to salt at the hearth; salt keeps meat, fish,
# cabbage and mushrooms through the winter. A tanning pit turns hides into leather with
# oak bark or lime (the base tanning_pit, a station the tannery room brings). Things of worth for trade carry the tag `trade_good` (caravans, AL-85).
MATERIAL_ITEMS.update({"salt": "salt"})
NEW_MATERIALS += [
    {"id": "brine", "tags": ["liquid", "salty"], "p": {"density": 0.5, "purity": 0.6},
     "shape": "liquid", "item": "brine", "effects": [{"id": "salty", "k": 0.6}], "worth": 0.3},
    # adapted from the old stone-age tables (content/resources/herbs.json): yarrow, the soldier's herb
    {"id": "yarrow", "tags": ["herb", "aromatic"], "p": {"moisture": 0.5, "purity": 0.8},
     "shape": "lump", "item": "yarrow", "effects": [{"id": "antiseptic", "k": 0.35}, {"id": "bitter", "k": 0.3}],
     "worth": 0.7},
]
# what a food item is: nutrition and how long it keeps (fields of items.json)
ITEM_FIELDS = {
    "salted_meat": {"nutrition": 0.8, "spoil_days": 200},
    "salted_fish": {"nutrition": 0.25, "spoil_days": 160},
    "sauerkraut": {"nutrition": 0.2, "spoil_days": 150},
    "salted_mushrooms": {"nutrition": 0.15, "spoil_days": 150},
    "rusks": {"nutrition": 0.4, "spoil_days": 240},
    "lard": {"nutrition": 0.7, "spoil_days": 180},
}
ITEMS += [
    ("brine", "brine", ["liquid", "craft_raw"], 20, 0.1, 1.0, [1, 1], ru("рассол", "рассола", "рассолу", "рассол", "рассолом", "рассоле", "m"), "brine"),
    ("yarrow", "yarrow", ["plant", "herb"], 20, 0.3, 0.1, [1, 1], ru("тысячелистник", "тысячелистника", "тысячелистнику", "тысячелистник", "тысячелистником", "тысячелистнике", "m"), "yarrow"),
    ("salted_meat", None, ["food", "preserved"], 10, 2, 0.5, [1, 1], ru("солонина", "солонины", "солонине", "солонину", "солониной", "солонине", "f"), "salt meat"),
    ("salted_fish", None, ["food", "preserved"], 10, 1, 0.3, [1, 1], ru("солёная рыба", "солёной рыбы", "солёной рыбе", "солёную рыбу", "солёной рыбой", "солёной рыбе", "f"), "salt fish"),
    ("sauerkraut", None, ["food", "preserved"], 10, 1, 0.5, [1, 1], ru("квашеная капуста", "квашеной капусты", "квашеной капусте", "квашеную капусту", "квашеной капустой", "квашеной капусте", "f"), "sauerkraut"),
    ("salted_mushrooms", None, ["food", "preserved"], 10, 1, 0.3, [1, 1], ru("солёные грибы", "солёных грибов", "солёным грибам", "солёные грибы", "солёными грибами", "солёных грибах", "pl"), "salted mushrooms"),
    ("rusks", None, ["food", "preserved"], 20, 0.5, 0.2, [1, 1], ru("сухари", "сухарей", "сухарям", "сухари", "сухарями", "сухарях", "pl"), "rusks"),
    ("lard", None, ["food", "preserved"], 10, 1.5, 0.4, [1, 1], ru("сало", "сала", "салу", "сало", "салом", "сале", "n"), "salt lard"),
]
TRADE_GOODS = {"beeswax", "leather", "woad", "madder", "dagger", "longbow", "tin_ore", "yew_stave"}  # pack items
ANNOTATE += [
    {"id": "salt", "material": "salt", "value": 3, "add_tags": ["trade_good"]},
    {"id": "silver", "add_tags": ["trade_good"]},
    {"id": "gold", "add_tags": ["trade_good"]},
    {"id": "sword", "add_tags": ["trade_good"]},
    {"id": "axe", "add_tags": ["trade_good"]},
]
OBJECTS += [
    obj("salt_spring", {"brine": 12}, ["marsh", "river", "meadow"], 0.15, regrow={"brine": 3}, every=144, stand=0.8, placed_last=True,
        _doc="солёный родник, редкий (0-1 на карту): рассол, соль выпаркой; ценный товар и повод для споров"),
    obj("yarrow_patch", {"yarrow": 5}, ["meadow"], 4, regrow={"yarrow": 1}, every=144, stand=0.6, placed_last=True,
        _doc="тысячелистник (из старых таблиц): трава от ран"),
]
_HEARTH = ["target_owner:family"]
def _cook(id_, uses, gives, dur, ru_name, en_name, skill="cooking"):
    pre = [f"has:{i}>={n}" for i, n in uses] + _HEARTH
    eff = [f"has:{i}-{n}" for i, n in uses] + [f"has:{i}+{n}" for i, n in gives] + [f"skill:{skill}+0.01"]
    return work(id_, "hearth", pre, eff, dur, reserve=False, ru_name=ru_name, en_name=en_name)
def _eat(item, hunger, ru_name, en_name):
    a = {"id": f"eat_{item}", "category": "household", "target": "self", "pre": [f"has:{item}>=1"],
         "eff": [f"has:{item}-1", f"need:hunger+{hunger}", "moodlet:ate_meal"], "duration": 1, "cost": 1,
         "_name": (ru_name, en_name)}
    return a
INTERACTIONS += [
    take("draw_brine", "salt_spring", "brine", 4, 2, ru_name="Набрать рассола", en_name="Draw brine"),
    _cook("boil_brine", [("brine", 4), ("firewood", 2)], [("salt", 2)], 8, "Выпарить соль", "Boil down salt"),
    _cook("salt_meat", [("meat", 3), ("salt", 1)], [("salted_meat", 3)], 3, "Засолить мясо", "Salt meat"),
    _cook("salt_fish", [("fish", 3), ("salt", 1)], [("salted_fish", 3)], 3, "Засолить рыбу", "Salt fish"),
    _cook("make_sauerkraut", [("vegetables", 4), ("salt", 1)], [("sauerkraut", 4)], 3, "Заквасить капусту", "Make sauerkraut"),
    _cook("salt_mushrooms", [("mushrooms", 4), ("salt", 1)], [("salted_mushrooms", 4)], 3, "Засолить грибы", "Salt mushrooms"),
    _cook("dry_rusks", [("bread", 2), ("firewood", 1)], [("rusks", 3)], 4, "Насушить сухарей", "Dry rusks"),
    _cook("salt_lard", [("animal_fat", 2), ("salt", 1)], [("lard", 2)], 2, "Засолить сало", "Salt lard"),
    _eat("salted_meat", 0.7, "Съесть солонины", "Eat salt meat"),
    _eat("salted_fish", 0.3, "Съесть солёной рыбы", "Eat salt fish"),
    _eat("sauerkraut", 0.25, "Съесть квашеной капусты", "Eat sauerkraut"),
    _eat("salted_mushrooms", 0.2, "Съесть солёных грибов", "Eat salted mushrooms"),
    _eat("rusks", 0.4, "Погрызть сухарей", "Eat rusks"),
    _eat("lard", 0.6, "Съесть сала", "Eat lard"),
    work("tan_hides_bark", "tanning_pit", ["has:hide>=2", "has:oak_bark>=2"],
         ["has:hide-2", "has:oak_bark-2", "has:leather+2", "skill:tanning+0.015", "body:fatigue+0.08"], 16, 12,
         ru_name="Выдубить кожи корой", en_name="Tan hides with bark"),
    work("tan_hides_lime", "tanning_pit", ["has:hide>=2", "has:quicklime>=1"],
         ["has:hide-2", "has:quicklime-1", "has:leather+2", "skill:tanning+0.015", "body:fatigue+0.08"], 12, 10,
         ru_name="Выдубить кожи известью", en_name="Tan hides with lime"),
    take("gather_yarrow", "yarrow_patch", "yarrow", 2, 2, skill="herbalism", ru_name="Нарвать тысячелистника", en_name="Gather yarrow"),
]
# a cup is a cup: carved of horn it is a horn cup and worth what horn is (AL-87)
CARVE_SHAPES = {"bone": ["cup_raw"]}
NEW_DESIGNS += [
    {"id": "horn_cup", "form": "wooden_cup", "steps": [
        {"process": "carve", "shape": "cup_raw", "slots": {"work": ["mat:horn"]}}]},
    {"id": "yarrow_poultice", "form": "poultice", "steps": [
        {"process": "mix", "mode": "grind_herbs", "slots": {"a": ["mat:yarrow"], "b": ["mat:garlic"]}}]},
]
DESIGN_NAMES.update({"horn_cup": ("роговой кубок", "horn cup"), "yarrow_poultice": ("припарка из тысячелистника", "yarrow poultice")})
OBJECT_NAMES.update({
    "salt_spring": (ru("солёный родник", "солёного родника", "солёному роднику", "солёный родник", "солёным родником", "солёном роднике", "m"), "salt spring"),
    "yarrow_patch": (ru("тысячелистник", "тысячелистника", "тысячелистнику", "тысячелистник", "тысячелистником", "тысячелистнике", "m"), "yarrow patch"),
})
MATERIAL_NAMES.update({
    "brine": ("рассол", "m", "{form} из рассола", "brine", "brine {form}"),
    "yarrow": ("тысячелистник", "m", "{form} из тысячелистника", "yarrow", "yarrow {form}"),
})

# ---------------------------------------------------------------- 2D client: glyphs and sprites
# Every object and item of the pack gets a record for the 2D client (gdd/ui/client_2d):
# a base sprite of the 16x16 set, recoloured by palette keys (sprites.py PAL), and a glyph
# with its colour for the symbol mode. The atlas itself stays local (LFS, 24.11).
SPRITE_RULES = [  # first match by id or tag: (test, sprite, recolour, glyph, colour)
    (lambda i, t: i.endswith("_tree"), "ob_tree", {}, "♣", "G"),
    (lambda i, t: i == "pine_tree", "ob_pine", {}, "♠", "D"),
    (lambda i, t: i in ("willow_stand", "wild_hemp", "dye_meadow", "poppy_patch", "hemlock_stand", "hellebore_patch",
                        "ramsons", "hop_vine", "yarrow_patch"), "ob_bush", {}, "\"", "l"),
    (lambda i, t: i in ("flint_scree", "limestone_outcrop", "copper_vein"), "ob_rock", {}, "▲", "W"),
    (lambda i, t: i in ("sand_bar", "bog_ore_bed", "peat_bog", "salt_spring"), "tl_sand", {}, "≈", "t"),
    (lambda i, t: i in ("charcoal_clamp", "lime_kiln", "tanning_pit"), "ob_hearth", {}, "◘", "M"),
    (lambda i, t: i == "beehive", "ob_barrel", {"n": "t", "N": "T"}, "♥", "y"),
    (lambda i, t: "ore" in t or i in ("flint", "limestone", "quicklime", "sand", "peat"), "it_ore", {}, "*", "W"),
    (lambda i, t: "herb" in t or "plant" in t or "dye" in t, "it_herb", {}, "♣", "l"),
    (lambda i, t: "wood" in t or "bark" in t, "it_herb", {"G": "n", "l": "o", "D": "N"}, "/", "n"),
    (lambda i, t: "food" in t, "it_bread", {}, "%", "o"),
    (lambda i, t: "weapon" in t, "it_sword", {}, "†", "m"),
    (lambda i, t: "tool" in t, "it_hammer", {}, "τ", "M"),
    (lambda i, t: True, "ob_sack", {}, "•", "t"),
]
COLOUR_OF = {"woad": "b", "madder": "r", "weld": "y", "gold": "H", "silver": "m", "copper_ore": "z", "salt": "w",
             "beeswax": "y", "tallow": "w", "poppy": "r", "hellebore": "G", "hemlock": "w", "brine": "U"}


# ---------------------------------------------------------------- build
def text_link(path, pairs, field):
    """Insert "field": "value" right after {"id": "<id>" on one-object-per-line files."""
    t = open(path, encoding="utf-8").read()
    for k, v in pairs.items():
        m = re.search(r'\{"id": "%s"[^\n]*' % re.escape(k), t)
        assert m, (path, k)
        line = m.group(0)
        if f'"{field}":' in line:
            line2 = re.sub(r'"%s": "[^"]*"' % field, f'"{field}": "{v}"', line, count=1)
        else:
            line2 = line.replace('{"id": "%s"' % k, '{"id": "%s", "%s": "%s"' % (k, field, v), 1)
        t = t.replace(line, line2, 1)
    json.loads(t)
    open(path, "w", encoding="utf-8").write(t)


def add_forms(path, forms):
    """Append archetypes missing from a one-object-per-line forms file."""
    t = open(path, encoding="utf-8").read()
    for fid, line in forms.items():
        if re.search(r'\{"id": "%s"' % re.escape(fid), t):
            continue
        end = t.rstrip().rfind("]")
        start = t.rfind("\n", 0, end)  # the line of the closing bracket keeps its indent
        head = t[:start].rstrip()
        t = head + ",\n  " + line + t[start:]
    json.loads(t)
    open(path, "w", encoding="utf-8").write(t)


def add_shape_map(path, proc, extra):
    """Add shapes a process may make from an input shape (one process per line)."""
    t = open(path, encoding="utf-8").read()
    m = re.search(r'\{"id": "%s".*?"shape_map": (\{[^\n]*?\})' % re.escape(proc), t, re.S)
    assert m, proc
    sm = json.loads(m.group(1))
    for src, outs in extra.items():
        sm.setdefault(src, [])
        sm[src] += [o for o in outs if o not in sm[src]]
    t = t[:m.start(1)] + json.dumps(sm, ensure_ascii=False) + t[m.end(1):]
    json.loads(t)
    open(path, "w", encoding="utf-8").write(t)


def sprite_of(iid, tags):
    for test, sprite, recolour, glyph, colour in SPRITE_RULES:
        if test(iid, tags):
            return {"sprite": sprite, "recolour": recolour, "glyph": glyph, "colour": COLOUR_OF.get(iid, colour)}


def main():
    # craft materials
    mp = P("content/ai/craft/materials.json")
    md = load(mp)
    have = {m["id"]: m for m in md["materials"]}
    for nm in NEW_MATERIALS:
        if nm["id"] in have:
            have[nm["id"]].clear()
            have[nm["id"]].update(nm)
        else:
            md["materials"].append(nm)
    have = {m["id"]: m for m in md["materials"]}
    for mat, item in MATERIAL_ITEMS.items():
        have[mat]["item"] = item
    for mat, heat in MATERIAL_HEAT.items():
        have[mat]["heat"] = heat
    for mat, tags in MATERIAL_TAGS.items():
        have[mat]["tags"] += [t for t in tags if t not in have[mat]["tags"]]
    save(mp, md)
    add_forms(P("content/ai/craft/forms.json"), NEW_FORMS)
    add_shape_map(P("content/ai/craft/processes.json"), "carve", CARVE_SHAPES)
    text_link(P("content/ai/craft/forms.json"), FORM_ITEMS, "item")
    dp = P("content/ai/craft/designs.json")
    dd = load(dp)
    dl = {x["id"]: i for i, x in enumerate(dd["designs"])}
    for ds in NEW_DESIGNS:
        if ds["id"] in dl:
            dd["designs"][dl[ds["id"]]] = ds
        else:
            dd["designs"].append(ds)
    save(dp, dd)
    ip = P("content/ai/interactions.json")
    idd = load(ip)
    for a in idd["interactions"]:
        for e in BASE_EFFECTS.get(a["id"], []):
            if e not in a["eff"]:
                a["eff"].append(e)
    save(ip, idd)

    # items: only the ones no other file has
    base = set()
    for f in ("items.json", "expansion/items.json", "cuisine/items.json"):
        base |= {x["id"] for x in load(P("content/ai", f))["items"]}
    pack_items, gear, loc_ru, loc_en = [], {}, {}, {}
    for (iid, mat, tags, stack, value, kg, size, rn, en) in ITEMS:
        if iid in base:
            continue
        e = {"id": iid}
        if mat:
            e["material"] = mat
        e.update({"tags": tags + (["trade_good"] if iid in TRADE_GOODS and "trade_good" not in tags else []),
                  "stack": stack, "value": value})
        e.update(ITEM_FIELDS.get(iid, {}))
        pack_items.append(e)
        gear[iid] = {"kg": kg, "size": size}
        loc_ru[f"item.{iid}.name"] = rn
        loc_en[f"item.{iid}.name"] = en
    save(P("content/ai/resources/items.json"),
         {"_doc": "AL-86 resources pack: raw things from the world and the items the craft archetypes count as. "
                  "Generated by tools/content_gen/gen_resources.py; merged like the expansion (a base id wins).",
          "items": pack_items, "annotate": ANNOTATE})
    objects = [dict(o) for o in OBJECTS]
    save(P("content/ai/resources/objects.json"),
         {"_doc": "AL-86 resources pack: sources in the world (tree species, deposits, yard kilns). "
                  "Generated by tools/content_gen/gen_resources.py.", "objects": objects})
    inter = []
    for a in INTERACTIONS:
        a = dict(a)
        rn, en = a.pop("_name")
        inter.append(a)
        loc_ru[f"interaction.{a['id']}.name"] = rn
        loc_en[f"interaction.{a['id']}.name"] = en
    save(P("content/ai/resources/interactions.json"),
         {"_doc": "AL-86 resources pack: getting things from the sources. Generated by tools/content_gen/gen_resources.py.",
          "interactions": inter})
    for oid, (rn, en) in OBJECT_NAMES.items():
        loc_ru[f"object.{oid}.name"] = rn
        loc_en[f"object.{oid}.name"] = en
    for fid, (nom, g, en) in FORM_NAMES.items():
        loc_ru[f"form.{fid}.name"] = {"nom": nom, "g": g}
        loc_en[f"form.{fid}.name"] = en
    for did, (rn, en) in DESIGN_NAMES.items():
        loc_ru[f"interaction.craft:{did}.name"] = "сделать: " + rn
        loc_en[f"interaction.craft:{did}.name"] = "make: " + en
    for mid, (nom, g, made, en, enmade) in MATERIAL_NAMES.items():
        loc_ru[f"material.{mid}.name"] = {"nom": nom, "g": g}
        loc_ru[f"material.{mid}.made"] = made
        loc_en[f"material.{mid}.name"] = en
        loc_en[f"material.{mid}.made"] = enmade
    # gear: weights of the pack items (gear.json is the one weight table)
    gp = P("content/ai/gear.json")
    gd = load(gp)
    for k, v in gear.items():
        gd["items"].setdefault(k, v)
    save(gp, gd)
    sprites = {"_doc": "AL-86: 2D client records of the resources pack (gdd/ui/client_2d): base sprite of the 16x16 "
                       "set, recolour by palette keys of sprites.py, glyph and colour for the symbol mode. "
                       "Generated by tools/content_gen/gen_resources.py.", "objects": {}, "items": {}}
    for o in OBJECTS:
        sprites["objects"][o["id"]] = sprite_of(o["id"], [])
    for (iid, mat, tags, *_rest) in ITEMS:
        sprites["items"][iid] = sprite_of(iid, tags)
    save(P("content/sprites_2d/resources.json"), sprites)
    for lang, src in (("ru", loc_ru), ("en", loc_en)):
        p = P("content/loc", lang, "resources.json")
        d = load(p) if os.path.exists(p) else {}
        d.update(src)
        save(p, d)
    print(f"items {len(pack_items)}, objects {len(objects)}, interactions {len(inter)}")


if __name__ == "__main__":
    main()
