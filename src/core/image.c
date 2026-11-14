#include <stdlib.h>
#include <string.h>

#include "image.h"

#define MAX_SIDE 2048
#define MAX_PIXELS (256L * 1024)

/* ------------------------------------------------------------ scaling */

/* A grayscale picture at the target size. Each source pixel fills the target pixels
   whose centers fall in it: plain nearest-neighbour, cheap on a 68000. */
typedef struct {
    unsigned char *px;
    short w, h, sw, sh;
    short *xs, *ys;     /* source column/row -> first target column/row; sw+1 and sh+1 long */
    unsigned char *mask;    /* a Bitmap1 mask at the target size, or NULL; see put_mask */
} Gray;

/* 0, -1 for a size Mill won't make, or IMAGE_NO_MEMORY */
static int gray_init(Gray *g, short w, short h, short sw, short sh)
{
    long i;
    memset(g, 0, sizeof *g);
    if (w < 1 || h < 1 || sw < 1 || sh < 1 || w > MAX_SIDE || h > MAX_SIDE || (long)w * h > MAX_PIXELS) return -1;
    g->w = w; g->h = h; g->sw = sw; g->sh = sh;
    g->px = malloc((long)w * h);
    g->xs = malloc((sw + 1) * sizeof(short));
    g->ys = malloc((sh + 1) * sizeof(short));
    if (!g->px || !g->xs || !g->ys) return IMAGE_NO_MEMORY;
    memset(g->px, 255, (long)w * h);
    for (i = 0; i <= sw; i++) g->xs[i] = (short)((i * w * 2 + sw) / (2L * sw) > w ? w : (i * w * 2 + sw) / (2L * sw));
    for (i = 0; i <= sh; i++) g->ys[i] = (short)((i * h * 2 + sh) / (2L * sh) > h ? h : (i * h * 2 + sh) / (2L * sh));
    g->xs[sw] = w;
    g->ys[sh] = h;
    return 0;
}

static void gray_free(Gray *g)
{
    free(g->px); free(g->xs); free(g->ys); free(g->mask);
}

static short row_bytes(short w) { return (short)((w + 15) / 16 * 2); }

/* Start a mask, covering nothing: a GIF's frame may not fill its whole picture */
static int mask_init(Gray *g)
{
    g->mask = calloc((long)row_bytes(g->w) * g->h, 1);
    return g->mask ? 0 : -1;
}

/* Mark the target pixels of source pixel (sx, sy) covered, as put fills them */
static void put_mask(Gray *g, long sx, long sy)
{
    short x, y, rb = row_bytes(g->w);
    if (sx < 0 || sy < 0 || sx >= g->sw || sy >= g->sh) return;
    for (y = g->ys[sy]; y < g->ys[sy + 1]; y++)
        for (x = g->xs[sx]; x < g->xs[sx + 1]; x++) g->mask[(long)y * rb + (x >> 3)] |= 0x80 >> (x & 7);
}

/* Hand g's mask to out, with out's bits cleared where it's 0; none if it covers it all */
static void take_mask(Gray *g, Bitmap1 *out)
{
    long i, n = (long)out->row_bytes * out->h, all = 1;
    short x, y;
    if (!g->mask) return;
    /* the padding at the end of each row counts as covered, so a full mask is all ones */
    for (y = 0; y < out->h; y++)
        for (x = out->w; x < out->row_bytes * 8; x++) g->mask[(long)y * out->row_bytes + (x >> 3)] |= 0x80 >> (x & 7);
    for (i = 0; i < n; i++) {
        out->bits[i] &= g->mask[i];
        if (g->mask[i] != 0xFF) all = 0;
    }
    if (all) return;
    out->mask = g->mask;
    g->mask = NULL;
}

static void put(Gray *g, long sx, long sy, unsigned char v)
{
    short x, y;
    if (sx < 0 || sy < 0 || sx >= g->sw || sy >= g->sh) return;
    for (y = g->ys[sy]; y < g->ys[sy + 1]; y++)
        for (x = g->xs[sx]; x < g->xs[sx + 1]; x++) g->px[(long)y * g->w + x] = v;
}

/* ------------------------------------------------------------ dithering */

/* Stretch a photo's tones so its darkest hundredth is black and its brightest white:
   in one bit, a picture that's all mid-grays is all checkerboard */
static void levels(Gray *g)
{
    long hist[256], n = (long)g->w * g->h, cut = n / 100, seen, i;
    int lo = 0, hi = 255;
    unsigned char map[256];
    memset(hist, 0, sizeof hist);
    for (i = 0; i < n; i++) hist[g->px[i]]++;
    for (seen = 0; lo < 255 && (seen += hist[lo]) <= cut; lo++) {}
    for (seen = 0; hi > 0 && (seen += hist[hi]) <= cut; hi--) {}
    if (hi - lo < 16) return;
    for (i = 0; i < 256; i++) map[i] = (unsigned char)(i <= lo ? 0 : i >= hi ? 255 : (i - lo) * 255L / (hi - lo));
    for (i = 0; i < n; i++) g->px[i] = map[g->px[i]];
}

/* Atkinson's: an eighth of the error to each of six neighbours, the rest dropped,
   which keeps light areas clean and gives the crisp look of early Mac pictures */
