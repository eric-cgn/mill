/* HTTP Basic and Digest authentication; see auth.h */
#include "auth.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ MD5 (RFC 1321) */

typedef unsigned long u32;      /* at least 32 bits; masked where it matters */

static const unsigned char md5_shift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static const u32 md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

#define ROTL(x, n) ((((x) << (n)) | (((x) & 0xffffffffUL) >> (32 - (n)))) & 0xffffffffUL)

static void md5_block(u32 h[4], const unsigned char *p)
{
    u32 m[16], a = h[0], b = h[1], c = h[2], d = h[3];
    int i;
    for (i = 0; i < 16; i++)
        m[i] = (u32)p[i * 4] | (u32)p[i * 4 + 1] << 8 | (u32)p[i * 4 + 2] << 16 | (u32)p[i * 4 + 3] << 24;
    for (i = 0; i < 64; i++) {
        u32 f, t;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        t = d;
        d = c;
        c = b;
        b = (b + ROTL((a + f + md5_k[i] + m[g]) & 0xffffffffUL, md5_shift[i])) & 0xffffffffUL;
        a = t;
    }
    h[0] = (h[0] + a) & 0xffffffffUL;
    h[1] = (h[1] + b) & 0xffffffffUL;
    h[2] = (h[2] + c) & 0xffffffffUL;
    h[3] = (h[3] + d) & 0xffffffffUL;
}

void auth_md5(const unsigned char *data, unsigned long len, unsigned char digest[16])
{
    u32 h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    unsigned char last[128];
    unsigned long i, rest, bits = len * 8;
    int n, j;
    for (i = 0; i + 64 <= len; i += 64) md5_block(h, data + i);
    rest = len - i;
    memcpy(last, data + i, rest);
    last[rest] = 0x80;
    n = rest < 56 ? 64 : 128;
    memset(last + rest + 1, 0, n - rest - 1);
    for (j = 0; j < 4; j++) last[n - 8 + j] = (unsigned char)(bits >> (8 * j));  /* lengths under 512M */
    md5_block(h, last);
    if (n == 128) md5_block(h, last + 64);
    for (j = 0; j < 16; j++) digest[j] = (unsigned char)(h[j / 4] >> (8 * (j % 4)));
}

/* MD5 of a string, as 32 lowercase hex digits */
static void md5_hex(const char *s, char out[33])
{
    unsigned char d[16];
    int i;
    auth_md5((const unsigned char *)s, strlen(s), d);
    for (i = 0; i < 16; i++) sprintf(out + i * 2, "%02x", d[i]);
}

/* ------------------------------------------------------------ base64 */

