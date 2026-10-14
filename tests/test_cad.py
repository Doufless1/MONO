#!/usr/bin/env python3
"""
MONO wallet - CAD / geometry test suite
========================================
Checks the generated parts the way a printer and an assembler would: is each
STL watertight, does each model match its stated envelope, do the parts mate,
does everything inside actually fit and reach what it has to reach, and does
the CONFIG block really drive the geometry.

    python tests/test_cad.py            # measure the committed STLs and the assemblies
    python tests/test_cad.py --regen    # also prove the CONFIG block is live

Every number here is measured off a mesh. v1's suite checked many of its
claims with arithmetic on the CONFIG constants - "pin length 1.0 mm" - and so
passed a model whose pins could not enter their 0.05 mm holes.
"""

import contextlib
import glob
import hashlib
import io
import os
import sys
import tempfile

import numpy as np
import trimesh

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "cad"))

import mono_wallet_cad as cad    # noqa: E402  (constants and builders; __main__ guard stops it running)
import dmu                       # noqa: E402

STL = os.path.join(ROOT, "cad", "stl")
EXPECTED = ["A1_looks_front_half.stl", "A2_looks_back_half.stl", "A3_spring_clip.stl", "A4_display_card.stl",
            "B1_works_back_cards.stl", "B2_works_front_electronics.stl", "B3_component_tray.stl"]

PASS = FAIL = 0
FAILURES, NOTES = [], []


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
    # format stores. Every slicer does the same on import, so this is the mesh
    # the printer will actually see.
    return trimesh.load(os.path.join(STL, name), process=True)


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def same_part(a_path, b_path):
    """True if two STLs are the same part. Bytes first; failing that, geometry,
    because the boolean engine's wheels are built by different compilers on
    Windows, macOS and Linux and may differ in the last float bit while the part
    is identical. A real design change moves volume, area or bounds far more."""
    if sha(a_path) == sha(b_path):
        return True, "byte-identical"
    a, b = trimesh.load(a_path, process=True), trimesh.load(b_path, process=True)
    same = (abs(a.volume - b.volume) <= 1e-4 * abs(a.volume) and abs(a.area - b.area) <= 1e-4 * a.area
            and np.allclose(a.bounds, b.bounds, atol=1e-3))
    return same, "same geometry (last-bit float differences only)" if same else \
        f"volume {a.volume:.3f} vs {b.volume:.3f} mm3, area {a.area:.2f} vs {b.area:.2f} mm2"


def depth_below(mesh, x, y, z_from, step=0.02):
    """Empty distance going -z from z_from at (x, y), before solid starts."""
    zs = np.arange(z_from, mesh.bounds[0][2] - step, -step)
    hit = mesh.contains(np.column_stack([np.full_like(zs, x), np.full_like(zs, y), zs]))
    return z_from - zs[np.argmax(hit)] if hit.any() else z_from - mesh.bounds[0][2]


