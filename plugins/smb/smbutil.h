/* smbutil.h: the parts of the SMB Plug-In's smb: that need no Mac: MD4, DES, the LM and NT
   password hashes and their challenge responses, NetBIOS names, times, character sets,
   and reading what a server sends. Plain C, so tests/test_smb.c runs them on a host. */
#ifndef SMBUTIL_H
#define SMBUTIL_H

/* little-endian numbers, as SMB has them */
#define SMB_GET16(p) ((unsigned short)((p)[0] | ((p)[1] << 8)))
#define SMB_GET32(p) ((unsigned long)(p)[0] | ((unsigned long)(p)[1] << 8) | ((unsigned long)(p)[2] << 16) | ((unsigned long)(p)[3] << 24))
void smb_put16(unsigned char *p, unsigned short v);
void smb_put32(unsigned char *p, unsigned long v);

/* MD4 (RFC 1320) of len bytes into 16 */
void smb_md4(const unsigned char *in, long len, unsigned char out[16]);
/* DES of one 8-byte block, with a 7-byte key (56 bits; the parity bits are added) */
void smb_des7(const unsigned char key[7], const unsigned char in[8], unsigned char out[8]);

/* A password (Mac Roman, C string) as the LM hash (upper case, 14 bytes at most) and the
   NT hash (MD4 of it in UTF-16LE) */
void smb_lm_hash(const char *password, unsigned char out[16]);
void smb_nt_hash(const char *password, unsigned char out[16]);
/* The 24-byte answer to the server's 8-byte challenge, from either hash */
void smb_response(const unsigned char hash[16], const unsigned char challenge[8], unsigned char out[24]);

/* A NetBIOS name (RFC 1001), first-level encoded: 34 bytes (length, 32 letters, 0). The
   name is upper-cased and padded with spaces to 15 characters, then suffix. */
void smb_nb_name(unsigned char out[34], const char *name, unsigned char suffix);

/* An NT time (100 ns since 1601, 8 bytes) as seconds since 1904, as the Mac counts;
   0 if it's before 1904 or after 2040 */
unsigned long smb_nt_time(const unsigned char *p);
/* A DOS date and time (as SMB_INFO_STANDARD has them) as y, m, d, h, min */
void smb_dos_time(unsigned short date, unsigned short time, short out[5]);

/* A byte of a name from the server (code page 850, the "OEM" one) as Mac Roman */
unsigned char smb_from_oem(unsigned char c);

/* One entry of a folder listing (TRANS2 FIND_FIRST2 or FIND_NEXT2) */
typedef struct {
    const unsigned char *name;  /* not ended with 0 */
    short len;
    short folder;
    unsigned long size;
    unsigned long when;         /* seconds since 1904, the server's own (level 0x101) */
    unsigned short date, time;  /* DOS date and time (level 1) */
} SmbEntry;
enum { kSmbInfoStandard = 1, kSmbFileDirectoryInfo = 0x101 };
/* The entry at p (of level, with end the data's end) into e; returns the next entry, or
   0 if p holds no whole entry */
const unsigned char *smb_find_entry(const unsigned char *p, const unsigned char *end, short level, SmbEntry *e);

/* Entry i of a RAP NetShareEnum reply (level 1: 20 bytes each): its name (up to 12
   characters), type (0 a disk), and comment, which may be empty. Returns 0 if it's
   past the data. */
short smb_share_entry(const unsigned char *data, long len, short i, unsigned short converter,
                      char name[13], unsigned short *type, char *comment, long csize);

#endif
