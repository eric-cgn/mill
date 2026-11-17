/* Layout, run for run: lays out the test pages with several made-up fonts, widths, and
   zooms, and checks each layout against a hash of how it came out before (layout.sums).
   Changes that should leave layout alone (faster measuring, say) must keep every run,
   line, box, anchor, and link the same.

     test_layout sums             print the hashes (to make layout.sums)
     test_layout dump PAGE SET    print the layout of one page, in one setting, in full
     test_layout                  check against tests/fixtures/layout.sums */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/html.h"
#include "../src/core/layout.h"

static const char *PAGES[] = {
    "tests/fixtures/front.html", "tests/fixtures/story.html", "tests/fixtures/weather.html",
    "tests/fixtures/html32.html", "tests/fixtures/tables-nested.html", "tests/fixtures/tables-portal.html",
    "tests/fixtures/tables-config.html", "tests/fixtures/tables-forum.html", "src/license.html",
    "src/changes.html",
};
#define NPAGES (int)(sizeof PAGES / sizeof PAGES[0])

/* Two made-up fonts. "add": each character a whole number of pixels, so a run is as wide
   as its pieces (a font at a size it has a strike of), and adds_up says so. "frac": widths
   in fractions, rounded once for the whole run, so pieces add up differently (a scaled font). */
static const unsigned char BASE[16] = { 6, 7, 5, 7, 4, 8, 6, 3, 9, 5, 6, 7, 4, 10, 6, 5 };
static long calls;

static long add_width(void *ctx, const MStyle *s, const char *t, long n)
{
    long w = 0, i;
    (void)ctx;
    calls++;
    for (i = 0; i < n; i++) w += (BASE[t[i] & 15] * (s->font % 5 + 9) * s->size + 60) / 120 + ((s->face & FACE_BOLD) != 0);
    return w;
}

static int adds_up(void *ctx, const MStyle *s)
{
    (void)ctx;
    (void)s;
    return 1;
}

static long frac_width(void *ctx, const MStyle *s, const char *t, long n)
{
    long w = 0, i;
    (void)ctx;
    calls++;
    for (i = 0; i < n; i++) w += BASE[(t[i] >> 1) & 15] * (s->font % 3 + 11);
    w = (w * s->size * 113 / 100 + 660) / 1320;
    return w + ((s->face & FACE_BOLD) ? n / 2 : 0);
}

static void metrics(void *ctx, const MStyle *s, short *a, short *d, short *l)
{
    (void)ctx;
    *a = (short)(s->size * 3 / 4 + (s->face & FACE_BOLD ? 1 : 0) + s->font % 2);
    *d = (short)(s->size / 4 + (s->face & FACE_ITALIC ? 1 : 0));
    *l = (short)(s->size > 12);
}

/* Some pictures have come in (their size known), some haven't */
static int image_size(void *ctx, long node, short *w, short *h)
{
    (void)ctx;
    if (node % 3) return 0;
    *w = (short)(20 + node % 50);
    *h = (short)(10 + node % 30);
    return 1;
}

static int control_size(void *ctx, long node, short *w, short *h, short *asc)
{
    (void)ctx;
    *w = (short)(40 + node % 60);
    *h = 16;
    *asc = 12;
    return 1;
}

static int font_by_name(void *ctx, const char *name)
{
    (void)ctx;
    if (!strcmp(name, "Arial") || !strcmp(name, "Helvetica")) return 21;
    if (!strcmp(name, "Times")) return 20;
    return -1;
}

typedef struct { const char *name; int frac; short width, zoom; char text_only; } Setting;
static const Setting SETS[] = {
    { "add-480", 0, 480, 0, 0 }, { "frac-480", 1, 480, 0, 0 }, { "add-200", 0, 200, 0, 0 },
    { "frac-1000", 1, 1000, 0, 0 }, { "add-480-z18", 0, 480, 18, 0 }, { "frac-480-z14t", 1, 480, 14, 1 },
};
#define NSETS (int)(sizeof SETS / sizeof SETS[0])

static char *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    char *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc(*len + 1);
    if (fread(b, 1, *len, f) != (size_t)*len) { fclose(f); free(b); return NULL; }
    fclose(f);
    return b;
}

