/* Parts of files: see range.h. Section numbers are RFC 2068's. */
#include <stdio.h>
#include <string.h>

#include "range.h"

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int same_ci(const char *a, const char *b, long n)
{
    for (; n > 0; n--, a++, b++) if (lower(*a) != lower(*b)) return 0;
    return 1;
}

static int is_space(int c) { return c == ' ' || c == '\t'; }

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

/* The value of the header name (with its colon) in text, len bytes: 1 with *s and *e
   around it, 0 if there's none. The first line (request or status) is skipped. */
static int header(const char *text, long len, const char *name, const char **s, const char **e)
{
    long i, line = 0, n = strlen(name);
    for (i = 0; i < len; i++) {
        if (text[i] != '\n') continue;
        if (line > 0) {
            const char *a = text + line, *b = text + (i > line && text[i - 1] == '\r' ? i - 1 : i);
            if (b - a >= n && same_ci(a, name, n)) {
                *s = a + n;
                *e = b;
                return 1;
            }
        }
        line = i + 1;
    }
    return 0;
}

int range_asked(const char *text, long len, long *first, long *last)
{
    const char *s, *e, *dash;
    if (!header(text, len, "Range:", &s, &e)) return 0;
    /* 14.36.1: bytes=first-last, or bytes=first- */
    while (s < e && is_space(*s)) s++;
    if (e - s < 6 || !same_ci(s, "bytes=", 6)) return -1;
    for (dash = s += 6; dash < e && *dash != '-'; dash++) {}
    if (dash == e || (*first = number(s, dash)) < 0) return -1;
    *last = -1;
    while (e > dash + 1 && is_space(e[-1])) e--;
    if (e > dash + 1 && ((*last = number(dash + 1, e)) < *first)) return -1;
    return 1;
}

int range_part(const char *text, long len, long size, long *first, long *last)
{
    long f, l;
    if (size >= 0 && range_asked(text, len, &f, &l) == 1 && f < size) {
        *first = f;
        *last = l < 0 || l >= size ? size - 1 : l;
        return 1;
    }
    *first = 0;
    *last = size - 1;
    if (size < 0) *last = -1;
    return 0;
}

long range_head(char *out, long room, const char *type, long size, int part, long first, long last)
{
    char buf[400];
    int n;
    if (part)
        n = snprintf(buf, sizeof buf, "HTTP/1.0 206 Partial Content\r\nContent-Type: %.200s\r\n"
                     "Content-Range: bytes %ld-%ld/%ld\r\nContent-Length: %ld\r\nAccept-Ranges: bytes\r\n\r\n",
                     type, first, last, size, last - first + 1);
    else if (size >= 0)
        n = snprintf(buf, sizeof buf, "HTTP/1.0 200 OK\r\nContent-Type: %.200s\r\nContent-Length: %ld\r\n"
                     "Accept-Ranges: bytes\r\n\r\n", type, size);
    else n = snprintf(buf, sizeof buf, "HTTP/1.0 200 OK\r\nContent-Type: %.200s\r\n\r\n", type);
    if (n < 0 || n >= room) return -1;
    memcpy(out, buf, n + 1);
    return n;
}

int range_seekable(const char *head, long len, long *size)
{
    const char *s, *e, *slash;
    int code = len > 12 && same_ci(head, "HTTP/", 5) && (s = memchr(head, ' ', len)) && s + 4 <= head + len ?
               (int)number(s + 1, s + 4) : 0;
    *size = -1;
    /* 14.5: Accept-Ranges: bytes (a list of units; "none" says it can't) */
    if (!header(head, len, "Accept-Ranges:", &s, &e)) return 0;
    for (;;) {
        const char *t;
        while (s < e && (is_space(*s) || *s == ',')) s++;
        if (s == e) return 0;
        for (t = s; s < e && *s != ',' && !is_space(*s); s++) {}
        if (s - t == 5 && same_ci(t, "bytes", 5)) break;
    }
    if (code == 200) {
        if (header(head, len, "Content-Length:", &s, &e)) *size = number(s, e);
    } else if (code == 206 && header(head, len, "Content-Range:", &s, &e) && (slash = memchr(s, '/', e - s))) {
        *size = number(slash + 1, e);
    }
    return *size >= 0;
}
