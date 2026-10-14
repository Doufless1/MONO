#!/usr/bin/env python3
"""
dmu.py - digital mock-up: does it actually go together?
========================================================
Builds both models fully assembled - printed parts, every component the
firmware drives, the cards, the screws and a USB-C plug pushed home - and asks
the questions an assembler would ask on the bench:

  clashes    does anything occupy the same space as anything else?
  access     can a USB-C plug reach the receptacle? can the cards leave
             through the mouth? do the LED's light and the buzzer's sound get
             out? can the servo's pin reach the latch channel?
  fasteners  do the screws pass freely through B2 and bite into B1?

Every answer is measured off the meshes with exact booleans (manifold3d), not
inferred from CONFIG numbers. That difference is how v1's blind USB slot and
its 0.05 mm pin holes got past a geometry suite that passed 71 checks.

    python cad/dmu.py
"""

import os
import sys
from dataclasses import dataclass
from itertools import combinations

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mono_wallet_cad as cad   # noqa: E402

EPS = 0.01          # mm^3: below this, two solids merely touch


@dataclass
class Check:
    group: str
    name: str
    ok: bool
    measured: str
    detail: str = ""


def vol(a, b):
    """Intersection volume, skipping the boolean when the boxes do not even meet."""
    (a0, a1), (b0, b1) = a.bounds, b.bounds
    if np.any(a1 < b0) or np.any(b1 < a0):
        return 0.0
    v = cad.intersect(a, b)
    if v.is_empty:
        return 0.0
    # Solids that only touch can meet in a sliver of zero volume. trimesh works
    # out the centre of mass alongside the volume and divides by it; the volume
    # (0) is right, the centre of mass is unused, so its warning is noise.
    with np.errstate(divide="ignore", invalid="ignore"):
        return float(v.volume)


def union(meshes):
    return cad.fuse(*meshes) if len(meshes) > 1 else meshes[0]


