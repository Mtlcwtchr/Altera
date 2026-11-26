# LR-167: regional vignettes for the review page: the new tiles, walls, roofs, decor, trees
# and vehicles put together at the world's scale (48 px = 1 m) the way the window draws them.
from PIL import Image

from export import to_rgba

T = 48


def img(e):
    px = to_rgba(e)
    h, w = len(px), len(px[0])
    im = Image.new("RGBA", (w, h))
    im.putdata([c for r in px for c in r])
    return im


class Scene:
    def __init__(self, E, w, h):
        self.E, self.w, self.h = E, w, h
        self.im = Image.new("RGBA", (w * T, h * T), (0, 0, 0, 255))
        self.later = []   # standing things sorted by their feet

    def ground(self, name, x0=0, y0=0, x1=None, y1=None):
        x1 = self.w if x1 is None else x1
        y1 = self.h if y1 is None else y1
        names = name if isinstance(name, (list, tuple)) else [name]
        for y in range(y0, y1):
            for x in range(x0, x1):
                n = names[(x * 7 + y * 13) % len(names)]
                self.im.alpha_composite(img(self.E[n]), (x * T, y * T))

    def tile(self, name, x, y):
        self.im.alpha_composite(img(self.E[name]), (int(x * T), int(y * T)))

    def stand(self, name, x, y, flip=False):
        """a standing thing with its feet at the bottom middle of cell (x, y)"""
        i = img(self.E[name])
        if flip:
            i = i.transpose(Image.FLIP_LEFT_RIGHT)
        self.later.append((y, x, i))

    def house(self, style, x0, y0, w, h, door=(1, "s"), windows=(), roof=None, cut=True):
        """walls on the cell lines around (x0..x0+w, y0..y0+h); the floor of the style inside;
        the roof over the north half (cut away over the south half to show the room)"""
        fl = {"hellenic": "tl_med_mosaic", "labyrinth": "tl_med_paving", "nilotic": "tl_nile_floor", "steppe": "tl_steppe_felt",
              "norse": "tl_north_floor", "dwarven": "tl_med_paving", "slavic": "tl_wood_floor", "celtic": "tl_floor", "sylvan": "tl_wood_moss"}.get(style, "tl_floor")
        self.ground(fl, x0, y0, x0 + w, y0 + h)
        for x in range(x0, x0 + w):
            for yy, side in ((y0, "n"), (y0 + h, "s")):
                k = "door_" if door and door[1] == side and x == x0 + door[0] else ("window_" if (x, side) in windows else "")
                self.tile("wl_%s_%sh" % (style, k), x, yy - 0.5)
        for y in range(y0, y0 + h):
            for xx, side in ((x0, "w"), (x0 + w, "e")):
                k = "window_" if (y, side) in windows else ""
                self.tile("wl_%s_%sv" % (style, k), xx - 0.5, y)
        for x, y in ((x0, y0), (x0 + w, y0), (x0, y0 + h), (x0 + w, y0 + h)):
            self.tile("wl_%s_post" % style, x - 0.5, y - 0.5)
        if roof:
            hh = h // 2 if cut else h
            for y in range(y0, y0 + hh):
                for x in range(x0, x0 + w):
                    self.tile(roof, x, y)

    def render(self):
        for y, x, i in sorted(self.later, key=lambda t: (t[0], t[1])):
            self.im.alpha_composite(i, (int(x * T + T / 2 - i.width / 2), int((y + 1) * T - i.height)))
        self.later = []
        return self.im


def med(E):
    s = Scene(E, 16, 10)
    s.ground(["tl_med_grass", "tl_med_grass_2", "tl_med_grass_3"])
    s.ground("tl_med_paving", 0, 6, 16, 7)
    s.ground("tl_med_sea", 0, 8, 16, 9)
    s.house("hellenic", 1, 1, 4, 4, door=(1, "s"), windows=((2, "e"), (3, "n")), roof="tl_med_roof")
    s.stand("dc_med_temple", 8.5, 4)
    s.stand("dc_med_altar", 9, 5)
    s.stand("ob_cypress", 6, 1); s.stand("ob_cypress", 13, 2); s.stand("ob_olive", 12, 5)
    s.stand("dc_med_statue", 6, 4); s.stand("dc_med_amphora", 5, 6); s.stand("dc_med_amphora", 5.6, 6.2)
    s.stand("dc_med_tripod", 11, 6)
    s.stand("tr_chariot", 2.5, 7)
    s.stand("ob_rock_sand", 13, 7)
    return s.render()


