/* What a file Mill saves becomes on the Mac: its type and creator, from rules the
   settings keep, and MacBinary, which carries a Mac file's two forks and Finder info
   over the web. */
#ifndef MILL_FILETYPES_H
#define MILL_FILETYPES_H

/* One rule on each line: "MIME-type file-name TYPE/CREA". A MIME type may be * or end
   in a slash and *, a file name may be * or start with *, and TYPE and CREA are four characters
   each (spaces count). The first rule that fits wins. */
extern const char FILETYPES_DEFAULT[];

/* Check one rule; returns 0, or -1 if it isn't one */
int filetype_rule_ok(const char *line, long len);
/* The type and creator for a file of the given MIME type (may be "") and name; rules
   are separated by line breaks or semicolons. Returns 1 if a rule fit, else 0 and
   BINA/????. */
int filetype_find(const char *rules, const char *mime, const char *name, char type[4], char creator[4]);

/* A name to save a URL's file as: its last path segment, %-decoded, without a
   MacBinary .bin, and short enough for the Mac (31 characters) */
void filetype_name(const char *url, char *out, int size);

typedef struct {
    char name[64];
    char type[4], creator[4];
    unsigned short flags;           /* Finder flags, less ones that only meant something on the sender's Mac */
    long data_at, data_len;         /* where each fork is in the MacBinary file */
    long rsrc_at, rsrc_len;
} MacBinary;

/* Read a MacBinary I, II or III header (128 bytes); returns 0, or -1 if it isn't one.
   II and III are known by their header's CRC; I by its zero bytes and lengths. */
int macbinary_header(const unsigned char *h, MacBinary *mb);

#endif
