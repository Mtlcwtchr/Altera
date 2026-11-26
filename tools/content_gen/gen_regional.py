#!/usr/bin/env python3
"""Regional pack (LR-168): the things of the belts of the mythic world (setting v0.3, LR-164).

The world is sewn of belts (content/species/cultures/*.json field `belt`, LR-166):
mediterranean (Greek), nile (Egyptian), forest (Slavic and Celtic woods), north (Norse),
steppe (Scythian-Turkic). Each wears, arms, eats, trades and offers to its gods its own
way. This script is the source of truth for:

  content/ai/regional/items.json        new things + annotate (offering tags on old ones)
  content/ai/regional/interactions.json their crafts (plain GOAP acts, as sew_tunic) and sources
  content/ai/regional/objects.json      belt-bound sources (olive grove, vineyard, date palm, ...)
  content/ai/regional/knowledge.json    the crafts of each belt (craft_<belt>)
  content/ai/regional/regions.json      per belt: clothes by sex, know-how, native things, offerings
  content/loc/{ru,en}/regional.json     names
  content/sprites_2d/resources.json     2D records of the pack's items/objects ("pack": "regional")
  links: content/ai/gear.json items, content/ai/caravans.json goods, content/combat/*.json and
         sim_bridge.json (weapons, armour, shields of the belts)

Run from the repo root:  python tools/content_gen/gen_regional.py   (idempotent)
"""
import json
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
P = lambda *a: os.path.join(ROOT, *a)
BELTS = ["mediterranean", "nile", "forest", "north", "steppe"]
ALL = BELTS


def load(p):
    with open(p, encoding="utf-8") as f:
        return json.load(f)


def save(p, d):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(json.dumps(d, ensure_ascii=False, indent=1) + "\n")


def ru(nom, gen, dat, acc, ins, pre, g):
    return {"nom": nom, "gen": gen, "dat": dat, "acc": acc, "ins": ins, "pre": pre, "g": g}


def W(slot, layer, warmth=0.0, bulk=1):
    return (slot, layer, warmth, bulk)


# ---------------------------------------------------------------- items
# id, belts, tags, value, kg, size, wear, ru, en, (sprite, recolour), extra fields
I = []


def item(iid, belts, tags, value, kg, size, wear, r, en, spr, **extra):
    I.append(dict(id=iid, belts=belts, tags=tags, value=value, kg=kg, size=size, wear=wear, ru=r, en=en,
                  spr=spr, extra=extra))


MED, NIL, FOR, NOR, STP = ["mediterranean"], ["nile"], ["forest"], ["north"], ["steppe"]

# ---- clothing: mediterranean
item("chiton", MED, ["clothing"], 7, 0.5, [2, 2], W("torso", "under", 0.25),
     ru("хитон", "хитона", "хитону", "хитон", "хитоном", "хитоне", "m"), "chiton", ("it_tunic", {"e": "w", "E": "g"}))
item("peplos", MED, ["clothing"], 9, 0.7, [2, 2], W("torso", "under", 0.3),
     ru("пеплос", "пеплоса", "пеплосу", "пеплос", "пеплосом", "пеплосе", "m"), "peplos", ("it_tunic", {"e": "u", "E": "B"}))
item("himation", MED, ["clothing"], 11, 1.2, [2, 3], W("torso", "over", 0.55),
     ru("гиматий", "гиматия", "гиматию", "гиматий", "гиматием", "гиматии", "m"), "himation", ("it_cloak", {"r": "w", "R": "g"}))
item("chlamys", MED, ["clothing"], 9, 0.8, [2, 2], W("torso", "over", 0.4),
     ru("хламида", "хламиды", "хламиде", "хламиду", "хламидой", "хламиде", "f"), "chlamys", ("it_cloak", {}))
item("sandals", MED + NIL, ["clothing"], 4, 0.4, [2, 1], W("feet", "shoes", 0.05),
     ru("сандалии", "сандалий", "сандалиям", "сандалии", "сандалиями", "сандалиях", "pl"), "sandals", ("it_boots", {"e": "t", "E": "T"}))
item("petasos", MED, ["clothing"], 3, 0.3, [2, 1], W("head", "cap", 0.05),
     ru("петас", "петаса", "петасу", "петас", "петасом", "петасе", "m"), "petasos", ("it_cap", {"e": "t", "E": "T"}))
# ---- clothing: nile
item("shendyt", NIL, ["clothing"], 5, 0.3, [2, 1], W("legs", "under", 0.05),
     ru("схенти", "схенти", "схенти", "схенти", "схенти", "схенти", "n"), "shendyt", ("it_trousers", {"e": "w", "E": "g"}))
item("kalasiris", NIL, ["clothing"], 9, 0.5, [2, 2], W("torso", "under", 0.15),
     ru("калазирис", "калазириса", "калазирису", "калазирис", "калазирисом", "калазирисе", "m"), "kalasiris", ("it_tunic", {"e": "w", "E": "w"}))
item("broad_collar", NIL, ["jewelry", "valuable"], 18, 0.4, [2, 1], None,
     ru("ожерелье-воротник", "ожерелья-воротника", "ожерелью-воротнику", "ожерелье-воротник", "ожерельем-воротником", "ожерелье-воротнике", "n"),
     "broad collar", ("it_charm", {"r": "u", "R": "H"}))
# ---- clothing: forest
item("sarafan", FOR, ["clothing"], 10, 0.9, [2, 3], W("torso", "over", 0.45),
     ru("сарафан", "сарафана", "сарафану", "сарафан", "сарафаном", "сарафане", "m"), "sarafan", ("it_cloak", {"r": "r", "R": "R"}))
item("bast_shoes", FOR, ["clothing"], 1, 0.3, [2, 1], W("feet", "shoes", 0.1),
     ru("лапти", "лаптей", "лаптям", "лапти", "лаптями", "лаптях", "pl"), "bast shoes", ("it_boots", {"e": "t", "E": "o"}))
item("plaid_cloak", FOR, ["clothing"], 12, 1.5, [2, 3], W("torso", "over", 0.7),
     ru("клетчатый плащ", "клетчатого плаща", "клетчатому плащу", "клетчатый плащ", "клетчатым плащом", "клетчатом плаще", "m"),
     "plaid cloak", ("it_cloak", {"r": "G", "R": "b"}))
item("flower_wreath", FOR + MED, ["clothing", "festive"], 1, 0.1, [1, 1], W("head", "cap", 0.0),
     ru("венок", "венка", "венку", "венок", "венком", "венке", "m"), "flower wreath", ("it_herb", {"G": "y", "l": "r"}), spoil_days=6)
# ---- clothing: north
item("fur_cloak", NOR, ["clothing", "valuable"], 18, 2.5, [2, 3], W("torso", "over", 0.95),
     ru("меховой плащ", "мехового плаща", "меховому плащу", "меховой плащ", "меховым плащом", "меховом плаще", "m"), "fur cloak",
     ("it_cloak", {"r": "e", "R": "E"}))
item("fur_hat", NOR + STP, ["clothing"], 6, 0.4, [1, 1], W("head", "cap", 0.5),
     ru("меховая шапка", "меховой шапки", "меховой шапке", "меховую шапку", "меховой шапкой", "меховой шапке", "f"), "fur hat",
     ("it_cap", {"e": "e", "E": "E"}))
item("apron_dress", NOR, ["clothing"], 10, 1.0, [2, 2], W("torso", "over", 0.45),
     ru("сарафан-фартук", "сарафана-фартука", "сарафану-фартуку", "сарафан-фартук", "сарафаном-фартуком", "сарафане-фартуке", "m"),
     "apron dress", ("it_tunic", {"e": "b", "E": "B"}))
item("brooch", NOR + FOR, ["jewelry"], 6, 0.05, [1, 1], None,
     ru("фибула", "фибулы", "фибуле", "фибулу", "фибулой", "фибуле", "f"), "brooch", ("it_coin", {}))
# ---- clothing: steppe
item("kaftan", STP, ["clothing"], 12, 1.4, [2, 3], W("torso", "over", 0.65),
     ru("кафтан", "кафтана", "кафтану", "кафтан", "кафтаном", "кафтане", "m"), "kaftan", ("it_cloak", {"r": "o", "R": "n"}))
item("felt_hood", STP, ["clothing"], 4, 0.3, [1, 1], W("head", "cap", 0.45),
     ru("башлык", "башлыка", "башлыку", "башлык", "башлыком", "башлыке", "m"), "felt hood", ("it_cap", {"e": "r", "E": "R"}))
item("riding_trousers", STP, ["clothing"], 7, 0.6, [2, 2], W("legs", "under", 0.35),
     ru("шаровары", "шаровар", "шароварам", "шаровары", "шароварами", "шароварах", "pl"), "riding trousers",
     ("it_trousers", {"e": "o", "E": "n"}))
item("soft_boots", STP, ["clothing"], 8, 0.8, [2, 2], W("feet", "shoes", 0.25),
     ru("ичиги", "ичигов", "ичигам", "ичиги", "ичигами", "ичигах", "pl"), "soft boots", ("it_boots", {"e": "r", "E": "R"}))

# ---- armour (combat: sim_bridge armor_of_item)
item("linothorax", MED, ["armour"], 22, 4.0, [2, 3], W("torso", "armour", 0.1, 2),
     ru("льняной панцирь", "льняного панциря", "льняному панцирю", "льняной панцирь", "льняным панцирем", "льняном панцире", "m"),
     "linothorax", ("it_mail", {"m": "w", "M": "g"}), armor="linen")
item("bronze_cuirass", MED, ["armour", "valuable"], 55, 8.0, [2, 3], W("torso", "armour", 0.0, 2),
     ru("бронзовый панцирь", "бронзового панциря", "бронзовому панцирю", "бронзовый панцирь", "бронзовым панцирем", "бронзовом панцире", "m"),
     "bronze cuirass", ("it_mail", {"m": "y", "M": "o"}), armor="bronze_plate")
