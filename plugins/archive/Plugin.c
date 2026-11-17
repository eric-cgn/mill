/* Archives: a Plug-In for Mill that browses zip, tar, gzip, MacBinary, and BinHex 4.0
   archives (the cores are archive.h's). Mill makes the listing page from Entry calls and
   opens a member by its "#/" address; zip and tar are read by parts (ReadAt) where the
   source allows, the others as they arrive. Its About page is its 'TEXT' 128. */
#include <string.h>
#include <Memory.h>
#include "MillPlugIn.h"
#include "archive.h"
#include "../gzhost/gzhost.h"

typedef struct {
    MillPlugIn base;
    MillMethods methods;        /* filled in by PlugInNew (see MillPlugIn.h's rules) */
    MillFormat formats[ARC_FORMATS];
} Archives;

/* One run */
typedef struct {
    const MillHost *host;
    MillFormatParams *f;
    ArcHost h;
    GzHost gz;                  /* Mill's inflate, for zip and gzip */
    ArcRequest r;
    void *d;                    /* a push core's state */
    int done;
    char name[64];              /* gzip: the archive's own name, for its member's */
} Run;

static void *a_alloc(void *ref, long n) { return ((Run *)ref)->host->NewMem(n); }
static void a_release(void *ref, void *p) { ((Run *)ref)->host->FreeMem(p); }

static OSType os_type(const char *c)
{
    return ((OSType)(unsigned char)c[0] << 24) | ((OSType)(unsigned char)c[1] << 16) |
           ((OSType)(unsigned char)c[2] << 8) | (OSType)(unsigned char)c[3];
}

/* An archive's member as Mill's Entry */
static OSErr to_mill(Run *x, const ArcEntry *e)
{
    MillEntry m;
    memset(&m, 0, sizeof m);
    m.name = e->name;
    m.charset = e->charset == ARC_CP437 ? millCP437 : e->charset == ARC_MACROMAN ? millMacRoman : millLatin1;
    m.kind = e->kind == ARC_FOLDER ? millMemberFolder : e->kind == ARC_LINK ? millMemberLink : millMemberFile;
    m.size = e->size;
    m.packed = e->packed;
    m.modified = e->modified;
    m.mode = e->mode;
    m.comment = e->comment;
    if (e->flags & ARC_FINDER) {
        m.flags |= millMemberMac;
        m.fileType = os_type(e->type);
        m.creator = os_type(e->creator);
        m.finderFlags = e->finder_flags;
        m.dataLen = e->data_len;
        m.rsrcLen = e->rsrc_len;
    }
    if (e->flags & (ARC_ENCRYPTED | ARC_CANT_OPEN)) m.flags |= millMemberClosed;
    m.pos = e->pos;
    return x->host->Entry(x->f->ref, &m);
}

static int a_entry(void *ref, const ArcEntry *e) { return to_mill(ref, e) != noErr; }

/* The member being opened: what's left of the path, then Mill's Entry (which may ask for
   both forks, to save a Mac file) */
static int a_member(void *ref, const ArcEntry *e, const char *rest, int *fork)
{
    Run *x = ref;
    x->f->rest = rest;
    x->f->forks = 0;
    if (to_mill(x, e) != noErr) return 1;
    if (x->f->forks) *fork = ARC_BOTH_FORKS;
    return 0;
}

static int a_out(void *ref, const unsigned char *p, long n)
{
    Run *x = ref;
    return !x->host->Write((MillReqRef)x->f->ref, p, n);
}

static long a_read_at(void *ref, unsigned long at, unsigned char *p, long n)
{
    Run *x = ref;
    return x->host->ReadAt(x->f->ref, at, p, n);
}

static long a_size(void *ref) { Run *x = ref; return x->host->Size(x->f->ref); }

