/* What Mill does once, as it starts: reading its settings, finding MacTCP's resolver,
   and finding the Plug-Ins and copying what each says about itself. In the Start code
   segment with the splash (mill.segmap), which is unloaded once the browser is up. */
#include <Multiverse.h>
#include <stdlib.h>
#include <string.h>

#include "mac_local.h"
#include "mac_net.h"
#include "mac_plugin.h"
#include "mac_sys.h"

/* ------------------------------------------------------------ Plug-Ins */

static char scheme_list[128];   /* for url_schemes, which keeps it */

static char *dup(const char *s)
{
    char *c;
    if (!s) s = "";
    if ((c = malloc(strlen(s) + 1))) strcpy(c, s);
    return c;
}

/* Mill's own copy of what millOpen said, as the Plug-In's code goes after */
static Boolean keep_info(Plug *p, const MillInfo *in)
{
    short i, k;
    p->name = dup(in->name);
    p->version = dup(in->version);
    p->copyright = dup(in->copyright);
    p->by = dup(in->by);
    p->description = dup(in->description);
    p->flags = in->flags;
    p->nproto = in->protocolCount > 0 ? in->protocolCount : 0;
    if (!p->name || !p->version || !p->copyright || !p->by || !p->description) return false;
    if (strlen(p->name) > 31) p->name[31] = 0;
    if (p->nproto && !(p->protos = calloc(p->nproto, sizeof(PlugProto)))) return false;
    for (i = 0; i < p->nproto; i++) {
        PlugProto *pp = &p->protos[i];
        const MillProtocol *mp = &in->protocols[i];
        char *c;
        pp->plug = p;
        if (!(pp->scheme = dup(mp->scheme)) || !(pp->heading = dup(mp->heading)) || !(pp->description = dup(mp->description)))
            return false;
        for (c = pp->scheme; *c; c++) if (*c >= 'A' && *c <= 'Z') *c |= 32;
        pp->nset = mp->settingCount > 0 ? mp->settingCount : 0;
        if (pp->nset && !(pp->sets = calloc(pp->nset, sizeof(MillSetting)))) return false;
        for (k = 0; k < pp->nset; k++) {
            const MillSetting *ms = &mp->settings[k];
            MillSetting *s = &pp->sets[k];
            *s = *ms;
            if (!(s->key = dup(ms->key)) || !(s->label = dup(ms->label)) || !(s->dflt = dup(ms->dflt))) return false;
            s->note = ms->note ? dup(ms->note) : NULL;
            s->choices = ms->choices ? dup(ms->choices) : NULL;
        }
    }
    p->nformat = in->formatCount > 0 ? in->formatCount : 0;
    if (p->nformat && !(p->formats = calloc(p->nformat, sizeof(PlugFormat)))) return false;
    for (i = 0; i < p->nformat; i++) {
        PlugFormat *pf = &p->formats[i];
        const MillFormat *mf = &in->formats[i];
        pf->plug = p;
        pf->gives = mf->gives;
        if (!(pf->key = dup(mf->key)) || !(pf->heading = dup(mf->heading)) || !(pf->description = dup(mf->description)) ||
            !(pf->types = dup(mf->types)) || !(pf->extensions = dup(mf->extensions)) || !(pf->fileTypes = dup(mf->fileTypes)))
            return false;
        pf->nset = mf->settingCount > 0 ? mf->settingCount : 0;
        if (pf->nset && !(pf->sets = calloc(pf->nset, sizeof(MillSetting)))) return false;
        for (k = 0; k < pf->nset; k++) {
            const MillSetting *ms = &mf->settings[k];
            MillSetting *s = &pf->sets[k];
            *s = *ms;
            if (!(s->key = dup(ms->key)) || !(s->label = dup(ms->label)) || !(s->dflt = dup(ms->dflt))) return false;
            s->note = ms->note ? dup(ms->note) : NULL;
            s->choices = ms->choices ? dup(ms->choices) : NULL;
        }
    }
    return true;
}

