/* smb: in the SMB Plug-In: SMB as Windows 95, Windows NT 4, and Samba spoke it (the NT LM
   0.12 dialect of CIFS), read-only.
     smb://host/                the server's shares (RAP NetShareEnum, over \PIPE\LANMAN)
     smb://host/share/path/     a folder, as a page (TRANS2 FIND_FIRST2 and FIND_NEXT2)
     smb://host/share/path      a file, as it is (OPEN_ANDX, then READ_ANDX a piece at a time,
                                from the offset a Range asks for, and CLOSE)
   Port 445 first, then 139 with a NetBIOS session (the server's name is the host's first
   part). A guest first; if that's refused, the name and password Mill asks for, sent as
   LM and NTLM responses to the server's challenge, or as they are if the server wants
   plain passwords. Names are in the server's code page (850), shown as Mac Roman. The
   connection is kept between pages (TcpKeep): negotiated, logged in, and connected to the
   share last used. */
#include <stddef.h>
#include <string.h>
#include <OSUtils.h>

#include "MillPlugIn.h"
#include "plugin.h"
#include "smbutil.h"

enum {
    BUF = 4400,             /* one message, and the most we ask the server to send */
    CHUNK = 4096,           /* a read, and a transaction's data */
    PATHN = 512,
    NAMEN = 256,
    ARENA = 16384           /* a folder's entries, sorted before they're shown */
};
enum { kLost = -30201, kGarbled = -30202, kNoSession = -30203 };
#define PID 0x4D4C

/* What a kept connection's note holds: the session as negotiate, session, and tree left it */
typedef struct {
    unsigned short uid, tid, mid;
    unsigned char secmode;      /* 1: logins (not share passwords); 2: challenge and response */
    unsigned long caps, key;
    short tz;                   /* the server's minutes west of UTC */
    unsigned char challenge[8];
    char share[256];            /* tid's */
} SmbSession;

typedef struct {
    MillPlugIn base;
    MillReqRef r;
    MillConn c;
    short stopped, ready;       /* ready: connected to the share, so it may be kept */
    unsigned short uid, tid, mid;
    unsigned char secmode;      /* 1: logins (not share passwords); 2: challenge and response */
    unsigned long caps, key;
    short tz;                   /* the server's minutes west of UTC */
    unsigned char challenge[8];
    unsigned char eclass;       /* the last reply's error (0 for none) */
    unsigned short ecode;
    unsigned char *w, *b;       /* its words and bytes */
    short wc, bc;
    long got;                   /* its length */
    unsigned char par[32];      /* a transaction's reply: parameters and data */
    unsigned char data[CHUNK];
    char host[NAMEN];
    char path[PATHN];           /* in the share, with backslashes: "\Projects\1997" */
    char shown[PATHN];          /* the address's path, for the page */
    char esc[NAMEN * 6];
    unsigned char buf[BUF];     /* 4 bytes of session header, then the SMB message */
} Smb;

#define H (me->base.host)
#define HDR (me->buf + 4)

/* ------------------------------------------------------------ the page */

static void out(Smb *me, const void *s, long n)
{
    if (!me->stopped && n > 0 && !H->Write(me->r, s, n)) me->stopped = 1;
}

static void outs(Smb *me, const char *s) { out(me, s, strlen(s)); }

static void outf(Smb *me, const char *fmt, long a, long b)
{
    long args[2];
    args[0] = a;
    args[1] = b;
    if (!me->stopped && !H->Printf(me->r, fmt, args)) me->stopped = 1;
}

/* n bytes of a name from the server, as Mac Roman made safe for HTML */
static void out_name(Smb *me, const unsigned char *s, long n)
{
    char m[NAMEN];
    long i;
    for (i = 0; i < n && i < NAMEN - 1; i++) m[i] = (char)smb_from_oem(s[i]);
    m[i] = 0;
    if (H->HtmlEscape(m, me->esc, sizeof me->esc) >= 0) outs(me, me->esc);
}

static void icon(Smb *me, short id)
{
    outf(me, "<IMG SRC=\"plugin://" SMB_NAME "/rsrc?type=ics%%23&id=%d\" WIDTH=16 HEIGHT=16 ALIGN=TOP> ", id, 0);
}

