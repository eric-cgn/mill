/* zip, as PKWARE's APPNOTE.TXT describes it for PKZIP 2.04g (1993): pulled with read_at.
   The end of central directory record (found from the end, past any comment) says where
   the central directory is; listing reads only that. A member opens from its central
   entry (which has the sizes and CRC-32 even when the local header defers them to a data
   descriptor), then its local header, then its data: stored, or deflated (inflated by
   Mill, the host's GzData, as bare deflate) and checked against the CRC. Not read: zip64,
   split archives (both after or beside 1993's feature set), encryption, and the other
   methods (shrink, implode, reduce): listed, but they don't open. */
#include <string.h>

#include "archive.h"

#define BUF 1536
#define NAME_MAX_ 1024
#define COMMENT_MAX 255

#define SIG_LOCAL 0x04034B50UL
#define SIG_CENTRAL 0x02014B50UL
#define SIG_END 0x06054B50UL
#define SIG_END64_LOC 0x07064B50UL

typedef struct {
    const ArcHost *h;
    const ArcRequest *r;
    long size;
    unsigned long cd, cd_end, count;    /* the central directory: start, end, entries */
    unsigned char buf[BUF];             /* what's been read: buf_len bytes at buf_at */
    unsigned long buf_at;
    long buf_len;
    int err;                            /* ARC_READ or ARC_BAD once a read fails */
    char name[NAME_MAX_ + 1], comment[COMMENT_MAX + 1];
    ArcEntry e;
    unsigned long method, flags, crc, csize, usize, local;
    /* opening */
    unsigned long out_crc, out_count;
} Zip;

/* n bytes (n <= BUF) at offset, from buf or read into it; NULL past the end or on an error */
static const unsigned char *get(Zip *z, unsigned long at, long n)
{
    long got, want;
    if (n > BUF || at > (unsigned long)z->size || (unsigned long)z->size - at < (unsigned long)n) { z->err = ARC_BAD; return NULL; }
    if (at >= z->buf_at && at - z->buf_at + (unsigned long)n <= (unsigned long)z->buf_len) return z->buf + (at - z->buf_at);
    want = (unsigned long)z->size - at < BUF ? (long)((unsigned long)z->size - at) : BUF;
    got = z->h->read_at(z->h->ref, at, z->buf, want);
    z->buf_at = at;
    z->buf_len = got < 0 ? 0 : got;
    if (got < 0) { z->err = ARC_READ; return NULL; }
    if (got < n) { z->err = ARC_BAD; return NULL; }
    return z->buf;
}

/* The end of central directory record: the last one whose comment reaches the end */
static int find_end(Zip *z)
{
    unsigned long lowest = z->size > 22 + 65535L ? (unsigned long)z->size - 22 - 65535 : 0, at;
    const unsigned char *p;
    if (z->size < 22) return ARC_BAD;
    for (at = (unsigned long)z->size - 22;; at--) {
        /* backward: on a miss, read the block that ends with this record */
        if (at < z->buf_at || at + 22 > z->buf_at + (unsigned long)z->buf_len) {
            unsigned long from = at + 22 > BUF ? at + 22 - BUF : 0;
            if (from < lowest) from = lowest;
            z->buf_len = 0;
            if (!get(z, from, (long)(at + 22 - from))) return z->err;
        }
        p = z->buf + (at - z->buf_at);
        if (arc_le32(p) == SIG_END && at + 22 + arc_le16(p + 20) == (unsigned long)z->size) break;
        if (at == lowest) return ARC_BAD;
    }
    if (arc_le16(p + 4) || arc_le16(p + 6) || arc_le16(p + 8) != arc_le16(p + 10)) return ARC_UNSUPPORTED;  /* split */
    z->count = arc_le16(p + 10);
    z->cd = arc_le32(p + 16);
    z->cd_end = z->cd + arc_le32(p + 12);
    if (z->count == 0xFFFF || arc_le32(p + 12) == 0xFFFFFFFFUL || z->cd == 0xFFFFFFFFUL) return ARC_UNSUPPORTED;
    if (at >= 20 && (p = get(z, at - 20, 4)) && arc_le32(p) == SIG_END64_LOC) return ARC_UNSUPPORTED;
    if (z->cd_end < z->cd || z->cd_end > at) return ARC_BAD;
    return 0;
}