static Plug *add_plug(void)
{
    Plug *more = realloc(plugs, (nplugs + 1) * sizeof(Plug));
    if (!more) return NULL;
    plugs = more;
    memset(&plugs[nplugs], 0, sizeof(Plug));
    return &plugs[nplugs++];
}

/* Open it once for its MillInfo, then close it again */
static void read_plug(Plug *p)
{
    MillOpenParams op;
    short i, k;
    if (!plugin_load(p, &op)) {
        p->status = op.interface > kMillInterface ? PLUG_NEWER : PLUG_FAILED;
        return;
    }
    if (!keep_info(p, &op.info)) p->status = PLUG_FAILED;
    plugin_close(p);
    if (p->status != PLUG_READY) return;
    /* a name or a scheme another already has: the first one keeps it */
    for (i = 0; i < nplugs && &plugs[i] != p; i++) {
        if (plugs[i].status != PLUG_READY) continue;
        if (!strcmp(plugs[i].name, p->name)) p->status = PLUG_CONFLICT;
        for (k = 0; k < p->nproto; k++) {
            short j;
            for (j = 0; j < plugs[i].nproto; j++) if (!strcmp(plugs[i].protos[j].scheme, p->protos[k].scheme)) p->status = PLUG_CONFLICT;
        }
    }
}

/* The folder Mill is in, from its open file */
static OSErr app_folder(short *vref, long *dir)
{
    FCBPBRec fcb;
    Str63 name;
    OSErr err;
    memset(&fcb, 0, sizeof fcb);
    fcb.ioNamePtr = name;
    fcb.ioRefNum = CurResFile();
    if ((err = PBGetFCBInfoSync(&fcb)) != noErr) return err;
    *vref = fcb.ioFCBVRefNum;
    *dir = fcb.ioFCBParID;
    return noErr;
}

/* ------------------------------------------------------------ Web Fonts */

/* Optional: font suitcases ('FFIL') in the Plug-In folder, read-only and kept open for the
   session. Mill runs the same without them. The font settings list each family in them that
   the Font Manager finds, with the System's. */
#define kTimesFont 20
PackFam pack_fams[PACK_FAMS];
short npack_fams;
PackFile pack_files[PACK_FILES];
short npack_files;
#ifdef MILL_MEMREPORT
Handle fonts_map;
#endif
static const short pack_sizes[7] = { 9, 10, 12, 14, 18, 24, 36 };

/* Whether a FONT resource is a sane strike (Inside Macintosh: Text, "Font Resources"): a
   damaged one can hang the Font Manager, so what's in it is checked before it's trusted.
   Every table has to lie inside the resource, and the glyph locations must rise. */
static Boolean font_sane(Handle h)
{
    const unsigned char *p;
    long size = GetHandleSize(h), n, i, loc_at, ow_at, bits;
    short first, last, rect_h, asc, desc, row_words, ow_loc;
    unsigned short type, prev, v;
    if (size < 26 + 4 * 2) return false;
    p = (const unsigned char *)*h;
#define W(o) ((short)((p[o] << 8) | p[(o) + 1]))
    type = (unsigned short)W(0);
    first = W(2);
    last = W(4);
    rect_h = W(14);
    ow_loc = W(16);
    asc = W(18);
    desc = W(20);
    row_words = W(24);
    if (type != 0x9000 && type != 0xB000) return false;
    if (first < 0 || last > 255 || first >= last) return false;
    if (asc < 0 || desc < 0 || rect_h < 1 || rect_h > 255 || asc + desc != rect_h) return false;
    if (row_words < 1 || row_words > 1024) return false;
    n = (long)last - first + 3;                     /* the characters, the missing glyph, and the end */
    loc_at = 26 + (long)row_words * 2 * rect_h;     /* the strike, then the locations... */
    ow_at = 16 + (long)ow_loc * 2;                  /* ...then the offsets and widths */
    if (ow_loc < 0 || ow_at != loc_at + n * 2 || ow_at + n * 2 > size) return false;
    bits = (long)row_words * 16;
    for (prev = 0, i = 0; i < n; i++) {
        v = (unsigned short)W(loc_at + i * 2);
        if (v < prev || v > bits) return false;
        prev = v;
    }
#undef W
    return true;
}

