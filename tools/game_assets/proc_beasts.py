"""Procedural low-poly beasts for bake_animals.py: hens and fish, built in an
empty Blender scene (armature, rigid-skinned parts, flat materials, keyed
clips) in the shape the Quaternius packs come in, facing -Y like them.
Coarse on purpose (the rough retro look of the project's style profile).

build('hen') / build('fish') leave one armature and its meshes in the scene,
with actions Idle, Idle_2, Walk, Run, Eating, Death (24 fps)."""
import bpy, bmesh, math
from mathutils import Vector, Matrix, Quaternion

FPS = 24


def mat(name, rgb):
    m = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    m.use_nodes = True
    b = next(n for n in m.node_tree.nodes if n.type == 'BSDF_PRINCIPLED')
    b.inputs['Base Color'].default_value = (*rgb, 1.0)
    return m


def part(name, arm, bone, verts_fn, material, segs=6):
    """A mesh part bound rigidly to `bone` (one vertex group, weight 1)."""
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    verts_fn(bm, segs)
    bm.to_mesh(me)
    bm.free()
    o = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(o)
    o.data.materials.append(material)
    vg = o.vertex_groups.new(name=bone)
    vg.add([v.index for v in me.vertices], 1.0, 'REPLACE')
    o.parent = arm
    mod = o.modifiers.new('arm', 'ARMATURE')
    mod.object = arm
    return o


def ellipsoid(c, r, rot=None):
    def f(bm, segs):
        res = bmesh.ops.create_uvsphere(bm, u_segments=segs + 2, v_segments=max(4, segs - 1), radius=1.0)
        M = Matrix.Translation(Vector(c)) @ (rot or Matrix.Identity(4)) @ Matrix.Diagonal((*r, 1.0))
        bmesh.ops.transform(bm, matrix=M, verts=res['verts'])
    return f


def cone(base, tip, r, segs_override=None):
    def f(bm, segs):
        n = segs_override or segs
        d = Vector(tip) - Vector(base)
        res = bmesh.ops.create_cone(bm, cap_ends=True, segments=n, radius1=r, radius2=0.0, depth=d.length)
        q = Vector((0, 0, 1)).rotation_difference(d.normalized())
        M = Matrix.Translation((Vector(base) + Vector(tip)) / 2) @ q.to_matrix().to_4x4()
        bmesh.ops.transform(bm, matrix=M, verts=res['verts'])
    return f


def cyl(a, b, r):
    def f(bm, segs):
        d = Vector(b) - Vector(a)
        res = bmesh.ops.create_cone(bm, cap_ends=True, segments=5, radius1=r, radius2=r * 0.8, depth=d.length)
        q = Vector((0, 0, 1)).rotation_difference(d.normalized())
        M = Matrix.Translation((Vector(a) + Vector(b)) / 2) @ q.to_matrix().to_4x4()
        bmesh.ops.transform(bm, matrix=M, verts=res['verts'])
    return f


def fin(pts):
    """A flat polygon (fins, tail, comb), two-sided by a second reversed face."""
    def f(bm, segs):
        vs = [bm.verts.new(Vector(p)) for p in pts]
        bm.faces.new(vs)
        vs2 = [bm.verts.new(Vector(p) + Vector((0.004, 0, 0))) for p in reversed(pts)]
        bm.faces.new(vs2)
    return f


def armature(name, bones):
    """bones: [(name, parent, head, tail)] in metres, armature at the origin."""
    ad = bpy.data.armatures.new(name)
    arm = bpy.data.objects.new(name, ad)
    bpy.context.scene.collection.objects.link(arm)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode='EDIT')
    eb = {}
    for n, p, h, t in bones:
        b = ad.edit_bones.new(n)
        b.head, b.tail = Vector(h), Vector(t)
        if p: b.parent = eb[p]
        eb[n] = b
    bpy.ops.object.mode_set(mode='OBJECT')
    for pb in arm.pose.bones: pb.rotation_mode = 'QUATERNION'
    return arm


def keys(arm, name, frames, fn, loop=True):
    """One action: fn(t in 0..1) -> {bone: (euler xyz radians, loc)} keyed at each frame."""
    act = bpy.data.actions.new(name)
    arm.animation_data_create()
    arm.animation_data.action = act
    for i in range(frames + 1):
        t = i / frames
        pose = fn(t)
        for pb in arm.pose.bones:
            e, l = pose.get(pb.name, ((0, 0, 0), (0, 0, 0)))
            from mathutils import Euler
            pb.rotation_quaternion = Euler(e).to_quaternion()
            pb.location = Vector(l)
            pb.keyframe_insert('rotation_quaternion', frame=i)
            pb.keyframe_insert('location', frame=i)
    act.use_fake_user = True
    return act


