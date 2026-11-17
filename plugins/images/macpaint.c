/* MacPaint documents (Apple Technical Note 86, "MacPaint Document Format"): a 512-byte
   header (a version, 38 patterns, padding), then 720 rows of 576 pixels, 1 black, each
   packed with PackBits. */
#include <string.h>

#include "images.h"

enum { W = 576, H = 720, ROW = W / 8, HEAD = 512 };

typedef struct {
    ImgBase b;
    long at;                /* bytes read */
    unsigned char head[4];  /* the version */
    ImgPack k;
    unsigned char bits[ROW + 128];
    int nb;
    short y;
    unsigned char row[W];
} Paint;

void *macpaint_begin(const ImgSink *s)
{
    Paint *p = img_new(s, sizeof(Paint));
    if (!p) return NULL;
    p->b.s = *s;
    p->k.unit = 1;
    return p;
}

void macpaint_end(void *d) { Paint *p = d; img_free(&p->b.s, p); }

/* Rows unpacked so far: whole ones to the sink */
static int rows(Paint *p)
{
    int x;
    while (p->nb >= ROW && p->y < H) {
        for (x = 0; x < W; x++) p->row[x] = p->bits[x >> 3] & (0x80 >> (x & 7)) ? 0 : 255;
        p->b.s.rect(p->b.s.ref, 0, p->y++, W, 1, p->row, NULL);
        p->nb -= ROW;
        memmove(p->bits, p->bits + ROW, p->nb);
    }
    return p->y >= H;
}

/* The version: 0 to 3 */
static int bad_version(const unsigned char *v) { return v[0] || v[1] || v[2] || v[3] > 3; }

int macpaint_data(void *d, const unsigned char *b, long n)
{
    Paint *p = d;
    long i;
    int r;
    if (!n) return p->at > HEAD ? IMG_DONE : IMG_BAD;   /* the end: what's there */
    for (i = 0; i < n; i++, p->at++) {
        unsigned char c = b[i];
        if (p->at < HEAD) {
            if (p->at < 4) {
                p->head[p->at] = c;
                if (p->at == 3 && bad_version(p->head)) return IMG_BAD;
            }
            if (p->at == HEAD - 1 && (r = p->b.s.size(p->b.s.ref, W, H, 0)) != 0) return r;
            continue;
        }
        p->nb += packbits(&p->k, c, p->bits + p->nb);
        if (rows(p)) return IMG_DONE;
    }
    return IMG_MORE;
}
