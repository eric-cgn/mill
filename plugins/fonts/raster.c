/* raster.c: one glyph outline to a 1-bit bitmap: contour walking, curve
 * flattening, and scan conversion with nonzero winding and pixel-center
 * sampling.
 *
 * Contours: Apple TrueType Reference Manual ch. 1, "Digitizing Letterform
 * Designs" (on-curve and off-curve points, quadratic B-splines, the implied
 * on-curve midpoint between two off-curve points).  Each quadratic arc is
 * flattened by midpoint subdivision, adds and shifts only, with an explicit
 * stack instead of recursion; its lines go into a small point buffer that one
 * loop turns into scanline crossings.
 *
 * Filling: ch. 1 (a pixel is on when its center is inside the outline,
 * nonzero winding) and ch. 2's SCANCTRL/SCANTYPE description of dropout
 * control.  Each edge adds its winding (+1 going up, -1 going down) at the
 * first pixel whose center is at or right of its crossing with each scanline
 * center it spans ([ymin, ymax), so a shared vertex counts once).  For a down
 * edge it is the first center strictly right: with TrueType's clockwise
 * contours a center on either side's edge then counts as inside.  A running
 * sum along each row gives the winding number at every center; the sweep
 * skips cells with no crossing a long word at a time and sets whole runs of
 * bits between crossings.
 *
 * Dropout control (TT_DROPOUT): an ink-begin and an ink-end crossing landing
 * on the same cell with that pixel still off means a stroke thinner than a
 * pixel passed between two centers; the pixel left of it (below it, for the
 * column scan) is turned on, like the TrueType "simple" dropout rule
 * (SCANTYPE 0/1).  The column scan only records crossings, it doesn't fill;
 * its cells are stored a column at a time so both scans step the same way.
 *
 * Coordinates are moved to the cell grid's origin first, so they are never
 * negative and every sum fits 16 bits (the glyph box is limited to 250
 * pixels), and biased by 31 so a scanline center is crossed where v >> 6
 * changes.  Per edge per direction: two 32/16 divides (DIVS.W) when it
 * crosses a scanline center, then adds.  MIT license. */
#include "tt.h"
#include <string.h>

typedef uint32_t __attribute__((may_alias)) u32a;
typedef uint16_t __attribute__((may_alias)) u16a;

/* the curve lies within |p0 - 2p1 + p2| / 4 of its chord: subdivide until that
 * is at most TOL/4 in 26.6 units (TOL 16: 1/16 pixel) */
#ifndef TOL
#define TOL 16
#endif
#define NPT 48   /* points buffered before their edges are scanned */

/* marks cell c in a row's touched mask, low bit first */
#if defined(__m68k__)
#define TOUCH(m, c) __asm__("bset %1,%0" : "+m"((m)[(c) >> 3]) : "d"((int16_t)(c)) : "cc")
#else
#define TOUCH(m, c) ((m)[(c) >> 3] |= (uint8_t)(1 << ((c) & 7)))
#endif

/* floor(n / d), d > 0, the quotient known to fit 16 bits; *r = remainder >= 0 */
static int16_t fdiv(int32_t n, int16_t d, uint16_t *r)
{
	int16_t q, m;
#if defined(__m68k__)
	int32_t t = n;
	__asm__("divs.w %1,%0" : "+d"(t) : "dm"(d) : "cc");
	q = (int16_t)t; m = (int16_t)(t >> 16);
#else
	q = (int16_t)(n / d); m = (int16_t)(n % d);
#endif
	if (m < 0) { q--; m += d; }
	*r = (uint16_t)m;
	return q;
}

/* Coordinates here are biased by 31 (see tt_render), so the scanline center
 * 64j + 32 is crossed between v and v + 1 exactly when v >> 6 changes, and
 * the cell at or right of a crossing u is (u + [remainder != 0]) >> 6. */

