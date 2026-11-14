/* file: as a built-in Plug-In: this Mac's disks, folders, and the files Mill can show.
   It uses the Plug-In interface like any other; for the File Manager it shares Mill's
   System 6 and 7 wrappers (mac_sys.h). */
#include <Multiverse.h>
#include <string.h>

#define MILL_BUILTIN
#define MILL_ENTRY file_main
#define MILL_NEW file_new
#define MILL_CLOSE file_close
#define MILL_REQUEST file_request
#include "../sdk/Glue/MillGlue.c"
#include "version.h"
#include "mac_sys.h"

#define ROOT_PARENT 1                   /* fsRtParID: what a volume's root folder is in */
#define IS_FOLDER 0x10
#define IS_INVISIBLE 0x4000
#define MAX_FILE (192L * 1024)
#define HTML_MAC "text/html; charset=macintosh"

typedef struct {
    MillPlugIn base;
    MillReqRef r;
    short stopped;
} FilePlugIn;

#define H (me->base.host)

static void out(FilePlugIn *me, const char *s, long n)
{
    if (!me->stopped && n > 0 && !H->Write(me->r, s, n)) me->stopped = 1;
}

static void out_s(FilePlugIn *me, const char *s) { out(me, s, strlen(s)); }

static void head(FilePlugIn *me, short code, const char *type)
{
    long a[3];
    a[0] = code;
    a[1] = (long)(code == 200 ? "OK" : "Not Found");
    a[2] = (long)type;
    if (!me->stopped && !H->Printf(me->r, "HTTP/1.0 %d %s\r\nContent-Type: %s\r\n\r\n", a)) me->stopped = 1;
}

/* Text inside HTML, escaped */
static void out_text(FilePlugIn *me, const char *s)
{
    char buf[700];
    if (H->HtmlEscape(s, buf, sizeof buf) >= 0) out_s(me, buf);
}

/* A Mac path as a file: URL: colons become slashes, and the rest is percent-encoded */
static void path_to_url(const char *path, char *url, short size)
{
    static const char hex[] = "0123456789ABCDEF";
    short o = 8;
    memcpy(url, "file:///", 8);
    for (; *path && o < size - 4; path++) {
        unsigned char c = (unsigned char)*path;
        if (c == ':') url[o++] = '/';
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.~", c)) url[o++] = c;
        else { url[o++] = '%'; url[o++] = hex[c >> 4]; url[o++] = hex[c & 15]; }
    }
    url[o] = 0;
}

static void pstr(const char *c, Str255 p)
{
    long n = strlen(c);
    if (n > 255) n = 255;
    p[0] = (unsigned char)n;
    memcpy(p + 1, c, n);
}

static void not_found(FilePlugIn *me, const char *what)
{
    head(me, 404, HTML_MAC);
    out_s(me, "<TITLE>Not found</TITLE><H2>Not found</H2><P>There's nothing at <TT>");
    out_text(me, what);
    out_s(me, "</TT>.");
}

/* A line of a listing: the icon, then the name as a link */
static void item(FilePlugIn *me, short icon, const char *url, const char *name)
{
    long a[1];
    a[0] = icon;
    H->Printf(me->r, "<IMG SRC=\"plugin://file/rsrc?type=ics%%23&id=%d\" WIDTH=16 HEIGHT=16 ALIGN=TOP> <A HREF=\"", a);
    out_text(me, url);
    out_s(me, "\">");
    out_text(me, name);
    out_s(me, "</A><BR>");
}

static void list_volumes(FilePlugIn *me)
{
    HParamBlockRec pb;
    Str255 name;
    short i;
    H->PlugInPage(me->r);   /* a page of its own, so it may show its icons */
    head(me, 200, HTML_MAC);
    out_s(me, "<TITLE>Disks</TITLE><H2>Disks</H2><P>");
    for (i = 1; !me->stopped; i++) {
        char cname[64], url[300];
        memset(&pb, 0, sizeof pb);
        pb.volumeParam.ioNamePtr = name;
        pb.volumeParam.ioVolIndex = i;
        if (PBHGetVInfoSync(&pb) != noErr) break;
        memcpy(cname, name + 1, name[0]);
        cname[name[0]] = ':';
        cname[name[0] + 1] = 0;
        path_to_url(cname, url, sizeof url);
        cname[name[0]] = 0;
        item(me, kMillIconDisk, url, cname);
    }
}

/* A folder's contents as a page; links are whole URLs, so the folder's own URL doesn't
   need its trailing slash */
