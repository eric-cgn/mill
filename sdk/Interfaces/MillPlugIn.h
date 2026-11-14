/* MillPlugIn.h: how Mill and its Plug-Ins talk. Interface version 1.

   A Plug-In is a code resource of type 'MPlg' with its entry point at byte 0, in a
   file in the "Mill Plug-Ins" folder next to Mill. It provides protocols, formats,
   or both.

   A protocol is a URL scheme with its own heading and settings under Protocols on
   Mill's settings page. For each request it is given the whole HTTP request and
   writes back a whole HTTP response, like a small web server or proxy.

   A format reads a kind of file as it arrives, a piece at a time, and gives Mill
   a picture, HTML (elements), or a stream of other bytes (see Formats, below). It
   has its own heading under Formats.

   Rules for every compiler (MPW and CodeWarrior):
   - No int in this interface: short is 16 bits, long is 32.
   - Every call between Mill and a Plug-In uses the pascal convention.
   - Mill calls only the entry point, MillMain. A pointer to any other function
     inside a Plug-In must never reach Mill.
   - No static or global data that holds an address (a table of strings or
     functions): MPW's and CodeWarrior's code resources can't fix it up. Fill
     such tables in at run time (see Examples/Finger).
   - '\r' and '\n' are swapped in MPW C ('\n' is CR there), so
     write HTTP's CR LF as MILL_CRLF.
   - Pointers Mill passes stay valid for the call they come with, unless a
     comment below says otherwise.
   - Host functions marked "moves memory" may compact the heap, so don't hold
     dereferenced handles across them.

   License: a Plug-In file carries its license text in a resource named "License", ID 129:
   'TXGZ' (the text as a gzip file; Mill reads it with its own gzip code) or 'TEXT' (plain,
   CR line ends). Mill reads it only when it builds mill://license, which has a section for
   each Plug-In file (anchor: its name in lower case, spaces as hyphens), and
   mill://plugins links each Plug-In to its section. Finger's .r file shows a Rez 'read'.

   Lifetime: Mill opens each Plug-In at startup to read its MillInfo, then closes
   it and unloads its code. It opens the Plug-In again for its first request and
   keeps it loaded while any window shows a page it made or any request to it is
   running. When the last such window closes or goes elsewhere, Mill closes it
   and unloads its code again. Anything that has to outlast a close goes in
   settings (SetSetting).

   Settings: each protocol lists its settings in MillProtocol. Mill shows them
   under that protocol's heading, after its own "Enable" checkbox (SCHEME.enable,
   on at first), checks them, and saves them; the Plug-In reads them with
   GetSetting. A protocol that's turned off is never loaded or called for. A protocol that can go through Mill's HTTP proxy
   lists the standard setting (kMillProxySetting) and, when it's on, answers
   with ProxyRequest instead of its own TCP. */

#ifndef MILL_PLUGIN_H
#define MILL_PLUGIN_H

#include <Types.h>
#include <Quickdraw.h>

#define kMillInterface 1

/* CR LF, the same with every compiler */
#define MILL_CRLF "\015\012"

/* Selectors, the first argument to MillMain */
enum {
    millOpen    = 0,    /* params: MillOpenParams */
    millClose   = 1,    /* params: NULL */
    millRequest = 2,    /* params: MillRequest */
    millPurge   = 3,    /* params: MillPurgeParams */
    millFormatBegin = 4,    /* params: MillFormatParams */
    millFormatData  = 5,    /* params: MillFormatParams */
    millFormatEnd   = 6     /* params: MillFormatParams */
};
/* A selector a Plug-In doesn't know is answered with millUnknown, so later
   versions of Mill can add selectors without breaking older Plug-Ins. */
enum { millUnknown = -4 };     /* unimpErr */
enum { millNoProxy = -30000 };  /* ProxyRequest with no proxy set */

/* The entry point, at byte 0 of the code resource. self is the Plug-In's own
   pointer: set by millOpen, then passed back on every call. */
typedef pascal OSErr (*MillMainProc)(short selector, void *params, void **self);

/* Kinds of setting */
enum {
    millText     = 0,       /* a line of text */
    millCheckbox = 1,       /* "yes" or "no" */
    millNumber   = 2,       /* a whole number from min to max */
    millChoice   = 3        /* one of choices */
};

