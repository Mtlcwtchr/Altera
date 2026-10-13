#!/usr/bin/env python3
"""import_megalith - a magic megalith made in Blender (doc/megaliths/) into the
terrain core's mesh format, so the lab's props renderer can stand it in the
world.

    python tools/terrain_lab/import_megalith.py                 # every content/config/terrain_core/megaliths/*.asset.json
    python tools/terrain_lab/import_megalith.py a1_monolith

Reads the asset manifest (written by the Blender script with --export) and
its glTF, and writes:

- assets/terrain_core/props/<id>_<kk>_lod{0,1,2}.mesh - FOLM v2, one model per
  variant, Z up in metres as authored (the glTF is Y up). Per vertex: uv.x =
  1 (the material darkens its own hollows), uv.y = height share up the model,
  blend = the `_EDGE` attribute (ledges and corners, where moss gathers). The
  three levels are the same mesh: a variant is 2-2.5 thousand triangles.
- their entries in assets/terrain_core/props/props.json, kind = <id>, so
  tools/blender/terrain_props.py --all leaves them alone;
- assets/terrain_core/megaliths/<dir>/engine/<texture>.png - the material's
  tiles at one size (the renderer loads them as one texture array), scaled up
  with hard pixels.

The material itself (projection, knobs) is read by the renderer straight from
the material manifest.

An asset whose manifest says "engine": "uv_atlas" (A-2, the carved stele)
wears baked maps on its own UVs instead: its vertices keep the glTF's UVs
(uv = atlas texcoord, blend = 0), its `debris` pieces become models
<id>_debris_<k> too, and engine/ gets the three layers the renderer loads as
one array at the maps' own size - basecolor.png (sRGB as baked),
normal.png (OpenGL tangent space) and surface.png (R = AO, G = roughness);
no alpha anywhere (a platform image loader may premultiply it).
"""
import argparse
import json
import struct
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
MANIFESTS = ROOT / "content" / "config" / "terrain_core" / "megaliths"
PROPS = ROOT / "assets" / "terrain_core" / "props"
TEXTURE_SIDE = 256
COMPONENTS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}
DTYPES = {5126: np.float32, 5123: np.uint16, 5125: np.uint32}


def read_gltf(path):
    g = json.loads(path.read_text(encoding="utf-8"))
    blob = (path.parent / g["buffers"][0]["uri"]).read_bytes()

    def accessor(i):
        a = g["accessors"][i]
        view = g["bufferViews"][a["bufferView"]]
        start = view.get("byteOffset", 0) + a.get("byteOffset", 0)
        n = COMPONENTS[a["type"]]
        data = np.frombuffer(blob, DTYPES[a["componentType"]], a["count"] * n, start)
        return data.reshape(a["count"], n) if n > 1 else data

    meshes = {}
    for node in g["nodes"]:
        if "mesh" not in node:
            continue
        prim = g["meshes"][node["mesh"]]["primitives"][0]
        at = prim["attributes"]
        meshes[node["name"]] = {
            "position": accessor(at["POSITION"]).astype(np.float64),
            "normal": accessor(at["NORMAL"]).astype(np.float64),
            "uv": accessor(at["TEXCOORD_0"]).astype(np.float64) if "TEXCOORD_0" in at else None,
            "edge": accessor(at["_EDGE"]).astype(np.float64) if "_EDGE" in at else None,
            "indices": accessor(prim["indices"]).astype(np.uint32),
        }
    return meshes


def y_up_to_z_up(v):
    """glTF (x, y up, z towards the viewer) -> Blender's (x, y, z up)."""
    return np.stack([v[:, 0], -v[:, 2], v[:, 1]], 1)


def write_mesh(path, position, normal, uv, blend, indices):
    lo, hi = position.min(0), position.max(0)
    vertices = np.concatenate([position, normal, uv, blend[:, None]], 1).astype(np.float32)
    with open(path, "wb") as f:
        f.write(b"FOLM")
        f.write(struct.pack("<3I", 2, len(vertices), len(indices)))
        f.write(struct.pack("<6f", *lo, *hi))
        f.write(vertices.tobytes())
        f.write(indices.astype(np.uint32).tobytes())
    return lo, hi


