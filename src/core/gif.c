/* GIF (87a and 89a), read as it arrives: the first image, its transparent color as the
   page showing through, interlaced or not. A row at a time goes to the sink. */
#include <stdlib.h>
#include <string.h>

#include "picture.h"

enum { HEADER, GLOBAL, BLOCK, EXT_LABEL, EXT_SIZE, EXT_DATA, IMAGE, LOCAL, LZW_MIN, SUB_SIZE, SUB_DATA, DONE };

typedef struct {
    PicSink s;
    int state, want, got;           /* bytes wanted for a fixed field, and had so far */
    unsigned char field[768];
    unsigned char lum[256];
    int global_n, local_n, transparent, ext_label, left_in_sub;
    short sw, sh, left, top, fw, fh, x, y, pass;
    int interlaced, sized;
    /* LZW */
    int min, clear, eoi, size, next, old, first;
    unsigned long bits;
    int nbits;
    unsigned short prefix[4096];
    unsigned char suffix[4096], stack[4097];
    unsigned char *row, *opaque;
} Gif;

static int le16(const unsigned char *p) { return p[0] | (p[1] << 8); }

void *gif_begin(const PicSink *s)
{
    Gif *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->s = *s;
    g->state = HEADER;
    g->want = 13;
    g->transparent = -1;
    return g;
}

void gif_end(void *d)
{
    Gif *g = d;
    if (!g) return;
    free(g->row);
    free(g->opaque);
    free(g);
}

static void palette(Gif *g, const unsigned char *pal, int n)
{
    int k;
    for (k = 0; k < 256; k++)
        g->lum[k] = k < n ? (unsigned char)((pal[3 * k] * 77 + pal[3 * k + 1] * 151 + pal[3 * k + 2] * 28) >> 8) : 0;
}

/* One palette index into the row; a finished row goes to the sink. 1 once the image is full. */
static int pixel(Gif *g, int k)
{
    static const short start[4] = { 0, 4, 2, 1 }, step[4] = { 8, 8, 4, 2 };
    if (g->y >= g->fh) return 1;
    /* the transparent color dithers as white, and is left out of the mask */
    g->row[g->x] = k == g->transparent ? 255 : g->lum[k];
    if (g->opaque) g->opaque[g->x] = k != g->transparent;
    if (++g->x < g->fw) return 0;
    g->s.rect(g->s.ref, g->left, (short)(g->top + g->y), g->fw, 1, g->row, g->opaque);
    g->x = 0;
    if (!g->interlaced) g->y++;
    else {
        g->y += step[g->pass];
        while (g->y >= g->fh && g->pass < 3) g->y = start[++g->pass];
    }
    return g->y >= g->fh;
}

/* One LZW code; 1 when the image is done, -1 for bad data */
static int code(Gif *g, int c)
{
    int in, sp = 0;
    if (c == g->eoi) return 1;
    if (c == g->clear) { g->size = g->min + 1; g->next = g->eoi + 1; g->old = -1; return 0; }
    if (g->old < 0) {
        if (c >= g->clear) return -1;
        g->stack[sp++] = (unsigned char)c;
        g->first = c;
        g->old = c;
    } else {
        in = c;
        if (c > g->next) return -1;
        if (c == g->next) { g->stack[sp++] = (unsigned char)g->first; c = g->old; }
        while (c >= g->clear && sp < 4096) { g->stack[sp++] = g->suffix[c]; c = g->prefix[c]; }
        g->first = c;
        g->stack[sp++] = (unsigned char)g->first;
        if (g->next < 4096) {
            g->prefix[g->next] = (unsigned short)g->old;
            g->suffix[g->next] = (unsigned char)g->first;
            g->next++;
            if (g->next == (1 << g->size) && g->size < 12) g->size++;
        }
        g->old = in;
    }
    while (sp > 0) if (pixel(g, g->stack[--sp])) return 1;
    return 0;
}

