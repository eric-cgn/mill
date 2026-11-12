#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "charset.h"
#include "html.h"

static const char *const TAG_NAMES[TAG_COUNT] = {
    "", "",
    "HTML", "HEAD", "TITLE", "BODY", "META", "SCRIPT", "STYLE",
    "P", "BR", "HR", "DIV", "CENTER", "BLOCKQUOTE", "PRE", "ADDRESS",
    "H1", "H2", "H3", "H4", "H5", "H6",
    "UL", "OL", "LI", "DL", "DT", "DD", "DIR", "MENU",
    "A", "B", "STRONG", "I", "EM", "U", "TT", "CODE", "KBD", "SAMP", "VAR",
    "CITE", "DFN", "BIG", "SMALL", "SUB", "SUP", "STRIKE", "FONT", "BASEFONT", "NOBR",
    "IMG", "MAP", "AREA",
    "TABLE", "CAPTION", "TR", "TD", "TH",
    "FORM", "INPUT", "SELECT", "OPTION", "TEXTAREA",
    "BASE", "LINK", "NOSCRIPT", "ISINDEX",
};

static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

int html_tag(const char *name, int len)
{
    int t, i;
    for (t = TAG_HTML; t < TAG_COUNT; t++) {
        const char *n = TAG_NAMES[t];
        for (i = 0; i < len && n[i] && upper(name[i]) == n[i]; i++) {}
        if (i == len && !n[i]) return t;
    }
    return TAG_UNKNOWN;
}

static int is_void(int t)
{
    switch (t) {
    case TAG_BR: case TAG_HR: case TAG_IMG: case TAG_META: case TAG_BASE: case TAG_LINK:
    case TAG_INPUT: case TAG_AREA: case TAG_BASEFONT: return 1;
    }
    return 0;
}