/* One setting, shown as a row under its protocol's heading. C strings. Saved
   in Mill Settings as SCHEME.KEY ("ftp.passive"). */
typedef struct MillSetting {
    const char *key;        /* letters, digits, and _ */
    const char *label;      /* "Passive mode" */
    const char *note;       /* the short line under it; may be NULL */
    short kind;
    const char *dflt;       /* the value until it's changed */
    long min, max;          /* millNumber */
    const char *choices;    /* millChoice: separated by '|', "Binary|Text" */
} MillSetting;

/* The standard setting for going through Mill's HTTP proxy: a checkbox, key
   "proxy", shown as "Use HTTP Proxy" */
#define kMillProxySetting { "proxy", "Use HTTP Proxy", "Send URLs with HTTP verbs to the configured HTTP proxy", \
                           millCheckbox, "no", 0, 0, NULL }

/* One protocol a Plug-In provides */
typedef struct MillProtocol {
    const char *scheme;     /* "ftp", lower case */
    const char *heading;    /* its heading under Protocols: "FTP" */
    const char *description;    /* under its Enable row: the protocol, and its version
                                   or RFC ("File Transfer Protocol (RFC 959)") */
    short settingCount;
    const MillSetting *settings;
} MillProtocol;

/* ---- Formats --------------------------------------------------------------
   Mill picks a format by the reply's Content-Type. Only when there's none, or
   it's application/octet-stream, does it go by the address's extension (or, for
   file:, the Finder type). An explicit other type is never overridden.

   A decode is three calls: millFormatBegin, millFormatData for each piece as it
   arrives, and millFormatEnd. A format never needs its whole input at once.
   Format Plug-Ins are loaded for a decode and unloaded when it ends.

   What a format gives (MillFormat.gives, one or more):
   - millGivesPicture: when it knows the picture's own size it calls PictureSize,
     and Mill answers with the size to draw at (both WIDTH and HEIGHT given: that;
     one: the other kept in proportion; neither: its own size; reduced to fit
     Mill's picture memory). It then gives pixels at its own size with
     PictureRect, in any order; Mill scales them, and dithers the whole when the
     data ends. Mill keeps the picture only at the size it's drawn.
   - millGivesElements: HTML, written with Write as it goes. Mill shows it, but
     the original bytes stay the document (View Source, Save, Back, Reload), the
     HTML's relative addresses go by the original's, and it's never a page the
     Plug-In made (PlugInPage). Where only a picture will do (IMG, BACKGROUND,
     image maps), Mill asks for millGivesPicture only.
   - millGivesStream: other bytes, written with Write, whose type is
     MillFormatParams.outType, set by millFormatBegin (gzip: "text/plain"...).
     Mill passes them to the format for that type, and so on. */

enum {
    millGivesPicture  = 1L << 0,
    millGivesElements = 1L << 1,
    millGivesStream   = 1L << 2
};

/* One format a Plug-In reads */
typedef struct MillFormat {
    const char *key;        /* for its settings, KEY.SETTING: "gif" */
    const char *heading;    /* its heading under Formats: "GIF" */
    const char *description;    /* "Graphics Interchange Format (GIF87a, GIF89a)" */
    const char *types;      /* content types, first is its own, by spaces: "image/gif" */
    const char *extensions; /* by spaces, lower case, no dots: "gif" */
    const char *fileTypes;  /* Finder types, 4 characters, by spaces: "GIFf" */
    long gives;             /* millGives..., one or more */
    short settingCount;
    const MillSetting *settings;
} MillFormat;

/* Identifies one decode in calls to the host */
typedef struct MillFormatRun *MillFormatRef;

/* Why a decode ended early (MillFormatParams.cut) */
enum { millCutNone = 0, millCutStop = 1, millCutLimit = 2, millCutMemory = 3, millCutBroken = 4 };

