/* Mill's network layer: names to addresses, and HTTP/1.0 over MacTCP. */
#ifndef MILL_NET_H
#define MILL_NET_H

#include "mactcp.h"

OSErr net_init(void);
void net_shutdown(void);
OSErr net_resolve(const char *host, ip_addr *addr);

/* Progress, for the status line ("Looking up host...", "Connecting..."); may be NULL */
extern void (*net_status)(const char *msg);

/* Called while waiting on the network; return true to give up (userCanceledErr) */
extern Boolean (*net_idle)(void);

/* How far the last request got, for saying what went wrong */
enum { NET_LOOKUP, NET_CONNECT, NET_TRANSFER };
extern int net_stage;

/* Called with each chunk of the response as it arrives; return false to stop. */
typedef Boolean (*net_sink)(const char *data, long len, void *ref);
/* An HTTP request to host:port, the site itself or a proxy: target is what the request
   line asks for (the path, or the whole address when asking a proxy), and site is the
   Host header. A POST when content_type is given, sending body; a GET otherwise.
   headers, if not NULL or empty, are more header lines to send (Cookie, Authorization),
   separated by CR LF, without one at the end. */
OSErr net_http_request(const char *host, unsigned short port, const char *target, const char *site,
                       const char *headers, const char *content_type, const char *body, long body_len,
                       net_sink sink, void *ref);

#endif
