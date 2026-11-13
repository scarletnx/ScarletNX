/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NX_NN_FS_H
#define SNX_NX_NN_FS_H
#include "common/cpu/aarch64/cpu.h"

/* The native nn::fs's state that the game sees (g_hle.nnfs, which a save state holds): its mounts, open files and
   open directories, by index. A save's root, file or directory is a slot of g_hle.fs (fss, files, dirs), which fs.c's
   save state finds again by path. A RomFS image's tables are the host's (fs.c), read again after a load */
#define NNFS_MOUNTS 32
#define NNFS_FILES  1024
#define NNFS_DIRS   256
#define NNFS_NAME   15

enum { NNFS_MOUNT_FREE, NNFS_MOUNT_ROM, NNFS_MOUNT_SAVE };
typedef struct {
    u32 used;
    u32 game;    /* the game's RomFS, else a data storage's */
    u64 data_id; /* that storage's data id (an add-on content's) */
} NnfsRom;
typedef struct {
    u32 kind;
    char name[NNFS_NAME + 1];
    u32 opens;     /* files and directories open on it */
    u32 rom;       /* a RomFS mount's image in roms */
    u32 fss;       /* a save's root: its slot in g_hle.fs.fss */
    u32 read_only; /* MountSaveDataReadOnly's: no write mode and no change of its tree */
    u32 save_type; /* a save's SaveDataAttribute type (1 account, 3 device, 4 temporary, 5 cache); 0 for the SD card */
} NnfsMount;
typedef struct {
    u32 used;
    u32 gen; /* in the handle, so a closed handle is refused */
    u32 mount;
    u32 mode;   /* the OpenMode: 1 read, 2 write, 4 allow append */
    u32 rom;    /* a RomFS file: its image in roms */
    u32 slot;   /* a save's file: its slot in g_hle.fs.files */
    u64 data;   /* a RomFS file: the image's offset of its data */
    u64 size;   /* and its size */
    u32 wstate; /* the write state, as the SDK's file accessor keeps it (fs.c's WRITE_*) */
    u32 sticky; /* a failed write's Result, which every later operation on the file returns */
} NnfsFile;
typedef struct {
    u32 used;
    u32 gen;
    u32 mount;
    u32 filter;    /* 1 directories, 2 files */
    u32 dir;       /* a RomFS directory: its entry in the directory table */
    u32 next_dir;  /* the next child directory to list, or none */
    u32 next_file; /* then the next file */
    u32 slot;      /* a save's directory: its slot in g_hle.fs.dirs */
} NnfsDir;
typedef struct {
    NnfsRom roms[NNFS_MOUNTS];
    NnfsMount mounts[NNFS_MOUNTS];
    NnfsFile files[NNFS_FILES];
    NnfsDir dirs[NNFS_DIRS];
} HleNnfs;

/* the native nn::fs (fs.hook's nn_fs family), before hook_install */
void nnfs_init(void);
#endif
