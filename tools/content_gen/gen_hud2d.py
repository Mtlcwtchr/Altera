#!/usr/bin/env python3
"""LR-137: the 2D window's pixel HUD: 9x9 icons and what they stand for.

Writes content/sprites_2d/hud2d.json:
  "sprites"  - hud_<name> grids in the sprites.json palette ('.' empty), 9x9;
  "vitals"   - health / strength / blood / mood -> icon;
  "attrs"    - the 8 attributes (expansion/attributes.json) -> icon, short name key;
  "tiers"    - the icons of the needs, by tier (the fallback grouping; LR-140: the sim
               sends its own tiers in the card, the window uses those); the need tiers (gdd/motivation tiers: body, safety, belonging, esteem,
               purpose, spirit) -> the need ids in each and their icons. A need the
               sim does not have yet (the derived ones) is skipped by the window, so
               the tier shows what the sim sends;
  "moodlets" - [regex on the moodlet id, icon]: the first match draws a thought.
Idempotent: rerun after a change here.  python tools/content_gen/gen_hud2d.py
"""
import json
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PATH = os.path.join(ROOT, "content", "sprites_2d", "hud2d.json")

# letters here -> the sprites.json palette
TO_PAL = {"o": "k", "r": "r", "R": "R", "y": "y", "Y": "T", "G": "G", "D": "D", "u": "u", "U": "B",
          "s": "w", "S": "W", "m": "n", "n": "o", "c": "U", "g": "H", "k": "N", "p": "t", "l": "l", "v": "v"}

