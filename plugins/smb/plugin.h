/* plugin.h: what the SMB Plug-In's parts share. Its names are here and only here: SMB_NAME
   for plugin://NAME/ (its pages and icons), and SMB_TITLE, the title on its About page.
   The file's own name ("SMB") is in build.sh and SMB.r. */
#ifndef PLUGIN_H
#define PLUGIN_H

#define SMB_NAME "smb"
#define SMB_TITLE "SMB"

/* smb.c: the protocol's description and settings, and its state for a request (the
   caller frees it with FreeMem) */
const MillProtocol *smb_protocol(void);
MillPlugIn *smb_state(const MillHost *host);
OSErr smb_request(MillPlugIn *self, MillRequest *r);

#endif
