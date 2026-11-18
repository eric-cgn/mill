/* Mill: a web browser for classic Macs (monochrome 68000 Macs). */
#include <Multiverse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/auth.h"
#include "core/charset.h"
#include "core/cookie.h"
#include "core/filetypes.h"
#include "core/fmt.h"
#include "core/fontsize.h"
#include "core/form.h"
#include "core/html.h"
#include "core/imagemap.h"
#include "core/image.h"
#include "core/layout.h"
#include "core/refresh.h"
#include "core/resume.h"
#include "core/url.h"
#include "mac_icons.h"
#include "mac_local.h"
#include "mac_login.h"
#include "mac_net.h"
#include "mac_memreport.h"
#include "mac_plugin.h"
#include "mac_save.h"
#include "mac_start.h"
#include "mac_upload.h"
#include "mac_sys.h"
#include "version.h"

enum { MENU_APPLE = 128, MENU_FILE, MENU_EDIT, MENU_VIEW, MENU_GO, MENU_BOOKMARKS, MENU_POPUP = 250, MENU_CONTEXT };
enum { FILE_NEW = 1, FILE_LOCATION, FILE_OPEN, FILE_CLOSE, FILE_PAGE_SETUP = 6, FILE_PRINT, FILE_QUIT = 9 };
enum { EDIT_CUT = 3, EDIT_COPY, EDIT_PASTE, EDIT_SELECT_ALL, EDIT_FIND, EDIT_FIND_AGAIN, EDIT_PREFERENCES = 10, EDIT_CLEAR_LOGINS };
enum { GO_BACK = 1, GO_FORWARD, GO_RELOAD, GO_HOME, GO_STOP = 6 };
enum { VIEW_LARGER = 1, VIEW_SMALLER, VIEW_PAGE_SOURCE = 4, VIEW_FULL_SCREEN = 6 };
enum { BM_ADD = 1, BM_FIRST = 3, BM_EDIT = BM_FIRST + BOOKMARKS + 1 };  /* the slots follow a line */
#define BAR 14          /* toolbar height: the address's highlight, and a pixel each side */
#define STATUS_SECS 5   /* a message shows this long once Mill is idle */
#define SCROLL 16
#define MAX_PAGE (192L * 1024)
#define MIN_PAGE (24L * 1024)   /* when memory's short, pages are cut shorter, down to this */
#define HISTORY MILL_HISTORY_MAX    /* history_size() says how many are kept */
#define FILE_MSG "Web pages can\325t open this Mac\325s files or Mill\325s own pages. Type the address to go there."
#define FORM_MSG "Forms can only send to the same kind of address as their page."
#define ALERT_MEMORY 202
#define ALERT_UNSENT 203
#define ALERT_NO_MEMORY 204
#define ALERT_FAILED 205
#define ALERT_RESUME 207
enum { UNSENT_STAY = 1, UNSENT_LEAVE };

/* One per distinct picture on the page (the same SRC at the same size is fetched once) */
typedef struct {
    char *url;
    short aw, ah;       /* WIDTH and HEIGHT from the page, -1 when not given */
    char state;         /* PIC_WAITING, PIC_SHOWN, PIC_FAILED */
    Boolean owned;      /* its bits are its own, not the picture memory's */
    Bitmap1 bm;
} Pic;
enum { PIC_WAITING, PIC_SHOWN, PIC_FAILED };

/* A browser window and its page */
typedef struct Browser {
    struct Browser *next;
    WindowPtr win;
    TEHandle url_te;
    TEHandle field_te;          /* typing in a text field or text area happens here */
    ControlHandle vscroll, hscroll;   /* hscroll shows only when the page is wider than the view */
    Boolean hbar, loading;
    Rect back_r, fwd_r, reload_r, mill_r, view_r, status_r;
    char status_text[128];
    Boolean status_up, status_held;  /* showing; held by the mouse on a link */
    long status_tick;                /* when it last changed */
    short mill_frame;
    long mill_tick;
    Doc doc;
    Layout lay;
    Forms forms;
    Boolean have_page;
    long scroll_y;              /* page pixels above the view */
    long scroll_x;              /* page pixels left of the view */
    short page_w;               /* how wide the laid-out page is */
    short scroll_scale;         /* page pixels per scroll bar unit (control values are 16-bit) */
    char page_url[300];
    char page_owner[32];        /* the Plug-In that marked this page as its own, or "" */
    char page_login[256];       /* the key of the login it was fetched with, or "" */
    Plug *held;                 /* the Plug-In that made it, kept loaded while it shows */
    char *history[HISTORY];     /* see remember */
    short hist_len, hist_pos;
    char *form_base;            /* form state brought back that had been sent, or NULL */
    Pic *pics;
    short npics;
    short *pic_of;              /* IMG node -> index in pics + 1, or 0 */
    long focus;                 /* the field being typed in, -1 for the address, or -2 for the page */
    long sel_a, sel_b;          /* with -2, the page's text selected: Doc.text offsets, either order */
    Boolean focus_shown;        /* the focused field was on screen at the last draw */
    Rect focus_clip;            /* and where */
    unsigned long refresh_at;   /* client pull: the TickCount to load again at, 0 for none */
    char refresh_url[300];      /* ...the address to load, "" for this page */
} Browser;

static Browser *browsers;       /* every window, front or not */
static Browser *br;             /* the one being worked on */
static Boolean quitting = false;
static Jar jar;         /* cookies, while Mill runs */
#define PAGE_LEFT (br->view_r.left - (short)br->scroll_x)  /* where page x 0 is in the window */

/* Full screen (see full_screen): the front window covers the whole screen, the menu bar
   hidden; just the page, and its scroll bars if the setting says so */
static Browser *full;           /* that window, or NULL */
static Rect full_was;           /* its content before, in global coordinates */
static Boolean full_bars;       /* the setting, as it was when full screen began */
static short bar_height;        /* the menu bar's height while it's hidden, else 0 */
static RgnHandle bar_rgn;       /* the menu bar's rectangle, added to the desktop while it's hidden */
static long loaded_tick;        /* when loading last ended: an Escape from before then was a Stop */
#define NO_BARS (br == full && !full_bars)

/* ------------------------------------------------------------ geometry */

static void geometry(void)
{
    Rect r = br->win->portRect;
    if (br == full) {   /* no toolbar: its buttons are nowhere */
        SetRect(&br->back_r, 0, 0, 0, 0);
        br->fwd_r = br->reload_r = br->mill_r = br->back_r;
        SetRect(&br->view_r, 0, 0, r.right - (full_bars ? SCROLL - 1 : 0), r.bottom - (br->hbar ? SCROLL - 1 : 0));
        return;
    }
    SetRect(&br->back_r, 3, 3, 3 + TOOL_ICON, 3 + TOOL_ICON);  /* level with the address's text */
    br->fwd_r = br->back_r;
    OffsetRect(&br->fwd_r, 13, 0);
    br->reload_r = br->fwd_r;
    OffsetRect(&br->reload_r, 13, 0);
    SetRect(&br->mill_r, r.right - SCROLL + 1, 0, r.right - SCROLL + 1 + MILL_ICON, MILL_H);  /* above the scroll bar */
    SetRect(&br->view_r, 0, BAR + 1, r.right - SCROLL + 1, r.bottom - (br->hbar ? SCROLL - 1 : 0));
}

/* The clip as it is, to put back with unclip. NULL if there wasn't the memory for a
   region (drawing on a NULL region would write over low memory); unclip then sets the
   clip to the whole window. */
static RgnHandle keep_clip(void)
{
    RgnHandle save = NewRgn();
    if (save) GetClip(save);
    return save;
}

static void unclip(RgnHandle save)
{
    if (!save) { ClipRect(&qd.thePort->portRect); return; }
    SetClip(save);
    DisposeRgn(save);
}

/* The watch, while Mill is busy */
static void watch_cursor(void)
{
    CursHandle c = GetCursor(watchCursor);
    if (c) SetCursor(*c);
}

/* Drawing limited to r, within the page view; returns the clip to put back */
static RgnHandle clip_to(const Rect *r)
{
    RgnHandle save = keep_clip();
    Rect c;
    SectRect(r, &br->view_r, &c);
    ClipRect(&c);
    return save;
}

/* ------------------------------------------------------------ icons */

static void draw_bits(const unsigned short *rows, short n, short width, const Rect *where, Boolean dim)
{
    BitMap bm;
    Rect src;
    unsigned short buf[16];
    short i;
    memcpy(buf, rows, n * 2);
    /* Dimmed: every other pixel cleared, in a checkerboard that starts at the
       picture's own top left corner, so mirror-image pictures (back and forward)
       dim to mirror images wherever they sit */
    if (dim)
        for (i = 0; i < n; i++) buf[i] &= i % 2 ? 0x5555 : 0xAAAA;
    bm.baseAddr = (Ptr)buf;
    bm.rowBytes = 2;
    SetRect(&bm.bounds, 0, 0, 16, n);
    SetRect(&src, 0, 0, width, n);
    CopyBits(&bm, &qd.thePort->portBits, &src, where, srcCopy, NULL);
}

static void draw_mill(void)
{
    if (br == full) return;
    draw_bits(ICON_MILL[br->mill_frame], MILL_H, 16, &br->mill_r, false);
}

/* Turn the sails a step; called while loading, at most every few ticks */
static void spin(void)
{
    GrafPtr old;
    if (TickCount() - br->mill_tick < 4) return;
    br->mill_tick = TickCount();
    br->mill_frame = (br->mill_frame + 1) & 3;
    GetPort(&old);
    SetPort(br->win);
    draw_mill();
    SetPort(old);
}

static void draw_toolbar(void)
{
    Rect r = br->win->portRect;
    if (br == full) return;
    draw_bits(ICON_BACK, TOOL_ICON, TOOL_ICON, &br->back_r, br->hist_pos <= 0);
    draw_bits(ICON_FORWARD, TOOL_ICON, TOOL_ICON, &br->fwd_r, br->hist_pos >= br->hist_len - 1);
    /* reload, or a stop sign while something loads */
    draw_bits(br->loading ? ICON_STOP : ICON_RELOAD, TOOL_ICON, TOOL_ICON, &br->reload_r, !br->loading && br->hist_pos < 0);
    TEUpdate(&(*br->url_te)->viewRect, br->url_te);
    draw_mill();
    MoveTo(r.left, BAR);
    LineTo(r.right, BAR);
}

/* Something is loading, or has finished: the reload button turns to a stop sign and back */
static void set_loading(Boolean on)
{
    GrafPtr old;
    br->loading = on;
    if (!on) loaded_tick = TickCount();
    if (br == full) return;
    GetPort(&old);
    SetPort(br->win);
    draw_bits(br->loading ? ICON_STOP : ICON_RELOAD, TOOL_ICON, TOOL_ICON, &br->reload_r, !br->loading && br->hist_pos < 0);
    SetPort(old);
}

/* ------------------------------------------------------------ status message */

/* Black text on a small white patch, lined along its top and right, over the page's
   bottom left corner. It stays while something loads or the mouse is on a link, and goes
   STATUS_SECS after it last changed. */
static Boolean page_limited;            /* draw_page: only page_limit */
static Boolean plain_cells;             /* draw_page: dark cells white, with black text (printing) */
static Rect page_limit;

static void draw_page(short only);

static void status_box(Rect *r)
{
    short w;
    TextFont(kFontIDGeneva);
    TextSize(9);
    TextFace(0);
    w = TextWidth(br->status_text, 0, strlen(br->status_text)) + 9;
    if (w > br->view_r.right - br->view_r.left) w = br->view_r.right - br->view_r.left;
    SetRect(r, br->view_r.left, br->view_r.bottom - 13, br->view_r.left + w, br->view_r.bottom);
}

static void draw_status(void)
{
    RgnHandle save;
    if (!br->status_up || br == full) return;
    status_box(&br->status_r);
    save = clip_to(&br->status_r);
    EraseRect(&br->status_r);
    /* a line along the top and down the right, so it reads as its own patch */
    MoveTo(br->status_r.left, br->status_r.top);
    LineTo(br->status_r.right - 1, br->status_r.top);
    LineTo(br->status_r.right - 1, br->status_r.bottom - 1);
    MoveTo(br->status_r.left + 4, br->status_r.bottom - 3);
    DrawText(br->status_text, 0, strlen(br->status_text));
    unclip(save);
}

/* Draw the page again where r covered it */
static void draw_page_area(const Rect *r)
{
    page_limit = *r;
    page_limited = true;
    draw_page(-1);
    page_limited = false;
}

static void hide_status(void)
{
    GrafPtr old;
    if (!br->status_up) return;
    br->status_up = br->status_held = false;
    GetPort(&old);
    SetPort(br->win);
    draw_page_area(&br->status_r);
    SetPort(old);
}

static void set_status(const char *msg)
{
    GrafPtr old;
    Rect was = br->status_r;
    Boolean was_up = br->status_up;
    br->status_tick = TickCount();
    br->status_held = false;
    if (!strcmp(msg, br->status_text) && (br->status_up || !msg[0])) { spin(); return; }
    strncpy(br->status_text, msg, sizeof br->status_text - 1);
    if (!msg[0]) { hide_status(); spin(); return; }
    GetPort(&old);
    SetPort(br->win);
    br->status_up = true;
    status_box(&br->status_r);
    if (was_up && was.right > br->status_r.right) {
        /* the part a longer message covered */
        was.left = br->status_r.right;
        draw_page_area(&was);
    }
    draw_status();
    SetPort(old);
    spin();
}

/* Called when Mill is idle */
static void status_idle(void)
{
    if (br->status_up && !br->loading && !br->status_held && TickCount() - br->status_tick > STATUS_SECS * 60L) hide_status();
}

/* ------------------------------------------------------------ QuickDraw metrics */

/* The font settings: a proportional and a fixed font, chosen by name (the System's installed
   fonts, and the families of any font suitcase in the Plug-Ins folder). Text in the core's
   FONT_PROP and FONT_FIXED is measured and drawn in them; a FACE the System has is its own font.
   Sizes are the core's (HTML 3.2's ladder). A suitcase's fonts are measured and drawn the same
   as the System's, so widths agree. */
static const short LADDER[7] = { 9, 10, 12, 14, 18, 24, 36 };
static short font_prop = 20, font_fixed = 22;    /* Times and Courier until fonts_choose */

static short ladder_at(short size)
{
    short k;
    for (k = 0; k < 6 && LADDER[k] < size; k++) {}
    return k;
}

/* A suitcase's family by font number */
static PackFam *pack_by_id(short id)
{
    short i;
    for (i = 0; i < npack_fams; i++) if (pack_fams[i].id == id) return &pack_fams[i];
    return NULL;
}

static Boolean same_name(const char *a, const char *b);

static PackFam *pack_by_name(const char *name)
{
    short i;
    for (i = 0; i < npack_fams; i++) if (same_name(pack_fams[i].name, name)) return &pack_fams[i];
    return NULL;
}

/* Whether a suitcase's family has every letter one width: its strike at the first size it has,
   measured with its file current. Decided once. */
static Boolean pack_fixed(PackFam *f)
{
    if (f->fixed < 0) {
        short was = CurResFile(), k = 0, sf = qd.thePort->txFont, ss = qd.thePort->txSize, w;
        static const short ladder[7] = { 9, 10, 12, 14, 18, 24, 36 };
        Style sface = qd.thePort->txFace;
        while (k < 6 && !(f->sizes & (1 << k))) k++;
        UseResFile(f->ref);
        TextFont(f->id);
        TextSize(ladder[k]);
        TextFace(0);
        w = CharWidth('i');
        f->fixed = w > 0 && w == CharWidth('M') && w == CharWidth('W');
        UseResFile(was);
        TextFont(sf);
        TextSize(ss);
        TextFace(sface);
    }
    return f->fixed;
}

static int font_number(const char *name);

/* The name used when a setting is empty or names a font that's gone: Web Fonts' Mill Serif and
   Mill Mono, or the System's Times and Courier without it */
const char *font_default(int fixed)
{
    const char *mill = fixed ? "Mill Mono" : "Mill Serif";
    return pack_by_name(mill) ? mill : fixed ? "Courier" : "Times";
}

static short name_pstr(const char *name, Str255 p)
{
    short n = (short)strlen(name);
    if (n > 31) n = 31;
    p[0] = (unsigned char)n;
    memcpy(p + 1, name, n);
    return n;
}

static Boolean same_name(const char *a, const char *b)
{
    while (*a && *b && (*a | 32) == (*b | 32)) { a++; b++; }
    return !*a && !*b;
}

/* An installed font's number, or -1. The suitcase's two aren't found by name unless its file
   is current, so they're known here. */
static int font_number(const char *name)
{
    Str255 p;
    short num;
    PackFam *pf;
    if (!name[0]) return -1;
    if ((pf = pack_by_name(name))) return pf->id;      /* a suitcase's: not found by name unless its file is current */
    name_pstr(name, p);
    GetFNum(p, &num);
    if (num == 0 && !same_name(name, "Chicago")) return -1;   /* 0 is also "not found" */
    return num;
}

static int qd_font_by_name(void *ctx, const char *name)
{
    (void)ctx;
    return font_number(name);
}

static Metrics qd_m;            /* (defined below) */
static short dist_zoom(void);

/* Form controls follow the zoom: their text by the text's factor (the System's Geneva, scaled by
   QuickDraw), their boxes and padding by the distances' (the text's, unless only the text is scaled) */
static short zscale(short z, long v)
{
    long r;
    if (z <= 9 || v == 0) return (short)v;
    r = (v * z + (v < 0 ? -4 : 4)) / 9;
    return (short)(r == 0 ? (v < 0 ? -1 : 1) : r);
}
#define CT(v) zscale(qd_m.zoom, (v))
#define CD(v) zscale(dist_zoom(), (v))

static void faces_forget(void);

void fonts_choose(void)
{
    int n = font_number(setting("font_prop", ""));
    qd_m.text_only = !strcmp(setting("scale_text_only", "no"), "yes");
    font_prop = n >= 0 ? n : font_number(font_default(0));
    if (font_prop < 0) font_prop = 20;
    n = font_number(setting("font_fixed", ""));
    font_fixed = n >= 0 ? n : font_number(font_default(1));
    if (font_fixed < 0) font_fixed = 22;
    faces_forget();                 /* the page fonts' stand-ins go with them */
}

/* A text style as the Font Manager is to get it: font, size, whether the font is the
   suitcase's (its file then has to be current), and whether its style is real only in an
   outline (Times Bold where Times has plain bitmaps). Or one the font Plug-In draws (plug:
   its family, from 1), measured and drawn with millFont, its lines as the Plug-In says
   (asc, desc, lead); QuickDraw draws it in font if the Plug-In can't. adds: its widths add up (a line is the sum
   of its pieces), so layout needn't measure a growing run again. */
typedef struct { short font, size, pack_ref, plug, asc, desc, lead; Boolean outline, adds; } Face;   /* pack_ref: the suitcase's file if its font is used, else 0 */

/* Whether the System has a strike of the font at the size (not one scaled from another), remembered
   for the last few asked, as text is measured a run at a time */
static Boolean real_size(short font, short size)
{
    static struct { short font, size; Boolean real; } seen[8];
    static short next;
    short i;
    for (i = 0; i < 8; i++) if (seen[i].size && seen[i].font == font && seen[i].size == size) return seen[i].real;
    i = next++ & 7;
    seen[i].font = font;
    seen[i].size = size;
    seen[i].real = RealFont(font, size);
    return seen[i].real;
}

/* A family's real styles: the entries of its FOND's association table (Inside Macintosh: Text,
   "Font Family Resources": a size, 0 for an outline, a style, and a font ID) with a style, whose
   FONT, NFNT, or 'sfnt' is there; outlines only with TrueType. Remembered for a few families. */
#define STYLED_MAX 24
typedef struct { Boolean known; short font, ref, n; struct { unsigned char size, style; } e[STYLED_MAX]; } Styled;
static Styled styled_seen[4];
static short styled_next;

static Boolean strike_there(short id, Boolean outline)
{
    Boolean there;
    SetResLoad(false);
    if (outline) there = GetResource('sfnt', id) != NULL;
    else there = GetResource('NFNT', id) != NULL || GetResource('FONT', id) != NULL;
    SetResLoad(true);
    return there;
}

static const Styled *styled_of(short font, short ref)
{
    Styled *st;
    Handle h;
    short i, n, was = -1, id[STYLED_MAX];
    for (i = 0; i < 4; i++) if (styled_seen[i].known && styled_seen[i].ref == ref && styled_seen[i].font == font) return &styled_seen[i];
    st = &styled_seen[styled_next++ & 3];
    st->known = true;
    st->font = font;
    st->ref = ref;
    st->n = 0;
    if (ref) { was = CurResFile(); UseResFile(ref); }
    if ((h = ref ? Get1Resource('FOND', font) : GetResource('FOND', font))) {
        const unsigned char *p = (const unsigned char *)*h;
        long size = GetHandleSize(h), count, k;
#define W(o) ((short)((p[o] << 8) | p[(o) + 1]))
        count = size >= 54 ? (long)W(52) + 1 : 0;
        for (k = 0; k < count && 54 + k * 6 + 6 <= size && st->n < STYLED_MAX; k++) {
            short sz = W(54 + k * 6), style = W(56 + k * 6);
            if (style <= 0 || style > 255 || sz < 0 || sz > 255 || (sz == 0 && !sys_outlines)) continue;
            st->e[st->n].size = (unsigned char)sz;
            st->e[st->n].style = (unsigned char)style;
            id[st->n++] = W(58 + k * 6);
        }
#undef W
        for (k = n = 0; k < st->n; k++)    /* (looking may move the FOND: its entries are copied first) */
            if (strike_there(id[k], st->e[k].size == 0)) st->e[n++] = st->e[k];
        st->n = n;
    }
    if (was >= 0) UseResFile(was);
    return st;
}

/* 1 if the family has a bitmap of the size in the style, 2 an outline */
static int styled_has(short font, short ref, short size, int style)
{
    const Styled *st = styled_of(font, ref);
    short i;
    int w = 0;
    for (i = 0; i < st->n; i++)
        if (st->e[i].style == style) {
            if (st->e[i].size == size) return 1;
            if (st->e[i].size == 0) w = 2;
        }
    return w;
}

/* The strikes text in a font can be drawn from: the System's font (sys) or a font file's
   family (pack, its sizes on the ladder), with the font Plug-In's outlines of that family
   from min to max (min 0: none). small: the run is 10 point or less (sys_has). */
typedef struct { short sys; PackFam *pack; short min, max; Boolean small; } Strikes;
enum { HAS_SYS = 1, HAS_PACK, HAS_OUTLINE, HAS_PLUG };     /* HAS_OUTLINE: the System's, the style real in an outline only;
                                                              HAS_PLUG: the font Plug-In's */

static Boolean pack_has(PackFam *pf, short size)
{
    short k = ladder_at(size);
    return LADDER[k] == size && (pf->sizes & (1 << k));
}

/* For font_pick: a real strike of the size in the style. The System's: any size RealFont
   answers for (a bitmap, or an outline at every size with TrueType), and a style's bitmap,
   or its outline, unless it's bold and the run 10 point or less: a bold outline drawn 1 bit
   deep has a plain one's stems there (Times Bold at 9 and 10), so QuickDraw makes bold, a
   pixel wider, from the plain strike. Asked of the run's size, not each one tried, so bold
   10 isn't drawn at 11. A font file's: its bitmaps, and its outlines in every style (the Plug-In
   makes the ones it lacks). */
static int sys_has(void *ctx, short size, int style)
{
    short font = ((Strikes *)ctx)->sys;
    if (!style) return real_size(font, size) ? HAS_SYS : 0;
    switch (styled_has(font, 0, size, style)) {
    case 1: return HAS_SYS;
    case 2: return (style & FACE_BOLD) && ((Strikes *)ctx)->small ? 0 : HAS_OUTLINE;
    }
    return 0;
}

static int pack_has_style(void *ctx, short size, int style)
{
    Strikes *st = ctx;
    if (style ? styled_has(st->pack->id, st->pack->ref, size, style) == 1 : pack_has(st->pack, size)) return HAS_PACK;
    return st->min && size >= st->min && size <= st->max ? HAS_PLUG : 0;
}

/* The font Plug-In's family of the name, or -1 */
static short plug_font_of(const char *name)
{
    short i;
    for (i = 0; i < nplug_fonts; i++) if (same_name(plug_fonts[i].name, name)) return i;
    return -1;
}

/* The font Plug-In can draw its family k at the size in the style: it's loaded, and has the
   memory to begin. Its lines' ascent, descent, and leading at the size go in f. */
static Boolean plug_ready(short k, short size, int style, Face *f)
{
    MillFontParams p;
    memset(&p, 0, sizeof p);
    p.what = millFontOpen;
    p.font = k;
    p.size = size;
    p.face = (short)style;
    if (plugin_font(&p) != noErr) return false;
    f->asc = p.ascent;
    f->desc = p.descent;
    f->lead = p.leading;
    return true;
}

/* The size and strike a run is drawn in (font_pick): one of the size, or near it, in the
   run's style, then plain; else the size asked for, scaled by the Font Manager. A font
   file's family (Mill Serif, Mill Mono) is its strikes alone, the System's font its own
   alone. Measured at it too, so lines are as tall as the size drawn. Remembered, as text is
   measured a run at a time. */
static struct { short font, size; unsigned char style; Face f; } faces_seen[16];
static short faces_next;

static void faces_forget(void)
{
    memset(faces_seen, 0, sizeof faces_seen);
    memset(styled_seen, 0, sizeof styled_seen);
}

static Face face_of(const MStyle *s)
{
    Face f;
    PackFam *pf;
    Strikes st;
    short i, font;
    unsigned char style = s->face & (FACE_BOLD | FACE_ITALIC);
    int w;
    if (s->font == FONT_PROP) font = font_prop;
    else if (s->font == FONT_FIXED) font = font_fixed;
    else font = s->font;
    for (i = 0; i < 16; i++)
        if (faces_seen[i].size && faces_seen[i].font == font && faces_seen[i].size == s->size && faces_seen[i].style == style) return faces_seen[i].f;
    memset(&f, 0, sizeof f);
    f.font = font;
    if ((pf = pack_by_id(font))) {
        short k = plug_font_of(pf->name);
        st.pack = pf;
        st.min = k >= 0 ? plug_fonts[k].min : 0;
        st.max = k >= 0 ? plug_fonts[k].max : 0;
        f.size = font_pick(s->size, style, pack_has_style, &st, &w);    /* the core's sizes, the ladder's times the zoom */
        if (w == HAS_PLUG && !plug_ready(k, f.size, style, &f)) {
            st.min = 0;                                /* no memory for it: the nearest bitmap */
            f.size = font_pick(s->size, style, pack_has_style, &st, &w);
        }
        f.font = pf->id;                               /* (no strike near: the Font Manager scales the suitcase's) */
        f.pack_ref = pf->ref;
        if (w == HAS_PLUG) f.plug = k + 1;
    } else {
        st.sys = font;
        st.small = s->size <= 10;
        f.size = font_pick(s->size, style, sys_has, &st, &w);
    }
    f.outline = w == HAS_OUTLINE;
    /* a bitmap strike (no TrueType: one RealFont answers for is a bitmap) or the Plug-In's
       outline: whole pixels a character, so a run is as wide as its pieces (qd_adds_up) */
    f.adds = w == HAS_PACK || w == HAS_PLUG || (w == HAS_SYS && !sys_outlines);
    i = faces_next++ & 15;
    faces_seen[i].font = font;
    faces_seen[i].size = s->size;
    faces_seen[i].style = style;
    faces_seen[i].f = f;
    return f;
}

/* With TrueType, the Font Manager takes a bitmap over an outline (plain Times 24, made bold by
   QuickDraw, over Times Bold's outline) unless outlines are preferred (Inside Macintosh: Text,
   SetOutlinePreferred): so they are, while a run whose style is real only in an outline is
   measured or drawn, and put back after (false, the default; Mill sets it nowhere else), so
   widths agree. */
static Boolean outline_enter(const Face *f)
{
    if (!f->outline) return false;
    SetOutlinePreferred(true);
    return true;
}

static void outline_leave(Boolean set)
{
    if (set) SetOutlinePreferred(false);
}

/* The Font Manager looks for a font from the current resource file down, and the suitcase
   is above Mill's own file in the chain: it's current only while text is measured or drawn
   in its fonts. font_enter gives what to hand font_leave. */
static short font_enter(const Face *f)
{
    short was = -1;
    if (f->pack_ref) {
        was = CurResFile();
        UseResFile(f->pack_ref);
    }
    return was;
}

static void font_leave(short was)
{
    if (was >= 0) UseResFile(was);
}

/* The fonts the settings list: the System's FONDs by name (found in every open file, the
   suitcase's too), with no symbol fonts, and for the fixed font only those whose letters
   are all one width. */
int font_list(int fixed, char (*names)[FONT_NAME_MAX], int max)
{
    int total = (int)CountResources('FOND'), n = 0, i, j;
    short sf = qd.thePort->txFont, ss = qd.thePort->txSize;
    Style sface = qd.thePort->txFace;
    SetResLoad(false);
    for (i = 1; i <= total && n < max; i++) {
        Handle h = GetIndResource('FOND', i);
        short id; ResType t; Str255 p;
        char name[FONT_NAME_MAX];
        PackFam *ours;
        short k;
        Boolean packed = false;
        if (!h) continue;
        GetResInfo(h, &id, &t, p);
        if (p[0] == 0 || p[1] == '.' || p[0] >= FONT_NAME_MAX) continue;
        memcpy(name, p + 1, p[0]);
        name[p[0]] = 0;
        if (same_name(name, "Symbol") || same_name(name, "Zapf Dingbats") || same_name(name, "Dingbats")) continue;
        ours = pack_by_id(id);
        for (k = 0; k < npack_files; k++) if (HomeResFile(h) == pack_files[k].ref) packed = true;
        if (packed && !(ours && ours->ref == HomeResFile(h))) continue;     /* a suitcase's family that failed its check */
        if (!packed) ours = NULL;
        for (j = 0; j < n; j++) if (same_name(names[j], name)) break;
        if (j < n) continue;
        if (fixed) {
            Boolean mono = false;
            if (ours) {
                SetResLoad(true);
                mono = pack_fixed(ours);
                SetResLoad(false);
            } else {
                short w;
                SetResLoad(true);
                TextFont(id);
                TextSize(12);
                TextFace(0);
                w = CharWidth('i');
                mono = w > 0 && w == CharWidth('M') && w == CharWidth('W');
                SetResLoad(false);
            }
            if (!mono) continue;
        }
        strcpy(names[n++], name);
    }
    SetResLoad(true);
    TextFont(sf);
    TextSize(ss);
    TextFace(sface);
    for (i = 1; i < n; i++) {      /* sorted, ignoring case */
        char t[FONT_NAME_MAX];
        for (j = i; j > 0; j--) {
            const char *a = names[j - 1], *b = names[j];
            while (*a && (*a | 32) == (*b | 32)) { a++; b++; }
            if ((*a | 32) <= (*b | 32)) break;
            strcpy(t, names[j]); strcpy(names[j], names[j - 1]); strcpy(names[j - 1], t);
        }
    }
    return n;
}

/* The port's font, size, and style, each set only if it isn't already: text is measured
   a word at a time, mostly in the style before */
static void text_style(short font, short size, short face)
{
    GrafPtr port = qd.thePort;
    if (port->txFont != font) TextFont(font);
    if (port->txSize != size) TextSize(size);
    if ((unsigned char)port->txFace != face) TextFace(face);
}

static void use_face(const Face *f, const MStyle *s)
{
    text_style(f->font, f->size, s->face & FACE_QD);
}

/* A run in the font Plug-In's family: its width, or drawn (pen: the baseline's start), and
   the width; -1 if the Plug-In can't (no memory), and QuickDraw measures or draws it */
