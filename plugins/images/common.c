/* What the Image Formats decoders share: picking one, memory, and PackBits. */
#include <string.h>

#include "images.h"

void *img_new(const ImgSink *s, long size)
{
    void *p = s->alloc(s->ref, size);
    if (p) memset(p, 0, size);
    return p;
}

void img_free(const ImgSink *s, void *p)
{
    if (p) s->release(s->ref, p);
}

void *img_begin(int format, const ImgSink *s)
{
    ImgBase *b;
    switch (format) {
    case IMG_MACPAINT: b = macpaint_begin(s); break;
    case IMG_PICT: b = pict_begin(s); break;
    case IMG_XBM: b = xbm_begin(s); break;
    case IMG_BMP: b = bmp_begin(s); break;
    case IMG_PNM: b = pnm_begin(s); break;
    case IMG_PNG: b = png_begin(s); break;
    default: return NULL;
    }
    if (b) b->format = format;
    return b;
}

int img_data(void *d, const unsigned char *p, long n)
{
    switch (((ImgBase *)d)->format) {
    case IMG_MACPAINT: return macpaint_data(d, p, n);
    case IMG_PICT: return pict_data(d, p, n);
    case IMG_XBM: return xbm_data(d, p, n);
    case IMG_BMP: return bmp_data(d, p, n);
    case IMG_PNM: return pnm_data(d, p, n);
    case IMG_PNG: return png_data(d, p, n);
    }
    return IMG_BAD;
}

void img_end(void *d)
{
    if (!d) return;
    switch (((ImgBase *)d)->format) {
    case IMG_MACPAINT: macpaint_end(d); break;
    case IMG_PICT: pict_end(d); break;
    case IMG_XBM: xbm_end(d); break;
    case IMG_BMP: bmp_end(d); break;
    case IMG_PNM: pnm_end(d); break;
    case IMG_PNG: png_end(d); break;
    }
}

/* A header byte n of 0 to 127 is n + 1 units as they are; -1 to -127, the next unit
   1 - n times; -128 is nothing */
int packbits(ImgPack *k, unsigned char c, unsigned char *out)
{
    int i, n;
    if (k->lit) { k->lit--; out[0] = c; return 1; }
    if (k->run) {
        k->v[k->got++] = c;
        if (k->got < k->unit) return 0;
        for (i = n = 0; i < k->run; i++) { out[n++] = k->v[0]; if (k->unit == 2) out[n++] = k->v[1]; }
        k->run = k->got = 0;
        return n;
    }
    if (c < 128) k->lit = (c + 1) * k->unit;
    else if (c > 128) k->run = 257 - c;
    return 0;
}
