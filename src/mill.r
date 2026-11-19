#include "Processes.r"

/* Preferred 1200K: room for a page with many pictures (The Old Net's 77 run out in 700K).
   Minimum 700K: what a 1 MB Mac Plus on System 6.0.3 gives an application from the Finder
   (735K), less a little; under MultiFinder at 1 MB the largest block is too small. In the
   minimum, pages still load: a page's source is let go of once it's read, and running out
   of memory stops loading and keeps what's there. */
resource 'SIZE' (-1) {
    reserved, acceptSuspendResumeEvents, reserved, canBackground, doesActivateOnFGSwitch,
    backgroundAndForeground, dontGetFrontClicks, ignoreChildDiedEvents, is32BitCompatible,
    isHighLevelEventAware, onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
    reserved, reserved, reserved,
    1200 * 1024,   /* preferred */
    700 * 1024     /* minimum */
};

#include "Finder.r"
#include "icons.r"
#include "listicons.r"
#include "splash.r"
#include "splash_text.r"
#include "pages.r"
#include "version.h"

/* What the Finder's Get Info shows */
resource 'vers' (1) {
    MILL_VERS_MAJOR, MILL_VERS_MINOR, release, 0x00, verUS,
    MILL_VERSION,
    MILL_VERSION ", \$A9 " MILL_YEAR " " MILL_PUBLISHER ". Freeware."
};

/* Mill's Finder icons: the windmill, and its Plug-In files */
type 'MiLL' { pstring; };
resource 'MiLL' (0, "Owner resource") { "Mill " MILL_VERSION " (Mill)" };
resource 'FREF' (128) { 'APPL', 0, "" };
/* Plug-In files ('MPlg', made by Mill): the windmill on a puzzle piece */
resource 'FREF' (129) { 'MPlg', 1, "" };
/* Web Fonts ('FFIL', creator 'MiLL'): the windmill with a serif A. The Finder takes the
   suitcase for Mill's, so it shows this icon and a double click starts Mill. */
resource 'FREF' (130) { 'FFIL', 2, "" };
resource 'BNDL' (128) {
    'MiLL', 0,
    { 'ICN#', { 0, 128; 1, 129; 2, 130 }; 'ics#', { 0, 128; 1, 129; 2, 130 }; 'FREF', { 0, 128; 1, 129; 2, 130 } }
};

/* Add Bookmark: which slot, and the title and address to keep there */
resource 'DLOG' (200, "Bookmark") {
    {0, 0, 180, 320}, dBoxProc, invisible, noGoAway, 0, 200, "", noAutoCenter
};
resource 'DITL' (200) {
    {
        {146, 236, 166, 306}, Button { enabled, "OK" },
        {146, 152, 166, 222}, Button { enabled, "Cancel" },
        {12, 12, 28, 84}, StaticText { disabled, "Bookmark:" },
        {10, 88, 30, 306}, UserItem { enabled },
        {42, 12, 58, 84}, StaticText { disabled, "Title:" },
        {42, 90, 58, 304}, EditText { enabled, "" },
        {70, 12, 86, 84}, StaticText { disabled, "Address:" },
        {70, 90, 102, 304}, EditText { enabled, "" },
        {142, 232, 170, 310}, UserItem { disabled },
        {112, 86, 128, 306}, CheckBox { enabled, "Save name and password" }
    }
};

/* A name and password, for a site (or later an FTP server) that asks */
resource 'DLOG' (201, "Login") {
    {0, 0, 150, 320}, dBoxProc, invisible, noGoAway, 0, 201, "", noAutoCenter
};
resource 'DITL' (201) {
    {
        {116, 236, 136, 306}, Button { enabled, "OK" },
        {116, 152, 136, 222}, Button { enabled, "Cancel" },
        {10, 12, 42, 306}, StaticText { disabled, "" },
        {54, 12, 70, 84}, StaticText { disabled, "Name:" },
        {54, 90, 70, 304}, EditText { enabled, "" },
        {82, 12, 98, 84}, StaticText { disabled, "Password:" },
        {82, 90, 98, 304}, EditText { enabled, "" },
        {112, 232, 140, 310}, UserItem { disabled }
    }
};

