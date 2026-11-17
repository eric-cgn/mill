/* MacTCP: the .IPP driver for TCP, and MacTCP's own resolver for names. Calls are
   synchronous for now; MacTCP keeps the system running underneath them. */
#include <Multiverse.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "mac_net.h"
#include "mac_sys.h"
#include "mac_memreport.h"
#include "version.h"

#define RCV_BUFFER_SIZE 16384
#define TIMEOUT 30  /* seconds */

enum { OPENRESOLVER = 1, CLOSERESOLVER = 2, STRTOADDR = 3 };

typedef OSErr (*dnr_proc)(long selector, ...);

short net_ipp = 0;
void (*net_status)(const char *msg) = NULL;
Boolean (*net_idle)(void) = NULL;
int net_stage = NET_LOOKUP;

static void status(const char *msg) { if (net_status) net_status(msg); }
Handle net_dnr = NULL;

/* ------------------------------------------------------------ opening MacTCP */

/* MacTCP's resolver lives in a 'dnrp' resource: in the MacTCP control panel or in a
   "MacTCP DNR" file, in the System Folder, Control Panels or Extensions. */
static Boolean is_resolver_file(const HFileInfo *pb)
{
    OSType type = pb->ioFlFndrInfo.fdType, creator = pb->ioFlFndrInfo.fdCreator;
    if (type == 'cdev' && (creator == 'ztcp' || creator == 'mtcp')) return true;
    return pb->ioNamePtr[0] == 10 && memcmp(pb->ioNamePtr + 1, "MacTCP DNR", 10) == 0;
}

static Handle load_dnrp_from(OSType folder)
{
    short vref; long dir; short i;
    Str63 name;
    HFileInfo pb;
    if (sys_folder(folder, &vref, &dir) != noErr) return NULL;
    for (i = 1; ; i++) {
        memset(&pb, 0, sizeof pb);
        pb.ioNamePtr = name;
        pb.ioVRefNum = vref;
        pb.ioDirID = dir;
        pb.ioFDirIndex = i;
        if (PBGetCatInfoSync((CInfoPBPtr)&pb) != noErr) return NULL;
        if ((pb.ioFlAttrib & ioDirMask) == 0 && is_resolver_file(&pb)) {
            FSSpec spec;
            short ref, was = CurResFile();
            Handle h;
            if (file_spec(vref, dir, name, &spec) != noErr) continue;
            ref = file_open_res(&spec, fsRdPerm);
            if (ref == -1) continue;
            h = Get1IndResource('dnrp', 1);
            if (h) { DetachResource(h); MoveHHi(h); HLock(h); }
            CloseResFile(ref);
            UseResFile(was);
            if (h) return h;
        }
    }
}

/* With MacPPP, opening MacTCP dials: so it's opened when first needed, not at launch.
   Every use of the network comes through here. Once the driver is open, it stays so;
   while it can't be opened, each use tries again. */
OSErr net_open(void)
{
    OSErr err;
    if (net_ipp) return noErr;
    status("Opening MacTCP\311");
    if ((err = OpenDriver("\p.IPP", &net_ipp)) != noErr) { net_ipp = 0; return NET_NO_MACTCP; }
    net_dnr = load_dnrp_from(kControlPanelFolderType);
    if (!net_dnr) net_dnr = load_dnrp_from(kSystemFolderType);
    if (!net_dnr) net_dnr = load_dnrp_from(kExtensionFolderType);
    if (net_dnr && ((dnr_proc)*net_dnr)(OPENRESOLVER, (char *)NULL) != noErr) {
        DisposeHandle(net_dnr);
        net_dnr = NULL;
    }
    return noErr;
}

/* ------------------------------------------------------------ the resolver */

#define LOOKUP_TICKS (30 * 60L)   /* give up on a name after 30 seconds */
#define SLOTS 4

/* Each lookup gets its own slot: one that was given up on may still be answered
   later, into its own slot, and isn't mistaken for the current one */
static struct { hostInfo info; char name[256]; } slots[SLOTS];
static volatile long answered = -1;
static long lookups = 0;

static pascal void resolve_result(hostInfo *info, char *user)
{
    (void)info;
    answered = (long)user;  /* called at interrupt time: only note which lookup finished */
}

static Boolean parse_dotted(const char *s, ip_addr *out)
{
    unsigned long parts[4]; int n = 0;
    while (n < 4) {
        unsigned long v = 0; int digits = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); digits++; }
        if (!digits || v > 255) return false;
        parts[n++] = v;
        if (n < 4) { if (*s != '.') return false; s++; }
    }
    if (*s) return false;
    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

