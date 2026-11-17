/* X bitmaps (Xlib - C Language X Interface, X11R5, XReadBitmapFile; and X10's): C source,
     #define name_width 16
     #define name_height 7
     static char name_bits[] = { 0x00, 0x1f, ... };
   1 is black, the low bit leftmost, each row starting a new byte (X10: "short" and 16-bit
   numbers, each row a new number). */
#include <string.h>

#include "images.h"

typedef struct {
    ImgBase b;
    char tok[32];
    int len, comment, star, slash;  /* a token so far; in a comment, and its last character a
                                   '*'; the last character a '/' */
    int define, which;          /* the last token was "define"; and then a name ending
                                   _width (1) or _height (2) */
    int x10, data;
    long w, h;
    short x, y;
    unsigned char *row;
} Xbm;

void *xbm_begin(const ImgSink *s)
{
    Xbm *p = img_new(s, sizeof(Xbm));
    if (!p) return NULL;
    p->b.s = *s;
    return p;
}

void xbm_end(void *d)
{
    Xbm *p = d;
    img_free(&p->b.s, p->row);
    img_free(&p->b.s, p);
}

static int ends(const Xbm *p, const char *s)
{
    int n = strlen(s);
    return p->len >= n && !memcmp(p->tok + p->len - n, s, n);
}

/* A number, decimal or 0x hex; -1 if it isn't one */
static long number(const Xbm *p)
{
    long v = 0;
    int i = 0, base = 10;
    if (p->len > 2 && p->tok[0] == '0' && (p->tok[1] == 'x' || p->tok[1] == 'X')) { base = 16; i = 2; }
    if (i >= p->len) return -1;
    for (; i < p->len; i++) {
        int c = p->tok[i], k = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
        if (k >= base || v > 100000L) return -1;
        v = v * base + k;
    }
    return v;
}

/* A whole token */
static int token(Xbm *p)
{
    long v;
    int k;
    if (p->data) {
        /* a row's bits, low first; what's left of the last number in a row is padding */
        if ((v = number(p)) < 0) return IMG_BAD;
        for (k = 0; k < (p->x10 ? 16 : 8) && p->x < p->w; k++) p->row[p->x++] = (v >> k) & 1 ? 0 : 255;
        if (p->x < p->w) return IMG_MORE;
        p->b.s.rect(p->b.s.ref, 0, p->y, (short)p->w, 1, p->row, NULL);
        p->x = 0;
        return ++p->y >= p->h ? IMG_DONE : IMG_MORE;
    }
    if (p->which && (v = number(p)) >= 0) {
        if (p->which == 1) p->w = v; else p->h = v;
        p->which = 0;
        return IMG_MORE;
    }
    p->which = 0;
    if (p->define) p->which = ends(p, "_width") ? 1 : ends(p, "_height") ? 2 : 0;
    p->define = p->len == 6 && !memcmp(p->tok, "define", 6);
    if (p->len == 5 && !memcmp(p->tok, "short", 5)) p->x10 = 1;
    return IMG_MORE;
}

int xbm_data(void *d, const unsigned char *b, long n)
{
    Xbm *p = d;
    long i;
    int r;
    if (!n) return p->y > 0 ? IMG_DONE : IMG_BAD;
    for (i = 0; i < n; i++) {
        int c = b[i];
        if (p->comment) {
            if (p->star && c == '/') p->comment = 0;
            p->star = c == '*';
            continue;
        }
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            if (p->len == sizeof p->tok) { memmove(p->tok, p->tok + 1, p->len - 1); p->len--; }   /* keep the end */
            p->tok[p->len++] = (char)c;
            p->slash = 0;
            continue;
        }
        if (c >= 128 || (c < ' ' && c != '\n' && c != '\r' && c != '\t')) return IMG_BAD;   /* not text */
        if (p->len && (r = token(p)) != IMG_MORE) return r;
        p->len = 0;
        if (c == '*' && p->slash) { p->comment = 1; p->star = 0; }
        p->slash = c == '/';
        if (c == '{' && !p->data) {
            if (p->w < 1 || p->h < 1 || p->w > 32767 || p->h > 32767) return IMG_BAD;
            if (!(p->row = p->b.s.alloc(p->b.s.ref, p->w))) return IMG_NO_MEMORY;
            if ((r = p->b.s.size(p->b.s.ref, (short)p->w, (short)p->h, 0)) != 0) return r;
            p->data = 1;
        }
        if (c == '}') return p->data ? IMG_DONE : IMG_BAD;
    }
    return IMG_MORE;
}
