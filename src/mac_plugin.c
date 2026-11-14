/* Plug-Ins: finding them, loading them only while they're used, and the host
   functions they call (sdk/Interfaces/MillPlugIn.h). */
#include <Multiverse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/auth.h"
#include "core/charset.h"
#include "core/fmt.h"
#include "mac_local.h"
#include "mac_plugin.h"
#include "mac_sys.h"
#include "mac_upload.h"
#include "mac_memreport.h"

/* The built-ins, each in a code segment of its own (mill.segmap) */
pascal OSErr http_main(short selector, void *params, void **self);
pascal OSErr https_main(short selector, void *params, void **self);
pascal OSErr file_main(short selector, void *params, void **self);
pascal OSErr gopher_main(short selector, void *params, void **self);
pascal OSErr ftp_main(short selector, void *params, void **self);
pascal OSErr gif_main(short selector, void *params, void **self);
pascal OSErr jpeg_main(short selector, void *params, void **self);
pascal OSErr netpbm_main(short selector, void *params, void **self);
pascal OSErr gzip_main(short selector, void *params, void **self);

Plug *plugs;
short nplugs;
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
    case 4: err = gopher_main(selector, params, &p->self); break;
    case 5: err = ftp_main(selector, params, &p->self); break;
    case 6: err = gif_main(selector, params, &p->self); break;
    case 7: err = jpeg_main(selector, params, &p->self); break;
    case 8: err = netpbm_main(selector, params, &p->self); break;
    case 9: err = gzip_main(selector, params, &p->self); break;
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
    case 4: UnloadSeg((void *)gopher_main); break;
    case 5: UnloadSeg((void *)ftp_main); break;
    case 6: UnloadSeg((void *)gif_main); break;
    case 7: UnloadSeg((void *)jpeg_main); break;
    case 8: UnloadSeg((void *)netpbm_main); break;
    case 9: UnloadSeg((void *)gzip_main); break;
    }
    if (p->code) { HUnlock(p->code); ReleaseResource(p->code); p->code = NULL; }
    if (p->res > 0) { CloseResFile(p->res); p->res = 0; }
}

