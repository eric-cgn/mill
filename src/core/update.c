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

/* A MacBinary file to download: http:// or https://, ending in .bin, and nothing that
   could end an attribute or a line */
static int is_link(const char *s)
{
    const char *p;
    long n = strlen(s);
    if (strncmp(s, "http://", 7) && strncmp(s, "https://", 8)) return 0;
    if (n < 5 || s[n - 4] != '.' || (s[n - 3] | 32) != 'b' || (s[n - 2] | 32) != 'i' || (s[n - 1] | 32) != 'n') return 0;
    for (p = s; *p; p++) if ((unsigned char)*p <= ' ' || *p == '"' || *p == '<' || *p == '>' || (unsigned char)*p >= 127) return 0;
    return 1;
}

int update_parse(const char *text, long len, Update *u)
{
    long i = 0;
    int line = 0;
    memset(u, 0, sizeof *u);
    strcpy(u->link, UPDATE_DEFAULT_LINK);
    if (len > UPDATE_MAX_BYTES) return -1;
    while (i < len) {
        long start = i, end, n;
        char buf[UPDATE_MAX_NOTE + 1];
        int k = 0;
        while (i < len && text[i] != '\r' && text[i] != '\n') i++;
        end = i;
        if (i < len && text[i] == '\r' && i + 1 < len && text[i + 1] == '\n') i++;
        if (i < len) i++;
        /* the line, without spaces at either end or anything unprintable */
        while (start < end && (text[start] == ' ' || text[start] == '\t')) start++;
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) end--;
        if (start == end) continue;     /* blank */
        for (n = start; n < end && k < UPDATE_MAX_NOTE; n++) {
            unsigned char c = (unsigned char)text[n];
            if (c < ' ' || c == 127) continue;
            buf[k++] = (char)(c >= 0x80 ? latin1_to_mac(c) : c);
        }
        buf[k] = 0;
        if (line == 0) {
            if (strlen(buf) >= sizeof u->version || !is_version(buf)) return -1;
            strcpy(u->version, buf);
        } else if (line == 1) {
            if (strlen(buf) < sizeof u->link && is_link(buf)) strcpy(u->link, buf);
        } else if (u->nnotes < UPDATE_MAX_NOTES) {
            strcpy(u->notes[u->nnotes++], buf);
        }
        line++;
    }
    return line > 0 ? 0 : -1;
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