item("bronze_greaves", MED, ["armour"], 20, 2.5, [2, 2], W("legs", "armour", 0.0),
     ru("поножи", "поножей", "поножам", "поножи", "поножами", "поножах", "pl"), "bronze greaves",
     ("it_boots", {"e": "y", "E": "o"}), armor="greaves")
item("crested_helm", MED, ["armour"], 30, 2.2, [2, 2], W("head", "helm", 0.0),
     ru("гребенчатый шлем", "гребенчатого шлема", "гребенчатому шлему", "гребенчатый шлем", "гребенчатым шлемом", "гребенчатом шлеме", "m"),
     "crested helm", ("it_helmet", {"m": "y", "M": "o"}), armor="helmet")
item("scale_armour", STP + NIL, ["armour"], 45, 9.0, [2, 3], W("torso", "armour", 0.1, 2),
     ru("чешуйчатый доспех", "чешуйчатого доспеха", "чешуйчатому доспеху", "чешуйчатый доспех", "чешуйчатым доспехом", "чешуйчатом доспехе", "m"),
     "scale armour", ("it_scale", {}), armor="scale")
item("nasal_helm", NOR + FOR, ["armour"], 26, 1.8, [2, 2], W("head", "helm", 0.0),
     ru("шлем с наносником", "шлема с наносником", "шлему с наносником", "шлем с наносником", "шлемом с наносником", "шлеме с наносником", "m"),
     "nasal helm", ("it_helmet", {}), armor="helmet")
item("pointed_helm", STP, ["armour"], 24, 1.6, [2, 2], W("head", "helm", 0.0),
     ru("остроконечный шлем", "остроконечного шлема", "остроконечному шлему", "остроконечный шлем", "остроконечным шлемом", "остроконечном шлеме", "m"),
     "pointed helm", ("it_helmet", {"m": "o", "M": "n"}), armor="helmet")

# ---- shields (combat: sim_bridge shield_of_item)
item("hoplon", MED, ["shield", "weapon"], 18, 7.0, [3, 3], None,
     ru("гоплон", "гоплона", "гоплону", "гоплон", "гоплоном", "гоплоне", "m"), "hoplon", ("ic_shield", {}), shield="hoplon")
item("round_shield", NOR + FOR, ["shield", "weapon"], 9, 4.0, [3, 3], None,
     ru("круглый щит", "круглого щита", "круглому щиту", "круглый щит", "круглым щитом", "круглом щите", "m"), "round shield",
     ("ic_shield", {}), shield="round")
item("oval_shield", FOR, ["shield", "weapon"], 10, 4.5, [2, 3], None,
     ru("овальный щит", "овального щита", "овальному щиту", "овальный щит", "овальным щитом", "овальном щите", "m"), "oval shield",
     ("ic_shield", {}), shield="kite")
item("wicker_shield", STP + NIL, ["shield", "weapon"], 4, 2.0, [2, 3], None,
     ru("плетёный щит", "плетёного щита", "плетёному щиту", "плетёный щит", "плетёным щитом", "плетёном щите", "m"), "wicker shield",
     ("ic_shield", {}), shield="wicker")

# ---- weapons (combat: sim_bridge weapon_of_item)
item("dory", MED, ["weapon", "tool"], 12, 2.4, [1, 4], None,
     ru("копьё-дори", "копья-дори", "копью-дори", "копьё-дори", "копьём-дори", "копье-дори", "n"), "dory", ("it_spear", {}), weapon="spear")
item("xiphos", MED, ["weapon"], 18, 0.9, [1, 3], None,
     ru("ксифос", "ксифоса", "ксифосу", "ксифос", "ксифосом", "ксифосе", "m"), "xiphos", ("it_sword", {"m": "y", "M": "o"}), weapon="sword")
item("kopis", MED, ["weapon"], 20, 1.2, [1, 3], None,
     ru("копис", "кописа", "копису", "копис", "кописом", "кописе", "m"), "kopis", ("it_sword", {}), weapon="sword")
item("sling", MED + NIL, ["weapon", "ranged"], 1, 0.1, [1, 1], None,
     ru("праща", "пращи", "праще", "пращу", "пращой", "праще", "f"), "sling", ("it_rope", {}))
item("khopesh", NIL, ["weapon", "valuable"], 22, 1.3, [1, 3], None,
     ru("хопеш", "хопеша", "хопешу", "хопеш", "хопешем", "хопеше", "m"), "khopesh", ("it_sickle", {"m": "y", "M": "o"}), weapon="sword")
item("seax", NOR, ["weapon", "tool"], 8, 0.5, [1, 2], None,
     ru("скрамасакс", "скрамасакса", "скрамасаксу", "скрамасакс", "скрамасаксом", "скрамасаксе", "m"), "seax", ("it_knife", {}), weapon="dagger")
item("bearded_axe", NOR, ["weapon", "tool"], 14, 1.6, [1, 3], None,
     ru("бородовидная секира", "бородовидной секиры", "бородовидной секире", "бородовидную секиру", "бородовидной секирой", "бородовидной секире", "f"),
     "bearded axe", ("it_axe", {}), weapon="axe")
item("akinakes", STP, ["weapon", "tool"], 10, 0.6, [1, 2], None,
     ru("акинак", "акинака", "акинаку", "акинак", "акинаком", "акинаке", "m"), "akinakes", ("it_knife", {"m": "H"}), weapon="dagger")
item("sagaris", STP, ["weapon"], 12, 1.2, [1, 3], None,
     ru("сагарис", "сагариса", "сагарису", "сагарис", "сагарисом", "сагарисе", "m"), "sagaris", ("it_axe", {"m": "o"}), weapon="axe")
item("composite_bow", STP, ["weapon", "ranged", "valuable"], 16, 0.7, [1, 3], None,
     ru("сложный лук", "сложного лука", "сложному луку", "сложный лук", "сложным луком", "сложном луке", "m"), "composite bow",
     ("ic_bow", {}))

# ---- metals of the era
item("bronze_ingot", ALL, ["metal", "craft_raw", "trade_good"], 6, 1.0, [1, 1], None,
     ru("бронзовый слиток", "бронзового слитка", "бронзовому слитку", "бронзовый слиток", "бронзовым слитком", "бронзовом слитке", "m"),
     "bronze ingot", ("it_ingot", {"m": "y", "M": "o"}), material="bronze")
item("iron_bar", ALL, ["metal", "craft_raw", "trade_good"], 4, 1.0, [1, 1], None,
     ru("железная крица", "железной крицы", "железной крице", "железную крицу", "железной крицей", "железной крице", "f"),
     "iron bloom", ("it_ingot", {}), material="wrought_iron")

# ---- food and drink
item("olives", MED, ["food", "fruit"], 1, 0.3, [1, 1], None,
     ru("оливки", "оливок", "оливкам", "оливки", "оливками", "оливках", "pl"), "olives", ("it_berries", {"v": "D", "V": "K"}),
     nutrition=0.12, spoil_days=60)
item("olive_oil", MED, ["food", "trade_good", "fuel"], 4, 1.0, [1, 2], None,
     ru("оливковое масло", "оливкового масла", "оливковому маслу", "оливковое масло", "оливковым маслом", "оливковом масле", "n"),
     "olive oil", ("it_jar", {"y": "l"}), nutrition=0.3, spoil_days=500)
item("grapes", MED, ["food", "fruit", "sweet"], 1, 0.4, [1, 1], None,
     ru("виноград", "винограда", "винограду", "виноград", "виноградом", "винограде", "m"), "grapes", ("it_berries", {"v": "v", "V": "V"}),
     nutrition=0.12, spoil_days=10)
item("figs", MED, ["food", "fruit", "sweet"], 1.5, 0.3, [1, 1], None,
     ru("смоквы", "смокв", "смоквам", "смоквы", "смоквами", "смоквах", "pl"), "figs", ("it_apple", {"r": "v", "R": "V"}),
     nutrition=0.15, spoil_days=60)
item("dates", NIL, ["food", "fruit", "sweet", "trade_good"], 1.5, 0.3, [1, 1], None,
     ru("финики", "фиников", "финикам", "финики", "финиками", "финиках", "pl"), "dates", ("it_berries", {"v": "n", "V": "N"}),
     nutrition=0.2, spoil_days=200)
item("flatbread", NIL + STP, ["food", "cooked"], 2, 0.3, [1, 1], None,
     ru("лепёшка", "лепёшки", "лепёшке", "лепёшку", "лепёшкой", "лепёшке", "f"), "flatbread", ("it_bread", {"y": "t"}),
     nutrition=0.35, spoil_days=8)
item("barley_beer", NIL, ["drink", "alcohol"], 2, 1.0, [1, 2], None,
     ru("ячменное пиво", "ячменного пива", "ячменному пиву", "ячменное пиво", "ячменным пивом", "ячменном пиве", "n"), "barley beer",
     ("it_ale", {}), nutrition=0.1, spoil_days=20)
item("kumis", STP, ["drink", "alcohol"], 3, 1.0, [1, 2], None,
     ru("кумыс", "кумыса", "кумысу", "кумыс", "кумысом", "кумысе", "m"), "kumis", ("it_bottle", {"D": "w", "l": "w", "K": "g"}),
     nutrition=0.15, spoil_days=12)
item("kvass", FOR, ["drink"], 1.5, 1.0, [1, 2], None,
     ru("квас", "кваса", "квасу", "квас", "квасом", "квасе", "m"), "kvass", ("it_ale", {"y": "n"}), nutrition=0.08, spoil_days=10)

# ---- goods, dyes, writing, reagents
item("amphora", MED, ["container", "trade_good"], 3, 4.0, [2, 3], None,
     ru("амфора", "амфоры", "амфоре", "амфору", "амфорой", "амфоре", "f"), "amphora", ("it_jug", {"e": "z", "E": "Z"}))
