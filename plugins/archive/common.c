/* What the container cores share: picking one, memory, names and paths, numbers, CRC-32,
   and dates. */
#include <string.h>

#include "archive.h"
#include "../../src/core/charset.h"

void *arc_new(const ArcHost *h, long size)
{
    void *p = h->alloc(h->ref, size);
    if (p) memset(p, 0, (size_t)size);
    return p;
}

void arc_free(const ArcHost *h, void *p)
{
    if (p) h->release(h->ref, p);
}

void *arc_begin(int format, const ArcHost *h, const ArcRequest *r)
{
    ArcBase *b;
    switch (format) {
    case ARC_TAR: b = tar_begin(h, r); break;
    case ARC_GZIP: b = gzip_begin(h, r); break;
    case ARC_MACBINARY: b = macbinary_begin(h, r); break;
    case ARC_BINHEX: b = binhex_begin(h, r); break;
    default: return NULL;   /* zip can only be pulled */
    }
    if (b) b->format = format;
    return b;
}

int arc_data(void *d, const unsigned char *p, long n)
{
    ArcBase *b = d;
    int r = ARC_BAD;
    if (b->result != ARC_MORE) return b->result;
    if (n < 0) return b->result = ARC_BAD;
    switch (b->format) {
    case ARC_TAR: r = tar_data(d, p, n); break;
    case ARC_GZIP: r = gzip_data(d, p, n); break;
    case ARC_MACBINARY: r = macbinary_data(d, p, n); break;
    case ARC_BINHEX: r = binhex_data(d, p, n); break;
    }
    return b->result = r;
}

const char *arc_rest(void *d)
{
    return ((ArcBase *)d)->rest;
}

void arc_end(void *d)
{
    ArcBase *b = d;
    if (!b) return;
    if (b->format == ARC_GZIP) gzip_end(d);
    arc_free(b->h, b);
}

int arc_pull(int format, const ArcHost *h, const ArcRequest *r, const char **rest)
{
    *rest = NULL;
    if (!h->read_at || !h->size) return ARC_UNSUPPORTED;
    if (format == ARC_ZIP) return zip_pull(h, r, rest);
    if (format == ARC_TAR) return tar_pull(h, r, rest);
    return ARC_UNSUPPORTED;
}

/* ---- names ---- */

long arc_clean(const unsigned char *in, long n, int backslash, char *out, long size, int *folder)
{
    long i = 0, o = 0;
    *folder = 0;
    if (size < 1) return -1;
    while (i < n) {
        long start, len;
        int c;
        /* one part, up to a separator */
        for (start = i; i < n; i++) {
            c = in[i];
            if (c == '/' || (backslash && c == '\\')) break;
            if (c < 32 || c == 127) return -1;
        }
        len = i - start;
        *folder = i < n;    /* a separator after it: so far, it ends in one */
        if (i < n) i++;
        if (len == 0 || (len == 1 && in[start] == '.')) continue;
        if (len == 2 && in[start] == '.' && in[start + 1] == '.') return -1;
        if (o + (o > 0) + len >= size) return -1;
        if (o > 0) out[o++] = '/';
        memcpy(out + o, in + start, (size_t)len);
        o += len;
    }
    out[o] = 0;
    return o > 0 ? o : -1;
}

static unsigned char to_mac(unsigned char c, int charset)
{
    return charset_byte_to_mac(c, charset == ARC_MACROMAN ? CHARSET_MAC : charset == ARC_LATIN1 ? CHARSET_LATIN1 : CHARSET_CP437);
}

void arc_hfs_name(const char *name, int charset, char out[32])
{
    const char *s = strrchr(name, '/'), *dot;
    long n, keep, ext = 0, i, o = 0;
    s = s ? s + 1 : name;
    n = (long)strlen(s);
    /* too long: cut the stem, keep a short extension (".txt", ".html") */
    keep = n;
    if (n > 31) {
        dot = strrchr(s, '.');
        if (dot && dot > s && s + n - dot <= 6) ext = s + n - dot;
        keep = 31 - ext;
    }
    for (i = 0; i < n && o < 31; i++) {
        unsigned char c;
        if (i == keep) i = n - ext;
        if (i >= n) break;
        c = to_mac((unsigned char)s[i], charset);
        if (c == ':') c = '-';
        if (c < 32 || c == 127 || c == 0) c = '-';
        if (o == 0 && c == '.') c = '_';
        out[o++] = (char)c;
    }
    out[o] = 0;
    if (!o) strcpy(out, "Untitled");
}

