/* HTTP/1.1 replies (RFC 2068): where a reply's head ends, what it says about its body,
   the chunked transfer-coding, and byte ranges. The http Plug-In's; nothing here reads
   or writes the network, so the host tests feed it replies in pieces of any size. */
#ifndef MILL_HTTP11_H
#define MILL_HTTP11_H

/* What a reply's head says */
typedef struct {
    int code;               /* the status; 0 if the first line isn't "HTTP/n.n nnn" */
    long length;            /* Content-Length; -1 if none */
    char chunked;           /* Transfer-Encoding ends in chunked */
    char bad;               /* a transfer coding Mill can't undo, or a broken (or doubled
                               and different) Content-Length */
    char range_bad;         /* a broken or doubled Content-Range */
    char closes;            /* the server closes the connection after it: a reply before
                               HTTP/1.1, or Connection: close (8.1.2.1, 14.10) */
    long first, last, total;    /* Content-Range: bytes first-last/total; first is -1 if
                                   there's no part, total -1 if none or '*' */
} HttpHead;

/* The length of the head at the start of buf (len bytes), through its blank line (CRLF
   or LF, RFC 2068 19.3); 0 if it isn't all there yet */
long http_head_end(const char *buf, long len);

/* Read a head of len bytes (from http_head_end) into h */
void http_head(const char *head, long len, HttpHead *h);

/* How the body ends (RFC 2068 4.4): none after the head (1xx, 204, 304, or a reply to
   HEAD), after length bytes, after the last chunk, or when the server closes; or
   HTTP_BAD, a head that can't be trusted to say */
enum { HTTP_NONE, HTTP_LENGTH, HTTP_CHUNKED, HTTP_CLOSE, HTTP_BAD };
int http_body(const HttpHead *h, int head_request);

/* Remove the header lines named name (with its colon: "Transfer-Encoding:") from the
   head that starts buf, head bytes of the len there, moving what follows down. Returns
   how many bytes went. */
long http_drop(char *buf, long head, long len, const char *name);

/* The reply to a request for bytes first to last (first -1: no Range was sent; the
   request's Range is read with range_asked, core/range.h):
   RANGE_PART, a 206 with just those bytes (fewer only if the file ends sooner);
   RANGE_WHOLE, a 200 instead, so the server can't seek; RANGE_BAD, a 206 that isn't
   that part, or a 206 to no Range; RANGE_PASS, anything else, passed on as it is (416
   and the other errors as pages, redirects, logins) */
enum { RANGE_PASS, RANGE_PART, RANGE_WHOLE, RANGE_BAD };
int http_range_reply(const HttpHead *h, long first, long last);

/* The chunked transfer-coding (RFC 2068 3.6), undone in place as the bytes come: start
   with it zeroed */
typedef struct {
    short state;
    long left;              /* the chunk's bytes still to come; its size, while it's read */
    long line;              /* bytes of the size line so far, or of the trailers */
} Chunked;

/* Decode len bytes at buf: the chunks' data goes to the start of buf, and its length is
   returned; *used is how many of the len were read, fewer only after the end. -1 if the
   coding is broken. Extensions and trailers are skipped: a size line, with its
   extensions, may be 1K, and the trailers 4K in all; a size goes to 2^31 - 1. */
long chunked_run(Chunked *c, char *buf, long len, long *used);
int chunked_done(const Chunked *c);

#endif
