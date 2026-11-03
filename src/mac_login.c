/* The name and password dialog; see mac_login.h */
#include <stdio.h>
#include <string.h>

#include "mac_login.h"

enum { LG_OK = 1, LG_CANCEL, LG_MESSAGE, LG_USER_LABEL, LG_USER, LG_PASS_LABEL, LG_PASS, LG_RING };
#define BULLET 0xA5     /* a bullet, in Mac Roman */

/* The password as typed: the field itself only ever holds bullets */
static char *secret;
static int secret_size;
static short secret_len;

static void set_text(DialogPtr d, short item, const char *c)
{
    short type;
    Handle h;
    Rect r;
    Str255 p;
    long n = strlen(c);
    if (n > 255) n = 255;
    p[0] = (unsigned char)n;
    memcpy(p + 1, c, n);
    GetDialogItem(d, item, &type, &h, &r);
    SetDialogItemText(h, p);
}

/* The ring around OK that says Return presses it */
static pascal void draw_ring(DialogPtr d, short item)
{
    short type;
    Handle h;
    Rect r;
    GetDialogItem(d, item, &type, &h, &r);
    PenSize(3, 3);
    FrameRoundRect(&r, 16, 16);
    PenNormal();
}

/* Return and Enter are OK; Escape and Command-period are Cancel. In the password field,
   each key changes secret the way it changes the field, and a bullet goes in instead. */
static pascal Boolean filter(DialogPtr d, EventRecord *e, short *item)
{
    DialogPeek dp = (DialogPeek)d;
    unsigned char c;
    if (e->what != keyDown && e->what != autoKey) return false;
    c = e->message & charCodeMask;
    if (c == '\r' || c == 3) { *item = LG_OK; return true; }
    if (c == 27 || (c == '.' && (e->modifiers & cmdKey))) { *item = LG_CANCEL; return true; }
    if (dp->editField == LG_PASS - 1 && c != '\t' && !(c >= 28 && c <= 31)) {   /* arrows move */
        TEHandle te = dp->textH;
        short from = (**te).selStart, to = (**te).selEnd;
        Boolean del = c == 8;
        if ((e->modifiers & cmdKey) || (!del && (c < ' ' || secret_len - (to - from) >= secret_size - 1))) {
            e->what = nullEvent;    /* no cut, copy, or paste, and nothing past the end */
            return false;
        }
        if (del && from == to && from > 0) from--;
        memmove(secret + from + !del, secret + to, secret_len - to);
        if (!del) {
            secret[from] = (char)c;
            e->message = (e->message & ~charCodeMask) | BULLET;
        }
        secret_len = secret_len - (to - from) + !del;
    }
    return false;
}

Boolean login_ask(const char *place, Boolean refused, char *user, int user_size, char *pass, int pass_size)
{
    DialogPtr d;
    GrafPtr old;
    short item, type;
    Handle h;
    Rect r;
    Str255 s;
    char msg[300];
    GetPort(&old);
    pass[0] = 0;
    if (!(d = GetNewDialog(201, NULL, (WindowPtr)-1))) return false;
    /* centered a third of the way down, as alerts are */
    MoveWindow(d, (qd.screenBits.bounds.right - (d->portRect.right - d->portRect.left)) / 2,
               20 + (qd.screenBits.bounds.bottom - 20 - (d->portRect.bottom - d->portRect.top)) / 3, false);
    GetDialogItem(d, LG_RING, &type, &h, &r);
    SetDialogItem(d, LG_RING, type, (Handle)(UserItemUPP)draw_ring, &r);
    snprintf(msg, sizeof msg, refused ? "The name or password for %s was wrong. Try again?"
                                      : "Enter a name and password for %s.", place);
    set_text(d, LG_MESSAGE, msg);
    set_text(d, LG_USER, user);
    secret = pass;
    secret_size = pass_size;
    secret_len = 0;
    SelectDialogItemText(d, user[0] ? LG_PASS : LG_USER, 0, 32767);
    ShowWindow(d);
    InitCursor();
    do ModalDialog((ModalFilterUPP)filter, &item);
    while (item != LG_OK && item != LG_CANCEL);
    if (item == LG_OK) {
        GetDialogItem(d, LG_USER, &type, &h, &r);
        GetDialogItemText(h, s);
        snprintf(user, user_size, "%.*s", s[0], (char *)s + 1);
        pass[secret_len] = 0;
    } else {
        memset(pass, 0, pass_size);
    }
    secret = NULL;
    DisposeDialog(d);
    SetPort(old);
    return item == LG_OK;
}
