/* imap: in the IMAP Plug-In: reading mail on an IMAP server, read-only, with addresses as
   RFC 2192 has them:
     imap://user@host/                          the mailboxes (LIST)
     imap://user@host/mailbox                   its messages, newest first (EXAMINE, FETCH)
     imap://user@host/mailbox?search            those a SEARCH finds
     imap://user@host/mailbox/;UID=n            a message: its headers and text
   Commands both IMAP4 (RFC 1730) and IMAP4rev1 (RFC 2060) have: LOGIN, LIST, EXAMINE (so
   nothing changes), FETCH and UID FETCH with ENVELOPE, FLAGS, RFC822.SIZE, RFC822.HEADER,
   BODYSTRUCTURE, and BODY.PEEK[n] (reading doesn't mark a message read), UID SEARCH, and
   LOGOUT. The text is the first text/plain part (else the first text part), decoded
   (RFC 2045) and shown in Mac Roman; other parts are named, not shown. The connection is
   kept between pages (TcpKeep), logged in, with the mailbox it has open: a message in it
   needs no EXAMINE. */
#include <string.h>

#include "MillPlugIn.h"
#include "imap.h"
#include "imaputil.h"
#include "mime.h"

enum { NAMEN = 256, LINE = 1024, BUF = 1024, RESP = 4096, HEAD = 3072, ESC = LINE * 6, PARTS = 12 };
enum { kLost = -30201, kGarbled = -30202, kDone = 1 };
enum { kUntagged = 1, kOk, kNo, kBad };

typedef struct {
    MillPlugIn base;
    MillRequest *req;
    MillReqRef r;
    MillConn c;
    short stopped;
    long tag, have, at, len, lit;   /* lit: the literal the line ends with, or -1 */
    unsigned long uidvalidity, exists;
    char open[NAMEN];           /* the mailbox EXAMINE opened on this connection, "" for none */
    char host[NAMEN], key[NAMEN + 16], user[NAMEN], pass[NAMEN];
    ImapUrl u;
    char line[LINE];
    char buf[BUF];
    char work[LINE];
    char esc[ESC];
    char resp[RESP];
    long respn;
    char head[HEAD];
    long headn;
} Imap;

#define H (me->base.host)

/* ------------------------------------------------------------ the page */

static void out(Imap *me, const char *s, long n)
{
    if (!me->stopped && n > 0 && !H->Write(me->r, s, n)) me->stopped = 1;
}

static void outs(Imap *me, const char *s) { out(me, s, strlen(s)); }

static void outf(Imap *me, const char *fmt, long a, long b)
{
    long args[2];
    args[0] = a;
    args[1] = b;
    if (!me->stopped && !H->Printf(me->r, fmt, args)) me->stopped = 1;
}

static void out_text(Imap *me, const char *s, long n, short cs)
{
    if (n > LINE - 1) n = LINE - 1;
    if (n > 0 && H->ToMacRoman(s, n, cs, me->work, sizeof me->work) >= 0 && H->HtmlEscape(me->work, me->esc, sizeof me->esc) >= 0)
        outs(me, me->esc);
}

/* A header's text with its encoded words decoded (RFC 1522) */
static void out_words(Imap *me, const char *s)
{
    char d[LINE];
    short cs = mime_words(s, d, sizeof d);
    out_text(me, d, strlen(d), cs);
}

static void icon(Imap *me, short id)
{
    outf(me, "<IMG SRC=\"plugin://" IMAP_NAME "/rsrc?type=ics%%23&id=%d\" WIDTH=16 HEIGHT=16 ALIGN=TOP> ", id, 0);
}

static void page_head(Imap *me, const char *title)
{
    H->PlugInPage(me->r);
    outs(me, "HTTP/1.0 200 OK" MILL_CRLF "Content-Type: text/html; charset=macintosh" MILL_CRLF MILL_CRLF "<TITLE>" IMAP_TITLE ": ");
    out_text(me, title, strlen(title), millGuess);
    outs(me, "</TITLE>\n");
}

/* A link to a mailbox (and anything after it in the address) */
static void box_link(Imap *me, const char *box, const char *after, const char *text)
{
    char e[NAMEN * 3];
    if (H->UrlEncode(box, e, sizeof e) < 0) return;
    outf(me, "<A HREF=\"/%s%s\">", (long)e, (long)after);
    out_text(me, text, strlen(text), millGuess);
    outs(me, "</A>");
}

