#!/usr/bin/env python3
"""Test images for the Image Formats Plug-In (tests/test_images.c), written byte by byte from the
formats' published specifications with nothing but Python's standard library, so the tests own
every byte. ./make.py writes them here; ./make.py --mac DIR also writes the larger set and the
HTML page used to try them on a Mac.

Each image is drawn from the same test card (card() below): a gray ramp left to right, a black
ring, and a white square in the middle, so a decoder's output can be checked against it."""
import os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))


# ---- the test card ------------------------------------------------------------------

def card(x, y, w, h):
    """0 (black) to 255 (white)"""
    cx, cy, r = w / 2, h / 2, min(w, h) * 0.4
    d = ((x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2) ** 0.5
    if abs(x + 0.5 - cx) < r * 0.3 and abs(y + 0.5 - cy) < r * 0.3:
        return 255
    if abs(d - r) < max(1.5, r * 0.08):
        return 0
    return int(255 * x / max(1, w - 1))


def color(x, y, w, h):
    g = card(x, y, w, h)
    return (g, (g + 85 * (y * 3 // max(1, h))) % 256 if 0 < g < 255 else g, 255 - g if 0 < g < 255 else g)


def lum(r, g, b):
    return (r * 77 + g * 151 + b * 28) >> 8


def bits(w, h):
    """rows of 0/1, 1 black"""
    return [[1 if card(x, y, w, h) < 128 else 0 for x in range(w)] for y in range(h)]


def pack_row(row, pad):
    """1-bit pixels to bytes, high bit first, padded to pad bytes"""
    out = bytearray()
    for i in range(0, len(row), 8):
        v = 0
        for k, b in enumerate(row[i:i + 8]):
            v |= b << (7 - k)
        out.append(v)
    while len(out) % pad:
        out.append(0)
    return bytes(out)


# ---- PackBits (Apple Technical Note 171) ---------------------------------------------

def packbits(data, unit=1):
    units = [data[i:i + unit] for i in range(0, len(data), unit)]
    out, i = bytearray(), 0
    while i < len(units):
        j = i
        while j + 1 < len(units) and units[j + 1] == units[i] and j - i < 127:
            j += 1
        if j > i:
            out.append(257 - (j - i + 1))
            out += units[i]
            i = j + 1
            continue
        j = i
        while j < len(units) and j - i < 128 and not (j + 1 < len(units) and units[j + 1] == units[j]):
            j += 1
        j = max(j, i + 1)
        out.append(j - i - 1)
        for u in units[i:j]:
            out += u
        i = j
    return bytes(out)


# ---- MacPaint (Apple Technical Note 86) -----------------------------------------------

def macpaint():
    rows = bits(576, 720)
    out = bytearray(struct.pack('>I', 2)) + bytes(38 * 8) + bytes(204)
    for row in rows:
        out += packbits(pack_row(row, 1))
    return bytes(out)


# ---- XBM (Xlib, XReadBitmapFile) -----------------------------------------------------

def xbm(w, h, x10=False):
    rows = bits(w, h)
    n = 16 if x10 else 8
    vals = []
    for row in rows:
        for i in range(0, w, n):
            v = 0
            for k, b in enumerate(row[i:i + n]):
                v |= b << k
            vals.append(v)
    fmt = '0x%04x' if x10 else '0x%02x'
    body = ',\n   '.join(', '.join(fmt % v for v in vals[i:i + 12]) for i in range(0, len(vals), 12))
    return ('/* a test card */\n#define card_width %d\n#define card_height %d\n#define card_x_hot 1\n'
            'static %s card_bits[] = {\n   %s};\n' % (w, h, 'short' if x10 else 'unsigned char', body)).encode()


# ---- Netpbm --------------------------------------------------------------------------

def pnm(kind, w, h):
    head = b'P%d\n# a test card\n%d %d\n' % (kind, w, h)
    if kind in (1, 4):
        rows = bits(w, h)
        if kind == 1:
            return head + b'\n'.join(b' '.join(b'%d' % b for b in r) for r in rows) + b'\n'
        return head + b''.join(pack_row(r, 1) for r in rows)
    head += b'255\n'
    if kind == 2:
        return head + b'\n'.join(b' '.join(b'%d' % card(x, y, w, h) for x in range(w)) for y in range(h)) + b'\n'
    if kind == 5:
        return head + bytes(card(x, y, w, h) for y in range(h) for x in range(w))
    px = [color(x, y, w, h) for y in range(h) for x in range(w)]
    if kind == 3:
        return head + b'\n'.join(b'%d %d %d' % p for p in px) + b'\n'
    return head + b''.join(bytes(p) for p in px)


# ---- BMP (Windows 3.1 SDK; OS/2 1.x's BITMAPCOREHEADER) ------------------------------

def bmp(w, h, nbits, comp=0, os2=False, topdown=False, delta=False):
    if nbits <= 8:
        n = 1 << nbits
        pal = [255 * i // (n - 1) for i in range(n)]       # a gray ramp, index 0 black
        def index(x, y):
            return card(x, y, w, h) * (n - 1) // 255 if nbits > 1 else (0 if card(x, y, w, h) < 128 else 1)
    rows = []
    order = range(h) if topdown else range(h - 1, -1, -1)
    if comp == 0:
        for y in order:
            if nbits == 24:
                r = b''.join(bytes(reversed(color(x, y, w, h))) for x in range(w))
            else:
                idx = [index(x, y) for x in range(w)]
                if nbits == 8:
                    r = bytes(idx)
                elif nbits == 4:
                    r = bytes((idx[i] << 4) | (idx[i + 1] if i + 1 < w else 0) for i in range(0, w, 2))
                else:
                    r = pack_row(idx, 1)
            rows.append(r + bytes((-len(r)) % 4))
        data = b''.join(rows)
    else:
        data = bytearray()
        for yi, y in enumerate(order):
            idx = [index(x, y) for x in range(w)]
            x = 3 if delta and yi == 3 else 0
            if delta and yi == 2:
                data += bytes([0, 2, 3, 1])       # 3 right and a row up: this row stays white, and the next one's first 3
                continue
            while x < w:
                v = idx[x]
                j = x
                while j < w and idx[j] == v and j - x < 255:
                    j += 1
                if j - x == 1 and x + 4 <= w:
                    # an absolute run of the next 4
                    run = idx[x:x + 4]
                    data += bytes([0, 4])
                    if comp == 1:
                        data += bytes(run)
                    else:
                        data += bytes([(run[0] << 4) | run[1], (run[2] << 4) | run[3]])
                    if len(data) % 2:
                        data.append(0)
                    x += 4
                    continue
                data += bytes([j - x, v if comp == 1 else (v << 4) | v])
                x = j
            data += bytes([0, 0])
        data += bytes([0, 1])
        data = bytes(data)
    if os2:
        info = struct.pack('<IHHHH', 12, w, h, 1, nbits)
        palette = b''.join(bytes([v, v, v]) for v in pal) if nbits <= 8 else b''
    else:
        info = struct.pack('<IiiHHIIiiII', 40, w, -h if topdown else h, 1, nbits, comp, len(data), 2835, 2835,
                           (1 << nbits) if nbits <= 8 else 0, 0)
        palette = b''.join(bytes([v, v, v, 0]) for v in pal) if nbits <= 8 else b''
    off = 14 + len(info) + len(palette)
    return b'BM' + struct.pack('<IHHI', off + len(data), 0, 0, off) + info + palette + data


# ---- PICT (Inside Macintosh I and V; Imaging With QuickDraw, appendix A) ------------------

def rect(t, l, b, r):
    return struct.pack('>hhhh', t, l, b, r)


def pict_v1(w, h):
    """lines, a rectangle, text, and a packed bitmap: the System draws it"""
    bw, bh = w // 2, h // 2
    rows = bits(bw, bh)
    rb = (bw + 15) // 16 * 2
    ops = bytearray()
    ops += b'\x11\x01'                                     # version 1
    ops += b'\xa0\x00\x82'                                 # ShortComment
    ops += b'\x01' + struct.pack('>h', 10) + rect(0, 0, h, w)   # ClipRgn
    ops += b'\x30' + rect(2, 2, h - 2, w - 2)              # frameRect
    ops += b'\x20' + struct.pack('>hhhh', 4, 4, h - 4, w - 4)   # Line
    ops += b'\x03\x00\x14' + b'\x0d\x00\x0c'               # TxFont Times, TxSize 12
    ops += b'\x28' + struct.pack('>hh', 20, 8) + bytes([4]) + b'Mill'   # LongText
    ops += b'\x98' + struct.pack('>H', rb) + rect(0, 0, bh, bw) + rect(0, 0, bh, bw)
    ops += rect(h // 2, w // 2, h // 2 + bh, w // 2 + bw) + struct.pack('>h', 0)   # srcCopy
    for r in rows:
        p = packbits(pack_row(r, 2)) if rb >= 8 else pack_row(r, 2)
        ops += (bytes([len(p)]) if rb <= 250 else struct.pack('>H', len(p))) + p if rb >= 8 else p
    ops += b'\xff'
    pic = struct.pack('>H', (10 + len(ops)) & 0xFFFF) + rect(0, 0, h, w) + bytes(ops)
    return bytes(512) + pic


def pict_v2(w, h, extended=False, words=False):
    """an 8-bit indexed pixel map, a 32-bit and a 16-bit direct one, a bitmap, and things to skip"""
    ops = bytearray()

    def op(n, data=b''):
        if len(ops) % 2:
            ops.append(0)
        ops.extend(struct.pack('>H', n) + data)

    def rows_packed(rows, rb, unit=1):
        # a byte count, or a word over 250 bytes a row (words: from 201, as some writers do)
        out = bytearray()
        for r in rows:
            p = packbits(r, unit)
            out += (bytes([len(p)]) if rb <= (200 if words else 250) else struct.pack('>H', len(p))) + p
        return bytes(out)

    def pixmap(rb, bnd, pack_type, pixel_type, pixel_size, cmp_count, cmp_size):
        return (struct.pack('>H', 0x8000 | rb) + rect(*bnd) + struct.pack('>hhIIIhhhhIII', 0, pack_type, 0, 72 << 16, 72 << 16,
                pixel_type, pixel_size, cmp_count, cmp_size, 0, 0, 0))

    # with an extended header, the opcodes are at 144 dpi: twice the frame
    s = 2 if extended else 1
    W, H = w * s, h * s
    op(0x0011, b'\x02\xff')
    if extended:
        op(0x0C00, struct.pack('>hhII', -2, 0, 144 << 16, 144 << 16) + rect(0, 0, H, W) + bytes(4))
    else:
        op(0x0C00, struct.pack('>hh', -1, -1) + struct.pack('>iiii', 0, 0, W << 16, H << 16) + bytes(4))
    op(0x001E)
    op(0x0001, struct.pack('>h', 10) + rect(0, 0, H, W))
    op(0x00A1, struct.pack('>hh', 100, 5) + b'hello')
    op(0x001A, struct.pack('>HHH', 0, 0, 0))
    op(0x0030, rect(1, 1, H - 1, W - 1))
    op(0x0028, struct.pack('>hh', 12, 4) + bytes([5]) + b'Hello')
    op(0x0071, struct.pack('>h', 18) + rect(0, 0, 9, 9) + struct.pack('>hhhh', 0, 0, 9, 9))   # a polygon
    # top left: an 8-bit gray ramp pixel map, packed, through a color table (index 0 white)
    qw, qh = W // 2, H // 2
    rb = qw + (qw & 1)
    ct = struct.pack('>IhH', 0, 0, 255) + b''.join(struct.pack('>HHHH', i, (255 - i) * 257, (255 - i) * 257, (255 - i) * 257) for i in range(256))
    rows = [bytes(255 - card(x, y, qw, qh) for x in range(qw)) + bytes(rb - qw) for y in range(qh)]
    op(0x0098, pixmap(rb, (0, 0, qh, qw), 0, 0, 8, 1, 8) + ct + rect(0, 0, qh, qw) + rect(0, 0, qh, qw) + struct.pack('>h', 0) +
       rows_packed(rows, rb))
    # top right: 32-bit direct, packed by component (red, green, blue planes)
    rows = []
    for y in range(qh):
        px = [color(x, y, qw, qh) for x in range(qw)]
        rows.append(bytes(p[0] for p in px) + bytes(p[1] for p in px) + bytes(p[2] for p in px))
    op(0x009A, struct.pack('>I', 0xFF) + pixmap(qw * 4, (0, 0, qh, qw), 4, 16, 32, 3, 8) +
       rect(0, 0, qh, qw) + rect(0, qw, qh, 2 * qw) + struct.pack('>h', 0) + rows_packed(rows, qw * 4))
    # bottom left: 16-bit direct, packed by word, through a mask region
    rows = []
    for y in range(qh):
        r = bytearray()
        for x in range(qw):
            c = color(x, y, qw, qh)
            r += struct.pack('>H', ((c[0] >> 3) << 10) | ((c[1] >> 3) << 5) | (c[2] >> 3))
        rows.append(bytes(r))
    op(0x009B, struct.pack('>I', 0xFF) + pixmap(qw * 2, (0, 0, qh, qw), 3, 16, 16, 3, 5) +
       rect(0, 0, qh, qw) + rect(qh, 0, 2 * qh, qw) + struct.pack('>h', 0) + struct.pack('>h', 10) + rect(qh, 0, 2 * qh, qw) +
       rows_packed(rows, qw * 2, 2))
    # bottom right: a 1-bit bitmap at half size, stretched to fill the quarter, unpacked
    bw, bh = qw // 2, qh // 2
    brb = (bw + 15) // 16 * 2
    rows = [pack_row(r, 2) for r in bits(bw, bh)]
    op(0x0090, struct.pack('>H', brb) + rect(0, 0, bh, bw) + rect(0, 0, bh, bw) + rect(qh, qw, 2 * qh, 2 * qw) +
       struct.pack('>h', 0) + b''.join(rows))
    op(0x0020, struct.pack('>hhhh', 0, 0, H, W))
    op(0x00FF)
    pic = struct.pack('>H', 0) + rect(0, 0, h, w) + bytes(ops)
    return bytes(512) + pic


# ---- the sets ------------------------------------------------------------------------

def unit_set():
    t = {}
    t['card.mac'] = macpaint()
    t['card11.xbm'] = xbm(37, 21)
    t['card10.xbm'] = xbm(37, 21, x10=True)
    for k in range(1, 7):
        t['card-p%d.pnm' % k] = pnm(k, 23, 17)
    t['card1.bmp'] = bmp(37, 21, 1)
    t['card4.bmp'] = bmp(37, 21, 4)
    t['card8.bmp'] = bmp(37, 21, 8, topdown=True)
    t['card24.bmp'] = bmp(37, 21, 24)
    t['card-os2.bmp'] = bmp(37, 21, 8, os2=True)
    t['card-rle8.bmp'] = bmp(37, 21, 8, comp=1, delta=True)
    t['card-rle4.bmp'] = bmp(37, 21, 4, comp=2)
    t['card-v1.pict'] = pict_v1(48, 32)
    t['card-v2.pict'] = pict_v2(48, 32)
    t['card-v2x.pict'] = pict_v2(48, 32, extended=True)
    t['card-wide.pict'] = pict_v2(420, 16, words=True)
    return t


def mac_set():
    """Larger copies for trying on a Mac, broken ones, and a page with them all"""
    t = {}
    t['paint.mac'] = macpaint()
    t['v1.pict'] = pict_v1(200, 120)
    t['v2.pict'] = pict_v2(200, 120)
    t['x11.xbm'] = xbm(120, 80)
    t['rgb.bmp'] = bmp(160, 100, 24)
    t['rle8.bmp'] = bmp(160, 100, 8, comp=1)
    t['rle4.bmp'] = bmp(160, 100, 4, comp=2)
    t['os2.bmp'] = bmp(160, 100, 1, os2=True)
    t['gray.pgm'] = pnm(5, 160, 100)
    t['bw.pbm'] = pnm(4, 160, 100)
    t['rgb.ppm'] = pnm(6, 160, 100)
    t['big.pgm'] = pnm(5, 1600, 1200)
    t['big.bmp'] = bmp(1200, 900, 1)
    # broken: cut short, or a wrong number where it counts
    t['bad.mac'] = t['paint.mac'][:300]
    t['bad.pict'] = t['v1.pict'][:540] + b'\x99\x99' + bytes(40)
    t['bad2.pict'] = t['v2.pict'][:560] + b'\x00\x9a\x00\x00\x00\xff\x7f\xff' + bytes(30)
    t['bad.xbm'] = b'#define bad_width 9\nstatic char bad_bits[] = { 0x1, zap };\n'
    t['bad.bmp'] = t['rgb.bmp'][:18] + struct.pack('<I', 7) + bytes(30)
    t['bad.pgm'] = b'P5\n-3 2\n255\n'
    return t


PAGE = '''<HTML><HEAD><TITLE>Image Formats</TITLE></HEAD><BODY>
<H2>Image Formats</H2>
<P>%s
<H3>Broken</H3>
<P>%s
<H3>Large, reduced</H3>
<P><IMG SRC="big.pgm"> <IMG SRC="big.bmp">
</BODY></HTML>
'''


def main():
    if len(sys.argv) > 2 and sys.argv[1] == '--mac':
        out = sys.argv[2]
        os.makedirs(out, exist_ok=True)
        t = mac_set()
        good = [n for n in t if not n.startswith('bad') and not n.startswith('big')]
        page = PAGE % ('\n'.join('<A HREF="%s"><IMG SRC="%s" ALT="%s"></A>' % (n, n, n) for n in good),
                       '\n'.join('<IMG SRC="%s" ALT="%s">' % (n, n) for n in t if n.startswith('bad')))
        t['index.html'] = page.encode()
    else:
        out, t = HERE, unit_set()
    for name, data in t.items():
        with open(os.path.join(out, name), 'wb') as f:
            f.write(data)
    print('%d files in %s' % (len(t), out))


if __name__ == '__main__':
    main()
