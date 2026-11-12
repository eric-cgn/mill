/* Host tests for Mill's portable core, with fixed fake font metrics. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/auth.h"
#include "../src/core/charset.h"
#include "../src/core/cookie.h"
#include "../src/core/filetypes.h"
#include "../src/core/form.h"
#include "../src/core/html.h"
#include "../src/core/imagemap.h"
#include "../src/core/image.h"
#include "../src/core/layout.h"
#include "../src/core/url.h"

/* run.sh builds everything with malloc, calloc, and realloc renamed to these, so a test
   can make the core run out of memory: allocation number fail_at fails (counting from 0),
   and with fail_rest every one after it too, as a full heap would */
#undef malloc
#undef calloc
#undef realloc
void *malloc(size_t n);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t n);
static long allocs, fail_at = -1;
static int fail_rest;
static int fails(void)
{
    long k = allocs++;
    return fail_at >= 0 && (k == fail_at || (fail_rest && k > fail_at));
}
void *t_malloc(size_t n) { return fails() ? NULL : malloc(n); }
void *t_calloc(size_t n, size_t size) { return fails() ? NULL : calloc(n, size); }
void *t_realloc(void *p, size_t n) { return fails() ? NULL : realloc(p, n); }

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static long fake_width(void *ctx, const MStyle *s, const char *t, long len) { (void)ctx; (void)t; return len * s->size / 2; }
static void fake_metrics(void *ctx, const MStyle *s, short *a, short *d, short *l) { (void)ctx; *a = s->size * 3 / 4; *d = s->size / 4; *l = 1; }
static Metrics M = { fake_width, fake_metrics, NULL, NULL, NULL, NULL };

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

static void test_charset(void)
{
    CHECK(entity_to_mac("eacute", 6) == 0x8E);
    CHECK(entity_to_mac("#8217", 5) == 0xD5);
    CHECK(entity_to_mac("#x2014", 6) == 0xD1);
    CHECK(entity_to_mac("amp", 3) == '&');
    CHECK(entity_to_mac("bogus", 5) == 0);
    CHECK(latin1_to_mac(0xE9) == 0x8E);
    CHECK(latin1_to_mac('A') == 'A');
    CHECK(latin1_to_mac(0x93) == 0xD2 && latin1_to_mac(0x97) == 0xD1);  /* Windows-1252 quotes and dashes */
    CHECK(entity_to_mac("mdash", 5) == 0xD1 && entity_to_mac("hellip", 6) == 0xC9);
    CHECK(unicode_to_mac(0x142) == 'l' && unicode_to_mac(0x10D) == 'c');  /* l with stroke, c with caron */
    CHECK(unicode_to_mac(0xFB01) == 0xDE && unicode_to_mac(0x3C0) == 0xB9 && unicode_to_mac(0x200B) == 0);
    CHECK(unicode_to_mac(0x4E2D) == '?');
    {
        int c;
        long u;
        for (c = 0x80; c < 0x100; c++) CHECK(unicode_to_mac(mac_to_unicode((unsigned char)c)) == c);
        CHECK(utf8_char((const unsigned char *)"\xE2\x80\x94", 3, &u) == 3 && u == 0x2014);
        CHECK(utf8_char((const unsigned char *)"\xC0\xAF", 2, &u) == 0);       /* overlong */
        CHECK(utf8_char((const unsigned char *)"\xED\xA0\x80", 3, &u) == 0);  /* a surrogate */
        CHECK(utf8_char((const unsigned char *)"\xE2\x80", 2, &u) == 0);       /* cut short */
    }
    CHECK(charset_named("UTF-8", 5) == CHARSET_UTF8 && charset_named("iso-8859-1", 10) == CHARSET_LATIN1);
    CHECK(charset_named("shift_jis", 9) == CHARSET_GUESS);
    CHECK(charset_sniff("caf\xC3\xA9", 5) == CHARSET_UTF8);
    CHECK(charset_sniff("caf\xE9", 4) == CHARSET_LATIN1);
    CHECK(charset_sniff("plain", 5) == CHARSET_LATIN1);
    CHECK(charset_sniff("<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=iso-8859-1\">caf\xC3\xA9", 76) == CHARSET_LATIN1);
    CHECK(charset_sniff("<meta charset=\"utf-8\">x", 24) == CHARSET_UTF8);
}

/* A UTF-8 page: its text in Mac Roman, and its forms answer in UTF-8 */
static void test_utf8(void)
{
    Doc d;
    Forms F;
    Submission sub;
    const char *html = "<TITLE>Caf\xC3\xA9 \xE2\x80\x94 \xC5\x81\xC3\xB3" "d\xC5\xBA</TITLE>"
                       "<FORM ACTION=/f><INPUT NAME=q VALUE=\"\xC3\xA9\xE2\x80\x99\"></FORM>";
    CHECK(html_parse(&d, html, strlen(html)) == 0);
    CHECK(d.charset == CHARSET_UTF8);
    CHECK(!strcmp(d.title, "Caf\x8E \xD1 L\x97" "dz"));  /* e acute, an em dash, and L and z without their marks */
    CHECK(forms_build(&F, &d) == 0);
    CHECK(forms_submit(&F, &d, F.fields[0].form, -1, 0, 0, NULL, NULL, &sub) == 0);
    if (strcmp(sub.data, "q=%C3%A9%E2%80%99")) { printf("FAIL utf-8 form data: %s\n", sub.data); failures++; }
    submission_free(&sub);
    forms_free(&F);
    html_free(&d);
}

/* DISABLED controls can't be changed and aren't sent */
static void test_disabled(void)
{
    Doc d;
    Forms F;
    Submission sub;
    const char *html = "<FORM ACTION=/f><INPUT NAME=a VALUE=1><INPUT NAME=b VALUE=2 DISABLED>"
                       "<INPUT TYPE=checkbox NAME=c VALUE=yes CHECKED DISABLED><INPUT TYPE=checkbox NAME=e VALUE=yes DISABLED></FORM>";
    CHECK(html_parse(&d, html, strlen(html)) == 0);
    CHECK(forms_build(&F, &d) == 0 && F.n == 4);
    CHECK(!F.fields[0].disabled && F.fields[1].disabled && F.fields[2].disabled);
    forms_click(&F, 3);
    CHECK(!F.fields[3].checked);
    CHECK(forms_submit(&F, &d, F.fields[0].form, -1, 0, 0, NULL, NULL, &sub) == 0);
    if (strcmp(sub.data, "a=1")) { printf("FAIL disabled form data: %s\n", sub.data); failures++; }
    submission_free(&sub);
    forms_free(&F);
    html_free(&d);
}

static void test_parse(void)
{
    Doc d;
    const char *html = "<HTML><HEAD><TITLE> Caf&eacute;  News </TITLE><SCRIPT>if (a<b) x();</SCRIPT></HEAD>"
                       "<BODY><P>One<P>Two <A HREF=\"/a.html\" NAME=top>link</A><UL><LI>x<LI>y</UL></BODY></HTML>";
    long i, ps = 0, lis = 0, scripts_text = 0;
    CHECK(html_parse(&d, html, strlen(html)) == 0);
    CHECK(!strcmp(d.title, "Caf\x8E News"));
    for (i = 0; i < d.nnodes; i++) {
        if (d.nodes[i].tag == TAG_P) { ps++; CHECK(d.nodes[d.nodes[i].parent].tag != TAG_P); }
        if (d.nodes[i].tag == TAG_LI) lis++;
        if (d.nodes[i].tag == TAG_TEXT && memchr(d.text + d.nodes[i].text, '(', d.nodes[i].len)) scripts_text++;
        if (d.nodes[i].tag == TAG_A) CHECK(!strcmp(html_attr(&d, &d.nodes[i], "href"), "/a.html") && !strcmp(html_attr(&d, &d.nodes[i], "name"), "top"));
    }
    CHECK(ps == 2 && lis == 2 && scripts_text == 0);
    html_free(&d);
    /* a page made on a Mac keeps its Mac Roman letters; entities still work */
    CHECK(html_parse_charset(&d, "<TITLE>Caf\x8E &eacute;</TITLE>", 29, 1) == 0);
    CHECK(!strcmp(d.title, "Caf\x8E \x8E"));
    html_free(&d);
}

static void test_layout(void)
{
    Doc d;
    Layout L;
    const char *html = "<H1>Title</H1><P>one two three four five six seven eight nine ten eleven twelve"
                       "<P>Read <A HREF=\"next.html\">more</A>.<PRE>a  b\nc</PRE><HR>";
    long i, links = 0;
    CHECK(html_parse(&d, html, strlen(html)) == 0);
    CHECK(layout_doc(&L, &d, &M, 120) == 0);
    for (i = 0; i < L.nruns; i++) {
        const Run *r = &L.runs[i];
        if (r->kind != RUN_RULE) CHECK(r->x >= 8 && r->x + r->width <= 120 - 8 + 40);  /* words wrap inside margins */
        if (r->link) links++;
    }
    CHECK(L.nlines >= 6);
    CHECK(L.nlinks == 1 && !strcmp(L.links[0], "next.html") && links == 1);
    CHECK(L.height > L.lines[L.nlines - 1].y);
    {
        /* the PRE keeps its two lines and its double space */
        int found = 0;
        for (i = 0; i < L.nruns; i++)
            if (L.runs[i].kind == RUN_TEXT && L.runs[i].len == 4 && !memcmp(d.text + L.runs[i].text, "a  b", 4)) found = 1;
        CHECK(found);
    }
    layout_free(&L);
    html_free(&d);
}

