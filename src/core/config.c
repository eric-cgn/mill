/* Saving mill://config's own sections (see config.h) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "filetypes.h"
#include "url.h"

/* The file type rules typed, checked and joined with semicolons for the settings file;
   returns 0, or the number of the first line that isn't a rule */
static int join_rules(char *text)
{
    char *in = text, *o = text;
    int line = 0;
    while (*in) {
        char *e = in;
        long n;
        while (*e && *e != '\r' && *e != '\n') e++;
        n = e - in;
        line++;
        while (n > 0 && in[n - 1] == ' ') n--;
        if (n > 0) {
            long k = 0;
            while (k < n && in[k] == ' ') k++;
            if (k < n) {
                if (filetype_rule_ok(in + k, n - k)) return line;
                if (o > text) *o++ = ';';
                memmove(o, in + k, n - k);
                o += n - k;
            }
        }
        if (*e == '\r' && e[1] == '\n') e++;
        in = *e ? e + 1 : e;
    }
    *o = 0;
    return 0;
}

int config_section(const ConfigIO *io, const char *body, long len, char *sec, int size)
{
    if (!io->field(body, len, "section", sec, size)) { sec[0] = 0; return 0; }
    return sec[0] != 0;
}

/* A checkbox: sent ("yes") if it's ticked, missing if not */
static void checkbox(const ConfigIO *io, const char *body, long len, const char *name)
{
    char v[8];
    io->put(name, io->field(body, len, name, v, sizeof v) ? "yes" : "no");
}

/* A whole number from lo to hi (empty_ok: "" is 0), or note says it must be */
static void number(const ConfigIO *io, const char *body, long len, const char *name, long lo, long hi, int empty_ok,
                   const char *what, const char *unit, char *note, int note_size)
{
    char k[16];
    long n;
    if (!io->field(body, len, name, k, sizeof k)) return;
    n = atol(k);
    if (n < lo || n > hi || ((k[0] || !empty_ok) && (k[0] < '0' || k[0] > '9')))
        snprintf(note, note_size, "%s must be from %ld to %ld%s.", what, lo, hi, unit);
    else {
        sprintf(k, "%ld", n);
        io->put(name, k);
    }
}

static void general(const ConfigIO *io, const char *body, long len, char *note, int note_size, char *typed, char *rules,
                    int size)
{
    static const char *const boxes[] = { "welcome", "scale_text_only", "full_screen_bars", "load_images", "cookies",
                                         "decode_macbinary", "upload_ext" };
    char home[256], v[160], *c;
    Url check;
    unsigned k;
    if (io->field(body, len, "homepage", home, sizeof home)) {
        if (home[0] && url_parse(home, &check)) snprintf(note, note_size, "That home page isn\325t an address Mill can open.");
        else io->put("homepage", home);
    }
    number(io, body, len, "image_memory", 0, 4096, 1, "Image memory", " K", note, note_size);
    number(io, body, len, "history", 1, io->history_max, 0, "Window history", "", note, note_size);
    number(io, body, len, "keep_alive", 0, io->keep_alive_max, 0, "Keep-alive", " seconds", note, note_size);
    /* a font's name: FONT_NAME_MAX (mac_local.h) and a few over, as before */
    if (io->field(body, len, "font_prop", v, 36)) io->put("font_prop", v);
    if (io->field(body, len, "font_fixed", v, 36)) io->put("font_fixed", v);
    for (k = 0; k < sizeof boxes / sizeof boxes[0]; k++) checkbox(io, body, len, boxes[k]);
    if (io->field(body, len, "user_agent", v, 141)) {
        for (c = v; *c; c++) if ((unsigned char)*c < ' ') *c = ' ';  /* one header line */
        io->put("user_agent", v);
    }
    if (io->field(body, len, "upload_as", v, 20) &&
        (!strcmp(v, "Automatic") || !strcmp(v, "Data fork only") || !strcmp(v, "MacBinary") || !strcmp(v, "BinHex")))
        io->put("upload_as", v);
    if (io->field(body, len, "file_types", typed, size)) {
        int bad;
        strcpy(rules, typed);
        if ((bad = join_rules(rules))) {
            snprintf(note, note_size, "Line %d of File types isn\325t a valid rule.", bad);
            return;
        }
        io->put("file_types", rules);
    }
    typed[0] = 0;
}

static void connection_check(const ConfigIO *io, const char *body, long len)
{
    char host[256], *c;
    checkbox(io, body, len, "net_check");
    if (io->field(body, len, "check_host", host, sizeof host)) {
        for (c = host; *c; c++) if ((unsigned char)*c <= ' ') *c = 0;     /* one name, no spaces */
        io->put("check_host", host[0] ? host : io->default_check);
    }
}

/* A title and an address each; no address empties the slot */
static void bookmarks(const ConfigIO *io, const char *body, long len, char *note, int note_size)
{
    int n;
    for (n = 0; n < io->bookmarks && !note[0]; n++) {
        char fname[16], url[300], title[100];
        Url u;
        sprintf(fname, "bu%d", n + 1);
        if (!io->field(body, len, fname, url, sizeof url)) continue;
        sprintf(fname, "bt%d", n + 1);
        if (!io->field(body, len, fname, title, sizeof title)) title[0] = 0;
        if (url[0] && url_parse(url, &u)) snprintf(note, note_size, "Bookmark %d isn\325t an address Mill can open.", n == 9 ? 0 : n + 1);
        else io->bookmark(n, url, title);
    }
}

int config_save(const ConfigIO *io, const char *body, long len, const char *sec, char *note, int note_size,
                char *typed, char *rules, int size)
{
    note[0] = typed[0] = 0;
    if (!strcmp(sec, CONFIG_GENERAL)) general(io, body, len, note, note_size, typed, rules, size);
    else if (!strcmp(sec, CONFIG_CHECK)) connection_check(io, body, len);
    else if (!strcmp(sec, CONFIG_PRINTING)) {
        checkbox(io, body, len, "print_headers");
        checkbox(io, body, len, "print_backgrounds");
    } else if (!strcmp(sec, CONFIG_BOOKMARKS)) bookmarks(io, body, len, note, note_size);
    else return 0;
    return 1;
}