/* millFormatBegin, Data, and End: the same block each time */
typedef struct MillFormatParams {
    MillFormatRef ref;      /* for host calls (and Write: it's a MillReqRef too) */
    short format;           /* which of the Plug-In's formats, from 0 */
    const char *type;       /* the content type, with its parameters ("; charset=") */
    const char *url;        /* where it came from */
    short width, height;    /* WIDTH and HEIGHT asked for; 0 if not given */
    long wants;             /* what Mill can use: millGives..., one or more */
    const char *data;       /* millFormatData: this piece */
    long len;
    short cut;              /* millFormatEnd: millCut..., if it ended early */
    char outType[64];       /* millGivesStream: set by millFormatBegin */
    void *state;            /* the format's own, for this decode: kept between calls */
} MillFormatParams;
/* millFormatData returns noErr to go on, millDone when it needs no more, or an
   error; millFormatEnd follows either way. */
enum { millDone = 1 };

/* Flags in MillInfo */
enum {
    millHasAbout = 1L << 0  /* serves plugin://NAME/about */
};

/* What a Plug-In tells Mill about itself, filled in by millOpen. Mill copies
   all of it, strings and tables, before millOpen's call returns, so they may
   point into the Plug-In's code or data. */
typedef struct MillInfo {
    const char *name;       /* NAME in plugin://NAME/; must be unique, up to 31 characters */
    const char *version;    /* "1.0" */
    const char *copyright;
    const char *by;         /* who made it: "OneInAMillion Software" */
    const char *description;    /* what it provides, a line on mill://plugins */
    long flags;
    short protocolCount;
    const MillProtocol *protocols;
    short formatCount;
    const MillFormat *formats;
} MillInfo;

/* Identifies one request in calls to the host; Mill's own, not to be looked into */
typedef struct MillRequestRef *MillReqRef;
/* An open TCP connection */
typedef long MillConn;

