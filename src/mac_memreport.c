/* A test build's memory report (MILL_MEMREPORT=1 ./build.sh): what Mill's partition
   holds, written to a text file "Mill Memory" next to Mill. Not in normal builds. */
#ifdef MILL_MEMREPORT
#include <Multiverse.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mac_memreport.h"
#include "mac_sys.h"

/* ------------------------------------------------------------ what Mill knows */

#define KNOWN 1200
static struct { const void *p; const char *label; char handle; } known[KNOWN];
static short nknown;

void memreport_note(const void *p, const char *label, int handle)
{
    if (!p || nknown >= KNOWN) return;
    known[nknown].p = p;
    known[nknown].label = label;
    known[nknown].handle = (char)handle;
    nknown++;
}

/* ------------------------------------------------------------ the text */

static char *out;
static long out_len, out_size;

static void say(const char *fmt, ...)
{
    va_list ap;
    long n;
    if (!out || out_len >= out_size - 2) return;
    va_start(ap, fmt);
    n = vsnprintf(out + out_len, out_size - out_len, fmt, ap);
    va_end(ap);
    out_len += n < out_size - out_len ? n : out_size - out_len - 1;
}

static void line(const char *what, long bytes)
{
    long tenths = (bytes * 10 + (bytes < 0 ? -512 : 512)) / 1024;
    say("%-44s %7ld %5ld.%ldK\r", what, bytes, tenths / 10, (tenths < 0 ? -tenths : tenths) % 10);
}

/* ------------------------------------------------------------ the heap */

/* Groups of blocks, by label (labels are the registry's own strings, or fixed ones) */
#define GROUPS 160
static struct { const char *label; long bytes, count; } groups[GROUPS];
static short ngroups;

static void add(const char *label, long bytes)
{
    short i;
    for (i = 0; i < ngroups && groups[i].label != label && strcmp(groups[i].label, label); i++) {}
    if (i == ngroups) {
        if (ngroups == GROUPS) label = "(too many kinds to list)", i = GROUPS - 1;
        else { groups[i].label = label; groups[i].bytes = groups[i].count = 0; ngroups++; }
    }
    groups[i].bytes += bytes;
    groups[i].count++;
}

/* Labels made up as the heap is walked (resources, sizes), kept until the report's done */
#define MADE 160
static char made[MADE][48];
static short nmade;

static const char *make(const char *s)
{
    short i;
    for (i = 0; i < nmade; i++) if (!strcmp(made[i], s)) return made[i];
    if (nmade == MADE) return "(other)";
    strncpy(made[nmade], s, 47);
    made[nmade][47] = 0;
    return made[nmade++];
}

static const char *known_label(Ptr start, long size, int handle, int *exact)
{
    short i;
    const char *inside = NULL;
    for (i = 0; i < nknown; i++) {
        if (known[i].handle != handle) continue;
        if ((Ptr)known[i].p == start) { *exact = 1; return known[i].label; }
        if (!handle && (Ptr)known[i].p > start && (Ptr)known[i].p < start + size && !inside) inside = known[i].label;
    }
    *exact = 0;
    return inside;
}

static long sum_free, sum_nonrel, sum_rel, nfree, nnonrel, nrel;
static Boolean malloc_own_blocks;

/* 24-bit Memory Manager blocks: an 8-byte header, its first long's high byte the tag
   (free 0, nonrelocatable 0x40, relocatable 0x80; low nibble the size correction) and
   its low three bytes the block's size; the second long a relocatable block's master
   pointer, relative to the zone */
