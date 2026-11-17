/* Host tests for the Image Formats Plug-In's PNG decoder (plugins/images/png.c): every file
   in tests/fixtures/png (made by its make.py, which checks each against PIL) decoded a byte at a time, a few bytes at a time,
   and all at once, with CRCs checked and not, and compared with its .ref.gz: every gray,
   every pixel's opacity, and each pixel given exactly once. Then broken, cut short, too
   large, and starved of memory, which must end cleanly. The grays are written to
   tests/out/png-NAME.pgm for looking at. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../plugins/images/images.h"
#include "../src/core/inflate.h"

/* Inflate: Mill's own, called directly here (a Plug-In calls it through the host) */
static void *t_gz_begin(void *ref, const GzMem *mem, int raw, gz_out out, void *out_ref)
{
    (void)ref;
    return gz_begin_with(mem, raw, out, out_ref);
}
static const GzCalls test_gz = { NULL, t_gz_begin, gz_data, gz_unused, gz_end };

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* ---- a sink that keeps the picture at its own size ---- */

typedef struct {
    short w, h, sizes, photo;
    unsigned char *gray, *op, *count;   /* count: how many times each pixel was given */
    int mask;                           /* -1 until the first rect: whether it gave opacity */
    long rects;
    long allocs, fail_at;               /* fail the fail_at'th allocation (0: never) */
    long live, peak;                    /* bytes the decoder has */
} Sink;

static int s_size(void *ref, short w, short h, int photo)
{
    Sink *s = ref;
    if (w < 1 || h < 1) return IMG_BAD;
    s->w = w; s->h = h; s->photo = (short)photo;
    s->gray = malloc((long)w * h);
    s->op = malloc((long)w * h);
    s->count = calloc((long)w * h, 1);
    memset(s->gray, 255, (long)w * h);
    memset(s->op, 1, (long)w * h);
    s->sizes++;
    return 0;
}

static void s_drawn(void *ref, short *w, short *h) { Sink *s = ref; *w = s->w; *h = s->h; }

static void s_rect(void *ref, short x, short y, short w, short h, const unsigned char *gray, const unsigned char *opaque)
{
    Sink *s = ref;
    short i, j;
    CHECK(s->gray != NULL);
    if (!s->gray) return;
    if (s->mask < 0) s->mask = opaque != NULL;
    CHECK(s->mask == (opaque != NULL));
    s->rects++;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            long at = (long)(y + j) * s->w + x + i;
            if (x + i < 0 || x + i >= s->w || y + j < 0 || y + j >= s->h) { CHECK(0); continue; }
            s->gray[at] = gray[(long)j * w + i];
            s->op[at] = opaque ? opaque[(long)j * w + i] : 1;
            if (s->count[at] < 255) s->count[at]++;
        }
}

typedef struct { long n; } Head;

static void *s_alloc(void *ref, long n)
{
    Sink *s = ref;
    Head *h;
    if (++s->allocs == s->fail_at) return NULL;
    if (!(h = malloc(sizeof(Head) + n))) return NULL;
    h->n = n;
    s->live += n;
    if (s->live > s->peak) s->peak = s->live;
    return h + 1;
}

static void s_release(void *ref, void *p)
{
    Sink *s = ref;
    Head *h = (Head *)p - 1;
    s->live -= h->n;
    free(h);
}

static void s_clear(Sink *s)
{
    free(s->gray); free(s->op); free(s->count);
    memset(s, 0, sizeof *s);
    s->mask = -1;
}

/* Decode d in pieces of piece bytes: the last png_data's answer */
static int decode(const unsigned char *d, long len, long piece, int crc, Sink *s)
{
    ImgSink k;
    void *dec;
    long pos = 0, fail = s->fail_at;
    int r = IMG_MORE;
    s_clear(s);
    s->fail_at = fail;
    k.ref = s; k.size = s_size; k.drawn = s_drawn; k.rect = s_rect; k.alloc = s_alloc; k.release = s_release;
    k.draw_pict = NULL;
    k.flags = crc ? IMG_CHECK_CRC : 0;
    k.gz = &test_gz;
    if (!(dec = img_begin(IMG_PNG, &k))) return IMG_NO_MEMORY;
    while (pos < len && r == IMG_MORE) {
        long n = len - pos < piece ? len - pos : piece;
        r = img_data(dec, d + pos, n);
        pos += n;
    }
    if (r == IMG_MORE) r = img_data(dec, d + pos, 0);
    img_end(dec);
    CHECK(s->live == 0);
    return r;
}

