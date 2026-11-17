/* parse.c: table directory, head/hhea/maxp/hmtx/loca/cmap, glyph loading.
 * Written from the Apple TrueType Reference Manual, chapter 6 "Font Files":
 * the table directory; 'head' (unitsPerEm, indexToLocFormat), 'hhea'
 * (ascent, descent, lineGap, numOfLongHorMetrics), 'maxp' (numGlyphs,
 * maxPoints, maxContours, maxComponentDepth), 'hmtx', 'loca', 'cmap'
 * (formats 0 and 4), and 'glyf' (simple glyph flags, composite glyph
 * component flags and transforms).
 * Version 2: a per-character table (glyph offset and length, advance) built
 * once at open through a small read-ahead block, so a glyph costs
 * one read of its whole record; scaling factors reduced to the smallest exact
 * multiplier and shift.  MIT license. */
#include "tt.h"
#include <string.h>

uint16_t tt_rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) { return ((uint32_t)tt_rd16(p) << 16) | tt_rd16(p + 2); }

/* Mac Roman 0x80..0xFF to Unicode (Apple's ROMAN.TXT mapping, Mac OS 8.5+ euro at 0xDB
 * replaced by the period currency sign U+00A4 as in 1997) */
static const uint16_t macroman[128] = {
	0xC4,0xC5,0xC7,0xC9,0xD1,0xD6,0xDC,0xE1,0xE0,0xE2,0xE4,0xE3,0xE5,0xE7,0xE9,0xE8,
	0xEA,0xEB,0xED,0xEC,0xEE,0xEF,0xF1,0xF3,0xF2,0xF4,0xF6,0xF5,0xFA,0xF9,0xFB,0xFC,
	0x2020,0xB0,0xA2,0xA3,0xA7,0x2022,0xB6,0xDF,0xAE,0xA9,0x2122,0xB4,0xA8,0x2260,0xC6,0xD8,
	0x221E,0xB1,0x2264,0x2265,0xA5,0xB5,0x2202,0x2211,0x220F,0x3C0,0x222B,0xAA,0xBA,0x3A9,0xE6,0xF8,
	0xBF,0xA1,0xAC,0x221A,0x192,0x2248,0x2206,0xAB,0xBB,0x2026,0xA0,0xC0,0xC3,0xD5,0x152,0x153,
	0x2013,0x2014,0x201C,0x201D,0x2018,0x2019,0xF7,0x25CA,0xFF,0x178,0x2044,0xA4,0x2039,0x203A,0xFB01,0xFB02,
	0x2021,0xB7,0x201A,0x201E,0x2030,0xC2,0xCA,0xC1,0xCB,0xC8,0xCD,0xCE,0xCF,0xCC,0xD3,0xD4,
	0xF8FF,0xD2,0xDA,0xDB,0xD9,0x131,0x2C6,0x2DC,0xAF,0x2D8,0x2D9,0x2DA,0xB8,0x2DD,0x2DB,0x2C7
};

static int rd(TTFont *f, uint32_t off, uint16_t len, void *buf) { return f->read(f->ctx, off, len, buf); }

/* cmap format 4 (segment mapping to delta values) for one code; seg holds
 * endCode[n], pad, startCode[n], idDelta[n], idRangeOffset[n] */
static uint16_t cmap4(TTFont *f, uint32_t sub, const uint8_t *seg, uint16_t n, uint16_t c)
{
	uint16_t i, ro, g;
	uint8_t b[2];
	for (i = 0; i < n; i++) {
		if (tt_rd16(seg + 2 * i) < c) continue;
		if (tt_rd16(seg + 2 * n + 2 + 2 * i) > c) return 0;
		ro = tt_rd16(seg + 6 * n + 2 + 2 * i);
		if (!ro) return (uint16_t)(c + tt_rd16(seg + 4 * n + 2 + 2 * i));
		/* idRangeOffset is relative to its own position in the subtable */
		if (rd(f, sub + 14 + 6 * (uint32_t)n + 2 + 2 * i + ro +
		       2 * (uint32_t)(c - tt_rd16(seg + 2 * n + 2 + 2 * i)), 2, b)) return 0;
		g = tt_rd16(b);
		return g ? (uint16_t)(g + tt_rd16(seg + 4 * n + 2 + 2 * i)) : 0;
	}
	return 0;
}

