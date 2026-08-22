#!/usr/bin/env python3
"""
MONO WALLET - parametric prototype generator
=============================================
Generates print-ready STLs for two prototypes:

  MODEL A  "LOOKS"  - exact GLB dimensions (56.2 x 89.2 x 7.2 mm).
                      Display / ergonomics model. Fragile 0.76 mm walls.
  MODEL B  "WORKS"  - enlarged functional mule (62 x 96 x 16 mm).
                      Real ISO cards + XIAO nRF52840 + R502 + LiPo + servo.

All dimensions in millimetres. Edit the CONFIG block and re-run.
Requires: trimesh, shapely, manifold3d, mapbox_earcut, numpy

    pip install trimesh shapely manifold3d mapbox_earcut numpy
    python3 mono_wallet_cad.py
"""

import numpy as np
import trimesh
from shapely.geometry import Point, box as sbox
from shapely.ops import unary_union
import os

OUT = "stl"
os.makedirs(OUT, exist_ok=True)

# ==============================================================
# CONFIG
# ==============================================================

# --- measured from monowallet_1.glb -------------------------------------
G_W, G_H, G_T = 56.2, 89.2, 7.2      # outer body, total closed thickness
G_R = 7.5                             # corner radius
G_SEAM_T, G_SEAM_INSET = 0.8, 0.83    # mid-body groove: height, depth
G_MOUTH_W, G_MOUTH_T = 45.4, 4.4      # card mouth opening
G_WELL_W, G_WELL_H, G_WELL_D = 23.6, 29.6, 1.2   # fingerprint well
G_WELL_CY = 63.1                      # well centre, Y
G_LED_D, G_LED_CY = 2.18, 42.1        # status LED
G_LOGO_D, G_LOGO_CY = 4.4, 11.1       # logo dot
G_LATCH_W, G_LATCH_L = 1.8, 30.0      # latch channel (+X edge)
G_LATCH_Y0, G_LATCH_T = 40.6, 3.2
G_USB_L, G_USB_T = 9.0, 2.4           # USB-C slot (-X edge)
G_USB_Y0 = 16.1
G_CLIP_W, G_CLIP_L, G_CLIP_RISE = 21.5, 54.78, 4.17   # rear spring clip

# --- real-world card (ISO/IEC 7810 ID-1) --------------------------------
CARD_W, CARD_H, CARD_T = 53.98, 85.60, 0.76
CARD_CLR = 0.35        # per-side clearance
N_CARDS_A = 4    # GLB models a 4-card stack; more would break through the
                 # 1.2 mm fingerprint well into the cavity. Do not raise.
N_CARDS_B = 6

# --- print settings -----------------------------------------------------
NOZZLE = 0.4
FIT = 0.20             # slip-fit clearance for mating printed parts

# --- MODEL B envelope ---------------------------------------------------
# Thickness driver: micro servo (6.5) and R502 barrel (5.5) set the bay depth.
B_W, B_H = 62.0, 96.0
B_WALL_BACK, B_WALL_FRONT = 2.0, 2.5
B_SPLIT = 0.0                      # seam plane, z
B_BACK_Z, B_FRONT_Z = -7.0, 9.5    # total 16.5 mm
B_CARD_Y0 = 8.0                    # card cavity starts here (screw land below)
B_BAY_W, B_BAY_H = 54.0, 84.0      # electronics bay
B_BAY_Y0, B_BAY_D = 6.0, 7.0
B_TONGUE_W, B_TONGUE_H = 0.8, 1.0  # seam tongue & groove
B_SCREW_XS = [-25.0, -9.0, 9.0, 25.0]
B_SCREW_Y = 4.0
B_SCREW_PILOT, B_SCREW_CLEAR, B_SCREW_HEAD = 1.6, 2.4, 4.2   # M2 self-tapper

# --- components (MEASURE YOURS AND EDIT) --------------------------------
XIAO_W, XIAO_H, XIAO_T = 21.0, 17.8, 4.0     # XIAO nRF52840 Sense
R502_D, R502_BODY_D, R502_T = 19.0, 21.0, 5.5  # sensor aperture / body / depth
LIPO_W, LIPO_H, LIPO_T = 30.0, 20.0, 4.2     # 302030-ish ~250 mAh
SERVO_W, SERVO_H, SERVO_T = 15.5, 6.5, 16.0  # 1.7 g linear/micro servo


