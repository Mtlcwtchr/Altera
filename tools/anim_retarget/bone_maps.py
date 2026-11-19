"""Bone maps onto the Quaternius Universal Animation Library rig (UE-style names).

Each map is {target_ual_bone: source_bone}. Bones missing in a source stay at rest
(for example fingers for Iglesias and CMU). Families:
  mixamo   - Mixamo FBX (mixamorig:*), T-pose
  iglesias - Kevin Iglesias FREE packs (B-*), T-pose with slight arm drop
  motifect - Motifect Daily Life BVH (Hips, LeftArm, LeftHandIndex1..4)
  cmu      - CMU ASF/AMC (root, lfemur, lhumerus ...), built by asf_amc.py
"""

UAL_SIDES = (("l", "Left"), ("r", "Right"))

# Bones whose rest direction is matched between source and target (head of this
# bone -> head of the "aim" bone). Only the long limb bones, where rests differ by
# pose (T vs lowered arms, stance width). Clavicles, hands, feet and fingers are
# not corrected: there the rigs differ by construction (CMU clavicles start at the
# neck, Quaternius ones at the chest front), and matching them raises the shoulders.
AIM = {}
for s, _ in UAL_SIDES:
    AIM.update({
        f"upperarm_{s}": f"lowerarm_{s}",
        f"lowerarm_{s}": f"hand_{s}",
        f"thigh_{s}": f"calf_{s}",
        f"calf_{s}": f"foot_{s}",
    })


def _mixamo(prefix="mixamorig:"):
    m = {
        "pelvis": "Hips", "spine_01": "Spine", "spine_02": "Spine1", "spine_03": "Spine2",
        "neck_01": "Neck", "Head": "Head",
    }
    for s, S in UAL_SIDES:
        m.update({
            f"clavicle_{s}": f"{S}Shoulder", f"upperarm_{s}": f"{S}Arm", f"lowerarm_{s}": f"{S}ForeArm",
            f"hand_{s}": f"{S}Hand", f"thigh_{s}": f"{S}UpLeg", f"calf_{s}": f"{S}Leg",
            f"foot_{s}": f"{S}Foot", f"ball_{s}": f"{S}ToeBase",
            f"ball_leaf_{s}": f"{S}Toe_End",
            f"thumb_01_{s}": f"{S}HandThumb1", f"thumb_02_{s}": f"{S}HandThumb2",
            f"thumb_03_{s}": f"{S}HandThumb3", f"thumb_04_leaf_{s}": f"{S}HandThumb4",
        })
        for f, F in (("index", "Index"), ("middle", "Middle"), ("ring", "Ring"), ("pinky", "Pinky")):
            for i in (1, 2, 3):
                m[f"{f}_0{i}_{s}"] = f"{S}Hand{F}{i}"
            m[f"{f}_04_leaf_{s}"] = f"{S}Hand{F}4"
    return {k: prefix + v for k, v in m.items()}


def _iglesias():
    # One source spine bone for two of ours: both copy its world rotation, so
    # spine_02 ends up straight relative to spine_01 (no doubled bend).
    m = {"pelvis": "B-hips", "spine_01": "B-spine", "spine_02": "B-spine", "spine_03": "B-chest",
         "neck_01": "B-neck", "Head": "B-head"}
    for s, S in (("l", "L"), ("r", "R")):
        m.update({
            f"clavicle_{s}": f"B-shoulder.{S}", f"upperarm_{s}": f"B-upperArm.{S}",
            f"lowerarm_{s}": f"B-forearm.{S}", f"hand_{s}": f"B-hand.{S}",
            f"thigh_{s}": f"B-thigh.{S}", f"calf_{s}": f"B-shin.{S}",
            f"foot_{s}": f"B-foot.{S}", f"ball_{s}": f"B-toe.{S}",
        })
    return m


def _motifect():
    m = {"pelvis": "Hips", "spine_01": "Spine1", "spine_02": "Spine2", "spine_03": "Chest",
         "neck_01": "Neck1", "Head": "Head"}
    for s, S in UAL_SIDES:
        m.update({
            f"clavicle_{s}": f"{S}Shoulder", f"upperarm_{s}": f"{S}Arm", f"lowerarm_{s}": f"{S}ForeArm",
            f"hand_{s}": f"{S}Hand", f"thigh_{s}": f"{S}Leg", f"calf_{s}": f"{S}Shin",
            f"foot_{s}": f"{S}Foot", f"ball_{s}": f"{S}ToeBase", f"ball_leaf_{s}": f"{S}ToeEnd",
            f"thumb_01_{s}": f"{S}HandThumb1", f"thumb_02_{s}": f"{S}HandThumb2",
            f"thumb_03_{s}": f"{S}HandThumb3", f"thumb_04_leaf_{s}": f"{S}HandThumbEnd",
        })
        # Motifect Index1 is a metacarpal, Index2 sits at the knuckle like UAL index_01.
        for f, F in (("index", "Index"), ("middle", "Middle"), ("ring", "Ring"), ("pinky", "Pinky")):
            for i in (1, 2, 3):
                m[f"{f}_0{i}_{s}"] = f"{S}Hand{F}{i + 1}"
            m[f"{f}_04_leaf_{s}"] = f"{S}Hand{F}End"
    return m


def _cmu():
    m = {"pelvis": "root", "spine_01": "lowerback", "spine_02": "upperback", "spine_03": "thorax",
         "neck_01": "lowerneck", "Head": "head"}
    for s in ("l", "r"):
        m.update({
            f"clavicle_{s}": f"{s}clavicle", f"upperarm_{s}": f"{s}humerus", f"lowerarm_{s}": f"{s}radius",
            f"hand_{s}": f"{s}hand", f"middle_01_{s}": f"{s}fingers",
            f"thigh_{s}": f"{s}femur", f"calf_{s}": f"{s}tibia", f"foot_{s}": f"{s}foot",
            f"ball_{s}": f"{s}toes", f"thumb_01_{s}": f"{s}thumb",
        })
    return m


MAPS = {
    "mixamo": _mixamo(),
    "iglesias": _iglesias(),
    "motifect": _motifect(),
    "cmu": _cmu(),
}

# Dedicated top bones whose first-frame pose is folded into the source rest.
ROOT = {"iglesias": "B-root"}
