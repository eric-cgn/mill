/* Check for updates: the file that says what the newest Mill is, and which is newer.
   The file (latest.txt) is plain text, Latin-1, lines ending in CR, LF, or CR LF:
     line 1   the newest version, N.N or N.N.N
     line 2   its MacBinary file, so a click downloads it: an address on the same site
              as latest.txt (http://mill.1in1e6.com/...), or the file isn't taken
     line 3+  what's new in it, one change log entry a line (none is fine)
   Blank lines are skipped. */
#ifndef MILL_UPDATE_H
#define MILL_UPDATE_H

#define UPDATE_MAX_BYTES 4096       /* a longer file isn't read */
#define UPDATE_MAX_NOTES 20         /* change lines past this are left out */
#define UPDATE_MAX_NOTE 200         /* and each is cut to this many characters */

/* What update_parse found wrong */
enum { UPDATE_NOT_FILE = -1, UPDATE_OFF_SITE = -2 };

typedef struct {
    char version[16];
    char link[256];
    char notes[UPDATE_MAX_NOTES][UPDATE_MAX_NOTE + 1];   /* in Mac Roman */
    int nnotes;
} Update;

/* The site an update file's address is on, which its download link must be on too:
   its scheme and host and a slash ("http://mill.1in1e6.com/"), into out. Returns 0, or
   -1 if url isn't a whole address or that won't fit. */
int update_site(const char *url, char *out, int size);
/* Read latest.txt's len bytes, whose download link must start with site. Returns 0;
   UPDATE_NOT_FILE if it's too long or its first line isn't a version; or UPDATE_OFF_SITE
   if its second line isn't an address on site. */
int update_parse(const char *text, long len, const char *site, Update *u);
/* Versions compared part by part as numbers (1.10 is after 1.9, and 1.9.1 after 1.9);
   one with more after its numbers (1.9b2) comes before the same numbers alone.
   Negative, 0, or positive, as a is older than, the same as, or newer than b. */
int update_compare(const char *a, const char *b);

#endif
