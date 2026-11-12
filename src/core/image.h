/* GIF and JPEG pictures made into 1-bit bitmaps: scaled to the size the page asks
   for and dithered with Bill Atkinson's dither, as MacPaint and HyperCard did. */
#ifndef MILL_IMAGE_H
#define MILL_IMAGE_H

/* One bit per pixel, 1 is black, rows padded to 16 bits (QuickDraw's BitMap layout).
   mask, laid out the same, has 1 where the picture covers the page and 0 where the page
   shows through (bits are 0 there too); NULL when it covers it all, as most do. */
typedef struct { short w, h, row_bytes; unsigned char *bits, *mask; } Bitmap1;

/* A GIF's or JPEG's own size; returns 0, or -1 if it's neither */
int image_dims(const unsigned char *data, long len, short *w, short *h);
/* Told how far decoding has got, in percent, every few percent; returning nonzero stops it */
typedef int (*image_progress)(void *ref, int percent);
/* Decode, scale to w x h, and dither. Returns 0, -1 (bad data, unsupported, or stopped),
   or IMAGE_NO_MEMORY. Only a GIF's first frame is used, and a GIF with a transparent
   color gets a mask. progress may be NULL. */
#define IMAGE_NO_MEMORY (-2)
int image_decode(const unsigned char *data, long len, short w, short h, Bitmap1 *out,
                 image_progress progress, void *ref);
void image_free(Bitmap1 *b);

#endif
