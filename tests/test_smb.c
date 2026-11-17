/* Host tests for the SMB Plug-In's helpers (plugins/smb/smbutil.c): MD4 against RFC
   1320, DES against the widely published DES textbook example, the LM and NT hashes and responses against
   MS-NLMP's NTLMv1 example (4.2.2), and reading listings and share lists. */
#include <stdio.h>
#include <string.h>

#include "../plugins/smb/smbutil.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int hex_is(const unsigned char *b, long n, const char *hex)
{
    char s[200];
    long i;
    for (i = 0; i < n; i++) snprintf(s + 2 * i, 3, "%02x", b[i]);
    if (strcmp(s, hex)) { printf("  got %s\n  not %s\n", s, hex); return 0; }
    return 1;
}

/* RFC 1320 */
static void test_md4(void)
{
    unsigned char d[16];
    const char *long80 = "12345678901234567890123456789012345678901234567890123456789012345678901234567890";
    smb_md4((const unsigned char *)"", 0, d);
    CHECK(hex_is(d, 16, "31d6cfe0d16ae931b73c59d7e0c089c0"));
    smb_md4((const unsigned char *)"abc", 3, d);
    CHECK(hex_is(d, 16, "a448017aaf21d8525fc10ae87aa6729d"));
    smb_md4((const unsigned char *)"message digest", 14, d);
    CHECK(hex_is(d, 16, "d9130a8164549fe818874806e1c7014b"));
    smb_md4((const unsigned char *)long80, 80, d);
    CHECK(hex_is(d, 16, "e33b4ddc9c38f2199c3e7b164fcc0536"));
}

/* FIPS 46 */
static void test_des(void)
{
    /* key 133457799BBCDFF1 without its parity bits */
    static const unsigned char key[7] = { 0x12, 0x69, 0x5b, 0xc9, 0xb7, 0xb7, 0xf8 };
    static const unsigned char in[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    unsigned char out[8];
    smb_des7(key, in, out);
    CHECK(hex_is(out, 8, "85e813540f0ab405"));
}

/* MS-NLMP 4.2.2 */
static void test_ntlm(void)
{
    static const unsigned char challenge[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    unsigned char h[16], r[24];
    smb_lm_hash("Password", h);
    CHECK(hex_is(h, 16, "e52cac67419a9a224a3b108f3fa6cb6d"));
    smb_response(h, challenge, r);
    CHECK(hex_is(r, 24, "98def7b87f88aa5dafe2df779688a172def11c7d5ccdef13"));
    smb_nt_hash("Password", h);
    CHECK(hex_is(h, 16, "a4f49c406510bdcab6824ee7c30fd852"));
    smb_response(h, challenge, r);
    CHECK(hex_is(r, 24, "67c43011f30298a2ad35ece64f16331c44bdbed927841f94"));
    /* the empty password's NT hash is MD4 of nothing */
    smb_nt_hash("", h);
    CHECK(hex_is(h, 16, "31d6cfe0d16ae931b73c59d7e0c089c0"));
}

static void test_names_times(void)
{
    unsigned char n[34];
    short t[5];
    static const unsigned char ft[8] = { 0x00, 0xa0, 0x40, 0xd5, 0xf4, 0xf0, 0xbc, 0x01 };    /* 1997-11-14 12:00 */
    smb_nb_name(n, "smb", 0x20);
    CHECK(n[0] == 32 && n[33] == 0 && !memcmp(n + 1, "FDENECCACACACACACACACACACACACACA", 32));
    smb_nb_name(n, "*SMBSERVER", 0x20);
    CHECK(!memcmp(n + 1, "CKFDENECFDEFFCFGEFFCCACACACACACA", 32));
    CHECK(smb_nt_time(ft) == 2962353600UL);
    smb_dos_time((17 << 9) | (11 << 5) | 14, (12 << 11) | (30 << 5), t);
    CHECK(t[0] == 1997 && t[1] == 11 && t[2] == 14 && t[3] == 12 && t[4] == 30);
    CHECK(smb_from_oem('A') == 'A' && smb_from_oem(0x82) == 0x8E);  /* e acute */
}

static void test_entries(void)
{
    unsigned char b[200];
    const unsigned char *p;
    SmbEntry e;
    char name[13], comment[40];
    unsigned short type;
    /* two FILE_DIRECTORY_INFO entries: a folder "Pictures", then a 335-byte "Read Me.txt" */
    memset(b, 0, sizeof b);
    smb_put32(b, 72);
    b[56] = 0x10;
    smb_put32(b + 60, 8);
    memcpy(b + 64, "Pictures", 8);
    smb_put32(b + 72 + 40, 335);
    memcpy(b + 72 + 24, "\x00\xa0\x40\xd5\xf4\xf0\xbc\x01", 8);
    smb_put32(b + 72 + 60, 11);
    memcpy(b + 72 + 64, "Read Me.txt", 11);
    p = smb_find_entry(b, b + 147, kSmbFileDirectoryInfo, &e);
    CHECK(p == b + 72 && e.folder && e.len == 8 && !memcmp(e.name, "Pictures", 8));
    p = smb_find_entry(p, b + 147, kSmbFileDirectoryInfo, &e);
    CHECK(p == b + 147 && !e.folder && e.size == 335 && e.len == 11 && e.when == 2962353600UL);
    CHECK(smb_find_entry(b, b + 40, kSmbFileDirectoryInfo, &e) == 0);
    /* Samba's names end with a 0 that it counts */
    smb_put32(b + 60, 9);
    p = smb_find_entry(b, b + 147, kSmbFileDirectoryInfo, &e);
    CHECK(e.len == 8);
    /* SMB_INFO_STANDARD: "a.txt", 5 bytes, then "b" */
    memset(b, 0, sizeof b);
    smb_put16(b + 8, (17 << 9) | (11 << 5) | 14);
    smb_put32(b + 12, 5);
    b[22] = 5;
    memcpy(b + 23, "a.txt", 6);
    b[29 + 20] = 0x10;
    b[29 + 22] = 1;
    b[29 + 23] = 'b';
    p = smb_find_entry(b, b + 54, kSmbInfoStandard, &e);
    CHECK(p == b + 29 && e.len == 5 && e.size == 5 && e.date == ((17 << 9) | (11 << 5) | 14));
    p = smb_find_entry(p, b + 54, kSmbInfoStandard, &e);
    CHECK(p == b + 54 && e.folder && e.len == 1 && e.name[0] == 'b');
    /* a NetShareEnum reply: "public" with a comment, converter 0x1000 */
    memset(b, 0, sizeof b);
    memcpy(b, "public", 6);
    smb_put32(b + 16, 0x1000 + 40);
    memcpy(b + 20, "IPC$", 4);
    smb_put16(b + 34, 3);
    smb_put32(b + 36, 0x1000 + 39);
    memcpy(b + 40, "Public files", 13);
    CHECK(smb_share_entry(b, 53, 0, 0x1000, name, &type, comment, sizeof comment) && !strcmp(name, "public") &&
          type == 0 && !strcmp(comment, "Public files"));
    CHECK(smb_share_entry(b, 53, 1, 0x1000, name, &type, comment, sizeof comment) && type == 3 && !comment[0]);
    CHECK(!smb_share_entry(b, 53, 3, 0x1000, name, &type, comment, sizeof comment));
}

int main(void)
{
    test_md4();
    test_des();
    test_ntlm();
    test_names_times();
    test_entries();
    printf(failures ? "smb: %d failed\n" : "smb: all passed\n", failures);
    return failures != 0;
}
