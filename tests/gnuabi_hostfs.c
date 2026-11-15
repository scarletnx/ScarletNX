/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host-backed namespace (gnu_vfs_set_root): a guest path names the sysroot's file first and the host's otherwise,
   a symlink with an absolute target stays in the sysroot, /proc and /dev stay virtual, pipes are host pipes */
#include "gnuabi/filesystem/vfs.h"
#include "gnuabi/abi/errno.inc"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

static GnuVfs vfs;
static GnuMemory memory;
static unsigned failures;
static void check(int value, const char* message) {
    if (!value) {
        fprintf(stderr, "GNU hostfs: %s\n", message);
        ++failures;
    }
}
static int64_t call(uint64_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    uint64_t arguments[6] = {a, b, c, d, 0, 0};
    int64_t result = 0;
    check(gnu_vfs_syscall(&vfs, &memory, n, arguments, &result), "syscall is handled");
    return result;
}
static uint64_t path_at(uint64_t address, const char* text) {
    check(!gnu_memory_write(&memory, address, text, strlen(text) + 1), "copy pathname");
    return address;
}
static uint64_t path(const char* text) { return path_at(0x10000, text); }
static void write_file(const char* file, const char* text) {
    check(!snx_write_file(file, text, strlen(text)), "write a host file");
}
/* the first bytes of the guest file at path, "" when it can't be opened */
static const char* contents(const char* name) {
    static char text[64];
    const uint64_t cwd = (uint64_t)-100;
    int64_t fd = call(56, cwd, path(name), 0, 0), n;
    text[0] = 0;
    if (fd < 0) return text;
    n = call(63, (uint64_t)fd, 0x12000, sizeof text - 1, 0);
    if (n > 0 && !gnu_memory_read(&memory, 0x12000, text, (size_t)n)) text[n] = 0;
    call(57, (uint64_t)fd, 0, 0, 0);
    return text;
}
int main(void) {
    const uint64_t cwd = (uint64_t)-100;
    char *dir = snx_temp_dir(NULL), *root, *host, *file, *guest_host;
    char text[256];
    int64_t r;
    check(dir != NULL, "temporary folder");
    if (!dir) return 1;
    root = snx_path_join(dir, "root");
    host = snx_path_join(dir, "host");
    file = snx_path_join(root, "etc");
    check(!snx_mkdirs(file), "sysroot etc");
    free(file);
    file = snx_path_join(root, "lib");
    check(!snx_mkdirs(file), "sysroot lib");
    free(file);
    check(!snx_mkdirs(host), "host folder");
    file = snx_path_join(root, "etc/marker");
    write_file(file, "sysroot");
    free(file);
    file = snx_path_join(root, "lib/libx");
    write_file(file, "library");
    free(file);
    file = snx_path_join(host, "outside");
    write_file(file, "host");
    free(file);
#ifndef _WIN32
    /* lib64 -> /lib, as glibc's sysroots have it: /lib is the sysroot's, not the host's */
    file = snx_path_join(root, "lib64");
    check(!symlink("/lib", file), "absolute symlink in the sysroot");
    free(file);
#endif
    check(!gnu_memory_init(&memory), "initialize guest memory");
    check(!gnu_memory_map(&memory, 0x10000, 0x10000, GNU_PROT_READ | GNU_PROT_WRITE, 0), "map guest buffers");
    check(!gnu_vfs_init(&vfs), "initialize filesystem");
    check(!gnu_vfs_set_root(&vfs, root), "set the sysroot");

    check(!strcmp(contents("/etc/marker"), "sysroot"), "a sysroot file by its guest path");
    check(!strcmp(contents("/etc/../etc/./marker"), "sysroot"), "dot and dot-dot components");
    guest_host = snx_path_join(host, "outside");
    check(!strcmp(contents(guest_host), "host"), "a host file the sysroot lacks");
#ifndef _WIN32
    check(!strcmp(contents("/lib64/libx"), "library"), "an absolute symlink stays in the sysroot");
    r = call(78, cwd, path("/lib64"), 0x12000, 64);
    check(r == 4 && !gnu_memory_read(&memory, 0x12000, text, 4) && !memcmp(text, "/lib", 4), "readlink's target");
#endif
    check(call(56, cwd, path("/etc/missing"), 0, 0) == -GNU_ENOENT, "a missing file");
    check(call(56, cwd, path("/etc/marker/x"), 0, 0) == -GNU_ENOTDIR, "a file as a folder");

    /* made where its folder is: the sysroot's /etc */
    r = call(56, cwd, path("/etc/made"), 64 | 1, 0644);
    check(r >= 0, "create a file in the sysroot");
    check(call(64, (uint64_t)r, path_at(0x11000, "new"), 3, 0) == 3, "write it");
    check(call(57, (uint64_t)r, 0, 0, 0) == 0, "close it");
    file = snx_path_join(root, "etc/made");
    check(snx_is_file(file), "the file is the sysroot's");
    free(file);
    check(call(35, cwd, path("/etc/made"), 0, 0) == 0, "unlink it");
    check(call(34, cwd, path("/etc/folder"), 0755, 0) == 0, "mkdir");
    check(call(34, cwd, path("/etc/folder"), 0755, 0) == -GNU_EEXIST, "mkdir of an existing folder");
    check(call(35, cwd, path("/etc/folder"), 512, 0) == 0, "rmdir");

    /* the working folder is a guest path; the sysroot's root is "/" */
    check(call(49, path("/etc"), 0, 0, 0) == 0, "chdir into the sysroot");
    r = call(17, 0x12000, 256, 0, 0);
    check(r == 5 && !gnu_memory_read(&memory, 0x12000, text, 5) && !strcmp(text, "/etc"), "getcwd");
    check(!strcmp(contents("marker"), "sysroot"), "a relative path from there");
    check(call(49, path("/proc/self"), 0, 0, 0) == 0, "chdir into the virtual nodes");
    r = call(17, 0x12000, 256, 0, 0);
    check(r > 0 && !gnu_memory_read(&memory, 0x12000, text, (size_t)r) && !strcmp(text, "/proc/self"),
          "getcwd in the virtual nodes");
    check(call(49, path("/"), 0, 0, 0) == 0, "chdir to the root");

    /* stat: the sysroot's file, statx's layout */
    check(call(79, cwd, path("/etc/marker"), 0x13000, 0) == 0, "newfstatat");
    check(!gnu_memory_read(&memory, 0x13000 + 48, text, 8) && text[0] == 7, "st_size");
    {
        uint64_t arguments[6] = {cwd, path("/etc/marker"), 0, 0x7ff, 0x13000, 0};
        int64_t result = -1;
        check(gnu_vfs_syscall(&vfs, &memory, 291, arguments, &result) && result == 0, "statx");
        check(!gnu_memory_read(&memory, 0x13000 + 40, text, 8) && text[0] == 7, "stx_size");
    }

    /* /dev stays virtual, and /proc/self/fd/N of a host file leads to it */
    check(call(56, cwd, path("/dev/null"), 1, 0) >= 0, "the virtual /dev/null");
    r = call(56, cwd, path("/etc/marker"), 0, 0);
    snprintf(text, sizeof text, "/proc/self/fd/%d", (int)r);
    check(!strcmp(contents(text), "sysroot"), "a host descriptor's file through /proc/self/fd");
    {
        int64_t n = call(78, cwd, path(text), 0x12000, 64);
        char target[64] = {0};
        check(n == 11 && !gnu_memory_read(&memory, 0x12000, target, 11) && !strcmp(target, "/etc/marker"),
              "readlink of /proc/self/fd/N is the guest path");
    }

    /* a pipe: what one end writes, the other reads, and the read end sees end of file once the writer is gone */
    {
        int fds[2];
        check(!gnu_vfs_pipe(&vfs, 0, fds), "pipe2");
        check(call(64, (uint64_t)fds[1], path_at(0x11000, "piped"), 5, 0) == 5, "write into the pipe");
        check(call(57, (uint64_t)fds[1], 0, 0, 0) == 0, "close the write end");
        check(call(63, (uint64_t)fds[0], 0x12000, 16, 0) == 5, "read from the pipe");
        check(call(63, (uint64_t)fds[0], 0x12000, 16, 0) == 0, "end of file");
        check(call(62, (uint64_t)fds[0], 0, 0, 0) == -GNU_ESPIPE, "a pipe does not seek");
    }

    gnu_vfs_destroy(&vfs);
    gnu_memory_destroy(&memory);
    snx_remove_tree(dir);
    free(guest_host);
    free(root);
    free(host);
    free(dir);
    if (!failures) puts("GNU host-backed namespace checks passed");
    return failures != 0;
}
