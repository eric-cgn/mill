/* ldaputil.c: see ldaputil.h */
#include <string.h>

#include "ldaputil.h"

/* ------------------------------------------------------------ writing */

void ldap_out(LdapOut *o, unsigned char *buf, long size)
{
    o->p = buf;
    o->n = 0;
    o->size = size;
    o->full = 0;
}

static void put(LdapOut *o, unsigned char b)
{
    if (o->n < o->size) o->p[o->n++] = b;
    else o->full = 1;
}

/* A definite length: short form below 128, else long form (X.690 8.1.3) */
static void put_len(LdapOut *o, long n)
{
    if (n < 128) { put(o, (unsigned char)n); return; }
    if (n < 256) { put(o, 0x81); put(o, (unsigned char)n); return; }
    if (n < 65536L) { put(o, 0x82); put(o, (unsigned char)(n >> 8)); put(o, (unsigned char)n); return; }
    put(o, 0x83);
    put(o, (unsigned char)(n >> 16));
    put(o, (unsigned char)(n >> 8));
    put(o, (unsigned char)n);
}

long ldap_begin(LdapOut *o, unsigned char tag)
{
    long at;
    put(o, tag);
    at = o->n;
    put(o, 0); put(o, 0); put(o, 0); put(o, 0);     /* room for the longest length */
    return at;
}

void ldap_end(LdapOut *o, long at)
{
    long len = o->n - at - 4, k;
    unsigned char l[4];
    LdapOut t;
    if (o->full) return;
    ldap_out(&t, l, 4);
    put_len(&t, len);
    k = 4 - t.n;                    /* the room not needed */
    memcpy(o->p + at, l, t.n);
    memmove(o->p + at + t.n, o->p + at + 4, len);
    o->n -= k;
}

/* INTEGER and ENUMERATED: the fewest two's complement bytes (X.690 8.3) */
void ldap_put_int(LdapOut *o, unsigned char tag, long v)
{
    short n = 4;
    put(o, tag);
    while (n > 1 && ((v >> (8 * (n - 1) - 1)) == 0 || (v >> (8 * (n - 1) - 1)) == -1)) n--;
    put(o, (unsigned char)n);
    while (n--) put(o, (unsigned char)(v >> (8 * n)));
}

void ldap_put_str(LdapOut *o, unsigned char tag, const char *s, long n)
{
    put(o, tag);
    put_len(o, n);
    while (n-- > 0) put(o, (unsigned char)*s++);
}

/* RFC 1777 4.1: LDAPMessage ::= SEQUENCE { messageID, protocolOp } */
static long message(LdapOut *o, long id)
{
    long at = ldap_begin(o, 0x30);
    ldap_put_int(o, 0x02, id);
    return at;
}

/* 4.3: BindRequest ::= [APPLICATION 0] SEQUENCE { version, name, simple [0] } */
void ldap_bind(LdapOut *o, long id, short version, const char *dn, const char *password)
{
    long m = message(o, id), b = ldap_begin(o, 0x60);
    ldap_put_int(o, 0x02, version);
    ldap_put_str(o, 0x04, dn, strlen(dn));
    ldap_put_str(o, 0x80, password, strlen(password));
    ldap_end(o, b);
    ldap_end(o, m);
}

/* 4.4: UnbindRequest ::= [APPLICATION 2] NULL */
void ldap_unbind(LdapOut *o, long id)
{
    long m = message(o, id);
    put(o, 0x42);
    put(o, 0);
    ldap_end(o, m);
}

