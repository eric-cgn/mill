#include <stdio.h>
#include <string.h>

#include "url.h"

static int prefix_ci(const char *s, const char *p)
{
    while (*p) { if ((*s | 32) != *p) return 0; s++; p++; }
    return 1;
}

static const char *SCHEMES[] = { "http://", "file://", "mill://", "https://" };
#define NSCHEMES 4

int url_is_web(int scheme) { return scheme == SCHEME_HTTP || scheme == SCHEME_HTTPS; }

static const char *others = "";
void url_schemes(const char *names) { others = names ? names : ""; }

unsigned short url_port(const Url *u)
{
    if (u->port) return u->port;
    return u->scheme == SCHEME_HTTPS ? 443 : u->scheme == SCHEME_HTTP ? 80 : 0;
}

/* The n characters at s, a scheme's name, if it's one of the Plug-Ins': into name, in
   lower case */
static int other_named(const char *s, long n, char *name)
{
    const char *p = others;
    long i;
    if (n < 1 || n > 11) return 0;
    for (i = 0; i < n; i++) name[i] = (char)(s[i] >= 'A' && s[i] <= 'Z' ? s[i] | 32 : s[i]);
    name[n] = 0;
    while (*p) {
        const char *e;
        while (*p == ' ') p++;
        for (e = p; *e && *e != ' '; e++) {}
        if (e - p == n && !memcmp(p, name, n)) return 1;
        p = e;
    }
    return 0;
}

static int same_ci(const char *a, const char *b, long n)
{
    while (n-- > 0) { if ((*a | 32) != (*b | 32)) return 0; a++; b++; }
    return 1;
}

/* n characters of s as a dotted IPv4 address into *a; 0 if they aren't one */
static int ipv4(const char *s, long n, unsigned long *a)
{
    unsigned long v = 0, part = 0;
    int parts = 0, digits = 0;
    long i;
    for (i = 0; i <= n; i++) {
        if (i == n || s[i] == '.') {
            if (!digits || part > 255) return 0;
            v = (v << 8 | part) & 0xFFFFFFFFUL;
            parts++;
            part = 0;
            digits = 0;
        } else if (s[i] >= '0' && s[i] <= '9' && digits < 3) {
            part = part * 10 + (s[i] - '0');
            digits++;
        } else {
            return 0;
        }
    }
    if (parts != 4) return 0;
    *a = v;
    return 1;
}

int url_host_in(const char *host, const char *list)
{
    long hn = strlen(host);
    unsigned long ip, net;
    int host_ip = ipv4(host, hn, &ip);
    while (*list) {
        const char *d, *slash;
        long n;
        while (*list == ' ' || *list == ',') list++;
        for (d = list; *list && *list != ' ' && *list != ','; list++) {}
        n = list - d;
        slash = memchr(d, '/', n);
        if (ipv4(d, slash ? slash - d : n, &net)) {
            /* an address, or a range of them: only ever an address's match */
            long bits = 32;
            unsigned long mask;
            if (slash) {
                const char *b = slash + 1;
                if (b == d + n || d + n - b > 2) continue;
                for (bits = 0; b < d + n; b++) {
                    if (*b < '0' || *b > '9') break;
                    bits = bits * 10 + (*b - '0');
                }
                if (b < d + n || bits > 32) continue;
            }
            mask = bits ? (0xFFFFFFFFUL << (32 - bits)) & 0xFFFFFFFFUL : 0;
            if (host_ip && (ip & mask) == (net & mask)) return 1;
            continue;
        }
        if (host_ip) continue;  /* a name never matches an address */
        if (n && *d == '.') { d++; n--; }
        if (!n) continue;
        if (hn == n && same_ci(host, d, n)) return 1;
        if (hn > n && host[hn - n - 1] == '.' && same_ci(host + hn - n, d, n)) return 1;
    }
    return 0;
}

static int hex(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
}

/* n bytes of s, with %XX decoded, into out */
static void decode(const char *s, long n, char *out, int size)
{
    int o = 0;
    long i;
    for (i = 0; i < n && o < size - 1; i++) {
        if (s[i] == '%' && i + 2 < n && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
            out[o++] = (char)(hex(s[i + 1]) * 16 + hex(s[i + 2]));
            i += 2;
        } else out[o++] = s[i];
    }
    out[o] = 0;
}

int url_has_scheme(const char *s)
{
    const char *p = s;
    if (!((*p | 32) >= 'a' && (*p | 32) <= 'z')) return 0;
    while (((*p | 32) >= 'a' && (*p | 32) <= 'z') || (*p >= '0' && *p <= '9') || *p == '+' || *p == '-' || *p == '.') p++;
    return p[0] == ':' && p[1] == '/' && p[2] == '/' ? (int)(p + 3 - s) : 0;
}

/* Where an address's host part starts and ends (after "scheme://", before the path) */
static const char *authority(const char *url, const char **end)
{
    const char *s = url + url_has_scheme(url);
    for (*end = s; **end && **end != '/' && **end != '?' && **end != '#'; (*end)++) {}
    return s;
}

