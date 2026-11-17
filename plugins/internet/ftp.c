/* ftp: (RFC 959, addresses as RFC 1738) in Internet Services: folders become pages,
   files come through as they are, and with a name and password, files can be sent up
   with a form. Passive mode by default; active mode (PORT) waits for the server. The
   control connection is kept between pages (TcpKeep), logged in and in the folder last
   shown; a data connection is made for each transfer. A Range is a SIZE, then REST and
   RETR (RFC 959 4.1.3; SIZE and REST in stream mode as most servers have them), closing
   the data connection once the part has come. */
#include <Multiverse.h>
#include <string.h>

#include "MillPlugIn.h"

#define CTL 512
#define NAME 256
#define NOTES 2048              /* server messages kept for the page, at most */

typedef struct {
    MillPlugIn base;
    MillReqRef r;
    short stopped;
    MillConn ctl;
    char in[CTL];           /* what's come on the control connection and not been read */
    short have;
    char msg[CTL];          /* the last reply's text */
    char notes[NOTES];      /* messages from the login and folder replies (230, 250) */
    short nlen;
    char path[NAME * 2];    /* as in the address, %-encoded */
    short clean;            /* the control connection is at rest: every reply read */
    char home[NAME * 2];    /* the folder logging in put it in (PWD), "" if not known */
    char at[NAME * 2];      /* the folder it's in now: the address's path, %-encoded */
    char shown[NAME * 2];   /* decoded, for the page */
    char work[NAME * 2], esc[NAME * 4];
} FtpPlugIn;

#define H (me->base.host)

static void out(FtpPlugIn *me, const char *s, long n)
{
    if (!me->stopped && n > 0 && !H->Write(me->r, s, n)) me->stopped = 1;
}

static void out_s(FtpPlugIn *me, const char *s) { out(me, s, strlen(s)); }

static void out_text(FtpPlugIn *me, const char *s)
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

static void page_head(FtpPlugIn *me, short code, const char *why, const char *type)
{
    long a[3];
    a[0] = code;
    a[1] = (long)why;
    a[2] = (long)type;
    H->Printf(me->r, "HTTP/1.0 %d %s\r\nContent-Type: %s\r\n", a);
}

/* The request failed: Mill says so in an alert, with the server's own words (its last
   reply) after prefix, a period added if it has none */
static void fail(FtpPlugIn *me, const char *prefix)
{
    char text[CTL + 64];
    long a[2], n;
    a[0] = (long)prefix;
    a[1] = (long)(me->msg[0] ? me->msg : "No reply.");
    n = H->Format(text, sizeof text - 1, "%s%s", a);
    if (n > 0 && n < (long)sizeof text - 1 && text[n - 1] != '.') { text[n++] = '.'; text[n] = 0; }
    H->Fail(me->r, text);
    me->stopped = 1;
}

/* ------------------------------------------------------------ the control connection */

/* One reply: its code (0 if the connection failed), with its last line's text in msg */
static short reply(FtpPlugIn *me)
{
    char line[CTL];
    short at = 0, code = 0, mark = me->nlen;
    for (;;) {
        char *nl = memchr(me->in, '\n', me->have);
        if (!nl) {
            long n = CTL - me->have;
            if (n <= 0) me->have = 0, n = CTL;    /* a line too long: dropped */
            if (H->TcpRecv(me->ctl, me->in + me->have, &n) != noErr || n == 0) { me->clean = 0; return 0; }
            me->have += (short)n;
            continue;
        }
        at = (short)(nl - me->in);
        memcpy(line, me->in, at);
        line[at] = 0;
        if (at && line[at - 1] == '\r') line[at - 1] = 0;
        me->have -= at + 1;
        memmove(me->in, nl + 1, me->have);
        /* "123-" goes on; "123 " (or just "123") is the last line */
        if (line[0] >= '1' && line[0] <= '5' && line[1] >= '0' && line[1] <= '9' && line[2] >= '0' && line[2] <= '9' &&
            line[3] != '-' && (!code || number(line) == code)) {
            strcpy(me->msg, line[3] ? line + 4 : "");
            /* the lines before the last are the server's message (wu-ftpd's welcome.msg
               and .message): kept for the page from logging in and changing folder only */
            code = (short)number(line);
            if (code != 230 && code != 250) me->nlen = mark;
            return code;
        }
        if (!code && line[3] == '-') code = (short)number(line);
        {
            /* a message line, without its "230-" */
            const char *t = line;
            short n;
            if (t[0] >= '1' && t[0] <= '5' && t[1] >= '0' && t[1] <= '9' && t[2] >= '0' && t[2] <= '9' && t[3] == '-') t += 4;
            n = (short)strlen(t);
            if (me->nlen + n + 1 < NOTES) {
                memcpy(me->notes + me->nlen, t, n);
                me->nlen += n;
                me->notes[me->nlen++] = '\n';
            }
        }
    }
}

