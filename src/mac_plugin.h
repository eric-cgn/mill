/* Plug-Ins: protocols that aren't Mill's own, built in (file:, gopher:, ftp:) or code
   resources in the "Mill Plug-Ins" folder. Each is loaded only while something uses it:
   a request to it, or a window showing a page it made. See sdk/Interfaces/MillPlugIn.h. */
#ifndef MILL_PLUGIN_H_MILL
#define MILL_PLUGIN_H_MILL

#include "core/auth.h"
#include "../sdk/Interfaces/MillPlugIn.h"
#include "core/image.h"
#include "core/url.h"
#include "mac_net.h"

#define BUILTINS 9     /* http, https, file, gopher, ftp, gif, jpeg, netpbm, gzip: Plug.builtin 1 to 9 */

enum { PLUG_READY, PLUG_NEWER, PLUG_CONFLICT, PLUG_SKIPPED, PLUG_FAILED };

typedef struct Plug Plug;

/* One protocol, as Mill keeps it (its own copy of what the Plug-In said) */
typedef struct {
    char *scheme, *heading, *description;
    short nset;
    MillSetting *sets;
    Plug *plug;
} PlugProto;

/* One format, likewise */
typedef struct {
    char *key, *heading, *description, *types, *extensions, *fileTypes;
    long gives;
    short nset;
    MillSetting *sets;
    Plug *plug;
} PlugFormat;

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
OSErr plugin_request(const Url *u, const char *url, const char *headers, const char *content_type,
                     struct Body *body, net_sink sink, void *ref,
                     const char *from_owner, char owner[32], char key[256]);

/* Keep the Plug-In for url's scheme loaded (a window shows a page it made); the
   Plug-In, to drop later, or NULL */
Plug *plugin_hold(const char *url);
void plugin_drop(Plug *p);
/* Where a protocol's setting is kept in Mill Settings (48 bytes): SCHEME.KEY, or just
   KEY for http: and https:, as Mill 1.x kept them; -1 if too long */
int plugin_setting_name(const char *scheme, const char *key, char *name);
/* Whether a protocol is on: Mill's own "Enable" setting, SCHEME.enable, on unless "no" */
Boolean plugin_enabled(const char *scheme);
/* ---- formats ---- */

/* The format for a reply: by its content type, or, when there's none or it's
   application/octet-stream, by name's extension (or a file's Finder type, 0 if none);
   only formats that are on and give one of wants. NULL if none. */
PlugFormat *format_for(const char *type, const char *name, OSType ftype, long wants);
/* Whether a format is on (Mill's own "Enable" setting: KEY.enable) */
Boolean format_enabled(const PlugFormat *f);
/* The content type for a file name, or a Finder type (TypeForName) */
long type_for_name(const char *name, char *out, long size);
long type_for_finder(OSType t, char *out, long size);

/* A decode: a format, and any formats its stream goes on to. Bytes at the end of the
   chain (a stream or elements nothing reads) go to out. aw, ah: WIDTH and HEIGHT, -1 if
   not given. encoding: "gzip" for Content-Encoding, else "" or NULL. */
typedef struct FmtRun FmtRun;
typedef Boolean (*fmt_out)(const char *data, long len, void *ref);
FmtRun *format_begin(PlugFormat *f, const char *type, const char *encoding, const char *url, short aw, short ah,
                     long wants, fmt_out out, void *out_ref);
/* Each piece: 0 to go on, 1 when it needs no more, < 0 when it can't go on */
int format_data(FmtRun *r, const char *p, long n);
/* The end (cut: millCut...): the picture, if the chain made one, into *pic (0, -1 for
   none, IMAGE_NO_MEMORY); the chain is freed */
int format_end(FmtRun *r, short cut, Bitmap1 *pic);
/* The type the chain gives at its end, if it's bytes (a stream's, or "text/html") */
const char *format_out_type(const FmtRun *r);

/* Memory is short: loaded Plug-Ins give back what they can; whether any did */
Boolean plugins_purge(long wanted);

#endif
