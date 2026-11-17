/* ldap: in the LDAP Plug-In: directory searches (LDAPv2, RFC 1777; v3, RFC 2251, if the
   server takes only that), with addresses as RFC 1959 has them:
     ldap://host/                           the server's directories (its root DSE's
                                            namingContexts) and a search form
     ldap://host/dn?attributes?scope?filter a search, its entries as a page
   RFC 2255's extensions too: bindname, to log in, and Mill's own x-layout=table or
   x-layout=records. Anonymous unless a login is kept for the server, the address asks for
   one, or the server refuses (strongAuthRequired, insufficientAccessRights): then a simple
   bind with the DN and password Mill asks for. Each entry's DN links to it; values that
   aren't text are shown by their size. The connection is kept between pages (TcpKeep),
   bound as the same DN. */
#include <string.h>

#include "MillPlugIn.h"
#include "ldap.h"
#include "ldaputil.h"

enum { NAMEN = 256, DNN = 512, VAL = 1024, ESC = VAL * 6, REQ = 1024, COLS = 16, COLN = 40 };
enum { kLost = -30201, kGarbled = -30202, kDone = 1 };
/* RFC 1777 4 (resultCode) */
enum { kSuccess = 0, kProtocolError = 2, kTimeLimit = 3, kSizeLimit = 4, kStrongAuth = 8, kNoSuchObject = 32,
       kInappropriateAuth = 48, kInvalidCredentials = 49, kInsufficientAccess = 50, kUnwilling = 53 };

typedef struct {
    MillPlugIn base;
    MillRequest *req;
    MillReqRef r;
    MillConn c;
    short stopped, loggedIn, version, bound;
    long id;
    LdapIn in;
    LdapUrl u;
    char host[NAMEN], key[NAMEN + 16], user[NAMEN], pass[NAMEN];
    short table;                /* the layout: 1 table, 0 records */
    long most, budget;          /* Most records; Most per record, in bytes */
    char cols[COLS][COLN];      /* the table's columns */
    short ncols;
    char msg[NAMEN];            /* a result's errorMessage */
    char dn[DNN];
    char val[VAL];
    char work[VAL];
    char esc[ESC];
    unsigned char req_buf[REQ];
} Ldap;

#define H (me->base.host)

/* ------------------------------------------------------------ the page */

static void out(Ldap *me, const char *s, long n)
{
    if (!me->stopped && n > 0 && !H->Write(me->r, s, n)) me->stopped = 1;
}

static void outs(Ldap *me, const char *s) { out(me, s, strlen(s)); }

static void outf(Ldap *me, const char *fmt, long a, long b)
{
    long args[2];
    args[0] = a;
    args[1] = b;
    if (!me->stopped && !H->Printf(me->r, fmt, args)) me->stopped = 1;
}

/* n bytes from the server (UTF-8 if they are, else Latin-1) as Mac Roman made safe for HTML */
static void out_text(Ldap *me, const char *s, long n)
{
    if (n > VAL - 1) n = VAL - 1;
    if (n > 0 && H->ToMacRoman(s, n, millGuess, me->work, sizeof me->work) >= 0 && H->HtmlEscape(me->work, me->esc, sizeof me->esc) >= 0)
        outs(me, me->esc);
}

/* A link to a search of dn: its own entry (scope base) or what's under it (one) */
static void dn_link(Ldap *me, const char *dn, const char *text, short one)
{
    char e[DNN * 3];
    if (ldap_escape(dn, e, sizeof e) < 0) return;
    outf(me, "<A HREF=\"/%s%s\">", (long)e, (long)(one ? "??one" : ""));
    out_text(me, text, strlen(text));
    outs(me, "</A>");
}

static void page_head(Ldap *me, const char *title)
{
    H->PlugInPage(me->r);
    outs(me, "HTTP/1.0 200 OK" MILL_CRLF "Content-Type: text/html; charset=macintosh" MILL_CRLF MILL_CRLF "<TITLE>" LDAP_TITLE ": ");
    out_text(me, title, strlen(title));
    outs(me, "</TITLE>\n");
}

