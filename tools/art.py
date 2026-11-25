#!/usr/bin/env python3
"""joyride's own art (original, CC0; nothing of the games):

  art/background.png   the launcher's background: a city at night seen from
                       above, as these games see it, dark enough for text
  art/joyride-*.png    the icon (art/joyride.svg) at the sizes wanted, by
                       rsvg-convert

  art.py [city|icon|all]

Also the street grid the GTA menus' wall has (backdrops.py imports city()).
Needs numpy and Pillow; the icon rsvg-convert.
"""
import os
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')


def city(w, h, block=64, seed=1969, cars=True):
    """a top-down city, w x h, in shades (0..1, H x W x 3) and its streets'
    mask: blocks of buildings (roofs of a few shades, things on them),
    parks, streets with their middle lines, a few cars' lights"""
    rng = np.random.default_rng(seed)
    S = 2                                         # drawn at twice the size, then halved
    W, H, B = w * S, h * S, block * S
    img = Image.new('RGB', (W, H), (24, 26, 31))
    d = ImageDraw.Draw(img)
    street = Image.new('L', (W, H), 0)
    ds = ImageDraw.Draw(street)
    # the streets: every few blocks across and down, some wider
    def lines(n, step_lo, step_hi):
        out, p = [], -int(rng.integers(0, step_hi)) * B
        while p < n + B:
            out.append(p)
            p += int(rng.integers(step_lo, step_hi + 1)) * B
        return out
    xs = lines(W, 3, 6)
    ys = lines(H, 3, 5)
    road = (34, 37, 44)
    walk = (44, 47, 54)
    for x in xs:
        d.rectangle([x - 6 * S, 0, x + B + 6 * S, H], fill=walk)
    for y in ys:
        d.rectangle([0, y - 6 * S, W, y + B + 6 * S], fill=walk)
    # the blocks between: roofs
    roofs = [(30, 33, 40), (36, 39, 47), (40, 42, 50), (33, 30, 34), (28, 32, 36), (45, 44, 48)]
    edges_x = xs + [W + B * 8]
    edges_y = ys + [H + B * 8]
    for i in range(len(edges_x) - 1):
        for j in range(len(edges_y) - 1):
            x0, x1 = edges_x[i] + B + 6 * S, edges_x[i + 1] - 6 * S
            y0, y1 = edges_y[j] + B + 6 * S, edges_y[j + 1] - 6 * S
            if x1 <= x0 or y1 <= y0:
                continue
            if rng.random() < 0.12:                   # a park
                d.rectangle([x0, y0, x1, y1], fill=(24, 34, 28))
                for _ in range(int((x1 - x0) * (y1 - y0) / (B * B) * 5)):
                    cx, cy = rng.integers(x0, x1), rng.integers(y0, y1)
                    r = int(rng.integers(5, 12)) * S
                    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(28, 44, 33))
                continue
            # buildings: the block cut into lots
            y = y0
            while y < y1:
                lh = int(rng.integers(1, 3)) * B // 2 + int(rng.integers(0, B // 2))
                x = x0
                while x < x1:
                    lw = int(rng.integers(1, 4)) * B // 2 + int(rng.integers(0, B // 2))
                    c = roofs[int(rng.integers(len(roofs)))]
                    bx1, by1 = min(x + lw, x1), min(y + lh, y1)
                    if bx1 - x < 6 * S or by1 - y < 6 * S:
                        x = bx1
                        continue
                    d.rectangle([x, y, bx1 - 2 * S, by1 - 2 * S], fill=c)
                    # a lighter rim and things on the roof
                    d.rectangle([x, y, bx1 - 2 * S, by1 - 2 * S], outline=tuple(min(255, v + 8) for v in c), width=2 * S)
                    for _ in range(int(rng.integers(0, 4))):
                        if bx1 - x < 24 * S or by1 - y < 24 * S:
                            break
                        ux, uy = rng.integers(x + 6 * S, bx1 - 16 * S), rng.integers(y + 6 * S, by1 - 16 * S)
                        us = int(rng.integers(5, 11)) * S
                        d.rectangle([ux, uy, ux + us, uy + us], fill=tuple(max(0, v - 7) for v in c))
                    x = bx1
                y = by1
    # the roads over the pavements, their middle lines
    for x in xs:
        d.rectangle([x, 0, x + B, H], fill=road)
        ds.rectangle([x, 0, x + B, H], fill=255)
    for y in ys:
        d.rectangle([0, y, W, y + B], fill=road)
        ds.rectangle([0, y, W, y + B], fill=255)
    for x in xs:
        for y0 in range(0, H, 28 * S):
            d.rectangle([x + B // 2 - 1 * S, y0, x + B // 2 + 1 * S, y0 + 14 * S], fill=(92, 78, 40))
    for y in ys:
        for x0 in range(0, W, 28 * S):
            d.rectangle([x0, y + B // 2 - 1 * S, x0 + 14 * S, y + B // 2 + 1 * S], fill=(92, 78, 40))
    for x in xs:                                      # crossings: no lines there
        for y in ys:
            d.rectangle([x, y, x + B, y + B], fill=road)
    pic = np.asarray(img, np.float32) / 255
    # cars: a few, each a body and its lights' glow
    glow = np.zeros_like(pic)
    if cars:
        gl = Image.new('RGB', (W, H), (0, 0, 0))
        dg = ImageDraw.Draw(gl)
        car = Image.new('RGB', (W, H), (0, 0, 0))
        mask = Image.new('L', (W, H), 0)
        dc, dm = ImageDraw.Draw(car), ImageDraw.Draw(mask)
        for _ in range(int(W * H / (B * B) / 9)):
            vertical = rng.random() < 0.5
            lane = rng.integers(0, 2)
            if vertical:
                x = xs[int(rng.integers(len(xs)))] + (B // 4 if lane else 3 * B // 4)
                y = int(rng.integers(0, H))
                cw, ch = 14 * S, 26 * S
                box = [x - cw // 2, y - ch // 2, x + cw // 2, y + ch // 2]
                head = (x, box[1] if lane else box[3])
                tail = (x, box[3] if lane else box[1])
            else:
                y = ys[int(rng.integers(len(ys)))] + (3 * B // 4 if lane else B // 4)
                x = int(rng.integers(0, W))
                cw, ch = 26 * S, 14 * S
                box = [x - cw // 2, y - ch // 2, x + cw // 2, y + ch // 2]
                head = (box[2] if lane else box[0], y)
                tail = (box[0] if lane else box[2], y)
            col = [(70, 74, 84), (90, 40, 36), (40, 60, 90), (120, 96, 40), (60, 64, 70)][int(rng.integers(5))]
            dc.rounded_rectangle(box, radius=4 * S, fill=col)
            dm.rounded_rectangle(box, radius=4 * S, fill=255)
            hr, tr = 16 * S, 8 * S
            dg.ellipse([head[0] - hr, head[1] - hr, head[0] + hr, head[1] + hr], fill=(110, 96, 60))
            dg.ellipse([tail[0] - tr, tail[1] - tr, tail[0] + tr, tail[1] + tr], fill=(110, 20, 16))
        m = np.asarray(mask, np.float32)[..., None] / 255
        pic = pic * (1 - m) + np.asarray(car, np.float32) / 255 * m
        glow = np.asarray(gl.filter(ImageFilter.GaussianBlur(10 * S)), np.float32) / 255
    pic = np.clip(pic + glow * 0.8, 0, 1)
    out = Image.fromarray((pic * 255 + 0.5).astype(np.uint8)).resize((w, h), Image.LANCZOS)
    st = street.resize((w, h), Image.BILINEAR)
    return np.asarray(out, np.float32) / 255, np.asarray(st, np.float32) / 255


def background(path, w=2560, h=1440):
    pic, _ = city(w, h)
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    # dark enough for the launcher's text, darker still at the edges
    v = 1 - 0.55 * (((xs - w / 2) / (w / 2)) ** 2 + ((ys - h / 2) / (h / 2)) ** 2) / 2
    pic = pic * 0.85 * v[..., None]
    Image.fromarray((np.clip(pic, 0, 1) * 255 + 0.5).astype(np.uint8)).save(path, optimize=True)
    print(path)


def icons():
    svg = os.path.join(ROOT, 'art', 'joyride.svg')
    for n in (16, 32, 48, 64, 128, 256, 512):
        out = os.path.join(ROOT, 'art', f'joyride-{n}.png')
        subprocess.run(['rsvg-convert', '-w', str(n), '-h', str(n), svg, '-o', out], check=True)
        print(out)


if __name__ == '__main__':
    what = sys.argv[1] if len(sys.argv) > 1 else 'all'
    if what in ('city', 'all'):
        background(os.path.join(ROOT, 'art', 'background.png'))
    if what in ('icon', 'all'):
        icons()
