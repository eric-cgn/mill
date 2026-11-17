/* PNG (Portable Network Graphics) Specification, version 1.0, RFC 2083 (the same for a
   decoder in W3C's version 1.2 and ISO/IEC 15948), with zlib's header (RFC 1950): every
   color type and bit depth, PLTE, tRNS and alpha, and Adam7 interlacing.
   Read as it arrives: chunks are taken apart a byte at a time, IDAT's zlib data goes to
   Mill's own inflate (the host's GzData) as it comes, and each row is unfiltered, made
   gray, and given to the sink as soon as it's whole. Only two rows are kept at the file's
   depth, besides inflate's 32K window.

   Gray: 8 bits of each sample (the high byte of 16), red, green, and blue weighted 77,
   151, and 28 out of 256 through tables, so no multiply per pixel. Alpha: a pixel at
   least half opaque is blended onto white; one less is where the page shows through (Mill
   draws a picture's pixels or the page, nothing between). gAMA, cHRM, sRGB, iCCP, and
   bKGD are skipped, like every other ancillary chunk: Mill dithers to black and white, so
   tones matter less than speed. CRCs are checked only if asked; zlib's Adler-32 never.

   Adam7 sends a picture in seven passes. Rows 1, 3, 5... come whole in pass 7, and the
   rest of rows 0, 2, 4... in pass 6, so only the pixels at even columns of even rows
   (passes 1 to 5) are kept until pass 6 fills the rows in: a quarter of the picture, a
   byte a pixel. Every pixel goes to the sink once, in whole rows. */
#include <string.h>

#include "images.h"

enum { SIG, HEAD, DATA, CRC };          /* where in the file */
enum { C_IHDR, C_PLTE, C_TRNS, C_IDAT, C_IEND, C_SKIP };    /* the chunk being read */
enum { Z_HEAD, Z_DATA, Z_END };         /* where in IDAT's zlib stream */
enum { M_TABLE, M_GRAY, M_GA, M_RGB, M_RGBA };      /* how a row becomes gray */

#define TYPE(a, b, c, d) (((unsigned long)(a) << 24) | ((unsigned long)(b) << 16) | ((unsigned)(c) << 8) | (d))

/* Adam7: each pass's first column and row, and the log2 of its steps */
static const unsigned char XS[7] = { 0, 4, 0, 2, 0, 1, 0 }, YS[7] = { 0, 0, 4, 0, 2, 0, 1 };
static const unsigned char XSH[7] = { 3, 3, 2, 2, 1, 1, 0 }, YSH[7] = { 3, 3, 3, 2, 2, 1, 1 };

typedef struct {
    ImgBase b;
    int state, what, chunks, err, done, started;
    unsigned char field[8];
    int got;                        /* bytes of field gathered */
    unsigned long len;              /* the chunk's data still to come */
    unsigned char buf[768];         /* IHDR, PLTE, or tRNS */
    int nbuf;
    unsigned long crc, *crctab;     /* crctab NULL: not checked */
    /* IHDR, PLTE, tRNS */
    long w, h;
    int depth, ctype, interlace, npal, trns, ntrns;
    unsigned char pal[768], trn[256];
    /* decoding */
    int mode, mask, bpp, bits;      /* bpp: filter bytes a pixel, at least 1; bits: a pixel's */
    void *gz;
    int zstate, zgot;
    unsigned char zhead[2];
    unsigned char *cur, *prev;      /* bpp zeros, then a row */
    long rb, rowpos;                /* the pass's row bytes; how much of this row is in (0: none, not even its filter) */
    int ft;
    int pass;                       /* 0 to 6 (0 only, not interlaced); 7 when done */
    long pw, ph, row;               /* the pass's size, and its rows done */
    unsigned char *gray, *op;       /* a row of the picture to give (op only with mask) */
    unsigned char *pg, *po;         /* a row of an interlaced pass */
    unsigned char *qg, *qo;         /* even rows' even columns, from passes 1 to 5 */
    long qw, qh;
    long even_done, odd_done;       /* interlaced rows given, in order */
    unsigned short *lum;            /* 768: red, green, and blue times 77, 151, and 28 */
    unsigned char tg[256], to[256]; /* M_TABLE: a sample's gray and opacity */
    unsigned short tv[3];           /* tRNS for gray and truecolor: its sample values */
} Png;

