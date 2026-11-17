/* URLs Mill can open (http:, https: through a proxy, file:, its own mill: pages, and
   the schemes its Plug-Ins add): splitting, and resolving links against the page
   they're on. */
#ifndef MILL_URL_H
#define MILL_URL_H

#include <stddef.h>

enum { SCHEME_HTTP, SCHEME_FILE, SCHEME_MILL, SCHEME_HTTPS, SCHEME_OTHER };

typedef struct {
    char scheme;
    char name[12];      /* SCHEME_OTHER's name, lower case: "gopher" */
    char user[64];      /* from user:password@host, decoded; empty if not given */
    char pass[64];
    char host[128];     /* empty for file:; the page's name for mill: (mill://about) */
    unsigned short port;    /* 0 if not given (see url_port) */
    char path[256];   /* starts with '/', includes any query; no fragment */
    char fragment[64];
} Url;

/* The Plug-In schemes Mill knows besides its own, separated by spaces ("gopher ftp");
   the string is kept, not copied */
void url_schemes(const char *names);
/* Parse an absolute URL (no scheme means http). Returns 0, or -1 if unusable (a scheme
   Mill doesn't know, or a port that isn't 1 to 65535). */
int url_parse(const char *s, Url *u);
/* Resolve href against base into out (an absolute URL string). Returns 0, or -1 for links
   Mill can't follow (mailto:, ftp:, ...). */
int url_resolve(const char *base, const char *href, char *out, int out_size);
void url_format(const Url *u, char *out, int out_size);
/* If s starts with a scheme and "://" (any scheme, known or not), that prefix's length;
   0 if not. A "://" later on, in the path, query or fragment, doesn't count. */
int url_has_scheme(const char *s);
/* http: or https:, the web's own kinds (as against file: and mill:) */
int url_is_web(int scheme);
/* The port to connect to: the one given, else 80 for http: and 443 for https:; 0 for
   other schemes with none given (their Plug-In picks) */
unsigned short url_port(const Url *u);
/* Whether host is in list (separated by spaces or commas): a domain includes its
   subdomains ("example.com" and ".example.com" both include www.example.com, in any
   case); an IP address only matches itself; a range (192.168.0.0/16) matches the
   addresses in it. */
int url_host_in(const char *host, const char *list);
/* Take any user:password@ out of an address, in place */
void url_strip_login(char *url);
/* url with user and password put in as user:password@ (each %-encoded as needed),
   replacing any already there, into out. Returns 0, or -1 if it won't fit. */
int url_with_login(const char *url, const char *user, const char *pass, char *out, int out_size);
/* The Referer for a request for target made from the page at page (RFC 1945 10.13): the
   page's address without any user:password@ or #fragment, into out. Returns 1, or 0 when
   none should go: page isn't a whole address Mill knows, it won't fit, or page is https:
   and target isn't (RFC 2616 15.1.3). */
int url_referer(const char *page, const char *target, char *out, int out_size);
/* How an address is opened, for url_scroll_only */
enum { URL_OPEN_LINK, URL_OPEN_HISTORY, URL_OPEN_RELOAD };
/* Whether opening url while the page at shown is showing only scrolls that page, not asking
   for it again: a link to a #name on it, or Back and Forward between two places on it.
   Reload always asks again. */
int url_scroll_only(const char *url, const char *shown, int how);

/* Members of archives, named after "#/": http://host/kit.zip#/docs/page.html#part2.
   The document's part of url: up to the first '#', or for a member up to the second
   (a change there is a new document; after it, only a scroll) */
size_t url_doc_len(const char *url);
/* The anchor to scroll to: after the '#' (a member's: after the second), or "" */
const char *url_anchor(const char *url);
/* The member path after "#/", %-decoded, without the anchor, into path: 1, or 0 (path
   "") if url names no member. A relative link in a member resolves against this path
   (url_resolve), not stopping above the archive's top. */
int url_member(const char *url, char *path, int size);
/* A member path for an address: %XX for what can't go in it as it is; its length, or -1 */
int url_member_encode(const char *path, char *out, int size);

#endif
