#!/usr/bin/env python3
"""LR-161: which picture each of the sim's things, objects and acts gets in the 2D client.

The sprites themselves are grids in icons_2d_src.py (gen_sprites_2d.py puts them into
sprites.json); this script only writes the mapping, idempotently:
  content/sprites_2d/craft2d.json "items"   - item id -> {sprite, recolour} (sheet, workshop, trade);
  content/sprites_2d/map2d.json   "objects" - object type -> {sprite, recolour, glyph, colour};
  content/sprites_2d/map2d.json   "acts"    - [regex on the act / goal id, sprite], appended
                                              after the AL-95 rules (the first match wins).
An id that already has a record keeps it (hand-made records win). The check that every id of
the content has a picture: python tools/validate_content.py icons
Rerun after a change here:  python tools/content_gen/gen_icons_2d.py
"""
import json
import os
from collections import OrderedDict

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SP = os.path.join(ROOT, "content", "sprites_2d")


def it(sprite, **rc):
    return {"sprite": sprite, "recolour": rc} if rc else {"sprite": sprite}


# --- things: item id -> picture (recolour: palette letter -> letter) -----------------------
ITEMS = {
    # food
    "apple_pie": it("it_pie", r="y"), "berry_tart": it("it_pie", r="v"), "cheese_pie": it("it_pie", r="F"),
    "fish_pie": it("it_pie", r="m"), "meat_pie": it("it_pie", r="R"),
    "loaf": it("it_bread"), "nut_bread": it("it_bread", y="o"), "wake_loaf": it("it_bread", y="t"),
    "honey_cake": it("it_bread", y="F", o="y"),
    "baked_apples": it("it_apple", w="y"), "dried_berries": it("it_berries", v="V", w="v"), "acorns": it("it_nut"),
    "baked_roots": it("it_root", o="z"), "egg": it("it_egg"), "eggs_greens": it("it_bowl_food", o="w", y="l"),
    "fish_soup": it("it_bowl_food", o="m", y="U"), "garden_soup": it("it_bowl_food", o="G", y="l"),
    "mushroom_stew": it("it_bowl_food", o="e", y="t"), "milk_porridge": it("it_bowl_food", o="g", y="w"),
    "honey_porridge": it("it_bowl_food"), "meal": it("it_bowl_food", o="z", y="o"),
    "sweet_curd": it("it_bowl_food", o="w", y="F"), "herb_roast": it("it_meat", w="l"),
    "blood_sausage": it("it_sausage", r="R", R="P"), "dried_meat": it("it_meat", r="Z", R="P", w="t"),
    "smoked_fish": it("it_fish", m="o", M="Z"), "roast_fish": it("it_fish", m="t", M="o"),
    "honey_milk": it("it_cup", u="w"), "herb_tea": it("it_cup", u="G"), "mulled_wine": it("it_cup", u="R"),
    "cider": it("it_ale", y="F"), "mead": it("it_ale", y="o"),
    "wine": it("it_bottle", D="R", l="r", K="R", r="y"), "berry_wine": it("it_bottle", D="V", l="v", K="V", r="w"),
    "flour": it("it_flour"), "seeds": it("it_seeds"), "seed_grain": it("it_seeds", y="T"),
    "seed_veg": it("it_seeds", y="l", T="G"), "sheaf": it("it_wheat"), "yeast": it("it_jar", y="w"),
    "hay": it("it_hay"), "thatch": it("it_hay", n="t"),
    # tools and gear
    "axe": it("it_axe"), "hoe": it("it_hoe"), "pitchfork": it("it_fork"), "sickle": it("it_sickle"),
    "spear": it("it_spear"), "digging_stick": it("it_stick"), "wooden_spade": it("it_spade", m="n", M="N"),
    "needle": it("it_needle"), "spindle": it("it_spindle"), "stone_blade": it("it_knife", m="W", M="x"),
    "snare": it("it_snare"), "fishing_line": it("it_rope", t="w", T="g"), "broom": it("it_broom"),
    "ladle": it("it_spoon"), "wooden_spoon": it("it_spoon"), "loom": it("st_loom"),
    "cauldron": it("it_pot"), "crucible": it("it_pot", m="z", M="Z", x="f"), "alembic": it("it_flask", l="U", D="u"),
    "volatile_flask": it("it_flask", l="f", D="r"),
    "bowl": it("it_bowl"), "trencher": it("it_bowl", n="t", N="T"), "clay_jug": it("it_jug"),
    "clay_pot": it("it_jar"), "pack_basket": it("it_basket"), "backpack": it("it_bag"),
    "satchel": it("it_bag", y="e"), "belt": it("it_belt"),
    "tankard": it("it_ale"), "wooden_cup": it("it_cup"),
    # potions
    "potion": it("it_potion"), "healing_potion": it("it_potion"), "fire_draught": it("it_potion", r="f", R="z"),
    "shadow_draught": it("it_potion", r="V", R="x"), "sleeping_draught": it("it_potion", r="b", R="B"),
    "quicksilver": it("it_potion", r="m", R="M"), "dragon_blood": it("it_potion", r="R", R="P"),
    "spider_venom": it("it_potion", r="l", R="G"), "remedy": it("it_jar", y="G"),
    # clothes and armour
    "tunic": it("it_tunic", e="w", E="g"), "gambeson": it("it_tunic", e="t", E="T"), "leather_armour": it("it_tunic"),
    "trousers": it("it_trousers"), "boots": it("it_boots"), "felt_cap": it("it_cap"), "cloak": it("it_cloak"),
    "mail_shirt": it("it_mail"), "helmet": it("it_helmet"),
    # stuff, metals, stones
    "cloth": it("it_cloth"), "cord": it("it_rope"), "thread": it("it_thread"), "spider_silk": it("it_thread", n="W"),
    "parchment": it("it_parchment"), "scroll": it("it_scroll"), "candle": it("it_candle"),
    "clay_candlestick": it("it_candle", w="z", g="Z"),
    "gold": it("it_ingot", m="y", M="o"), "silver": it("it_ingot"), "mithril": it("it_ingot", m="U", M="u"),
    "sulfur": it("it_ore", W="y", m="F", M="o", z="o"), "moonstone": it("it_gem", U="w", u="g"),
    "dried_herbs": it("it_herb", G="T", l="t", D="n"), "moon_moss": it("it_herb", G="U", l="w", D="u"),
    "night_moss": it("it_herb", G="v", l="U", D="V"), "bandage": it("it_bandage"), "carcass": it("it_carcass"),
    "horn": it("it_horn"),
    # trinkets and toys
    "bone_charm": it("it_charm"), "household_idol": it("it_idol"), "artifact": it("it_artifact"),
    "bone_comb": it("it_comb"), "bone_dice": it("it_dice"), "bone_flute": it("it_flute", t="w", x="g"),
    "clay_whistle": it("it_flute", t="z", x="Z"), "drum": it("it_drum"), "clay_pipe": it("it_pipe"),
    "carved_toy": it("it_toy"), "rag_doll": it("it_doll"),
    # beasts' parts
    "basilisk_eye": it("it_gem", U="l", u="G", w="F"), "basilisk_scale": it("it_scale"),
    "dragon_scale": it("it_scale", G="r", l="z", D="R"), "dragon_bone": it("it_bone", w="t", g="T"),
    "drowner_brain": it("it_organ", r="l", R="G"), "troll_liver": it("it_organ", r="e", R="E"),
    "griffin_feather": it("it_feather", w="t", g="o"), "harpy_feather": it("it_feather", w="g", g="W"),
    "manticore_spine": it("it_fang", w="r", g="R", t="x"), "varg_fang": it("it_fang"), "varg_pelt": it("it_pelt"),
    # LR-170: myth creatures of the belts
    "siren_feather": it("it_feather", w="t", g="o"), "gorgon_head": it("it_organ", r="G", R="l"),
    "gorgon_blood": it("it_organ", r="e", R="E"), "hydra_venom": it("it_organ", r="l", R="G"),
    "hydra_scale": it("it_scale"), "chimera_horn": it("it_fang", w="t", g="T"), "nemean_hide": it("it_pelt", W="t", g="o", M="T"),
    "grave_dust": it("ob_rock", W="g", m="g"), "leshy_bark": it("it_stick"), "rusalka_hair": it("it_rope", t="l"),
    "upyr_fang": it("it_fang"), "gorynych_scale": it("it_scale", G="l", l="G"), "kelpie_mane": it("it_rope", t="G"),
    "barrow_gold": it("it_gem", U="t", u="o", w="T"), "albasty_hair": it("it_rope", t="o"),
    "serpent_scale": it("it_scale", G="t", l="o"), "mummy_linen": it("it_rope", t="w"),
    "jackal_pelt": it("it_pelt", W="z", g="Z", M="Z"), "scarab_shell": it("it_scale", G="b", l="B"),
}

