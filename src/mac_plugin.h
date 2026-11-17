/* Plug-Ins: protocols that aren't Mill's own, built in (http:, https:, file:) or code
   resources in the "Mill Plug-Ins" folder. Each is loaded only while something uses it:
   a request to it, or a window showing a page it made. See sdk/Interfaces/MillPlugIn.h. */
#ifndef MILL_PLUGIN_H_MILL
#define MILL_PLUGIN_H_MILL

#include "core/auth.h"
#include "../sdk/Interfaces/MillPlugIn.h"
#include "core/image.h"
#include "core/url.h"
#include "mac_net.h"

#define BUILTINS 5     /* http, https, file, gif, jpeg: Plug.builtin 1 to 5 */

/* PLUG_NEWER and PLUG_OLDER: its 'MIfc' gives another interface (none: older), so none of
   its code was loaded */
enum { PLUG_READY, PLUG_NEWER, PLUG_CONFLICT, PLUG_SKIPPED, PLUG_FAILED, PLUG_OLDER };

typedef struct Plug Plug;

/* One protocol, as Mill keeps it (its own copy of what the Plug-In said) */
typedef struct {
    char *scheme, *heading, *description;
    long block;         /* MillProtocol.blockSize: 0 if it can't seek */
    short nset;
    MillSetting *sets;
    Plug *plug;
} PlugProto;

/* One format, likewise. One whose key a later one has is replaced by it (MillPlugIn.h,
   Formats): never used, nor its settings shown. */
typedef struct PlugFormat PlugFormat;
struct PlugFormat {
    char *key, *heading, *description, *types, *extensions, *fileTypes;
    long gives;
    short nset;
    MillSetting *sets;
    Plug *plug;
    PlugFormat *replaced_by, *replaces;
};

struct Plug {
    char *name, *version, *copyright, *by, *description;
    char file[64];          /* "" for a built-in */
    long flags;
    short nproto;
    PlugProto *protos;
    short nformat;
    PlugFormat *formats;
    short status;           /* PLUG_... */
    short users;            /* requests and windows using it */
    Boolean loaded;
    short builtin;          /* which, from 1; 0 for a file */
    FSSpec spec;
    short res;              /* its file's resources while loaded */
    Boolean shared;         /* res is a font file's, open all along: never closed here */
    Handle code;
    void *self;
};

/* Find and read every Plug-In (Shift held skips those in the folder): mac_init.c */
void plugins_init(void);
/* For it: the host table ready, a Plug-In loaded and opened (false if it can't
   start), and closed and unloaded again */
void plugin_host_init(void);
Boolean plugin_load(Plug *p, MillOpenParams *op);
void plugin_close(Plug *p);
void plugins_quit(void);
extern Plug *plugs;
extern short nplugs;
/* The protocol for a scheme's name, or NULL */
PlugProto *plugin_proto(const char *scheme);

/* Ask the Plug-In for u's scheme, as a server would be asked: method GET or POST,
   headers the extra header lines (CR LF between, none at the end), and a body
   (mac_upload.h) with content_type for a POST. The response goes to sink. from_owner is the Plug-In that
   marked the asking page as its own ("" if none); owner gets this Plug-In's name if it
   marks the reply (PlugInPage), else "". u's user and password are the address's;
   key is where the Plug-In says they belong (MillRequest.loginKey), or "". paramErr if no Plug-In has the scheme or it couldn't start. */
/* plugin_request's answer when the Plug-In called Fail; plugin_failure says why */
#define PLUGIN_FAILED (-30001)
const char *plugin_failure(void);
/* ...and when it called AskLogin; plugin_asked says for what */
#define PLUGIN_ASKED (-30002)
typedef struct {
    char prompt[200], name_label[32], pass_label[32], key[256], name[64], pass[64];
    short forget;       /* a kept login SavedLogin gave for key didn't work */
} PluginAsked;
const PluginAsked *plugin_asked(void);
/* ...and when it called EndedEarly: what it wrote is cut off */
#define PLUGIN_CUT (-30003)
/* What was typed for keys (NULL: nothing), offered by SavedLogin for the requests that
   follow until it's cleared */
