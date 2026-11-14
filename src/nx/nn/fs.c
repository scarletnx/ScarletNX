/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nn::fs, native (fs.hook). The SDK's public file system functions, answered from the runtime's own back end with
   no fsp-srv session, as nvn/ answers NVN with no nvhost channel. The behavior is that of a clean-room spec of the
   SDK's functions (their checks, their order, their results and which results stop the game), never the SDK's code.
   What a save's back end answers (a file that exists, a write past the end) is the file system server's, the core in
   fs.c (fs.h), which this file calls.

   The layer keeps its own mount table and handles. A mount it made answers here. A mount name it does not know, and a
   handle it did not make, run the function's own code (hook_run_original) with the registers as the call left them,
   so the SDK's own mounts (one a mod made through the SDK's code) still work. The runtime has no fsp-srv: a function
   that reaches the server stops the game in sm, which names it (hle/services/sm.c). Its handles
   have bit 63 set, which no guest address has, so a handle tells which side made it.

   A 32-bit game (g_arm_mod) calls the same handlers through its own declarations (fs.hook's a32). What differs there
   is in four places: a size_t the game reads back is 4 bytes (put_size), a handle is 4 bytes with bit 0 set, which
   no pointer to the SDK's accessor has (handle_make, handle_parts), and the SDK's own code runs from the hooked
   function the thread is in (sdk_own).

   Mount kinds here: the game's RomFS (MountRom) and an add-on content's, read through fs.c's images, and the
   account's save (also read only), the device's save, cache storage, temporary storage and the SD card, on the host
   folders fs.c gives them. The mounts, files and directories are g_hle.nnfs's and g_hle.fs's slots, which a save
   state holds.

   Locks: g_nnfs.lock guards the tables, and is taken before fs_lock (the tree's) when both are held. */
#include "nx/nn/fs.h"
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "nx/hle/filesystem/fs.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/hle.h"
#include "nx/hle/services/services.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <stdlib.h>
#include <string.h>

#define SYM_MOUNT_ROM          "_ZN2nn2fs8MountRomEPKcPvm"
#define SYM_MOUNT_ROM_NAME     "_ZN2nn2fs8MountRomEPKc"
#define SYM_QUERY_ROM_CACHE    "_ZN2nn2fs22QueryMountRomCacheSizeEPm"
#define SYM_MOUNT_ROM_PROGRAM  "_ZN2nn2fs8MountRomEPKciPvm"
#define SYM_QUERY_ROM_PROGRAM  "_ZN2nn2fs22QueryMountRomCacheSizeEPmi"
#define SYM_UNMOUNT            "_ZN2nn2fs7UnmountEPKc"
#define SYM_OPEN_FILE          "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci"
#define SYM_CLOSE_FILE         "_ZN2nn2fs9CloseFileENS0_10FileHandleE"
#define SYM_READ_FILE          "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm"
#define SYM_READ_FILE_OPT      "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE"
#define SYM_READ_EXACT         "_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm"
#define SYM_READ_EXACT_OPT     "_ZN2nn2fs8ReadFileENS0_10FileHandleElPvmRKNS0_10ReadOptionE"
#define SYM_WRITE_FILE         "_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE"
#define SYM_FLUSH_FILE         "_ZN2nn2fs9FlushFileENS0_10FileHandleE"
#define SYM_SET_FILE_SIZE      "_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl"
#define SYM_GET_FILE_SIZE      "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE"
#define SYM_GET_OPEN_MODE      "_ZN2nn2fs15GetFileOpenModeENS0_10FileHandleE"
#define SYM_QUERY_RANGE        "_ZN2nn2fs10QueryRangeEPNS0_14QueryRangeInfoENS0_10FileHandleEll"
#define SYM_INVALIDATE_CACHE   "_ZN2nn2fs15InvalidateCacheENS0_10FileHandleE"
#define SYM_MOUNT_ROM_ON_FILE  "_ZN2nn2fs14MountRomOnFileEPKcNS0_10FileHandleEPvm"
#define SYM_QUERY_ROM_ON_FILE  "_ZN2nn2fs28QueryMountRomOnFileCacheSizeEPmNS0_10FileHandleE"
#define SYM_UNPREPARED         "_ZN2nn2fs20QueryUnpreparedRangeEPNS0_5RangeENS0_10FileHandleE"
#define SYM_UNPREPARED_DETAIL  "_ZN2nn2fs26QueryUnpreparedRangeDetailEPNS0_19UnpreparedRangeInfoENS0_10FileHandleE"
#define SYM_LAZY_RATE          "_ZN2nn2fs27QueryLazyLoadCompletionRateEPiNS0_10FileHandleEi"
#define SYM_LAZY_READY         "_ZN2nn2fs33ReadyLazyLoadFileForciblyForDebugENS0_10FileHandleEll"
#define SYM_OPEN_DIR           "_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci"
#define SYM_READ_DIR           "_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl"
#define SYM_DIR_COUNT          "_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE"
#define SYM_CLOSE_DIR          "_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE"
#define SYM_ENTRY_TYPE         "_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc"
#define SYM_CREATE_FILE        "_ZN2nn2fs10CreateFileEPKcl"
#define SYM_CREATE_FILE_OPT    "_ZN2nn2fs10CreateFileEPKcli"
#define SYM_DELETE_FILE        "_ZN2nn2fs10DeleteFileEPKc"
#define SYM_CREATE_DIR         "_ZN2nn2fs15CreateDirectoryEPKc"
#define SYM_DELETE_DIR         "_ZN2nn2fs15DeleteDirectoryEPKc"
#define SYM_DELETE_DIR_REC     "_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc"
#define SYM_CLEAN_DIR_REC      "_ZN2nn2fs25CleanDirectoryRecursivelyEPKc"
#define SYM_RENAME_FILE        "_ZN2nn2fs10RenameFileEPKcS2_"
#define SYM_RENAME_DIR         "_ZN2nn2fs15RenameDirectoryEPKcS2_"
#define SYM_FREE_SPACE         "_ZN2nn2fs16GetFreeSpaceSizeEPlPKc"
#define SYM_TOTAL_SPACE        "_ZN2nn2fs17GetTotalSpaceSizeEPlPKc"
#define SYM_TIME_STAMP         "_ZN2nn2fs24GetFileTimeStampForDebugEPNS0_13FileTimeStampEPKc"
#define SYM_TIME_STAMP_RAW     "_ZN2nn2fs27GetFileTimeStampRawForDebugEPNS0_16FileTimeStampRawEPKc"
#define SYM_COMMIT             "_ZN2nn2fs6CommitEPKc"
#define SYM_IS_PATH_MOUNTED    "_ZN2nn2fs13IsPathMountedEPKc"
#define SYM_IS_MOUNTED         "_ZN2nn2fs9IsMountedEPKc"
#define SYM_PATH_MAX           "_ZN2nn2fs16GetPathLengthMaxEPmPKc"
#define SYM_ENTRY_NAME_MAX     "_ZN2nn2fs21GetEntryNameLengthMaxEPmPKc"
#define SYM_UTF16_PATH_MAX     "_ZN2nn2fs21GetUtf16PathLengthMaxEPbPmPKc"
#define SYM_UTF16_NAME_MAX     "_ZN2nn2fs26GetUtf16EntryNameLengthMaxEPbPmPKc"
#define SYM_COMMIT_OPT         "_ZN2nn2fs6CommitEPKcRKNS0_12CommitOptionE"
#define SYM_COMMIT_SAVE        "_ZN2nn2fs14CommitSaveDataEPKc"
#define SYM_COMMIT_NAMES       "_ZN2nn2fs6CommitEPKPKci"
#define SYM_READ_CACHE_LIST    "_ZN2nn2fs20ReadCacheStorageListEPiPNS0_16CacheStorageInfoENS0_22CacheStorageListHandleEi"
#define SYM_CLOSE_CACHE_LIST   "_ZN2nn2fs21CloseCacheStorageListENS0_22CacheStorageListHandleE"
#define SYM_ENABLE_FILE_CACHE  "_ZN2nn2fs29EnableIndividualFileDataCacheEPKcPvm"
#define SYM_DISABLE_FILE_CACHE "_ZN2nn2fs30DisableIndividualFileDataCacheEPKc"
#define SYM_RESTORE_FLAG       "_ZN2nn2fs22GetSaveDataRestoreFlagEPbPKc"

/* a Result of the fs module (2): the description above bit 9 */
#define FSR(d) ((u32)2 | (u32)(d) << 9)
enum {
    R_PATH_NOT_FOUND = FSR(1),
    R_MOUNT_EXISTS = FSR(60),
    R_TARGET_NOT_FOUND = FSR(1002), /* a save that is not there: another application's */
    R_NOT_IMPLEMENTED = FSR(3001),  /* what the SDK's file system answers a request it has no code for */
    R_OUT_OF_RANGE = FSR(3005),
    R_CACHE_TOO_SMALL = FSR(6000), /* MountRom's cache buffer is smaller than QueryMountRomCacheSize's */
    R_PRECONDITION = FSR(6000),    /* PreconditionViolation: a save's attribute asked of a mount without one */
    R_NEGATIVE_COUNT = FSR(6001),
    R_PATH_TOO_LONG = FSR(6003), /* also a ".." above the root */
    R_PATH_FORMAT = FSR(6005),
    R_DIFFERENT_MOUNTS = FSR(6034),
    R_NEGATIVE_OFFSET = FSR(6061),
    R_NEGATIVE_SIZE = FSR(6062),
    R_NULL_ARGUMENT = FSR(6063),
    R_BAD_MOUNT_NAME = FSR(6065),
    R_COMMIT_NAME_COUNT = FSR(6071), /* Commit of more mounts than COMMIT_NAMES_MAX */
    R_BAD_OPEN_MODE = FSR(6072),
    R_BAD_DIR_MODE = FSR(6074),
    R_READ_ONLY = FSR(6364),       /* the RomFS refuses a change of its tree */
    R_UNFLUSHED_CLOSE = FSR(6454), /* CloseFile after a write that no flush followed */
    R_UNMOUNT_OPEN = FSR(6455),    /* Unmount with files or directories open on the mount */
};

#define PATH_MAX_LEN   0x300 /* a longer path after normalization is R_PATH_TOO_LONG */
#define MOUNT_NAME_MAX NNFS_NAME
#define ENTRY_SIZE     0x310 /* nn::fs::DirectoryEntry: name[0x301], type (s8) at 0x304, size (s64) at 0x308 */
#define ROM_NONE       0xFFFFFFFFu
#define PATH_LENGTH_MAX 0x300 /* what the SDK takes as a path's and a name's limit when the attribute names none */
#define ROM_DIR_FIXED  24 /* parent, sibling, first child directory, first file, hash, name length */
#define ROM_FILE_FIXED 32 /* parent, sibling, data offset (u64), size (u64), hash, name length */
#define DIR_NO_SIZE    0x40000000u /* OpenDirectory's flag: the entries need no file size */

#define NMOUNTS  NNFS_MOUNTS
#define NFILES   NNFS_FILES
#define NDIRS    NNFS_DIRS
#define TAG_FILE ((u64)1 << 63)
#define TAG_DIR  ((u64)3 << 62)
#define TAG_MASK ((u64)3 << 62)

/* the save types of a SaveDataAttribute */
enum { SAVE_ACCOUNT = 1, SAVE_DEVICE = 3, SAVE_TEMPORARY = 4, SAVE_CACHE = 5 };
/* the write state of an open file, as the SDK's file accessor keeps it */
enum { WRITE_CLEAN, WRITE_NEEDS_FLUSH, WRITE_FAILED };

/* A RomFS image's tables, the host's: the game's (fs.c's image) or a data storage's, an add-on content's (st), read once
   when it is mounted, and again after a save state's load (rom_at). g_hle.nnfs.roms says which image each one is */
typedef struct {
    int used;
    int game;
    u64 data_id;
    FsStorage st;
    u8* dirs;
    u64 dirs_size;
    u8* files;
    u64 files_size;
    u64 data; /* the image's offset of the data the file entries' offsets count from */
    u8 header[0x50];
} NRom;

enum { MOUNT_FREE = NNFS_MOUNT_FREE, MOUNT_ROM = NNFS_MOUNT_ROM, MOUNT_SAVE = NNFS_MOUNT_SAVE };
typedef NnfsMount NMount;
typedef NnfsFile NFile;
typedef NnfsDir NDir;

/* the host's side, which a save state never holds */
static struct {
    PMutex lock;
    int game_rom; /* the game's RomFS: 0 not looked at yet, 1 its tables read, -1 none to read (the image never changes
                     in a run) */
    NRom roms[NMOUNTS];
} g_nnfs;

void nnfs_init(void) { pm_init(&g_nnfs.lock); }

/* ---- the SDK's own code, for what is not native */

/* runs the hooked function's own code with the CPU as the call left it, and gives its x0, or a 32-bit game's r0: there
   the function is the one whose handler the thread is in, under whatever symbol its SDK gives it */
static u64 sdk_own(CPU* c, const char* symbol) {
    if (g_arm_mod) return (u32)hook_run_original_a32(c);
    u64 at = hook_site(symbol);
    if (!at) fatal("nn::fs: %s has no code of its own to run", symbol);
    hook_run_original(c, at);
    return c->x[0];
}

/* ---- the result policy */

/* Whether the SDK stops the game on r: a Result of another module, or of fs with a description of 1000 and above. The
   SDK's start (nninitInitializeSdkModule) sets SetResultHandledByApplication, which makes this the rule, and the game
   sets no handler of its own (the spec, sections 1.2 and 1.2a) */
static int abort_needed(u32 r) {
    if ((r & 0x1ff) != 2) return 1;
    return ((r >> 9) & 0x1fff) >= 1000;
}
SNX_NORETURN static void stop(const char* function, u32 r) {
    fatal("nn::fs::%s failed with result 0x%x (error %04u-%04u), which the SDK stops the game on", function, r,
          2000 + (r & 0x1ff), (r >> 9) & 0x1fff);
}
/* the end of a function that returns a Result */
static u32 done(const char* function, u32 r) {
    if (r && abort_needed(r)) stop(function, r);
    return r;
}
/* a Result of the "not enough space" range (fs, 30 to 45), which fails a write without sticking to the file */
static int space_result(u32 r) {
    u32 d = (r >> 9) & 0x1fff;
    return (r & 0x1ff) == 2 && d >= 30 && d <= 45;
}

/* ---- guest memory */

static const char* gstr(const CPU* c, u64 a) { return (const char*)hook_host(c, a); }
static void put_u64(CPU* c, u64 a, u64 v) { snx_st_le64(hook_host(c, a), v); }
static void put_u32(CPU* c, u64 a, u32 v) { snx_st_le32(hook_host(c, a), v); }
/* a size_t of the game's: 8 bytes, or a 32-bit game's 4 */
static void put_size(CPU* c, u64 a, u64 v) {
    if (g_arm_mod)
        put_u32(c, a, (u32)v);
    else
        put_u64(c, a, v);
}

/* ---- mount names and paths */

/* the native mount named by the first n bytes of name, or -1 (lock held) */
static int mount_find(const char* name, size_t n) {
    for (int i = 0; i < NMOUNTS; i++)
        if (g_hle.nnfs.mounts[i].kind != MOUNT_FREE && strlen(g_hle.nnfs.mounts[i].name) == n &&
            !memcmp(g_hle.nnfs.mounts[i].name, name, n))
            return i;
    return -1;
}
static int mount_named(const char* name) { return name ? mount_find(name, strnlen(name, MOUNT_NAME_MAX + 1)) : -1; }

/* CheckMountName: 0, R_NULL_ARGUMENT or R_BAD_MOUNT_NAME. 1 to 15 bytes of well-formed UTF-8 with no '/' or ':', not
   one ASCII letter (a drive), and not a reserved name (one that starts with '@') unless reserved (the name-only
   MountRom's CheckMountNameAcceptingReservedMountName) */
static u32 check_mount_name(const char* name, int reserved) {
    if (!name) return R_NULL_ARGUMENT;
    if (name[0] == '@' && !reserved) return R_BAD_MOUNT_NAME;
    size_t n = strnlen(name, MOUNT_NAME_MAX + 1);
    if (!n || n > MOUNT_NAME_MAX) return R_BAD_MOUNT_NAME;
    if (n == 1 && ((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z'))) return R_BAD_MOUNT_NAME;
    if (memchr(name, '/', n) || memchr(name, ':', n)) return R_BAD_MOUNT_NAME;
    if (!snx_utf8_valid(name, n, NULL, 0)) return R_BAD_MOUNT_NAME;
    return 0;
}

/* a new mount of kind named name (lock held): its index, or -1 with *r the Result */
static int mount_new(const char* name, int kind, int reserved, u32* r) {
    *r = check_mount_name(name, reserved);
    if (*r) return -1;
    if (mount_find(name, strlen(name)) >= 0) {
        *r = R_MOUNT_EXISTS;
        return -1;
    }
    for (int i = 0; i < NMOUNTS; i++)
        if (g_hle.nnfs.mounts[i].kind == MOUNT_FREE) {
            NMount* m = &g_hle.nnfs.mounts[i];
            *m = (NMount){0};
            m->kind = kind;
            snprintf(m->name, sizeof m->name, "%s", name);
            return i;
        }
    fatal("nn::fs: more than %d native mounts", NMOUNTS);
}

/* The native mount of a path ("name:/sub"), with its sub path normalized into out: '\' as '/', no empty, '.' or
   trailing parts, ".." taken back. -1 when the path is not on a native mount (the SDK's own code takes it, with its
   own errors for a malformed one), else the mount, with *r 0 or the path's error (lock held) */
static int path_mount(const char* path, char out[PATH_MAX_LEN + 1], u32* r) {
    size_t i = 0;
    *r = 0;
    if (!path) return -1;
    while (i < MOUNT_NAME_MAX + 1 && path[i] && path[i] != ':') i++;
    if (i == 0 || i > MOUNT_NAME_MAX || path[i] != ':') return -1;
    int m = mount_find(path, i);
    if (m < 0) return -1;
    const char* sub = path + i + 1;
    if (*sub != '/' && *sub != '\\') {
        *r = R_PATH_FORMAT;
        return m;
    }
    size_t len = 0;
    out[len++] = '/';
    while (*sub) {
        while (*sub == '/' || *sub == '\\') sub++;
        const char* part = sub;
        while (*sub && *sub != '/' && *sub != '\\') sub++;
        size_t n = (size_t)(sub - part);
        if (!n || (n == 1 && part[0] == '.')) continue;
        if (n == 2 && part[0] == '.' && part[1] == '.') {
            if (len == 1) { /* above the root */
                *r = R_PATH_TOO_LONG;
                return m;
            }
            while (len > 1 && out[len - 1] != '/') len--;
            if (len > 1) len--;
            continue;
        }
        if (len > 1) {
            if (len >= PATH_MAX_LEN) {
                *r = R_PATH_TOO_LONG;
                return m;
            }
            out[len++] = '/';
        }
        if (n > PATH_MAX_LEN - len) {
            *r = R_PATH_TOO_LONG;
            return m;
        }
        memcpy(out + len, part, n);
        len += n;
    }
    out[len] = 0;
    return m;
}

/* ---- the RomFS's tables */

/* size bytes of the image at off into host memory: 0, or -1 */
static int rom_peek(NRom* rm, u64 off, void* dst, u64 size) {
    return rm->game ? romfs_peek(off, dst, size) : fs_storage_read(&rm->st, off, dst, size);
}
/* reads the image's header and tables: 0, or -1 with the reason said */
static int rom_tables(NRom* rm, const char* what) {
    u8* h = rm->header;
    if (rom_peek(rm, 0, h, sizeof rm->header)) {
        WARN("nn::fs: %s has no RomFS header to read\n", what);
        return -1;
    }
    u64 dtab = snx_ld_le64(h + 24), dsize = snx_ld_le64(h + 32), ftab = snx_ld_le64(h + 56),
        fsize = snx_ld_le64(h + 64);
    if (snx_ld_le64(h) != 0x50 || dsize < ROM_DIR_FIXED || dsize > ((u64)1 << 30) || fsize > ((u64)1 << 30)) {
        WARN("nn::fs: %s has a RomFS header that the native layer does not read\n", what);
        return -1;
    }
    u8* dirs = malloc((size_t)dsize);
    u8* files = malloc(fsize ? (size_t)fsize : 1);
    if (!dirs || !files || rom_peek(rm, dtab, dirs, dsize) || rom_peek(rm, ftab, files, fsize)) {
        free(dirs);
        free(files);
        WARN("nn::fs: could not read the RomFS tables of %s\n", what);
        return -1;
    }
    rm->dirs = dirs;
    rm->dirs_size = dsize;
    rm->files = files;
    rm->files_size = fsize;
    rm->data = snx_ld_le64(h + 72);
    return 0;
}
static void rom_drop(NRom* rm) { /* lock held */
    if (!rm->used) return;
    free(rm->dirs);
    free(rm->files);
    if (!rm->game) fs_storage_close(&rm->st);
    *rm = (NRom){0};
}
/* the image, the game's or data_id's storage, its tables read into rm (lock held): 0, or the Result */
static u32 rom_load(NRom* rm, int game, u64 data_id) {
    char what[48];
    *rm = (NRom){0};
    rm->game = game;
    rm->data_id = data_id;
    if (game)
        snprintf(what, sizeof what, "the game's RomFS");
    else {
        snprintf(what, sizeof what, "the data %016llx", (unsigned long long)data_id);
        u32 r = fs_storage_open(data_id, &rm->st);
        if (r) {
            *rm = (NRom){0};
            return r;
        }
    }
    if (rom_tables(rm, what)) {
        if (!game) fs_storage_close(&rm->st);
        *rm = (NRom){0};
        return R_OUT_OF_RANGE; /* a storage the SDK's RomFS cannot read either */
    }
    rm->used = 1;
    return 0;
}
/* image i's tables (lock held), read again when the host's are not of the image g_hle.nnfs.roms[i] names, as after a
   save state's load */
static NRom* rom_at(u32 i) {
    const NnfsRom* ref = &g_hle.nnfs.roms[i];
    NRom* rm = &g_nnfs.roms[i];
    if (rm->used && rm->game == (int)ref->game && rm->data_id == ref->data_id) return rm;
    rom_drop(rm);
    if (rom_load(rm, (int)ref->game, ref->data_id))
        fatal("nn::fs: the RomFS image %u that a mount had can't be read again after the save state's load", i);
    return rm;
}
/* image i (not the game's, which stays for a new mount) no longer mounted */
static void rom_release(u32 i) {
    g_hle.nnfs.roms[i] = (NnfsRom){0};
    rom_drop(&g_nnfs.roms[i]);
}
/* roms[0], the game's RomFS (lock held): 0, or -1 when the game has none the tables can be read from, and the SDK's
   own code mounts it */
static int game_rom(void) {
    if (g_nnfs.game_rom < 0) return -1;
    NnfsRom* ref = &g_hle.nnfs.roms[0];
    *ref = (NnfsRom){.used = 1, .game = 1};
    NRom* rm = &g_nnfs.roms[0];
    if (rm->used && rm->game) return 0;
    rom_drop(rm);
    if (rom_load(rm, 1, 0)) {
        g_nnfs.game_rom = -1;
        *ref = (NnfsRom){0};
        return -1;
    }
    if (!g_nnfs.game_rom)
        LOG("nn::fs: the RomFS mounts natively (tables of %llu and %llu bytes)\n", (unsigned long long)rm->dirs_size,
            (unsigned long long)rm->files_size);
    g_nnfs.game_rom = 1;
    return 0;
}
/* the directory entry at off: its field i (a u32), or ROM_NONE past the table */
static u32 rom_dir_field(const NRom* rm, u32 off, int i) {
    if ((u64)off + ROM_DIR_FIXED > rm->dirs_size) return ROM_NONE;
    return snx_ld_le32(rm->dirs + off + 4 * i);
}
static u32 rom_file_field(const NRom* rm, u32 off, int i) {
    if ((u64)off + ROM_FILE_FIXED > rm->files_size) return ROM_NONE;
    return snx_ld_le32(rm->files + off + 4 * i);
}
/* an entry's name and its length, or NULL past the table */
static const char* rom_name(const NRom* rm, int dir, u32 off, u32* n) {
    const u8* t = dir ? rm->dirs : rm->files;
    u64 size = dir ? rm->dirs_size : rm->files_size, fixed = dir ? ROM_DIR_FIXED : ROM_FILE_FIXED;
    if ((u64)off + fixed > size) return NULL;
    *n = snx_ld_le32(t + off + fixed - 4);
    if (*n > size - off - fixed) return NULL;
    return (const char*)t + off + fixed;
}
static u64 rom_file_u64(const NRom* rm, u32 off, int at) { return snx_ld_le64(rm->files + off + at); }

/* the child of directory dir named by [name, name + n): its entry, *is_dir set, or ROM_NONE. Names compare exactly */
static u32 rom_child(const NRom* rm, u32 dir, const char* name, size_t n, int* is_dir) {
    for (u32 e = rom_dir_field(rm, dir, 2); e != ROM_NONE; e = rom_dir_field(rm, e, 1)) {
        u32 len;
        const char* s = rom_name(rm, 1, e, &len);
        if (!s) break;
        if (len == n && !memcmp(s, name, n)) {
            *is_dir = 1;
            return e;
        }
    }
    for (u32 e = rom_dir_field(rm, dir, 3); e != ROM_NONE; e = rom_file_field(rm, e, 1)) {
        u32 len;
        const char* s = rom_name(rm, 0, e, &len);
        if (!s) break;
        if (len == n && !memcmp(s, name, n)) {
            *is_dir = 0;
            return e;
        }
    }
    return ROM_NONE;
}
/* the entry at a normalized path: 0, or R_PATH_NOT_FOUND */
static u32 rom_lookup(const NRom* rm, const char* path, int* is_dir, u32* entry) {
    u32 cur = 0;
    int dir = 1;
    const char* p = path + 1;
    while (*p) {
        const char* end = strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (!dir) return R_PATH_NOT_FOUND; /* a part under a file */
        cur = rom_child(rm, cur, p, n, &dir);
        if (cur == ROM_NONE) return R_PATH_NOT_FOUND;
        p += n;
        if (*p == '/') p++;
    }
    *is_dir = dir;
    *entry = cur;
    return 0;
}
/* the cache MountRom needs (lock held, tables loaded): the directory hash, directory, file hash and file tables, the
   header's sizes at 0x10, 0x20, 0x30 and 0x40, together and rounded up to 8 */
static u64 rom_cache_size(const NRom* rm) {
    const u8* h = rm->header;
    u64 n = snx_ld_le64(h + 0x10) + snx_ld_le64(h + 0x20) + snx_ld_le64(h + 0x30) + snx_ld_le64(h + 0x40);
    return (n + 7) & ~(u64)7;
}

/* ---- handles */

/* A native handle as the game holds it, of a file or a directory (dir), slot i at generation gen. A 64-bit game's:
   bit 63, the kind in bit 62, the generation and the index above the low 4 bits, which stay 0: the console's handle
   is a pointer to an accessor the SDK allocates, and a game may keep flags of its own in those bits and mask them off
   before it passes the handle back. A 32-bit game's FileHandle and DirectoryHandle are one pointer of 4 bytes: bit 0
   set, which no pointer to the SDK's accessor (a heap object) has, bit 1 for a directory, the index in bits 2 to 11
   and the low 20 bits of the generation above them */
#define H32_NATIVE   1u
#define H32_DIR      2u
#define H32_GEN_MASK 0xfffffu
#define H64_LOW      4 /* the low bits a 64-bit handle leaves 0 */
typedef char nnfs_index_fits_a_handle32[NFILES <= 1024 && NDIRS <= 1024 ? 1 : -1]; /* the index's 10 bits */
static u64 handle_make(int dir, u32 gen, u32 i) {
    if (g_arm_mod) return H32_NATIVE | (dir ? H32_DIR : 0) | i << 2 | (gen & H32_GEN_MASK) << 12;
    return (dir ? TAG_DIR : TAG_FILE) | (u64)gen << 32 | (u64)i << H64_LOW;
}
/* h's slot and generation when it is a native handle of the kind, and the bits of a slot's generation it holds: 1 */
static int handle_parts(u64 h, int dir, u32* i, u32* gen, u32* gen_mask) {
    if (g_arm_mod) {
        if ((h & (H32_NATIVE | H32_DIR)) != (H32_NATIVE | (dir ? H32_DIR : 0)) || h >> 32) return 0;
        *i = (u32)h >> 2 & 0x3ffu;
        *gen = (u32)h >> 12;
        *gen_mask = H32_GEN_MASK;
        return 1;
    }
    if ((h & TAG_MASK) != (dir ? TAG_DIR : TAG_FILE) || (h & ((1u << H64_LOW) - 1))) return 0;
    *i = (u32)(h & 0xffffffffu) >> H64_LOW;
    *gen = (u32)(h >> 32) & 0x3fffffffu;
    *gen_mask = 0x3fffffffu;
    return 1;
}
static void put_handle(CPU* c, u64 a, u64 h) {
    if (g_arm_mod)
        put_u32(c, a, (u32)h);
    else
        put_u64(c, a, h);
}
static int file_of(u64 h) { /* lock held: the native file of h, or -1 */
    u32 i, gen, mask;
    if (!handle_parts(h, 0, &i, &gen, &mask)) return -1;
    if (i >= NFILES || !g_hle.nnfs.files[i].used || (g_hle.nnfs.files[i].gen & mask) != gen) return -1;
    return (int)i;
}
static int dir_of(u64 h) {
    u32 i, gen, mask;
    if (!handle_parts(h, 1, &i, &gen, &mask)) return -1;
    if (i >= NDIRS || !g_hle.nnfs.dirs[i].used || (g_hle.nnfs.dirs[i].gen & mask) != gen) return -1;
    return (int)i;
}
static int is_native(u64 h) { return g_arm_mod ? (h & H32_NATIVE) != 0 : (h & TAG_FILE) != 0; }

/* a native handle that no live object has: the game passed a closed one, which the SDK's own code would read freed
   memory for */
SNX_NORETURN static void stale(const char* function, u64 h) {
    if (!g_arm_mod && (h & ((1u << H64_LOW) - 1)))
        fatal("nn::fs::%s: the game passed the handle %llx, whose low %d bits a native handle has clear: flags of the "
              "game's own left in it, or a save state from a runtime that put a handle's index there",
              function, (unsigned long long)h, H64_LOW);
    fatal("nn::fs::%s: the game passed the handle %llx, which is closed", function, (unsigned long long)h);
}
/* the native file of h, with the lock taken: the caller unlocks */
static NFile* file_take(const char* function, u64 h) {
    pm_lock(&g_nnfs.lock);
    int i = file_of(h);
    if (i < 0) stale(function, h);
    return &g_hle.nnfs.files[i];
}
static NDir* dir_take(const char* function, u64 h) {
    pm_lock(&g_nnfs.lock);
    int i = dir_of(h);
    if (i < 0) stale(function, h);
    return &g_hle.nnfs.dirs[i];
}
static int is_save(const NFile* f) { return g_hle.nnfs.mounts[f->mount].kind == MOUNT_SAVE; }
/* a save's root, file and directory: the slots of g_hle.fs that fs.c's save state finds again by path. obj marks one
   taken */
static FsNode* mount_root(int m) { return g_hle.fs.fss[g_hle.nnfs.mounts[m].fss].root; }
static FileData* file_data(const NFile* f) { return &g_hle.fs.files[f->slot]; }
static DirData* dir_data(const NDir* d) { return &g_hle.fs.dirs[d->slot]; }
/* a free slot of g_hle.fs's tables, whose obj is 0 (fs's lock held) */
static u32 fss_slot(void) {
    for (u32 i = 0; i < FS_FS_MAX; i++)
        if (!g_hle.fs.fss[i].obj) return i;
    fatal("nn::fs: more than %d saves mounted natively", FS_FS_MAX);
}
static u32 file_slot(void) {
    for (u32 i = 0; i < FS_FILE_MAX; i++)
        if (!g_hle.fs.files[i].obj) return i;
    fatal("nn::fs: more than %d save files open natively", FS_FILE_MAX);
}
static u32 dir_slot(void) {
    for (u32 i = 0; i < FS_DIR_MAX; i++)
        if (!g_hle.fs.dirs[i].obj) return i;
    fatal("nn::fs: more than %d save directories open natively", FS_DIR_MAX);
}
/* the file's state after a write, flush or size change: ok on success, else failed, with the Result kept for every
   later operation unless it is one of space */
static void after_change(NFile* f, u32 r, u32 ok) {
    if (!r) {
        f->wstate = ok;
        return;
    }
    f->wstate = WRITE_FAILED;
    if (!space_result(r)) f->sticky = r;
}

/* ---- mounts */

/* a RomFS image (roms[rom], its tables read) mounted as name, its tables copied into the game's cache as the SDK keeps
   them there: 0, or the Result (lock held). A mount that fails leaves the image as it was */
static u32 mount_image(CPU* c, const char* function, u64 name, u64 cache, u64 cache_size, u32 rom) {
    NRom* rm = rom_at(rom);
    const char* s = name ? gstr(c, name) : NULL;
    u32 r = check_mount_name(s, 0);
    if (!r && !cache) r = R_NULL_ARGUMENT;
    if (!r && cache_size < rom_cache_size(rm)) r = R_CACHE_TOO_SMALL;
    int m = r ? -1 : mount_new(s, MOUNT_ROM, 0, &r);
    if (m < 0) return r;
    g_hle.nnfs.mounts[m].rom = rom;
    u64 at = cache; /* the four tables in the header's order */
    for (int t = 0; t < 4; t++) {
        u64 off = snx_ld_le64(rm->header + 8 + 16 * t), size = snx_ld_le64(rm->header + 16 + 16 * t);
        if (rom_peek(rm, off, hook_host(c, at), size)) fatal("nn::fs: could not read the RomFS tables for %s", function);
        at += size;
    }
    LOG("nn::fs: %s(\"%s\") native\n", function, s);
    return 0;
}

/* MountRom(name, cache, cache_size), whose own code is the function symbol's */
static u32 mount_rom(CPU* c, const char* symbol, u64 name, u64 cache, u64 cache_size) {
    pm_lock(&g_nnfs.lock);
    if (game_rom()) {
        pm_unlock(&g_nnfs.lock);
        return (u32)sdk_own(c, symbol);
    }
    u32 r = mount_image(c, "MountRom", name, cache, cache_size, 0);
    pm_unlock(&g_nnfs.lock);
    return done("MountRom", r);
}
u32 nnfs_mount_rom(CPU* c, u64 name, u64 cache, u64 cache_size) {
    return mount_rom(c, SYM_MOUNT_ROM, name, cache, cache_size);
}

/* MountRom(name): the SDK allocates its cache itself (SetAllocator's allocator) and frees it at Unmount; the native
   layer keeps the tables, so nothing of the game's memory is taken. The name may be a reserved one */
u32 nnfs_mount_rom_name(CPU* c, u64 name) {
    pm_lock(&g_nnfs.lock);
    if (game_rom()) {
        pm_unlock(&g_nnfs.lock);
        return (u32)sdk_own(c, SYM_MOUNT_ROM_NAME);
    }
    u32 r;
    const char* s = name ? gstr(c, name) : NULL;
    int m = mount_new(s, MOUNT_ROM, 1, &r);
    if (m >= 0) LOG("nn::fs: MountRom(\"%s\") native\n", s);
    pm_unlock(&g_nnfs.lock);
    return done("MountRom", r);
}

static u32 query_rom_cache_size(CPU* c, const char* symbol, u64 size) {
    pm_lock(&g_nnfs.lock);
    if (game_rom()) {
        pm_unlock(&g_nnfs.lock);
        return (u32)sdk_own(c, symbol);
    }
    u64 n = rom_cache_size(rom_at(0));
    pm_unlock(&g_nnfs.lock);
    if (!size) return done("QueryMountRomCacheSize", R_NULL_ARGUMENT);
    put_size(c, size, n);
    return 0;
}
u32 nnfs_query_mount_rom_cache_size(CPU* c, u64 size) { return query_rom_cache_size(c, SYM_QUERY_ROM_CACHE, size); }

/* MountRom and QueryMountRomCacheSize for the program of the application at index, whose RomFS the SDK opens: this
   program's own when index is this program's (g_program, 0 for a game of one program), answered as the overloads
   without an index are. The native layer has no other program's RomFS, so the game stops, naming both */
static void own_program(const char* function, i32 index) {
    if (index != g_program)
        fatal("nn::fs::%s: the game asks for the RomFS of program %d of its application, and this is program %d, the "
              "only one whose RomFS the runtime mounts",
              function, index, g_program);
}
u32 nnfs_mount_rom_program(CPU* c, u64 name, i32 index, u64 cache, u64 cache_size) {
    own_program("MountRom", index);
    return mount_rom(c, SYM_MOUNT_ROM_PROGRAM, name, cache, cache_size);
}
u32 nnfs_query_mount_rom_cache_size_program(CPU* c, u64 size, i32 index) {
    own_program("QueryMountRomCacheSize", index);
    return query_rom_cache_size(c, SYM_QUERY_ROM_PROGRAM, size);
}

/* The data storage of id (an add-on content's, or a system archive's, as OpenDataStorageByDataId gives it), its tables
   read into a free image (lock held): the image, or -1 with *r the Result */
static int data_image(u64 id, u32* r) {
    int rom = -1;
    for (int i = 1; i < NMOUNTS && rom < 0; i++)
        if (!g_hle.nnfs.roms[i].used) rom = i;
    if (rom < 0) fatal("nn::fs: more than %d RomFS images mounted natively", NMOUNTS - 1);
    rom_drop(&g_nnfs.roms[rom]); /* an image of a state from before a load */
    *r = rom_load(&g_nnfs.roms[rom], 0, id);
    if (*r) return -1;
    g_hle.nnfs.roms[rom] = (NnfsRom){.used = 1, .data_id = id};
    return rom;
}
/* The add-on content of index: its data id is the game's base id + index, as aoc's PrepareAddOnContent, which
   always finds it in place here, gives it */
static int aoc_image(i32 index, u32* r) { return data_image(aoc_base_id(g_hle.kernel.title_id) + (u64)(u32)index, r); }

u32 nnfs_mount_add_on_content(CPU* c, u64 name, i32 index, u64 cache, u64 cache_size) {
    pm_lock(&g_nnfs.lock);
    u32 r;
    int rom = aoc_image(index, &r);
    if (rom >= 0) {
        r = mount_image(c, "MountAddOnContent", name, cache, cache_size, (u32)rom);
        if (r) rom_release((u32)rom);
    }
    pm_unlock(&g_nnfs.lock);
    return done("MountAddOnContent", r);
}

u32 nnfs_query_mount_add_on_content_cache_size(CPU* c, u64 size, i32 index) {
    pm_lock(&g_nnfs.lock);
    u32 r = 0;
    u64 n = 0;
    if (!size)
        r = R_NULL_ARGUMENT;
    else {
        int rom = aoc_image(index, &r);
        if (rom >= 0) {
            n = rom_cache_size(rom_at((u32)rom));
            rom_release((u32)rom);
        }
    }
    pm_unlock(&g_nnfs.lock);
    if (!r) put_size(c, size, n);
    return done("QueryMountAddOnContentCacheSize", r);
}

/* MountSystemData and its cache size: a system archive's RomFS, mounted as an add-on content's is. A game's SDK
   through 13 reads its profanity filter's word lists this way */
u32 nnfs_mount_system_data(CPU* c, u64 name, u64 id, u64 cache, u64 cache_size) {
    pm_lock(&g_nnfs.lock);
    u32 r;
    int rom = data_image(id, &r);
    if (rom >= 0) {
        r = mount_image(c, "MountSystemData", name, cache, cache_size, (u32)rom);
        if (r) rom_release((u32)rom);
    }
    pm_unlock(&g_nnfs.lock);
    return done("MountSystemData", r);
}
u32 nnfs_query_mount_system_data_cache_size(CPU* c, u64 size, u64 id) {
    pm_lock(&g_nnfs.lock);
    u32 r = 0;
    u64 n = 0;
    if (!size)
        r = R_NULL_ARGUMENT;
    else {
        int rom = data_image(id, &r);
        if (rom >= 0) {
            n = rom_cache_size(rom_at((u32)rom));
            rom_release((u32)rom);
        }
    }
    pm_unlock(&g_nnfs.lock);
    if (!r) put_size(c, size, n);
    return done("QueryMountSystemDataCacheSize", r);
}

/* mount m's root folder (retained), in a slot of g_hle.fs.fss: noted and journaled as fs_save_root says */
static void mount_root_set(int m, FsNode* root, int noted, int journaled) {
    fs_lock();
    u32 i = fss_slot();
    g_hle.fs.fss[i] = (FsData){.obj = 1, .root = root, .noted = (u32)noted, .journaled = (u32)journaled};
    fs_unlock();
    g_hle.nnfs.mounts[m].fss = i;
}
/* a save of type for uid (16 bytes) and index, mounted as name, read only when read_only */
static u32 mount_save(CPU* c, const char* function, u64 name, const u8 uid[16], u8 type, u16 index, int read_only) {
    pm_lock(&g_nnfs.lock);
    u32 r;
    const char* s = name ? gstr(c, name) : NULL;
    int m = mount_new(s, MOUNT_SAVE, 0, &r);
    if (m >= 0) {
        FsNode* root;
        int noted, journaled;
        r = fs_save_root(type, uid, index, &root, &noted, &journaled);
        if (r)
            g_hle.nnfs.mounts[m] = (NMount){0};
        else {
            mount_root_set(m, root, noted, journaled);
            g_hle.nnfs.mounts[m].read_only = (u32)read_only;
            g_hle.nnfs.mounts[m].save_type = type;
            LOG("nn::fs: %s(\"%s\") native\n", function, s);
        }
    }
    pm_unlock(&g_nnfs.lock);
    return done(function, r);
}
static const u8 g_nobody[16]; /* the user of a save that no account owns */
u32 nnfs_mount_save_data(CPU* c, u64 name, u64 uid) {
    u8 id[16];
    memcpy(id, hook_host(c, uid), sizeof id); /* nn::account::Uid, two u64: the fs UserId is the same bytes */
    return mount_save(c, "MountSaveData", name, id, SAVE_ACCOUNT, 0, 0);
}
/* MountDeviceSaveData: the application's save that the device keeps, for every user. The SDK opens it as it does the
   account's, in the user's space, with a SaveDataAttribute of type Device and no user (read in SDK 0.12.16) */
u32 nnfs_mount_device_save_data(CPU* c, u64 name) {
    return mount_save(c, "MountDeviceSaveData", name, g_nobody, SAVE_DEVICE, 0, 0);
}
/* The saves of an application: the game's own are here, and no other game's are. Its own id's save of a user is made
   when it is opened (fs_save_root), as the console makes it at the game's start, so it is there */
static int own_application(u64 id) { return id == g_hle.kernel.title_id; }
/* MountSaveDataReadOnly: MountSaveData's checks and save, opened read only (command 53 on the console); another
   application's save is not found */
static u32 mount_save_read_only(CPU* c, u64 name, u64 application, const u8 uid[16]) {
    if (!own_application(application)) {
        u32 r = check_mount_name(name ? gstr(c, name) : NULL, 0);
        return done("MountSaveDataReadOnly", r ? r : R_TARGET_NOT_FOUND);
    }
    return mount_save(c, "MountSaveDataReadOnly", name, uid, SAVE_ACCOUNT, 0, 1);
}
u32 nnfs_mount_save_data_read_only(CPU* c, u64 name, u64 application, u64 uid) {
    u8 id[16];
    memcpy(id, hook_host(c, uid), sizeof id);
    return mount_save_read_only(c, name, application, id);
}
u32 nnfs_mount_save_data_read_only_user(CPU* c, u64 name, u64 application, u64 uid_lo, u64 uid_hi) {
    u8 id[16];
    snx_st_le64(id, uid_lo);
    snx_st_le64(id + 8, uid_hi);
    return mount_save_read_only(c, name, application, id);
}
/* IsSaveDataExisting: the game's own account save is there, another application's is not (the console's open of it
   answers 2-1002, which the SDK turns into false) */
int nnfs_is_save_data_existing(CPU* c, u64 application, u64 uid) {
    (void)c, (void)uid;
    return own_application(application);
}
int nnfs_is_own_save_data_existing(CPU* c, u64 uid) {
    (void)c, (void)uid;
    return 1;
}
u32 nnfs_is_save_data_existing_result(CPU* c, u64 out, u64 application, u64 uid) {
    (void)uid;
    if (!out) return done("IsSaveDataExisting", R_NEGATIVE_COUNT);
    *(u8*)hook_host(c, out) = (u8)own_application(application);
    return 0;
}
u32 nnfs_is_own_save_data_existing_result(CPU* c, u64 out, u64 uid) {
    (void)uid;
    if (!out) return done("IsSaveDataExisting", R_NEGATIVE_COUNT);
    *(u8*)hook_host(c, out) = 1;
    return 0;
}
u32 nnfs_mount_cache_storage(CPU* c, u64 name) {
    return mount_save(c, "MountCacheStorage", name, g_nobody, SAVE_CACHE, 0, 0);
}
u32 nnfs_mount_cache_storage_index(CPU* c, u64 name, i32 index) {
    return mount_save(c, "MountCacheStorage", name, g_nobody, SAVE_CACHE, (u16)index, 0);
}
u32 nnfs_mount_temporary_storage(CPU* c, u64 name) {
    return mount_save(c, "MountTemporaryStorage", name, g_nobody, SAVE_TEMPORARY, 0, 0);
}

/* IsMounted: a native mount's name is mounted, though the SDK's own table never holds it (a game that asks before it
   opens a file would refuse every file of the mount); any other name is that table's to answer */
int nnfs_is_mounted(CPU* c, u64 name) {
    pm_lock(&g_nnfs.lock);
    int m = mount_named(name ? gstr(c, name) : NULL);
    pm_unlock(&g_nnfs.lock);
    if (m >= 0) return 1;
    return (int)(sdk_own(c, SYM_IS_MOUNTED) & 1);
}

void nnfs_unmount(CPU* c, u64 name) {
    pm_lock(&g_nnfs.lock);
    int m = mount_named(name ? gstr(c, name) : NULL);
    if (m < 0) {
        pm_unlock(&g_nnfs.lock);
        sdk_own(c, SYM_UNMOUNT);
        return;
    }
    NMount* mt = &g_hle.nnfs.mounts[m];
    if (mt->opens) stop("Unmount", R_UNMOUNT_OPEN);
    if (mt->kind == MOUNT_SAVE) {
        fs_lock();
        fs_root_release(g_hle.fs.fss[mt->fss].root);
        g_hle.fs.fss[mt->fss] = (FsData){0};
        fs_unlock();
    }
    if (mt->kind == MOUNT_ROM && mt->rom) rom_release(mt->rom); /* the game's image stays for a new mount */
    *mt = (NMount){0};
    pm_unlock(&g_nnfs.lock);
}

/* ---- files */

u32 nnfs_open_file(CPU* c, u64 handle, u64 path, i32 mode) {
    char p[PATH_MAX_LEN + 1];
    u32 r;
    pm_lock(&g_nnfs.lock);
    int m = path_mount(path ? gstr(c, path) : NULL, p, &r);
    if (m < 0) {
        pm_unlock(&g_nnfs.lock);
        return (u32)sdk_own(c, SYM_OPEN_FILE);
    }
    if (!r && !handle) r = R_NULL_ARGUMENT;
    if (!r && ((u32)mode > 7 || !(mode & 3))) r = R_BAD_OPEN_MODE;
    NMount* mt = &g_hle.nnfs.mounts[m];
    NFile opened = {0};
    if (!r && mt->kind == MOUNT_ROM) {
        const NRom* rm = rom_at(mt->rom);
        int is_dir = 0;
        u32 e = 0;
        if (mode != 1) r = R_BAD_OPEN_MODE; /* the RomFS opens for reading only */
        if (!r) r = rom_lookup(rm, p, &is_dir, &e);
        if (!r && is_dir) r = R_PATH_NOT_FOUND;
        if (!r) {
            opened.rom = mt->rom;
            opened.data = rm->data + rom_file_u64(rm, e, 8);
            opened.size = rom_file_u64(rm, e, 16);
        }
    } else if (!r && mt->read_only && (mode & 2)) {
        r = R_BAD_OPEN_MODE; /* HYPOTHESIS: a read-only save refuses a write mode as the RomFS does */
    } else if (!r) {
        fs_lock();
        opened.slot = file_slot();
        r = fs_file_open(mount_root(m), p, (u32)mode, &g_hle.fs.files[opened.slot]);
        if (!r) g_hle.fs.files[opened.slot].obj = 1;
        fs_unlock();
    }
    if (!r) {
        int i = -1;
        for (int k = 0; k < NFILES && i < 0; k++)
            if (!g_hle.nnfs.files[k].used) i = k;
        if (i < 0) fatal("nn::fs: more than %d files open natively", NFILES);
        NFile* f = &g_hle.nnfs.files[i];
        u32 gen = (f->gen + 1) & 0x3fffffffu;
        *f = opened;
        f->used = 1;
        f->gen = gen;
        f->mount = (u32)m;
        f->mode = (u32)mode;
        mt->opens++;
        put_handle(c, handle, handle_make(0, gen, (u32)i));
    }
    pm_unlock(&g_nnfs.lock);
    return done("OpenFile", r);
}

void nnfs_close_file(CPU* c, u64 h) {
    if (!is_native(h)) {
        sdk_own(c, SYM_CLOSE_FILE);
        return;
    }
    NFile* f = file_take("CloseFile", h);
    if (f->wstate == WRITE_NEEDS_FLUSH && !f->sticky) stop("CloseFile", R_UNFLUSHED_CLOSE);
    if (is_save(f)) {
        fs_lock();
        fs_file_close(file_data(f));
        *file_data(f) = (FileData){0};
        fs_unlock();
    }
    g_hle.nnfs.mounts[f->mount].opens--;
    u32 gen = f->gen;
    *f = (NFile){0};
    f->gen = gen;
    pm_unlock(&g_nnfs.lock);
}

/* FileAccessor::Read's checks in their order, then the read: *n the bytes read */
static u32 file_read(const char* function, u64 h, u64 read_out, int want_read, i64 offset, u64 buffer, u64 size,
                     u64* n) {
    *n = 0;
    NFile* f = file_take(function, h);
    u32 r = f->sticky;
    if (!r && want_read && !read_out) r = R_NULL_ARGUMENT;
    if (!r && size) {
        if (!buffer)
            r = R_NULL_ARGUMENT;
        else if (offset < 0)
            r = R_NEGATIVE_OFFSET;
        else if ((i64)size < 0)
            r = R_NEGATIVE_SIZE;
        else if ((u64)offset > (u64)INT64_MAX - size)
            r = R_OUT_OF_RANGE;
    }
    if (r || !size) {
        pm_unlock(&g_nnfs.lock);
        return r;
    }
    if (is_save(f)) {
        fs_lock();
        r = fs_file_read(file_data(f), (u64)offset, buffer, size, n);
        fs_unlock();
        pm_unlock(&g_nnfs.lock);
        return r;
    }
    /* a RomFS never changes, and its image stays while a file is open on it (Unmount stops the game first): read it
       with no lock held */
    NRom* rm = rom_at(f->rom);
    u64 data = f->data, total = f->size;
    pm_unlock(&g_nnfs.lock);
    if ((u64)offset > total) return R_OUT_OF_RANGE; /* at the end reads nothing, past it is an error */
    u64 count = size < total - (u64)offset ? size : total - (u64)offset;
    if (rm->game ? romfs_read_guest(data + (u64)offset, buffer, count)
                 : fs_storage_read_guest(&rm->st, data + (u64)offset, buffer, count))
        return R_OUT_OF_RANGE;
    /* the game's RomFS is one storage whatever mount reads it (the SDK read it through one IStorage); another
       image is its own */
    u64 wait = fs_pace_read(rm->game ? (const void*)&g_hle.fs : (const void*)&rm->st, data + (u64)offset, count);
    if (wait) {
        /* off the thread's core, as the console's storage read blocks the thread and its core runs the others: a
           movie's decoder, on the core of its read-ahead, set up too late to free its first buffers otherwise */
        KThread* t = kthread_cur();
        core_leave(t);
        plat_sleep_ns((i64)wait);
        core_enter(t);
    }
    *n = count;
    return 0;
}

u32 nnfs_read_file(CPU* c, u64 read, u64 h, i64 offset, u64 buffer, u64 size) {
    if (!is_native(h)) return (u32)sdk_own(c, SYM_READ_FILE);
    u64 n;
    u32 r = file_read("ReadFile", h, read, 1, offset, buffer, size, &n);
    if (!r) put_size(c, read, n);
    return done("ReadFile", r);
}
u32 nnfs_read_file_option(CPU* c, u64 read, u64 h, i64 offset, u64 buffer, u64 size, u64 option) {
    (void)option; /* ReadOption has no flag a read here differs by */
    if (!is_native(h)) return (u32)sdk_own(c, SYM_READ_FILE_OPT);
    u64 n;
    u32 r = file_read("ReadFile", h, read, 1, offset, buffer, size, &n);
    if (!r) put_size(c, read, n);
    return done("ReadFile", r);
}
/* the overloads with no count read the whole size, or fail with R_OUT_OF_RANGE */
u32 nnfs_read_file_exact(CPU* c, u64 h, i64 offset, u64 buffer, u64 size) {
    if (!is_native(h)) return (u32)sdk_own(c, SYM_READ_EXACT);
    u64 n;
    u32 r = file_read("ReadFile", h, 0, 0, offset, buffer, size, &n);
    if (!r && n != size) r = R_OUT_OF_RANGE;
    return done("ReadFile", r);
}
u32 nnfs_read_file_exact_option(CPU* c, u64 h, i64 offset, u64 buffer, u64 size, u64 option) {
    (void)option;
    if (!is_native(h)) return (u32)sdk_own(c, SYM_READ_EXACT_OPT);
    u64 n;
    u32 r = file_read("ReadFile", h, 0, 0, offset, buffer, size, &n);
    if (!r && n != size) r = R_OUT_OF_RANGE;
    return done("ReadFile", r);
}

/* FileAccessor::Write: the sticky Result, the checks in their order, the write, then the file's state */
u32 nnfs_write_file(CPU* c, u64 h, i64 offset, u64 buffer, u64 size, u64 option) {
    if (!is_native(h)) return (u32)sdk_own(c, SYM_WRITE_FILE);
    u32 opt = snx_ld_le32(hook_host(c, option)); /* WriteOption: bit 0 flushes */
    NFile* f = file_take("WriteFile", h);
    u32 r = f->sticky;
    if (!r && !is_save(f)) r = R_READ_ONLY; /* a RomFS file is open for reading only */
    if (!r && !size) {
        if (opt & 1) {
            fs_lock();
            r = fs_file_flush(file_data(f));
            fs_unlock();
            after_change(f, r, WRITE_CLEAN);
        }
    } else if (!r) {
        if (!buffer)
            r = R_NULL_ARGUMENT;
        else if (offset < 0)
            r = R_NEGATIVE_OFFSET;
        else if ((i64)size < 0)
            r = R_NEGATIVE_SIZE;
        else if ((u64)offset > (u64)INT64_MAX - size)
            r = R_OUT_OF_RANGE;
        if (!r) {
            fs_lock();
            r = fs_file_write(file_data(f), opt, (u64)offset, buffer, size);
            fs_unlock();
            after_change(f, r, (opt & 1) ? WRITE_CLEAN : WRITE_NEEDS_FLUSH);
        }
    }
    pm_unlock(&g_nnfs.lock);
    return done("WriteFile", r);
}
u32 nnfs_flush_file(CPU* c, u64 h) {
    if (!is_native(h)) return (u32)sdk_own(c, SYM_FLUSH_FILE);
    NFile* f = file_take("FlushFile", h);
    u32 r = f->sticky;
    if (!r && is_save(f)) {
        fs_lock();
        r = fs_file_flush(file_data(f));
        fs_unlock();
        after_change(f, r, WRITE_CLEAN);
    }
    pm_unlock(&g_nnfs.lock);
    return done("FlushFile", r);
}
u32 nnfs_set_file_size(CPU* c, u64 h, i64 size) {
    if (!is_native(h)) return (u32)sdk_own(c, SYM_SET_FILE_SIZE);
    NFile* f = file_take("SetFileSize", h);
    u32 r = f->sticky;
    if (!r && size < 0) r = R_NEGATIVE_SIZE;
    if (!r && !is_save(f)) r = R_READ_ONLY;
    if (!r) {
        fs_lock();
        r = fs_file_set_size(file_data(f), size);
        fs_unlock();
        after_change(f, r, f->wstate);
    }
    pm_unlock(&g_nnfs.lock);
    return done("SetFileSize", r);
}
u32 nnfs_get_file_size(CPU* c, u64 size, u64 h) {
    if (!is_native(h)) return (u32)sdk_own(c, SYM_GET_FILE_SIZE);
    NFile* f = file_take("GetFileSize", h);
    u32 r = f->sticky;
    if (!r && !size) r = R_NULL_ARGUMENT;
    if (!r) {
        u64 n = f->size;
        if (is_save(f)) {
            fs_lock();
            n = file_data(f)->node->size;
            fs_unlock();
        }
        put_u64(c, size, n);
    }
    pm_unlock(&g_nnfs.lock);
    return done("GetFileSize", r);
}
i32 nnfs_get_file_open_mode(CPU* c, u64 h) {
    if (!is_native(h)) return (i32)sdk_own(c, SYM_GET_OPEN_MODE);
    NFile* f = file_take("GetFileOpenMode", h);
    i32 mode = (i32)f->mode;
    pm_unlock(&g_nnfs.lock);
    return mode;
}

/* the queries the game does not import: a native handle in one is a gap of this layer, said plainly */
static u32 unsupported(CPU* c, const char* symbol, const char* function, u64 h) {
    if (!is_native(h)) return (u32)sdk_own(c, symbol);
    fatal("nn::fs::%s on a native file is not implemented yet", function);
}
u32 nnfs_query_range(CPU* c, u64 info, u64 h, i64 offset, i64 size) {
    (void)info, (void)offset, (void)size;
    return unsupported(c, SYM_QUERY_RANGE, "QueryRange", h);
}
u32 nnfs_invalidate_cache(CPU* c, u64 h) { return unsupported(c, SYM_INVALIDATE_CACHE, "InvalidateCache", h); }
u32 nnfs_mount_rom_on_file(CPU* c, u64 name, u64 h, u64 cache, u64 cache_size) {
    (void)name, (void)cache, (void)cache_size;
    return unsupported(c, SYM_MOUNT_ROM_ON_FILE, "MountRomOnFile", h);
}
u32 nnfs_query_mount_rom_on_file_cache_size(CPU* c, u64 size, u64 h) {
    (void)size;
    return unsupported(c, SYM_QUERY_ROM_ON_FILE, "QueryMountRomOnFileCacheSize", h);
}
u32 nnfs_query_unprepared_range(CPU* c, u64 range, u64 h) {
    (void)range;
    return unsupported(c, SYM_UNPREPARED, "QueryUnpreparedRange", h);
}
u32 nnfs_query_unprepared_range_detail(CPU* c, u64 info, u64 h) {
    (void)info;
    return unsupported(c, SYM_UNPREPARED_DETAIL, "QueryUnpreparedRangeDetail", h);
}
u32 nnfs_query_lazy_load_completion_rate(CPU* c, u64 rate, u64 h, i32 unknown) {
    (void)rate, (void)unknown;
    return unsupported(c, SYM_LAZY_RATE, "QueryLazyLoadCompletionRate", h);
}
u32 nnfs_ready_lazy_load_file(CPU* c, u64 h, i64 offset, i64 size) {
    (void)offset, (void)size;
    return unsupported(c, SYM_LAZY_READY, "ReadyLazyLoadFileForciblyForDebug", h);
}

/* ---- directories */

u32 nnfs_open_directory(CPU* c, u64 handle, u64 path, i32 mode) {
    char p[PATH_MAX_LEN + 1];
    u32 r;
    pm_lock(&g_nnfs.lock);
    int m = path_mount(path ? gstr(c, path) : NULL, p, &r);
    if (m < 0) {
        pm_unlock(&g_nnfs.lock);
        return (u32)sdk_own(c, SYM_OPEN_DIR);
    }
    u32 um = (u32)mode;
    if (!r && !handle) r = R_NULL_ARGUMENT;
    if (!r && (!(um & 3) || (um & 0x3ffffffcu) || (um >> 30) > 2)) r = R_BAD_DIR_MODE;
    NMount* mt = &g_hle.nnfs.mounts[m];
    NDir opened = {0};
    opened.filter = um & 3;
    if (!r && mt->kind == MOUNT_ROM) {
        const NRom* rm = rom_at(mt->rom);
        int is_dir = 0;
        u32 e = 0;
        r = rom_lookup(rm, p, &is_dir, &e);
        if (!r && !is_dir) r = R_PATH_NOT_FOUND;
        if (!r) {
            opened.dir = e;
            opened.next_dir = rom_dir_field(rm, e, 2);
            opened.next_file = rom_dir_field(rm, e, 3);
        }
    } else if (!r) {
        /* the server takes the flag for no file sizes as its bit 31 */
        fs_lock();
        opened.slot = dir_slot();
        r = fs_dir_open(mount_root(m), p, (um & 3) | ((um & DIR_NO_SIZE) ? 0x80000000u : 0),
                        &g_hle.fs.dirs[opened.slot]);
        if (!r) g_hle.fs.dirs[opened.slot].obj = 1;
        fs_unlock();
    }
    if (!r) {
        int i = -1;
        for (int k = 0; k < NDIRS && i < 0; k++)
            if (!g_hle.nnfs.dirs[k].used) i = k;
        if (i < 0) fatal("nn::fs: more than %d directories open natively", NDIRS);
        NDir* d = &g_hle.nnfs.dirs[i];
        u32 gen = (d->gen + 1) & 0x3fffffffu;
        *d = opened;
        d->used = 1;
        d->gen = gen;
        d->mount = (u32)m;
        mt->opens++;
        put_handle(c, handle, handle_make(1, gen, (u32)i));
    }
    pm_unlock(&g_nnfs.lock);
    return done("OpenDirectory", r);
}

/* writes the next RomFS entry of d to the guest at out, when out is not 0 (lock held): 1, or 0 at the end. The child
   directories come first, then the files, each in the table's order */
static int rom_dir_next(CPU* c, NDir* d, u64 out) {
    const NRom* rm = rom_at(g_hle.nnfs.mounts[d->mount].rom);
    int is_dir;
    u32 e, len;
    const char* name;
    u64 size = 0;
    if ((d->filter & 1) && d->next_dir != ROM_NONE) {
        e = d->next_dir;
        d->next_dir = rom_dir_field(rm, e, 1);
        is_dir = 1;
    } else if ((d->filter & 2) && d->next_file != ROM_NONE) {
        e = d->next_file;
        d->next_file = rom_file_field(rm, e, 1);
        is_dir = 0;
        size = rom_file_u64(rm, e, 16);
    } else
        return 0;
    if (!(name = rom_name(rm, is_dir, e, &len))) return 0;
    if (out) {
        u8* p = hook_host(c, out);
        memset(p, 0, ENTRY_SIZE);
        memcpy(p, name, len < PATH_MAX_LEN ? len : PATH_MAX_LEN);
        p[0x304] = is_dir ? 0 : 1;
        snx_st_le64(p + 0x308, size);
    }
    return 1;
}

u32 nnfs_read_directory(CPU* c, u64 count, u64 entries, u64 h, i64 max) {
    if (!is_native(h)) return (u32)sdk_own(c, SYM_READ_DIR);
    NDir* d = dir_take("ReadDirectory", h);
    u32 r = 0;
    if (!count)
        r = R_NULL_ARGUMENT;
    else if (!max)
        put_u64(c, count, 0);
    else if (!entries)
        r = R_NULL_ARGUMENT;
    else if (max < 0)
        r = R_NEGATIVE_COUNT;
    else if (g_hle.nnfs.mounts[d->mount].kind == MOUNT_SAVE) {
        u64 n;
        fs_lock();
        r = fs_dir_read(dir_data(d), entries, (u64)max, &n);
        fs_unlock();
        if (!r) put_u64(c, count, n);
    } else {
        i64 n = 0;
        while (n < max && rom_dir_next(c, d, entries + (u64)n * ENTRY_SIZE)) n++;
        put_u64(c, count, (u64)n);
    }
    pm_unlock(&g_nnfs.lock);
    return done("ReadDirectory", r);
}

u32 nnfs_get_directory_entry_count(CPU* c, u64 count, u64 h) {
    if (!is_native(h)) return (u32)sdk_own(c, SYM_DIR_COUNT);
    NDir* d = dir_take("GetDirectoryEntryCount", h);
    u32 r = 0;
    if (!count)
        r = R_NULL_ARGUMENT;
    else if (g_hle.nnfs.mounts[d->mount].kind == MOUNT_SAVE) {
        fs_lock();
        u64 n = fs_dir_count(dir_data(d));
        fs_unlock();
        put_u64(c, count, n);
    } else {
        const NRom* rm = rom_at(g_hle.nnfs.mounts[d->mount].rom);
        NDir all = *d; /* a copy from the start, which the listing does not move */
        all.next_dir = rom_dir_field(rm, all.dir, 2);
        all.next_file = rom_dir_field(rm, all.dir, 3);
        u64 n = 0;
        while (rom_dir_next(c, &all, 0)) n++;
        put_u64(c, count, n);
    }
    pm_unlock(&g_nnfs.lock);
    return done("GetDirectoryEntryCount", r);
}

void nnfs_close_directory(CPU* c, u64 h) {
    if (!is_native(h)) {
        sdk_own(c, SYM_CLOSE_DIR);
        return;
    }
    NDir* d = dir_take("CloseDirectory", h);
    if (g_hle.nnfs.mounts[d->mount].kind == MOUNT_SAVE) {
        fs_lock();
        fs_dir_close(dir_data(d));
        *dir_data(d) = (DirData){0};
        fs_unlock();
    }
    g_hle.nnfs.mounts[d->mount].opens--;
    u32 gen = d->gen;
    *d = (NDir){0};
    d->gen = gen;
    pm_unlock(&g_nnfs.lock);
}

/* ---- path functions */

/* the native mount of path, with the lock taken and the normalized path in p: -1 (lock released) when it is not */
static int path_take(CPU* c, u64 path, char p[PATH_MAX_LEN + 1], u32* r) {
    pm_lock(&g_nnfs.lock);
    int m = path_mount(path ? gstr(c, path) : NULL, p, r);
    if (m < 0) pm_unlock(&g_nnfs.lock);
    return m;
}

u32 nnfs_get_entry_type(CPU* c, u64 type, u64 path) {
    char p[PATH_MAX_LEN + 1];
    u32 r;
    int m = path_take(c, path, p, &r);
    if (m < 0) return (u32)sdk_own(c, SYM_ENTRY_TYPE);
    if (!r && !type) r = R_NULL_ARGUMENT;
    if (!r && g_hle.nnfs.mounts[m].kind == MOUNT_ROM) {
        const NRom* rm = rom_at(g_hle.nnfs.mounts[m].rom);
        int is_dir = 0;
        u32 e;
        r = rom_lookup(rm, p, &is_dir, &e);
        if (!r) put_u32(c, type, is_dir ? 0 : 1);
    } else if (!r) {
        u32 t;
        fs_lock();
        r = fs_root_entry_type(mount_root(m), p, &t);
        fs_unlock();
        if (!r) put_u32(c, type, t);
    }
    pm_unlock(&g_nnfs.lock);
    return done("GetEntryType", r);
}

/* What the SDK's C library asks of a path before it opens it (open, opendir, stat; SDK 16 on). The SDK answers the
   limits from its file system's attribute, which the service gave all zero: 0x300 for the path and for an entry's
   name, and no UTF-16 limit (false, 0). A path on no native mount is the SDK's own to answer */
int nnfs_is_path_mounted(CPU* c, u64 path) {
    char p[PATH_MAX_LEN + 1];
    u32 r;
    if (path_take(c, path, p, &r) < 0) return (int)(sdk_own(c, SYM_IS_PATH_MOUNTED) & 1);
    pm_unlock(&g_nnfs.lock);
    return 1;
}
static u32 length_max(CPU* c, const char* symbol, u64 has, int utf16, u64 out, u64 path) {
    char p[PATH_MAX_LEN + 1];
    u32 r;
    if (path_take(c, path, p, &r) < 0) return (u32)sdk_own(c, symbol);
    pm_unlock(&g_nnfs.lock);
    if (utf16 && has) *hook_host(c, has) = 0;
    if (out) put_size(c, out, utf16 ? 0 : PATH_LENGTH_MAX);
    return 0;
}
u32 nnfs_get_path_length_max(CPU* c, u64 out, u64 path) { return length_max(c, SYM_PATH_MAX, 0, 0, out, path); }
u32 nnfs_get_entry_name_length_max(CPU* c, u64 out, u64 path) {
    return length_max(c, SYM_ENTRY_NAME_MAX, 0, 0, out, path);
}
u32 nnfs_get_utf16_path_length_max(CPU* c, u64 has, u64 out, u64 path) {
    return length_max(c, SYM_UTF16_PATH_MAX, has, 1, out, path);
}
u32 nnfs_get_utf16_entry_name_length_max(CPU* c, u64 has, u64 out, u64 path) {
    return length_max(c, SYM_UTF16_NAME_MAX, has, 1, out, path);
}

/* the changes of a mount's tree */
enum { CHANGE_CREATE_FILE, CHANGE_DELETE_FILE, CHANGE_CREATE_DIR, CHANGE_DELETE_DIR, CHANGE_DELETE_TREE, CHANGE_CLEAN };
static u32 change(CPU* c, const char* symbol, const char* function, u64 path, int what, i64 size) {
    char p[PATH_MAX_LEN + 1];
    u32 r;
    int m = path_take(c, path, p, &r);
    if (m < 0) return (u32)sdk_own(c, symbol);
    if (!r && what == CHANGE_CREATE_FILE && size < 0) r = R_NEGATIVE_SIZE;
    if (!r && (g_hle.nnfs.mounts[m].kind == MOUNT_ROM || g_hle.nnfs.mounts[m].read_only)) r = R_READ_ONLY;
    if (!r) {
        FsNode* root = mount_root(m);
        fs_lock();
        switch (what) {
        case CHANGE_CREATE_FILE: r = fs_root_create(root, p, size, 0); break;
        case CHANGE_DELETE_FILE: r = fs_root_remove(root, p, 0, 0, 0); break;
        case CHANGE_CREATE_DIR: r = fs_root_create(root, p, 0, 1); break;
        case CHANGE_DELETE_DIR: r = fs_root_remove(root, p, 1, 0, 0); break;
        case CHANGE_DELETE_TREE: r = fs_root_remove(root, p, 1, 1, 0); break;
        default: r = fs_root_remove(root, p, 1, 1, 1); break;
        }
        fs_unlock();
    }
    pm_unlock(&g_nnfs.lock);
    return done(function, r);
}
u32 nnfs_create_file(CPU* c, u64 path, i64 size) {
    return change(c, SYM_CREATE_FILE, "CreateFile", path, CHANGE_CREATE_FILE, size);
}
u32 nnfs_create_file_option(CPU* c, u64 path, i64 size, i32 option) {
    (void)option; /* the option makes a large file of parts on the console: the host's files have no such limit */
    return change(c, SYM_CREATE_FILE_OPT, "CreateFile", path, CHANGE_CREATE_FILE, size);
}
u32 nnfs_delete_file(CPU* c, u64 path) { return change(c, SYM_DELETE_FILE, "DeleteFile", path, CHANGE_DELETE_FILE, 0); }
u32 nnfs_create_directory(CPU* c, u64 path) {
    return change(c, SYM_CREATE_DIR, "CreateDirectory", path, CHANGE_CREATE_DIR, 0);
}
u32 nnfs_delete_directory(CPU* c, u64 path) {
    return change(c, SYM_DELETE_DIR, "DeleteDirectory", path, CHANGE_DELETE_DIR, 0);
}
u32 nnfs_delete_directory_recursively(CPU* c, u64 path) {
    return change(c, SYM_DELETE_DIR_REC, "DeleteDirectoryRecursively", path, CHANGE_DELETE_TREE, 0);
}
u32 nnfs_clean_directory_recursively(CPU* c, u64 path) {
    return change(c, SYM_CLEAN_DIR_REC, "CleanDirectoryRecursively", path, CHANGE_CLEAN, 0);
}

/* a rename: both paths on the same mount (else R_DIFFERENT_MOUNTS), which the RomFS refuses */
static u32 rename_entry(CPU* c, const char* symbol, const char* function, u64 from, u64 to, int dir) {
    char p[PATH_MAX_LEN + 1], q[PATH_MAX_LEN + 1];
    u32 r, r2;
    pm_lock(&g_nnfs.lock);
    int m = path_mount(from ? gstr(c, from) : NULL, p, &r);
    int m2 = path_mount(to ? gstr(c, to) : NULL, q, &r2);
    if (m < 0 && m2 < 0) {
        pm_unlock(&g_nnfs.lock);
        return (u32)sdk_own(c, symbol);
    }
    if (!r) r = r2;
    if (!r && m != m2) r = R_DIFFERENT_MOUNTS;
    if (!r && (g_hle.nnfs.mounts[m].kind == MOUNT_ROM || g_hle.nnfs.mounts[m].read_only)) r = R_READ_ONLY;
    if (!r) {
        fs_lock();
        r = fs_root_rename(mount_root(m), p, q, dir);
        fs_unlock();
    }
    pm_unlock(&g_nnfs.lock);
    return done(function, r);
}
u32 nnfs_rename_file(CPU* c, u64 from, u64 to) {
    return rename_entry(c, SYM_RENAME_FILE, "RenameFile", from, to, 0);
}
u32 nnfs_rename_directory(CPU* c, u64 from, u64 to) {
    return rename_entry(c, SYM_RENAME_DIR, "RenameDirectory", from, to, 1);
}

/* GetFreeSpaceSize and GetTotalSpaceSize take a bare mount name, or a path whose sub path is the root */
static u32 space(CPU* c, const char* symbol, const char* function, u64 out, u64 path, int total) {
    char p[PATH_MAX_LEN + 1];
    u32 r = 0;
    pm_lock(&g_nnfs.lock);
    const char* s = path ? gstr(c, path) : NULL;
    int m = mount_named(s);
    if (m < 0) {
        m = path_mount(s, p, &r);
        if (m >= 0 && !r && strcmp(p, "/")) r = R_NULL_ARGUMENT;
    }
    int kind = m >= 0 ? g_hle.nnfs.mounts[m].kind : MOUNT_FREE;
    pm_unlock(&g_nnfs.lock);
    if (m < 0) return (u32)sdk_own(c, symbol);
    if (!r && !out) r = R_NULL_ARGUMENT;
    /* the RomFS answers 0 for its free space. The spec does not give its total size yet: 0 too */
    if (!r) put_u64(c, out, kind == MOUNT_SAVE ? (u64)fs_root_space(total) : 0);
    return done(function, r);
}
u32 nnfs_get_free_space_size(CPU* c, u64 size, u64 path) {
    return space(c, SYM_FREE_SPACE, "GetFreeSpaceSize", size, path, 0);
}
u32 nnfs_get_total_space_size(CPU* c, u64 size, u64 path) {
    return space(c, SYM_TOTAL_SPACE, "GetTotalSpaceSize", size, path, 1);
}

/* A file's time stamps, which the SDK asks its file system for: the times it was created, modified and accessed in
   seconds since 1970, and whether they are set. A save's files have the one time the host's tree keeps, for all
   three. A RomFS keeps none: the SDK's file system answers the request as not implemented, which stops the game.
   The raw form is the service's 0x20 bytes; the other is filled from it, the three times and the flag's byte (read
   in SDK 3.5, 10.4 and 20.5), so the 7 bytes after the flag are the game's */
static u32 time_stamp(CPU* c, const char* symbol, const char* function, u64 out, u64 path, int raw) {
    char p[PATH_MAX_LEN + 1];
    u32 r;
    u64 seconds = 0;
    int m = path_take(c, path, p, &r);
    if (m < 0) return (u32)sdk_own(c, symbol);
    if (!r && !out) r = R_NULL_ARGUMENT;
    if (!r && g_hle.nnfs.mounts[m].kind == MOUNT_ROM) {
        r = R_NOT_IMPLEMENTED;
    } else if (!r) {
        fs_lock();
        r = fs_root_time_stamp(mount_root(m), p, &seconds);
        fs_unlock();
    }
    pm_unlock(&g_nnfs.lock);
    if (!r) {
        put_u64(c, out, seconds);
        put_u64(c, out + 8, seconds);
        put_u64(c, out + 16, seconds);
        *hook_host(c, out + 24) = 1;
        if (raw) memset(hook_host(c, out + 25), 0, 7);
    }
    return done(function, r);
}
u32 nnfs_get_file_time_stamp(CPU* c, u64 stamp, u64 path) {
    return time_stamp(c, SYM_TIME_STAMP, "GetFileTimeStampForDebug", stamp, path, 0);
}
u32 nnfs_get_file_time_stamp_raw(CPU* c, u64 stamp, u64 path) {
    return time_stamp(c, SYM_TIME_STAMP_RAW, "GetFileTimeStampRawForDebug", stamp, path, 1);
}

/* ---- a file's data cache

   EnableIndividualFileDataCache (SDK 6 on; read in 6.5, 7.3, 11.4, 16.2 and 20.5) has the SDK read a whole file into the
   game's buffer and answer the file's later reads from it, on a mount that takes such a cache: a RomFS or a save, and
   not the SD card (its mount registers without one). Here a read is the host's own on every native mount, so no cache
   is kept and the buffer stays as the game gave it. What remains are the SDK's checks, in its order: a buffer
   (NullptrArgument), and on a mount that takes a cache the file, opened to read (that open's Result), and room for it
   (InvalidSize, 6062). A second Enable of one file, which the SDK refuses with fs 70, is not seen: nothing is kept to
   see it. Disable ends a cache that was never made, as the SDK's does for a file it does not hold */

/* whether native mount m is the SD card's (lock held): the mount the SDK keeps no file data cache on */
static int mount_is_sd_card(int m) {
    FsNode* sd;
    if (g_hle.nnfs.mounts[m].kind != MOUNT_SAVE || fs_sd_root(&sd)) return 0;
    fs_lock();
    int is = mount_root(m) == sd;
    fs_root_release(sd);
    fs_unlock();
    return is;
}
u32 nnfs_enable_individual_file_data_cache(CPU* c, u64 path, u64 buffer, u64 size) {
    char p[PATH_MAX_LEN + 1];
    u32 r;
    int m = path_take(c, path, p, &r);
    if (m < 0) return (u32)sdk_own(c, SYM_ENABLE_FILE_CACHE);
    if (!r && !buffer) r = R_NULL_ARGUMENT;
    u64 n = 0;
    if (!r && g_hle.nnfs.mounts[m].kind == MOUNT_ROM) {
        const NRom* rm = rom_at(g_hle.nnfs.mounts[m].rom);
        int is_dir = 0;
        u32 e = 0;
        r = rom_lookup(rm, p, &is_dir, &e);
        if (!r && is_dir) r = R_PATH_NOT_FOUND;
        if (!r) n = rom_file_u64(rm, e, 16);
    } else if (!r && !mount_is_sd_card(m)) {
        FileData fd = {0};
        fs_lock();
        r = fs_file_open(mount_root(m), p, 1, &fd);
        if (!r) {
            n = fd.node->size;
            fs_file_close(&fd);
        }
        fs_unlock();
    }
    if (!r && (i64)n > (i64)size) r = R_NEGATIVE_SIZE; /* InvalidSize, which a negative size also gives */
    pm_unlock(&g_nnfs.lock);
    return done("EnableIndividualFileDataCache", r);
}
void nnfs_disable_individual_file_data_cache(CPU* c, u64 path) {
    char p[PATH_MAX_LEN + 1];
    u32 r;
    int m = path_take(c, path, p, &r);
    if (m < 0) {
        sdk_own(c, SYM_DISABLE_FILE_CACHE);
        return;
    }
    pm_unlock(&g_nnfs.lock);
    if (r) stop("DisableIndividualFileDataCache", r); /* the SDK stops on any failure of it */
}

/* a commit of native mount m (lock held): a save writes its note, and the RomFS has nothing to commit */
static u32 commit_mount(int m) {
    u32 r = 0;
    if (g_hle.nnfs.mounts[m].kind == MOUNT_SAVE) {
        const FsData* fs = &g_hle.fs.fss[g_hle.nnfs.mounts[m].fss];
        fs_lock();
        /* a journaled save's last commit is made then, which may be refused; any other root's files are its own */
        if (fs->journaled)
            r = fs_save_commit(fs->root, (int)fs->noted);
        else
            fs_root_commit(fs->root, (int)fs->noted);
        fs_unlock();
    }
    return r;
}
static u32 commit(CPU* c, const char* symbol, const char* function, u64 name) {
    pm_lock(&g_nnfs.lock);
    int m = mount_named(name ? gstr(c, name) : NULL);
    if (m < 0) {
        pm_unlock(&g_nnfs.lock);
        return (u32)sdk_own(c, symbol);
    }
    u32 r = commit_mount(m);
    pm_unlock(&g_nnfs.lock);
    return done(function, r);
}
u32 nnfs_commit(CPU* c, u64 name) { return commit(c, SYM_COMMIT, "Commit", name); }
u32 nnfs_commit_option(CPU* c, u64 name, u64 option) {
    (void)option;
    return commit(c, SYM_COMMIT_OPT, "Commit", name);
}
u32 nnfs_commit_save_data(CPU* c, u64 name) { return commit(c, SYM_COMMIT_SAVE, "CommitSaveData", name); }

/* Commit(names, count), the mounts the SDK commits together, at most COMMIT_NAMES_MAX of them: the array checked
   first, then the count, which may be 0. Each native mount commits as Commit does, in the order given; the console
   commits them in one go, and here a commit that fails comes after the ones before it are made. A name the native
   layer did not mount runs the SDK's own code for the whole call */
#define COMMIT_NAMES_MAX 10
u32 nnfs_commit_names(CPU* c, u64 names, i32 count) {
    int m[COMMIT_NAMES_MAX];
    u32 r = 0;
    if (!names) return done("Commit", R_NULL_ARGUMENT);
    if ((u32)count > COMMIT_NAMES_MAX) return done("Commit", R_COMMIT_NAME_COUNT);
    pm_lock(&g_nnfs.lock);
    for (i32 k = 0; k < count; k++) {
        u64 name = snx_ld_le64(hook_host(c, names + 8 * (u64)k));
        m[k] = mount_named(name ? gstr(c, name) : NULL);
        if (m[k] < 0) {
            pm_unlock(&g_nnfs.lock);
            return (u32)sdk_own(c, SYM_COMMIT_NAMES);
        }
    }
    for (i32 k = 0; k < count && !r; k++) r = commit_mount(m[k]);
    pm_unlock(&g_nnfs.lock);
    return done("Commit", r);
}

/* ---- save data and cache storage that the console makes through am and the file system service. The host's folders
   take what a game writes, so nothing is ever missing space: a save and a cache storage are made when they are
   mounted (fs_save_root), and these answer as the runtime's am and fs services do */

#define LIST_HANDLE (TAG_DIR | 0xfffffff0u) /* the native cache storage list: generation 0, which no directory has */

u32 nnfs_ensure_save_data(CPU* c, u64 uid) {
    (void)c, (void)uid;
    return 0;
}
u32 nnfs_extend_save_data(CPU* c, u64 uid, i64 size, i64 journal_size) {
    (void)c, (void)uid;
    am_save_data_extend(SAVE_ACCOUNT, size, journal_size);
    return 0;
}
u32 nnfs_get_save_data_size(CPU* c, u64 size, u64 journal_size, u64 uid) {
    (void)uid;
    if (!size || !journal_size) return done("GetSaveDataSize", R_NEGATIVE_COUNT); /* the SDK's null check gives 6001 */
    i64 n, j;
    am_save_data_size(SAVE_ACCOUNT, &n, &j);
    put_u64(c, size, (u64)n);
    put_u64(c, journal_size, (u64)j);
    return 0;
}
/* GetSaveDataRestoreFlag: the Restore flag (bit 4) of the flags in the extra data of the save mounted as name. The
   SDK finds the mount, then checks out (NullptrArgument), then asks the mount for its save's attribute, which only
   MountDeviceSaveData's mount carries: any other mount, an account's save and the SD card's included, answers
   PreconditionViolation (2-6000), which the SDK stops on (read in SDK 10.4 to 20.5). The runtime's saves keep no
   extra data and nothing sets the flag, so the device's save answers it clear */
u32 nnfs_get_save_data_restore_flag(CPU* c, u64 out, u64 name) {
    pm_lock(&g_nnfs.lock);
    int m = mount_named(name ? gstr(c, name) : NULL);
    u32 type = m < 0 ? 0 : g_hle.nnfs.mounts[m].save_type;
    pm_unlock(&g_nnfs.lock);
    if (m < 0) return (u32)sdk_own(c, SYM_RESTORE_FLAG);
    u32 r = !out ? R_NULL_ARGUMENT : type != SAVE_DEVICE ? R_PRECONDITION : 0;
    if (!r) *(u8*)hook_host(c, out) = 0;
    return done("GetSaveDataRestoreFlag", r);
}
u32 nnfs_create_cache_storage(CPU* c, i32 index, i64 size, i64 journal_size) {
    (void)c, (void)size, (void)journal_size;
    return done("CreateCacheStorage", index < 0 ? R_NEGATIVE_COUNT : 0);
}
u32 nnfs_delete_cache_storage(CPU* c, i32 index) {
    (void)c;
    return done("DeleteCacheStorage", index < 0 ? R_NEGATIVE_COUNT : 0);
}
u32 nnfs_get_cache_storage_size(CPU* c, u64 size, u64 journal_size, i32 index) {
    u32 r = index < 0 ? R_NEGATIVE_COUNT : !size || !journal_size ? R_NULL_ARGUMENT : 0;
    if (!r) {
        put_u64(c, size, 0); /* the file system service keeps no sizes of a cache storage */
        put_u64(c, journal_size, 0);
    }
    return done("GetCacheStorageSize", r);
}
/* the list of cache storages, which the file system service's save data reader gives none of */
u32 nnfs_open_cache_storage_list(CPU* c, u64 handle) {
    if (!handle) return done("OpenCacheStorageList", R_NULL_ARGUMENT);
    put_u64(c, handle, LIST_HANDLE);
    return 0;
}
u32 nnfs_read_cache_storage_list(CPU* c, u64 count, u64 infos, u64 h, i32 max) {
    if (h != LIST_HANDLE) return (u32)sdk_own(c, SYM_READ_CACHE_LIST);
    u32 r = max < 0 ? R_NEGATIVE_COUNT : !count || !infos ? R_NULL_ARGUMENT : 0;
    if (!r) put_u32(c, count, 0);
    return done("ReadCacheStorageList", r);
}
void nnfs_close_cache_storage_list(CPU* c, u64 h) {
    if (h != LIST_HANDLE) sdk_own(c, SYM_CLOSE_CACHE_LIST);
}

/* ---- the access log, the SD card and the host PC's file system */

/* The access log stays off: the file system service answers its global mode as 0, and the game sets no log callback.
   Answered here, the SDK never opens its fsp-srv session for the question */
int nnfs_is_enabled_access_log(CPU* c) {
    (void)c;
    return 0;
}
int nnfs_is_enabled_access_log_mask(CPU* c, u32 mask) {
    (void)c, (void)mask;
    return 0;
}

/* the SD card, the data folder's sdmc/, which is always inserted */
int nnfs_is_sd_card_inserted(CPU* c) {
    (void)c;
    return 1;
}
static u32 mount_sd_card(CPU* c, const char* function, u64 name) {
    pm_lock(&g_nnfs.lock);
    u32 r;
    const char* s = name ? gstr(c, name) : NULL;
    int m = mount_new(s, MOUNT_SAVE, 0, &r);
    if (m >= 0) {
        FsNode* root;
        r = fs_sd_root(&root);
        if (r)
            g_hle.nnfs.mounts[m] = (NMount){0};
        else {
            mount_root_set(m, root, 0, 0);
            LOG("nn::fs: %s(\"%s\") native\n", function, s);
        }
    }
    pm_unlock(&g_nnfs.lock);
    return done(function, r);
}
u32 nnfs_mount_sd_card(CPU* c, u64 name) { return mount_sd_card(c, "MountSdCard", name); }
u32 nnfs_mount_sd_card_for_debug(CPU* c, u64 name) { return mount_sd_card(c, "MountSdCardForDebug", name); }
/* a console has no host PC: the file system service refuses it, as the runtime's does */
u32 nnfs_mount_host_root(CPU* c) {
    (void)c;
    return done("MountHostRoot", fs_host_root_result());
}
u32 nnfs_mount_host_root_option(CPU* c, u64 option) {
    (void)c, (void)option;
    return done("MountHostRoot", fs_host_root_result());
}
/* a folder of the host PC under a mount name: the same service, and the same refusal */
u32 nnfs_mount_host(CPU* c, u64 name, u64 path) {
    (void)c, (void)name, (void)path;
    return done("MountHost", fs_host_root_result());
}
u32 nnfs_mount_host_option(CPU* c, u64 name, u64 path, u64 option) {
    (void)c, (void)name, (void)path, (void)option;
    return done("MountHost", fs_host_root_result());
}
