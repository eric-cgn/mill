/* mime.h: reading Internet messages, for the News and IMAP Plug-Ins: header fields (RFC 822),
   encoded words in them (RFC 1522), and MIME bodies (RFC 2045: quoted-printable and base64).
   Plain C with no Mac calls, so tests/test_mime.c runs it on a host. Each Plug-In that uses
   it compiles it in. */
#ifndef MIME_H
#define MIME_H

/* Character sets, as MillPlugIn.h's ToMacRoman takes them */
enum { kMimeGuess = 0, kMimeLatin1 = 2, kMimeUTF8 = 3 };

/* A header field's value from a block of header lines (CR LF or LF line ends): name in any
   case, continuation lines joined with a space (RFC 822 3.1.1), no leading or trailing
   space. Its length, or -1 if there's none; cut to fit out. */
long mime_header(const char *head, long len, const char *name, char *out, long size);

/* A parameter of a structured value ("text/plain; charset=ISO-8859-1"): RFC 2045 5.1. Its
   length, or -1 if there's none. */
long mime_param(const char *value, const char *name, char *out, long size);

/* value's first word, lower case, up to ';' or a space: "text/plain" */
void mime_type(const char *value, char *out, long size);

/* A charset name as one of kMime... (US-ASCII reads as Latin-1) */
short mime_charset(const char *name);

/* in with its encoded words (=?charset?Q?...?= or ?B?) decoded, into out (always ended
   with 0); white space between two encoded words is dropped (RFC 1522 6.2). Returns the
   charset of the words decoded (kMimeGuess if none). */
short mime_words(const char *in, char *out, long size);

/* One line of quoted-printable (without its line end) decoded, in place: RFC 2045 6.7.
   Returns the new length; *soft is set if it ended with '=' (it joins the next line). */
long mime_qp(char *line, long len, short *soft);

/* base64 a piece at a time: RFC 2045 6.8. Characters outside the alphabet are skipped;
   '=' ends it. Returns the bytes written to out (at most 3 * len / 4 + 3). */
typedef struct {
    unsigned long bits;
    short n, done;
} MimeB64;
long mime_b64(MimeB64 *s, const char *in, long len, unsigned char *out);

/* A mailbox's name to show (RFC 822 6.1): the phrase of "Name <addr>", the comment of
   "addr (Name)", else the address; quotes taken off */
void mime_who(const char *in, char *out, long size);

/* A date (RFC 822 5.1, "Fri, 14 Nov 1997 12:00:00 -0800") as "14 Nov 1997"; in as it
   is if it isn't one */
void mime_day(const char *in, char *out, long size);

#endif
