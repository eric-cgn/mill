/* imap.h: what the IMAP Plug-In's parts share. Its names are here and only here: IMAP_NAME
   for plugin://NAME/ (its pages and icons), and IMAP_TITLE, the title on its pages. The
   file's own name ("IMAP") is in build.sh and IMAP.r. */
#ifndef IMAP_H
#define IMAP_H

#define IMAP_NAME "imap"
#define IMAP_TITLE "IMAP"

/* imap.c: the protocol's description and settings, and the state for a request (the caller
   frees it with FreeMem) */
const MillProtocol *imap_protocol(void);
MillPlugIn *imap_state(const MillHost *host);
OSErr imap_request(MillPlugIn *self, MillRequest *r);

#endif
