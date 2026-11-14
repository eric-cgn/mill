/* Mill's own pages, mill:about, config, plugins, and license, answered as a
   web server would. In a code segment of their own (mill.segmap), unloaded after each
   page, so they only take memory while one is being made. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/charset.h"
#include "core/filetypes.h"
#include "core/inflate.h"
#include "core/update.h"
#include "mac_local.h"
#include "mac_plugin.h"
#include "mac_sys.h"
#include "version.h"

#define HTML_MAC "text/html; charset=macintosh"
#define SETTINGS_TABLE "<TABLE BORDER=1 CELLPADDING=4 CELLSPACING=0 WIDTH=\"100%\">"

/* ------------------------------------------------------------ responses */

typedef struct { net_sink sink; void *ref; int stopped; } Out;

static void out(Out *o, const char *s, long n)
{
    if (!o->stopped && n > 0 && !o->sink(s, n, o->ref)) o->stopped = 1;
}

static void out_s(Out *o, const char *s) { out(o, s, strlen(s)); }

static void head(Out *o, int code, const char *type)
{
    char h[160];
    sprintf(h, "HTTP/1.0 %d %s\r\nContent-Type: %s\r\n\r\n", code, code == 200 ? "OK" : "Not Found", type);
    out_s(o, h);
}

/* Text inside HTML, with & < > and " escaped */
static void out_text(Out *o, const char *s)
{
    for (; *s && !o->stopped; s++) {
        switch (*s) {
        case '&': out_s(o, "&amp;"); break;
        case '<': out_s(o, "&lt;"); break;
        case '>': out_s(o, "&gt;"); break;
        case '"': out_s(o, "&quot;"); break;
        default: out(o, s, 1);
        }
    }
}

static void out_text_n(Out *o, const char *s, long n)
{
    char one[2];
    long i;
    for (i = 0; i < n; i++) { one[0] = s[i]; one[1] = 0; out_text(o, one); }
}

/* The file type rules typed in mill://config, checked and joined with semicolons for
   the settings file; returns 0, or the number of the first line that isn't a rule */
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

/* Rules one on each line, for a TEXTAREA */
static void out_rules(Out *o, const char *rules)
{
    for (; *rules && !o->stopped; rules++) {
        if (*rules == ';' || *rules == '\n') { out_s(o, "\r"); continue; }
        out_text_n(o, rules, 1);
    }
}

/* ------------------------------------------------------------ mill: pages */

long (*local_fetch)(const char *url, char *buf, long size, char *why, int why_size) = NULL;

/* Check for updates, asked for by the About page's button (never by itself): what the
   site says is newest, against this copy */
#define NOTE_CUT 32     /* characters of each New in line shown on the About page */
#define NOTES_SHOWN 2   /* New in lines shown there */

static void check_for_updates(Out *o)
{
    char why[120], site[128], *text = malloc(UPDATE_MAX_BYTES + 1);
    Update *u = malloc(sizeof *u);
    long n = -1;
    int k, err;
    strcpy(why, "Not enough memory.");
    /* the download link must be on the site the file came from: this copy's own */
    if (update_site(MILL_UPDATE_URL, site, sizeof site)) strcpy(why, "Its address isn't one Mill can open.");
    else if (text && u && local_fetch) {
        n = local_fetch(MILL_UPDATE_URL, text, UPDATE_MAX_BYTES + 1, why, sizeof why);
        if (n > UPDATE_MAX_BYTES) { n = -1; strcpy(why, "Its answer was too long."); }
        if (n >= 0 && (err = update_parse(text, n, site, u)) != 0) {
            n = -1;
            if (err == UPDATE_OFF_SITE) {
                /* site without its scheme and last slash: mill.1in1e6.com */
                const char *host = strstr(site, "://") + 3;
                snprintf(why, sizeof why, "Its download link isn't on %.*s.", (int)(strlen(host) - 1), host);
            } else {
                strcpy(why, "Its answer wasn't a version number.");
            }
        }
    }
    if (n < 0) {
        out_s(o, "<P>Mill couldn't check for updates. ");
        out_text(o, why);
    } else if (update_compare(u->version, MILL_VERSION) > 0) {
        out_s(o, "<P><B>A new version is available!</B><BR><A HREF=\"");
        out_text(o, u->link);
        out_s(o, "\">Download Mill ");
        out_text(o, u->version);
        out_s(o, "</A>");
        if (u->nnotes) {
            out_s(o, "<P>New in ");
            out_text(o, u->version);
            out_s(o, ":");
            /* each line cut to 32 characters, so a long one doesn't widen the page; the
               whole story is on the site */
            /* at most two lines here; more, and the last bullet says there's more on the site */
            for (k = 0; k < u->nnotes && k < NOTES_SHOWN; k++) {
                char cut[NOTE_CUT + 1];
                size_t len = strlen(u->notes[k]);
                out_s(o, "<BR>&nbsp;&nbsp;\245&nbsp;");   /* a bullet, without a list's blank line above */
                if (len <= NOTE_CUT) out_text(o, u->notes[k]);
                else {
                    memcpy(cut, u->notes[k], NOTE_CUT);
                    len = NOTE_CUT;
                    while (len > 0 && cut[len - 1] == ' ') len--;
                    cut[len] = 0;
                    out_text(o, cut);
                    out_s(o, "\311");
                }
            }
            out_s(o, "<BR>&nbsp;&nbsp;\245&nbsp;<A HREF=\"");
            out_text(o, site);
            out_s(o, u->nnotes > NOTES_SHOWN ? "\">See more\311</A>" : "\">Details\311</A>");
        }
    } else {
        out_s(o, "<P>Mill is up to date.");
    }
    free(text);
    free(u);
}

