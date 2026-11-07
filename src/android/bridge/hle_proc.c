/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The app's own /proc files (hle_proc.h) */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "android/bridge/hle_proc.h"
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
    const char* file = path ? own_file(path) : NULL;
    if (!file) return -2;
    void (*write)(FILE*) = NULL;
    bool maps = false, command = false, name = false;
    if (!strcmp(file, "status")) write = write_status;
    else if (!strcmp(file, "stat")) write = write_stat;
    else if (!strcmp(file, "maps")) maps = true;
    else if (!strcmp(file, "cmdline")) command = true;
    else if (!strcmp(file, "comm")) name = true;
    else return -2;
    FILE* f = tmpfile();
    if (!f) return -1;
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