# the base things that had only a glyph (the window drew a letter): the glyph stays for the symbol mode
GLYPH_ONLY = {
    "stone": it("ob_rock"), "clay": it("it_ore", W="z", m="z", M="Z", o="Z"), "hide": it("it_pelt", W="e", g="E", M="E"),
    "meat": it("it_meat"), "fish": it("it_fish"), "fiber_raw": it("it_hay", t="l", T="G"), "bast_fiber": it("it_rope", t="o"),
    "branch": it("it_stick"), "honey": it("it_honey"), "milk": it("it_jug", z="w", Z="g"), "eggs": it("it_egg"),
    "berries": it("it_berries"), "vegetables": it("it_root"), "cheese": it("it_cheese"), "bone": it("it_bone"),
    "water": it("ic_bucket"), "wool": it("it_pelt", W="w", g="g", M="g"), "flax": it("it_wheat", y="l", T="G"),
    "charcoal": it("it_ore", W="x", m="M", M="x", o="x", z="x"), "salt": it("it_flour"), "mushrooms": it("it_mushroom"),
    "roots": it("it_root"), "hazelnuts": it("it_nut"), "wild_greens": it("it_herb"), "sinew": it("it_rope", t="w", T="g"),
    "antler": it("it_horn", t="w", T="g"), "feathers": it("it_feather"), "fat": it("it_jar", y="w"),
    "tallow": it("it_candle"), "reeds": it("it_wheat", y="T", T="D"), "straw": it("it_hay"),
    "sand": it("it_flour", w="t", g="T"),
}

