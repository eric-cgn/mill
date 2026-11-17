/* glyph.c: one glyph at a time, for a cache filled as text needs it (the way
 * the TrueType scaler renders lazily) rather than a whole strike up front.
 * Same pixels, advance and ink placement as the strike.  MIT license. */
#include "tt.h"
#include <string.h>

/* ci: character index (code - TT_FIRST, or TT_NOTDEF), at the size set by tt_scale */
TTGlyph *tt_glyph(TTFont *f, TTRaster *s, uint16_t ci)
{
	TTGlyph *g;
	int16_t v, r, w = 0, h = 0, rb = 0;
	if (ci > TT_NOTDEF || tt_load(f, ci)) return 0;
	v = tt_advance(f, ci);
	if (f->np && tt_render(f, s)) {
		w = s->ix1 - s->ix0 + 1; h = s->iy1 - s->iy0 + 1; rb = (w + 7) >> 3;
	}
	g = tt_alloc(offsetof(TTGlyph, bits) + (uint16_t)(rb * h));
	if (!g) { for (r = s->iy0; h && r <= s->iy1; r++) tt_copyrow(s, r, 0, 0); return 0; }
	memset(g, 0, offsetof(TTGlyph, bits) + (uint16_t)(rb * h));
	g->adv = (uint8_t)(v < 0 ? 0 : v > 254 ? 254 : v);
	if (h) {
		g->left = (int8_t)(s->c0 + s->ix0);
		g->top = (int8_t)(s->r0 + s->iy1 + 1);
		g->w = (uint8_t)w; g->h = (uint8_t)h; g->rowBytes = (uint8_t)rb;
		for (r = s->iy1; r >= s->iy0; r--) tt_copyrow(s, r, g->bits + (uint16_t)((s->iy1 - r) * rb), 0);
	}
	return g;
}
