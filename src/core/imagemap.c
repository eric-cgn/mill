#include <stdlib.h>
#include <string.h>

#include "imagemap.h"

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int eq_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) if (lower((unsigned char)*a) != lower((unsigned char)*b)) return 0;
    return !*a && !*b;
}

long imagemap_for(const Doc *d, long img)
{
    const char *use = html_attr(d, &d->nodes[img], "usemap"), *hash;
    long i;
    if (!use) return -1;
    hash = strchr(use, '#');
    use = hash ? hash + 1 : use;
    if (!*use) return -1;
    for (i = 0; i < d->nnodes; i++) {
        const char *name;
        if (d->nodes[i].tag == TAG_MAP && (name = html_attr(d, &d->nodes[i], "name")) && eq_ci(name, use)) return i;
    }
    return -1;
}

/* COORDS: numbers separated by commas or spaces; how many were read */
static int coords(const char *s, long *v, int max)
{
    int n = 0;
    while (s && *s && n < max) {
        while (*s && !(*s >= '0' && *s <= '9') && *s != '-') s++;
        if (!*s) break;
        v[n++] = strtol(s, (char **)&s, 10);
        if (*s == '.') while (*s == '.' || (*s >= '0' && *s <= '9')) s++;  /* 12.5: just 12 */
    }
    return n;
}

static int in_poly(const long *v, int n, long x, long y)
{
    int i, j, in = 0;
    for (i = 0, j = n - 2; i < n; j = i, i += 2) {
        long xi = v[i], yi = v[i + 1], xj = v[j], yj = v[j + 1];
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
    }
    return in;
}

static long area_hit(const Doc *d, long node, short x, short y, long *def)
{
    const Node *n = &d->nodes[node];
    const char *shape = html_attr(d, n, "shape");
    long v[200];
    int k = coords(html_attr(d, n, "coords"), v, 200), hit = 0;
    if (shape && (lower(shape[0]) == 'd')) { if (*def < 0) *def = node; return -1; }  /* default: if nothing else */
    if (shape && lower(shape[0]) == 'c') {
        hit = k >= 3 && (x - v[0]) * (long)(x - v[0]) + (y - v[1]) * (long)(y - v[1]) <= v[2] * v[2];
    } else if (shape && lower(shape[0]) == 'p') {
        hit = k >= 6 && in_poly(v, k & ~1, x, y);
    } else if (k >= 4) {
        long x1 = v[0] < v[2] ? v[0] : v[2], x2 = v[0] < v[2] ? v[2] : v[0];
        long y1 = v[1] < v[3] ? v[1] : v[3], y2 = v[1] < v[3] ? v[3] : v[1];
        hit = x >= x1 && x <= x2 && y >= y1 && y <= y2;
    }
    return hit ? node : -1;
}

static long walk(const Doc *d, long node, short x, short y, long *def)
{
    long c, hit;
    for (c = d->nodes[node].first; c >= 0; c = d->nodes[c].next) {
        if (d->nodes[c].tag == TAG_AREA && (hit = area_hit(d, c, x, y, def)) >= 0) return hit;
        if (d->nodes[c].first >= 0 && (hit = walk(d, c, x, y, def)) >= 0) return hit;
    }
    return -1;
}

long imagemap_hit(const Doc *d, long map, short x, short y)
{
    long def = -1, hit = walk(d, map, x, y, &def);
    if (hit < 0) hit = def;
    if (hit < 0 || html_attr(d, &d->nodes[hit], "nohref") || !html_attr(d, &d->nodes[hit], "href")) return -1;
    return hit;
}