# ================================================================ model B ==
def run_b():
    L = cad.layout_b()
    P = cad.model_b(L)
    B1, B2, B3 = P["B1_works_back_cards"], P["B2_works_front_electronics"], P["B3_component_tray"]
    C = cad.components_b(L)
    plug = cad.usb_plug_b(L)
    out = []

    # ---- clashes: every pair, except the two that are meant to overlap ----
    solids = {"B1": B1, "B2": B2, "B3": B3, **C, **plug}
    meant = {frozenset(("plug_shell", "xiao_usbc"))}                 # a plug in its socket
    meant |= {frozenset((f"screw_{i + 1}", "B1")) for i in range(len(L.screws))}   # threads biting
    worst, pairs = (0.0, None), 0
    for (na, a), (nb, b) in combinations(solids.items(), 2):
        if frozenset((na, nb)) in meant:
            continue
        pairs += 1
        v = vol(a, b)
        if v > worst[0]:
            worst = (v, f"{na} / {nb}")
        if v > EPS:
            out.append(Check("clash", f"{na} clear of {nb}", False, f"{v:.3f} mm3 overlap"))
    out.append(Check("clash", f"no two of {len(solids)} solids overlap ({pairs} pairs)",
                     worst[0] <= EPS, f"worst {worst[0]:.4f} mm3" + (f" ({worst[1]})" if worst[1] else "")))

    # ---- USB-C -------------------------------------------------------------
    case = union([B1, B2])
    shell_hit, body_hit = vol(plug["plug_shell"], case), vol(plug["plug_body"], case)
    engage = L.plug_tip_x - L.usbc_face_x
    out.append(Check("access", "a USB-C plug passes through the case to the XIAO", shell_hit <= EPS and body_hit <= EPS,
                     f"shell {shell_hit:.3f} mm3, overmould {body_hit:.3f} mm3 in the way",
                     "v1's slot stopped 0.5 mm short of the bay, and led to the LiPo"))
    out.append(Check("access", "the plug goes in far enough to make contact", engage >= cad.USBC_MIN_ENGAGE,
                     f"{engage:.2f} mm engaged (contacts mate by {cad.USBC_MIN_ENGAGE} mm)"))

    # ---- cards ---------------------------------------------------------------
    cards = union([v for k, v in C.items() if k.startswith("card_")])
    lift = cad.B_H + 2.0 - L.card_y0                 # lift the stack until it has fully left
    sweep = cad.solid(cad.rrect(cad.CARD_W, cad.CARD_H + lift, cad.CARD_R,
                                cy=L.card_y0 + (cad.CARD_H + lift) / 2), *cards.bounds[:, 2])
    nubs = cad.retention_nubs_b(L)
    blocked = vol(sweep, B1) - (vol(sweep, nubs) if nubs is not None else 0.0)
    out.append(Check("access", "the cards slide out through the mouth", blocked <= EPS and vol(sweep, B2) <= EPS,
                     f"{max(blocked, 0):.3f} mm3 in the way besides the retention nubs",
                     "v1's mouth was 47.5 mm wide, for 53.98 mm cards"))
    mouth = open_span(B1, y=cad.B_H - 0.3, z=sum(L.pocket_z) / 2)
    out.append(Check("access", "the mouth is wider than a card", mouth >= cad.CARD_W + 0.4,
                     f"{mouth:.2f} mm open at the top edge, card {cad.CARD_W} mm"))
    if nubs is not None:
        out.append(Check("access", "retention nubs catch the cards without trapping them",
                         abs(protrusion(B1, L) - cad.B_RETAIN) < 0.05,
                         f"{protrusion(B1, L):.2f} mm proud of each side wall"))

    # ---- what separates the cards from the electronics -------------------------
    xs = np.linspace(-L.pocket_w / 2 + 2, L.pocket_w / 2 - 2, 15)
    ys = np.linspace(cad.B_POCKET_Y0 + 4, cad.B_H - 4, 15)
    grid = np.array([(x, y, -cad.B_SEPTUM / 2) for x in xs for y in ys])
    covered = float(B1.contains(grid).mean())
    out.append(Check("access", "a septum separates the cards from the electronics", covered == 1.0,
                     f"{covered * 100:.0f} % of the pocket roofed over",
                     "v1's card pocket and electronics bay were one open void"))

    # ---- light, sound, sensor, latch --------------------------------------------
    led_path = cad.solid(cad.circle(*L.led_xy, cad.G_LED_D - 0.2, 24), cad.B_BAY_D, L.front_z + 1)
    out.append(Check("access", "the LED's light reaches the front face", vol(led_path, B2) <= EPS and vol(led_path, B3) <= EPS,
                     f"{vol(led_path, B2) + vol(led_path, B3):.3f} mm3 in the way", "v1's tray covered it"))
    sound = union([cad.solid(cad.circle(x, y, 0.6, 12), L.buzzer_z[1], L.front_z + 1) for x, y in L.grille])
    out.append(Check("access", "the buzzer's sound has a way out", vol(sound, B2) <= EPS,
                     f"{len(L.grille)} grille slots open over it", "v1 had no buzzer pocket at all"))
    window = cad.solid(cad.circle(*L.r502_xy, cad.R502_WINDOW_D, 64), L.r502_z[1], L.front_z + 1)
    rim = cad.solid(cad.circle(*L.r502_xy, cad.R502_D, 64).difference(cad.circle(*L.r502_xy, L.r502_window_d, 64)),
                    L.r502_z[1], L.r502_z[1] + L.lip_t)
    out.append(Check("access", "the R502's whole sensing window is exposed", vol(window, B2) <= EPS,
                     f"window {cad.R502_WINDOW_D} mm, aperture {L.r502_window_d:.1f} mm"))
    out.append(Check("access", "a lip holds the R502 in from the front", vol(rim, B2) > 1.0,
                     f"{vol(rim, B2):.1f} mm3 of lip over its rim"))
    w = L.latch_window
    pin = cad.box(L.servo[2], w[1] + 0.2, cad.B_W / 2 + 0.5, w[3] - 0.2, L.latch_z[0] + 0.2, L.latch_z[1] - 0.2)
    out.append(Check("access", "the servo's pin can reach the latch channel", vol(pin, B2) <= EPS,
                     f"{vol(pin, B2):.3f} mm3 in the way", "v1's latch slot was blind behind 0.5 mm of wall"))

    # ---- fasteners ---------------------------------------------------------------
    for i, (x, y) in enumerate(L.screws):
        s = C[f"screw_{i + 1}"]
        free = vol(s, B2)
        bite = 0.0 - L.screw_tip_z
        ok = free <= EPS and bite >= 3.0 and L.pilot_z[0] <= L.screw_tip_z
        out.append(Check("fasteners", f"M2x{cad.B_SCREW_L:.0f} at x={x:+.0f}: free through B2, bites into B1", ok,
                         f"{bite:.1f} mm of thread in B1, pilot {abs(L.pilot_z[0]):.1f} mm deep"))

    # ---- the tray, and the envelope ---------------------------------------------------
    out.append(Check("fit", "the tray drops into the bay", vol(B3, B2) <= EPS,
                     f"{(cad.B_BAY_W - B3.extents[0]) / 2:.2f} mm clearance per side"))
    out.append(Check("fit", f"{cad.N_CARDS_B} cards fit the pocket",
                     L.pocket_d >= cad.N_CARDS_B * cad.CARD_T + 0.3,
                     f"{L.pocket_d:.2f} mm deep for a {cad.N_CARDS_B * cad.CARD_T:.2f} mm stack"))
    out.append(Check("fit", "closed Model B stays pocketable (< 20 mm)", L.thickness < 20.0, f"{L.thickness:.2f} mm"))
    return out


def open_span(mesh, y, z, x_max=40.0, step=0.05):
    """Widest run of empty x at (y, z), centred on x = 0."""
    xs = np.arange(-x_max, x_max, step)
    inside = mesh.contains(np.column_stack([xs, np.full_like(xs, y), np.full_like(xs, z)]))
    c = len(xs) // 2
    if inside[c]:
        return 0.0
    lo, hi = c, c
    while lo > 0 and not inside[lo - 1]:
        lo -= 1
    while hi < len(xs) - 1 and not inside[hi + 1]:
        hi += 1
    return (hi - lo) * step


