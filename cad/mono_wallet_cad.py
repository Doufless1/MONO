#!/usr/bin/env python3
"""
MONO WALLET - parametric prototype generator, v2
=================================================
Print-ready STLs and assembled 3D models for two prototypes:

  MODEL A  "LOOKS"  the concept's exact dimensions (56.2 x 89.2 x 7.2 mm),
                    taken from reference/mono-wallet_1.glb. A display and
                    ergonomics model with 0.76 mm walls: print it gently.
  MODEL B  "WORKS"  an enlarged functional mule (62 x 96 x 17.6 mm) that holds
                    real ISO cards and every part the firmware drives: XIAO
                    nRF52840 Sense, GROW R502-A, LiPo, micro servo, status LED
                    and buzzer.

All dimensions in millimetres. Edit the CONFIG block and re-run. Every derived
dimension and every component position comes from layout_b(), which the part
builders, the clash checks (cad/dmu.py) and the test suite all share, so
features that have to line up are computed from one another rather than typed
twice.

    pip install -r cad/requirements.txt
    python cad/mono_wallet_cad.py          # STLs -> cad/stl/, assemblies -> cad/assembly/

What changed from v1, measured off v1's own meshes (docs/REVIEW.md, C1-C13):
  A   the alignment-pin holes were 0.05 mm deep - cut on the wrong side of the
      split - so the halves could not close; the card cavity broke through the
      outer skin around both top corners.
  B   the USB-C and latch slots were blind behind a 0.5 mm membrane; the XIAO
      was nowhere near the USB slot; cards and electronics shared one open void;
      the servo pocket stood a 16 mm servo up in a 7 mm bay; the card mouth was
      narrower than a card; N_CARDS_B did nothing; the tongue hung into the card
      exit and met its groove with no clearance; the tray covered the status
      LED; the buzzer had nowhere to go.
"""

import argparse
import json
import os
from types import SimpleNamespace

import numpy as np
import trimesh
from shapely.geometry import Point, Polygon, box as sbox
from shapely.ops import unary_union

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "stl")            # next to this file whatever the cwd (v1 wrote into the cwd)
ASM = os.path.join(HERE, "assembly")

# ==============================================================
# CONFIG
# ==============================================================

# --- measured from reference/mono-wallet_1.glb --------------------------
G_W, G_H, G_T = 56.2, 89.2, 7.2      # outer body, total closed thickness
G_R = 7.5                             # corner radius
G_SEAM_T, G_SEAM_INSET = 0.8, 0.83    # mid-body groove: height, depth
G_MOUTH_W, G_MOUTH_T = 45.4, 4.4      # card mouth opening (a graphic in the concept)
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
CARD_R = 3.18          # corner radius
CARD_CLR = 0.35        # per-side clearance
N_CARDS_B = 6          # drives Model B's pocket depth and total thickness

# --- the concept's cards (measured off reference/mono-wallet_1.glb) -----
# The concept was drawn around 52.02 x 84.02 mm cards, and a real ISO card
# does not fit its outline: at best it leaves 0.24 mm of skin over its
# corners, and with room for the alignment pins it pokes out. That is where
# v1's corner breakout and its 0.76 mm walls came from. So Model A holds
# concept-size display cards (print A4 four times) and gets 1.74 mm walls;
# real cards are Model B's job. A_CARDS = "iso" restores v1's trade-off.
G_CARD_W, G_CARD_H, G_CARD_R, G_CARD_T = 52.02, 84.02, 4.31, 0.70
A_CARDS = "concept"    # "concept" | "iso"
N_CARDS_A = 4          # the concept's stack; more would break through the
                       # 1.2 mm fingerprint well. Do not raise.

# --- print settings -----------------------------------------------------
NOZZLE = 0.4
FIT = 0.20             # slip-fit clearance for mating printed parts
Z_FIT = 0.20           # extra depth where a tongue meets its groove
MIN_WALL = 0.6         # thinnest skin the generator will leave anywhere

# --- MODEL A ------------------------------------------------------------
A_LAND = 2.9           # solid band below the card cavity. v1 used 1.8, which
                       # left the pins nowhere to go but the seam groove.
A_PIN_D, A_PIN_L = 1.2, 1.0
A_PIN_XS = (-15.0, 15.0)

# --- MODEL B envelope ---------------------------------------------------
# Stack-up, back to front:  back wall | card pocket | septum | bay | front wall
B_W, B_H = 62.0, 96.0
B_R_BOTTOM, B_R_TOP = 7.5, 3.0     # tight top corners: the full-width card
                                   # mouth then leaves no knife edge
