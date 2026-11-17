/* newsutil.h: the parts of the News Plug-In that need no Mac: reading NNTP's replies
   (RFC 977) and XOVER's lines, and matching group names. Plain C, so tests/test_news.c
   runs them on a host. */
#ifndef NEWSUTIL_H
#define NEWSUTIL_H

/* A reply's three-digit code (RFC 977 2.4.2), or 0 if line doesn't start with one */
short news_code(const char *line);

/* A line of LIST's reply, "group last first p" (RFC 977 3.6.1), taken apart in place:
   1, or 0 if it isn't one */
typedef struct {
    char *name;
    long first, last;
    char flag;          /* y, n, m, ... */
} NewsGroup;
short news_group(char *line, NewsGroup *g);

/* A line of XOVER's reply, taken apart in place at its tabs: number, Subject, From, Date,
   Message-ID, References, bytes, Lines. 1, or 0 if it has too few fields. */
typedef struct {
    long number, lines;
    char *subject, *from, *date, *id, *refs;
} NewsOver;
short news_over(char *line, NewsOver *o);

/* Whether name matches pattern, in any case: with * or ?, as a wildmat (RFC 977 3.10);
   without, anywhere in name */
short news_match(const char *name, const char *pattern);

/* A line of a multi-line reply as sent (RFC 977 2.4.1): -1 if it's the "." that ends it;
   else the length of the line once a doubled leading dot is taken off (in place) */
long news_unstuff(char *line, long len);

/* The next Message-ID in a References header from *p ("<...>"), into out without its
   brackets: its length, or 0 when there are no more */
long news_ref(const char **p, char *out, long size);

#endif