def protrusion(B1, L):
    """How far a retention nub stands proud of the pocket's side wall."""
    z = sum(L.pocket_z) / 2
    wall = L.pocket_w / 2
    xs = np.arange(wall - 1.0, wall + 0.01, 0.01)
    hit = B1.contains(np.column_stack([xs, np.full_like(xs, cad.B_H - 4.0), np.full_like(xs, z)]))
    return float(wall - xs[np.argmax(hit)]) if hit.any() else 0.0


# ================================================================ model A ==
def run_a():
    A = cad.model_a()
    A1, A2 = A["A1_looks_front_half"], A["A2_looks_back_half"]
    cards = cad.cards_a()
    out = []
    v = vol(A1, A2)
    out.append(Check("clash", "the two halves close: pins sit in their holes", v <= EPS,
                     f"{v:.3f} mm3 overlap", "v1's holes were 0.05 mm deep, cut on the wrong side of the split"))
    cw, ch, cr, ct = cad.a_card()
    hit = sum(vol(c, A1) + vol(c, A2) for c in cards.values())
    out.append(Check("fit", f"{cad.N_CARDS_A} {cad.A_CARDS} cards ({cw} x {ch} mm) fit inside", hit <= EPS,
                     f"{hit:.3f} mm3 overlap"))

    # The skin between the cavity and the outside, probed all the way round,
    # just behind the depth of the concept's own edge features (the latch
    # channel and the USB slot are cut 1.0 mm in, on purpose) and away from
    # the mouth, which is meant to be open.
    body = cad.rrect(cad.G_W, cad.G_H, cad.G_R, cy=cad.G_H / 2)
    ring = body.buffer(-1.1).exterior
    pts = [ring.interpolate(t, normalized=True) for t in np.linspace(0, 1, 720, endpoint=False)]
    mouth_half = cad.G_MOUTH_W / 2 + 0.5
    probe = np.array([(p.x, p.y, z) for p in pts for z in (-0.6, 0.6)
                      if not (p.y > cad.G_H - 5 and abs(p.x) < mouth_half)])
    closed = float((A1.contains(probe) | A2.contains(probe)).mean())
    out.append(Check("fit", "the card cavity never breaks through the outer skin", closed == 1.0,
                     f"{closed * 100:.1f} % of the perimeter closed (outside the mouth)",
                     "v1's broke out over 67 of 90 degrees of each top corner"))
    skin = min_skin_a()
    out.append(Check("fit", "even behind the edge features, a skin is left", skin >= cad.MIN_WALL - 0.01,
                     f"thinnest {skin:.2f} mm", "v1 cut its 1.0 mm-deep latch channel and USB slot into 0.76 mm walls"))
    cav_w = cw + 2 * cad.CARD_CLR
    wall = (cad.G_W - cav_w) / 2
    out.append(Check("fit", "side walls are at least two perimeters", wall >= 2 * cad.NOZZLE,
                     f"{wall:.2f} mm" + (" (concept cards)" if cad.A_CARDS != "iso" else " (ISO cards: v1's trade-off)")))
    return out


def min_skin_a():
    """Thinnest material between the card cavity and the outside of Model A,
    including behind the latch channel and USB slot (2-D, in the split plane)."""
    body = cad.rrect(cad.G_W, cad.G_H, cad.G_R, cy=cad.G_H / 2)
    cw, ch, cr, ct = cad.a_card()
    cav_w, cav_h = cw + 2 * cad.CARD_CLR, ch + 2 * cad.CARD_CLR
    cav = cad.rrect(cav_w, cav_h, cr + cad.CARD_CLR, cy=cad.A_LAND + cav_h / 2).intersection(body.buffer(-cad.MIN_WALL))
    from shapely.geometry import box as sbox
    from shapely.ops import unary_union
    cuts = unary_union([
        sbox(cad.G_W / 2 - 1.0, cad.G_LATCH_Y0, cad.G_W / 2 + 3.0, cad.G_LATCH_Y0 + cad.G_LATCH_L),
        sbox(-cad.G_W / 2 - 3.0, cad.G_USB_Y0, -cad.G_W / 2 + 1.0, cad.G_USB_Y0 + cad.G_USB_L)])
    outside = body.difference(cuts)
    mouth = sbox(-cad.G_MOUTH_W / 2, cad.G_H - 4.0, cad.G_MOUTH_W / 2, cad.G_H + 4.0)
    return outside.exterior.distance(cav.difference(mouth.buffer(1.0)))


def run():
    return run_a(), run_b()


def report(results=None):
    a, b = results or run()
    bad = 0
    for title, checks in (("MODEL A - looks", a), ("MODEL B - works", b)):
        print(f"\n  {title}")
        for c in checks:
            bad += not c.ok
            print(f"    {'PASS' if c.ok else 'FAIL'}  [{c.group:9s}] {c.name}  -  {c.measured}")
    print(f"\n  {sum(len(x) for x in (a, b)) - bad} passed, {bad} failed\n")
    return bad


if __name__ == "__main__":
    sys.exit(1 if report() else 0)
