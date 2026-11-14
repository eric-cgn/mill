/* MillGlue.c: MillMain for a Plug-In, so you only write PlugInNew and your methods
   (see MillPlugIn.h). Add this file to your Plug-In's project as it is.

   It does what each compiler needs before a code resource's globals and static
   data can be used, then calls your functions with self. Mill built-ins include it
   with MILL_ENTRY and MILL_NEW defined, to give each its own names. */

#include "MillPlugIn.h"
#include <Memory.h>
#include <Resources.h>

#ifndef MILL_ENTRY
#ifdef __MWERKS__
#define MILL_ENTRY main     /* its code resources start at main */
#else
#define MILL_ENTRY MillMain
#endif
#define MILL_NEW PlugInNew
#endif

#if defined(MILL_BUILTIN)
    /* compiled into Mill: its globals are Mill's own, already set up */
#   define GLUE_ENTER()
#   define GLUE_LEAVE()
/* KIT-BEGIN-REPO-ONLY (kit.py leaves this block out of the disk image) */
#elif defined(__GNUC__) && defined(macintosh)
    /* Retro68 (GCC): a code resource built with -Wl,--mac-flat fixes up its own
       addresses the first time it runs. Link with -Wl,-eMILLMAIN: pascal names
       are upper case. See examples/finger. */
#   include <Retro68Runtime.h>
#   define GLUE_ENTER() RETRO68_RELOCATE()
#   define GLUE_LEAVE()
/* KIT-END-REPO-ONLY */
#elif defined(__MWERKS__)
    /* CodeWarrior: globals through A4 (A4Stuff.h). Tested with CW4. */
#   include <A4Stuff.h>
#   define GLUE_ENTER() long glue_a4 = SetCurrentA4()
#   define GLUE_LEAVE() SetA4(glue_a4)
#else
    /* MPW: no globals; SC -b keeps strings in the code. Tested with MPW 3.5. */
#   define GLUE_ENTER()
#   define GLUE_LEAVE()
#endif

MillPlugIn *MILL_NEW(const MillHost *host, short resFile, MillInfo *info);

#if defined(MILL_BUILTIN)
    /* A built-in's code is a segment that Mill unloads and loads again, maybe
       somewhere else, so no function pointers into it are kept: its methods are
       called by name (MILL_CLOSE, MILL_REQUEST, and MILL_PURGE, if it has one) */
static void MILL_CLOSE(MillPlugIn *self);
#   define GLUE_CLOSE(me) MILL_CLOSE(me)
#   ifdef MILL_REQUEST
static OSErr MILL_REQUEST(MillPlugIn *self, MillRequest *r);
#       define GLUE_REQUEST(me, r) MILL_REQUEST(me, r)
#   else
#       define GLUE_REQUEST(me, r) millUnknown
#   endif
#   ifdef MILL_FORMAT
static OSErr MILL_FORMAT(MillPlugIn *self, short selector, MillFormatParams *f);
#       define GLUE_FORMAT(me, s, f) MILL_FORMAT(me, s, f)
#   else
#       define GLUE_FORMAT(me, s, f) millUnknown
#   endif
#   ifdef MILL_PURGE
static long MILL_PURGE(MillPlugIn *self, long wanted);
#       define GLUE_PURGE(me, w) MILL_PURGE(me, w)
#   else
#       define GLUE_PURGE(me, w) 0
#   endif
#else
#   define GLUE_CLOSE(me) (me)->methods->close(me)
#   define GLUE_REQUEST(me, r) ((me)->methods->request ? (me)->methods->request(me, r) : millUnknown)
#   define GLUE_FORMAT(me, s, f) ((me)->methods->format ? (me)->methods->format(me, s, f) : millUnknown)
#   define GLUE_PURGE(me, w) ((me)->methods->purge ? (me)->methods->purge(me, w) : 0)
#endif

static void glue_answer(const MillHost *h, MillRequest *r, short code, const char *why);
static void glue_rsrc(const MillHost *h, MillRequest *r);

/* The entry point. It comes first in the file, so it's first in the code
   resource with compilers that put functions in order (MPW). */