# ==============================================================
# helpers
# ==============================================================

def rrect(w, h, r, cx=0.0, cy=0.0, seg=64):
    """Rounded rectangle as a shapely polygon."""
    r = min(r, w / 2 - 1e-6, h / 2 - 1e-6)
    return sbox(cx - w / 2 + r, cy - h / 2 + r,
                cx + w / 2 - r, cy + h / 2 - r).buffer(r, resolution=seg // 4)


def solid(poly, z0, z1):
    """Extrude a shapely polygon between two z heights."""
    m = trimesh.creation.extrude_polygon(poly, height=abs(z1 - z0))
    m.apply_translation([0, 0, min(z0, z1)])
    return m


def cut(a, b):
    return trimesh.boolean.difference([a, b], engine="manifold")


def fuse(*parts):
    return trimesh.boolean.union(list(parts), engine="manifold")


def save(mesh, name, flip_for_print=False):
    """Export STL. flip_for_print rotates 180 deg about X so the outer face
    lands on the bed (best surface, no supports for the open cavity)."""
    m = mesh.copy()
    if flip_for_print:
        m.apply_transform(trimesh.transformations.rotation_matrix(np.pi, [1, 0, 0]))
    m.apply_translation([0, 0, -m.bounds[0][2]])          # sit on z=0
    m.apply_translation([-m.centroid[0], -m.bounds[0][1], 0])
    path = os.path.join(OUT, name)
    m.export(path)
    e = m.extents
    print(f"  {name:34s} {e[0]:6.2f} x {e[1]:6.2f} x {e[2]:5.2f} mm   "
          f"vol {m.volume/1000:6.2f} cm3   watertight={m.is_watertight}")
    return m


# ==============================================================
# MODEL A - "LOOKS"  (exact GLB geometry)
# ==============================================================

def model_a():
    print("\nMODEL A  -  looks model, exact GLB dimensions")
    half = G_T / 2                       # 3.6
    cav_w = CARD_W + 2 * CARD_CLR        # 54.68
    cav_h = G_H - 1.8                    # bottom wall 1.8, open at top
    cav_t = N_CARDS_A * CARD_T + 0.3     # 4.86
    cav_hz = cav_t / 2

    body_poly = rrect(G_W, G_H, G_R, cy=G_H / 2)
    cav_poly = rrect(cav_w, cav_h, 5.0, cy=1.8 + cav_h / 2)

    # seam groove: material removed in a band around the waist
    ring = body_poly.buffer(2.0).difference(
        rrect(G_W - 2 * G_SEAM_INSET, G_H - 2 * G_SEAM_INSET,
              G_R - G_SEAM_INSET, cy=G_H / 2))

    # card mouth: narrowed lip at the top edge
    mouth = sbox(-G_MOUTH_W / 2, G_H - 4.0, G_MOUTH_W / 2, G_H + 4.0)

    # edge features straddling the seam
    latch = sbox(G_W / 2 - 1.0, G_LATCH_Y0, G_W / 2 + 3.0, G_LATCH_Y0 + G_LATCH_L)
    usb = sbox(-G_W / 2 - 3.0, G_USB_Y0, -G_W / 2 + 1.0, G_USB_Y0 + G_USB_L)

    halves = {}
    for side in ("front", "back"):
        s = 1 if side == "front" else -1
        z0, z1 = (0, half) if s > 0 else (-half, 0)
        m = solid(body_poly, z0, z1)

        # card cavity + mouth
        m = cut(m, solid(cav_poly, -cav_hz, cav_hz))
        m = cut(m, solid(mouth, -G_MOUTH_T / 2, G_MOUTH_T / 2))
        # seam groove (this half's share)
        m = cut(m, solid(ring, 0 if s > 0 else -G_SEAM_T / 2,
                         G_SEAM_T / 2 if s > 0 else 0))
        # edge cut-outs
        m = cut(m, solid(latch, -G_LATCH_T / 2, G_LATCH_T / 2))
        m = cut(m, solid(usb, -G_USB_T / 2, G_USB_T / 2))

        if s > 0:
            # front face detail
            well = rrect(G_WELL_W, G_WELL_H, 4.0, cy=G_WELL_CY)
            m = cut(m, solid(well, half - G_WELL_D, half + 1))
            m = cut(m, solid(Point(0, G_LED_CY).buffer(G_LED_D / 2, 32),
                             half - 0.5, half + 1))
            m = cut(m, solid(Point(0, G_LOGO_CY).buffer(G_LOGO_D / 2, 32),
                             half - 0.4, half + 1))
            # alignment pins in the bottom land
            pins = [solid(Point(x, 0.9).buffer(0.6, 24), -1.0, 0)
                    for x in (-15, 15)]
            m = fuse(m, *pins)
        else:
            holes = [solid(Point(x, 0.9).buffer(0.6 + FIT / 2, 24), -0.05, 1.1)
                     for x in (-15, 15)]
            for h in holes:
                m = cut(m, h)

        halves[side] = m

    save(halves["front"], "A1_looks_front_half.stl", flip_for_print=True)
    save(halves["back"], "A2_looks_back_half.stl", flip_for_print=False)

    # rear spring clip - separate leaf-spring part, print in PETG
    t = 1.0
    n = 40
    ys = np.linspace(0, G_CLIP_L, n)
    arch = G_CLIP_RISE * np.sin(np.pi * ys / G_CLIP_L) ** 1.4
    top = np.column_stack([ys, arch + t])
    bot = np.column_stack([ys, arch])
    prof = np.vstack([top, bot[::-1]])
    from shapely.geometry import Polygon
    clip2d = Polygon(prof)
    clip = trimesh.creation.extrude_polygon(clip2d, height=G_CLIP_W)
    # lie flat: width -> X, length -> Y, arch -> Z  (x,y,z) <- (z,x,y)
    M = np.eye(4)
    M[:3, :3] = np.array([[0, 0, 1], [1, 0, 0], [0, 1, 0]], dtype=float)
    clip.apply_transform(M)
    save(clip, "A3_spring_clip.stl")


# ==============================================================
# MODEL B - "WORKS"  (functional mule)
# ==============================================================

def model_b():
    print("\nMODEL B  -  works model, functional mule")
    cav_w = CARD_W + 2 * CARD_CLR
    cav_h = CARD_H + 0.6
    cav_d = N_CARDS_B * CARD_T + 0.4          # 5.72

    body = rrect(B_W, B_H, G_R, cy=B_H / 2)
    card_cav = rrect(cav_w, cav_h + 4, 5.0, cy=B_CARD_Y0 + (cav_h + 4) / 2 - 2)
    bay = rrect(B_BAY_W, B_BAY_H, 5.0, cy=B_BAY_Y0 + B_BAY_H / 2)
    mouth = sbox(-45.0 / 2, B_H - 6.0, 45.0 / 2, B_H + 4.0)

    tongue_ring = body.buffer(-1.2).difference(body.buffer(-1.2 - B_TONGUE_W))

    # ---- BACK half : cards ------------------------------------------
    bk = solid(body, B_BACK_Z, B_SPLIT)
    bk = cut(bk, solid(card_cav, B_BACK_Z + B_WALL_BACK, B_SPLIT + 0.01))
    bk = cut(bk, solid(mouth, B_BACK_Z + B_WALL_BACK, B_SPLIT + 0.01))
    # tongue groove
    bk = cut(bk, solid(tongue_ring.buffer(FIT / 2), -B_TONGUE_H, B_SPLIT + 0.01))
    # screw pilots
    for x in B_SCREW_XS:
        bk = cut(bk, solid(Point(x, B_SCREW_Y).buffer(B_SCREW_PILOT / 2, 24),
                           B_BACK_Z + 1.2, B_SPLIT + 0.01))
    save(bk, "B1_works_back_cards.stl", flip_for_print=False)

    # ---- FRONT half : electronics -----------------------------------
    fr = solid(body, B_SPLIT, B_FRONT_Z)
    fr = cut(fr, solid(bay, B_SPLIT - 0.01, B_SPLIT + B_BAY_D))
    # tongue
    fr = fuse(fr, solid(tongue_ring, -B_TONGUE_H, B_SPLIT + 0.01))
    # fingerprint sensor aperture + counterbore
    fr = cut(fr, solid(Point(0, G_WELL_CY).buffer(R502_D / 2, 48),
                       B_SPLIT, B_FRONT_Z + 1))
    fr = cut(fr, solid(Point(0, G_WELL_CY).buffer(R502_BODY_D / 2 + FIT, 48),
                       B_FRONT_Z - 1.2, B_FRONT_Z + 1))
    # status LED
    fr = cut(fr, solid(Point(0, G_LED_CY).buffer(2.2 / 2, 24),
                       B_SPLIT, B_FRONT_Z + 1))
    # USB-C access, latch slot
    fr = cut(fr, solid(sbox(-B_W / 2 - 3, G_USB_Y0, -B_W / 2 + 3.5,
                            G_USB_Y0 + G_USB_L), B_SPLIT + 0.8, B_SPLIT + 4.3))
    fr = cut(fr, solid(sbox(B_W / 2 - 3.5, G_LATCH_Y0, B_W / 2 + 3,
                            G_LATCH_Y0 + G_LATCH_L), B_SPLIT + 0.8, B_SPLIT + 3.8))
    # screw clearance + head counterbore
    for x in B_SCREW_XS:
        fr = cut(fr, solid(Point(x, B_SCREW_Y).buffer(B_SCREW_CLEAR / 2, 24),
                           B_SPLIT - 1, B_FRONT_Z + 1))
        fr = cut(fr, solid(Point(x, B_SCREW_Y).buffer(B_SCREW_HEAD / 2, 24),
                           B_FRONT_Z - 1.8, B_FRONT_Z + 1))
    save(fr, "B2_works_front_electronics.stl", flip_for_print=True)

    # ---- component tray : cheap to reprint as parts arrive ----------
    tw, th, tt = B_BAY_W - 2 * FIT, B_BAY_H - 2 * FIT, 1.2
    tray_poly = rrect(tw, th, 4.6, cy=B_BAY_Y0 + B_BAY_H / 2)
    tray = solid(tray_poly, 0, tt)

    def pocket_walls(cx, cy, w, h, wall=1.2, tall=3.0):
        outer = sbox(cx - w / 2 - wall, cy - h / 2 - wall,
                     cx + w / 2 + wall, cy + h / 2 + wall)
        inner = sbox(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2)
        return solid(outer.difference(inner), tt, tt + tall)

    # keep clear of the R502 barrel at (0, G_WELL_CY)
    tray = cut(tray, solid(Point(0, G_WELL_CY).buffer(R502_BODY_D / 2 + 0.8, 48),
                           -1, tt + 6))
    # Layout (all inside the 54 x 84 bay, clear of the sensor barrel):
    #   LiPo  30x20 at (-9, 18)      XIAO 21x17.8 at (8, 40)
    #   servo 6.5x15.5 at (22, 62) - sits through the tray on the bay floor
    tray = fuse(tray,
                pocket_walls(-9.0, 18.0, LIPO_W + FIT, LIPO_H + FIT, tall=3.5),
                pocket_walls(8.0, 40.0, XIAO_W + FIT, XIAO_H + FIT, tall=3.0),
                pocket_walls(22.0, 62.0, SERVO_H + FIT, SERVO_W + FIT, tall=4.5))
    # servo drops through to the bay floor (it is 6.5 mm thick)
    tray = cut(tray, solid(sbox(22 - (SERVO_H + FIT) / 2, 62 - (SERVO_W + FIT) / 2,
                                22 + (SERVO_H + FIT) / 2, 62 + (SERVO_W + FIT) / 2),
                           -1, tt + 0.01))
    # cable pass-throughs
    for cx, cy in ((-9.0, 31.0), (8.0, 52.0), (-20.0, 50.0)):
        tray = cut(tray, solid(sbox(cx - 3.5, cy - 1.5, cx + 3.5, cy + 1.5),
                               -1, tt + 1))
    save(tray, "B3_component_tray.stl")


if __name__ == "__main__":
    print("=" * 74)
    print("MONO WALLET  -  parametric prototype generator")
    print("=" * 74)
    model_a()
    model_b()
    print("\nSTLs written to ./%s/\n" % OUT)
