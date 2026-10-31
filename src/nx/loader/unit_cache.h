/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The code units' cache (module_llvm.c's units, and module_arm.c's of a 32-bit game): each unit's object kept under a
   key, the SHA-256 of the unit's LLVM IR and of everything else its object is made with, in a folder every game
   shares. A unit whose key is there, whole, is not compiled again; the module descriptor and the link always are. C11
   and the LLVM-C headers, as module_llvm.c: not for the C99 driver */
#ifndef SNX_NX_UNIT_CACHE_H
#define SNX_NX_UNIT_CACHE_H

#include "common/cpu/recompiler/llvm/ir.h"
#include <stdio.h>

/* What every unit's key holds besides the unit's own target machine and IR (and SNX_IR_OBJECT_VERSION) */
typedef struct {
    const char* llvm;   /* the LLVM that compiles: snx_unit_cache_llvm's, another one in tests */
    char opt;           /* the -O letter */
    const char* tuning; /* SnxIrTuning.text, NULL for LLVM's own settings */
} SnxUnitIdentity;

typedef struct SnxUnitCache SnxUnitCache;
/* The room on the disk a folder is on, as snx_disk_room reads it: its bytes free and its size. 0, or -1 with errno (or
   Windows' last error) saying why */
typedef int (*SnxUnitDisk)(const char* folder, SnxU64* free_bytes, SnxU64* total);

/* A kept object's last bytes: "SNXUNIT1", then the SHA-256 of its key's 64 digits followed by the object. A file
   without them is not the whole object its name says */
#define SNX_UNIT_TRAILER 40

/* What one trim did (snx_unit_cache_trim) */
typedef struct {
    size_t files, removed, failed; /* the cache's files, those removed, and those it could not remove */
    SnxU64 held, removed_bytes;    /* their bytes before, and the bytes removed */
} SnxUnitTrim;

/* This process's LLVM as text: the version its headers name, the one its library says, and its revision when the
   headers have one */
void snx_unit_cache_llvm(char* out, size_t size);
/* The cache in folder, made when it is not there; NULL with why in error */
SnxUnitCache* snx_unit_cache_open(const char* folder, const SnxUnitIdentity* identity, char* error, size_t error_size);
void snx_unit_cache_close(SnxUnitCache* cache);
/* The disk's room read through disk from now on, snx_disk_room until then: a test's own */
void snx_unit_cache_disk(SnxUnitCache* cache, SnxUnitDisk disk);
const char* snx_unit_cache_folder(const SnxUnitCache* cache);
/* A unit's key into key, 64 hex digits and a NUL: the SHA-256 of the identity, the triple, CPU and features of the target
   machine that compiles it, and its IR's bitcode. 0, or -1 when OpenSSL cannot hash */
int snx_unit_cache_key(const SnxUnitCache* cache, const char* triple, const char* cpu, const char* features,
                       const void* bitcode, size_t size, char key[65]);
/* object, size bytes, kept under key (a file written beside it, then renamed over the name): 0, or -1 with why in
   error */
int snx_unit_cache_keep(const SnxUnitCache* cache, const char* key, const unsigned char* object, size_t size,
                        char* error, size_t error_size);
/* snx_ir_object through the cache: the object kept under the unit's key when it is there whole (*reused 1, the file's
   time renewed), else the one snx_ir_object makes, then kept. A kept object that is not whole, or cannot be read, is
   said on stderr and made again; one that cannot be kept is said, and the unit has it all the same. The cache's disk
   always keeps a tenth of itself free: the cache's oldest objects are removed to make room for a new one, and one
   there is no room for is not cached. Each reason a unit is not cached is said once, for the first unit it stops; the counts (units
   not cached, objects removed for room, reused ones whose time could not be renewed) at snx_unit_cache_close. Units
   of one cache may run on several threads. key, when not NULL, gets the unit's key when its object is whole in the
   cache on return, else an empty string; the cache then holds that object until snx_unit_cache_close, and makes no
   room by removing it, so that snx_unit_cache_read has it again. 0, or -1 with why in error (snx_ir_object's) */
int snx_unit_cache_object(SnxUnitCache* cache, SnxIr* ir, unsigned char** data, size_t* size, int* reused, char* key,
                          char* error, size_t error_size);
/* The object kept under key, malloc'd, its size in *size; NULL with why in error: the file and the system's reason
   when it cannot be read, or why it is not whole */
unsigned char* snx_unit_cache_read(const SnxUnitCache* cache, const char* key, size_t* size, char* error,
                                   size_t error_size);
/* The cache's files in folder (its .o, and the .tmp a write left) held to limit bytes, the oldest by their time (the
   last write or reuse) removed first, then by name; 0 for no limit. Each file it cannot remove is said on stderr. 0, or
   -1 with why in error when the folder cannot be listed */
int snx_unit_cache_trim(const char* folder, SnxU64 limit, SnxUnitTrim* trim, char* error, size_t error_size);
/* A module build's last word on its units, on out (stdout): how many of count the cache in folder gave (reused, bytes
   of them), unless quiet, then the folder held to limit bytes (0: none), what that removed said */
void snx_unit_cache_report(FILE* out, const char* folder, SnxU64 limit, size_t reused, size_t count, SnxU64 bytes,
                           int quiet);

#endif
