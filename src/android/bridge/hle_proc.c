/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The app's own /proc files (hle_proc.h) */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "android/bridge/hle_proc.h"
#include "android/bridge/hle.h"
#include "android/bridge/host.h"
#include "android/bridge/linker.h"
#include "common/utilities/endian.h"
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define dup _dup
#define fileno _fileno
#else
#include <unistd.h>
#endif

static char g_package[256] = "app";

void hle_proc_set_app(const char* package) {
    if (package && package[0]) snprintf(g_package, sizeof g_package, "%s", package);
}

/* what an Android device's zygote starts an app as */
static const char app_exe[] = "/system/bin/app_process64";

/* the file name under the app's process directory, NULL when path is not in it */
static const char* own_file(const char* path) {
    static const char* const selves[] = {"/proc/self/", "/proc/thread-self/"};
    for (size_t i = 0; i < 2; ++i) {
        size_t n = strlen(selves[i]);
        if (!strncmp(path, selves[i], n)) return path + n;
    }
    char own[32];
    int n = snprintf(own, sizeof own, "/proc/%u/", (unsigned)host_pid());
    return n > 0 && !strncmp(path, own, (size_t)n) ? path + n : NULL;
}

/* ---- the system's libraries as files ----
   /system/lib64/<name> and /system/lib/<name> of a library the HLE is (libvulkan.so, libEGL.so): an ELF shared
   object of no code whose .dynamic has the library's DT_SONAME. A loader that copies the system's library to load it
   again (adrenotools' copy of libvulkan.so, its soname patched) reads this; its e_ident's padding marks it, and its
   string table keeps the name a second time, which android_dlopen_ext of the copy reads back
   (hle_proc_stub_library) */
static const char stub_mark[] = "SNXLIB";
static const char* system_library(const char* path) {
    static const char* const dirs[] = {"/system/lib64/", "/system/lib/"};
    for (size_t i = 0; i < 2; ++i) {
        size_t n = strlen(dirs[i]);
        if (!strncmp(path, dirs[i], n) && !strchr(path + n, '/') && hle_dl_is_system(path + n)) return path + n;
    }
    return NULL;
}
static void put16(uint8_t* p, uint16_t v) { snx_st_le16(p, v); }
static void put32(uint8_t* p, uint32_t v) { snx_st_le32(p, v); }
static void put64(uint8_t* p, uint64_t v) { snx_st_le64(p, v); }
static void write_stub(FILE* f, const char* name) {
    size_t len = strlen(name);
    size_t strtab = 64, strsize = 1 + 2 * (len + 1);
    size_t dynamic = (strtab + strsize + 7) & ~(size_t)7, shoff = dynamic + 32;
    uint8_t out[1024] = {0};
    if (shoff + 3 * 64 > sizeof out) return;
    memcpy(out, "\177ELF", 4);
    out[4] = 2; /* ELFCLASS64 */
    out[5] = 1; /* little-endian */
    out[6] = 1; /* EV_CURRENT */
    memcpy(out + 9, stub_mark, sizeof stub_mark - 1);
    put16(out + 16, 3);   /* ET_DYN */
    put16(out + 18, 183); /* EM_AARCH64 */
    put32(out + 20, 1);
    put64(out + 40, shoff);
    put16(out + 52, 64); /* e_ehsize */
    put16(out + 58, 64); /* e_shentsize */
    put16(out + 60, 3);  /* e_shnum: none, .dynstr, .dynamic */
    memcpy(out + strtab + 1, name, len);
    memcpy(out + strtab + 1 + len + 1, name, len);
    put64(out + dynamic, 14); /* DT_SONAME */
    put64(out + dynamic + 8, 1);
    uint8_t* sh = out + shoff + 64; /* .dynstr */
    put32(sh + 4, 3);               /* SHT_STRTAB */
    put64(sh + 24, strtab);
    put64(sh + 32, strsize);
    sh += 64; /* .dynamic */
    put32(sh + 4, 6); /* SHT_DYNAMIC */
    put64(sh + 8, 2); /* SHF_ALLOC */
    put64(sh + 24, dynamic);
    put64(sh + 32, 32);
    put32(sh + 40, 1); /* sh_link: .dynstr */
    put64(sh + 56, 16);
    fwrite(out, 1, shoff + 3 * 64, f);
}
bool hle_proc_stub_library(int fd, char* name, size_t size) {
    uint8_t in[512];
#ifdef _WIN32
    (void)fd;
    (void)in;
    (void)name;
    (void)size;
    return false;
#else
    ssize_t n = pread(fd, in, sizeof in, 0);
    if (n < 66 || memcmp(in, "\177ELF", 4) || memcmp(in + 9, stub_mark, sizeof stub_mark - 1)) return false;
    size_t first = strnlen((const char*)in + 65, (size_t)n - 65);
    size_t at = 65 + first + 1;
    if (at >= (size_t)n) return false;
    size_t len = strnlen((const char*)in + at, (size_t)n - at);
    if (at + len >= (size_t)n || len + 1 > size) return false;
    memcpy(name, in + at, len + 1);
    return true;
#endif
}

