/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Windows platform layer's written-page tracking (platform/plat_win32.c): the reservation's write watch reports
   the pages written since the last scan, the kernel's writes (ReadFile into guest memory) among them, and
   write_tracking=2 must make the scan say -1. write_tracking=1 has no Windows version: the watch tracks, said once.
   The arguments: the folder for its files, then Debug settings as the runtime takes them, --key=value */
#include "nx/runtime/platform/plat_win32.c"
#include "nx/runtime/ini.h"
#include <stdarg.h>
#include <stdio.h>

static char g_log[1024]; /* the log's lines, as far as they fit */
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    size_t n = strlen(g_log);
    (void)lvl;
    va_start(ap, fmt);
    vsnprintf(g_log + n, sizeof g_log - n, fmt, ap);
    va_end(ap);
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
    for (int i = 2; i < argc; i++) {
        const char* eq = strchr(argv[i], '=');
        char key[64];
        if (strncmp(argv[i], "--", 2) || !eq || (size_t)(eq - argv[i] - 2) >= sizeof key) {
            printf("not --key=value: %s\n", argv[i]);
            return 2;
        }
        snprintf(key, sizeof key, "%.*s", (int)(eq - argv[i] - 2), argv[i] + 2);
        if (!ini_find(g_debug_ini, key)) {
            printf("not a Debug setting: %s\n", argv[i]);
            return 2;
        }
        ini_read(ini_find(g_debug_ini, key), eq + 1);
    }
    printf("host_page\n");
    {
        int given = g_settings.host_page_size;
        g_settings.host_page_size = 65536;
        CHECK(plat_vm_page() == 65536, "host_page=65536 is not the page");
        g_log[0] = 0;
        g_settings.host_page_size = 12288;
        CHECK(plat_vm_page() == 4096, "host_page=12288 is taken");
        CHECK(!strcmp(g_log, "memory: host_page=12288 is not a power of two from the host's page, 4096 bytes, to "
                             "1048576: the runtime works in 4096-byte pages\n"),
              "host_page's refusal does not name the value given and the page in effect");
        g_settings.host_page_size = given;
    }
    printf("written-page tracking\n");
    g_base = plat_vm_reserve((u64)1 << 30);
    CHECK(g_base != NULL, "no reservation");
    g_log[0] = 0;
    plat_vm_track(g_base, (u64)1 << 30);
    if (g_settings.write_tracking == 1)
        CHECK(!strcmp(g_log, "memory: write_tracking=1 (page protection) has no Windows version: the reservation's "
                             "write watch tracks writes, as write_tracking=0 does\n"),
              "write_tracking=1 is not said with the tracking in effect");
    CHECK(!plat_vm_commit(g_base, 64 * 4096), "commit failed");
    if (g_settings.write_tracking == 2) {
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