/* the rows: winding and crossing kind per cell, and the touched mask */
static __attribute__((noinline)) void rows(TTRaster *s, const int16_t *pt, const int16_t *end)
{
	int16_t ua, va, ub, vb, j, n, du, dv, u, t, c, sq = 0, rs = s->rs, rb = s->rb;
	uint16_t r, sr = 0;
	uint8_t *p, *q, *pm;
	for (; pt + 2 < end; pt += 2) {
		va = pt[1]; vb = pt[3];
		if (!((va ^ vb) & ~63)) continue;   /* no scanline center crossed */
		if (va < vb) { ua = pt[0]; ub = pt[2]; } else { ua = pt[2]; ub = pt[0]; t = va; va = vb; vb = t; }
		j = va >> 6; n = (vb >> 6) - j;
		/* crossing at the first center, then a fixed step per scan line, as
		 * integer part plus remainder over dv (exact, like Bresenham) */
		du = ub - ua; dv = vb - va;
		u = ua + fdiv((int32_t)(int16_t)((j << 6) + 63 - va) * du, dv, &r);
		if (n > 1) sq = fdiv((int32_t)du << 6, dv, &sr);
		p = s->rg + (int16_t)(j * rs);
		pm = s->tm + (int16_t)(j * rb);
		if (pt[1] < pt[3]) for (;;) {   /* ink begins on an edge going up: first center at or right */
			t = u + (r != 0);
			c = t >> 6;
			q = p + c + c;
			q[0]++;
			TOUCH(pm, c);
#ifdef TT_DROPOUT
			q[1] = (q[1] | 0x40) + (((t & 63) + 1 - (r != 0)) >> 1);
#endif
			if (!--n) break;
			p += rs; pm += rb; u += sq; r += sr;
			if (r >= (uint16_t)dv) { r -= dv; u++; }
		} else for (;;) {                /* ends going down: first center strictly right */
			t = u + 1;
			c = t >> 6;
			q = p + c + c;
			q[0]--;
			TOUCH(pm, c);
#ifdef TT_DROPOUT
			q[1] = (q[1] | 0x80) + ((t & 63) >> 1);
#endif
			if (!--n) break;
			p += rs; pm += rb; u += sq; r += sr;
			if (r >= (uint16_t)dv) { r -= dv; u++; }
		}
	}
}

#ifdef TT_DROPOUT
/* the columns (x and y swapped): crossing kind per cell, and the cell list */
static __attribute__((noinline)) void cols(TTRaster *s, const int16_t *pt, const int16_t *end)
{
	int16_t ua, va, ub, vb, j, n, du, dv, u, t, sq = 0, csh = s->csh;
	uint16_t r, sr = 0, *l;
	uint8_t *p, *q, *cg = s->cg;
	for (; pt + 2 < end; pt += 2) {
		va = pt[0]; vb = pt[2];
		if (!((va ^ vb) & ~63)) continue;
		if (va < vb) { ua = pt[1]; ub = pt[3]; } else { ua = pt[3]; ub = pt[1]; t = va; va = vb; vb = t; }
		j = va >> 6; n = (vb >> 6) - j;
		du = ub - ua; dv = vb - va;
		u = ua + fdiv((int32_t)(int16_t)((j << 6) + 63 - va) * du, dv, &r);
		if (n > 1) sq = fdiv((int32_t)du << 6, dv, &sr);
		p = cg + (j << csh);
		/* list the cells for the sweep while there's room */
		if (s->ncl + n <= NCL) { l = s->cl + s->ncl; s->ncl += n; } else { l = 0; s->ncl = NCL + 1; }
		if (pt[2] < pt[0]) for (;;) {   /* ink begins on an edge going left */
			t = u + (r != 0);
			q = p + (t >> 6);
			*q = (*q | 0x40) + (((t & 63) + 1 - (r != 0)) >> 1);
			if (l) *l++ = (uint16_t)(q - cg);
			if (!--n) break;
			p += 1 << csh; u += sq; r += sr;
			if (r >= (uint16_t)dv) { r -= dv; u++; }
		} else for (;;) {
			t = u + 1;
			q = p + (t >> 6);
			*q = (*q | 0x80) + ((t & 63) >> 1);
			if (l) *l++ = (uint16_t)(q - cg);
			if (!--n) break;
			p += 1 << csh; u += sq; r += sr;
			if (r >= (uint16_t)dv) { r -= dv; u++; }
		}
	}
}
#endif

/* scans the lines buf[0] -> buf[1] -> ... -> end[-1] (x, y pairs); keeps the last point */
static int16_t *edges(TTRaster *s, int16_t *buf, int16_t *end)
{
	rows(s, buf, end);
#ifdef TT_DROPOUT
	cols(s, buf, end);
#endif
	buf[0] = end[-2]; buf[1] = end[-1];
	return buf + 2;
}

/* one quadratic arc from the last buffered point (x0, y0): subdivides at
 * midpoints, depth first, the right halves waiting on a stack; returns the
 * new buffer end */
