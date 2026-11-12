/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's files as the runtime itself reads them (the loader, shared fonts, the control data, the shader programs
   the game ships for gpu/shipped_pass.c), beside the file system core in fs.c that the native nn::fs calls */
#ifndef SNX_FS_H
#define SNX_FS_H
#include "nx/runtime/state_fns.h"
#include "common/cpu/aarch64/cpu.h"
#include "nx/runtime/platform/plat.h" /* FILE, PFileMap */
#include "nx/hle/filesystem/fs_tree.h" /* FsTree, FsNode */
#include "nx/runtime/driver/container.h"

/* Indexes the game's data folder and maps RomFS and ExeFS for the run. Called once, before any guest file is loaded */
void fs_init(const char* keys, const char* title_keys);
/* save_layout=global's saves under root (the data folder's save/), this title's folders there now grafted: after the
   users are known, before the game runs (fs.c's save_global) */
void fs_saves_init(const char* root);
/* the folders of uid's saves this run reaches, out of the index for acc's DeleteUser to move to the Trash: the data
   folder's save/user.<uid>/ (save_layout=global's, every title's) and this game's own save/user.<uid> (game's). Their
   host paths into paths, max of them; how many */
int fs_user_saves_take(const u64 uid[2], char (*paths)[FS_TREE_PATH_MAX], int max);
/* the line said when a save opened this run was last written by a newer version of the program (fs.c's save_note), for
   a crash report to say again: nothing when none was. Takes no lock, as a crash's handler may run while one is held */
void fs_save_report(void);
extern u64 g_fs_bytes_read, g_fs_bytes_written; /* the game's file I/O, for the rate plot */
/* Lookups by a path in the index. *size is 0 when the file isn't there */
FILE* fs_open_read(const char* path, u64* size);
/* save states (runtime/state.c): the open file systems, files and folders by path into the state, and what is open
   let go of before a load writes g_hle over it (fs_state_resolve, below, finds the state's again) */
struct StateIo;
int fs_state_save(struct StateIo* io);
/* and the application's temporary storage, save/temp, whole: every launch empties it (fs_init) */
int fs_state_temp_save(struct StateIo* io);
void fs_state_drop(void);
int fs_map_file(const char* path, PFileMap* map); /* 0, or -1 */
/* the mods' patches the program was recompiled with (HleFs' patches), which the loader writes over the modules they
   are for: their count, and their paths in *paths */
size_t fs_patches(char* const** paths);
/* 1 when id is a DLC read in place from its dump (one the game's addons= checks), which OpenDataStorageByDataId serves
   as installed */
int fs_aoc_has(u64 id);
/* the checked DLCs the game plays with, those found extracted or read from their dumps, in addons= order, into *ids:
   their count */
size_t fs_dlcs(const u64** ids);
/* the programs of this program's application, as its dump read in place says them: -1 when no dump says */
int fs_program_count(void);
/* a game's read of [off, off + sz) of a storage (a RomFS image), once it is done: one that goes on where the thread's
   last read of that storage ended waits a console's time for its bytes (fs.c's fs_pace_ns) */
void fs_pace_read(const void* storage, u64 off, u64 sz);
u8* fs_read_file(const char* path, u64* size);    /* malloc'd, or NULL */
/* a file of the game's HtmlDocument content (the pages its web applet shows), by its path under html-document/,
   malloc'd: NULL when the game has no dump open, or the dump no such content or file */
u8* fs_html_document(const char* path, u64* size);
/* a file that exists only in memory, which the lookups find like any other. 0, or -1 */
int fs_add_virtual_file(const char* path, const void* data, u64 size);
/* a file the system writes on the game's SD card (caps' album), through the index as the game's own are: its folder
   dir, made with those above it, and in it name, made new holding data. FS_TREE_OK; FS_TREE_EXISTS when name is there
   already; another FsTreeResult, with the host's reason in errno (snx_os_error), when it could not be written */