/* ---- files ---- */

static unsigned char *slurp(const char *name, long *len)
{
    char path[256];
    FILE *f;
    unsigned char *b;
    snprintf(path, sizeof path, "tests/fixtures/png/%s", name);
    if (!(f = fopen(path, "rb"))) { printf("FAIL: no %s\n", path); failures++; return NULL; }
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc(*len + 1);
    if (fread(b, 1, *len, f) != (size_t)*len) *len = 0;
    fclose(f);
    return b;
}

/* A .ref.gz, unpacked with Mill's own inflate */
typedef struct { unsigned char *b; long n, room; } Buf;

static int grow(void *ref, const unsigned char *p, long n)
{
    Buf *b = ref;
    if (b->n + n > b->room) { b->room = (b->n + n) * 2; b->b = realloc(b->b, b->room); }
    memcpy(b->b + b->n, p, n);
    b->n += n;
    return 0;
}

typedef struct { int w, h, mask; unsigned char *gray, *op, *all; } Ref;

static int load_ref(const char *name, Ref *r)
{
    char file[128];
    long len;
    unsigned char *z;
    Buf b = { NULL, 0, 0 };
    void *g;
    int k;
    snprintf(file, sizeof file, "%s.ref.gz", name);
    if (!(z = slurp(file, &len))) return -1;
    g = gz_begin(grow, &b);
    k = gz_data(g, z, len);
    gz_end(g);
    free(z);
    if (k != 1 || sscanf((char *)b.b, "%d %d %d", &r->w, &r->h, &r->mask) != 3) { free(b.b); return -1; }
    r->all = b.b;
    r->gray = (unsigned char *)memchr(b.b, '\n', b.n) + 1;
    r->op = r->gray + (long)r->w * r->h;
    return 0;
}

static void save(const Sink *s, const char *name)
{
    char path[256];
    FILE *f;
    snprintf(path, sizeof path, "tests/out/png-%s.pgm", name);
    if (!s->gray || !(f = fopen(path, "wb"))) return;
    fprintf(f, "P5\n%d %d\n255\n", s->w, s->h);
    fwrite(s->gray, 1, (long)s->w * s->h, f);
    fclose(f);
}

/* Whether the sink has exactly what ref says, each pixel given once */
static int same(const Sink *s, const Ref *r, const char *name, long piece)
{
    long i, n = (long)r->w * r->h, bad = 0, twice = 0;
    if (s->w != r->w || s->h != r->h) { printf("  %s: %dx%d, not %dx%d\n", name, s->w, s->h, r->w, r->h); return 0; }
    if ((s->mask > 0) != r->mask) { printf("  %s: mask %d, not %d\n", name, s->mask, r->mask); return 0; }
    for (i = 0; i < n; i++) {
        /* where the page shows through, the gray doesn't matter */
        if ((s->gray[i] != r->gray[i] && (!r->mask || r->op[i])) || (r->mask && s->op[i] != r->op[i])) {
            if (!bad) printf("  %s (pieces of %ld): (%ld, %ld) gray %d op %d, not %d %d\n", name, piece, i % r->w, i / r->w,
                             s->gray[i], s->op[i], r->gray[i], r->op[i]);
            bad++;
        }
        if (s->count[i] != 1) twice++;
    }
    if (twice) printf("  %s: %ld pixels not given exactly once\n", name, twice);
    return !bad && !twice;
}

/* ---- the tests ---- */

static const long PIECES[] = { 1, 7, 509, 1L << 30 };

/* crcs: 2 to decode with CRCs checked as well as not, 1 for not only */
static void test_ok(const char *name, const char *refname, const unsigned char *d, long len, int crcs)
{
    Ref r;
    Sink s;
    size_t k;
    int crc;
    memset(&s, 0, sizeof s);
    s.mask = -1;
    if (load_ref(refname, &r)) { CHECK(0); return; }
    for (crc = 0; crc < crcs; crc++)
        for (k = 0; k < sizeof PIECES / sizeof PIECES[0]; k++) {
            int got = decode(d, len, PIECES[k], crc, &s);
            if (got != IMG_DONE) printf("  %s: %d\n", name, got);
            CHECK(got == IMG_DONE);
            CHECK(s.sizes == 1);
            CHECK(same(&s, &r, name, PIECES[k]));
        }
    save(&s, name);
    s_clear(&s);
    free(r.all);
}

