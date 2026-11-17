/* Settings an older Mill saved, brought up to this one's (see migrate.h) */
#include <stdio.h>
#include <string.h>

#include "migrate.h"

/* The starting bookmarks of the 2.1 builds that saved no version (see migrate.h) */
static const char *const old_first[] = {
    "http://welcome.1in1e6.com/ Welcome to the Internet",
    "http://frogfind.com/ FrogFind",
    "http://info.cern.ch/ The First Web Site",
    "http://www.textfiles.com/ textfiles.com",
    "gopher://gopher.floodgap.com/ Floodgap Gopher",
    "ftp://ftp.jussieu.fr/pub/mac/info-mac/ Info-Mac Archive",
    "time://time.nist.gov/ Correct Time"
};
#define N_OLD (int)(sizeof old_first / sizeof *old_first)

static void slot_name(int i, char *name)
{
    sprintf(name, "bookmark%d", i + 1);
}

static const char *slot(const SettingsIO *io, int i)
{
    char name[16];
    slot_name(i, name);
    return io->get(name, "");
}

static void slot_put(const SettingsIO *io, int i, const char *v)
{
    char name[16];
    slot_name(i, name);
    io->put(name, v);
}

/* Whether a slot other than except holds b's address */
static int address_in(const SettingsIO *io, const char *b, int except)
{
    long ul = strcspn(b, " ");
    int i;
    for (i = 0; i < MIGRATE_BOOKMARKS; i++) {
        const char *v = slot(io, i);
        if (i != except && !strncmp(v, b, ul) && (v[ul] == ' ' || !v[ul])) return 1;
    }
    return 0;
}

static int is_old(const char *v)
{
    int k;
    for (k = 0; k < N_OLD; k++) if (!strcmp(v, old_first[k])) return 1;
    return 0;
}

static void bookmarks_upgrade(const SettingsIO *io, const char *const *first, int n_first)
{
    int i, j, old = 0, exact = 1;
    for (i = 0; i < MIGRATE_BOOKMARKS; i++) {
        const char *v = slot(io, i);
        if (strcmp(v, i < N_OLD ? old_first[i] : "")) exact = 0;
        if (is_old(v)) old = 1;
    }
    if (exact) {
        for (i = 0; i < MIGRATE_BOOKMARKS; i++) slot_put(io, i, i < n_first ? first[i] : "");
        return;
    }
    if (old) {
        /* an old one whose title is a starting one's at another address */
        for (i = 0; i < MIGRATE_BOOKMARKS; i++) {
            const char *v = slot(io, i), *t;
            if (!is_old(v)) continue;
            t = strchr(v, ' ');
            for (j = 0; j < n_first; j++) {
                const char *ft = strchr(first[j], ' ');
                if (ft && !strcmp(ft, t) && strcmp(first[j], v)) break;
            }
            if (j < n_first && !address_in(io, first[j], i)) slot_put(io, i, first[j]);
        }
        return;
    }
    for (i = 0; i < n_first && i < MIGRATE_BOOKMARKS; i++)
        if (!slot(io, i)[0] && !address_in(io, first[i], -1)) slot_put(io, i, first[i]);
}

void settings_upgrade(const SettingsIO *io, const char *saved_by, const char *agent, const char *serif, const char *mono,
                      const char *const *first, int n_first)
{
    static const char pre[] = "Mozilla/1.22 (compatible; Mill/", post[] = "; Macintosh; 68K)";
    const char *ua = io->get("user_agent", NULL);
    /* an older Mill's own User-Agent: one version with no spaces between pre and post */
    if (ua && !strncmp(ua, pre, sizeof pre - 1)) {
        const char *ver = ua + sizeof pre - 1, *e = strchr(ver, ';');
        if (e && e > ver && !strcmp(e, post) && !memchr(ver, ' ', e - ver)) io->put("user_agent", agent);
    }
    if (saved_by) return;
    io->put("welcome", "yes");
    /* 2.0 saved its defaults by name when Settings was saved; a font chosen since stays */
    if (!strcmp(io->get("font_prop", ""), "Times")) io->put("font_prop", serif);
    if (!strcmp(io->get("font_fixed", ""), "Courier")) io->put("font_fixed", mono);
    bookmarks_upgrade(io, first, n_first);
}