static int16_t *quad(TTRaster *s, int16_t *buf, int16_t *e, int16_t x0, int16_t y0,
	int16_t x1, int16_t y1, int16_t x2, int16_t y2)
{
	int16_t st[8 * 5], *sp = st, ex, ey, d = 0;
	for (;;) {
		ex = (int16_t)(x0 + x2) - (int16_t)(x1 + x1);
		ey = (int16_t)(y0 + y2) - (int16_t)(y1 + y1);
		if (ex < 0) ex = -ex;
		if (ey < 0) ey = -ey;
		if (d > 7 || (uint16_t)(ex + ey) <= TOL) {
			if (e == buf + 2 * NPT) e = edges(s, buf, e);
			e[0] = x2; e[1] = y2; e += 2;
			if (sp == st) return e;
			x0 = x2; y0 = y2;
			sp -= 5; x1 = sp[0]; y1 = sp[1]; x2 = sp[2]; y2 = sp[3]; d = sp[4];
			continue;
		}
		d++;
		sp[0] = (uint16_t)(x1 + x2) >> 1; sp[1] = (uint16_t)(y1 + y2) >> 1;
		sp[2] = x2; sp[3] = y2; sp[4] = d; sp += 5;
		x2 = (uint16_t)(x0 + x2 + x1 + x1 + 2) >> 2; y2 = (uint16_t)(y0 + y2 + y1 + y1 + 2) >> 2;
		x1 = (uint16_t)(x0 + x1) >> 1; y1 = (uint16_t)(y0 + y1) >> 1;
	}
}

#define PUT(px, py) do { if (e == buf + 2 * NPT) e = edges(s, buf, e); e[0] = (px); e[1] = (py); e += 2; } while (0)

static void outline(TTFont *f, TTRaster *s)
{
	uint16_t c, st = 0, en, n, i, k, cnt;
	int16_t sx, sy, cx, cy, qx = 0, qy = 0, px, py, buf[2 * NPT], *e;
	int8_t ctl;
	const int16_t *X = f->x, *Y = f->y;
	const uint8_t *on = f->on;
	for (c = 0; c < f->nc; c++, st = en + 1) {
		en = f->ends[c];
		if (en < st || en >= f->np) break;
		n = en - st + 1;
		if (n < 2) continue;
		/* start on an on-curve point, or the midpoint of the first and last */
		if (on[st]) { sx = X[st]; sy = Y[st]; i = st + 1; cnt = n - 1; }
		else if (on[en]) { sx = X[en]; sy = Y[en]; i = st; cnt = n - 1; }
		else { sx = (uint16_t)(X[st] + X[en]) >> 1; sy = (uint16_t)(Y[st] + Y[en]) >> 1; i = st; cnt = n; }
		cx = sx; cy = sy; ctl = 0;
		buf[0] = sx; buf[1] = sy; e = buf + 2;
		for (k = 0; k < cnt; k++, i++) {
			if (i > en) i = st;
			px = X[i]; py = Y[i];
			if (on[i]) {
				if (ctl) e = quad(s, buf, e, cx, cy, qx, qy, px, py);
				else PUT(px, py);
				cx = px; cy = py; ctl = 0;
			} else {
				if (ctl) {
					int16_t mx = (uint16_t)(qx + px) >> 1, my = (uint16_t)(qy + py) >> 1;
					e = quad(s, buf, e, cx, cy, qx, qy, mx, my);
					cx = mx; cy = my;
				}
				qx = px; qy = py; ctl = 1;
			}
		}
		if (ctl) e = quad(s, buf, e, cx, cy, qx, qy, sx, sy);
		else PUT(sx, sy);
		edges(s, buf, e);
	}
}

/* sets bits [a, b) of a row */
static void run(uint8_t *row, int16_t a, int16_t b)
{
	uint8_t *d = row + (a >> 3);
	int16_t n = (b >> 3) - (a >> 3);
	uint8_t m = 0xFF >> (a & 7);
	if (!n) { *d |= m & ~(0xFF >> (b & 7)); return; }
	*d++ |= m;
	while (--n) *d++ = 0xFF;
	if (b & 7) *d |= ~(0xFF >> (b & 7));
}