/* The run whose text starts with word, or NULL */
static const Run *find(const Layout *L, const Doc *d, const char *word)
{
    long i, n = strlen(word);
    for (i = 0; i < L->nruns; i++)
        if (L->runs[i].kind == RUN_TEXT && L->runs[i].len >= n && !memcmp(d->text + L->runs[i].text, word, n)) return &L->runs[i];
    return NULL;
}

static long run_y(const Layout *L, const Run *r)
{
    long i, k = r - L->runs;
    for (i = 0; i < L->nlines; i++) if (k >= L->lines[i].first && k < L->lines[i].first + L->lines[i].count) return L->lines[i].y;
    return -1;
}

/* A list right after a heading: every item's text starts at the list's margin */
static int progress_seen[3];   /* calls, last percent, out of order */
static int note_progress(void *ctx, int percent)
{
    (void)ctx;
    if (percent <= progress_seen[1] && progress_seen[0]) progress_seen[2]++;
    progress_seen[0]++;
    progress_seen[1] = percent;
    return 0;
}
static int stop_at_half(void *ctx, int percent) { (void)ctx; return percent >= 50; }

/* Progress goes up to near 100 in order; a nonzero return stops the layout */
static void test_progress(void)
{
    Doc d;
    Layout L;
    Metrics m = M;
    char html[4000];
    int i;
    html[0] = 0;
    for (i = 0; i < 100; i++) strcat(html, "<P>Lorem ipsum dolor sit amet.");
    CHECK(html_parse(&d, html, strlen(html)) == 0);
    m.progress = note_progress;
    CHECK(layout_doc(&L, &d, &m, 200) == 0);
    CHECK(progress_seen[0] > 10 && progress_seen[1] >= 90 && !progress_seen[2]);
    layout_free(&L);
    m.progress = stop_at_half;
    CHECK(layout_doc(&L, &d, &m, 200) == -2);
    layout_free(&L);
    html_free(&d);
}

static void test_lists(void)
{
    Doc d;
    Layout L;
    const char *html = "<H3>Pages</H3><UL><LI><A HREF=\"a\">one</A>, x<LI><A HREF=\"b\">two</A>, y</UL>"
                       "<DL><DT>Key<DD>what</DL>";
    const Run *one, *two, *key, *what;
    CHECK(html_parse(&d, html, strlen(html)) == 0);
    CHECK(layout_doc(&L, &d, &M, 200) == 0);
    one = find(&L, &d, "one"); two = find(&L, &d, "two"); key = find(&L, &d, "Key"); what = find(&L, &d, "what");
    CHECK(one && two && one->x == two->x && one->x > 8);
    CHECK(key && what && what->x > key->x);
    layout_free(&L);
    html_free(&d);
}

/* A cell's set WIDTH holds when the table is short of room, so tables line up */
static void test_fixed_width(void)
{
    Doc d;
    Layout L;
    const char *html = "<TABLE WIDTH=100% BORDER=0 CELLSPACING=0 CELLPADDING=0><TR><TD WIDTH=110>Label</TD>"
                       "<TD>one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen "
                       "sixteen seventeen eighteen nineteen twenty</TD></TR></TABLE>";
    const Run *one;
    CHECK(html_parse(&d, html, strlen(html)) == 0);
    CHECK(layout_doc(&L, &d, &M, 300) == 0);
    one = find(&L, &d, "one");
    if (one && one->x != 8 + 110) printf("fixed width: second column at %d, not %d\n", one->x, 8 + 110);
    CHECK(one && one->x == 8 + 110);
    layout_free(&L);
    html_free(&d);
}

static void test_floats(void)
{
    Doc d;
    Layout L;
    long i;
    const Run *img = NULL;
    long img_y = 0, img_h = 0;
    int beside = 0, below = 0, bad = 0;
    /* 6px a character at size 12: a 300px page */
    const char *html = "<P><IMG SRC=a.gif WIDTH=60 HEIGHT=40 ALIGN=LEFT HSPACE=4>one two three four five six seven "
                       "eight nine ten eleven twelve thirteen fourteen fifteen sixteen seventeen eighteen nineteen "
                       "twenty twentyone twentytwo twentythree twentyfour twentyfive twentysix twentyseven";
    CHECK(html_parse(&d, html, strlen(html)) == 0);
    CHECK(layout_doc(&L, &d, &M, 300) == 0);
    for (i = 0; i < L.nlines; i++) {
        const MLine *ln = &L.lines[i];
        if (ln->count == 1 && L.runs[ln->first].kind == RUN_IMAGE) { img = &L.runs[ln->first]; img_y = ln->y; img_h = ln->height; }
    }
    CHECK(img && img->x == 8 + 4);
    for (i = 0; img && i < L.nlines; i++) {
        const MLine *ln = &L.lines[i];
        const Run *r = &L.runs[ln->first];
        if (r->kind != RUN_TEXT) continue;
        if (ln->y < img_y + img_h) { beside++; if (r->x < img->x + img->width + 4) bad++; }
        else { below++; if (r->x != 8) bad++; }
    }
    if (bad || !beside || !below) printf("floats: %d beside, %d below, %d misplaced\n", beside, below, bad);
    CHECK(beside >= 2 && below >= 1 && !bad);
    layout_free(&L);
    html_free(&d);
    /* on the right, and BR CLEAR goes below */
    html = "<IMG SRC=a.gif WIDTH=60 HEIGHT=40 ALIGN=RIGHT>beside<BR CLEAR=right>after";
    CHECK(html_parse(&d, html, strlen(html)) == 0);
    CHECK(layout_doc(&L, &d, &M, 300) == 0);
    {
        const Run *beside_r = find(&L, &d, "beside"), *after = find(&L, &d, "after");
        CHECK(beside_r && after && run_y(&L, after) >= 8 + 40 && run_y(&L, beside_r) < 8 + 40);
        for (i = 0; i < L.nruns; i++) if (L.runs[i].kind == RUN_IMAGE) CHECK(L.runs[i].x + L.runs[i].width == 300 - 8);
    }
    layout_free(&L);
    html_free(&d);
}

static void test_bgcolor(void)
{
    Doc d;
    Layout L;
    long i;
    int black = 0, white = 0;
    const Run *nav, *tan, *inner, *outer;
    const char *html =
        "<TABLE WIDTH=100% BORDER=0 CELLSPACING=0><TR><TD BGCOLOR=\"#003366\"><FONT COLOR=\"#FFFFFF\">"
        "<A HREF=/>Front</A> | News</FONT></TD></TR></TABLE>"
        "<TABLE><TR><TD BGCOLOR=\"#CCCC99\">Tan</TD></TR></TABLE>"
        "<TABLE BGCOLOR=navy><TR><TD>Outer<TABLE><TR><TD BGCOLOR=white>Inner</TD></TR></TABLE></TD></TR></TABLE>";
    CHECK(html_parse(&d, html, strlen(html)) == 0);
    CHECK(layout_doc(&L, &d, &M, 300) == 0);
    for (i = 0; i < L.nboxes; i++) { if (L.boxes[i].thick == 0) black++; if (L.boxes[i].thick == -1) white++; }
    nav = find(&L, &d, "Front"); tan = find(&L, &d, "Tan"); inner = find(&L, &d, "Inner"); outer = find(&L, &d, "Outer");
    CHECK(black == 2 && white == 1);                      /* the nav cell and the navy table's cell; the white cell in it */
    CHECK(nav && (nav->face & FACE_INVERSE) && (nav->face & FACE_UNDERLINE));
    CHECK(tan && !(tan->face & FACE_INVERSE));            /* light: black text, no fill */
    CHECK(outer && (outer->face & FACE_INVERSE) && inner && !(inner->face & FACE_INVERSE));
    layout_free(&L);
    html_free(&d);
}

static void test_tables(void)
{
    Doc d;
    Layout L;
    const Run *a, *b, *c, *e;
    /* two columns side by side; the fixed 570 shrinks to the 300 we have */
    const char *t1 = "<TABLE WIDTH=570 BORDER=1><TR><TD>alpha beta</TD><TD>gamma</TD></TR>"
                     "<TR><TD COLSPAN=2>delta spans both</TD></TR></TABLE><P>after";
    const char *t2 = "<TABLE><TR><TD ROWSPAN=2>tall one two three four five six seven</TD><TD>right1</TD></TR>"
                     "<TR><TD>right2</TD></TR></TABLE>";
    const char *t3 = "<TABLE><TR><TD>outer<TABLE><TR><TD>in1</TD><TD>in2</TD></TR></TABLE></TD><TD>side</TD></TR></TABLE>";
    long i;

    CHECK(html_parse(&d, t1, strlen(t1)) == 0);
    CHECK(layout_doc(&L, &d, &M, 300) == 0);
    a = find(&L, &d, "alpha"); b = find(&L, &d, "gamma"); c = find(&L, &d, "delta"); e = find(&L, &d, "after");
    CHECK(a && b && c && e);
    if (a && b && c && e) {
        CHECK(run_y(&L, a) == run_y(&L, b) && b->x > a->x + a->width);
        CHECK(run_y(&L, c) > run_y(&L, a));
        CHECK(run_y(&L, e) > run_y(&L, c));
    }
    for (i = 0; i < L.nruns; i++) CHECK(L.runs[i].x + L.runs[i].width <= 300);
    CHECK(L.nboxes == 4);  /* three cells and the table */
    for (i = 0; i < L.nboxes; i++) CHECK(L.boxes[i].x + L.boxes[i].w <= 300 - 8);
    for (i = 1; i < L.nlines; i++) CHECK(L.lines[i].y >= L.lines[i - 1].y);
    layout_free(&L);
    html_free(&d);

    CHECK(html_parse(&d, t2, strlen(t2)) == 0);
    CHECK(layout_doc(&L, &d, &M, 200) == 0);
    a = find(&L, &d, "tall"); b = find(&L, &d, "right1"); c = find(&L, &d, "right2");
    CHECK(a && b && c);
    if (a && b && c) {
        CHECK(b->x > a->x && c->x == b->x && run_y(&L, c) > run_y(&L, b));
    }
    layout_free(&L);
    html_free(&d);

    CHECK(html_parse(&d, t3, strlen(t3)) == 0);
    CHECK(layout_doc(&L, &d, &M, 400) == 0);
    a = find(&L, &d, "in1"); b = find(&L, &d, "in2"); c = find(&L, &d, "side");
    CHECK(a && b && c);
    if (a && b && c) CHECK(run_y(&L, a) == run_y(&L, b) && b->x > a->x && c->x > b->x);
    layout_free(&L);
    html_free(&d);
}

