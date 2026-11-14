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

resource 'DLOG' (128) {
    {50, 76, 270, 436}, dBoxProc, invisible, noGoAway, 0, 128, "", noAutoCenter
};
resource 'DITL' (128) {
    {
        {184, 276, 206, 346}, Button { enabled, "Install" },
        {184, 190, 206, 260}, Button { enabled, "Quit" },
        {52, 20, 70, 200}, RadioButton { enabled, "Easy Install" },
        {74, 20, 92, 200}, RadioButton { enabled, "Custom Install" },
        {102, 44, 120, 220}, CheckBox { enabled, "Mill" },
        {122, 44, 140, 220}, CheckBox { enabled, "Internet Services" },
        {142, 44, 160, 220}, CheckBox { enabled, "Web Fonts" },
        {162, 44, 180, 240}, CheckBox { enabled, "Mill Plug-In SDK" },
        {188, 20, 204, 186}, StaticText { disabled, "" },
        {180, 272, 210, 350}, UserItem { disabled },
        {14, 20, 38, 340}, StaticText { disabled, "Install Mill " MILL_VERSION }
    }
};

resource 'ALRT' (129) {
    {70, 96, 170, 416}, 129, { OK, visible, sound1, OK, visible, sound1, OK, visible, sound1, OK, visible, sound1 }, noAutoCenter
};
resource 'DITL' (129) {
    { {70, 236, 90, 306}, Button { enabled, "OK" }, {10, 70, 60, 306}, StaticText { disabled, "^0" } }
};
resource 'ALRT' (130) {
    {70, 96, 170, 416}, 130, { OK, visible, sound1, OK, visible, sound1, OK, visible, sound1, OK, visible, sound1 }, noAutoCenter
};
resource 'DITL' (130) {
    { {70, 236, 90, 306}, Button { enabled, "OK" }, {10, 70, 60, 306}, StaticText { disabled, "Mill is installed." } }
};
