/* Cookies, the basic parts of RFC 6265, kept while Mill runs: each goes back to the host
   (or the domain) and path it came from, until it expires or Mill quits. Mill only speaks
   http, so cookies marked Secure, which may only travel over https, aren't kept.
   Times are seconds since 1904, as the Mac counts them. */
#ifndef MILL_COOKIE_H
#define MILL_COOKIE_H

typedef struct {
    char *name, *value, *domain, *path;
    unsigned long expires;          /* 0: when Mill quits */
    char host_only;                 /* for that host alone, not its subdomains */
} Cookie;

typedef struct {
    Cookie *c;
    int n;
    long bytes;
} Jar;

#define JAR_MAX_COOKIES 64
#define JAR_MAX_BYTES (16L * 1024)

/* Take a Set-Cookie header's value (line, len bytes) from a response from host for path */
void jar_set(Jar *j, const char *host, const char *path, const char *line, long len, unsigned long now);
/* The Cookie header's value for a request to host and path: its length in out (0 if
   none); cookies that don't fit are left out */
int jar_header(Jar *j, const char *host, const char *path, unsigned long now, char *out, int size);
void jar_free(Jar *j);

/* An HTTP date ("Sun, 06 Nov 1994 08:49:37 GMT", also with dashes or a two-digit year)
   in seconds since 1904; 0 if it isn't one */
unsigned long cookie_date(const char *s, long len);

#endif
