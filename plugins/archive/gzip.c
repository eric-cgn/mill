/* gzip (RFC 1952) as a container of one member. The header (read here, as it arrives)
   gives the name (FNAME, else the archive's name less .gz), the date (MTIME), and a
   comment; the original size is the trailer's ISIZE, read with read_at when the host can.
   Opening the member inflates the deflate data after the header (Mill's, the host's
   GzData, as bare deflate) and checks the trailer's CRC-32 and ISIZE. Only the first member of a file of
   several is read. */
#include <string.h>

#include "archive.h"

#define TEXT_MAX 255

enum { FIXED, XLEN, EXTRA, FNAME, FCOMMENT, FHCRC, BODY, TRAILER, FINISHED };

typedef struct {
    ArcBase b;
    int state, flags;
    long got, left;
    unsigned char fixed[10], xlen[2], trailer[8];
    char name[TEXT_MAX + 1], comment[TEXT_MAX + 1];
    int nname, ncomment;
    long header;                /* the header's length */
    void *gz;
    unsigned long crc, count;
} Gzip;

void *gzip_begin(const ArcHost *h, const ArcRequest *r)
{
    Gzip *g = arc_new(h, sizeof *g);
    if (!g) return NULL;
    g->b.h = h;
    g->b.r = *r;
    return g;
}

void gzip_end(void *d)
{
    Gzip *g = d;
    if (g->gz) g->b.h->gz->end(g->gz);
    g->gz = NULL;
}

/* The member's name: the header's, else the archive's less .gz (.tgz is a .tar) */
static void member_name(Gzip *g, char *out, long size)
{
    int folder;
    const char *s, *base;
    long n;
    if (g->nname && arc_clean((const unsigned char *)g->name, g->nname, 0, out, size, &folder) > 0) {
        /* the header names a file; a path in it is the sender's business, so the last part */
        const char *slash = strrchr(out, '/');
        if (slash) memmove(out, slash + 1, strlen(slash));
        return;
    }
    s = g->b.r.name ? g->b.r.name : "";
    base = strrchr(s, '/');
    base = base ? base + 1 : s;
    n = (long)strlen(base);
    if (n > size - 1) n = size - 1;
    memcpy(out, base, (size_t)n);
    out[n] = 0;
    if (n > 3 && (!strcmp(out + n - 3, ".gz") || !strcmp(out + n - 3, ".GZ"))) out[n - 3] = 0;
    else if (n > 4 && (!strcmp(out + n - 4, ".tgz") || !strcmp(out + n - 4, ".TGZ"))) strcpy(out + n - 3, "tar");
    if (arc_clean((const unsigned char *)out, (long)strlen(out), 0, out, size, &folder) < 0) strcpy(out, "Untitled");
}

/* Its one member: listed, or (opening) told to the host before its bytes */
static int list(Gzip *g)
{
    const ArcHost *h = g->b.h;
    ArcEntry e;
    char name[TEXT_MAX + 1];
    unsigned long mtime = arc_le32(g->fixed + 4);
    member_name(g, name, sizeof name);
    arc_entry_init(&e);
    e.name = name;
    e.charset = ARC_LATIN1;
    e.kind = ARC_FILE;
    if (mtime) e.modified = arc_unix_date(mtime);
    if (g->ncomment) { g->comment[g->ncomment] = 0; e.comment = g->comment; }
    /* ISIZE, from the end, if it can be read there */
    if (h->read_at && h->size) {
        long size = h->size(h->ref);
        unsigned char t[4];
        if (size >= g->header + 8 && h->read_at(h->ref, (unsigned long)size - 4, t, 4) == 4) {
            e.size = (long)(arc_le32(t) & 0x7FFFFFFFUL);
            e.packed = size;
        }
    }
    if (g->b.r.open) return arc_member(h, &e, g->b.rest, NULL);
    return h->entry(h->ref, &e) ? ARC_STOPPED : ARC_DONE;
}