/* The picture and name on the right; on the left, what Mill is, how much memory is free,
   and the button that checks for updates (check: it was pressed) */
static void about(Out *o, int check)
{
    long free, total;
    char line[64];
    head(o, 200, HTML_MAC);
    out_s(o,
        "<TITLE>About Mill</TITLE>"
        "<TABLE ALIGN=CENTER BORDER=0 CELLPADDING=4>"
        "<TR><TD ROWSPAN=3 VALIGN=MIDDLE>"
        "<IMG SRC=\"mill://splash\" WIDTH=192 HEIGHT=163 ALT=\"Don Quixote and the windmills\">"
        "<TD VALIGN=TOP><BR><H1>Mill</H1>"
        "<TR><TD VALIGN=MIDDLE>");
    mem_report(&free, &total);
    snprintf(line, sizeof line, "Memory: %ldK free of %ldK", free / 1024, total / 1024);
    out_s(o, line);
    /* the button, or once pressed, what it found in its place */
    if (check) check_for_updates(o);
    else out_s(o,
        "<FORM METHOD=GET ACTION=\"mill://about\"><INPUT TYPE=hidden NAME=check VALUE=1>"
        "<INPUT TYPE=submit VALUE=\"Check for updates\311\"></FORM>");
    out_s(o,
        "<TR><TD VALIGN=BOTTOM>Version " MILL_VERSION " &middot; Freeware<BR>&copy; " MILL_YEAR " <A HREF=\"" MILL_PUBLISHER_SITE "\">" MILL_PUBLISHER "</A>"
        "</TABLE><HR>"
        "<H3>About Mill</H3>"
        "<P>Mill is a capable web browser for monochrome Macs. It is the most feature-complete, no-nonsense "
        "browser available for the Plus and later."
        "<P><B>System Requirements:</B><UL>"
        "<LI>Mac Plus or later"
        "<LI>System 6.0.3 with MacTCP or later"
        "<LI>700K free RAM</UL>"
        "<H3>What Mill reads</H3><UL>"
        "<LI>HTML 3.2, which included text, lists, tables, images (and maps), and forms, as well as a few extras "
        "like bgcolor and font face. No applets, JavaScript, style sheets, or frames."
        "<LI>URL schemes: http://, https:// (through a proxy), file://, gopher://, ftp://, and mill:// (built-ins), "
        "and more with Plug-Ins"
        "<LI>Character sets: UTF-8, ISO 8859-1, and Mac Roman"
        "<LI>Formats: GIF (static), JPEG (baseline), Netpbm, and gzip; more with Plug-Ins"
        "<LI>Pages up to 192K for rendering, but larger for downloads"
        "<LI>Cookies: kept until you quit Mill</UL>"
        "<H3>Built-in pages</H3><UL>"
        "<LI><A HREF=\"mill://about\">mill://about</A>, this page"
        "<LI><A HREF=\"mill://config\">mill://config</A>, settings"
        "<LI><A HREF=\"mill://plugins\">mill://plugins</A>, plug-ins"
        "<LI><A HREF=\"mill://license\">mill://license</A>, licenses</UL>"
        "<H3>Keys</H3><DL>"
        "<DT>Command-L<DD>Type an address"
        "<DT>Command-O<DD>Open a file on this Mac"
        "<DT>Command-[ and Command-]<DD>Back and forward"
        "<DT>Command-R<DD>Reload"
        "<DT>Command-+ and Command-minus<DD>Larger and smaller text (View menu)"
        "<DT>Command-H<DD>Go to the home page"
        "<DT>Command-period or Escape<DD>Stop loading (or click the stop sign)"
        "<DT>Command-1 to Command-0<DD>Open a bookmark"
        "<DT>Command-D<DD>Bookmark this page"
        "<DT>Option-Command and a number<DD>Bookmark this page in that slot"
        "<DT>Control-click, or hold the button down<DD>Save an image, a link, or the page's source</DL>"
        "<P><A HREF=\"" MILL_SITE "changes.html\">Change log</A>"
        "<H3>Credits</H3><UL>"
        "<LI>The drawing is by George A. Harker, from James Baldwin's <I>Stories of Don Quixote</I> (1910), "
        "now in the public domain."
        "<LI>JPEG decoding by ChaN's <A HREF=\"mill://license#tjpgd\">TJpgDec</A>."
        "<LI>Built with Wolfgang Thaller's Retro68, including its "
        "<A HREF=\"mill://license#runtime\">runtime</A>, the <A HREF=\"mill://license#newlib\">newlib</A> "
        "C library, and <A HREF=\"mill://license#multiversal\">Multiversal Interfaces</A>."
        "<LI>Source code under the <A HREF=\"mill://license#mill\">MIT License</A>.</UL>");
}

