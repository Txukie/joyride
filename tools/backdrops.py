#!/usr/bin/env python3
"""joyride - the pictures around the menus on wide screens, made from the
player's own copy of each game (nothing of the games is in joyride).

The menus stay 640x480 (4:3); on a wider screen joyride draws a picture
behind them, cropped about its middle to the window (backdrop.c). Made here
from the menu's own art, 32:9 wide so every narrower shape is a crop of it:

  gta1        the rust of GTA's menus (F_UPPER over F_LOWER0) at its warmest,
              darkened, with a city seen from above in it (art.py's), tone
              on tone; the menu a panel on it, a soft shadow round it.
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


def panel_shadow(dist, depth, reach):
    """how much darker (log) the wall is by the menu's panel"""
    return depth * np.exp(-np.maximum(dist - 1, 0) / reach) * (dist > 0)


def wall(gtadata, pattern, base=None, depth=0.11):
    """the menu a panel on a wall of one colour (the menu's own, or base)
    with a motif in it, tone on tone: pattern(xs, ys, x0) -> -1..1, drawn at
    the final size (4 samples a pixel each way), in the menu's pixels as
    units; or the motif already made, an array at the final size"""
    src = menu_picture(gtadata).astype(np.float32) / 255
    if base is None:
        base = src[MENU_H - 2, 2]            # the lower half's flat colour (London's teal)
    out_w, x0 = WIDE, (WIDE - MENU_W) // 2
    if callable(pattern):
        n = SCALE * 4
        ys, xs = np.mgrid[0:MENU_H * n, 0:out_w * n].astype(np.float32)
        shade = pattern((xs + 0.5) / n, (ys + 0.5) / n, x0)
        shade = shade.reshape(MENU_H * SCALE, 4, out_w * SCALE, 4).mean((1, 3))
    else:
        shade = pattern
    ys, xs = np.mgrid[0:MENU_H * SCALE, 0:out_w * SCALE].astype(np.float32)
    ys, xs = (ys + 0.5) / SCALE, (xs + 0.5) / SCALE
    dist = np.where(xs < x0, x0 - xs, np.where(xs >= x0 + MENU_W, xs - (x0 + MENU_W), 0)).astype(np.float32)
    L = np.full(xs.shape, np.log(0.82), np.float32)              # a shade darker than the menu
    L += depth * shade
    L -= 0.55 * (dist / 534.0) ** 1.8                            # falling away to the sides
    L -= 0.35 * ((ys - MENU_H / 2) / (MENU_H / 2)) ** 2 * (dist > 0)   # and to top and bottom
    L -= panel_shadow(dist, 0.75, 14.0)
    out = base[None, None, :] * np.exp(L)[..., None]
    big = np.asarray(Image.fromarray((src * 255 + 0.5).astype(np.uint8)).resize((MENU_W * SCALE, MENU_H * SCALE), Image.LANCZOS),
                     np.float32) / 255
    out[:, x0 * SCALE:(x0 + MENU_W) * SCALE] = big
    return out, True


def streets(seed=1997):
    """a city seen from above, as GTA sees it: art.py's, its light as the
    motif (-1..1), no colour of its own"""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from art import city
    pic, _ = city(WIDE * SCALE, MENU_H * SCALE, block=22 * SCALE, seed=seed, cars=False)
    lum = pic @ np.array([0.3, 0.59, 0.11], np.float32)
    return np.clip((lum - lum.mean()) / (3 * lum.std() + 1e-6), -1, 1)


def gta1_wall(gtadata):
    """GTA's: the dark rust of its menu, the city in it"""
    src = menu_picture(gtadata).astype(np.float32) / 255
    # the rust at its warmest (the menu's orange, darkened): its brightest
    # tenth, as dark as London's teal
    lum = src @ np.array([0.3, 0.59, 0.11], np.float32)
    warm = src[(lum > np.quantile(lum, 0.9)) & (src[..., 0] > src[..., 2] * 1.6)]
    hue = warm.mean(0) if len(warm) else np.array([0.6, 0.32, 0.12], np.float32)
    base = hue * (0.17 / float(hue @ np.array([0.3, 0.59, 0.11])))
    return wall(gtadata, streets(), base=base, depth=0.40)


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
    'gta1': gta1_wall,
    'london1969': lambda d: wall(d, roundels),
    'london1961': lambda d: wall(d, houndstooth),
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
