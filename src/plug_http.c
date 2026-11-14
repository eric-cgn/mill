/* http: as a built-in Plug-In. Mill makes the request (cookies, the Referer) and reads
   the reply (redirects, content types, saving); this connects, to the site or to the
   HTTP proxy, sends it, and passes the reply back. Logins are this Plug-In's: it reads
   a 401's WWW-Authenticate (a 407's Proxy-Authenticate, from the proxy), answers with
   the name and password Mill gives it (Basic, or Digest), and otherwise passes the
   server's page back and calls AskLogin. It's also where other Plug-Ins' ProxyRequest
   goes (MillRequest.forward). For the proxy's exclusions it uses Mill's own address
   code (core/url.h), and for logins core/httpauth.c, in this Plug-In's segment. */
#include <Multiverse.h>
#include <stdio.h>
#include <string.h>

#define MILL_BUILTIN
#define MILL_ENTRY http_main
#define MILL_NEW http_new
#define MILL_CLOSE http_close
#define MILL_REQUEST http_request
#include "../sdk/Glue/MillGlue.c"
#include "core/url.h"
#include "core/httpauth.h"
#include "version.h"

#define H (self->host)

/* The proxy, if it's on, set, and the host isn't one of its exclusions */
static short proxy_for(MillPlugIn *self, const char *host, Url *via)
{
    char v[256];
    if (H->GetSetting("http", "proxy_on", v, sizeof v) != 3 || strcmp(v, "yes")) return 0;
    if (H->GetSetting("http", "proxy", v, sizeof v) <= 0 || url_parse(v, via) || via->scheme != SCHEME_HTTP) return 0;
    if (H->GetSetting("http", "proxy_exclude", v, sizeof v) > 0 && url_host_in(host, v)) return 0;
    return 1;
}

static int same_ci(const char *a, const char *b, long n)
{
    for (; n > 0; n--, a++, b++) if ((*a | 32) != (*b | 32)) return 0;
    return 1;
}

/* s onto out (size bytes, kept ended) */
static void add(char *out, int size, const char *s)
{
    int n = strlen(out);
    while (*s && n < size - 1) out[n++] = *s++;
    out[n] = 0;
}

/* The reply's status, and in ch what its header named name (WWW-Authenticate or
   Proxy-Authenticate) asks for; head is len bytes, through the blank line */
static int read_head(const char *head, long len, const char *name, Challenge *ch)
{
    long i, l, nl = strlen(name);
    int code = len > 12 && !memcmp(head, "HTTP/", 5) ? (head[9] - '0') * 100 + (head[10] - '0') * 10 + head[11] - '0' : 0;
    memset(ch, 0, sizeof *ch);
    for (l = 0, i = 0; i < len; i++) {
        if (head[i] != '\n') continue;
        if (i - l > nl && head[l + nl] == ':' && same_ci(head + l, name, nl)) {
            long e = i;
            if (e > l && head[e - 1] == '\r') e--;
            auth_challenge(head + l + nl + 1, e - l - nl - 1, ch);
        }
        l = i + 1;
    }
    return code;
}

/* One side's login (the site's, or the proxy's): what was sent, and how many times */
typedef struct {
    char name[64], pass[64];
    short tries;
    char out[700];          /* "\r\nAuthorization: ...", or "" */
} Side;

typedef struct {
    char head[2048];        /* the reply's first bytes: its head, if it fits */
    char start[600];
    char server[300];       /* "http://host:port" or "proxy host:port" */
    char keys[256];         /* AskLogin's: the realm's key, and the folder's for Basic (MillRequest.loginKey's size) */
    char name[64], pass[64];
    Side site, proxy;
    Challenge ch;
} Work;

/* The keys a login is kept under (MillPlugIn.h's loginKey). The realm's, which a 401
   or 407 names: http://www.example.com:80 "Members", or proxy proxy.lab:8080 "Squid".
   With Basic, also one for where it worked, to send it there before being asked:
   the folder for a site (RFC 2617: paths below the one asked for are likely the same
   realm), http://www.example.com:80/members/ Basic; for a proxy, every request through
   it, proxy proxy.lab:8080 Basic. */
static void realm_key(char *out, int size, const char *server, const char *realm)
{
    out[0] = 0;
    add(out, size, server);
    add(out, size, " \"");
    add(out, size, realm);
    add(out, size, "\"");
}

static void basic_key(char *out, int size, const char *server, const char *path, long folder)
{
    int n;
    out[0] = 0;
    add(out, size, server);
    n = strlen(out);
    while (folder-- > 0 && n < size - 1) out[n++] = *path++;
    out[n] = 0;
    add(out, size, " Basic");
}

/* The folder of a path (to and including its last slash, before any query): its length */
static long folder_len(const char *path)
{
    long e = strcspn(path, "?#");
    while (e > 0 && path[e - 1] != '/') e--;
    return e;
}

/* A login kept for Basic where this request goes: the site's (this folder's or one
   above it), or the proxy's, made into its header line */