item("kylix", MED, ["container", "festive"], 3, 0.3, [1, 1], None,
     ru("килик", "килика", "килику", "килик", "киликом", "килике", "m"), "kylix", ("it_cup", {"e": "z", "E": "x"}))
item("drinking_horn", NOR + FOR, ["container", "festive"], 3, 0.3, [1, 2], None,
     ru("турий рог", "турьего рога", "турьему рогу", "турий рог", "турьим рогом", "турьем роге", "m"), "drinking horn", ("it_horn", {}))
item("purple_dye", MED, ["dye", "luxury", "trade_good"], 25, 0.1, [1, 1], None,
     ru("пурпур", "пурпура", "пурпуру", "пурпур", "пурпуром", "пурпуре", "m"), "purple dye", ("it_jar", {"y": "v"}))
item("papyrus", NIL, ["writing", "trade_good"], 3, 0.1, [1, 1], None,
     ru("папирус", "папируса", "папирусу", "папирус", "папирусом", "папирусе", "m"), "papyrus", ("it_scroll", {}))
item("felt", STP, ["textile", "craft_raw", "trade_good"], 3, 0.8, [2, 2], None,
     ru("войлок", "войлока", "войлоку", "войлок", "войлоком", "войлоке", "m"), "felt", ("it_cloth", {"w": "t", "g": "T"}))
item("sable_fur", NOR + FOR, ["textile", "luxury", "trade_good"], 14, 0.3, [1, 2], None,
     ru("соболья шкурка", "собольей шкурки", "собольей шкурке", "собольную шкурку", "собольей шкуркой", "собольей шкурке", "f"),
     "sable fur", ("it_pelt", {"W": "N", "g": "p", "M": "P"}))
item("amber", NOR + FOR, ["gem", "valuable", "trade_good"], 10, 0.05, [1, 1], None,
     ru("янтарь", "янтаря", "янтарю", "янтарь", "янтарём", "янтаре", "m"), "amber", ("it_gem", {"r": "o", "R": "f"}))
item("natron", NIL, ["mineral", "reagent", "trade_good"], 2, 1.0, [1, 1], None,
     ru("натрон", "натрона", "натрону", "натрон", "натроном", "натроне", "m"), "natron", ("it_flour", {}), material="natron")
item("frankincense", NIL + MED, ["incense", "luxury", "trade_good"], 12, 0.1, [1, 1], None,
     ru("ладан", "ладана", "ладану", "ладан", "ладаном", "ладане", "m"), "frankincense", ("it_gem", {"r": "t", "R": "T"}), material="frankincense")
item("myrrh", NIL, ["incense", "luxury", "trade_good", "reagent"], 14, 0.1, [1, 1], None,
     ru("смирна", "смирны", "смирне", "смирну", "смирной", "смирне", "f"), "myrrh", ("it_gem", {"r": "z", "R": "Z"}), material="myrrh")
item("silphium", MED, ["herb", "reagent", "valuable", "trade_good"], 15, 0.1, [1, 1], None,
     ru("сильфий", "сильфия", "сильфию", "сильфий", "сильфием", "сильфии", "m"), "silphium", ("it_herb", {"G": "y", "D": "o"}), material="silphium")
item("mandrake", MED, ["herb", "reagent", "poisonous"], 8, 0.2, [1, 1], None,
     ru("мандрагора", "мандрагоры", "мандрагоре", "мандрагору", "мандрагорой", "мандрагоре", "f"), "mandrake", ("it_root", {}), material="mandrake")
item("mistletoe", FOR, ["herb", "reagent", "sacred"], 6, 0.1, [1, 1], None,
     ru("омела", "омелы", "омеле", "омелу", "омелой", "омеле", "f"), "mistletoe", ("it_herb", {"l": "w"}), material="mistletoe")
item("henbane", FOR + NOR, ["herb", "reagent", "poisonous"], 3, 0.1, [1, 1], None,
     ru("белена", "белены", "белене", "белену", "беленой", "белене", "f"), "henbane", ("it_herb", {"G": "W", "l": "v"}), material="henbane")
item("wormwood", STP, ["herb", "reagent", "aromatic"], 1.5, 0.1, [1, 1], None,
     ru("полынь", "полыни", "полыни", "полынь", "полынью", "полыни", "f"), "wormwood", ("it_herb", {"G": "W", "l": "g"}), material="wormwood")

# ---- instruments
item("lyre", MED, ["leisure", "instrument"], 10, 1.0, [2, 2], None,
     ru("лира", "лиры", "лире", "лиру", "лирой", "лире", "f"), "lyre", ("it_horn", {"e": "t"}))
item("gusli", FOR, ["leisure", "instrument"], 8, 1.5, [2, 2], None,
     ru("гусли", "гуслей", "гуслям", "гусли", "гуслями", "гуслях", "pl"), "gusli", ("it_drum", {}))
item("kobyz", STP, ["leisure", "instrument"], 8, 1.2, [1, 3], None,
     ru("кобыз", "кобыза", "кобызу", "кобыз", "кобызом", "кобызе", "m"), "kobyz", ("it_flute", {"w": "n"}))

# ---- offerings and amulets (rites: tag offering + offering_<kind>)
item("votive_figurine", ALL, ["offering", "offering_votive", "craft"], 2, 0.2, [1, 1], None,
     ru("обетная фигурка", "обетной фигурки", "обетной фигурке", "обетную фигурку", "обетной фигуркой", "обетной фигурке", "f"),
     "votive figurine", ("it_idol", {}))
item("kyphi", NIL, ["incense", "offering", "offering_incense", "trade_good"], 8, 0.1, [1, 1], None,
     ru("кифи", "кифи", "кифи", "кифи", "кифи", "кифи", "n"), "kyphi", ("it_gem", {"r": "N", "R": "P"}))
item("scarab_amulet", NIL, ["jewelry", "amulet", "offering", "offering_treasure"], 6, 0.05, [1, 1], None,
     ru("амулет-скарабей", "амулета-скарабея", "амулету-скарабею", "амулет-скарабей", "амулетом-скарабеем", "амулете-скарабее", "m"),
     "scarab amulet", ("it_charm", {"r": "u", "R": "B"}))
item("silver_torc", FOR, ["jewelry", "valuable", "offering", "offering_treasure"], 30, 0.3, [1, 1], None,
     ru("серебряная гривна", "серебряной гривны", "серебряной гривне", "серебряную гривну", "серебряной гривной", "серебряной гривне", "f"),
     "silver torc", ("it_charm", {"r": "m", "R": "M"}))
item("arm_ring", NOR, ["jewelry", "valuable", "offering", "offering_treasure", "oath"], 20, 0.15, [1, 1], None,
     ru("клятвенное запястье", "клятвенного запястья", "клятвенному запястью", "клятвенное запястье", "клятвенным запястьем", "клятвенном запястье", "n"),
     "oath ring", ("it_charm", {"r": "m", "R": "M"}))
item("rune_charm", NOR, ["amulet", "offering", "offering_votive"], 3, 0.05, [1, 1], None,
     ru("рунный оберег", "рунного оберега", "рунному оберегу", "рунный оберег", "рунным оберегом", "рунном обереге", "m"),
     "rune charm", ("it_charm", {"r": "W", "R": "x"}))
item("obereg", FOR, ["amulet", "offering", "offering_votive"], 2, 0.05, [1, 1], None,
     ru("оберег", "оберега", "оберегу", "оберег", "оберегом", "обереге", "m"), "protective charm", ("it_charm", {"r": "r", "R": "R"}))
item("gold_plaque", STP, ["jewelry", "valuable", "offering", "offering_treasure"], 24, 0.05, [1, 1], None,
     ru("золотая бляшка", "золотой бляшки", "золотой бляшке", "золотую бляшку", "золотой бляшкой", "золотой бляшке", "f"),
     "gold plaque", ("it_coin", {"y": "H"}))
item("sacrificial_cake", MED + NIL, ["food", "offering", "offering_food"], 2, 0.2, [1, 1], None,
     ru("жертвенная лепёшка", "жертвенной лепёшки", "жертвенной лепёшке", "жертвенную лепёшку", "жертвенной лепёшкой", "жертвенной лепёшке", "f"),
     "sacrificial cake", ("it_bread", {"y": "H"}), nutrition=0.2, spoil_days=10)

ITEM_IDS = {x["id"] for x in I}

# Old things of the world get the offering tags (annotate) and their belts (regions native).
ANNOTATE = [
    ("wine", ["offering", "offering_libation"]), ("mead", ["offering", "offering_libation"]),
    ("milk", ["offering", "offering_libation"]), ("ale", ["offering", "offering_libation"]),
    ("honey", ["offering", "offering_food"]), ("bread", ["offering", "offering_food"]),
    ("honey_cake", ["offering", "offering_food"]), ("cheese", ["offering", "offering_food"]),
    ("candle", ["offering", "offering_light"]), ("beeswax", ["offering", "offering_light"]),
    ("silver", ["offering", "offering_treasure"]), ("gold", ["offering", "offering_treasure"]),
    ("coin", ["offering", "offering_treasure"]), ("bone_charm", ["amulet", "offering", "offering_votive"]),
    ("household_idol", ["offering_votive"]), ("carved_toy", ["offering", "offering_votive"]),
    ("olive_oil", ["offering", "offering_libation"]), ("kumis", ["offering", "offering_libation"]),
    ("barley_beer", ["offering", "offering_libation"]), ("frankincense", ["offering", "offering_incense"]),
    ("myrrh", ["offering", "offering_incense"]), ("amber", ["offering", "offering_treasure"]),
    ("mistletoe", ["offering", "offering_votive"]), ("flower_wreath", ["offering", "offering_votive"]),
    ("dates", ["offering", "offering_food"]), ("figs", ["offering", "offering_food"]),
]