/* The central entry at offset into z's fields and e; its length (to the next), or an
   error. e.name is NULL if the entry is skipped (its name refused, or past the limits). */
static long central(Zip *z, unsigned long at)
{
    const unsigned char *p;
    unsigned long ext, nlen, xlen, clen, host;
    int folder;
    long n;
    if (at > z->cd_end || z->cd_end - at < 46) return ARC_BAD;
    if (!(p = get(z, at, 46))) return z->err;
    if (arc_le32(p) != SIG_CENTRAL) return ARC_BAD;
    host = p[5];
    z->flags = arc_le16(p + 8);
    z->method = arc_le16(p + 10);
    z->crc = arc_le32(p + 16);
    z->csize = arc_le32(p + 20);
    z->usize = arc_le32(p + 24);
    nlen = arc_le16(p + 28);
    xlen = arc_le16(p + 30);
    clen = arc_le16(p + 32);
    ext = arc_le32(p + 38);
    z->local = arc_le32(p + 42);
    arc_entry_init(&z->e);
    z->e.name = NULL;
    z->e.modified = arc_dos_date(arc_le16(p + 14), arc_le16(p + 12));
    if (z->cd_end - at - 46 < nlen + xlen + clen) return ARC_BAD;
    n = 46 + (long)(nlen + xlen + clen);
    if (nlen > NAME_MAX_ || z->csize > 0x7FFFFFFFUL || z->usize > 0x7FFFFFFFUL) return n;
    if (!(p = get(z, at + 46, (long)nlen))) return z->err;
    /* MS-DOS zippers may have written '\' between folders */
    if (arc_clean(p, (long)nlen, host == 0, z->name, sizeof z->name, &folder) < 0) return n;
    z->e.name = z->name;
    z->e.charset = ARC_CP437;
    z->e.kind = folder ? ARC_FOLDER : ARC_FILE;
    if (host == 0 && (ext & 0x10)) z->e.kind = ARC_FOLDER;     /* MS-DOS's directory attribute */
    if (host == 3) {                                            /* Unix: the mode in the high word */
        z->e.mode = (long)((ext >> 16) & 07777);
        if (((ext >> 16) & 0170000UL) == 0120000UL) z->e.kind = ARC_LINK;
        if (((ext >> 16) & 0170000UL) == 0040000UL) z->e.kind = ARC_FOLDER;
    }
    if (z->e.kind == ARC_FILE) {
        z->e.size = (long)z->usize;
        z->e.packed = (long)z->csize;
    }
    if (clen) {
        unsigned long i, k = clen > COMMENT_MAX ? COMMENT_MAX : clen;
        if (!(p = get(z, at + 46 + nlen + xlen, (long)k))) return z->err;
        for (i = 0; i < k; i++) z->comment[i] = p[i] < 32 ? ' ' : (char)p[i];
        z->comment[k] = 0;
        z->e.comment = z->comment;
    }
    if (z->flags & 1) z->e.flags |= ARC_ENCRYPTED;
    if (z->method != 0 && z->method != 8) z->e.flags |= ARC_CANT_OPEN;
    z->e.pos = at;
    return n;
}

static int inflated(void *ref, const unsigned char *p, long n)
{
    Zip *z = ref;
    z->out_crc = arc_crc32(z->out_crc, p, n);
    z->out_count += (unsigned long)n;
    return z->h->out(z->h->ref, p, n);
}

