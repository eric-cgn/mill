#include <string.h>

#include "mac_sys.h"
#include "mac_memreport.h"

#define ROOT_DIR 2          /* fsRtDirID: a volume's root folder */
#define ROOT_PARENT 1       /* fsRtParID: what the root folder is in */
#define IS_FOLDER 0x10
#define GESTALT_TRAP 0xA1AD
#define FIND_FOLDER_PRESENT 1
#define ON_SYSTEM_DISK ((short)0x8000)   /* kOnSystemDisk */

Boolean sys_fsspec, sys_new_sf, sys_find_folder, sys_wne;

/* ------------------------------------------------------------ files */

static void pcopy(StringPtr to, ConstStr255Param from, int max)
{
    int n = from[0] > max ? max : from[0];
    memcpy(to + 1, from + 1, n);
    to[0] = (unsigned char)n;
}

/* A folder's or file's catalog entry in folder dir */
static OSErr cat_info(short vref, long dir, ConstStr255Param name, CInfoPBRec *pb)
{
    Str255 n;
    pcopy(n, name, 255);
    memset(pb, 0, sizeof *pb);
    pb->hFileInfo.ioNamePtr = n;
    pb->hFileInfo.ioVRefNum = vref;
    pb->hFileInfo.ioDirID = dir;
    return PBGetCatInfoSync(pb);
}

/* FSMakeFSSpec for System 6: a name in a folder, or a full path walked from the root */
static OSErr make_spec(short vref, long dir, ConstStr255Param name, FSSpec *spec)
{
    CInfoPBRec pb;
    const unsigned char *p = name + 1, *end = name + 1 + name[0];
    const unsigned char *colon = memchr(p, ':', name[0]);
    OSErr err;
    memset(spec, 0, sizeof *spec);
    if (!colon || colon == p) {
        /* a plain name (or a leading-colon partial path, which Mill doesn't use) */
        spec->vRefNum = vref;
        spec->parID = dir;
        pcopy(spec->name, name, 63);
        return cat_info(vref, dir, spec->name, &pb);
    }
    {
        /* the volume: everything up to the first colon */
        HParamBlockRec v;
        Str255 vol;
        memset(&v, 0, sizeof v);
        vol[0] = (unsigned char)(colon - p + 1);
        memcpy(vol + 1, p, vol[0]);
        v.volumeParam.ioNamePtr = vol;
        v.volumeParam.ioVolIndex = -1;
        if ((err = PBHGetVInfoSync(&v)) != noErr) return err;
        vref = v.volumeParam.ioVRefNum;
        dir = ROOT_DIR;
        p = colon + 1;
        if (p >= end) {
            /* just the volume: its root folder */
            spec->vRefNum = vref;
            spec->parID = ROOT_PARENT;
            spec->name[0] = (unsigned char)(vol[0] - 1);
            memcpy(spec->name + 1, vol + 1, spec->name[0]);
            return noErr;
        }
    }
    /* each folder in turn, then the last name */
    for (;;) {
        Str255 part;
        const unsigned char *next = memchr(p, ':', end - p);
        long n = (next ? next : end) - p;
        if (n > 63) return bdNamErr;
        part[0] = (unsigned char)n;
        memcpy(part + 1, p, n);
        if (!next || next + 1 >= end) {
            spec->vRefNum = vref;
            spec->parID = dir;
            pcopy(spec->name, part, 63);
            return cat_info(vref, dir, part, &pb);
        }
        if ((err = cat_info(vref, dir, part, &pb)) != noErr) return err == fnfErr ? dirNFErr : err;
        if (!(pb.hFileInfo.ioFlAttrib & IS_FOLDER)) return dirNFErr;
        dir = pb.dirInfo.ioDrDirID;
        p = next + 1;
    }
}

OSErr file_spec(short vref, long dir, ConstStr255Param name, FSSpec *spec)
{
    if (sys_fsspec) return FSMakeFSSpec(vref, dir, name, spec);
    return make_spec(vref, dir, name, spec);
}

