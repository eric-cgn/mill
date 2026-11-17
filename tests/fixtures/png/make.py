#!/usr/bin/env python3
"""Test files for the Image Formats Plug-In's PNG decoder (tests/test_png.c): *.png here, each
with a *.ref.gz: its
width, height, and whether it has a mask, on a line, then the grays and the opacity png.c
should give for it, worked out from what PIL decodes.

Most are written here, by hand (zlib and struct), so every color type, bit depth, filter,
and Adam7 come up, on purpose: each row's filter is chosen in turn, all five. A few are
PIL's own. Run it with Python 3 and PIL (Pillow); the files it writes are checked in, so
the tests need neither. ./make.py writes them here.

list.txt has a line a file: its name, what the decoder must do with it, and the
.ref.gz that goes with it (an interlaced file shares its plain twin's):
  ok     decode, and match the .ref.gz exactly
  bad    end in PNG_BAD (as the file is cut: before any pixels, or the rows that came)
  crc    match the .ref with CRCs unchecked; PNG_BAD with them checked
  cut    the file cut short: PNG_DONE at its end; not interlaced, each row either the
         .ref's or white, and at least one the .ref's
  any    anything but a crash (deflate data spoilt in the middle)
  large  PNG_TOO_LARGE
"""
import gzip
import os
import struct
import sys
import zlib

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = HERE
ADAM7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]
CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
ALPHAS = [0, 64, 127, 128, 200, 255, 255, 255]


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def card(x, y, w, h):
    """An 8-bit test card: a gradient left to right, a dark ring, a white square"""
    cx, cy, r = w / 2.0, h / 2.0, min(w, h) * 0.4
    dx, dy = x + 0.5 - cx, y + 0.5 - cy
    d = (dx * dx + dy * dy) ** 0.5
    if abs(dx) < r * 0.3 and abs(dy) < r * 0.3:
        return 255
    if abs(d - r) < max(r * 0.12, 1.5):
        return 20
    return int(255 * x / max(w - 1, 1))


def noise(x, y, k=0):
    return (x * 73 + y * 151 + k * 29 + (x * y) % 7) & 255


