/* Forms: the state of each control on a page, and what submitting a form sends. */
#ifndef MILL_FORM_H
#define MILL_FORM_H

#include "html.h"

enum { FIELD_TEXT, FIELD_PASSWORD, FIELD_HIDDEN, FIELD_CHECKBOX, FIELD_RADIO, FIELD_SUBMIT, FIELD_RESET,
       FIELD_IMAGE, FIELD_FILE, FIELD_TEXTAREA, FIELD_SELECT };

typedef struct {
    long node;          /* the INPUT, SELECT or TEXTAREA */
    long form;          /* its FORM, or -1 for a control outside any form */
    const char *name;   /* its NAME (in the Doc), or NULL */
    char type;
    char checked;       /* checkboxes and radio buttons */
    char multiple;      /* SELECT MULTIPLE */
    char *value;        /* what's typed (text, password, textarea), a hidden value, or a chosen file's path */
    long *options;      /* SELECT: its OPTION nodes */
    char *selected;     /* SELECT: whether each option is chosen */
    short noptions;
    short size, maxlength, rows, cols;
} Field;

typedef struct { Field *fields; long n; } Forms;

/* Every control on the page, in document order, with its initial state. Returns 0 or -1. */
int forms_build(Forms *F, const Doc *d);
void forms_free(Forms *F);
/* The field for a control node, or -1 */
long forms_find(const Forms *F, long node);
/* Back to the page's own values (a Reset button) */
void forms_reset(Forms *F, const Doc *d, long form);
int forms_set_value(Field *f, const char *value);
/* A click on a checkbox (toggles) or radio button (chosen, and the others of its name not) */
void forms_click(Forms *F, long index);
/* What an OPTION shows */
void forms_option_label(const Doc *d, long option, char *out, int size);

typedef struct {
    char *data;         /* the query string (GET) or the request body (POST) */
    long len;
    int post, multipart;
    char boundary[40];
    const char *action; /* FORM ACTION as written ("" for the page itself) */
} Submission;

/* Reads a file chosen for upload: returns its length and a malloc'd copy, or -1 */
typedef long (*file_reader)(void *ctx, const char *path, char **data);

/* What submitting the form sends, by the field that submitted it (a submit button or an
   image, at x,y), or -1 for Return in a text field. Text is sent as ISO 8859-1. */
int forms_submit(const Forms *F, const Doc *d, long form, long by, short x, short y,
                 file_reader read_file, void *ctx, Submission *out);
void submission_free(Submission *s);

#endif