/* The page's search form, for the root page and plugin://ldap/about */
void ldap_form(const MillHost *h, MillReqRef r, const char *host, const char *base)
{
    char esc[NAMEN * 6];
    long a[1];
    a[0] = 0;
    h->Printf(r, "<FORM ACTION=\"plugin://" LDAP_NAME "/go\"><TABLE>"
                 "<TR><TD><B>ldap</B> server</TD><TD><INPUT NAME=host SIZE=24 VALUE=\"", a);
    if (h->HtmlEscape(host, esc, sizeof esc) >= 0) h->Write(r, esc, strlen(esc));
    h->Printf(r, "\"></TD></TR>\n<TR><TD>Base</TD><TD><INPUT NAME=base SIZE=24 VALUE=\"", a);
    if (h->HtmlEscape(base, esc, sizeof esc) >= 0) h->Write(r, esc, strlen(esc));
    h->Printf(r, "\"></TD></TR>\n<TR><TD>Name</TD><TD><INPUT NAME=name SIZE=24></TD></TR>\n"
                 "<TR><TD>or filter</TD><TD><INPUT NAME=filter SIZE=24></TD></TR>\n"
                 "<TR><TD>Results as</TD><TD><SELECT NAME=layout><OPTION>Records<OPTION>Table</SELECT> "
                 "<INPUT TYPE=SUBMIT VALUE=\"Search\"></TD></TR></TABLE></FORM>\n", a);
}

/* ------------------------------------------------------------ the connection */

static long tcp_fill(LdapIn *in, unsigned char *buf, long size)
{
    Ldap *me = (Ldap *)in->ref;
    long n = size;
    if (H->TcpRecv(me->c, buf, &n) != noErr) return 0;
    return n;
}

static OSErr send_out(Ldap *me, LdapOut *o)
{
    if (o->full) return kGarbled;
    return H->TcpSend(me->c, o->p, o->n);
}

/* The next message's protocolOp: its tag, with its length in *len (RFC 1777 4.1); -1 if
   the connection was lost or it wasn't an LDAPMessage */
static short next_op(Ldap *me, long *len)
{
    long n;
    if (ldap_tag(&me->in, &n) != 0x30) return -1;
    if (ldap_tag(&me->in, &n) != 0x02) return -1;
    ldap_skip(&me->in, n);      /* messageID: one request at a time, so it's ours */
    return ldap_tag(&me->in, len);
}

/* A simple bind (RFC 1777 4.3): its resultCode, or -1 lost. Version 2 first; version 3
   for a server that takes only that. */
static long bind(Ldap *me, const char *dn, const char *password)
{
    LdapOut o;
    long len, code;
    short op;
    for (;;) {
        ldap_out(&o, me->req_buf, REQ);
        ldap_bind(&o, ++me->id, me->version, dn, password);
        if (send_out(me, &o) != noErr) return -1;
        memset(me->req_buf, 0, REQ);
        if ((op = next_op(me, &len)) != 0x61) return -1;
        code = ldap_result(&me->in, len, me->msg, sizeof me->msg);
        if (code != kProtocolError || me->version == 3) return code;
        me->version = 3;
    }
}

/* Ask for a DN and password: first (wrong 0), or again after the server refused them */
static void ask(Ldap *me, short wrong)
{
    char prompt[NAMEN + 64];
    long a[1];
    a[0] = (long)me->host;
    me->req->loginKey[0] = 0;
    if (wrong) outs(me, "HTTP/1.0 401 Unauthorized" MILL_CRLF "Content-Type: text/html" MILL_CRLF MILL_CRLF
                        "<TITLE>Login failed</TITLE><H2>Login failed</H2><P>The server refused the name or password.");
    H->Format(prompt, sizeof prompt, wrong ? "The name or password for %s was wrong. Try again?" : "Log in to %s.", a);
    H->AskLogin(me->r, prompt, "DN:", NULL, me->key,
                me->user[0] ? me->user : me->u.bindname[0] ? me->u.bindname : NULL, NULL);
}

/* A result that isn't success, as an alert or a login */
static void refused(Ldap *me, long code)
{
    char text[NAMEN + 64];
    long a[2];
    if (code == kNoSuchObject) { H->Fail(me->r, "No such entry."); return; }
    if (code == kStrongAuth || code == kInsufficientAccess || code == kInappropriateAuth || code == kUnwilling) {
        if (!me->loggedIn) { ask(me, 0); return; }
        H->Fail(me->r, "The server refused access.");
        return;
    }
    a[0] = (long)(me->msg[0] ? me->msg : "no reason given");
    a[1] = code;
    H->Format(text, sizeof text, "The server refused it: %s (error %d).", a);
    H->Fail(me->r, text);
}