pascal OSErr MILL_ENTRY(short selector, void *params, void **self)
{
    MillPlugIn *me;
    OSErr err = noErr;
    GLUE_ENTER();
    me = (MillPlugIn *)*self;
    switch (selector) {
    case millOpen: {
        MillOpenParams *op = (MillOpenParams *)params;
        /* a Mill older than this kit: say which interface is needed */
        if (op->interface < kMillInterface) { op->interface = kMillInterface; err = millUnknown; break; }
        me = MILL_NEW(op->host, op->resFile, &op->info);
        if (!me) { err = memFullErr; break; }
        me->host = op->host;
        me->resFile = op->resFile;
        *self = me;
        break;
    }
    case millClose:
        if (me) GLUE_CLOSE(me);
        *self = 0;
        break;
    case millRequest: {
        MillRequest *r = (MillRequest *)params;
        const char *p = r->path;
        if (!me) { err = paramErr; break; }
        if (r->protocol < 0 && p[0] == '/' && p[1] == 'r' && p[2] == 's' && p[3] == 'r' && p[4] == 'c' &&
            (p[5] == '?' || !p[5])) { glue_rsrc(me->host, r); break; }
        err = GLUE_REQUEST(me, r);
        if (err == millUnknown && r->protocol < 0) { glue_answer(me->host, r, 404, "Not Found"); err = noErr; }
        break;
    }
    case millPurge: {
        MillPurgeParams *mp = (MillPurgeParams *)params;
        mp->freed = me ? GLUE_PURGE(me, mp->wanted) : 0;
        break;
    }
    case millFormatBegin: case millFormatData: case millFormatEnd:
        err = me ? GLUE_FORMAT(me, selector, (MillFormatParams *)params) : paramErr;
        break;
    default:
        err = millUnknown;
    }
    GLUE_LEAVE();
    return err;
}

static void glue_answer(const MillHost *h, MillRequest *r, short code, const char *why)
{
    long a[3];
    a[0] = code;
    a[1] = (long)why;
    a[2] = (long)why;
    h->Printf(r->ref, "HTTP/1.0 %d %s" MILL_CRLF "Content-Type: text/html" MILL_CRLF MILL_CRLF "<TITLE>%s</TITLE>", a);
}

/* plugin://NAME/rsrc?type=TYPE&id=ID or &name=NAME */
static void glue_rsrc(const MillHost *h, MillRequest *r)
{
    const char *q = r->path + 5;
    char type[16], id[16], name[64];
    long n, len, a[2];
    short has_id, has_name, from = h->From(r->ref);
    Str255 pname;
    Handle res;
    ResType t;
    if (from != millFromNone && from != millFromOwnPage && from != millFromOwn && from != millFromLocal) { glue_answer(h, r, 403, "Forbidden"); return; }
    if (*q == '?') q++;
    len = 0;
    while (q[len]) len++;
    has_id = h->FormField(q, len, "id", id, sizeof id) > 0;
    has_name = h->FormField(q, len, "name", name, sizeof name) > 0;
    n = h->FormField(q, len, "type", type, sizeof type);
    if (n < 1 || n > 4 || has_id == has_name) { glue_answer(h, r, 400, "Bad Request"); return; }
    while (n < 4) type[n++] = ' ';
    t = ((ResType)(unsigned char)type[0] << 24) | ((ResType)(unsigned char)type[1] << 16) |
        ((ResType)(unsigned char)type[2] << 8) | (unsigned char)type[3];
    if (has_id) {
        long v = 0;
        const char *c = id;
        short neg = *c == '-';
        if (neg) c++;
        while (*c >= '0' && *c <= '9') v = v * 10 + (*c++ - '0');
        res = GetResource(t, (short)(neg ? -v : v));
    } else {
        n = 0;
        while (name[n] && n < 255) { pname[n + 1] = name[n]; n++; }
        pname[0] = (unsigned char)n;
        res = GetNamedResource(t, pname);
    }
    if (!res) { glue_answer(h, r, 404, "Not Found"); return; }
    type[4] = 0;
    n = 4;
    while (n > 0 && type[n - 1] == ' ') type[--n] = 0;
    a[0] = (long)type;
    a[1] = GetHandleSize(res);
    h->Printf(r->ref, "HTTP/1.0 200 OK" MILL_CRLF "Content-Type: x-rsrc/%s" MILL_CRLF
              "Content-Length: %ld" MILL_CRLF MILL_CRLF, a);
    HLock(res);
    h->Write(r->ref, *res, a[1]);
    HUnlock(res);
    if (HomeResFile(res) != 0) ReleaseResource(res);   /* the System's stay as they are */
}