/* A family's FOND with nothing the Font Manager would have to chase outside it, and every
   FONT its association table names there and sound (the current resource file is the
   suitcase). Returns the sizes it has plain strikes for (a bit each, as PackFont says), or 0. */
static short family_sizes(Handle fond, short family)
{
    const unsigned char *p = (const unsigned char *)*fond;
    long size = GetHandleSize(fond), count, i;
    short mask = 0, k;
    if (size < 54 + 6) return 0;
#define W(o) ((short)((p[o] << 8) | p[(o) + 1]))
#define L(o) (((long)p[o] << 24) | ((long)p[(o) + 1] << 16) | ((long)p[(o) + 2] << 8) | p[(o) + 3])
    if (W(2) != family) return 0;
    if ((L(16) != 0 && (L(16) < 0 || L(16) >= size)) || (L(20) != 0 && (L(20) < 0 || L(20) >= size)) ||
        (L(24) != 0 && (L(24) < 0 || L(24) >= size)))
        return 0;
    count = (long)W(52) + 1;
    if (count < 1 || count > 64 || 54 + count * 6 > size) return 0;
    for (i = 0; i < count; i++) {
        short strike_size = W(54 + i * 6), style = W(56 + i * 6), id = W(58 + i * 6);
        Handle f;
        Boolean ok;
        if (style < 0 || style > 3 || id < 0) return 0;
        if (!(f = Get1Resource('FONT', id))) return 0;
        ok = font_sane(f);
        ReleaseResource(f);
        if (!ok) return 0;
        p = (const unsigned char *)*fond;           /* (loading may have moved it) */
        if (style == 0)
            for (k = 0; k < 7; k++) if (pack_sizes[k] == strike_size) mask |= 1 << k;
    }
#undef W
#undef L
    return mask;
}

static void open_fonts(const FSSpec *spec, ConstStr255Param name)
{
    short was = CurResFile(), ref, i, k, n, first = npack_fams;
    if (npack_files >= PACK_FILES || RealFont(kTimesFont, 36)) return;   /* the System's own is better */
    ref = file_open_res(spec, fsRdPerm);
    if (ref == -1) { UseResFile(was); return; }
    /* it's on top of the chain and current. Each family's FOND and FONTs are checked before
       the Font Manager sees them, then the Font Manager is asked for each size, while the
       suitcase is current. */
    if (ResError() == noErr) {
        n = Count1Resources('FOND');
        for (i = 1; i <= n && npack_fams < PACK_FAMS; i++) {
            Handle d = Get1IndResource('FOND', i);
            short id, mask, j;
            ResType t;
            Str255 nm;
            if (!d) continue;
            GetResInfo(d, &id, &t, nm);
            for (j = 0; j < npack_fams; j++) if (pack_fams[j].id == id) break;   /* an ID already taken */
            if (j < npack_fams || nm[0] == 0 || nm[0] >= sizeof pack_fams[0].name) { ReleaseResource(d); continue; }
            HLock(d);
            mask = family_sizes(d, id);
            HUnlock(d);
            for (k = 0; k < 7; k++)
                if ((mask & (1 << k)) && !RealFont(id, pack_sizes[k])) mask &= ~(1 << k);
            if (mask) {
                PackFam *f = &pack_fams[npack_fams++];
                f->id = id;
                f->ref = ref;
                f->sizes = mask;
                f->fixed = -1;
                memcpy(f->name, nm + 1, nm[0]);
                f->name[nm[0]] = 0;
            }
            ReleaseResource(d);
        }
    }
    UseResFile(was);
    if (npack_fams == first) { CloseResFile(ref); UseResFile(was); return; }
    pack_files[npack_files].ref = ref;
    if (name[0] < sizeof pack_files[0].file) { memcpy(pack_files[npack_files].file, name + 1, name[0]); pack_files[npack_files].file[name[0]] = 0; }
    npack_files++;
#ifdef MILL_MEMREPORT
    if (!fonts_map) fonts_map = (Handle)LMGetTopMapHndl();
#endif
}