static short icon_for(const char *name)
{
    const char *dot = strrchr(name, '.');
    char e[9];
    short i;
    for (i = 0; i < 8 && dot && dot[i + 1]; i++) e[i] = (char)(dot[i + 1] | 32);
    e[i] = 0;
    if (!strcmp(e, "txt") || !strcmp(e, "msg") || !strcmp(e, "asc") || !strcmp(e, "html") || !strcmp(e, "htm") ||
        !strcmp(name, "README")) return kMillIconText;
    if (!strcmp(e, "gif") || !strcmp(e, "jpg") || !strcmp(e, "jpeg") || !strcmp(e, "pict") || !strcmp(e, "bmp") ||
        !strcmp(e, "pbm") || !strcmp(e, "pgm") || !strcmp(e, "ppm") || !strcmp(e, "pnm")) return kMillIconPicture;
    if (!strcmp(e, "hqx") || !strcmp(e, "sit") || !strcmp(e, "bin") || !strcmp(e, "zip") || !strcmp(e, "cpt") ||
        !strcmp(e, "sea") || !strcmp(e, "gz") || !strcmp(e, "tar") || !strcmp(e, "z") || !strcmp(e, "exe")) return kMillIconBinary;
    return kMillIconDocument;
}

static void page_head(Smb *me)
{
    H->PlugInPage(me->r);
    outs(me, "HTTP/1.0 200 OK" MILL_CRLF "Content-Type: text/html; charset=macintosh" MILL_CRLF MILL_CRLF "<TITLE>SMB: ");
    out_name(me, (const unsigned char *)me->host, strlen(me->host));
    outs(me, "</TITLE><H2>");
    out_name(me, (const unsigned char *)me->host, strlen(me->host));
    out_name(me, (const unsigned char *)me->shown, strlen(me->shown));
    outs(me, "</H2><P>");
}

static void redirect(Smb *me, MillRequest *r)
{
    outf(me, "HTTP/1.0 301 Moved" MILL_CRLF "Location: %s/" MILL_CRLF MILL_CRLF, (long)r->url, 0);
}

/* What the server's last refusal means, for an alert (a missing file is a folder if
   folder is set) */
static void refused(Smb *me, short folder)
{
    char text[64];
    long a[2];
    const char *t = NULL;
    short c = me->eclass, e = me->ecode;
    if (c == 1 && (e == 2 || e == 3 || e == 18)) t = folder || e == 3 ? "No such folder." : "No such file.";
    else if ((c == 2 && e == 6) || (c == 1 && e == 67)) t = "No such share.";
    else if ((c == 1 && e == 5) || (c == 2 && e == 4)) t = "The server refused access.";
    else if (c == 2 && e == 2) t = "The server refused the login.";
    else if (c == 1 && (e == 32 || e == 33)) t = "The file is in use.";
    if (!t) {
        a[0] = c;
        a[1] = e;
        H->Format(text, sizeof text, "The server refused it (error %d.%d).", a);
        t = text;
    }
    H->Fail(me->r, t);
}

/* ------------------------------------------------------------ messages */

/* Start a message: the header, and wc words (zero) to fill in; returns the words */
/* MS-CIFS */
static unsigned char *msg(Smb *me, unsigned char cmd, short wc)
{
    unsigned char *p = HDR;
    memset(p, 0, 35 + 2 * wc);
    p[0] = 0xFF; p[1] = 'S'; p[2] = 'M'; p[3] = 'B';
    p[4] = cmd;
    p[9] = 0x08;            /* paths in any case */
    p[10] = 0x01;           /* long names are fine */
    smb_put16(p + 24, me->tid);
    smb_put16(p + 26, PID);
    smb_put16(p + 28, me->uid);
    smb_put16(p + 30, ++me->mid);
    p[32] = (unsigned char)wc;
    return p + 33;
}

/* Finish the message begun with msg, its bytes ending at end: its length in buf, with
   the session header */
static long seal(Smb *me, unsigned char *end)
{
    unsigned char *p = HDR, *bc = p + 35 + 2 * p[32] - 2;
    long len = end - p;
    smb_put16(bc, (unsigned short)(end - bc - 2));
    me->buf[0] = 0;
    me->buf[1] = (unsigned char)(len >> 16);
    me->buf[2] = (unsigned char)(len >> 8);
    me->buf[3] = (unsigned char)len;
    return len + 4;
}

/* Send the message begun with msg, its bytes ending at end */
static OSErr send_msg(Smb *me, unsigned char *end)
{
    return H->TcpSend(me->c, me->buf, seal(me, end));
}

static OSErr recv_n(Smb *me, unsigned char *p, long n)
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

/* The reply to the last message: its words and bytes in w and b, and its error, if the
   server said no, in eclass and ecode. An error only if the connection failed or what
   came isn't SMB. */
