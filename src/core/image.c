#include <stdlib.h>
#include <string.h>

#include "image.h"
#include "../third_party/tjpgd/tjpgd.h"

#define MAX_SIDE 2048
#define MAX_PIXELS (256L * 1024)

/* ------------------------------------------------------------ scaling */

/* A grayscale picture at the target size. Each source pixel fills the target pixels
   whose centers fall in it: plain nearest-neighbour, cheap on a 68000. */
typedef struct {
    unsigned char *px;
    short w, h, sw, sh;
    short *xs, *ys;     /* source column/row -> first target column/row; sw+1 and sh+1 long */
} Gray;

static int gray_init(Gray *g, short w, short h, short sw, short sh)
{
    long i;
    memset(g, 0, sizeof *g);
    if (w < 1 || h < 1 || sw < 1 || sh < 1 || w > MAX_SIDE || h > MAX_SIDE || (long)w * h > MAX_PIXELS) return -1;
    g->w = w; g->h = h; g->sw = sw; g->sh = sh;
    g->px = malloc((long)w * h);
    g->xs = malloc((sw + 1) * sizeof(short));
    g->ys = malloc((sh + 1) * sizeof(short));
    if (!g->px || !g->xs || !g->ys) return -1;
    memset(g->px, 255, (long)w * h);
    for (i = 0; i <= sw; i++) g->xs[i] = (short)((i * w * 2 + sw) / (2L * sw) > w ? w : (i * w * 2 + sw) / (2L * sw));
    for (i = 0; i <= sh; i++) g->ys[i] = (short)((i * h * 2 + sh) / (2L * sh) > h ? h : (i * h * 2 + sh) / (2L * sh));
    g->xs[sw] = w;
    g->ys[sh] = h;
    return 0;
}

static void gray_free(Gray *g)
{
    free(g->px); free(g->xs); free(g->ys);
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
    out->row_bytes = (short)((w + 15) / 16 * 2);
    out->bits = calloc((long)out->row_bytes * h, 1);
    if (!rows || !out->bits) { free(rows); free(out->bits); out->bits = NULL; return -1; }
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

/* ------------------------------------------------------------ GIF */

typedef struct {
    const unsigned char *d;
    long len, pos;
    long block_left;     /* bytes left in the current data sub-block */
    unsigned long bits;
    int nbits;
} GifReader;

static int gif_byte(GifReader *r)
{
    if (r->block_left == 0) {
        if (r->pos >= r->len) return -1;
        r->block_left = r->d[r->pos++];
        if (r->block_left == 0) return -1;  /* the image's data ended */
    }
    if (r->pos >= r->len) return -1;
    r->block_left--;
    return r->d[r->pos++];
}

static int gif_code(GifReader *r, int size)
{
    int c;
    while (r->nbits < size) {
        int b = gif_byte(r);
        if (b < 0) return -1;
        r->bits |= (unsigned long)b << r->nbits;
        r->nbits += 8;
    }
    c = (int)(r->bits & ((1UL << size) - 1));
    r->bits >>= size;
    r->nbits -= size;
    return c;
}

static int le16(const unsigned char *p) { return p[0] | (p[1] << 8); }

/* Report progress by how much of the data has been read */
typedef struct { image_progress fn; void *ref; long len, next; } Progress;

static int report(Progress *pr, long pos)
{
    if (!pr->fn || pos < pr->next) return 0;
    pr->next = pos + pr->len / 25 + 1;
    return pr->fn(pr->ref, (int)(pos * 100 / pr->len));
}

static long skip_subblocks(const unsigned char *d, long len, long p)
{
    while (p < len && d[p]) p += d[p] + 1;
    return p + 1;
}

static int gif_decode(const unsigned char *d, long len, short w, short h, Bitmap1 *out, Progress *pr)
{
    unsigned char lum[256];
    const unsigned char *global = NULL;
    int global_n = 0, transparent = -1, k;
    long p = 13;
    short sw, sh;
    Gray g;
    int result = -1;

    sw = (short)le16(d + 6);
    sh = (short)le16(d + 8);
    if (d[10] & 0x80) {
        global_n = 2 << (d[10] & 7);
        global = d + 13;
        p += 3L * global_n;
    }
    if (gray_init(&g, w, h, sw, sh)) { gray_free(&g); return -1; }

    while (p < len) {
        if (d[p] == 0x21 && p + 1 < len) {
            if (d[p + 1] == 0xF9 && p + 6 < len && (d[p + 3] & 1)) transparent = d[p + 6];
            p = skip_subblocks(d, len, p + 2);
        } else if (d[p] == 0x2C && p + 10 < len) {
            /* the first image: LZW codes to palette indices to grays */
            short left = (short)le16(d + p + 1), top = (short)le16(d + p + 3);
            short fw = (short)le16(d + p + 5), fh = (short)le16(d + p + 7);
            int flags = d[p + 9], interlaced = flags & 0x40, pass = 0, min, clear, eoi, size, next, old = -1, first = 0;
            const unsigned char *pal = global;
            int pal_n = global_n;
            unsigned short *prefix = malloc(4096 * sizeof(unsigned short));
            unsigned char *suffix = malloc(4096), *stack = malloc(4097);
            long x = 0, y = 0;
            static const short start[4] = { 0, 4, 2, 1 }, step[4] = { 8, 8, 4, 2 };
            GifReader r;
            p += 10;
            if (flags & 0x80) { pal_n = 2 << (flags & 7); pal = d + p; p += 3L * pal_n; }
            for (k = 0; k < 256; k++) {
                if (pal && k < pal_n) lum[k] = (unsigned char)((pal[3 * k] * 77 + pal[3 * k + 1] * 151 + pal[3 * k + 2] * 28) >> 8);
                else lum[k] = 0;
            }
            if (transparent >= 0) lum[transparent] = 255;
            if (!prefix || !suffix || !stack || p >= len) goto gif_done;
            min = d[p++];
            if (min < 2 || min > 8) goto gif_done;
            clear = 1 << min; eoi = clear + 1;
            size = min + 1; next = eoi + 1;
            for (k = 0; k < clear; k++) { prefix[k] = 0; suffix[k] = (unsigned char)k; }
            memset(&r, 0, sizeof r);
            r.d = d; r.len = len; r.pos = p;
            for (;;) {
                int code = gif_code(&r, size), in, sp = 0;
                if (code < 0 || code == eoi) break;
                if (code == clear) { size = min + 1; next = eoi + 1; old = -1; continue; }
                if (old < 0) {
                    if (code >= clear) break;
                    stack[sp++] = (unsigned char)code;
                    first = code;
                    old = code;
                } else {
                    in = code;
                    if (code > next) break;
                    if (code == next) { stack[sp++] = (unsigned char)first; code = old; }
                    while (code >= clear && sp < 4096) { stack[sp++] = suffix[code]; code = prefix[code]; }
                    first = code;
                    stack[sp++] = (unsigned char)first;
                    if (next < 4096) {
                        prefix[next] = (unsigned short)old;
                        suffix[next] = (unsigned char)first;
                        next++;
                        if (next == (1 << size) && size < 12) size++;
                    }
                    old = in;
                }
                while (sp > 0) {
                    put(&g, left + x, top + y, lum[stack[--sp]]);
                    if (++x == fw) {
                        x = 0;
                        if (!interlaced) y++;
                        else {
                            y += step[pass];
                            while (y >= fh && pass < 3) y = start[++pass];
                        }
                    }
                }
                if (y >= fh) break;
                if (report(pr, r.pos)) goto gif_done;
            }
            result = dither(&g, out);
        gif_done:
            free(prefix); free(suffix); free(stack);
            break;
        } else {
            break;  /* the trailer, or something we don't know */
        }
    }
    gray_free(&g);
    return result;
}

/* ------------------------------------------------------------ JPEG */

typedef struct {
    const unsigned char *d;
    long len, pos;
    Gray *g;
    Progress *pr;
} JpegSource;

static size_t jpeg_in(JDEC *jd, uint8_t *buf, size_t n)
{
    JpegSource *s = jd->device;
    if ((long)n > s->len - s->pos) n = (size_t)(s->len - s->pos);
    if (buf) memcpy(buf, s->d + s->pos, n);
    s->pos += n;
    return n;
}

static int jpeg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    JpegSource *s = jd->device;
    const uint8_t *px = bitmap;
    long x, y;
    for (y = rect->top; y <= rect->bottom; y++)
        for (x = rect->left; x <= rect->right; x++) put(s->g, x, y, *px++);
    return !report(s->pr, s->pos);
}

