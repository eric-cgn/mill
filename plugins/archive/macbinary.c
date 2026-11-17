/* MacBinary I (1985), II (1987), and III (1996): one Mac file, a 128-byte header (read
   by src/core/filetypes.c's macbinary_header, which knows II and III by the header's
   CRC-16), then the data fork and the resource fork, each padded to 128 bytes. Listing
   needs only the header; opening gives one fork (the data fork, or with ARC_RSRC_FORK the
   resource fork). */
#include <string.h>

#include "archive.h"
#include "../../src/core/filetypes.h"

enum { HEAD, SKIP, FORK, FINISHED };

typedef struct {
    ArcBase b;
    int state;
    long got, at, left;     /* header bytes in; where the input is; fork bytes to give */
    long rsrc_at, rsrc_len; /* ARC_BOTH_FORKS: the resource fork, after the data fork */
    unsigned char h[128];
} MacBin;

void *macbinary_begin(const ArcHost *h, const ArcRequest *r)
{
    MacBin *m = arc_new(h, sizeof *m);
    if (!m) return NULL;
    m->b.h = h;
    m->b.r = *r;
    return m;
}

/* To the fork in got, left bytes of it (none: on to the resource fork, if both are wanted) */
static int next_fork(MacBin *m)
{
    if (!m->left) {
        if (!m->rsrc_len) { m->state = FINISHED; return ARC_DONE; }
        m->left = m->rsrc_len;
        m->got = m->rsrc_at;
        m->rsrc_len = 0;
    }
    m->state = m->got > m->at ? SKIP : FORK;
    return ARC_MORE;
}

static int header(MacBin *m)
{
    MacBinary mb;
    ArcEntry e;
    char name[64];
    if (macbinary_header(m->h, &mb)) return ARC_BAD;
    {
        arc_entry_init(&e);
        arc_mac_name((const unsigned char *)mb.name, (int)strlen(mb.name), name);
        e.name = name;
        e.charset = ARC_MACROMAN;
        e.kind = ARC_FILE;
        e.size = mb.data_len;
        e.packed = mb.rsrc_at + (mb.rsrc_len + 127) / 128 * 128;
        e.modified = arc_be32(m->h + 95);
        memcpy(e.type, mb.type, 4);
        memcpy(e.creator, mb.creator, 4);
        e.finder_flags = mb.flags;
        e.data_len = mb.data_len;
        e.rsrc_len = mb.rsrc_len;
        e.flags = ARC_FINDER;
        if (!m->b.r.open) {
            m->state = FINISHED;
            return m->b.h->entry(m->b.h->ref, &e) ? ARC_STOPPED : ARC_DONE;
        }
        m->b.rest = arc_first(m->b.r.path);
        if (arc_member(m->b.h, &e, m->b.rest, &m->b.r.fork) != ARC_MORE) return ARC_STOPPED;
    }
    m->rsrc_at = mb.rsrc_at;
    m->rsrc_len = m->b.r.fork == ARC_BOTH_FORKS ? mb.rsrc_len : 0;
    if (m->b.r.fork == ARC_RSRC_FORK) { m->left = mb.rsrc_len; m->got = mb.rsrc_at; }
    else { m->left = mb.data_len; m->got = mb.data_at; }
    return next_fork(m);
}

int macbinary_data(void *d, const unsigned char *p, long n)
{
    MacBin *m = d;
    int r;
    if (m->state == FINISHED) return ARC_DONE;
    if (n == 0) return ARC_BAD;     /* cut short */
    while (n > 0) {
        long k;
        switch (m->state) {
        case HEAD:
            k = 128 - m->got < n ? 128 - m->got : n;
            memcpy(m->h + m->got, p, (size_t)k);
            m->got += k; m->at += k; p += k; n -= k;
            if (m->got == 128 && (r = header(m)) != ARC_MORE) return r;
            break;
        case SKIP:  /* to the fork, at got */
            k = m->got - m->at < n ? m->got - m->at : n;
            m->at += k; p += k; n -= k;
            if (m->at == m->got) m->state = FORK;
            break;
        case FORK:
            k = m->left < n ? m->left : n;
            if (m->b.h->out(m->b.h->ref, p, k)) return ARC_STOPPED;
            p += k; n -= k;
            m->at += k;
            if (!(m->left -= k) && (r = next_fork(m)) != ARC_MORE) return r;
            break;
        }
    }
    return ARC_MORE;
}
