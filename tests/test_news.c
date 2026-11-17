/* Host tests for the News Plug-In's helpers (plugins/news/newsutil.c): reply codes (RFC 977
   2.4.2), LIST lines (3.6.1), XOVER lines (RFC 2980 2.8), wildmats (RFC 977 3.10), dot
   stuffing (2.4.1), and References (RFC 1036 2.2.5). */
#include <stdio.h>
#include <string.h>

#include "../plugins/news/newsutil.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_code(void)
{
    CHECK(news_code("200 news.example ready") == 200);
    CHECK(news_code("480") == 480);
    CHECK(news_code("2000 x") == 0 && news_code("20x") == 0 && news_code("600 x") == 0 && news_code("") == 0);
}

static void test_group(void)
{
    char l[64];
    NewsGroup g;
    strcpy(l, "lab.test 0000000012 0000000001 y");
    CHECK(news_group(l, &g) && !strcmp(g.name, "lab.test") && g.last == 12 && g.first == 1 && g.flag == 'y');
    strcpy(l, "comp.sys.mac.misc 99 100 m");
    CHECK(news_group(l, &g) && g.last == 99 && g.first == 100 && g.flag == 'm');
    strcpy(l, "nospace");
    CHECK(!news_group(l, &g));
}

static void test_over(void)
{
    char l[300];
    NewsOver o;
    strcpy(l, "3\tRe: First post\tEric <e@x>\tMon, 10 Nov 1997 10:02:00 -0800\t<3@x>\t<1@x> <2@x>\t525\t7\tXref: h lab.test:3");
    CHECK(news_over(l, &o) && o.number == 3 && !strcmp(o.subject, "Re: First post") && !strcmp(o.from, "Eric <e@x>"));
    CHECK(!strcmp(o.date, "Mon, 10 Nov 1997 10:02:00 -0800") && !strcmp(o.id, "<3@x>") && !strcmp(o.refs, "<1@x> <2@x>") && o.lines == 7);
    strcpy(l, "5\tS\tF\tD\t<5@x>");
    CHECK(news_over(l, &o) && o.number == 5 && o.lines == 0 && !o.refs[0]);
    strcpy(l, "x\tS\tF\tD\t<5@x>");
    CHECK(!news_over(l, &o));
    strcpy(l, "5\tS\tF");
    CHECK(!news_over(l, &o));
}

static void test_match(void)
{
    CHECK(news_match("comp.sys.mac.misc", "MAC"));
    CHECK(!news_match("comp.sys.amiga", "mac"));
    CHECK(news_match("comp.sys.mac.misc", "comp.*.mac.*"));
    CHECK(!news_match("comp.sys.mac.misc", "comp.*.mac"));
    CHECK(news_match("lab.test", "lab.tes?") && !news_match("lab.test", "lab.te?"));
    CHECK(news_match("anything", "") && news_match("anything", "*"));
}

static void test_unstuff_ref(void)
{
    char l[16];
    const char *p = "<1@x> <2@x>\t <bad <3@y>";
    char id[32];
    strcpy(l, ".");
    CHECK(news_unstuff(l, 1) == -1);
    strcpy(l, "..two");
    CHECK(news_unstuff(l, 5) == 4 && !memcmp(l, ".two", 4));
    strcpy(l, ".x");
    CHECK(news_unstuff(l, 2) == 2);
    CHECK(news_ref(&p, id, sizeof id) == 3 && !strcmp(id, "1@x"));
    CHECK(news_ref(&p, id, sizeof id) == 3 && !strcmp(id, "2@x"));
    CHECK(news_ref(&p, id, sizeof id) == 3 && !strcmp(id, "3@y"));
    CHECK(news_ref(&p, id, sizeof id) == 0);
}

int main(void)
{
    test_code();
    test_group();
    test_over();
    test_match();
    test_unstuff_ref();
    if (failures) { printf("%d news check(s) failed\n", failures); return 1; }
    printf("news: all passed\n");
    return 0;
}
