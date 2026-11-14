/* Netpbm pictures, read as they arrive: PBM (P1, P4), PGM (P2, P5), and PPM (P3, P6),
   plain (numbers as text) or raw, a row at a time to the sink. Up to 16 bits a sample. */
#include <stdlib.h>
#include <string.h>

#include "picture.h"

typedef struct {
    PicSink s;
    int kind;                   /* 1 to 6, from P1 to P6 */
    int header;                 /* header numbers read: width, height, maxval */
    long num[3];
    long cur;                   /* the number being read, or -1 between */
    int comment;
    short w, h, x, y;
    long maxval;
    int bytes, channel;         /* raw: bytes a sample (1 or 2); which sample of a pixel */
    long sample[3];
    int have;                   /* raw: bytes of the sample so far */
    unsigned char *row;
} Pnm;

void *pnm_begin(const PicSink *s)
{
    Pnm *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->s = *s;
    p->cur = -1;
    return p;
}

void pnm_end(void *d)
{
    Pnm *p = d;
    if (!p) return;
    free(p->row);
    free(p);
}

static int channels(const Pnm *p) { return p->kind == 3 || p->kind == 6 ? 3 : 1; }

/* A pixel's samples are in: its gray into the row, and a full row to the sink */
static int pixel(Pnm *p)
{
    long v;
    if (p->kind == 1 || p->kind == 4) v = p->sample[0] ? 0 : 255;  /* PBM: 1 is black */
    else if (channels(p) == 3) v = (p->sample[0] * 77 + p->sample[1] * 151 + p->sample[2] * 28) / p->maxval;
    else v = p->sample[0] * 255 / p->maxval;
    p->row[p->x] = (unsigned char)(v > 255 ? 255 : v);
    if (++p->x < p->w) return 0;
    p->s.rect(p->s.ref, 0, p->y, p->w, 1, p->row, NULL);
    p->x = 0;
    return ++p->y >= p->h;
}

/* The header's numbers are all read */
static int start(Pnm *p)
{
    int r;
    p->w = (short)p->num[0];
    p->h = (short)p->num[1];
    p->maxval = p->kind == 1 || p->kind == 4 ? 1 : p->num[2];
    if (p->num[0] < 1 || p->num[1] < 1 || p->num[0] > 32767 || p->num[1] > 32767 || p->maxval < 1 || p->maxval > 65535) return -1;
    p->bytes = p->maxval > 255 ? 2 : 1;
    if (!(p->row = malloc(p->w))) return -2;
    return (r = p->s.size(p->s.ref, p->w, p->h, p->kind != 1 && p->kind != 4)) != 0 ? r : 0;
}

int pnm_data(void *d, const unsigned char *b, long n)
{
    Pnm *p = d;
    long i;
    int r;
    for (i = 0; i < n; i++) {
        unsigned char c = b[i];
        if (!p->kind) {
            /* "P1" to "P6" */
            if (p->header == 0 && p->cur == -1) { if (c != 'P') return -1; p->cur = -2; continue; }
            if (c < '1' || c > '6') return -1;
            p->kind = c - '0';
            p->cur = -1;
            continue;
        }
        if (p->header < (p->kind == 1 || p->kind == 4 ? 2 : 3) || (p->kind <= 3 && p->y < p->h)) {
            /* numbers as text: the header's, and a plain file's samples */
            if (p->comment) { if (c == '\n' || c == '\r') p->comment = 0; continue; }
            if (c == '#') { p->comment = 1; continue; }
            if (c >= '0' && c <= '9' && !(p->kind == 1 && p->header >= 2)) {
                p->cur = (p->cur < 0 ? 0 : p->cur * 10) + (c - '0');
                if (p->cur > 1000000L) return -1;
                continue;
            }
            if (p->kind == 1 && p->header >= 2 && (c == '0' || c == '1')) p->cur = c - '0';  /* P1: digits needn't be apart */
            else if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return -1;
            if (p->cur < 0) continue;
            if (p->header < (p->kind == 1 || p->kind == 4 ? 2 : 3)) {
                p->num[p->header++] = p->cur;
                p->cur = -1;
                if (p->header == (p->kind == 1 || p->kind == 4 ? 2 : 3)) {
                    if ((r = start(p)) != 0) return r;
                    /* raw: the samples start after this one whitespace character */
                }
                continue;
            }
            p->sample[p->channel++] = p->cur;
            p->cur = -1;
            if (p->channel == channels(p)) { p->channel = 0; if (pixel(p)) return 1; }
            continue;
        }
        if (p->y >= p->h) return 1;
        if (p->kind == 4) {
            /* PBM raw: 8 pixels a byte, each row starting a new byte */
            int k;
            for (k = 0; k < 8 && p->x < p->w; k++) {
                short y = p->y;
                p->sample[0] = (c >> (7 - k)) & 1;
                if (pixel(p)) return 1;
                if (p->y != y) break;
            }
            continue;
        }
        /* PGM, PPM raw: a byte or two (high first) a sample */
        p->sample[p->channel] = p->have ? (p->sample[p->channel] << 8) | c : c;
        if (++p->have < p->bytes) continue;
        p->have = 0;
        if (++p->channel == channels(p)) { p->channel = 0; if (pixel(p)) return 1; }
    }
    return p->y >= p->h && p->h ? 1 : 0;
}
