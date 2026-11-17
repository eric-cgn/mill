/* imaputil.c: see imaputil.h */
#include <string.h>

#include "imaputil.h"

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static short same_ci(const char *a, const char *b, long n)
{
    while (n-- > 0) if (lower(*a++) != lower(*b++)) return 0;
    return 1;
}

static long number(const char **p, const char *end)
{
    long n = 0;
    while (*p < end && **p >= '0' && **p <= '9') n = n * 10 + (*(*p)++ - '0');
    return n;
}

/* RFC 2060 9 (formal syntax): atom, quoted, literal ("{" number "}" CRLF *CHAR8), NIL, and
   "(" ... ")" */
short imap_tok(ImapIn *in, ImapTok *t)
{
    const char *p = in->p;
    t->s = p;
    t->n = 0;
    t->quoted = 0;
    while (p < in->end && (*p == ' ' || *p == '\r' || *p == '\n')) p++;
    if (p >= in->end) { in->p = p; return t->kind = kImapEnd; }
    if (*p == '(' || *p == ')') { in->p = p + 1; return t->kind = *p == '(' ? kImapOpen : kImapClose; }
    if (*p == '"') {
        const char *s = ++p;
        while (p < in->end && *p != '"') { if (*p == '\\' && p + 1 < in->end) p++; p++; }
        t->s = s;
        t->n = p - s;
        t->quoted = 1;
        in->p = p < in->end ? p + 1 : p;
        return t->kind = kImapString;
    }
    if (*p == '{') {
        long n;
        p++;
        n = number(&p, in->end);
        if (p < in->end && *p == '}') p++;
        if (p < in->end && *p == '\r') p++;
        if (p < in->end && *p == '\n') p++;
        if (n > in->end - p) n = in->end - p;
        t->s = p;
        t->n = n;
        in->p = p + n;
        return t->kind = kImapString;
    }
    t->s = p;
    /* an atom, or a fetch item with a section in brackets: BODY[1.2] */
    while (p < in->end && *p != ' ' && *p != '(' && *p != ')' && *p != '\r' && *p != '\n') {
        if (*p == '[') { while (p < in->end && *p != ']') p++; }
        if (p < in->end) p++;
    }
    t->n = p - t->s;
    in->p = p;
    return t->kind = t->n == 3 && same_ci(t->s, "NIL", 3) ? kImapNil : kImapAtom;
}

void imap_skip_list(ImapIn *in)
{
    ImapTok t;
    short depth = 1;
    while (depth > 0 && imap_tok(in, &t) != kImapEnd) {
        if (t.kind == kImapOpen) depth++;
        else if (t.kind == kImapClose) depth--;
    }
}

/* The next item, whatever it is: skipped */
static void skip_item(ImapIn *in)
{
    ImapTok t;
    if (imap_tok(in, &t) == kImapOpen) imap_skip_list(in);
}

long imap_str(const ImapTok *t, char *out, long size)
{
    long i, o = 0;
    if (t->kind == kImapString || t->kind == kImapAtom) {
        for (i = 0; i < t->n; i++) {
            char c = t->s[i];
            if (t->quoted && c == '\\' && i + 1 < t->n) c = t->s[++i];
            if (o < size - 1) out[o++] = c;
        }
    }
    if (size) out[o] = 0;
    return o;
}

long imap_literal(const char *line, long len)
{
    long n = 0, k = 1;
    if (len < 3 || line[len - 1] != '}') return -1;
    for (len -= 2; len >= 0 && line[len] >= '0' && line[len] <= '9'; len--, k *= 10) n += (line[len] - '0') * k;
    return len >= 0 && line[len] == '{' && k > 1 ? n : -1;
}

/* The next string (or NIL) of a list into out */
static void next_str(ImapIn *in, char *out, long size)
{
    ImapTok t;
    imap_tok(in, &t);
    if (t.kind == kImapOpen) { imap_skip_list(in); t.kind = kImapNil; }
    imap_str(&t, out, size);
}

/* ENVELOPE (RFC 2060 7.4.2): date, subject, from, sender, reply-to, to, cc, bcc,
   in-reply-to, message-id; an address is (name adl mailbox host) */