void plugin_login_typed(const char *keys, const char *user, const char *pass);
/* The key of the last login SavedLogin gave during plugin_request, "" if none */
const char *plugin_login_used(void);
/* A Plug-In stopped by Stop is tidying up: its network calls go on meanwhile */
Boolean plugin_tidying(void);
/* The names and passwords given while Mill runs, by the keys Plug-Ins name */
extern Keyring keyring;

struct Body;
/* window: the window asking, which may keep one connection between requests (see
   mac_net.h); page: whether it's for the window's page, which may replace the kept one,
   else (an image, a file saved) it only borrows it. A GET whose kept connection had gone
   before it answered is asked again, once, on a new one. */
OSErr plugin_request(const Url *u, const char *url, const char *headers, const char *content_type,
                     struct Body *body, net_sink sink, void *ref,
                     const char *from_owner, char owner[32], char key[256], const void *window, Boolean page);
/* The window shows url now: its kept connection is closed if it's to another server */
void plugin_page_shown(const void *window, const char *url);
/* Memory is short: kept connections are closed, but while window loads url, not its own
   to url's server, which the request would only open again. Whether any was closed. */
Boolean plugin_keep_room(const void *window, const char *url);

/* Keep the Plug-In for url's scheme loaded (a window shows a page it made); the
   Plug-In, to drop later, or NULL */
Plug *plugin_hold(const char *url);
void plugin_drop(Plug *p);
/* Where a protocol's setting is kept in Mill Settings (48 bytes): SCHEME.KEY, or just
   KEY for http: and https:, as Mill 1.x kept them; -1 if too long */
int plugin_setting_name(const char *scheme, const char *key, char *name);
/* Whether a protocol is on: Mill's own "Enable" setting, SCHEME.enable, on unless "no" */
Boolean plugin_enabled(const char *scheme);
/* Parts of files (MillPlugIn.h): the block size of u's protocol, 0 if it can't seek; and
   whether Mill may read parts there: it can seek, and its Support Random Access is on
   (file: always) */
long plugin_block_size(const Url *u);
Boolean plugin_random_access(const Url *u);
/* Whether the file in the reply to url (its head: len bytes, through the blank line) can
   be read in parts, the "seekable" a format is told: url is file:, or its protocol can
   seek and its Support Random Access is on; and the reply said Accept-Ranges: bytes and
   the file's size. If so, the size goes in *size and the protocol's block size (the
   parts to ask for) in *block. */
Boolean plugin_seekable(const char *url, const char *head, long len, long *size, long *block);
/* ---- formats ---- */

/* The format for a reply: by its content type, or, when there's none or it's
   application/octet-stream, by name's extension (or a file's Finder type, 0 if none);
   only formats that are on and give one of wants. NULL if none. */
PlugFormat *format_for(const char *type, const char *name, OSType ftype, long wants);
/* Whether a format is on (Mill's own "Enable" setting: KEY.enable) */
Boolean format_enabled(const PlugFormat *f);
/* The format a key names (the one in use, not one it replaced), or NULL */
PlugFormat *format_by_key(const char *key);
/* Formats with one key: each replaced by the last of them (plugins_init, once all are read) */
void formats_overlap(void);
/* The content type for a file name, or a Finder type (TypeForName) */
long type_for_name(const char *name, char *out, long size);
long type_for_finder(OSType t, char *out, long size);

/* A run: a format reading the reply, then any formats for what it gives (a stream's
   output, an archive's member), each told what Mill does with it through a sink. A
   run's format is asked to begin once it has its first bytes (FIRST_BYTES, or all
   there is); its answer goes to sink->ask. */
typedef struct FmtRun FmtRun;

