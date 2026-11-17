#!/usr/bin/env python3
'''Mill's outline fonts: the TrueType files Web Fonts carries for Mill Serif and Mill Mono at 18
point and up, made from the fonts' releases (build.py calls it; it can also run on its own).

  Type 1 to TrueType   convert(pfb, afm, family, style): Bitstream's Courier 10 Pitch, Type 1
                       outlines and AFM metrics, to a TrueType font (cubic curves to quadratic
                       with cu2qu, error 1 unit in 1000; seac accents kept as composites).
  Reduced for Mill     reduce(ttf, ...): Mac Roman only, no hints, no layout tables, a few
                       names, accented letters drawn as base + accent made composites, curves
                       simplified within 1/8 pixel at 18 point, 256 units per em, and a Mac
                       Roman (1, 0) format 0 cmap beside the (3, 1) one. About 15K a face.

Every step is fontTools (version pinned in build.sh), and the same input gives the same bytes:
the 'head' table's modified date is set to MODIFIED, not the time of the build.

  python3 -I outlines.py OUT_DIR NAME=SRC.ttf ...         reduce TrueType files
  python3 -I outlines.py --t1 PFB AFM OUT.ttf FAMILY STYLE convert one Type 1 font

Inputs are data only: parsed by fontTools, nothing in them is executed.'''

import io, math, os, sys, unicodedata

from fontTools import subset
from fontTools.afmLib import AFM
from fontTools.agl import toUnicode
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.recordingPen import RecordingPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.t1Lib import T1Font
from fontTools.ttLib import TTFont
from fontTools.ttLib.scaleUpem import scale_upem
from fontTools.ttLib.tables._c_m_a_p import cmap_format_0
from fontTools.ttLib.tables._g_l_y_f import Glyph, GlyphComponent, GlyphCoordinates, flagOnCurve

UPEM = 256          # units per em of the reduced fonts
TOLPX = 0.125       # how far a simplified curve may move, in pixels...
PPEM = 18           # ...at this size (the smallest Mill draws outlines at)
MODIFIED = 2965852800   # 'head' modified: 1997-12-25 00:00, seconds since 1904

# Mac Roman as of 1997: 0xDB is the currency sign (Apple made it the euro in 1998); 0xF0 is the
# Apple logo (not counted)
MR = {}
for b in range(0x20, 0x100):
    if b == 0x7f:
        continue
    u = ord(bytes([b]).decode('mac_roman'))
    if u == 0x20AC:
        u = 0xA4
    MR[b] = u
ALIAS = {0xA0: [0x20], 0x2126: [0x3A9], 0x3A9: [0x2126], 0x2206: [0x394], 0x394: [0x2206], 0xB5: [0x3BC], 0x3BC: [0xB5]}


# ---- Type 1 to TrueType

def convert(src, afmp, out, fam, style):
    '''A Type 1 font (PFB or PFA) and its AFM as a TrueType font with quadratic outlines'''
    t1 = T1Font(src)
    t1.parse()
    gs = t1.getGlyphSet()
    cs = t1.font['CharStrings']
    afm = AFM(afmp)
    names = ['.notdef'] + sorted(n for n in cs.keys() if n != '.notdef')
    glyphs, adv = {}, {}
    for n in names:
        cs[n].decompile()
        _r = RecordingPen()
        gs[n].draw(_r)
        adv[n] = round(gs[n].width)
        p = TTGlyphPen(gs)
        gs[n].draw(Cu2QuPen(p, max_err=1.0, reverse_direction=True))   # seac: two components
        glyphs[n] = p.glyph()
    cmap = {}
    for n in names:
        u = toUnicode(n)
        if len(u) == 1 and ord(u) not in cmap:
            cmap[ord(u)] = n
    fi = t1.font['FontInfo']
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap(cmap)
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics({n: (adv[n], 0) for n in names})
    asc, desc = afm._attrs.get('Ascender', 750), afm._attrs.get('Descender', -250)
    fb.setupHorizontalHeader(ascent=asc, descent=desc)
    fb.setupNameTable({'familyName': fam, 'styleName': style,
                       'uniqueFontIdentifier': f'{fam} {style} (from {fi.get("FullName")} {fi.get("version")})',
                       'fullName': f'{fam} {style}', 'psName': (fam + '-' + style).replace(' ', ''),
                       'version': 'Version ' + str(fi.get('version', '1.0')), 'copyright': fi.get('Notice', '')})
    fb.setupOS2(sTypoAscender=asc, sTypoDescender=desc, usWinAscent=max(asc, 0), usWinDescent=-desc,
                sxHeight=round(afm._attrs.get('XHeight', 0)), sCapHeight=round(afm._attrs.get('CapHeight', 0)),
                version=2, usWeightClass=700 if 'Bold' in style else 400,
                fsSelection=(0x20 if 'Bold' in style else 0) | (0x01 if 'Italic' in style else 0) | (0x40 if style == 'Regular' else 0))
    fb.setupPost(isFixedPitch=1 if fi.get('isFixedPitch') else 0, italicAngle=float(fi.get('ItalicAngle', 0)))
    fb.setupHead(macStyle=(1 if 'Bold' in style else 0) | (2 if 'Italic' in style else 0))
    fb.setupMaxp()
    fb.font['post'].formatType = 3.0
    fb.save(out)


