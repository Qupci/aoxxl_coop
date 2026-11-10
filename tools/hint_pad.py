"""Measures the co-op hint boxes' paddings in an autotest run (XXL_HINTLOG=1).

For each screenshot, the plate last logged before it ("hints: player N's box
plate at x a-b, y c-d") is cut out of the shot; its ink (text and icons: any
channel brighter than INK) gives the left, right, top and bottom padding in
pixels of the shot.

    py -3.11 tools/hint_pad.py testruns/<run>
"""
import os
import re
import sys

from PIL import Image

INK = 175

def main(run):
    log = open(os.path.join(run, 'xxl_coop.log'), encoding='utf-8', errors='replace').read().splitlines()
    plate = None
    rows = []
    for line in log:
        m = re.search(r"player (\d)'s box plate at x ([\d.]+)-([\d.]+), y ([\d.-]+)-([\d.]+)", line)
        if m:
            plate = tuple(float(v) for v in m.groups()[1:])
            continue
        m = re.search(r'screenshot .*\\([^\\]+\.png) saved', line)
        if m and plate:
            rows.append((m.group(1), plate))
    for name, (x0, x1, y0, y1) in rows:
        path = os.path.join(run, name)
        if not os.path.exists(path):
            continue
        im = Image.open(path).convert('RGB')
        w, h = im.size
        L, R = round(x0 * w), round(x1 * w)
        T, B = round((1 - y1) * h), round((1 - y0) * h)
        B = min(B, h - 4)   # (the shots' last rows are a bright strip, not the game's)
        box = im.crop((L + 4, T + 4, R - 4, B - 4))
        px = box.load()
        xs, ys = [], []
        for yy in range(box.height):
            for xx in range(box.width):
                if max(px[xx, yy]) > INK:
                    xs.append(xx)
                    ys.append(yy)
        if not xs:
            print(f'{name:24s} no ink in the plate {L}-{R} x {T}-{B}')
            continue
        il, ir, it, ib = min(xs) + 4, max(xs) + 4, min(ys) + 4, max(ys) + 4
        pw, ph = R - L, B - T
        print(f'{name:24s} plate {pw:3d}x{ph:3d}  pad left {il:3d} right {pw - 1 - ir:3d}  top {it:3d} bottom {ph - 1 - ib:3d}')

if __name__ == '__main__':
    main(sys.argv[1])
