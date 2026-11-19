"""Batch retarget onto the Quaternius UAL rig and export one GLB per clip.

blender -b --factory-startup -P retarget_ual.py -- --template ual_rig.blend --jobs jobs.json
        --results results.jsonl [--only id1,id2] [--skip-existing] [--shard i/n]

jobs.json: [{"id", "family": mixamo|iglesias|motifect|cmu|ual, "src", "out",
             "asf" (cmu), "src_fps" (cmu), "action" (ual), "frames": [a, b] (optional)}]

How a bone is retargeted (no constraints, no rest-pose editing):
  * the source is turned about Z so that its hips' left-right axis matches ours;
  * each mapped bone takes the source bone's world rotation change from rest,
    pre-corrected by the rotation that brings the source's rest limb direction
    onto ours (so T-pose vs slightly lowered arms does not leak into the clip);
  * the pelvis also takes the source hips' travel, scaled by leg length
    (hips height above the ankles);
  * the target `root` bone stays still: root motion is left in the pelvis.
Output: 30 fps, frames from 0, quaternion keys on every frame.
"""
import json
import math
import os
import sys
import time
import traceback

import bpy
from bpy_extras import anim_utils
from mathutils import Matrix, Quaternion, Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bone_maps  # noqa: E402

OUT_FPS = 30
TARGET = "UAL"
HIPS = "pelvis"


def _rot(m):
    return m.to_3x3().normalized().to_quaternion()


def _args():
    a = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    out = {"skip_existing": False, "only": None}
    i = 0
    while i < len(a):
        k = a[i]
        if k == "--skip-existing":
            out["skip_existing"] = True
            i += 1
            continue
        out[k.lstrip("-").replace("-", "_")] = a[i + 1]
        i += 2
    return out


def make_template(ual_glb, path):
    """Armature-only copy of the UAL rig (object named UAL), saved as a .blend."""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.context.scene.render.fps = OUT_FPS
    bpy.ops.import_scene.gltf(filepath=ual_glb)
    arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
    for o in list(bpy.data.objects):
        if o is not arm:
            bpy.data.objects.remove(o)
    arm.animation_data_clear()
    for a in list(bpy.data.actions):
        bpy.data.actions.remove(a)
    arm.name = TARGET
    for pb in arm.pose.bones:
        pb.rotation_mode = "QUATERNION"
        pb.rotation_quaternion = (1, 0, 0, 0)
        pb.location = (0, 0, 0)
    bpy.ops.wm.save_as_mainfile(filepath=path)


# ---------------------------------------------------------------- import

def import_source(job):
    fam, src = job["family"], job["src"]
    before = set(bpy.data.objects)
    if fam == "cmu":
        import asf_amc
        arm = asf_amc.build(job["asf"], src, src_fps=job.get("src_fps", 120), out_fps=OUT_FPS, name="SRC")
        return arm, OUT_FPS
    if src.lower().endswith(".bvh"):
        bpy.ops.import_anim.bvh(filepath=src, global_scale=job.get("scale", 0.01), rotate_mode="NATIVE",
                                update_scene_fps=True, update_scene_duration=True)
    else:
        bpy.ops.import_scene.fbx(filepath=src, automatic_bone_orientation=False, ignore_leaf_bones=False)
    new = [o for o in bpy.data.objects if o not in before]
    arm = next(o for o in new if o.type == "ARMATURE")
    for o in new:
        if o.type == "MESH":
            bpy.data.objects.remove(o)
    fps = bpy.context.scene.render.fps / bpy.context.scene.render.fps_base
    return arm, fps


# ---------------------------------------------------------------- retarget

def _yaw_align(src, tgt, bmap, fix):
    """Rotation about Z that turns the source hips' left->right axis onto ours."""
    def lat(arm, l, r, m):
        return (arm.matrix_world @ m @ arm.data.bones[l].head_local) - (arm.matrix_world @ m @ arm.data.bones[r].head_local)
    sv = lat(src, bmap["thigh_l"], bmap["thigh_r"], fix)
    tv = lat(tgt, "thigh_l", "thigh_r", Matrix.Identity(4))
    sv.z = tv.z = 0
    ang = math.atan2(sv.x * tv.y - sv.y * tv.x, sv.x * tv.x + sv.y * tv.y)
    return Matrix.Rotation(ang, 4, "Z")


