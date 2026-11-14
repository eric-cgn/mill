/* Mill's version, for the splash screen, the User-Agent, and the 'vers' resource */
#define MILL_VERSION "2.0"
#define MILL_VERS_MAJOR 0x02  /* BCD, for 'vers' */
#define MILL_VERS_MINOR 0x00  /* 0.0: minor 0, fix 0 */
#define MILL_PUBLISHER "OneInAMillion Software"
#define MILL_PUBLISHER_SITE "http://1in1e6.com/"
#define MILL_YEAR "2026"

/* Where Check for updates looks (see core/update.h). A test copy can look elsewhere:
   MILL_UPDATE_URL=http://... ./build.sh */
/* Mill's site: the change log (About links to it) */
#define MILL_SITE "http://mill.1in1e6.com/"
#ifndef MILL_UPDATE_URL
#define MILL_UPDATE_URL "http://mill.1in1e6.com/latest.txt"
#endif
