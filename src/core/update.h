/* Check for updates: the file that says what the newest Mill is, and which is newer.
   The file (latest.txt) is plain text, Latin-1, lines ending in CR, LF, or CR LF:
     line 1   the newest version, N.N or N.N.N
     line 2   its MacBinary file, an http:// (or https://) address ending in .bin, so
              a click downloads it
     line 3+  what's new in it, one change log entry a line (none is fine)
   Blank lines are skipped. */
#ifndef MILL_UPDATE_H
#define MILL_UPDATE_H

#define UPDATE_MAX_BYTES 4096       /* a longer file isn't read */
#define UPDATE_MAX_NOTES 20         /* change lines past this are left out */
#define UPDATE_MAX_NOTE 200         /* and each is cut to this many characters */
#define UPDATE_DEFAULT_LINK "http://mill.1in1e6.com/"

typedef struct {
    char version[16];
    char link[256];                 /* UPDATE_DEFAULT_LINK (the site) if line 2 isn't a .bin */
    char notes[UPDATE_MAX_NOTES][UPDATE_MAX_NOTE + 1];   /* in Mac Roman */
    int nnotes;
} Update;

/* Read latest.txt's len bytes. Returns 0, or -1 if it's too long or its first line
   isn't a version. */
int update_parse(const char *text, long len, Update *u);
/* Versions compared part by part as numbers (1.10 is after 1.9, and 1.9.1 after 1.9);
   one with more after its numbers (1.9b2) comes before the same numbers alone.
   Negative, 0, or positive, as a is older than, the same as, or newer than b. */
int update_compare(const char *a, const char *b);

#endif
