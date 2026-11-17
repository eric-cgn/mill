/* nfs: in the NFS Plug-In: NFS version 3 (RFC 1813) over TCP, with addresses as RFC 2224 has
   them, read-only.
     nfs://host/                the server's exports (MOUNTPROC_EXPORT)
     nfs://host/export/path/    a folder, as a page (MNT, LOOKUP, then READDIRPLUS)
     nfs://host/export/path     a file, as it is (READ a piece at a time, from the offset
                                a Range asks for)
   The portmapper (port 111) gives MOUNT's and NFS's ports. Without one, NFS is on the
   address's port or 2049, and MOUNT on the Mount port setting. Each call carries AUTH_UNIX
   credentials: the User ID and Group ID settings, from "Macintosh". Names are shown as
   UTF-8 if they are, else Latin-1; times in the Map control panel's zone. The connection to
   NFS is kept between pages (TcpKeep) with the export's handle, so another page in the
   same export needs neither the portmapper nor MOUNT. */
#include <stddef.h>
#include <string.h>
#include <OSUtils.h>

#include "MillPlugIn.h"
#include "nfs.h"
#include "nfsutil.h"

enum {
    BUF = 4400,             /* one call or reply */
    CHUNK = 4096,           /* a read, and the most a folder's reply may hold */
    PATHN = 512,
    NAMEN = 256,
    ARENA = 16384           /* a folder's entries, sorted before they're shown */
};
enum { kLost = -30201, kGarbled = -30202 };
enum { kNfsVersion = 3, kTcp = 6 };
/* procedures: RFC 1833 3.1; RFC 1813 5.2, 3.3 */
enum { kGetPort = 3, kMnt = 1, kExport = 5, kGetAttr = 1, kLookup = 3, kRead = 6, kReadDir = 16, kReadDirPlus = 17 };

typedef struct {
    MillPlugIn base;
    MillReqRef r;
    MillConn c;
    unsigned short port;        /* c's */
    short stopped;
    unsigned long xid, uid, gid;
    long tz;                    /* seconds east of UTC */
    NfsIn in;                   /* the last reply's results */
    NfsFh fh;
    char host[NAMEN];
    char path[PATHN];           /* the address's, decoded: "/export/public/Projects/" */
    char esc[NAMEN * 6];
    unsigned char buf[BUF];     /* 4 bytes of record mark, then the message */
} Nfs;

#define H (me->base.host)

/* ------------------------------------------------------------ the page */

static void out(Nfs *me, const void *s, long n)
{
    if (!me->stopped && n > 0 && !H->Write(me->r, s, n)) me->stopped = 1;
}

static void outs(Nfs *me, const char *s) { out(me, s, strlen(s)); }

static void outf(Nfs *me, const char *fmt, long a, long b)
{
    long args[2];
    args[0] = a;
    args[1] = b;
    if (!me->stopped && !H->Printf(me->r, fmt, args)) me->stopped = 1;
}

/* n bytes of a name from the server, as Mac Roman made safe for HTML */
static void out_name(Nfs *me, const unsigned char *s, long n)
{
    char m[NAMEN];
    if (n > NAMEN - 1) n = NAMEN - 1;
    if (H->ToMacRoman((const char *)s, n, millGuess, m, sizeof m) >= 0 && H->HtmlEscape(m, me->esc, sizeof me->esc) >= 0)
        outs(me, me->esc);
}

/* A line of the page: an icon, and a link to n bytes of name (a folder's with a slash) */
static void link(Nfs *me, short id, const unsigned char *name, long n, short folder)
{
    char s[NAMEN], href[NAMEN * 3];
    memcpy(s, name, n);
    s[n] = 0;
    if (H->UrlEncode(s, href, sizeof href - 2) < 0) return;
    if (folder) strcat(href, "/");
    outf(me, "<IMG SRC=\"plugin://" NFS_NAME "/rsrc?type=ics%%23&id=%d\" WIDTH=16 HEIGHT=16 ALIGN=TOP> <A HREF=\"%s\">", id, (long)href);
    out_name(me, name, n);
    outs(me, "</A>");
}

static void page_head(Nfs *me)
{
    H->PlugInPage(me->r);
    outs(me, "HTTP/1.0 200 OK" MILL_CRLF "Content-Type: text/html; charset=macintosh" MILL_CRLF MILL_CRLF "<TITLE>NFS: ");
    out_name(me, (const unsigned char *)me->host, strlen(me->host));
    outs(me, "</TITLE><H2>");
    out_name(me, (const unsigned char *)me->host, strlen(me->host));
    out_name(me, (const unsigned char *)me->path, strlen(me->path));
    outs(me, "</H2><P>");
}

