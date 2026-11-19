# Retarget onto the Quaternius UAL rig

Turns third-party humanoid animations into clips for the Quaternius Universal
Animation Library rig (the rig of our characters) and writes one GLB per clip to
`content/animations/humanoid_ual/clips/`, plus `manifest.json` and `GAPS.md` there.
No engine code; Blender 5.2 headless.

Sources (unpacked to a work folder, e.g. `D:/AlteraGame/_anim_src`, from the zips in Downloads):

| family | packs | how it is read |
|---|---|---|
| `ual` | UAL1/UAL2 Standard GLB | same rig, actions copied as is |
| `mixamo` | Sword and Shield, Great Sword, Pro Melee Axe, Male Injured, Gestures Basic, Pro Longbow, Action Adventure, Farming, Male Drunk, Pro Magic, single clips | FBX |
| `iglesias` | Human Melee / Crafting / Basic Motions / Dance / Spellcasting FREE (male and `_f` female, `_rm` root-motion versions) | FBX |
| `motifect` | Daily Life v1.0 (modern-life clips skipped) | BVH, cm |
| `cmu` | CMU mocap `allasfamc.zip`, about 260 picked trials (`CMU_PICK` in build_jobs.py), pairs 18/19, 20/21, 22/23 as A/B | ASF/AMC, own reader `asf_amc.py` |

Duplicates named `(1)` are skipped.

## Steps

```
set B="C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"
:: 1. rig template (armature only) from UAL1
%B% -b --factory-startup -P retarget_ual.py -- --make-template <UAL1_Standard.glb> --template work/ual_rig.blend
:: 2. UAL action lists, then the job list (needs work/ual1_actions.json, work/ual2_actions.json, cmu_index.json)
python -I build_jobs.py --src <src> --cmu-index work/cmu_index.json --out-dir <repo>/content/animations/humanoid_ual/clips --jobs work/jobs.json
:: 3. batch (shard to run several Blenders at once; --skip-existing to resume)
%B% -b --factory-startup -P retarget_ual.py -- --template work/ual_rig.blend --jobs work/jobs.json --results work/results_0.jsonl --shard 0/4 --skip-existing
:: 4. manifest and gap report
python -I build_manifest.py --jobs work/jobs.json --results "work/results_*.jsonl" --clip-map clip_map.json --interactions interactions.json --out-dir <repo>/content/animations/humanoid_ual
```

`cmu_index.json` is the motion list of mocap.cs.cmu.edu (search page, all subjects),
parsed to `{subject, trial, desc, fps}`. `ual1_actions.json` / `ual2_actions.json` are the
action names of the UAL GLBs.

Checks: `verify_glb.py` re-imports clips (bones match the rig, ankle height, pelvis range);
`preview_sheet.py` renders a row of frames on the UAL mannequin.

## Method

`retarget_ual.py`, per mapped bone (`bone_maps.py`):

1. the source is turned about Z so its hips' left-right axis matches ours;
2. the bone takes the source bone's world rotation change from rest; for the long limb
   bones it is first corrected by the rotation that brings the source rest direction
   onto ours (T-pose vs lowered arms, stance width);
3. the pelvis takes the source hips' travel, scaled by leg length; the floor is the
   lowest ankle over the clip, and after solving the pelvis is shifted so our lowest
   ankle sits at its rest height;
4. keys on every frame at 30 fps from frame 0, quaternions; root stays still.

Clavicles, hands, feet and fingers are not direction-corrected (rigs differ by build:
CMU clavicles start at the neck), they take the plain rotation change.

## Licences

Mixamo, Iglesias and Motifect clips must not be redistributed as standalone files,
and CMU use needs confirming with the authors before release, so the GLBs are not
in git (`content/animations/humanoid_ual/clips/` is ignored) and are rebuilt from the
sources. `manifest.json` carries the licence per family.
