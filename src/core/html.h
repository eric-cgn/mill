/* HTML 3.2 (plus what Netscape 3 and IE 3 shared) parsed into a compact node tree.
   Text is stored decoded, in Mac Roman. */
#ifndef MILL_HTML_H
#define MILL_HTML_H

enum {
    TAG_UNKNOWN = 0, TAG_TEXT,
    TAG_HTML, TAG_HEAD, TAG_TITLE, TAG_BODY, TAG_META, TAG_SCRIPT, TAG_STYLE,
    TAG_P, TAG_BR, TAG_HR, TAG_DIV, TAG_CENTER, TAG_BLOCKQUOTE, TAG_PRE, TAG_ADDRESS,
    TAG_H1, TAG_H2, TAG_H3, TAG_H4, TAG_H5, TAG_H6,
    TAG_UL, TAG_OL, TAG_LI, TAG_DL, TAG_DT, TAG_DD, TAG_DIR, TAG_MENU,
    TAG_A, TAG_B, TAG_STRONG, TAG_I, TAG_EM, TAG_U, TAG_TT, TAG_CODE, TAG_KBD, TAG_SAMP, TAG_VAR,
    TAG_CITE, TAG_DFN, TAG_BIG, TAG_SMALL, TAG_SUB, TAG_SUP, TAG_STRIKE, TAG_FONT, TAG_BASEFONT, TAG_NOBR,
    TAG_IMG, TAG_MAP, TAG_AREA,
    TAG_TABLE, TAG_CAPTION, TAG_TR, TAG_TD, TAG_TH,
    TAG_FORM, TAG_INPUT, TAG_SELECT, TAG_OPTION, TAG_TEXTAREA,
    TAG_BASE, TAG_LINK, TAG_NOSCRIPT, TAG_ISINDEX,
    TAG_COUNT
};

typedef struct {
    short tag;
    long parent, first, last, next;  /* node indices, -1 for none */
    long text, len;                  /* TAG_TEXT: run in Doc.text */
    long attrs;                      /* offset into Doc.attrs, or -1: name\0value\0 ... \0 */
} Node;

typedef struct {
    Node *nodes;
    long nnodes, cap_nodes;
    char *text;
    long text_len, cap_text;
    char *attrs;
    long attrs_len, cap_attrs;
    char title[128];
    int charset;                     /* what the page was in: CHARSET_MAC, _LATIN1, or _UTF8 */
} Doc;

/* Parse a whole HTML page. Returns 0, or -1 when out of memory, when doc holds the page
   as far as it got (still to be freed with html_free). */
int html_parse(Doc *doc, const char *src, long len);
/* The same, for a page in a known charset (CHARSET_* in charset.h); html_parse and
   CHARSET_GUESS go by the page's META tag, else whether it's valid UTF-8 */
int html_parse_charset(Doc *doc, const char *src, long len, int charset);
void html_free(Doc *doc);

/* An attribute's value (Mac Roman, decoded), or NULL. Names are matched without case. */
const char *html_attr(const Doc *doc, const Node *n, const char *name);
int html_tag(const char *name, int len);

#endif
