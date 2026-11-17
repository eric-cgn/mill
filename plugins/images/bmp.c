/* Windows and OS/2 bitmaps (Microsoft Windows 3.1 SDK, Programmer's Reference vol. 3:
   BITMAPFILEHEADER, BITMAPINFOHEADER, BITMAPCOREHEADER, and "Bitmap Compression"; IBM
   OS/2 2.0 Presentation Manager Programming Reference: BITMAPINFOHEADER2): 1, 4, 8, and
   24 bits a pixel, uncompressed, RLE8, or RLE4. Rows go bottom up unless the height is
   negative, each padded to 4 bytes. */
#include <string.h>

#include "images.h"

enum { FILEHEAD = 14, MAXHEAD = 124 };
enum { HEAD, INFO, PALETTE, SKIP, ROWS, RLE, DONE };

typedef struct {
    ImgBase b;
    int state;
    long at;                /* bytes read */
    long want, got;         /* a field being gathered */
    unsigned char field[MAXHEAD];
    long off, hsize;        /* where the pixels start; the info header's size */
    long w, h, rowbytes, x, y;
    int bits, comp, ncolors, entry, topdown;
    unsigned char lum[256];
    unsigned char *raw, *row;
    /* RLE: the escape or count, and its second byte; an absolute run's bytes left (and
       its padding) */
    int rle[2], nrle, abs_left, abs_pad, abs_hi;
} Bmp;

void *bmp_begin(const ImgSink *s)
{
    Bmp *p = img_new(s, sizeof(Bmp));
    if (!p) return NULL;
    p->b.s = *s;
    p->want = FILEHEAD;
    return p;
}

void bmp_end(void *d)
{
    Bmp *p = d;
    img_free(&p->b.s, p->raw);
    img_free(&p->b.s, p->row);
    img_free(&p->b.s, p);
}

static long le16(const unsigned char *q) { return q[0] | ((long)q[1] << 8); }
static long le32(const unsigned char *q) { return (long)(short)le16(q + 2) * 65536L + le16(q); }   /* signed */

/* The row at y (counted from the first row in the file) to the sink, then a white one */
static int put_row(Bmp *p)
{
    if (p->y < p->h) p->b.s.rect(p->b.s.ref, 0, (short)(p->topdown ? p->y : p->h - 1 - p->y), (short)p->w, 1, p->row, NULL);
    memset(p->row, 255, p->w);
    p->x = 0;
    return ++p->y >= p->h;
}

/* The header's fields are in: check them, and start */
static int start(Bmp *p)
{
    const unsigned char *f = p->field;
    int r;
    if (p->hsize == 12) {   /* OS/2 1.x */
        p->w = le16(f + 4);
        p->h = le16(f + 6);
        p->bits = (int)le16(f + 10);
    } else {
        p->w = le32(f + 4);
        p->h = le32(f + 8);
        p->bits = (int)le16(f + 14);
        p->comp = p->hsize >= 20 ? (int)le32(f + 16) : 0;
        p->ncolors = p->hsize >= 36 ? (int)le32(f + 32) : 0;
    }
    if (p->h < 0) { p->topdown = 1; p->h = -p->h; }
    if (p->w < 1 || p->h < 1 || p->w > 32767 || p->h > 32767) return IMG_BAD;
    if (p->bits != 1 && p->bits != 4 && p->bits != 8 && p->bits != 24) return IMG_BAD;
    if (p->comp > 2 || (p->comp == 1 && p->bits != 8) || (p->comp == 2 && p->bits != 4)) return IMG_BAD;
    if (p->bits <= 8 && (p->ncolors <= 0 || p->ncolors > (1 << p->bits))) p->ncolors = 1 << p->bits;
    if (p->bits > 8) p->ncolors = 0;
    p->rowbytes = (p->w * p->bits + 31) / 32 * 4;
    if (!(p->row = p->b.s.alloc(p->b.s.ref, p->w))) return IMG_NO_MEMORY;
    memset(p->row, 255, p->w);
    if (!p->comp && !(p->raw = p->b.s.alloc(p->b.s.ref, p->rowbytes))) return IMG_NO_MEMORY;
    if ((r = p->b.s.size(p->b.s.ref, (short)p->w, (short)p->h, p->bits == 24)) != 0) return r;
    return IMG_MORE;
}

/* An uncompressed row is in raw */
static int raw_row(Bmp *p)
{
    long x;
    const unsigned char *q = p->raw;
    for (x = 0; x < p->w; x++) {
        if (p->bits == 24) p->row[x] = IMG_LUM(q[3 * x + 2], q[3 * x + 1], q[3 * x]);
        else if (p->bits == 8) p->row[x] = p->lum[q[x]];
        else if (p->bits == 4) p->row[x] = p->lum[(q[x >> 1] >> (x & 1 ? 0 : 4)) & 15];
        else p->row[x] = p->lum[(q[x >> 3] >> (7 - (x & 7))) & 1];
    }
    return put_row(p);
}

