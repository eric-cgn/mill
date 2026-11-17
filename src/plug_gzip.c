/* Content-Encoding: gzip, undone so a page can be shown: a transfer matter, not a format
   (gzip files are the Archives Plug-In's). In a segment of its own, with core/inflate.c. */
#include <Multiverse.h>

#include "core/inflate.h"

void *encoding_begin(int (*out)(void *ref, const unsigned char *p, long n), void *ref) { return gz_begin(out, ref); }
int encoding_data(void *g, const unsigned char *p, long n) { return gz_data(g, p, n); }
void encoding_end(void *g) { gz_end(g); }
