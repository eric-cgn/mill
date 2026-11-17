/* news: and nntp: in the News Plug-In: Usenet news from an NNTP server (RFC 977), read-only,
   with addresses as RFC 1738 has them (the host always named):
     news://host/                  the groups (LIST), a page at a time, with a search
     news://host/group             its articles, newest first (GROUP, then XOVER)
     news://host/group/number      an article (ARTICLE)
     news://host/id@domain         an article by its Message-ID (ARTICLE <id>)
   nntp:// is the same. XOVER and AUTHINFO USER/PASS are the common extensions of the day
   (written up since in RFC 2980); a server without XOVER gets HEAD for each article. When
   a server asks for a login (480), the address's name and password go, or the one kept,
   or Mill asks. Text parts are shown (quoted-printable and base64 decoded, RFC 2045); other
   parts are named, not shown. The connection is kept between pages (TcpKeep): in reader
   mode, logged in if it had to, in the group last opened. */
#include <string.h>

#include "MillPlugIn.h"
#include "news.h"
#include "newsutil.h"
#include "mime.h"

enum { LINE = 1024, BUF = 1024, NAMEN = 256, HEAD = 3072, ESC = LINE * 6 };
enum { kLost = -30201, kGarbled = -30202, kDone = 1 };
enum { kGroupsPage = 200 };

/* An article's line on a group's page */
typedef struct {
    long number, lines;
    char subject[96], from[48], day[16];
    short cs;                   /* the subject's and name's charset */
} Row;

typedef struct {
    MillPlugIn base;
    MillRequest *req;
    MillReqRef r;
    MillConn c;
    short stopped, tried, broken;   /* broken: a reply was cut off */
    short ready;                /* greeted, and in reader mode */
    char group[NAMEN];          /* the current group (GROUP), "" for none */
    long have, at, len;         /* the receive buffer's bytes and where reading is; the line's length */
    char host[NAMEN];
    char user[NAMEN], pass[NAMEN], key[NAMEN + 16];
    char cmd[LINE];             /* the last command, to send again after a login */
    char line[LINE];
    char buf[BUF];
    char work[LINE];
    char esc[ESC];
    char head[HEAD];
    long headn;
} News;

#define H (me->base.host)

/* ------------------------------------------------------------ the page */

static void out(News *me, const char *s, long n)
{
    if (!me->stopped && n > 0 && !H->Write(me->r, s, n)) me->stopped = 1;
}

static void outs(News *me, const char *s) { out(me, s, strlen(s)); }

static void outf(News *me, const char *fmt, long a, long b)
{
    long args[2];
    args[0] = a;
    args[1] = b;
    if (!me->stopped && !H->Printf(me->r, fmt, args)) me->stopped = 1;
}

/* n bytes from the server in charset, as Mac Roman made safe for HTML */
static void out_text(News *me, const char *s, long n, short cs)
{
    if (n > LINE - 1) n = LINE - 1;
    if (n > 0 && H->ToMacRoman(s, n, cs, me->work, sizeof me->work) >= 0 && H->HtmlEscape(me->work, me->esc, sizeof me->esc) >= 0)
        outs(me, me->esc);
}

/* A header's value with its encoded words decoded (RFC 1522) */
static void out_words(News *me, const char *s)
{
    char d[LINE];
    short cs = mime_words(s, d, sizeof d);
    out_text(me, d, strlen(d), cs);
}

static void icon(News *me, short id)
{
    outf(me, "<IMG SRC=\"plugin://" NEWS_NAME "/rsrc?type=ics%%23&id=%d\" WIDTH=16 HEIGHT=16 ALIGN=TOP> ", id, 0);
}

static void page_head(News *me, const char *title)
{
    H->PlugInPage(me->r);
    outs(me, "HTTP/1.0 200 OK" MILL_CRLF "Content-Type: text/html; charset=macintosh" MILL_CRLF MILL_CRLF "<TITLE>" NEWS_TITLE ": ");
    out_text(me, title, strlen(title), millGuess);
    outs(me, "</TITLE>\n");
}

