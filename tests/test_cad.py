#!/usr/bin/env python3
"""
MONO wallet - CAD / geometry test suite
========================================
Checks the generated STLs the way a printer and an assembler would: is it
watertight, does it match the stated envelope, do the parts mate, do the
real components fit, and does the CONFIG block actually drive the geometry.

    python tests/test_cad.py            # measure the STLs already in stl/
    python tests/test_cad.py --regen    # also re-run the generator to prove
                                        # the CONFIG parameters are live

Nothing here needs the hardware. It does need the STLs, so run
mono_wallet_cad.py first.
"""

import os
import sys
import io
import glob
import hashlib
import contextlib
import numpy as np
import trimesh

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)

import mono_wallet_cad as cad          # constants only; __main__ guard stops it running

STL = os.path.join(ROOT, "stl")

PASS = FAIL = 0
FAILURES = []
NOTES = []


def check(cond, msg):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"    PASS  {msg}")
    else:
        FAIL += 1
        FAILURES.append(msg)
        print(f"    FAIL  {msg}")
    return bool(cond)


def note(msg):
    NOTES.append(msg)
    print(f"    NOTE  {msg}")


def section(s):
    print(f"\n  == {s} ==")


def load(name):
    # process=True merges the duplicated per-triangle vertices that the STL
    # format stores. Every slicer does the same thing on import, so this is
    # the mesh the printer will actually see - checking the raw one would
    # call all six parts leaky.
    return trimesh.load(os.path.join(STL, name), process=True)


def open_pocket_depth(mesh, x, y, step=0.05):
    """Depth of an open-topped pocket on the vertical line (x, y).
    Returns (depth, floor_z) measured from the top of the part."""
    z0, z1 = mesh.bounds[0][2], mesh.bounds[1][2]
    zs = np.arange(z0 + step, z1, step)
    pts = np.column_stack([np.full_like(zs, x), np.full_like(zs, y), zs])
    inside = mesh.contains(pts)
    if not inside.any():
        return z1 - z0, z0
    floor = zs[inside][-1]
    return z1 - floor, floor


def rect(cx, cy, w, h):
    return (cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2)


def overlap(a, b):
    return not (a[2] <= b[0] or b[2] <= a[0] or a[3] <= b[1] or b[3] <= a[1])


def contains_rect(outer, inner):
    return (inner[0] >= outer[0] and inner[1] >= outer[1] and
            inner[2] <= outer[2] and inner[3] <= outer[3])


