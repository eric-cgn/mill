/* Host tests for the Image Formats Plug-In's decoders (plugins/images): every fixture in
   tests/fixtures/images (made by its make.py) decoded a byte at a time, a few bytes at a
   time, and all at once, and checked against the test card it was drawn from; then cut
   short, scrambled, and starved of memory, which must end cleanly. The grays each
   decoder gives are written to tests/out/img-*.pgm for looking at. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../plugins/images/images.h"

/* Inflate: Mill's own, called directly here (a Plug-In calls it through the host) */
static void *t_gz_begin(void *ref, const GzMem *mem, int raw, gz_out out, void *out_ref)
{
    (void)ref;
    return gz_begin_with(mem, raw, out, out_ref);
}
static const GzCalls test_gz = { NULL, t_gz_begin, gz_data, gz_unused, gz_end };

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* ---- the test card, as make.py draws it ---- */

static int card(int x, int y, int w, int h)
{
    double cx = w / 2.0, cy = h / 2.0, r = (w < h ? w : h) * 0.4, dx = x + 0.5 - cx, dy = y + 0.5 - cy;
    double d = __builtin_sqrt(dx * dx + dy * dy), t = r * 0.08 > 1.5 ? r * 0.08 : 1.5;
    if (__builtin_fabs(dx) < r * 0.3 && __builtin_fabs(dy) < r * 0.3) return 255;
    if (__builtin_fabs(d - r) < t) return 0;
    return (int)(255.0 * x / (w - 1 > 1 ? w - 1 : 1));
}

static int color_lum(int x, int y, int w, int h)
{
    int g = card(x, y, w, h), gg = g, b = g;
    if (g > 0 && g < 255) { gg = (g + 85 * (y * 3 / (h > 1 ? h : 1))) % 256; b = 255 - g; }
    return (g * 77 + gg * 151 + b * 28) >> 8;
}

/* ---- a sink that keeps the grays at the image's own size ---- */

typedef struct {
    short w, h, sizes, photo;
    unsigned char *px;
    long allocs, fail_at;       /* fail the fail_at'th allocation (0: never) */
    int drew;                   /* draw_pict: calls, and what it was given */
    long pict_len;
    short pict_w, pict_h;
    unsigned char pict_last;
} Sink;

static int s_size(void *ref, short w, short h, int photo)
{
    Sink *s = ref;
    if (w < 1 || h < 1) return IMG_BAD;
    free(s->px);
    s->px = malloc((long)w * h);
    memset(s->px, 255, (long)w * h);
    s->w = w; s->h = h; s->photo = (short)photo;
    s->sizes++;
    return 0;
}
static void s_drawn(void *ref, short *w, short *h) { Sink *s = ref; *w = s->w; *h = s->h; }
static void s_rect(void *ref, short x, short y, short w, short h, const unsigned char *gray, const unsigned char *opaque)
{
    Sink *s = ref;
    short i, j;
    CHECK(opaque == NULL);      /* only PNG's (tests/test_png.c) */
    CHECK(s->px != NULL);
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            CHECK(x + i >= 0 && x + i < s->w && y + j >= 0 && y + j < s->h);
            if (x + i >= 0 && x + i < s->w && y + j >= 0 && y + j < s->h) s->px[(long)(y + j) * s->w + x + i] = gray[(long)j * w + i];
        }
}
static void *s_alloc(void *ref, long n)
{
    Sink *s = ref;
    if (++s->allocs == s->fail_at) return NULL;
    return malloc(n);
}
static void s_release(void *ref, void *p) { (void)ref; free(p); }
static int s_draw_pict(void *ref, const unsigned char *pic, long len, short w, short h)
{
    Sink *s = ref;
    s->drew++;
    s->pict_len = len;
    s->pict_w = w; s->pict_h = h;
    s->pict_last = pic[len - 1];
    return 0;
}

