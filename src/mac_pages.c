/* Mill's own pages, mill:about, config, plugins, and license, answered as a
   web server would. In a code segment of their own (mill.segmap), unloaded after each
   page, so they only take memory while one is being made. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/charset.h"
#include "core/config.h"
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
    if (update_site(MILL_UPDATE_URL, site, sizeof site)) strcpy(why, "Its address isn\325t one Mill can open.");
    else if (text && u && local_fetch) {
        n = local_fetch(MILL_UPDATE_URL, text, UPDATE_MAX_BYTES + 1, why, sizeof why);
        if (n > UPDATE_MAX_BYTES) { n = -1; strcpy(why, "Its answer was too long."); }
        if (n >= 0 && (err = update_parse(text, n, site, u)) != 0) {
            n = -1;
            if (err == UPDATE_OFF_SITE) {
                /* site without its scheme and last slash: mill.1in1e6.com */
                const char *host = strstr(site, "://") + 3;
                snprintf(why, sizeof why, "Its download link isn\325t on %.*s.", (int)(strlen(host) - 1), host);
            } else {
                strcpy(why, "Its answer wasn\325t a version number.");
            }
        }
    }
    if (n < 0) {
        out_s(o, "<P>Mill couldn\325t check for updates. ");
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
        "<LI>Mac Plus or later, 1 MB RAM"
        "<LI>System 6.0.3, MacTCP 1.1, or later"
        "<LI>700K free RAM"
        "<LI>Recommended: System 6.0.8, 2 MB RAM</UL>"
        "<H3>What Mill reads</H3><UL>"
        "<LI>HTML 3.2, which included text, lists, tables, images (and maps), and forms, as well as a few extras "
        "like bgcolor and font face. No applets, JavaScript, style sheets, or frames."
        "<LI>URL schemes: http://, https:// (through a proxy), file://, mill://, and more with Plug-Ins"
        "<LI>Character sets: UTF-8, ISO 8859-1, and Mac Roman"
        "<LI>Formats: GIF (static) and JPEG (baseline); more with Plug-Ins"
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
        "<DT>Control-click, or hold down the mouse button<DD>Save an image, a link, or the page\325s source</DL>"
        "<P><A HREF=\"" MILL_SITE "changes.html\">Change log</A>"
        "<H3>Credits</H3><UL>"
        "<LI>The drawing is by George A. Harker, from James Baldwin\325s <I>Stories of Don Quixote</I> (1910), "
        "now in the public domain."
        "<LI>JPEG decoding by ChaN\325s <A HREF=\"mill://license#tjpgd\">TJpgDec</A>."
        "<LI>Built with Wolfgang Thaller\325s Retro68, including its "
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
        out_s(o, "<PRE><FONT SIZE=2>");
        found = license_text(o, LICENSE_ID_FILE, 1);
        out_s(o, found ? "</FONT></PRE>" : "</FONT></PRE><P>This file has no license text.");
        UseResFile(was);
    } else {
        out_s(o, "<P>This file has no license text.");
    }
}

/* Mill's own text (with the notices for what's built in), then a section for each Plug-In file
   and for Web Fonts, from the license each carries */
static void license_page(Out *o)
{
    short i, was = CurResFile();
    head(o, 200, HTML_MAC);
    UseResFile(LMGetCurApRefNum());     /* Mill's own: a font file above it has a TEXT 128 too */
    if (!license_text(o, LICENSE_ID_MILL, 0)) { out_s(o, "<TITLE>Licenses</TITLE><H2>Licenses</H2>"); }
    UseResFile(was);
    out_s(o, "<HR><H2>Plug-In files</H2>");
    for (i = 0; i < nplugs && !o->stopped; i++) {
        const Plug *p = &plugs[i];
        short ref;
        if (p->builtin) continue;
        ref = file_open_res(&p->spec, fsRdPerm);
        license_section(o, p->file, ref == -1 ? 0 : ref);   /* the name in the Finder */
        if (ref != -1) CloseResFile(ref);
    }
    for (i = 0; i < npack_files && !o->stopped; i++) license_section(o, pack_files[i].file, pack_files[i].ref);
}


/* ------------------------------------------------------------ Plug-Ins' settings */

