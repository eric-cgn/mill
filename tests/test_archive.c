/* Host tests for the container cores (plugins/archive): every fixture in
   tests/fixtures/archive (made by its make.sh) listed and opened, pushed a byte at a time,
   a few bytes at a time, and all at once, or pulled; members compared with the files they
   were made from (tests/fixtures/archive/src). Then cut short, scrambled, starved of
   memory, and given reads that fail, which must all end cleanly (the sanitizers watch). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../plugins/archive/archive.h"
#include "../src/core/macenc.h"

/* Inflate: Mill's own, called directly here (a Plug-In calls it through the host) */
static void *t_gz_begin(void *ref, const GzMem *mem, int raw, gz_out out, void *out_ref)
{
    (void)ref;
    return gz_begin_with(mem, raw, out, out_ref);
}
static const GzCalls test_gz = { NULL, t_gz_begin, gz_data, gz_unused, gz_end };

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define DIR "tests/fixtures/archive/"
#define DATE_MAC (883008000UL + 2082844800UL)   /* 1997-12-25 00:00 UTC, in the Mac's seconds */

typedef struct { unsigned char *p; long n; } Buf;

static Buf load(const char *name)
{
    char path[256];
    FILE *f;
    Buf b = { NULL, 0 };
    snprintf(path, sizeof path, DIR "%s", name);
    if (!(f = fopen(path, "rb"))) { printf("FAIL can't read %s\n", path); failures++; b.p = malloc(1); return b; }
    fseek(f, 0, SEEK_END);
    b.n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b.p = malloc(b.n ? b.n : 1);
    if (fread(b.p, 1, b.n, f) != (size_t)b.n) failures++;
    fclose(f);
    return b;
}

/* ---- a host that keeps what it's given ---- */

#define MAX_ENTRIES 32

typedef struct {
    char name[300], comment[300], link[120];
    ArcEntry e;
} Got;

typedef struct {
    const unsigned char *src;       /* pull: the archive */
    long src_len, said_size;        /* said_size: what size() answers */
    long reads, fail_read;          /* fail the fail_read'th read (0: never) */
    long allocs, fail_alloc, live;
    Got ent[MAX_ENTRIES];
    int n;
    unsigned char *out;
    long olen, ocap;
    int stop_entry, stop_out;
    int members;                /* member calls: the opened member, before its bytes */
    char member_name[64], member_rest[64];
    long olen_at_member;
    int both;                   /* member asks for both forks */
} T;

static void *t_alloc(void *ref, long n)
{
    T *t = ref;
    if (++t->allocs == t->fail_alloc) return NULL;
    t->live++;
    return malloc(n);
}
static void t_release(void *ref, void *p) { T *t = ref; t->live--; free(p); }
static int t_entry(void *ref, const ArcEntry *e)
{
    T *t = ref;
    if (t->n < MAX_ENTRIES) {
        Got *g = &t->ent[t->n];
        g->e = *e;
        snprintf(g->name, sizeof g->name, "%s", e->name);
        snprintf(g->comment, sizeof g->comment, "%s", e->comment ? e->comment : "");
        snprintf(g->link, sizeof g->link, "%s", e->link ? e->link : "");
    }
    t->n++;
    return t->stop_entry && t->n >= t->stop_entry;
}
static int t_member(void *ref, const ArcEntry *e, const char *rest, int *fork)
{
    T *t = ref;
    if (t->both) *fork = ARC_BOTH_FORKS;
    t->members++;
    snprintf(t->member_name, sizeof t->member_name, "%s", e->name);
    snprintf(t->member_rest, sizeof t->member_rest, "%s", rest);
    t->olen_at_member = t->olen;
    return 0;
}
static int t_out(void *ref, const unsigned char *p, long n)
{
    T *t = ref;
    CHECK(n > 0);
    if (t->olen + n > t->ocap) {
        t->ocap = (t->olen + n) * 2;
        t->out = realloc(t->out, t->ocap);
    }
    memcpy(t->out + t->olen, p, n);
    t->olen += n;
    return t->stop_out && t->olen >= t->stop_out;
}
static long t_read_at(void *ref, unsigned long at, unsigned char *p, long n)
{
    T *t = ref;
    if (++t->reads == t->fail_read) return -1;
    CHECK(n > 0);
    if (at >= (unsigned long)t->src_len) return 0;
    if (n > t->src_len - (long)at) n = t->src_len - (long)at;
    memcpy(p, t->src + at, n);
    return n;
}
static long t_size(void *ref) { return ((T *)ref)->said_size; }

static ArcHost host(T *t, const Buf *src)
{
    ArcHost h;
    memset(&h, 0, sizeof h);
    h.ref = t;
    h.alloc = t_alloc;
    h.release = t_release;
    h.entry = t_entry;
    h.out = t_out;
    h.member = t_member;
    h.gz = &test_gz;
    if (src) {
        t->src = src->p;
        t->src_len = t->said_size = src->n;
        h.read_at = t_read_at;
        h.size = t_size;
    }
    return h;
}

static void reset(T *t)
{
    free(t->out);
    memset(t, 0, sizeof *t);
}

static ArcRequest req(int open, const char *path)
{
    ArcRequest r;
    memset(&r, 0, sizeof r);
    r.open = open;
    r.path = path ? path : "";
    return r;
}

