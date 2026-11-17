/* nfsutil.c: XDR, RPC, and the MOUNT and NFS version 3 replies the NFS Plug-In's nfs: reads
   (see nfsutil.h) */
#include <string.h>

#include "nfsutil.h"

/* RFC 1832 3.2 */
unsigned char *nfs_put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
    return p + 4;
}

/* RFC 1832 3.10, 3.11 */
unsigned char *nfs_put_opaque(unsigned char *p, const void *s, long n)
{
    p = nfs_put32(p, n);
    memcpy(p, s, n);
    p += n;
    while (n & 3) { *p++ = 0; n++; }
    return p;
}

/* RFC 1831 8; AUTH_UNIX: RFC 1831 Appendix A */
unsigned char *nfs_call(unsigned char *p, unsigned long xid, unsigned long prog, unsigned long vers,
                        unsigned long proc, const char *machine, unsigned long uid, unsigned long gid)
{
    unsigned char *len;
    long n = strlen(machine);
    p = nfs_put32(p, xid);
    p = nfs_put32(p, 0);            /* CALL */
    p = nfs_put32(p, 2);            /* RPC version 2 */
    p = nfs_put32(p, prog);
    p = nfs_put32(p, vers);
    p = nfs_put32(p, proc);
    p = nfs_put32(p, 1);            /* AUTH_UNIX */
    len = p;
    p = nfs_put32(p + 4, 0);        /* stamp */
    p = nfs_put_opaque(p, machine, n);
    p = nfs_put32(p, uid);
    p = nfs_put32(p, gid);
    p = nfs_put32(p, 0);            /* no other groups */
    nfs_put32(len, p - len - 4);
    p = nfs_put32(p, 0);            /* AUTH_NULL verifier */
    return nfs_put32(p, 0);
}

unsigned long nfs_u32(NfsIn *in)
{
    unsigned long v;
    if (in->bad || in->end - in->p < 4) { in->bad = 1; return 0; }
    v = NFS_GET32(in->p);
    in->p += 4;
    return v;
}

void nfs_skip(NfsIn *in, long n)
{
    if (in->bad || n < 0 || in->end - in->p < n) in->bad = 1;
    else in->p += n;
}

const unsigned char *nfs_opaque(NfsIn *in, long *len)
{
    const unsigned char *s;
    unsigned long n = nfs_u32(in);
    if (n > 0x10000L) in->bad = 1;
    s = in->p;
    nfs_skip(in, (n + 3) & ~3L);
    *len = in->bad ? 0 : (long)n;
    return s;
}

/* RFC 1831 8 */
short nfs_reply(const unsigned char *p, long len, unsigned long xid, NfsIn *in)
{
    unsigned long stat;
    in->p = p;
    in->end = p + len;
    in->bad = 0;
    if (nfs_u32(in) != xid || nfs_u32(in) != 1) return kRpcGarbled;     /* REPLY */
    if (nfs_u32(in) != 0) return in->bad ? kRpcGarbled : kRpcRefused;   /* MSG_ACCEPTED */
    nfs_u32(in);                                                        /* the verifier */
    nfs_opaque(in, &len);
    stat = nfs_u32(in);
    if (in->bad) return kRpcGarbled;
    if (stat == 1 || stat == 2 || stat == 3) return kRpcUnavailable;    /* PROG_UNAVAIL, PROG_MISMATCH, PROC_UNAVAIL */
    return stat ? kRpcGarbled : 0;
}

/* RFC 1813 2.5 */
void nfs_fattr(NfsIn *in, NfsAttr *a)
{
    a->type = nfs_u32(in);
    nfs_skip(in, 16);               /* mode, nlink, uid, gid */
    a->size = nfs_u32(in) ? 0xFFFFFFFFUL : 0;
    a->size |= nfs_u32(in);
    nfs_skip(in, 40);               /* used, rdev, fsid, fileid, atime */
    a->mtime = nfs_u32(in) + 2082844800UL;
    nfs_skip(in, 12);               /* its nanoseconds, ctime */
}

/* RFC 1813 2.5 */
short nfs_post_op(NfsIn *in, NfsAttr *a)
{
    memset(a, 0, sizeof *a);
    if (!nfs_u32(in)) return 0;
    nfs_fattr(in, a);
    return !in->bad;
}

void nfs_fh(NfsIn *in, NfsFh *fh)
{
    long n;
    const unsigned char *s = nfs_opaque(in, &n);
    if (n > 64) in->bad = 1;
    if (in->bad) n = 0;
    fh->len = n;
    memcpy(fh->data, s, n);
}

/* RFC 1813 3.3.16, 3.3.17 */
short nfs_entry(NfsIn *in, short plus, NfsEntry *e)
{
    long n;
    if (!nfs_u32(in)) return 0;
    nfs_skip(in, 8);                /* fileid */
    e->name = nfs_opaque(in, &e->len);
    if (!in->bad && in->end - in->p >= 8) memcpy(e->cookie, in->p, 8);
    nfs_skip(in, 8);
    e->attrs = 0;
    memset(&e->a, 0, sizeof e->a);
    if (plus) {
        e->attrs = nfs_post_op(in, &e->a);
        if (nfs_u32(in)) nfs_opaque(in, &n);   /* its handle */
    }
    return !in->bad;
}

/* RFC 1813 5.2.5 */
short nfs_export(NfsIn *in, const unsigned char **dir, long *len)
{
    long n;
    if (!nfs_u32(in)) return 0;
    *dir = nfs_opaque(in, len);
    while (nfs_u32(in) && !in->bad) nfs_opaque(in, &n);     /* its groups */
    return !in->bad;
}