static void envelope(ImapIn *in, ImapMsg *m)
{
    ImapTok t;
    next_str(in, m->date, sizeof m->date);
    next_str(in, m->subject, sizeof m->subject);
    m->from[0] = 0;
    if (imap_tok(in, &t) == kImapOpen) {
        if (imap_tok(in, &t) == kImapOpen) {
            char name[80], box[64], host[64];
            next_str(in, name, sizeof name);
            next_str(in, box, sizeof box);      /* adl */
            next_str(in, box, sizeof box);
            next_str(in, host, sizeof host);
            imap_skip_list(in);
            if (name[0]) strcpy(m->from, name);
            else {
                long k = strlen(box);
                strcpy(m->from, box);
                if (host[0] && k + 1 + (long)strlen(host) < (long)sizeof m->from) { m->from[k] = '@'; strcpy(m->from + k + 1, host); }
            }
        } else if (t.kind == kImapClose) { /* () */ }
        if (t.kind != kImapClose) imap_skip_list(in);
    }
    imap_skip_list(in);         /* the rest of the envelope */
}

short imap_fetch(const char *s, long n, ImapMsg *m)
{
    ImapIn in;
    ImapTok t;
    const char *p;
    memset(m, 0, sizeof *m);
    in.p = s;
    in.end = s + n;
    if (imap_tok(&in, &t) != kImapAtom || t.n != 1 || *t.s != '*') return 0;
    if (imap_tok(&in, &t) != kImapAtom) return 0;
    p = t.s;
    m->seq = number(&p, t.s + t.n);
    if (imap_tok(&in, &t) != kImapAtom || t.n != 5 || !same_ci(t.s, "FETCH", 5)) return 0;
    if (imap_tok(&in, &t) != kImapOpen) return 0;
    while (imap_tok(&in, &t) == kImapAtom) {
        if (t.n == 3 && same_ci(t.s, "UID", 3)) {
            imap_tok(&in, &t);
            p = t.s;
            m->uid = number(&p, t.s + t.n);
        } else if (t.n == 11 && same_ci(t.s, "RFC822.SIZE", 11)) {
            imap_tok(&in, &t);
            p = t.s;
            m->size = number(&p, t.s + t.n);
        } else if (t.n == 5 && same_ci(t.s, "FLAGS", 5)) {
            if (imap_tok(&in, &t) == kImapOpen)
                while (imap_tok(&in, &t) == kImapAtom) if (t.n == 5 && same_ci(t.s, "\\Seen", 5)) m->seen = 1;
        } else if (t.n == 8 && same_ci(t.s, "ENVELOPE", 8)) {
            if (imap_tok(&in, &t) == kImapOpen) envelope(&in, m);
        } else skip_item(&in);
    }
    return 1;
}

short imap_list(const char *s, long n, char *name, long size, short *noselect)
{
    ImapIn in;
    ImapTok t;
    in.p = s;
    in.end = s + n;
    *noselect = 0;
    if (imap_tok(&in, &t) != kImapAtom || *t.s != '*') return 0;
    if (imap_tok(&in, &t) != kImapAtom || t.n != 4 || !same_ci(t.s, "LIST", 4)) return 0;
    if (imap_tok(&in, &t) != kImapOpen) return 0;
    while (imap_tok(&in, &t) == kImapAtom) if (t.n == 9 && same_ci(t.s, "\\Noselect", 9)) *noselect = 1;
    imap_tok(&in, &t);          /* the hierarchy delimiter */
    if (imap_tok(&in, &t) != kImapString && t.kind != kImapAtom) return 0;
    imap_str(&t, name, size);
    return 1;
}

/* One body (RFC 2060 7.4.2 BODYSTRUCTURE, after its "("): a multipart's parts, numbered
   prefix.1, prefix.2, ...; or a leaf, numbered prefix */
