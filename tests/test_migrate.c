/* Host tests for bringing an older Mill's settings up to this one's (src/core/migrate.c):
   each case is a settings file's lines, what Web Fonts makes the defaults, and what
   settings_upgrade should leave. */
#include <stdio.h>
#include <string.h>

#include "../src/core/migrate.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define AGENT "Mozilla/1.22 (compatible; Mill/2.1; Macintosh; 68K)"

static char names[32][32], values[32][128];
static int n;

static const char *get(const char *name, const char *dflt)
{
    int i;
    for (i = 0; i < n; i++) if (!strcmp(names[i], name)) return values[i];
    return dflt;
}

static void put(const char *name, const char *value)
{
    int i;
    for (i = 0; i < n && strcmp(names[i], name); i++) {}
    if (i == n) strcpy(names[n++], name);
    strcpy(values[i], value);
}

static const SettingsIO io = { get, put };

/* lines: "name=value", ending with NULL */
static void load(const char *const *lines)
{
    n = 0;
    for (; *lines; lines++) {
        const char *eq = strchr(*lines, '=');
        sprintf(names[n], "%.*s", (int)(eq - *lines), *lines);
        strcpy(values[n++], eq + 1);
    }
}

/* 2.1's starting bookmarks (mac_local.c's first_bookmarks) */
static const char *const first[] = {
    "http://welcome.1in1e6.com/ Welcome to the Internet",
    "http://frogfind.com/ FrogFind",
    "http://info.cern.ch/ The First Web Site",
    "gopher://gopher.floodgap.com/ Floodgap Gopher",
    "ftp://ftp.funet.fi/pub/mac/info-mac/ Info-Mac Archive",
    "finger://graph.no/oslo Weather by Finger",
    "dict://dict.org/d:internet Dictionary",
    "time://time.nist.gov/ Correct Time"
};

static void upgrade(const char *const *lines, int web_fonts)
{
    load(lines);
    settings_upgrade(&io, get("version", NULL), AGENT, web_fonts ? "Mill Serif" : "Times", web_fonts ? "Mill Mono" : "Courier",
                     first, 8);
}

/* Whether bookmark1 to bookmark10 are want's ten ("" for empty; a missing one counts as empty) */
static int slots(const char *const *want)
{
    char name[16];
    int i;
    for (i = 0; i < 10; i++) {
        const char *v;
        sprintf(name, "bookmark%d", i + 1);
        v = get(name, "");
        if (strcmp(v, want[i])) { printf("  %s=%s, not %s\n", name, v, want[i]); return 0; }
    }
    return 1;
}

static int is(const char *name, const char *want)
{
    const char *v = get(name, NULL);
    if (!want) return v == NULL;
    return v && !strcmp(v, want);
}