class Pic:
    """A picture's samples at the file's depth, and what each pixel should come out as"""

    def __init__(self, w, h, ctype, depth, trns=False):
        self.w, self.h, self.ctype, self.depth = w, h, ctype, depth
        self.palette = None
        self.trns = None
        n = 1 << depth if depth < 16 else 65536
        top = n - 1
        px = []
        if ctype == 3:
            count = min(n, 200)
            self.palette = [((i * 67 + 13) & 255, (i * 151 + 7) & 255, (i * 29 + 200) & 255) for i in range(count)]
            self.palette[0] = (0, 0, 0)
            self.palette[-1] = (255, 255, 255)
            if trns:
                self.trns = bytes(ALPHAS[i % len(ALPHAS)] for i in range(min(count, 6)))
        for y in range(h):
            row = []
            for x in range(w):
                c = card(x, y, w, h)
                if ctype == 0:
                    v = c * top // 255 if depth < 16 else (c << 8) | noise(x, y)
                    row.append((v,))
                elif ctype == 2:
                    r, g, b = c, (x * 255 // max(w - 1, 1)) ^ (y * 9 & 255), 255 - c
                    if depth == 16:
                        row.append(((r << 8) | noise(x, y), (g << 8) | noise(x, y, 1), (b << 8) | noise(x, y, 2)))
                    else:
                        row.append((r, g, b))
                elif ctype == 3:
                    k = len(self.palette)
                    row.append(((x * 3 + y * 5 + c // 32) % k,))
                elif ctype == 4:
                    a = ALPHAS[(x + 2 * y) % len(ALPHAS)]
                    if depth == 16:
                        row.append(((c << 8) | noise(x, y), (a << 8) | noise(x, y, 3)))
                    else:
                        row.append((c, a))
                elif ctype == 6:
                    r, g, b = c, (y * 255 // max(h - 1, 1)), (x * 37) & 255
                    a = ALPHAS[(x + 3 * y) % len(ALPHAS)]
                    if depth == 16:
                        row.append(tuple((v << 8) | noise(x, y, i) for i, v in enumerate((r, g, b, a))))
                    else:
                        row.append((r, g, b, a))
            px.append(row)
        if trns and ctype in (0, 2):
            # a color that's there, made transparent; for 16 bits, near misses that agree
            # in the high byte stay opaque
            self.trns = px[h // 2][w // 3] if ctype == 0 else (0x1122 if depth == 16 else 0x11, 0x3344 if depth == 16 else 0x33,
                                                                0x5566 if depth == 16 else 0x55)
            if ctype == 2:
                for y in range(h):
                    for x in range(w):
                        if (x + y) % 5 == 0:
                            px[y][x] = self.trns
                        elif depth == 16 and (x + y) % 5 == 1:
                            px[y][x] = (0x1123, 0x3344, 0x5566)
            if ctype == 0 and depth == 16:
                for y in range(0, h, 2):
                    px[y][0] = (self.trns[0] ^ 1,)
        self.px = px

    def expect(self):
        """What png.c should give: grays, opacity, and whether there's a mask at all"""
        gray, op = bytearray(), bytearray()
        mask = self.ctype in (4, 6) or self.trns is not None
        if self.ctype == 3 and self.trns is not None and all(a == 255 for a in self.trns):
            mask = False
        top = (1 << self.depth) - 1
        for row in self.px:
            for s in row:
                a = 255
                if self.ctype == 0:
                    v = s[0] * 255 // top if self.depth < 16 else s[0] >> 8
                    if self.trns is not None and s == self.trns:
                        a = 0
                elif self.ctype == 2:
                    r, g, b = (c >> 8 for c in s) if self.depth == 16 else s
                    v = (r * 77 + g * 151 + b * 28) >> 8
                    if self.trns is not None and s == tuple(self.trns):
                        a = 0
                elif self.ctype == 3:
                    r, g, b = self.palette[s[0]]
                    v = (r * 77 + g * 151 + b * 28) >> 8
                    if self.trns is not None and s[0] < len(self.trns):
                        a = self.trns[s[0]]
                elif self.ctype == 4:
                    v, a = (c >> 8 for c in s) if self.depth == 16 else s
                else:
                    r, g, b, a = (c >> 8 for c in s) if self.depth == 16 else s
                    v = (r * 77 + g * 151 + b * 28) >> 8
                g8, o = over(v, a)
                gray.append(g8)
                op.append(o)
        return gray, op, mask


def over(v, a):
    if a == 255:
        return v, 1
    if a >= 128:
        return 255 - ((255 - v) * a + 127) // 255, 1      # rounded to the nearest
    return 255, 0


def pack(pic, xs, ys, dx, dy):
    """The raw rows of one pass (or the whole picture), unfiltered"""
    rows = []
    d = pic.depth
    for y in range(ys, pic.h, dy):
        bits, n, out = 0, 0, bytearray()
        for x in range(xs, pic.w, dx):
            for v in pic.px[y][x]:
                if d == 16:
                    out += struct.pack(">H", v)
                elif d == 8:
                    out.append(v)
                else:
                    bits = (bits << d) | v
                    n += d
                    if n == 8:
                        out.append(bits)
                        bits = n = 0
        if n:
            out.append(bits << (8 - n))
        if out:
            rows.append(bytes(out))
    return rows


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c


def filtered(rows, bpp, first=0):
    """Each row with a filter byte, the filters taken in turn"""
    out = bytearray()
    prev = bytes(len(rows[0])) if rows else b""
    for i, row in enumerate(rows):
        f = (i + first) % 5
        out.append(f)
        for j, v in enumerate(row):
            a = row[j - bpp] if j >= bpp else 0
            b = prev[j]
            c = prev[j - bpp] if j >= bpp else 0
            pred = [0, a, b, (a + b) // 2, paeth(a, b, c)][f]
            out.append((v - pred) & 255)
        prev = row
    return bytes(out)


def encode(pic, interlace=0, level=9, idat=None, before_idat=b"", after_idat=b"", filters=0):
    bpp = max(1, pic.depth * CHANNELS[pic.ctype] // 8)
    if interlace:
        raw = b"".join(filtered(pack(pic, *p), bpp, filters + k) for k, p in enumerate(ADAM7))
    else:
        raw = filtered(pack(pic, 0, 0, 1, 1), bpp, filters)
    z = zlib.compress(raw, level)
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", pic.w, pic.h, pic.depth, pic.ctype, 0, 0, interlace))
    if pic.palette:
        out += chunk(b"PLTE", bytes(c for rgb in pic.palette for c in rgb))
    if pic.trns is not None:
        if pic.ctype == 3:
            out += chunk(b"tRNS", pic.trns)
        else:
            out += chunk(b"tRNS", b"".join(struct.pack(">H", v) for v in (pic.trns if pic.ctype == 2 else pic.trns[:1])))
    out += before_idat
    step = idat or len(z) or 1
    for i in range(0, max(len(z), 1), step):
        out += chunk(b"IDAT", z[i:i + step])
    return out + after_idat + chunk(b"IEND", b"")


def check_with_pil(path, pic):
    """PIL must decode the file to the same samples (at 8 bits) we meant"""
    im = Image.open(path)
    im.load()
    w, h = im.size
    assert (w, h) == (pic.w, pic.h), path
    data = list(im.get_flattened_data() if hasattr(im, "get_flattened_data") else im.getdata())
    top = (1 << pic.depth) - 1
    for y in range(h):
        for x in range(w):
            got = data[y * w + x]
            s = pic.px[y][x]
            if pic.ctype == 0:
                want = s[0] >> 8 if pic.depth == 16 else s[0] * 255 // top
                if im.mode in ("I", "I;16"):
                    got = got >> 8
                assert got == want, (path, x, y, got, want, im.mode)
            elif pic.ctype == 3:
                assert got == s[0], (path, x, y, got, s)
            else:
                want = tuple(c >> 8 for c in s) if pic.depth == 16 else tuple(s)
                if pic.ctype == 4 and im.mode == "RGBA":
                    assert got[0] == got[1] == got[2], (path, got)
                    got = (got[0], got[3])
                assert tuple(got) == want, (path, x, y, got, want, im.mode)
    if pic.ctype == 3:
        pal = im.getpalette()
        for i, rgb in enumerate(pic.palette):
            assert tuple(pal[3 * i:3 * i + 3]) == rgb, path
        if pic.trns is not None:
            t = im.info.get("transparency")
            t = bytes([t]) if isinstance(t, int) else t
            assert t[:len(pic.trns)] == pic.trns, path


def write_ref(name, w, h, gray, op, mask):
    with gzip.GzipFile(os.path.join(OUT, name + ".ref.gz"), "wb", mtime=0) as f:
        f.write(b"%d %d %d\n" % (w, h, mask))
        f.write(bytes(gray))
        f.write(bytes(op))


def main():
    os.makedirs(OUT, exist_ok=True)
    listing = []

    def add(name, data, kind, pic=None, ref=None):
        path = os.path.join(OUT, name + ".png")
        with open(path, "wb") as f:
            f.write(data)
        if pic is not None:
            check_with_pil(path, pic)
            if not ref:
                write_ref(name, pic.w, pic.h, *pic.expect())
        listing.append("%s %s %s" % (name, kind, ref or name))

    kinds = [(0, 1), (0, 2), (0, 4), (0, 8), (0, 16), (2, 8), (2, 16), (3, 1), (3, 2), (3, 4), (3, 8), (4, 8), (4, 16), (6, 8), (6, 16)]
    for il in (0, 1):
        for ct, d in kinds:
            name = "c%d-%d%s" % (ct, d, "-i" if il else "")
            pic = Pic(37, 23, ct, d)
            add(name, encode(pic, il, filters=ct + d), "ok", pic, name[:-2] if il else None)
        for ct, d in [(0, 1), (0, 2), (0, 8), (0, 16), (2, 8), (2, 16), (3, 2), (3, 8)]:
            name = "t%d-%d%s" % (ct, d, "-i" if il else "")
            pic = Pic(29, 19, ct, d, trns=True)
            add(name, encode(pic, il, filters=d), "ok", pic, name[:-2] if il else None)
    # Adam7's corners: passes with nothing in them, one column, one row
    for w, h, ct, d in [(1, 1, 6, 8), (1, 1, 0, 1), (2, 2, 2, 8), (1, 9, 6, 8), (1, 9, 0, 8), (9, 1, 4, 8), (5, 3, 3, 4),
                        (3, 5, 2, 16), (8, 8, 0, 8), (9, 9, 6, 16), (16, 3, 0, 2), (2, 17, 3, 1)]:
        pic = Pic(w, h, ct, d)
        add("e%dx%d-%d-%d" % (w, h, ct, d), encode(pic, 0, filters=3), "ok", pic)
        add("e%dx%d-%d-%d-i" % (w, h, ct, d), encode(pic, 1), "ok", pic, "e%dx%d-%d-%d" % (w, h, ct, d))
    # deflate's stored blocks; IDAT a byte at a time; chunks to skip on the way
    pic = Pic(40, 30, 2, 8)
    add("stored", encode(pic, 0, level=0), "ok", pic)
    add("stored-i", encode(pic, 1, level=0), "ok", pic, "stored")
    extra = chunk(b"gAMA", struct.pack(">I", 45455)) + chunk(b"tEXt", b"Comment\0made by make.py") + chunk(b"bKGD", b"\0\1\0\2\0\3")
    add("ancillary", encode(pic, 0, before_idat=extra, after_idat=chunk(b"tIME", b"\x07\xd0\x01\x01\x00\x00\x00")), "ok", pic, "stored")
    pic = Pic(16, 12, 6, 8)
    add("tiny-idat", encode(pic, 1, idat=1), "ok", pic)
    # PIL's own files
    pic = Pic(48, 32, 6, 8)
    im = Image.new("RGBA", (pic.w, pic.h))
    im.putdata([p for row in pic.px for p in row])
    im.save(os.path.join(OUT, "pil-rgba.png"), optimize=True)
    add("pil-rgba", open(os.path.join(OUT, "pil-rgba.png"), "rb").read(), "ok", pic)
    pic = Pic(48, 32, 3, 8, trns=True)
    im = Image.new("P", (pic.w, pic.h))
    im.putpalette([c for rgb in pic.palette for c in rgb])
    im.putdata([p[0] for row in pic.px for p in row])
    im.save(os.path.join(OUT, "pil-p.png"), transparency=pic.trns)
    add("pil-p", open(os.path.join(OUT, "pil-p.png"), "rb").read(), "ok", pic)
    # a typical page picture, for timing: 200 x 150, truecolor and palette, plain and interlaced
    for ct, d in [(2, 8), (3, 8), (6, 8)]:
        pic = Pic(200, 150, ct, d)
        add("big-%d" % ct, encode(pic, 0), "ok", pic)
        add("big-%d-i" % ct, encode(pic, 1), "ok", pic, "big-%d" % ct)

    # broken files
    pic = Pic(37, 23, 2, 8)
    good = encode(pic, 0)
    write_ref("broken", pic.w, pic.h, *pic.expect())
    add("bad-sig", b"\x89PNG\r\n\x1a\x0b" + good[8:], "bad")
    add("bad-critical", good[:33] + chunk(b"ABCD", b"xyz") + good[33:], "bad")
    add("bad-first", good[:8] + chunk(b"gAMA", struct.pack(">I", 45455)) + good[8:], "bad")
    add("bad-depth", good[:8] + chunk(b"IHDR", struct.pack(">IIBBBBB", 5, 5, 4, 2, 0, 0, 0)) + good[33:], "bad")
    add("bad-nopal", encode(Pic(9, 9, 3, 8), 0).replace(b"PLTE", b"pLTE"), "bad")
    add("bad-zlib", good[:41] + b"\x79" + good[42:], "bad")      # zlib's header, CM 9
    raw = bytes([5] + [0] * 3 * 37) * 23                          # filter type 5
    z = zlib.compress(raw)
    add("bad-filter", good[:33] + chunk(b"IDAT", z) + chunk(b"IEND", b""), "bad")
    # deflate data spoilt in the middle: an error, or rows of rubbish, never a crash
    spoilt = bytearray(good)
    for i in range(60, 60 + 40, 3):
        spoilt[i] ^= 0x5A
    add("bad-deflate", bytes(spoilt), "any")
    crc = bytearray(good)
    crc[29] ^= 1        # IHDR's CRC
    add("crc", bytes(crc), "crc", None, "broken")
    # cut short: halfway through IDAT, plain and interlaced
    for il in (0, 1):
        data = encode(pic, il)
        name = "broken-cut%s" % ("-i" if il else "")
        add(name, data[:len(data) // 2], "cut", None, "broken")
    # too large
    add("large-interlaced", b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 600, 600, 8, 0, 0, 0, 1)) +
        chunk(b"IDAT", zlib.compress(b"\0")) + chunk(b"IEND", b""), "large")
    add("large-row", b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 3000, 1, 16, 6, 0, 0, 0)) +
        chunk(b"IDAT", zlib.compress(b"\0")) + chunk(b"IEND", b""), "large")
    add("large-side", b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 40000, 1, 1, 0, 0, 0, 0)) +
        chunk(b"IDAT", zlib.compress(b"\0")) + chunk(b"IEND", b""), "large")
    with open(os.path.join(OUT, "list.txt"), "w") as f:
        f.write("\n".join(listing) + "\n")
    print(len(listing), "files")


if __name__ == "__main__":
    sys.exit(main())
