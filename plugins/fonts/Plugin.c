/* Web Fonts: the Plug-In inside the Web Fonts file that draws Mill Serif and Mill Mono from
   their outlines, from 18 point up (MillPlugIn.h, Fonts). The outlines are the file's 'Msfn'
   resources, TrueType data the Font Manager never sees, listed in 'Mfnt' 128; they're read
   straight from the file's resource fork, a letter at a time. Each letter is drawn the first
   time it's needed at a size and kept in a cache, as Apple's TrueType does; the cache is
   purgeable, and it, the letters' work area, and the open fonts are let go on millPurge.
   millFontOpen gives the lines' ascent, descent, and leading, from each outline's hhea.
   The engine (tt.h, parse.c, raster.c, glyph.c, strike.c) is written from Apple's TrueType
   Reference Manual: no hinting, with dropout control. */
#include <string.h>
#include <Memory.h>
#include <Files.h>
#include <Resources.h>
#include <Quickdraw.h>

#include "MillPlugIn.h"
#include "tt.h"

#define FAMS 4          /* families in the index */
#define FACES 8         /* outlines open at once (two families of three) */
#define STRIKES 6       /* sizes in the cache */
#define CACHE_MAX 24576L    /* the cache's bytes, all sizes */
#define RASTER_KEEP 8192    /* a larger work area is let go after each run */
#define BAD_OFF 0xFFFFFFFFUL    /* TTChar.off of a character the font lacks (parse.c) */

/* A letter in the cache: its ink, rows of rowBytes (even, for CopyBits), top row first */
typedef struct {
    signed char left, top;      /* the ink's left edge from the pen, its top above the baseline */
    unsigned char w, h;         /* 0 x 0: no ink (a space) */
    unsigned short rowBytes;
} Letter;
#define NOT_YET 0               /* Strike.at: not drawn yet */
#define NONE 1                  /* letter(): none (no memory) */

/* One outline: where its data is in the resource fork, and the font once opened */
typedef struct {
    short id, ref;              /* its 'Msfn' ID; the file's resource fork */
    long off, len;
    TTFont *f;
    short scaled;               /* the size tt_scale was last given */
} Face;

/* The letters of one face at one size: a purgeable handle, at[] (where each letter is in it)
   and then the letters */
typedef struct {
    Handle h;
    short face, size;
    long used;
} Strike;
#define STRIKE_HEAD ((TT_NCH + 1) * 2L)
#define AT(h) ((unsigned short *)*(h))

typedef struct {
    MillPlugIn base;
    MillMethods methods;
    short nfonts;
    MillFont fonts[FAMS];
    char names[FAMS][32];
    short faceOf[FAMS][4];      /* each family's face for plain, bold, italic, bold italic; -1 none */
    short nfaces;
    Face faces[FACES];
    TTRaster ras;
    Strike strikes[STRIKES];
    long clock;
} Fonts;

uint32_t tt_peak, tt_cur;
void *tt_alloc(uint32_t n) { return NewPtr((long)n); }
void tt_free(void *p) { if (p) DisposePtr((Ptr)p); }

static unsigned short be16(const unsigned char *p) { return (unsigned short)((p[0] << 8) | p[1]); }
static long be32(const unsigned char *p) { return ((long)be16(p) << 16) | be16(p + 2); }

/* tt_open's reads: the face's bytes, from the resource fork */
static int face_read(void *ctx, uint32_t off, uint16_t len, void *buf)
{
    Face *fc = ctx;
    long n = len;
    if ((long)off + len > fc->len) return -1;
    if (SetFPos(fc->ref, fsFromStart, fc->off + (long)off) != noErr) return -1;
    return FSRead(fc->ref, &n, buf) != noErr || n != len;
}

/* Where each 'Msfn' resource's data is in the resource fork (Inside Macintosh: More Macintosh
   Toolbox, "Resource Manager", the resource fork's header and map): System 6 has no
   ReadPartialResource, so the map is read the same way */
