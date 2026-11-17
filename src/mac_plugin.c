/* Plug-Ins: finding them, loading them only while they're used, and the host
   functions they call (sdk/Interfaces/MillPlugIn.h). */
#include <Multiverse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/auth.h"
#include "core/charset.h"
#include "core/filetypes.h"
#include "core/fmt.h"
#include "core/inflate.h"
#include "core/range.h"
#include "mac_local.h"
#include "mac_plugin.h"
#include "mac_sys.h"
#include "mac_upload.h"
#include "mac_memreport.h"

/* The built-ins, each in a code segment of its own (mill.segmap) */
pascal OSErr http_main(short selector, void *params, void **self);
pascal OSErr https_main(short selector, void *params, void **self);
pascal OSErr file_main(short selector, void *params, void **self);
pascal OSErr gif_main(short selector, void *params, void **self);
pascal OSErr jpeg_main(short selector, void *params, void **self);

Plug *plugs;
short nplugs;
Plug font_plug;
PlugFont plug_fonts[PLUG_FONTS];
short nplug_fonts;
static MillHost host;
static short app_res;           /* Mill's own resources */

/* ------------------------------------------------------------ calling */

/* Calls go straight to a built-in, so they pass through the jump table, which loads
   its segment */
static OSErr call(Plug *p, short selector, void *params)
{
    short was = CurResFile();
    OSErr err = millUnknown;
    /* its own resources come first, then Mill's, then the System's */
    UseResFile(p->res > 0 ? p->res : app_res);
    switch (p->builtin) {
    case 1: err = http_main(selector, params, &p->self); break;
    case 2: err = https_main(selector, params, &p->self); break;
    case 3: err = file_main(selector, params, &p->self); break;
    case 4: err = gif_main(selector, params, &p->self); break;
    case 5: err = jpeg_main(selector, params, &p->self); break;
    default: if (p->code) err = ((MillMainProc)*p->code)(selector, params, &p->self);
    }
    UseResFile(was);
    return err;
}

static void unload(Plug *p)
{
    switch (p->builtin) {
    case 1: UnloadSeg((void *)http_main); break;
    case 2: UnloadSeg((void *)https_main); break;
    case 3: UnloadSeg((void *)file_main); break;
    case 4: UnloadSeg((void *)gif_main); break;
    case 5: UnloadSeg((void *)jpeg_main); break;
    }
    if (p->code) { HUnlock(p->code); ReleaseResource(p->code); p->code = NULL; }
    if (p->res > 0 && !p->shared) { CloseResFile(p->res); p->res = 0; }
}

/* Load it and open it: false if it can't start */
Boolean plugin_load(Plug *p, MillOpenParams *op)
{
    memset(op, 0, sizeof *op);
    op->interface = kMillInterface;
    op->host = &host;
    if (!p->builtin) {
        short was = CurResFile();
        Handle h;
        if (!p->shared && (p->res = file_open_res(&p->spec, fsRdPerm)) == -1) { p->res = 0; return false; }
        UseResFile(p->res);
        /* the interface it was built for ('MIfc' 128), before any of its code is loaded:
           another one's glue would run against this Mill's tables. Without it, it's older;
           without code as well, it's no Plug-In (op->interface stays ours: it failed). */
        if ((h = Get1Resource('MIfc', 128)) != NULL) {
            op->interface = GetHandleSize(h) >= 2 ? *(short *)*h : 0;
            ReleaseResource(h);
        } else if (Count1Resources('MPlg') > 0) op->interface = 0;
        p->code = op->interface == kMillInterface ? Get1IndResource('MPlg', 1) : NULL;
        UseResFile(was);
        if (!p->code) { unload(p); return false; }
        HNoPurge(p->code);
        MoveHHi(p->code);
        HLock(p->code);
        op->resFile = p->res;
    }
    p->self = NULL;
    if (call(p, millOpen, op) != noErr) { unload(p); return false; }
    return true;
}

void plugin_close(Plug *p)
{
    call(p, millClose, NULL);
    p->self = NULL;
    p->loaded = false;
    unload(p);
}

static Boolean use(Plug *p)
{
    MillOpenParams op;
    if (p->status != PLUG_READY) return false;
    if (!p->loaded) {
        if (!plugin_load(p, &op)) return false;
        p->loaded = true;
    }
    p->users++;
    return true;
}

static void done_with(Plug *p)
{
    if (p && p->users > 0 && --p->users == 0 && p->loaded) plugin_close(p);
}

/* ------------------------------------------------------------ what each one is */

void plugins_quit(void)
{
    short i;
    for (i = 0; i < nplugs; i++) if (plugs[i].loaded) plugin_close(&plugs[i]);
    if (font_plug.loaded) plugin_close(&font_plug);
}

OSErr plugin_font(MillFontParams *p)
{
    MillOpenParams op;
    if (font_plug.status != PLUG_READY || !nplug_fonts) return fnfErr;
    if (!font_plug.loaded) {
        if (!plugin_load(&font_plug, &op)) return memFullErr;
        font_plug.loaded = true;
    }
    return call(&font_plug, millFont, p);
}

PlugProto *plugin_proto(const char *scheme)
{
    short i, k;
    for (i = 0; i < nplugs; i++) {
        if (plugs[i].status != PLUG_READY) continue;
        for (k = 0; k < plugs[i].nproto; k++) if (!strcmp(plugs[i].protos[k].scheme, scheme)) return &plugs[i].protos[k];
    }
    return NULL;
}

static Boolean any_enabled(const Plug *p);

/* The Plug-In that answers u, and which of its protocols (-1: plugin://NAME/) */
static Plug *route(const Url *u, short *protocol)
{
    PlugProto *pp;
    short i;
    if (u->scheme == SCHEME_OTHER && !strcmp(u->name, "plugin")) {
        *protocol = -1;
        for (i = 0; i < nplugs; i++) if (plugs[i].status == PLUG_READY && !strcmp(plugs[i].name, u->host)) return &plugs[i];
        return NULL;
    }
    pp = plugin_proto(u->scheme == SCHEME_FILE ? "file" : u->scheme == SCHEME_HTTP ? "http" :
                      u->scheme == SCHEME_HTTPS ? "https" : u->scheme == SCHEME_OTHER ? u->name : "");
    if (!pp) return NULL;
    *protocol = (short)(pp - pp->plug->protos);
    return pp->plug;
}

Plug *plugin_hold(const char *url)
{
    Url u;
    Plug *p;
    short k;
    if (url_parse(url, &u) || !(p = route(&u, &k)) || (k >= 0 ? !plugin_enabled(p->protos[k].scheme) : !any_enabled(p)) ||
        !use(p)) return NULL;
    return p;
}

void plugin_drop(Plug *p) { done_with(p); }

Boolean plugins_purge(long wanted)
{
    short i;
    long freed = 0;
    for (i = 0; i < nplugs && freed < wanted; i++) {
        MillPurgeParams mp;
        if (!plugs[i].loaded) continue;
        mp.wanted = wanted - freed;
        mp.freed = 0;
        if (call(&plugs[i], millPurge, &mp) == noErr) freed += mp.freed;
    }
    /* the font Plug-In: its letters, and then its code, loaded again when text needs it */
    if (freed < wanted && font_plug.loaded) {
        MillPurgeParams mp;
        mp.wanted = wanted - freed;
        mp.freed = 0;
        if (call(&font_plug, millPurge, &mp) == noErr) freed += mp.freed;
        if (freed < wanted) {
            freed += GetHandleSize(font_plug.code);
            plugin_close(&font_plug);
        }
    }
    return freed > 0;
}

/* ------------------------------------------------------------ requests */

#define CONNS 4

struct MillRequestRef {
    short kind;                 /* 0: a request (a format's run is 1, and shares Write) */
    net_sink sink;
    void *ref;
    Boolean stopped;
    Plug *plug;
    const Url *u;
    const char *url, *headers, *content_type;
    Body *body;                 /* a POST's, made as it's read (ReadBody, NextPart) */
    const char *text;
    const char *from_owner;     /* who marked the page that asked */
    Boolean marked;             /* PlugInPage: this reply is the Plug-In's own page */
    Boolean proxied;
    Boolean wrote;              /* some of a response has gone */
    Boolean failed;             /* Fail: the request failed, and Mill says why */
    Boolean asked;              /* AskLogin: it needs a name and password */
    Boolean early;              /* EndedEarly: what was written is cut off */
    MillRequest *req;           /* the Plug-In's view, for ProxyRequest to pass on the login */
    NetConn *conns[CONNS];
    /* Kept connections (TcpKept, TcpKeep): the window asking, and whether for its page
       (else an image or a file to save, which only borrows the window's kept connection) */
    const void *window;
    Boolean page;
    Boolean cut;                /* stopped partway (Stop, or Mill wanted no more): nothing is kept */
    Boolean safe;               /* a GET: may be asked again (a POST never is) */
    Boolean stale;              /* the kept connection failed before it answered: ask again */
    NetConn *reused;            /* the kept connection TcpKept gave, and what came on it */
    long reused_got;
    char server[160];           /* the key's start: "ftp://host:0" */
    NetConn *keep;              /* TcpKeep's: kept when the request ends well */
    char *keep_key;             /* its key, note, and bye, as net_keep_put takes them */
    long keep_nlen, keep_blen;
};