/* Write a 1-bit bitmap as a PBM, for looking at */
static void save_pbm(const Bitmap1 *b, const char *path)
{
    FILE *f = fopen(path, "wb");
    short y;
    if (!f) return;
    fprintf(f, "P4\n%d %d\n", b->w, b->h);
    for (y = 0; y < b->h; y++) fwrite(b->bits + (long)y * b->row_bytes, 1, (b->w + 7) / 8, f);
    fclose(f);
}

/* Decoding progress: counts reports, and stops once past stop_at percent */
static int reports, last_pct, stop_at;
static int count_progress(void *ref, int pct)
{
    (void)ref;
    CHECK(pct >= last_pct && pct <= 100);
    last_pct = pct;
    reports++;
    return pct > stop_at;
}

static void test_image(const char *name, short want_w, short want_h, short w, short h)
{
    char path[200];
    long len, i, black = 0;
    unsigned char *src;
    short nw, nh;
    Bitmap1 b;
    snprintf(path, sizeof path, "tests/fixtures/%s", name);
    if (!(src = (unsigned char *)slurp(path, &len))) { printf("skip %s\n", name); return; }
    CHECK(image_dims(src, len, &nw, &nh) == 0 && nw == want_w && nh == want_h);
    reports = last_pct = 0; stop_at = 100;
    CHECK(image_decode(src, len, w, h, &b, count_progress, NULL) == 0);
    CHECK(reports > 0);
    if (b.bits) {
        CHECK(b.w == w && b.h == h && b.row_bytes % 2 == 0);
        for (i = 0; i < (long)b.row_bytes * h; i++) { unsigned char v = b.bits[i]; while (v) { black += v & 1; v >>= 1; } }
        printf("%-14s %dx%d -> %dx%d, %ld%% black\n", name, nw, nh, w, h, black * 100 / ((long)w * h));
        CHECK(black > 0 && black < (long)w * h);
        snprintf(path, sizeof path, "/tmp/out-%s-%dx%d.pbm", name, w, h);
        save_pbm(&b, path);
        image_free(&b);
    }
    CHECK(image_decode((const unsigned char *)"GIF89a", 6, 10, 10, &b, NULL, NULL) == -1);
    /* stopping partway gives nothing */
    if (len > 4096) {
        reports = last_pct = 0; stop_at = 10;
        CHECK(image_decode(src, len, w, h, &b, count_progress, NULL) == -1 && !b.bits);
    }
    free(src);
}

static long fake_read(void *ctx, const char *path, char **data)
{
    (void)ctx;
    if (strcmp(path, "Macintosh HD:Notes")) return -1;
    *data = malloc(5);
    memcpy(*data, "hello", 5);
    return 5;
}

static void test_forms(void)
{
    Doc d;
    Forms F;
    Submission sub;
    Layout L;
    long i, q, r1, r2, text, area, sel, go;
    const char *html =
        "<FORM ACTION=\"/search\">"
        "<INPUT NAME=q VALUE=\"caf&eacute; au lait\"><INPUT TYPE=hidden NAME=lang VALUE=en>"
        "<INPUT TYPE=radio NAME=r VALUE=a CHECKED><INPUT TYPE=radio NAME=r VALUE=b>"
        "<INPUT TYPE=radio NAME=other VALUE=z CHECKED>"
        "<INPUT TYPE=checkbox NAME=c><SELECT NAME=s><OPTION>One<OPTION VALUE=2 SELECTED>Two</SELECT>"
        "<TEXTAREA NAME=t>line <B>one</B>\nline two</TEXTAREA><INPUT TYPE=submit NAME=go VALUE=Go></FORM>"
        "<FORM METHOD=post ENCTYPE=\"multipart/form-data\"><INPUT TYPE=file NAME=f><INPUT NAME=n VALUE=x></FORM>";
    CHECK(html_parse(&d, html, strlen(html)) == 0);
    CHECK(forms_build(&F, &d) == 0);
    CHECK(F.n == 11);
    CHECK(!forms_changed(&F, &d));
    q = r1 = r2 = text = area = sel = go = -1;
    for (i = 0; i < F.n; i++) {
        Field *f = &F.fields[i];
        if (f->type == FIELD_TEXT && f->name && !strcmp(f->name, "q")) text = i;
        if (f->type == FIELD_RADIO && f->name && !strcmp(f->name, "r")) { if (r1 < 0) r1 = i; else r2 = i; }
        if (f->type == FIELD_TEXTAREA) area = i;
        if (f->type == FIELD_SELECT) sel = i;
        if (f->type == FIELD_SUBMIT) go = i;
    }
    CHECK(text >= 0 && r1 >= 0 && r2 >= 0 && area >= 0 && sel >= 0 && go >= 0);
    if (area >= 0) CHECK(!strcmp(F.fields[area].value, "line <B>one</B>\nline two"));
    if (sel >= 0) CHECK(F.fields[sel].noptions == 2 && !F.fields[sel].selected[0] && F.fields[sel].selected[1]);
    CHECK(forms_find(&F, F.fields[3].node) == 3);

    /* radio buttons switch within their name only */
    forms_click(&F, r2);
    CHECK(!F.fields[r1].checked && F.fields[r2].checked && F.fields[4].checked);
    CHECK(forms_changed(&F, &d));
    forms_set_value(&F.fields[text], "M\x8Ell\xD5s & co\rnext");  /* Mac Roman, a curly quote, a CR */
    CHECK(forms_submit(&F, &d, F.fields[text].form, go, 0, 0, NULL, NULL, &sub) == 0);
    CHECK(!sub.post && !strcmp(sub.action, "/search"));
    if (strcmp(sub.data, "q=M%E9ll%27s+%26+co%0D%0Anext&lang=en&r=b&other=z&s=2&t=line+%3CB%3Eone%3C%2FB%3E%0D%0Aline+two&go=Go"))
        { printf("FAIL form data: %s\n", sub.data); failures++; }
    submission_free(&sub);
    forms_reset(&F, &d, F.fields[text].form);
    CHECK(F.fields[r1].checked && !F.fields[r2].checked && !strcmp(F.fields[text].value, "caf\x8E au lait"));
    CHECK(!forms_changed(&F, &d));
    /* what counts as a change: typing, a choice, a checkbox; not a value typed back */
    forms_set_value(&F.fields[area], "line <B>one</B>\nline two");
    CHECK(!forms_changed(&F, &d));
    forms_set_value(&F.fields[area], "other");
    CHECK(forms_changed(&F, &d));
    forms_reset(&F, &d, F.fields[area].form);
    F.fields[sel].selected[0] = 1;
    F.fields[sel].selected[1] = 0;
    CHECK(forms_changed(&F, &d));
    forms_reset(&F, &d, F.fields[sel].form);
    forms_click(&F, 5);   /* the checkbox */
    CHECK(forms_changed(&F, &d));
    forms_reset(&F, &d, F.fields[5].form);
    CHECK(!forms_changed(&F, &d));

    /* a file upload */
    forms_set_value(&F.fields[9], "Macintosh HD:Notes");
    CHECK(forms_submit(&F, &d, F.fields[9].form, -1, 0, 0, fake_read, NULL, &sub) == 0);
    CHECK(sub.post && sub.multipart && !strcmp(sub.action, ""));
    CHECK(strstr(sub.data, "name=\"f\"; filename=\"Notes\"\r\nContent-Type: application/octet-stream\r\n\r\nhello\r\n") != NULL);
    CHECK(strstr(sub.data, "name=\"n\"\r\n\r\nx\r\n") != NULL);
    CHECK(sub.len > 0 && !memcmp(sub.data + sub.len - 2, "\r\n", 2) && strstr(sub.data, "MillFormBoundary7d3a9e0c--"));
    submission_free(&sub);

    /* controls take room in the layout; hidden ones don't */
    CHECK(layout_doc(&L, &d, &M, 400) == 0);
    for (i = q = 0; i < L.nruns; i++) if (L.runs[i].kind == RUN_CONTROL) q++;
    CHECK(q == 10);
    layout_free(&L);
    forms_free(&F);
    html_free(&d);
}

static void check_resolve(const char *base, const char *href, const char *want)
{
    char out[300];
    int r = url_resolve(base, href, out, sizeof out);
    if (!want) { CHECK(r == -1); return; }
    if (r || strcmp(out, want)) { printf("FAIL resolve %s + %s = %s (want %s)\n", base, href, r ? "ERR" : out, want); failures++; }
}

static void check_cookies_by(Jar *j, const char *host, const char *path, int https, unsigned long now, const char *want)
{
    char got[256];
    jar_header(j, host, path, https, now, got, sizeof got);
    if (strcmp(got, want)) printf("cookies for %s%s: \"%s\", not \"%s\"\n", host, path, got, want);
    CHECK(!strcmp(got, want));
}

static void check_cookies(Jar *j, const char *host, const char *path, unsigned long now, const char *want)
{
    check_cookies_by(j, host, path, 0, now, want);
}