static void setting_name(const char *scheme, const MillSetting *st, char *name)
{
    if (plugin_setting_name(scheme, st->key, name)) name[0] = 0;
}

static const char *setting_now(const char *scheme, const MillSetting *st)
{
    char name[48];
    setting_name(scheme, st, name);
    return setting(name, st->dflt);
}

/* A protocol's or a format's own settings (scheme: its scheme or key), a row each, after
   its Enable row */
static void setting_rows(Out *o, const char *scheme, const MillSetting *sets, short n)
{
    short j;
    for (j = 0; j < n; j++) {
        const MillSetting *st = &sets[j];
        const char *v = setting_now(scheme, st);
        char name[48];
        setting_name(scheme, st, name);
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
            short k = 0;
            /* each sent back as its number: a form sends Latin-1, which hasn't
               Mac Roman's curly quotes */
            out_s(o, "<SELECT NAME=\"");
            out_text(o, name);
            out_s(o, "\">");
            while (*c) {
                const char *e = strchr(c, '|');
                long n = e ? e - c : (long)strlen(c);
                char opt[40];
                sprintf(opt, "<OPTION VALUE=%d%s>", k++, (long)strlen(v) == n && !memcmp(v, c, n) ? " SELECTED" : "");
                out_s(o, opt);
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
}

/* A section's own FORM, after its heading: which section it is, and "Saved." if it was just saved */
static void section_start(Out *o, const char *sec, const char *saved)
{
    out_s(o, "<FORM METHOD=POST ACTION=\"mill://config\"><INPUT TYPE=hidden NAME=section VALUE=\"");
    out_text(o, sec);
    out_s(o, "\">");
    if (!strcmp(sec, saved)) out_s(o, "<P><B>Saved.</B>");
}

/* Its Save button, under its last row */
static void section_end(Out *o) { out_s(o, "<P><INPUT TYPE=submit VALUE=\"Save\"></FORM>"); }

/* Each protocol a Plug-In has, under its own heading, after HTTP and HTTPS */
static void plugin_settings_form(Out *o, const char *saved)
{
    short i, k;
    for (i = 0; i < nplugs; i++) {
        if (plugs[i].status != PLUG_READY) continue;
        for (k = 0; k < plugs[i].nproto; k++) {
            const PlugProto *pp = &plugs[i].protos[k];
            out_s(o, "<H4><A NAME=\"");
            out_text(o, pp->scheme);
            out_s(o, "\">");
            out_text(o, pp->heading);
            out_s(o, "</A></H4>");
            section_start(o, pp->scheme, saved);
            /* Mill's own first row: whether the protocol is on at all */
            out_s(o, SETTINGS_TABLE "<TR><TD VALIGN=TOP WIDTH=110><B>Enable</B><TD><INPUT TYPE=checkbox VALUE=yes NAME=\"");
            out_text(o, pp->scheme);
            out_s(o, plugin_enabled(pp->scheme) ? ".enable\" CHECKED>" : ".enable\">");
            if (pp->description[0]) { out_s(o, " "); out_text(o, pp->description); }
            setting_rows(o, pp->scheme, pp->sets, pp->nset);
            out_s(o, "</TABLE>");
            section_end(o);
        }
    }
}

/* Each format a Plug-In reads (not one replaced): Enable and what it is, and its types and
   extensions, then its own settings */
static void format_settings_form(Out *o, const char *saved)
{
    short i, k;
    for (i = 0; i < nplugs; i++) {
        if (plugs[i].status != PLUG_READY) continue;
        for (k = 0; k < plugs[i].nformat; k++) {
            const PlugFormat *f = &plugs[i].formats[k];
            const char *e;
            if (f->replaced_by) continue;
            out_s(o, "<H4><A NAME=\"");
            out_text(o, f->key);
            out_s(o, "\">");
            out_text(o, f->heading);
            out_s(o, "</A></H4>");
            section_start(o, f->key, saved);
            out_s(o, SETTINGS_TABLE "<TR><TD VALIGN=TOP WIDTH=110><B>Enable</B><TD><INPUT TYPE=checkbox VALUE=yes NAME=\"");
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
            setting_rows(o, f->key, f->sets, f->nset);
            out_s(o, "</TABLE>");
            section_end(o);
        }
    }
}

/* One protocol's or format's settings from the form, each checked; false, with note saying
   what's wrong, if one isn't right */
static Boolean settings_save(const char *body, long len, const char *scheme, const char *heading,
                             const MillSetting *sets, short nset, char *note, int note_size)
{
    short j;
    char name[48], v[256], *c;
    snprintf(name, sizeof name, "%.15s.enable", scheme);
    setting_put(name, form_field(body, len, name, v, sizeof v) ? "yes" : "no");
    for (j = 0; j < nset; j++) {
        const MillSetting *st = &sets[j];
        setting_name(scheme, st, name);
        if (st->kind == millCheckbox) { setting_put(name, form_field(body, len, name, v, sizeof v) ? "yes" : "no"); continue; }
        if (!form_field(body, len, name, v, sizeof v)) continue;
        for (c = v; *c; c++) if ((unsigned char)*c < ' ') *c = ' ';  /* one line in the settings file */
        if (st->kind == millChoice && st->choices) {
            /* its number (see the page): the choice's own text is kept */
            const char *ch = st->choices, *e;
            long n = atol(v);
            for (; n > 0 && (e = strchr(ch, '|')); n--) ch = e + 1;
            if (n) continue;
            snprintf(v, sizeof v, "%.*s", (int)((e = strchr(ch, '|')) ? e - ch : (long)strlen(ch)), ch);
        }
        if (st->kind == millNumber) {
            long n = atol(v);
            const char *d = v[0] == '-' ? v + 1 : v;      /* a minus, where the range goes below 0 */
            if (*d < '0' || *d > '9' || n < st->min || n > st->max) {
                snprintf(note, note_size, "%s %s must be from %ld to %ld.", heading, st->label, st->min, st->max);
                return false;
            }
            sprintf(v, "%ld", n);
        }
        setting_put(name, v);
    }
    return true;
}

/* The protocol's or format's own section sec, saved as SCHEME.KEY (or a format's KEY.KEY), once
   each is checked; note says what's wrong. False if sec isn't one. */
static Boolean plugin_settings_save(const char *sec, const char *body, long len, char *note, int note_size)
{
    short i, k;
    for (i = 0; i < nplugs; i++) {
        if (plugs[i].status != PLUG_READY) continue;
        for (k = 0; k < plugs[i].nformat; k++) {
            const PlugFormat *f = &plugs[i].formats[k];
            if (f->replaced_by || strcmp(f->key, sec)) continue;
            settings_save(body, len, f->key, f->heading, f->sets, f->nset, note, note_size);
            return true;
        }
        for (k = 0; k < plugs[i].nproto; k++) {
            const PlugProto *pp = &plugs[i].protos[k];
            if (strcmp(pp->scheme, sec)) continue;
            settings_save(body, len, pp->scheme, pp->heading, pp->sets, pp->nset, note, note_size);
            return true;
        }
    }
    return false;
}

static const char *const plug_status[] = { "", "needs a newer Mill", "name already used", "skipped (Shift held)", "couldn\325t start",
                                           "made for an older Mill" };

/* One protocol's or format's line: its link to its settings, what it is, and its state */
static void plugin_line(Out *o, const Plug *p, const char *key, const char *label, const char *description, Boolean on)
{
    (void)p;
    out_s(o, "<BR>&nbsp;&nbsp;<A HREF=\"mill://config#");
    out_text(o, key);
    out_s(o, "\">");
    out_text(o, label);
    out_s(o, "</A> ");
    out_text(o, description);
    out_s(o, on ? " &middot; On" : " &middot; Off");
}

/* Where a format is from, for a line about another: "JPEG in Built-In" */
static void format_from(Out *o, const PlugFormat *f)
{
    out_text(o, f->heading);
    out_s(o, " in ");
    out_text(o, f->plug->builtin ? "Built-In" : f->plug->file);
}

/* A format's line, and which format it replaced, or which replaced it (MillPlugIn.h, Formats) */
static void format_line(Out *o, const Plug *p, const PlugFormat *f)
{
    if (f->replaced_by) {
        out_s(o, "<BR>&nbsp;&nbsp;");
        out_text(o, f->heading);
        out_s(o, " ");
        out_text(o, f->description);
        out_s(o, " &middot; replaced by ");
        format_from(o, f->replaced_by);
        return;
    }
    plugin_line(o, p, f->key, f->heading, f->description, format_enabled(f));
    if (f->replaces) {
        out_s(o, " &middot; replaces ");
        format_from(o, f->replaces);
    }
}

static Boolean has_about_text(const Plug *p);

/* A file's heading on mill://plugins: its name, then what it says about itself */
static void plugin_heading(Out *o, const char *title, const char *version, const char *by, const char *description)
{
    out_s(o, "<P><B>");
    out_text(o, title);
    out_s(o, "</B>");
    if (version && version[0]) { out_s(o, " "); out_text(o, version); }
    if (by && by[0]) { out_s(o, " by "); out_text(o, by); }
    if (description && description[0]) { out_s(o, "<BR>"); out_text(o, description); }
}

/* License and About under a file's heading */
static void plugin_links(Out *o, const char *license_of, const char *about)
{
    char anchor[40];
    license_anchor(license_of, anchor, sizeof anchor);
    out_s(o, "<BR><A HREF=\"mill://license#");
    out_s(o, anchor);
    out_s(o, "\">License</A>");
    if (about) {
        out_s(o, " &middot; <A HREF=\"");
        out_s(o, about);
        out_s(o, "\">About</A>");
    }
}

static void plugin_lines(Out *o, const Plug *p)
{
    short k;
    if (p->status != PLUG_READY) { out_s(o, "<BR>Not loaded: "); out_s(o, plug_status[p->status]); }
    for (k = 0; k < p->nproto; k++) {
        char label[24];
        snprintf(label, sizeof label, "%.20s:", p->protos[k].scheme);
        plugin_line(o, p, p->protos[k].scheme, label, p->protos[k].description, plugin_enabled(p->protos[k].scheme));
    }
    for (k = 0; k < p->nformat; k++) format_line(o, p, &p->formats[k]);
}

/* mill://plugins: one entry for what's built into Mill, then one for each file in the Plug-Ins
   folder, Plug-Ins and font files alike, as Netscape's about:plugins listed them */
static void plugins_page(Out *o)
{
    short i, k;
    head(o, 200, HTML_MAC);
    out_s(o, "<TITLE>Plug-Ins</TITLE><H2>Plug-Ins</H2>");
    plugin_heading(o, "Built-In", MILL_VERSION, MILL_PUBLISHER, "Part of Mill.");
    plugin_links(o, "Mill", "mill://about");
    for (i = 0; i < nplugs; i++) if (plugs[i].builtin) plugin_lines(o, &plugs[i]);
    for (i = 0; i < nplugs; i++) {
        const Plug *p = &plugs[i];
        char path[256], about[80];
        if (p->builtin) continue;
        plugin_heading(o, p->file, p->version, p->by, p->description);
        snprintf(about, sizeof about, "plugin://%s/about", p->name ? p->name : "");
        plugin_links(o, p->file, p->name && ((p->flags & millHasAbout) || has_about_text(p)) ? about : NULL);
        spec_to_path(&p->spec, path, sizeof path);
        out_s(o, "<BR>File: ");
        out_text(o, path);
        plugin_lines(o, p);
    }
    for (i = 0; i < npack_files; i++) {
        char anchor[40], about[60];
        license_anchor(pack_files[i].file, anchor, sizeof anchor);
        snprintf(about, sizeof about, "plugin://%s/about", anchor);
        char version[32] = "", path[256];
        short n = 0;
        Handle h;
        {
            short was = CurResFile();
            UseResFile(pack_files[i].ref);
            if ((h = Get1Resource('vers', 1))) {          /* the short version string, after 6 bytes */
                unsigned char len = (*h)[6];
                if (len < sizeof version) { memcpy(version, *h + 7, len); version[len] = 0; }
                ReleaseResource(h);
            }
            UseResFile(was);
        }
        for (k = 0; k < npack_fams; k++) if (pack_fams[k].ref == pack_files[i].ref) n++;
        plugin_heading(o, pack_files[i].file, version, MILL_PUBLISHER, NULL);
        out_s(o, "<BR>Fonts: ");
        for (k = 0; k < npack_fams; k++)
            if (pack_fams[k].ref == pack_files[i].ref) {
                out_text(o, pack_fams[k].name);
                if (--n > 0) out_s(o, n == 1 ? " and " : ", ");
            }
        out_s(o, ".");
        if (font_plug.res == pack_files[i].ref && font_plug.status == PLUG_READY) {
            char from[40];
            snprintf(from, sizeof from, " Outlines from %d point.", plug_fonts[0].min);
            out_s(o, from);
        }
        plugin_links(o, pack_files[i].file, about);
        {
            FCBPBRec fcb;
            Str63 name;
            FSSpec spec;
            memset(&fcb, 0, sizeof fcb);
            fcb.ioRefNum = pack_files[i].ref;
            fcb.ioNamePtr = name;
            if (PBGetFCBInfoSync(&fcb) == noErr) {        /* (no FSMakeFSSpec on System 6) */
                spec.vRefNum = fcb.ioFCBVRefNum;
                spec.parID = fcb.ioFCBParID;
                memcpy(spec.name, name, name[0] + 1);
                spec_to_path(&spec, path, sizeof path);
                out_s(o, "<BR>File: ");
                out_text(o, path);
            }
        }
    }
}

/* A file's own About text ('TEXT' 128) as a page, for a font file (plugin://NAME/about, NAME its
   name as an anchor: "web-fonts") or a Plug-In that doesn't answer /about itself: its lines joined into
   paragraphs (a blank line between) and its indented lines kept as lines, to wrap to the window */
static void text_about(Out *o, const char *title, short res)
{
    short was = CurResFile();
    Handle h;
    head(o, 200, HTML_MAC);
    out_s(o, "<TITLE>");
    out_text(o, title);
    out_s(o, "</TITLE><H2>");
    out_text(o, title);
    out_s(o, "</H2>");
    UseResFile(res);
    if ((h = Get1Resource('TEXT', 128))) {
        long n = GetHandleSize(h), at = 0, end;
        Boolean indented = false;
        HLock(h);
        out_s(o, "<P>");
        while (at < n) {
            const char *t = *h;
            for (end = at; end < n && t[end] != '\r' && t[end] != '\n'; end++) ;
            if (end == at) { out_s(o, "<P>"); indented = false; }
            else {
                if (t[at] == ' ') out_s(o, "<BR>&nbsp;&nbsp;");
                else if (indented) out_s(o, "<BR>");
                indented = t[at] == ' ';
                out_text_n(o, t + at, end - at);
                out_s(o, " ");
            }
            at = end + 1;
        }
        HUnlock(h);
        ReleaseResource(h);
    } else out_s(o, "<P>This file has no About text.");
    UseResFile(was);
}

/* Whether a Plug-In's file has About text of its own ('TEXT' 128), for Mill to show */
static Boolean has_about_text(const Plug *p)
{
    short ref, was = CurResFile();
    Boolean has = false;
    if (p->builtin || (ref = file_open_res(&p->spec, fsRdPerm)) == -1) return false;
    UseResFile(ref);
    SetResLoad(false);
    has = Get1Resource('TEXT', 128) != NULL;
    SetResLoad(true);
    CloseResFile(ref);
    UseResFile(was);
    return has;
}

/* A plugin:// address Mill answers for the file: a font file's (it has no code), or the About
   page of a Plug-In that doesn't make its own. True if it's one (and answered). */
Boolean font_file_request(const Url *u, net_sink sink, void *ref)
{
    short k;
    Boolean about = !strcmp(u->path, "/about");
    for (k = 0; k < nplugs; k++) {
        const Plug *p = &plugs[k];
        short res;
        if (p->builtin || !p->name || strcmp(u->host, p->name) || (p->flags & millHasAbout) || !about) continue;
        if ((res = file_open_res(&p->spec, fsRdPerm)) == -1) return false;
        {
            Out o;
            o.sink = sink;
            o.ref = ref;
            o.stopped = 0;
            text_about(&o, p->file, res);
        }
        CloseResFile(res);
        return true;
    }
    for (k = 0; k < npack_files; k++) {
        char anchor[40];
        license_anchor(pack_files[k].file, anchor, sizeof anchor);
        if (!strcmp(u->host, anchor)) {
            Out o;
            o.sink = sink;
            o.ref = ref;
            o.stopped = 0;
            if (about || !strcmp(u->path, "/") || !u->path[0]) text_about(&o, pack_files[k].file, pack_files[k].ref);
            else {
                head(&o, 404, HTML_MAC);
                out_s(&o, "<TITLE>Not Found</TITLE><H2>Not Found</H2><P>A font file has only an About page.");
            }
            return true;
        }
    }
    return false;
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

/* mill://config, and a section's form sent to it: saved, then the page asked for again with
   ?saved=SECTION#SECTION, so it says so at that section and Reload doesn't send it again */
static void config(Out *o, const Url *u, const char *body, long len)
{
    static char typed[2048], rules[2048];
    char note[120] = "", where[300], k[16], saved[32] = "";
    const char *shown = NULL;   /* rules typed with a mistake in them, shown again to fix */
    FSSpec spec;
    {
        const char *q = strstr(u->path, "saved=");
        if (q) snprintf(saved, sizeof saved, "%.*s", (int)strcspn(q + 6, "&"), q + 6);
    }
    if (body) {
        const ConfigIO io = { form_field, setting_put, bookmark_put, MILL_HISTORY_MAX, MILL_KEEP_ALIVE_MAX,
                              BOOKMARKS, MILL_DEFAULT_PING };
        char sec[32];
        if (config_section(&io, body, len, sec, sizeof sec)) {
            if (config_save(&io, body, len, sec, note, sizeof note, typed, rules, sizeof typed)) {
                if (typed[0]) shown = typed;
                if (!strcmp(sec, CONFIG_GENERAL)) { fonts_choose(); net_keep_secs = keep_alive_secs(); }
            } else if (!plugin_settings_save(sec, body, len, note, sizeof note)) sec[0] = 0;
            if (sec[0] && !note[0]) {
                OSErr err = settings_write();
                if (err == noErr) {
                    char h[160];
                    snprintf(h, sizeof h, "HTTP/1.0 303 See Other\r\nLocation: mill://config?saved=%s#%s\r\n\r\n", sec, sec);
                    out_s(o, h);
                    return;
                }
                snprintf(note, sizeof note, "Mill couldn\325t save its settings (error %d).", err);
            }
        }
    }
    head(o, 200, HTML_MAC);
    out_s(o, "<TITLE>Mill Settings</TITLE><H2>Settings</H2>");
    if (note[0]) { out_s(o, "<P><B>"); out_text(o, note); out_s(o, "</B>"); }
    /* each section a table: the setting's name, then its control and what it does */
    out_s(o, "<P><A HREF=\"#general\">General</A> | <A HREF=\"#check\">Connection check</A> | <A HREF=\"#protocols\">Protocols</A> | <A HREF=\"#formats\">Formats</A> | <A HREF=\"#printing\">Printing</A> | <A HREF=\"#bookmarks\">Bookmarks</A>"
             "<P>After changing a section, click Save at the bottom of that section. "
             "Changes to more than one section at a time are lost."
             "<H3><A NAME=general>General</A></H3>");
    section_start(o, CONFIG_GENERAL, saved);
    out_s(o, SETTINGS_TABLE
             "<TR><TD VALIGN=TOP WIDTH=110><B>Home page</B><TD><INPUT NAME=homepage SIZE=36 VALUE=\"");
    out_text(o, setting("homepage", MILL_DEFAULT_HOME));
    out_s(o, "\"><BR>Opens at startup and with Home in the Go menu."
             "<TR><TD VALIGN=TOP WIDTH=110><B>Show welcome page on next launch</B><TD><INPUT TYPE=checkbox NAME=welcome VALUE=yes");
    if (!strcmp(setting("welcome", "yes"), "yes")) out_s(o, " CHECKED");
    out_s(o, "> Opens " MILL_WELCOME_SHOWN " the next time Mill starts."
             "<TR><TD VALIGN=TOP WIDTH=110><B>Image memory</B><TD><INPUT NAME=image_memory SIZE=5 VALUE=\"");
    sprintf(k, "%ld", image_memory_k());
    out_s(o, k);
    out_s(o, "\"> K<BR>Memory for caching images, much faster when images are reused between pages; 0 to turn off."
             "<TR><TD VALIGN=TOP WIDTH=110><B>Window history</B><TD><INPUT NAME=history SIZE=3 VALUE=\"");
    sprintf(k, "%d", history_size());
    out_s(o, k);
    out_s(o, "\"><BR>Pages remembered for Back, 1 to 32."
             "<TR><TD VALIGN=TOP WIDTH=110><B>Keep-alive</B><TD><INPUT NAME=keep_alive SIZE=5 VALUE=\"");
    sprintf(k, "%ld", keep_alive_secs());
    out_s(o, k);
    out_s(o, "\"> seconds<BR>Idle time before closing a connection to a server; 0 to turn off.");
    font_row(o, "Proportional font", "prop", 0, "Default font for page text.");
    font_row(o, "Fixed font", "fixed", 1, "Monospace font for fixed-width text.");
    out_s(o, "<TR><TD VALIGN=TOP WIDTH=110><B>Scale Text Only</B><TD><INPUT TYPE=checkbox NAME=scale_text_only VALUE=yes");
    if (!strcmp(setting("scale_text_only", "no"), "yes")) out_s(o, " CHECKED");
    out_s(o, "> Zoom scales text only. Uses less memory.");
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
             "the first match wins. Empty uses Mill\325s defaults.<BR><TEXTAREA NAME=file_types ROWS=8 COLS=36>");
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
    out_s(o, "> Automatically appends .bin or .hqx if uploading a file as MacBinary or BinHex</TABLE>");
    section_end(o);
    out_s(o, "<H3><A NAME=check>Connection check</A></H3>");
    section_start(o, CONFIG_CHECK, saved);
    out_s(o, SETTINGS_TABLE
             "<TR><TD VALIGN=TOP WIDTH=110><B>Check internet connectivity at launch</B><TD><INPUT TYPE=checkbox NAME=net_check VALUE=yes");
    if (!strcmp(setting("net_check", "yes"), "yes")) out_s(o, " CHECKED");
    out_s(o, "> Warns at launch if the server below doesn\325t reply."
             "<TR><TD VALIGN=TOP WIDTH=110><B>Server to ping</B><TD><INPUT NAME=ping_host SIZE=36 VALUE=\"");
    out_text(o, setting("ping_host", MILL_DEFAULT_PING));
    out_s(o, "\"><BR>A name or an address.</TABLE>");
    section_end(o);
    out_s(o, "<H3><A NAME=protocols>Protocols</A></H3>");
    plugin_settings_form(o, saved);
    out_s(o, "<H3><A NAME=formats>Formats</A></H3>");
    format_settings_form(o, saved);
    out_s(o, "<H3><A NAME=printing>Printing</A></H3>");
    section_start(o, CONFIG_PRINTING, saved);
    out_s(o, SETTINGS_TABLE
             "<TR><TD VALIGN=TOP WIDTH=110><B>Print headers and footers</B><TD><INPUT TYPE=checkbox NAME=print_headers VALUE=yes");
    if (print_headers()) out_s(o, " CHECKED");
    out_s(o, "> The page\325s title at the top of each sheet; its address, the page number, and the date at the bottom."
             "<TR><TD VALIGN=TOP WIDTH=110><B>Print backgrounds</B><TD><INPUT TYPE=checkbox NAME=print_backgrounds VALUE=yes");
    if (print_backgrounds()) out_s(o, " CHECKED");
    out_s(o, "> Prints dark table cells black, as on screen. When off, they print white with black text.</TABLE>");
    section_end(o);
    out_s(o, "<H3><A NAME=bookmarks>Bookmarks</A></H3>");
    section_start(o, CONFIG_BOOKMARKS, saved);
    out_s(o, SETTINGS_TABLE "<TR><TH><TH>Title<TH>Address");
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
    section_end(o);
    out_s(o, "<HR><P>Settings are saved in ");
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
    else if (!strcmp(u->host, "config")) config(o, u, body, len);
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