/* MS-CIFS */
static OSErr reply(Smb *me)
{
    unsigned char *p = HDR;
    OSErr err;
    long len;
    for (;;) {
        if ((err = recv_n(me, me->buf, 4)) != noErr) return err;
        len = ((long)me->buf[1] << 16) | ((long)me->buf[2] << 8) | me->buf[3];
        if (len > BUF - 4) return kGarbled;
        if ((err = recv_n(me, p, len)) != noErr) return err;
        if (me->buf[0] == 0x85) continue;           /* NetBIOS keep-alive */
        if (me->buf[0] || len < 35 || p[0] != 0xFF || p[1] != 'S' || p[2] != 'M' || p[3] != 'B') return kGarbled;
        if (SMB_GET16(p + 30) == me->mid) break;    /* else not an answer to us: passed over */
    }
    me->got = len;
    me->eclass = p[5];
    me->ecode = SMB_GET16(p + 7);
    me->wc = p[32];
    me->w = p + 33;
    me->b = me->w + 2 * me->wc + 2;
    if (35 + 2 * me->wc > len) { me->wc = 0; me->bc = 0; return me->eclass ? noErr : kGarbled; }
    me->bc = SMB_GET16(me->b - 2);
    if (me->b + me->bc > p + len) me->bc = (short)(p + len - me->b);
    return noErr;
}

static OSErr call(Smb *me, unsigned char *end)
{
    OSErr err = send_msg(me, end);
    return err != noErr ? err : reply(me);
}

static unsigned char *put_s(unsigned char *b, const char *s)
{
    long n = strlen(s) + 1;
    memcpy(b, s, n);
    return b + n;
}

/* ------------------------------------------------------------ the session */

/* A connection: the address's port; else 445, and if that's refused, 139 */
static OSErr smb_open(Smb *me, MillRequest *r)
{
    OSErr err;
    unsigned short port = r->port;
    char called[16];
    short i;
    unsigned char *p = me->buf;
    if (port != 139) {
        err = H->TcpOpen(me->r, r->host, port ? port : 445, &me->c);
        if (err == noErr || port || err == userCanceledErr) return err;
        port = 139;
    }
    /* RFC 1002 4.3.2 */
    /* the NetBIOS session service (RFC 1002): it wants the server's NetBIOS name, which is
       the host name's first part, as a rule; for an address, or if that's refused,
       *SMBSERVER, which NT 4 and Samba answer to as well */
    for (i = 0; i < 15 && r->host[i] && r->host[i] != '.'; i++) called[i] = r->host[i];
    called[i] = 0;
    if (r->host[0] >= '0' && r->host[0] <= '9') strcpy(called, "*SMBSERVER");
    for (;;) {
        if ((err = H->TcpOpen(me->r, r->host, port, &me->c)) != noErr) return err;
        p[0] = 0x81; p[1] = 0; p[2] = 0; p[3] = 68;
        smb_nb_name(p + 4, called, 0x20);
        smb_nb_name(p + 38, "MILL", 0);
        if ((err = H->TcpSend(me->c, p, 72)) != noErr || (err = recv_n(me, p, 4)) != noErr) return err;
        if (p[0] == 0x82) return noErr;
        H->TcpClose(me->c);
        me->c = 0;
        if (called[0] == '*') return kNoSession;
        strcpy(called, "*SMBSERVER");
    }
}

/* MS-CIFS: SMB_COM_NEGOTIATE */
static OSErr negotiate(Smb *me)
{
    unsigned char *w = msg(me, 0x72, 0), *b = w + 2;
    OSErr err;
    memcpy(b, "\002NT LM 0.12", 12);
    if ((err = call(me, b + 12)) != noErr) return err;
    w = me->w;
    if (me->eclass || me->wc != 17 || SMB_GET16(w)) {
        H->Fail(me->r, "The server doesn\325t support NT LM 0.12.");
        return 1;
    }
    me->secmode = w[2];
    me->key = SMB_GET32(w + 15);
    me->caps = SMB_GET32(w + 19);
    me->tz = (short)SMB_GET16(w + 31);
    if (me->secmode & 8) {
        H->Fail(me->r, "The server requires signed messages.");
        return 1;
    }
    if ((me->secmode & 2) && (w[33] != 8 || me->bc < 8)) return kGarbled;
    memcpy(me->challenge, me->b, 8);
    return noErr;
}

