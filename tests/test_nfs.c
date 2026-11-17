/* Host tests for the NFS Plug-In's helpers (plugins/nfs/nfsutil.c): an RPC call with
   AUTH_UNIX credentials, byte for byte; replies accepted, refused, unavailable, and not
   ours; fattr3; READDIRPLUS and READDIR entries; the export list; and short input. The
   expected bytes are worked out from RFC 1831, 1832, and 1813. */
#include <stdio.h>
#include <string.h>

#include "../plugins/nfs/nfsutil.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int hex_is(const unsigned char *b, long n, const char *hex)
{
    char s[600];
    long i;
    for (i = 0; i < n; i++) snprintf(s + 2 * i, 3, "%02x", b[i]);
    s[2 * n] = 0;
    if (strcmp(s, hex)) { printf("  got %s\n  not %s\n", s, hex); return 0; }
    return 1;
}

/* XDR built a word at a time */
static unsigned char msg[1024];
static long at;
static void w(unsigned long v) { nfs_put32(msg + at, v); at += 4; }
static void o(const char *s, long n) { at = nfs_put_opaque(msg + at, s, n) - msg; }
static void fattr(unsigned long type, unsigned long hi, unsigned long lo, unsigned long mtime)
{
    long i;
    w(type);
    for (i = 0; i < 4; i++) w(i + 1);   /* mode, nlink, uid, gid */
    w(hi); w(lo);                       /* size */
    for (i = 0; i < 10; i++) w(0xEEEEEEEEUL);   /* used, rdev, fsid, fileid, atime */
    w(mtime); w(0xEEEEEEEEUL);          /* mtime */
    w(0xEEEEEEEEUL); w(0xEEEEEEEEUL);   /* ctime */
}
/* an accepted reply's header, SUCCESS */
static void accepted(unsigned long xid) { at = 0; w(xid); w(1); w(0); w(0); w(0); w(0); }

static void test_put(void)
{
    unsigned char b[16];
    memset(b, 0xAA, sizeof b);
    CHECK(nfs_put32(b, 0x12345678UL) == b + 4 && hex_is(b, 4, "12345678"));
    CHECK(nfs_put_opaque(b, "abcde", 5) == b + 12);
    CHECK(hex_is(b, 12, "000000056162636465000000"));
    CHECK(nfs_put_opaque(b, "abcd", 4) == b + 8);
}

static void test_call(void)
{
    /* MOUNTPROC3_EXPORT (RFC 1813 5.2.5), as a server took it */
    unsigned char b[128];
    unsigned char *end = nfs_call(b, 0x4D494C4FUL, kMountProg, 3, 5, "Macintosh", 65534, 65534);
    CHECK(end - b == 72);
    CHECK(hex_is(b, end - b, "4d494c4f0000000000000002000186a50000000300000005"
                             "000000010000002000000000000000094d6163696e746f73680000000000fffe0000fffe00000000"
                             "0000000000000000"));
}

static void test_reply(void)
{
    NfsIn in;
    accepted(7); w(42);
    CHECK(nfs_reply(msg, at, 7, &in) == 0 && nfs_u32(&in) == 42 && !in.bad);
    CHECK(nfs_reply(msg, at, 8, &in) == kRpcGarbled);           /* not ours */
    CHECK(nfs_reply(msg, 20, 7, &in) == kRpcGarbled);           /* cut short */
    at = 0; w(7); w(1); w(1); w(1); w(5);                        /* MSG_DENIED, AUTH_ERROR, AUTH_TOOWEAK */
    CHECK(nfs_reply(msg, at, 7, &in) == kRpcRefused);
    at = 0; w(7); w(1); w(0); w(1); o("vrfy", 4); w(2); w(3); w(3);    /* a verifier, then PROG_MISMATCH */
    CHECK(nfs_reply(msg, at, 7, &in) == kRpcUnavailable);
    at = 0; w(7); w(0);                                          /* a CALL */
    CHECK(nfs_reply(msg, at, 7, &in) == kRpcGarbled);
}