/* ------------------------------------------------------------ entries */

/* An attribute whose values are DNs: shown as links */
static short dn_valued(const char *type)
{
    static const char *const names = " member uniquemember manager owner seealso secretary roleoccupant ";
    char t[COLN + 2];
    long i, n = strlen(type);
    if (n > COLN - 1) return 0;
    t[0] = ' ';
    for (i = 0; i < n; i++) t[i + 1] = (char)(type[i] >= 'A' && type[i] <= 'Z' ? type[i] + 32 : type[i]);
    t[n + 1] = ' ';
    t[n + 2] = 0;
    return strstr(names, t) != NULL;
}

static short is_binary(const char *type, const char *v, long kept)
{
    const char *semi = strchr(type, ';');
    if (semi && !strcmp(semi, ";binary")) return 1;
    return !ldap_text((const unsigned char *)v, kept);
}

/* A table's column for type: its number, or -1 */
static short column(Ldap *me, const char *type)
{
    short i;
    for (i = 0; i < me->ncols; i++) {
        const char *a = me->cols[i], *b = type;
        while (*a && *b && (*a | 32) == (*b | 32)) a++, b++;
        if (!*a && !*b) return i;
    }
    return -1;
}

enum { kText, kDn, kBinary, kCut };

/* One value of n bytes, read into val: its kind, and in *shown the bytes to show (text
   cut to what's left of the record's budget, *left; -1 once it's spent) */
static short value(Ldap *me, const char *type, long n, long *left, long *shown)
{
    long kept = ldap_bytes(&me->in, n, me->val, sizeof me->val);
    *shown = kept;
    if (is_binary(type, me->val, kept)) { *left -= 20; return kBinary; }
    if (n > *left) { *shown = *left > 0 ? *left : 0; *left = -1; return kCut; }
    *left -= n;
    return dn_valued(type) ? kDn : kText;
}

static void value_out(Ldap *me, short kind, const char *v, long n, long size)
{
    if (kind == kBinary) outf(me, "(binary, %ld bytes)", size, 0);
    else if (kind == kDn) dn_link(me, v, v, 0);
    else {
        out_text(me, v, n);
        if (kind == kCut) outs(me, "\311");     /* the ellipsis, in Mac Roman */
    }
}

/* SearchResultEntry ::= [APPLICATION 4] SEQUENCE { objectName, attributes SEQUENCE OF
   SEQUENCE { type, vals SET OF value } } (RFC 1777 4.5.2). Records: the DN and a table of
   its own. Table: a row, the RDN first; the values wait in arena until the entry ends,
   since its attributes come in the server's order, not the columns'. */
