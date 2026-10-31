/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/runtime/internal.h"
#include "3ds/ipc/ctr_ipc.h"
#include "common/utilities/common.h"
#include "snx_settings.h"
#include "support/3ds_guest.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__) /* Darwin defines no __unix__ */
#include <sys/resource.h>
#include <sys/stat.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#endif
static bool no_mmio(void* context, uint32_t address, const void* bytes, size_t size) {
    (void)context;
    (void)address;
    (void)bytes;
    (void)size;
    assert(0 && "FS buffers must never invoke MMIO");
    return false;
}
static void archives(SnxCtrProcess* p, uint32_t fs, bool absent) {
    uint32_t a[] = {0x80c00c2, 3, 1, 1, 0x4002, 0x300020};
    unsigned before = ctr_object_count(p);
    uint64_t serial = p->archive_sequence;
    a[5] = 0x304000;
    ctr_call(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0xe0e01bf5);
    assert(p->archive_sequence == serial && ctr_object_count(p) == before);
    a[5] = 0x300020;
    ctr_call(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
    if (absent) {
        assert(ctr_command_word(p, 1) == 0xc8804464 && !p->archive_sequence);
        return;
    }
    assert(ctr_command_word(p, 0) == 0x80c00c0 && !ctr_command_word(p, 1));
    uint64_t token = (uint64_t)ctr_command_word(p, 2) | (uint64_t)ctr_command_word(p, 3) << 32;
    assert(token && ctr_object_count(p) == before);
    uint32_t open[] = {0x80201c2, 0, (uint32_t)token, (uint32_t)(token >> 32), 2, 12, 1, 0, 0x30002, 0x300000};
    ctr_call(p, fs, open, 10, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x8020042 && !ctr_command_word(p, 1) && ctr_command_word(p, 2) == 0x10);
    uint32_t file = ctr_command_word(p, 3);
    uint32_t close[] = {0x80e0080, (uint32_t)token, (uint32_t)(token >> 32)};
    ctr_call(p, fs, close, 3, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    ctr_call(p, fs, close, 3, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 1) == 0xc8804465);
    ctr_call(p, fs, open, 10, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 1) == 0xc8804465);
    uint32_t size[] = {0x8040000};
    ctr_call(p, file, size, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && ctr_command_word(p, 2));
    uint32_t finish[] = {0x8080000};
    ctr_call(p, file, finish, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && !snx_ctr_handle_close(p, file));
    uint64_t tokens[CTR_SELF_ARCHIVES];
    for (unsigned i = 0; i < CTR_SELF_ARCHIVES; ++i) {
        ctr_call(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
        assert(!ctr_command_word(p, 1));
        tokens[i] = (uint64_t)ctr_command_word(p, 2) | (uint64_t)ctr_command_word(p, 3) << 32;
        assert(tokens[i] > token);
    }
    serial = p->archive_sequence;
    ctr_call(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 1) == 0xd0401834 && p->archive_sequence == serial && ctr_object_count(p) == before);
    for (unsigned i = 0; i < CTR_SELF_ARCHIVES; ++i) {
        close[1] = (uint32_t)tokens[i];
        close[2] = (uint32_t)(tokens[i] >> 32);
        ctr_call(p, fs, close, 3, SNX_CTR_SVC_RETURN, 0);
        assert(!ctr_command_word(p, 1));
    }
    p->archive_sequence = UINT32_MAX;
    ctr_call(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && !ctr_command_word(p, 2) && ctr_command_word(p, 3) == 1);
    open[2] = 0;
    open[3] = 1;
    ctr_call(p, fs, open, 10, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    file = ctr_command_word(p, 3);
    close[1] = 0;
    close[2] = 1;
    ctr_call(p, fs, close, 3, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    ctr_call(p, file, finish, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && !snx_ctr_handle_close(p, file));
    serial = p->archive_sequence;
    p->archive_sequence = UINT64_MAX;
    ctr_call(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 1) == 0xd0401834 && p->archive_sequence == UINT64_MAX);
    for (unsigned i = 0; i < CTR_SELF_ARCHIVES; ++i) assert(!p->self_archives[i].token);
    p->archive_sequence = serial;
}
/* The backing file's real host modification time, computed independently here
   from the host stat/FILETIME and compared with both the ControlArchive wire
   reply and the RomFS API result. */
static uint64_t host_datetime(const char* path) {
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA data;
    assert(GetFileAttributesExA(path, GetFileExInfoStandard, &data));
    ULARGE_INTEGER time = {.LowPart = data.ftLastWriteTime.dwLowDateTime,
                           .HighPart = data.ftLastWriteTime.dwHighDateTime};
    assert(time.QuadPart >= 116444736000000000ull);
    return (time.QuadPart - 116444736000000000ull) / 10000 + 2208988800000ull;
#else
    struct stat st;
    assert(!stat(path, &st) && st.st_mtime >= 0);
#if defined(__APPLE__) /* as fs/tree.c reads it */
    return (uint64_t)st.st_mtime * 1000 + (uint64_t)st.st_mtimespec.tv_nsec / 1000000 + 2208988800000ull;
#else
    return (uint64_t)st.st_mtime * 1000 + (uint64_t)st.st_mtim.tv_nsec / 1000000 + 2208988800000ull;
#endif
#endif
}
static uint64_t get64(SnxCtrProcess* p, uint32_t address) {
    unsigned char b[8];
    assert(snx_ctr_read_ram(p->space, address, b, sizeof b) == SNX_CTR_MEM_OK);
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)b[i] << (i * 8);
    return v;
}
static void enc16(unsigned char* b, const uint16_t* units, unsigned bytes) {
    assert(bytes <= 256);
    for (unsigned i = 0; i < bytes; ++i) b[i] = (unsigned char)(units[i / 2] >> ((i & 1) * 8));
}
static void utf16(SnxCtrProcess* p, uint32_t address, const uint16_t* units, unsigned bytes) {
    unsigned char b[256];
    enc16(b, units, bytes);
    assert(snx_ctr_write_ram(p->space, address, b, bytes) == SNX_CTR_MEM_OK);
}
static void timestamps(SnxCtrProcess* p, uint32_t fs, const char* directory) {
    uint32_t a[] = {0x80c00c2, 3, 1, 1, 0x4002, 0x300020};
    ctr_call(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    uint32_t lo = ctr_command_word(p, 2), hi = ctr_command_word(p, 3);
    char* hello_path = snx_path_join(directory, "romfs/hello.txt");
    char* root_path = snx_path_join(directory, "romfs");
    uint64_t hello = host_datetime(hello_path), root = host_datetime(root_path);
    free(hello_path);
    free(root_path);
    static const uint16_t hello16[] = {'h', 'e', 'l', 'l', 'o', '.', 't', 'x', 't', 0};
    static const uint16_t slashed16[] = {'\\', 'h', 'e', 'l', 'l', 'o', '.', 't', 'x', 't', 0};
    static const uint16_t root16[] = {0};
    static const uint16_t missing16[] = {'n', 'o', 'p', 'e', 0};
    static const uint16_t bad16[] = {0xd800, 0};
    uint32_t cmd[] = {0x80d0144, lo, hi, 1, 20, 8, (20u << 4) | 10, 0x300400, (8u << 4) | 12, 0x300500};
    utf16(p, 0x300400, hello16, 20);
    ctr_call(p, fs, cmd, 10, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x080d0040 && !ctr_command_word(p, 1));
    assert(get64(p, 0x300500) == hello && hello > 2208988800000ull);
    uint64_t api = 0;
    unsigned char raw[256];
    enc16(raw, hello16, 20);
    assert(snx_ctr_romfs_time(p->romfs, raw, 20, &api) == SNX_CTR_TIME_OK && api == hello);
    utf16(p, 0x300400, slashed16, 22);
    cmd[4] = 22;
    cmd[6] = (22u << 4) | 10;
    ctr_call(p, fs, cmd, 10, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && get64(p, 0x300500) == hello); /* \ separator resolves the same backing file */
    utf16(p, 0x300400, root16, 2);
    cmd[4] = 2;
    cmd[6] = (2u << 4) | 10;
    ctr_call(p, fs, cmd, 10, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && get64(p, 0x300500) == root);
    enc16(raw, root16, 2);
    assert(snx_ctr_romfs_time(p->romfs, raw, 2, &api) == SNX_CTR_TIME_OK && api == root);
    ctr_put(p, 0x300500, 0xdeadbeef);
    utf16(p, 0x300400, missing16, 10);
    cmd[4] = 10;
    cmd[6] = (10u << 4) | 10;
    ctr_call(p, fs, cmd, 10, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 1) == 0xc8804478 && ctr_command_word(p, 0) == 0x080d0040 &&
           (uint32_t)get64(p, 0x300500) == 0xdeadbeef);
    enc16(raw, missing16, 10);
    assert(snx_ctr_romfs_time(p->romfs, raw, 10, &api) == SNX_CTR_TIME_NOTFOUND);
    utf16(p, 0x300400, bad16, 4);
    cmd[4] = 4;
    cmd[6] = (4u << 4) | 10;
    ctr_call(p, fs, cmd, 10, SNX_CTR_SVC_UNSUPPORTED, 0); /* unpaired surrogate is not a path */
    enc16(raw, bad16, 4);
    assert(snx_ctr_romfs_time(p->romfs, raw, 4, &api) == SNX_CTR_TIME_INVALID);
    cmd[3] = 0;
    ctr_call(p, fs, cmd, 10, SNX_CTR_SVC_UNSUPPORTED, 0); /* save commit needs a save archive */
    cmd[3] = 1;
    cmd[4] = 3;
    cmd[6] = (3u << 4) | 10;
    ctr_call(p, fs, cmd, 10, SNX_CTR_SVC_UNSUPPORTED, 0); /* odd path size */
    cmd[4] = 20;
    cmd[6] = (20u << 4) | 10;
    ctr_call(p, fs, cmd, 10, SNX_CTR_SVC_UNSUPPORTED, 0); /* stale bytes are not the earlier path */
    cmd[6] = (20u << 4) | 11;
    ctr_call(p, fs, cmd, 10, SNX_CTR_SVC_RETURN, SNX_CTR_IPC_BAD_DESCRIPTOR);
    cmd[6] = (20u << 4) | 10;
    cmd[7] = 0x304000;
    ctr_call(p, fs, cmd, 10, SNX_CTR_SVC_RETURN, 0xe0e01bf5); /* unmapped path input */
    cmd[7] = 0x300400;
    cmd[9] = 0x100000;
    ctr_call(p, fs, cmd, 10, SNX_CTR_SVC_RETURN, 0xe0e01bf5); /* read-only output span */
    cmd[9] = 0x300500;
    cmd[1] = lo + 0x1000;
    ctr_call(p, fs, cmd, 10, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 1) == 0xc8804465); /* stale archive token */
    cmd[1] = lo;
    uint32_t close[] = {0x80e0080, lo, hi};
    ctr_call(p, fs, close, 3, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    ctr_call(p, fs, cmd, 10, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 1) == 0xc8804465); /* timestamps need a live binding */
}
/* The sdmc area is read-write, backed by the shared host tree like NX: real
   create/write/read/resize/rename/delete, with host files as the durable
   store. Directory probes match the devoptab's stat fallback. */
