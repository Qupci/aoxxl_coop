"""Cuts the game's action icons (the font's glyphs U+E000..U+E00F) out of a language pack's font pages.

The icons are characters of the font "arial24-windows" (CManager2d's font in xxGLOC.KWN, an Rt2d bitmap font,
RW chunk 0x199): the loc strings embed them ("Use \\ue000"). Its two 256x256 pages are images in the same file.

usage: python export_game_icons.py GLOC.KWN OUTDIR     (01GLOC.KWN is English)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwn_textures as kt

NAMES = {
    0xE000: "action", 0xE001: "launch_dogmatix", 0xE002: "jump", 0xE003: "pause", 0xE004: "empty_display_hide_interface",
    0xE005: "up", 0xE006: "down", 0xE007: "right", 0xE008: "left", 0xE009: "targeted_attack", 0xE00A: "grab_enemy",
    0xE00B: "launch_obelix", 0xE00C: "empty_c", 0xE00D: "empty_d", 0xE00E: "confirm", 0xE00F: "cancel",
}


def font(d):
    i = d.find(b"\x99\x01\x00\x00", d.find(b"arial24-windows0.png"))
    p = i + 12
    _, _, _, _, _, _, first_wide, n_glyphs, n_wide = struct.unpack_from("<IIffIIIII", d, p)
    p += 36
    wide = struct.unpack_from("<%dH" % n_wide, d, p)
    p += 2 * n_wide + 256
    glyphs = []
    for _ in range(n_glyphs):
        glyphs.append(struct.unpack_from("<5fB", d, p))
        p += 21
    n_tex = struct.unpack_from("<I", d, p)[0]
    names = [d[p + 4 + 32 * k:p + 36 + 32 * k].split(b"\0")[0].decode() for k in range(n_tex)]
    return first_wide, wide, glyphs, names


def main():
    path, out = sys.argv[1], sys.argv[2]
    d = open(path, "rb").read()
    first_wide, wide, glyphs, names = font(d)
    # The pages' images come right before their texture chunks, in file order: find each name's image.
    pages = {}
    imgs = list(kt.images(d))
    for name in names:
        at = d.find(name.encode())
        before = [im for im in imgs if im[5] < at]
        name_, w, h, bpp, pitch, px, pal = before[-1]
        pages[name] = (w, h, kt.rgba(d, w, h, bpp, pitch, px, pal))
    os.makedirs(os.path.join(out, "font_pages"), exist_ok=True)
    for name, (w, h, px) in pages.items():
        kt.png(os.path.join(out, "font_pages", name), w, h, px)
    for code, label in NAMES.items():
        g = wide[code - first_wide]
        if g == 0xFFFF:
            continue
        u0, v0, u1, v1, _, page = glyphs[g]
        w, h, px = pages[names[page]]
        x0, y0, x1, y1 = int(round(u0 * w)), int(round(v0 * h)), int(round(u1 * w)), int(round(v1 * h))
        cw, ch = x1 - x0, y1 - y0
        cut = b"".join(px[((y0 + y) * w + x0) * 4:((y0 + y) * w + x1) * 4] for y in range(ch))
        if not any(cut[3::4]):
            continue   # an empty cell
        kt.png(os.path.join(out, "U+%04X_%s.png" % (code, label)), cw, ch, cut)
        # 4x, nearest neighbour, for looking at
        big = b"".join(b"".join(cut[(y // 4 * cw + x // 4) * 4:(y // 4 * cw + x // 4) * 4 + 4] for x in range(cw * 4))
                       for y in range(ch * 4))
        os.makedirs(os.path.join(out, "x4"), exist_ok=True)
        kt.png(os.path.join(out, "x4", "U+%04X_%s.png" % (code, label)), cw * 4, ch * 4, big)


if __name__ == "__main__":
    main()
