/* QuickDraw pictures, PICT version 1 and 2 (Inside Macintosh vol. I, "QuickDraw: Pictures";
   vol. V, "Color QuickDraw: Picture Format"; Inside Macintosh: Imaging With QuickDraw,
   1994, Appendix A, "Picture Opcodes"), with or without a file's 512-byte header.

   Version 1 is the original QuickDraw's, so the System draws it (draw_pict) on any Mac:
   it's kept as it comes, and its opcodes are checked as they arrive (lengths, PackBits
   rows that fill exactly their row) before the System sees it. Version 2 needs Color
   QuickDraw, which a Plus hasn't got: its bitmaps and pixel maps (BitsRect, PackBitsRect,
   DirectBitsRect, and their Rgn forms) are read here, a row at a time as they arrive,
   and everything else (lines, shapes, text) is skipped. */
#include <string.h>

#include "images.h"

enum { FIELD = 2048 + 8, KEEP = 96L * 1024, LOOK = 512 + 16 };

/* What the gathered bytes are */
enum { S_HEAD, S_OP, S_SIZE, S_LEN2, S_LEN4, S_TEXT, S_COMMENT, S_HEADEROP, S_PAT, S_RB, S_PIXMAP, S_BITMAP,
       S_CTHEAD, S_CTAB, S_RECTS, S_MASK, S_COUNT, S_ROW, S_DONE };

typedef struct {
    ImgBase b;
    /* the start of the file, until where the picture starts is known */
    unsigned char look[LOOK];
    long at, base;              /* bytes in look; where the picture starts */
    int found, v2, sized;
    /* the walk: skip bytes, then gather want of them into dst, then st says what they are */
    long pos, skip, want, got, good;    /* good: where the last whole opcode ended */
    int st, op;
    unsigned char *dst;
    unsigned char field[FIELD];
    short frame[4];             /* top, left, bottom, right: the opcodes' coordinates */
    /* version 1: the picture, kept */
    unsigned char *keep;
    long nkeep, room;
    /* a bitmap or pixel map being read */
    int pixmap, direct, pattern, packed, wide, unit, packType, pixelSize, cmpCount, ctflags;
    long rb, ulen, row, nraw;
    short bounds[4], src[4], dst4[4];
    unsigned char lum[256];
    unsigned char *pk, *raw, *gray, *out;
    ImgPack k;
} Pict;

static long be16(const unsigned char *q) { return ((long)q[0] << 8) | q[1]; }
static short s16(const unsigned char *q) { return (short)be16(q); }

void *pict_begin(const ImgSink *s)
{
    Pict *p = img_new(s, sizeof(Pict));
    if (!p) return NULL;
    p->b.s = *s;
    return p;
}

static void raster_free(Pict *p)
{
    img_free(&p->b.s, p->pk);
    img_free(&p->b.s, p->raw);
    img_free(&p->b.s, p->gray);
    img_free(&p->b.s, p->out);
    p->pk = p->raw = p->gray = p->out = NULL;
}

void pict_end(void *d)
{
    Pict *p = d;
    raster_free(p);
    img_free(&p->b.s, p->keep);
    img_free(&p->b.s, p);
}

/* ---- the walk ---- */

static void gather(Pict *p, int st, long skip, long want)
{
    p->st = st;
    p->skip = skip;
    p->want = want;
    p->got = 0;
    p->dst = p->field;
}

/* After skip bytes of data, the next opcode: version 2's start on an even byte */
static void next_op(Pict *p, long skip)
{
    if (p->v2 && ((p->pos + skip) & 1)) skip++;
    gather(p, S_OP, skip, p->v2 ? 2 : 1);
}

/* Version 1's opcodes, by the opcode */
static int v1_op(int op)
{
    return op <= 0x11 || (op >= 0x20 && op <= 0x23) || (op >= 0x28 && op <= 0x2B) || (op >= 0x30 && op <= 0x91) ||
           op == 0x98 || op == 0x99 || op == 0xA0 || op == 0xA1 || op == 0xFF;
}