static void walk(THz z, char section)
{
    Ptr p = (Ptr)&z->heapData, lim = z->bkLim;
    char buf[64];
    while (p < lim) {
        unsigned long head = *(unsigned long *)p;
        unsigned char tag = (unsigned char)(head >> 24);
        long size = (long)(head & 0x00FFFFFFUL);
        Ptr data = p + 8;
        if (size < 12 || p + size > lim) { say("(heap walk stopped: a bad block at %08lx)\r", (long)p); return; }
        switch (tag >> 6) {
        case 0:
            sum_free += size; nfree++;
            if (section) add("Free", size);
            break;
        case 1: {
            int exact;
            const char *l = known_label(data, size - 8, 0, &exact);
            sum_nonrel += size; nnonrel++;
            if (!section) break;
            if (l && exact) add(l, size);
            else if (l) { snprintf(buf, sizeof buf, "Holds %s, and more", l); add(make(buf), size); }
            else {
                /* master pointer blocks: each long points into the heap, or is 0 or a free link */
                long k, mps = 0, n = (size - 8) / 4;
                for (k = 0; k < n; k++) {
                    unsigned long v = ((unsigned long *)data)[k] & 0x00FFFFFFUL;
                    if (v == 0 || (v >= (unsigned long)&z->heapData && v < (unsigned long)lim)) mps++;
                }
                if (n >= 16 && mps == n && size - 8 == n * 4) add("Master pointers (nonrel)", size);
                else { snprintf(buf, sizeof buf, "Nonrel, other: %ld bytes each", size); add(make(buf), size); }
            }
            break;
        }
        case 2: {
            Handle h = (Handle)((Ptr)z + *(long *)(p + 4));
            unsigned char flags = (unsigned char)(*(unsigned long *)h >> 24);
            int exact;
            const char *l = known_label((Ptr)h, 0, 1, &exact);
            sum_rel += size; nrel++;
            if (!section) break;
            if (l) { add(l, size); break; }
            if (flags & 0x20) {
                short id;
                ResType type;
                Str255 name;
                GetResInfo(h, &id, &type, name);
                if (ResError() == noErr) {
                    snprintf(buf, sizeof buf, "Resource '%.4s' %d \"%.*s\"%s%s", (char *)&type, id,
                             name[0] > 20 ? 20 : name[0], (char *)name + 1, flags & 0x80 ? " locked" : "",
                             flags & 0x40 ? " purgeable" : "");
                    add(make(buf), size);
                    break;
                }
            }
            snprintf(buf, sizeof buf, "Relocatable, other%s%s", flags & 0x80 ? " (locked)" : "", flags & 0x40 ? " (purgeable)" : "");
            add(make(buf), size);
            break;
        }
        default:
            snprintf(buf, sizeof buf, "Tag %02x", tag);
            add(make(buf), size);
        }
        p += size;
    }
}

/* Whether the Memory Manager is in 32-bit mode (System 7 says through Gestalt; the Plus
   and System 6 are always 24-bit) */
static Boolean mode32(void)
{
    long r;
    return Gestalt(gestaltAddressingModeAttr, &r) == noErr && (r & (1L << gestalt32BitAddressing));
}

/* The header of the block that a pointer from malloc is in, if it starts there */
static unsigned long header_before(const char *p)
{
    const unsigned char *volatile q = (const unsigned char *)p - 8;
    return ((unsigned long)q[0] << 24) | ((unsigned long)q[1] << 16) | ((unsigned long)q[2] << 8) | q[3];
}

static int by_bytes(const void *a, const void *b)
{
    long d = ((const long *)b)[1] - ((const long *)a)[1];
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}

/* ------------------------------------------------------------ the file */

static void write_file(Boolean first)
{
    FCBPBRec fcb;
    Str63 app;
    FSSpec spec;
    short ref;
    long n = out_len;
    OSErr err;
    memset(&fcb, 0, sizeof fcb);
    fcb.ioNamePtr = app;
    fcb.ioRefNum = LMGetCurApRefNum();     /* Mill's own file */
    if (PBGetFCBInfoSync(&fcb) != noErr) return;
    file_spec(fcb.ioFCBVRefNum, fcb.ioFCBParID, "\pMill Memory", &spec);
    err = file_create(&spec, 'ttxt', 'TEXT');
    if (err != noErr && err != dupFNErr) return;
    if (file_open(&spec, fsWrPerm, &ref) != noErr) return;
    if (first) SetEOF(ref, 0);
    else SetFPos(ref, fsFromLEOF, 0);
    FSWrite(ref, &n, out);
    FSClose(ref);
    FlushVol(NULL, spec.vRefNum);
}

