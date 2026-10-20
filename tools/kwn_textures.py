"""Dump the RenderWare images (chunk 0x18) of XXL's .KWN files as PNGs.

A texture dictionary entry is: name[32], filtering, uAddr, vAddr (u32 each), then an RW Image chunk
(0x18 > struct 0x1: width, height, bpp, pitch; pixels; a palette of 1<<bpp RGBA entries when bpp <= 8).
The files are scanned for that pattern, so no KWN parsing is needed.

usage: python kwn_textures.py OUTDIR FILE.KWN [FILE.KWN ...]   (writes OUTDIR/<file>/<name>.png + index.tsv)
"""
import os
import struct
import sys
import zlib


def png(path, w, h, rgba):
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def images(data):
    pos = 0
    while True:
        i = data.find(b"\x18\x00\x00\x00", pos)
        if i < 0:
            return
        pos = i + 4
        if i < 44 or i + 40 > len(data):
            continue
        t1, s1 = struct.unpack_from("<II", data, i + 12)
        if t1 != 1 or s1 != 16:
            continue
        w, h, bpp, pitch = struct.unpack_from("<IIII", data, i + 24)
        if not (0 < w <= 4096 and 0 < h <= 4096 and bpp in (4, 8, 32) and pitch >= w * bpp // 8):
            continue
        name = data[i - 44:i - 12].split(b"\0")[0].decode("latin-1")
        px = i + 40
        pal = px + pitch * h
        yield name, w, h, bpp, pitch, px, pal
        pos = pal


def rgba(data, w, h, bpp, pitch, px, pal):
    out = bytearray(w * h * 4)
    if bpp == 32:
        for y in range(h):
            row = data[px + y * pitch:px + y * pitch + w * 4]
            out[y * w * 4:(y + 1) * w * 4] = row  # RW images are stored RGBA
        return bytes(out)
    n = 1 << bpp
    p = data[pal:pal + 4 * n]
    for y in range(h):
        for x in range(w):
            if bpp == 8:
                k = data[px + y * pitch + x]
            else:
                b = data[px + y * pitch + x // 2]
                k = (b >> 4) if x & 1 else (b & 15)
            o = (y * w + x) * 4
            out[o:o + 4] = p[k * 4:k * 4 + 4]
    return bytes(out)


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, "index.tsv"), "a", encoding="utf-8") as idx:
        for path in sys.argv[2:]:
            data = open(path, "rb").read()
            tag = os.path.basename(path)
            sub = os.path.join(outdir, tag)
            os.makedirs(sub, exist_ok=True)
            seen = {}
            for name, w, h, bpp, pitch, px, pal in images(data):
                base = "".join(c if c.isalnum() or c in "_-." else "_" for c in name) or "unnamed"
                seen[base] = seen.get(base, 0) + 1
                if seen[base] > 1:
                    base += "_%d" % seen[base]
                png(os.path.join(sub, base + ".png"), w, h, rgba(data, w, h, bpp, pitch, px, pal))
                idx.write("%s\t%s\t%dx%d\t%d\n" % (tag, name, w, h, bpp))


if __name__ == "__main__":
    main()
