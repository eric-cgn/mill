/* Netpbm as a built-in format Plug-In: pictures, read as they arrive (core/pnm.c). */
#include <Multiverse.h>
#include <string.h>

#define MILL_BUILTIN
#define MILL_ENTRY netpbm_main
#define MILL_NEW netpbm_new
#define MILL_CLOSE netpbm_close
#define MILL_FORMAT picture_format
#include "../sdk/Glue/MillGlue.c"
#include "version.h"

#define DECODER_BEGIN pnm_begin
#define DECODER_DATA pnm_data
#define DECODER_END pnm_end
#include "plug_picture.h"

static void netpbm_close(MillPlugIn *self) { self->host->FreeMem(self); }

static const MillFormat formats[] = {
    { "netpbm", "Netpbm", "Portable bitmap, graymap, and pixmap (PBM, PGM, PPM)", "image/x-portable-bitmap image/x-portable-graymap image/x-portable-pixmap image/x-portable-anymap", "pbm pgm ppm pnm", "", millGivesPicture, 0, NULL }
};

MillPlugIn *netpbm_new(const MillHost *host, short resFile, MillInfo *info)
{
    MillPlugIn *me = host->NewMem(sizeof *me);
    (void)resFile;
    if (!me) return NULL;
    me->methods = NULL;     /* a built-in's are called by name (MillGlue.c) */
    info->name = "netpbm";
    info->version = "1.0";
    info->copyright = "\251 " MILL_YEAR " " MILL_PUBLISHER;
    info->by = MILL_PUBLISHER;
    info->description = "Portable bitmap, graymap, and pixmap (PBM, PGM, PPM).";
    info->formatCount = 1;
    info->formats = formats;
    return me;
}
