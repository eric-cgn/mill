/* Mac files as one stream of bytes, for uploading: MacBinary II and BinHex 4.0. */
#ifndef MILL_MACENC_H
#define MILL_MACENC_H

/* CRC-16 (CCITT, as XMODEM): what MacBinary II and BinHex use */
unsigned short crc_ccitt(unsigned short crc, const unsigned char *p, long n);

/* A MacBinary II header: name (Mac Roman, up to 63), type and creator, Finder flags,
   the forks' lengths, and the dates (seconds since 1904) */
void macbinary_make(unsigned char h[128], const char *name, const char type[4], const char creator[4],
                      unsigned short flags, long data_len, long rsrc_len, unsigned long created, unsigned long modified);
/* BinHex's header block, before the data fork: its length (up to 22 + 63) */
int binhex_header(unsigned char *h, const char *name, const char type[4], const char creator[4],
                  unsigned short flags, long data_len, long rsrc_len);

/* BinHex 4.0's text, made from the bytes put in (header block, data fork and its CRC,
   resource fork and its CRC): run-length coded, six bits a character, 64 to a line,
   between the introduction and colons. With out NULL it only counts. */
typedef struct {
    unsigned char *out;
    long len, size, count;      /* out's bytes, its room, and all made so far */
    int last, run;              /* the run being counted: its byte (-1 none), how many */
    unsigned long bits;
    int nbits, col;
} Hqx;
void hqx_begin(Hqx *h, unsigned char *out, long size);
/* Put bytes in; returns 0, or -1 if out is full (make room and put the rest) */
int hqx_put(Hqx *h, const unsigned char *p, long n);
void hqx_finish(Hqx *h);

#endif