# ---------------------------------------------------------------- craft materials of the reagents (alchemy: aspect.<id>)
MATERIALS = [
    {"id": "natron", "tags": ["mineral", "reagent", "preserve"], "p": {"purity": 0.7, "density": 0.6}, "shape": "powder",
     "effects": [{"id": "preserves", "k": 0.8}, {"id": "salty", "k": 0.4}, {"id": "aspect.death", "k": 0.2}, {"id": "aspect.water", "k": 0.15}],
     "worth": 1.4, "item": "natron"},
    {"id": "frankincense", "tags": ["resin", "incense", "reagent", "aromatic"], "p": {"flammable": 0.8, "purity": 0.85, "density": 0.4},
     "shape": "lump", "effects": [{"id": "fresh", "k": 0.5}, {"id": "aspect.light", "k": 0.35}, {"id": "aspect.bond", "k": 0.2}],
     "worth": 4.0, "item": "frankincense"},
    {"id": "myrrh", "tags": ["resin", "incense", "reagent", "aromatic"], "p": {"flammable": 0.7, "purity": 0.85, "density": 0.4},
     "shape": "lump", "effects": [{"id": "antiseptic", "k": 0.5}, {"id": "aspect.life", "k": 0.25}, {"id": "aspect.essence", "k": 0.25}],
     "worth": 4.5, "item": "myrrh"},
    {"id": "silphium", "tags": ["herb", "reagent", "aromatic"], "p": {"moisture": 0.4, "purity": 0.85}, "shape": "lump",
     "effects": [{"id": "pain_relief", "k": 0.4}, {"id": "antiseptic", "k": 0.3}, {"id": "aspect.life", "k": 0.4}], "worth": 5.0, "item": "silphium"},
    {"id": "mandrake", "tags": ["herb", "reagent", "poisonous"], "p": {"toxic": 0.4, "moisture": 0.5, "purity": 0.8}, "shape": "lump",
     "effects": [{"id": "sedative", "k": 0.6}, {"id": "aspect.sleep", "k": 0.4}, {"id": "aspect.mind", "k": 0.3}], "worth": 3.0, "item": "mandrake"},
    {"id": "mistletoe", "tags": ["herb", "reagent", "sacred"], "p": {"moisture": 0.5, "purity": 0.85}, "shape": "lump",
     "effects": [{"id": "aspect.life", "k": 0.3}, {"id": "aspect.plants", "k": 0.35}, {"id": "aspect.bond", "k": 0.2}], "worth": 2.5, "item": "mistletoe"},
    {"id": "henbane", "tags": ["herb", "reagent", "poisonous"], "p": {"toxic": 0.6, "moisture": 0.5, "purity": 0.8}, "shape": "lump",
     "effects": [{"id": "poison", "k": 0.5}, {"id": "aspect.mind", "k": 0.4}, {"id": "aspect.poison", "k": 0.2}], "worth": 1.2, "item": "henbane"},
    {"id": "wormwood", "tags": ["herb", "reagent", "aromatic"], "p": {"moisture": 0.4, "purity": 0.8, "toxic": 0.1}, "shape": "lump",
     "effects": [{"id": "bitter", "k": 0.7}, {"id": "antiseptic", "k": 0.3}, {"id": "aspect.shadow", "k": 0.15}, {"id": "aspect.mind", "k": 0.15}],
     "worth": 0.8, "item": "wormwood"},
]
MATERIAL_NAMES = {  # id: (ru nom, g, made, en, en made)
    "natron": ("натрон", "m", "{form} с натроном", "natron", "natron {form}"),
    "frankincense": ("ладан", "m", "{form} с ладаном", "frankincense", "frankincense {form}"),
    "myrrh": ("смирна", "f", "{form} со смирной", "myrrh", "myrrh {form}"),
    "silphium": ("сильфий", "m", "{form} из сильфия", "silphium", "silphium {form}"),
    "mandrake": ("мандрагора", "f", "{form} из мандрагоры", "mandrake", "mandrake {form}"),
    "mistletoe": ("омела", "f", "{form} из омелы", "mistletoe", "mistletoe {form}"),
    "henbane": ("белена", "f", "{form} из белены", "henbane", "henbane {form}"),
    "wormwood": ("полынь", "f", "полынн{form:g|ый|ая|ое|ые} {form}", "wormwood", "wormwood {form}"),
}

# ---------------------------------------------------------------- crafts (plain GOAP acts, as sew_tunic)
ACTS = []


def craft(iid, uses, belt, knows, dur, ru_name, en_name, tool=None, target=None, n=1, skill=None, extra_pre=()):
    pre = []
    for k in knows:
        pre.append(f"knows:{k}")
    if belt:
        pre.append(f"knows:craft_{belt}")
    if tool:
        pre.append(f"has:{tool}")
    pre += [f"has:{i}>={c}" for i, c in uses]
    pre += list(extra_pre)
    sk = skill or (knows[0] if knows else "toolmaking")
    eff = [f"has:{i}-{c}" for i, c in uses] + [f"has:{iid}+{n}", f"skill:{sk}+0.02", "moodlet:made_something"]
    a = {"id": f"make_{iid}" if not iid.startswith("make_") else iid, "category": "work",
         "target": "object:" + target if target else "self", "pre": pre, "eff": eff,
         "duration": dur, "cost": dur, "fsm": "Work", "legal": [], "visibility": "public", "_name": (ru_name, en_name)}
    if target:
        a["from_target"] = True
        a["reserve"] = True
    ACTS.append(a)


SEW = ["sewing"]
# clothes (needle + cloth + thread; wool cloth is cloth too)
craft("chiton", [("cloth", 2), ("thread", 1)], "mediterranean", SEW, 10, "Сшить хитон", "Sew a chiton", tool="needle")
craft("peplos", [("cloth", 2), ("thread", 1)], "mediterranean", SEW, 11, "Сшить пеплос", "Sew a peplos", tool="needle")
craft("himation", [("cloth", 3), ("thread", 1)], "mediterranean", SEW, 13, "Сшить гиматий", "Sew a himation", tool="needle")
craft("chlamys", [("cloth", 2), ("thread", 1)], "mediterranean", SEW, 10, "Сшить хламиду", "Sew a chlamys", tool="needle")
craft("sandals", [("leather", 1), ("cord", 1)], None, ["leatherwork"], 6, "Сделать сандалии", "Make sandals", tool="stone_blade")
craft("petasos", [("felt", 1), ("thread", 1)], "mediterranean", SEW, 5, "Свалять петас", "Make a petasos", tool="needle")
craft("shendyt", [("cloth", 1), ("thread", 1)], "nile", SEW, 6, "Сшить схенти", "Sew a shendyt", tool="needle")
craft("kalasiris", [("cloth", 2), ("thread", 1)], "nile", SEW, 10, "Сшить калазирис", "Sew a kalasiris", tool="needle")
craft("sarafan", [("cloth", 3), ("thread", 2)], "forest", SEW, 14, "Сшить сарафан", "Sew a sarafan", tool="needle")
craft("bast_shoes", [("linden_bast", 3)], "forest", ["fiber_working"], 6, "Сплести лапти", "Weave bast shoes", skill="fiber_working")
craft("plaid_cloak", [("cloth", 3), ("thread", 2)], "forest", SEW, 15, "Сшить клетчатый плащ", "Sew a plaid cloak", tool="needle")
craft("flower_wreath", [("wild_greens", 2)], None, ["foraging"], 2, "Сплести венок", "Weave a wreath", skill="foraging")
craft("fur_cloak", [("hide", 3), ("thread", 2)], "north", SEW, 16, "Сшить меховой плащ", "Sew a fur cloak", tool="needle")
craft("fur_hat", [("hide", 1), ("thread", 1)], None, SEW, 6, "Сшить меховую шапку", "Sew a fur hat", tool="needle")
craft("apron_dress", [("cloth", 2), ("thread", 1)], "north", SEW, 10, "Сшить сарафан-фартук", "Sew an apron dress", tool="needle")
craft("kaftan", [("cloth", 3), ("felt", 1), ("thread", 2)], "steppe", SEW, 15, "Сшить кафтан", "Sew a kaftan", tool="needle")
craft("felt_hood", [("felt", 1), ("thread", 1)], "steppe", SEW, 5, "Сшить башлык", "Sew a felt hood", tool="needle")
craft("riding_trousers", [("cloth", 2), ("thread", 1)], "steppe", SEW, 9, "Сшить шаровары", "Sew riding trousers", tool="needle")
craft("soft_boots", [("leather", 2), ("thread", 1)], "steppe", ["leatherwork"], 9, "Сшить ичиги", "Sew soft boots", tool="needle")
craft("felt", [("wool", 3), ("water", 1)], "steppe", ["fiber_working"], 8, "Свалять войлок", "Full felt", skill="fiber_working")
# armour and shields
craft("linothorax", [("cloth", 6), ("resin", 2)], "mediterranean", SEW, 24, "Склеить льняной панцирь", "Glue a linothorax",
      tool="needle", skill="armorer")
craft("bronze_cuirass", [("bronze_ingot", 4), ("charcoal", 3), ("leather", 1)], "mediterranean", ["smithing"], 40,
      "Выковать бронзовый панцирь", "Hammer a bronze cuirass", tool="hammer", target="forge", skill="armorer")
craft("bronze_greaves", [("bronze_ingot", 2), ("charcoal", 2)], "mediterranean", ["smithing"], 24,
      "Выковать поножи", "Hammer greaves", tool="hammer", target="forge", skill="armorer")
craft("crested_helm", [("bronze_ingot", 2), ("charcoal", 2), ("feathers", 2)], "mediterranean", ["smithing"], 28,
      "Выковать гребенчатый шлем", "Hammer a crested helm", tool="hammer", target="forge", skill="armorer")