/* A link to the page above, with the parent icon */
static void up(News *me, const char *href, const char *text)
{
    char e[NAMEN * 3];
    if (H->UrlEncode(href, e, sizeof e) < 0) return;
    icon(me, kMillIconParent);
    outf(me, "<A HREF=\"%s\">", (long)e, 0);
    out_text(me, text, strlen(text), millGuess);
    outs(me, "</A>\n");
}

/* The server's words, after its reply's code, in an alert */
static void refused(News *me)
{
    char text[LINE + 40];
    long a[1];
    a[0] = (long)(me->line[3] == ' ' ? me->line + 4 : me->line);
    H->Format(text, sizeof text, "The server refused it: %s", a);
    H->Fail(me->r, text);
}

/* ------------------------------------------------------------ the connection */

/* One line of the reply into line (without CR LF; cut to fit), its length in len */
static OSErr get_line(News *me)
{
    long k = 0;
    for (;;) {
        while (me->at < me->have) {
            char ch = me->buf[me->at++];
            if (ch == 10) {
                if (k && me->line[k - 1] == 13) k--;
                me->line[k] = 0;
                me->len = k;
                return noErr;
            }
            if (k < LINE - 1) me->line[k++] = ch;
        }
        me->have = BUF;
        me->at = 0;
        {
            OSErr err = H->TcpRecv(me->c, me->buf, &me->have);
            if (err != noErr) { me->have = 0; me->broken = 1; return err; }
            if (!me->have) { me->broken = 1; return kLost; }
        }
    }
}

static OSErr send_line(News *me, const char *s)
{
    char l[LINE + 4];
    long n = strlen(s);
    if (n > LINE - 1) n = LINE - 1;
    memcpy(l, s, n);
    l[n++] = 13;
    l[n++] = 10;
    if (H->TcpSend(me->c, l, n) != noErr) { me->broken = 1; return kLost; }
    return noErr;
}

/* Ask for a name and password: first (wrong 0), or again after the server refused them */
static void ask(News *me, short wrong)
{
    char prompt[NAMEN + 64];
    long a[1];
    a[0] = (long)me->host;
    me->req->loginKey[0] = 0;
    if (wrong) outs(me, "HTTP/1.0 401 Unauthorized" MILL_CRLF "Content-Type: text/html" MILL_CRLF MILL_CRLF
                        "<TITLE>Login failed</TITLE><H2>Login failed</H2><P>The server refused the name or password.");
    H->Format(prompt, sizeof prompt, wrong ? "The name or password for %s was wrong. Try again?" : "Log in to %s.", a);
    H->AskLogin(me->r, prompt, NULL, NULL, me->key, me->user[0] ? me->user : NULL, NULL);
}

/* AUTHINFO USER and PASS: RFC 2980 3.1.1 (381 wants the password, 281 is in, 482 and 502
   refused). kDone if Mill is to ask. */
static OSErr login(News *me)
{
    char l[NAMEN + 20];
    long a[1];
    short code;
    OSErr err;
    if (!me->user[0]) { ask(me, 0); return kDone; }
    a[0] = (long)me->user;
    H->Format(l, sizeof l, "AUTHINFO USER %s", a);
    if ((err = send_line(me, l)) != noErr || (err = get_line(me)) != noErr) return err;
    code = news_code(me->line);
    if (code == 381) {
        a[0] = (long)me->pass;
        H->Format(l, sizeof l, "AUTHINFO PASS %s", a);
        err = send_line(me, l);
        memset(l, 0, sizeof l);
        if (err != noErr || (err = get_line(me)) != noErr) return err;
        code = news_code(me->line);
    }
    if (code != 281) { ask(me, 1); return kDone; }
    if (me->req->user[0]) strcpy(me->req->loginKey, me->key);     /* the address's: kept under it */
    return noErr;
}

/* Send a command (fmt with one string or number) and read its reply's first line: its code
   in *code. A server that wants a login first (480: RFC 2980 3.1.1) gets one, and the
   command again. */
