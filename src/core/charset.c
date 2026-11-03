#include <string.h>

#include "charset.h"

/* ISO 8859-1 0xA0..0xFF to Mac Roman; characters Mac Roman lacks become a near ASCII
   look-alike. */
static const unsigned char LATIN1_HIGH[96] = {
    0xCA, 0xC1, 0xA2, 0xA3, 0xDB, 0xB4, '|',  0xA4, 0xAC, 0xA9, 0xBB, 0xC7, 0xC2, '-',  0xA8, 0xF8,
    0xA1, 0xB1, '2',  '3',  0xAB, 0xB5, 0xA6, 0xE1, 0xFC, '1',  0xBC, 0xC8, '?',  '?',  '?',  0xC0,
    0xCB, 0xE7, 0xE5, 0xCC, 0x80, 0x81, 0xAE, 0x82, 0xE9, 0x83, 0xE6, 0xE8, 0xED, 0xEA, 0xEB, 0xEC,
    'D',  0x84, 0xF1, 0xEE, 0xEF, 0xCD, 0x85, 'x',  0xAF, 0xF4, 0xF2, 0xF3, 0x86, 'Y',  'T',  0xA7,
    0x88, 0x87, 0x89, 0x8B, 0x8A, 0x8C, 0xBE, 0x8D, 0x8F, 0x8E, 0x90, 0x91, 0x93, 0x92, 0x94, 0x95,
    'd',  0x96, 0x98, 0x97, 0x99, 0x9B, 0x9A, 0xD6, 0xBF, 0x9D, 0x9C, 0x9E, 0x9F, 'y',  't',  0xD8,
};

/* HTML's Latin-1 entity names, in code order from 160 */
static const char *const NAMES[96] = {
    "nbsp", "iexcl", "cent", "pound", "curren", "yen", "brvbar", "sect", "uml", "copy", "ordf", "laquo",
    "not", "shy", "reg", "macr", "deg", "plusmn", "sup2", "sup3", "acute", "micro", "para", "middot",
    "cedil", "sup1", "ordm", "raquo", "frac14", "frac12", "frac34", "iquest", "Agrave", "Aacute", "Acirc",
    "Atilde", "Auml", "Aring", "AElig", "Ccedil", "Egrave", "Eacute", "Ecirc", "Euml", "Igrave", "Iacute",
    "Icirc", "Iuml", "ETH", "Ntilde", "Ograve", "Oacute", "Ocirc", "Otilde", "Ouml", "times", "Oslash",
    "Ugrave", "Uacute", "Ucirc", "Uuml", "Yacute", "THORN", "szlig", "agrave", "aacute", "acirc", "atilde",
    "auml", "aring", "aelig", "ccedil", "egrave", "eacute", "ecirc", "euml", "igrave", "iacute", "icirc",
    "iuml", "eth", "ntilde", "ograve", "oacute", "ocirc", "otilde", "ouml", "divide", "oslash", "ugrave",
    "uacute", "ucirc", "uuml", "yacute", "thorn", "yuml",
};

/* Windows-1252's 0x80..0x9F, which pages labeled ISO 8859-1 often use for quotes and dashes */
static const unsigned char CP1252_LOW[32] = {
    0xDB, '?',  0xE2, 0xC4, 0xE3, 0xC9, 0xA0, 0xE0, 0xF6, 0xE4, 'S',  0xDC, 0xCE, '?',  'Z',  '?',
    '?',  0xD4, 0xD5, 0xD2, 0xD3, 0xA5, 0xD0, 0xD1, 0xF7, 0xAA, 's',  0xDD, 0xCF, '?',  'z',  0xD9,
};

