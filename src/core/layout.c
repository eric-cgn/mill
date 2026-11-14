#include <stdlib.h>
#include <string.h>

#include "layout.h"

#define MARGIN 8
#define INDENT 24
#define BULLET 0xA5  /* Mac Roman bullet */
#define MAX_COLS 64
#define WIDE 30000   /* "no wrapping" when measuring */

/* HTML font sizes 1..7 in points, Netscape's ladder */
static const short SIZES[8] = { 9, 9, 10, 12, 14, 18, 24, 36 };

/* A size or distance at the page's zoom (zn over 9), in whole pixels; one that's there stays at least 1 */
static short zoom_px(short zn, long v)
{
    long r;
    if (zn == 9 || v == 0) return (short)v;
    r = (v * zn + (v < 0 ? -4 : 4)) / 9;
    return (short)(r == 0 ? (v < 0 ? -1 : 1) : r);
}
#define ZS(S, v) zoom_px((S)->zn, (v))

enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };
/* Measuring lays content out and throws it away: at width 0 (every break taken) for
   its narrowest, and unwrapped for its widest */
enum { LAYOUT, MEASURE_MIN, MEASURE_MAX };

typedef struct {
    short font, size_index;
    unsigned char face;
    short link;
    char pre, align, hidden;
    char dark;              /* on a dark background: text goes white */
} Ctx;

typedef struct { long node; short min, max; } Measured;
typedef struct {
    Measured *tables; long n, cap;
    int shown;              /* the progress last reported */
    int stopped;            /* progress said to stop */
} Shared;

/* A floating image (ALIGN=LEFT or RIGHT): text keeps to the side of edge until y reaches bottom */
#define MAX_FLOATS 8
typedef struct { long bottom; short edge; char right; } Float;
typedef struct { long node; Ctx c; } Waiting;   /* a float met mid-line, placed once the line ends */

typedef struct {
    Layout *L;
    const Doc *d;
    const Metrics *m;
    Shared *sh;             /* measured tables, so nested ones are measured once */
    long y, top;            /* pen; top of the current box, where gaps collapse */
    short left, right;      /* current block margins */
    short x;                /* pen within the current line */
    short zn;               /* zoom: distances are times zn over 9 */
    short zt;               /* and text sizes times zt over 9 (zn is 9 when only the text is scaled) */
    short lead;             /* a tab began the line: where its first text starts, from the margin */
    long line_first;        /* first run of the current line */
    long pending_gap;
    int space;              /* a collapsed space is waiting before the next word */
    char fresh;             /* the next word starts a run of its own (a DD beside its DT) */
    int brk;                /* the line may break here without a space (after an image) */
    char align;
    char marker[20]; short marker_len;  /* list marker waiting for its item's first line */
    char marker_shape;      /* ...a drawn bullet instead of marker's text: 1 + MARK_DISC, _CIRCLE, or _SQUARE */
    char lstyle;            /* this list's TYPE: OL '1' 'a' 'A' 'i' 'I'; UL 'd' disc, 'c' circle, 's' square */
    short basefont;         /* BASEFONT SIZE, what FONT SIZE=+n and -n count from (3) */
    int measure;
    short extent;           /* right edge of the widest line so far */
    Float fl[MAX_FLOATS]; int nfl;      /* this flow's floats (a table cell has its own) */
    Waiting wait[MAX_FLOATS]; int nwait;
    int oom;
} State;

/* ------------------------------------------------------------ buffers */

static char *copy_string(const char *s)
{
    size_t n = strlen(s) + 1;
    char *c = malloc(n);
    if (c) memcpy(c, s, n);
    return c;
}

static int grow(void **p, long *cap, long need, long size)
{
    long n;
    void *q;
    if (need <= *cap) return 0;
    n = *cap ? *cap : 64;
    while (n < need) n *= 2;
    if (!(q = realloc(*p, n * size))) return -1;
    *p = q;
    *cap = n;
    return 0;
}

static MStyle style_of(const State *S, const Ctx *c)
{
    MStyle s;
    s.font = c->pre ? FONT_FIXED : c->font;
    /* PRE is the fixed font at 10, or 9 when made smaller (FONT SIZE=1 or 2, SMALL): these are
       steps on the size ladder from the fixed font's size setting, 10 by default (Courier
       or Mill Mono there: 5 pixels a character, so 80 columns fit a Plus's full-width
       window, as text files and gopher menus need) */
    s.size = zoom_px(S->zt, c->pre ? (c->size_index < 3 ? 9 : 10) : SIZES[c->size_index < 1 ? 1 : c->size_index > 7 ? 7 : c->size_index]);
    s.face = c->face | (c->link ? FACE_UNDERLINE : 0) | (c->dark ? FACE_INVERSE : 0);
    return s;
}

short layout_shift(short size, unsigned char face)
{
    if (face & FACE_SUP) return (short)(size * 2 / 5);
    if (face & FACE_SUB) return (short)-(size / 4 + 1);
    return 0;
}

static Run *add_run(State *S)
{
    Run *r;
    if (grow((void **)&S->L->runs, &S->L->cap_runs, S->L->nruns + 1, sizeof(Run))) { S->oom = 1; return NULL; }
    r = &S->L->runs[S->L->nruns++];
    memset(r, 0, sizeof *r);
    return r;
}

static long add_gen(State *S, const char *s, long len)
{
    long at = S->L->gen_len;
    if (grow((void **)&S->L->gen, &S->L->cap_gen, at + len, 1)) { S->oom = 1; return 0; }
    memcpy(S->L->gen + at, s, len);
    S->L->gen_len += len;
    return at;
}

static void add_box(State *S, long y, short x, short w, short h, short thick)
{
    Box *b;
    if (grow((void **)&S->L->boxes, &S->L->cap_boxes, S->L->nboxes + 1, sizeof(Box))) { S->oom = 1; return; }
    b = &S->L->boxes[S->L->nboxes++];
    b->y = y; b->x = x; b->w = w; b->h = h; b->thick = thick;
}

static void reach(State *S)
{
    if (S->x > S->extent) S->extent = S->x;
}

/* ------------------------------------------------------------ lines */

static void apply_gap(State *S)
{
    if (S->pending_gap && S->y > S->top) S->y += S->pending_gap;
    S->pending_gap = 0;
}

/* Where the next line will go: past any gap waiting before it */
static long next_y(const State *S)
{
    return S->pending_gap && S->y > S->top ? S->y + S->pending_gap : S->y;
}

/* The margins of the line at the pen, narrowed by the floats beside it */
static short line_left(const State *S)
{
    long y = next_y(S);
    short l = S->left;
    int i;
    for (i = 0; i < S->nfl; i++) if (!S->fl[i].right && S->fl[i].bottom > y && S->fl[i].edge > l) l = S->fl[i].edge;
    return l;
}

static short line_right(const State *S)
{
    long y = next_y(S);
    short r = S->right;
    int i;
    for (i = 0; i < S->nfl; i++) if (S->fl[i].right && S->fl[i].bottom > y && S->fl[i].edge < r) r = S->fl[i].edge;
    return r;
}

/* Move the pen below floats: on the left, the right, or both (side 'l', 'r', 'a') */
static void clear_floats(State *S, char side)
{
    int i;
    for (i = 0; i < S->nfl; i++)
        if ((side == 'a' || (side == 'r') == S->fl[i].right) && S->fl[i].bottom > next_y(S)) { apply_gap(S); S->y = S->fl[i].bottom; }
}

static void place_float(State *S, long node, const Ctx *c);