/* Log in as name (or DOMAIN\name), or as a guest with "" and "". Refused: eclass. */
/* MS-CIFS: SMB_COM_SESSION_SETUP_ANDX; MS-NLMP 3.3.1 */
static OSErr session(Smb *me, const char *name, const char *password, short *guest)
{
    unsigned char *w = msg(me, 0x73, 13), *b = w + 28, h[16];
    const char *slash = strchr(name, '\\');
    char user[NAMEN];
    short lm = 0, nt = 0;
    OSErr err;
    w[0] = 0xFF;
    smb_put16(w + 4, BUF - 4);
    smb_put16(w + 6, 1);
    smb_put32(w + 10, me->key);
    if ((me->secmode & 1) && name[0]) {
        if (me->secmode & 2) {
            smb_lm_hash(password, h);
            smb_response(h, me->challenge, b);
            smb_nt_hash(password, h);
            smb_response(h, me->challenge, b + 24);
            lm = nt = 24;
        } else {
            lm = (short)strlen(password) + 1;
            memcpy(b, password, lm);
        }
    }
    smb_put16(w + 14, lm);
    smb_put16(w + 16, nt);
    b += lm + nt;
    strcpy(user, slash ? slash + 1 : name);
    b = put_s(b, user);
    if (slash) { memcpy(b, name, slash - name); b += slash - name; }
    *b++ = 0;
    b = put_s(b, "Mac OS");
    b = put_s(b, "Mill");
    if ((err = call(me, b)) != noErr) return err;
    me->uid = SMB_GET16(HDR + 28);
    *guest = me->wc >= 3 && (me->w[4] & 1);
    return noErr;
}

/* Connect to share (IPC$ for the share list). A server with share passwords (Windows 95's
   own sharing) has the password here. Refused: eclass. */
/* MS-CIFS: SMB_COM_TREE_CONNECT_ANDX */
static OSErr tree(Smb *me, const char *share, const char *password)
{
    unsigned char *w = msg(me, 0x75, 4), *b = w + 10;
    short n = 1;
    OSErr err;
    w[0] = 0xFF;
    if (!(me->secmode & 1) && password[0]) {
        if (me->secmode & 2) {
            unsigned char h[16];
            smb_lm_hash(password, h);
            smb_response(h, me->challenge, b);
            n = 24;
        } else {
            n = (short)strlen(password) + 1;
            memcpy(b, password, n);
        }
    } else *b = 0;
    smb_put16(w + 6, n);
    b += n;
    *b++ = '\\';
    *b++ = '\\';
    memcpy(b, me->host, strlen(me->host));
    b += strlen(me->host);
    *b++ = '\\';
    b = put_s(b, share);
    b = put_s(b, strcmp(share, "IPC$") ? "?????" : "IPC");
    if ((err = call(me, b)) != noErr) return err;
    me->tid = SMB_GET16(HDR + 24);
    return noErr;
}

/* A transaction: TRANSACTION (0x25) to \PIPE\LANMAN, or TRANSACTION2 (0x32) with its
   setup word. The reply's parameters go in par, its data in data (both perhaps from
   several replies); their lengths in *pn and *dn. Refused: eclass. */
static OSErr trans(Smb *me, unsigned char cmd, unsigned short setup, const unsigned char *par, short plen,
                   short *pn, short *dn)
{
    /* MS-CIFS: SMB_COM_TRANSACTION, SMB_COM_TRANSACTION2 */
    short wc = cmd == 0x32 ? 15 : 14;
    unsigned char *w = msg(me, cmd, wc), *b = w + 2 * wc + 2, *p;
    long tp = 0, td = 0, totp, totd;
    OSErr err;
    smb_put16(w, plen);
    smb_put16(w + 4, sizeof me->par);
    smb_put16(w + 6, CHUNK);
    smb_put16(w + 18, plen);
    if (cmd == 0x25) b = put_s(b, "\\PIPE\\LANMAN");
    else *b++ = 0;
    while ((b - HDR) & 3) *b++ = 0;
    smb_put16(w + 20, (unsigned short)(b - HDR));
    memcpy(b, par, plen);
    b += plen;
    smb_put16(w + 24, (unsigned short)(b - HDR));
    if (cmd == 0x32) { w[26] = 1; smb_put16(w + 28, setup); }
    *pn = *dn = 0;
    if ((err = send_msg(me, b)) != noErr) return err;
    do {
        unsigned short pc, po, pd, dc, dof, dd;
        if ((err = reply(me)) != noErr) return err;
        if (me->eclass && !(me->eclass == 1 && me->ecode == 234)) return noErr;     /* 234: more data */
        me->eclass = 0;
        if (me->wc < 10) return kGarbled;
        w = me->w;
        totp = SMB_GET16(w);
        totd = SMB_GET16(w + 2);
        pc = SMB_GET16(w + 6); po = SMB_GET16(w + 8); pd = SMB_GET16(w + 10);
        dc = SMB_GET16(w + 12); dof = SMB_GET16(w + 14); dd = SMB_GET16(w + 16);
        p = HDR;
        if (po + pc > me->got || dof + dc > me->got || pd + pc > sizeof me->par || dd + dc > CHUNK) return kGarbled;
        memcpy(me->par + pd, p + po, pc);
        memcpy(me->data + dd, p + dof, dc);
        tp += pc;
        td += dc;
    } while (tp < totp || td < totd);
    *pn = (short)totp;
    *dn = (short)totd;
    return noErr;
}