/* lowest set bit of a byte */
static const uint8_t low[256] = {
#define L4(n) n,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0
	L4(0), 4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0, L4(5), L4(4), L4(6), L4(4), L4(5), L4(4),
	L4(7), L4(4), L4(5), L4(4), L4(6), L4(4), L4(5), L4(4)
#undef L4
};

#ifdef TT_DROPOUT
/* one column cell (offset o: column << csh | row), cleared: a begin and an end
 * crossing with the pixel off turns on that pixel or the one below.  Returns
 * the pixel to turn on, in the same form, or 0xFFFF */
static inline __attribute__((always_inline)) uint16_t ctest(TTRaster *s, uint16_t o)
{
	uint8_t k = s->cg[o];
	int16_t c, r;
	if (!k) return 0xFFFF;
	s->cg[o] = 0;
	if ((k & 0xC0) != 0xC0) return 0xFFFF;
	c = o >> s->csh; r = o & ((1 << s->csh) - 1);
	if (s->bits[(int16_t)(r * s->rb) + (c >> 3)] & (0x80 >> (c & 7))) return 0xFFFF;
	return (k & 0x3F) < 32 && r ? o - 1 : o;
}

static inline __attribute__((always_inline)) void cset(TTRaster *s, uint16_t o)
{
	int16_t c = o >> s->csh, r = o & ((1 << s->csh) - 1);
	s->bits[(int16_t)(r * s->rb) + (c >> 3)] |= 0x80 >> (c & 7);
	if (c < s->ix0) s->ix0 = c;
	if (c > s->ix1) s->ix1 = c;
	if (s->iy0 < 0 || r < s->iy0) s->iy0 = r;
	if (r > s->iy1) s->iy1 = r;
}
#endif

static __attribute__((noinline)) void sweep(TTRaster *s)
{
	int16_t r, c, e, k8, w = s->w, x0 = w, x1 = -1, y0 = -1, y1 = -1;
	uint8_t *row = s->rg, *brow = s->bits, *mrow = s->tm, *q, k, m;
	int8_t sum;
	/* rows: winding sum to bits, row dropouts, the ink box; only the touched
	 * cells are visited, in order, with runs of bits between them */
	for (r = 0; r < s->h; r++, row += s->rs, brow += s->rb, mrow += s->rb) {
		int16_t f0 = w, f1 = -1;
		sum = 0; c = 0;
		for (k8 = 0; k8 < s->rb; k8++) {
			if (!(m = mrow[k8])) continue;
			mrow[k8] = 0;
			do {
				e = (k8 << 3) + low[m];
				m &= m - 1;
				if (sum && c < e) { run(brow, c, e); if (c < f0) f0 = c; f1 = e - 1; }
				q = row + e + e;
				sum += (int8_t)q[0]; k = q[1];
				*(u16a *)q = 0;
#ifdef TT_DROPOUT
				if ((k & 0xC0) == 0xC0 && !sum) {
					int16_t d = (k & 0x3F) < 32 && e ? e - 1 : e;
					brow[d >> 3] |= 0x80 >> (d & 7);
					if (d < f0) f0 = d;
					if (d > f1) f1 = d;
				} else
#endif
				if (sum) {
					brow[e >> 3] |= 0x80 >> (e & 7);
					if (e < f0) f0 = e;
					f1 = e;
				}
				c = e + 1;
			} while (m);
		}
		if (sum && c < w) { run(brow, c, w); if (c < f0) f0 = c; f1 = w - 1; }
		if (f1 >= 0) {
			if (f0 < x0) x0 = f0;
			if (f1 > x1) x1 = f1;
			if (y0 < 0) y0 = r;
			y1 = r;
		}
	}
	s->ix0 = x0; s->ix1 = x1; s->iy0 = y0; s->iy1 = y1;
#ifdef TT_DROPOUT
	{	/* column dropouts: the pixel tests see only the row pass's bits.  The
		 * listed cells, or every cell if the list ran out */
		uint16_t n, o, t, *l = s->cl;
		if (s->ncl <= NCL) {
			/* in edge order: every test first, then the pixels, so no test sees a
			 * pixel another column dropout turned on */
			for (n = s->ncl; n; n--, l++) *l = ctest(s, *l);
			for (n = s->ncl, l = s->cl; n; n--, l++) if (*l != 0xFFFF) cset(s, *l);
		} else {
			/* a column at a time, bottom up: a dropout only turns on its own
			 * pixel or the one below, which were tested already */
			for (o = 0, n = s->w << s->csh; o < n; o++) if (s->cg[o] && (t = ctest(s, o)) != 0xFFFF) cset(s, t);
		}
		s->ncl = 0;
	}
#endif
}