static void *mem_alloc(void *ref, long n) { Png *p = ref; return p->b.s.alloc(p->b.s.ref, n); }
static void mem_release(void *ref, void *q) { Png *p = ref; p->b.s.release(p->b.s.ref, q); }

static void *zalloc(Png *p, long n)
{
    void *q = p->b.s.alloc(p->b.s.ref, n);
    if (q) memset(q, 0, n);
    return q;
}
static void release(Png *p, void *q) { if (q) p->b.s.release(p->b.s.ref, q); }

void *png_begin(const ImgSink *s)
{
    Png *p = s->alloc(s->ref, sizeof(Png));
    if (!p) return NULL;
    memset(p, 0, sizeof *p);
    p->b.s = *s;
    if (s->flags & IMG_CHECK_CRC) {
        /* the specification's CRC-32 (its Annex D), a nibble at a time */
        unsigned long c;
        int n, k;
        if (!(p->crctab = s->alloc(s->ref, 16 * sizeof(unsigned long)))) { s->release(s->ref, p); return NULL; }
        for (n = 0; n < 16; n++) {
            for (c = n, k = 0; k < 4; k++) c = c & 1 ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
            p->crctab[n] = c;
        }
    }
    return p;
}

void png_end(void *d)
{
    Png *p = d;
    if (!p) return;
    if (p->gz) p->b.s.gz->end(p->gz);
    release(p, p->cur); release(p, p->prev);
    release(p, p->gray); release(p, p->op);
    release(p, p->pg); release(p, p->po);
    release(p, p->qg); release(p, p->qo);
    release(p, p->lum); release(p, p->crctab);
    p->b.s.release(p->b.s.ref, p);
}

static void crc_add(Png *p, const unsigned char *b, long n)
{
    unsigned long c = p->crc;
    const unsigned long *t = p->crctab;
    while (n-- > 0) {
        c ^= *b++;
        c = (c >> 4) ^ t[c & 15];
        c = (c >> 4) ^ t[c & 15];
    }
    p->crc = c;
}

static unsigned long be32(const unsigned char *q)
{
    return ((unsigned long)q[0] << 24) | ((unsigned long)q[1] << 16) | ((unsigned)q[2] << 8) | q[3];
}

/* ---- rows to grays ---- */

/* v at alpha a onto white: 255 - (255 - v) a / 255, rounded, in 16 bits */
#define OVER_WHITE(v, a) ((unsigned char)(255 - ((t = (unsigned short)((unsigned short)(255 - (v)) * (unsigned short)(a) + 128)), \
                                                  (unsigned short)(t + (t >> 8)) >> 8)))

/* A pixel at alpha a: its gray in *g, and in *o whether it covers the page */
#define ALPHA(v, a, g, o) do { \
        if ((a) == 255) { *(g) = (v); *(o) = 1; } \
        else if ((a) >= 128) { *(g) = OVER_WHITE(v, a); *(o) = 1; } \
        else { *(g) = 255; *(o) = 0; } } while (0)

