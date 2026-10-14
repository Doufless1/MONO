#!/usr/bin/env python3
"""
gif.py - a screencast, as a small and evenly timed GIF
======================================================
    python tools/gif.py FRAMES_DIR OUT.gif [--crop X,Y,W,H] [--width 1000] [--fps 12.5]

FRAMES_DIR holds what tools/snap.mjs records: PNG frames plus frames.json,
{"frames": [{"file": ..., "t": seconds}, ...], "end": seconds}. Chrome sends
a frame whenever the page repaints, so they arrive unevenly; each tick of the
GIF's fixed rate shows the newest frame drawn by then.

One palette serves the whole clip, built from frames across it, with no
dithering. A part of the page that did not change therefore stays
byte-identical from frame to frame, and the encoder only has to store the
part that moved. That is what keeps a 15-second clip of a 1000-pixel-wide
page down to a few megabytes.
"""

import argparse
import json
import os
import sys

from PIL import Image

FRAME_BUDGET = 16          # frames sampled to build the shared palette


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("frames")
    ap.add_argument("out")
    ap.add_argument("--crop", help="X,Y,W,H in screencast pixels")
    ap.add_argument("--width", type=int, default=1000)
    ap.add_argument("--fps", type=float, default=12.5)
    a = ap.parse_args()

    with open(os.path.join(a.frames, "frames.json")) as f:
        rec = json.load(f)
    shots = rec["frames"]
    if not shots:
        raise SystemExit("no frames recorded")
    crop = tuple(int(v) for v in a.crop.split(",")) if a.crop else None

    def picture(i):
        im = Image.open(os.path.join(a.frames, shots[i]["file"])).convert("RGB")
        if crop:
            x, y, w, h = crop
            im = im.crop((x, y, x + w, y + h))
        if im.width > a.width:
            im = im.resize((a.width, round(im.height * a.width / im.width)), Image.LANCZOS)
        return im

    # the even timeline: at each tick, the newest frame that had been drawn
    t0, end = shots[0]["t"], max(rec.get("end", 0), shots[-1]["t"])
    rate = len(shots) / max(end - t0, 1e-6)
    if rate < 0.8 * a.fps:
        print(f"warning: captured only {rate:.1f} frames/s for a {a.fps:g} fps GIF; "
              "is the machine busy, or the GPU unavailable?", file=sys.stderr)
    step, ticks, i = 1 / a.fps, [], 0
    for k in range(int((end - t0) / step) + 1):
        while i + 1 < len(shots) and shots[i + 1]["t"] <= t0 + k * step:
            i += 1
        ticks.append(i)

    # one GIF frame per run of identical pictures, held for the whole run
    frames, holds, last = [], [], None
    for idx in ticks:
        if frames and idx == last:
            holds[-1] += 1
            continue
        im = picture(idx)
        if frames and im.tobytes() == frames[-1].tobytes():
            holds[-1] += 1
        else:
            frames.append(im)
            holds.append(1)
        last = idx

    # a shared palette, from frames spread across the clip
    pick = frames[:: max(1, len(frames) // FRAME_BUDGET)][:FRAME_BUDGET]
    sheet = Image.new("RGB", (pick[0].width, pick[0].height * len(pick)))
    for n, im in enumerate(pick):
        sheet.paste(im, (0, n * im.height))
    palette = sheet.quantize(colors=255, method=Image.Quantize.MEDIANCUT)
    gif = [im.quantize(palette=palette, dither=Image.Dither.NONE) for im in frames]

    ms = 1000 / a.fps
    gif[0].save(a.out, save_all=True, append_images=gif[1:], loop=0, disposal=1,
                duration=[round(h * ms / 10) * 10 for h in holds])
    size = os.path.getsize(a.out)
    print(f"({gif[0].width}x{gif[0].height}, {len(gif)} frames from {len(shots)}, "
          f"{len(ticks) * ms / 1000:.1f} s, {size / 1e6:.1f} MB)")


if __name__ == "__main__":
    main()
