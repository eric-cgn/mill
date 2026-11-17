/* Image Formats: a Plug-In for Mill that reads MacPaint, PICT, XBM, BMP, Netpbm, and PNG
   images (the decoders are images.h's), giving Mill pixels with PictureSize and PictureRect.
   Its About page is its 'TEXT' 128. */
#include <string.h>
#include <Memory.h>
#include <Quickdraw.h>

#include "MillPlugIn.h"
#include "images.h"
#include "../gzhost/gzhost.h"

typedef struct {
    MillPlugIn base;
    MillMethods methods;        /* filled in by PlugInNew (see MillPlugIn.h's rules) */
    MillFormat formats[IMG_FORMATS];
    MillSetting crc;            /* PNG's */
} Images;

/* One decode */
typedef struct {
    const MillHost *host;
    MillFormatRef ref;
    void *dec;
    short dw, dh;
    GzHost gz;                  /* Mill's inflate, for PNG */
} Run;

static int run_size(void *ref, short w, short h, int photo)
{
    Run *p = ref;
    OSErr err = p->host->PictureSize(p->ref, w, h, (short)photo, &p->dw, &p->dh);
    return err == noErr ? 0 : err == memFullErr ? IMG_NO_MEMORY : IMG_BAD;
}

static void run_drawn(void *ref, short *w, short *h) { Run *p = ref; *w = p->dw; *h = p->dh; }

static void run_rect(void *ref, short x, short y, short w, short h, const unsigned char *gray, const unsigned char *opaque)
{
    Run *p = ref;
    p->host->PictureRect(p->ref, x, y, w, h, gray, opaque);
}

static void *run_alloc(void *ref, long n) { return ((Run *)ref)->host->NewMem(n); }
static void run_release(void *ref, void *q) { ((Run *)ref)->host->FreeMem(q); }

/* A version 1 PICT, drawn by QuickDraw into a bitmap of its own at w x h, then given a row
   at a time */
static int run_draw_pict(void *ref, const unsigned char *pic, long len, short w, short h)
{
    Run *p = ref;
    short rb = (short)((w + 15) / 16 * 2), y, x;
    Handle pict = NewHandle(len);
    unsigned char *gray = NULL;
    GrafPort port;
    GrafPtr was;
    BitMap bm;
    bm.baseAddr = NULL;
    if (pict) bm.baseAddr = NewPtrClear((long)rb * h);
    if (bm.baseAddr) gray = p->host->NewMem(w);
    if (!pict || !bm.baseAddr || !gray) {
        if (pict) DisposeHandle(pict);
        if (bm.baseAddr) DisposePtr(bm.baseAddr);
        return IMG_NO_MEMORY;
    }
    BlockMove(pic, *pict, len);
    bm.rowBytes = rb;
    SetRect(&bm.bounds, 0, 0, w, h);
    GetPort(&was);
    OpenPort(&port);
    SetPortBits(&bm);
    PortSize(w, h);
    RectRgn(port.visRgn, &bm.bounds);
    ClipRect(&bm.bounds);
    EraseRect(&bm.bounds);
    HLock(pict);
    DrawPicture((PicHandle)pict, &bm.bounds);
    ClosePort(&port);
    SetPort(was);
    DisposeHandle(pict);
    for (y = 0; y < h; y++) {
        const unsigned char *row = (unsigned char *)bm.baseAddr + (long)y * rb;
        for (x = 0; x < w; x++) gray[x] = row[x >> 3] & (0x80 >> (x & 7)) ? 0 : 255;
        p->host->PictureRect(p->ref, 0, y, w, 1, gray, NULL);
    }
    DisposePtr(bm.baseAddr);
    p->host->FreeMem(gray);
    return 0;
}