static void up_front(MillPlugIn *self, Work *w, Side *side, const char *path, const char *field)
{
    long f = path ? folder_len(path) : 0;
    char key[400];
    for (;;) {
        basic_key(key, sizeof key, w->server, path, f);
        if (H->SavedLogin(key, side->name, sizeof side->name, side->pass, sizeof side->pass)) break;
        if (!path || f <= 1) return;
        for (f--; f > 0 && path[f - 1] != '/'; f--) {}
    }
    w->ch.scheme = AUTH_BASIC;
    strcpy(side->out, field);
    if (auth_header(&w->ch, side->name, side->pass, 1, "", "", "", side->out + strlen(field), sizeof side->out - strlen(field)))
        side->tries = 1;
    else side->out[0] = 0;
}

static OSErr http_request(MillPlugIn *self, MillRequest *r)
{
    Url via;
    const char *line_end = strstr(r->text, "\r\n"), *sp;
    long n, have, a[3];
    MillConn c;
    OSErr err;
    Work *w;
    short attempt = 0;
    unsigned long count = 0;
    short proxied = proxy_for(self, r->host, &via);
    if (r->forward && !proxied) return millNoProxy;
    if (!line_end || !(sp = strchr(r->text, ' '))) return paramErr;
    if (!(w = H->NewMem(sizeof *w))) return memFullErr;
    memset(w, 0, sizeof *w);
    /* the request line again, as a proxy is asked for the whole address */
    a[0] = sp - r->text;
    a[1] = (long)r->text;
    a[2] = (long)(proxied ? r->url : r->path);
    if ((n = H->Format(w->start, sizeof w->start, "%.*s %s HTTP/1.0\r\nAccept: */*", a)) >= (long)sizeof w->start) {
        H->FreeMem(w);
        return paramErr;
    }
    /* logins kept for Basic: sent without waiting to be asked */
    a[0] = (long)r->scheme;
    a[1] = (long)r->host;
    a[2] = r->port ? r->port : !strcmp(r->scheme, "https") ? 443 : 80;
    H->Format(w->server, sizeof w->server, "%s://%s:%d", a);
    up_front(self, w, &w->site, r->path, "\r\nAuthorization: ");
    if (proxied) {
        a[0] = (long)via.host;
        a[1] = url_port(&via);
        H->Format(w->server, sizeof w->server, "proxy %s:%d", a);
        up_front(self, w, &w->proxy, NULL, "\r\nProxy-Authorization: ");
    }
    for (;;) {
        Boolean asks = false;
        int code;
        if (proxied) err = H->TcpOpen(r->ref, via.host, url_port(&via), &c);
        else err = H->TcpOpen(r->ref, r->host, r->port ? r->port : 80, &c);
        if (err != noErr) break;
        err = H->TcpSend(c, w->start, n);
        if (err == noErr && w->site.out[0]) err = H->TcpSend(c, w->site.out, strlen(w->site.out));
        if (err == noErr && w->proxy.out[0]) err = H->TcpSend(c, w->proxy.out, strlen(w->proxy.out));
        if (err == noErr) err = H->TcpSend(c, line_end, r->len - (line_end - r->text));
        /* a POST's body, as it's made (from the start again, if asked again) */
        if (attempt++ > 0) H->ReadBody(r->ref, NULL, NULL);
        while (err == noErr) {
            long m = sizeof w->head;
            if ((err = H->ReadBody(r->ref, w->head, &m)) != noErr || m == 0) break;
            err = H->TcpSend(c, w->head, m);
        }
        if (err != noErr) { H->TcpClose(c); break; }
        H->Status(r->ref, "Waiting for reply...");
        /* the head first, to see if it asks for a login */
        for (have = 0; have < (long)sizeof w->head;) {
            long m = sizeof w->head - have, k;
            if ((err = H->TcpRecv(c, w->head + have, &m)) != noErr || m == 0) break;
            for (k = have > 3 ? have - 3 : 0, have += m; k + 3 < have; k++)
                if (!memcmp(w->head + k, "\r\n\r\n", 4)) break;
            if (k + 3 < have) break;
        }
        code = read_head(w->head, have, "WWW-Authenticate", &w->ch);
        if (code == 407 && proxied) code = read_head(w->head, have, "Proxy-Authenticate", &w->ch);
        if ((code == 401 || (code == 407 && proxied)) && w->ch.scheme != AUTH_NONE) {
            Side *side = code == 407 ? &w->proxy : &w->site;
            const char *field = code == 407 ? "\r\nProxy-Authorization: " : "\r\nAuthorization: ";
            Boolean address = false, have_login;
            char key[400];
            /* whose login: the server's (a 401) or the proxy's (a 407), and the keys for it */
            if (code == 407) {
                a[0] = (long)via.host;
                a[1] = url_port(&via);
                H->Format(w->server, sizeof w->server, "proxy %s:%d", a);
            } else {
                a[0] = (long)r->scheme;
                a[1] = (long)r->host;
                a[2] = r->port ? r->port : !strcmp(r->scheme, "https") ? 443 : 80;
                H->Format(w->server, sizeof w->server, "%s://%s:%d", a);
            }
            realm_key(w->keys, sizeof w->keys, w->server, w->ch.realm);
            if (w->ch.scheme == AUTH_BASIC) {
                basic_key(key, sizeof key, w->server, r->path, code == 407 ? 0 : folder_len(r->path));
                add(w->keys, sizeof w->keys, "\n");
                add(w->keys, sizeof w->keys, key);
            }
            /* a login for it: the address's (a site's, tried first), else one kept or typed
               for the realm */
            realm_key(key, sizeof key, w->server, w->ch.realm);
            if (code == 401 && r->user[0] && side->tries == 0) {
                snprintf(w->name, sizeof w->name, "%s", r->user);
                snprintf(w->pass, sizeof w->pass, "%s", r->password);
                have_login = address = true;
            } else {
                have_login = H->SavedLogin(key, w->name, sizeof w->name, w->pass, sizeof w->pass) != 0;
            }
            /* sent unless it's what was just refused (once more if Digest's nonce went stale) */
            if (have_login && side->tries < 3 &&
                (side->tries == 0 || strcmp(w->name, side->name) || strcmp(w->pass, side->pass) ||
                 (side->tries == 1 && w->ch.stale))) {
                char cnonce[12];
                long t = TickCount(), f = strlen(field);
                int k;
                for (k = 0; k < 8; k++) cnonce[k] = "0123456789abcdef"[(t >> (k * 4)) & 15];
                cnonce[8] = 0;
                strcpy(side->name, w->name);
                strcpy(side->pass, w->pass);
                strcpy(side->out, field);
                memcpy(w->head, r->text, sp - r->text);
                w->head[sp - r->text] = 0;
                /* Digest's uri is the request line's: the whole address, to a proxy */
                if (!auth_header(&w->ch, side->name, side->pass, ++count, w->head, proxied ? r->url : r->path, cnonce,
                                 side->out + f, sizeof side->out - f))
                    side->out[0] = 0;
                H->TcpClose(c);
                if (!side->out[0]) break;
                side->tries++;
                /* the address's login: Mill keeps it for these keys if it works */
                if (address) snprintf(r->loginKey, sizeof r->loginKey, "%s", w->keys);
                else if (code == 401) r->loginKey[0] = 0;
                continue;
            }
            asks = true;
        }
        /* the reply as it came, and the rest of it */
        if (have > 0 && H->Write(r->ref, w->head, have)) {
            for (;;) {
                n = sizeof w->head;
                if ((err = H->TcpRecv(c, w->head, &n)) != noErr || n == 0) break;
                if (!H->Write(r->ref, w->head, n)) break;
            }
        }
        H->TcpClose(c);
        if (asks) {
            /* a name and password it didn't have, or that didn't work: Mill asks.
               "Log in to www.example.com (Members)." */
            Side *side = code == 407 ? &w->proxy : &w->site;
            char prompt[300];
            strcpy(prompt, side->tries ? "The name or password for " : "Log in to ");
            if (code == 407) add(prompt, sizeof prompt, "the proxy ");
            add(prompt, sizeof prompt, code == 407 ? via.host : r->host);
            if (w->ch.realm[0]) {
                add(prompt, sizeof prompt, " (");
                add(prompt, sizeof prompt, w->ch.realm);
                add(prompt, sizeof prompt, ")");
            }
            add(prompt, sizeof prompt, side->tries ? " was wrong. Try again?" : ".");
            r->loginKey[0] = 0;
            H->AskLogin(r->ref, prompt, NULL, NULL, w->keys, side->tries ? side->name : code == 401 ? r->user : NULL, NULL);
            err = noErr;
        }
        break;
    }
    memset(w, 0, sizeof *w);    /* the logins */
    H->FreeMem(w);
    return err;
}