static int inflated(void *ref, const unsigned char *p, long n)
{
    Gzip *g = ref;
    g->crc = arc_crc32(g->crc, p, n);
    g->count += (unsigned long)n;
    return g->b.h->out(g->b.h->ref, p, n);
}

static int body_starts(Gzip *g)
{
    GzMem mem;
    if (!g->b.r.open) { g->state = FINISHED; return list(g); }
    g->b.rest = arc_first(g->b.r.path);
    if (list(g) != ARC_MORE) return ARC_STOPPED;
    mem.ref = g->b.h->ref;
    mem.alloc = g->b.h->alloc;
    mem.release = g->b.h->release;
    if (!(g->gz = g->b.h->gz->begin(g->b.h->gz->ref, &mem, 1, inflated, g))) return ARC_NO_MEMORY;
    g->state = BODY;
    return ARC_MORE;
}

/* After the header's fixed part, FEXTRA, FNAME, FCOMMENT: the next part it has */
static int next_part(Gzip *g, int after)
{
    if (after < EXTRA && (g->flags & 4)) { g->left = 2; g->got = 0; return XLEN; }
    if (after < FNAME && (g->flags & 8)) return FNAME;
    if (after < FCOMMENT && (g->flags & 16)) return FCOMMENT;
    if (after < FHCRC && (g->flags & 2)) { g->left = 2; return FHCRC; }
    return BODY;
}

int gzip_data(void *d, const unsigned char *p, long n)
{
    Gzip *g = d;
    int r;
    if (g->state == FINISHED) return ARC_DONE;
    if (n == 0) return ARC_BAD;     /* the end, before the member's */
    while (n > 0) {
        if (g->state == BODY) {
            long used;
            r = g->b.h->gz->data(g->gz, p, n);
            if (r == -2) return ARC_STOPPED;
            if (r < 0) return ARC_BAD;
            if (r == 0) return ARC_MORE;
            used = n - g->b.h->gz->unused(g->gz);
            p += used; n -= used;
            g->state = TRAILER;
            g->got = 0;
            continue;
        }
        if (g->state == TRAILER) {
            g->trailer[g->got++] = *p++; n--;
            if (g->got < 8) continue;
            g->state = FINISHED;
            if (arc_le32(g->trailer) != g->crc || arc_le32(g->trailer + 4) != (g->count & 0xFFFFFFFFUL)) return ARC_BAD;
            return ARC_DONE;
        }
        {
            unsigned char c = *p++;
            n--;
            g->header++;
            switch (g->state) {
            case FIXED:
                g->fixed[g->got++] = c;
                if ((g->got == 1 && c != 31) || (g->got == 2 && c != 139) || (g->got == 3 && c != 8) ||
                    (g->got == 4 && (c & 0xE0)))
                    return ARC_BAD;
                if (g->got < 10) break;
                g->flags = g->fixed[3];
                g->state = next_part(g, FIXED);
                break;
            case XLEN:
                g->xlen[g->got++] = c;
                if (g->got < 2) break;
                g->left = arc_le16(g->xlen);
                g->state = g->left ? EXTRA : next_part(g, EXTRA);
                break;
            case EXTRA:
                if (!--g->left) g->state = next_part(g, EXTRA);
                break;
            case FNAME:
                if (!c) { g->state = next_part(g, FNAME); break; }
                if (g->nname < TEXT_MAX) g->name[g->nname++] = (char)c;
                break;
            case FCOMMENT:
                if (!c) { g->state = next_part(g, FCOMMENT); break; }
                if (g->ncomment < TEXT_MAX) g->comment[g->ncomment++] = (char)(c < 32 ? ' ' : c);
                break;
            case FHCRC:
                if (!--g->left) g->state = BODY;
                break;
            }
            if (g->state == BODY && (r = body_starts(g)) != ARC_MORE) return r;
        }
    }
    return ARC_MORE;
}
