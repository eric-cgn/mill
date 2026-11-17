/* NFS: a Plug-In for Mill for Unix file servers.
     nfs://host/export/path     NFS version 3 (nfs.c)
   plugin://nfs/about explains the addresses and has a form, filled in with the Default
   server setting. */
#include <string.h>

#include "MillPlugIn.h"
#include "nfs.h"

enum { HOST = 256 };

typedef struct {
    MillPlugIn base;
    MillMethods methods;        /* filled in by PlugInNew (see MillPlugIn.h's rules) */
    MillProtocol protocol;
} Nfs1;

static void nfs_close(MillPlugIn *self) { self->host->FreeMem(self); }

static void put(MillRequest *r, const MillHost *h, const char *s) { h->Write(r->ref, s, strlen(s)); }

/* plugin://nfs/about: the addresses, and a form */
static OSErr do_about(Nfs1 *me, MillRequest *r)
{
    const MillHost *h = me->base.host;
    char host[HOST], esc[HOST * 6];
    if (h->GetSetting("nfs", "server", host, sizeof host) < 0) host[0] = 0;
    h->HtmlEscape(host, esc, sizeof esc);
    h->PlugInPage(r->ref);
    put(r, h, "HTTP/1.0 200 OK" MILL_CRLF "Content-Type: text/html" MILL_CRLF MILL_CRLF
        "<HTML><HEAD><TITLE>" NFS_TITLE "</TITLE></HEAD><BODY><H2>" NFS_TITLE "</H2>\n"
        "<P>Address formats:<UL>"
        "<LI>nfs://host/ for its exports"
        "<LI>nfs://host/export/path"
        "</UL>Or use the form below.</P>\n"
        "<FORM ACTION=\"plugin://" NFS_NAME "/go\"><P><B>nfs</B> server "
        "<INPUT NAME=host SIZE=20 VALUE=\"");
    put(r, h, esc);
    put(r, h, "\"> <INPUT TYPE=SUBMIT VALUE=\"Open\"></P></FORM>\n</BODY></HTML>\n");
    return noErr;
}

/* plugin://nfs/go?host=server, from the about page's form: a redirect to the address */
static OSErr do_go(Nfs1 *me, MillRequest *r)
{
    const MillHost *h = me->base.host;
    const char *q = strchr(r->path, '?');
    char host[HOST], url[HOST + 16];
    long args[1], ql = q ? (long)strlen(++q) : 0, i, o = 0;
    if (h->From(r->ref) != millFromOwnPage) { h->Fail(r->ref, NFS_TITLE ": Only its own page can send this."); return noErr; }
    if (!q || h->FormField(q, ql, "host", host, sizeof host) < 0) {
        h->Fail(r->ref, NFS_TITLE ": The submitted data was invalid.");
        return noErr;
    }
    for (i = 0; host[i]; i++) {     /* a host name, and a port after a colon */
        char c = host[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == ':') host[o++] = c;
    }
    host[o] = 0;
    if (!host[0]) { h->Fail(r->ref, NFS_TITLE ": Enter a server name."); return noErr; }
    args[0] = (long)host;
    h->Format(url, sizeof url, "HTTP/1.0 302 Found" MILL_CRLF "Location: nfs://%s/" MILL_CRLF MILL_CRLF, args);
    put(r, h, url);
    return noErr;
}

static OSErr plugin_request(MillPlugIn *self, MillRequest *r)
{
    Nfs1 *me = (Nfs1 *)self;
    const MillHost *h = me->base.host;
    MillPlugIn *s;
    OSErr err;
    if (r->protocol < 0) {
        if (!strcmp(r->path, "/about")) return do_about(me, r);
        if (!strncmp(r->path, "/go?", 4)) return do_go(me, r);
        return millUnknown;
    }
    if (r->protocol != 0) return millUnknown;
    if (!(s = nfs_state(h))) return memFullErr;
    err = nfs_request(s, r);
    h->FreeMem(s);
    return err;
}

MillPlugIn *PlugInNew(const MillHost *host, short resFile, MillInfo *info)
{
    Nfs1 *me = host->NewMem(sizeof *me);
    (void)resFile;
    if (!me) return 0;
    me->methods.close = nfs_close;
    me->methods.request = plugin_request;
    me->methods.purge = 0;
    me->methods.format = 0;
    me->methods.font = 0;
    me->methods.gzOut = 0;
    me->base.methods = &me->methods;
    me->protocol = *nfs_protocol();
    info->name = NFS_NAME;
    info->version = "1.0";
    info->copyright = "Copyright 2026 OneInAMillion Software";
    info->by = "OneInAMillion Software";
    info->description = "NFS (Unix file sharing).";
    info->flags = millHasAbout;
    info->protocolCount = 1;
    info->protocols = &me->protocol;
    info->formatCount = 0;
    info->formats = 0;
    return &me->base;
}