static OSErr command(News *me, const char *fmt, long arg, short *code)
{
    long a[1];
    OSErr err;
    a[0] = arg;
    H->Format(me->cmd, sizeof me->cmd, fmt, a);
    for (;;) {
        if ((err = send_line(me, me->cmd)) != noErr || (err = get_line(me)) != noErr) return err;
        *code = news_code(me->line);
        if (!*code) return kGarbled;
        if (*code != 480 || me->tried) return noErr;
        me->tried = 1;
        if ((err = login(me)) != noErr) return err;
    }
}

/* The next line of a multi-line reply (RFC 977 2.4.1): 1, or 0 at its end ("." or an error
   in *err) */
static short next_line(News *me, OSErr *err)
{
    if ((*err = get_line(me)) != noErr) return 0;
    return (me->len = news_unstuff(me->line, me->len)) >= 0;
}

/* ------------------------------------------------------------ the groups */

/* LIST, a page of kGroupsPage at a time, those whose names have find in them */
static OSErr groups(News *me, const char *query)
{
    char find[64], skip_s[16], e[200];
    long skip = 0, count = 0, shown = 0, n = query ? (long)strlen(query) : 0;
    short code, more = 0;
    NewsGroup g;
    OSErr err;
    const char *p;
    find[0] = 0;
    if (query && H->FormField(query, n, "find", find, sizeof find) < 0) find[0] = 0;
    if (query && H->FormField(query, n, "skip", skip_s, sizeof skip_s) > 0) for (p = skip_s; *p >= '0' && *p <= '9'; p++) skip = skip * 10 + (*p - '0');
    if ((err = command(me, "LIST", 0, &code)) != noErr) return err;
    if (code != 215) { refused(me); return kDone; }
    page_head(me, me->host);
    outs(me, "<H2>");
    out_text(me, me->host, strlen(me->host), millGuess);
    outs(me, "</H2>\n<FORM ACTION=\"/\">Find groups: <INPUT NAME=find SIZE=20 VALUE=\"");
    out_text(me, find, strlen(find), millGuess);
    outs(me, "\"> <INPUT TYPE=SUBMIT VALUE=\"Find\"></FORM>\n");
    while (next_line(me, &err) && !me->stopped) {
        if (!news_group(me->line, &g) || (find[0] && !news_match(g.name, find))) continue;
        if (++count <= skip) continue;
        if (shown == kGroupsPage) { more = 1; break; }
        shown++;
        if (H->UrlEncode(g.name, e, sizeof e) < 0) continue;
        icon(me, kMillIconFolder);
        outf(me, "<A HREF=\"%s\">", (long)e, 0);
        out_text(me, g.name, strlen(g.name), millGuess);
        outf(me, "</A> (%ld)<BR>\n", g.last >= g.first && g.last ? g.last - g.first + 1 : 0, 0);
    }
    if (err != noErr && !shown) return err;
    if (!shown) outs(me, find[0] ? "<P>No groups match." : "<P>No groups.");
    if (more) {
        if (H->UrlEncode(find, e, sizeof e) < 0) e[0] = 0;
        outf(me, "<P><A HREF=\"?skip=%ld&find=%s\">More groups</A>\n", skip + shown, (long)e);
        H->TcpClose(me->c);         /* the rest of the list isn't wanted */
        me->c = 0;
    }
    return noErr;
}

/* ------------------------------------------------------------ a group */

