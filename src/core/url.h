/* URLs Mill can open (http:, https: through a proxy, file:, and its own mill: pages):
   splitting, and resolving links against the page they're on. */
#ifndef MILL_URL_H
#define MILL_URL_H

enum { SCHEME_HTTP, SCHEME_FILE, SCHEME_MILL, SCHEME_HTTPS };

typedef struct {
    char scheme;
    char user[64];      /* from user:password@host, decoded; empty if not given */
    char pass[64];
    char host[128];     /* empty for file:; the page's name for mill: (mill://about) */
    unsigned short port;    /* 80 for http:, 443 for https: unless given */
    char path[256];   /* starts with '/', includes any query; no fragment */
    char fragment[64];
} Url;

/* Parse an absolute URL (no scheme means http). Returns 0, or -1 if unusable. */
int url_parse(const char *s, Url *u);
/* Resolve href against base into out (an absolute URL string). Returns 0, or -1 for links
   Mill can't follow (mailto:, ftp:, ...). */
int url_resolve(const char *base, const char *href, char *out, int out_size);
void url_format(const Url *u, char *out, int out_size);
/* http: or https:, the web's own kinds (as against file: and mill:) */
int url_is_web(int scheme);
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

#endif
