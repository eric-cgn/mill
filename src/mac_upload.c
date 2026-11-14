/* Form bodies, made as they're sent (mac_upload.h). In a code segment of its own,
   loaded only while a form is sent. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/charset.h"
#include "core/macenc.h"
#include "mac_local.h"
#include "mac_net.h"
#include "mac_plugin.h"
#include "mac_sys.h"
#include "mac_upload.h"

enum { AS_FIELD, AS_DATA, AS_MACBINARY, AS_BINHEX };
enum { HEAD, CONTENT, CRLF, END, DONE };
#define PIECE 1024              /* read from a file at a time */
#define PENDING (4L * 1024)     /* BinHex's text from a piece: at most about 2.8 times it */

typedef struct {
    const FormPart *fp;
    int as;
    char filename[80], type[48];    /* as sent */
    char macname[64];               /* the Mac's own name, inside MacBinary and BinHex */
    FSSpec spec;
    FInfo fi;
    long dlen, rlen, size;
    unsigned long created, modified;
    char *head;
    long head_len;
} Part;

struct Body {
    const Submission *s;
    Part *parts;
    int n;
    long size, sent;
    char type[80], end[64];
    long end_len;
    /* reading */
    Boolean by_part;
    int part, stage;
    long off;
    /* the file being read */
    short fork;                     /* its open fork, 0 if none */
    int seg;                        /* which piece of its stream (see raw) */
    long seg_left;
    unsigned short crc;
    unsigned char hdr[128];
    int hdr_len;
    unsigned char *pend;            /* made and not yet read (BinHex's text) */
    long plen, ppos;
    Hqx hqx;
    Boolean finished;
};

static int setting_as(void)
{
    const char *v = setting("upload_as", "Automatic");
    return !strcmp(v, "Data fork only") ? AS_DATA : !strcmp(v, "MacBinary") ? AS_MACBINARY :
           !strcmp(v, "BinHex") ? AS_BINHEX : -1;
}

static void close_fork(Body *b)
{
    if (b->fork) { FSClose(b->fork); b->fork = 0; }
}

static OSErr open_fork(Body *b, Part *p, int rsrc)
{
    close_fork(b);
    return rsrc ? file_open_rf(&p->spec, fsRdPerm, &b->fork) : file_open(&p->spec, fsRdPerm, &b->fork);
}

/* Bytes of an open fork; zeros past its end (padding) */
static long read_fork(Body *b, unsigned char *buf, long n)
{
    long got = n;
    if (!b->fork || FSRead(b->fork, &got, buf) != noErr) {}
    if (got < n) memset(buf + got, 0, n - got);
    return n;
}

/* A file's stream before any BinHex text: MacBinary's header, forks, and padding; or
   BinHex's header block, forks, and their CRCs. Up to n bytes; 0 at the end. */
static long raw(Body *b, Part *p, unsigned char *buf, long n)
{
    for (;;) {
        long take;
        if (b->seg_left == 0) {
            int bh = p->as == AS_BINHEX;
            b->seg++;
            switch (b->seg) {
            case 1:     /* the header */
                if (bh) b->hdr_len = binhex_header(b->hdr, p->macname, (char *)&p->fi.fdType, (char *)&p->fi.fdCreator,
                                                   (unsigned short)p->fi.fdFlags, p->dlen, p->rlen);
                else {
                    macbinary_make(b->hdr, p->macname, (char *)&p->fi.fdType, (char *)&p->fi.fdCreator,
                                     (unsigned short)p->fi.fdFlags, p->dlen, p->rlen, p->created, p->modified);
                    b->hdr_len = 128;
                }
                b->seg_left = b->hdr_len;
                break;
            case 2: open_fork(b, p, 0); b->crc = 0; b->seg_left = p->dlen; break;
            case 3: b->seg_left = bh ? 2 : (128 - p->dlen % 128) % 128; break;
            case 4: open_fork(b, p, 1); b->crc = 0; b->seg_left = p->rlen; break;
            case 5: b->seg_left = bh ? 2 : (128 - p->rlen % 128) % 128; break;
            default: close_fork(b); return 0;
            }
            continue;
        }
        take = b->seg_left < n ? b->seg_left : n;
        if (b->seg == 1) memcpy(buf, b->hdr + (b->hdr_len - b->seg_left), take);
        else if (b->seg == 2 || b->seg == 4) { read_fork(b, buf, take); b->crc = crc_ccitt(b->crc, buf, take); }
        else if (p->as == AS_BINHEX) {
            /* a fork's CRC, high byte first */
            unsigned char c[2];
            c[0] = (unsigned char)(b->crc >> 8);
            c[1] = (unsigned char)b->crc;
            memcpy(buf, c + (2 - b->seg_left), take);
        } else memset(buf, 0, take);
        b->seg_left -= take;
        return take;
    }
}