static int cmap(TTFont *f, uint32_t cm)
{
	uint8_t b[8], *seg;
	uint16_t n, i, c, best = 0;
	uint32_t sub = 0, off;
	if (rd(f, cm, 4, b)) return -1;
	n = tt_rd16(b + 2);
	for (i = 0; i < n; i++) {   /* prefer Unicode format 4, else Mac Roman format 0 */
		if (rd(f, cm + 4 + 8 * (uint32_t)i, 8, b)) return -1;
		off = cm + rd32(b + 4);
		c = (uint16_t)((tt_rd16(b) << 8) | tt_rd16(b + 2));
		if (c == 0x0301 || c == 0x0003 || c == 0x0004) { uint8_t h[2]; rd(f, off, 2, h);
			if (tt_rd16(h) == 4) { sub = off; best = 4; } }
		else if (c == 0x0100 && !best) { sub = off; best = 1; }
	}
	if (best == 4) {
		if (rd(f, sub + 6, 2, b)) return -1;
		n = tt_rd16(b) / 2;
		seg = tt_alloc(8 * (uint32_t)n + 2);
		if (!seg || rd(f, sub + 14, (uint16_t)(8 * n + 2), seg)) { tt_free(seg); return -1; }
		for (c = TT_FIRST; c <= TT_LAST; c++)
			f->gid[c - TT_FIRST] = c == 0x7F ? 0 :
				cmap4(f, sub, seg, n, c < 0x80 ? c : macroman[c - 0x80]);
		tt_free(seg);
		return 0;
	}
	if (best == 1) {
		uint8_t m[TT_NCH];
		if (rd(f, sub + 6 + TT_FIRST, TT_NCH, m)) return -1;
		for (i = 0; i < TT_NCH; i++) f->gid[i] = m[i];
		return 0;
	}
	return -1;
}


#define BAD 0xFFFFFFFFUL   /* TTChar.off: glyph can't be located */

/* read-ahead block for building the character table at open: the loca, hmtx
 * and glyph header reads are small and mostly near each other */
typedef TTBlk Blk;
static int brd(TTFont *f, Blk *k, uint32_t off, uint16_t len, void *out)
{
	if (off >= k->base && off + len <= k->base + k->n) { memcpy(out, k->b + (uint16_t)(off - k->base), len); return 0; }
	k->n = 0;
	if (len <= sizeof k->b && !rd(f, off, sizeof k->b, k->b)) {   /* fails near the end of the file */
		k->base = off; k->n = sizeof k->b; memcpy(out, k->b, len); return 0;
	}
	return rd(f, off, len, out);
}

static int locate(TTFont *f, Blk *k, uint16_t g, uint32_t *off, uint32_t *len)
{
	uint8_t b[8];
	if (g >= f->numGlyphs) return -1;
	if (f->locaLong) {
		if (k ? brd(f, k, f->loca + 4 * (uint32_t)g, 8, b) : rd(f, f->loca + 4 * (uint32_t)g, 8, b)) return -1;
		*off = rd32(b); *len = rd32(b + 4) - *off;
	} else {
		if (k ? brd(f, k, f->loca + 2 * (uint32_t)g, 4, b) : rd(f, f->loca + 2 * (uint32_t)g, 4, b)) return -1;
		*off = 2 * (uint32_t)tt_rd16(b); *len = 2 * (uint32_t)tt_rd16(b + 2) - *off;
	}
	*off += f->glyf;
	return 0;
}

static void chars(TTFont *f)
{
	Blk k;
	TTChar *c;
	uint16_t i, g;
	uint32_t off, len;
	uint8_t b[10];
	k.n = 0;
	for (i = 0, c = f->ch; i <= TT_NCH; i++, c++) {   /* loca */
		g = i < TT_NCH ? f->gid[i] : 0;
		c->off = BAD; c->len = 0; c->aw = 0;
		if (i < TT_NCH && !g) continue;
		if (locate(f, &k, g, &off, &len) || len > 65535) continue;
		c->off = off; c->len = (uint16_t)len;
	}
	k.n = 0;
	for (i = 0, c = f->ch; i <= TT_NCH; i++, c++) {   /* hmtx */
		if (c->off == BAD) continue;
		g = i < TT_NCH ? f->gid[i] : 0;
		if (g >= f->numHMetrics) g = f->numHMetrics - 1;
		if (brd(f, &k, f->hmtx + 4 * (uint32_t)g, 2, b)) continue;
		c->aw = tt_rd16(b);
		if (c->aw > 32767) c->aw = 32767;
	}
}

