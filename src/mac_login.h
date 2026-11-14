/* Asking for a name and password, for a Plug-In's AskLogin. Mill keeps what it's
   given; this only asks. The password shows as bullets and is never drawn. */
#ifndef MILL_LOGIN_H
#define MILL_LOGIN_H

#include <Multiverse.h>

/* prompt is the Plug-In's, as are the labels ("" for "Name:" and "Password:"). user and
   pass come in as the name and password to suggest, and go out as typed. False if the
   person cancels (pass is then empty). */
Boolean login_ask(const char *prompt, const char *name_label, const char *pass_label,
                  char *user, int user_size, char *pass, int pass_size);

#endif
