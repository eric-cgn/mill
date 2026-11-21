#!/usr/bin/env python3
'''Web Fonts: builds "Web Fonts", Mill Serif and Mill Mono for Mill: a font suitcase of bitmaps
at 9, 10, 12, and 14 point, from Adobe's X11 bitmap fonts (BDF, hand-tuned at each size), their
outlines for 18 point and up (outlines.py: Tinos, and Bitstream's Courier 10 Pitch), and the
code that draws them (the Plug-In in plugins/fonts, given with --code). It reads the fonts'
releases from the archives kept in fonts/ (--download: from the releases themselves), checks
their SHA-256, converts them, and checks the outlines it makes against their SHA-256 too.

  python3 build.py [--config fonts.json] [--out dist] [--cache cache] [--preview] [--styles]
                   [--code "Web Fonts.flt"] [--table]

Needs freetype-py, macresources, and fontTools (and Pillow for --preview). Writes, in --out:
  Web Fonts.bin             MacBinary: type FFIL, creator MiLL, resource fork only
  Web Fonts Read Me.txt     what it is and the licenses (CR line ends, Mac Roman)

A family with "files" in fonts.json is converted from BDF strikes, one file per size and style
(its characters are Unicode, ISO 10646-1: each Mac Roman 32 to 255 character is looked up by
its Unicode value; those a strike lacks show as the missing glyph, and are listed). One with a
"ttf" is rendered at its size in pixels per em (72 dpi) with FreeType's monochrome mode. Either
is written as FONT resources (family ID * 128 + size, purgeable) with a FOND for each family.
A family's "outlines" are 'Msfn' resources (TrueType data the Font Manager doesn't see: only
Mill's Plug-In draws them), listed in 'Mfnt' 128 with the sizes they're for.'''

import argparse, hashlib, io, json, os, struct, sys, tarfile, tempfile, urllib.request, zipfile, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import freetype
import macresources

UA = 'Mill-webfonts/1.0 (+https://github.com/eric-cgn/mill; font build script)'
MAC_EPOCH = 2082844800
PERIOD = 882921600                            # 1997-12-25: the files' date unless SOURCE_DATE_EPOCH says


def get(url, sha256, cache):
    '''A file downloaded once into the cache and checked against its pinned SHA-256'''
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, os.path.basename(url))
    if not os.path.exists(path):
        req = urllib.request.Request(url, headers={'User-Agent': UA})
        with urllib.request.urlopen(req, timeout=120) as r, open(path, 'wb') as f:
            f.write(r.read())
    data = open(path, 'rb').read()
    got = hashlib.sha256(data).hexdigest()
    if got != sha256:
        sys.exit('%s: SHA-256 is %s, expected %s' % (path, got, sha256))
    return data


def members(data, kind):
    '''{base name: bytes} for the files in a tarball or zip'''
    out = {}
    if kind == 'zip':
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            for n in z.namelist():
                if not n.endswith('/'):
                    out[os.path.basename(n)] = z.read(n)
    else:
        with tarfile.open(fileobj=io.BytesIO(data)) as t:
            for m in t.getmembers():
                if m.isfile():
                    out[os.path.basename(m.name)] = t.extractfile(m).read()
    return out


def source_notice(src, cache, files, here):
    '''The text a source's license asks to stay with every copy: its license file (kept in this
    folder, or a member of the archive), or the files the config names (downloaded and
    checked), and, if it says where, the copyright notice cut out of one of them'''
    parts = []
    lic = src['license']
    if 'file' in lic:
        parts.append(open(os.path.join(here, lic['file']), 'rb').read().decode('utf-8', 'replace').strip())
    if 'member' in lic:
        parts.append(files[lic['member']].decode('utf-8', 'replace').strip())
    for item in lic.get('urls', []):
        text = get(item['url'], item['sha256'], cache).decode('utf-8', 'replace')
        if 'start' in item:
            a = text.index(item['start'])
            text = text[a:text.index(item['end'], a)]
        parts.append(text.strip())
    return '\n\n'.join(parts)