/* An opcode's data of fixed length (Appendix A, table A-2), or -1 if it's not fixed */
static long fixed_len(Pict *p, int op)
{
    static const unsigned char low[0x24] = {
        0, 0, 8, 2, 1, 2, 4, 4, 2, 8, 8, 4, 4, 2, 4, 4, 8, 0, 0, 0, 0, 2, 2, 0, 0, 0, 6, 6, 0, 6, 0, 6,
        8, 4, 6, 2 };
    if (op == 0x01 || op == 0x11 || (op >= 0x12 && op <= 0x14)) return -1;
    if (op < 0x24) return low[op];
    if (op >= 0x30 && op <= 0x5F) return op & 8 ? 0 : 8;       /* rects, rounded rects, ovals */
    if (op >= 0x60 && op <= 0x6F) return op & 8 ? 4 : 12;      /* arcs */
    if ((op >= 0x78 && op <= 0x7F) || (op >= 0x88 && op <= 0x8F)) return 0;
    if (op == 0xA0) return 2;
    if (op >= 0xB0 && op <= 0xCF) return 0;
    if (op >= 0x100 && op <= 0x7FFF) return op == 0x0C00 ? -1 : (long)(op >> 8) * 2;
    if (op >= 0x8000 && op <= 0x80FF) return 0;
    (void)p;
    return -1;
}

/* A row's grays, at the bounds' width, from raw */
static void row_gray(Pict *p)
{
    long w = p->bounds[3] - p->bounds[1], x;
    const unsigned char *q = p->raw;
    for (x = 0; x < w; x++) {
        unsigned v;
        if (!p->pixmap) { p->gray[x] = q[x >> 3] & (0x80 >> (x & 7)) ? 0 : 255; continue; }
        if (p->direct && p->pixelSize == 16) {
            v = (unsigned)be16(q + 2 * x);
            p->gray[x] = IMG_LUM(((v >> 10) & 31) * 255 / 31, ((v >> 5) & 31) * 255 / 31, (v & 31) * 255 / 31);
        } else if (p->direct) {
            if (p->packType == 4) { long a = (p->cmpCount - 3) * w; p->gray[x] = IMG_LUM(q[a + x], q[a + w + x], q[a + 2 * w + x]); }
            else if (p->packType == 2) p->gray[x] = IMG_LUM(q[3 * x], q[3 * x + 1], q[3 * x + 2]);
            else p->gray[x] = IMG_LUM(q[4 * x + 1], q[4 * x + 2], q[4 * x + 3]);
        } else {
            long bit = x * p->pixelSize;
            v = (q[bit >> 3] >> (8 - p->pixelSize - (bit & 7))) & ((1 << p->pixelSize) - 1);
            p->gray[x] = p->lum[v];
        }
    }
}

/* A row of the source, mapped from its srcRect to its dstRect in the picture's frame */
static void row_put(Pict *p)
{
    long by = p->bounds[0] + p->row, sh = p->src[2] - p->src[0], sw = p->src[3] - p->src[1];
    long dh = p->dst4[2] - p->dst4[0], dw = p->dst4[3] - p->dst4[1], dy, x0, x1, x;
    if (by < p->src[0] || by >= p->src[2] || sh < 1 || sw < 1 || dh < 1 || dw < 1) return;
    x0 = p->dst4[1] > p->frame[1] ? p->dst4[1] : p->frame[1];
    x1 = p->dst4[3] < p->frame[3] ? p->dst4[3] : p->frame[3];
    if (x1 <= x0) return;
    for (x = x0; x < x1; x++) {
        long sx = p->src[1] + (x - p->dst4[1]) * sw / dw - p->bounds[1];
        p->out[x - x0] = sx >= 0 && sx < p->bounds[3] - p->bounds[1] ? p->gray[sx] : 255;
    }
    for (dy = p->dst4[0] + (by - p->src[0]) * dh / sh; dy < p->dst4[0] + (by - p->src[0] + 1) * dh / sh; dy++)
        if (dy >= p->frame[0] && dy < p->frame[2])
            p->b.s.rect(p->b.s.ref, (short)(x0 - p->frame[1]), (short)(dy - p->frame[0]), (short)(x1 - x0), 1, p->out, NULL);
}

