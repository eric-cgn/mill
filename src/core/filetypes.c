#include <stdio.h>
#include <string.h>

#include "filetypes.h"

const char FILETYPES_DEFAULT[] =
    "* *.hqx TEXT/SITx\n"
    "application/mac-binhex40 * TEXT/SITx\n"
    "* *.sit SIT!/SITx\n"
    "* *.cpt PACT/CPCT\n"
    "* *.bin BINA/SITx\n"
    "* *.img dImg/dCpy\n"
    "* *.image dImg/dCpy\n"
    "* *.dsk dImg/dCpy\n"
    "* *.pdf PDF /CARO\n"
    "* *.gif GIFf/JVWR\n"
    "image/gif * GIFf/JVWR\n"
    "* *.jpg JPEG/JVWR\n"
    "image/jpeg * JPEG/JVWR\n"
    "* *.txt TEXT/ttxt\n"
    "text/* * TEXT/ttxt\n"
    "* * BINA/????\n";

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int is_end(char c) { return c == '\n' || c == '\r' || c == ';' || c == 0; }

/* A rule's parts: the two patterns, and where TYPE/CREA starts */
typedef struct { const char *mime; long mime_len; const char *name; long name_len; const char *codes; } Rule;

static int parse(const char *s, long len, Rule *r)
{
    long i = 0, k;
    while (i < len && s[i] == ' ') i++;
    r->mime = s + i;
    while (i < len && s[i] != ' ') i++;
    r->mime_len = s + i - r->mime;
    while (i < len && s[i] == ' ') i++;
    r->name = s + i;
    while (i < len && s[i] != ' ') i++;
    r->name_len = s + i - r->name;
    while (i < len && s[i] == ' ') i++;
    if (!r->mime_len || !r->name_len || len - i < 9 || s[i + 4] != '/') return -1;
    r->codes = s + i;
    for (k = i + 9; k < len; k++) if (s[k] != ' ') return -1;
    return 0;
}

int filetype_rule_ok(const char *line, long len)
{
    Rule r;
    return parse(line, len, &r);
}

static int mime_fits(const Rule *r, const char *mime)
{
    long n = 0, i;
    if (r->mime_len == 1 && r->mime[0] == '*') return 1;
    while (mime[n] && mime[n] != ';' && mime[n] != ' ') n++;
    if (r->mime_len >= 2 && r->mime[r->mime_len - 1] == '*' && r->mime[r->mime_len - 2] == '/') {
        if (n < r->mime_len - 1) return 0;
        for (i = 0; i < r->mime_len - 1; i++) if (lower(mime[i]) != lower(r->mime[i])) return 0;
        return 1;
    }
    if (n != r->mime_len) return 0;
    for (i = 0; i < n; i++) if (lower(mime[i]) != lower(r->mime[i])) return 0;
    return 1;
}

static int name_fits(const Rule *r, const char *name)
{
    long n = strlen(name), i, want = r->name_len;
    const char *pat = r->name;
    if (want == 1 && pat[0] == '*') return 1;
    if (pat[0] == '*') {
        pat++; want--;
        if (n < want) return 0;
        name += n - want;
        n = want;
    }
    if (n != want) return 0;
    for (i = 0; i < n; i++) if (lower(name[i]) != lower(pat[i])) return 0;
    return 1;
}

int filetype_find(const char *rules, const char *mime, const char *name, char type[4], char creator[4])
{
    const char *p = rules;
    while (*p) {
        const char *e = p;
        Rule r;
        while (!is_end(*e)) e++;
        if (e > p && !parse(p, e - p, &r) && mime_fits(&r, mime) && name_fits(&r, name)) {
            memcpy(type, r.codes, 4);
            memcpy(creator, r.codes + 5, 4);
            return 1;
        }
        p = *e ? e + 1 : e;
    }
    memcpy(type, "BINA", 4);
    memcpy(creator, "????", 4);
    return 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

void filetype_name(const char *url, char *out, int size)
{
    const char *start = strstr(url, "://"), *end, *seg;
    int o = 0;
    long n;
    start = start ? start + 3 : url;
    end = start + strcspn(start, "?#");
    for (seg = end; seg > start && seg[-1] != '/'; seg--) {}
    if (seg == start || seg == end) {
        /* no file name in the path: the host's name */
        for (seg = start, end = start; *end && *end != '/' && *end != ':' && *end != '?' && *end != '#'; end++) {}
    }
    while (seg < end && o < size - 1 && o < 31) {
        int c = (unsigned char)*seg++;
        if (c == '%' && seg + 1 < end && hexval(seg[0]) >= 0 && hexval(seg[1]) >= 0) {
            c = hexval(seg[0]) * 16 + hexval(seg[1]);
            seg += 2;
        }
        if (c == ':') c = '-';  /* the Mac's path separator */
        if (c < 32) continue;
        out[o++] = (char)c;
    }
    out[o] = 0;
    n = o;
    if (n > 4 && !strcmp(out + n - 4, ".bin")) out[n - 4] = 0;
    if (!out[0]) snprintf(out, size, "%s", "Untitled");
}

static unsigned long be32(const unsigned char *p) { return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | (p[2] << 8) | p[3]; }

/* CRC-16/XMODEM, as MacBinary II uses */
static unsigned short crc16(const unsigned char *p, int n)
{
    unsigned short crc = 0;
    int i, b;
    for (i = 0; i < n; i++) {
        crc ^= (unsigned short)(p[i] << 8);
        for (b = 0; b < 8; b++) crc = (unsigned short)(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

int macbinary_header(const unsigned char *h, MacBinary *mb)
{
    unsigned long dlen, rlen;
    long second = 0;
    int i;
    if (h[0] != 0 || h[74] != 0 || h[82] != 0 || h[1] < 1 || h[1] > 63) return -1;
    dlen = be32(h + 83);
    rlen = be32(h + 87);
    if (dlen > 0x7FFFFF || rlen > 0x7FFFFF) return -1;
    if (crc16(h, 124) == ((h[124] << 8) | h[125])) {
        second = (h[120] << 8) | h[121];  /* II and III may have a second header */
    } else {
        for (i = 99; i < 126; i++) if (h[i]) return -1;  /* I: the rest is zero */
    }
    memcpy(mb->name, h + 2, h[1]);
    mb->name[h[1]] = 0;
    memcpy(mb->type, h + 65, 4);
    memcpy(mb->creator, h + 69, 4);
    /* not on the desktop, not yet seen by the Finder, not changed or busy */
    mb->flags = (unsigned short)(((h[73] << 8) | h[101]) & ~0x0701);
    mb->data_at = 128 + (second + 127) / 128 * 128;
    mb->data_len = (long)dlen;
    mb->rsrc_at = mb->data_at + ((long)dlen + 127) / 128 * 128;
    mb->rsrc_len = (long)rlen;
    return 0;
}
