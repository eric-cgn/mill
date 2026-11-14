/* Install Mill 2.0: a welcome window, then the install window (Easy or Custom). The files are gzip-compressed
   'MBIN' resources of this program, each a MacBinary file named (by the resource's name) for
   where it goes inside the folder: "Mill", "Mill Plug-Ins:Web Fonts". Plain File Manager calls,
   so it runs on System 6. */
#include <Multiverse.h>
#include <string.h>
#include <stdlib.h>

#include "../src/core/inflate.h"

enum { W1 = 128, W2 = 129 };
/* the welcome window's items, and the install window's (the order of their DITLs) */
enum { W1_CONT = 1, W1_QUIT, W1_RING, W1_TITLE, W1_TEXT };
enum { W2_INSTALL = 1, W2_QUIT, W2_RING, W2_POPUP, W2_DESC, W2_HEAD_APP, W2_MILL, W2_HEAD_PLUG, W2_INET, W2_FONTS,
       W2_HEAD_DEV, W2_SDK, W2_LOC, W2_CHOOSE, W2_FOLDER, W2_DISK, W2_AVAIL, W2_NEED, W2_STATUS };
enum { ID_MILL = 128, ID_INET = 129, ID_FONTS = 130, ID_SDK = 1000 };

/* a file being written, from the MacBinary stream that comes out of inflate */
typedef struct {
    short vref;
    long dir;
    Str255 name;
    unsigned char head[128];
    long dlen, rlen;
    long at;                /* where in the stream the next piece begins */
    short df, rf;
    char opened;
    OSErr err;
} Out;

static void pstr(Str255 out, const char *s)
{
    out[0] = (unsigned char)strlen(s);
    memcpy(out + 1, s, out[0]);
}

static short place_vref;    /* the install folder: volume and directory */
static long place_dir;

static OSErr dir_of(short vref, long parent, ConstStr255Param name, long *dir)
{
    HParamBlockRec h;
    CInfoPBRec c;
    OSErr err;
    memset(&h, 0, sizeof h);
    h.fileParam.ioNamePtr = (StringPtr)name;
    h.fileParam.ioVRefNum = vref;
    h.fileParam.ioDirID = parent;
    err = PBDirCreateSync(&h);
    if (err == noErr) { *dir = h.fileParam.ioDirID; return noErr; }
    if (err != dupFNErr) return err;
    memset(&c, 0, sizeof c);
    c.dirInfo.ioNamePtr = (StringPtr)name;
    c.dirInfo.ioVRefNum = vref;
    c.dirInfo.ioDrDirID = parent;
    if ((err = PBGetCatInfoSync(&c)) != noErr) return err;
    if (!(c.dirInfo.ioFlAttrib & 16)) return dupFNErr;      /* a file in the way */
    *dir = c.dirInfo.ioDrDirID;
    return noErr;
}

static Boolean has_file(short vref, long dir, ConstStr255Param name)
{
    CInfoPBRec c;
    memset(&c, 0, sizeof c);
    c.hFileInfo.ioNamePtr = (StringPtr)name;
    c.hFileInfo.ioVRefNum = vref;
    c.hFileInfo.ioDirID = dir;
    return PBGetCatInfoSync(&c) == noErr && !(c.hFileInfo.ioFlAttrib & 16);
}

static void remove_file(short vref, long dir, ConstStr255Param name)
{
    HParamBlockRec h;
    memset(&h, 0, sizeof h);
    h.fileParam.ioNamePtr = (StringPtr)name;
    h.fileParam.ioVRefNum = vref;
    h.fileParam.ioDirID = dir;
    PBHDeleteSync(&h);
}

static OSErr put(short ref, const unsigned char *p, long n)
{
    ParamBlockRec pb;
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioRefNum = ref;
    pb.ioParam.ioBuffer = (Ptr)p;
    pb.ioParam.ioReqCount = n;
    pb.ioParam.ioPosMode = fsAtMark;
    return PBWriteSync(&pb);
}

static OSErr open_fork(short vref, long dir, ConstStr255Param name, Boolean rsrc, short *ref)
{
    HParamBlockRec h;
    OSErr err;
    memset(&h, 0, sizeof h);
    h.fileParam.ioNamePtr = (StringPtr)name;
    h.fileParam.ioVRefNum = vref;
    h.fileParam.ioDirID = dir;
    h.ioParam.ioPermssn = fsWrPerm;
    err = rsrc ? PBHOpenRFSync(&h) : PBHOpenSync(&h);
    *ref = h.ioParam.ioRefNum;
    return err;
}