def read_source(key, src, here, a):
    '''{file name: bytes} of a source: from its archive here (the files of its "dir"), or with
    --download from its release ("url") or its files ("urls"); each file "members" names
    checked against its SHA-256'''
    if 'archive' in src and not a.download:
        out = {}
        with tarfile.open(os.path.join(here, src['archive'])) as t:
            for m in t.getmembers():
                if m.isfile() and os.path.dirname(m.name) == src['dir']:
                    out[os.path.basename(m.name)] = t.extractfile(m).read()
    elif 'url' in src:
        out = members(get(src['url'], src['sha256'], a.cache), src['format'])
    elif 'urls' in src:                       # loose files, each checked by its "members" SHA-256
        out = {name: get(url, src['members'][name], a.cache) for name, url in src['urls'].items()}
    else:
        sys.exit('%s: no release to download; build from %s' % (key, src['archive']))
    for name, sha in src.get('members', {}).items():
        if name not in out:
            sys.exit('%s: %s is missing' % (key, name))
        got = hashlib.sha256(out[name]).hexdigest()
        if got != sha:
            sys.exit('%s: %s: SHA-256 is %s, expected %s' % (key, name, got, sha))
    return out


def method_for(f, size):
    '''The hinting method for a family at a size: "font" (its own TrueType hints) or "auto"
    (FreeType's autohinter), from the family's "hinting" in the config; every style uses the
    plain strike's method at that size'''
    h = f.get('hinting', {})
    return h.get('sizes', {}).get(str(size), h.get('default', 'font'))


def mac_roman(code):
    return bytes([code]).decode('mac_roman')


