/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* common/filesystem/host.c on its own: shared read-only files (contents, an empty file, a missing one, sizes past
   what a mapping can hold), folders, and the mutex's static initializer and trylock */
#define _POSIX_C_SOURCE 200809L
#include "common/filesystem/host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned test_checks;
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        test_checks++;                                                                                                 \
        if (!(expr)) {                                                                                                 \
            fprintf(stderr, "filesystem_host.c:%d: check failed: %s\n", __LINE__, #expr);                              \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

static PMutex test_lock = PMUTEX_INIT; /* usable before any init runs */

static void write_file(const char* path, const char* text, size_t size) {
    FILE* f = fopen(path, "wb");
    CHECK(f != NULL);
    CHECK(fwrite(text, 1, size, f) == size);
    CHECK(!fclose(f));
}

int main(void) {
    char dir[64], full[96], empty[96];
    int fd;
    const uint8_t* data;
    PMutex m;
    sprintf(dir, "snx-host-test-%ld", (long)getpid());
    CHECK(!plat_mkdir(dir));
    CHECK(plat_mkdir(dir) != 0); /* there already */
    sprintf(full, "%s/full.bin", dir);
    sprintf(empty, "%s/empty.bin", dir);
    write_file(full, "shared bytes", 12);
    write_file(empty, "", 0);

    CHECK(!plat_file_share(full, 12, &fd, &data));
    CHECK(fd >= 0 && data != NULL && !memcmp(data, "shared bytes", 12));
    plat_file_unshare(fd, data, 12);
    /* an empty file keeps its descriptor and has no mapping */
    CHECK(!plat_file_share(empty, 0, &fd, &data));
    CHECK(fd >= 0 && data == NULL);
    plat_file_unshare(fd, data, 0);
    /* failures leave neither */
    fd = 5;
    data = (const uint8_t*)dir;
    CHECK(plat_file_share("snx-host-test-missing/none.bin", 1, &fd, &data) != 0 && fd == -1 && data == NULL);
    fd = 5;
    CHECK(plat_file_share(full, (uint64_t)INT64_MAX + 1, &fd, &data) != 0 && fd == -1 && data == NULL);

    CHECK(!remove(full) && !remove(empty) && !rmdir(dir));

    pm_lock(&test_lock);
    CHECK(!pm_trylock(&test_lock));
    pm_unlock(&test_lock);
    CHECK(pm_trylock(&test_lock));
    pm_unlock(&test_lock);
    pm_init(&m);
    CHECK(pm_trylock(&m));
    pm_unlock(&m);
    pm_destroy(&m);
    printf("filesystem_host: %u checks passed\n", test_checks);
    return 0;
}