static void close_fork(short ref)
{
    ParamBlockRec pb;
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioRefNum = ref;
    PBCloseSync(&pb);
}

static long be32(const unsigned char *p) { return ((long)p[0] << 24) | ((long)p[1] << 16) | ((long)p[2] << 8) | p[3]; }

/* The file: created (a file of that name is replaced) once the 128-byte header is in */
static OSErr begin_file(Out *o)
{
    HParamBlockRec h;
    OSErr err;
    remove_file(o->vref, o->dir, o->name);
    memset(&h, 0, sizeof h);
    h.fileParam.ioNamePtr = o->name;
    h.fileParam.ioVRefNum = o->vref;
    h.fileParam.ioDirID = o->dir;
    if ((err = PBHCreateSync(&h)) != noErr) return err;
    o->dlen = be32(o->head + 83);
    o->rlen = be32(o->head + 87);
    o->df = o->rf = 0;
    if (o->dlen && (err = open_fork(o->vref, o->dir, o->name, false, &o->df)) != noErr) return err;
    if (o->rlen && (err = open_fork(o->vref, o->dir, o->name, true, &o->rf)) != noErr) return err;
    o->opened = 1;
    return noErr;
}

static void end_file(Out *o)
{
    HParamBlockRec h;
    if (o->df) close_fork(o->df);
    if (o->rf) close_fork(o->rf);
    o->df = o->rf = 0;
    if (!o->opened) return;
    /* type, creator, and Finder flags from the header; "inited" off so the Finder places it */
    memset(&h, 0, sizeof h);
    h.fileParam.ioNamePtr = o->name;
    h.fileParam.ioVRefNum = o->vref;
    h.fileParam.ioDirID = o->dir;
    if (PBHGetFInfoSync(&h) == noErr) {
        h.fileParam.ioDirID = o->dir;
        memcpy(&h.fileParam.ioFlFndrInfo.fdType, o->head + 65, 4);
        memcpy(&h.fileParam.ioFlFndrInfo.fdCreator, o->head + 69, 4);
        h.fileParam.ioFlFndrInfo.fdFlags = (short)((((o->head[73] << 8) | o->head[101])) & ~0x0100);
        h.fileParam.ioFlFndrInfo.fdLocation.h = h.fileParam.ioFlFndrInfo.fdLocation.v = 0;
        h.fileParam.ioFlFndrInfo.fdFldr = 0;
        PBHSetFInfoSync(&h);
    }
}

/* inflate's output: the MacBinary file, data fork then resource fork, each from the next 128 bytes */
static int sink(void *ref, const unsigned char *p, long n)
{
    Out *o = ref;
    long at = o->at, end, i;
    if (o->err) return 1;
    if (n <= 0) return 0;
    end = at + n;
    for (i = at; i < 128 && i < end; i++) o->head[i] = p[i - at];
    if (end >= 128 && !o->opened && (o->err = begin_file(o)) != noErr) return 1;
    if (o->opened) {
        long ds = 128, de = 128 + o->dlen, rs = 128 + ((o->dlen + 127) / 128) * 128, re = rs + o->rlen;
        long a = at > ds ? at : ds, b = end < de ? end : de;
        if (o->df && b > a && (o->err = put(o->df, p + (a - at), b - a)) != noErr) return 1;
        a = at > rs ? at : rs; b = end < re ? end : re;
        if (o->rf && b > a && (o->err = put(o->rf, p + (a - at), b - a)) != noErr) return 1;
    }
    o->at = end;
    return 0;
}

/* One payload resource into the install folder, at the path its name gives */
static OSErr install_one(Handle res)
{
    short id;
    ResType t;
    Str255 path, comp;
    long dir = place_dir, len, i = 1, start;
    OSErr err = noErr;
    Out o;
    void *z;
    int r;
    GetResInfo(res, &id, &t, path);
    memset(&o, 0, sizeof o);
    o.vref = place_vref;
    for (;;) {
        start = i;
        while (i <= path[0] && path[i] != ':') i++;
        len = i - start;
        comp[0] = (unsigned char)len;
        memcpy(comp + 1, path + start, len);
        if (i > path[0]) break;                 /* the last part is the file */
        if ((err = dir_of(place_vref, dir, comp, &dir)) != noErr) return err;
        i++;
    }
    memcpy(o.name, comp, comp[0] + 1);
    o.dir = dir;
    if (!(z = gz_begin(sink, &o))) return memFullErr;
    HLock(res);
    r = gz_data(z, (unsigned char *)*res, GetHandleSize(res));
    HUnlock(res);
    gz_end(z);
    end_file(&o);
    if (o.err) return o.err;
    return r < 0 ? paramErr : noErr;
}