/* The functions Mill provides. Valid from millOpen until millClose. */
typedef struct MillHost {
    long size;              /* sizeof(MillHost) in this Mill; fields past it are missing */
    long version;           /* kMillInterface of this Mill */
    QDGlobals *qd;          /* Mill's QuickDraw globals */

    /* Memory: nonrelocatable blocks. NewMem moves memory and returns NULL if
       there's no room. */
    pascal void *(*NewMem)(long size);
    pascal void (*FreeMem)(void *p);

    /* The status line, and how far along a request is (total 0 if unknown) */
    pascal void (*Status)(MillReqRef r, const char *text);
    pascal void (*Progress)(MillReqRef r, long done, long total);
    /* Call during long work that isn't network I/O: spins the windmill, redraws
       windows, and checks Command-period. Returns nonzero if the person asked to
       stop. Moves memory. */
    pascal short (*Busy)(MillReqRef r);

    /* TCP through MacTCP. All of these wait, calling Busy as they go, and return
       userCanceledErr (-128) if the person stops. After that, they work for a
       few seconds more, to tidy up (FTP deletes a stopped upload's part), and
       then return userCanceledErr again. Mill closes any connections a request
       leaves open when it ends. All move memory. */
    pascal OSErr (*TcpOpen)(MillReqRef r, const char *host, unsigned short port, MillConn *conn);
    pascal OSErr (*TcpSend)(MillConn conn, const void *data, long len);
    /* *len: in, the room in buf; out, the bytes received, 0 when the other side
       has closed */
    pascal OSErr (*TcpRecv)(MillConn conn, void *buf, long *len);
    pascal void (*TcpClose)(MillConn conn);
    /* Wait for a connection from elsewhere (FTP's PORT): this Mac's address and
       the port Mill picked, to tell the other side. TcpAccept waits until it
       connects, then conn works like one from TcpOpen. */
    pascal OSErr (*TcpListen)(MillReqRef r, unsigned long *addr, unsigned short *port, MillConn *conn);
    pascal OSErr (*TcpAccept)(MillConn conn);

    /* The response: status line, headers, blank line, then the body, in pieces
       of any size. Returns 0 when Mill wants no more (stopped, or over the page
       limit); finish up then. Moves memory. */
    pascal short (*Write)(MillReqRef r, const void *data, long len);

    /* Send this request to Mill's HTTP proxy (Settings, Protocols, HTTP) and
       write the proxy's answer as the response. Mill's http Plug-In does the
       sending. Returns millNoProxy if no proxy is set. Moves memory. */
    pascal OSErr (*ProxyRequest)(MillReqRef r);
    /* millNoProxy also when the proxy is off, or the host is one of its
       exclusions: go direct then. */

    /* A protocol's settings: those it lists, and any others it keeps for
       itself (not shown). GetSetting returns the value's length (the default
       if unchanged), or -1 if there's none. SetSetting moves memory. */
    pascal long (*GetSetting)(const char *scheme, const char *key, char *out, long size);
    pascal OSErr (*SetSetting)(const char *scheme, const char *key, const char *value);

    /* Mill's printf. Arguments go in an array, one long or pointer each, so
       every compiler agrees: %d %u %x take a long (an l is allowed), %s a
       string, %c a character; - and 0 flags, a width, and for %s a precision
       (%.*s: the length comes first in args).
       Format writes into out (always ended with 0) and returns the length it
       wanted; Printf writes to the response, like Write. */
    pascal long (*Format)(char *out, long size, const char *fmt, const long *args);
    pascal short (*Printf)(MillReqRef r, const char *fmt, const long *args);

    /* Helpers a Plug-In may use or ignore. Each returns the length written to
       out, or -1 if not found or it won't fit. */
    /* A header's value from the request, by name in any case */
    pascal long (*Header)(MillReqRef r, const char *name, char *out, long size);
    /* A field from a query string or form body (name=value&...), decoded to
       Mac Roman */
    pascal long (*FormField)(const char *form, long len, const char *name, char *out, long size);
    /* in, with < > & " made safe to put in HTML (for names that come from a server) */
    pascal long (*HtmlEscape)(const char *in, char *out, long size);
    /* Where the request came from (its Referer), as one of millFrom... */
    pascal short (*From)(MillReqRef r);
    /* %XX for what can't go in a URL path as it is (space, %, ", <, >, #, ?,
       and bytes outside ASCII); UrlDecode turns %XX back (in place is fine) */
    pascal long (*UrlEncode)(const char *in, char *out, long size);
    pascal long (*UrlDecode)(const char *in, char *out, long size);
    /* Base64 (RFC 2045), as in Basic authorization: Base64Decode skips
       anything that isn't a base64 character */
    pascal long (*Base64Encode)(const void *in, long len, char *out, long size);
    pascal long (*Base64Decode)(const char *in, long len, void *out, long size);
    /* MD5 of len bytes into 16 */
    pascal void (*MD5)(const void *in, long len, unsigned char digest[16]);
    /* Text in charset (millUTF8, millLatin1, or millGuess: UTF-8 if it is, else
       Latin-1) as Mac Roman, which is what Mill draws */
    pascal long (*ToMacRoman)(const char *in, long len, short charset, char *out, long size);

    /* This reply is a page the Plug-In made itself (a listing, a menu), so it
       may use the Plug-In's plugin://NAME/ addresses (its icons, say). Never
       call it for anything passed through from a server: a file, a document,
       or a ProxyRequest reply. The mark goes to Mill directly, not in a header,
       so no server can forge it. */
    pascal void (*PlugInPage)(MillReqRef r);

    /* Formats (millGivesPicture): the picture's own size, and whether it's a
       photo (its tones are stretched before dithering); the size Mill will draw
       it at comes back in *drawW, *drawH. memFullErr if there's no room. A format
       that can shrink as it decodes (JPEG) calls it again with the size it'll
       give pixels at; the drawn size stays. A format's run may also call Write,
       Status, Progress, and Busy, with its MillFormatRef as the MillReqRef. */
    pascal OSErr (*PictureSize)(MillFormatRef f, short w, short h, short photo, short *drawW, short *drawH);
    /* Pixels at the picture's own size: rows of w grays (0 black, 255 white) at
       x, y; opaque, if not NULL, is 0 where the page shows through (give it from
       the first call on, or never) */
    pascal void (*PictureRect)(MillFormatRef f, short x, short y, short w, short h,
                               const unsigned char *gray, const unsigned char *opaque);

    /* The content type for a file name, from Mill's own table and every format's
       extensions ("application/octet-stream" if none), and for a Finder type
       (likewise; for file:). Each returns its length. */
    pascal long (*TypeForName)(const char *name, char *out, long size);
    pascal long (*TypeForFinderType)(OSType type, char *out, long size);

    /* A POST's body, made as it's read: *len in, the room in buf; out, the bytes
       put there, 0 at the end. These are the raw bytes, as HTTP sends them;
       after NextPart, the contents of that part. buf NULL starts it over (to
       send it again). userCanceledErr if the person stops; Mill shows
       "Sending..." as it goes. */
    pascal OSErr (*ReadBody)(MillReqRef r, void *buf, long *len);
    /* A multipart/form-data body a part at a time: its field name, and for a
       file its name and type as sent (with .bin and application/macbinary, or
       .hqx and application/mac-binhex40, as Mill's settings encode it; a TEXT
       file is text/plain); filename and type are NULL for a field. eofErr after
       the last part, paramErr if the body isn't multipart. */
    pascal OSErr (*NextPart)(MillReqRef r, const char **name, const char **filename, const char **type);

    /* The request failed: Mill drops any response written so far and says text
       in an alert, leaving the page and its history as they were (as Stop
       does). Write nothing after it. Use the server's own words where it gave
       some: "The server refused it: Operation not permitted." A server's own
       error page (HTTP's 404 or 403) is written as a page instead. A network
       error returned before any reply (from TcpOpen and the rest) is an alert
       too, worded by Mill. A login refused is AskLogin, not Fail. A format's run
       may call it too: its decode just stops, with no alert. */
    pascal void (*Fail)(MillReqRef r, const char *text);

    /* Logins. A Plug-In names who is asking for one with a loginKey: a string
       Mill never reads, only compares, so a protocol makes it say what it
       needs (which server, which realm, which proxy) and keeps it the same each
       time. For example:
         a site's 401           http://www.example.com:80 "Members"
         a proxy's 407          proxy proxy.lab:8080 "Squid"
         an FTP server          ftp://ftp.example.com:21
       A login may be kept under more than one key, one per line (HTTP adds
       one for the folder where Basic worked, to send it there unasked).
       Logins are kept in memory until Mill quits, never shown or saved. */

    /* The request needs a name and password: a way it ends, like Fail. Write
       the server's own page first if there is one (status, headers, HTML),
       then call AskLogin, and write nothing after. Mill shows its dialog with
       prompt (the Plug-In's own words: "Log in to www.example.com (Members).")
       and the two labels (NULL: Mill's own), the fields filled in with
       defaultName and defaultPassword (NULL: empty). On OK Mill runs the
       request again, and SavedLogin gives what was typed for loginKey; if it
       ends without AskLogin, Mill keeps it under loginKey. On Cancel Mill shows
       the page written, if any, and otherwise stays where it was; a kept login
       that SavedLogin gave for loginKey is forgotten. Mill asks once per
       action: AskLogin on that second run counts as Cancel. */
    pascal void (*AskLogin)(MillReqRef r, const char *prompt, const char *nameLabel, const char *passwordLabel,
                            const char *loginKey, const char *defaultName, const char *defaultPassword);
    /* The login kept under loginKey (or just typed for it): nonzero if there's
       one. Whether to send it before being asked is the Plug-In's choice. */
    pascal short (*SavedLogin)(const char *loginKey, char *name, long nameSize, char *password, long passwordSize);
} MillHost;