static MillReqRef current;     /* the request running now (Plug-Ins run one at a time) */

Boolean plugin_enabled(const char *scheme)
{
    char name[48];
    sprintf(name, "%.15s.enable", scheme);
    return strcmp(setting(name, "yes"), "no") != 0;
}

/* Whether any of a Plug-In's protocols is on */
static Boolean any_enabled(const Plug *p)
{
    short k;
    for (k = 0; k < p->nproto; k++) if (plugin_enabled(p->protos[k].scheme)) return true;
    return false;
}

/* One request to Plug-In p's protocol (forward: for another Plug-In's ProxyRequest) */
/* ------------------------------------------------------------ logins */

/* A login typed for a key, offered by SavedLogin until the request it was typed for
   ends; which kept logins this request was given (a key each), and the last given */
Keyring keyring;
static Keyring typed;
static unsigned long tidy_until;    /* see tidy() */
static char given[4][AUTH_KEYS];
static short given_n;
static char used_key[AUTH_KEYS];

void plugin_login_typed(const char *keys, const char *user, const char *pass)
{
    auth_clear(&typed);
    if (keys) auth_save(&typed, keys, user, pass);
}

const char *plugin_login_used(void) { return used_key; }

static pascal short h_saved_login(const char *key, char *name, long name_size, char *pass, long pass_size)
{
    Cred *c = auth_find(&typed, key);
    if (!c && (c = auth_find(&keyring, key)) != NULL && given_n < 4) snprintf(given[given_n++], AUTH_KEYS, "%s", key);
    if (!c) return 0;
    snprintf(used_key, sizeof used_key, "%s", key);
    if (name) snprintf(name, name_size, "%s", c->user);
    if (pass) snprintf(pass, pass_size, "%s", c->pass);
    return 1;
}

/* Once the request has ended: the connection TcpKeep named is kept for the window, if
   all went well and it's for the window's page, or it's the window's kept one, borrowed
   (by an image, say); the rest are closed */
static void conns_end(struct MillRequestRef *rr, OSErr err)
{
    short i;
    Boolean clean = !rr->cut && !rr->stale && err != userCanceledErr && rr->keep_key &&
                    (rr->page || rr->keep == rr->reused);
    for (i = 0; i < CONNS; i++) {
        NetConn *c = rr->conns[i];
        if (!c) continue;
        rr->conns[i] = NULL;
        if (c == rr->keep && clean) {
            const char *k = rr->keep_key;
            long klen = strlen(k) + 1;
            net_keep_put(rr->window, c, k, k + klen, rr->keep_nlen, k + klen + rr->keep_nlen, rr->keep_blen);
        } else net_tcp_close(c);
    }
    if (rr->keep_key) DisposePtr(rr->keep_key);
    rr->keep_key = NULL;
}

static OSErr run(Plug *p, short protocol, const Url *u, const char *url, const char *headers, const char *content_type,
                 Body *body, net_sink sink, void *ref, const char *from_owner, char owner[32],
                 short forward, Boolean *wrote, char *key, const void *window, Boolean page, Boolean *stale)
{
    struct MillRequestRef rr;
    MillRequest r;
    MillReqRef was = current;
    char *text;
    long n;
    OSErr err;
    owner[0] = 0;
    if (forward ? !plugin_enabled(p->protos[protocol].scheme) : protocol < 0 ? !any_enabled(p) :
        !plugin_enabled(p->protos[protocol].scheme)) {
        /* turned off in Settings: never loaded or called */
        char page[160];
        long n;
        if (forward) return millNoProxy;
        n = sprintf(page, "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n<TITLE>Turned off</TITLE><P>%.40s is turned off in Settings.",
                    protocol < 0 ? p->name : p->protos[protocol].heading);
        sink(page, n, ref);
        return noErr;
    }
    if (!headers) headers = "";
    if (!(text = malloc(strlen(u->path) + strlen(u->host) + strlen(headers) + 160))) return memFullErr;
    n = sprintf(text, "%s %s HTTP/1.0\r\nHost: %s", content_type ? "POST" : "GET", u->path, u->host);
    if (u->port) n += sprintf(text + n, ":%u", u->port);
    if (headers[0]) n += sprintf(text + n, "\r\n%s", headers);
    if (content_type && body) n += sprintf(text + n, "\r\nContent-Type: %.100s\r\nContent-Length: %ld", content_type, body_size(body));
    n += sprintf(text + n, "\r\n\r\n");
    if (!use(p)) { free(text); return paramErr; }
    memset(&rr, 0, sizeof rr);
    rr.sink = sink;
    rr.ref = ref;
    rr.plug = p;
    rr.u = u;
    rr.url = url;
    rr.headers = headers;
    rr.content_type = content_type;
    rr.body = content_type ? body : NULL;
    rr.text = text;
    rr.from_owner = from_owner ? from_owner : "";
    rr.window = window;
    rr.page = page;
    rr.safe = !content_type;
    memset(&r, 0, sizeof r);
    r.ref = &rr;
    r.protocol = protocol;
    r.text = text;
    r.len = n;
    r.url = url;
    r.scheme = protocol < 0 ? "plugin" : forward ? (u->scheme == SCHEME_OTHER ? u->name : u->scheme == SCHEME_HTTPS ? "https" : "http") :
               p->protos[protocol].scheme;
    r.forward = forward;
    r.user = u->user;
    r.password = u->pass;
    r.host = u->host;
    r.port = u->port;
    r.path = u->path;
    rr.req = &r;
    current = &rr;
    err = call(p, millRequest, &r);
    current = was;
    if (rr.marked && !rr.proxied) strcpy(owner, p->name);
    if (rr.failed) err = PLUGIN_FAILED;
    else if (rr.asked) err = PLUGIN_ASKED;
    else if (rr.early && err != userCanceledErr) err = PLUGIN_CUT;
    if (key && r.loginKey[0]) snprintf(key, AUTH_KEYS, "%s", r.loginKey);
    conns_end(&rr, err);
    if (stale) *stale = rr.stale;
    free(text);
    done_with(p);
    if (wrote) *wrote = rr.wrote;
    if (err == PLUGIN_FAILED || err == PLUGIN_ASKED || err == PLUGIN_CUT) return err;
    /* failing before any answer (couldn't connect, say): Mill says why, as for the web */
    if (err != noErr && err != millUnknown && !rr.wrote) return err == paramErr ? ioErr : err;
    return err == userCanceledErr ? err : noErr;
}