def import_atlas_asset(asset, manifest_path, catalogue):
    """A-2 and the like: own UVs, baked maps (see the module's doc)."""
    ident = asset["id"]
    meshes = read_gltf(ROOT / asset["model"])
    for name in [n for n, m in catalogue.items() if m["kind"] in (ident, ident + "_debris")]:
        del catalogue[name]
    models = [(f"{ident}_{k:02d}", ident, v) for k, v in enumerate(asset["variants"])]
    models += [(f"{ident}_debris_{k}", ident + "_debris", v) for k, v in enumerate(asset.get("debris", []))]
    for name, kind, entry in models:
        m = meshes[entry["node"]]
        pos = y_up_to_z_up(m["position"])
        nrm = y_up_to_z_up(m["normal"])
        uv = m["uv"]
        for level in range(3):
            lo, hi = write_mesh(PROPS / f"{name}_lod{level}.mesh", pos, nrm, uv, np.zeros(len(pos)), m["indices"])
        tris = len(m["indices"]) // 3
        catalogue[name] = {"kind": kind, "lo": [round(float(a), 2) for a in lo], "hi": [round(float(a), 2) for a in hi],
                           "triangles": [tris, tris, tris]}
        print(f"{name}: {tris} triangles, {lo.round(1)} .. {hi.round(1)}")
    material = json.loads((manifest_path.parent / asset["material"]).read_text(encoding="utf-8"))
    tex = material["textures"]
    out = ROOT / Path(asset["model"]).parent / "engine"
    out.mkdir(exist_ok=True)
    Image.open(ROOT / tex["basecolor"]["file"]).convert("RGB").save(out / "basecolor.png")
    Image.open(ROOT / tex["normal"]["file"]).convert("RGB").save(out / "normal.png")
    ao = np.asarray(Image.open(ROOT / tex["ao"]["file"]).convert("L"))
    rough = np.asarray(Image.open(ROOT / tex["roughness"]["file"]).convert("L"))
    Image.fromarray(np.dstack([ao, rough, np.zeros_like(ao)]), "RGB").save(out / "surface.png")
    print(f"{ident}: {len(models)} models, maps in {out.relative_to(ROOT)}")


def import_asset(manifest_path, catalogue):
    asset = json.loads(manifest_path.read_text(encoding="utf-8"))
    ident = asset["id"]
    if asset.get("engine") == "uv_atlas":
        return import_atlas_asset(asset, manifest_path, catalogue)
    meshes = read_gltf(ROOT / asset["model"])
    for name in [n for n, m in catalogue.items() if m["kind"] == ident]:
        del catalogue[name]
    for k, variant in enumerate(asset["variants"]):
        m = meshes[variant["node"]]
        pos = y_up_to_z_up(m["position"])
        nrm = y_up_to_z_up(m["normal"])
        top = max(pos[:, 2].max(), 1e-3)
        uv = np.stack([np.ones(len(pos)), np.clip(pos[:, 2] / top, 0, 1)], 1)
        edge = m["edge"] if m["edge"] is not None else np.zeros(len(pos))
        name = f"{ident}_{k:02d}"
        for level in range(3):
            lo, hi = write_mesh(PROPS / f"{name}_lod{level}.mesh", pos, nrm, uv, edge, m["indices"])
        tris = len(m["indices"]) // 3
        catalogue[name] = {"kind": ident, "lo": [round(float(a), 2) for a in lo], "hi": [round(float(a), 2) for a in hi],
                           "triangles": [tris, tris, tris]}
        print(f"{name}: {tris} triangles, {lo.round(1)} .. {hi.round(1)}")

    material = json.loads((manifest_path.parent / asset["material"]).read_text(encoding="utf-8"))
    out = ROOT / Path(asset["model"]).parent / "engine"
    out.mkdir(exist_ok=True)
    for key, tex in material["textures"].items():
        img = Image.open(ROOT / tex["file"]).convert("RGB")
        if img.size != (TEXTURE_SIDE, TEXTURE_SIDE):
            img = img.resize((TEXTURE_SIDE, TEXTURE_SIDE), Image.NEAREST)
        img.save(out / f"{key}.png")
    print(f"{ident}: {len(asset['variants'])} variants, textures in {out.relative_to(ROOT)}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ids", nargs="*")
    args = ap.parse_args()
    index = PROPS / "props.json"
    catalogue = json.loads(index.read_text())
    files = [MANIFESTS / f"{i}.asset.json" for i in args.ids] or sorted(MANIFESTS.glob("*.asset.json"))
    for f in files:
        import_asset(f, catalogue)
    index.write_text(json.dumps(catalogue, indent=1))


if __name__ == "__main__":
    main()
