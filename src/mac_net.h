/* Mill's network layer: names to addresses, and TCP connections over MacTCP, which
   Plug-Ins (http: among them) use through the host table. */
#ifndef MILL_NET_H
#define MILL_NET_H

#include "mactcp.h"

/* Open MacTCP and its resolver (mac_init.c, run once) */
OSErr net_init(void);
extern short net_ipp;       /* the .IPP driver */
extern Handle net_dnr;      /* the resolver's code */
void net_shutdown(void);
OSErr net_resolve(const char *host, ip_addr *addr);

/* Progress, for the status line ("Looking up host...", "Connecting..."); may be NULL */
extern void (*net_status)(const char *msg);

/* Called while waiting on the network; return true to give up (userCanceledErr) */
extern Boolean (*net_idle)(void);

/* How far the last request got, for saying what went wrong */
enum { NET_LOOKUP, NET_CONNECT, NET_TRANSFER };
extern int net_stage;

/* Called with each chunk of a response as it arrives; return false to stop. */
typedef Boolean (*net_sink)(const char *data, long len, void *ref);

/* TCP connections. Each waits, calling
   net_idle, and returns userCanceledErr if that asks to stop. */
typedef struct NetConn NetConn;
/* Look up host and connect, saying so through net_status */
OSErr net_tcp_open(const char *host, unsigned short port, NetConn **c);
OSErr net_tcp_send(NetConn *c, const void *data, long len);
/* *len: in, the room in buf; out, what came, 0 once the other side has closed */
OSErr net_tcp_recv(NetConn *c, void *buf, long *len);
void net_tcp_close(NetConn *c);
/* Wait for a connection on a port of Mill's choosing: this Mac's address and that port
   (for FTP's PORT); net_tcp_accept waits until it comes */
OSErr net_tcp_listen(NetConn **c, ip_addr *addr, unsigned short *port);
OSErr net_tcp_accept(NetConn *c);

#endif