/* ---- the windows ---- */

static void set_text(DialogPtr d, short item, const char *text)
{
    short type;
    Handle h;
    Rect r;
    Str255 s;
    GetDialogItem(d, item, &type, &h, &r);
    s[0] = (unsigned char)strlen(text);
    memcpy(s + 1, text, s[0]);
    SetDialogItemText(h, s);
    EraseRect(&r);
    DrawDialog(d);
}

static void set_pstr(DialogPtr d, short item, const char *label, ConstStr255Param name)
{
    char buf[200];
    short n = (short)strlen(label);
    memcpy(buf, label, n);
    memcpy(buf + n, name + 1, name[0] > 150 ? 150 : name[0]);
    buf[n + (name[0] > 150 ? 150 : name[0])] = 0;
    set_text(d, item, buf);
}

static void ring(DialogPtr d, short item)
{
    short type;
    Handle h;
    Rect r;
    PenState ps;
    GetDialogItem(d, item, &type, &h, &r);
    GetPenState(&ps);
    PenSize(3, 3);
    InsetRect(&r, -4, -4);
    FrameRoundRect(&r, 16, 16);
    SetPenState(&ps);
}

static pascal void draw_ring1(DialogPtr d, short item) { (void)item; ring(d, W1_CONT); }
static pascal void draw_ring2(DialogPtr d, short item) { (void)item; ring(d, W2_INSTALL); }

static void set_check(DialogPtr d, short item, short on)
{
    short type;
    Handle h;
    Rect r;
    GetDialogItem(d, item, &type, &h, &r);
    SetControlValue((ControlHandle)h, on);
}

static short get_check(DialogPtr d, short item)
{
    short type;
    Handle h;
    Rect r;
    GetDialogItem(d, item, &type, &h, &r);
    return GetControlValue((ControlHandle)h);
}

static void alert_text(const char *text, OSErr err)
{
    Str255 a, num;
    a[0] = (unsigned char)strlen(text);
    memcpy(a + 1, text, a[0]);
    if (err) {
        NumToString(err, num);
        if (a[0] + 1 + num[0] < 255) { a[++a[0]] = ' '; memcpy(a + a[0] + 1, num + 1, num[0]); a[0] += num[0]; }
    }
    ParamText(a, "\p", "\p", "\p");
    StopAlert(140, NULL);
}

/* ---- window 1: the welcome, with a text box that scrolls ---- */

static TEHandle w1_te;
static ControlHandle w1_bar;
static Rect w1_box, w1_view;
static short w1_line, w1_top, w1_max;       /* line height, lines scrolled, most lines to scroll */

static void w1_scroll(short to)
{
    if (to < 0) to = 0;
    if (to > w1_max) to = w1_max;
    if (to == w1_top) return;
    TEScroll(0, (w1_top - to) * w1_line, w1_te);
    w1_top = to;
    SetControlValue(w1_bar, to);
}

static pascal void w1_track(ControlHandle c, short part)
{
    short page = (w1_view.bottom - w1_view.top) / w1_line - 1;
    (void)c;
    if (part == inUpButton) w1_scroll(w1_top - 1);
    else if (part == inDownButton) w1_scroll(w1_top + 1);
    else if (part == inPageUp) w1_scroll(w1_top - page);
    else if (part == inPageDown) w1_scroll(w1_top + page);
}

static pascal void w1_draw(DialogPtr d, short item)
{
    (void)item;
    FrameRect(&w1_box);
    TEUpdate(&w1_view, w1_te);
    DrawControls(d);
}

static pascal Boolean w1_filter(DialogPtr d, EventRecord *e, short *item)
{
    if (e->what == keyDown || e->what == autoKey) {
        char c = e->message & charCodeMask;
        if (c == 13 || c == 3) { *item = W1_CONT; return true; }
    }
    if (e->what == mouseDown) {
        Point p = e->where;
        ControlHandle c;
        short part;
        SetPort(d);
        GlobalToLocal(&p);
        part = FindControl(p, d, &c);
        if (part && c == w1_bar) {
            if (part == inThumb) { TrackControl(c, p, NULL); w1_scroll(GetControlValue(c)); }
            else TrackControl(c, p, (ControlActionUPP)w1_track);
            *item = W1_TITLE;
            return true;
        }
    }
    return false;
}

