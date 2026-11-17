/* HTTP/1.1 replies (RFC 2068), for the http Plug-In: see http11.h. Section numbers are
   RFC 2068's; where it's vague, RFC 2616's are noted. */
#include <string.h>

#include "http11.h"

#define SIZE_LINE 1024      /* a chunk's size line, with its extensions */
#define TRAILERS 4096       /* the trailers, all of them */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int same_ci(const char *a, const char *b, long n)
{
    for (; n > 0; n--, a++, b++) if (lower(*a) != lower(*b)) return 0;
    return 1;
}

static int is_space(int c) { return c == ' ' || c == '\t'; }

long http_head_end(const char *buf, long len)
{
    long i;
    /* an empty line: LF, then CRLF or LF */
    for (i = 0; i < len; i++) {
        if (buf[i] != '\n') continue;
        if (i + 1 < len && buf[i + 1] == '\n') return i + 2;
        if (i + 2 < len && buf[i + 1] == '\r' && buf[i + 2] == '\n') return i + 3;
    }
    return 0;
}

/* A whole number of decimal digits at s..e, with spaces around it: -1 if it isn't one,
   or is 2^31 or more */
static long number(const char *s, const char *e)
{
    long n = 0;
    while (s < e && is_space(*s)) s++;
    while (e > s && is_space(e[-1])) e--;
    if (s == e) return -1;
    for (; s < e; s++) {
        if (*s < '0' || *s > '9' || n > (0x7FFFFFFFL - 9) / 10) return -1;
        n = n * 10 + *s - '0';
    }
    return n;
}

/* Transfer-Encoding's value (14.40): codings separated by commas, each maybe with
   parameters. chunked must be last (3.6); identity means none (2616 3.6). */
static void codings(const char *s, const char *e, HttpHead *h)
{
    while (s < e) {
        const char *t, *te;
        while (s < e && (is_space(*s) || *s == ',')) s++;
        if (s == e) break;
        for (t = s; s < e && *s != ',' && *s != ';'; s++) {}
        for (te = s; te > t && is_space(te[-1]); te--) {}
        while (s < e && *s != ',') s++;     /* its parameters */
        if (te - t == 7 && same_ci(t, "chunked", 7)) {
            if (h->chunked) h->bad = 1;     /* twice */
            h->chunked = 1;
        } else if (!(te - t == 8 && same_ci(t, "identity", 8))) {
            h->bad = 1;                     /* one Mill didn't ask for and can't undo */
        }
    }
}

/* Connection's value (14.10): tokens separated by commas; "close" says the server
   closes after this reply (8.1.2.1) */
static void connection(const char *s, const char *e, HttpHead *h)
{
    while (s < e) {
        const char *t, *te;
        while (s < e && (is_space(*s) || *s == ',')) s++;
        for (t = s; s < e && *s != ','; s++) {}
        for (te = s; te > t && is_space(te[-1]); te--) {}
        if (te - t == 5 && same_ci(t, "close", 5)) h->closes = 1;
    }
}

/* Content-Range's value (14.17): "bytes first-last/total", total maybe '*'; or a 416's,
   with a star for the part and just the total (2616 14.16) */
static void content_range(const char *s, const char *e, HttpHead *h)
{
    const char *dash, *slash;
    h->range_bad = 1;
    while (s < e && is_space(*s)) s++;
    if (e - s < 6 || !same_ci(s, "bytes", 5) || !is_space(s[5])) return;
    for (s += 6; s < e && is_space(*s); s++) {}
    for (slash = s; slash < e && *slash != '/'; slash++) {}
    for (dash = s; dash < slash && *dash != '-'; dash++) {}
    if (slash == e) return;
    h->total = number(slash + 1, e);
    if (h->total < 0) {
        while (slash + 1 < e && is_space(slash[1])) slash++;
        while (e > slash + 1 && is_space(e[-1])) e--;
        if (e - slash != 2 || slash[1] != '*') return;
    }
    if (dash == slash) {
        h->range_bad = !(slash - s == 1 && *s == '*' && h->total >= 0);
        return;
    }
    h->first = number(s, dash);
    h->last = number(dash + 1, slash);
    if (h->first < 0 || h->last < h->first || (h->total >= 0 && h->last >= h->total)) h->first = -1;
    else h->range_bad = 0;
}