/* Push src in pieces of chunk (0: all at once), then the end; the result. *rest gets arc_rest. */
static int push(T *t, int format, const Buf *src, long chunk, const ArcRequest *r, int seekable, char *rest)
{
    ArcHost h = host(t, seekable ? src : NULL);
    void *d = arc_begin(format, &h, r);
    long at = 0;
    int res = ARC_MORE;
    if (rest) rest[0] = 0;
    if (!d) return ARC_NO_MEMORY;
    if (chunk <= 0) chunk = src->n ? src->n : 1;
    while (res == ARC_MORE && at < src->n) {
        long k = src->n - at < chunk ? src->n - at : chunk;
        res = arc_data(d, src->p + at, k);
        at += k;
    }
    if (res == ARC_MORE) res = arc_data(d, NULL, 0);
    CHECK(arc_data(d, src->p, src->n ? 1 : 0) == res);    /* the result stays */
    if (rest && arc_rest(d)) snprintf(rest, 300, "%s", arc_rest(d));
    arc_end(d);
    CHECK(t->live == 0);
    return res;
}

static int pull(T *t, int format, const Buf *src, const ArcRequest *r, char *rest)
{
    ArcHost h = host(t, src);
    const char *rp;
    int res = arc_pull(format, &h, r, &rp);
    if (rest) snprintf(rest, 300, "%s", rp ? rp : "");
    CHECK(t->live == 0);
    return res;
}

static int same(const T *t, const Buf *b) { return t->olen == b->n && (!b->n || !memcmp(t->out, b->p, b->n)); }

static const Got *find(const T *t, const char *name)
{
    int i;
    for (i = 0; i < t->n && i < MAX_ENTRIES; i++) if (!strcmp(t->ent[i].name, name)) return &t->ent[i];
    return NULL;
}

static const long CHUNKS[] = { 1, 3, 7, 100, 512, 4096, 0 };
#define NCHUNKS (int)(sizeof CHUNKS / sizeof CHUNKS[0])

/* ---- the helpers ---- */

static void test_names(void)
{
    char out[64];
    int folder;
    static const struct { const char *in; const char *want; int folder; } cases[] = {
        { "docs/readme.txt", "docs/readme.txt", 0 }, { "/abs.txt", "abs.txt", 0 }, { "./a/./b/", "a/b", 1 },
        { "a//b", "a/b", 0 }, { "../x", NULL, 0 }, { "a/../b", NULL, 0 }, { "a/..", NULL, 0 }, { "", NULL, 0 },
        { "/", NULL, 0 }, { "./", NULL, 0 }, { "a\001b", NULL, 0 }, { "..a/b..", "..a/b..", 0 }, { "docs/", "docs", 1 } };
    unsigned i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        long n = arc_clean((const unsigned char *)cases[i].in, (long)strlen(cases[i].in), 0, out, sizeof out, &folder);
        if (cases[i].want) CHECK(n == (long)strlen(cases[i].want) && !strcmp(out, cases[i].want) && folder == cases[i].folder);
        else CHECK(n == -1);
    }
    CHECK(arc_clean((const unsigned char *)"a\\..\\b", 6, 1, out, sizeof out, &folder) == -1);
    CHECK(arc_clean((const unsigned char *)"a\\b", 3, 1, out, sizeof out, &folder) == 3 && !strcmp(out, "a/b"));
    CHECK(arc_clean((const unsigned char *)"a\\b", 3, 0, out, sizeof out, &folder) == 3 && !strcmp(out, "a\\b"));
    CHECK(arc_clean((const unsigned char *)"abcdef", 6, 0, out, 6, &folder) == -1);     /* no room */
    CHECK(arc_clean((const unsigned char *)"a/b\0c", 5, 0, out, sizeof out, &folder) == -1);

    arc_hfs_name("docs/readme.txt", ARC_LATIN1, out);
    CHECK(!strcmp(out, "readme.txt"));
    arc_hfs_name("a:b", ARC_LATIN1, out);
    CHECK(!strcmp(out, "a-b"));
    arc_hfs_name(".profile", ARC_LATIN1, out);
    CHECK(!strcmp(out, "_profile"));
    arc_hfs_name("A very long file name that goes on and on.html", ARC_LATIN1, out);
    CHECK(strlen(out) == 31 && !strcmp(out, "A very long file name that.html"));
    arc_hfs_name("A very long file name without an extension at all", ARC_LATIN1, out);
    CHECK(strlen(out) == 31 && !strncmp(out, "A very long file name without a", 31));
    arc_hfs_name("caf\202.txt", ARC_CP437, out);    /* CP437 0x82 is e acute, Mac Roman 0x8E */
    CHECK(!strcmp(out, "caf\216.txt"));
    arc_hfs_name("caf\351.txt", ARC_LATIN1, out);   /* Latin-1 0xE9 */
    CHECK(!strcmp(out, "caf\216.txt"));
    arc_hfs_name("caf\216.txt", ARC_MACROMAN, out);
    CHECK(!strcmp(out, "caf\216.txt"));
    arc_hfs_name("box\263", ARC_CP437, out);        /* a box-drawing line: nothing like it */
    CHECK(!strcmp(out, "box?"));
    arc_hfs_name("", ARC_LATIN1, out);
    CHECK(!strcmp(out, "Untitled"));

    CHECK(arc_on_path("docs", "docs/readme.txt") && !strcmp(arc_on_path("docs", "docs/readme.txt"), "readme.txt"));
    CHECK(arc_on_path("kit.tar", "/kit.tar") && !*arc_on_path("kit.tar", "kit.tar"));
    CHECK(!arc_on_path("doc", "docs/readme.txt"));
    CHECK(!strcmp(arc_first("kit.tar/docs/a"), "docs/a") && !*arc_first("kit.tar"));

    CHECK(arc_crc32(0, (const unsigned char *)"123456789", 9) == 0xCBF43926UL);
    CHECK(arc_crc32(arc_crc32(0, (const unsigned char *)"1234", 4), (const unsigned char *)"56789", 5) == 0xCBF43926UL);
    CHECK(arc_unix_date(0) == 2082844800UL && arc_unix_date(883008000UL) == DATE_MAC);
    /* 25 December 1997 is (1997-1980) << 9 | 12 << 5 | 25; 13:45:30 is 13 << 11 | 45 << 5 | 15 */
    CHECK(arc_dos_date((17 << 9) | (12 << 5) | 25, 0) == DATE_MAC);
    CHECK(arc_dos_date((17 << 9) | (12 << 5) | 25, (13 << 11) | (45 << 5) | 15) == DATE_MAC + 13 * 3600 + 45 * 60 + 30);
    CHECK(arc_dos_date((20 << 9) | (3 << 5) | 1, 0) == 2082844800UL + 951868800UL);  /* 2000-03-01, after a leap day */
    CHECK(arc_dos_date(0, 0) == 0);
}