/* Send a command (CR LF added), and read its reply */
static short command(FtpPlugIn *me, const char *fmt, const char *arg)
{
    char c[NAME + 16];
    long a[1], n;
    a[0] = (long)arg;
    n = H->Format(c, sizeof c - 2, fmt, a);
    if (n >= (long)sizeof c - 2) return 0;
    c[n++] = '\r';
    c[n++] = '\n';
    if (H->TcpSend(me->ctl, c, n) != noErr) { me->clean = 0; return 0; }
    return reply(me);
}

/* Into the folder name (decoded) from the one it's in: at follows */
static void enter(FtpPlugIn *me, const char *name)
{
    long n = strlen(me->at);
    if (H->UrlEncode(name, me->at + n, sizeof me->at - n - 1) < 0 || strchr(name, '/')) me->home[0] = 0;  /* lost track */
    else strcat(me->at, "/");
}

/* ------------------------------------------------------------ the data connection */

/* Ready a data connection, before the command that uses it: passive (the server says
   where) or active (Mill waits, and tells it where) */
static OSErr data_open(FtpPlugIn *me, MillRequest *r, short active, MillConn *d)
{
    OSErr err;
    char *p, arg[32];
    long a[6], k;
    *d = 0;
    if (active) {
        unsigned long addr;
        unsigned short port;
        if ((err = H->TcpListen(me->r, &addr, &port, d)) != noErr) return err;
        a[0] = addr >> 24 & 255; a[1] = addr >> 16 & 255; a[2] = addr >> 8 & 255; a[3] = addr & 255;
        a[4] = port >> 8; a[5] = port & 255;
        H->Format(arg, sizeof arg, "%d,%d,%d,%d,%d,%d", a);
        if (command(me, "PORT %s", arg) / 100 != 2) { H->TcpClose(*d); *d = 0; return ioErr; }
        return noErr;
    }
    if (command(me, "PASV", "") != 227) return ioErr;
    /* "Entering Passive Mode (h1,h2,h3,h4,p1,p2)": the port is all that's used; the
       address is the server's own, as a server behind a router may give another */
    for (p = me->msg; *p && (*p < '0' || *p > '9'); p++) {}
    for (k = 0; k < 6 && *p; k++) {
        a[k] = number(p);
        while (*p >= '0' && *p <= '9') p++;
        while (*p && (*p < '0' || *p > '9')) p++;
    }
    if (k < 6) return ioErr;
    return H->TcpOpen(me->r, r->host, (unsigned short)(a[4] * 256 + a[5]), d);
}

/* After the command that started the transfer: an active connection now comes */
static OSErr data_start(FtpPlugIn *me, short active, MillConn d)
{
    (void)me;
    return active ? H->TcpAccept(d) : noErr;
}

/* ------------------------------------------------------------ pages */

static void icon(FtpPlugIn *me, short id)
{
    long a[1];
    a[0] = id;
    H->Printf(me->r, "<IMG SRC=\"plugin://internet-services/rsrc?type=ics%%23&id=%d\" WIDTH=16 HEIGHT=16 ALIGN=TOP> ", a);
}

static short icon_for(const char *name)
{
    const char *dot = strrchr(name, '.');
    char e[9];
    short i;
    for (i = 0; i < 8 && dot && dot[i + 1]; i++) e[i] = (char)(dot[i + 1] | 32);
    e[i] = 0;
    /* text by name, as Mill's table has it: .txt, .msg, .message, .asc, HTML, and README */
    if (!strcmp(e, "txt") || !strcmp(e, "msg") || !strcmp(e, "message") || !strcmp(e, "asc") || !strcmp(e, "html") ||
        !strcmp(e, "htm") || !strcmp(name, "README")) return kMillIconText;
    if (!strcmp(e, "gif") || !strcmp(e, "jpg") || !strcmp(e, "jpeg") || !strcmp(e, "pict") ||
        !strcmp(e, "pbm") || !strcmp(e, "pgm") || !strcmp(e, "ppm") || !strcmp(e, "pnm")) return kMillIconPicture;
    if (!strcmp(e, "hqx") || !strcmp(e, "sit") || !strcmp(e, "bin") || !strcmp(e, "zip") || !strcmp(e, "cpt") ||
        !strcmp(e, "sea") || !strcmp(e, "gz") || !strcmp(e, "tar") || !strcmp(e, "z")) return kMillIconBinary;
    return kMillIconDocument;
}

