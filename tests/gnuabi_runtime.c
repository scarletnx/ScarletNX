/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gnuabi/process/process.h"
#include "gnuabi/abi/errno/errno.h"
#include "gnuabi/runtime/execution.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(expression)                                                                                              \
    do {                                                                                                               \
        if (!(expression)) {                                                                                           \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);                                           \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)
#if defined(__linux__)
/* the host's protection of the page at address, from /proc/self/maps: "rw", "r-" or "--" (absent: "") */
static const char* host_protection(const void* address) {
    static char perms[3];
    char line[512];
    FILE* maps = fopen("/proc/self/maps", "r");
    perms[0] = 0;
    if (!maps) return perms;
    while (fgets(line, sizeof line, maps)) {
        unsigned long long lo, hi;
        char p[5];
        if (sscanf(line, "%llx-%llx %4s", &lo, &hi, p) == 3 && (uintptr_t)address >= lo && (uintptr_t)address < hi) {
            perms[0] = p[0], perms[1] = p[1], perms[2] = 0;
            break;
        }
    }
    fclose(maps);
    return perms;
}
#endif
static void put16(unsigned char* p, unsigned value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}
static void put32(unsigned char* p, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) p[i] = (unsigned char)(value >> (8 * i));
}
static void put64(unsigned char* p, uint64_t value) {
    for (unsigned i = 0; i < 8; i++) p[i] = (unsigned char)(value >> (8 * i));
}
static void executable(unsigned char* bytes, unsigned type) {
    memset(bytes, 0, 512);
    memcpy(bytes, "\177ELF\2\1\1", 7);
    put16(bytes + 16, type);
    put16(bytes + 18, 183);
    put32(bytes + 20, 1);
    put64(bytes + 24, type == 3 ? 0x100 : 0x400100);
    put64(bytes + 32, 64);
    put16(bytes + 52, 64);
    put16(bytes + 54, 56);
    put16(bytes + 56, 1);
    put32(bytes + 64, 1);
    put32(bytes + 68, 5);
    put64(bytes + 80, type == 3 ? 0 : 0x400000);
    put64(bytes + 96, 512);
    put64(bytes + 104, 8192);
    put64(bytes + 112, 4096);
    put32(bytes + 256, 0xd65f03c0);
}
static void faulting_guest(CPU* cpu, uint64_t entry) {
    (void)entry;
    cpu->x[0] = *(volatile unsigned char*)(cpu->mem + 0x80000);
}
int main(int argc, char** argv) {
    GnuProcess* p = calloc(1, sizeof *p);
    CHECK(p);
    if (argc > 1) { /* --host-page=N, as the runtime's option: a host with N-byte pages (gnuabi_memory_64k_pages) */
        CHECK(argc == 2 && !strncmp(argv[1], "--host-page=", 12));
        gnu_memory_host_page(strtoull(argv[1] + 12, NULL, 10));
    }
    CHECK(gnu_memory_init(&p->memory) == 0);
    CHECK(gnu_memory_map(&p->memory, 0x10000, 8192, 3, 0) == 0);
    const char marker[] = "test";
    char copy[5];
    CHECK(gnu_memory_write(&p->memory, 0x10ffe, marker, sizeof marker) == 0);
    CHECK(gnu_memory_read(&p->memory, 0x10ffe, copy, sizeof copy) == 0 && !memcmp(copy, marker, sizeof copy));
    CHECK(gnu_memory_protect(&p->memory, 0x11000, 4096, 1) == 0);
    CHECK(gnu_memory_write(&p->memory, 0x10ffe, marker, sizeof marker) == -GNU_EFAULT);
    CHECK(gnu_memory_access(&p->memory, UINT64_MAX, 2, 1) == NULL);
    CHECK(gnu_memory_protect(&p->memory, 0x30000, 0, 1) == 0);
    CHECK(gnu_memory_protect(&p->memory, 0x30001, 0, 1) == -GNU_EINVAL);
    CHECK(gnu_memory_unmap(&p->memory, 0x11000, 4096) == 0);
    CHECK(gnu_memory_read(&p->memory, 0x11000, copy, 1) == -GNU_EFAULT);
    CHECK(gnu_memory_mmap(&p->memory, 0, 4096, 3, 0x22) >= 0x40000000);
    CHECK(gnu_memory_mmap(&p->memory, 0x10000, 4096, 3, 0x100022) == -GNU_EEXIST);
    CHECK(gnu_memory_mmap(&p->memory, 0x10001, 4096, 3, 0x32) == -GNU_EINVAL);
    CHECK(gnu_memory_unmap(&p->memory, 0x10000, 4096) == 0);
    gnu_memory_destroy(&p->memory);
#if defined(__linux__)
    /* Guest pages sharing a host page (larger than 4 KiB here, or with --host-page): it is as permissive as the
       most permissive of them, and less again when that one changes */
    CHECK(gnu_memory_init(&p->memory) == 0);
    if (p->memory.host_page > 4096) {
        unsigned char* host = p->memory.base + 0x100000;
        CHECK(gnu_memory_map(&p->memory, 0x100000, 4096, 1, 0) == 0);
        CHECK(!strcmp(host_protection(host), "r-"));
        CHECK(gnu_memory_map(&p->memory, 0x101000, 4096, 3, 0) == 0);
        CHECK(!strcmp(host_protection(host), "rw"));
        CHECK(gnu_memory_write(&p->memory, 0x100000, marker, 1) == -GNU_EFAULT); /* the guest's own still say no */
        CHECK(gnu_memory_protect(&p->memory, 0x101000, 4096, 1) == 0);
        CHECK(!strcmp(host_protection(host), "r-"));
        CHECK(gnu_memory_unmap(&p->memory, 0x100000, 4096) == 0);
        CHECK(!strcmp(host_protection(host), "r-") && gnu_memory_read(&p->memory, 0x100000, copy, 1) == -GNU_EFAULT);
        CHECK(gnu_memory_unmap(&p->memory, 0x101000, 4096) == 0);
        CHECK(!strcmp(host_protection(host), "--"));
    }
    gnu_memory_destroy(&p->memory);
#endif
    unsigned char bytes[512];
    executable(bytes, 2);
    char error[256] = {0};
    CHECK(gnu_memory_init(&p->memory) == 0);
    CHECK(gnu_elf_load_image(&p->elf, &p->memory, bytes, sizeof bytes, error, sizeof error) == 0);
    CHECK(p->elf.entry == 0x400100 && p->elf.phdr == 0x400040 && p->elf.code_count == 1);
    CHECK(gnu_memory_access(&p->memory, 0x400100, 4, 4) != NULL);
    CHECK(gnu_memory_write(&p->memory, 0x400100, marker, 1) == -GNU_EFAULT);
    unsigned char bss[16];
    memset(bss, 1, sizeof bss);
    CHECK(gnu_memory_read(&p->memory, 0x401000, bss, sizeof bss) == 0);
    for (size_t i = 0; i < sizeof bss; i++) CHECK(bss[i] == 0);
    const char* args[] = {"/program", "argument"};
    const char* env[] = {"KEY=value", NULL};
    CHECK(gnu_process_stack(p, 2, args, env) == 0);
    CHECK(!(p->cpu.x[31] & 15));
    uint64_t stack[7];
    CHECK(gnu_memory_read(&p->memory, p->cpu.x[31], stack, sizeof stack) == 0);
    for (size_t i = 0; i < 7; i++) stack[i] = snx_le64(stack[i]); /* the guest's words, little-endian on any host */
    CHECK(stack[0] == 2 && stack[3] == 0 && stack[5] == 0 && stack[6] == 3);
    CHECK(gnu_memory_read_string(&p->memory, stack[1], error, sizeof error) == 0 && !strcmp(error, "/program"));
    CHECK(gnu_memory_read_string(&p->memory, stack[4], error, sizeof error) == 0 && !strcmp(error, "KEY=value"));
    uint64_t initial = p->memory.brk_current;
    CHECK(gnu_memory_brk(&p->memory, initial + 5000) == initial + 5000);
    CHECK(gnu_memory_brk(&p->memory, UINT64_MAX) == initial + 5000);
    CHECK(gnu_memory_brk(&p->memory, initial) == initial);
    CHECK(gnu_vfs_init(&p->vfs) == 0);
    p->pid = p->tid = 1000;
    p->cpu.thread = p;
    uint64_t a[6] = {0};
    CHECK(gnu_syscall(p, UINT64_MAX, a) == -GNU_ENOSYS);
    CHECK(gnu_syscall(p, 172, a) == 1000);
    CHECK(gnu_syscall(p, 0x1000000acull, a) == 1000);
    a[0] = p->cpu.x[31] - 512;
    CHECK(gnu_syscall(p, 160, a) == 0);
    CHECK(gnu_memory_read(&p->memory, a[0] + 4 * 65, error, 65) == 0 && !strcmp(error, "aarch64"));
    a[0] = 0;
    a[1] = p->cpu.x[31] - 64;
    CHECK(gnu_syscall(p, 113, a) == 0);
    a[0] = p->cpu.x[31] - 128;
    a[1] = 16;
    a[2] = 0;
    CHECK(gnu_syscall(p, 278, a) == 16);
    uint32_t futex_value = snx_le32(123);
    CHECK(gnu_memory_write(&p->memory, a[0], &futex_value, 4) == 0);
    a[0] = p->cpu.x[31] - 128;
    a[1] = 1;
    a[2] = 0;
    CHECK(gnu_syscall(p, 98, a) == 0);
    a[1] = 0;
    a[2] = 124;
    CHECK(gnu_syscall(p, 98, a) == -GNU_EAGAIN);
    a[0] = 1;
    a[1] = 0;
    a[2] = 0;
    a[3] = 7;
    CHECK(gnu_syscall(p, 134, a) == -GNU_EINVAL);
    a[0] = 42;
    gnu_syscall(p, 93, a);
    CHECK(p->exited && p->exit_code == 42);
    p->cpu.mem = p->memory.base;
    p->exited = 0;
    CHECK(gnu_jit_init(&p->jit, NULL, error, sizeof error) == 0);
    gnu_jit_bind(&p->jit, &p->cpu);
    gnu_jit_install(&p->jit, 0x400100, faulting_guest);
    p->cpu.pc = 0x400100;
    CHECK(gnu_runtime_run(p) == 139);
    CHECK(p->faulted == 11 && strstr(p->diagnostic, "mapped permissions"));
    gnu_process_destroy(p);
    gnu_jit_destroy(&p->jit);
    memset(p, 0, sizeof *p);
    executable(bytes, 3);
    CHECK(gnu_memory_init(&p->memory) == 0);
    CHECK(gnu_elf_load_image(&p->elf, &p->memory, bytes, sizeof bytes, error, sizeof error) == 0);
    CHECK(p->elf.entry == 0x10000100 && p->elf.phdr == 0x10000040);
    gnu_elf_destroy(&p->elf);
    gnu_memory_destroy(&p->memory);
    executable(bytes, 2);
    put64(bytes + 104, UINT64_MAX);
    CHECK(gnu_memory_init(&p->memory) == 0);
    CHECK(gnu_elf_load_image(&p->elf, &p->memory, bytes, sizeof bytes, error, sizeof error) < 0);
    CHECK(strstr(error, "PT_LOAD") != NULL);
    /* a refused header says the field it read */
    executable(bytes, 2);
    put16(bytes + 18, 62);
    CHECK(gnu_elf_load_image(&p->elf, &p->memory, bytes, sizeof bytes, error, sizeof error) < 0);
    CHECK(!strcmp(error, "ELF machine 62, not 183"));
    executable(bytes, 2);
    bytes[4] = 3;
    CHECK(gnu_elf_load_image(&p->elf, &p->memory, bytes, sizeof bytes, error, sizeof error) < 0);
    CHECK(!strcmp(error, "ELF class 3 is neither 1 (32-bit) nor 2 (64-bit)"));
    executable(bytes, 2);
    put16(bytes + 56, 2); /* PT_LOAD, then PT_INTERP */
    put32(bytes + 120, 3);
    put64(bytes + 128, 300);
    put64(bytes + 152, 11);
    put64(bytes + 160, 11);
    memcpy(bytes + 300, "/lib/ld.so", 11);
    CHECK(gnu_elf_load_image(&p->elf, &p->memory, bytes, sizeof bytes, error, sizeof error) == 0);
    CHECK(!strcmp(p->elf.interp, "/lib/ld.so")); /* the dynamic linker the process loads beside it */
    gnu_elf_destroy(&p->elf);
    gnu_memory_destroy(&p->memory);
    free(p);
    puts("GNU ABI memory, ELF, stack, syscall tests passed");
    return 0;
}
