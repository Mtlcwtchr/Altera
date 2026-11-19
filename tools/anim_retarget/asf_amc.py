"""CMU ASF/AMC reader for Blender: builds an armature with one action, resampled.

The ASF rest pose (all dofs zero) becomes the armature rest. For every bone the
local motion is L = C * Rdof * C^-1 (C from the ASF `axis`, Euler XYZ applied as
Rz*Ry*Rx, like the CMU reference parsers), and the root also carries translation.
CMU is Y-up in inches/0.45; the armature is built Z-up in metres.

Usage from another Blender script:
    import asf_amc; arm = asf_amc.build(asf_path, amc_path, src_fps=120, out_fps=30)
"""
import math

import bpy
from bpy_extras import anim_utils
from mathutils import Euler, Matrix, Vector

# CMU (x, y, z) Y-up -> Blender (x, -z, y) Z-up
A = Matrix(((1, 0, 0), (0, 0, -1), (0, 1, 0)))


def _euler_xyz(rx, ry, rz):
    # Rz * Ry * Rx in degrees (static XYZ), as in the CMU amc viewers.
    return Euler((math.radians(rx), math.radians(ry), math.radians(rz)), "XYZ").to_matrix()


def parse_asf(path):
    bones, hier, root = {}, {}, {}
    unit_len = 1.0
    sec, cur = None, None
    with open(path, encoding="latin-1") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith(":"):
                parts = line.split()
                sec = parts[0][1:]
                continue
            p = line.split()
            if sec == "units" and p[0] == "length":
                unit_len = float(p[1])
            elif sec == "root":
                if p[0] == "order":
                    root["order"] = [x.lower() for x in p[1:]]
                elif p[0] == "position":
                    root["position"] = Vector(map(float, p[1:4]))
                elif p[0] == "orientation":
                    root["orientation"] = [float(x) for x in p[1:4]]
            elif sec == "bonedata":
                if p[0] == "begin":
                    cur = {"dof": []}
                elif p[0] == "end":
                    bones[cur["name"]] = cur
                elif p[0] == "name":
                    cur["name"] = p[1]
                elif p[0] == "direction":
                    cur["direction"] = Vector(map(float, p[1:4]))
                elif p[0] == "length":
                    cur["length"] = float(p[1])
                elif p[0] == "axis":
                    cur["axis"] = [float(x) for x in p[1:4]]
                elif p[0] == "dof":
                    cur["dof"] = [x.lower() for x in p[1:]]
            elif sec == "hierarchy":
                if p[0] in ("begin", "end"):
                    continue
                for c in p[1:]:
                    hier[c] = p[0]
    # inches/0.45 units -> metres
    scale = (1.0 / unit_len) * 0.0254
    return bones, hier, root, scale


def parse_amc(path):
    frames, cur = [], None
    with open(path, encoding="latin-1") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#") or line.startswith(":"):
                continue
            p = line.split()
            if len(p) == 1 and p[0].isdigit():
                cur = {}
                frames.append(cur)
            elif cur is not None:
                cur[p[0]] = [float(x) for x in p[1:]]
    return frames


def _order(bones, hier):
    out, seen = [], set()

    def visit(n):
        if n in seen:
            return
        par = hier.get(n)
        if par and par != "root":
            visit(par)
        seen.add(n)
        out.append(n)
    for n in bones:
        visit(n)
    return out