ICONS = {
    "heart": [".........", ".rr...rr.", "rrrr.rrrr", "rrrrrrrrr", "rrrrrrrrr", ".rrrrrrR.", "..rrrrR..", "...rrR...", "....R...."],
    "bolt": [".....yy..", "....yy...", "...yy....", "..yyyyy..", "....yy...", "...yy....", "..yy.....", ".yy......", "........."],
    "drop": ["....r....", "....r....", "...rrr...", "...rrr...", "..rrrrr..", "..rrrrr..", "..rrrRr..", "...rRr...", "........."],
    "face": ["..ggggg..", ".g.....g.", "g..g.g..g", "g.......g", "g.g...g.g", "g..ggg..g", ".g.....g.", "..ggggg..", "........."],
    "frown": ["..ggggg..", ".g.....g.", "g..g.g..g", "g.......g", "g..ggg..g", "g.g...g.g", ".g.....g.", "..ggggg..", "........."],
    "bread": [".........", "..mmmmm..", ".mnnnnnm.", "mnnnnnnnm", "mnnnnnnnm", "mmmmmmmmm", ".........", ".........", "........."],
    "cup": [".........", ".sssssss.", ".s.uuu.s.", ".suuuuus.", ".suuuuus.", "..suuus..", "...sss...", "..sssss..", "........."],
    "moon": ["...yyy...", "..yy.....", ".yy......", ".yy......", ".yy......", ".yy......", "..yy.....", "...yyy...", "........."],
    "flame": ["....n....", "...nn....", "...nnn.n.", "..nnynnn.", "..nyyynn.", ".nnyyynn.", ".nnyyyn..", "..nnnn...", "........."],
    "soap": [".........", "..c...c..", ".c.c.c.c.", "..c...c..", ".sssssss.", ".sSSSSSs.", ".sssssss.", ".........", "........."],
    "people": ["..s...s..", ".sss.sss.", "..s...s..", ".sss.sss.", "sssssssss", "sssssssss", ".........", ".........", "........."],
    "lute": ["......m..", ".....m.m.", "....m....", "..mmm....", ".mmnmm...", ".mnnnm...", ".mmnmm...", "..mmm....", "........."],
    "star": ["....y....", "....y....", "yyyyyyyyy", ".yyyyyyy.", "..yyyyy..", "..yy.yy..", ".yy...yy.", ".........", "........."],
    "house": ["....m....", "...mmm...", "..mmmmm..", ".mmmmmmm.", "..sssss..", "..s.S.s..", "..s.S.s..", "..sssss..", "........."],
    "sack": ["...mmm...", "....m....", "..nnnnn..", ".nnnnnnn.", ".nnnnnnn.", ".nnnnnnn.", "..nnnnn..", ".........", "........."],
    "ring": [".........", "...yyy...", "..y...y..", ".y.....y.", ".y.....y.", ".y.....y.", "..y...y..", "...yyy...", "........."],
    "hand": ["...s.s...", ".s.s.s...", ".s.s.s.s.", ".sssssss.", ".sssssss.", ".ssssss..", "..ssss...", "..ssss...", "........."],
    "crown": [".........", "y...y...y", "yy.yyy.yy", "yyyyyyyyy", "yyyyyyyyy", "yrryyyrry", "yyyyyyyyy", ".........", "........."],
    "tree": ["...GGG...", "..GGGGG..", ".GGGGGGG.", ".GGGGGGG.", "..GGGGG..", "....m....", "....m....", "...mmm...", "........."],
    "sun": ["....y....", ".y..y..y.", "..yyyyy..", "..yyyyy..", "yyyyyyyyy", "..yyyyy..", "..yyyyy..", ".y..y..y.", "....y...."],
    "fist": [".........", "..ssss...", ".sSsSss..", ".ssssss..", ".sssssss.", "..sssss..", "..sssss..", ".........", "........."],
    "feather": ["......cc.", ".....ccc.", "....ccc..", "...ccc...", "..ccc....", "..cc.....", ".s.......", "s........", "........."],
    "book": [".........", ".UUUUUUU.", ".UpppppU.", ".UpppppU.", ".UpppppU.", ".UpppppU.", ".UUUUUUU.", ".........", "........."],
    "eye": [".........", ".........", "..sssss..", ".ssuuuss.", "ssuoouuss", ".ssuuuss.", "..sssss..", ".........", "........."],
    "will": ["....g....", "...ggg...", "..ggggg..", "...ggg...", "...ggg...", "...ggg...", "...ggg...", "...ggg...", "........."],
    "mouth": [".........", ".sssssss.", "sssssssss", "ss.....ss", "sssssssss", ".sssssss.", "..s......", ".s.......", "........."],
    "endur": [".........", "..rr.rr..", ".rrrrrrr.", ".rrrrrrr.", "..rrrrr..", "...rrr...", "....r....", ".........", "........."],
    "pin": ["...rrr...", "..rrrrr..", "..rrsrr..", "..rrrrr..", "...rrr...", "....r....", "....r....", ".........", "........."],
    "plus": [".........", "....G....", "....G....", "..GGGGG..", "....G....", "....G....", ".........", ".........", "........."],
    "minus": [".........", ".........", ".........", "..rrrrr..", ".........", ".........", ".........", ".........", "........."],
    "wound": [".........", ".r.....r.", "..r...r..", "...r.r...", "....r....", "...r.r...", "..r...r..", ".r.....r.", "........."],
    "skill": [".........", ".....ss..", "....sSs..", "...sSs...", "..mSs....", ".mmm.....", ".mm......", ".........", "........."],
    "bag": [".........", "...mmm...", "..m...m..", ".nnnnnnn.", ".nnmmmnn.", ".nnnnnnn.", ".nnnnnnn.", "..nnnnn..", "........."],
    "plan": [".........", ".sssssss.", ".s.....s.", ".s.ooo.s.", ".s.....s.", ".s.ooo.s.", ".s.....s.", ".sssssss.", "........."],
    "heartbreak": [".........", ".rr...rr.", "rrrr.rrrr", "rrr.rrrrr", "rrrr.rrrr", ".rr.rrrR.", "..rr.rR..", "...rrR...", "....R...."],
    "skull": [".........", "..sssss..", ".sssssss.", ".so.s.os.", ".sssssss.", "..sssss..", "..s.s.s..", ".........", "........."],
    "zzz": [".........", ".uuuu....", "...u.....", "..u......", ".uuuu....", "....uuu..", ".....u...", "....uuu..", "........."],
    "note": [".........", "....yyyy.", "....y..y.", "....y..y.", "....y..y.", "..yyy.yy.", ".yyyy.yy.", "..yy.....", "........."],
    "snow": ["....c....", ".c..c..c.", "..c.c.c..", "...ccc...", "ccccccccc", "...ccc...", "..c.c.c..", ".c..c..c.", "....c...."],
    "coin": [".........", "...yyy...", "..yYYYy..", ".yYyyyYy.", ".yYyyyYy.", ".yYyyyYy.", "..yYYYy..", "...yyy...", "........."],
    "kiss": [".........", ".........", ".rr...rr.", "rrrr.rrrr", ".rrrrrrr.", "..rrrrr..", "...rrr...", ".........", "........."],
    "hammer": [".........", ".SSSSS...", ".SSSSS...", "...m.....", "...m.....", "...m.....", "...m.....", "...m.....", "........."],
}


