/* What Mill answers itself: its own pages (mill:), the Mac's files (file:), and its
   settings, kept in System Folder:Preferences:Mill Settings. */
#ifndef MILL_LOCAL_H
#define MILL_LOCAL_H

#include <Multiverse.h>

#include "core/image.h"
#include "core/url.h"
#include "mac_net.h"

/* Answer a mill: URL the way a web server would: a whole HTTP response, through
   sink. A POST when content_type is given. (file: is a Plug-In.) */
OSErr local_request(const Url *u, const char *content_type, const char *body, long body_len, net_sink sink, void *ref);

/* An image Mill has built in (mill://splash); returns 0, -1 if there is none by that
   name, or IMAGE_NO_MEMORY */
int local_image(const char *url, Bitmap1 *out);

/* Fetches url as any page is fetched (proxy, User-Agent, redirects, Stop) into buf: the
   body's length, at most size, or -1 with why it couldn't in Mill's usual words. mac_main
   sets it; mill://about?check=1 uses it to check for updates. */
extern long (*local_fetch)(const char *url, char *buf, long size, char *why, int why_size);

#define MILL_DEFAULT_HOME "mill://about"
#define MILL_DEFAULT_AGENT "Mozilla/1.22 (compatible; Mill/" MILL_VERSION "; Macintosh; 68K)"
#define MILL_DEFAULT_IMAGE_K 128
#define MILL_DEFAULT_HISTORY 10
#define MILL_HISTORY_MAX 32     /* pages kept for Back, at most, in each window */

/* A Mac file's full path ("Macintosh HD:Web:index.html") */
void spec_to_path(const FSSpec *spec, char *path, int size);
/* The file: URL for a Mac file */
void spec_to_url(const FSSpec *spec, char *out, int size);
/* A Mac file's data fork, up to max bytes, malloc'd; its length, or -1 */
long read_mac_file(const char *path, char **data, long max);

void settings_load(void);
const char *setting(const char *name, const char *dflt);
/* Change a setting (not yet saved); save them all to the settings file */
void setting_put(const char *name, const char *value);
void put_setting(const char *name, long nlen, const char *value, long vlen);
/* Where the settings file is (it may not exist yet) */
OSErr settings_spec(FSSpec *spec);
OSErr settings_write(void);
/* The font settings (mac_main.c): the proportional and the fixed font, each with a size.
   font_list fills out with the installed fonts' names (the fixed-width ones only, if
   fixed), sorted, and returns how many; fonts_choose reads the settings again. */
#define FONT_NAME_MAX 32
int font_list(int fixed, char (*names)[FONT_NAME_MAX], int max);
void fonts_choose(void);
const char *font_default(int fixed);    /* the name used when the setting is empty or gone */
/* One value from a form's urlencoded body, decoded to Mac Roman: 1, or 0 if it isn't there */
int form_field(const char *body, long len, const char *name, char *out, int size);
/* How much memory decoded images may keep, in K */
long image_memory_k(void);
/* How many pages each window keeps for Back and Forward, 1 to MILL_HISTORY_MAX */
int history_size(void);
/* Whether pages fetch their images as they open (else a click on one does) */
int auto_images(void);
/* The rules that give a saved file its type and creator (see core/filetypes.h) */
const char *file_type_rules(void);
/* Whether a MacBinary file is saved as the Mac file it carries */
int decode_macbinary(void);
int print_headers(void);
int print_backgrounds(void);
/* Whether sites may keep cookies (until Mill quits) */
int accept_cookies(void);
/* The User-Agent sent with each request; "" sends none */
const char *user_agent(void);

/* Bookmarks: ten slots, n from 0 (Command-1) to 9 (Command-0), kept in the settings */
#define BOOKMARKS 10
/* Slot n's address and title (a title may be ""), or 0 if it's empty */
int bookmark(int n, char *url, int url_size, char *title, int title_size);
/* Put a page in slot n (url NULL or "" empties it), and save the settings; noErr or why not */
OSErr bookmark_set(int n, const char *url, const char *title);
/* The same, without saving */
void bookmark_put(int n, const char *url, const char *title);

#endif
