/* mime.c: see mime.h */
#include <string.h>

#include "mime.h"

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static short same_ci(const char *a, const char *b, long n)
{
    while (n-- > 0) if (lower(*a++) != lower(*b++)) return 0;
    return 1;
}

static short space(char c) { return c == ' ' || c == '\t'; }

/* RFC 822 3.1.1, 3.2: "name:" at a line's start, then the value, with the lines that start
   with white space after it */
long mime_header(const char *head, long len, const char *name, char *out, long size)
{
    const char *p = head, *end = head + len;
    long n = strlen(name), o = 0;
    while (p < end) {
        const char *eol = memchr(p, '\n', end - p);
        if (!eol) eol = end;
        if (eol - p > n && same_ci(p, name, n) && p[n] == ':') {
            const char *v = p + n + 1;
            for (;;) {
                const char *e = eol;
                if (e > v && e[-1] == '\r') e--;
                for (; v < e; v++) {
                    if (o == 0 && space(*v)) continue;
                    if (*v == '\t') { if (o < size - 1) out[o++] = ' '; continue; }
                    if (o < size - 1) out[o++] = *v;
                }
                if (eol >= end || eol + 1 >= end || !space(eol[1])) break;
                v = eol + 1;        /* a continuation line: joined with one space */
                while (v < end && space(*v)) v++;
                if (o && o < size - 1) out[o++] = ' ';
                eol = memchr(v, '\n', end - v);
                if (!eol) eol = end;
            }
            while (o && space(out[o - 1])) o--;
            out[o] = 0;
            return o;
        }
        p = eol + 1;
    }
    if (size) out[0] = 0;
    return -1;
}

/* RFC 2045 5.1: parameter := attribute "=" value, value := token / quoted-string */
long mime_param(const char *value, const char *name, char *out, long size)
{
    const char *p = value;
    long n = strlen(name), o = 0;
    while ((p = strchr(p, ';')) != NULL) {
        p++;
        while (space(*p)) p++;
        if (same_ci(p, name, n) && (p[n] == '=' || space(p[n]))) {
            p += n;
            while (space(*p)) p++;
            if (*p++ != '=') continue;
            while (space(*p)) p++;
            if (*p == '"') {
                for (p++; *p && *p != '"'; p++) {
                    if (*p == '\\' && p[1]) p++;
                    if (o < size - 1) out[o++] = *p;
                }
            } else {
                for (; *p && *p != ';' && !space(*p); p++) if (o < size - 1) out[o++] = *p;
            }
            out[o] = 0;
            return o;
        }
    }
    if (size) out[0] = 0;
    return -1;
}

void mime_type(const char *value, char *out, long size)
{
    long o = 0;
    while (space(*value)) value++;
    for (; *value && *value != ';' && !space(*value); value++) if (o < size - 1) out[o++] = lower(*value);
    out[o] = 0;
}

short mime_charset(const char *name)
{
    if (same_ci(name, "utf-8", 6)) return kMimeUTF8;
    if (same_ci(name, "iso-8859-1", 11) || same_ci(name, "us-ascii", 9) || same_ci(name, "latin1", 7)) return kMimeLatin1;
    return kMimeGuess;
}

static short hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

long mime_b64(MimeB64 *s, const char *in, long len, unsigned char *out)
{
    long o = 0;
    for (; len > 0 && !s->done; in++, len--) {
        const char *k;
        if (*in == '=') { s->done = 1; break; }
        if (!*in || !(k = strchr(B64, *in))) continue;
        s->bits = (s->bits << 6) | (unsigned long)(k - B64);
        if (++s->n == 4) {
            out[o++] = (unsigned char)(s->bits >> 16);
            out[o++] = (unsigned char)(s->bits >> 8);
            out[o++] = (unsigned char)s->bits;
            s->n = 0;
            s->bits = 0;
        }
    }
    if (s->done && s->n) {      /* the bytes the last, short group holds */
        if (s->n >= 2) out[o++] = (unsigned char)(s->bits >> (s->n == 2 ? 4 : 10));
        if (s->n == 3) out[o++] = (unsigned char)(s->bits >> 2);
        s->n = 0;
    }
    return o;
}

long mime_qp(char *line, long len, short *soft)
{
    long i, o = 0;
    while (len && (line[len - 1] == ' ' || line[len - 1] == '\t')) len--;   /* 6.7 (3) */
    *soft = 0;
    for (i = 0; i < len; i++) {
        if (line[i] == '=') {
            if (i == len - 1) { *soft = 1; break; }     /* 6.7 (5) */
            if (i + 2 < len && hexval(line[i + 1]) >= 0 && hexval(line[i + 2]) >= 0) {
                line[o++] = (char)(hexval(line[i + 1]) * 16 + hexval(line[i + 2]));
                i += 2;
                continue;
            }
        }
        line[o++] = line[i];
    }
    return o;
}

