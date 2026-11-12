/* What differs between System 6 and System 7, behind one set of calls. sys_init looks
   once for each System 7 feature Mill uses; where one is missing, these fall back to
   what System 6.0.8 and the Plus's ROM have. Files always go through the HFS "H" calls,
   which both have, so only dialogs, folders, resource files, and waiting for events
   take two paths. An FSSpec here is plain data (volume, folder ID, name). */
#ifndef MILL_SYS_H
#define MILL_SYS_H

#include <Multiverse.h>

void sys_init(void);
extern Boolean sys_fsspec;      /* the File Manager's FSSpec calls */
extern Boolean sys_new_sf;      /* StandardGetFile and StandardPutFile */
extern Boolean sys_find_folder; /* FindFolder, and so a Preferences folder */
extern Boolean sys_wne;         /* WaitNextEvent (System 7, or MultiFinder on System 6) */

/* Like FSMakeFSSpec: name is a file's name in folder dir, or a full path ("Disk:a:b")
   with vref and dir 0. fnfErr means only the file itself doesn't exist (spec is filled). */
OSErr file_spec(short vref, long dir, ConstStr255Param name, FSSpec *spec);
OSErr file_open(const FSSpec *spec, SInt8 perm, short *ref);       /* the data fork */
OSErr file_open_rf(const FSSpec *spec, SInt8 perm, short *ref);    /* the resource fork, as bytes */
OSErr file_create(const FSSpec *spec, OSType creator, OSType type);
OSErr file_delete(const FSSpec *spec);
OSErr file_get_info(const FSSpec *spec, FInfo *fi);
OSErr file_set_info(const FSSpec *spec, const FInfo *fi);
/* Open a file's resources for the Resource Manager; its refNum, or -1 */
short file_open_res(const FSSpec *spec, SInt8 perm);

/* A system folder (FindFolder's kinds; Preferences is made if it's missing). System 6
   has only the System Folder, which stands in for the others. */
OSErr sys_folder(OSType which, short *vref, long *dir);

/* Ask for a file to open, or where to save; false if the person cancels */
Boolean sys_choose_file(FSSpec *spec);
Boolean sys_choose_save(ConstStr255Param prompt, ConstStr255Param name, FSSpec *spec);

/* The next event, waiting up to sleep ticks where the system lets other programs run */
Boolean sys_next_event(EventRecord *ev, long sleep);

/* A reserve of memory, the Mac's "rainy day" block: when the heap is full, the Memory
   Manager is given it (through a grow-zone function), so the Toolbox can still draw,
   put up an alert, and keep Mill working. mem_short takes it back if there's room, and
   says whether it's still gone: memory has run out. */
void mem_init(void);
Boolean mem_short(void);
/* For mill://about: what's free in Mill's heap now, and the heap's whole size */
void mem_report(long *free, long *total);

#endif
