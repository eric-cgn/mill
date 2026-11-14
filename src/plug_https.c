/* https: as a built-in Plug-In. Mill can't do TLS itself, so an https: address goes
   through the HTTP proxy (ProxyRequest) whenever HTTPS is on; with no proxy set, an
   alert says so. */
#include <Multiverse.h>
#include <string.h>

#define MILL_BUILTIN
#define MILL_ENTRY https_main
#define MILL_NEW https_new
#define MILL_CLOSE https_close
#define MILL_REQUEST https_request
#include "../sdk/Glue/MillGlue.c"
#include "version.h"

static OSErr https_request(MillPlugIn *self, MillRequest *r)
{
    OSErr err;
    /* on (Mill's own Enable) means through the HTTP proxy; none set, an alert */
    if ((err = self->host->ProxyRequest(r->ref)) != millNoProxy) return err;
    self->host->Fail(r->ref, "TLS requires an HTTP proxy.");
    return noErr;
}

static void https_close(MillPlugIn *self) { self->host->FreeMem(self); }

static const MillProtocol protocols[] = { { "https", "HTTPS", "Secure HTTP via proxy", 0, NULL } };

MillPlugIn *https_new(const MillHost *host, short resFile, MillInfo *info)
{
    MillPlugIn *me = host->NewMem(sizeof *me);
    (void)resFile;
    if (!me) return NULL;
    me->methods = NULL;     /* a built-in's are called by name (MillGlue.c) */
    info->name = "https";
    info->version = "1.0";
    info->copyright = "\251 " MILL_YEAR " " MILL_PUBLISHER;
    info->by = MILL_PUBLISHER;
    info->description = "Secure sites, through the HTTP proxy.";
    info->protocolCount = 1;
    info->protocols = protocols;
    return me;
}
