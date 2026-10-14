#!/usr/bin/env python3
"""
Render the ONYX app icons from the mark's own geometry
======================================================
    python tools/make_icons.py

Writes mono_app/icons/: icon.svg, icon-192.png, icon-512.png and a maskable
icon-maskable-512.png whose mark sits inside the 80 % safe zone Android crops
to. The mark is the same ring and four-point star the app draws: a circle of
radius 42 (stroke 8) and four cubic curves in a 100 x 100 box.
"""

import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "mono_app", "icons")

INK, PAPER = (11, 11, 11, 255), (255, 255, 255, 255)
STAR = [((50, 25), (52, 42), (58, 48), (75, 50)),
        ((75, 50), (58, 52), (52, 58), (50, 75)),
        ((50, 75), (48, 58), (42, 52), (25, 50)),
        ((25, 50), (42, 48), (48, 42), (50, 25))]

SVG = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
<rect width="100" height="100" rx="22" fill="#0B0B0B"/>
<g transform="translate(18 18) scale(.64)">
<circle cx="50" cy="50" r="42" fill="none" stroke="#fff" stroke-width="8"/>
<path d="M50 25C52 42 58 48 75 50 58 52 52 58 50 75 48 58 42 52 25 50 42 48 48 42 50 25Z" fill="#fff"/>
</g>
</svg>
"""


def cubic(p0, p1, p2, p3, n=48):
    pts = []
    for i in range(n + 1):
        t = i / n
        a, b, c, d = (1 - t) ** 3, 3 * (1 - t) ** 2 * t, 3 * (1 - t) * t ** 2, t ** 3
        pts.append((a * p0[0] + b * p1[0] + c * p2[0] + d * p3[0],
                    a * p0[1] + b * p1[1] + c * p2[1] + d * p3[1]))
    return pts


def render(size, mark_fraction, rounded):
    ss = 4                                          # supersample, then downscale: smooth edges
    big = size * ss
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    if rounded:
        d.rounded_rectangle([0, 0, big - 1, big - 1], radius=int(big * 0.22), fill=INK)
    else:
        d.rectangle([0, 0, big, big], fill=INK)

    scale = big * mark_fraction / 100.0
    off = (big - 100 * scale) / 2
    to = lambda x, y: (off + x * scale, off + y * scale)

    cx, cy = to(50, 50)
    r_out, r_in = 46 * scale, 38 * scale            # radius 42, stroke 8
    d.ellipse([cx - r_out, cy - r_out, cx + r_out, cy + r_out], fill=PAPER)
    d.ellipse([cx - r_in, cy - r_in, cx + r_in, cy + r_in], fill=INK)

    outline = []
    for seg in STAR:
        outline += cubic(*seg)[:-1]
    d.polygon([to(x, y) for x, y in outline], fill=PAPER)
    return img.resize((size, size), Image.LANCZOS)


def main():
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "icon.svg"), "w", encoding="utf-8") as f:
        f.write(SVG)
    render(192, 0.64, rounded=False).save(os.path.join(OUT, "icon-192.png"), optimize=True)
    render(512, 0.64, rounded=False).save(os.path.join(OUT, "icon-512.png"), optimize=True)
    render(512, 0.52, rounded=False).save(os.path.join(OUT, "icon-maskable-512.png"), optimize=True)
    for name in sorted(os.listdir(OUT)):
        print(f"  mono_app/icons/{name:24s} {os.path.getsize(os.path.join(OUT, name)) / 1024:6.1f} KB")


if __name__ == "__main__":
    main()
