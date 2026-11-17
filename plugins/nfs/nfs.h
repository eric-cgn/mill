/* nfs.h: what the NFS Plug-In's parts share. Its names are here and only here: NFS_NAME
   for plugin://NAME/ (its pages and icons), and NFS_TITLE, the title on its About page.
   The file's own name ("NFS") is in build.sh and NFS.r. */
#ifndef NFS_H
#define NFS_H

#define NFS_NAME "nfs"
#define NFS_TITLE "NFS"

/* nfs.c: the protocol's description and settings, and its state for a request (the
   caller frees it with FreeMem) */
const MillProtocol *nfs_protocol(void);
MillPlugIn *nfs_state(const MillHost *host);
OSErr nfs_request(MillPlugIn *self, MillRequest *r);

#endif
