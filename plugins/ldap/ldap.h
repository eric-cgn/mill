/* ldap.h: what the LDAP Plug-In's parts share. Its names are here and only here: LDAP_NAME
   for plugin://NAME/ (its pages and icons), and LDAP_TITLE, the title on its pages. The
   file's own name ("LDAP") is in build.sh and LDAP.r. */
#ifndef LDAP_H
#define LDAP_H

#define LDAP_NAME "ldap"
#define LDAP_TITLE "LDAP"

/* ldap.c: the protocol's description and settings, the state for a request (the caller
   frees it with FreeMem), and the search form its pages share */
const MillProtocol *ldap_protocol(void);
MillPlugIn *ldap_state(const MillHost *host);
OSErr ldap_request(MillPlugIn *self, MillRequest *r);
void ldap_form(const MillHost *h, MillReqRef r, const char *host, const char *base);

#endif
