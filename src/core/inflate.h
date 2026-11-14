/* gzip files (RFC 1952) and the deflate data in them (RFC 1951), read as they arrive.
   What comes out goes to out in pieces; only the 32K window deflate needs is kept, and
   only while decoding. */
#ifndef MILL_INFLATE_H
#define MILL_INFLATE_H

/* out returns 0 to go on, nonzero to stop */
typedef int (*gz_out)(void *ref, const unsigned char *p, long n);

/* NULL if there's no memory */
void *gz_begin(gz_out out, void *ref);
/* Each piece: 0 to go on, 1 at the end of the (first) member, -1 for data it can't
   read, -2 if out said to stop */
int gz_data(void *d, const unsigned char *p, long n);
void gz_end(void *d);

#endif