static void h_pb(HParamBlockRec *pb, const FSSpec *spec, Str63 name)
{
    memset(pb, 0, sizeof *pb);
    pcopy(name, spec->name, 63);
    pb->fileParam.ioNamePtr = name;
    pb->fileParam.ioVRefNum = spec->vRefNum;
    pb->fileParam.ioDirID = spec->parID;
}

OSErr file_open(const FSSpec *spec, SInt8 perm, short *ref)
{
    HParamBlockRec pb;
    Str63 name;
    OSErr err;
    h_pb(&pb, spec, name);
    pb.ioParam.ioPermssn = perm;
    err = PBHOpenSync(&pb);
    *ref = pb.ioParam.ioRefNum;
    return err;
}

OSErr file_open_rf(const FSSpec *spec, SInt8 perm, short *ref)
{
    HParamBlockRec pb;
    Str63 name;
    OSErr err;
    h_pb(&pb, spec, name);
    pb.ioParam.ioPermssn = perm;
    err = PBHOpenRFSync(&pb);
    *ref = pb.ioParam.ioRefNum;
    return err;
}

OSErr file_get_info(const FSSpec *spec, FInfo *fi)
{
    HParamBlockRec pb;
    Str63 name;
    OSErr err;
    h_pb(&pb, spec, name);
    if ((err = PBHGetFInfoSync(&pb)) == noErr) *fi = pb.fileParam.ioFlFndrInfo;
    return err;
}

OSErr file_set_info(const FSSpec *spec, const FInfo *fi)
{
    HParamBlockRec pb;
    Str63 name;
    OSErr err;
    h_pb(&pb, spec, name);
    if ((err = PBHGetFInfoSync(&pb)) != noErr) return err;  /* keeps the dates as they are */
    pb.fileParam.ioFlFndrInfo = *fi;
    pb.fileParam.ioDirID = spec->parID;  /* GetFInfo changed it to the file's number */
    return PBHSetFInfoSync(&pb);
}

OSErr file_create(const FSSpec *spec, OSType creator, OSType type)
{
    HParamBlockRec pb;
    Str63 name;
    FInfo fi;
    OSErr err;
    h_pb(&pb, spec, name);
    if ((err = PBHCreateSync(&pb)) != noErr) return err;
    if ((err = file_get_info(spec, &fi)) != noErr) return err;
    fi.fdType = type;
    fi.fdCreator = creator;
    return file_set_info(spec, &fi);
}

OSErr file_delete(const FSSpec *spec)
{
    HParamBlockRec pb;
    Str63 name;
    h_pb(&pb, spec, name);
    return PBHDeleteSync(&pb);
}

short file_open_res(const FSSpec *spec, SInt8 perm)
{
    WDPBRec wd;
    short ref;
    if (sys_fsspec) return FSpOpenResFile((FSSpec *)spec, perm);
    /* System 6: through a working directory for the file's folder */
    memset(&wd, 0, sizeof wd);
    wd.ioVRefNum = spec->vRefNum;
    wd.ioWDDirID = spec->parID;
    wd.ioWDProcID = 'MiLL';
    if (PBOpenWDSync(&wd) != noErr) return -1;
    ref = OpenRFPerm(spec->name, wd.ioVRefNum, perm);
    PBCloseWDSync(&wd);
    return ref;
}

/* ------------------------------------------------------------ folders */

/* A working directory's volume and folder */
static OSErr wd_folder(short wdref, short *vref, long *dir)
{
    WDPBRec wd;
    OSErr err;
    memset(&wd, 0, sizeof wd);
    wd.ioVRefNum = wdref;
    if ((err = PBGetWDInfoSync(&wd)) != noErr) return err;
    *vref = wd.ioWDVRefNum;
    *dir = wd.ioWDDirID;
    return noErr;
}