def build_hen():
    S = math.sin
    two = 2 * math.pi
    arm = armature('Hen', [
        ('root', None, (0, 0, 0), (0, 0, 0.05)),
        ('Body', 'root', (0, 0.02, 0.2), (0, -0.08, 0.22)),
        ('Neck', 'Body', (0, -0.08, 0.24), (0, -0.12, 0.33)),
        ('Head', 'Neck', (0, -0.12, 0.33), (0, -0.17, 0.36)),
        ('Tail', 'Body', (0, 0.08, 0.24), (0, 0.14, 0.33)),
        ('Wing.L', 'Body', (0.07, -0.02, 0.24), (0.08, 0.1, 0.22)),
        ('Wing.R', 'Body', (-0.07, -0.02, 0.24), (-0.08, 0.1, 0.22)),
        ('Leg.L', 'root', (0.035, 0, 0.14), (0.035, 0, 0.02)),
        ('Leg.R', 'root', (-0.035, 0, 0.14), (-0.035, 0, 0.02)),
    ])
    white, brown = mat('hen_feather', (0.78, 0.62, 0.42)), mat('hen_dark', (0.42, 0.26, 0.14))
    red, yellow = mat('hen_comb', (0.62, 0.06, 0.04)), mat('hen_beak', (0.85, 0.62, 0.12))
    eye = mat('hen_eye', (0.03, 0.03, 0.03))
    part('h_body', arm, 'Body', ellipsoid((0, 0.0, 0.22), (0.085, 0.13, 0.085)), white)
    part('h_breast', arm, 'Body', ellipsoid((0, -0.07, 0.2), (0.07, 0.06, 0.07)), white, 5)
    part('h_neck', arm, 'Neck', ellipsoid((0, -0.11, 0.3), (0.04, 0.045, 0.06)), white, 5)
    part('h_head', arm, 'Head', ellipsoid((0, -0.14, 0.355), (0.04, 0.045, 0.042)), white, 5)
    part('h_beak', arm, 'Head', cone((0, -0.175, 0.352), (0, -0.215, 0.345), 0.014, 4), yellow)
    part('h_comb', arm, 'Head', fin([(0, -0.17, 0.38), (0, -0.15, 0.415), (0, -0.13, 0.405), (0, -0.11, 0.42), (0, -0.1, 0.385)]), red)
    part('h_wattle', arm, 'Head', ellipsoid((0, -0.18, 0.325), (0.012, 0.012, 0.02)), red, 4)
    part('h_eyeL', arm, 'Head', ellipsoid((0.035, -0.155, 0.365), (0.008, 0.008, 0.008)), eye, 4)
    part('h_eyeR', arm, 'Head', ellipsoid((-0.035, -0.155, 0.365), (0.008, 0.008, 0.008)), eye, 4)
    part('h_tail', arm, 'Tail', fin([(0, 0.08, 0.24), (0, 0.17, 0.37), (0, 0.2, 0.32), (0, 0.16, 0.24)]), brown)
    part('h_wingL', arm, 'Wing.L', ellipsoid((0.08, 0.03, 0.23), (0.02, 0.09, 0.055)), brown, 5)
    part('h_wingR', arm, 'Wing.R', ellipsoid((-0.08, 0.03, 0.23), (0.02, 0.09, 0.055)), brown, 5)
    for sd, x in (('L', 0.035), ('R', -0.035)):
        part('h_leg' + sd, arm, 'Leg.' + sd, cyl((x, 0, 0.15), (x, -0.005, 0.015), 0.008), yellow)
        part('h_foot' + sd, arm, 'Leg.' + sd, fin([(x, -0.04, 0.004), (x + 0.015, 0.0, 0.004), (x, 0.02, 0.004), (x - 0.015, 0.0, 0.004)]), yellow)

    def idle(t, a=1.0):
        return {'Neck': ((0.08 * S(two * t) * a, 0, 0.25 * S(two * t * 2) * a), (0, 0, 0)),
                'Head': ((0, 0, 0.3 * S(two * t * 3 + 1) * a), (0, 0, 0)),
                'Body': ((0, 0, 0), (0, 0, 0.004 * S(two * t * 2))),
                'Tail': ((0.05 * S(two * t), 0, 0), (0, 0, 0))}

    def walk(t, sp=1.0):
        sw = S(two * t)
        return {'Leg.L': ((0.55 * sw, 0, 0), (0, 0, 0)), 'Leg.R': ((-0.55 * sw, 0, 0), (0, 0, 0)),
                'Body': ((0, 0.06 * sw, 0), (0, 0, 0.01 * abs(S(two * t)))),
                'Neck': ((-0.25 * S(two * t * 2), 0, 0), (0, -0.015 * S(two * t * 2), 0)),
                'Wing.L': ((0, 0, 0.15 * sp * S(two * t * 2)), (0, 0, 0)),
                'Wing.R': ((0, 0, -0.15 * sp * S(two * t * 2)), (0, 0, 0))}

    def peck(t):
        k = max(0.0, S(two * t * 2))
        return {'Neck': ((-1.1 * k, 0, 0), (0, 0, -0.02 * k)), 'Head': ((-0.4 * k, 0, 0), (0, 0, 0)),
                'Body': ((-0.25 * k, 0, 0), (0, 0, 0)), 'Tail': ((0.3 * k, 0, 0), (0, 0, 0))}

    def death(t):
        k = min(1.0, t * 1.6)
        return {'root': ((0, k * 1.5, 0), (0, 0, 0.03 * k)), 'Neck': ((0.9 * k, 0, 0), (0, 0, 0)),
                'Leg.L': ((0.6 * k, 0, 0), (0, 0, 0)), 'Leg.R': ((0.3 * k, 0, 0), (0, 0, 0))}

    keys(arm, 'Idle', 48, idle)
    keys(arm, 'Idle_2', 72, lambda t: idle(t, 1.6))
    keys(arm, 'Walk', 16, walk)
    keys(arm, 'Run', 10, lambda t: walk(t, 3.0))
    keys(arm, 'Eating', 30, peck)
    keys(arm, 'Death', 20, death, loop=False)
    return arm