/* The next row's bytes, or the end of the bitmap */
static int row_next(Pict *p)
{
    if (p->row >= p->bounds[2] - p->bounds[0]) { raster_free(p); next_op(p, 0); return IMG_MORE; }
    if (p->packed) gather(p, S_COUNT, 0, p->wide ? 2 : 1);
    else { gather(p, S_ROW, 0, p->ulen); p->dst = p->raw; }
    return IMG_MORE;
}

/* The bitmap's or pixel map's header is read: check it, and get ready for its rows */
static int raster_start(Pict *p)
{
    long w = p->bounds[3] - p->bounds[1], h = p->bounds[2] - p->bounds[0], need, dw = p->dst4[3] - p->dst4[1];
    if (w < 1 || h < 1 || w > 32767 || h > 32767 || dw > 32767) return IMG_BAD;
    if (!p->pixmap) { need = (w + 7) / 8; p->ulen = p->rb; }
    else if (p->direct) {
        if (p->pixelSize != 16 && p->pixelSize != 32) return IMG_BAD;
        if (p->rb < 8) p->packType = 1;
        else if (p->packType == 0) p->packType = p->pixelSize == 16 ? 3 : 4;
        if (p->pixelSize == 16 ? p->packType != 1 && p->packType != 3 : p->packType < 1 || p->packType == 3 || p->packType > 4) return IMG_BAD;
        if (p->packType == 4 && (p->cmpCount < 3 || p->cmpCount > 4)) return IMG_BAD;
        need = p->pixelSize / 8 * w;
        if (p->packType == 2) need = 3 * w;
        p->ulen = p->packType == 2 ? 3 * w : p->packType == 4 ? p->cmpCount * w : p->rb;
    } else {
        if (p->pixelSize != 1 && p->pixelSize != 2 && p->pixelSize != 4 && p->pixelSize != 8) return IMG_BAD;
        need = (w * p->pixelSize + 7) / 8;
        p->ulen = p->rb;
    }
    if ((p->packType != 2 && p->rb < need) || p->ulen < 1) return IMG_BAD;
    p->unit = p->direct && p->packType == 3 ? 2 : 1;
    /* packed: version 2's PackBitsRect and the direct kinds 3 and 4, from 8 bytes a row up */
    p->packed = p->rb >= 8 && (p->direct ? p->packType > 2 : (p->op & 0xF8) == 0x98 || p->pattern);
    p->wide = p->rb > 250;
    p->row = 0;
    p->pk = p->b.s.alloc(p->b.s.ref, p->ulen * 2 + 16);
    p->raw = p->b.s.alloc(p->b.s.ref, p->ulen + 256);
    p->gray = p->b.s.alloc(p->b.s.ref, w);
    p->out = p->b.s.alloc(p->b.s.ref, dw > 0 ? dw : 1);
    if (!p->pk || !p->raw || !p->gray || !p->out) return IMG_NO_MEMORY;
    return row_next(p);
}

/* A row's bytes are in */
static int row_done(Pict *p)
{
    if (p->packed) {
        long i, n = 0;
        memset(&p->k, 0, sizeof p->k);
        p->k.unit = p->unit;
        for (i = 0; i < p->got; i++) {
            n += packbits(&p->k, p->pk[i], p->raw + n);
            if (n > p->ulen) return IMG_BAD;    /* more than a row */
        }
        if (n != p->ulen) return IMG_BAD;
    }
    if (!p->pattern && p->v2) { row_gray(p); row_put(p); }
    p->row++;
    return row_next(p);
}