static void start_content(Body *b, Part *p)
{
    b->seg = 0;
    b->seg_left = 0;
    b->plen = b->ppos = 0;
    b->finished = false;
    b->off = 0;
    close_fork(b);
    if (p->as == AS_DATA) open_fork(b, p, 0);
    if (p->as == AS_BINHEX) hqx_begin(&b->hqx, b->pend, PENDING);
    if (p->as == AS_BINHEX) b->plen = b->hqx.len;
}

/* A part's contents, as sent: up to n bytes, 0 at the end */
static long content(Body *b, Part *p, unsigned char *buf, long n)
{
    long got;
    switch (p->as) {
    case AS_FIELD:
        got = p->size - b->off < n ? p->size - b->off : n;
        memcpy(buf, p->fp->value + b->off, got);
        b->off += got;
        return got;
    case AS_DATA:
        got = p->size - b->off < n ? p->size - b->off : n;
        read_fork(b, buf, got);
        b->off += got;
        if (b->off >= p->size) close_fork(b);
        return got;
    case AS_MACBINARY:
        return raw(b, p, buf, n);
    }
    /* BinHex: its text, made a piece at a time */
    while (b->ppos >= b->plen && !b->finished) {
        unsigned char piece[PIECE];
        long r = raw(b, p, piece, PIECE);
        b->hqx.len = 0;
        if (r > 0) hqx_put(&b->hqx, piece, r);
        else { hqx_finish(&b->hqx); b->finished = true; }
        b->plen = b->hqx.len;
        b->ppos = 0;
    }
    got = b->plen - b->ppos < n ? b->plen - b->ppos : n;
    memcpy(buf, b->pend + b->ppos, got);
    b->ppos += got;
    return got;
}

/* What a file is sent as, and its size */
static OSErr look_at(Body *b, Part *p, char *why, int why_size)
{
    Str255 path;
    CInfoPBRec pb;
    const char *base = p->fp->value, *c;
    long n = strlen(p->fp->value);
    short ref;
    int as = setting_as();
    char ext[8] = "";
    if (n > 255) n = 255;
    path[0] = (unsigned char)n;
    memcpy(path + 1, p->fp->value, n);
    for (c = p->fp->value; *c; c++) if (*c == ':') base = c + 1;
    snprintf(p->macname, sizeof p->macname, "%s", base);
    if (!*base) { p->as = AS_DATA; p->size = 0; strcpy(p->type, "application/octet-stream"); return noErr; }
    if (file_spec(0, 0, path, &p->spec) != noErr || file_get_info(&p->spec, &p->fi) != noErr) {
        snprintf(why, why_size, "Mill couldn't find \"%s\" to send.", base);
        return fnfErr;
    }
    memset(&pb, 0, sizeof pb);
    pb.hFileInfo.ioNamePtr = path;
    pb.hFileInfo.ioVRefNum = p->spec.vRefNum;
    pb.hFileInfo.ioDirID = p->spec.parID;
    memcpy(path, p->spec.name, p->spec.name[0] + 1);
    if (PBGetCatInfoSync(&pb) == noErr) {
        p->dlen = pb.hFileInfo.ioFlLgLen;
        p->rlen = pb.hFileInfo.ioFlRLgLen;
        p->created = pb.hFileInfo.ioFlCrDat;
        p->modified = pb.hFileInfo.ioFlMdDat;
    } else if (file_open(&p->spec, fsRdPerm, &ref) == noErr) { GetEOF(ref, &p->dlen); FSClose(ref); }
    /* Automatic: MacBinary for a file with a resource fork, else its data fork */
    if (as < 0) as = p->rlen > 0 ? AS_MACBINARY : AS_DATA;
    p->as = as;
    if (as == AS_MACBINARY) {
        p->size = 128 + (p->dlen + 127) / 128 * 128 + (p->rlen + 127) / 128 * 128;
        strcpy(p->type, "application/macbinary");
        strcpy(ext, ".bin");
    } else if (as == AS_BINHEX) {
        /* counted by making it once without keeping it */
        unsigned char piece[PIECE];
        long r;
        Hqx count;
        b->seg = 0;
        b->seg_left = 0;
        hqx_begin(&count, NULL, 0);
        while ((r = raw(b, p, piece, PIECE)) > 0) hqx_put(&count, piece, r);
        hqx_finish(&count);
        close_fork(b);
        p->size = count.count;
        strcpy(p->type, "application/mac-binhex40");
        strcpy(ext, ".hqx");
    } else {
        p->size = p->dlen;
        if (p->fi.fdType == 'TEXT') strcpy(p->type, "text/plain");
        else type_for_name(base, p->type, sizeof p->type);
    }
    /* the name as sent: Latin-1, and .bin or .hqx added if the settings say */
    for (n = 0; base[n] && n < (long)sizeof p->filename - 6; n++) p->filename[n] = (char)mac_to_latin1((unsigned char)base[n]);
    p->filename[n] = 0;
    if (ext[0] && strcmp(setting("upload_ext", "yes"), "no")) strcat(p->filename, ext);
    return noErr;
}