craft("scale_armour", [("bronze_ingot", 3), ("leather", 2), ("cord", 2)], "steppe", ["smithing"], 36,
      "Нашить чешуйчатый доспех", "Make scale armour", tool="hammer", target="forge", skill="armorer")
craft("nasal_helm", [("iron_bar", 2), ("charcoal", 2)], "north", ["smithing"], 26,
      "Выковать шлем с наносником", "Forge a nasal helm", tool="hammer", target="forge", skill="armorer")
craft("pointed_helm", [("bronze_ingot", 2), ("charcoal", 2)], "steppe", ["smithing"], 24,
      "Выковать остроконечный шлем", "Hammer a pointed helm", tool="hammer", target="forge", skill="armorer")
craft("hoplon", [("plank", 2), ("bronze_ingot", 1), ("leather", 1)], "mediterranean", ["carpentry"], 22,
      "Сделать гоплон", "Make a hoplon", tool="hammer", skill="carpentry")
craft("round_shield", [("plank", 2), ("leather", 1)], None, ["carpentry"], 12, "Сделать круглый щит", "Make a round shield",
      tool="axe", skill="carpentry")
craft("oval_shield", [("plank", 2), ("leather", 1)], "forest", ["carpentry"], 13, "Сделать овальный щит", "Make an oval shield",
      tool="axe", skill="carpentry")
craft("wicker_shield", [("reeds", 4), ("hide", 1)], None, ["fiber_working"], 8, "Сплести щит", "Weave a wicker shield",
      skill="basketry")
# weapons
craft("dory", [("ash_stave", 1), ("bronze_ingot", 1), ("charcoal", 1)], "mediterranean", ["smithing"], 12,
      "Сделать дори", "Make a dory", tool="hammer", target="forge", skill="smithing")
craft("xiphos", [("bronze_ingot", 2), ("charcoal", 2)], "mediterranean", ["smithing"], 18, "Отлить ксифос", "Cast a xiphos",
      tool="hammer", target="forge")
craft("kopis", [("iron_bar", 2), ("charcoal", 2)], "mediterranean", ["smithing"], 20, "Выковать копис", "Forge a kopis",
      tool="hammer", target="forge")
craft("sling", [("cord", 2), ("leather", 1)], None, ["fiber_working"], 3, "Сплести пращу", "Make a sling", skill="fiber_working")
craft("khopesh", [("bronze_ingot", 2), ("charcoal", 2)], "nile", ["smithing"], 20, "Отлить хопеш", "Cast a khopesh",
      tool="hammer", target="forge")
craft("seax", [("iron_bar", 1), ("charcoal", 1)], "north", ["smithing"], 10, "Выковать скрамасакс", "Forge a seax",
      tool="hammer", target="forge")
craft("bearded_axe", [("iron_bar", 2), ("charcoal", 2), ("ash_stave", 1)], "north", ["smithing"], 16,
      "Выковать секиру", "Forge a bearded axe", tool="hammer", target="forge")
craft("akinakes", [("iron_bar", 1), ("charcoal", 1)], "steppe", ["smithing"], 10, "Выковать акинак", "Forge an akinakes",
      tool="hammer", target="forge")
craft("sagaris", [("bronze_ingot", 1), ("charcoal", 1), ("ash_stave", 1)], "steppe", ["smithing"], 14,
      "Сделать сагарис", "Make a sagaris", tool="hammer", target="forge")
craft("composite_bow", [("horn", 2), ("sinew", 2), ("ash_stave", 1), ("resin", 1)], "steppe", ["fletching"], 30,
      "Склеить сложный лук", "Glue a composite bow", tool="stone_blade", skill="fletching")
# metals at the forge
craft("bronze_ingot", [("copper_ore", 3), ("tin_ore", 1), ("charcoal", 2)], None, ["smithing"], 12,
      "Выплавить бронзу", "Smelt bronze", target="forge", n=2)
craft("iron_bar", [("bog_ore", 3), ("charcoal", 3)], None, ["smithing"], 14, "Выплавить крицу", "Smelt an iron bloom", target="forge")
# food, drink, goods
craft("olive_oil", [("olives", 6)], "mediterranean", ["cooking"], 6, "Выжать оливковое масло", "Press olive oil", n=2, skill="cooking")
craft("flatbread", [("grain", 2), ("water", 1)], None, ["cooking"], 3, "Испечь лепёшки", "Bake flatbread", target="hearth", n=3,
      skill="cooking", extra_pre=["target_owner:family"])
craft("barley_beer", [("grain", 3), ("water", 2)], "nile", ["brewing"], 10, "Сварить ячменное пиво", "Brew barley beer",
      target="hearth", n=3, skill="brewing", extra_pre=["target_owner:family"])
craft("kumis", [("milk", 4)], "steppe", ["husbandry"], 6, "Заквасить кумыс", "Ferment kumis", n=3, skill="husbandry")
craft("kvass", [("bread", 1), ("water", 3)], "forest", ["brewing"], 6, "Поставить квас", "Brew kvass", n=3, skill="brewing")
craft("sacrificial_cake", [("grain", 1), ("honey", 1)], None, ["faith"], 3, "Испечь жертвенную лепёшку", "Bake an offering cake",
      target="hearth", n=2, skill="cooking", extra_pre=["target_owner:family"])
craft("amphora", [("clay", 4)], "mediterranean", ["pottery"], 10, "Вылепить амфору", "Throw an amphora", skill="pottery")
craft("kylix", [("clay", 1)], "mediterranean", ["pottery"], 5, "Вылепить килик", "Throw a kylix", skill="pottery")
craft("drinking_horn", [("horn", 1)], None, ["toolmaking"], 4, "Сделать питьевой рог", "Make a drinking horn", tool="stone_blade")
craft("kyphi", [("myrrh", 1), ("honey", 1), ("wine", 1)], "nile", ["faith"], 6, "Смешать кифи", "Blend kyphi", skill="herbalism")
craft("votive_figurine", [("clay", 1)], None, ["pottery"], 3, "Вылепить обетную фигурку", "Mould a votive figurine", skill="pottery")
craft("scarab_amulet", [("stone", 1)], "nile", ["toolmaking"], 5, "Вырезать скарабея", "Carve a scarab", tool="stone_blade")
craft("rune_charm", [("bone", 1)], "north", ["toolmaking"], 3, "Вырезать рунный оберег", "Carve a rune charm", tool="stone_blade")
craft("obereg", [("branch", 1), ("cord", 1)], "forest", ["fiber_working"], 2, "Сделать оберег", "Make a protective charm",
      skill="fiber_working")
craft("silver_torc", [("silver", 2), ("charcoal", 1)], "forest", ["smithing"], 12, "Выковать гривну", "Forge a silver torc",
      tool="hammer", target="forge")
craft("arm_ring", [("silver", 1), ("charcoal", 1)], "north", ["smithing"], 8, "Выковать запястье", "Forge an oath ring",
      tool="hammer", target="forge")
craft("gold_plaque", [("gold", 1)], "steppe", ["smithing"], 8, "Вычеканить золотую бляшку", "Chase a gold plaque", tool="hammer")
craft("brooch", [("bronze_ingot", 1)], None, ["smithing"], 5, "Отлить фибулу", "Cast a brooch", tool="hammer", target="forge")
craft("broad_collar", [("cloth", 1), ("amber", 2), ("thread", 1)], "nile", SEW, 8, "Собрать ожерелье-воротник", "String a broad collar",
      tool="needle")
craft("lyre", [("horn", 2), ("sinew", 2), ("plank", 1)], "mediterranean", ["carpentry"], 16, "Сделать лиру", "Make a lyre",
      skill="music")
craft("gusli", [("plank", 2), ("sinew", 3)], "forest", ["carpentry"], 16, "Сделать гусли", "Make gusli", skill="music")
craft("kobyz", [("plank", 1), ("hide", 1), ("sinew", 2)], "steppe", ["carpentry"], 14, "Сделать кобыз", "Make a kobyz", skill="music")