/* The gathered field is in: what it is says what comes next */
static int step(Pict *p)
{
    const unsigned char *f = p->field;
    long n;
    int r;
    switch (p->st) {
    case S_HEAD:    /* picSize, picFrame */
        p->frame[0] = s16(f + 2); p->frame[1] = s16(f + 4); p->frame[2] = s16(f + 6); p->frame[3] = s16(f + 8);
        if (p->frame[2] <= p->frame[0] || p->frame[3] <= p->frame[1]) return IMG_BAD;
        if (p->v2) {
            if ((r = p->b.s.size(p->b.s.ref, (short)(p->frame[3] - p->frame[1]), (short)(p->frame[2] - p->frame[0]), 0)) != 0) return r;
            p->sized = 1;
        }
        next_op(p, 0);
        return IMG_MORE;
    case S_OP:
        p->op = p->v2 ? (int)be16(f) : f[0];
        p->good = p->pos - (p->v2 ? 2 : 1);
        if (p->op == 0xFF) { p->st = S_DONE; return IMG_DONE; }
        if (!p->v2 && !v1_op(p->op)) return IMG_BAD;
        if ((n = fixed_len(p, p->op)) >= 0) { next_op(p, n); return IMG_MORE; }
        switch (p->op) {
        case 0x01: gather(p, S_SIZE, 0, 2); return IMG_MORE;                  /* clip region */
        case 0x11: next_op(p, p->v2 ? 2 : 1); return IMG_MORE;               /* the version */
        case 0x12: case 0x13: case 0x14: gather(p, S_PAT, 0, 10); return IMG_MORE;
        case 0x28: gather(p, S_TEXT, 0, 5); return IMG_MORE;
        case 0x29: case 0x2A: gather(p, S_TEXT, 0, 2); return IMG_MORE;
        case 0x2B: gather(p, S_TEXT, 0, 3); return IMG_MORE;
        case 0xA1: gather(p, S_COMMENT, 0, 4); return IMG_MORE;
        case 0x0C00: gather(p, S_HEADEROP, 0, 24); return IMG_MORE;
        case 0x90: case 0x91: case 0x98: case 0x99:
            p->direct = p->pattern = 0;
            gather(p, S_RB, 0, 2);
            return IMG_MORE;
        case 0x9A: case 0x9B:
            if (!p->v2) return IMG_BAD;
            p->direct = 1; p->pattern = 0;
            gather(p, S_RB, 4, 2);      /* after its baseAddr */
            return IMG_MORE;
        }
        if ((p->op >= 0x70 && p->op <= 0x77) || (p->op >= 0x80 && p->op <= 0x87)) { gather(p, S_SIZE, 0, 2); return IMG_MORE; }
        if ((p->op >= 0x24 && p->op <= 0x2F) || (p->op >= 0x92 && p->op <= 0xAF)) { gather(p, S_LEN2, 0, 2); return IMG_MORE; }
        gather(p, S_LEN4, 0, 4);        /* 0xD0 to 0xFE, 0x8100 and up */
        return IMG_MORE;
    case S_SIZE:    /* a region's or polygon's size, its own two bytes included */
        if ((n = be16(f)) < 2) return IMG_BAD;
        next_op(p, n - 2);
        return IMG_MORE;
    case S_LEN2: next_op(p, be16(f)); return IMG_MORE;
    case S_LEN4:
        if ((n = (be16(f) << 16) | be16(f + 2)) < 0) return IMG_BAD;
        next_op(p, n);
        return IMG_MORE;
    case S_TEXT: next_op(p, f[p->want - 1]); return IMG_MORE;
    case S_COMMENT: next_op(p, be16(f + 2)); return IMG_MORE;
    case S_HEADEROP:
        /* extended version 2 (-2): the opcodes' coordinates are its srcRect's */
        if (s16(f) == -2) {
            short fr[4];
            int k;
            for (k = 0; k < 4; k++) fr[k] = s16(f + 12 + 2 * k);
            if (fr[2] > fr[0] && fr[3] > fr[1]) {
                memcpy(p->frame, fr, sizeof fr);
                if ((r = p->b.s.size(p->b.s.ref, (short)(fr[3] - fr[1]), (short)(fr[2] - fr[0]), 0)) != 0) return r;
            }
        }
        next_op(p, 0);
        return IMG_MORE;
    case S_PAT:     /* a pixel pattern: a dither color, or a pixel map of its own */
        if (be16(f) == 2) { next_op(p, 6); return IMG_MORE; }
        if (be16(f) != 1) { next_op(p, 0); return IMG_MORE; }
        p->pattern = 1; p->direct = 0;
        memset(p->dst4, 0, sizeof p->dst4);
        gather(p, S_RB, 0, 2);
        return IMG_MORE;
    case S_RB:
        p->rb = be16(f) & 0x3FFF;
        p->pixmap = (f[0] & 0x80) != 0;
        if (p->direct && !p->pixmap) return IMG_BAD;
        gather(p, p->pixmap ? S_PIXMAP : S_BITMAP, 0, p->pixmap ? 44 : 8);
        return IMG_MORE;
    case S_BITMAP:
    case S_PIXMAP:
        for (n = 0; n < 4; n++) p->bounds[n] = s16(f + 2 * n);
        p->pixelSize = 1;
        p->cmpCount = 1;
        p->packType = 0;
        if (p->pixmap) {
            p->packType = (int)be16(f + 10);
            p->pixelSize = (int)be16(f + 26);
            p->cmpCount = (int)be16(f + 28);
            if (!p->direct) { gather(p, S_CTHEAD, 0, 8); return IMG_MORE; }
        }
        if (p->pattern) return raster_start(p);
        gather(p, S_RECTS, 0, 18);
        return IMG_MORE;
    case S_CTHEAD:  /* a color table: seed, flags, and its size less one */
        p->ctflags = (int)be16(f + 4);
        if ((n = be16(f + 6)) > 255) return IMG_BAD;
        for (r = 0; r < 256; r++) p->lum[r] = (unsigned char)(255 - r * 255 / ((1 << p->pixelSize) - 1 > 0 ? (1 << p->pixelSize) - 1 : 1));
        gather(p, S_CTAB, 0, 8 * (n + 1));
        return IMG_MORE;
    case S_CTAB:
        for (n = 0; n < p->want / 8; n++) {
            const unsigned char *e = f + 8 * n;
            int i = p->ctflags & 0x8000 ? (int)n : e[1];
            p->lum[i] = IMG_LUM(e[2], e[4], e[6]);
        }
        if (p->pattern) return raster_start(p);
        gather(p, S_RECTS, 0, 18);
        return IMG_MORE;
    case S_RECTS:   /* srcRect, dstRect, mode */
        for (n = 0; n < 4; n++) { p->src[n] = s16(f + 2 * n); p->dst4[n] = s16(f + 8 + 2 * n); }
        if (p->op & 1) { gather(p, S_MASK, 0, 2); return IMG_MORE; }
        return raster_start(p);
    case S_MASK:    /* the Rgn kinds' mask region, skipped */
        if ((n = be16(f)) < 2) return IMG_BAD;
        r = raster_start(p);
        if (r == IMG_MORE) p->skip = n - 2;
        return r;
    case S_COUNT:
        /* a byte, or a word over 250 bytes a row; some writers give words from 201 up, which
           a first count of 0 shows (version 2 only: version 1 goes to QuickDraw as it is) */
        if (!p->wide && !p->row && !f[0] && p->rb > 200 && p->v2 && p->want == 1) { p->wide = 1; gather(p, S_COUNT, 0, 1); return IMG_MORE; }
        n = p->wide && p->want == 2 ? be16(f) : f[0];
        if (n > p->ulen * 2 + 16) return IMG_BAD;
        if (!n) return IMG_BAD;
        gather(p, S_ROW, 0, n);
        p->dst = p->pk;
        return IMG_MORE;
    case S_ROW:
        return row_done(p);
    }
    return IMG_BAD;
}