OSErr net_resolve(const char *host, ip_addr *addr)
{
    long id = lookups++, start = TickCount();
    int k = (int)(id % SLOTS);
    OSErr err;
    if ((err = net_open()) != noErr) return err;
    if (parse_dotted(host, addr)) return noErr;
    if (!net_dnr) return noNameServer;
    memset(&slots[k].info, 0, sizeof slots[k].info);
    strncpy(slots[k].name, host, 255);
    slots[k].name[255] = 0;
    err = ((dnr_proc)*net_dnr)(STRTOADDR, slots[k].name, &slots[k].info, (ProcPtr)resolve_result, (char *)id);
    if (err == cacheFault) {
        while (answered != id) {
            SystemTask();
            if (net_idle && net_idle()) return userCanceledErr;
            if (TickCount() - start > LOOKUP_TICKS) return commandTimeout;
        }
        err = (OSErr)slots[k].info.rtnCode;
    }
    if (err == noErr) *addr = slots[k].info.addr[0];
    return err;
}

/* ------------------------------------------------------------ ICMP echo */

/* The layout MacTCP expects (68K alignment): parameters at 28, the reply's times at 50 */
typedef char echo_dest_at_28[offsetof(ICMPParamBlock, cs.echo.dest) == 28 ? 1 : -1];
typedef char echo_notify_at_46[offsetof(ICMPParamBlock, cs.echo.icmpCompletion) == 46 ? 1 : -1];
typedef char reply_at_50[offsetof(ICMPParamBlock, cs.reply.echoRequestOut) == 50 ? 1 : -1];

/* The call, and how it ended. The call's own ioResult goes to 0 once the request is sent;
   MacTCP says how it went only when it calls icmpCompletion, with a block of its own whose
   ioResult is noErr or icmpEchoTimeoutErr, and which carries the call's userDataPtr back:
   this record, so the routine needs nothing else at interrupt time. */
typedef struct {
    ICMPParamBlock pb;
    volatile short ended;
    volatile OSErr result;
} EchoCall;

/* Here, in a segment that stays: MacTCP may end a call given up on after the Start
   segment has gone, so its record and completion routine stay too */
static EchoCall echo = { { { 0 } }, 1, 0 };

/* A C routine (MiscIPPB.h, kCStackBased), though MacTCP.p says Pascal: one that pops its
   argument as Pascal does crashes MacTCP */
static void echo_done(void *iopb)
{
    ICMPParamBlock *r = iopb;
    EchoCall *c = (EchoCall *)r->cs.reply.userDataPtr;
    if (c != &echo) return;
    c->result = r->ioResult;
    c->ended = 1;
}

OSErr net_ping(ip_addr addr, short secs)
{
    static char data[] = "Mill";
    long start = TickCount();
    OSErr err;
    if (!echo.ended) return commandTimeout;     /* the last one is still out */
    if ((err = net_open()) != noErr) return err;
    memset(&echo, 0, sizeof echo);
    echo.pb.ioCRefNum = net_ipp;
    echo.pb.csCode = ipctlEchoICMP;
    echo.pb.cs.echo.dest = addr;
    echo.pb.cs.echo.data.length = sizeof data - 1;
    echo.pb.cs.echo.data.ptr = data;
    echo.pb.cs.echo.timeout = secs;
    echo.pb.cs.echo.icmpCompletion = echo_done;
    echo.pb.cs.echo.userDataPtr = (long)&echo;
    if ((err = PBControlAsync((ParmBlkPtr)&echo.pb)) != noErr || echo.pb.ioResult < 0) {
        echo.ended = 1;     /* refused: icmpCompletion won't be called */
        return err != noErr ? err : echo.pb.ioResult;
    }
    /* MacTCP ends it after secs; a little longer, in case it doesn't */
    while (!echo.ended) {
        SystemTask();
        if (TickCount() - start > (secs + 2) * 60L) break;
    }
    if (!echo.ended) return icmpEchoTimeoutErr;
    return echo.result;
}

/* ------------------------------------------------------------ setup */

/* MacTCP's driver isn't closed: other programs share it. The resolver, Mill's own
   copy, is closed if Mill opened it. */
