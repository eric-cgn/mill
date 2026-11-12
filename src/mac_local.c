#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/charset.h"
#include "core/filetypes.h"
#include "core/update.h"
#include "mac_local.h"
#include "mac_sys.h"
#include "version.h"

#define ROOT_PARENT 1                   /* fsRtParID: what a volume's root folder is in */
#define IS_FOLDER 0x10
#define IS_INVISIBLE 0x4000
#define MAX_FILE (192L * 1024)
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

static void pstr(const char *c, Str255 p)
{
    long n = strlen(c);
    if (n > 255) n = 255;
    p[0] = (unsigned char)n;
    memcpy(p + 1, c, n);
}

/* ------------------------------------------------------------ paths and file: URLs */

void spec_to_path(const FSSpec *s, char *path, int size)
{
    static char buf[600];
    CInfoPBRec pb;
    Str255 name;
    long dir = s->parID;
    int at = sizeof buf - 1;
    buf[at] = 0;
    at -= s->name[0];
    memcpy(buf + at, s->name + 1, s->name[0]);
    while (dir != ROOT_PARENT && at > 300) {
        memset(&pb, 0, sizeof pb);
        pb.dirInfo.ioNamePtr = name;
        pb.dirInfo.ioVRefNum = s->vRefNum;
        pb.dirInfo.ioFDirIndex = -1;
        pb.dirInfo.ioDrDirID = dir;
        if (PBGetCatInfoSync(&pb) != noErr) break;
        buf[--at] = ':';
        at -= name[0];
        memcpy(buf + at, name + 1, name[0]);
        dir = pb.dirInfo.ioDrParID;
    }
    snprintf(path, size, "%s%s", buf + at, s->parID == ROOT_PARENT ? ":" : "");
}

