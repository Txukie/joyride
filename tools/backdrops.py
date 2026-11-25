#!/usr/bin/env python3
"""joyride - the pictures around the menus on wide screens, made from the
player's own copy of each game (nothing of the games is in joyride).

The menus stay 640x480 (4:3); on a wider screen joyride draws a picture
behind them, cropped about its middle to the window (backdrop.c). Made here
from the menu's own art, 32:9 wide so every narrower shape is a crop of it:

  gta1        the rusted wall of GTA's menus (F_UPPER over F_LOWER0) carried
              on: another stretch of the same wall, its fine detail kept and
              its light continued from the menu's edges, falling away to
              the sides; the menu a panel on it, a soft shadow round it.
  london1969  the teal of London's menus, a shade darker, with faint mod
              target roundels; the same panel and shadow.
  london1961  the same teal with a faint houndstooth.

  backdrops.py <game id> <its GTADATA folder> <out.png>
  backdrops.py --all <joyride's data folder>    every game installed there

Needs numpy and Pillow (tools/requirements.txt).
"""
import os
import sys

import numpy as np
from PIL import Image

MENU_W, MENU_H = 640, 480
WIDE = 1708                 # 32:9 at 480 rows
SCALE = 2                   # the picture is made at twice the menu's size


def blur(a, s):
    """Gaussian blur, edges reflected (a: H x W or H x W x C, float)"""
    r = max(1, int(3 * s))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / s) ** 2)
    k /= k.sum()

    def one(x, axis):
        pad = [(0, 0)] * x.ndim
        pad[axis] = (r, r)
        xp = np.pad(x, pad, mode='reflect')
        out = np.zeros_like(x)
        for i, w in enumerate(k):
            sl = [slice(None)] * x.ndim
            sl[axis] = slice(i, i + x.shape[axis])
            out += w * xp[tuple(sl)]
        return out
    return one(one(a, 0), 1)


def find(gtadata, name):
    """a file in GTADATA, any case"""
    for f in os.listdir(gtadata):
        if f.upper() == name.upper():
            return os.path.join(gtadata, f)
    raise FileNotFoundError(f'{name} in {gtadata}')


def raw(gtadata, name):
    """one of the menu's 24-bit pictures, 640 wide"""
    b = open(find(gtadata, name + '.RAW'), 'rb').read()
    h = len(b) // (MENU_W * 3)
    return np.frombuffer(b[:MENU_W * 3 * h], np.uint8).reshape(h, MENU_W, 3)


def menu_picture(gtadata):
    return np.concatenate([raw(gtadata, 'F_UPPER'), raw(gtadata, 'F_LOWER0')])


def distance(out_w, x0):
    """each column's distance (pixels) outside the menu, 0 inside"""
    xs = np.arange(out_w)
    return np.where(xs < x0, x0 - xs, np.where(xs >= x0 + MENU_W, xs - (x0 + MENU_W - 1), 0)).astype(np.float32)


def panel_shadow(dist, depth, reach):
    """how much darker (log) the wall is by the menu's panel"""
    return depth * np.exp(-np.maximum(dist - 1, 0) / reach) * (dist > 0)


def gta1(gtadata):
    eps = 0.06
    src = menu_picture(gtadata).astype(np.float32) / 255
    out_w, x0 = WIDE, (WIDE - MENU_W) // 2
    lg = np.log(src + eps)
    low = blur(lg, 36)                       # the light
    det = lg - low                           # the wall's own detail
    # the sides: another stretch of the wall (upside down, from the far
    # side of the menu), so nothing repeats beside the panel
    n = x0
    turned = det[::-1]
    reach = min(n, MENU_W)
    canvas = np.zeros((MENU_H, out_w, 3), np.float32)
    canvas[:, x0:x0 + MENU_W] = det
    left = turned[:, MENU_W - reach:]
    right = turned[:, :reach]
    if n > reach:                            # wider than the wall: the far end turned again
        left = np.concatenate([det[:, ::-1][:, MENU_W - (n - reach):], left], axis=1)
        right = np.concatenate([right, det[:, ::-1][:, :n - reach]], axis=1)
    canvas[:, :x0] = left[:, -x0:]
    canvas[:, x0 + MENU_W:] = right[:, :out_w - x0 - MENU_W]
    # the light: the menu's edge columns held outward, smoothed, falling away
    dist = distance(out_w, x0)
    L = np.zeros_like(canvas)
    L[:, x0:x0 + MENU_W] = low
    L[:, :x0] = low[:, :8].mean(1)[:, None, :]
    L[:, x0 + MENU_W:] = low[:, -8:].mean(1)[:, None, :]
    Ls = blur(L, 30)
    w = np.clip(dist / 80, 0, 1)[None, :, None]
    L = L * (1 - w) + Ls * w
    L -= (1.1 * (dist / 534.0) ** 1.6)[None, :, None]
    L -= panel_shadow(dist, 0.9, 16.0)[None, :, None]
    out = np.exp(L + canvas) - eps
    out[:, x0:x0 + MENU_W] = src
    return out