static short hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c |= 32;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* One filter, "(" ... ")": RFC 1960 2. *ps moves past it. */
static short item(LdapOut *o, const char **ps, short depth)
{
    const char *s = *ps, *a, *e;
    long at;
    if (*s++ != '(' || depth > 16) return 0;
    if (*s == '&' || *s == '|' || *s == '!') {
        /* and [0], or [1]: SET OF Filter; not [2]: Filter */
        short count = 0;
        at = ldap_begin(o, *s == '&' ? 0xA0 : *s == '|' ? 0xA1 : 0xA2);
        if (*s++ == '!') { if (!item(o, &s, depth + 1)) return 0; count = 1; }
        else while (*s == '(') { if (!item(o, &s, depth + 1)) return 0; count++; }
        if (!count) return 0;
        ldap_end(o, at);
    } else {
        /* attr filtertype value; present is attr=*, substrings attr=[initial]*[any*]*[final] */
        unsigned char tag = 0xA3;
        char v[256];
        short stars[64], nstar = 0;
        long k = 0;
        for (a = s; *s && !strchr("=~<>()", *s); s++) {}
        if (s == a) return 0;
        e = s;
        while (e > a && e[-1] == ' ') e--;
        if (*s == '~' || *s == '<' || *s == '>') {
            tag = *s == '~' ? 0xA8 : *s == '>' ? 0xA5 : 0xA6;
            if (*++s != '=') return 0;
        } else if (*s != '=') return 0;
        s++;
        while (*s && *s != ')') {
            char c = *s++;
            if (c == '*' && tag == 0xA3) {
                if (nstar < 64) stars[nstar++] = (short)k;
                continue;
            }
            if (c == '\\' && *s) {
                if (hexval(s[0]) >= 0 && hexval(s[1]) >= 0) { c = (char)(hexval(s[0]) * 16 + hexval(s[1])); s += 2; }
                else c = *s++;          /* RFC 1960's \* \( \) \\ */
            } else if (c == '(') return 0;
            if (k < (long)sizeof v - 1) v[k++] = c;
        }
        v[k] = 0;
        if (tag == 0xA3 && nstar == 1 && k == 0) {
            ldap_put_str(o, 0x87, a, e - a);    /* present [7] */
        } else if (nstar) {
            /* substrings [4]: SEQUENCE { type, SEQUENCE OF { initial [0], any [1], final [2] } } */
            long sq, i, from = 0;
            at = ldap_begin(o, 0xA4);
            ldap_put_str(o, 0x04, a, e - a);
            sq = ldap_begin(o, 0x30);
            for (i = 0; i <= nstar; i++) {
                long to = i < nstar ? stars[i] : k;
                if (to > from) ldap_put_str(o, i == 0 ? 0x80 : i == nstar ? 0x82 : 0x81, v + from, to - from);
                from = to;
            }
            ldap_end(o, sq);
            ldap_end(o, at);
        } else {
            /* AttributeValueAssertion ::= SEQUENCE { type, value } */
            at = ldap_begin(o, tag);
            ldap_put_str(o, 0x04, a, e - a);
            ldap_put_str(o, 0x04, v, k);
            ldap_end(o, at);
        }
    }
    if (*s != ')') return 0;
    *ps = s + 1;
    return 1;
}

short ldap_filter(LdapOut *o, const char *filter)
{
    char f[300];
    const char *s = filter;
    while (*s == ' ') s++;
    if (*s != '(') {
        long n = strlen(s);
        if (n > (long)sizeof f - 3) return 0;
        f[0] = '(';
        memcpy(f + 1, s, n);
        f[n + 1] = ')';
        f[n + 2] = 0;
        s = f;
    }
    if (!item(o, &s, 0)) return 0;
    while (*s == ' ') s++;
    return !*s && !o->full;
}

/* 4.5.1: SearchRequest ::= [APPLICATION 3] SEQUENCE { baseObject, scope, derefAliases,
   sizeLimit, timeLimit, attrsOnly, filter, attributes } */
short ldap_search(LdapOut *o, long id, const char *base, short scope, long sizeLimit, short typesOnly,
                  const char *filter, const char *attrs)
{
    long m = message(o, id), sr = ldap_begin(o, 0x63), as;
    const char *p = attrs;
    ldap_put_str(o, 0x04, base, strlen(base));
    ldap_put_int(o, 0x0A, scope);
    ldap_put_int(o, 0x0A, 0);           /* neverDerefAliases */
    ldap_put_int(o, 0x02, sizeLimit);
    ldap_put_int(o, 0x02, 0);           /* no time limit of ours */
    put(o, 0x01); put(o, 1); put(o, typesOnly ? 0xFF : 0);
    if (!ldap_filter(o, filter[0] ? filter : "(objectClass=*)")) return 0;
    as = ldap_begin(o, 0x30);
    while (*p) {
        const char *e;
        while (*p == ' ' || *p == ',') p++;
        for (e = p; *e && *e != ','; e++) {}
        while (e > p && e[-1] == ' ') e--;
        if (e > p && !(e - p == 1 && *p == '*')) ldap_put_str(o, 0x04, p, e - p);
        p = e;
        while (*p && *p != ',') p++;
    }
    ldap_end(o, as);
    ldap_end(o, sr);
    ldap_end(o, m);
    return !o->full;
}

/* ------------------------------------------------------------ reading */

short ldap_byte(LdapIn *in)
{
    if (in->at >= in->have) {
        in->at = 0;
        in->have = in->bad ? 0 : in->fill(in, in->buf, sizeof in->buf);
        if (in->have <= 0) { in->have = 0; in->bad = 1; return 0; }
    }
    in->pos++;
    return in->buf[in->at++];
}

short ldap_tag(LdapIn *in, long *len)
{
    short tag = ldap_byte(in), b = ldap_byte(in), n;
    *len = b;
    if (b & 0x80) {
        n = b & 0x7F;
        if (!n || n > 4) { in->bad = 1; *len = 0; return -1; }
        for (*len = 0; n--; ) *len = (*len << 8) | ldap_byte(in);
        if (*len < 0) { in->bad = 1; *len = 0; }
    }
    return in->bad ? -1 : tag;
}

long ldap_bytes(LdapIn *in, long len, char *out, long size)
{
    long k = 0;
    while (len-- > 0 && !in->bad) {
        short c = ldap_byte(in);
        if (k < size - 1) out[k++] = (char)c;
    }
    if (size) out[k] = 0;
    return k;
}