/* A Mac path as a file: URL: colons become slashes, and the rest is percent-encoded */
static void path_to_url(const char *path, char *url, int size)
{
    int o = snprintf(url, size, "file:///");
    for (; *path && o < size - 4; path++) {
        unsigned char c = (unsigned char)*path;
        if (c == ':') url[o++] = '/';
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.~", c)) url[o++] = c;
        else o += sprintf(url + o, "%%%02X", c);
    }
    url[o] = 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c |= 32;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* A file: URL's path ("/Macintosh%20HD/Web/x.html") as a Mac path ("Macintosh HD:Web:x.html") */
static void url_to_path(const char *p, char *path, int size)
{
    int o = 0;
    if (*p == '/') p++;
    for (; *p && *p != '?' && o < size - 2; p++) {
        char c = *p;
        if (c == '/') c = ':';
        else if (c == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) { c = (char)(hexval(p[1]) * 16 + hexval(p[2])); p += 2; }
        path[o++] = c;
    }
    path[o] = 0;
    if (o && !strchr(path, ':')) { path[o++] = ':'; path[o] = 0; }  /* a volume by itself is "Name:" */
}

void spec_to_url(const FSSpec *spec, char *url, int size)
{
    char path[300];
    CInfoPBRec pb;
    Str255 name;
    spec_to_path(spec, path, sizeof path);
    memcpy(name, spec->name, spec->name[0] + 1);
    memset(&pb, 0, sizeof pb);
    pb.hFileInfo.ioNamePtr = name;
    pb.hFileInfo.ioVRefNum = spec->vRefNum;
    pb.hFileInfo.ioDirID = spec->parID;
    if (PBGetCatInfoSync(&pb) == noErr && (pb.hFileInfo.ioFlAttrib & IS_FOLDER) && path[strlen(path) - 1] != ':')
        strcat(path, ":");
    path_to_url(path, url, size);
}

long read_mac_file(const char *path, char **data, long max)
{
    Str255 p;
    FSSpec spec;
    short ref;
    long len;
    pstr(path, p);
    if (file_spec(0, 0, p, &spec) != noErr || file_open(&spec, fsRdPerm, &ref) != noErr) return -1;
    GetEOF(ref, &len);
    if (len > max) len = max;
    if (!(*data = malloc(len + 1)) || FSRead(ref, &len, *data) != noErr) { free(*data); FSClose(ref); return -1; }
    FSClose(ref);
    return len;
}

static void not_found(Out *o, const char *what)
{
    head(o, 404, HTML_MAC);
    out_s(o, "<TITLE>Not found</TITLE><H2>Not found</H2><P>There's nothing at <TT>");
    out_text(o, what);
    out_s(o, "</TT>.");
}

static void list_volumes(Out *o)
{
    HParamBlockRec pb;
    Str255 name;
    short i;
    head(o, 200, HTML_MAC);
    out_s(o, "<TITLE>Disks</TITLE><H2>Disks</H2><UL>");
    for (i = 1; !o->stopped; i++) {
        char cname[64], url[300];
        memset(&pb, 0, sizeof pb);
        pb.volumeParam.ioNamePtr = name;
        pb.volumeParam.ioVolIndex = i;
        if (PBHGetVInfoSync(&pb) != noErr) break;
        memcpy(cname, name + 1, name[0]);
        cname[name[0]] = ':';
        cname[name[0] + 1] = 0;
        path_to_url(cname, url, sizeof url);
        out_s(o, "<LI><A HREF=\"");
        out_text(o, url);
        out_s(o, "\">");
        cname[name[0]] = 0;
        out_text(o, cname);
        out_s(o, "</A>");
    }
    out_s(o, "</UL>");
}

/* A folder's contents as a page; links are whole URLs, so the folder's own URL doesn't
   need its trailing slash */
static void list_folder(Out *o, short vref, long dir, const char *path)
{
    CInfoPBRec pb;
    Str255 name;
    char child[600], url[700], parent[300];
    short i;
    int len = strlen(path);
    head(o, 200, HTML_MAC);
    out_s(o, "<TITLE>");
    out_text(o, path);
    out_s(o, "</TITLE><H2>");
    out_text(o, path);
    out_s(o, "</H2><P>");
    /* up: the folder that holds this one, or the list of disks */
    strcpy(parent, path);
    if (len && parent[len - 1] == ':') parent[--len] = 0;
    while (len > 0 && parent[len - 1] != ':') parent[--len] = 0;
    path_to_url(parent, url, sizeof url);
    out_s(o, "<A HREF=\"");
    out_text(o, url);
    out_s(o, "\">Up</A><UL>");
    for (i = 1; !o->stopped; i++) {
        int folder, hidden;
        memset(&pb, 0, sizeof pb);
        pb.hFileInfo.ioNamePtr = name;
        pb.hFileInfo.ioVRefNum = vref;
        pb.hFileInfo.ioDirID = dir;
        pb.hFileInfo.ioFDirIndex = i;
        if (PBGetCatInfoSync(&pb) != noErr) break;
        folder = (pb.hFileInfo.ioFlAttrib & IS_FOLDER) != 0;
        hidden = folder ? (pb.dirInfo.ioDrUsrWds.frFlags & IS_INVISIBLE) : (pb.hFileInfo.ioFlFndrInfo.fdFlags & IS_INVISIBLE);
        if (hidden) continue;
        snprintf(child, sizeof child, "%s%s%.*s%s", path, path[strlen(path) - 1] == ':' ? "" : ":",
                 name[0], (char *)name + 1, folder ? ":" : "");
        path_to_url(child, url, sizeof url);
        out_s(o, "<LI><A HREF=\"");
        out_text(o, url);
        out_s(o, "\">");
        child[0] = 0;
        snprintf(child, sizeof child, "%.*s", name[0], (char *)name + 1);
        out_text(o, child);
        out_s(o, folder ? "</A> (folder)" : "</A>");
    }
    out_s(o, "</UL>");
}

static int ext_is(const char *path, const char *ext)
{
    const char *dot = strrchr(path, '.');
    if (!dot || strchr(dot, ':')) return 0;
    for (dot++; *dot && *ext; dot++, ext++) if ((*dot | 32) != *ext) return 0;
    return !*dot && !*ext;
}

static void send_file(Out *o, FSSpec *spec, OSType type, const char *path)
{
    const char *ctype;
    char chunk[1024];
    short ref;
    long n, total = 0;
    if (ext_is(path, "html") || ext_is(path, "htm")) ctype = HTML_MAC;
    else if (ext_is(path, "gif") || type == 'GIFf') ctype = "image/gif";
    else if (ext_is(path, "jpg") || ext_is(path, "jpeg") || type == 'JPEG') ctype = "image/jpeg";
    else if (type == 'TEXT' || ext_is(path, "txt")) ctype = "text/plain; charset=macintosh";
    else {
        head(o, 200, HTML_MAC);
        out_s(o, "<TITLE>Can't show this file</TITLE><P>Mill can't show <TT>");
        out_text(o, path);
        out_s(o, "</TT>. It shows text, HTML, GIF, and JPEG files only.");
        return;
    }
    if (file_open(spec, fsRdPerm, &ref) != noErr) { not_found(o, path); return; }
    head(o, 200, ctype);
    do {
        n = sizeof chunk;
        FSRead(ref, &n, chunk);
        out(o, chunk, n);
        total += n;
    } while (n == sizeof chunk && total < MAX_FILE && !o->stopped);
    FSClose(ref);
}

static void file_request(const Url *u, Out *o)
{
    char path[300];
    Str255 p, name;
    FSSpec spec;
    CInfoPBRec pb;
    url_to_path(u->path, path, sizeof path);
    if (!path[0]) { list_volumes(o); return; }
    pstr(path, p);
    if (strlen(path) > 255 || file_spec(0, 0, p, &spec) != noErr) { not_found(o, path); return; }
    /* a disk itself: its root folder, which System 6 won't look up by name in folder 1 */
    if (spec.parID == ROOT_PARENT) { list_folder(o, spec.vRefNum, 2, path); return; }
    memcpy(name, spec.name, spec.name[0] + 1);
    memset(&pb, 0, sizeof pb);
    pb.hFileInfo.ioNamePtr = name;
    pb.hFileInfo.ioVRefNum = spec.vRefNum;
    pb.hFileInfo.ioDirID = spec.parID;
    if (PBGetCatInfoSync(&pb) != noErr) { not_found(o, path); return; }
    if (pb.hFileInfo.ioFlAttrib & IS_FOLDER) list_folder(o, spec.vRefNum, pb.dirInfo.ioDrDirID, path);
    else send_file(o, &spec, pb.hFileInfo.ioFlFndrInfo.fdType, path);
}

/* ------------------------------------------------------------ settings */

#define MAX_SETTINGS 32
static char *names[MAX_SETTINGS], *values[MAX_SETTINGS];
static int nsettings;

static OSErr settings_spec(FSSpec *spec)
{
    short vref;
    long dir;
    OSErr err;
    if ((err = sys_folder(kPreferencesFolderType, &vref, &dir)) != noErr) return err;
    err = file_spec(vref, dir, "\pMill Settings", spec);
    return err == fnfErr ? noErr : err;
}

static char *copy(const char *s, long n)
{
    char *c = malloc(n + 1);
    if (c) { memcpy(c, s, n); c[n] = 0; }
    return c;
}

static void put_setting(const char *name, long nlen, const char *value, long vlen)
{
    int i;
    for (i = 0; i < nsettings; i++) {
        if ((long)strlen(names[i]) == nlen && !memcmp(names[i], name, nlen)) {
            char *v = copy(value, vlen);
            if (v) { free(values[i]); values[i] = v; }
            return;
        }
    }
    if (nsettings == MAX_SETTINGS) return;
    names[nsettings] = copy(name, nlen);
    values[nsettings] = copy(value, vlen);
    if (names[nsettings] && values[nsettings]) nsettings++;
}

const char *setting(const char *name, const char *dflt)
{
    int i;
    for (i = 0; i < nsettings; i++) if (!strcmp(names[i], name)) return values[i];
    return dflt;
}

/* One name=value on each line; lines starting with # are notes */
void settings_load(void)
{
    FSSpec spec;
    short ref;
    long len;
    char *text, *p, *end;
    if (settings_spec(&spec) != noErr || file_open(&spec, fsRdPerm, &ref) != noErr) return;
    GetEOF(ref, &len);
    if (len > 16384) len = 16384;
    if ((text = malloc(len + 1)) && FSRead(ref, &len, text) == noErr) {
        text[len] = 0;
        for (p = text; *p; p = end + (*end != 0)) {
            char *eq;
            long n;
            for (end = p; *end && *end != '\r' && *end != '\n'; end++) {}
            n = end - p;
            while (n > 0 && *p == ' ') { p++; n--; }
            if (n == 0 || *p == '#' || !(eq = memchr(p, '=', n))) continue;
            {
                long nl = eq - p, vs = nl + 1;
                while (nl > 0 && p[nl - 1] == ' ') nl--;
                while (vs < n && p[vs] == ' ') vs++;
                if (nl) put_setting(p, nl, p + vs, n - vs);
            }
        }
    }
    free(text);
    FSClose(ref);
}

static OSErr settings_save(void)
{
    FSSpec spec;
    short ref;
    long n;
    int i;
    OSErr err;
    static const char header[] = "# Mill's settings: one name=value on each line. Saving in mill://config rewrites this file.\r";
    if ((err = settings_spec(&spec)) != noErr) return err;
    err = file_create(&spec, 'ttxt', 'TEXT');  /* SimpleText opens it */
    if (err != noErr && err != dupFNErr) return err;
    if ((err = file_open(&spec, fsWrPerm, &ref)) != noErr) return err;
    SetEOF(ref, 0);
    n = strlen(header);
    err = FSWrite(ref, &n, header);
    for (i = 0; i < nsettings && err == noErr; i++) {
        n = strlen(names[i]); err = FSWrite(ref, &n, names[i]);
        n = 1; if (err == noErr) err = FSWrite(ref, &n, "=");
        n = strlen(values[i]); if (err == noErr) err = FSWrite(ref, &n, values[i]);
        n = 1; if (err == noErr) err = FSWrite(ref, &n, "\r");
    }
    FSClose(ref);
    FlushVol(NULL, spec.vRefNum);
    return err;
}

long image_memory_k(void)
{
    const char *v = setting("image_memory", NULL);
    return v ? atol(v) : MILL_DEFAULT_IMAGE_K;
}

int history_size(void)
{
    const char *v = setting("history", NULL);
    long n = v ? atol(v) : MILL_DEFAULT_HISTORY;
    return n < 1 ? 1 : n > MILL_HISTORY_MAX ? MILL_HISTORY_MAX : (int)n;
}

int auto_images(void)
{
    return strcmp(setting("load_images", "yes"), "no") != 0;
}

const char *file_type_rules(void)
{
    const char *v = setting("file_types", "");
    return v[0] ? v : FILETYPES_DEFAULT;
}

int decode_macbinary(void)
{
    return strcmp(setting("decode_macbinary", "yes"), "no") != 0;
}

int print_headers(void)
{
    return strcmp(setting("print_headers", "yes"), "no") != 0;
}

int print_backgrounds(void)
{
    return strcmp(setting("print_backgrounds", "no"), "yes") == 0;
}

int accept_cookies(void)
{
    return strcmp(setting("cookies", "yes"), "no") != 0;
}

const char *user_agent(void)
{
    return setting("user_agent", MILL_DEFAULT_AGENT);
}

const char *proxy(void)
{
    return setting("proxy", "");
}

int proxy_on(void)
{
    return !strcmp(setting("proxy_on", "no"), "yes");
}

const char *proxy_excluded(void)
{
    return setting("proxy_exclude", "");
}

int proxy_https(void)
{
    return !strcmp(setting("proxy_https", "no"), "yes");
}

/* Each slot is a setting, bookmark1 to bookmark10: "address title" (addresses have no spaces) */
static void bookmark_name(int n, char *name)
{
    sprintf(name, "bookmark%d", (n + 1) % 100);
}

int bookmark(int n, char *url, int url_size, char *title, int title_size)
{
    char name[24];
    const char *v, *sp;
    long ul;
    bookmark_name(n, name);
    v = setting(name, "");
    if (!v[0]) return 0;
    sp = strchr(v, ' ');
    ul = sp ? sp - v : (long)strlen(v);
    snprintf(url, url_size, "%.*s", (int)ul, v);
    snprintf(title, title_size, "%s", sp ? sp + 1 : "");
    return 1;
}

static void bookmark_put(int n, const char *url, const char *title)
{
    char name[24], v[400], *c;
    long ul = strcspn(url ? url : "", " \r\n");
    bookmark_name(n, name);
    if (!url || !ul) { put_setting(name, strlen(name), "", 0); return; }
    snprintf(v, sizeof v, "%.*s %s", (int)ul, url, title ? title : "");
    for (c = v; *c; c++) if ((unsigned char)*c < ' ') *c = ' ';  /* one line in the settings file */
    put_setting(name, strlen(name), v, strlen(v));
}

OSErr bookmark_set(int n, const char *url, const char *title)
{
    bookmark_put(n, url, title);
    return settings_save();
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
static void check_for_updates(Out *o)
{
    char why[120], *text = malloc(UPDATE_MAX_BYTES + 1);
    Update *u = malloc(sizeof *u);
    long n = -1;
    int k;
    strcpy(why, "Not enough memory.");
    if (text && u && local_fetch) {
        n = local_fetch(MILL_UPDATE_URL, text, UPDATE_MAX_BYTES + 1, why, sizeof why);
        if (n > UPDATE_MAX_BYTES) { n = -1; strcpy(why, "Its answer was too long."); }
        if (n >= 0 && update_parse(text, n, u)) { n = -1; strcpy(why, "Its answer wasn't a version number."); }
    }
    if (n < 0) {
        out_s(o, "<P>Mill couldn't check for updates. ");
        out_text(o, why);
    } else if (update_compare(u->version, MILL_VERSION) > 0) {
        out_s(o, "<P><B>A new version is available!</B><BR>Mill ");
        out_text(o, u->version);
        out_s(o, "<BR><A HREF=\"");
        out_text(o, u->link);
        out_s(o, "\">Click here to download</A>");
        if (u->nnotes) {
            out_s(o, "<P>New in ");
            out_text(o, u->version);
            out_s(o, ":<UL>");
            for (k = 0; k < u->nnotes; k++) { out_s(o, "<LI>"); out_text(o, u->notes[k]); }
            out_s(o, "</UL>");
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
    head(o, 200, HTML_MAC);
    out_s(o,
        "<TITLE>About Mill</TITLE>"
        "<TABLE WIDTH=\"100%\" BORDER=0 CELLPADDING=4><TR><TD VALIGN=TOP>"
        "A web browser for classic Macs<BR>"
        "Version " MILL_VERSION " &middot; Freeware<BR>&copy; " MILL_YEAR " " MILL_PUBLISHER);
    {
        long free, total;
        char line[64];
        mem_report(&free, &total);
        snprintf(line, sizeof line, "<BR>Memory: %ldK free of %ldK", free / 1024, total / 1024);
        out_s(o, line);
    }
    out_s(o,
        "<FORM METHOD=GET ACTION=\"mill://about\"><INPUT TYPE=hidden NAME=check VALUE=1>"
        "<INPUT TYPE=submit VALUE=\"Check for updates\311\"></FORM>");
    if (check) check_for_updates(o);
    out_s(o,
        "<TD ALIGN=CENTER VALIGN=TOP>"
        "<IMG SRC=\"mill://splash\" WIDTH=192 HEIGHT=163 ALT=\"Don Quixote and the windmills\">"
        "<H1>Mill</H1></TABLE><HR>"
        "<H3>About Mill</H3>"
        "<P>Mill is a capable web browser for monochrome Macs. It is the most feature-complete, no-nonsense "
        "browser available for the Plus and later."
        "<P><B>System Requirements:</B><UL>"
        "<LI>Mac Plus or later"
        "<LI>System 6.0.3 with MacTCP or later"
        "<LI>700K free</UL>"
        "<H3>What Mill reads</H3><UL>"
        "<LI>HTML 3.2, which included text, lists, tables, images (and maps), and forms, as well as a few extras "
        "like bgcolor and font face. No applets, JavaScript, style sheets, or frames."
        "<LI>URL schemes: http://, https:// (through a proxy), file://, and mill:// (built-ins)"
        "<LI>Character sets: UTF-8, ISO 8859-1, and Mac Roman"
        "<LI>Image formats: GIF (static) and JPEG (baseline)"
        "<LI>Pages up to 192K for rendering, but larger for downloads"
        "<LI>Cookies: kept until you quit Mill</UL>"
        "<H3>Built-in pages</H3><UL>"
        "<LI><A HREF=\"mill://about\">mill://about</A>, this page"
        "<LI><A HREF=\"mill://config\">mill://config</A>, settings"
        "<LI><A HREF=\"mill://change-log\">mill://change-log</A>, what each version added"
        "<LI><A HREF=\"mill://license\">mill://license</A>, licenses</UL>"
        "<H3>Keys</H3><DL>"
        "<DT>Command-L<DD>Type an address"
        "<DT>Command-O<DD>Open a file on this Mac"
        "<DT>Command-[ and Command-]<DD>Back and forward"
        "<DT>Command-R<DD>Reload"
        "<DT>Command-H<DD>Go to the home page"
        "<DT>Command-period or Escape<DD>Stop loading (or click the stop sign)"
        "<DT>Command-1 to Command-0<DD>Open a bookmark"
        "<DT>Command-D<DD>Bookmark this page"
        "<DT>Option-Command and a number<DD>Bookmark this page in that slot"
        "<DT>Control-click, or hold the button down<DD>Save an image, a link, or the page's source</DL>"
        "<H3>Credits</H3><UL>"
        "<LI>The drawing is by George A. Harker, from James Baldwin's <I>Stories of Don Quixote</I> (1910), "
        "now in the public domain."
        "<LI>JPEG decoding by ChaN's <A HREF=\"mill://license#tjpgd\">TJpgDec</A>; GIF decoding is Mill's own."
        "<LI>Built with Wolfgang Thaller's Retro68, including its "
        "<A HREF=\"mill://license#runtime\">runtime</A>, the <A HREF=\"mill://license#newlib\">newlib</A> "
        "C library, and <A HREF=\"mill://license#multiversal\">Multiversal Interfaces</A>."
        "<LI>Source code under the <A HREF=\"mill://license#mill\">MIT License</A>.</UL>");
}

/* mill://license and mill://change-log, kept as 'TEXT' resources (tools/pages.py) so each
   is only in memory while it's sent */
static void text_page(Out *o, short id, const char *name)
{
    Handle h = GetResource('TEXT', id);
    if (!h) { not_found(o, name); return; }
    head(o, 200, HTML_MAC);
    HLock(h);
    out(o, *h, GetHandleSize(h));
    HUnlock(h);
    ReleaseResource(h);
}

/* One value from a form's urlencoded body, decoded to Mac Roman */
static int form_value(const char *body, long len, const char *name, char *out_buf, int size)
{
    long i = 0, nl = strlen(name);
    while (i < len) {
        long end = i;
        while (end < len && body[end] != '&') end++;
        if (end - i > nl && !memcmp(body + i, name, nl) && body[i + nl] == '=') {
            long k;
            int o = 0;
            for (k = i + nl + 1; k < end && o < size - 1; k++) {
                int c = body[k];
                if (c == '+') c = ' ';
                else if (c == '%' && k + 2 < end && hexval(body[k + 1]) >= 0 && hexval(body[k + 2]) >= 0) {
                    c = hexval(body[k + 1]) * 16 + hexval(body[k + 2]);
                    k += 2;
                }
                out_buf[o++] = (char)latin1_to_mac((unsigned char)c);
            }
            out_buf[o] = 0;
            return 1;
        }
        i = end + 1;
    }
    return 0;
}

static void config(Out *o, const char *body, long len)
{
    static char types[2048], typed[2048];
    char note[120] = "", home[256], where[300], k[16], via[140], skip[256], agent[141];
    const char *shown = NULL;   /* rules typed with a mistake in them, shown again to fix */
    FSSpec spec;
    if (body) {
        Url check;
        if (form_value(body, len, "homepage", home, sizeof home)) {
            if (home[0] && url_parse(home, &check)) snprintf(note, sizeof note, "That home page isn't an address Mill can open.");
            else put_setting("homepage", 8, home, strlen(home));
        }
        if (form_value(body, len, "image_memory", k, sizeof k)) {
            long n = atol(k);
            if (n < 0 || n > 4096 || (k[0] && (k[0] < '0' || k[0] > '9'))) snprintf(note, sizeof note, "Image memory must be from 0 to 4096 K.");
            else { sprintf(k, "%ld", n); put_setting("image_memory", 12, k, strlen(k)); }
        }
        if (form_value(body, len, "history", k, sizeof k)) {
            long n = atol(k);
            if (n < 1 || n > MILL_HISTORY_MAX || k[0] < '0' || k[0] > '9')
                snprintf(note, sizeof note, "Pages kept for Back must be from 1 to %d.", MILL_HISTORY_MAX);
            else { sprintf(k, "%ld", n); put_setting("history", 7, k, strlen(k)); }
        }
        /* a checkbox that isn't checked isn't sent */
        if (form_value(body, len, "load_images", k, sizeof k)) put_setting("load_images", 11, "yes", 3);
        else put_setting("load_images", 11, "no", 2);
        if (form_value(body, len, "decode_macbinary", k, sizeof k)) put_setting("decode_macbinary", 16, "yes", 3);
        else put_setting("decode_macbinary", 16, "no", 2);
        if (form_value(body, len, "cookies", k, sizeof k)) put_setting("cookies", 7, "yes", 3);
        else put_setting("cookies", 7, "no", 2);
        if (form_value(body, len, "user_agent", agent, sizeof agent)) {
            char *c;
            for (c = agent; *c; c++) if ((unsigned char)*c < ' ') *c = ' ';  /* one header line */
            put_setting("user_agent", 10, agent, strlen(agent));
        }
        if (form_value(body, len, "proxy", via, sizeof via)) {
            Url p;
            if (via[0] && (url_parse(via, &p) || p.scheme != SCHEME_HTTP || strcmp(p.path, "/")))
                snprintf(note, sizeof note, "The HTTP proxy should be a host and port, such as proxy.example:8080.");
            else put_setting("proxy", 5, via, strlen(via));
        }
        if (form_value(body, len, "proxy_on", k, sizeof k)) {
            if (!proxy()[0]) snprintf(note, sizeof note, "Enable proxy needs an HTTP proxy.");
            else put_setting("proxy_on", 8, "yes", 3);
        } else put_setting("proxy_on", 8, "no", 2);
        if (form_value(body, len, "proxy_exclude", skip, sizeof skip)) put_setting("proxy_exclude", 13, skip, strlen(skip));
        if (form_value(body, len, "proxy_https", k, sizeof k)) put_setting("proxy_https", 11, "yes", 3);
        else put_setting("proxy_https", 11, "no", 2);
        if (form_value(body, len, "print_headers", k, sizeof k)) put_setting("print_headers", 13, "yes", 3);
        else put_setting("print_headers", 13, "no", 2);
        if (form_value(body, len, "print_backgrounds", k, sizeof k)) put_setting("print_backgrounds", 17, "yes", 3);
        else put_setting("print_backgrounds", 17, "no", 2);
        if (form_value(body, len, "file_types", types, sizeof types)) {
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
                if (!form_value(body, len, fname, url, sizeof url)) continue;
                sprintf(fname, "bt%d", n + 1);
                if (!form_value(body, len, fname, title, sizeof title)) title[0] = 0;
                if (url[0] && url_parse(url, &u)) snprintf(note, sizeof note, "Bookmark %d isn't an address Mill can open.", n == 9 ? 0 : n + 1);
                else bookmark_put(n, url, title);
            }
        }
        if (!note[0]) {
            OSErr err = settings_save();
            if (err == noErr) strcpy(note, "Saved.");
            else snprintf(note, sizeof note, "Mill couldn't save its settings (error %d).", err);
        }
    }
    head(o, 200, HTML_MAC);
    out_s(o, "<TITLE>Mill Settings</TITLE><H2>Settings</H2>");
    if (note[0]) { out_s(o, "<P><B>"); out_text(o, note); out_s(o, "</B>"); }
    /* each section a table: the setting's name, then its control and what it does */
    out_s(o, "<P><A HREF=\"#general\">General</A> | <A HREF=\"#proxy\">Proxy</A> | <A HREF=\"#printing\">Printing</A> | <A HREF=\"#bookmarks\">Bookmarks</A>"
             "<FORM METHOD=POST ACTION=\"mill://config\"><H3><A NAME=general>General</A></H3>"
             SETTINGS_TABLE
             "<TR><TD VALIGN=TOP WIDTH=110><B>Home page</B><TD><INPUT NAME=homepage SIZE=36 VALUE=\"");
    out_text(o, setting("homepage", MILL_DEFAULT_HOME));
    out_s(o, "\"><BR>Opens at startup and with Home in the Go menu."
             "<TR><TD VALIGN=TOP WIDTH=110><B>Image memory</B><TD><INPUT NAME=image_memory SIZE=5 VALUE=\"");
    sprintf(k, "%ld", image_memory_k());
    out_s(o, k);
    out_s(o, "\"> K<BR>Memory for caching images, much faster when images are reused between pages; 0 to turn off."
             "<TR><TD VALIGN=TOP WIDTH=110><B>Pages kept for Back</B><TD><INPUT NAME=history SIZE=3 VALUE=\"");
    sprintf(k, "%d", history_size());
    out_s(o, k);
    out_s(o, "\"><BR>Each window remembers this many pages, 1 to 32: their addresses and what was typed in their "
             "forms, not the pages themselves."
             "<TR><TD VALIGN=TOP WIDTH=110><B>Load images automatically</B><TD><INPUT TYPE=checkbox NAME=load_images VALUE=yes");
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
    out_s(o, "> Automatically decode MacBinary if detected in a .bin download.</TABLE>"
             "<H3><A NAME=proxy>Proxy</A></H3>" SETTINGS_TABLE
             "<TR><TD VALIGN=TOP WIDTH=110><B>Enable proxy</B><TD><INPUT TYPE=checkbox NAME=proxy_on VALUE=yes");
    if (proxy_on()) out_s(o, " CHECKED");
    out_s(o, ">"
             "<TR><TD VALIGN=TOP WIDTH=110><B>HTTP proxy</B><TD><INPUT NAME=proxy SIZE=30 VALUE=\"");
    out_text(o, proxy());
    out_s(o, "\"><BR>A proxy server, as host:port."
             "<TR><TD VALIGN=TOP WIDTH=110><B>Proxy excluded domains</B><TD><INPUT NAME=proxy_exclude SIZE=36 VALUE=\"");
    out_text(o, proxy_excluded());
    out_s(o, "\"><BR>Domains (with their subdomains), IP addresses, or ranges such as 192.168.0.0/16, "
             "separated by spaces."
             "<TR><TD VALIGN=TOP WIDTH=110><B>Enable https over proxy</B><TD><INPUT TYPE=checkbox NAME=proxy_https VALUE=yes");
    if (proxy_https()) out_s(o, " CHECKED");
    out_s(o, "> Sends https:// addresses to the proxy. The connection from Mill to the proxy is not encrypted."
             "</TABLE><H3><A NAME=printing>Printing</A></H3>" SETTINGS_TABLE
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
    else if (!strcmp(u->host, "license")) text_page(o, 128, "mill://license");
    else if (!strcmp(u->host, "change-log")) text_page(o, 129, "mill://change-log");
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
    if (u->scheme == SCHEME_FILE) file_request(u, &o);
    else mill_request(u, content_type ? body : NULL, body_len, &o);
    return noErr;
}

int local_image(const char *url, Bitmap1 *bm)
{
    Handle h;
    short *head_words;
    long size;
    if (strcmp(url, "mill://splash")) return -1;
    if (!(h = GetResource('SPLS', 128))) return IMAGE_NO_MEMORY;  /* it's always there */
    HLock(h);
    head_words = (short *)*h;
    bm->w = head_words[0];
    bm->h = head_words[1];
    bm->row_bytes = head_words[2];
    bm->mask = NULL;
    size = (long)bm->row_bytes * bm->h;
    bm->bits = malloc(size);
    if (bm->bits) memcpy(bm->bits, *h + 6, size);
    HUnlock(h);
    ReleaseResource(h);
    return bm->bits ? 0 : IMAGE_NO_MEMORY;
}
