/* Mill: a web browser for classic Macs (monochrome 68000 Macs). */
#include <Multiverse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/auth.h"
#include "core/charset.h"
#include "core/cookie.h"
#include "core/filetypes.h"
#include "core/form.h"
#include "core/html.h"
#include "core/imagemap.h"
#include "core/image.h"
#include "core/layout.h"
#include "core/url.h"
#include "mac_icons.h"
#include "mac_local.h"
#include "mac_login.h"
#include "mac_net.h"
#include "mac_save.h"
#include "mac_start.h"
#include "mac_sys.h"
#include "version.h"

enum { MENU_APPLE = 128, MENU_FILE, MENU_EDIT, MENU_GO, MENU_BOOKMARKS, MENU_POPUP = 250, MENU_CONTEXT };
enum { FILE_LOCATION = 1, FILE_OPEN, FILE_QUIT = 4 };
enum { EDIT_CUT = 3, EDIT_COPY, EDIT_PASTE, EDIT_PREFERENCES = 7 };
enum { GO_BACK = 1, GO_FORWARD, GO_RELOAD, GO_HOME };
enum { BM_ADD = 1, BM_FIRST = 3, BM_EDIT = BM_FIRST + BOOKMARKS + 1 };  /* the slots follow a line */
#define BAR 16          /* toolbar height */
#define STATUS_SECS 5   /* a message shows this long once Mill is idle */
#define SCROLL 16
#define MAX_PAGE (192L * 1024)
#define PAGE_K "192"
#define MAX_UPLOAD (256L * 1024)
#define HISTORY 32
#define FILE_MSG "Web pages can't open this Mac's files or Mill's own pages. Type the address to go there."
#define FORM_MSG "Forms can only send to the same kind of address as their page."

static WindowPtr win;
static TEHandle url_te;
static ControlHandle vscroll, hscroll;   /* hscroll shows only when the page is wider than the view */
static Boolean hbar;
static Boolean quitting = false, loading = false;
static char status_text[128];
static short mill_frame = 0;
static long mill_tick = 0;

static Doc doc;
static Jar jar;         /* cookies, while Mill runs */
static Keyring keyring; /* names and passwords given for sites, while Mill runs */
static Layout lay;
static Forms forms;
static Boolean have_page = false;
static long scroll_y = 0;       /* page pixels above the view */
static long scroll_x = 0;       /* page pixels left of the view */
static short page_w;            /* how wide the laid-out page is */
#define PAGE_LEFT (view_r.left - (short)scroll_x)  /* where page x 0 is in the window */
static short scroll_scale = 1;  /* page pixels per scroll bar unit (control values are 16-bit) */
static char page_url[300];

static char *history[HISTORY];
static short hist_len = 0, hist_pos = -1;

/* ------------------------------------------------------------ geometry */

static Rect back_r, fwd_r, reload_r, mill_r, view_r, status_r;

static void geometry(void)
{
    Rect r = win->portRect;
    SetRect(&back_r, 3, 3, 3 + TOOL_ICON, 3 + TOOL_ICON);
    fwd_r = back_r;
    OffsetRect(&fwd_r, 13, 0);
    reload_r = fwd_r;
    OffsetRect(&reload_r, 13, 0);
    SetRect(&mill_r, r.right - SCROLL + 1, 0, r.right - SCROLL + 1 + MILL_ICON, MILL_ICON);  /* above the scroll bar */
    SetRect(&view_r, 0, BAR + 1, r.right - SCROLL + 1, r.bottom - (hbar ? SCROLL - 1 : 0));
}

/* Drawing limited to r, within the page view; returns the clip to put back */
static RgnHandle clip_to(const Rect *r)
{
    RgnHandle save = NewRgn();
    Rect c;
    GetClip(save);
    SectRect(r, &view_r, &c);
    ClipRect(&c);
    return save;
}

static void unclip(RgnHandle save)
{
    SetClip(save);
    DisposeRgn(save);
}

/* ------------------------------------------------------------ icons */

static void draw_bits(const unsigned short *rows, short n, short width, const Rect *where, Boolean dim)
{
    BitMap bm;
    Rect src;
    unsigned short buf[16];
    memcpy(buf, rows, n * 2);
    bm.baseAddr = (Ptr)buf;
    bm.rowBytes = 2;
    SetRect(&bm.bounds, 0, 0, 16, n);
    SetRect(&src, 0, 0, width, n);
    CopyBits(&bm, &win->portBits, &src, where, srcCopy, NULL);
    if (dim) {
        PenPat(&qd.gray);
        PenMode(patBic);
        PaintRect(where);
        PenNormal();
    }
}

static void draw_mill(void)
{
    draw_bits(ICON_MILL[mill_frame], 16, 16, &mill_r, false);
}

/* Turn the sails a step; called while loading, at most every few ticks */
static void spin(void)
{
    GrafPtr old;
    if (TickCount() - mill_tick < 4) return;
    mill_tick = TickCount();
    mill_frame = (mill_frame + 1) & 3;
    GetPort(&old);
    SetPort(win);
    draw_mill();
    SetPort(old);
}

static void draw_toolbar(void)
{
    Rect r = win->portRect;
    draw_bits(ICON_BACK, TOOL_ICON, TOOL_ICON, &back_r, hist_pos <= 0);
    draw_bits(ICON_FORWARD, TOOL_ICON, TOOL_ICON, &fwd_r, hist_pos >= hist_len - 1);
    /* reload, or a stop sign while something loads */
    draw_bits(loading ? ICON_STOP : ICON_RELOAD, TOOL_ICON, TOOL_ICON, &reload_r, !loading && hist_pos < 0);
    TEUpdate(&(*url_te)->viewRect, url_te);
    draw_mill();
    MoveTo(r.left, BAR);
    LineTo(r.right, BAR);
}

/* Something is loading, or has finished: the reload button turns to a stop sign and back */
static void set_loading(Boolean on)
{
    GrafPtr old;
    loading = on;
    GetPort(&old);
    SetPort(win);
    draw_bits(loading ? ICON_STOP : ICON_RELOAD, TOOL_ICON, TOOL_ICON, &reload_r, !loading && hist_pos < 0);
    SetPort(old);
}

/* ------------------------------------------------------------ status message */

/* Black text on a small white patch, lined along its top and right, over the page's
   bottom left corner. It stays while something loads or the mouse is on a link, and goes
   STATUS_SECS after it last changed. */
static Boolean status_up, status_held;  /* showing; held by the mouse on a link */
static long status_tick;                /* when it last changed */
static Boolean page_limited;            /* draw_page: only page_limit */
static Rect page_limit;

static void draw_page(short only);

static void status_box(Rect *r)
{
    short w;
    TextFont(kFontIDGeneva);
    TextSize(9);
    TextFace(0);
    w = TextWidth(status_text, 0, strlen(status_text)) + 9;
    if (w > view_r.right - view_r.left) w = view_r.right - view_r.left;
    SetRect(r, view_r.left, view_r.bottom - 13, view_r.left + w, view_r.bottom);
}

static void draw_status(void)
{
    RgnHandle save;
    if (!status_up) return;
    status_box(&status_r);
    save = clip_to(&status_r);
    EraseRect(&status_r);
    /* a line along the top and down the right, so it reads as its own patch */
    MoveTo(status_r.left, status_r.top);
    LineTo(status_r.right - 1, status_r.top);
    LineTo(status_r.right - 1, status_r.bottom - 1);
    MoveTo(status_r.left + 4, status_r.bottom - 3);
    DrawText(status_text, 0, strlen(status_text));
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
    if (!status_up) return;
    status_up = status_held = false;
    GetPort(&old);
    SetPort(win);
    draw_page_area(&status_r);
    SetPort(old);
}

static void set_status(const char *msg)
{
    GrafPtr old;
    Rect was = status_r;
    Boolean was_up = status_up;
    status_tick = TickCount();
    status_held = false;
    if (!strcmp(msg, status_text) && (status_up || !msg[0])) { spin(); return; }
    strncpy(status_text, msg, sizeof status_text - 1);
    if (!msg[0]) { hide_status(); spin(); return; }
    GetPort(&old);
    SetPort(win);
    status_up = true;
    status_box(&status_r);
    if (was_up && was.right > status_r.right) {
        /* the part a longer message covered */
        was.left = status_r.right;
        draw_page_area(&was);
    }
    draw_status();
    SetPort(old);
    spin();
}

/* Called when Mill is idle */
static void status_idle(void)
{
    if (status_up && !loading && !status_held && TickCount() - status_tick > STATUS_SECS * 60L) hide_status();
}

/* ------------------------------------------------------------ QuickDraw metrics */

static void use_style(const MStyle *s)
{
    TextFont(s->font);
    TextSize(s->size);
    TextFace(s->face & ~FACE_INVERSE);
}

static long qd_width(void *ctx, const MStyle *s, const char *text, long len)
{
    (void)ctx;
    use_style(s);
    return TextWidth((Ptr)text, 0, (short)len);
}

static void qd_metrics(void *ctx, const MStyle *s, short *a, short *d, short *l)
{
    FontInfo fi;
    (void)ctx;
    use_style(s);
    GetFontInfo(&fi);
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
} Kept;

static Kept *kept;
static short nkept;
static long kept_bytes, page_number;

static long bitmap_bytes(const Bitmap1 *b) { return (long)b->row_bytes * b->h; }

static Kept *kept_find(const char *url, short aw, short ah)
{
    short i;
    for (i = 0; i < nkept; i++) if (kept[i].aw == aw && kept[i].ah == ah && !strcmp(kept[i].url, url)) return &kept[i];
    return NULL;
}

/* Make room for need bytes under limit by letting go of pictures this page isn't showing,
   least recently shown first; returns whether there's room */
