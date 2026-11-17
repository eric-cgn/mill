/* newsutil.c: see newsutil.h */
#include <string.h>

#include "newsutil.h"

static long num(const char **p)
{
    long n = 0;
    while (**p == ' ' || **p == '\t') (*p)++;
    while (**p >= '0' && **p <= '9') n = n * 10 + (*(*p)++ - '0');
    return n;
}

short news_code(const char *line)
{
    if (line[0] < '1' || line[0] > '5' || line[1] < '0' || line[1] > '9' || line[2] < '0' || line[2] > '9') return 0;
    if (line[3] && line[3] != ' ') return 0;
    return (short)((line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0'));
}

short news_group(char *line, NewsGroup *g)
{
    char *sp = strchr(line, ' ');
    const char *p;
    if (!sp || sp == line) return 0;
    *sp = 0;
    p = sp + 1;
    g->name = line;
    g->last = num(&p);
    g->first = num(&p);
    while (*p == ' ') p++;
    g->flag = *p ? *p : 'y';
    return 1;
}

short news_over(char *line, NewsOver *o)
{
    char *f[8];
    short n = 0;
    const char *p;
    f[0] = line;
    while (n < 7 && (line = strchr(line, '\t')) != NULL) { *line++ = 0; f[++n] = line; }
    if (n < 4) return 0;
    if ((line = strchr(f[n], '\t')) != NULL) *line = 0;    /* Xref and the rest */
    p = f[0];
    o->number = num(&p);
    o->subject = f[1];
    o->from = f[2];
    o->date = f[3];
    o->id = f[4];
    o->refs = n >= 5 ? f[5] : f[n] + strlen(f[n]);
    o->lines = 0;
    if (n >= 7) { p = f[7]; o->lines = num(&p); }
    return o->number > 0;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

/* RFC 977 3.10 (wildmat): * any run, ? any one character */
static short wild(const char *s, const char *p)
{
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (p[1] == '*') p++;
            if (!p[1]) return 1;
            for (; *s; s++) if (wild(s, p + 1)) return 1;
            return 0;
        }
        if (!*s || (*p != '?' && lower(*p) != lower(*s))) return 0;
    }
    return !*s;
}

short news_match(const char *name, const char *pattern)
{
    long n = strlen(pattern), i;
    if (strpbrk(pattern, "*?")) return wild(name, pattern);
    for (; *name; name++) {
        for (i = 0; i < n && name[i] && lower(name[i]) == lower(pattern[i]); i++) {}
        if (i == n) return 1;
    }
    return n == 0;
}

long news_unstuff(char *line, long len)
{
    if (len == 1 && line[0] == '.') return -1;
    if (len >= 2 && line[0] == '.' && line[1] == '.') { memmove(line, line + 1, len - 1); len--; }
    return len;
}

long news_ref(const char **p, char *out, long size)
{
    const char *a = strchr(*p, '<'), *b, *p2;
    long n;
    if (!a || !(b = strchr(a, '>'))) return 0;
    while ((p2 = memchr(a + 1, '<', b - a - 1)) != NULL) a = p2;     /* "<bad <id>": the id */
    n = b - a - 1;
    *p = b + 1;
    if (n <= 0 || n >= size) return news_ref(p, out, size);
    memcpy(out, a + 1, n);
    out[n] = 0;
    return n;
}
