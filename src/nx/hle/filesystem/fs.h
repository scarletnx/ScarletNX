/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's files as the runtime itself reads them (the loader, shared fonts, the control data, the shader programs
   the game ships for gpu/shipped_pass.c), beside the fsp-srv service in fs.c */
#ifndef SNX_FS_H
#define SNX_FS_H
#include "nx/runtime/state_fns.h"
#include "common/cpu/aarch64/cpu.h"
#include "nx/runtime/platform/plat.h" /* FILE, PFileMap */
#include "nx/hle/filesystem/fs_tree.h" /* FsTree, FsNode */
#include "nx/runtime/driver/container.h"

/* Indexes the game's data folder and maps RomFS and ExeFS for the run. Called once, before any guest file is loaded */
void fs_init(const char* keys, const char* title_keys);
extern u64 g_fs_bytes_read, g_fs_bytes_written; /* the game's file I/O, for the rate plot */
/* Lookups by a path in the index. *size is 0 when the file isn't there */
FILE* fs_open_read(const char* path, u64* size);
/* save states (runtime/state.c): the open file systems, files and folders by path into the state, and what is open
   let go of before a load writes g_hle over it (fs_state_resolve, below, finds the state's again) */
struct StateIo;
int fs_state_save(struct StateIo* io);
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
u8* fs_read_file(const char* path, u64* size);    /* malloc'd, or NULL */
/* a file of the game's HtmlDocument content (the pages its web applet shows), by its path under html-document/,
   malloc'd: NULL when the game has no dump open, or the dump no such content or file */
u8* fs_html_document(const char* path, u64* size);
/* a file that exists only in memory, which the lookups find like any other. 0, or -1 */
int fs_add_virtual_file(const char* path, const void* data, u64 size);
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
/* The service objects' data, each in a pool (ipc.c's lock guards them, svcobj_new_data). obj is the object's pool
   index + 1, which ipc.c writes */
/* an IFileSystem's root folder, retained */
typedef struct {
    u32 obj;
    FsNode* root;
} FsData;
/* an IFile, or a system archive's IStorage (the RomFS's has no data). f is NULL for a node held in memory */
typedef struct {
    u32 obj;
    FILE* f;
    FsNode* node;
    u32 mode; /* OpenMode: 1 read, 2 write, 4 allow append */
} FileData;
/* an IDirectory: the entries filter picks (1 folders, 2 files), after the name last listed, until done */
typedef struct {
    u32 obj;
    FsNode* node;
    u32 filter;
    char last[FS_TREE_PATH_MAX]; /* a name, not a node: the entry may be deleted or renamed meanwhile */
    int done;
} DirData;
#define FS_FS_MAX   64 /* IFileSystems: one per mount (save data, the SD card, caches, BCAT), a few dozen at most */
#define FS_FILE_MAX 1024 /* IFiles and IStorages: each is an object of the fsp-srv domain, which holds DOMAIN_OBJECTS */
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
    u8 emptyrom[0x200]; /* the empty RomFS of the system archives (emptyrom_init) */
    u8 ngword2rom[0x2000]; /* the synthesized NgWord2 archive 0100000000000823 (ngword2rom_init) */
    u64 ngword2rom_size;   /* the bytes used in ngword2rom */
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
/* fs's host side: locks and condition variables, which a save state never holds (hle.h's g_host) */
typedef struct {
    PMutex dlock;
} HostFs;
/* all zero: fs_init sets the tree up */
extern const StateFnTable g_fs_state_fns; /* the functions a save state may hold of fs.c */
/* a load's: the state's open file systems, files and folders found by path in this process's tree and opened, files as
   the loaded fs (loaded, the state's g_hle.fs) had them, before anything of the running game changes (NULL, with
   why, when one isn't there any more), then put in place in g_hle once it is the state's, or let go of when the load
   is refused */
typedef struct FsStaged FsStaged;
FsStaged* fs_state_resolve(struct StateIo* io, const HleFs* loaded, char* why, size_t whysize);
void fs_state_install(FsStaged* st);
void fs_state_staged_free(FsStaged* st);

#endif
