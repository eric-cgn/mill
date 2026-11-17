/* Client pull, as Netscape 1.1 had it: a Refresh header, or <META HTTP-EQUIV="Refresh">,
   says "N" (load this page again in N seconds) or "N; URL=address" (load that one). */
#ifndef MILL_REFRESH_H
#define MILL_REFRESH_H
#include "html.h"
/* "N", "N; URL=address", "N, URL=address", or "N; address" (spaces and quotes allowed):
   1 with *secs and url ("" for this page), else 0. url is cut to size. */
int refresh_parse(const char *s, long n, long *secs, char *url, int size);
/* The CONTENT of the page's first META HTTP-EQUIV="Refresh", or NULL */
const char *refresh_meta(const Doc *doc);
#endif
