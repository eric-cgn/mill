/* archive.h: the container formats' cores (zip, tar, gzip, MacBinary, BinHex 4.0), plain C
   so tests/test_archive.c runs them on a host. Not yet a Plug-In: the format API they will
   sit under (Mill 2.1's millGivesMembers, Entry, ReadAt) is still being drawn, so each
   core talks to an ArcHost of callbacks that the Plug-In layer will fill in.

   Two ways in, by what the format needs:
   - push (arc_begin, arc_data, arc_end): bytes in order, in pieces of any size. tar,
     gzip, MacBinary, and BinHex.
   - pull (arc_pull): the core asks for bytes with read_at and size. zip, and tar.

   Either way the core lists (each member through entry) or opens a member (its bytes
   through out). 68000 rules: bytes are read one at a time (no odd-address words), no
   big tables, memory only from the host. */
#ifndef ARCHIVE_H
#define ARCHIVE_H

#include "../../src/core/inflate.h"

enum { ARC_ZIP, ARC_TAR, ARC_GZIP, ARC_MACBINARY, ARC_BINHEX, ARC_FORMATS };

/* What the calls return */
enum {
    ARC_MORE = 0,           /* push: give more */
    ARC_DONE = 1,           /* listed, or the member is out */
    ARC_BAD = -1,           /* not this format, cut short, or a checksum that fails */
    ARC_NO_MEMORY = -2,
    ARC_STOPPED = -3,       /* entry or out said to stop */
    ARC_UNSUPPORTED = -4,   /* zip64, a split zip, encryption, a method other than stored and deflate */
    ARC_NOT_FOUND = -5,     /* no file member on the path */
    ARC_READ = -6           /* read_at failed */
};

enum { ARC_FILE, ARC_FOLDER, ARC_LINK };

/* What a name's bytes are in: zip (before 2006) is IBM PC code page 437, tar is
   unspecified (taken as Latin-1), gzip is Latin-1 (RFC 1952), MacBinary and BinHex are
   Mac Roman */
enum { ARC_CP437, ARC_LATIN1, ARC_MACROMAN };

/* Entry flags */
#define ARC_ENCRYPTED 1     /* listed, but can't be opened */
#define ARC_CANT_OPEN 2     /* a method or type the core doesn't read */
#define ARC_FINDER 4        /* type, creator, and finder_flags are set */

typedef struct {
    const char *name;           /* cleaned (arc_clean), '/' between folders, no '/' at the end */
    int charset;
    int kind;                   /* ARC_FILE, ARC_FOLDER, ARC_LINK */
    long size, packed;          /* -1 if not known */
    unsigned long modified;     /* seconds since 1904 (the Mac's clock), 0 if not known */
    long mode;                  /* Unix permission bits, -1 if none */
    const char *comment;        /* NULL if none */
    const char *link;           /* a link's target, else NULL */
    char type[4], creator[4];   /* with ARC_FINDER */
    unsigned short finder_flags;
    long data_len, rsrc_len;    /* a Mac file's forks, -1 if not one */
    int flags;
    unsigned long pos;          /* opaque: hand it back in ArcRequest to open this member directly */
} ArcEntry;

typedef struct {
    void *ref;
    /* Memory: NULL if there's none */
    void *(*alloc)(void *ref, long n);
    void (*release)(void *ref, void *p);
    /* Each member when listing; 0 to go on, nonzero to stop */
    int (*entry)(void *ref, const ArcEntry *e);
    /* The opened member's bytes, in order; 0 to go on, nonzero to stop */
    int (*out)(void *ref, const unsigned char *p, long n);
    /* Opening: the member found, just before its bytes, with what's left of the path after
       it (arc_rest's); 0 to go on, nonzero to stop. *fork is the request's, and may be
       changed (a Mac file saved with both forks). May be NULL. */
    int (*member)(void *ref, const ArcEntry *e, const char *rest, int *fork);
    /* Pull only (NULL otherwise): n bytes at offset into p; how many were read (fewer at
       the end), or -1. size: the archive's length, or -1 if not known. */
    long (*read_at)(void *ref, unsigned long offset, unsigned char *p, long n);
    long (*size)(void *ref);
    /* Inflate, for zip and gzip: Mill's (the host's GzBegin and the rest), or the tests' */
    const GzCalls *gz;
} ArcHost;

/* The forks of a Mac file (MacBinary, BinHex) */
enum { ARC_DATA_FORK, ARC_RSRC_FORK, ARC_BOTH_FORKS /* the data fork, then the resource fork */ };

