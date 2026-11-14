/* JPEG as a built-in format Plug-In: pictures, read as they arrive (core/jpeg.c). */
#include <Multiverse.h>
#include <string.h>

#define MILL_BUILTIN
#define MILL_ENTRY jpeg_main
#define MILL_NEW jpeg_new
#define MILL_CLOSE jpeg_close
#define MILL_FORMAT picture_format
#include "../sdk/Glue/MillGlue.c"
#include "version.h"

#define DECODER_BEGIN jpeg_begin
#define DECODER_DATA jpeg_data
#define DECODER_END jpeg_end
#include "plug_picture.h"

static void jpeg_close(MillPlugIn *self) { self->host->FreeMem(self); }

static const MillFormat formats[] = {
    { "jpeg", "JPEG", "JPEG File Interchange Format, baseline", "image/jpeg image/pjpeg", "jpg jpeg jpe", "JPEG", millGivesPicture, 0, NULL }
};

MillPlugIn *jpeg_new(const MillHost *host, short resFile, MillInfo *info)
{
    MillPlugIn *me = host->NewMem(sizeof *me);
    (void)resFile;
    if (!me) return NULL;
    me->methods = NULL;     /* a built-in's are called by name (MillGlue.c) */
    info->name = "jpeg";
    info->version = "1.0";
    info->copyright = "\251 " MILL_YEAR " " MILL_PUBLISHER;
    info->by = MILL_PUBLISHER;
    info->description = "JPEG File Interchange Format, baseline.";
    info->formatCount = 1;
    info->formats = formats;
    return me;
}