/* the 15 bytes of a thread's name the kernel keeps: the package's first */
static void comm(char out[16]) {
    snprintf(out, 16, "%s", g_package);
}

static void write_status(FILE* f) {
    char name[16];
    comm(name);
    unsigned pid = host_pid();
    fprintf(f,
            "Name:\t%s\nUmask:\t0077\nState:\tS (sleeping)\nTgid:\t%u\nNgid:\t0\nPid:\t%u\nPPid:\t1\nTracerPid:\t0\n"
            "Uid:\t10200\t10200\t10200\t10200\nGid:\t10200\t10200\t10200\t10200\nFDSize:\t512\n"
            "Groups:\t3003 9997 20200 50200\nVmPeak:\t 8388608 kB\nVmSize:\t 8388608 kB\nVmLck:\t       0 kB\n"
            "VmPin:\t       0 kB\nVmHWM:\t 2097152 kB\nVmRSS:\t 2097152 kB\nThreads:\t64\nSigQ:\t0/31092\n"
            "SigPnd:\t0000000000000000\nShdPnd:\t0000000000000000\nSigBlk:\t0000000000001204\n"
            "SigIgn:\t0000000000000001\nSigCgt:\t00000006400084f8\nCapInh:\t0000000000000000\n"
            "CapPrm:\t0000000000000000\nCapEff:\t0000000000000000\nCapBnd:\t0000000000000000\n"
            "CapAmb:\t0000000000000000\nNoNewPrivs:\t0\nSeccomp:\t2\nSeccomp_filters:\t1\n"
            "Speculation_Store_Bypass:\tthread vulnerable\nCpus_allowed:\tff\nCpus_allowed_list:\t0-7\n"
            "Mems_allowed:\t1\nMems_allowed_list:\t0\nvoluntary_ctxt_switches:\t1000\nnonvoluntary_ctxt_switches:\t100\n",
            name, pid, pid);
}

static void write_stat(FILE* f) {
    char name[16];
    comm(name);
    fprintf(f,
            "%u (%s) S 1 1 0 0 -1 4210752 100000 0 0 0 1000 500 0 0 20 0 64 0 100 8589934592 524288 "
            "18446744073709551615 1 1 0 0 0 0 4612 1 1073775864 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
            host_pid(), name);
}

/* the guest's libraries, a line for each loadable segment: their addresses and access, at the place an APK's native
   libraries are on a device */
static int write_library(void* context, uint64_t base, const char* name, uint64_t phdr, unsigned count) {
    FILE* f = context;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned char* h = (const unsigned char*)(uintptr_t)(phdr + 56ull * i);
        if (snx_ld_le32(h) != 1) continue; /* PT_LOAD */
        uint32_t flags = snx_ld_le32(h + 4);
        uint64_t offset = snx_ld_le64(h + 8), vaddr = snx_ld_le64(h + 16), memsz = snx_ld_le64(h + 40);
        uint64_t start = (base + vaddr) & ~(uint64_t)0xfff, end = (base + vaddr + memsz + 0xfff) & ~(uint64_t)0xfff;
        fprintf(f, "%010llx-%010llx %c%c%cp %08llx fd:00 0                          /data/app/%s/lib/arm64/%s\n",
                (unsigned long long)start, (unsigned long long)end, flags & 4 ? 'r' : '-', flags & 2 ? 'w' : '-',
                flags & 1 ? 'x' : '-', (unsigned long long)(offset & ~(uint64_t)0xfff), g_package, name);
    }
    return 0;
}

int hle_proc_open(const char* path) {
    const char* library = path ? system_library(path) : NULL;
    const char* file = path && !library ? own_file(path) : NULL;
    if (!file && !library) return -2;
    void (*write)(FILE*) = NULL;
    bool maps = false, command = false, name = false;
    if (library) {}
    else if (!strcmp(file, "status")) write = write_status;
    else if (!strcmp(file, "stat")) write = write_stat;
    else if (!strcmp(file, "maps")) maps = true;
    else if (!strcmp(file, "cmdline")) command = true;
    else if (!strcmp(file, "comm")) name = true;
    else return -2;
    FILE* f = tmpfile();
    if (!f) return -1;
    if (library) write_stub(f, library);
    if (write) write(f);
    if (maps) linker_each(write_library, f);
    if (command) fwrite(g_package, 1, strlen(g_package) + 1, f);
    if (name) {
        char n[16];
        comm(n);
        fprintf(f, "%s\n", n);
    }
    fflush(f);
    int fd = dup(fileno(f)); /* the descriptor keeps the file after the stream goes */
    fclose(f);
#ifdef _WIN32
    if (fd >= 0) _lseeki64(fd, 0, SEEK_SET);
#else
    if (fd >= 0) lseek(fd, 0, SEEK_SET);
#endif
    return fd;
}

long hle_proc_readlink(const char* path, char* buffer, size_t size) {
    const char* file = path ? own_file(path) : NULL;
    if (!file || strcmp(file, "exe")) return -2;
    size_t n = strlen(app_exe) < size ? strlen(app_exe) : size;
    memcpy(buffer, app_exe, n);
    return (long)n;
}