OSErr sys_folder(OSType which, short *vref, long *dir)
{
    SysEnvRec env;
    if (sys_find_folder)
        return FindFolder(ON_SYSTEM_DISK, which, which == kPreferencesFolderType, vref, dir);
    /* System 6: the System Folder, which SysEnvirons gives as a working directory */
    if (SysEnvirons(1, &env) != noErr) return fnfErr;
    return wd_folder(env.sysVRefNum, vref, dir);
}

/* ------------------------------------------------------------ choosing files */

static Point dialog_at(short w, short h)
{
    Point p;
    p.h = (qd.screenBits.bounds.right - w) / 2;
    p.v = 20 + (qd.screenBits.bounds.bottom - 20 - h) / 3;
    return p;
}

Boolean sys_choose_file(FSSpec *spec)
{
    if (sys_new_sf) {
        StandardFileReply reply;
        StandardGetFile(NULL, -1, NULL, &reply);
        if (!reply.sfGood) return false;
        *spec = reply.sfFile;
        return true;
    } else {
        SFReply reply;
        short vref;
        long dir;
        SFGetFile(dialog_at(348, 200), "\p", NULL, -1, NULL, NULL, &reply);
        if (!reply.good || wd_folder(reply.vRefNum, &vref, &dir) != noErr) return false;
        spec->vRefNum = vref;
        spec->parID = dir;
        pcopy(spec->name, reply.fName, 63);
        return true;
    }
}

Boolean sys_choose_save(ConstStr255Param prompt, ConstStr255Param name, FSSpec *spec)
{
    if (sys_new_sf) {
        StandardFileReply reply;
        StandardPutFile(prompt, name, &reply);
        if (!reply.sfGood) return false;
        *spec = reply.sfFile;
        return true;
    } else {
        SFReply reply;
        short vref;
        long dir;
        SFPutFile(dialog_at(304, 184), prompt, name, NULL, &reply);
        if (!reply.good || wd_folder(reply.vRefNum, &vref, &dir) != noErr) return false;
        spec->vRefNum = vref;
        spec->parID = dir;
        pcopy(spec->name, reply.fName, 63);
        return true;
    }
}

/* ------------------------------------------------------------ events */

Boolean sys_next_event(EventRecord *ev, long sleep)
{
    if (sys_wne) return WaitNextEvent(everyEvent, ev, sleep, NULL);
    SystemTask();   /* desk accessories get their time */
    return GetNextEvent(everyEvent, ev);
}

/* ------------------------------------------------------------ memory */

#define RESERVE (24L * 1024)
#define MASTERS 4           /* blocks of master pointers made at the start, low in the heap */

static Handle reserve;
static Boolean restoring;   /* the reserve itself is what the Memory Manager wants room for */

/* Called by the Memory Manager when it can't find room: the reserve goes, once */
static pascal long grow_zone(Size needed)
{
    (void)needed;
    if (!reserve || !*reserve || restoring) return 0;
    EmptyHandle(reserve);
    return RESERVE;
}

void mem_init(void)
{
    short i;
    for (i = 0; i < MASTERS; i++) MoreMasters();
    reserve = NewHandle(RESERVE);
    SetGrowZone(NewGrowZoneUPP(grow_zone));
}

void mem_report(long *free, long *total)
{
    THz z = ApplicationZone();
    long contig;
    /* free once what can be purged is (unloaded code, resources), as Mill will when it needs it */
    PurgeSpace(free, &contig);
    *total = (long)z->bkLim - (long)&z->heapData;
}

Boolean mem_short(void)
{
    if (!reserve || *reserve) return false;
    /* only when it would leave some over: trying compacts the heap, which takes a while */
    if (FreeMem() >= RESERVE + RESERVE / 2) {
        restoring = true;
        ReallocateHandle(reserve, RESERVE);
        restoring = false;
    }
    return !*reserve;
}

#ifdef MILL_MEMREPORT
void memreport_sys(void)
{
    memreport_note(reserve, "Out-of-memory reserve", 1);
}
#endif