/* Mill's shared listing icons: 'ics#' resources (a 16x16 picture and its mask)
   in Mill, so every Plug-In can show them as
   <IMG SRC="plugin://NAME/rsrc?type=ics%23&id=64" WIDTH=16 HEIGHT=16>.
   IDs 64 to 79 are Mill's; a Plug-In's own resources are 128 and up, and one of
   its own with the same type and ID is found first. */
enum {
    kMillIconFolder = 64,
    kMillIconDocument = 65,
    kMillIconText = 66,
    kMillIconPicture = 67,
    kMillIconBinary = 68,
    kMillIconApplication = 69,
    kMillIconParent = 70,
    kMillIconDisk = 71,
    kMillIconSearch = 72,
    kMillIconLink = 73
};

/* For ToMacRoman */
enum { millGuess = 0, millLatin1 = 2, millUTF8 = 3 };

/* Answers from From() */
enum {
    millFromNone    = 0,    /* typed, a bookmark, or a menu */
    millFromOwnPage = 1,    /* a page this Plug-In marked with PlugInPage */
    millFromOwn     = 2,    /* one of its own plugin://NAME/ pages */
    millFromLocal   = 3,    /* one of Mill's own mill: pages */
    millFromOther   = 4     /* anything else, such as a web page or a file */
};

