/* A small printf for Plug-Ins: every argument is a long (or a pointer as a long), so
   code built by any compiler passes them the same way. */
#ifndef MILL_FMT_H
#define MILL_FMT_H

/* %d %u %x %s %c %% (an l before d, u, or x is allowed), with - and 0 flags, a width,
   and a precision for %s (.* takes it from the arguments, before the string). Writes
   at most size - 1 characters and a 0 (size > 0), and
   returns the length the whole would have. */
long mill_format(char *out, long size, const char *fmt, const long *args);

#endif