B_WALL_BACK, B_SEPTUM, B_WALL_FRONT = 2.0, 1.0, 2.5
B_POCKET_CLR = 0.5                 # card stack slack, through the thickness
B_BAY_D = 7.0                      # the servo (6.5) and the R502 seat set this
B_POCKET_Y0 = 6.0                  # card pocket starts here; screw land below
B_BAY_W, B_BAY_H, B_BAY_Y0, B_BAY_R = 54.0, 84.0, 6.0, 5.0
B_TONGUE_W, B_TONGUE_H, B_TONGUE_INSET = 0.8, 1.0, 1.2
B_SCREW_XS = (-25.0, -9.0, 9.0, 25.0)
B_SCREW_Y = 4.0
B_SCREW_PILOT, B_SCREW_CLEAR = 1.6, 2.4            # M2 self-tapping
B_SCREW_HEAD, B_SCREW_HEAD_H, B_SCREW_L = 4.2, 1.8, 12.0
B_THUMB_R = 9.0                    # thumb notch in the back wall at the mouth
B_RETAIN = 0.35                    # card retention nubs at the mouth, per side (0 = none)
B_WALL_POCKET = 1.2                # tray pocket walls
B_WALL_POCKET_H = 3.0

# --- components (MEASURE YOURS AND EDIT) --------------------------------
XIAO_L, XIAO_W, XIAO_T = 21.0, 17.8, 4.0     # Seeed wiki: 21 x 17.8 mm; T incl. USB-C
XIAO_PCB_T = 1.0
USBC_W, USBC_H, USBC_D = 9.0, 3.2, 7.4       # receptacle envelope
USBC_PROUD = 0.6                             # how far it overhangs the board edge
XIAO_USB_CY = 20.6                           # where the concept puts the USB port
R502_D, R502_WINDOW_D, R502_T = 22.0, 19.5, 5.5   # R502-A: outer 22, window 19.5
                                                  # (retailer listings); T unverified
LIPO_L, LIPO_W, LIPO_T = 30.0, 20.0, 4.2     # 402030-class pouch + protection board
SERVO_L, SERVO_W, SERVO_T = 16.0, 15.5, 6.5  # 1.7 g micro servo lying flat:
                                             # T is the 6.5 mm side, and it must
                                             # be vertical in a 7 mm bay
LED_D, LED_H, LED_FLANGE_D = 3.0, 4.5, 3.8
BUZZER_S, BUZZER_T = 9.0, 3.0                # 9 x 9 mm SMD magnetic buzzer
PLUG_SHELL_W, PLUG_SHELL_H, PLUG_SHELL_L = 8.4, 2.6, 6.65   # USB-C plug, metal shell
PLUG_BODY_W, PLUG_BODY_H, PLUG_BODY_L = 12.2, 6.6, 20.0     # ... and its overmould
USBC_MIN_ENGAGE = 5.0                        # contacts mate by about here


# ==============================================================
# helpers
# ==============================================================