/* Picture bytes, from picSize on */
static int walk(Pict *p, const unsigned char *b, long n)
{
    int r;
    if (!p->v2) {
        /* version 1 is kept for the System to draw */
        if (p->nkeep + n + 1 > p->room) {     /* (and a byte for finish's end) */
            long room = p->room ? p->room : 4096;
            unsigned char *k;
            while (room < p->nkeep + n + 1) room *= 2;
            if (room > KEEP) { if (p->nkeep + n + 1 > KEEP) return IMG_NO_MEMORY; room = KEEP; }
            if (!(k = p->b.s.alloc(p->b.s.ref, room))) return IMG_NO_MEMORY;
            if (p->keep) memcpy(k, p->keep, p->nkeep);
            img_free(&p->b.s, p->keep);
            p->keep = k;
            p->room = room;
        }
        memcpy(p->keep + p->nkeep, b, n);
        p->nkeep += n;
    }
    while (n > 0 && p->st != S_DONE) {
        long take;
        if (p->skip) {
            take = p->skip < n ? p->skip : n;
            p->skip -= take; p->pos += take; b += take; n -= take;
            continue;
        }
        take = p->want - p->got < n ? p->want - p->got : n;
        memcpy(p->dst + p->got, b, take);
        p->got += take; p->pos += take; b += take; n -= take;
        if (p->got < p->want) return IMG_MORE;
        if ((r = step(p)) != IMG_MORE) return r;
    }
    return p->st == S_DONE ? IMG_DONE : IMG_MORE;
}