static void finish_line(State *S, int force, const Ctx *c)
{
    short left = line_left(S), right = line_right(S);
    Layout *L = S->L;
    long count = L->nruns - S->line_first, i;
    short asc = 0, desc = 0, lead = 0, used, shift = 0;
    MLine *ln;
    if (count == 0 && !force) return;
    if (count == 0) {
        MStyle s = style_of(S, c);
        S->m->font_metrics(S->m->ctx, &s, &asc, &desc, &lead);
    }
    for (i = S->line_first; i < L->nruns; i++) {
        Run *r = &L->runs[i];
        short a, d, l, up_by;
        MStyle s;
        if (r->kind == RUN_RULE) { short m = ZS(S, 4), up = (r->size + 1) / 2, dn = r->size / 2; if (asc < m) asc = m; if (desc < m) desc = m; if (asc < up) asc = up; if (desc < dn) desc = dn; continue; }
        if (r->kind == RUN_IMAGE || r->kind == RUN_CONTROL) {
            if (r->font > asc) asc = r->font;
            if (r->size - r->font > desc) desc = r->size - r->font;
            continue;
        }
        s.font = r->font; s.size = r->size; s.face = r->kind == RUN_MARK ? 0 : r->face;
        S->m->font_metrics(S->m->ctx, &s, &a, &d, &l);
        up_by = layout_shift(r->size, r->face);     /* SUP and SUB take more of the line above or below */
        if (up_by > 0) a += up_by; else d -= up_by;
        if (a > asc) asc = a;
        if (d > desc) desc = d;
        if (l > lead) lead = l;
    }
    used = S->x - left;
    if (S->align == ALIGN_CENTER) shift = (right - left - used) / 2;
    else if (S->align == ALIGN_RIGHT) shift = right - left - used;
    if (shift > 0) for (i = S->line_first; i < L->nruns; i++) if (L->runs[i].kind != RUN_RULE) L->runs[i].x += shift;
    apply_gap(S);
    if (grow((void **)&L->lines, &L->cap_lines, L->nlines + 1, sizeof(MLine))) { S->oom = 1; return; }
    ln = &L->lines[L->nlines++];
    ln->y = S->y;
    ln->ascent = asc;
    ln->height = asc + desc + lead;
    ln->first = S->line_first;
    ln->count = count;
    if (ln->height > L->tallest) L->tallest = ln->height;
    S->y += ln->height;
    S->line_first = L->nruns;
    S->space = 0;
    S->brk = 0;
    /* floats met during the line take their place now, below it */
    for (i = 0; i < S->nwait; i++) place_float(S, S->wait[i].node, &S->wait[i].c);
    S->nwait = 0;
    S->x = line_left(S);
    S->lead = 0;
}

static void block_break(State *S, long gap, const Ctx *c)
{
    finish_line(S, 0, c);
    if (gap > S->pending_gap) S->pending_gap = gap;
    S->space = 0;
}

static short gap_for(State *S, const Ctx *c)
{
    short a, d, l;
    MStyle s = style_of(S, c);
    S->m->font_metrics(S->m->ctx, &s, &a, &d, &l);
    return (a + d + l) * 2 / 3;
}

/* A list marker hangs to the left of its item's first line */
static void place_marker(State *S, const Ctx *c)
{
    MStyle s = style_of(S, c);
    Run *r;
    long w;
    if (!S->marker_len) return;
    if (S->marker_shape) {
        short dm = (s.size + 1) / 2;
        if (dm < 4) dm = 4;
        if (!(r = add_run(S))) return;
        r->kind = RUN_MARK;
        r->x = line_left(S) - dm - ZS(S, 6);
        r->width = dm;
        r->font = s.font; r->size = s.size;
        r->face = (unsigned char)(S->marker_shape - 1);
        S->marker_len = S->marker_shape = 0;
        return;
    }
    w = S->m->text_width(S->m->ctx, &s, S->marker, S->marker_len);
    if (!(r = add_run(S))) return;
    r->kind = RUN_GEN;
    r->text = add_gen(S, S->marker, S->marker_len);
    r->len = S->marker_len;
    r->x = line_left(S) - (short)w - ZS(S, 6);
    r->width = (short)w;
    r->font = s.font; r->size = s.size; r->face = s.face & ~FACE_UNDERLINE;
    S->marker_len = 0;
}

/* Room for something w wide at the pen: wraps first if it doesn't fit and the line
   may break here. Returns the space to leave before it. */
static long make_room(State *S, const Ctx *c, long w)
{
    long sp = 0;
    int i;
    if (S->L->nruns == S->line_first) {
        /* a line not yet begun starts at the margin, which a list, a quote, or a float may
           have moved; if what comes first won't fit beside the floats, it goes below them */
        for (i = 0; i < MAX_FLOATS && !c->pre && w > line_right(S) - line_left(S); i++) {
            long below = -1, y = next_y(S);
            int k;
            for (k = 0; k < S->nfl; k++) if (S->fl[k].bottom > y && (below < 0 || S->fl[k].bottom < below)) below = S->fl[k].bottom;
            if (below < 0) break;
            apply_gap(S);
            S->y = below;
        }
        S->x = line_left(S) + S->lead;
        S->lead = 0;
    }
    if (S->space && S->x > line_left(S)) {
        MStyle s = style_of(S, c);
        sp = S->m->text_width(S->m->ctx, &s, " ", 1);
    }
    if (!c->pre && (S->space || S->brk) && S->x + sp + w > line_right(S) && S->x > line_left(S)) {
        finish_line(S, 0, c);
        sp = 0;
    }
    if (S->L->nruns == S->line_first) place_marker(S, c);
    return sp;
}

/* Add one word (no spaces, unless pre) at the pen, wrapping first if it doesn't fit */
static void add_word(State *S, const Ctx *c, int kind, long text, long len, const char *bytes)
{
    MStyle s = style_of(S, c);
    long w = S->m->text_width(S->m->ctx, &s, bytes, len);
    long sp = make_room(S, c, w);
    Run *r;
    /* extend the previous run when it's the same style and the text continues after one space */
    if (S->L->nruns > S->line_first && kind == RUN_TEXT && !S->fresh) {
        Run *p = &S->L->runs[S->L->nruns - 1];
        if (p->kind == RUN_TEXT && p->font == s.font && p->size == s.size && p->face == s.face && p->link == c->link &&
            ((sp && p->text + p->len + 1 == text && S->d->text[p->text + p->len] == ' ') || (!sp && p->text + p->len == text)) &&
            p->len + len + 1 < 30000) {
            p->len = (short)(text + len - p->text);
            /* measured whole, as it will be drawn: a scaled font's widths add up differently word by word */
            p->width = (short)S->m->text_width(S->m->ctx, &s, S->d->text + p->text, p->len);
            S->x = p->x + p->width;
            S->space = S->brk = 0;
            reach(S);
            return;
        }
    }
    S->fresh = 0;
    if (!(r = add_run(S))) return;
    r->kind = kind;
    r->text = text;
    r->len = (short)len;
    r->x = S->x + (short)sp;
    r->width = (short)w;
    r->font = s.font; r->size = s.size; r->face = s.face;
    r->link = c->link;
    S->x += (short)(sp + w);
    S->space = 0;
    S->brk = kind == RUN_GEN;
    reach(S);
}

