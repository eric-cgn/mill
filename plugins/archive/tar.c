/* tar: POSIX.1-1988 ustar headers (IEEE Std 1003.1-1988, section 10.1.1) and the older
   Version 7 Unix ones they extend. Each member is a 512-byte header, then its data padded
   to 512; two zero blocks (or simply the end) end the archive. There is no directory, so
   listing reads every header: pushed (in order, skipping the data as it goes by) or
   pulled (hopping from header to header with read_at). */
#include <string.h>

#include "archive.h"

#define BLOCK 512
#define NAME_MAX_ 257       /* ustar: a 155-byte prefix, '/', and a 100-byte name */

typedef struct {
    int kind, ok;           /* ok: a member to report (not a device or an unknown type) */
    long size;              /* the data that follows the header */
    char name[NAME_MAX_ + 1], link[101];
    ArcEntry e;
} TarHead;

enum { HEAD, SKIP, DATA, FINISHED };

typedef struct {
    ArcBase b;
    int state;
    long got, left;         /* header bytes in; data still to skip or give */
    unsigned long at;       /* where the header being read starts */
    unsigned char hdr[BLOCK];
    TarHead t;
} Tar;

/* An octal field: digits after any spaces, up to a space or NUL; -1 if there are none, or
   for a number past 2^31 - 1 (or GNU's base-256, from after 1997) */
static long octal(const unsigned char *p, int n)
{
    unsigned long v = 0;
    int i = 0, digits = 0;
    while (i < n && p[i] == ' ') i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++, digits++) {
        if (v > 0x0FFFFFFFUL) return -1;
        v = v * 8 + (unsigned long)(p[i] - '0');
    }
    if (!digits || (i < n && p[i] != ' ' && p[i] != 0)) return -1;
    return v > 0x7FFFFFFFUL ? -1 : (long)v;
}

/* 1: a header, read into t; 0: a zero block (the end); -1: not a header */
static int parse(const unsigned char *h, TarHead *t, unsigned long at)
{
    unsigned long sum = 0;
    long ssum = 0, chk, mode, mtime;
    int i, n, folder, type = h[156];
    char raw[NAME_MAX_ + 1];
    for (i = 0; i < BLOCK && !h[i]; i++) {}
    if (i == BLOCK) return 0;
    /* the checksum: every byte, with its own field as spaces; some old tars summed signed chars */
    for (i = 0; i < BLOCK; i++) {
        int c = i >= 148 && i < 156 ? ' ' : h[i];
        sum += (unsigned long)c;
        ssum += c > 127 ? c - 256 : c;
    }
    chk = octal(h + 148, 8);
    if (chk < 0 || ((unsigned long)chk != sum && chk != ssum)) return -1;
    memset(t, 0, sizeof *t);
    arc_entry_init(&t->e);
    n = 0;
    /* ustar's prefix: only with POSIX's magic ("ustar" NUL); GNU's "ustar  " puts other things there */
    if (!memcmp(h + 257, "ustar", 6) && h[345]) {
        for (i = 0; i < 155 && h[345 + i]; i++) raw[n++] = (char)h[345 + i];
        raw[n++] = '/';
    }
    for (i = 0; i < 100 && h[i]; i++) raw[n++] = (char)h[i];
    if (arc_clean((const unsigned char *)raw, n, 0, t->name, sizeof t->name, &folder) < 0) {
        t->name[0] = 0;     /* refused: skipped, never listed or opened */
        folder = 0;
    }
    mode = octal(h + 100, 8);
    mtime = octal(h + 136, 12);
    t->size = octal(h + 124, 12);
    if (t->size < 0) return -1;
    switch (type) {
    case '0': case 0: case '7':     /* a file (V7: NUL; '7' contiguous, read as a file) */
        t->kind = folder ? ARC_FOLDER : ARC_FILE;
        t->ok = 1;
        break;
    case '5':
        t->kind = ARC_FOLDER;
        t->ok = 1;
        t->size = 0;
        break;
    case '1': case '2':             /* hard and symbolic links: the target, no data */
        t->kind = ARC_LINK;
        t->ok = 1;
        t->size = 0;
        for (i = 0; i < 100 && h[157 + i]; i++) t->link[i] = h[157 + i] < 32 ? '?' : (char)h[157 + i];
        t->link[i] = 0;
        break;
    case '3': case '4': case '6':   /* devices and FIFOs: no data, not listed */
        t->size = 0;
        break;
    default:                        /* anything else: skipped, data and all */
        break;
    }
    if (!t->name[0]) t->ok = 0;
    t->e.name = t->name;
    t->e.charset = ARC_LATIN1;
    t->e.kind = t->kind;
    if (t->kind == ARC_FILE) t->e.size = t->e.packed = t->size;
    if (mtime > 0) t->e.modified = arc_unix_date((unsigned long)mtime);
    if (mode >= 0) t->e.mode = mode & 07777;
    if (t->kind == ARC_LINK) t->e.link = t->link;
    t->e.pos = at;
    return 1;
}

static long padded(long n) { return (n + BLOCK - 1) / BLOCK * BLOCK; }

/* What a header means for the request: ARC_MORE to go on past it, 1 to give its data, or
   an error or ARC_STOPPED */