static const char WELCOME[] =
    "Mill is a web browser for monochrome Macs. It reads HTML 3.2, and shows pictures, tables, and forms.\r\r"
    "This installs Mill, its Internet Services Plug-In (Finger, Whois, Dict, Time, FTP, and Gopher), and Web Fonts. "
    "Custom Install also has the Mill Plug-In SDK.\r\r"
    "Mill needs a Mac Plus or later, System 6.0.3 with MacTCP or later, and 700K of free memory. "
    "About 500K of disk space holds everything.\r\r"
    "Freeware from OneInAMillion Software.";

static Boolean welcome(void)
{
    DialogPtr d;
    short item, type, lines;
    Handle h;
    Rect r;
    Boolean go = false;
    if (!(d = GetNewDialog(W1, NULL, (WindowPtr)-1))) return false;
    GetDialogItem(d, W1_CONT, &type, &h, &r);
    GetDialogItem(d, W1_RING, &type, &h, &r);
    SetDialogItem(d, W1_RING, type, (Handle)(UserItemUPP)draw_ring1, &r);
    GetDialogItem(d, W1_TEXT, &type, &h, &w1_box);
    SetDialogItem(d, W1_TEXT, type, (Handle)(UserItemUPP)w1_draw, &w1_box);
    SetPort(d);
    TextFont(kFontIDGeneva);
    TextSize(10);
    w1_view = w1_box;
    w1_view.right -= 16;
    InsetRect(&w1_view, 5, 4);
    w1_te = TENew(&w1_view, &w1_view);
    TESetText((Ptr)WELCOME, strlen(WELCOME), w1_te);
    w1_line = (*w1_te)->lineHeight;
    lines = (*w1_te)->nLines;
    w1_max = lines - (w1_view.bottom - w1_view.top) / w1_line;
    if (w1_max < 0) w1_max = 0;
    w1_top = 0;
    SetRect(&r, w1_box.right - 16, w1_box.top, w1_box.right, w1_box.bottom);
    r.top--; r.bottom++; r.right++;
    w1_bar = NewControl(d, &r, "\p", true, 0, 0, w1_max, scrollBarProc, 0);
    HiliteControl(w1_bar, w1_max ? 0 : 255);
    TextFont(systemFont);
    TextSize(12);
    ShowWindow(d);
    for (;;) {
        ModalDialog((ModalFilterUPP)w1_filter, &item);
        if (item == W1_CONT) { go = true; break; }
        if (item == W1_QUIT) break;
    }
    TEDispose(w1_te);
    DisposeDialog(d);
    return go;
}

/* ---- window 2: what to install, and where ---- */

static short place_vol;             /* where the install folder is: a volume, the folder it's in, and its name; */
static long place_parent, place_found;   /* or an existing install's own folder (place_found, else 0) */
static Str255 place_name, place_vol_name;
static Boolean custom;
static long size_of[4];             /* Mill, Internet Services, Web Fonts, SDK: bytes */
static MenuHandle popup;

static const char *const DESC[5] = {
    "Mill, and its standard Plug-Ins: Internet Services and Web Fonts.",     /* Easy */
    "The web browser.",
    "Finger, Whois, Dict, Time, FTP, and Gopher.",
    "Fonts for pages that name them.",
    "Files for writing Mill Plug-Ins."
};

static void number_k(char *out, long k)
{
    Str255 s;
    NumToString(k, s);
    memcpy(out, s + 1, s[0]);
    out[s[0]] = 'K';
    out[s[0] + 1] = 0;
}

static void show_space(DialogPtr d)
{
    char line[80];
    HParamBlockRec pb;
    Str255 vn;
    long need = 0, free_k = 0;
    short i;
    static const short CHK[3] = { W2_MILL, W2_INET, W2_FONTS };
    for (i = 0; i < 3; i++) if (!custom || get_check(d, CHK[i])) need += size_of[i];
    if (custom && get_check(d, W2_SDK)) need += size_of[3];
    memset(&pb, 0, sizeof pb);
    pb.volumeParam.ioNamePtr = vn;
    pb.volumeParam.ioVRefNum = place_vol;
    if (PBHGetVInfoSync(&pb) == noErr) {
        free_k = ((long)(unsigned short)pb.volumeParam.ioVFrBlk * (pb.volumeParam.ioVAlBlkSiz / 512)) / 2;
        memcpy(place_vol_name, vn, vn[0] + 1);
    }
    strcpy(line, "Disk space available: ");
    number_k(line + strlen(line), free_k);
    set_text(d, W2_AVAIL, line);
    strcpy(line, "Approximate disk space needed: ");
    number_k(line + strlen(line), (need + 1023) / 1024);
    set_text(d, W2_NEED, line);
    set_pstr(d, W2_DISK, "Disk: ", place_vol_name);
}