def rrect(w, h, r, cx=0.0, cy=0.0, seg=64):
    """Rounded rectangle as a shapely polygon."""
    r = min(r, w / 2 - 1e-6, h / 2 - 1e-6)
    return sbox(cx - w / 2 + r, cy - h / 2 + r,
                cx + w / 2 - r, cy + h / 2 - r).buffer(r, quad_segs=seg // 4)


def outline(w, h, r_bottom, r_top, cy):
    """A rounded rectangle whose top corners have their own radius."""
    lower = rrect(w, h, r_bottom, cy=cy).intersection(sbox(-w, cy - h, w, cy))
    upper = rrect(w, h, r_top, cy=cy).intersection(sbox(-w, cy, w, cy + h))
    return unary_union([lower, upper])


def circle(x, y, d, seg=48):
    return Point(x, y).buffer(d / 2, quad_segs=seg // 4)


def solid(poly, z0, z1):
    """Extrude a shapely polygon (or multipolygon) between two z heights."""
    if poly.geom_type == "MultiPolygon":
        return fuse(*[solid(p, z0, z1) for p in poly.geoms])
    m = trimesh.creation.extrude_polygon(poly, height=abs(z1 - z0))
    m.apply_translation([0, 0, min(z0, z1)])
    return m


def xslot(cy, cz, w, h, r, x0, x1):
    """A rounded slot through an X-facing wall: w along y, h along z, from x0 to x1."""
    m = trimesh.creation.extrude_polygon(rrect(w, h, r, cx=cy, cy=cz), height=x1 - x0)
    M = np.array([[0, 0, 1, x0], [1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 0, 1]], dtype=float)
    m.apply_transform(M)
    return m


def box(x0, y0, x1, y1, z0, z1):
    return solid(sbox(x0, y0, x1, y1), z0, z1)


def cut(a, *bs):
    return trimesh.boolean.difference([a, *bs], engine="manifold") if bs else a


def fuse(*parts):
    return trimesh.boolean.union(list(parts), engine="manifold")


def intersect(a, b):
    return trimesh.boolean.intersection([a, b], engine="manifold")


ORIENT = {
    # rotate so the best face lands on the bed
    "asis": np.eye(4),
    "flip": trimesh.transformations.rotation_matrix(np.pi, [1, 0, 0]),         # face down
    "upright": trimesh.transformations.rotation_matrix(np.pi / 2, [1, 0, 0]),  # on its bottom edge
}


def save(mesh, name, orient="asis", out=None, quiet=False):
    """Export a print-oriented STL that sits on z = 0, centred in x."""
    m = mesh.copy()
    m.apply_transform(ORIENT[orient])
    m.apply_translation([-m.bounds[:, 0].mean(), -m.bounds[0][1], -m.bounds[0][2]])
    os.makedirs(out or OUT, exist_ok=True)
    m.export(os.path.join(out or OUT, name))
    if not quiet:
        e = m.extents
        print(f"  {name:34s} {e[0]:6.2f} x {e[1]:6.2f} x {e[2]:5.2f} mm   "
              f"vol {m.volume / 1000:6.2f} cm3   watertight={m.is_watertight}   print: {orient}")
    return m


# ==============================================================
# MODEL A - "LOOKS"  (the concept's geometry)
# ==============================================================

def a_card():
    """(width, height, corner radius, thickness) of the cards Model A holds."""
    return (CARD_W, CARD_H, CARD_R, CARD_T) if A_CARDS == "iso" else (G_CARD_W, G_CARD_H, G_CARD_R, G_CARD_T)


def model_a():
    """Returns {name: mesh} in assembled coordinates: z = 0 is the split."""
    half = G_T / 2
    body = rrect(G_W, G_H, G_R, cy=G_H / 2)
    cw, ch, cr, ct = a_card()
    cav_w, cav_h = cw + 2 * CARD_CLR, ch + 2 * CARD_CLR
    cav_t = N_CARDS_A * ct + 0.3                 # 3.10 for four concept cards
    cav_hz = cav_t / 2

    # Clipped to the outline inset by MIN_WALL, so whatever the card size the
    # cavity follows the rounded corners instead of breaking out through them
    # (v1's left no skin over 67 of 90 degrees of each top corner). With the
    # concept's cards the clip never bites; with ISO cards it is what keeps
    # the corners closed.
    cav = rrect(cav_w, cav_h, cr + CARD_CLR, cy=A_LAND + cav_h / 2).intersection(body.buffer(-MIN_WALL))

    ring = body.buffer(2.0).difference(
        rrect(G_W - 2 * G_SEAM_INSET, G_H - 2 * G_SEAM_INSET, G_R - G_SEAM_INSET, cy=G_H / 2))
    mouth = sbox(-G_MOUTH_W / 2, G_H - 4.0, G_MOUTH_W / 2, G_H + 4.0)
    latch = sbox(G_W / 2 - 1.0, G_LATCH_Y0, G_W / 2 + 3.0, G_LATCH_Y0 + G_LATCH_L)
    usb = sbox(-G_W / 2 - 3.0, G_USB_Y0, -G_W / 2 + 1.0, G_USB_Y0 + G_USB_L)

    # alignment pins sit in the middle of the land, clear of the seam groove
    pin_y = (G_SEAM_INSET + A_LAND) / 2

    halves = {}
    for side in ("front", "back"):
        s = 1 if side == "front" else -1
        z0, z1 = (0, half) if s > 0 else (-half, 0)
        m = solid(body, z0, z1)
        m = cut(m,
                solid(cav, -cav_hz, cav_hz),
                solid(mouth, -G_MOUTH_T / 2, G_MOUTH_T / 2),
                solid(ring, 0 if s > 0 else -G_SEAM_T / 2, G_SEAM_T / 2 if s > 0 else 0),
                solid(latch, -G_LATCH_T / 2, G_LATCH_T / 2),
                solid(usb, -G_USB_T / 2, G_USB_T / 2))
        if s > 0:
            m = cut(m,
                    solid(rrect(G_WELL_W, G_WELL_H, 4.0, cy=G_WELL_CY), half - G_WELL_D, half + 1),
                    solid(circle(0, G_LED_CY, G_LED_D, 32), half - 0.5, half + 1),
                    solid(circle(0, G_LOGO_CY, G_LOGO_D, 32), half - 0.4, half + 1))
            m = fuse(m, *[solid(circle(x, pin_y, A_PIN_D, 24), -A_PIN_L, 0.01) for x in A_PIN_XS])
        else:
            # v1 cut these from z = -0.05 to +1.1: 0.05 mm into this half and
            # the rest into thin air, so the 1 mm pins bottomed out.
            m = cut(m, *[solid(circle(x, pin_y, A_PIN_D + FIT, 24), -(A_PIN_L + Z_FIT), 0.05)
                         for x in A_PIN_XS])
        halves[side] = m

    # rear spring clip - separate leaf-spring part, print in PETG
    t, n = 1.0, 40
    ys = np.linspace(0, G_CLIP_L, n)
    arch = G_CLIP_RISE * np.sin(np.pi * ys / G_CLIP_L) ** 1.4
    prof = np.vstack([np.column_stack([ys, arch + t]), np.column_stack([ys, arch])[::-1]])
    clip = trimesh.creation.extrude_polygon(Polygon(prof), height=G_CLIP_W)
    M = np.eye(4)
    M[:3, :3] = np.array([[0, 0, 1], [1, 0, 0], [0, 1, 0]], dtype=float)   # width->X, length->Y, arch->Z
    clip.apply_transform(M)
    clip.apply_translation([-G_CLIP_W / 2, 0, 0])
    return {"A1_looks_front_half": halves["front"], "A2_looks_back_half": halves["back"],
            "A3_spring_clip": clip, "A4_display_card": solid(rrect(cw, ch, cr, cy=ch / 2), 0, ct)}


def cards_a():
    """Model A's card stack where it sits: on the cavity floor, centred in the split."""
    cw, ch, cr, ct = a_card()
    z0 = -(N_CARDS_A * ct) / 2
    return {f"card_{i + 1}": solid(rrect(cw, ch, cr, cy=A_LAND + CARD_CLR + ch / 2), z0 + i * ct, z0 + (i + 1) * ct)
            for i in range(N_CARDS_A)}


# ==============================================================
# MODEL B - "WORKS"  (functional mule)
# ==============================================================

def layout_b():
    """Every derived dimension and component position for Model B, in assembled
    coordinates: x across, y up the wallet (the card mouth is at y = B_H), z
    through it. z = 0 is the split: cards behind it, electronics in front."""
    L = SimpleNamespace()
    L.body = outline(B_W, B_H, B_R_BOTTOM, B_R_TOP, cy=B_H / 2)

    # stack-up: N_CARDS_B really drives it now (v1 computed cav_d and never used it)
    L.pocket_d = N_CARDS_B * CARD_T + B_POCKET_CLR
    L.back_z = -(B_WALL_BACK + L.pocket_d + B_SEPTUM)
    L.pocket_z = (L.back_z + B_WALL_BACK, -B_SEPTUM)
    L.bay_z = (0.0, B_BAY_D)
    L.front_z = B_BAY_D + B_WALL_FRONT
    L.thickness = L.front_z - L.back_z

    # card pocket: rounded at the bottom like a card, square and open at the top
    L.pocket_w = CARD_W + 2 * CARD_CLR
    L.pocket_r = CARD_R + CARD_CLR
    L.pocket = unary_union([
        rrect(L.pocket_w, 2 * L.pocket_r + 2, L.pocket_r, cy=B_POCKET_Y0 + L.pocket_r + 1),
        sbox(-L.pocket_w / 2, B_POCKET_Y0 + L.pocket_r, L.pocket_w / 2, B_H + 2)])
    L.card_y0 = B_POCKET_Y0 + CARD_CLR
    L.mouth_w = L.pocket_w

    # electronics bay and the tray that lies against the front wall inside it
    L.bay = rrect(B_BAY_W, B_BAY_H, B_BAY_R, cy=B_BAY_Y0 + B_BAY_H / 2)
    L.tray_poly = rrect(B_BAY_W - 2 * FIT, B_BAY_H - 2 * FIT, B_BAY_R - FIT, cy=B_BAY_Y0 + B_BAY_H / 2)
    L.tray_z = (B_BAY_D - 1.2, B_BAY_D)
    L.deck_z = L.tray_z[0]                       # components rest on this face, toward the septum
    L.walls_z = (L.deck_z - B_WALL_POCKET_H, L.deck_z)

    # R502: pushed into a stepped bore from inside; a 0.6 mm lip keeps it in,
    # and its whole sensing window shows through the finger well
    L.well_floor_z = L.front_z - G_WELL_D
    L.lip_t = 0.6
    L.r502_xy = (0.0, G_WELL_CY)
    L.r502_z = (L.well_floor_z - L.lip_t - R502_T, L.well_floor_z - L.lip_t)
    L.r502_bore_d = R502_D + 2 * FIT
    L.r502_window_d = R502_WINDOW_D + 0.6
    L.r502_tray_d = R502_D + 2.0

    # XIAO against the -X wall with its USB-C facing out. The case's USB slot
    # is derived from this, so the two can no longer disagree (in v1 the slot
    # led to the LiPo and the XIAO sat 30 mm away).
    bx0 = -B_BAY_W / 2
    L.xiao = (bx0, XIAO_USB_CY - XIAO_W / 2, bx0 + XIAO_L, XIAO_USB_CY + XIAO_W / 2)
    L.xiao_z = (L.deck_z - XIAO_T, L.deck_z)
    L.usbc_z = (L.deck_z - XIAO_PCB_T - USBC_H, L.deck_z - XIAO_PCB_T)
    L.usbc_cy, L.usbc_cz = XIAO_USB_CY, sum(L.usbc_z) / 2
    L.usbc_face_x = bx0 - USBC_PROUD
    L.usb_slot = (PLUG_SHELL_W + 1.2, max(PLUG_SHELL_H, USBC_H) + 0.6)     # w (y), h (z)
    L.usb_relief = (PLUG_BODY_W + 0.5, PLUG_BODY_H + 0.5, 3.0)            # w, h, depth into the wall
    L.usb_relief_floor_x = -B_W / 2 + L.usb_relief[2]
    L.plug_tip_x = L.usb_relief_floor_x + PLUG_SHELL_L

    # LiPo beside the XIAO, the two pockets sharing a wall
    g, w = FIT / 2, B_WALL_POCKET
    lx0 = L.xiao[2] + 2 * g + w
    L.lipo = (lx0, 11.2, lx0 + LIPO_L, 11.2 + LIPO_W)
    L.lipo_z = (L.deck_z - LIPO_T, L.deck_z)

    # servo flat on the bay floor through a cut-out in the tray, its output end
    # at the +X wall where a window opens into the latch channel
    sx1 = B_BAY_W / 2 - 0.1
    L.servo = (sx1 - SERVO_L, 39.1, sx1, 39.1 + SERVO_W)
    L.servo_z = (B_BAY_D - SERVO_T, B_BAY_D)
    L.latch = (B_W / 2 - G_LATCH_W, G_LATCH_Y0, B_W / 2 + 1.0, G_LATCH_Y0 + G_LATCH_L)
    L.latch_z = (1.8, 1.8 + G_LATCH_T)
    L.latch_window = (B_BAY_W / 2 - 0.5, 43.0, B_W / 2 - G_LATCH_W + 0.2, 51.0)

    # status LED and buzzer press against the inside of the front wall, through
    # holes in the tray, so the light and the sound get out (v1's tray covered
    # the LED hole and the buzzer had no home)
    L.led_xy = (0.0, G_LED_CY)
    L.led_z = (B_BAY_D - LED_H, B_BAY_D)
    L.buzzer_xy = (-20.0, G_WELL_CY)
    L.buzzer_z = (B_BAY_D - BUZZER_T, B_BAY_D)
    L.grille = [(L.buzzer_xy[0] + dx, L.buzzer_xy[1]) for dx in (-2.4, 0.0, 2.4)]

    # fasteners
    L.screws = [(x, B_SCREW_Y) for x in B_SCREW_XS]
    L.screw_head_z = (L.front_z - B_SCREW_HEAD_H, L.front_z - B_SCREW_HEAD_H + 1.6)
    L.screw_tip_z = L.front_z - B_SCREW_HEAD_H - B_SCREW_L
    L.pilot_z = (L.back_z + 1.2, 0.01)

    # the tongue runs round the bottom and sides only: never across the card
    # mouth (v1's hung into the card exit) or the USB relief
    ring = L.body.buffer(-B_TONGUE_INSET).difference(L.body.buffer(-B_TONGUE_INSET - B_TONGUE_W))
    keep_out = unary_union([
        sbox(-L.pocket_w / 2 - 1.0, B_H - 8.0, L.pocket_w / 2 + 1.0, B_H + 1.0),
        sbox(-B_W / 2 - 1.0, L.usbc_cy - L.usb_relief[0] / 2 - 1.5,
             -B_W / 2 + L.usb_relief[2] + 1.5, L.usbc_cy + L.usb_relief[0] / 2 + 1.5)])
    L.tongue = ring.difference(keep_out)
    L.groove = ring.buffer(FIT / 2).difference(keep_out)
    return L


def _usb_relief(L):
    w, h, d = L.usb_relief
    return xslot(L.usbc_cy, L.usbc_cz, w, h, 1.8, -B_W / 2 - 1.0, -B_W / 2 + d)


def retention_nubs_b(L):
    """Two half-cylinders standing B_RETAIN proud of the pocket walls just inside
    the mouth: cards snap past them on the way out instead of sliding out of an
    upside-down wallet. None when B_RETAIN is 0."""
    if B_RETAIN <= 0:
        return None
    r = 1.0
    return fuse(*[solid(circle(sx * (L.pocket_w / 2 + r - B_RETAIN), B_H - 4.0, 2 * r, 24), *L.pocket_z)
                  for sx in (-1, 1)])


def model_b(L=None):
    """Returns {name: mesh} for B1, B2, B3 in assembled coordinates."""
    L = L or layout_b()

    # ---- B1: the card sleeve - back wall, pocket, septum ---------------
    b1 = solid(L.body, L.back_z, 0.0)
    b1 = cut(b1,
             solid(L.pocket, *L.pocket_z),
             solid(circle(0, B_H, 2 * B_THUMB_R, 64).intersection(L.body), L.back_z - 1, L.pocket_z[0] + 0.01),
             solid(L.groove, -(B_TONGUE_H + Z_FIT), 0.01),
             _usb_relief(L),
             *[solid(circle(x, y, B_SCREW_PILOT, 24), *L.pilot_z) for x, y in L.screws])
    nubs = retention_nubs_b(L)
    if nubs is not None:
        b1 = fuse(b1, nubs)

    # ---- B2: the electronics shell -------------------------------------
    b2 = solid(L.body, 0.0, L.front_z)
    b2 = cut(b2, solid(L.bay, -0.01, B_BAY_D))
    b2 = fuse(b2, solid(L.tongue, -B_TONGUE_H, 0.01))
    rx, ry = L.r502_xy
    b2 = cut(b2,
             # finger well, the sensor window through the lip, and the bore behind it
             solid(rrect(G_WELL_W, G_WELL_H, 4.0, cy=G_WELL_CY), L.well_floor_z, L.front_z + 1),
             solid(circle(rx, ry, L.r502_window_d, 64), L.r502_z[1] - 0.01, L.well_floor_z + 0.01),
             solid(circle(rx, ry, L.r502_bore_d, 64), B_BAY_D - 0.01, L.r502_z[1]),
             # status LED and buzzer grille
             solid(circle(*L.led_xy, G_LED_D, 24), B_BAY_D - 0.01, L.front_z + 1),
             *[solid(rrect(1.0, 5.0, 0.5, cx=x, cy=y), B_BAY_D - 0.01, L.front_z + 1) for x, y in L.grille],
             # USB-C: a slot right through the wall to the receptacle (v1's stopped
             # 0.5 mm short of the bay) and a relief outside for the overmould
             xslot(L.usbc_cy, L.usbc_cz, *L.usb_slot, 1.0, -B_W / 2 - 1.0, -B_BAY_W / 2 + 0.6),
             _usb_relief(L),
             # latch channel on the +X edge, and a window into it from the servo
             box(*L.latch, *L.latch_z),
             box(*L.latch_window, *L.latch_z),
             *[solid(circle(x, y, B_SCREW_CLEAR, 24), -B_TONGUE_H - 0.1, L.front_z + 1) for x, y in L.screws],
             *[solid(circle(x, y, B_SCREW_HEAD, 24), L.front_z - B_SCREW_HEAD_H, L.front_z + 1) for x, y in L.screws])

    # ---- B3: component tray ---------------------------------------------
    g, w = FIT / 2, B_WALL_POCKET

    def grow(r, d):
        return sbox(r[0] - d, r[1] - d, r[2] + d, r[3] + d)

    floor = L.tray_poly.difference(unary_union([
        circle(*L.r502_xy, L.r502_tray_d, 64),
        circle(*L.led_xy, LED_D + 0.2, 24),
        sbox(L.buzzer_xy[0] - BUZZER_S / 2 - 0.2, L.buzzer_xy[1] - BUZZER_S / 2 - 0.2,
             L.buzzer_xy[0] + BUZZER_S / 2 + 0.2, L.buzzer_xy[1] + BUZZER_S / 2 + 0.2),
        grow(L.servo, g),                         # the servo sits through the tray on the bay floor
    ]))
    xiao_ring = grow(L.xiao, g + w).difference(grow(L.xiao, g))
    xiao_ring = xiao_ring.difference(sbox(-B_W, L.xiao[1] - 5, L.xiao[0] + 1.0, L.xiao[3] + 5))  # open toward the USB slot
    lipo_ring = grow(L.lipo, g + w).difference(grow(L.lipo, g))
    walls = unary_union([xiao_ring, lipo_ring]).difference(unary_union([
        sbox(-15.0, L.xiao[3] - 1, -11.0, L.xiao[3] + 3),                    # sensor, LED and servo wires
        sbox(L.xiao[2] - 1, 25.5, L.lipo[0] + 1, 28.5),                      # battery leads to the BAT pads
        sbox(10.0, L.lipo[3] - 1, 14.0, L.lipo[3] + 3),                      # servo lead
    ])).intersection(L.tray_poly)
    tray = fuse(solid(floor, *L.tray_z), solid(walls, *L.walls_z))

    return {"B1_works_back_cards": b1, "B2_works_front_electronics": b2, "B3_component_tray": tray}


def components_b(L=None):
    """Envelopes of everything that goes inside Model B, where it goes."""
    L = L or layout_b()
    C = {}
    for i in range(N_CARDS_B):
        z0 = L.pocket_z[0] + i * CARD_T
        C[f"card_{i + 1}"] = solid(rrect(CARD_W, CARD_H, CARD_R, cy=L.card_y0 + CARD_H / 2), z0, z0 + CARD_T)
    C["xiao"] = box(*L.xiao, L.deck_z - XIAO_PCB_T, L.deck_z)
    C["xiao_usbc"] = xslot(L.usbc_cy, L.usbc_cz, USBC_W, USBC_H, 1.0, L.usbc_face_x, L.usbc_face_x + USBC_D)
    C["r502"] = solid(circle(*L.r502_xy, R502_D, 64), *L.r502_z)
    C["lipo"] = box(*L.lipo, *L.lipo_z)
    C["servo"] = box(*L.servo, *L.servo_z)
    C["led"] = fuse(solid(circle(*L.led_xy, LED_D, 24), *L.led_z),
                    solid(circle(*L.led_xy, LED_FLANGE_D, 24), L.led_z[0], L.led_z[0] + 1.0))
    bx, by = L.buzzer_xy
    C["buzzer"] = box(bx - BUZZER_S / 2, by - BUZZER_S / 2, bx + BUZZER_S / 2, by + BUZZER_S / 2, *L.buzzer_z)
    for i, (x, y) in enumerate(L.screws):
        C[f"screw_{i + 1}"] = fuse(solid(circle(x, y, 3.8, 24), *L.screw_head_z),
                                   solid(circle(x, y, 2.0, 24), L.screw_tip_z, L.screw_head_z[0] + 0.01))
    return C


def usb_plug_b(L=None):
    """A USB-C plug pushed fully home: shell and overmould."""
    L = L or layout_b()
    x = L.usb_relief_floor_x
    return {
        "plug_shell": xslot(L.usbc_cy, L.usbc_cz, PLUG_SHELL_W, PLUG_SHELL_H, 1.0, x, L.plug_tip_x),
        "plug_body": xslot(L.usbc_cy, L.usbc_cz, PLUG_BODY_W, PLUG_BODY_H, 2.0, x - PLUG_BODY_L, x),
    }


# ==============================================================
# assemblies for the web viewer
# ==============================================================

PALETTE = {   # rgb, metallic, roughness
    "shell":    ((0.032, 0.033, 0.036), 0.05, 0.42),
    "tray":     ((0.30, 0.31, 0.33), 0.0, 0.62),
    "card":     ((0.62, 0.60, 0.56), 0.0, 0.8),
    "pcb":      ((0.10, 0.12, 0.15), 0.3, 0.45),
    "metal":    ((0.76, 0.77, 0.80), 1.0, 0.3),
    "sensor":   ((0.04, 0.04, 0.05), 0.4, 0.15),
    "lipo":     ((0.70, 0.71, 0.74), 0.7, 0.35),
    "servo":    ((0.16, 0.32, 0.66), 0.0, 0.5),
    "led":      ((0.25, 0.85, 0.45), 0.0, 0.3),
    "buzzer":   ((0.07, 0.07, 0.07), 0.2, 0.5),
}
ROLE = {"B1": "shell", "B2": "shell", "B3": "tray", "A1": "shell", "A2": "shell", "A3": "metal",
        "card": "card", "xiao_usbc": "metal", "xiao": "pcb", "r502": "sensor", "lipo": "lipo",
        "servo": "servo", "led": "led", "buzzer": "buzzer", "screw": "metal"}
EXPLODE_B = {"B1": -18.0, "card": -9.0, "B2": 19.0, "B3": 12.0, "xiao": 5.0, "xiao_usbc": 5.0,
             "lipo": 5.0, "servo": 3.0, "r502": 14.0, "led": 9.0, "buzzer": 9.0, "screw": 27.0}


def _role(name, table):
    for k in sorted(table, key=len, reverse=True):
        if name.startswith(k):
            return table[k]
    return None


def export_assembly(meshes, path, explode=None):
    """A glTF binary with one named, coloured node per part, plus a JSON sidecar
    the viewer reads for explode offsets and labels."""
    scene = trimesh.Scene()
    meta = []
    for name, mesh in meshes.items():
        role = _role(name, ROLE) or "shell"
        rgb, metal, rough = PALETTE[role]
        m = mesh.copy()
        m.visual = trimesh.visual.TextureVisuals(material=trimesh.visual.material.PBRMaterial(
            name=role, baseColorFactor=[*rgb, 1.0], metallicFactor=metal, roughnessFactor=rough))
        scene.add_geometry(m, node_name=name, geom_name=name)
        e = mesh.extents
        meta.append({"name": name, "role": role, "explode_z": (explode and _role(name, explode)) or 0.0,
                     "size_mm": [round(float(v), 2) for v in e]})
    os.makedirs(os.path.dirname(path), exist_ok=True)
    # metres, Y up: what glTF viewers expect
    to_gltf = np.array([[1, 0, 0, 0], [0, 0, 1, 0], [0, -1, 0, 0], [0, 0, 0, 1]], dtype=float) @ np.diag([1e-3] * 3 + [1])
    scene.apply_transform(to_gltf)
    scene.export(path)
    with open(path.replace(".glb", ".json"), "w", encoding="utf-8") as f:
        json.dump({"units": "m", "up": "y", "parts": meta}, f, indent=1)
    return path


# ==============================================================
# main
# ==============================================================

def build(out=None, assemblies=True, quiet=False):
    say = (lambda *a: None) if quiet else print
    say("=" * 74)
    say("MONO WALLET  -  parametric prototype generator v2")
    say("=" * 74)

    say("\nMODEL A  -  looks model, the concept's dimensions")
    A = model_a()
    save(A["A1_looks_front_half"], "A1_looks_front_half.stl", "flip", out, quiet)
    save(A["A2_looks_back_half"], "A2_looks_back_half.stl", "asis", out, quiet)
    save(A["A3_spring_clip"], "A3_spring_clip.stl", "asis", out, quiet)
    if A_CARDS != "iso":
        save(A["A4_display_card"], "A4_display_card.stl", "asis", out, quiet)

    L = layout_b()
    say(f"\nMODEL B  -  works model, {B_W:.0f} x {B_H:.0f} x {L.thickness:.2f} mm for {N_CARDS_B} cards")
    B = model_b(L)
    save(B["B1_works_back_cards"], "B1_works_back_cards.stl", "upright", out, quiet)
    save(B["B2_works_front_electronics"], "B2_works_front_electronics.stl", "flip", out, quiet)
    save(B["B3_component_tray"], "B3_component_tray.stl", "flip", out, quiet)

    if assemblies:
        C = components_b(L)
        clip = A["A3_spring_clip"].copy()
        clip.apply_transform(trimesh.transformations.rotation_matrix(np.pi, [1, 0, 0]))   # bulge backwards
        clip.apply_translation([0, 22.9 - clip.bounds[0][1], -G_T / 2 - clip.bounds[1][2]])
        export_assembly({"A1_looks_front_half": A["A1_looks_front_half"],
                         "A2_looks_back_half": A["A2_looks_back_half"], "A3_spring_clip": clip, **cards_a()},
                        os.path.join(ASM, "model_a.glb"),
                        {"A1": 14.0, "A2": -14.0, "A3": -24.0, "card": 0.0})
        export_assembly({**{k[:2]: v for k, v in B.items()}, **C},
                        os.path.join(ASM, "model_b.glb"), EXPLODE_B)
        say(f"\n  assemblies: cad/assembly/model_a.glb, model_b.glb (+ .json part lists)")

    say(f"\nSTLs written to {os.path.relpath(out or OUT)}{os.sep}\n")
    return A, B, L


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="MONO wallet prototype generator")
    ap.add_argument("--out", help="where the STLs go (default: cad/stl)")
    ap.add_argument("--no-assembly", action="store_true", help="skip the GLB assemblies")
    args = ap.parse_args()
    build(args.out, assemblies=not args.no_assembly)
