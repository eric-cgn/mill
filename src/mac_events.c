/* System 7's Apple Events: the required four, and Get URL, which Internet Config sends the
   web browser it's set to. In a code segment of its own, loaded only where sys_init finds
   the Apple Event Manager, and never unloaded: its handlers stay installed. */
#include <Multiverse.h>

#include "mac_local.h"
#include "mac_sys.h"

enum { AE_LAUNCH, AE_OPEN, AE_URL, AE_QUIT };
#define GET_URL 'GURL'  /* Get URL's class and ID, both: Internet Config's kInternetEventClass, kAEGetURL */

static pascal OSErr handle(const AppleEvent *ev, AppleEvent *reply, int32_t what)
{
    AEDescList list;
    AEKeyword key;
    DescType type;
    Size size;
    long i, n;
    FSSpec spec;
    FInfo fi;
    char url[400];
    OSErr err = noErr;
    switch (what) {
    case AE_OPEN:
        /* each file as Open File opens it; Mill's own Plug-Ins and Web Fonts only start it */
        if ((err = AEGetParamDesc((AppleEvent *)ev, keyDirectObject, typeAEList, &list)) != noErr) break;
        if ((err = AECountItems(&list, &n)) == noErr)
            for (i = 1; i <= n; i++)
                if (AEGetNthPtr(&list, i, typeFSS, &key, &type, &spec, sizeof spec, &size) == noErr &&
                    file_get_info(&spec, &fi) == noErr && fi.fdType != 'FFIL' && fi.fdType != 'MPlg') {
                    spec_to_url(&spec, url, sizeof url);
                    ae_go(url);
                }
        AEDisposeDesc(&list);
        break;
    case AE_URL:
        /* an address, as Open Location goes to it */
        if ((err = AEGetParamPtr((AppleEvent *)ev, keyDirectObject, typeChar, &type, url, sizeof url - 1, &size)) != noErr) break;
        url[size < (Size)sizeof url ? size : (Size)sizeof url - 1] = 0;
        ae_go(url);
        break;
    case AE_QUIT:
        /* as Quit: a changed form asks first; Cancel stops a Shut Down */
        if (!ae_quit()) err = userCanceledErr;
        break;
    }
    return err;
}

void ae_install(void)
{
    /* Print Documents opens them, as Open Documents does */
    static const struct { AEEventClass c; AEEventID id; short what; } events[] = {
        { kCoreEventClass, kAEOpenApplication, AE_LAUNCH },
        { kCoreEventClass, kAEOpenDocuments, AE_OPEN },
        { kCoreEventClass, kAEPrintDocuments, AE_OPEN },
        { kCoreEventClass, kAEQuitApplication, AE_QUIT },
        { GET_URL, GET_URL, AE_URL },
    };
    short i;
    for (i = 0; i < 5; i++)
        AEInstallEventHandler(events[i].c, events[i].id, (AEEventHandlerUPP)handle, events[i].what, false);
}