/* HEAD for an article without XOVER (RFC 977 3.1.2): 1 if it's there */
static short head_row(News *me, long number, Row *row, OSErr *err)
{
    short code;
    char v[LINE];
    if ((*err = command(me, "HEAD %ld", number, &code)) != noErr) return 0;
    if (code != 221) return 0;
    me->headn = 0;
    while (next_line(me, err)) {
        if (me->headn + me->len + 2 < HEAD) {
            memcpy(me->head + me->headn, me->line, me->len);
            me->headn += me->len;
            me->head[me->headn++] = '\n';
        }
    }
    if (*err != noErr) return 0;
    row->number = number;
    mime_header(me->head, me->headn, "Subject", v, sizeof v);
    row->cs = mime_words(v, row->subject, sizeof row->subject);
    mime_header(me->head, me->headn, "From", v, sizeof v);
    {
        char w[LINE];
        short cs = mime_words(v, w, sizeof w);
        if (cs != kMimeGuess) row->cs = cs;
        mime_who(w, row->from, sizeof row->from);
    }
    mime_header(me->head, me->headn, "Date", v, sizeof v);
    mime_day(v, row->day, sizeof row->day);
    row->lines = 0;
    if (mime_header(me->head, me->headn, "Lines", v, sizeof v) > 0) {
        const char *p = v;
        while (*p >= '0' && *p <= '9') row->lines = row->lines * 10 + (*p++ - '0');
    }
    return 1;
}

static void row_out(News *me, const char *group, const Row *row)
{
    char e[NAMEN * 3];
    if (H->UrlEncode(group, e, sizeof e) < 0) return;
    outf(me, "<TR><TD><A HREF=\"/%s/%ld\">", (long)e, row->number);
    if (row->subject[0]) out_text(me, row->subject, strlen(row->subject), row->cs);
    else outs(me, "(no subject)");
    outs(me, "</A></TD><TD>");
    out_text(me, row->from, strlen(row->from), row->cs);
    outs(me, "</TD><TD>");
    out_text(me, row->day, strlen(row->day), millGuess);
    outf(me, "</TD><TD ALIGN=RIGHT>%ld</TD></TR>\n", row->lines, 0);
}

/* GROUP (RFC 977 3.2.1), then XOVER for a page of its newest articles (RFC 2980 2.8), shown
   newest first; ?before=N for the page before article N */
static OSErr group(News *me, const char *name, const char *query)
{
    char v[16], e[NAMEN * 3];
    const char *p;
    long first, last, before = 0, lo, hi, page = 50, n = 0, i;
    short code, over = 1;
    Row *rows;
    NewsOver o;
    OSErr err;
    if (H->GetSetting("news", "page", v, sizeof v) > 0) {
        for (page = 0, p = v; *p >= '0' && *p <= '9'; p++) page = page * 10 + (*p - '0');
        if (page < 10) page = 10;
        if (page > 200) page = 200;
    }
    if ((err = command(me, "GROUP %s", (long)name, &code)) != noErr) return err;
    if (code == 411) { H->Fail(me->r, "No such group."); return kDone; }
    if (code != 211) { refused(me); return kDone; }
    strcpy(me->group, name);
    p = me->line + 3;       /* 211 n first last group */
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') p++;
    while (*p == ' ') p++;
    for (first = 0; *p >= '0' && *p <= '9'; p++) first = first * 10 + (*p - '0');
    while (*p == ' ') p++;
    for (last = 0; *p >= '0' && *p <= '9'; p++) last = last * 10 + (*p - '0');
    if (query && H->FormField(query, strlen(query), "before", v, sizeof v) > 0)
        for (p = v; *p >= '0' && *p <= '9'; p++) before = before * 10 + (*p - '0');
    hi = before > 0 && before - 1 < last ? before - 1 : last;
    lo = hi - page + 1 < first ? first : hi - page + 1;
    if (!(rows = H->NewMem(page * sizeof(Row)))) return memFullErr;
    H->Status(me->r, "Reading\311");
    if (last && hi >= lo && hi >= first) {
        long range[2];
        char r[40];
        range[0] = lo;
        range[1] = hi;
        H->Format(r, sizeof r, "XOVER %ld-%ld", range);
        if ((err = command(me, "%s", (long)r, &code)) != noErr) goto done;
        if (code == 224) {
            while (next_line(me, &err)) {
                if (n >= page || !news_over(me->line, &o) || o.number < lo || o.number > hi) continue;
                rows[n].number = o.number;
                rows[n].lines = o.lines;
                rows[n].cs = mime_words(o.subject, rows[n].subject, sizeof rows[n].subject);
                {
                    char w[LINE];
                    short cs = mime_words(o.from, w, sizeof w);
                    if (cs != kMimeGuess) rows[n].cs = cs;
                    mime_who(w, rows[n].from, sizeof rows[n].from);
                }
                mime_day(o.date, rows[n].day, sizeof rows[n].day);
                n++;
            }
            if (err != noErr) goto done;
        } else over = 0;    /* no XOVER (500, 501, 502...): HEAD for each */
    }
    page_head(me, name);
    up(me, "/", "Groups");
    outs(me, "<H2>");
    out_text(me, name, strlen(name), millGuess);
    outs(me, "</H2>\n");
    if (H->UrlEncode(name, e, sizeof e) < 0) e[0] = 0;
    if ((over && !n) || !last || hi < lo) outs(me, "<P>No articles.\n");
    else {
        outs(me, "<TABLE CELLSPACING=0 CELLPADDING=2><TR><TH ALIGN=LEFT>Subject</TH><TH ALIGN=LEFT>From</TH>"
                 "<TH ALIGN=LEFT>Date</TH><TH ALIGN=RIGHT>Lines</TH></TR>\n");
        if (over) for (i = n - 1; i >= 0 && !me->stopped; i--) row_out(me, name, &rows[i]);
        else for (i = hi; i >= lo && !me->stopped; i--) {
            if (head_row(me, i, &rows[0], &err)) row_out(me, name, &rows[0]);
            if (err != noErr) break;
        }
        outs(me, "</TABLE>\n");
        err = noErr;
    }
    if (lo > first || hi < last) {
        outs(me, "<P>");
        if (hi < last) {
            if (hi + page >= last) outf(me, "<A HREF=\"/%s\">Newer articles</A> &nbsp; ", (long)e, 0);
            else outf(me, "<A HREF=\"?before=%ld\">Newer articles</A> &nbsp; ", hi + page + 1, 0);
        }
        if (lo > first) outf(me, "<A HREF=\"?before=%ld\">Older articles</A>", lo, 0);
        outs(me, "\n");
    }
done:
    H->FreeMem(rows);
    return err;
}