/* Find: what to look for on the page */
resource 'DLOG' (208, "Find") {
    {0, 0, 84, 320}, dBoxProc, invisible, noGoAway, 0, 208, "", noAutoCenter
};
resource 'DITL' (208) {
    {
        {50, 236, 70, 306}, Button { enabled, "Find" },
        {50, 152, 70, 222}, Button { enabled, "Cancel" },
        {14, 12, 30, 52}, StaticText { disabled, "Find:" },
        {14, 58, 30, 304}, EditText { enabled, "" },
        {46, 232, 74, 310}, UserItem { disabled }
    }
};

/* An archive (a format with members): Cancel, Browse, or Save..., Save... the default.
   ^0 its name, ^1 Mill's line ("This is a zip archive, 2,100K."), ^2 why Browse is dimmed */
resource 'DLOG' (206, "Archive") {
    {0, 0, 150, 340}, dBoxProc, invisible, noGoAway, 0, 206, "", noAutoCenter
};
resource 'DITL' (206) {
    {
        {116, 256, 136, 326}, Button { enabled, "Save\$C9" },
        {116, 92, 136, 162}, Button { enabled, "Cancel" },
        {116, 174, 136, 244}, Button { enabled, "Browse" },
        {10, 14, 26, 326}, StaticText { disabled, "^0" },
        {30, 14, 46, 326}, StaticText { disabled, "^1" },
        {50, 14, 82, 326}, StaticText { disabled, "^2" },
        {88, 12, 104, 326}, CheckBox { enabled, "Browse after saving" },
        {112, 252, 140, 330}, UserItem { disabled }
    }
};

/* When there isn't the memory for another window; placed for a 512x342 screen, as
   System 6 doesn't center alerts */
resource 'ALRT' (202) {
    {80, 96, 190, 416}, 202, {
        OK, visible, sound1, OK, visible, sound1, OK, visible, sound1, OK, visible, sound1
    }, noAutoCenter
};
resource 'DITL' (202) {
    {
        {76, 236, 96, 306}, Button { enabled, "OK" },
        {10, 70, 66, 306}, StaticText { disabled, "There isn\$D5t enough memory for another window. Close a window and try again." }
    }
};

/* Leaving, closing, or quitting with a form that's changed and not sent; ^0 says which */
resource 'ALRT' (203) {
    {80, 96, 196, 416}, 203, {
        OK, visible, sound1, OK, visible, sound1, OK, visible, sound1, OK, visible, sound1
    }, noAutoCenter
};
resource 'DITL' (203) {
    {
        {82, 236, 102, 306}, Button { enabled, "Stay" },
        {82, 152, 102, 222}, Button { enabled, "Leave" },
        {10, 70, 70, 306}, StaticText { disabled, "This page has a form you\$D5ve changed and haven\$D5t sent. ^0" }
    }
};

/* Memory ran out while loading, saving, or laying out; ^0 says what happened */
resource 'ALRT' (204) {
    {80, 96, 180, 416}, 204, {
        OK, visible, sound1, OK, visible, sound1, OK, visible, sound1, OK, visible, sound1
    }, noAutoCenter
};
resource 'DITL' (204) {
    {
        {66, 236, 86, 306}, Button { enabled, "OK" },
        {10, 70, 56, 306}, StaticText { disabled, "Mill ran out of memory, so ^0" }
    }
};

/* Save As of a name whose part file ("part." and the name) is there, where the protocol
   can seek: Resume (the default), Replace, or Cancel */
resource 'ALRT' (207) {
    {80, 96, 166, 416}, 207, {
        OK, visible, sound1, OK, visible, sound1, OK, visible, sound1, OK, visible, sound1
    }, noAutoCenter
};
resource 'DITL' (207) {
    {
        {56, 236, 76, 306}, Button { enabled, "Resume" },
        {56, 152, 76, 222}, Button { enabled, "Replace" },
        {56, 68, 76, 138}, Button { enabled, "Cancel" },
        {10, 70, 42, 306}, StaticText { disabled, "Attempt to resume, or replace this file?" }
    }
};

/* A request that failed (a Plug-In's Fail, a protocol's network error): what went wrong */
resource 'ALRT' (205) {
    {80, 96, 196, 416}, 205, {
        OK, visible, sound1, OK, visible, sound1, OK, visible, sound1, OK, visible, sound1
    }, noAutoCenter
};
resource 'DITL' (205) {
    {
        {82, 236, 102, 306}, Button { enabled, "OK" },
        {10, 70, 74, 306}, StaticText { disabled, "^0" }
    }
};