def build_fish():
    S = math.sin
    two = 2 * math.pi
    arm = armature('Fish', [
        ('root', None, (0, 0, 0), (0, 0, 0.03)),
        ('Body', 'root', (0, -0.04, 0.0), (0, 0.04, 0.0)),
        ('Tail1', 'Body', (0, 0.04, 0.0), (0, 0.11, 0.0)),
        ('Tail2', 'Tail1', (0, 0.11, 0.0), (0, 0.17, 0.0)),
    ])
    back, belly = mat('fish_back', (0.2, 0.26, 0.24)), mat('fish_belly', (0.62, 0.6, 0.5))
    finm, eye = mat('fish_fin', (0.36, 0.3, 0.2)), mat('fish_eye', (0.02, 0.02, 0.02))
    part('f_head', arm, 'Body', ellipsoid((0, -0.04, 0.0), (0.025, 0.06, 0.035)), back, 6)
    part('f_belly', arm, 'Body', ellipsoid((0, -0.03, -0.012), (0.022, 0.055, 0.022)), belly, 5)
    part('f_mid', arm, 'Tail1', ellipsoid((0, 0.06, 0.0), (0.02, 0.055, 0.028)), back, 6)
    part('f_tailroot', arm, 'Tail2', ellipsoid((0, 0.125, 0.0), (0.01, 0.035, 0.014)), back, 5)
    part('f_tail', arm, 'Tail2', fin([(0, 0.145, 0.0), (0, 0.2, 0.045), (0, 0.185, 0.0), (0, 0.2, -0.045)]), finm)
    part('f_dorsal', arm, 'Tail1', fin([(0, 0.0, 0.03), (0, 0.03, 0.06), (0, 0.08, 0.025)]), finm)
    part('f_eyeL', arm, 'Body', ellipsoid((0.02, -0.075, 0.008), (0.006, 0.006, 0.006)), eye, 4)
    part('f_eyeR', arm, 'Body', ellipsoid((-0.02, -0.075, 0.008), (0.006, 0.006, 0.006)), eye, 4)

    def swim(t, a=1.0, f=1.0):
        return {'Body': ((0, 0, 0.12 * a * S(two * t * f)), (0, 0, 0.004 * S(two * t))),
                'Tail1': ((0, 0, -0.35 * a * S(two * t * f + 0.8)), (0, 0, 0)),
                'Tail2': ((0, 0, -0.55 * a * S(two * t * f + 1.6)), (0, 0, 0))}

    def death(t):
        k = min(1.0, t * 1.5)
        return {'root': ((0, math.pi * k, 0), (0, 0, 0)), 'Tail2': ((0, 0, 0.3 * k), (0, 0, 0))}

    keys(arm, 'Idle', 36, lambda t: swim(t, 0.4))
    keys(arm, 'Idle_2', 48, lambda t: swim(t, 0.6, 2))
    keys(arm, 'Walk', 18, swim)
    keys(arm, 'Run', 10, lambda t: swim(t, 1.4))
    keys(arm, 'Eating', 24, lambda t: swim(t, 0.5, 2))
    keys(arm, 'Death', 20, death, loop=False)
    return arm


def build(kind):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.context.scene.render.fps = FPS
    return {'hen': build_hen, 'fish': build_fish}[kind]()