/* The server's words after a tagged NO or BAD, or BYE, in an alert */
static void refused(Imap *me, const char *line)
{
    char text[LINE + 40];
    const char *p = strchr(line, ' ');
    long a[1];
    if (p) p = strchr(p + 1, ' ');      /* past the tag and NO */
    a[0] = (long)(p ? p + 1 : line);
    H->Format(text, sizeof text, "The server refused it: %s", a);
    H->Fail(me->r, text);
}

/* ------------------------------------------------------------ the connection */

/* One line into line (cut to fit), without CR LF; lit is the literal it ends with, or -1 */
static OSErr get_line(Imap *me)
{
    long k = 0;
    for (;;) {
        while (me->at < me->have) {
            char ch = me->buf[me->at++];
            if (ch == 10) {
                if (k && me->line[k - 1] == 13) k--;
                me->line[k] = 0;
                me->len = k;
                me->lit = imap_literal(me->line, k);
                return noErr;
            }
            if (k < LINE - 1) me->line[k++] = ch;
        }
        me->have = BUF;
        me->at = 0;
        {
            OSErr err = H->TcpRecv(me->c, me->buf, &me->have);
            if (err != noErr) { me->have = 0; return err; }
            if (!me->have) return kLost;
        }
    }
}

/* n bytes of a literal: up to size kept in out (NULL: none), the rest read and dropped.
   Returns how many were kept. */
static long get_bytes(Imap *me, long n, char *out, long size)
{
    long kept = 0;
    while (n > 0) {
        long k = me->have - me->at;
        if (k <= 0) {
            me->have = BUF;
            me->at = 0;
            if (H->TcpRecv(me->c, me->buf, &me->have) != noErr || !me->have) { me->have = 0; return -1; }
            continue;
        }
        if (k > n) k = n;
        if (out && kept < size) {
            long c = k < size - kept ? k : size - kept;
            memcpy(out + kept, me->buf + me->at, c);
            kept += c;
        }
        me->at += k;
        n -= k;
    }
    return kept;
}

/* A whole response into resp, its literals in place ("{n}" CR LF and the bytes kept, n
   being what was kept): kUntagged, or for our tag kOk, kNo, kBad */
static short response(Imap *me, OSErr *err)
{
    char t[16];
    long a[1], k;
    me->respn = 0;
    if ((*err = get_line(me)) != noErr) return 0;
    for (;;) {
        k = me->len;
        if (me->lit >= 0) while (k && me->line[k - 1] != '{') k--;     /* the line up to its "{" */
        if (me->respn + k + 24 < RESP) { memcpy(me->resp + me->respn, me->line, k); me->respn += k; }
        if (me->lit < 0) break;
        {
            long room = RESP - me->respn - 24, n = me->lit, kept;
            char *at;
            if (room < 0) room = 0;
            if (n > room) n = room;
            a[0] = n;
            at = me->resp + me->respn + H->Format(me->resp + me->respn, 24, "%ld}\r\n", a);
            if ((kept = get_bytes(me, me->lit, at, n)) < 0) { *err = kLost; return 0; }
            me->respn = at - me->resp + kept;
            if ((*err = get_line(me)) != noErr) return 0;
        }
    }
    me->resp[me->respn] = 0;
    if (me->resp[0] == '*') return kUntagged;
    a[0] = me->tag;
    k = H->Format(t, sizeof t, "a%ld ", a);
    if (strncmp(me->resp, t, k)) return kUntagged;      /* a continuation, or another's */
    return !strncmp(me->resp + k, "OK", 2) ? kOk : !strncmp(me->resp + k, "NO", 2) ? kNo : kBad;
}

/* Send a command (fmt with one string or number), tagged a1, a2, ... */
static OSErr command(Imap *me, const char *fmt, long arg)
{
    char l[LINE + 24];
    long a[2], n;
    a[0] = ++me->tag;
    n = H->Format(l, sizeof l, "a%ld ", a);
    a[0] = arg;
    n += H->Format(l + n, sizeof l - n - 2, fmt, a);
    l[n++] = 13;
    l[n++] = 10;
    return H->TcpSend(me->c, l, n);
}

/* Read to the command's tagged response, ignoring untagged ones: its kOk, kNo, kBad (the
   line in line), or 0 with *err */