int fs_sd_write_new(const char* dir, const char* name, const void* data, u64 size);
/* the RomFS the game opens: romfs.bin as it is, or an image built from the romfs folder */
void romfs_init(void);
/* What the runtime reads of the RomFS for itself (the shader programs the game ships, gpu/shipped_pass.c), from any
   thread once romfs_init ran (main.c calls it before video_init, which starts the renderer's thread that reads).
   romfs_files hands each entry of the image's file table to each, its name in its folder (the table has no paths),
   its data's offset in the image and its size, while each returns 0: 0 for all of them (none without an image), 1
   when each stopped, -1 when the table couldn't be read (a later call may), -2 when it is malformed, as it will be
   on every call: a header or table past the image or past any game's size (nothing listed), an entry whose data
   lies past the image (left out), or one whose name runs past the table (it and all after it left out). romfs_peek
   reads the image as the game would, a piece at a time so the game's own reads wait for one piece at most, and
   without logging or warning of the files it comes from: 0, or -1 past its end or when part of it couldn't be read */
typedef int (*RomfsEach)(void* ctx, const char* name, u64 off, u64 size);
int romfs_files(RomfsEach each, void* ctx);
int romfs_peek(u64 off, void* dst, u64 size);
/* size bytes of the RomFS image at off into guest memory at addr, as the game's own reads take them (logged, its
   files opened as reached): 0, or -1 when the range is past the image, the buffer isn't writable guest memory, or part
   couldn't be read from the host (that part zeroed) */
int romfs_read_guest(u64 off, u64 addr, u64 size);
/* ---- HLE state in g_hle (hle.h) */
/* RomFS. With no romfs.bin, the image is made up from the romfs/ folder: its header and tables in memory (rmeta), the
   files' data read from the files themselves when the game reads that part of the image. With mods' romfs/ folders
   played, it is made up the same way from the game's RomFS with theirs laid over it (rlayered), a file of the game's
   image read from that image */
typedef struct {
    FsNode* node; /* retained for the run: the image keeps its files. NULL for a file of the image under the mods */
    u64 base;     /* where that one's data is in that image */
    u64 size;
    u64 off;    /* where its data starts in the image */
    int logged; /* the game's first read of it was logged, and its first failure warned (atomics: game threads read
                   at once). romfs_peek's reads don't count */
    int warned;
} RFile;
/* unmapped RomFS files open on the host, least recently used closed first */
#define NOPEN 48
typedef struct {
    FsNode* node;
    FILE* f;
    u64 used; /* the clock when it was last read */
} ROpen;
/* An open root folder, file and directory: slots of g_hle.fs (fss, files, dirs), which the native nn::fs takes and a
   save state finds again by path. obj is not 0 in a slot that is taken */
/* a root folder, retained */
typedef struct {
    u32 obj;
    FsNode* root;
    u32 noted; /* 1 for an Account, Device or BCAT save, whose commits write its note (save_note) */
    /* 1 for a save kept as the console keeps one in a folder: root is the copy the game works in, 1 in the save's own
       folder, beside its last commit, 0 (fs.c's "A save's commit") */
    u32 journaled;
} FsData;
/* an open file. f is NULL for a node held in memory */
typedef struct {
    u32 obj;
    FILE* f;
    FsNode* node;
    u32 mode; /* OpenMode: 1 read, 2 write, 4 allow append */
} FileData;
/* an open directory: the entries filter picks (1 folders, 2 files), after the name last listed, until done */
typedef struct {
    u32 obj;
    FsNode* node;
    u32 filter;
    char last[FS_TREE_PATH_MAX]; /* a name, not a node: the entry may be deleted or renamed meanwhile */
    int done;
} DirData;
/* The file system server's core, which the native nn::fs (nn/fs.c) calls: the operations on a root folder, an
   open file and an open directory, with the arguments as values and the console's Results. Each takes tree.lock held
   (fs_lock) */
void fs_lock(void);
void fs_unlock(void);
/* the root of the save of type (1 an account's, 4 temporary, 5 cache) for uid and index, made when it is not there,
   retained: 0, or the Result. noted: an Account, Device or BCAT save, whose commits write its note. journaled: a save
   kept as the console keeps one in a folder, whose root is the copy the game works in, committed by fs_save_commit;
   any other root's commit is fs_root_commit's. Takes the lock */
u32 fs_save_root(u8 type, const u8 uid[16], u16 index, FsNode** root, int* noted, int* journaled);
/* a journaled save's commit: its working folder work made its last commit, refused while a file of it is open for
   writing (WriteModeFileNotClosed), then its note when noted. 0, or the Result */
