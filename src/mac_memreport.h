/* The test build's memory report (MILL_MEMREPORT=1 ./build.sh); nothing otherwise */
#ifndef MILL_MEMREPORT_H
#define MILL_MEMREPORT_H

#ifdef MILL_MEMREPORT
#include <Multiverse.h>
/* A block Mill knows, by its pointer (handle 0) or its handle (handle 1) */
void memreport_note(const void *p, const char *label, int handle);
/* What each part of Mill knows it has (mac_main.c, mac_plugin.c, mac_sys.c, mac_net.c) */
void memreport_main(void);
void memreport_plugins(void);
void memreport_sys(void);
void memreport_net(void);
void memreport_settings(void);    /* mac_local.c, by way of memreport_net */
#define memreport_known() (memreport_main(), memreport_plugins(), memreport_sys(), memreport_net())
/* Write "Mill Memory" next to Mill: first starts it again, else it's added to */
void memreport_snapshot(const char *when, Boolean first);
#endif

#endif