static short finish(Imap *me, OSErr *err)
{
    short st;
    while ((st = response(me, err)) == kUntagged) {}
    if (st) { strncpy(me->line, me->resp, LINE - 1); me->line[LINE - 1] = 0; }
    return st;
}

/* Ask for a name and password: first (wrong 0), or again after the server refused them */
static void ask(Imap *me, short wrong)
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

/* ------------------------------------------------------------ mailboxes */

/* LIST "" "*" (RFC 2060 6.3.8, RFC 1730 6.3.8): each mailbox a line; those that can't be
   opened (\Noselect) without a link */
static OSErr mailboxes(Imap *me)
{
    char name[NAMEN];
    short st, noselect, n = 0;
    OSErr err;
    if ((err = command(me, "LIST \"\" \"*\"", 0)) != noErr) return err;
    page_head(me, me->host);
    outs(me, "<H2>");
    out_text(me, me->host, strlen(me->host), millGuess);
    outs(me, "</H2>\n");
    while ((st = response(me, &err)) == kUntagged) {
        if (!imap_list(me->resp, me->respn, name, sizeof name, &noselect)) continue;
        icon(me, kMillIconFolder);
        if (noselect) out_text(me, name, strlen(name), millGuess);
        else box_link(me, name, "", name);
        outs(me, "<BR>\n");
        n++;
    }
    if (!st) return err;
    if (!n) outs(me, "<P>No mailboxes.\n");
    return noErr;
}

/* EXAMINE (read-only: RFC 2060 6.3.2): the message count and UIDVALIDITY. Unless again
   is set, the mailbox open already will do. */
static OSErr examine(Imap *me, short again)
{
    char q[NAMEN + 8];
    const char *p;
    short st;
    OSErr err;
    if (!again && me->open[0] && !strcmp(me->open, me->u.mailbox)) return noErr;
    if (imap_quote(me->u.mailbox, q, sizeof q) < 0) { H->Fail(me->r, "No such mailbox."); return kDone; }
    if ((err = command(me, "EXAMINE %s", (long)q)) != noErr) return err;
    me->open[0] = 0;        /* a failed EXAMINE closes the one open (RFC 2060 6.3.1) */
    me->exists = me->uidvalidity = 0;
    while ((st = response(me, &err)) == kUntagged) {
        if ((p = strstr(me->resp, " EXISTS")) != NULL) {
            while (p > me->resp && p[-1] >= '0' && p[-1] <= '9') p--;
            for (me->exists = 0; *p >= '0' && *p <= '9'; p++) me->exists = me->exists * 10 + (*p - '0');
        } else if ((p = strstr(me->resp, "[UIDVALIDITY ")) != NULL)
            for (p += 13, me->uidvalidity = 0; *p >= '0' && *p <= '9'; p++) me->uidvalidity = me->uidvalidity * 10 + (*p - '0');
    }
    if (!st) return err;
    if (st != kOk) {
        strncpy(me->line, me->resp, LINE - 1);
        me->line[LINE - 1] = 0;
        H->Fail(me->r, "No such mailbox.");
        return kDone;
    }
    strcpy(me->open, me->u.mailbox);
    return noErr;
}

/* A mailbox's messages, a page of them, newest first: FETCH by number, or UID FETCH of the
   UIDs a search found (RFC 2060 6.4.4, 6.4.5, 6.4.8) */
