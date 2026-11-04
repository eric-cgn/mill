/* MacTCP: the .IPP driver for TCP, and MacTCP's own resolver for names. Calls are
   synchronous for now; MacTCP keeps the system running underneath them. */
#include <Multiverse.h>
#include <stdio.h>
#include <string.h>

#include "mac_net.h"
#include "mac_sys.h"
#include "version.h"

#define RCV_BUFFER_SIZE 16384
#define TIMEOUT 30  /* seconds */

enum { OPENRESOLVER = 1, CLOSERESOLVER = 2, STRTOADDR = 3 };

typedef OSErr (*dnr_proc)(long selector, ...);

static short ipp = 0;
void (*net_status)(const char *msg) = NULL;
Boolean (*net_idle)(void) = NULL;
int net_stage = NET_LOOKUP;

static void status(const char *msg) { if (net_status) net_status(msg); }
static Handle dnr_code = NULL;

/* ------------------------------------------------------------ the resolver */

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
            short ref;
            Handle h;
            if (file_spec(vref, dir, name, &spec) != noErr) continue;
            ref = file_open_res(&spec, fsRdPerm);
            if (ref == -1) continue;
            h = Get1IndResource('dnrp', 1);
            if (h) { DetachResource(h); MoveHHi(h); HLock(h); }
            CloseResFile(ref);
            if (h) return h;
        }
    }
}

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
    if (!dnr_code) return noNameServer;
    memset(&slots[k].info, 0, sizeof slots[k].info);
    strncpy(slots[k].name, host, 255);
    slots[k].name[255] = 0;
    err = ((dnr_proc)*dnr_code)(STRTOADDR, slots[k].name, &slots[k].info, (ProcPtr)resolve_result, (char *)id);
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

OSErr net_init(void)
{
    OSErr err = OpenDriver("\p.IPP", &ipp);
    if (err != noErr) return err;
    dnr_code = load_dnrp_from(kControlPanelFolderType);
    if (!dnr_code) dnr_code = load_dnrp_from(kSystemFolderType);
    if (!dnr_code) dnr_code = load_dnrp_from(kExtensionFolderType);
    if (dnr_code && ((dnr_proc)*dnr_code)(OPENRESOLVER, (char *)NULL) != noErr) {
        DisposeHandle(dnr_code);
        dnr_code = NULL;
    }
    return noErr;
}

void net_shutdown(void)
{
    if (dnr_code) {
        ((dnr_proc)*dnr_code)(CLOSERESOLVER);
        DisposeHandle(dnr_code);
        dnr_code = NULL;
    }
}

/* ------------------------------------------------------------ TCP */

/* A MacTCP call. Ones that wait on the network (opening, sending, receiving, closing) run
   in the background while the sails turn; if net_idle asks to stop, the connection is
   aborted, which ends the call, and it returns userCanceledErr. */
static OSErr tcp_call(TCPiopb *pb, short code)
{
    pb->ioCRefNum = ipp;
    pb->csCode = code;
    if (code == TCPCreate || code == TCPRelease || code == TCPAbort) return PBControlSync((ParmBlkPtr)pb);
    PBControlAsync((ParmBlkPtr)pb);
    while (pb->ioResult > 0) {
        SystemTask();
        if (net_idle && net_idle()) {
            TCPiopb abort;
            memset(&abort, 0, sizeof abort);
            abort.ioCRefNum = ipp;
            abort.csCode = TCPAbort;
            abort.tcpStream = pb->tcpStream;
            PBControlSync((ParmBlkPtr)&abort);
            while (pb->ioResult > 0) SystemTask();  /* pb is on the caller's stack: wait for MacTCP to let go */
            return userCanceledErr;
        }
    }
    return pb->ioResult;
}

#define BODY_PIECES 8   /* a send's buffers are at most 64K each: a body of up to 512K */

