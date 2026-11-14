#include <string.h>

#include "macenc.h"

unsigned short crc_ccitt(unsigned short crc, const unsigned char *p, long n)
{
    while (n-- > 0) {
        int k;
        crc ^= (unsigned short)(*p++ << 8);
        for (k = 0; k < 8; k++) crc = (unsigned short)(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

static void be32(unsigned char *p, unsigned long v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }

void macbinary_make(unsigned char h[128], const char *name, const char type[4], const char creator[4],
                      unsigned short flags, long data_len, long rsrc_len, unsigned long created, unsigned long modified)
{
    size_t n = strlen(name);
    unsigned short crc;
    if (n > 63) n = 63;
    memset(h, 0, 128);
    h[1] = (unsigned char)n;
    memcpy(h + 2, name, n);
    memcpy(h + 65, type, 4);
    memcpy(h + 69, creator, 4);
    h[73] = (unsigned char)(flags >> 8);
    be32(h + 83, (unsigned long)data_len);
    be32(h + 87, (unsigned long)rsrc_len);
    be32(h + 91, created);
    be32(h + 95, modified);
    h[101] = (unsigned char)(flags & 0xFF);    /* MacBinary II: the low byte too */
    h[122] = 129;                               /* written as II, read by I and up */
    h[123] = 129;
    crc = crc_ccitt(0, h, 124);
    h[124] = (unsigned char)(crc >> 8);
    h[125] = (unsigned char)crc;
}

int binhex_header(unsigned char *h, const char *name, const char type[4], const char creator[4],
                  unsigned short flags, long data_len, long rsrc_len)
{
    size_t n = strlen(name);
    int k;
    unsigned short crc;
    if (n > 63) n = 63;
    h[0] = (unsigned char)n;
    memcpy(h + 1, name, n);
    k = 1 + (int)n;
    h[k++] = 0;
    memcpy(h + k, type, 4); k += 4;
    memcpy(h + k, creator, 4); k += 4;
    h[k++] = (unsigned char)(flags >> 8);
    h[k++] = (unsigned char)flags;
    be32(h + k, (unsigned long)data_len); k += 4;
    be32(h + k, (unsigned long)rsrc_len); k += 4;
    crc = crc_ccitt(0, h, k);
    h[k++] = (unsigned char)(crc >> 8);
    h[k++] = (unsigned char)crc;
    return k;
}

static const char HQX[] = "!\"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr";
static const char INTRO[] = "(This file must be converted with BinHex 4.0)\r\r:";

static int out_char(Hqx *h, int c)
{
    if (h->out) {
        if (h->len >= h->size) return -1;
        h->out[h->len++] = (unsigned char)c;
    }
    h->count++;
    return 0;
}

/* A byte after run-length coding: six bits at a time out */
static int six(Hqx *h, int b)
{
    h->bits = (h->bits << 8) | (unsigned)b;
    h->nbits += 8;
    while (h->nbits >= 6) {
        h->nbits -= 6;
        if (out_char(h, HQX[(h->bits >> h->nbits) & 63])) return -1;
        if (++h->col == 64) { if (out_char(h, '\r')) return -1; h->col = 0; }
    }
    return 0;
}

/* The run so far, coded: the byte (0x90 as 0x90 0x00), then 0x90 and the count for 3 or more */
static int flush_run(Hqx *h)
{
    int k;
    if (h->last < 0) return 0;
    if (h->run >= 3) {
        if (six(h, h->last) || (h->last == 0x90 && six(h, 0))) return -1;
        if (six(h, 0x90) || six(h, h->run)) return -1;
    } else {
        for (k = 0; k < h->run; k++) if (six(h, h->last) || (h->last == 0x90 && six(h, 0))) return -1;
    }
    h->last = -1;
    h->run = 0;
    return 0;
}

void hqx_begin(Hqx *h, unsigned char *out, long size)
{
    const char *p;
    memset(h, 0, sizeof *h);
    h->out = out;
    h->size = size;
    h->last = -1;
    h->col = 1;     /* the first line has the colon */
    for (p = INTRO; *p; p++) out_char(h, *p);
}

int hqx_put(Hqx *h, const unsigned char *p, long n)
{
    long i;
    for (i = 0; i < n; i++) {
        if (p[i] == h->last && h->run < 255) { h->run++; continue; }
        if (flush_run(h)) return -1;
        h->last = p[i];
        h->run = 1;
    }
    return 0;
}

void hqx_finish(Hqx *h)
{
    flush_run(h);
    if (h->nbits) {
        h->bits <<= 6 - h->nbits;
        out_char(h, HQX[h->bits & 63]);
        h->nbits = 0;
    }
    out_char(h, ':');
}
