/* images.h: the Image Formats Plug-In's decoders, plain C, so tests/test_images.c runs them
   on a host (and tests/test_png.c, PNG). Each reads a file a piece at a time, as it arrives,
   and gives rows of grays to an ImgSink (Mill's PictureSize and PictureRect, or a test's). */
#ifndef IMAGES_H
#define IMAGES_H

#include "../../src/core/inflate.h"

/* The Plug-In's name and title, in one place: plugin://image-formats/ is the file's name in
   lower case with hyphens */
#define IMG_NAME "image-formats"
#define IMG_TITLE "Image Formats"

typedef struct {
    void *ref;
    /* The size pixels will come at, and whether it's a photo; again if that changes (a
       PICT's own resolution). 0 to go on, else stop with that. */
    int (*size)(void *ref, short w, short h, int photo);
    /* The size it'll be drawn at, once size has been called */
    void (*drawn)(void *ref, short *w, short *h);
    /* Rows of w grays (0 black, 255 white) at x, y; opaque, if not NULL, is 0 where the
       page shows through, given from the first call on, or never */
    void (*rect)(void *ref, short x, short y, short w, short h, const unsigned char *gray, const unsigned char *opaque);
    /* Memory: NULL if there's none */
    void *(*alloc)(void *ref, long n);
    void (*release)(void *ref, void *p);
    /* A version 1 PICT (from its picSize on) drawn by the System at w x h, its rows given
       to rect; 0, or an error as size's */
    int (*draw_pict)(void *ref, const unsigned char *pic, long len, short w, short h);
    /* IMG_CHECK_CRC: PNG checks each chunk's CRC */
    long flags;
    /* Inflate, for PNG: Mill's (the host's GzBegin and the rest), or the tests' */
    const GzCalls *gz;
} ImgSink;

enum { IMG_CHECK_CRC = 1 };

/* What data returns */
enum { IMG_MORE = 0, IMG_DONE = 1, IMG_BAD = -1, IMG_NO_MEMORY = -2, IMG_TOO_LARGE = -3 };

/* The formats, in the Plug-In's order */
enum { IMG_MACPAINT, IMG_PICT, IMG_XBM, IMG_BMP, IMG_PNM, IMG_PNG, IMG_FORMATS };

/* begin (NULL if there's no memory); data with each piece: IMG_MORE, IMG_DONE once the
   image is in, IMG_BAD, or an error from size; data with n 0 when there's no more, to finish
   with what's there; end, after done or not, frees it */
void *img_begin(int format, const ImgSink *s);
int img_data(void *d, const unsigned char *p, long n);
void img_end(void *d);

/* ---- for the decoders ---- */

/* What every decoder's state starts with */
typedef struct {
    ImgSink s;
    int format;
} ImgBase;

void *img_new(const ImgSink *s, long size);   /* zeroed */
void img_free(const ImgSink *s, void *p);

/* PackBits (Apple Technical Note 171): one byte in; the bytes it makes go to out (up to
   128 units), and the count comes back. unit is 1, or 2 for PICT's 16-bit runs. */
typedef struct { int unit, lit, run, got; unsigned char v[2]; } ImgPack;
int packbits(ImgPack *k, unsigned char c, unsigned char *out);

/* Gray of 8-bit red, green, and blue */
#define IMG_LUM(r, g, b) ((unsigned char)(((r) * 77L + (g) * 151L + (b) * 28L) >> 8))

void *macpaint_begin(const ImgSink *s);
int macpaint_data(void *d, const unsigned char *p, long n);
void macpaint_end(void *d);
void *pict_begin(const ImgSink *s);
int pict_data(void *d, const unsigned char *p, long n);
void pict_end(void *d);
void *xbm_begin(const ImgSink *s);
int xbm_data(void *d, const unsigned char *p, long n);
void xbm_end(void *d);
void *bmp_begin(const ImgSink *s);
int bmp_data(void *d, const unsigned char *p, long n);
void bmp_end(void *d);
void *pnm_begin(const ImgSink *s);
int pnm_data(void *d, const unsigned char *p, long n);
void pnm_end(void *d);
/* PNG: an interlaced picture keeps a byte for every pixel in an even row and column (two
   with transparency) until its last passes; past PNG_INTERLACE_MAX, IMG_TOO_LARGE, as for
   a row of more than PNG_ROW_MAX bytes at the file's own depth (two are kept) */
#define PNG_INTERLACE_MAX 65536L
#define PNG_ROW_MAX 16384L
void *png_begin(const ImgSink *s);
int png_data(void *d, const unsigned char *p, long n);
void png_end(void *d);

#endif
