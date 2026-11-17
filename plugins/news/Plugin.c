/* News: a Plug-In for Mill for Usenet news.
     news://host/group          NNTP (news.c); nntp:// is the same
   plugin://news/about explains the addresses and has a form, filled in with the Default
   server setting. */
#include <string.h>

#include "MillPlugIn.h"
#include "news.h"

enum { HOST = 256 };

typedef struct {
    MillPlugIn base;
    MillMethods methods;        /* filled in by PlugInNew (see MillPlugIn.h's rules) */
    MillProtocol protocols[2];
} News1;

static void news_close(MillPlugIn *self) { self->host->FreeMem(self); }

static void put(MillRequest *r, const MillHost *h, const char *s) { h->Write(r->ref, s, strlen(s)); }

/* plugin://news/about: the addresses, and a form */
static OSErr do_about(News1 *me, MillRequest *r)
{
    const MillHost *h = me->base.host;
    char host[HOST], esc[HOST * 6];
    if (h->GetSetting("news", "server", host, sizeof host) < 0) host[0] = 0;
    h->HtmlEscape(host, esc, sizeof esc);
    h->PlugInPage(r->ref);
    put(r, h, "HTTP/1.0 200 OK" MILL_CRLF "Content-Type: text/html" MILL_CRLF MILL_CRLF
        "<HTML><HEAD><TITLE>" NEWS_TITLE "</TITLE></HEAD><BODY><H2>" NEWS_TITLE "</H2>\n"
        "<P>Address formats:<UL>"
        "<LI>news://host/ for its groups"
        "<LI>news://host/group"
        "<LI>news://host/group/number"
        "<LI>news://host/message-id"
        "</UL>nntp:// addresses work the same way. Read-only. Or use the form below.</P>\n"
        "<FORM ACTION=\"plugin://" NEWS_NAME "/go\"><P><B>news</B> server "
        "<INPUT NAME=host SIZE=20 VALUE=\"");
    put(r, h, esc);
    put(r, h, "\"> group <INPUT NAME=group SIZE=16> <INPUT TYPE=SUBMIT VALUE=\"Open\"></P></FORM>\n</BODY></HTML>\n");
    return noErr;
}

/* Only what can go in a host (and a port after a colon), or a group name */
static void keep(char *s, const char *also)
{
    long i, o = 0;
    for (i = 0; s[i]; i++) {
        char c = s[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || strchr(also, c)) s[o++] = c;
    }
    s[o] = 0;
}

/* plugin://news/go?host=server&group=name, from the about page's form: a redirect */
static OSErr do_go(News1 *me, MillRequest *r)
{
    const MillHost *h = me->base.host;
    const char *q = strchr(r->path, '?');
    char host[HOST], group[HOST], url[2 * HOST + 64];
    long args[2], ql = q ? (long)strlen(++q) : 0;
    if (h->From(r->ref) != millFromOwnPage) { h->Fail(r->ref, NEWS_TITLE ": Only its own page can send this."); return noErr; }
    if (!q || h->FormField(q, ql, "host", host, sizeof host) < 0) {
        h->Fail(r->ref, NEWS_TITLE ": The submitted data was invalid.");
        return noErr;
    }
    if (h->FormField(q, ql, "group", group, sizeof group) < 0) group[0] = 0;
    keep(host, ":");
    keep(group, "_+");
    if (!host[0]) { h->Fail(r->ref, NEWS_TITLE ": Enter a server name."); return noErr; }
    args[0] = (long)host;
    args[1] = (long)group;
    h->Format(url, sizeof url, "HTTP/1.0 302 Found" MILL_CRLF "Location: news://%s/%s" MILL_CRLF MILL_CRLF, args);
    put(r, h, url);
    return noErr;
}

static OSErr plugin_request(MillPlugIn *self, MillRequest *r)
{
    News1 *me = (News1 *)self;
    const MillHost *h = me->base.host;
    MillPlugIn *s;
    OSErr err;
    if (r->protocol < 0) {
        if (!strcmp(r->path, "/about")) return do_about(me, r);
        if (!strncmp(r->path, "/go?", 4)) return do_go(me, r);
        return millUnknown;
    }
    if (r->protocol > 1) return millUnknown;
    if (!(s = news_state(h))) return memFullErr;
    err = news_request(s, r);
    h->FreeMem(s);
    return err;
}

MillPlugIn *PlugInNew(const MillHost *host, short resFile, MillInfo *info)
{
    News1 *me = host->NewMem(sizeof *me);
    (void)resFile;
    if (!me) return 0;
    me->methods.close = news_close;
    me->methods.request = plugin_request;
    me->methods.purge = 0;
    me->methods.format = 0;
    me->methods.font = 0;
    me->methods.gzOut = 0;
    me->base.methods = &me->methods;
    me->protocols[0] = news_protocols()[0];
    me->protocols[1] = news_protocols()[1];
    info->name = NEWS_NAME;
    info->version = "1.0";
    info->copyright = "Copyright 2026 OneInAMillion Software";
    info->by = "OneInAMillion Software";
    info->description = "Usenet news (NNTP).";
    info->flags = millHasAbout;
    info->protocolCount = 2;
    info->protocols = me->protocols;
    info->formatCount = 0;
    info->formats = 0;
    return &me->base;
}