static long plug_run(const Face *f, const MStyle *s, const char *text, long len, short what, short h, short v)
{
    MillFontParams p;
    memset(&p, 0, sizeof p);
    p.what = what;
    p.font = f->plug - 1;
    p.size = f->size;
    p.face = s->face & (FACE_BOLD | FACE_ITALIC);
    p.text = (const unsigned char *)text;
    p.len = len;
    p.pen.h = h;
    p.pen.v = v;
    p.mode = (s->face & FACE_INVERSE) && !plain_cells ? srcBic : srcOr;
    return plugin_font(&p) == noErr ? p.width : -1;
}

static long qd_width(void *ctx, const MStyle *s, const char *text, long len)
{
    short was, w;
    long pw;
    Boolean ol;
    Face f = face_of(s);
    (void)ctx;
    if (f.plug && (pw = plug_run(&f, s, text, len, millFontWidth, 0, 0)) >= 0) return pw;
    was = font_enter(&f);
    use_face(&f, s);
    ol = outline_enter(&f);
    w = TextWidth((Ptr)text, 0, (short)len);
    outline_leave(ol);
    font_leave(was);
    return w;
}

static void qd_metrics(void *ctx, const MStyle *s, short *a, short *d, short *l)
{
    FontInfo fi;
    Boolean ol;
    short was;
    Face f = face_of(s);
    (void)ctx;
    /* A line is as tall as the font that draws it: the font Plug-In's outlines as it says,
       else the strike's own (a font file's family's, from its file) */
    if (f.plug) {
        *a = f.asc;
        *d = f.desc;
        *l = f.lead;
        return;
    }
    was = font_enter(&f);
    use_face(&f, s);
    ol = outline_enter(&f);
    GetFontInfo(&fi);
    outline_leave(ol);
    font_leave(was);
    *a = fi.ascent;
    *d = fi.descent;
    *l = fi.leading;
}

/* ------------------------------------------------------------ the picture memory */

/* Decoded pictures stay here between pages, up to the picture memory setting, so a
   page shown again needs no fetching or decoding. Keyed like the page's own: the
   address and the WIDTH and HEIGHT asked for. */
typedef struct {
    char *url;
    short aw, ah;
    Bitmap1 bm;
    long used;          /* the page load that last showed it */
    short users;        /* windows whose page shows it now */
} Kept;

static Kept *kept;
static short nkept;
static long kept_bytes, page_number;

static long bitmap_bytes(const Bitmap1 *b) { return (long)b->row_bytes * b->h * (b->mask ? 2 : 1); }

static Kept *kept_find(const char *url, short aw, short ah)
{
    short i;
    for (i = 0; i < nkept; i++) if (kept[i].aw == aw && kept[i].ah == ah && !strcmp(kept[i].url, url)) return &kept[i];
    return NULL;
}

/* Make room for need bytes under limit by letting go of pictures no window is showing,
   least recently shown first; returns whether there's room */
static Boolean kept_trim(long need, long limit)
{
    while (kept_bytes + need > limit) {
        short i, oldest = -1;
        for (i = 0; i < nkept; i++)
            if (!kept[i].users && (oldest < 0 || kept[i].used < kept[oldest].used)) oldest = i;
        if (oldest < 0) return false;
        kept_bytes -= bitmap_bytes(&kept[oldest].bm);
        free(kept[oldest].url);
        image_free(&kept[oldest].bm);
        kept[oldest] = kept[--nkept];
    }
    return true;
}

/* Let go of every kept picture no window is showing; whether any went */
static Boolean kept_release(void)
{
    long was = kept_bytes;
    kept_trim(0, 0);
    return kept_bytes < was;
}

/* Keep a picture; on success the memory owns its bits */
static Boolean kept_add(const char *url, short aw, short ah, const Bitmap1 *bm)
{
    long limit = image_memory_k() * 1024L, size = bitmap_bytes(bm);
    Kept *k;
    if (size > limit || !kept_trim(size, limit)) return false;
    if (!(k = realloc(kept, (nkept + 1) * sizeof(Kept)))) return false;
    kept = k;
    k = &kept[nkept];
    if (!(k->url = malloc(strlen(url) + 1))) return false;
    strcpy(k->url, url);
    k->aw = aw;
    k->ah = ah;
    k->bm = *bm;
    k->used = page_number;
    k->users = 1;
    kept_bytes += size;
    nkept++;
    return true;
}

/* ------------------------------------------------------------ pictures */

static void pics_free(void)
{
    short i;
    for (i = 0; i < br->npics; i++) {
        Pic *p = &br->pics[i];
        Kept *k;
        if (p->owned) image_free(&p->bm);
        else if (p->state == PIC_SHOWN && (k = kept_find(p->url, p->aw, p->ah))) k->users--;
        free(p->url);
    }
    free(br->pics);
    free(br->pic_of);
    br->pics = NULL;
    br->pic_of = NULL;
    br->npics = 0;
}

static short px_attr(const Node *n, const char *name)
{
    const char *v = html_attr(&br->doc, n, name);
    return v && !strchr(v, '%') && atoi(v) > 0 ? (short)atoi(v) : -1;
}

static int scheme_of(const char *url)
{
    Url u;
    return url_parse(url, &u) ? -1 : u.scheme;
}

/* The kind of address: http: and https: are one kind, the web's */
static int kind_of(const char *url)
{
    int s = scheme_of(url);
    return s == SCHEME_HTTPS || s == SCHEME_OTHER ? SCHEME_HTTP : s;
}

/* A Plug-In's own address (plugin://NAME/...): any page may ask for one, as a picture
   too, and the Plug-In decides; but nothing redirects to one */
static Boolean plugin_url(const char *url)
{
    return !strncmp(url, "plugin:", 7) || !strncmp(url, "PLUGIN:", 7);
}

/* The page's pictures, and which IMG shows which; false if memory ran out */
static Boolean pics_collect(void)
{
    long i;
    int kind = kind_of(br->page_url);
    br->pic_of = calloc(br->doc.nnodes, sizeof(short));
    if (!br->pic_of) return false;
    for (i = 0; i < br->doc.nnodes; i++) {
        const Node *n = &br->doc.nodes[i];
        const char *src;
        char url[300];
        short k, aw, ah;
        Pic *p;
        if (n->tag != TAG_IMG || !(src = html_attr(&br->doc, n, "src")) || url_resolve(br->page_url, src, url, sizeof url)) continue;
        /* no pictures from another kind of address (but Mill's own listing icons, for its listings) */
        if (kind_of(url) != kind && !plugin_url(url) && strncmp(url, "mill://icon/", 12)) continue;
        aw = px_attr(n, "width");
        ah = px_attr(n, "height");
        for (k = 0; k < br->npics; k++) if (br->pics[k].aw == aw && br->pics[k].ah == ah && !strcmp(br->pics[k].url, url)) break;
        if (k == br->npics) {
            Kept *keep;
            if (!(p = realloc(br->pics, (br->npics + 1) * sizeof(Pic)))) return false;
            br->pics = p;
            p = &br->pics[br->npics];
            memset(p, 0, sizeof *p);
            if (!(p->url = malloc(strlen(url) + 1))) return false;
            strcpy(p->url, url);
            p->aw = aw;
            p->ah = ah;
            if ((keep = kept_find(url, aw, ah))) {
                /* shown before: no fetching or decoding */
                p->bm = keep->bm;
                p->state = PIC_SHOWN;
                keep->used = page_number;
                keep->users++;
            }
            br->npics++;
        }
        br->pic_of[i] = k + 1;
    }
    return true;
}

static int qd_image_size(void *ctx, long node, short *w, short *h)
{
    const Pic *p;
    (void)ctx;
    if (!br->pic_of || !br->pic_of[node]) return 0;
    p = &br->pics[br->pic_of[node] - 1];
    if (p->state != PIC_SHOWN) return 0;
    *w = p->bm.w;
    *h = p->bm.h;
    return 1;
}

/* ------------------------------------------------------------ form controls */

#define LINE_H CT(11)   /* a line of Geneva 9, which every control uses, at the zoom */
#define CHOOSE "Choose\311"

#define AREA_BAR CD(16)          /* a text area's scroll bar, wide as the Mac's */

static void control_font(void)
{
    TextFont(kFontIDGeneva);
    TextSize(CT(9));
    TextFace(0);
}

/* The control font's ascent, for where a baseline goes */
static short ctl_asc(void)
{
    FontInfo fi;
    GetFontInfo(&fi);
    return fi.ascent;
}

/* A TextEdit record in the control font, for a field being typed in or measured */
static void te_ctl_style(TEHandle te)
{
    FontInfo fi;
    control_font();
    GetFontInfo(&fi);
    (*te)->txFont = kFontIDGeneva;
    (*te)->txSize = CT(9);
    (*te)->lineHeight = fi.ascent + fi.descent + fi.leading;
    (*te)->fontAscent = fi.ascent;
}

static Field *field_of(long node)
{
    long i = br->forms.n ? forms_find(&br->forms, node) : -1;
    return i >= 0 ? &br->forms.fields[i] : NULL;
}

static const char *button_label(const Field *f)
{
    const char *v = html_attr(&br->doc, &br->doc.nodes[f->node], "value");
    if (v && *v) return v;
    if (f->type == FIELD_RESET) return "Reset";
    if (f->type == FIELD_IMAGE && (v = html_attr(&br->doc, &br->doc.nodes[f->node], "alt")) && *v) return v;
    return "Submit";
}

/* A SELECT shown as a list (MULTIPLE or SIZE above 1): how many rows; 0 for a pop-up menu */
static short list_rows(const Field *f)
{
    if (!f->multiple && f->size <= 1) return 0;
    return f->noptions < 1 ? 1 : f->noptions > 10 ? 10 : f->noptions;
}

static int qd_control_size(void *ctx, long node, short *w, short *h, short *asc)
{
    Field *f = field_of(node);
    short cw, i, most = br->view_r.right - br->view_r.left - CD(24);
    (void)ctx;
    if (!f || f->type == FIELD_HIDDEN) return 0;
    control_font();
    cw = CharWidth('n');
    *h = LINE_H + CD(4);
    *asc = LINE_H;
    switch (f->type) {
    case FIELD_CHECKBOX: case FIELD_RADIO:
        *w = *h = CT(11);
        *asc = CT(10);
        return 1;
    case FIELD_SUBMIT: case FIELD_RESET: case FIELD_IMAGE: {
        const char *l = button_label(f);
        *w = TextWidth((Ptr)l, 0, strlen(l)) + CD(16);
        break;
    }
    case FIELD_SELECT: {
        short widest = CD(20);
        char label[100];
        for (i = 0; i < f->noptions; i++) {
            short lw;
            forms_option_label(&br->doc, f->options[i], label, sizeof label);
            if ((lw = TextWidth(label, 0, strlen(label))) > widest) widest = lw;
        }
        if (list_rows(f)) { *w = widest + CD(8); *h = list_rows(f) * LINE_H + CD(4); *asc = *h - CD(4); }
        else *w = widest + CD(24);
        break;
    }
    case FIELD_TEXTAREA:
        *w = (f->cols > 0 ? f->cols : 20) * cw + CD(8) + AREA_BAR;
        *h = (f->rows > 0 ? f->rows : 2) * LINE_H + CD(4);
        *asc = *h - CD(4);
        break;
    default:  /* text, password, file */
        *w = (f->size > 0 ? f->size : 20) * cw + CD(6);
        if (f->type == FIELD_FILE) *w += TextWidth(CHOOSE, 0, strlen(CHOOSE)) + CD(20);
    }
    if (*w > most) *w = most > CD(20) ? most : CD(20);
    return 1;
}

static short zoom_step;         /* page zoom, 0 (normal) to 6: the size ladder's step; every window */
/* The zoom distances and pictures have: the text's, unless only the text is scaled */
static int qd_adds_up(void *ctx, const MStyle *s)
{
    (void)ctx;
    return face_of(s).adds;
}

static Metrics qd_m = { qd_width, qd_metrics, qd_image_size, qd_control_size, NULL, NULL, qd_font_by_name, 0, 0, qd_adds_up };
static short dist_zoom(void) { return qd_m.text_only || qd_m.zoom < 9 ? 9 : qd_m.zoom; }

/* Where a control is in the window right now; false if it isn't laid out */
static Boolean control_box(long node, Rect *box)
{
    long i, k;
    for (i = 0; i < br->lay.nlines; i++) {
        const MLine *ln = &br->lay.lines[i];
        for (k = ln->first; k < ln->first + ln->count; k++) {
            const Run *r = &br->lay.runs[k];
            if (r->kind == RUN_CONTROL && r->text == node) {
                short top = br->view_r.top + (short)(ln->y - br->scroll_y) + ln->ascent - r->font;
                SetRect(box, PAGE_LEFT + r->x, top, PAGE_LEFT + r->x + r->width, top + r->size);
                return true;
            }
        }
    }
    return false;
}

/* The part of a text control that holds text */
static Rect text_part(const Field *f, Rect box)
{
    if (f->type == FIELD_FILE) box.right -= TextWidth(CHOOSE, 0, strlen(CHOOSE)) + CD(20);
    if (f->type == FIELD_TEXTAREA) box.right -= AREA_BAR - 1;
    InsetRect(&box, CD(2), CD(2));
    return box;
}

/* A text area's scroll bar, along its right side */
static Rect area_bar(const Rect *box)
{
    Rect bar;
    SetRect(&bar, box->right - AREA_BAR, box->top, box->right, box->bottom);
    return bar;
}

/* How tall a text area's text is, laid out in inner, in pixels */
static long area_height(const Field *f, const Rect *inner)
{
    TEHandle te;
    long h;
    if (f - br->forms.fields == br->focus) return (long)(*br->field_te)->nLines * (*br->field_te)->lineHeight;
    if (!(te = TENew(inner, inner))) return 0;
    te_ctl_style(te);
    TESetText(f->value, strlen(f->value), te);
    TECalText(te);
    h = (long)(*te)->nLines * (*te)->lineHeight;
    TEDispose(te);
    return h;
}

/* The scroll bar, for text scrolled up by top of height in a view of view pixels:
   arrows at the ends, a gray track and its thumb between, all blank if it all fits */
static void draw_area_bar(const Rect *box, long top, long height, long view)
{
    Rect bar = area_bar(box), up, down, track, thumb;
    short i, n = CD(4), x = bar.left + AREA_BAR / 2;
    EraseRect(&bar);
    FrameRect(&bar);
    if (height <= view) return;
    if (top > height - view) top = height - view;
    if (top < 0) top = 0;
    SetRect(&up, bar.left, bar.top, bar.right, bar.top + AREA_BAR);
    SetRect(&down, bar.left, bar.bottom - AREA_BAR, bar.right, bar.bottom);
    FrameRect(&up);
    FrameRect(&down);
    for (i = 0; i < n; i++) {
        MoveTo(x - i, up.top + CD(4) + i); LineTo(x + i, up.top + CD(4) + i);
        MoveTo(x - i, down.bottom - CD(5) - i); LineTo(x + i, down.bottom - CD(5) - i);
    }
    SetRect(&track, bar.left + 1, up.bottom, bar.right - 1, down.top);
    if (track.bottom - track.top < AREA_BAR) return;
    FillRect(&track, &qd.gray);
    thumb = track;
    thumb.top += (short)((track.bottom - track.top - AREA_BAR) * top / (height - view));
    thumb.bottom = thumb.top + AREA_BAR;
    InsetRect(&thumb, -1, 0);
    EraseRect(&thumb);
    FrameRect(&thumb);
}

static void draw_button(const Rect *b, const char *label)
{
    EraseRoundRect(b, CD(8), CD(8));
    FrameRoundRect(b, CD(8), CD(8));
    MoveTo(b->left + (b->right - b->left - TextWidth((Ptr)label, 0, strlen(label))) / 2, b->bottom - CD(4));
    DrawText((Ptr)label, 0, strlen(label));
}

/* Put the typing area over a field, keeping any sideways scroll it has */
static void place_te(const Rect *inner, Boolean wrap)
{
    Rect old = (*br->field_te)->viewRect;
    (*br->field_te)->viewRect = *inner;
    OffsetRect(&(*br->field_te)->destRect, inner->left - old.left, inner->top - old.top);
    if (wrap) (*br->field_te)->destRect.right = (*br->field_te)->destRect.left + (inner->right - inner->left);
}

static void draw_text_box(const Field *f, const Rect *box, Boolean focused)
{
    Rect inner = text_part(f, *box), frame = inner;
    RgnHandle save;
    InsetRect(&frame, -CD(2), -CD(2));
    EraseRect(&frame);
    FrameRect(&frame);
    save = clip_to(&inner);
    if (focused && f->type != FIELD_PASSWORD) {
        place_te(&inner, f->type == FIELD_TEXTAREA);
        TEUpdate(&inner, br->field_te);
        br->focus_shown = true;
        SectRect(&inner, &br->view_r, &br->focus_clip);
    } else if (f->type == FIELD_TEXTAREA) {
        Rect from = inner;
        from.top -= (short)f->scroll;   /* the lines scrolled past are drawn above, and clipped */
        TETextBox(f->value, strlen(f->value), &from, teFlushLeft);
    } else {
        const char *v = f->value ? f->value : "";
        long n = strlen(v);
        MoveTo(inner.left, inner.top + ctl_asc());
        if (f->type == FIELD_PASSWORD) {
            long i;
            for (i = 0; i < n; i++) DrawChar((char)0xA5);  /* a bullet for each letter */
            if (focused) { Line(0, -ctl_asc()); Move(0, ctl_asc()); }      /* where typing goes */
        } else {
            DrawText((Ptr)v, 0, n);
        }
    }
    unclip(save);
    if (f->type == FIELD_TEXTAREA) {
        long top = focused ? (*br->field_te)->viewRect.top - (*br->field_te)->destRect.top : f->scroll;
        save = clip_to(box);
        draw_area_bar(box, top, area_height(f, &inner), inner.bottom - inner.top);
        unclip(save);
    }
}

static void draw_popup(const Field *f, const Rect *box)
{
    char label[100] = "";
    short i, n = CD(4), x = box->right - CD(12);
    Rect in = *box;
    for (i = 0; i < f->noptions; i++) if (f->selected[i]) { forms_option_label(&br->doc, f->options[i], label, sizeof label); break; }
    EraseRect(box);
    FrameRect(box);
    in.right -= CD(14);
    {
        RgnHandle save = clip_to(&in);
        MoveTo(box->left + CD(4), box->bottom - CD(4));
        DrawText(label, 0, strlen(label));
        unclip(save);
    }
    for (i = 0; i < n; i++) {             /* the down arrow */
        MoveTo(x - (n - 1 - i), box->top + CD(6) + i);
        LineTo(x + (n - 1 - i), box->top + CD(6) + i);
    }
}

static void draw_list(const Field *f, const Rect *box)
{
    short i, rows = list_rows(f);
    char label[100];
    EraseRect(box);
    FrameRect(box);
    for (i = 0; i < rows && i < f->noptions; i++) {
        Rect row;
        SetRect(&row, box->left + 1, box->top + CD(2) + i * LINE_H, box->right - 1, box->top + CD(2) + (i + 1) * LINE_H);
        forms_option_label(&br->doc, f->options[i], label, sizeof label);
        MoveTo(row.left + CD(3), row.bottom - CD(2));
        DrawText(label, 0, strlen(label));
        if (f->selected[i]) InvertRect(&row);
    }
    if (f->noptions > rows) { MoveTo(box->right - CD(10), box->bottom - CD(3)); DrawText("\311", 0, 1); }
}

static void draw_control(const Run *r, short top)
{
    Field *f = field_of(r->text);
    Rect box;
    if (!f) return;
    SetRect(&box, PAGE_LEFT + r->x, top, PAGE_LEFT + r->x + r->width, top + r->size);
    control_font();
    switch (f->type) {
    case FIELD_CHECKBOX:
        EraseRect(&box);
        FrameRect(&box);
        if (f->checked) {
            MoveTo(box.left, box.top); LineTo(box.right - 1, box.bottom - 1);
            MoveTo(box.right - 1, box.top); LineTo(box.left, box.bottom - 1);
        }
        break;
    case FIELD_RADIO:
        EraseOval(&box);
        FrameOval(&box);
        if (f->checked) { Rect dot = box; InsetRect(&dot, CD(3), CD(3)); PaintOval(&dot); }
        break;
    case FIELD_SUBMIT: case FIELD_RESET: case FIELD_IMAGE:
        draw_button(&box, button_label(f));
        break;
    case FIELD_SELECT:
        if (list_rows(f)) draw_list(f, &box); else draw_popup(f, &box);
        break;
    case FIELD_FILE: {
        Rect button = box;
        button.left = text_part(f, box).right + CD(6);
        draw_text_box(f, &box, false);
        draw_button(&button, CHOOSE);
        break;
    }
    default:
        draw_text_box(f, &box, f - br->forms.fields == br->focus);
    }
    if (f->disabled) {          /* dimmed, as the Control Manager dims a control */
        PenPat(&qd.gray);
        PenMode(patBic);
        PaintRect(&box);
        PenNormal();
    }
}

/* ------------------------------------------------------------ the page view */

/* Bullets: a disc d wide (d odd, so there is a middle pixel) is drawn from these row widths, made to look round in
   1 bit; larger ones are worked out. A ring is the disc's outline, one pixel thick and joined at the diagonals. */
static const unsigned char DISC_ROWS[7][13] = {
    { 1, 3, 1 },
    { 3, 5, 5, 5, 3 },
    { 3, 5, 7, 7, 7, 5, 3 },
    { 3, 7, 7, 9, 9, 9, 7, 7, 3 },
    { 5, 9, 9, 11, 11, 11, 11, 11, 9, 9, 5 },
    { 5, 9, 11, 11, 13, 13, 13, 13, 13, 11, 11, 9, 5 }
};

static Boolean in_disc(short x, short y, short d)
{
    if (x < 0 || y < 0 || x >= d || y >= d) return false;
    if (d <= 13) {
        short w = DISC_ROWS[(d - 3) / 2][y], from = (d - w) / 2;
        return x >= from && x < from + w;
    } else {
        long dx = 2L * x - (d - 1), dy = 2L * y - (d - 1);
        return dx * dx + dy * dy <= (long)d * d - d;
    }
}

static Boolean in_ring(short x, short y, short d)
{
    short dx, dy;
    if (!in_disc(x, y, d)) return false;
    for (dx = -1; dx <= 1; dx++)
        for (dy = -1; dy <= 1; dy++)
            if ((d >= 5 || !dx || !dy) && !in_disc(x + dx, y + dy, d)) return true;   /* a corner counts too: no gaps in the ring */
    return false;
}

/* A list bullet, drawn here (the font's is oval): a disc, a ring, or a square, r->width wide, its middle on
   the middle of the text's x-height */
static void draw_mark(const Run *r, short base)
{
    short d = r->width, x, y, left = PAGE_LEFT + r->x, top = base - (r->size * 23 + 50) / 100 - d / 2;
    Rect m;
    if (r->face == MARK_SQUARE) {
        SetRect(&m, left, top, left + d, top + d);
        PaintRect(&m);
        return;
    }
    for (y = 0; y < d; y++)
        for (x = 0; x < d; x++) {
            if (r->face == MARK_CIRCLE ? !in_ring(x, y, d) : !in_disc(x, y, d)) continue;
            SetRect(&m, left + x, top + y, left + x + 1, top + y + 1);
            PaintRect(&m);
        }
}

static void draw_picture(const Run *r, short top)
{
    Rect box, in;
    const Pic *p = br->pic_of && br->pic_of[r->text] ? &br->pics[br->pic_of[r->text] - 1] : NULL;
    SetRect(&box, PAGE_LEFT + r->x, top, PAGE_LEFT + r->x + r->width, top + r->size);
    in = box;
    if (r->face) {
        PenSize(r->face, r->face);
        FrameRect(&box);
        PenNormal();
        InsetRect(&in, r->face, r->face);
    }
    if (p && p->state == PIC_SHOWN) {
        BitMap bm;
        bm.rowBytes = p->bm.row_bytes;
        SetRect(&bm.bounds, 0, 0, p->bm.w, p->bm.h);
        if (p->bm.mask) {
            /* transparent parts: clear where the picture covers, then add its black,
               so the page (a dark cell too) shows through the rest */
            bm.baseAddr = (Ptr)p->bm.mask;
            CopyBits(&bm, &qd.thePort->portBits, &bm.bounds, &in, srcBic, NULL);
            bm.baseAddr = (Ptr)p->bm.bits;
            CopyBits(&bm, &qd.thePort->portBits, &bm.bounds, &in, srcOr, NULL);
        } else {
            bm.baseAddr = (Ptr)p->bm.bits;
            CopyBits(&bm, &qd.thePort->portBits, &bm.bounds, &in, srcCopy, NULL);
        }
    } else {
        /* not here (yet): a gray frame, the image icon, and the ALT text if they fit */
        const char *alt = html_attr(&br->doc, &br->doc.nodes[r->text], "alt");
        short text_x = in.left + 3;
        PenPat(&qd.gray);
        FrameRect(&in);
        PenNormal();
        if (in.right - in.left >= 15 && in.bottom - in.top >= 13) {
            Rect icon;
            SetRect(&icon, in.left + 2, in.top + 2, in.left + 13, in.top + 11);
            draw_bits(ICON_IMAGE, 9, 11, &icon, false);
            text_x = icon.right + 3;
        }
        if (alt && *alt && in.bottom - in.top >= 12) {
            Rect inside = in;
            RgnHandle save;
            InsetRect(&inside, 1, 1);
            save = clip_to(&inside);
            control_font();
            MoveTo(text_x, in.top + 10);
            DrawText((Ptr)alt, 0, strlen(alt));
            unclip(save);
        }
    }
}

/* Where pics[which] shows in the view, all its places together; false if nowhere */
static Boolean pic_area(short which, Rect *area)
{
    long i, k, top = br->scroll_y, bottom = br->scroll_y + (br->view_r.bottom - br->view_r.top);
    Boolean any = false;
    SetRect(area, 0, 0, 0, 0);
    for (i = layout_line_at(&br->lay, top); i < br->lay.nlines && br->lay.lines[i].y < bottom; i++) {
        const MLine *ln = &br->lay.lines[i];
        short base = br->view_r.top + (short)(ln->y - br->scroll_y) + ln->ascent;
        for (k = ln->first; k < ln->first + ln->count; k++) {
            const Run *r = &br->lay.runs[k];
            Rect box;
            if (r->kind != RUN_IMAGE || !br->pic_of || br->pic_of[r->text] != which + 1) continue;
            SetRect(&box, PAGE_LEFT + r->x, base - r->font, PAGE_LEFT + r->x + r->width, base - r->font + r->size);
            if (any) UnionRect(area, &box, area);
            else *area = box;
            any = true;
        }
    }
    return any;
}

/* How wide a text run's first n characters are */
static short run_width(const Run *r, long n)
{
    MStyle s;
    s.font = r->font; s.size = r->size; s.face = r->face;
    return (short)qd_width(NULL, &s, br->doc.text + r->text, n);
}

/* Invert the page's text between Doc.text offsets a and b where it shows */
static void sel_invert(long a, long b)
{
    long i, k, s, e;
    if (a > b) { s = a; a = b; b = s; }
    for (i = layout_line_at(&br->lay, br->scroll_y); i < br->lay.nlines && br->lay.lines[i].y < br->scroll_y + br->view_r.bottom; i++) {
        const MLine *ln = &br->lay.lines[i];
        Rect rc;
        rc.top = br->view_r.top + (short)(ln->y - br->scroll_y);
        rc.bottom = rc.top + ln->height;
        for (k = ln->first; k < ln->first + ln->count; k++) {
            const Run *r = &br->lay.runs[k];
            s = a > r->text ? a : r->text;
            e = b < r->text + r->len ? b : r->text + r->len;
            if (r->kind != RUN_TEXT || s >= e) continue;
            rc.left = PAGE_LEFT + r->x + run_width(r, s - r->text);
            rc.right = PAGE_LEFT + r->x + run_width(r, e - r->text);
            InvertRect(&rc);
        }
    }
}

/* Draw the page's visible part; with only >= 0, just the runs showing that picture */
static void draw_page(short only)
{
    long i, top = br->scroll_y, bottom = br->scroll_y + (br->view_r.bottom - br->view_r.top);
    RgnHandle save;
    Rect area = br->view_r, under;
    Boolean part = page_limited;
    if (page_limited) SectRect(&br->view_r, &page_limit, &area);
    if (only >= 0 && br->have_page && br->pics[only].bm.mask) {
        /* a picture the page shows through: the page under it is drawn again first,
           over what stood in for it while it loaded */
        if (!pic_area(only, &under) || !SectRect(&area, &under, &area)) return;
        only = -1;
        part = true;
    }
    save = keep_clip();
    ClipRect(&area);
    if (only < 0) { EraseRect(&area); if (!part) br->focus_shown = false; }
    if (br->have_page) {
        /* fills first, last to first so outer cells go under inner ones; then frames */
        for (i = br->lay.nboxes - 1; i >= 0 && only < 0; i--) {
            const Box *b = &br->lay.boxes[i];
            Rect r;
            if (b->thick > 0 || plain_cells || b->y + b->h <= top || b->y >= bottom) continue;
            SetRect(&r, PAGE_LEFT + b->x, br->view_r.top + (short)(b->y - br->scroll_y),
                    PAGE_LEFT + b->x + b->w, br->view_r.top + (short)(b->y - br->scroll_y) + b->h);
            if (b->thick == 0) PaintRect(&r);
            else EraseRect(&r);
        }
        for (i = 0; i < br->lay.nboxes && only < 0; i++) {
            const Box *b = &br->lay.boxes[i];
            Rect r;
            if (b->thick <= 0 || b->y + b->h <= top || b->y >= bottom) continue;
            SetRect(&r, PAGE_LEFT + b->x, br->view_r.top + (short)(b->y - br->scroll_y),
                    PAGE_LEFT + b->x + b->w, br->view_r.top + (short)(b->y - br->scroll_y) + b->h);
            PenSize(b->thick, b->thick);
            FrameRect(&r);
            PenNormal();
        }
        for (i = layout_line_at(&br->lay, top); i < br->lay.nlines && br->lay.lines[i].y < bottom; i++) {
            const MLine *ln = &br->lay.lines[i];
            long k;
            short base = br->view_r.top + (short)(ln->y - br->scroll_y) + ln->ascent;
            if (ln->y + ln->height <= top) continue;
            for (k = ln->first; k < ln->first + ln->count; k++) {
                const Run *r = &br->lay.runs[k];
                if (only >= 0) {
                    if (r->kind == RUN_IMAGE && br->pic_of && br->pic_of[r->text] == only + 1) draw_picture(r, base - r->font);
                    continue;
                }
                if (r->kind == RUN_RULE) {
                    short y = br->view_r.top + (short)(ln->y - br->scroll_y) + ln->height / 2, t = r->size < 1 ? 1 : r->size;
                    Rect bar;
                    SetRect(&bar, PAGE_LEFT + r->x, y - t / 2, PAGE_LEFT + r->x + r->width, y - t / 2 + t);
                    if (t == 1) { MoveTo(bar.left, y); LineTo(bar.right - 1, y); }
                    else if (r->face) PaintRect(&bar);          /* NOSHADE: a solid bar */
                    else FrameRect(&bar);                       /* else outlined, the 1-bit engraving */
                    continue;
                }
                if (r->kind == RUN_IMAGE) { draw_picture(r, base - r->font); continue; }
                if (r->kind == RUN_CONTROL) { draw_control(r, base - r->font); continue; }
                if (r->kind == RUN_MARK) { draw_mark(r, base); continue; }
                {
                    MStyle rs;
                    Face f;
                    short was;
                    long pw;
                    Boolean ol;
                    rs.font = r->font; rs.size = r->size; rs.face = r->face;
                    f = face_of(&rs);
                    use_face(&f, &rs);
                    short up = layout_shift(r->size, r->face);
                    if ((r->face & FACE_INVERSE) && !plain_cells) TextMode(srcBic);  /* white, on a dark cell */
                    MoveTo(PAGE_LEFT + r->x, base - up);
                    if (f.plug && (pw = plug_run(&f, &rs, r->kind == RUN_GEN ? br->lay.gen + r->text : br->doc.text + r->text,
                                                 r->len, millFontDraw, PAGE_LEFT + r->x, base - up)) >= 0) {
                        if (r->face & FACE_UNDERLINE) {         /* QuickDraw's place for it, a pixel under the baseline */
                            if ((r->face & FACE_INVERSE) && !plain_cells) PenMode(patBic);
                            MoveTo(PAGE_LEFT + r->x, base - up + 1);
                            LineTo(PAGE_LEFT + r->x + (short)pw - 1, base - up + 1);
                            PenNormal();
                        }
                    } else {
                        was = font_enter(&f);
                        ol = outline_enter(&f);
                        DrawText(r->kind == RUN_GEN ? br->lay.gen + r->text : br->doc.text + r->text, 0, r->len);
                        outline_leave(ol);
                        font_leave(was);
                    }
                    if (r->face & FACE_STRIKE) {
                        short sy = base - up - (r->size + 2) / 3;
                        if ((r->face & FACE_INVERSE) && !plain_cells) PenMode(patBic);
                        MoveTo(PAGE_LEFT + r->x, sy);
                        LineTo(PAGE_LEFT + r->x + r->width - 1, sy);
                        PenNormal();
                    }
                }
                TextMode(srcOr);
            }
        }
        if (only < 0 && br->focus < -1) sel_invert(br->sel_a, br->sel_b);
    }
    unclip(save);
    draw_status();  /* the message stays over the page */
}