/* What a MOUNT or NFS status other than 0 means, for an alert (missing: what to say when
   there's no such thing) */
/* RFC 1813 2.6; mountstat3: RFC 1813 5 */
static void refused(Nfs *me, unsigned long st, const char *missing)
{
    char text[64];
    long a[1];
    const char *t = missing;
    if (st == 1 || st == 13) t = "The server refused access.";
    else if (st == 20) t = "No such folder.";
    else if (st != 2) {
        a[0] = st;
        H->Format(text, sizeof text, "The server refused it (error %d).", a);
        t = text;
    }
    H->Fail(me->r, t);
}

/* The Map control panel's offset from UTC (see Internet.c's map_offset) */
static long map_offset(void)
{
    unsigned char loc[12];
    long delta, err;
    if (GetTrapAddress(0xA051) == GetTrapAddress(0xA89F)) return 0;
    memset(loc, 0, sizeof loc);
    __asm__ volatile (
        "move.l %1,%%a0\n\t"
        "move.l #0x000C00E4,%%d0\n\t"
        ".short 0xA051\n\t"
        "move.l %%d0,%0"
        : "=r"(err) : "g"(loc) : "d0", "d1", "d2", "a0", "a1", "cc", "memory");
    if ((short)err != 0) return 0;
    delta = ((long)loc[9] << 16) | ((long)loc[10] << 8) | loc[11];
    if (delta & 0x800000L) delta -= 0x1000000L;
    return delta;
}

/* ------------------------------------------------------------ calls */

/* a setting's number */
static unsigned long number(const char *s)
{
    unsigned long n = 0;
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return n;
}

/* A connection to port (the one open already, if it's to the same) */
static OSErr connect(Nfs *me, unsigned short port)
{
    OSErr err;
    if (me->c) {
        if (me->port == port) return noErr;
        H->TcpClose(me->c);
        me->c = 0;
    }
    if ((err = H->TcpOpen(me->r, me->host, port, &me->c)) != noErr) me->c = 0;
    me->port = port;
    return err;
}

/* Start a call to prog's proc: returns where its arguments go */
static unsigned char *rpc(Nfs *me, unsigned long prog, unsigned long proc)
{
    return nfs_call(me->buf + 4, ++me->xid, prog, prog == kPmapProg ? 2 : kNfsVersion, proc, "Macintosh", me->uid, me->gid);
}

static OSErr recv_n(Nfs *me, unsigned char *p, long n)
{
    while (n > 0) {
        long k = n;
        OSErr err = H->TcpRecv(me->c, p, &k);
        if (err != noErr) return err;
        if (!k) return kLost;
        p += k;
        n -= k;
    }
    return noErr;
}

/* Send the call begun with rpc, its arguments ending at end, and read the reply into in.
   1 if it failed with an alert. */
static OSErr call(Nfs *me, unsigned char *end)
{
    unsigned long mark;
    long n = 0;
    OSErr err;
    nfs_put32(me->buf, 0x80000000UL | (unsigned long)(end - me->buf - 4));
    if ((err = H->TcpSend(me->c, me->buf, end - me->buf)) != noErr) return err;
    do {                        /* the reply's fragments, one after another: RFC 1831 10 */
        if ((err = recv_n(me, me->buf + n, 4)) != noErr) return err;
        mark = NFS_GET32(me->buf + n);
        if ((mark & 0x7FFFFFFFUL) > (unsigned long)(BUF - n)) return kGarbled;
        if ((err = recv_n(me, me->buf + n, mark & 0x7FFFFFFFUL)) != noErr) return err;
        n += mark & 0x7FFFFFFFUL;
    } while (!(mark & 0x80000000UL));
    switch (nfs_reply(me->buf, n, me->xid, &me->in)) {
    case 0: return noErr;
    case kRpcRefused: H->Fail(me->r, "The server refused access."); return 1;
    case kRpcUnavailable: H->Fail(me->r, "The server doesn\325t support NFS version 3."); return 1;
    }
    return kGarbled;
}

/* A status, first of a MOUNT or NFS reply: nonzero failed with an alert */
static short status(Nfs *me, const char *missing)
{
    unsigned long st = nfs_u32(&me->in);
    if (st) refused(me, st, missing);
    return st != 0;
}