/* The layout as text, every field of everything in it; hashed as it's written */
static unsigned long hash;
static FILE *dump_to;
static void put(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void put(const char *fmt, ...)
{
    char b[400];
    const unsigned char *p;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    for (p = (const unsigned char *)b; *p; p++) hash = (hash ^ *p) * 16777619UL & 0xFFFFFFFFUL;
    if (dump_to) fputs(b, dump_to);
}

static int lay(const char *page, const Setting *st, int *err)
{
    long len, i;
    char *src = slurp(page, &len);
    Doc d;
    Layout L;
    Metrics m;
    if (!src) { printf("FAIL can't read %s\n", page); return -1; }
    memset(&d, 0, sizeof d);
    html_parse(&d, src, len);
    memset(&m, 0, sizeof m);
    m.text_width = st->frac ? frac_width : add_width;
    m.adds_up = st->frac ? NULL : adds_up;
    m.font_metrics = metrics;
    m.image_size = image_size;
    m.control_size = control_size;
    m.font_by_name = font_by_name;
    m.zoom = st->zoom;
    m.text_only = st->text_only;
    *err = layout_doc(&L, &d, &m, st->width);
    hash = 2166136261UL;
    put("%s %s: %d, height %ld width %d tallest %d\n", page, st->name, *err, L.height, L.width, L.tallest);
    for (i = 0; i < L.nlines; i++) {
        const MLine *ln = &L.lines[i];
        long k;
        put("line %ld y %ld h %d a %d\n", i, ln->y, ln->height, ln->ascent);
        for (k = ln->first; k < ln->first + ln->count; k++) {
            const Run *r = &L.runs[k];
            put("  run %ld kind %d x %d w %d font %d size %d face %d link %d len %d: ", k, r->kind, r->x, r->width, r->font, r->size, r->face, r->link, r->len);
            if (r->kind == RUN_TEXT) put("%.*s\n", r->len, d.text + r->text);
            else if (r->kind == RUN_GEN) put("[%.*s]\n", r->len, L.gen + r->text);
            else put("<%ld>\n", r->text);
        }
    }
    put("%ld runs\n", L.nruns);
    for (i = 0; i < L.nboxes; i++) put("box y %ld x %d w %d h %d thick %d\n", L.boxes[i].y, L.boxes[i].x, L.boxes[i].w, L.boxes[i].h, L.boxes[i].thick);
    for (i = 0; i < L.nanchors; i++) put("anchor %s y %ld\n", L.anchors[i].name, L.anchors[i].y);
    for (i = 0; i < L.nlinks; i++) put("link %ld %s\n", i + 1, L.links[i]);
    layout_free(&L);
    html_free(&d);
    free(src);
    return 0;
}

int main(int argc, char **argv)
{
    int p, s, err, failures = 0, checked = 0;
    if (argc == 4 && !strcmp(argv[1], "dump")) {
        dump_to = stdout;
        for (s = 0; s < NSETS; s++) if (!strcmp(SETS[s].name, argv[3])) return lay(argv[2], &SETS[s], &err) ? 1 : 0;
        return 1;
    }
    if (argc == 2 && !strcmp(argv[1], "sums")) {
        for (p = 0; p < NPAGES; p++)
            for (s = 0; s < NSETS; s++) {
                calls = 0;
                if (lay(PAGES[p], &SETS[s], &err)) return 1;
                printf("%s %s %08lx\n", PAGES[p], SETS[s].name, hash);
                fprintf(stderr, "%s %s: %ld width calls\n", PAGES[p], SETS[s].name, calls);
            }
        return 0;
    }
    {
        long len;
        char *sums = slurp("tests/fixtures/layout.sums", &len), *line;
        if (!sums) { printf("FAIL no tests/fixtures/layout.sums\n"); return 1; }
        sums[len] = 0;
        for (p = 0; p < NPAGES; p++)
            for (s = 0; s < NSETS; s++) {
                char want[300];
                if (lay(PAGES[p], &SETS[s], &err)) return 1;
                snprintf(want, sizeof want, "%s %s %08lx\n", PAGES[p], SETS[s].name, hash);
                checked++;
                for (line = sums; *line; line = strchr(line, '\n') + 1) if (!strncmp(line, want, strlen(want))) break;
                if (!*line) { printf("FAIL layout of %s (%s) changed: %s", PAGES[p], SETS[s].name, want); failures++; }
                if (err) { printf("FAIL layout of %s (%s) returned %d\n", PAGES[p], SETS[s].name, err); failures++; }
            }
        free(sums);
    }
    printf("%d layouts, %d changed\n", checked, failures);
    return failures != 0;
}
