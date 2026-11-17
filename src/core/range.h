/* Parts of files (RFC 2068 14.16, 14.35, 14.36): what a request's Range asks for, the
   head of the reply a protocol gives it, and whether a reply can be read in parts. Mill
   answers FileHead with these, and the http Plug-In reads Range with range_asked. */
#ifndef MILL_RANGE_H
#define MILL_RANGE_H

/* A request's Range header (len bytes of request text): 1 with first and last (-1 for
   "to the end") for "bytes=first-last" or "bytes=first-", 0 if there's none, -1 for
   any other kind (several ranges, a suffix), which Mill never asks for */
int range_asked(const char *text, long len, long *first, long *last);

/* The bytes of a file of size bytes (-1: unknown) to send for a request: 1 for a part,
   first to last; 0 for the whole, 0 to size - 1 (to -1 if the size is unknown). The
   whole when there's no Range, one of another kind, an unknown size, or a first past
   the end (14.36.1: such a range is ignored); a last past the end is the end. */
int range_part(const char *text, long len, long size, long *first, long *last);

/* The head of that reply, for a file of type: "HTTP/1.0 206 Partial Content" with
   Content-Range, or 200; Content-Length and Accept-Ranges: bytes if the size is known;
   and the blank line. Its length, or -1 if it won't fit in room. */
long range_head(char *out, long room, const char *type, long size, int part, long first, long last);

/* Whether a reply's head (len bytes, through its blank line) says its file can be read
   in parts: 200 or 206, Accept-Ranges: bytes, and a Content-Length (for a 206, the
   whole's size from Content-Range); its size then in *size, else -1 */
int range_seekable(const char *head, long len, long *size);

#endif