def nile(E):
    s = Scene(E, 16, 10)
    s.ground(["tl_nile_sand", "tl_nile_sand_2", "tl_nile_sand_3"])
    s.ground("tl_nile_silt", 0, 5, 16, 7)
    s.ground("tl_nile_river", 0, 7, 16, 9)
    s.house("nilotic", 1, 1, 4, 3, door=(2, "s"), windows=((1, "e"),), roof="tl_nile_roof")
    s.stand("dc_nile_naos", 9.5, 3)
    s.stand("dc_nile_obelisk", 7, 3)
    s.stand("dc_nile_sphinx", 12.5, 4)
    s.stand("ob_palm", 6, 5); s.stand("ob_palm", 13, 1); s.stand("ob_palm", 0, 5)
    s.stand("ob_papyrus", 3, 6); s.stand("ob_papyrus", 9, 6); s.stand("ob_papyrus", 11, 6)
    s.stand("tr_reedboat", 6.5, 8)
    s.stand("tr_camel", 3, 4)
    s.stand("ob_well_mud", 6, 2)
    return s.render()


def forest(E):
    s = Scene(E, 16, 10)
    s.ground(["tl_wood_meadow", "tl_wood_meadow_2", "tl_wood_meadow_3"])
    s.ground(["tl_forest", "tl_forest_2", "tl_forest_3"], 9, 0, 16, 9)
    s.ground("tl_road", 0, 6, 9, 7)
    s.house("slavic", 1, 1, 4, 4, door=(1, "s"), windows=((2, "e"),), roof="tl_wood_roof")
    s.house("celtic", 6, 1, 3, 3, door=(1, "s"), roof="tl_roof")
    s.stand("dc_wood_idol", 7, 5)
    s.stand("dc_wood_well_crane", 2, 7)
    s.stand("ob_tree", 10, 2); s.stand("ob_tree_birch", 12, 4); s.stand("ob_pine", 13, 1); s.stand("ob_sacred_tree", 11, 7)
    s.stand("ob_bush_berry", 9, 5); s.stand("ob_rock_moss", 13, 7)
    s.stand("dc_wood_stone", 9, 8)
    s.stand("tr_cart", 5.5, 8)
    s.stand("ob_firewood", 5, 5)
    return s.render()


def north(E):
    s = Scene(E, 16, 10)
    s.ground(["tl_north_tundra", "tl_north_tundra_2", "tl_north_tundra_3"])
    s.ground(["tl_north_snow", "tl_north_snow_2", "tl_north_snow_3"], 9, 0, 16, 4)
    s.ground("tl_north_sea", 0, 7, 16, 9)
    for x in range(9, 16):
        s.tile("cf_snow_n_%d" % (x % 2), x, 4)
    s.house("norse", 1, 1, 6, 3, door=(2, "s"), windows=((1, "e"),), roof="tl_north_roof")
    s.house("dwarven", 9, 0, 3, 2, door=(1, "s"), windows=((0, "e"),), roof=None)
    s.stand("dc_north_runestone", 8, 5)
    s.stand("dc_north_cairn", 12, 6)
    s.stand("ob_pine_snow", 13, 2); s.stand("ob_pine", 8, 2); s.stand("ob_dead_tree", 0, 5)
    s.stand("tr_longship", 6.5, 8.6)
    s.stand("tr_reindeer", 3, 5)
    s.stand("tr_sled", 5, 6)
    return s.render()


def steppe(E):
    s = Scene(E, 16, 10)
    s.ground(["tl_steppe_grass", "tl_steppe_grass_2", "tl_steppe_grass_3"])
    s.stand("dc_steppe_yurt", 2, 3)
    s.stand("dc_steppe_yurt", 5.5, 2)
    s.stand("dc_steppe_kurgan", 10.5, 3)
    s.stand("dc_steppe_balbal", 8, 6)
    s.stand("dc_steppe_totem", 13, 5)
    s.stand("dc_steppe_ovoo", 12, 8)
    s.stand("tr_kibitka", 3, 7)
    s.stand("tr_horse", 6, 6); s.stand("tr_pony", 8, 8, flip=True)
    s.stand("ob_bush", 0, 8)
    return s.render()


ALL = (("med", med), ("nile", nile), ("forest", forest), ("north", north), ("steppe", steppe))