def retarget(src, tgt, family, src_fps, frames=None, clip_name="clip"):
    bmap = {t: s for t, s in bone_maps.MAPS[family].items() if t in tgt.data.bones and s in src.data.bones}
    act_src = src.animation_data.action if src.animation_data else None
    if frames:
        f0, f1 = float(frames[0]), float(frames[1])
    else:
        f0, f1 = act_src.frame_range
    sc = bpy.context.scene
    # Some sources keep a still top "root" bone whose pose differs from its rest
    # (Iglesias Spellcasting: rest lying on its back at 1/100 scale, root posed up and
    # x100). The root pose at the first frame is folded into the source rest.
    fix = Matrix.Identity(4)
    root = bone_maps.ROOT.get(family)
    if root and root in src.pose.bones:
        sc.frame_set(int(f0))
        fix = src.pose.bones[root].matrix @ src.data.bones[root].matrix_local.inverted()
    RL = lambda n: fix @ src.data.bones[n].matrix_local  # noqa: E731
    RH = lambda n: fix @ src.data.bones[n].head_local  # noqa: E731
    Rz = _yaw_align(src, tgt, bmap, fix)
    SW = lambda: Rz @ src.matrix_world  # noqa: E731
    tw = tgt.matrix_world
    tw_rot = _rot(tw)
    order = sorted(tgt.data.bones, key=lambda b: len(b.parent_recursive))
    rest_rel = {b.name: (b.parent.matrix_local.inverted() @ b.matrix_local if b.parent else b.matrix_local.copy())
                for b in order}
    t_rest_w = {b: _rot(tw @ tgt.data.bones[b].matrix_local) for b in bmap}
    s_rest_w = {s: _rot(SW() @ RL(s)) for s in set(bmap.values())}

    # rest direction correction for limbs
    corr = {}
    for b, aim in bone_maps.AIM.items():
        if b in bmap and aim in bmap and bmap[aim] != bmap[b]:
            sd = (SW() @ RH(bmap[aim])) - (SW() @ RH(bmap[b]))
            td = (tw @ tgt.data.bones[aim].head_local) - (tw @ tgt.data.bones[b].head_local)
            if sd.length > 1e-5 and td.length > 1e-5:
                corr[b] = sd.normalized().rotation_difference(td.normalized())
    # hips travel scale: hips height above the ankles
    def ank(arm, w, names):
        return min((w @ arm.data.bones[n].head_local).z for n in names)
    s_h = (SW() @ RH(bmap[HIPS]))
    t_h = (tw @ tgt.data.bones[HIPS].head_local)
    s_leg = s_h.z - min((SW() @ RH(bmap[n])).z for n in ("foot_l", "foot_r"))
    t_leg = t_h.z - ank(tgt, tw, ["foot_l", "foot_r"])
    ratio = t_leg / s_leg if s_leg > 1e-4 else 1.0

    n_out = int(math.floor((f1 - f0) * OUT_FPS / src_fps + 1e-6)) + 1
    # Ground: the rest pose of some sources (BVH) is not placed on the floor, so the
    # floor is taken as the lowest ankle over the clip; our ankles keep their rest height.
    s_ank_min = 1e9
    for k in range(n_out):
        f = f0 + k * src_fps / OUT_FPS
        fi = int(math.floor(f))
        sc.frame_set(fi, subframe=f - fi)
        m = SW()
        for n in (bmap["foot_l"], bmap["foot_r"]):
            s_ank_min = min(s_ank_min, (m @ src.pose.bones[n].head).z)
    t_ank = ank(tgt, tw, ["foot_l", "foot_r"])
    keys = {b: [] for b in bmap}
    locs = []
    err = 0.0
    hips_path = []
    dir_err_sum, dir_err_max = [0.0, 0], [0.0, "", 0]
    foot_min = 1e9
    for k in range(n_out):
        f = f0 + k * src_fps / OUT_FPS
        fi = int(math.floor(f))
        sc.frame_set(fi, subframe=f - fi)
        sw_mat = SW()
        pose = {}
        for b in order:
            mp = pose.get(b.parent.name, Matrix.Identity(4)) if b.parent else Matrix.Identity(4)
            base = mp @ rest_rel[b.name]
            if b.name not in bmap:
                pose[b.name] = base
                continue
            s = bmap[b.name]
            sw = _rot(sw_mat @ src.pose.bones[s].matrix)
            delta = sw @ s_rest_w[s].inverted()
            want = delta @ corr.get(b.name, Quaternion()).inverted() @ t_rest_w[b.name]
            lq = _rot(base).inverted() @ (tw_rot.inverted() @ want)
            prev = keys[b.name][-1] if keys[b.name] else None
            if prev is not None and lq.dot(prev) < 0:
                lq.negate()
            loc = Vector()
            if b.name == HIPS:
                hs = sw_mat @ src.pose.bones[s].head
                want_w = t_h + (hs - s_h) * ratio
                want_w.z = t_ank + (hs.z - s_ank_min) * ratio
                want_a = tw.inverted() @ want_w
                loc = base.to_3x3().inverted() @ (want_a - base.translation)
                locs.append(loc.copy())
                hips_path.append(want_w.copy())
            pose[b.name] = base @ Matrix.Translation(loc) @ lq.to_matrix().to_4x4()
            keys[b.name].append(lq)
            got = tw_rot @ _rot(pose[b.name])
            a = (got @ want.inverted()).angle
            err = max(err, math.degrees(min(a, 2 * math.pi - a)))
        for n in ("foot_l", "foot_r"):
            foot_min = min(foot_min, (tw @ pose[n].translation).z)
        # independent check: limb directions (joint to joint) source vs target, world space
        for b in corr:
            aim = bone_maps.AIM[b]
            if aim not in pose:
                continue
            td = tw @ pose[aim].translation - tw @ pose[b].translation
            sd = sw_mat @ src.pose.bones[bmap[aim]].head - sw_mat @ src.pose.bones[bmap[b]].head
            if td.length > 1e-5 and sd.length > 1e-5:
                ang = math.degrees(td.angle(sd))
                dir_err_sum[0] += ang
                dir_err_sum[1] += 1
                if ang > dir_err_max[0]:
                    dir_err_max[:] = [ang, b, k]

    # Put the lowest ankle back at its rest height (proportions differ by a few cm).
    ground_dz = t_ank - foot_min
    if locs and abs(ground_dz) < 0.12:
        base = rest_rel[HIPS] if not tgt.data.bones[HIPS].parent else (
            rest_rel[tgt.data.bones[HIPS].parent.name] @ rest_rel[HIPS])
        off = base.to_3x3().inverted() @ (tw.to_3x3().inverted() @ Vector((0, 0, ground_dz)))
        locs = [v + off for v in locs]
        foot_min += ground_dz
        hips_path = [p + Vector((0, 0, ground_dz)) for p in hips_path]
    else:
        ground_dz = 0.0 if not locs else ground_dz

    act = bpy.data.actions.new(clip_name)
    tgt.animation_data_create()
    tgt.animation_data.action = act
    slot = tgt.animation_data.action_slot or act.slots.new(id_type="OBJECT", name=tgt.name)
    tgt.animation_data.action_slot = slot
    cb = anim_utils.action_ensure_channelbag_for_slot(act, slot)
    lin = bpy.types.Keyframe.bl_rna.properties["interpolation"].enum_items["LINEAR"].value

    def write(path, idx, group, vals):
        fc = cb.fcurves.ensure(path, index=idx, group_name=group)
        fc.keyframe_points.add(len(vals))
        fc.keyframe_points.foreach_set("co", [v for i, x in enumerate(vals) for v in (i, x)])
        fc.keyframe_points.foreach_set("interpolation", [lin] * len(vals))
        fc.update()
    for b, qs in keys.items():
        tgt.pose.bones[b].rotation_mode = "QUATERNION"
        for i in range(4):
            write(f'pose.bones["{b}"].rotation_quaternion', i, b, [q[i] for q in qs])
    for i in range(3):
        write(f'pose.bones["{HIPS}"].location', i, HIPS, [v[i] for v in locs])

    # loop seam: largest bone rotation difference between first and last frame
    seam = 0.0
    for b, qs in keys.items():
        d = qs[0].rotation_difference(qs[-1]).angle
        seam = max(seam, math.degrees(min(d, 2 * math.pi - d)))
    travel = (hips_path[-1] - hips_path[0]) if hips_path else Vector()
    unmapped = sorted(t for t in ("pelvis", "spine_03", "Head", "upperarm_l", "upperarm_r", "lowerarm_l",
                                  "lowerarm_r", "hand_l", "hand_r", "thigh_l", "thigh_r", "calf_l", "calf_r",
                                  "foot_l", "foot_r") if t not in bmap)
    return {
        "frames": n_out, "duration_s": round((n_out - 1) / OUT_FPS, 3), "mapped": len(bmap),
        "fingers": any(k.startswith("index_01") for k in bmap),
        "unmapped_key_bones": unmapped, "max_error_deg": round(err, 3), "hips_scale": round(ratio, 4),
        "loop_seam_deg": round(seam, 2),
        "limb_dir_err_mean_deg": round(dir_err_sum[0] / max(1, dir_err_sum[1]), 3),
        "limb_dir_err_max": [round(dir_err_max[0], 2), dir_err_max[1], dir_err_max[2]],
        "hips_travel_m": [round(travel.x, 3), round(travel.y, 3), round(travel.z, 3)],
        "hips_min_z": round(min(p.z for p in hips_path), 3) if hips_path else None,
        "ankle_min_z": round(foot_min, 3), "ankle_rest_z": round(t_ank, 3), "ground_shift_m": round(ground_dz, 3),
    }


