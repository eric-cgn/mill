/* HTTP's Basic and Digest authentication (RFC 7617, RFC 7616 and RFC 2069, with MD5 or
   MD5-sess, and qop=auth or none): reading a server's challenge and answering it. The
   http Plug-In's; the names and passwords themselves are Mill's (auth.h). */
#ifndef MILL_HTTPAUTH_H
#define MILL_HTTPAUTH_H

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

/* The Authorization header's value for a challenge, a name and password, the count of
   requests made with the challenge's nonce so far (1 for the first), the method, and uri
   (the request's path); cnonce is a few hex digits that change each time (for Digest).
   Returns its length, or 0 if it won't fit. */
int auth_header(const Challenge *ch, const char *name, const char *password, unsigned long count,
                const char *method, const char *uri, const char *cnonce, char *out, int size);

#endif
