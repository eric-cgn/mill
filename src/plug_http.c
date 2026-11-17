/* http: as a built-in Plug-In. Mill makes the request (cookies, the Referer) and reads
   the reply (redirects, content types, saving); this connects, to the site or to the
   HTTP proxy, sends it, and passes the reply back. Logins are this Plug-In's: it reads
   a 401's WWW-Authenticate (a 407's Proxy-Authenticate, from the proxy), answers with
   the name and password Mill gives it (Basic, or Digest), and otherwise passes the
   server's page back and calls AskLogin. It's also where other Plug-Ins' ProxyRequest
   goes (MillRequest.forward). For the proxy's exclusions it uses Mill's own address
   code (core/url.h), for logins core/httpauth.c, and for reading HTTP/1.1 replies
   core/http11.c, all in this Plug-In's segment.

   It asks in HTTP/1.1 (RFC 2068) and hands Mill the reply's head and its body, chunks
   undone; a body that stops before its end (short of its Content-Length or last chunk)
   is passed on as far as it came, cut off (EndedEarly). A Range in Mill's request is
   sent as it is; a 206 must be just that part, all of it, and a 200 to it means the
   server can't seek, so only its head is passed on.

   Connections are persistent (8.1): one whose reply ended where its Content-Length or
   last chunk said, with nothing after it, is kept for the window's next page there
   (TcpKeep). A reply that ends when the server closes, says Connection: close, or comes
   from an HTTP/1.0 server isn't kept, and neither is anything through the proxy. A GET
   or HEAD starts on the kept connection (TcpKept); Mill asks again on a new one if it
   had closed (8.1.4). A POST always has a new one. */
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
#include "core/http11.h"
#include "core/range.h"
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
    char head[4096];        /* the reply's first bytes: its head, which must fit */
    char start[600];
    char server[300];       /* "http://host:port" or "proxy host:port" */
    char keys[256];         /* AskLogin's: the realm's key, and the folder's for Basic (MillRequest.loginKey's size) */
    char name[64], pass[64];
    Side site, proxy;
    Challenge ch;
    HttpHead h;
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

/* How a body is passed on: as it comes, to the close; length bytes; or chunked */
typedef struct {
    int kind;
    long left;              /* HTTP_LENGTH: the bytes still to come */
    Chunked ck;
    int extra;              /* more came after its end: the connection isn't at rest */
} Body;

/* n bytes of body at buf to Mill: 1 for more, 0 at its end (or when Mill wants no
   more), -1 if its chunks are broken */
static int body_out(MillPlugIn *self, MillReqRef ref, Body *b, char *buf, long n)
{
    long used;
    if (b->kind == HTTP_LENGTH) {
        if (n > b->left) {
            n = b->left;        /* what's after it isn't this reply's */
            b->extra = 1;
        }
        b->left -= n;
    } else if (b->kind == HTTP_CHUNKED) {
        long all = n;
        if ((n = chunked_run(&b->ck, buf, n, &used)) < 0) return -1;
        if (used < all) b->extra = 1;
    } else if (b->kind != HTTP_CLOSE) {
        if (n > 0) b->extra = 1;
        return 0;
    }
    if (n > 0 && !H->Write(ref, buf, n)) return 0;
    return b->kind == HTTP_CLOSE || (b->kind == HTTP_LENGTH ? b->left > 0 : !chunked_done(&b->ck));
}

