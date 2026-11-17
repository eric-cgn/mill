/* Going on with a download that stopped: no record is kept, only the part file itself,
   "part." and the name, which becomes the name once the download is complete. */
#ifndef MILL_RESUME_H
#define MILL_RESUME_H

#include "filetypes.h"

/* The part file's name for name (a Mac name, up to 31 characters): "part." and name,
   cut to 26 characters; out holds 32 */
void part_name(const char *name, char *out);

/* What Save As does with the name chosen: RESUME_FRESH when there's no part file,
   RESUME_ASK (Cancel, Replace, Resume) when there is and the protocol can seek, else
   RESUME_REPLACE: it's written over */
enum { RESUME_FRESH, RESUME_ASK, RESUME_REPLACE };
int resume_ask(int part_exists, int can_seek);

/* Where the download goes on from: the byte of the stream after what the part file
   holds (dlen and rlen bytes in its forks; type and creator its Finder's). head is the
   stream's first 128 bytes, fetched again, or NULL when MacBinary isn't decoded: if
   they're MacBinary and the part file has its type and creator, it holds the forks
   (*mb, *decoded set), and the data fork is padded to 128 in the stream. -1 if the
   part file doesn't fit what the stream says: start again. */
long resume_at(const unsigned char *head, const char type[4], const char creator[4], long dlen, long rlen,
               MacBinary *mb, int *decoded);

/* Whether a reply's status lets the part be appended: 206; anything else (200, the
   whole; 416, nothing) starts again */
int resume_appends(int code);

#endif
