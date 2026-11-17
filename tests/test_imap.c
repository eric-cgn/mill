/* Host tests for the IMAP Plug-In's helpers (plugins/imap/imaputil.c): tokens, with quoted
   strings and literals (RFC 2060 4), a FETCH with ENVELOPE (7.4.2), LIST (7.2.2),
   BODYSTRUCTURE's parts and their section numbers (7.4.2, 6.4.5), imap: addresses
   (RFC 2192 3), and quoting. */
#include <stdio.h>
#include <string.h>

#include "../plugins/imap/imaputil.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_tokens(void)
{
    const char *s = "* 1 FETCH (BODY[1.2] {5}\r\nab)c\"d \"q\\\"x\" NIL)";
    ImapIn in;
    ImapTok t;
    char v[16];
    in.p = s;
    in.end = s + strlen(s);
    CHECK(imap_tok(&in, &t) == kImapAtom && t.n == 1);
    CHECK(imap_tok(&in, &t) == kImapAtom && t.n == 1 && *t.s == '1');
    CHECK(imap_tok(&in, &t) == kImapAtom && t.n == 5);
    CHECK(imap_tok(&in, &t) == kImapOpen);
    CHECK(imap_tok(&in, &t) == kImapAtom && t.n == 9 && !memcmp(t.s, "BODY[1.2]", 9));
    CHECK(imap_tok(&in, &t) == kImapString && imap_str(&t, v, sizeof v) == 5 && !strcmp(v, "ab)c\""));
    CHECK(imap_tok(&in, &t) == kImapAtom && t.n == 1 && *t.s == 'd');
    CHECK(imap_tok(&in, &t) == kImapString && t.quoted && imap_str(&t, v, sizeof v) == 3 && !strcmp(v, "q\"x"));
    CHECK(imap_tok(&in, &t) == kImapNil && imap_str(&t, v, sizeof v) == 0 && !v[0]);
    CHECK(imap_tok(&in, &t) == kImapClose && imap_tok(&in, &t) == kImapEnd);
    CHECK(imap_literal("* 1 FETCH (BODY[1] {123}", 24) == 123);
    CHECK(imap_literal("a OK done", 9) == -1 && imap_literal("{}", 2) == -1 && imap_literal("x {1x}", 6) == -1);
}

static void test_fetch(void)
{
    const char *s = "* 12 FETCH (UID 20 FLAGS (\\Answered \\Seen) RFC822.SIZE 1234 ENVELOPE "
                    "(\"Mon, 3 Nov 1997 09:07:00 -0800\" {11}\r\nA \"subject\" ((NIL NIL \"eric\" \"1in1e6.com\") "
                    "(\"Second\" NIL \"x\" \"y\")) NIL NIL ((\"Tester\" NIL \"tester\" \"1in1e6.com\")) NIL NIL NIL \"<1@x>\") X-ITEM (1 2))";
    ImapMsg m;
    CHECK(imap_fetch(s, strlen(s), &m));
    CHECK(m.seq == 12 && m.uid == 20 && m.size == 1234 && m.seen);
    CHECK(!strcmp(m.date, "Mon, 3 Nov 1997 09:07:00 -0800") && !strcmp(m.subject, "A \"subject\"") && !strcmp(m.from, "eric@1in1e6.com"));
    s = "* 3 FETCH (FLAGS () ENVELOPE (NIL NIL NIL NIL NIL NIL NIL NIL NIL NIL) UID 7)";
    CHECK(imap_fetch(s, strlen(s), &m) && m.uid == 7 && !m.seen && !m.subject[0] && !m.from[0]);
    s = "* 3 FETCH (UID 8 ENVELOPE (\"d\" \"s\" ((\"Name\" NIL \"a\" \"b\")) NIL NIL NIL NIL NIL NIL NIL))";
    CHECK(imap_fetch(s, strlen(s), &m) && !strcmp(m.from, "Name"));
    CHECK(!imap_fetch("* 3 EXISTS", 10, &m));
    CHECK(!imap_fetch("a1 OK FETCH done", 16, &m));
}

static void test_list(void)
{
    char name[32];
    short no;
    const char *s = "* LIST (\\HasNoChildren) \".\" Lists.Mill";
    CHECK(imap_list(s, strlen(s), name, sizeof name, &no) && !strcmp(name, "Lists.Mill") && !no);
    s = "* LIST (\\Noselect \\HasChildren) \"/\" \"Old Mail\"";
    CHECK(imap_list(s, strlen(s), name, sizeof name, &no) && !strcmp(name, "Old Mail") && no);
    s = "* LIST () NIL {5}\r\nINBOX";
    CHECK(imap_list(s, strlen(s), name, sizeof name, &no) && !strcmp(name, "INBOX"));
    CHECK(!imap_list("* LSUB () \".\" x", 16, name, sizeof name, &no));
}