/* n pixels of an unfiltered row at s to grays at g (and, with mask, opacity at o) */
static void convert(Png *p, const unsigned char *s, unsigned short n, unsigned char *g, unsigned char *o)
{
    const unsigned short *lr = p->lum, *lgr = p->lum + 256, *lb = p->lum + 512;
    unsigned short t;
    int k = p->depth >> 3;          /* bytes a sample, 8 and 16 bits */
    switch (p->mode) {
    case M_TABLE:
        if (p->depth == 8) {
            if (o) while (n--) { *g++ = p->tg[*s]; *o++ = p->to[*s++]; }
            else while (n--) *g++ = p->tg[*s++];
        } else {
            int d = p->depth, shift = 8 - d, m = (1 << d) - 1;
            unsigned char byte = *s++, v;
            while (n--) {
                if (shift < 0) { byte = *s++; shift = 8 - d; }
                v = (byte >> shift) & m;
                shift -= d;
                *g++ = p->tg[v];
                if (o) *o++ = p->to[v];
            }
        }
        break;
    case M_GRAY:    /* 16 bits (8 goes through the table) */
        if (o) {
            unsigned char hi = (unsigned char)(p->tv[0] >> 8), lo = (unsigned char)p->tv[0];
            while (n--) { *g++ = s[0]; *o++ = s[0] != hi || s[1] != lo; s += 2; }
        } else while (n--) { *g++ = *s; s += 2; }
        break;
    case M_GA:
        while (n--) { ALPHA(s[0], s[k], g, o); g++; o++; s += 2 * k; }
        break;
    case M_RGB:
        if (o && k == 1) {
            unsigned char r = (unsigned char)p->tv[0], gg = (unsigned char)p->tv[1], b = (unsigned char)p->tv[2];
            while (n--) {
                *g++ = (unsigned char)((unsigned short)(lr[s[0]] + lgr[s[1]] + lb[s[2]]) >> 8);
                *o++ = s[0] != r || s[1] != gg || s[2] != b;
                s += 3;
            }
        } else if (o) {
            while (n--) {
                *g++ = (unsigned char)((unsigned short)(lr[s[0]] + lgr[s[2]] + lb[s[4]]) >> 8);
                *o++ = ((unsigned short)s[0] << 8 | s[1]) != p->tv[0] || ((unsigned short)s[2] << 8 | s[3]) != p->tv[1] ||
                       ((unsigned short)s[4] << 8 | s[5]) != p->tv[2];
                s += 6;
            }
        } else {
            while (n--) { *g++ = (unsigned char)((unsigned short)(lr[s[0]] + lgr[s[k]] + lb[s[2 * k]]) >> 8); s += 3 * k; }
        }
        break;
    case M_RGBA:
        while (n--) {
            unsigned char v = (unsigned char)((unsigned short)(lr[s[0]] + lgr[s[k]] + lb[s[2 * k]]) >> 8);
            ALPHA(v, s[3 * k], g, o);
            g++; o++;
            s += 4 * k;
        }
        break;
    }
}

/* ---- interlaced pictures' even rows ---- */

/* Which pass (0 to 4) gave even column x of even row y */
static int pass_of(long x, long y)
{
    if (!(y & 7)) return !(x & 7) ? 0 : (x & 7) == 4 ? 1 : 3;
    if ((y & 7) == 4) return x & 3 ? 3 : 2;
    return 4;
}

/* Even row y from what's kept, into gray and op, its odd columns copied from the left;
   a pixel not in yet is taken from the passes that are, or else is white (and, with a
   mask, shows the page) */
static void even_row(Png *p, long y)
{
    /* each pass's grid, once it's all in: where a pixel takes its place from */
    static const unsigned char MX[5] = { 7, 3, 3, 1, 1 }, MY[5] = { 7, 7, 3, 3, 1 };
    long x, at, here = (y >> 1) * p->qw, from = 0, mx = 0;
    if (p->pass > 0 && p->pass < 5) {
        from = ((y & ~(long)MY[p->pass - 1]) >> 1) * p->qw;
        mx = ~(long)MX[p->pass - 1];
    }
    for (x = 0; x < p->w; x += 2) {
        int k = pass_of(x, y);
        if (k < p->pass || (k == p->pass && ((y - YS[k]) >> YSH[k]) < p->row)) at = here + (x >> 1);
        else if (p->pass > 0) at = from + ((x & mx) >> 1);
        else {
            p->gray[x] = 255;
            if (p->op) p->op[x] = 0;
            goto odd;
        }
        p->gray[x] = p->qg[at];
        if (p->op) p->op[x] = p->qo[at];
    odd:
        if (x + 1 < p->w) {
            p->gray[x + 1] = p->gray[x];
            if (p->op) p->op[x + 1] = p->op[x];
        }
    }
}

/* What's not given yet of an interlaced picture: from what's kept (see even_row), odd
   rows as the row above */
