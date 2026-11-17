/* smbutil.c: see smbutil.h. MD4 is RFC 1320's; DES is FIPS 46's, a bit at a time, which is
   slow but small (a login needs five blocks). */
#include <string.h>

#include "smbutil.h"

typedef unsigned long u32;
#define M32 0xFFFFFFFFUL

void smb_put16(unsigned char *p, unsigned short v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

void smb_put32(unsigned char *p, unsigned long v)
{
    smb_put16(p, (unsigned short)v);
    smb_put16(p + 2, (unsigned short)(v >> 16));
}

/* ---- MD4 ---- */
/* RFC 1320 */

#define ROL(x, n) ((((x) << (n)) | (((x) & M32) >> (32 - (n)))) & M32)

static void md4_block(u32 h[4], const unsigned char *b)
{
    static const unsigned char s[12] = { 3, 7, 11, 19, 3, 5, 9, 13, 3, 9, 11, 15 };
    static const unsigned char k2[16] = { 0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15 };
    static const unsigned char k3[16] = { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };
    u32 x[16], a = h[0], bb = h[1], c = h[2], d = h[3], t, f;
    short i;
    for (i = 0; i < 16; i++) x[i] = SMB_GET32(b + 4 * i);
    for (i = 0; i < 48; i++) {
        if (i < 16) f = ((bb & c) | (~bb & d)) + x[i];
        else if (i < 32) f = ((bb & c) | (bb & d) | (c & d)) + x[k2[i - 16]] + 0x5A827999UL;
        else f = (bb ^ c ^ d) + x[k3[i - 32]] + 0x6ED9EBA1UL;
        t = (a + f) & M32;
        t = ROL(t, s[(i >> 4) * 4 + (i & 3)]);
        a = d; d = c; c = bb; bb = t;
    }
    h[0] = (h[0] + a) & M32;
    h[1] = (h[1] + bb) & M32;
    h[2] = (h[2] + c) & M32;
    h[3] = (h[3] + d) & M32;
}

void smb_md4(const unsigned char *in, long len, unsigned char out[16])
{
    u32 h[4];
    unsigned char last[128];
    long n = len, k;
    short i;
    h[0] = 0x67452301UL; h[1] = 0xEFCDAB89UL; h[2] = 0x98BADCFEUL; h[3] = 0x10325476UL;
    for (; n >= 64; n -= 64, in += 64) md4_block(h, in);
    memset(last, 0, sizeof last);
    memcpy(last, in, n);
    last[n] = 0x80;
    k = n < 56 ? 64 : 128;
    smb_put32(last + k - 8, (u32)len << 3);
    smb_put32(last + k - 4, (u32)len >> 29);
    md4_block(h, last);
    if (k == 128) md4_block(h, last + 64);
    for (i = 0; i < 4; i++) smb_put32(out + 4 * i, h[i]);
}

/* ---- DES ---- */
/* FIPS 46 */

static const unsigned char IP[64] = {
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22, 14, 6,
    64, 56, 48, 40, 32, 24, 16, 8, 57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7 };
static const unsigned char FP[64] = {
    40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31, 38, 6, 46, 14, 54, 22, 62, 30,
    37, 5, 45, 13, 53, 21, 61, 29, 36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25 };
static const unsigned char EX[48] = {
    32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
    16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1 };
static const unsigned char PP[32] = {
    16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
    2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25 };
static const unsigned char PC1[56] = {
    57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51, 43, 35, 27,
    19, 11, 3, 60, 52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22,
    14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4 };
static const unsigned char PC2[48] = {
    14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2,
    41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32 };
static const unsigned char SB[8][64] = {
    { 14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7, 0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
      4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0, 15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13 },
    { 15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10, 3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
      0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15, 13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9 },
    { 10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8, 13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
      13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7, 1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12 },
    { 7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15, 13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
      10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4, 3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14 },
    { 2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9, 14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
      4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14, 11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3 },
    { 12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11, 10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
      9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6, 4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13 },
    { 4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1, 13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
      1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2, 6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12 },
    { 13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7, 1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
      7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8, 2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11 }
};

#define BIT(p, n) (((p)[(n) >> 3] >> (7 - ((n) & 7))) & 1)

void smb_des7(const unsigned char key[7], const unsigned char in[8], unsigned char out[8])
{
    unsigned char cd[56], lr[64], f[32], t;
    short i, r, s, n;
    /* PC1 numbers the 64-bit key's bits, every eighth a parity bit it never picks: the
       others are the 56 bits of key, in order */
    for (i = 0; i < 56; i++) {
        n = PC1[i] - 1;
        n = (n >> 3) * 7 + (n & 7);
        cd[i] = BIT(key, n);
    }
    for (i = 0; i < 64; i++) lr[i] = BIT(in, IP[i] - 1);
    for (r = 0; r < 16; r++) {
        for (s = (r < 2 || r == 8 || r == 15) ? 1 : 2; s > 0; s--) {
            t = cd[0]; memmove(cd, cd + 1, 27); cd[27] = t;
            t = cd[28]; memmove(cd + 28, cd + 29, 27); cd[55] = t;
        }
        for (s = 0; s < 8; s++) {
            unsigned char b[6], v;
            for (i = 0; i < 6; i++) b[i] = lr[32 + EX[s * 6 + i] - 1] ^ cd[PC2[s * 6 + i] - 1];
            v = SB[s][(b[0] << 5) | (b[5] << 4) | (b[1] << 3) | (b[2] << 2) | (b[3] << 1) | b[4]];
            for (i = 0; i < 4; i++) f[s * 4 + i] = (v >> (3 - i)) & 1;
        }
        for (i = 0; i < 32; i++) {
            t = lr[i] ^ f[PP[i] - 1];
            lr[i] = lr[32 + i];
            lr[32 + i] = t;
        }
    }
    memset(out, 0, 8);
    for (i = 0; i < 64; i++) {
        /* after the last round the halves go back the other way: R16 L16 */
        n = FP[i] - 1;
        n = n < 32 ? n + 32 : n - 32;
        out[i >> 3] |= lr[n] << (7 - (i & 7));
    }
}

/* ---- passwords ---- */

/* Mac Roman 128-255 as Unicode */
static const unsigned short MAC_UNI[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x00A4, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7
};

/* MS-NLMP 3.3.1 */
void smb_lm_hash(const char *password, unsigned char out[16])
{
    static const unsigned char magic[8] = { 'K', 'G', 'S', '!', '@', '#', '$', '%' };
    unsigned char pw[14];
    short i;
    memset(pw, 0, sizeof pw);
    for (i = 0; i < 14 && password[i]; i++) {
        unsigned char c = (unsigned char)password[i];
        pw[i] = c >= 'a' && c <= 'z' ? c - 32 : c;
    }
    smb_des7(pw, magic, out);
    smb_des7(pw + 7, magic, out + 8);
}

/* MS-NLMP 3.3.1 */
void smb_nt_hash(const char *password, unsigned char out[16])
{
    unsigned char u[256];
    short i;
    for (i = 0; i < 128 && password[i]; i++) {
        unsigned char c = (unsigned char)password[i];
        smb_put16(u + 2 * i, c < 128 ? c : MAC_UNI[c - 128]);
    }
    smb_md4(u, 2 * i, out);
}

/* MS-NLMP 3.3.1 */
void smb_response(const unsigned char hash[16], const unsigned char challenge[8], unsigned char out[24])
{
    unsigned char k[21];
    memset(k, 0, sizeof k);
    memcpy(k, hash, 16);
    smb_des7(k, challenge, out);
    smb_des7(k + 7, challenge, out + 8);
    smb_des7(k + 14, challenge, out + 16);
}

/* ---- names, times, characters ---- */

/* RFC 1001 */
void smb_nb_name(unsigned char out[34], const char *name, unsigned char suffix)
{
    short i;
    out[0] = 32;
    for (i = 0; i < 16; i++) {
        unsigned char c = i == 15 ? suffix : *name ? (unsigned char)*name++ : ' ';
        if (c >= 'a' && c <= 'z') c -= 32;
        out[1 + 2 * i] = 'A' + (c >> 4);
        out[2 + 2 * i] = 'A' + (c & 15);
    }
    out[33] = 0;
}

/* w (four 16-bit digits, least first) divided by d, in place */
static void div16(unsigned short w[4], unsigned short d)
{
    u32 r = 0;
    short i;
    for (i = 3; i >= 0; i--) {
        r = (r << 16) | w[i];
        w[i] = (unsigned short)(r / d);
        r %= d;
    }
}

/* MS-CIFS */
unsigned long smb_nt_time(const unsigned char *p)
{
    /* 1601 to 1904 in seconds: 0x239EAE080 */
    unsigned short w[4];
    u32 hi, lo;
    short i;
    for (i = 0; i < 4; i++) w[i] = SMB_GET16(p + 2 * i);
    div16(w, 10000);
    div16(w, 1000);
    hi = ((u32)w[3] << 16) | w[2];
    lo = ((u32)w[1] << 16) | w[0];
    if (hi < 2 || (hi == 2 && lo < 0x39EAE080UL)) return 0;
    if (lo < 0x39EAE080UL) hi--;
    if (hi != 2) return 0;
    return (lo - 0x39EAE080UL) & M32;
}

/* MS-CIFS */
void smb_dos_time(unsigned short date, unsigned short time, short out[5])
{
    out[0] = 1980 + (date >> 9);
    out[1] = (date >> 5) & 15;
    out[2] = date & 31;
    out[3] = time >> 11;
    out[4] = (time >> 5) & 63;
}

/* code page 850 128-255 as Mac Roman ('?' where it has none: box drawing and the rest) */
static const unsigned char OEM_MAC[128] = {
    0x82, 0x9F, 0x8E, 0x89, 0x8A, 0x88, 0x8C, 0x8D, 0x90, 0x91, 0x8F, 0x95, 0x94, 0x93, 0x80, 0x81,
    0x83, 0xBE, 0xAE, 0x99, 0x9A, 0x98, 0x9E, 0x9D, 0xD8, 0x85, 0x86, 0xBF, 0xA3, 0xAF, 0x3F, 0xC4,
    0x87, 0x92, 0x97, 0x9C, 0x96, 0x84, 0xBB, 0xBC, 0xC0, 0xA8, 0xC2, 0x3F, 0x3F, 0xC1, 0xC7, 0xC8,
    0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0xE7, 0xE5, 0xCB, 0xA9, 0x3F, 0x3F, 0x3F, 0x3F, 0xA2, 0xB4, 0x3F,
    0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x8B, 0xCC, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F,
    0x3F, 0x3F, 0xE6, 0xE8, 0xE9, 0xF5, 0xEA, 0xEB, 0xEC, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0xED, 0x3F,
    0xEE, 0xA7, 0xEF, 0xF1, 0x9B, 0xCD, 0xB5, 0x3F, 0x3F, 0xF2, 0xF3, 0xF4, 0x3F, 0x3F, 0xF8, 0xAB,
    0x3F, 0xB1, 0x3F, 0x3F, 0xA6, 0xA4, 0xD6, 0xFC, 0xA1, 0xAC, 0xE1, 0x3F, 0x3F, 0x3F, 0x3F, 0xCA
};

unsigned char smb_from_oem(unsigned char c)
{
    return c < 128 ? c : OEM_MAC[c - 128];
}

/* ---- replies ---- */

/* MS-CIFS */
const unsigned char *smb_find_entry(const unsigned char *p, const unsigned char *end, short level, SmbEntry *e)
{
    long room = end - p, next;
    memset(e, 0, sizeof *e);
    if (level == kSmbFileDirectoryInfo) {
        /* NextEntryOffset, FileIndex, four times, EndOfFile, AllocationSize,
           ExtFileAttributes, FileNameLength, FileName */
        if (room < 64) return 0;
        e->len = (short)SMB_GET32(p + 60);
        if (e->len < 0 || 64 + e->len > room) return 0;
        e->name = p + 64;
        while (e->len && !e->name[e->len - 1]) e->len--;   /* Samba counts the 0 ending a name */
        e->folder = (p[56] & 0x10) != 0;
        e->size = SMB_GET32(p + 40);
        e->when = smb_nt_time(p + 24);
        next = (long)SMB_GET32(p);
        return next > 0 && next <= room ? p + next : end;
    }
    /* SMB_INFO_STANDARD: three DOS dates and times, DataSize, AllocationSize,
       Attributes, FileNameLength, FileName and its 0 */
    if (room < 23) return 0;
    e->len = p[22];
    if (23 + e->len > room) return 0;
    e->name = p + 23;
    e->folder = (p[20] & 0x10) != 0;
    e->size = SMB_GET32(p + 12);
    e->date = SMB_GET16(p + 8);
    e->time = SMB_GET16(p + 10);
    next = 23 + e->len + 1;
    return next < room ? p + next : end;
}

short smb_share_entry(const unsigned char *data, long len, short i, unsigned short converter,
                      char name[13], unsigned short *type, char *comment, long csize)
{
    /* MS-RAP */
    const unsigned char *e = data + 20L * i;
    long at, k;
    if (20L * (i + 1) > len) return 0;
    memcpy(name, e, 13);
    name[12] = 0;
    *type = SMB_GET16(e + 14);
    at = (long)((SMB_GET32(e + 16) & 0xFFFF) - converter);
    for (k = 0; at >= 0 && at + k < len && data[at + k] && k < csize - 1; k++) comment[k] = data[at + k];
    comment[k] = 0;
    return 1;
}
