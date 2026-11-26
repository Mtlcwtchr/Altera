#!/usr/bin/env python3
"""LR-153: the look of the world map window (content/sprites_2d/worldmap2d.json).

Icons are 16x16 grids in the letters of sprites.json "pal" ('.' = clear); the
window draws them at the map's scale. Terrain: a base colour, a darker shade for
the dither and a small glyph (rows of '.', 'd' dark, 'l' light) repeated over
the land at a seed's hash. Run: python3 tools/content_gen/gen_worldmap2d.py
"""
import json, os

def pad(rows):
    rows = [r.ljust(16, '.')[:16] for r in rows]
    while len(rows) < 16:
        rows.append('.' * 16)
    return rows

S = {}
S['wm_hamlet'] = pad([
 '................',
 '................',
 '................',
 '................',
 '.......kk.......',
 '......kRRk......',
 '.....kRRRRk.....',
 '....kRRRRRRk....',
 '.....kttttk.....',
 '.....ktNktk.....',
 '.....ktNktk.....',
 '.....kkkkkk.....',
])
S['wm_village'] = pad([
 '................',
 '................',
 '................',
 '..........kk....',
 '...kk....kRRk...',
 '..kRRk..kRRRRk..',
 '.kRRRRk.kttttk..',
 'kRRRRRRkktNttk..',
 '.kttttk.kkkkkk..',
 '.ktNktk.........',
 '.ktNktk.........',
 '.kkkkkk.........',
])
S['wm_town'] = pad([
 '................',
 '.......kk.......',
 '......kyyk......',
 '......kWWk......',
 '..kk..kWWk..kk..',
 '.kRRk.kWWk.kRRk.',
 'kRRRRkkWWkkRRRRk',
 'kttttkWWWWkttttk',
 'ktNttkWkkWktNttk',
 'ktNttkWkkWktNttk',
 'kkkkkkkkkkkkkkkk',
])
S['wm_city'] = pad([
 '.......kk.......',
 '......kyyk......',
 '..k...kWWk...k..',
 '.kWk..kWWk..kWk.',
 'kWWWkkWWWWkkWWWk',
 'kWkWkWWWWWWkWkWk',
 'kWWWWWWkkWWWWWWk',
 'kWWWWWkNNkWWWWWk',
 'kWWWWWkNNkWWWWWk',
 'kkkkkkkkkkkkkkkk',
])
S['wm_keep'] = pad([
 '................',
 '....k.k.k.k.....',
 '....kWkWkWk.....',
 '....kWWWWWk.....',
 '....kWkkkWk.....',
 '.k.kkWWWWWkk.k..',
 '.kWkWWWWWWWkWk..',
 '.kWWWWWkWWWWWk..',
 '.kWWWWkNkWWWWk..',
 '.kWWWWkNkWWWWk..',
 '.kkkkkkkkkkkkk..',
])
S['wm_abbey'] = pad([
 '.......k........',
 '......kyk.......',
 '.......k........',
 '......kWk.......',
 '.....kWWWk......',
 '....kWWWWWkkkk..',
 '...kWWWWWWWRRRk.',
 '...kWWkyyWkRRRk.',
 '...kWWkyyWkWWWk.',
 '...kWWWWWWkWNWk.',
 '...kkkkkkkkkkkk.',
])
S['wm_camp'] = pad([
 '................',
 '................',
 '.......k........',
 '......kek.......',
 '.....keeek......',
 '....keeEeek.....',
 '...keeeEeeek....',
 '..keeekNkeeek...',
 '.keeeekNkeeeek..',
 '.kkkkkkkkkkkkk..',
 '......kfk.......',
 '.....kfFfk......',
 '......kkk.......',
])
S['wm_lair'] = pad([
 '................',
 '.......k........',
 '......kxk.......',
 '.....kxMxk......',
 '....kxMMMxk.....',
 '...kxMMxMMxk....',
 '..kxMMkkkMMxk...',
 '.kxMMkfFfkMMxk..',
 'kxMMMkfffkMMMxk.',
 'kkkkkkkkkkkkkkk.',
])
S['wm_hold'] = pad([
 '................',
 '.....k....k.....',
 '....kMk..kMk....',
 '...kMMMkkMMMk...',
 '..kMMMMMMMMMMk..',
 '.kMMMMkkkkMMMMk.',
 '.kMMMkWWWWkMMMk.',
 'kMMMMkWkkWkMMMMk',
 'kMMMMkWkkWkMMMMk',
 'kkkkkkkkkkkkkkkk',
])
S['wm_grove'] = pad([
 '................',
 '.....kkk........',
 '....kGlGk..kk...',
 '...kGGlGGkkGGk..',
 '...kGGGGGkGlGGk.',
 '....kGGGkkGGGGk.',
 '.....kNk..kGGk..',
 '..kk.kNk...kNk..',
 '.kUUk.kNk..kNk..',
 '.kUUk.kNk..kNk..',
 '.kkkkkkkkkkkkk..',
])
S['wm_ruins'] = pad([
 '................',
 '................',
 '...k......k.....',
 '..kWk....kWk....',
 '..kWk.k..kWk....',
 '..kWkkWk.kWkk...',
 '..kWWWWk.kWWWk..',
 '..kWkkWkkkWkWk..',
 '..kWk.kWWWWkWk..',
 '.kkkkkkkkkkkkkk.',
])
S['wm_tower'] = pad([
 '................',
 '......k.k.......',
 '......kWkk......',
 '......kWWk......',
 '.......kWk......',
 '......kWWk......',
 '......kWkWk.....',
 '......kWWWk.....',
 '.....kWWkWk.....',
 '.....kWWWWWk....',
 '....kkkkkkkk....',
])
S['wm_cave'] = pad([
 '................',
 '................',
 '.....kkkkk......',
 '....kMMMMMk.....',
 '...kMMMMMMMk....',
 '..kMMMkkkMMMk...',
 '..kMMkPPPkMMk...',
 '.kMMMkPPPkMMMk..',
 '.kMMkPPPPPkMMk..',
 'kkkkkkkkkkkkkkk.',
])
S['wm_mine'] = pad([
 '................',
 '.k.........k....',
 '.kNk.....kNk....',
 '..kNk...kNk.....',
 '...kNk.kNk......',
 '....kNkNk.......',
 '.....kNk........',
 '....kMkNk.......',
 '...kMk.kNk......',
 '..kMMk..kNk.....',
 '..kkk....kk.....',
])
S['wm_barrow'] = pad([
 '................',
 '................',
 '.......k........',
 '......kWk.......',
 '......kWk.......',
 '.....kkWkk......',
 '....kGGGGGk.....',
 '...kGGlGGGGk....',
 '..kGGGGGGlGGk...',
 '.kGGGlGGGGGGGk..',
 '.kkkkkkkkkkkkk..',
])
S['wm_stones'] = pad([
 '................',
 '................',
 '................',
 '..kk..kkkk..kk..',
 '.kWWk.kWWk.kWWk.',
 '.kWWk.kkkk.kWWk.',
 '.kWWk......kWWk.',
 '.kWWk.kk...kWWk.',
 '.kWWkkWWk..kWWk.',
 '.kkkkkkkkkkkkkk.',
])
S['wm_shrine'] = pad([
 '................',
 '......kk........',
 '.....kyyk.......',
 '......kk........',
 '.....kWWk.......',
 '....kWyyWk......',
 '....kWyyWk......',
 '....kWWWWk......',
 '...kMMMMMMk.....',
 '..kkkkkkkkkk....',
])
S['wm_beast'] = pad([
 '................',
 '................',
 '..k.......k.....',
 '..kk.....kk.....',
 '..kNk...kNk.....',
 '..kNNkkkNNk.....',
 '..kNyNNNyNk.....',
 '..kNNNNNNNk.....',
 '...kNwNwNk......',
 '....kNNNk.......',
 '.....kkk........',
])
S['wm_witch'] = pad([
 '................',
 '.......k........',
 '......kvk.......',
 '.....kvvvk......',
 '....kvvvvvk.....',
 '...kvvvvvvvk....',
 '..kkkkkkkkkkk...',
 '...keeekFeek....',
 '...keNekeeek....',
 '...keNekeeek....',
 '...kNkkkkkNk....',
 '...kNk...kNk....',
])
S['wm_battle'] = pad([
 '................',
 '.k..........k...',
 '.kmk......kmk...',
 '..kmk....kmk....',
 '...kmk..kmk.....',
 '....kmkkmk......',
 '.....kmmk.......',
 '....kmkkmk......',
 '...kNk..kNk.....',
 '..kNk....kNk....',
 '.kRk......kRk...',
 '.kk........kk...',
])
S['wm_oldbattle'] = pad([
 '................',
 '................',
 '.....kkkk.......',
 '....kwwwwk......',
 '...kwkwwkwk.....',
 '...kwwwwwwk.....',
 '....kwkkwk......',
 '.....kwwk.......',
 '.k...........k..',
 'kwk..kwk....kwk.',
 '.kwkkwk.kwkkwk..',
 '..kk.....kk.....',
])
S['wm_spring'] = pad([
 '................',
 '................',
 '......kk........',
 '.....kUUk.......',
 '....kUUUUk......',
 '....kUuuUk......',
 '...kMkUUkMk.....',
 '..kMMMkkMMMk....',
 '..kMuuuuuuMk....',
 '..kMMMMMMMMk....',
 '...kkkkkkkk.....',
])
S['wm_you'] = pad([
 '................',
 '.....kkkkk......',
 '....kfFFFfk.....',
 '...kfFkkkFfk....',
 '...kfFk.kFfk....',
 '...kfFkkkFfk....',
 '....kfFFFfk.....',
 '.....kfFfk......',
 '......kfk.......',
 '......kfk.......',
 '.......k........',
])

