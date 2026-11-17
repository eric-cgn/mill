/* imaputil.h: the parts of the IMAP Plug-In that need no Mac: reading IMAP's responses (the
   syntax both IMAP4, RFC 1730, and IMAP4rev1, RFC 2060, share: atoms, quoted strings,
   literals, NIL, and parenthesized lists), FETCH's ENVELOPE and BODYSTRUCTURE, LIST, and
   imap: addresses (RFC 2192). Plain C, so tests/test_imap.c runs them on a host. */
#ifndef IMAPUTIL_H
#define IMAPUTIL_H

/* A response held whole, its literals in place as "{n}" CR LF and n bytes */
typedef struct {
    const char *p, *end;
} ImapIn;
enum { kImapEnd, kImapAtom, kImapString, kImapNil, kImapOpen, kImapClose };
typedef struct {
    short kind;
    const char *s;      /* an atom's or a string's bytes (a quoted string's with its \ escapes) */
    long n;
    short quoted;
} ImapTok;
/* The next token (spaces skipped) */
short imap_tok(ImapIn *in, ImapTok *t);
/* The rest of a list after its "(": skipped, with any lists in it */
void imap_skip_list(ImapIn *in);
/* A string's or an atom's bytes into out (always ended with 0; "" for NIL): its length */
long imap_str(const ImapTok *t, char *out, long size);

/* The n in a line's closing "{n}" (a literal follows), or -1 */
long imap_literal(const char *line, long len);

/* An untagged FETCH (RFC 2060 7.4.2, RFC 1730 7.4.2): "* 12 FETCH (UID 20 FLAGS (\Seen)
   RFC822.SIZE 1234 ENVELOPE (...))". 1 if it was one. The envelope's date, subject, and
   first From address (its name, else mailbox@host) come out as sent. */
typedef struct {
    long seq, uid, size;
    short seen;
    char date[40], subject[128], from[80];
} ImapMsg;
short imap_fetch(const char *s, long n, ImapMsg *m);

/* An untagged LIST (RFC 2060 7.2.2): the name, and whether it can't be opened (\Noselect).
   1 if it was one. */
short imap_list(const char *s, long n, char *name, long size, short *noselect);

/* A BODYSTRUCTURE's parts (RFC 2060 7.4.2), each leaf in order, with its section number
   for BODY[...] ("1", "2.1"): up to max of them; returns how many there were */
typedef struct {
    char section[16];
    char type[40];          /* "text/plain", lower case */
    char charset[24], encoding[24], name[48];  /* encoding in lower case */
    long size;
} ImapPart;
short imap_parts(const char *s, long n, ImapPart *parts, short max);

/* An address's path (from its slash): /mailbox, /mailbox;UIDVALIDITY=v/;UID=n,
   /mailbox?search (RFC 2192 3), with Mill's own ;X-BEFORE=n on a mailbox for its pages */
typedef struct {
    char mailbox[200];
    char search[200];
    unsigned long uid, uidvalidity;
    long before;
} ImapUrl;
void imap_url(const char *path, ImapUrl *u);

/* s as a quoted string (RFC 2060 4.3), into out: its length, or -1 if it won't go (CR, LF,
   or no room) */
long imap_quote(const char *s, char *out, long size);

#endif