/* What a file is, by its name (Mill's table and its formats') */
static const char *content_type(FtpPlugIn *me, const char *name)
{
    H->TypeForName(name, me->work, sizeof me->work);
    return me->work;
}

/* One line of LIST, in Unix's form or DOS's, as a line of the page; anything else is
   shown as it is */
static void list_line(FtpPlugIn *me, char *l)
{
    char *f[9], *name = NULL, href[NAME * 3];
    short n = 0, folder = 0, link = 0;
    long size = -1, a[1];
    char *p = l;
    if (!*l || !strncmp(l, "total ", 6)) return;
    while (n < 9 && *p) {
        while (*p == ' ') p++;
        if (!*p) break;
        f[n++] = p;
        if (n == 9) break;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    if (n == 9 && strchr("-dlbcps", l[0]) && (f[0][1] == 'r' || f[0][1] == '-')) {
        /* drwxr-xr-x 2 owner group 4096 Jan  1 2020 name */
        name = f[8];
        folder = l[0] == 'd';
        link = l[0] == 'l';
        size = number(f[4]);
        if (link && (p = strstr(name, " -> "))) *p = 0;
    } else if (n >= 4 && f[0][2] == '-' && f[1][2] == ':') {
        /* 01-31-97  12:00PM  <DIR>  name, or a size in the place of <DIR> */
        folder = !strcmp(f[2], "<DIR>");
        if (!folder) size = number(f[2]);
        name = f[3];
        for (p = name; p < f[n - 1]; p++) if (!*p) *p = ' ';  /* names with spaces */
    }
    if (!name) {
        for (p = l; n > 1 && p < f[n - 1]; p++) if (!*p) *p = ' ';
        out_s(me, "&nbsp; &nbsp; &nbsp;");
        out_text(me, l);
        out_s(me, "<BR>");
        return;
    }
    if (!strcmp(name, ".") || !strcmp(name, "..")) return;
    if (H->UrlEncode(name, href, sizeof href - 2) < 0) return;
    if (folder) strcat(href, "/");
    icon(me, folder ? kMillIconFolder : link ? kMillIconLink : icon_for(name));
    out_s(me, "<A HREF=\"");
    out_s(me, href);
    out_s(me, "\">");
    out_text(me, name);
    out_s(me, "</A>");
    if (!folder && !link && size >= 0) { a[0] = (size + 1023) / 1024; H->Printf(me->r, " %ldK", a); }
    out_s(me, "<BR>");
}

/* The folder, already the current one: a page of its files */
static OSErr listing(FtpPlugIn *me, MillRequest *r, short active, short upload, const char *note, short show_notes)
{
    MillConn d;
    OSErr err;
    char buf[1024], line[NAME * 2];
    long n, i;
    short at = 0, code;
    if ((err = data_open(me, r, active, &d)) != noErr) { fail(me, "FTP: "); return noErr; }
    code = command(me, "LIST", "");
    if (code != 125 && code != 150) { H->TcpClose(d); fail(me, "FTP: "); return noErr; }
    if ((err = data_start(me, active, d)) != noErr) { me->clean = 0; return err; }
    H->PlugInPage(me->r);
    page_head(me, 200, "OK", "text/html; charset=macintosh");
    out_s(me, "\r\n<TITLE>FTP: ");
    out_text(me, r->host);
    out_s(me, "</TITLE><H2>");
    out_text(me, r->host);
    out_text(me, me->shown);
    out_s(me, "</H2>");
    if (note) { out_s(me, "<P><B>"); out_text(me, note); out_s(me, "</B>"); }
    if (me->nlen && show_notes) {
        /* the server's messages, a line at a time */
        char *p = me->notes, *e;
        out_s(me, "<PRE><FONT SIZE=2>");     /* 10 points: 80 columns fit a Plus's window */
        while (p < me->notes + me->nlen && (e = memchr(p, '\n', me->notes + me->nlen - p))) {
            *e = 0;
            out_text(me, p);
            out_s(me, "\n");
            p = e + 1;
        }
        out_s(me, "</FONT></PRE>");
    }
    out_s(me, "<P>");
    if (me->path[1]) { icon(me, kMillIconParent); out_s(me, "<A HREF=\"..\">Up</A><BR>"); }
    for (;;) {
        n = sizeof buf;
        if ((err = H->TcpRecv(d, buf, &n)) != noErr || n == 0 || me->stopped) break;
        for (i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                if (at && line[at - 1] == '\r') at--;
                line[at] = 0;
                list_line(me, line);
                at = 0;
            } else if (at < (short)sizeof line - 1) line[at++] = buf[i];
        }
    }
    if (at) { line[at] = 0; list_line(me, line); }
    H->TcpClose(d);
    if (err != noErr && err != userCanceledErr) { me->clean = 0; strcpy(me->msg, "The connection was lost."); fail(me, "FTP: "); return noErr; }
    if (err == noErr && !me->stopped) reply(me);
    else me->clean = 0;
    if (upload) out_s(me, "<FORM METHOD=POST ENCTYPE=\"multipart/form-data\"><HR><INPUT TYPE=file NAME=file> "
                          "<INPUT TYPE=submit VALUE=\"Upload\"></FORM>");
    return err == userCanceledErr ? err : noErr;
}