static void interlace_rest(Png *p)
{
    long y;
    for (y = 0; y < p->h; y++) {
        if (y & 1 ? y >> 1 < p->odd_done : y >> 1 < p->even_done) continue;
        even_row(p, y & ~1L);
        p->b.s.rect(p->b.s.ref, 0, (short)y, (short)p->w, 1, p->gray, p->op);
    }
    p->even_done = p->qh;
    p->odd_done = p->h >> 1;
}

/* ---- rows ---- */

/* From p->pass on, the first pass with pixels, and its first row: 0 if there are no
   more */
static int start_pass(Png *p)
{
    if (!p->interlace) {
        if (p->pass) return 0;
        p->pw = p->w;
        p->ph = p->h;
    } else {
        for (; p->pass < 7; p->pass++) {
            p->pw = p->w > XS[p->pass] ? (p->w - XS[p->pass] + (1 << XSH[p->pass]) - 1) >> XSH[p->pass] : 0;
            p->ph = p->h > YS[p->pass] ? (p->h - YS[p->pass] + (1 << YSH[p->pass]) - 1) >> YSH[p->pass] : 0;
            if (p->pw && p->ph) break;
        }
        if (p->pass == 7) return 0;
    }
    p->rb = (p->pw * p->bits + 7) >> 3;
    memset(p->prev, 0, p->bpp + p->rb);
    p->row = 0;
    p->rowpos = 0;
    return 1;
}

/* Undo the row's filter (the specification's section 9), against the row before */
static int unfilter(Png *p)
{
    unsigned char *c = p->cur + p->bpp, *u = p->prev + p->bpp;
    unsigned short n = (unsigned short)p->rb, bpp = (unsigned short)p->bpp;
    switch (p->ft) {
    case 0:
        break;
    case 1:     /* Sub */
        c += bpp; n -= bpp < n ? bpp : n;
        while (n--) { *c += c[-bpp]; c++; }
        break;
    case 2:     /* Up */
        while (n--) *c++ += *u++;
        break;
    case 3:     /* Average */
        while (n--) { *c += (unsigned char)(((unsigned short)c[-bpp] + *u++) >> 1); c++; }
        break;
    case 4:     /* Paeth */
        while (n--) {
            short a = c[-bpp], b = *u, cc = u[-bpp], pa = b - cc, pb = a - cc, pc = pa + pb;
            if (pa < 0) pa = -pa;
            if (pb < 0) pb = -pb;
            if (pc < 0) pc = -pc;
            *c++ += (unsigned char)(pa <= pb && pa <= pc ? a : pb <= pc ? b : cc);
            u++;
        }
        break;
    default:
        return -1;
    }
    return 0;
}

/* A row is in: give it, or keep it; 1 when the picture's done or broken */
static int row_in(Png *p)
{
    const unsigned char *s = p->cur + p->bpp;
    unsigned char *t;
    if (unfilter(p)) { p->err = IMG_BAD; return 1; }
    if (!p->interlace) {
        convert(p, s, (unsigned short)p->w, p->gray, p->op);
        p->b.s.rect(p->b.s.ref, 0, (short)p->row, (short)p->w, 1, p->gray, p->op);
    } else if (p->pass == 6) {
        convert(p, s, (unsigned short)p->w, p->gray, p->op);
        p->b.s.rect(p->b.s.ref, 0, (short)(p->row * 2 + 1), (short)p->w, 1, p->gray, p->op);
        p->odd_done = p->row + 1;
    } else if (p->pass == 5) {
        /* the odd columns of an even row: the rest is kept */
        long i, at = p->row * p->qw;
        convert(p, s, (unsigned short)p->pw, p->pg, p->po);
        for (i = 0; i < p->pw; i++) {
            p->gray[2 * i] = p->qg[at + i];
            p->gray[2 * i + 1] = p->pg[i];
            if (p->op) { p->op[2 * i] = p->qo[at + i]; p->op[2 * i + 1] = p->po[i]; }
        }
        if (p->w & 1) {
            p->gray[p->w - 1] = p->qg[at + i];
            if (p->op) p->op[p->w - 1] = p->qo[at + i];
        }
        p->b.s.rect(p->b.s.ref, 0, (short)(p->row * 2), (short)p->w, 1, p->gray, p->op);
        p->even_done = p->row + 1;
    } else {
        /* passes 1 to 5: kept, at half the columns and rows */
        long i, step = 1L << (XSH[p->pass] - 1);
        long at = ((YS[p->pass] + (p->row << YSH[p->pass])) >> 1) * p->qw + (XS[p->pass] >> 1);
        convert(p, s, (unsigned short)p->pw, p->pg, p->po);
        for (i = 0; i < p->pw; i++, at += step) {
            p->qg[at] = p->pg[i];
            if (p->qo) p->qo[at] = p->po[i];
        }
    }
    t = p->cur; p->cur = p->prev; p->prev = t;
    p->rowpos = 0;
    if (++p->row < p->ph) return 0;
    p->pass++;
    if (start_pass(p)) return 0;
    if (p->interlace) interlace_rest(p);    /* rows only passes 1 to 5 had (one column) */
    p->done = 1;
    return 1;
}

