/* Host tests for reading HTTP/1.1 replies (src/core/http11.c, RFC 2068): heads, how a
   body ends (4.4), the chunked coding (3.6), and ranges (14.16, 14.35). Each canned
   reply goes through the same steps as the http Plug-In's (src/plug_http.c), arriving
   all at once, a byte at a time, and in random pieces. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/http11.h"
#include "../src/core/range.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* What the Plug-In ends with: R_CUT is EndedEarly (what came is shown, cut off), R_EARLY
   the Fail for a part that ended early */
enum { R_OK, R_UNREADABLE, R_TOO_MANY, R_EARLY, R_WRONG_PART, R_NOTHING, R_CUT };

/* The connection: the reply, given out whole (mode 0), a byte at a time (1), or in
   random pieces (2) */
typedef struct {
    const char *reply;
    long len, pos;
    int mode;
} Conn;

static long recv_some(Conn *c, char *buf, long room)
{
    long n = c->mode == 0 ? room : c->mode == 1 ? 1 : 1 + rand() % (rand() % 4 ? 9 : 600);
    if (n > room) n = room;
    if (n > c->len - c->pos) n = c->len - c->pos;
    memcpy(buf, c->reply + c->pos, n);
    c->pos += n;
    return n;
}

typedef struct {
    int kind;
    long left;
    Chunked ck;
    int extra;
} Body;

static int kept;            /* the Plug-In would keep the connection (TcpKeep) */

static char out[131072];     /* what Mill was given */
static long out_len;

static void write_out(const char *buf, long n)
{
    if (out_len + n > (long)sizeof out) n = sizeof out - out_len;
    memcpy(out + out_len, buf, n);
    out_len += n;
}

/* As the Plug-In's body_out */
static int body_out(Body *b, char *buf, long n)
{
    long used;
    if (b->kind == HTTP_LENGTH) {
        if (n > b->left) {
            n = b->left;
            b->extra = 1;
        }
        b->left -= n;
    } else if (b->kind == HTTP_CHUNKED) {
        long all = n;
        if ((n = chunked_run(&b->ck, buf, n, &used)) < 0) return -1;
        if (used < all) b->extra = 1;
    } else if (b->kind != HTTP_CLOSE) {
        if (n > 0) b->extra = 1;
        return 0;
    }
    if (n > 0) write_out(buf, n);
    return b->kind == HTTP_CLOSE || (b->kind == HTTP_LENGTH ? b->left > 0 : !chunked_done(&b->ck));
}

/* The reply to a request (with Range text if range, and HEAD if head_request), as the
   Plug-In's http_request reads it */
static int run(const char *reply, long len, int mode, const char *request)
{
    static char buf[4096];
    Conn c;
    HttpHead h;
    Body b;
    long have = 0, head = 0, first = -1, last = -1, d, part;
    int interim = 0, more, ranged = range_asked(request, strlen(request), &first, &last);
    int head_request = !memcmp(request, "HEAD ", 5);
    c.reply = reply;
    c.len = len;
    c.pos = 0;
    c.mode = mode;
    out_len = 0;
    kept = 0;
    for (;;) {
        long m = sizeof buf - have;
        if (have > 0 && (head = http_head_end(buf, have)) > 0) {
            http_head(buf, head, &h);
            if (h.code > 199 || h.code < 100 || ++interim > 4) break;
            memmove(buf, buf + head, have -= head);
            head = 0;
            continue;
        }
        if (m == 0 || (m = recv_some(&c, buf + have, m)) == 0) break;
        have += m;
    }
    if (have == 0 && !interim) return R_NOTHING;
    memset(&b, 0, sizeof b);
    b.kind = head ? http_body(&h, head_request) : HTTP_BAD;
    if (b.kind == HTTP_BAD || h.code < 200) return head || have < (long)sizeof buf ? R_UNREADABLE : R_TOO_MANY;
    if ((part = http_range_reply(&h, ranged ? first : -1, last)) == RANGE_BAD) return R_WRONG_PART;
    if (part == RANGE_WHOLE) b.kind = HTTP_NONE;
    b.left = h.length;
    if (h.chunked) {
        d = http_drop(buf, head, have, "Transfer-Encoding:");
        d += http_drop(buf, head - d, have - d, "Content-Length:");
        head -= d;
        have -= d;
    }
    write_out(buf, head);
    for (more = body_out(&b, buf + head, have - head); more > 0;) {
        long m = recv_some(&c, buf, sizeof buf);
        if (m == 0) { more = b.kind == HTTP_CLOSE ? 0 : -2; break; }
        more = body_out(&b, buf, m);
    }
    kept = more == 0 && !h.closes && !b.extra && part != RANGE_WHOLE &&
           (b.kind == HTTP_NONE || (b.kind == HTTP_LENGTH && b.left == 0) || (b.kind == HTTP_CHUNKED && chunked_done(&b.ck)));
    return more == -1 ? R_UNREADABLE : more == -2 ? (part == RANGE_PART ? R_EARLY : R_CUT) : R_OK;
}

