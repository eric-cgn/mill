/* Finger: an example Plug-In for Mill. finger://host/user asks host about user
   (RFC 1288) and shows the answer as text; finger://host/ asks about everyone. */
#include <string.h>

#include "MillPlugIn.h"

typedef struct {
    MillPlugIn base;            /* first, so this is a MillPlugIn too */
    MillMethods methods;        /* filled in by PlugInNew (see MillPlugIn.h's rules) */
    MillProtocol protocol;
} Finger;

static void finger_close(MillPlugIn *self)
{
    self->host->FreeMem(self);
}

static OSErr finger_request(MillPlugIn *self, MillRequest *r)
{
    const MillHost *h = self->host;
    char query[128], buf[512];
    long n, k;
    MillConn c;
    OSErr err;
    if (r->protocol < 0) return millUnknown;    /* no plugin:// pages of its own */
    /* the user is the path after its slash, %-decoded */
    if (h->UrlDecode(r->path + 1, query, sizeof query - 2) < 0) query[0] = 0;
    for (k = 0; query[k]; k++) if (query[k] == '?' || query[k] == '/') query[k] = 0;
    n = strlen(query);
    query[n++] = 13;    /* CR LF (see MILL_CRLF) */
    query[n++] = 10;
    if ((err = h->TcpOpen(r->ref, r->host, r->port ? r->port : 79, &c)) != noErr) return err;
    if ((err = h->TcpSend(c, query, n)) != noErr) { h->TcpClose(c); return err; }
    h->Write(r->ref, "HTTP/1.0 200 OK" MILL_CRLF "Content-Type: text/plain" MILL_CRLF MILL_CRLF, 45);
    for (;;) {
        n = sizeof buf;
        if ((err = h->TcpRecv(c, buf, &n)) != noErr || n == 0) break;
        if (!h->Write(r->ref, buf, n)) break;
    }
    h->TcpClose(c);
    return err == -128 ? err : noErr;   /* userCanceledErr: stopped */
}

MillPlugIn *PlugInNew(const MillHost *host, short resFile, MillInfo *info)
{
    Finger *me = host->NewMem(sizeof *me);
    (void)resFile;
    if (!me) return 0;
    me->methods.close = finger_close;
    me->methods.request = finger_request;
    me->methods.purge = 0;
    me->methods.format = 0;
    me->base.methods = &me->methods;
    me->protocol.scheme = "finger";
    me->protocol.heading = "Finger";
    me->protocol.description = "Finger User Information Protocol (RFC 1288)";
    me->protocol.settingCount = 0;
    me->protocol.settings = 0;
    info->name = "finger";
    info->version = "1.0";
    info->copyright = "Example";
    info->by = "OneInAMillion Software";
    info->description = "Who's on a host, and about its users (RFC 1288).";
    info->protocolCount = 1;
    info->protocols = &me->protocol;
    return &me->base;
}