/* Inflate's output: the filter bytes and rows, run together */
static int inflated(void *ref, const unsigned char *b, long n)
{
    Png *p = ref;
    while (n > 0) {
        long take;
        if (p->done || p->err) return 1;
        if (!p->rowpos) { p->ft = *b++; n--; p->rowpos = 1; continue; }
        take = p->rb + 1 - p->rowpos;
        if (take > n) take = n;
        memcpy(p->cur + p->bpp + p->rowpos - 1, b, take);
        p->rowpos += take;
        b += take;
        n -= take;
        if (p->rowpos == p->rb + 1 && row_in(p)) return 1;
    }
    return 0;
}

/* ---- chunks ---- */

/* IHDR is in */
static int header(Png *p)
{
    const unsigned char *b = p->buf;
    int d;
    if (p->nbuf != 13) return IMG_BAD;
    p->w = (long)be32(b);
    p->h = (long)be32(b + 4);
    d = p->depth = b[8];
    p->ctype = b[9];
    p->interlace = b[12];
    if (p->w < 1 || p->h < 1 || b[10] || b[11] || p->interlace > 1) return IMG_BAD;
    switch (p->ctype) {
    case 0: if (d != 1 && d != 2 && d != 4 && d != 8 && d != 16) return IMG_BAD; break;
    case 3: if (d != 1 && d != 2 && d != 4 && d != 8) return IMG_BAD; break;
    case 2: case 4: case 6: if (d != 8 && d != 16) return IMG_BAD; break;
    default: return IMG_BAD;
    }
    if (p->w > 32767 || p->h > 32767) return IMG_TOO_LARGE;
    return IMG_MORE;
}

/* The first IDAT: everything is known, so make the tables and rows, then tell the sink
   the size. Memory is taken before the sink's, so what's left is its. */
