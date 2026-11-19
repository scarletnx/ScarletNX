/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The RomFS image's layout (Switchbrew's "RomFS"; the hash and bucket count as Eden's fsmitm_romfsbuild): a header of
   ten u64s, then the directory hash table, the directory table, the file hash table and the file table, padded to
   0x200, then the file data. Table entries refer to each other by their offset in their table. What the runtime's
   image of romfs/ (nx/hle/filesystem/fs.c) and the packer's fabricated NCAs (nx/runtime/driver/fabricate.c) share */
#ifndef SNX_ROMFS_LAYOUT_H
#define SNX_ROMFS_LAYOUT_H

#include <stdint.h>
#include <string.h>

#define SNX_ROMFS_NONE   0xFFFFFFFFu
#define SNX_ROMFS_HEADER 0x50 /* ten u64s */
/* parent, sibling, first child, first file, next in bucket, name length; the name */
#define SNX_ROMFS_DIR_ENTRY 0x18
/* parent, sibling, u64 data offset, u64 size, next in bucket, name length; the name */
#define SNX_ROMFS_FILE_ENTRY 0x20

/* the bucket of an entry named name in the directory at table offset parent. The name's bytes count unsigned. The
   format fixes both the seed (the parent's offset xor 123456789) and the step (rotate right by 5, then xor in the
   next byte): the game's own tables were built with it */
static inline uint32_t snx_romfs_hash(uint32_t parent, const char* name) {
    uint32_t h = parent ^ 123456789u;
    for (const unsigned char* p = (const unsigned char*)name; *p; p++) h = ((h >> 5) | (h << 27)) ^ *p;
    return h;
}
/* the buckets of a hash table for n entries */
static inline uint32_t snx_romfs_buckets(uint32_t n) {
    uint32_t c = n;
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    while (c % 2 == 0 || c % 3 == 0 || c % 5 == 0 || c % 7 == 0 || c % 11 == 0 || c % 13 == 0 || c % 17 == 0) c++;
    return c;
}
/* a table entry's bytes: its fixed part, then its name padded to 4 */
static inline uint64_t snx_romfs_entry_size(uint32_t fixed, const char* name) {
    return fixed + ((strlen(name) + 3) & ~(size_t)3);
}

#endif