# ---- geometry

def contours(glyph, glyf):
    coords, ends, flags = glyph.getCoordinates(glyf)
    out, s = [], 0
    for e in ends:
        out.append([(coords[i][0], coords[i][1], bool(flags[i] & flagOnCurve)) for i in range(s, e + 1)])
        s = e + 1
    return out


def explicit(c):
    '''the implied on-curve points made real; the list starts at an on-curve point'''
    pts, n = [], len(c)
    for i in range(n):
        p, q = c[i], c[(i + 1) % n]
        pts.append(p)
        if not p[2] and not q[2]:
            pts.append(((p[0] + q[0]) / 2, (p[1] + q[1]) / 2, True))
    k = next((i for i, p in enumerate(pts) if p[2]), None)
    if k is None:
        return None
    return pts[k:] + pts[:k]


def qpts(a, c, b, steps=8):
    return [((1 - u) ** 2 * a[0] + 2 * u * (1 - u) * c[0] + u * u * b[0], (1 - u) ** 2 * a[1] + 2 * u * (1 - u) * c[1] + u * u * b[1])
            for u in (t / steps for t in range(steps + 1))]


def dseg(p, a, b):
    vx, vy = b[0] - a[0], b[1] - a[1]
    L = vx * vx + vy * vy
    t = 0 if L == 0 else max(0, min(1, ((p[0] - a[0]) * vx + (p[1] - a[1]) * vy) / L))
    return math.hypot(p[0] - a[0] - t * vx, p[1] - a[1] - t * vy)


def dpoly(p, poly):
    return min(dseg(p, poly[i], poly[i + 1]) for i in range(len(poly) - 1))


def segments(pts):
    '''explicit points (from an on-curve one) as segments: ['L', a, b] or ['Q', a, c, b]'''
    segs, m, i = [], len(pts), 0
    while i < m:
        a, q = pts[i], pts[(i + 1) % m]
        if q[2]:
            segs.append(['L', a, q])
            i += 1
        else:
            segs.append(['Q', a, q, pts[(i + 2) % m]])
            i += 2
    return segs


def seg_poly(s):
    return [s[1][:2], s[2][:2]] if s[0] == 'L' else qpts(s[1], s[2], s[3])


def simplify_contour(c, tol):
    '''duplicate points dropped; then neighboring lines, or curves, joined into one wherever the
    result stays within tol of the original both ways'''
    pts = explicit(c)
    if pts is None:
        return c
    d = [pts[0]]
    for p in pts[1:]:
        if (p[0], p[1]) != (d[-1][0], d[-1][1]) or p[2] != d[-1][2]:
            d.append(p)
    if len(d) > 1 and (d[-1][0], d[-1][1]) == (d[0][0], d[0][1]) and d[-1][2]:
        d.pop()
    segs = segments(d)
    for s_ in segs:
        s_.append(seg_poly(s_))   # s_[-1]: the original samples it covers
    if tol > 0:
        changed = True
        while changed and len(segs) > 2:
            changed = False
            for i in range(len(segs)):
                s1, s2 = segs[i], segs[(i + 1) % len(segs)]
                orig = s1[-1] + s2[-1][1:]
                cand = None
                if s1[0] == 'L' and s2[0] == 'L':
                    cand = ['L', s1[1], s2[2]]
                elif s1[0] == 'Q' and s2[0] == 'Q':
                    a, c1, b = s1[1], s1[2], s2[3]
                    c2 = s2[2]
                    dx1, dy1 = c1[0] - a[0], c1[1] - a[1]
                    dx2, dy2 = c2[0] - b[0], c2[1] - b[1]
                    den = dx1 * dy2 - dy1 * dx2
                    if abs(den) > 1e-9:
                        t = ((b[0] - a[0]) * dy2 - (b[1] - a[1]) * dx2) / den
                        if t > 0:
                            cand = ['Q', a, (a[0] + t * dx1, a[1] + t * dy1, False), b]
                if cand is None:
                    continue
                newp = seg_poly(cand)
                if all(dpoly(p, newp) <= tol for p in orig) and all(dpoly(p, orig) <= tol for p in newp):
                    j = (i + 1) % len(segs)
                    cand.append(orig)
                    segs[i] = cand
                    del segs[j]
                    changed = True
                    break
    out = []
    for s in segs:
        if s[0] == 'L':
            out.append(s[1])
        else:
            out += [s[1], s[2]]
    res, n = [], len(out)
    for i, p in enumerate(out):     # implied on-curve points (exact midpoints) dropped again
        a, b = out[i - 1], out[(i + 1) % n]
        if p[2] and not a[2] and not b[2] and abs((a[0] + b[0]) / 2 - p[0]) <= 1e-6 and abs((a[1] + b[1]) / 2 - p[1]) <= 1e-6:
            continue
        res.append(p)
    return res