/* "scheme://host:port", the host in lower case: the start of a kept connection's key */
static void server_of(const Url *u, const char *scheme, char *out, long size)
{
    char *p;
    snprintf(out, size, "%s://%s:%u", scheme, u->host, u->port);
    for (p = out + strlen(scheme) + 3; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
}

OSErr plugin_request(const Url *u, const char *url, const char *headers, const char *content_type,
                     Body *body, net_sink sink, void *ref,
                     const char *from_owner, char owner[32], char key[AUTH_KEYS], const void *window, Boolean page)
{
    short protocol;
    Plug *p = route(u, &protocol);
    Boolean stale = false;
    OSErr err;
    owner[0] = 0;
    key[0] = 0;
    given_n = 0;
    used_key[0] = 0;
    if (!p) return paramErr;
    tidy_until = 0;
    err = run(p, protocol, u, url, headers, content_type, body, sink, ref, from_owner, owner, 0, NULL, key, window, page, &stale);
    if (stale) {
        /* the kept connection had gone before it answered (the server gave up on it meanwhile):
           once more, on a new one. Only a GET: nothing of the reply has gone anywhere. */
        given_n = 0;
        used_key[0] = 0;
        err = run(p, protocol, u, url, headers, content_type, body, sink, ref, from_owner, owner, 0, NULL, key, window, page, NULL);
    }
    tidy_until = 0;
    return err;
}

/* The start of the key of a kept connection for url (see keep_key); false if no Plug-In
   keeps one for it (mill:, file:, or not an address) */
static Boolean url_server(const char *url, char *server, long size)
{
    Url u;
    if (url_parse(url, &u) || u.scheme == SCHEME_MILL || u.scheme == SCHEME_FILE) return false;
    server_of(&u, u.scheme == SCHEME_HTTP ? "http" : u.scheme == SCHEME_HTTPS ? "https" : u.name, server, size);
    return true;
}

/* The window's page is now url: its kept connection goes if it's to another server */
void plugin_page_shown(const void *window, const char *url)
{
    char server[160];
    net_keep_close(window, url_server(url, server, sizeof server) ? server : NULL);
}

Boolean plugin_keep_room(const void *window, const char *url)
{
    char server[160];
    if (window && url && url_server(url, server, sizeof server)) return net_keep_close_others(window, server);
    return net_keep_close(NULL, NULL);
}

/* ------------------------------------------------------------ host functions */

static pascal void *h_new_mem(long size) { return NewPtr(size); }
static pascal void h_free_mem(void *p) { if (p) DisposePtr(p); }

static pascal void h_status(MillReqRef r, const char *text)
{
    (void)r;
    if (net_status) net_status(text);
}

static pascal void h_progress(MillReqRef r, long done, long total)
{
    char msg[48];
    long a[2];
    a[0] = done_k(done, total);
    a[1] = (total + 1023) / 1024;
    mill_format(msg, sizeof msg, total > 0 ? "Reading\311 %ldK of %ldK" : "Reading\311 %ldK", a);
    h_status(r, msg);
}

static pascal short h_busy(MillReqRef r)
{
    if (!(net_idle && net_idle())) return 0;
    if (r && r->kind == 0) r->cut = true;
    return 1;
}

static pascal OSErr h_tcp_open(MillReqRef r, const char *h, unsigned short port, MillConn *conn)
{
    short i;
    NetConn *c;
    OSErr err;
    *conn = 0;
    for (i = 0; i < CONNS && r->conns[i]; i++) {}
    if (i == CONNS) return insufficientResources;
    if ((err = net_tcp_open(h, port, &c)) != noErr) return err;
    r->conns[i] = c;
    *conn = (MillConn)c;
    return noErr;
}

static pascal OSErr h_tcp_listen(MillReqRef r, unsigned long *addr, unsigned short *port, MillConn *conn)
{
    short i;
    NetConn *c;
    OSErr err;
    *conn = 0;
    for (i = 0; i < CONNS && r->conns[i]; i++) {}
    if (i == CONNS) return insufficientResources;
    if ((err = net_tcp_listen(&c, (ip_addr *)addr, port)) != noErr) return err;
    r->conns[i] = c;
    *conn = (MillConn)c;
    return noErr;
}

/* After Stop ends a Plug-In's network call, its calls go on for a few seconds more, so
   it can tidy up (FTP deletes a stopped upload's part); then they're stopped too */
#define TIDY_TICKS (5 * 60)

Boolean plugin_tidying(void) { return tidy_until && (unsigned long)TickCount() < tidy_until; }

static OSErr tidy(OSErr err)
{
    if (err == userCanceledErr) {
        if (!tidy_until) tidy_until = TickCount() + TIDY_TICKS;
        if (current) current->cut = true;
    }
    return err;
}

/* A kept connection that fails before a byte has come back on it: the server closed it
   meanwhile. A GET is asked again (plugin_request), so the Plug-In's reply to this is
   dropped. */
static void check_reused(MillConn conn, OSErr err, long got)
{
    MillReqRef r = current;
    if (!r || (NetConn *)conn != r->reused || r->reused_got) return;
    if (err == noErr && got > 0) r->reused_got = got;
    else if (err != userCanceledErr && r->safe && !r->wrote) r->stale = true;
}

static pascal OSErr h_tcp_accept(MillConn conn) { return tidy(net_tcp_accept((NetConn *)conn)); }
static pascal OSErr h_tcp_send(MillConn conn, const void *data, long len)
{
    OSErr err = tidy(net_tcp_send((NetConn *)conn, data, len));
    if (err != noErr) check_reused(conn, err, 0);
    return err;
}
static pascal OSErr h_tcp_recv(MillConn conn, void *buf, long *len)
{
    OSErr err = tidy(net_tcp_recv((NetConn *)conn, buf, len));
    check_reused(conn, err, *len);
    return err;
}

static pascal void h_tcp_close(MillConn conn)
{
    short i;
    if (!current || !conn) return;
    if ((NetConn *)conn == current->keep) current->keep = NULL;
    for (i = 0; i < CONNS; i++) {
        if (current->conns[i] == (NetConn *)conn) { current->conns[i] = NULL; net_tcp_close((NetConn *)conn); }
    }
}

/* The key of a kept connection: the address's server, then login */
static char *keep_key(MillReqRef r, const char *login, long extra)
{
    long n;
    char *k;
    if (!r->server[0]) server_of(r->u, r->req->scheme, r->server, sizeof r->server);
    n = strlen(r->server) + 1 + strlen(login ? login : "") + 1;
    if (!(k = NewPtr(n + extra))) return NULL;
    sprintf(k, "%s\r%s", r->server, login ? login : "");
    return k;
}

static pascal OSErr h_tcp_kept(MillReqRef r, const char *login, MillConn *conn, void *note, long *len)
{
    short i;
    NetConn *c;
    char *k;
    long room = *len;
    *conn = 0;
    *len = 0;
    if (r->kind != 0 || r->req->forward || r->reused) return fnfErr;
    for (i = 0; i < CONNS && r->conns[i]; i++) {}
    if (i == CONNS || !(k = keep_key(r, login, 0))) return fnfErr;
    *len = room;
    c = net_keep_take(r->window, k, note, len);
    DisposePtr(k);
    if (!c) { *len = 0; return fnfErr; }
    r->conns[i] = c;
    r->reused = c;
    *conn = (MillConn)c;
    return noErr;
}

static pascal void h_tcp_keep(MillReqRef r, MillConn conn, const char *login, const void *note, long nlen,
                              const void *bye, long blen)
{
    short i;
    char *k;
    long klen;
    if (r->kind != 0 || r->req->forward || !conn || nlen < 0 || nlen > NET_NOTE_MAX || blen < 0 || blen > NET_NOTE_MAX) return;
    for (i = 0; i < CONNS && r->conns[i] != (NetConn *)conn; i++) {}
    if (i == CONNS) return;
    if (r->keep_key) DisposePtr(r->keep_key);
    r->keep = NULL;
    if (!(r->keep_key = k = keep_key(r, login, nlen + blen))) return;
    klen = strlen(k) + 1;
    if (nlen) memcpy(k + klen, note, nlen);
    if (blen) memcpy(k + klen + nlen, bye, blen);
    r->keep_nlen = nlen;
    r->keep_blen = blen;
    r->keep = (NetConn *)conn;
}

static short run_write(struct MillFormatRun *f, const void *data, long len);

static pascal short h_write(MillReqRef r, const void *data, long len)
{
    if (r->kind == 1) return run_write((struct MillFormatRun *)r, data, len);
    if (r->stopped || r->stale) return 0;
    if (len > 0) r->wrote = true;
    if (len > 0 && !r->sink(data, len, r->ref)) {
        /* Mill wants no more: it has all it was told of, or a limit, or Stop; only Stop
           leaves the connection in doubt for Mill (the Plug-In knows what it left unread) */
        r->stopped = true;
        if (net_idle && net_idle()) r->cut = true;
    }
    return !r->stopped;
}

static pascal long h_format(char *out, long size, const char *fmt, const long *args)
{
    return mill_format(out, size, fmt, args);
}

static pascal short h_printf(MillReqRef r, const char *fmt, const long *args)
{
    char buf[256], *big;
    long n = mill_format(buf, sizeof buf, fmt, args);
    short ok;
    if (n < (long)sizeof buf) return h_write(r, buf, n);
    if (!(big = malloc(n + 1))) return h_write(r, buf, sizeof buf - 1);
    mill_format(big, n + 1, fmt, args);
    ok = h_write(r, big, n);
    free(big);
    return ok;
}

/* The HTTP proxy is the http Plug-In's: it's asked to forward this request there, and
   says millNoProxy if it has none. It's called the way Mill calls any Plug-In (run),
   which loads it first and keeps it loaded until it returns, so the asking Plug-In's
   own code, locked while it runs, never calls into code that isn't there. */
static pascal OSErr h_proxy_request(MillReqRef r)
{
    char whole[420], owner[32];
    PlugProto *pp = plugin_proto("http");
    Boolean wrote = false;
    OSErr err;
    const Url *u = r->u;
    if (!pp) return millNoProxy;
    /* the whole address, name and password too (ftp: has no other way) */
    if (u->user[0]) { if (url_with_login(r->url, u->user, u->pass, whole, sizeof whole)) return memFullErr; }
    else snprintf(whole, sizeof whole, "%s", r->url);
    if (r->body) body_rewind(r->body);
    err = run(pp->plug, (short)(pp - pp->plug->protos), u, whole, r->headers, r->content_type, r->body,
              r->sink, r->ref, "", owner, 1, &wrote, r->req->loginKey, r->window, false, NULL);
    if (err == millNoProxy && !wrote) return millNoProxy;
    if (err == PLUGIN_ASKED) {
        /* the site wants a login: the request ends, as if its own Plug-In asked */
        r->asked = r->stopped = true;
        err = userCanceledErr;
    } else if (err == PLUGIN_CUT) {
        r->early = r->stopped = true;
        err = noErr;
    }
    r->proxied = true;
    if (wrote) r->wrote = true;
    return err;
}

/* The settings a protocol or a format lists say what each is until it's changed */
static const MillSetting *listed(const char *scheme, const char *key)
{
    PlugProto *pp = plugin_proto(scheme);
    PlugFormat *f;
    short k;
    if (pp) for (k = 0; k < pp->nset; k++) if (!strcmp(pp->sets[k].key, key)) return &pp->sets[k];
    if ((f = format_by_key(scheme)) != NULL) for (k = 0; k < f->nset; k++) if (!strcmp(f->sets[k].key, key)) return &f->sets[k];
    return NULL;
}

int plugin_setting_name(const char *scheme, const char *key, char *name)
{
    if (strlen(scheme) + strlen(key) + 2 > 48) return -1;
    /* http: and https: keep the names Mill 1.x saved them under (proxy_on, proxy_https) */
    if (!strcmp(scheme, "http") || !strcmp(scheme, "https")) { strcpy(name, key); return 0; }
    strcpy(name, scheme);
    strcat(name, ".");
    strcat(name, key);
    return 0;
}

static pascal long h_get_setting(const char *scheme, const char *key, char *out, long size)
{
    char name[48];
    const char *v;
    const MillSetting *s;
    long n;
    if (plugin_setting_name(scheme, key, name)) return -1;
    if (!(v = setting(name, NULL))) v = (s = listed(scheme, key)) ? s->dflt : NULL;
    if (!v) return -1;
    n = strlen(v);
    if (n >= size) return -1;
    strcpy(out, v);
    return n;
}

static pascal OSErr h_set_setting(const char *scheme, const char *key, const char *value)
{
    char name[48];
    if (plugin_setting_name(scheme, key, name) || strchr(value, '\r') || strchr(value, '\n')) return paramErr;
    setting_put(name, value);
    return settings_write();
}

static long put(char *out, long size, const char *s, long n)
{
    if (n >= size) return -1;
    memcpy(out, s, n);
    out[n] = 0;
    return n;
}

static pascal long h_header(MillReqRef r, const char *name, char *out, long size)
{
    const char *t = r->text, *e;
    long nl = strlen(name);
    while ((e = strstr(t, "\r\n")) && e != t) {
        if (e - t > nl && t[nl] == ':' && !strncasecmp(t, name, nl)) {
            const char *v = t + nl + 1;
            while (*v == ' ') v++;
            return put(out, size, v, e - v);
        }
        t = e + 2;
    }
    return -1;
}

static pascal long h_form_field(const char *form, long len, const char *name, char *out, long size)
{
    if (size < 1 || !form_field(form, len, name, out, size > 32767 ? 32767 : (int)size)) return -1;
    return strlen(out);
}

static pascal long h_html_escape(const char *in, char *out, long size)
{
    long o = 0;
    for (; *in; in++) {
        const char *e = *in == '&' ? "&amp;" : *in == '<' ? "&lt;" : *in == '>' ? "&gt;" : *in == '"' ? "&quot;" : NULL;
        long n = e ? strlen(e) : 1;
        if (o + n >= size) return -1;
        if (e) memcpy(out + o, e, n);
        else out[o] = *in;
        o += n;
    }
    if (o >= size) return -1;
    out[o] = 0;
    return o;
}

static pascal short h_from(MillReqRef r)
{
    char ref[300], own[48];
    if (h_header(r, "Referer", ref, sizeof ref) < 0) return millFromNone;
    if (!strcmp(r->from_owner, r->plug->name)) return millFromOwnPage;
    sprintf(own, "plugin://%.31s/", r->plug->name);
    if (!strncasecmp(ref, own, strlen(own))) return millFromOwn;
    if (!strncasecmp(ref, "mill:", 5)) return millFromLocal;
    return millFromOther;
}

static pascal void h_plugin_page(MillReqRef r) { r->marked = true; }

static void run_stop(struct MillFormatRun *r);
static char failure[200];
const char *plugin_failure(void) { return failure; }

static pascal void h_fail(MillReqRef r, const char *text)
{
    if (r->kind == 1) { run_stop((struct MillFormatRun *)r); return; }    /* a format: its decode stops */
    if (r->stale) { r->stopped = true; return; }      /* asked again instead */
    snprintf(failure, sizeof failure, "%s", text && *text ? text : "The request failed.");
    r->failed = true;
    r->stopped = true;      /* Write takes nothing more */
}

static pascal void h_ended_early(MillReqRef r)
{
    if (r->kind == 1) return;
    r->early = true;
    r->stopped = true;      /* Write takes nothing more */
}

static PluginAsked asked;
const PluginAsked *plugin_asked(void) { return &asked; }

static pascal void h_ask_login(MillReqRef r, const char *prompt, const char *name_label, const char *pass_label, const char *key,
                               const char *name, const char *pass)
{
    short i;
    if (r->kind == 1) return;
    if (r->stale) { r->stopped = true; return; }      /* asked again instead */
    /* a kept login given for this key: it didn't work */
    for (asked.forget = 0, i = 0; i < given_n; i++) if (key && !strcmp(given[i], key)) asked.forget = 1;
    snprintf(asked.name, sizeof asked.name, "%s", name ? name : "");
    snprintf(asked.pass, sizeof asked.pass, "%s", pass ? pass : "");
    snprintf(asked.prompt, sizeof asked.prompt, "%s", prompt ? prompt : "");
    snprintf(asked.name_label, sizeof asked.name_label, "%s", name_label ? name_label : "");
    snprintf(asked.pass_label, sizeof asked.pass_label, "%s", pass_label ? pass_label : "");
    snprintf(asked.key, sizeof asked.key, "%s", key ? key : "");
    r->asked = true;
    r->stopped = true;      /* Write takes nothing more */
}

static pascal OSErr h_read_body(MillReqRef r, void *buf, long *len)
{
    if (r->body && !buf) { body_rewind(r->body); return noErr; }    /* from the start again */
    if (!r->body) { *len = 0; return noErr; }
    return body_read(r->body, buf, len);
}

static pascal OSErr h_next_part(MillReqRef r, const char **name, const char **filename, const char **type)
{
    return r->body ? body_next_part(r->body, name, filename, type) : paramErr;
}

static pascal long h_url_encode(const char *in, char *out, long size)
{
    static const char digits[] = "0123456789ABCDEF";
    long o = 0;
    for (; *in; in++) {
        unsigned char c = (unsigned char)*in;
        if (c <= ' ' || c >= 127 || strchr("%\"<>#?", c)) {
            if (o + 3 >= size) return -1;
            out[o++] = '%';
            out[o++] = digits[c >> 4];
            out[o++] = digits[c & 15];
        } else {
            if (o + 1 >= size) return -1;
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
    return o;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c |= 32;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static pascal long h_url_decode(const char *in, char *out, long size)
{
    long o = 0;
    for (; *in; in++) {
        char c = *in;
        if (c == '%' && hexval(in[1]) >= 0 && hexval(in[2]) >= 0) { c = (char)(hexval(in[1]) * 16 + hexval(in[2])); in += 2; }
        if (o + 1 >= size) return -1;
        out[o++] = c;
    }
    out[o] = 0;
    return o;
}

static pascal long h_base64_encode(const void *in, long len, char *out, long size)
{
    long n = auth_base64(in, (int)len, out, (int)(size > 32767 ? 32767 : size));
    return n ? n : (len ? -1 : 0);
}

static pascal long h_base64_decode(const char *in, long len, void *out, long size)
{
    unsigned char *o = out;
    unsigned long bits = 0;
    long i, n = 0;
    int have = 0;
    for (i = 0; i < len; i++) {
        char c = in[i];
        int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 :
                c == '+' ? 62 : c == '/' ? 63 : -1;
        if (v < 0) continue;
        bits = bits << 6 | v;
        if ((have += 6) >= 8) {
            have -= 8;
            if (n >= size) return -1;
            o[n++] = (unsigned char)(bits >> have);
        }
    }
    return n;
}

static pascal void h_md5(const void *in, long len, unsigned char digest[16]) { auth_md5(in, len, digest); }

static pascal long h_to_mac_roman(const char *in, long len, short cs, char *out, long size)
{
    const unsigned char *s = (const unsigned char *)in;
    long i = 0, o = 0;
    if (cs != millLatin1 && cs != millUTF8) cs = charset_sniff(in, len) == CHARSET_UTF8 ? millUTF8 : millLatin1;
    while (i < len) {
        unsigned char c;
        long u;
        int k;
        if (cs == millUTF8 && s[i] >= 0x80 && (k = utf8_char(s + i, len - i, &u)) > 0) { c = unicode_to_mac(u); i += k; }
        else c = s[i] < 0x80 ? s[i] : latin1_to_mac(s[i]), i++;
        if (!c) continue;
        if (o + 1 >= size) return -1;
        out[o++] = (char)c;
    }
    out[o] = 0;
    return o;
}

static pascal OSErr h_picture_size(MillFormatRef f, short w, short h, short photo, short *dw, short *dh);
static pascal void h_picture_rect(MillFormatRef f, short x, short y, short w, short h,
                                  const unsigned char *gray, const unsigned char *opaque);
static pascal long h_type_for_name(const char *name, char *out, long size);
static pascal long h_type_for_finder(OSType t, char *out, long size);
static pascal OSErr h_entry(MillFormatRef f, const MillEntry *e);
static pascal long h_read_at(MillFormatRef f, unsigned long at, void *buf, long len);
static pascal long h_size(MillFormatRef f);

static pascal MillGzRef h_gz_begin(MillReqRef r, void *mem, long size, short raw, void *ref);
static pascal short h_gz_data(MillGzRef z, const void *p, long n);
static pascal long h_gz_unused(MillGzRef z);
static pascal void h_gz_end(MillGzRef z);

/* RFC 2068's ranges, for any protocol that can seek: core/range.c */
static pascal short h_file_head(MillReqRef r, const char *type, long size, long *first, long *last)
{
    char head[400];
    long n;
    short part;
    if (r->kind != 0) return 0;
    part = (short)range_part(r->text, strlen(r->text), size, first, last);
    if (!type) return part;
    if ((n = range_head(head, sizeof head, type, size, part, *first, *last)) < 0) {
        part = 0;
        *first = 0;
        *last = size - 1;
        n = range_head(head, sizeof head, "application/octet-stream", size, 0, 0, size - 1);
    }
    h_write(r, head, n);
    return part;
}

/* The protocol for an address, or NULL */
static PlugProto *proto_of(const Url *u)
{
    return plugin_proto(u->scheme == SCHEME_FILE ? "file" : u->scheme == SCHEME_HTTP ? "http" :
                        u->scheme == SCHEME_HTTPS ? "https" : u->scheme == SCHEME_OTHER ? u->name : "");
}

long plugin_block_size(const Url *u)
{
    PlugProto *pp = proto_of(u);
    return pp ? pp->block : 0;
}

Boolean plugin_random_access(const Url *u)
{
    PlugProto *pp = proto_of(u);
    char v[8];
    if (!pp || pp->block <= 0) return false;
    if (u->scheme == SCHEME_FILE) return true;     /* always on, with no setting */
    return h_get_setting(pp->scheme, "random_access", v, sizeof v) == 3 && !strcmp(v, "yes");
}

Boolean plugin_seekable(const char *url, const char *head, long len, long *size, long *block)
{
    Url u;
    *size = -1;
    *block = 0;
    if (url_parse(url, &u) || !plugin_random_access(&u) || !range_seekable(head, len, size)) return false;
    *block = plugin_block_size(&u);
    return true;
}

void plugin_host_init(void)
{
    app_res = CurResFile();
    host.size = sizeof host;
    host.version = kMillInterface;
    host.qd = &qd;
    host.NewMem = h_new_mem;
    host.FreeMem = h_free_mem;
    host.Status = h_status;
    host.Progress = h_progress;
    host.Busy = h_busy;
    host.TcpOpen = h_tcp_open;
    host.TcpSend = h_tcp_send;
    host.TcpRecv = h_tcp_recv;
    host.TcpClose = h_tcp_close;
    host.TcpListen = h_tcp_listen;
    host.TcpAccept = h_tcp_accept;
    host.TcpKept = h_tcp_kept;
    host.TcpKeep = h_tcp_keep;
    host.Write = h_write;
    host.ProxyRequest = h_proxy_request;
    host.GetSetting = h_get_setting;
    host.SetSetting = h_set_setting;
    host.Format = h_format;
    host.Printf = h_printf;
    host.FileHead = h_file_head;
    host.Header = h_header;
    host.FormField = h_form_field;
    host.HtmlEscape = h_html_escape;
    host.From = h_from;
    host.UrlEncode = h_url_encode;
    host.UrlDecode = h_url_decode;
    host.Base64Encode = h_base64_encode;
    host.Base64Decode = h_base64_decode;
    host.MD5 = h_md5;
    host.ToMacRoman = h_to_mac_roman;
    host.PlugInPage = h_plugin_page;
    host.PictureSize = h_picture_size;
    host.PictureRect = h_picture_rect;
    host.Entry = h_entry;
    host.ReadAt = h_read_at;
    host.Size = h_size;
    host.TypeForName = h_type_for_name;
    host.TypeForFinderType = h_type_for_finder;
    host.ReadBody = h_read_body;
    host.NextPart = h_next_part;
    host.Fail = h_fail;
    host.AskLogin = h_ask_login;
    host.SavedLogin = h_saved_login;
    host.EndedEarly = h_ended_early;
    host.GzBegin = h_gz_begin;
    host.GzData = h_gz_data;
    host.GzUnused = h_gz_unused;
    host.GzEnd = h_gz_end;
}

#ifdef MILL_MEMREPORT
void memreport_plugins(void)
{
    short i, k, j;
    if (fonts_map) memreport_note(fonts_map, "Web Fonts: the open suitcase's resource map", 1);
    memreport_note(plugs, "Plug-In table", 0);
    for (i = 0; i < nplugs; i++) {
        Plug *p = &plugs[i];
        const char *l = "Plug-In info and settings (Mill's copies)";
        memreport_note(p->name, l, 0);
        memreport_note(p->version, l, 0);
        memreport_note(p->copyright, l, 0);
        memreport_note(p->by, l, 0);
        memreport_note(p->description, l, 0);
        memreport_note(p->protos, l, 0);
        memreport_note(p->self, "Plug-Ins' own memory (loaded)", 0);
        for (k = 0; k < p->nproto; k++) {
            PlugProto *pp = &p->protos[k];
            memreport_note(pp->scheme, l, 0);
            memreport_note(pp->heading, l, 0);
            memreport_note(pp->description, l, 0);
            memreport_note(pp->sets, l, 0);
            for (j = 0; j < pp->nset; j++) {
                memreport_note(pp->sets[j].key, l, 0);
                memreport_note(pp->sets[j].label, l, 0);
                memreport_note(pp->sets[j].note, l, 0);
                memreport_note(pp->sets[j].dflt, l, 0);
                memreport_note(pp->sets[j].choices, l, 0);
            }
        }
    }
}
#endif

/* ------------------------------------------------------------ formats */

static int same_ci(const char *a, const char *b, long n)
{
    while (n-- > 0) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x |= 32;
        if (y >= 'A' && y <= 'Z') y |= 32;
        if (x != y) return 0;
    }
    return 1;
}

/* Whether word (n characters) is one of list's, separated by spaces, in any case */
static Boolean listed_in(const char *list, const char *word, long n)
{
    while (*list) {
        const char *e;
        while (*list == ' ') list++;
        for (e = list; *e && *e != ' '; e++) {}
        if (e - list == n && n && same_ci(list, word, n)) return true;
        list = e;
    }
    return false;
}

/* A name's (or address's) extension, without the dot: its length, 0 if none */
static long extension(const char *name, const char **ext)
{
    const char *end = name + strcspn(name, "?#"), *p = end;
    while (p > name && p[-1] != '.' && p[-1] != '/' && p[-1] != ':') p--;
    if (p == name || p[-1] != '.') return 0;
    *ext = p;
    return end - p;
}

Boolean format_enabled(const PlugFormat *f)
{
    char name[48];
    sprintf(name, "%.15s.enable", f->key);
    return strcmp(setting(name, "yes"), "no") != 0;
}

PlugFormat *format_by_key(const char *key)
{
    short i, k;
    for (i = 0; i < nplugs; i++)
        for (k = 0; plugs[i].status == PLUG_READY && k < plugs[i].nformat; k++)
            if (!plugs[i].formats[k].replaced_by && !strcmp(plugs[i].formats[k].key, key)) return &plugs[i].formats[k];
    return NULL;
}

void formats_overlap(void)
{
    short i, k, j, m;
    for (i = 0; i < nplugs; i++)
        for (k = 0; plugs[i].status == PLUG_READY && k < plugs[i].nformat; k++) {
            PlugFormat *f = &plugs[i].formats[k], *last = NULL;
            /* the last loaded with its key, after it (Plug-Ins in folder order, after the built-ins) */
            for (j = i; j < nplugs; j++)
                for (m = j == i ? k + 1 : 0; plugs[j].status == PLUG_READY && m < plugs[j].nformat; m++)
                    if (!strcmp(plugs[j].formats[m].key, f->key)) last = &plugs[j].formats[m];
            f->replaced_by = last;
            if (last && !last->replaces) last->replaces = f;
        }
}

PlugFormat *format_for(const char *type, const char *name, OSType ftype, long wants)
{
    long n = type ? (long)strcspn(type, "; ") : 0, en = 0;
    const char *ext = NULL;
    Boolean by_type = n && !(n == 24 && same_ci(type, "application/octet-stream", 24));
    short i, k;
    if (!by_type && name) en = extension(name, &ext);
    for (i = 0; i < nplugs; i++) {
        if (plugs[i].status != PLUG_READY) continue;
        for (k = 0; k < plugs[i].nformat; k++) {
            PlugFormat *f = &plugs[i].formats[k];
            if (f->replaced_by || !(f->gives & wants) || !format_enabled(f)) continue;
            if (by_type ? listed_in(f->types, type, n)
                        : (en && listed_in(f->extensions, ext, en)) || (ftype && listed_in(f->fileTypes, (const char *)&ftype, 4)))
                return f;
        }
    }
    return NULL;
}

/* The first word of a list, into out */
static long first_word(const char *list, char *out, long size)
{
    long n = strcspn(list, " ");
    if (n >= size) n = size - 1;
    memcpy(out, list, n);
    out[n] = 0;
    return n;
}

long type_for_name(const char *name, char *out, long size)
{
    static const char *const known[][2] = {
        { "html", "text/html" }, { "htm", "text/html" }, { "txt", "text/plain" }, { "text", "text/plain" },
        { "msg", "text/plain" }, { "message", "text/plain" }, { "asc", "text/plain" },
        { "gif", "image/gif" }, { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" }
    };
    const char *ext, *base = name, *end = name + strcspn(name, "?#");
    long n = extension(name, &ext);
    short i, k;
    /* the name README is text too (only that name: the type goes by name, never by contents) */
    for (; name < end; name++) if (*name == '/' || *name == ':') base = name + 1;
    if (end - base == 6 && !memcmp(base, "README", 6)) return first_word("text/plain", out, size);
    if (n) {
        for (i = 0; i < (short)(sizeof known / sizeof known[0]); i++)
            if ((long)strlen(known[i][0]) == n && same_ci(known[i][0], ext, n)) return first_word(known[i][1], out, size);
        for (i = 0; i < nplugs; i++)
            for (k = 0; plugs[i].status == PLUG_READY && k < plugs[i].nformat; k++)
                if (!plugs[i].formats[k].replaced_by && listed_in(plugs[i].formats[k].extensions, ext, n))
                    return first_word(plugs[i].formats[k].types, out, size);
    }
    return first_word("application/octet-stream", out, size);
}

long type_for_finder(OSType t, char *out, long size)
{
    short i, k;
    if (t == 'TEXT') return first_word("text/plain", out, size);
    for (i = 0; i < nplugs; i++)
        for (k = 0; plugs[i].status == PLUG_READY && k < plugs[i].nformat; k++)
            if (!plugs[i].formats[k].replaced_by && listed_in(plugs[i].formats[k].fileTypes, (const char *)&t, 4))
                return first_word(plugs[i].formats[k].types, out, size);
    return first_word("application/octet-stream", out, size);
}

#define PICTURE_SPARE (24L * 1024)    /* memory a picture leaves for the rest of Mill */
#define FIRST_BYTES 512               /* what millFormatBegin is given to decide with */

/* Content-Encoding: gzip, undone before the reply's format (plug_gzip.c, in its own segment) */
void *encoding_begin(int (*out)(void *ref, const unsigned char *p, long n), void *ref);
int encoding_data(void *g, const unsigned char *p, long n);
void encoding_end(void *g);

/* Where a run's input goes once it's decided */
enum { IN_WAIT, IN_FORMAT, IN_SAVE, IN_DROP };
/* Where its output goes */
enum { OUT_NONE, OUT_PAGE, OUT_NEXT, OUT_SAVE };

/* One format reading one input: the reply (depth 0), or a member another run gives */
struct MillFormatRun {
    short kind;                 /* 1: a format's run (Write tells it from a request) */
    PlugFormat *f;              /* NULL: the bytes go straight out (a page undone from gzip) */
    MillFormatParams prm;
    char type[80], url[300], path[256];
    ImageCanvas *canvas;
    short dw, dh, photo, aw, ah;
    long wants;
    const FmtSink *sink;
    short depth;
    MillEntry member;           /* depth > 0: the member it reads (name in member_name) */
    char member_name[256];
    void *gz;                   /* Content-Encoding: gzip, undone first */
    char *head;                 /* the first bytes, until there are FIRST_BYTES or the end */
    short nhead;
    short in, out;              /* IN_..., OUT_... */
    Boolean begun;              /* Begin returned noErr: End is owed */
    Boolean listing;            /* path asks for the listing */
    Boolean stopped;
    FmtSource *src;             /* millFormatPull: where ReadAt reads, and its last block */
    char *block;
    unsigned long block_at;
    long block_len;
    struct MillFormatRun *next; /* OUT_NEXT: the format for what it gives */
    char final[64];             /* OUT_PAGE: the type of what it gives */
    Boolean deciding;           /* in Begin: what it writes is kept in pend, till the output is known */
    char *pend;
    long npend;
};

/* Inflates under way, Content-Encoding's and Plug-Ins' (GzBegin): the Gzip segment is let
   go when the last ends */
static short gz_live;

static void gz_done(void)
{
    if (gz_live > 0 && --gz_live == 0) UnloadSeg((void *)encoding_begin);
}

static void free_run(struct MillFormatRun *r)
{
    if (!r) return;
    canvas_free(r->canvas);
    if (r->gz) { encoding_end(r->gz); gz_done(); }
    if (r->f) done_with(r->f->plug);
    free(r->pend);
    free(r->head);
    free(r->block);
    free(r);
}

static Boolean is_listing(const char *path)
{
    size_t n = strlen(path);
    return n == 0 || path[n - 1] == '/';
}

static struct MillFormatRun *new_run(PlugFormat *f, const char *type, const char *url, const char *path, short aw, short ah,
                                     long wants, long size, Boolean seekable, const FmtSink *sink, short depth)
{
    struct MillFormatRun *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    if (f && !use(f->plug)) { free(r); return NULL; }
    if (f && !(r->head = malloc(FIRST_BYTES))) { done_with(f->plug); free(r); return NULL; }
    r->kind = 1;
    r->f = f;
    r->aw = aw;
    r->ah = ah;
    r->wants = wants;
    r->sink = sink;
    r->depth = depth;
    snprintf(r->type, sizeof r->type, "%s", type ? type : "");
    snprintf(r->url, sizeof r->url, "%s", url ? url : "");
    snprintf(r->path, sizeof r->path, "%s", path ? path : "");
    r->listing = is_listing(r->path);
    r->prm.ref = r;
    r->prm.format = f ? (short)(f - f->plug->formats) : 0;
    r->prm.type = r->type;
    r->prm.url = r->url;
    r->prm.path = r->path;
    r->prm.width = aw > 0 ? aw : 0;
    r->prm.height = ah > 0 ? ah : 0;
    r->prm.wants = wants;
    r->prm.size = size;
    r->prm.seekable = seekable;
    if (!f) { r->in = IN_FORMAT; r->out = OUT_PAGE; snprintf(r->final, sizeof r->final, "%.63s", r->type); }
    return r;
}

static int run_input(struct MillFormatRun *r, const char *p, long n);

/* Where a format's output goes, once it has said what it is (a stream's outType, or HTML) */
static void route_output(struct MillFormatRun *r)
{
    PlugFormat *next;
    if (r->f->gives & millGivesMembers) return;     /* each member's Entry says */
    if (r->f->gives & millGivesStream && r->prm.outType[0]) {
        if ((next = format_for(r->prm.outType, NULL, 0, r->wants)) != NULL && next != r->f &&
            (r->next = new_run(next, r->prm.outType, r->url, "", r->aw, r->ah, r->wants, -1, false, r->sink, r->depth + 1)) != NULL)
            r->out = OUT_NEXT;
        else { r->out = OUT_PAGE; snprintf(r->final, sizeof r->final, "%s", r->prm.outType); }
    } else if (r->f->gives & millGivesElements && r->wants & millGivesElements) {
        /* HTML, with the character set Begin gave, if it gave one */
        r->out = OUT_PAGE;
        if (same_ci(r->prm.outType, "text/html", 9)) snprintf(r->final, sizeof r->final, "%s", r->prm.outType);
        else strcpy(r->final, "text/html");
    }
}

static short run_write(struct MillFormatRun *f, const void *data, long len);

/* What Begin wrote, once the output is known: sent on (go), or dropped (and by an archive,
   whose members' Entry calls say where their bytes go) */
static void pend_end(struct MillFormatRun *r, Boolean go)
{
    char *p = r->pend;
    long n = r->npend;
    r->pend = NULL;
    r->npend = 0;
    if (go && n && r->out != OUT_NONE) run_write(r, p, n);
    free(p);
}

/* The first bytes are in (or all there is): Begin, and what Mill does with the answer */
static void run_decide(struct MillFormatRun *r)
{
    FmtAsk a;
    short got;
    OSErr err;
    if (r->in != IN_WAIT) return;
    r->prm.data = r->head;
    r->prm.len = r->nhead;
    r->prm.answer = millShowNow;
    r->prm.pull = 0;
    r->prm.reason[0] = 0;
    r->prm.outType[0] = 0;
    r->deciding = true;
    err = call(r->f->plug, millFormatBegin, &r->prm);
    r->deciding = false;
    if (err != noErr) { pend_end(r, false); r->in = IN_DROP; r->stopped = true; return; }
    r->begun = true;
    memset(&a, 0, sizeof a);
    a.depth = r->depth;
    a.f = r->f;
    a.answer = r->prm.answer;
    a.pull = r->prm.pull && r->prm.seekable;
    a.reason = r->prm.reason;
    a.size = r->prm.size;
    a.seekable = r->prm.seekable;
    a.path = r->path;
    a.member = r->depth ? &r->member : NULL;
    got = r->sink->ask(r->sink->ref, &a);
    r->prm.data = NULL;
    r->prm.len = 0;
    if (got == FMT_GO) {
        r->in = IN_FORMAT;
        route_output(r);
        pend_end(r, true);
        if (r->nhead && !r->stopped) run_input(r, r->head, r->nhead);
    } else if (got == FMT_SAVE) {
        pend_end(r, false);
        r->in = IN_SAVE;
        if (r->nhead && (!r->sink->save || !r->sink->save(r->head, r->nhead, r->sink->ref))) r->stopped = true;
    } else {
        if (got != FMT_PULL) pend_end(r, false);     /* (pulled: kept for format_pull) */
        r->in = IN_DROP;    /* stopped, or to be pulled (format_pull) */
        r->stopped = true;
    }
}

/* Bytes into a run: kept until it's decided, then to its format (or saved) */
static int run_input(struct MillFormatRun *r, const char *p, long n)
{
    OSErr err;
    if (r->stopped) return 1;
    if (r->in == IN_WAIT) {
        long k = FIRST_BYTES - r->nhead;
        if (k > n) k = n;
        if (p != r->head) { memcpy(r->head + r->nhead, p, k); r->nhead += (short)k; p += k; n -= k; }
        if (r->nhead < FIRST_BYTES) return 0;
        run_decide(r);
        if (r->stopped) return 1;
        /* the first bytes went on with the decision: the rest follows */
        if (n <= 0) return 0;
    }
    if (n <= 0) return r->stopped;
    if (r->in == IN_SAVE) {
        if (!r->sink->save(p, n, r->sink->ref)) r->stopped = true;
        return r->stopped;
    }
    if (r->in != IN_FORMAT) return 1;
    if (!r->f) {
        if (!r->sink->out(p, n, r->sink->ref)) r->stopped = true;
        return r->stopped;
    }
    r->prm.data = p;
    r->prm.len = n;
    err = call(r->f->plug, millFormatData, &r->prm);
    r->prm.data = NULL;
    if (err == millDone || r->stopped) return 1;
    return err == noErr ? 0 : err == fnfErr ? -2 : -1;
}

/* Content-Encoding: what the gzip code gives goes into the run */
static int encoded_out(void *ref, const unsigned char *p, long n)
{
    struct MillFormatRun *r = ref;
    return run_input(r, (const char *)p, n) != 0;
}

FmtRun *format_begin(PlugFormat *f, const char *type, const char *encoding, const char *url, const char *path,
                     unsigned long pos, Boolean has_pos, short aw, short ah, long wants, long size, Boolean seekable,
                     const FmtSink *sink)
{
    struct MillFormatRun *r = new_run(f, type, url, path, aw, ah, wants, size, seekable, sink, 0);
    if (!r) return NULL;
    r->prm.pos = pos;
    r->prm.hasPos = has_pos;
    if (encoding && same_ci(encoding, "gzip", 4)) {
        /* undone first: the format (or the page) gets what's inside, of unknown size, and
           never seeks in it */
        r->prm.size = -1;
        r->prm.seekable = 0;
        if (!(r->gz = encoding_begin(encoded_out, r))) { free_run(r); return NULL; }
        gz_live++;
    }
    return (FmtRun *)r;
}

const char *format_out_type(const FmtRun *run)
{
    const struct MillFormatRun *r = (const struct MillFormatRun *)run;
    while (r && r->next) r = r->next;
    return r && r->final[0] ? r->final : NULL;
}

int format_data(FmtRun *run, const char *p, long n)
{
    struct MillFormatRun *r = (struct MillFormatRun *)run;
    int got;
    if (r->stopped) return 1;
    if (!r->gz) return run_input(r, p, n);
    got = encoding_data(r->gz, (const unsigned char *)p, n);
    if (r->stopped || got == 1) return 1;
    return got == 0 ? 0 : got == -2 ? 1 : -1;
}

void format_input_end(FmtRun *run)
{
    struct MillFormatRun *r = (struct MillFormatRun *)run;
    if (r && r->f && r->in == IN_WAIT) run_decide(r);
}

static void run_stop(struct MillFormatRun *r) { r->stopped = true; }

/* What a format writes: on to the next format, the page, or the file being saved; during
   Begin, kept till Mill knows which */
static short run_write(struct MillFormatRun *r, const void *data, long len)
{
    if (r->stopped || len <= 0) return !r->stopped;
    if (r->deciding) {
        char *more = realloc(r->pend, r->npend + len);
        if (!more) { r->stopped = true; return 0; }
        memcpy(more + r->npend, data, len);
        r->pend = more;
        r->npend += len;
        return 1;
    }
    switch (r->out) {
    case OUT_NEXT: if (run_input(r->next, data, len) != 0) r->stopped = true; break;
    case OUT_PAGE: if (!r->sink->out(data, len, r->sink->ref)) r->stopped = true; break;
    case OUT_SAVE: if (!r->sink->save || !r->sink->save(data, len, r->sink->ref)) r->stopped = true; break;
    default: r->stopped = true;     /* a member's bytes before its Entry, or nothing wants them */
    }
    return !r->stopped;
}

/* The type of a member: by its Finder type, when it has a telling one, else by its name. A
   Mac's text is Mac Roman (core/filetypes.c). */
static void member_type(const MillEntry *e, const char *name, char *out, long size)
{
    char by_name[64];
    if (e->flags & millMemberMac && e->fileType == 'TEXT') {
        type_for_name(name, by_name, sizeof by_name);
        snprintf(out, size, "%s", mac_text_type(by_name));
        return;
    }
    if (e->flags & millMemberMac && e->fileType && e->fileType != 'BINA' && e->fileType != '????' &&
        type_for_finder(e->fileType, out, size) && strcmp(out, "application/octet-stream"))
        return;
    type_for_name(name, out, size);
}

static pascal OSErr h_entry(MillFormatRef f, const MillEntry *e)
{
    struct MillFormatRun *r = (struct MillFormatRun *)f, *next;
    char type[64];
    PlugFormat *fmt;
    const char *rest;
    Boolean forks = false;
    if (r->stopped || !e || !e->name) return userCanceledErr;
    if (r->listing) return r->sink->entry && r->sink->entry(r->sink->ref, e, r->path) ? noErr : userCanceledErr;
    if (r->out != OUT_NONE) return noErr;   /* the member's Entry, again */
    /* the member it's about to give: where its bytes go */
    member_type(e, e->name, type, sizeof type);
    rest = r->prm.rest ? r->prm.rest : "";
    fmt = format_for(type, e->name, (e->flags & millMemberMac) ? e->fileType : 0, r->wants);
    if (!fmt && !(r->wants & millGivesPicture) && format_for(type, e->name, 0, millGivesPicture)) {
        /* a picture, on a page: the page is the picture's (Mill asks for it again as an image) */
        snprintf(r->final, sizeof r->final, "%s", type);
        r->stopped = true;
        return userCanceledErr;
    }
    if (fmt) {
        if (!(next = new_run(fmt, type, r->url, rest, r->aw, r->ah, r->wants, e->size, false, r->sink, r->depth + 1)))
            return memFullErr;
        next->member = *e;
        snprintf(next->member_name, sizeof next->member_name, "%s", e->name);
        next->member.name = next->member_name;
        next->member.comment = NULL;
        r->next = next;
        r->out = OUT_NEXT;
        return noErr;
    }
    if (*rest) return fnfErr;   /* a path past a member no format reads */
    if (!type[0] || !strncmp(type, "text/", 5)) {
        if (!r->sink->out) { r->stopped = true; return userCanceledErr; }   /* an image that's text */
        r->out = OUT_PAGE;
        snprintf(r->final, sizeof r->final, "%s", type[0] ? type : "text/plain");
        return noErr;
    }
    /* nothing shows it: saved, if Mill's saving (as a Mac file, both forks, when it is one) */
    if (!r->sink->save_member || !r->sink->save_member(r->sink->ref, e, &forks)) { r->stopped = true; return userCanceledErr; }
    r->prm.forks = forks;
    r->out = OUT_SAVE;
    return noErr;
}

/* ReadAt: whole blocks of the protocol's size, the last one kept */
static pascal long h_read_at(MillFormatRef f, unsigned long at, void *buf, long len)
{
    struct MillFormatRun *r = (struct MillFormatRun *)f;
    FmtSource *s = r->src;
    long done = 0;
    if (!s || len < 0) return -1;
    while (len > 0) {
        long k;
        if (s->size >= 0 && at >= (unsigned long)s->size) break;
        if (r->block && at >= r->block_at && at < r->block_at + r->block_len) {
            k = r->block_at + r->block_len - at;
            if (k > len) k = len;
            memcpy((char *)buf + done, r->block + (at - r->block_at), k);
        } else if (len >= s->block) {
            /* a large read: straight into buf */
            if ((k = s->read(s->ref, at, (char *)buf + done, len)) < 0) return -1;
            if (k == 0) break;
        } else {
            if (!r->block && !(r->block = malloc(s->block))) return -1;
            r->block_at = at - at % s->block;
            r->block_len = 0;
            if ((k = s->read(s->ref, r->block_at, r->block, s->block)) < 0) return -1;
            r->block_len = k;
            if (at >= r->block_at + k) break;
            continue;
        }
        at += k;
        done += k;
        len -= k;
    }
    return done;
}

static pascal long h_size(MillFormatRef f)
{
    struct MillFormatRun *r = (struct MillFormatRun *)f;
    return r->src ? r->src->size : -1;
}

OSErr format_pull(FmtRun *run, FmtSource *src)
{
    struct MillFormatRun *r = (struct MillFormatRun *)run;
    OSErr err = noErr;
    if (!r->f || !r->begun) return paramErr;
    if (src->block < 512) src->block = 512;
    r->src = src;
    r->stopped = false;
    if (r->prm.pull) {
        r->in = IN_DROP;
        route_output(r);
        pend_end(r, true);
        err = call(r->f->plug, millFormatPull, &r->prm);
    } else {
        /* a format that reads in order, from a source Mill reads itself (file:): all of
           it, from the start, as Data */
        char *buf = malloc(src->block);
        unsigned long at = 0;
        long n = 0;
        int got = 0;
        if (!buf) { r->src = NULL; return memFullErr; }
        r->in = IN_FORMAT;
        route_output(r);
        pend_end(r, true);
        while (!r->stopped && (n = src->read(src->ref, at, buf, src->block)) > 0) {
            at += n;
            if ((got = run_input(r, buf, n)) != 0) break;
        }
        free(buf);
        err = n < 0 ? ioErr : got == -2 ? fnfErr : got < 0 ? paramErr : noErr;
    }
    r->src = NULL;
    return err;
}

int format_end(FmtRun *run, short cut, Bitmap1 *pic, char *type)
{
    struct MillFormatRun *r = (struct MillFormatRun *)run, *next;
    int got = -1;
    if (pic) memset(pic, 0, sizeof *pic);
    if (type) type[0] = 0;
    while (r) {
        /* all its input is in: decided now if the first bytes never filled */
        if (r->f && r->in == IN_WAIT && !cut) run_decide(r);
        if (r->begun) {
            r->prm.cut = cut;
            r->prm.data = NULL;
            r->prm.len = 0;
            call(r->f->plug, millFormatEnd, &r->prm);   /* a stream may give its last bytes now */
        }
        if (r->canvas && pic && got != 0) got = canvas_finish(r->canvas, r->photo, pic);
        if (type && r->final[0]) snprintf(type, 64, "%s", r->final);
        next = r->next;
        free_run(r);
        r = next;
    }
    return got;
}

static pascal OSErr h_picture_size(MillFormatRef f, short w, short h, short photo, short *dw, short *dh)
{
    struct MillFormatRun *r = (struct MillFormatRun *)f;
    int err;
    if (w < 1 || h < 1) return paramErr;
    r->photo = photo;
    if (!r->canvas) {
        image_fit(w, h, r->aw, r->ah, &r->dw, &r->dh);
        /* smaller, in proportion, to fit the memory there is: a byte a pixel while it's
           made, and an eighth of that (twice, with a mask) once it's dithered, and room left */
        while ((long)r->dw * r->dh * 5 / 4 + PICTURE_SPARE > MaxBlock() && r->dw > 16 && r->dh > 16) {
            r->dw = (short)((long)r->dw * 7 / 8);
            r->dh = (short)((long)r->dh * 7 / 8);
        }
        while ((err = canvas_new(&r->canvas, r->dw, r->dh, w, h)) == IMAGE_NO_MEMORY && r->dw > 16 && r->dh > 16) {
            r->dw = (short)((long)r->dw * 7 / 8);
            r->dh = (short)((long)r->dh * 7 / 8);
        }
    } else err = canvas_source(r->canvas, w, h);
    *dw = r->dw;
    *dh = r->dh;
    return err == 0 ? noErr : err == IMAGE_NO_MEMORY ? memFullErr : paramErr;
}

static pascal void h_picture_rect(MillFormatRef f, short x, short y, short w, short h,
                                  const unsigned char *gray, const unsigned char *opaque)
{
    struct MillFormatRun *r = (struct MillFormatRun *)f;
    if (r->canvas) canvas_rect(r->canvas, x, y, w, h, gray, opaque);
}

static pascal long h_type_for_name(const char *name, char *out, long size) { return type_for_name(name, out, size); }
static pascal long h_type_for_finder(OSType t, char *out, long size) { return type_for_finder(t, out, size); }

/* ------------------------------------------------------------ inflate for Plug-Ins */

/* GzBegin's: Mill's inflate (core/inflate.c, in the Gzip segment) in the caller's memory,
   this at its start, then inflate's state and window; what it makes goes to the Plug-In
   through its entry point (millGzOut) */
struct MillGz {
    Plug *plug;
    void *ref;
    void *gz;
    char *mem;
    long used, size;
};

static void *gz_carve(void *ref, long n)
{
    struct MillGz *z = ref;
    char *p;
    n = (n + 3) & ~3L;
    if (z->used + n > z->size) return NULL;
    p = z->mem + z->used;
    z->used += n;
    return p;
}

static void gz_keep(void *ref, void *p) { (void)ref; (void)p; }     /* the caller's, freed by it */

static int gz_to_plug(void *ref, const unsigned char *p, long n)
{
    struct MillGz *z = ref;
    MillGzOutParams g;
    g.ref = z->ref;
    g.data = p;
    g.len = n;
    return call(z->plug, millGzOut, &g) != noErr;
}

static pascal MillGzRef h_gz_begin(MillReqRef r, void *mem, long size, short raw, void *ref)
{
    struct MillGz *z = mem;
    struct MillFormatRun *f = (struct MillFormatRun *)r;
    Plug *p = !r ? NULL : r->kind == 1 ? (f->f ? f->f->plug : NULL) : r->plug;
    GzMem m;
    if (!p || !mem || size < (long)((sizeof *z + 3) & ~3L) + gz_size() + 4) return NULL;
    z->plug = p;
    z->ref = ref;
    z->mem = mem;
    z->size = size;
    z->used = (sizeof *z + 3) & ~3L;
    m.ref = z;
    m.alloc = gz_carve;
    m.release = gz_keep;
    if (!(z->gz = gz_begin_with(&m, raw, gz_to_plug, z))) return NULL;
    gz_live++;
    return z;
}

static pascal short h_gz_data(MillGzRef z, const void *p, long n) { return z ? (short)gz_data(z->gz, p, n) : -1; }
static pascal long h_gz_unused(MillGzRef z) { return z ? gz_unused(z->gz) : 0; }

static pascal void h_gz_end(MillGzRef z)
{
    if (!z || !z->gz) return;
    gz_end(z->gz);
    z->gz = NULL;
    gz_done();
}
