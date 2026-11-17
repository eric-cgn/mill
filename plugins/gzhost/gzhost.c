/* gzhost.c: Mill's inflate as the cores' GzCalls (gzhost.h). Each inflate takes two blocks
   from the core's GzMem: Mill's kMillGzMem, and this one, which says where the output goes. */
#include "gzhost.h"

typedef struct {
    GzMem mem;
    gz_out out;
    void *out_ref;
    void *block;
    MillGzRef gz;
    const MillHost *host;
} Inflate;

static void *begin(void *ref, const GzMem *mem, int raw, gz_out out, void *out_ref)
{
    GzHost *g = ref;
    Inflate *z = mem->alloc(mem->ref, sizeof *z);
    if (!z) return 0;
    z->mem = *mem;
    z->out = out;
    z->out_ref = out_ref;
    z->host = g->host;
    z->gz = 0;
    if ((z->block = mem->alloc(mem->ref, kMillGzMem)) != 0)
        z->gz = g->host->GzBegin(g->r, z->block, kMillGzMem, (short)(raw != 0), z);
    if (!z->gz) {
        if (z->block) mem->release(mem->ref, z->block);
        mem->release(mem->ref, z);
        return 0;
    }
    return z;
}

static int data(void *d, const unsigned char *p, long n)
{
    Inflate *z = d;
    return z->host->GzData(z->gz, p, n);
}

static long unused(void *d)
{
    Inflate *z = d;
    return z->host->GzUnused(z->gz);
}

static void end(void *d)
{
    Inflate *z = d;
    GzMem mem;
    if (!z) return;
    z->host->GzEnd(z->gz);
    mem = z->mem;
    mem.release(mem.ref, z->block);
    mem.release(mem.ref, z);
}

void gzhost_init(GzHost *g, const MillHost *host, MillReqRef r)
{
    g->calls.ref = g;
    g->calls.begin = begin;
    g->calls.data = data;
    g->calls.unused = unused;
    g->calls.end = end;
    g->host = host;
    g->r = r;
}

OSErr gzhost_out(MillPlugIn *self, MillGzOutParams *p)
{
    Inflate *z = p->ref;
    (void)self;
    return z->out(z->out_ref, p->data, p->len) ? 1 : noErr;
}
