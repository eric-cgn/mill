/* tt.h: minimal TrueType-to-bitmap rasterizer for the 68000, version 2.
 * Integer only: no floats, and no 32x32 multiplies or 32/32 divides in the
 * per-point or per-scanline paths (68000: MULS.W 16x16->32, DIVS.W 32/16).
 * Gives the same bitmaps as version 1, faster: one read per glyph record,
 * a per-character table read at open, a non-recursive flattener feeding one
 * edge loop, and a sweep that skips empty cells a long word at a time and
 * writes bits.  Two outputs: a whole 'FONT'/'NFNT' strike, or one glyph at
 * a time for a cache (tt_glyph).
 * MIT license, OneInAMillion Software. */
#ifndef TT_H
#define TT_H
#include <stdint.h>
#include <stddef.h>

/* reads len bytes at file offset off; returns 0 on success */
typedef int (*tt_read_fn)(void *ctx, uint32_t off, uint16_t len, void *buf);

#define TT_FIRST 32   /* strike covers Mac Roman 0x20..0xFF */
#define TT_LAST 255
#define TT_NCH (TT_LAST - TT_FIRST + 1)
#define TT_NOTDEF TT_NCH   /* character index of the missing symbol (glyph 0) */

typedef struct {
	uint32_t off;      /* glyph record, file offset; 0xFFFFFFFF = can't be located */
	uint16_t len;      /* record length; under 10 = empty */
	uint16_t aw;       /* advance width, font units */
} TTChar;

/* read-ahead block for runs of small reads near each other */
typedef struct { uint32_t base; uint16_t n; uint8_t b[256]; } TTBlk;

typedef struct {
	tt_read_fn read; void *ctx;
	uint32_t glyf, loca, hmtx;
	uint16_t upem, numGlyphs, numHMetrics;
	int16_t locaLong;
	int16_t hAsc, hDesc, hGap;         /* hhea, font units */
	uint16_t maxPts, maxCont, maxDepth;
	uint16_t gid[TT_NCH];              /* glyph per Mac Roman code, 0 = missing */
	TTChar *ch;                        /* TT_NCH + 1 entries, the last for glyph 0 */
	/* scaling: v26.6 = (fu * m + half) >> k, m and k reduced (m odd or k 0) */
	int16_t m; uint8_t k;
	/* current glyph */
	int16_t *x, *y; uint8_t *on; uint16_t *ends;
	uint16_t np, nc;
	uint8_t *gbuf; uint16_t gbufLen;
} TTFont;

/* parse.c */
int tt_open(TTFont *f, tt_read_fn rd, void *ctx);   /* 0 ok */
void tt_close(TTFont *f);
void tt_scale(TTFont *f, int ppem);
int tt_load(TTFont *f, uint16_t ci);                 /* character index; points in 26.6 pixels; 0 ok */
int16_t tt_advance(TTFont *f, uint16_t ci);          /* whole pixels */
int tt_hbox(TTFont *f, uint16_t ci, int16_t bb[4], TTBlk *k);   /* header box, font units; 0 ok */
int16_t tt_sc(TTFont *f, int16_t v);                 /* font units -> 26.6 */
uint16_t tt_rd16(const uint8_t *p);

/* raster.c: one glyph's outline to bits */
#ifndef NCL
#define NCL 256    /* column crossings listed per glyph (36 pt Tinos needs 162); past that the sweep scans every column cell */
#endif
#ifdef TT_DROPOUT
#define NBUF 3
#else
#define NBUF 1
#endif
typedef struct {
	uint8_t *blk;      /* one block for all of these */
	uint8_t *rg;       /* row cells, 2 bytes each (winding, crossing kind), row-major, rs bytes a row */
	uint8_t *cg;       /* column cells, 1 byte each (crossing kind), column-major, 1 << csh bytes a column */
	uint8_t *bits;     /* the glyph, rb bytes a row, row 0 at the bottom */
	uint8_t *tm;       /* touched row cells, a bit each (low bit first), rb bytes a row */
	uint16_t *cl;      /* touched column cells (column << csh | row), ncl of them; more than NCL: too many */
	uint16_t size, ncl;   /* bytes allocated */
	int16_t c0, r0, w, h, rs, csh, rb; /* first column and row (y up), size */
	int16_t ix0, ix1, iy0, iy1;        /* ink box after tt_render (cells) */
} TTRaster;
/* renders the loaded glyph (f->x, f->y) into r; 1 if any ink. Clears its
 * cells as it goes; r->bits keeps the ink until tt_copy is called */
int tt_render(TTFont *f, TTRaster *r);
/* copies ink row iy (iy0..iy1) to dst at bit x and clears it in r->bits */
void tt_copyrow(TTRaster *r, int16_t iy, uint8_t *dst, uint16_t x);
void tt_rfree(TTRaster *r);

/* strike.c: builds a 'FONT'/'NFNT' resource body; returns it or 0 */
uint8_t *tt_strike(TTFont *f, int size, uint32_t *len);

/* glyph.c: one glyph for a cache, at the size set by tt_scale */
typedef struct {
	int8_t left;       /* ink left edge from the origin, pixels */
	int8_t top;        /* first row's distance above the baseline, pixels (row 0 is top) */
	uint8_t w, h;      /* ink size; 0 x 0 for a blank glyph */
	uint8_t adv;       /* advance, pixels */
	uint8_t rowBytes;
	uint8_t bits[1];   /* h rows of rowBytes, leftmost pixel = bit 7 */
} TTGlyph;
TTGlyph *tt_glyph(TTFont *f, TTRaster *r, uint16_t ci);   /* tt_free it; 0 = missing or no memory */

/* memory hooks */
void *tt_alloc(uint32_t n);
void tt_free(void *p);
extern uint32_t tt_peak, tt_cur;   /* bytes, for measuring */
#endif