/* MOUNT's and NFS's ports, from the portmapper if there is one: RFC 1833 3 */
static OSErr ports(Nfs *me, MillRequest *r, unsigned short *mount, unsigned short *nfs)
{
    char v[8];
    unsigned short *at[2];
    unsigned long prog[2], port;
    short i;
    OSErr err;
    *mount = H->GetSetting("nfs", "mountport", v, sizeof v) > 0 ? (unsigned short)number(v) : 2049;
    *nfs = r->port ? r->port : 2049;
    if ((err = connect(me, 111)) != noErr) return err == userCanceledErr ? err : noErr;
    at[0] = mount; prog[0] = kMountProg;
    at[1] = nfs; prog[1] = kNfsProg;
    for (i = 0; i < 2 - (r->port != 0); i++) {
        unsigned char *p = rpc(me, kPmapProg, kGetPort);
        p = nfs_put32(p, prog[i]);
        p = nfs_put32(p, kNfsVersion);
        p = nfs_put32(p, kTcp);
        if ((err = call(me, nfs_put32(p, 0))) != noErr) return err;
        port = nfs_u32(&me->in);
        if (me->in.bad) return kGarbled;
        if (!port || port > 65535L) { H->Fail(me->r, "The server doesn\325t support NFS version 3."); return 1; }
        *at[i] = (unsigned short)port;
    }
    return noErr;
}

/* ------------------------------------------------------------ exports */

/* The exports: as a page (to is 0), or the longest that the path starts with, mounted:
   its handle in fh, and its length returned in *to */
static OSErr exports(Nfs *me, long *to)
{
    const unsigned char *dir;
    long n, best = -1;
    OSErr err;
    if ((err = call(me, rpc(me, kMountProg, kExport))) != noErr) return err;
    if (!to) page_head(me);
    while (nfs_export(&me->in, &dir, &n)) {
        if (!to) {
            if (n > 1 && n < NAMEN && dir[0] == '/') {
                link(me, kMillIconDisk, dir + 1, n - 1, 1);
                outs(me, "<BR>\n");
            }
        } else if (n > best && n < PATHN && !memcmp(dir, me->path, n) &&
                   (!me->path[n] || me->path[n] == '/' || dir[n - 1] == '/'))
            best = n;
    }
    if (!to) return noErr;
    if (me->in.bad) return kGarbled;
    if (best < 0) { H->Fail(me->r, "No such export."); return 1; }
    /* MNT: RFC 1813 5.2.1 */
    if ((err = call(me, nfs_put_opaque(rpc(me, kMountProg, kMnt), me->path, best))) != noErr ||
        status(me, "No such export.")) return err ? err : 1;
    nfs_fh(&me->in, &me->fh);
    if (me->in.bad) return kGarbled;
    *to = best;
    return noErr;
}

/* ------------------------------------------------------------ folders and files */

typedef struct {
    unsigned long size, when;
    unsigned char folder, known, len;   /* known: its size, when, and folder are */
    char name[1];
} Item;
/* its room in the block, from the start of one to the next */
#define ITEM_SIZE(n) ((offsetof(Item, name) + (n) + sizeof(long) - 1) & ~(sizeof(long) - 1))

static short icon_for(const char *name)
{
    const char *dot = strrchr(name, '.');
    char e[9];
    short i;
    for (i = 0; i < 8 && dot && dot[i + 1]; i++) e[i] = (char)(dot[i + 1] | 32);
    e[i] = 0;
    if (!strcmp(e, "txt") || !strcmp(e, "asc") || !strcmp(e, "html") || !strcmp(e, "htm") || !strcmp(name, "README"))
        return kMillIconText;
    if (!strcmp(e, "gif") || !strcmp(e, "jpg") || !strcmp(e, "jpeg") || !strcmp(e, "pict") || !strcmp(e, "pbm") ||
        !strcmp(e, "pgm") || !strcmp(e, "ppm")) return kMillIconPicture;
    if (!strcmp(e, "hqx") || !strcmp(e, "sit") || !strcmp(e, "bin") || !strcmp(e, "zip") || !strcmp(e, "cpt") ||
        !strcmp(e, "sea") || !strcmp(e, "gz") || !strcmp(e, "tar") || !strcmp(e, "z")) return kMillIconBinary;
    return kMillIconDocument;
}

/* folders first, then by name, in any case */
static short before(const Item *a, const Item *b)
{
    short i;
    if (a->folder != b->folder) return a->folder;
    for (i = 0; i < a->len && i < b->len; i++) {
        unsigned char x = (unsigned char)a->name[i], y = (unsigned char)b->name[i];
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return x < y;
    }
    return a->len < b->len;
}