# ---------------------------------------------------------------- export

def export_glb(tgt, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    bpy.ops.object.select_all(action="DESELECT")
    tgt.select_set(True)
    bpy.context.view_layer.objects.active = tgt
    act = tgt.animation_data.action
    sc = bpy.context.scene
    sc.frame_start, sc.frame_end = int(act.frame_range[0]), int(act.frame_range[1])
    bpy.ops.export_scene.gltf(
        filepath=path, export_format="GLB", use_selection=True,
        export_animations=True, export_animation_mode="ACTIONS",
        export_force_sampling=True, export_frame_range=False, export_def_bones=False,
        export_anim_single_armature=True, export_reset_pose_bones=True,
        export_optimize_animation_size=False, export_apply=False, export_yup=True,
        export_materials="NONE", export_skins=True,
    )


def _cleanup_source(src):
    acts = set()
    for o in list(bpy.data.objects):
        if o.name != TARGET:
            if o.animation_data and o.animation_data.action:
                acts.add(o.animation_data.action)
            bpy.data.objects.remove(o)
    for a in list(bpy.data.actions):
        if a in acts or a.users == 0 or a.name.startswith("SRC"):
            if not (bpy.data.objects[TARGET].animation_data and bpy.data.objects[TARGET].animation_data.action == a):
                bpy.data.actions.remove(a)
    for d in list(bpy.data.armatures):
        if d.users == 0:
            bpy.data.armatures.remove(d)


def run_ual(job, template):
    """UAL clips: same rig, copy the action onto the template armature and export."""
    bpy.ops.wm.open_mainfile(filepath=template)
    bpy.context.scene.render.fps = OUT_FPS
    tgt = bpy.data.objects[TARGET]
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=job["src"])
    new = [o for o in bpy.data.objects if o not in before]
    act = bpy.data.actions[job["action"]]
    src = next(o for o in new if o.type == "ARMATURE")
    for o in new:
        bpy.data.objects.remove(o)
    for a in list(bpy.data.actions):
        if a is not act:
            bpy.data.actions.remove(a)
    act.name = job["id"]
    tgt.animation_data_create()
    tgt.animation_data.action = act
    if not tgt.animation_data.action_slot and act.slots:
        tgt.animation_data.action_slot = act.slots[0]
    f0, f1 = act.frame_range
    hips = [fc for fc in act.layers[0].strips[0].channelbags[0].fcurves
            if fc.data_path == f'pose.bones["{HIPS}"].location']
    travel = [round(fc.evaluate(f1) - fc.evaluate(f0), 3) for fc in sorted(hips, key=lambda c: c.array_index)]
    export_glb(tgt, job["out"])
    return {"frames": int(round(f1 - f0)) + 1, "duration_s": round((f1 - f0) / OUT_FPS, 3), "mapped": "all",
            "fingers": True, "max_error_deg": 0.0, "hips_travel_local": travel}


