/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Vita's save states with the GPU's work in them (src/vita/runtime/state.c, vita/hle/gxm.c), on a Vulkan device
   that imports guest memory and on one that works on copies (vk_ext_external_memory_host=off). A process of the test's
   maps two memory blocks for sceGxm, draws a scene on a color surface in one and has the GPU copy the other's bytes,
   and is saved before that work is waited for: the state holds what the GPU wrote. After the CPU writes over it all, a
   load gives the memory back and makes the renderer again, which then draws and copies from that memory; a fresh
   process's renderer is made by the load itself. Skips (77) without a Vulkan device */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* setenv, sigaction */
#endif
#include "vita/gpu/gxm.h"
#include "vita/gpu/render.h"
#include "vita/gpu/vk.h"
#include "vita/hle/hle.h"
#include "vita/runtime/process.h"
#include "vita/runtime/state.h"
#include "vita/runtime/watch.h"
#include "common/utilities/common.h"
#include "nx/runtime/ini.h"
#include "snx_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#endif

#define CHECK(expression)                                                                                              \
    do {                                                                                                               \
        if (!(expression)) {                                                                                           \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);                                           \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)
#define BLOCK 0x40000u

#ifndef _WIN32
/* a write to a page the renderer copied: the watch's, which lets it through */
static void on_fault(int signal, siginfo_t* info, void* context) {
    (void)context;
    if (vita_watch_fault((uintptr_t)info->si_addr)) return;
    static const char said[] = "a fault the watch does not know\n";
    if (write(2, said, sizeof said - 1) < 0) _exit(3);
    _exit(signal);
}
#endif

static uint32_t call(VitaProcess* p, VitaThread* t, const char* name, const uint32_t* args, unsigned count) {
    const VitaHle* h = vita_hle_table;
    while (h->name && strcmp(h->name, name)) ++h;
    if (!h->name) exit(1);
    for (unsigned i = 0; i < count && i < 4; ++i) t->cpu.r[i] = args[i];
    p->current = t;
    uint32_t r = h->function(p, t);
    p->current = NULL;
    return r;
}
#define CALL(name, ...)                                                                                                \
    call(p, t, name, (const uint32_t[]){__VA_ARGS__}, sizeof((const uint32_t[]){__VA_ARGS__}) / sizeof(uint32_t))

static unsigned char pattern(unsigned which, uint32_t i) { return (unsigned char)(which * 0x35u + i * 7u + (i >> 8)); }
static void fill(VitaProcess* p, uint32_t at, uint32_t size, unsigned which) {
    unsigned char* to = vita_ptr(p, at);
    for (uint32_t i = 0; i < size; ++i) to[i] = pattern(which, i);
}
static uint32_t wrong(VitaProcess* p, uint32_t at, uint32_t size, unsigned which) {
    const unsigned char* got = vita_ptr(p, at);
    uint32_t n = 0;
    for (uint32_t i = 0; i < size; ++i) n += got[i] != pattern(which, i);
    return n;
}
/* a scene on a linear color surface of 48 x 24 at at, nothing drawn: loaded from memory and stored back */
static void scene(VitaProcess* p, uint32_t at) {
    GxmContext* c = &p->gxm->context;
    p->gxm->targets[0] = (GxmTarget){.width = 48, .height = 24};
    c->has_color = true, c->target = 0;
    c->color = (GxmColorSurface){.width = 48, .height = 24, .stride = 48, .data = at, .type = GXM_COLOR_SURFACE_LINEAR};
    vita_render_begin_scene(p, c);
    vita_render_end_scene(p, c);
}
static bool copy(VitaProcess* p, uint32_t from, uint32_t to, uint32_t size) {
    return vita_render_transfer_copy(p, size / 4, 1, 4, from, 0, 0, (int32_t)size, to, 0, 0, (int32_t)size);
}