/* ---------------------------------------------------------------- mill://license */

/* A license is a resource, 'TXGZ' (gzip, what Mill's own and the font file and Finger use) or
   'TEXT' (plain, for authors without a gzip tool), ID 128 in Mill and 129 in a Plug-In
   or font file, named "License". It's read only when this page is built, unpacked as it is
   sent through the built-in gzip code, and released. */
#define LICENSE_ID_FILE 129
#define LICENSE_ID_MILL 128

typedef struct { Out *o; int escape; } LicOut;

static int lic_write(void *ref, const unsigned char *p, long n)
{
    LicOut *l = ref;
    if (l->escape) out_text_n(l->o, (const char *)p, n);
    else out(l->o, (const char *)p, n);
    return l->o->stopped;
}

/* The current resource file's license, sent to o (escaped, as plain text, if escape); false if
   it has none */
static Boolean license_text(Out *o, short id, int escape)
{
    LicOut l;
    Handle h = Get1Resource('TXGZ', id);
    l.o = o;
    l.escape = escape;
    if (h) {
        void *gz;
        HLock(h);
        if ((gz = gz_begin(lic_write, &l))) {
            gz_data(gz, (const unsigned char *)*h, GetHandleSize(h));
            gz_end(gz);
        }
        HUnlock(h);
        ReleaseResource(h);
        return true;
    }
    if ((h = Get1Resource('TEXT', id))) {
        HLock(h);
        lic_write(&l, (const unsigned char *)*h, GetHandleSize(h));
        HUnlock(h);
        ReleaseResource(h);
        return true;
    }
    return false;
}

/* A heading's anchor: lower case letters and digits, the rest a hyphen ("Web Fonts" is web-fonts) */
static void license_anchor(const char *name, char *out_anchor, int size)
{
    int n = 0;
    for (; *name && n < size - 1; name++)
        out_anchor[n++] = (*name >= 'A' && *name <= 'Z') ? *name + 32 :
                          ((*name >= 'a' && *name <= 'z') || (*name >= '0' && *name <= '9')) ? *name : '-';
    out_anchor[n] = 0;
}

static void license_section(Out *o, const char *name, short ref)
{
    char anchor[40];
    short was = CurResFile();
    license_anchor(name, anchor, sizeof anchor);
    out_s(o, "<HR><H3><A NAME=\"");
    out_s(o, anchor);
    out_s(o, "\">");
    out_text(o, name);
    out_s(o, "</A></H3>");
    if (ref > 0) {
        Boolean found;
        UseResFile(ref);
        out_s(o, "<PRE>");
        found = license_text(o, LICENSE_ID_FILE, 1);
        out_s(o, found ? "</PRE>" : "</PRE><P>This file has no license text.");
        UseResFile(was);
    } else {
        out_s(o, "<P>This file has no license text.");
    }
}

/* Mill's own text (with the notices for what's built in), then a section for each Plug-In file
   and for Web Fonts, from the license each carries */
static void license_page(Out *o)
{
    short i;
    head(o, 200, HTML_MAC);
    if (!license_text(o, LICENSE_ID_MILL, 0)) { out_s(o, "<TITLE>Licenses</TITLE><H2>Licenses</H2>"); }
    out_s(o, "<HR><H2>Plug-In files</H2>");
    for (i = 0; i < nplugs && !o->stopped; i++) {
        const Plug *p = &plugs[i];
        short ref;
        if (p->builtin) continue;
        ref = file_open_res(&p->spec, fsRdPerm);
        license_section(o, p->name ? p->name : p->file, ref == -1 ? 0 : ref);
        if (ref != -1) CloseResFile(ref);
    }
    for (i = 0; i < npack_files && !o->stopped; i++) license_section(o, pack_files[i].file, pack_files[i].ref);
}