/* One encoded word at p ("=?charset?Q?text?="): RFC 1522 2. Its length, with the bytes
   written at *o, or 0 if it isn't one. */
static long word(const char *p, char *out, long size, long *o, short *cs)
{
    const char *q1, *q2, *end;
    char set[24];
    long n;
    if (p[0] != '=' || p[1] != '?') return 0;
    if (!(q1 = strchr(p + 2, '?')) || !q1[1] || q1[2] != '?') return 0;
    if (!(end = strstr(q1 + 3, "?="))) return 0;
    for (q2 = p + 2; q2 < end; q2++) if (*q2 == ' ') return 0;
    n = q1 - (p + 2);
    if (n >= (long)sizeof set) return 0;
    memcpy(set, p + 2, n);
    set[n] = 0;
    *cs = mime_charset(set);
    q2 = q1 + 3;
    if (lower(q1[1]) == 'q') {          /* 4.2: "_" is a space, =XX a byte */
        for (; q2 < end; q2++) {
            char c = *q2;
            if (c == '_') c = ' ';
            else if (c == '=' && q2 + 2 < end && hexval(q2[1]) >= 0 && hexval(q2[2]) >= 0) {
                c = (char)(hexval(q2[1]) * 16 + hexval(q2[2]));
                q2 += 2;
            }
            if (*o < size - 1) out[(*o)++] = c;
        }
    } else if (lower(q1[1]) == 'b') {   /* 4.1 */
        MimeB64 s;
        unsigned char b[3];
        memset(&s, 0, sizeof s);
        for (; q2 < end; q2++) {
            long k = mime_b64(&s, q2, 1, b), j;
            for (j = 0; j < k; j++) if (*o < size - 1) out[(*o)++] = (char)b[j];
        }
        if (!s.done) { s.done = 1; }
    } else return 0;
    return end + 2 - p;
}

short mime_words(const char *in, char *out, long size)
{
    long o = 0, k, after = -1;     /* after: where the last word's output ended */
    short cs = kMimeGuess, c;
    const char *p = in;
    while (*p) {
        if ((k = word(p, out, size, &o, &c)) > 0) {
            cs = c;
            p += k;
            after = o;
            continue;
        }
        if (space(*p) && after >= 0) {
            /* white space between two words goes; before other text it stays */
            const char *q = p;
            long save = o;
            char scratch[2];
            long so = 0;
            short sc;
            while (space(*q)) q++;
            if (word(q, scratch, 1, &so, &sc) > 0) { p = q; o = save; continue; }
        }
        after = -1;
        if (o < size - 1) out[o++] = *p;
        p++;
    }
    out[o] = 0;
    return cs;
}

/* RFC 822 6.1: mailbox = addr-spec / phrase route-addr */
void mime_who(const char *in, char *out, long size)
{
    const char *lt = strchr(in, '<'), *lp = strchr(in, '('), *a, *b;
    long o = 0;
    while (space(*in)) in++;
    if (lt && lt > in) { a = in; b = lt; }
    else if (lp && strchr(lp, ')')) { a = lp + 1; b = strchr(lp, ')'); }
    else if (lt) { a = lt + 1; b = strchr(lt, '>'); if (!b) b = a + strlen(a); }
    else { a = in; b = in + strlen(in); }
    while (a < b && (space(*a) || *a == '"')) a++;
    while (b > a && (space(b[-1]) || b[-1] == '"')) b--;
    for (; a < b; a++) {
        if (*a == '\\' && a + 1 < b) a++;
        if (o < size - 1) out[o++] = *a;
    }
    out[o] = 0;
    if (!o) {       /* "<>" or nothing: the whole thing */
        long n = strlen(in);
        if (n >= size) n = size - 1;
        memcpy(out, in, n);
        out[n] = 0;
    }
}

/* RFC 822 5.1: date-time = [ day "," ] date time; date = 1*2DIGIT month 2*4DIGIT */
void mime_day(const char *in, char *out, long size)
{
    const char *p = in, *c = strchr(in, ',');
    long o = 0;
    short words = 0;
    if (c && c - in < 10) p = c + 1;
    while (space(*p)) p++;
    if (*p >= '0' && *p <= '9') {
        const char *s = p;
        for (; *p && words < 3; p++) {
            if (space(*p)) {
                words++;
                while (space(p[1])) p++;
            }
        }
        if (words >= 2) {
            long n = p - s;
            while (n && space(s[n - 1])) n--;
            if (n >= size) n = size - 1;
            memcpy(out, s, n);
            out[n] = 0;
            return;
        }
    }
    for (; in[o] && o < size - 1; o++) out[o] = in[o];
    out[o] = 0;
}
