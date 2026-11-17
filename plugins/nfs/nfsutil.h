/* nfsutil.h: the parts of the NFS Plug-In's nfs: that need no Mac: XDR (RFC 1832), ONC RPC
   calls and replies (RFC 1831) with AUTH_UNIX, and reading what MOUNT and NFS version 3
   (RFC 1813) send. Plain C, so tests/test_nfs.c runs them on a host. */
#ifndef NFSUTIL_H
#define NFSUTIL_H

/* big-endian numbers, as XDR has them */
#define NFS_GET32(p) (((unsigned long)(p)[0] << 24) | ((unsigned long)(p)[1] << 16) | ((unsigned long)(p)[2] << 8) | (unsigned long)(p)[3])
unsigned char *nfs_put32(unsigned char *p, unsigned long v);
/* opaque or string: its length, its bytes, and 0 to 3 zeros to a multiple of 4 */
unsigned char *nfs_put_opaque(unsigned char *p, const void *s, long n);

enum { kPmapProg = 100000, kNfsProg = 100003, kMountProg = 100005 };

/* An RPC call's header at p: xid, the program, its version, and the procedure, with
   AUTH_UNIX credentials (machine, uid, gid, no other groups) and no verifier. Returns
   where its arguments go. */
unsigned char *nfs_call(unsigned char *p, unsigned long xid, unsigned long prog, unsigned long vers,
                        unsigned long proc, const char *machine, unsigned long uid, unsigned long gid);

/* Reading XDR: each read moves p on; past end, bad is set and reads give 0 */
typedef struct {
    const unsigned char *p, *end;
    short bad;
} NfsIn;
unsigned long nfs_u32(NfsIn *in);
void nfs_skip(NfsIn *in, long n);
/* opaque or string: its bytes (in place) and length */
const unsigned char *nfs_opaque(NfsIn *in, long *len);

/* The reply to call xid in len bytes at p. Its results are in *in if it's 0; else
   kRpcGarbled, kRpcRefused (MSG_DENIED: the credentials or the RPC version), or
   kRpcUnavailable (no such program, version, or procedure). */
enum { kRpcGarbled = 1, kRpcRefused = 2, kRpcUnavailable = 3 };
short nfs_reply(const unsigned char *p, long len, unsigned long xid, NfsIn *in);

/* fattr3, in part: the Unix seconds become seconds since 1904 (UTC) */
enum { kNf3Reg = 1, kNf3Dir = 2 };
typedef struct {
    unsigned long type, size, mtime;    /* size: 0xFFFFFFFF if it's 4 GB or more */
} NfsAttr;
void nfs_fattr(NfsIn *in, NfsAttr *a);
/* post_op_attr: nonzero if the attributes follow (*a set); else *a is cleared */
short nfs_post_op(NfsIn *in, NfsAttr *a);

/* An nfs_fh3 or fhandle3 (up to 64 bytes) */
typedef struct {
    unsigned long len;
    unsigned char data[64];
} NfsFh;
/* Read one (bad if it's too long) */
void nfs_fh(NfsIn *in, NfsFh *fh);

/* One entry of READDIRPLUS's list (entryplus3), or READDIR's (entry3, plus 0: no
   attributes): 1 if there is one, 0 at the list's end */
typedef struct {
    const unsigned char *name;  /* not ended with 0 */
    long len;
    unsigned char cookie[8];
    short attrs;                /* nonzero if a holds its attributes */
    NfsAttr a;
} NfsEntry;
short nfs_entry(NfsIn *in, short plus, NfsEntry *e);

/* One export of MOUNTPROC_EXPORT's list (exportnode): 1 if there is one, 0 at its end */
short nfs_export(NfsIn *in, const unsigned char **dir, long *len);

#endif