u32 fs_save_commit(FsNode* work, int noted);
/* the SD card's root (the data folder's sdmc/), made when it is not there, retained. The card is always there.
   Takes the lock */
u32 fs_sd_root(FsNode** root);
/* what opening the host's file system answers: a console has no host PC to reach */
u32 fs_host_root_result(void);
void fs_root_release(FsNode* root);
u32 fs_root_create(FsNode* root, const char* path, i64 size, int dir);
u32 fs_root_remove(FsNode* root, const char* path, int dir, int recursive, int clean);
u32 fs_root_rename(FsNode* root, const char* from, const char* to, int dir);
u32 fs_root_entry_type(FsNode* root, const char* path, u32* type); /* 0 a directory, 1 a file */
/* when the entry at path was last modified, in seconds since 1970: the one time the host's tree keeps */
u32 fs_root_time_stamp(FsNode* root, const char* path, u64* seconds);
void fs_root_commit(FsNode* root, int noted);
/* the free (total 0) or total (1) space that a save reports, the same for every save */
i64 fs_root_space(int total);
/* a file opened in mode (1 read, 2 write, 4 allow append) into out, its node retained */
u32 fs_file_open(FsNode* root, const char* path, u32 mode, FileData* out);
void fs_file_close(FileData* f);
/* size bytes at off into guest memory at addr, clamped to the end of the file: *n the bytes read */
u32 fs_file_read(FileData* f, u64 off, u64 addr, u64 size, u64* n);
/* size bytes of guest memory at addr to off. option bit 0 flushes */
u32 fs_file_write(FileData* f, u32 option, u64 off, u64 addr, u64 size);
u32 fs_file_flush(FileData* f);
u32 fs_file_set_size(FileData* f, i64 size);
u32 fs_dir_open(FsNode* root, const char* path, u32 mode, DirData* out);
void fs_dir_close(DirData* d);
/* up to max 0x310-byte DirectoryEntry records into guest memory at addr, after the last one listed: *count of them */
u32 fs_dir_read(DirData* d, u64 addr, u64 max, u64* count);
u64 fs_dir_count(const DirData* d);
/* A data storage as OpenDataStorageByDataId opens it: a DLC read in place from its dump (aoc, its index), or the file
   of a system archive or add-on content in the data folder, or the one the runtime makes (file, aoc -1). Each call
   takes the locks it needs */
