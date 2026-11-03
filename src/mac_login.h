/* Asking for a name and password, for whichever protocol needs one: HTTP's Basic and
   Digest now, FTP later. The protocol keeps what it's given; this only asks. The
   password shows as bullets and is never drawn. */
#ifndef MILL_LOGIN_H
#define MILL_LOGIN_H

#include <Multiverse.h>

/* place says what wants them ("Members" at www.example.com); refused says the last ones
   didn't work. user comes in as the name to suggest, and both go out as typed. False
   if the person cancels (pass is then empty). */
Boolean login_ask(const char *place, Boolean refused, char *user, int user_size, char *pass, int pass_size);

#endif