static OSErr messages(Imap *me)
{
    char v[16], set[RESP / 2], after[48];
    long page = 30, lo, hi, n = 0, i, a[2];
    ImapMsg *rows;
    short st;
    OSErr err;
    const char *p;
    if (H->GetSetting("imap", "page", v, sizeof v) > 0) {
        for (page = 0, p = v; *p >= '0' && *p <= '9'; p++) page = page * 10 + (*p - '0');
        if (page < 10) page = 10;
        if (page > 200) page = 200;
    }
    if ((err = examine(me, 1)) != noErr) return err;      /* the count now */
    hi = me->u.before > 0 && (unsigned long)me->u.before - 1 < me->exists ? me->u.before - 1 : (long)me->exists;
    lo = hi - page + 1 < 1 ? 1 : hi - page + 1;
    set[0] = 0;
    if (me->u.search[0]) {
        /* UID SEARCH: the last page of what it finds. The search goes in the command as it
           is, so a line end in it (another command, from someone else's link) is refused. */
        long uids[200], count = 0;
        for (p = me->u.search; *p; p++) if ((unsigned char)*p < 32) { H->Fail(me->r, "The search wasn\325t understood."); return kDone; }
        if ((err = command(me, "UID SEARCH %s", (long)me->u.search)) != noErr) return err;
        while ((st = response(me, &err)) == kUntagged) {
            if (strncmp(me->resp, "* SEARCH", 8)) continue;
            for (p = me->resp + 8; *p; ) {
                long u = 0;
                while (*p == ' ') p++;
                while (*p >= '0' && *p <= '9') u = u * 10 + (*p++ - '0');
                if (u) { if (count == 200) { memmove(uids, uids + 1, 199 * sizeof(long)); count--; } uids[count++] = u; }
                else if (*p) p++;
            }
        }
        if (!st) return err;
        if (st != kOk) { refused(me, me->resp); return kDone; }
        for (i = count > page ? count - page : 0; i < count; i++) {
            long k = strlen(set);
            a[0] = uids[i];
            if (k + 12 < (long)sizeof set) H->Format(set + k, 12, k ? ",%ld" : "%ld", a);
        }
        lo = 1;
        hi = 0;
    }
    if (!(rows = H->NewMem(page * sizeof(ImapMsg)))) return memFullErr;
    H->Status(me->r, "Reading\311");
    if (set[0]) err = command(me, "UID FETCH %s (UID FLAGS RFC822.SIZE ENVELOPE)", (long)set);
    else if (me->exists && hi >= lo) {
        a[0] = lo;
        a[1] = hi;
        H->Format(after, sizeof after, "%ld:%ld", a);
        err = command(me, "FETCH %s (UID FLAGS RFC822.SIZE ENVELOPE)", (long)after);
    } else set[0] = 0;
    if (err != noErr) goto done;
    if (set[0] || (me->exists && hi >= lo)) {
        while ((st = response(me, &err)) == kUntagged)
            if (n < page && imap_fetch(me->resp, me->respn, &rows[n]) && rows[n].uid) n++;
        if (!st) goto done;
    }
    page_head(me, me->u.mailbox);
    icon(me, kMillIconParent);
    outs(me, "<A HREF=\"/\">Mailboxes</A>\n<H2>");
    out_text(me, me->u.mailbox, strlen(me->u.mailbox), millGuess);
    outs(me, "</H2>\n");
    if (!n) outs(me, me->u.search[0] ? "<P>No messages match.\n" : "<P>No messages.\n");
    else {
        outs(me, "<TABLE CELLSPACING=0 CELLPADDING=2><TR><TH ALIGN=LEFT>Subject</TH><TH ALIGN=LEFT>From</TH>"
                 "<TH ALIGN=LEFT>Date</TH><TH ALIGN=RIGHT>Size</TH></TR>\n");
        a[0] = me->uidvalidity;
        H->Format(after, sizeof after, ";UIDVALIDITY=%lu/;UID=", a);
        for (i = n - 1; i >= 0 && !me->stopped; i--) {
            ImapMsg *m = &rows[i];
            char t[LINE], w[LINE];
            long k = strlen(after);
            a[0] = m->uid;
            H->Format(after + k, sizeof after - k, "%lu", a);
            outs(me, m->seen ? "<TR><TD>" : "<TR><TD><B>");    /* unread in bold */
            {
                char e[NAMEN * 3];
                if (H->UrlEncode(me->u.mailbox, e, sizeof e) >= 0) outf(me, "<A HREF=\"/%s%s\">", (long)e, (long)after);
            }
            if (m->subject[0]) out_words(me, m->subject);
            else outs(me, "(no subject)");
            outs(me, m->seen ? "</A></TD><TD>" : "</A></B></TD><TD>");
            mime_words(m->from, w, sizeof w);
            mime_who(w, t, sizeof t);
            out_text(me, t, strlen(t), millGuess);
            outs(me, "</TD><TD>");
            mime_day(m->date, t, sizeof t);
            out_text(me, t, strlen(t), millGuess);
            outf(me, "</TD><TD ALIGN=RIGHT>%ldK</TD></TR>\n", (m->size + 1023) / 1024, 0);
            after[k] = 0;
        }
        outs(me, "</TABLE>\n");
    }
    if (!me->u.search[0] && (lo > 1 || (unsigned long)hi < me->exists)) {
        char e[NAMEN * 3];
        if (H->UrlEncode(me->u.mailbox, e, sizeof e) < 0) e[0] = 0;
        outs(me, "<P>");
        if ((unsigned long)hi < me->exists) {
            if ((unsigned long)(hi + page) >= me->exists) outf(me, "<A HREF=\"/%s\">Newer messages</A> &nbsp; ", (long)e, 0);
            else { a[0] = (long)e; a[1] = hi + page + 1; outf(me, "<A HREF=\"/%s;X-BEFORE=%ld\">Newer messages</A> &nbsp; ", a[0], a[1]); }
        }
        if (lo > 1) outf(me, "<A HREF=\"/%s;X-BEFORE=%ld\">Older messages</A>", (long)e, lo);
        outs(me, "\n");
    }
done:
    H->FreeMem(rows);
    return err;
}