void net_shutdown(void)
{
    net_close_wait(true);       /* their buffers are in Mill's heap */
    if (net_dnr) {
        ((dnr_proc)*net_dnr)(CLOSERESOLVER);
        DisposeHandle(net_dnr);
        net_dnr = NULL;
    }
}

/* ------------------------------------------------------------ TCP */

static void closing_idle(void);

/* A MacTCP call. Ones that wait on the network (opening, sending, receiving, closing) run
   in the background while the sails turn; if net_idle asks to stop, the connection is
   aborted, which ends the call, and it returns userCanceledErr. */
static OSErr tcp_call(TCPiopb *pb, short code)
{
    pb->ioCRefNum = net_ipp;
    pb->csCode = code;
    if (code == TCPCreate || code == TCPRelease || code == TCPAbort) return PBControlSync((ParmBlkPtr)pb);
    PBControlAsync((ParmBlkPtr)pb);
    while (pb->ioResult > 0) {
        SystemTask();
        closing_idle();
        if (net_idle && net_idle()) {
            TCPiopb abort;
            memset(&abort, 0, sizeof abort);
            abort.ioCRefNum = net_ipp;
            abort.csCode = TCPAbort;
            abort.tcpStream = pb->tcpStream;
            PBControlSync((ParmBlkPtr)&abort);
            while (pb->ioResult > 0) SystemTask();  /* pb is on the caller's stack: wait for MacTCP to let go */
            return userCanceledErr;
        }
    }
    return pb->ioResult;
}

/* ------------------------------------------------------------ connections */

struct NetConn {
    StreamPtr stream;
    Ptr rcv_buffer;
    TCPiopb *wait;          /* a listen's open, still going */
};

static void conn_free(NetConn *c)
{
    TCPiopb pb;
    if (c->stream) {
        memset(&pb, 0, sizeof pb);
        pb.tcpStream = c->stream;
        tcp_call(&pb, TCPRelease);
    }
    if (c->wait) { while (c->wait->ioResult > 0) SystemTask(); DisposePtr((Ptr)c->wait); }
    if (c->rcv_buffer) DisposePtr(c->rcv_buffer);
    DisposePtr((Ptr)c);
}

static OSErr conn_new(NetConn **out)
{
    TCPiopb pb;
    NetConn *c;
    OSErr err;
    *out = NULL;
    closing_idle();
    if (!(c = (NetConn *)NewPtrClear(sizeof *c))) return memFullErr;
    /* short of room: the closes going on hold some */
    while (!(c->rcv_buffer = NewPtr(RCV_BUFFER_SIZE)))
        if (!net_close_wait(false)) { DisposePtr((Ptr)c); return memFullErr; }
    memset(&pb, 0, sizeof pb);
    pb.csParam.create.rcvBuff = c->rcv_buffer;
    pb.csParam.create.rcvBuffLen = RCV_BUFFER_SIZE;
    if ((err = tcp_call(&pb, TCPCreate)) != noErr) { conn_free(c); return err; }
    c->stream = pb.tcpStream;
    *out = c;
    return noErr;
}

OSErr net_tcp_open(const char *host, unsigned short port, NetConn **out)
{
    TCPiopb pb;
    ip_addr addr;
    OSErr err;
    *out = NULL;
    net_stage = NET_LOOKUP;
    if ((err = net_open()) != noErr) return err;
    status("Looking up host\311");
    if ((err = net_resolve(host, &addr)) != noErr) return err;
    if ((err = conn_new(out)) != noErr) return err;
    memset(&pb, 0, sizeof pb);
    pb.tcpStream = (*out)->stream;
    pb.csParam.open.ulpTimeoutValue = TIMEOUT;
    pb.csParam.open.ulpTimeoutAction = 1;           /* abort */
    pb.csParam.open.validityFlags = 0xC0;           /* both of the above are set */
    pb.csParam.open.commandTimeoutValue = TIMEOUT;
    pb.csParam.open.remoteHost = addr;
    pb.csParam.open.remotePort = port;
    net_stage = NET_CONNECT;
    status("Connecting\311");
    if ((err = tcp_call(&pb, TCPActiveOpen)) != noErr) { conn_free(*out); *out = NULL; return err; }
    net_stage = NET_TRANSFER;
    return noErr;
}

