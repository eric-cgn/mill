#include <string.h>

#include "charset.h"
#include "update.h"

/* N.N or N.N.N, each part one to four digits */
static int is_version(const char *s)
{
    int parts = 0, digits = 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            if (++digits > 4) return 0;
        } else if (*s == '.' || !*s) {
            if (!digits) return 0;
            parts++;
            digits = 0;
            if (!*s) break;
        } else {
            return 0;
        }
    }
    return parts == 2 || parts == 3;
}

int update_site(const char *url, char *out, int size)
{
    const char *host = strstr(url, "://"), *end;
    if (!host || host == url || !host[3] || host[3] == '/') return -1;
    end = strchr(host + 3, '/');
    if (!end) end = host + strlen(host);
    if (end - url + 2 > size) return -1;
    memcpy(out, url, end - url);
    out[end - url] = '/';
    out[end - url + 1] = 0;
    return 0;
}

/* An address on site (scheme and host in any case), with nothing that could end an
   attribute or a line */
static int is_link(const char *s, const char *site)
{
    const char *p;
    long n = strlen(site);
    if (!n || (long)strlen(s) < n) return 0;
    for (p = site; *p; p++, s++) if ((*s | 32) != (*p | 32)) return 0;
    for (s -= n; *s; s++) if ((unsigned char)*s <= ' ' || *s == '"' || *s == '<' || *s == '>' || (unsigned char)*s >= 127) return 0;
    return 1;
}

int update_parse(const char *text, long len, const char *site, Update *u)
{
    long i = 0;
    int line = 0;
    memset(u, 0, sizeof *u);
    if (len > UPDATE_MAX_BYTES) return UPDATE_NOT_FILE;
    while (i < len) {
        long start = i, end, n;
        char buf[UPDATE_MAX_NOTE + 1];
        int k = 0, cut = 0;
        while (i < len && text[i] != '\r' && text[i] != '\n') i++;
        end = i;
        if (i < len && text[i] == '\r' && i + 1 < len && text[i + 1] == '\n') i++;
        if (i < len) i++;
        /* the line, without spaces at either end or anything unprintable */
        while (start < end && (text[start] == ' ' || text[start] == '\t')) start++;
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) end--;
        if (start == end) continue;     /* blank */
        for (n = start; n < end; n++) {
            unsigned char c = (unsigned char)text[n];
            if (c < ' ' || c == 127) continue;
            if (k == UPDATE_MAX_NOTE) { cut = 1; break; }
            buf[k++] = (char)(c >= 0x80 ? latin1_to_mac(c) : c);
        }
        buf[k] = 0;
        if (line == 0) {
            if (cut || strlen(buf) >= sizeof u->version || !is_version(buf)) return UPDATE_NOT_FILE;
            strcpy(u->version, buf);
        } else if (line == 1) {
            /* the whole address, as it was sent, or none */
            if (cut || strlen(buf) >= sizeof u->link || !is_link(buf, site)) return UPDATE_OFF_SITE;
            strcpy(u->link, buf);
        } else if (u->nnotes < UPDATE_MAX_NOTES) {
            strcpy(u->notes[u->nnotes++], buf);
        }
        line++;
    }
    return line == 0 ? UPDATE_NOT_FILE : line == 1 ? UPDATE_OFF_SITE : 0;
}

/* The next number in a version, past its dot; -1 when there are no more */
static long part(const char **s)
{
    long n = 0;
    if (**s < '0' || **s > '9') return -1;
    while (**s >= '0' && **s <= '9') n = n * 10 + (*(*s)++ - '0');
    if (**s == '.' && (*s)[1] >= '0' && (*s)[1] <= '9') (*s)++;
    return n;
}

int update_compare(const char *a, const char *b)
{
    for (;;) {
        long x = part(&a), y = part(&b);
        if (x < 0 && y < 0) break;
        if (x < 0) x = 0;   /* 1.9 is 1.9.0 */
        if (y < 0) y = 0;
        if (x != y) return x < y ? -1 : 1;
    }
    /* the same numbers: a pre-release (anything after them) comes first */
    if (!*a != !*b) return *a ? -1 : 1;
    return strcmp(a, b) < 0 ? -1 : strcmp(a, b) > 0;
}
