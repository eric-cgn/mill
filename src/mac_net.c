/* MacTCP: the .IPP driver for TCP, and MacTCP's own resolver for names. Calls are
   synchronous for now; MacTCP keeps the system running underneath them. */
#include <Multiverse.h>
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

/* ------------------------------------------------------------ setup */

void net_shutdown(void)
{
    if (net_dnr) {
        ((dnr_proc)*net_dnr)(CLOSERESOLVER);
        DisposeHandle(net_dnr);
        net_dnr = NULL;
    }
}

/* ------------------------------------------------------------ TCP */

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
    NetConn *c = (NetConn *)NewPtrClear(sizeof *c);
    OSErr err;
    *out = NULL;
    if (!c) return memFullErr;
    if (!(c->rcv_buffer = NewPtr(RCV_BUFFER_SIZE))) { DisposePtr((Ptr)c); return memFullErr; }
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
    status("Looking up host...");
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
    status("Connecting...");
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

void net_tcp_close(NetConn *c)
{
    TCPiopb pb;
    if (!c) return;
    if (!c->wait) {
        memset(&pb, 0, sizeof pb);
        pb.tcpStream = c->stream;
        pb.csParam.close.ulpTimeoutValue = TIMEOUT;
        pb.csParam.close.validityFlags = 0x80;
        tcp_call(&pb, TCPClose);
    }
    conn_free(c);
}

OSErr net_tcp_listen(NetConn **out, ip_addr *addr, unsigned short *port)
{
    static unsigned short next = 0;
    GetAddrParamBlock ga;
    NetConn *c;
    OSErr err;
    *out = NULL;
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

#ifdef MILL_MEMREPORT
void memreport_net(void)
{
    memreport_note(net_dnr, "MacTCP resolver code (from its file)", 1);
    memreport_settings();
}
#endif
