"""
Studio renders of the wallet, for the README and the project page
=================================================================
Runs inside Blender, headless:

    blender -b -P tools/render.py -- [--quick] [--only NAME]

Renders into docs/images/:
    concept.jpg         the concept model (cad/reference/mono-wallet_1.glb)
    model_b.jpg         the works model, assembled
    model_b_open.jpg    the works model, exploded: every part in its place
    model_a.jpg         the looks model with its display cards

JPEG at quality 90: a denoised render is smooth, so it is about 30 times
smaller than the PNG and indistinguishable from it, which matters for a
README and for the project page's first paint.

Same lights, camera language and materials for every shot, so they read as one
set. Cycles with denoising; --quick drops the samples for a fast look.
"""

import json
import math
import os
import sys

import bpy
from mathutils import Vector

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs", "images")
argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
QUICK = "--quick" in argv
ONLY = argv[argv.index("--only") + 1] if "--only" in argv else None


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    s = bpy.context.scene
    s.render.engine = "CYCLES"
    s.cycles.device = "CPU"
    s.cycles.samples = 24 if QUICK else 160
    s.cycles.use_denoising = True
    s.render.resolution_x, s.render.resolution_y = (800, 500) if QUICK else (1600, 1000)
    s.render.film_transparent = False
    s.render.image_settings.file_format = "JPEG"
    s.render.image_settings.quality = 90
    s.view_settings.view_transform = "AgX"
    s.view_settings.look = "AgX - Medium High Contrast"
    world = bpy.data.worlds.new("studio")
    world.use_nodes = True
    bg = world.node_tree.nodes["Background"]
    bg.inputs[0].default_value = (0.012, 0.012, 0.013, 1)
    bg.inputs[1].default_value = 1.0
    s.world = world
    return s


def material(name, rgb, metallic=0.0, roughness=0.5, emission=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    p = m.node_tree.nodes["Principled BSDF"]
    p.inputs["Base Color"].default_value = (*rgb, 1)
    p.inputs["Metallic"].default_value = metallic
    p.inputs["Roughness"].default_value = roughness
    if emission:
        p.inputs["Emission Color"].default_value = (*rgb, 1)
        p.inputs["Emission Strength"].default_value = emission
    return m


def floor(z=0.0):
    bpy.ops.mesh.primitive_plane_add(size=4, location=(0, 0, z))
    f = bpy.context.active_object
    f.data.materials.append(material("floor", (0.018, 0.018, 0.02), 0.0, 0.42))
    return f




def aim(obj, target):
    obj.rotation_euler = (target - obj.location).to_track_quat("-Z", "Y").to_euler()


def studio(centre, scale):
    """Key, fill and rim, placed relative to the subject and scaled to its size.
    Energies are for a ~10 cm product a few tens of centimetres away."""
    k = scale / 0.1
    for name, offset, energy, size in (
            ("key",  (0.30, -0.26, 0.36), 6.0, 0.24),
            ("fill", (-0.38, -0.14, 0.18), 2.6, 0.32),
            ("rim",  (-0.10, 0.42, 0.24), 7.0, 0.16)):
        d = bpy.data.lights.new(name, "AREA")
        d.energy, d.size = energy * k * k, size * k
        o = bpy.data.objects.new(name, d)
        o.location = centre + Vector(offset) * k
        bpy.context.scene.collection.objects.link(o)
        aim(o, centre)


def camera(target, distance, elev, azim, lens=70):
    d = bpy.data.cameras.new("cam")
    d.lens = lens
    d.sensor_fit = "HORIZONTAL"
    d.dof.use_dof = True
    d.dof.focus_distance = distance
    d.dof.aperture_fstop = 16.0
    o = bpy.data.objects.new("cam", d)
    bpy.context.scene.collection.objects.link(o)
    e, a = math.radians(elev), math.radians(azim)
    o.location = target + Vector((distance * math.cos(e) * math.sin(a), -distance * math.cos(e) * math.cos(a), distance * math.sin(e)))
    aim(o, target)
    bpy.context.scene.camera = o


def bounds(objs):
    pts = [o.matrix_world @ Vector(c) for o in objs if o.type == "MESH" for c in o.bound_box]
    lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
    hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
    return lo, hi


def import_glb(path):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=path)
    return [o for o in bpy.data.objects if o not in before]


def smooth(objs, angle=30):
    for o in objs:
        if o.type == "MESH":
            bpy.context.view_layer.objects.active = o
            try:
                bpy.ops.object.shade_auto_smooth(angle=math.radians(angle))
            except Exception:
                pass


def finish(name):
    s = bpy.context.scene
    s.render.filepath = os.path.join(OUT, name)
    os.makedirs(OUT, exist_ok=True)
    bpy.ops.render.render(write_still=True)
    print(f"  rendered docs/images/{name}")


def stage(objs, elev=28, azim=-32, margin=1.35, lens=70):
    """Sit the objects on the floor and frame them."""
    lo, hi = bounds(objs)
    roots = [o for o in objs if o.parent is None]
    for o in roots:
        o.location.z -= lo.z
    lo, hi = bounds(objs)
    floor(0.0)
    centre, radius = (lo + hi) / 2, (hi - lo).length / 2
    studio(centre, max((hi - lo).x, (hi - lo).y, (hi - lo).z))
    # Fit the bounding sphere inside the narrower (vertical) field of view.
    s = bpy.context.scene
    vfov = 2 * math.atan(18.0 * s.render.resolution_y / s.render.resolution_x / lens)
    camera(centre, radius / math.sin(vfov / 2) * margin / 1.35, elev, azim, lens)


def shot_concept():
    reset()
    objs = import_glb(os.path.join(ROOT, "cad", "reference", "mono-wallet_1.glb"))
    smooth(objs)
    stage(objs, elev=24, azim=-38)
    finish("concept.jpg")


def shot_model_b(exploded):
    reset()
    path = os.path.join(ROOT, "cad", "assembly", "model_b.glb")
    objs = import_glb(path)
    with open(path.replace(".glb", ".json"), encoding="utf-8") as f:
        meta = {p["name"]: p for p in json.load(f)["parts"]}
    for o in objs:
        if o.name in meta and meta[o.name]["role"] == "led":
            o.active_material = material("led-lit", (0.25, 1.0, 0.5), 0, 0.3, emission=6.0)
        if exploded and o.name in meta:
            o.location.z += meta[o.name]["explode_z"] / 1000.0
    smooth(objs, 40)
    stage(objs, elev=24 if exploded else 26, azim=-36, margin=1.6 if exploded else 1.45)
    finish("model_b_open.jpg" if exploded else "model_b.jpg")


def shot_model_a():
    reset()
    objs = import_glb(os.path.join(ROOT, "cad", "assembly", "model_a.glb"))
    smooth(objs, 40)
    stage(objs, elev=26, azim=-34)
    finish("model_a.jpg")


SHOTS = {"concept": shot_concept, "model_b": lambda: shot_model_b(False),
         "model_b_open": lambda: shot_model_b(True), "model_a": shot_model_a}

for name, fn in SHOTS.items():
    if ONLY in (None, name):
        fn()