/* the checks on the device made: 0, or 1 having said why */
static int run(const char* root, const char* device) {
    char why[512] = "", *folder = snx_path_join(root, "game"), *saved = snx_path_join(root, "gpu.snxstate");
    CHECK(folder && saved && !snx_mkdirs(folder));
    VitaImage image = {0};
    VitaProcess* p = vita_process_new(&image, folder, why, sizeof why);
    CHECK(p && vita_watch_init(p));
    VitaThread* t = &p->threads[0];
    t->cpu.r[13] -= 0x100;
    uint32_t params = vita_alloc(p, &p->user, 0x1000, 0x1000);
    CHECK(params);
    vita_write32(p, params + 4, 2), vita_write32(p, params + 16, 0x40000);
    CHECK(CALL("sceGxmInitialize", params) == 0 && p->gxm->render);
    uint32_t a = vita_alloc(p, &p->cdram, BLOCK, BLOCK), b = vita_alloc(p, &p->cdram, BLOCK, BLOCK);
    CHECK(a && b && CALL("sceGxmMapMemory", a, BLOCK) == 0 && CALL("sceGxmMapMemory", b, BLOCK) == 0);
    /* the GPU's work, recorded and not waited for: a scene on b, a copy of a's bytes past it */
    fill(p, a, 0x1000, 1);
    fill(p, b, 48 * 24 * 4, 2);
    scene(p, b);
    CHECK(copy(p, a, b + 0x10000, 0x1000));
    CHECK(!snx_vita_state_save(p, saved, NULL, why, sizeof why));
    CHECK(!wrong(p, b + 0x10000, 0x1000, 1)); /* the save waited for the GPU, whose copy is in memory */
    /* the CPU over all of it, then the state back */
    fill(p, a, 0x1000, 9), fill(p, b, 48 * 24 * 4, 9), fill(p, b + 0x10000, 0x1000, 9);
    if (snx_vita_state_load(p, saved, why, sizeof why)) {
        fprintf(stderr, "vita_state_gpu (%s): %s\n", device, why);
        return 1;
    }
    CHECK(p->gxm->render && !wrong(p, a, 0x1000, 1) && !wrong(p, b, 48 * 24 * 4, 2) &&
          !wrong(p, b + 0x10000, 0x1000, 1));
    /* the renderer made again draws and copies from the state's memory */
    scene(p, b);
    CHECK(copy(p, b, a + 0x20000, 48 * 24 * 4));
    vita_render_wait(p, vita_render_finish(p));
    CHECK(!wrong(p, a + 0x20000, 48 * 24 * 4, 2));
    vita_process_free(p);
    /* a fresh process: its renderer is the load's */
    VitaProcess* q = vita_process_new(&image, folder, why, sizeof why);
    CHECK(q && vita_watch_init(q) && !q->gxm);
    if (snx_vita_state_load(q, saved, why, sizeof why)) {
        fprintf(stderr, "vita_state_gpu (%s): a fresh process: %s\n", device, why);
        return 1;
    }
    CHECK(q->gxm && q->gxm->render && !wrong(q, b + 0x10000, 0x1000, 1));
    CHECK(copy(q, b + 0x10000, a + 0x30000, 0x1000));
    vita_render_wait(q, vita_render_finish(q));
    CHECK(!wrong(q, a + 0x30000, 0x1000, 1));
    vita_process_free(q);
    remove(saved);
    free(saved);
    free(folder);
    printf("ok (%s)\n", device);
    return 0;
}

int main(void) {
#ifndef _WIN32
    struct sigaction action = {.sa_sigaction = on_fault, .sa_flags = SA_SIGINFO | SA_NODEFER};
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    setenv("SDL_AUDIO_DRIVER", "dummy", 1);
#endif
    char* root = snx_temp_dir(NULL);
    CHECK(root);
    const char* devices[2] = {"import", "copy"};
    for (unsigned d = 0; d < 2; ++d) {
        char why[512];
        ini_read(ini_find(g_vk_extensions_ini, "vk_ext_external_memory_host"), d ? "off" : "auto");
        if (!vita_vk_init(NULL, why, sizeof why)) {
            printf("vita_state_gpu: skipped, no Vulkan device (%s)\n", why);
            return 77;
        }
        if (!d && !vita_vk()->host_import) printf("the device does not import guest memory: its copies alone\n");
        int failed = (d || vita_vk()->host_import) && run(root, devices[d]);
        vita_vk_free();
        if (failed) return 1;
    }
    CHECK(!snx_remove_tree(root));
    free(root);
    puts("vita_state_gpu: ok");
    return 0;
}