static int is_space(char ch) { return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t'; }

static void flow_text(State *S, const Ctx *c, long off, long len)
{
    const char *t = S->d->text;
    long i = off, end = off + len;
    if (c->pre) {
        while (i < end) {
            long j = i;
            while (j < end && t[j] != '\n' && t[j] != '\r') j++;
            /* a tab moves the pen to the next stop, every 8 spaces of the run's font from where the
               line starts; no tab glyph is drawn (HTML 3.2) */
            long a = i;
            while (a < j) {
                long b = a;
                while (b < j && t[b] != '\t') b++;
                if (b > a) add_word(S, c, RUN_TEXT, a, b - a, t + a);
                if (b < j) {
                    MStyle st = style_of(S, c);
                    long tab = 8 * S->m->text_width(S->m->ctx, &st, " ", 1);
                    if (tab < 1) tab = 1;
                    long origin = line_left(S);
                    long begun = S->L->nruns > S->line_first;
                    long at = begun ? S->x : origin + S->lead;
                    long next = origin + ((at - origin) / tab + 1) * tab;
                    if (begun) S->x = (short)next;
                    else S->lead = (short)(next - origin);      /* the line has no run yet: make_room places it */
                    S->space = 0;
                    reach(S);
                }
                a = b + 1;
            }
            if (j < end) {
                if (t[j] == '\r' && j + 1 < end && t[j + 1] == '\n') j++;
                finish_line(S, 1, c);
            }
            i = j + 1;
        }
        return;
    }
    while (i < end) {
        long j;
        if (is_space(t[i])) { S->space = 1; i++; continue; }
        j = i;
        while (j < end && !is_space(t[j])) j++;
        add_word(S, c, RUN_TEXT, i, j - i, t + i);
        i = j;
    }
}

/* ------------------------------------------------------------ elements */

/* The first name in a FACE list that the System has, else the proportional font */
static int font_from_face(const Metrics *m, const char *face)
{
    char word[40];
    const char *p = face;
    while (*p) {
        int n = 0, f;
        while (*p == ' ' || *p == ',') p++;
        while (*p && *p != ',' && n < 39) word[n++] = *p++;
        while (n > 0 && word[n - 1] == ' ') n--;
        word[n] = 0;
        if (n && m->font_by_name && (f = m->font_by_name(m->ctx, word)) >= 0) return f;
        while (*p && *p != ',') p++;
    }
    return FONT_PROP;
}

static char align_attr(const char *v, char dflt)
{
    if (!v) return dflt;
    if ((v[0] | 32) == 'c' || (v[0] | 32) == 'm') return ALIGN_CENTER;
    if ((v[0] | 32) == 'r') return ALIGN_RIGHT;
    if ((v[0] | 32) == 'l') return ALIGN_LEFT;
    return dflt;
}

static int eq_ci(const char *a, const char *b)
{
    while (*a && *b && (*a | 32) == (*b | 32)) { a++; b++; }
    return !*a && !*b;
}

static void walk(State *S, long node, Ctx c);

/* An OL's TYPE (1, a, A, i, I; case counts), or what it was; a UL's (disc, circle, square) */
static char ol_style(const char *v, char was)
{
    return v && v[0] && strchr("1aAiI", v[0]) ? v[0] : was;
}

static char ul_style(const char *v, char was)
{
    char ch = v ? (char)(v[0] | 32) : 0;
    return ch == 'd' || ch == 'c' || ch == 's' ? ch : was;
}

/* The marker for item number num in an OL: 1., a., A., i., or I. Numbers a letter or a
   Roman numeral can't show (under 1, or Roman over 3999) are digits. Returns its length. */
static short number_text(char *out, long num, char type)
{
    char t[16];
    short n = 0, k;
    if (num >= 1 && (type == 'a' || type == 'A')) {
        long q = num;
        while (q > 0 && n < 6) { q--; t[n++] = (char)((type == 'a' ? 'a' : 'A') + q % 26); q /= 26; }
        for (k = 0; k < n / 2; k++) { char c = t[k]; t[k] = t[n - 1 - k]; t[n - 1 - k] = c; }
    } else if (num >= 1 && num <= 3999 && (type == 'i' || type == 'I')) {
        static const short V[13] = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
        static const char R[] = "m\0cm\0d\0cd\0c\0xc\0l\0xl\0x\0ix\0v\0iv\0i";   /* what V's values are in, packed (no pointers, no globals) */
        const char *p = R;
        int i;
        long q = num;
        for (i = 0; i < 13; i++, p += strlen(p) + 1)
            while (q >= V[i]) {
                const char *u = p;
                for (; *u; u++) t[n++] = type == 'I' ? (char)(*u - 32) : *u;
                q -= V[i];
            }
    } else {
        char rev[12];
        long q = num < 0 ? -num : num;
        short len = 0;
        if (q == 0) rev[len++] = '0';
        while (q > 0 && len < 11) { rev[len++] = (char)('0' + q % 10); q /= 10; }
        n = 0;
        if (num < 0) out[n++] = '-';
        while (len > 0) out[n++] = rev[--len];
        out[n++] = '.';
        return n;
    }
    for (k = 0; k < n; k++) out[k] = t[k];
    out[n++] = '.';
    return n;
}

static int is_compact(const Doc *d, const Node *n) { return html_attr(d, n, "compact") != NULL; }

static void children(State *S, long node, const Ctx *c)
{
    long k;
    for (k = S->d->nodes[node].first; k >= 0 && !S->oom; k = S->d->nodes[k].next) walk(S, k, *c);
}

/* Lay out a node's children in a box of their own (a table cell, a caption) starting
   at y; returns where they end */
static long flow_in(State *S, long node, const Ctx *c, short left, short right, char align, long y)
{
    State C = *S;
    C.left = C.x = left;
    C.right = right;
    C.align = align;
    C.y = C.top = y;
    C.pending_gap = 0;
    C.line_first = S->L->nruns;
    C.space = C.brk = 0;
    C.lead = 0;
    C.marker_len = 0;
    C.extent = 0;
    C.nfl = C.nwait = 0;
    children(&C, node, c);
    finish_line(&C, 0, c);
    if (C.oom) S->oom = 1;
    if (C.extent > S->extent) S->extent = C.extent;
    clear_floats(&C, 'a');  /* the box holds its floats */
    return C.y;
}

/* A node's content width: its widest unbreakable piece, or its unwrapped width */
static short measure(State *S, long node, const Ctx *c, int mode)
{
    Layout *L = S->L;
    long runs = L->nruns, lines = L->nlines, gen = L->gen_len, boxes = L->nboxes;
    State M = *S;
    M.measure = mode;
    M.extent = 0;
    flow_in(&M, node, c, 0, mode == MEASURE_MIN ? 0 : WIDE, ALIGN_LEFT, 0);
    if (M.oom) S->oom = 1;
    L->nruns = runs; L->nlines = lines; L->gen_len = gen; L->nboxes = boxes;
    return M.extent;
}

/* ------------------------------------------------------------ images */

/* An image's size on the page, border included, and its border */
static void image_box(State *S, long node, const Ctx *c, short *total_w, short *total_h, short *border)
{
    const Doc *d = S->d;
    const Node *n = &d->nodes[node];
    const char *alt = html_attr(d, n, "alt"), *v;
    short w = -1, h = -1, nw = 0, nh = 0, a, de, l;
    int known = S->m->image_size && S->m->image_size(S->m->ctx, node, &nw, &nh);
    MStyle s = style_of(S, c);

    if ((v = html_attr(d, n, "width"))) {
        w = (short)atol(v);
        if (strchr(v, '%')) w = S->measure ? 1 : (short)((long)w * (S->right - S->left) / 100);
        else w = ZS(S, w);
    }
    if ((v = html_attr(d, n, "height")) && !strchr(v, '%')) h = ZS(S, (short)atol(v));
    nw = ZS(S, nw);             /* the picture is kept at its normal size, and drawn stretched */
    nh = ZS(S, nh);
    if (known) {
        if (w < 0 && h < 0) { w = nw; h = nh; }
        else if (w < 0) w = nh ? (short)((long)nw * h / nh) : nw;
        else if (h < 0) h = nw ? (short)((long)nh * w / nw) : nh;
    }
    S->m->font_metrics(S->m->ctx, &s, &a, &de, &l);
    if (w < 0 || h < 0) {
        /* not here yet and no size given: a frame around the placeholder icon (11 x 9)
           and the ALT text, or a small square */
        if (alt && *alt) {
            if (w < 0) w = (short)S->m->text_width(S->m->ctx, &s, alt, strlen(alt)) + ZS(S, 8 + 14);
            if (h < 0) h = a + de + ZS(S, 4) < ZS(S, 13) ? ZS(S, 13) : a + de + ZS(S, 4);
        } else {
            if (w < 0) w = ZS(S, 16);
            if (h < 0) h = ZS(S, 16);
        }
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    v = html_attr(d, n, "border");
    *border = v ? (short)atol(v) : (c->link ? 1 : 0);
    if (*border < 0 || *border > 8) *border = 1;
    *border = ZS(S, *border);
    *total_w = w + 2 * *border;
    *total_h = h + 2 * *border;
}

static short space_attr(const State *S, long node, const char *name)
{
    const char *v = html_attr(S->d, &S->d->nodes[node], name);
    short n = v ? (short)atol(v) : 0;
    return n < 0 || n > 64 ? 0 : ZS(S, n);
}

static void image(State *S, long node, const Ctx *c)
{
    const Doc *d = S->d;
    const char *v = html_attr(d, &d->nodes[node], "align");
    short border, hspace = space_attr(S, node, "hspace"), asc, a, de, l, total_w, total_h;
    MStyle s = style_of(S, c);
    long sp;
    Run *r;

    /* ALIGN=LEFT and RIGHT float to the margin, with text alongside: placed now if the
       line is empty, else once it ends. Measuring, they're just their width. */
    if (v && ((v[0] | 32) == 'l' || (v[0] | 32) == 'r') && !S->measure) {
        if (S->L->nruns == S->line_first) place_float(S, node, c);
        else if (S->nwait < MAX_FLOATS) { S->wait[S->nwait].node = node; S->wait[S->nwait].c = *c; S->nwait++; }
        return;
    }
    image_box(S, node, c, &total_w, &total_h, &border);

    /* where it sits against the text: bottom on the baseline unless ALIGN says otherwise */
    S->m->font_metrics(S->m->ctx, &s, &a, &de, &l);
    asc = total_h;
    if (v) {
        if ((v[0] | 32) == 'm' || (v[0] | 32) == 'a') asc = (total_h + a) / 2;
        else if ((v[0] | 32) == 't' || (v[0] | 32) == 'l' || (v[0] | 32) == 'r') asc = a;
    }

    sp = make_room(S, c, total_w + 2 * hspace);
    if (!(r = add_run(S))) return;
    r->kind = RUN_IMAGE;
    r->text = node;
    r->x = S->x + (short)sp + hspace;
    r->width = total_w;
    r->size = total_h;
    r->font = asc;
    r->face = (unsigned char)border;
    r->link = c->link;
    S->x += (short)sp + total_w + 2 * hspace;
    S->space = 0;
    S->brk = 1;
    reach(S);
}

/* Put a floating image at the margin, at the top of the next line, as a line of its own;
   the lines beside it keep to its side until they're past its bottom */
static void place_float(State *S, long node, const Ctx *c)
{
    const Doc *d = S->d;
    const char *v = html_attr(d, &d->nodes[node], "align");
    int right = v && (v[0] | 32) == 'r', i, k;
    short border, total_w, total_h, x, hspace = space_attr(S, node, "hspace"), vspace = space_attr(S, node, "vspace");
    long y;
    Run *r;
    MLine *ln;
    image_box(S, node, c, &total_w, &total_h, &border);
    apply_gap(S);
    y = S->y;
    x = right ? line_right(S) - hspace - total_w : line_left(S) + hspace;
    if (!(r = add_run(S))) return;
    r->kind = RUN_IMAGE;
    r->text = node;
    r->x = x;
    r->width = total_w;
    r->size = total_h;
    r->font = total_h;  /* drawn from its line's top */
    r->face = (unsigned char)border;
    r->link = c->link;
    if (grow((void **)&S->L->lines, &S->L->cap_lines, S->L->nlines + 1, sizeof(MLine))) { S->oom = 1; return; }
    ln = &S->L->lines[S->L->nlines++];
    ln->y = y + vspace;
    ln->ascent = total_h;
    ln->height = total_h;
    ln->first = S->L->nruns - 1;
    ln->count = 1;
    if (total_h > S->L->tallest) S->L->tallest = total_h;
    S->line_first = S->L->nruns;
    /* drop floats already passed, to make room in the list */
    for (i = k = 0; i < S->nfl; i++) if (S->fl[i].bottom > y) S->fl[k++] = S->fl[i];
    S->nfl = k;
    if (S->nfl < MAX_FLOATS) {
        S->fl[S->nfl].bottom = y + total_h + 2 * vspace;
        S->fl[S->nfl].edge = right ? x - hspace : x + total_w + hspace;
        S->fl[S->nfl].right = (char)right;
        S->nfl++;
    }
    if (x + total_w + hspace > S->extent) S->extent = x + total_w + hspace;
    S->x = line_left(S);
}

/* ------------------------------------------------------------ form controls */

static void control(State *S, long node, const Ctx *c)
{
    const Doc *d = S->d;
    const Node *n = &d->nodes[node];
    const char *type = html_attr(d, n, "type");
    short w, h, asc;
    long sp;
    Run *r;
    if (n->tag == TAG_INPUT && type && eq_ci(type, "hidden")) return;
    if (!S->m->control_size || !S->m->control_size(S->m->ctx, node, &w, &h, &asc)) {
        if (S->m->control_size) return;
        w = 80; h = 16; asc = 12;  /* no platform to ask (tests) */
    }
    sp = make_room(S, c, w);
    if (!(r = add_run(S))) return;
    r->kind = RUN_CONTROL;
    r->text = node;
    r->x = S->x + (short)sp;
    r->width = w;
    r->size = h;
    r->font = asc;
    S->x += (short)sp + w;
    S->space = 0;
    S->brk = 1;
    reach(S);
}

/* ------------------------------------------------------------ tables */

typedef struct {
    long node, tr;
    short row, col, cspan, rspan, min, max;
    long h;
    short minh;                  /* HEIGHT in pixels (HTML 3.2 has it on a cell): its row is at least this tall */
    long lines, boxes, anchors;  /* where its content starts in the Layout's arrays */
    char dark, fill;             /* text white; and a fill: 0 none, 1 black, -1 white */
} Cell;

typedef struct {
    Cell *cells; long ncells, cap;
    short ncols, nrows;
    long caption;
    short busy[MAX_COLS];        /* rows still covered by a ROWSPAN from above */
    short cmin[MAX_COLS], cmax[MAX_COLS], cw[MAX_COLS], pct[MAX_COLS], cx[MAX_COLS + 1];
    char fixed[MAX_COLS];
} Grid;

static void collect(State *S, Grid *g, long parent)
{
    const Doc *d = S->d;
    long k, j;
    for (k = d->nodes[parent].first; k >= 0 && !S->oom; k = d->nodes[k].next) {
        short t = d->nodes[k].tag, col = 0, i;
        if (t == TAG_CAPTION) { if (g->caption < 0) g->caption = k; continue; }
        if (t != TAG_TR) {
            /* rows inside something else, a FORM say */
            if (t != TAG_TEXT && t != TAG_TABLE && d->nodes[k].first >= 0) collect(S, g, k);
            continue;
        }
        for (j = d->nodes[k].first; j >= 0; j = d->nodes[j].next) {
            const Node *cn = &d->nodes[j];
            const char *v;
            short cs, rs;
            Cell *cl;
            if (cn->tag != TAG_TD && cn->tag != TAG_TH) continue;
            while (col < MAX_COLS && g->busy[col] > 0) col++;
            if (col >= MAX_COLS) break;
            cs = (v = html_attr(d, cn, "colspan")) ? (short)atoi(v) : 1;
            rs = (v = html_attr(d, cn, "rowspan")) ? (short)atoi(v) : 1;
            if (cs < 1) cs = 1;
            if (rs < 1) rs = 1;
            if (rs > 256) rs = 256;
            if (col + cs > MAX_COLS) cs = MAX_COLS - col;
            if (grow((void **)&g->cells, &g->cap, g->ncells + 1, sizeof(Cell))) { S->oom = 1; return; }
            cl = &g->cells[g->ncells++];
            memset(cl, 0, sizeof *cl);
            cl->node = j;
            cl->tr = k;
            cl->row = g->nrows;
            cl->col = col;
            cl->cspan = cs;
            cl->rspan = rs;
            for (i = col; i < col + cs; i++) g->busy[i] = rs;
            col += cs;
            if (col > g->ncols) g->ncols = col;
        }
        for (i = 0; i < MAX_COLS; i++) if (g->busy[i] > 0) g->busy[i]--;
        g->nrows++;
    }
}

/* Whether a BGCOLOR ("#rrggbb", "rrggbb", or one of HTML's sixteen names) is under half
   bright: 1 dark, 0 light, -1 none or unknown */
static int dark_color(const char *v)
{
    static const struct { const char *name; long rgb; } NAMES[] = {
        { "black", 0x000000 }, { "silver", 0xC0C0C0 }, { "gray", 0x808080 }, { "white", 0xFFFFFF },
        { "maroon", 0x800000 }, { "red", 0xFF0000 }, { "purple", 0x800080 }, { "fuchsia", 0xFF00FF },
        { "green", 0x008000 }, { "lime", 0x00FF00 }, { "olive", 0x808000 }, { "yellow", 0xFFFF00 },
        { "navy", 0x000080 }, { "blue", 0x0000FF }, { "teal", 0x008080 }, { "aqua", 0x00FFFF },
    };
    long rgb = -1;
    int i;
    if (!v) return -1;
    while (*v == ' ') v++;
    for (i = 0; i < (int)(sizeof NAMES / sizeof NAMES[0]); i++) if (eq_ci(v, NAMES[i].name)) rgb = NAMES[i].rgb;
    if (rgb < 0) {
        char *end;
        if (*v == '#') v++;
        rgb = strtol(v, &end, 16);
        if (end - v != 6) return -1;
    }
    /* brightness as the eye sees it: green counts most, blue least */
    return ((rgb >> 16 & 255) * 299 + (rgb >> 8 & 255) * 587 + (rgb & 255) * 114) / 1000 < 128;
}

static Ctx cell_ctx(const Node *cn, Ctx c)
{
    if (cn->tag == TAG_TH) c.face |= FACE_BOLD;
    return c;
}

/* A cell's attribute, or its row's */
static const char *cell_attr(const Doc *d, const Cell *cl, const char *name)
{
    const char *v = html_attr(d, &d->nodes[cl->node], name);
    return v ? v : html_attr(d, &d->nodes[cl->tr], name);
}

static void table_attrs(const State *S, const Doc *d, const Node *n, short *border, short *pad, short *spacing)
{
    const char *v;
    *border = (v = html_attr(d, n, "border")) ? (*v ? (short)atoi(v) : 1) : 0;
    *pad = (v = html_attr(d, n, "cellpadding")) ? (short)atoi(v) : 1;
    *spacing = (v = html_attr(d, n, "cellspacing")) ? (short)atoi(v) : 2;
    if (*border < 0 || *border > 16) *border = 1;
    if (*pad < 0 || *pad > 64) *pad = 1;
    if (*spacing < 0 || *spacing > 64) *spacing = 2;
    *border = ZS(S, *border);
    *pad = ZS(S, *pad);
    *spacing = ZS(S, *spacing);
}

/* Spread a spanning cell's need over its columns */
static void spread(short *cols, short n, short need)
{
    short have = 0, i, extra;
    for (i = 0; i < n; i++) have += cols[i];
    if (have >= need) return;
    extra = need - have;
    for (i = 0; i < n; i++) cols[i] += extra / n + (i == n - 1 ? extra % n : 0);
}

/* Collect the cells and measure each column's narrowest and widest */
static int build_grid(State *S, long node, const Ctx *c, Grid *g, short pad, short spacing)
{
    const Doc *d = S->d;
    long i;
    short k;
    memset(g, 0, sizeof *g);
    g->caption = -1;
    collect(S, g, node);
    for (i = 0; i < g->ncells && !S->oom; i++) {
        Cell *cl = &g->cells[i];
        const Node *cn = &d->nodes[cl->node];
        Ctx cc = cell_ctx(cn, *c);
        const char *v;
        if (cl->row + cl->rspan > g->nrows) cl->rspan = g->nrows - cl->row;
        cl->max = measure(S, cl->node, &cc, MEASURE_MAX) + 2 * pad;
        cl->min = html_attr(d, cn, "nowrap") ? cl->max : measure(S, cl->node, &cc, MEASURE_MIN) + 2 * pad;
        if ((v = html_attr(d, cn, "width")) && cl->cspan == 1) {
            short w = (short)atoi(v);
            if (!strchr(v, '%')) w = ZS(S, w);
            if (strchr(v, '%')) {
                if (w > g->pct[cl->col]) g->pct[cl->col] = w;
            } else if (w > 0) {
                cl->max = w > cl->min ? w : cl->min;
                g->fixed[cl->col] = 1;
            }
        }
    }
    for (i = 0; i < g->ncells; i++) {
        Cell *cl = &g->cells[i];
        if (cl->cspan != 1) continue;
        if (cl->min > g->cmin[cl->col]) g->cmin[cl->col] = cl->min;
        if (cl->max > g->cmax[cl->col]) g->cmax[cl->col] = cl->max;
    }
    for (i = 0; i < g->ncells; i++) {
        Cell *cl = &g->cells[i];
        if (cl->cspan == 1) continue;
        spread(g->cmin + cl->col, cl->cspan, cl->min - (cl->cspan - 1) * spacing);
        spread(g->cmax + cl->col, cl->cspan, cl->max - (cl->cspan - 1) * spacing);
    }
    for (k = 0; k < g->ncols; k++) if (g->cmax[k] < g->cmin[k]) g->cmax[k] = g->cmin[k];
    return S->oom ? -1 : 0;
}

static void sums(const Grid *g, long *smin, long *smax)
{
    short k;
    *smin = *smax = 0;
    for (k = 0; k < g->ncols; k++) { *smin += g->cmin[k]; *smax += g->cmax[k]; }
}

/* Column widths for a table "inner" wide, Netscape's way: every column gets its
   narrowest, then the room left goes in proportion to how much more each wants */
static void distribute(Grid *g, long inner)
{
    long smin, smax, used = 0, weight = 0;
    short k, last = g->ncols - 1;
    int any_free = 0;
    sums(g, &smin, &smax);
    for (k = 0; k < g->ncols; k++) if (!g->fixed[k]) any_free = 1;
    if (inner >= smax) {
        /* more room than wanted: the extra goes to the columns without a set width */
        long extra = inner - smax;
        for (k = 0; k < g->ncols; k++) if (!any_free || !g->fixed[k]) weight += g->cmax[k] + 1;
        for (k = 0; k < g->ncols; k++) {
            long share = (!any_free || !g->fixed[k]) && weight ? extra * (g->cmax[k] + 1) / weight : 0;
            g->cw[k] = g->cmax[k] + (short)share;
        }
    } else if (inner <= smin || smax == smin) {
        for (k = 0; k < g->ncols; k++) g->cw[k] = g->cmin[k];
    } else {
        /* short of room: columns with a set width keep it if the others can still have
           their narrowest, and those others share what's left */
        long fixed_w = 0, free_min = 0, free_max = 0, room, want;
        for (k = 0; k < g->ncols; k++) {
            if (g->fixed[k]) fixed_w += g->cmax[k];
            else { free_min += g->cmin[k]; free_max += g->cmax[k]; }
        }
        if (any_free && fixed_w && inner >= fixed_w + free_min) {
            room = inner - fixed_w - free_min;
            want = free_max - free_min;
            for (k = 0; k < g->ncols; k++)
                g->cw[k] = g->fixed[k] ? g->cmax[k]
                         : g->cmin[k] + (short)(want ? (long)(g->cmax[k] - g->cmin[k]) * room / want : 0);
        } else {
            room = inner - smin;
            want = smax - smin;
            for (k = 0; k < g->ncols; k++) g->cw[k] = g->cmin[k] + (short)((long)(g->cmax[k] - g->cmin[k]) * room / want);
        }
    }
    for (k = 0; k < g->ncols; k++) used += g->cw[k];
    if (inner > used && last >= 0 && inner > smin) g->cw[last] += (short)(inner - used);
}

static Measured *memo(Shared *sh, long node)
{
    long i;
    for (i = 0; i < sh->n; i++) if (sh->tables[i].node == node) return &sh->tables[i];
    return NULL;
}

static void table(State *S, long node, Ctx c)
{
    Layout *L = S->L;
    const Doc *d = S->d;
    const Node *n = &d->nodes[node];
    Grid *g;
    short border, pad, spacing, avail = S->right - S->left, overhead, k, r, wpx = -1, target, tx;
    long smin, smax, i, ty, *rh, *ry;
    const char *v;
    int wpct = 0, caption_below = 0;

    table_attrs(S, d, n, &border, &pad, &spacing);
    if ((v = html_attr(d, n, "width"))) { wpct = strchr(v, '%') != NULL; wpx = (short)atoi(v); if (!wpct) wpx = ZS(S, wpx); }
    block_break(S, 0, &c);

    if (S->measure) {
        /* an enclosing table wants this one's narrowest and widest */
        Measured *mm = memo(S->sh, node);
        short w;
        if (!mm) {
            Measured m;
            if (!(g = malloc(sizeof *g))) { S->oom = 1; return; }
            if (build_grid(S, node, &c, g, pad, spacing)) { free(g->cells); free(g); return; }
            overhead = 2 * border + (g->ncols + 1) * spacing;
            sums(g, &smin, &smax);
            m.node = node;
            m.min = (short)(smin + overhead);
            m.max = (short)(smax + overhead);
            if (wpx > 0 && !wpct && wpx > m.min) m.max = wpx;
            free(g->cells);
            free(g);
            if (grow((void **)&S->sh->tables, &S->sh->cap, S->sh->n + 1, sizeof(Measured))) { S->oom = 1; return; }
            S->sh->tables[S->sh->n++] = m;
            mm = &S->sh->tables[S->sh->n - 1];
        }
        w = S->measure == MEASURE_MIN ? mm->min : mm->max;
        if (S->left + w > S->extent) S->extent = S->left + w;
        return;
    }

    if (!(g = malloc(sizeof *g))) { S->oom = 1; return; }
    if (build_grid(S, node, &c, g, pad, spacing) || !g->ncells) { free(g->cells); free(g); return; }
    overhead = 2 * border + (g->ncols + 1) * spacing;
    sums(g, &smin, &smax);

    /* the table's width: what it asks for, but never wider than the window unless its
       content can't be squeezed (fixed widths were set for bigger screens than these) */
    if (wpx > 0) target = wpct ? (short)((long)avail * (wpx > 100 ? 100 : wpx) / 100) : (wpx < avail ? wpx : avail);
    else target = smax + overhead < avail ? (short)(smax + overhead) : avail;
    if (target < smin + overhead) target = (short)(smin + overhead);
    for (k = 0; k < g->ncols; k++) {
        if (g->pct[k]) {
            short want = (short)((long)(target - overhead) * g->pct[k] / 100);
            if (want > g->cmax[k]) g->cmax[k] = want;
            g->fixed[k] = 1;
        }
    }
    distribute(g, target - overhead);

    tx = S->left;
    {
        char al = align_attr(html_attr(d, n, "align"), S->align);
        if (al == ALIGN_CENTER && target < avail) tx += (avail - target) / 2;
        if (al == ALIGN_RIGHT && target < avail) tx += avail - target;
    }
    g->cx[0] = tx + border + spacing;
    for (k = 0; k < g->ncols; k++) g->cx[k + 1] = g->cx[k] + g->cw[k] + spacing;

    apply_gap(S);
    /* CAPTION ALIGN=BOTTOM goes below the table */
    {
        const char *ca = g->caption >= 0 ? html_attr(d, &d->nodes[g->caption], "align") : NULL;
        caption_below = ca && (ca[0] | 32) == 'b';
    }
    if (g->caption >= 0 && !caption_below) S->y = flow_in(S, g->caption, &c, tx, tx + target, ALIGN_CENTER, S->y);

    /* each cell once, at the top of a box of its own; moved into place below */
    /* a cell's BGCOLOR is its own, its row's, or its table's; without one, it shows what's under it */
    {
        int td = dark_color(html_attr(d, &d->nodes[node], "bgcolor"));
        for (i = 0; i < g->ncells; i++) {
            Cell *cl = &g->cells[i];
            int dk = dark_color(cell_attr(d, cl, "bgcolor"));
            if (dk < 0) dk = td;
            cl->dark = dk < 0 ? c.dark : (char)dk;
            cl->fill = dk < 0 ? 0 : dk ? 1 : c.dark ? -1 : 0;
        }
    }
    for (i = 0; i < g->ncells && !S->oom; i++) {
        Cell *cl = &g->cells[i];
        const Node *cn = &d->nodes[cl->node];
        Ctx cc = cell_ctx(cn, c);
        cc.dark = cl->dark;
        char al = align_attr(cell_attr(d, cl, "align"), cn->tag == TAG_TH ? ALIGN_CENTER : ALIGN_LEFT);
        cl->lines = L->nlines;
        cl->boxes = L->nboxes;
        cl->anchors = L->nanchors;
        cl->h = flow_in(S, cl->node, &cc, g->cx[cl->col] + pad, g->cx[cl->col + cl->cspan] - spacing - pad, al, pad) + pad;
        {
            const char *hv = cell_attr(d, cl, "height");
            cl->minh = hv && !strchr(hv, '%') && atoi(hv) > 0 ? ZS(S, atoi(hv) > 2000 ? 2000 : atoi(hv)) : 0;
        }
    }

    rh = calloc(g->nrows + 1, sizeof(long));
    ry = calloc(g->nrows + 1, sizeof(long));
    if (!rh || !ry) { free(rh); free(ry); free(g->cells); free(g); S->oom = 1; return; }
    for (i = 0; i < g->ncells; i++) {
        Cell *cl = &g->cells[i];
        long ch = cl->h > cl->minh ? cl->h : cl->minh;
        if (cl->rspan == 1 && ch > rh[cl->row]) rh[cl->row] = ch;
    }
    for (i = 0; i < g->ncells; i++) {
        Cell *cl = &g->cells[i];
        long have = (cl->rspan - 1) * spacing, ch = cl->h > cl->minh ? cl->h : cl->minh;
        if (cl->rspan == 1) continue;
        for (r = cl->row; r < cl->row + cl->rspan; r++) have += rh[r];
        if (ch > have) rh[cl->row + cl->rspan - 1] += ch - have;
    }
    ty = S->y;
    ry[0] = ty + border + spacing;
    for (r = 0; r < g->nrows; r++) ry[r + 1] = ry[r] + rh[r] + spacing;

    for (i = 0; i < g->ncells; i++) {
        Cell *cl = &g->cells[i];
        const char *va = cell_attr(d, cl, "valign");
        long boxh = ry[cl->row + cl->rspan] - spacing - ry[cl->row], shift = ry[cl->row], j;
        long end_lines = i + 1 < g->ncells ? g->cells[i + 1].lines : L->nlines;
        long end_boxes = i + 1 < g->ncells ? g->cells[i + 1].boxes : L->nboxes;
        long end_anchors = i + 1 < g->ncells ? g->cells[i + 1].anchors : L->nanchors;
        if (!va || (va[0] | 32) == 'm' || (va[0] | 32) == 'c') shift += (boxh - cl->h) / 2;
        else if ((va[0] | 32) == 'b') shift += boxh - cl->h;
        for (j = cl->lines; j < end_lines; j++) L->lines[j].y += shift;
        for (j = cl->boxes; j < end_boxes; j++) L->boxes[j].y += shift;
        for (j = cl->anchors; j < end_anchors; j++) L->anchors[j].y += shift;
    }
    /* fills and borders after the shifting, so they stay where they're put */
    for (i = 0; i < g->ncells; i++) {
        Cell *cl = &g->cells[i];
        if (cl->fill)
            add_box(S, ry[cl->row], g->cx[cl->col], g->cx[cl->col + cl->cspan] - spacing - g->cx[cl->col],
                    (short)(ry[cl->row + cl->rspan] - spacing - ry[cl->row]), cl->fill > 0 ? 0 : -1);
    }
    for (i = 0; border && i < g->ncells; i++) {
        Cell *cl = &g->cells[i];
        add_box(S, ry[cl->row], g->cx[cl->col], g->cx[cl->col + cl->cspan] - spacing - g->cx[cl->col],
                (short)(ry[cl->row + cl->rspan] - spacing - ry[cl->row]), 1);
    }
    if (border) add_box(S, ty, tx, target, (short)(ry[g->nrows] - ty + border), border);

    S->y = ry[g->nrows] + border;
    if (caption_below) S->y = flow_in(S, g->caption, &c, tx, tx + target, ALIGN_CENTER, S->y);
    S->x = S->left;
    S->line_first = L->nruns;
    S->space = S->brk = 0;
    if (tx + target > S->extent) S->extent = tx + target;
    free(rh);
    free(ry);
    free(g->cells);
    free(g);
}

/* ------------------------------------------------------------ the tree */

static void walk(State *S, long node, Ctx c)
{
    const Doc *d = S->d;
    const Node *n = &d->nodes[node];
    const char *v;
    short save_left = S->left, save_right = S->right;
    char save_align = S->align, save_lstyle = S->lstyle;
    int block = 0;

    if (S->sh->stopped || S->oom) return;  /* out of memory: the page ends here */
    if (S->m->progress && d->nnodes > 0 && node * 100 / d->nnodes > S->sh->shown) {
        /* nodes are in page order, so how far through them is how far along */
        S->sh->shown = (int)(node * 100 / d->nnodes);
        if (S->m->progress(S->m->ctx, S->sh->shown)) { S->sh->stopped = 1; return; }
    }
    if (n->tag == TAG_TEXT) { if (!c.hidden) flow_text(S, &c, n->text, n->len); return; }

    switch (n->tag) {
    case TAG_TITLE: case TAG_SCRIPT: case TAG_STYLE: case TAG_META: case TAG_BASE: case TAG_LINK:
    case TAG_MAP: case TAG_HEAD:
        return;
    case TAG_INPUT: case TAG_SELECT: case TAG_TEXTAREA:
        control(S, node, &c);
        return;
    case TAG_B: case TAG_STRONG: c.face |= FACE_BOLD; break;
    case TAG_I: case TAG_EM: case TAG_CITE: case TAG_VAR: case TAG_DFN: c.face |= FACE_ITALIC; break;
    case TAG_U: c.face |= FACE_UNDERLINE; break;
    case TAG_TT: case TAG_CODE: case TAG_KBD: case TAG_SAMP: c.font = FONT_FIXED; break;
    case TAG_STRIKE: c.face |= FACE_STRIKE; break;
    case TAG_BIG: c.size_index++; break;
    case TAG_SMALL: c.size_index--; break;
    case TAG_SUB: c.size_index--; c.face = (unsigned char)((c.face & ~FACE_SUP) | FACE_SUB); break;
    case TAG_SUP: c.size_index--; c.face = (unsigned char)((c.face & ~FACE_SUB) | FACE_SUP); break;
    case TAG_BASEFONT:
        if ((v = html_attr(d, n, "size")) && atoi(v) > 0) S->basefont = atoi(v) > 7 ? 7 : (short)atoi(v);
        return;
    case TAG_FONT:
        if ((v = html_attr(d, n, "size"))) {
            int rel = (v[0] == '+' || v[0] == '-'), num = atoi(v[0] == '+' ? v + 1 : v), sz = rel ? S->basefont + num : num;
            c.size_index = sz < 1 ? 1 : sz > 7 ? 7 : sz;
        }
        if ((v = html_attr(d, n, "face"))) c.font = (short)font_from_face(S->m, v);
        break;
    case TAG_A:
        if (S->measure) break;  /* measuring: nothing to record */
        if ((v = html_attr(d, n, "name"))) {
            Layout *L = S->L;
            if (grow((void **)&L->anchors, &L->cap_anchors, L->nanchors + 1, sizeof(Anchor)) ||
                !(L->anchors[L->nanchors].name = copy_string(v))) {
                S->oom = 1;
                return;
            }
            L->anchors[L->nanchors].y = S->y + S->pending_gap;
            L->nanchors++;
        }
        if ((v = html_attr(d, n, "href"))) {
            Layout *L = S->L;
            if (grow((void **)&L->links, &L->cap_links, L->nlinks + 1, sizeof(char *)) ||
                !(L->links[L->nlinks] = copy_string(v))) {
                S->oom = 1;
                return;
            }
            c.link = (short)++L->nlinks;
        }
        break;
    case TAG_BR:
        finish_line(S, 1, &c);
        if ((v = html_attr(d, n, "clear")) && ((v[0] | 32) == 'l' || (v[0] | 32) == 'r' || (v[0] | 32) == 'a'))
            clear_floats(S, (char)(v[0] | 32));
        return;
    case TAG_HR:
        block_break(S, gap_for(S, &c) / 2, &c);
        {
            Run *r;
            if (S->L->nruns == S->line_first && (r = add_run(S))) {
                /* HTML 3.2: SIZE is the thickness in pixels (2), WIDTH pixels or a percent of the line (all of
                   it), ALIGN left, center (when narrower than the line), or right, NOSHADE a solid bar.
                   size is the thickness, face 1 for NOSHADE. */
                short full = line_right(S) - line_left(S), w = full, thick = 2, off;
                const char *v;
                char al = ALIGN_CENTER;
                if ((v = html_attr(d, n, "size")) && atoi(v) > 0) thick = (short)(atoi(v) > 200 ? 200 : atoi(v));
                if ((v = html_attr(d, n, "width")) && atoi(v) > 0) {
                    w = strchr(v, '%') ? (short)((long)full * (atoi(v) > 100 ? 100 : atoi(v)) / 100) : ZS(S, atoi(v));
                    if (w > full) w = full;
                }
                al = align_attr(html_attr(d, n, "align"), ALIGN_CENTER);
                off = al == ALIGN_LEFT ? 0 : al == ALIGN_RIGHT ? full - w : (full - w) / 2;
                r->kind = RUN_RULE;
                r->x = line_left(S) + off;
                r->width = S->measure ? 0 : w;
                r->size = ZS(S, thick);
                r->face = html_attr(d, n, "noshade") ? 1 : 0;
                finish_line(S, 1, &c);
            }
        }
        block_break(S, gap_for(S, &c) / 2, &c);
        return;
    case TAG_IMG:
        image(S, node, &c);
        return;
    case TAG_P: case TAG_DIV: case TAG_ADDRESS:
        block = 1;
        block_break(S, n->tag == TAG_DIV ? 0 : gap_for(S, &c), &c);
        S->align = align_attr(html_attr(d, n, "align"), S->align);
        if (n->tag == TAG_ADDRESS) c.face |= FACE_ITALIC;
        break;
    case TAG_CENTER:
        block = 1;
        block_break(S, 0, &c);
        S->align = ALIGN_CENTER;
        break;
    case TAG_H1: case TAG_H2: case TAG_H3: case TAG_H4: case TAG_H5: case TAG_H6:
        block = 1;
        c.size_index = 6 - (n->tag - TAG_H1);
        c.face |= FACE_BOLD;
        block_break(S, gap_for(S, &c), &c);
        S->align = align_attr(html_attr(d, n, "align"), S->align);
        break;
    case TAG_BLOCKQUOTE:
        block = 1;
        block_break(S, gap_for(S, &c), &c);
        S->left += ZS(S, INDENT);
        S->right -= ZS(S, INDENT);
        break;
    case TAG_PRE:
        block = 1;
        block_break(S, gap_for(S, &c), &c);
        c.pre = 1;
        break;
    case TAG_UL: case TAG_OL: case TAG_DIR: case TAG_MENU: case TAG_DL: {
        int nested = 0, compact = is_compact(d, n);
        long p;
        for (p = n->parent; p >= 0; p = d->nodes[p].parent)
            if (d->nodes[p].tag == TAG_UL || d->nodes[p].tag == TAG_OL || d->nodes[p].tag == TAG_DL) nested = 1;
        block = 1;
        block_break(S, nested || compact ? 0 : gap_for(S, &c), &c);
        if (n->tag != TAG_DL) S->left += ZS(S, compact ? 16 : INDENT);
        if (n->tag == TAG_OL) {
            long k, num = (v = html_attr(d, n, "start")) ? atol(v) : 1;
            S->lstyle = ol_style(html_attr(d, n, "type"), '1');
            for (k = n->first; k >= 0 && !S->oom; k = d->nodes[k].next) {
                if (d->nodes[k].tag == TAG_LI) {
                    /* TYPE and VALUE on an item hold for it and the items after it */
                    S->lstyle = ol_style(html_attr(d, &d->nodes[k], "type"), S->lstyle);
                    if ((v = html_attr(d, &d->nodes[k], "value")) && *v) num = atol(v);
                    S->marker_len = number_text(S->marker, num++, S->lstyle);
                    S->marker_shape = 0;
                    block_break(S, 0, &c);
                    children(S, k, &c);
                } else {
                    walk(S, k, c);
                }
            }
            block_break(S, nested || compact ? 0 : gap_for(S, &c), &c);
            S->left = save_left; S->right = save_right; S->align = save_align; S->lstyle = save_lstyle;
            return;
        }
        if (n->tag != TAG_DL) S->lstyle = ul_style(html_attr(d, n, "type"), 'd');
        break;
    }
    case TAG_LI:
        block_break(S, 0, &c);
        if ((v = html_attr(d, n, "type"))) S->lstyle = ul_style(v, S->lstyle);
        S->marker_shape = (char)(1 + (S->lstyle == 'c' ? MARK_CIRCLE : S->lstyle == 's' ? MARK_SQUARE : MARK_DISC));
        S->marker_len = 1;
        break;
    case TAG_DT:
        block_break(S, 0, &c);
        break;
    case TAG_DD: {
        /* DL COMPACT: the definition starts beside a term that's shorter than the indent */
        const Node *dl = n->parent >= 0 ? &d->nodes[n->parent] : NULL;
        long prev = dl ? dl->first : -1;
        short ind = ZS(S, INDENT);
        while (prev >= 0 && d->nodes[prev].next != node) prev = d->nodes[prev].next;
        if (dl && dl->tag == TAG_DL && is_compact(d, dl) && prev >= 0 && d->nodes[prev].tag == TAG_DT &&
            S->L->nruns > S->line_first && S->x + ZS(S, 4) <= S->left + ind) {
            S->left += ind;
            S->x = S->left;
            S->space = 0;
            S->fresh = 1;
        } else {
            block_break(S, 0, &c);
            S->left += ind;
        }
        block = 1;
        break;
    }
    case TAG_TABLE:
        /* tables start below floats rather than squeeze in beside them */
        finish_line(S, 0, &c);
        clear_floats(S, 'a');
        table(S, node, c);
        return;
    /* rows and cells outside any table: rows break lines, cells are separated by space */
    case TAG_CAPTION:
        block = 1;
        block_break(S, 0, &c);
        break;
    case TAG_TR:
        block_break(S, 0, &c);
        break;
    case TAG_TD: case TAG_TH:
        if (n->tag == TAG_TH) c.face |= FACE_BOLD;
        S->space = 1;
        children(S, node, &c);
        S->space = 1;
        return;
    case TAG_FORM:
        block = 1;
        block_break(S, 0, &c);
        break;
    }

    children(S, node, &c);

    if (block) {
        short g = (n->tag == TAG_DIV || n->tag == TAG_CENTER || n->tag == TAG_DD || n->tag == TAG_FORM ||
                   n->tag == TAG_CAPTION) ? 0 : gap_for(S, &c);
        if (n->tag == TAG_UL || n->tag == TAG_DIR || n->tag == TAG_MENU || n->tag == TAG_DL) {
            long p; int nested = is_compact(d, n);
            for (p = n->parent; p >= 0; p = d->nodes[p].parent)
                if (d->nodes[p].tag == TAG_UL || d->nodes[p].tag == TAG_OL || d->nodes[p].tag == TAG_DL) nested = 1;
            if (nested) g = 0;
        }
        block_break(S, g, &c);
        S->lstyle = save_lstyle;
        S->left = save_left;
        S->right = save_right;
        S->align = save_align;
    }
}

static int by_y(const void *a, const void *b)
{
    const MLine *p = a, *q = b;
    if (p->y != q->y) return p->y < q->y ? -1 : 1;
    return p->first < q->first ? -1 : p->first > q->first;
}

/* Give back what the lists grew beyond what they hold; shrinking stays in place */
static void fit(void **p, long *cap, long n, long size)
{
    void *q;
    if (*p && n > 0 && (q = realloc(*p, n * size))) { *p = q; *cap = n; }
}

int layout_doc(Layout *L, const Doc *d, const Metrics *m, short width)
{
    State S;
    Shared sh;
    Ctx c;
    long i;
    memset(L, 0, sizeof *L);
    memset(&S, 0, sizeof S);
    memset(&sh, 0, sizeof sh);
    S.L = L; S.d = d; S.m = m; S.sh = &sh;
    S.basefont = 3;
    S.lstyle = 'd';
    S.zt = m->zoom > 9 ? m->zoom : 9;
    S.zn = m->text_only ? 9 : S.zt;
    S.left = S.x = ZS(&S, MARGIN);
    S.right = width - ZS(&S, MARGIN);
    S.y = S.top = ZS(&S, MARGIN);
    memset(&c, 0, sizeof c);
    c.font = FONT_PROP;
    c.size_index = 3;
    L->width = width;
    walk(&S, 0, c);
    finish_line(&S, 0, &c);
    free(sh.tables);
    L->height = S.y;
    for (i = 0; i < L->nlines; i++)
        if (L->lines[i].y + L->lines[i].height > L->height) L->height = L->lines[i].y + L->lines[i].height;
    L->height += ZS(&S, MARGIN);
    if (L->nlines) qsort(L->lines, L->nlines, sizeof(MLine), by_y);
    fit((void **)&L->lines, &L->cap_lines, L->nlines, sizeof(MLine));
    fit((void **)&L->runs, &L->cap_runs, L->nruns, sizeof(Run));
    fit((void **)&L->gen, &L->cap_gen, L->gen_len, 1);
    fit((void **)&L->links, &L->cap_links, L->nlinks, sizeof(char *));
    fit((void **)&L->anchors, &L->cap_anchors, L->nanchors, sizeof(Anchor));
    fit((void **)&L->boxes, &L->cap_boxes, L->nboxes, sizeof(Box));
    return sh.stopped ? -2 : S.oom ? -1 : 0;
}

void layout_free(Layout *L)
{
    long i;
    for (i = 0; i < L->nlinks; i++) free(L->links[i]);
    for (i = 0; i < L->nanchors; i++) free(L->anchors[i].name);
    free(L->lines); free(L->runs); free(L->gen); free(L->links); free(L->anchors); free(L->boxes);
    memset(L, 0, sizeof *L);
}

long layout_line_at(const Layout *L, long y)
{
    long lo = 0, hi = L->nlines;
    y -= L->tallest;
    while (lo < hi) {
        long mid = (lo + hi) / 2;
        if (L->lines[mid].y < y) lo = mid + 1; else hi = mid;
    }
    return lo;
}

const Run *layout_hit(const Layout *L, long y, short x)
{
    long i, k;
    for (i = layout_line_at(L, y); i < L->nlines && L->lines[i].y <= y; i++) {
        const MLine *ln = &L->lines[i];
        if (y >= ln->y + ln->height) continue;
        for (k = ln->first; k < ln->first + ln->count; k++) {
            const Run *r = &L->runs[k];
            if (x >= r->x && x < r->x + r->width) return r;
        }
    }
    return NULL;
}