int url_parse(const char *s, Url *u)
{
    const char *p, *end;
    long n;
    int k;
    memset(u, 0, sizeof *u);
    while (*s == ' ') s++;
    for (k = 0; k < NSCHEMES && !prefix_ci(s, SCHEMES[k]); k++) {}
    if (k < NSCHEMES) { u->scheme = (char)k; s += strlen(SCHEMES[k]); }
    else if ((k = url_has_scheme(s)) != 0) {
        if (!other_named(s, k - 3, u->name)) return -1;  /* a scheme no Plug-In has */
        u->scheme = SCHEME_OTHER;
        s += k;
    }
    for (end = s; *end && *end != '/' && *end != '?' && *end != '#'; end++) {}
    /* user:password@ before the host (the last @, as a password may hold one) */
    for (p = end; p > s && p[-1] != '@'; p--) {}
    if (p > s) {
        const char *colon = memchr(s, ':', p - 1 - s);
        if (colon) {
            decode(s, colon - s, u->user, sizeof u->user);
            decode(colon + 1, p - 1 - colon - 1, u->pass, sizeof u->pass);
        } else decode(s, p - 1 - s, u->user, sizeof u->user);
        s = p;
    }
    n = end - s;
    if (n >= (long)sizeof u->host || (n == 0 && u->scheme != SCHEME_FILE)) return -1;
    memcpy(u->host, s, n);
    if (u->scheme == SCHEME_MILL) for (k = 0; u->host[k]; k++) u->host[k] |= 32;  /* mill://About is mill://about */
    if ((url_is_web(u->scheme) || u->scheme == SCHEME_OTHER) && (p = strchr(u->host, ':'))) {
        /* 1 to 65535; nothing after the colon is no port */
        long port = 0;
        char *colon = (char *)p;
        for (p++; *p; p++) {
            if (*p < '0' || *p > '9' || (port = port * 10 + (*p - '0')) > 65535) return -1;
        }
        if (p > colon + 1 && !port) return -1;
        u->port = (unsigned short)port;
        *colon = 0;
        if (!u->host[0]) return -1;
    }
    p = end;
    if (*p != '/') { strcpy(u->path, "/"); }
    {
        const char *hash = strchr(p, '#');
        long plen = hash ? hash - p : (long)strlen(p);
        long at = (*p != '/') ? 1 : 0;
        if (plen + at >= (long)sizeof u->path) plen = sizeof u->path - 1 - at;
        memcpy(u->path + at, p, plen);
        u->path[at + plen] = 0;
        if (hash) { strncpy(u->fragment, hash + 1, sizeof u->fragment - 1); }
    }
    return 0;
}

void url_format(const Url *u, char *out, int out_size)
{
    if (u->scheme == SCHEME_FILE) snprintf(out, out_size, "file://%s", u->path);
    else if (u->scheme == SCHEME_MILL) snprintf(out, out_size, "mill://%s%s", u->host, strcmp(u->path, "/") ? u->path : "");
    else if (u->scheme == SCHEME_OTHER) {
        if (u->port) snprintf(out, out_size, "%s://%s:%u%s", u->name, u->host, u->port, u->path);
        else snprintf(out, out_size, "%s://%s%s", u->name, u->host, u->path);
    }
    else if (!u->port || u->port == (u->scheme == SCHEME_HTTPS ? 443 : 80)) snprintf(out, out_size, "%s%s%s", SCHEMES[(int)u->scheme], u->host, u->path);
    else snprintf(out, out_size, "%s%s:%u%s", SCHEMES[(int)u->scheme], u->host, u->port, u->path);
    if (u->user[0]) {
        char with[300];
        if (!url_with_login(out, u->user, u->pass, with, sizeof with)) snprintf(out, out_size, "%s", with);
    }
}

void url_strip_login(char *url)
{
    const char *end, *s = authority(url, &end), *at;
    for (at = end; at > s && at[-1] != '@'; at--) {}
    if (at > s) memmove((char *)s, at, strlen(at) + 1);
}

/* s %-encoded so it can't end the user or password early */
static int encode(const char *s, char *out, int size)
{
    static const char digits[] = "0123456789ABCDEF";
    int o = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c <= ' ' || c >= 127 || strchr(":@/?#%[]\\", c)) {
            if (o + 3 >= size) return -1;
            out[o++] = '%';
            out[o++] = digits[c >> 4];
            out[o++] = digits[c & 15];
        } else {
            if (o + 1 >= size) return -1;
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
    return o;
}