static unsigned char *slurp(const char *name, long *len)
{
    char path[256];
    FILE *f;
    unsigned char *b;
    snprintf(path, sizeof path, "tests/fixtures/images/%s", name);
    if (!(f = fopen(path, "rb"))) { printf("FAIL: no %s\n", path); failures++; return NULL; }
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc(*len + 1);
    if (fread(b, 1, *len, f) != (size_t)*len) *len = 0;
    fclose(f);
    return b;
}

/* Decode d in pieces of piece bytes: the last data's answer (IMG_DONE...) */
static int decode(int format, const unsigned char *d, long len, long piece, Sink *s)
{
    ImgSink k;
    void *dec;
    long pos = 0;
    int r = IMG_MORE;
    long fail = s->fail_at;
    free(s->px);
    memset(s, 0, sizeof *s);
    s->fail_at = fail;
    k.ref = s; k.size = s_size; k.drawn = s_drawn; k.rect = s_rect; k.alloc = s_alloc; k.release = s_release;
    k.draw_pict = s_draw_pict;
    k.flags = 0;
    k.gz = &test_gz;
    if (!(dec = img_begin(format, &k))) return IMG_NO_MEMORY;
    while (pos < len && r == IMG_MORE) {
        long n = len - pos < piece ? len - pos : piece;
        r = img_data(dec, d + pos, n);
        pos += n;
    }
    if (r == IMG_MORE) r = img_data(dec, d + pos, 0);
    img_end(dec);
    return r;
}

static void save(const Sink *s, const char *name)
{
    char path[256];
    FILE *f;
    snprintf(path, sizeof path, "tests/out/img-%s.pgm", name);
    if (!s->px || !(f = fopen(path, "wb"))) return;
    fprintf(f, "P5\n%d %d\n255\n", s->w, s->h);
    fwrite(s->px, 1, (long)s->w * s->h, f);
    fclose(f);
}

enum { BILEVEL, GRAY, COLOR, PICT2 };

/* How far the grays are from the card's: pixels more than tol off */
static long misses(const Sink *s, int kind, int tol)
{
    long bad = 0;
    int x, y;
    for (y = 0; y < s->h; y++)
        for (x = 0; x < s->w; x++) {
            int want, got = s->px[(long)y * s->w + x];
            if (kind == BILEVEL) want = card(x, y, s->w, s->h) < 128 ? 0 : 255;
            else if (kind == GRAY) want = card(x, y, s->w, s->h);
            else if (kind == COLOR) want = color_lum(x, y, s->w, s->h);
            else {
                /* PICT 2's quarters: an 8-bit card, a 32-bit and a 16-bit color one, and a
                   half-size bitmap stretched */
                int qw = s->w / 2, qh = s->h / 2, qx = x % qw, qy = y % qh;
                if (x < qw && y < qh) want = card(qx, qy, qw, qh);
                else if (y < qh) want = color_lum(qx, qy, qw, qh);
                else if (x < qw) want = color_lum(qx, qy, qw, qh);
                else want = card(qx / 2, qy / 2, qw / 2, qh / 2) < 128 ? 0 : 255;
            }
            if (got - want > tol || want - got > tol) bad++;
        }
    return bad;
}

static void one(const char *name, int format, short w, short h, int kind, int tol)
{
    static const long pieces[] = { 1, 7, 1L << 20 };
    unsigned char *d, *first = NULL;
    long len, k;
    Sink s;
    memset(&s, 0, sizeof s);
    if (!(d = slurp(name, &len))) return;
    for (k = 0; k < 3; k++) {
        int r = decode(format, d, len, pieces[k], &s);
        if (r != IMG_DONE || s.w != w || s.h != h) printf("  %s: piece %ld: %d, %dx%d\n", name, pieces[k], r, s.w, s.h);
        CHECK(r == IMG_DONE && s.w == w && s.h == h);
        if (!s.px || s.w != w || s.h != h) break;
        if (!k) {
            long m = misses(&s, kind, tol);
            if (m) printf("  %s: %ld of %ld pixels off\n", name, m, (long)w * h);
            CHECK(m == 0);
            save(&s, name);
            first = malloc((long)w * h);
            memcpy(first, s.px, (long)w * h);
        } else CHECK(!memcmp(first, s.px, (long)w * h));    /* however it comes */
    }
    free(first);
    free(s.px);
    free(d);
}