/* ------------------------------------------------------------ Plug-Ins' settings */

static void setting_name(const PlugProto *pp, const MillSetting *st, char *name)
{
    if (plugin_setting_name(pp->scheme, st->key, name)) name[0] = 0;
}

static const char *setting_now(const PlugProto *pp, const MillSetting *st)
{
    char name[48];
    setting_name(pp, st, name);
    return setting(name, st->dflt);
}

/* Each protocol a Plug-In has, under its own heading, after HTTP and HTTPS */
static void plugin_settings_form(Out *o)
{
    short i, k, j;
    for (i = 0; i < nplugs; i++) {
        if (plugs[i].status != PLUG_READY) continue;
        for (k = 0; k < plugs[i].nproto; k++) {
            const PlugProto *pp = &plugs[i].protos[k];
            out_s(o, "<H4><A NAME=\"");
            out_text(o, pp->scheme);
            out_s(o, "\">");
            out_text(o, pp->heading);
            out_s(o, "</A></H4>");
            /* Mill's own first row: whether the protocol is on at all */
            out_s(o, SETTINGS_TABLE "<TR><TD VALIGN=TOP WIDTH=110><B>Enable</B><TD><INPUT TYPE=checkbox VALUE=yes NAME=\"");
            out_text(o, pp->scheme);
            out_s(o, plugin_enabled(pp->scheme) ? ".enable\" CHECKED>" : ".enable\">");
            if (pp->description[0]) { out_s(o, " "); out_text(o, pp->description); }
            for (j = 0; j < pp->nset; j++) {
                const MillSetting *st = &pp->sets[j];
                const char *v = setting_now(pp, st);
                char name[48];
                setting_name(pp, st, name);
                out_s(o, "<TR><TD VALIGN=TOP WIDTH=110><B>");
                out_text(o, st->label);
                out_s(o, "</B><TD>");
                if (st->kind == millCheckbox) {
                    out_s(o, "<INPUT TYPE=checkbox VALUE=yes NAME=\"");
                    out_text(o, name);
                    out_s(o, !strcmp(v, "yes") ? "\" CHECKED>" : "\">");
                    if (st->note) { out_s(o, " "); out_text(o, st->note); }
                    continue;
                }
                if (st->kind == millChoice && st->choices) {
                    const char *c = st->choices;
                    out_s(o, "<SELECT NAME=\"");
                    out_text(o, name);
                    out_s(o, "\">");
                    while (*c) {
                        const char *e = strchr(c, '|');
                        long n = e ? e - c : (long)strlen(c);
                        out_s(o, (long)strlen(v) == n && !memcmp(v, c, n) ? "<OPTION SELECTED>" : "<OPTION>");
                        out_text_n(o, c, n);
                        c += n + (e != NULL);
                    }
                    out_s(o, "</SELECT>");
                } else {
                    out_s(o, st->kind == millNumber ? "<INPUT SIZE=6 NAME=\"" : "<INPUT SIZE=30 NAME=\"");
                    out_text(o, name);
                    out_s(o, "\" VALUE=\"");
                    out_text(o, v);
                    out_s(o, "\">");
                }
                if (st->note) { out_s(o, "<BR>"); out_text(o, st->note); }
            }
            out_s(o, "</TABLE>");
        }
    }
}

/* Each format a Plug-In reads: Enable and what it is, and its types and extensions */
static void format_settings_form(Out *o)
{
    short i, k;
    for (i = 0; i < nplugs; i++) {
        if (plugs[i].status != PLUG_READY) continue;
        for (k = 0; k < plugs[i].nformat; k++) {
            const PlugFormat *f = &plugs[i].formats[k];
            const char *e;
            out_s(o, "<H4><A NAME=\"");
            out_text(o, f->key);
            out_s(o, "\">");
            out_text(o, f->heading);
            out_s(o, "</A></H4>" SETTINGS_TABLE "<TR><TD VALIGN=TOP WIDTH=110><B>Enable</B><TD><INPUT TYPE=checkbox VALUE=yes NAME=\"");
            out_text(o, f->key);
            out_s(o, format_enabled(f) ? ".enable\" CHECKED> " : ".enable\"> ");
            out_text(o, f->description);
            /* its types, then its extensions with dots */
            out_s(o, "<BR>");
            out_text(o, f->types);
            for (e = f->extensions; *e; ) {
                long n;
                while (*e == ' ') e++;
                if (!*e) break;
                n = strcspn(e, " ");
                out_s(o, " .");
                out_text_n(o, e, n);
                e += n;
            }
            out_s(o, "</TABLE>");
        }
    }
}