static const char GET[] = "GET / HTTP/1.0\r\nHost: h\r\n\r\n";

/* The reply, every way it can arrive, gives result and Mill gets expect (if not NULL) */
static void reply_is(const char *request, const char *reply, long len, int result, const char *expect, int line)
{
    int mode, i;
    if (len < 0) len = strlen(reply);
    for (mode = 0; mode < 3; mode++) {
        for (i = 0; i < (mode == 2 ? 40 : 1); i++) {
            int got = run(reply, len, mode, request);
            if (got != result || (expect && (out_len != (long)strlen(expect) || memcmp(out, expect, out_len)))) {
                printf("FAIL %s:%d: mode %d: result %d (wanted %d), gave \"%.*s\"\n", __FILE__, line, mode, got, result,
                       (int)(out_len > 200 ? 200 : out_len), out);
                failures++;
                return;
            }
        }
    }
}
#define REPLY(req, rep, res, exp) reply_is(req, rep, -1, res, exp, __LINE__)

static void test_head(void)
{
    HttpHead h;
    const char *s = "HTTP/1.1 206 Partial Content\r\nContent-length:  12 \r\ncontent-range: bytes 100-111/5000\r\n"
                    "Transfer-Encoding: identity\r\nAccept-Ranges: bytes\r\n\r\nbody";
    long e = http_head_end(s, strlen(s));
    CHECK(e == (long)strlen(s) - 4);
    http_head(s, e, &h);
    CHECK(h.code == 206 && h.length == 12 && !h.chunked && !h.bad && h.first == 100 && h.last == 111 && h.total == 5000);
    CHECK(http_head_end("HTTP/1.1 200 OK\r\n\r", 18) == 0);
    CHECK(http_head_end("HTTP/1.1 200 OK\n\nx", 18) == 17);
    CHECK(http_head_end("HTTP/1.1 200 OK\r\nA: b\n\r\nx", 25) == 24);
    s = "HTTP/1.1 200 OK\r\nContent-Range: bytes 0-9/*\r\nTransfer-Encoding: x-gzip, chunked\r\n\r\n";
    http_head(s, strlen(s), &h);
    CHECK(h.first == 0 && h.last == 9 && h.total == -1 && h.chunked && h.bad);
    s = "HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked;q=1\r\n\r\n";
    http_head(s, strlen(s), &h);
    CHECK(h.chunked && !h.bad && http_body(&h, 0) == HTTP_CHUNKED);
    /* status lines */
    http_head("HTTP/1.0 404 Not Found\r\n\r\n", 26, &h);
    CHECK(h.code == 404 && http_body(&h, 0) == HTTP_CLOSE);
    http_head("HTTP/1.1 2000 OK\r\n\r\n", 20, &h);
    CHECK(h.code == 0 && http_body(&h, 0) == HTTP_BAD);
    http_head("HTTP/x 200 OK\r\n\r\n", 17, &h);
    CHECK(h.code == 0);
    http_head("ICY 200 OK\r\n\r\n", 14, &h);
    CHECK(h.code == 0);
    /* lengths */
    s = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n";
    http_head(s, strlen(s), &h);
    CHECK(!h.bad && http_body(&h, 0) == HTTP_LENGTH);
    s = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n";
    http_head(s, strlen(s), &h);
    CHECK(h.bad && http_body(&h, 0) == HTTP_BAD);
    s = "HTTP/1.1 200 OK\r\nContent-Length: 99999999999\r\n\r\n";
    http_head(s, strlen(s), &h);
    CHECK(h.bad);
    s = "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n";
    http_head(s, strlen(s), &h);
    CHECK(h.bad);
    s = "HTTP/1.1 304 Not Modified\r\nContent-Length: 5\r\n\r\n";
    http_head(s, strlen(s), &h);
    CHECK(http_body(&h, 0) == HTTP_NONE);
    /* Content-Range that's wrong in itself */
    s = "HTTP/1.1 206 Partial\r\nContent-Range: bytes 9-5/100\r\n\r\n";
    http_head(s, strlen(s), &h);
    CHECK(h.range_bad && h.first == -1);
    s = "HTTP/1.1 206 Partial\r\nContent-Range: bytes 0-100/100\r\n\r\n";
    http_head(s, strlen(s), &h);
    CHECK(h.range_bad && !h.bad);
    s = "HTTP/1.1 206 Partial\r\nContent-Range: 0-1/100\r\n\r\n";
    http_head(s, strlen(s), &h);
    CHECK(h.range_bad);
    s = "HTTP/1.1 416 Requested range not satisfiable\r\nContent-Range: bytes */100\r\n\r\n";
    http_head(s, strlen(s), &h);
    CHECK(!h.range_bad && h.first == -1 && h.total == 100);
    s = "HTTP/1.1 416 Requested range not satisfiable\r\nContent-Range: bytes */*\r\n\r\n";
    http_head(s, strlen(s), &h);
    CHECK(h.range_bad);
}