static void locate(Fonts *me, short ref)
{
    unsigned char head[16], *map;
    long n = 16, data, at, len, i, k;
    short ntypes;
    if (SetFPos(ref, fsFromStart, 0) || FSRead(ref, &n, head) || n != 16) return;
    data = be32(head);
    at = be32(head + 4);
    len = be32(head + 12);
    if (len < 30 || len > 32767 || !(map = (unsigned char *)NewPtr(len))) return;
    n = len;
    if (SetFPos(ref, fsFromStart, at) || FSRead(ref, &n, map) || n != len) { DisposePtr((Ptr)map); return; }
    at = be16(map + 24);                                /* the type list */
    ntypes = (short)(be16(map + at) + 1);
    for (i = 0; i < ntypes && at + 2 + i * 8 + 8 <= len; i++) {
        const unsigned char *t = map + at + 2 + i * 8;
        long refs = at + be16(t + 6);
        short count = (short)(be16(t + 4) + 1);
        if (be32(t) != 'Msfn') continue;
        for (k = 0; k < count && refs + k * 12 + 12 <= len; k++) {
            const unsigned char *r = map + refs + k * 12;
            short id = (short)be16(r), f;
            long d = data + (((long)r[5] << 16) | ((long)r[6] << 8) | r[7]);
            unsigned char size[4];
            long four = 4;
            for (f = 0; f < me->nfaces; f++) {
                Face *fc = &me->faces[f];
                if (fc->off || fc->id != id) continue;
                if (SetFPos(ref, fsFromStart, d) || FSRead(ref, &four, size) || four != 4) continue;
                fc->off = d + 4;
                fc->len = be32(size);
                fc->ref = ref;
            }
        }
    }
    DisposePtr((Ptr)map);
}

/* ------------------------------------------------------------ the cache */

static void strike_drop(Strike *s)
{
    if (s->h) DisposeHandle(s->h);
    s->h = NULL;
    s->size = 0;
}

static long cache_bytes(Fonts *me)
{
    long n = 0;
    short i;
    for (i = 0; i < STRIKES; i++) if (me->strikes[i].h && *me->strikes[i].h) n += GetHandleSize(me->strikes[i].h);
    return n;
}

/* The face's letters at the size, made empty if there are none yet (NULL: no memory) */
static Strike *strike_of(Fonts *me, short face, short size)
{
    Strike *s, *old = NULL;
    short i;
    for (i = 0; i < STRIKES; i++) {
        s = &me->strikes[i];
        if (s->h && !*s->h) strike_drop(s);             /* purged */
        if (s->h && s->face == face && s->size == size) { s->used = ++me->clock; return s; }
        if (!old || !s->h || (old->h && s->used < old->used)) old = s;
    }
    strike_drop(old);
    if (!(old->h = NewHandleClear(STRIKE_HEAD))) return NULL;
    HPurge(old->h);
    old->face = face;
    old->size = size;
    old->used = ++me->clock;
    return old;
}

/* Room for n more bytes of letters within CACHE_MAX: other sizes let go, least recently used
   first, then this one's letters */
static void cache_room(Fonts *me, Strike *keep, long n)
{
    while (cache_bytes(me) + n > CACHE_MAX) {
        Strike *old = NULL;
        short i;
        for (i = 0; i < STRIKES; i++) {
            Strike *s = &me->strikes[i];
            if (s != keep && s->h && (!old || s->used < old->used)) old = s;
        }
        if (!old) break;
        strike_drop(old);
    }
    if (cache_bytes(me) + n > CACHE_MAX && keep->h && *keep->h) {
        SetHandleSize(keep->h, STRIKE_HEAD);
        memset(*keep->h, 0, STRIKE_HEAD);
    }
}

/* The character's index in the font: its own, or the missing glyph's if the font lacks it */
static unsigned short char_index(TTFont *f, unsigned char c)
{
    unsigned short ci = c < TT_FIRST ? TT_NOTDEF : (unsigned short)(c - TT_FIRST);
    if (f->ch[ci].off == BAD_OFF) ci = TT_NOTDEF;
    return ci;
}

static unsigned short store(Fonts *me, Strike *s, const TTGlyph *g);

/* The letter for ci in the strike, drawn now if it isn't yet: its offset in the handle, or
   NONE. The strike's handle may have moved or gone (purged) after. */
static unsigned short letter(Fonts *me, Strike *s, TTFont *f, unsigned short ci)
{
    TTGlyph *g;
    unsigned short got;
    if (!s->h || !*s->h) return NONE;
    if ((got = AT(s->h)[ci]) != NOT_YET) return got;
    HNoPurge(s->h);                                     /* (drawing it may need memory) */
    got = NONE;
    if ((g = tt_glyph(f, &me->ras, ci)) != NULL) {
        got = store(me, s, g);
        tt_free(g);
    }
    if (got != NONE) AT(s->h)[ci] = got;                /* (else tried again next time) */
    HPurge(s->h);
    return got;
}