static Boolean kept_trim(long need, long limit)
{
    while (kept_bytes + need > limit) {
        short i, oldest = -1;
        for (i = 0; i < nkept; i++)
            if (kept[i].used != page_number && (oldest < 0 || kept[i].used < kept[oldest].used)) oldest = i;
        if (oldest < 0) return false;
        kept_bytes -= bitmap_bytes(&kept[oldest].bm);
        free(kept[oldest].url);
        image_free(&kept[oldest].bm);
        kept[oldest] = kept[--nkept];
    }
    return true;
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
    kept_bytes += size;
    nkept++;
    return true;
}

/* ------------------------------------------------------------ pictures */

/* One per distinct picture on the page (the same SRC at the same size is fetched once) */
typedef struct {
    char *url;
    short aw, ah;       /* WIDTH and HEIGHT from the page, -1 when not given */
    char state;         /* PIC_WAITING, PIC_SHOWN, PIC_FAILED */
    Boolean owned;      /* its bits are its own, not the picture memory's */
    Bitmap1 bm;
} Pic;
enum { PIC_WAITING, PIC_SHOWN, PIC_FAILED };

static Pic *pics;
static short npics;
static short *pic_of;   /* IMG node -> index in pics + 1, or 0 */

static void pics_free(void)
{
    short i;
    for (i = 0; i < npics; i++) { free(pics[i].url); if (pics[i].owned) image_free(&pics[i].bm); }
    free(pics);
    free(pic_of);
    pics = NULL;
    pic_of = NULL;
    npics = 0;
}

static short px_attr(const Node *n, const char *name)
{
    const char *v = html_attr(&doc, n, name);
    return v && !strchr(v, '%') && atoi(v) > 0 ? (short)atoi(v) : -1;
}

static int scheme_of(const char *url)
{
    Url u;
    return url_parse(url, &u) ? -1 : u.scheme;
}

static void pics_collect(void)
{
    long i;
    int scheme = scheme_of(page_url);
    pic_of = calloc(doc.nnodes, sizeof(short));
    if (!pic_of) return;
    for (i = 0; i < doc.nnodes; i++) {
        const Node *n = &doc.nodes[i];
        const char *src;
        char url[300];
        short k, aw, ah;
        Pic *p;
        if (n->tag != TAG_IMG || !(src = html_attr(&doc, n, "src")) || url_resolve(page_url, src, url, sizeof url)) continue;
        if (scheme_of(url) != scheme) continue;  /* no pictures from another kind of address */
        aw = px_attr(n, "width");
        ah = px_attr(n, "height");
        for (k = 0; k < npics; k++) if (pics[k].aw == aw && pics[k].ah == ah && !strcmp(pics[k].url, url)) break;
        if (k == npics) {
            Kept *keep;
            if (!(p = realloc(pics, (npics + 1) * sizeof(Pic)))) break;
            pics = p;
            p = &pics[npics];
            memset(p, 0, sizeof *p);
            if (!(p->url = malloc(strlen(url) + 1))) break;
            strcpy(p->url, url);
            p->aw = aw;
            p->ah = ah;
            if ((keep = kept_find(url, aw, ah))) {
                /* shown before: no fetching or decoding */
                p->bm = keep->bm;
                p->state = PIC_SHOWN;
                keep->used = page_number;
            }
            npics++;
        }
        pic_of[i] = k + 1;
    }
}

static int qd_image_size(void *ctx, long node, short *w, short *h)
{
    const Pic *p;
    (void)ctx;
    if (!pic_of || !pic_of[node]) return 0;
    p = &pics[pic_of[node] - 1];
    if (p->state != PIC_SHOWN) return 0;
    *w = p->bm.w;
    *h = p->bm.h;
    return 1;
}

/* ------------------------------------------------------------ form controls */

#define LINE_H 11       /* a line of Geneva 9, which every control uses */
#define CHOOSE "Choose..."

static long focus = -1;         /* the field being typed in, or -1 for the address */
static TEHandle field_te;       /* typing in a text field or text area happens here */
static Boolean focus_shown;     /* the focused field was on screen at the last draw */
static Rect focus_clip;         /* and where */

static void control_font(void)
{
    TextFont(kFontIDGeneva);
    TextSize(9);
    TextFace(0);
}

static Field *field_of(long node)
{
    long i = forms.n ? forms_find(&forms, node) : -1;
    return i >= 0 ? &forms.fields[i] : NULL;
}

static const char *button_label(const Field *f)
{
    const char *v = html_attr(&doc, &doc.nodes[f->node], "value");
    if (v && *v) return v;
    if (f->type == FIELD_RESET) return "Reset";
    if (f->type == FIELD_IMAGE && (v = html_attr(&doc, &doc.nodes[f->node], "alt")) && *v) return v;
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
    short cw, i, most = view_r.right - view_r.left - 24;
    (void)ctx;
    if (!f || f->type == FIELD_HIDDEN) return 0;
    control_font();
    cw = CharWidth('n');
    *h = 15;
    *asc = 11;
    switch (f->type) {
    case FIELD_CHECKBOX: case FIELD_RADIO:
        *w = *h = 11;
        *asc = 10;
        return 1;
    case FIELD_SUBMIT: case FIELD_RESET: case FIELD_IMAGE: {
        const char *l = button_label(f);
        *w = TextWidth((Ptr)l, 0, strlen(l)) + 16;
        break;
    }
    case FIELD_SELECT: {
        short widest = 20;
        char label[100];
        for (i = 0; i < f->noptions; i++) {
            short lw;
            forms_option_label(&doc, f->options[i], label, sizeof label);
            if ((lw = TextWidth(label, 0, strlen(label))) > widest) widest = lw;
        }
        if (list_rows(f)) { *w = widest + 8; *h = list_rows(f) * LINE_H + 4; *asc = *h - 4; }
        else *w = widest + 24;
        break;
    }
    case FIELD_TEXTAREA:
        *w = (f->cols > 0 ? f->cols : 20) * cw + 8;
        *h = (f->rows > 0 ? f->rows : 2) * LINE_H + 4;
        *asc = *h - 4;
        break;
    default:  /* text, password, file */
        *w = (f->size > 0 ? f->size : 20) * cw + 6;
        if (f->type == FIELD_FILE) *w += TextWidth(CHOOSE, 0, strlen(CHOOSE)) + 20;
    }
    if (*w > most) *w = most > 20 ? most : 20;
    return 1;
}

static Metrics qd_m = { qd_width, qd_metrics, qd_image_size, qd_control_size, NULL };

