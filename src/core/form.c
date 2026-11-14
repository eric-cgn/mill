#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "charset.h"
#include "form.h"

static int eq_ci(const char *a, const char *b)
{
    while (*a && *b && (*a | 32) == (*b | 32)) { a++; b++; }
    return !*a && !*b;
}

static char *copy(const char *s)
{
    char *c = malloc(strlen(s) + 1);
    if (c) strcpy(c, s);
    return c;
}

static int field_type(const char *t)
{
    static const struct { const char *name; char type; } TYPES[] = {
        { "password", FIELD_PASSWORD }, { "hidden", FIELD_HIDDEN }, { "checkbox", FIELD_CHECKBOX },
        { "radio", FIELD_RADIO }, { "submit", FIELD_SUBMIT }, { "reset", FIELD_RESET },
        { "image", FIELD_IMAGE }, { "file", FIELD_FILE },
    };
    int i;
    if (t) for (i = 0; i < (int)(sizeof TYPES / sizeof TYPES[0]); i++) if (eq_ci(t, TYPES[i].name)) return TYPES[i].type;
    return FIELD_TEXT;
}

/* The text under a node, all of it */
static char *text_of(const Doc *d, long node)
{
    long k, len = 0;
    char *s;
    for (k = d->nodes[node].first; k >= 0; k = d->nodes[k].next) if (d->nodes[k].tag == TAG_TEXT) len += d->nodes[k].len;
    if (!(s = malloc(len + 1))) return NULL;
    len = 0;
    for (k = d->nodes[node].first; k >= 0; k = d->nodes[k].next) {
        if (d->nodes[k].tag != TAG_TEXT) continue;
        memcpy(s + len, d->text + d->nodes[k].text, d->nodes[k].len);
        len += d->nodes[k].len;
    }
    s[len] = 0;
    return s;
}