static void read_folder(Boolean skip)
{
    short vref, i;
    long dir;
    CInfoPBRec pb;
    Str63 name;
    if (app_folder(&vref, &dir) != noErr) return;
    memset(&pb, 0, sizeof pb);
    pb.dirInfo.ioNamePtr = (StringPtr)"\pMill Plug-Ins";
    pb.dirInfo.ioVRefNum = vref;
    pb.dirInfo.ioDrDirID = dir;
    if (PBGetCatInfoSync(&pb) != noErr || !(pb.dirInfo.ioFlAttrib & 0x10)) return;
    dir = pb.dirInfo.ioDrDirID;
    for (i = 1; ; i++) {
        Plug *p;
        memset(&pb, 0, sizeof pb);
        pb.hFileInfo.ioNamePtr = name;
        pb.hFileInfo.ioVRefNum = vref;
        pb.hFileInfo.ioDirID = dir;
        pb.hFileInfo.ioFDirIndex = i;
        if (PBGetCatInfoSync(&pb) != noErr) break;
        if (pb.hFileInfo.ioFlAttrib & 0x10) continue;
        if (pb.hFileInfo.ioFlFndrInfo.fdType == 'FFIL') {
            /* not a Plug-In, and not listed: Web Fonts */
            FSSpec fs;
            if (!skip && file_spec(vref, dir, name, &fs) == noErr) open_fonts(&fs, name);
            continue;
        }
        if (!(p = add_plug())) break;
        memcpy(p->file, name + 1, name[0]);
        p->file[name[0]] = 0;
        if (file_spec(vref, dir, name, &p->spec) != noErr) { p->status = PLUG_FAILED; continue; }
        if (skip) { p->status = PLUG_SKIPPED; continue; }
        read_plug(p);
        if (p->status != PLUG_READY && !p->name) p->name = dup(p->file);
    }
}

void plugins_init(void)
{
    short i, k;
    unsigned char keys[16];
    plugin_host_init();
    for (i = 1; i <= BUILTINS; i++) {
        Plug *p = add_plug();
        if (!p) break;
        p->builtin = i;
        read_plug(p);
    }
    GetKeys(keys);
    read_folder((keys[7] & 1) != 0);    /* Shift (key code 56) */
    /* the array has moved as it grew: each protocol points at its Plug-In again */
    for (i = 0; i < nplugs; i++) {
        for (k = 0; k < plugs[i].nproto; k++) plugs[i].protos[k].plug = &plugs[i];
        for (k = 0; k < plugs[i].nformat; k++) plugs[i].formats[k].plug = &plugs[i];
    }
    /* the schemes for url.c: all but file:, which it knows already */
    strcpy(scheme_list, "plugin");
    for (i = 0; i < nplugs; i++) {
        if (plugs[i].status != PLUG_READY) continue;
        for (k = 0; k < plugs[i].nproto; k++) {
            const char *s = plugs[i].protos[k].scheme;
            if (!strcmp(s, "file") || !strcmp(s, "mill") || !strcmp(s, "http") || !strcmp(s, "https")) continue;
            if (strlen(scheme_list) + strlen(s) + 2 > sizeof scheme_list) continue;
            if (scheme_list[0]) strcat(scheme_list, " ");
            strcat(scheme_list, s);
        }
    }
    url_schemes(scheme_list);
}

/* ------------------------------------------------------------ settings */

/* One name=value on each line; lines starting with # are notes */
void settings_load(void)
{
    FSSpec spec;
    short ref;
    long len;
    char *text, *p, *end;
    if (settings_spec(&spec) != noErr || file_open(&spec, fsRdPerm, &ref) != noErr) return;
    GetEOF(ref, &len);
    if (len > 16384) len = 16384;
    if ((text = malloc(len + 1)) && FSRead(ref, &len, text) == noErr) {
        text[len] = 0;
        for (p = text; *p; p = end + (*end != 0)) {
            char *eq;
            long n;
            for (end = p; *end && *end != '\r' && *end != '\n'; end++) {}
            n = end - p;
            while (n > 0 && *p == ' ') { p++; n--; }
            if (n == 0 || *p == '#' || !(eq = memchr(p, '=', n))) continue;
            {
                long nl = eq - p, vs = nl + 1;
                while (nl > 0 && p[nl - 1] == ' ') nl--;
                while (vs < n && p[vs] == ' ') vs++;
                if (nl) put_setting(p, nl, p + vs, n - vs);
            }
        }
    }
    free(text);
    FSClose(ref);
    /* Mill 1.x's "Enable https over proxy" (proxy_https) becomes HTTPS's Enable, which
       now means the same: set it only if HTTPS's own isn't saved yet */
    if (!setting("https.enable", NULL) && setting("proxy_https", NULL))
        setting_put("https.enable", strcmp(setting("proxy_https", "no"), "yes") ? "no" : "yes");
}