static void sd_area(SnxCtrProcess* p, uint32_t fs, const char* directory) {
    uint32_t a[] = {0x80c00c2, 9, 1, 1, 0x4002, 0x300020};
    ctr_call(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    uint32_t lo = ctr_command_word(p, 2), hi = ctr_command_word(p, 3);
    static const uint16_t dir16[] = {'d', 0};
    static const uint16_t file16[] = {'d', '/', 'f', '.', 'b', 'i', 'n', 0};
    static const uint16_t moved16[] = {'d', '/', 'm', '.', 'b', 'i', 'n', 0};
    utf16(p, 0x300400, dir16, 4);
    /* the path's static buffer descriptor must name buffer 0 with the path's size */
    uint32_t bad_mkdir[] = {0x8090182, 0, lo, hi, 4, 4, 0, 0x10402, 0x300400};
    ctr_call(p, fs, bad_mkdir, 9, SNX_CTR_SVC_RETURN, SNX_CTR_IPC_BAD_DESCRIPTOR);
    uint32_t bad_rmdir[] = {0x8060142, 0, lo, hi, 4, 4, 0x20002, 0x300400};
    ctr_call(p, fs, bad_rmdir, 8, SNX_CTR_SVC_RETURN, SNX_CTR_IPC_BAD_DESCRIPTOR);
    uint32_t mkdir[] = {0x8090182, 0, lo, hi, 4, 4, 0, 0x10002, 0x300400};
    ctr_call(p, fs, mkdir, 9, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    ctr_call(p, fs, mkdir, 9, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 1) == 0xc88044be); /* already exists */
    utf16(p, 0x300400, file16, 16);
    uint32_t mkfile[] = {0x8080202, 0, lo, hi, 4, 16, 0, 4, 0, 0x40002, 0x300400};
    ctr_call(p, fs, mkfile, 11, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    uint32_t open[] = {0x80201c2, 0, lo, hi, 4, 16, 3, 0, 0x40002, 0x300400};
    ctr_call(p, fs, open, 10, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x8020042 && !ctr_command_word(p, 1) && ctr_command_word(p, 2) == 0x10);
    uint32_t file = ctr_command_word(p, 3);
    ctr_put(p, 0x300600, 0x64636261); /* "abcd" */
    uint32_t write[] = {0x8030102, 0, 0, 4, 0, (4u << 4) | 10, 0x300600};
    ctr_call(p, file, write, 7, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && ctr_command_word(p, 2) == 4);
    uint32_t size[] = {0x8040000};
    ctr_call(p, file, size, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && ctr_command_word(p, 2) == 4 && !ctr_command_word(p, 3));
    uint32_t grow[] = {0x8050080, 6, 0};
    ctr_call(p, file, grow, 3, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    ctr_put(p, 0x300500, 0);
    ctr_put(p, 0x300504, 0);
    uint32_t read[] = {0x80200c2, 0, 0, 6, (6u << 4) | 12, 0x300500};
    ctr_call(p, file, read, 6, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && ctr_command_word(p, 2) == 6);
    unsigned char bytes[8];
    assert(snx_ctr_read_ram(p->space, 0x300500, bytes, 6) == SNX_CTR_MEM_OK);
    assert(!memcmp(bytes, "abcd\0\0", 6)); /* grown with zeros */
    uint32_t finish[] = {0x8080000};
    ctr_call(p, file, finish, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && !snx_ctr_handle_close(p, file));
    /* stat-style probes: a directory is not a file, then opens as directory */
    utf16(p, 0x300400, dir16, 4);
    uint32_t probe[] = {0x80201c2, 0, lo, hi, 4, 4, 1, 0, 0x10002, 0x300400};
    ctr_call(p, fs, probe, 10, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 1) == 0xc8804702);
    utf16(p, 0x300400, dir16, 4);
    uint32_t opend[] = {0x80b0102, lo, hi, 4, 4, 0x10002, 0x300400};
    ctr_call(p, fs, opend, 7, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x80b0042 && !ctr_command_word(p, 1) && ctr_command_word(p, 2) == 0x10);
    uint32_t handle = ctr_command_word(p, 3);
    uint32_t dclose[] = {0x8020000}; /* a directory session's Close (libctru FSDIR_Close) */
    ctr_call(p, handle, dclose, 1, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x8020040 && !ctr_command_word(p, 1) && !snx_ctr_handle_close(p, handle));
    /* rename through the wire, then delete */
    utf16(p, 0x300400, file16, 16);
    utf16(p, 0x300700, moved16, 16);
    uint32_t rename[] = {0x8050244, 0, lo, hi, 4, 16, lo, hi, 4, 16, 0x40002, 0x300400, (16u << 14) | (1u << 11) | 2,
                         0x300700};
    ctr_call(p, fs, rename, 14, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    /* the host file under the shared sdmc tree carries the written bytes */
    char* host = snx_path_join(directory, "sdmc/d/m.bin");
    FILE* disk = fopen(host, "rb");
    assert(disk);
    char content[8] = {0};
    assert(fread(content, 1, 6, disk) == 6 && !memcmp(content, "abcd\0\0", 6));
    assert(!fclose(disk));
    free(host);
    uint32_t remove[] = {0x8040142, 0, lo, hi, 4, 16, 0x40002, 0x300700};
    ctr_call(p, fs, remove, 8, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    ctr_call(p, fs, remove, 8, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 1) == 0xc8804478); /* gone */
    utf16(p, 0x300400, dir16, 4);
    uint32_t rmdir[] = {0x8060142, 0, lo, hi, 4, 4, 0x10002, 0x300400};
    ctr_call(p, fs, rmdir, 8, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    /* save commit on the SD area succeeds: writes already landed on the host */
    uint32_t commit[] = {0x80d0144, lo, hi, 0, 12, 8, (12u << 4) | 10, 0x300400, (8u << 4) | 12, 0x300500};
    ctr_call(p, fs, commit, 10, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
}
/* A game's ExtSaveData (archive 6): missing until CreateExtSaveData makes it in the SD card's extdata folder, its files
   fixed in size, its folder read through a directory session, then all of it deleted. Then the shared ExtSaveData
   (archive 7) PTM made at the mount, with its Play Coins */
static void extdata(SnxCtrProcess* p, uint32_t fs, const char* directory) {
    ctr_put(p, 0x300020, 1); /* the path: media SDMC, save ID low, high */
    ctr_put(p, 0x300024, 0x1234);
    ctr_put(p, 0x300028, 0);
    uint32_t a[] = {0x80c00c2, 6, 2, 12, (12u << 14) | 2, 0x300020};
    ctr_call(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 1) == 0xc8a04478); /* not found: the game creates it */
    uint32_t info[] = {0x84500c2, 6, 2, 12, (12u << 14) | 2, 0x300020};
    ctr_call(p, fs, info, 6, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 1) == 0xc8a04554);
    ctr_put(p, 0x300600, 0x48444d53); /* a four-byte icon */
    uint32_t create[] = {0x8510242, 1, 0x1234, 0, 0, 10, 20, 0x100000, 0, 4, (4u << 4) | 10, 0x300600};
    ctr_call(p, fs, create, 12, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x8510042 && !ctr_command_word(p, 1) &&
           ctr_command_word(p, 2) == ((4u << 4) | 10) && ctr_command_word(p, 3) == 0x300600);
    char host[1024];
    snprintf(host, sizeof host,
             "%s/sdmc/Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000/extdata/"
             "00000000/00001234/metadata",
             directory);
    FILE* f = fopen(host, "rb");
    unsigned char metadata[17];
    assert(f && fread(metadata, 1, sizeof metadata, f) == 16 && !fclose(f));
    assert(metadata[2] == 0x10 && metadata[4] == 10 && metadata[8] == 20);
    ctr_call(p, fs, info, 6, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x8450140 && !ctr_command_word(p, 1) && ctr_command_word(p, 2) == 0x100000 &&
           ctr_command_word(p, 3) == 10 && ctr_command_word(p, 4) == 20 && !ctr_command_word(p, 5));
    ctr_call(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x80c00c0 && !ctr_command_word(p, 1));
    uint32_t lo = ctr_command_word(p, 2), hi = ctr_command_word(p, 3);
    static const uint16_t file16[] = {'/', 'f', '.', 'd', 'a', 't', 0};
    static const uint16_t root16[] = {'/', 0};
    utf16(p, 0x300400, file16, 14);
    uint32_t mkfile[] = {0x8080202, 0, lo, hi, 4, 14, 0, 8, 0, (14u << 14) | 2, 0x300400};
    ctr_call(p, fs, mkfile, 11, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    uint32_t open[] = {0x80201c2, 0, lo, hi, 4, 14, 7, 0, (14u << 14) | 2, 0x300400};
    ctr_call(p, fs, open, 10, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 1) == 0xe0c046f8); /* no create flag in an ExtSaveData */
    open[6] = 1;                     /* read only, opened read-write all the same */
    ctr_call(p, fs, open, 10, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    uint32_t file = ctr_command_word(p, 3);
    ctr_put(p, 0x300600, 0x64636261);
    uint32_t write[] = {0x8030102, 6, 0, 4, 0, (4u << 4) | 10, 0x300600};
    ctr_call(p, file, write, 7, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && ctr_command_word(p, 2) == 2); /* stops at the end */
    write[1] = 9;
    ctr_call(p, file, write, 7, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 1) == 0xe0c046c1); /* past it */
    uint32_t grow[] = {0x8050080, 100, 0};
    ctr_call(p, file, grow, 3, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    uint32_t size[] = {0x8040000};
    ctr_call(p, file, size, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && ctr_command_word(p, 2) == 8); /* kept its size */
    uint32_t flush[] = {0x8090000};
    ctr_call(p, file, flush, 1, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x8090040 && !ctr_command_word(p, 1));
    uint32_t priority[] = {0x80a0040, 5}, get_priority[] = {0x80b0000};
    ctr_call(p, file, priority, 2, SNX_CTR_SVC_RETURN, 0);
    ctr_call(p, file, get_priority, 1, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x80b0080 && ctr_command_word(p, 2) == 5);
    uint32_t link[] = {0x80c0000};
    ctr_call(p, file, link, 1, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x80c0042 && !ctr_command_word(p, 1) && ctr_command_word(p, 2) == 0x10);
    uint32_t linked = ctr_command_word(p, 3);
    ctr_call(p, linked, get_priority, 1, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 2) == 5);
    uint32_t finish[] = {0x8080000};
    ctr_call(p, linked, finish, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && !snx_ctr_handle_close(p, linked));
    ctr_call(p, file, finish, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && !snx_ctr_handle_close(p, file));
    /* the folder's one entry, then none */
    utf16(p, 0x300400, root16, 4);
    uint32_t opend[] = {0x80b0102, lo, hi, 4, 4, (4u << 14) | 2, 0x300400};
    ctr_call(p, fs, opend, 7, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    uint32_t dir = ctr_command_word(p, 3);
    uint32_t read[] = {0x8010042, 2, ((2u * 0x228) << 4) | 12, 0x300800};
    ctr_call(p, dir, read, 4, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x8010082 && !ctr_command_word(p, 1) && ctr_command_word(p, 2) == 1);
    unsigned char entry[0x228];
    assert(snx_ctr_read_ram(p->space, 0x300800, entry, sizeof entry) == SNX_CTR_MEM_OK);
    assert(entry[0] == 'f' && entry[2] == '.' && !entry[12] && !memcmp(entry + 0x20c, "F       ", 9) &&
           !memcmp(entry + 0x216, "DAT", 4) && !entry[0x21c] && entry[0x21e] == 1 && entry[0x220] == 8);
    ctr_call(p, dir, read, 4, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && !ctr_command_word(p, 2));
    uint32_t dclose[] = {0x8020000};
    ctr_call(p, dir, dclose, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && !snx_ctr_handle_close(p, dir));
    uint32_t free_bytes[] = {0x8120080, lo, hi};
    ctr_call(p, fs, free_bytes, 3, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x81200c0 && !ctr_command_word(p, 1) && ctr_command_word(p, 2) == 32u << 20 &&
           !ctr_command_word(p, 3)); /* 32 MiB */
    uint32_t close[] = {0x80e0080, lo, hi};
    ctr_call(p, fs, close, 3, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    uint32_t del[] = {0x8520100, 1, 0x1234, 0, 0};
    ctr_call(p, fs, del, 5, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x8520040 && !ctr_command_word(p, 1));
    ctr_call(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 1) == 0xc8a04478);
    assert(!fopen(host, "rb"));
    /* shared: 0xF000000B, its high word whatever the path says */
    ctr_put(p, 0x300020, 0);
    ctr_put(p, 0x300024, 0xf000000b);
    ctr_put(p, 0x300028, 0xf0000000);
    a[1] = 7;
    ctr_call(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x80c00c0 && !ctr_command_word(p, 1));
    lo = ctr_command_word(p, 2), hi = ctr_command_word(p, 3);
    static const uint16_t coins16[] = {'/', 'g', 'a', 'm', 'e', 'c', 'o', 'i', 'n', '.', 'd', 'a', 't', 0};
    utf16(p, 0x300400, coins16, 28);
    uint32_t coins[] = {0x80201c2, 0, lo, hi, 4, 28, 1, 0, (28u << 14) | 2, 0x300400};
    ctr_call(p, fs, coins, 10, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    file = ctr_command_word(p, 3);
    ctr_call(p, file, size, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && ctr_command_word(p, 2) == 20);
    uint32_t read_coins[] = {0x80200c2, 0, 0, 20, (20u << 4) | 12, 0x300100};
    ctr_call(p, file, read_coins, 6, SNX_CTR_SVC_RETURN, 0);
    unsigned char coin[20];
    assert(!ctr_command_word(p, 1) && snx_ctr_read_ram(p->space, 0x300100, coin, 20) == SNX_CTR_MEM_OK);
    assert(coin[0] == 0 && coin[1] == 0x4f && coin[4] == 42 && coin[16] == 0xde && coin[17] == 7);
    ctr_call(p, file, finish, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && !snx_ctr_handle_close(p, file));
    close[1] = lo, close[2] = hi;
    ctr_call(p, fs, close, 3, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    ctr_put(p, 0x300024, 0xf0000099); /* a shared one the system did not make */
    ctr_call(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 1) == 0xc8a04554);
}
/* The NCCH archive through OpenFileDirectly: a system title Azahar has an open stand-in for (the Mii data, the bad
   word list) is that RomFS, another one NotFound, and the title's own RomFS a file */
static void ncch(SnxCtrProcess* p, uint32_t fs, bool absent) {
    for (unsigned i = 0; i < 5; ++i) ctr_put(p, 0x300040 + i * 4, 0); /* NCCH data, content 0, its RomFS */
    uint32_t open[] = {0x8030204, 0, 0x2345678a, 2, 16, 2, 0x14, 1, 0, (16u << 14) | 0x802, 0x300020,
                       (0x14u << 14) | 2, 0x300040};
    uint32_t size[] = {0x8040000}, close[] = {0x8080000};
    static const struct {
        uint64_t title;
        uint32_t size;
    } stand_ins[] = {{0x0004009b00010202ull, 73600}, {0x000400db00010302ull, 1508}};
    for (unsigned i = 0; i < 2; ++i) {
        ctr_put(p, 0x300020, (uint32_t)stand_ins[i].title);
        ctr_put(p, 0x300024, (uint32_t)(stand_ins[i].title >> 32));
        ctr_call(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 0) == 0x8030042 && !ctr_command_word(p, 1));
        uint32_t file = ctr_command_word(p, 3);
        ctr_call(p, file, size, 1, SNX_CTR_SVC_RETURN, 0);
        assert(!ctr_command_word(p, 1) && ctr_command_word(p, 2) == stand_ins[i].size && !ctr_command_word(p, 3));
        uint32_t read[] = {0x80200c2, 0, 0, 4, (4u << 4) | 12, 0x300100};
        ctr_call(p, file, read, 6, SNX_CTR_SVC_RETURN, 0);
        unsigned char header[4];
        assert(!ctr_command_word(p, 1) && ctr_command_word(p, 2) == 4 &&
               snx_ctr_read_ram(p->space, 0x300100, header, 4) == SNX_CTR_MEM_OK);
        assert(header[0] == 0x28 && !header[1] && !header[2] && !header[3]); /* a level 3 RomFS's header size */
        ctr_call(p, file, close, 1, SNX_CTR_SVC_RETURN, 0);
        assert(!ctr_command_word(p, 1) && !snx_ctr_handle_close(p, file));
    }
    ctr_put(p, 0x300020, 0x00010102); /* a system title with no stand-in */
    ctr_put(p, 0x300024, 0x0004009b);
    ctr_call(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 1) == 0xc8804478);
    ctr_put(p, 0x300020, (uint32_t)p->title_id);
    ctr_put(p, 0x300024, (uint32_t)(p->title_id >> 32));
    ctr_call(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0);
    if (absent) {
        assert(ctr_command_word(p, 1) == 0xc8804464);
        return;
    }
    assert(ctr_command_word(p, 0) == 0x8030042 && !ctr_command_word(p, 1) && ctr_command_word(p, 2) == 0x10);
    uint32_t file = ctr_command_word(p, 3);
    ctr_call(p, file, close, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && !snx_ctr_handle_close(p, file));
}
/* SelfNCCH's other file types: an ExeFS file (the extraction's exefs/icon), one it lacks, an unknown name, the code
   and an update's RomFS */
static void self_exefs(SnxCtrProcess* p, uint32_t fs) {
    uint32_t a[] = {0x80c00c2, 3, 1, 1, 0x4002, 0x300020};
    ctr_call(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    uint32_t lo = ctr_command_word(p, 2), hi = ctr_command_word(p, 3);
    static const struct {
        uint32_t type;
        char name[9];
        uint32_t result;
    } cases[] = {{2, "banner", 0xc8804637}, {2, ".code", 0xe0e046be}, {1, "", 0xd9004676}, {5, "", 0xc8804464}};
    unsigned char path[12] = {2, 0, 0, 0, 'i', 'c', 'o', 'n'};
    uint32_t open[] = {0x80201c2, 0, lo, hi, 2, 12, 1, 0, 0x30002, 0x300000};
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; ++i) {
        unsigned char other[12] = {(unsigned char)cases[i].type};
        memcpy(other + 4, cases[i].name, 8);
        assert(snx_ctr_write_ram(p->space, 0x300000, other, sizeof other) == SNX_CTR_MEM_OK);
        ctr_call(p, fs, open, 10, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 1) == cases[i].result);
    }
    assert(snx_ctr_write_ram(p->space, 0x300000, path, sizeof path) == SNX_CTR_MEM_OK);
    ctr_call(p, fs, open, 10, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x8020042 && !ctr_command_word(p, 1));
    uint32_t file = ctr_command_word(p, 3);
    uint32_t read[] = {0x80200c2, 0, 0, 16, (16u << 4) | 12, 0x300100};
    ctr_call(p, file, read, 6, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && ctr_command_word(p, 2) == 9);
    unsigned char bytes[9];
    assert(snx_ctr_read_ram(p->space, 0x300100, bytes, 9) == SNX_CTR_MEM_OK && !memcmp(bytes, "SMDH-icon", 9));
    uint32_t write[] = {0x8030102, 0, 0, 4, 0, (4u << 4) | 10, 0x300100};
    ctr_call(p, file, write, 7, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 1) == 0xc92044e6); /* read-only */
    uint32_t finish[] = {0x8080000}, close[] = {0x80e0080, lo, hi};
    ctr_call(p, file, finish, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && !snx_ctr_handle_close(p, file));
    ctr_call(p, fs, close, 3, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    static const unsigned char romfs_path[12] = {0}; /* the RomFS's path again, which later checks open */
    assert(snx_ctr_write_ram(p->space, 0x300000, romfs_path, sizeof romfs_path) == SNX_CTR_MEM_OK);
}
int main(int argc, char** argv) {
    assert(argc == 4);
    g_settings.ctr_answer_unknown_commands = 0; /* an unknown command stops, as the tests expect */
#if defined(__unix__) || defined(__APPLE__)
    if (!strcmp(argv[3], "fallback")) {
        struct rlimit limit = {64, 64};
        assert(!setrlimit(RLIMIT_NOFILE, &limit));
    }
#endif
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = ctr_image(code, sizeof code, 0x3000);
    for (unsigned model = 0; model < 2; ++model) {
        SnxCtrProcess* p = snx_ctr_process_new(&image, (SnxCtrModel)model);
        assert(p);
        bool absent = !strcmp(argv[3], "absent");
        assert(snx_ctr_process_mount_fs(p, argv[1]) && (!!p->romfs != absent));
        if (!absent) assert(!snx_ctr_process_mount_fs(p, argv[1]));
        uint32_t fs;
        assert(!snx_ctr_session_create(p, SNX_CTR_SERVICE_FS, &fs));
        uint32_t open[] = {0x8030204, 0, 3, 1, 1, 2, 12, 1, 0, 0x4802, 0x300020, 0x30002, 0x300000};
        ctr_call(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 1) == SNX_CTR_IPC_INVALID_STATE);
        uint32_t init[] = {0x8010002, 0x20, 0xffffffff};
        ctr_call(p, fs, init, 3, SNX_CTR_SVC_RETURN, 0);
        assert(!ctr_command_word(p, 1));
        unsigned before = ctr_object_count(p);
        archives(p, fs, absent);
        if (!absent) timestamps(p, fs, argv[1]);
        sd_area(p, fs, argv[1]);
        extdata(p, fs, argv[1]);
        ncch(p, fs, absent);
        if (!absent) self_exefs(p, fs);
        if (absent) {
            ctr_call(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0);
            assert(ctr_command_word(p, 1) == 0xc8804464 && ctr_object_count(p) == before);
            snx_ctr_process_free(p);
            continue;
        }
        open[9] = 0x8802;
        ctr_call(p, fs, open, 13, SNX_CTR_SVC_RETURN, SNX_CTR_IPC_BAD_DESCRIPTOR);
        assert(ctr_object_count(p) == before && ctr_command_word(p, 0) == open[0]);
        open[9] = 0x4802;
        open[10] = 0x304000;
        ctr_call(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0xe0e01bf5);
        assert(ctr_object_count(p) == before);
        open[10] = 0x300020;
        open[7] = 3;
        ctr_call(p, fs, open, 13, SNX_CTR_SVC_UNSUPPORTED, 0);
        assert(ctr_object_count(p) == before);
        open[7] = 1;
        uint32_t aliases[CTR_HANDLES], n = 0, unused;
        while (n < CTR_HANDLES && !snx_ctr_handle_add(p, 1, &aliases[n])) ++n;
        ctr_call(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 1) == 0xd8600413 && ctr_object_count(p) == before);
        for (unsigned i = 0; i < n; ++i) assert(!snx_ctr_handle_close(p, aliases[i]));
        ctr_call(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 0) == 0x8030042 && !ctr_command_word(p, 1) && ctr_command_word(p, 2) == 0x10);
        uint32_t file = ctr_command_word(p, 3), id = snx_ctr_handle_object(p, file), duplicate;
        assert(id && p->objects[id].type == CTR_OBJ_FILE && !snx_ctr_handle_add(p, id, &duplicate));
        uint32_t size_cmd[] = {0x8040000};
        ctr_call(p, file, size_cmd, 1, SNX_CTR_SVC_RETURN, 0);
        uint64_t size = (uint64_t)ctr_command_word(p, 2) | (uint64_t)ctr_command_word(p, 3) << 32;
        assert(size && size < 12000);
        assert(ctr_command_word(p, 0) == 0x80400c0 && size == snx_ctr_romfs_size(p->romfs));
        /* the fixed 256-byte negative-read patterns below need 256 bytes even
           when a tiny fixture's whole RomFS view is smaller */
        size_t alloc = size < 256 ? 256 : (size_t)size;
        unsigned char *expected = malloc(alloc), *actual = malloc(alloc);
        assert(expected && actual);
        assert(snx_ctr_romfs_read(p->romfs, 0, expected, (size_t)size));
        uint32_t read[] = {0x80200c2, 0, 0, (uint32_t)size, (uint32_t)size << 4 | 12, 0x300800};
        ctr_call(p, file, read, 6, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 0) == 0x8020082 && !ctr_command_word(p, 1) && ctr_command_word(p, 2) == size &&
               ctr_command_word(p, 3) == read[4] && ctr_command_word(p, 4) == read[5]);
        assert(snx_ctr_read_ram(p->space, read[5], actual, (size_t)size) == SNX_CTR_MEM_OK &&
               !memcmp(actual, expected, (size_t)size));
        if (!model) assert(!snx_write_file(argv[2], actual, (size_t)size));
        for (uint32_t off = 0; off < size; off += 137) {
            size_t n = size - off < 139 ? (size_t)(size - off) : 139;
            assert(snx_ctr_romfs_read(p->romfs, off, actual, n) && !memcmp(actual, expected + off, n));
        }
        if (!strcmp(argv[3], "fallback")) {
            char* source = snx_path_join(argv[1], "romfs/hello.txt");
            char* moved = snx_path_join(argv[1], "romfs/hello.moved");
            assert(source && moved && !rename(source, moved));
            memset(actual, 0x55, (size_t)size);
            assert(snx_ctr_write_ram(p->space, read[5], actual, (size_t)size) == SNX_CTR_MEM_OK);
            ctr_call(p, file, read, 6, SNX_CTR_SVC_UNSUPPORTED, 0);
            assert(ctr_command_word(p, 0) == read[0]);
            assert(snx_ctr_read_ram(p->space, read[5], actual, (size_t)size) == SNX_CTR_MEM_OK);
            for (uint64_t i = 0; i < size; ++i) assert(actual[i] == 0x55);
            assert(!rename(moved, source));
            free(source);
            free(moved);
        }
        read[1] = (uint32_t)size - 2;
        read[3] = 32;
        read[4] = 32 << 4 | 12;
        ctr_call(p, duplicate, read, 6, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 2) == 2);
        read[1] = (uint32_t)size + 1;
        ctr_call(p, file, read, 6, SNX_CTR_SVC_RETURN, 0);
        assert(!ctr_command_word(p, 2));
        read[1] = read[2] = UINT32_MAX;
        ctr_call(p, file, read, 6, SNX_CTR_SVC_RETURN, 0);
        assert(!ctr_command_word(p, 2));
        read[1] = read[2] = 0;
        read[3] = 512;
        read[4] = 512 << 4 | 12;
        read[5] = 0x303f00;
        unsigned char fill[256];
        memset(fill, 0x55, sizeof fill);
        assert(snx_ctr_write_ram(p->space, read[5], fill, 256) == SNX_CTR_MEM_OK);
        ctr_call(p, file, read, 6, SNX_CTR_SVC_RETURN, 0xe0e01bf5);
        assert(ctr_command_word(p, 0) == read[0]);
        assert(snx_ctr_read_ram(p->space, read[5], actual, 256) == SNX_CTR_MEM_OK && !memcmp(fill, actual, 256));
        read[5] = 0x200000;
        ctr_call(p, file, read, 6, SNX_CTR_SVC_RETURN, 0xe0e01bf5);
        assert(snx_ctr_map(p->space, 0x50000000, 0x10100000, 4096, 3) == SNX_CTR_MEM_OK);
        snx_ctr_memory_mmio(p->memory, NULL, NULL, no_mmio);
        read[5] = 0x50000000;
        ctr_call(p, file, read, 6, SNX_CTR_SVC_RETURN, 0xe0e01bf5);
        read[5] = 0x300800;
        read[4] = 16 << 4 | 12;
        ctr_call(p, file, read, 6, SNX_CTR_SVC_RETURN, SNX_CTR_IPC_BAD_DESCRIPTOR);
        read[4] = 512 << 4 | 10; /* a buffer the service may only read, for a Read */
        ctr_call(p, file, read, 6, SNX_CTR_SVC_RETURN, SNX_CTR_IPC_BAD_DESCRIPTOR);
        uint32_t unknown[] = {0x8060000}; /* no FSFILE command 0x806 */
        ctr_call(p, file, unknown, 1, SNX_CTR_SVC_UNSUPPORTED, 0);
        uint32_t close[] = {0x8080000};
        ctr_call(p, file, close, 1, SNX_CTR_SVC_RETURN, 0);
        assert(!ctr_command_word(p, 1));
        ctr_call(p, duplicate, size_cmd, 1, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 1) == SNX_CTR_IPC_INVALID_STATE);
        assert(!snx_ctr_handle_close(p, file) && p->objects[id].refs == 1 && !snx_ctr_handle_close(p, duplicate) &&
               !p->objects[id].type);
        ctr_call(p, file, size_cmd, 1, SNX_CTR_SVC_RETURN, 0xd8e007f7);
        for (n = 0; n < 64; ++n) {
            ctr_call(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0);
            assert(!ctr_command_word(p, 1));
            aliases[n] = ctr_command_word(p, 3);
        }
        before = ctr_object_count(p);
        ctr_call(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0);
        assert(ctr_command_word(p, 1) == 0xd0401834 && ctr_object_count(p) == before);
        for (n = 0; n < 64; ++n) assert(!snx_ctr_handle_close(p, aliases[n]));
        assert(!snx_ctr_handle_add(p, 1, &unused) && !snx_ctr_handle_close(p, unused));
        free(expected);
        free(actual);
        snx_ctr_process_free(p);
    }
    puts("CTR RomFS: real files, shared/fallback reads, checked buffers, quotas, close/duplicate lifetime passed");
    return 0;
}
