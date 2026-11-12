/* Mill's version, for the splash screen, the User-Agent, and the 'vers' resource */
#define MILL_VERSION "1.9.2"
#define MILL_VERS_MAJOR 0x01  /* BCD, for 'vers' */
#define MILL_VERS_MINOR 0x92  /* 9.2: minor 9, fix 2 */
#define MILL_PUBLISHER "OneInAMillion Software"
#define MILL_YEAR "2026"

/* Where Check for updates looks (see core/update.h). A test copy can look elsewhere:
   MILL_UPDATE_URL=http://... ./build.sh */
#ifndef MILL_UPDATE_URL
#define MILL_UPDATE_URL "http://mill.1in1e6.com/latest.txt"
#endif