/* Cut short at every length (to 600 bytes, then in steps), scrambled bytes, and memory
   that runs out at each allocation: any answer will do, as long as nothing goes wrong */
static void robust(const char *name, int format)
{
    unsigned char *d, *e;
    long len, cut, i, k;
    unsigned seed = 12345;
    Sink s;
    memset(&s, 0, sizeof s);
    if (!(d = slurp(name, &len))) return;
    for (cut = 0; cut < len; cut += cut < 600 ? 1 : 97) decode(format, d, cut, 5, &s);
    e = malloc(len);
    for (k = 0; k < 300; k++) {
        memcpy(e, d, len);
        for (i = 0; i < 1 + (long)(k % 6); i++) {
            seed = seed * 1103515245 + 12345;
            e[(seed >> 8) % len] ^= (unsigned char)(1 + (seed >> 20) % 255);
        }
        decode(format, e, len, 1 + k % 13, &s);
    }
    for (k = 1; k < 12; k++) {
        int r;
        s.fail_at = k;
        r = decode(format, d, len, 64, &s);
        if (r != IMG_DONE && r != IMG_NO_MEMORY) printf("  %s: allocation %ld failing: %d\n", name, k, r);
        CHECK(r == IMG_DONE || r == IMG_NO_MEMORY);
        s.fail_at = 0;
    }
    free(e);
    free(s.px);
    free(d);
}

static void test_pict_v1(void)
{
    unsigned char *d;
    long len;
    Sink s;
    memset(&s, 0, sizeof s);
    if (!(d = slurp("card-v1.pict", &len))) return;
    /* checked as it comes, then drawn by the System at the size Mill picked */
    CHECK(decode(IMG_PICT, d, len, 3, &s) == IMG_DONE && s.drew == 1 && s.pict_len == len - 512 &&
          s.pict_w == 48 && s.pict_h == 32 && s.pict_last == 0xFF);
    /* cut short: drawn up to its last whole opcode, with an end put there */
    CHECK(decode(IMG_PICT, d, len - 40, 3, &s) == IMG_DONE && s.drew == 1 && s.pict_len < len - 512 && s.pict_last == 0xFF);
    /* an opcode version 1 hasn't got: never drawn */
    d[512 + 12] = 0xC0;
    CHECK(decode(IMG_PICT, d, len, 3, &s) == IMG_BAD && s.drew == 0);
    free(s.px);
    free(d);
}

static void test_pnm(void)
{
    static const char *const plain[] = { "P1\n# a comment\n3 2\n1 0 1\n0 1 0\n", "P1 3 2 101010" };
    static const unsigned char raw[] = { 'P', '4', ' ', '3', ' ', '2', '\n', 0xA0, 0x40 };
    static const unsigned char rgb16[] = { 'P', '6', ' ', '1', ' ', '1', ' ', '6', '5', '5', '3', '5', '\n', 255, 255, 0, 0, 0, 0 };
    Sink s;
    int k;
    memset(&s, 0, sizeof s);
    for (k = 0; k < 2; k++) {
        CHECK(decode(IMG_PNM, (const unsigned char *)plain[k], strlen(plain[k]), 1, &s) == IMG_DONE);
        CHECK(s.w == 3 && s.h == 2 && s.px[0] == 0 && s.px[1] == 255 && s.px[4] == 0 && s.photo == 0);
    }
    CHECK(decode(IMG_PNM, raw, sizeof raw, 1, &s) == IMG_DONE && s.w == 3 && s.px[0] == 0 && s.px[1] == 255 && s.px[4] == 0);
    CHECK(decode(IMG_PNM, rgb16, sizeof rgb16, 2, &s) == IMG_DONE && s.px[0] == 77 && s.photo == 1);
    CHECK(decode(IMG_PNM, (const unsigned char *)"P7 1 1", 6, 6, &s) == IMG_BAD);
    free(s.px);
}