/* the entries gathered, sorted, as lines of the page. Those READDIR gave (not READDIRPLUS)
   are looked up first, for their attributes. */
static OSErr show_items(Nfs *me, char *arena, unsigned short *at, short n)
{
    short i, j, gap;
    char when[40];
    long a[5];
    NfsFh fh;
    NfsAttr attr;
    OSErr err;
    for (i = 0; i < n && !me->stopped; i++) {
        Item *it = (Item *)(arena + at[i]);
        if (it->known) continue;
        if ((err = call(me, nfs_put_opaque(nfs_put_opaque(rpc(me, kNfsProg, kLookup), me->fh.data, me->fh.len), it->name, it->len))) != noErr)
            return err;
        if (nfs_u32(&me->in)) continue;
        nfs_fh(&me->in, &fh);
        nfs_post_op(&me->in, &attr);
        it->size = attr.size;
        it->when = attr.mtime;
        it->folder = attr.type == kNf3Dir;
    }
    for (gap = n / 2; gap > 0; gap /= 2)        /* Shell's sort */
        for (i = gap; i < n; i++)
            for (j = i - gap; j >= 0 && before((Item *)(arena + at[j + gap]), (Item *)(arena + at[j])); j -= gap) {
                unsigned short t = at[j];
                at[j] = at[j + gap];
                at[j + gap] = t;
            }
    for (i = 0; i < n && !me->stopped; i++) {
        Item *it = (Item *)(arena + at[i]);
        DateTimeRec d;
        char name[NAMEN];
        memcpy(name, it->name, it->len);
        name[it->len] = 0;
        link(me, it->folder ? kMillIconFolder : icon_for(name), (const unsigned char *)it->name, it->len, it->folder);
        if (!it->folder) outf(me, " &nbsp; %ldK", (long)((it->size + 1023) / 1024), 0);
        if (it->when) {
            SecondsToDate(it->when + me->tz, &d);
            a[0] = d.year; a[1] = d.month; a[2] = d.day; a[3] = d.hour; a[4] = d.minute;
            H->Format(when, sizeof when, " &nbsp; %04d-%02d-%02d %02d:%02d", a);
            outs(me, when);
        }
        outs(me, "<BR>\n");
    }
    return noErr;
}

/* A folder: READDIRPLUS, or READDIR if the server doesn't do it, from cookie to cookie:
   RFC 1813 3.3.17, 3.3.16 */
static OSErr folder(Nfs *me, short root)
{
    unsigned char cookie[16], *p;
    unsigned short *at;
    short n = 0, eof = 0, shown = 0, plus = 1, full;
    unsigned long st;
    long used = 0;
    char *arena;
    NfsEntry e;
    NfsAttr dir;
    OSErr err = noErr;
    memset(cookie, 0, sizeof cookie);
    /* a block for the entries: names from the top, offsets to them from the bottom */
    if (!(arena = H->NewMem(ARENA))) return memFullErr;
    at = (unsigned short *)(arena + ARENA) - 1;
    while (!eof && !me->stopped) {
        p = nfs_put_opaque(rpc(me, kNfsProg, plus ? kReadDirPlus : kReadDir), me->fh.data, me->fh.len);
        memcpy(p, cookie, 16);      /* the last entry's cookie, and the verifier */
        p += 16;
        if (plus) p = nfs_put32(p, CHUNK / 4);
        if ((err = call(me, nfs_put32(p, CHUNK))) != noErr) break;
        st = nfs_u32(&me->in);
        if (st == 10004 && plus) { plus = 0; continue; }       /* NFS3ERR_NOTSUPP */
        if (st) { refused(me, st, "No such folder."); err = 1; break; }
        nfs_post_op(&me->in, &dir);
        if (me->in.end - me->in.p >= 8) memcpy(cookie + 8, me->in.p, 8);
        nfs_skip(&me->in, 8);
        if (!shown) {
            page_head(me);
            outf(me, "<IMG SRC=\"plugin://" NFS_NAME "/rsrc?type=ics%%23&id=%d\" WIDTH=16 HEIGHT=16 ALIGN=TOP> <A HREF=\"%s\">Up</A><BR>\n", kMillIconParent, (long)(root ? "/" : ".."));
            shown = 1;
        }
        full = 0;
        while (nfs_entry(&me->in, plus, &e)) {
            Item *i;
            if (e.len > 255 || (e.name[0] == '.' && (e.len == 1 || (e.len == 2 && e.name[1] == '.')))) {
                memcpy(cookie, e.cookie, 8);
                continue;
            }
            /* full: what's here first, then on from the last one kept */
            if (used + ITEM_SIZE(e.len) + 2 * (n + 2) > ARENA) { full = 1; break; }
            memcpy(cookie, e.cookie, 8);
            i = (Item *)(arena + used);
            i->size = e.a.size;
            i->when = e.a.mtime;
            i->folder = e.a.type == kNf3Dir;
            i->known = (unsigned char)e.attrs;
            i->len = (unsigned char)e.len;
            memcpy(i->name, e.name, e.len);
            *(at - n) = (unsigned short)used;
            n++;
            used += ITEM_SIZE(e.len);
        }
        if (me->in.bad) { err = shown ? noErr : kGarbled; break; }
        if (full) {
            if ((err = show_items(me, arena, at - n + 1, n)) != noErr) break;
            used = 0;
            n = 0;
        } else eof = (short)nfs_u32(&me->in);
    }
    if (!err) err = show_items(me, arena, at - n + 1, n);
    H->FreeMem(arena);
    return err;
}