Body *body_new(const Submission *s, char *why, int why_size)
{
    Body *b = calloc(1, sizeof *b);
    int i;
    snprintf(why, why_size, "Not enough memory.");
    if (!b) return NULL;
    b->s = s;
    if (!s->multipart) {
        b->size = s->len;
        strcpy(b->type, "application/x-www-form-urlencoded");
        return b;
    }
    sprintf(b->type, "multipart/form-data; boundary=%.40s", s->boundary);
    if (!(b->pend = malloc(PENDING)) || (s->nparts && !(b->parts = calloc(s->nparts, sizeof(Part))))) { body_free(b); return NULL; }
    b->n = s->nparts;
    for (i = 0; i < b->n; i++) {
        Part *p = &b->parts[i];
        char head[600];
        p->fp = &s->parts[i];
        if (p->fp->file) { if (look_at(b, p, why, why_size) != noErr) { body_free(b); return NULL; } }
        else { p->as = AS_FIELD; p->size = strlen(p->fp->value); }
        if ((p->head_len = form_part_head(s, p->fp, p->fp->file ? p->filename : NULL, p->type, head, sizeof head)) < 0 ||
            !(p->head = malloc(p->head_len + 1))) { body_free(b); return NULL; }
        memcpy(p->head, head, p->head_len + 1);
        b->size += p->head_len + p->size + 2;
    }
    b->end_len = form_end(s, b->end, sizeof b->end);
    b->size += b->end_len;
    body_rewind(b);
    return b;
}

void body_free(Body *b)
{
    int i;
    if (!b) return;
    close_fork(b);
    for (i = 0; i < b->n; i++) free(b->parts[i].head);
    free(b->parts);
    free(b->pend);
    free(b);
}

long body_size(const Body *b) { return b->size; }
const char *body_type(const Body *b) { return b->type; }

void body_rewind(Body *b)
{
    close_fork(b);
    b->by_part = false;
    b->part = 0;
    b->stage = HEAD;
    b->off = 0;
    b->sent = 0;
    if (b->n && b->part < b->n) b->stage = HEAD;
    else if (b->s->multipart) b->stage = END;
}

static void say(Body *b)
{
    char msg[48];
    sprintf(msg, "Sending... %ldK of %ldK", (b->sent + 1023) / 1024, (b->size + 1023) / 1024);
    if (net_status) net_status(msg);
}

OSErr body_read(Body *b, char *buf, long *len)
{
    long room = *len, got = 0, n;
    unsigned char *out = (unsigned char *)buf;
    *len = 0;
    if (net_idle && net_idle()) return userCanceledErr;
    if (!b->s->multipart) {
        n = b->s->len - b->off < room ? b->s->len - b->off : room;
        memcpy(buf, b->s->data + b->off, n);
        b->off += n;
        *len = n;
        b->sent += n;
        say(b);
        return noErr;
    }
    while (got < room && b->stage != DONE) {
        Part *p = b->part < b->n ? &b->parts[b->part] : NULL;
        switch (b->stage) {
        case HEAD:
            if (b->by_part) { b->stage = CONTENT; start_content(b, p); break; }
            n = p->head_len - b->off < room - got ? p->head_len - b->off : room - got;
            memcpy(out + got, p->head + b->off, n);
            got += n;
            if ((b->off += n) == p->head_len) { b->stage = CONTENT; start_content(b, p); }
            break;
        case CONTENT:
            n = content(b, p, out + got, room - got);
            got += n;
            if (n == 0) {
                if (b->by_part) { b->stage = DONE; break; }   /* this part's end */
                b->stage = CRLF;
                b->off = 0;
            }
            break;
        case CRLF:
            n = 2 - b->off < room - got ? 2 - b->off : room - got;
            memcpy(out + got, "\r\n" + b->off, n);
            got += n;
            if ((b->off += n) == 2) {
                b->off = 0;
                b->stage = ++b->part < b->n ? HEAD : END;
            }
            break;
        case END:
            n = b->end_len - b->off < room - got ? b->end_len - b->off : room - got;
            memcpy(out + got, b->end + b->off, n);
            got += n;
            if ((b->off += n) == b->end_len) b->stage = DONE;
            break;
        }
    }
    *len = got;
    b->sent += got;
    say(b);
    return noErr;
}

OSErr body_next_part(Body *b, const char **name, const char **filename, const char **type)
{
    Part *p;
    if (!b->s->multipart) return paramErr;
    if (!b->by_part) { body_rewind(b); b->by_part = true; b->part = -1; }
    close_fork(b);
    if (++b->part >= b->n) { b->stage = DONE; return eofErr; }
    p = &b->parts[b->part];
    *name = p->fp->name;
    *filename = p->fp->file ? p->filename : NULL;
    *type = p->fp->file ? p->type : NULL;
    b->stage = CONTENT;
    start_content(b, p);
    return noErr;
}

char *body_all(Body *b, long *len)
{
    char *all = malloc(b->size + 1);
    long got = 0, n;
    if (!all) return NULL;
    body_rewind(b);
    while (got < b->size) {
        n = b->size - got;
        if (body_read(b, all + got, &n) != noErr || n == 0) break;
        got += n;
    }
    all[got] = 0;
    *len = got;
    return all;
}