static OSErr entry(Ldap *me, long len, char *arena, long room)
{
    long start = me->in.pos, alen, vlen, left = me->budget, k, used = 0, shown;
    char type[COLN];
    short over = 0, kind, c = 0, i;
    ldap_tag(&me->in, &k);
    ldap_bytes(&me->in, k, me->dn, sizeof me->dn);
    if (me->table) {
        char rdn[DNN];
        for (k = 0; me->dn[k] && me->dn[k] != ',' && k < DNN - 1; k++) rdn[k] = me->dn[k];
        rdn[k] = 0;
        outs(me, "<TR><TD VALIGN=TOP>");
        dn_link(me, me->dn, rdn, 0);
        outs(me, "</TD>");
    } else {
        if (me->u.scope != kLdapBase) {     /* a single entry's DN is the page's heading */
            outs(me, "<P><B>");
            dn_link(me, me->dn, me->dn, 0);
            outs(me, "</B>\n");
        }
        outs(me, "<TABLE BORDER=1 CELLSPACING=0 CELLPADDING=2>\n");
    }
    ldap_tag(&me->in, &alen);
    k = me->in.pos;
    while (me->in.pos - k < alen && !me->in.bad && !me->stopped) {
        long vstart, kk;
        ldap_tag(&me->in, &kk);             /* the attribute's SEQUENCE */
        ldap_tag(&me->in, &kk);
        ldap_bytes(&me->in, kk, type, sizeof type);
        ldap_tag(&me->in, &vlen);           /* SET OF value */
        vstart = me->in.pos;
        if (over || (me->table && (c = column(me, type)) < 0)) { ldap_skip(&me->in, vlen); continue; }
        if (!me->table) {
            outs(me, "<TR><TD VALIGN=TOP><B>");
            out_text(me, type, strlen(type));
            outs(me, "</B></TD><TD>");
        }
        while (me->in.pos - vstart < vlen && !me->in.bad) {
            short first = me->in.pos == vstart;
            if (left < 0) { ldap_skip(&me->in, vlen - (me->in.pos - vstart)); break; }
            ldap_tag(&me->in, &kk);
            kind = value(me, type, kk, &left, &shown);
            if (!me->table) {
                if (!first) outs(me, "<BR>");
                value_out(me, kind, me->val, shown, kk);
            } else if (used + shown + 8 <= room) {
                /* a cell's item: column, kind, the value's size (4), the length kept (2), bytes */
                arena[used] = (char)c;
                arena[used + 1] = (char)kind;
                arena[used + 2] = (char)(kk >> 24);
                arena[used + 3] = (char)(kk >> 16);
                arena[used + 4] = (char)(kk >> 8);
                arena[used + 5] = (char)kk;
                arena[used + 6] = (char)(shown >> 8);
                arena[used + 7] = (char)shown;
                memcpy(arena + used + 8, me->val, shown);
                used += 8 + shown;
            } else left = -1;
        }
        if (!me->table) outs(me, "</TD></TR>\n");
        if (left < 0) over = 1;
    }
    if (me->table) {
        for (i = 0; i < me->ncols; i++) {
            long at, l, size;
            short n = 0;
            outs(me, "<TD VALIGN=TOP>");
            for (at = 0; at < used; at += 8 + l) {
                l = (unsigned char)arena[at + 6] << 8 | (unsigned char)arena[at + 7];
                if (arena[at] != i) continue;
                size = (long)(unsigned char)arena[at + 2] << 24 | (long)(unsigned char)arena[at + 3] << 16 |
                       (long)(unsigned char)arena[at + 4] << 8 | (unsigned char)arena[at + 5];
                if (n++) outs(me, "<BR>");
                memcpy(me->val, arena + at + 8, l);
                me->val[l] = 0;
                value_out(me, arena[at + 1], me->val, l, size);
            }
            outs(me, "</TD>");
        }
        outs(me, over ? "<TD>(more not shown)</TD></TR>\n" : "</TR>\n");
    } else {
        if (over) outs(me, "<TR><TD COLSPAN=2>(more not shown)</TD></TR>\n");
        outs(me, "</TABLE>\n");
    }
    k = len - (me->in.pos - start);
    if (k > 0) ldap_skip(&me->in, k);
    return me->in.bad ? kLost : noErr;
}

/* The table's columns: the attributes asked for, else every type the entries have, in the
   order first seen, from a search for types only (attrsOnly) */
static OSErr columns(Ldap *me)
{
    LdapOut o;
    const char *p = me->u.attrs;
    long len, k, alen;
    short op;
    char type[COLN];
    me->ncols = 0;
    while (*p && me->ncols < COLS) {
        long n = 0;
        while (*p == ' ' || *p == ',') p++;
        while (*p && *p != ',' && n < COLN - 1) me->cols[me->ncols][n++] = *p++;
        while (n && me->cols[me->ncols][n - 1] == ' ') n--;
        me->cols[me->ncols][n] = 0;
        while (*p && *p != ',') p++;
        if (n && strcmp(me->cols[me->ncols], "*")) me->ncols++;
    }
    if (me->ncols) return noErr;
    ldap_out(&o, me->req_buf, REQ);
    if (!ldap_search(&o, ++me->id, me->u.dn, me->u.scope, me->most, 1, me->u.filter, "")) return kGarbled;
    if (send_out(me, &o) != noErr) return kLost;
    while ((op = next_op(me, &len)) == 0x64 || op == 0x73) {
        long start = me->in.pos;
        if (op == 0x64) {
            ldap_tag(&me->in, &k);
            ldap_skip(&me->in, k);
            ldap_tag(&me->in, &alen);
            {
                long astart = me->in.pos;
                while (me->in.pos - astart < alen && !me->in.bad) {
                    ldap_tag(&me->in, &k);
                    ldap_tag(&me->in, &k);
                    ldap_bytes(&me->in, k, type, sizeof type);
                    ldap_tag(&me->in, &k);
                    ldap_skip(&me->in, k);
                    if (column(me, type) < 0 && me->ncols < COLS) strcpy(me->cols[me->ncols++], type);
                }
            }
        }
        k = len - (me->in.pos - start);
        if (k > 0) ldap_skip(&me->in, k);
    }
    if (op != 0x65) return kLost;
    ldap_result(&me->in, len, me->msg, sizeof me->msg);
    return noErr;
}

