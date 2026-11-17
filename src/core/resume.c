/* Going on with a download: see resume.h */
#include <string.h>

#include "resume.h"

void part_name(const char *name, char *out)
{
    size_t n = strlen(name);
    if (n > 26) n = 26;
    memcpy(out, "part.", 5);
    memcpy(out + 5, name, n);
    out[5 + n] = 0;
}

int resume_ask(int part_exists, int can_seek)
{
    if (!part_exists) return RESUME_FRESH;
    return can_seek ? RESUME_ASK : RESUME_REPLACE;
}

long resume_at(const unsigned char *head, const char type[4], const char creator[4], long dlen, long rlen,
               MacBinary *mb, int *decoded)
{
    *decoded = 0;
    if (dlen < 0 || rlen < 0) return -1;
    if (head && !macbinary_header(head, mb) && !memcmp(mb->type, type, 4) && !memcmp(mb->creator, creator, 4)) {
        *decoded = 1;
        if (dlen > mb->data_len || rlen > mb->rsrc_len) return -1;
        /* the data fork, its padding, then the resource fork */
        if (dlen < mb->data_len) return rlen ? -1 : mb->data_at + dlen;
        return mb->rsrc_at + rlen;
    }
    /* as it came: only a data fork */
    return rlen ? -1 : dlen;
}

int resume_appends(int code) { return code == 206; }