/* ------------------------------------------------------------ shares */

/* MS-RAP: NetShareEnum */
static OSErr shares(Smb *me)
{
    /* NetShareEnum (0), "WrLeh" "B13BWz", level 1, a 4096-byte buffer */
    static const unsigned char par[19] = { 0, 0, 'W', 'r', 'L', 'e', 'h', 0, 'B', '1', '3', 'B', 'W', 'z', 0, 1, 0, 0, 0x10 };
    char name[13], comment[64], href[40];
    unsigned short type, conv, n, i;
    short pn, dn;
    OSErr err;
    if ((err = trans(me, 0x25, 0, par, sizeof par, &pn, &dn)) != noErr) return err;
    if (me->eclass) { refused(me, 0); return noErr; }
    if (pn < 8 || (SMB_GET16(me->par) && SMB_GET16(me->par) != 234)) { H->Fail(me->r, "The server would not list its shares."); return noErr; }
    conv = SMB_GET16(me->par + 2);
    n = SMB_GET16(me->par + 4);
    page_head(me);
    for (i = 0; i < n && smb_share_entry(me->data, dn, i, conv, name, &type, comment, sizeof comment); i++) {
        /* disks only, and not the hidden ones (C$) */
        if (type != 0 || !name[0] || name[strlen(name) - 1] == '$') continue;
        icon(me, kMillIconDisk);
        H->UrlEncode(name, href, sizeof href - 1);
        outf(me, "<A HREF=\"%s/\">", (long)href, 0);
        out_name(me, (const unsigned char *)name, strlen(name));
        outs(me, "</A>");
        if (comment[0]) { outs(me, " &nbsp; "); out_name(me, (const unsigned char *)comment, strlen(comment)); }
        outs(me, "<BR>\n");
    }
    return noErr;
}

/* ------------------------------------------------------------ folders */

/* FIND_FIRST2 (first) or FIND_NEXT2 for pattern: the entries in me->data (*dn bytes),
   *count of them, *end set at the last; *sid for the next. Refused: eclass. */
static OSErr find(Smb *me, const char *pattern, short level, short first, unsigned short *sid, short *count,
                  short *end, short *dn)
{
    /* MS-CIFS: TRANS2_FIND_FIRST2, TRANS2_FIND_NEXT2 */
    unsigned char p[PATHN + 16];
    short pn;
    OSErr err;
    long k = strlen(pattern) + 1;
    memset(p, 0, 12);
    if (first) {
        smb_put16(p, 0x16);         /* folders, hidden and system files too */
        smb_put16(p + 2, 200);
        smb_put16(p + 4, 2);        /* close when it's done */
        smb_put16(p + 6, level);
    } else {
        smb_put16(p, *sid);
        smb_put16(p + 2, 200);
        smb_put16(p + 4, level);
        smb_put16(p + 10, 2 | 8);   /* close when done; go on from the last */
    }
    if (k > PATHN + 4) return kGarbled;
    memcpy(p + 12, pattern, k);
    if ((err = trans(me, 0x32, first ? 1 : 2, p, 12 + (short)k, &pn, dn)) != noErr || me->eclass) return err;
    if (pn < (first ? 6 : 4)) return kGarbled;
    if (first) *sid = SMB_GET16(me->par);
    *count = (short)SMB_GET16(me->par + (first ? 2 : 0));
    *end = (short)SMB_GET16(me->par + (first ? 4 : 2));
    return noErr;
}

typedef struct {
    unsigned long size, when;
    unsigned short date, time;
    unsigned char folder, len;
    char name[1];
} Item;
/* its room in the block, from the start of one to the next */
#define ITEM_SIZE(n) ((offsetof(Item, name) + (n) + sizeof(long) - 1) & ~(sizeof(long) - 1))

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