OSErr net_http_request(const char *host, unsigned short port, const char *target, const char *site,
                       const char *headers, const char *content_type, const char *body, long body_len,
                       net_sink sink, void *ref)
{
    TCPiopb pb;
    Ptr rcv_buffer, chunk;
    StreamPtr stream;
    ip_addr addr;
    wdsEntry wds[BODY_PIECES + 2];
    char *request;
    OSErr err;
    long len;
    int w;

    net_stage = NET_LOOKUP;
    status("Looking up host...");
    if ((err = net_resolve(host, &addr)) != noErr) return err;
    if (!(rcv_buffer = NewPtr(RCV_BUFFER_SIZE))) return memFullErr;
    if (!(chunk = NewPtr(2048))) { DisposePtr(rcv_buffer); return memFullErr; }
    if (body_len > 65535L * BODY_PIECES) body_len = 65535L * BODY_PIECES;
    if (!headers) headers = "";
    if (!(request = NewPtr(strlen(target) + strlen(site) + strlen(headers) + 256))) { DisposePtr(chunk); DisposePtr(rcv_buffer); return memFullErr; }

    memset(&pb, 0, sizeof pb);
    pb.csParam.create.rcvBuff = rcv_buffer;
    pb.csParam.create.rcvBuffLen = RCV_BUFFER_SIZE;
    if ((err = tcp_call(&pb, TCPCreate)) != noErr) goto done;
    stream = pb.tcpStream;

    memset(&pb.csParam, 0, sizeof pb.csParam);
    pb.tcpStream = stream;
    pb.csParam.open.ulpTimeoutValue = TIMEOUT;
    pb.csParam.open.ulpTimeoutAction = 1;           /* abort */
    pb.csParam.open.validityFlags = 0xC0;           /* both of the above are set */
    pb.csParam.open.commandTimeoutValue = TIMEOUT;
    pb.csParam.open.remoteHost = addr;
    pb.csParam.open.remotePort = port;
    net_stage = NET_CONNECT;
    status("Connecting...");
    if ((err = tcp_call(&pb, TCPActiveOpen)) != noErr) goto release;

    /* An HTTP/1.0 request, as browsers of 1996 sent them */
    strcpy(request, content_type ? "POST " : "GET ");
    strcat(request, target);
    strcat(request, " HTTP/1.0\r\nAccept: */*\r\nHost: ");
    strcat(request, site);
    if (headers[0]) {
        strcat(request, "\r\n");
        strcat(request, headers);
    }
    if (content_type) {
        char *end = request + strlen(request);
        sprintf(end, "\r\nContent-Type: %.100s\r\nContent-Length: %ld", content_type, body_len);
    }
    strcat(request, "\r\n\r\n");
    wds[0].length = strlen(request);
    wds[0].ptr = request;
    for (w = 1, len = 0; content_type && len < body_len; w++) {
        long piece = body_len - len > 65535L ? 65535L : body_len - len;
        wds[w].length = (unsigned short)piece;
        wds[w].ptr = (Ptr)body + len;
        len += piece;
    }
    wds[w].length = 0;
    memset(&pb.csParam, 0, sizeof pb.csParam);
    pb.tcpStream = stream;
    pb.csParam.send.ulpTimeoutValue = TIMEOUT;
    pb.csParam.send.ulpTimeoutAction = 1;
    pb.csParam.send.validityFlags = 0xC0;
    pb.csParam.send.pushFlag = true;
    pb.csParam.send.wdsPtr = (Ptr)wds;
    if ((err = tcp_call(&pb, TCPSend)) != noErr) goto release;
    net_stage = NET_TRANSFER;
    status("Waiting for reply...");

    for (;;) {
        memset(&pb.csParam, 0, sizeof pb.csParam);
        pb.tcpStream = stream;
        pb.csParam.receive.commandTimeoutValue = TIMEOUT;
        pb.csParam.receive.rcvBuff = chunk;
        pb.csParam.receive.rcvBuffLen = 2048;
        err = tcp_call(&pb, TCPRcv);
        if (err != noErr) break;
        len = pb.csParam.receive.rcvBuffLen;
        if (len > 0 && !sink(chunk, len, ref)) break;
    }
    if (err == connectionClosing || err == connectionTerminated) err = noErr;

    memset(&pb.csParam, 0, sizeof pb.csParam);
    pb.tcpStream = stream;
    pb.csParam.close.ulpTimeoutValue = TIMEOUT;
    pb.csParam.close.validityFlags = 0x80;
    tcp_call(&pb, TCPClose);

release:
    memset(&pb.csParam, 0, sizeof pb.csParam);
    pb.tcpStream = stream;
    tcp_call(&pb, TCPRelease);
done:
    DisposePtr(request);
    DisposePtr(chunk);
    DisposePtr(rcv_buffer);
    return err;
}