static void list_folder(FilePlugIn *me, short vref, long dir, const char *path)
{
    CInfoPBRec pb;
    Str255 name;
    char child[600], url[700], parent[300];
    short i;
    short len = strlen(path);
    H->PlugInPage(me->r);
    head(me, 200, HTML_MAC);
    out_s(me, "<TITLE>");
    out_text(me, path);
    out_s(me, "</TITLE><H2>");
    out_text(me, path);
    out_s(me, "</H2><P>");
    /* up: the folder that holds this one, or the list of disks */
    strcpy(parent, path);
    if (len && parent[len - 1] == ':') parent[--len] = 0;
    while (len > 0 && parent[len - 1] != ':') parent[--len] = 0;
    path_to_url(parent, url, sizeof url);
    item(me, kMillIconParent, url, "Up");
    for (i = 1; !me->stopped; i++) {
        short folder, hidden;
        OSType type;
        memset(&pb, 0, sizeof pb);
        pb.hFileInfo.ioNamePtr = name;
        pb.hFileInfo.ioVRefNum = vref;
        pb.hFileInfo.ioDirID = dir;
        pb.hFileInfo.ioFDirIndex = i;
        if (PBGetCatInfoSync(&pb) != noErr) break;
        folder = (pb.hFileInfo.ioFlAttrib & IS_FOLDER) != 0;
        hidden = folder ? (pb.dirInfo.ioDrUsrWds.frFlags & IS_INVISIBLE) : (pb.hFileInfo.ioFlFndrInfo.fdFlags & IS_INVISIBLE);
        if (hidden) continue;
        type = folder ? 0 : pb.hFileInfo.ioFlFndrInfo.fdType;
        strcpy(child, path);
        if (child[strlen(child) - 1] != ':') strcat(child, ":");
        strncat(child, (char *)name + 1, name[0]);
        if (folder) strcat(child, ":");
        path_to_url(child, url, sizeof url);
        memcpy(child, name + 1, name[0]);
        child[name[0]] = 0;
        item(me, folder ? kMillIconFolder : type == 'APPL' ? kMillIconApplication : type == 'TEXT' ? kMillIconText :
                 (type == 'GIFf' || type == 'JPEG' || type == 'PICT') ? kMillIconPicture : kMillIconDocument,
             url, child);
    }
}

static void send_file(FilePlugIn *me, FSSpec *spec, OSType type, const char *path)
{
    const char *ctype;
    char chunk[1024], found[64];
    short ref;
    long n, total = 0;
    /* by its name, then by its Finder type; text here is Mac Roman */
    H->TypeForName(path, found, sizeof found);
    if (!strcmp(found, "application/octet-stream")) H->TypeForFinderType(type, found, sizeof found);
    if (!strcmp(found, "text/html")) ctype = HTML_MAC;
    else if (!strcmp(found, "text/plain")) ctype = "text/plain; charset=macintosh";
    else if (strcmp(found, "application/octet-stream")) ctype = found;
    else {
        head(me, 200, HTML_MAC);
        out_s(me, "<TITLE>Can't show this file</TITLE><P>Mill can't show <TT>");
        out_text(me, path);
        out_s(me, "</TT>. It shows text, HTML, and the formats in Settings.");
        return;
    }
    if (file_open(spec, fsRdPerm, &ref) != noErr) { not_found(me, path); return; }
    head(me, 200, ctype);
    do {
        n = sizeof chunk;
        FSRead(ref, &n, chunk);
        out(me, chunk, n);
        total += n;
    } while (n == sizeof chunk && total < MAX_FILE && !me->stopped);
    FSClose(ref);
}

static OSErr file_request(MillPlugIn *self, MillRequest *r)
{
    FilePlugIn *me = (FilePlugIn *)self;
    char path[300];
    Str255 p, name;
    FSSpec spec;
    CInfoPBRec pb;
    const char *q;
    long n;
    me->r = r->ref;
    me->stopped = 0;
    /* the path ("/Macintosh%20HD/Web/x.html") as a Mac path ("Macintosh HD:Web:x.html") */
    n = strcspn(r->path + 1, "?");
    if (n >= (long)sizeof path - 2) n = sizeof path - 3;
    memcpy(path, r->path + 1, n);
    path[n] = 0;
    for (q = path; *q; q++) if (*q == '/') *(char *)q = ':';
    H->UrlDecode(path, path, sizeof path);
    if (path[0] && !strchr(path, ':')) strcat(path, ":");  /* a volume by itself is "Name:" */
    if (!path[0]) { list_volumes(me); return noErr; }
    pstr(path, p);
    if (strlen(path) > 255 || file_spec(0, 0, p, &spec) != noErr) { not_found(me, path); return noErr; }
    /* a disk itself: its root folder, which System 6 won't look up by name in folder 1 */
    if (spec.parID == ROOT_PARENT) { list_folder(me, spec.vRefNum, 2, path); return noErr; }
    memcpy(name, spec.name, spec.name[0] + 1);
    memset(&pb, 0, sizeof pb);
    pb.hFileInfo.ioNamePtr = name;
    pb.hFileInfo.ioVRefNum = spec.vRefNum;
    pb.hFileInfo.ioDirID = spec.parID;
    if (PBGetCatInfoSync(&pb) != noErr) not_found(me, path);
    else if (pb.hFileInfo.ioFlAttrib & IS_FOLDER) list_folder(me, spec.vRefNum, pb.dirInfo.ioDrDirID, path);
    else send_file(me, &spec, pb.hFileInfo.ioFlFndrInfo.fdType, path);
    return noErr;
}

static void file_close(MillPlugIn *self) { self->host->FreeMem(self); }

static const MillProtocol protocols[] = { { "file", "File", "Files on this Mac (RFC 1738)", 0, NULL } };

MillPlugIn *file_new(const MillHost *host, short resFile, MillInfo *info)
{
    FilePlugIn *me = host->NewMem(sizeof *me);
    (void)resFile;
    if (!me) return NULL;
    me->base.methods = NULL;    /* a built-in's are called by name (MillGlue.c) */
    info->name = "file";
    info->version = "1.0";
    info->copyright = "\251 " MILL_YEAR " " MILL_PUBLISHER;
    info->by = MILL_PUBLISHER;
    info->description = "This Mac's disks and files.";
    info->protocolCount = 1;
    info->protocols = protocols;
    return &me->base;
}
