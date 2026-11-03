/* Text arrives as UTF-8, ISO 8859-1 (read as Windows-1252, as browsers do), or Mac
   Roman, with HTML entities; the Mac draws Mac Roman. */
#ifndef MILL_CHARSET_H
#define MILL_CHARSET_H

/* What a page is in. GUESS: its META tag says, else valid UTF-8 is UTF-8, else Latin-1 */
enum { CHARSET_GUESS, CHARSET_MAC, CHARSET_LATIN1, CHARSET_UTF8 };

/* A charset's name ("utf-8", "ISO-8859-1", "macintosh"...), or CHARSET_GUESS if Mill
   doesn't know it */
int charset_named(const char *name, int len);
/* What a page with no charset from its server is in: its META tag's, else UTF-8 if it's
   valid UTF-8 with something beyond ASCII, else Latin-1 */
int charset_sniff(const char *src, long len);

unsigned char latin1_to_mac(unsigned char c);
/* The other way, for what Mill sends (form data); '?' for what Latin-1 lacks */
unsigned char mac_to_latin1(unsigned char c);
/* A Mac Roman character's Unicode code point */
long mac_to_unicode(unsigned char c);
/* A Unicode code point in Mac Roman: letters it lacks lose their accents, what draws
   nothing (zero-width spaces, byte order marks) is 0, and anything else is '?' */
unsigned char unicode_to_mac(long u);
/* One UTF-8 character at s (n bytes there): its length, or 0 if it isn't valid UTF-8 */
int utf8_char(const unsigned char *s, long n, long *u);
/* Decode the entity name or number between '&' and ';' (name not including either).
   Returns the Mac Roman byte, or 0 if unknown. */
unsigned char entity_to_mac(const char *name, int len);

#endif