/* ------------------------------------------------------------ pages */

/* ldap://host/: the root DSE's namingContexts (RFC 2251 3.4), and the form */
static OSErr root(Ldap *me)
{
    LdapOut o;
    long len, k, alen, vlen, n = 0;
    short op;
    char type[COLN], first[DNN];
    first[0] = 0;
    ldap_out(&o, me->req_buf, REQ);
    if (!ldap_search(&o, ++me->id, "", kLdapBase, 1, 0, "(objectClass=*)", "namingContexts")) return kGarbled;
    if (send_out(me, &o) != noErr) return kLost;
    page_head(me, me->host);
    outs(me, "<H2>");
    out_text(me, me->host, strlen(me->host));
    outs(me, "</H2>\n");
    while ((op = next_op(me, &len)) == 0x64 || op == 0x73) {
        long start = me->in.pos;
        if (op == 0x64) {
            ldap_tag(&me->in, &k);
            ldap_skip(&me->in, k);
            ldap_tag(&me->in, &alen);
            {
                long astart = me->in.pos;
                while (me->in.pos - astart < alen && !me->in.bad) {
                    ldap_tag(&me->in, &k);
                    ldap_tag(&me->in, &k);
                    ldap_bytes(&me->in, k, type, sizeof type);
                    ldap_tag(&me->in, &vlen);
                    {
                        long vstart = me->in.pos;
                        while (me->in.pos - vstart < vlen && !me->in.bad) {
                            ldap_tag(&me->in, &k);
                            ldap_bytes(&me->in, k, me->dn, sizeof me->dn);
                            if (!n++) strcpy(first, me->dn);
                            outf(me, "<IMG SRC=\"plugin://" LDAP_NAME "/rsrc?type=ics%%23&id=%d\" WIDTH=16 HEIGHT=16 ALIGN=TOP> ", kMillIconFolder, 0);
                            dn_link(me, me->dn, me->dn, 1);
                            outs(me, "<BR>\n");
                        }
                    }
                }
            }
        }
        k = len - (me->in.pos - start);
        if (k > 0) ldap_skip(&me->in, k);
    }
    if (!n) outs(me, "<P>The server names no directories.\n");
    ldap_form(H, me->r, me->host, first);
    return op == 0x65 || me->stopped ? noErr : kLost;
}

