/* Internet Services: a Plug-In for Mill with four small protocols.
     finger://host/user         RFC 1288   the answer as text
     whois://host/query         RFC 3912   the answer as text
     dict://host/d:word[:db]    RFC 2229   definitions as HTML; m:word[:db[:strategy]] matches
     time://host/               RFC 868    the server's time, and a button to set this Mac's clock
   Mill's addresses need a host, so "default" stands for a protocol's Default server
   setting: whois://default/example.com, dict://default/d:word, time://default/. */
#include <string.h>
#include <OSUtils.h>

#include "MillPlugIn.h"

enum { kFinger = 0, kWhois, kDict, kTime, kProtocols };
enum { LINE = 768, ESC = 4700, HOST = 256, BUF = 1024 };
/* 1900 to 1904: four years, none of them leap */
#define EPOCH_GAP 126144000UL
#define MAX_OFFSET 315576000L       /* ten years, in seconds */

typedef struct {
    MillPlugIn base;            /* first, so this is a MillPlugIn too */
    MillMethods methods;        /* filled in by PlugInNew (see MillPlugIn.h's rules) */
    MillProtocol protocol[kProtocols];
    MillSetting whoisSet[1], dictSet[1], timeSet[2];
    MillReqRef r;
    short stopped;
    char line[LINE];
    char esc[ESC];
    char buf[BUF];
    char host[HOST];
} Inet;

static void inet_close(MillPlugIn *self) { self->host->FreeMem(self); }

/* ---- helpers ---- */

static void putn(Inet *me, const char *s, long n)
{
    if (!me->stopped && !me->base.host->Write(me->r, s, n)) me->stopped = 1;
}

static void put(Inet *me, const char *s) { putn(me, s, strlen(s)); }

/* Printf's arguments are longs */
static void put1(Inet *me, const char *fmt, long a)
{
    long args[1];
    args[0] = a;
    if (!me->stopped && !me->base.host->Printf(me->r, fmt, args)) me->stopped = 1;
}

static void put2(Inet *me, const char *fmt, long a, long b)
{
    long args[2];
    args[0] = a;
    args[1] = b;
    if (!me->stopped && !me->base.host->Printf(me->r, fmt, args)) me->stopped = 1;
}

/* s, made safe for HTML, to the response */
static void put_esc(Inet *me, const char *s)
{
    if (me->base.host->HtmlEscape(s, me->esc, ESC) < 0) me->esc[0] = 0;
    put(me, me->esc);
}

static void header(Inet *me, const char *type)
{
    put1(me, "HTTP/1.0 200 OK" MILL_CRLF "Content-Type: %s" MILL_CRLF MILL_CRLF, (long)type);
}

static void failf(Inet *me, const char *fmt, const char *arg)
{
    char text[200];
    long a[1];
    a[0] = (long)arg;
    me->base.host->Format(text, sizeof text, fmt, a);
    me->base.host->Fail(me->r, text);
}

/* The host to connect to: the address's, or the setting's for "default" */
static const char *pick_host(Inet *me, MillRequest *r, const char *scheme, const char *dflt)
{
    if (strcmp(r->host, "default")) return r->host;
    if (me->base.host->GetSetting(scheme, "server", me->host, HOST) <= 0 || !me->host[0]) strcpy(me->host, dflt);
    return me->host;
}

static long number_setting(Inet *me, const char *scheme, const char *key)
{
    char v[24];
    const char *p = v;
    long n = 0;
    short neg = 0;
    if (me->base.host->GetSetting(scheme, key, v, sizeof v) <= 0) return 0;
    while (*p && *p != '-' && *p != '+' && (*p < '0' || *p > '9')) p++;     /* "UTC-8" */
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
    return neg ? -n : n;
}

/* Receive into me->buf; 0 bytes or an error ends it */
static OSErr recv_some(Inet *me, MillConn c, long *n)
{
    *n = BUF;
    return me->base.host->TcpRecv(c, me->buf, n);
}

