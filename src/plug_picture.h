/* What the built-in picture formats (GIF, JPEG, Netpbm) share: a decoder from
   core/picture.h, wrapped as a format. Each plug_*.c defines DECODER_BEGIN, _DATA, and
   _END, then includes this, so each has its own copy in its own segment. */
#include "core/picture.h"

typedef struct {
    const MillHost *host;
    MillFormatRef ref;
    void *dec;
    short dw, dh;
} PicRun;

static int pic_size(void *ref, short w, short h, int photo)
{
    PicRun *p = ref;
    OSErr err = p->host->PictureSize(p->ref, w, h, (short)photo, &p->dw, &p->dh);
    return err == noErr ? 0 : err == memFullErr ? -2 : -1;
}

static void pic_drawn(void *ref, short *w, short *h) { PicRun *p = ref; *w = p->dw; *h = p->dh; }

static void pic_rect(void *ref, short x, short y, short w, short h, const unsigned char *gray, const unsigned char *opaque)
{
    PicRun *p = ref;
    p->host->PictureRect(p->ref, x, y, w, h, gray, opaque);
}

static OSErr picture_format(MillPlugIn *self, short selector, MillFormatParams *f)
{
    PicRun *p = f->state;
    int r;
    switch (selector) {
    case millFormatBegin: {
        PicSink s;
        if (!(p = self->host->NewMem(sizeof *p))) return memFullErr;
        p->host = self->host;
        p->ref = f->ref;
        p->dw = p->dh = 0;
        s.ref = p; s.size = pic_size; s.drawn = pic_drawn; s.rect = pic_rect;
        if (!(p->dec = DECODER_BEGIN(&s))) { self->host->FreeMem(p); return memFullErr; }
        f->state = p;
        return noErr;
    }
    case millFormatData:
        r = DECODER_DATA(p->dec, (const unsigned char *)f->data, f->len);
        return r == 0 ? noErr : r == 1 ? millDone : r == -2 ? memFullErr : paramErr;
    case millFormatEnd:
        if (!p) return noErr;
        if (!f->cut) DECODER_DATA(p->dec, (const unsigned char *)"", 0);   /* that's all: finish */
        DECODER_END(p->dec);
        self->host->FreeMem(p);
        f->state = NULL;
        return noErr;
    }
    return millUnknown;
}
