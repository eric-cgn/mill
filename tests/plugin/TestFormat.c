/* Test Format: a format Plug-In for trying Mill's formats on a Mac, never shipped. Its one
   format has the key "macpaint", so it replaces Image Formats' MacPaint (MillPlugIn.h,
   Formats: it loads after it, as "Test Format" follows "Image Formats" in the folder), and
   reads files named .mtest. Its Begin writes the page's start, which Mill keeps until it
   has answered, and says the page is Mac Roman (outType); the file then goes through as
   the page's text, and End closes it. A file with ê in Mac Roman (0x90) shows ê only if
   Mill reads the page as outType says. */
#include <string.h>
#include "MillPlugIn.h"

typedef struct {
    MillPlugIn base;
    MillMethods methods;
    MillFormat format;
} Test;

static OSErr test_format(MillPlugIn *self, short selector, MillFormatParams *f)
{
    const MillHost *h = self->host;
    const char *s;
    switch (selector) {
    case millFormatBegin:
        s = "<TITLE>Test Format</TITLE><H2>Test Format</H2><P>Written during Begin.<PRE>";
        h->Write((MillReqRef)f->ref, s, strlen(s));
        strcpy(f->outType, "text/html; charset=macintosh");
        f->answer = millShowNow;
        return noErr;
    case millFormatData:
        h->Write((MillReqRef)f->ref, f->data, f->len);
        return noErr;
    case millFormatEnd:
        s = "</PRE><P>Written at the end.";
        h->Write((MillReqRef)f->ref, s, strlen(s));
        return noErr;
    }
    return millUnknown;
}

static void test_close(MillPlugIn *self) { self->host->FreeMem(self); }

MillPlugIn *PlugInNew(const MillHost *host, short resFile, MillInfo *info)
{
    Test *me = host->NewMem(sizeof *me);
    (void)resFile;
    if (!me) return 0;
    memset(me, 0, sizeof *me);
    me->methods.close = test_close;
    me->methods.format = test_format;
    me->base.methods = &me->methods;
    me->format.key = "macpaint";
    me->format.heading = "Test Format";
    me->format.description = "Mac Roman text as a page, for testing";
    me->format.types = "text/x-mill-test";
    me->format.extensions = "mtest";
    me->format.fileTypes = "";
    me->format.gives = millGivesElements;
    info->name = "test-format";
    info->version = "1.0";
    info->copyright = "";
    info->by = "OneInAMillion Software";
    info->description = "A format for testing Mill.";
    info->formatCount = 1;
    info->formats = &me->format;
    return &me->base;
}