static int dither(const Gray *g, Bitmap1 *out)
{
    short w = g->w, h = g->h, x, y;
    short *rows = calloc(3 * (w + 3), sizeof(short));
    short *e0, *e1, *e2, *t;
    out->w = w;
    out->h = h;
    out->row_bytes = row_bytes(w);
    out->bits = calloc((long)out->row_bytes * h, 1);
    if (!rows || !out->bits) { free(rows); free(out->bits); out->bits = NULL; return IMAGE_NO_MEMORY; }
    e0 = rows + 1; e1 = e0 + w + 3; e2 = e1 + w + 3;
    for (y = 0; y < h; y++) {
        const unsigned char *src = g->px + (long)y * w;
        unsigned char *dst = out->bits + (long)y * out->row_bytes;
        for (x = 0; x < w; x++) {
            short v = src[x] + e0[x], err;
            if (v < 128) { dst[x >> 3] |= 0x80 >> (x & 7); err = v; } else err = v - 255;
            err >>= 3;
            e0[x + 1] += err; e0[x + 2] += err;
            e1[x - 1] += err; e1[x] += err; e1[x + 1] += err;
            e2[x] += err;
        }
        t = e0; e0 = e1; e1 = e2; e2 = t;
        memset(e2 - 1, 0, (w + 3) * sizeof(short));
    }
    free(rows);
    return 0;
}

/* ------------------------------------------------------------ the canvas */

struct ImageCanvas { Gray g; };

void image_fit(short sw, short sh, short aw, short ah, short *w, short *h)
{
    long W, H;
    if (aw > 0 && ah > 0) { W = aw; H = ah; }
    else if (aw > 0) { W = aw; H = sh > 0 && sw > 0 ? (long)sh * aw / sw : aw; }
    else if (ah > 0) { H = ah; W = sw > 0 && sh > 0 ? (long)sw * ah / sh : ah; }
    else { W = sw; H = sh; }
    if (W < 1) W = 1;
    if (H < 1) H = 1;
    /* too big for Mill: smaller, keeping its proportions */
    while (W > MAX_SIDE || H > MAX_SIDE || W * H > MAX_PIXELS) {
        W = W * 7 / 8;
        H = H * 7 / 8;
        if (W < 1) W = 1;
        if (H < 1) H = 1;
    }
    *w = (short)W;
    *h = (short)H;
}

int canvas_new(ImageCanvas **c, short w, short h, short sw, short sh)
{
    int err;
    if (!(*c = malloc(sizeof **c))) return IMAGE_NO_MEMORY;
    if ((err = gray_init(&(*c)->g, w, h, sw, sh)) != 0) { canvas_free(*c); *c = NULL; }
    return err;
}

int canvas_source(ImageCanvas *c, short sw, short sh)
{
    Gray *g = &c->g;
    short *xs, *ys;
    long i;
    if (sw < 1 || sh < 1) return -1;
    xs = malloc((sw + 1) * sizeof(short));
    ys = malloc((sh + 1) * sizeof(short));
    if (!xs || !ys) { free(xs); free(ys); return IMAGE_NO_MEMORY; }
    free(g->xs);
    free(g->ys);
    g->xs = xs;
    g->ys = ys;
    g->sw = sw;
    g->sh = sh;
    for (i = 0; i <= sw; i++) xs[i] = (short)((i * g->w * 2 + sw) / (2L * sw) > g->w ? g->w : (i * g->w * 2 + sw) / (2L * sw));
    for (i = 0; i <= sh; i++) ys[i] = (short)((i * g->h * 2 + sh) / (2L * sh) > g->h ? g->h : (i * g->h * 2 + sh) / (2L * sh));
    xs[sw] = g->w;
    ys[sh] = g->h;
    return 0;
}

void canvas_size(const ImageCanvas *c, short *w, short *h) { *w = c->g.w; *h = c->g.h; }

void canvas_rect(ImageCanvas *c, short x, short y, short w, short h, const unsigned char *gray, const unsigned char *opaque)
{
    Gray *g = &c->g;
    short i, j;
    /* the first pixels say whether there's a mask: it starts covering nothing */
    if (opaque && !g->mask && mask_init(g)) opaque = NULL;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            put(g, x + i, y + j, gray[(long)j * w + i]);
            if (g->mask && (!opaque || opaque[(long)j * w + i])) put_mask(g, x + i, y + j);
        }
}

int canvas_finish(ImageCanvas *c, int photo, Bitmap1 *out)
{
    int err;
    memset(out, 0, sizeof *out);
    if (photo) levels(&c->g);
    if ((err = dither(&c->g, out)) == 0) take_mask(&c->g, out);
    return err;
}

void canvas_free(ImageCanvas *c)
{
    if (!c) return;
    gray_free(&c->g);
    free(c);
}

void image_free(Bitmap1 *b)
{
    free(b->bits);
    free(b->mask);
    b->bits = b->mask = NULL;
}

int image_icon(const char *type, const unsigned char *data, long len, Bitmap1 *out)
{
    short side, masked;
    long n;
    if (!memcmp(type, "ics#", 4)) { side = 16; masked = 1; }
    else if (!memcmp(type, "ICN#", 4)) { side = 32; masked = 1; }
    else if (!memcmp(type, "ICON", 4)) { side = 32; masked = 0; }
    else if (!memcmp(type, "SICN", 4)) { side = 16; masked = 0; }
    else return -1;
    n = (long)side * side / 8;  /* rows of 2 or 4 bytes: QuickDraw's layout already */
    if (len < n * (masked + 1)) return -1;
    memset(out, 0, sizeof *out);
    out->w = out->h = side;
    out->row_bytes = side / 8;
    if (!(out->bits = malloc(n))) return IMAGE_NO_MEMORY;
    memcpy(out->bits, data, n);
    if (masked) {
        long i;
        if (!(out->mask = malloc(n))) { free(out->bits); out->bits = NULL; return IMAGE_NO_MEMORY; }
        memcpy(out->mask, data + n, n);
        for (i = 0; i < n; i++) out->bits[i] &= out->mask[i];   /* bits are 0 where the page shows */
    }
    return 0;
}