/* Starting one of these ends an open paragraph */
static int ends_p(int t)
{
    switch (t) {
    case TAG_P: case TAG_H1: case TAG_H2: case TAG_H3: case TAG_H4: case TAG_H5: case TAG_H6:
    case TAG_UL: case TAG_OL: case TAG_DL: case TAG_PRE: case TAG_DIV: case TAG_CENTER:
    case TAG_BLOCKQUOTE: case TAG_TABLE: case TAG_HR: case TAG_FORM: case TAG_ADDRESS: case TAG_DIR: case TAG_MENU:
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ growing buffers */

static int grow(void **p, long *cap, long need, long size)
{
    long n;
    void *q;
    if (need <= *cap) return 0;
    n = *cap ? *cap : 64;
    while (n < need) n *= 2;
    q = realloc(*p, n * size);
    if (!q) return -1;
    *p = q;
    *cap = n;
    return 0;
}

static long add_node(Doc *d, int tag, long parent)
{
    Node *n;
    long i = d->nnodes;
    if (grow((void **)&d->nodes, &d->cap_nodes, i + 1, sizeof(Node))) return -1;
    n = &d->nodes[i];
    n->tag = tag;
    n->parent = parent;
    n->first = n->last = n->next = -1;
    n->text = n->len = 0;
    n->attrs = -1;
    if (parent >= 0) {
        Node *p = &d->nodes[parent];
        if (p->last >= 0) d->nodes[p->last].next = i; else p->first = i;
        p->last = i;
    }
    d->nnodes++;
    return i;
}

static int add_text(Doc *d, const char *s, long len)
{
    if (grow((void **)&d->text, &d->cap_text, d->text_len + len, 1)) return -1;
    memcpy(d->text + d->text_len, s, len);
    d->text_len += len;
    return 0;
}

static int add_attr_bytes(Doc *d, const char *s, long len)
{
    if (grow((void **)&d->attrs, &d->cap_attrs, d->attrs_len + len, 1)) return -1;
    memcpy(d->attrs + d->attrs_len, s, len);
    d->attrs_len += len;
    return 0;
}

/* Decode entities and the page's charset in src[0..len) into Mac Roman in out (at most
   len bytes); returns length. */
static int page_charset;    /* CHARSET_MAC, CHARSET_LATIN1, or CHARSET_UTF8 */

static long decode(const char *src, long len, char *out)
{
    long i = 0, n = 0;
    while (i < len) {
        unsigned char c = (unsigned char)src[i];
        if (c == '&') {
            long j = i + 1;
            while (j < len && j - i < 10 && src[j] != ';' && src[j] != '&' && src[j] != ' ' && src[j] != '<') j++;
            if (j > i + 1) {
                unsigned char m = entity_to_mac(src + i + 1, (int)(j - i - 1));
                if (m) {
                    out[n++] = (char)m;
                    i = (j < len && src[j] == ';') ? j + 1 : j;
                    continue;
                }
            }
        }
        if (c >= 0x80 && page_charset == CHARSET_UTF8) {
            long u;
            int k = utf8_char((const unsigned char *)src + i, len - i, &u);
            if (k) {
                unsigned char m = unicode_to_mac(u);
                if (m) out[n++] = (char)m;
                i += k;
                continue;
            }
        }
        out[n++] = page_charset == CHARSET_MAC ? (char)c : (char)latin1_to_mac(c);
        i++;
    }
    return n;
}

/* ------------------------------------------------------------ the parser */

typedef struct {
    Doc *doc;
    long stack[64];
    int depth;
} Parser;

static long current(Parser *p) { return p->stack[p->depth - 1]; }

static int open_index(Parser *p, int tag)
{
    int i;
    for (i = p->depth - 1; i > 0; i--)
        if (p->doc->nodes[p->stack[i]].tag == tag) return i;
    return -1;
}

static void close_to(Parser *p, int index) { if (index > 0) p->depth = index; }

/* Close tag if it's open, but not past a containing element of the stop kind */
static void close_within(Parser *p, int tag, int stop1, int stop2)
{
    int i;
    for (i = p->depth - 1; i > 0; i--) {
        int t = p->doc->nodes[p->stack[i]].tag;
        if (t == tag) { p->depth = i; return; }
        if (t == stop1 || t == stop2) return;
    }
}

static int start_tag(Parser *p, int tag, const char *attrs, long attrs_len);
static void end_tag(Parser *p, int tag);

/* ISINDEX, as browsers drew it: a rule, the prompt, a text field, and a rule, in a form
   (with ISINDEX's ACTION, else this page) whose field is named isindex, so it sends just
   the words typed */
static int isindex(Parser *p, const char *attrs, long attrs_len)
{
    static const char dflt[] = "This is a searchable index. Enter search keywords: ";
    char prompt[256];
    const char *v;
    long form, node, n;
    if (start_tag(p, TAG_HR, "", 0) || start_tag(p, TAG_FORM, attrs, attrs_len)) return -1;
    form = p->doc->nnodes - 1;
    v = html_attr(p->doc, &p->doc->nodes[form], "prompt");
    snprintf(prompt, sizeof prompt, "%s", v ? v : dflt);
    n = strlen(prompt);
    if ((node = add_node(p->doc, TAG_TEXT, form)) < 0 || add_text(p->doc, prompt, n)) return -1;
    p->doc->nodes[node].text = p->doc->text_len - n;   /* only once the text is there */
    p->doc->nodes[node].len = n;
    if (start_tag(p, TAG_INPUT, "name=isindex", 12)) return -1;
    end_tag(p, TAG_FORM);
    return start_tag(p, TAG_HR, "", 0);
}

static int start_tag(Parser *p, int tag, const char *attrs, long attrs_len)
{
    long node;
    if (tag == TAG_HTML || tag == TAG_HEAD || tag == TAG_BODY) return 0;  /* structure is implied */
    if (tag == TAG_ISINDEX) return isindex(p, attrs, attrs_len);
    if (ends_p(tag)) close_within(p, TAG_P, TAG_TD, TAG_TH);
    switch (tag) {
    case TAG_LI: close_within(p, TAG_LI, TAG_UL, TAG_OL); break;
    case TAG_DT: case TAG_DD: close_within(p, TAG_DT, TAG_DL, -1); close_within(p, TAG_DD, TAG_DL, -1); break;
    case TAG_TR: close_within(p, TAG_TD, TAG_TABLE, -1); close_within(p, TAG_TH, TAG_TABLE, -1);
                 close_within(p, TAG_TR, TAG_TABLE, -1); break;
    case TAG_TD: case TAG_TH: close_within(p, TAG_TD, TAG_TABLE, -1); close_within(p, TAG_TH, TAG_TABLE, -1); break;
    case TAG_OPTION: close_within(p, TAG_OPTION, TAG_SELECT, -1); break;
    case TAG_A: close_within(p, TAG_A, -1, -1); break;  /* anchors don't nest */
    }
    if ((node = add_node(p->doc, tag, current(p))) < 0) return -1;
    if (attrs_len > 0) {
        /* name=value pairs; names lowercased, values decoded */
        long i = 0, start = p->doc->attrs_len;
        int any = 0;
        while (i < attrs_len) {
            long ns, ne, vs, ve;
            char q;
            while (i < attrs_len && (attrs[i] == ' ' || attrs[i] == '\t' || attrs[i] == '\n' || attrs[i] == '\r' || attrs[i] == '/')) i++;
            ns = i;
            while (i < attrs_len && attrs[i] != '=' && attrs[i] != ' ' && attrs[i] != '\t' && attrs[i] != '\n' && attrs[i] != '\r') i++;
            ne = i;
            if (ne == ns) { i++; continue; }
            while (i < attrs_len && (attrs[i] == ' ' || attrs[i] == '\n' || attrs[i] == '\r' || attrs[i] == '\t')) i++;
            vs = ve = i;
            if (i < attrs_len && attrs[i] == '=') {
                i++;
                while (i < attrs_len && (attrs[i] == ' ' || attrs[i] == '\n' || attrs[i] == '\r')) i++;
                q = (i < attrs_len && (attrs[i] == '"' || attrs[i] == '\'')) ? attrs[i++] : 0;
                vs = i;
                while (i < attrs_len && (q ? attrs[i] != q : attrs[i] != ' ' && attrs[i] != '\t' && attrs[i] != '\n' && attrs[i] != '\r')) i++;
                ve = i;
                if (q && i < attrs_len) i++;
            }
            {
                long k, dn;
                for (k = ns; k < ne; k++) {
                    char c = attrs[k] >= 'A' && attrs[k] <= 'Z' ? attrs[k] + 32 : attrs[k];
                    if (add_attr_bytes(p->doc, &c, 1)) return -1;
                }
                if (add_attr_bytes(p->doc, "", 1)) return -1;
                if (grow((void **)&p->doc->attrs, &p->doc->cap_attrs, p->doc->attrs_len + (ve - vs) + 1, 1)) return -1;
                dn = decode(attrs + vs, ve - vs, p->doc->attrs + p->doc->attrs_len);
                p->doc->attrs_len += dn;
                if (add_attr_bytes(p->doc, "", 1)) return -1;
                any = 1;
            }
        }
        if (any) {
            if (add_attr_bytes(p->doc, "", 1)) return -1;
            p->doc->nodes[node].attrs = start;
        }
    }
    if (!is_void(tag) && p->depth < 64) p->stack[p->depth++] = node;
    return 0;
}

static void end_tag(Parser *p, int tag)
{
    int i;
    if (tag == TAG_HTML || tag == TAG_HEAD || tag == TAG_BODY || is_void(tag)) return;
    if (tag == TAG_TD || tag == TAG_TH || tag == TAG_TR || tag == TAG_LI) {
        close_within(p, tag, TAG_TABLE, tag == TAG_LI ? TAG_UL : -1);
        return;
    }
    if ((i = open_index(p, tag)) > 0) close_to(p, i);
}

static int text(Parser *p, const char *s, long len)
{
    Doc *d = p->doc;
    long node, n;
    if (len <= 0) return 0;
    /* decoded straight onto the end of the page's text, as it never gets longer */
    if (grow((void **)&d->text, &d->cap_text, d->text_len + len, 1)) return -1;
    n = decode(s, len, d->text + d->text_len);
    if ((node = add_node(d, TAG_TEXT, current(p))) < 0) return -1;
    d->nodes[node].text = d->text_len;
    d->nodes[node].len = n;
    d->text_len += n;
    return 0;
}

/* Give back what the buffers grew beyond what they hold; shrinking stays in place */
static void fit(Doc *d)
{
    void *q;
    if (d->nodes && (q = realloc(d->nodes, d->nnodes * sizeof(Node)))) { d->nodes = q; d->cap_nodes = d->nnodes; }
    if (d->text && (q = realloc(d->text, d->text_len + 1))) { d->text = q; d->cap_text = d->text_len + 1; }
    if (d->attrs && (q = realloc(d->attrs, d->attrs_len + 1))) { d->attrs = q; d->cap_attrs = d->attrs_len + 1; }
}

static void set_title(Doc *d, const char *s, long len)
{
    char buf[256];
    long n, i, o = 0;
    int space = 1;
    if (len > 255) len = 255;
    n = decode(s, len, buf);
    for (i = 0; i < n && o < 127; i++) {
        char c = buf[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') { if (!space) d->title[o++] = ' '; space = 1; }
        else { d->title[o++] = c; space = 0; }
    }
    while (o > 0 && d->title[o - 1] == ' ') o--;
    d->title[o] = 0;
}

static long find_ci(const char *s, long from, long len, const char *what)
{
    long i, k, wl = strlen(what);
    for (i = from; i + wl <= len; i++) {
        for (k = 0; k < wl && upper(s[i + k]) == what[k]; k++) {}
        if (k == wl) return i;
    }
    return len;
}

int html_parse(Doc *doc, const char *src, long len)
{
    return html_parse_charset(doc, src, len, 0);
}

int html_parse_charset(Doc *doc, const char *src, long len, int charset)
{
    Parser p;
    long i = 0, text_start = 0;
    memset(doc, 0, sizeof *doc);
    memset(&p, 0, sizeof p);
    p.doc = doc;
    page_charset = charset == CHARSET_GUESS ? charset_sniff(src, len) : charset;
    doc->charset = page_charset;
    if (add_node(doc, TAG_HTML, -1) < 0) return -1;
    p.stack[0] = 0;
    p.depth = 1;
    /* the text is at most as long as the page: room for it at once, rather than room
       twice over while it doubles (it's trimmed to fit after) */
    if ((doc->text = malloc(len + 1))) doc->cap_text = len + 1;

    while (i < len) {
        long j, name_start;
        int closing, tag;
        if (src[i] != '<') { i++; continue; }
        /* is this a tag? '<' then a letter, '/', '!' or '?' */
        if (i + 1 >= len) { i++; continue; }
        {
            char c1 = src[i + 1];
            int letter = (c1 | 32) >= 'a' && (c1 | 32) <= 'z';
            if (!letter && c1 != '/' && c1 != '!' && c1 != '?') { i++; continue; }
        }
        if (text(&p, src + text_start, i - text_start)) goto oom;
        if (src[i + 1] == '!' && i + 3 < len && src[i + 2] == '-' && src[i + 3] == '-') {
            j = i + 4;
            while (j + 2 < len && !(src[j] == '-' && src[j + 1] == '-' && src[j + 2] == '>')) j++;
            i = text_start = (j + 2 < len) ? j + 3 : len;
            continue;
        }
        if (src[i + 1] == '!' || src[i + 1] == '?') {
            j = i + 2;
            while (j < len && src[j] != '>') j++;
            i = text_start = j < len ? j + 1 : len;
            continue;
        }
        closing = src[i + 1] == '/';
        name_start = i + 1 + closing;
        j = name_start;
        while (j < len && src[j] != '>' && src[j] != ' ' && src[j] != '\t' && src[j] != '\n' && src[j] != '\r' && src[j] != '/') j++;
        tag = html_tag(src + name_start, (int)(j - name_start));
        {
            long attrs = j;
            char q = 0;
            while (j < len && (q || src[j] != '>')) {
                if (!q && (src[j] == '"' || src[j] == '\'')) q = src[j];
                else if (q && src[j] == q) q = 0;
                j++;
            }
            if (!closing && tag != TAG_UNKNOWN) {
                if (start_tag(&p, tag, src + attrs, j - attrs)) goto oom;
            } else if (closing && tag != TAG_UNKNOWN) {
                end_tag(&p, tag);
            }
        }
        i = text_start = j < len ? j + 1 : len;
        /* Raw text: SCRIPT and STYLE contents are skipped, TITLE's kept as the title, and
           a TEXTAREA's kept as its text (less the line break after the tag) */
        if (!closing && (tag == TAG_SCRIPT || tag == TAG_STYLE || tag == TAG_TITLE || tag == TAG_TEXTAREA)) {
            long end = find_ci(src, i, len, tag == TAG_SCRIPT ? "</SCRIPT" : tag == TAG_STYLE ? "</STYLE" :
                               tag == TAG_TITLE ? "</TITLE" : "</TEXTAREA");
            if (tag == TAG_TITLE) set_title(doc, src + i, end - i);
            if (tag == TAG_TEXTAREA) {
                long from = i;
                if (from < end && src[from] == '\r') from++;
                if (from < end && src[from] == '\n') from++;
                if (text(&p, src + from, end - from)) goto oom;
            }
            end_tag(&p, tag);
            j = end;
            while (j < len && src[j] != '>') j++;
            i = text_start = j < len ? j + 1 : len;
        }
    }
    if (text(&p, src + text_start, len - text_start)) goto oom;
    fit(doc);
    return 0;
oom:
    fit(doc);   /* what was read so far is a whole page in itself */
    return -1;
}

void html_free(Doc *doc)
{
    free(doc->nodes);
    free(doc->text);
    free(doc->attrs);
    memset(doc, 0, sizeof *doc);
}

const char *html_attr(const Doc *doc, const Node *n, const char *name)
{
    const char *a;
    if (n->attrs < 0) return NULL;
    a = doc->attrs + n->attrs;
    while (*a) {
        const char *v = a + strlen(a) + 1;
        if (!strcmp(a, name)) return v;
        a = v + strlen(v) + 1;
    }
    return NULL;
}