static int jpeg_decode(const unsigned char *d, long len, short w, short h, Bitmap1 *out, Progress *pr)
{
    JDEC jd;
    JpegSource s;
    Gray g;
    void *pool = malloc(4096);
    int scale = 0, result = -1;
    memset(&g, 0, sizeof g);
    s.d = d; s.len = len; s.pos = 0; s.g = &g; s.pr = pr;
    if (!pool || jd_prepare(&jd, jpeg_in, pool, 4096, &s) != JDR_OK) goto done;
    /* let the decoder shrink by 2, 4 or 8 while the result is still at least the target */
    while (scale < 3 && (jd.width >> (scale + 1)) >= w && (jd.height >> (scale + 1)) >= h) scale++;
    if (gray_init(&g, w, h, (short)((jd.width + (1 << scale) - 1) >> scale), (short)((jd.height + (1 << scale) - 1) >> scale))) goto done;
    if (jd_decomp(&jd, jpeg_out, (uint8_t)scale) != JDR_OK) goto done;
    levels(&g);
    result = dither(&g, out);
done:
    gray_free(&g);
    free(pool);
    return result;
}

/* ------------------------------------------------------------ */

int image_dims(const unsigned char *d, long len, short *w, short *h)
{
    long p;
    if (len >= 10 && !memcmp(d, "GIF8", 4)) {
        *w = (short)le16(d + 6);
        *h = (short)le16(d + 8);
        return 0;
    }
    if (len < 4 || d[0] != 0xFF || d[1] != 0xD8) return -1;
    /* JPEG: the frame header (SOFn) has the size */
    for (p = 2; p + 9 < len; ) {
        int marker, seg;
        if (d[p] != 0xFF) return -1;
        marker = d[p + 1];
        if (marker == 0xFF) { p++; continue; }
        seg = (d[p + 2] << 8) | d[p + 3];
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            *h = (short)((d[p + 5] << 8) | d[p + 6]);
            *w = (short)((d[p + 7] << 8) | d[p + 8]);
            return 0;
        }
        p += 2 + seg;
    }
    return -1;
}

int image_decode(const unsigned char *d, long len, short w, short h, Bitmap1 *out,
                 image_progress progress, void *ref)
{
    Progress pr;
    pr.fn = progress; pr.ref = ref; pr.len = len; pr.next = 0;
    memset(out, 0, sizeof *out);
    if (len >= 13 && !memcmp(d, "GIF8", 4)) return gif_decode(d, len, w, h, out, &pr);
    if (len >= 4 && d[0] == 0xFF && d[1] == 0xD8) return jpeg_decode(d, len, w, h, out, &pr);
    return -1;
}

void image_free(Bitmap1 *b)
{
    free(b->bits);
    b->bits = NULL;
}