/* Saved as SCHEME.KEY, once each is checked; note says what's wrong */
static void plugin_settings_save(const char *body, long len, char *note, int note_size)
{
    short i, k, j;
    for (i = 0; i < nplugs; i++) {
        if (plugs[i].status != PLUG_READY) continue;
        for (k = 0; k < plugs[i].nformat; k++) {
            char name[48], v[8];
            snprintf(name, sizeof name, "%.15s.enable", plugs[i].formats[k].key);
            setting_put(name, form_field(body, len, name, v, sizeof v) ? "yes" : "no");
        }
        for (k = 0; k < plugs[i].nproto; k++) {
            const PlugProto *pp = &plugs[i].protos[k];
            {
                char name[48], v[8];
                snprintf(name, sizeof name, "%.15s.enable", pp->scheme);
                setting_put(name, form_field(body, len, name, v, sizeof v) ? "yes" : "no");
            }
            for (j = 0; j < pp->nset; j++) {
                const MillSetting *st = &pp->sets[j];
                char name[48], v[256], *c;
                setting_name(pp, st, name);
                if (st->kind == millCheckbox) { setting_put(name, form_field(body, len, name, v, sizeof v) ? "yes" : "no"); continue; }
                if (!form_field(body, len, name, v, sizeof v)) continue;
                for (c = v; *c; c++) if ((unsigned char)*c < ' ') *c = ' ';  /* one line in the settings file */
                if (st->kind == millNumber) {
                    long n = atol(v);
                    if (v[0] < '0' || v[0] > '9' || n < st->min || n > st->max) {
                        snprintf(note, note_size, "%s %s must be from %ld to %ld.", pp->heading, st->label, st->min, st->max);
                        return;
                    }
                    sprintf(v, "%ld", n);
                }
                setting_put(name, v);
            }
        }
    }
}

static const char *const plug_status[] = { "", "needs a newer Mill", "name already used", "skipped (Shift held)", "couldn't start" };

/* One protocol's or format's line: its link to its settings, what it is, and its state */
static void plugin_line(Out *o, const Plug *p, const char *key, const char *label, const char *description, Boolean on)
{
    out_s(o, "<BR>&nbsp;&nbsp;<A HREF=\"mill://config#");
    out_text(o, key);
    out_s(o, "\">");
    out_text(o, label);
    out_s(o, "</A> ");
    out_text(o, description);
    out_s(o, on ? " &middot; On" : " &middot; Off");
    out_s(o, p->builtin ? ", built in" : ", file");
}

/* mill://plugins: the index of Plug-Ins, as Netscape's about:plugins listed them */
static void plugins_page(Out *o)
{
    short i, k;
    head(o, 200, HTML_MAC);
    out_s(o, "<TITLE>Plug-Ins</TITLE><H2>Plug-Ins</H2>");
    for (i = 0; i < nplugs; i++) {
        const Plug *p = &plugs[i];
        out_s(o, "<P><B>");
        out_text(o, p->name ? p->name : p->file);
        out_s(o, "</B>");
        if (p->version) { out_s(o, " "); out_text(o, p->version); }
        if (p->by && p->by[0]) { out_s(o, " by "); out_text(o, p->by); }
        if (p->description && p->description[0]) { out_s(o, "<BR>"); out_text(o, p->description); }
        {
            char anchor[40];
            license_anchor(p->builtin ? "Mill" : (p->name ? p->name : p->file), anchor, sizeof anchor);
            out_s(o, "<BR><A HREF=\"mill://license#");
            out_s(o, anchor);
            out_s(o, "\">License</A>");
        }
        if (!p->builtin) {
            char path[256];
            spec_to_path(&p->spec, path, sizeof path);
            out_s(o, "<BR>File: ");
            out_text(o, path);
        }
        if (p->status != PLUG_READY) { out_s(o, "<BR>Not loaded: "); out_s(o, plug_status[p->status]); }
        for (k = 0; k < p->nproto; k++) {
            char label[24];
            snprintf(label, sizeof label, "%.20s:", p->protos[k].scheme);
            plugin_line(o, p, p->protos[k].scheme, label, p->protos[k].description, plugin_enabled(p->protos[k].scheme));
        }
        for (k = 0; k < p->nformat; k++)
            plugin_line(o, p, p->formats[k].key, p->formats[k].heading, p->formats[k].description, format_enabled(&p->formats[k]));
    }
    for (i = 0; i < npack_files; i++) {
        char anchor[40];
        short k;
        license_anchor(pack_files[i].file, anchor, sizeof anchor);
        out_s(o, "<P><B>");
        out_text(o, pack_files[i].file);
        out_s(o, "</B><BR>Fonts:");
        for (k = 0; k < npack_fams; k++)
            if (pack_fams[k].ref == pack_files[i].ref) { out_s(o, " "); out_text(o, pack_fams[k].name); }
        out_s(o, "<BR><A HREF=\"mill://license#");
        out_s(o, anchor);
        out_s(o, "\">License</A> &middot; file");
    }
    if (!nplugs && !npack_files) out_s(o, "<P>None.");
}

