/* What Mill needs beyond Multiversal's MacTCP definitions: the resolver MacTCP ships
   as a 'dnrp' resource, as documented in the MacTCP Programmer's Guide. */
#ifndef MILL_MACTCP_H
#define MILL_MACTCP_H

#include <Multiverse.h>

#define NUM_ALT_ADDRS 4
typedef struct {
    long rtnCode;
    char cname[255];
    ip_addr addr[NUM_ALT_ADDRS];
} hostInfo;

#endif