typedef struct {
    int aoc;
    FileData file;
} FsStorage;
u32 fs_storage_open(u64 data_id, FsStorage* out);
void fs_storage_close(FsStorage* st);
u64 fs_storage_size(const FsStorage* st);
/* size bytes at off into host memory (dst) or guest memory (addr): 0, or -1 past the end or when the host read fails */
int fs_storage_read(FsStorage* st, u64 off, void* dst, u64 size);
int fs_storage_read_guest(FsStorage* st, u64 off, u64 addr, u64 size);
#define FS_FS_MAX   64 /* IFileSystems: one per mount (save data, the SD card, caches, BCAT), a few dozen at most */
#define FS_FILE_MAX 1024 /* the save files the native nn::fs has open */
#define FS_DIR_MAX  128 /* IDirectories, 4 KB each: a game lists a few folders at a time, and closes each after */
/* a DLC read in place: its dump, mapped, and its Data NCA's RomFS image */
typedef struct {
    SnxDump dump;
    PFileMap map;
    SnxHacFs* fs;
} FsAoc;
typedef struct {
    FsTree tree;
    RFile* rfiles; /* in offset order: romfs_build lays the files out in this order */
    int nrfiles;
    u8* rmeta;
    u64 rmeta_size; /* header + tables sit at the start of the image */
    u64 rdata_off, rtotal;
    FILE* rraw;     /* plain romfs.bin, when it couldn't be mapped */
    FsNode* rimage; /* plain romfs.bin, mapped */
    ROpen openf[NOPEN]; /* rf_open's files, under tree.lock */
    u64 openclk;
    u8 emptyrom[0x200];       /* the empty RomFS of the system archives (emptyrom_init) */
    u8 ngword2rom[0x2000];    /* the synthesized NgWord2 archive 0100000000000823 (filesrom_init) */
    u64 ngword2rom_size;      /* the bytes used in ngword2rom */
    u8 miimodelrom[0x600000]; /* the generated MiiModel archive 0100000000000802 (filesrom_init, mii_model.c), made
                                 at its first open: untouched memory until then */
    u64 miimodelrom_size;     /* the bytes used in miimodelrom, 0 before it is made */
    /* the service objects' data */
    FsData fss[FS_FS_MAX];
    FileData files[FS_FILE_MAX];
    DirData dirs[FS_DIR_MAX];
    u32 fs_next, file_next, dir_next;
    /* Extract dump off: the dump the game's folder links to, mapped for the whole run, and its program's
       ExeFS and RomFS read out of it (dump_open). dlock keeps one read at a time: their decryption is shared */
    SnxDump dump;
    SnxHacFs* dfs;
    PFileMap dmap;
    SnxHacFs* hfs;  /* its HtmlDocument content, read under dlock too (fs_html_document) */
    int html_tried; /* hfs looked for */
    /* the update's dump read in place (dump_open), read with the game's in dump; and the checked DLCs read from their
       dumps (dlcs_open), naocs of them, each in a hactool of its own (dlock keeps one read at a time) */
    PFileMap amap;
    FsAoc* aocs;
    size_t naocs;
    u64* dlcs; /* the checked DLCs the game plays with, extracted or read in place, ndlcs of them (fs_dlcs) */
    size_t ndlcs;
    /* the mods played (snx_mods_played): the files of their exefs/ the program was recompiled with, read in place of
       the game's (mexefs_names, mexefs_paths, nmexefs of them), and how many romfs/ folders are grafted as mods.<i> */
    char **mexefs_names, **mexefs_paths;
    size_t nmexefs, nmromfs;
    /* and their patches (common/utilities/patches.h) the program was recompiled with, which the loader writes over the
       modules they are made for, npatches of them */
    char** patches;
    size_t npatches;
    int rlayered; /* the RomFS image is made up with the mods' over the game's (romfs_init) */
} HleFs;
/* fs's host side, which a save state never holds (hle.h's g_host): locks, and what the RomFS folder's files may still
   map as the game first reads them (fs_tree_map, tree.lock) */
typedef struct {
    PMutex dlock;
    FsShare romfs;
    char save_root[FS_TREE_PATH_MAX]; /* save_layout=global's folder, the data folder's save/ (fs_saves_init) */
    /* the title ID the saves are kept by (fs_saves_init): the program's own, a collection's other program its
       application's */
    u64 save_title;
    unsigned save_told;               /* the save types whose folder of the other layout was said, a bit each */
    unsigned save_flat_told;          /* and those whose save an older version kept, played where it is, was */
    unsigned save_stray_told; /* and those whose folder holds more than the save, which the game does not read */
    char save_newer[FS_TREE_PATH_MAX + 256]; /* the last save opened that a newer version wrote, said (save_note) */
    int pace_told;                           /* the first paced read was said (fs.c's fs_pace_ns) */
} HostFs;
/* fs's thread state (hle.h's t_hle.fs): where this thread's last storage read ended, which the next one continues as
   a stream when it starts there (fs.c's fs_pace_ns) */
typedef struct {
    const void* storage; /* the storage it read, NULL for none yet */
    u64 next;            /* the offset just past it */
} TlsFs;
/* all zero: fs_init sets the tree up */
/* a load's: the state's open file systems, files and folders found by path in this process's tree and opened, files as
   the loaded fs (loaded, the state's g_hle.fs) had them, before anything of the running game changes (NULL, with
   why, when one isn't there any more), then put in place in g_hle once it is the state's, or let go of when the load
   is refused. With temp, the state's save/temp (fs_state_temp_save) is read too: the ones it had open under save/temp
   are checked against it, and once installed it takes the running game's place and they are opened there. io NULL:
   the state had nothing open */
typedef struct FsStaged FsStaged;
FsStaged* fs_state_resolve(struct StateIo* io, struct StateIo* temp, const HleFs* loaded, char* why, size_t whysize);
void fs_state_install(FsStaged* st);
void fs_state_staged_free(FsStaged* st);

#endif
