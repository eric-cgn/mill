/* ldaputil.h: the parts of the LDAP Plug-In that need no Mac: BER as LDAP uses it (RFC 1777
   4, X.690's definite lengths), the requests it sends, string filters (RFC 1960), and
   ldap: addresses (RFC 1959, with RFC 2255's extensions). Plain C, so tests/test_ldap.c
   runs them on a host. */
#ifndef LDAPUTIL_H
#define LDAPUTIL_H

/* ---- writing BER into a buffer. Each put returns 0 if it won't fit. */
typedef struct {
    unsigned char *p;
    long n, size;
    short full;
} LdapOut;
void ldap_out(LdapOut *o, unsigned char *buf, long size);
/* A constructed element: begin returns where its length goes, end fills it in (moving its
   contents back when the length is short) */
long ldap_begin(LdapOut *o, unsigned char tag);
void ldap_end(LdapOut *o, long at);
void ldap_put_int(LdapOut *o, unsigned char tag, long v);     /* INTEGER 0x02, ENUMERATED 0x0A, BOOLEAN 0x01 */
void ldap_put_str(LdapOut *o, unsigned char tag, const char *s, long n);   /* OCTET STRING 0x04, or a context tag */

/* The requests: RFC 1777 4.1, 4.3, 4.5 */
void ldap_bind(LdapOut *o, long id, short version, const char *dn, const char *password);
void ldap_unbind(LdapOut *o, long id);
enum { kLdapBase = 0, kLdapOne = 1, kLdapSub = 2 };
/* filter: a string filter (RFC 1960); attrs: names separated by commas, or "" for all.
   Returns 0 if the filter isn't one. */
short ldap_search(LdapOut *o, long id, const char *base, short scope, long sizeLimit, short typesOnly,
                  const char *filter, const char *attrs);
/* A string filter as BER (RFC 1960 2, with RFC 2254's \XX escapes too); one without
   parentheses round it is taken as if it had them. 0 if it isn't one. */
short ldap_filter(LdapOut *o, const char *filter);

/* ---- reading BER as it arrives: fill gets more (0 at the end), with ref its own */
typedef struct LdapIn {
    unsigned char buf[512];
    long have, at;
    long pos;           /* bytes read in all */
    long (*fill)(struct LdapIn *in, unsigned char *buf, long size);
    void *ref;
    short bad;          /* ran out, or wasn't BER */
} LdapIn;
short ldap_byte(LdapIn *in);
/* An element's tag and length (-1 bad) */
short ldap_tag(LdapIn *in, long *len);
/* len bytes: up to size - 1 kept in out (ended with 0), the rest skipped. Returns how
   many were kept. */
long ldap_bytes(LdapIn *in, long len, char *out, long size);
void ldap_skip(LdapIn *in, long len);
long ldap_int(LdapIn *in, long len);

/* An LDAPResult's code, after its tag and length (RFC 1777 4); the matched DN skipped and
   the error message into msg; any v3 referral skipped too */
long ldap_result(LdapIn *in, long len, char *msg, long size);

/* ---- addresses: ldap://host/dn?attributes?scope?filter?extensions, the path from its
   slash, %XX decoded part by part. Missing parts are "" (scope is base). */
typedef struct {
    char dn[256];
    char attrs[256];
    short scope;
    char filter[256];
    char bindname[256];     /* the bindname extension (RFC 2255 5) */
    short table;            /* x-layout=table, x-layout=records: 1, 0; -1 if not given */
} LdapUrl;
/* 0 if it's not one (a scope that isn't base, one, or sub) */
short ldap_url(const char *path, LdapUrl *u);
/* s with what a URL part can't hold as it is %XX'd: also "?" and "," for attributes */
long ldap_escape(const char *s, char *out, long size);

/* A Name field as a filter: (|(cn=NAME*)(sn=NAME*)), with * ( ) \ in it escaped */
void ldap_name_filter(const char *name, char *out, long size);

/* Whether n bytes look like text (no control characters but tab, CR, and LF) */
short ldap_text(const unsigned char *s, long n);

#endif