int main(void)
{
    /* 2.0.1's settings, saved in Settings with its default fonts: Web Fonts' now */
    static const char *const v201[] = { "homepage=mill://about", "font_prop=Times", "font_fixed=Courier",
        "user_agent=Mozilla/1.22 (compatible; Mill/2.0.1; Macintosh; 68K)", NULL };
    static const char *const v201_mono[] = { "font_fixed=Courier", NULL };
    static const char *const v20_chosen[] = { "font_prop=Geneva", "font_fixed=Monaco", NULL };
    static const char *const v20_none[] = { "homepage=http://example.com/", NULL };
    static const char *const v20_off[] = { "welcome=no", NULL };
    static const char *const v21[] = { "version=2.1", "font_prop=Times", "font_fixed=Courier", "welcome=no", NULL };
    static const char *const v21_unset[] = { "version=2.1", NULL };
    static const char *const own_agent[] = { "user_agent=Mozilla/1.22 (compatible; Mill/2.0 beta; Macintosh; 68K)", NULL };
    static const char *const other_agent[] = { "user_agent=Lynx/2.7", NULL };
    static const char *const v19_agent[] = { "version=1.9", "user_agent=Mozilla/1.22 (compatible; Mill/1.9; Macintosh; 68K)", NULL };

    upgrade(v201, 1);
    CHECK(is("font_prop", "Mill Serif"));
    CHECK(is("font_fixed", "Mill Mono"));
    CHECK(is("welcome", "yes"));
    CHECK(is("user_agent", AGENT));
    CHECK(is("homepage", "mill://about"));

    /* the upgrade the user tested: only Courier saved */
    upgrade(v201_mono, 1);
    CHECK(is("font_fixed", "Mill Mono"));
    CHECK(is("font_prop", NULL));

    /* without Web Fonts the defaults are still Times and Courier */
    upgrade(v201, 0);
    CHECK(is("font_prop", "Times"));
    CHECK(is("font_fixed", "Courier"));
    CHECK(is("welcome", "yes"));

    /* fonts the user chose in 2.0 stay */
    upgrade(v20_chosen, 1);
    CHECK(is("font_prop", "Geneva"));
    CHECK(is("font_fixed", "Monaco"));

    /* no fonts saved: none are written (the defaults apply) */
    upgrade(v20_none, 1);
    CHECK(is("font_prop", NULL));
    CHECK(is("font_fixed", NULL));
    CHECK(is("welcome", "yes"));
    CHECK(is("user_agent", NULL));

    /* before 2.1 there was no welcome setting: on once, whatever the file says */
    upgrade(v20_off, 1);
    CHECK(is("welcome", "yes"));

    /* 2.1's own settings: fonts chosen in 2.1 and the welcome setting stay */
    upgrade(v21, 1);
    CHECK(is("font_prop", "Times"));
    CHECK(is("font_fixed", "Courier"));
    CHECK(is("welcome", "no"));
    upgrade(v21_unset, 1);
    CHECK(is("welcome", NULL));

    /* User-Agents: an older Mill's becomes this one's; one the user wrote stays */
    upgrade(own_agent, 1);
    CHECK(is("user_agent", "Mozilla/1.22 (compatible; Mill/2.0 beta; Macintosh; 68K)"));   /* a space: not Mill's */
    upgrade(other_agent, 1);
    CHECK(is("user_agent", "Lynx/2.7"));
    upgrade(v19_agent, 1);
    CHECK(is("user_agent", AGENT));
    CHECK(is("welcome", NULL));     /* a version was saved: not before 2.1 */

    /* bookmarks saved by the 2.1 builds still called 2.0.1, which started with seven: the
       final test's settings file, exactly */
    {
        static const char *const dev201[] = { "font_fixed=Courier",
            "user_agent=Mozilla/1.22 (compatible; Mill/2.0.1; Macintosh; 68K)",
            "bookmark1=http://welcome.1in1e6.com/ Welcome to the Internet",
            "bookmark2=http://frogfind.com/ FrogFind",
            "bookmark3=http://info.cern.ch/ The First Web Site",
            "bookmark4=http://www.textfiles.com/ textfiles.com",
            "bookmark5=gopher://gopher.floodgap.com/ Floodgap Gopher",
            "bookmark6=ftp://ftp.jussieu.fr/pub/mac/info-mac/ Info-Mac Archive",
            "bookmark7=time://time.nist.gov/ Correct Time",
            "bookmark8=", "bookmark9=", "bookmark10=", NULL };
        static const char *const want21[] = { first[0], first[1], first[2], first[3], first[4], first[5], first[6], first[7], "", "" };
        /* the user's own: one added in 8, Floodgap emptied, CERN retitled, textfiles kept */
        static const char *const changed[] = {
            "bookmark1=http://welcome.1in1e6.com/ Welcome to the Internet",
            "bookmark2=http://frogfind.com/ FrogFind",
            "bookmark3=http://info.cern.ch/ CERN",
            "bookmark4=http://www.textfiles.com/ textfiles.com",
            "bookmark5=",
            "bookmark6=ftp://ftp.jussieu.fr/pub/mac/info-mac/ Info-Mac Archive",
            "bookmark7=time://time.nist.gov/ Correct Time",
            "bookmark8=http://example.com/ Mine", "bookmark9=", "bookmark10=", NULL };
        static const char *const changed_want[] = {
            "http://welcome.1in1e6.com/ Welcome to the Internet", "http://frogfind.com/ FrogFind",
            "http://info.cern.ch/ CERN", "http://www.textfiles.com/ textfiles.com", "",
            "ftp://ftp.funet.fi/pub/mac/info-mac/ Info-Mac Archive", "time://time.nist.gov/ Correct Time",
            "http://example.com/ Mine", "", "" };
        /* jussieu's moved, but FUNET's is already in another slot: kept as it is */
        static const char *const both[] = {
            "bookmark1=ftp://ftp.jussieu.fr/pub/mac/info-mac/ Info-Mac Archive",
            "bookmark2=ftp://ftp.funet.fi/pub/mac/info-mac/ Info-Mac at FUNET", NULL };
        static const char *const both_want[] = {
            "ftp://ftp.jussieu.fr/pub/mac/info-mac/ Info-Mac Archive",
            "ftp://ftp.funet.fi/pub/mac/info-mac/ Info-Mac at FUNET", "", "", "", "", "", "", "", "" };
        /* released 2.0.1 and earlier: all ten empty, or none saved */
        static const char *const empty[] = { "bookmark1=", "bookmark2=", "bookmark3=", "bookmark4=", "bookmark5=",
            "bookmark6=", "bookmark7=", "bookmark8=", "bookmark9=", "bookmark10=", NULL };
        /* released 2.0.1 with the user's own in 1 and 9, one a starting address */
        static const char *const own20[] = { "bookmark1=http://example.com/ Mine",
            "bookmark9=gopher://gopher.floodgap.com/ Gopher", "bookmark10=", NULL };
        static const char *const own20_want[] = { "http://example.com/ Mine", first[1], first[2], "",
            first[4], first[5], first[6], first[7], "gopher://gopher.floodgap.com/ Gopher", "" };
        /* 2.1's own: nothing changes, even 2.0.1's seven */
        static const char *const v21_old[] = { "version=2.1",
            "bookmark4=http://www.textfiles.com/ textfiles.com",
            "bookmark6=ftp://ftp.jussieu.fr/pub/mac/info-mac/ Info-Mac Archive", "bookmark8=", NULL };
        static const char *const v21_old_want[] = { "", "", "", "http://www.textfiles.com/ textfiles.com", "",
            "ftp://ftp.jussieu.fr/pub/mac/info-mac/ Info-Mac Archive", "", "", "", "" };
        static const char *const none10[] = { "", "", "", "", "", "", "", "", "", "" };

        upgrade(dev201, 1);
        CHECK(slots(want21));
        CHECK(is("user_agent", AGENT));
        CHECK(is("font_fixed", "Mill Mono"));
        upgrade(changed, 1);
        CHECK(slots(changed_want));
        upgrade(both, 1);
        CHECK(slots(both_want));
        upgrade(empty, 1);
        CHECK(slots(want21));
        upgrade(v20_none, 1);
        CHECK(slots(want21));
        upgrade(own20, 1);
        CHECK(slots(own20_want));
        upgrade(v21_old, 1);
        CHECK(slots(v21_old_want));
        upgrade(v21_unset, 1);
        CHECK(slots(none10));
    }

    if (failures) { printf("%d failures\n", failures); return 1; }
    printf("migrate: all passed\n");
    return 0;
}