/* ------------------------------------------------------------ the system */

#define GESTALT_TRAP 0xA1AD
#define FIND_FOLDER_PRESENT 1

static Boolean trap_available(unsigned short trap, TrapType type)
{
    return NGetTrapAddress(trap, type) != NGetTrapAddress(_Unimplemented, kToolboxTrapType);
}

void sys_init(void)
{
    long r;
    sys_wne = trap_available(_WaitNextEvent, kToolboxTrapType);
    if (!trap_available(GESTALT_TRAP, kOSTrapType)) return;  /* before 6.0.4: none of the rest */
    sys_fsspec = Gestalt(gestaltFSAttr, &r) == noErr && (r & gestaltHasFSSpecCalls);
    sys_new_sf = Gestalt(gestaltStandardFileAttr, &r) == noErr && (r & gestaltStandardFile58);
    sys_find_folder = Gestalt(gestaltFindFolderAttr, &r) == noErr && (r & FIND_FOLDER_PRESENT);
}

/* ------------------------------------------------------------ MacTCP */

enum { OPENRESOLVER = 1 };
typedef OSErr (*dnr_proc)(long selector, ...);

/* MacTCP's resolver lives in a 'dnrp' resource: in the MacTCP control panel or in a
   "MacTCP DNR" file, in the System Folder, Control Panels or Extensions. */
static Boolean is_resolver_file(const HFileInfo *pb)
{
    OSType type = pb->ioFlFndrInfo.fdType, creator = pb->ioFlFndrInfo.fdCreator;
    if (type == 'cdev' && (creator == 'ztcp' || creator == 'mtcp')) return true;
    return pb->ioNamePtr[0] == 10 && memcmp(pb->ioNamePtr + 1, "MacTCP DNR", 10) == 0;
}

static Handle load_dnrp_from(OSType folder)
{
    short vref; long dir; short i;
    Str63 name;
    HFileInfo pb;
    if (sys_folder(folder, &vref, &dir) != noErr) return NULL;
    for (i = 1; ; i++) {
        memset(&pb, 0, sizeof pb);
        pb.ioNamePtr = name;
        pb.ioVRefNum = vref;
        pb.ioDirID = dir;
        pb.ioFDirIndex = i;
        if (PBGetCatInfoSync((CInfoPBPtr)&pb) != noErr) return NULL;
        if ((pb.ioFlAttrib & ioDirMask) == 0 && is_resolver_file(&pb)) {
            FSSpec spec;
            short ref;
            Handle h;
            if (file_spec(vref, dir, name, &spec) != noErr) continue;
            ref = file_open_res(&spec, fsRdPerm);
            if (ref == -1) continue;
            h = Get1IndResource('dnrp', 1);
            if (h) { DetachResource(h); MoveHHi(h); HLock(h); }
            CloseResFile(ref);
            if (h) return h;
        }
    }
}

OSErr net_init(void)
{
    OSErr err = OpenDriver("\p.IPP", &net_ipp);
    if (err != noErr) return err;
    net_dnr = load_dnrp_from(kControlPanelFolderType);
    if (!net_dnr) net_dnr = load_dnrp_from(kSystemFolderType);
    if (!net_dnr) net_dnr = load_dnrp_from(kExtensionFolderType);
    if (net_dnr && ((dnr_proc)*net_dnr)(OPENRESOLVER, (char *)NULL) != noErr) {
        DisposeHandle(net_dnr);
        net_dnr = NULL;
    }
    return noErr;
}