/* Whether the first bytes can be this format (a wrong type or name is saved, not read) */
static int looks_right(int format, const unsigned char *p, long n)
{
    switch (format) {
    case ARC_ZIP: return n >= 4 && p[0] == 'P' && p[1] == 'K' && (p[2] == 3 || p[2] == 5) && (p[3] == 4 || p[3] == 6);
    case ARC_GZIP: return n >= 3 && p[0] == 0x1F && p[1] == 0x8B && p[2] == 8;
    case ARC_MACBINARY: return n >= 128 && p[0] == 0 && p[1] >= 1 && p[1] <= 63 && p[74] == 0;
    case ARC_TAR: return n >= 512 || n == 0;
    }
    return 1;   /* BinHex: its line may come after a mail's headers */
}

/* gzip's own name, for its member when the header has none: the last name in the address
   before this format's part of the path ("kit.tgz", or "b.gz" in "a.zip#/b.gz/") */
static void own_name(Run *x)
{
    const MillHost *H = x->host;
    const char *u = x->f->url, *h = strchr(u, '#'), *s, *e;
    char buf[300];
    long n, keep;
    buf[0] = 0;
    if (h && h[1] == '/' && strlen(x->f->path) < strcspn(h + 2, "#")) {
        /* inside another archive: the part of the path before this format's */
        s = h + 2;
        e = s + strcspn(s, "#");
        n = e - s < (long)sizeof buf - 1 ? e - s : (long)sizeof buf - 1;
        memcpy(buf, s, n);
        buf[n] = 0;
        H->UrlDecode(buf, buf, sizeof buf);
        keep = (long)strlen(buf) - (long)strlen(x->f->path);
        if (keep < 0) keep = 0;
        buf[keep] = 0;
        while (keep > 0 && buf[keep - 1] == '/') buf[--keep] = 0;
    }
    if (!buf[0] || !(h && h[1] == '/' && strlen(x->f->path) < strcspn(h + 2, "#"))) {
        /* the archive itself: the address's last name */
        e = h ? h : u + strlen(u);
        if ((s = strchr(u, '?')) != NULL && s < e) e = s;
        n = e - u < (long)sizeof buf - 1 ? e - u : (long)sizeof buf - 1;
        memcpy(buf, u, n);
        buf[n] = 0;
        H->UrlDecode(buf, buf, sizeof buf);
    }
    s = strrchr(buf, '/');
    s = s ? s + 1 : buf;
    n = (long)strlen(s) < (long)sizeof x->name - 1 ? (long)strlen(s) : (long)sizeof x->name - 1;
    memcpy(x->name, s, n);
    x->name[n] = 0;
}

static Run *new_run(MillPlugIn *self, MillFormatParams *f)
{
    Run *x = self->host->NewMem(sizeof *x);
    size_t n;
    if (!x) return NULL;
    memset(x, 0, sizeof *x);
    x->host = self->host;
    x->f = f;
    x->h.ref = x;
    x->h.alloc = a_alloc;
    x->h.release = a_release;
    x->h.entry = a_entry;
    x->h.out = a_out;
    x->h.member = a_member;
    gzhost_init(&x->gz, self->host, (MillReqRef)f->ref);
    x->h.gz = &x->gz.calls;
    if (f->seekable) { x->h.read_at = a_read_at; x->h.size = a_size; }
    n = strlen(f->path);
    x->r.open = n > 0 && f->path[n - 1] != '/';
    x->r.path = f->path;
    x->r.pos = f->pos;
    x->r.has_pos = f->hasPos;
    x->r.fork = ARC_DATA_FORK;
    if (f->format == ARC_GZIP) { own_name(x); x->r.name = x->name; }
    return x;
}

static OSErr result(int r)
{
    switch (r) {
    case ARC_MORE: return noErr;
    case ARC_DONE: return millDone;
    case ARC_STOPPED: return millDone;
    case ARC_NOT_FOUND: return fnfErr;
    case ARC_NO_MEMORY: return memFullErr;
    case ARC_READ: return ioErr;
    }
    return paramErr;    /* not this format, cut short, damaged, or packed a way not read */
}