static void show_place(DialogPtr d)
{
    Str255 n;
    if (place_found) {
        CInfoPBRec c;
        memset(&c, 0, sizeof c);
        c.dirInfo.ioNamePtr = n;
        c.dirInfo.ioVRefNum = place_vol;
        c.dirInfo.ioDrDirID = place_found;
        c.dirInfo.ioFDirIndex = -1;
        if (PBGetCatInfoSync(&c) != noErr) memcpy(n, place_name, place_name[0] + 1);
    } else memcpy(n, place_name, place_name[0] + 1);
    set_pstr(d, W2_FOLDER, "Folder: ", n);
    show_space(d);
}

static void item_text(DialogPtr d, short which) { set_text(d, W2_DESC, DESC[which]); }

static void set_mode(DialogPtr d, Boolean c)
{
    static const short LIST[] = { W2_HEAD_APP, W2_MILL, W2_HEAD_PLUG, W2_INET, W2_FONTS, W2_HEAD_DEV, W2_SDK };
    short i;
    custom = c;
    for (i = 0; i < 7; i++) { if (c) ShowDialogItem(d, LIST[i]); else HideDialogItem(d, LIST[i]); }
    set_text(d, W2_DESC, c ? "Choose what to install." : DESC[0]);
    show_space(d);
    InvalRect(&d->portRect);
}

static pascal void popup_draw(DialogPtr d, short item)
{
    short type;
    Handle h;
    Rect r;
    const char *t = custom ? "Custom Install" : "Easy Install";
    Str255 s;
    GetDialogItem(d, item, &type, &h, &r);
    FrameRect(&r);
    MoveTo(r.left + 2, r.bottom); LineTo(r.right, r.bottom); LineTo(r.right, r.top + 2);     /* the shadow */
    s[0] = (unsigned char)strlen(t);
    memcpy(s + 1, t, s[0]);
    MoveTo(r.left + 8, r.top + 15);
    DrawString(s);
    MoveTo(r.right - 18, r.top + 7);                /* the arrows */
    Line(8, 0); Line(-4, 5); Line(-4, -5);
}

static void choose_place(void)
{
    SFReply reply;
    Point where;
    WDPBRec wd;
    Str255 mill;
    long dir;
    where.h = 90;
    where.v = 70;
    SFPutFile(where, "\pInstall Mill in:", place_name, NULL, &reply);
    if (!reply.good) return;
    memset(&wd, 0, sizeof wd);
    wd.ioVRefNum = reply.vRefNum;
    if (PBGetWDInfoSync(&wd) != noErr) { place_vol = reply.vRefNum; dir = 2; } else { place_vol = wd.ioWDVRefNum; dir = wd.ioWDDirID; }
    pstr(mill, "Mill");
    place_parent = dir;
    place_found = has_file(place_vol, dir, mill) ? dir : 0;      /* the folder shown holds Mill: use it as it is */
    memcpy(place_name, reply.fName, reply.fName[0] + 1);
}

/* The folder to install in, made if need be */
static OSErr resolve_place(void)
{
    place_vref = place_vol;
    if (place_found) { place_dir = place_found; return noErr; }
    return dir_of(place_vol, place_parent, place_name, &place_dir);
}