/* ---- zip ---- */

static void test_zip(void)
{
    T t = { 0 };
    Buf kit = load("kit.zip"), readme = load("src/docs/readme.txt"), hello = load("src/hello.txt"), bin = load("src/bin.dat");
    Buf b;
    ArcRequest r = req(0, NULL);
    char rest[300];
    const Got *g;
    unsigned long pos_readme, pos_hello;

    CHECK(pull(&t, ARC_ZIP, &kit, &r, rest) == ARC_DONE);
    CHECK(t.n == 6);
    CHECK((g = find(&t, "hello.txt")) && g->e.kind == ARC_FILE && g->e.size == 24 && g->e.packed == 24 && g->e.modified == DATE_MAC &&
          g->e.charset == ARC_CP437 && g->e.mode == 0644);
    pos_hello = g ? g->e.pos : 0;
    CHECK((g = find(&t, "docs")) && g->e.kind == ARC_FOLDER && g->e.size == -1);
    CHECK((g = find(&t, "docs/sub")) && g->e.kind == ARC_FOLDER);
    CHECK((g = find(&t, "docs/empty.txt")) && g->e.kind == ARC_FILE && g->e.size == 0);
    CHECK((g = find(&t, "docs/readme.txt")) && g->e.size == readme.n && g->e.packed < readme.n / 10);
    pos_readme = g ? g->e.pos : 0;
    CHECK((g = find(&t, "bin.dat")) && g->e.size == 20000 && !g->e.flags);
    reset(&t);

    r = req(1, "docs/readme.txt");
    CHECK(pull(&t, ARC_ZIP, &kit, &r, rest) == ARC_DONE && same(&t, &readme) && !strcmp(rest, ""));
    reset(&t);
    r = req(1, "bin.dat");
    CHECK(pull(&t, ARC_ZIP, &kit, &r, rest) == ARC_DONE && same(&t, &bin));
    reset(&t);
    r = req(1, "docs/empty.txt");
    CHECK(pull(&t, ARC_ZIP, &kit, &r, rest) == ARC_DONE && t.olen == 0);
    reset(&t);
    /* the path walk: names until a file, then the rest */
    r = req(1, "/docs/readme.txt/inner/page.html");
    CHECK(pull(&t, ARC_ZIP, &kit, &r, rest) == ARC_DONE && same(&t, &readme) && !strcmp(rest, "inner/page.html"));
    reset(&t);
    r = req(1, "docs");
    CHECK(pull(&t, ARC_ZIP, &kit, &r, rest) == ARC_NOT_FOUND && t.olen == 0);
    reset(&t);
    r = req(1, "nothing.txt");
    CHECK(pull(&t, ARC_ZIP, &kit, &r, rest) == ARC_NOT_FOUND);
    reset(&t);
    /* the position: straight there, and only a hint */
    r = req(1, "docs/readme.txt");
    r.has_pos = 1;
    r.pos = pos_readme;
    CHECK(pull(&t, ARC_ZIP, &kit, &r, rest) == ARC_DONE && same(&t, &readme));
    CHECK(t.reads <= 6);
    reset(&t);
    r.pos = pos_hello;
    CHECK(pull(&t, ARC_ZIP, &kit, &r, rest) == ARC_DONE && same(&t, &readme));
    reset(&t);
    r.pos = 12345;
    CHECK(pull(&t, ARC_ZIP, &kit, &r, rest) == ARC_DONE && same(&t, &readme));
    reset(&t);
    /* stopping */
    r = req(0, NULL);
    t.stop_entry = 2;
    CHECK(pull(&t, ARC_ZIP, &kit, &r, rest) == ARC_STOPPED && t.n == 2);
    reset(&t);
    r = req(1, "docs/readme.txt");
    t.stop_out = 1;
    CHECK(pull(&t, ARC_ZIP, &kit, &r, rest) == ARC_STOPPED);
    reset(&t);
    /* no size, no zip */
    t.said_size = -1;
    {
        ArcHost h = host(&t, &kit);
        const char *rp;
        t.said_size = -1;
        CHECK(arc_pull(ARC_ZIP, &h, &r, &rp) == ARC_UNSUPPORTED && !rp);
    }
    reset(&t);
    CHECK(arc_begin(ARC_ZIP, NULL, &r) == NULL);    /* zip isn't pushed */

    b = load("stored.zip");
    r = req(1, "docs/readme.txt");
    CHECK(pull(&t, ARC_ZIP, &b, &r, rest) == ARC_DONE && same(&t, &readme));
    reset(&t);
    free(b.p);

    b = load("link.zip");
    r = req(0, NULL);
    CHECK(pull(&t, ARC_ZIP, &b, &r, rest) == ARC_DONE && t.n == 2);
    CHECK((g = find(&t, "link")) && g->e.kind == ARC_LINK && g->e.size == -1);
    reset(&t);
    r = req(1, "link");
    CHECK(pull(&t, ARC_ZIP, &b, &r, rest) == ARC_UNSUPPORTED);
    reset(&t);
    free(b.p);

    b = load("comment.zip");
    r = req(1, "hello.txt");
    CHECK(pull(&t, ARC_ZIP, &b, &r, rest) == ARC_DONE && same(&t, &hello));
    reset(&t);
    free(b.p);

    b = load("encrypted.zip");
    r = req(0, NULL);
    CHECK(pull(&t, ARC_ZIP, &b, &r, rest) == ARC_DONE && t.n == 1 && (t.ent[0].e.flags & ARC_ENCRYPTED));
    reset(&t);
    r = req(1, "hello.txt");
    CHECK(pull(&t, ARC_ZIP, &b, &r, rest) == ARC_UNSUPPORTED && t.olen == 0);
    reset(&t);
    free(b.p);

    b = load("zip64.zip");
    r = req(0, NULL);
    CHECK(pull(&t, ARC_ZIP, &b, &r, rest) == ARC_UNSUPPORTED && t.n == 0);
    reset(&t);
    free(b.p);

    b = load("evil.zip");
    r = req(0, NULL);
    CHECK(pull(&t, ARC_ZIP, &b, &r, rest) == ARC_DONE && t.n == 2);
    CHECK(find(&t, "abs.txt") && (g = find(&t, "dos/path.txt")) && !strcmp(g->comment, "From a PC."));
    reset(&t);
    r = req(1, "dos/path.txt");
    CHECK(pull(&t, ARC_ZIP, &b, &r, rest) == ARC_DONE && same(&t, &hello));
    reset(&t);
    r = req(1, "../evil.txt");
    CHECK(pull(&t, ARC_ZIP, &b, &r, rest) == ARC_NOT_FOUND);
    reset(&t);
    free(b.p);

    free(kit.p); free(readme.p); free(hello.p); free(bin.p);
}

