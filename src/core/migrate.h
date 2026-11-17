/* Settings an older Mill saved, brought up to this one's (mac_init.c's settings_migrate) */
#ifndef MILL_MIGRATE_H
#define MILL_MIGRATE_H

/* Mill's settings, as mac_local.c keeps them: get gives dflt for one that isn't there */
typedef struct {
    const char *(*get)(const char *name, const char *dflt);
    void (*put)(const char *name, const char *value);
} SettingsIO;

/* Bookmark slots, bookmark1 to bookmark10, each "address title" or "" */
#define MIGRATE_BOOKMARKS 10

/* What changes in settings saved by version saved_by (NULL: before 2.1, which saved none):
   - an older Mill's own User-Agent becomes agent, this one's (one the user wrote is kept);
   - before 2.1: welcome=yes, so the welcome page opens once; and 2.0's default fonts,
     font_prop=Times and font_fixed=Courier, become serif and mono, the defaults now (Mill
     Serif and Mill Mono with Web Fonts; Times and Courier without, which changes nothing);
   - before 2.1, the bookmarks, given first, this Mill's n_first starting ones. Released 2.0.1
     and earlier had none, and saved all ten slots empty; builds of 2.1 in the making that
     saved no version (still called 2.0.1, then 2.1) started with seven, saved in slots 1-7:
     . exactly those seven, slots 8 to 10 empty: first replaces them;
     . otherwise, a slot holding exactly one of the seven whose address first has moved (the
       same title: Info-Mac Archive, now at FUNET) gets the new address, unless that is in
       another slot; the rest, textfiles.com too, stays. Empty slots stay empty: the user
       emptied them, or they were the user's;
     . with none of the seven: each empty slot of the first n_first gets its starting
       bookmark, unless that address is in another slot.
   The caller saves the version. */
void settings_upgrade(const SettingsIO *io, const char *saved_by, const char *agent, const char *serif, const char *mono,
                      const char *const *first, int n_first);

#endif