# --- objects on the map: type -> picture and symbol (the symbol mode, G) ------------------------
OBJECTS = {
    "barrel": {"sprite": "ob_barrel", "glyph": "o", "colour": "n"},
    "campfire": {"sprite": "st_campfire", "glyph": "☼", "colour": "f"},
    "chopping_block": {"sprite": "ob_block", "glyph": "π", "colour": "n"},
    "corpse": {"sprite": "ob_corpse", "glyph": "%", "colour": "W"},
    "drying_rack": {"sprite": "st_rack", "glyph": "#", "colour": "n"},
    "firewood": {"sprite": "ob_firewood", "glyph": "=", "colour": "n"},
    "furnace": {"sprite": "st_forge", "glyph": "&", "colour": "f"},
    "grave": {"sprite": "ob_grave", "glyph": "+", "colour": "W"},
    "loom_frame": {"sprite": "st_loom", "glyph": "#", "colour": "t"},
    "mortar": {"sprite": "st_mortar", "glyph": "u", "colour": "W"},
    "oven": {"sprite": "st_oven", "glyph": "Ω", "colour": "z"},
    "potter_wheel": {"sprite": "st_wheel", "glyph": "o", "colour": "z"},
    "roof_piece": {"sprite": "ob_roof_piece", "glyph": "^", "colour": "t"},
    "rubble": {"sprite": "ob_rubble", "glyph": ":", "colour": "W"},
    "tanning_pit": {"sprite": "st_pit", "glyph": "O", "colour": "e"},
    "wash_tub": {"sprite": "st_tub", "glyph": "u", "colour": "u"},
    "weaving_frame": {"sprite": "ob_frame", "glyph": "#", "colour": "w"},
    "workbench": {"sprite": "st_bench", "glyph": "π", "colour": "n"},
    "wreckage": {"sprite": "ob_wreck", "glyph": "x", "colour": "N"},
}