static void test_attr(void)
{
    NfsIn in;
    NfsAttr a;
    NfsFh fh;
    accepted(1); w(1); fattr(kNf3Reg, 0, 307200, 879508800UL);  /* 1997-11-14 12:00 UTC */
    CHECK(nfs_reply(msg, at, 1, &in) == 0 && nfs_post_op(&in, &a) && !in.bad && in.p == in.end);
    CHECK(a.type == kNf3Reg && a.size == 307200 && a.mtime == 879508800UL + 2082844800UL);
    accepted(1); fattr(kNf3Dir, 1, 5, 0);                       /* 4 GB and more */
    nfs_reply(msg, at, 1, &in);
    nfs_fattr(&in, &a);
    CHECK(!in.bad && a.type == kNf3Dir && a.size == 0xFFFFFFFFUL);
    accepted(1); w(0);
    nfs_reply(msg, at, 1, &in);
    CHECK(!nfs_post_op(&in, &a) && a.type == 0 && !in.bad);
    accepted(1); o("0123456789abcdefghijklmnopq", 27);
    nfs_reply(msg, at, 1, &in);
    nfs_fh(&in, &fh);
    CHECK(!in.bad && fh.len == 27 && !memcmp(fh.data, "0123456789abcdefghijklmnopq", 27));
    accepted(1); w(65); while (at < 96) w(0);                  /* too long a handle */
    nfs_reply(msg, at, 1, &in);
    nfs_fh(&in, &fh);
    CHECK(in.bad && fh.len == 0);
    accepted(1); w(1); w(kNf3Reg);                              /* attributes cut short */
    nfs_reply(msg, at, 1, &in);
    CHECK(!nfs_post_op(&in, &a) && in.bad);
}

static void test_entries(void)
{
    NfsIn in;
    NfsEntry e;
    /* READDIRPLUS: one with attributes and a handle, one without either, then the end */
    accepted(2);
    w(1); w(0); w(11); o("Read Me.txt", 11); w(0); w(5);
    w(1); fattr(kNf3Reg, 0, 316, 1);
    w(1); o("handle", 6);
    w(1); w(0); w(12); o("Projects", 8); w(0xFFFFFFFFUL); w(6); w(0); w(0);
    w(0); w(1);                                                 /* no more; eof */
    nfs_reply(msg, at, 2, &in);
    CHECK(nfs_entry(&in, 1, &e) && e.len == 11 && !memcmp(e.name, "Read Me.txt", 11) &&
          hex_is(e.cookie, 8, "0000000000000005") && e.attrs && e.a.size == 316);
    CHECK(nfs_entry(&in, 1, &e) && e.len == 8 && !memcmp(e.name, "Projects", 8) &&
          hex_is(e.cookie, 8, "ffffffff00000006") && !e.attrs && e.a.type == 0);
    CHECK(!nfs_entry(&in, 1, &e) && nfs_u32(&in) == 1 && !in.bad && in.p == in.end);
    /* READDIR: names and cookies only */
    accepted(3);
    w(1); w(0); w(9); o("a.gif", 5); w(0); w(1);
    w(1); w(0); w(10); o("b", 1); w(0); w(2);
    w(0); w(0);
    nfs_reply(msg, at, 3, &in);
    CHECK(nfs_entry(&in, 0, &e) && e.len == 5 && !e.attrs && hex_is(e.cookie, 8, "0000000000000001"));
    CHECK(nfs_entry(&in, 0, &e) && e.len == 1 && e.name[0] == 'b');
    CHECK(!nfs_entry(&in, 0, &e) && nfs_u32(&in) == 0 && !in.bad);
    /* cut off in the middle of a name */
    accepted(4); w(1); w(0); w(9); w(40); w(0x41414141UL);
    nfs_reply(msg, at, 4, &in);
    CHECK(!nfs_entry(&in, 1, &e) && in.bad);
}

static void test_exports(void)
{
    NfsIn in;
    const unsigned char *dir;
    long n;
    /* /export/public to everyone; /export/private to two groups */
    accepted(5);
    w(1); o("/export/public", 14); w(0);
    w(1); o("/export/private", 15); w(1); o("staff", 5); w(1); o("192.0.2.0/24", 12); w(0);
    w(0);
    nfs_reply(msg, at, 5, &in);
    CHECK(nfs_export(&in, &dir, &n) && n == 14 && !memcmp(dir, "/export/public", 14));
    CHECK(nfs_export(&in, &dir, &n) && n == 15 && !memcmp(dir, "/export/private", 15));
    CHECK(!nfs_export(&in, &dir, &n) && !in.bad && in.p == in.end);
    /* none */
    accepted(6); w(0);
    nfs_reply(msg, at, 6, &in);
    CHECK(!nfs_export(&in, &dir, &n) && !in.bad);
}

int main(void)
{
    test_put();
    test_call();
    test_reply();
    test_attr();
    test_entries();
    test_exports();
    if (failures) { printf("%d nfs check(s) failed\n", failures); return 1; }
    printf("nfs: all passed\n");
    return 0;
}
