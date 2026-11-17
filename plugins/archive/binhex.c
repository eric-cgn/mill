/* BinHex 4.0 (Yves Lempereur, 1985): one Mac file as text. After the line "(This file must
   be converted with BinHex 4.0)" and a colon, six bits a character from a 64-character set,
   line breaks ignored, up to a closing colon. The bytes are run-length coded (0x90 and a
   count repeats the byte before it; 0x90 0 is 0x90 itself), and are: the header (name
   length, name, version 0, type, creator, Finder flags, the two forks' lengths) and its
   CRC, the data fork and its CRC, the resource fork and its CRC. The CRCs are CRC-16 CCITT
   (src/core/macenc.c's crc_ccitt). Text before the line is skipped (mail headers, notes). */
#include <string.h>

#include "archive.h"
#include "../../src/core/macenc.h"

static const char MARK[] = "(This file must be converted with BinHex";
static const char HQX[] = "!\"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr";
#define FIRST '!'
#define SPAN ('r' - '!' + 1)

enum { PRELUDE, COLON, TEXT, FINISHED };                    /* reading the text */
enum { H_LEN, H_REST, H_CRC, D_FORK, D_CRC, R_FORK, R_CRC, H_DONE };  /* what the bytes are */

typedef struct {
    ArcBase b;
    int state, mark;            /* mark: how much of MARK has been seen */
    unsigned char rev[SPAN];    /* a character's six bits, plus one (0: not in the set) */
    unsigned long bits;
    int nbits;
    int rle, prev;              /* after 0x90; the byte before (-1: none yet) */
    int part;
    long got, need;
    unsigned short crc;
    unsigned char h[22 + 63];   /* the header, then a CRC's two bytes */
    long dlen, rlen;
    unsigned char obuf[256];
    int olen;
} HqxIn;

void *binhex_begin(const ArcHost *h, const ArcRequest *r)
{
    HqxIn *x = arc_new(h, sizeof *x);
    int i;
    if (!x) return NULL;
    x->b.h = h;
    x->b.r = *r;
    for (i = 0; i < 64; i++) x->rev[HQX[i] - FIRST] = (unsigned char)(i + 1);
    x->prev = -1;
    return x;
}

static int flush(HqxIn *x)
{
    int stop = x->olen && x->b.h->out(x->b.h->ref, x->obuf, x->olen);
    x->olen = 0;
    return stop ? ARC_STOPPED : ARC_MORE;
}

/* Its one member: listed, or (opening) told to the host before its bytes */
static int entry(HqxIn *x)
{
    ArcEntry e;
    char name[64];
    int n = x->h[0];
    const unsigned char *f = x->h + 2 + n;
    arc_entry_init(&e);
    arc_mac_name(x->h + 1, n, name);
    e.name = name;
    e.charset = ARC_MACROMAN;
    e.kind = ARC_FILE;
    e.size = e.data_len = x->dlen;
    e.rsrc_len = x->rlen;
    memcpy(e.type, f, 4);
    memcpy(e.creator, f + 4, 4);
    /* as MacBinary's: not on the desktop, not yet seen by the Finder, not changed or busy */
    e.finder_flags = (unsigned short)(((f[8] << 8) | f[9]) & ~0x0701);
    e.flags = ARC_FINDER;
    if (x->b.r.open) return arc_member(x->b.h, &e, x->b.rest, &x->b.r.fork);
    return x->b.h->entry(x->b.h->ref, &e) ? ARC_STOPPED : ARC_DONE;
}

/* The next part after one ends; ARC_DONE once the fork wanted is out */
static int next(HqxIn *x, int part)
{
    int want = x->b.r.fork == ARC_DATA_FORK ? D_FORK : R_FORK;   /* the last fork it gives */
    x->got = 0;
    x->crc = 0;
    x->part = part;
    if (part == D_FORK) x->need = x->dlen;
    else if (part == R_FORK) x->need = x->rlen;
    else if (part == D_CRC || part == R_CRC) x->need = 2;
    if (part == want + 2 || part == H_DONE) return ARC_DONE;
    if ((part == D_FORK || part == R_FORK) && !x->need) return next(x, part + 1);
    return ARC_MORE;
}

