#include "Processes.r"
#include "Finder.r"
#include "payload.r"
#include "../src/version.h"
#include "icons.r"

resource 'SIZE' (-1) {
    reserved, acceptSuspendResumeEvents, reserved, canBackground, doesActivateOnFGSwitch,
    backgroundAndForeground, dontGetFrontClicks, ignoreChildDiedEvents, is32BitCompatible,
    notHighLevelEventAware, onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
    reserved, reserved, reserved,
    400 * 1024, 300 * 1024
};

resource 'vers' (1) {
    MILL_VERS_MAJOR, MILL_VERS_MINOR, release, 0x00, verUS, MILL_VERSION,
    "Install Mill " MILL_VERSION ", \$A9 " MILL_YEAR " " MILL_PUBLISHER ". Freeware."
};

type 'MiLI' { pstring; };
resource 'MiLI' (0, "Owner resource") { "Install Mill " MILL_VERSION };
resource 'FREF' (128) { 'APPL', 0, "" };
resource 'BNDL' (128) {
    'MiLI', 0,
    { 'ICN#', { 0, 128 }; 'ics#', { 0, 128 }; 'FREF', { 0, 128 } }
};

/* Window 1, the welcome: the title, and a scrolling text box (drawn by the program) */
resource 'DLOG' (128) {
    {40, 76, 290, 436}, dBoxProc, invisible, noGoAway, 0, 128, "", noAutoCenter
};
resource 'DITL' (128) {
    {
        {212, 270, 234, 340}, Button { enabled, "Continue" },
        {212, 184, 234, 254}, Button { enabled, "Quit" },
        {206, 266, 240, 344}, UserItem { disabled },
        {12, 14, 34, 346}, StaticText { disabled, "Install Mill " MILL_VERSION },
        {44, 14, 196, 346}, UserItem { enabled }
    }
};

/* Window 2, the install: the pop-up menu (drawn by the program), what's in it, and where it goes */
resource 'DLOG' (129) {
    {20, 36, 340, 476}, dBoxProc, invisible, noGoAway, 0, 129, "", noAutoCenter
};
resource 'DITL' (129) {
    {
        {290, 340, 312, 420}, Button { enabled, "Install" },
        {290, 250, 312, 320}, Button { enabled, "Quit" },
        {284, 334, 314, 426}, UserItem { disabled },
        {10, 14, 30, 174}, UserItem { enabled },
        {38, 14, 86, 426}, StaticText { disabled, "" },
        {92, 14, 108, 200}, StaticText { disabled, "Application" },
        {108, 30, 124, 220}, CheckBox { enabled, "Mill" },
        {124, 14, 140, 200}, StaticText { disabled, "Plug-Ins" },
        {140, 30, 156, 220}, CheckBox { enabled, "Internet Services" },
        {156, 30, 172, 220}, CheckBox { enabled, "Web Fonts" },
        {172, 14, 188, 200}, StaticText { disabled, "Developer" },
        {188, 30, 204, 240}, CheckBox { enabled, "Mill Plug-In SDK" },
        {208, 14, 224, 200}, StaticText { disabled, "Install Location" },
        {206, 330, 226, 426}, Button { enabled, "Choose..." },
        {226, 14, 242, 426}, StaticText { disabled, "" },
        {242, 14, 258, 426}, StaticText { disabled, "" },
        {258, 14, 274, 426}, StaticText { disabled, "" },
        {274, 14, 290, 426}, StaticText { disabled, "" },
        {294, 14, 310, 240}, StaticText { disabled, "" }
    }
};

resource 'ALRT' (140) {
    {70, 96, 170, 416}, 140, { OK, visible, sound1, OK, visible, sound1, OK, visible, sound1, OK, visible, sound1 }, noAutoCenter
};
resource 'DITL' (140) {
    { {70, 236, 90, 306}, Button { enabled, "OK" }, {10, 70, 60, 306}, StaticText { disabled, "^0" } }
};
resource 'ALRT' (141) {
    {70, 96, 170, 416}, 141, { OK, visible, sound1, OK, visible, sound1, OK, visible, sound1, OK, visible, sound1 }, noAutoCenter
};
resource 'DITL' (141) {
    { {70, 236, 90, 306}, Button { enabled, "OK" }, {10, 70, 60, 306}, StaticText { disabled, "Mill is installed." } }
};
