/* Host tests of mill://config's saving (src/core/config.c): a section's form saves only that
   section, so a checkbox in another section, which its form doesn't send, is left as it is */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/core/config.h"

#define MAX 64
static char names[MAX][32], values[MAX][300];
static int n;

static const char *get(const char *name)
{
    int i;
    for (i = 0; i < n; i++) if (!strcmp(names[i], name)) return values[i];
    return NULL;
}

static void put(const char *name, const char *value)
{
    int i;
    for (i = 0; i < n && strcmp(names[i], name); i++) ;
    if (i == n) { assert(n < MAX); snprintf(names[n++], sizeof names[0], "%s", name); }
    snprintf(values[i], sizeof values[0], "%s", value);
}

static void bookmark(int k, const char *url, const char *title)
{
    char name[8], v[300];
    sprintf(name, "bm%d", k);
    snprintf(v, sizeof v, "%s %s", url, title);
    put(name, url[0] ? v : "");
}

/* As mac_local.c's form_field, without the %-decoding, which these bodies don't need */
static int field(const char *body, long len, const char *name, char *out, int size)
{
    long i = 0, nl = strlen(name);
    while (i < len) {
        long end = i, k;
        int o = 0;
        while (end < len && body[end] != '&') end++;
        if (end - i > nl && !memcmp(body + i, name, nl) && body[i + nl] == '=') {
            for (k = i + nl + 1; k < end && o < size - 1; k++) out[o++] = body[k] == '+' ? ' ' : body[k];
            out[o] = 0;
            return 1;
        }
        i = end + 1;
    }
    return 0;
}

static const ConfigIO io = { field, put, bookmark, 32, 3600, 10, "mill.1in1e6.com" };

/* Every built-in setting, with its checkboxes half ticked and half not */
static void start(void)
{
    n = 0;
    put("homepage", "http://old/");
    put("image_memory", "64");
    put("history", "8");
    put("keep_alive", "300");
    put("font_prop", "Times");
    put("font_fixed", "Courier");
    put("user_agent", "Old");
    put("upload_as", "Automatic");
    put("file_types", "*/* * TEXT/ttxt");
    put("welcome", "yes");
    put("scale_text_only", "no");
    put("full_screen_bars", "yes");
    put("load_images", "yes");
    put("cookies", "no");
    put("decode_macbinary", "yes");
    put("upload_ext", "no");
    put("net_check", "no");
    put("check_host", "old.host");
    put("print_headers", "yes");
    put("print_backgrounds", "no");
    put("bm0", "http://a/ A");
    put("bm1", "");
}

static const char *const all[] = { "homepage", "image_memory", "history", "keep_alive", "font_prop", "font_fixed",
                                   "user_agent", "upload_as", "file_types", "welcome", "scale_text_only", "full_screen_bars", "load_images",
                                   "cookies", "decode_macbinary", "upload_ext", "net_check", "check_host",
                                   "print_headers", "print_backgrounds", "bm0", "bm1" };

static char before[MAX][300];

static void keep(void)
{
    unsigned i;
    for (i = 0; i < sizeof all / sizeof all[0]; i++) snprintf(before[i], sizeof before[0], "%s", get(all[i]));
}

/* Each setting not in changed (a space-separated list) is as it was */
static void others_unchanged(const char *changed)
{
    unsigned i;
    for (i = 0; i < sizeof all / sizeof all[0]; i++) {
        char word[40];
        snprintf(word, sizeof word, " %s ", all[i]);
        if (strstr(changed, word)) continue;
        if (strcmp(get(all[i]), before[i])) {
            fprintf(stderr, "%s changed: %s -> %s\n", all[i], before[i], get(all[i]));
            assert(0);
        }
    }
}

static int save(const char *body, char *note)
{
    static char typed[512], rules[512];
    char sec[32];
    if (!config_section(&io, body, strlen(body), sec, sizeof sec)) return -1;
    return config_save(&io, body, strlen(body), sec, note, 120, typed, rules, sizeof typed);
}