# ==========================================================================
def main():
    print("\n================ MONO CAD - hardware-free geometry checks "
          "================")

    files = sorted(os.path.basename(p) for p in glob.glob(os.path.join(STL, "*.stl")))
    if len(files) != 6:
        print(f"\n  expected 6 STLs in {STL}, found {len(files)}."
              f"  Run:  python mono_wallet_cad.py\n")
        return 1

    meshes = {f: load(f) for f in files}

    # ------------------------------------------------------ printability --
    section("Mesh integrity / printability")
    for f, m in meshes.items():
        check(m.is_watertight, f"{f} is watertight (slicer-safe)")
    for f, m in meshes.items():
        check(m.is_volume,
              f"{f} is a closed volume - positive, wound consistently, no self-overlap")
    for f, m in meshes.items():
        degenerate = int((m.area_faces < 1e-9).sum())
        check(degenerate == 0, f"{f} has no degenerate faces ({degenerate} found)")
    for f, m in meshes.items():
        genus = (2 - m.euler_number) // 2
        check(genus >= 0, f"{f} has sane topology (genus {genus})")

    # --------------------------------------------------------- envelope ---
    section("Model A - envelope vs the GLB it reproduces")
    a1, a2 = meshes["A1_looks_front_half.stl"], meshes["A2_looks_back_half.stl"]
    e1, e2 = a1.extents, a2.extents
    check(abs(e1[0] - cad.G_W) < 0.05 and abs(e1[1] - cad.G_H) < 0.05,
          f"A1 footprint {e1[0]:.2f} x {e1[1]:.2f} matches GLB {cad.G_W} x {cad.G_H}")
    check(abs(e2[0] - cad.G_W) < 0.05 and abs(e2[1] - cad.G_H) < 0.05,
          f"A2 footprint {e2[0]:.2f} x {e2[1]:.2f} matches GLB {cad.G_W} x {cad.G_H}")
    check(abs(e2[2] - cad.G_T / 2) < 0.05,
          f"A2 is exactly half the body thickness ({e2[2]:.2f} = {cad.G_T}/2)")
    pin_len = e1[2] - cad.G_T / 2
    check(abs(pin_len - 1.0) < 0.05,
          f"A1 carries {pin_len:.2f} mm alignment pins above its half-thickness")
    closed_a = cad.G_T
    check(abs(closed_a - 7.2) < 0.01,
          f"closed Model A is {closed_a:.2f} mm, the GLB total thickness")

    section("Model B - envelope vs the stated mule")
    b1, b2 = meshes["B1_works_back_cards.stl"], meshes["B2_works_front_electronics.stl"]
    f1, f2 = b1.extents, b2.extents
    check(abs(f1[0] - cad.B_W) < 0.05 and abs(f1[1] - cad.B_H) < 0.05,
          f"B1 footprint {f1[0]:.2f} x {f1[1]:.2f} matches {cad.B_W} x {cad.B_H}")
    check(abs(f2[0] - cad.B_W) < 0.05 and abs(f2[1] - cad.B_H) < 0.05,
          f"B2 footprint {f2[0]:.2f} x {f2[1]:.2f} matches {cad.B_W} x {cad.B_H}")
    closed_b = abs(cad.B_BACK_Z) + cad.B_FRONT_Z
    check(abs(closed_b - 16.5) < 0.01,
          f"closed Model B is {closed_b:.2f} mm, matching the 16.5 mm note")
    check(closed_b < 20.0, "closed Model B stays under 20 mm (pocketable)")

    # ------------------------------------------------------- tongue fit ---
    section("Half-to-half mating")
    check(cad.FIT > 0, f"a slip-fit clearance is specified (FIT = {cad.FIT} mm)")
    check(0.10 <= cad.FIT <= 0.35,
          f"FIT = {cad.FIT} mm is inside the usual FDM slip-fit band 0.10-0.35")
    tongue_proud = cad.B_TONGUE_H
    check(tongue_proud > 0 and tongue_proud <= 2.0,
          f"tongue stands {tongue_proud} mm proud - enough to locate, not enough to snap")
    check(cad.B_TONGUE_W >= cad.NOZZLE * 2,
          f"tongue is {cad.B_TONGUE_W} mm wide, at least 2 nozzle widths")

    # ------------------------------------------------------- card fit A ---
    section("Card fit - Model A (4 cards)")
    cav_w_a = cad.CARD_W + 2 * cad.CARD_CLR
    cav_t_a = cad.N_CARDS_A * cad.CARD_T + 0.3
    stack_a = cad.N_CARDS_A * cad.CARD_T
    check(cav_w_a >= cad.CARD_W + 0.4,
          f"cavity is {cav_w_a:.2f} mm wide vs a {cad.CARD_W} mm card "
          f"({cav_w_a - cad.CARD_W:.2f} mm total slack)")
    check(cav_t_a - stack_a >= 0.2,
          f"cavity is {cav_t_a:.2f} mm deep for a {stack_a:.2f} mm stack "
          f"({cav_t_a - stack_a:.2f} mm slack)")
    wall_a = (cad.G_W - cav_w_a) / 2
    check(wall_a > 0, f"side wall is {wall_a:.2f} mm - positive, so the part closes")
    if wall_a < 2 * cad.NOZZLE:
        note(f"Model A side wall is {wall_a:.2f} mm, under two {cad.NOZZLE} mm "
             f"perimeters. The docstring already calls this fragile; print it "
             f"with 2 perimeters forced and expect it to be a looks-only shell.")

    # ------------------------------------------------------- card fit B ---
    section("Card fit - Model B (6 cards, the functional mule)")
    cav_w_b = cad.CARD_W + 2 * cad.CARD_CLR
    stack_b = cad.N_CARDS_B * cad.CARD_T
    depth_b, floor_b = open_pocket_depth(b1, 0.0, 50.0)
    check(abs(depth_b - (abs(cad.B_BACK_Z) - cad.B_WALL_BACK)) < 0.15,
          f"measured card pocket is {depth_b:.2f} mm deep, as the wall stack-up predicts")
    check(depth_b >= stack_b,
          f"{depth_b:.2f} mm pocket holds a {stack_b:.2f} mm six-card stack "
          f"({depth_b - stack_b:.2f} mm spare)")
    check(cav_w_b >= cad.CARD_W + 0.4,
          f"pocket is {cav_w_b:.2f} mm wide vs a {cad.CARD_W} mm card")

    # the opening the cards must actually pass through
    r = 5.0
    d = (cad.B_CARD_Y0 + (cad.CARD_H + 0.6 + 4) / 2 - 2) + (cad.CARD_H + 0.6 + 4) / 2 - cad.B_H
    if 0 < d < r:
        half = (cav_w_b / 2 - r) + np.sqrt(max(r * r - (r - d) ** 2, 0.0))
        mouth_b = 2 * half
    else:
        mouth_b = 45.0
    check(mouth_b > 0, f"Model B has a {mouth_b:.2f} mm opening at the top edge")
    if mouth_b < cad.CARD_W + 0.4:
        note(f"Model B card slot is {mouth_b:.2f} mm across but an ISO card is "
             f"{cad.CARD_W} mm. Cards can only be loaded by unscrewing the case. "
             f"If you meant to slide them in and out, the mouth needs "
             f"{cad.CARD_W + 0.4:.1f} mm+.")
    if cad.G_MOUTH_W < cad.CARD_W:
        note(f"Model A mouth is {cad.G_MOUTH_W} mm vs the same {cad.CARD_W} mm card "
             f"- same story, and A does not come apart at all.")

    # ------------------------------------------------- electronics bay ----
    section("Electronics bay and component tray")
    tray = meshes["B3_component_tray.stl"]
    te = tray.extents
    check(te[0] <= cad.B_BAY_W and te[1] <= cad.B_BAY_H,
          f"tray {te[0]:.2f} x {te[1]:.2f} fits the {cad.B_BAY_W} x {cad.B_BAY_H} bay")
    clr = (cad.B_BAY_W - te[0]) / 2
    check(clr >= 0.15,
          f"tray has {clr:.2f} mm clearance per side (drops in without sanding)")
    check(te[2] <= cad.B_BAY_D,
          f"tray with its tallest wall is {te[2]:.2f} mm, inside the "
          f"{cad.B_BAY_D} mm bay depth")

    bay = rect(0, cad.B_BAY_Y0 + cad.B_BAY_H / 2, cad.B_BAY_W, cad.B_BAY_H)
    pockets = {
        "LiPo":  rect(-9.0, 18.0, cad.LIPO_W + cad.FIT + 2.4, cad.LIPO_H + cad.FIT + 2.4),
        "XIAO":  rect(8.0, 40.0, cad.XIAO_W + cad.FIT + 2.4, cad.XIAO_H + cad.FIT + 2.4),
        "servo": rect(22.0, 62.0, cad.SERVO_H + cad.FIT + 2.4, cad.SERVO_W + cad.FIT + 2.4),
    }
    for name, p in pockets.items():
        check(contains_rect(bay, p), f"{name} pocket sits inside the bay footprint")
    names = list(pockets)
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            check(not overlap(pockets[names[i]], pockets[names[j]]),
                  f"{names[i]} and {names[j]} pockets do not collide")

    # the R502 barrel comes down through the tray
    barrel_r = cad.R502_BODY_D / 2 + 0.8
    for name, p in pockets.items():
        dx = max(p[0] - 0.0, 0.0, 0.0 - p[2])
        dy = max(p[1] - cad.G_WELL_CY, 0.0, cad.G_WELL_CY - p[3])
        dist = np.hypot(dx, dy)
        check(dist >= barrel_r,
              f"{name} pocket clears the R502 barrel by {dist - barrel_r:.2f} mm")

    # depth budget
    check(cad.R502_T <= cad.B_BAY_D,
          f"R502 barrel {cad.R502_T} mm fits the {cad.B_BAY_D} mm bay")
    check(cad.LIPO_T + 1.2 <= cad.B_BAY_D,
          f"LiPo {cad.LIPO_T} mm on a 1.2 mm tray floor fits the bay")
    check(cad.XIAO_T + 1.2 <= cad.B_BAY_D,
          f"XIAO {cad.XIAO_T} mm on a 1.2 mm tray floor fits the bay")
    servo_thru = max(cad.SERVO_W, cad.SERVO_H, cad.SERVO_T)
    if min(cad.SERVO_W, cad.SERVO_H, cad.SERVO_T) > cad.B_BAY_D:
        check(False, "servo fits the bay depth in some orientation")
    else:
        check(True, f"servo fits the bay depth on its smallest face "
                    f"({min(cad.SERVO_W, cad.SERVO_H, cad.SERVO_T)} mm "
                    f"<= {cad.B_BAY_D} mm)")
        if servo_thru > cad.B_BAY_D:
            note(f"the servo pocket footprint is {cad.SERVO_H} x {cad.SERVO_W} mm, "
                 f"which leaves its {cad.SERVO_T} mm dimension vertical in a "
                 f"{cad.B_BAY_D} mm bay. Either SERVO_T is mislabelled or the "
                 f"pocket needs to be {cad.SERVO_T} x {cad.SERVO_H} mm. Measure "
                 f"your actual servo before printing B3.")

    # ------------------------------------------------------------ screws --
    section("Fasteners")
    check(cad.B_SCREW_PILOT < cad.B_SCREW_CLEAR < cad.B_SCREW_HEAD,
          f"M2 pilot {cad.B_SCREW_PILOT} < clearance {cad.B_SCREW_CLEAR} "
          f"< head {cad.B_SCREW_HEAD}")
    check(cad.B_SCREW_PILOT >= 1.4,
          f"pilot {cad.B_SCREW_PILOT} mm is not so tight it splits the boss")
    for x in cad.B_SCREW_XS:
        edge = cad.B_W / 2 - (abs(x) + cad.B_SCREW_HEAD / 2)
        check(edge > 0.8,
              f"screw at x={x} leaves {edge:.2f} mm of wall outboard of its head")
    card_bottom = cad.B_CARD_Y0 + (cad.CARD_H + 0.6 + 4) / 2 - 2 - (cad.CARD_H + 0.6 + 4) / 2
    check(cad.B_SCREW_Y + cad.B_SCREW_HEAD / 2 <= card_bottom + 0.15,
          f"screw heads stop at y={cad.B_SCREW_Y + cad.B_SCREW_HEAD / 2:.2f}, "
          f"below the card pocket at y={card_bottom:.2f}")

    # ---------------------------------------------------- sensor opening --
    section("Fingerprint sensor mounting")
    check(cad.R502_D < cad.R502_BODY_D,
          f"aperture {cad.R502_D} mm is smaller than the {cad.R502_BODY_D} mm body, "
          f"so the sensor cannot fall through")
    lip = (cad.R502_BODY_D - cad.R502_D) / 2
    check(lip >= 0.8, f"retaining lip is {lip:.2f} mm all round")
    check(cad.G_WELL_W >= cad.R502_D and cad.G_WELL_H >= cad.R502_D,
          f"Model A finger well {cad.G_WELL_W} x {cad.G_WELL_H} covers the "
          f"{cad.R502_D} mm sensor it stands in for")

    # ---------------------------------------------- parametric integrity --
    if "--regen" in sys.argv:
        section("Is the CONFIG block actually live?")
        import tempfile
        orig_out, orig_n = cad.OUT, cad.N_CARDS_B
        try:
            tmp = tempfile.mkdtemp(prefix="mono_cad_")
            cad.OUT, cad.N_CARDS_B = tmp, 12          # double the card count
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                cad.model_b()
            m = trimesh.load(os.path.join(tmp, "B1_works_back_cards.stl"),
                             process=False)
            d12, _ = open_pocket_depth(m, 0.0, 50.0)
            moved = abs(d12 - depth_b) > 0.2
            check(moved,
                  f"raising N_CARDS_B from 6 to 12 changes the pocket depth "
                  f"({depth_b:.2f} -> {d12:.2f} mm)")
            if not moved:
                note("N_CARDS_B is computed into cav_d at mono_wallet_cad.py:213 "
                     "and then never used - the card pocket depth is actually "
                     "fixed by B_BACK_Z and B_WALL_BACK. Editing N_CARDS_B does "
                     "nothing, which the CONFIG comment does not say.")
        finally:
            cad.OUT, cad.N_CARDS_B = orig_out, orig_n

    # --------------------------------------------------- stale comments --
    section("CONFIG block self-consistency")
    check(abs((cad.N_CARDS_A * cad.CARD_T + 0.3) - 3.34) < 0.01,
          f"Model A cavity really is {cad.N_CARDS_A * cad.CARD_T + 0.3:.2f} mm")
    note(f"mono_wallet_cad.py:130 annotates cav_t as 4.86 mm but with "
         f"N_CARDS_A={cad.N_CARDS_A} it is "
         f"{cad.N_CARDS_A * cad.CARD_T + 0.3:.2f} mm (4.86 is the 6-card value).")
    note(f"mono_wallet_cad.py:213 annotates cav_d as 5.72 mm but with "
         f"N_CARDS_B={cad.N_CARDS_B} it is "
         f"{cad.N_CARDS_B * cad.CARD_T + 0.4:.2f} mm.")

    # --------------------------------------------------------- summary ----
    print("\n" + "-" * 70)
    print(f"  {PASS} passed, {FAIL} failed, {len(NOTES)} notes")
    if FAILURES:
        print("\n  Failures:")
        for f in FAILURES:
            print(f"    - {f}")
    if NOTES:
        print("\n  Notes to act on before printing:")
        for i, n in enumerate(NOTES, 1):
            print(f"    {i}. {n}")
    print()
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
