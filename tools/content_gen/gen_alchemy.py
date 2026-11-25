#!/usr/bin/env python3
"""Alchemy, enchanting and artifacts (AL-117, LivelyRealms setting v0.2 §7-§8).

Source of truth for content/ai/alchemy/*.json, content/loc/{ru,en}/alchemy.json and
small idempotent links into content/ai/craft (properties conductivity/mana_capacity
and aspects of materials, new materials, processes, stations, shapes, archetypes,
starter designs, the magic block of crafting_config) and content/sprites_2d/craft2d.json
(process and station glyphs). Run from the repo root:

    python tools/content_gen/gen_alchemy.py

Aspects come from content/magic/aspects.json (AL-116); an aspect in a material is
the effect "aspect.<id>" (vector +), "aspect.<id>.minus" (-) or "aspect.<id>.shift" (~).
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


def A(aspect, k, vec="+"):
    suf = {"+": "", "-": ".minus", "~": ".shift"}[vec]
    return {"id": "aspect." + aspect + suf, "k": k}


# ------------------------------------------------- existing materials: magic in them
# id -> (conductivity, mana_capacity, [aspect effects], [extra tags])
PATCH = {
    "silver": (0.85, 0.6, [A("light", 0.2)], ["rune_metal"]),
    "gold": (0.7, 0.5, [A("light", 0.15)], ["rune_metal"]),
    "copper": (0.5, 0.25, [], []),
    "tin": (0.3, 0.15, [], []),
    "bronze": (0.4, 0.25, [], []),
    # iron conducts poorly and quenches fey magic (elves dislike it) [УТВ]
    "wrought_iron": (0.1, 0.05, [], ["cold_iron"]),
    "steel": (0.1, 0.05, [], ["cold_iron"]),
    "iron_bloom": (0.08, 0.03, [], ["cold_iron"]),
    "bone": (0.35, 0.35, [A("death", 0.1)], []),
    "antler": (0.4, 0.45, [A("beasts", 0.2)], []),
    "horn": (0.3, 0.3, [A("beasts", 0.15)], []),
    "feather": (0.25, 0.1, [A("air", 0.15)], []),
    "yew_wood": (0.25, 0.2, [A("death", 0.15)], []),
    "oak_wood": (0.15, 0.1, [A("earth", 0.05)], []),
    "ash_wood": (0.2, 0.15, [A("air", 0.05)], []),
    "linden_wood": (0.2, 0.2, [], []),
    "granite": (0.1, 0.15, [A("earth", 0.1), A("stone", 0.1)], []),
    "limestone": (0.1, 0.1, [A("earth", 0.1)], []),
    "flint": (0.15, 0.1, [A("fire", 0.05)], []),
    "salt": (None, None, [A("earth", 0.15)], []),
    "clay": (0.1, 0.1, [A("earth", 0.1)], []),
    "charcoal": (None, None, [A("fire", 0.2)], []),
    "wood_ash": (None, None, [A("fire", 0.1)], []),
    "resin": (None, None, [A("fire", 0.1), A("plants", 0.1)], []),
    "beeswax": (0.1, 0.2, [A("life", 0.05)], []),
    "honey": (None, None, [A("life", 0.1)], []),
    "garlic": (None, None, [A("life", 0.1), A("death", 0.15, "-")], []),
    "yarrow": (None, None, [A("life", 0.2)], []),
    "willow_bark": (None, None, [A("life", 0.1)], []),
    "nettle": (None, None, [A("life", 0.1), A("blood", 0.1)], []),
    "poppy": (None, None, [A("sleep", 0.4)], []),
    "hemlock": (None, None, [A("death", 0.3), A("poison", 0.3)], []),
    "hellebore": (None, None, [A("poison", 0.3), A("mind", 0.15, "-")], []),
    "mint": (None, None, [A("air", 0.1)], []),
    "linden_flower": (None, None, [A("sleep", 0.1), A("mind", 0.1)], []),
    "hops": (None, None, [A("sleep", 0.15)], []),
    "egg": (None, None, [A("life", 0.05)], []),
    "water": (None, None, [A("water", 0.1)], []),
    "brine": (None, None, [A("water", 0.1), A("earth", 0.05)], []),
    "alcohol": (None, None, [A("fire", 0.1)], ["solvent"]),
    "vinegar": (None, None, [], ["solvent"]),
    "lye": (None, None, [], ["solvent"]),
}

# ------------------------------------------------------------- new materials
# raw ones have no source yet (pending in tests/test_altera_sim_resources.cpp):
# monster parts come with the monsters (M5), rare minerals with the caravans (realm),
# the mosses with their forage places (worldgen).
NEW_MATERIALS = [
    {"id": "troll_liver", "tags": ["reagent", "monster_part", "animal", "perishable"], "item": "troll_liver",
     "p": {"density": 0.6, "moisture": 0.7, "toxic": 0.1, "conductivity": 0.3, "mana_capacity": 0.2},
     "effects": [A("life", 0.7), {"id": "bitter", "k": 0.3}], "worth": 6.0},
    {"id": "dragon_blood", "tags": ["reagent", "monster_part", "liquid", "blood"], "item": "dragon_blood",
     "shape": "liquid", "p": {"density": 0.6, "moisture": 0.9, "flammable": 0.6, "conductivity": 0.6, "mana_capacity": 0.5},
     "effects": [A("fire", 0.8), A("essence", 0.2)], "worth": 30.0},
    {"id": "dragon_bone", "tags": ["bone", "monster_part", "rune_bone"], "item": "dragon_bone",
     "p": {"hardness": 0.75, "toughness": 0.7, "density": 0.6, "flex": 0.15, "edge": 0.4, "purity": 0.9,
           "conductivity": 0.8, "mana_capacity": 0.95}, "shape": "bone", "len": 3,
     "effects": [A("fire", 0.2)], "worth": 25.0},
    {"id": "moonstone", "tags": ["stone", "gem", "mineral", "rune_stone"], "item": "moonstone",
     "p": {"hardness": 0.6, "toughness": 0.3, "density": 0.7, "purity": 0.85, "conductivity": 0.7, "mana_capacity": 0.85},
     "effects": [A("light", 0.3), A("bond", 0.2)], "worth": 15.0},
    {"id": "mithril", "tags": ["metal", "soft_metal", "precious", "rune_metal"], "item": "mithril",
     "p": {"hardness": 0.7, "toughness": 0.8, "density": 0.35, "flex": 0.5, "melt": 4, "purity": 0.95, "edge": 0.5,
           "conductivity": 0.95, "mana_capacity": 0.8}, "salvage": "mithril", "worth": 40.0,
     "states": [{"id": "hot", "when": "heat>=3", "temp": True, "add": {"workable": 0.8}}]},
    {"id": "sulfur", "tags": ["mineral", "reagent", "fuel"], "item": "sulfur",
     "p": {"hardness": 0.2, "density": 0.5, "flammable": 0.9, "toxic": 0.2},
     "effects": [A("fire", 0.35), A("poison", 0.1)], "worth": 2.0},
    {"id": "quicksilver", "tags": ["metal_raw", "liquid", "reagent", "solvent"], "item": "quicksilver", "shape": "liquid",
     "p": {"density": 1.0, "toxic": 0.6, "conductivity": 0.6, "mana_capacity": 0.3},
     "effects": [A("essence", 0.3), A("poison", 0.3)], "worth": 6.0},
    {"id": "night_moss", "tags": ["herb", "reagent", "moss"], "item": "night_moss",
     "p": {"density": 0.2, "moisture": 0.5, "flammable": 0.2},
     "effects": [A("shadow", 0.45), A("sleep", 0.1)], "worth": 2.5},
    {"id": "moon_moss", "tags": ["herb", "reagent", "moss"], "item": "moon_moss",
     "p": {"density": 0.2, "moisture": 0.5, "flammable": 0.2},
     "effects": [A("light", 0.3), A("bond", 0.25)], "worth": 3.0},
    # made ones
    {"id": "spirit", "tags": ["alcohol", "liquid", "solvent", "distilled"], "raw": False, "shape": "liquid",
     "p": {"density": 0.4, "moisture": 0.4, "flammable": 0.8, "purity": 0.9},
     "effects": [A("fire", 0.2), {"id": "warm", "k": 0.4}, {"id": "antiseptic", "k": 0.3}], "worth": 2.0},
    {"id": "parchment", "tags": ["hide", "parchment", "writing"], "raw": False, "shape": "sheet", "len": 2,
     "p": {"hardness": 0.2, "toughness": 0.4, "density": 0.4, "flex": 0.6, "flammable": 0.6, "porous": 0.3,
           "moisture": 0.1, "fiber": 0.3, "conductivity": 0.5, "mana_capacity": 0.3}, "worth": 1.5},
]
NEW_MATERIAL_REFINE = {"alcohol": [{"process": "distill", "to": "spirit"}],
                       "fruit_wine": [{"process": "distill", "to": "spirit"}]}

# AL-117 tail: the monster parts of AL-114 (content/creatures/_items.json) become materials
MONSTER_MATERIALS = [
    {"id": "spider_venom", "tags": ["reagent", "monster_part", "liquid", "poisonous"], "item": "spider_venom", "shape": "liquid",
     "p": {"density": 0.5, "moisture": 0.8, "toxic": 0.7}, "effects": [A("poison", 0.6), {"id": "poison", "k": 0.5}], "worth": 6.0},
    {"id": "drowner_brain", "tags": ["reagent", "monster_part", "perishable"], "item": "drowner_brain",
     "p": {"density": 0.5, "moisture": 0.8, "toxic": 0.2, "conductivity": 0.3, "mana_capacity": 0.3},
     "effects": [A("mind", 0.4), A("water", 0.2)], "worth": 5.0},
    {"id": "basilisk_eye", "tags": ["gem", "monster_part", "reagent", "rune_stone"], "item": "basilisk_eye",
     "p": {"hardness": 0.6, "toughness": 0.3, "density": 0.6, "purity": 0.8, "conductivity": 0.8, "mana_capacity": 0.9},
     "effects": [A("stone", 0.5), A("death", 0.2, "~")], "worth": 40.0},
    {"id": "griffin_feather", "tags": ["feather", "monster_part", "reagent"], "item": "griffin_feather",
     "p": {"density": 0.1, "flex": 0.6, "flammable": 0.5, "conductivity": 0.45, "mana_capacity": 0.35},
     "effects": [A("air", 0.4), A("beasts", 0.15)], "worth": 4.0},
]
MONSTER_ITEMS = {m["id"]: m["id"] for m in MONSTER_MATERIALS}  # item id -> material
NEW_MATERIALS += MONSTER_MATERIALS

# the rare reagents from afar: the travelling trader carries a few (trade.json roles.trader.stock)
TRADER_STOCK = {"moonstone": 1, "mithril": 1, "sulfur": 2, "quicksilver": 1, "dragon_blood": 1, "dragon_bone": 1,
                "troll_liver": 1}

# the mosses: an alchemist looks for them where other things grow (no new world objects:
# new objects shift the ids the world hashes by)
INTERACTIONS = [
    {"id": "gather_night_moss", "category": "work", "target": "object:mushroom_glade",
     "pre": ["knows:alchemy", "target_stock:mushrooms>=2"],
     "eff": ["has:night_moss+1", "target_stock:mushrooms-2", "skill:herbalism+0.01", "skill:alchemy+0.005", "body:fatigue+0.03"],
     "from_target": True, "duration": 4, "cost": 4, "fsm": "Work", "legal": [], "visibility": "public", "reserve": True,
     "_part": "AL-117"},
    {"id": "gather_moon_moss", "category": "work", "target": "object:limestone_outcrop",
     "pre": ["knows:alchemy", "target_stock:limestone>=2"],
     "eff": ["has:moon_moss+1", "target_stock:limestone-2", "skill:herbalism+0.01", "skill:alchemy+0.005", "body:fatigue+0.03"],
     "from_target": True, "duration": 4, "cost": 4, "fsm": "Work", "legal": [], "visibility": "public", "reserve": True,
     "_part": "AL-117"},
]

# ----------------------------------------------------------------- shapes
NEW_SHAPES = {
    "crucible_raw": '{"id": "crucible_raw", "stage": "finished", "role": "body", "len": [1, 2, 1], "v": {"capacity": 0.3}}',
    "alembic_raw": '{"id": "alembic_raw", "stage": "finished", "role": "body", "len": [1, 3, 2], "v": {"capacity": 0.5}}',
}
SHAPE_MAPS = {"mould": {"lump": ["crucible_raw"]}, "hammer": {"plate": ["alembic_raw"]}}

# -------------------------------------------------------------- processes
TRIGGERS = ["constant", "on_hit", "on_touch", "on_word", "on_threat"]
NEW_PROCESSES = {
    "distill": {"id": "distill", "op": "shape", "skill": "alchemy", "skill_by": [{"tag": "alcohol", "skill": "brewing"}],
                "requires": {"station": ["alembic"], "heat_min": 2},
                "inputs": [{"slot": "work", "shape": ["liquid"]}],
                "time": 6, "difficulty": {"base": 0.4, "add": "0.3*aspects"},
                "transform": {"moisture": "x*0.5"},
                "concentrate": {"aspect.": 1.6, "*": 0.7},
                "flaws": [{"id": "burnt", "p": "0.2*(1-q)"}],
                "risk": [{"id": "scald", "p": 0.03, "eff": ["body:health-0.03"]}],
                "marks": ["distilled"], "risky": True, "chain_in": ["liquid"], "chain_out": ["liquid"]},
    "calcine": {"id": "calcine", "op": "shape", "skill": "alchemy", "skill_by": [{"tag": "metal", "skill": "smithing"}],
                "requires": {"station": ["crucible"], "heat_min": 3},
                "inputs": [{"slot": "work", "tag": ["mineral", "herb", "bone", "metal", "metal_raw", "reagent", "fuel"]}],
                "time": 8, "difficulty": {"base": 0.45},
                "shape_map": {"lump": ["powder"], "bone": ["powder"], "fiber_raw": ["powder"], "powder": ["powder"]},
                "transform": {"moisture": "0", "flammable": "x*0.2"},
                "concentrate": {"aspect.": 1.5, "*": 0.3},
                "risk": [{"id": "burn_hand", "p": 0.04, "eff": ["body:health-0.04"]}],
                "marks": ["calcined"], "risky": True, "chain_in": ["lump"], "chain_out": ["powder"]},
    "dissolve": {"id": "dissolve", "op": "soak", "skill": "alchemy", "skill_by": [{"tag": "herb", "skill": "herbalism"}],
                 "requires": {"station": ["pot", "tub", "bench", "alembic"]},
                 "inputs": [{"slot": "work", "shape": ["any"]}, {"slot": "liquid", "tag": ["solvent", "water"], "max": 2}],
                 "time": 4, "difficulty": {"base": 0.3},
                 "shape_map": {"lump": ["liquid"], "powder": ["liquid"], "fiber_raw": ["liquid"], "bone": ["liquid"], "liquid": ["liquid"]},
                 "concentrate": {"aspect.": 1.2},
                 "extract": "dissolve", "marks": ["dissolved"], "chain_in": ["any"], "chain_out": ["liquid"]},
    "scrape": {"id": "scrape", "op": "shape", "skill": "tanning",
               "requires": {"tool": ["cutting"], "station": ["frame", "bench"]},
               "inputs": [{"slot": "work", "tag": ["hide"], "shape": ["hide", "sheet"]}],
               "time": 12, "difficulty": {"base": 0.35},
               "shape_map": {"hide": ["sheet"]}, "to_material": "parchment",
               "flaws": [{"id": "uneven", "p": "0.3*(1-q)"}]},
    "inscribe": {"id": "inscribe", "op": "inscribe", "skill": "runecraft",
                 "requires": {"tool": ["cutting", "needle"]},
                 "inputs": [{"slot": "work", "shape": ["any"]}, {"slot": "ink", "optional": True, "shape": ["any"],
                             "tag": ["liquid", "reagent", "blood"], "either": True}],
                 "time": 6, "difficulty": {"base": 0.45, "add": "0.3*(1-conductivity)"},
                 "marks": ["inscribed"], "worth": 1.2},
    "bind": {"id": "bind", "op": "bind", "skill": "runecraft",
             "requires": {"station": ["bench", "alembic"]},
             "inputs": [{"slot": "work", "tag": ["inscribed"]}, {"slot": "reagent", "shape": ["any"], "max": 3}],
             "time": 8, "difficulty": {"base": 0.5, "add": "0.3*rune"},
             "risk": [{"id": "backlash", "p": 0.02, "eff": ["body:health-0.03"]}],
             "modes": TRIGGERS, "marks": ["enchanted"], "risky": True, "worth": 1.3},
}

# --------------------------------------------------------------- stations
# portable: an alembic or a crucible in hand works with the heat of a hearth/forge at hand
# (distill and calcine are shape steps with heat_min, so any hot station at hand serves)
NEW_STATIONS = [
    {"id": "alembic", "_doc": "AL-117: a copper still; distils with the heat of a hearth or forge at hand",
     "tags": ["alembic"], "quality_bonus": 0.05, "even": 0.7},
    {"id": "crucible", "_doc": "AL-117: a fired clay crucible; calcines in a forge or furnace",
     "tags": ["crucible"], "quality_bonus": 0.0, "even": 0.6},
]

# ------------------------------------------------------------- archetypes
# inserted before healing_draught: an aspect potion wins over the herbal draughts it resembles
POTION_ASPECTS = ["fire", "water", "air", "earth", "life", "plants", "beasts", "weather", "metal", "stone", "poison",
                  "blood", "mind", "sleep", "light", "shadow", "death", "space", "time", "bond", "essence"]
ANY_ASPECT = "|".join("aspect." + a + s for a in POTION_ASPECTS for s in ("", ".minus", ".shift"))
POTION_DERIVE = '"derive": {"kind": "consumable", "power": "aspects", "unstable": "unstable", "tags": ["potion"]}'
NEW_FORMS_BEFORE = "healing_draught"
NEW_FORMS = {
    "volatile_flask": '{"id": "volatile_flask", "category": "potion", "item": "volatile_flask", "named": false, "needs": {"shape": ["liquid"], "effects": ["%s"], "unstable": ">=0.4"}, "derive": {"kind": "thrown", "power": "aspects", "unstable": "unstable", "tags": ["grenade"]}}' % ANY_ASPECT,
    "healing_potion": '{"id": "healing_potion", "category": "potion", "item": "healing_potion", "named": false, "needs": {"shape": ["liquid"], "effects": ["aspect.life"], "eff(aspect.life)": ">=0.15", "safe": ">=0.4", "unstable": "<0.4"}, %s}' % POTION_DERIVE,
    "sleeping_draught": '{"id": "sleeping_draught", "category": "potion", "item": "sleeping_draught", "named": false, "needs": {"shape": ["liquid"], "effects": ["aspect.sleep"], "eff(aspect.sleep)": ">=0.15", "safe": ">=0.4", "unstable": "<0.4"}, %s}' % POTION_DERIVE,
    "shadow_draught": '{"id": "shadow_draught", "category": "potion", "item": "shadow_draught", "named": false, "needs": {"shape": ["liquid"], "effects": ["aspect.shadow"], "eff(aspect.shadow)": ">=0.15", "safe": ">=0.4", "unstable": "<0.4"}, %s}' % POTION_DERIVE,
    "fire_draught": '{"id": "fire_draught", "category": "potion", "item": "fire_draught", "named": false, "needs": {"shape": ["liquid"], "effects": ["aspect.fire|aspect.fire.minus"], "eff(aspect.fire) + eff(aspect.fire.minus)": ">=0.15", "safe": ">=0.4", "unstable": "<0.4"}, %s}' % POTION_DERIVE,
    "potion": '{"id": "potion", "category": "potion", "item": "potion", "named": false, "needs": {"shape": ["liquid"], "effects": ["%s"], "aspects": ">=0.15", "safe": ">=0.4", "unstable": "<0.4"}, %s}' % (ANY_ASPECT, POTION_DERIVE),
    "scroll": '{"id": "scroll", "category": "magic", "item": "scroll", "named": false, "needs": {"mat": ["parchment"], "profile": ["enchanted"]}, "derive": {"kind": "consumable", "power": "rune", "tags": ["scroll"]}}',
    "parchment_sheet": '{"id": "parchment_sheet", "category": "material", "item": "parchment", "named": false, "needs": {"mat": ["parchment"], "shape": ["sheet"]}}',
    "crucible": '{"id": "crucible", "category": "tool", "item": "crucible", "named": false, "needs": {"shape": ["crucible_raw"], "state": ["fired"]}, "derive": {"kind": "tool", "tags": ["crucible"]}}',
    "alembic": '{"id": "alembic", "category": "tool", "item": "alembic", "named": false, "needs": {"shape": ["alembic_raw"], "mat": ["metal"]}, "derive": {"kind": "tool", "tags": ["alembic"]}}',
}

# --------------------------------------------------------- starter designs
NEW_DESIGNS = [
    {"id": "crucible", "form": "crucible", "steps": [
        {"process": "mould", "shape": "crucible_raw", "len": 1, "slots": {"work": ["mat:clay"]}},
        {"process": "dry", "slots": {"work": ["step:1"]}},
        {"process": "heat", "mode": "fire", "heat": 2, "slots": {"work": ["step:2"]}}]},
    {"id": "alembic", "form": "alembic", "steps": [
        {"process": "smelt", "slots": {"ore": ["mat:copper_ore"], "fuel": ["mat:charcoal"]}},
        {"process": "hammer", "shape": "bar", "slots": {"work": ["step:1"]}},
        {"process": "hammer", "shape": "plate", "slots": {"work": ["step:2"]}},
        {"process": "hammer", "shape": "alembic_raw", "slots": {"work": ["step:3"]}}]},
    {"id": "healing_potion", "form": "healing_potion", "stations": ["alembic"], "steps": [
        {"process": "dissolve", "shape": "liquid", "slots": {"work": ["mat:yarrow"], "liquid": ["mat:water"]}},
        {"process": "distill", "heat": 2, "slots": {"work": ["step:1"]}}]},
    {"id": "sleeping_draught", "form": "sleeping_draught", "steps": [
        {"process": "dissolve", "shape": "liquid", "slots": {"work": ["mat:poppy"], "liquid": ["mat:water"]}}]},
    {"id": "shadow_draught", "form": "shadow_draught", "steps": [
        {"process": "dissolve", "shape": "liquid", "slots": {"work": ["mat:night_moss"], "liquid": ["mat:water"]}}]},
    {"id": "fire_draught", "form": "fire_draught", "stations": ["crucible"], "steps": [
        {"process": "calcine", "shape": "powder", "heat": 3, "slots": {"work": ["mat:charcoal"]}},
        {"process": "dissolve", "shape": "liquid", "slots": {"work": ["step:1"], "liquid": ["mat:water"]}}]},
    {"id": "potion", "form": "potion", "stations": ["crucible"], "steps": [
        {"process": "calcine", "shape": "powder", "heat": 3, "slots": {"work": ["mat:antler"]}},
        {"process": "dissolve", "shape": "liquid", "slots": {"work": ["step:1"], "liquid": ["mat:water"]}}]},
    {"id": "volatile_flask", "form": "volatile_flask", "stations": ["alembic"], "steps": [
        {"process": "mix", "mode": "grind_herbs", "slots": {"a": ["mat:hemlock"], "b": ["mat:yarrow"]}},
        {"process": "dissolve", "shape": "liquid", "slots": {"work": ["step:1"], "liquid": ["mat:water"]}},
        {"process": "distill", "heat": 2, "slots": {"work": ["step:2"]}},
        {"process": "distill", "heat": 2, "slots": {"work": ["step:3"]}}]},
    {"id": "parchment_sheet", "form": "parchment_sheet", "steps": [
        {"process": "scrape", "shape": "sheet", "slots": {"work": ["mat:hide_raw"]}}]},
    {"id": "scroll", "form": "scroll", "stations": ["alembic"], "steps": [
        {"process": "scrape", "shape": "sheet", "slots": {"work": ["mat:hide_raw"]}},
        {"process": "inscribe", "slots": {"work": ["step:1"]}},
        {"process": "dissolve", "shape": "liquid", "slots": {"work": ["mat:yarrow"], "liquid": ["mat:water"]}},
        {"process": "distill", "heat": 2, "slots": {"work": ["step:3"]}},
        {"process": "bind", "mode": "on_word", "slots": {"work": ["step:2"], "reagent": ["step:4"]}}]},
]

MAGIC_CONFIG = {
    "_doc": "AL-117 (LivelyRealms §7-§8): rune_min_conductivity: a carrier below it takes no rune; aspect_max: cap of an aspect effect; unstable_safe / blast_k: opposed aspects (aspects.json opposes, or + against - of one aspect) blow a step or a flask up with chance (unstable - safe) * blast_k * (1.2 - q); bind_cap_share: rune power <= carrier mana_capacity * share; rune_worth / resonance_worth: worth multipliers; resonance_deeds: deeds for resonance 1 - 1/e; resonance_rune: rune power grows with resonance; triggers: modes of bind; charge_per_use: share of the capacity one use spends.",
    "rune_min_conductivity": 0.3, "aspect_max": 1.0, "unstable_safe": 0.25, "blast_k": 1.2,
    "blast_effects": ["body:health-0.08"], "bind_cap_share": 1.0, "rune_worth": 2.0, "resonance_worth": 3.0,
    "resonance_deeds": 12, "resonance_rune": 0.5, "triggers": TRIGGERS, "default_trigger": "on_word",
    "charge_per_use": 0.25,
}

# --------------------------------------------------------------- the pack
ITEMS = [  # id, material, tags, stack, value, ru, en
    ("potion", None, ["potion", "magic", "drink"], 5, 2.0, ru("зелье", "зелья", "зелью", "зелье", "зельем", "зелье", "n"), "potion"),
    ("healing_potion", None, ["potion", "magic", "medicine"], 5, 3.0, ru("целебное зелье", "целебного зелья", "целебному зелью", "целебное зелье", "целебным зельем", "целебном зелье", "n"), "healing potion"),
    ("sleeping_draught", None, ["potion", "magic", "medicine"], 5, 2.0, ru("сонное зелье", "сонного зелья", "сонному зелью", "сонное зелье", "сонным зельем", "сонном зелье", "n"), "sleeping draught"),
    ("shadow_draught", None, ["potion", "magic"], 5, 4.0, ru("теневое зелье", "теневого зелья", "теневому зелью", "теневое зелье", "теневым зельем", "теневом зелье", "n"), "shadow draught"),
    ("fire_draught", None, ["potion", "magic"], 5, 3.0, ru("огненное зелье", "огненного зелья", "огненному зелью", "огненное зелье", "огненным зельем", "огненном зелье", "n"), "fire draught"),
    ("volatile_flask", None, ["potion", "magic", "weapon"], 3, 3.0, ru("гремучая склянка", "гремучей склянки", "гремучей склянке", "гремучую склянку", "гремучей склянкой", "гремучей склянке", "f"), "volatile flask"),
    ("scroll", None, ["magic", "writing"], 5, 5.0, ru("свиток", "свитка", "свитку", "свиток", "свитком", "свитке", "m"), "scroll"),
    ("parchment", "parchment", ["writing", "craft_raw"], 10, 1.0, ru("пергамент", "пергамента", "пергаменту", "пергамент", "пергаментом", "пергаменте", "m"), "parchment"),
    ("alembic", None, ["tool", "alchemy"], 1, 6.0, ru("перегонный куб", "перегонного куба", "перегонному кубу", "перегонный куб", "перегонным кубом", "перегонном кубе", "m"), "alembic"),
    ("crucible", None, ["tool", "alchemy"], 1, 1.5, ru("тигель", "тигля", "тиглю", "тигель", "тиглем", "тигле", "m"), "crucible"),
    ("troll_liver", "troll_liver", ["reagent", "monster_part", "perishable"], 3, 8.0, ru("печень тролля", "печени тролля", "печени тролля", "печень тролля", "печенью тролля", "печени тролля", "f"), "troll liver"),
    ("dragon_blood", "dragon_blood", ["reagent", "monster_part"], 3, 30.0, ru("драконья кровь", "драконьей крови", "драконьей крови", "драконью кровь", "драконьей кровью", "драконьей крови", "f"), "dragon blood"),
    ("dragon_bone", "dragon_bone", ["monster_part", "craft_raw"], 3, 25.0, ru("драконья кость", "драконьей кости", "драконьей кости", "драконью кость", "драконьей костью", "драконьей кости", "f"), "dragon bone"),
    ("moonstone", "moonstone", ["gem", "craft_raw", "trade_good"], 10, 15.0, ru("лунный камень", "лунного камня", "лунному камню", "лунный камень", "лунным камнем", "лунном камне", "m"), "moonstone"),
    ("mithril", "mithril", ["metal", "craft_raw", "trade_good"], 10, 40.0, ru("мифрил", "мифрила", "мифрилу", "мифрил", "мифрилом", "мифриле", "m"), "mithril"),
    ("sulfur", "sulfur", ["reagent", "craft_raw", "trade_good"], 20, 2.0, ru("сера", "серы", "сере", "серу", "серой", "сере", "f"), "sulfur"),
    ("quicksilver", "quicksilver", ["reagent", "craft_raw", "trade_good"], 10, 6.0, ru("ртуть", "ртути", "ртути", "ртуть", "ртутью", "ртути", "f"), "quicksilver"),
    ("night_moss", "night_moss", ["herb", "reagent"], 20, 2.5, ru("ночной мох", "ночного мха", "ночному мху", "ночной мох", "ночным мхом", "ночном мхе", "m"), "night moss"),
    ("moon_moss", "moon_moss", ["herb", "reagent"], 20, 3.0, ru("лунный мох", "лунного мха", "лунному мху", "лунный мох", "лунным мхом", "лунном мхе", "m"), "moon moss"),
]

MOODLETS = [  # id, value, ttl, ru, en
    ("drank_potion", 0.03, 12, "Выпил зелье", "Drank a potion"),
    ("potion_burn", -0.15, 12, "Жжёт изнутри", "Burning inside"),
    ("potion_cold", -0.1, 12, "Знобит от зелья", "Chilled by a potion"),
    ("potion_heal_wounds", 0.1, 24, "Раны затягиваются", "Wounds closing"),
    ("potion_hide", 0.02, 6, "Укрыт тенью", "Veiled in shadow"),
    ("potion_calm", 0.1, 12, "Спокойствие", "Calm"),
    ("potion_fear", -0.2, 12, "Наведённый страх", "Uncanny fear"),
    ("potion_sleep", 0.0, 8, "Клонит в сон", "Drowsy"),
    ("potion_poison", -0.25, 24, "Отравлен", "Poisoned"),
    ("potion_strength", 0.08, 12, "Прилив сил", "Surge of strength"),
    ("potion_armor", 0.05, 12, "Кожа как камень", "Skin like stone"),
    ("potion_mana", 0.05, 12, "Сила в жилах", "Power in the veins"),
]

KNOWLEDGE = [
    {"id": "alchemy", "requires": ["herbalism"], "learn_ticks": 72, "self_ticks": 600, "start_chance_adult": 0.0,
     "family_knowledge": True, "_part": "AL-117"},
    {"id": "runecraft", "requires": ["alchemy"], "learn_ticks": 96, "self_ticks": None, "start_chance_adult": 0.0,
     "family_knowledge": False, "_part": "AL-117"},
]

# ---------------------------------------------------------------- loc
def adj_made(stem_ru):  # "серебрян" -> made-of phrase
    return stem_ru + "{form:g|ый|ая|ое|ые} {form}"


LOC_RU = {
    "material.troll_liver.name": ru("печень тролля", "печени тролля", "печени тролля", "печень тролля", "печенью тролля", "печени тролля", "f"),
    "material.troll_liver.made": "{form} из печени тролля",
    "material.dragon_blood.name": ru("драконья кровь", "драконьей крови", "драконьей крови", "драконью кровь", "драконьей кровью", "драконьей крови", "f"),
    "material.dragon_blood.made": "{form} на драконьей крови",
    "material.dragon_bone.name": ru("драконья кость", "драконьей кости", "драконьей кости", "драконью кость", "драконьей костью", "драконьей кости", "f"),
    "material.dragon_bone.made": "{form} из драконьей кости",
    "material.moonstone.name": ru("лунный камень", "лунного камня", "лунному камню", "лунный камень", "лунным камнем", "лунном камне", "m"),
    "material.moonstone.made": "{form} из лунного камня",
    "material.mithril.name": ru("мифрил", "мифрила", "мифрилу", "мифрил", "мифрилом", "мифриле", "m"),
    "material.mithril.made": adj_made("мифрилов"),
    "material.sulfur.name": ru("сера", "серы", "сере", "серу", "серой", "сере", "f"),
    "material.sulfur.made": "{form} из серы",
    "material.quicksilver.name": ru("ртуть", "ртути", "ртути", "ртуть", "ртутью", "ртути", "f"),
    "material.quicksilver.made": "{form} на ртути",
    "material.night_moss.name": ru("ночной мох", "ночного мха", "ночному мху", "ночной мох", "ночным мхом", "ночном мхе", "m"),
    "material.night_moss.made": "{form} из ночного мха",
    "material.moon_moss.name": ru("лунный мох", "лунного мха", "лунному мху", "лунный мох", "лунным мхом", "лунном мхе", "m"),
    "material.moon_moss.made": "{form} из лунного мха",
    "material.spider_venom.name": ru("паучий яд", "паучьего яда", "паучьему яду", "паучий яд", "паучьим ядом", "паучьем яде", "m"),
    "material.spider_venom.made": "{form} на паучьем яде",
    "material.drowner_brain.name": ru("мозг утопца", "мозга утопца", "мозгу утопца", "мозг утопца", "мозгом утопца", "мозге утопца", "m"),
    "material.drowner_brain.made": "{form} из мозга утопца",
    "material.basilisk_eye.name": ru("глаз василиска", "глаза василиска", "глазу василиска", "глаз василиска", "глазом василиска", "глазе василиска", "m"),
    "material.basilisk_eye.made": "{form} с глазом василиска",
    "material.griffin_feather.name": ru("перо грифона", "пера грифона", "перу грифона", "перо грифона", "пером грифона", "пере грифона", "n"),
    "material.griffin_feather.made": "{form} из пера грифона",
    "interaction.gather_night_moss.name": "собрать ночной мох",
    "interaction.gather_moon_moss.name": "собрать лунный мох",
    "ui2d.craft.drink": "Выпить",
    "ui2d.craft.use_rune": "Применить руну",
    "ui2d.craft.trigger.constant": "постоянно", "ui2d.craft.trigger.on_hit": "при ударе",
    "ui2d.craft.trigger.on_touch": "касанием", "ui2d.craft.trigger.on_word": "по слову",
    "ui2d.craft.trigger.on_threat": "при угрозе",
    "ui2d.craft.rune": "руна: {trigger}, заряд {charge}%",
    "ui2d.refused.not_a_potion": "Это нельзя выпить",
    "ui2d.refused.no_charge": "Руна выдохлась",
    "ui2d.refused.not_enchanted": "В вещи нет зачарования",
    "ui2d.refused.blast": "Склянка взорвалась",
    "material.spirit.name": ru("спирт", "спирта", "спирту", "спирт", "спиртом", "спирте", "m"),
    "material.spirit.made": "{form} на спирту",
    "material.parchment.name": ru("пергамент", "пергамента", "пергаменту", "пергамент", "пергаментом", "пергаменте", "m"),
    "material.parchment.made": "пергаментн{form:g|ый|ая|ое|ые} {form}",
    "shape.crucible_raw.name": {"nom": "тигель", "g": "m"},
    "shape.alembic_raw.name": {"nom": "перегонный куб", "g": "m"},
    "process.distill.name": {"nom": "перегонка", "g": "f"},
    "process.calcine.name": {"nom": "прокаливание", "g": "n"},
    "process.dissolve.name": {"nom": "растворение", "g": "n"},
    "process.scrape.name": {"nom": "выделка пергамента", "g": "f"},
    "process.inscribe.name": {"nom": "нанесение руны", "g": "n"},
    "process.bind.name": {"nom": "связывание", "g": "n"},
    "process.mode.constant.name": "постоянно",
    "process.mode.on_hit.name": "при ударе",
    "process.mode.on_touch.name": "при касании",
    "process.mode.on_word.name": "по слову",
    "process.mode.on_threat.name": "при угрозе",
    "station.alembic.name": {"nom": "перегонный куб", "g": "m"},
    "station.crucible.name": {"nom": "тигель", "g": "m"},
    "form.volatile_flask.name": {"nom": "гремучая склянка", "g": "f"},
    "form.healing_potion.name": {"nom": "целебное зелье", "g": "n"},
    "form.sleeping_draught.name": {"nom": "сонное зелье", "g": "n"},
    "form.shadow_draught.name": {"nom": "теневое зелье", "g": "n"},
    "form.fire_draught.name": {"nom": "огненное зелье", "g": "n"},
    "form.potion.name": {"nom": "зелье", "g": "n"},
    "form.scroll.name": {"nom": "свиток", "g": "m"},
    "form.parchment_sheet.name": {"nom": "лист пергамента", "g": "m"},
    "form.crucible.name": {"nom": "тигель", "g": "m"},
    "form.alembic.name": {"nom": "перегонный куб", "g": "m"},
    "craft.reason.carrier_cannot_hold": "{arg} не держит магию: руна не ляжет",
    "craft.reason.no_rune": "на вещи нет руны: сначала нанести руну",
    "craft.reason.no_aspect": "в реагенте нет аспектов магии",
    "knowledge.alchemy.name": "Алхимия",
    "knowledge.runecraft.name": "Руническое дело",
    "craft.enchanted": "зачаровано",
    "craft.rune.trigger": "срабатывает: {trigger}",
    "craft.rune.charge": "заряд {charge}",
    "craft.resonance": "резонанс {value}",
    "alchemy.drink.ok": "{who} выпивает {item}",
    "alchemy.drink.blast": "склянка взрывается в руках у {who}",
    "alchemy.reason.not_a_potion": "это нельзя выпить как зелье",
    "alchemy.reason.no_charge": "руна выдохлась: нужен отдых или подпитка",
    "alchemy.reason.not_enchanted": "в вещи нет зачарования",
    "alchemy.reason.no_such_item": "такой вещи нет",
    "alchemy.reason.blast": "склянка взорвалась",
}
LOC_EN = {
    "material.troll_liver.name": "troll liver", "material.troll_liver.made": "troll liver {form}",
    "material.dragon_blood.name": "dragon blood", "material.dragon_blood.made": "dragon blood {form}",
    "material.dragon_bone.name": "dragon bone", "material.dragon_bone.made": "dragon bone {form}",
    "material.moonstone.name": "moonstone", "material.moonstone.made": "moonstone {form}",
    "material.mithril.name": "mithril", "material.mithril.made": "mithril {form}",
    "material.sulfur.name": "sulfur", "material.sulfur.made": "sulfur {form}",
    "material.quicksilver.name": "quicksilver", "material.quicksilver.made": "quicksilver {form}",
    "material.night_moss.name": "night moss", "material.night_moss.made": "night moss {form}",
    "material.moon_moss.name": "moon moss", "material.moon_moss.made": "moon moss {form}",
    "material.spider_venom.name": "spider venom", "material.spider_venom.made": "spider venom {form}",
    "material.drowner_brain.name": "drowner brain", "material.drowner_brain.made": "drowner brain {form}",
    "material.basilisk_eye.name": "basilisk eye", "material.basilisk_eye.made": "basilisk eye {form}",
    "material.griffin_feather.name": "griffin feather", "material.griffin_feather.made": "griffin feather {form}",
    "interaction.gather_night_moss.name": "gather night moss", "interaction.gather_moon_moss.name": "gather moon moss",
    "ui2d.craft.trigger.constant": "constant", "ui2d.craft.trigger.on_hit": "on hit",
    "ui2d.craft.trigger.on_touch": "by touch", "ui2d.craft.trigger.on_word": "on a word",
    "ui2d.craft.trigger.on_threat": "on a threat",
    "ui2d.craft.drink": "Drink", "ui2d.craft.use_rune": "Use the rune", "ui2d.craft.rune": "rune: {trigger}, charge {charge}%",
    "ui2d.refused.not_a_potion": "That cannot be drunk", "ui2d.refused.no_charge": "The rune is spent",
    "ui2d.refused.not_enchanted": "There is no enchantment in it", "ui2d.refused.blast": "The flask burst",
    "material.spirit.name": "spirit", "material.spirit.made": "spirit {form}",
    "material.parchment.name": "parchment", "material.parchment.made": "parchment {form}",
    "shape.crucible_raw.name": "crucible", "shape.alembic_raw.name": "alembic",
    "process.distill.name": "distil", "process.calcine.name": "calcine", "process.dissolve.name": "dissolve",
    "process.scrape.name": "scrape parchment", "process.inscribe.name": "inscribe a rune", "process.bind.name": "bind",
    "process.mode.constant.name": "always", "process.mode.on_hit.name": "on hit", "process.mode.on_touch.name": "on touch",
    "process.mode.on_word.name": "on a word", "process.mode.on_threat.name": "on threat",
    "station.alembic.name": "alembic", "station.crucible.name": "crucible",
    "form.volatile_flask.name": "volatile flask", "form.healing_potion.name": "healing potion",
    "form.sleeping_draught.name": "sleeping draught", "form.shadow_draught.name": "shadow draught",
    "form.fire_draught.name": "fire draught", "form.potion.name": "potion", "form.scroll.name": "scroll",
    "form.parchment_sheet.name": "parchment sheet", "form.crucible.name": "crucible", "form.alembic.name": "alembic",
    "craft.reason.carrier_cannot_hold": "{arg} does not hold magic: no rune will take",
    "craft.reason.no_rune": "no rune on it: inscribe one first",
    "craft.reason.no_aspect": "the reagent carries no aspect of magic",
    "knowledge.alchemy.name": "Alchemy", "knowledge.runecraft.name": "Runecraft",
    "craft.enchanted": "enchanted", "craft.rune.trigger": "fires: {trigger}", "craft.rune.charge": "charge {charge}",
    "craft.resonance": "resonance {value}",
    "alchemy.drink.ok": "{who} drinks {item}", "alchemy.drink.blast": "the flask bursts in {who}'s hands",
    "alchemy.reason.not_a_potion": "that cannot be drunk as a potion",
    "alchemy.reason.no_charge": "the rune is spent: it needs rest or feeding",
    "alchemy.reason.not_enchanted": "there is no enchantment in it",
    "alchemy.reason.no_such_item": "no such thing", "alchemy.reason.blast": "the flask burst",
}

PROC_GLYPHS = {"distill": ["⚗", "u"], "calcine": ["♁", "z"], "dissolve": ["◒", "u"], "scrape": ["▭", "e"],
               "inscribe": ["ᚱ", "W"], "bind": ["✶", "H"]}
STATION_GLYPHS = {"alembic": "⚗", "crucible": "∪"}


# ---------------------------------------------------------------- helpers
def add_lines(path, entries, before=None):
    """Insert one-object-per-line entries missing from a list file (before entry `before`, else at the end)."""
    t = open(path, encoding="utf-8").read()
    for eid, line in entries.items():
        m = re.search(r'\{"id": "%s"[^\n]*\}(?=,?\n)' % re.escape(eid), t)
        if m:  # ours: one line, kept up to date
            t = t[:m.start()] + line + t[m.end():]
            continue
        m = re.search(r'\n(\s*)\{"id": "%s"' % re.escape(before), t) if before else None
        if m:
            t = t[:m.start()] + "\n" + m.group(1) + line + "," + t[m.start():]
        else:
            end = t.rstrip().rfind("]")
            start = t.rfind("\n", 0, end)
            head = t[:start].rstrip()
            t = head + ",\n  " + line + t[start:]
    json.loads(t)
    open(path, "w", encoding="utf-8").write(t)


def add_shape_map(path, proc, extra):
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


def add_processes(path, procs):
    """Processes are hand-formatted; a new one goes in as a compact block before the closing bracket."""
    t = open(path, encoding="utf-8").read()
    for pid, p in procs.items():
        body = json.dumps(p, ensure_ascii=False)
        m = re.search(r'\{"id": "%s", "op"[^\n]*\}(?=,?\n)' % re.escape(pid), t)
        if m:  # ours: one line, kept up to date
            t = t[:m.start()] + body + t[m.end():]
            continue
        end = t.rstrip().rfind("]")
        start = t.rfind("\n", 0, end)
        head = t[:start].rstrip()
        t = head + ",\n\n  " + body + t[start:]
    json.loads(t)
    open(path, "w", encoding="utf-8").write(t)


def add_blend(path, props):
    t = open(path, encoding="utf-8").read()
    for prop, rule in props.items():
        if '"%s": "' % prop in t[t.find('"blend"'):]:
            continue
        t = t.replace('"waterproof": "max"', '"waterproof": "max", "%s": "%s"' % (prop, rule), 1)
    json.loads(t)
    open(path, "w", encoding="utf-8").write(t)


def main():
    craft = lambda f: P("content/ai/craft", f)
    # materials
    md = load(craft("materials.json"))
    have = {m["id"]: m for m in md["materials"]}
    for nm in NEW_MATERIALS:
        if nm["id"] in have:
            have[nm["id"]].clear()
            have[nm["id"]].update(nm)
        else:
            md["materials"].append(dict(nm))
    have = {m["id"]: m for m in md["materials"]}
    for mid, (cond, cap, effs, tags) in PATCH.items():
        m = have[mid]
        if cond is not None:
            m.setdefault("p", {})["conductivity"] = cond
            m["p"]["mana_capacity"] = cap
        el = m.setdefault("effects", [])
        for e in effs:
            for x in el:
                if x["id"] == e["id"]:
                    x["k"] = e["k"]
                    break
            else:
                el.append(dict(e))
        if not el:
            del m["effects"]
        m["tags"] += [t for t in tags if t not in m["tags"]]
    for mid, refs in NEW_MATERIAL_REFINE.items():
        rl = have[mid].setdefault("refine", [])
        for r in refs:
            if not any(x.get("process") == r["process"] for x in rl):
                rl.append(r)
    save(craft("materials.json"), md)

    add_lines(craft("shapes.json"), NEW_SHAPES)
    for proc, sm in SHAPE_MAPS.items():
        add_shape_map(craft("processes.json"), proc, sm)
    add_processes(craft("processes.json"), NEW_PROCESSES)
    add_blend(craft("functions.json"), {"conductivity": "max", "mana_capacity": "max"})

    sd = load(craft("stations.json"))
    ids = {s["id"] for s in sd["stations"]}
    sd["stations"] += [s for s in NEW_STATIONS if s["id"] not in ids]
    save(craft("stations.json"), sd)

    add_lines(craft("forms.json"), NEW_FORMS, before=NEW_FORMS_BEFORE)

    dd = load(craft("designs.json"))
    at = {x["id"]: i for i, x in enumerate(dd["designs"])}
    for ds in NEW_DESIGNS:
        if ds["id"] in at:
            dd["designs"][at[ds["id"]]] = ds
        else:
            dd["designs"].append(ds)
    save(craft("designs.json"), dd)

    cfg = load(craft("crafting_config.json"))
    cfg["magic"] = MAGIC_CONFIG
    save(craft("crafting_config.json"), cfg)

    # the pack
    pack = lambda f: P("content/ai/alchemy", f)
    items = []
    loc_ru = dict(LOC_RU)
    loc_en = dict(LOC_EN)
    for (iid, mat, tags, stack, value, rn, en) in ITEMS:
        e = {"id": iid}
        if mat:
            e["material"] = mat
        e.update({"tags": tags, "stack": stack, "value": value, "_part": "AL-117"})
        items.append(e)
        loc_ru["item.%s.name" % iid] = rn
        loc_en["item.%s.name" % iid] = en
    save(pack("items.json"), {"_doc": "AL-117: potions, scrolls, portable alchemy stations (an item whose id is a station id works as that station in hand) and the reagents of alchemy. Generated by tools/content_gen/gen_alchemy.py.", "items": items})
    save(pack("needs.json"), {"_doc": "AL-117: moodlets of drinking potions (potion_<body field of content/magic/aspects.json>).", "moodlets": [{"id": m[0], "value": m[1], "ttl": m[2], "_part": "AL-117"} for m in MOODLETS]})
    for (mid, _v, _t, mr, me) in MOODLETS:
        loc_ru["moodlet.%s.name" % mid] = mr
        loc_en["moodlet.%s.name" % mid] = me
    save(pack("interactions.json"), {"_doc": "AL-117: where an alchemist finds the mosses (at existing places: no new world objects).", "interactions": INTERACTIONS})
    # the monster parts of AL-114 carry their materials
    ip = P("content/creatures/_items.json")
    cit = load(ip)
    for e in cit["items"]:
        if e["id"] in MONSTER_ITEMS:
            e["material"] = MONSTER_ITEMS[e["id"]]
    save(ip, cit)
    # the travelling trader carries the rare reagents from afar
    # (a text edit of the one stock line: trade.json keeps its hand layout)
    tp = P("content/ai/trade.json")
    with open(tp, encoding="utf-8") as f:
        txt = f.read()
    st = load(tp)["roles"]["trader"]["stock"]
    add = ", ".join('"%s": %d' % (k, v) for k, v in TRADER_STOCK.items() if k not in st)
    if add:
        line = '"stock": ' + json.dumps(st, ensure_ascii=False)
        assert txt.count(line) == 1, "trade.json: the trader stock line moved"
        txt = txt.replace(line, line[:-1] + ", " + add + "}")
        with open(tp, "w", encoding="utf-8", newline="\n") as f:
            f.write(txt)
    save(pack("knowledge.json"), {"_doc": "AL-117: alchemy (distil, calcine, dissolve) and runecraft (inscribe, bind). Nobody starts with them (no world roll): learnt by doing or from a teacher.", "knowledge": KNOWLEDGE})
    # the craft acts of the starter designs (craft:<design>)
    for ds in NEW_DESIGNS:
        fr = LOC_RU["form.%s.name" % ds["form"]]["nom"]
        loc_ru["interaction.craft:%s.name" % ds["id"]] = "сделать: " + fr
        loc_en["interaction.craft:%s.name" % ds["id"]] = "make: " + LOC_EN["form.%s.name" % ds["form"]]
    for lang, d in (("ru", loc_ru), ("en", loc_en)):
        save(P("content/loc", lang, "alchemy.json"), dict(sorted(d.items())))

    # 2D workshop glyphs
    lp = P("content/sprites_2d/craft2d.json")
    look = load(lp)
    for k, v in PROC_GLYPHS.items():
        look["processes"].setdefault(k, v)
    for k, v in STATION_GLYPHS.items():
        look["station_glyphs"].setdefault(k, v)
    save(lp, look)


if __name__ == "__main__":
    main()