/* ------------------------------------------------------------ a message */

typedef struct {
    short cte, cs;      /* 0 as it is, 1 quoted-printable, 2 base64; the charset */
    MimeB64 b64;
    char line[LINE];
    long n;
} Text;

/* A line of the text, decoded */
static void text_line(Imap *me, Text *t, char *s, long n)
{
    short soft = 0;
    if (t->cte == 2) {
        unsigned char d[LINE];
        long k = mime_b64(&t->b64, s, n, d), i, o = 0;
        for (i = 0; i < k; i++) if (d[i] != 13) d[o++] = d[i];
        out_text(me, (char *)d, o, t->cs);
        return;
    }
    if (t->cte == 1) n = mime_qp(s, n, &soft);
    out_text(me, s, n, t->cs);
    if (!soft) outs(me, "\n");
}

/* n bytes of the text part, as they come: in lines */
static OSErr text_bytes(Imap *me, Text *t, long n)
{
    while (n > 0 && !me->stopped) {
        char c;
        if (me->at >= me->have) {
            me->have = BUF;
            me->at = 0;
            if (H->TcpRecv(me->c, me->buf, &me->have) != noErr || !me->have) { me->have = 0; return kLost; }
        }
        c = me->buf[me->at++];
        n--;
        if (c == 10) {
            if (t->n && t->line[t->n - 1] == 13) t->n--;
            text_line(me, t, t->line, t->n);
            t->n = 0;
        } else if (t->n < LINE - 1) t->line[t->n++] = c;
    }
    if (n > 0) get_bytes(me, n, NULL, 0);      /* stopped: the rest is read and dropped */
    return noErr;
}

static void field(Imap *me, const char *label, const char *name)
{
    char v[LINE];
    if (mime_header(me->head, me->headn, name, v, sizeof v) <= 0) return;
    outf(me, "<B>%s:</B> ", (long)label, 0);
    out_words(me, v);
    outs(me, "<BR>\n");
}