/* RFC 1813 3.3.6 */
static OSErr file(Nfs *me, unsigned long size)
{
    const char *name = strrchr(me->path, '/') + 1;
    char type[64];
    long at, last;
    const unsigned char *data;
    long n;
    unsigned long eof, st;
    NfsAttr a;
    OSErr err;
    H->TypeForName(name, type, sizeof type);
    /* the whole, or the part a Range asks for: READ takes an offset (3.3.6) */
    if (size > 0x7FFFFFFFUL) size = 0x7FFFFFFFUL;
    H->FileHead(me->r, type, (long)size, &at, &last);
    while (at <= last && !me->stopped) {
        unsigned char *p = nfs_put_opaque(rpc(me, kNfsProg, kRead), me->fh.data, me->fh.len);
        p = nfs_put32(nfs_put32(p, 0), (unsigned long)at);
        if ((err = call(me, nfs_put32(p, last - at + 1 < CHUNK ? (unsigned long)(last - at + 1) : CHUNK))) != noErr) return err;
        if ((st = nfs_u32(&me->in)) != 0) { refused(me, st, "No such file."); return 1; }
        nfs_post_op(&me->in, &a);
        nfs_u32(&me->in);           /* count */
        eof = nfs_u32(&me->in);
        data = nfs_opaque(&me->in, &n);
        if (me->in.bad) return kGarbled;
        if (n > last - at + 1) n = last - at + 1;
        out(me, data, n);
        at += n;
        if (!n || eof) break;
    }
    return noErr;
}

/* ------------------------------------------------------------ a request */

/* What a kept connection's note holds: the export mounted, and NFS's port */
typedef struct {
    unsigned long xid;
    unsigned short port;
    NfsFh fh;
    char export[400];           /* (a note holds kMillNoteMax bytes) */
} NfsMount;