/* One CR LF line into me->line (cut to fit). Returns 0 at the end of the data. */
static short get_line(Inet *me, MillConn c, long *have, long *at, short *eof)
{
    long k = 0;
    for (;;) {
        while (*at < *have) {
            char ch = me->buf[(*at)++];
            if (ch == 10) { me->line[k] = 0; return 1; }
            if (ch != 13 && k < LINE - 1) me->line[k++] = ch;
        }
        if (*eof) { me->line[k] = 0; return k > 0; }
        {
            OSErr err = recv_some(me, c, have);
            *at = 0;
            if (err != noErr || *have == 0) { *eof = 1; *have = 0; if (err == -128) me->stopped = 1; }
        }
        if (me->stopped) { me->line[k] = 0; return 0; }
    }
}

/* ---- finger and whois: send a line, show the reply as text ---- */

static OSErr ask_text(Inet *me, MillRequest *r, const char *host, short port, const char *query)
{
    const MillHost *h = me->base.host;
    long n, k;
    MillConn c;
    OSErr err;
    char q[200];
    for (k = 0; query[k] && k < 196; k++) q[k] = query[k];
    q[k++] = 13;    /* CR LF (see MILL_CRLF) */
    q[k++] = 10;
    if ((err = h->TcpOpen(r->ref, host, r->port ? r->port : port, &c)) != noErr) return err;
    if ((err = h->TcpSend(c, q, k)) != noErr) { h->TcpClose(c); return err; }
    header(me, "text/plain");
    for (;;) {
        err = recv_some(me, c, &n);
        if (err != noErr || n == 0) break;
        putn(me, me->buf, n);
        if (me->stopped) break;
    }
    h->TcpClose(c);
    return err == -128 ? err : noErr;
}

static OSErr do_finger(Inet *me, MillRequest *r)
{
    char who[128];
    long k;
    me->base.host->UrlDecode(r->path + 1, who, sizeof who);
    for (k = 0; who[k]; k++) if (who[k] == '?' || (unsigned char)who[k] < 32) who[k] = 0;
    return ask_text(me, r, r->host, 79, who);
}

static OSErr do_whois(Inet *me, MillRequest *r)
{
    const MillHost *h = me->base.host;
    char q[160];
    long k;
    const char *host = pick_host(me, r, "whois", "whois.iana.org");
    const char *p = strchr(r->path, '?');
    /* the words after '?' (from ISINDEX, with + for space), else the path */
    strncpy(q, p ? p + 1 : r->path + 1, sizeof q - 1);
    q[sizeof q - 1] = 0;
    if (p) for (k = 0; q[k]; k++) if (q[k] == '+') q[k] = ' ';
    h->UrlDecode(q, q, sizeof q);
    for (k = 0; q[k]; k++) if ((unsigned char)q[k] < 32) q[k] = ' ';
    if (!q[0]) {
        header(me, "text/html");
        put(me, "<TITLE>Whois</TITLE><ISINDEX PROMPT=\"Whois: \">");
        return noErr;
    }
    return ask_text(me, r, host, 43, q);
}

/* ---- dict ---- */

/* the next word of s (quoted, or up to a space) into out; returns where it ended */
static const char *token(const char *s, char *out, long size)
{
    long k = 0;
    while (*s == ' ') s++;
    if (*s == '"') {
        for (s++; *s && *s != '"'; s++) {
            if (*s == '\\' && s[1]) s++;
            if (k < size - 1) out[k++] = *s;
        }
        if (*s) s++;
    } else {
        for (; *s && *s != ' '; s++) if (k < size - 1) out[k++] = *s;
    }
    out[k] = 0;
    return s;
}

static short is_code(const char *s)
{
    return s[0] >= '1' && s[0] <= '5' && s[1] >= '0' && s[1] <= '9' && s[2] >= '0' && s[2] <= '9' && (s[3] == ' ' || !s[3]);
}

/* in, for DICT's command line: no quotes, backslashes, or control characters (in place is fine) */
static void clean(const char *in, char *out, long size)
{
    long k = 0;
    for (; *in && k < size - 1; in++) if (*in != '"' && *in != '\\' && (unsigned char)*in >= 32) out[k++] = *in;
    out[k] = 0;
}

static void dict_form(Inet *me, const char *host, const char *word)
{
    char hh[HOST], ww[200];
    me->base.host->HtmlEscape(host, hh, sizeof hh);
    me->base.host->HtmlEscape(word, ww, sizeof ww);
    put(me, "<FORM ACTION=\"dict://");
    put(me, hh);
    put(me, "/\"><P>Word: <INPUT NAME=w VALUE=\"");
    put(me, ww);
    put(me, "\" SIZE=24> <INPUT TYPE=SUBMIT NAME=a VALUE=\"Define\"> <INPUT TYPE=SUBMIT NAME=a VALUE=\"Match\"></P></FORM><HR>\n");
}