/* UID FETCH: BODYSTRUCTURE, RFC822.HEADER, then BODY.PEEK[n] of the text */
static OSErr message(Imap *me)
{
    ImapPart parts[PARTS];
    short np = 0, i, text = -1, st;
    char cmd[64];
    long a[2];
    Text t;
    OSErr err;
    if ((err = examine(me, 0)) != noErr) return err;
    if (me->u.uidvalidity && me->u.uidvalidity != me->uidvalidity) {
        H->Fail(me->r, "The mailbox has changed since. Open it again.");
        return kDone;
    }
    if ((err = command(me, "UID FETCH %lu (BODYSTRUCTURE)", (long)me->u.uid)) != noErr) return err;
    while ((st = response(me, &err)) == kUntagged) {
        if (!np) np = imap_parts(me->resp, me->respn, parts, PARTS);
    }
    if (!st) return err;
    if (!np) { H->Fail(me->r, "No such message."); return kDone; }
    if (np > PARTS) np = PARTS;
    for (i = 0; i < np && text < 0; i++) if (!strcmp(parts[i].type, "text/plain")) text = i;
    for (i = 0; i < np && text < 0; i++) if (!strncmp(parts[i].type, "text/", 5) && strcmp(parts[i].type, "text/html")) text = i;
    /* the headers: RFC822.HEADER (BODY.PEEK[HEADER] in IMAP4rev1; neither marks it read) */
    if ((err = command(me, "UID FETCH %lu (RFC822.HEADER)", (long)me->u.uid)) != noErr) return err;
    me->headn = 0;
    while ((st = response(me, &err)) == kUntagged) {
        char *b = strstr(me->resp, "}\r\n");
        if (b && !me->headn) {
            long k = me->respn - (b + 3 - me->resp);
            if (k > HEAD) k = HEAD;
            memcpy(me->head, b + 3, k);
            me->headn = k;
        }
    }
    if (!st) return err;
    {
        char v[LINE], s[LINE];
        if (mime_header(me->head, me->headn, "Subject", v, sizeof v) < 0) v[0] = 0;
        mime_words(v, s, sizeof s);
        page_head(me, s[0] ? s : "(no subject)");
    }
    icon(me, kMillIconParent);
    box_link(me, me->u.mailbox, "", me->u.mailbox);
    outs(me, "\n<P>");
    field(me, "Subject", "Subject");
    field(me, "From", "From");
    field(me, "To", "To");
    field(me, "Cc", "Cc");
    field(me, "Date", "Date");
    outs(me, "<HR><PRE><FONT SIZE=2>");     /* 10 points: 80 columns fit a Plus's window */
    if (text >= 0) {
        memset(&t, 0, sizeof t);
        t.cte = !strcmp(parts[text].encoding, "quoted-printable") ? 1 : !strcmp(parts[text].encoding, "base64") ? 2 : 0;
        t.cs = parts[text].charset[0] ? mime_charset(parts[text].charset) : millGuess;
        a[0] = (long)me->u.uid;
        a[1] = (long)parts[text].section;
        H->Format(cmd, sizeof cmd, "UID FETCH %lu (BODY.PEEK[%s])", a);
        if ((err = command(me, "%s", (long)cmd)) != noErr) return err;
        H->Status(me->r, "Reading\311");
        /* "* n FETCH (UID u BODY[1] {size}", the bytes, then ")" */
        while (!me->stopped) {
            if ((err = get_line(me)) != noErr) return err;
            if (me->line[0] != '*') break;
            if (me->lit >= 0) {
                if ((err = text_bytes(me, &t, me->lit)) != noErr) return err;
                if (t.n) text_line(me, &t, t.line, t.n);
                if ((err = get_line(me)) != noErr) return err;
            }
        }
    } else outs(me, "No text to show.\n");
    for (i = 0; i < np; i++) {
        if (i == text) continue;
        outs(me, text >= 0 && (i == 0 || (i == 1 && text == 0)) ? "\nNot shown: " : "Not shown: ");
        if (parts[i].name[0]) {
            char w[LINE];
            mime_words(parts[i].name, w, sizeof w);
            out_text(me, w, strlen(w), millGuess);
            outs(me, ", ");
        }
        out_text(me, parts[i].type, strlen(parts[i].type), millGuess);
        a[0] = (parts[i].size * (!strcmp(parts[i].encoding, "base64") ? 3 : 4) / 4 + 1023) / 1024;
        outf(me, " (%ldK)\n", a[0], 0);
    }
    outs(me, "</FONT></PRE>\n");
    return noErr;
}

/* ------------------------------------------------------------ a request */