static void draw_view(void) { draw_page(-1); }

static long max_scroll(void)
{
    long m = br->have_page ? br->lay.height - (br->view_r.bottom - br->view_r.top) : 0;
    return m > 0 ? m : 0;
}

static void update_scroll(void)
{
    long m = max_scroll();
    br->scroll_scale = (short)(m / 30000 + 1);
    SetControlMaximum(br->vscroll, (short)(m / br->scroll_scale));
    SetControlValue(br->vscroll, (short)(br->scroll_y / br->scroll_scale));
}

static void scroll_to(long y)
{
    long m = max_scroll(), dy;
    if (y > m) y = m;
    if (y < 0) y = 0;
    dy = br->scroll_y - y;
    if (!dy) return;
    br->scroll_y = y;
    SetControlValue(br->vscroll, (short)(br->scroll_y / br->scroll_scale));
    draw_view();
}

static long max_scroll_x(void)
{
    long m = br->hbar || NO_BARS ? br->page_w - (br->view_r.right - br->view_r.left) : 0;
    return m > 0 ? m : 0;
}

/* The horizontal scroll bar runs along the bottom, beside the grow box */
static void place_hbar(void)
{
    Rect r = br->win->portRect;
    MoveControl(br->hscroll, -1, r.bottom - SCROLL + 1);
    SizeControl(br->hscroll, r.right - SCROLL + 3, SCROLL);
}

/* The vertical one down the right, below the toolbar and above the grow box; in full
   screen from the top, and to the bottom when there's no horizontal one */
static void place_vbar(void)
{
    Rect r = br->win->portRect;
    short top = br == full ? -1 : BAR, bottom = br == full && !br->hbar ? r.bottom + 1 : r.bottom - SCROLL + 2;
    MoveControl(br->vscroll, r.right - SCROLL + 1, top);
    SizeControl(br->vscroll, SCROLL, bottom - top);
}

/* Show the horizontal scroll bar when the page is wider than the view, else give its
   room to the page */
static void fit_hbar(void)
{
    Boolean need = br->have_page && !NO_BARS && br->page_w > br->view_r.right - br->view_r.left;
    if (need != br->hbar) {
        br->hbar = need;
        geometry();
        place_hbar();
        if (br == full) place_vbar();
        if (br->hbar) ShowControl(br->hscroll);
        else HideControl(br->hscroll);
        if (br->scroll_y > max_scroll()) br->scroll_y = max_scroll();
        update_scroll();
    }
    if (br->scroll_x > max_scroll_x()) br->scroll_x = max_scroll_x();
    SetControlMaximum(br->hscroll, (short)max_scroll_x());
    SetControlValue(br->hscroll, (short)br->scroll_x);
}

static void scroll_x_to(long x)
{
    long m = max_scroll_x();
    if (x > m) x = m;
    if (x < 0) x = 0;
    if (x == br->scroll_x) return;
    br->scroll_x = x;
    SetControlValue(br->hscroll, (short)br->scroll_x);
    draw_view();
}

/* While a page loads, Stop (or running out of memory) stops laying it out too; once
   loading has stopped, what's there is laid out whole */
static Boolean stop_asked(void);
static Boolean stopped;     /* loading was stopped (see below) */
static Boolean layout_stops;

static int laying_out(void *ctx, int percent)
{
    char msg[32];
    (void)ctx;
    sprintf(msg, "Laying out\311 %d%%", percent);
    set_status(msg);
    return layout_stops && stop_asked();
}

static Boolean make_room(void);

/* Lay the page out again, for the window's width. Returns 0, -1 when memory ran out, or
   -2 when stopped; either way the page shows as far as it got. */
static int relayout(void)
{
    long i;
    int err;
    if (!br->have_page) return 0;
    layout_free(&br->lay);
    set_status("Laying out\311");
    br->have_page = false;   /* nothing draws the layout while it's being made */
    layout_stops = br->loading && !stopped;
    qd_m.progress = laying_out;
    err = layout_doc(&br->lay, &br->doc, &qd_m, br->view_r.right - br->view_r.left);
    while (err == -1 && make_room()) {
        /* room made: try again */
        layout_free(&br->lay);
        err = layout_doc(&br->lay, &br->doc, &qd_m, br->view_r.right - br->view_r.left);
    }
    qd_m.progress = NULL;
    br->have_page = true;
    /* the page's width: its widest run or frame, and the margin to its right */
    br->page_w = 0;
    for (i = 0; i < br->lay.nruns; i++) if (br->lay.runs[i].x + br->lay.runs[i].width > br->page_w) br->page_w = br->lay.runs[i].x + br->lay.runs[i].width;
    for (i = 0; i < br->lay.nboxes; i++) if (br->lay.boxes[i].x + br->lay.boxes[i].w > br->page_w) br->page_w = br->lay.boxes[i].x + br->lay.boxes[i].w;
    br->page_w += dist_zoom() * 8 / 9;
    SetPort(br->win);
    fit_hbar();
    return err;
}

/* Just the grow box: DrawGrowIcon also outlines a horizontal scroll bar that's often
   not there */
static void draw_grow(void)
{
    Rect g = br->win->portRect;
    RgnHandle save;
    if (br == full) return;
    save = keep_clip();
    g.left = g.right - SCROLL + 1;
    g.top = g.bottom - SCROLL + 1;
    ClipRect(&g);
    DrawGrowIcon(br->win);
    unclip(save);
}

/* The whole window, now rather than at the next update event */
static void redraw(void)
{
    SetPort(br->win);
    EraseRect(&br->win->portRect);
    draw_toolbar();
    draw_view();
    DrawControls(br->win);
    draw_grow();
    draw_status();
    ValidRect(&br->win->portRect);
}

/* ------------------------------------------------------------ windows */

static Browser *browser_of(WindowPtr w)
{
    Browser *x;
    for (x = browsers; x && x->win != w; x = x->next) {}
    return x;
}

static WindowPtr print_win;     /* how printing is going, while it is */
static void draw_print_win(void);
static Browser *new_browser(WindowPtr behind);
static void close_browser(Browser *x);
static Browser *need(void);

/* Work on window x from here on */
static void use(Browser *x)
{
    br = x;
    if (x) SetPort(x->win);
}

/* The front window, if it's Mill's (a desk accessory's may be in front) */
static Browser *front(void)
{
    return browser_of(FrontWindow());
}

/* Draw a window the Window Manager says needs it, whichever it is, and come back to
   the one being worked on */
static void update_window(WindowPtr w)
{
    Browser *was = br, *x = browser_of(w);
    GrafPtr old;
    GetPort(&old);
    BeginUpdate(w);
    if (x) { use(x); redraw(); }
    else if (w == print_win) draw_print_win();
    EndUpdate(w);
    br = was;
    SetPort(old);
}

/* A window coming to the front, or going behind another: its scroll bars, its typing,
   and its grow box show which one is active */
static void activate(WindowPtr w, Boolean on)
{
    Browser *was = br, *x = browser_of(w);
    TEHandle te;
    if (!x) return;
    use(x);
    te = x->focus >= 0 && x->forms.fields[x->focus].type != FIELD_PASSWORD ? x->field_te : x->url_te;
    if (x->focus > -2) { if (on) TEActivate(te); else TEDeactivate(te); }
    HiliteControl(x->vscroll, on ? 0 : 255);
    HiliteControl(x->hscroll, on ? 0 : 255);
    draw_grow();
    use(was ? was : x);
}

/* ------------------------------------------------------------ typing in fields */

/* Keep what's been typed in the field being typed in */
static void keep_typing(void)
{
    Field *f;
    if (br->focus < 0 || (f = &br->forms.fields[br->focus])->type == FIELD_PASSWORD) return;
    {
        CharsHandle h = TEGetText(br->field_te);
        long n = (*br->field_te)->teLength;
        char *v = malloc(n + 1);
        if (v) {
            memcpy(v, *h, n);
            v[n] = 0;
            forms_set_value(f, v);
            free(v);
        }
        f->scroll = (*br->field_te)->viewRect.top - (*br->field_te)->destRect.top;
    }
}

/* Keep what's been typed, and give the keyboard back to the address */
static void blur(void)
{
    if (br->focus >= 0) {
        keep_typing();
        TEDeactivate(br->field_te);
        br->focus = -1;
        br->focus_shown = false;
    }
    if (br->focus < -1) {
        br->focus = -1;
        if (br->sel_a != br->sel_b) { br->sel_a = br->sel_b; draw_view(); }
    }
    TEActivate(br->url_te);
}

/* Redraw the scroll bar of the text area being typed in */
static void area_bar_update(const Field *f)
{
    Rect box, inner;
    RgnHandle save;
    if (!control_box(f->node, &box)) return;
    inner = text_part(f, box);
    save = clip_to(&box);
    control_font();
    draw_area_bar(&box, (*br->field_te)->viewRect.top - (*br->field_te)->destRect.top, area_height(f, &inner),
                  inner.bottom - inner.top);
    unclip(save);
}

/* TextEdit only scrolls up and down, and only to follow typing: a one-line field
   (or the address bar) scrolls sideways here instead */

/* How wide te's text is up to offset n, in its own font */
static short te_width(TEHandle te, short n)
{
    short font = br->win->txFont, size = br->win->txSize, w;
    TextFont((*te)->txFont);
    TextSize((*te)->txSize);
    HLock((*te)->hText);
    w = TextWidth(*(*te)->hText, 0, n);
    HUnlock((*te)->hText);
    TextFont(font);
    TextSize(size);
    return w;
}

/* Scroll a one-line te sideways so the insertion point shows, and no further left
   than its text needs */
static void te_show_caret(TEHandle te)
{
    Rect view = (*te)->viewRect;
    short left = (*te)->destRect.left, room = view.right - view.left - 2;
    short x = te_width(te, (*te)->selEnd), end = te_width(te, (*te)->teLength);
    short to = view.left - left;              /* how far it's scrolled now */
    if (x - to > room) to = x - room;
    if (x < to) to = x;
    if (to > end - room) to = end - room;     /* no empty space after the text */
    if (to < 0) to = 0;
    if (to != view.left - left) TEScroll(view.left - left - to, 0, te);
}

/* While a selection is dragged past a field's edge, scroll toward the mouse: called by
   TextEdit (from glue below) for as long as the button is down */
static TEHandle dragging_te;
static unsigned long drag_tick;
__attribute__((used)) void drag_scroll(void)
{
    TEHandle te = dragging_te;
    Rect view;
    Point m;
    if (!te || TickCount() - drag_tick < 2) return;
    drag_tick = TickCount();
    view = (*te)->viewRect;
    GetMouse(&m);
    if ((*te)->destRect.right - (*te)->destRect.left > view.right - view.left) {
        /* one line: sideways, a few pixels at a time */
        short scrolled = view.left - (*te)->destRect.left;
        short rest = te_width(te, (*te)->teLength) - scrolled - (view.right - view.left - 2);
        if (m.h > view.right && rest > 0) TEScroll(-(rest < 6 ? rest : 6), 0, te);
        else if (m.h < view.left && scrolled > 0) TEScroll(scrolled < 6 ? scrolled : 6, 0, te);
    } else {
        /* a text area: a line at a time */
        short line = (*te)->lineHeight;
        long top = view.top - (*te)->destRect.top;
        long below = (long)(*te)->nLines * line - top - (view.bottom - view.top);
        if (m.v > view.bottom && below > 0) TEScroll(0, (short)-(below < line ? below : line), te);
        else if (m.v < view.top && top > 0) TEScroll(0, (short)(top < line ? top : line), te);
    }
}

/* TextEdit calls clikLoop with nothing on the stack, expects D0 nonzero to go on, and
   counts on D1, D2, A0, and A1 surviving */
extern void drag_loop(void);
asm(".text\n"
    "drag_loop:\n"
    "\tmovem.l %d1-%d2/%a0-%a1,-(%sp)\n"
    "\tjsr drag_scroll\n"
    "\tmovem.l (%sp)+,%d1-%d2/%a0-%a1\n"
    "\tmoveq #1,%d0\n"
    "\trts\n");

/* TEClick, scrolling as the mouse leaves te */
static void te_click(Point p, Boolean shift, TEHandle te)
{
    dragging_te = te;
    (*te)->clikLoop = (ProcPtr)drag_loop;
    TEClick(p, shift, te);
    dragging_te = NULL;
}

/* Type in a field: a text field, password field, or text area */
static void focus_field(long i, const Point *click, Boolean shift)
{
    Field *f = &br->forms.fields[i];
    Rect box;
    if (br->focus == i) {
        if (click && f->type != FIELD_PASSWORD) {
            RgnHandle save = clip_to(&br->focus_clip);
            te_click(*click, shift, br->field_te);
            unclip(save);
            if (f->type == FIELD_TEXTAREA) area_bar_update(f);
        }
        return;
    }
    blur();
    TEDeactivate(br->url_te);
    br->focus = i;
    if (!control_box(f->node, &box)) return;
    if (box.top < br->view_r.top || box.bottom > br->view_r.bottom) {
        scroll_to(br->scroll_y + (box.top - br->view_r.top) - 20);
        control_box(f->node, &box);
    }
    if (f->type != FIELD_PASSWORD) {
        Rect inner = text_part(f, box);
        te_ctl_style(br->field_te);
        (*br->field_te)->viewRect = inner;
        (*br->field_te)->destRect = inner;
        if (f->type != FIELD_TEXTAREA) (*br->field_te)->destRect.right = inner.left + 2000;  /* one long line */
        else OffsetRect(&(*br->field_te)->destRect, 0, (short)-f->scroll);
        TEAutoView(false, br->field_te);       /* (starting where it was scrolled to) */
        TESetText(f->value, strlen(f->value), br->field_te);
        TECalText(br->field_te);
        TEActivate(br->field_te);
        if (click) te_click(*click, shift, br->field_te);
        else TESetSelect(0, 32767, br->field_te);
        TEAutoView(f->type == FIELD_TEXTAREA, br->field_te);   /* then typing keeps in view */
    }
    draw_view();
}

/* Tab: the next field to type in, or the previous one with Shift */
static void next_field(int step)
{
    long i, n = br->forms.n;
    for (i = 1; i <= n; i++) {
        long k = ((br->focus < 0 ? (step > 0 ? -1 : 0) : br->focus) + i * step + n * 2) % n;
        char t = br->forms.fields[k].type;
        if ((t == FIELD_TEXT || t == FIELD_PASSWORD || t == FIELD_TEXTAREA) && !br->forms.fields[k].disabled) { focus_field(k, NULL, false); return; }
    }
}

static void type_in_field(char c, EventRecord *ev)
{
    Field *f = &br->forms.fields[br->focus];
    RgnHandle save;
    if (f->type == FIELD_PASSWORD) {
        long n = strlen(f->value);
        char *v = malloc(n + 2);
        if (!v) return;
        strcpy(v, f->value);
        if (c == 8) { if (n) v[n - 1] = 0; }
        else if ((unsigned char)c >= 32 && c != 0x7F && (!f->maxlength || n < f->maxlength)) { v[n] = c; v[n + 1] = 0; }
        forms_set_value(f, v);
        free(v);
        draw_view();
        return;
    }
    if ((unsigned char)c >= 32 && c != 0x7F && f->maxlength && (*br->field_te)->teLength >= f->maxlength &&
        (*br->field_te)->selStart == (*br->field_te)->selEnd) return;
    save = clip_to(&br->focus_clip);
    TEKey(c, br->field_te);
    if (f->type != FIELD_TEXTAREA) te_show_caret(br->field_te);
    unclip(save);
    if (f->type == FIELD_TEXTAREA) area_bar_update(f);
    (void)ev;
}

/* ------------------------------------------------------------ loading */

typedef struct {
    char *buf;
    long size;          /* how much buf holds: MAX_PAGE, unless memory was short */
    long len;           /* bytes received; only the first size are kept in buf */
    long total;         /* the whole response's size when the server says, else 0 */
    Boolean headed;     /* its headers have been looked at */
    int code;
    long body;          /* where the body starts */
    char mime[64];
    const char *url;    /* what's being fetched */
    const Url *req;     /* the same, parsed: Set-Cookie headers are for its host and path */
    Boolean cut;        /* longer than size: only the start is kept */
    Boolean may_save;   /* a page Mill can't show is saved */
    Boolean save_all;   /* saved whatever it is (Save ... As) */
    Boolean have_spec;  /* where to save it is already chosen */
    FSSpec spec;
    Boolean saving, declined;
    Saver saver;
    char owner[32];     /* the Plug-In that marked it as its own page, or "" */
    Pic *picture;       /* a picture, decoded by a format as it comes (load_images) */
    FmtRun *run;        /* the format decoding the body as it comes */
    long raw;           /* bytes received, headers and all, as the server sent them */
    char encoding[16];  /* Content-Encoding */
    char inner[64];     /* what a page's format gives (gzip's inside; a format's HTML) */
    Boolean failed;     /* the format couldn't read it */
    Boolean closed;     /* the connection closed early: only the start came (EndedEarly) */
    Boolean as_text;    /* View Source: its own bytes, as text, whatever it is */
    /* Archives (Mill 2.1): the format's answer, a member, a listing */
    char path[256];     /* the member path after "#/", decoded ("" lists, or none) */
    Boolean deciding;   /* the format hasn't answered yet: the reply's bytes are kept too */
    Boolean raw_lost;   /* ...but they didn't all fit */
    Boolean pull;       /* Mill reads the source itself (format_pull) once the reply ends */
    Boolean browse;     /* Browse chosen: the page is the listing, at the address with "#/" */
    Boolean after;      /* "Browse after saving" checked */
    Boolean cancelled;  /* Cancel in the archive dialog */
    Boolean range_failed;   /* a part was asked for and the whole came */
    Boolean listing;    /* the page is a listing Mill made */
    Boolean member;     /* the address has "#/": inside an archive */
    Boolean decided;    /* the first bytes were in, and Mill decided what to do */
    Boolean member_saving;  /* a member (not the reply) is being saved */
    Boolean not_found;  /* no such member */
    /* Download resume (Mill 2.1): the part file, "part." and the name (core/resume.h) */
    Boolean part_asked; /* what to do about a part file is decided */
    Boolean resume_later;   /* Resume chosen once this reply had begun: asked for again */
    Boolean resume;     /* going on in the part file, from resume_from of the stream */
    Boolean resume_mb;  /* ...which holds MacBinary's forks, as rmb has them */
    Boolean restart;    /* the reply to the Range wasn't the part: from the start again */
    Boolean restarted;  /* ...and it was */
    long resume_from;
    MacBinary rmb;
    Boolean ranged;     /* a Range is asked for: range_first to range_last (-1: the end) */
    long range_first, range_last;
    PlugFormat *fmt;    /* the reply's format */
    short src_ref;      /* file: the file, open for reading, or 0 */
    long src_size;
    struct Listing *list;
    FmtSink sink;
} Response;

static void response_init(Response *r)
{
    char *buf = r->buf;
    long size = r->size;
    memset(r, 0, sizeof *r);
    r->buf = buf;
    r->size = size;
}

static Boolean leaving;      /* a new page is loading, and the one shown can go */
static const char *loading;  /* the address a page is loading from (load's), NULL if none */

/* The pictures of the page being left go back to waiting, letting go of their bits
   (or their hold on the picture memory's); whether any were showing */
static Boolean pics_unshow(void)
{
    short i;
    Boolean any = false;
    for (i = 0; i < br->npics; i++) {
        Pic *p = &br->pics[i];
        Kept *k;
        if (p->state != PIC_SHOWN) continue;
        if (p->owned) image_free(&p->bm);
        else if ((k = kept_find(p->url, p->aw, p->ah))) k->users--;
        memset(&p->bm, 0, sizeof p->bm);
        p->owned = false;
        p->state = PIC_WAITING;
        any = true;
    }
    return any;
}

/* Memory is short: let go of kept pictures no window shows; then, while a new page
   loads, the pictures of the page being left, and then that page itself (Back loads it
   again). The person asked to go somewhere else, so the new page comes first. True if
   anything went, so it's worth trying again. */
static Boolean make_room(void)
{
    GrafPtr old;
    if (kept_release()) return true;
    if (plugins_purge(16384)) return true;
    if (net_close_wait(false)) return true;         /* so does each one closing */
    /* each kept connection holds 16K; but not the one the page loading will use, which
       its request would only open again (and log in again) */
    if (plugin_keep_room(br, loading)) return true;
    if (!leaving || !br->have_page) return false;
    GetPort(&old);
    SetPort(br->win);
    if (pics_unshow()) kept_release();
    else {
        layout_free(&br->lay);
        pics_free();
        forms_free(&br->forms);
        html_free(&br->doc);
        free(br->form_base);
        br->form_base = NULL;
        br->have_page = false;
        br->scroll_x = br->scroll_y = 0;
        update_scroll();
    }
    InvalRect(&br->view_r);
    SetPort(old);
    return true;
}

/* Whether memory has run out, after making what room there is */
static Boolean memory_short(void)
{
    while (mem_short()) if (!make_room()) return true;
    return false;
}

/* A buffer to read into: MAX_PAGE, or when memory is short (even after making room)
   half that and so on down to MIN_PAGE, so a page still loads, cut shorter. False if
   there isn't even that. */
static Boolean response_buffer(Response *r)
{
    long size = MAX_PAGE;
    while (!(r->buf = malloc(size))) {
        if (make_room()) continue;
        if (size <= MIN_PAGE) break;
        size /= 2;
    }
    r->size = r->buf ? size : 0;
    return r->buf != NULL;
}

static char activity[40];   /* what's loading, ahead of the network's progress */
static Boolean stopped;
static Boolean ran_out;     /* memory ran out while loading, which stopped it */

/* Out of memory: loading stops, as if the person had pressed Stop, and what's loaded
   so far stays; out_of_memory_alert says so once it's over */
static void out_of_memory(void)
{
    stopped = ran_out = true;
}

/* "Mill ran out of memory, so " and then what happened */
static void out_of_memory_alert(const char *what)
{
    Str255 p;
    p[0] = (unsigned char)strlen(what);
    memcpy(p + 1, what, p[0]);
    ParamText(p, "\p", "\p", "\p");
    set_status("Out of memory.");
    InitCursor();
    StopAlert(ALERT_NO_MEMORY, NULL);
}

static void progress(const char *msg)
{
    char line[128];
    if (activity[0]) { snprintf(line, sizeof line, "%s: %s", activity, msg); set_status(line); }
    else set_status(msg);
}

/* Command-period or Escape (Clear, on the Plus's keypad) stops loading, and so does a
   click on the stop sign, and running out of memory */
static Boolean stop_asked(void)
{
    EventRecord e;
    /* other windows uncovered meanwhile are drawn; only this one loads */
    while (GetNextEvent(updateMask, &e)) update_window((WindowPtr)e.message);
    if (!stopped && memory_short()) out_of_memory();
    if (!stopped && EventAvail(mDownMask, &e)) {
        WindowPtr w;
        if (FindWindow(e.where, &w) == inContent && w == br->win) {
            GrafPtr old;
            Point p = e.where;
            GetPort(&old);
            SetPort(br->win);
            GlobalToLocal(&p);
            if (PtInRect(p, &br->reload_r)) {
                GetNextEvent(mDownMask, &e);
                InvertRect(&br->reload_r);   /* pressed, briefly */
                while (StillDown()) {}
                InvertRect(&br->reload_r);
                stopped = true;
            }
            SetPort(old);
        }
    }
    if (!stopped && EventAvail(keyDownMask, &e)) {
        char c = e.message & charCodeMask;
        if ((c == '.' && (e.modifiers & cmdKey)) || c == 27) {
            GetNextEvent(keyDownMask, &e);
            stopped = true;
        }
    }
    return stopped;
}

static int prefix_ci(const char *s, const char *p, long n);

/* Once the headers are in: the status code, the MIME type, and the whole size */
static void read_head(Response *r)
{
    long i, line = 0, length = -1;
    for (i = 0; i < r->len; i++) {
        long e;
        if (r->buf[i] != '\n') continue;
        e = i > line && r->buf[i - 1] == '\r' ? i - 1 : i;
        if (e == line) {
            r->body = i + 1;
            if (length >= 0) r->total = r->body + length;
            r->headed = true;
            return;
        }
        if (line == 0 && e > 12 && !memcmp(r->buf, "HTTP/", 5)) r->code = atoi(r->buf + 9);
        if (e - line > 11 && prefix_ci(r->buf + line, "Set-Cookie:", 11) && r->req && url_is_web(r->req->scheme) && accept_cookies()) {
            unsigned long now;
            ReadDateTime(&now);  /* the trap: GetDateTime is a low-memory read GCC flags */
            jar_set(&jar, r->req->host, r->req->path, r->req->scheme == SCHEME_HTTPS, r->buf + line + 11, e - line - 11, now);
        }
        if (e - line > 15 && prefix_ci(r->buf + line, "Content-Length:", 15)) length = atol(r->buf + line + 15);
        if (e - line > 17 && prefix_ci(r->buf + line, "Content-Encoding:", 17)) {
            long st = line + 17;
            while (st < e && r->buf[st] == ' ') st++;
            snprintf(r->encoding, sizeof r->encoding, "%.*s", (int)(e - st), r->buf + st);
        }
        if (e - line > 13 && prefix_ci(r->buf + line, "Content-Type:", 13)) {
            long st = line + 13, n;
            while (st < e && r->buf[st] == ' ') st++;
            n = e - st < (long)sizeof r->mime - 1 ? e - st : (long)sizeof r->mime - 1;
            memcpy(r->mime, r->buf + st, n);
            r->mime[n] = 0;
        }
        line = i + 1;
    }
}

static void pstr_of(const char *c, Str255 p);
static const char *request_headers(const Url *u, const char *referer);

static Boolean gzipped(const Response *r) { return r->encoding[0] && prefix_ci(r->encoding, "gzip", 4); }

static Boolean ask_where_named(const char *name, FSSpec *spec)
{
    Str255 p;
    Boolean chosen;
    p[0] = (unsigned char)strlen(name);
    memcpy(p + 1, name, p[0]);
    InitCursor();
    chosen = sys_choose_save("\pSave as:", p, spec);
    watch_cursor();
    return chosen;
}

static Boolean ask_where(const char *url, FSSpec *spec)
{
    char name[64];
    filetype_name(url, name, sizeof name);
    return ask_where_named(name, spec);
}

/* ---- download resume: the part file ---- */

enum { PART_NONE, PART_REPLACE, PART_RESUME, PART_CANCEL };
enum { RA_RESUME = 1, RA_REPLACE, RA_CANCEL };

/* Return and Enter are Resume; Escape and Command-period are Cancel */
static pascal Boolean resume_filter(DialogPtr d, EventRecord *e, short *item)
{
    (void)d;
    if (e->what == keyDown || e->what == autoKey) {
        char c = e->message & charCodeMask;
        if (c == '\r' || c == 3) { *item = RA_RESUME; return true; }
        if (c == 27 || (c == '.' && (e->modifiers & cmdKey))) { *item = RA_CANCEL; return true; }
    }
    return false;
}

/* Saving url as spec: what to do about a part file there. Mill's alert when the
   protocol can seek (Support Random Access doesn't matter here); else it's replaced. */
static short ask_part(const char *url, const FSSpec *spec)
{
    FSSpec part;
    Url u;
    long d, rs;
    short item;
    saver_part_spec(spec, &part);
    switch (resume_ask(file_lengths(&part, &d, &rs) == noErr, !url_parse(url, &u) && plugin_block_size(&u) > 0)) {
    case RESUME_FRESH: return PART_NONE;
    case RESUME_REPLACE: return PART_REPLACE;
    }
    InitCursor();
    item = CautionAlert(ALERT_RESUME, (ModalFilterUPP)resume_filter);
    watch_cursor();
    return item == RA_RESUME ? PART_RESUME : item == RA_REPLACE ? PART_REPLACE : PART_CANCEL;
}

/* Start saving the body to a file instead of keeping it; false if it won't be */
static Boolean start_save(Response *r)
{
    char name[64], type[4], creator[4];
    if (r->raw_lost) { r->declined = true; return false; }     /* the start is gone: never a file without it */
    if (!r->have_spec && !ask_where(r->url, &r->spec)) { r->declined = true; return false; }
    if (!r->part_asked) {
        short part = ask_part(r->url, &r->spec);
        r->part_asked = true;
        if (part == PART_CANCEL) { r->declined = r->cancelled = true; return false; }
        /* this reply is from the start: asked for again, from the part file's end */
        if (part == PART_RESUME) { r->resume_later = r->declined = r->cancelled = true; return false; }
    }
    if (r->resume) {
        /* the part asked for, appended; or the whole, and it starts again */
        if (!resume_appends(r->code)) { r->restart = r->declined = r->cancelled = true; return false; }
        r->saving = true;
        r->deciding = false;
        if (saver_resume(&r->saver, &r->spec, r->resume_mb, &r->rmb, r->resume_from) != noErr) return false;
        return saver_write(&r->saver, r->buf + r->body, r->len - r->body) == noErr;
    }
    {
        /* rules look at the name as the server gave it */
        const char *q = r->url + strcspn(r->url, "?#"), *seg = q;
        while (seg > r->url && seg[-1] != '/') seg--;
        snprintf(name, sizeof name, "%.*s", (int)(q - seg), seg);
    }
    filetype_find(file_type_rules(), r->mime, name, type, creator);
    saver_begin(&r->saver, &r->spec, type, creator, decode_macbinary(), true);
    r->saving = true;
    r->deciding = false;
    return saver_write(&r->saver, r->buf + r->body, r->len - r->body) == noErr;
}

/* Into the buffer: false once it's full (only the start is kept) */
static Boolean keep(Response *r, const char *data, long len)
{
    if (r->len + len > r->size) {
        memcpy(r->buf + r->len, data, r->size - r->len);
        r->len = r->size;
        r->cut = true;
        return false;
    }
    memcpy(r->buf + r->len, data, len);
    r->len += len;
    return true;
}

/* A page's format's bytes (gzip's inside, a format's HTML, a member) are the page */
static Boolean decoded(const char *data, long len, void *ref) { return keep(ref, data, len); }

/* ---- archives: reading a source by parts (Mill 2.1) ---- */

static void source_close(Response *r)
{
    if (r->src_ref) FSClose(r->src_ref);
    r->src_ref = 0;
}

static int hexdigit(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
}