static OSErr images_format(MillPlugIn *self, short selector, MillFormatParams *f)
{
    Run *p = f->state;
    int r;
    switch (selector) {
    case millFormatBegin: {
        ImgSink s;
        char v[8];
        if (!(p = self->host->NewMem(sizeof *p))) return memFullErr;
        p->host = self->host;
        p->ref = f->ref;
        p->dw = p->dh = 0;
        s.ref = p; s.size = run_size; s.drawn = run_drawn; s.rect = run_rect;
        s.alloc = run_alloc; s.release = run_release; s.draw_pict = run_draw_pict;
        gzhost_init(&p->gz, self->host, (MillReqRef)f->ref);
        s.gz = &p->gz.calls;
        /* png.crc: "yes" to check PNG's CRCs (Mill Settings; off unless set) */
        s.flags = f->format == IMG_PNG && self->host->GetSetting("png", "crc", v, sizeof v) == 3 && !strcmp(v, "yes") ?
                  IMG_CHECK_CRC : 0;
        if (!(p->dec = img_begin(f->format, &s))) { self->host->FreeMem(p); return memFullErr; }
        f->state = p;
        return noErr;
    }
    case millFormatData:
        r = img_data(p->dec, (const unsigned char *)f->data, f->len);
        return r == IMG_MORE ? noErr : r == IMG_DONE ? millDone : r == IMG_NO_MEMORY || r == IMG_TOO_LARGE ? memFullErr : paramErr;
    case millFormatEnd:
        if (!p) return noErr;
        /* that's all: finish (an interlaced PNG cut short, too: its missing pixels come
           from the passes that came) */
        if (!f->cut || (f->format == IMG_PNG && f->cut != millCutMemory)) img_data(p->dec, (const unsigned char *)"", 0);
        img_end(p->dec);
        self->host->FreeMem(p);
        f->state = NULL;
        return noErr;
    }
    return millUnknown;
}

static void images_close(MillPlugIn *self) { self->host->FreeMem(self); }

static void format(MillFormat *f, const char *key, const char *heading, const char *description,
                   const char *types, const char *extensions, const char *fileTypes)
{
    f->key = key;
    f->heading = heading;
    f->description = description;
    f->types = types;
    f->extensions = extensions;
    f->fileTypes = fileTypes;
    f->gives = millGivesPicture;
    f->settingCount = 0;
    f->settings = 0;
}

MillPlugIn *PlugInNew(const MillHost *host, short resFile, MillInfo *info)
{
    Images *me = host->NewMem(sizeof *me);
    MillFormat *f;
    (void)resFile;
    if (!me) return 0;
    me->methods.close = images_close;
    me->methods.request = 0;
    me->methods.purge = 0;
    me->methods.format = images_format;
    me->methods.font = 0;
    me->methods.gzOut = gzhost_out;
    me->base.methods = &me->methods;
    f = me->formats;
    /* in images.h's order (IMG_MACPAINT...) */
    format(f++, "macpaint", "MacPaint", "MacPaint document", "image/x-macpaint", "mac pntg", "PNTG");
    format(f++, "pict", "PICT", "QuickDraw PICT (version 1 and 2)", "image/pict image/x-pict", "pict pct", "PICT");
    format(f++, "xbm", "XBM", "X Window System bitmap (X10, X11)", "image/x-xbitmap image/x-xbm", "xbm", "");
    format(f++, "bmp", "BMP", "Windows and OS/2 bitmap", "image/bmp image/x-ms-bmp image/x-bmp", "bmp", "BMPf BMP ");
    format(f++, "netpbm", "Netpbm", "Portable bitmap, graymap, and pixmap (PBM, PGM, PPM)",
           "image/x-portable-bitmap image/x-portable-graymap image/x-portable-pixmap image/x-portable-anymap", "pbm pgm ppm pnm", "");
    format(f++, "png", "PNG", "Portable Network Graphics (PNG 1.0)", "image/png image/x-png", "png", "PNGf");
    me->crc.key = "crc";
    me->crc.label = "Check CRCs";
    me->crc.note = "Checks each chunk against its CRC. Slower.";
    me->crc.kind = millCheckbox;
    me->crc.dflt = "no";
    me->crc.min = me->crc.max = 0;
    me->crc.choices = 0;
    f[-1].settingCount = 1;
    f[-1].settings = &me->crc;
    info->name = IMG_NAME;
    info->version = "1.0";
    info->copyright = "Copyright 2026 OneInAMillion Software";
    info->by = "OneInAMillion Software";
    info->description = "MacPaint, PICT, XBM, BMP, Netpbm, and PNG images.";
    info->flags = 0;
    info->protocolCount = 0;
    info->protocols = 0;
    info->formatCount = IMG_FORMATS;
    info->formats = me->formats;
    return &me->base;
}
