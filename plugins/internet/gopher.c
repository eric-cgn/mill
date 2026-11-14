/* gopher: (RFC 1436) in Internet Services: menus become pages, text and files come
   through as they are, and searches (type 7) use ISINDEX. */
#include <Multiverse.h>
#include <string.h>

#include "MillPlugIn.h"

#define LINE 1024

typedef struct {
    MillPlugIn base;
    MillReqRef r;
    short stopped;
    char line[LINE];
    char esc[LINE * 2];
    char work[LINE];
    short images;           /* the "Show images inline" setting */
} GopherPlugIn;

#define H (me->base.host)

static void out(GopherPlugIn *me, const char *s, long n)
{
    if (!me->stopped && n > 0 && !H->Write(me->r, s, n)) me->stopped = 1;
}

static void out_s(GopherPlugIn *me, const char *s) { out(me, s, strlen(s)); }

/* Server text, as Mac Roman and safe in HTML */
static void out_text(GopherPlugIn *me, const char *s)
{
    if (H->ToMacRoman(s, strlen(s), millGuess, me->work, sizeof me->work) >= 0 &&
        H->HtmlEscape(me->work, me->esc, sizeof me->esc) >= 0) out_s(me, me->esc);
}

static long number(const char *s)
{
    long n = 0;
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return n;
}

static void icon(GopherPlugIn *me, short id)
{
    long a[1];
    a[0] = id;
    H->Printf(me->r, "<IMG SRC=\"plugin://internet/rsrc?type=ics%%23&id=%d\" WIDTH=16 HEIGHT=16 ALIGN=TOP> ", a);
}

static void header(GopherPlugIn *me, const char *type)
{
    long a[1];
    a[0] = (long)type;
    H->Printf(me->r, "HTTP/1.0 200 OK\r\nContent-Type: %s\r\n\r\n", a);
}

/* One menu line: type, then display, selector, host, and port, between tabs */
static void menu_line(GopherPlugIn *me, char *l)
{
    char type = l[0], *f[4], url[LINE + 64], sel[LINE];
    short n = 0, ic;
    long a[4];
    if (!type || (type == '.' && !l[1])) return;
    l++;
    f[0] = l;
    while (n < 3 && (l = strchr(l, '\t'))) { *l++ = 0; f[++n] = l; }
    if ((l = strchr(f[n], '\t'))) *l = 0;   /* Gopher+ fields after the port */
    if (type == 'i' || type == '3' || n < 3) {
        /* information, or an error the server put in the menu */
        if (type == '3') icon(me, kMillIconDocument);
        else out_s(me, "    ");     /* where a link's icon is, so ASCII art lines up */
        out_text(me, f[0]);
        out_s(me, "\n");
        return;
    }
    if (type == '8' || type == 'T' || type == '2') return;  /* telnet and CSO: nothing Mill can do */
    if (type == 'h' && !strncmp(f[1], "URL:", 4)) {
        /* a link out of gopherspace */
        if (H->Format(url, sizeof url, "%s", (a[0] = (long)(f[1] + 4), a)) >= (long)sizeof url) return;
        ic = kMillIconLink;
    } else {
        long port = number(f[3]);
        if (H->UrlEncode(f[1], sel, sizeof sel) < 0) return;
        a[0] = (long)f[2];
        if (port == 70 || port == 0) { a[1] = type; a[2] = (long)sel; H->Format(url, sizeof url, "gopher://%s/%c%s", a); }
        else { a[1] = port; a[2] = type; a[3] = (long)sel; H->Format(url, sizeof url, "gopher://%s:%d/%c%s", a); }
        ic = type == '1' ? kMillIconFolder : type == '0' ? kMillIconText : type == '7' ? kMillIconSearch :
             (type == 'g' || type == 'I' || type == 'p') ? kMillIconPicture : type == 'h' ? kMillIconDocument : kMillIconBinary;
    }
    if (H->HtmlEscape(url, sel, sizeof sel) < 0) sel[0] = 0;     /* the address, for HTML (out_text uses esc) */
    if (me->images && ic != kMillIconLink && ic != kMillIconFolder && ic != kMillIconSearch &&
        (type == 'g' || type == 'I' ||
         (H->TypeForName(f[1], me->work, sizeof me->work) > 6 && !memcmp(me->work, "image/", 6)))) {
        /* the picture itself, a link to it, on a line of its own above the item's */
        out_s(me, "    <A HREF=\"");
        out_s(me, sel);
        out_s(me, "\"><IMG SRC=\"");
        out_s(me, sel);
        out_s(me, "\" ALT=\"");
        out_text(me, f[0][0] ? f[0] : f[1]);
        out_s(me, "\"></A>\n");
    }
    icon(me, ic);
    out_s(me, "<A HREF=\"");
    out_s(me, sel);
    out_s(me, "\">");
    out_text(me, f[0][0] ? f[0] : f[1]);
    out_s(me, "</A>\n");
}

/* What a file is, from its item type and its name (Mill's table and its formats') */
static const char *content_type(const MillHost *h, char type, const char *sel, char *out, long size)
{
    if (type == 'g') return "image/gif";
    if (type == 'h') return "text/html";
    if (type == '0') return "text/plain";
    h->TypeForName(sel, out, size);
    return out;
}

/* A menu's first line is in: an error the server sent instead of a menu (type 3) fails
   the request with its words; else the page starts. 1 to go on. */