static void body(ImapIn *in, const char *prefix, ImapPart *parts, short max, short *count, short depth)
{
    ImapTok t;
    char sec[16];
    short i = 0;
    const char *save = in->p;
    if (imap_tok(in, &t) == kImapOpen) {
        /* multipart: its bodies, then its subtype and any extension data */
        do {
            long k = strlen(prefix);
            i++;
            if (k + 4 >= (long)sizeof sec) { imap_skip_list(in); continue; }
            if (k) { memcpy(sec, prefix, k); sec[k++] = '.'; }
            sec[k] = 0;
            {
                char num[6];
                short j = 0, v = i;
                do { num[j++] = (char)('0' + v % 10); v /= 10; } while (v && j < 5);
                while (j) sec[k++] = num[--j];
                sec[k] = 0;
            }
            if (depth < 8) body(in, sec, parts, max, count, depth + 1);
            else imap_skip_list(in);
        } while (imap_tok(in, &t) == kImapOpen);
        if (t.kind != kImapClose) imap_skip_list(in);
        return;
    }
    in->p = save;
    {
        ImapPart *p = *count < max ? &parts[*count] : NULL;
        char type[24], sub[24];
        long k;
        const char *q;
        (*count)++;
        next_str(in, type, sizeof type);
        next_str(in, sub, sizeof sub);
        if (p) {
            memset(p, 0, sizeof *p);
            strcpy(p->section, prefix[0] ? prefix : "1");
            for (k = 0; type[k]; k++) type[k] = lower(type[k]);
            for (k = 0; sub[k]; k++) sub[k] = lower(sub[k]);
            k = strlen(type);
            if (k + 1 + (long)strlen(sub) < (long)sizeof p->type) { strcpy(p->type, type); p->type[k] = '/'; strcpy(p->type + k + 1, sub); }
        }
        /* body-fld-param: ("charset" "x" "name" "y") or NIL */
        if (imap_tok(in, &t) == kImapOpen) {
            char a[16], v[48];
            while (imap_tok(in, &t) == kImapString || t.kind == kImapAtom) {
                imap_str(&t, a, sizeof a);
                next_str(in, v, sizeof v);
                if (p && same_ci(a, "charset", 8)) strncpy(p->charset, v, sizeof p->charset - 1);
                if (p && same_ci(a, "name", 5)) strncpy(p->name, v, sizeof p->name - 1);
            }
        }
        skip_item(in);          /* body-fld-id */
        skip_item(in);          /* body-fld-desc */
        if (p) {
            next_str(in, p->encoding, sizeof p->encoding);
            for (k = 0; p->encoding[k]; k++) p->encoding[k] = lower(p->encoding[k]);
        } else skip_item(in);
        imap_tok(in, &t);       /* body-fld-octets */
        q = t.s;
        if (p) p->size = number(&q, t.s + t.n);
        imap_skip_list(in);     /* lines, envelope and body of a message/rfc822, extensions */
    }
}

short imap_parts(const char *s, long n, ImapPart *parts, short max)
{
    ImapIn in;
    ImapTok t;
    short count = 0;
    const char *b;
    for (b = s; b + 13 <= s + n; b++) if (same_ci(b, "BODYSTRUCTURE", 13)) break;
    if (b + 13 > s + n) return 0;
    in.p = b + 13;
    in.end = s + n;
    if (imap_tok(&in, &t) != kImapOpen) return 0;
    body(&in, "", parts, max, &count, 0);
    return count;
}

static short hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static void decode(const char *s, long n, char *out, long size)
{
    long i, o = 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == '%' && i + 2 < n && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            c = (char)(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            i += 2;
        }
        if (o < size - 1) out[o++] = c;
    }
    out[o] = 0;
}

/* RFC 2192 3: imessagepart = enc_mailbox [uidvalidity] iuid [isection], iuid = "/;UID=" nz_number;
   imailboxlist / imessagelist = enc_mailbox [uidvalidity] ["?" enc_search] */
void imap_url(const char *path, ImapUrl *u)
{
    const char *p = path, *end, *semi, *q;
    memset(u, 0, sizeof *u);
    if (*p == '/') p++;
    q = strchr(p, '?');
    end = q ? q : p + strlen(p);
    for (semi = p; semi < end && *semi != ';'; semi++) {}
    /* "/;UID=" ends the mailbox with a slash before its semicolon */
    decode(p, semi > p && semi[-1] == '/' ? semi - 1 - p : semi - p, u->mailbox, sizeof u->mailbox);
    for (p = semi; p < end; ) {
        const char *e;
        for (e = p + 1; e < end && *e != ';' && *e != '/'; e++) {}
        if (e - p > 5 && same_ci(p, ";UID=", 5)) { q = p + 5; u->uid = number(&q, e); }
        else if (e - p > 13 && same_ci(p, ";UIDVALIDITY=", 13)) { q = p + 13; u->uidvalidity = number(&q, e); }
        else if (e - p > 10 && same_ci(p, ";X-BEFORE=", 10)) { q = p + 10; u->before = number(&q, e); }
        p = e;
        while (p < end && *p == '/') p++;
    }
    if (end[0] == '?') decode(end + 1, strlen(end + 1), u->search, sizeof u->search);
}

long imap_quote(const char *s, char *out, long size)
{
    long o = 0;
    if (size < 3) return -1;
    out[o++] = '"';
    for (; *s; s++) {
        if (*s == '\r' || *s == '\n') return -1;
        if (*s == '"' || *s == '\\') { if (o >= size - 3) return -1; out[o++] = '\\'; }
        if (o >= size - 2) return -1;
        out[o++] = *s;
    }
    out[o++] = '"';
    out[o] = 0;
    return o;
}