void http_head(const char *head, long len, HttpHead *h)
{
    long i, line = 0;
    const char *p = head + 5;
    memset(h, 0, sizeof *h);
    h->length = h->first = h->total = -1;
    /* the status line (6.1): HTTP/1.1 200 OK */
    h->closes = 1;
    if (len > 5 && !memcmp(head, "HTTP/", 5)) {
        long major = 0, minor = 0;
        for (; p < head + len && *p >= '0' && *p <= '9'; p++) if (major < 100) major = major * 10 + *p - '0';
        if (p > head + 5 && p < head + len && *p == '.') {
            const char *q = ++p;
            for (; p < head + len && *p >= '0' && *p <= '9'; p++) if (minor < 100) minor = minor * 10 + *p - '0';
            /* 3.1: 1.1 and later stay open unless they say otherwise (8.1.2.1) */
            h->closes = major < 1 || (major == 1 && minor < 1);
            if (p > q && p + 4 <= head + len && *p == ' ' && p[1] >= '1' && p[1] <= '5' &&
                p[2] >= '0' && p[2] <= '9' && p[3] >= '0' && p[3] <= '9' && (p + 4 == head + len || !(p[4] >= '0' && p[4] <= '9')))
                h->code = (p[1] - '0') * 100 + (p[2] - '0') * 10 + p[3] - '0';
        }
    }
    for (i = 0; i < len; i++) {
        long e;
        if (head[i] != '\n') continue;
        e = i > line && head[i - 1] == '\r' ? i - 1 : i;
        if (line > 0 && e > line) {
            const char *s = head + line, *end = head + e;
            long n = e - line;
            if (n >= 15 && same_ci(s, "Content-Length:", 15)) {
                long v = number(s + 15, end);
                if (v < 0 || (h->length >= 0 && v != h->length)) h->bad = 1;
                else h->length = v;
            } else if (n >= 18 && same_ci(s, "Transfer-Encoding:", 18)) {
                codings(s + 18, end, h);
            } else if (n >= 11 && same_ci(s, "Connection:", 11)) {
                connection(s + 11, end, h);
            } else if (n >= 14 && same_ci(s, "Content-Range:", 14)) {
                if (h->first >= 0 || h->range_bad) h->range_bad = 1;   /* twice */
                else content_range(s + 14, end, h);
            }
        }
        line = i + 1;
    }
}

int http_body(const HttpHead *h, int head_request)
{
    /* 4.4: no body after 1xx, 204, 304, or a reply to HEAD */
    if (head_request || (h->code >= 100 && h->code < 200) || h->code == 204 || h->code == 304) return HTTP_NONE;
    if (h->bad || !h->code) return HTTP_BAD;
    /* chunked over Content-Length, which is then ignored (4.4, 2616 4.4) */
    if (h->chunked) return HTTP_CHUNKED;
    if (h->length >= 0) return HTTP_LENGTH;
    return HTTP_CLOSE;
}

long http_drop(char *buf, long head, long len, const char *name)
{
    long i, line = 0, gone = 0, n = strlen(name);
    for (i = 0; i < head; i++) {
        if (buf[i] != '\n') continue;
        if (line > 0 && i + 1 - line > n && same_ci(buf + line, name, n)) {
            long cut = i + 1 - line;
            memmove(buf + line, buf + i + 1, len - i - 1);
            head -= cut;
            len -= cut;
            gone += cut;
            i = line - 1;
            continue;
        }
        line = i + 1;
    }
    return gone;
}

