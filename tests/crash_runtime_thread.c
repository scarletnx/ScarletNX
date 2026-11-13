/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The crash report of one of the runtime's threads (src/nx/runtime/crash.c), and the note it prints
   (common/platform/thread.c's plat_thread_note). A driver that crashes compiling a pipeline on a pipeline worker gives the report its
   own library and offset only, which name no shader: the report then names the thread as the system has it and says
   what the thread noted it was doing, the pipeline and its programs; a thread with no note, a driver's own, says that
   it noted none. The note is the calling thread's alone and cut at its size. The rest of the runtime is faked here: the
   game's threads (none), its memory (not reserved) and the end of the run, which returns to the test */
#include "nx/runtime/main.h"
#include "nx/hle/filesystem/fs.h"
#include "nx/hle/hle.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/memory/mem.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/platform/log.h"
#include "common/platform/thread.h"
#include "common/utilities/common.h"
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int g_failures;
#define CHECK(ok, ...)                                                                                                 \
    do {                                                                                                               \
        if (!(ok)) {                                                                                                   \
            fprintf(stderr, "crash_runtime_thread.c:%d: %s: ", __LINE__, #ok);                                         \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

/* the faked runtime: no game thread, no module, no reservation */
u8* g_mem;
u64 g_as_size;
const SnxModule* g_mod;
KThread* kthread_cur(void) { return NULL; }
u32 arm_block_at(const void* host_pc, u64* offset) {
    (void)host_pc;
    *offset = 0;
    return 0;
}
void fs_save_report(void) {}
void kernel_dump_regs(CPU* c) { (void)c; }
void kernel_dump_libc_fault(const CPU* c, u64 fault) {
    (void)c;
    (void)fault;
}
int mem_short_hint(char* out, size_t size) {
    (void)out;
    (void)size;
    return 0;
}
void mem_report(char* out, int n) {
    if (n > 0) out[0] = 0;
}
int mem_reserved(void) { return 0; }
void mem_place(const void* p, char* out, size_t size) {
    (void)p;
    if (size) out[0] = 0;
}
int mem_foreign(char* out, size_t size) {
    (void)out;
    (void)size;
    return 0;
}

/* what the report said, and where fatal returns to */
static struct {
    char said[8192];
    size_t at;
    jmp_buf end;
} g_report;
static void say(const char* fmt, va_list ap) {
    if (g_report.at >= sizeof g_report.said) return;
    int n = vsnprintf(g_report.said + g_report.at, sizeof g_report.said - g_report.at, fmt, ap);
    if (n > 0) g_report.at += (size_t)n;
}
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    say(fmt, ap);
    va_end(ap);
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    say(fmt, ap);
    va_end(ap);
    longjmp(g_report.end, 1);
}

/* the report of a crash at host_pc reading 0x40, on the calling thread */
static const char* report(void) {
    g_report.at = 0;
    g_report.said[0] = 0;
    if (!setjmp(g_report.end)) crash_report((void*)&strrchr, (void*)0x40, SIGSEGV);
    return g_report.said;
}

#define PIPELINE_NOTE                                                                                                  \
    "making the graphics pipeline 0123456789abcdef, vs 1111111111111111 (12 KB of SPIR-V), fs 2222222222222222 (3 KB " \
    "of SPIR-V); color formats 37+blend, depth 126, stencil 0, 1 sample, topology class 3"

/* a pipeline worker: its name, and the pipeline it notes while the driver makes it */
static void* worker(void* arg) {
    char name[64], line[512];
    const char* said;
    (void)arg;
    snx_thread_name("Vulkan pipelines");
    snx_thread_name_get(name, sizeof name);
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(_WIN32)
    /* Linux and Android keep 15 bytes of a name, FreeBSD 19, the others 63 */
    CHECK(strlen(name) >= 15 && !strncmp(name, "Vulkan pipelines", strlen(name)), "the name read back is \"%s\"", name);
#endif
    plat_thread_note("making the graphics pipeline %016llx, vs %016llx (%u KB of SPIR-V), fs %016llx (%u KB of "
                     "SPIR-V); color formats %u+blend, depth %u, stencil %u, %u sample, topology class %u",
                     0x0123456789abcdefull, 0x1111111111111111ull, 12u, 0x2222222222222222ull, 3u, 37u, 126u, 0u, 1u,
                     3u);
    CHECK(!strcmp(plat_thread_note_get(), PIPELINE_NOTE), "the note is \"%s\"", plat_thread_note_get());
    said = report();
    if (name[0])
        snprintf(line, sizeof line, "  on one of the runtime's threads, not the game's: \"%s\"\n", name);
    else
        snprintf(line, sizeof line,
                 "  on one of the runtime's threads, not the game's, which the system gives no name\n");
    CHECK(strstr(said, line) != NULL, "no line \"%s\" in the report:\n%s", line, said);
    CHECK(strstr(said, "  it was " PIPELINE_NOTE "\n") != NULL, "the note is not in the report:\n%s", said);
    CHECK(!strstr(said, "noted nothing"), "a noted thread's report says it noted nothing:\n%s", said);
    CHECK(strstr(said, "The game crashed (invalid memory access)") != NULL, "the report ends without its fatal:\n%s",
          said);
    plat_thread_note_clear();
    CHECK(!plat_thread_note_get()[0], "the note after its clear is \"%s\"", plat_thread_note_get());
    said = report();
    CHECK(strstr(said, "  it noted nothing it was doing") != NULL, "a cleared note leaves no such line:\n%s", said);
    CHECK(!strstr(said, "it was making"), "a cleared note stays in the report:\n%s", said);
    return NULL;
}

/* another thread, started while the main thread holds a note: its own is empty, and its note leaves the main one */
static void* other(void* arg) {
    const char** seen = arg;
    seen[0] = plat_thread_note_get()[0] ? "the main thread's" : "";
    plat_thread_note("the other thread's");
    return NULL;
}

int main(void) {
    pthread_t t;
    const char* seen[1] = {"not run"};
    char cut[PLAT_THREAD_NOTE_SIZE + 64];

    CHECK(!pthread_create(&t, NULL, worker, NULL) && !pthread_join(t, NULL), "the worker did not run");

    CHECK(!plat_thread_note_get()[0], "a thread that noted nothing has \"%s\"", plat_thread_note_get());
    plat_thread_note("the main thread's");
    CHECK(!pthread_create(&t, NULL, other, (void*)seen) && !pthread_join(t, NULL), "the other thread did not run");
    CHECK(!strcmp(seen[0], ""), "a new thread's note is %s", seen[0]);
    CHECK(!strcmp(plat_thread_note_get(), "the main thread's"), "the main thread's note became \"%s\"",
          plat_thread_note_get());

    /* a note past the size is cut at PLAT_THREAD_NOTE_SIZE - 1 bytes, its end kept */
    memset(cut, 'x', sizeof cut - 1);
    cut[sizeof cut - 1] = 0;
    plat_thread_note("%s", cut);
    CHECK(strlen(plat_thread_note_get()) == PLAT_THREAD_NOTE_SIZE - 1, "a %zu-byte note keeps %zu bytes",
          sizeof cut - 1, strlen(plat_thread_note_get()));
    plat_thread_note_clear();

    /* a name read into a small buffer is cut there */
    {
        char small[4] = "zzz";
        snx_thread_name("Vulkan pipelines");
        snx_thread_name_get(small, sizeof small);
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(_WIN32)
        CHECK(!strcmp(small, "Vul"), "a 4-byte buffer holds \"%s\"", small);
#endif
        snx_thread_name_get(small, 0); /* no room: nothing written */
    }

    if (g_failures) {
        fprintf(stderr, "crash_runtime_thread: %d failed\n", g_failures);
        return 1;
    }
    printf("crash_runtime_thread: ok\n");
    return 0;
}
