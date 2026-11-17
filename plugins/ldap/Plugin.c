/* LDAP: a Plug-In for Mill for directories.
     ldap://host/dn?attributes?scope?filter     LDAP (ldap.c)
   plugin://ldap/about explains the addresses and has a search form, filled in with the
   Default server and Default base settings. */
#include <string.h>

#include "MillPlugIn.h"
#include "ldap.h"
#include "ldaputil.h"

enum { HOST = 256 };

typedef struct {
    MillPlugIn base;
    MillMethods methods;        /* filled in by PlugInNew (see MillPlugIn.h's rules) */
    MillProtocol protocol;
} Ldap1;

static void ldap_close(MillPlugIn *self) { self->host->FreeMem(self); }

static void put(MillRequest *r, const MillHost *h, const char *s) { h->Write(r->ref, s, strlen(s)); }

/* plugin://ldap/about: the addresses, and the form */
static OSErr do_about(Ldap1 *me, MillRequest *r)
{
    const MillHost *h = me->base.host;
    char host[HOST], base[HOST];
    if (h->GetSetting("ldap", "server", host, sizeof host) < 0) host[0] = 0;
    if (h->GetSetting("ldap", "base", base, sizeof base) < 0) base[0] = 0;
    h->PlugInPage(r->ref);
    put(r, h, "HTTP/1.0 200 OK" MILL_CRLF "Content-Type: text/html" MILL_CRLF MILL_CRLF
        "<HTML><HEAD><TITLE>" LDAP_TITLE "</TITLE></HEAD><BODY><H2>" LDAP_TITLE "</H2>\n"
        "<P>Address formats:<UL>"
        "<LI>ldap://host/ for its directories"
        "<LI>ldap://host/dn for an entry"
        "<LI>ldap://host/dn?attributes?scope?filter"
        "</UL>Scope: base, one, or sub. Read-only. Or use the form below.</P>\n");
    ldap_form(h, r->ref, host, base);
    put(r, h, "</BODY></HTML>\n");
    return noErr;
}

/* Only what can go in a host (and a port after a colon) */
static void keep_host(char *s)
{
    long i, o = 0;
    for (i = 0; s[i]; i++) {
        char c = s[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == ':') s[o++] = c;
    }
    s[o] = 0;
}

/* plugin://ldap/go?host=&base=&name=&filter=&layout=, from the form: a redirect to the
   search (a Name makes the filter) */
static OSErr do_go(Ldap1 *me, MillRequest *r)
{
    const MillHost *h = me->base.host;
    const char *q = strchr(r->path, '?');
    char host[HOST], base[HOST], name[HOST], filter[HOST], layout[16], eb[HOST * 3], ef[HOST * 3], url[HOST * 7];
    long args[4], ql = q ? (long)strlen(++q) : 0;
    if (h->From(r->ref) != millFromOwnPage) { h->Fail(r->ref, LDAP_TITLE ": Only its own page can send this."); return noErr; }
    if (!q || h->FormField(q, ql, "host", host, sizeof host) < 0) {
        h->Fail(r->ref, LDAP_TITLE ": The submitted data was invalid.");
        return noErr;
    }
    if (h->FormField(q, ql, "base", base, sizeof base) < 0) base[0] = 0;
    if (h->FormField(q, ql, "name", name, sizeof name) < 0) name[0] = 0;
    if (h->FormField(q, ql, "filter", filter, sizeof filter) < 0) filter[0] = 0;
    if (h->FormField(q, ql, "layout", layout, sizeof layout) < 0) layout[0] = 0;
    keep_host(host);
    if (!host[0]) { h->Fail(r->ref, LDAP_TITLE ": Enter a server name."); return noErr; }
    if (name[0]) ldap_name_filter(name, filter, sizeof filter);
    if (ldap_escape(base, eb, sizeof eb) < 0 || ldap_escape(filter, ef, sizeof ef) < 0) {
        h->Fail(r->ref, LDAP_TITLE ": The submitted data was invalid.");
        return noErr;
    }
    args[0] = (long)host;
    args[1] = (long)eb;
    args[2] = (long)ef;
    args[3] = (long)(!strcmp(layout, "Table") ? "?x-layout=table" : "");
    h->Format(url, sizeof url, "HTTP/1.0 302 Found" MILL_CRLF "Location: ldap://%s/%s??sub?%s%s" MILL_CRLF MILL_CRLF, args);
    put(r, h, url);
    return noErr;
}

static OSErr plugin_request(MillPlugIn *self, MillRequest *r)
{
    Ldap1 *me = (Ldap1 *)self;
    const MillHost *h = me->base.host;
    MillPlugIn *s;
    OSErr err;
    if (r->protocol < 0) {
        if (!strcmp(r->path, "/about")) return do_about(me, r);
        if (!strncmp(r->path, "/go?", 4)) return do_go(me, r);
        return millUnknown;
    }
    if (r->protocol != 0) return millUnknown;
    if (!(s = ldap_state(h))) return memFullErr;
    err = ldap_request(s, r);
    h->FreeMem(s);
    return err;
}

MillPlugIn *PlugInNew(const MillHost *host, short resFile, MillInfo *info)
{
    Ldap1 *me = host->NewMem(sizeof *me);
    (void)resFile;
    if (!me) return 0;
    me->methods.close = ldap_close;
    me->methods.request = plugin_request;
    me->methods.purge = 0;
    me->methods.format = 0;
    me->methods.font = 0;
    me->methods.gzOut = 0;
    me->base.methods = &me->methods;
    me->protocol = *ldap_protocol();
    info->name = LDAP_NAME;
    info->version = "1.0";
    info->copyright = "Copyright 2026 OneInAMillion Software";
    info->by = "OneInAMillion Software";
    info->description = "Directory searches (LDAP).";
    info->flags = millHasAbout;
    info->protocolCount = 1;
    info->protocols = &me->protocol;
    info->formatCount = 0;
    info->formats = 0;
    return &me->base;
}