/* millOpen. The first two fields never change in any version, so Mill and a
   Plug-In can always agree on which interface they share. */
typedef struct MillOpenParams {
    long interface;         /* in: Mill's kMillInterface */
    const MillHost *host;   /* in */
    short resFile;          /* in: the Plug-In file's resource refnum */
    MillInfo info;          /* out */
} MillOpenParams;

/* millRequest. The request is complete when MillMain returns. An address the
   protocol can't use (a port it doesn't take, a path it doesn't understand)
   gets an error page in the response, such as 400 Bad Request. */
typedef struct MillRequest {
    MillReqRef ref;         /* for host calls */
    short protocol;         /* which of the Plug-In's protocols, from 0, in MillInfo's
                               order; -1 for its own plugin://NAME/ addresses */
    /* The request as a server would receive it, up to the blank line: request
       line ("GET /path HTTP/1.0") and headers (Host, User-Agent, Referer when a
       page asked for it, cookies, Content-Type and Content-Length with a
       body); never Authorization, which is the Plug-In's to send. A POST's
       body is read with ReadBody, as it's made: it's never all in memory. */
    const char *text;
    long len;
    /* The address, already taken apart. user and password are the address's
       own (ftp://name:password@host/), "" if not given; kept and typed logins
       come from SavedLogin. */
    const char *url;
    const char *scheme;     /* "ftp", the same as protocols[protocol].scheme */
    const char *user;
    const char *password;
    const char *host;
    unsigned short port;    /* 0 if the address gives none: use the protocol's usual one */
    const char *path;       /* starts with '/', includes any query */
    /* Nonzero only for Mill's own http Plug-In, when another Plug-In called
       ProxyRequest: send this request (url is whole, scheme is the asking
       Plug-In's) to the HTTP proxy, or answer millNoProxy without writing. */
    short forward;
    /* If the Plug-In logged in with the address's user and password, the key
       they're for (see AskLogin): Mill keeps them under it if the request
       ends without AskLogin, so the server's other pages log in the same way */
    char loginKey[256];
} MillRequest;

/* millPurge: memory is short. Free what you can do without. */
typedef struct MillPurgeParams {
    long wanted;            /* in: bytes Mill is looking for */
    long freed;             /* out: bytes the Plug-In gave back */
} MillPurgeParams;

/* ---- For authors -------------------------------------------------------
   The SDK's glue file (MillGlue.c) supplies MillMain and does each
   compiler's setup for globals. You write a "subclass": a struct that begins
   with MillPlugIn, plus the functions below.

   The glue answers plugin://NAME/rsrc?type=TYPE&id=ID (or &name=NAME) itself,
   with the resource as the body and content type x-rsrc/TYPE (TYPE shorter
   than 4 characters is padded with spaces: x-rsrc/snd is 'snd '). It looks the
   resource up with GetResource, so the Plug-In's file is searched first, then
   Mill, then the System.

   Who may use plugin://NAME/: a typed address (or a bookmark or menu), a mill:
   page, or a page NAME marked as its own with PlugInPage. Mill turns anything
   else away with 403 Forbidden before NAME is called, and never follows a
   redirect into plugin://. The glue checks From() as well.
   Every other plugin://NAME/ address goes to your request method, with
   protocol -1; return millUnknown there for 404 Not Found. */

typedef struct MillPlugIn MillPlugIn;

typedef struct MillMethods {
    void (*close)(MillPlugIn *self);                        /* free self */
    OSErr (*request)(MillPlugIn *self, MillRequest *r);     /* protocols; else NULL */
    long (*purge)(MillPlugIn *self, long wanted);           /* may be NULL */
    /* formats (else NULL): millFormatBegin, millFormatData, or millFormatEnd */
    OSErr (*format)(MillPlugIn *self, short selector, MillFormatParams *f);
} MillMethods;

struct MillPlugIn {
    const MillMethods *methods;
    const MillHost *host;
    short resFile;
};

/* Yours to write: make self (with host->NewMem), set its methods, and fill in
   info. Return NULL if it can't start. */
MillPlugIn *PlugInNew(const MillHost *host, short resFile, MillInfo *info);

#endif
