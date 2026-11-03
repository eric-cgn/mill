#include "Processes.r"

/* About 1.5 MB is what a 4 MB Mac Plus on System 7.5.5 can give an application */
resource 'SIZE' (-1) {
    reserved, acceptSuspendResumeEvents, reserved, canBackground, doesActivateOnFGSwitch,
    backgroundAndForeground, dontGetFrontClicks, ignoreChildDiedEvents, is32BitCompatible,
    notHighLevelEventAware, onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
    reserved, reserved, reserved,
    1536 * 1024,
    1024 * 1024
};

#include "Finder.r"
#include "icons.r"
#include "splash.r"
#include "pages.r"
#include "version.h"

/* What the Finder's Get Info shows */
resource 'vers' (1) {
    MILL_VERS_MAJOR, MILL_VERS_MINOR, release, 0x00, verUS,
    MILL_VERSION,
    MILL_VERSION ", \$A9 " MILL_YEAR " " MILL_PUBLISHER ". Freeware."
};

/* Mill's Finder icon: the windmill */
type 'MiLL' { pstring; };
resource 'MiLL' (0, "Owner resource") { "Mill " MILL_VERSION " (Mill)" };
resource 'FREF' (128) { 'APPL', 0, "" };
resource 'BNDL' (128) {
    'MiLL', 0,
    { 'ICN#', { 0, 128 }; 'ics#', { 0, 128 }; 'FREF', { 0, 128 } }
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