def run_job(job, template):
    if job["family"] == "ual":
        return run_ual(job, template)
    bpy.ops.wm.open_mainfile(filepath=template)
    bpy.context.scene.render.fps = OUT_FPS
    tgt = bpy.data.objects[TARGET]
    src, fps = import_source(job)
    info = retarget(src, tgt, job["family"], fps, job.get("frames"), clip_name=job["id"])
    info["src_fps"] = fps
    _cleanup_source(src)
    export_glb(tgt, job["out"])
    return info


def main():
    a = _args()
    if "make_template" in a:
        make_template(a["make_template"], a["template"])
        return
    jobs = json.load(open(a["jobs"], encoding="utf-8"))
    only = set(a["only"].split(",")) if a.get("only") else None
    if a.get("shard"):  # "i/n": every n-th job starting at i
        i, n = (int(x) for x in a["shard"].split("/"))
        jobs = jobs[i::n]
    with open(a["results"], "a", encoding="utf-8") as res:
        for job in jobs:
            if only and job["id"] not in only:
                continue
            if a["skip_existing"] and os.path.exists(job["out"]):
                continue
            t0 = time.time()
            try:
                info = run_job(job, a["template"])
                info.update(id=job["id"], ok=True)
            except Exception as e:  # keep the batch going, report the failure
                info = {"id": job["id"], "ok": False, "error": f"{type(e).__name__}: {e}",
                        "trace": traceback.format_exc()[-800:]}
            info["sec"] = round(time.time() - t0, 2)
            res.write(json.dumps(info, ensure_ascii=False) + "\n")
            res.flush()
            print("JOB", info["id"], info.get("ok"), info.get("error", ""), flush=True)


if __name__ == "__main__":
    main()
