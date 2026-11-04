#include <stdlib.h>
#include <string.h>

#include "cookie.h"

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static char *copy_lower(const char *s, long n, int lowercase)
{
    char *c = malloc(n + 1);
    long i;
    if (!c) return NULL;
    for (i = 0; i < n; i++) c[i] = lowercase ? (char)lower((unsigned char)s[i]) : s[i];
    c[n] = 0;
    return c;
}

static int eq_ci(const char *a, long n, const char *b)
{
    long i;
    if ((long)strlen(b) != n) return 0;
    for (i = 0; i < n; i++) if (lower((unsigned char)a[i]) != b[i]) return 0;
    return 1;
}

static void trim(const char **s, long *n)
{
    while (*n > 0 && (**s == ' ' || **s == '\t')) { (*s)++; (*n)--; }
    while (*n > 0 && ((*s)[*n - 1] == ' ' || (*s)[*n - 1] == '\t')) (*n)--;
}

static int is_ip(const char *host)
{
    for (; *host; host++) if ((*host < '0' || *host > '9') && *host != '.') return 0;
    return 1;
}

/* host is domain, or ends in ".domain" (and isn't an IP address) */
static int domain_match(const char *host, const char *domain)
{
    long h = strlen(host), d = strlen(domain), i;
    if (h == d) {
        for (i = 0; i < h; i++) if (lower((unsigned char)host[i]) != domain[i]) return 0;
        return 1;
    }
    if (h < d + 1 || is_ip(host) || host[h - d - 1] != '.') return 0;
    for (i = 0; i < d; i++) if (lower((unsigned char)host[h - d + i]) != domain[i]) return 0;
    return 1;
}

/* The request's path without its query */
static long path_len(const char *path)
{
    return (long)strcspn(path, "?#");
}

static int path_match(const char *req, const char *cpath)
{
    long r = path_len(req), c = strlen(cpath);
    if (r < c || memcmp(req, cpath, c)) return 0;
    return r == c || cpath[c - 1] == '/' || req[c] == '/';
}

static void drop(Jar *j, int k)
{
    Cookie *c = &j->c[k];
    j->bytes -= (long)(strlen(c->name) + strlen(c->value) + strlen(c->domain) + strlen(c->path));
    free(c->name); free(c->value); free(c->domain); free(c->path);
    memmove(c, c + 1, (j->n - k - 1) * sizeof(Cookie));  /* oldest first stays oldest first */
    j->n--;
}

static const char *const MONTHS = "janfebmaraprmayjunjulaugsepoctnovdec";

unsigned long cookie_date(const char *s, long len)
{
    long i = 0, day = -1, month = -1, year = -1, hh = -1, mm = 0, ss = 0;
    unsigned long days;
    long y, m;
    /* tokens, in any order: a day of the month, a month's name, a year, and hh:mm:ss */
    while (i < len) {
        long st, n;
        while (i < len && !((s[i] >= '0' && s[i] <= '9') || ((s[i] | 32) >= 'a' && (s[i] | 32) <= 'z') || s[i] == ':')) i++;
        st = i;
        while (i < len && ((s[i] >= '0' && s[i] <= '9') || ((s[i] | 32) >= 'a' && (s[i] | 32) <= 'z') || s[i] == ':')) i++;
        n = i - st;
        if (!n) break;
        if (n >= 5 && s[st + 2] == ':' && hh < 0) {
            hh = atol(s + st);
            mm = atol(s + st + 3);
            if (n >= 8 && s[st + 5] == ':') ss = atol(s + st + 6);
        } else if (s[st] >= '0' && s[st] <= '9') {
            long v = atol(s + st);
            if (n <= 2 && day < 0) day = v;
            else if (year < 0) year = n <= 2 ? (v < 70 ? 2000 + v : 1900 + v) : v;
        } else if (n >= 3 && month < 0) {
            int k;
            for (k = 0; k < 12; k++)
                if (lower((unsigned char)s[st]) == MONTHS[3 * k] && lower((unsigned char)s[st + 1]) == MONTHS[3 * k + 1] &&
                    lower((unsigned char)s[st + 2]) == MONTHS[3 * k + 2]) month = k + 1;
        }
    }
    if (day < 1 || day > 31 || month < 1 || year < 1904 || year > 2039 || hh < 0 || hh > 23 || mm > 59 || ss > 59) return 0;
    /* days from 1904-01-01 to the date */
    y = month <= 2 ? year - 1 : year;
    m = month <= 2 ? month + 9 : month - 3;
    days = (unsigned long)(365L * y + y / 4 - y / 100 + y / 400 + (153 * m + 2) / 5 + day - 1) - 695361UL;
    return days * 86400UL + (unsigned long)(hh * 3600L + mm * 60L + ss);
}

