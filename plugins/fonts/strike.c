/* strike.c: one size of one style as a 'FONT'/'NFNT' resource body, laid out as
 * in Inside Macintosh I, "The Font Manager" (font records: FontRec, bit image,
 * location table, offset/width table) and Inside Macintosh IV (nDescent as the
 * high word of owTLoc).  Proportional font, fontType propFont 0x9000; chars
 * 0x20..0xFF plus the missing symbol (glyph 0, .notdef).
 *
 * Pass 1 reads the glyph header boxes (through a read-ahead block) for an
 * upper bound on the image size; pass 2 rasterizes each glyph and
 * copies its ink box into that image; then the image is packed to its real
 * width and height in place and the two tables follow it in the same block.
 * MIT license. */
#include "tt.h"
#include <string.h>

static void put16(uint8_t *p, int16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

uint8_t *tt_strike(TTFont *f, int size, uint32_t *len)
{
	TTRaster fill;
	TTBlk k;
	int16_t i, bb[4], asc0 = 0, desc0 = 0, asc = 0, desc = 0, v, w, kernMax = 0, widMax = 0, fr = 0;
	int16_t xmin, xmax, ymin, ymax;
	uint16_t j, x = 0, rb0, rb, rows0, rows;
	uint32_t wsum = 0, img0, total;
	uint8_t *blk, *img, *ow;
	int8_t inkL[TT_NCH + 1];
	uint8_t adv[TT_NCH + 1], have[TT_NCH + 1];
	uint16_t loc[TT_NCH + 2];

	if (size < 1 || size > 127) return 0;
	tt_scale(f, size);
	k.n = 0;
	for (i = 0; i <= TT_NCH; i++) {   /* pass 1: header boxes */
		if (tt_hbox(f, i, bb, &k)) continue;
		xmin = tt_sc(f, bb[0]); ymin = tt_sc(f, bb[1]); xmax = tt_sc(f, bb[2]); ymax = tt_sc(f, bb[3]);
		v = ((xmax - 32) >> 6) - ((xmin + 31) >> 6) + 2;   /* + 1 spare: composites' boxes can be off */
		if (v > 0) wsum += v;
		v = ((ymax - 32) >> 6) + 2;
		if (v > asc0) asc0 = v;
		v = -((ymin + 31) >> 6) + 1;
		if (v > desc0) desc0 = v;
	}
	rb0 = (uint16_t)(((wsum + 15) >> 4) << 1);
	rows0 = asc0 + desc0;
	img0 = (uint32_t)rb0 * rows0;
	total = 26 + img0 + 4 * (TT_NCH + 2);
	blk = tt_alloc(total);
	if (!blk) return 0;
	memset(blk, 0, total);
	img = blk + 26;
	memset(&fill, 0, sizeof fill);

	for (i = 0; i <= TT_NCH; i++) {   /* pass 2: rasterize */
		loc[i] = x; have[i] = 0; inkL[i] = 0; adv[i] = 0;
		if (tt_load(f, i)) continue;
		have[i] = 1;
		v = tt_advance(f, i);
		adv[i] = (uint8_t)(v < 0 ? 0 : v > 254 ? 254 : v);
		if (adv[i] > widMax) widMax = adv[i];
		if (!tt_render(f, &fill)) continue;
		w = fill.ix1 - fill.ix0 + 1;
		{	/* row 0 of img is asc0 pixels above the baseline */
			int16_t r, row, fits = (uint32_t)x + w <= (uint32_t)rb0 * 8;
			for (r = fill.iy0; r <= fill.iy1; r++) {
				row = asc0 - 1 - (fill.r0 + r);
				tt_copyrow(&fill, r, fits && row >= 0 && row < (int16_t)rows0 ? img + (uint16_t)row * rb0 : 0, x);
			}
			if (!fits) continue;
		}
		inkL[i] = (int8_t)(fill.c0 + fill.ix0);
		x += w;
		if (inkL[i] < kernMax) kernMax = inkL[i];
		if (inkL[i] + w > fr) fr = inkL[i] + w;
		v = fill.r0 + fill.iy1 + 1;
		if (v > asc) asc = v;
		v = -(fill.r0 + fill.iy0);
		if (v > desc) desc = v;
	}
	loc[TT_NCH + 1] = x;
	tt_rfree(&fill);
	if (asc > asc0) asc = asc0;
	if (desc > desc0) desc = desc0;

	/* pack the image to its real width and rows, in place */
	rb = (uint16_t)(((x + 15) >> 4) << 1);
	rows = asc + desc;
	for (j = 0; j < rows; j++)
		memmove(img + (uint32_t)j * rb, img + (uint32_t)(j + asc0 - asc) * rb0, rb);
	img0 = (uint32_t)rb * rows;
	for (j = 0; j < TT_NCH + 2; j++) put16(img + img0 + 2 * j, (int16_t)loc[j]);
	ow = img + img0 + 2 * (TT_NCH + 2);
	for (j = 0; j <= TT_NCH; j++)
		put16(ow + 2 * j, have[j] ? (int16_t)(((inkL[j] - kernMax) << 8) | adv[j]) : -1);
	put16(ow + 2 * (TT_NCH + 1), -1);

	put16(blk, (int16_t)0x9000);          /* fontType: propFont */
	put16(blk + 2, TT_FIRST);
	put16(blk + 4, TT_LAST);
	put16(blk + 6, widMax);
	put16(blk + 8, kernMax);
	total = (uint32_t)(ow - blk - 16) >> 1;   /* owTLoc: words from itself to owTable */
	put16(blk + 10, (int16_t)(total >> 16));  /* nDescent: its high word */
	put16(blk + 12, fr - kernMax);         /* fRectWidth */
	put16(blk + 14, rows);                 /* fRectHeight */
	put16(blk + 16, (int16_t)total);
	put16(blk + 18, asc);
	put16(blk + 20, desc);
	/* leading: so that a line is as tall as the TrueType ascender + descender + lineGap */
	v = (tt_sc(f, f->hAsc - f->hDesc + f->hGap) + 32) >> 6;
	put16(blk + 22, v > rows ? v - rows : 0);
	put16(blk + 24, rb >> 1);              /* rowWords */
	*len = (uint32_t)(ow - blk) + 2 * (TT_NCH + 2);
	return blk;
}