/* A drawn letter put in the strike: its offset, or NONE */
static unsigned short store(Fonts *me, Strike *s, const TTGlyph *g)
{
    long at, rb, n;
    short r;
    Letter *l;
    rb = ((g->w + 15) >> 4) << 1;
    n = (long)sizeof(Letter) + rb * g->h;
    cache_room(me, s, n);
    at = GetHandleSize(s->h);
    if (at + n > 65535L) return NONE;
    SetHandleSize(s->h, at + n);
    if (GetHandleSize(s->h) != at + n) return NONE;
    l = (Letter *)(*s->h + at);
    l->left = g->left;
    l->top = g->top;
    l->w = g->w;
    l->h = g->h;
    l->rowBytes = (unsigned short)rb;
    for (r = 0; r < g->h; r++) {
        unsigned char *row = (unsigned char *)(l + 1) + r * rb;
        memcpy(row, g->bits + r * g->rowBytes, g->rowBytes);
        if (rb > g->rowBytes) memset(row + g->rowBytes, 0, rb - g->rowBytes);
    }
    return (unsigned short)at;
}

/* ------------------------------------------------------------ the faces */

/* The face that draws font in face (QuickDraw's bits), and whether it's made bold the way
   QuickDraw makes bold, by drawing it again a pixel to the right: bold italic is the italic
   made bold; any style the family lacks, the plain */
static short face_for(Fonts *me, short font, short face, Boolean *smear)
{
    short st = face & 3;
    const short *of;
    *smear = false;
    if (font < 0 || font >= me->nfonts) return -1;
    of = me->faceOf[font];
    if (of[st] >= 0) return of[st];
    if (st == 3 && of[2] >= 0) { *smear = true; return of[2]; }
    if (st == 3 && of[1] >= 0) return of[1];
    *smear = (st & 1) != 0;
    return of[0];
}

/* The face open and scaled to the size; NULL if it can't be */
static TTFont *face_open(Fonts *me, short k, short size)
{
    Face *fc = &me->faces[k];
    if (!fc->off) return NULL;
    if (!fc->f) {
        if (!(fc->f = (TTFont *)NewPtr(sizeof(TTFont)))) return NULL;
        if (tt_open(fc->f, face_read, fc)) {
            tt_close(fc->f);
            DisposePtr((Ptr)fc->f);
            fc->f = NULL;
            return NULL;
        }
        fc->scaled = 0;
    }
    if (fc->scaled != size) { tt_scale(fc->f, size); fc->scaled = size; }
    return fc->f;
}

static void faces_close(Fonts *me)
{
    short k;
    for (k = 0; k < me->nfaces; k++)
        if (me->faces[k].f) { tt_close(me->faces[k].f); DisposePtr((Ptr)me->faces[k].f); me->faces[k].f = NULL; }
}

/* ------------------------------------------------------------ millFont */

static OSErr fonts_font(MillPlugIn *self, MillFontParams *p)
{
    Fonts *me = (Fonts *)self;
    Boolean smear;
    short k = face_for(me, p->font, p->face, &smear);
    TTFont *f;
    Strike *s = NULL;
    long i, w = 0;
    GrafPtr port;
    if (k < 0) return fnfErr;
    if (p->size < me->fonts[p->font].minSize || p->size > me->fonts[p->font].maxSize) return paramErr;
    if (!(f = face_open(me, k, p->size))) return memFullErr;
    if (p->what == millFontOpen) {
        /* the lines: hhea's ascender, descender, and line gap at the size, scaled as the
           letters are; up to whole pixels, so the letters within them fit (the gap rounded) */
        p->ascent = (short)((tt_sc(f, f->hAsc) + 63) >> 6);
        p->descent = (short)((tt_sc(f, (int16_t)-f->hDesc) + 63) >> 6);
        p->leading = (short)(f->hGap > 0 ? (tt_sc(f, f->hGap) + 32) >> 6 : 0);
        return noErr;
    }
    if (p->what == millFontWidth) {
        for (i = 0; i < p->len; i++) w += tt_advance(f, char_index(f, p->text[i])) + smear;
        p->width = w;
        return noErr;
    }
    if (p->what != millFontDraw) return millUnknown;
    GetPort(&port);
    for (i = 0; i < p->len; i++) {
        unsigned short ci = char_index(f, p->text[i]), at;
        short adv = tt_advance(f, ci);
        if (!s || !s->h || !*s->h) s = strike_of(me, k, p->size);
        at = s ? letter(me, s, f, ci) : NONE;
        if (s && s->h && *s->h && at != NONE) {
            Letter *l;
            BitMap bm;
            Rect from, to;
            char state = HGetState(s->h);
            HLock(s->h);
            l = (Letter *)(*s->h + at);
            if (l->w && l->h) {
                bm.baseAddr = (Ptr)(l + 1);
                bm.rowBytes = (short)l->rowBytes;
                SetRect(&bm.bounds, 0, 0, l->w, l->h);
                to.left = (short)(p->pen.h + w + l->left);
                to.top = (short)(p->pen.v - l->top);
                to.right = to.left + l->w;
                to.bottom = to.top + l->h;
                from = bm.bounds;
                CopyBits(&bm, &port->portBits, &from, &to, p->mode, NULL);
                if (smear) { OffsetRect(&to, 1, 0); CopyBits(&bm, &port->portBits, &from, &to, p->mode, NULL); }
            }
            HSetState(s->h, state);
        }
        w += adv + smear;
    }
    if (me->ras.size > RASTER_KEEP) tt_rfree(&me->ras);   /* a zoomed size's: not kept */
    p->width = w;
    return noErr;
}