def grid(rows):
    out = []
    for r in rows:
        assert len(r) == 9, r
        out.append("".join("." if ch == "." else TO_PAL[ch] for ch in r))
    return out


def main():
    data = {
        "_doc": "LR-137: the pixel HUD of the 2D window. Generated by tools/content_gen/gen_hud2d.py, do not edit by hand.",
        "sprites": {"hud_" + k: grid(v) for k, v in ICONS.items()},
        "vitals": {"health": "hud_heart", "strength": "hud_bolt", "blood": "hud_drop", "mood": "hud_face", "mood_low": "hud_frown"},
        "attrs": [
            {"id": "strength", "icon": "hud_fist"}, {"id": "agility", "icon": "hud_feather"},
            {"id": "endurance", "icon": "hud_endur"}, {"id": "intellect", "icon": "hud_book"},
            {"id": "perception", "icon": "hud_eye"}, {"id": "willpower", "icon": "hud_will"},
            {"id": "charisma", "icon": "hud_mouth"}, {"id": "dexterity", "icon": "hud_hand"},
        ],
        "tiers": [
            {"id": "body", "needs": [["hunger", "hud_bread"], ["thirst", "hud_cup"], ["energy", "hud_moon"],
                                     ["warmth", "hud_flame"], ["hygiene", "hud_soap"]]},
            {"id": "safety", "needs": [["safety", "hud_house"], ["provision", "hud_sack"]]},
            {"id": "belonging", "needs": [["social", "hud_people"], ["kinship", "hud_ring"], ["acceptance", "hud_hand"]]},
            {"id": "esteem", "needs": [["standing", "hud_crown"], ["fun", "hud_lute"]]},
            {"id": "purpose", "needs": [["purpose", "hud_star"], ["legacy", "hud_tree"]]},
            {"id": "spirit", "needs": [["grace", "hud_sun"]]},
        ],
        "other_need": "hud_star",
        "moodlets": [
            ["ate|meal|bread|feast|food|catch", "hud_bread"], ["hungry|starving|fasted", "hud_bread"],
            ["thirst|drink|tipsy|hungover|merry", "hud_cup"], ["rest|slept|sleep|exhaust|refresh", "hud_moon"],
            ["warm|cozy", "hud_flame"], ["cold|freez", "hud_snow"], ["clean|tidy", "hud_soap"],
            ["chat|company|confid|comfort|lonely|homesick", "hud_people"], ["pray|bless|omen|grace|peace|soothed|calm", "hud_sun"],
            ["kiss|love|wedding|flatter", "hud_kiss"], ["reject|grief|griev|heart", "hud_heartbreak"],
            ["injur|maul|stung|sick|ill_|fever|wound|sore", "hud_wound"], ["dead|cursed|dread|dark", "hud_skull"],
            ["sang|danc|fun|amus|game|contest|star|amazed|moved", "hud_note"], ["made|work|new_clothes|proud", "hud_hammer"],
            ["home", "hud_house"], ["help|begged|refused", "hud_hand"], ["guilty|caught|insult|scold|embarrass|irritat|alarm|fright", "hud_frown"],
        ],
        "moodlet_good": "hud_plus",
        "moodlet_bad": "hud_minus",
    }
    with open(PATH, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
        f.write("\n")
    print("wrote", PATH, len(data["sprites"]), "icons")


if __name__ == "__main__":
    main()