static void rle_pixel(Bmp *p, int k)
{
    if (p->x < p->w) p->row[p->x] = p->lum[k];
    p->x++;
}

/* One byte of RLE8 or RLE4 data */
static int rle_byte(Bmp *p, unsigned char c)
{
    int k;
    if (p->abs_left) {
        /* an absolute run: pixels as they are */
        if (p->bits == 8) { rle_pixel(p, c); p->abs_left--; }
        else {
            rle_pixel(p, c >> 4);
            if (--p->abs_left) { rle_pixel(p, c & 15); p->abs_left--; }
        }
        return IMG_MORE;
    }
    if (p->abs_pad) { p->abs_pad = 0; return IMG_MORE; }
    p->rle[p->nrle++] = c;
    if (p->nrle < 2) return IMG_MORE;
    if (p->rle[0]) {
        /* a count, and the pixel (RLE4: two, alternating) */
        for (k = 0; k < p->rle[0]; k++) rle_pixel(p, p->bits == 8 ? p->rle[1] : (k & 1 ? p->rle[1] & 15 : p->rle[1] >> 4));
        p->nrle = 0;
        return IMG_MORE;
    }
    switch (p->rle[1]) {
    case 0: p->nrle = 0; return put_row(p) ? IMG_DONE : IMG_MORE;      /* end of a row */
    case 1: put_row(p); return IMG_DONE;                                /* the end */
    case 2: return IMG_MORE;    /* a delta: its two bytes are read in bmp_data */
    default: {
        /* an absolute run of n pixels, padded to 16 bits */
        int n = p->rle[1], bytes = p->bits == 8 ? n : (n + 1) / 2;
        p->abs_left = n;
        p->abs_pad = bytes & 1;
        p->nrle = 0;
        return IMG_MORE;
    }
    }
}

int bmp_data(void *d, const unsigned char *b, long n)
{
    Bmp *p = d;
    long i = 0;
    int r;
    if (!n) {
        if (p->state < ROWS) return IMG_BAD;
        if (p->state == RLE && p->x) put_row(p);
        return IMG_DONE;
    }
    while (i < n) {
        if (p->want) {
            long take = p->want - p->got < n - i ? p->want - p->got : n - i;
            if (p->state < SKIP) memcpy(p->field + p->got, b + i, take);
            else if (p->state == ROWS) memcpy(p->raw + p->got, b + i, take);
            p->got += take;
            p->at += take;
            i += take;
            if (p->got < p->want) return IMG_MORE;
            p->want = p->got = 0;
            switch (p->state) {
            case HEAD:
                if (p->field[0] != 'B' || p->field[1] != 'M') return IMG_BAD;
                p->off = le32(p->field + 10);
                p->state = INFO;
                p->want = 4;
                p->got = 0;
                break;
            case INFO:
                if (!p->hsize) {
                    p->hsize = le32(p->field);
                    if (p->hsize != 12 && (p->hsize < 16 || p->hsize > MAXHEAD)) return IMG_BAD;
                    p->want = p->hsize;
                    p->got = 4;
                    break;
                }
                if ((r = start(p)) != IMG_MORE) return r;
                p->state = PALETTE;
                p->want = p->hsize == 12 ? 3 : 4;
                if (!p->ncolors) p->want = 0;
                break;
            case PALETTE:
                p->lum[p->entry++] = IMG_LUM(p->field[2], p->field[1], p->field[0]);
                if (p->entry < p->ncolors) p->want = p->hsize == 12 ? 3 : 4;
                break;
            case SKIP:
                break;
            case ROWS:
                if (raw_row(p)) return IMG_DONE;
                p->want = p->rowbytes;
                break;
            }
            if (p->want) continue;
            if (p->state == PALETTE || p->state == INFO) {
                /* on to the pixels, wherever the header says they start */
                p->state = SKIP;
                if (p->off > p->at) { p->want = p->off - p->at; continue; }
            }
            if (p->state == SKIP) {
                p->state = p->comp ? RLE : ROWS;
                if (!p->comp) p->want = p->rowbytes;
            }
            continue;
        }
        if (p->state != RLE) return IMG_BAD;
        /* RLE: a delta's two bytes come after its escape */
        if (p->nrle >= 2 && p->rle[0] == 0 && p->rle[1] == 2) {
            p->field[p->nrle++ - 2] = b[i++];
            p->at++;
            if (p->nrle < 4) continue;
            p->nrle = 0;
            if (p->field[1]) {
                long x = p->x + p->field[0], dy = p->field[1];
                while (dy-- > 0) if (put_row(p)) return IMG_DONE;
                p->x = x;
            } else p->x += p->field[0];
            continue;
        }
        r = rle_byte(p, b[i++]);
        p->at++;
        if (r != IMG_MORE) return r;
    }
    return IMG_MORE;
}