static void set_cookie_by(Jar *j, const char *host, const char *path, int https, const char *line, unsigned long now)
{
    jar_set(j, host, path, https, line, strlen(line), now);
}

static void set_cookie(Jar *j, const char *host, const char *path, const char *line, unsigned long now)
{
    set_cookie_by(j, host, path, 0, line, now);
}

static void test_cookies(void)
{
    Jar j;
    unsigned long y2k = 3029529600UL;   /* 2000-01-01 00:00:00 in the Mac's seconds since 1904 */
    char big[300];
    int i;
    memset(&j, 0, sizeof j);
    CHECK(cookie_date("Sat, 01 Jan 2000 00:00:00 GMT", 29) == y2k);
    CHECK(cookie_date("Saturday, 01-Jan-00 00:00:10 GMT", 32) == y2k + 10);
    CHECK(cookie_date("Sun, 06 Nov 1994 08:49:37 GMT", 29) == 2866956577UL);
    CHECK(cookie_date("soon", 4) == 0);

    set_cookie(&j, "Shop.example.com", "/cart/view?x=1", "id=42; Path=/", y2k);
    set_cookie(&j, "shop.example.com", "/cart/view", "pref=big", y2k);                 /* path /cart */
    set_cookie(&j, "shop.example.com", "/", "site=a; Domain=.example.com", y2k);
    set_cookie(&j, "shop.example.com", "/", "tld=x; Domain=com", y2k);                 /* too wide: dropped */
    set_cookie(&j, "shop.example.com", "/", "other=x; Domain=example.org", y2k);       /* not ours: dropped */
    set_cookie(&j, "shop.example.com", "/", "s=1; Secure", y2k);                       /* https only: dropped */
    set_cookie(&j, "shop.example.com", "/", "old=1; Expires=Fri, 31 Dec 1999 23:59:59 GMT", y2k);
    set_cookie(&j, "shop.example.com", "/", "brief=1; Max-Age=60", y2k);
    CHECK(j.n == 4);
    check_cookies(&j, "shop.example.com", "/cart/view", y2k, "id=42; pref=big; site=a; brief=1");
    check_cookies(&j, "shop.example.com", "/cartoon", y2k, "id=42; site=a; brief=1");
    check_cookies(&j, "www.example.com", "/cart", y2k, "site=a");
    check_cookies(&j, "example.org", "/", y2k, "");
    check_cookies(&j, "shop.example.com", "/", y2k + 61, "id=42; site=a");              /* brief expired */
    set_cookie(&j, "shop.example.com", "/", "id=43", y2k);                              /* replaces id=42 */
    set_cookie(&j, "shop.example.com", "/", "site=; Domain=example.com; Max-Age=0", y2k); /* deletes site */
    check_cookies(&j, "shop.example.com", "/", y2k, "id=43");
    /* Secure: kept from https, and only sent back over it */
    set_cookie_by(&j, "shop.example.com", "/", 1, "s=2; Secure", y2k);
    check_cookies(&j, "shop.example.com", "/", y2k, "id=43");
    check_cookies_by(&j, "shop.example.com", "/", 1, y2k, "id=43; s=2");
    /* full: the oldest go first */
    memset(big, 'v', sizeof big - 1);
    big[sizeof big - 1] = 0;
    for (i = 0; i < JAR_MAX_COOKIES + 5; i++) {
        char line[340];
        sprintf(line, "c%d=%s", i, big);
        set_cookie(&j, "a.example.com", "/", line, y2k);
    }
    CHECK(j.n <= JAR_MAX_COOKIES && j.bytes <= JAR_MAX_BYTES);
    {
        char got[64];
        jar_header(&j, "a.example.com", "/", 0, y2k, got, sizeof got);  /* too long to fit: none */
        CHECK(got[0] == 0);
    }
    jar_free(&j);
    CHECK(j.n == 0);
}

/* The HREF of the area at (x, y) of the first IMG, or "" for none */
static const char *map_at(const Doc *d, short x, short y)
{
    long i, map, area;
    for (i = 0; i < d->nnodes && d->nodes[i].tag != TAG_IMG; i++) {}
    if (i == d->nnodes || (map = imagemap_for(d, i)) < 0) return "(no map)";
    area = imagemap_hit(d, map, x, y);
    return area < 0 ? "" : html_attr(d, &d->nodes[area], "href");
}

static void test_imagemaps(void)
{
    Doc d;
    const char *html =
        "<IMG SRC=nav.gif USEMAP=\"#Nav\" WIDTH=200 HEIGHT=100>"
        "<P>text<MAP NAME=nav>"
        "<AREA SHAPE=rect COORDS=\"0,0,49,49\" HREF=home.html>"
        "<AREA SHAPE=circle COORDS=\"100,50,20\" HREF=circle.html>"
        "<AREA SHAPE=poly COORDS=\"150,10, 190,10, 170,40\" HREF=tri.html>"
        "<AREA SHAPE=rect COORDS=\"0,60,40,99\" NOHREF>"
        "<AREA SHAPE=default HREF=rest.html></MAP>";
    CHECK(html_parse(&d, html, strlen(html)) == 0);
    CHECK(!strcmp(map_at(&d, 10, 10), "home.html"));
    CHECK(!strcmp(map_at(&d, 49, 49), "home.html"));
    CHECK(!strcmp(map_at(&d, 105, 55), "circle.html"));
    CHECK(!strcmp(map_at(&d, 170, 20), "tri.html"));
    CHECK(!strcmp(map_at(&d, 151, 38), "rest.html"));     /* outside the triangle */
    CHECK(!strcmp(map_at(&d, 20, 80), ""));               /* NOHREF takes the point */
    CHECK(!strcmp(map_at(&d, 195, 95), "rest.html"));
    html_free(&d);
    CHECK(html_parse(&d, "<IMG SRC=x.gif USEMAP=other.html#none>", 38) == 0);
    CHECK(!strcmp(map_at(&d, 1, 1), "(no map)"));
    html_free(&d);
}

static void test_isindex(void)
{
    Doc d;
    Forms F;
    Submission sub;
    long i;
    int prompt = 0, hrs = 0;
    const char *html = "<TITLE>Search</TITLE><ISINDEX PROMPT=\"Find:\">";
    CHECK(html_parse(&d, html, strlen(html)) == 0);
    for (i = 0; i < d.nnodes; i++) {
        if (d.nodes[i].tag == TAG_HR) hrs++;
        if (d.nodes[i].tag == TAG_TEXT && d.nodes[i].len == 5 && !memcmp(d.text + d.nodes[i].text, "Find:", 5)) prompt = 1;
    }
    CHECK(hrs == 2 && prompt);
    CHECK(forms_build(&F, &d) == 0 && F.n == 1);
    forms_set_value(&F.fields[0], "old macs");
    CHECK(forms_submit(&F, &d, F.fields[0].form, -1, 0, 0, NULL, NULL, &sub) == 0);
    if (strcmp(sub.data, "old+macs") || sub.post || sub.action[0]) { printf("FAIL isindex: %s\n", sub.data); failures++; }
    submission_free(&sub);
    forms_free(&F);
    html_free(&d);
}

static void check_type(const char *rules, const char *mime, const char *name, const char *want)
{
    char t[4], c[4], got[10];
    filetype_find(rules, mime, name, t, c);
    sprintf(got, "%.4s/%.4s", t, c);
    if (strcmp(got, want)) printf("type %s %s: %s, not %s\n", mime, name, got, want);
    CHECK(!strcmp(got, want));
}

