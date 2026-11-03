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

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static long fake_width(void *ctx, const MStyle *s, const char *t, long len) { (void)ctx; (void)t; return len * s->size / 2; }
static void fake_metrics(void *ctx, const MStyle *s, short *a, short *d, short *l) { (void)ctx; *a = s->size * 3 / 4; *d = s->size / 4; *l = 1; }
static Metrics M = { fake_width, fake_metrics, NULL, NULL, NULL };

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
    forms_set_value(&F.fields[text], "M\x8Ell\xD5s & co\rnext");  /* Mac Roman, a curly quote, a CR */
    CHECK(forms_submit(&F, &d, F.fields[text].form, go, 0, 0, NULL, NULL, &sub) == 0);
    CHECK(!sub.post && !strcmp(sub.action, "/search"));
    if (strcmp(sub.data, "q=M%E9ll%27s+%26+co%0D%0Anext&lang=en&r=b&other=z&s=2&t=line+%3CB%3Eone%3C%2FB%3E%0D%0Aline+two&go=Go"))
        { printf("FAIL form data: %s\n", sub.data); failures++; }
    submission_free(&sub);
    forms_reset(&F, &d, F.fields[text].form);
    CHECK(F.fields[r1].checked && !F.fields[r2].checked && !strcmp(F.fields[text].value, "caf\x8E au lait"));

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

static void check_cookies(Jar *j, const char *host, const char *path, unsigned long now, const char *want)
{
    char got[256];
    jar_header(j, host, path, now, got, sizeof got);
    if (strcmp(got, want)) printf("cookies for %s%s: \"%s\", not \"%s\"\n", host, path, got, want);
    CHECK(!strcmp(got, want));
}

static void set_cookie(Jar *j, const char *host, const char *path, const char *line, unsigned long now)
{
    jar_set(j, host, path, line, strlen(line), now);
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
        jar_header(&j, "a.example.com", "/", y2k, got, sizeof got);  /* too long to fit: none */
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
    }
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

int main(void)
{
    test_charset();
    test_utf8();
    test_parse();
    test_layout();
    test_lists();
    test_floats();
    test_bgcolor();
    test_filetypes();
    test_imagemaps();
    test_isindex();
    test_cookies();
    test_urls();
    test_auth();
    test_logins();
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