int auth_base64(const unsigned char *in, int len, char *out, int size)
{
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i, o = 0;
    if ((len + 2) / 3 * 4 + 1 > size) return 0;
    for (i = 0; i < len; i += 3) {
        unsigned long v = (unsigned long)in[i] << 16 | (i + 1 < len ? (unsigned long)in[i + 1] << 8 : 0) |
                          (i + 2 < len ? in[i + 2] : 0);
        out[o++] = abc[v >> 18 & 63];
        out[o++] = abc[v >> 12 & 63];
        out[o++] = i + 1 < len ? abc[v >> 6 & 63] : '=';
        out[o++] = i + 2 < len ? abc[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

/* ------------------------------------------------------------ challenges */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int same_ci(const char *a, long n, const char *b)
{
    long i;
    if ((long)strlen(b) != n) return 0;
    for (i = 0; i < n; i++) if (lower(a[i]) != lower(b[i])) return 0;
    return 1;
}

static int is_space(char c) { return c == ' ' || c == '\t'; }
static int is_token(char c) { return c > ' ' && c != ',' && c != '=' && c != '"' && c != 127; }

/* A quoted string or a token at v[*i], unquoted into out (cut to fit) */
static void param_value(const char *v, long len, long *i, char *out, int size)
{
    int o = 0;
    if (*i < len && v[*i] == '"') {
        for ((*i)++; *i < len && v[*i] != '"'; (*i)++) {
            if (v[*i] == '\\' && *i + 1 < len) (*i)++;
            if (o < size - 1) out[o++] = v[*i];
        }
        if (*i < len) (*i)++;
    } else {
        for (; *i < len && is_token(v[*i]); (*i)++) if (o < size - 1) out[o++] = v[*i];
    }
    out[o] = 0;
}

/* Does a comma-separated list (qop="auth,auth-int") have word in it? */
static int has_word(const char *list, const char *word)
{
    const char *p = list;
    while (*p) {
        const char *e;
        while (*p == ' ' || *p == ',') p++;
        for (e = p; *e && *e != ',' && *e != ' '; e++) {}
        if (e > p && same_ci(p, e - p, word)) return 1;
        p = e;
    }
    return 0;
}

/* A challenge is a scheme name and then name=value parameters; a token not followed by
   '=' starts the next challenge */
void auth_challenge(const char *v, long len, Challenge *c)
{
    long i = 0;
    while (i < len) {
        Challenge t;
        long s;
        char algorithm[128], qop[128], value[128];
        int ok = 1;
        while (i < len && (is_space(v[i]) || v[i] == ',')) i++;
        for (s = i; i < len && is_token(v[i]); i++) {}
        if (i == s) { i++; continue; }
        memset(&t, 0, sizeof t);
        algorithm[0] = qop[0] = 0;
        t.scheme = same_ci(v + s, i - s, "basic") ? AUTH_BASIC : same_ci(v + s, i - s, "digest") ? AUTH_DIGEST : AUTH_NONE;
        for (;;) {
            long n, nl;
            while (i < len && (is_space(v[i]) || v[i] == ',')) i++;
            for (n = i; i < len && is_token(v[i]); i++) {}
            if (i == n) break;
            nl = i - n;
            while (i < len && is_space(v[i])) i++;
            if (i >= len || v[i] != '=') { i = n; break; }     /* the next challenge */
            for (i++; i < len && is_space(v[i]); i++) {}
            param_value(v, len, &i, value, sizeof value);
            if (same_ci(v + n, nl, "realm")) strcpy(t.realm, value);
            else if (same_ci(v + n, nl, "nonce")) strcpy(t.nonce, value);
            else if (same_ci(v + n, nl, "opaque")) strcpy(t.opaque, value);
            else if (same_ci(v + n, nl, "algorithm")) strcpy(algorithm, value);
            else if (same_ci(v + n, nl, "qop")) strcpy(qop, value);
            else if (same_ci(v + n, nl, "stale")) t.stale = same_ci(value, strlen(value), "true");
        }
        if (t.scheme == AUTH_DIGEST) {
            if (!t.nonce[0]) ok = 0;
            if (algorithm[0]) {
                if (same_ci(algorithm, strlen(algorithm), "md5-sess")) t.sess = 1;
                else if (!same_ci(algorithm, strlen(algorithm), "md5")) ok = 0;   /* SHA-256 and others */
            }
            if (qop[0]) {
                if (has_word(qop, "auth")) t.qop = 1;
                else ok = 0;    /* only auth-int, which needs the body's hash */
            }
        }
        if (ok && t.scheme > c->scheme) *c = t;
    }
}

/* ------------------------------------------------------------ the keyring */

static int same_host(const Cred *c, const char *host, unsigned short port)
{
    return c->port == port && same_ci(host, strlen(host), c->host);
}

static int under(const char *path, const char *dir)
{
    return !strncmp(path, dir, strlen(dir));
}

Cred *auth_for_path(Keyring *k, const char *host, unsigned short port, const char *path)
{
    Cred *best = NULL;
    int i;
    for (i = 0; i < k->n; i++) {
        Cred *c = &k->c[i];
        if (same_host(c, host, port) && under(path, c->dir) && (!best || strlen(c->dir) > strlen(best->dir))) best = c;
    }
    if (best) best->used = ++k->clock;
    return best;
}

Cred *auth_for_realm(Keyring *k, const char *host, unsigned short port, const char *realm)
{
    int i;
    for (i = 0; i < k->n; i++)
        if (same_host(&k->c[i], host, port) && !strcmp(k->c[i].ch.realm, realm)) return &k->c[i];
    return NULL;
}

/* A path's folder: up to and including its last slash, before any query */
static void folder_of(const char *path, char *out, int size)
{
    long q = strcspn(path, "?#"), e = q;
    while (e > 0 && path[e - 1] != '/') e--;
    if (e == 0) { snprintf(out, size, "/"); return; }
    snprintf(out, size, "%.*s", (int)e, path);
}

Cred *auth_keep(Keyring *k, const char *host, unsigned short port, const char *path,
                const Challenge *ch, const char *user, const char *pass)
{
    Cred *c = auth_for_realm(k, host, port, ch->realm);
    char dir[128];
    folder_of(path, dir, sizeof dir);
    if (c) {
        /* widen its folder to the one both are under (both start with a slash) */
        int i = 0;
        while (c->dir[i] && c->dir[i] == dir[i]) i++;
        while (i > 1 && c->dir[i - 1] != '/') i--;
        c->dir[i] = 0;
    } else {
        if (k->n < AUTH_MAX) c = &k->c[k->n++];
        else {
            int i;
            for (c = &k->c[0], i = 1; i < k->n; i++) if (k->c[i].used < c->used) c = &k->c[i];
        }
        memset(c, 0, sizeof *c);
        snprintf(c->host, sizeof c->host, "%s", host);
        c->port = port;
        strcpy(c->dir, dir);
    }
    snprintf(c->user, sizeof c->user, "%s", user);
    snprintf(c->pass, sizeof c->pass, "%s", pass);
    auth_renew(c, ch);
    c->used = ++k->clock;
    return c;
}

void auth_renew(Cred *c, const Challenge *ch)
{
    c->ch = *ch;
    c->nc = 0;
}

void auth_forget(Keyring *k, Cred *c)
{
    int i = (int)(c - k->c);
    memset(c, 0, sizeof *c);
    memmove(&k->c[i], &k->c[i + 1], (k->n - i - 1) * sizeof *c);
    memset(&k->c[--k->n], 0, sizeof *c);
}

void auth_clear(Keyring *k)
{
    memset(k, 0, sizeof *k);
}

/* ------------------------------------------------------------ the header */

/* s with " and \ escaped, for a quoted string */
static void quoted(const char *s, char *out, int size)
{
    int o = 0;
    for (; *s && o < size - 2; s++) {
        if (*s == '"' || *s == '\\') out[o++] = '\\';
        out[o++] = *s;
    }
    out[o] = 0;
}

int auth_header(Cred *c, const char *method, const char *uri, const char *cn, char *out, int size)
{
    char buf[400];
    int n;
    if (c->ch.scheme == AUTH_BASIC) {
        n = snprintf(buf, sizeof buf, "%s:%s", c->user, c->pass);
        if (n >= (int)sizeof buf || size < 7) return 0;
        strcpy(out, "Basic ");
        n = auth_base64((unsigned char *)buf, n, out + 6, size - 6);
        memset(buf, 0, sizeof buf);
        return n ? n + 6 : 0;
    } else {
        char ha1[33], ha2[33], resp[33], nc[9], user[140], realm[260], u[600];
        snprintf(buf, sizeof buf, "%s:%s:%s", c->user, c->ch.realm, c->pass);
        md5_hex(buf, ha1);
        if (c->ch.sess) {
            snprintf(buf, sizeof buf, "%s:%s:%s", ha1, c->ch.nonce, cn);
            md5_hex(buf, ha1);
        }
        if (snprintf(u, sizeof u, "%s:%s", method, uri) >= (int)sizeof u) return 0;
        md5_hex(u, ha2);
        c->nc++;
        sprintf(nc, "%08lx", c->nc & 0xffffffffUL);
        if (c->ch.qop) snprintf(buf, sizeof buf, "%s:%s:%s:%s:auth:%s", ha1, c->ch.nonce, nc, cn, ha2);
        else snprintf(buf, sizeof buf, "%s:%s:%s", ha1, c->ch.nonce, ha2);
        md5_hex(buf, resp);
        memset(buf, 0, sizeof buf);
        quoted(c->user, user, sizeof user);
        quoted(c->ch.realm, realm, sizeof realm);
        n = snprintf(out, size, "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\"",
                     user, realm, c->ch.nonce, uri, resp);
        if (n < size && c->ch.sess) n += snprintf(out + n, size - n, ", algorithm=MD5-sess");
        if (n < size && c->ch.opaque[0]) n += snprintf(out + n, size - n, ", opaque=\"%s\"", c->ch.opaque);
        if (n < size && c->ch.qop) n += snprintf(out + n, size - n, ", qop=auth, nc=%s, cnonce=\"%s\"", nc, cn);
        return n < size ? n : 0;
    }
}
