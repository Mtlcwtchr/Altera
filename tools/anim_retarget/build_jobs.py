"""Build the job list for retarget_ual.py from the unpacked sources.

python -I build_jobs.py --src D:/AlteraGame/_anim_src --cmu-index cmu_index.json
                        --out-dir <repo>/content/animations/humanoid_ual/clips --jobs jobs.json

Clip ids: <pack>__<slug>. Packs: ual1, ual2 (Quaternius, same rig, copied),
mx_<pack> (Mixamo), igl_<pack> (Kevin Iglesias FREE, male; female variants get _f),
mot (Motifect, from BVH), cmu (selected CMU trials, see CMU_PICK).
Duplicates named "(1)" are skipped.
"""
import argparse
import glob
import json
import os
import re

MIXAMO_PACKS = {
    "Sword_and_Shield_Pack": "mx_sword_shield",
    "Great_Sword_Pack": "mx_great_sword",
    "Pro_Melee_Axe_Pack": "mx_axe",
    "Male_Injured_Pack": "mx_injured",
    "Gestures_Pack_Basic": "mx_gestures",
    "Pro_Longbow_Pack": "mx_longbow",
    "Action_Adventure_Pack": "mx_adventure",
    "Farming_Pack": "mx_farming",
    "Male_Drunk_Pack": "mx_drunk",
    "Pro_Magic_Pack": "mx_magic",
    "mixamo_single": "mx_single",
}
IGLESIAS_PACKS = {
    "Human_Melee_Animations_FREE": "igl_melee",
    "Human_Crafting_Animations_FREE": "igl_crafting",
    "Human_Basic_Motions_FREE": "igl_basic",
    "Human_Dance_Animations_FREE": "igl_dance",
    "Human_Spellcasting_Animations_FREE": "igl_spell",
}
# Modern-life Motifect clips that make no sense in Altera
MOTIFECT_SKIP = {"check_phone_standing", "take_photo_with_phone", "type_on_keyboard", "push_shopping_cart",
                 "open_refrigerator", "brush_teeth", "write_on_whiteboard", "climb_stairs_down", "climb_stairs_up"}

# CMU trials picked for everyday, village, social and two-person actions.
CMU_PICK = {
    2: [5, 6, 7, 8, 9, 10],
    13: [1, 4, 5, 6, 7, 8, 9, 14, 15, 16, 17, 20, 23, 24, 25, 26, 33, 35],
    14: [13, 15, 16, 27, 29, 30, 33],
    15: [4, 6, 7],
    18: list(range(1, 16)), 19: list(range(1, 16)),
    20: [2, 3, 6, 9, 11], 21: [2, 3, 6, 9, 11],
    22: list(range(1, 26)), 23: list(range(1, 26)),
    40: [10, 11],
    56: [2, 3, 4, 5, 6, 7, 8],
    77: [1, 6, 7, 8, 16, 17, 18, 19, 20, 33],
    79: [1, 4, 5, 6, 8, 9, 10, 12, 13, 14, 15, 16, 17, 18, 20, 25, 26, 27, 33, 34, 35, 38, 41, 42, 43, 44,
         55, 68, 69, 70, 71, 72, 73, 74, 84, 86, 87, 88, 89, 90, 95],
    80: [4, 5, 6, 8, 9, 10, 12, 13, 14, 16, 17, 19, 20, 24, 27, 29, 32, 33, 34, 36, 40, 41, 43, 44, 45, 46,
         48, 61, 71, 73],
    111: [2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 14, 17, 18, 25, 32, 33, 36, 37],
    115: [1, 6],
    139: [6, 16, 17, 18, 19, 20, 25],
    140: [1, 2, 3, 4, 8],
    141: [16, 17, 21, 22, 23, 27],
    143: [10, 11, 12, 13, 14, 18, 19, 26, 28, 30],
}
PAIRS = {18: 19, 20: 21, 22: 23}  # subject A -> subject B of the same take


