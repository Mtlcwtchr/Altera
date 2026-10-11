"""world_megaliths - the megaliths (doc/rework/MESH_TERRAIN_PLAN.md): strange,
surreal standing things - rings and stakes sticking out of the ground,
Stonehenge-like stones, standing panels, needles, the stone giants. Not
terrain: they stand on it. Placed by world_features.py from the "megaliths"
section of content/config/terrain_core/props.json.
"""


def place(world, rules, catalogue, rng, add):
    import world_features as wf
    return {"groups": wf.place_groups(world, rules, catalogue, rng, add)}
