/* Install Mill 2.0: one window, Easy Install or Custom Install. The files are gzip-compressed
   'MBIN' resources of this program, each a MacBinary file named (by the resource's name) for
   where it goes inside the folder: "Mill", "Mill Plug-Ins:Web Fonts". Plain File Manager calls,
   so it runs on System 6. */
#include <Multiverse.h>
#include <string.h>
#include <stdlib.h>

#include "../src/core/inflate.h"

enum { DLG = 128, ITEM_INSTALL = 1, ITEM_QUIT, ITEM_EASY, ITEM_CUSTOM, ITEM_MILL, ITEM_INET, ITEM_FONTS, ITEM_SDK,
       ITEM_STATUS, ITEM_RING };
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

/* ---- the window ---- */

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

static pascal void draw_ring(DialogPtr d, short item) { (void)item; ring(d, ITEM_INSTALL); }

static void set_check(DialogPtr d, short item, short on, short enabled)
{
    short type;
    Handle h;
    Rect r;
    GetDialogItem(d, item, &type, &h, &r);
    SetControlValue((ControlHandle)h, on);
    HiliteControl((ControlHandle)h, enabled ? 0 : 255);
}

static short get_check(DialogPtr d, short item)
{
    short type;
    Handle h;
    Rect r;
    GetDialogItem(d, item, &type, &h, &r);
    return GetControlValue((ControlHandle)h);
}

static void status(DialogPtr d, const char *text)
{
    short type;
    Handle h;
    Rect r;
    Str255 s;
    GetDialogItem(d, ITEM_STATUS, &type, &h, &r);
    s[0] = (unsigned char)strlen(text);
    memcpy(s + 1, text, s[0]);
    SetDialogItemText(h, s);
    EraseRect(&r);
    DrawDialog(d);
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
    StopAlert(129, NULL);
}

static void radio(DialogPtr d, short on)
{
    short type;
    Handle h;
    Rect r;
    GetDialogItem(d, ITEM_EASY, &type, &h, &r);
    SetControlValue((ControlHandle)h, on == ITEM_EASY);
    GetDialogItem(d, ITEM_CUSTOM, &type, &h, &r);
    SetControlValue((ControlHandle)h, on == ITEM_CUSTOM);
}

static void mode(DialogPtr d, Boolean custom)
{
    radio(d, custom ? ITEM_CUSTOM : ITEM_EASY);
    if (!custom) {
        set_check(d, ITEM_MILL, 1, 0);
        set_check(d, ITEM_INET, 1, 0);
        set_check(d, ITEM_FONTS, 1, 0);
        set_check(d, ITEM_SDK, 0, 0);
    } else {
        set_check(d, ITEM_MILL, get_check(d, ITEM_MILL), 1);
        set_check(d, ITEM_INET, get_check(d, ITEM_INET), 1);
        set_check(d, ITEM_FONTS, get_check(d, ITEM_FONTS), 1);
        set_check(d, ITEM_SDK, get_check(d, ITEM_SDK), 1);
    }
}

/* Where: Standard File's folder and the name typed. An existing Mill's folder (one with "Mill" in
   it) is used as it is; else a folder of that name in the one shown, made if it isn't there. */
static Boolean choose_place(void)
{
    SFReply reply;
    Point where;
    WDPBRec wd;
    long dir;
    Str255 mill;
    where.h = 90;
    where.v = 70;
    SFPutFile(where, "\pInstall Mill in:", "\pMill", NULL, &reply);
    if (!reply.good) return false;
    memset(&wd, 0, sizeof wd);
    wd.ioVRefNum = reply.vRefNum;
    if (PBGetWDInfoSync(&wd) != noErr) { place_vref = reply.vRefNum; dir = 0; } else { place_vref = wd.ioWDVRefNum; dir = wd.ioWDDirID; }
    pstr(mill, "Mill");
    if (!has_file(place_vref, dir, mill)) {
        if (dir_of(place_vref, dir, reply.fName, &dir) != noErr) { alert_text("Can't make the folder.", 0); return false; }
    }
    place_dir = dir;
    return true;
}

static OSErr run_install(DialogPtr d, Boolean mill, Boolean inet, Boolean fonts, Boolean sdk)
{
    static const struct { short id; char *what; } ONE[3] = { { ID_MILL, "Mill" }, { ID_INET, "Internet Services" }, { ID_FONTS, "Web Fonts" } };
    Boolean want[3];
    short i, n;
    OSErr err = noErr;
    Str255 name;
    long dir;
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
        status(d, ONE[i].what);
        if (!(h = Get1Resource('MBIN', ONE[i].id))) return resNotFound;
        err = install_one(h);
        ReleaseResource(h);
    }
    if (sdk && !err) {
        n = Count1Resources('MBIN');
        status(d, "Plug-In SDK");
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

int main(void)
{
    DialogPtr d;
    short item, type;
    Handle h;
    Rect r;
    Boolean custom = false, done = false;
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
    FlushEvents(everyEvent, 0);
    if (!(d = GetNewDialog(DLG, NULL, (WindowPtr)-1))) return 1;
    GetDialogItem(d, ITEM_RING, &type, &h, &r);
    SetDialogItem(d, ITEM_RING, type, (Handle)(UserItemUPP)draw_ring, &r);
    set_check(d, ITEM_MILL, 1, 0);
    set_check(d, ITEM_INET, 1, 0);
    set_check(d, ITEM_FONTS, 1, 0);
    set_check(d, ITEM_SDK, 0, 0);
    mode(d, false);
    ShowWindow(d);
    SetPort(d);
    while (!done) {
        ModalDialog(NULL, &item);
        switch (item) {
        case ITEM_QUIT: done = true; break;
        case ITEM_EASY: custom = false; mode(d, false); break;
        case ITEM_CUSTOM: custom = true; mode(d, true); break;
        case ITEM_MILL: case ITEM_INET: case ITEM_FONTS: case ITEM_SDK:
            set_check(d, item, !get_check(d, item), 1);
            break;
        case ITEM_INSTALL: {
            OSErr err;
            if (custom && !get_check(d, ITEM_MILL) && !get_check(d, ITEM_INET) && !get_check(d, ITEM_FONTS) && !get_check(d, ITEM_SDK)) break;
            if (!choose_place()) break;
            SetCursor(*GetCursor(watchCursor));
            err = run_install(d, get_check(d, ITEM_MILL), get_check(d, ITEM_INET), get_check(d, ITEM_FONTS), get_check(d, ITEM_SDK));
            InitCursor();
            status(d, "");
            if (err) alert_text("Couldn't install. Error", err);
            else { NoteAlert(130, NULL); done = true; }
            break;
        }
        }
    }
    DisposeDialog(d);
    return 0;
}
