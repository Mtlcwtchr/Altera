"""AL-11: dev-only Skyrim textures for the build kit, the ground and the sky.

    python world_tex.py [--data <Skyrim SE Data>] [--repo D:/AlteraGame/altera]

Writes (all git-ignored, read in place of ours when present, so deleting them
brings our own art back):
  assets/altera_game/buildings/buildings_sky.astm   our kit's geometry with Skyrim's
                                                     farmhouse/landscape textures per layer
  assets/terrain_core/materials/skyrim/<name>.png    every ground material from a Skyrim
                                                     landscape texture, colour-matched to ours
                                                     so the biome palette stays
  assets/terrain_core/sky/skyrim/day.png, night.png  panoramas from Skyrim's sky textures:
                                                     upper and horizon clouds on a day gradient,
                                                     stars, the galaxy, Masser and Secunda at night
Every entry is a stand-in (replace_before_prod), listed in
assets/altera_game/props/registry_world.json.
"""
import argparse, io, json, os, struct, sys
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bsa import Library, SEP

# buildings.astm layer (bake_buildings.py order) -> Skyrim texture
KIT = {0: 'architecture/farmhouse/stonewall01.dds', 1: 'architecture/farmhouse/woodwalkway01.dds',
       2: 'landscape/dirt02.dds', 3: 'architecture/farmhouse/farmwindowinterior01.dds',
       5: 'architecture/farmhouse/woodwall01.dds', 6: 'architecture/farmhouse/thatch02.dds',
       7: 'architecture/farmhouse/woodpost02.dds', 8: 'architecture/farmhouse/woodwall01.dds',
       9: 'architecture/farmhouse/woodwalkway01.dds', 10: 'landscape/dirt01.dds',
       11: 'architecture/farmhouse/roof01.dds'}

GROUND = {
    'ash_black': 'volcanictundradirt01', 'crust_ochre': 'volcanictundraminerals01', 'dirt_brown': 'dirt01',
    'dirt_dark': 'dirt02', 'dirt_mud': 'rivermud', 'dirt_pebbles': 'dirtpath01', 'dirt_red': 'reachdirt01',
    'earth_ochre': 'fallforestdirt01', 'earth_orange': 'fallforestdirt01', 'earth_yellow': 'fallforestdirt01',
    'grass_burnt': 'tundra02', 'grass_dark': 'forestgrass02', 'grass_dry': 'tundra01', 'grass_flowers': 'fieldgrass02',
    'grass_gold': 'fieldgrass01', 'grass_leaves': 'fallforestleaves01', 'grass_lush': 'forestgrass01',
    'grass_meadow': 'fielddirtgrass01', 'grass_olive': 'reachgrass01', 'grass_stony': 'rocksgrass01',
    'gravel_black': 'volcanictundragravel01', 'gravel_grey': 'riverbottom', 'gravel_pale': 'coastbeach02',
    'rock_dark': 'rocks01', 'rock_grey': 'mountainslab01', 'rock_mossy': 'reachmossyrocks01',
    'rock_ochre': 'fallforestrocks01', 'rock_orange': 'tundrarocks01', 'rock_pale': 'mountainslab02',
    'rock_red_hatch': 'volcanictundrarocks01', 'rock_red_terrace': 'mineralpoolterrace', 'rocky_dark': 'rocks01',
    'rocky_grey': 'mountainslab02', 'rocky_meadow': 'rocksgrass01', 'rocky_slabs': 'mountainslab01',
    'sand_beach': 'coastbeach01', 'sand_dune': 'coastbeach02', 'sand_gold': 'coastbeach01',
    'sand_grey': 'riverbededge', 'scree_gravel': 'volcanictundragravel01', 'scree_slabs': 'mountainslab02',
    'snow': 'snow01landscape',
}


def load(lib, path):
    p = path if path.startswith('textures') else 'textures/' + path
    d = lib.read(p.replace('/', SEP))
    if d is None:   # some live one folder deeper (landscape/mountains/...)
        tail = SEP + p.split('/')[-1].lower()
        hit = next((k for k in lib.index if k.startswith('textures') and k.endswith(tail)), None)
        d = lib.read(hit) if hit else None
    if d is None:
        print('  MISSING', path)
        return None
    im = Image.open(io.BytesIO(d))
    im.load()
    return np.asarray(im.convert('RGBA'), np.float32) / 255.0