OSErr nfs_request(MillPlugIn *self, MillRequest *r)
{
    Nfs *me = (Nfs *)self;
    unsigned short mport, nport;
    char v[16], login[24], *p, *q;
    NfsMount m;
    long ml;
    long n, to;
    short slash;
    NfsAttr a;
    OSErr err;
    me->r = r->ref;
    me->stopped = 0;
    me->c = 0;
    me->xid = 0x4D494C4CUL;
    me->tz = map_offset();
    me->uid = me->gid = 65534;
    if (H->GetSetting("nfs", "uid", v, sizeof v) > 0) me->uid = number(v);
    if (H->GetSetting("nfs", "gid", v, sizeof v) > 0) me->gid = number(v);
    /* the address: /export/folder/file, without a query: RFC 2224 */
    strncpy(me->host, r->host, NAMEN - 1);
    me->host[NAMEN - 1] = 0;
    n = strcspn(r->path, "?");
    if (n >= PATHN) n = PATHN - 1;
    memcpy(me->path, r->path, n);
    me->path[n] = 0;
    H->UrlDecode(me->path, me->path, sizeof me->path);
    n = strlen(me->path);
    slash = me->path[n - 1] == '/';
    if (slash && n > 1) me->path[--n] = 0;
    /* the connection kept from the last page, as the same user and group: still in the
       export this path is in? */
    m.export[0] = 0;
    nport = 0;
    {
        long args[2];
        args[0] = me->uid;
        args[1] = me->gid;
        H->Format(login, sizeof login, "%lu:%lu", args);
    }
    ml = sizeof m;
    if (n > 1 && H->TcpKept(me->r, login, &me->c, &m, &ml) == noErr) {
        to = ml == sizeof m ? (long)strlen(m.export) : 0;
        if (to > 0 && to < (long)sizeof m.export && !memcmp(m.export, me->path, to) && (!me->path[to] || me->path[to] == '/' || m.export[to - 1] == '/')) {
            me->xid = m.xid;
            me->port = nport = m.port;
            me->fh = m.fh;
            H->Status(me->r, "Reading\311");
            goto in;
        }
        H->TcpClose(me->c);
        me->c = 0;
    }
    if ((err = ports(me, r, &mport, &nport)) != noErr || (err = connect(me, mport)) != noErr) goto quit;
    H->Status(me->r, "Reading\311");
    if (n == 1) { err = exports(me, NULL); goto quit; }
    if ((err = exports(me, &to)) != noErr || (err = connect(me, nport)) != noErr) goto quit;
    if (to < (long)sizeof m.export) {
        memcpy(m.export, me->path, to);
        m.export[to] = 0;
        m.fh = me->fh;
    }
in:
    m.port = nport;
    /* down the path from the export, a name at a time: RFC 1813 3.3.3, 3.3.1 */
    a.type = kNf3Dir;
    a.size = 0;
    for (p = me->path + to; *p; p = q) {
        while (*p == '/') p++;
        if (!*p) break;
        q = strchr(p, '/');
        if (!q) q = p + strlen(p);
        if ((err = call(me, nfs_put_opaque(nfs_put_opaque(rpc(me, kNfsProg, kLookup), me->fh.data, me->fh.len), p, q - p))) != noErr ||
            status(me, *q || slash ? "No such folder." : "No such file.")) { if (!err) err = 1; goto quit; }
        nfs_fh(&me->in, &me->fh);
        if (!nfs_post_op(&me->in, &a)) {
            if ((err = call(me, nfs_put_opaque(rpc(me, kNfsProg, kGetAttr), me->fh.data, me->fh.len))) != noErr ||
                status(me, "No such file.")) { if (!err) err = 1; goto quit; }
            nfs_fattr(&me->in, &a);
        }
        if (me->in.bad) { err = kGarbled; goto quit; }
    }
    if (slash) strcpy(me->path + n, "/");
    if (a.type == kNf3Dir && !slash)
        outf(me, "HTTP/1.0 301 Moved" MILL_CRLF "Location: %s/" MILL_CRLF MILL_CRLF, (long)r->url, 0);
    else if (a.type == kNf3Dir) err = folder(me, !me->path[to + 1]);    /* the export's own: Up is the exports */
    else if (slash) H->Fail(me->r, "No such folder.");
    else err = file(me, a.size);
quit:
    if (me->c && n > 1 && me->port == nport && (err == noErr || err == 1) && m.export[0]) {
        /* every reply is read whole, so it's at rest even if Mill wanted no more. NFS has
           no session to end: kept for the next page with the export's handle. */
        m.xid = me->xid;
        H->TcpKeep(me->r, me->c, login, &m, sizeof m, NULL, 0);
        me->c = 0;
    }
    if (me->c) H->TcpClose(me->c);
    me->c = 0;
    if (err == kLost || err == kGarbled) {
        H->Fail(me->r, err == kLost ? "The connection was lost." : "The server\325s reply was not understood.");
        err = noErr;
    }
    return err > 0 ? noErr : err;
}

static const MillSetting settings[] = {
    { "server", "Default server", "Fills in the form on plugin://" NFS_NAME "/about", millText, "", 0, 0, NULL },
    { "uid", "User ID", NULL, millNumber, "65534", 0, 2147483647L, NULL },
    { "gid", "Group ID", NULL, millNumber, "65534", 0, 2147483647L, NULL },
    { "mountport", "Mount port", "For servers without a portmapper", millNumber, "2049", 1, 65535L, NULL },
    kMillRandomAccessSetting("yes")
};
static const MillProtocol protocols[] = { { "nfs", "NFS", "Network File System version 3 (RFC 1813)", 8192, 5, settings } };

const MillProtocol *nfs_protocol(void) { return protocols; }

MillPlugIn *nfs_state(const MillHost *host)
{
    Nfs *me = host->NewMem(sizeof *me);
    if (!me) return NULL;
    me->base.methods = NULL;
    me->base.host = host;
    return &me->base;
}