/* the entries gathered, sorted, as lines of the page */
static void show_items(Smb *me, char *arena, unsigned short *at, short n)
{
    short gap, i, j;
    char href[NAMEN * 3], name[NAMEN], when[40];
    long a[5];
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
        short t[5];
        memcpy(name, it->name, it->len);
        name[it->len] = 0;
        if (H->UrlEncode(name, href, sizeof href - 2) < 0) continue;
        if (it->folder) strcat(href, "/");
        icon(me, it->folder ? kMillIconFolder : icon_for(name));
        outf(me, "<A HREF=\"%s\">", (long)href, 0);
        out_name(me, (const unsigned char *)it->name, it->len);
        outs(me, "</A>");
        if (!it->folder) outf(me, " &nbsp; %ldK", (long)((it->size + 1023) / 1024), 0);
        if (it->when) {
            SecondsToDate(it->when - 60L * me->tz, &d);
            t[0] = d.year; t[1] = d.month; t[2] = d.day; t[3] = d.hour; t[4] = d.minute;
        } else smb_dos_time(it->date, it->time, t);
        if (t[0] > 1980) {
            for (j = 0; j < 5; j++) a[j] = t[j];
            H->Format(when, sizeof when, " &nbsp; %04d-%02d-%02d %02d:%02d", a);
            outs(me, when);
        }
        outs(me, "<BR>\n");
    }
}

static OSErr folder(Smb *me)
{
    char pattern[PATHN + 4], last[NAMEN];
    unsigned short sid = 0, *at;
    short level = (me->caps & 0x10) ? kSmbFileDirectoryInfo : kSmbInfoStandard;
    short count, end = 0, dn, n = 0, k, shown = 0;
    long used = 0;
    char *arena;
    OSErr err;
    strcpy(pattern, me->path);
    strcat(pattern, "\\*");
    last[0] = 0;
    /* a block for the entries: names from the top, offsets to them from the bottom */
    if (!(arena = H->NewMem(ARENA))) return memFullErr;
    at = (unsigned short *)(arena + ARENA) - 1;
    do {
        const unsigned char *p, *e;
        SmbEntry it;
        if ((err = find(me, shown ? last : pattern, level, !shown, &sid, &count, &end, &dn)) != noErr) goto done;
        if (me->eclass) {
            if (!shown) refused(me, 1);
            goto done;
        }
        if (!shown) {
            page_head(me);
            icon(me, kMillIconParent);
            outs(me, "<A HREF=\"..\">Up</A><BR>\n");
            shown = 1;
        }
        e = me->data + dn;
        for (p = me->data, k = 0; k < count && p && p < e; k++) {
            p = smb_find_entry(p, e, level, &it);
            if (!p) break;
            memcpy(last, it.name, it.len < NAMEN - 1 ? it.len : NAMEN - 1);
            last[it.len < NAMEN - 1 ? it.len : NAMEN - 1] = 0;
            if (it.len > 255 || (it.name[0] == '.' && (it.len == 1 || (it.len == 2 && it.name[1] == '.')))) continue;
            if (used + ITEM_SIZE(it.len) + 2 * (n + 2) > ARENA) {
                /* full: what's here first */
                show_items(me, arena, at - n + 1, n);
                used = 0;
                n = 0;
            }
            {
                Item *i = (Item *)(arena + used);
                i->size = it.size;
                i->when = it.when;
                i->date = it.date;
                i->time = it.time;
                i->folder = (unsigned char)it.folder;
                i->len = (unsigned char)it.len;
                memcpy(i->name, it.name, it.len);
                *(at - n) = (unsigned short)used;
                n++;
                used += ITEM_SIZE(it.len);
            }
        }
    } while (!end && count > 0 && !me->stopped);
    show_items(me, arena, at - n + 1, n);
done:
    H->FreeMem(arena);
    return err;
}

/* ------------------------------------------------------------ files */

