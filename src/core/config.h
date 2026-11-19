/* Saving mill://config. Each section of the page is a FORM of its own, with a Save button and a
   hidden section=NAME, and saving one saves only its own settings: a checkbox that isn't ticked
   isn't sent, so one in another section is left as it is, not turned off. */
#ifndef MILL_CONFIG_H
#define MILL_CONFIG_H

/* What the saving needs from Mill (mac_local.c's, in Mill) */
typedef struct {
    /* one value from the form's body, decoded: 1, or 0 if it isn't there */
    int (*field)(const char *body, long len, const char *name, char *out, int size);
    void (*put)(const char *name, const char *value);
    void (*bookmark)(int n, const char *url, const char *title);    /* url "" empties slot n */
    int history_max, keep_alive_max, bookmarks;
    const char *default_check;
} ConfigIO;

/* Mill's own sections, as each FORM's section field and its heading's anchor */
#define CONFIG_GENERAL "general"
#define CONFIG_CHECK "check"
#define CONFIG_PRINTING "printing"
#define CONFIG_BOOKMARKS "bookmarks"

/* The section a form was sent from, into sec; 0 if it says none */
int config_section(const ConfigIO *io, const char *body, long len, char *sec, int size);

/* Saves Mill's own section sec from the form: 1 if sec is one of them, else 0 and nothing
   changes (a Plug-In's protocol or format; mac_pages.c saves those). note is "" if all was
   right, or says what wasn't; a File types rule that isn't one leaves what was typed in
   typed, to show again. rules is room to check the rules in (as big as typed). */
int config_save(const ConfigIO *io, const char *body, long len, const char *sec, char *note, int note_size,
                char *typed, char *rules, int size);

#endif
