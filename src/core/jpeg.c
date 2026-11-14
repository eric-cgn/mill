/* Baseline JPEG, read as it arrives, through ChaN's TJpgDec one MCU at a time. Before
   each MCU the decoder's state is kept; if the data runs out partway, it goes back to
   that and waits for more. Of the header only what decoding needs is kept: markers
   such as APPn (EXIF can be 64K) and COM are passed over as they come. */
#include <stdlib.h>
#include <string.h>

#include "picture.h"
#include "../third_party/tjpgd/tjpgd.h"

#define POOL 4096
#define MAX_HEADER (16L * 1024)
#define MAX_QUEUE (16L * 1024)

enum { MARK, MARKER, LEN1, LEN2, SEGMENT, SCAN, DECODING, FINISHED };

typedef struct {
    PicSink s;
    JDEC jd;
    unsigned char pool[POOL];
    int phase, marker, keep;
    long seg_left;
    unsigned char *hdr;         /* the header markers decoding needs */
    long hlen, hpos;
    unsigned char *q;           /* data not yet decoded */
    long qlen, qpos;
    unsigned int mx, my, x, y;
    uint16_t rst, rsc;
    /* the state before the MCU being decoded */
    uint8_t ck_inbuf[JD_SZBUF];
    long ck_dptr, ck_qpos;
    size_t ck_dctr;
    uint8_t ck_dbit;
    int16_t ck_dcv[3];
    uint16_t ck_rst, ck_rsc;
} Jpeg;

static size_t jpeg_in(JDEC *jd, uint8_t *buf, size_t n)
{
    Jpeg *j = jd->device;
    size_t got = 0;
    if (j->hpos < j->hlen) {
        got = (size_t)(j->hlen - j->hpos) < n ? (size_t)(j->hlen - j->hpos) : n;
        if (buf) memcpy(buf, j->hdr + j->hpos, got);
        j->hpos += got;
    }
    if (got < n && j->qpos < j->qlen) {
        size_t more = (size_t)(j->qlen - j->qpos) < n - got ? (size_t)(j->qlen - j->qpos) : n - got;
        if (buf) memcpy(buf + got, j->q + j->qpos, more);
        j->qpos += more;
        got += more;
    }
    return got;
}

static int jpeg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    Jpeg *j = jd->device;
    j->s.rect(j->s.ref, (short)rect->left, (short)rect->top, (short)(rect->right - rect->left + 1),
              (short)(rect->bottom - rect->top + 1), bitmap, NULL);
    return 1;
}

void *jpeg_begin(const PicSink *s)
{
    Jpeg *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    j->s = *s;
    j->phase = MARK;
    return j;
}

void jpeg_end(void *d)
{
    Jpeg *j = d;
    if (!j) return;
    free(j->hdr);
    free(j->q);
    free(j);
}

static int add(unsigned char **buf, long *len, long max, const unsigned char *p, long n)
{
    unsigned char *more;
    if (*len + n > max || !(more = realloc(*buf, *len + n))) return -1;
    memcpy(more + *len, p, n);
    *buf = more;
    *len += n;
    return 0;
}

static void keep_state(Jpeg *j)
{
    memcpy(j->ck_inbuf, j->jd.inbuf, JD_SZBUF);
    j->ck_dptr = j->jd.dptr - j->jd.inbuf;
    j->ck_dctr = j->jd.dctr;
    j->ck_dbit = j->jd.dbit;
    memcpy(j->ck_dcv, j->jd.dcv, sizeof j->ck_dcv);
    j->ck_rst = j->rst;
    j->ck_rsc = j->rsc;
    /* what's been read is done with */
    if (j->qpos) {
        memmove(j->q, j->q + j->qpos, j->qlen - j->qpos);
        j->qlen -= j->qpos;
        j->qpos = 0;
    }
    j->ck_qpos = 0;
}

static void go_back(Jpeg *j)
{
    memcpy(j->jd.inbuf, j->ck_inbuf, JD_SZBUF);
    j->jd.dptr = j->jd.inbuf + j->ck_dptr;
    j->jd.dctr = j->ck_dctr;
    j->jd.dbit = j->ck_dbit;
    memcpy(j->jd.dcv, j->ck_dcv, sizeof j->ck_dcv);
    j->rst = j->ck_rst;
    j->rsc = j->ck_rsc;
    j->qpos = j->ck_qpos;
}