static short start_menu(GopherPlugIn *me, MillRequest *r)
{
    long a[1];
    if (me->line[0] == '3') {
        char text[LINE + 16], *tab = strchr(me->line, '\t');
        if (tab) *tab = 0;
        a[0] = (long)(me->line + 1);
        H->Format(text, sizeof text, "Gopher: %s", a);
        H->Fail(me->r, text);
        me->stopped = 1;
        return 0;
    }
    H->PlugInPage(me->r);
    header(me, "text/html; charset=macintosh");
    a[0] = (long)r->host;
    H->Printf(me->r, "<TITLE>Gopher: %s</TITLE>", a);
    out_s(me, "<PRE><FONT SIZE=1>");     /* Courier 9: 80 columns fit the window */
    me->images = H->GetSetting("gopher", "images", me->work, sizeof me->work) == 3 && !strcmp(me->work, "yes");
    return 1;
}

OSErr gopher_request(MillPlugIn *self, MillRequest *r)
{
    GopherPlugIn *me = (GopherPlugIn *)self;
    char type = '1', *sel = me->line, *query = NULL, *p;
    char buf[1024];
    MillConn c;
    OSErr err;
    long n;
    short menu, text, at = 0, started = 0;
    char tail[10];
    short kept = 0;
    me->r = r->ref;
    me->stopped = 0;
    /* /Tselector, %-encoded; a search's words come after '?' (from ISINDEX) or a tab */
    if (r->path[1]) { type = r->path[1]; strncpy(sel, r->path + 2, LINE - 3); sel[LINE - 3] = 0; }
    else sel[0] = 0;
    if (type == '7' && (p = strchr(sel, '?'))) { *p = 0; query = p + 1; }
    H->UrlDecode(sel, sel, LINE);
    if (query) {
        for (p = query; *p; p++) if (*p == '+') *p = ' ';
        H->UrlDecode(query, query, LINE);
    } else if (type == '7' && (p = strchr(sel, '\t'))) { *p = 0; query = p + 1; }
    if (type == '7' && !query) {
        /* ask for the words, and come back here with them */
        H->PlugInPage(me->r);
        header(me, "text/html");
        out_s(me, "<TITLE>Search</TITLE><ISINDEX PROMPT=\"Search for: \">");
        return noErr;
    }
    menu = type == '1' || type == '7';
    text = type == '0';
    if (H->GetSetting("gopher", "proxy", buf, sizeof buf) == 3 && !strcmp(buf, "yes") &&
        (err = H->ProxyRequest(me->r)) != millNoProxy) return err;
    if ((err = H->TcpOpen(me->r, r->host, r->port ? r->port : 70, &c)) != noErr) return err;
    n = strlen(sel);
    if (query) { sel[n++] = '\t'; strcpy(sel + n, query); n += strlen(query); }
    sel[n++] = '\r';
    sel[n++] = '\n';
    if ((err = H->TcpSend(c, sel, n)) != noErr) { H->TcpClose(c); return err; }
    sel[n - 2] = 0;
    if (!menu) header(me, content_type(H, type, sel, me->esc, sizeof me->esc));
    me->line[0] = 0;
    for (;;) {
        long i;
        n = sizeof buf;
        if ((err = H->TcpRecv(c, buf, &n)) != noErr || n == 0 || me->stopped) break;
        if (menu) {
            /* whole lines, which may arrive in pieces */
            for (i = 0; i < n; i++) {
                if (buf[i] == '\n') {
                    if (at && me->line[at - 1] == '\r') at--;
                    me->line[at] = 0;
                    if (!started && !(started = start_menu(me, r))) break;
                    menu_line(me, me->line);
                    at = 0;
                } else if (at < LINE - 1) me->line[at++] = buf[i];
            }
        } else if (text) {
            /* all but the last few bytes, which may be the ".\r\n" that ends it */
            if (n >= 5) {
                out(me, tail, kept);
                out(me, buf, n - 5);
                memcpy(tail, buf + n - 5, 5);
                kept = 5;
            } else {
                memcpy(tail + kept, buf, n);
                kept += n;
                if (kept > 5) { out(me, tail, kept - 5); memmove(tail, tail + kept - 5, 5); kept = 5; }
            }
        } else out(me, buf, n);
    }
    if (!me->stopped && err != noErr && err != userCanceledErr) {
        H->Fail(me->r, "Gopher: The connection was lost.");
        H->TcpClose(c);
        return noErr;
    }
    if (menu && at && !me->stopped) {
        me->line[at] = 0;
        if (started || (started = start_menu(me, r)) != 0) menu_line(me, me->line);
    }
    if (menu && !started && !me->stopped) start_menu(me, r);     /* an empty menu */
    if (text) {
        if (kept >= 3 && tail[kept - 1] == '\n' && tail[kept - 2] == '\r' && tail[kept - 3] == '.') kept -= 3;
        else if (kept >= 2 && tail[kept - 1] == '\n' && tail[kept - 2] == '.') kept -= 2;
        out(me, tail, kept);
    }
    if (menu && started) out_s(me, "</FONT></PRE>");
    H->TcpClose(c);
    return err == userCanceledErr ? err : noErr;
}

static const MillSetting settings[] = {
    { "images", "Show images inline", "Picture items in menus as pictures, above their links.", millCheckbox, "no", 0, 0, NULL },
    kMillProxySetting
};
static const MillProtocol protocols[] = { { "gopher", "Gopher", "Internet Gopher Protocol (RFC 1436)", 2, settings } };

/* Internet.c's: the protocol's description and settings, and its state for a request (the caller
   sets base.host, and frees it with FreeMem) */
const MillProtocol *gopher_protocol(void) { return protocols; }

MillPlugIn *gopher_state(const MillHost *host)
{
    GopherPlugIn *me = host->NewMem(sizeof *me);
    if (!me) return NULL;
    me->base.methods = NULL;
    me->base.host = host;
    return &me->base;
}