static void test_drop(void)
{
    char b[200];
    long d;
    strcpy(b, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nX: y\r\ntransfer-encoding: chunked\r\n\r\nBODY");
    d = http_drop(b, strlen(b) - 4, strlen(b), "Transfer-Encoding:");
    CHECK(d == 56 && !memcmp(b, "HTTP/1.1 200 OK\r\nX: y\r\n\r\nBODY", 29));
    strcpy(b, "HTTP/1.1 200 OK\r\nX-Transfer-Encoding: a\r\n\r\n");
    CHECK(http_drop(b, strlen(b), strlen(b), "Transfer-Encoding:") == 0);
}

static void test_range_asked(void)
{
    long f, l;
    CHECK(range_asked("GET / HTTP/1.0\r\nRange: bytes=10-19\r\n\r\n", 39, &f, &l) == 1 && f == 10 && l == 19);
    CHECK(range_asked("GET / HTTP/1.0\r\nrange:bytes=4096-\r\n\r\n", 37, &f, &l) == 1 && f == 4096 && l == -1);
    CHECK(range_asked(GET, strlen(GET), &f, &l) == 0);
    CHECK(range_asked("GET / HTTP/1.0\r\nRange: bytes=-500\r\n\r\n", 37, &f, &l) == -1);
    CHECK(range_asked("GET / HTTP/1.0\r\nRange: bytes=1-2,4-5\r\n\r\n", 40, &f, &l) == -1);
    CHECK(range_asked("GET / HTTP/1.0\r\nRange: bytes=5-3\r\n\r\n", 36, &f, &l) == -1);
    CHECK(range_asked("GET / HTTP/1.0\r\nRange: pages=1-3\r\n\r\n", 36, &f, &l) == -1);
}

/* What a protocol that can seek sends for a request (core/range.c, FileHead), and what
   Mill reads back from it */
#define ASK(r) "GET /f HTTP/1.0\r\nHost: h\r\n" r "\r\n"
static void test_range_part(void)
{
    long f, l, size;
    char h[400];
    const char *r;
    /* no Range, or one Mill never asks for: the whole */
    CHECK(range_part(GET, strlen(GET), 1000, &f, &l) == 0 && f == 0 && l == 999);
    r = ASK("Range: bytes=-5\r\n");
    CHECK(range_part(r, strlen(r), 1000, &f, &l) == 0 && f == 0 && l == 999);
    /* a part, and one that runs past the end */
    r = ASK("Range: bytes=100-199\r\n");
    CHECK(range_part(r, strlen(r), 1000, &f, &l) == 1 && f == 100 && l == 199);
    r = ASK("Range: bytes=900-5000\r\n");
    CHECK(range_part(r, strlen(r), 1000, &f, &l) == 1 && f == 900 && l == 999);
    r = ASK("Range: bytes=512-\r\n");
    CHECK(range_part(r, strlen(r), 1000, &f, &l) == 1 && f == 512 && l == 999);
    /* a start past the end is ignored (14.36.1); so is any Range with the size unknown */
    r = ASK("Range: bytes=1000-1001\r\n");
    CHECK(range_part(r, strlen(r), 1000, &f, &l) == 0 && f == 0 && l == 999);
    r = ASK("Range: bytes=0-0\r\n");
    CHECK(range_part(r, strlen(r), 0, &f, &l) == 0 && f == 0 && l == -1);
    CHECK(range_part(r, strlen(r), -1, &f, &l) == 0 && f == 0 && l == -1);
    CHECK(range_part(r, strlen(r), 1, &f, &l) == 1 && f == 0 && l == 0);
    /* the heads */
    CHECK(range_head(h, sizeof h, "application/zip", 1000, 1, 100, 199) > 0);
    CHECK(!strcmp(h, "HTTP/1.0 206 Partial Content\r\nContent-Type: application/zip\r\nContent-Range: bytes 100-199/1000\r\n"
                     "Content-Length: 100\r\nAccept-Ranges: bytes\r\n\r\n"));
    CHECK(range_head(h, sizeof h, "text/plain", 1000, 0, 0, 999) > 0);
    CHECK(!strcmp(h, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 1000\r\nAccept-Ranges: bytes\r\n\r\n"));
    CHECK(range_head(h, sizeof h, "text/plain", -1, 0, 0, -1) > 0);
    CHECK(!strcmp(h, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\n"));
    CHECK(range_head(h, 20, "text/plain", 1000, 0, 0, 999) == -1);
    /* Mill reads them back */
    range_head(h, sizeof h, "application/zip", 1000, 1, 100, 199);
    CHECK(range_seekable(h, strlen(h), &size) == 1 && size == 1000);
    range_head(h, sizeof h, "application/zip", 1000, 0, 0, 999);
    CHECK(range_seekable(h, strlen(h), &size) == 1 && size == 1000);
    range_head(h, sizeof h, "application/zip", -1, 0, 0, -1);
    CHECK(range_seekable(h, strlen(h), &size) == 0 && size == -1);
    r = "HTTP/1.1 200 OK\r\nAccept-Ranges: none\r\nContent-Length: 5\r\n\r\n";
    CHECK(range_seekable(r, strlen(r), &size) == 0);
    r = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n";
    CHECK(range_seekable(r, strlen(r), &size) == 0);
    r = "HTTP/1.1 200 OK\r\naccept-ranges: x-pages, bytes\r\nContent-Length: 5\r\n\r\n";
    CHECK(range_seekable(r, strlen(r), &size) == 1 && size == 5);
    r = "HTTP/1.1 404 Not Found\r\nAccept-Ranges: bytes\r\nContent-Length: 5\r\n\r\n";
    CHECK(range_seekable(r, strlen(r), &size) == 0);
    r = "HTTP/1.1 206 Partial\r\nAccept-Ranges: bytes\r\nContent-Range: bytes 0-4/*\r\n\r\n";
    CHECK(range_seekable(r, strlen(r), &size) == 0);
}

/* Whether the connection is kept after the reply, every way the reply can arrive */
static void kept_is(const char *request, const char *reply, int want, int line, int modes)
{
    int mode;
    for (mode = 0; mode < modes; mode++) {
        run(reply, strlen(reply), mode, request);
        if (kept != want) {
            printf("FAIL %s:%d: mode %d: kept %d, wanted %d\n", __FILE__, line, mode, kept, want);
            failures++;
        }
    }
}
#define KEPT(req, rep, want) kept_is(req, rep, want, __LINE__, 3)
/* Bytes after the end are seen only if they came with it (all at once); otherwise
   they're left unread, and Mill never gives out a kept connection with bytes waiting */
#define KEPT_WHOLE(req, rep, want) kept_is(req, rep, want, __LINE__, 1)

static void test_keep(void)
{
    HttpHead h;
    /* 8.1.2.1: 1.1 stays open unless it says close; 1.0 closes */
    http_head("HTTP/1.1 200 OK\r\n\r\n", 19, &h);
    CHECK(!h.closes);
    http_head("HTTP/1.0 200 OK\r\n\r\n", 19, &h);
    CHECK(h.closes);
    http_head("HTTP/1.1 200 OK\r\nConnection: Keep-Alive, Close\r\n\r\n", 49, &h);
    CHECK(h.closes);
    http_head("HTTP/1.1 200 OK\r\nConnection: closed\r\n\r\n", 39, &h);
    CHECK(!h.closes);
    http_head("HTTP/2.0 200 OK\r\n\r\n", 19, &h);
    CHECK(!h.closes);
    /* the end from the length or the last chunk: kept */
    KEPT(GET, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", 1);
    KEPT(GET, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n", 1);
    KEPT(GET, "HTTP/1.1 304 Not Modified\r\n\r\n", 1);
    KEPT(GET, "HTTP/1.1 404 Not Found\r\nContent-Length: 4\r\n\r\ngone", 1);
    KEPT("GET / HTTP/1.0\r\nRange: bytes=0-1\r\n\r\n",
         "HTTP/1.1 206 Partial\r\nContent-Range: bytes 0-1/9\r\nContent-Length: 2\r\n\r\nhe", 1);
    /* no length, a close, 1.0, an early end, more after it, or a whole file to a Range: not */
    KEPT(GET, "HTTP/1.1 200 OK\r\n\r\nhello", 0);
    KEPT(GET, "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 5\r\n\r\nhello", 0);
    KEPT(GET, "HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nhello", 0);
    KEPT(GET, "HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\nhello", 0);
    KEPT_WHOLE(GET, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloEXTRA", 0);
    KEPT_WHOLE(GET, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\nX", 0);
    KEPT(GET, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n", 0);
    KEPT("GET / HTTP/1.0\r\nRange: bytes=0-1\r\n\r\n", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", 0);
}

/* The chunked decoder alone, on the edge cases of its bounds */
static long decode(const char *s, long len, char *to, int *ok)
{
    static char buf[20000];
    Chunked c;
    long n, used;
    memset(&c, 0, sizeof c);
    memcpy(buf, s, len);
    n = chunked_run(&c, buf, len, &used);
    *ok = n >= 0 && chunked_done(&c);
    if (n > 0) memcpy(to, buf, n);
    return n;
}

static void test_chunked(void)
{
    static char big[12000], got[12000];
    int ok;
    long n;
    CHECK(decode("0\r\n\r\n", 5, got, &ok) == 0 && ok);
    CHECK(decode("000\r\n\r\n", 7, got, &ok) == 0 && ok);
    CHECK(decode("3\r\nabc\r\n0\r\n\r\nNEXT", 17, got, &ok) == 3 && ok && !memcmp(got, "abc", 3));
    CHECK(decode("7FFFFFFF\r\n", 10, got, &ok) == 0 && !ok);           /* fine, just not all here */
    CHECK(decode("80000000\r\n", 10, got, &ok) < 0);                   /* 2^31 */
    CHECK(decode("000000000000000000001\r\nx\r\n0\r\n\r\n", 31, got, &ok) == 1 && ok);
    CHECK(decode("\r\n", 2, got, &ok) < 0);
    CHECK(decode(" 3\r\nabc\r\n", 9, got, &ok) < 0);
    CHECK(decode("3 ;a=b\r\nabc\r\n0 \r\n\r\n", 19, got, &ok) == 3 && ok);
    CHECK(decode("3 x\r\nabc", 8, got, &ok) < 0);
    CHECK(decode("3\rabc", 5, got, &ok) < 0);
    CHECK(decode("3\r\nabcX\r\n", 9, got, &ok) < 0);
    CHECK(decode("3\r\nabc\rX", 8, got, &ok) < 0);
    CHECK(decode("0\r\nA: b\r\n\rX", 11, got, &ok) < 0);
    /* an extension of 1K is too long; so are 4K of trailers */
    memset(big, 'a', sizeof big);
    memcpy(big, "1;", 2);
    CHECK(decode(big, 1030, got, &ok) < 0);
    memcpy(big, "1;", 2);
    memcpy(big + 1000, "\r\nx\r\n0\r\n\r\n", 10);
    CHECK(decode(big, 1010, got, &ok) == 1 && ok);
    memset(big, 'a', sizeof big);
    memcpy(big, "0\r\n", 3);
    CHECK(decode(big, 5000, got, &ok) < 0);
    for (n = 3; n + 6 < 3000; n += 6) memcpy(big + n, "X: y\r\n", 6);
    memcpy(big + n, "\r\n", 2);
    CHECK(decode(big, n + 2, got, &ok) == 0 && ok);
}

static void test_replies(void)
{
    static char big[100000], want[100000];
    long i, n, w;
    char range[100];

    /* Content-Length: what follows isn't this reply's */
    REPLY(GET, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nAccept-Ranges: bytes\r\n\r\nhelloEXTRA", R_OK,
          "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nAccept-Ranges: bytes\r\n\r\nhello");
    REPLY(GET, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\nEXTRA", R_OK, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    /* short of it: what came is passed on, cut off */
    REPLY(GET, "HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\nhello", R_CUT, "HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\nhello");
    REPLY(GET, "HTTP/1.1 404 Not Found\r\nContent-Length: 50\r\n\r\ngone", R_CUT, "HTTP/1.1 404 Not Found\r\nContent-Length: 50\r\n\r\ngone");
    REPLY(GET, "HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\n", R_CUT, "HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\n");
    /* the server's close */
    REPLY(GET, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nhello\r\n\r\nthere", R_OK,
          "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nhello\r\n\r\nthere");
    REPLY(GET, "HTTP/1.0 200 OK\n\nbare", R_OK, "HTTP/1.0 200 OK\n\nbare");
    /* chunked, with extensions and trailers; Content-Length is overruled and dropped */
    REPLY(GET, "HTTP/1.1 200 OK\r\nDate: x\r\nTransfer-Encoding: chunked\r\nContent-Length: 999\r\nServer: s\r\n\r\n"
               "5;name=\"v;al\"\r\nhello\r\n1a\r\nabcdefghijklmnopqrstuvwxyz\r\n0;last\r\nExpires: 0\r\nX-T: 1\r\n\r\nNEXT REPLY",
          R_OK, "HTTP/1.1 200 OK\r\nDate: x\r\nServer: s\r\n\r\nhelloabcdefghijklmnopqrstuvwxyz");
    REPLY(GET, "HTTP/1.1 200 OK\nTransfer-Encoding: chunked\n\n3\nabc\n0\n\n", R_OK, "HTTP/1.1 200 OK\n\nabc");
    /* ...and before the last chunk, or the trailers' end */
    REPLY(GET, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n", R_CUT, "HTTP/1.1 200 OK\r\n\r\nhello");
    REPLY(GET, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n3\r\nthe", R_CUT,
          "HTTP/1.1 200 OK\r\n\r\nhellothe");
    REPLY(GET, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel", R_CUT, "HTTP/1.1 200 OK\r\n\r\nhel");
    REPLY(GET, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n", R_CUT, "HTTP/1.1 200 OK\r\n\r\n");
    REPLY(GET, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nhello\r\n0\r\n\r\n", R_UNREADABLE, NULL);
    REPLY(GET, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFF\r\nhello", R_UNREADABLE, NULL);
    REPLY(GET, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhello\r\n0\r\n\r\n", R_UNREADABLE, NULL);
    REPLY(GET, "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n", R_UNREADABLE, NULL);
    /* a big chunked body, in chunks of every size */
    n = sprintf(big, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    w = sprintf(want, "HTTP/1.1 200 OK\r\n\r\n");
    for (i = 1; n < 25000; i = i * 3 + 1) {
        long k;
        n += sprintf(big + n, "%lX\r\n", i);
        for (k = 0; k < i; k++) big[n++] = want[w++] = 'a' + (i + k) % 26;
        n += sprintf(big + n, "\r\n");
    }
    want[w] = 0;
    n += sprintf(big + n, "0\r\n\r\n");
    reply_is(GET, big, n, R_OK, want, __LINE__);
    /* 1xx before the reply; no body after 204, 304, or HEAD */
    REPLY(GET, "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 100 Continue\r\nX: y\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi",
          R_OK, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi");
    REPLY(GET, "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 100 Continue\r\n\r\n"
               "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\n\r\n", R_UNREADABLE, NULL);
    REPLY(GET, "HTTP/1.1 100 Continue\r\n\r\n", R_UNREADABLE, NULL);
    REPLY(GET, "HTTP/1.1 204 No Content\r\nContent-Length: 5\r\n\r\nhello", R_OK, "HTTP/1.1 204 No Content\r\nContent-Length: 5\r\n\r\n");
    REPLY(GET, "HTTP/1.1 304 Not Modified\r\nTransfer-Encoding: chunked\r\n\r\n", R_OK, "HTTP/1.1 304 Not Modified\r\n\r\n");
    REPLY("HEAD / HTTP/1.0\r\n\r\n", "HTTP/1.1 200 OK\r\nContent-Length: 5000\r\n\r\n", R_OK, "HTTP/1.1 200 OK\r\nContent-Length: 5000\r\n\r\n");
    /* malformed */
    REPLY(GET, "", R_NOTHING, NULL);
    REPLY(GET, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n", R_UNREADABLE, NULL);
    REPLY(GET, "<HTML>an HTTP/0.9 page</HTML>", R_UNREADABLE, NULL);
    REPLY(GET, "HTTP/1.1 OK\r\n\r\n", R_UNREADABLE, NULL);
    REPLY(GET, "HTTP/1.1 200 OK\r\nContent-Length: 5x\r\n\r\nhello", R_UNREADABLE, NULL);
    n = sprintf(big, "HTTP/1.1 200 OK\r\n");
    while (n < 5000) n += sprintf(big + n, "Set-Cookie: a=b\r\n");
    n += sprintf(big + n, "\r\n");
    reply_is(GET, big, n, R_TOO_MANY, NULL, __LINE__);

    /* ranges: the part asked for */
    sprintf(range, "GET /f HTTP/1.0\r\nHost: h\r\nRange: bytes=100-104\r\n\r\n");
    REPLY(range, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 100-104/5000\r\nContent-Length: 5\r\n\r\nhello", R_OK,
          "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 100-104/5000\r\nContent-Length: 5\r\n\r\nhello");
    REPLY(range, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 100-104/*\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
          R_OK, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 100-104/*\r\n\r\nhello");
    /* ...all of it: a part that ends early is an error, not cut off */
    REPLY(range, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 100-104/5000\r\nContent-Length: 5\r\n\r\nhel", R_EARLY, NULL);
    REPLY(range, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 100-104/5000\r\nTransfer-Encoding: chunked\r\n\r\n"
                 "5\r\nhello\r\n", R_EARLY, NULL);
    /* ...shorter only where the file ends */
    REPLY(range, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 100-102/103\r\nContent-Length: 3\r\n\r\nhel", R_OK, NULL);
    REPLY(range, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 100-102/5000\r\nContent-Length: 3\r\n\r\nhel", R_WRONG_PART, NULL);
    REPLY(range, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 100-102/*\r\n\r\nhel", R_WRONG_PART, NULL);
    /* ...not some other part */
    REPLY(range, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-4/5000\r\n\r\nhello", R_WRONG_PART, NULL);
    REPLY(range, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 100-105/5000\r\n\r\nhello!", R_WRONG_PART, NULL);
    REPLY(range, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 100-104/5000\r\nContent-Length: 6\r\n\r\nhello!", R_WRONG_PART, NULL);
    REPLY(range, "HTTP/1.1 206 Partial Content\r\nContent-Type: multipart/byteranges; boundary=X\r\n\r\n--X\r\n", R_WRONG_PART, NULL);
    REPLY(range, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 100-104/5000\r\nContent-Range: bytes 100-104/5000\r\n\r\nhello",
          R_WRONG_PART, NULL);
    REPLY(GET, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 100-104/5000\r\n\r\nhello", R_WRONG_PART, NULL);
    /* ...and an open end */
    REPLY("GET /f HTTP/1.0\r\nRange: bytes=4990-\r\n\r\n",
          "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 4990-4999/5000\r\n\r\n0123456789", R_OK, NULL);
    REPLY("GET /f HTTP/1.0\r\nRange: bytes=4990-\r\n\r\n",
          "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 4990-4995/5000\r\n\r\n012345", R_WRONG_PART, NULL);
    /* the whole thing instead: the head says so, and the body isn't read */
    REPLY(range, "HTTP/1.1 200 OK\r\nContent-Length: 5000\r\n\r\nthe whole file...", R_OK, "HTTP/1.1 200 OK\r\nContent-Length: 5000\r\n\r\n");
    REPLY(range, "HTTP/1.0 200 OK\r\n\r\nthe whole file...", R_OK, "HTTP/1.0 200 OK\r\n\r\n");
    REPLY(range, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nwhole\r\n0\r\n\r\n", R_OK, "HTTP/1.1 200 OK\r\n\r\n");
    /* 416, and other answers, as they are */
    REPLY(range, "HTTP/1.1 416 Requested range not satisfiable\r\nContent-Range: bytes */50\r\nContent-Length: 4\r\n\r\nnope", R_OK,
          "HTTP/1.1 416 Requested range not satisfiable\r\nContent-Range: bytes */50\r\nContent-Length: 4\r\n\r\nnope");
    REPLY(range, "HTTP/1.1 404 Not Found\r\nContent-Length: 4\r\n\r\ngone", R_OK, "HTTP/1.1 404 Not Found\r\nContent-Length: 4\r\n\r\ngone");
    REPLY(range, "HTTP/1.1 302 Found\r\nLocation: /g\r\nContent-Length: 0\r\n\r\n", R_OK, NULL);
}

/* Random bytes, and random damage to a good reply: never a crash, a hang, or more out
   than went in */
static void test_fuzz(void)
{
    static char b[3000];
    static const char junk[] = "HTTP/1. 2006\r\n:abcdef0123;-Content-Range:Transfer-Encoding: chunked";
    const char *good = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-9/10\r\nTransfer-Encoding: chunked\r\n\r\n"
                       "4;x=y\r\n0123\r\n6\r\n456789\r\n0\r\nT: 1\r\n\r\n";
    int t;
    for (t = 0; t < 20000; t++) {
        long n = strlen(good), k;
        memcpy(b, good, n);
        if (t % 2) {
            for (k = 0; k < 1 + t % 4; k++) b[rand() % n] = "0123456789aF\r\n;: -/x"[rand() % 21];
        } else {
            n = rand() % sizeof b;
            for (k = 0; k < n; k++) b[k] = junk[rand() % (sizeof junk - 1)];
        }
        run(b, n, t % 3, "GET / HTTP/1.0\r\nRange: bytes=0-9\r\n\r\n");
        CHECK(out_len <= n);
    }
}

int main(void)
{
    srand(1997);
    test_head();
    test_drop();
    test_range_asked();
    test_range_part();
    test_keep();
    test_chunked();
    test_replies();
    test_fuzz();
    if (failures) { printf("%d http check(s) failed\n", failures); return 1; }
    printf("http: all passed\n");
    return 0;
}
