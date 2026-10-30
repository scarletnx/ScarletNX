/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Windows platform layer's written-page tracking (platform/plat_win32.c): the reservation's write watch reports
   the pages written since the last scan, the kernel's writes (ReadFile into guest memory) among them, and SNX_NO_WW
   must make the scan say -1 */
#include "nx/runtime/platform/plat_win32.c"
#include <stdarg.h>
#include <stdio.h>

void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(2);
}

void log_crash_mode(void) {}
void crash_report(void* host_pc, void* fault, int sig) {
    (void)host_pc;
    (void)fault;
    (void)sig;
}
void plat_prof_add_thread(HANDLE h) { (void)h; }

static unsigned checks, failures;
#define CHECK(c, what)                                                                                                 \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            failures++;                                                                                                \
            printf("FAIL line %d: %s\n", __LINE__, what);                                                              \
        }                                                                                                              \
    } while (0)

static u8* g_base;
static u64 g_written; /* bit n: page n reported written */
static void on_written(u8* p, u64 size) {
    u64 a;
    for (a = (u64)(p - g_base) >> 12; a < (u64)(p - g_base + size) >> 12; a++)
        if (a < 64) g_written |= 1ull << a;
}
static u64 scan(void) {
    g_written = 0;
    int status = plat_ww_scan(g_base, 64 * 4096, on_written);
    printf("write-watch scan: status=%d pages=%016llx\n", status, (unsigned long long)g_written);
    return status ? ~0ull : g_written;
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : ".";
    char path[1200];
    HANDLE h;
    DWORD got = 0;
    FILE* f;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("written-page tracking\n");
    g_base = plat_vm_reserve((u64)1 << 30);
    CHECK(g_base != NULL, "no reservation");
    CHECK(!plat_vm_commit(g_base, 64 * 4096), "commit failed");
    if (getenv("SNX_NO_WW")) {
        CHECK(scan() == ~0ull, "a scan without tracking did not fail");
    } else {
        scan();
        CHECK(scan() == 0, "pages reported written twice");
        g_base[3 * 4096] = 1;
        g_base[10 * 4096 + 77] = 2;
        CHECK(scan() == (1ull << 3 | 1ull << 10), "pages 3 and 10 not reported alone");
        CHECK(scan() == 0, "a scan did not watch its pages again");
        /* A first read can fault in a demand-zero page that Windows reports as written. Clear that first-touch
           report before checking that another read of the resident page does not dirty it. */
        (void)*(volatile u8*)&g_base[20 * 4096];
        u64 first_touch = scan();
        CHECK((first_touch & ~(1ull << 20)) == 0, "a first read reported unrelated pages");
        (void)*(volatile u8*)&g_base[20 * 4096];
        g_base[5 * 4096] = 3;
        CHECK(scan() == 1ull << 5, "page 5 not reported alone after a read elsewhere");
        CHECK(scan() == 0, "pages reported again after the resident-page read");
        /* the kernel writing guest memory (ReadFile into it) counts too */
        snprintf(path, sizeof path, "%s/platform_win32.bin", dir);
        f = fopen(path, "wb");
        CHECK(f != NULL, "no test file");
        if (f) {
            fwrite("0123456789", 1, 10, f);
            fclose(f);
        }
        h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        plat_ww_open(g_base + 7 * 4096, 10);
        CHECK(h != INVALID_HANDLE_VALUE && ReadFile(h, g_base + 7 * 4096, 10, &got, NULL) && got == 10,
              "ReadFile into guest memory failed");
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        CHECK(scan() == 1ull << 7, "a ReadFile into page 7 was not reported");
        remove(path);
    }
    printf("decommit\n");
    g_base[9 * 4096] = 9;
    plat_vm_decommit(g_base, 64 * 4096);
    CHECK(!plat_vm_commit(g_base, 64 * 4096) && g_base[9 * 4096] == 0, "memory not zero after decommit");
    printf("%u checks, %u failed\n", checks, failures);
    return failures != 0;
}