def build(asf_path, amc_path, src_fps=120, out_fps=30, name="CMU"):
    bones, hier, root, scale = parse_asf(asf_path)
    frames = parse_amc(amc_path)
    order = _order(bones, hier)
    C, Ci = {}, {}
    for n, b in bones.items():
        C[n] = _euler_xyz(*b.get("axis", (0, 0, 0)))
        Ci[n] = C[n].inverted()
    Croot = _euler_xyz(*root.get("orientation", (0, 0, 0)))
    Croot_i = Croot.inverted()

    # rest positions (CMU space), all dofs zero
    start, end = {}, {}
    rpos = root.get("position", Vector()) * scale
    for n in order:
        par = hier.get(n, "root")
        s = rpos if par == "root" else end[par]
        start[n] = s
        end[n] = s + bones[n]["direction"].normalized() * bones[n]["length"] * scale

    arm_data = bpy.data.armatures.new(name)
    arm = bpy.data.objects.new(name, arm_data)
    bpy.context.scene.collection.objects.link(arm)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode="EDIT")
    eb = arm_data.edit_bones
    r = eb.new("root")
    r.head = A @ rpos
    r.tail = A @ (rpos + Vector((0, 0.1, 0)))  # up in CMU space
    for n in order:
        e = eb.new(n)
        e.head = A @ start[n]
        tail = A @ end[n]
        if (tail - e.head).length < 1e-3:
            tail = e.head + Vector((0, 0, 0.01))
        e.tail = tail
        e.use_connect = False
        e.parent = eb[hier.get(n, "root")] if hier.get(n, "root") != "root" else r
    bpy.ops.object.mode_set(mode="OBJECT")

    rest = {b.name: b.matrix_local.to_3x3() for b in arm_data.bones}
    rest_i = {k: v.inverted() for k, v in rest.items()}
    step = max(1, int(round(src_fps / out_fps)))
    sel = frames[::step]
    act = bpy.data.actions.new(f"{name}|amc")
    arm.animation_data_create()
    arm.animation_data.action = act
    slot = arm.animation_data.action_slot or act.slots.new(id_type="OBJECT", name=arm.name)
    arm.animation_data.action_slot = slot
    cb = anim_utils.action_ensure_channelbag_for_slot(act, slot)
    rot_keys = {n: [] for n in ["root"] + order}
    loc_keys = []
    rorder = root.get("order", ["tx", "ty", "tz", "rx", "ry", "rz"])
    for i, fr in enumerate(sel):
        vals = dict(zip(rorder, fr.get("root", [0] * 6)))
        T = Vector((vals.get("tx", 0), vals.get("ty", 0), vals.get("tz", 0))) * scale
        Lroot = Croot @ _euler_xyz(vals.get("rx", 0), vals.get("ry", 0), vals.get("rz", 0)) @ Croot_i
        G_root = A @ Lroot @ A.transposed()
        q = (rest_i["root"] @ G_root @ rest["root"]).to_quaternion()
        rot_keys["root"].append(q)
        # root bone head moves from rest rpos to T (positions are absolute in AMC)
        loc_keys.append(rest_i["root"] @ (A @ (T - rpos) if root.get("position") is not None else A @ T))
        for n in order:
            d = dict(zip(bones[n]["dof"], fr.get(n, [])))
            L = C[n] @ _euler_xyz(d.get("rx", 0), d.get("ry", 0), d.get("rz", 0)) @ Ci[n]
            Lb = A @ L @ A.transposed()
            q = (rest_i[n] @ Lb @ rest[n]).to_quaternion()
            rot_keys[n].append(q)

    def write(path, idx, group, vals):
        fc = cb.fcurves.ensure(path, index=idx, group_name=group)
        fc.keyframe_points.add(len(vals))
        fc.keyframe_points.foreach_set("co", [v for k, x in enumerate(vals) for v in (k + 1, x)])
        fc.update()
    for n, qs in rot_keys.items():
        arm.pose.bones[n].rotation_mode = "QUATERNION"
        for k in range(1, len(qs)):
            if qs[k].dot(qs[k - 1]) < 0:
                qs[k].negate()
        for i in range(4):
            write(f'pose.bones["{n}"].rotation_quaternion', i, n, [q[i] for q in qs])
    for i in range(3):
        write('pose.bones["root"].location', i, "root", [v[i] for v in loc_keys])
    bpy.context.scene.render.fps = out_fps
    bpy.context.scene.render.fps_base = 1.0
    return arm