# --- acts: [regex on the act or goal id, sprite] -----------------------------------------------
# ACTS_FIRST go before the AL-95 rules (where those would pick a wrong picture), ACTS after them.
ACTS_FIRST = [
    ["tend_livestock", "ic_paw"],
]
ACTS = [
    ["^(take|give)_.*(wine|cider|mead|ale)", "it_ale"],
    ["^(take|give)_.*(fish)", "it_fish"],
    ["^(take|give)_.*(meat|sausage)", "it_meat"],
    ["^(take|give)_.*(bread|loaf|cheese|pie|tart|curd|meal|vegetable|roots|milk|food)", "it_bread"],
    ["take_firewood|gather_firewood|gather_branches|stock_firewood", "ob_firewood"],
    ["^take_seed|set_aside", "it_seeds"],
    ["fell_|_tree$|cut_ash|cut_yew|cut_linden|cut_withies|peel_|strip_|tap_pine|chop", "it_axe"],
    ["cut_hay|cut_hemp|cut_reeds|bundle_thatch|pull_wild_flax", "it_sickle"],
    ["catch_fish|fish_|_fish$|fish_shoal", "it_fish"],
    ["salt_|smoke_meat|butcher|slaughter|kill_hen|_lard", "it_knife"],
    ["^watch_(boar|deer|hare|wolf)|snare|trap_", "it_snare"],
    ["fetch_water|draw_brine|fill_trough|boil_brine", "ic_bucket"],
    ["milk_", "it_jug"],
    ["feed_livestock|feed_hens|pet_|shoo_|shear_|graze_|tend_livestock", "ic_paw"],
    ["harvest_hive", "it_honey"],
    ["dig_|cut_peat|bury_|mine_", "ic_pick"],
    ["gather_|pick_|stock_herbs", "it_basket"],
    ["warm_|get_warm|light_candle", "ic_fire"],
    ["wash|comb_hair|swim", "ic_wash"],
    ["sweep|tidy", "it_broom"],
    ["burn_charcoal|burn_lime|collect_ash", "ic_fire"],
    ["twist_cord|ret_fiber", "it_rope"],
    ["tan_hides", "it_pelt"],
    ["whittle|carve", "it_toy"],
    ["brew", "it_ale"],
    ["deliver_to_site|claim_object|take_inventory|stock_food|^take_", "it_basket"],
    ["dry_rusks|set_leaven", "it_bread"],
    ["smoke_pipe", "it_pipe"],
    ["toast_health", "it_ale"],
    ["sing|lullaby|serenade|play_drum|play_flute|blow_whistle|recite_poem", "ic_note"],
    ["danc", "ic_dance"],
    ["dice|tafl|riddle|foot_race|wrestle|have_fun|^play", "it_dice"],
    ["embrace|intimacy|marry|marriage|divorce|swear_friendship", "ic_heart"],
    ["child|birth|_ward$|adopt", "ic_child"],
    ["practice_archery", "ic_bow"],
    ["practice_weapon|shove|raid|declare_war|keep_watch", "ic_shield"],
    ["appoint|council|execute|exile|judge|propose_law|punish|vote|prove_innocence|stand_surety|release_captive|accuse", "ic_law"],
    ["seize_power|swear_fealty|name_heir|recite_lineage|surrender", "ic_crown"],
    ["pickpocket|steal|extort", "ic_steal"],
    ["cast_|use_artifact|read_omens|whisper_to_charm", "ic_magic"],
    ["fast_for_gods|pilgrimage|stargaze", "ic_pray"],
    ["learn|teach|practice|observe|examine", "ic_book"],
    ["eavesdrop", "ic_eye"],
    ["boast|confide|curse|tell_stories|counsel|reveal|calm_down|socialize|share_news|news", "ic_talk"],
    ["give|help|ask_|invite|vouch|care|share|offer_trade", "ic_hand"],
    ["flee|shelter|idle", "ic_walk"],
    ["make_|build", "it_hammer"],
    ["tend_|weed", "it_wheat"],
]


def load(name):
    with open(os.path.join(SP, name), encoding="utf-8") as f:
        return json.load(f, object_pairs_hook=OrderedDict)


def save(name, data):   # craft2d.json's own layout (indent 1)
    with open(os.path.join(SP, name), "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
        f.write("\n")


def main():
    craft = load("craft2d.json")
    n_items = 0
    for k, v in ITEMS.items():
        if k not in craft["items"]:
            craft["items"][k] = v
            n_items += 1
    for k, v in GLYPH_ONLY.items():
        rec = craft["items"].get(k)
        if isinstance(rec, dict) and "sprite" not in rec:
            rec.update(v)
            n_items += 1
    # stations the workshop knew only by glyph
    for k, v in {"alembic": "it_flask", "crucible": "it_pot"}.items():
        craft["stations"].setdefault(k, v)
    save("craft2d.json", craft)

    # map2d.json is laid out by hand (one record a line): add lines, keep the rest as it is
    path = os.path.join(SP, "map2d.json")
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    mp = json.loads("\n".join(lines))
    at = next(i for i, l in enumerate(lines) if l.startswith('  "_default":'))
    add = [f'  {json.dumps(k)}: {json.dumps(v, ensure_ascii=False)},' for k, v in OBJECTS.items() if k not in mp["objects"]]
    lines[at:at] = add
    n_obj = len(add)
    a0 = next(i for i, l in enumerate(lines) if l.startswith(' "acts": ['))
    have = {a[0] for a in mp["acts"]}
    first = [a for a in ACTS_FIRST if a[0] not in have]
    lines[a0 + 1:a0 + 1] = [f'  {json.dumps(a, ensure_ascii=False)},' for a in first]
    end = next(i for i in range(a0, len(lines)) if lines[i] == " ]")
    have = {a[0] for a in mp["acts"]}
    new_acts = [a for a in ACTS if a[0] not in have]
    if new_acts:
        lines[end - 1] = lines[end - 1].rstrip(",") + ","
        lines[end:end] = [f'  {json.dumps(a, ensure_ascii=False)},' for a in new_acts]
        lines[end + len(new_acts) - 1] = lines[end + len(new_acts) - 1].rstrip(",")
    n_acts = len(new_acts) + len(first)
    text = "\n".join(lines)
    json.loads(text)   # still JSON
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"items +{n_items}, objects +{n_obj}, acts +{n_acts}")


if __name__ == "__main__":
    main()