/* Load it and open it: false if it can't start */
Boolean plugin_load(Plug *p, MillOpenParams *op)
{
    memset(op, 0, sizeof *op);
    op->interface = kMillInterface;
    op->host = &host;
    if (!p->builtin) {
        short was = CurResFile();
        if ((p->res = file_open_res(&p->spec, fsRdPerm)) == -1) { p->res = 0; return false; }
        UseResFile(p->res);
        p->code = Get1IndResource('MPlg', 1);
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
    MillRequest *req;           /* the Plug-In's view, for ProxyRequest to pass on the login */            /* ProxyRequest answered: never the Plug-In's own page */
    NetConn *conns[CONNS];
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

static OSErr run(Plug *p, short protocol, const Url *u, const char *url, const char *headers, const char *content_type,
                 Body *body, net_sink sink, void *ref, const char *from_owner, char owner[32],
                 short forward, Boolean *wrote, char *key)
{
    struct MillRequestRef rr;
    MillRequest r;
    MillReqRef was = current;
    char *text;
    long n;
    short i;
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
    if (key && r.loginKey[0]) snprintf(key, AUTH_KEYS, "%s", r.loginKey);
    for (i = 0; i < CONNS; i++) if (rr.conns[i]) net_tcp_close(rr.conns[i]);
    free(text);
    done_with(p);
    if (wrote) *wrote = rr.wrote;
    if (err == PLUGIN_FAILED || err == PLUGIN_ASKED) return err;
    /* failing before any answer (couldn't connect, say): Mill says why, as for the web */
    if (err != noErr && err != millUnknown && !rr.wrote) return err == paramErr ? ioErr : err;
    return err == userCanceledErr ? err : noErr;
}

OSErr plugin_request(const Url *u, const char *url, const char *headers, const char *content_type,
                     Body *body, net_sink sink, void *ref,
                     const char *from_owner, char owner[32], char key[AUTH_KEYS])
{
    short protocol;
    Plug *p = route(u, &protocol);
    owner[0] = 0;
    OSErr err;
    key[0] = 0;
    given_n = 0;
    used_key[0] = 0;
    if (!p) return paramErr;
    tidy_until = 0;
    err = run(p, protocol, u, url, headers, content_type, body, sink, ref, from_owner, owner, 0, NULL, key);
    tidy_until = 0;
    return err;
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
    a[0] = (done + 1023) / 1024;
    a[1] = (total + 1023) / 1024;
    mill_format(msg, sizeof msg, total > 0 ? "Reading... %ldK of %ldK" : "Reading... %ldK", a);
    h_status(r, msg);
}

static pascal short h_busy(MillReqRef r)
{
    (void)r;
    return net_idle && net_idle();
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
    if (err == userCanceledErr && !tidy_until) tidy_until = TickCount() + TIDY_TICKS;
    return err;
}

static pascal OSErr h_tcp_accept(MillConn conn) { return tidy(net_tcp_accept((NetConn *)conn)); }
static pascal OSErr h_tcp_send(MillConn conn, const void *data, long len) { return tidy(net_tcp_send((NetConn *)conn, data, len)); }
static pascal OSErr h_tcp_recv(MillConn conn, void *buf, long *len) { return tidy(net_tcp_recv((NetConn *)conn, buf, len)); }

static pascal void h_tcp_close(MillConn conn)
{
    short i;
    if (!current || !conn) return;
    for (i = 0; i < CONNS; i++) {
        if (current->conns[i] == (NetConn *)conn) { current->conns[i] = NULL; net_tcp_close((NetConn *)conn); }
    }
}

static short run_write(struct MillFormatRun *f, const void *data, long len);

static pascal short h_write(MillReqRef r, const void *data, long len)
{
    if (r->kind == 1) return run_write((struct MillFormatRun *)r, data, len);
    if (r->stopped) return 0;
    if (len > 0) r->wrote = true;
    if (len > 0 && !r->sink(data, len, r->ref)) r->stopped = true;
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
              r->sink, r->ref, "", owner, 1, &wrote, r->req->loginKey);
    if (err == millNoProxy && !wrote) return millNoProxy;
    if (err == PLUGIN_ASKED) {
        /* the site wants a login: the request ends, as if its own Plug-In asked */
        r->asked = r->stopped = true;
        err = userCanceledErr;
    }
    r->proxied = true;
    if (wrote) r->wrote = true;
    return err;
}

/* The settings a protocol lists say what each is until it's changed */
static const MillSetting *listed(const char *scheme, const char *key)
{
    PlugProto *pp = plugin_proto(scheme);
    short k;
    if (pp) for (k = 0; k < pp->nset; k++) if (!strcmp(pp->sets[k].key, key)) return &pp->sets[k];
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
    snprintf(failure, sizeof failure, "%s", text && *text ? text : "The request failed.");
    r->failed = true;
    r->stopped = true;      /* Write takes nothing more */
}

static PluginAsked asked;
const PluginAsked *plugin_asked(void) { return &asked; }

static pascal void h_ask_login(MillReqRef r, const char *prompt, const char *name_label, const char *pass_label, const char *key,
                               const char *name, const char *pass)
{
    short i;
    if (r->kind == 1) return;
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
    host.Write = h_write;
    host.ProxyRequest = h_proxy_request;
    host.GetSetting = h_get_setting;
    host.SetSetting = h_set_setting;
    host.Format = h_format;
    host.Printf = h_printf;
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
    host.TypeForName = h_type_for_name;
    host.TypeForFinderType = h_type_for_finder;
    host.ReadBody = h_read_body;
    host.NextPart = h_next_part;
    host.Fail = h_fail;
    host.AskLogin = h_ask_login;
    host.SavedLogin = h_saved_login;
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
            if (!(f->gives & wants) || !format_enabled(f)) continue;
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
                if (listed_in(plugs[i].formats[k].extensions, ext, n)) return first_word(plugs[i].formats[k].types, out, size);
    }
    return first_word("application/octet-stream", out, size);
}

long type_for_finder(OSType t, char *out, long size)
{
    short i, k;
    if (t == 'TEXT') return first_word("text/plain", out, size);
    for (i = 0; i < nplugs; i++)
        for (k = 0; plugs[i].status == PLUG_READY && k < plugs[i].nformat; k++)
            if (listed_in(plugs[i].formats[k].fileTypes, (const char *)&t, 4)) return first_word(plugs[i].formats[k].types, out, size);
    return first_word("application/octet-stream", out, size);
}

#define PICTURE_SPARE (24L * 1024)    /* memory a picture leaves for the rest of Mill */