/* file:, read by Mill itself: the file the address names, open; its size, or -1 */
static long source_open_file(Response *r)
{
    Str255 p;
    FSSpec spec;
    const char *q = r->req->path + 1;
    long n = strcspn(q, "?"), i, o = 0;
    if (r->src_ref) return r->src_size;
    /* "/Macintosh%20HD/Web/kit.zip" as "Macintosh HD:Web:kit.zip", as file: has it */
    for (i = 0; i < n && o < 255; i++) {
        int c = (unsigned char)q[i];
        if (c == '/') c = ':';
        else if (c == '%' && i + 2 < n && hexdigit(q[i + 1]) >= 0 && hexdigit(q[i + 2]) >= 0) {
            c = hexdigit(q[i + 1]) * 16 + hexdigit(q[i + 2]);
            i += 2;
        }
        p[++o] = (unsigned char)c;
    }
    p[0] = (unsigned char)o;
    if (!o || file_spec(0, 0, p, &spec) != noErr || file_open(&spec, fsRdPerm, &r->src_ref) != noErr) {
        r->src_ref = 0;
        return -1;
    }
    if (GetEOF(r->src_ref, &r->src_size) != noErr) { source_close(r); return -1; }
    return r->src_size;
}

static long file_read(void *ref, unsigned long at, char *p, long n)
{
    Response *r = ref;
    OSErr err;
    char msg[48];
    if (stop_asked()) return -1;
    if (SetFPos(r->src_ref, fsFromStart, (long)at) != noErr) return -1;
    err = FSRead(r->src_ref, &n, p);
    sprintf(msg, "Reading\311 %ldK of %ldK", done_k((long)at + n, r->src_size), (r->src_size + 1023) / 1024);
    progress(msg);
    return err == noErr || err == eofErr ? n : -1;
}

/* A part by a Range request, through the protocol: 206 brings the part; 200, the whole,
   means it can't (the dialog again: Ranges that fail). Reached only when
   plugin_seekable said yes. */
typedef struct { char *p; long want, got; long line; Boolean headed, part; char head[512]; short nhead; } RangeRead;

static Boolean range_collect(const char *data, long len, void *ref)
{
    RangeRead *g = ref;
    if (stop_asked()) return false;
    while (!g->headed && len > 0) {
        /* the status line and headers, up to the blank line */
        char c = *data++;
        len--;
        if (g->nhead < (short)sizeof g->head - 1) g->head[g->nhead++] = c;
        if (c == '\n') {
            g->head[g->nhead] = 0;
            if (!g->line) g->part = g->nhead > 12 && !memcmp(g->head, "HTTP/", 5) && atoi(g->head + 9) == 206;
            g->line++;
            if (g->nhead <= 2) g->headed = true;    /* the blank line */
            g->nhead = 0;
        }
    }
    if (!g->headed || len <= 0) return true;
    if (!g->part) return false;     /* the whole thing: never read through it */
    if (len > g->want - g->got) len = g->want - g->got;
    memcpy(g->p + g->got, data, len);
    g->got += len;
    return g->got < g->want;
}

static long range_read(void *ref, unsigned long at, char *p, long n)
{
    Response *r = ref;
    RangeRead g;
    Url with = *r->req;
    char headers[1700], bare[300], owner[32], key[AUTH_KEYS];
    OSErr err;
    memset(&g, 0, sizeof g);
    g.p = p;
    g.want = n;
    snprintf(bare, sizeof bare, "%.*s", (int)strcspn(r->url, "#"), r->url);
    snprintf(headers, sizeof headers, "%s%sRange: bytes=%lu-%lu", request_headers(r->req, NULL),
             request_headers(r->req, NULL)[0] ? "\r\n" : "", at, at + n - 1);
    /* the window's page's server: a part borrows its kept connection, if any */
    err = plugin_request(&with, bare, headers, NULL, NULL, range_collect, &g, "", owner, key, br, false);
    if (err != noErr || stopped) return -1;
    if (!g.part) { r->range_failed = true; return -1; }
    return g.got;
}

/* Whether the reply can be read by parts, and its size (-1 if not known) */
static Boolean source_seekable(Response *r, long *size)
{
    long whole, block;
    *size = r->total > 0 ? r->total - r->body : -1;
    if (r->req->scheme == SCHEME_FILE) return (*size = source_open_file(r)) >= 0;
    return plugin_seekable(r->url, r->buf, r->body, &whole, &block);
}

/* Why it can't be browsed where it is */
static const char *why_not(Response *r, const FmtAsk *a)
{
    if (a->reason && a->reason[0]) return a->reason;
    if (a->depth > 0) return "It\325s inside another archive. It may only be browsed after saving.";
    if (plugin_block_size(r->req) > 0 && !plugin_random_access(r->req)) return "Support Random Access is off. It may only be browsed after saving.";
    return "This server sends only whole files. It may only be browsed after saving.";
}

/* ---- the dialog: Cancel, Browse, or Save... ---- */

#define DIALOG_ARCHIVE 206
enum { AD_SAVE = 1, AD_CANCEL, AD_BROWSE, AD_NAME, AD_LINE, AD_REASON, AD_AFTER, AD_RING };

static pascal void draw_ring(DialogPtr d, short item);

/* Centered a third of the way down, as alerts are */
static void center_dialog(DialogPtr d)
{
    MoveWindow(d, (qd.screenBits.bounds.right - (d->portRect.right - d->portRect.left)) / 2,
               20 + (qd.screenBits.bounds.bottom - 20 - (d->portRect.bottom - d->portRect.top)) / 3, false);
}

/* Return and Enter are Save...; Escape and Command-period are Cancel */
static pascal Boolean ad_filter(DialogPtr d, EventRecord *e, short *item)
{
    (void)d;
    if (e->what == keyDown || e->what == autoKey) {
        char c = e->message & charCodeMask;
        if (c == '\r' || c == 3) { *item = AD_SAVE; return true; }
        if (c == 27 || (c == '.' && (e->modifiers & cmdKey))) { *item = AD_CANCEL; return true; }
    }
    return false;
}

/* "2,100K" */
static void kilobytes(long bytes, char *out)
{
    long k = (bytes + 1023) / 1024;
    if (k >= 1000000L) sprintf(out, "%ld,%03ld,%03ldK", k / 1000000L, k / 1000 % 1000, k % 1000);
    else if (k >= 1000) sprintf(out, "%ld,%03ldK", k / 1000, k % 1000);
    else sprintf(out, "%ldK", k);
}

/* Mill's own dialog for an archive, the same on System 6 and 7 (no Standard File hook):
   AD_CANCEL, AD_BROWSE, or AD_SAVE, and whether "Browse after saving" was checked */
static short archive_dialog(const char *name, const PlugFormat *f, long size, const char *reason, Boolean can_browse,
                            Boolean can_after, Boolean *after)
{
    DialogPtr d;
    short item, type;
    Handle h;
    Rect r;
    Str255 p0, p1, p2;
    char line[100], k[24];
    const char *hd = f->heading;
    Boolean an = strchr("AEIOUaeiou", hd[0]) != NULL;
    *after = false;
    if (size >= 0) { kilobytes(size, k); snprintf(line, sizeof line, "This is %s %s archive, %s.", an ? "an" : "a", hd, k); }
    else snprintf(line, sizeof line, "This is %s %s archive.", an ? "an" : "a", hd);
    pstr_of(name, p0);
    pstr_of(line, p1);
    pstr_of(reason ? reason : "", p2);
    ParamText(p0, p1, p2, "\p");
    if (!(d = GetNewDialog(DIALOG_ARCHIVE, NULL, (WindowPtr)-1))) return AD_CANCEL;
    center_dialog(d);
    GetDialogItem(d, AD_RING, &type, &h, &r);
    SetDialogItem(d, AD_RING, type, (Handle)(UserItemUPP)draw_ring, &r);
    GetDialogItem(d, AD_BROWSE, &type, &h, &r);
    if (!can_browse) HiliteControl((ControlHandle)h, 255);
    GetDialogItem(d, AD_AFTER, &type, &h, &r);
    if (!can_after) HiliteControl((ControlHandle)h, 255);
    ShowWindow(d);
    InitCursor();
    do {
        ModalDialog((ModalFilterUPP)ad_filter, &item);
        if (item == AD_AFTER) {
            GetDialogItem(d, AD_AFTER, &type, &h, &r);
            SetControlValue((ControlHandle)h, !GetControlValue((ControlHandle)h));
        }
        if (item == AD_BROWSE && !can_browse) item = 0;
    } while (item != AD_SAVE && item != AD_CANCEL && item != AD_BROWSE);
    GetDialogItem(d, AD_AFTER, &type, &h, &r);
    *after = can_after && GetControlValue((ControlHandle)h);
    DisposeDialog(d);
    SetPort(br->win);
    watch_cursor();
    return item;
}

/* ---- the format's answer ---- */

static Boolean is_listing_path(const char *path)
{
    size_t n = strlen(path);
    return n == 0 || path[n - 1] == '/';
}

/* An address's last name, as the server gave it, in Mac Roman (a file: address's is) */
static void last_name(const char *url, char *out, int size)
{
    char mac[64];
    const char *e = url + strcspn(url, "?#"), *s = e;
    long i, o = 0;
    while (s > url && s[-1] != '/') s--;
    for (i = 0; s + i < e && o < size - 1 && o < 63; i++) {
        int c = (unsigned char)s[i];
        if (c == '%' && s + i + 2 < e && hexdigit(s[i + 1]) >= 0 && hexdigit(s[i + 2]) >= 0) {
            c = hexdigit(s[i + 1]) * 16 + hexdigit(s[i + 2]);
            i += 2;
        }
        if (c >= 32) mac[o++] = (char)c;
    }
    mac[o] = 0;
    charset_name_to_mac(mac, !strncmp(url, "file:", 5) ? CHARSET_MAC : CHARSET_UTF8, out, size);
    if (!out[0]) filetype_name(url, out, size);
}

/* The file name an archive (or a member of one) is shown by */
static void archive_name(const Response *r, const FmtAsk *a, char *out, int size)
{
    char mac[256];
    if (a->member) {
        charset_name_to_mac(a->member->name, a->member->charset, mac, sizeof mac);
        hfs_name(mac, out);
    } else last_name(r->url, out, size);
}

/* The page's bytes start again: what the format gives */
static short begin_page(Response *r, short to)
{
    r->deciding = false;
    r->len = r->body;
    if (to == FMT_PULL) r->pull = true;
    return to;
}

/* Save the bytes a member's format was given (a .tar inside a .tar.gz) */
static short save_inner(Response *r, const FmtAsk *a)
{
    char name[64], type[4], creator[4], t[64];
    archive_name(r, a, name, sizeof name);
    if (!ask_where_named(name, &r->spec)) { r->declined = true; return FMT_STOP; }
    type_for_name(name, t, sizeof t);
    filetype_find(file_type_rules(), t, name, type, creator);
    saver_begin(&r->saver, &r->spec, type, creator, false, false);
    r->member_saving = true;
    return FMT_SAVE;
}

/* sink->ask: what Mill does with a format's answer */
static short reply_ask(void *ref, const FmtAsk *a)
{
    Response *r = ref;
    Boolean members = (a->f->gives & millGivesMembers) != 0, listing = members && is_listing_path(a->path);
    Boolean seekable_member = a->depth == 0 && (a->pull || r->src_ref);    /* Mill reads it by parts */
    short answer = a->answer, item;
    char name[64];
    if (members && answer == millShowNow) answer = millBrowseNow;    /* the same, for an archive */
    if (a->depth == 0) r->fmt = a->f;
    if (r->picture) {
        /* an inline picture: no dialogs, and only what shows */
        if (answer == millShowNow || (answer == millBrowseNow && !listing))
            return a->depth == 0 ? begin_page(r, seekable_member && members ? FMT_PULL : FMT_GO) : FMT_GO;
        return FMT_STOP;
    }
    if (!members) {
        if (answer == millShowNow) return a->depth == 0 ? begin_page(r, FMT_GO) : FMT_GO;
        /* save only (and "Open after saving", which no 2.1 format asks for) */
        if (a->depth > 0) return save_inner(r, a);
        if (!r->may_save) return FMT_STOP;
        start_save(r);
        return FMT_STOP;
    }
    if (answer == millSaveOnly) {
        if (a->depth > 0) return save_inner(r, a);
        if (r->may_save) start_save(r);
        return FMT_STOP;
    }
    if (answer == millBrowseNow && (r->member || a->depth > 0)) {
        /* inside the archive already: no dialog */
        if (listing) r->listing = true;
        return a->depth == 0 ? begin_page(r, seekable_member ? FMT_PULL : FMT_GO) : FMT_GO;
    }
    /* the archive itself, or one that can only be browsed once saved: Mill's dialog */
    archive_name(r, a, name, sizeof name);
    /* (the checkbox is dimmed where saving would decode the MacBinary: nothing to browse) */
    item = archive_dialog(name, a->f, a->size, answer == millBrowseNow ? NULL : why_not(r, a), answer == millBrowseNow,
                          a->depth > 0 || !(decode_macbinary() && strstr(a->f->types, "macbinary")), &r->after);
    if (item == AD_CANCEL) { r->declined = r->cancelled = true; return FMT_STOP; }
    if (item == AD_BROWSE) {
        r->browse = r->listing = true;
        return a->depth == 0 ? begin_page(r, seekable_member ? FMT_PULL : FMT_GO) : FMT_GO;
    }
    if (a->depth > 0) return save_inner(r, a);
    start_save(r);
    return FMT_STOP;
}

/* ---- the listing Mill makes from Entry calls ---- */

#define LIST_MAX 2000

typedef struct {
    char *name;                 /* the part after the folder, as the archive has it */
    short charset, kind, flags;
    OSType ftype;               /* a Mac file's Finder type, or 0 */
    long size;
    unsigned long modified, pos;
} ListItem;

typedef struct Listing {
    char doc[300];              /* the archive's address, without "#/" */
    char path[256];             /* the folder listed, "" or ending in '/' */
    Boolean top;                /* listed by the reply's own format (whose positions they are) */
    ListItem *items;
    short n, room;
} Listing;

/* The last listing shown, so a click hands the member's position back */
static Listing *last_list;

static void list_free(Listing *l)
{
    short i;
    if (!l) return;
    for (i = 0; i < l->n; i++) free(l->items[i].name);
    free(l->items);
    free(l);
}

static Boolean list_has_folder(const Listing *l, const char *name, long n)
{
    short i;
    for (i = 0; i < l->n; i++)
        if (l->items[i].kind == millMemberFolder && (long)strlen(l->items[i].name) == n && !memcmp(l->items[i].name, name, n))
            return true;
    return false;
}

/* sink->entry: a member, kept if it's in the folder listed (the archive's list is flat:
   folders are names' prefixes) */
static Boolean reply_entry(void *ref, const MillEntry *e, const char *folder)
{
    Response *r = ref;
    Listing *l = r->list;
    size_t pn = strlen(folder);
    const char *rest, *slash;
    long n;
    ListItem *it;
    if (!l && !(l = r->list = calloc(1, sizeof *l))) return false;
    l->top = !strcmp(folder, r->path);
    if (strncmp(e->name, folder, pn) || !e->name[pn]) return true;
    rest = e->name + pn;
    slash = strchr(rest, '/');
    n = slash ? slash - rest : (long)strlen(rest);
    if ((slash || e->kind == millMemberFolder) && list_has_folder(l, rest, n)) return true;
    if (l->n >= LIST_MAX) return false;
    if (l->n == l->room) {
        ListItem *more = realloc(l->items, (l->room + 64) * sizeof *more);
        if (!more) return false;
        l->items = more;
        l->room += 64;
    }
    it = &l->items[l->n];
    memset(it, 0, sizeof *it);
    if (!(it->name = malloc(n + 1))) return false;
    memcpy(it->name, rest, n);
    it->name[n] = 0;
    it->charset = e->charset;
    it->kind = slash ? millMemberFolder : e->kind;
    it->size = slash ? -1 : e->size;
    it->modified = slash ? 0 : e->modified;
    it->flags = slash ? 0 : e->flags;
    it->ftype = !slash && (e->flags & millMemberMac) ? e->fileType : 0;
    it->pos = e->pos;
    l->n++;
    return true;
}

/* sink->save_member: a member nothing shows, saved (a Mac file with both its forks) */
static Boolean reply_save_member(void *ref, const MillEntry *e, Boolean *forks)
{
    Response *r = ref;
    char mac[256], name[32], type[4], creator[4], t[64];
    if (r->picture || r->as_text || !r->may_save) return false;
    charset_name_to_mac(e->name, e->charset, mac, sizeof mac);
    hfs_name(mac, name);
    if (!ask_where_named(name, &r->spec)) { r->declined = true; return false; }
    if (e->flags & millMemberMac) {
        MacBinary mb;
        memset(&mb, 0, sizeof mb);
        memcpy(mb.type, &e->fileType, 4);
        memcpy(mb.creator, &e->creator, 4);
        mb.flags = e->finderFlags;
        mb.data_len = e->dataLen > 0 ? e->dataLen : 0;
        mb.rsrc_len = e->rsrcLen > 0 ? e->rsrcLen : 0;
        saver_begin_forks(&r->saver, &r->spec, &mb);
        *forks = true;
    } else {
        type_for_name(mac, t, sizeof t);
        filetype_find(file_type_rules(), t, name, type, creator);
        saver_begin(&r->saver, &r->spec, type, creator, false, false);
    }
    r->member_saving = true;
    return true;
}

static Boolean reply_save(const char *p, long n, void *ref)
{
    Response *r = ref;
    return saver_write(&r->saver, p, n) == noErr;
}

/* < > & " made safe for HTML */
static void html_escape_to(const char *in, char *out, long size)
{
    long o = 0;
    for (; *in; in++) {
        const char *rep = *in == '<' ? "&lt;" : *in == '>' ? "&gt;" : *in == '&' ? "&amp;" : *in == '"' ? "&quot;" : NULL;
        long n = rep ? (long)strlen(rep) : 1;
        if (o + n >= size) break;
        if (rep) memcpy(out + o, rep, n);
        else out[o] = *in;
        o += n;
    }
    out[o] = 0;
}

/* What a listed member is, for its icon: by its Finder type if it tells, else its name */
static void member_icon_type(const ListItem *it, const char *mac, char *out, long size)
{
    if (it->ftype && it->ftype != 'BINA' && it->ftype != '????' && type_for_finder(it->ftype, out, size) &&
        strcmp(out, "application/octet-stream"))
        return;
    type_for_name(mac, out, size);
}

/* The listing as a page, in the buffer after the reply's headers: Mill's own icons, the
   names as links to their "#/" addresses, sizes, and dates */
static void listing_page(Response *r, const char *target)
{
    Listing *l = r->list;
    char doc[300], docesc[400], mac[256], esc[400], enc[400], small[80];
    long o = r->body;
    short i;
    size_t dn = strcspn(target, "#");
    Str255 date;
#define PUT(s) do { const char *s_ = (s); long n_ = strlen(s_); if (o + n_ < r->size) { memcpy(r->buf + o, s_, n_); o += n_; } } while (0)
    snprintf(doc, sizeof doc, "%.*s", (int)dn, target);
    if (!l && !(l = r->list = calloc(1, sizeof *l))) return;
    html_escape_to(doc, docesc, sizeof docesc);
    /* the archive's name, and the folder in it */
    last_name(doc, small, sizeof small);
    charset_name_to_mac(r->path, CHARSET_UTF8, mac, sizeof mac);
    html_escape_to(small, esc, sizeof esc);
    PUT("<TITLE>"); PUT(esc);
    if (mac[0]) { html_escape_to(mac, enc, sizeof enc); PUT("/"); PUT(enc); }
    PUT("</TITLE><H2>"); PUT(esc);
    if (mac[0]) { PUT("/"); PUT(enc); }
    PUT("</H2><P>");
    /* up: the folder that holds this one, or the one that holds the archive */
    sprintf(small, "<IMG SRC=\"mill://icon/%d\" WIDTH=16 HEIGHT=16 ALIGN=TOP> <A HREF=\"", kMillIconParent);
    PUT(small);
    if (r->path[0]) {
        size_t n = strlen(r->path);
        snprintf(mac, sizeof mac, "%s", r->path);
        mac[--n] = 0;
        while (n > 0 && mac[n - 1] != '/') mac[--n] = 0;
        PUT(docesc);
        PUT("#/");
        if (url_member_encode(mac, enc, sizeof enc) >= 0) PUT(enc);
    } else {
        char *slash = strrchr(docesc, '/');
        if (slash) slash[1] = 0;
        PUT(docesc);
        html_escape_to(doc, docesc, sizeof docesc);
    }
    PUT("\">Up</A><BR>\r");
    if (!l->n) PUT(r->failed ? "<P>Mill couldn\325t read this archive." : "<P>Nothing in it.");
    for (i = 0; i < l->n; i++) {
        ListItem *it = &l->items[i];
        short icon = it->kind == millMemberFolder ? kMillIconFolder : it->kind == millMemberLink ? kMillIconLink : kMillIconDocument;
        charset_name_to_mac(it->name, it->charset, mac, sizeof mac);
        if (icon == kMillIconDocument) {
            char t[64];
            member_icon_type(it, mac, t, sizeof t);
            icon = !strncmp(t, "text/", 5) ? kMillIconText : !strncmp(t, "image/", 6) ? kMillIconPicture :
                   strcmp(t, "application/octet-stream") ? kMillIconBinary : kMillIconDocument;
        }
        html_escape_to(mac, esc, sizeof esc);
        sprintf(small, "<IMG SRC=\"mill://icon/%d\" WIDTH=16 HEIGHT=16 ALIGN=TOP> ", icon);
        PUT(small);
        /* its address: the folder's path, then its name */
        snprintf(mac, sizeof mac, "%s%s%s", r->path, it->name, it->kind == millMemberFolder ? "/" : "");
        if (it->kind == millMemberLink || (it->flags & millMemberClosed) || url_member_encode(mac, enc, sizeof enc) < 0) {
            PUT(esc);
        } else {
            PUT("<A HREF=\""); PUT(docesc); PUT("#/"); PUT(enc); PUT("\">"); PUT(esc); PUT("</A>");
        }
        if (it->kind != millMemberFolder && it->size >= 0) { kilobytes(it->size, small + 1); small[0] = ' '; PUT(small); }
        if (it->modified) {
            IUDateString((long)it->modified, shortDate, date);
            sprintf(small, ", %.*s", date[0] < 60 ? date[0] : 60, (char *)date + 1);
            PUT(small);
        }
        PUT("<BR>\r");
    }
#undef PUT
    r->len = o;
    /* kept for the next click: the folder's members and their positions */
    snprintf(l->doc, sizeof l->doc, "%s", doc);
    snprintf(l->path, sizeof l->path, "%s", r->path);
    list_free(last_list);
    last_list = l;
    r->list = NULL;
    strcpy(r->inner, "text/html");
}

/* A member's position from the last listing, if it was on it */
static Boolean list_pos(const char *target, const char *path, unsigned long *pos)
{
    size_t dn = strcspn(target, "#"), pn;
    short i;
    if (!last_list || !last_list->top || strlen(last_list->doc) != dn || strncmp(last_list->doc, target, dn)) return false;
    pn = strlen(last_list->path);
    if (strncmp(path, last_list->path, pn)) return false;
    for (i = 0; i < last_list->n; i++)
        if (last_list->items[i].kind == millMemberFile && !strcmp(last_list->items[i].name, path + pn)) {
            *pos = last_list->items[i].pos;
            return true;
        }
    return false;
}

/* Once the headers and the first bytes are in: a format, if one reads it, is asked what
   to do (millFormatBegin); else a page is kept, or a file saved. False to stop. */
static Boolean start_reply(Response *r)
{
    PlugFormat *f;
    Boolean gz = gzipped(r), seekable, has_pos;
    long wants, size, n;
    unsigned long pos = 0;
    char *rest = NULL;
    int got = 0;
    if (r->save_all) return start_save(r);
    if (r->as_text) return true;    /* the original bytes, not what a format makes of them */
    wants = r->picture ? millGivesPicture | millGivesStream | millGivesMembers : millGivesElements | millGivesStream | millGivesMembers;
    f = format_for(r->mime, r->url, 0, wants);
    if (!f) {
        if (r->picture) return true;    /* Mill's own icons: kept whole */
        /* a picture by itself: its page shows it (and asks for it again, as an image) */
        if (format_for(r->mime, r->url, 0, millGivesPicture)) return true;
        if (r->mime[0] && !prefix_ci(r->mime, "text/", 5)) return r->may_save ? start_save(r) : true;
        if (!gz) return true;
        /* text, gzip'd for the journey: undone */
    }
    seekable = f && source_seekable(r, &size);
    if (!f) size = -1;
    has_pos = r->member && list_pos(r->url, r->path, &pos);
    if ((n = r->len - r->body) > 0 && !(rest = malloc(n))) { ran_out = true; return false; }
    if (n > 0) memcpy(rest, r->buf + r->body, n);
    r->run = format_begin(f, r->mime, gz ? r->encoding : NULL, r->url, r->path, pos, has_pos,
                          r->picture ? r->picture->aw : -1, r->picture ? r->picture->ah : -1, wants, size, seekable, &r->sink);
    if (!r->run) { free(rest); r->failed = true; return false; }
    if (f) r->deciding = true;      /* the reply's bytes are kept too, till the format answers */
    else r->len = r->body;
    if (n > 0) got = format_data(r->run, rest, n);
    free(rest);
    if (got < 0) r->failed = true;
    return got == 0 || r->saving;
}

/* While the format hasn't answered: the reply's bytes, for saving it whole */
static void keep_raw(Response *r, const char *data, long len)
{
    if (r->raw_lost || r->len + len > r->size) { r->raw_lost = true; return; }
    memcpy(r->buf + r->len, data, len);
    r->len += len;
}

/* "Reading... 3K of 4K", of the body; "Drawing..." once all of a picture is in */
static void show_progress(Response *r)
{
    char msg[48];
    long from = r->resume ? r->resume_from : 0;     /* what the part file had */
    long got = from + r->raw - r->body;
    const char *verb = r->saving ? "Saving" : "Reading";
    if (r->picture && r->total > 0 && r->raw >= r->total) strcpy(msg, "Drawing\311");
    else if (r->total > 0) sprintf(msg, "%s\311 %ldK of %ldK", verb, done_k(got, from + r->total - r->body),
                                   (from + r->total - r->body + 1023) / 1024);
    else sprintf(msg, "%s\311 %ldK", verb, (got + 1023) / 1024);
    progress(msg);
}

static Boolean collect(const char *data, long len, void *ref)
{
    Response *r = ref;
    Boolean shown = r->headed;
    if (stop_asked()) return false;
    r->raw += len;
    if (shown) show_progress(r);    /* before the bytes are decoded or written */
    if (r->saving) {
        if (saver_write(&r->saver, data, len) != noErr) return false;
        r->len += len;
    } else if (r->run) {
        int got;
        if (r->deciding) keep_raw(r, data, len);
        got = format_data(r->run, data, len);
        if (got == -2) r->not_found = true;
        else if (got < 0) r->failed = true;
        if (got != 0 && !r->saving) return false;
    } else {
        if (!keep(r, data, len)) return false;
        if (!r->headed) read_head(r);
        if (!shown) show_progress(r);
        /* once the headers and the first bytes are in (to decide with), or all there is */
        if (r->headed && !r->decided && r->code >= 200 && r->code < 300 &&
            (r->len - r->body >= 512 || (r->total > 0 && r->raw >= r->total))) {
            r->decided = true;
            if (!start_reply(r)) return false;
        }
    }
    /* done at the length it gave, even if the server (or a proxy) keeps the connection open */
    return !(r->total > 0 && r->raw >= r->total);
}

/* The reply has ended: the format answers now if it hadn't, then Mill reads by parts if
   it was to (format_pull) */
static void finish_reply(Response *r)
{
    if (r->headed && !r->decided && r->code >= 200 && r->code < 300 && !stopped) {
        r->decided = true;
        if (!start_reply(r)) return;
    }
    if (!r->run) return;
    if (r->deciding && !stopped) format_input_end(r->run);
    if (r->pull && !stopped) {
        FmtSource s;
        OSErr err;
        s.size = r->src_ref ? r->src_size : r->total - r->body;
        s.block = plugin_block_size(r->req);
        s.read = r->src_ref ? file_read : range_read;
        s.ref = r;
        err = format_pull(r->run, &s);
        if (err == fnfErr) r->not_found = true;
        else if (err != noErr && !stopped) r->failed = true;
    }
}

enum { FETCH_BAD_URL = 1, FETCH_CROSS, FETCH_FAILED, FETCH_SILENT };

/* Why a request failed (FETCH_FAILED): a Plug-In's Fail */
static char fail_text[200];

/* A request that failed, other than a page's (which gets Mill's error page): said in an alert */
static void fail_alert(const char *text)
{
    Str255 p;
    long n = strlen(text);
    if (n > 255) n = 255;
    p[0] = (unsigned char)n;
    memcpy(p + 1, text, n);
    ParamText(p, "\p", "\p", "\p");
    InitCursor();
    NoteAlert(ALERT_FAILED, NULL);
}

/* A download that stopped: what its part file holds, after what happened (said first);
   false if there's no part file */
static Boolean kept_status(const Response *r, const char *what)
{
    char msg[160], k[24];
    long d, rs;
    const FSSpec *part = &r->saver.part;
    if (!r->saver.in_part || file_lengths(part, &d, &rs) != noErr) return false;
    kilobytes(d + rs, k);
    snprintf(msg, sizeof msg, "%s%s%s saved in \322%.*s\323.", what, what[0] ? " " : "", k, part->name[0], (char *)part->name + 1);
    set_status(msg);
    return true;
}

/* After a fetch that saved, or was to: say how it went; whether the file is there */
static Boolean finish_save(Response *r, OSErr err)
{
    char msg[120], name[64];
    if (r->declined) { set_status(r->cancelled ? "" : "Not saved."); return false; }
    snprintf(name, sizeof name, "%.*s", r->spec.name[0], (char *)r->spec.name + 1);
    if (stopped || err != noErr) {
        saver_abort(&r->saver);
        if (err == FETCH_FAILED) { fail_alert(fail_text); kept_status(r, ""); }
        else if (stopped) { if (!kept_status(r, "Stopped.")) set_status("Stopped. The file wasn\325t saved."); }
        else {
            snprintf(msg, sizeof msg, "The download failed (MacTCP error %d).", err);
            if (!kept_status(r, msg)) set_status(msg);
        }
        return false;
    }
    if (r->closed) { saver_abort(&r->saver); err = eofErr; }     /* only its start came */
    else err = saver_end(&r->saver);
    if (err == eofErr && kept_status(r, "Ended early.")) return false;
    if (err != noErr) {
        snprintf(msg, sizeof msg, err == eofErr ? "\322%s\323 ended early and wasn\325t saved." : "Mill couldn\325t save \322%s\323 (error %d).", name, err);
        set_status(msg);
        return false;
    }
    snprintf(msg, sizeof msg, r->restarted ? "Saved \322%s\323 from the start: the server couldn\325t resume." :
             r->saver.macbinary && !r->saver.forks ? "Saved \322%s\323, decoded from MacBinary." : "Saved \322%s\323.", name);
    set_status(msg);
    return true;
}

