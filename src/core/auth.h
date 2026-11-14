/* The names and passwords Mill is given, kept until it quits, never shown or written
   to disk. Each is kept under the keys a Plug-In named for it (MillPlugIn.h's
   loginKey): strings Mill only compares, one per line. What they mean, and when a
   login is sent, are the Plug-Ins' (HTTP's Basic and Digest are in httpauth.h). Also
   MD5 and base64, which Mill offers Plug-Ins. */
#ifndef MILL_AUTH_H
#define MILL_AUTH_H

#define AUTH_KEYS 256

typedef struct {
    char keys[AUTH_KEYS];   /* its keys, one per line */
    char user[64], pass[64];
    unsigned long used;     /* when it was last used, to drop the oldest when full */
} Cred;

#define AUTH_MAX 8

typedef struct {
    Cred c[AUTH_MAX];
    int n;
    unsigned long clock;
} Keyring;

/* The login one of whose keys is key, or NULL */
Cred *auth_find(Keyring *k, const char *key);
/* Keep a name and password under keys (one per line), in place of what those keys
   found before; the oldest goes when the keyring is full */
Cred *auth_save(Keyring *k, const char *keys, const char *user, const char *pass);
void auth_forget(Keyring *k, Cred *c);
/* Forget every password, wiping them from memory */
void auth_clear(Keyring *k);

void auth_md5(const unsigned char *data, unsigned long len, unsigned char digest[16]);
/* in as base64 into out: its length, or 0 if it won't fit */
int auth_base64(const unsigned char *in, int len, char *out, int size);

#endif
