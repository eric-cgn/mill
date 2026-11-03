/* HTTP authentication: Basic (RFC 7617) and Digest (RFC 7616 with MD5 or MD5-sess, and
   qop=auth or none, as RFC 2069 servers ask). What a person types is kept until Mill
   quits, for the host, port, and realm that asked, and sent again without asking to
   paths under the folder where it was first asked for. */
#ifndef MILL_AUTH_H
#define MILL_AUTH_H

enum { AUTH_NONE, AUTH_BASIC, AUTH_DIGEST };

typedef struct {
    int scheme;             /* AUTH_NONE if there's nothing Mill can answer */
    char realm[128], nonce[128], opaque[128];
    char sess;              /* algorithm=MD5-sess */
    char qop;               /* the server offers qop=auth */
    char stale;             /* the nonce is old but the password was right */
} Challenge;

/* Read one WWW-Authenticate header's value (len bytes), which may hold more than one
   challenge, into c if it has one Mill answers better than what c holds already
   (Digest over Basic). Start with c zeroed and call it for each such header. */
void auth_challenge(const char *v, long len, Challenge *c);

typedef struct {
    char host[64];
    unsigned short port;
    char dir[128];          /* sent without asking to paths under this */
    char user[64], pass[64];
    Challenge ch;
    unsigned long nc;       /* requests made with ch's nonce */
    unsigned long used;     /* when it was last used, to drop the oldest when full */
} Cred;

#define AUTH_MAX 8

typedef struct {
    Cred c[AUTH_MAX];
    int n;
    unsigned long clock;
} Keyring;

/* What to send ahead of asking for a request to host, port, and path: the entry whose
   folder is the longest that path is under, or NULL */
Cred *auth_for_path(Keyring *k, const char *host, unsigned short port, const char *path);
/* The entry for host, port, and realm, or NULL */
Cred *auth_for_realm(Keyring *k, const char *host, unsigned short port, const char *realm);
/* Keep a name and password for the challenge that a request for path got; an entry
   for the same realm is replaced, and its folder widened to cover path's as well */
Cred *auth_keep(Keyring *k, const char *host, unsigned short port, const char *path,
                const Challenge *ch, const char *user, const char *pass);
/* A new challenge for an entry (a stale nonce): the count starts again */
void auth_renew(Cred *c, const Challenge *ch);
void auth_forget(Keyring *k, Cred *c);
/* Forget every password, wiping them from memory */
void auth_clear(Keyring *k);

/* The Authorization header's value for method and uri (the request's path) into out;
   cnonce is a few hex digits that change each time (for Digest). Returns its length,
   or 0 if it won't fit. */
int auth_header(Cred *c, const char *method, const char *uri, const char *cnonce, char *out, int size);

/* The pieces, for tests */
void auth_md5(const unsigned char *data, unsigned long len, unsigned char digest[16]);
int auth_base64(const unsigned char *in, int len, char *out, int size);

#endif