/* What sink->ask answers */
enum { FMT_GO, FMT_STOP, FMT_SAVE, FMT_PULL };
typedef struct {
    short depth;                /* 0: the reply itself; 1 and on: a member's bytes, or a stream's */
    PlugFormat *f;
    short answer, pull;         /* millFormatBegin's (pull only when seekable) */
    const char *reason;         /* the format's reason, or "" */
    long size;                  /* -1 if not known */
    Boolean seekable;
    const char *path;           /* the member path it was given ("" lists) */
    const MillEntry *member;    /* depth > 0: the member it reads, or NULL for a stream's */
} FmtAsk;

typedef struct {
    void *ref;
    /* bytes at the end of the chain: a page, text, a stream nothing reads (NULL: none wanted) */
    Boolean (*out)(const char *p, long n, void *ref);
    /* a format began: FMT_GO, FMT_STOP, FMT_SAVE (its input goes to save), or FMT_PULL
       (depth 0: the input stops, and Mill calls format_pull) */
    short (*ask)(void *ref, const FmtAsk *a);
    /* a listing's member, for the folder path names ("" or ending in '/'): false to stop */
    Boolean (*entry)(void *ref, const MillEntry *e, const char *path);
    /* a member nothing shows: true if it's to be saved (then its bytes go to save; *forks
       set for both forks of a Mac file); NULL: never */
    Boolean (*save_member)(void *ref, const MillEntry *e, Boolean *forks);
    Boolean (*save)(const char *p, long n, void *ref);
} FmtSink;

/* Where a pull reads: n bytes at offset into p, the count (fewer at the end), or -1; in
   blocks of block bytes (the protocol's), the last kept */
typedef struct {
    long size, block;
    long (*read)(void *ref, unsigned long at, char *p, long n);
    void *ref;
} FmtSource;

/* A run of f (NULL: no format, the bytes are the page, after encoding) for a reply of
   type from url (its whole address); path is the member path (after "#/", decoded,
   without an anchor), pos a hint from the last listing. aw, ah: WIDTH and HEIGHT, -1 if
   not given. encoding: "gzip" for Content-Encoding, undone first. size: -1 if not known. */
FmtRun *format_begin(PlugFormat *f, const char *type, const char *encoding, const char *url, const char *path,
                     unsigned long pos, Boolean has_pos, short aw, short ah, long wants, long size, Boolean seekable,
                     const FmtSink *sink);
/* Each piece of the reply: 0 to go on, 1 when it needs no more, < 0 when it can't go on
   (-2: no such member) */
int format_data(FmtRun *r, const char *p, long n);
/* The reply is all in: its format decides now if its first bytes never filled */
void format_input_end(FmtRun *r);
/* After FMT_PULL: the format lists or gives its member, reading src */
OSErr format_pull(FmtRun *r, FmtSource *src);
/* The end (cut: millCut...): the picture, if the chain made one, into *pic (0, -1 for
   none, IMAGE_NO_MEMORY), and the type at its end (format_out_type) into type (64
   bytes), if not NULL; the chain is freed */
int format_end(FmtRun *r, short cut, Bitmap1 *pic, char *type);
/* The type the chain gives at its end, if it's bytes for the page ("text/html", a
   member's type), or a picture member's type when the page is to be that picture */
const char *format_out_type(const FmtRun *r);

/* Memory is short: loaded Plug-Ins give back what they can; whether any did */
Boolean plugins_purge(long wanted);

/* ---- fonts ---- */

/* The Plug-In in a font file that draws families of its own (MillPlugIn.h, Fonts), never in
   plugs: mac_init.c reads its families when it opens the font file. Its code is loaded
   when a run of text first needs it. */
typedef struct { char name[32]; short min, max; } PlugFont;
#define PLUG_FONTS 4
extern Plug font_plug;
extern PlugFont plug_fonts[PLUG_FONTS];
extern short nplug_fonts;
/* millFont, loading the Plug-In if it isn't: noErr, or why not */
OSErr plugin_font(MillFontParams *p);

#endif