def teal_wall(gtadata, pattern):
    """made at the final size (the motif's edges drawn there, 4 samples a
    pixel each way), in the menu's pixels as units"""
    src = menu_picture(gtadata).astype(np.float32) / 255
    base = src[MENU_H - 2, 2]                # the lower half's flat teal
    out_w, x0 = WIDE, (WIDE - MENU_W) // 2
    n = SCALE * 4
    ys, xs = np.mgrid[0:MENU_H * n, 0:out_w * n].astype(np.float32)
    shade = pattern((xs + 0.5) / n, (ys + 0.5) / n, x0)        # -1..1, the motif
    shade = shade.reshape(MENU_H * SCALE, 4, out_w * SCALE, 4).mean((1, 3))
    ys, xs = np.mgrid[0:MENU_H * SCALE, 0:out_w * SCALE].astype(np.float32)
    ys, xs = (ys + 0.5) / SCALE, (xs + 0.5) / SCALE
    dist = np.where(xs < x0, x0 - xs, np.where(xs >= x0 + MENU_W, xs - (x0 + MENU_W), 0)).astype(np.float32)
    L = np.full(xs.shape, np.log(0.82), np.float32)              # a shade darker than the menu
    L += 0.11 * shade
    L -= 0.55 * (dist / 534.0) ** 1.8                            # falling away to the sides
    L -= 0.35 * ((ys - MENU_H / 2) / (MENU_H / 2)) ** 2 * (dist > 0)   # and to top and bottom
    L -= panel_shadow(dist, 0.75, 14.0)
    out = base[None, None, :] * np.exp(L)[..., None]
    big = np.asarray(Image.fromarray((src * 255 + 0.5).astype(np.uint8)).resize((MENU_W * SCALE, MENU_H * SCALE), Image.LANCZOS),
                     np.float32) / 255
    out[:, x0 * SCALE:(x0 + MENU_W) * SCALE] = big
    return out, True


def roundels(xs, ys, x0):
    """mod targets, one each side, half behind the menu"""
    s = np.zeros_like(xs)
    band, rings = 26.0, 7
    for cx, cy in ((x0 - 30, MENU_H * 0.66), (WIDE - x0 + 30, MENU_H * 0.30)):
        r = np.hypot(xs - cx, ys - cy)
        ring = np.floor(r / band)
        s += np.where(r < band * rings, np.where(ring % 2 == 0, 1.0, -1.0), 0)
    return s


def houndstooth(xs, ys, x0):
    """houndstooth: a 4 x 4 check, the mixed squares a twill of it"""
    u = np.floor((xs - x0) / 4.0).astype(int) % 8
    v = np.floor(ys / 4.0).astype(int) % 8
    dark = (u < 4) & (v < 4)
    light = (u >= 4) & (v >= 4)
    twill = np.isin((u + v) % 4, (1, 2))
    return np.where(dark | (~light & twill), 1.0, -1.0)


MAKERS = {
    'gta1': gta1,
    'london1969': lambda d: teal_wall(d, roundels),
    'london1961': lambda d: teal_wall(d, houndstooth),
}


def make(game, gtadata, out_path):
    pic = MAKERS[game](gtadata)
    full = isinstance(pic, tuple)            # made at the final size already
    if full:
        pic = pic[0]
    img = Image.fromarray((np.clip(pic, 0, 1) * 255 + 0.5).astype(np.uint8))
    if not full:
        img = img.resize((img.width * SCALE, img.height * SCALE), Image.LANCZOS)
    os.makedirs(os.path.dirname(out_path) or '.', exist_ok=True)
    tmp = out_path + '.tmp.png'
    img.save(tmp)
    os.replace(tmp, out_path)
    print(f'{game}: {out_path} ({img.width}x{img.height})')


def installed(data):
    """the games installed in joyride's data folder: id -> GTADATA"""
    out = {}
    loc = os.path.join(data, 'games', 'gta1', 'location')
    if os.path.exists(loc):
        root = open(loc).read().strip()
        if os.path.isdir(os.path.join(root, 'GTADATA')):
            out['gta1'] = os.path.join(root, 'GTADATA')
    for g in ('london1969', 'london1961'):
        d = os.path.join(data, 'games', g, 'play', 'GTADATA')
        if os.path.isdir(d):
            out[g] = d
    return out


def main(argv):
    if len(argv) == 3 and argv[1] == '--all':
        data = argv[2]
        for game, gtadata in installed(data).items():
            make(game, gtadata, os.path.join(data, 'extracted', 'menu', f'{game}.png'))
        return 0
    if len(argv) == 4 and argv[1] in MAKERS:
        make(argv[1], argv[2], argv[3])
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