static OSErr run_install(DialogPtr d, Boolean mill, Boolean inet, Boolean fonts, Boolean sdk)
{
    static const struct { short id; char *what; } ONE[3] = { { ID_MILL, "Mill" }, { ID_INET, "Internet Services" }, { ID_FONTS, "Web Fonts" } };
    Boolean want[3];
    short i, n;
    OSErr err = resolve_place();
    Str255 name;
    long dir;
    if (err) return err;
    want[0] = mill; want[1] = inet; want[2] = fonts;
    /* retired names from earlier versions */
    pstr(name, "Mill Fonts");
    remove_file(place_vref, place_dir, name);
    pstr(name, "Mill Fonts Read Me");
    remove_file(place_vref, place_dir, name);
    pstr(name, "Mill Plug-Ins");
    if (dir_of(place_vref, place_dir, name, &dir) == noErr) {
        pstr(name, "Finger");
        remove_file(place_vref, dir, name);
    }
    for (i = 0; i < 3 && !err; i++) {
        Handle h;
        if (!want[i]) continue;
        set_text(d, W2_STATUS, ONE[i].what);
        if (!(h = Get1Resource('MBIN', ONE[i].id))) return resNotFound;
        err = install_one(h);
        ReleaseResource(h);
    }
    if (sdk && !err) {
        n = Count1Resources('MBIN');
        set_text(d, W2_STATUS, "Plug-In SDK");
        for (i = 1; i <= n && !err; i++) {
            Handle h = Get1IndResource('MBIN', i);
            short id;
            ResType t;
            Str255 nm;
            if (!h) continue;
            GetResInfo(h, &id, &t, nm);
            if (id >= ID_SDK) err = install_one(h);
            ReleaseResource(h);
        }
    }
    FlushVol(NULL, place_vref);
    return err;
}

static void install_window(void)
{
    DialogPtr d;
    short item, type, i;
    Handle h;
    Rect r;
    Boolean done = false;
    SysEnvRec env;
    Handle sz;
    if (!(d = GetNewDialog(W2, NULL, (WindowPtr)-1))) return;
    SysEnvirons(1, &env);
    place_vol = env.sysVRefNum;
    place_parent = 2;
    place_found = 0;
    pstr(place_name, "Mill");
    if ((sz = Get1Resource('ISIZ', 128)) && GetHandleSize(sz) >= 16) for (i = 0; i < 4; i++) size_of[i] = ((long *)*sz)[i];
    GetDialogItem(d, W2_RING, &type, &h, &r);
    SetDialogItem(d, W2_RING, type, (Handle)(UserItemUPP)draw_ring2, &r);
    GetDialogItem(d, W2_POPUP, &type, &h, &r);
    SetDialogItem(d, W2_POPUP, type, (Handle)(UserItemUPP)popup_draw, &r);
    popup = NewMenu(200, "\p");
    AppendMenu(popup, "\pEasy Install;Custom Install");
    set_check(d, W2_MILL, 1);
    set_check(d, W2_INET, 1);
    set_check(d, W2_FONTS, 1);
    set_check(d, W2_SDK, 0);
    ShowWindow(d);
    SetPort(d);
    set_mode(d, false);
    show_place(d);
    while (!done) {
        ModalDialog(NULL, &item);
        switch (item) {
        case W2_QUIT: done = true; break;
        case W2_POPUP: {
            Point p;
            long c;
            GetDialogItem(d, W2_POPUP, &type, &h, &r);
            p.v = r.top;
            p.h = r.left;
            LocalToGlobal(&p);
            InsertMenu(popup, -1);
            CheckItem(popup, 1, !custom);
            CheckItem(popup, 2, custom);
            c = PopUpMenuSelect(popup, p.v, p.h, custom ? 2 : 1);
            DeleteMenu(200);
            if (HiWord(c) && (LoWord(c) == 2) != custom) {
                set_mode(d, LoWord(c) == 2);
                EraseRect(&r);
                DrawDialog(d);
            }
            break;
        }
        case W2_MILL: case W2_INET: case W2_FONTS: case W2_SDK:
            set_check(d, item, !get_check(d, item));
            item_text(d, item == W2_MILL ? 1 : item == W2_INET ? 2 : item == W2_FONTS ? 3 : 4);
            show_space(d);
            break;
        case W2_CHOOSE:
            choose_place();
            show_place(d);
            break;
        case W2_INSTALL: {
            OSErr err;
            Boolean m = !custom || get_check(d, W2_MILL), n = !custom || get_check(d, W2_INET);
            Boolean f = !custom || get_check(d, W2_FONTS), s = custom && get_check(d, W2_SDK);
            if (!m && !n && !f && !s) break;
            SetCursor(*GetCursor(watchCursor));
            err = run_install(d, m, n, f, s);
            InitCursor();
            set_text(d, W2_STATUS, "");
            if (err) alert_text("Couldn't install. Error", err);
            else { NoteAlert(141, NULL); done = true; }
            break;
        }
        }
    }
    DisposeMenu(popup);
    DisposeDialog(d);
}

int main(void)
{
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
    FlushEvents(everyEvent, 0);
    if (welcome()) install_window();
    return 0;
}