# sources of the belts (placed only where the map's culture lives: objects.belts)
OBJECTS = [
    {"id": "olive_grove", "stand_m": 0.9, "stock": {"olives": 12, "log": 2}, "regrow": {"olives": 4}, "regrow_every": 288,
     "max_stock": {"olives": 12, "log": 2}, "biomes": ["meadow", "forest"], "per_km2": 8, "owned": False, "belts": MED,
     "placed_last": True, "_doc": "LR-168: оливы юга: оливки (масло жмут дома), брёвна"},
    {"id": "grapevine", "stand_m": 0.7, "stock": {"grapes": 10}, "regrow": {"grapes": 4}, "regrow_every": 288,
     "max_stock": {"grapes": 10}, "biomes": ["meadow"], "per_km2": 6, "owned": False, "belts": MED, "placed_last": True,
     "_doc": "LR-168: дикая лоза юга: виноград"},
    {"id": "fig_tree", "stand_m": 0.8, "stock": {"figs": 8}, "regrow": {"figs": 3}, "regrow_every": 288,
     "max_stock": {"figs": 8}, "biomes": ["meadow", "river"], "per_km2": 4, "owned": False, "belts": MED, "placed_last": True,
     "_doc": "LR-168: смоковница: смоквы"},
    {"id": "date_palm", "stand_m": 0.8, "stock": {"dates": 10, "reeds": 2}, "regrow": {"dates": 4}, "regrow_every": 288,
     "max_stock": {"dates": 10, "reeds": 2}, "biomes": ["river", "meadow"], "per_km2": 8, "owned": False, "belts": NIL,
     "placed_last": True, "_doc": "LR-168: финиковая пальма Нила"},
    {"id": "natron_flat", "stand_m": 0.8, "stock": {"natron": 8}, "regrow": {"natron": 2}, "regrow_every": 432,
     "max_stock": {"natron": 8}, "biomes": ["marsh", "river"], "per_km2": 0.5, "owned": False, "belts": NIL, "placed_last": True,
     "_doc": "LR-168: натроновое озерцо: соль бальзамировщиков и стекольщиков"},
    {"id": "wormwood_steppe", "stand_m": 0.6, "stock": {"wormwood": 6}, "regrow": {"wormwood": 2}, "regrow_every": 144,
     "max_stock": {"wormwood": 6}, "biomes": ["meadow"], "per_km2": 6, "owned": False, "belts": STP, "placed_last": True,
     "_doc": "LR-168: полынная степь"},
    {"id": "silphium_patch", "stand_m": 0.6, "stock": {"silphium": 3}, "regrow": {"silphium": 1}, "regrow_every": 432,
     "max_stock": {"silphium": 3}, "biomes": ["meadow"], "per_km2": 0.6, "owned": False, "belts": MED, "placed_last": True,
     "_doc": "LR-168: сильфий, редкий: лекарство и пряность юга"},
]
OBJECT_NAMES = {
    "olive_grove": (ru("оливковая роща", "оливковой рощи", "оливковой роще", "оливковую рощу", "оливковой рощей", "оливковой роще", "f"), "olive grove"),
    "grapevine": (ru("виноградная лоза", "виноградной лозы", "виноградной лозе", "виноградную лозу", "виноградной лозой", "виноградной лозе", "f"), "grapevine"),
    "fig_tree": (ru("смоковница", "смоковницы", "смоковнице", "смоковницу", "смоковницей", "смоковнице", "f"), "fig tree"),
    "date_palm": (ru("финиковая пальма", "финиковой пальмы", "финиковой пальме", "финиковую пальму", "финиковой пальмой", "финиковой пальме", "f"), "date palm"),
    "natron_flat": (ru("натроновое озерцо", "натронового озерца", "натроновому озерцу", "натроновое озерцо", "натроновым озерцом", "натроновом озерце", "n"), "natron flat"),
    "wormwood_steppe": (ru("полынник", "полынника", "полыннику", "полынник", "полынником", "полыннике", "m"), "wormwood"),
    "silphium_patch": (ru("заросль сильфия", "заросли сильфия", "заросли сильфия", "заросль сильфия", "зарослью сильфия", "заросли сильфия", "f"), "silphium patch"),
}
OBJECT_SPRITES = {
    "olive_grove": ("ob_tree", {"G": "W", "D": "x"}, "♣", "W"), "grapevine": ("ob_bush", {"G": "v", "l": "V"}, "\"", "v"),
    "fig_tree": ("ob_tree", {}, "♣", "G"), "date_palm": ("ob_pine", {"G": "o", "D": "n"}, "♠", "o"),
    "natron_flat": ("tl_sand", {}, "≈", "w"), "wormwood_steppe": ("ob_bush", {"G": "W", "l": "g"}, "\"", "W"),
    "silphium_patch": ("ob_bush", {"G": "y"}, "\"", "y"),
}


def take(id_, target, item_id, n, dur, skill, ru_name, en_name, tool=None, knows=None):
    pre = ([f"knows:{knows}"] if knows else []) + ([f"has:{tool}"] if tool else []) + [f"target_stock:{item_id}>={n}"]
    eff = [f"has:{item_id}+{n}", f"target_stock:{item_id}-{n}", f"skill:{skill}+0.01", "body:fatigue+0.03"]
    ACTS.append({"id": id_, "category": "work", "target": "object:" + target, "pre": pre, "eff": eff, "from_target": True,
                 "duration": dur, "cost": dur, "fsm": "Work", "legal": [], "visibility": "public", "reserve": True,
                 "_name": (ru_name, en_name)})


take("pick_olives", "olive_grove", "olives", 3, 3, "foraging", "Собрать оливки", "Pick olives")
take("pick_grapes", "grapevine", "grapes", 3, 2, "foraging", "Нарвать винограда", "Pick grapes")
take("pick_figs", "fig_tree", "figs", 2, 2, "foraging", "Нарвать смокв", "Pick figs")
take("pick_dates", "date_palm", "dates", 3, 3, "foraging", "Собрать финики", "Pick dates")
take("dig_natron_ore", "natron_flat", "natron", 2, 3, "foraging", "Наскрести натрона", "Scrape natron")
take("gather_wormwood_herb", "wormwood_steppe", "wormwood", 2, 2, "herbalism", "Нарезать полыни", "Cut wormwood")
take("gather_silphium", "silphium_patch", "silphium", 1, 3, "herbalism", "Собрать сильфий", "Gather silphium", knows="herbalism")
# wine from grapes at home (the old wine item of the expansion)
craft("make_wine", [("grapes", 6)], "mediterranean", ["brewing"], 8, "Надавить вина", "Tread wine", skill="brewing")
ACTS[-1]["eff"] = ["has:grapes-6", "has:wine+2", "skill:brewing+0.02", "moodlet:made_something"]
# eat and drink what is new
for iid, hunger, rn, en in [("olives", 0.1, "Поесть оливок", "Eat olives"), ("figs", 0.15, "Поесть смокв", "Eat figs"),
                            ("dates", 0.2, "Поесть фиников", "Eat dates"), ("grapes", 0.12, "Поесть винограда", "Eat grapes"),
                            ("flatbread", 0.35, "Съесть лепёшку", "Eat flatbread")]:
    ACTS.append({"id": f"eat_{iid}", "category": "household", "target": "self", "pre": [f"has:{iid}>=1"],
                 "eff": [f"has:{iid}-1", f"need:hunger+{hunger}", "moodlet:ate_meal"], "duration": 1, "cost": 1,
                 "_name": (rn, en)})

# ---------------------------------------------------------------- knowledge: the crafts of each belt
KNOW = {
    "mediterranean": ("ремёсла Срединного моря", "crafts of the Middle Sea"),
    "nile": ("ремёсла Нила", "crafts of the Nile"),
    "forest": ("ремёсла Средних лесов", "crafts of the Middle Woods"),
    "north": ("ремёсла Севера", "crafts of the North"),
    "steppe": ("ремёсла Степи", "crafts of the Steppe"),
}

# ---------------------------------------------------------------- regions: what each belt wears, makes, offers
CULTURE_OWN = {
        # cultures of LR-166 whose ways differ from their belt's
        "grove_nymph": {"clothes": {"f": {"flower_wreath": 1}, "child": {"chiton": 1}}},
        "vila_glade": {"clothes": {"f": {"flower_wreath": 1}, "child": {"tunic": 1}}},
        "satyr_vine": {"clothes": {"m": {"flower_wreath": 1}, "f": {"flower_wreath": 1}, "child": {"chiton": 1}}},
        "labyrinth": {"clothes": {"m": {"shendyt": 1, "belt": 1}, "f": {"chiton": 1, "belt": 1}, "child": {"chiton": 1}}},
        "smith_cyclops": {"clothes": {"m": {"chiton": 1, "belt": 1}, "f": {"peplos": 1, "belt": 1}, "child": {"chiton": 1}}},
        "isle_sailor": {"clothes": {"m": {"chiton": 1, "belt": 1}, "f": {"peplos": 1, "sandals": 1, "belt": 1},
                                    "child": {"chiton": 1}}},
        "steppe_rider": {"knowledge": ["riding"]},
        "celtic_grove": {"clothes": {"m": {"tunic": 1, "trousers": 1, "plaid_cloak": 1, "belt": 1},
                                     "f": {"tunic": 1, "plaid_cloak": 1, "belt": 1}, "child": {"tunic": 1}}},
}

REGIONS = {
    "_doc": "LR-168: things of the belts (setting v0.3). belts.<belt>: clothes (m/f/child: what a person of the belt starts "
            "dressed in, instead of gear.json base_clothes), knowledge (the belt's crafts), metal (the era's main metal), "
            "native (the belt's own things: markets, caravans, the 2D lists), offerings (what its people give the gods, "
            "kinds by tag offering_<kind>). cultures.<culture>: a culture's own clothes/knowledge over its belt's; belt = "
            "for culture files without the field (old ids). races.<race>.skip_slots: body slots it does not dress. "
            "Read by src/sim/regional.cpp. Generated by tools/content_gen/gen_regional.py.",
    "belts": {
        "mediterranean": {
            "metal": "bronze", "knowledge": ["craft_mediterranean"],
            "clothes": {"m": {"chiton": 1, "sandals": 1, "belt": 1}, "f": {"peplos": 1, "sandals": 1, "belt": 1},
                        "child": {"chiton": 1}},
            "offerings": ["wine", "olive_oil", "honey", "sacrificial_cake", "frankincense", "votive_figurine", "honey_cake"],
        },
        "nile": {
            "metal": "bronze", "knowledge": ["craft_nile"],
            "clothes": {"m": {"shendyt": 1, "sandals": 1}, "f": {"kalasiris": 1, "sandals": 1}, "child": {"shendyt": 1}},
            "offerings": ["kyphi", "frankincense", "myrrh", "barley_beer", "bread", "dates", "scarab_amulet", "votive_figurine"],
        },
        "forest": {
            "metal": "iron", "knowledge": ["craft_forest"],
            "clothes": {"m": {"tunic": 1, "trousers": 1, "bast_shoes": 1, "belt": 1},
                        "f": {"tunic": 1, "sarafan": 1, "bast_shoes": 1, "belt": 1}, "child": {"tunic": 1}},
            "offerings": ["mead", "bread", "honey", "milk", "obereg", "silver_torc", "mistletoe", "flower_wreath"],
        },
        "north": {
            "metal": "iron", "knowledge": ["craft_north"],
            "clothes": {"m": {"tunic": 1, "trousers": 1, "boots": 1, "belt": 1},
                        "f": {"tunic": 1, "apron_dress": 1, "boots": 1, "belt": 1}, "child": {"tunic": 1}},
            "offerings": ["mead", "ale", "arm_ring", "silver", "amber", "rune_charm"],
        },
        "steppe": {
            "metal": "bronze_iron", "knowledge": ["craft_steppe"],
            "clothes": {"m": {"tunic": 1, "riding_trousers": 1, "soft_boots": 1, "belt": 1},
                        "f": {"tunic": 1, "riding_trousers": 1, "soft_boots": 1, "belt": 1}, "child": {"tunic": 1}},
            "offerings": ["kumis", "milk", "dried_meat", "gold_plaque", "wormwood"],
        },
    },
    "cultures": {
        # LR-166 culture ids: their files carry `belt` (that wins); these keep the pack working before and without them
        **{c: {"belt": b} for b, cs in {
            "mediterranean": ["polis_hellene", "isle_sailor", "pastoral_hills", "labyrinth", "grove_nymph", "satyr_vine",
                              "smith_cyclops", "port_kobal"],
            "nile": ["nile_priest"],
            "forest": ["forest_elf", "vila_glade", "celtic_grove", "slav_village"],
            "north": ["north_folk", "dwarf_hold", "alv_hall", "jotun_hold", "troll_fell"],
            "steppe": ["steppe_clan", "steppe_rider"]}.items() for c in cs},
        # old culture ids until their files carry `belt` (LR-166 migration: content/species/migrate.json)
        "fantasy_european": {"belt": "forest"}, "sylvan": {"belt": "forest"}, "dwarven": {"belt": "north"},
        "orcish": {"belt": "steppe"}, "goblin": {"belt": "mediterranean"}, "ogre": {"belt": "north"},
        "troll": {"belt": "north"}, "draconic": {"belt": "north"},
    },
    "races": {
        "centaur": {"skip_slots": ["legs", "feet"]},
        "steppe_centaur": {"skip_slots": ["legs", "feet"]},
        "satyr": {"skip_slots": ["feet"]},
        "minotaur": {"skip_slots": ["feet"]},
    },
}