def apply(glyph, cs):
    coords, flags, ends = [], [], []
    for c in cs:
        for x, y, on in c:
            coords.append((x, y))
            flags.append(1 if on else 0)
        ends.append(len(coords) - 1)
    glyph.coordinates = GlyphCoordinates(coords)
    glyph.flags = bytearray(flags)
    glyph.endPtsOfContours = ends
    glyph.program.fromBytecode(b'')


def drop_implied_after_round(glyph, half):
    '''on-curve points within half a unit of their off-curve neighbors' midpoint removed'''
    cs, s = [], 0
    for e in glyph.endPtsOfContours:
        c = [(glyph.coordinates[i][0], glyph.coordinates[i][1], bool(glyph.flags[i] & 1)) for i in range(s, e + 1)]
        s = e + 1
        n = len(c)
        keep = [True] * n
        for i, p in enumerate(c):
            a, b = c[i - 1], c[(i + 1) % n]
            if p[2] and not a[2] and not b[2] and keep[i - 1] and abs((a[0] + b[0]) / 2 - p[0]) <= half and abs((a[1] + b[1]) / 2 - p[1]) <= half:
                keep[i] = False
        cs.append([p for p, k in zip(c, keep) if k])
    apply(glyph, cs)


# ---- composites

def _sig(cs):
    return tuple(tuple(on for _, _, on in c) for c in cs)


def _match(r, a, tol):
    dx, dy = r[0][0][0] - a[0][0][0], r[0][0][1] - a[0][0][1]
    for rc, ac in zip(r, a):
        for p, q in zip(rc, ac):
            if abs(p[0] - q[0] - dx) > tol or abs(p[1] - q[1] - dy) > tol:
                return None
    return dx, dy


def recompose(f):
    '''an accented letter drawn as base + accent outlines becomes a composite of the two glyphs;
    points may differ by up to half a unit of the target grid'''
    g = f['glyf']
    order = f.getGlyphOrder()
    cm = f.getBestCmap()
    tol = 0.5 * f['head'].unitsPerEm / UPEM
    simple = {n: contours(g[n], g) for n in order if g[n].numberOfContours > 0}
    index = {}
    for n, cs in simple.items():
        index.setdefault(_sig(cs), []).append(n)
    done = 0
    for u in sorted(set(MR.values())):
        n = cm.get(u)
        if n is None or n not in simple:
            continue
        d = unicodedata.decomposition(chr(u)).split()
        if len(d) != 2 or d[0].startswith('<'):
            continue
        bu = int(d[0], 16)
        bases = [cm.get(bu)] + (['dotlessi', cm.get(0x131)] if bu == 0x69 else [])
        cs = simple[n]
        found = None
        for b in bases:
            if b not in simple:
                continue
            bc = simple[b]
            nb = len(bc)
            for at in (0, len(cs) - nb):
                if at < 0 or cs[at:at + nb] != bc:
                    continue
                rest = cs[:at] + cs[at + nb:]
                for an in index.get(_sig(rest), []):
                    if an in (n, b) or g[an].isComposite():
                        continue
                    m = _match(rest, simple[an], tol)
                    if m:
                        found = (b, an, m, at)
                        break
                if found:
                    break
            if found:
                break
        if not found:
            continue
        b, an, (dx, dy), at = found
        ng = Glyph()
        ng.numberOfContours = -1
        ng.components = []
        for cn, x, y in (((b, 0, 0), (an, dx, dy)) if at == 0 else ((an, dx, dy), (b, 0, 0))):
            c = GlyphComponent()
            c.glyphName = cn
            c.x, c.y = round(x), round(y)
            c.flags = 0x4 if cn == b else 0
            ng.components.append(c)
        g[n] = ng
        done += 1
    return done