def slug(s):
    s = s.lower().replace("@", "_")
    s = re.sub(r"\(2 subjects.*?\)|\(subjects - subject [ab]\)", "", s)
    s = re.sub(r"[^a-z0-9]+", "_", s).strip("_")
    return re.sub(r"_+", "_", s)[:60]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--cmu-index", required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--jobs", required=True)
    a = ap.parse_args()
    S, O = a.src.replace("\\", "/"), a.out_dir.replace("\\", "/")
    jobs, seen = [], set()

    def add(pack, name, **kw):
        cid = f"{pack}__{slug(name)}"
        n = 2
        base = cid
        while cid in seen:
            cid = f"{base}_{n}"
            n += 1
        seen.add(cid)
        jobs.append(dict(id=cid, pack=pack, out=f"{O}/{pack}/{cid}.glb", **kw))

    # Quaternius UAL: actions listed by name from the probe of the GLBs
    for pack, sub, glb in (("ual1", "Universal_Animation_Library_Standard_/Universal Animation Library[Standard]", "UAL1_Standard.glb"),
                           ("ual2", "Universal_Animation_Library_2_Standard_/Universal Animation Library 2[Standard]", "UAL2_Standard.glb")):
        names = json.load(open(os.path.join(os.path.dirname(a.jobs), f"{pack}_actions.json"), encoding="utf-8"))
        for act in names:
            if act == "A_TPose":
                continue
            add(pack, act, family="ual", src=f"{S}/{sub}/Unreal-Godot/{glb}", action=act, source_name=act)

    for d, pack in MIXAMO_PACKS.items():
        for f in sorted(glob.glob(f"{S}/{d}/**/*.fbx", recursive=True)):
            name = os.path.splitext(os.path.basename(f))[0]
            if "(1)" in name or name.lower() == "x bot":  # duplicate / bare character mesh
                continue
            add(pack, name, family="mixamo", src=f.replace("\\", "/"), source_name=name)

    for d, pack in IGLESIAS_PACKS.items():
        for f in sorted(glob.glob(f"{S}/{d}/**/Animations/**/*.fbx", recursive=True)):
            name = os.path.splitext(os.path.basename(f))[0]
            fem = "/Female/" in f.replace("\\", "/")
            if "Masked" in f:
                continue
            rm = "/RootMotion/" in f.replace("\\", "/")
            nm = name.split("@", 1)[-1]
            if rm and not re.search(r"_rm", nm, re.I):
                nm += "_rm"
            nm += "_f" if fem else ""
            add(pack, nm, family="iglesias", src=f.replace("\\", "/"), source_name=name, female=fem)

    for f in sorted(glob.glob(f"{S}/Motifect_daily_life_complete_v1_0/BVH/*.bvh")):
        name = os.path.splitext(os.path.basename(f))[0]
        if name in MOTIFECT_SKIP:
            continue
        add("mot", name, family="motifect", src=f.replace("\\", "/"), source_name=name, scale=0.01)

    idx = {(o["subject"], o["trial"]): o for o in json.load(open(a.cmu_index, encoding="utf-8"))}
    base = f"{S}/cmu/all_asfamc/subjects"
    pair_of = {**PAIRS, **{b: a_ for a_, b in PAIRS.items()}}
    for subj, trials in CMU_PICK.items():
        for t in trials:
            o = idx.get((subj, t))
            if not o:
                continue
            amc = f"{base}/{subj:02d}/{subj:02d}_{t:02d}.amc"
            if not os.path.exists(amc):
                continue
            extra = {}
            name = f"{subj:02d}_{t:02d}_{o['desc']}"
            if subj in pair_of:
                role = "A" if subj in PAIRS else "B"
                name += f"_{role}"
                extra = {"pair_role": role, "pair_take": f"{min(subj, pair_of[subj]):02d}/{t:02d}"}
            add("cmu", name, family="cmu", src=amc, asf=f"{base}/{subj:02d}/{subj:02d}.asf",
                src_fps=o["fps"], source_name=f"{subj:02d}_{t:02d}", description=o["desc"], **extra)

    json.dump(jobs, open(a.jobs, "w", encoding="utf-8"), ensure_ascii=False, indent=0)
    from collections import Counter
    print(len(jobs), dict(Counter(j["pack"] for j in jobs)))


if __name__ == "__main__":
    main()