void forms_option_label(const Doc *d, long option, char *out, int size)
{
    char *t = text_of(d, option), *p;
    int o = 0, space = 1;
    out[0] = 0;
    if (!t) return;
    for (p = t; *p && o < size - 1; p++) {
        if (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') { if (!space) out[o++] = ' '; space = 1; }
        else { out[o++] = *p; space = 0; }
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = 0;
    free(t);
}

/* A field as the page gave it; -1 if there wasn't the memory for its text */
static int initial(Field *f, const Doc *d)
{
    const Node *n = &d->nodes[f->node];
    const char *v;
    short i;
    free(f->value);
    f->value = NULL;
    f->scroll = 0;
    f->checked = html_attr(d, n, "checked") != NULL;
    if (f->type == FIELD_TEXTAREA) f->value = text_of(d, f->node);
    else if (f->type == FIELD_TEXT || f->type == FIELD_PASSWORD || f->type == FIELD_HIDDEN)
        f->value = copy((v = html_attr(d, n, "value")) ? v : "");
    else if (f->type == FIELD_FILE) f->value = copy("");
    if (f->type == FIELD_SELECT && f->noptions) {
        int any = 0;
        for (i = 0; i < f->noptions; i++) {
            f->selected[i] = html_attr(d, &d->nodes[f->options[i]], "selected") != NULL;
            if (f->selected[i]) {
                if (any && !f->multiple) f->selected[i] = 0;  /* a pop-up shows one */
                any = 1;
            }
        }
        if (!any && !f->multiple) f->selected[0] = 1;
    }
    return !f->value && (f->type == FIELD_TEXTAREA || f->type == FIELD_TEXT || f->type == FIELD_PASSWORD ||
                         f->type == FIELD_HIDDEN || f->type == FIELD_FILE) ? -1 : 0;
}

int forms_build(Forms *F, const Doc *d)
{
    long i, p, k;
    memset(F, 0, sizeof *F);
    for (i = 0; i < d->nnodes; i++) {
        const Node *n = &d->nodes[i];
        Field *f;
        const char *v;
        if (n->tag != TAG_INPUT && n->tag != TAG_SELECT && n->tag != TAG_TEXTAREA) continue;
        if (!(f = realloc(F->fields, (F->n + 1) * sizeof(Field)))) return -1;
        F->fields = f;
        f = &F->fields[F->n++];
        memset(f, 0, sizeof *f);
        f->node = i;
        f->form = -1;
        for (p = n->parent; p >= 0; p = d->nodes[p].parent) if (d->nodes[p].tag == TAG_FORM) { f->form = p; break; }
        f->type = n->tag == TAG_SELECT ? FIELD_SELECT : n->tag == TAG_TEXTAREA ? FIELD_TEXTAREA
                                                       : (char)field_type(html_attr(d, n, "type"));
        f->name = html_attr(d, n, "name");
        f->multiple = n->tag == TAG_SELECT && html_attr(d, n, "multiple") != NULL;
        f->disabled = html_attr(d, n, "disabled") != NULL;
        f->size = (v = html_attr(d, n, "size")) ? (short)atoi(v) : 0;
        f->maxlength = (v = html_attr(d, n, "maxlength")) ? (short)atoi(v) : 0;
        f->rows = (v = html_attr(d, n, "rows")) ? (short)atoi(v) : 0;
        f->cols = (v = html_attr(d, n, "cols")) ? (short)atoi(v) : 0;
        if (f->type == FIELD_SELECT) {
            for (k = n->first; k >= 0; k = d->nodes[k].next) if (d->nodes[k].tag == TAG_OPTION) f->noptions++;
            f->options = malloc((f->noptions + 1) * sizeof(long));
            f->selected = calloc(f->noptions + 1, 1);
            if (!f->options || !f->selected) return -1;
            f->noptions = 0;
            for (k = n->first; k >= 0; k = d->nodes[k].next) if (d->nodes[k].tag == TAG_OPTION) f->options[f->noptions++] = k;
        }
        if (initial(f, d)) return -1;
    }
    return 0;
}

void forms_free(Forms *F)
{
    long i;
    for (i = 0; i < F->n; i++) { free(F->fields[i].value); free(F->fields[i].options); free(F->fields[i].selected); }
    free(F->fields);
    memset(F, 0, sizeof *F);
}

long forms_find(const Forms *F, long node)
{
    long lo = 0, hi = F->n;
    while (lo < hi) {
        long mid = (lo + hi) / 2;
        if (F->fields[mid].node < node) lo = mid + 1; else hi = mid;
    }
    return lo < F->n && F->fields[lo].node == node ? lo : -1;
}

/* Whether a field differs from what the page gave it */
static int field_changed(const Field *f, const Doc *d)
{
    Field was;
    int changed;
    if (f->type == FIELD_HIDDEN || f->type == FIELD_SUBMIT || f->type == FIELD_RESET || f->type == FIELD_IMAGE) return 0;
    memset(&was, 0, sizeof was);
    was.node = f->node;
    was.type = f->type;
    was.multiple = f->multiple;
    was.options = f->options;
    was.noptions = f->noptions;
    if (f->noptions && !(was.selected = calloc(f->noptions, 1))) return 0;
    initial(&was, d);
    if (f->type == FIELD_CHECKBOX || f->type == FIELD_RADIO) changed = f->checked != was.checked;
    else if (f->type == FIELD_SELECT) changed = f->noptions && memcmp(f->selected, was.selected, f->noptions) != 0;
    else changed = strcmp(f->value ? f->value : "", was.value ? was.value : "") != 0;
    free(was.value);
    free(was.selected);
    return changed;
}

int forms_changed(const Forms *F, const Doc *d)
{
    long i;
    for (i = 0; i < F->n; i++) if (field_changed(&F->fields[i], d)) return 1;
    return 0;
}

void forms_reset(Forms *F, const Doc *d, long form)
{
    long i;
    for (i = 0; i < F->n; i++) if (F->fields[i].form == form) initial(&F->fields[i], d);
}

int forms_set_value(Field *f, const char *value)
{
    char *c = copy(value);
    if (!c) return -1;
    free(f->value);
    f->value = c;
    return 0;
}

void forms_click(Forms *F, long index)
{
    Field *f = &F->fields[index];
    long i;
    if (f->disabled) return;
    if (f->type == FIELD_CHECKBOX) { f->checked = !f->checked; return; }
    if (f->type != FIELD_RADIO) return;
    f->checked = 1;
    for (i = 0; i < F->n; i++) {
        Field *o = &F->fields[i];
        if (i != index && o->type == FIELD_RADIO && o->form == f->form && o->name && f->name && !strcmp(o->name, f->name))
            o->checked = 0;
    }
}

/* ------------------------------------------------------------ submitting */

typedef struct { char *p; long len, cap; int oom, utf8; } Buf;

static void put(Buf *b, const char *s, long n)
{
    if (b->oom) return;
    if (b->len + n + 1 > b->cap) {
        long cap = b->cap ? b->cap : 256;
        char *q;
        while (b->len + n + 1 > cap) cap *= 2;
        if (!(q = realloc(b->p, cap))) { b->oom = 1; return; }
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void put_s(Buf *b, const char *s) { put(b, s, strlen(s)); }

/* Mac Roman to the page's charset (UTF-8, else Latin-1), with every line break as
   CR LF, as forms send them */
static void put_text(Buf *b, const char *s, int url)
{
    for (; *s; s++) {
        unsigned char bytes[4];
        int n = 1, i;
        if (*s == '\r' || *s == '\n') {
            if (*s == '\r' && s[1] == '\n') s++;
            put_s(b, url ? "%0D%0A" : "\r\n");
            continue;
        }
        if (b->utf8 && (unsigned char)*s >= 0x80) {
            long u = mac_to_unicode((unsigned char)*s);
            if (u < 0x800) { bytes[0] = (unsigned char)(0xC0 | (u >> 6)); n = 2; }
            else { bytes[0] = (unsigned char)(0xE0 | (u >> 12)); bytes[1] = (unsigned char)(0x80 | ((u >> 6) & 0x3F)); n = 3; }
            bytes[n - 1] = (unsigned char)(0x80 | (u & 0x3F));
        } else {
            bytes[0] = mac_to_latin1((unsigned char)*s);
        }
        for (i = 0; i < n; i++) {
            unsigned char c = bytes[i];
            if (!url) put(b, (char *)&c, 1);
            else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.*", c)) put(b, (char *)&c, 1);
            else if (c == ' ') put_s(b, "+");
            else {
                char hex[4];
                sprintf(hex, "%%%02X", c);
                put_s(b, hex);
            }
        }
    }
}

/* A multipart part: its name and text as they're sent */
static void add_part(Submission *s, Buf *b, const char *name, const char *value, int file)
{
    Buf n, v;
    FormPart *more;
    memset(&n, 0, sizeof n);
    memset(&v, 0, sizeof v);
    n.utf8 = v.utf8 = b->utf8;
    put_text(&n, name, 0);
    put(&n, "", 1);
    if (file) put(&v, value, strlen(value) + 1);    /* a Mac path, as it is */
    else { put_text(&v, value, 0); put(&v, "", 1); }
    if (n.oom || v.oom || !(more = realloc(s->parts, (s->nparts + 1) * sizeof(FormPart)))) {
        free(n.p); free(v.p); b->oom = 1; return;
    }
    s->parts = more;
    s->parts[s->nparts].name = n.p;
    s->parts[s->nparts].value = v.p;
    s->parts[s->nparts].file = file;
    s->nparts++;
}

static void pair(Buf *b, Submission *s, const char *name, const char *value)
{
    if (s->multipart) { add_part(s, b, name, value, 0); return; }
    if (b->len) put_s(b, "&");
    put_text(b, name, 1);
    put_s(b, "=");
    put_text(b, value, 1);
}

long form_part_head(const Submission *s, const FormPart *p, const char *filename, const char *type, char *out, long size)
{
    long n = snprintf(out, size, "--%s\r\nContent-Disposition: form-data; name=\"%s\"", s->boundary, p->name);
    if (filename) n += snprintf(out + (n < size ? n : size), n < size ? size - n : 0, "; filename=\"%s\"\r\nContent-Type: %s", filename, type);
    n += snprintf(out + (n < size ? n : size), n < size ? size - n : 0, "\r\n\r\n");
    return n < size ? n : -1;
}

long form_end(const Submission *s, char *out, long size)
{
    long n = snprintf(out, size, "--%s--\r\n", s->boundary);
    return n < size ? n : -1;
}

static void option_value(const Doc *d, long option, char *out, int size)
{
    const char *v = html_attr(d, &d->nodes[option], "value");
    if (v) snprintf(out, size, "%s", v);
    else forms_option_label(d, option, out, size);
}

int forms_submit(const Forms *F, const Doc *d, long form, long by, short x, short y, Submission *out)
{
    const Node *fn = form >= 0 ? &d->nodes[form] : NULL;
    const char *method = fn ? html_attr(d, fn, "method") : NULL, *enctype = fn ? html_attr(d, fn, "enctype") : NULL;
    Buf b;
    long i;
    memset(out, 0, sizeof *out);
    memset(&b, 0, sizeof b);
    b.utf8 = d->charset == CHARSET_UTF8;  /* forms answer in their page's charset */
    out->post = method && eq_ci(method, "post");
    out->multipart = out->post && enctype && eq_ci(enctype, "multipart/form-data");
    out->action = fn && html_attr(d, fn, "action") ? html_attr(d, fn, "action") : "";
    strcpy(out->boundary, "MillFormBoundary7d3a9e0c");
    for (i = 0; i < F->n; i++) {
        const Field *f = &F->fields[i];
        const char *name = f->name, *v = html_attr(d, &d->nodes[f->node], "value");
        short k;
        if (f->form != form || !name || !*name || f->disabled) continue;
        switch (f->type) {
        case FIELD_TEXT: case FIELD_PASSWORD: case FIELD_HIDDEN: case FIELD_TEXTAREA:
            /* a first field named isindex (ISINDEX makes one) sends just its words */
            if (f->type == FIELD_TEXT && !b.len && !out->multipart && eq_ci(name, "isindex")) put_text(&b, f->value ? f->value : "", 1);
            else pair(&b, out, name, f->value ? f->value : "");
            break;
        case FIELD_CHECKBOX: case FIELD_RADIO:
            if (f->checked) pair(&b, out, name, v ? v : "on");
            break;
        case FIELD_SUBMIT:
            if (i == by) pair(&b, out, name, v ? v : "Submit");
            break;
        case FIELD_IMAGE:
            if (i == by) {
                char key[300], num[8];
                snprintf(key, sizeof key, "%s.x", name); sprintf(num, "%d", x); pair(&b, out, key, num);
                snprintf(key, sizeof key, "%s.y", name); sprintf(num, "%d", y); pair(&b, out, key, num);
            }
            break;
        case FIELD_SELECT:
            for (k = 0; k < f->noptions; k++) {
                char val[256];
                if (!f->selected[k]) continue;
                option_value(d, f->options[k], val, sizeof val);
                pair(&b, out, name, val);
            }
            break;
        case FIELD_FILE:
            if (!out->multipart) pair(&b, out, name, f->value ? f->value : "");
            else add_part(out, &b, name, f->value ? f->value : "", 1);
            break;
        }
    }
    if (b.oom) { free(b.p); return -1; }
    if (!b.p) put_s(&b, "");
    if (b.oom) return -1;
    out->data = b.p;
    out->len = b.len;
    return 0;
}

void submission_free(Submission *s)
{
    int i;
    free(s->data);
    s->data = NULL;
    for (i = 0; i < s->nparts; i++) { free(s->parts[i].name); free(s->parts[i].value); }
    free(s->parts);
    s->parts = NULL;
    s->nparts = 0;
}

/* ------------------------------------------------------------ kept for Back */

/* The fields whose state is the person's to keep */
static int kept_type(const Field *f)
{
    return !f->disabled && (f->type == FIELD_TEXT || f->type == FIELD_TEXTAREA || f->type == FIELD_CHECKBOX ||
                            f->type == FIELD_RADIO || f->type == FIELD_SELECT);
}

/* Which of the page's forms (0 for the first FORM) a field's form is; -1 for none */
static long form_number(const Doc *d, long form)
{
    long i, n = 0;
    if (form < 0) return -1;
    for (i = 0; i < form; i++) if (d->nodes[i].tag == TAG_FORM) n++;
    return n;
}

/* s with the characters that separate entries, and line breaks, as %XX */
static void put_escaped(Buf *b, const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < ' ' || c == '%' || c == '&' || c == '=' || c == ':') {
            char hex[4];
            sprintf(hex, "%%%02X", c);
            put_s(b, hex);
        } else {
            put(b, (const char *)&c, 1);
        }
    }
}

char *forms_save(const Forms *F, const Doc *d)
{
    Buf b;
    long i;
    memset(&b, 0, sizeof b);
    for (i = 0; i < F->n && !b.oom; i++) {
        const Field *f = &F->fields[i];
        char head[40];
        if (!kept_type(f) || !field_changed(f, d)) continue;
        sprintf(head, "%s%ld:%ld:%d:", b.len ? "&" : "", i, form_number(d, f->form), f->type);
        put_s(&b, head);
        put_escaped(&b, f->name ? f->name : "");
        put_s(&b, "=");
        if (f->type == FIELD_CHECKBOX || f->type == FIELD_RADIO) put_s(&b, f->checked ? "1" : "0");
        else if (f->type == FIELD_SELECT) {
            short k;
            for (k = 0; k < f->noptions; k++) put_s(&b, f->selected[k] ? "1" : "0");
        } else put_escaped(&b, f->value ? f->value : "");
        if (b.len > FORMS_STATE_MAX) break;     /* too much: none of it is kept */
    }
    if (b.oom || b.len > FORMS_STATE_MAX) { free(b.p); return NULL; }
    return b.p;
}

/* n bytes of s with %XX decoded, into a malloc'd string */
static char *unescape(const char *s, long n)
{
    char *out = malloc(n + 1);
    long i, o = 0;
    if (!out) return NULL;
    for (i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n) {
            int hi = s[i + 1], lo = s[i + 2];
            hi = hi >= '0' && hi <= '9' ? hi - '0' : hi >= 'A' && hi <= 'F' ? hi - 'A' + 10 : -1;
            lo = lo >= '0' && lo <= '9' ? lo - '0' : lo >= 'A' && lo <= 'F' ? lo - 'A' + 10 : -1;
            if (hi >= 0 && lo >= 0) { out[o++] = (char)(hi * 16 + lo); i += 2; continue; }
        }
        out[o++] = s[i];
    }
    out[o] = 0;
    return out;
}

/* One entry back into its field, if the field still matches */
static void restore_one(Forms *F, const Doc *d, long i, long form, int type, const char *name, const char *value)
{
    Field *f;
    if (i < 0 || i >= F->n) return;
    f = &F->fields[i];
    if (!kept_type(f) || f->type != type || form_number(d, f->form) != form || strcmp(f->name ? f->name : "", name)) return;
    if (f->type == FIELD_CHECKBOX || f->type == FIELD_RADIO) {
        if ((value[0] == '0' || value[0] == '1') && !value[1]) f->checked = value[0] == '1';
    } else if (f->type == FIELD_SELECT) {
        short k;
        if ((long)strlen(value) != f->noptions || strspn(value, "01") != strlen(value)) return;
        for (k = 0; k < f->noptions; k++) f->selected[k] = value[k] == '1';
    } else if (!forms_set_value(f, value)) {
        f->scroll = 0;
    }
}

void forms_restore(Forms *F, const Doc *d, const char *state)
{
    const char *p = state;
    while (p && *p) {
        const char *end = strchr(p, '&'), *eq;
        char *rest, *name, *value;
        long i, form;
        int type;
        if (!end) end = p + strlen(p);
        /* place:form:type:name=value */
        i = strtol(p, &rest, 10);
        if (*rest == ':') {
            form = strtol(rest + 1, &rest, 10);
            if (*rest == ':') {
                type = (int)strtol(rest + 1, &rest, 10);
                if (*rest == ':' && rest < end && (eq = memchr(rest, '=', end - rest)) != NULL) {
                    name = unescape(rest + 1, eq - rest - 1);
                    value = unescape(eq + 1, end - eq - 1);
                    if (name && value) restore_one(F, d, i, form, type, name, value);
                    free(name);
                    free(value);
                }
            }
        }
        p = *end ? end + 1 : end;
    }
}
