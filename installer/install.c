/* Install Mill 2.0: one window, from a welcome to Easy or Custom Install to the installing. The files are gzip-compressed
   'MBIN' resources of this program, each a MacBinary file named (by the resource's name) for
   where it goes inside the folder: "Mill", "Mill Plug-Ins:Web Fonts". Plain File Manager calls,
   so it runs on System 6. */
#include <Multiverse.h>
#include <string.h>
#include <stdlib.h>

#include "../src/core/inflate.h"

/* One window, 440 by 256, for every stage; the program draws what's in it over a user item, around the buttons */
enum { WIN = 128, B_PRIMARY = 1, B_QUIT, B_CHOOSE, CANVAS };
enum { S_WELCOME, S_INSTALL, S_PROGRESS, S_DONE, S_ERROR };
enum { ID_MILL = 128, ID_INET = 129, ID_FONTS = 130, ID_SDK = 1000 };

/* a file being written, from the MacBinary stream that comes out of inflate */
typedef struct {
    short vref;
    long dir;
    Str255 name, tmp;       /* the file's name, and what it's called while it's written */
    unsigned char head[128];
    long dlen, rlen;
    Point loc;              /* where its icon goes in the folder's window, if place */
    char place;
    long at, polled;        /* where in the stream the next piece begins, and where Cancel was last looked for */
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

/* The file, written under a name of its own and put in place when it's whole, so a Cancel or a full disk
   leaves what was there. Created once the 128-byte header is in. */
static OSErr begin_file(Out *o)
{
    HParamBlockRec h;
    OSErr err;
    short n = o->name[0] > 30 ? 30 : o->name[0];
    o->tmp[0] = (unsigned char)(n + 1);
    o->tmp[1] = '~';
    memcpy(o->tmp + 2, o->name + 1, n);
    remove_file(o->vref, o->dir, o->tmp);
    memset(&h, 0, sizeof h);
    h.fileParam.ioNamePtr = o->tmp;
    h.fileParam.ioVRefNum = o->vref;
    h.fileParam.ioDirID = o->dir;
    if ((err = PBHCreateSync(&h)) != noErr) return err;
    o->opened = 1;
    o->dlen = be32(o->head + 83);
    o->rlen = be32(o->head + 87);
    o->df = o->rf = 0;
    if (o->dlen && (err = open_fork(o->vref, o->dir, o->tmp, false, &o->df)) != noErr) return err;
    if (o->rlen && (err = open_fork(o->vref, o->dir, o->tmp, true, &o->rf)) != noErr) return err;
    return noErr;
}

/* Done with it: whole, it gets its type, creator, and flags and its name (replacing the old file); else it's deleted */
static void end_file(Out *o, Boolean whole)
{
    HParamBlockRec h;
    if (o->df) close_fork(o->df);
    if (o->rf) close_fork(o->rf);
    o->df = o->rf = 0;
    if (!o->opened) return;
    if (!whole) { remove_file(o->vref, o->dir, o->tmp); return; }
    /* "inited" off so the Finder places it */
    memset(&h, 0, sizeof h);
    h.fileParam.ioNamePtr = o->tmp;
    h.fileParam.ioVRefNum = o->vref;
    h.fileParam.ioDirID = o->dir;
    if (PBHGetFInfoSync(&h) == noErr) {
        h.fileParam.ioDirID = o->dir;
        memcpy(&h.fileParam.ioFlFndrInfo.fdType, o->head + 65, 4);
        memcpy(&h.fileParam.ioFlFndrInfo.fdCreator, o->head + 69, 4);
        h.fileParam.ioFlFndrInfo.fdFlags = (short)((((o->head[73] << 8) | o->head[101])) & ~0x0100);
        h.fileParam.ioFlFndrInfo.fdLocation.h = h.fileParam.ioFlFndrInfo.fdLocation.v = 0;
        h.fileParam.ioFlFndrInfo.fdFldr = 0;
        if (o->place) {         /* "inited" on, so the Finder keeps the place */
            h.fileParam.ioFlFndrInfo.fdFlags |= 0x0100;
            h.fileParam.ioFlFndrInfo.fdLocation = o->loc;
        }
        PBHSetFInfoSync(&h);
    }
    remove_file(o->vref, o->dir, o->name);
    memset(&h, 0, sizeof h);
    h.ioParam.ioNamePtr = o->tmp;
    h.ioParam.ioVRefNum = o->vref;
    h.fileParam.ioDirID = o->dir;
    h.ioParam.ioMisc = (Ptr)o->name;
    PBHRenameSync(&h);
}

/* Cancel: a click on the button, Escape, or Command-period, looked for as the files are written */
static DialogPtr main_dlg;
static ControlHandle c_primary;

static Boolean poll_cancel(void)
{
    EventRecord e;
    while (GetNextEvent(mDownMask | keyDownMask | autoKeyMask, &e)) {
        if (e.what == mouseDown) {
            Point p = e.where;
            ControlHandle c;
            WindowPtr w;
            if (FindWindow(p, &w) == inContent && w == (WindowPtr)main_dlg) {
                SetPort(main_dlg);
                GlobalToLocal(&p);
                if (FindControl(p, main_dlg, &c) && c == c_primary && TrackControl(c, p, NULL)) return true;
            }
        } else {
            char ch = e.message & charCodeMask;
            if (ch == 27 || (ch == '.' && (e.modifiers & cmdKey))) return true;
        }
    }
    return false;
}

/* inflate's output: the MacBinary file, data fork then resource fork, each from the next 128 bytes */
static int sink(void *ref, const unsigned char *p, long n)
{
    Out *o = ref;
    long at = o->at, end, i;
    if (o->err) return 1;
    if (n <= 0) return 0;
    if (at == 0 || at - o->polled >= 8192) {       /* every 8K or so */
        o->polled = at;
        if (poll_cancel()) { o->err = userCanceledErr; return 1; }
    }
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

/* The Finder's places in the install folder's window. A location is the icon's top left, less the offset the
   Finder adds (measured on System 6: an icon's middle is 32 across and 25 down from its location, from the
   window's own corner): icon middles at 50, 140, and 230 across and 34 and 104 down. Mill is at the top
   left, the folders a row down from the second column. */
enum { COL_W = 90, ROW_H = 70, LEFT = 18, TOP = 9 };

static void put_folder(short vref, long parent, ConstStr255Param name, short col, short row, const Rect *window)
{
    CInfoPBRec c;
    memset(&c, 0, sizeof c);
    c.dirInfo.ioNamePtr = (StringPtr)name;
    c.dirInfo.ioVRefNum = vref;
    c.dirInfo.ioDrDirID = parent;
    if (PBGetCatInfoSync(&c) != noErr) return;
    if (col >= 0) { c.dirInfo.ioDrUsrWds.frLocation.h = LEFT + col * COL_W; c.dirInfo.ioDrUsrWds.frLocation.v = TOP + row * ROW_H; }
    if (window) c.dirInfo.ioDrUsrWds.frRect = *window;
    c.dirInfo.ioDrUsrWds.frFlags |= 0x0100;
    c.dirInfo.ioNamePtr = (StringPtr)name;
    c.dirInfo.ioDrDirID = parent;
    PBSetCatInfoSync(&c);
}

/* The install folder's own window: big enough for Mill and both folders, with room for the bars */
static void size_window(short vref, long dir)
{
    Str255 name;
    CInfoPBRec c;
    Rect r;
    memset(&c, 0, sizeof c);
    c.dirInfo.ioNamePtr = name;
    c.dirInfo.ioVRefNum = vref;
    c.dirInfo.ioDrDirID = dir;
    c.dirInfo.ioFDirIndex = -1;
    if (PBGetCatInfoSync(&c) != noErr) return;
    SetRect(&r, 40, 60, 340, 250);      /* all of it shows: icons to 275 across, the labels to 133 down, the scroll bars */
    put_folder(vref, c.dirInfo.ioDrParID, name, -1, 0, &r);
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
        {
            long parent = dir;
            if ((err = dir_of(place_vref, dir, comp, &dir)) != noErr) return err;
            if (parent == place_dir) {
                Str255 p1, p2;
                pstr(p1, "Mill Plug-Ins");
                pstr(p2, "Mill Plug-In SDK");
                if (EqualString(comp, p1, false, true)) put_folder(place_vref, parent, comp, 1, 1, NULL);
                else if (EqualString(comp, p2, false, true)) put_folder(place_vref, parent, comp, 2, 1, NULL);
            }
        }
        i++;
    }
    memcpy(o.name, comp, comp[0] + 1);
    o.dir = dir;
    if (dir == place_dir) {
        Str255 m;
        pstr(m, "Mill");
        if (EqualString(comp, m, false, true)) { o.place = 1; o.loc.h = LEFT; o.loc.v = TOP; }
    }
    if (!(z = gz_begin(sink, &o))) return memFullErr;
    HLock(res);
    r = gz_data(z, (unsigned char *)*res, GetHandleSize(res));
    HUnlock(res);
    gz_end(z);
    end_file(&o, o.err == noErr && r >= 0);
    if (o.err) return o.err;
    return r < 0 ? paramErr : noErr;
}

/* ---- the window ---- */

enum { MARGIN = 12, ROW = 16, BAR = 16, ROWS = 7 };

static short stage;
static char message[64], note[48];     /* what's shown, and a note under the box */
static short progress_done, progress_total;
static ControlHandle c_quit, c_choose, w1_bar, w2_bar;

static void small_font(void) { TextFont(kFontIDGeneva); TextSize(9); TextFace(0); }
static void big_font(void) { TextFont(systemFont); TextSize(12); TextFace(0); }

static void ring(void)
{
    Rect r = (*c_primary)->contrlRect;
    PenState ps;
    if (!(*c_primary)->contrlVis) return;
    GetPenState(&ps);
    PenSize(3, 3);
    InsetRect(&r, -4, -4);
    FrameRoundRect(&r, 16, 16);
    SetPenState(&ps);
}

static void pascalize(Str255 s, const char *t)
{
    short n = (short)strlen(t);
    if (n > 255) n = 255;
    s[0] = (unsigned char)n;
    memcpy(s + 1, t, n);
}

/* Text cut to fit in width, with an ellipsis, in the current font */
static void draw_fit(const char *t, short width)
{
    Str255 s;
    short n;
    pascalize(s, t);
    n = s[0];
    if (StringWidth(s) > width) {
        while (n > 0) {
            s[0] = (unsigned char)(n + 1);
            s[n + 1] = (char)0xC9;
            if (StringWidth(s) <= width) break;
            n--;
        }
    }
    DrawString(s);
}

static void draw_centered(const char *t, short y)
{
    Str255 s;
    pascalize(s, t);
    MoveTo(MARGIN + (416 - StringWidth(s)) / 2, y);
    DrawString(s);
}

/* ---- the welcome, with a text box that scrolls ---- */

static TEHandle w1_te;
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

static const char WELCOME[] =
    "Mill is a web browser for monochrome Macs. It reads HTML 3.2, with pictures, tables, and forms.\r\r"
    "Easy Install puts in Mill, the Internet Services Plug-In (Finger, Whois, Dict, Time, FTP, and Gopher), and Web Fonts. "
    "Custom Install also offers the Mill Plug-In SDK.\r\r"
    "Mill needs a Mac Plus or later, System 6.0.3 with MacTCP or later, and 700K of free memory.\r\r"
    "Freeware from OneInAMillion Software.";

/* ---- the install choices ---- */

static const struct { const char *text; short part; } LIST[ROWS] = {      /* part: -1 a heading, else 0 Mill, 1 Internet Services, 2 Web Fonts, 3 SDK */
    { "Application", -1 }, { "Mill", 0 }, { "Plug-Ins", -1 }, { "Internet Services", 1 }, { "Web Fonts", 2 },
    { "Developer", -1 }, { "Mill Plug-In SDK", 3 }
};
static const char *const DESC[4] = {
    "The web browser.", "Finger, Whois, Dict, Time, FTP, and Gopher.", "Serif and Mono bitmaps at every HTML size.",
    "Files for writing Mill Plug-Ins, for CodeWarrior 4 and MPW 3.4."
};

static short place_vol;             /* where the install folder is: a volume, the folder it's in, and its name; */
static long place_parent, place_found;   /* or an existing install's own folder (place_found, else 0) */
static Str255 place_name, place_vol_name;
static Boolean custom, checked[4];
static long size_of[4];             /* Mill, Internet Services, Web Fonts, SDK: bytes */
static long avail_k, need_k;
static short sel = -1, top;         /* the selected row (an item, or -1), and the first row shown */
static MenuHandle popup;
static Rect w2_popup, w2_box, w2_list;

static void number_k(char *out, long k)
{
    Str255 s;
    NumToString(k, s);
    memcpy(out, s + 1, s[0]);
    out[s[0]] = 'K';
    out[s[0] + 1] = 0;
}

/* The disk's free space and what's picked needs; Install is dim if nothing is picked or there's no room */
static void count_space(void)
{
    HParamBlockRec pb;
    Str255 vn;
    long need = 0;
    short i;
    Boolean any = false;
    for (i = 0; i < 4; i++) if (custom ? checked[i] : i < 3) { need += size_of[i]; any = true; }
    memset(&pb, 0, sizeof pb);
    pb.volumeParam.ioNamePtr = vn;
    pb.volumeParam.ioVRefNum = place_vol;
    if (PBHGetVInfoSync(&pb) == noErr) {
        avail_k = ((long)(unsigned short)pb.volumeParam.ioVFrBlk * (pb.volumeParam.ioVAlBlkSiz / 512)) / 2;
        memcpy(place_vol_name, vn, vn[0] + 1);
    }
    need_k = (need + 1023) / 1024;
    if (stage == S_INSTALL) HiliteControl(c_primary, any && need_k <= avail_k ? 0 : 255);
}

static void folder_name(Str255 n)
{
    if (place_found) {
        CInfoPBRec c;
        memset(&c, 0, sizeof c);
        c.dirInfo.ioNamePtr = n;
        c.dirInfo.ioVRefNum = place_vol;
        c.dirInfo.ioDrDirID = place_found;
        c.dirInfo.ioFDirIndex = -1;
        if (PBGetCatInfoSync(&c) == noErr) return;
    }
    memcpy(n, place_name, place_name[0] + 1);
}

static void draw_check(short left, short top_, Boolean on)
{
    Rect b;
    SetRect(&b, left, top_, left + 12, top_ + 12);
    FrameRect(&b);
    if (on) { MoveTo(b.left + 2, b.top + 2); LineTo(b.right - 3, b.bottom - 3); MoveTo(b.right - 3, b.top + 2); LineTo(b.left + 2, b.bottom - 3); }
}

static void draw_list(void)
{
    short i, y;
    RgnHandle save = NewRgn();
    GetClip(save);
    ClipRect(&w2_list);
    for (i = top; i < ROWS && (y = w2_list.top + (i - top) * ROW) < w2_list.bottom; i++) {
        char sz[16];
        Str255 s;
        Rect row;
        SetRect(&row, w2_list.left, y, w2_list.right, y + ROW);
        pascalize(s, LIST[i].text);
        if (LIST[i].part < 0) {
            small_font();
            MoveTo(row.left + 6, y + 12);
            DrawString(s);
            continue;
        }
        small_font();
        draw_check(row.left + 22, y + 2, checked[LIST[i].part]);
        MoveTo(row.left + 40, y + 12);
        DrawString(s);
        number_k(sz, (size_of[LIST[i].part] + 1023) / 1024);
        pascalize(s, sz);
        MoveTo(row.right - 8 - StringWidth(s), y + 12);
        DrawString(s);
        if (i == sel) InvertRect(&row);
    }
    SetClip(save);
    DisposeRgn(save);
}

static void draw_install(void)
{
    Rect frame, lab;
    Str255 name, s;
    char line[240];
    FrameRect(&w2_popup);                                           /* the pop-up menu */
    MoveTo(w2_popup.left + 2, w2_popup.bottom); LineTo(w2_popup.right, w2_popup.bottom); LineTo(w2_popup.right, w2_popup.top + 2);
    big_font();
    pascalize(s, custom ? "Custom Install" : "Easy Install");
    MoveTo(w2_popup.left + 8, w2_popup.top + 15);
    DrawString(s);
    {                                   /* the pop-up arrow: a solid triangle, rows 11, 9, 7, 5, 3, 1 wide */
        short k, cx = w2_popup.right - 17;
        Rect row;
        for (k = 0; k < 6; k++) {
            SetRect(&row, cx - 5 + k, w2_popup.top + 7 + k, cx + 6 - k, w2_popup.top + 8 + k);
            PaintRect(&row);
        }
    }
    FrameRect(&w2_box);                                             /* the box: the description, or the list */
    small_font();
    if (!custom) TEUpdate(&w1_view, w1_te);
    else draw_list();
    small_font();
    MoveTo(w2_box.left, w2_box.bottom + 14);                        /* what the selected item is */
    if (note[0]) draw_fit(note, 416);
    else if (custom && sel >= 0) draw_fit(DESC[LIST[sel].part], 416);
    strcpy(line, "Disk space available: ");
    number_k(line + strlen(line), avail_k);
    strcat(line, "      Needed: ");
    number_k(line + strlen(line), need_k);
    MoveTo(w2_box.left, w2_box.bottom + 30);
    DrawString((ConstStr255Param)"\000");
    pascalize(s, line);
    DrawString(s);
    SetRect(&frame, w2_box.left, w2_box.bottom + 58, 428, w2_box.bottom + 90);      /* where it goes */
    FrameRect(&frame);
    big_font();
    {
        short w = StringWidth("\pInstall Location");       /* the gap in the line: 5 pixels each side of the text */
        SetRect(&lab, frame.left + 12, frame.top, frame.left + 12 + w + 10, frame.top + 1);
        EraseRect(&lab);
        MoveTo(lab.left + 5, frame.top + 5);
        DrawString("\pInstall Location");
    }
    small_font();
    folder_name(name);
    {
        char fn[80], vn[80];
        memcpy(fn, name + 1, name[0] > 60 ? 60 : name[0]);
        fn[name[0] > 60 ? 60 : name[0]] = 0;
        memcpy(vn, place_vol_name + 1, place_vol_name[0] > 60 ? 60 : place_vol_name[0]);
        vn[place_vol_name[0] > 60 ? 60 : place_vol_name[0]] = 0;
        strcpy(line, "Items will go into the folder \"");
        strcat(line, fn);
        strcat(line, "\" on the disk \"");
        strcat(line, vn);
        strcat(line, "\".");
    }
    MoveTo(frame.left + 8, frame.top + 20);
    draw_fit(line, 330 - 8);
    big_font();
}

/* ---- installing, and the end ---- */

static void draw_progress(void)
{
    Rect bar, fill;
    big_font();
    draw_centered(message, 96);
    SetRect(&bar, MARGIN + 60, 116, MARGIN + 356, 132);
    FrameRect(&bar);
    fill = bar;
    InsetRect(&fill, 2, 2);
    if (progress_total) fill.right = fill.left + (fill.right - fill.left) * progress_done / progress_total;
    PaintRect(&fill);
}

static void draw_message(void)
{
    big_font();
    draw_centered(message, 112);
}

static pascal void draw_all(DialogPtr d, short item)
{
    (void)item;
    switch (stage) {
    case S_INSTALL: draw_install(); break;
    case S_PROGRESS: draw_progress(); break;
    default: draw_message();
    }
    ring();
    DrawControls(d);
}

static void redraw(DialogPtr d)
{
    Rect r = d->portRect;
    count_space();
    EraseRect(&r);
    draw_all(d, CANVAS);
}

static void place_control(DialogPtr d, short item, short top_, short left, short bottom, short right, const char *title, Boolean show)
{
    ControlHandle c = item == B_PRIMARY ? c_primary : item == B_QUIT ? c_quit : c_choose;
    Str255 t;
    short type;
    Handle h;
    Rect r;
    HideControl(c);
    GetDialogItem(d, item, &type, &h, &r);
    if (show) SetRect(&r, left, top_, right, bottom);
    else SetRect(&r, -100, -100, -80, -80);             /* the dialog's item goes with it, so a click can't find it */
    SetDialogItem(d, item, type, h, &r);
    if (!show) return;
    MoveControl(c, left, top_);
    SizeControl(c, right - left, bottom - top_);
    if (title) { pascalize(t, title); SetControlTitle(c, t); }
    HiliteControl(c, 0);
    ShowControl(c);
}

/* Which controls show, and where, for a stage */
static void set_stage(DialogPtr d, short st, const char *text)
{
    stage = st;
    if (text) strcpy(message, text);
    if (st == S_INSTALL && !custom) ShowControl(w1_bar); else HideControl(w1_bar);
    if (st == S_INSTALL && custom) ShowControl(w2_bar); else HideControl(w2_bar);
    switch (st) {
    case S_INSTALL:
        place_control(d, B_PRIMARY, 12, 348, 34, 428, "Install", true);
        place_control(d, B_QUIT, 46, 348, 68, 428, "Quit", true);
        place_control(d, B_CHOOSE, 220, 342, 240, 422, "Choose\311", true);
        break;
    case S_PROGRESS:
        place_control(d, B_PRIMARY, 12, 348, 34, 428, "Cancel", true);
        place_control(d, B_QUIT, 0, 0, 0, 0, NULL, false);
        place_control(d, B_CHOOSE, 0, 0, 0, 0, NULL, false);
        break;
    case S_DONE:
        place_control(d, B_PRIMARY, 0, 0, 0, 0, NULL, false);
        place_control(d, B_QUIT, 222, 348, 244, 428, "Quit", true);
        place_control(d, B_CHOOSE, 0, 0, 0, 0, NULL, false);
        break;
    default:        /* an error: back to the choices, or quit */
        place_control(d, B_PRIMARY, 222, 348, 244, 428, "OK", true);
        place_control(d, B_QUIT, 222, 258, 244, 338, "Quit", true);
        place_control(d, B_CHOOSE, 0, 0, 0, 0, NULL, false);
    }
    redraw(d);
}

static void set_top(DialogPtr d, short to)
{
    short max = ROWS - (w2_list.bottom - w2_list.top) / ROW;
    if (to > max) to = max;
    if (to < 0) to = 0;
    if (to == top) return;
    top = to;
    SetControlValue(w2_bar, to);
    redraw(d);
}

static DialogPtr bar_dialog;
static pascal void w2_track(ControlHandle c, short part)
{
    short page = (w2_list.bottom - w2_list.top) / ROW - 1;
    (void)c;
    if (part == inUpButton) set_top(bar_dialog, top - 1);
    else if (part == inDownButton) set_top(bar_dialog, top + 1);
    else if (part == inPageUp) set_top(bar_dialog, top - page);
    else if (part == inPageDown) set_top(bar_dialog, top + page);
}

static void set_custom(DialogPtr d, Boolean c)
{
    note[0] = 0;
    custom = c;
    sel = -1;
    top = 0;
    SetControlValue(w2_bar, 0);
    if (c) { ShowControl(w2_bar); HideControl(w1_bar); } else { HideControl(w2_bar); ShowControl(w1_bar); }
    redraw(d);
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

static void step(DialogPtr d, const char *verb, const char *what, short done)
{
    strcpy(message, verb);
    strcat(message, what);
    progress_done = done;
    redraw(d);
}

/* A folder that's there (not made, as dir_of does) */
static OSErr find_dir(short vref, long parent, ConstStr255Param name, long *dir)
{
    CInfoPBRec c;
    OSErr err;
    memset(&c, 0, sizeof c);
    c.dirInfo.ioNamePtr = (StringPtr)name;
    c.dirInfo.ioVRefNum = vref;
    c.dirInfo.ioDrDirID = parent;
    if ((err = PBGetCatInfoSync(&c)) != noErr) return err;
    if (!(c.dirInfo.ioFlAttrib & 16)) return fnfErr;
    *dir = c.dirInfo.ioDrDirID;
    return noErr;
}

/* An old name from an earlier version, gone if it's there */
static OSErr retire(DialogPtr d, long dir, const char *what)
{
    Str255 name;
    pstr(name, what);
    if (!has_file(place_vref, dir, name)) return noErr;
    if (poll_cancel()) return userCanceledErr;
    step(d, "Removing: ", what, progress_done);
    remove_file(place_vref, dir, name);
    return noErr;
}

static OSErr run_install(DialogPtr d, Boolean mill, Boolean inet, Boolean fonts, Boolean sdk)
{
    static const struct { short id; char *what; } ONE[3] = { { ID_MILL, "Mill" }, { ID_INET, "Internet Services" }, { ID_FONTS, "Web Fonts" } };
    Boolean want[3];
    short i, n, done = 0;
    OSErr err = noErr;
    Str255 name;
    long dir;
    want[0] = mill; want[1] = inet; want[2] = fonts;
    size_window(place_vref, place_dir);
    progress_total = mill + inet + fonts + sdk;
    /* retired names from earlier versions */
    err = retire(d, place_dir, "Mill Fonts");
    if (!err) err = retire(d, place_dir, "Mill Fonts Read Me");
    pstr(name, "Mill Plug-Ins");
    if (!err && find_dir(place_vref, place_dir, name, &dir) == noErr) err = retire(d, dir, "Finger");
    if (err) return err;
    for (i = 0; i < 3 && !err; i++) {
        Handle h;
        if (!want[i]) continue;
        if (poll_cancel()) return userCanceledErr;
        step(d, "Installing: ", ONE[i].what, done++);
        if (!(h = Get1Resource('MBIN', ONE[i].id))) return resNotFound;
        err = install_one(h);
        ReleaseResource(h);
    }
    if (sdk && !err) {
        n = Count1Resources('MBIN');
        if (poll_cancel()) return userCanceledErr;
        step(d, "Installing: ", "Mill Plug-In SDK", done++);
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
    progress_done = progress_total;
    return err;
}

/* Clicks on what the program draws: the pop-up menu, the list */
static Boolean install_click(DialogPtr d, Point p)
{
    if (PtInRect(p, &w2_popup)) {
        Point g;
        long c;
        g.v = w2_popup.top;
        g.h = w2_popup.left;
        LocalToGlobal(&g);
        InsertMenu(popup, -1);
        CheckItem(popup, 1, !custom);
        CheckItem(popup, 2, custom);
        c = PopUpMenuSelect(popup, g.v, g.h, custom ? 2 : 1);
        DeleteMenu(200);
        if (HiWord(c) && (LoWord(c) == 2) != custom) set_custom(d, LoWord(c) == 2);
        return true;
    }
    if (custom && PtInRect(p, &w2_list)) {
        short i = top + (p.v - w2_list.top) / ROW;
        if (i < ROWS && LIST[i].part >= 0) {
            short part = LIST[i].part;
            note[0] = 0;
            if (p.h < w2_list.left + 38) checked[part] = !checked[part];
            sel = i;
            redraw(d);
        }
        return true;
    }
    return false;
}

static Point click_pt;

static pascal Boolean filter(DialogPtr d, EventRecord *e, short *item)
{
    if (e->what == keyDown || e->what == autoKey) {
        char c = e->message & charCodeMask;
        if ((c == 13 || c == 3) && (*c_primary)->contrlVis && (*c_primary)->contrlHilite != 255) { *item = B_PRIMARY; return true; }
    }
    if (e->what == mouseDown) {
        Point p = e->where;
        ControlHandle c;
        short part;
        WindowPtr w;
        Rect limit = qd.screenBits.bounds;
        if (FindWindow(p, &w) == inDrag && w == (WindowPtr)d) {      /* it moves by its title bar */
            InsetRect(&limit, 4, 4);
            DragWindow(w, p, &limit);
            *item = CANVAS;
            return true;
        }
        SetPort(d);
        GlobalToLocal(&p);
        part = FindControl(p, d, &c);
        if (part && stage == S_INSTALL && !custom && c == w1_bar) {
            if (part == inThumb) { TrackControl(c, p, NULL); w1_scroll(GetControlValue(c)); }
            else TrackControl(c, p, (ControlActionUPP)w1_track);
            *item = CANVAS;
            return true;
        }
        if (part && stage == S_INSTALL && c == w2_bar) {
            bar_dialog = d;
            if (part == inThumb) { TrackControl(c, p, NULL); set_top(d, GetControlValue(c)); }
            else TrackControl(c, p, (ControlActionUPP)w2_track);
            *item = CANVAS;
            return true;
        }
        click_pt = p;       /* a click on what's drawn comes back as CANVAS, with the button still down */
    }
    return false;
}

int main(void)
{
    DialogPtr d;
    short item, type, i;
    Handle h;
    Rect r;
    Boolean done = false;
    SysEnvRec env;
    Handle sz;
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
    FlushEvents(everyEvent, 0);
    if (!(d = GetNewDialog(WIN, NULL, (WindowPtr)-1))) return 1;
    main_dlg = d;
    SetPort(d);
    c_primary = (GetDialogItem(d, B_PRIMARY, &type, &h, &r), (ControlHandle)h);
    c_quit = (GetDialogItem(d, B_QUIT, &type, &h, &r), (ControlHandle)h);
    c_choose = (GetDialogItem(d, B_CHOOSE, &type, &h, &r), (ControlHandle)h);
    GetDialogItem(d, CANVAS, &type, &h, &r);
    SetDialogItem(d, CANVAS, type, (Handle)(UserItemUPP)draw_all, &r);
    /* the Easy box's text and bar: the welcome */
    SetRect(&w1_box, MARGIN, 44, MARGIN + 300, 156);
    w1_view = w1_box;
    w1_view.right -= BAR;
    InsetRect(&w1_view, 6, 5);
    small_font();
    w1_te = TENew(&w1_view, &w1_view);
    TESetText((Ptr)WELCOME, strlen(WELCOME), w1_te);
    w1_line = (*w1_te)->lineHeight;
    w1_view.bottom = w1_view.top + (w1_view.bottom - w1_view.top) / w1_line * w1_line;     /* whole lines only */
    (*w1_te)->viewRect = w1_view;
    w1_max = (*w1_te)->nLines - (w1_view.bottom - w1_view.top) / w1_line;
    if (w1_max < 0) w1_max = 0;
    SetRect(&r, w1_box.right - BAR, w1_box.top, w1_box.right, w1_box.bottom);
    w1_bar = NewControl(d, &r, "\p", false, 0, 0, w1_max, scrollBarProc, 0);
    HiliteControl(w1_bar, w1_max ? 0 : 255);
    /* the choices */
    SysEnvirons(1, &env);
    place_vol = env.sysVRefNum;
    place_parent = 2;
    place_found = 0;
    pstr(place_name, "Mill");
    if ((sz = Get1Resource('ISIZ', 128)) && GetHandleSize(sz) >= 16) for (i = 0; i < 4; i++) size_of[i] = ((long *)*sz)[i];
    for (i = 0; i < 3; i++) checked[i] = true;
    SetRect(&w2_popup, MARGIN, MARGIN, MARGIN + 140, MARGIN + 20);
    SetRect(&w2_box, MARGIN, 44, MARGIN + 300, 156);
    w2_list = w2_box;
    InsetRect(&w2_list, 1, 1);
    w2_list.right = w2_box.right - BAR;
    SetRect(&r, w2_box.right - BAR, w2_box.top, w2_box.right, w2_box.bottom);
    w2_bar = NewControl(d, &r, "\p", false, 0, 0, ROWS - (w2_list.bottom - w2_list.top) / ROW, scrollBarProc, 0);
    popup = NewMenu(200, "\p");
    AppendMenu(popup, "\pEasy Install;Custom Install");
    big_font();
    ShowWindow(d);
    count_space();
    set_stage(d, S_INSTALL, NULL);
    while (!done) {
        ModalDialog((ModalFilterUPP)filter, &item);
        if (item == B_QUIT) done = true;
        else if (item == CANVAS) { if (stage == S_INSTALL) install_click(d, click_pt); }
        else if (item == B_CHOOSE) { choose_place(); note[0] = 0; redraw(d); }
        else if (item == B_PRIMARY) {
            if (stage == S_ERROR) set_stage(d, S_INSTALL, NULL);
            else if (stage == S_INSTALL) {
                OSErr err;
                SetCursor(*GetCursor(watchCursor));
                if ((err = resolve_place()) != noErr) { InitCursor(); set_stage(d, S_ERROR, "Folder creation failed."); continue; }
                progress_done = 0;
                set_stage(d, S_PROGRESS, "");
                err = run_install(d, !custom || checked[0], !custom || checked[1], !custom || checked[2], custom && checked[3]);
                InitCursor();
                note[0] = 0;
                if (err == userCanceledErr) {
                    strcpy(note, "Installation canceled.");
                    set_stage(d, S_INSTALL, NULL);
                } else if (err) {
                    char text[48], num[16];
                    Str255 n;
                    NumToString(err, n);
                    memcpy(num, n + 1, n[0]);
                    num[n[0]] = 0;
                    strcpy(text, "Installation failed (error ");
                    strcat(text, num);
                    strcat(text, ").");
                    set_stage(d, S_ERROR, text);
                } else set_stage(d, S_DONE, "Mill is installed.");
            }
        }
    }
    DisposeMenu(popup);
    TEDispose(w1_te);
    DisposeDialog(d);
    return 0;
}
