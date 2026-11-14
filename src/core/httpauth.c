/* HTTP Basic and Digest authentication (RFC 7617, RFC 7616 with MD5); see httpauth.h */
#include <stdio.h>
#include <string.h>

#include "auth.h"
#include "httpauth.h"

/* MD5 of a string, as 32 lowercase hex digits */
static void md5_hex(const char *s, char out[33])
{
    unsigned char d[16];
    int i;
    auth_md5((const unsigned char *)s, strlen(s), d);
    for (i = 0; i < 16; i++) sprintf(out + i * 2, "%02x", d[i]);
}

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

int auth_header(const Challenge *ch, const char *name, const char *password, unsigned long count,
                const char *method, const char *uri, const char *cn, char *out, int size)
{
    char buf[400];
    int n;
    if (ch->scheme == AUTH_BASIC) {
        n = snprintf(buf, sizeof buf, "%s:%s", name, password);
        if (n >= (int)sizeof buf || size < 7) return 0;
        strcpy(out, "Basic ");
        n = auth_base64((unsigned char *)buf, n, out + 6, size - 6);
        memset(buf, 0, sizeof buf);
        return n ? n + 6 : 0;
    } else {
        char ha1[33], ha2[33], resp[33], nc[9], user[140], realm[260], u[600];
        snprintf(buf, sizeof buf, "%s:%s:%s", name, ch->realm, password);
        md5_hex(buf, ha1);
        if (ch->sess) {
            snprintf(buf, sizeof buf, "%s:%s:%s", ha1, ch->nonce, cn);
            md5_hex(buf, ha1);
        }
        if (snprintf(u, sizeof u, "%s:%s", method, uri) >= (int)sizeof u) return 0;
        md5_hex(u, ha2);
        sprintf(nc, "%08lx", count & 0xffffffffUL);
        if (ch->qop) snprintf(buf, sizeof buf, "%s:%s:%s:%s:auth:%s", ha1, ch->nonce, nc, cn, ha2);
        else snprintf(buf, sizeof buf, "%s:%s:%s", ha1, ch->nonce, ha2);
        md5_hex(buf, resp);
        memset(buf, 0, sizeof buf);
        quoted(name, user, sizeof user);
        quoted(ch->realm, realm, sizeof realm);
        n = snprintf(out, size, "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\"",
                     user, realm, ch->nonce, uri, resp);
        if (n < size && ch->sess) n += snprintf(out + n, size - n, ", algorithm=MD5-sess");
        if (n < size && ch->opaque[0]) n += snprintf(out + n, size - n, ", opaque=\"%s\"", ch->opaque);
        if (n < size && ch->qop) n += snprintf(out + n, size - n, ", qop=auth, nc=%s, cnonce=\"%s\"", nc, cn);
        return n < size ? n : 0;
    }
}