/* Where the picture starts: after a 512-byte header, or at once; v2 too. 0 if it's
   neither. */
static int picture_at(Pict *p, long at, long have)
{
    const unsigned char *q = p->look + at;
    if (at + 12 > have) return 0;
    if (q[10] == 0x11 && q[11] == 0x01) { p->v2 = 0; return 1; }
    if (at + 14 <= have && q[10] == 0x00 && q[11] == 0x11 && q[12] == 0x02 && q[13] == 0xFF) { p->v2 = 1; return 1; }
    return 0;
}

/* The start is in look (have bytes): find the picture, and walk what's there */
static int find(Pict *p, long have)
{
    if (picture_at(p, 512, have)) p->base = 512;
    else if (picture_at(p, 0, have)) p->base = 0;
    else return IMG_BAD;
    p->found = 1;
    gather(p, S_HEAD, 0, 10);
    return walk(p, p->look + p->base, have - p->base);
}

/* The end: version 1 to the System, up to its last whole opcode */
static int finish(Pict *p)
{
    short w, h;
    int r;
    if (p->v2) return p->sized ? IMG_DONE : IMG_BAD;
    if (!p->keep || p->good <= 10) return IMG_BAD;
    if (p->st != S_DONE) p->keep[p->good] = 0xFF;   /* cut short: an end where the last whole one ended */
    w = (short)(p->frame[3] - p->frame[1]);
    h = (short)(p->frame[2] - p->frame[0]);
    if ((r = p->b.s.size(p->b.s.ref, w, h, 0)) != 0) return r;
    p->b.s.drawn(p->b.s.ref, &w, &h);
    if ((r = p->b.s.size(p->b.s.ref, w, h, 0)) != 0) return r;      /* the System draws it at that size */
    img_free(&p->b.s, p->pk);   /* (nothing else is needed now) */
    p->pk = NULL;
    r = p->b.s.draw_pict(p->b.s.ref, p->keep, p->good + 1, w, h);
    return r ? r : IMG_DONE;
}

int pict_data(void *d, const unsigned char *b, long n)
{
    Pict *p = d;
    int r;
    if (!n) {
        if (!p->found && ((r = find(p, p->at)) < 0 || !p->found)) return IMG_BAD;
        return finish(p);
    }
    if (!p->found) {
        long take = LOOK - p->at < n ? LOOK - p->at : n;
        memcpy(p->look + p->at, b, take);
        p->at += take;
        b += take;
        n -= take;
        if (p->at < LOOK) return IMG_MORE;
        if ((r = find(p, LOOK)) != IMG_MORE) return r == IMG_DONE ? finish(p) : r;
    }
    r = walk(p, b, n);
    return r == IMG_DONE ? finish(p) : r;
}