/* the 10-byte glyph header's box: through k, a read-ahead block (k->n = 0 to start) */
int tt_hbox(TTFont *f, uint16_t ci, int16_t bb[4], TTBlk *k)
{
	uint8_t b[10];
	TTChar *c = &f->ch[ci];
	if (c->off == BAD || c->len < 10 || brd(f, k, c->off, 10, b)) return -1;
	bb[0] = (int16_t)tt_rd16(b + 2); bb[1] = (int16_t)tt_rd16(b + 4);
	bb[2] = (int16_t)tt_rd16(b + 6); bb[3] = (int16_t)tt_rd16(b + 8);
	return 0;
}

int tt_open(TTFont *f, tt_read_fn read, void *ctx)
{
	uint8_t b[36];
	uint16_t n, i;
	uint32_t tag, off, head = 0, hhea = 0, maxp = 0, cm = 0, gl;
	memset(f, 0, sizeof *f);
	f->read = read; f->ctx = ctx;
	if (rd(f, 4, 2, b)) return -1;
	n = tt_rd16(b);
	for (i = 0; i < n; i++) {
		if (rd(f, 12 + 16 * (uint32_t)i, 16, b)) return -1;
		tag = rd32(b); off = rd32(b + 8);
		if (tag == 0x68656164) head = off;        /* head */
		else if (tag == 0x68686561) hhea = off;   /* hhea */
		else if (tag == 0x6D617870) maxp = off;   /* maxp */
		else if (tag == 0x686D7478) f->hmtx = off;
		else if (tag == 0x6C6F6361) f->loca = off;
		else if (tag == 0x676C7966) f->glyf = off;
		else if (tag == 0x636D6170) cm = off;
	}
	if (!head || !hhea || !maxp || !f->hmtx || !f->loca || !f->glyf || !cm) return -1;
	if (rd(f, head + 18, 2, b)) return -1;
	f->upem = tt_rd16(b);
	if (rd(f, head + 50, 2, b)) return -1;
	f->locaLong = (int16_t)tt_rd16(b);
	if (rd(f, hhea + 4, 6, b)) return -1;
	f->hAsc = (int16_t)tt_rd16(b); f->hDesc = (int16_t)tt_rd16(b + 2); f->hGap = (int16_t)tt_rd16(b + 4);
	if (rd(f, hhea + 34, 2, b)) return -1;
	f->numHMetrics = tt_rd16(b);
	if (rd(f, maxp, 32, b)) return -1;
	f->numGlyphs = tt_rd16(b + 4);
	f->maxPts = tt_rd16(b + 6);
	if (tt_rd16(b + 10) > f->maxPts) f->maxPts = tt_rd16(b + 10);
	f->maxCont = tt_rd16(b + 8);
	if (tt_rd16(b + 12) > f->maxCont) f->maxCont = tt_rd16(b + 12);
	f->maxDepth = tt_rd16(b + 30);
	if (f->upem < 16 || !f->numHMetrics || cmap(f, cm)) return -1;
	/* a whole record without instructions: header, end points, flags and two 2-byte
	 * coordinates per point, alignment padding */
	gl = 12 + 2 * (uint32_t)f->maxCont + 5 * (uint32_t)f->maxPts + 4;
	if (gl > 65535) return -1;
	f->gbufLen = (uint16_t)gl;
	/* point arrays: + 1 so a composite can be checked before it overflows */
	f->x = tt_alloc(4 * ((uint32_t)f->maxPts + 1));
	f->y = f->x + f->maxPts + 1;
	f->on = tt_alloc(f->maxPts + 1);
	f->ends = tt_alloc(2 * ((uint32_t)f->maxCont + 1));
	f->gbuf = tt_alloc(f->gbufLen);
	f->ch = tt_alloc((TT_NCH + 1) * sizeof(TTChar));
	if (!f->x || !f->on || !f->ends || !f->gbuf || !f->ch) return -1;
	chars(f);
	return 0;
}

void tt_close(TTFont *f)
{
	tt_free(f->x); tt_free(f->on); tt_free(f->ends); tt_free(f->gbuf); tt_free(f->ch);
	f->x = 0; f->on = 0; f->ends = 0; f->gbuf = 0; f->ch = 0;
}

/* choose m (<= 32767) and k so that fu * m >> k == fu * ppem * 64 / upem, then
 * drop common factors of 2: the same results with a shorter shift (none at
 * all for upem 256 at sizes divisible by 4) */
void tt_scale(TTFont *f, int ppem)
{
	uint32_t num = (uint32_t)ppem << 6;
	uint8_t k = 0;
	int16_t m;
	while (k < 24 && num < (1UL << (29 - k)) && ((num << (k + 1)) / f->upem) < 32768) k++;
	m = (int16_t)(((num << k) + f->upem / 2) / f->upem);
	while (k && !(m & 1)) { m >>= 1; k--; }
	f->k = k; f->m = m;
}