/* Mac Roman 0x80..0xFF in Unicode (0xDB is the euro sign, as from Mac OS 8.5) */
static const unsigned short MAC_HIGH[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

/* Latin Extended-A (U+0100..U+017F) without accents, for letters Mac Roman lacks */
static const char LATIN_EXT_A[129] =
    "AaAaAaCcCcCcCcDd"
    "DdEeEeEeEeEeGgGg"
    "GgGgHhHhIiIiIiIi"
    "IiIiJjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOo"
    "OoOoRrRrRrSsSsSs"
    "SsTtTtTtUuUuUuUu"
    "UuUuWwYyYZzZzZzs";

unsigned char latin1_to_mac(unsigned char c)
{
    if (c < 0x80) return c;
    return c < 0xA0 ? CP1252_LOW[c - 0x80] : LATIN1_HIGH[c - 0xA0];
}

unsigned char mac_to_latin1(unsigned char c)
{
    int i;
    if (c < 0x80) return c;
    for (i = 0; i < 96; i++) if (LATIN1_HIGH[i] == c) return (unsigned char)(0xA0 + i);
    switch (c) {
    case 0xD4: case 0xD5: return '\'';   /* curly quotes and dashes have plain stand-ins */
    case 0xD2: case 0xD3: return '"';
    case 0xD0: case 0xD1: return '-';
    }
    return '?';
}

long mac_to_unicode(unsigned char c)
{
    return c < 0x80 ? c : MAC_HIGH[c - 0x80];
}

unsigned char unicode_to_mac(long u)
{
    int i;
    if (u < 0x80) return (unsigned char)u;
    if (u >= 0xA0 && u <= 0xFF) return LATIN1_HIGH[u - 0xA0];
    for (i = 0; i < 128; i++) if (MAC_HIGH[i] == u) return (unsigned char)(0x80 + i);
    if (u >= 0x100 && u <= 0x17F) return (unsigned char)LATIN_EXT_A[u - 0x100];
    switch (u) {
    case 0x200B: case 0x200C: case 0x200D: case 0x200E: case 0x200F: case 0x2060: case 0xFEFF: case 0xAD:
        return 0;   /* zero-width, direction marks, byte order mark */
    case 0x2010: case 0x2011: case 0x2012: case 0x2212: return '-';
    case 0x2002: case 0x2003: case 0x2009: case 0x200A: case 0x202F: return ' ';
    case 0x2032: return '\'';
    case 0x2033: return '"';
    }
    return '?';
}

int utf8_char(const unsigned char *s, long n, long *u)
{
    int len, i;
    long v;
    if (s[0] < 0x80) { *u = s[0]; return 1; }
    if (s[0] >= 0xC2 && s[0] <= 0xDF) { len = 2; v = s[0] & 0x1F; }
    else if (s[0] >= 0xE0 && s[0] <= 0xEF) { len = 3; v = s[0] & 0x0F; }
    else if (s[0] >= 0xF0 && s[0] <= 0xF4) { len = 4; v = s[0] & 0x07; }
    else return 0;
    if (n < len) return 0;
    for (i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        v = (v << 6) | (s[i] & 0x3F);
    }
    /* no overlong forms, surrogates, or past U+10FFFF */
    if ((len == 3 && v < 0x800) || (len == 4 && (v < 0x10000 || v > 0x10FFFF)) || (v >= 0xD800 && v <= 0xDFFF)) return 0;
    *u = v;
    return len;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int name_is(const char *name, int len, const char *want)
{
    int i;
    if ((int)strlen(want) != len) return 0;
    for (i = 0; i < len; i++) if (lower((unsigned char)name[i]) != want[i]) return 0;
    return 1;
}

int charset_named(const char *name, int len)
{
    if (name_is(name, len, "utf-8") || name_is(name, len, "utf8")) return CHARSET_UTF8;
    if (name_is(name, len, "iso-8859-1") || name_is(name, len, "iso8859-1") || name_is(name, len, "latin1") ||
        name_is(name, len, "windows-1252") || name_is(name, len, "cp1252") || name_is(name, len, "us-ascii") ||
        name_is(name, len, "iso-8859-15"))
        return CHARSET_LATIN1;
    if (name_is(name, len, "macintosh") || name_is(name, len, "x-mac-roman") || name_is(name, len, "mac")) return CHARSET_MAC;
    return CHARSET_GUESS;
}

/* "charset=" in a META tag near the top of the page */
static int meta_charset(const char *src, long len)
{
    long i, end = len < 4096 ? len : 4096;
    for (i = 0; i + 5 < end; i++) {
        long j;
        if (src[i] != '<' || lower(src[i + 1]) != 'm' || lower(src[i + 2]) != 'e' || lower(src[i + 3]) != 't' || lower(src[i + 4]) != 'a')
            continue;
        for (j = i + 5; j + 7 < end && src[j] != '>'; j++) {
            if (lower(src[j]) == 'c' && name_is(src + j, 7, "charset")) {
                long k = j + 7, st;
                while (k < end && (src[k] == ' ' || src[k] == '=' || src[k] == '"' || src[k] == '\'')) k++;
                st = k;
                while (k < end && (((src[k] | 32) >= 'a' && (src[k] | 32) <= 'z') || (src[k] >= '0' && src[k] <= '9') || src[k] == '-' || src[k] == '_')) k++;
                return charset_named(src + st, (int)(k - st));
            }
        }
    }
    return CHARSET_GUESS;
}

int charset_sniff(const char *src, long len)
{
    const unsigned char *s = (const unsigned char *)src;
    long i = 0, u;
    int multi = 0, cs = meta_charset(src, len);
    if (cs != CHARSET_GUESS) return cs;
    while (i < len) {
        int k = utf8_char(s + i, len - i, &u);
        if (!k) return CHARSET_LATIN1;
        if (k > 1) multi = 1;
        i += k;
    }
    return multi ? CHARSET_UTF8 : CHARSET_LATIN1;
}

/* HTML 4's other common entities */
static const struct { const char *name; unsigned short u; } MORE[] = {
    { "ndash", 0x2013 }, { "mdash", 0x2014 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "sbquo", 0x201A },
    { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "bdquo", 0x201E }, { "dagger", 0x2020 }, { "Dagger", 0x2021 },
    { "bull", 0x2022 }, { "hellip", 0x2026 }, { "permil", 0x2030 }, { "lsaquo", 0x2039 }, { "rsaquo", 0x203A },
    { "euro", 0x20AC }, { "trade", 0x2122 }, { "OElig", 0x0152 }, { "oelig", 0x0153 }, { "Scaron", 0x0160 },
    { "scaron", 0x0161 }, { "Yuml", 0x0178 }, { "fnof", 0x0192 }, { "circ", 0x02C6 }, { "tilde", 0x02DC },
    { "ensp", 0x2002 }, { "emsp", 0x2003 }, { "thinsp", 0x2009 }, { "minus", 0x2212 }, { "pi", 0x03C0 },
    { "ne", 0x2260 }, { "le", 0x2264 }, { "ge", 0x2265 }, { "infin", 0x221E }, { "loz", 0x25CA },
};

unsigned char entity_to_mac(const char *name, int len)
{
    int i;
    if (len >= 2 && name[0] == '#') {
        long u = 0;
        if (name[1] == 'x' || name[1] == 'X') {
            for (i = 2; i < len; i++) {
                char c = name[i];
                int d = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
                if (d < 0) return 0;
                u = u * 16 + d;
            }
        } else {
            for (i = 1; i < len; i++) {
                if (name[i] < '0' || name[i] > '9') return 0;
                u = u * 10 + (name[i] - '0');
            }
        }
        return unicode_to_mac(u);
    }
    if (len == 3 && !memcmp(name, "amp", 3)) return '&';
    if (len == 2 && !memcmp(name, "lt", 2)) return '<';
    if (len == 2 && !memcmp(name, "gt", 2)) return '>';
    if (len == 4 && !memcmp(name, "quot", 4)) return '"';
    for (i = 0; i < 96; i++)
        if ((int)strlen(NAMES[i]) == len && !memcmp(NAMES[i], name, len)) return LATIN1_HIGH[i];
    for (i = 0; i < (int)(sizeof MORE / sizeof MORE[0]); i++)
        if ((int)strlen(MORE[i].name) == len && !memcmp(MORE[i].name, name, len)) return unicode_to_mac(MORE[i].u);
    return 0;
}
