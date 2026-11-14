/* Pictures made into 1-bit bitmaps: scaled to the size the page asks for and dithered. */
#ifndef MILL_IMAGE_H
#define MILL_IMAGE_H

/* One bit per pixel, 1 is black, rows padded to 16 bits (QuickDraw's BitMap layout).
   mask, laid out the same, has 1 where the picture covers the page and 0 where the page
   shows through (bits are 0 there too); NULL when it covers it all, as most do. */
typedef struct { short w, h, row_bytes; unsigned char *bits, *mask; } Bitmap1;

#define IMAGE_NO_MEMORY (-2)

/* The size to draw a picture of size sw x sh: aw x ah when both are given (> 0), one of
   them with the other in proportion, else its own size; then smaller, in proportion, if
   it's more than Mill makes */
void image_fit(short sw, short sh, short aw, short ah, short *w, short *h);

/* A picture being made at w x h, from pixels at its own size sw x sh (picture.h's
   decoders give them): kept in gray, then dithered (Bill Atkinson's dither, as MacPaint
   and HyperCard did) into a Bitmap1 when it's all in. Each returns 0, -1 for a size
   Mill won't make, or IMAGE_NO_MEMORY. */
typedef struct ImageCanvas ImageCanvas;
int canvas_new(ImageCanvas **c, short w, short h, short sw, short sh);
/* The pixels will come at another size (a JPEG shrinking as it decodes) */
int canvas_source(ImageCanvas *c, short sw, short sh);
void canvas_size(const ImageCanvas *c, short *w, short *h);
/* Rows of w grays at x, y of its own size; opaque as in picture.h */
void canvas_rect(ImageCanvas *c, short x, short y, short w, short h, const unsigned char *gray, const unsigned char *opaque);
/* Dither into out (photo: tones stretched first); the canvas is still to be freed */
int canvas_finish(ImageCanvas *c, int photo, Bitmap1 *out);
void canvas_free(ImageCanvas *c);
/* A Mac icon resource's data as a picture: type is its four characters ('ics#' and
   'ICN#' carry a mask; 'ICON' and 'SICN', the first of a list, don't). 0, -1 if it
   isn't one of those or is too short, or IMAGE_NO_MEMORY. */
int image_icon(const char *type, const unsigned char *data, long len, Bitmap1 *out);
void image_free(Bitmap1 *b);

#endif