static OSErr retrieve(FtpPlugIn *me, MillRequest *r, short active, const char *name)
{
    MillConn d;
    OSErr err;
    char buf[1024];
    long n, a[1], size = -1, got = 0, first = 0, last = -1, want = -1;
    short code, ranged, part = 0, rest = 1;
    char *p;
    /* a part: the file's size first, to know which bytes */
    ranged = H->Header(me->r, "Range", buf, sizeof buf) > 0;
    if (ranged && command(me, "SIZE %s", name) == 213 && me->msg[0] >= '0' && me->msg[0] <= '9') size = number(me->msg);
    if (ranged) part = H->FileHead(me->r, NULL, size, &first, &last);
    if ((err = data_open(me, r, active, &d)) != noErr) { fail(me, "FTP: "); return noErr; }
    /* REST just before RETR, which then starts there (RFC 959 4.1.3) */
    if (part) {
        a[0] = first;
        H->Format(buf, sizeof buf, "%ld", a);
        if (command(me, "REST %s", buf) != 350) part = rest = 0;
    }
    code = command(me, "RETR %s", name);
    if (code != 125 && code != 150) {
        H->TcpClose(d);
        /* a folder asked for without its slash: there, with one */
        if (command(me, "CWD %s", name) / 100 == 2) {
            enter(me, name);
            page_head(me, 301, "Moved", "text/html");
            a[0] = (long)r->url;
            H->Printf(me->r, "Location: %s/\r\n\r\n", a);
        } else fail(me, "FTP: ");
        return noErr;
    }
    if ((err = data_start(me, active, d)) != noErr) { me->clean = 0; return err; }
    /* "150 Opening BINARY mode data connection for x (1234 bytes)" */
    if (!ranged && (p = strrchr(me->msg, '(')) && p[1] >= '0' && p[1] <= '9') size = number(p + 1);
    if (rest) {
        /* the part (206), or the whole with its size (200): the same bytes FileHead said */
        part = H->FileHead(me->r, content_type(me, name), size, &first, &last);
        if (part) want = last - first + 1;
    } else {
        /* the server refused REST: the whole, and it can't be read in parts */
        page_head(me, 200, "OK", content_type(me, name));
        if (size >= 0) { a[0] = size; H->Printf(me->r, "Content-Length: %ld\r\n", a); }
        out_s(me, "\r\n");
    }
    if (!part) want = size;
    for (;;) {
        n = sizeof buf;
        if (part && n > want - got) n = want - got;
        if (n == 0 || (err = H->TcpRecv(d, buf, &n)) != noErr || n == 0 || me->stopped) break;
        got += n;
        out(me, buf, n);
    }
    /* a part, once it's all come: the server ends the transfer when its data connection
       closes (426, or 226 if it had sent the rest already) */
    H->TcpClose(d);
    if (err != noErr && err != userCanceledErr) { me->clean = 0; strcpy(me->msg, "The connection was lost."); fail(me, "FTP: "); return noErr; }
    /* all of it (Mill wants no more once it has the size the server gave): the 226 follows */
    if (err == noErr && (!me->stopped || got == want)) reply(me);
    else me->clean = 0;
    return err == userCanceledErr ? err : noErr;
}