static int consider(ArcBase *b, TarHead *t)
{
    const char *rest;
    if (!t->ok) return ARC_MORE;
    if (!b->r.open) return b->h->entry(b->h->ref, &t->e) ? ARC_STOPPED : ARC_MORE;
    if (t->kind == ARC_FOLDER || !(rest = arc_on_path(t->name, b->r.path))) return ARC_MORE;
    if (t->kind == ARC_LINK) return ARC_UNSUPPORTED;
    b->rest = rest;
    return arc_member(b->h, &t->e, rest, NULL) == ARC_MORE ? 1 : ARC_STOPPED;
}

/* ---- push ---- */

void *tar_begin(const ArcHost *h, const ArcRequest *r)
{
    Tar *t = arc_new(h, sizeof *t);
    if (!t) return NULL;
    t->b.h = h;
    t->b.r = *r;
    return t;
}

int tar_data(void *d, const unsigned char *p, long n)
{
    Tar *t = d;
    int r;
    if (t->state == FINISHED) return ARC_DONE;
    if (n == 0) {
        /* the end: fine between members (some tars leave out the zero blocks) */
        if (t->state == HEAD && t->got == 0) return t->b.r.open ? ARC_NOT_FOUND : ARC_DONE;
        return ARC_BAD;
    }
    while (n > 0) {
        long k;
        switch (t->state) {
        case HEAD:
            k = BLOCK - t->got < n ? BLOCK - t->got : n;
            memcpy(t->hdr + t->got, p, (size_t)k);
            t->got += k; p += k; n -= k;
            if (t->got < BLOCK) break;
            t->got = 0;
            r = parse(t->hdr, &t->t, t->at);
            if (r < 0) return ARC_BAD;
            if (r == 0) { t->state = FINISHED; return t->b.r.open ? ARC_NOT_FOUND : ARC_DONE; }
            r = consider(&t->b, &t->t);
            if (r < 0) return r;
            t->at += BLOCK + (unsigned long)padded(t->t.size);
            if (r == 1) {
                t->state = DATA;
                t->left = t->t.size;
                if (!t->left) { t->state = FINISHED; return ARC_DONE; }
            } else {
                t->left = padded(t->t.size);
                t->state = t->left ? SKIP : HEAD;
            }
            break;
        case SKIP:
            k = t->left < n ? t->left : n;
            p += k; n -= k;
            if (!(t->left -= k)) t->state = HEAD;
            break;
        case DATA:
            k = t->left < n ? t->left : n;
            if (t->b.h->out(t->b.h->ref, p, k)) return ARC_STOPPED;
            p += k; n -= k;
            if (!(t->left -= k)) { t->state = FINISHED; return ARC_DONE; }
            break;
        }
    }
    return ARC_MORE;
}

/* ---- pull ---- */

/* Give n bytes at offset to out, through buf (BLOCK bytes) */
static int give(const ArcHost *h, unsigned long at, long n, unsigned char *buf)
{
    while (n > 0) {
        long k = n < BLOCK ? n : BLOCK, got = h->read_at(h->ref, at, buf, k);
        if (got < 0) return ARC_READ;
        if (got < k) return ARC_BAD;
        if (h->out(h->ref, buf, k)) return ARC_STOPPED;
        at += (unsigned long)k;
        n -= k;
    }
    return ARC_DONE;
}

/* The header at offset: 1, 0 at the end (a zero block, or nothing more), or an error */
static int header_at(Tar *t, unsigned long at, long size)
{
    long got = t->b.h->read_at(t->b.h->ref, at, t->hdr, BLOCK);
    int r;
    if (got < 0) return ARC_READ;
    if (got == 0) return 0;
    if (got < BLOCK) return ARC_BAD;
    r = parse(t->hdr, &t->t, at);
    if (r < 0) return ARC_BAD;
    /* data past the end of the archive */
    if (r && size >= 0 && (unsigned long)size - at - BLOCK < (unsigned long)t->t.size) return ARC_BAD;
    return r;
}

int tar_pull(const ArcHost *h, const ArcRequest *r, const char **rest)
{
    Tar *t = arc_new(h, sizeof *t);
    unsigned long at = 0;
    long size = h->size(h->ref);
    int res;
    if (!t) return ARC_NO_MEMORY;
    t->b.h = h;
    t->b.r = *r;
    /* a position from the listing: the header there, if it's the one */
    if (r->open && r->has_pos && r->pos % BLOCK == 0 && (size < 0 || r->pos < (unsigned long)size) &&
        header_at(t, r->pos, size) == 1 && (res = consider(&t->b, &t->t)) == 1) {
        res = give(h, r->pos + BLOCK, t->t.size, t->hdr);
        goto done;
    }
    for (;;) {
        if (size >= 0 && at >= (unsigned long)size) { res = 0; break; }
        if ((res = header_at(t, at, size)) <= 0) break;
        res = consider(&t->b, &t->t);
        if (res == 1) { res = give(h, at + BLOCK, t->t.size, t->hdr); goto done; }
        if (res < 0) break;
        at += BLOCK + (unsigned long)padded(t->t.size);
    }
    if (res == 0) res = r->open ? ARC_NOT_FOUND : ARC_DONE;
done:
    if (res == ARC_DONE && r->open) *rest = t->b.rest;
    arc_free(h, t);
    return res;
}