# ---------------------------------------------------------------- markets and caravans
# good: price, bulk, make, use (caravans.json expr; belt.<id> = 1 where the settlement's culture is of that belt)
GOODS = {
    "olive_oil": {"price": 4.0, "bulk": 1.0, "make": "belt.mediterranean * land * 0.25", "use": "0.04 + belt.mediterranean * 0.12 + tag(cult) * 0.05"},
    "wine": {"price": 6.0, "bulk": 1.0, "make": "belt.mediterranean * land * 0.2", "use": "0.04 + wealth_head * 0.01 + belt.mediterranean * 0.06"},
    "figs": {"price": 1.5, "bulk": 0.6, "make": "belt.mediterranean * land * 0.2", "use": "0.03"},
    "dates": {"price": 1.5, "bulk": 0.6, "make": "belt.nile * land * 0.3", "use": "0.03 + belt.nile * 0.1"},
    "papyrus": {"price": 3.0, "bulk": 0.3, "make": "belt.nile * crafts * 0.6", "use": "0.003 + tag(cult) * 0.05 + wealth_head * 0.004"},
    "natron": {"price": 2.0, "bulk": 1.0, "deposit": 0.3, "make": "belt.nile * deposit * 1.0", "use": "0.003 + belt.nile * 0.03"},
    "frankincense": {"price": 12.0, "bulk": 0.2, "make": "belt.nile * 0.03", "use": "0.002 + tag(cult) * 0.04 + belt.mediterranean * 0.01"},
    "myrrh": {"price": 14.0, "bulk": 0.2, "make": "belt.nile * 0.02", "use": "0.001 + tag(cult) * 0.03"},
    "purple_dye": {"price": 25.0, "bulk": 0.1, "make": "belt.mediterranean * crafts * 0.04", "use": "0.0005 + wealth_head * 0.003"},
    "silphium": {"price": 15.0, "bulk": 0.1, "make": "belt.mediterranean * 0.01", "use": "0.001 + wealth_head * 0.002"},
    "amphora": {"price": 3.0, "bulk": 1.5, "make": "belt.mediterranean * crafts * 0.4", "use": "0.02"},
    "bronze_ingot": {"price": 6.0, "bulk": 0.8, "deposit": 0.12, "make": "deposit * crafts * 0.6 + people.dwarf * deposit * 0.4", "use": "crafts * 0.06 + fort * 0.04"},
    "iron_bar": {"price": 4.0, "bulk": 0.8, "deposit": 0.15, "make": "deposit * crafts * 0.5 * (belt.north + belt.forest + belt.steppe)", "use": "crafts * 0.05 + fort * 0.04"},
    "amber": {"price": 10.0, "bulk": 0.1, "deposit": 0.2, "make": "deposit * (belt.north + belt.forest) * 0.2", "use": "0.002 + wealth_head * 0.004"},
    "sable_fur": {"price": 14.0, "bulk": 0.3, "make": "(belt.north + belt.forest) * land * 0.03", "use": "0.002 + wealth_head * 0.005"},
    "mistletoe": {"price": 6.0, "bulk": 0.1, "make": "belt.forest * land * 0.02 + culture.celtic_grove * 0.05", "use": "0.001 + tag(cult) * 0.03"},
    "kumis": {"price": 3.0, "bulk": 1.0, "make": "belt.steppe * land * 0.3", "use": "0.02 + belt.steppe * 0.15"},
    "felt": {"price": 3.0, "bulk": 0.8, "make": "belt.steppe * land * 0.2 + belt.steppe * crafts * 0.3", "use": "0.03 + belt.steppe * 0.05"},
    "composite_bow": {"price": 16.0, "bulk": 0.5, "make": "belt.steppe * crafts * 0.1", "use": "fort * 0.03 + tag(raiders) * 0.08"},
    "gold_plaque": {"price": 24.0, "bulk": 0.05, "make": "belt.steppe * crafts * 0.01", "use": "0.0005 + wealth_head * 0.002"},
    "kyphi": {"price": 8.0, "bulk": 0.1, "make": "belt.nile * crafts * 0.05", "use": "0.001 + tag(cult) * 0.04"},
}

TRADER_STOCK = ["frankincense", "myrrh", "mandrake", "mistletoe", "henbane"]

# ---------------------------------------------------------------- combat links
ARMOR_NEW = {
    "linen": {"cover": ["torso"], "pen": {"slash": 0.45, "thrust": 0.3}, "trans": {"slash": 0.88, "thrust": 0.9, "blunt": 0.85}, "stamina_pen": 0.4},
    "bronze_plate": {"cover": ["torso"], "pen": {"slash": 0.85, "thrust": 0.75}, "trans": {"slash": 0.82, "thrust": 0.82, "blunt": 0.8}, "stamina_pen": 1.0, "gap": 0.15},
    "greaves": {"cover": ["leg_l", "leg_r"], "pen": {"slash": 0.8, "thrust": 0.7}, "trans": {"slash": 0.85, "thrust": 0.85, "blunt": 0.8}, "stamina_pen": 0.4, "gap": 0.2},
    "scale": {"cover": ["torso", "arm_l", "arm_r"], "pen": {"slash": 0.65, "thrust": 0.45}, "trans": {"slash": 0.9, "thrust": 0.9, "blunt": 0.85}, "stamina_pen": 0.8, "gap": 0.1},
}
SHIELD_NEW = {
    "hoplon": {"guard_mult": 0.55, "stamina": 4.0, "weight": 7.0},
    "wicker": {"guard_mult": 0.75, "stamina": 2.0, "weight": 1.5},
}
ARMOR_NAMES = {"linen": ("льняной панцирь", "linen corselet"), "bronze_plate": ("бронзовый панцирь", "bronze cuirass"),
               "greaves": ("поножи", "greaves"), "scale": ("чешуя", "scale")}
SHIELD_NAMES = {"hoplon": ("гоплон", "hoplon"), "wicker": ("плетёный щит", "wicker shield")}


def add_keys_text(path, new, indent="  "):
    """Append top-level keys to a pretty JSON object file, one compact line each, keeping its own lines."""
    t = open(path, encoding="utf-8").read()
    d = json.loads(t)
    add = {k: v for k, v in new.items() if k not in d}
    if not add:
        return
    end = t.rstrip().rfind("}")
    head = t[:end].rstrip()
    lines = "".join(f",\n{indent}\"{k}\": {json.dumps(v, ensure_ascii=False)}" for k, v in add.items())
    t = head + lines + "\n" + t[end:]
    json.loads(t)
    open(path, "w", encoding="utf-8").write(t)


def set_goods(path):
    """caravans.json goods: one good per line; add ours (replace their lines when they change)."""
    t = open(path, encoding="utf-8").read()
    for gid, g in GOODS.items():
        line = f'  "{gid}":' + " " * max(1, 13 - len(gid)) + json.dumps(g, ensure_ascii=False) + ","
        m = re.search(r'\n  "%s": [^\n]*' % re.escape(gid), t)
        if m:
            old = m.group(0)
            keep_comma = old.rstrip().endswith(",")
            t = t.replace(old, "\n" + (line if keep_comma else line[:-1]), 1)
            continue
        m = re.search(r"\n\s*\"goods\": \{\n", t)
        assert m
        t = t[:m.end()] + line + "\n" + t[m.end():]
    json.loads(t)
    open(path, "w", encoding="utf-8").write(t)