int main(void)
{
    char note[120];

    /* General, with all its boxes unticked: only General's change */
    start();
    keep();
    assert(save("section=general&homepage=http://new/&image_memory=128&history=4&keep_alive=60&font_prop=Geneva"
                "&font_fixed=Monaco&user_agent=New&upload_as=BinHex&file_types=", note) == 1);
    assert(!note[0]);
    assert(!strcmp(get("welcome"), "no") && !strcmp(get("load_images"), "no") && !strcmp(get("decode_macbinary"), "no"));
    assert(!strcmp(get("full_screen_bars"), "no"));
    assert(!strcmp(get("homepage"), "http://new/") && !strcmp(get("upload_as"), "BinHex") && !strcmp(get("file_types"), ""));
    others_unchanged(" homepage image_memory history keep_alive font_prop font_fixed user_agent upload_as file_types"
                     " welcome scale_text_only full_screen_bars load_images cookies decode_macbinary upload_ext"
                     " net_check check_host ");
    assert(!strcmp(get("print_headers"), "yes") && !strcmp(get("print_backgrounds"), "no"));
    assert(!strcmp(get("net_check"), "no"));

    /* General with its boxes ticked */
    start();
    keep();
    assert(save("section=general&scale_text_only=yes&full_screen_bars=yes&cookies=yes&upload_ext=yes", note) == 1 && !note[0]);
    assert(!strcmp(get("full_screen_bars"), "yes"));
    assert(!strcmp(get("scale_text_only"), "yes") && !strcmp(get("cookies"), "yes") && !strcmp(get("upload_ext"), "yes"));
    assert(!strcmp(get("welcome"), "no"));
    others_unchanged(" welcome scale_text_only full_screen_bars load_images cookies decode_macbinary upload_ext net_check ");

    /* Printing: its own two boxes and nothing else, even if the body has others' fields */
    start();
    keep();
    assert(save("section=printing&print_backgrounds=yes&cookies=yes&homepage=http://x/&bu1=http://b/", note) == 1);
    assert(!strcmp(get("print_headers"), "no") && !strcmp(get("print_backgrounds"), "yes"));
    others_unchanged(" print_headers print_backgrounds ");

    /* The connection check, in General */
    start();
    keep();
    assert(save("section=general&net_check=yes&check_host=new.host", note) == 1 && !note[0]);
    assert(!strcmp(get("net_check"), "yes") && !strcmp(get("check_host"), "new.host"));
    others_unchanged(" homepage image_memory history keep_alive font_prop font_fixed user_agent upload_as file_types welcome scale_text_only full_screen_bars load_images cookies decode_macbinary upload_ext net_check check_host ");
    start();
    keep();
    assert(save("section=general&check_host=", note) == 1);
    assert(!strcmp(get("net_check"), "no") && !strcmp(get("check_host"), "mill.1in1e6.com"));
    others_unchanged(" homepage image_memory history keep_alive font_prop font_fixed user_agent upload_as file_types welcome scale_text_only full_screen_bars load_images cookies decode_macbinary upload_ext net_check check_host ");

    /* Bookmarks */
    start();
    keep();
    assert(save("section=bookmarks&bt1=&bu1=&bt2=B&bu2=http://b/", note) == 1 && !note[0]);
    assert(!strcmp(get("bm0"), "") && !strcmp(get("bm1"), "http://b/ B"));
    others_unchanged(" bm0 bm1 ");

    /* A Plug-In's section isn't Mill's own: nothing changes */
    start();
    keep();
    assert(save("section=ftp&ftp.enable=yes&welcome=yes", note) == 0);
    others_unchanged(" ");
    /* nor does a form that says no section */
    assert(save("welcome=yes&print_headers=yes", note) == -1);
    others_unchanged(" ");

    /* What's wrong is said, and a rule that isn't one is shown again */
    start();
    keep();
    assert(save("section=general&history=99", note) == 1);
    assert(!strcmp(note, "Window history must be from 1 to 32."));
    assert(save("section=general&image_memory=", note) == 1 && !note[0] && !strcmp(get("image_memory"), "0"));
    assert(save("section=general&keep_alive=", note) == 1 && !strcmp(note, "Keep-alive must be from 0 to 3600 seconds."));
    {
        static char typed[512], rules[512];
        const char *b = "section=general&file_types=text/html+*+TEXT/MOSS%0Dnot+a+rule";
        /* %0D isn't decoded here: make the line break real */
        char body[100];
        strcpy(body, b);
        *strstr(body, "%0D") = '\r';
        memmove(strchr(body, '\r') + 1, strchr(body, '\r') + 3, strlen(strchr(body, '\r') + 3) + 1);
        assert(config_save(&io, body, strlen(body), "general", note, 120, typed, rules, sizeof typed) == 1);
        assert(!strcmp(note, "Line 2 of File types isn\325t a valid rule."));
        assert(!strcmp(typed, "text/html * TEXT/MOSS\rnot a rule"));
        assert(!strcmp(get("file_types"), "*/* * TEXT/ttxt"));
    }
    start();
    assert(save("section=bookmarks&bu3=nonsense:x", note) == 1 && !strcmp(note, "Bookmark 3 isn\325t an address Mill can open."));
    puts("config: ok");
    return 0;
}