static void test_parts(void)
{
    ImapPart p[8];
    /* one text part: section 1 */
    const char *s = "* 1 FETCH (UID 5 BODYSTRUCTURE (\"TEXT\" \"PLAIN\" (\"CHARSET\" \"ISO-8859-1\") NIL NIL \"QUOTED-PRINTABLE\" 250 6 NIL NIL NIL))";
    CHECK(imap_parts(s, strlen(s), p, 8) == 1);
    CHECK(!strcmp(p[0].section, "1") && !strcmp(p[0].type, "text/plain") && !strcmp(p[0].charset, "ISO-8859-1") &&
          !strcmp(p[0].encoding, "quoted-printable") && p[0].size == 250);
    /* mixed: an alternative (1.1, 1.2), a file (2), and a message/rfc822 (3) */
    s = "* 1 FETCH (BODYSTRUCTURE (((\"text\" \"plain\" (\"charset\" \"us-ascii\") NIL NIL \"7bit\" 10 1)"
        "(\"text\" \"html\" NIL NIL NIL \"7bit\" 20 1) \"alternative\" (\"boundary\" \"i\"))"
        "(\"application\" \"octet-stream\" (\"name\" \"notes.bin\") NIL NIL \"base64\" 1400 NIL (\"attachment\" NIL) NIL)"
        "(\"message\" \"rfc822\" NIL NIL NIL \"7bit\" 300 (NIL \"s\" NIL NIL NIL NIL NIL NIL NIL NIL) "
        "(\"text\" \"plain\" NIL NIL NIL \"7bit\" 5 1) 9) \"mixed\" (\"boundary\" \"o\") NIL NIL)))";
    CHECK(imap_parts(s, strlen(s), p, 8) == 4);
    CHECK(!strcmp(p[0].section, "1.1") && !strcmp(p[0].type, "text/plain") && !strcmp(p[0].charset, "us-ascii"));
    CHECK(!strcmp(p[1].section, "1.2") && !strcmp(p[1].type, "text/html"));
    CHECK(!strcmp(p[2].section, "2") && !strcmp(p[2].name, "notes.bin") && !strcmp(p[2].encoding, "base64") && p[2].size == 1400);
    CHECK(!strcmp(p[3].section, "3") && !strcmp(p[3].type, "message/rfc822") && p[3].size == 300);
    /* more parts than room: counted, not kept */
    CHECK(imap_parts(s, strlen(s), p, 2) == 4 && !strcmp(p[1].section, "1.2"));
    CHECK(imap_parts("* 1 FETCH (UID 2)", 17, p, 8) == 0);
}

static void test_url(void)
{
    ImapUrl u;
    char q[32];
    imap_url("/INBOX;UIDVALIDITY=880000000/;UID=20", &u);
    CHECK(!strcmp(u.mailbox, "INBOX") && u.uidvalidity == 880000000UL && u.uid == 20 && !u.search[0]);
    imap_url("/Lists.Mill/;UID=2/;SECTION=1", &u);
    CHECK(!strcmp(u.mailbox, "Lists.Mill") && u.uid == 2 && !u.uidvalidity);
    imap_url("/Old%20Mail/Sent", &u);
    CHECK(!strcmp(u.mailbox, "Old Mail/Sent") && !u.uid);
    imap_url("/INBOX?FROM%20eric", &u);
    CHECK(!strcmp(u.mailbox, "INBOX") && !strcmp(u.search, "FROM eric"));
    imap_url("/INBOX;X-BEFORE=21", &u);
    CHECK(!strcmp(u.mailbox, "INBOX") && u.before == 21);
    imap_url("/INBOX/", &u);
    CHECK(!strcmp(u.mailbox, "INBOX"));
    imap_url("/", &u);
    CHECK(!u.mailbox[0]);
    CHECK(imap_quote("a\"b\\c", q, sizeof q) == 9 && !strcmp(q, "\"a\\\"b\\\\c\""));
    CHECK(imap_quote("a\rb", q, sizeof q) == -1 && imap_quote("abcdef", q, 6) == -1);
}

int main(void)
{
    test_tokens();
    test_fetch();
    test_list();
    test_parts();
    test_url();
    if (failures) { printf("%d imap check(s) failed\n", failures); return 1; }
    printf("imap: all passed\n");
    return 0;
}