/* A file sent with the folder's upload form: STOR it here, as Mill sends it (ReadBody),
   a piece at a time. A TEXT file's line endings become what the settings say. A note
   for the page; NULL if the person stopped; "" if it failed (Fail has said why). */
static const char *store(FtpPlugIn *me, MillRequest *r, short active, char *note, short size)
{
    const char *field, *filename = NULL, *type = NULL, *p;
    char in[1024], out[2048], name[NAME], eol[16];
    long n, o, a[2];
    short code, text, cr = 0;
    MillConn d;
    OSErr err;
    while ((err = H->NextPart(me->r, &field, &filename, &type)) == noErr && !(filename && *filename)) {}
    if (err != noErr) { H->Fail(me->r, "Choose a file to upload."); return ""; }
    /* just its name, not the folders it was in */
    for (p = filename; *p; p++) if (*p == ':' || *p == '/' || *p == '\\') filename = p + 1;
    if (!*filename || strlen(filename) >= NAME) { H->Fail(me->r, "Choose a file to upload."); return ""; }
    strcpy(name, filename);
    if (H->GetSetting("ftp", "eol", eol, sizeof eol) < 0) strcpy(eol, "Unix [LF]");
    text = type && !strcmp(type, "text/plain") && strcmp(eol, "Don\325t convert");
    if (data_open(me, r, active, &d) != noErr) goto refused;
    code = command(me, "STOR %s", name);
    if (code != 125 && code != 150) { H->TcpClose(d); goto refused; }
    if (data_start(me, active, d) != noErr) { me->clean = 0; H->TcpClose(d); goto refused; }
    for (;;) {
        n = sizeof in;
        if ((err = H->ReadBody(me->r, in, &n)) != noErr || n == 0) break;
        if (text) {
            /* CR, LF, or CR LF in, the chosen ending out */
            long k;
            for (k = 0, o = 0; k < n; k++) {
                char c = in[k];
                if (c == '\n' && cr) { cr = 0; continue; }
                cr = c == '\r';
                if (c == '\r' || c == '\n') {
                    if (eol[0] == 'M') out[o++] = '\r';
                    else if (eol[0] == 'U') out[o++] = '\n';
                    else { out[o++] = '\r'; out[o++] = '\n'; }
                } else out[o++] = c;
            }
            if ((err = H->TcpSend(d, out, o)) != noErr) break;
        } else if ((err = H->TcpSend(d, in, n)) != noErr) break;
    }
    H->TcpClose(d);
    if (err == userCanceledErr) {
        /* stopped: what got there is deleted, quietly (Mill gives a few seconds for it) */
        reply(me);
        command(me, "DELE %s", name);
        me->clean = 0;
        return NULL;
    }
    code = reply(me);
    if (err != noErr || code / 100 != 2) goto refused;
    a[0] = (long)name;
    H->Format(note, size, "Uploaded %s.", a);
    return note;
refused:
    /* the server's own words */
    fail(me, "The server refused it: ");
    return "";
}

/* The folder the last PWD reply names (257 "/pub/mac" ...: RFC 959 5.4, quotes doubled
   inside), into home; "" if it can't be read */
static void read_home(FtpPlugIn *me)
{
    const char *p = strchr(me->msg, '"');
    short n = 0;
    me->home[0] = 0;
    if (!p) return;
    for (p++; *p && n < (short)sizeof me->home - 1; p++) {
        if (*p == '"' && p[1] != '"') { me->home[n] = 0; return; }
        if (*p == '"') p++;
        me->home[n++] = *p;
    }
    me->home[0] = 0;
}

/* From the folder it's in to the address's (RFC 1738: from the login's, a CWD for each
   part): further down goes on from where it is; anywhere else starts again from home.
   0 if a CWD failed (with Fail). */