/* Where a control is in the window right now; false if it isn't laid out */
static Boolean control_box(long node, Rect *box)
{
    long i, k;
    for (i = 0; i < lay.nlines; i++) {
        const MLine *ln = &lay.lines[i];
        for (k = ln->first; k < ln->first + ln->count; k++) {
            const Run *r = &lay.runs[k];
            if (r->kind == RUN_CONTROL && r->text == node) {
                short top = view_r.top + (short)(ln->y - scroll_y) + ln->ascent - r->font;
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
    if (f->type == FIELD_FILE) box.right -= TextWidth(CHOOSE, 0, strlen(CHOOSE)) + 20;
    InsetRect(&box, 2, 2);
    return box;
}

static void draw_button(const Rect *b, const char *label)
{
    EraseRoundRect(b, 8, 8);
    FrameRoundRect(b, 8, 8);
    MoveTo(b->left + (b->right - b->left - TextWidth((Ptr)label, 0, strlen(label))) / 2, b->bottom - 4);
    DrawText((Ptr)label, 0, strlen(label));
}

/* Put the typing area over a field, keeping any sideways scroll it has */
static void place_te(const Rect *inner, Boolean wrap)
{
    Rect old = (*field_te)->viewRect;
    (*field_te)->viewRect = *inner;
    OffsetRect(&(*field_te)->destRect, inner->left - old.left, inner->top - old.top);
    if (wrap) (*field_te)->destRect.right = (*field_te)->destRect.left + (inner->right - inner->left);
}

static void draw_text_box(const Field *f, const Rect *box, Boolean focused)
{
    Rect inner = text_part(f, *box), frame = inner;
    RgnHandle save;
    InsetRect(&frame, -2, -2);
    EraseRect(&frame);
    FrameRect(&frame);
    save = clip_to(&inner);
    if (focused && f->type != FIELD_PASSWORD) {
        place_te(&inner, f->type == FIELD_TEXTAREA);
        TEUpdate(&inner, field_te);
        focus_shown = true;
        SectRect(&inner, &view_r, &focus_clip);
    } else if (f->type == FIELD_TEXTAREA) {
        TETextBox(f->value, strlen(f->value), &inner, teFlushLeft);
    } else {
        const char *v = f->value ? f->value : "";
        long n = strlen(v);
        MoveTo(inner.left, inner.top + 9);
        if (f->type == FIELD_PASSWORD) {
            long i;
            for (i = 0; i < n; i++) DrawChar((char)0xA5);  /* a bullet for each letter */
            if (focused) { Line(0, -9); Move(0, 9); }      /* where typing goes */
        } else {
            DrawText((Ptr)v, 0, n);
        }
    }
    unclip(save);
}

static void draw_popup(const Field *f, const Rect *box)
{
    char label[100] = "";
    short i, x = box->right - 12;
    Rect in = *box;
    for (i = 0; i < f->noptions; i++) if (f->selected[i]) { forms_option_label(&doc, f->options[i], label, sizeof label); break; }
    EraseRect(box);
    FrameRect(box);
    MoveTo(box->left + 1, box->bottom);   /* a pop-up menu's shadow */
    LineTo(box->right, box->bottom);
    LineTo(box->right, box->top + 1);
    in.right -= 14;
    {
        RgnHandle save = clip_to(&in);
        MoveTo(box->left + 4, box->bottom - 4);
        DrawText(label, 0, strlen(label));
        unclip(save);
    }
    for (i = 0; i < 4; i++) {             /* the down arrow */
        MoveTo(x - 3 + i, box->top + 6 + i);
        LineTo(x + 3 - i, box->top + 6 + i);
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
        SetRect(&row, box->left + 1, box->top + 2 + i * LINE_H, box->right - 1, box->top + 2 + (i + 1) * LINE_H);
        forms_option_label(&doc, f->options[i], label, sizeof label);
        MoveTo(row.left + 3, row.bottom - 2);
        DrawText(label, 0, strlen(label));
        if (f->selected[i]) InvertRect(&row);
    }
    if (f->noptions > rows) { MoveTo(box->right - 10, box->bottom - 3); DrawText("...", 0, 3); }
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
        if (f->checked) { Rect dot = box; InsetRect(&dot, 3, 3); PaintOval(&dot); }
        break;
    case FIELD_SUBMIT: case FIELD_RESET: case FIELD_IMAGE:
        draw_button(&box, button_label(f));
        break;
    case FIELD_SELECT:
        if (list_rows(f)) draw_list(f, &box); else draw_popup(f, &box);
        break;
    case FIELD_FILE: {
        Rect button = box;
        button.left = text_part(f, box).right + 6;
        draw_text_box(f, &box, false);
        draw_button(&button, CHOOSE);
        break;
    }
    default:
        draw_text_box(f, &box, f - forms.fields == focus);
    }
}

/* ------------------------------------------------------------ the page view */

static void draw_picture(const Run *r, short top)
{
    Rect box, in;
    const Pic *p = pic_of && pic_of[r->text] ? &pics[pic_of[r->text] - 1] : NULL;
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
        bm.baseAddr = (Ptr)p->bm.bits;
        bm.rowBytes = p->bm.row_bytes;
        SetRect(&bm.bounds, 0, 0, p->bm.w, p->bm.h);
        CopyBits(&bm, &win->portBits, &bm.bounds, &in, srcCopy, NULL);
    } else {
        /* not here (yet): a gray frame, the image icon, and the ALT text if they fit */
        const char *alt = html_attr(&doc, &doc.nodes[r->text], "alt");
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

/* Draw the page's visible part; with only >= 0, just the runs showing that picture */
static void draw_page(short only)
{
    long i, top = scroll_y, bottom = scroll_y + (view_r.bottom - view_r.top);
    RgnHandle save = NewRgn();
    Rect area = view_r;
    if (page_limited) SectRect(&view_r, &page_limit, &area);
    GetClip(save);
    ClipRect(&area);
    if (only < 0) { EraseRect(&area); if (!page_limited) focus_shown = false; }
    if (have_page) {
        /* fills first, last to first so outer cells go under inner ones; then frames */
        for (i = lay.nboxes - 1; i >= 0 && only < 0; i--) {
            const Box *b = &lay.boxes[i];
            Rect r;
            if (b->thick > 0 || b->y + b->h <= top || b->y >= bottom) continue;
            SetRect(&r, PAGE_LEFT + b->x, view_r.top + (short)(b->y - scroll_y),
                    PAGE_LEFT + b->x + b->w, view_r.top + (short)(b->y - scroll_y) + b->h);
            if (b->thick == 0) PaintRect(&r);
            else EraseRect(&r);
        }
        for (i = 0; i < lay.nboxes && only < 0; i++) {
            const Box *b = &lay.boxes[i];
            Rect r;
            if (b->thick <= 0 || b->y + b->h <= top || b->y >= bottom) continue;
            SetRect(&r, PAGE_LEFT + b->x, view_r.top + (short)(b->y - scroll_y),
                    PAGE_LEFT + b->x + b->w, view_r.top + (short)(b->y - scroll_y) + b->h);
            PenSize(b->thick, b->thick);
            FrameRect(&r);
            PenNormal();
        }
        for (i = layout_line_at(&lay, top); i < lay.nlines && lay.lines[i].y < bottom; i++) {
            const MLine *ln = &lay.lines[i];
            long k;
            short base = view_r.top + (short)(ln->y - scroll_y) + ln->ascent;
            if (ln->y + ln->height <= top) continue;
            for (k = ln->first; k < ln->first + ln->count; k++) {
                const Run *r = &lay.runs[k];
                if (only >= 0) {
                    if (r->kind == RUN_IMAGE && pic_of && pic_of[r->text] == only + 1) draw_picture(r, base - r->font);
                    continue;
                }
                if (r->kind == RUN_RULE) {
                    short y = view_r.top + (short)(ln->y - scroll_y) + ln->height / 2;
                    MoveTo(PAGE_LEFT + r->x, y);
                    LineTo(PAGE_LEFT + r->x + r->width - 1, y);
                    continue;
                }
                if (r->kind == RUN_IMAGE) { draw_picture(r, base - r->font); continue; }
                if (r->kind == RUN_CONTROL) { draw_control(r, base - r->font); continue; }
                TextFont(r->font);
                TextSize(r->size);
                TextFace(r->face & ~FACE_INVERSE);
                if (r->face & FACE_INVERSE) TextMode(srcBic);  /* white, on a dark cell */
                MoveTo(PAGE_LEFT + r->x, base);
                DrawText(r->kind == RUN_GEN ? lay.gen + r->text : doc.text + r->text, 0, r->len);
                TextMode(srcOr);
            }
        }
    }
    SetClip(save);
    DisposeRgn(save);
    draw_status();  /* the message stays over the page */
}

static void draw_view(void) { draw_page(-1); }

static long max_scroll(void)
{
    long m = have_page ? lay.height - (view_r.bottom - view_r.top) : 0;
    return m > 0 ? m : 0;
}

static void update_scroll(void)
{
    long m = max_scroll();
    scroll_scale = (short)(m / 30000 + 1);
    SetControlMaximum(vscroll, (short)(m / scroll_scale));
    SetControlValue(vscroll, (short)(scroll_y / scroll_scale));
}

static void scroll_to(long y)
{
    long m = max_scroll(), dy;
    if (y > m) y = m;
    if (y < 0) y = 0;
    dy = scroll_y - y;
    if (!dy) return;
    scroll_y = y;
    SetControlValue(vscroll, (short)(scroll_y / scroll_scale));
    draw_view();
}

static long max_scroll_x(void)
{
    long m = hbar ? page_w - (view_r.right - view_r.left) : 0;
    return m > 0 ? m : 0;
}

/* The horizontal scroll bar runs along the bottom, beside the grow box */
static void place_hbar(void)
{
    Rect r = win->portRect;
    MoveControl(hscroll, -1, r.bottom - SCROLL + 1);
    SizeControl(hscroll, r.right - SCROLL + 3, SCROLL);
}

/* Show the horizontal scroll bar when the page is wider than the view, else give its
   room to the page */
static void fit_hbar(void)
{
    Boolean need = have_page && page_w > view_r.right - view_r.left;
    if (need != hbar) {
        hbar = need;
        geometry();
        place_hbar();
        if (hbar) ShowControl(hscroll);
        else HideControl(hscroll);
        if (scroll_y > max_scroll()) scroll_y = max_scroll();
        update_scroll();
    }
    if (scroll_x > max_scroll_x()) scroll_x = max_scroll_x();
    SetControlMaximum(hscroll, (short)max_scroll_x());
    SetControlValue(hscroll, (short)scroll_x);
}

static void scroll_x_to(long x)
{
    long m = max_scroll_x();
    if (x > m) x = m;
    if (x < 0) x = 0;
    if (x == scroll_x) return;
    scroll_x = x;
    SetControlValue(hscroll, (short)scroll_x);
    draw_view();
}

static void relayout(void)
{
    long i;
    if (!have_page) return;
    layout_free(&lay);
    set_status("Laying out...");
    if (layout_doc(&lay, &doc, &qd_m, view_r.right - view_r.left)) set_status("Not enough memory for this page.");
    /* the page's width: its widest run or frame, and the margin to its right */
    page_w = 0;
    for (i = 0; i < lay.nruns; i++) if (lay.runs[i].x + lay.runs[i].width > page_w) page_w = lay.runs[i].x + lay.runs[i].width;
    for (i = 0; i < lay.nboxes; i++) if (lay.boxes[i].x + lay.boxes[i].w > page_w) page_w = lay.boxes[i].x + lay.boxes[i].w;
    page_w += 8;
    SetPort(win);
    fit_hbar();
}

/* The whole window, now rather than at the next update event */
static void redraw(void)
{
    SetPort(win);
    EraseRect(&win->portRect);
    draw_toolbar();
    draw_view();
    DrawControls(win);
    {
        /* just the grow box: DrawGrowIcon also outlines a horizontal scroll bar that's
           often not there */
        Rect g = win->portRect;
        RgnHandle save = NewRgn();
        g.left = g.right - SCROLL + 1;
        g.top = g.bottom - SCROLL + 1;
        GetClip(save);
        ClipRect(&g);
        DrawGrowIcon(win);
        SetClip(save);
        DisposeRgn(save);
    }
    draw_status();
    ValidRect(&win->portRect);
}

/* ------------------------------------------------------------ typing in fields */

/* Keep what's been typed, and give the keyboard back to the address */
static void blur(void)
{
    if (focus >= 0) {
        Field *f = &forms.fields[focus];
        if (f->type != FIELD_PASSWORD) {
            CharsHandle h = TEGetText(field_te);
            long n = (*field_te)->teLength;
            char *v = malloc(n + 1);
            if (v) {
                memcpy(v, *h, n);
                v[n] = 0;
                forms_set_value(f, v);
                free(v);
            }
        }
        TEDeactivate(field_te);
        focus = -1;
        focus_shown = false;
    }
    TEActivate(url_te);
}

/* Type in a field: a text field, password field, or text area */
static void focus_field(long i, const Point *click, Boolean shift)
{
    Field *f = &forms.fields[i];
    Rect box;
    if (focus == i) {
        if (click && f->type != FIELD_PASSWORD) TEClick(*click, shift, field_te);
        return;
    }
    blur();
    TEDeactivate(url_te);
    focus = i;
    if (!control_box(f->node, &box)) return;
    if (box.top < view_r.top || box.bottom > view_r.bottom) {
        scroll_to(scroll_y + (box.top - view_r.top) - 20);
        control_box(f->node, &box);
    }
    if (f->type != FIELD_PASSWORD) {
        Rect inner = text_part(f, box);
        (*field_te)->viewRect = inner;
        (*field_te)->destRect = inner;
        if (f->type != FIELD_TEXTAREA) (*field_te)->destRect.right = inner.left + 2000;  /* one long line */
        TESetText(f->value, strlen(f->value), field_te);
        TECalText(field_te);
        TEActivate(field_te);
        if (click) TEClick(*click, shift, field_te);
        else TESetSelect(0, 32767, field_te);
    }
    draw_view();
}

/* Tab: the next field to type in, or the previous one with Shift */
static void next_field(int step)
{
    long i, n = forms.n;
    for (i = 1; i <= n; i++) {
        long k = ((focus < 0 ? (step > 0 ? -1 : 0) : focus) + i * step + n * 2) % n;
        char t = forms.fields[k].type;
        if (t == FIELD_TEXT || t == FIELD_PASSWORD || t == FIELD_TEXTAREA) { focus_field(k, NULL, false); return; }
    }
}

static void type_in_field(char c, EventRecord *ev)
{
    Field *f = &forms.fields[focus];
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
    if ((unsigned char)c >= 32 && c != 0x7F && f->maxlength && (*field_te)->teLength >= f->maxlength &&
        (*field_te)->selStart == (*field_te)->selEnd) return;
    save = clip_to(&focus_clip);
    TEKey(c, field_te);
    unclip(save);
    (void)ev;
}

/* ------------------------------------------------------------ loading */

typedef struct {
    char *buf;
    long len;           /* bytes received; only the first MAX_PAGE are kept in buf */
    long total;         /* the whole response's size when the server says, else 0 */
    Boolean headed;     /* its headers have been looked at */
    int code;
    long body;          /* where the body starts */
    char mime[64];
    const char *url;    /* what's being fetched */
    const Url *req;     /* the same, parsed: Set-Cookie headers are for its host and path */
    Boolean cut;        /* longer than MAX_PAGE: only the start is kept */
    Boolean may_save;   /* a page Mill can't show is saved */
    Boolean save_all;   /* saved whatever it is (Save ... As) */
    Boolean have_spec;  /* where to save it is already chosen */
    FSSpec spec;
    Boolean saving, declined;
    Saver saver;
} Response;

static void response_init(Response *r)
{
    char *buf = r->buf;
    memset(r, 0, sizeof *r);
    r->buf = buf;
}

static char activity[40];   /* what's loading, ahead of the network's progress */
static Boolean stopped;

static void progress(const char *msg)
{
    char line[128];
    if (activity[0]) { snprintf(line, sizeof line, "%s: %s", activity, msg); set_status(line); }
    else set_status(msg);
}

/* Command-period or Escape (Clear, on the Plus's keypad) stops loading, and so does a
   click on the stop sign */
static Boolean stop_asked(void)
{
    EventRecord e;
    if (!stopped && EventAvail(mDownMask, &e)) {
        WindowPtr w;
        if (FindWindow(e.where, &w) == inContent && w == win) {
            GrafPtr old;
            Point p = e.where;
            GetPort(&old);
            SetPort(win);
            GlobalToLocal(&p);
            if (PtInRect(p, &reload_r)) {
                GetNextEvent(mDownMask, &e);
                InvertRect(&reload_r);   /* pressed, briefly */
                while (StillDown()) {}
                InvertRect(&reload_r);
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
        if (e - line > 11 && prefix_ci(r->buf + line, "Set-Cookie:", 11) && r->req && r->req->scheme == SCHEME_HTTP && accept_cookies()) {
            unsigned long now;
            ReadDateTime(&now);  /* the trap: GetDateTime is a low-memory read GCC flags */
            jar_set(&jar, r->req->host, r->req->path, r->buf + line + 11, e - line - 11, now);
        }
        if (e - line > 15 && prefix_ci(r->buf + line, "Content-Length:", 15)) length = atol(r->buf + line + 15);
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

/* What Mill shows: HTML, text, GIF, and JPEG (and pages that don't say) */
static Boolean showable(const char *mime)
{
    return !mime[0] || prefix_ci(mime, "text/", 5) || prefix_ci(mime, "image/gif", 9) ||
           prefix_ci(mime, "image/jpeg", 10) || prefix_ci(mime, "image/pjpeg", 11);
}

static Boolean ask_where(const char *url, FSSpec *spec)
{
    char name[64];
    Str255 p;
    Boolean chosen;
    filetype_name(url, name, sizeof name);
    p[0] = (unsigned char)strlen(name);
    memcpy(p + 1, name, p[0]);
    InitCursor();
    chosen = sys_choose_save("\pSave as:", p, spec);
    SetCursor(*GetCursor(watchCursor));
    return chosen;
}

/* Start saving the body to a file instead of keeping it; false if it won't be */
static Boolean start_save(Response *r)
{
    char name[64], type[4], creator[4];
    if (!r->have_spec && !ask_where(r->url, &r->spec)) { r->declined = true; return false; }
    {
        /* rules look at the name as the server gave it */
        const char *q = r->url + strcspn(r->url, "?#"), *seg = q;
        while (seg > r->url && seg[-1] != '/') seg--;
        snprintf(name, sizeof name, "%.*s", (int)(q - seg), seg);
    }
    filetype_find(file_type_rules(), r->mime, name, type, creator);
    saver_begin(&r->saver, &r->spec, type, creator, decode_macbinary());
    r->saving = true;
    return saver_write(&r->saver, r->buf + r->body, r->len - r->body) == noErr;
}

static Boolean collect(const char *data, long len, void *ref)
{
    Response *r = ref;
    char msg[48];
    if (stop_asked()) return false;
    if (r->saving) {
        if (saver_write(&r->saver, data, len) != noErr) return false;
        r->len += len;
    } else {
        if (r->len + len > MAX_PAGE) {
            /* too long to show whole: keep what fits */
            memcpy(r->buf + r->len, data, MAX_PAGE - r->len);
            r->len = MAX_PAGE;
            r->cut = true;
            return false;
        }
        memcpy(r->buf + r->len, data, len);
        r->len += len;
        if (!r->headed) {
            read_head(r);
            if (r->headed && r->code >= 200 && r->code < 300 && (r->save_all || (r->may_save && !showable(r->mime))))
                if (!start_save(r)) return false;
        }
    }
    if (r->total > 0) sprintf(msg, "%s... %ldK of %ldK", r->saving ? "Saving" : "Reading", (r->len + 1023) / 1024, (r->total + 1023) / 1024);
    else sprintf(msg, "%s... %ldK", r->saving ? "Saving" : "Reading", (r->len + 1023) / 1024);
    progress(msg);
    return true;
}

/* After a fetch that saved, or was to: say how it went */
static void finish_save(Response *r, OSErr err)
{
    char msg[120], name[64];
    if (r->declined) { set_status("Not saved."); return; }
    snprintf(name, sizeof name, "%.*s", r->spec.name[0], (char *)r->spec.name + 1);
    if (stopped || err != noErr) {
        saver_abort(&r->saver);
        if (stopped) set_status("Stopped. The file wasn't saved.");
        else { snprintf(msg, sizeof msg, "The download failed (MacTCP error %d).", err); set_status(msg); }
        return;
    }
    if ((err = saver_end(&r->saver)) != noErr) {
        snprintf(msg, sizeof msg, err == eofErr ? "\"%s\" ended early and wasn't saved." : "Mill couldn't save \"%s\" (error %d).", name, err);
        set_status(msg);
        return;
    }
    snprintf(msg, sizeof msg, r->saver.macbinary ? "Saved \"%s\", decoded from MacBinary." : "Saved \"%s\".", name);
    set_status(msg);
}

static void show_url(const char *url)
{
    TESetText((Ptr)url, strlen(url), url_te);
    TESetSelect(0, 0, url_te);
    InvalRect(&(*url_te)->viewRect);
}

static void remember(const char *url)
{
    short i;
    if (hist_pos >= 0 && !strcmp(history[hist_pos], url)) return;
    for (i = hist_pos + 1; i < hist_len; i++) free(history[i]);
    hist_len = hist_pos + 1;
    if (hist_len == HISTORY) {
        free(history[0]);
        memmove(history, history + 1, (HISTORY - 1) * sizeof(char *));
        hist_len--;
    }
    history[hist_len] = malloc(strlen(url) + 1);
    if (history[hist_len]) { strcpy(history[hist_len], url); hist_pos = hist_len++; }
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
    Challenge challenge; /* from WWW-Authenticate, on a 401 */
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
            if (e - l > 17 && prefix_ci(b + l, "WWW-Authenticate:", 17))
                auth_challenge(b + l + 17, e - l - 17, &h->challenge);
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

enum { FETCH_BAD_URL = 1, FETCH_CROSS };

/* A 401 Mill can answer: with the address's own name and password the first time, again
   with a new nonce when Digest's went stale, and otherwise by asking. sent is what went
   with the request, if anything. False shows the server's page instead. */
static Boolean answer(const Url *u, const Challenge *ch, Cred *sent, char *login_user, char *login_pass)
{
    char user[64], pass[64], place[200];
    Cred *known;
    if (sent && sent->ch.scheme == AUTH_DIGEST && ch->scheme == AUTH_DIGEST && ch->stale) {
        auth_renew(sent, ch);
        return true;
    }
    if (login_user[0]) {
        auth_keep(&keyring, u->host, u->port, u->path, ch, login_user, login_pass);
        login_user[0] = 0;
        memset(login_pass, 0, 64);
        return true;
    }
    known = auth_for_realm(&keyring, u->host, u->port, ch->realm);
    snprintf(user, sizeof user, "%s", known ? known->user : "");
    if (ch->realm[0]) snprintf(place, sizeof place, "\"%.100s\" at %.80s", ch->realm, u->host);
    else snprintf(place, sizeof place, "%.80s", u->host);
    if (!login_ask(place, sent != NULL, user, sizeof user, pass, sizeof pass)) {
        if (sent) auth_forget(&keyring, sent);  /* they didn't work: don't send them again */
        SetCursor(*GetCursor(watchCursor));
        return false;
    }
    SetCursor(*GetCursor(watchCursor));
    auth_keep(&keyring, u->host, u->port, u->path, ch, user, pass);
    memset(pass, 0, sizeof pass);
    return true;
}

/* The extra header lines for an HTTP request: cookies, and a name and password kept
   for where it goes (which *sent says, or NULL) */
static const char *request_headers(const Url *u, const char *method, Cred **sent)
{
    static char headers[1700], value[600];
    static unsigned long count;
    unsigned long now;
    int n = 0;
    ReadDateTime(&now);  /* the trap: GetDateTime is a low-memory read GCC flags */
    headers[0] = 0;
    if (accept_cookies() && jar_header(&jar, u->host, u->path, now, headers + 8, 1024) > 0) {
        memcpy(headers, "Cookie: ", 8);
        n = strlen(headers);
    }
    if ((*sent = auth_for_path(&keyring, u->host, u->port, u->path)) != NULL) {
        char cnonce[20];
        sprintf(cnonce, "%08lx%04lx", (unsigned long)TickCount(), ++count & 0xffff);
        if (auth_header(*sent, method, u->path, cnonce, value, sizeof value))
            snprintf(headers + n, sizeof headers - n, "%sAuthorization: %s", n ? "\r\n" : "", value);
        memset(value, 0, sizeof value);
    }
    return headers;
}

/* Fetch a URL into resp (a POST when content_type is given), following up to five
   redirects of the same kind and answering a site's request for a name and password;
   target becomes where it ended up, without any user:password@. Returns noErr, a MacTCP
   error, FETCH_BAD_URL, or FETCH_CROSS. */
static OSErr fetch(char *target, const char *content_type, const char *body, long body_len,
                   Response *resp, Url *u, Head *h)
{
    int redirects = 0, answers = 0;
    char login_user[64], login_pass[64];    /* from the address, until a site asks */
    char login_host[128];                   /* ...and only for its own host and port */
    unsigned short login_port = 0;
    Cred *sent = NULL;
    OSErr err;
    login_user[0] = login_pass[0] = 0;
    for (;;) {
        if (url_parse(target, u)) return FETCH_BAD_URL;
        if (u->user[0]) {
            strcpy(login_user, u->user);
            strcpy(login_pass, u->pass);
            strcpy(login_host, u->host);
            login_port = u->port;
            memset(u->pass, 0, sizeof u->pass);
            u->user[0] = 0;
            url_strip_login(target);
        }
        resp->len = resp->total = resp->body = 0;
        resp->code = 0;
        resp->mime[0] = 0;
        resp->headed = resp->cut = false;
        resp->url = target;
        resp->req = u;
        if (u->scheme == SCHEME_HTTP) {
            const char *headers = request_headers(u, content_type ? "POST" : "GET", &sent);
            err = net_http_request(u->host, u->port, u->path, headers, content_type, body, body_len, collect, resp);
        } else {
            err = local_request(u, content_type, body, body_len, collect, resp);
        }
        if (err != noErr || resp->saving || resp->declined) return err;
        split_response(resp->buf, resp->len, h);
        if (login_user[0] && (u->port != login_port || strcmp(u->host, login_host))) {
            login_user[0] = 0;  /* redirected somewhere else */
            memset(login_pass, 0, sizeof login_pass);
        }
        if (h->code == 401 && h->challenge.scheme != AUTH_NONE && u->scheme == SCHEME_HTTP && answers++ < 4 &&
            answer(u, &h->challenge, sent, login_user, login_pass))
            continue;
        if ((h->code == 301 || h->code == 302 || h->code == 303) && h->location[0] && redirects++ < 5) {
            char next[300];
            if (url_resolve(target, h->location, next, sizeof next)) return noErr;
            if (scheme_of(next) != u->scheme) return FETCH_CROSS;
            strcpy(target, next);
            content_type = NULL;  /* a redirect after a POST is fetched with GET */
            continue;
        }
        return noErr;
    }
}

/* While an image decodes: how far it's got, the sails turn, and Command-period stops */
static int decoding(void *ref, int percent)
{
    char msg[32];
    (void)ref;
    sprintf(msg, "Decoding... %d%%", percent);
    progress(msg);
    spin();
    return stop_asked();
}

static Boolean decode_image(Pic *p, Response *resp, long body)
{
    short nw, nh, w, h;
    const unsigned char *data = (unsigned char *)resp->buf + body;
    long len = resp->len - body;
    if (image_dims(data, len, &nw, &nh) || nw < 1 || nh < 1) return false;
    w = p->aw > 0 ? p->aw : p->ah > 0 ? (short)((long)nw * p->ah / nh) : nw;
    h = p->ah > 0 ? p->ah : p->aw > 0 ? (short)((long)nh * p->aw / nw) : nh;
    if (!image_decode(data, len, w, h, &p->bm, decoding, NULL)) return true;
    if (stopped) return false;
    /* short of memory: let go of kept images this page isn't showing, and try again */
    kept_trim(0, 0);
    return !image_decode(data, len, w, h, &p->bm, decoding, NULL);
}

#define IMAGES_AUTO (-1)    /* load_images: as the settings say */

static Boolean wanted(const Pic *p, short which, short i)
{
    if (p->state != PIC_WAITING) return false;
    if (which >= 0) return i == which;
    return auto_images() || !strncmp(p->url, "mill:", 5);  /* Mill's own images always load */
}

/* Fetch and show the page's images, one at a time, after its text: those the settings
   say to (IMAGES_AUTO), or just pics[which] */
static void load_images(Response *resp, short which)
{
    short i, waiting = 0, n = 0;
    Boolean resized = false;
    for (i = 0; i < npics; i++) if (wanted(&pics[i], which, i)) waiting++;
    for (i = 0; i < npics && !stop_asked(); i++) {
        Pic *p = &pics[i];
        char target[300];
        Url u;
        Head h;
        if (!wanted(p, which, i)) continue;
        if (waiting > 1) sprintf(activity, "Image %d of %d", ++n, waiting);
        else strcpy(activity, "Image");
        p->state = PIC_FAILED;
        if (!strncmp(p->url, "mill:", 5)) {
            if (local_image(p->url, &p->bm)) continue;
        } else {
            strcpy(target, p->url);
            if (fetch(target, NULL, NULL, 0, resp, &u, &h) != noErr || h.code >= 300) {
                if (stopped) p->state = PIC_WAITING;
                continue;
            }
            if (!decode_image(p, resp, h.body)) {
                if (stopped) p->state = PIC_WAITING;  /* it can still be loaded */
                continue;
            }
        }
        p->state = PIC_SHOWN;
        p->owned = !kept_add(p->url, p->aw, p->ah, &p->bm);
        if (p->aw < 0 || p->ah < 0) resized = true;  /* the page has to make room for it */
        else { SetPort(win); draw_page(i); }
    }
    activity[0] = 0;
    if (resized) {
        relayout();
        if (scroll_y > max_scroll()) scroll_y = max_scroll();
        update_scroll();
        SetPort(win);
        draw_view();
    }
    if (stopped) set_status("Stopped.");
}

static void set_title(const char *t)
{
    Str255 title;
    title[0] = (unsigned char)(strlen(t) > 250 ? 250 : strlen(t));
    memcpy(title + 1, t, title[0]);
    SetWTitle(win, title);
}

/* Open a URL (a POST when content_type is given). Returns whether a page came. */
#define VIEW_SOURCE "view-source:"

/* Text as a page of its own: in <PRE>, with & < and > escaped so none of it is read as
   HTML; the title, if any, escaped the same way. malloc'd; its length in *len. */
static char *pre_page(const char *title, const char *text, long n, long *len)
{
    long i, size = n + strlen(title) * 5 + 64;
    char *page, *o;
    for (i = 0; i < n; i++) size += text[i] == '&' ? 4 : (text[i] == '<' || text[i] == '>') ? 3 : 0;
    if (!(page = malloc(size))) return NULL;
    o = page;
    for (i = 0; i < 2; i++) {
        const char *s = i ? text : title;
        long k, m = i ? n : (long)strlen(title);
        if (!i && !m) continue;
        strcpy(o, i ? "<PRE>" : "<TITLE>");
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

static Boolean load(const char *url, Boolean record, const char *content_type, const char *body, long body_len)
{
    /* view-source:address shows what address sends, as text */
    Boolean source = !strncmp(url, VIEW_SOURCE, strlen(VIEW_SOURCE));
    char shown_url[320];
    char target[300];
    Response resp;
    Url u;
    Head h;
    OSErr err;
    Boolean shown = false, cut;

    blur();
    strncpy(target, source ? url + strlen(VIEW_SOURCE) : url, sizeof target - 1);
    target[sizeof target - 1] = 0;
    resp.buf = malloc(MAX_PAGE);
    if (!resp.buf) { set_status("Not enough memory."); return false; }
    response_init(&resp);
    resp.may_save = !source;
    set_loading(true);
    stopped = false;
    activity[0] = 0;
    memset(&u, 0, sizeof u);
    SetCursor(*GetCursor(watchCursor));
    snprintf(shown_url, sizeof shown_url, "%s%s", source ? VIEW_SOURCE : "", target);
    show_url(shown_url);
    err = fetch(target, content_type, body, body_len, &resp, &u, &h);
    snprintf(shown_url, sizeof shown_url, "%s%s", source ? VIEW_SOURCE : "", target);  /* where it ended up */
    if (resp.saving || resp.declined) {
        /* a file Mill can't show: saved, and the page stays */
        finish_save(&resp, err);
        show_url(page_url);
        goto done;
    }
    show_url(shown_url);
    if (stopped || err == userCanceledErr) { set_status("Stopped."); goto done; }
    if (err == FETCH_BAD_URL) { set_status("Mill opens http:, file:, and mill: addresses."); goto done; }
    if (err == FETCH_CROSS) { set_status("Stopped at a redirect to another kind of address."); goto done; }
    if (err != noErr) {
        char msg[200];
        if (net_stage == NET_LOOKUP)
            snprintf(msg, sizeof msg, err == commandTimeout ? "Looking up %s took too long." : "Couldn't find %s.", u.host);
        else if (net_stage == NET_CONNECT)
            snprintf(msg, sizeof msg, "Couldn't connect to %s.", u.host);
        else
            snprintf(msg, sizeof msg, "The connection to %s failed (MacTCP error %d).", u.host, err);
        set_status(msg);
        goto done;
    }

    set_status("Formatting...");
    if (have_page) { layout_free(&lay); pics_free(); forms_free(&forms); html_free(&doc); have_page = false; }
    page_number++;
    if (source || h.kind != BODY_HTML) {
        /* source and plain text show preformatted, and a picture by itself on a page */
        long n = resp.len - h.body, len;
        const char *text = resp.buf + h.body;
        char *page = NULL;
        /* the charset goes by the text itself, before escaping hides its META tag */
        int cs = h.charset != CHARSET_GUESS ? h.charset : charset_sniff(text, n);
        if (source && resp.mime[0] && !prefix_ci(resp.mime, "text/", 5)) {
            char msg[100];
            snprintf(msg, sizeof msg, "Mill can't show %s as source.", resp.mime);
            page = pre_page(msg, msg, strlen(msg), &len);
        } else if (source) {
            page = pre_page(shown_url, text, n, &len);
        } else if (h.kind == BODY_PICTURE) {
            if ((page = malloc(strlen(target) + 32))) len = sprintf(page, "<IMG SRC=\"%s\">", target);
        } else {
            page = pre_page("", text, n, &len);
        }
        if (page) {
            have_page = html_parse_charset(&doc, page, len, cs) == 0;
            free(page);
        }
    } else {
        have_page = html_parse_charset(&doc, resp.buf + h.body, resp.len - h.body, h.charset) == 0;
    }
    if (have_page && forms_build(&forms, &doc)) forms_free(&forms);
    if (!have_page) { set_status("Not enough memory for this page."); goto done; }
    strcpy(page_url, shown_url);
    if (record) remember(shown_url);
    set_title(doc.title[0] ? doc.title : shown_url);
    pics_collect();
    scroll_x = 0;
    relayout();
    scroll_y = 0;
    if (u.fragment[0]) {
        long i;
        for (i = 0; i < lay.nanchors; i++)
            if (!strcmp(lay.anchors[i].name, u.fragment)) { scroll_y = lay.anchors[i].y; break; }
        if (scroll_y > max_scroll()) scroll_y = max_scroll();
    }
    update_scroll();
    redraw();
    cut = resp.cut;
    response_init(&resp);   /* images are never saved */
    load_images(&resp, IMAGES_AUTO);
    if (!stopped) set_status(h.code >= 400 ? "The server sent an error page." :
                             cut ? "Showed the first " PAGE_K "K of this page." : "Done.");
    shown = true;
done:
    free(resp.buf);
    set_loading(false);
    InitCursor();
    InvalRect(&win->portRect);
    return shown;
}

/* What you type, choose from a menu, or go back to opens anything; pages can't reach
   across kinds of address */
static Boolean go(const char *url, Boolean record)
{
    return load(url, record, NULL, NULL, 0);
}

static Boolean same_kind(const char *url)
{
    return scheme_of(url) == scheme_of(page_url);
}

/* file: and mill: are this Mac's own: a web page (http:, and later others) can't link
   into them, as browsers since keep web pages out of file: and about:. Any other link
   may change kind. Images, forms, and redirects stay with their page's own kind. */
static Boolean local_scheme(int scheme) { return scheme == SCHEME_FILE || scheme == SCHEME_MILL; }

static Boolean may_follow(const char *url)
{
    return local_scheme(scheme_of(page_url)) || !local_scheme(scheme_of(url));
}

static void go_typed(void)
{
    char url[300];
    long n = (*url_te)->teLength;
    if (n > 290) n = 290;
    memcpy(url, *(*url_te)->hText, n);
    url[n] = 0;
    if (!strstr(url, "://")) {
        memmove(url + 7, url, n + 1);
        memcpy(url, "http://", 7);
    }
    go(url, true);
}

static void go_history(short delta)
{
    if (hist_pos + delta < 0 || hist_pos + delta >= hist_len) return;
    hist_pos += delta;
    go(history[hist_pos], false);
}

static void reload(void)
{
    if (hist_pos >= 0) go(history[hist_pos], false);
}

static void go_home(void)
{
    go(setting("homepage", MILL_DEFAULT_HOME), true);
}

/* Fetch one image a page opened without */
static void load_image(short which)
{
    Response resp;
    if (!have_page || loading) return;
    if (!(resp.buf = malloc(MAX_PAGE))) { set_status("Not enough memory."); return; }
    response_init(&resp);
    blur();
    set_loading(true);
    stopped = false;
    SetCursor(*GetCursor(watchCursor));
    load_images(&resp, which);
    if (!stopped) set_status(pics[which].state == PIC_SHOWN ? "Done." : "Mill couldn't load that image.");
    free(resp.buf);
    set_loading(false);
    InitCursor();
}

/* Save Image, Link, or Page Source As: fetch url into a file, whatever it is */
static void save_url(const char *url)
{
    Response resp;
    char target[300];
    Url u;
    Head h;
    OSErr err;
    if (loading) return;
    if (!(resp.buf = malloc(MAX_PAGE))) { set_status("Not enough memory."); return; }
    response_init(&resp);
    if (!ask_where(url, &resp.spec)) { free(resp.buf); InitCursor(); return; }
    resp.have_spec = resp.save_all = true;
    blur();
    set_loading(true);
    stopped = false;
    activity[0] = 0;
    SetCursor(*GetCursor(watchCursor));
    snprintf(target, sizeof target, "%s", url);
    err = fetch(target, NULL, NULL, 0, &resp, &u, &h);
    if (resp.saving) finish_save(&resp, err);
    else if (stopped) set_status("Stopped.");
    else if (err == noErr && h.code >= 300) set_status("The server didn't send it.");
    else set_status("Mill couldn't get it.");
    free(resp.buf);
    set_loading(false);
    InitCursor();
}

static long read_upload(void *ctx, const char *path, char **data)
{
    (void)ctx;
    return read_mac_file(path, data, MAX_UPLOAD);
}

/* Send a form, as its field by (a button, or -1 for Return in a text field) sends it */
static void submit(long form, long by, short x, short y)
{
    Submission s;
    char target[300], content_type[80];
    char *q;
    blur();
    if (forms_submit(&forms, &doc, form, by, x, y, read_upload, NULL, &s)) { set_status("Not enough memory to send the form."); return; }
    if (url_resolve(page_url, s.action[0] ? s.action : page_url, target, sizeof target)) {
        set_status("Mill can't send this form to that address.");
    } else if (!same_kind(target)) {
        set_status(FORM_MSG);
    } else {
        if ((q = strchr(target, '#'))) *q = 0;
        if (s.post) {
            if (s.multipart) sprintf(content_type, "multipart/form-data; boundary=%s", s.boundary);
            else strcpy(content_type, "application/x-www-form-urlencoded");
            load(target, true, content_type, s.data, s.len);
        } else {
            if ((q = strchr(target, '?'))) *q = 0;
            if (strlen(target) + s.len + 2 > sizeof target) set_status("This form's data is too long for an address.");
            else { strcat(target, "?"); strcat(target, s.data); go(target, true); }
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
    if (!have_page || !PtInRect(p, &view_r)) return NULL;
    return layout_hit(&lay, scroll_y + (p.v - view_r.top), p.h - PAGE_LEFT);
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
    return r && r->kind == RUN_IMAGE && !r->link && pic_of && pic_of[r->text] &&
           pics[pic_of[r->text] - 1].state == PIC_WAITING ? r : NULL;
}

static void follow(const Run *r)
{
    char target[300];
    if (url_resolve(page_url, lay.links[r->link - 1], target, sizeof target)) {
        set_status("Mill opens http:, file:, and mill: addresses.");
        return;
    }
    if (!may_follow(target)) { set_status(FILE_MSG); return; }
    go(target, true);
}

/* A run's box in the window */
static Boolean run_box(const Run *r, Rect *box)
{
    long k = r - lay.runs, i;
    for (i = 0; i < lay.nlines; i++) {
        const MLine *ln = &lay.lines[i];
        if (k >= ln->first && k < ln->first + ln->count) {
            short top = view_r.top + (short)(ln->y - scroll_y) + ln->ascent - r->font;
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
    if ((map = imagemap_for(&doc, r->text)) >= 0) {
        if ((area = imagemap_hit(&doc, map, x, y)) < 0) return -1;
        return url_resolve(page_url, html_attr(&doc, &doc.nodes[area], "href"), out, size) ? -1 : 1;
    }
    if (r->link && html_attr(&doc, &doc.nodes[r->text], "ismap")) {
        long n;
        if (url_resolve(page_url, lay.links[r->link - 1], out, size)) return -1;
        n = strcspn(out, "?#");
        snprintf(out + n, size - n, "?%d,%d", x < 0 ? 0 : x, y < 0 ? 0 : y);
        return 1;
    }
    return 0;
}

static void go_map(const char *target)
{
    if (!may_follow(target)) set_status(FILE_MSG);
    else go(target, true);
}

enum { CM_LOAD_IMAGE = 1, CM_VIEW_IMAGE, CM_SAVE_IMAGE, CM_SAVE_LINK, CM_VIEW_SOURCE, CM_SAVE_SOURCE };
#define HOLD 30         /* ticks the button is held for the menu, as Mac browsers did */

/* The menu for what's under the mouse: an image, a link, or the page itself */
static void context_menu(Point p, const Run *r)
{
    MenuHandle m = NewMenu(MENU_CONTEXT, "\p");
    short codes[6], n = 0;
    long choice;
    Point where = p;
    const char *page = !strncmp(page_url, VIEW_SOURCE, strlen(VIEW_SOURCE)) ? page_url + strlen(VIEW_SOURCE) : page_url;
    short pic = r && r->kind == RUN_IMAGE && pic_of && pic_of[r->text] ? pic_of[r->text] - 1 : -1;
    char target[320];
    if (!m || !have_page) return;
    if (pic >= 0) {
        AppendMenu(m, "\pLoad Image");
        codes[n++] = CM_LOAD_IMAGE;
        if (pics[pic].state == PIC_SHOWN) DisableItem(m, n);
        AppendMenu(m, "\pView Image");
        codes[n++] = CM_VIEW_IMAGE;
        AppendMenu(m, "\pSave Image As...");
        codes[n++] = CM_SAVE_IMAGE;
    }
    if (r && r->link && r->kind != RUN_CONTROL) {
        AppendMenu(m, "\pSave Link As...");
        codes[n++] = CM_SAVE_LINK;
    }
    if (!n) {
        AppendMenu(m, "\pView Source");
        codes[n++] = CM_VIEW_SOURCE;
        if (page != page_url) DisableItem(m, n);  /* already */
        AppendMenu(m, "\pSave Page Source As...");
        codes[n++] = CM_SAVE_SOURCE;
    }
    InsertMenu(m, -1);
    LocalToGlobal(&where);
    choice = PopUpMenuSelect(m, where.v, where.h, 0);
    DeleteMenu(MENU_CONTEXT);
    DisposeMenu(m);
    if (!LoWord(choice)) return;
    switch (codes[LoWord(choice) - 1]) {
    case CM_LOAD_IMAGE:
        pics[pic].state = PIC_WAITING;  /* one that failed can be tried again */
        load_image(pic);
        break;
    case CM_VIEW_IMAGE:
        go(pics[pic].url, true);  /* on a page of its own */
        break;
    case CM_SAVE_IMAGE:
        save_url(pics[pic].url);
        break;
    case CM_SAVE_LINK:
        if (url_resolve(page_url, lay.links[r->link - 1], target, sizeof target)) set_status("Mill opens http:, file:, and mill: addresses.");
        else if (!may_follow(target)) set_status(FILE_MSG);
        else save_url(target);
        break;
    case CM_VIEW_SOURCE:
        snprintf(target, sizeof target, "%s%s", VIEW_SOURCE, page);
        go(target, true);
        break;
    case CM_SAVE_SOURCE:
        save_url(page);
        break;
    }
}

/* A click on the page: a link opens and an image waiting to load loads, when the button
   comes up over it; held down, or with Control, it's the menu */
static void page_click(Point p, EventRecord *ev)
{
    const Run *r = run_at(p);
    Point now;
    if (!(ev->modifiers & ControlKey)) {
        while (StillDown() && TickCount() - ev->when < HOLD) {}
        if (!StillDown()) {
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
    long page = view_r.bottom - view_r.top - 16, line = 16;
    if (c == hscroll) {
        page = view_r.right - view_r.left - 16;
        switch (part) {
        case inUpButton: scroll_x_to(scroll_x - line); break;
        case inDownButton: scroll_x_to(scroll_x + line); break;
        case inPageUp: scroll_x_to(scroll_x - page); break;
        case inPageDown: scroll_x_to(scroll_x + page); break;
        }
        return;
    }
    switch (part) {
    case inUpButton: scroll_to(scroll_y - line); break;
    case inDownButton: scroll_to(scroll_y + line); break;
    case inPageUp: scroll_to(scroll_y - page); break;
    case inPageDown: scroll_to(scroll_y + page); break;
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
        forms_option_label(&doc, f->options[i], label, sizeof label);
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

/* A click on a form control: true if it was one */
static Boolean click_control(Point p, EventRecord *ev)
{
    const Run *r = run_at(p);
    long i;
    Field *f;
    Rect box;
    if (!r || r->kind != RUN_CONTROL || (i = forms_find(&forms, r->text)) < 0 || !control_box(r->text, &box)) return false;
    f = &forms.fields[i];
    switch (f->type) {
    case FIELD_CHECKBOX: case FIELD_RADIO:
        if (press(&box)) { forms_click(&forms, i); draw_view(); }
        break;
    case FIELD_SUBMIT: case FIELD_IMAGE:
        if (press(&box)) submit(f->form, i, p.h - box.left, p.v - box.top);
        break;
    case FIELD_RESET:
        if (press(&box)) {
            if (focus >= 0 && forms.fields[focus].form == f->form) { TEDeactivate(field_te); focus = -1; TEActivate(url_te); }
            forms_reset(&forms, &doc, f->form);
            draw_view();
        }
        break;
    case FIELD_SELECT:
        if (list_rows(f) && f->noptions <= list_rows(f)) {
            short row = (p.v - box.top - 2) / LINE_H;
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
        button.left = text_part(f, box).right + 6;
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
    go(url, true);
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

/* The slot as a pop-up menu: a box with a shadow, as System 7 draws them */
static pascal void draw_slot(DialogPtr d, short item)
{
    Rect r;
    Str255 s;
    item_rect(d, item, &r);
    r.right -= 1;
    r.bottom -= 1;
    EraseRect(&r);
    FrameRect(&r);
    MoveTo(r.left + 2, r.bottom);
    LineTo(r.right, r.bottom);
    LineTo(r.right, r.top + 2);
    slot_text(bd_slot, s);
    MoveTo(r.left + 6, r.bottom - 5);
    DrawString(s);
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
    Url pu;
    if (!page_url[0]) return;
    if (!url_parse(page_url, &pu) && pu.scheme == SCHEME_HTTP) login = auth_for_path(&keyring, pu.host, pu.port, pu.path);
    blur();
    if (slot < 0) {
        for (slot = 0; slot < BOOKMARKS && bookmark(slot, url, sizeof url, title, sizeof title); slot++) {}
        if (slot == BOOKMARKS) slot = 0;
    }
    bd_slot = slot;
    if (!(d = GetNewDialog(200, NULL, (WindowPtr)-1))) return;
    /* centered a third of the way down, as alerts are */
    MoveWindow(d, (qd.screenBits.bounds.right - (d->portRect.right - d->portRect.left)) / 2,
               20 + (qd.screenBits.bounds.bottom - 20 - (d->portRect.bottom - d->portRect.top)) / 3, false);
    GetDialogItem(d, BD_SLOT, &type, &h, &r);
    SetDialogItem(d, BD_SLOT, type, (Handle)(UserItemUPP)draw_slot, &r);
    GetDialogItem(d, BD_RING, &type, &h, &r);
    SetDialogItem(d, BD_RING, type, (Handle)(UserItemUPP)draw_ring, &r);
    pstr_of(doc.title[0] ? doc.title : page_url, s);
    GetDialogItem(d, BD_TITLE, &type, &h, &r);
    SetDialogItemText(h, s);
    pstr_of(page_url, s);
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
                SetPort(win);
                set_status("That address is too long to save with a name and password.");
                return;
            }
            strcpy(url, with);
            memset(with, 0, sizeof with);
        }
    }
    DisposeDialog(d);
    SetPort(win);
    if (item != BD_OK) return;
    n = bd_slot == 9 ? 0 : bd_slot + 1;
    if (url[0]) {
        Url u;
        if (url_parse(url, &u)) { set_status("That isn't an address Mill can open."); return; }
    }
    if (bookmark_set(bd_slot, url, title) != noErr) sprintf(msg, "Mill couldn't save its bookmarks.");
    else if (url[0]) sprintf(msg, "Bookmarked as Command-%d.", n);
    else sprintf(msg, "Bookmark %d is empty now.", n);
    set_status(msg);
}

static void open_bookmark(short n)
{
    char url[300], title[100];
    if (bookmark(n, url, sizeof url, title, sizeof title)) go(url, true);
}

/* The Bookmarks menu shows the slots as they are now */
static void refresh_bookmarks(void)
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
    if (page_url[0]) EnableItem(m, BM_ADD);
    else DisableItem(m, BM_ADD);
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
    return focus >= 0 && forms.fields[focus].type != FIELD_PASSWORD ? field_te : url_te;
}

static void do_menu(long choice)
{
    short menu = HiWord(choice), item = LoWord(choice);
    if (menu == MENU_APPLE) {
        if (item == 1) {
            go("mill://about", true);
        } else {
            Str255 name;
            GetMenuItemText(GetMenuHandle(MENU_APPLE), item, name);
            OpenDeskAcc(name);
        }
    } else if (menu == MENU_FILE) {
        if (item == FILE_LOCATION) { blur(); TESetSelect(0, 32767, url_te); }
        if (item == FILE_OPEN) open_file();
        if (item == FILE_QUIT) quitting = true;
    } else if (menu == MENU_EDIT) {
        if (item == EDIT_CUT) TECut(typing_te());
        if (item == EDIT_COPY) TECopy(typing_te());
        if (item == EDIT_PASTE) TEPaste(typing_te());
        if (item == EDIT_PREFERENCES) go("mill://config", true);
    } else if (menu == MENU_BOOKMARKS) {
        if (item == BM_ADD) bookmark_dialog(-1);
        else if (item == BM_EDIT) go("mill://config#bookmarks", true);
        else if (item >= BM_FIRST && item < BM_FIRST + BOOKMARKS) open_bookmark(item - BM_FIRST);
    } else if (menu == MENU_GO) {
        if (item == GO_BACK) go_history(-1);
        if (item == GO_FORWARD) go_history(1);
        if (item == GO_RELOAD) reload();
        if (item == GO_HOME) go_home();
    }
    HiliteMenu(0);
}

static void resized(void)
{
    Rect t;
    geometry();
    t = (*url_te)->viewRect;
    t.right = mill_r.left - 4;
    (*url_te)->viewRect = t;
    MoveControl(vscroll, win->portRect.right - SCROLL + 1, BAR);
    SizeControl(vscroll, SCROLL, win->portRect.bottom - BAR - SCROLL + 2);
    place_hbar();
    relayout();
    if (scroll_y > max_scroll()) scroll_y = max_scroll();
    update_scroll();
    InvalRect(&win->portRect);
}

static void do_mouse(EventRecord *ev)
{
    WindowPtr w;
    short part = FindWindow(ev->where, &w);
    switch (part) {
    case inMenuBar: refresh_bookmarks(); do_menu(MenuSelect(ev->where)); break;
    case inSysWindow: SystemClick(ev, w); break;
    case inDrag: DragWindow(w, ev->where, &qd.screenBits.bounds); break;
    case inGoAway: if (TrackGoAway(w, ev->where)) quitting = true; break;
    case inGrow: {
        Rect limits;
        long size;
        SetRect(&limits, 240, 120, 32767, 32767);
        if ((size = GrowWindow(w, ev->where, &limits))) { SizeWindow(w, LoWord(size), HiWord(size), true); resized(); }
        break;
    }
    case inZoomIn: case inZoomOut:
        if (TrackBox(w, ev->where, part)) { SetPort(w); EraseRect(&w->portRect); ZoomWindow(w, part, false); resized(); }
        break;
    case inContent: {
        Point p = ev->where;
        ControlHandle c;
        short cpart;
        GlobalToLocal(&p);
        if ((cpart = FindControl(p, w, &c))) {
            if (cpart == inThumb) {
                TrackControl(c, p, NULL);
                if (c == hscroll) scroll_x_to(GetControlValue(c));
                else scroll_to((long)GetControlValue(c) * scroll_scale);
            } else {
                TrackControl(c, p, (ControlActionUPP)scroll_action);
            }
        } else if (PtInRect(p, &back_r)) { if (hist_pos > 0 && press(&back_r)) go_history(-1); }
        else if (PtInRect(p, &fwd_r)) { if (hist_pos < hist_len - 1 && press(&fwd_r)) go_history(1); }
        else if (PtInRect(p, &reload_r)) { if (hist_pos >= 0 && press(&reload_r)) reload(); }
        else if (PtInRect(p, &(*url_te)->viewRect)) { blur(); TEClick(p, (ev->modifiers & shiftKey) != 0, url_te); }
        else if (click_control(p, ev)) {}
        else page_click(p, ev);
        break;
    }
    }
}

static void do_key(EventRecord *ev)
{
    char c = ev->message & charCodeMask;
    long page = view_r.bottom - view_r.top - 16;
    Boolean in_area = focus >= 0 && forms.fields[focus].type == FIELD_TEXTAREA;
    if (ev->modifiers & cmdKey) {
        if (c == '[') { go_history(-1); return; }
        if (c == ']') { go_history(1); return; }
        if (c == 'r' || c == 'R') { reload(); return; }
        if ((ev->modifiers & optionKey) && option_digit(ev) >= 0) { bookmark_dialog(option_digit(ev)); return; }
        refresh_bookmarks();
        do_menu(MenuKey(c));
        return;
    }
    /* control characters TextEdit would draw as boxes: Escape, Clear, forward delete... */
    if (((unsigned char)c < ' ' && !strchr("\b\t\r\003\001\004\013\014\034\035\036\037", c)) || c == 0x7F) return;
    if (c == '\t' && forms.n) { next_field((ev->modifiers & shiftKey) ? -1 : 1); return; }
    switch (c) {
    case 0x0B: scroll_to(scroll_y - page); return;   /* page up */
    case 0x0C: scroll_to(scroll_y + page); return;   /* page down */
    case 0x01: scroll_to(0); return;                 /* home */
    case 0x04: scroll_to(max_scroll()); return;      /* end */
    case 0x1E: if (!in_area) { scroll_to(scroll_y - 16); return; } break;  /* up arrow */
    case 0x1F: if (!in_area) { scroll_to(scroll_y + 16); return; } break;  /* down arrow */
    }
    if (focus >= 0) {
        if ((c == '\r' && !in_area) || c == 3) { submit(forms.fields[focus].form, -1, 0, 0); return; }
        type_in_field(c, ev);
        return;
    }
    if (c == '\r' || c == 3) { go_typed(); return; }
    TEKey(c, url_te);
}

/* Show a link's address in the status line while the mouse is over it, as browsers did */
static void hover(void)
{
    static const Run *last = NULL;
    static char last_map[300];   /* on an image map: the address last shown */
    Point p;
    const Run *r;
    char target[300];
    int m;
    if (loading) return;
    SetPort(win);
    GetMouse(&p);
    if (!(r = link_at(p))) r = waiting_image_at(p);
    /* an image map's address changes as the mouse moves across it */
    m = r && waiting_image_at(p) ? 0 : map_target(p, run_at(p), target, sizeof target);
    if (m > 0) {
        last = run_at(p);
        if (strcmp(target, last_map) || !status_up) {
            strcpy(last_map, target);
            set_status(target);
            status_held = true;
        }
        return;
    }
    if (m < 0) {
        last = run_at(p);
        last_map[0] = 0;
        if (status_held) hide_status();
        return;
    }
    last_map[0] = 0;
    if (r == last) return;
    last = r;
    if (r && !r->link) {
        set_status("Use the context menu to load this image.");
        status_held = true;
    } else if (r) {
        if (url_resolve(page_url, lay.links[r->link - 1], target, sizeof target))
            snprintf(target, sizeof target, "%s", lay.links[r->link - 1]);
        set_status(target);
        status_held = true;
    } else if (status_held) {
        hide_status();  /* the link's address goes with the mouse */
    }
}

static void setup_menus(void)
{
    MenuHandle m = NewMenu(MENU_APPLE, "\p\024");
    AppendMenu(m, "\pAbout Mill...;(-");
    AppendResMenu(m, 'DRVR');
    InsertMenu(m, 0);
    m = NewMenu(MENU_FILE, "\pFile");
    AppendMenu(m, "\pOpen Location.../L;Open File.../O;(-;Quit/Q");
    InsertMenu(m, 0);
    m = NewMenu(MENU_EDIT, "\pEdit");
    AppendMenu(m, "\p(Undo/Z;(-;Cut/X;Copy/C;Paste/V;(-;Preferences...");
    InsertMenu(m, 0);
    m = NewMenu(MENU_GO, "\pGo");
    AppendMenu(m, "\pBack/[;Forward/];Reload/R;Home/H");
    InsertMenu(m, 0);
    m = NewMenu(MENU_BOOKMARKS, "\pBookmarks");
    AppendMenu(m, "\pAdd Bookmark.../D;(-");
    {
        /* the ten slots, Command-1 to Command-9 and Command-0; their names come later */
        static unsigned char key[] = "\p /1";
        short n;
        for (n = 0; n < BOOKMARKS; n++) {
            key[3] = (unsigned char)(n == 9 ? '0' : '1' + n);
            AppendMenu(m, key);
        }
    }
    AppendMenu(m, "\p(-;Edit Bookmarks...");
    InsertMenu(m, 0);
    DrawMenuBar();
}

static void net_progress(const char *msg) { progress(msg); }

/* While the network keeps us waiting: the sails turn, and Command-period stops */
static Boolean net_waiting(void)
{
    spin();
    return stop_asked();
}

int mill_run(WindowPtr splash)
{
    Rect r, t;
    EventRecord ev;
    Boolean net_ok;

    sys_init();  /* System 6 or 7: which calls to use */
    InitCursor();
    setup_menus();

    /* Fits a 512x342 screen, the smallest a Mill Mac has */
    r = qd.screenBits.bounds;
    SetRect(&r, r.left + 2, r.top + 40, r.right - 2, r.bottom - 2);
    win = NewWindow(NULL, &r, "\pMill", true, documentProc + 8 /* with a zoom box */, splash, true, 0);
    SetPort(win);
    geometry();
    SetRect(&t, reload_r.right + 6, 3, mill_r.left - 4, BAR - 2);
    TextFont(kFontIDGeneva);
    TextSize(9);
    url_te = TENew(&t, &t);
    (*url_te)->destRect.right = t.left + 2000;  /* one long line: a URL never wraps */
    field_te = TENew(&t, &t);                   /* moved over a field when one is typed in */
    TEActivate(url_te);
    SetRect(&t, 0, 0, 10, 10);
    vscroll = NewControl(win, &t, "\p", true, 0, 0, 0, scrollBarProc, 0);
    hscroll = NewControl(win, &t, "\p", false, 0, 0, 0, scrollBarProc, 0);
    resized();

    settings_load();
    net_status = net_progress;
    net_idle = net_waiting;
    net_ok = net_init() == noErr;
    DisposeWindow(splash);  /* the browser is ready */
    SetPort(win);

    /* the home page, or Mill's own page when the network isn't there */
    if (!net_ok || !go(setting("homepage", MILL_DEFAULT_HOME), true)) {
        char why[128];
        strcpy(why, net_ok ? status_text : "MacTCP isn't available.");
        go("mill://about", true);
        set_status(why);  /* why it isn't the home page */
    }

    while (!quitting) {
        if (focus >= 0 && focus_shown && forms.fields[focus].type != FIELD_PASSWORD) {
            RgnHandle save;
            SetPort(win);
            save = clip_to(&focus_clip);
            TEIdle(field_te);
            unclip(save);
        } else if (focus < 0) {
            TEIdle(url_te);
        }
        if (!sys_next_event(&ev, 10)) { hover(); status_idle(); continue; }
        switch (ev.what) {
        case mouseDown: do_mouse(&ev); break;
        case keyDown: case autoKey: do_key(&ev); break;
        case updateEvt:
            BeginUpdate((WindowPtr)ev.message);
            redraw();
            EndUpdate((WindowPtr)ev.message);
            break;
        }
    }
    net_shutdown();
    return 0;
}