/* ------------------------------------------------------------ an article */

enum { kText, kSkip, kPartHead, kOutside };

typedef struct {
    short cte;              /* 0 as it is, 1 quoted-printable, 2 base64 */
    short cs;
    MimeB64 b64;
    char boundary[80];
    short mode;             /* kText or kSkip, or for multipart, kPartHead and kOutside too */
    short multi, shown;     /* multipart; a text part's been shown */
    long size;              /* a part not shown: its bytes, as sent */
    char what[120];         /* and its name and type */
} Body;

/* Content-Type and Content-Transfer-Encoding from a block of headers (RFC 2045 5, 6), for
   the whole article (top) or a part */
static void body_kind(News *me, Body *b, short top)
{
    char ct[200], type[64], v[80];
    if (mime_header(me->head, me->headn, "Content-Type", ct, sizeof ct) < 0) strcpy(ct, "text/plain");
    mime_type(ct, type, sizeof type);
    b->cs = mime_param(ct, "charset", v, sizeof v) > 0 ? mime_charset(v) : millGuess;
    b->cte = 0;
    if (mime_header(me->head, me->headn, "Content-Transfer-Encoding", v, sizeof v) > 0) {
        mime_type(v, v, sizeof v);
        b->cte = !strcmp(v, "quoted-printable") ? 1 : !strcmp(v, "base64") ? 2 : 0;
    }
    memset(&b->b64, 0, sizeof b->b64);
    b->size = 0;
    if (top && !strncmp(type, "multipart/", 10) && mime_param(ct, "boundary", b->boundary, sizeof b->boundary) > 0) {
        b->multi = 1;
        b->mode = kOutside;
        return;
    }
    if (!strncmp(type, "text/", 5) && (!strcmp(type, "text/plain") || !b->multi || !b->shown) && strcmp(type, "text/html")) {
        b->mode = kText;
        b->shown = 1;
        return;
    }
    b->mode = kSkip;
    if (mime_param(ct, "name", v, sizeof v) <= 0) {
        char cd[200];
        if (mime_header(me->head, me->headn, "Content-Disposition", cd, sizeof cd) < 0 || mime_param(cd, "filename", v, sizeof v) <= 0) v[0] = 0;
    }
    {
        long a[2];
        a[0] = (long)v;
        a[1] = (long)type;
        H->Format(b->what, sizeof b->what, v[0] ? "%s, %s" : "%s%s", a);
    }
}

