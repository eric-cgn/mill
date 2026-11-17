/* news.h: what the News Plug-In's parts share. Its names are here and only here: NEWS_NAME
   for plugin://NAME/ (its pages and icons), and NEWS_TITLE, the title on its pages. The
   file's own name ("News") is in build.sh and News.r. */
#ifndef NEWS_H
#define NEWS_H

#define NEWS_NAME "news"
#define NEWS_TITLE "News"

/* news.c: the protocols' descriptions and settings (news: and nntp:), and the state for a
   request (the caller frees it with FreeMem) */
const MillProtocol *news_protocols(void);
MillPlugIn *news_state(const MillHost *host);
OSErr news_request(MillPlugIn *self, MillRequest *r);

#endif