OSErr net_tcp_send(NetConn *c, const void *data, long len)
{
    TCPiopb pb;
    wdsEntry wds[2];
    OSErr err = noErr;
    while (len > 0 && err == noErr) {
        long piece = len > 65535L ? 65535L : len;
        wds[0].length = (unsigned short)piece;
        wds[0].ptr = (Ptr)data;
        wds[1].length = 0;
        memset(&pb, 0, sizeof pb);
        pb.tcpStream = c->stream;
        pb.csParam.send.ulpTimeoutValue = TIMEOUT;
        pb.csParam.send.ulpTimeoutAction = 1;
        pb.csParam.send.validityFlags = 0xC0;
        pb.csParam.send.pushFlag = true;
        pb.csParam.send.wdsPtr = (Ptr)wds;
        err = tcp_call(&pb, TCPSend);
        data = (const char *)data + piece;
        len -= piece;
    }
    return err;
}

OSErr net_tcp_recv(NetConn *c, void *buf, long *len)
{
    TCPiopb pb;
    OSErr err;
    memset(&pb, 0, sizeof pb);
    pb.tcpStream = c->stream;
    pb.csParam.receive.commandTimeoutValue = TIMEOUT;
    pb.csParam.receive.rcvBuff = buf;
    pb.csParam.receive.rcvBuffLen = (unsigned short)(*len > 65535L ? 65535L : *len);
    err = tcp_call(&pb, TCPRcv);
    if (err == connectionClosing || err == connectionTerminated) { *len = 0; return noErr; }
    *len = err == noErr ? pb.csParam.receive.rcvBuffLen : 0;
    return err;
}

/* ------------------------------------------------------------ closing */

/* TCPClose ends only once Mill's FIN is acknowledged (MacTCP Programmer's Guide, 1989,
   TCPClose), and a lost FIN is sent again 7 seconds later: waiting held the page that
   long. So closes go on here while Mill carries on, and each stream (and its 16K) is
   released when its close ends. One still waiting after CLOSE_TICKS is aborted, if all
   Mill sent has been acknowledged: TCPSend ends only then (TCPSend, same guide), so
   nothing sent is cut off. */
#define CLOSING 4
#define CLOSE_TICKS 60

static struct { NetConn *c; TCPiopb pb; unsigned long since; } closing[CLOSING];

/* Nothing sent is unacknowledged but the FIN */
static Boolean all_acked(NetConn *c)
{
    TCPiopb pb;
    memset(&pb, 0, sizeof pb);
    pb.ioCRefNum = net_ipp;
    pb.csCode = TCPStatus;
    pb.tcpStream = c->stream;
    if (PBControlSync((ParmBlkPtr)&pb) != noErr) return true;   /* no connection left */
    return pb.csParam.status.amtUnackedData <= 1 && pb.csParam.status.sendNext - pb.csParam.status.sendUnacked <= 1;
}

/* Slot i: released if its close has ended; aborted first if it has waited long enough
   (force: whatever is unacknowledged) */
static Boolean closing_check(short i, Boolean force)
{
    NetConn *c = closing[i].c;
    if (!c) return false;
    if (closing[i].pb.ioResult > 0 && TickCount() - closing[i].since >= CLOSE_TICKS) {
        if (force || all_acked(c)) {
            TCPiopb abort;
            memset(&abort, 0, sizeof abort);
            abort.ioCRefNum = net_ipp;
            abort.csCode = TCPAbort;
            abort.tcpStream = c->stream;
            PBControlSync((ParmBlkPtr)&abort);
            while (closing[i].pb.ioResult > 0) SystemTask();
        } else closing[i].since = TickCount();     /* ask again later */
    }
    if (closing[i].pb.ioResult > 0) return false;
    closing[i].c = NULL;
    conn_free(c);
    return true;
}

static void closing_idle(void)
{
    short i;
    for (i = 0; i < CLOSING; i++) closing_check(i, false);
}

Boolean net_close_wait(Boolean quitting)
{
    short i;
    Boolean any = false;
    for (i = 0; i < CLOSING; i++) {
        if (!closing[i].c) continue;
        while (!closing_check(i, quitting)) SystemTask();
        any = true;
    }
    return any;
}

void net_tcp_close(NetConn *c)
{
    short i;
    if (!c) return;
    if (c->wait) { conn_free(c); return; }
    closing_idle();
    for (i = 0; i < CLOSING && closing[i].c; i++) {}
    if (i == CLOSING) {
        net_close_wait(false);
        i = 0;
    }
    memset(&closing[i].pb, 0, sizeof closing[i].pb);
    closing[i].pb.ioCRefNum = net_ipp;
    closing[i].pb.csCode = TCPClose;
    closing[i].pb.tcpStream = c->stream;
    closing[i].pb.csParam.close.ulpTimeoutValue = TIMEOUT;
    closing[i].pb.csParam.close.validityFlags = 0x80;
    closing[i].since = TickCount();
    closing[i].c = c;
    PBControlAsync((ParmBlkPtr)&closing[i].pb);
    closing_check(i, false);    /* it may have ended already (the connection was gone) */
}

