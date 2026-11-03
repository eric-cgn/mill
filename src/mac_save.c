#include <string.h>

#include "mac_save.h"
#include "mac_sys.h"

static OSType os_type(const char *c)
{
    return ((OSType)(unsigned char)c[0] << 24) | ((OSType)(unsigned char)c[1] << 16) |
           ((OSType)(unsigned char)c[2] << 8) | (unsigned char)c[3];
}

void saver_begin(Saver *s, const FSSpec *spec, const char type[4], const char creator[4], Boolean decode)
{
    memset(s, 0, sizeof *s);
    s->spec = *spec;
    memcpy(s->type, type, 4);
    memcpy(s->creator, creator, 4);
    s->decode = decode;
}

static void put(short ref, const char *data, long len, OSErr *err)
{
    if (*err == noErr && len > 0) *err = FSWrite(ref, &len, data);
}

/* The part of data (which starts at s->pos) that falls in [at, at + n) */
static void put_range(Saver *s, short ref, const char *data, long len, long at, long n)
{
    long from = at > s->pos ? at - s->pos : 0, to = at + n - s->pos;
    if (to > len) to = len;
    if (to > from) put(ref, data + from, to - from, &s->err);
}

static void put_data(Saver *s, const char *data, long len)
{
    if (!s->macbinary) {
        put(s->df, data, len, &s->err);
    } else {
        put_range(s, s->df, data, len, s->mb.data_at, s->mb.data_len);
        if (s->rf) put_range(s, s->rf, data, len, s->mb.rsrc_at, s->mb.rsrc_len);
    }
    s->pos += len;
}

/* Now that the first bytes are here: make the file */
static void decide(Saver *s)
{
    OSType type = os_type(s->type), creator = os_type(s->creator);
    s->decided = true;
    s->macbinary = s->decode && s->nhead == 128 && !macbinary_header(s->head, &s->mb);
    if (s->macbinary) { type = os_type(s->mb.type); creator = os_type(s->mb.creator); }
    file_delete(&s->spec);  /* Standard File already asked about replacing it */
    if ((s->err = file_create(&s->spec, creator, type)) != noErr) return;
    if ((s->err = file_open(&s->spec, fsWrPerm, &s->df)) != noErr) { s->df = 0; return; }
    if (s->macbinary && s->mb.rsrc_len > 0) {
        /* the resource fork as the MacBinary file has it, map and all */
        if ((s->err = file_open_rf(&s->spec, fsWrPerm, &s->rf)) != noErr) { s->rf = 0; return; }
    }
    put_data(s, (const char *)s->head, s->nhead);
}

OSErr saver_write(Saver *s, const char *data, long len)
{
    if (s->err) return s->err;
    if (!s->decided) {
        long n = 128 - s->nhead;
        if (n > len) n = len;
        memcpy(s->head + s->nhead, data, n);
        s->nhead += (short)n;
        data += n;
        len -= n;
        if (s->nhead < 128) return noErr;
        decide(s);
    }
    if (s->err == noErr && len > 0) put_data(s, data, len);
    return s->err;
}

static void close_forks(Saver *s)
{
    if (s->df) FSClose(s->df);
    if (s->rf) FSClose(s->rf);
    s->df = s->rf = 0;
}

OSErr saver_end(Saver *s)
{
    if (!s->decided) decide(s);
    if (s->err == noErr && s->macbinary &&
        (s->pos < s->mb.data_at + s->mb.data_len || (s->mb.rsrc_len && s->pos < s->mb.rsrc_at + s->mb.rsrc_len)))
        s->err = eofErr;  /* it ended early */
    close_forks(s);
    if (s->err == noErr && s->macbinary) {
        FInfo fi;
        if (file_get_info(&s->spec, &fi) == noErr) {
            fi.fdFlags = s->mb.flags;
            fi.fdLocation.h = fi.fdLocation.v = 0;
            file_set_info(&s->spec, &fi);
        }
    }
    if (s->err != noErr) file_delete(&s->spec);
    FlushVol(NULL, s->spec.vRefNum);
    return s->err;
}

void saver_abort(Saver *s)
{
    close_forks(s);
    if (s->decided) file_delete(&s->spec);
}