/* One byte of the file */
static int byte(HqxIn *x, unsigned char c)
{
    int r;
    switch (x->part) {
    case H_LEN:
        if (c < 1 || c > 63) return ARC_BAD;
        x->h[0] = c;
        x->crc = crc_ccitt(0, &c, 1);
        x->got = 1;
        x->need = 1 + c + 1 + 4 + 4 + 2 + 4 + 4;
        x->part = H_REST;
        return ARC_MORE;
    case H_REST:
        x->h[x->got++] = c;
        x->crc = crc_ccitt(x->crc, &c, 1);
        if (x->got < x->need) return ARC_MORE;
        x->part = H_CRC;
        x->got = 0;
        return ARC_MORE;
    case H_CRC: case D_CRC: case R_CRC: {
        unsigned char *k = x->h + 22 + 63 - 2;  /* the header's room is free past its end */
        if (x->part == H_CRC) k = x->h + x->need;
        k[x->got++] = c;
        if (x->got < 2) return ARC_MORE;
        if (((k[0] << 8) | k[1]) != x->crc) return ARC_BAD;
        if (x->part == H_CRC) {
            const unsigned char *f = x->h + 1 + x->h[0] + 1 + 10;
            if (arc_be32(f) > 0x7FFFFFFFUL || arc_be32(f + 4) > 0x7FFFFFFFUL) return ARC_BAD;
            x->dlen = (long)arc_be32(f);
            x->rlen = (long)arc_be32(f + 4);
            if (!x->b.r.open) { x->state = FINISHED; return entry(x); }
            x->b.rest = arc_first(x->b.r.path);
            if (entry(x) != ARC_MORE) return ARC_STOPPED;
            return next(x, D_FORK);
        }
        return next(x, x->part + 1);
    }
    case D_FORK: case R_FORK:
        x->crc = crc_ccitt(x->crc, &c, 1);
        if (x->b.r.fork == ARC_BOTH_FORKS || x->part == (x->b.r.fork == ARC_RSRC_FORK ? R_FORK : D_FORK)) {
            x->obuf[x->olen++] = c;
            if (x->olen == (int)sizeof x->obuf && (r = flush(x)) != ARC_MORE) return r;
        }
        if (++x->got < x->need) return ARC_MORE;
        if ((r = flush(x)) != ARC_MORE) return r;
        x->got = 0;
        x->part++;
        return ARC_MORE;
    }
    return ARC_BAD;
}

/* One byte before the run-length coding is undone */
static int unrle(HqxIn *x, unsigned char c)
{
    int r = ARC_MORE;
    if (x->rle) {
        x->rle = 0;
        if (c == 0) { x->prev = 0x90; return byte(x, 0x90); }
        if (x->prev < 0) return ARC_BAD;
        /* the byte before, c times in all: once already */
        while (--c > 0 && r == ARC_MORE) r = byte(x, (unsigned char)x->prev);
        return r;
    }
    if (c == 0x90) { x->rle = 1; return ARC_MORE; }
    x->prev = c;
    return byte(x, c);
}

int binhex_data(void *d, const unsigned char *p, long n)
{
    HqxIn *x = d;
    int r;
    if (x->state == FINISHED) return ARC_DONE;
    if (n == 0) return ARC_BAD;     /* the end, before the file's */
    while (n-- > 0) {
        unsigned char c = *p++;
        switch (x->state) {
        case PRELUDE:
            if (c == (unsigned char)MARK[x->mark]) x->mark++;
            else x->mark = c == '(' ? 1 : 0;
            if (!MARK[x->mark]) x->state = COLON;
            break;
        case COLON:
            if (c == ':') x->state = TEXT;
            break;
        case TEXT:
            if (c == '\r' || c == '\n' || c == ' ' || c == '\t') break;
            if (c == ':') return ARC_BAD;   /* the end of the text, before the end of the file */
            if (c < FIRST || c >= FIRST + SPAN || !x->rev[c - FIRST]) return ARC_BAD;
            x->bits = (x->bits << 6) | (unsigned long)(x->rev[c - FIRST] - 1);
            x->nbits += 6;
            if (x->nbits >= 8) {
                x->nbits -= 8;
                r = unrle(x, (unsigned char)(x->bits >> x->nbits));
                x->bits &= (1UL << x->nbits) - 1;
                if (r != ARC_MORE) { x->state = FINISHED; return r; }
            }
            break;
        }
    }
    return ARC_MORE;
}
