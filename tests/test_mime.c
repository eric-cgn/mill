/* Host tests for plugins/mime/mime.c, shared by the News and IMAP Plug-Ins: header fields
   with continuation lines (RFC 822 3.1.1), parameters (RFC 2045 5.1), encoded words in Q
   and B (RFC 1522), quoted-printable (RFC 2045 6.7), base64 in pieces (6.8), names, and
   dates. The expected values are worked out from the RFCs. */
#include <stdio.h>
#include <string.h>

#include "../plugins/mime/mime.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_header(void)
{
    const char *h = "From: Eric <eric@1in1e6.com>\r\nSubject: one\r\n two\r\n\tthree\r\nX-Empty:\r\ncontent-type:  text/plain ;\n charset=\"ISO-8859-1\"\n";
    char v[64];
    CHECK(mime_header(h, strlen(h), "from", v, sizeof v) == 22 && !strcmp(v, "Eric <eric@1in1e6.com>"));
    CHECK(mime_header(h, strlen(h), "Subject", v, sizeof v) > 0 && !strcmp(v, "one two three"));
    CHECK(mime_header(h, strlen(h), "X-Empty", v, sizeof v) == 0 && !v[0]);
    CHECK(mime_header(h, strlen(h), "Date", v, sizeof v) == -1);
    CHECK(mime_header(h, strlen(h), "Content-Type", v, sizeof v) > 0 && !strcmp(v, "text/plain ; charset=\"ISO-8859-1\""));
    CHECK(mime_header(h, strlen(h), "Subject", v, 5) == 3 && !strcmp(v, "one"));
    /* a name that's only the start of another isn't it */
    CHECK(mime_header("Subjects: no\n", 13, "Subject", v, sizeof v) == -1);
}

static void test_param(void)
{
    char v[64];
    CHECK(mime_param("text/plain; charset=\"ISO-8859-1\"", "charset", v, sizeof v) == 10 && !strcmp(v, "ISO-8859-1"));
    CHECK(mime_param("multipart/mixed; boundary=\"=_a b\"; x=y", "BOUNDARY", v, sizeof v) > 0 && !strcmp(v, "=_a b"));
    CHECK(mime_param("text/plain;format=flowed;charset=us-ascii", "charset", v, sizeof v) > 0 && !strcmp(v, "us-ascii"));
    CHECK(mime_param("text/plain", "charset", v, sizeof v) == -1);
    CHECK(mime_param("a/b; name=\"q\\\"x\"", "name", v, sizeof v) > 0 && !strcmp(v, "q\"x"));
    mime_type(" Text/Plain; charset=x", v, sizeof v);
    CHECK(!strcmp(v, "text/plain"));
    CHECK(mime_charset("ISO-8859-1") == kMimeLatin1 && mime_charset("us-ascii") == kMimeLatin1);
    CHECK(mime_charset("UTF-8") == kMimeUTF8 && mime_charset("koi8-r") == kMimeGuess);
}

static void test_words(void)
{
    char v[80];
    CHECK(mime_words("=?ISO-8859-1?Q?Caf=E9_cr=E8me?=", v, sizeof v) == kMimeLatin1 && !strcmp(v, "Caf\xe9 cr\xe8me"));
    /* RFC 1522 8's examples: space between words goes, next to text it stays */
    mime_words("=?ISO-8859-1?Q?a?= =?ISO-8859-1?Q?b?=", v, sizeof v);
    CHECK(!strcmp(v, "ab"));
    mime_words("=?ISO-8859-1?Q?a?= b", v, sizeof v);
    CHECK(!strcmp(v, "a b"));
    mime_words("(=?ISO-8859-1?Q?a?=  \t  =?ISO-8859-2?Q?_b?=)", v, sizeof v);
    CHECK(!strcmp(v, "(a b)"));
    CHECK(mime_words("=?UTF-8?B?Q2Fmw6k=?=", v, sizeof v) == kMimeUTF8 && !strcmp(v, "Caf\xc3\xa9"));
    CHECK(mime_words("plain text", v, sizeof v) == kMimeGuess && !strcmp(v, "plain text"));
    /* not words: left as they are */
    mime_words("=?bad =?x?Z?y?= =?x?Q?", v, sizeof v);
    CHECK(!strcmp(v, "=?bad =?x?Z?y?= =?x?Q?"));
    mime_words("=?ISO-8859-1?Q?long_one?=", v, 5);
    CHECK(!strcmp(v, "long"));
}

static void test_qp(void)
{
    char l[80];
    short soft;
    long n;
    strcpy(l, "caf=E9 =3D ok   ");
    n = mime_qp(l, strlen(l), &soft);
    CHECK(n == 9 && !memcmp(l, "caf\xe9 = ok", 9) && !soft);
    strcpy(l, "goes on=");
    n = mime_qp(l, strlen(l), &soft);
    CHECK(n == 7 && !memcmp(l, "goes on", 7) && soft);
    strcpy(l, "bad =ZZ and =4");
    n = mime_qp(l, strlen(l), &soft);
    CHECK(n == 14 && !memcmp(l, "bad =ZZ and =4", 14) && !soft);
}

static void test_b64(void)
{
    MimeB64 s;
    unsigned char o[32];
    long n;
    memset(&s, 0, sizeof s);
    n = mime_b64(&s, "SGVsbG8s", 8, o);
    CHECK(n == 6 && !memcmp(o, "Hello,", 6));
    /* a group split over two pieces, line ends skipped, and the padded end */
    n = mime_b64(&s, "IHdv\r\ncmx", 9, o);
    CHECK(n == 3 && !memcmp(o, " wo", 3));
    n = mime_b64(&s, "kIQ==", 5, o);
    CHECK(n == 4 && !memcmp(o, "rld!", 4) && s.done);
    CHECK(mime_b64(&s, "QUJD", 4, o) == 0);     /* nothing after the end */
    memset(&s, 0, sizeof s);
    n = mime_b64(&s, "YQ=", 3, o);
    CHECK(n == 1 && o[0] == 'a');
}

static void test_who_day(void)
{
    char v[64];
    mime_who("Eric <eric@1in1e6.com>", v, sizeof v);
    CHECK(!strcmp(v, "Eric"));
    mime_who("\"Pat \\\"P\\\" X\" <pat@x>", v, sizeof v);
    CHECK(!strcmp(v, "Pat \"P\" X"));
    mime_who("eric@1in1e6.com (Eric W)", v, sizeof v);
    CHECK(!strcmp(v, "Eric W"));
    mime_who("<eric@1in1e6.com>", v, sizeof v);
    CHECK(!strcmp(v, "eric@1in1e6.com"));
    mime_who("eric@1in1e6.com", v, sizeof v);
    CHECK(!strcmp(v, "eric@1in1e6.com"));
    mime_day("Fri, 14 Nov 1997 12:00:00 -0800", v, sizeof v);
    CHECK(!strcmp(v, "14 Nov 1997"));
    mime_day("14 Nov 97 12:00 GMT", v, sizeof v);
    CHECK(!strcmp(v, "14 Nov 97"));
    mime_day("yesterday", v, sizeof v);
    CHECK(!strcmp(v, "yesterday"));
}

int main(void)
{
    test_header();
    test_param();
    test_words();
    test_qp();
    test_b64();
    test_who_day();
    if (failures) { printf("%d mime check(s) failed\n", failures); return 1; }
    printf("mime: all passed\n");
    return 0;
}