/* A font setting's row: a list of the fonts to choose from */
static void font_row(Out *o, const char *label, const char *key, int fixed, const char *note)
{
    static char names[40][FONT_NAME_MAX];
    char name[40];
    int n = font_list(fixed, names, 40), i;
    snprintf(name, sizeof name, "font_%s", key);
    out_s(o, "<TR><TD VALIGN=TOP WIDTH=110><B>");
    out_s(o, label);
    out_s(o, "</B><TD><SELECT NAME=");
    out_s(o, name);
    out_s(o, ">");
    {
        const char *cur = setting(name, font_default(fixed));
        int found = 0;
        for (i = 0; i < n; i++) if (!strcmp(names[i], cur)) found = 1;
        if (!found) cur = font_default(fixed);
        for (i = 0; i < n; i++) {
            out_s(o, strcmp(names[i], cur) ? "<OPTION>" : "<OPTION SELECTED>");
            out_text(o, names[i]);
        }
    }
    out_s(o, "</SELECT><BR>");
    out_s(o, note);
}

static void config(Out *o, const char *body, long len)
{
    static char types[2048], typed[2048];
    char note[120] = "", home[256], where[300], k[16], agent[141];
    const char *shown = NULL;   /* rules typed with a mistake in them, shown again to fix */
    FSSpec spec;
    if (body) {
        Url check;
        if (form_field(body, len, "homepage", home, sizeof home)) {
            if (home[0] && url_parse(home, &check)) snprintf(note, sizeof note, "That home page isn't an address Mill can open.");
            else put_setting("homepage", 8, home, strlen(home));
        }
        if (form_field(body, len, "image_memory", k, sizeof k)) {
            long n = atol(k);
            if (n < 0 || n > 4096 || (k[0] && (k[0] < '0' || k[0] > '9'))) snprintf(note, sizeof note, "Image memory must be from 0 to 4096 K.");
            else { sprintf(k, "%ld", n); put_setting("image_memory", 12, k, strlen(k)); }
        }
        if (form_field(body, len, "history", k, sizeof k)) {
            long n = atol(k);
            if (n < 1 || n > MILL_HISTORY_MAX || k[0] < '0' || k[0] > '9')
                snprintf(note, sizeof note, "Window history must be from 1 to %d.", MILL_HISTORY_MAX);
            else { sprintf(k, "%ld", n); put_setting("history", 7, k, strlen(k)); }
        }
        /* a checkbox that isn't checked isn't sent */
        {
            static const char *const keys[] = { "font_prop", "font_fixed" };
            char font[FONT_NAME_MAX + 4];
            int f;
            for (f = 0; f < 2; f++)
                if (form_field(body, len, keys[f], font, sizeof font)) put_setting(keys[f], strlen(keys[f]), font, strlen(font));
            fonts_choose();
        }
        if (form_field(body, len, "scale_text_only", k, sizeof k)) put_setting("scale_text_only", 15, "yes", 3);
        else put_setting("scale_text_only", 15, "no", 2);
        fonts_choose();
        if (form_field(body, len, "load_images", k, sizeof k)) put_setting("load_images", 11, "yes", 3);
        else put_setting("load_images", 11, "no", 2);
        if (form_field(body, len, "decode_macbinary", k, sizeof k)) put_setting("decode_macbinary", 16, "yes", 3);
        else put_setting("decode_macbinary", 16, "no", 2);
        {
            char as[20];
            if (form_field(body, len, "upload_as", as, sizeof as) &&
                (!strcmp(as, "Automatic") || !strcmp(as, "Data fork only") || !strcmp(as, "MacBinary") || !strcmp(as, "BinHex")))
                setting_put("upload_as", as);
        }
        setting_put("upload_ext", form_field(body, len, "upload_ext", k, sizeof k) ? "yes" : "no");
        if (form_field(body, len, "cookies", k, sizeof k)) put_setting("cookies", 7, "yes", 3);
        else put_setting("cookies", 7, "no", 2);
        if (form_field(body, len, "user_agent", agent, sizeof agent)) {
            char *c;
            for (c = agent; *c; c++) if ((unsigned char)*c < ' ') *c = ' ';  /* one header line */
            put_setting("user_agent", 10, agent, strlen(agent));
        }
        if (form_field(body, len, "print_headers", k, sizeof k)) put_setting("print_headers", 13, "yes", 3);
        else put_setting("print_headers", 13, "no", 2);
        if (form_field(body, len, "print_backgrounds", k, sizeof k)) put_setting("print_backgrounds", 17, "yes", 3);
        else put_setting("print_backgrounds", 17, "no", 2);
        if (form_field(body, len, "file_types", types, sizeof types)) {
            int bad;
            strcpy(typed, types);
            if ((bad = join_rules(types))) {
                snprintf(note, sizeof note, "Line %d of File types isn't a valid rule.", bad);
                shown = typed;
            } else {
                put_setting("file_types", 10, types, strlen(types));
            }
        }
        {
            /* bookmarks: a title and an address each; no address empties the slot */
            int n;
            for (n = 0; n < BOOKMARKS && !note[0]; n++) {
                char fname[8], url[300], title[100];
                Url u;
                sprintf(fname, "bu%d", n + 1);
                if (!form_field(body, len, fname, url, sizeof url)) continue;
                sprintf(fname, "bt%d", n + 1);
                if (!form_field(body, len, fname, title, sizeof title)) title[0] = 0;
                if (url[0] && url_parse(url, &u)) snprintf(note, sizeof note, "Bookmark %d isn't an address Mill can open.", n == 9 ? 0 : n + 1);
                else bookmark_put(n, url, title);
            }
        }
        if (!note[0]) plugin_settings_save(body, len, note, sizeof note);
        if (!note[0]) {
            OSErr err = settings_write();
            if (err == noErr) strcpy(note, "Saved.");
            else snprintf(note, sizeof note, "Mill couldn't save its settings (error %d).", err);
        }
    }
    head(o, 200, HTML_MAC);
    out_s(o, "<TITLE>Mill Settings</TITLE><H2>Settings</H2>");
    if (note[0]) { out_s(o, "<P><B>"); out_text(o, note); out_s(o, "</B>"); }
    /* each section a table: the setting's name, then its control and what it does */
    out_s(o, "<P><A HREF=\"#general\">General</A> | <A HREF=\"#protocols\">Protocols</A> | <A HREF=\"#formats\">Formats</A> | <A HREF=\"#printing\">Printing</A> | <A HREF=\"#bookmarks\">Bookmarks</A>"
             "<FORM METHOD=POST ACTION=\"mill://config\"><H3><A NAME=general>General</A></H3>"
             SETTINGS_TABLE
             "<TR><TD VALIGN=TOP WIDTH=110><B>Home page</B><TD><INPUT NAME=homepage SIZE=36 VALUE=\"");
    out_text(o, setting("homepage", MILL_DEFAULT_HOME));
    out_s(o, "\"><BR>Opens at startup and with Home in the Go menu."
             "<TR><TD VALIGN=TOP WIDTH=110><B>Image memory</B><TD><INPUT NAME=image_memory SIZE=5 VALUE=\"");
    sprintf(k, "%ld", image_memory_k());
    out_s(o, k);
    out_s(o, "\"> K<BR>Memory for caching images, much faster when images are reused between pages; 0 to turn off."
             "<TR><TD VALIGN=TOP WIDTH=110><B>Window history</B><TD><INPUT NAME=history SIZE=3 VALUE=\"");
    sprintf(k, "%d", history_size());
    out_s(o, k);
    out_s(o, "\"><BR>Pages remembered for Back, 1 to 32.");
    font_row(o, "Proportional font", "prop", 0, "Most text.");
    font_row(o, "Fixed font", "fixed", 1, "PRE, TT, CODE, KBD, SAMP, text files, and listings.");
    out_s(o, "<TR><TD VALIGN=TOP WIDTH=110><B>Scale Text Only</B><TD><INPUT TYPE=checkbox NAME=scale_text_only VALUE=yes");
    if (!strcmp(setting("scale_text_only", "no"), "yes")) out_s(o, " CHECKED");
    out_s(o, "> Mill scales only the text when zooming (lower memory use)");
    out_s(o, "<TR><TD VALIGN=TOP WIDTH=110><B>Load images automatically</B><TD><INPUT TYPE=checkbox NAME=load_images VALUE=yes");
    if (auto_images()) out_s(o, " CHECKED");
    out_s(o, "> When off, use the context menu to load individually."
             "<TR><TD VALIGN=TOP WIDTH=110><B>Accept cookies</B><TD><INPUT TYPE=checkbox NAME=cookies VALUE=yes");
    if (accept_cookies()) out_s(o, " CHECKED");
    out_s(o, "> Lets sites remember you, as after signing in; kept until you quit."
             "<TR><TD VALIGN=TOP WIDTH=110><B>User agent</B><TD><INPUT NAME=user_agent SIZE=36 VALUE=\"");
    out_text(o, user_agent());
    out_s(o, "\"><BR>Empty sends none."
             "<TR><TD VALIGN=TOP WIDTH=110><B>File types</B><TD>Mac type and creator for saved files, so the Finder knows "
             "what opens them. One rule per line: MIME type, file name, TYPE/CREATOR; * matches anything, and "
             "the first match wins. Empty uses Mill's defaults.<BR><TEXTAREA NAME=file_types ROWS=8 COLS=36>");
    out_rules(o, shown ? shown : file_type_rules());
    out_s(o, "</TEXTAREA><TR><TD VALIGN=TOP WIDTH=110><B>Decode MacBinary</B>"
             "<TD><INPUT TYPE=checkbox NAME=decode_macbinary VALUE=yes");
    if (decode_macbinary()) out_s(o, " CHECKED");
    out_s(o, "> Automatically decode MacBinary if detected in a .bin download."
             "<TR><TD VALIGN=TOP WIDTH=110><B>Upload Mac files as</B><TD><SELECT NAME=upload_as>");
    {
        static const char *const as[] = { "Automatic", "Data fork only", "MacBinary", "BinHex" };
        const char *now = setting("upload_as", "Automatic");
        int k;
        for (k = 0; k < 4; k++) {
            out_s(o, strcmp(now, as[k]) ? "<OPTION>" : "<OPTION SELECTED>");
            out_s(o, as[k]);
        }
    }
    out_s(o, "</SELECT>"
             "<TR><TD VALIGN=TOP WIDTH=110><B>Add upload file extension</B><TD><INPUT TYPE=checkbox NAME=upload_ext VALUE=yes");
    if (strcmp(setting("upload_ext", "yes"), "no")) out_s(o, " CHECKED");
    out_s(o, "> Automatically appends .bin or .hqx if uploading a file as MacBinary or BinHex</TABLE>"
             "<H3><A NAME=protocols>Protocols</A></H3>");
    plugin_settings_form(o);
    out_s(o, "<H3><A NAME=formats>Formats</A></H3>");
    format_settings_form(o);
    out_s(o, "<H3><A NAME=printing>Printing</A></H3>" SETTINGS_TABLE
             "<TR><TD VALIGN=TOP WIDTH=110><B>Print headers and footers</B><TD><INPUT TYPE=checkbox NAME=print_headers VALUE=yes");
    if (print_headers()) out_s(o, " CHECKED");
    out_s(o, "> The page's title at the top of each sheet; its address, the page number, and the date at the bottom."
             "<TR><TD VALIGN=TOP WIDTH=110><B>Print backgrounds</B><TD><INPUT TYPE=checkbox NAME=print_backgrounds VALUE=yes");
    if (print_backgrounds()) out_s(o, " CHECKED");
    out_s(o, "> Prints dark table cells black, as on screen. When off, they print white with black text."
             "</TABLE><H3><A NAME=bookmarks>Bookmarks</A></H3>" SETTINGS_TABLE "<TR><TH><TH>Title<TH>Address");
    {
        int n;
        for (n = 0; n < BOOKMARKS; n++) {
            char url[300], title[100], line[80];
            if (!bookmark(n, url, sizeof url, title, sizeof title)) url[0] = title[0] = 0;
            sprintf(line, "<TR><TD ALIGN=CENTER><TT>%d</TT><TD><INPUT NAME=bt%d SIZE=16 VALUE=\"", n == 9 ? 0 : n + 1, n + 1);
            out_s(o, line);
            out_text(o, title);
            sprintf(line, "\"><TD><INPUT NAME=bu%d SIZE=36 VALUE=\"", n + 1);
            out_s(o, line);
            out_text(o, url);
            out_s(o, "\">");
        }
    }
    out_s(o, "</TABLE>");
    out_s(o, "<P><INPUT TYPE=submit VALUE=\"Save\"></FORM><HR><P>Settings are saved in ");
    if (settings_spec(&spec) == noErr) {
        spec_to_path(&spec, where, sizeof where);
        out_s(o, "<TT>");
        out_text(o, where);
        out_s(o, "</TT>");
    } else {
        out_s(o, "the Preferences folder");
    }
    out_s(o, ", which you can also edit with a text editor.");
}

static void mill_request(const Url *u, const char *body, long len, Out *o)
{
    if (!strcmp(u->host, "about")) about(o, strstr(u->path, "check=1") != NULL);
    else if (!strcmp(u->host, "config")) config(o, body, len);
    else if (!strcmp(u->host, "plugins")) plugins_page(o);
    else if (!strcmp(u->host, "license")) license_page(o);
    else {
        head(o, 404, HTML_MAC);
        out_s(o, "<TITLE>Not found</TITLE><H2>Not found</H2>");
    }
}

OSErr local_request(const Url *u, const char *content_type, const char *body, long body_len, net_sink sink, void *ref)
{
    Out o;
    o.sink = sink;
    o.ref = ref;
    o.stopped = 0;
    mill_request(u, content_type ? body : NULL, body_len, &o);
    return noErr;
}