int http_range_reply(const HttpHead *h, long first, long last)
{
    if (h->code == 206) {
        /* 10.2.7: just the part asked for, said in Content-Range. A last past the end
           is taken as the end (14.36.1), so fewer bytes come only if the file ends. */
        if (first < 0 || h->first != first || h->range_bad) return RANGE_BAD;
        if (last >= 0 && h->last != last && !(h->last < last && h->total == h->last + 1)) return RANGE_BAD;
        if (last < 0 && h->total != h->last + 1) return RANGE_BAD;
        if (h->length >= 0 && !h->chunked && h->length != h->last - h->first + 1) return RANGE_BAD;
        return RANGE_PART;
    }
    /* 14.36.2: a server may ignore Range, and send the whole thing */
    if (h->code == 200 && first >= 0) return RANGE_WHOLE;
    return RANGE_PASS;
}

enum { CK_SIZE, CK_SIZE_SP, CK_EXT, CK_SIZE_LF, CK_DATA, CK_DATA_CR, CK_DATA_LF,
       CK_TRAILER, CK_TRAILER_LINE, CK_END_LF, CK_DONE, CK_BAD };

static int hex(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* 3.6: Chunked-Body = *chunk last-chunk footer CRLF, where chunk = chunk-size
   [chunk-ext] CRLF chunk-data CRLF, last-chunk = 1*("0") [chunk-ext] CRLF, and the
   footer is entity headers. A bare LF ends a line too (19.3). */
long chunked_run(Chunked *c, char *buf, long len, long *used)
{
    long i = 0, out = 0;
    while (i < len && c->state != CK_DONE && c->state != CK_BAD) {
        int ch;
        if (c->state == CK_DATA) {
            long n = len - i < c->left ? len - i : c->left;
            memmove(buf + out, buf + i, n);
            out += n;
            i += n;
            if ((c->left -= n) == 0) c->state = CK_DATA_CR;
            continue;
        }
        ch = buf[i++];
        switch (c->state) {
        case CK_SIZE:
            if (hex(ch) >= 0) {
                if (c->left > 0x7FFFFFFL) c->state = CK_BAD;
                else c->left = c->left * 16 + hex(ch);
                break;
            }
            /* fall through */
        case CK_SIZE_SP:
            if (c->line == 0) c->state = CK_BAD;  /* no digits */
            else if (is_space(ch)) c->state = CK_SIZE_SP;
            else if (ch == ';') c->state = CK_EXT;
            else if (ch == '\r') c->state = CK_SIZE_LF;
            else if (ch == '\n') goto sized;
            else c->state = CK_BAD;
            break;
        case CK_EXT:
            /* chunk-ext: skipped */
            if (ch == '\r') c->state = CK_SIZE_LF;
            else if (ch == '\n') goto sized;
            break;
        case CK_SIZE_LF:
            if (ch != '\n') { c->state = CK_BAD; break; }
        sized:
            c->line = 0;
            c->state = c->left ? CK_DATA : CK_TRAILER;
            continue;
        case CK_DATA_CR:
            if (ch == '\r') c->state = CK_DATA_LF;
            else if (ch == '\n') c->state = CK_SIZE;
            else c->state = CK_BAD;
            continue;
        case CK_DATA_LF:
            c->state = ch == '\n' ? CK_SIZE : CK_BAD;
            continue;
        case CK_TRAILER:
            /* a line of the footer, or the empty line that ends it all */
            if (ch == '\r') c->state = CK_END_LF;
            else if (ch == '\n') c->state = CK_DONE;
            else c->state = CK_TRAILER_LINE;
            break;
        case CK_TRAILER_LINE:
            if (ch == '\n') c->state = CK_TRAILER;
            break;
        case CK_END_LF:
            c->state = ch == '\n' ? CK_DONE : CK_BAD;
            break;
        }
        /* the size line's bytes, or the footer's, so far: bounded */
        if (++c->line > (c->state >= CK_TRAILER ? TRAILERS : SIZE_LINE) && c->state != CK_DONE) c->state = CK_BAD;
    }
    *used = i;
    return c->state == CK_BAD ? -1 : out;
}

int chunked_done(const Chunked *c) { return c->state == CK_DONE; }
