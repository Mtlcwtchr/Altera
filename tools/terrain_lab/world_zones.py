"""Where things go: the continent's shape is read, covered with patches of natural
outline, and the patches are given their biomes by rules of neighbourhood and place
(the desert lies on the peninsula the sea washes from most sides, the stone steppe
follows it, the plain follows the steppe, the range stands at the plain's far edge, the
marshes lie on the west-east side opposite the desert, the forest between the marshes and
the range, the black lands take the rest).

    zone, info = place_zones(land, rules, n, seed, rs, fbm)
"""
import heapq
import numpy as np
from scipy import ndimage
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import dijkstra
from scipy.spatial import cKDTree


def make_patches(land, world_km, rs, seed, fbm, patch_km2):
    """Noisy-Voronoi patches over the land: labels (-1 off the land), seed points."""
    n = land.shape[0]
    km_px = world_km / n
    ys, xs = np.nonzero(land)
    rmin = 0.62 * np.sqrt(patch_km2) / km_px
    order = rs.permutation(len(ys))
    pts = []
    for i in order[:60000]:
        p = (ys[i], xs[i])
        if all((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 >= rmin * rmin for q in pts):
            pts.append(p)
    pts = np.array(pts, np.float32)
    # the plane bends before the nearest seed is looked for: the borders wander
    diam_km = 2 * np.sqrt(patch_km2 / np.pi)
    base = max(3, int(round(world_km / diam_km)))
    amp = 0.38 * diam_km / 2 / km_px
    wy = ys + amp * fbm(n, seed + 501, octaves=3, base=base)[ys, xs] + 0.3 * amp * fbm(n, seed + 503, octaves=3, base=base * 3)[ys, xs]
    wx = xs + amp * fbm(n, seed + 502, octaves=3, base=base)[ys, xs] + 0.3 * amp * fbm(n, seed + 504, octaves=3, base=base * 3)[ys, xs]
    _, nearest = cKDTree(pts).query(np.c_[wy, wx])
    labels = np.full((n, n), -1, np.int32)
    labels[ys, xs] = nearest
    return labels, pts


def washed(land, cy, cx, km_px, radii_km=(10.0, 22.0), rays=16):
    """How much of the way round a point the sea is near: 0..1."""
    n = land.shape[0]
    ang = np.linspace(0, 2 * np.pi, rays, endpoint=False)
    score = 0.0
    for r_km in radii_km:
        steps = np.linspace(2.0, r_km / km_px, 14)
        yy = np.clip((cy + np.outer(np.sin(ang), steps)).astype(int), 0, n - 1)
        xx = np.clip((cx + np.outer(np.cos(ang), steps)).astype(int), 0, n - 1)
        hit = (~land[yy, xx]).any(axis=1)
        score += hit.mean() / len(radii_km)
    return score


def place_zones(land, rules, n, seed, rs, fbm):
    world_km = rules["world_km"]
    km_px = world_km / n
    cfg = rules["placement"]
    shares = cfg["shares"]
    labels, pts = make_patches(land, world_km, rs, seed, fbm, cfg["patch_km2"])
    P = len(pts)
    flat = labels.ravel()
    area = np.bincount(flat[flat >= 0], minlength=P).astype(np.float64)
    total = area.sum()
    idx = np.indices((n, n))
    cen = np.c_[ndimage.sum(idx[0], labels, range(P)) / np.maximum(area, 1),
                ndimage.sum(idx[1], labels, range(P)) / np.maximum(area, 1)]
    # neighbours
    pairs = set()
    for a, b in ((labels[:, :-1], labels[:, 1:]), (labels[:-1, :], labels[1:, :])):
        m = (a != b) & (a >= 0) & (b >= 0)
        for u, v in set(zip(a[m].tolist(), b[m].tolist())):
            pairs.add((min(u, v), max(u, v)))
    adj = [[] for _ in range(P)]
    rows, cols, wts = [], [], []
    for u, v in pairs:
        adj[u].append(v)
        adj[v].append(u)
        d = float(np.hypot(*(cen[u] - cen[v]))) * km_px
        rows += [u, v]
        cols += [v, u]
        wts += [d, d]
    graph = coo_matrix((wts, (rows, cols)), shape=(P, P)).tocsr()

    expo = np.array([washed(land, cen[p][0], cen[p][1], km_px) for p in range(P)])
    ymin, ymax = cen[:, 0].min(), cen[:, 0].max()
    south = (cen[:, 0] - ymin) / max(ymax - ymin, 1.0)
    land_c = np.array([(cen[:, 0] * area).sum() / total, (cen[:, 1] * area).sum() / total])

    # 1. The desert: the patch the sea washes from the most sides (the narrowest
    # peninsula), the south preferred; it takes its washed neighbours until it
    # has its share.
    first = int(np.argmax(expo - 0.5 * south))
    # (it takes the patches nearest to that one over the land, so the whole tip is desert)
    near_first = dijkstra(graph, indices=first)
    desert, got = set(), 0.0
    for p in np.argsort(near_first):
        if got >= shares["golden_desert"] * total:
            break
        desert.add(int(p))
        got += area[p]
    dsel = sorted(desert)
    dc = (cen[dsel] * area[dsel][:, None]).sum(axis=0) / area[dsel].sum()

    # 2. All the rest in order of how far it lies from the desert over the land.
    g = dijkstra(graph, indices=dsel, min_only=True)
    g[~np.isfinite(g)] = g[np.isfinite(g)].max() + 1.0
    name_of = {p: "golden_desert" for p in desert}
    band_order = [("canyon_steppe", shares["canyon_steppe"]), ("green_plateau", shares["green_plateau"]),
                  ("white_mountains", shares["white_mountains"])]
    # The desert passes into nothing but the stone steppe: whatever land touches it
    # is the canyon belt that wraps it (the sea washes the rest).
    cum = 0.0
    for p in sorted(desert):
        for q in adj[p]:
            if q not in desert and q not in name_of:
                name_of[q] = "canyon_steppe"
                cum += area[q]
    k = 0
    edge = band_order[0][1] * total
    # The order is how far a patch lies from the desert, over the land and (by
    # `axis_weight`) along the north axis, so the belts run across the continent.
    wgt = cfg.get("axis_weight", 0.6)
    along = np.maximum(cen[:, 0] - dc[0], 0.0) * km_px
    key = (1.0 - wgt) * g + wgt * along * (g.max() / max(along.max(), 1.0))
    for p in np.argsort(key):
        p = int(p)
        if p in name_of:
            continue
        while k < len(band_order) and cum >= edge:
            k += 1
            if k < len(band_order):
                edge += band_order[k][1] * total
        name_of[p] = band_order[k][0] if k < len(band_order) else "black_lands"
        cum += area[p]

    # 3. The marshes: on the side west-east opposite the desert, on the plain.
    side = -1.0 if dc[1] > land_c[1] else 1.0
    spanx = max(float(np.ptp(cen[:, 1])), 1.0)
    lateral = side * (cen[:, 1] - land_c[1]) / spanx        # > 0: the marsh side
    plain = [p for p in range(P) if name_of.get(p) == "green_plateau"]
    plain.sort(key=lambda p: -(lateral[p] + 0.3 * expo[p]))
    marsh, got = set(), 0.0
    for p in plain:
        if got >= shares["great_marshes"] * total:
            break
        if lateral[p] > 0.05:
            marsh.add(p)
            got += area[p]
    for p in marsh:
        name_of[p] = "great_marshes"
    # 4. The dark forest: from the marshes up the plain and to the range's foot.
    forest, got = set(), 0.0
    ok = lambda q: name_of.get(q) == "green_plateau" and lateral[q] > 0.0
    heap = []
    for p in marsh:
        for q in adj[p]:
            if ok(q):
                heapq.heappush(heap, (-lateral[q], q))
    seen = set()
    while heap and got < shares["dark_forest"] * total:
        _, q = heapq.heappop(heap)
        if q in seen or not ok(q):
            continue
        seen.add(q)
        forest.add(q)
        got += area[q]
        for r_ in adj[q]:
            if r_ not in seen and ok(r_):
                heapq.heappush(heap, (-lateral[r_], r_))
    for p in forest:
        name_of[p] = "dark_forest"

    zone = np.full((n, n), "ocean", dtype=object)
    names = np.array([name_of.get(p, "black_lands") for p in range(P)], dtype=object)
    inland = labels >= 0
    zone[inland] = names[labels[inland]]

    # 5. The river valley: where the white range and the black peaks come within a
    # few km of each other - no trench of its own, the slopes meet there.
    white = zone == "white_mountains"
    black = zone == "black_lands"
    if white.any() and black.any():
        vw = cfg.get("valley_km", 5.0)
        dw = ndimage.distance_transform_edt(~white) * km_px
        db = ndimage.distance_transform_edt(~black) * km_px
        zone[inland & (dw < vw) & (db < vw) & (white | black)] = "river_valley"
    return zone, {"patches": P, "desert_patches": len(desert), "marsh_side": side}


def import_zones(path, palette, land, n):
    """A coloured biome picture (north up) read as the zones: every land pixel takes
    the zone whose palette colour is nearest to the picture's colour there (river and
    lake blue count as the ground round them). -> zone array like place_zones."""
    from PIL import Image
    Image.MAX_IMAGE_PIXELS = None
    img = Image.open(path).convert("RGB").resize((n, n), Image.NEAREST)
    rgb = np.asarray(img, np.float32)[::-1]
    names = list(palette)
    cols = np.array([palette[k] for k in names], np.float32)
    water = np.array(palette.get("_water", [30, 110, 240]), np.float32)
    d = np.stack([np.linalg.norm(rgb - c, axis=2) for c in cols], axis=2)
    pick = np.argmin(d, axis=2)
    wet = np.linalg.norm(rgb - water, axis=2) < 60
    # (the edge pixels between colours are blends: they count as wet, too)
    wet = ndimage.binary_dilation(wet, iterations=2)
    zone = np.full((n, n), "ocean", dtype=object)
    arr = np.array(names, dtype=object)
    zone[land] = arr[pick[land]]
    # Blue lines and lakes, and the blends where one colour meets another or the black
    # of the sea (anti-aliasing: a dark blend would pass for the black lands or the
    # forest): the zone of the nearest ground whose colour is a true one.
    true_colour = d.min(axis=2) < 28.0
    dry = land & ~wet & true_colour
    _, near = ndimage.distance_transform_edt(~dry, return_indices=True)
    zone = np.where(land & ~dry, zone[near[0], near[1]], zone)
    # land the picture leaves unpainted (black): the nearest painted zone
    return zone