OSErr net_tcp_listen(NetConn **out, ip_addr *addr, unsigned short *port)
{
    static unsigned short next = 0;
    GetAddrParamBlock ga;
    NetConn *c;
    OSErr err;
    *out = NULL;
    if ((err = net_open()) != noErr) return err;
    memset(&ga, 0, sizeof ga);
    ga.ioCRefNum = net_ipp;
    ga.csCode = ipctlGetAddr;
    if ((err = PBControlSync((ParmBlkPtr)&ga)) != noErr) return err;
    *addr = ga.ourAddress;
    if ((err = conn_new(&c)) != noErr) return err;
    if (!(c->wait = (TCPiopb *)NewPtrClear(sizeof(TCPiopb)))) { conn_free(c); return memFullErr; }
    /* a port of Mill's own choosing, from the range kept for such things */
    if (!next) next = (unsigned short)(TickCount() & 0x0FFF);
    *port = (unsigned short)(49152U + (next++ & 0x3FFF));
    c->wait->ioCRefNum = net_ipp;
    c->wait->csCode = TCPPassiveOpen;
    c->wait->tcpStream = c->stream;
    c->wait->csParam.open.ulpTimeoutValue = TIMEOUT;
    c->wait->csParam.open.ulpTimeoutAction = 1;
    c->wait->csParam.open.validityFlags = 0xC0;
    c->wait->csParam.open.commandTimeoutValue = TIMEOUT;
    c->wait->csParam.open.localPort = *port;
    PBControlAsync((ParmBlkPtr)c->wait);
    if (c->wait->ioResult < 0) { err = c->wait->ioResult; conn_free(c); return err; }
    *out = c;
    return noErr;
}

OSErr net_tcp_accept(NetConn *c)
{
    OSErr err;
    if (!c->wait) return noErr;
    while (c->wait->ioResult > 0) {
        SystemTask();
        if (net_idle && net_idle()) {
            TCPiopb abort;
            memset(&abort, 0, sizeof abort);
            abort.ioCRefNum = net_ipp;
            abort.csCode = TCPAbort;
            abort.tcpStream = c->stream;
            PBControlSync((ParmBlkPtr)&abort);
            return userCanceledErr;
        }
    }
    err = c->wait->ioResult;
    DisposePtr((Ptr)c->wait);
    c->wait = NULL;
    return err;
}

/* ------------------------------------------------------------ kept connections */

#define KEPT 8                  /* windows with one kept, at most; then the oldest goes */
#define QUIET_TICKS 60          /* closing a kept one: a second to say goodbye */

long net_keep_secs = 300;

typedef struct {
    const void *owner;
    NetConn *conn;
    unsigned long since;        /* when it was last used */
    long nlen, blen;
    char *key;                  /* the key, a 0, the note, then bye */
} Kept;
static Kept kept[KEPT];

/* A call made while no request runs (Quit, closing a window): it waits for no one, and
   gives up after ticks, aborting the connection */
static OSErr quiet_call(TCPiopb *pb, short code, long ticks)
{
    unsigned long start = TickCount();
    pb->ioCRefNum = net_ipp;
    pb->csCode = code;
    PBControlAsync((ParmBlkPtr)pb);
    while (pb->ioResult > 0) {
        SystemTask();
        if (TickCount() - start > (unsigned long)ticks) {
            TCPiopb abort;
            memset(&abort, 0, sizeof abort);
            abort.ioCRefNum = net_ipp;
            abort.csCode = TCPAbort;
            abort.tcpStream = pb->tcpStream;
            PBControlSync((ParmBlkPtr)&abort);
            while (pb->ioResult > 0) SystemTask();
            return commandTimeout;
        }
    }
    return pb->ioResult;
}

/* Still open both ways, and nothing has come since: a server that's given up says so
   first (FTP's 421, IMAP's BYE) or closes */
static Boolean usable(NetConn *c)
{
    TCPiopb pb;
    memset(&pb, 0, sizeof pb);
    pb.ioCRefNum = net_ipp;
    pb.csCode = TCPStatus;
    pb.tcpStream = c->stream;
    if (PBControlSync((ParmBlkPtr)&pb) != noErr) return false;
    return pb.csParam.status.connectionState == 8 && pb.csParam.status.amtUnreadData == 0;
}