void ldap_skip(LdapIn *in, long len)
{
    while (len > 0 && !in->bad) {
        long k = in->have - in->at;
        if (k <= 0) { ldap_byte(in); len--; continue; }
        if (k > len) k = len;
        in->at += k;
        in->pos += k;
        len -= k;
    }
}

long ldap_int(LdapIn *in, long len)
{
    long v = 0;
    short first = 1;
    while (len-- > 0) {
        short b = ldap_byte(in);
        if (first && (b & 0x80)) v = -1;
        v = (v << 8) | b;
        first = 0;
    }
    return v;
}

/* RFC 1777 4: LDAPResult ::= SEQUENCE { resultCode, matchedDN, errorMessage }; v3 adds
   referral [3] (RFC 2251 4.1.10), skipped with anything else after the message */
long ldap_result(LdapIn *in, long len, char *msg, long size)
{
    long start = in->pos, code = -1, k;
    if (size) msg[0] = 0;
    if (ldap_tag(in, &k) == 0x0A) code = ldap_int(in, k);
    else ldap_skip(in, k);
    ldap_tag(in, &k);
    ldap_skip(in, k);                   /* matchedDN */
    ldap_tag(in, &k);
    ldap_bytes(in, k, msg, size);       /* errorMessage */
    k = len - (in->pos - start);
    if (k > 0) ldap_skip(in, k);
    return in->bad ? -1 : code;
}

/* ------------------------------------------------------------ addresses */

static long decode(const char *s, long n, char *out, long size)
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
    return o;
}

static short same_ci(const char *a, const char *b, long n)
{
    while (n-- > 0) if ((*a++ | 32) != (*b++ | 32)) return 0;
    return 1;
}

/* RFC 1959 2 (and 2255 3): ldap://hostport/dn[?attributes[?scope[?filter[?extensions]]]] */
short ldap_url(const char *path, LdapUrl *u)
{
    const char *part[5], *p = path;
    long len[5];
    short n = 0, i;
    char scope[8];
    memset(u, 0, sizeof *u);
    u->table = -1;
    if (*p == '/') p++;
    part[0] = p;
    for (; *p; p++) if (*p == '?' && n < 4) { len[n] = p - part[n]; part[++n] = p + 1; }
    len[n] = p - part[n];
    for (i = n + 1; i < 5; i++) { part[i] = ""; len[i] = 0; }
    decode(part[0], len[0], u->dn, sizeof u->dn);
    decode(part[1], len[1], u->attrs, sizeof u->attrs);
    decode(part[2], len[2], scope, sizeof scope);
    decode(part[3], len[3], u->filter, sizeof u->filter);
    if (!scope[0] || same_ci(scope, "base", 5)) u->scope = kLdapBase;
    else if (same_ci(scope, "one", 4)) u->scope = kLdapOne;
    else if (same_ci(scope, "sub", 4)) u->scope = kLdapSub;
    else return 0;
    /* extensions: [!]type[=value], separated by commas (2255 5) */
    for (p = part[4]; p < part[4] + len[4]; ) {
        const char *e = memchr(p, ',', part[4] + len[4] - p), *eq;
        if (!e) e = part[4] + len[4];
        if (*p == '!') p++;
        eq = memchr(p, '=', e - p);
        if (eq && eq - p == 8 && same_ci(p, "bindname", 8)) decode(eq + 1, e - eq - 1, u->bindname, sizeof u->bindname);
        else if (eq && eq - p == 8 && same_ci(p, "x-layout", 8)) u->table = (e - eq - 1 == 5 && same_ci(eq + 1, "table", 5));
        p = e + 1;
    }
    return 1;
}

long ldap_escape(const char *s, char *out, long size)
{
    static const char hex[] = "0123456789ABCDEF";
    long o = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c <= ' ' || c >= 127 || strchr("%?#\"<>", c)) {
            if (o + 3 >= size) return -1;
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        } else {
            if (o + 1 >= size) return -1;
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
    return o;
}

void ldap_name_filter(const char *name, char *out, long size)
{
    char e[200];
    long k = 0, o;
    for (; *name && k < (long)sizeof e - 4; name++) {
        if (strchr("*()\\", *name)) {
            static const char hex[] = "0123456789abcdef";
            e[k++] = '\\';
            e[k++] = hex[(unsigned char)*name >> 4];
            e[k++] = hex[*name & 15];
        } else e[k++] = *name;
    }
    e[k] = 0;
    o = 0;
    {
        const char *parts[5];
        short i;
        parts[0] = "(|(cn=";
        parts[1] = e;
        parts[2] = "*)(sn=";
        parts[3] = e;
        parts[4] = "*))";
        for (i = 0; i < 5; i++) {
            const char *p = parts[i];
            while (*p && o < size - 1) out[o++] = *p++;
        }
    }
    out[o] = 0;
}

short ldap_text(const unsigned char *s, long n)
{
    while (n-- > 0) {
        unsigned char c = *s++;
        if (c < 32 && c != 9 && c != 10 && c != 13) return 0;
    }
    return 1;
}