/* ---- tar ---- */

static void same_listing(const T *a, const T *b)
{
    int i;
    CHECK(a->n == b->n);
    for (i = 0; i < a->n && i < b->n && i < MAX_ENTRIES; i++)
        CHECK(!strcmp(a->ent[i].name, b->ent[i].name) && a->ent[i].e.kind == b->ent[i].e.kind && a->ent[i].e.size == b->ent[i].e.size &&
              a->ent[i].e.pos == b->ent[i].e.pos && a->ent[i].e.modified == b->ent[i].e.modified && !strcmp(a->ent[i].link, b->ent[i].link));
}

static void test_tar(void)
{
    T t = { 0 }, ref = { 0 };
    Buf kit = load("kit.tar"), readme = load("src/docs/readme.txt"), hello = load("src/hello.txt"), bin = load("src/bin.dat"), b;
    ArcRequest r = req(0, NULL);
    char rest[300];
    const Got *g;
    int c;
    unsigned long pos_readme = 0;

    CHECK(pull(&ref, ARC_TAR, &kit, &r, rest) == ARC_DONE);
    CHECK(ref.n == 7);
    CHECK((g = find(&ref, "hello.txt")) && g->e.kind == ARC_FILE && g->e.size == 24 && g->e.modified == DATE_MAC && g->e.mode == 0644 &&
          g->e.charset == ARC_LATIN1 && g->e.pos % 512 == 0);
    CHECK((g = find(&ref, "docs")) && g->e.kind == ARC_FOLDER);
    CHECK((g = find(&ref, "docs/sub")) && g->e.kind == ARC_FOLDER);
    CHECK((g = find(&ref, "docs/empty.txt")) && g->e.size == 0);
    CHECK((g = find(&ref, "link")) && g->e.kind == ARC_LINK && !strcmp(g->link, "hello.txt"));
    CHECK((g = find(&ref, "docs/readme.txt")) && g->e.size == readme.n);
    if (g) pos_readme = g->e.pos;
    for (c = 0; c < NCHUNKS; c++) {
        CHECK(push(&t, ARC_TAR, &kit, CHUNKS[c], &r, 0, rest) == ARC_DONE);
        same_listing(&t, &ref);
        reset(&t);
    }
    for (c = 0; c < NCHUNKS; c++) {
        r = req(1, "docs/readme.txt/x/y");
        CHECK(push(&t, ARC_TAR, &kit, CHUNKS[c], &r, 0, rest) == ARC_DONE && same(&t, &readme) && !strcmp(rest, "x/y"));
        reset(&t);
        r = req(1, "bin.dat");
        CHECK(push(&t, ARC_TAR, &kit, CHUNKS[c], &r, 0, rest) == ARC_DONE && same(&t, &bin));
        reset(&t);
    }
    r = req(1, "docs/readme.txt");
    CHECK(pull(&t, ARC_TAR, &kit, &r, rest) == ARC_DONE && same(&t, &readme));
    reset(&t);
    r.has_pos = 1;
    r.pos = pos_readme;
    CHECK(pull(&t, ARC_TAR, &kit, &r, rest) == ARC_DONE && same(&t, &readme) && t.reads == 1 + (readme.n + 511) / 512);
    reset(&t);
    r.pos = 0;      /* another member's header: only a hint */
    CHECK(pull(&t, ARC_TAR, &kit, &r, rest) == ARC_DONE && same(&t, &readme));
    reset(&t);
    r.pos = 100;
    CHECK(pull(&t, ARC_TAR, &kit, &r, rest) == ARC_DONE && same(&t, &readme));
    reset(&t);
    r = req(1, "hello.txt");
    CHECK(pull(&t, ARC_TAR, &kit, &r, rest) == ARC_DONE && same(&t, &hello));
    reset(&t);
    r = req(1, "docs/empty.txt");
    CHECK(pull(&t, ARC_TAR, &kit, &r, rest) == ARC_DONE && t.olen == 0);
    reset(&t);
    CHECK(push(&t, ARC_TAR, &kit, 0, &r, 0, rest) == ARC_DONE && t.olen == 0);
    reset(&t);
    r = req(1, "link");
    CHECK(pull(&t, ARC_TAR, &kit, &r, rest) == ARC_UNSUPPORTED);
    reset(&t);
    r = req(1, "docs");
    CHECK(pull(&t, ARC_TAR, &kit, &r, rest) == ARC_NOT_FOUND);
    reset(&t);
    CHECK(push(&t, ARC_TAR, &kit, 7, &r, 0, rest) == ARC_NOT_FOUND);
    reset(&t);

    b = load("v7.tar");
    r = req(0, NULL);
    CHECK(pull(&t, ARC_TAR, &b, &r, rest) == ARC_DONE && t.n == 5);
    CHECK((g = find(&t, "docs")) && g->e.kind == ARC_FOLDER);
    CHECK((g = find(&t, "docs/readme.txt")) && g->e.size == readme.n && g->e.modified == DATE_MAC);
    reset(&t);
    r = req(1, "docs/readme.txt");
    CHECK(push(&t, ARC_TAR, &b, 512, &r, 0, rest) == ARC_DONE && same(&t, &readme));
    reset(&t);
    free(b.p);

    b = load("deep.tar");
    r = req(0, NULL);
    CHECK(pull(&t, ARC_TAR, &b, &r, rest) == ARC_DONE && t.n == 1 && strlen(t.ent[0].name) == 130 && !strncmp(t.ent[0].name, "aaaa", 4) &&
          !strcmp(t.ent[0].name + 121, "/deep.txt"));
    reset(&t);
    free(b.p);

    b = load("evil.tar");
    r = req(0, NULL);
    CHECK(push(&t, ARC_TAR, &b, 0, &r, 0, rest) == ARC_DONE && t.n == 3);
    CHECK(find(&t, "abs.txt") && find(&t, "ok/fine.txt") && find(&t, "ok.txt"));
    reset(&t);
    r = req(1, "ok.txt");
    CHECK(push(&t, ARC_TAR, &b, 0, &r, 0, rest) == ARC_DONE && same(&t, &hello));
    reset(&t);
    free(b.p);

    reset(&ref);
    free(kit.p); free(readme.p); free(hello.p); free(bin.p);
}