/* A part that wasn't shown, named where it was */
static void skipped(News *me, Body *b)
{
    long k = b->cte == 2 ? b->size * 3 / 4 : b->size;
    outs(me, "Not shown: ");
    out_text(me, b->what, strlen(b->what), millGuess);
    outf(me, " (%ldK)\n", (k + 1023) / 1024, 0);
}

/* A line of the body as text, decoded */
static void text_line(News *me, Body *b, char *s, long n)
{
    short soft = 0;
    if (b->cte == 2) {
        unsigned char d[LINE];
        long k = mime_b64(&b->b64, s, n, d), i, o = 0;
        for (i = 0; i < k; i++) if (d[i] != 13) d[o++] = d[i];      /* CR LF to the line end PRE takes */
        out_text(me, (char *)d, o, b->cs);
        return;
    }
    if (b->cte == 1) n = mime_qp(s, n, &soft);
    out_text(me, s, n, b->cs);
    if (!soft) outs(me, "\n");
}

/* One line of the body: by the part it's in */
static void body_line(News *me, Body *b)
{
    char *s = me->line;
    long n = me->len, bl = strlen(b->boundary);
    if (b->multi && n >= bl + 2 && s[0] == '-' && s[1] == '-' && !memcmp(s + 2, b->boundary, bl)) {
        /* RFC 2046 5.1.1: --boundary starts a part, --boundary-- ends the last */
        if (b->mode == kSkip) skipped(me, b);
        if (n >= bl + 4 && s[bl + 2] == '-' && s[bl + 3] == '-') b->mode = kOutside;
        else { b->mode = kPartHead; me->headn = 0; }
        return;
    }
    switch (b->mode) {
    case kText: text_line(me, b, s, n); break;
    case kSkip: b->size += n + 2; break;
    case kPartHead:
        if (!n) { body_kind(me, b, 0); break; }
        if (me->headn + n + 2 < HEAD) {
            memcpy(me->head + me->headn, s, n);
            me->headn += n;
            me->head[me->headn++] = '\n';
        }
        break;
    }
}

/* A header, as a line of the page: its label and value (encoded words decoded) */
static void field(News *me, const char *label, const char *name)
{
    char v[LINE];
    if (mime_header(me->head, me->headn, name, v, sizeof v) <= 0) return;
    outf(me, "<B>%s:</B> ", (long)label, 0);
    out_words(me, v);
    outs(me, "<BR>\n");
}

/* ARTICLE by number in a group (RFC 977 3.1.1), or by Message-ID: the headers that matter,
   and the body as text */