def main():
    pack = P("content/ai/regional")
    # items
    items = []
    for x in I:
        e = {"id": x["id"], "tags": x["tags"], "stack": 1 if x["wear"] or "weapon" in x["tags"] or "armour" in x["tags"] else 20,
             "value": x["value"], "belts": x["belts"]}
        for k in ("material", "nutrition", "spoil_days"):
            if k in x["extra"]:
                e[k] = x["extra"][k]
        if x["wear"] and x["wear"][2]:
            e["warmth"] = x["wear"][2]
        items.append(e)
    annotate = [{"id": iid, "add_tags": tags} for iid, tags in ANNOTATE if iid not in ITEM_IDS]
    # offering tags of the pack's own items live on the items themselves
    own = {iid: tags for iid, tags in ANNOTATE if iid in ITEM_IDS}
    for e in items:
        for t in own.get(e["id"], []):
            if t not in e["tags"]:
                e["tags"].append(t)
    save(P(pack, "items.json"), {"_doc": "LR-168: things of the belts (setting v0.3): clothes, arms, armour, shields, metals, food, "
                                         "goods, reagents, instruments, offerings. `belts` = whose own thing it is (regions.json "
                                         "native). Offering kinds: tag offering_<libation|food|incense|votive|treasure|light>. "
                                         "Generated by tools/content_gen/gen_regional.py.",
                                 "items": items, "annotate": annotate})
    acts = []
    for a in ACTS:
        a = dict(a)
        a.pop("_name")
        acts.append(a)
    save(P(pack, "interactions.json"), {"_doc": "LR-168: crafts of the belts (knows:craft_<belt>) and their sources. "
                                                "Generated by tools/content_gen/gen_regional.py.", "interactions": acts})
    save(P(pack, "objects.json"), {"_doc": "LR-168: sources of some belts only (objects.belts: placed where the map's culture "
                                           "lives). Generated by tools/content_gen/gen_regional.py.", "objects": OBJECTS})
    save(P(pack, "knowledge.json"), {"_doc": "LR-168: the crafts of each belt: everyone of the belt starts with them "
                                             "(regions.json), others learn them from a teacher. Generated by gen_regional.py.",
                                     "knowledge": [{"id": f"craft_{b}", "requires": [], "learn_ticks": 48, "self_ticks": None}
                                                   for b in BELTS]})
    reg = json.loads(json.dumps(REGIONS))
    for cid, extra in CULTURE_OWN.items():
        reg["cultures"].setdefault(cid, {}).update(extra)
    for b in BELTS:
        reg["belts"][b]["native"] = [x["id"] for x in I if b in x["belts"]]
    save(P(pack, "regions.json"), reg)

    # gear: weight, room, where worn
    gp = P("content/ai/gear.json")
    g = load(gp)
    for x in I:
        rec = {"kg": x["kg"], "size": x["size"]}
        if x["wear"]:
            slot, layer, warmth, bulk = x["wear"]
            rec["wear"] = {"slot": slot, "layer": layer, "bulk": bulk}
            if warmth:
                rec["warmth"] = warmth
        g["items"][x["id"]] = rec
    save_gear(gp, g)

    # combat: armour, shields, the bridge
    add_keys_text(P("content/combat/armor.json"), ARMOR_NEW)
    add_keys_text(P("content/combat/shields.json"), SHIELD_NEW)
    bp = P("content/combat/sim_bridge.json")
    b = load(bp)
    weapons = [x for x in I if "weapon" in x["extra"]]
    wi = [x["id"] for x in weapons] + [w for w in b["weapon_items"] if w not in {x["id"] for x in weapons}]
    b["weapon_items"] = wi
    for x in weapons:
        b["weapon_of_item"][x["id"]] = x["extra"]["weapon"]
    for x in I:
        if "armor" in x["extra"]:
            b["armor_of_item"][x["id"]] = x["extra"]["armor"]
    b["shield_of_item"] = {x["id"]: x["extra"]["shield"] for x in I if "shield" in x["extra"]}
    t = open(bp, encoding="utf-8").read()
    for key in ("weapon_items", "weapon_of_item", "armor_of_item", "shield_of_item"):
        val = json.dumps(b[key], ensure_ascii=False).replace('{"', '{ "').replace('"}', '" }')
        m = re.search(r'\n  "%s": [^\n]*' % key, t)
        if m:
            t = t.replace(m.group(0), f'\n  "{key}": {val},' if m.group(0).rstrip().endswith(",") else f'\n  "{key}": {val}', 1)
        else:
            m2 = re.search(r'\n  "armor_of_item": [^\n]*\n', t)
            t = t[:m2.end()] + f'  "{key}": {val},\n' + t[m2.end():]
    json.loads(t)
    open(bp, "w", encoding="utf-8").write(t)

    # craft materials (the reagents' own, for alchemy and cooking)
    mp = P("content/ai/craft/materials.json")
    md = load(mp)
    have = {m["id"]: m for m in md["materials"]}
    for nm in MATERIALS:
        if nm["id"] in have:
            have[nm["id"]].clear()
            have[nm["id"]].update(nm)
        else:
            md["materials"].append(nm)
    save(mp, md)

    # the travelling trader brings the far reagents (trade.json roles: one stock line)
    tp = P("content/ai/trade.json")
    tt = open(tp, encoding="utf-8").read()
    m = re.search(r'"stock": (\{"coin": 40[^\n]*\})', tt)
    assert m, "trader stock"
    st = json.loads(m.group(1))
    for k in TRADER_STOCK:
        st.setdefault(k, 1)
    tt = tt[:m.start(1)] + json.dumps(st, ensure_ascii=False) + tt[m.end(1):]
    json.loads(tt)
    open(tp, "w", encoding="utf-8").write(tt)

    # markets
    set_goods(P("content/ai/caravans.json"))

    # loc
    lru = {"_doc": "LR-168: things of the belts (gen_regional.py)."}
    len_ = {"_doc": "LR-168: things of the belts (gen_regional.py)."}
    for x in I:
        lru[f"item.{x['id']}.name"] = x["ru"]
        len_[f"item.{x['id']}.name"] = x["en"]
    for a in ACTS:
        rn, en = a["_name"]
        lru[f"interaction.{a['id']}.name"] = rn
        len_[f"interaction.{a['id']}.name"] = en
    for oid, (rn, en) in OBJECT_NAMES.items():
        lru[f"object.{oid}.name"] = rn
        len_[f"object.{oid}.name"] = en
    for mid, (nom, g_, made, en, en_made) in MATERIAL_NAMES.items():
        lru[f"material.{mid}.name"] = {"nom": nom, "g": g_}
        lru[f"material.{mid}.made"] = made
        len_[f"material.{mid}.name"] = en
        len_[f"material.{mid}.made"] = en_made
    for b_, (rn, en) in KNOW.items():
        lru[f"knowledge.craft_{b_}.name"] = rn
        len_[f"knowledge.craft_{b_}.name"] = en
        lru[f"skill.craft_{b_}.name"] = rn
        len_[f"skill.craft_{b_}.name"] = en
    for k, (rn, en) in ARMOR_NAMES.items():
        lru[f"armor.{k}.name"] = rn
        len_[f"armor.{k}.name"] = en
    for k, (rn, en) in SHIELD_NAMES.items():
        lru[f"shield.{k}.name"] = rn
        len_[f"shield.{k}.name"] = en
    save(P("content/loc/ru/regional.json"), lru)
    save(P("content/loc/en/regional.json"), len_)

    # 2D records (into the resources records, marked as ours so gen_resources keeps them)
    sp = P("content/sprites_2d/resources.json")
    s = load(sp)
    for x in I:
        sprite, rec = x["spr"]
        glyph = "†" if "weapon" in x["tags"] else ("[" if "armour" in x["tags"] or "clothing" in x["tags"] else
                                                   ("%" if "food" in x["tags"] or "drink" in x["tags"] else "•"))
        s["items"][x["id"]] = {"sprite": sprite, "recolour": rec, "glyph": glyph, "colour": "t", "pack": "regional"}
    for oid, (sprite, rec, glyph, colour) in OBJECT_SPRITES.items():
        s["objects"][oid] = {"sprite": sprite, "recolour": rec, "glyph": glyph, "colour": colour, "pack": "regional"}
    save(sp, s)

    # the icon list for the art thread
    lines = ["# LR-168: id предметов и объектов пака regional (иконки 2D)", "",
             "Сейчас у каждого запись-заглушка в content/sprites_2d/resources.json (базовый спрайт + перекраска, \"pack\": \"regional\").", ""]
    for b_ in BELTS:
        lines.append(f"## {b_}")
        for x in I:
            if x["belts"] and x["belts"][0] == b_ and x["belts"] != ALL:
                lines.append(f"- `{x['id']}`: {x['ru']['nom']} ({x['en']})")
        lines.append("")
    lines.append("## общие (все пояса)")
    for x in I:
        if x["belts"] == ALL:
            lines.append(f"- `{x['id']}`: {x['ru']['nom']} ({x['en']})")
    lines.append("")
    lines.append("## объекты карты")
    for oid, (rn, en) in OBJECT_NAMES.items():
        lines.append(f"- `{oid}`: {rn['nom']} ({en})")
    out = "/mnt/project-files/gdd/setting/mythic_ids/REGIONAL_ITEMS_IDS.md"
    if os.path.isdir(os.path.dirname(out)):
        open(out, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    print(f"regional: {len(I)} items, {len(ACTS)} acts, {len(OBJECTS)} objects, {len(GOODS)} goods")


def save_gear(path, g):
    """gear.json keeps one item per line in its items block."""
    t = open(path, encoding="utf-8").read()
    for x in I:
        rec = json.dumps(g["items"][x["id"]], ensure_ascii=False)
        m = re.search(r'\n(\s*)"%s": \{[^\n]*' % re.escape(x["id"]), t)
        if m:
            old = m.group(0)
            comma = "," if old.rstrip().endswith(",") else ""
            t = t.replace(old, f'\n{m.group(1)}"{x["id"]}": {rec}{comma}', 1)
        else:
            m = re.search(r'\n(\s*)"items": \{\n', t)
            assert m, "gear.json items"
            ind = m.group(1) + " "
            t = t[:m.end()] + f'{ind}"{x["id"]}": {rec},\n' + t[m.end():]
    json.loads(t)
    open(path, "w", encoding="utf-8").write(t)


if __name__ == "__main__":
    main()