int16_t tt_sc(TTFont *f, int16_t v)
{
	return (int16_t)(((int32_t)v * f->m + (f->k ? 1L << (f->k - 1) : 0)) >> f->k);
}

int16_t tt_advance(TTFont *f, uint16_t ci)
{
	return (int16_t)(((int32_t)(int16_t)f->ch[ci].aw * f->m + (1L << (f->k + 5))) >> (f->k + 6));   /* rounded once */
}

/* simple glyph from its record (got bytes of it in gbuf): appended at f->np / f->nc, font units */
static int simple(TTFont *f, uint32_t off, uint32_t len, uint16_t got, int16_t nc)
{
	uint8_t *p, *q, fl, rep, *o;
	uint16_t i, np, base = f->np, ins, need;
	uint32_t ds;
	int16_t v, *X;
	if ((uint32_t)f->nc + nc > f->maxCont) return -1;
	if (12 + 2 * (uint32_t)nc > got) return -1;
	p = f->gbuf + 10;
	np = tt_rd16(p + 2 * (nc - 1)) + 1;
	if ((uint32_t)base + np > f->maxPts) return -1;
	for (i = 0; i < nc; i++, p += 2) f->ends[f->nc + i] = base + tt_rd16(p);
	ins = tt_rd16(p);   /* the instructions are skipped: no hinting */
	ds = 12 + 2 * (uint32_t)nc + ins;
	if (ds > len) return -1;
	len -= ds;
	need = len > 5 * (uint32_t)f->maxPts ? 5 * f->maxPts : (uint16_t)len;
	if (ds + need <= got) p = f->gbuf + (uint16_t)ds;
	else { if (rd(f, off + ds, need, f->gbuf)) return -1; p = f->gbuf; }   /* hinted: past the instructions */
	q = p + need;
	/* flags: bit 0 on curve, 1 x short, 2 y short, 3 repeat, 4 x same/positive, 5 y same/positive */
	o = f->on + base;
	for (i = 0; i < np;) {
		if (p >= q) return -1;
		fl = *p++; rep = 0;
		if (fl & 8) { if (p >= q) return -1; rep = *p++; }
		do o[i++] = fl; while (rep-- && i < np);
	}
	for (v = 0, X = f->x + base, i = 0; i < np; i++) {
		fl = o[i];
		if (fl & 2) { if (p >= q) return -1; if (fl & 16) v += *p++; else v -= *p++; }
		else if (!(fl & 16)) { if (p + 1 >= q) return -1; v += (int16_t)((p[0] << 8) | p[1]); p += 2; }
		X[i] = v;
	}
	for (v = 0, X = f->y + base, i = 0; i < np; i++) {
		fl = o[i];
		if (fl & 4) { if (p >= q) return -1; if (fl & 32) v += *p++; else v -= *p++; }
		else if (!(fl & 32)) { if (p + 1 >= q) return -1; v += (int16_t)((p[0] << 8) | p[1]); p += 2; }
		X[i] = v;
		o[i] = fl & 1;
	}
	f->np += np; f->nc += nc;
	return 0;
}

static int glyph(TTFont *f, uint16_t depth, uint32_t off, uint32_t len);

/* a component glyph: from the character table when it's there, else through loca */
static int component(TTFont *f, uint16_t g, uint16_t depth)
{
	uint16_t i;
	uint32_t off, len;
	if (depth > f->maxDepth + 1 || depth > 8) return -1;
	if (!g) i = TT_NOTDEF;
	else for (i = 0; i < TT_NCH && f->gid[i] != g; i++) ;
	if ((i < TT_NCH || !g) && f->ch[i].off != BAD) return glyph(f, depth, f->ch[i].off, f->ch[i].len);
	if (locate(f, 0, g, &off, &len)) return -1;
	return glyph(f, depth, off, len);
}

/* component bytes: from the record copy cb (have bytes) or, past it, the file */
static int cget(TTFont *f, const uint8_t *cb, uint16_t have, uint32_t off, uint32_t rel, uint16_t n, uint8_t *out)
{
	if (rel + n <= have) { memcpy(out, cb + (uint16_t)rel, n); return 0; }
	return rd(f, off + rel, n, out);
}

/* composite glyph: component flags ARG_1_AND_2_ARE_WORDS 1, ARGS_ARE_XY_VALUES 2,
 * WE_HAVE_A_SCALE 8, MORE_COMPONENTS 0x20, WE_HAVE_AN_X_AND_Y_SCALE 0x40,
 * WE_HAVE_A_TWO_BY_TWO 0x80.  ROUND_XY_TO_GRID and SCALED_COMPONENT_OFFSET are
 * ignored (no hinting; offsets unscaled, as Microsoft fonts expect).  The
 * record is copied out of gbuf first, since the components reuse it. */
