/* gzip files (RFC 1952) and the deflate data in them (RFC 1951), read as they arrive.
   What comes out goes to out in pieces; only the 32K window deflate needs is kept, and
   only while decoding. */
#ifndef MILL_INFLATE_H
#define MILL_INFLATE_H

/* out returns 0 to go on, nonzero to stop */
typedef int (*gz_out)(void *ref, const unsigned char *p, long n);

/* Memory from the caller (a Plug-In has no malloc): alloc returns NULL if there's none */
typedef struct {
    void *ref;
    void *(*alloc)(void *ref, long n);
    void (*release)(void *ref, void *p);
} GzMem;

/* NULL if there's no memory. gz_begin reads a gzip file, with malloc; gz_begin_with
   takes its memory from mem, and with raw set reads bare deflate data (a zip member, or
   what follows a gzip header read by the caller). */
void *gz_begin(gz_out out, void *ref);
void *gz_begin_with(const GzMem *mem, int raw, gz_out out, void *ref);
/* Each piece: 0 to go on, 1 at the end of the (first) member, -1 for data it can't
   read, -2 if out said to stop */
int gz_data(void *d, const unsigned char *p, long n);
/* After gz_data returned 1: how many bytes at the end of that piece weren't deflate
   data (a gzip trailer, or whatever follows) */
long gz_unused(void *d);
void gz_end(void *d);
/* The memory gz_begin_with takes, all told (its state and the window), less any rounding */
long gz_size(void);

/* The same four calls, for code that runs in a Plug-In, where they're Mill's (the host's
   GzBegin, GzData, GzUnused, and GzEnd; plugins/gzhost), and on a host in the tests,
   where they're these. begin takes ref, then gz_begin_with's arguments. */
typedef struct {
    void *ref;
    void *(*begin)(void *ref, const GzMem *mem, int raw, gz_out out, void *out_ref);
    int (*data)(void *d, const unsigned char *p, long n);
    long (*unused)(void *d);
    void (*end)(void *d);
} GzCalls;

#endif
