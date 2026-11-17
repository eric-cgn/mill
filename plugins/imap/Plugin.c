/* IMAP: a Plug-In for Mill for reading mail.
     imap://user@host/mailbox   IMAP (imap.c)
   plugin://imap/about explains the addresses and has a form, filled in with the Default
   server setting. */
#include <string.h>

#include "MillPlugIn.h"
#include "imap.h"

enum { HOST = 256 };

typedef struct {
    MillPlugIn base;
    MillMethods methods;        /* filled in by PlugInNew (see MillPlugIn.h's rules) */
    MillProtocol protocol;
} Imap1;

static void imap_close(MillPlugIn *self) { self->host->FreeMem(self); }

static void put(MillRequest *r, const MillHost *h, const char *s) { h->Write(r->ref, s, strlen(s)); }

/* plugin://imap/about: the addresses, and a form */
static OSErr do_about(Imap1 *me, MillRequest *r)
{
    const MillHost *h = me->base.host;
    char host[HOST], esc[HOST * 6];
    if (h->GetSetting("imap", "server", host, sizeof host) < 0) host[0] = 0;
    h->HtmlEscape(host, esc, sizeof esc);
    h->PlugInPage(r->ref);
    put(r, h, "HTTP/1.0 200 OK" MILL_CRLF "Content-Type: text/html" MILL_CRLF MILL_CRLF
        "<HTML><HEAD><TITLE>" IMAP_TITLE "</TITLE></HEAD><BODY><H2>" IMAP_TITLE "</H2>\n"
        "<P>Address formats:<UL>"
        "<LI>imap://user@host/ for the mailboxes"
        "<LI>imap://user@host/mailbox"
        "<LI>imap://user@host/mailbox?search"
        "<LI>imap://user@host/mailbox/;UID=n"
        "</UL>Read-only: reading a message doesn&rsquo;t mark it read. Or use the form below.</P>\n"
        "<FORM ACTION=\"plugin://" IMAP_NAME "/go\"><P><B>imap</B> server "
        "<INPUT NAME=host SIZE=20 VALUE=\"");
    put(r, h, esc);
    put(r, h, "\"> user <INPUT NAME=user SIZE=12> <INPUT TYPE=SUBMIT VALUE=\"Open\"></P></FORM>\n</BODY></HTML>\n");
    return noErr;
}

/* Only what can go in a host (and a port after a colon), or a user name */
static void keep(char *s, const char *also)
{
    long i, o = 0;
    for (i = 0; s[i]; i++) {
        char c = s[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || strchr(also, c)) s[o++] = c;
    }
    s[o] = 0;
}

/* plugin://imap/go?host=server&user=name, from the about page's form: a redirect */
static OSErr do_go(Imap1 *me, MillRequest *r)
{
    const MillHost *h = me->base.host;
    const char *q = strchr(r->path, '?');
    char host[HOST], user[HOST], url[2 * HOST + 64];
    long args[3], ql = q ? (long)strlen(++q) : 0;
    if (h->From(r->ref) != millFromOwnPage) { h->Fail(r->ref, IMAP_TITLE ": Only its own page can send this."); return noErr; }
    if (!q || h->FormField(q, ql, "host", host, sizeof host) < 0) {
        h->Fail(r->ref, IMAP_TITLE ": The submitted data was invalid.");
        return noErr;
    }
    if (h->FormField(q, ql, "user", user, sizeof user) < 0) user[0] = 0;
    keep(host, ":");
    keep(user, "_+");
    if (!host[0]) { h->Fail(r->ref, IMAP_TITLE ": Enter a server name."); return noErr; }
    args[0] = (long)user;
    args[1] = (long)(user[0] ? "@" : "");
    args[2] = (long)host;
    h->Format(url, sizeof url, "HTTP/1.0 302 Found" MILL_CRLF "Location: imap://%s%s%s/" MILL_CRLF MILL_CRLF, args);
    put(r, h, url);
    return noErr;
}

static OSErr plugin_request(MillPlugIn *self, MillRequest *r)
{
    Imap1 *me = (Imap1 *)self;
    const MillHost *h = me->base.host;
    MillPlugIn *s;
    OSErr err;
    if (r->protocol < 0) {
        if (!strcmp(r->path, "/about")) return do_about(me, r);
        if (!strncmp(r->path, "/go?", 4)) return do_go(me, r);
        return millUnknown;
    }
    if (r->protocol != 0) return millUnknown;
    if (!(s = imap_state(h))) return memFullErr;
    err = imap_request(s, r);
    h->FreeMem(s);
    return err;
}

MillPlugIn *PlugInNew(const MillHost *host, short resFile, MillInfo *info)
{
    Imap1 *me = host->NewMem(sizeof *me);
    (void)resFile;
    if (!me) return 0;
    me->methods.close = imap_close;
    me->methods.request = plugin_request;
    me->methods.purge = 0;
    me->methods.format = 0;
    me->methods.font = 0;
    me->methods.gzOut = 0;
    me->base.methods = &me->methods;
    me->protocol = *imap_protocol();
    info->name = IMAP_NAME;
    info->version = "1.0";
    info->copyright = "Copyright 2026 OneInAMillion Software";
    info->by = "OneInAMillion Software";
    info->description = "Mail on IMAP servers.";
    info->flags = millHasAbout;
    info->protocolCount = 1;
    info->protocols = &me->protocol;
    info->formatCount = 0;
    info->formats = 0;
    return &me->base;
}