def fit(img, size):
    return np.asarray(Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8)).resize(
        (size, size), Image.LANCZOS), np.float32) / 255.0


def kit(lib, repo, report):
    src = os.path.join(repo, 'assets/altera_game/buildings/buildings.astm')
    b = bytearray(open(src, 'rb').read())
    v, nm, nv, ni, nl, ls = struct.unpack_from('<6I', b, 4)
    off = 28 + nm * 64 + nv * 36 + ni * 4
    for L, tex in KIT.items():
        if L >= nl:
            continue
        img = load(lib, tex)
        if img is None:
            continue
        a = fit(img, ls)
        a[:, :, 3] = 1
        at = off + L * ls * ls * 4
        b[at:at + ls * ls * 4] = (a * 255 + 0.5).astype(np.uint8).tobytes()
        report.append({'kit_layer': L, 'texture': tex, 'replace_before_prod': True})
    out = os.path.join(repo, 'assets/altera_game/buildings/buildings_sky.astm')
    open(out, 'wb').write(bytes(b))
    print('wrote', out)


def ground(lib, repo, report):
    src = os.path.join(repo, 'assets/terrain_core/materials')
    out = os.path.join(src, 'skyrim')
    os.makedirs(out, exist_ok=True)
    for name in sorted(os.listdir(src)):
        if not name.endswith('.png'):
            continue
        key = name[:-4]
        sky = GROUND.get(key, 'dirt01')
        ours = Image.open(os.path.join(src, name)).convert('RGB')
        size = ours.size[0]
        mean = np.asarray(ours, np.float32).reshape(-1, 3).mean(0) / 255.0
        img = load(lib, 'landscape/' + sky + '.dds')
        if img is None:
            continue
        a = fit(img, size)[:, :, :3]
        # keep our palette: scale the Skyrim detail to our material's mean colour
        a = a * (mean / np.maximum(a.reshape(-1, 3).mean(0), 1e-3))
        Image.fromarray((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8)).save(os.path.join(out, name))
        report.append({'material': key, 'texture': 'landscape/' + sky + '.dds', 'replace_before_prod': True})
    # the renderer reads the surface maps beside the colour (materials/pbr): ours stay
    import shutil
    if os.path.isdir(os.path.join(src, 'pbr')):
        shutil.copytree(os.path.join(src, 'pbr'), os.path.join(out, 'pbr'), dirs_exist_ok=True)
    print('wrote', out)


def sample(tex, u, v):
    h, w = tex.shape[:2]
    x = (np.floor(u * w).astype(np.int64)) % w
    y = (np.floor(v * h).astype(np.int64)) % h
    return tex[y, x]