int tt_render(TTFont *f, TTRaster *s)
{
	int16_t xmin, xmax, ymin, ymax, *X = f->x, *Y = f->y, dx, dy;
	uint16_t i, n = f->np, rs, cs = 0, rb, csh = 0;
	uint32_t need;
	if (!n) return 0;
	xmin = xmax = X[0]; ymin = ymax = Y[0];
	for (i = 1; i < n; i++) {
		if (X[i] < xmin) xmin = X[i]; else if (X[i] > xmax) xmax = X[i];
		if (Y[i] < ymin) ymin = Y[i]; else if (Y[i] > ymax) ymax = Y[i];
	}
	if ((int32_t)xmax - xmin > 16000 || (int32_t)ymax - ymin > 16000) return 0;
	/* cells: every pixel center inside the box, + 1 each side for dropout pixels
	 * and crossings beyond the box */
	s->c0 = ((xmin + 31) >> 6) - 1;
	s->w = ((xmax - 32) >> 6) - s->c0 + 2;
	s->r0 = ((ymin + 31) >> 6) - 1;
	s->h = ((ymax - 32) >> 6) - s->r0 + 2;
	if (s->w < 2 || s->h < 2) return 0;
	if ((uint32_t)s->w * (uint16_t)s->h > 65535 / NBUF) return 0;   /* as version 1 */
	rs = (2 * s->w + 3) & ~3;   /* long words, for the sweep */
#ifdef TT_DROPOUT
	while ((1 << csh) < s->h) csh++;   /* columns: a power of two, so a cell's offset splits with shifts */
	cs = 1 << csh;
	if (cs < 4) cs = 4, csh = 2;
#endif
	rb = ((s->w + 15) >> 4) << 1;
	/* rows, columns, the glyph's bits, each row's touched mask, the column list */
	need = (uint32_t)rs * s->h + (uint32_t)cs * s->w + 2 * (uint32_t)rb * s->h;
#ifdef TT_DROPOUT
	need += 2 * NCL;
#endif
	if (need > 65535) return 0;
	if (need > s->size) {
		tt_free(s->blk);
		s->blk = tt_alloc(need);
		s->size = s->blk ? (uint16_t)need : 0;
		if (!s->blk) return 0;
		memset(s->blk, 0, need);
	}
	/* the list first: it isn't cleared, so it must stay out of the next glyph's cells */
	s->rs = rs; s->csh = csh; s->rb = rb;
	s->cl = (uint16_t *)s->blk;
#ifdef TT_DROPOUT
	s->rg = s->blk + 2 * NCL;
#else
	s->rg = s->blk;
#endif
	s->cg = s->rg + rs * s->h;
	s->bits = s->cg + cs * s->w;
	s->tm = s->bits + rb * s->h;
	s->ncl = 0;
	/* to the grid's origin less 31: nothing negative from here on.  Every
	 * rounding step in the flattener is a floor, so it gives the same points
	 * moved by the same amount */
	dx = (s->c0 << 6) - 31; dy = (s->r0 << 6) - 31;
	for (i = 0; i < n; i++) { X[i] -= dx; Y[i] -= dy; }
	outline(f, s);
	sweep(s);
	return s->iy0 >= 0;
}

/* row iy of the ink box to dst at bit x (dst 0: just clear it); bits left of
 * ix0 and right of ix1 are all zero */
void tt_copyrow(TTRaster *s, int16_t iy, uint8_t *dst, uint16_t x)
{
	uint8_t *sp = s->bits + (int16_t)(iy * s->rb), *se = sp + (s->ix1 >> 3), *d, v, t;
	int16_t pos = x - (s->ix0 & 7), sh;
	for (sp += s->ix0 >> 3; sp <= se; sp++, pos += 8) {
		if (!(v = *sp)) continue;
		*sp = 0;
		if (!dst) continue;
		if (pos < 0) { v <<= -pos; d = dst; sh = 0; }
		else { d = dst + (pos >> 3); sh = pos & 7; }
		*d |= v >> sh;
		if (sh && (t = (uint8_t)(v << (8 - sh))) != 0) d[1] |= t;
	}
}

void tt_rfree(TTRaster *s)
{
	tt_free(s->blk);
	s->blk = 0; s->size = 0;
}