out = {
 "_doc": "LR-153: вид окна карты мира (tools/game_2d/native/worldmap2d_view.cpp). Сгенерировано tools/content_gen/gen_worldmap2d.py, править там. sprites — значки 16x16 в буквах sprites.json pal; terrain — цвет, тень и узор клетки; settlement_icons — правила значка поселения (первое совпадение); poi_icons — значок по виду места; domain_colours — цвета держав по порядку.",
 "sprites": S,
 "terrain": {
  "sea":       {"c": "#22324a", "d": "#1c2a40", "glyph": ["....", ".l..", "....", "...l"]},
  "lake":      {"c": "#2c4462", "d": "#243a56", "glyph": ["....", "..l.", "....", "...."]},
  "plains":    {"c": "#9a9356", "d": "#8a8448", "glyph": ["....", ".d..", "....", "..d."]},
  "forest":    {"c": "#3f5f30", "d": "#2c4622", "glyph": [".l..", "lddl", ".d..", "...."]},
  "hills":     {"c": "#8a7650", "d": "#6e5c3c", "glyph": ["....", ".ll.", "d..d", "...."]},
  "mountains": {"c": "#6e6660", "d": "#4e4844", "glyph": [".l..", "ldl.", "dddd", "...."]},
  "marsh":     {"c": "#4f5e46", "d": "#3e4a38", "glyph": ["....", "l.l.", ".dd.", "...."]}
 },
 "river": "#3c6c9c",
 "road": "#c8b078",
 "war": "#c03828",
 "tribute": "#d8b048",
 "fog_edge": "#141010",
 "settlement_icons": [
  {"inst": "lair", "icon": "wm_lair"},
  {"tag": "outlaw", "icon": "wm_camp"},
  {"tag": "cult", "icon": "wm_abbey"},
  {"culture": "dwarven", "icon": "wm_hold"},
  {"culture": "sylvan", "icon": "wm_grove"},
  {"size": "city", "icon": "wm_city"},
  {"tag": "fortified", "icon": "wm_keep"},
  {"size": "town", "icon": "wm_town"},
  {"size": "hamlet", "icon": "wm_hamlet"},
  {"icon": "wm_village"}
 ],
 "poi_icons": {
  "ruins": "wm_ruins", "watchtower": "wm_tower", "cave": "wm_cave", "mine": "wm_mine", "barrow": "wm_barrow",
  "standing_stones": "wm_stones", "shrine": "wm_shrine", "beast_lair": "wm_beast", "witch_hut": "wm_witch",
  "old_battlefield": "wm_oldbattle", "spring": "wm_spring", "battlefield": "wm_battle"
 },
 "domain_colours": ["#d8b048", "#a83c30", "#4a78b8", "#5a9a48", "#8a5ab0", "#c87830", "#48a0a0", "#b05a80", "#909090", "#706040"],
 "reach_km": {"hamlet": 5, "village": 7, "town": 11, "city": 15},
 "zooms": [1.0, 1.6, 2.5, 4.0]
}
p = os.path.join(os.path.dirname(__file__), '..', '..', 'content', 'sprites_2d', 'worldmap2d.json')
with open(p, 'w', encoding='utf-8') as f:
    json.dump(out, f, ensure_ascii=False, indent=1)
print('wrote', os.path.normpath(p))