static OSErr archives_format(MillPlugIn *self, short selector, MillFormatParams *f)
{
    Run *x = f->state;
    const char *rest;
    int r;
    switch (selector) {
    case millFormatBegin: {
        int open = f->path[0] && f->path[strlen(f->path) - 1] != '/';
        if (!looks_right(f->format, (const unsigned char *)f->data, f->len)) { f->answer = millSaveOnly; return noErr; }
        if (!(x = new_run(self, f))) return memFullErr;
        f->state = x;
        switch (f->format) {
        case ARC_ZIP:
            /* its directory is at the end: read by parts, or not at all */
            f->answer = f->seekable ? millBrowseNow : millBrowseAfterSaving;
            f->pull = f->seekable;
            break;
        case ARC_TAR:
            /* no directory: every header in turn, skipping by parts; from a stream, a member
               can still be read in order, but not a listing */
            f->answer = f->seekable || open ? millBrowseNow : millBrowseAfterSaving;
            f->pull = f->seekable;
            break;
        default:
            /* one member, described by the header: from anywhere */
            f->answer = millBrowseNow;
        }
        return noErr;
    }
    case millFormatData:
        if (!x) return paramErr;
        if (!x->d && !(x->d = arc_begin(f->format, &x->h, &x->r))) return memFullErr;
        r = arc_data(x->d, (const unsigned char *)f->data, f->len);
        if (r != ARC_MORE) x->done = 1;
        return result(r);
    case millFormatPull:
        if (!x) return paramErr;
        r = arc_pull(f->format, &x->h, &x->r, &rest);
        x->done = 1;
        return r == ARC_DONE || r == ARC_STOPPED ? noErr : result(r);
    case millFormatEnd:
        if (!x) return noErr;
        if (x->d) {
            if (!x->done && !f->cut) arc_data(x->d, (const unsigned char *)"", 0);  /* the end of the input */
            arc_end(x->d);
        }
        self->host->FreeMem(x);
        f->state = NULL;
        return noErr;
    }
    return millUnknown;
}

static void archives_close(MillPlugIn *self) { self->host->FreeMem(self); }

static void format(MillFormat *f, const char *key, const char *heading, const char *description,
                   const char *types, const char *extensions, const char *fileTypes)
{
    f->key = key;
    f->heading = heading;
    f->description = description;
    f->types = types;
    f->extensions = extensions;
    f->fileTypes = fileTypes;
    f->gives = millGivesMembers;
    f->settingCount = 0;
    f->settings = 0;
}

MillPlugIn *PlugInNew(const MillHost *host, short resFile, MillInfo *info)
{
    Archives *me = host->NewMem(sizeof *me);
    MillFormat *f;
    (void)resFile;
    if (!me) return 0;
    me->methods.close = archives_close;
    me->methods.request = 0;
    me->methods.purge = 0;
    me->methods.format = archives_format;
    me->methods.font = 0;
    me->methods.gzOut = gzhost_out;
    me->base.methods = &me->methods;
    f = me->formats;
    /* in archive.h's order (ARC_ZIP...) */
    format(f++, "zip", "zip", "PKZIP archive (stored and deflated)", "application/zip application/x-zip-compressed", "zip", "pZIP");
    format(f++, "tar", "tar", "Tape archive (ustar and V7)", "application/x-tar application/tar", "tar", "TARF");
    format(f++, "gzip", "gzip", "GNU zip (RFC 1952)", "application/gzip application/x-gzip", "gz tgz", "Gzip");
    format(f++, "macbinary", "MacBinary", "MacBinary I, II, and III", "application/macbinary application/x-macbinary", "bin", "");
    format(f++, "binhex", "BinHex", "BinHex 4.0", "application/mac-binhex40 application/x-binhex40", "hqx", "");
    info->name = "archives";
    info->version = "1.0";
    info->copyright = "Copyright 2026 OneInAMillion Software";
    info->by = "OneInAMillion Software";
    info->description = "Zip, tar, gzip, MacBinary, and BinHex archives.";
    info->flags = 0;
    info->protocolCount = 0;
    info->protocols = 0;
    info->formatCount = ARC_FORMATS;
    info->formats = me->formats;
    return &me->base;
}
