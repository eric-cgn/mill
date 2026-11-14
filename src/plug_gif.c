/* GIF as a built-in format Plug-In: pictures, read as they arrive (core/gif.c). */
#include <Multiverse.h>
#include <string.h>

#define MILL_BUILTIN
#define MILL_ENTRY gif_main
#define MILL_NEW gif_new
#define MILL_CLOSE gif_close
#define MILL_FORMAT picture_format
#include "../sdk/Glue/MillGlue.c"
#include "version.h"

#define DECODER_BEGIN gif_begin
#define DECODER_DATA gif_data
#define DECODER_END gif_end
#include "plug_picture.h"

static void gif_close(MillPlugIn *self) { self->host->FreeMem(self); }

static const MillFormat formats[] = {
    { "gif", "GIF", "Graphics Interchange Format (GIF87a, GIF89a)", "image/gif", "gif", "GIFf", millGivesPicture, 0, NULL }
};

MillPlugIn *gif_new(const MillHost *host, short resFile, MillInfo *info)
{
    MillPlugIn *me = host->NewMem(sizeof *me);
    (void)resFile;
    if (!me) return NULL;
    me->methods = NULL;     /* a built-in's are called by name (MillGlue.c) */
    info->name = "gif";
    info->version = "1.0";
    info->copyright = "\251 " MILL_YEAR " " MILL_PUBLISHER;
    info->by = MILL_PUBLISHER;
    info->description = "Graphics Interchange Format (GIF87a, GIF89a).";
    info->formatCount = 1;
    info->formats = formats;
    return me;
}