static void dict_link(Inet *me, const char *host, const char *word, const char *db)
{
    char enc[300], w[340], hh[HOST];
    long i, o = 0;
    me->base.host->UrlEncode(word, enc, sizeof enc);
    for (i = 0; enc[i] && o < (long)sizeof w - 4; i++) {
        if (enc[i] == ':') { w[o++] = '%'; w[o++] = '3'; w[o++] = 'A'; }
        else w[o++] = enc[i];
    }
    w[o] = 0;
    me->base.host->HtmlEscape(host, hh, sizeof hh);
    put(me, "<LI><A HREF=\"dict://");
    put(me, hh);
    put(me, "/d:");
    put(me, w);
    put(me, "\">");
    put_esc(me, word);
    put(me, "</A> (");
    put_esc(me, db);
    put(me, ")\n");
}

static OSErr do_dict(Inet *me, MillRequest *r)
{
    const MillHost *h = me->base.host;
    const char *host = pick_host(me, r, "dict", "dict.org");
    char cmd[560], word[160], db[64], strat[32], shown[160];
    const char *q;
    short mode = 0;     /* 0 database list, 1 define, 2 match */
    long have = 0, at = 0;
    short eof = 0, section = 0, any = 0;
    MillConn c;
    OSErr err;
    word[0] = 0;
    strcpy(db, "*");
    strcpy(strat, ".");
    if ((q = strchr(r->path, '?')) != 0) {
        /* a form's /?w=word&a=Define, or ISINDEX's words alone */
        long ql = strlen(++q), n;
        char a[16];
        if (h->FormField(q, ql, "w", word, sizeof word) > 0) {
            mode = 1;
            if (h->FormField(q, ql, "a", a, sizeof a) > 0 && (a[0] == 'M' || a[0] == 'm')) mode = 2;
        } else if (!strchr(q, '=')) {
            strncpy(word, q, sizeof word - 1);
            word[sizeof word - 1] = 0;
            for (n = 0; word[n]; n++) if (word[n] == '+') word[n] = ' ';
            h->UrlDecode(word, word, sizeof word);
            mode = 1;
        }
    } else if ((r->path[1] == 'd' || r->path[1] == 'm') && r->path[2] == ':') {
        /* /d:word[:db] or /m:word[:db[:strategy]] */
        char part[3][160];
        short np = 0;
        const char *s = r->path + 3;
        mode = r->path[1] == 'd' ? 1 : 2;
        while (np < 3) {
            long k = 0;
            while (*s && *s != ':' && k < 159) part[np][k++] = *s++;
            part[np][k] = 0;
            np++;
            if (*s != ':') break;
            s++;
        }
        h->UrlDecode(part[0], word, sizeof word);
        if (np > 1 && part[1][0]) h->UrlDecode(part[1], db, sizeof db);
        if (np > 2 && part[2][0]) h->UrlDecode(part[2], strat, sizeof strat);
    }
    clean(word, shown, sizeof shown);
    if (mode && !shown[0]) mode = 0;
    clean(db, db, sizeof db);
    clean(strat, strat, sizeof strat);
    if (!db[0]) strcpy(db, "*");
    if (!strat[0]) strcpy(strat, ".");
    cmd[0] = 0;
    if (mode == 1) {
        strcat(cmd, "DEFINE "); strcat(cmd, db); strcat(cmd, " \""); strcat(cmd, shown); strcat(cmd, "\"\015\012");
    } else if (mode == 2) {
        strcat(cmd, "MATCH "); strcat(cmd, db); strcat(cmd, " "); strcat(cmd, strat);
        strcat(cmd, " \""); strcat(cmd, shown); strcat(cmd, "\"\015\012");
    } else strcat(cmd, "SHOW DB\015\012");
    strcat(cmd, "QUIT\015\012");
    if ((err = h->TcpOpen(r->ref, host, r->port ? r->port : 2628, &c)) != noErr) return err;
    /* the greeting, 220 */
    if (!get_line(me, c, &have, &at, &eof) || strncmp(me->line, "220", 3)) {
        h->TcpClose(c);
        if (me->stopped) return -128;
        failf(me, "The server refused it: %s", me->line[0] ? me->line + (me->line[3] == ' ' ? 4 : 0) : "no greeting.");
        return noErr;
    }
    if ((err = h->TcpSend(c, cmd, strlen(cmd))) != noErr) { h->TcpClose(c); return err; }
    header(me, "text/html; charset=utf-8");
    put(me, "<HTML><HEAD><TITLE>");
    if (mode) put_esc(me, shown); else put(me, "Dictionary");
    put(me, "</TITLE></HEAD><BODY><H2>Dictionary</H2>\n");
    dict_form(me, host, shown);
    while (!me->stopped && get_line(me, c, &have, &at, &eof)) {
        if (section == 0) {
            /* a status line */
            if (!is_code(me->line)) continue;
            if (!strncmp(me->line, "151", 3)) {
                /* 151 "word" database "description" */
                char w[160], dname[64], desc[160];
                const char *s = token(me->line + 4, w, sizeof w);
                s = token(s, dname, sizeof dname);
                token(s, desc, sizeof desc);
                put(me, "<P><B>");
                put_esc(me, desc[0] ? desc : dname);
                put(me, "</B> (");
                put_esc(me, dname);
                put(me, ")</P>\n<PRE>");
                section = 1;
                any = 1;
            } else if (!strncmp(me->line, "152", 3)) {
                put(me, "<UL>\n");
                section = 2;
                any = 1;
            } else if (!strncmp(me->line, "110", 3)) {
                put(me, "<P>Databases on ");
                put_esc(me, host);
                put(me, ":</P><UL>\n");
                section = 3;
                any = 1;
            } else if (me->line[0] == '5' || me->line[0] == '4') {
                if (!strncmp(me->line, "552", 3)) put(me, "<P>No match.</P>\n");
                else { put(me, "<P>"); put_esc(me, me->line[3] ? me->line + 4 : me->line); put(me, "</P>\n"); }
                any = 1;
            } else if (!strncmp(me->line, "250", 3)) break;
            continue;
        }
        if (me->line[0] == '.' && !me->line[1]) {
            put(me, section == 1 ? "</PRE>\n" : "</UL>\n");
            section = 0;
            continue;
        }
        {
            const char *t = me->line[0] == '.' ? me->line + 1 : me->line;   /* undo dot stuffing */
            if (section == 1) { put_esc(me, t); put(me, "\n"); }
            else {
                char d[64], w[160];
                const char *s = token(t, d, sizeof d);
                token(s, w, sizeof w);
                if (section == 2) dict_link(me, host, w, d);
                else {
                    put(me, "<LI><B>");
                    put_esc(me, d);
                    put(me, "</B> ");
                    put_esc(me, w);
                    put(me, "\n");
                }
            }
        }
    }
    if (section) put(me, section == 1 ? "</PRE>\n" : "</UL>\n");
    if (!any && !me->stopped) put(me, "<P>No answer from the server.</P>\n");
    put(me, "</BODY></HTML>\n");
    h->TcpClose(c);
    return me->stopped ? -128 : noErr;
}