# ---- reduced for Mill

def options():
    o = subset.Options()
    o.hinting = False
    o.notdef_outline = True
    o.recalc_bounds = True
    o.recalc_timestamp = False
    o.drop_tables += ['Debg', 'STAT', 'DSIG', 'gasp', 'HVAR', 'fvar', 'gvar', 'avar', 'MVAR', 'hdmx', 'LTSH', 'VDMX', 'PCLT', 'meta', 'prop',
                      'GSUB', 'GPOS', 'GDEF', 'kern', 'morx', 'BASE', 'JSTF']
    o.layout_features = []
    o.name_IDs = [0, 1, 2, 4, 6, 13, 14]
    o.name_languages = [0x409]
    o.name_legacy = False
    o.glyph_names = False
    o.legacy_cmap = False
    o.symbol_cmap = False
    return o


def reduce(data):
    '''A TrueType font's bytes reduced for Mill; and what it lacks of Mac Roman (codes, hex)'''
    src = io.BytesIO(data)
    f = TTFont(src, recalcTimestamp=False)
    tol = TOLPX * f['head'].unitsPerEm / PPEM
    for t in f['cmap'].tables:
        if t.isUnicode():
            for u, alts in ALIAS.items():
                if u not in t.cmap:
                    for a in alts:
                        if a in t.cmap:
                            t.cmap[u] = t.cmap[a]
                            break
    best = dict(f.getBestCmap())
    missing = ['%02X' % b for b, u in MR.items() if b != 0xF0 and u not in best]
    recompose(f)
    uni = sorted({u for u in MR.values() if u != 0xF8FF})
    ss = subset.Subsetter(options())
    ss.populate(unicodes=uni)
    ss.subset(f)
    g = f['glyf']
    for n in f.getGlyphOrder():
        gl = g[n]
        if gl.numberOfContours > 0 and not gl.isComposite():
            apply(gl, [simplify_contour(c, tol) for c in contours(gl, g)])
    scale_upem(f, UPEM)
    f['head'].unitsPerEm = UPEM
    g = f['glyf']
    for n in f.getGlyphOrder():
        gl = g[n]
        if gl.numberOfContours > 0 and not gl.isComposite():
            drop_implied_after_round(gl, 0.5)
            cs = []
            for c in contours(gl, g):
                cs.append([p for i, p in enumerate(c) if not (p[2] and c[i - 1][2] and (p[0], p[1]) == (c[i - 1][0], c[i - 1][1]))])
            apply(gl, cs)
    for n in f.getGlyphOrder():
        gl = g[n]
        if gl.numberOfContours != 0:
            gl.recalcBounds(g)
            f['hmtx'][n] = (f['hmtx'][n][0], gl.xMin)
        else:
            f['hmtx'][n] = (f['hmtx'][n][0], 0)
    best = f.getBestCmap()
    t = cmap_format_0(0)
    t.platformID, t.platEncID, t.language, t.cmap = 1, 0, 0, {}
    for b in range(256):
        t.cmap[b] = best.get(MR.get(b), '.notdef') if b in MR else '.notdef'
    f['cmap'].tables = [x for x in f['cmap'].tables if (x.platformID, x.platEncID) == (3, 1)] + [t]
    v = TTFont(io.BytesIO(data))['name'].getDebugName(5) or ''
    f['name'].setName(f"{v.split(';')[0]}; reduced for Mill (Mac Roman, unhinted, upem {UPEM})", 5, 3, 1, 0x409)
    f['post'].formatType = 3.0
    f['head'].modified = MODIFIED
    out = io.BytesIO()
    f.save(out)
    return out.getvalue(), missing


def main():
    if sys.argv[1:2] == ['--t1']:
        convert(*sys.argv[2:7])
        return
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for arg in sys.argv[2:]:
        name, src = arg.split('=', 1)
        data, missing = reduce(open(src, 'rb').read())
        open(os.path.join(out, name + '.ttf'), 'wb').write(data)
        print('%s: %d bytes%s' % (name, len(data), ', lacks ' + ' '.join(missing) if missing else ''))


if __name__ == '__main__':
    main()