OSErr imap_request(MillPlugIn *self, MillRequest *r)
{
    Imap *me = (Imap *)self;
    char qu[NAMEN + 8], qp[NAMEN + 8], *semi, saved[NAMEN], note[NAMEN + 32];
    long a[2], n;
    short st;
    OSErr err;
    me->req = r;
    me->r = r->ref;
    me->stopped = 0;
    me->c = 0;
    me->tag = me->have = me->at = 0;
    strncpy(me->host, r->host, NAMEN - 1);
    me->host[NAMEN - 1] = 0;
    imap_url(r->path, &me->u);
    /* who: the address's user (RFC 2192 3.2: user;AUTH=type, only LOGIN here), with the
       password kept for this server (its key: imap://host:port) if it's the same user;
       else the login kept; else Mill asks */
    a[0] = (long)r->host;
    a[1] = r->port ? r->port : 143;
    H->Format(me->key, sizeof me->key, "imap://%s:%d", a);
    strncpy(me->user, r->user, NAMEN - 1);
    me->user[NAMEN - 1] = 0;
    if ((semi = strchr(me->user, ';')) != NULL) *semi = 0;
    me->pass[0] = saved[0] = 0;
    if (!H->SavedLogin(me->key, saved, sizeof saved, me->pass, sizeof me->pass) ||
        (me->user[0] && strcmp(saved, me->user))) { ask(me, 0); return noErr; }
    if (!me->user[0]) strcpy(me->user, saved);
    me->open[0] = 0;
    /* the connection kept from the last page, logged in as this user: its note is the
       last tag, the open mailbox's UIDVALIDITY, and its name, "12 880000000 INBOX" */
    n = sizeof note - 1;
    if (H->TcpKept(me->r, me->user, &me->c, note, &n) == noErr) {
        const char *p = note;
        note[n] = 0;
        while (*p >= '0' && *p <= '9') me->tag = me->tag * 10 + (*p++ - '0');
        if (*p == ' ') for (p++, me->uidvalidity = 0; *p >= '0' && *p <= '9'; p++) me->uidvalidity = me->uidvalidity * 10 + (*p - '0');
        if (*p == ' ' && strlen(p + 1) < NAMEN) { strcpy(me->open, p + 1); memset(me->pass, 0, sizeof me->pass); goto in; }
        H->TcpClose(me->c);
        me->c = 0;
        me->tag = 0;
    }
    if ((err = H->TcpOpen(me->r, r->host, r->port ? r->port : 143, &me->c)) != noErr) { me->c = 0; return err; }
    H->Status(me->r, "Logging in\311");
    /* the greeting: OK, PREAUTH (already in), or BYE (RFC 2060 7.1) */
    if ((err = get_line(me)) != noErr) goto quit;
    if (strncmp(me->line, "* OK", 4) && strncmp(me->line, "* PREAUTH", 9)) {
        if (!strncmp(me->line, "* BYE", 5)) { refused(me, me->line); err = kDone; } else err = kGarbled;
        goto quit;
    }
    if (me->line[2] == 'O') {
        /* LOGIN (RFC 2060 6.2.2, RFC 1730 6.2.2) */
        if (imap_quote(me->user, qu, sizeof qu) < 0 || imap_quote(me->pass, qp, sizeof qp) < 0) { ask(me, 1); goto quit; }
        a[0] = (long)qu;
        a[1] = (long)qp;
        H->Format(me->work, sizeof me->work, "LOGIN %s %s", a);
        memset(qp, 0, sizeof qp);
        err = command(me, "%s", (long)me->work);
        memset(me->work, 0, sizeof me->work);
        if (err != noErr || !(st = finish(me, &err))) goto quit;
        if (st != kOk) { ask(me, 1); goto quit; }
    }
in:
    if (!me->u.mailbox[0]) err = mailboxes(me);
    else if (me->u.uid) err = message(me);
    else err = messages(me);
quit:
    memset(me->pass, 0, sizeof me->pass);
    if (me->c && err == noErr && !me->stopped) {
        /* every response read: kept for the next page (Mill says LOGOUT when it closes it) */
        char bye[24];
        long b[3];
        b[0] = me->tag;
        b[1] = me->open[0] ? me->uidvalidity : 0;
        b[2] = (long)me->open;
        n = H->Format(note, sizeof note, "%ld %lu %s", b);
        a[0] = me->tag + 1;
        H->Format(bye, sizeof bye, "a%ld LOGOUT" MILL_CRLF, a);
        if (n < (long)sizeof note - 1) { H->TcpKeep(me->r, me->c, me->user, note, n, bye, strlen(bye)); me->c = 0; }
    }
    if (me->c) {
        if (err == noErr || err == kDone) command(me, "LOGOUT", 0);   /* RFC 2060 6.1.3 */
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
    { "server", "Default server", "Fills in the form on plugin://" IMAP_NAME "/about", millText, "", 0, 0, NULL },
    { "page", "Messages per page", NULL, millNumber, "30", 10, 200, NULL }
};
static const MillProtocol protocols[] = { { "imap", "IMAP", "Internet Message Access Protocol (RFC 2060)", 0, 2, settings } };

const MillProtocol *imap_protocol(void) { return protocols; }

MillPlugIn *imap_state(const MillHost *host)
{
    Imap *me = host->NewMem(sizeof *me);
    if (!me) return NULL;
    me->base.methods = NULL;
    me->base.host = host;
    return &me->base;
}