static void http_close(MillPlugIn *self) { self->host->FreeMem(self); }

static const MillSetting settings[] = {
    { "proxy_on", "Enable proxy", NULL, millCheckbox, "no", 0, 0, NULL },
    { "proxy", "HTTP proxy", "A proxy server, as host:port.", millText, "", 0, 0, NULL },
    { "proxy_exclude", "Proxy excluded domains",
      "Domains (with their subdomains), IP addresses, or ranges such as 192.168.0.0/16, separated by spaces.",
      millText, "", 0, 0, NULL }
};
static const MillProtocol protocols[] = { { "http", "HTTP", "Hypertext Transfer Protocol 1.0 (RFC 1945)", 3, settings } };

MillPlugIn *http_new(const MillHost *host, short resFile, MillInfo *info)
{
    MillPlugIn *me = host->NewMem(sizeof *me);
    (void)resFile;
    if (!me) return NULL;
    me->methods = NULL;     /* a built-in's are called by name (MillGlue.c) */
    info->name = "http";
    info->version = "1.0";
    info->copyright = "\251 " MILL_YEAR " " MILL_PUBLISHER;
    info->by = MILL_PUBLISHER;
    info->description = "The web, directly or through a proxy.";
    info->protocolCount = 1;
    info->protocols = protocols;
    return me;
}