# ==========================================================================
def main():
    print("\n================ MONO CAD - hardware-free geometry checks ================")

    files = sorted(os.path.basename(p) for p in glob.glob(os.path.join(STL, "*.stl")))
    if files != sorted(EXPECTED):
        print(f"\n  expected {EXPECTED} in {STL}, found {files}.  Run:  python cad/mono_wallet_cad.py\n")
        return 1
    meshes = {f: load(f) for f in files}

    # ------------------------------------------------------ printability --
    section("Mesh integrity / printability")
    for f, m in meshes.items():
        degenerate = int((m.area_faces < 1e-9).sum())
        genus = (2 - m.euler_number) // 2
        check(m.is_watertight and m.is_volume and degenerate == 0 and genus >= 0,
              f"{f}: watertight closed volume, {degenerate} degenerate faces, genus {genus}")
    for f, m in meshes.items():
        check(abs(m.bounds[0][2]) < 1e-6, f"{f} sits flat on the bed (z min = {m.bounds[0][2]:.3f})")

    # ---------------------------------------------- the committed STLs ---
    section("The committed STLs are what the generator makes")
    tmp = tempfile.mkdtemp(prefix="mono_cad_")
    with contextlib.redirect_stdout(io.StringIO()):
        cad.build(out=tmp, assemblies=False, quiet=True)
    for f in EXPECTED:
        ok, how = same_part(os.path.join(tmp, f), os.path.join(STL, f))
        check(ok, f"{f} matches a fresh build ({how})" if ok else
              f"{f} is stale: {how}. Run python cad/mono_wallet_cad.py and commit cad/stl/")

    # --------------------------------------------------------- envelope ---
    section("Model A - envelope vs the concept it reproduces")
    a1, a2 = meshes["A1_looks_front_half.stl"], meshes["A2_looks_back_half.stl"]
    for name, e in (("A1", a1.extents), ("A2", a2.extents)):
        check(abs(e[0] - cad.G_W) < 0.05 and abs(e[1] - cad.G_H) < 0.05,
              f"{name} footprint {e[0]:.2f} x {e[1]:.2f} matches the concept's {cad.G_W} x {cad.G_H}")
    check(abs(a2.extents[2] - cad.G_T / 2) < 0.05, f"A2 is half the body thickness ({a2.extents[2]:.2f} = {cad.G_T}/2)")
    pin = a1.extents[2] - cad.G_T / 2
    check(abs(pin - cad.A_PIN_L) < 0.05, f"A1 carries {pin:.2f} mm alignment pins below its split face")
    A = cad.model_a()
    pin_y = (cad.G_SEAM_INSET + cad.A_LAND) / 2
    hole = depth_below(A["A2_looks_back_half"], cad.A_PIN_XS[0], pin_y, 0.0)
    check(hole >= cad.A_PIN_L + 0.1, f"A2's pin holes are {hole:.2f} mm deep for {cad.A_PIN_L} mm pins "
                                     f"(v1: 0.05 mm, cut on the wrong side of the split)")
    groove_hit = A["A2_looks_back_half"].contains([[cad.A_PIN_XS[0], cad.G_SEAM_INSET - 0.1, -0.2]])[0]
    check(not groove_hit, "the pins stay clear of the seam groove, so none shows at the bottom edge")

    section("Model B - envelope")
    L = cad.layout_b()
    b1, b2 = meshes["B1_works_back_cards.stl"], meshes["B2_works_front_electronics.stl"]
    check(abs(b1.extents[0] - cad.B_W) < 0.05 and abs(b1.extents[2] - cad.B_H) < 0.05,
          f"B1 prints upright: {b1.extents[0]:.2f} wide, {b1.extents[2]:.2f} tall, {b1.extents[1]:.2f} thick")
    check(abs(b2.extents[0] - cad.B_W) < 0.05 and abs(b2.extents[1] - cad.B_H) < 0.05,
          f"B2 footprint {b2.extents[0]:.2f} x {b2.extents[1]:.2f} matches {cad.B_W} x {cad.B_H}")
    closed = b1.extents[1] + b2.extents[2] - cad.B_TONGUE_H
    check(abs(closed - L.thickness) < 0.05, f"closed Model B measures {closed:.2f} mm, as the stack-up says")
    check(L.thickness < 20.0, "closed Model B stays under 20 mm (pocketable)")

    # ------------------------------------------------------- mating ------
    section("Half-to-half mating")
    check(0.10 <= cad.FIT <= 0.35, f"FIT = {cad.FIT} mm is inside the usual FDM slip-fit band 0.10-0.35")
    check(cad.B_TONGUE_W >= cad.NOZZLE * 2, f"tongue is {cad.B_TONGUE_W} mm wide, at least 2 nozzle widths")
    B = cad.model_b(L)
    gx = cad.B_W / 2 - cad.B_TONGUE_INSET - cad.B_TONGUE_W / 2
    groove = depth_below(B["B1_works_back_cards"], gx, cad.B_H / 2, 0.0)
    check(groove >= cad.B_TONGUE_H + cad.Z_FIT - 0.03,
          f"the groove is {groove:.2f} mm deep for a {cad.B_TONGUE_H} mm tongue (v1: no clearance at all)")
    tongue_at_mouth = B["B2_works_front_electronics"].contains([[0.0, cad.B_H - cad.B_TONGUE_INSET - 0.4, -0.5]])[0]
    check(not tongue_at_mouth, "no tongue crosses the card mouth (v1's hung into the card exit)")

    # ------------------------------------------------ the CONFIG is live --
    if "--regen" in sys.argv:
        section("Is the CONFIG block actually live?")
        orig = cad.N_CARDS_B
        try:
            cad.N_CARDS_B = 12
            L12 = cad.layout_b()
            b1_12 = cad.model_b(L12)["B1_works_back_cards"]
            d6 = depth_below(B["B1_works_back_cards"], 0.0, 50.0, L.pocket_z[1] - 0.01)
            d12 = depth_below(b1_12, 0.0, 50.0, L12.pocket_z[1] - 0.01)
            check(abs((d12 - d6) - 6 * cad.CARD_T) < 0.05,
                  f"raising N_CARDS_B from 6 to 12 deepens the measured pocket {d6:.2f} -> {d12:.2f} mm (v1: did nothing)")
            check(abs((L12.thickness - L.thickness) - 6 * cad.CARD_T) < 0.01,
                  f"... and the wallet grows to {L12.thickness:.2f} mm to hold them")
        finally:
            cad.N_CARDS_B = orig

    # ------------------------------------------------- the mock-up -------
    a_checks, b_checks = dmu.run()
    for title, checks in (("Model A assembled", a_checks), ("Model B assembled: clashes, access, fasteners", b_checks)):
        section(title)
        for c in checks:
            check(c.ok, f"{c.name} - {c.measured}")

    # ----------------------------------------------------- reality check --
    section("What the numbers in CONFIG still assume")
    note(f"R502_T = {cad.R502_T} mm is unverified: the R502-A listings give its "
         f"diameter ({cad.R502_D} mm) but not its height. Measure yours before printing B2.")
    note(f"the servo is modelled as a {cad.SERVO_L} x {cad.SERVO_W} x {cad.SERVO_T} mm block; real micro "
         f"servos have mounting ears and a horn. Check your part against the pocket.")

    # --------------------------------------------------------- summary ----
    print("\n" + "-" * 70)
    print(f"  {PASS} passed, {FAIL} failed, {len(NOTES)} notes")
    if FAILURES:
        print("\n  Failures:")
        for f in FAILURES:
            print(f"    - {f}")
    print()
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