static OSErr article(News *me, const char *grp, long number, const char *id)
{
    char v[LINE], one[NAMEN], e[NAMEN * 3];
    const char *p;
    long k;
    short code, i;
    Body b;
    OSErr err;
    if (grp && strcmp(grp, me->group)) {
        if ((err = command(me, "GROUP %s", (long)grp, &code)) != noErr) return err;
        if (code == 411) { H->Fail(me->r, "No such group."); return kDone; }
        if (code != 211) { refused(me); return kDone; }
        strcpy(me->group, grp);
    }
    if (grp) {
        err = command(me, "ARTICLE %ld", number, &code);
    } else err = command(me, "ARTICLE <%s>", (long)id, &code);
    if (err != noErr) return err;
    if (code == 423 || code == 430) { H->Fail(me->r, "No such article."); return kDone; }
    if (code != 220) { refused(me); return kDone; }
    H->Status(me->r, "Reading\311");
    me->headn = 0;
    while (next_line(me, &err) && me->len) {
        if (me->headn + me->len + 2 < HEAD) {
            memcpy(me->head + me->headn, me->line, me->len);
            me->headn += me->len;
            me->head[me->headn++] = '\n';
        }
    }
    if (err != noErr) return err;
    /* the group to go up to: this one, or the first it was posted to */
    one[0] = 0;
    if (grp) { strncpy(one, grp, NAMEN - 1); one[NAMEN - 1] = 0; }
    else if (mime_header(me->head, me->headn, "Newsgroups", v, sizeof v) > 0) {
        for (k = 0; v[k] && v[k] != ',' && v[k] != ' ' && k < NAMEN - 1; k++) one[k] = v[k];
        one[k] = 0;
    }
    if (mime_header(me->head, me->headn, "Subject", v, sizeof v) < 0) v[0] = 0;
    {
        char t[LINE];
        mime_words(v, t, sizeof t);
        page_head(me, t[0] ? t : "(no subject)");
    }
    if (one[0]) {
        v[0] = '/';
        strcpy(v + 1, one);
        up(me, v, one);
    }
    outs(me, "<P>");
    field(me, "Subject", "Subject");
    field(me, "From", "From");
    field(me, "Date", "Date");
    if (mime_header(me->head, me->headn, "Newsgroups", v, sizeof v) > 0) {
        outs(me, "<B>Newsgroups:</B> ");
        for (p = v; *p; ) {
            while (*p == ',' || *p == ' ') p++;
            for (k = 0; p[k] && p[k] != ',' && p[k] != ' ' && k < NAMEN - 1; k++) one[k] = p[k];
            one[k] = 0;
            p += k;
            if (!k) break;
            if (H->UrlEncode(one, e, sizeof e) < 0) continue;
            outf(me, "<A HREF=\"/%s\">", (long)e, 0);
            out_text(me, one, k, millGuess);
            outs(me, "</A> ");
        }
        outs(me, "<BR>\n");
    }
    /* each earlier article it follows, by Message-ID (RFC 1036 2.2.5) */
    if (mime_header(me->head, me->headn, "References", v, sizeof v) > 0) {
        outs(me, "<B>References:</B>");
        for (p = v, i = 1; news_ref(&p, one, sizeof one); i++) {
            if (H->UrlEncode(one, e, sizeof e) < 0) continue;
            outf(me, " <A HREF=\"/%s\">%d</A>", (long)e, i);
        }
        outs(me, "<BR>\n");
    }
    outs(me, "<HR><PRE><FONT SIZE=2>");     /* 10 points: 80 columns fit a Plus's window */
    memset(&b, 0, sizeof b);
    body_kind(me, &b, 1);
    while (!me->stopped && next_line(me, &err)) body_line(me, &b);
    if (b.mode == kSkip) skipped(me, &b);
    outs(me, "</FONT></PRE>\n");
    return err;
}

/* ------------------------------------------------------------ a request */

