/* Flow a parsed page into lines of styled runs, for a given width. Text measuring
   is supplied by the platform (QuickDraw on the Mac, fixed metrics in tests). */
#ifndef MILL_LAYOUT_H
#define MILL_LAYOUT_H

#include "html.h"

/* A style's font is a Mac font number, or one of these two for the platform to resolve: the
   proportional font (most text) and the fixed font (PRE, TT, CODE, KBD, SAMP); the settings
   choose them. QuickDraw face bits follow. */
enum { FONT_PROP = 0x7FF0, FONT_FIXED = 0x7FF1 };
enum { FACE_BOLD = 1, FACE_ITALIC = 2, FACE_UNDERLINE = 4 };
/* Text on a dark background (a BGCOLOR under half bright), drawn white; a bit QuickDraw
   doesn't use, to take off before TextFace */
#define FACE_INVERSE 0x80

typedef struct { short font, size; unsigned char face; } MStyle;

typedef struct {
    long (*text_width)(void *ctx, const MStyle *s, const char *text, long len);
    void (*font_metrics)(void *ctx, const MStyle *s, short *ascent, short *descent, short *leading);
    /* An image's size once it has loaded (returns 1), else 0; may be NULL */
    int (*image_size)(void *ctx, long node, short *w, short *h);
    /* A form control's size and its height above the baseline; returns 0 if it isn't shown
       (hidden fields); may be NULL */
    int (*control_size)(void *ctx, long node, short *w, short *h, short *ascent);
    void *ctx;
    /* How far along it is, 0 to 100, each time that changes; returns nonzero to stop.
       May be NULL. */
    int (*progress)(void *ctx, int percent);
    /* The font number of an installed font by name (FACE=), or -1 if there isn't one; may be NULL */
    int (*font_by_name)(void *ctx, const char *name);
    /* Page zoom: every size and distance is this over 9 (9 or 0 is normal; 36 is 4 times), rounded.
       The platform sets it from HTML 3.2's size ladder, 9, 10, 12, 14, 18, 24, 36. */
    short zoom;
    /* with zoom, scale only the text's sizes: distances and pictures stay at normal size */
    char text_only;
} Metrics;

enum { RUN_TEXT, RUN_GEN, RUN_RULE, RUN_IMAGE, RUN_CONTROL };

/* RUN_IMAGE and RUN_CONTROL: text is the IMG (or INPUT, SELECT, TEXTAREA) node, size its
   height, font its height above the baseline, and face an image's border width */
typedef struct {
    long text;          /* offset into Doc.text (RUN_TEXT) or Layout.gen (RUN_GEN) */
    short len;
    short x, width;
    short font, size;
    unsigned char face, kind;
    short link;         /* 1-based index into Layout.links, 0 for none */
} Run;

typedef struct {
    long y;
    short height, ascent;
    long first, count;  /* runs */
} MLine;

/* A frame: table and cell borders. thick 0 is a black fill (a dark BGCOLOR), and -1 a
   white one (a light cell inside a dark one); a fill comes after the fills inside it, so
   fills are drawn last to first, before any frames. */
typedef struct { long y; short x, w, h, thick; } Box;

typedef struct { char *name; long y; } Anchor;

typedef struct {
    MLine *lines; long nlines, cap_lines;   /* sorted by y; table cells sit side by side */
    Run *runs; long nruns, cap_runs;
    char *gen; long gen_len, cap_gen;
    char **links; long nlinks, cap_links;
    Anchor *anchors; long nanchors, cap_anchors;
    Box *boxes; long nboxes, cap_boxes;
    long height;
    short width, tallest;                   /* tallest line, for finding the lines near a y */
} Layout;

/* Returns 0, -1 when out of memory, or -2 when progress said to stop */
int layout_doc(Layout *L, const Doc *d, const Metrics *m, short width);
void layout_free(Layout *L);
/* The run under a point in page coordinates, or NULL */
const Run *layout_hit(const Layout *L, long y, short x);
/* The first line that could reach y; draw from here while lines start above the bottom */
long layout_line_at(const Layout *L, long y);

#endif