int main(void)
{
    one("card.mac", IMG_MACPAINT, 576, 720, BILEVEL, 0);
    one("card11.xbm", IMG_XBM, 37, 21, BILEVEL, 0);
    one("card10.xbm", IMG_XBM, 37, 21, BILEVEL, 0);
    one("card-p1.pnm", IMG_PNM, 23, 17, BILEVEL, 0);
    one("card-p2.pnm", IMG_PNM, 23, 17, GRAY, 0);
    one("card-p3.pnm", IMG_PNM, 23, 17, COLOR, 1);
    one("card-p4.pnm", IMG_PNM, 23, 17, BILEVEL, 0);
    one("card-p5.pnm", IMG_PNM, 23, 17, GRAY, 0);
    one("card-p6.pnm", IMG_PNM, 23, 17, COLOR, 1);
    one("card1.bmp", IMG_BMP, 37, 21, BILEVEL, 0);
    one("card4.bmp", IMG_BMP, 37, 21, GRAY, 17);
    one("card8.bmp", IMG_BMP, 37, 21, GRAY, 1);
    one("card24.bmp", IMG_BMP, 37, 21, COLOR, 1);
    one("card-os2.bmp", IMG_BMP, 37, 21, GRAY, 1);
    one("card-rle4.bmp", IMG_BMP, 37, 21, GRAY, 17);
    one("card-v2.pict", IMG_PICT, 48, 32, PICT2, 9);
    one("card-v2x.pict", IMG_PICT, 96, 64, PICT2, 9);
    one("card-wide.pict", IMG_PICT, 420, 16, PICT2, 9);
    {
        /* RLE8 with a delta: the row it skips stays white past where it moved to */
        unsigned char *d;
        long len, m;
        Sink s;
        memset(&s, 0, sizeof s);
        if ((d = slurp("card-rle8.bmp", &len))) {
            CHECK(decode(IMG_BMP, d, len, 3, &s) == IMG_DONE && s.w == 37 && s.h == 21);
            m = misses(&s, GRAY, 1);
            if (m <= 0 || m > 40) printf("  card-rle8.bmp: %ld off\n", m);
            CHECK(m > 0 && m <= 40);
            save(&s, "card-rle8.bmp");
            free(d);
        }
        free(s.px);
    }
    test_pict_v1();
    test_pnm();
    {
        static const struct { const char *name; int format; } all[] = {
            { "card.mac", IMG_MACPAINT }, { "card11.xbm", IMG_XBM }, { "card10.xbm", IMG_XBM },
            { "card-p1.pnm", IMG_PNM }, { "card-p3.pnm", IMG_PNM }, { "card-p6.pnm", IMG_PNM }, { "card1.bmp", IMG_BMP },
            { "card8.bmp", IMG_BMP }, { "card24.bmp", IMG_BMP }, { "card-os2.bmp", IMG_BMP }, { "card-rle8.bmp", IMG_BMP },
            { "card-rle4.bmp", IMG_BMP }, { "card-v1.pict", IMG_PICT }, { "card-v2.pict", IMG_PICT },
            { "card-v2x.pict", IMG_PICT }, { "card-wide.pict", IMG_PICT }
        };
        unsigned k;
        for (k = 0; k < sizeof all / sizeof all[0]; k++) robust(all[k].name, all[k].format);
    }
    printf(failures ? "%d failures\n" : "images: all passed\n", failures);
    return failures != 0;
}