/* ---- gzip ---- */

static void test_gzip(void)
{
    T t = { 0 };
    Buf b = load("readme.txt.gz"), readme = load("src/docs/readme.txt"), hello = load("src/hello.txt"), tgz = load("kit.tgz"),
        tar = load("kit.tar");
    ArcRequest r = req(0, NULL);
    char rest[300];
    int c;

    for (c = 0; c < NCHUNKS; c++) {
        CHECK(push(&t, ARC_GZIP, &b, CHUNKS[c], &r, 0, rest) == ARC_DONE && t.n == 1);
        CHECK(!strcmp(t.ent[0].name, "readme.txt") && t.ent[0].e.modified == DATE_MAC && t.ent[0].e.size == -1 && t.ent[0].e.kind == ARC_FILE);
        reset(&t);
        r = req(1, "readme.txt");
        CHECK(push(&t, ARC_GZIP, &b, CHUNKS[c], &r, 0, rest) == ARC_DONE && same(&t, &readme) && !strcmp(rest, ""));
        reset(&t);
        r = req(0, NULL);
    }
    /* the size, when the end can be read */
    CHECK(push(&t, ARC_GZIP, &b, 0, &r, 1, rest) == ARC_DONE && t.n == 1 && t.ent[0].e.size == readme.n && t.ent[0].e.packed == b.n);
    reset(&t);
    free(b.p);

    /* no name in the header: the archive's, less .gz */
    b = load("noname.gz");
    r.name = "http://host/files/hello.txt.gz";
    CHECK(push(&t, ARC_GZIP, &b, 0, &r, 0, rest) == ARC_DONE && !strcmp(t.ent[0].name, "hello.txt") && t.ent[0].e.modified == 0);
    reset(&t);
    r.name = NULL;
    CHECK(push(&t, ARC_GZIP, &b, 0, &r, 0, rest) == ARC_DONE && !strcmp(t.ent[0].name, "Untitled"));
    reset(&t);
    r = req(1, "anything");
    CHECK(push(&t, ARC_GZIP, &b, 1, &r, 0, rest) == ARC_DONE && same(&t, &hello));
    reset(&t);
    free(b.p);

    /* every header field */
    b = load("full.gz");
    r = req(0, NULL);
    for (c = 0; c < NCHUNKS; c++) {
        CHECK(push(&t, ARC_GZIP, &b, CHUNKS[c], &r, 1, rest) == ARC_DONE && t.n == 1 && !strcmp(t.ent[0].name, "full.txt") &&
              !strcmp(t.ent[0].comment, "Merry Christmas") && t.ent[0].e.modified == DATE_MAC && t.ent[0].e.size == hello.n);
        reset(&t);
    }
    r = req(1, "full.txt");
    CHECK(push(&t, ARC_GZIP, &b, 1, &r, 0, rest) == ARC_DONE && same(&t, &hello));
    reset(&t);
    free(b.p);

    /* .tgz: names its member .tar; then the tar inside, pushed from the gzip's output */
    r = req(0, NULL);
    r.name = "kit.tgz";
    CHECK(push(&t, ARC_GZIP, &tgz, 0, &r, 0, rest) == ARC_DONE && !strcmp(t.ent[0].name, "kit.tar"));
    reset(&t);
    r = req(1, "kit.tar/docs/readme.txt");
    CHECK(push(&t, ARC_GZIP, &tgz, 1000, &r, 0, rest) == ARC_DONE && same(&t, &tar) && !strcmp(rest, "docs/readme.txt"));
    {
        T inner = { 0 };
        Buf whole = { t.out, t.olen };
        ArcRequest ri = req(1, rest);
        CHECK(push(&inner, ARC_TAR, &whole, 333, &ri, 0, NULL) == ARC_DONE && same(&inner, &readme));
        reset(&inner);
    }
    reset(&t);
    r = req(1, "kit.tar");
    t.stop_out = 1;
    CHECK(push(&t, ARC_GZIP, &tgz, 0, &r, 0, rest) == ARC_STOPPED);
    reset(&t);

    free(tgz.p); free(tar.p); free(readme.p); free(hello.p);
}