/* ---- time ---- */

/* seconds since 1904 as "Sat 2026-11-14 12:00:00" */
static void show_time(Inet *me, unsigned long secs, char *out, long size)
{
    DateTimeRec d;
    long a[8];
    static const char days[] = "SunMonTueWedThuFriSat";
    SecondsToDate(secs, &d);
    a[0] = 3;
    a[1] = (long)(days + 3 * (d.dayOfWeek - 1));
    a[2] = d.year;
    a[3] = d.month;
    a[4] = d.day;
    a[5] = d.hour;
    a[6] = d.minute;
    a[7] = d.second;
    me->base.host->Format(out, size, "%.*s %04d-%02d-%02d %02d:%02d:%02d", a);
}

/* the server's time, seconds since 1900. If port 37 won't do and the address names no port,
   *daytime is set and the server's answer on port 13 (RFC 867) is in me->line. */
static OSErr read_server_time(Inet *me, MillRequest *r, const char *host, unsigned long *srv, short *daytime)
{
    const MillHost *h = me->base.host;
    MillConn c;
    OSErr err;
    long got = 0, n;
    unsigned char t[4];
    *daytime = 0;
    err = h->TcpOpen(r->ref, host, r->port ? r->port : 37, &c);
    if (err == noErr) {
        while (got < 4) {
            n = 4 - got;
            err = h->TcpRecv(c, t + got, &n);
            if (err != noErr || n == 0) break;
            got += n;
        }
        h->TcpClose(c);
        if (got == 4) {
            *srv = ((unsigned long)t[0] << 24) | ((unsigned long)t[1] << 16) | ((unsigned long)t[2] << 8) | t[3];
            return noErr;
        }
    }
    if (err == -128 || r->port) return err ? err : -1;
    if ((err = h->TcpOpen(r->ref, host, 13, &c)) != noErr) return err;
    got = 0;
    for (;;) {
        n = LINE - 1 - got;
        if (n <= 0 || h->TcpRecv(c, me->line + got, &n) != noErr || n == 0) break;
        got += n;
    }
    h->TcpClose(c);
    me->line[got] = 0;
    *daytime = 1;
    return noErr;
}

