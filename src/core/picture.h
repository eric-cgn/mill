/* Picture decoders that read a file a piece at a time, as it arrives, and give pixels
   as they go: GIF, JPEG (baseline), and Netpbm. Mill's format Plug-Ins wrap them; the
   pixels go to a PicSink (Mill's canvas, or a test's). */
#ifndef MILL_PICTURE_H
#define MILL_PICTURE_H

typedef struct {
    void *ref;
    /* The size pixels will come at, and whether it's a photo (tones stretched before
       dithering); called before any pixels, and again if a decoder changes it (JPEG
       can shrink as it decodes). 0 to go on, else stop with that. */
    int (*size)(void *ref, short w, short h, int photo);
    /* The draw size, once size has been called (for JPEG's choice of shrinking) */
    void (*drawn)(void *ref, short *w, short *h);
    /* Rows of w grays (0 black, 255 white) at x, y; opaque, if not NULL, is 0 where
       the page shows through, and is given from the first call on, or never */
    void (*rect)(void *ref, short x, short y, short w, short h, const unsigned char *gray, const unsigned char *opaque);
} PicSink;

/* Each decoder: begin (NULL if there's no memory); data with each piece, which returns
   0 to go on, 1 once the picture is done, -1 for data it can't read, or the error size
   gave; data with n 0 when there's no more, to finish with what's there; end, after
   done or not, frees it */
void *gif_begin(const PicSink *s);
int gif_data(void *d, const unsigned char *p, long n);
void gif_end(void *d);

void *jpeg_begin(const PicSink *s);
int jpeg_data(void *d, const unsigned char *p, long n);
void jpeg_end(void *d);

void *pnm_begin(const PicSink *s);
int pnm_data(void *d, const unsigned char *p, long n);
void pnm_end(void *d);

#endif
