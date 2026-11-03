/* The splash, in a code segment of its own (see mill.segmap). Retro68 always puts its
   runtime and libc in segment 1; the Finder loads that, then this small segment, and the
   splash goes up while the rest of Mill (60K) loads, which from a floppy or over
   AppleShare takes a while. */
#include <Multiverse.h>

#include "mac_start.h"
#include "version.h"

#define EXTRA_STACK (32L * 1024)  /* the 68000 Macs' 8K stack is too little for nested tables */
#define SPLASH_W 212
#define SPLASH_H 252

static void center_text(ConstStr255Param s, short font, short size, short face, short y)
{
    TextFont(font);
    TextSize(size);
    TextFace(face);
    MoveTo((SPLASH_W - StringWidth(s)) / 2, y);
    DrawString(s);
}

/* Don Quixote and the windmills (G. A. Harker, 1910), over the name and version as
   mill://about shows them: centered below the menu bar */
static WindowPtr splash_open(void)
{
    Rect r, screen = qd.screenBits.bounds;
    WindowPtr w;
    Handle pic = GetResource('SPLS', 128);
    short top = 20 + (screen.bottom - 20 - SPLASH_H) / 2;
    SetRect(&r, (screen.right - SPLASH_W) / 2, top, (screen.right + SPLASH_W) / 2, top + SPLASH_H);
    w = NewWindow(NULL, &r, "\p", true, dBoxProc, (WindowPtr)-1, false, 0);
    SetPort(w);
    if (pic) {
        BitMap bm;
        short *head;
        Rect dst;
        HLock(pic);
        head = (short *)*pic;
        bm.baseAddr = *pic + 6;
        bm.rowBytes = head[2];
        SetRect(&bm.bounds, 0, 0, head[0], head[1]);
        SetRect(&dst, (SPLASH_W - head[0]) / 2, 8, (SPLASH_W - head[0]) / 2 + head[0], 8 + head[1]);
        CopyBits(&bm, &w->portBits, &bm.bounds, &dst, srcCopy, NULL);
        HUnlock(pic);
        ReleaseResource(pic);
    }
    /* as mill://about: an H1 in Times 24 bold, then lines of Times 12 */
    center_text("\pMill", 20 /* Times */, 24, bold, 199);
    center_text("\pA web browser for classic Macs", 20, 12, 0, 215);
    center_text("\pVersion " MILL_VERSION " \xE1 Freeware", 20, 12, 0, 229);
    center_text("\p\xA9 " MILL_YEAR " " MILL_PUBLISHER, 20, 12, 0, 243);
    TextFont(0);
    TextSize(0);
    TextFace(0);
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
    splash = splash_open();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    return mill_run(splash);
}