/* ---- MacBinary and BinHex ---- */

static void test_mac(void)
{
    T t = { 0 };
    Buf data = load("src/mac-data"), rsrc = load("src/mac-rsrc"), b;
    ArcRequest r;
    char rest[300];
    static const char *const files[] = { "mb1.bin", "mb2.bin", "mb3.bin" };
    int i, c;

    for (i = 0; i < 3; i++) {
        b = load(files[i]);
        r = req(0, NULL);
        CHECK(push(&t, ARC_MACBINARY, &b, 0, &r, 0, rest) == ARC_DONE && t.n == 1);
        CHECK(!strcmp(t.ent[0].name, i == 1 ? "Read Me-Now" : "Read Me") && t.ent[0].e.charset == ARC_MACROMAN);
        CHECK(!memcmp(t.ent[0].e.type, "TEXT", 4) && !memcmp(t.ent[0].e.creator, "ttxt", 4) && (t.ent[0].e.flags & ARC_FINDER));
        CHECK(t.ent[0].e.data_len == data.n && t.ent[0].e.rsrc_len == (i == 2 ? 0 : rsrc.n) && t.ent[0].e.size == data.n);
        CHECK(t.ent[0].e.modified == DATE_MAC && t.ent[0].e.packed == b.n);
        CHECK((t.ent[0].e.finder_flags & 0xFF00) == 0);    /* 0x0100, "inited", is cleared */
        if (i) CHECK(t.ent[0].e.finder_flags == 0x20);
        reset(&t);
        for (c = 0; c < NCHUNKS; c++) {
            r = req(1, "Read Me/inner");
            CHECK(push(&t, ARC_MACBINARY, &b, CHUNKS[c], &r, 0, rest) == ARC_DONE && same(&t, &data) && !strcmp(rest, "inner"));
            reset(&t);
            r.fork = ARC_RSRC_FORK;
            CHECK(push(&t, ARC_MACBINARY, &b, CHUNKS[c], &r, 0, rest) == ARC_DONE);
            CHECK(i == 2 ? t.olen == 0 : same(&t, &rsrc));
            reset(&t);
        }
        free(b.p);
    }

    b = load("readme.hqx");
    for (c = 0; c < NCHUNKS; c++) {
        r = req(0, NULL);
        CHECK(push(&t, ARC_BINHEX, &b, CHUNKS[c], &r, 0, rest) == ARC_DONE && t.n == 1 && !strcmp(t.ent[0].name, "Read Me"));
        CHECK(!memcmp(t.ent[0].e.type, "TEXT", 4) && !memcmp(t.ent[0].e.creator, "ttxt", 4) && t.ent[0].e.data_len == data.n &&
              t.ent[0].e.rsrc_len == rsrc.n && t.ent[0].e.finder_flags == 0);
        reset(&t);
        r = req(1, "Read Me");
        CHECK(push(&t, ARC_BINHEX, &b, CHUNKS[c], &r, 0, rest) == ARC_DONE && same(&t, &data));
        reset(&t);
        r.fork = ARC_RSRC_FORK;
        CHECK(push(&t, ARC_BINHEX, &b, CHUNKS[c], &r, 0, rest) == ARC_DONE && same(&t, &rsrc));
        reset(&t);
    }
    free(b.p);
    b = load("empty.hqx");
    r = req(1, "Nothing");
    CHECK(push(&t, ARC_BINHEX, &b, 1, &r, 0, rest) == ARC_DONE && t.olen == 0);
    reset(&t);
    r.fork = ARC_RSRC_FORK;
    CHECK(push(&t, ARC_BINHEX, &b, 1, &r, 0, rest) == ARC_DONE && t.olen == 0);
    reset(&t);
    free(b.p);

    /* what Mill's own BinHex encoder (src/core/macenc.c, for uploads) makes reads back */
    {
        static unsigned char text[8192];
        unsigned char h[100], crc[2];
        unsigned short k;
        Hqx q;
        int n = binhex_header(h, "Up", "TEXT", "ttxt", 0, data.n, rsrc.n);
        hqx_begin(&q, text, sizeof text);
        hqx_put(&q, h, n);
        hqx_put(&q, data.p, data.n);
        k = crc_ccitt(0, data.p, data.n); crc[0] = (unsigned char)(k >> 8); crc[1] = (unsigned char)k;
        hqx_put(&q, crc, 2);
        hqx_put(&q, rsrc.p, rsrc.n);
        k = crc_ccitt(0, rsrc.p, rsrc.n); crc[0] = (unsigned char)(k >> 8); crc[1] = (unsigned char)k;
        hqx_put(&q, crc, 2);
        hqx_finish(&q);
        b.p = text;
        b.n = q.len;
        r = req(1, "Up");
        CHECK(push(&t, ARC_BINHEX, &b, 5, &r, 0, rest) == ARC_DONE && same(&t, &data));
        reset(&t);
        r.fork = ARC_RSRC_FORK;
        CHECK(push(&t, ARC_BINHEX, &b, 5, &r, 0, rest) == ARC_DONE && same(&t, &rsrc));
        reset(&t);
    }
    /* no "(This file must be converted" line: not BinHex */
    b.p = (unsigned char *)"Some text\r:abc:\r";
    b.n = 16;
    r = req(0, NULL);
    CHECK(push(&t, ARC_BINHEX, &b, 0, &r, 0, rest) == ARC_BAD && t.n == 0);
    reset(&t);
    free(data.p); free(rsrc.p);
}

