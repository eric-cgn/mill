/* Mill starts in two parts: mac_start.c, in the first code segment, puts up the splash;
   then the rest of Mill loads and runs. */
#ifndef MILL_START_H
#define MILL_START_H

#include <Multiverse.h>

/* The browser, once the Toolbox is started; it closes the splash when it's ready, then
   unloads this segment, so it never returns (ExitToShell) once it's running */
int mill_run(WindowPtr splash);
int main(void);

#endif
