/* gzhost.h: the GzCalls (src/core/inflate.h) a Plug-In's cores use, made of Mill's own
   inflate, the host's GzBegin, GzData, GzUnused, and GzEnd, so no Plug-In carries a copy.
   What Mill inflates comes back through the entry point (millGzOut): set the Plug-In's
   methods.gzOut to gzhost_out. */
#ifndef GZHOST_H
#define GZHOST_H

#include "MillPlugIn.h"
#include "../../src/core/inflate.h"

/* What the calls need: the host, and the request or format run they're for */
typedef struct {
    GzCalls calls;
    const MillHost *host;
    MillReqRef r;
} GzHost;

/* Fill g in; then &g->calls is the cores' */
void gzhost_init(GzHost *g, const MillHost *host, MillReqRef r);
/* methods.gzOut */
OSErr gzhost_out(MillPlugIn *self, MillGzOutParams *p);

#endif