static short go_to(FtpPlugIn *me, const char *folder)
{
    char part[NAME];
    const char *seg, *p;
    long n = strlen(me->at);
    if (strncmp(folder, me->at, n)) {
        if (!me->home[0] || command(me, "CWD %s", me->home) / 100 != 2) { fail(me, "FTP: "); return 0; }
        strcpy(me->at, "/");
        n = 1;
    }
    for (seg = folder + n; (p = strchr(seg, '/')); seg = p + 1) {
        n = p - seg;
        if (n >= NAME) n = NAME - 1;
        memcpy(part, seg, n);
        part[n] = 0;
        H->UrlDecode(part, part, sizeof part);
        if (!part[0]) continue;
        if (command(me, "CWD %s", part) / 100 != 2) { fail(me, "FTP: "); return 0; }
        enter(me, part);
    }
    return 1;
}

OSErr ftp_request(MillPlugIn *self, MillRequest *r)
{
    FtpPlugIn *me = (FtpPlugIn *)self;
    char user[NAME], pass[NAME], anon_pass[NAME], key[NAME + 16], prompt[NAME + 64], setting[8], note[NAME + 16], *seg;
    char kept[kMillNoteMax], folder[NAME * 2];
    const char *said = NULL;
    short code, active, anonymous;
    long a[2];
    long n;
    OSErr err = noErr;
    me->r = r->ref;
    me->stopped = 0;
    me->have = 0;
    me->nlen = 0;
    me->msg[0] = 0;
    if (H->GetSetting("ftp", "proxy", setting, sizeof setting) == 3 && !strcmp(setting, "yes") &&
        (err = H->ProxyRequest(me->r)) != millNoProxy) return err;
    active = H->GetSetting("ftp", "passive", setting, sizeof setting) == 2 && !strcmp(setting, "no");
    /* who: the address's name and password; else the login kept or typed for this
       server (its key: ftp://host:port); else anonymous, by itself or asked first, as
       the settings say */
    if (H->GetSetting("ftp", "anon_password", anon_pass, sizeof anon_pass) < 0) strcpy(anon_pass, "mill@");
    a[0] = (long)r->host;
    a[1] = r->port ? r->port : 21;
    H->Format(key, sizeof key, "ftp://%s:%d", a);
    strcpy(user, r->user);
    strcpy(pass, r->password);
    if (user[0]) {
        /* a name without its password: the one kept for it here */
        if (!pass[0] && H->SavedLogin(key, note, sizeof note, pass, sizeof pass) && strcmp(note, user)) pass[0] = 0;
        strcpy(r->loginKey, key);
    } else if (!H->SavedLogin(key, user, sizeof user, pass, sizeof pass)) {
        if (H->GetSetting("ftp", "anon_auto", setting, sizeof setting) == 2 && !strcmp(setting, "no")) {
            H->Format(prompt, sizeof prompt, "Log in to %s.", a);
            H->AskLogin(me->r, prompt, NULL, NULL, key, "anonymous", anon_pass);
            return noErr;
        }
        strcpy(user, "anonymous");
        strcpy(pass, anon_pass);
    }
    anonymous = !strcmp(user, "anonymous") || !strcmp(user, "ftp");
    /* the path, without ;type= or a query, decoded */
    n = strcspn(r->path, ";?");
    if (n >= (long)sizeof me->path) n = sizeof me->path - 1;
    memcpy(me->path, r->path, n);
    me->path[n] = 0;
    H->UrlDecode(me->path, me->shown, sizeof me->shown);
    me->clean = 1;
    me->at[0] = me->home[0] = 0;
    /* the connection kept from the last page here, as this user: its note is home, a 0, and
       where it is now */
    n = sizeof kept;
    if (H->TcpKept(me->r, user, &me->ctl, kept, &n) == noErr) {
        long h = memchr(kept, 0, n) ? (long)strlen(kept) : -1, rest = n - h - 1;
        if (h > 0 && h < (long)sizeof me->home && rest > 0 && rest < (long)sizeof me->at) {
            strcpy(me->home, kept);
            memcpy(me->at, kept + h + 1, rest);
            me->at[rest] = 0;
            memset(pass, 0, sizeof pass);
            goto in;
        }
        H->TcpClose(me->ctl);       /* a note that wasn't ours */
        me->ctl = 0;
    }
    if ((err = H->TcpOpen(me->r, r->host, r->port ? r->port : 21, &me->ctl)) != noErr) return err;
    H->Status(me->r, "Logging in\311");
    code = reply(me);
    if (code == 220) {
        code = command(me, "USER %s", user);
        if (code == 331) code = command(me, "PASS %s", pass);
    }
    memset(pass, 0, sizeof pass);
    if (code == 530 || code == 331 || code == 332) {
        /* the server's words as the page, if the person cancels; Mill asks */
        r->loginKey[0] = 0;
        H->Printf(me->r, "HTTP/1.0 401 Unauthorized\r\nContent-Type: text/html\r\n\r\n"
                         "<TITLE>Login failed</TITLE><H2>Login failed</H2><P>", a);
        out_text(me, me->msg);
        H->Format(prompt, sizeof prompt, anonymous ? "Log in to %s." : "The name or password for %s was wrong. Try again?", a);
        H->AskLogin(me->r, prompt, NULL, NULL, key, anonymous ? "anonymous" : user, anonymous ? anon_pass : NULL);
        goto quit;
    }
    if (code / 100 != 2) { fail(me, "FTP: "); goto quit; }
    command(me, "TYPE I", "");
    /* where logging in put it, to come back to (without it, nothing is kept) */
    if (command(me, "PWD", "") == 257) read_home(me);
    else me->home[0] = 0;
    strcpy(me->at, "/");
in:
    /* into the address's folder, leaving the last part if it's a file */
    seg = strrchr(me->path, '/') + 1;
    n = seg - me->path;
    memcpy(folder, me->path, n);
    folder[n] = 0;
    if (!go_to(me, folder)) goto quit;
    if (*seg) {
        char name[NAME];
        H->UrlDecode(seg, name, sizeof name);
        err = retrieve(me, r, active, name);
    } else {
        if (!strncmp(r->text, "POST", 4) && !anonymous) {
            if (!(said = store(me, r, active, note, sizeof note))) { err = userCanceledErr; goto quit; }  /* stopped */
            if (!*said) goto quit;      /* failed: Mill says why */
        }
        err = listing(me, r, active, !anonymous, said,
                      !(H->GetSetting("ftp", "messages", setting, sizeof setting) == 2 && !strcmp(setting, "no")));
    }
quit:
    if (me->clean && me->home[0] && me->at[0] && err == noErr) {
        /* at rest: kept for the next page, with where it is (Mill says QUIT when it closes it) */
        n = strlen(me->home) + 1;
        memcpy(kept, me->home, n);
        if (n + (long)strlen(me->at) <= (long)sizeof kept) {
            memcpy(kept + n, me->at, strlen(me->at));
            H->TcpKeep(me->r, me->ctl, user, kept, n + strlen(me->at), "QUIT" MILL_CRLF, 6);
            me->ctl = 0;
            return err;
        }
    }
    if (!me->stopped && err != userCanceledErr) command(me, "QUIT", "");
    H->TcpClose(me->ctl);
    me->ctl = 0;
    return err;
}