/* The header's all here: start the decoder, and choose how much it shrinks */
static int start(Jpeg *j)
{
    short dw, dh;
    int scale = 0, r;
    if (jd_prepare(&j->jd, jpeg_in, j->pool, POOL, j) != JDR_OK) return -1;
    if ((r = j->s.size(j->s.ref, (short)j->jd.width, (short)j->jd.height, 1)) != 0) return r;
    j->s.drawn(j->s.ref, &dw, &dh);
    /* shrink by 2, 4, or 8 while the result is still at least the size it's drawn */
    while (scale < 3 && (j->jd.width >> (scale + 1)) >= dw && (j->jd.height >> (scale + 1)) >= dh) scale++;
    if (scale && (r = j->s.size(j->s.ref, (short)((j->jd.width + (1 << scale) - 1) >> scale),
                                (short)((j->jd.height + (1 << scale) - 1) >> scale), 1)) != 0) return r;
    j->jd.scale = (uint8_t)scale;
    j->jd.dcv[0] = j->jd.dcv[1] = j->jd.dcv[2] = 0;
    j->mx = j->jd.msx * 8;
    j->my = j->jd.msy * 8;
    free(j->hdr);
    j->hdr = NULL;
    j->hlen = j->hpos = 0;
    keep_state(j);
    j->phase = DECODING;
    return 0;
}

/* As many MCUs as the data allows */
static int decode(Jpeg *j)
{
    JRESULT rc;
    while (j->y < j->jd.height) {
        if (j->jd.nrst && j->rst++ == j->jd.nrst) {
            rc = jd_restart(&j->jd, j->rsc++);
            if (rc == JDR_INP) { go_back(j); return 0; }
            if (rc != JDR_OK) return 1;     /* what's decoded shows */
            j->rst = 1;
        }
        rc = jd_mcu(&j->jd, jpeg_out, j->x, j->y);
        if (rc == JDR_INP) { go_back(j); return 0; }
        if (rc != JDR_OK) return 1;
        if ((j->x += j->mx) >= j->jd.width) { j->x = 0; j->y += j->my; }
        keep_state(j);
    }
    j->phase = FINISHED;
    return 1;
}

int jpeg_data(void *d, const unsigned char *p, long n)
{
    Jpeg *j = d;
    long i = 0;
    int r;
    while (i < n && j->phase < SCAN) {
        unsigned char b = p[i];
        switch (j->phase) {
        case MARK:
            if (b != 0xFF) return -1;
            i++;
            j->phase = MARKER;
            break;
        case MARKER:
            i++;
            if (b == 0xFF) break;           /* fill */
            j->marker = b;
            if (b == 0xD8) {                /* SOI */
                static const unsigned char soi[2] = { 0xFF, 0xD8 };
                if (add(&j->hdr, &j->hlen, MAX_HEADER, soi, 2)) return -1;
                j->phase = MARK;
            } else j->phase = LEN1;
            break;
        case LEN1:
            j->seg_left = (long)b << 8;
            i++;
            j->phase = LEN2;
            break;
        case LEN2: {
            unsigned char m[4];
            j->seg_left |= b;
            i++;
            if (j->seg_left < 2) return -1;
            /* APPn and COM aren't needed to decode */
            j->keep = !((j->marker >= 0xE0 && j->marker <= 0xEF) || j->marker == 0xFE);
            if (j->keep) {
                m[0] = 0xFF; m[1] = (unsigned char)j->marker; m[2] = (unsigned char)(j->seg_left >> 8); m[3] = (unsigned char)j->seg_left;
                if (add(&j->hdr, &j->hlen, MAX_HEADER, m, 4)) return -1;
            }
            j->seg_left -= 2;
            j->phase = SEGMENT;
            if (j->seg_left == 0) j->phase = j->marker == 0xDA ? SCAN : MARK;
            break;
        }
        case SEGMENT: {
            long take = j->seg_left < n - i ? j->seg_left : n - i;
            if (j->keep && add(&j->hdr, &j->hlen, MAX_HEADER, p + i, take)) return -1;
            i += take;
            if ((j->seg_left -= take) == 0) j->phase = j->marker == 0xDA ? SCAN : MARK;   /* SOS: the data follows */
            break;
        }
        }
    }
    if (j->phase < SCAN) return 0;
    if (i < n && add(&j->q, &j->qlen, MAX_QUEUE + j->qpos, p + i, n - i)) return -1;
    if (j->phase == SCAN) {
        /* the decoder reads ahead a buffer's worth as it starts */
        if (j->qlen < JD_SZBUF && n) return 0;      /* unless that's all there is */
        if ((r = start(j)) != 0) return r;
    }
    if (j->phase == DECODING) return decode(j);
    return 1;
}