/* ---- numbers, CRC-32, dates ---- */

unsigned short arc_le16(const unsigned char *p) { return (unsigned short)(p[0] | (p[1] << 8)); }
unsigned long arc_le32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}
unsigned long arc_be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

/* A nibble at a time: a 64-byte table instead of 1K */
static const unsigned long CRC_NIBBLE[16] = {
    0x00000000UL, 0x1DB71064UL, 0x3B6E20C8UL, 0x26D930ACUL, 0x76DC4190UL, 0x6B6B51F4UL, 0x4DB26158UL, 0x5005713CUL,
    0xEDB88320UL, 0xF00F9344UL, 0xD6D6A3E8UL, 0xCB61B38CUL, 0x9B64C2B0UL, 0x86D3D2D4UL, 0xA00AE278UL, 0xBDBDF21CUL };

unsigned long arc_crc32(unsigned long crc, const unsigned char *p, long n)
{
    crc = ~crc & 0xFFFFFFFFUL;
    while (n-- > 0) {
        crc ^= *p++;
        crc = (crc >> 4) ^ CRC_NIBBLE[crc & 15];
        crc = (crc >> 4) ^ CRC_NIBBLE[crc & 15];
    }
    return ~crc & 0xFFFFFFFFUL;
}

#define MAC_TO_UNIX 2082844800UL    /* 1904 to 1970: 24,107 days */

unsigned long arc_unix_date(unsigned long t)
{
    if (t > 0xFFFFFFFFUL - MAC_TO_UNIX) return 0xFFFFFFFFUL;
    return t + MAC_TO_UNIX;
}

unsigned long arc_dos_date(unsigned short date, unsigned short time)
{
    static const short before[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    long y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31, days;
    if (!date || m < 1 || m > 12 || d < 1) return 0;
    /* days from 1904-01-01; every fourth year from 1904 to 2099 is a leap year */
    days = (y - 1904) * 365 + (y - 1904 + 3) / 4 + before[m - 1] + d - 1;
    if (m > 2 && y % 4 == 0) days++;
    return (unsigned long)days * 86400UL + (unsigned long)(time >> 11) * 3600UL + ((time >> 5) & 63) * 60UL + (time & 31) * 2UL;
}

/* ---- paths ---- */

const char *arc_on_path(const char *name, const char *path)
{
    size_t n = strlen(name);
    while (*path == '/') path++;
    if (strncmp(name, path, n)) return NULL;
    if (path[n] == 0) return path + n;
    if (path[n] == '/') return path + n + 1;
    return NULL;
}

const char *arc_first(const char *path)
{
    while (*path == '/') path++;
    while (*path && *path != '/') path++;
    return *path ? path + 1 : path;
}

void arc_mac_name(const unsigned char *in, int n, char out[64])
{
    unsigned char t[63];
    int i, folder;
    if (n > 63) n = 63;
    for (i = 0; i < n; i++) t[i] = in[i] == '/' || in[i] < 32 || in[i] == 127 ? '-' : in[i];
    if (arc_clean(t, n, 0, out, 64, &folder) < 0) strcpy(out, "Untitled");
}

int arc_member(const ArcHost *h, const ArcEntry *e, const char *rest, int *fork)
{
    int none = ARC_DATA_FORK;
    return h->member && h->member(h->ref, e, rest ? rest : "", fork ? fork : &none) ? ARC_STOPPED : ARC_MORE;
}

void arc_entry_init(ArcEntry *e)
{
    memset(e, 0, sizeof *e);
    e->size = e->packed = e->mode = e->data_len = e->rsrc_len = -1;
}