/* ---- the opened member, told before its bytes; both forks of a Mac file ---- */

static void test_members(void)
{
    static const struct { const char *file; int format; int pulled; const char *path, *name, *rest; } M[] = {
        { "kit.zip", ARC_ZIP, 1, "docs/readme.txt", "docs/readme.txt", "" },
        { "kit.zip", ARC_ZIP, 1, "docs/readme.txt/x/y", "docs/readme.txt", "x/y" },
        { "kit.tar", ARC_TAR, 1, "docs/readme.txt", "docs/readme.txt", "" },
        { "kit.tar", ARC_TAR, 0, "hello.txt/more", "hello.txt", "more" },
        { "readme.txt.gz", ARC_GZIP, 0, "readme.txt", "readme.txt", "" },
        { "kit.tgz", ARC_GZIP, 0, "kit.tar/docs/readme.txt", "kit.tar", "docs/readme.txt" },
        { "mb2.bin", ARC_MACBINARY, 0, "Read Me", "Read Me-Now", "" },
        { "readme.hqx", ARC_BINHEX, 0, "Read Me/a", "Read Me", "a" },
    };
    unsigned i;
    T t = { 0 };
    char rest[300];
    for (i = 0; i < sizeof M / sizeof M[0]; i++) {
        Buf b = load(M[i].file);
        ArcRequest r = req(1, M[i].path);
        int res;
        if (M[i].format == ARC_GZIP) r.name = M[i].file;
        res = M[i].pulled ? pull(&t, M[i].format, &b, &r, rest) : push(&t, M[i].format, &b, 7, &r, 0, rest);
        CHECK(res == ARC_DONE && t.members == 1 && t.olen_at_member == 0 && t.olen > 0);
        CHECK(!strcmp(t.member_name, M[i].name) && !strcmp(t.member_rest, M[i].rest));
        if (strcmp(t.member_name, M[i].name) || strcmp(t.member_rest, M[i].rest))
            printf("FAIL member %s %s: %s, %s\n", M[i].file, M[i].path, t.member_name, t.member_rest);
        reset(&t);
        free(b.p);
    }
    /* both forks, one after the other */
    {
        Buf data = load("src/mac-data"), rsrc = load("src/mac-rsrc"), both;
        static const char *const files[] = { "mb1.bin", "mb2.bin", "readme.hqx" };
        both.n = data.n + rsrc.n;
        both.p = malloc(both.n);
        memcpy(both.p, data.p, data.n);
        memcpy(both.p + data.n, rsrc.p, rsrc.n);
        for (i = 0; i < 3; i++) {
            Buf b = load(files[i]);
            long c;
            for (c = 1; c < 300; c += 37) {
                ArcRequest r = req(1, "Read Me");
                r.fork = ARC_BOTH_FORKS;
                CHECK(push(&t, i < 2 ? ARC_MACBINARY : ARC_BINHEX, &b, c, &r, 0, rest) == ARC_DONE && same(&t, &both));
                reset(&t);
                /* or asked for by the host when it hears of the member */
                r.fork = ARC_DATA_FORK;
                t.both = 1;
                CHECK(push(&t, i < 2 ? ARC_MACBINARY : ARC_BINHEX, &b, c, &r, 0, rest) == ARC_DONE && same(&t, &both));
                reset(&t);
            }
            free(b.p);
        }
        free(data.p); free(rsrc.p); free(both.p);
    }
}

/* ---- cut short, scrambled, starved ---- */

typedef struct { const char *file; int format; int pulled; const char *member; const char *expect; int fork; } Case;