static int composite(TTFont *f, uint32_t off, uint32_t len, uint16_t got, uint16_t depth)
{
	uint8_t b[16], cb[96];
	uint16_t fl, g, i, base, top = f->np, have = 0;
	uint32_t rel = 10;
	int16_t dx, dy, xx, xy, yx, yy, t;
	if (len <= sizeof cb && got >= len) { have = (uint16_t)len; memcpy(cb, f->gbuf, have); }
	do {
		if (cget(f, cb, have, off, rel, 4, b)) return -1;
		fl = tt_rd16(b); g = tt_rd16(b + 2);
		i = (fl & 1 ? 4 : 2) + (fl & 8 ? 2 : fl & 0x40 ? 4 : fl & 0x80 ? 8 : 0);
		if (cget(f, cb, have, off, rel + 4, i, b)) return -1;
		rel += 4 + i;
		if (fl & 1) { dx = (int16_t)tt_rd16(b); dy = (int16_t)tt_rd16(b + 2); i = 4; }
		else if (fl & 2) { dx = (int8_t)b[0]; dy = (int8_t)b[1]; i = 2; }
		else { dx = b[0]; dy = b[1]; i = 2; }
		xx = yy = 0x4000; xy = yx = 0;
		if (fl & 8) xx = yy = (int16_t)tt_rd16(b + i);
		else if (fl & 0x40) { xx = (int16_t)tt_rd16(b + i); yy = (int16_t)tt_rd16(b + i + 2); }
		else if (fl & 0x80) { xx = (int16_t)tt_rd16(b + i); yx = (int16_t)tt_rd16(b + i + 2);
			xy = (int16_t)tt_rd16(b + i + 4); yy = (int16_t)tt_rd16(b + i + 6); }
		base = f->np;
		if (component(f, g, depth + 1)) return -1;
		if (fl & 0xC8) for (i = base; i < f->np; i++) {   /* F2Dot14 transform: 16x16 multiplies */
			t = (int16_t)(((int32_t)f->x[i] * xx + (int32_t)f->y[i] * xy) >> 14);
			f->y[i] = (int16_t)(((int32_t)f->x[i] * yx + (int32_t)f->y[i] * yy) >> 14);
			f->x[i] = t;
		}
		if (!(fl & 2)) {   /* match parent point dx to component point dy */
			uint16_t pp = top + (uint16_t)dx, cp = base + (uint16_t)dy;
			if (pp >= base || cp >= f->np) return -1;
			t = f->x[pp] - f->x[cp]; dy = f->y[pp] - f->y[cp]; dx = t;
		}
		if (dx | dy) for (i = base; i < f->np; i++) { f->x[i] += dx; f->y[i] += dy; }
	} while (fl & 0x20);
	return 0;
}

/* one read for the whole record when it fits gbuf (always, without instructions) */
static int glyph(TTFont *f, uint16_t depth, uint32_t off, uint32_t len)
{
	uint16_t got;
	int16_t nc;
	if (len < 10) return 0;   /* empty glyph */
	got = len > f->gbufLen ? f->gbufLen : (uint16_t)len;
	if (rd(f, off, got, f->gbuf)) return -1;
	nc = (int16_t)tt_rd16(f->gbuf);
	if (nc > 0) return simple(f, off, len, got, nc);
	if (nc < 0) return composite(f, off, len, got, depth);
	return 0;
}

int tt_load(TTFont *f, uint16_t ci)
{
	uint16_t n;
	int16_t *p, m = f->m;
	uint8_t k = f->k;
	TTChar *c = &f->ch[ci];
	f->np = f->nc = 0;
	if (c->off == BAD || glyph(f, 0, c->off, c->len)) { f->np = f->nc = 0; return -1; }
	/* x and y are one array: scale both in one loop */
	n = f->np; p = f->x;
	if (n) {
		if (!k) {
			do { p[0] = p[0] * m; p[f->maxPts + 1] = p[f->maxPts + 1] * m; p++; } while (--n);
		} else {
			int32_t h = 1L << (k - 1);
			int16_t d = f->maxPts + 1;
			do {
				p[0] = (int16_t)(((int32_t)p[0] * m + h) >> k);
				p[d] = (int16_t)(((int32_t)p[d] * m + h) >> k);
				p++;
			} while (--n);
		}
	}
	return 0;
}