/* MS-CIFS: SMB_COM_OPEN_ANDX, SMB_COM_READ_ANDX, SMB_COM_CLOSE */
static OSErr file(Smb *me, MillRequest *r)
{
    unsigned char *w = msg(me, 0x2D, 15), *b = w + 32;
    const char *name = strrchr(me->path, '\\') + 1;
    unsigned short fid;
    unsigned long size;
    long at, last;
    char type[64];
    OSErr err;
    w[0] = 0xFF;
    smb_put16(w + 6, 0x0040);       /* read, others may too */
    smb_put16(w + 8, 0x0016);
    smb_put16(w + 16, 1);           /* open it if it's there */
    b = put_s(b, me->path);
    if ((err = call(me, b)) != noErr) return err;
    if (me->eclass || me->wc < 15 || (SMB_GET16(me->w + 6) & 0x10)) {
        /* a folder, asked for without its slash? */
        unsigned short sid;
        short count = 0, end, dn, level = (me->caps & 0x10) ? kSmbFileDirectoryInfo : kSmbInfoStandard;
        unsigned char eclass = me->eclass;
        unsigned short ecode = me->ecode;
        SmbEntry it;
        if ((err = find(me, me->path, level, 1, &sid, &count, &end, &dn)) != noErr) return err;
        if (!me->eclass && count > 0 && smb_find_entry(me->data, me->data + dn, level, &it) && it.folder) {
            redirect(me, r);
            return noErr;
        }
        me->eclass = eclass ? eclass : 1;
        me->ecode = eclass ? ecode : 5;
        refused(me, 0);
        return noErr;
    }
    fid = SMB_GET16(me->w + 4);
    size = SMB_GET32(me->w + 12);
    H->TypeForName(name, type, sizeof type);
    /* the whole, or the part a Range asks for: READ_ANDX takes an offset */
    if (size > 0x7FFFFFFFUL) size = 0x7FFFFFFFUL;
    H->FileHead(me->r, type, (long)size, &at, &last);
    while (at <= last && !me->stopped) {
        unsigned short n, off;
        w = msg(me, 0x2E, 10);
        w[0] = 0xFF;
        smb_put16(w + 4, fid);
        smb_put32(w + 6, (unsigned long)at);
        smb_put16(w + 10, last - at + 1 < CHUNK ? (unsigned short)(last - at + 1) : CHUNK);
        if ((err = call(me, w + 22)) != noErr) return err;
        if (me->eclass || me->wc < 12) { err = kLost; break; }
        n = SMB_GET16(me->w + 10);
        off = SMB_GET16(me->w + 12);
        if (!n || off + n > me->got) break;
        out(me, HDR + off, n);
        at += n;
    }
    if (err == noErr) {
        /* the file closed again, so a kept session holds nothing open (LastWriteTime 0:
           left as it is) */
        w = msg(me, 0x04, 3);
        smb_put16(w, fid);
        err = call(me, w + 8);
    }
    return err;
}

/* ------------------------------------------------------------ a request */

/* Ask for a name and password for this server: first (wrong 0), or again after the server
   refused the ones given (a page says so if the person cancels) */
static void ask(Smb *me, MillRequest *r, const char *key, short wrong, const char *user)
{
    char prompt[NAMEN + 64];
    long a[1];
    a[0] = (long)r->host;
    r->loginKey[0] = 0;
    if (wrong) outs(me, "HTTP/1.0 401 Unauthorized" MILL_CRLF "Content-Type: text/html" MILL_CRLF MILL_CRLF
                        "<TITLE>Login failed</TITLE><H2>Login failed</H2><P>The server refused the name or password.");
    H->Format(prompt, sizeof prompt, wrong ? "The name or password for %s was wrong. Try again?" : "Log in to %s.", a);
    H->AskLogin(me->r, prompt, NULL, NULL, key, user[0] ? user : NULL, NULL);
}

/* Into the session kept from the last page, as this user (0 if there's none), connected
   to share: a TREE_DISCONNECT and a new TREE_CONNECT if it was connected to another */
static short resume(Smb *me, const char *user, const char *share, const char *pass, OSErr *err)
{
    SmbSession s;
    long n = sizeof s;
    *err = noErr;
    if (H->TcpKept(me->r, user, &me->c, &s, &n) != noErr) return 0;
    if (n != sizeof s) { H->TcpClose(me->c); me->c = 0; return 0; }
    me->uid = s.uid; me->tid = s.tid; me->mid = s.mid;
    me->secmode = s.secmode; me->caps = s.caps; me->key = s.key; me->tz = s.tz;
    memcpy(me->challenge, s.challenge, 8);
    me->eclass = 0;
    s.share[sizeof s.share - 1] = 0;
    if (!strcmp(s.share, share)) return 1;
    /* MS-CIFS: SMB_COM_TREE_DISCONNECT, then the new share */
    if ((*err = call(me, msg(me, 0x71, 0) + 2)) != noErr) return 1;
    me->tid = 0;
    *err = tree(me, share, pass);
    return 1;
}

/* At rest (every reply is read whole, even when Mill wants no more): kept for the next
   page, with a LOGOFF_ANDX for Mill to send when it closes it */
static void keep(Smb *me, const char *user, const char *share)
{
    SmbSession s;
    unsigned char *w;
    long n;
    memset(&s, 0, sizeof s);
    s.uid = me->uid; s.tid = me->tid;
    s.secmode = me->secmode; s.caps = me->caps; s.key = me->key; s.tz = me->tz;
    memcpy(s.challenge, me->challenge, 8);
    strncpy(s.share, share, sizeof s.share - 1);
    /* MS-CIFS: SMB_COM_LOGOFF_ANDX */
    w = msg(me, 0x74, 2);
    w[0] = 0xFF;
    n = seal(me, w + 6);
    s.mid = me->mid;
    H->TcpKeep(me->r, me->c, user, &s, sizeof s, me->buf, n);
    me->c = 0;
}