def sky(lib, repo, report):
    out = os.path.join(repo, 'assets/terrain_core/sky/skyrim')
    os.makedirs(out, exist_ok=True)
    W, H = 2048, 1024
    u = (np.arange(W) + 0.5) / W
    v = (np.arange(H) + 0.5) / H
    U, V = np.meshgrid(u, v)
    az = (U - 0.5) * 2 * np.pi
    th = V * np.pi
    d = np.stack([np.sin(th) * np.cos(az), np.sin(th) * np.sin(az), np.cos(th)], -1)
    z = d[..., 2]
    up = np.clip(z, 0, 1)
    # the dome: clouds on a plane above, read where the ray meets it
    k = 1.0 / np.maximum(z, 0.04)
    pu, pv = d[..., 0] * k * 0.35, d[..., 1] * k * 0.35
    fade = np.clip((z - 0.02) / 0.25, 0, 1)

    # ---- day
    zen = np.array([0.24, 0.42, 0.70])
    hor = np.array([0.72, 0.78, 0.84])
    gnd = np.array([0.42, 0.44, 0.45])
    t = up[..., None] ** 0.55
    day = hor * (1 - t) + zen * t
    day = np.where(z[..., None] < 0, hor * 0.7 + gnd * 0.3, day)
    for name, scale, amount in (('skyrimcloudsupper01', 1.0, 0.85), ('skyrimcloudslower01', 0.6, 0.7)):
        c = load(lib, 'sky/' + name + '.dds')
        if c is None:
            continue
        s = sample(c, pu * scale, pv * scale)
        a = (s[..., 3] if s[..., 3].std() > 0.01 else s[..., :3].mean(-1)) * fade * amount
        col = 0.75 + 0.25 * s[..., :3].mean(-1, keepdims=True)
        day = day * (1 - a[..., None]) + col * a[..., None]
        report.append({'sky': 'day', 'texture': 'sky/' + name + '.dds', 'replace_before_prod': True})
    hz = load(lib, 'sky/skyrimcloudshorizon01.dds')
    if hz is not None:
        band = np.clip(1 - np.abs(z - 0.06) / 0.12, 0, 1)
        s = sample(hz, U * 4, 0.5 - (z - 0.06) / 0.24)
        a = (s[..., 3] if s[..., 3].std() > 0.01 else s[..., :3].mean(-1)) * band * 0.8
        day = day * (1 - a[..., None]) + 0.9 * a[..., None]
        report.append({'sky': 'day', 'texture': 'sky/skyrimcloudshorizon01.dds', 'replace_before_prod': True})

    # ---- night
    nz = np.array([0.015, 0.02, 0.05])
    nh = np.array([0.05, 0.07, 0.11])
    night = nh * (1 - t) + nz * t
    night = np.where(z[..., None] < 0, nh * 0.6, night)
    stars = load(lib, 'sky/skystars.dds')
    if stars is not None:
        s = sample(stars, U * 6, V * 3)[..., :3]
        lum = s.mean(-1, keepdims=True)
        pts = np.clip((lum - np.quantile(lum, 0.995)) * 8, 0, 1)   # only the brightest specks
        night = night + (s * 0.05 + pts) * np.clip(z, 0, 1)[..., None] ** 0.3
        report.append({'sky': 'night', 'texture': 'sky/skystars.dds', 'replace_before_prod': True})
    gal = load(lib, 'sky/skyrimgalaxy.dds')
    if gal is not None:
        # a band across the sky, tilted
        tilt = d[..., 0] * 0.6 + d[..., 2] * 0.8
        band = np.clip(1 - np.abs(tilt) / 0.35, 0, 1)
        s = sample(gal, U * 2, 0.5 + tilt)[..., :3]
        night = night + s * band[..., None] * 0.12 * (z > 0)[..., None]
        report.append({'sky': 'night', 'texture': 'sky/skyrimgalaxy.dds', 'replace_before_prod': True})
    for name, dirv, size in (('masser_full', (0.5, 0.3, 0.55), 0.22), ('secunda_full', (-0.4, 0.5, 0.45), 0.08)):
        m = load(lib, 'sky/' + name + '.dds')
        if m is None:
            continue
        c = np.array(dirv, np.float64)
        c /= np.linalg.norm(c)
        e1 = np.cross(c, [0, 0, 1])
        e1 /= np.linalg.norm(e1)
        e2 = np.cross(e1, c)
        dc = (d * c).sum(-1)
        mu = (d * e1).sum(-1) / np.maximum(dc, 1e-3) / size + 0.5
        mv = -(d * e2).sum(-1) / np.maximum(dc, 1e-3) / size + 0.5
        inside = (dc > 0) & (mu > 0) & (mu < 1) & (mv > 0) & (mv < 1)
        s = sample(m, np.clip(mu, 0, 0.999), np.clip(mv, 0, 0.999))
        a = s[..., 3] * inside
        night = night * (1 - a[..., None]) + s[..., :3] * a[..., None]
        report.append({'sky': 'night', 'texture': 'sky/' + name + '.dds', 'replace_before_prod': True})
    for nm_, img in (('day.png', day), ('night.png', night)):
        Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8)).save(os.path.join(out, nm_))
    print('wrote', out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default='D:/Steam/steamapps/common/Skyrim Special Edition/Data')
    ap.add_argument('--repo', default='D:/AlteraGame/altera')
    a = ap.parse_args()
    lib = Library(a.data)
    report = {'kit': [], 'ground': [], 'sky': []}
    kit(lib, a.repo, report['kit'])
    ground(lib, a.repo, report['ground'])
    sky(lib, a.repo, report['sky'])
    json.dump({'source': 'Skyrim Special Edition (Bethesda), Data/*.bsa', 'replace_before_prod': True,
               'world': report},
              open(os.path.join(a.repo, 'assets/altera_game/props/registry_world.json'), 'w', encoding='utf-8'),
              ensure_ascii=False, indent=1)


if __name__ == '__main__':
    main()
