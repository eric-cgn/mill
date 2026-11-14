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

/* One window, 440 by 256, a movable modal dialog in the middle of the screen, for every stage; the program draws
   what's in it (the text box, the pop-up menu, the list) over a user item, around the buttons */
resource 'DLOG' (128) {
    {62, 36, 318, 476}, noGrowDocProc, invisible, noGoAway, 0, 128, "Install Mill " MILL_VERSION, noAutoCenter
};
resource 'DITL' (128) {
    {
        {222, 348, 244, 428}, Button { enabled, "Continue" },
        {222, 258, 244, 338}, Button { enabled, "Quit" },
        {216, 336, 236, 420}, Button { enabled, "Choose" },
        {0, 0, 256, 440}, UserItem { enabled }
    }
};