class Strike:
    '''One font at one size: the glyphs, then the FONT resource'''

    def __init__(self, ttf, size, max_ascent=None, max_descent=None, method='font'):
        self._keep = ttf                                       # the face reads from this
        face = freetype.Face(io.BytesIO(ttf))
        self.size = size
        face.set_char_size(size * 64, 0, 72, 72)             # size pixels per em
        self.first, self.last = 32, 255
        flags = freetype.FT_LOAD_TARGET_MONO | freetype.FT_LOAD_RENDER
        if method == 'auto':                                  # FreeType's autohinter, not the font's own hints
            flags |= freetype.FT_LOAD_FORCE_AUTOHINT
        elif method != 'font':
            sys.exit('hinting method %r is not "font" or "auto"' % method)
        self.glyphs = {}
        for code in range(self.first, self.last + 1):
            ch = mac_roman(code)
            idx = face.get_char_index(ord(ch)) if len(ch) == 1 else 0
            if idx == 0:
                continue                                      # not in the font: the missing glyph shows
            self.glyphs[code] = self.render(face, idx, flags)
        self.missing = self.render(face, 0, flags)
        m = face.size
        # the strike holds the tallest ink, so nothing is clipped, unless the config caps it
        # (smaller lines, but the tallest accents lose their tops)
        everything = list(self.glyphs.values()) + [self.missing]
        self.ascent = max(g['top'] for g in everything)
        self.descent = max(g['rows'] - g['top'] for g in everything)
        if max_ascent:
            self.ascent = min(self.ascent, max_ascent)
        if max_descent:
            self.descent = min(self.descent, max_descent)
        self.leading = round((m.height - m.ascender + m.descender) / 64)
        if self.leading < 0:
            self.leading = 0

    @staticmethod
    def render(face, idx, flags):
        face.load_glyph(idx, flags)
        s = face.glyph
        b = s.bitmap
        rows = []
        for r in range(b.rows):
            line = bytes(b.buffer[r * b.pitch:r * b.pitch + (b.width + 7) // 8])
            rows.append([(line[x >> 3] >> (7 - (x & 7))) & 1 for x in range(b.width)])
        return {'rows': b.rows, 'width': b.width, 'left': s.bitmap_left, 'top': s.bitmap_top,
                'advance': (s.advance.x + 32) >> 6, 'bits': rows}

    def font_resource(self):
        '''The FONT resource's data (Inside Macintosh: Text, "Font Resources")'''
        order = [c for c in range(self.first, self.last + 1) if c in self.glyphs]
        everything = [self.glyphs.get(c) for c in range(self.first, self.last + 1)] + [self.missing]
        kern_max = min([0] + [g['left'] for g in everything if g])
        height = self.ascent + self.descent
        loc, x = [], 0
        for g in everything:
            loc.append(x)
            if g:
                x += g['width']
        loc.append(x)
        row_words = (x + 15) // 16
        image = [[0] * (row_words * 16) for _ in range(height)]
        for g, start in zip(everything, loc):
            if not g:
                continue
            for r, bits in enumerate(g['bits']):
                y = self.ascent - g['top'] + r
                if y < 0 or y >= height:
                    continue
                for i, v in enumerate(bits):
                    if v:
                        image[y][start + i] = 1
        strike = bytearray()
        for line in image:
            for w in range(row_words):
                n = 0
                for v in line[w * 16:w * 16 + 16]:
                    n = (n << 1) | v
                strike += struct.pack('>H', n)
        ow = []
        for g in everything:
            ow.append(0xFFFF if not g else ((g['left'] - kern_max) << 8) | g['advance'])
        ow[-2] = ow[-2] if ow[-2] != 0xFFFF else 0
        ow.append(0xFFFF)                                      # the end
        # the missing glyph's entry sits after the last character
        n_chars = self.last - self.first + 1
        assert len(everything) == n_chars + 1 and len(loc) == n_chars + 2 and len(ow) == n_chars + 2
        wid_max = max(g['advance'] for g in everything if g)
        rect_w = max(g['left'] + g['width'] for g in everything if g) - kern_max
        ow_loc = (26 + len(strike) + len(loc) * 2 - 16) // 2
        head = struct.pack('>Hhhhhhhhhhhhh', 0x9000, self.first, self.last, wid_max, kern_max,
                           -self.descent, rect_w, height, ow_loc, self.ascent, self.descent, self.leading, row_words)
        return head + bytes(strike) + struct.pack('>%dH' % len(loc), *loc) + struct.pack('>%dH' % len(ow), *ow), wid_max

    def mean_width(self):
        return sum(self.glyphs[c]['advance'] for c in self.glyphs) / len(self.glyphs)


PIXELS = {}     # (family, style, slot size): the BDF strike's pixel size, file, and source


def parse_bdf(data):
    '''{Unicode value: glyph} and the font's ascent and descent, from a BDF file'''
    glyphs, props, cur, bitmap = {}, {}, None, None
    for line in data.decode('latin-1').split('\n'):
        w = line.split()
        if not w:
            continue
        if bitmap is not None and w[0] != 'ENDCHAR':
            bitmap.append(w[0])
        elif w[0] in ('FONT_ASCENT', 'FONT_DESCENT', 'PIXEL_SIZE'):
            props[w[0]] = int(w[1])
        elif w[0] == 'STARTCHAR':
            cur = {}
        elif cur is not None and w[0] == 'ENCODING':
            cur['enc'] = int(w[1])
        elif cur is not None and w[0] == 'DWIDTH':
            cur['advance'] = int(w[1])
        elif cur is not None and w[0] == 'BBX':
            cur['width'], cur['rows'], cur['left'], yoff = (int(x) for x in w[1:5])
            cur['top'] = cur['rows'] + yoff
        elif w[0] == 'BITMAP':
            bitmap = []
        elif w[0] == 'ENDCHAR':
            bits = []
            for hexrow in bitmap:
                n = int(hexrow, 16) if hexrow else 0
                total = len(hexrow) * 4
                bits.append([(n >> (total - 1 - x)) & 1 for x in range(cur['width'])])
            cur['bits'] = bits
            glyphs[cur['enc']] = cur
            cur = bitmap = None
    return glyphs, props.get('FONT_ASCENT', 0), props.get('FONT_DESCENT', 0), props.get('PIXEL_SIZE', 0)


class BdfStrike(Strike):
    '''One BDF file as a strike: the hand-made bitmaps of X11's Adobe fonts at one size, with
    their characters moved into Mac Roman's places by Unicode value'''

    def __init__(self, data, size, name):
        self.size = size
        self.first, self.last = 32, 255
        by_unicode, asc, desc, self.px = parse_bdf(data)
        self.glyphs, self.absent = {}, []
        for code in range(self.first, self.last + 1):
            ch = mac_roman(code)
            g = by_unicode.get(ord(ch)) if len(ch) == 1 else None
            if g is None and code == 0xCA:
                g = by_unicode.get(0x20)                   # the non-breaking space, as a space
            if g is None:
                if code != 0x7F:
                    self.absent.append(code)
                continue
            self.glyphs[code] = g
        d = by_unicode.get(0)
        if d is None:                                      # no "missing" glyph: a hollow box
            w = max(3, self.glyphs[ord('n')]['advance'] - 1)
            h = max(4, asc - 1)
            box = [[1 if r in (0, h - 1) or c in (0, w - 1) else 0 for c in range(w)] for r in range(h)]
            d = {'rows': h, 'width': w, 'left': 0, 'top': h, 'advance': w + 1, 'bits': box}
        self.missing = d
        everything = list(self.glyphs.values()) + [self.missing]
        self.ascent = max([asc] + [g['top'] for g in everything])
        self.descent = max([desc] + [g['rows'] - g['top'] for g in everything])
        self.leading = 0
        self.name = name



def fond_resource(family_id, strikes, assoc):
    '''A FOND with no width, kerning, or style tables: assoc says which FONT is the strike
    for each size and style (bold 1, italic 2); a style it doesn't list QuickDraw makes itself.
    Metrics are fractions of an em, in 1/4096, the largest of the sizes'.'''
    s0 = strikes[0]
    fx = lambda px, em: int(round(px * 4096 / em))
    asc = max(fx(s.ascent, s.size) for s in strikes)
    desc = max(fx(s.descent, s.size) for s in strikes)
    lead = max(fx(s.leading, s.size) for s in strikes)
    wid = max(fx(s.font_resource()[1], s.size) for s in strikes)
    head = struct.pack('>HhHHhhhh', 0, family_id, s0.first, s0.last, asc, -desc, lead, wid)
    head += struct.pack('>III', 0, 0, 0)                      # no width, kerning, or style tables
    head += b'\0' * 18 + b'\0' * 4 + struct.pack('>h', 0)     # properties, international, version
    head += struct.pack('>h', len(assoc) - 1)
    for size, style, font_id in sorted(assoc):
        head += struct.pack('>hhh', size, style, font_id)
    return head


def bcd(n):
    return (n // 10) << 4 | n % 10


def vers(version, short, long_):
    major, minor = (int(x) for x in version.split('.')[:2])
    s, l = short.encode('mac_roman'), long_.encode('mac_roman')
    return struct.pack('>BBBBH', bcd(major), minor << 4, 0x80, 0, 0) + bytes([len(s)]) + s + bytes([len(l)]) + l


def gzip(data):
    '''A gzip member with a fixed header (no name, time 0, OS unknown), so the bytes are the same
    whichever Python makes them: gzip.compress's OS byte differs between Python versions'''
    z = zlib.compressobj(9, zlib.DEFLATED, -15)
    body = z.compress(data) + z.flush()
    return b'\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff' + body + struct.pack('<II', zlib.crc32(data), len(data) & 0xFFFFFFFF)


def mac_text(text):
    return text.replace('\r\n', '\n').replace('\n', '\r').encode('mac_roman', 'replace')


STYLE_NAMES = {'1': 'bold', '2': 'italic', '3': 'bold italic'}


def about_text(cfg, fonts, a):
    '''What the suitcase is, and what each family comes from'''
    lines = ['%s %s' % (cfg['name'], cfg['version']), '',
             'Fonts for Mill, the web browser. Put this file in the "Mill Plug-Ins" folder beside',
             'Mill. Mill Serif and Mill Mono then become Mill\'s default fonts.', '']
    for f in fonts:
        sizes = ', '.join(str(z) for z in f['sizes'])
        real = [STYLE_NAMES[k] for k in sorted(f.get('styles', {})) if a.styles]
        lines += ['%s (%s point; regular%s):' % (f['family'], sizes, ''.join(', ' + r for r in real)), '  ' + f['from'] + '.']
        if f.get('outlines') and a.code:
            lo, hi = f['outlines']['sizes']
            lines += ['%s outlines (%d to %d point; regular, bold, and italic):' % (f['family'], lo, hi),
                      '  ' + f['outlines']['from'] + '.']
    lines += ['', 'The font names are Mill\'s own, not the designs\'.', '',
              'The fonts keep the licenses they came under, which require their copyright notices',
              'and license texts to stay with every copy: resource TXGZ 129 "License" of this file,',
              'shown on Mill\'s license page. In short:']
    shown = {}
    for key in sorted({k for f in fonts for k in source_keys(f) | (outline_keys(f) if a.code else set())}):
        src = cfg['sources'][key]
        shown.setdefault(src['summary'], []).append(src['name'])
    if a.code:
        shown.setdefault(cfg['code']['summary'], []).append(cfg['code']['name'])
    for summary, names in shown.items():
        lines += ['  %s: %s' % (' and '.join(names), summary)]
    return '\n'.join(lines)


def macbinary(name, ftype, creator, rsrc, when):
    '''MacBinary II: no data fork, the resource fork, and the header's CRC'''
    n = name.encode('mac_roman')
    h = bytearray(128)
    h[1] = len(n)
    h[2:2 + len(n)] = n
    h[65:69], h[69:73] = ftype, creator
    struct.pack_into('>II', h, 83, 0, len(rsrc))
    struct.pack_into('>II', h, 91, when, when)
    h[122] = h[123] = 129
    crc = 0
    for b in h[:124]:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    struct.pack_into('>H', h, 124, crc)
    return bytes(h) + rsrc + b'\0' * (-len(rsrc) % 128)


def source_keys(f):
    """The sources a family reads: its "source", or those its "files" name"""
    if 'source' in f:
        return {f['source']}
    keys = {k for v in [f['files']] + [st['files'] for st in f.get('styles', {}).values()] for pair in v.values() for k in [pair[0]]}
    return keys


def outline_keys(f):
    """The sources a family's outlines read"""
    return {v['source'] for k, v in f.get('outlines', {}).items() if k.isdigit()}


def outline_data(f, files):
    '''{style: the reduced TrueType font's bytes} for a family's "outlines": a TrueType file of
    its source reduced (outlines.py), or a Type 1 font converted first; each checked against
    its SHA-256, so the build gives the same fonts every time'''
    import outlines
    out = {}
    for k, v in sorted((k, v) for k, v in f.get('outlines', {}).items() if k.isdigit()):
        src = files[v['source']]
        if 'type1' in v:
            pfb, afm, fam, style = v['type1']
            with tempfile.TemporaryDirectory() as d:
                for name in (pfb, afm):
                    open(os.path.join(d, name), 'wb').write(src[name])
                outlines.convert(os.path.join(d, pfb), os.path.join(d, afm), os.path.join(d, 'out.ttf'), fam, style)
                ttf = open(os.path.join(d, 'out.ttf'), 'rb').read()
        else:
            ttf = src[v['file']]
        data, missing = outlines.reduce(ttf)
        got = hashlib.sha256(data).hexdigest()
        if got != v['sha256']:
            sys.exit('%s %s outline: SHA-256 is %s, expected %s' % (f['family'], STYLE_NAMES.get(k, 'regular'), got, v['sha256']))
        print('%s %s outline: %d bytes%s' % (f['family'], STYLE_NAMES.get(k, 'regular'), len(data),
                                            ', lacks ' + ' '.join(missing) if missing else ''))
        out[int(k)] = data
    return out


def mfnt_resource(entries):
    '''Mfnt 128, the outlines' index for the Plug-In: a count, then for each family its name
    (a Pascal string in 32 bytes), the smallest and largest size it's drawn at, whether every
    letter is one width, and the 'Msfn' ID of its plain, bold, italic, and bold italic (0: none)'''
    out = struct.pack('>h', len(entries))
    for name, lo, hi, fixed, ids in entries:
        n = name.encode('mac_roman')
        out += (bytes([len(n)]) + n).ljust(32, b'\0') + struct.pack('>hhh4h', lo, hi, fixed, *ids)
    return out


def interface_number(header):
    '''kMillInterface from sdk/Interfaces/MillInterface.h'''
    for line in open(header):
        w = line.split()
        if len(w) == 3 and w[:2] == ['#define', 'kMillInterface']:
            return int(w[2])
    sys.exit('%s: no kMillInterface' % header)


def build(cfg, a, files, notices):
    '''The resources for the families in cfg; sizes and strikes by family for the table'''
    resources, built, table, absent = [], [], [], {}
    seen = set()
    for f in cfg['fonts']:
        ids = [f['id']] + [v['id'] for v in f.get('styles', {}).values()]
        for i in ids:
            if i in seen or not 128 <= i <= 255:
                sys.exit('%s: font ID %d is used twice or is not 128 to 255' % (f['family'], i))
            seen.add(i)
        # A style's real strikes must be the family's smallest sizes, with no gap (all of them, or
        # the first few). On a Mac Plus (Mini vMac), Bold at only 18, 24, and 36 point, with plain
        # strikes from 9, hung the machine when text was set at the sizes below; Bold at all
        # sizes, and at 9, 10, and 12 of 9 to 36, are tested and fine. The cause isn't known, so
        # a layout that isn't one of those kinds is refused.
        for k, v in ({} if a.allow_partial_styles else f.get('styles', {})).items():
            have = sorted(v.get('sizes', f['sizes']))
            if have != sorted(f['sizes'])[:len(have)]:
                sys.exit('%s: style %s has real strikes at %s, which are not the smallest sizes of %s' %
                         (f['family'], k, have, sorted(f['sizes'])))
        strikes, assoc = [], []
        # plain at the family's own ID * 128 + size; the other styles, if asked for, at IDs of
        # their own (an unused family each), which the FOND's association table points to
        variants = [(0, f.get('ttf') or f['files'], f['id'], f['family'], f['sizes'])] + [
            (int(k), v.get('ttf') or v['files'], v['id'], '%s %s' % (f['family'], STYLE_NAMES[k].title()), v.get('sizes', f['sizes']))
            for k, v in sorted(f.get('styles', {}).items())]
        for style, ttf, fid, rname, sizes in variants:
            for size in sizes:
                if isinstance(ttf, dict):                     # BDF: a file for each size
                    src, member = ttf[str(size)]
                    st = BdfStrike(files[src][member], size, member)
                    absent[member] = st.absent
                    PIXELS[(f['family'], style, size)] = (st.px, member, src)
                else:
                    st = Strike(files[f['source']][ttf], size, f.get('max_ascent'), f.get('max_descent'), method_for(f, size))
                data, wid = st.font_resource()
                table.append((f['family'], style, size, len(data)))
                if style and not a.styles:
                    continue
                resources.append(macresources.Resource(b'FONT', fid * 128 + size, '%s %d' % (rname, size), 0x20, data))
                assoc.append((size, style, fid * 128 + size))
                if style == 0:
                    strikes.append(st)
                print('%s %d: ascent %d, descent %d, leading %d, widest %d, mean width %.2f, %d bytes' %
                      (rname, size, st.ascent, st.descent, st.leading, wid, st.mean_width(), len(data)))
                if a.preview:
                    dump_preview(st, os.path.join(a.out, '%s %d.png' % (rname, size)), data)
        resources.append(macresources.Resource(b'FOND', f['id'], f['family'], 0x20, fond_resource(f['id'], strikes, assoc)))
        built.append(f)
    index = []
    for f in built:
        ol = f.get('outlines')
        if not ol or not a.code:              # nothing draws them without the Plug-In
            continue
        ids = [0, 0, 0, 0]
        for style, data in outline_data(f, files).items():
            fid = f['id'] if style == 0 else f['styles'][str(style)]['id']
            resources.append(macresources.Resource(b'Msfn', fid, '%s%s' % (f['family'], '' if style == 0 else ' ' + STYLE_NAMES[str(style)].title()), 0x20, data))
            ids[style] = fid
            table.append((f['family'], style, 'outline', len(data)))
        index.append((f['family'], ol['sizes'][0], ol['sizes'][1], 1 if f.get('fixed') else 0, ids))
    if index:
        resources.append(macresources.Resource(b'Mfnt', 128, 'Outlines', 0, mfnt_resource(index)))
    if a.code:
        flt = open(a.code, 'rb').read()
        resources.append(macresources.Resource(b'MPlg', 128, cfg['name'], 0, flt))
        resources.append(macresources.Resource(b'MIfc', 128, 'Interface', 0, struct.pack('>h', interface_number(a.interface))))
        table.append(('code', 0, 'code', len(flt)))
    ver = cfg['version']
    froms = '; '.join(f['from'] for f in built)
    lic = ', '.join(sorted({cfg['sources'][k]['licence_name'] for f in built for k in source_keys(f) | (outline_keys(f) if a.code else set())}))
    if absent:
        by_char = {}
        for member, codes in sorted(absent.items()):
            for c in codes:
                by_char.setdefault(c, []).append(member)
        print('Mac Roman characters some strikes lack (shown as the missing glyph):')
        for c in sorted(by_char):
            print('  %02X %s: %d of %d strikes' % (c, mac_roman(c), len(by_char[c]), len(absent)))
    resources.append(macresources.Resource(b'vers', 1, '', 0, vers(ver, ver, '%s %s, %s' % (cfg['name'], ver, ' and '.join(f['family'] for f in built)))))
    resources.append(macresources.Resource(b'vers', 2, '', 0, vers(ver, '%s %s' % (cfg['name'], ver), 'Fonts for Mill (%s)' % lic)))
    about = about_text(cfg, built, a)
    resources.append(macresources.Resource(b'TEXT', 128, 'About %s' % cfg['name'], 0, mac_text(about)))
    # the license, gzipped ('TXGZ'; Mill reads it only when it builds mill://license): ID 129, "License"
    resources.append(macresources.Resource(b'TXGZ', 129, 'License', 0, gzip(mac_text(notices))))
    return resources, about, table


def size_table(table):
    '''Bytes of each FONT resource by family, style, and size; then the outlines and the code'''
    bitmaps = [t for t in table if isinstance(t[2], int)]
    sizes = sorted({t[2] for t in bitmaps})
    out = ['%-22s' % 'FONT resource bytes' + ''.join('%8d' % z for z in sizes) + '   total']
    fams = []
    for fam, style, size, n in bitmaps:
        if (fam, style) not in fams:
            fams.append((fam, style))
    for fam, style in fams:
        row = {size: n for f, st, size, n in bitmaps if f == fam and st == style}
        name = '%s %s' % (fam, ['plain', 'bold', 'italic', 'bold italic'][style])
        out.append('%-22s' % name + ''.join('%8s' % (row.get(z, '-')) for z in sizes) + '%8d' % sum(row.values()))
    for fam, style in fams:
        if style == 0 and any(k[0] == fam for k in PIXELS):
            out.append('%-22s' % (fam + ' pixels') + ''.join('%8s' % PIXELS.get((fam, 0, z), ('-',))[0] for z in sizes))
    for fam, style, kind, n in table:
        if kind == 'outline':
            out.append('%-22s %8d  Msfn' % ('%s %s' % (fam, ['plain', 'bold', 'italic', 'bold italic'][style]), n))
        elif kind == 'code':
            out.append('%-22s %8d  MPlg' % ('code', n))
    return '\n'.join(out)


def split(resources):
    '''The file's bytes in three: bitmaps (FONT and FOND), outlines (Msfn and Mfnt), and code
    (MPlg and MIfc); the rest (About, License, vers) and the map with the bitmaps'''
    whole = len(macresources.make_file(resources))
    outlines = sum(len(r) + 4 for r in resources if r.type in (b'Msfn', b'Mfnt'))
    code = sum(len(r) + 4 for r in resources if r.type in (b'MPlg', b'MIfc'))
    return whole - outlines - code, outlines, code


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--config', default=os.path.join(here, 'fonts.json'))
    ap.add_argument('--out', default='dist')
    ap.add_argument('--cache', default='cache')
    ap.add_argument('--preview', action='store_true', help='also write a PNG of each strike')
    ap.add_argument('--styles', action='store_true', help='also make the real bold and italic strikes fonts.json lists, at the sizes it lists (QuickDraw makes the styles from the plain strike otherwise)')
    ap.add_argument('--allow-partial-styles', action='store_true', help='skip the check that refuses real style strikes missing a size half or double one that exists (for testing)')
    ap.add_argument('--download', action='store_true', help='read the sources from their release files (url and sha256, or urls and members, in fonts.json) instead of the copies in this folder')
    ap.add_argument('--code', help='the Plug-In that draws the outlines, a flat code resource (plugins/fonts/build.sh makes it), put in as MPlg 128 with its MIfc')
    ap.add_argument('--interface', default=os.path.join(here, '..', '..', 'sdk', 'Interfaces', 'MillInterface.h'), help='where kMillInterface is defined, for the MIfc')
    ap.add_argument('--table', action='store_true', help='print the size of every strike, and the suitcase with and without the real styles, and stop')
    a = ap.parse_args()
    cfg = json.load(open(a.config))
    files, notices = {}, []
    texts = {}
    here = os.path.dirname(os.path.abspath(a.config))
    for key in sorted({k for f in cfg['fonts'] for k in source_keys(f) | outline_keys(f)}):
        src = cfg['sources'][key]
        files[key] = read_source(key, src, here, a)
        texts.setdefault(source_notice(src, a.cache, files[key], here), []).append(src['name'])
    if a.code:                                # the Plug-In's code: Mill's own license
        code = cfg['code']
        texts.setdefault(open(os.path.join(here, code['license']), 'rb').read().decode('utf-8', 'replace').strip(), []).append(code['name'])
    for text, names in texts.items():         # one notice for sources that share it
        notices.append('=== %s ===\n\n%s' % (', '.join(names), text))
    notices = ('\n\n' + '-' * 70 + '\n\n').join(notices)
    when = int(os.environ.get('SOURCE_DATE_EPOCH', PERIOD)) + MAC_EPOCH
    if a.table:
        a.styles = True
        res, _, table = build(cfg, a, files, notices)
        print(size_table(table))
        with_styles = len(macresources.make_file(res))
        a.styles = False
        res2, _, _ = build(cfg, a, files, notices)
        print('\nsuitcase, plain only: %d bytes; with every real style listed: %d bytes' % (len(macresources.make_file(res2)), with_styles))
        return
    resources, about, table = build(cfg, a, files, notices)
    os.makedirs(a.out, exist_ok=True)
    out = macbinary(cfg['name'], b'FFIL', b'MiLL', macresources.make_file(resources), when)
    open(os.path.join(a.out, cfg['name'] + '.bin'), 'wb').write(out)
    open(os.path.join(a.out, cfg['name'] + ' Read Me.txt'), 'wb').write(
        mac_text(about + '\n\n' + '-' * 70 + '\n\n' + notices + '\n'))
    b, o, c = split(resources)
    print('%s.bin: %d bytes; resource fork %d: bitmaps %d, outlines %d, code %d' % (cfg['name'], len(out), b + o + c, b, o, c))


def dump_preview(st, path, data):
    '''The strike's bit image as a PNG, for looking at'''
    from PIL import Image
    (_, _, _, _, _, _, _, height, _, _, _, _, row_words) = struct.unpack('>13h', data[:26])
    im = Image.new('1', (row_words * 16, height), 1)
    for y in range(height):
        for w in range(row_words):
            n = struct.unpack('>H', data[26 + (y * row_words + w) * 2:28 + (y * row_words + w) * 2])[0]
            for b in range(16):
                if n >> (15 - b) & 1:
                    im.putpixel((w * 16 + b, y), 0)
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    im.save(path)


if __name__ == '__main__':
    main()
