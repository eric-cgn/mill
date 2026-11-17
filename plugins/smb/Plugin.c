/* SMB: a Plug-In for Mill for Windows and Samba file servers.
     smb://host/share/path      SMB, as Windows and Samba share files (smb.c)
   Every address names its server. plugin://smb/about explains the addresses and
   has a form, filled in with the protocol's Default server setting. */
#include <string.h>

#include "MillPlugIn.h"
#include "plugin.h"

enum { kSmb = 0, kProtocols };
enum { HOST = 256 };

typedef struct {
    MillPlugIn base;
    MillMethods methods;        /* filled in by PlugInNew (see MillPlugIn.h's rules) */
    MillProtocol protocol[kProtocols];
} Files;

static void files_close(MillPlugIn *self) { self->host->FreeMem(self); }

static void put(MillRequest *r, const MillHost *h, const char *s) { h->Write(r->ref, s, strlen(s)); }

/* plugin://smb/about: the addresses, and a form for each protocol */
static OSErr do_about(Files *me, MillRequest *r)
{
    const MillHost *h = me->base.host;
    char host[HOST], esc[HOST * 6];
    if (h->GetSetting("smb", "server", host, sizeof host) < 0) host[0] = 0;
    h->HtmlEscape(host, esc, sizeof esc);
    h->PlugInPage(r->ref);
    put(r, h, "HTTP/1.0 200 OK" MILL_CRLF "Content-Type: text/html" MILL_CRLF MILL_CRLF
        "<HTML><HEAD><TITLE>" SMB_TITLE "</TITLE></HEAD><BODY><H2>" SMB_TITLE "</H2>\n"
        "<P>Address formats:<UL>"
        "<LI>smb://host/ for its shares"
        "<LI>smb://host/share/path"
        "</UL>Or use the form below.</P>\n"
        "<FORM ACTION=\"plugin://" SMB_NAME "/go\"><P><INPUT TYPE=HIDDEN NAME=p VALUE=smb><B>smb</B> server "
        "<INPUT NAME=host SIZE=20 VALUE=\"");
    put(r, h, esc);
    put(r, h, "\"> <INPUT TYPE=SUBMIT VALUE=\"Open\"></P></FORM>\n</BODY></HTML>\n");
    return noErr;
}

/* plugin://smb/go?p=smb&host=server, from the about page's form: a redirect to
   the address in full */
static OSErr do_go(Files *me, MillRequest *r)
{
    const MillHost *h = me->base.host;
    const char *q = strchr(r->path, '?');
    char scheme[12], host[HOST], url[HOST + 16];
    long args[2], ql = q ? (long)strlen(++q) : 0, i, o = 0;
    if (h->From(r->ref) != millFromOwnPage) { h->Fail(r->ref, SMB_TITLE ": Only its own page can send this."); return noErr; }
    if (!q || h->FormField(q, ql, "p", scheme, sizeof scheme) <= 0 || strcmp(scheme, "smb") ||
        h->FormField(q, ql, "host", host, sizeof host) < 0) {
        h->Fail(r->ref, SMB_TITLE ": The submitted data was invalid.");
        return noErr;
    }
    for (i = 0; host[i]; i++) {     /* a host name, and a port after a colon */
        char c = host[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == ':') host[o++] = c;
    }
    host[o] = 0;
    if (!host[0]) { h->Fail(r->ref, SMB_TITLE ": Enter a server name."); return noErr; }
    args[0] = (long)scheme;
    args[1] = (long)host;
    h->Format(url, sizeof url, "HTTP/1.0 302 Found" MILL_CRLF "Location: %s://%s/" MILL_CRLF MILL_CRLF, args);
    put(r, h, url);
    return noErr;
}

static OSErr files_request(MillPlugIn *self, MillRequest *r)
{
    Files *me = (Files *)self;
    const MillHost *h = me->base.host;
    MillPlugIn *s;
    OSErr err;
    if (r->protocol < 0) {
        if (!strcmp(r->path, "/about")) return do_about(me, r);
        if (!strncmp(r->path, "/go?", 4)) return do_go(me, r);
        return millUnknown;
    }
    if (r->protocol != kSmb) return millUnknown;
    if (!(s = smb_state(h))) return memFullErr;
    err = smb_request(s, r);
    h->FreeMem(s);
    return err;
}

MillPlugIn *PlugInNew(const MillHost *host, short resFile, MillInfo *info)
{
    Files *me = host->NewMem(sizeof *me);
    (void)resFile;
    if (!me) return 0;
    me->methods.close = files_close;
    me->methods.request = files_request;
    me->methods.purge = 0;
    me->methods.format = 0;
    me->methods.font = 0;
    me->methods.gzOut = 0;
    me->base.methods = &me->methods;
    me->protocol[kSmb] = *smb_protocol();
    info->name = SMB_NAME;
    info->version = "1.0";
    info->copyright = "Copyright 2026 OneInAMillion Software";
    info->by = "OneInAMillion Software";
    info->description = "SMB (Windows and Samba file sharing).";
    info->flags = millHasAbout;
    info->protocolCount = kProtocols;
    info->protocols = me->protocol;
    info->formatCount = 0;
    info->formats = 0;
    return &me->base;
}