static OSErr http_request(MillPlugIn *self, MillRequest *r)
{
    Url via;
    const char *line_end = strstr(r->text, "\r\n"), *sp;
    long n, have, head, d, part, a[3], first, last;
    MillConn c;
    OSErr err;
    Work *w;
    short attempt = 0;
    unsigned long count = 0;
    short proxied = proxy_for(self, r->host, &via);
    int head_request = r->len > 5 && !memcmp(r->text, "HEAD ", 5);
    int safe = head_request || (r->len > 4 && !memcmp(r->text, "GET ", 4));
    int ranged = range_asked(r->text, r->len, &first, &last);
    char note[4];
    long nlen;
    if (r->forward && !proxied) return millNoProxy;
    if (!line_end || !(sp = strchr(r->text, ' ')) || ranged < 0) return paramErr;
    if (!(w = H->NewMem(sizeof *w))) return memFullErr;
    memset(w, 0, sizeof *w);
    /* the request line again, as a proxy is asked for the whole address */
    a[0] = sp - r->text;
    a[1] = (long)r->text;
    a[2] = (long)(proxied ? r->url : r->path);
    if ((n = H->Format(w->start, sizeof w->start, "%.*s %s HTTP/1.1\r\nAccept: */*", a)) >= (long)sizeof w->start) {
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
        const char *fail = NULL;
        Body b;
        int code, interim = 0, more = 0;
        nlen = sizeof note;
        if (proxied) err = H->TcpOpen(r->ref, via.host, url_port(&via), &c);
        else if (attempt > 0 || !safe || H->TcpKept(r->ref, "", &c, note, &nlen) != noErr)
            err = H->TcpOpen(r->ref, r->host, r->port ? r->port : 80, &c);
        if (err != noErr) break;
        /* a POST's body, as it's made (from the start again, if asked again) */
        if (attempt++ > 0) H->ReadBody(r->ref, NULL, NULL);
        {
            /* the request and the body's start in one send: MacTCP ends a send once it's
               acknowledged, so a second would wait a round trip */
            long so = strlen(w->site.out), po = strlen(w->proxy.out), rest = r->len - (line_end - r->text);
            long m = sizeof w->head;
            char *q;
            if ((err = H->ReadBody(r->ref, w->head, &m)) != noErr) m = 0;
            else if (!(q = H->NewMem(n + so + po + rest + m))) err = memFullErr;
            else {
                memcpy(q, w->start, n);
                memcpy(q + n, w->site.out, so);
                memcpy(q + n + so, w->proxy.out, po);
                memcpy(q + n + so + po, line_end, rest);
                memcpy(q + n + so + po + rest, w->head, m);
                err = H->TcpSend(c, q, n + so + po + rest + m);
                H->FreeMem(q);
            }
            while (err == noErr && m > 0) {
                m = sizeof w->head;
                if ((err = H->ReadBody(r->ref, w->head, &m)) != noErr || m == 0) break;
                err = H->TcpSend(c, w->head, m);
            }
        }
        if (err != noErr) { H->TcpClose(c); break; }
        H->Status(r->ref, "Waiting for reply\311");
        /* the head first, after any 1xx (RFC 2068 10.1: 100 Continue is skipped), to see
           how the body ends and if it asks for a login */
        for (have = head = 0;;) {
            long m = sizeof w->head - have;
            if (have > 0 && (head = http_head_end(w->head, have)) > 0) {
                http_head(w->head, head, &w->h);
                if (w->h.code > 199 || w->h.code < 100 || ++interim > 4) break;
                memmove(w->head, w->head + head, have -= head);
                head = 0;
                continue;
            }
            if (m == 0 || (err = H->TcpRecv(c, w->head + have, &m)) != noErr || m == 0) break;
            have += m;
        }
        if (err != noErr || (have == 0 && !interim)) { H->TcpClose(c); break; }
        memset(&b, 0, sizeof b);
        part = RANGE_PASS;
        b.kind = head ? http_body(&w->h, head_request) : HTTP_BAD;
        if (b.kind == HTTP_BAD || w->h.code < 200)
            fail = head || have < (long)sizeof w->head ? "The server\325s reply couldn\325t be read." : "The server\325s reply had too many headers.";
        /* a Range asked for: the part, or the whole (so the server can't seek: its head
           says so, and the rest isn't read) */
        else if ((part = http_range_reply(&w->h, ranged ? first : -1, last)) == RANGE_BAD) fail = "The server sent the wrong part.";
        else if (part == RANGE_WHOLE) b.kind = HTTP_NONE;
        if (fail) {
            H->TcpClose(c);
            H->Fail(r->ref, fail);
            break;
        }
        b.left = w->h.length;
        if (w->h.chunked) {
            /* Mill gets the body with its chunks undone (or none of it): no
               Transfer-Encoding, and no Content-Length, which chunked overrules (4.4) */
            d = http_drop(w->head, head, have, "Transfer-Encoding:");
            d += http_drop(w->head, head - d, have - d, "Content-Length:");
            head -= d;
            have -= d;
        }
        code = read_head(w->head, head, "WWW-Authenticate", &w->ch);
        if (code == 407 && proxied) code = read_head(w->head, head, "Proxy-Authenticate", &w->ch);
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
        /* the head, and the body to its end */
        if (H->Write(r->ref, w->head, head)) {
            for (more = body_out(self, r->ref, &b, w->head + head, have - head); more > 0;) {
                long m = sizeof w->head;
                if ((err = H->TcpRecv(c, w->head, &m)) != noErr) break;
                if (m == 0) { more = b.kind == HTTP_CLOSE ? 0 : -2; break; }
                more = body_out(self, r->ref, &b, w->head, m);
            }
            /* ended early: what came is shown, cut off; but a part must be all there */
            if (more == -1) fail = "The server\325s reply couldn\325t be read.";
            else if (more == -2 && part == RANGE_PART) fail = "The server\325s reply ended early.";
        }
        /* read to the end its head gave, and nothing more: the next page here may use it */
        if (err == noErr && !fail && !proxied && !w->h.closes && !b.extra && part != RANGE_WHOLE &&
            (b.kind == HTTP_NONE || (b.kind == HTTP_LENGTH && b.left == 0) ||
             (b.kind == HTTP_CHUNKED && chunked_done(&b.ck))))
            H->TcpKeep(r->ref, c, "", NULL, 0, NULL, 0);
        else H->TcpClose(c);
        if (fail) {
            H->Fail(r->ref, fail);
            err = noErr;
        } else if (asks) {
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
        } else if (more == -2) H->EndedEarly(r->ref);
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
      millText, "", 0, 0, NULL },
    kMillRandomAccessSetting("no")
};
static const MillProtocol protocols[] = { { "http", "HTTP", "Hypertext Transfer Protocol 1.1 (RFC 2068)", 4096, 4, settings } };

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