/* A saved file's file: address, with "#/" to browse it: false if it won't fit */
static Boolean saved_url(const FSSpec *spec, char *out, int size)
{
    static const char hex[] = "0123456789ABCDEF";
    char path[400];
    long dir = spec->parID, n = spec->name[0];
    CInfoPBRec pb;
    Str255 name;
    int o, i;
    memcpy(path + sizeof path - n, spec->name + 1, n);
    i = sizeof path - n;
    /* up through its folders to the disk, whose parent is 1 */
    while (dir != 1) {
        memset(&pb, 0, sizeof pb);
        pb.dirInfo.ioNamePtr = name;
        pb.dirInfo.ioVRefNum = spec->vRefNum;
        pb.dirInfo.ioFDirIndex = -1;
        pb.dirInfo.ioDrDirID = dir;
        if (PBGetCatInfoSync(&pb) != noErr || i < name[0] + 1) return false;
        path[--i] = ':';
        i -= name[0];
        memcpy(path + i, name + 1, name[0]);
        dir = pb.dirInfo.ioDrParID;
    }
    o = snprintf(out, size, "file:///");
    for (; i < (int)sizeof path; i++) {
        unsigned char c = (unsigned char)path[i];
        if (o + 4 >= size) return false;
        if (c == ':') out[o++] = '/';
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.~", c)) out[o++] = c;
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    if (o + 3 >= size) return false;
    strcpy(out + o, "#/");
    return true;
}


static void show_url(const char *url)
{
    /* from the start, however far the last address was scrolled */
    (*br->url_te)->destRect.left = (*br->url_te)->viewRect.left;
    (*br->url_te)->destRect.right = (*br->url_te)->viewRect.left + 2000;
    TESetText((Ptr)url, strlen(url), br->url_te);
    TESetSelect(0, 0, br->url_te);
    InvalRect(&(*br->url_te)->viewRect);
}

/* A history entry is three strings, one after the other, and nothing else (no page,
   layout, or pictures; Back loads the page again): the address; the page it came from
   ("" for none), so Reload can say the same again; and what was typed in its forms when
   it was left ("" for nothing), 'S' if that was sent or 'K' if not, then forms_save's
   text. At most history_size() are kept, oldest going first. */
static void remember(const char *url, const char *from)
{
    short i, keep = history_size();
    char *e;
    if (br->hist_pos >= 0 && !strcmp(br->history[br->hist_pos], url)) return;
    for (i = br->hist_pos + 1; i < br->hist_len; i++) free(br->history[i]);
    br->hist_len = br->hist_pos + 1;
    while (br->hist_len >= keep) {
        /* the oldest goes (and more, if the setting was made smaller) */
        free(br->history[0]);
        memmove(br->history, br->history + 1, (br->hist_len - 1) * sizeof(char *));
        br->hist_len--;
    }
    br->hist_pos = br->hist_len - 1;
    if (!(e = malloc(strlen(url) + strlen(from) + 3))) return;
    strcpy(e, url);
    strcpy(e + strlen(url) + 1, from);
    e[strlen(url) + 1 + strlen(from) + 1] = 0;
    br->history[br->hist_len] = e;
    br->hist_pos = br->hist_len++;
}

/* A history entry's form state: "" for none */
static const char *entry_forms(const char *e)
{
    e += strlen(e) + 1;
    return e + strlen(e) + 1;
}

/* Keep what's typed in the page being left with its history entry, for Back and
   Forward to bring back; sent says the form is being submitted */
static void keep_form_state(Boolean sent)
{
    char *e, *state = NULL;
    long head;
    if (!br->have_page || br->hist_pos < 0 || strcmp(br->history[br->hist_pos], br->page_url)) return;
    keep_typing();
    if (br->forms.n) state = forms_save(&br->forms, &br->doc);
    e = br->history[br->hist_pos];
    head = entry_forms(e) - e;
    if (!state && !e[head]) return;     /* nothing then, nothing now */
    if (!(e = malloc(head + (state ? strlen(state) + 2 : 1)))) { free(state); return; }
    memcpy(e, br->history[br->hist_pos], head);
    if (state) { e[head] = sent ? 'S' : 'K'; strcpy(e + head + 1, state); }
    else e[head] = 0;
    free(state);
    free(br->history[br->hist_pos]);
    br->history[br->hist_pos] = e;
}

static int prefix_ci(const char *s, const char *p, long n)
{
    long i;
    for (i = 0; i < n; i++) if ((s[i] | 32) != (p[i] | 32)) return 0;
    return 1;
}

enum { BODY_TEXT, BODY_HTML, BODY_PICTURE };

typedef struct {
    int code;
    long body;          /* where the body starts */
    int kind;           /* BODY_TEXT, BODY_HTML or BODY_PICTURE */
    int charset;        /* from the header's charset=, else CHARSET_GUESS */
    char location[300];
    long refresh, refresh_len;  /* the Refresh header's value, where it is in the reply (len 0: none) */
} Head;

/* Headers end at a blank line */
static void split_response(const char *b, long len, Head *h)
{
    long i, line = 0;
    memset(h, 0, sizeof *h);
    h->kind = BODY_HTML;
    h->body = len;
    if (len > 12 && !memcmp(b, "HTTP/", 5)) h->code = atoi(b + 9);
    for (i = 0; i < len; i++) {
        if (b[i] == '\n') {
            long l = line, e = i;
            if (e > l && b[e - 1] == '\r') e--;
            if (e == l) { h->body = i + 1; return; }
            if (e - l > 9 && prefix_ci(b + l, "Location:", 9)) {
                long s = l + 9;
                while (s < e && b[s] == ' ') s++;
                snprintf(h->location, sizeof h->location, "%.*s", (int)(e - s), b + s);
            }
            if (e - l > 8 && prefix_ci(b + l, "Refresh:", 8)) {
                h->refresh = l + 8;
                h->refresh_len = e - l - 8;
            }
            if (e - l > 13 && prefix_ci(b + l, "Content-Type:", 13)) {
                long s = l + 13, k;
                while (s < e && b[s] == ' ') s++;
                h->kind = prefix_ci(b + s, "text/html", 9) ? BODY_HTML : prefix_ci(b + s, "image/", 6) ? BODY_PICTURE : BODY_TEXT;
                for (k = s; k + 8 <= e; k++) {
                    if (prefix_ci(b + k, "charset=", 8)) {
                        long st = k + 8, en;
                        if (st < e && b[st] == '"') st++;
                        for (en = st; en < e && b[en] != ';' && b[en] != ' ' && b[en] != '"'; en++) {}
                        h->charset = charset_named(b + st, (int)(en - st));
                        break;
                    }
                }
            }
            line = i + 1;
        }
    }
}


/* The extra header lines for a request: the User-Agent, the page it came from (referer,
   or NULL), and cookies. A login is the Plug-In's to send. */
static const char *request_headers(const Url *u, const char *referer)
{
    static char headers[1600];
    unsigned long now;
    int n = 0;
    ReadDateTime(&now);  /* the trap: GetDateTime is a low-memory read GCC flags */
    headers[0] = 0;
    if (user_agent()[0]) n = snprintf(headers, 160, "User-Agent: %.140s", user_agent());
    if (referer) n += snprintf(headers + n, 320, "%sReferer: %.300s", n ? "\r\n" : "", referer);
    if (url_is_web(u->scheme) && accept_cookies() && jar_header(&jar, u->host, u->path, u->scheme == SCHEME_HTTPS, now, headers + n + (n ? 10 : 8), 1024) > 0) {
        memcpy(headers + n, n ? "\r\nCookie: " : "Cookie: ", n ? 10 : 8);
    }
    return headers;
}

static char asked[140];    /* the host of the last request, for saying what went wrong */

/* Fetch a URL into resp (a POST when content_type is given), following up to five
   redirects of the same kind and answering a site's request for a name and password;
   target becomes where it ended up, without any user:password@. from is the page that
   asked for it, sent as the Referer (redirects too), or NULL. Returns noErr, a MacTCP
   error, FETCH_BAD_URL, or FETCH_CROSS. */
/* The key of the login the last page's fetch logged in with, "" if none (bookmarks
   offer it) */
static char fetched_login[AUTH_KEYS];

static void forget_logins(char *addr_pass, char *typed_pass)
{
    memset(addr_pass, 0, 64);
    memset(typed_pass, 0, 64);
    plugin_login_typed(NULL, NULL, NULL);
}

static OSErr fetch_reply(char *target, const char *from, const char *content_type, Body *body,
                         Response *resp, Url *u, Head *h, Boolean page)
{
    int redirects = 0;
    /* the address's login, for its own server; and what was typed in the dialog this
       action (Mill asks once), for typed's keys */
    char addr_user[64], addr_pass[64];
    char typed[AUTH_KEYS], typed_user[64], typed_pass[64];
    OSErr err;
    addr_user[0] = addr_pass[0] = typed[0] = typed_user[0] = typed_pass[0] = 0;
    fetched_login[0] = 0;
    plugin_login_typed(NULL, NULL, NULL);
    for (;;) {
        if (url_parse(target, u)) { forget_logins(addr_pass, typed_pass); return FETCH_BAD_URL; }
        if (u->user[0]) {
            strcpy(addr_user, u->user);
            strcpy(addr_pass, u->pass);
            memset(u->pass, 0, sizeof u->pass);
            u->user[0] = 0;
            url_strip_login(target);
        }
        resp->len = resp->total = resp->body = 0;
        resp->code = 0;
        resp->mime[0] = 0;
        resp->owner[0] = 0;
        if (resp->run) { format_end(resp->run, millCutStop, NULL, NULL); resp->run = NULL; }
        resp->raw = 0;
        resp->encoding[0] = resp->inner[0] = 0;
        resp->failed = false;
        resp->headed = resp->cut = resp->closed = false;
        resp->url = target;
        resp->req = u;
        /* an archive's member (after "#/"): the format is given the whole address */
        resp->member = url_member(target, resp->path, sizeof resp->path);
        resp->decided = resp->deciding = resp->raw_lost = resp->pull = false;
        resp->listing = resp->browse = resp->not_found = resp->range_failed = false;
        resp->fmt = NULL;
        source_close(resp);
        list_free(resp->list);
        resp->list = NULL;
        resp->sink.ref = resp;
        resp->sink.out = resp->picture ? NULL : decoded;
        resp->sink.ask = reply_ask;
        resp->sink.entry = reply_entry;
        resp->sink.save_member = reply_save_member;
        resp->sink.save = reply_save;
        if (u->scheme != SCHEME_MILL) {
            char referer[300];
            Boolean refer = from && url_referer(from, target, referer, sizeof referer);
            const char *headers = request_headers(u, refer ? referer : NULL);
            if (resp->ranged) {
                /* a part: the rest of a download, or its first bytes again */
                static char with_range[1700];
                char range[48];
                if (resp->range_last >= 0) sprintf(range, "Range: bytes=%ld-%ld", resp->range_first, resp->range_last);
                else sprintf(range, "Range: bytes=%ld-", resp->range_first);
                snprintf(with_range, sizeof with_range, "%s%s%s", headers, headers[0] ? "\r\n" : "", range);
                headers = with_range;
            }
            {
                /* every scheme but mill: is a Plug-In's (http: and https: too). A page may only
                   use plugin://NAME/ if NAME marked it as its own; typed addresses and mill:
                   pages always may. */
                const char *owner = from && br && !strcmp(from, br->page_url) ? br->page_owner : "";
                strcpy(asked, u->host[0] ? u->host : "this Mac");
                if (u->scheme == SCHEME_OTHER && !strcmp(u->name, "plugin") && from && strncmp(from, "mill:", 5) &&
                    strcmp(owner, u->host)) {
                    static const char no[] = "HTTP/1.0 403 Forbidden\r\nContent-Type: text/html\r\n\r\n"
                        "<TITLE>Forbidden</TITLE><H2>Forbidden</H2><P>Only the Plug-In&rsquo;s own pages can use this address.";
                    collect(no, sizeof no - 1, resp);
                    err = noErr;
                } else if (u->scheme == SCHEME_OTHER && !strcmp(u->name, "plugin") && font_file_request(u, collect, resp)) {
                    err = noErr;        /* a font file's About: Mill answers for it */
                } else {
                    Url with = *u;
                    char key[AUTH_KEYS], bare[300];
                    strcpy(with.user, addr_user);
                    strcpy(with.pass, addr_pass);
                    if (body) body_rewind(body);    /* asked again: from the start */
                    /* the address without its fragment: a member's path is the format's */
                    snprintf(bare, sizeof bare, "%.*s", (int)strcspn(target, "#"), target);
                    err = plugin_request(&with, bare, headers, content_type, body, collect, resp, owner, resp->owner, key, br, page);
                    memset(with.pass, 0, sizeof with.pass);
                    if (err == PLUGIN_CUT) { resp->closed = true; err = noErr; }   /* shown as far as it came */
                    if (err == PLUGIN_FAILED) {
                        snprintf(fail_text, sizeof fail_text, "%s", plugin_failure());
                        forget_logins(addr_pass, typed_pass);
                        return FETCH_FAILED;
                    }
                    if (err == paramErr) { forget_logins(addr_pass, typed_pass); return FETCH_BAD_URL; }
                    if (err == PLUGIN_ASKED) {
                        /* it needs a login: the person's, asked once per action (asked
                           again after the dialog is a Cancel); else its page, or nothing */
                        const PluginAsked *a = plugin_asked();
                        Cred *k;
                        if (!typed[0]) {
                            char name[64];
                            snprintf(name, sizeof name, "%s", a->name);
                            snprintf(typed_pass, sizeof typed_pass, "%s", a->pass);
                            memset(((PluginAsked *)a)->pass, 0, sizeof a->pass);
                            if (login_ask(a->prompt, a->name_label, a->pass_label, name, sizeof name, typed_pass, sizeof typed_pass)) {
                                watch_cursor();
                                snprintf(typed, sizeof typed, "%s", a->key[0] ? a->key : "\n");
                                strcpy(typed_user, name);
                                plugin_login_typed(typed, typed_user, typed_pass);
                                continue;
                            }
                            watch_cursor();
                        } else if (strcmp(a->key, typed)) {
                            /* what was typed worked; it's someone else asking now (the site,
                               after its proxy) */
                            auth_save(&keyring, typed, typed_user, typed_pass);
                        }
                        memset(((PluginAsked *)a)->pass, 0, sizeof a->pass);
                        /* cancelled: a kept login that didn't work isn't offered again */
                        if (a->forget && (k = auth_find(&keyring, a->key)) != NULL) auth_forget(&keyring, k);
                        split_response(resp->buf, resp->len, h);
                        if (h->body >= resp->len) err = FETCH_SILENT;    /* no page, or just its head */
                        else err = noErr;
                    } else if (err == noErr) {
                        /* it ended without asking: what was typed, or the address's login, worked */
                        if (typed[0] && typed[0] != '\n') auth_save(&keyring, typed, typed_user, typed_pass);
                        if (key[0] && addr_user[0]) auth_save(&keyring, key, addr_user, addr_pass);
                        snprintf(fetched_login, sizeof fetched_login, "%s", typed[0] && typed[0] != '\n' ? typed :
                                 key[0] && addr_user[0] ? key : plugin_login_used());
                    }
                }
            }
        } else {
            static short making_page;   /* a mill: page can fetch (Check for updates) */
            making_page++;
            {
                /* Mill's own pages' forms are small: all at once */
                long len = 0;
                char *all = body ? body_all(body, &len) : NULL;
                err = local_request(u, content_type, all, len, collect, resp);
                free(all);
            }
            if (--making_page == 0) UnloadSeg((void *)local_request);  /* mill: pages take memory only while one's made */
        }
        if (err == noErr || err == userCanceledErr) finish_reply(resp);
        if (resp->run && !resp->picture) {
            /* a page a format read: the rest of what it gives */
            format_end(resp->run, resp->cut ? millCutLimit : stopped ? millCutStop : resp->closed ? millCutClosed : 0, NULL, resp->inner);
            resp->run = NULL;
        }
        if (resp->listing && !resp->saving && !resp->member_saving && !resp->declined && !stopped && err == noErr)
            listing_page(resp, target);
        if (err != noErr || resp->saving || resp->member_saving || resp->declined) {
            forget_logins(addr_pass, typed_pass);
            return err;
        }
        split_response(resp->buf, resp->len, h);
        if (resp->inner[0]) {
            /* what the format gave: HTML, or text; or a member that's a picture, shown as one.
               In the character set it said (a Mac's text member: Mac Roman), else guessed. */
            h->kind = prefix_ci(resp->inner, "text/html", 9) ? BODY_HTML : prefix_ci(resp->inner, "image/", 6) ? BODY_PICTURE : BODY_TEXT;
            h->charset = charset_of_type(resp->inner);
        } else if (h->kind != BODY_HTML && h->code < 300 &&
                   (format_for(resp->mime, target, 0, millGivesPicture) ||
                    (gzipped(resp) && format_for(resp->mime, NULL, 0, millGivesPicture)))) {
            h->kind = BODY_PICTURE;     /* a picture a format reads, by type or by name */
        } else if (h->kind == BODY_PICTURE && !format_for(resp->mime, target, 0, millGivesPicture)) {
            h->kind = BODY_TEXT;
        }
        if (resp->not_found) {
            /* no such member: said as file: says it */
            long o = h->body;
            char line[400], mac[256], esc[300];
            charset_name_to_mac(resp->path, CHARSET_GUESS, mac, sizeof mac);
            html_escape_to(mac, esc, sizeof esc);
            snprintf(line, sizeof line, "<TITLE>Not found</TITLE><H2>Not found</H2><P>There\325s nothing at <TT>%.300s</TT>.", esc);
            if (o + (long)strlen(line) < resp->size) { strcpy(resp->buf + o, line); resp->len = o + strlen(line); }
            h->kind = BODY_HTML;
            h->charset = CHARSET_MAC;
        }
        if (resp->listing) { h->kind = BODY_HTML; h->charset = CHARSET_MAC; }
        if ((h->code == 301 || h->code == 302 || h->code == 303) && h->location[0] && redirects++ < 5) {
            char next[300];
            const char *m = strchr(target, '#');
            if (url_resolve(target, h->location, next, sizeof next)) return noErr;
            /* a member of an archive that moved is still that member */
            if (m && m[1] == '/' && !strchr(next, '#') && strlen(next) + strlen(m) < sizeof next) strcat(next, m);
            if (kind_of(next) != kind_of(target) || plugin_url(next)) return FETCH_CROSS;
            strcpy(target, next);
            content_type = NULL;  /* a redirect after a POST is fetched with GET */
            body = NULL;
            /* the next address's login is its own */
            addr_user[0] = typed[0] = 0;
            forget_logins(addr_pass, typed_pass);
            continue;
        }
        forget_logins(addr_pass, typed_pass);
        return noErr;
    }
}

/* One fetch, then the file a pull read (file:) closed */
static OSErr fetch(char *target, const char *from, const char *content_type, Body *body,
                   Response *resp, Url *u, Head *h, Boolean page)
{
    OSErr err = fetch_reply(target, from, content_type, body, resp, u, h, page);
    source_close(resp);
    list_free(resp->list);
    resp->list = NULL;
    return err;
}

/* A Mac icon from a Plug-In (plugin://NAME/rsrc, x-rsrc/TYPE), at its own size: 0, -1,
   or IMAGE_NO_MEMORY. Other pictures go to their formats as they arrive. */
static int decode_image(Pic *p, Response *resp, long body)
{
    char type[5] = "    ";
    long n;
    if (strncmp(resp->mime, "x-rsrc/", 7)) return -1;
    n = strlen(resp->mime + 7);
    memcpy(type, resp->mime + 7, n < 4 ? n : 4);
    return image_icon(type, (const unsigned char *)resp->buf + body, resp->len - body, &p->bm);
}

#define IMAGES_AUTO (-1)    /* load_images: as the settings say */
/* A page's pictures stop loading by themselves while this much is left, for laying the
   page out again, dialogs, and menus; a picture asked for by itself may use it */
#define PICTURE_ROOM (32L * 1024)
#define PICTURE_BUFFER (16L * 1024)  /* a picture's reply: headers, and icons kept whole */

static Boolean wanted(const Pic *p, short which, short i)
{
    if (p->state != PIC_WAITING) return false;
    if (which >= 0) return i == which;
    return auto_images() || !strncmp(p->url, "mill:", 5);  /* Mill's own images always load */
}

/* Fetch and show the page's images, one at a time, after its text: those the settings
   say to (IMAGES_AUTO), or just pics[which]. Each is read into a buffer of its own,
   trimmed to fit before it's decoded and let go of after, so pictures have the most room. */
static void load_images(Response *resp, short which)
{
    short i, waiting = 0, n = 0;
    Boolean resized = false;
    for (i = 0; i < br->npics; i++) if (wanted(&br->pics[i], which, i)) waiting++;
    for (i = 0; i < br->npics && !stop_asked(); i++) {
        Pic *p = &br->pics[i];
        char target[300];
        Url u;
        Head h;
        Kept *keep;
        if (!wanted(p, which, i)) continue;
        if ((keep = kept_find(p->url, p->aw, p->ah))) {
            /* another window loaded it meanwhile */
            p->bm = keep->bm;
            p->state = PIC_SHOWN;
            keep->used = page_number;
            keep->users++;
            if (p->aw < 0 || p->ah < 0) resized = true;
            else { SetPort(br->win); draw_page(i); }
            continue;
        }
        if (waiting > 1) sprintf(activity, "Image %d of %d", ++n, waiting);
        else strcpy(activity, "Image");
        if (which == IMAGES_AUTO) {
            while (FreeMem() < PICTURE_ROOM && make_room()) {}
            if (FreeMem() < PICTURE_ROOM) { out_of_memory(); break; }
        }
        p->state = PIC_FAILED;
        if (!strncmp(p->url, "mill:", 5)) {
            int got;
            while ((got = local_image(p->url, &p->bm)) == IMAGE_NO_MEMORY && make_room()) {}
            if (got == IMAGE_NO_MEMORY) out_of_memory();
            if (got) continue;
        } else {
            OSErr err;
            int got = -1;
            /* a picture goes to its format as it comes: the buffer is only for the headers,
               and Mill's own icons */
            if (!(resp->buf = malloc(PICTURE_BUFFER))) { out_of_memory(); p->state = PIC_WAITING; break; }
            resp->size = PICTURE_BUFFER;
            resp->picture = p;
            strcpy(target, p->url);
            /* a picture alone on a page is that page: no page asked for it */
            err = fetch(target, strcmp(p->url, br->page_url) ? br->page_url : NULL, NULL, NULL, resp, &u, &h, false);
            if (err == memFullErr) out_of_memory();
            if (resp->run) {
                if (!stopped) progress("Drawing\311");     /* the rest is decoded and drawn */
                got = format_end(resp->run, stopped ? millCutStop : resp->failed ? millCutBroken :
                                 resp->closed ? millCutClosed : 0, &p->bm, NULL);
                resp->run = NULL;
                if (got == 0 && (stopped || (err != noErr && err != userCanceledErr && !resp->raw))) { image_free(&p->bm); got = -1; }
                if (got == IMAGE_NO_MEMORY) out_of_memory();
            } else if (err == noErr && h.code < 300) {
                got = decode_image(p, resp, h.body);    /* Mill's own icons (x-rsrc) */
            }
            resp->picture = NULL;
            free(resp->buf);
            resp->buf = NULL;
            if (got == IMAGE_NO_MEMORY) continue;  /* it shows as a picture that didn't load */
            if (got) {
                if (stopped || resp->closed) p->state = PIC_WAITING;  /* it can still be loaded */
                continue;
            }
        }
        p->state = PIC_SHOWN;
        p->owned = !kept_add(p->url, p->aw, p->ah, &p->bm);
        if (p->aw < 0 || p->ah < 0) resized = true;  /* the page has to make room for it */
        else { SetPort(br->win); draw_page(i); }
    }
    activity[0] = 0;
    if (resized) {
        if (relayout() == -1) out_of_memory();
        if (br->scroll_y > max_scroll()) br->scroll_y = max_scroll();
        update_scroll();
        SetPort(br->win);
        draw_view();
    }
    if (stopped) set_status("Stopped.");
}

static void set_title(const char *t)
{
    Str255 title;
    title[0] = (unsigned char)(strlen(t) > 250 ? 250 : strlen(t));
    memcpy(title + 1, t, title[0]);
    SetWTitle(br->win, title);
}

/* Open a URL (a POST when content_type is given). Returns whether a page came. */
#define VIEW_SOURCE "view-source:"

/* Text as a page of its own: in <PRE>, with & < and > escaped so none of it is read as
   HTML; the title, if any, escaped the same way. malloc'd; its length in *len. */
static char *pre_page(const char *title, const char *text, long n, long *len)
{
    long i, size = n + strlen(title) * 5 + 80;
    char *page, *o;
    for (i = 0; i < n; i++) size += text[i] == '&' ? 4 : (text[i] == '<' || text[i] == '>') ? 3 : 0;
    if (!(page = malloc(size))) return NULL;
    o = page;
    for (i = 0; i < 2; i++) {
        const char *s = i ? text : title;
        long k, m = i ? n : (long)strlen(title);
        if (!i && !m) continue;
        strcpy(o, i ? "<PRE><FONT SIZE=2>" : "<TITLE>");  /* 10 points: 80 columns fit a Plus's window */
        o += strlen(o);
        for (k = 0; k < m; k++) {
            switch (s[k]) {
            case '&': memcpy(o, "&amp;", 5); o += 5; break;
            case '<': memcpy(o, "&lt;", 4); o += 4; break;
            case '>': memcpy(o, "&gt;", 4); o += 4; break;
            default: *o++ = s[k];
            }
        }
        if (!i) { strcpy(o, "</TITLE>"); o += 8; }
    }
    *len = o - page;
    return page;
}

/* Open url in this window; from is the page that asked for it (a link, a form), or NULL */
/* What went wrong with a request that failed on the network, as the status line says it */
static void net_trouble(OSErr err, char *msg, int size)
{
    if (err == NET_NO_MACTCP)
        snprintf(msg, size, "MacTCP isn\325t available.");
    else if (net_stage == NET_LOOKUP)
        snprintf(msg, size, err == commandTimeout ? "Looking up %s took too long." : "Couldn\325t find %s.", asked);
    else if (net_stage == NET_CONNECT)
        snprintf(msg, size, "Couldn\325t connect to %s.", asked);
    else
        snprintf(msg, size, "The connection to %s failed (MacTCP error %d).", asked, err);
}

/* local_fetch (see mac_local.h), for Check for updates: a GET with no Referer, its status
   line saying what it's for. Stop stops just this, so the page it's for still shows. */
static long fetch_for_page(const char *url, char *buf, long size, char *why, int why_size)
{
    Response resp;
    char target[300], was[40];
    Url u;
    Head h;
    OSErr err;
    long n = -1;
    memset(&resp, 0, sizeof resp);
    resp.size = size + 2048;    /* and its headers */
    if (!(resp.buf = malloc(resp.size))) { snprintf(why, why_size, "Not enough memory."); return -1; }
    strcpy(was, activity);
    strcpy(activity, "Checking for updates");
    set_status("Checking for updates\311");
    snprintf(target, sizeof target, "%s", url);
    err = fetch(target, NULL, NULL, NULL, &resp, &u, &h, false);
    if (err == memFullErr) out_of_memory();
    if (stopped || err == userCanceledErr) snprintf(why, why_size, ran_out ? "Not enough memory." : "Stopped.");
    else if (err == FETCH_BAD_URL || err == FETCH_CROSS) snprintf(why, why_size, "Its address isn\325t one Mill can open.");
    else if (err == FETCH_FAILED) snprintf(why, why_size, "%s", fail_text);
    else if (err == FETCH_SILENT) snprintf(why, why_size, "It asked for a name and password.");
    else if (err != noErr) net_trouble(err, why, why_size);
    else if (h.code != 200) snprintf(why, why_size, "The server didn\325t send it (error %d).", h.code);
    else if (resp.cut) snprintf(why, why_size, "Its answer was too long.");
    else if (resp.closed) snprintf(why, why_size, "Its answer ended early.");
    else {
        n = resp.len - h.body;
        if (n > size) n = size;
        memcpy(buf, resp.buf + h.body, n);
    }
    free(resp.buf);
    strcpy(activity, was);
    stopped = false;    /* the check stopped; the page goes on (running out is still said) */
    return n;
}

static Boolean coming_back;     /* load: Back or Forward, so the page's forms come back too */
static Boolean forms_kept;      /* load: the page being left has had its forms kept already */

static Boolean reloading;       /* load: Reload, so the page is asked for again */
static long refresh_y = -1;     /* load: a refresh of this page, which stays scrolled here */

/* The length of an address without its #fragment (a member's "#/" path is the document's) */
static size_t doc_part(const char *url) { return url_doc_len(url); }

/* A link to a #name on the page already shown, or Back and Forward between two places on it:
   the page is only scrolled, not asked for again. how is a URL_OPEN_ kind. */
static Boolean same_page(const char *url, int how, Boolean record, const char *from)
{
    size_t n = doc_part(url);
    const char *frag = url[n] == '#' ? url + n : NULL;
    long i;
    if (!br->have_page || strncmp(url, VIEW_SOURCE, strlen(VIEW_SOURCE)) == 0 || strlen(url) >= sizeof br->page_url ||
        !url_scroll_only(url, br->page_url, how))
        return false;
    coming_back = false;
    forms_kept = false;     /* the page's fields stay as they are */
    blur();
    br->scroll_y = 0;
    if (frag && frag[1]) {
        for (i = 0; i < br->lay.nanchors; i++)
            if (!strcmp(br->lay.anchors[i].name, frag + 1)) { br->scroll_y = br->lay.anchors[i].y; break; }
        if (br->scroll_y > max_scroll()) br->scroll_y = max_scroll();
    }
    strcpy(br->page_url, url);
    show_url(url);
    if (record) remember(url, from ? from : "");
    update_scroll();
    redraw();
    return true;
}

/* A page load that got no reply: Mill's own page says why in the server's place, its
   address staying (Reload tries again). title is its heading too; text is said as is. */
static void error_page(Response *r, Head *h, const char *title, const char *text)
{
    char esc[400];
    long n;
    html_escape_to(text, esc, sizeof esc);
    n = snprintf(r->buf, r->size, "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n"
                 "<TITLE>%s</TITLE><H2>%s</H2><P>%s", title, title, esc);
    r->len = n < r->size ? n : r->size - 1;
    r->cut = r->closed = false;
    r->owner[0] = 0;
    split_response(r->buf, r->len, h);
    h->charset = CHARSET_MAC;
}

/* Why a page load got no reply: the error page's heading and sentence */
static void no_reply(OSErr err, Boolean nothing, char *title, char *msg, int size)
{
    if (err == FETCH_FAILED) {
        strcpy(title, "Couldn\325t open the page");
        snprintf(msg, size, "%s", fail_text);
    } else if (err == FETCH_BAD_URL) {
        strcpy(title, "Couldn\325t open the page");
        snprintf(msg, size, "Mill can\325t open this kind of address.");
    } else if (err == NET_NO_MACTCP) {
        strcpy(title, "No MacTCP");
        snprintf(msg, size, "MacTCP isn\325t available, so Mill can\325t reach %s.", asked);
    } else if (nothing) {
        strcpy(title, "No reply");
        snprintf(msg, size, "%s closed the connection without replying.", asked);
    } else if (net_stage == NET_LOOKUP) {
        strcpy(title, "Server not found");
        snprintf(msg, size, err == commandTimeout ? "Looking up %s took too long." : "There\325s no server named %s.", asked);
    } else if (net_stage == NET_CONNECT) {
        strcpy(title, "No connection");
        snprintf(msg, size, "The server %s couldn\325t be contacted.", asked);
    } else if (err == commandTimeout) {
        strcpy(title, "No reply");
        snprintf(msg, size, "%s didn\325t reply.", asked);
    } else {
        strcpy(title, "No reply");
        snprintf(msg, size, "The connection to %s failed (MacTCP error %d).", asked, err);
    }
}

static Boolean save_url(const char *url, const char *from, Boolean browse);
static void arm_refresh(long secs, Boolean back);
static Boolean save_to(const char *url, const char *from, const FSSpec *spec, Boolean resume, Boolean browse);

/* Whether the last load showed a page without an error: all of a 2xx reply */
static Boolean page_whole;

static Boolean load(const char *url, Boolean record, const char *from, const char *content_type, Body *body)
{
    /* view-source:address shows what address sends, as text */
    Boolean source = !strncmp(url, VIEW_SOURCE, strlen(VIEW_SOURCE));
    char shown_url[320];
    char target[300], referrer[300];
    Response resp;
    Url u;
    Head h;
    OSErr err;
    Boolean shown = false, cut, closed;
    Boolean failed = false;     /* no reply: Mill's error page shows */
    char why[200];
    int parsed = -1;
    long page_k;
    Boolean back = coming_back;
    Plug *making;   /* the Plug-In for the address, kept loaded from asking to showing */
    Boolean again = reloading;
    long keep_y = refresh_y;
    long refresh_secs = -1;
    char then[320]; /* what's next: the saved copy to browse, or (save_after) the archive to save */
    short save_after = 0;
    Boolean resume_later = false, resume_browse = false;
    FSSpec resume_spec;
    then[0] = 0;

    reloading = false;
    refresh_y = -1;
    page_whole = false;
    if (!body && same_page(url, again ? URL_OPEN_RELOAD : record ? URL_OPEN_LINK : URL_OPEN_HISTORY, record, from)) return true;
    br->refresh_at = 0;     /* leaving, or stopped: a refresh asked for is off */
    coming_back = false;
    blur();
    snprintf(referrer, sizeof referrer, "%s", from ? from : "");  /* from may be this window's page_url */
    strncpy(target, source ? url + strlen(VIEW_SOURCE) : url, sizeof target - 1);
    target[sizeof target - 1] = 0;
    /* (url and from may be in a history entry, which this replaces) */
    if (!forms_kept) keep_form_state(false);
    forms_kept = false;
    stopped = ran_out = false;
    leaving = true;     /* until the new page takes this one's place */
    loading = target;
    if (!response_buffer(&resp)) {
        leaving = false;
        loading = NULL;
        InvalRect(&br->win->portRect);
        out_of_memory_alert("it couldn\325t load this page.");
        return false;
    }
    response_init(&resp);
    page_k = resp.size / 1024;
    resp.may_save = !source;
    resp.as_text = source;
    set_loading(true);
    activity[0] = 0;
    memset(&u, 0, sizeof u);
    watch_cursor();
    snprintf(shown_url, sizeof shown_url, "%s%s", source ? VIEW_SOURCE : "", target);
    show_url(shown_url);
    making = plugin_hold(target);
    err = fetch(target, referrer[0] ? referrer : NULL, content_type, body, &resp, &u, &h, true);
    /* where it ended up; Browse makes it the archive's listing */
    snprintf(shown_url, sizeof shown_url, "%s%s%s", source ? VIEW_SOURCE : "", target, resp.browse ? "#/" : "");
    if (resp.saving || resp.member_saving || resp.declined) {
        /* a file Mill can't show: saved, and the page stays; then browsed, if asked */
        if (finish_save(&resp, err) && resp.after && !saved_url(&resp.spec, then, sizeof then)) then[0] = 0;
        if (resp.resume_later) {
            /* Resume: asked for again, from the part file's end */
            resume_later = true;
            resume_browse = resp.after;
            resume_spec = resp.spec;
            snprintf(then, sizeof then, "%.*s", (int)strcspn(target, "#"), target);
        }
        show_url(br->page_url);
        goto done;
    }
    if (resp.range_failed && resp.fmt) {
        /* ranges were claimed, and the whole came: the dialog again, browsing only once saved */
        char name[64];
        Boolean after;
        filetype_name(target, name, sizeof name);
        show_url(br->page_url);
        if (archive_dialog(name, resp.fmt, resp.total > 0 ? resp.total - resp.body : -1,
                           "This server stopped sending parts. It may only be browsed after saving.", false, true, &after) == AD_SAVE) {
            snprintf(then, sizeof then, "%.*s", (int)strcspn(target, "#"), target);
            save_after = after ? 2 : 1;
        } else set_status("");
        goto done;
    }
    if (err == FETCH_SILENT) {
        /* a cancelled login: the page and its history stay as they were, as after Stop */
        show_url(br->page_url);
        goto done;
    }
    show_url(shown_url);
    if (err == memFullErr) { out_of_memory(); show_url(br->page_url); goto done; }
    if (stopped || err == userCanceledErr) {
        /* the page being left stays; but if it went to make room, what came of the new
           one shows instead */
        if (br->have_page || !resp.headed || resp.code < 200 || resp.code >= 300) { set_status("Stopped."); goto done; }
        split_response(resp.buf, resp.len, &h);
        err = noErr;
    }
    if (err == FETCH_CROSS) { set_status("Stopped at a redirect to another kind of address."); goto done; }
    if (err != noErr || resp.len == 0) {
        /* no reply at all (no MacTCP, lookup, connect, timeout, closed before any reply, the
           Plug-In's failure, an address no Plug-In takes): Mill's error page, as a server's
           would be; a server's own error page is shown as it is */
        char title[40];
        no_reply(err, err == noErr, title, why, sizeof why);
        error_page(&resp, &h, title, why);
        failed = true;
    }
    if (!failed && !source && h.refresh_len > 0 &&
        !refresh_parse(resp.buf + h.refresh, h.refresh_len, &refresh_secs, br->refresh_url, sizeof br->refresh_url))
        refresh_secs = -1;

    set_status("Formatting\311");
    {
        /* the end of the buffer the page didn't use is room for the page itself */
        char *fits = realloc(resp.buf, resp.len + 1);  /* shrinks where it is */
        if (fits) resp.buf = fits;
    }
    if (br->have_page) { layout_free(&br->lay); pics_free(); forms_free(&br->forms); html_free(&br->doc); br->have_page = false; }
    free(br->form_base);
    br->form_base = NULL;
    leaving = false;    /* what's loading now is this page */
    page_number++;
    if (!failed && (source || h.kind != BODY_HTML)) {
        /* source and plain text show preformatted, and a picture by itself on a page */
        long n = resp.len - h.body, len;
        const char *text = resp.buf + h.body;
        char *page = NULL;
        /* the charset goes by the text itself, before escaping hides its META tag */
        int cs = h.charset != CHARSET_GUESS ? h.charset : charset_sniff(text, n);
        if (source) {
            page = pre_page(shown_url, text, n, &len);
        } else if (h.kind == BODY_PICTURE) {
            if ((page = malloc(strlen(target) + 32))) len = sprintf(page, "<IMG SRC=\"%s\">", target);
        } else {
            page = pre_page("", text, n, &len);
        }
        if (page) {
            parsed = html_parse_charset(&br->doc, page, len, cs);
            free(page);
        }
    } else {
        parsed = html_parse_charset(&br->doc, resp.buf + h.body, resp.len - h.body, h.charset);
        while (parsed && make_room()) {
            /* room made: try again */
            html_free(&br->doc);
            parsed = html_parse_charset(&br->doc, resp.buf + h.body, resp.len - h.body, h.charset);
        }
    }
    /* the source is in the page now; pictures get buffers of their own */
    free(resp.buf);
    resp.buf = NULL;
    if (parsed) out_of_memory();
    /* out of memory partway, the page is what was read so far */
    br->have_page = br->doc.nnodes > 0;
    if (!br->have_page) { html_free(&br->doc); set_status("Stopped."); goto done; }
    if (forms_build(&br->forms, &br->doc)) { forms_free(&br->forms); out_of_memory(); }
    else if (back && br->forms.n && br->hist_pos >= 0 && *entry_forms(br->history[br->hist_pos])) {
        /* back to a page left with things typed in it: they come back. If they'd been
           sent, they aren't changes to warn about until something else is changed. */
        const char *state = entry_forms(br->history[br->hist_pos]);
        forms_restore(&br->forms, &br->doc, state + 1);
        if (state[0] == 'S') br->form_base = forms_save(&br->forms, &br->doc);
    }
    strcpy(br->page_url, shown_url);
    strcpy(br->page_login, fetched_login);
    {
        /* the Plug-In that made this page stays loaded while it shows */
        Plug *was = br->held;
        br->held = plugin_hold(shown_url);
        plugin_drop(was);
        strcpy(br->page_owner, source ? "" : resp.owner);
    }
    if (record) remember(shown_url, referrer);
    set_title(br->doc.title[0] ? br->doc.title : shown_url);
    if (!pics_collect()) out_of_memory();
    br->scroll_x = 0;
    if (relayout() == -1) out_of_memory();
    br->scroll_y = 0;
    if (*url_anchor(shown_url)) {
        long i;
        const char *anchor = url_anchor(shown_url);
        for (i = 0; i < br->lay.nanchors; i++)
            if (!strcmp(br->lay.anchors[i].name, anchor)) { br->scroll_y = br->lay.anchors[i].y; break; }
        if (br->scroll_y > max_scroll()) br->scroll_y = max_scroll();
    }
    if (keep_y >= 0) br->scroll_y = keep_y > max_scroll() ? max_scroll() : keep_y;
    update_scroll();
    redraw();
    cut = resp.cut;
    closed = resp.closed;
    response_init(&resp);   /* images are never saved */
    load_images(&resp, IMAGES_AUTO);
    if (!stopped) {
        char msg[48];
        sprintf(msg, "Showed the first %ldK of this page.", page_k);
        set_status(failed ? why : h.code >= 400 ? "The server sent an error page." : cut ? msg :
                   closed ? "The connection closed early." : "Done.");
    }
    page_whole = !failed && !stopped && h.code >= 200 && h.code < 300 && !closed;
    if (!failed && !source && !stopped) {
        const char *m;
        if (refresh_secs < 0 && (m = refresh_meta(&br->doc)) &&
            !refresh_parse(m, strlen(m), &refresh_secs, br->refresh_url, sizeof br->refresh_url))
            refresh_secs = -1;
        arm_refresh(refresh_secs, back);
    }
    shown = true;
done:
    loading = NULL;
    plugin_drop(making);
    /* the window keeps a connection only to its page's server (a page that failed may have
       kept one to another) */
    plugin_page_shown(br, br->page_url + (strncmp(br->page_url, VIEW_SOURCE, strlen(VIEW_SOURCE)) ? 0 : strlen(VIEW_SOURCE)));
    leaving = false;
    free(resp.buf);
    set_loading(false);
    InitCursor();
    InvalRect(&br->win->portRect);
    if (ran_out) out_of_memory_alert("it stopped loading this page.");
    /* Browse after saving: the saved copy, read by parts (file: always can be) */
    if (save_after) return save_url(then, referrer[0] ? referrer : NULL, save_after == 2) && shown;
    if (resume_later) return save_to(then, referrer[0] ? referrer : NULL, &resume_spec, true, resume_browse) && shown;
    if (then[0] && !stopped) return load(then, true, NULL, NULL, NULL);
    return shown;
}

/* A page with a form that's been changed and not sent: ask before what (such as "Leave
   this page?") happens. True to go ahead. */
static Boolean forms_unsent(void)
{
    if (!br || !br->have_page || !br->forms.n) return false;
    keep_typing();
    if (!forms_changed(&br->forms, &br->doc)) return false;
    if (br->form_base) {
        /* brought back by Back as it was sent, and not changed since */
        char *now = forms_save(&br->forms, &br->doc);
        Boolean same = now && !strcmp(now, br->form_base);
        free(now);
        if (same) return false;
    }
    return true;
}

static Boolean may_leave(const char *what)
{
    Str255 p;
    if (!forms_unsent()) return true;
    p[0] = (unsigned char)strlen(what);
    memcpy(p + 1, what, p[0]);
    ParamText(p, "\p", "\p", "\p");
    InitCursor();
    return CautionAlert(ALERT_UNSENT, NULL) == UNSENT_LEAVE;
}

/* What you type, choose from a menu, or go back to opens anything; pages can't reach
   across kinds of address. from is the page a link was on, or NULL. */
static Boolean go(const char *url, Boolean record, const char *from)
{
    if (!may_leave("Leave this page?")) return false;
    return load(url, record, from, NULL, NULL);
}

static Boolean same_kind(const char *url)
{
    return kind_of(url) == kind_of(br->page_url);
}

/* file: and mill: are this Mac's own: a web page (http: or https:) can't link
   into them, as browsers since keep web pages out of file: and about:. Any other link
   may change kind. Images, forms, and redirects stay with their page's own kind. */
static Boolean local_scheme(int scheme) { return scheme == SCHEME_FILE || scheme == SCHEME_MILL; }

static Boolean may_follow(const char *url)
{
    return local_scheme(scheme_of(br->page_url)) || !local_scheme(scheme_of(url));
}

static void go_typed(void)
{
    char url[300];
    long n = (*br->url_te)->teLength;
    if (n > 290) n = 290;
    memcpy(url, *(*br->url_te)->hText, n);
    url[n] = 0;
    if (!url_has_scheme(url)) {
        memmove(url + 7, url, n + 1);
        memcpy(url, "http://", 7);
    }
    go(url, true, NULL);
}

static void go_history(short delta)
{
    if (br->hist_pos + delta < 0 || br->hist_pos + delta >= br->hist_len || !may_leave("Leave this page?")) return;
    keep_form_state(false);     /* while hist_pos is still this page's */
    forms_kept = true;
    br->hist_pos += delta;
    coming_back = true;
    /* always a GET: a page that came from a POST is asked for again without its body */
    load(br->history[br->hist_pos], false, NULL, NULL, NULL);
}

/* Reload sends the Referer the page first had; Back and Forward send none */
static void reload(void)
{
    const char *h;
    if (br->hist_pos < 0) return;
    h = br->history[br->hist_pos];
    if (!may_leave("Leave this page?")) return;
    reloading = true;
    load(h, false, h + strlen(h) + 1, NULL, NULL);
}

static void go_home(void)
{
    go(setting("homepage", MILL_DEFAULT_HOME), true, NULL);
}

/* Client pull (Refresh, from the header or a META tag): the page just shown asks to be
   loaded again in secs (-1: it doesn't), or to load br->refresh_url. Back and Forward
   don't go on to another address, so they can't be trapped; this page is refreshed at
   most every REFRESH_MIN seconds. */
#define REFRESH_MIN 5
static void arm_refresh(long secs, Boolean back)
{
    char to[300];
    br->refresh_at = 0;
    if (secs < 0) return;
    if (br->refresh_url[0]) {
        size_t n;
        if (url_resolve(br->page_url, br->refresh_url, to, sizeof to) || !may_follow(to)) return;
        n = doc_part(to);
        if (n == doc_part(br->page_url) && !strncmp(to, br->page_url, n)) br->refresh_url[0] = 0;
        else if (back) return;
        else strcpy(br->refresh_url, to);
    }
    if (!br->refresh_url[0] && secs < REFRESH_MIN) secs = REFRESH_MIN;
    br->refresh_at = TickCount() + secs * 60 + 1;
}

/* Idle, in the front window: a refresh that's due, unless a field is being typed in or a
   form has unsent changes (then it waits until neither is so) */
static void refresh_due(void)
{
    char to[300];
    if (!br->refresh_at || (long)(TickCount() - br->refresh_at) < 0 || br->loading) return;
    if (br->focus >= 0 || forms_unsent()) return;
    br->refresh_at = 0;
    if (br->refresh_url[0]) {
        strcpy(to, br->refresh_url);
        load(to, true, br->page_url, NULL, NULL);
    } else {
        refresh_y = br->scroll_y;
        reload();
        refresh_y = -1;
    }
}

/* Fetch one image a page opened without */
static void load_image(short which)
{
    Response resp;
    if (!br->have_page || br->loading) return;
    resp.buf = NULL;    /* load_images makes its own */
    resp.size = 0;
    response_init(&resp);
    blur();
    set_loading(true);
    stopped = ran_out = false;
    watch_cursor();
    load_images(&resp, which);
    if (!stopped) set_status(br->pics[which].state == PIC_SHOWN ? "Done." : "Mill couldn\325t load that image.");
    set_loading(false);
    InitCursor();
    if (ran_out) out_of_memory_alert("it couldn\325t load that image.");
}

/* Resume: where the stream goes on from (resp->resume_from), and whether the part file
   holds decoded MacBinary's forks. With MacBinary decoded, the stream's first 128 bytes
   are asked for again, to learn its forks. 1 to go on, 0 to start again, -1 if that
   fetch failed (*err). */
static int resume_point(Response *r, char *target, const char *from, Url *u, Head *h, OSErr *err)
{
    FSSpec part;
    FInfo fi;
    long d, rs;
    unsigned char head[128];
    Boolean mb = decode_macbinary();
    int dec;
    saver_part_spec(&r->spec, &part);
    if (file_lengths(&part, &d, &rs) != noErr || file_get_info(&part, &fi) != noErr) return 0;
    if (mb) {
        /* its own bytes, kept: no format, no file */
        r->as_text = r->ranged = true;
        r->save_all = false;
        r->range_first = 0;
        r->range_last = 127;
        *err = fetch(target, from, NULL, NULL, r, u, h, false);
        r->as_text = r->ranged = false;
        r->save_all = true;
        if (*err != noErr || stopped) return -1;
        if (!resume_appends(r->code) || r->len - r->body < 128) return 0;
        memcpy(head, r->buf + r->body, 128);
    }
    r->resume_from = resume_at(mb ? head : NULL, (const char *)&fi.fdType, (const char *)&fi.fdCreator, d, rs, &r->rmb, &dec);
    r->resume_mb = dec != 0;
    return r->resume_from >= 0;
}

/* Fetch url into spec (chosen already), whatever it is, by way of its part file: from
   the part file's end if resume. from is the page it's on (the Referer), or NULL.
   browse: then browse the saved copy (an archive). Whether it was saved. */
static Boolean save_to(const char *url, const char *from, const FSSpec *spec, Boolean resume, Boolean browse)
{
    char then[320];
    Boolean saved = false;
    Response resp;
    char target[300];
    Url u;
    Head h;
    OSErr err = noErr;
    int go_on = 0;
    stopped = ran_out = false;
    if (!response_buffer(&resp)) { out_of_memory_alert("it couldn\325t save that."); return false; }
    response_init(&resp);
    resp.spec = *spec;
    resp.have_spec = resp.save_all = resp.part_asked = true;
    blur();
    set_loading(true);
    activity[0] = 0;
    watch_cursor();
    snprintf(target, sizeof target, "%s", url);
    if (resume && (go_on = resume_point(&resp, target, from, &u, &h, &err)) > 0) {
        resp.resume = resp.ranged = true;
        resp.range_first = resp.resume_from;
        resp.range_last = -1;
        err = fetch(target, from, NULL, NULL, &resp, &u, &h, false);
        /* the whole came instead (200), or nothing (416): start again */
        if (resp.restart || (!resp.saving && err == noErr && !stopped && resp.code == 416)) go_on = 0;
    }
    if (resume && go_on == 0) {
        response_init(&resp);
        resp.spec = *spec;
        resp.have_spec = resp.save_all = resp.part_asked = resp.restarted = true;
        snprintf(target, sizeof target, "%s", url);
    }
    if (!resume || go_on == 0) err = fetch(target, from, NULL, NULL, &resp, &u, &h, false);
    if (err == memFullErr) out_of_memory();
    if (resp.saving) saved = finish_save(&resp, err);
    else if (err == FETCH_FAILED) fail_alert(fail_text);
    else if (err == FETCH_SILENT) set_status("Not saved.");
    else if (stopped) set_status("Stopped.");
    else if (err == noErr && h.code >= 300) set_status("The server didn\325t send it.");
    else set_status("Mill couldn\325t get it.");
    free(resp.buf);
    set_loading(false);
    InitCursor();
    if (ran_out) out_of_memory_alert("it couldn\325t save that.");
    if (saved && browse && saved_url(&resp.spec, then, sizeof then)) return load(then, true, NULL, NULL, NULL);
    return saved;
}

/* Save Image, Link, or Page Source As: where, then what about a part file there, then
   save_to. Whether it was saved. */
static Boolean save_url(const char *url, const char *from, Boolean browse)
{
    FSSpec spec;
    short part;
    if (br->loading) return false;
    if (!ask_where(url, &spec)) { InitCursor(); return false; }
    if ((part = ask_part(url, &spec)) == PART_CANCEL) { InitCursor(); return false; }
    return save_to(url, from, &spec, part == PART_RESUME, browse);
}

/* Send a form, as its field by (a button, or -1 for Return in a text field) sends it */
static void submit(long form, long by, short x, short y)
{
    Submission s;
    char target[300], content_type[80];
    char *q;
    blur();
    if (forms_submit(&br->forms, &br->doc, form, by, x, y, &s)) { out_of_memory_alert("it couldn\325t send the form."); return; }
    if (url_resolve(br->page_url, s.action[0] ? s.action : br->page_url, target, sizeof target)) {
        set_status("Mill can\325t send this form to that address.");
    } else if (!same_kind(target)) {
        set_status(FORM_MSG);
    } else {
        if ((q = strchr(target, '#'))) *q = 0;
        if (s.post) {
            /* the body is made as it's sent: files are read a piece at a time */
            char why[120];
            Body *body = body_new(&s, why, sizeof why);
            if (!body) set_status(why);
            else {
                strcpy(content_type, body_type(body));
                keep_form_state(true);
                forms_kept = true;
                load(target, true, br->page_url, content_type, body);
                body_free(body);
            }
            UnloadSeg((void *)body_new);    /* sending's code, only while it's used */
        } else {
            if ((q = strchr(target, '?'))) *q = 0;
            if (strlen(target) + s.len + 2 > sizeof target) set_status("This form\325s data is too long for an address.");
            else {
                strcat(target, "?");
                strcat(target, s.data);
                keep_form_state(true);
                forms_kept = true;
                load(target, true, br->page_url, NULL, NULL);
            }
        }
    }
    submission_free(&s);
}

static void choose_file(Field *f)
{
    FSSpec spec;
    char path[300];
    if (!sys_choose_file(&spec)) return;
    spec_to_path(&spec, path, sizeof path);
    forms_set_value(f, path);
}

/* ------------------------------------------------------------ events */

static const Run *run_at(Point p)
{
    if (!br->have_page || !PtInRect(p, &br->view_r)) return NULL;
    return layout_hit(&br->lay, br->scroll_y + (p.v - br->view_r.top), p.h - PAGE_LEFT);
}

static const Run *link_at(Point p)
{
    const Run *r = run_at(p);
    return r && r->link && r->kind != RUN_CONTROL ? r : NULL;
}

/* An image not loaded yet, and not a link: a click loads it */
static const Run *waiting_image_at(Point p)
{
    const Run *r = run_at(p);
    return r && r->kind == RUN_IMAGE && !r->link && br->pic_of && br->pic_of[r->text] &&
           br->pics[br->pic_of[r->text] - 1].state == PIC_WAITING ? r : NULL;
}

static void follow(const Run *r)
{
    char target[300];
    if (url_resolve(br->page_url, br->lay.links[r->link - 1], target, sizeof target)) {
        set_status("Mill can\325t open this kind of address.");
        return;
    }
    if (!may_follow(target)) { set_status(FILE_MSG); return; }
    go(target, true, br->page_url);
}

/* A run's box in the window */
static Boolean run_box(const Run *r, Rect *box)
{
    long k = r - br->lay.runs, i;
    for (i = 0; i < br->lay.nlines; i++) {
        const MLine *ln = &br->lay.lines[i];
        if (k >= ln->first && k < ln->first + ln->count) {
            short top = br->view_r.top + (short)(ln->y - br->scroll_y) + ln->ascent - r->font;
            SetRect(box, PAGE_LEFT + r->x, top, PAGE_LEFT + r->x + r->width, top + r->size);
            return true;
        }
    }
    return false;
}

/* Where a point on an image map goes: a client-side map's AREA (USEMAP), or a link with
   the point added (ISMAP: "?x,y"). 1 with the address in out; -1 for a part of a map
   that goes nowhere; 0 if r isn't a map. */
static int map_target(Point p, const Run *r, char *out, int size)
{
    Rect box;
    long map, area;
    short x, y;
    if (!r || r->kind != RUN_IMAGE || !run_box(r, &box)) return 0;
    x = p.h - box.left - r->face;  /* in the image, inside its border */
    y = p.v - box.top - r->face;
    if (dist_zoom() > 9) { x = (short)((long)x * 9 / dist_zoom()); y = (short)((long)y * 9 / dist_zoom()); }   /* back to the image's own pixels */
    if ((map = imagemap_for(&br->doc, r->text)) >= 0) {
        if ((area = imagemap_hit(&br->doc, map, x, y)) < 0) return -1;
        return url_resolve(br->page_url, html_attr(&br->doc, &br->doc.nodes[area], "href"), out, size) ? -1 : 1;
    }
    if (r->link && html_attr(&br->doc, &br->doc.nodes[r->text], "ismap")) {
        long n;
        if (url_resolve(br->page_url, br->lay.links[r->link - 1], out, size)) return -1;
        n = strcspn(out, "?#");
        snprintf(out + n, size - n, "?%d,%d", x < 0 ? 0 : x, y < 0 ? 0 : y);
        return 1;
    }
    return 0;
}

static void go_map(const char *target)
{
    if (!may_follow(target)) set_status(FILE_MSG);
    else go(target, true, br->page_url);
}

enum { CM_LOAD_IMAGE = 1, CM_VIEW_IMAGE, CM_SAVE_IMAGE, CM_NEW_WINDOW, CM_SAVE_LINK, CM_VIEW_SOURCE, CM_SAVE_SOURCE, CM_VIEW_LINK };
/* Held this long for the menu: the double-click time, at least half a second; moved this
   far first, it's a click (as Netscape 4's context menus did) */
#define HOLD 30
#define HYSTERESIS 6

/* The menu for what's under the mouse: an image, a link, or the page itself */
static void context_menu(Point p, const Run *r)
{
    MenuHandle m = NewMenu(MENU_CONTEXT, "\p");
    short codes[6], n = 0;
    long choice;
    Point where = p;
    const char *page = !strncmp(br->page_url, VIEW_SOURCE, strlen(VIEW_SOURCE)) ? br->page_url + strlen(VIEW_SOURCE) : br->page_url;
    short pic = r && r->kind == RUN_IMAGE && br->pic_of && br->pic_of[r->text] ? br->pic_of[r->text] - 1 : -1;
    char target[320];
    if (!m || !br->have_page) return;
    if (pic >= 0) {
        AppendMenu(m, "\pLoad Image");
        codes[n++] = CM_LOAD_IMAGE;
        if (br->pics[pic].state == PIC_SHOWN) DisableItem(m, n);
        AppendMenu(m, "\pView Image");
        codes[n++] = CM_VIEW_IMAGE;
        AppendMenu(m, "\pSave Image As\311");
        codes[n++] = CM_SAVE_IMAGE;
    }
    if (r && r->link && r->kind != RUN_CONTROL) {
        AppendMenu(m, "\pOpen Link in New Window");
        codes[n++] = CM_NEW_WINDOW;
        AppendMenu(m, "\pSave Link As\311");
        codes[n++] = CM_SAVE_LINK;
        AppendMenu(m, "\pView Link as Text");
        codes[n++] = CM_VIEW_LINK;
    }
    if (!n) {
        AppendMenu(m, "\pView Source");
        codes[n++] = CM_VIEW_SOURCE;
        if (page != br->page_url) DisableItem(m, n);  /* already */
        AppendMenu(m, "\pSave Page Source As\311");
        codes[n++] = CM_SAVE_SOURCE;
    }
    InsertMenu(m, -1);
    LocalToGlobal(&where);
    /* where it wouldn't fit, the menu goes to the mouse's left (or above), clear of it: moved
       there by the Menu Manager, it would be over the mouse, and a release without moving
       would choose its first item */
    CalcMenuSize(m);
    if (where.h + (**m).menuWidth > qd.screenBits.bounds.right && where.h > (**m).menuWidth + 1) where.h -= (**m).menuWidth + 1;
    if (where.v + (**m).menuHeight > qd.screenBits.bounds.bottom && where.v > (**m).menuHeight + 21) where.v -= (**m).menuHeight + 1;
    choice = PopUpMenuSelect(m, where.v, where.h, 0);
    DeleteMenu(MENU_CONTEXT);
    DisposeMenu(m);
    if (!LoWord(choice)) return;
    switch (codes[LoWord(choice) - 1]) {
    case CM_LOAD_IMAGE:
        br->pics[pic].state = PIC_WAITING;  /* one that failed can be tried again */
        load_image(pic);
        break;
    case CM_VIEW_IMAGE:
        go(br->pics[pic].url, true, br->page_url);  /* on a page of its own */
        break;
    case CM_SAVE_IMAGE:
        save_url(br->pics[pic].url, br->page_url, false);
        break;
    case CM_NEW_WINDOW: case CM_SAVE_LINK: case CM_VIEW_LINK:
        if (url_resolve(br->page_url, br->lay.links[r->link - 1], target, sizeof target)) set_status("Mill can\325t open this kind of address.");
        else if (!may_follow(target)) set_status(FILE_MSG);
        else if (codes[LoWord(choice) - 1] == CM_SAVE_LINK) save_url(target, br->page_url, false);
        else if (codes[LoWord(choice) - 1] == CM_VIEW_LINK) {
            /* what it sends, as text: View Source, for the link */
            char source[340];
            snprintf(source, sizeof source, "%s%s", VIEW_SOURCE, target);
            go(source, true, br->page_url);
        }
        else {
            char from[300];
            strcpy(from, br->page_url);     /* the new window becomes br */
            if (new_browser((WindowPtr)-1)) go(target, true, from);
        }
        break;
    case CM_VIEW_SOURCE:
        snprintf(target, sizeof target, "%s%s", VIEW_SOURCE, page);
        go(target, true, NULL);
        break;
    case CM_SAVE_SOURCE:
        save_url(page, NULL, false);
        break;
    }
}

/* The page takes the keyboard from the address or a field, its selection cleared */
static void page_focus(void)
{
    RgnHandle save;
    hide_status();
    if (br->focus > -2) { blur(); TEDeactivate(br->url_te); br->focus = -2; }
    save = clip_to(&br->view_r);
    sel_invert(br->sel_a, br->sel_b);
    unclip(save);
    br->sel_a = br->sel_b = 0;
}

/* The Doc.text offset in the page's text nearest p, or -1 if there's none on its line */
static long sel_at(Point p)
{
    long y = br->scroll_y + (p.v - br->view_r.top), i, k;
    short x = p.h - PAGE_LEFT, n, w0 = 0, w1;
    const Run *best = NULL;
    for (i = layout_line_at(&br->lay, y); i < br->lay.nlines && br->lay.lines[i].y <= y; i++) {
        const MLine *ln = &br->lay.lines[i];
        if (y >= ln->y + ln->height) continue;
        for (k = ln->first; k < ln->first + ln->count; k++) {
            const Run *r = &br->lay.runs[k];
            if (r->kind == RUN_TEXT && r->x <= x && (!best || r->x > best->x)) best = r;
        }
    }
    if (!best) return -1;
    for (n = 0; n < best->len; n++, w0 = w1)
        if ((w1 = run_width(best, n + 1)) + w0 > 2 * (x - best->x)) break;
    return best->text + n;
}

/* Select text from p while the button is down; the page scrolls at the view's edges */
static void drag_select(Point p)
{
    Rect v = br->view_r;
    unsigned long t = 0;
    long at;
    RgnHandle save;
    page_focus();
    br->sel_a = br->sel_b = sel_at(p);
    while (StillDown()) {
        GetMouse(&p);
        if (TickCount() - t > 2) {
            t = TickCount();
            scroll_to(br->scroll_y + (p.v <= v.top ? -16 : p.v >= v.bottom - 1 ? 16 : 0));
            scroll_x_to(br->scroll_x + (p.h <= v.left ? -16 : p.h >= v.right - 1 ? 16 : 0));
        }
        if (p.v < v.top) p.v = v.top;
        if (p.v >= v.bottom) p.v = v.bottom - 1;
        if ((at = sel_at(p)) < 0) continue;
        if (br->sel_a < 0) br->sel_a = br->sel_b = at;
        save = clip_to(&v);
        sel_invert(br->sel_b, at);
        unclip(save);
        br->sel_b = at;
    }
    if (br->sel_a < 0) br->sel_a = br->sel_b = 0;
}

/* Copy the page's selected text: a return where a line starts again at the left, a space for a gap */
static void copy_sel(void)
{
    long a = br->sel_a < br->sel_b ? br->sel_a : br->sel_b, b = br->sel_a + br->sel_b - a, k, n = 0, s, e;
    short px = 0;
    char *buf = malloc(b - a + br->lay.nruns), *lm = (char *)0xAB0;
    if (!buf) return;
    for (k = 0; k < br->lay.nruns; k++) {
        const Run *r = &br->lay.runs[k];
        s = a > r->text ? a : r->text;
        e = b < r->text + r->len ? b : r->text + r->len;
        if (r->kind != RUN_TEXT || s >= e) continue;
        if (n && r->x != px) buf[n++] = r->x < px ? '\r' : ' ';
        memcpy(buf + n, br->doc.text + s, e - s);
        n += e - s;
        px = r->x + r->width;
    }
    ZeroScrap();
    PutScrap(n, 'TEXT', buf);
    __asm__("" : "+a"(lm));         /* TEScrpLength and TEScrpHandle, hidden from GCC */
    PtrToXHand(buf, *(Handle *)(lm + 4), n);     /* TextEdit's scrap too, for pasting here */
    *(short *)lm = n;
    free(buf);
}

/* A click on the page: a link opens and an image waiting to load loads, when the button
   comes up over it; held down, or with Control, it's the menu */
static void page_click(Point p, EventRecord *ev)
{
    const Run *r = run_at(p);
    Point now;
    if (!(ev->modifiers & ControlKey)) {
        long *dbl = (long *)0x2F0;     /* DoubleTime: GetDblTime is this low-memory read */
        unsigned long hold, start = ev->when;
        Boolean moved = false;
        __asm__("" : "+a"(dbl));       /* hidden from GCC, which flags a fixed address */
        hold = *dbl > HOLD ? *dbl : HOLD;
        if (TickCount() - start > 6) start = TickCount();  /* a press that waited: from now */
        while (StillDown() && TickCount() - start < hold) {
            GetMouse(&now);
            if (now.h - p.h >= HYSTERESIS || p.h - now.h >= HYSTERESIS ||
                now.v - p.v >= HYSTERESIS || p.v - now.v >= HYSTERESIS) { moved = true; break; }
        }
        if (moved) { drag_select(p); return; }
        if (!StillDown()) {
            page_focus();
            GetMouse(&now);
            if (r && run_at(now) == r) {
                char target[300];
                int m;
                /* an image that's waiting loads only from the context menu, so a click on
                   a linked one follows the link (its map, if any, isn't drawn yet) */
                if (waiting_image_at(p)) { if (r->link) follow(r); }
                else if ((m = map_target(p, r, target, sizeof target))) { if (m > 0) go_map(target); }
                else if (link_at(p)) follow(r);
            }
            return;
        }
    }
    context_menu(p, r);
}

static pascal void scroll_action(ControlHandle c, short part)
{
    long page = br->view_r.bottom - br->view_r.top - 16, line = 16;
    if (c == br->hscroll) {
        page = br->view_r.right - br->view_r.left - 16;
        switch (part) {
        case inUpButton: scroll_x_to(br->scroll_x - line); break;
        case inDownButton: scroll_x_to(br->scroll_x + line); break;
        case inPageUp: scroll_x_to(br->scroll_x - page); break;
        case inPageDown: scroll_x_to(br->scroll_x + page); break;
        }
        return;
    }
    switch (part) {
    case inUpButton: scroll_to(br->scroll_y - line); break;
    case inDownButton: scroll_to(br->scroll_y + line); break;
    case inPageUp: scroll_to(br->scroll_y - page); break;
    case inPageDown: scroll_to(br->scroll_y + page); break;
    }
}

/* Track a button like a Mac button: inverted while the mouse is inside */
static Boolean press(const Rect *r)
{
    Boolean inside = true;
    InvertRect(r);
    while (StillDown()) {
        Point p;
        GetMouse(&p);
        if (PtInRect(p, r) != inside) { inside = !inside; InvertRect(r); }
    }
    if (inside) InvertRect(r);
    return inside;
}

static void choose_option(Field *f, const Rect *box)
{
    MenuHandle m = NewMenu(MENU_POPUP, "\p");
    short i, current = 1;
    long choice;
    Point where;
    char label[100];
    if (!m) return;
    for (i = 0; i < f->noptions; i++) {
        Str255 item;
        forms_option_label(&br->doc, f->options[i], label, sizeof label);
        item[0] = (unsigned char)strlen(label);
        memcpy(item + 1, label, item[0]);
        AppendMenu(m, "\p ");
        SetMenuItemText(m, i + 1, item);  /* as is: no menu metacharacters */
        if (f->selected[i]) { CheckItem(m, i + 1, true); if (current == 1) current = i + 1; }
    }
    InsertMenu(m, -1);
    where.h = box->left;
    where.v = box->top;
    LocalToGlobal(&where);
    choice = PopUpMenuSelect(m, where.v, where.h, current);
    if (LoWord(choice)) {
        short k = LoWord(choice) - 1;
        if (f->multiple) f->selected[k] = !f->selected[k];
        else { memset(f->selected, 0, f->noptions); f->selected[k] = 1; }
    }
    DeleteMenu(MENU_POPUP);
    DisposeMenu(m);
    draw_view();
}

/* A click in text area i's scroll bar at p: a line at a time on the arrows, a view at a
   time on the track, for as long as the button is held. The area takes the typing. */
static void area_scroll(long i, Point p)
{
    Field *f = &br->forms.fields[i];
    Rect box, inner, bar;
    long view, most, by, wait;
    if (br->focus != i) {
        focus_field(i, NULL, false);
        TEAutoView(false, br->field_te);
        TESetSelect(0, 0, br->field_te);
        TEAutoView(true, br->field_te);
    }
    if (!control_box(f->node, &box)) return;
    inner = text_part(f, box);
    bar = area_bar(&box);
    view = inner.bottom - inner.top;
    most = area_height(f, &inner) - view;
    if (most <= 0) return;
    if (p.v < bar.top + AREA_BAR) by = -(*br->field_te)->lineHeight;
    else if (p.v >= bar.bottom - AREA_BAR) by = (*br->field_te)->lineHeight;
    else {
        long top = (*br->field_te)->viewRect.top - (*br->field_te)->destRect.top;
        short thumb = bar.top + AREA_BAR + (short)((bar.bottom - bar.top - 3 * AREA_BAR) * top / most);
        by = (view / (*br->field_te)->lineHeight - 1) * (*br->field_te)->lineHeight;
        if (by < (*br->field_te)->lineHeight) by = (*br->field_te)->lineHeight;
        if (p.v < thumb) by = -by;
    }
    for (wait = 15; ; wait = 4) {   /* a pause before it repeats, as the Mac's scroll bars */
        long top = (*br->field_te)->viewRect.top - (*br->field_te)->destRect.top, to = top + by;
        unsigned long until = TickCount() + wait;
        RgnHandle save;
        if (to < 0) to = 0;
        if (to > most) to = most;
        if (to != top) {
            save = clip_to(&br->focus_clip);
            TEScroll(0, (short)(top - to), br->field_te);
            unclip(save);
            area_bar_update(f);
        }
        while (TickCount() < until && StillDown()) {}
        if (!StillDown()) break;
    }
}

/* A click on a form control: true if it was one */
static Boolean click_control(Point p, EventRecord *ev)
{
    const Run *r = run_at(p);
    long i;
    Field *f;
    Rect box;
    if (!r || r->kind != RUN_CONTROL || (i = forms_find(&br->forms, r->text)) < 0 || !control_box(r->text, &box)) return false;
    f = &br->forms.fields[i];
    if (f->disabled) return true;
    if (f->type == FIELD_TEXTAREA) {
        Rect bar = area_bar(&box);
        if (PtInRect(p, &bar)) { area_scroll(i, p); return true; }
    }
    switch (f->type) {
    case FIELD_CHECKBOX: case FIELD_RADIO:
        if (press(&box)) { forms_click(&br->forms, i); draw_view(); }
        break;
    case FIELD_SUBMIT: case FIELD_IMAGE:
        if (press(&box)) submit(f->form, i, p.h - box.left, p.v - box.top);
        break;
    case FIELD_RESET:
        if (press(&box)) {
            if (br->focus >= 0 && br->forms.fields[br->focus].form == f->form) { TEDeactivate(br->field_te); br->focus = -1; TEActivate(br->url_te); }
            forms_reset(&br->forms, &br->doc, f->form);
            draw_view();
        }
        break;
    case FIELD_SELECT:
        if (list_rows(f) && f->noptions <= list_rows(f)) {
            short row = (p.v - box.top - CD(2)) / LINE_H;
            if (row >= 0 && row < f->noptions) {
                if (f->multiple) f->selected[row] = !f->selected[row];
                else { memset(f->selected, 0, f->noptions); f->selected[row] = 1; }
                draw_view();
            }
        } else {
            choose_option(f, &box);  /* a pop-up menu, or a list too long to show */
        }
        break;
    case FIELD_FILE: {
        Rect button = box;
        button.left = text_part(f, box).right + CD(6);
        if (PtInRect(p, &button) && press(&button)) { choose_file(f); draw_view(); }
        break;
    }
    default:
        focus_field(i, &p, (ev->modifiers & shiftKey) != 0);
    }
    return true;
}

static void open_file(void)
{
    FSSpec spec;
    char url[400];
    if (!sys_choose_file(&spec)) return;
    spec_to_url(&spec, url, sizeof url);
    if (need()) go(url, true, NULL);
}

/* ------------------------------------------------------------ printing */

static THPrint print_rec;       /* Page Setup's choices, for every window, while Mill runs */
static char print_what[100], print_step[60];
static short print_pct;
#define PRINT_EDGE 16           /* room for the header line, and for the footer's */
#define PR_PG_MAX 9999          /* the Printing Manager's last page (iPrPgMax) */

/* The window that says how printing is going: what, which step, and a bar */
static void draw_print_win(void)
{
    Rect bar;
    SetPort(print_win);
    EraseRect(&print_win->portRect);
    TextFont(0);
    TextSize(12);
    MoveTo(16, 24);
    DrawText(print_what, 0, strlen(print_what));
    TextFont(kFontIDGeneva);
    TextSize(9);
    MoveTo(16, 42);
    DrawText(print_step, 0, strlen(print_step));
    SetRect(&bar, 16, 50, print_win->portRect.right - 16, 62);
    FrameRect(&bar);
    InsetRect(&bar, 1, 1);
    bar.right = bar.left + (short)((long)(bar.right - bar.left) * print_pct / 100);
    PaintRect(&bar);
    MoveTo(16, 80);
    DrawText("Command-period stops printing.", 0, 30);
}

static void print_progress(const char *step, int percent)
{
    GrafPtr old;
    snprintf(print_step, sizeof print_step, "%s", step);
    print_pct = (short)percent;
    if (!print_win) return;
    GetPort(&old);
    draw_print_win();
    SetPort(old);
}

static int print_laying_out(void *ctx, int percent)
{
    char msg[32];
    (void)ctx;
    sprintf(msg, "Laying out\311 %d%%", percent);
    print_progress(msg, percent);
    return stop_asked();
}

/* The Printing Manager's record, made the first time; false if there's no printer
   driver (none chosen in the Chooser). PrOpen comes first. */
static Boolean print_ready(void)
{
    if (PrError() != noErr) return false;
    if (!print_rec) {
        if (!(print_rec = (THPrint)NewHandleClear(sizeof(TPrint)))) return false;
        PrintDefault(print_rec);
    } else {
        PrValidate(print_rec);
    }
    return PrError() == noErr;
}

static void print_trouble(OSErr err)
{
    char msg[100];
    if (err == iPrAbort) strcpy(msg, "Printing stopped.");
    else if (err == fnfErr || err == resNotFound) strcpy(msg, "Choose a printer in the Chooser first.");
    else if (err == memFullErr) strcpy(msg, "Out of memory.");
    else snprintf(msg, sizeof msg, "Mill couldn\325t print this page (error %d).", err);
    if (br) set_status(msg);
    else SysBeep(10);
}

static void page_setup(void)
{
    PrOpen();
    if (print_ready()) { InitCursor(); PrStlDialog(print_rec); }
    else print_trouble(PrError());
    PrClose();
    if (br) SetPort(br->win);
}

/* Where each printed page starts, in page pixels, and how many there are: a page ends
   above the first line that wouldn't fit whole, unless that line is taller than a page */
static long *paginate(const Layout *L, long body, short *npages)
{
    long *starts = NULL, y = 0, bottom = 0, i;
    short n = 0;
    for (i = 0; i < L->nlines; i++) if (L->lines[i].y + L->lines[i].height > bottom) bottom = L->lines[i].y + L->lines[i].height;
    for (i = 0; i < L->nboxes; i++) if (L->boxes[i].y + L->boxes[i].h > bottom) bottom = L->boxes[i].y + L->boxes[i].h;
    do {
        long end = y + body, cut = end, *more;
        if (!(more = realloc(starts, (n + 2) * sizeof(long)))) { free(starts); return NULL; }
        starts = more;
        starts[n++] = y;
        for (i = layout_line_at(L, y); i < L->nlines && L->lines[i].y < end; i++) {
            const MLine *ln = &L->lines[i];
            if (ln->y > y && ln->y + ln->height > end && ln->y < cut) cut = ln->y;
        }
        y = cut;
    } while (y < bottom);
    starts[n] = y;  /* where the last page ends */
    *npages = n;
    return starts;
}

/* Text cut to fit between left and right, with an ellipsis if it had to be */
static void draw_fit(const char *t, short left, short right, short base)
{
    short n = (short)strlen(t), room = right - left;
    MoveTo(left, base);
    if (TextWidth((Ptr)t, 0, n) <= room) { DrawText((Ptr)t, 0, n); return; }
    room -= CharWidth((char)0xC9);
    while (n > 0 && TextWidth((Ptr)t, 0, n) > room) n--;
    DrawText((Ptr)t, 0, n);
    DrawChar((char)0xC9);
}

/* One printed page, number n of the laid-out page play, on paper: the window's drawing,
   pointed at the printer's port for a moment */
static void print_one(Layout *play, const long *starts, short n, short npages, const Rect *paper, short edge)
{
    Layout lay = br->lay;
    Rect view = br->view_r;
    long sy = br->scroll_y, sx = br->scroll_x;
    Boolean up = br->status_up;
    long focus = br->focus;
    br->lay = *play;
    br->focus = -1;     /* a field being typed in prints as text, not through TextEdit */
    SetRect(&br->view_r, paper->left, paper->top + edge, paper->right, paper->top + edge + (short)(starts[n] - starts[n - 1]));
    br->scroll_y = starts[n - 1];
    br->scroll_x = 0;
    br->status_up = false;
    plain_cells = !print_backgrounds();
    draw_page(-1);
    plain_cells = false;
    br->lay = lay;
    br->view_r = view;
    br->scroll_y = sy;
    br->scroll_x = sx;
    br->status_up = up;
    br->focus = focus;
    if (edge) {
        char line[60];
        Str255 date;
        unsigned long now;
        short w;
        TextFont(kFontIDGeneva);
        TextSize(9);
        TextFace(0);
        draw_fit(br->doc.title[0] ? br->doc.title : br->page_url, paper->left, paper->right, paper->top + 9);
        ReadDateTime(&now);
        IUDateString(now, abbrevDate, date);
        snprintf(line, sizeof line, "%.*s   Page %d of %d", date[0], (char *)date + 1, n, npages);
        w = TextWidth(line, 0, strlen(line));
        MoveTo(paper->right - w, paper->bottom - 3);
        DrawText(line, 0, strlen(line));
        draw_fit(br->page_url, paper->left, paper->right - w - 12, paper->bottom - 3);
    }
}

/* Print the front window's page, laid out again for the paper */
static void print_page(void)
{
    TPPrPort port;
    TPrStatus st;
    Layout play;
    long *starts = NULL;
    short npages = 0, first, last, n, edge;
    Rect paper, r;
    int err;
    if (!br->have_page || br->loading) return;
    keep_typing();
    PrOpen();
    if (!print_ready()) { print_trouble(PrError()); PrClose(); return; }
    InitCursor();
    if (!PrJobDialog(print_rec)) { PrClose(); SetPort(br->win); return; }
    /* Mill prints only the pages asked for, so the Printing Manager shouldn't skip any */
    first = (**print_rec).prJob.iFstPage;
    last = (**print_rec).prJob.iLstPage;
    (**print_rec).prJob.iFstPage = 1;
    (**print_rec).prJob.iLstPage = PR_PG_MAX;
    if (first < 1) first = 1;
    paper = (**print_rec).prInfo.rPage;
    edge = print_headers() ? PRINT_EDGE : 0;

    r = qd.screenBits.bounds;
    SetRect(&r, (r.right - 300) / 2, 20 + (r.bottom - 20 - 96) / 3, (r.right - 300) / 2 + 300, 20 + (r.bottom - 20 - 96) / 3 + 96);
    snprintf(print_what, sizeof print_what, "Printing \322%.60s\323", br->doc.title[0] ? br->doc.title : br->page_url);
    print_step[0] = 0;
    print_pct = 0;
    print_win = NewWindow(NULL, &r, "\p", true, dBoxProc, (WindowPtr)-1, false, 0);
    set_loading(true);   /* the stop sign stops printing too */
    stopped = false;
    watch_cursor();

    /* laid out with the screen's fonts, as the Mac's printer drivers expect */
    SetPort(br->win);
    qd_m.progress = print_laying_out;
    qd_m.zoom = 0;          /* a sheet is at normal size */
    err = layout_doc(&play, &br->doc, &qd_m, paper.right - paper.left);
    qd_m.zoom = zoom_step ? LADDER[zoom_step] : 0;
    qd_m.progress = NULL;
    if (!err && !(starts = paginate(&play, paper.bottom - paper.top - 2 * edge, &npages))) err = -1;
    if (err) {
        layout_free(&play);
        PrClose();
        if (print_win) { DisposeWindow(print_win); print_win = NULL; }
        set_loading(false);
        InitCursor();
        SetPort(br->win);
        set_status(err == -2 ? "Printing stopped." : "Not enough memory to print this page.");
        return;
    }
    if (last > npages) last = npages;

    port = PrOpenDoc(print_rec, NULL, NULL);
    if (PrError() == noErr) {
        for (n = first; n <= last && PrError() == noErr; n++) {
            char msg[40];
            sprintf(msg, "Page %d of %d", n, npages);
            print_progress(msg, (int)((long)(n - first) * 100 / (last - first + 1)));
            if (stop_asked()) { PrSetError(iPrAbort); break; }
            PrOpenPage(port, NULL);
            if (PrError() == noErr) print_one(&play, starts, n, npages, &paper, edge);
            PrClosePage(port);
        }
    }
    PrCloseDoc(port);
    if (PrError() == noErr && (**print_rec).prJob.bJDocLoop == bSpoolLoop) {
        /* a spooled job (the ImageWriter's best quality) prints now */
        print_progress("Printing\311", 100);
        PrPicFile(print_rec, NULL, NULL, NULL, &st);
    }
    err = PrError();
    PrClose();
    layout_free(&play);
    free(starts);
    if (print_win) DisposeWindow(print_win);
    print_win = NULL;
    set_loading(false);
    InitCursor();
    SetPort(br->win);
    if (err == noErr) set_status("Printed.");
    else print_trouble(err);
}

static void leave_full(void);

/* Before quitting: each window with a changed form asks, in front */
static Boolean may_quit(void)
{
    Browser *x;
    for (x = browsers; x; x = x->next) {
        use(x);
        keep_typing();
        if (x->have_page && x->forms.n && forms_changed(&x->forms, &x->doc)) {
            if (x != full) leave_full();    /* another window comes in front to ask */
            SelectWindow(x->win);
            redraw();
            if (!may_leave("Quit Mill?")) { use(front()); return false; }
        }
    }
    use(front());
    return true;
}

/* ------------------------------------------------------------ bookmarks */

enum { BD_OK = 1, BD_CANCEL, BD_SLOT_LABEL, BD_SLOT, BD_TITLE_LABEL, BD_TITLE, BD_URL_LABEL, BD_URL, BD_RING, BD_LOGIN };
static short bd_slot;   /* the slot the dialog shows */

/* "1  Title", or "1  (empty)": a slot as the menus show it */
static void slot_text(int n, Str255 out)
{
    char url[300], title[100], line[80];
    if (!bookmark(n, url, sizeof url, title, sizeof title)) sprintf(line, "%d  (empty)", n == 9 ? 0 : n + 1);
    else {
        url_strip_login(url);   /* the menu doesn't show a password */
        snprintf(line, sizeof line, "%d  %.40s%s", n == 9 ? 0 : n + 1, title[0] ? title : url, strlen(title[0] ? title : url) > 40 ? "\xC9" : "");
    }
    out[0] = (unsigned char)strlen(line);
    memcpy(out + 1, line, out[0]);
}

static void item_rect(DialogPtr d, short item, Rect *r)
{
    short type;
    Handle h;
    GetDialogItem(d, item, &type, &h, r);
}

/* The slot as a pop-up menu: a box and the arrow, like the pop-ups on pages */
static pascal void draw_slot(DialogPtr d, short item)
{
    Rect r;
    Str255 s;
    item_rect(d, item, &r);
    r.right -= 1;
    r.bottom -= 1;
    EraseRect(&r);
    FrameRect(&r);
    {
        short i, x = r.right - 12;
        for (i = 0; i < 4; i++) { MoveTo(x - (3 - i), r.top + 6 + i); LineTo(x + (3 - i), r.top + 6 + i); }
    }
    slot_text(bd_slot, s);
    {
        RgnHandle save = NewRgn();
        Rect in = r;
        in.right -= 18;
        GetClip(save);
        ClipRect(&in);
        MoveTo(r.left + 6, r.bottom - 5);
        DrawString(s);
        SetClip(save);
        DisposeRgn(save);
    }
}

/* The ring around OK that says Return presses it */
static pascal void draw_ring(DialogPtr d, short item)
{
    Rect r;
    item_rect(d, item, &r);
    PenSize(3, 3);
    FrameRoundRect(&r, 16, 16);
    PenNormal();
}

/* Return and Enter are OK; Escape and Command-period are Cancel */
static pascal Boolean bd_filter(DialogPtr d, EventRecord *e, short *item)
{
    (void)d;
    if (e->what == keyDown || e->what == autoKey) {
        char c = e->message & charCodeMask;
        if (c == '\r' || c == 3) { *item = BD_OK; return true; }
        if (c == 27 || (c == '.' && (e->modifiers & cmdKey))) { *item = BD_CANCEL; return true; }
    }
    return false;
}

static void choose_slot(DialogPtr d)
{
    MenuHandle m = NewMenu(MENU_POPUP, "\p");
    Rect r;
    Point where;
    long choice;
    short n;
    if (!m) return;
    for (n = 0; n < BOOKMARKS; n++) {
        Str255 s;
        slot_text(n, s);
        AppendMenu(m, "\p ");
        SetMenuItemText(m, n + 1, s);
    }
    CheckItem(m, bd_slot + 1, true);
    InsertMenu(m, -1);
    SetPort(d);  /* LocalToGlobal below goes by the current port */
    item_rect(d, BD_SLOT, &r);
    where.h = r.left;
    where.v = r.top;
    LocalToGlobal(&where);
    choice = PopUpMenuSelect(m, where.v, where.h, bd_slot + 1);
    DeleteMenu(MENU_POPUP);
    DisposeMenu(m);
    if (LoWord(choice)) bd_slot = LoWord(choice) - 1;
    InvalRect(&r);
}

static void pstr_of(const char *c, Str255 p)
{
    long n = strlen(c);
    if (n > 255) n = 255;
    p[0] = (unsigned char)n;
    memcpy(p + 1, c, n);
}

/* Command-D, or Option-Command and a number: keep this page in a slot (slot < 0: the
   first empty one), with its title and address to change first, and the name and
   password given for it too (as user:password@ in the address) if the box is checked */
static void bookmark_dialog(short slot)
{
    DialogPtr d;
    short item, type, n;
    Handle h;
    Rect r;
    Str255 s;
    char url[300], title[100], msg[80];
    Cred *login = NULL;
    if (!br->page_url[0]) return;
    if (br->page_login[0]) login = auth_find(&keyring, br->page_login);
    blur();
    if (slot < 0) {
        for (slot = 0; slot < BOOKMARKS && bookmark(slot, url, sizeof url, title, sizeof title); slot++) {}
        if (slot == BOOKMARKS) slot = 0;
    }
    bd_slot = slot;
    if (!(d = GetNewDialog(200, NULL, (WindowPtr)-1))) return;
    center_dialog(d);
    GetDialogItem(d, BD_SLOT, &type, &h, &r);
    SetDialogItem(d, BD_SLOT, type, (Handle)(UserItemUPP)draw_slot, &r);
    GetDialogItem(d, BD_RING, &type, &h, &r);
    SetDialogItem(d, BD_RING, type, (Handle)(UserItemUPP)draw_ring, &r);
    pstr_of(br->doc.title[0] ? br->doc.title : br->page_url, s);
    GetDialogItem(d, BD_TITLE, &type, &h, &r);
    SetDialogItemText(h, s);
    pstr_of(br->page_url, s);
    GetDialogItem(d, BD_URL, &type, &h, &r);
    SetDialogItemText(h, s);
    GetDialogItem(d, BD_LOGIN, &type, &h, &r);
    if (!login) HiliteControl((ControlHandle)h, 255);   /* nothing given for this page */
    SelectDialogItemText(d, BD_TITLE, 0, 32767);
    ShowWindow(d);
    InitCursor();
    do {
        ModalDialog((ModalFilterUPP)bd_filter, &item);
        if (item == BD_SLOT) choose_slot(d);
        if (item == BD_LOGIN) {
            GetDialogItem(d, BD_LOGIN, &type, &h, &r);
            SetControlValue((ControlHandle)h, !GetControlValue((ControlHandle)h));
        }
    } while (item != BD_OK && item != BD_CANCEL);
    if (item == BD_OK) {
        Boolean keep;
        GetDialogItem(d, BD_LOGIN, &type, &h, &r);
        keep = login && GetControlValue((ControlHandle)h);
        GetDialogItem(d, BD_TITLE, &type, &h, &r);
        GetDialogItemText(h, s);
        sprintf(title, "%.*s", s[0] < 99 ? s[0] : 99, (char *)s + 1);
        GetDialogItem(d, BD_URL, &type, &h, &r);
        GetDialogItemText(h, s);
        sprintf(url, "%.*s", s[0], (char *)s + 1);
        if (keep && url[0]) {
            char with[300];
            if (url_with_login(url, login->user, login->pass, with, sizeof with)) {
                DisposeDialog(d);
                SetPort(br->win);
                set_status("That address is too long to save with a name and password.");
                return;
            }
            strcpy(url, with);
            memset(with, 0, sizeof with);
        }
    }
    DisposeDialog(d);
    SetPort(br->win);
    if (item != BD_OK) return;
    n = bd_slot == 9 ? 0 : bd_slot + 1;
    if (url[0]) {
        Url u;
        if (url_parse(url, &u)) { set_status("That isn\325t an address Mill can open."); return; }
    }
    if (bookmark_set(bd_slot, url, title) != noErr) sprintf(msg, "Mill couldn\325t save its bookmarks.");
    else if (url[0]) sprintf(msg, "Bookmarked as Command-%d.", n);
    else sprintf(msg, "Bookmark %d is empty now.", n);
    set_status(msg);
}

static void open_bookmark(short n)
{
    char url[300], title[100];
    if (bookmark(n, url, sizeof url, title, sizeof title) && need()) go(url, true, NULL);
}

/* Find: what to look for, kept for Find Again */
static Str255 find_what;
enum { FD_FIND = 1, FD_CANCEL, FD_LABEL, FD_TEXT, FD_RING };

/* Ask what to find; false for Cancel */
static Boolean find_dialog(void)
{
    DialogPtr d;
    short item, type;
    Handle h;
    Rect r;
    HiliteMenu(0);
    if (!(d = GetNewDialog(208, NULL, (WindowPtr)-1))) return false;
    center_dialog(d);
    GetDialogItem(d, FD_RING, &type, &h, &r);
    SetDialogItem(d, FD_RING, type, (Handle)(UserItemUPP)draw_ring, &r);
    GetDialogItem(d, FD_TEXT, &type, &h, &r);
    SetDialogItemText(h, find_what);
    SelectDialogItemText(d, FD_TEXT, 0, 32767);
    ShowWindow(d);
    InitCursor();
    do ModalDialog((ModalFilterUPP)bd_filter, &item); while (item > FD_CANCEL);
    if (item == FD_FIND) GetDialogItemText(h, find_what);
    DisposeDialog(d);
    SetPort(br->win);
    return item == FD_FIND;
}

/* A character as Find compares it: letters in either case alike, and any white space a space */
static unsigned char fold(unsigned char c)
{
    return c <= ' ' ? ' ' : c >= 'A' && c <= 'Z' ? c | 32 : c;
}

/* Select n characters at Doc.text offset at, in run r on line ln, and scroll to them:
   across only if they don't show from the left, down to a third of the way if they're not in view */
static void find_show(const MLine *ln, const Run *r, long at, long n)
{
    long y = br->scroll_y, x = br->scroll_x, h = br->view_r.bottom - br->view_r.top,
         w = br->view_r.right - br->view_r.left, px = r->x + run_width(r, at - r->text);
    RgnHandle save;
    page_focus();
    br->sel_a = at;
    br->sel_b = at + n;
    scroll_x_to(px < w - 32 ? 0 : px < x || px >= x + w - 16 ? px - 16 : x);
    if (ln->y < y || ln->y + ln->height > y + h) scroll_to(ln->y - h / 3);
    if (y == br->scroll_y && x == br->scroll_x) {   /* no redraw: just show the selection */
        save = clip_to(&br->view_r);
        sel_invert(at, at + n);
        unclip(save);
    }
}

/* Select the next place on the page that has find_what, after the selection (or from
   the top), going round once. A beep if there's none. */
static void find_next(void)
{
    long n = find_what[0], from = 0, at, i, k, end = br->doc.text_len - n;
    short pass;
    if (br->focus < -1) from = br->sel_a > br->sel_b ? br->sel_a : br->sel_b;
    for (pass = 0; pass < 2 && n && br->have_page; pass++, from = 0)
        for (at = from; at <= end; at++) {
            for (i = 0; i < n && fold(br->doc.text[at + i]) == fold(find_what[i + 1]); i++) {}
            if (i < n) continue;
            for (i = 0; i < br->lay.nlines; i++) {   /* only text that shows */
                const MLine *ln = &br->lay.lines[i];
                for (k = ln->first; k < ln->first + ln->count; k++) {
                    const Run *r = &br->lay.runs[k];
                    if (r->kind == RUN_TEXT && at >= r->text && at < r->text + r->len) { find_show(ln, r, at, n); return; }
                }
            }
        }
    SysBeep(10);
}

/* The menus as they are now: the bookmark slots, and what needs a window */
static void refresh_menus(void)
{
    MenuHandle m = GetMenuHandle(MENU_BOOKMARKS);
    char url[300], title[100];
    short n;
    for (n = 0; n < BOOKMARKS; n++) {
        Str255 s;
        if (bookmark(n, url, sizeof url, title, sizeof title)) {
            url_strip_login(url);   /* a menu doesn't show a password */
            snprintf((char *)s + 1, 254, "%.40s%s", title[0] ? title : url, strlen(title[0] ? title : url) > 40 ? "\xC9" : "");
            s[0] = (unsigned char)strlen((char *)s + 1);
            EnableItem(m, BM_FIRST + n);
        } else {
            pstr_of("Empty", s);
            DisableItem(m, BM_FIRST + n);
        }
        SetMenuItemText(m, BM_FIRST + n, s);
    }
    if (br && br->page_url[0]) EnableItem(m, BM_ADD);
    else DisableItem(m, BM_ADD);
    {
        WindowPtr w = FrontWindow();
        Boolean da = w && ((WindowPeek)w)->windowKind < 0;
        MenuHandle file = GetMenuHandle(MENU_FILE), edit = GetMenuHandle(MENU_EDIT), nav = GetMenuHandle(MENU_GO);
        short i;
        if (br || da) EnableItem(file, FILE_CLOSE); else DisableItem(file, FILE_CLOSE);
        if (br && br->have_page) EnableItem(file, FILE_PRINT); else DisableItem(file, FILE_PRINT);
        for (i = EDIT_CUT; i <= EDIT_PASTE; i++) if (br || da) EnableItem(edit, i); else DisableItem(edit, i);
        /* Select All: the address or the field being typed in (a password's text has no selection) */
        if (br && !da && !(br->focus >= 0 && br->forms.fields[br->focus].type == FIELD_PASSWORD)) EnableItem(edit, EDIT_SELECT_ALL); else DisableItem(edit, EDIT_SELECT_ALL);
        MenuHandle view = GetMenuHandle(MENU_VIEW);
        for (i = GO_BACK; i <= GO_RELOAD; i++) if (br) EnableItem(nav, i); else DisableItem(nav, i);
        if (br && br->loading) EnableItem(nav, GO_STOP); else DisableItem(nav, GO_STOP);
        if (br && zoom_step < 6) EnableItem(view, VIEW_LARGER); else DisableItem(view, VIEW_LARGER);
        if (br && zoom_step > 0) EnableItem(view, VIEW_SMALLER); else DisableItem(view, VIEW_SMALLER);
        if (br && !da && br->have_page) EnableItem(edit, EDIT_FIND); else DisableItem(edit, EDIT_FIND);
        if (br && !da && br->have_page && find_what[0]) EnableItem(edit, EDIT_FIND_AGAIN); else DisableItem(edit, EDIT_FIND_AGAIN);
        if (br && br->have_page) EnableItem(view, VIEW_PAGE_SOURCE); else DisableItem(view, VIEW_PAGE_SOURCE);
        if (br) EnableItem(view, VIEW_FULL_SCREEN); else DisableItem(view, VIEW_FULL_SCREEN);
    }
}

/* The front window, or a new one when there's none; NULL if there's no memory for it */
static Browser *need(void)
{
    if (!br) use(new_browser((WindowPtr)-1));
    return br;
}

/* Option-Command and a number key: which slot (0 for 1 ... 9 for 0), or -1. By key code,
   since Option changes the character; with the characters as a fallback. */
static short option_digit(const EventRecord *ev)
{
    static const unsigned char codes[BOOKMARKS] = { 0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19, 0x1D };
    static const unsigned char chars[BOOKMARKS] = { 0xC1, 0xAA, 0xA3, 0xA2, 0xB0, 0xA4, 0xA6, 0xA5, 0xBB, 0xBC };
    unsigned char code = (unsigned char)((ev->message & keyCodeMask) >> 8), c = (unsigned char)(ev->message & charCodeMask);
    short n;
    for (n = 0; n < BOOKMARKS; n++) if (codes[n] == code || chars[n] == c) return n;
    return -1;
}

static TEHandle typing_te(void)
{
    return br->focus >= 0 && br->forms.fields[br->focus].type != FIELD_PASSWORD ? br->field_te : br->url_te;
}

static void zoom_by(int dir);
static void full_screen(Boolean on);

static void do_menu(long choice)
{
    short menu = HiWord(choice), item = LoWord(choice);
    WindowPtr w = FrontWindow();
    if (menu == MENU_EDIT && item <= EDIT_PASTE && SystemEdit(item - 1)) { HiliteMenu(0); return; }  /* a desk accessory's */
    if (menu == MENU_APPLE) {
        if (item == 1) {
            if (need()) go("mill://about", true, NULL);
        } else {
            Str255 name;
            GetMenuItemText(GetMenuHandle(MENU_APPLE), item, name);
            OpenDeskAcc(name);
        }
    } else if (menu == MENU_FILE) {
        if (item == FILE_NEW || item == FILE_LOCATION) leave_full();   /* the window, or the address, shows */
        if (item == FILE_NEW) { Browser *x = new_browser((WindowPtr)-1); if (x) { use(x); go_home(); } }
        if (item == FILE_LOCATION && need()) { blur(); TESetSelect(0, 32767, br->url_te); }
        if (item == FILE_OPEN) open_file();
        if (item == FILE_CLOSE) {
            if (w && ((WindowPeek)w)->windowKind < 0) CloseDeskAcc(((WindowPeek)w)->windowKind);
            else if (br && may_leave("Close this window?")) close_browser(br);
        }
        if (item == FILE_PAGE_SETUP) page_setup();
        if (item == FILE_PRINT && br) print_page();
        if (item == FILE_QUIT) quitting = may_quit();
    } else if (menu == MENU_EDIT) {
        if (item == EDIT_PREFERENCES) { if (need()) go("mill://config", true, NULL); }
        if (item == EDIT_FIND && br && find_dialog()) find_next();
        if (item == EDIT_FIND_AGAIN && br) find_next();
        if (item == EDIT_CLEAR_LOGINS) {
            /* every name and password typed, and every cookie: the next protected page asks again */
            auth_clear(&keyring);
            jar_free(&jar);
            net_keep_close(NULL, NULL);     /* logged in as someone: they go too */
            if (br) set_status("Logins and cookies cleared.");
        }
        else if (br && (br->focus < -1 || (br == full && br->focus < 0))) {   /* the page */
            if (item == EDIT_COPY && br->sel_a != br->sel_b) copy_sel();
            if (item == EDIT_SELECT_ALL && br->have_page) { page_focus(); br->sel_b = br->doc.text_len; draw_view(); }
        }
        else if (br && !(br == full && typing_te() == br->url_te)) {   /* not the hidden address */
            if (item == EDIT_CUT) TECut(typing_te());
            if (item == EDIT_COPY) TECopy(typing_te());
            if (item == EDIT_PASTE) TEPaste(typing_te());
            if (item == EDIT_SELECT_ALL && !(br->focus >= 0 && br->forms.fields[br->focus].type == FIELD_PASSWORD)) {
                if (br->focus >= 0) {
                    RgnHandle save = clip_to(&br->focus_clip);
                    TESetSelect(0, 32767, br->field_te);
                    unclip(save);
                } else TESetSelect(0, 32767, br->url_te);
            }
        }
    } else if (menu == MENU_BOOKMARKS) {
        if (item == BM_ADD) { if (br) bookmark_dialog(-1); }
        else if (item == BM_EDIT) { if (need()) go("mill://config#bookmarks", true, NULL); }
        else if (item >= BM_FIRST && item < BM_FIRST + BOOKMARKS) open_bookmark(item - BM_FIRST);
    } else if (menu == MENU_VIEW) {
        if (item == VIEW_LARGER) zoom_by(1);
        else if (item == VIEW_SMALLER) zoom_by(-1);
        else if (item == VIEW_PAGE_SOURCE && br && br->have_page) {
            char target[340];
            const char *page = !strncmp(br->page_url, VIEW_SOURCE, strlen(VIEW_SOURCE)) ? br->page_url + strlen(VIEW_SOURCE) : br->page_url;
            snprintf(target, sizeof target, "%s%s", VIEW_SOURCE, page);
            go(target, true, NULL);
        } else if (item == VIEW_FULL_SCREEN && br) {
            HiliteMenu(0);
            full_screen(br != full);
        }
    } else if (menu == MENU_GO) {
        if (item == GO_STOP) { if (br && br->loading) stopped = true; }
        else if (item == GO_HOME) { if (need()) go_home(); }
        else if (br) {
            if (item == GO_BACK) go_history(-1);
            if (item == GO_FORWARD) go_history(1);
            if (item == GO_RELOAD) reload();
        }
    }
    HiliteMenu(0);
}

/* Page zoom: Command-plus (or equals) and Command-minus move along HTML 3.2's size ladder, so
   every size and distance is the ladder's step over 9: 1, 10/9, 12/9, 14/9, 2, 24/9, 4. One zoom
   for every window, for the session. Each window's page is laid out again, and kept at about the
   same place. */
static void zoom_by(int dir)
{
    static const short steps[7] = { 9, 10, 12, 14, 18, 24, 36 };
    short now = zoom_step + dir;
    Browser *b, *was = br;
    Boolean failed = false;
    if (now < 0 || now > 6 || now == zoom_step) return;
    zoom_step = now;
    qd_m.zoom = steps[now];
    for (b = browsers; b; b = b->next) {
        long before, height;
        if (!b->have_page) continue;
        br = b;
        before = br->scroll_y;
        height = br->lay.height > 0 ? br->lay.height : 1;
        watch_cursor();
        if (relayout() == -1) failed = true;
        br->scroll_y = br->lay.height * before / height;      /* the same part of the page */
        if (br->scroll_y > max_scroll()) br->scroll_y = max_scroll();
        update_scroll();
        InvalRect(&br->win->portRect);
    }
    br = was;
    if (br) SetPort(br->win);
    InitCursor();
    if (failed) out_of_memory_alert("it shows only part of this page.");
}

static void resized(void)
{
    Rect t;
    geometry();
    t = (*br->url_te)->viewRect;
    t.right = br->mill_r.left - 4;
    (*br->url_te)->viewRect = t;
    place_vbar();
    place_hbar();
    if (relayout() == -1) {
        InvalRect(&br->win->portRect);
        out_of_memory_alert("it shows only part of this page.");
    }
    if (br->scroll_y > max_scroll()) br->scroll_y = max_scroll();
    update_scroll();
    InvalRect(&br->win->portRect);
}

/* The menu bar hidden, the period's way: its height 0, and its rectangle added to the
   desktop so a window may draw there. Put back with show_bar, before Mill quits and while
   another application is in front. */
static void hide_bar(void)
{
    Rect r = qd.screenBits.bounds;
    if (bar_height || !LMGetMBarHeight() || (!bar_rgn && !(bar_rgn = NewRgn()))) return;
    bar_height = LMGetMBarHeight();
    r.bottom = r.top + bar_height;
    RectRgn(bar_rgn, &r);
    LMSetMBarHeight(0);
    UnionRgn(GetGrayRgn(), bar_rgn, GetGrayRgn());
    if (FrontWindow()) CalcVisBehind((WindowPeek)FrontWindow(), bar_rgn);
    if (full) {     /* the full screen window draws there now */
        GrafPtr old;
        GetPort(&old);
        SetPort(full->win);
        InvalRect(&r);  /* it's at the screen's top left: its coordinates are the screen's */
        SetPort(old);
    }
}

static void show_bar(void)
{
    if (!bar_height) return;
    LMSetMBarHeight(bar_height);
    bar_height = 0;
    DiffRgn(GetGrayRgn(), bar_rgn, GetGrayRgn());
    if (FrontWindow()) CalcVisBehind((WindowPeek)FrontWindow(), bar_rgn);
    HiliteMenu(0);
    DrawMenuBar();
}

/* Full screen on or off for the front window (br): Command-K, and Escape to leave. The
   window moves so its title bar is above the screen and its content covers all of it; the
   toolbar and status line go, and the page is laid out for the new width. Leaving puts
   the window back where and as large as it was. */
static void full_screen(Boolean on)
{
    WindowPtr w = br->win;
    TEHandle te = br->url_te;
    Rect r;
    if (on == (br == full) || (on && full)) return;
    SetPort(w);
    if (br->focus >= 0) blur();
    if (on) {
        r = w->portRect;
        LocalToGlobal((Point *)&r.top);
        LocalToGlobal((Point *)&r.bottom);
        full_was = r;
        full = br;
        full_bars = !strcmp(setting("full_screen_bars", "no"), "yes");
        /* the address, out of sight: its caret and anything typed draw off the screen */
        OffsetRect(&(*te)->viewRect, 0, -1000);
        OffsetRect(&(*te)->destRect, 0, -1000);
        HideControl(br->hscroll);
        br->hbar = false;
        if (!full_bars) HideControl(br->vscroll);
        hide_bar();
        r = qd.screenBits.bounds;
        MoveWindow(w, r.left, r.top, false);
        SizeWindow(w, r.right - r.left, r.bottom - r.top, true);
        resized();
    } else {
        full = NULL;
        OffsetRect(&(*te)->viewRect, 0, 1000);
        OffsetRect(&(*te)->destRect, 0, 1000);
        HideControl(br->hscroll);
        br->hbar = false;
        show_bar();
        MoveWindow(w, full_was.left, full_was.top, false);
        SizeWindow(w, full_was.right - full_was.left, full_was.bottom - full_was.top, true);
        resized();
        ShowControl(br->vscroll);
    }
}

/* Out of full screen, whichever window is (New Window, Open Location, and quitting with
   another window to ask about) */
static void leave_full(void)
{
    Browser *was = br;
    if (!full) return;
    use(full);
    full_screen(false);
    use(was);
}

/* Room for a window and a page being read into it, and some to spare, so a window can
   still be closed when memory runs short */
#define WINDOW_ROOM (MAX_PAGE + 48L * 1024)

/* A new window, empty, behind behind ((WindowPtr)-1 for in front), and worked on from
   here; NULL, after saying so, if there isn't the memory for it */
static Browser *new_browser(WindowPtr behind)
{
    Browser *x, *y;
    Rect r, t;
    short n = 0;
    if (MaxBlock() < WINDOW_ROOM || !(x = calloc(1, sizeof *x))) {
        InitCursor();
        StopAlert(ALERT_MEMORY, NULL);
        return NULL;
    }
    for (y = browsers; y; y = y->next) n++;
    /* Fits a 512x342 screen, the smallest a Mill Mac has, each a little below and right
       of the last, four deep */
    r = qd.screenBits.bounds;
    SetRect(&r, r.left + 2 + n % 4 * 8, r.top + 40 + n % 4 * 8, r.right - 2, r.bottom - 2);
    x->win = NewWindow(NULL, &r, "\pMill", true, documentProc + 8 /* with a zoom box */, behind, true, 0);
    x->hist_pos = -1;
    x->focus = -1;
    x->scroll_scale = 1;
    x->next = browsers;
    browsers = x;
    use(x);
    geometry();
    /* Geneva 9 is 12 high (10 above the baseline, 2 below), its top rows kept for
       accents: rows 1 to 12 put capitals level with the buttons, descenders whole */
    SetRect(&t, x->reload_r.right + 6, 1, x->mill_r.left - 4, BAR - 1);
    TextFont(kFontIDGeneva);
    TextSize(9);
    x->url_te = TENew(&t, &t);
    (*x->url_te)->destRect.right = t.left + 2000;  /* one long line: a URL never wraps */
    x->field_te = TENew(&t, &t);                   /* moved over a field when one is typed in */
    TEActivate(x->url_te);
    SetRect(&t, 0, 0, 10, 10);
    x->vscroll = NewControl(x->win, &t, "\p", true, 0, 0, 0, scrollBarProc, 0);
    x->hscroll = NewControl(x->win, &t, "\p", false, 0, 0, 0, scrollBarProc, 0);
    resized();
    return x;
}

/* Close a window and let go of its page; Mill keeps running with none */
static void close_browser(Browser *x)
{
    Browser **p;
    short i;
    use(x);
    if (x == full) { full = NULL; show_bar(); }    /* the window goes: just the menu bar back */
    net_keep_close(x, NULL);
    if (x->have_page) { layout_free(&x->lay); pics_free(); forms_free(&x->forms); html_free(&x->doc); }
    free(x->form_base);
    plugin_drop(x->held);
    for (i = 0; i < x->hist_len; i++) free(x->history[i]);
    TEDispose(x->url_te);
    TEDispose(x->field_te);
    DisposeWindow(x->win);  /* and its scroll bars */
    for (p = &browsers; *p != x; p = &(*p)->next) {}
    *p = x->next;
    free(x);
    use(front());
}

static void do_mouse(EventRecord *ev)
{
    WindowPtr w;
    short part = FindWindow(ev->where, &w);
    if (part == inMenuBar) { refresh_menus(); do_menu(MenuSelect(ev->where)); return; }
    if (part == inSysWindow) { SystemClick(ev, w); return; }
    if (part == inDrag) { DragWindow(w, ev->where, &qd.screenBits.bounds); return; }
    if (!browser_of(w)) return;
    if (w != FrontWindow()) { SelectWindow(w); return; }  /* a click in a window behind brings it forward */
    use(browser_of(w));
    if (br == full && part == inGrow) part = inContent;    /* no grow box: that corner is page */
    switch (part) {
    case inGoAway: if (TrackGoAway(w, ev->where) && may_leave("Close this window?")) close_browser(br); break;
    case inGrow: {
        Rect limits;
        long size;
        SetRect(&limits, 240, 120, 32767, 32767);
        if ((size = GrowWindow(w, ev->where, &limits))) { SizeWindow(w, LoWord(size), HiWord(size), true); resized(); }
        break;
    }
    case inZoomIn: case inZoomOut:
        if (TrackBox(w, ev->where, part)) { EraseRect(&w->portRect); ZoomWindow(w, part, false); resized(); }
        break;
    case inContent: {
        Point p = ev->where;
        ControlHandle c;
        short cpart;
        GlobalToLocal(&p);
        if ((cpart = FindControl(p, w, &c))) {
            if (cpart == inThumb) {
                TrackControl(c, p, NULL);
                if (c == br->hscroll) scroll_x_to(GetControlValue(c));
                else scroll_to((long)GetControlValue(c) * br->scroll_scale);
            } else {
                TrackControl(c, p, (ControlActionUPP)scroll_action);
            }
        } else if (PtInRect(p, &br->back_r)) { if (br->hist_pos > 0 && press(&br->back_r)) go_history(-1); }
        else if (PtInRect(p, &br->fwd_r)) { if (br->hist_pos < br->hist_len - 1 && press(&br->fwd_r)) go_history(1); }
        else if (PtInRect(p, &br->reload_r)) { if (br->hist_pos >= 0 && press(&br->reload_r)) reload(); }
        else if (PtInRect(p, &(*br->url_te)->viewRect)) { blur(); te_click(p, (ev->modifiers & shiftKey) != 0, br->url_te); }
        else if (click_control(p, ev)) {}
        else page_click(p, ev);
        break;
    }
    }
}

static void do_key(EventRecord *ev)
{
    char c = ev->message & charCodeMask;
    long page;
    Boolean in_area;
    if (br && br->refresh_at && (c == 27 || (c == '.' && (ev->modifiers & cmdKey)))) {
        /* Stop, with nothing loading: a refresh asked for is off */
        br->refresh_at = 0;
        set_status("Stopped.");
        return;
    }
    if (ev->modifiers & cmdKey) {
        if (br && c == '[') { go_history(-1); return; }
        if (br && c == ']') { go_history(1); return; }
        if (br && (c == 'r' || c == 'R')) { reload(); return; }
        if (c == '=') c = '+';          /* Command-equals is Command-plus without Shift; Command-Shift-minus is minus */
        if (c == '_') c = '-';
        if (br && (ev->modifiers & optionKey) && option_digit(ev) >= 0) { bookmark_dialog(option_digit(ev)); return; }
        refresh_menus();
        do_menu(MenuKey(c));
        return;
    }
    if (!br) return;
    /* Escape leaves full screen; one pressed while loading stopped it (stop_asked) */
    if (c == 0x1B && br == full) { if (ev->when > loaded_tick) full_screen(false); return; }
    page = br->view_r.bottom - br->view_r.top - 16;
    in_area = br->focus >= 0 && br->forms.fields[br->focus].type == FIELD_TEXTAREA;
    /* control characters TextEdit would draw as boxes: Escape, Clear, forward delete... */
    if (((unsigned char)c < ' ' && !strchr("\b\t\r\003\001\004\013\014\034\035\036\037", c)) || c == 0x7F) return;
    if (c == '\t' && br->forms.n) { next_field((ev->modifiers & shiftKey) ? -1 : 1); return; }
    switch (c) {
    case 0x0B: scroll_to(br->scroll_y - page); return;   /* page up */
    case 0x0C: scroll_to(br->scroll_y + page); return;   /* page down */
    case 0x01: scroll_to(0); return;                 /* home */
    case 0x04: scroll_to(max_scroll()); return;      /* end */
    case 0x1E: if (!in_area) { scroll_to(br->scroll_y - 16); return; } break;  /* up arrow */
    case 0x1F: if (!in_area) { scroll_to(br->scroll_y + 16); return; } break;  /* down arrow */
    }
    if (br == full && br->focus < 0) {
        /* the address isn't there to type in: keys move the page */
        if (c == 0x1C) scroll_x_to(br->scroll_x - 16);      /* left arrow */
        if (c == 0x1D) scroll_x_to(br->scroll_x + 16);      /* right arrow */
        if (c == ' ') scroll_to(br->scroll_y + ((ev->modifiers & shiftKey) ? -page : page));
        return;
    }
    if (br->focus >= 0) {
        if ((c == '\r' && !in_area) || c == 3) { submit(br->forms.fields[br->focus].form, -1, 0, 0); return; }
        type_in_field(c, ev);
        return;
    }
    if (br->focus < -1) blur();
    if (c == '\r' || c == 3) { go_typed(); return; }
    TEKey(c, br->url_te);
    te_show_caret(br->url_te);
}

/* Show a link's address in the status line while the mouse is over it, as browsers did */
static void hover(void)
{
    static const Run *last = NULL;
    static char last_map[300];   /* on an image map: the address last shown */
    static Browser *last_br;     /* the window they were in */
    Point p;
    const Run *r;
    char target[300];
    int m;
    if (br->loading) return;
    if (br != last_br) { last = NULL; last_map[0] = 0; last_br = br; }
    SetPort(br->win);
    GetMouse(&p);
    if (!(r = link_at(p))) r = waiting_image_at(p);
    /* an image map's address changes as the mouse moves across it */
    m = r && waiting_image_at(p) ? 0 : map_target(p, run_at(p), target, sizeof target);
    if (m > 0) {
        last = run_at(p);
        if (strcmp(target, last_map) || !br->status_up) {
            strcpy(last_map, target);
            set_status(target);
            br->status_held = true;
        }
        return;
    }
    if (m < 0) {
        last = run_at(p);
        last_map[0] = 0;
        if (br->status_held) hide_status();
        return;
    }
    last_map[0] = 0;
    if (r == last) return;
    last = r;
    if (r && !r->link) {
        set_status("Use the context menu to load this image.");
        br->status_held = true;
    } else if (r) {
        if (url_resolve(br->page_url, br->lay.links[r->link - 1], target, sizeof target))
            snprintf(target, sizeof target, "%s", br->lay.links[r->link - 1]);
        set_status(target);
        br->status_held = true;
    } else if (br->status_held) {
        hide_status();  /* the link's address goes with the mouse */
    }
}

static void setup_menus(void)
{
    MenuHandle m = NewMenu(MENU_APPLE, "\p\024");
    AppendMenu(m, "\pAbout Mill\311;(-");
    AppendResMenu(m, 'DRVR');
    InsertMenu(m, 0);
    m = NewMenu(MENU_FILE, "\pFile");
    AppendMenu(m, "\pNew Window/N;Open Location\311/L;Open File\311/O;Close/W;(-;Page Setup\311;Print\311/P;(-;Quit/Q");
    InsertMenu(m, 0);
    m = NewMenu(MENU_EDIT, "\pEdit");
    AppendMenu(m, "\p(Undo/Z;(-;Cut/X;Copy/C;Paste/V;Select All/A;Find\311/F;Find Again/G;(-;Preferences\311;Clear Logins and Cookies");
    InsertMenu(m, 0);
    m = NewMenu(MENU_VIEW, "\pView");
    AppendMenu(m, "\pLarger Text/+;Smaller Text/-;(-;Page Source;(-;Full Screen/K");
    InsertMenu(m, 0);
    m = NewMenu(MENU_GO, "\pGo");
    AppendMenu(m, "\pBack/[;Forward/];Reload/R;Home/H;(-;Stop/.");
    InsertMenu(m, 0);
    m = NewMenu(MENU_BOOKMARKS, "\pBookmarks");
    AppendMenu(m, "\pAdd Bookmark\311/D;(-");
    {
        /* the ten slots, Command-1 to Command-9 and Command-0; their names come later */
        static unsigned char key[] = "\p /1";
        short n;
        for (n = 0; n < BOOKMARKS; n++) {
            key[3] = (unsigned char)(n == 9 ? '0' : '1' + n);
            AppendMenu(m, key);
        }
    }
    AppendMenu(m, "\p(-;Edit Bookmarks\311");
    InsertMenu(m, 0);
    DrawMenuBar();
}

static void net_progress(const char *msg) { progress(msg); }

/* While the network keeps us waiting: the sails turn, and Command-period stops */
static Boolean net_waiting(void)
{
    spin();
    if (plugin_tidying()) return false;
    return stop_asked();
}

/* Once, at startup: the Plug-Ins skipped as made for another interface (mill://plugins
   lists them too) */
static void skipped_alert(void)
{
    char text[256];
    short i, n = 0, more = 0;
    long len = sprintf(text, "Plug-Ins made for another version of Mill weren\325t loaded: ");
    for (i = 0; i < nplugs; i++) {
        long k;
        if (plugs[i].status != PLUG_OLDER && plugs[i].status != PLUG_NEWER) continue;
        k = strlen(plugs[i].file);
        if (len + k + 16 > 150) { more++; continue; }
        len += sprintf(text + len, "%s%s", n ? ", " : "", plugs[i].file);
        n++;
    }
    if (!n) return;
    if (more) sprintf(text + len, ", and %d more.", more);
    else strcpy(text + len, ".");
    fail_alert(text);
}

int mill_run(WindowPtr splash)
{
    EventRecord ev;
    Boolean reached, welcome;

    sys_init();  /* System 6 or 7: which calls to use */
    mem_init();
    local_fetch = fetch_for_page;
    InitCursor();
    setup_menus();
    if (!new_browser(splash)) return 1;

    settings_load();
    net_keep_secs = keep_alive_secs();
    net_idle = net_waiting;
    plugins_init();
    settings_migrate();
    fonts_choose();
    /* the connection check, when it's on (not without MacTCP: the first page says so).
       Only it opens MacTCP at launch; else the first page that needs it does. */
    reached = true;
    if (!strcmp(setting("net_check", "yes"), "yes")) {
        if (splash_say) splash_say("MacTCP");
        if (net_open() == noErr) reached = net_check();
    }
    net_status = net_progress;
    splash_say = NULL;      /* it's in the splash's segment, which goes below */
    DisposeWindow(splash);  /* the browser is ready */
    SetPort(br->win);
    /* the splash and what Mill does once are done: their segment goes. main's frame
       stays below, so Mill never returns there; it ends with ExitToShell. */
    UnloadSeg((void *)main);
#ifdef MILL_MEMREPORT
    memreport_snapshot("the window is up, before the first page", true);
#endif

    if (!reached) fail_alert("The Internet wasn\325t reachable. If that\325s expected, turn off Connection check from the Settings page.");
    /* the welcome page while that setting is on, until it loads without an error; else the
       home page. One that fails shows Mill's error page, as any page would. */
    welcome = !strcmp(setting("welcome", "yes"), "yes");
    if (go(welcome ? MILL_WELCOME : setting("homepage", MILL_DEFAULT_HOME), true, NULL) && welcome && page_whole) {
        setting_put("welcome", "no");
        settings_write();
    }
#ifdef MILL_MEMREPORT
    memreport_snapshot("the first page has loaded", false);
#endif
    skipped_alert();

    while (!quitting) {
        use(front());
        if (br && br->focus >= 0 && br->focus_shown && br->forms.fields[br->focus].type != FIELD_PASSWORD) {
            RgnHandle save = clip_to(&br->focus_clip);
            TEIdle(br->field_te);
            unclip(save);
        } else if (br && br->focus < 0) {
            TEIdle(br->url_te);
        }
        if (!sys_next_event(&ev, 10)) { if (br) { hover(); status_idle(); refresh_due(); } mem_short(); net_keep_idle(); continue; }
        switch (ev.what) {
        case mouseDown: do_mouse(&ev); break;
        case keyDown: case autoKey: do_key(&ev); break;
        case updateEvt: update_window((WindowPtr)ev.message); break;
        case activateEvt: activate((WindowPtr)ev.message, (ev.modifiers & activeFlag) != 0); break;
        case osEvt:
            /* switching applications in full screen: the others get the menu bar back */
            if ((ev.message >> 24 & 0xFF) == 1 && full) { if (ev.message & 1) hide_bar(); else show_bar(); }
            break;
        }
    }
    show_bar();     /* the Finder's menu bar */
    net_keep_close(NULL, NULL);
    plugins_quit();
    net_shutdown();
    ExitToShell();
    return 0;
}

#ifdef MILL_MEMREPORT
void memreport_main(void)
{
    Browser *x;
    long i;
    for (x = browsers; x; x = x->next) {
        memreport_note(x, "Browser window (Mill's record)", 0);
        memreport_note(x->win, "Window record (Toolbox)", 0);
        memreport_note(x->url_te, "TextEdit records", 1);
        memreport_note(x->field_te, "TextEdit records", 1);
        if (x->url_te) memreport_note((*x->url_te)->hText, "TextEdit text", 1);
        if (x->field_te) memreport_note((*x->field_te)->hText, "TextEdit text", 1);
        memreport_note(x->vscroll, "Scroll bars (controls)", 1);
        memreport_note(x->hscroll, "Scroll bars (controls)", 1);
        memreport_note(((WindowPeek)x->win)->strucRgn, "Window regions", 1);
        memreport_note(((WindowPeek)x->win)->contRgn, "Window regions", 1);
        memreport_note(((WindowPeek)x->win)->updateRgn, "Window regions", 1);
        memreport_note(((WindowPeek)x->win)->titleHandle, "Window regions", 1);
        memreport_note(x->win->visRgn, "Window regions", 1);
        memreport_note(x->win->clipRgn, "Window regions", 1);
        memreport_note(x->doc.nodes, "Page: parsed nodes", 0);
        memreport_note(x->doc.text, "Page: text", 0);
        memreport_note(x->doc.attrs, "Page: attributes", 0);
        memreport_note(x->lay.lines, "Page: layout", 0);
        memreport_note(x->lay.runs, "Page: layout", 0);
        memreport_note(x->lay.gen, "Page: layout", 0);
        memreport_note(x->lay.links, "Page: layout", 0);
        memreport_note(x->lay.anchors, "Page: layout", 0);
        memreport_note(x->lay.boxes, "Page: layout", 0);
        for (i = 0; i < x->lay.nlinks; i++) memreport_note(x->lay.links[i], "Page: link addresses", 0);
        for (i = 0; i < x->lay.nanchors; i++) memreport_note(x->lay.anchors[i].name, "Page: layout", 0);
        memreport_note(x->forms.fields, "Page: forms", 0);
        for (i = 0; i < x->forms.n; i++) {
            memreport_note(x->forms.fields[i].value, "Page: forms", 0);
            memreport_note(x->forms.fields[i].options, "Page: forms", 0);
            memreport_note(x->forms.fields[i].selected, "Page: forms", 0);
        }
        memreport_note(x->pics, "Page: picture list", 0);
        memreport_note(x->pic_of, "Page: picture list", 0);
        for (i = 0; i < x->npics; i++) {
            memreport_note(x->pics[i].url, "Page: picture list", 0);
            if (x->pics[i].owned) {
                memreport_note(x->pics[i].bm.bits, "Page: pictures (not cached)", 0);
                memreport_note(x->pics[i].bm.mask, "Page: pictures (not cached)", 0);
            }
        }
        for (i = 0; i < x->hist_len; i++) memreport_note(x->history[i], "Window history", 0);
        memreport_note(x->form_base, "Page: forms", 0);
    }
    memreport_note(kept, "Image cache", 0);
    for (i = 0; i < nkept; i++) {
        memreport_note(kept[i].url, "Image cache", 0);
        memreport_note(kept[i].bm.bits, "Image cache", 0);
        memreport_note(kept[i].bm.mask, "Image cache", 0);
    }
    memreport_note(&browsers, "Mill's globals", 0);
}
#endif