static void time_page(Inet *me, const char *title)
{
    me->base.host->PlugInPage(me->r);   /* the clock is set only from a page marked like this */
    header(me, "text/html");
    put(me, "<HTML><HEAD><TITLE>");
    put(me, title);
    put(me, "</TITLE></HEAD><BODY><H2>");
    put(me, title);
    put(me, "</H2>\n");
}

static OSErr do_time(Inet *me, MillRequest *r)
{
    const MillHost *h = me->base.host;
    const char *host = pick_host(me, r, "time", "time.nist.gov");
    unsigned long srv, mac, local;
    long tz = number_setting(me, "time", "zone"), diff;
    short daytime;
    char hh[HOST], t1[48], t2[48];
    OSErr err;
    if (!strncmp(r->text, "POST ", 5)) {
        /* set the clock by the offset the page measured: the Mac's time now, plus offset */
        char body[64], v[24];
        long n = sizeof body - 1, off = 0;
        const char *p = v;
        short neg = 0, digits = 0;
        if (h->From(r->ref) != millFromOwnPage) { h->Fail(r->ref, "Time: Only this Plug-In's own page can set the clock."); return noErr; }
        if (h->ReadBody(r->ref, body, &n) != noErr || n < 0) n = 0;
        body[n] = 0;
        if (h->FormField(body, n, "offset", v, sizeof v) <= 0) { h->Fail(r->ref, "Time: The offset is missing."); return noErr; }
        if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
        for (; *p >= '0' && *p <= '9'; p++) {
            if (off <= MAX_OFFSET) off = off * 10 + (*p - '0');
            digits++;
        }
        if (*p || !digits || off > MAX_OFFSET) { h->Fail(r->ref, "Time: The offset is not a number within ten years."); return noErr; }
        if (neg) off = -off;
        GetDateTime(&mac);
        mac += (unsigned long)off;
        SetDateTime(mac);
        time_page(me, "Time");
        show_time(me, mac, t2, sizeof t2);
        put1(me, "<P>This Mac's clock is set: %s</P>\n", (long)t2);
        h->HtmlEscape(host, hh, sizeof hh);
        put2(me, "<P><A HREF=\"time://%s/\">Check again</A></P></BODY></HTML>\n", (long)hh, 0);
        return noErr;
    }
    if ((err = read_server_time(me, r, host, &srv, &daytime)) != noErr) return err;
    h->HtmlEscape(host, hh, sizeof hh);
    time_page(me, "Time");
    if (daytime) {
        put(me, "<P>No time service on port 37. The daytime service (RFC 867) says:</P><PRE>");
        put_esc(me, me->line);
        put(me, "</PRE></BODY></HTML>\n");
        return noErr;
    }
    local = srv - EPOCH_GAP + (unsigned long)(tz * 3600L);
    GetDateTime(&mac);
    diff = (long)(local - mac);
    show_time(me, local, t1, sizeof t1);
    show_time(me, mac, t2, sizeof t2);
    put2(me, "<P>Server %s: <B>%s</B>", (long)hh, (long)t1);
    put1(me, "<BR>This Mac: <B>%s</B>", (long)t2);
    put2(me, "<BR>Difference: %ld second%s, ", diff < 0 ? -diff : diff, (long)(diff == 1 || diff == -1 ? "" : "s"));
    put(me, diff < 0 ? "the Mac is ahead." : diff > 0 ? "the Mac is behind." : "none.");
    if (tz) put2(me, "<BR>Time zone: UTC%s%ld hours.", (long)(tz < 0 ? "-" : "+"), tz < 0 ? -tz : tz);
    put(me, "</P>\n");
    put1(me, "<FORM METHOD=POST ACTION=\"time://%s/\">", (long)hh);
    put1(me, "<INPUT TYPE=HIDDEN NAME=offset VALUE=\"%ld\">", diff);
    put(me, "<INPUT TYPE=SUBMIT VALUE=\"Set this Mac's clock\"></FORM>\n</BODY></HTML>\n");
    return noErr;
}