typedef struct {
    int open;               /* 0 to list, 1 to open the member at path */
    /* The member path after "#/", %-decoded (docs/readme.txt, or kit.tar/docs/readme.txt
       for a member of a member). The core takes names until it reaches a file; what's
       left is the rest (arc_rest), for the next container. A one-member format (gzip,
       MacBinary, BinHex) takes the first name whatever it is: its name is a label. */
    const char *path;
    unsigned long pos;      /* an Entry's pos, if has_pos: a hint, checked against the name */
    int has_pos;
    int fork;               /* MacBinary, BinHex: ARC_DATA_FORK, ARC_RSRC_FORK, or ARC_BOTH_FORKS */
    const char *name;       /* gzip: the archive's own file name, to name the member when the header doesn't */
} ArcRequest;

/* Push. begin: NULL if there's no memory or the format can't be pushed (zip). data with
   each piece: ARC_MORE, ARC_DONE, or an error; with n 0 at the end of the input. A
   listing that ends cleanly at the end of the input is ARC_DONE. end frees it, done or
   not. rest: after the member was found, the path left over ("" if none), else NULL. */
void *arc_begin(int format, const ArcHost *h, const ArcRequest *r);
int arc_data(void *d, const unsigned char *p, long n);
const char *arc_rest(void *d);
void arc_end(void *d);

/* Pull (zip, tar): list, or open the member; ARC_DONE or an error. *rest as arc_rest. */
int arc_pull(int format, const ArcHost *h, const ArcRequest *r, const char **rest);

/* ---- names ---- */

/* A member's name made safe: '\' read as '/' only where the format says (zip, from DOS);
   empty and "." parts and leading '/' dropped; a ".." part, a control character, or
   nothing left rejects it. Into out (size bytes); its length, or -1 if rejected. *folder
   is set if the name ended in '/'. */
long arc_clean(const unsigned char *in, long n, int backslash, char *out, long size, int *folder);

/* A name to save a member as on the Mac: its last part in Mac Roman, ':' as '-', no
   control characters, no leading '.' (names starting with a period are the Device
   Manager's), at most 31 bytes with the extension kept. out has 32 bytes. */
void arc_hfs_name(const char *name, int charset, char out[32]);

/* ---- for the cores ---- */

typedef struct {
    const ArcHost *h;
    ArcRequest r;
    const char *rest;       /* see arc_rest */
    int format;
    int result;             /* once not ARC_MORE, what every later arc_data returns */
} ArcBase;

void *arc_new(const ArcHost *h, long size);    /* zeroed */
void arc_free(const ArcHost *h, void *p);

/* Little- and big-endian numbers, a byte at a time */
unsigned short arc_le16(const unsigned char *p);
unsigned long arc_le32(const unsigned char *p);
unsigned long arc_be32(const unsigned char *p);

/* CRC-32 (ISO 3309, as zip and gzip use): start with 0 */
unsigned long arc_crc32(unsigned long crc, const unsigned char *p, long n);

/* Dates in the Mac's seconds since 1904: from a Unix time, and from MS-DOS's date and
   time words (local time, two-second steps; 0 if the date is 0) */
unsigned long arc_unix_date(unsigned long t);
unsigned long arc_dos_date(unsigned short date, unsigned short time);

/* If the member named name (a cleaned name) is on path, the rest of path after it ("" if
   path is just name), else NULL. arc_first: the rest after path's first name. */
const char *arc_on_path(const char *name, const char *path);
const char *arc_first(const char *path);

/* A Mac file's name (MacBinary, BinHex: one name, up to 63 bytes, '/' allowed) as one
   cleaned part, '/' and control characters as '-'; out has 64 bytes */
void arc_mac_name(const unsigned char *in, int n, char out[64]);

/* An Entry with nothing known */
void arc_entry_init(ArcEntry *e);
/* Opening: tell the host's member (if any) about e; ARC_MORE, or ARC_STOPPED. fork: the
   request's, which the host may change; NULL for formats without forks. */
int arc_member(const ArcHost *h, const ArcEntry *e, const char *rest, int *fork);

/* The cores */
int zip_pull(const ArcHost *h, const ArcRequest *r, const char **rest);
void *tar_begin(const ArcHost *h, const ArcRequest *r);
int tar_data(void *d, const unsigned char *p, long n);
int tar_pull(const ArcHost *h, const ArcRequest *r, const char **rest);
void *gzip_begin(const ArcHost *h, const ArcRequest *r);
int gzip_data(void *d, const unsigned char *p, long n);
void gzip_end(void *d);
void *macbinary_begin(const ArcHost *h, const ArcRequest *r);
int macbinary_data(void *d, const unsigned char *p, long n);
void *binhex_begin(const ArcHost *h, const ArcRequest *r);
int binhex_data(void *d, const unsigned char *p, long n);

#endif