/* A search: its entries as records or a table */
static OSErr search(Ldap *me)
{
    LdapOut o;
    long len, n = 0, code = kSuccess, k, room = me->budget + 1024;
    short op = -1;
    OSErr err = noErr;
    char parent[DNN], *arena = NULL;
    if (me->table) {
        if ((err = columns(me)) != noErr) return err;
        if (!(arena = H->NewMem(room))) return memFullErr;
    }
    ldap_out(&o, me->req_buf, REQ);
    if (!ldap_search(&o, ++me->id, me->u.dn, me->u.scope, me->most, 0, me->u.filter, me->u.attrs)) {
        H->Fail(me->r, "The filter wasn\325t understood.");
        err = kDone;
        goto done;
    }
    if ((err = send_out(me, &o)) != noErr) goto done;
    H->Status(me->r, "Searching\311");
    while (!me->stopped) {
        if ((op = next_op(me, &len)) < 0) { err = kLost; goto done; }
        if (op == 0x65) { code = ldap_result(&me->in, len, me->msg, sizeof me->msg); break; }
        if (op != 0x64) { ldap_skip(&me->in, len); continue; }      /* a v3 reference */
        if (n >= me->most) { code = kSizeLimit; break; }            /* one more than Most records */
        if (!n) {
            /* the page starts with the first entry */
            page_head(me, me->u.dn[0] ? me->u.dn : me->host);
            for (k = 0; me->u.dn[k] && me->u.dn[k] != ','; k++) {}
            strcpy(parent, me->u.dn[k] ? me->u.dn + k + 1 : "");
            while (parent[0] == ' ') memmove(parent, parent + 1, strlen(parent));
            if (me->u.dn[0]) {
                outf(me, "<IMG SRC=\"plugin://" LDAP_NAME "/rsrc?type=ics%%23&id=%d\" WIDTH=16 HEIGHT=16 ALIGN=TOP> ", kMillIconParent, 0);
                if (parent[0]) dn_link(me, parent, parent, 1);
                else { outs(me, "<A HREF=\"/\">"); out_text(me, me->host, strlen(me->host)); outs(me, "</A>"); }
                outs(me, "\n");
            }
            outs(me, "<H2>");
            out_text(me, me->u.dn[0] ? me->u.dn : me->host, strlen(me->u.dn[0] ? me->u.dn : me->host));
            outs(me, "</H2>\n");
            if (me->u.scope != kLdapBase) {
                outs(me, "<P><B>Filter:</B> ");
                out_text(me, me->u.filter[0] ? me->u.filter : "(objectClass=*)", strlen(me->u.filter[0] ? me->u.filter : "(objectClass=*)"));
                outf(me, "<BR><B>Scope:</B> %s\n", (long)(me->u.scope == kLdapOne ? "one level" : "subtree"), 0);
            }
            if (me->table) {
                short i;
                outs(me, "<TABLE BORDER=1 CELLSPACING=0 CELLPADDING=2><TR><TH ALIGN=LEFT>DN</TH>");
                for (i = 0; i < me->ncols; i++) {
                    outs(me, "<TH ALIGN=LEFT>");
                    out_text(me, me->cols[i], strlen(me->cols[i]));
                    outs(me, "</TH>");
                }
                outs(me, "</TR>\n");
            }
        }
        if ((err = entry(me, len, arena, room)) != noErr) goto done;
        n++;
    }
    if (!n) {
        if (code != kSuccess && code != kSizeLimit) { refused(me, code); err = kDone; goto done; }
        page_head(me, me->u.dn[0] ? me->u.dn : me->host);
        outs(me, "<P>No records.\n");
        goto done;
    }
    if (me->table) outs(me, "</TABLE>\n");
    if (code == kSizeLimit) outf(me, "<P>First %ld records shown. Narrow the search to see the rest.\n", n, 0);
    else if (code == kTimeLimit) outs(me, "<P>The server stopped the search (time limit).\n");
    else if (me->u.scope != kLdapBase) outf(me, n == 1 ? "<P>%ld record.\n" : "<P>%ld records.\n", n, 0);
    if (op != 0x65) { H->TcpClose(me->c); me->c = 0; }    /* the rest isn't wanted */
done:
    if (arena) H->FreeMem(arena);
    return err;
}

/* ------------------------------------------------------------ a request */

static long number(const char *s)
{
    long n = 0;
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return n;
}