static int start(Png *p)
{
    static const unsigned char CHANNELS[7] = { 1, 0, 3, 1, 2, 0, 4 };
    int i, r, n = 1 << (p->depth > 8 ? 8 : p->depth);
    long maxrb;
    GzMem mem;
    unsigned short t;
    if (p->ctype == 3 && !p->npal) return IMG_BAD;
    p->bits = p->depth * CHANNELS[p->ctype];
    p->bpp = p->bits < 8 ? 1 : p->bits >> 3;
    maxrb = (p->w * p->bits + 7) >> 3;
    if (maxrb > PNG_ROW_MAX) return IMG_TOO_LARGE;
    /* the tables, and whether any pixel can show the page */
    switch (p->ctype) {
    case 3:
        for (i = 0; i < 256; i++) {
            const unsigned char *c = p->pal + 3 * i;
            unsigned char v = i < p->npal ? (unsigned char)((c[0] * 77L + c[1] * 151L + c[2] * 28L) >> 8) : 0;
            unsigned char a = i < p->ntrns ? p->trn[i] : 255;
            ALPHA(v, a, &p->tg[i], &p->to[i]);
            if (a < 255) p->mask = 1;
        }
        p->mode = M_TABLE;
        break;
    case 0:
        p->mask = p->trns;
        if (p->depth <= 8) {
            for (i = 0; i < n; i++) {
                p->tg[i] = (unsigned char)(i * 255L / (n - 1));
                p->to[i] = !(p->trns && i == p->tv[0]);
            }
            p->mode = M_TABLE;
        } else p->mode = M_GRAY;
        break;
    case 2:
        p->mask = p->trns;
        p->mode = M_RGB;
        break;
    case 4:
        p->mask = 1;
        p->mode = M_GA;
        break;
    case 6:
        p->mask = 1;
        p->mode = M_RGBA;
        break;
    }
    if (p->ctype == 2 || p->ctype == 6) {
        if (!(p->lum = p->b.s.alloc(p->b.s.ref, 768 * sizeof(unsigned short)))) return IMG_NO_MEMORY;
        for (i = 0, t = 0; i < 256; i++, t++) {
            p->lum[i] = (unsigned short)(t * 77);
            p->lum[256 + i] = (unsigned short)(t * 151);
            p->lum[512 + i] = (unsigned short)(t * 28);
        }
    }
    if (p->interlace) {
        p->qw = (p->w + 1) >> 1;
        p->qh = (p->h + 1) >> 1;
        if (p->qw * p->qh * (1 + p->mask) > PNG_INTERLACE_MAX) return IMG_TOO_LARGE;
        if (!(p->qg = zalloc(p, p->qw * p->qh)) || !(p->pg = zalloc(p, p->qw))) return IMG_NO_MEMORY;
        if (p->mask && (!(p->qo = zalloc(p, p->qw * p->qh)) || !(p->po = zalloc(p, p->qw)))) return IMG_NO_MEMORY;
    }
    if (!(p->cur = zalloc(p, p->bpp + maxrb)) || !(p->prev = zalloc(p, p->bpp + maxrb)) ||
        !(p->gray = zalloc(p, p->w)) || (p->mask && !(p->op = zalloc(p, p->w)))) return IMG_NO_MEMORY;
    mem.ref = p;
    mem.alloc = mem_alloc;
    mem.release = mem_release;
    if (!p->b.s.gz || !(p->gz = p->b.s.gz->begin(p->b.s.gz->ref, &mem, 1, inflated, p))) return IMG_NO_MEMORY;
    start_pass(p);
    p->started = 1;
    if ((r = p->b.s.size(p->b.s.ref, (short)p->w, (short)p->h, p->ctype == 2 || p->ctype == 6)) != 0) return r;
    return IMG_MORE;
}

/* IDAT's bytes: zlib's header (RFC 1950), then deflate; what follows it is ignored */
static void idat(Png *p, const unsigned char *b, long n)
{
    while (n > 0 && !p->done && !p->err) {
        if (p->zstate == Z_HEAD) {
            p->zhead[p->zgot++] = *b++;
            n--;
            if (p->zgot < 2) continue;
            /* deflate, a window of 32K or less, no preset dictionary */
            if ((p->zhead[0] & 15) != 8 || p->zhead[0] >> 4 > 7 || (p->zhead[1] & 32) ||
                ((unsigned short)p->zhead[0] << 8 | p->zhead[1]) % 31) { p->err = IMG_BAD; return; }
            p->zstate = Z_DATA;
        } else if (p->zstate == Z_DATA) {
            int r = p->b.s.gz->data(p->gz, b, n);
            if (r == -2 || p->done || p->err) return;
            if (r < 0) { p->err = IMG_BAD; return; }
            if (r == 1) p->zstate = Z_END;
            return;
        } else return;
    }
}

/* A chunk's data is all in */
static int chunk_end(Png *p)
{
    int i;
    switch (p->what) {
    case C_IHDR:
        return header(p);
    case C_PLTE:
        if (p->nbuf % 3) return IMG_BAD;
        memcpy(p->pal, p->buf, p->nbuf);
        p->npal = p->nbuf / 3;
        break;
    case C_TRNS:
        if (p->ctype == 3) {
            p->ntrns = p->nbuf > 256 ? 256 : p->nbuf;
            memcpy(p->trn, p->buf, p->ntrns);
        } else if (p->ctype == 0 || p->ctype == 2) {
            int k = p->ctype ? 3 : 1;
            if (p->nbuf < 2 * k) break;
            for (i = 0; i < k; i++) p->tv[i] = (unsigned short)(p->buf[2 * i] << 8 | p->buf[2 * i + 1]);
            p->trns = 1;
        }
        break;
    }
    return IMG_MORE;
}

