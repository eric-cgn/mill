/* A form's body, made as it's sent: fields from memory, files read from disk a piece at a
   time, MacBinary or BinHex as the settings say. Its size is known before it's sent. */
#ifndef MILL_UPLOAD_H
#define MILL_UPLOAD_H

#include <Multiverse.h>

#include "core/form.h"

typedef struct Body Body;

/* The body for a POSTed form (s stays the caller's until body_free); NULL with why set
   if a file can't be read, or there's no memory */
Body *body_new(const Submission *s, char *why, int why_size);
void body_free(Body *b);
long body_size(const Body *b);
const char *body_type(const Body *b);   /* its Content-Type */
/* From the start again (a request asked for again, after a 401) */
void body_rewind(Body *b);
/* The next bytes as they're sent: *len in, the room in buf; out, what was put there, 0
   at the end. userCanceledErr if the person stops. Says how far it's got in the status
   line. */
OSErr body_read(Body *b, char *buf, long *len);
/* A multipart body a part at a time: the next part's name, file name (NULL for a field),
   and type (NULL for a field); body_read then reads its contents. eofErr after the last,
   paramErr if the body isn't multipart. */
OSErr body_next_part(Body *b, const char **name, const char **filename, const char **type);
/* The whole body, malloc'd (for Mill's own pages, whose forms are small) */
char *body_all(Body *b, long *len);

#endif