OSErr news_request(MillPlugIn *self, MillRequest *r)
{
    News *me = (News *)self;
    char path[NAMEN], *q, *slash, note[NAMEN + 2];
    const char *query;
    long n, a[2];
    short code;
    OSErr err;
    me->req = r;
    me->r = r->ref;
    me->stopped = me->tried = me->broken = me->ready = 0;
    me->group[0] = 0;
    me->c = 0;
    me->have = me->at = 0;
    strncpy(me->host, r->host, NAMEN - 1);
    me->host[NAMEN - 1] = 0;
    /* who: the address's name and password, else the login kept for this server (its key:
       news://host:port, for both schemes) */
    a[0] = (long)r->host;
    a[1] = r->port ? r->port : 119;
    H->Format(me->key, sizeof me->key, "news://%s:%d", a);
    strncpy(me->user, r->user, NAMEN - 1);
    strncpy(me->pass, r->password, NAMEN - 1);
    me->user[NAMEN - 1] = me->pass[NAMEN - 1] = 0;
    if (!me->user[0]) H->SavedLogin(me->key, me->user, sizeof me->user, me->pass, sizeof me->pass);
    /* the address: /, /group, /group/number, or /message-id (RFC 1738 3.6, 3.7) */
    query = strchr(r->path, '?');
    n = query ? query - r->path : (long)strlen(r->path);
    if (query) query++;
    if (n >= NAMEN) n = NAMEN - 1;
    memcpy(path, r->path, n);
    path[n] = 0;
    H->UrlDecode(path, path, sizeof path);
    for (q = path; *q; q++) if ((unsigned char)*q <= ' ') {
        outs(me, "HTTP/1.0 404 Not Found" MILL_CRLF "Content-Type: text/html" MILL_CRLF MILL_CRLF "<TITLE>Not found</TITLE><P>No such group.");
        return noErr;
    }
    n = strlen(path);
    if (n > 1 && path[n - 1] == '/') path[--n] = 0;
    /* the connection kept from the last page, as this user: its note is the group it's in */
    n = sizeof note - 1;
    if (H->TcpKept(me->r, me->user, &me->c, note, &n) == noErr) {
        note[n] = 0;
        strcpy(me->group, note);
        goto in;
    }
    if ((err = H->TcpOpen(me->r, r->host, r->port ? r->port : 119, &me->c)) != noErr) { me->c = 0; goto quit; }
    /* the greeting: 200 or 201 (no posting) are in; RFC 977 2.4.3 */
    if ((err = get_line(me)) != noErr) goto quit;
    code = news_code(me->line);
    if (code != 200 && code != 201) { if (code) refused(me); else err = kGarbled; goto quit; }
    /* MODE READER: for a server that also takes feeds (RFC 2980 2.3); others say 500 */
    if ((err = command(me, "MODE READER", 0, &code)) != noErr) goto quit;
in:
    me->ready = 1;
    if (!path[1]) err = groups(me, query);
    else if (strchr(path, '@')) {
        q = path + 1;
        if (*q == '<') q++;
        if ((slash = strchr(q, '>')) != NULL) *slash = 0;
        err = article(me, NULL, 0, q);
    } else if ((slash = strchr(path + 1, '/')) != NULL) {
        long number = 0;
        *slash++ = 0;
        while (*slash >= '0' && *slash <= '9') number = number * 10 + (*slash++ - '0');
        if (*slash || !number) {
            outs(me, "HTTP/1.0 404 Not Found" MILL_CRLF "Content-Type: text/html" MILL_CRLF MILL_CRLF "<TITLE>Not found</TITLE><P>No such article.");
            goto quit;
        }
        err = article(me, path + 1, number, NULL);
    } else err = group(me, path + 1, query);
quit:
    memset(me->pass, 0, sizeof me->pass);
    if (me->c && me->ready && (err == noErr || err == kDone) && !me->stopped && !me->broken) {
        /* every reply read: kept for the next page (Mill says QUIT when it closes it) */
        H->TcpKeep(me->r, me->c, me->user, me->group, strlen(me->group), "QUIT" MILL_CRLF, 6);
        me->c = 0;
    }
    if (me->c) {
        if (err == noErr || err == kDone) send_line(me, "QUIT");    /* RFC 977 3.11 */
        H->TcpClose(me->c);
    }
    me->c = 0;
    if (err == kLost || err == kGarbled) {
        H->Fail(me->r, err == kLost ? "The connection was lost." : "The server\325s reply was not understood.");
        err = noErr;
    }
    return err > 0 ? noErr : err;
}

static const MillSetting settings[] = {
    { "server", "Default server", "Fills in the form on plugin://" NEWS_NAME "/about", millText, "", 0, 0, NULL },
    { "page", "Articles per page", NULL, millNumber, "50", 10, 200, NULL }
};
static const MillProtocol protocols[] = {
    { "news", "News", "Network News Transfer Protocol (RFC 977)", 0, 2, settings },
    { "nntp", "NNTP", "nntp: addresses for News (RFC 1738)", 0, 0, NULL }
};

const MillProtocol *news_protocols(void) { return protocols; }

MillPlugIn *news_state(const MillHost *host)
{
    News *me = host->NewMem(sizeof *me);
    if (!me) return NULL;
    me->base.methods = NULL;
    me->base.host = host;
    return &me->base;
}