OSErr ldap_request(MillPlugIn *self, MillRequest *r)
{
    Ldap *me = (Ldap *)self;
    char v[16];
    long a[2], code;
    OSErr err;
    LdapOut o;
    me->req = r;
    me->r = r->ref;
    me->stopped = me->loggedIn = me->bound = 0;
    me->c = 0;
    me->id = 0;
    me->version = 2;
    memset(&me->in, 0, sizeof me->in);
    me->in.fill = tcp_fill;
    me->in.ref = me;
    strncpy(me->host, r->host, NAMEN - 1);
    me->host[NAMEN - 1] = 0;
    if (!ldap_url(r->path, &me->u)) { H->Fail(me->r, "The scope must be base, one, or sub."); return noErr; }
    me->table = me->u.table >= 0 ? me->u.table : H->GetSetting("ldap", "layout", v, sizeof v) > 0 && !strcmp(v, "Table");
    me->most = H->GetSetting("ldap", "most", v, sizeof v) > 0 ? number(v) : 100;
    if (me->most < 1) me->most = 1;
    me->budget = (H->GetSetting("ldap", "budget", v, sizeof v) > 0 ? number(v) : 1) * 1024L;
    if (me->budget < 1024) me->budget = 1024;
    /* who: the login kept for this server (its key: ldap://host:port), else anonymous */
    a[0] = (long)r->host;
    a[1] = r->port ? r->port : 389;
    H->Format(me->key, sizeof me->key, "ldap://%s:%d", a);
    me->user[0] = me->pass[0] = 0;
    H->SavedLogin(me->key, me->user, sizeof me->user, me->pass, sizeof me->pass);
    if (me->u.bindname[0] && !me->user[0]) { ask(me, 0); return noErr; }
    /* the connection kept from the last page, bound as this DN: its note is the last
       message ID and the version, "12 2" */
    {
        char note[24];
        long n = sizeof note - 1;
        if (H->TcpKept(me->r, me->user, &me->c, note, &n) == noErr) {
            const char *p = note;
            note[n] = 0;
            while (*p >= '0' && *p <= '9') me->id = me->id * 10 + (*p++ - '0');
            if (*p == ' ' && (p[1] == '2' || p[1] == '3')) { me->version = p[1] - '0'; memset(me->pass, 0, sizeof me->pass); goto in; }
            H->TcpClose(me->c);
            me->c = 0;
            me->id = 0;
        }
    }
    if ((err = H->TcpOpen(me->r, r->host, r->port ? r->port : 389, &me->c)) != noErr) { me->c = 0; return err; }
    H->Status(me->r, "Logging in\311");
    code = bind(me, me->user, me->pass);
    memset(me->pass, 0, sizeof me->pass);
    if (code < 0) { err = kLost; goto quit; }
    if (code == kInvalidCredentials && me->user[0]) { ask(me, 1); goto quit; }
    if (code != kSuccess) { refused(me, code); goto quit; }
in:
    me->loggedIn = me->user[0] != 0;
    me->bound = 1;
    if (!me->u.dn[0] && !strchr(r->path, '?')) err = root(me);
    else err = search(me);
quit:
    if (me->c && me->bound && (err == noErr || err == kDone) && !me->stopped) {
        /* every reply read: kept for the next page, with an unbind for Mill to send when it
           closes it (RFC 1777 4.4) */
        char note[24];
        long a2[2];
        a2[0] = me->id;
        a2[1] = me->version;
        H->Format(note, sizeof note, "%ld %d", a2);
        ldap_out(&o, me->req_buf, REQ);
        ldap_unbind(&o, me->id + 1);
        if (!o.full) { H->TcpKeep(me->r, me->c, me->user, note, strlen(note), o.p, o.n); me->c = 0; }
    }
    if (me->c) {
        ldap_out(&o, me->req_buf, REQ);
        ldap_unbind(&o, ++me->id);      /* RFC 1777 4.4 */
        send_out(me, &o);
        H->TcpClose(me->c);
    }
    me->c = 0;
    if (err == kLost || err == kGarbled) {
        H->Fail(me->r, err == kLost ? "The connection was lost." : "The server\325s reply was not understood.");
        err = noErr;
    }
    return err > 0 ? noErr : err;
}

static const MillSetting settings[] = {
    { "server", "Default server", "Fills in the form on plugin://" LDAP_NAME "/about", millText, "", 0, 0, NULL },
    { "base", "Default base", NULL, millText, "", 0, 0, NULL },
    { "layout", "Results as", NULL, millChoice, "Records", 0, 0, "Records|Table" },
    { "most", "Most records", NULL, millNumber, "100", 1, 1000, NULL },
    { "budget", "Maximum per record", NULL, millChoice, "1K", 0, 0, "1K|2K|4K|8K" }
};
static const MillProtocol protocols[] = { { "ldap", "LDAP", "Lightweight Directory Access Protocol (RFC 1777)", 0, 5, settings } };

const MillProtocol *ldap_protocol(void) { return protocols; }

MillPlugIn *ldap_state(const MillHost *host)
{
    Ldap *me = host->NewMem(sizeof *me);
    if (!me) return NULL;
    me->base.methods = NULL;
    me->base.host = host;
    return &me->base;
}
