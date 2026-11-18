/* Client pull: Refresh's value, from the header or the page's META tag (refresh.h) */
#include <string.h>
#include "refresh.h"

static int lower(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

int refresh_parse(const char *s, long n, long *secs, char *url, int size)
{
    long i = 0, v = 0, e;
    int digits = 0;
    url[0] = 0;
    while (i < n && space(s[i])) i++;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        if (v < 100000L) v = v * 10 + (s[i] - '0');
        i++;
        digits++;
    }
    if (!digits) return 0;
    while (i < n && (s[i] == '.' || (s[i] >= '0' && s[i] <= '9'))) i++;     /* "2.5": 2 */
    while (i < n && space(s[i])) i++;
    if (i < n && s[i] != ';' && s[i] != ',') return 0;
    if (i < n) i++;
    while (i < n && space(s[i])) i++;
    if (n - i >= 3 && lower(s[i]) == 'u' && lower(s[i + 1]) == 'r' && lower(s[i + 2]) == 'l') {
        long j = i + 3;
        while (j < n && space(s[j])) j++;
        if (j < n && s[j] == '=') {
            i = j + 1;
            while (i < n && space(s[i])) i++;
        }
    }
    if (i < n && (s[i] == '"' || s[i] == '\'')) i++;
    for (e = n; e > i && (space(s[e - 1]) || s[e - 1] == '"' || s[e - 1] == '\''); e--) {}
    if (e - i >= size) e = i + size - 1;
    memcpy(url, s + i, e - i);
    url[e - i] = 0;
    *secs = v;
    return 1;
}

const char *refresh_meta(const Doc *doc)
{
    static const char want[] = "refresh";
    long i;
    for (i = 0; i < doc->nnodes; i++) {
        const char *eq;
        int k;
        if (doc->nodes[i].tag != TAG_META || !(eq = html_attr(doc, &doc->nodes[i], "http-equiv"))) continue;
        for (k = 0; want[k] && lower(eq[k]) == want[k]; k++) {}
        if (!want[k] && !eq[k]) return html_attr(doc, &doc->nodes[i], "content");
    }
    return NULL;
}
