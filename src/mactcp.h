/* What Mill needs beyond Multiversal's MacTCP definitions: the resolver MacTCP ships
   as a 'dnrp' resource, and the IP driver's echo, as documented in the MacTCP
   Programmer's Guide. */
#ifndef MILL_MACTCP_H
#define MILL_MACTCP_H

#include <Multiverse.h>

#define NUM_ALT_ADDRS 4
typedef struct {
    long rtnCode;
    char cname[255];
    ip_addr addr[NUM_ALT_ADDRS];
} hostInfo;

/* The IP driver's ICMP echo (Apple's MacTCP.p, Universal Interfaces 2.0, 1994, and
   MiscIPPB.h, whose C calling convention for the notify routine is the right one): sent to
   dest with data. The call itself ends once the request is sent; MacTCP then calls
   icmpCompletion with a block of its own, as ICMPParamBlock, when the reply comes (noErr)
   or after timeout seconds (icmpEchoTimeoutErr), with the call's userDataPtr in it.
   (As found with MacTCP 2.0.6 on System 7.5.5, and 1.1.1 on 6.0.3.) */
#define ipctlEchoICMP 17
#define icmpEchoTimeoutErr (-23035)
typedef void (*ICMPEchoNotifyProc)(void *iopb);
typedef struct {
    char fill12[12];
    ProcPtr ioCompletion;
    short ioResult;
    char fill18[6];
    short ioCRefNum;
    short csCode;
    union {
        struct {
            ip_addr dest;
            wdsEntry data;
            short timeout;
            Ptr options;
            unsigned short optLength;
            ICMPEchoNotifyProc icmpCompletion;
            long userDataPtr;
        } echo;
        struct {
            short params[11];
            unsigned long echoRequestOut, echoReplyIn;   /* ticks */
            rdsEntry echoedData;
            Ptr options;
            long userDataPtr;
        } reply;
    } cs;
} ICMPParamBlock;

#endif