static Boolean expired(const Kept *k)
{
    return net_keep_secs <= 0 || TickCount() - k->since > (unsigned long)net_keep_secs * 60;
}

/* Close a kept one: its goodbye (if it's still open), then the connection */
static void drop(Kept *k)
{
    TCPiopb pb;
    NetConn *c = k->conn;
    if (k->blen && usable(c)) {
        wdsEntry wds[2];
        wds[0].length = (unsigned short)k->blen;
        wds[0].ptr = k->key + strlen(k->key) + 1 + k->nlen;
        wds[1].length = 0;
        memset(&pb, 0, sizeof pb);
        pb.tcpStream = c->stream;
        pb.csParam.send.pushFlag = true;
        pb.csParam.send.wdsPtr = (Ptr)wds;
        quiet_call(&pb, TCPSend, QUIET_TICKS);
    }
    net_tcp_close(c);
    DisposePtr(k->key);
    memset(k, 0, sizeof *k);
}

static Kept *kept_of(const void *owner)
{
    short i;
    for (i = 0; i < KEPT; i++) if (kept[i].conn && kept[i].owner == owner) return &kept[i];
    return NULL;
}

NetConn *net_keep_take(const void *owner, const char *key, void *note, long *len)
{
    Kept *k = kept_of(owner);
    NetConn *c;
    long room = *len;
    *len = 0;
    if (!k || strcmp(k->key, key)) return NULL;
    if (expired(k) || !usable(k->conn)) { drop(k); return NULL; }
    *len = k->nlen < room ? k->nlen : room;
    memcpy(note, k->key + strlen(k->key) + 1, *len);
    c = k->conn;
    DisposePtr(k->key);
    memset(k, 0, sizeof *k);
    return c;
}

Boolean net_keep_put(const void *owner, NetConn *c, const char *key, const void *note, long nlen,
                     const void *bye, long blen)
{
    Kept *k = kept_of(owner);
    long klen = strlen(key) + 1;
    short i;
    if (k) drop(k);
    if (net_keep_secs <= 0 || nlen < 0 || nlen > NET_NOTE_MAX || blen < 0 || blen > NET_NOTE_MAX) { net_tcp_close(c); return false; }
    for (i = 0, k = NULL; i < KEPT; i++) {
        if (!kept[i].conn) { k = &kept[i]; break; }
        if (!k || kept[i].since - k->since > 0x80000000UL) k = &kept[i];    /* the oldest */
    }
    if (k->conn) drop(k);
    if (!(k->key = NewPtr(klen + nlen + blen))) { net_tcp_close(c); return false; }
    memcpy(k->key, key, klen);
    memcpy(k->key + klen, note, nlen);
    memcpy(k->key + klen + nlen, bye, blen);
    k->nlen = nlen;
    k->blen = blen;
    k->owner = owner;
    k->conn = c;
    k->since = TickCount();
    return true;
}

Boolean net_keep_close(const void *owner, const char *server)
{
    short i;
    Boolean any = false;
    long n = server ? strlen(server) : 0;
    for (i = 0; i < KEPT; i++) {
        Kept *k = &kept[i];
        if (!k->conn || (owner && k->owner != owner)) continue;
        if (server && !strncmp(k->key, server, n) && k->key[n] == '\r') continue;
        drop(k);
        any = true;
    }
    return any;
}

Boolean net_keep_close_others(const void *owner, const char *server)
{
    short i;
    Boolean any = false;
    long n = strlen(server);
    for (i = 0; i < KEPT; i++) {
        Kept *k = &kept[i];
        if (!k->conn || (k->owner == owner && !strncmp(k->key, server, n) && k->key[n] == '\r')) continue;
        drop(k);
        any = true;
    }
    return any;
}

void net_keep_idle(void)
{
    static unsigned long last;
    short i;
    closing_idle();
    if (TickCount() - last < 120) return;      /* every two seconds is plenty */
    last = TickCount();
    for (i = 0; i < KEPT; i++) if (kept[i].conn && (expired(&kept[i]) || !usable(kept[i].conn))) drop(&kept[i]);
}

#ifdef MILL_MEMREPORT
void memreport_net(void)
{
    memreport_note(net_dnr, "MacTCP resolver code (from its file)", 1);
    memreport_settings();
}
#endif