static const MillSetting settings[] = {
    { "passive", "Passive mode", "When off, the server connects back to this Mac.", millCheckbox, "yes", 0, 0, NULL },
    { "messages", "Show server messages", "Welcome and folder messages above listings.", millCheckbox, "yes", 0, 0, NULL },
    { "anon_auto", "Automatic anonymous", "Always attempt an anonymous login before asking for credentials.", millCheckbox, "yes", 0, 0, NULL },
    { "anon_password", "Anonymous login password", "Many servers request an email address as this value.",
      millText, "mill@", 0, 0, NULL },
    { "eol", "Convert line endings on TEXT files", NULL, millChoice, "Unix [LF]", 0, 0, "Don\325t convert|Mac [CR]|Unix [LF]|DOS [CRLF]" },
    kMillRandomAccessSetting("no"),
    kMillProxySetting
};
static const MillProtocol protocols[] = { { "ftp", "FTP", "File Transfer Protocol (RFC 959)", 8192, 7, settings } };

/* Internet.c's: the protocol's description and settings, and its state for a request (the caller
   sets base.host, and frees it with FreeMem) */
const MillProtocol *ftp_protocol(void) { return protocols; }

MillPlugIn *ftp_state(const MillHost *host)
{
    FtpPlugIn *me = host->NewMem(sizeof *me);
    if (!me) return NULL;
    me->base.methods = NULL;
    me->base.host = host;
    return &me->base;
}