/* Every allocation failing in turn: IMG_NO_MEMORY, nothing left allocated */
static void test_memory(const char *name, const unsigned char *d, long len)
{
    Sink s;
    long k, total;
    memset(&s, 0, sizeof s);
    s.mask = -1;
    decode(d, len, 1L << 30, 0, &s);
    total = s.allocs;
    for (k = 1; k <= total; k++) {
        int r;
        s.fail_at = k;
        r = decode(d, len, 509, 0, &s);
        if (r != IMG_NO_MEMORY) printf("  %s: allocation %ld failing: %d\n", name, k, r);
        CHECK(r == IMG_NO_MEMORY);
    }
    s_clear(&s);
}

static void test_cut(const char *name, const unsigned char *d, long len, int interlaced)
{
    Ref r;
    Sink s;
    long y, x, whole = 0;
    memset(&s, 0, sizeof s);
    s.mask = -1;
    if (load_ref("broken", &r)) { CHECK(0); return; }
    CHECK(decode(d, len, 509, 0, &s) == IMG_DONE);
    CHECK(s.w == r.w && s.h == r.h);
    if (!interlaced && s.gray) {
        for (y = 0; y < r.h; y++) {
            int ref = 1, white = 1;
            for (x = 0; x < r.w; x++) {
                if (s.gray[y * r.w + x] != r.gray[y * r.w + x]) ref = 0;
                if (s.gray[y * r.w + x] != 255) white = 0;
            }
            CHECK(ref || white);
            whole += ref;
        }
        CHECK(whole > 0);
        printf("  %s: %ld of %d rows\n", name, whole, r.h);
    }
    if (interlaced && s.count) for (x = 0; x < (long)r.w * r.h; x++) CHECK(s.count[x] == 1);
    save(&s, name);
    s_clear(&s);
    free(r.all);
}

int main(void)
{
    FILE *f = fopen("tests/fixtures/png/list.txt", "r");
    char line[200], name[100], kind[20], ref[100];
    int files = 0;
    if (!f) { printf("FAIL: no tests/fixtures/png/list.txt\n"); return 1; }
    while (fgets(line, sizeof line, f)) {
        char file[128];
        long len;
        unsigned char *d;
        Sink s;
        if (sscanf(line, "%99s %19s %99s", name, kind, ref) != 3) continue;
        snprintf(file, sizeof file, "%s.png", name);
        if (!(d = slurp(file, &len))) continue;
        files++;
        memset(&s, 0, sizeof s);
        s.mask = -1;
        if (!strcmp(kind, "ok")) {
            test_ok(name, ref, d, len, 2);
            test_memory(name, d, len);
            if (!strncmp(name, "big-", 4)) {
                decode(d, len, 1024, 0, &s);
                printf("  %s: %ld bytes at most, %ld rects\n", name, s.peak, s.rects);
            }
        } else if (!strcmp(kind, "bad")) {
            size_t k;
            for (k = 0; k < sizeof PIECES / sizeof PIECES[0]; k++) {
                int r = decode(d, len, PIECES[k], 0, &s);
                if (r != IMG_BAD) printf("  %s: %d\n", name, r);
                CHECK(r == IMG_BAD);
            }
        } else if (!strcmp(kind, "crc")) {
            test_ok(name, ref, d, len, 1);
            CHECK(decode(d, len, 7, 1, &s) == IMG_BAD);
        } else if (!strcmp(kind, "cut")) {
            test_cut(name, d, len, strstr(name, "-i") != NULL);
        } else if (!strcmp(kind, "large")) {
            CHECK(decode(d, len, 509, 0, &s) == IMG_TOO_LARGE);
        } else if (!strcmp(kind, "any")) {
            size_t k;
            for (k = 0; k < sizeof PIECES / sizeof PIECES[0]; k++) decode(d, len, PIECES[k], 1, &s), decode(d, len, PIECES[k], 0, &s);
        }
        s_clear(&s);
        free(d);
    }
    fclose(f);
    printf("%d files, %d checks: %s (%d failed)\n", files, checks, failures ? "FAILED" : "ok", failures);
    return failures != 0;
}
