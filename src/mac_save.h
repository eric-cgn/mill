/* Saving what Mill fetches to a file, as it arrives: as is, with a type and creator, or,
   for MacBinary, as the Mac file it carries. */
#ifndef MILL_SAVE_H
#define MILL_SAVE_H

#include <Multiverse.h>

#include "core/filetypes.h"

typedef struct {
    FSSpec spec;
    char type[4], creator[4];       /* for a file that isn't MacBinary */
    Boolean decode;                 /* MacBinary becomes the file it carries */
    Boolean decided, macbinary;
    unsigned char head[128];        /* the first bytes, until it's known whether they're MacBinary */
    short nhead;
    MacBinary mb;
    short df, rf;                   /* open forks, or 0 */
    long pos;                       /* bytes seen */
    OSErr err;
} Saver;

void saver_begin(Saver *s, const FSSpec *spec, const char type[4], const char creator[4], Boolean decode);
OSErr saver_write(Saver *s, const char *data, long len);
/* Finish the file; on an error, it's deleted */
OSErr saver_end(Saver *s);
/* Stop and delete what was saved */
void saver_abort(Saver *s);

#endif
