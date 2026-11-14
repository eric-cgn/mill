#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/charset.h"
#include "core/filetypes.h"
#include "mac_local.h"
#include "mac_sys.h"
#include "mac_memreport.h"
#include "version.h"

#define ROOT_PARENT 1                   /* fsRtParID: what a volume's root folder is in */
#define IS_FOLDER 0x10

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

/* ------------------------------------------------------------ settings */

#define MAX_SETTINGS 64
static char *names[MAX_SETTINGS], *values[MAX_SETTINGS];
static int nsettings;

OSErr settings_spec(FSSpec *spec)
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

void setting_put(const char *name, const char *value) { put_setting(name, strlen(name), value, strlen(value)); }

void put_setting(const char *name, long nlen, const char *value, long vlen)
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

OSErr settings_write(void)
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

void bookmark_put(int n, const char *url, const char *title)
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
    return settings_write();
}

/* One value from a form's urlencoded body, decoded to Mac Roman */
int form_field(const char *body, long len, const char *name, char *out_buf, int size)
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

#ifdef MILL_MEMREPORT
void memreport_settings(void)
{
    int i;
    for (i = 0; i < nsettings; i++) {
        memreport_note(names[i], "Settings", 0);
        memreport_note(values[i], "Settings", 0);
    }
}
#endif
