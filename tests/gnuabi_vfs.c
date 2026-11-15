/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gnuabi/filesystem/vfs.h"
#include "gnuabi/abi/errno.inc"
#include <stdio.h>
#include <string.h>

static GnuVfs vfs;
static GnuMemory memory;
static unsigned failures;
static void check(int value, const char* message) {
    if (!value) {
        fprintf(stderr, "GNU VFS: %s\n", message);
        ++failures;
    }
}
static int64_t call(uint64_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    uint64_t arguments[6] = {a, b, c, d, 0, 0};
    int64_t result = 0;
    check(gnu_vfs_syscall(&vfs, &memory, n, arguments, &result), "syscall is handled");
    return result;
}
static uint64_t path(const char* text) {
    check(!gnu_memory_write(&memory, 0x10000, text, strlen(text) + 1), "copy pathname");
    return 0x10000;
}
static uint64_t read64(const unsigned char* p) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i) result |= (uint64_t)p[i] << (i * 8);
    return result;
}
static void put64(unsigned char* p, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) p[i] = (unsigned char)(value >> (i * 8));
}
int main(void) {
    check(!gnu_memory_init(&memory), "initialize guest memory");
    check(!gnu_memory_map(&memory, 0x10000, 0x10000, GNU_PROT_READ | GNU_PROT_WRITE, 0), "map guest buffers");
    check(!gnu_vfs_init(&vfs), "initialize filesystem");
    const char* argv[] = {"/test/program", "argument"};
    check(!gnu_vfs_set_process(&vfs, 1234, "/test/program", 2, argv), "process metadata");
    check(!gnu_vfs_set_executable(&vfs, "ELF-test", 8), "executable image");
    const uint64_t cwd = (uint64_t)-100;
    check(call(56, cwd, path("/etc/passwd"), 0, 0) == -GNU_ENOENT, "host files stay outside guest namespace");
    check(call(56, cwd, path("/tmp/../../etc/passwd"), 0, 0) == -GNU_ENOENT, "parent traversal stays in guest root");
    check(call(34, cwd, path("/tmp/work"), 0700, 0) == 0, "mkdirat tmpfs");
    int directory = (int)call(56, cwd, path("/tmp/work"), 16384, 0);
    check(directory >= 3, "AArch64 O_DIRECTORY flag");
    int fd = (int)call(56, (uint64_t)directory, path("file"), 64 | 2, 0600);
    check(fd >= 3, "directory-relative openat");
    check(!gnu_memory_write(&memory, 0x12000, "abcdef", 6), "copy write buffer");
    check(call(64, (uint64_t)fd, 0x12000, 6, 0) == 6, "write tmpfs");
    check(call(62, (uint64_t)fd, 0, 0, 0) == 0, "rewind tmpfs");
    int duplicate = (int)call(23, (uint64_t)fd, 0, 0, 0);
    check(duplicate >= 3, "dup descriptor");
    check(call(63, (uint64_t)duplicate, 0x14000, 2, 0) == 2, "read duplicated descriptor");
    char bytes[128] = {0};
    gnu_memory_read(&memory, 0x14000, bytes, 2);
    check(!memcmp(bytes, "ab", 2), "dup starts at shared position");
    check(call(63, (uint64_t)fd, 0x14000, 2, 0) == 2, "read original descriptor");
    gnu_memory_read(&memory, 0x14000, bytes, 2);
    check(!memcmp(bytes, "cd", 2), "dup shares open-file offset");
    check(call(63, (uint64_t)fd, 0x30000, 1, 0) == -GNU_EFAULT, "unmapped read buffer faults safely");
    check(call(62, (uint64_t)fd, 0, 1, 0) == 4, "fault preserves file position");
    check(call(80, (uint64_t)fd, 0x14000, 0, 0) == 0, "fstat");
    gnu_memory_read(&memory, 0x14000, bytes, 128);
    check(read64((unsigned char*)bytes + 48) == 6, "AArch64 stat size offset");
    check(((unsigned char)bytes[16] | ((unsigned char)bytes[17] << 8)) == (0100000 | 0600), "AArch64 stat mode offset");
    check(call(79, (uint64_t)fd, path(""), 0x14000, 4096) == 0, "newfstatat AT_EMPTY_PATH");
    check(call(35, (uint64_t)directory, path("file"), 0, 0) == 0, "unlink tmpfs");
    check(call(56, (uint64_t)directory, path("file"), 0, 0) == -GNU_ENOENT, "unlinked pathname disappears");
    check(call(62, (uint64_t)fd, 0, 0, 0) == 0 && call(63, (uint64_t)fd, 0x14000, 6, 0) == 6,
          "unlinked open file remains readable");
    char descriptor_path[128];
    snprintf(descriptor_path, sizeof descriptor_path, "/proc/self/fd/%d", fd);
    int64_t link_length = call(78, cwd, path(descriptor_path), 0x14000, 127);
    check(link_length > 0, "proc descriptor readlink");
    if (link_length > 0 && link_length < 128) {
        gnu_memory_read(&memory, 0x14000, bytes, (size_t)link_length);
        bytes[link_length] = 0;
        check(!strcmp(bytes, "/tmp/work/file (deleted)"), "guest descriptor target and deleted suffix");
    }
    check(call(25, (uint64_t)fd, 2, 1, 0) == 0, "set descriptor cloexec");
    check(call(25, (uint64_t)fd, 1, 0, 0) == 1 && call(25, (uint64_t)duplicate, 1, 0, 0) == 0,
          "cloexec belongs to descriptor");
    check(call(25, (uint64_t)fd, 4, 1024, 0) == 0 && (call(25, (uint64_t)duplicate, 3, 0, 0) & 1024),
          "append belongs to shared open file");
    check(call(46, (uint64_t)fd, 10, 0, 0) == 0, "ftruncate extends tmpfs");
    check(call(62, (uint64_t)fd, 6, 0, 0) == 6 && call(63, (uint64_t)fd, 0x14000, 4, 0) == 4, "read extended tmpfs");
    gnu_memory_read(&memory, 0x14000, bytes, 4);
    check(bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 0, "extension is zero-filled");
    check(call(61, (uint64_t)directory, 0x14000, 128, 0) > 0, "getdents64 directory");
    gnu_memory_read(&memory, 0x14000, bytes, 24);
    check((unsigned char)bytes[18] == 4 && !strcmp(bytes + 19, "."), "linux_dirent64 offset and DT_DIR");
    check(call(50, (uint64_t)directory, 0, 0, 0) == 0, "fchdir");
    check(call(17, 0x14000, 128, 0, 0) == 10, "getcwd includes terminating NUL");
    gnu_memory_read(&memory, 0x14000, bytes, 10);
    check(!strcmp(bytes, "/tmp/work"), "guest cwd");
    check(call(49, path("/"), 0, 0, 0) == 0, "chdir root");
    check(call(35, cwd, path("/tmp/work/."), 512, 0) == -GNU_EINVAL, "rmdir dot rejects invalid operation");
    int zero = (int)call(56, cwd, path("/dev/zero"), 2, 0);
    memset(bytes, 0xff, 32);
    gnu_memory_write(&memory, 0x14000, bytes, 32);
    check(call(63, (uint64_t)zero, 0x14000, 32, 0) == 32, "dev zero read");
    gnu_memory_read(&memory, 0x14000, bytes, 32);
    for (unsigned i = 0; i < 32; ++i) check(bytes[i] == 0, "dev zero data");
    check(call(64, (uint64_t)zero, 0x30000, 32, 0) == 32, "zero discards buffer without copying");
    int null = (int)call(56, cwd, path("/dev/null"), 2, 0);
    check(call(63, (uint64_t)null, 0x30000, 32, 0) == 0, "null read does not touch guest buffer");
    check(call(64, (uint64_t)null, 0x30000, 32, 0) == 32, "null write discards bytes");
    check(call(78, cwd, path("/proc/self/exe"), 0x14000, 5) == 5, "readlink truncates without NUL");
    gnu_memory_read(&memory, 0x14000, bytes, 5);
    check(!memcmp(bytes, "/test", 5), "proc exe path");
    int executable = (int)call(56, cwd, path("/proc/self/exe"), 0, 0);
    check(call(63, (uint64_t)executable, 0x14000, 8, 0) == 8, "proc exe reads imported executable");
    gnu_memory_read(&memory, 0x14000, bytes, 8);
    check(!memcmp(bytes, "ELF-test", 8), "executable bytes");
    int guest_executable = (int)call(56, cwd, path("/test/program"), 0, 0);
    check(call(63, (uint64_t)guest_executable, 0x14000, 8, 0) == 8, "readlink target resolves in guest namespace");
    check(call(56, cwd, path("/proc/self/exe"), 32768, 0) == -GNU_ELOOP, "AArch64 O_NOFOLLOW rejects proc exe symlink");
    int pathfd = (int)call(56, cwd, path("/proc/self/exe"), 2097152 | 32768, 0);
    check(pathfd >= 0 && call(78, (uint64_t)pathfd, path(""), 0x14000, 128) == 13,
          "readlinkat empty path for O_PATH symlink");
    check(call(56, cwd, path("/tmp/path-must-not-create"), 2097152 | 64, 0600) == -GNU_ENOENT,
          "O_PATH ignores O_CREAT");
    check(call(63, (uint64_t)pathfd, 0x14000, 1, 0) == -GNU_EBADF, "O_PATH descriptor is not readable");
    int cmdline = (int)call(56, cwd, path("/proc/1234/cmdline"), 0, 0);
    check(call(63, (uint64_t)cmdline, 0x14000, 128, 0) == 23, "proc cmdline uses guest argv");
    gnu_memory_read(&memory, 0x14000, bytes, 23);
    check(!memcmp(bytes, "/test/program\0argument\0", 23), "NUL-delimited cmdline");
    int maps = (int)call(56, cwd, path("/proc/self/maps"), 0, 0);
    check(call(63, (uint64_t)maps, 0x14000, 128, 0) > 0, "proc maps contains guest mappings");
    check(call(56, cwd, path("/proc/self/status"), 1, 0) == -GNU_EACCES, "procfs is readonly");
    check(call(56, cwd, path("/dev/new"), 64 | 2, 0600) == -GNU_EROFS, "devfs disallows guest-created devices");
    int vectorfd = (int)call(56, cwd, path("/tmp/vector"), 64 | 2, 0600);
    unsigned char vectors[32];
    put64(vectors, 0x12000);
    put64(vectors + 8, 2);
    put64(vectors + 16, 0x12002);
    put64(vectors + 24, 4);
    gnu_memory_write(&memory, 0x16000, vectors, sizeof vectors);
    check(call(66, (uint64_t)vectorfd, 0x16000, 2, 0) == 6, "writev translates guest 64-bit iovec");
    check(call(62, (uint64_t)vectorfd, 0, 0, 0) == 0, "rewind vector file");
    put64(vectors, 0x14000);
    put64(vectors + 8, 2);
    put64(vectors + 16, 0x14002);
    put64(vectors + 24, 4);
    gnu_memory_write(&memory, 0x16000, vectors, sizeof vectors);
    check(call(65, (uint64_t)vectorfd, 0x16000, 2, 0) == 6, "readv translates guest 64-bit iovec");
    gnu_memory_read(&memory, 0x14000, bytes, 6);
    check(!memcmp(bytes, "abcdef", 6), "vectored data roundtrip");
    check(call(57, (uint64_t)fd, 0, 0, 0) == 0 && call(57, (uint64_t)fd, 0, 0, 0) == -GNU_EBADF,
          "close descriptor semantics");
    int held[256];
    size_t held_count = 0;
    int64_t duplicate_result = 0;
    while (held_count < 256) {
        duplicate_result = call(23, (uint64_t)vectorfd, 0, 0, 0);
        if (duplicate_result < 0) break;
        held[held_count++] = (int)duplicate_result;
    }
    check(duplicate_result == -GNU_EMFILE, "guest descriptor table reports EMFILE");
    check(call(56, cwd, path("/tmp/exhausted-create"), 64 | 2, 0600) == -GNU_EMFILE,
          "openat enforces descriptor limit");
    for (size_t i = 0; i < held_count; ++i)
        check(call(57, (uint64_t)held[i], 0, 0, 0) == 0, "release exhausted descriptors");
    check(call(56, cwd, path("/tmp/exhausted-create"), 0, 0) == -GNU_ENOENT, "EMFILE does not create a pathname");
    gnu_vfs_destroy(&vfs);
    gnu_memory_destroy(&memory);
    if (!failures) puts("GNU VFS isolation, descriptor, ABI layout, devfs/tmpfs/procfs checks passed");
    return failures ? 1 : 0;
}