int gif_data(void *d, const unsigned char *p, long n)
{
    Gif *g = d;
    long i = 0;
    int r;
    while (i < n && g->state != DONE) {
        unsigned char b;
        /* fixed fields gather in field first */
        if (g->want) {
            long take = g->want - g->got < n - i ? g->want - g->got : n - i;
            memcpy(g->field + g->got, p + i, take);
            g->got += (int)take;
            i += take;
            if (g->got < g->want) return 0;
            g->want = g->got = 0;
            switch (g->state) {
            case HEADER:
                if (memcmp(g->field, "GIF8", 4)) return -1;
                g->sw = (short)le16(g->field + 6);
                g->sh = (short)le16(g->field + 8);
                if (g->sw < 1 || g->sh < 1) return -1;
                if ((r = g->s.size(g->s.ref, g->sw, g->sh, 0)) != 0) return r;
                g->sized = 1;
                if (g->field[10] & 0x80) {
                    g->global_n = 2 << (g->field[10] & 7);
                    g->state = GLOBAL;
                    g->want = 3 * g->global_n;
                } else g->state = BLOCK;
                break;
            case GLOBAL:
                palette(g, g->field, g->global_n);
                g->state = BLOCK;
                break;
            case IMAGE: {
                int flags = g->field[8];
                g->left = (short)le16(g->field);
                g->top = (short)le16(g->field + 2);
                g->fw = (short)le16(g->field + 4);
                g->fh = (short)le16(g->field + 6);
                g->interlaced = flags & 0x40;
                if (g->fw < 1 || g->fh < 1) return -1;
                if (!(g->row = malloc(g->fw))) return -2;
                if (g->transparent >= 0 && !(g->opaque = malloc(g->fw))) return -2;
                if (flags & 0x80) { g->local_n = 2 << (flags & 7); g->state = LOCAL; g->want = 3 * g->local_n; }
                else g->state = LZW_MIN;
                break;
            }
            case LOCAL:
                palette(g, g->field, g->local_n);
                g->state = LZW_MIN;
                break;
            }
            continue;
        }
        b = p[i++];
        switch (g->state) {
        case BLOCK:
            if (b == 0x21) g->state = EXT_LABEL;
            else if (b == 0x2C) { g->state = IMAGE; g->want = 9; }
            else g->state = DONE;       /* the trailer, or something unknown */
            break;
        case EXT_LABEL:
            g->ext_label = b;
            g->state = EXT_SIZE;
            break;
        case EXT_SIZE:
            if (!b) { g->state = BLOCK; break; }
            g->left_in_sub = b;
            g->got = 0;
            g->state = EXT_DATA;
            break;
        case EXT_DATA:
            /* a Graphic Control Extension's first sub-block: flags, delay, transparent color */
            if (g->ext_label == 0xF9 && g->got < 4) g->field[g->got] = b;
            g->got++;
            if (--g->left_in_sub == 0) {
                if (g->ext_label == 0xF9 && g->got >= 4 && (g->field[0] & 1)) g->transparent = g->field[3];
                g->got = 0;
                g->state = EXT_SIZE;
            }
            break;
        case LZW_MIN: {
            int k;
            if (b < 2 || b > 8) return -1;
            g->min = b;
            g->clear = 1 << b;
            g->eoi = g->clear + 1;
            g->size = b + 1;
            g->next = g->eoi + 1;
            g->old = -1;
            for (k = 0; k < g->clear; k++) { g->prefix[k] = 0; g->suffix[k] = (unsigned char)k; }
            g->state = SUB_SIZE;
            break;
        }
        case SUB_SIZE:
            if (!b) { g->state = DONE; break; }   /* the image's data ended */
            g->left_in_sub = b;
            g->state = SUB_DATA;
            break;
        case SUB_DATA:
            g->bits |= (unsigned long)b << g->nbits;
            g->nbits += 8;
            while (g->nbits >= g->size) {
                int c = (int)(g->bits & ((1UL << g->size) - 1));
                g->bits >>= g->size;
                g->nbits -= g->size;
                /* bad data ends it too: what came before it shows, as it always has */
                if ((r = code(g, c)) != 0) { g->state = DONE; return 1; }
            }
            if (--g->left_in_sub == 0) g->state = SUB_SIZE;
            break;
        }
    }
    return g->state == DONE ? 1 : 0;
}
