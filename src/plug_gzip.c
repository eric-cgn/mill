/* gzip as a built-in format Plug-In: a stream, unpacked as it arrives (core/inflate.c),
   for .gz files and Content-Encoding: gzip. What's inside goes on to the format for
   its type, or to the page. */
#include <Multiverse.h>
#include <string.h>

#define MILL_BUILTIN
#define MILL_ENTRY gzip_main
#define MILL_NEW gzip_new
#define MILL_CLOSE gzip_close
#define MILL_FORMAT gzip_format
#include "../sdk/Glue/MillGlue.c"
#include "core/inflate.h"
#include "version.h"

typedef struct { const MillHost *host; MillFormatRef ref; void *gz; } GzRun;

static int gz_write(void *ref, const unsigned char *p, long n)
{
    GzRun *g = ref;
    return !g->host->Write((MillReqRef)g->ref, p, n);
}

static OSErr gzip_format(MillPlugIn *self, short selector, MillFormatParams *f)
{
    GzRun *g = f->state;
    int r;
    switch (selector) {
    case millFormatBegin: {
        /* what's inside: its type by its name without .gz (Mill sets it for Content-Encoding) */
        char name[300], *dot;
        strncpy(name, f->url, sizeof name - 1);
        name[sizeof name - 1] = 0;
        name[strcspn(name, "?#")] = 0;
        if ((dot = strrchr(name, '.')) && !strchr(dot, '/')) *dot = 0;
        self->host->TypeForName(name, f->outType, sizeof f->outType);
        if (!(g = self->host->NewMem(sizeof *g))) return memFullErr;
        g->host = self->host;
        g->ref = f->ref;
        if (!(g->gz = gz_begin(gz_write, g))) { self->host->FreeMem(g); return memFullErr; }
        f->state = g;
        return noErr;
    }
    case millFormatData:
        r = gz_data(g->gz, (const unsigned char *)f->data, f->len);
        return r == 0 ? noErr : r == 1 || r == -2 ? millDone : paramErr;
    case millFormatEnd:
        if (!g) return noErr;
        gz_end(g->gz);
        self->host->FreeMem(g);
        f->state = NULL;
        return noErr;
    }
    return millUnknown;
}

static void gzip_close(MillPlugIn *self) { self->host->FreeMem(self); }

static const MillFormat formats[] = {
    { "gzip", "gzip", "GNU zip (RFC 1952), and Content-Encoding: gzip", "application/gzip application/x-gzip", "gz", "Gzip",
      millGivesStream, 0, NULL }
};

MillPlugIn *gzip_new(const MillHost *host, short resFile, MillInfo *info)
{
    MillPlugIn *me = host->NewMem(sizeof *me);
    (void)resFile;
    if (!me) return NULL;
    me->methods = NULL;     /* a built-in's are called by name (MillGlue.c) */
    info->name = "gzip";
    info->version = "1.0";
    info->copyright = "\251 " MILL_YEAR " " MILL_PUBLISHER;
    info->by = MILL_PUBLISHER;
    info->description = "Unpacks gzip files as they arrive.";
    info->formatCount = 1;
    info->formats = formats;
    return me;
}