static long fonts_purge(MillPlugIn *self, long wanted)
{
    Fonts *me = (Fonts *)self;
    long freed = cache_bytes(me) + me->ras.size;
    short i;
    (void)wanted;
    for (i = 0; i < STRIKES; i++) strike_drop(&me->strikes[i]);
    tt_rfree(&me->ras);
    for (i = 0; i < me->nfaces; i++) if (me->faces[i].f) freed += sizeof(TTFont) + 4L * (me->faces[i].f->maxPts + 1) * 2;
    faces_close(me);
    return freed;
}

static void fonts_close(MillPlugIn *self)
{
    Fonts *me = (Fonts *)self;
    fonts_purge(self, 0);
    self->host->FreeMem(me);
}

/* ------------------------------------------------------------ opening */

/* The families from 'Mfnt' 128 (tools/webfonts/build.py writes it): a count, then for each a
   name (a Pascal string in 32 bytes), its smallest and largest size, whether it's fixed, and
   the 'Msfn' IDs of its plain, bold, italic, and bold italic (0: none) */
static void read_index(Fonts *me, short resFile)
{
    Handle h = Get1Resource('Mfnt', 128);
    long size, n, i;
    short st;
    if (!h) return;
    size = GetHandleSize(h);
    HLock(h);
    n = size >= 2 ? (short)be16((unsigned char *)*h) : 0;
    for (i = 0; i < n && me->nfonts < FAMS && 2 + (i + 1) * 46 <= size; i++) {
        const unsigned char *e = (unsigned char *)*h + 2 + i * 46;
        MillFont *mf = &me->fonts[me->nfonts];
        short len = e[0] > 31 ? 31 : e[0];
        memcpy(me->names[me->nfonts], e + 1, len);
        me->names[me->nfonts][len] = 0;
        mf->name = me->names[me->nfonts];
        mf->minSize = (short)be16(e + 32);
        mf->maxSize = (short)be16(e + 34);
        mf->fixed = (short)be16(e + 36);
        for (st = 0; st < 4; st++) {
            short id = (short)be16(e + 38 + st * 2);
            me->faceOf[me->nfonts][st] = -1;
            if (id && me->nfaces < FACES) {
                me->faces[me->nfaces].id = id;
                me->faceOf[me->nfonts][st] = me->nfaces++;
            }
        }
        me->nfonts++;
    }
    HUnlock(h);
    ReleaseResource(h);
    locate(me, resFile);
    for (i = 0; i < me->nfaces; i++)                   /* any not found are dropped */
        if (!me->faces[i].off) me->faces[i].len = 0;
}

MillPlugIn *PlugInNew(const MillHost *host, short resFile, MillInfo *info)
{
    Fonts *me = host->NewMem(sizeof(Fonts));
    if (!me) return NULL;
    memset(me, 0, sizeof *me);
    me->methods.close = fonts_close;
    me->methods.request = 0;
    me->methods.purge = fonts_purge;
    me->methods.format = 0;
    me->methods.font = fonts_font;
    me->methods.gzOut = 0;
    me->base.methods = &me->methods;
    read_index(me, resFile);
    info->name = "web-fonts";
    info->version = "2.0";
    info->copyright = "Copyright 2026 OneInAMillion Software";
    info->by = "OneInAMillion Software";
    info->description = "Mill Serif and Mill Mono outlines.";
    info->fontCount = me->nfonts;
    info->fonts = me->fonts;
    return &me->base;
}
