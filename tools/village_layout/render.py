"""Top-down SVG of a planned village (AL-84) from a world config (its
village_site) and a save of that world at day 0. Usage:
python render.py WORLD.json SAVE.json OUT.svg [title]"""
import json, math, sys

COL = {"farmer": "#c9a66b", "smith": "#8a5a44", "innkeeper": "#b5523b", "baker": "#d08a3c", "carpenter": "#9c7a3c",
       "weaver": "#a08fbf", "tanner": "#6f7a3a", "potter": "#b8744f", "chapel": "#e8e2d0", "granary": "#d9c37a"}
ZCOL = {"field": "#d8cf8a", "cemetery": "#8f9a8a", "pasture": "#a9cf8a", "woodlot": "#5d8a55", "spare": "none",
        "green": "#b9d79a"}


GUESS = [("shrine", "chapel"), ("brewing_vat", "innkeeper"), ("forge", "smith"), ("tanning_pit", "tanner"),
         ("potter_wheel", "potter"), ("loom_frame", "weaver"), ("oven", "baker"), ("workbench", "carpenter"),
         ("quern", "granary"), ("house", "farmer")]


def guess(types):
    for t, r in GUESS:
        if t in types: return r
    return "granary" if types else ""


def items(x):
    return x.values() if isinstance(x, dict) else x


def main(world, save, out, title=""):
    cfg = json.load(open(world))
    sv = json.load(open(save))
    site = cfg["village_site"]
    n, c, x0, y0 = site["n"], site["cell"], site["x0"], site["y0"]
    sts = list(items(sv.get("settlements", [])))
    st = sts[0] if sts else {}
    S = 2.0  # px per metre
    half = 230.0  # metres round the village's centre
    cx0, cy0 = st.get("square") or st.get("center") or [x0 + (n - 1) * c / 2] * 2
    W = 2 * half * S
    tx = lambda x: (x - cx0 + half) * S
    ty = lambda y: (y - cy0 + half) * S
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W:.0f} {W:.0f}" width="{W:.0f}" height="{W:.0f}">']
    # ground: heights as bands, wood, water (cells of 8 m)
    h = site["h_dm"]
    lo, hi = min(h), max(h)
    step = 2
    for j in range(0, n - 1, step):
        for i in range(0, n - 1, step):
            k = j * n + i
            if not (-20 < tx(x0 + i * c) < W + 20 and -20 < ty(y0 + j * c) < W + 20): continue
            t = (h[k] - lo) / max(1, hi - lo)
            band = math.floor(t * 10) / 10
            g = int(200 - band * 70)
            col = f"rgb({g - 25},{g},{g - 45})"
            if site.get("wood") and site["wood"][k]: col = f"rgb({g - 110},{g - 60},{g - 110})"
            if site["wet"][k]: col = "#6c95b8"
            o.append(f'<rect x="{tx(x0 + i * c):.1f}" y="{ty(y0 + j * c):.1f}" width="{c * step * S + .5:.1f}" height="{c * step * S + .5:.1f}" fill="{col}"/>')
    for z in st.get("zones", []):
        r = z["rect"]  # [cx, cy, yaw, hw, hd]
        cx, cy, yaw, hw, hd = r
        fill = ZCOL.get(z["kind"], "#ccc")
        dash = ' stroke-dasharray="4 3"' if z["kind"] == "spare" else ""
        o.append(f'<rect x="{-hw * S:.1f}" y="{-hd * S:.1f}" width="{2 * hw * S:.1f}" height="{2 * hd * S:.1f}" fill="{fill}" fill-opacity="0.85" stroke="#4a4030" stroke-width="0.6"{dash} transform="translate({tx(cx):.1f},{ty(cy):.1f}) rotate({math.degrees(yaw):.1f})"/>')
    if st.get("square"):
        sx, sy = st["square"]
        o.append(f'<circle cx="{tx(sx):.1f}" cy="{ty(sy):.1f}" r="{st.get("square_r", 12) * S:.1f}" fill="#cbb68f" stroke="#6b5a3a"/>')
    for line, wd in zip(st.get("roads", []), st.get("road_width", [])):
        pts = " ".join(f"{tx(p[0]):.1f},{ty(p[1]):.1f}" for p in line)
        col = "#a68a5c" if wd >= 2 else "#8c7a55"
        o.append(f'<polyline points="{pts}" fill="none" stroke="{col}" stroke-width="{wd * S:.1f}" stroke-linecap="round" stroke-linejoin="round"/>')
    cell = 2.0
    objs = {ob["id"]: ob for ob in items(sv.get("objects", []))}
    for s in st.get("structures", []):
        q, gx, gy = s["frame"]
        i0, j0, i1, j1 = s["cells"]

        def pt(u, v):
            for _ in range(q % 4): u, v = -v, u
            return tx(u + gx * cell), ty(v + gy * cell)
        poly = [pt(i0 * cell, j0 * cell), pt(i1 * cell, j0 * cell), pt(i1 * cell, j1 * cell), pt(i0 * cell, j1 * cell)]
        rec = s.get("recipe", "") or guess([objs[i]["type"] for i in s.get("objects", []) if i in objs])
        o.append('<polygon points="' + " ".join(f"{a:.1f},{b:.1f}" for a, b in poly) +
                 f'" fill="{COL.get(rec, "#c9a66b")}" stroke="#3a2a1a" stroke-width="0.8"/>')
        d = s.get("door")
        if d: o.append(f'<circle cx="{tx(d[0]):.1f}" cy="{ty(d[1]):.1f}" r="1.2" fill="#222"/>')
        for p in s.get("props", []):
            px, py = p["pos"]
            o.append(f'<circle cx="{tx(px):.1f}" cy="{ty(py):.1f}" r="0.6" fill="#5a4630"/>')
        if rec:
            cx = sum(a for a, _ in poly) / 4
            cy = sum(b for _, b in poly) / 4
            o.append(f'<text x="{cx:.1f}" y="{cy + 2:.1f}" font-size="9" text-anchor="middle" font-family="sans-serif" fill="#111">{rec}</text>')
    for ob in objs.values():
        t = ob.get("type")
        if t in ("well", "village_fire", "shrine", "field", "mill", "grave"):
            p = ob.get("pos")
            if not p: continue
            sym = {"well": "#2f6fa8", "village_fire": "#e0602a", "shrine": "#fff", "field": "#7a6a2a", "mill": "#555", "grave": "#444"}[t]
            o.append(f'<circle cx="{tx(p[0]):.1f}" cy="{ty(p[1]):.1f}" r="3" fill="{sym}" stroke="#111" stroke-width="0.4"/>')
    e = st.get("entrance")
    if e and st.get("road_kind"):
        o.append(f'<text x="{tx(e[0]):.1f}" y="{ty(e[1]) - 6:.1f}" font-size="12" text-anchor="middle" font-family="sans-serif" fill="#7a1d0d">въезд</text>')
    if title:
        o.append(f'<text x="8" y="18" font-size="14" font-family="sans-serif" fill="#111">{title}</text>')
    o.append("</svg>")
    open(out, "w").write("\n".join(o))


if __name__ == "__main__":
    main(*sys.argv[1:5])