int url_with_login(const char *url, const char *user, const char *pass, char *out, int out_size)
{
    char bare[300], u[200], p[200];
    const char *end, *s;
    snprintf(bare, sizeof bare, "%s", url);
    url_strip_login(bare);
    if (!user[0]) { snprintf(out, out_size, "%s", bare); return 0; }
    if (encode(user, u, sizeof u) < 0 || encode(pass, p, sizeof p) < 0) return -1;
    s = authority(bare, &end);
    if ((int)(strlen(bare) + strlen(u) + strlen(p) + 3) > out_size) return -1;
    snprintf(out, out_size, "%.*s%s%s%s@%s", (int)(s - bare), bare, u, pass[0] ? ":" : "", p, s);
    return 0;
}

int url_referer(const char *page, const char *target, char *out, int out_size)
{
    Url p, t;
    char *c;
    if (!page || !url_has_scheme(page) || url_parse(page, &p)) return 0;
    memset(p.pass, 0, sizeof p.pass);
    /* an https: page's address doesn't go out over plain http: */
    if (p.scheme == SCHEME_HTTPS && (url_parse(target, &t) || t.scheme != SCHEME_HTTPS)) return 0;
    if ((int)strlen(page) >= out_size) return 0;
    strcpy(out, page);
    url_strip_login(out);
    if ((c = strchr(out, '#'))) *c = 0;
    for (c = out; *c; c++) if ((unsigned char)*c < ' ' || *c == 127) return 0;  /* one header line */
    return 1;
}

/* Remove "." and ".." segments from an absolute path, in place */
static void normalize(char *path)
{
    char out[256];
    long o = 0;
    const char *p = path;
    char *q = strchr(path, '?');
    char query[256] = "";
    if (q) { strncpy(query, q, sizeof query - 1); *q = 0; }
    while (*p) {
        const char *seg = p + 1, *next = strchr(seg, '/');
        long len = next ? next - seg : (long)strlen(seg);
        if (len == 1 && seg[0] == '.') { if (!next) out[o++] = '/'; }
        else if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            /* drop the last segment written */
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
            if (!next) out[o++] = '/';
        } else {
            if (o + len + 1 >= (long)sizeof out) break;
            out[o++] = '/';
            memcpy(out + o, seg, len);
            o += len;
        }
        if (!next) break;
        p = next;
    }
    if (o == 0) out[o++] = '/';
    out[o] = 0;
    {
        long ql = strlen(query);
        if (o + ql > 255) ql = 255 - o;
        memcpy(path, out, o);
        memcpy(path + o, query, ql);
        path[o + ql] = 0;
    }
}

/* A colon before any '/', '?' or '#' means a scheme Mill doesn't know */
static int other_scheme(const char *href)
{
    const char *colon = strchr(href, ':'), *first = strpbrk(href, "/?#");
    return colon && (!first || colon < first);
}

int url_resolve(const char *base, const char *href, char *out, int out_size)
{
    Url b, r;
    while (*href == ' ') href++;
    if (url_has_scheme(href)) {
        if (url_parse(href, &r)) return -1;  /* a scheme Mill doesn't know */
        if (r.scheme != SCHEME_OTHER) normalize(r.path);  /* a Plug-In's paths are its own */
    } else if (other_scheme(href)) {
        return -1;  /* mailto:, ftp:, javascript: ... */
    } else {
        if (url_parse(base, &b)) return -1;
        r = b;
        r.fragment[0] = 0;
        if (href[0] == '#') {
            strncpy(r.fragment, href + 1, sizeof r.fragment - 1);
        } else if (href[0] == '/' && href[1] == '/') {
            char tmp[300];
            if (b.scheme == SCHEME_OTHER) snprintf(tmp, sizeof tmp, "%s:%s", b.name, href);
            else snprintf(tmp, sizeof tmp, "%.*s%s", (int)(strchr(SCHEMES[(int)b.scheme], ':') + 1 - SCHEMES[(int)b.scheme]),
                          SCHEMES[(int)b.scheme], href);
            if (url_parse(tmp, &r)) return -1;
        } else {
            char path[300];
            const char *hash = strchr(href, '#');
            long hlen = hash ? hash - href : (long)strlen(href);
            if (href[0] == '/') {
                snprintf(path, sizeof path, "%.*s", (int)hlen, href);
            } else if (href[0] == '?') {
                char *q = strchr(b.path, '?');
                if (q) *q = 0;
                snprintf(path, sizeof path, "%s%.*s", b.path, (int)hlen, href);
            } else {
                char dir[256];
                char *q = strchr(b.path, '?'), *slash;
                if (q) *q = 0;
                strcpy(dir, b.path);
                slash = strrchr(dir, '/');
                if (slash) slash[1] = 0;
                snprintf(path, sizeof path, "%s%.*s", dir, (int)hlen, href);
            }
            strncpy(r.path, path, sizeof r.path - 1);
            r.path[sizeof r.path - 1] = 0;
            normalize(r.path);
            if (hash) strncpy(r.fragment, hash + 1, sizeof r.fragment - 1);
        }
    }
    url_format(&r, out, out_size);
    if (r.fragment[0] && (int)(strlen(out) + strlen(r.fragment) + 2) < out_size) {
        strcat(out, "#");
        strcat(out, r.fragment);
    }
    return 0;
}
