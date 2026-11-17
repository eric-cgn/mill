/* The splash, in a code segment of its own (see mill.segmap). Retro68 always puts its
   runtime and libc in segment 1; the Finder loads that, then this small segment, and the
   splash goes up while the rest of Mill (60K) loads, which from a floppy or over
   AppleShare takes a while. */
#include <Multiverse.h>
#include <string.h>

#include "mac_start.h"
#include "version.h"

#define EXTRA_STACK (32L * 1024)  /* the 68000 Macs' 8K stack is too little for nested tables */
#define SPLASH_W 212
#define SPLASH_H 250
#define TEXT_TOP 180            /* the words, under the drawing */
#define SPLASH_VERSION_Y 213    /* TEXT_TOP + the baseline tools/splash.py text prints */
#define SPLASH_STATUS_Y 242     /* "Loading: ..." */

void (*splash_say)(const char *what);
static WindowPtr splash_win;

/* a picture made by tools/splash.py ('SPLS': width, height, row bytes, then rows),
   centered at top */
static void put_picture(WindowPtr w, short id, short top)
{
    Handle pic = GetResource('SPLS', id);
    BitMap bm;
    short *head;
    Rect dst;
    if (!pic) return;
    HLock(pic);
    head = (short *)*pic;
    bm.baseAddr = *pic + 6;
    bm.rowBytes = head[2];
    SetRect(&bm.bounds, 0, 0, head[0], head[1]);
    SetRect(&dst, (SPLASH_W - head[0]) / 2, top, (SPLASH_W - head[0]) / 2 + head[0], top + head[1]);
    CopyBits(&bm, &w->portBits, &bm.bounds, &dst, srcCopy, NULL);
    HUnlock(pic);
    ReleaseResource(pic);
}

/* a line in Geneva 9, which every System has, centered at y */
static void center_line(ConstStr255Param s, short y)
{
    TextFont(3 /* Geneva */);
    TextSize(9);
    MoveTo((SPLASH_W - StringWidth(s)) / 2, y);
    DrawString(s);
    TextFont(0);
    TextSize(0);
}

/* what Mill is loading, under the copyright, until the browser is up */
static void splash_status(const char *what)
{
    Str255 s;
    Rect r;
    GrafPtr was;
    short n = strlen(what);
    if (n > 200) n = 200;
    memcpy(s + 1, "Loading: ", 9);
    memcpy(s + 10, what, n);
    s[0] = 9 + n;
    GetPort(&was);
    SetPort(splash_win);    /* the browser's window may be the port by now */
    SetRect(&r, 0, SPLASH_STATUS_Y - 10, SPLASH_W, SPLASH_STATUS_Y + 3);
    EraseRect(&r);
    center_line(s, SPLASH_STATUS_Y);
    SetPort(was);
}

/* Don Quixote and the windmills (G. A. Harker, 1910), then "Mill" and the copyright in Mill
   Serif as a picture (tools/splash.py text), so they look as mill://about does with Web
   Fonts on any System; the version between them; centered below the menu bar */
static WindowPtr splash_open(void)
{
    Rect r, screen = qd.screenBits.bounds;
    WindowPtr w;
    short top = 20 + (screen.bottom - 20 - SPLASH_H) / 2;
    SetRect(&r, (screen.right - SPLASH_W) / 2, top, (screen.right + SPLASH_W) / 2, top + SPLASH_H);
    w = NewWindow(NULL, &r, "\p", true, dBoxProc, (WindowPtr)-1, false, 0);
    SetPort(w);
    put_picture(w, 128, 8);
    put_picture(w, 129, TEXT_TOP);
    center_line("\pVersion " MILL_VERSION " - Freeware", SPLASH_VERSION_Y);
    return w;
}

int main(void)
{
    WindowPtr splash;
    SetApplLimit(LMGetApplLimit() - EXTRA_STACK);
    MaxApplZone();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    splash_win = splash = splash_open();
    splash_say = splash_status;
    splash_status("Mill");
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    return mill_run(splash);
}