OSErr smb_request(MillPlugIn *self, MillRequest *r)
{
    Smb *me = (Smb *)self;
    char share[NAMEN], user[NAMEN], pass[NAMEN], key[NAMEN + 8], *p;
    const char *rest;
    short named, guest = 0;
    long n, a[1];
    OSErr err;
    me->r = r->ref;
    me->stopped = me->ready = 0;
    me->c = 0;
    me->uid = me->tid = me->mid = 0;
    /* the address: /share/folder/file, without a query */
    strncpy(me->host, r->host, NAMEN - 1);
    me->host[NAMEN - 1] = 0;
    n = strcspn(r->path, "?");
    if (n >= PATHN) n = PATHN - 1;
    memcpy(me->shown, r->path, n);
    me->shown[n] = 0;
    rest = me->shown + 1;
    p = strchr(rest, '/');
    n = p ? p - rest : (long)strlen(rest);
    if (n >= NAMEN) n = NAMEN - 1;
    memcpy(share, rest, n);
    share[n] = 0;
    H->UrlDecode(share, share, sizeof share);
    if (share[0] && !p) { redirect(me, r); return noErr; }     /* smb://host/share: its folder */
    H->UrlDecode(p ? p : "", me->path, sizeof me->path);
    for (p = me->path; *p; p++) if (*p == '/') *p = '\\';
    n = strlen(me->path);
    if (n && me->path[n - 1] == '\\') me->path[n - 1] = 0;     /* a folder: "" for the share's own */
    H->UrlDecode(me->shown, me->shown, sizeof me->shown);
    /* who: the address's name and password; else the login kept or typed for this server
       (its key: smb://host); else a guest */
    a[0] = (long)r->host;
    H->Format(key, sizeof key, "smb://%s", a);
    strncpy(user, r->user, NAMEN - 1);
    strncpy(pass, r->password, NAMEN - 1);
    user[NAMEN - 1] = pass[NAMEN - 1] = 0;
    if (user[0]) strcpy(r->loginKey, key);
    else H->SavedLogin(key, user, sizeof user, pass, sizeof pass);
    named = user[0] != 0;
    if (resume(me, user, share[0] ? share : "IPC$", pass, &err)) {
        if (err != noErr) goto quit;
        goto in;
    }
    if ((err = smb_open(me, r)) != noErr) goto quit;
    H->Status(me->r, "Logging in\311");
    if ((err = negotiate(me)) != noErr) goto quit;
    if ((err = session(me, user, pass, &guest)) != noErr) goto quit;
    if (me->eclass) {
        /* refused: Samba says 1.5, Windows 2.2 */
        ask(me, r, key, named, user);
        goto quit;
    }
    if ((err = tree(me, share[0] ? share : "IPC$", pass)) != noErr) goto quit;
in:
    if (me->eclass) {
        if ((me->eclass == 1 && me->ecode == 5) || (me->eclass == 2 && (me->ecode == 4 || me->ecode == 2))) ask(me, r, key, named && guest, user);
        else refused(me, 0);
        goto quit;
    }
    memset(pass, 0, sizeof pass);
    me->ready = 1;
    H->Status(me->r, "Reading\311");
    if (!share[0]) err = shares(me);
    else if (!me->shown[0] || me->shown[strlen(me->shown) - 1] == '/') err = folder(me);
    else err = file(me, r);
quit:
    memset(pass, 0, sizeof pass);
    if (me->c && me->ready && err == noErr) keep(me, user, share[0] ? share : "IPC$");
    if (me->c) H->TcpClose(me->c);
    me->c = 0;
    if (err == kLost || err == kGarbled || err == kNoSession) {
        H->Fail(me->r, err == kLost ? "The connection was lost." :
                       err == kNoSession ? "The server refused the connection." : "The server\325s reply was not understood.");
        err = noErr;
    }
    return err > 0 ? noErr : err;
}

static const MillSetting settings[] = {
    { "server", "Default server", "Fills in the form on plugin://" SMB_NAME "/about", millText, "", 0, 0, NULL },
    kMillRandomAccessSetting("yes")
};
static const MillProtocol protocols[] = { { "smb", "SMB", "Server Message Block (CIFS, NT LM 0.12)", 8192, 2, settings } };

const MillProtocol *smb_protocol(void) { return protocols; }

MillPlugIn *smb_state(const MillHost *host)
{
    Smb *me = host->NewMem(sizeof *me);
    if (!me) return NULL;
    me->base.methods = NULL;
    me->base.host = host;
    return &me->base;
}