void jar_set(Jar *j, const char *host, const char *path, int https, const char *line, long len, unsigned long now)
{
    const char *name, *value, *p = line, *end = line + len;
    long nlen, vlen, plen;
    char *domain = NULL, *cpath = NULL;
    int host_only = 1, secure = 0, k;
    long max_age = 0;
    int have_max_age = 0;
    unsigned long expires = 0;
    Cookie *c;

    /* name=value, then attributes after semicolons */
    name = p;
    while (p < end && *p != ';') p++;
    {
        const char *eq = memchr(name, '=', p - name);
        if (!eq) return;
        nlen = eq - name;
        value = eq + 1;
        vlen = p - value;
    }
    trim(&name, &nlen);
    trim(&value, &vlen);
    if (!nlen || nlen + vlen > 4096) return;
    while (p < end) {
        const char *a = ++p, *v = NULL;
        long alen, avlen = 0;
        while (p < end && *p != ';') p++;
        alen = p - a;
        {
            const char *eq = memchr(a, '=', alen);
            if (eq) { v = eq + 1; avlen = a + alen - v; alen = eq - a; }
        }
        trim(&a, &alen);
        if (v) trim(&v, &avlen);
        if (eq_ci(a, alen, "secure")) secure = 1;
        else if (eq_ci(a, alen, "max-age") && v && avlen) { max_age = atol(v); have_max_age = 1; }
        else if (eq_ci(a, alen, "expires") && v) expires = cookie_date(v, avlen);
        else if (eq_ci(a, alen, "domain") && v && avlen) {
            if (*v == '.') { v++; avlen--; }
            free(domain);
            domain = avlen ? copy_lower(v, avlen, 1) : NULL;
        } else if (eq_ci(a, alen, "path") && v && avlen && *v == '/') {
            free(cpath);
            cpath = copy_lower(v, avlen, 0);
        }
    }
    if (secure && !https) goto out;  /* for https only */
    if (domain) {
        /* the host's own domain or a parent with a dot in it (not "com") */
        if (!strchr(domain, '.') || !domain_match(host, domain)) goto out;
        host_only = 0;
    } else if (!(domain = copy_lower(host, strlen(host), 1))) {
        goto out;
    }
    if (!cpath) {
        /* the request's folder */
        plen = path_len(path);
        while (plen > 1 && path[plen - 1] != '/') plen--;
        if (plen > 1) plen--;
        if (plen < 1 || path[0] != '/') { path = "/"; plen = 1; }
        if (!(cpath = copy_lower(path, plen, 0))) goto out;
    }
    if (have_max_age) expires = max_age > 0 ? now + (unsigned long)max_age : 1;
    /* the same name, domain, and path replaces it, or an expired one deletes it */
    for (k = 0; k < j->n; k++)
        if (eq_ci(j->c[k].domain, strlen(j->c[k].domain), domain) && !strcmp(j->c[k].path, cpath) &&
            (long)strlen(j->c[k].name) == nlen && !memcmp(j->c[k].name, name, nlen)) { drop(j, k); break; }
    if (expires && expires <= now) goto out;
    while (j->n && (j->n >= JAR_MAX_COOKIES || j->bytes + nlen + vlen + (long)(strlen(domain) + strlen(cpath)) > JAR_MAX_BYTES))
        drop(j, 0);  /* make room: the oldest go first */
    if (!(c = realloc(j->c, (j->n + 1) * sizeof(Cookie)))) goto out;
    j->c = c;
    c = &j->c[j->n];
    if (!(c->name = copy_lower(name, nlen, 0)) || !(c->value = copy_lower(value, vlen, 0))) {
        free(c->name);
        goto out;
    }
    c->domain = domain;
    c->path = cpath;
    c->expires = expires;
    c->host_only = (char)host_only;
    c->secure = (char)secure;
    j->bytes += nlen + vlen + (long)(strlen(domain) + strlen(cpath));
    j->n++;
    return;
out:
    free(domain);
    free(cpath);
}

int jar_header(Jar *j, const char *host, const char *path, int https, unsigned long now, char *out, int size)
{
    int k = 0, o = 0;
    char lhost[128];
    long i, hl = strlen(host);
    if (hl >= (long)sizeof lhost) hl = sizeof lhost - 1;
    for (i = 0; i < hl; i++) lhost[i] = (char)lower((unsigned char)host[i]);
    lhost[hl] = 0;
    out[0] = 0;
    while (k < j->n) {
        Cookie *c = &j->c[k];
        long n;
        if (c->expires && c->expires <= now) { drop(j, k); continue; }
        k++;
        if (c->host_only ? strcmp(lhost, c->domain) : !domain_match(lhost, c->domain)) continue;
        if (!path_match(path, c->path) || (c->secure && !https)) continue;
        n = (long)(strlen(c->name) + strlen(c->value) + 1 + (o ? 2 : 0));
        if (o + n >= size) continue;
        if (o) { out[o++] = ';'; out[o++] = ' '; }
        strcpy(out + o, c->name);
        o += (int)strlen(c->name);
        out[o++] = '=';
        strcpy(out + o, c->value);
        o += (int)strlen(c->value);
    }
    return o;
}

void jar_free(Jar *j)
{
    while (j->n) drop(j, 0);
    free(j->c);
    j->c = NULL;
    j->bytes = 0;
}
