/* Mill's network layer: names to addresses, and TCP connections over MacTCP, which
   Plug-Ins (http: among them) use through the host table. */
#ifndef MILL_NET_H
#define MILL_NET_H

#include "mactcp.h"

/* Open MacTCP and its resolver, the first time the network is needed (with MacPPP,
   that dials): noErr once open, NET_NO_MACTCP if it can't be. Each net_ call that
   needs MacTCP calls it. */
OSErr net_open(void);
extern short net_ipp;       /* the .IPP driver */
extern Handle net_dnr;      /* the resolver's code */
void net_shutdown(void);
OSErr net_resolve(const char *host, ip_addr *addr);
/* At startup, when Settings asks: whether the name to look up is found, saying
   "Connection check" on the splash (mac_init.c) */
Boolean net_check(void);

/* Progress, for the status line ("Looking up host...", "Connecting..."); may be NULL */
extern void (*net_status)(const char *msg);

/* Called while waiting on the network; return true to give up (userCanceledErr) */
extern Boolean (*net_idle)(void);

/* How far the last request got, for saying what went wrong */
enum { NET_LOOKUP, NET_CONNECT, NET_TRANSFER };
extern int net_stage;
/* At NET_CONNECT: whether the server refused the connection (a reset), not just didn't answer */
extern Boolean net_refused;

/* Called with each chunk of a response as it arrives; return false to stop. */
typedef Boolean (*net_sink)(const char *data, long len, void *ref);

/* TCP connections. Each waits, calling
   net_idle, and returns userCanceledErr if that asks to stop. */
typedef struct NetConn NetConn;
/* What opening a connection returns when MacTCP isn't there */
#define NET_NO_MACTCP notOpenErr
/* Look up host and connect, saying so through net_status */
OSErr net_tcp_open(const char *host, unsigned short port, NetConn **c);
OSErr net_tcp_send(NetConn *c, const void *data, long len);
/* *len: in, the room in buf; out, what came, 0 once the other side has closed */
OSErr net_tcp_recv(NetConn *c, void *buf, long *len);
/* Closes without waiting: the close goes on while Mill does, and ends by itself */
void net_tcp_close(NetConn *c);
/* Wait for the closes still going on (quitting: abort those still waiting); whether any
   were (each held 16K) */
Boolean net_close_wait(Boolean quitting);
/* Wait for a connection on a port of Mill's choosing: this Mac's address and that port
   (for FTP's PORT); net_tcp_accept waits until it comes */
OSErr net_tcp_listen(NetConn **c, ip_addr *addr, unsigned short *port);
OSErr net_tcp_accept(NetConn *c);

/* Kept connections: between requests, each window (owner) may keep one idle connection,
   to its page's server, for the next request there. key names the server and login
   ("ftp://host:0" CR "login"); note is what the protocol left with it, and bye what's
   sent before Mill closes it (FTP's QUIT). Mill closes a kept connection when its window
   goes to another server or closes, after net_keep_secs idle, on Clear Logins, and on
   Quit. */
extern long net_keep_secs;              /* 0: nothing is kept */
#define NET_NOTE_MAX 512
/* owner's connection for key, taken out of the table with its note (*len: in, the room;
   out, the length); NULL if it keeps none for key, or the one it kept has closed, been
   spoken on, or waited too long (those are closed) */
NetConn *net_keep_take(const void *owner, const char *key, void *note, long *len);
/* Keep c for owner (closing any it kept before); false if it couldn't (c is closed) */
Boolean net_keep_put(const void *owner, NetConn *c, const char *key, const void *note, long nlen,
                     const void *bye, long blen);
/* Close owner's kept connection (NULL: every window's); unless it's to server, the start
   of its key ("ftp://host:0"), when server isn't NULL. Whether any was closed. */
Boolean net_keep_close(const void *owner, const char *server);
/* Close every kept connection but owner's to server (making room while owner loads a
   page there, which would only open another). Whether any was closed. */
Boolean net_keep_close_others(const void *owner, const char *server);
/* Now and then, while Mill waits for events: close those that have waited too long or
   that the server has closed */
void net_keep_idle(void);

#endif