/* Gather bytes into field until it has want; whether it has */
static int gather(Png *p, const unsigned char **b, long *n, int want)
{
    while (p->got < want && *n > 0) { p->field[p->got++] = *(*b)++; (*n)--; }
    return p->got == want;
}

/* The picture's end: what's missing of an interlaced one filled in */
static int finish(Png *p)
{
    if (!p->started) return IMG_BAD;
    if (p->interlace && !p->done) interlace_rest(p);
    p->done = 1;
    return IMG_DONE;
}

int png_data(void *d, const unsigned char *b, long n)
{
    Png *p = d;
    int r;
    if (p->err) return p->err;
    if (p->done) return IMG_DONE;
    if (!n) return finish(p);
    while (n > 0) {
        switch (p->state) {
        case SIG:
            if (!gather(p, &b, &n, 8)) return IMG_MORE;
            if (memcmp(p->field, "\211PNG\r\n\032\n", 8)) return p->err = IMG_BAD;
            p->got = 0;
            p->state = HEAD;
            break;
        case HEAD: {
            unsigned long type;
            if (!gather(p, &b, &n, 8)) return IMG_MORE;
            p->got = 0;
            p->len = be32(p->field);
            type = be32(p->field + 4);
            if (p->len > 0x7FFFFFFFUL) return p->err = IMG_BAD;
            p->what = type == TYPE('I', 'H', 'D', 'R') ? C_IHDR : type == TYPE('P', 'L', 'T', 'E') ? C_PLTE :
                      type == TYPE('t', 'R', 'N', 'S') ? C_TRNS : type == TYPE('I', 'D', 'A', 'T') ? C_IDAT :
                      type == TYPE('I', 'E', 'N', 'D') ? C_IEND : C_SKIP;
            /* IHDR first and only once; an unknown critical chunk (an upper case first
               letter) can't be skipped; PLTE and tRNS count only before IDAT */
            if ((p->what == C_IHDR) != (p->chunks++ == 0)) return p->err = IMG_BAD;
            if (p->what == C_SKIP && !(p->field[4] & 32)) return p->err = IMG_BAD;
            if ((p->what == C_PLTE || p->what == C_TRNS) && p->started) p->what = C_SKIP;
            if (p->what <= C_TRNS && p->len > sizeof p->buf) return p->err = IMG_BAD;
            if (p->crctab) { p->crc = 0xFFFFFFFFUL; crc_add(p, p->field + 4, 4); }
            p->nbuf = 0;
            if (p->what == C_IDAT && !p->started && (r = start(p)) != IMG_MORE) return p->err = r;
            p->state = DATA;
            break;
        }
        case DATA: {
            long take = (long)p->len < n ? (long)p->len : n;
            if (p->crctab) crc_add(p, b, take);
            if (p->what == C_IDAT) idat(p, b, take);
            else if (p->what != C_SKIP) {
                memcpy(p->buf + p->nbuf, b, take);
                p->nbuf += (int)take;
            }
            if (p->err) return p->err;
            if (p->done) return IMG_DONE;
            b += take;
            n -= take;
            p->len -= take;
            if (!p->len) {
                if ((r = chunk_end(p)) != IMG_MORE) return p->err = r;
                p->state = CRC;
            }
            break;
        }
        case CRC:
            if (!gather(p, &b, &n, 4)) return IMG_MORE;
            p->got = 0;
            if (p->crctab && be32(p->field) != (p->crc ^ 0xFFFFFFFFUL)) return p->err = IMG_BAD;
            if (p->what == C_IEND) return finish(p);
            p->state = HEAD;
            break;
        }
    }
    return IMG_MORE;
}
