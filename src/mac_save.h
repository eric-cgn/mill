/* Saving what Mill fetches to a file, as it arrives: as is, with a type and creator, or,
   for MacBinary, as the Mac file it carries. A download goes to "part." and its name
   (core/resume.h) until it's complete, and stays there if it stops, to go on from later. */
#ifndef MILL_SAVE_H
#define MILL_SAVE_H

#include <Multiverse.h>

#include "core/filetypes.h"

typedef struct {
    FSSpec spec;
    FSSpec part;                    /* where it's written: "part." and the name, or spec */
    Boolean in_part;                /* written to the part file, renamed once complete */
    char type[4], creator[4];       /* for a file that isn't MacBinary */
    Boolean decode;                 /* MacBinary becomes the file it carries */
    Boolean decided, macbinary;
    Boolean forks;                  /* saver_begin_forks: mb is given, not read */
    unsigned char head[128];        /* the first bytes, until it's known whether they're MacBinary */
    short nhead;
    MacBinary mb;
    short df, rf;                   /* open forks, or 0 */
    long pos;                       /* bytes seen */
    OSErr err;
} Saver;

/* in_part: a download, written to the part file (written over if it's there) */
void saver_begin(Saver *s, const FSSpec *spec, const char type[4], const char creator[4], Boolean decode,
                 Boolean in_part);
/* A download going on in its part file, from byte at of what's fetched: as it came, or
   decoded from MacBinary (mb, read again from the stream's first 128 bytes) */
OSErr saver_resume(Saver *s, const FSSpec *spec, Boolean decoded, const MacBinary *mb, long at);
/* The part file for spec */
void saver_part_spec(const FSSpec *spec, FSSpec *part);
/* A Mac file whose forks come one after the other (an archive's member): the data fork's
   data_len bytes, then the resource fork's rsrc_len, with mb's type, creator, and flags
   (data_at 0, rsrc_at data_len) */
void saver_begin_forks(Saver *s, const FSSpec *spec, const MacBinary *mb);
OSErr saver_write(Saver *s, const char *data, long len);
/* Finish the file: a part file becomes the name, replacing a file there. On an error, a
   part file stays; any other is deleted. */
OSErr saver_end(Saver *s);
/* Stop: a part file stays, any other is deleted */
void saver_abort(Saver *s);

#endif