/* ---- the Plug-In ---- */

static OSErr inet_request(MillPlugIn *self, MillRequest *r)
{
    Inet *me = (Inet *)self;
    me->r = r->ref;
    me->stopped = 0;
    switch (r->protocol) {
    case kFinger: return do_finger(me, r);
    case kWhois: return do_whois(me, r);
    case kDict: return do_dict(me, r);
    case kTime: return do_time(me, r);
    }
    return millUnknown;
}

static void setting(MillSetting *s, const char *key, const char *label, const char *note, short kind,
                    const char *dflt, long min, long max)
{
    s->key = key;
    s->label = label;
    s->note = note;
    s->kind = kind;
    s->dflt = dflt;
    s->min = min;
    s->max = max;
    s->choices = 0;
}

static void protocol(MillProtocol *p, const char *scheme, const char *heading, const char *desc,
                     short n, const MillSetting *s)
{
    p->scheme = scheme;
    p->heading = heading;
    p->description = desc;
    p->settingCount = n;
    p->settings = s;
}

MillPlugIn *PlugInNew(const MillHost *host, short resFile, MillInfo *info)
{
    Inet *me = host->NewMem(sizeof *me);
    (void)resFile;
    if (!me) return 0;
    me->methods.close = inet_close;
    me->methods.request = inet_request;
    me->methods.purge = 0;
    me->methods.format = 0;
    me->base.methods = &me->methods;
    setting(&me->whoisSet[0], "server", "Default server", "For whois://default/query", millText, "whois.iana.org", 0, 0);
    setting(&me->dictSet[0], "server", "Default server", "For dict://default/d:word", millText, "dict.org", 0, 0);
    setting(&me->timeSet[0], "server", "Default server", "For time://default/", millText, "time.nist.gov", 0, 0);
    setting(&me->timeSet[1], "zone", "Time zone", "Hours from UTC. System 6 has none, so UTC sets this Mac's clock to UTC.", millChoice, "UTC", 0, 0);
    /* a choice, not a number: Mill's number settings take no minus sign, and a menu item
       that starts with one is a divider */
    me->timeSet[1].choices = "UTC-12|UTC-11|UTC-10|UTC-9|UTC-8|UTC-7|UTC-6|UTC-5|UTC-4|UTC-3|UTC-2|UTC-1|UTC|"
                             "UTC+1|UTC+2|UTC+3|UTC+4|UTC+5|UTC+6|UTC+7|UTC+8|UTC+9|UTC+10|UTC+11|UTC+12|UTC+13|UTC+14";
    protocol(&me->protocol[kFinger], "finger", "Finger", "Finger User Information Protocol (RFC 1288)", 0, 0);
    protocol(&me->protocol[kWhois], "whois", "Whois", "Whois (RFC 3912)", 1, me->whoisSet);
    protocol(&me->protocol[kDict], "dict", "Dict", "Dictionary Server Protocol (RFC 2229)", 1, me->dictSet);
    protocol(&me->protocol[kTime], "time", "Time", "Time Protocol (RFC 868)", 2, me->timeSet);
    info->name = "internet";
    info->version = "1.0";
    info->copyright = "Copyright 2026 OneInAMillion Software";
    info->by = "OneInAMillion Software";
    info->description = "Finger, whois, dict, and time.";
    info->flags = 0;
    info->protocolCount = kProtocols;
    info->protocols = me->protocol;
    info->formatCount = 0;
    info->formats = 0;
    return &me->base;
}