void memreport_snapshot(const char *when, Boolean first)
{
    THz z = ApplicationZone();
    Ptr zone = (Ptr)z, heap_end = z->bkLim, limit = LMGetApplLimit(), stack_base = LMGetCurStackBase();
    Ptr a5 = LMGetCurrentA5(), buf_ptr = LMGetBufPtr();
    long above = 0, below = 0, here, mp, i, sys;
    Handle code0;
    char sp_mark;
    short k;
    /* the report's own buffer comes first, so the walk sees it too */
    out_size = 12000;
    if (!(out = NewPtr(out_size))) return;
    out_len = 0;
    nknown = 0;
    memreport_note(out, "This report's text (test build)", 0);
    memreport_known();
    {
        /* malloc: one Memory Manager block each, or a pool of its own? */
        char *a = malloc(40), *b = malloc(40);
        malloc_own_blocks = a && b && !mode32() && (header_before(a) >> 30) == 1 && (header_before(a) & 0xFFFFFF) >= 48 &&
                            (header_before(b) >> 30) == 1;
        free(a);
        free(b);
    }
    {
        /* CODE 0 says how big the A5 world is: above A5, then below */
        short was = CurResFile();
        UseResFile(LMGetCurApRefNum());
        code0 = Get1Resource('CODE', 0);
        UseResFile(was);
    }
    if (code0) { above = ((long *)*code0)[0]; below = ((long *)*code0)[1]; }
    say("Mill memory: %s\r", when);
    say("Memory Manager: %s; malloc: %s\r", mode32() ? "32-bit" : "24-bit",
        malloc_own_blocks ? "NewPtr for each (each is its own block)" : "not one block each (a pool)");
    say("\rPartition, low to high (Mill's zone start %08lx to A5 world end %08lx)\r", (long)zone, (long)(a5 + above));
    line("Partition total", a5 + above - zone);
    line("  Application heap (zone to bkLim)", heap_end - zone);
    line("  Heap end to ApplLimit (zone trailer)", limit - heap_end);
    line("  Stack (ApplLimit to CurStackBase)", stack_base - limit);
    here = stack_base - &sp_mark;
    line("    used now", here);
    line("    free", stack_base - limit - here);
    line("  Gap, CurStackBase to globals", (a5 - below) - stack_base);
    line("  A5 world, below A5 (globals, QD globals)", below);
    line("    of which QuickDraw globals", sizeof(QDGlobals));
    line("    of which the report's registry (test build)", sizeof known + sizeof groups + sizeof made);
    line("  A5 world, above A5 (parameters, jump table)", above);
    line("    of which jump table", above - 32);
    say("BufPtr %08lx; A5 world end to BufPtr: %ld bytes (not Mill's)\r", (long)buf_ptr, (long)(buf_ptr - (a5 + above)));
    say("Addresses: zone %08lx, bkLim %08lx, ApplLimit %08lx, CurStackBase %08lx, A5 %08lx; CODE 0: %ld above, %ld below\r",
        (long)zone, (long)heap_end, (long)limit, (long)stack_base, (long)a5, above, below);
    sys = (long)LMGetSysZone()->bkLim - (long)LMGetSysZone();
    say("System heap %ldK (not counted)\r", sys / 1024);
    if (mode32()) {
        say("\rHeap walk skipped: 32-bit zone headers aren't read by this report.\r");
    } else {
        ngroups = 0;
        nmade = 0;
        sum_free = sum_nonrel = sum_rel = nfree = nnonrel = nrel = 0;
        walk(z, 1);
        say("\rApplication heap (%ld blocks)\r", nfree + nnonrel + nrel);
        line("Zone header", (Ptr)&z->heapData - zone);
        say("Free: %ld blocks\r", nfree);
        line("  Free total", sum_free);
        say("Nonrelocatable: %ld blocks\r", nnonrel);
        line("  Nonrelocatable total", sum_nonrel);
        say("Relocatable: %ld blocks\r", nrel);
        line("  Relocatable total", sum_rel);
        mp = (heap_end - zone) - ((Ptr)&z->heapData - zone) - sum_free - sum_nonrel - sum_rel;
        line("Unexplained in the heap", mp);
        /* the groups, largest first */
        {
            long order[GROUPS][2];
            for (i = 0; i < ngroups; i++) { order[i][0] = i; order[i][1] = groups[i].bytes; }
            qsort(order, ngroups, sizeof order[0], by_bytes);
            say("\rBy kind (bytes include each block's 8-byte header)\r");
            for (i = 0; i < ngroups; i++) {
                char what[80];
                k = (short)order[i][0];
                snprintf(what, sizeof what, "%s (%ld)", groups[k].label, groups[k].count);
                line(what, groups[k].bytes);
            }
        }
    }
    say("\r");
    write_file(first);
    DisposePtr(out);
    out = NULL;
}
#endif