/* The member whose central entry central() just read */
static int open_member(Zip *z)
{
    const unsigned char *p;
    unsigned long data, left;
    void *gz = NULL;
    int r = ARC_MORE;
    if (z->flags & 1) return ARC_UNSUPPORTED;
    if (z->method != 0 && z->method != 8) return ARC_UNSUPPORTED;
    if (!(p = get(z, z->local, 30))) return z->err;
    if (arc_le32(p) != SIG_LOCAL) return ARC_BAD;
    data = z->local + 30 + arc_le16(p + 26) + arc_le16(p + 28);
    if (data > z->cd || z->cd - data < z->csize) return ARC_BAD;
    if (z->method == 0 && z->csize != z->usize) return ARC_BAD;
    if (z->method == 8) {
        GzMem mem;
        mem.ref = z->h->ref;
        mem.alloc = z->h->alloc;
        mem.release = z->h->release;
        if (!(gz = z->h->gz->begin(z->h->gz->ref, &mem, 1, inflated, z))) return ARC_NO_MEMORY;
    }
    for (left = z->csize; left > 0 && r == ARC_MORE; ) {
        long k = left < BUF ? (long)left : BUF, got = z->h->read_at(z->h->ref, data, z->buf, k);
        z->buf_len = 0;     /* buf now holds member data */
        if (got < 0) { r = ARC_READ; break; }
        if (got < k) { r = ARC_BAD; break; }
        if (gz) {
            int g = z->h->gz->data(gz, z->buf, k);
            if (g == 1) r = ARC_DONE;
            else if (g == -2) r = ARC_STOPPED;
            else if (g < 0) r = ARC_BAD;
        } else if (inflated(z, z->buf, k)) r = ARC_STOPPED;
        data += (unsigned long)k;
        left -= (unsigned long)k;
    }
    if (gz) {
        z->h->gz->end(gz);
        if (r == ARC_MORE) r = ARC_BAD;     /* the deflate data didn't end */
    }
    if (r == ARC_MORE || r == ARC_DONE)
        r = z->out_crc == z->crc && z->out_count == z->usize ? ARC_DONE : ARC_BAD;
    return r;
}

int zip_pull(const ArcHost *h, const ArcRequest *r, const char **rest)
{
    Zip *z = arc_new(h, sizeof *z);
    unsigned long at, i;
    long n;
    int res;
    if (!z) return ARC_NO_MEMORY;
    z->h = h;
    z->r = r;
    z->size = h->size(h->ref);
    if (z->size < 0) { res = ARC_UNSUPPORTED; goto done; }
    if ((res = find_end(z)) != 0) goto done;
    /* a position from the listing: the central entry there, if it's the one */
    if (r->open && r->has_pos && r->pos >= z->cd && r->pos < z->cd_end && central(z, r->pos) > 0 && z->e.name &&
        z->e.kind != ARC_FOLDER && (*rest = arc_on_path(z->name, r->path)) != NULL) {
        res = z->e.kind == ARC_LINK ? ARC_UNSUPPORTED : arc_member(h, &z->e, *rest, NULL) != ARC_MORE ? ARC_STOPPED : open_member(z);
        goto done;
    }
    res = r->open ? ARC_NOT_FOUND : ARC_DONE;
    for (at = z->cd, i = 0; i < z->count; i++, at += (unsigned long)n) {
        if ((n = central(z, at)) < 0) { res = (int)n; break; }
        if (!z->e.name) continue;
        if (!r->open) {
            if (h->entry(h->ref, &z->e)) { res = ARC_STOPPED; break; }
            continue;
        }
        if (z->e.kind == ARC_FOLDER || !(*rest = arc_on_path(z->name, r->path))) continue;
        res = z->e.kind == ARC_LINK ? ARC_UNSUPPORTED : arc_member(h, &z->e, *rest, NULL) != ARC_MORE ? ARC_STOPPED : open_member(z);
        break;
    }
done:
    if (res != ARC_DONE || !r->open) *rest = NULL;
    arc_free(h, z);
    return res;
}