static const Case CASES[] = {
    { "kit.zip", ARC_ZIP, 1, "docs/readme.txt", "src/docs/readme.txt", 0 },
    { "stored.zip", ARC_ZIP, 1, "hello.txt", "src/hello.txt", 0 },
    { "evil.zip", ARC_ZIP, 1, "dos/path.txt", "src/hello.txt", 0 },
    { "kit.tar", ARC_TAR, 1, "docs/readme.txt", "src/docs/readme.txt", 0 },
    { "kit.tar", ARC_TAR, 0, "hello.txt", "src/hello.txt", 0 },
    { "v7.tar", ARC_TAR, 0, "docs/readme.txt", "src/docs/readme.txt", 0 },
    { "readme.txt.gz", ARC_GZIP, 0, "readme.txt", "src/docs/readme.txt", 0 },
    { "full.gz", ARC_GZIP, 0, "full.txt", "src/hello.txt", 0 },
    { "mb2.bin", ARC_MACBINARY, 0, "Read Me", "src/mac-data", 0 },
    { "mb1.bin", ARC_MACBINARY, 0, "Read Me", "src/mac-rsrc", 1 },
    { "readme.hqx", ARC_BINHEX, 0, "Read Me", "src/mac-data", 0 },
    { "readme.hqx", ARC_BINHEX, 0, "Read Me", "src/mac-rsrc", 1 },
};

/* One go at an archive, listing (open 0) or opening; the result */
static int run(T *t, const Case *k, const Buf *b, int open, long chunk)
{
    ArcRequest r = req(open, open ? k->member : NULL);
    r.fork = k->fork;
    return k->pulled ? pull(t, k->format, b, &r, NULL) : push(t, k->format, b, chunk, &r, 1, NULL);
}

/* For formats with checksums, an open that says done has the right bytes */
static int checked(int format) { return format == ARC_ZIP || format == ARC_GZIP || format == ARC_BINHEX; }

static unsigned long seed = 1997;
static unsigned long rnd(void) { seed = seed * 1103515245UL + 12345UL; return (seed >> 16) & 0x7FFF; }

static void test_robust(void)
{
    unsigned i;
    for (i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        const Case *k = &CASES[i];
        Buf b = load(k->file), want = load(k->expect), cut;
        T t = { 0 };
        long n, step, allocs;
        int open, res, trial;

        /* cut short everywhere (every length for small files, many for big ones) */
        step = b.n > 3000 ? b.n / 997 + 1 : 1;
        for (n = 0; n < b.n; n += (n > b.n - 700 ? 1 : step)) {
            cut.p = malloc(n ? n : 1);
            memcpy(cut.p, b.p, n);
            cut.n = n;
            for (open = 0; open < 2; open++) {
                res = run(&t, k, &cut, open, 1 + (long)(n % 37));
                CHECK(res <= ARC_DONE && res != ARC_MORE && res != ARC_NO_MEMORY);
                /* an open can't finish from a cut-short archive, unless the cut is past its member */
                if (open && res == ARC_DONE) CHECK(same(&t, &want));
                if (open && res == ARC_DONE && (k->format == ARC_ZIP || k->format == ARC_GZIP)) CHECK(n >= b.n - 8);
                reset(&t);
            }
            free(cut.p);
        }

        /* scrambled: a byte at a time, then a few at once */
        cut.p = malloc(b.n);
        cut.n = b.n;
        for (trial = 0; trial < 1500; trial++) {
            int bytes = trial < 1000 ? 1 : 1 + (int)(rnd() % 8), j;
            memcpy(cut.p, b.p, b.n);
            for (j = 0; j < bytes; j++) {
                long at = trial < 1000 && b.n < 1000 ? trial % b.n : (long)((rnd() << 15 | rnd()) % (unsigned long)b.n);
                cut.p[at] ^= (unsigned char)(1 + rnd() % 255);
            }
            for (open = 0; open < 2; open++) {
                res = run(&t, k, &cut, open, 1 + (long)(rnd() % 600));
                CHECK(res <= ARC_DONE && res != ARC_MORE && res != ARC_NO_MEMORY);
                if (open && res == ARC_DONE && checked(k->format)) CHECK(same(&t, &want));
                CHECK(t.n <= 64);
                reset(&t);
            }
        }
        free(cut.p);

        /* starved: every allocation in turn fails */
        for (open = 0; open < 2; open++) {
            run(&t, k, &b, open, 100);
            allocs = t.allocs;
            reset(&t);
            for (n = 1; n <= allocs; n++) {
                t.fail_alloc = n;
                res = run(&t, k, &b, open, 100);
                CHECK(res == ARC_NO_MEMORY);
                reset(&t);
            }
        }

        /* reads that fail */
        if (k->pulled) {
            long reads;
            for (open = 0; open < 2; open++) {
                run(&t, k, &b, open, 0);
                reads = t.reads;
                reset(&t);
                for (n = 1; n <= reads; n++) {
                    t.fail_read = n;
                    CHECK(run(&t, k, &b, open, 0) == ARC_READ);
                    reset(&t);
                }
            }
        }
        free(b.p);
        free(want.p);
    }
}

int main(void)
{
    test_names();
    test_zip();
    test_tar();
    test_gzip();
    test_mac();
    test_members();
    test_robust();
    if (failures) printf("archive: %d of %d checks failed\n", failures, checks);
    else printf("archive: all passed (%d checks)\n", checks);
    return failures != 0;
}
