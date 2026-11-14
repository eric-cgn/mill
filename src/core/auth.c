/* MD5 and base64, and the keyring of names and passwords; see auth.h. HTTP's own
   Basic and Digest are in httpauth.c, with the http Plug-In. */
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

/* ------------------------------------------------------------ the keyring */

/* key is one of c's keys (its lines) */
static int has_key(const Cred *c, const char *key)
{
    const char *p = c->keys;
    long n = strlen(key);
    if (!n) return 0;
    while (*p) {
        const char *e = strchr(p, '\n');
        long len = e ? e - p : (long)strlen(p);
        if (len == n && !memcmp(p, key, n)) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

Cred *auth_find(Keyring *k, const char *key)
{
    int i;
    for (i = 0; i < k->n; i++)
        if (has_key(&k->c[i], key)) {
            k->c[i].used = ++k->clock;
            return &k->c[i];
        }
    return NULL;
}

Cred *auth_save(Keyring *k, const char *keys, const char *user, const char *pass)
{
    Cred *c;
    const char *p = keys;
    /* what any of these keys found before is replaced */
    while (*p) {
        const char *e = strchr(p, '\n');
        char one[AUTH_KEYS];
        long len = e ? e - p : (long)strlen(p);
        if (len < (long)sizeof one) {
            memcpy(one, p, len);
            one[len] = 0;
            while ((c = auth_find(k, one)) != NULL) auth_forget(k, c);
        }
        if (!e) break;
        p = e + 1;
    }
    if (k->n < AUTH_MAX) c = &k->c[k->n++];
    else {
        int i;
        for (c = &k->c[0], i = 1; i < k->n; i++) if (k->c[i].used < c->used) c = &k->c[i];
    }
    memset(c, 0, sizeof *c);
    snprintf(c->keys, sizeof c->keys, "%s", keys);
    snprintf(c->user, sizeof c->user, "%s", user);
    snprintf(c->pass, sizeof c->pass, "%s", pass);
    c->used = ++k->clock;
    return c;
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