struct MillFormatRun {
    short kind;                 /* 1: a format's run (Write tells it from a request) */
    PlugFormat *f;
    MillFormatParams prm;
    char type[80], url[300];
    ImageCanvas *canvas;
    short dw, dh, photo, aw, ah;
    long wants;
    struct MillFormatRun *next;     /* where its stream goes on to */
    fmt_out out;                    /* or, at the end of the chain, here */
    void *out_ref;
    Boolean stopped;
    char final[64];
};

static void free_run(struct MillFormatRun *r)
{
    if (!r) return;
    canvas_free(r->canvas);
    done_with(r->f->plug);
    free(r);
}

/* Start one format; then, for a stream, the format it goes on to */
static struct MillFormatRun *new_run(PlugFormat *f, const char *type, const char *forced, const char *url, short aw, short ah,
                                     long wants, fmt_out out, void *out_ref)
{
    struct MillFormatRun *r = calloc(1, sizeof *r);
    PlugFormat *next;
    char inner[300];
    if (!r) return NULL;
    if (!use(f->plug)) { free(r); return NULL; }
    r->kind = 1;
    r->f = f;
    r->aw = aw;
    r->ah = ah;
    r->wants = wants;
    snprintf(r->type, sizeof r->type, "%s", type);
    snprintf(r->url, sizeof r->url, "%s", url ? url : "");
    r->prm.ref = r;
    r->prm.format = (short)(f - f->plug->formats);
    r->prm.type = r->type;
    r->prm.url = r->url;
    r->prm.width = aw > 0 ? aw : 0;
    r->prm.height = ah > 0 ? ah : 0;
    r->prm.wants = wants | millGivesStream;
    r->out = out;
    r->out_ref = out_ref;
    if (call(f->plug, millFormatBegin, &r->prm) != noErr) { free_run(r); return NULL; }
    if (forced) snprintf(r->prm.outType, sizeof r->prm.outType, "%s", forced);
    if (f->gives & millGivesStream && r->prm.outType[0]) {
        /* what it gives goes on to the format for that type, by name without .gz too */
        char *dot;
        snprintf(inner, sizeof inner, "%s", r->url);
        inner[strcspn(inner, "?#")] = 0;
        if ((dot = strrchr(inner, '.')) && !strchr(dot, '/')) *dot = 0;
        if ((next = format_for(r->prm.outType, inner, 0, wants)) != NULL && next != f)
            r->next = new_run(next, r->prm.outType, NULL, inner, aw, ah, wants, out, out_ref);
        if (!r->next) snprintf(r->final, sizeof r->final, "%s", r->prm.outType);
    } else if (f->gives & millGivesElements && wants & millGivesElements) {
        strcpy(r->final, "text/html");
    }
    return r;
}

FmtRun *format_begin(PlugFormat *f, const char *type, const char *encoding, const char *url, short aw, short ah,
                     long wants, fmt_out out, void *out_ref)
{
    if (encoding && same_ci(encoding, "gzip", 4)) {
        /* Content-Encoding: the gzip format first, giving the reply's own type */
        PlugFormat *g = format_for("application/gzip", NULL, 0, millGivesStream);
        return g ? (FmtRun *)new_run(g, "application/gzip", type, url, aw, ah, wants, out, out_ref) : NULL;
    }
    return f ? (FmtRun *)new_run(f, type, NULL, url, aw, ah, wants, out, out_ref) : NULL;
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
    OSErr err;
    if (r->stopped) return 1;
    r->prm.data = p;
    r->prm.len = n;
    err = call(r->f->plug, millFormatData, &r->prm);
    if (err == millDone || r->stopped) return 1;
    return err == noErr ? 0 : -1;
}

static void run_stop(struct MillFormatRun *r) { r->stopped = true; }

static short run_write(struct MillFormatRun *r, const void *data, long len)
{
    if (r->stopped || len <= 0) return !r->stopped;
    if (r->next) { if (format_data((FmtRun *)r->next, data, len) != 0) r->stopped = true; }
    else if (r->out && r->final[0]) { if (!r->out(data, len, r->out_ref)) r->stopped = true; }
    return !r->stopped;
}

int format_end(FmtRun *run, short cut, Bitmap1 *pic)
{
    struct MillFormatRun *r = (struct MillFormatRun *)run, *next;
    int got = -1;
    if (pic) memset(pic, 0, sizeof *pic);
    while (r) {
        r->prm.cut = cut;
        r->prm.data = NULL;
        r->prm.len = 0;
        call(r->f->plug, millFormatEnd, &r->prm);   /* a stream may give its last bytes now */
        if (r->canvas && pic && got != 0) got = canvas_finish(r->canvas, r->photo, pic);
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