static void test_filetypes(void)
{
    unsigned char h[128];
    MacBinary mb;
    char name[64];
    int i;
    unsigned short crc = 0;
    check_type(FILETYPES_DEFAULT, "application/octet-stream", "Tool.sit", "SIT!/SITx");
    check_type(FILETYPES_DEFAULT, "text/plain", "Tool.HQX", "TEXT/SITx");          /* names before types */
    check_type(FILETYPES_DEFAULT, "application/mac-binhex40; x=y", "get", "TEXT/SITx");
    check_type(FILETYPES_DEFAULT, "text/html", "index.html", "TEXT/ttxt");
    check_type(FILETYPES_DEFAULT, "", "manual.pdf", "PDF /CARO");
    check_type(FILETYPES_DEFAULT, "application/zip", "a.zip", "BINA/????");
    check_type("text/* * TEXT/ttxt;image/gif * GIFf/JVWR", "image/gif", "x", "GIFf/JVWR");
    check_type("", "text/plain", "x", "BINA/????");
    CHECK(filetype_rule_ok("* *.sit SIT!/SITx", 17) == 0);
    CHECK(filetype_rule_ok("* *.pdf PDF /CARO  ", 19) == 0);
    CHECK(filetype_rule_ok("* *.sit SIT!", 12) == -1);
    CHECK(filetype_rule_ok("*.sit SIT!/SITx", 15) == -1);

    filetype_name("http://h/files/Tool%201.0.sit.bin?x=1", name, sizeof name);
    CHECK(!strcmp(name, "Tool 1.0.sit"));
    filetype_name("http://h/", name, sizeof name);
    CHECK(!strcmp(name, "h"));
    filetype_name("mill://about", name, sizeof name);
    CHECK(!strcmp(name, "about"));
    filetype_name("http://h/a:b", name, sizeof name);
    CHECK(!strcmp(name, "a-b"));
    filetype_name("http://h/a-very-long-name-for-a-mac-file-indeed.txt", name, sizeof name);
    CHECK(strlen(name) == 31);

    /* a MacBinary II header: known by its CRC */
    memset(h, 0, sizeof h);
    h[1] = 4; memcpy(h + 2, "Tool", 4);
    memcpy(h + 65, "APPLTOOL", 8);
    h[73] = 0x21;                       /* has bundle, and on the desktop */
    h[86] = 10;                         /* 10 bytes of data */
    h[89] = 1; h[90] = 0;               /* 256 of resources */
    h[122] = 129; h[123] = 129;
    for (i = 0; i < 124; i++) {
        int b;
        crc ^= (unsigned short)(h[i] << 8);
        for (b = 0; b < 8; b++) crc = (unsigned short)(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    h[124] = (unsigned char)(crc >> 8); h[125] = (unsigned char)crc;
    CHECK(macbinary_header(h, &mb) == 0);
    CHECK(!strcmp(mb.name, "Tool") && !memcmp(mb.type, "APPL", 4) && !memcmp(mb.creator, "TOOL", 4));
    CHECK(mb.flags == 0x2000 && mb.data_at == 128 && mb.data_len == 10 && mb.rsrc_at == 256 && mb.rsrc_len == 256);
    h[125] ^= 1;                        /* a bad CRC, and II's bytes aren't zero: not MacBinary */
    CHECK(macbinary_header(h, &mb) == -1);
    memset(h + 99, 0, 27);              /* MacBinary I */
    CHECK(macbinary_header(h, &mb) == 0 && mb.rsrc_at == 256);
    h[0] = 'G';                         /* a GIF, say */
    CHECK(macbinary_header(h, &mb) == -1);
}

static void test_urls(void)
{
    const char *b = "http://www.example.com/news/story.html";
    check_resolve(b, "other.html", "http://www.example.com/news/other.html");
    check_resolve(b, "/img/a.gif", "http://www.example.com/img/a.gif");
    check_resolve(b, "../about.html", "http://www.example.com/about.html");
    check_resolve("http://h/a/b/c.html", "../d.html", "http://h/a/d.html");
    check_resolve("http://h/a/b/c.html", "../../d.html", "http://h/d.html");
    check_resolve("http://h/a/b/c.html", "..", "http://h/a/");
    check_resolve("http://h/a/b/c.html", "./d/./e.html", "http://h/a/b/d/e.html");
    check_resolve(b, "#top", "http://www.example.com/news/story.html#top");
    check_resolve(b, "http://Other.example.org:8080/x?y=1", "http://Other.example.org:8080/x?y=1");
    check_resolve(b, "mailto:someone@example.org", NULL);
    check_resolve("http://www.example.com/", "weather/", "http://www.example.com/weather/");
    check_resolve("http://www.example.com", "a", "http://www.example.com/a");
    check_resolve("file:///Macintosh%20HD/Web/index.html", "pics/a.gif", "file:///Macintosh%20HD/Web/pics/a.gif");
    check_resolve("file:///Macintosh%20HD/Web/index.html", "../Read%20Me", "file:///Macintosh%20HD/Read%20Me");
    check_resolve("mill://about", "mill://Config", "mill://config");
    check_resolve("mill://config", "?saved=1", "mill://config/?saved=1");
    check_resolve("http://www.example.com/", "file:///Macintosh%20HD/", "file:///Macintosh%20HD/");
    {
        Url u;
        CHECK(url_parse("mill://about", &u) == 0 && u.scheme == SCHEME_MILL && !strcmp(u.host, "about") && !strcmp(u.path, "/"));
        CHECK(url_parse("file:///Macintosh%20HD/x.html", &u) == 0 && u.scheme == SCHEME_FILE && !u.host[0]);
        CHECK(url_parse("www.example.com", &u) == 0 && u.scheme == SCHEME_HTTP);
        CHECK(url_parse("gopher://x", &u) == -1);
        CHECK(url_parse("HTTPS://Secure.example/a", &u) == 0 && u.scheme == SCHEME_HTTPS && u.port == 443 &&
              !strcmp(u.host, "Secure.example") && !strcmp(u.path, "/a"));
        CHECK(url_parse("https://h:8443/", &u) == 0 && u.port == 8443);
        CHECK(url_parse("proxy.example:5001", &u) == 0 && u.scheme == SCHEME_HTTP && u.port == 5001 && !strcmp(u.host, "proxy.example"));
        CHECK(url_is_web(SCHEME_HTTP) && url_is_web(SCHEME_HTTPS) && !url_is_web(SCHEME_FILE) && !url_is_web(SCHEME_MILL));
    }
    /* https: resolves like http:, and keeps its own default port */
    check_resolve("https://www.example.com/news/", "a.html", "https://www.example.com/news/a.html");
    check_resolve("https://www.example.com/", "//cdn.example.com/x.gif", "https://cdn.example.com/x.gif");
    check_resolve("http://www.example.com/", "//cdn.example.com/x.gif", "http://cdn.example.com/x.gif");
    check_resolve("http://www.example.com/", "https://www.example.com:443/", "https://www.example.com/");
    check_resolve("https://www.example.com:8443/a", "b", "https://www.example.com:8443/b");
    check_resolve("https://www.example.com/", "http://www.example.com:80/", "http://www.example.com/");
    /* "://" after the scheme, in a path, query or fragment, is only text */
    {
        const char *q = "http://www.example.com/get?url=http://example.org/a&b=1";
        Url u;
        char out[300];
        CHECK(url_parse(q, &u) == 0 && u.scheme == SCHEME_HTTP && !strcmp(u.host, "www.example.com") &&
              !strcmp(u.path, "/get?url=http://example.org/a&b=1"));
        CHECK(url_parse("https://www.example.com/r?to=https://example.org/", &u) == 0 &&
              u.scheme == SCHEME_HTTPS && !strcmp(u.host, "www.example.com"));
        CHECK(url_parse("http://www.example.com/a#see:http://example.org/", &u) == 0 &&
              !strcmp(u.path, "/a") && !strcmp(u.fragment, "see:http://example.org/"));
        CHECK(url_parse("www.example.com/get?url=http://example.org/", &u) == 0 && u.scheme == SCHEME_HTTP &&
              !strcmp(u.host, "www.example.com"));
        CHECK(url_parse("gopher://example.org/?u=http://example.com/", &u) == -1);
        check_resolve(q, "/get?u=http://example.org/b", "http://www.example.com/get?u=http://example.org/b");
        check_resolve(q, "page?u=http://example.org/b", "http://www.example.com/page?u=http://example.org/b");
        check_resolve(q, "?u=http://example.org/b", "http://www.example.com/get?u=http://example.org/b");
        check_resolve(q, "#http://example.org/", "http://www.example.com/get?url=http://example.org/a&b=1#http://example.org/");
        check_resolve(b, "a.html#x://y", "http://www.example.com/news/a.html#x://y");
        check_resolve(b, "//cdn.example.com/x?u=http://example.org/", "http://cdn.example.com/x?u=http://example.org/");
        check_resolve(b, q, q);
        /* typed addresses: http:// goes in front unless one is there already */
        CHECK(url_has_scheme("http://www.example.com/") == 7);
        CHECK(url_has_scheme("HTTPS://www.example.com/") == 8);
        CHECK(url_has_scheme("gopher://example.org/") == 9);
        CHECK(!url_has_scheme("www.example.com/get?url=http://example.org/"));
        CHECK(!url_has_scheme("www.example.com#http://example.org/"));
        CHECK(!url_has_scheme("proxy.example:5001"));
        CHECK(!url_has_scheme("/a?u=http://example.org/"));
        url_strip_login(strcpy(out, "http://joe@www.example.com/get?url=http://example.org/"));
        CHECK(!strcmp(out, "http://www.example.com/get?url=http://example.org/"));
        url_strip_login(strcpy(out, "www.example.com/get?url=http://joe@example.org/"));
        CHECK(!strcmp(out, "www.example.com/get?url=http://joe@example.org/"));
    }
    /* domains that skip the proxy */
    CHECK(url_host_in("example.com", "example.com"));
    CHECK(url_host_in("www.Example.COM", "other.org, example.com"));
    CHECK(url_host_in("news.example.lan", ".lan"));
    CHECK(url_host_in("lan", ".lan"));
    CHECK(!url_host_in("badexample.com", "example.com"));
    CHECK(!url_host_in("example.com", "www.example.com"));
    CHECK(!url_host_in("example.com", ""));
    CHECK(!url_host_in("example.com", " , ."));
    /* addresses: only themselves, or a range that holds them; never a domain's suffix */
    CHECK(url_host_in("10.1.2.5", "10.1.2.5"));
    CHECK(!url_host_in("10.1.2.5", "2.5"));
    CHECK(!url_host_in("10.0.0.1", "0.1 0.0.1"));
    CHECK(!url_host_in("10.1.2.50", "10.1.2.5"));
    CHECK(url_host_in("172.16.5.5", "example.com 172.16.0.0/12"));
    CHECK(url_host_in("172.31.255.255", "172.16.0.0/12"));
    CHECK(!url_host_in("172.32.0.1", "172.16.0.0/12"));
    CHECK(url_host_in("192.168.1.20", "192.168.1.0/24"));
    CHECK(!url_host_in("192.168.2.20", "192.168.1.0/24"));
    CHECK(url_host_in("8.8.8.8", "0.0.0.0/0"));
    CHECK(url_host_in("10.1.2.3", "10.1.2.3/32"));
    CHECK(!url_host_in("10.1.2.3", "10.1.2.0/33 10.1.2.0/ 10.1.2.0/x 10.1.2.0/024"));
    CHECK(!url_host_in("1.2.3.4", ".lan, 1.2.3"));
    CHECK(!url_host_in("www.example.com", "1.2.3.4 0.0.0.0/0"));
    CHECK(url_host_in("1.2.3.4.example.com", "example.com"));
    CHECK(url_host_in("256.1.1.1", "256.1.1.1"));  /* not an address: a name, and it matches as one */
}

/* The layout as a picture: text as bars, rules, borders, and image boxes */
static void save_layout(const Layout *L, const char *name, long rows)
{
    long w = L->width, i, k, y, x;
    unsigned char *px = calloc(w * rows, 1);
    char path[200];
    FILE *f;
#define DOT(X, Y) do { long _x = (X), _y = (Y); if (_x >= 0 && _x < w && _y >= 0 && _y < rows) px[_y * w + _x] = 1; } while (0)
    for (i = 0; i < L->nboxes; i++) {
        const Box *b = &L->boxes[i];
        for (x = b->x; x < b->x + b->w; x++) { DOT(x, b->y); DOT(x, b->y + b->h - 1); }
        for (y = b->y; y < b->y + b->h; y++) { DOT(b->x, y); DOT(b->x + b->w - 1, y); }
    }
    for (i = 0; i < L->nlines; i++) {
        const MLine *ln = &L->lines[i];
        for (k = ln->first; k < ln->first + ln->count; k++) {
            const Run *r = &L->runs[k];
            long base = ln->y + ln->ascent;
            if (r->kind == RUN_RULE) { for (x = r->x; x < r->x + r->width; x++) DOT(x, ln->y + ln->height / 2); continue; }
            if (r->kind == RUN_IMAGE) {
                long t = base - r->font;
                for (x = r->x; x < r->x + r->width; x++) { DOT(x, t); DOT(x, t + r->size - 1); }
                for (y = t; y < t + r->size; y++) { DOT(r->x, y); DOT(r->x + r->width - 1, y); DOT(r->x + (y - t) * r->width / r->size, y); }
                continue;
            }
            for (y = base - r->size * 3 / 4 + 1; y <= base; y++)
                for (x = r->x; x < r->x + r->width; x++) if (((x + y) & 1) || y == base) DOT(x, y);
        }
    }
    snprintf(path, sizeof path, "/tmp/out-layout-%s.pbm", name);
    if ((f = fopen(path, "wb"))) {
        fprintf(f, "P4\n%ld %ld\n", w, rows);
        for (y = 0; y < rows; y++) for (x = 0; x < w; x += 8) {
            unsigned char b = 0; int j;
            for (j = 0; j < 8; j++) if (x + j < w && px[y * w + x + j]) b |= 0x80 >> j;
            fputc(b, f);
        }
        fclose(f);
    }
    free(px);
}

static void test_fixture(const char *name)
{
    char path[200];
    long len;
    char *src;
    Doc d;
    Layout L;
    snprintf(path, sizeof path, "tests/fixtures/%s", name);
    if (!(src = slurp(path, &len))) { printf("skip %s\n", name); return; }
    CHECK(html_parse(&d, src, len) == 0);
    CHECK(layout_doc(&L, &d, &M, 496) == 0);
    printf("%-14s %6ld bytes: %5ld nodes, %5ld lines, %5ld runs, %4ld links, %3ld boxes, height %ld, \"%s\"\n",
           name, len, d.nnodes, L.nlines, L.nruns, L.nlinks, L.nboxes, L.height, d.title);
    {
        long i, widest = 0;
        for (i = 0; i < L.nruns; i++) if (L.runs[i].x + L.runs[i].width > widest) widest = L.runs[i].x + L.runs[i].width;
        printf("%14s widest run ends at %ld of 496\n", "", widest);
    }
    CHECK(L.nlines > 5 && L.nlinks > 3);
    save_layout(&L, name, 900);
    layout_free(&L);
    html_free(&d);
    free(src);
}

static void md5_is(const char *s, const char *want)
{
    unsigned char d[16];
    char got[33];
    int i;
    auth_md5((const unsigned char *)s, strlen(s), d);
    for (i = 0; i < 16; i++) sprintf(got + i * 2, "%02x", d[i]);
    if (strcmp(got, want)) { printf("FAIL md5(%s) = %s\n", s, got); failures++; }
}

static Challenge challenge(const char *v)
{
    Challenge c;
    memset(&c, 0, sizeof c);
    auth_challenge(v, strlen(v), &c);
    return c;
}

static void test_auth(void)
{
    Keyring k;
    Challenge c;
    Cred *cr;
    char out[600], b[64];
    md5_is("", "d41d8cd98f00b204e9800998ecf8427e");
    md5_is("The quick brown fox jumps over the lazy dog", "9e107d9d372bb6826bd81d3542a419d6");
    md5_is("12345678901234567890123456789012345678901234567890123456789012345678901234567890",
           "57edf4a22be3c955ac49da2e2107b67a");
    md5_is("1234567890123456789012345678901234567890123456789012345", "c9ccf168914a1bcfc3229f1948e67da0");
    CHECK(auth_base64((const unsigned char *)"Aladdin:open sesame", 19, b, sizeof b) == 28 &&
          !strcmp(b, "QWxhZGRpbjpvcGVuIHNlc2FtZQ=="));
    CHECK(auth_base64((const unsigned char *)"ab", 2, b, 4) == 0);   /* won't fit */

    c = challenge("Basic realm=\"Members\"");
    CHECK(c.scheme == AUTH_BASIC && !strcmp(c.realm, "Members"));
    c = challenge("Digest realm=\"testrealm@host.com\", qop=\"auth,auth-int\", nonce=\"dcd98b7102dd2f0e8b11d0f600bfb0c093\", "
                  "opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"");
    CHECK(c.scheme == AUTH_DIGEST && c.qop && !c.sess && !strcmp(c.realm, "testrealm@host.com") &&
          !strcmp(c.opaque, "5ccc069c403ebaf9f0171e9517f40e41"));
    c = challenge("Digest realm=\"x\", nonce=\"n\", qop=\"auth-int\"");          /* can't answer */
    CHECK(c.scheme == AUTH_NONE);
    c = challenge("Digest realm=\"x\", nonce=\"n\", algorithm=SHA-256, Basic realm=\"y\"");  /* falls back */
    CHECK(c.scheme == AUTH_BASIC && !strcmp(c.realm, "y"));
    c = challenge("Basic realm=\"y\", Digest realm=\"x\", nonce=\"n\", algorithm=MD5-sess, stale=TRUE");
    CHECK(c.scheme == AUTH_DIGEST && c.sess && c.stale && !strcmp(c.realm, "x"));
    c = challenge("Basic realm=\"say \\\"hi\\\"\"");
    CHECK(!strcmp(c.realm, "say \"hi\""));
    c = challenge("Negotiate");
    CHECK(c.scheme == AUTH_NONE);

    /* RFC 2617's example, then the same nonce again, then RFC 2069 (no qop) */
    memset(&k, 0, sizeof k);
    c = challenge("Digest realm=\"testrealm@host.com\", qop=\"auth\", nonce=\"dcd98b7102dd2f0e8b11d0f600bfb0c093\", "
                  "opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"");
    cr = auth_keep(&k, "www.nowhere.org", 80, "/dir/index.html", &c, "Mufasa", "Circle Of Life");
    CHECK(auth_header(cr, "GET", "/dir/index.html", "0a4f113b", out, sizeof out) > 0);
    CHECK(strstr(out, "response=\"6629fae49393a05397450978507c4ef1\"") && strstr(out, "nc=00000001") &&
          strstr(out, "opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"") && !strncmp(out, "Digest username=\"Mufasa\"", 24));
    auth_header(cr, "GET", "/dir/index.html", "0a4f113b", out, sizeof out);
    CHECK(strstr(out, "nc=00000002") != NULL);
    c.qop = 0;
    auth_renew(cr, &c);
    auth_header(cr, "GET", "/dir/index.html", "0a4f113b", out, sizeof out);
    CHECK(strstr(out, "response=\"670fd8c2df070c60b045671b8b24ff02\"") && !strstr(out, "qop"));
    c.qop = c.sess = 1;
    auth_renew(cr, &c);
    auth_header(cr, "GET", "/dir/index.html", "0a4f113b", out, sizeof out);
    auth_header(cr, "GET", "/dir/index.html", "0a4f113b", out, sizeof out);
    CHECK(strstr(out, "response=\"d16df0df0d92cef8935129145e21b5e1\"") && strstr(out, "algorithm=MD5-sess"));

    /* Basic, and which paths get it without asking */
    c = challenge("Basic realm=\"WallyWorld\"");
    cr = auth_keep(&k, "Example.com", 8080, "/members/a/page.html?x=1", &c, "Aladdin", "open sesame");
    CHECK(!strcmp(cr->dir, "/members/a/"));
    CHECK(auth_header(cr, "GET", "/", "", out, sizeof out) > 0 && !strcmp(out, "Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ=="));
    CHECK(auth_for_path(&k, "example.com", 8080, "/members/a/other") == cr);
    CHECK(auth_for_path(&k, "example.com", 8080, "/members/b/") == NULL);
    CHECK(auth_for_path(&k, "example.com", 80, "/members/a/") == NULL);
    auth_keep(&k, "example.com", 8080, "/members/b/x", &c, "Aladdin", "open sesame");   /* same realm: widens */
    CHECK(k.n == 2 && !strcmp(cr->dir, "/members/") && auth_for_path(&k, "example.com", 8080, "/members/b/") == cr);
    auth_keep(&k, "example.com", 8080, "/other", &c, "Aladdin", "open sesame");
    CHECK(!strcmp(cr->dir, "/"));
    CHECK(auth_for_realm(&k, "EXAMPLE.COM", 8080, "WallyWorld") == cr);
    auth_forget(&k, cr);
    CHECK(k.n == 1 && auth_for_realm(&k, "example.com", 8080, "WallyWorld") == NULL);
    auth_clear(&k);
    CHECK(k.n == 0);
}

static void test_logins(void)
{
    Url u;
    char out[300];
    CHECK(url_parse("http://joe:p%40ss:w@Host.example:8080/a?b", &u) == 0);
    CHECK(!strcmp(u.user, "joe") && !strcmp(u.pass, "p@ss:w") && !strcmp(u.host, "Host.example") && u.port == 8080 &&
          !strcmp(u.path, "/a?b"));
    CHECK(url_parse("http://joe@host/", &u) == 0 && !strcmp(u.user, "joe") && !u.pass[0] && !strcmp(u.host, "host"));
    CHECK(url_parse("host/a@b", &u) == 0 && !u.user[0] && !strcmp(u.host, "host") && !strcmp(u.path, "/a@b"));
    strcpy(out, "http://joe:secret@host:81/a@b#c");
    url_strip_login(out);
    CHECK(!strcmp(out, "http://host:81/a@b#c"));
    CHECK(url_with_login("http://old:x@host/p#f", "j@e", "a:b/c d", out, sizeof out) == 0 &&
          !strcmp(out, "http://j%40e:a%3Ab%2Fc%20d@host/p#f"));
    CHECK(url_parse(out, &u) == 0 && !strcmp(u.user, "j@e") && !strcmp(u.pass, "a:b/c d"));
    CHECK(url_with_login("http://host/", "joe", "", out, sizeof out) == 0 && !strcmp(out, "http://joe@host/"));
    CHECK(url_with_login("http://host/", "joe", "pw", out, 12) == -1);
    CHECK(url_resolve("http://host/a/b", "http://u:p@other/x", out, sizeof out) == 0 && !strcmp(out, "http://u:p@other/x"));
}

static void test_referer(void)
{
    char out[300];
    CHECK(url_referer("http://www.example.com/a?q=1", "http://example.org/b", out, sizeof out) == 1 &&
          !strcmp(out, "http://www.example.com/a?q=1"));
    /* no name and password, and no fragment */
    CHECK(url_referer("http://joe:secret@www.example.com:8080/a@b?q#top", "http://example.org/", out, sizeof out) == 1 &&
          !strcmp(out, "http://www.example.com:8080/a@b?q"));
    CHECK(url_referer("http://joe@example.com/#x", "http://example.com/i.gif", out, sizeof out) == 1 &&
          !strcmp(out, "http://example.com/"));
    /* https: to https: goes, https: to anything else doesn't */
    CHECK(url_referer("https://example.com/s", "https://example.org/", out, sizeof out) == 1 && !strcmp(out, "https://example.com/s"));
    CHECK(url_referer("https://example.com/s", "http://example.org/", out, sizeof out) == 0);
    CHECK(url_referer("https://example.com/s", "mill://about", out, sizeof out) == 0);
    CHECK(url_referer("http://example.com/s", "https://example.org/", out, sizeof out) == 1);
    /* Mill's own pages and files may be the referrer */
    CHECK(url_referer("mill://config", "http://example.org/", out, sizeof out) == 1 && !strcmp(out, "mill://config"));
    CHECK(url_referer("file:///Disk/page.html", "http://example.org/", out, sizeof out) == 1 && !strcmp(out, "file:///Disk/page.html"));
    /* not whole addresses Mill knows */
    CHECK(url_referer("", "http://example.org/", out, sizeof out) == 0);
    CHECK(url_referer(NULL, "http://example.org/", out, sizeof out) == 0);
    CHECK(url_referer("example.com/a", "http://example.org/", out, sizeof out) == 0);
    CHECK(url_referer("view-source:http://example.com/", "http://example.org/", out, sizeof out) == 0);
    CHECK(url_referer("ftp://example.com/", "http://example.org/", out, sizeof out) == 0);
    /* too long, or more than one line */
    CHECK(url_referer("http://example.com/abcdef", "http://example.org/", out, 20) == 0);
    CHECK(url_referer("http://example.com/a\r\nX: y", "http://example.org/", out, sizeof out) == 0);
}

/* A GIF made here: 4 colors (white, black, gray, red), red transparent when transparent
   is set, the picture's index at (x, y) from at(). Its LZW data is a clear code before
   every two pixels, so the codes stay 3 bits wide. */
static long make_gif(unsigned char *g, short w, short h, int transparent, int (*at)(int x, int y))
{
    static const unsigned char head[] = { 'G', 'I', 'F', '8', '9', 'a' };
    static const unsigned char pal[] = { 255, 255, 255, 0, 0, 0, 128, 128, 128, 255, 0, 0 };
    unsigned char data[4096];
    long n = 0, d = 0, i, bits = 0;
    int nbits = 0, x, y, k = 0;
    memcpy(g, head, 6); n = 6;
    g[n++] = (unsigned char)w; g[n++] = (unsigned char)(w >> 8);
    g[n++] = (unsigned char)h; g[n++] = (unsigned char)(h >> 8);
    g[n++] = 0x81; g[n++] = 0; g[n++] = 0;
    memcpy(g + n, pal, 12); n += 12;
    if (transparent) {
        static const unsigned char gce[] = { 0x21, 0xF9, 4, 1, 0, 0, 3, 0 };
        memcpy(g + n, gce, 8); n += 8;
    }
    g[n++] = 0x2C;
    g[n++] = 0; g[n++] = 0; g[n++] = 0; g[n++] = 0;
    g[n++] = (unsigned char)w; g[n++] = (unsigned char)(w >> 8);
    g[n++] = (unsigned char)h; g[n++] = (unsigned char)(h >> 8);
    g[n++] = 0;
    g[n++] = 2;     /* minimum code size */
#define CODE(c) do { bits |= (long)(c) << nbits; nbits += 3; while (nbits >= 8) { data[d++] = (unsigned char)bits; bits >>= 8; nbits -= 8; } } while (0)
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            if (k++ % 2 == 0) CODE(4);
            CODE(at(x, y));
        }
    CODE(5);
    if (nbits) data[d++] = (unsigned char)bits;
#undef CODE
    for (i = 0; i < d; i += 255) {
        long c = d - i > 255 ? 255 : d - i;
        g[n++] = (unsigned char)c;
        memcpy(g + n, data + i, c); n += c;
    }
    g[n++] = 0;
    g[n++] = 0x3B;
    return n;
}

/* 2x2 checks of transparent red and black */
static int checks(int x, int y) { return (x / 2 + y / 2) % 2 ? 1 : 3; }
static int all_black(int x, int y) { (void)x; (void)y; return 1; }

static int bit_at(const unsigned char *b, short row_bytes, int x, int y) { return (b[(long)y * row_bytes + x / 8] >> (7 - x % 8)) & 1; }

static void test_transparent(void)
{
    unsigned char gif[8192];
    long len;
    Bitmap1 b;
    int x, y, ok;
    short scale;
    /* at its own size and twice it, each pixel's mask follows its source pixel */
    len = make_gif(gif, 8, 4, 1, checks);
    for (scale = 1; scale <= 2; scale++) {
        CHECK(image_decode(gif, len, 8 * scale, 4 * scale, &b, NULL, NULL) == 0 && b.mask);
        if (!b.mask) continue;
        ok = 1;
        for (y = 0; y < b.h; y++)
            for (x = 0; x < b.w; x++) {
                int covered = checks(x / scale, y / scale) != 3;
                if (bit_at(b.mask, b.row_bytes, x, y) != covered || bit_at(b.bits, b.row_bytes, x, y) != covered) ok = 0;
            }
        CHECK(ok);
        image_free(&b);
        CHECK(!b.bits && !b.mask);
    }
    /* half size: each target pixel is its nearest source pixel, in the mask and the bits */
    CHECK(image_decode(gif, len, 4, 2, &b, NULL, NULL) == 0 && b.mask);
    if (b.mask) {
        ok = 1;
        for (y = 0; y < b.h; y++)
            for (x = 0; x < b.w; x++) {
                int covered = checks(2 * x, 2 * y) != 3;
                if (bit_at(b.mask, b.row_bytes, x, y) != covered || (!covered && bit_at(b.bits, b.row_bytes, x, y))) ok = 0;
            }
        CHECK(ok);
    }
    image_free(&b);
    /* no transparent color, or one no pixel uses: no mask */
    len = make_gif(gif, 8, 4, 0, checks);
    CHECK(image_decode(gif, len, 8, 4, &b, NULL, NULL) == 0 && b.bits && !b.mask);
    image_free(&b);
    len = make_gif(gif, 8, 4, 1, all_black);
    CHECK(image_decode(gif, len, 8, 4, &b, NULL, NULL) == 0 && b.bits && !b.mask);
    image_free(&b);
}

/* Every node within the page's text and attributes, and joined up within the page */
static int doc_whole(const Doc *d)
{
    long i;
    for (i = 0; i < d->nnodes; i++) {
        const Node *n = &d->nodes[i];
        if (n->parent >= d->nnodes || n->first >= d->nnodes || n->next >= d->nnodes || n->attrs >= d->attrs_len) return 0;
        if (n->tag == TAG_TEXT && (n->text < 0 || n->len < 0 || n->text + n->len > d->text_len)) return 0;
    }
    return 1;
}

/* Every line's runs there, every run's text, link, and every link and anchor's name */
static int layout_whole(const Layout *L, const Doc *d)
{
    long i;
    for (i = 0; i < L->nlines; i++)
        if (L->lines[i].first < 0 || L->lines[i].first + L->lines[i].count > L->nruns) return 0;
    for (i = 0; i < L->nruns; i++) {
        const Run *r = &L->runs[i];
        if (r->kind == RUN_TEXT && r->text + r->len > d->text_len) return 0;
        if (r->kind == RUN_GEN && r->text + r->len > L->gen_len) return 0;
        if (r->link < 0 || r->link > L->nlinks) return 0;
    }
    for (i = 0; i < L->nlinks; i++) if (!L->links[i]) return 0;
    for (i = 0; i < L->nanchors; i++) if (!L->anchors[i].name) return 0;
    return 1;
}

/* Out of memory at each allocation in turn, once or from then on: the core says so, and
   what it has made so far holds together, for Mill to show as far as it got */
static void test_out_of_memory(void)
{
    static const char form[] = "<FORM><SELECT NAME=a><OPTION>x<OPTION SELECTED>y</SELECT><INPUT NAME=b VALUE=c>"
                               "<TEXTAREA NAME=d>e</TEXTAREA><ISINDEX></FORM>";
    unsigned char gif[8192];
    long len, total, k, gif_len;
    int rest, parsed, laid, all_ok = 1, image_ok = 1, forms_ok = 1;
    char *src = slurp("tests/fixtures/story.html", &len);
    Doc d;
    Layout L;
    Forms F;
    Bitmap1 b;
    if (!src) { printf("skip out of memory\n"); return; }
    allocs = 0;
    CHECK(html_parse(&d, src, len) == 0 && layout_doc(&L, &d, &M, 496) == 0);
    total = allocs;
    layout_free(&L);
    html_free(&d);
    for (rest = 0; rest <= 1; rest++)
        for (k = 0; k < total; k++) {
            allocs = 0; fail_at = k; fail_rest = rest;
            parsed = html_parse(&d, src, len);
            laid = d.nnodes ? layout_doc(&L, &d, &M, 496) : 0;
            fail_at = -1;
            if (parsed && parsed != -1) all_ok = 0;
            if (laid && laid != -1) all_ok = 0;
            if (!doc_whole(&d) || (d.nnodes && !layout_whole(&L, &d))) all_ok = 0;
            if (d.nnodes) { layout_hit(&L, 100, 50); layout_free(&L); }
            html_free(&d);
        }
    CHECK(all_ok);
    free(src);

    /* forms */
    allocs = 0;
    CHECK(html_parse(&d, form, strlen(form)) == 0 && forms_build(&F, &d) == 0);
    forms_free(&F);
    html_free(&d);
    total = allocs;
    for (k = 0; k < total; k++) {
        allocs = 0; fail_at = k; fail_rest = 1;
        memset(&F, 0, sizeof F);
        if (html_parse(&d, form, strlen(form)) == 0 && forms_build(&F, &d) == 0) forms_ok = 0;
        fail_at = -1;
        forms_free(&F);
        html_free(&d);
    }
    CHECK(forms_ok);

    /* pictures: nothing made, and IMAGE_NO_MEMORY rather than bad data */
    gif_len = make_gif(gif, 8, 4, 1, checks);
    allocs = 0;
    CHECK(image_decode(gif, gif_len, 16, 8, &b, NULL, NULL) == 0 && b.mask);
    image_free(&b);
    total = allocs;
    for (k = 0; k < total; k++) {
        allocs = 0; fail_at = k; fail_rest = 1;
        if (image_decode(gif, gif_len, 16, 8, &b, NULL, NULL) != IMAGE_NO_MEMORY || b.bits || b.mask) image_ok = 0;
        fail_at = -1;
        image_free(&b);
    }
    CHECK(image_ok);
}

/* Form state kept for Back: what's kept, what never is, and putting it back */
static void test_form_state(void)
{
    static const char page[] =
        "<FORM><INPUT NAME=q VALUE=a><INPUT TYPE=password NAME=p><INPUT TYPE=hidden NAME=h VALUE=x>"
        "<INPUT TYPE=checkbox NAME=c><INPUT TYPE=radio NAME=r VALUE=1 CHECKED><INPUT TYPE=radio NAME=r VALUE=2>"
        "<SELECT NAME=s><OPTION>one<OPTION>two<OPTION>three</SELECT>"
        "<SELECT NAME=m MULTIPLE><OPTION>a<OPTION SELECTED>b</SELECT>"
        "<TEXTAREA NAME=t>old</TEXTAREA><INPUT TYPE=file NAME=f><INPUT NAME=d DISABLED></FORM>"
        "<FORM><INPUT NAME=q></FORM>";
    /* the same page with a field added at the start of the first form, and one renamed */
    static const char changed[] =
        "<FORM><INPUT NAME=new><INPUT NAME=q VALUE=a></FORM><FORM><INPUT NAME=query></FORM>";
    static const char text[] = "hello & = : 100%\r\nline two";
    Doc d, d2;
    Forms F, G;
    char *state, *big;
    long i;
    CHECK(html_parse(&d, page, strlen(page)) == 0 && forms_build(&F, &d) == 0 && F.n == 12);
    CHECK(forms_save(&F, &d) == NULL);  /* nothing changed: nothing kept */
    forms_set_value(&F.fields[0], text);
    forms_set_value(&F.fields[1], "secret");
    forms_set_value(&F.fields[9], "Disk:Folder:file");
    forms_set_value(&F.fields[10], "can't");
    forms_click(&F, 3);
    forms_click(&F, 5);
    F.fields[6].selected[0] = 0; F.fields[6].selected[2] = 1;
    F.fields[7].selected[0] = 1;
    forms_set_value(&F.fields[8], "new\rtext");
    forms_set_value(&F.fields[11], "two");
    state = forms_save(&F, &d);
    CHECK(state && !strstr(state, "secret") && !strstr(state, "Disk") && !strstr(state, "can't") && !strchr(state, '\r'));
    /* back onto the same page, fresh */
    CHECK(forms_build(&G, &d) == 0);
    forms_restore(&G, &d, state);
    CHECK(!strcmp(G.fields[0].value, text) && !strcmp(G.fields[8].value, "new\rtext") && !strcmp(G.fields[11].value, "two"));
    CHECK(G.fields[3].checked && !G.fields[4].checked && G.fields[5].checked);
    CHECK(!G.fields[6].selected[0] && G.fields[6].selected[2] && G.fields[7].selected[0] && G.fields[7].selected[1]);
    CHECK(!strcmp(G.fields[1].value, "") && !strcmp(G.fields[9].value, "") && !strcmp(G.fields[10].value, ""));
    CHECK(!strcmp(G.fields[2].value, "x"));
    CHECK(forms_changed(&G, &d));
    /* saving what was put back gives the same */
    {
        char *again = forms_save(&G, &d);
        CHECK(again && !strcmp(again, state));
        free(again);
    }
    forms_free(&G);
    /* a page that changed: nothing lands on the wrong field */
    CHECK(html_parse(&d2, changed, strlen(changed)) == 0 && forms_build(&G, &d2) == 0 && G.n == 3);
    forms_restore(&G, &d2, state);
    CHECK(!strcmp(G.fields[0].value, "") && !strcmp(G.fields[1].value, "a") && !strcmp(G.fields[2].value, ""));
    forms_free(&G);
    html_free(&d2);
    /* the right form: the second form's q only goes to the second form's q */
    CHECK(html_parse(&d2, "<FORM></FORM><FORM><INPUT NAME=q></FORM>", 39) == 0 && forms_build(&G, &d2) == 0);
    forms_restore(&G, &d2, "0:1:0:q=yes");
    CHECK(!strcmp(G.fields[0].value, "yes"));
    forms_restore(&G, &d2, "0:0:0:q=no");
    CHECK(!strcmp(G.fields[0].value, "yes"));
    forms_free(&G);
    html_free(&d2);
    /* nonsense is left alone */
    CHECK(forms_build(&G, &d) == 0);
    forms_restore(&G, &d, "junk&0:&:0:q&99:0:0:q=x&0:0:3:q=1&3:0:3:c=2&6:0:10:s=01&6:0:10:s=0x1&0:0:0:q=%4");
    CHECK(!strcmp(G.fields[0].value, "%4") && !G.fields[3].checked && !G.fields[6].selected[2]);
    forms_free(&G);
    free(state);
    /* more than FORMS_STATE_MAX: none of it */
    big = malloc(FORMS_STATE_MAX + 2);
    for (i = 0; i < FORMS_STATE_MAX + 1; i++) big[i] = 'x';
    big[i] = 0;
    forms_set_value(&F.fields[8], big);
    CHECK(forms_save(&F, &d) == NULL);
    big[FORMS_STATE_MAX - 200] = 0;
    forms_set_value(&F.fields[8], big);
    CHECK((state = forms_save(&F, &d)) != NULL && strlen(state) <= FORMS_STATE_MAX);
    free(state);
    free(big);
    /* out of memory: nothing, and nothing left over */
    for (i = 0; i < 40; i++) {
        allocs = 0; fail_at = i; fail_rest = 1;
        state = forms_save(&F, &d);
        fail_at = -1;
        free(state);
    }
    forms_free(&F);
    html_free(&d);
}

int main(void)
{
    test_charset();
    test_fixed_width();
    test_disabled();
    test_utf8();
    test_parse();
    test_layout();
    test_lists();
    test_progress();
    test_floats();
    test_bgcolor();
    test_filetypes();
    test_imagemaps();
    test_isindex();
    test_cookies();
    test_urls();
    test_auth();
    test_logins();
    test_referer();
    test_transparent();
    test_out_of_memory();
    test_form_state();
    test_tables();
    test_forms();
    test_image("masthead-s.gif", 318, 57, 318, 57);
    test_image("partly.gif", 32, 32, 32, 32);
    test_image("partly.gif", 32, 32, 64, 64);
    test_image("rainier-t.jpg", 160, 120, 160, 120);
    test_image("rainier-t.jpg", 160, 120, 80, 60);
    test_fixture("front.html");
    test_fixture("weather.html");
    test_fixture("story.html");
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
