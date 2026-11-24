/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The console's GL layer (src/nx/gpu/gl/: gl_loader.c, gl_egl.c, gl_calls.c, gl_host_nx.c), with the runtime stubbed
   to a GL driver module's exports, a dispatch table, guest memory and a renderer that records its presents. A game with
   no GL driver keeps every slot, and so does a 32-bit one, which says so; otherwise every egl and _gl export takes a
   handler, and the calls go through the slots as a game's do: EGL's display, config (matched, refused for a size it
   hasn't, EGL_BAD_ATTRIBUTE for an attribute it doesn't know), a 4.5 core debug context (a 2.1 one refused with
   EGL_BAD_MATCH), a window surface the size of the handheld display, eglMakeCurrent setting the viewport to it,
   glGetString's version in guest memory, eglGetProcAddress answering a GL name with its export; glClear's color in the
   image, then in the scissor alone, rows from GL's bottom; eglSwapBuffers presenting that image (RGBA8888,
   pitch-linear) and drawing into the other next, and, with a GPU thread that lags, a frame ahead of it at most (the
   swap after waiting off the core for the last present of its next image); a program of desktop GLSL 3.30 core and 1.20
   compiled, linked and its uniforms written and read through the slots with the guest's pointers, and a shader refused
   with its line in the log; a buffer of immutable storage mapped persistently at an address of guest memory, written
   through it and read back, a vertex array's attribute reading the array buffer bound; a texture given texels from a
   pointer passed on the stack, kept in guest memory, and a parameter of it read back, and a framebuffer complete once
   that texture is attached; the point size and polygon mode set; a fence signaled at once; a draw on a GPU thread that
   lags reading the bytes it was called with, a write over them taking the buffer a new range with no wait, and a
   deleted texture's storage held while its upload waits; a GL and an EGL function without a handler stopping the game,
   the message naming the function */
#include "common/gpu/gl/gl_framebuffer.h"
#include "common/gpu/gl/gl_object.h"
#include "common/gpu/gl/gl_state.h"
#include "common/gpu/gl/gl_texture.h"
#include "common/platform/thread.h"
#include "common/platform/time.h"
#include "nx/gpu/gl/gl_layer.h"
#include "nx/gpu/nvn/nvn_tex.h"
#include "nx/gpu/nvn/nvn_tic.h"
#include "nx/gpu/renderer.h"
#include "nx/nn/hook.h"
#include "nx/hle/services/vi.h"
#include "nx/memory/mem.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAIN    0x10000u
#define DRIVER  0x20000u
#define END     0x30000u
#define NAMES   0x40000u  /* the driver's string table */
#define SCRATCH 0x50000u  /* where the game keeps its arguments */
#define HEAP    0x100000u /* kmem_alloc's */
#define WINDOW  (16u << 20)  /* the kernel's region ends here (its pages: kmem_alloc's), the layer's window starts */
#define GUEST   (512u << 20) /* two window surfaces with their depth, the draws' 16 MiB ring, textures */

static _Atomic GuestFn slots[(END - MAIN) / 4];
static u8 guest[GUEST];
u8* g_mem = guest;
_Atomic GuestFn* g_disp = slots;
u64 g_code_lo = MAIN, g_code_hi = END;
static const SnxModInfo modules[2] = {{.name = "main", .base = MAIN, .size = DRIVER - MAIN},
                                      {.name = "driver", .base = DRIVER, .size = END - DRIVER}};
static SnxModule module = {.nmods = 2, .mods = modules};
const SnxModule* g_mod = &module;
const SnxArmModule* g_arm_mod;
Settings g_settings;
_Atomic int g_frames;

static const char* const exports[] = {"eglGetDisplay",
                                      "eglInitialize",
                                      "eglBindAPI",
                                      "eglChooseConfig",
                                      "eglCreateContext",
                                      "eglGetError",
                                      "eglMakeCurrent",
                                      "eglSwapBuffers",
                                      "eglCreateWindowSurface",
                                      "eglGetProcAddress",
                                      "eglCopyBuffers",
                                      "_glClearColor",
                                      "_glClear",
                                      "_glGetString",
                                      "_glGetIntegerv",
                                      "_glEnable",
                                      "_glDisable",
                                      "_glScissor",
                                      "_glDrawArrays",
                                      "_glDrawElements",
                                      "_glDispatchCompute",
                                      "_glActiveTexture",
                                      "eglQuerySurface",
                                      "eglDestroySurface",
                                      "_glCreateShader",
                                      "_glShaderSource",
                                      "_glCompileShader",
                                      "_glGetShaderiv",
                                      "_glGetShaderInfoLog",
                                      "_glCreateProgram",
                                      "_glAttachShader",
                                      "_glBindAttribLocation",
                                      "_glLinkProgram",
                                      "_glGetProgramiv",
                                      "_glGetUniformLocation",
                                      "_glUseProgram",
                                      "_glUniform4f",
                                      "_glUniformMatrix4fv",
                                      "_glGetUniformfv",
                                      "_glGetAttribLocation",
                                      "_glCreateBuffers",
                                      "_glNamedBufferStorage",
                                      "_glMapNamedBufferRange",
                                      "_glUnmapNamedBuffer",
                                      "_glGetNamedBufferSubData",
                                      "_glGenVertexArrays",
                                      "_glBindVertexArray",
                                      "_glBindBuffer",
                                      "_glBufferData",
                                      "_glBufferSubData",
                                      "_glVertexAttribPointer",
                                      "_glEnableVertexAttribArray",
                                      "_glDisableVertexAttribArray",
                                      "_glGetVertexAttribiv",
                                      "_glGenTextures",
                                      "_glBindTexture",
                                      "_glTexImage2D",
                                      "_glTexParameteri",
                                      "_glGetTexParameteriv",
                                      "_glGenerateMipmap",
                                      "_glGenFramebuffers",
                                      "_glBindFramebuffer",
                                      "_glFramebufferTexture2D",
                                      "_glCheckFramebufferStatus",
                                      "_glGenRenderbuffers",
                                      "_glBindRenderbuffer",
                                      "_glRenderbufferStorage",
                                      "_glFramebufferRenderbuffer",
                                      "_glPointSize",
                                      "_glPolygonMode",
                                      "_glFenceSync",
                                      "_glClientWaitSync"};
#define EXPORTS (sizeof exports / sizeof *exports)
static u64 export_name[EXPORTS];
static u64 at(const char* name) {
    for (size_t i = 0; i < EXPORTS; i++)
        if (!strcmp(exports[i], name)) return DRIVER + 0x100 + 0x40 * i;
    abort();
}
int kernel_symbol(u32 i, u64* addr, const char** name) {
    if (i >= EXPORTS) return 0;
    *addr = DRIVER + 0x100 + 0x40 * (u64)i, *name = (const char*)GP(export_name[i]);
    return 1;
}
static int no_driver; /* the game's modules export no GL driver */
u64 kernel_find_symbol_exact(const char* name) {
    if (no_driver) return 0;
    for (size_t i = 0; i < EXPORTS; i++)
        if (!strcmp(exports[i], name)) return at(name);
    return 0;
}
int mem_user_string(char* dst, u64 addr, u64 cap) {
    for (u64 i = 0; i < cap; i++)
        if (!(dst[i] = (char)guest[addr + i])) return (int)(i + 1);
    return 0;
}
/* the address space: the kernel's region small, as the console's GiB is small against a game's textures, and the window
   past it where the layer maps its memory (mem.c's mem_find_free and mem_map over a bump) */
MemLayout g_layout = {.base = 0, .end = GUEST, .kern = HEAP, .kern_end = WINDOW};
static u64 g_window = WINDOW;
u64 mem_find_free(u64 lo, u64 hi, u64 size, u64 align) {
    u64 a = (g_window + align - 1) & ~(align - 1);
    assert(lo >= WINDOW && hi == GUEST);
    return a >= lo && a + size <= hi ? a : 0;
}
int mem_map(u64 addr, u64 size, u32 state, u32 perm) {
    (void)state, (void)perm;
    g_window = addr + size;
    return 0;
}
static u64 heap = HEAP;
u64 kmem_alloc(u64 size, u32 state, u32 perm) {
    u64 a = heap;
    (void)state, (void)perm;
    if (heap + size > WINDOW) return 0;
    heap += (size + 0xfff) & ~0xfffull;
    return a;
}
int nce_slot_taken(u64 addr, const char* what) { return (void)addr, (void)what, 0; }
void jit_slot_taken(u64 addr, GuestFn fn) { (void)addr, (void)fn; }
int hook_routed(const char* name) { return (void)name, 1; }
int shader_vk_set(int stage) { return stage == 4; } /* shader.c's: the fragment stage's set 1 */
int shader_vk_tex_set(int stage) { return shader_vk_set(stage) + (g_spirv_translate.split_sets ? 5 : 0); }
ShCaps g_spirv_translate = {.uniform_cbufs = 18}; /* the renderer's rules: sets, no windows */
/* nvdrv's GPU address space: the layer's chunks at addresses of their own, mapped one to one here */
static u64 g_gpu_next = 0x4000000000ull;
/* vi's shown layer: the size its native window answers (vi_window_size) */
static u32 g_layer_size[2] = {1280, 720};
void vi_window_size(u32* width, u32* height) { *width = g_layer_size[0], *height = g_layer_size[1]; }
/* a game's native window (an ANativeWindow at WINDOW: '_wnd', its query at +144): the SDK's query answers its default
   size (6, 7: what the game set) and its buffers' (0, 1) from window_answers, through hook_guest_call */
#define WINDOW_QUERY 0x7700u
static u32 window_answers[8];
int mem_user_mapped(u64 addr, u64 size) { return addr && addr + size <= GUEST; }
void hook_guest_call(CPU* c, u64 fn, u64 a0, u64 a1, u64 a2, u64 a3) {
    (void)a0, (void)a3;
    assert(fn == WINDOW_QUERY && a1 < 8);
    gw32(a2, window_answers[a1]);
    c->x[0] = 0;
}
u64 gmmu_space_alloc(u64 size, u64 align) {
    u64 a = g_gpu_next;
    (void)align;
    g_gpu_next += (size + 0xffff) & ~0xffffull;
    return a;
}
static struct {
    u64 gva, cpu, size;
} g_maps[256];
static unsigned g_nmaps;
void gmmu_space_map(u64 gva, u64 cpu, u64 size) {
    assert(g_nmaps < 256);
    g_maps[g_nmaps].gva = gva, g_maps[g_nmaps].cpu = cpu, g_maps[g_nmaps++].size = size;
}
/* the guest bytes a GPU address maps to */
static const u8* gpu_bytes(u64 gva) {
    for (unsigned i = 0; i < g_nmaps; i++)
        if (gva >= g_maps[i].gva && gva < g_maps[i].gva + g_maps[i].size)
            return GP(g_maps[i].cpu + gva - g_maps[i].gva);
    fprintf(stderr, "gl_layer: GPU address %llx is mapped nowhere\n", (unsigned long long)gva);
    exit(1);
}
static u64 gpu_cpu(u64 gva) { return (u64)(gpu_bytes(gva) - guest); }
/* maxwell3d.c's notice of guest bytes the runtime wrote, which the GPU's caches take again: the last 256 ranges */
static struct {
    u64 cpu[256], size[256];
    unsigned n;
} g_written;
void gpu_mem_written_cpu(u64 cpu, u64 size) {
    g_written.cpu[g_written.n % 256] = cpu, g_written.size[g_written.n++ % 256] = size;
}
/* whether [cpu, cpu + size) was told written */
static int told_written(u64 cpu, u64 size) {
    for (unsigned i = 0; i < 256 && i < g_written.n; i++)
        if (cpu >= g_written.cpu[i] && cpu + size <= g_written.cpu[i] + g_written.size[i]) return 1;
    return 0;
}
/* nvn_draw.c's render target of an NVN texture: its GPU address and size, the view asked for kept */
static NvnView g_surface_view;
int nvn_draw_surface_of(const NvnTexture* t, const NvnView* v, DsSurf* s, int* depth, u32* msaa) {
    memset(s, 0, sizeof *s);
    g_surface_view = v ? *v : (NvnView){0};
    s->gva = t->gpu, s->width = t->d.width, s->height = t->d.height, s->format = t->d.format, s->layers = 1;
    *depth = t->d.format == NVN_FORMAT_DEPTH24_STENCIL8, *msaa = DS_MSAA_1X1;
    return t->gpu != 0;
}
/* nvn_copy.c's upload into an NVN-laid-out texture, on the GPU thread: what the layer asked for, its first bytes kept */
static struct {
    NvnTexture t;
    NvnView v;
    NVNcopyRegion r;
    u8 bytes[64];
    i64 row, image;
    int count;
} g_upload;
static struct {
    u32 base, top, count;
} g_levels;
void nvn_copy_levels(const NvnTexture* t, unsigned from, unsigned to) {
    (void)t;
    g_levels.base = from, g_levels.top = to, g_levels.count++;
}
int nvn_copy_texels_in(const NvnTexture* t, const NvnView* v, const NVNcopyRegion* region, const u8* bytes, i64 row,
                       i64 image) {
    const u64 size = (u64)image * (u64)region->depth; /* the upload's bytes, its slices image bytes apart */
    g_upload.t = *t, g_upload.v = *v, g_upload.r = *region, g_upload.row = row, g_upload.image = image;
    memset(g_upload.bytes, 0, sizeof g_upload.bytes);
    memcpy(g_upload.bytes, bytes, size < sizeof g_upload.bytes ? size : sizeof g_upload.bytes);
    g_upload.count++;
    return 1;
}
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
/* the guest core: a hook's wait leaves it (hook_wait_begin) and takes it back; the present's pacing is waited for off
   it, as the console's present blocks the thread and its core runs the others */
static struct {
    _Atomic int off; /* read by the test's GPU thread too */
    int paced, paced_off;
} core;
void hook_wait_begin(void) {
    assert(!core.off);
    core.off = 1;
}
void hook_wait_end(void) {
    assert(core.off);
    core.off = 0;
}
void present_pace(u32 interval, int slot) {
    (void)interval, (void)slot;
    core.paced++, core.paced_off += core.off;
}
/* the GPU thread: its turn at once, or, held, the calls kept in order until gpu_run, as a GPU thread that lags behind
   the game's */
static struct {
    _Atomic int held; /* the test's GPU thread clears it, the game's side sets it */
    char pad[60];
    _Atomic unsigned n; /* a call is counted once it is kept, for the test's GPU thread */
    void (*fn[16])(void*);
    void* arg[16];
} g_lagging;
void gpu_call(void (*fn)(void*), void* arg, const u32* chans, u32 nchans) {
    unsigned n = g_lagging.n;
    (void)chans, (void)nchans;
    if (!g_lagging.held) {
        fn(arg);
        return;
    }
    assert(n < 16);
    g_lagging.fn[n] = fn, g_lagging.arg[n] = arg;
    g_lagging.n = n + 1;
}
static void gpu_run(void) {
    g_lagging.held = 0;
    for (unsigned i = 0; i < g_lagging.n; i++) g_lagging.fn[i](g_lagging.arg[i]);
    g_lagging.n = 0;
}
/* a GPU thread of its own that catches up once the game's swap has queued its present (want calls kept) and left the
   core; started as it begins, ran when it is done, neither when it gave up after 10 s */
static struct {
    _Atomic int started, ran;
    unsigned want;
} g_gpu_thread;
static void gpu_thread(void* arg) {
    const u64 until = plat_ns() + 10000000000ull;
    (void)arg;
    while (g_lagging.n < g_gpu_thread.want || !core.off) {
        if (plat_ns() > until) return;
        plat_yield();
    }
    g_gpu_thread.started = 1;
    gpu_run();
    g_gpu_thread.ran = 1;
}
static struct {
    unsigned count;
    u64 addr;
    u32 w, h, pitch, layout, fmt, transform;
} shown;
static void present_buffer(u64 cpu_addr, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4],
                           u32 transform, u64 released_ns, u64 period_ns, u32 shown_syncpt) {
    (void)bhl, (void)crop, (void)released_ns, (void)period_ns, (void)shown_syncpt;
    shown.count++, shown.addr = cpu_addr, shown.w = w, shown.h = h, shown.pitch = pitch, shown.layout = layout;
    shown.fmt = fmt, shown.transform = transform;
}
/* the renderer's draws, clears and host programs: the last state, call and clears, each program's stage and
   description, and the first bytes attribute 0 read when it drew */
static struct {
    unsigned draws, nclear, programs;
    u8 vertex[12];
    DsState ds;
    DrawCall call;
    DrawClear clear[8];
    int stage[8];
    ShaderInfo si[8];
} g_drawn;
static void drawn(const DsState* ds, const DrawCall* dc, DrawOwner* o) {
    const u64 at = ds->vbuf.addr[ds->vfmt.stream[0]];
    (void)o;
    g_drawn.draws++, g_drawn.ds = *ds, g_drawn.call = *dc;
    if (at) memcpy(g_drawn.vertex, gpu_bytes(at), sizeof g_drawn.vertex);
}
static void cleared(const DsState* ds, const DrawClear* c, const DrawOwner* o) {
    (void)o;
    g_drawn.ds = *ds;
    assert(g_drawn.nclear < 8);
    g_drawn.clear[g_drawn.nclear++] = *c;
}
static u32 host_program(int stage, const u32* spirv, size_t words, const ShaderInfo* si) {
    assert(g_drawn.programs < 8 && words > 5 && spirv[0] == 0x07230203u);
    g_drawn.stage[g_drawn.programs] = stage, g_drawn.si[g_drawn.programs] = *si;
    return 100 + g_drawn.programs++;
}
static Renderer renderer = {
    .present_buffer = present_buffer, .draw = drawn, .clear = cleared, .host_program = host_program};
const Renderer* g_renderer = &renderer;
int g_renderer_up = 1;
/* the log's last line */
static char logged[512];
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vsnprintf(logged, sizeof logged, fmt, ap);
    va_end(ap);
}
/* the runtime's fatal: the message kept and the test resumed where it expects one, else the test fails */
static jmp_buf stopped;
static int expect_stop;
static char stop_message[256];
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(stop_message, sizeof stop_message, fmt, ap);
    va_end(ap);
    if (expect_stop) longjmp(stopped, 1);
    fprintf(stderr, "fatal: %s\n", stop_message);
    exit(1);
}

/* ---- the driver's own code, which must never run once the layer has its slots */
static unsigned driver_ran;
static void own(CPU* RESTRICT c, u64 entry) {
    (void)entry;
    driver_ran++;
    c->x[0] = 0xdead;
    c->pc = c->x[30];
}
static _Atomic GuestFn* slot(u64 addr) { return &slots[(addr - MAIN) / 4]; }

static CPU cpu;
static void arg_f(int i, float v) {
    memset(&cpu.v[i], 0, sizeof cpu.v[i]);
    memcpy(&cpu.v[i], &v, sizeof v);
}
/* a call of the export named, with x0 to x5, returning to main's code */
static u64 call(const char* name, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4);
/* a call of the export named that must stop the game: the message it stops with */
static const char* stops(const char* name) {
    stop_message[0] = 0;
    expect_stop = 1;
    if (!setjmp(stopped)) {
        call(name, 0, 0, 0, 0, 0);
        fprintf(stderr, "gl_layer: %s returned instead of stopping the game\n", name);
        exit(1);
    }
    expect_stop = 0;
    return stop_message;
}
static u64 call(const char* name, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4) {
    u64 pc, lr = MAIN + 0x10;
    cpu.x[0] = a0, cpu.x[1] = a1, cpu.x[2] = a2, cpu.x[3] = a3, cpu.x[4] = a4;
    cpu.x[30] = lr;
    cpu.pc = at(name);
    while ((pc = cpu.pc) != lr) snx_lookup(&cpu, pc)(&cpu, pc);
    return cpu.x[0];
}
static u64 put_ints(u64 where, const i32* v, int n) {
    for (int i = 0; i < n; i++) gw32(where + 4 * (u64)i, (u32)v[i]);
    return where;
}
/* a program of a vertex and a fragment shader's text, compiled and linked through the slots: its name */
static u64 program(const char* vs, const char* fs) {
    const u64 text = SCRATCH + 0x4000, strings = SCRATCH + 0x4f00, status = SCRATCH + 0x4f10;
    u64 prog = call("_glCreateProgram", 0, 0, 0, 0, 0);
    for (int i = 0; i < 2; i++) {
        const char* src = i ? fs : vs;
        u64 sh = call("_glCreateShader", i ? 0x8b30 : 0x8b31, 0, 0, 0, 0);
        memcpy(GP(text), src, strlen(src) + 1);
        gw64(strings, text);
        call("_glShaderSource", sh, 1, strings, 0, 0);
        call("_glCompileShader", sh, 0, 0, 0, 0);
        call("_glGetShaderiv", sh, 0x8b81, status, 0, 0);
        assert(gr32(status) == 1);
        call("_glAttachShader", prog, sh, 0, 0, 0);
    }
    call("_glLinkProgram", prog, 0, 0, 0, 0);
    call("_glGetProgramiv", prog, 0x8b82, status, 0, 0);
    assert(gr32(status) == 1);
    return prog;
}

int main(void) {
    static GuestFn before[sizeof slots / sizeof slots[0]];
    u64 name_at = NAMES;
    for (size_t i = 0; i < EXPORTS; i++) {
        export_name[i] = name_at;
        memcpy(GP(name_at), exports[i], strlen(exports[i]) + 1);
        name_at += strlen(exports[i]) + 1;
        *slot(at(exports[i])) = own;
    }
    *slot(MAIN + 0x10) = own;
    cpu.mem = guest, cpu.disp = slots, cpu.code_lo = MAIN, cpu.code_hi = END;

    /* no GL driver, or a 32-bit game's: every slot kept, the 32-bit game told */
    memcpy(before, slots, sizeof slots);
    no_driver = 1;
    gl_loader_init();
    no_driver = 0;
    assert(!memcmp(before, slots, sizeof slots));
    {
        static const SnxArmModule arm32 = {0};
        g_arm_mod = &arm32;
    }
    gl_loader_init();
    g_arm_mod = NULL;
    assert(!memcmp(before, slots, sizeof slots) && strstr(logged, "the game is 32-bit code"));

    gl_loader_init();
    for (size_t i = 0; i < EXPORTS; i++) assert(*slot(at(exports[i])) != own);

    /* EGL: the display, a matching config and one that isn't, an attribute the config hasn't */
    assert(call("eglGetDisplay", 0, 0, 0, 0, 0) == 1);
    assert(call("eglInitialize", 1, SCRATCH, SCRATCH + 4, 0, 0) == 1);
    assert(gr32(SCRATCH) == 1 && gr32(SCRATCH + 4) == 5);
    assert(call("eglBindAPI", 0x30a2, 0, 0, 0, 0) == 1); /* EGL_OPENGL_API */
    {
        const i32 rgb[] = {0x3024, 8, 0x3025, 24, 0x3040, 8, 0x3038}; /* red 8, depth 24, OpenGL: matches */
        const i32 deep[] = {0x3024, 10, 0x3038};                      /* red 10: none */
        const i32 odd[] = {0x3099, 1, 0x3038};                        /* not a config attribute */
        assert(call("eglChooseConfig", 1, put_ints(SCRATCH + 0x100, rgb, 7), SCRATCH + 0x200, 1, SCRATCH + 0x208));
        assert(gr64(SCRATCH + 0x200) == 1 && gr32(SCRATCH + 0x208) == 1);
        assert(call("eglChooseConfig", 1, put_ints(SCRATCH + 0x100, deep, 3), SCRATCH + 0x200, 1, SCRATCH + 0x208));
        assert(gr32(SCRATCH + 0x208) == 0);
        assert(!call("eglChooseConfig", 1, put_ints(SCRATCH + 0x100, odd, 3), SCRATCH + 0x200, 1, SCRATCH + 0x208));
        assert(call("eglGetError", 0, 0, 0, 0, 0) == 0x3004 && call("eglGetError", 0, 0, 0, 0, 0) == 0x3000);
    }
    /* contexts: 2.1 refused (EGL_BAD_MATCH), 4.5 core with debug made */
    u64 ctx;
    {
        const i32 old[] = {0x3098, 2, 0x30fb, 1, 0x3038};
        const i32 core[] = {0x3098, 4, 0x30fb, 5, 0x30fc, 1, 0x3038};
        assert(!call("eglCreateContext", 1, 1, 0, put_ints(SCRATCH + 0x100, old, 5), 0));
        assert(call("eglGetError", 0, 0, 0, 0, 0) == 0x3009);
        ctx = call("eglCreateContext", 1, 1, 0, put_ints(SCRATCH + 0x100, core, 7), 0);
        assert(ctx && snx_gl_context_api((u32)ctx) == SNX_GL_API_DESKTOP && snx_gl_context_version((u32)ctx) == 45);
    }
    /* a window surface is its native window's default size, as the game's driver reads it: the size the game set
       (1920x1080 docked, one game), else vi's layer's, 1280x720 docked too (another game draws its last pass at that);
       a window that is no ANativeWindow takes the layer's */
    g_settings.tv_mode = 1;
    {
        static const u32 asked[3][2] = {{1920, 1080}, {0, 0}, {1600, 900}};
        static const u32 made[4][2] = {{1920, 1080}, {1280, 720}, {1280, 720}, {1280, 720}};
        u64 window = SCRATCH + 0x600;
        gw32(window, 0x5f776e64u); /* '_wnd' */
        gw64(window + 144, WINDOW_QUERY);
        for (int k = 0; k < 4; k++) {
            u64 docked;
            if (k < 3) {
                window_answers[6] = asked[k][0], window_answers[7] = asked[k][1];
                window_answers[0] = 1280, window_answers[1] = 720; /* the buffers', which the driver doesn't take */
            }
            if (k == 2) gw32(window, 0x12345678u); /* not '_wnd': the layer's size, the query never called */
            docked = call("eglCreateWindowSurface", 1, 1, k == 3 ? 0 : window, 0, 0);
            assert(docked && call("eglQuerySurface", 1, docked, 0x3057, SCRATCH + 0x500, 0) == 1); /* EGL_WIDTH */
            assert(call("eglQuerySurface", 1, docked, 0x3056, SCRATCH + 0x504, 0) == 1);           /* EGL_HEIGHT */
            assert(gr32(SCRATCH + 0x500) == made[k][0] && gr32(SCRATCH + 0x504) == made[k][1]);
            assert(call("eglDestroySurface", 1, docked, 0, 0, 0) == 1);
        }
    }
    g_settings.tv_mode = 0;
    u64 surface = call("eglCreateWindowSurface", 1, 1, 0, 0, 0);
    assert(surface);
    assert(call("eglMakeCurrent", 1, surface, surface, ctx, 0) == 1);

    /* GL: the viewport is the handheld display's, the version string is in guest memory, the names resolve */
    call("_glGetIntegerv", 0x0ba2, SCRATCH + 0x300, 0, 0, 0);
    assert(gr32(SCRATCH + 0x300) == 0 && gr32(SCRATCH + 0x308) == 1280 && gr32(SCRATCH + 0x30c) == 720);
    {
        u64 version = call("_glGetString", 0x1f02, 0, 0, 0, 0);
        assert(version && !strcmp((const char*)GP(version), "4.5.0 ScarletNX"));
        assert(call("_glGetString", 0x1f02, 0, 0, 0, 0) == version); /* one copy */
    }
    memcpy(GP(SCRATCH + 0x400), "glClearColor", 13);
    assert(call("eglGetProcAddress", SCRATCH + 0x400, 0, 0, 0, 0) == at("_glClearColor"));
    memcpy(GP(SCRATCH + 0x400), "eglSwapBuffers", 15);
    assert(call("eglGetProcAddress", SCRATCH + 0x400, 0, 0, 0, 0) == at("eglSwapBuffers"));
    memcpy(GP(SCRATCH + 0x400), "glNothing", 10);
    assert(call("eglGetProcAddress", SCRATCH + 0x400, 0, 0, 0, 0) == 0);

    /* glClear: the renderer's clear of the surface's next image, pitch-linear RGBA8 at its GPU address, whole; then its
       color and depth in the scissor's 10x10 at GL's bottom left */
    arg_f(0, 0.25f), arg_f(1, 0.5f), arg_f(2, 1), arg_f(3, 1);
    call("_glClearColor", 0, 0, 0, 0, 0);
    call("_glClear", 0x4000, 0, 0, 0, 0);
    DsSurf window;
    {
        u32 w, h;
        float color[4];
        gl_host_surface_size((u32)surface, &w, &h);
        assert(w == 1280 && h == 720);
        assert(g_drawn.nclear == 1 && g_drawn.clear[0].slot == 0 && g_drawn.clear[0].color_mask == 0xf);
        assert(!g_drawn.clear[0].aspects && !g_drawn.clear[0].scissored);
        memcpy(color, g_drawn.clear[0].color, sizeof color);
        assert(color[0] == 0.25f && color[1] == 0.5f && color[2] == 1 && color[3] == 1);
        window = g_drawn.ds.tgt.color[0];
        assert(g_drawn.ds.tgt.count == 1 && window.width == 1280 * 4 && window.height == 720 && window.mem == 1u << 12);
        assert(window.format == nvn_rt_format(NVN_FORMAT_RGBA8, &(int){0}) && g_drawn.ds.tgt.depth.gva);
        assert(g_drawn.ds.dyn.window_origin == DS_ORIGIN_FLIP_Y && g_drawn.ds.dyn.clip_height == 720);
    }
    call("_glEnable", 0x0c11, 0, 0, 0, 0); /* GL_SCISSOR_TEST */
    call("_glScissor", 0, 0, 10, 10, 0);
    g_drawn.nclear = 0;
    call("_glClear", 0x4000 | 0x100, 0, 0, 0, 0); /* color and depth */
    assert(g_drawn.nclear == 1 && g_drawn.clear[0].aspects == 1 && g_drawn.clear[0].scissored);
    assert(g_drawn.ds.vp.scissor_x[0] == 10u << 16 && g_drawn.ds.vp.scissor_y[0] == 10u << 16);
    call("_glDisable", 0x0c11, 0, 0, 0, 0);
    assert(call("eglSwapBuffers", 1, surface, 0, 0, 0) == 1);
    assert(shown.count == 1 && g_frames == 1 && shown.w == 1280 && shown.h == 720 && shown.pitch == 1280 * 4);
    assert(shown.layout == 0 && shown.fmt == 1 && shown.addr == gpu_cpu(window.gva)); /* the image cleared */
    assert(shown.transform == 2); /* GL's rows count up: shown flipped, as a lower left NVN window is */
    assert(core.paced == 1 && core.paced_off == 1 && !core.off); /* paced off the core, back on it after */
    g_drawn.nclear = 0;
    call("_glClear", 0x4000, 0, 0, 0, 0);
    assert(g_drawn.nclear == 1 && g_drawn.ds.tgt.color[0].gva != window.gva); /* the other image */
    assert(call("eglSwapBuffers", 1, surface, 0, 0, 0) == 1);
    assert(shown.count == 2 && shown.addr == gpu_cpu(g_drawn.ds.tgt.color[0].gva));
    /* a GPU thread that lags: the next swap goes on, the game a frame ahead of it, and the one after waits off the core
       until the GPU thread ran the last present of the image the game draws into next */
    {
        const unsigned before = shown.count;
        g_lagging.held = 1;
        call("_glClear", 0x4000, 0, 0, 0, 0);
        assert(call("eglSwapBuffers", 1, surface, 0, 0, 0) == 1);
        assert(shown.count == before && g_lagging.n == 2); /* its clear and present kept, the game on */
        call("_glClear", 0x4000, 0, 0, 0, 0);
        g_gpu_thread.want = 4;
        assert(!plat_thread(gpu_thread, NULL, 0));
        assert(call("eglSwapBuffers", 1, surface, 0, 0, 0) == 1);
        assert(g_gpu_thread.started); /* back only once the GPU thread ran the image's last present */
        while (!g_gpu_thread.ran) plat_yield();
        assert(shown.count == before + 2 && !g_lagging.n && !core.off);
    }

    /* a program through the slots: its shaders' text from the guest's array of pointers, a 3.30 core vertex shader
       and a 1.20 fragment shader (the f suffix, a precision statement), linked; a uniform written and read back */
    u64 prog, ab, vao;
    {
        static const char vs[] = "#version 330 core\nlayout(location = 0) in vec4 p;\nuniform mat4 m;\n"
                                 "void main() { gl_Position = m * p; }\n";
        static const char fs[] = "#version 120\nprecision mediump float;\nuniform vec4 tint;\n"
                                 "void main() { gl_FragColor = tint * 0.5f; }\n";
        const u64 text = SCRATCH + 0x1000, strings = SCRATCH + 0x2000, lengths = SCRATCH + 0x2100;
        const u64 status = SCRATCH + 0x2200, out = SCRATCH + 0x2300, name = SCRATCH + 0x2400;
        u64 v, f;
        memcpy(GP(text), vs, sizeof vs);
        memcpy(GP(text + 0x400), fs, 20);                  /* fs in two strings: the first 20 bytes, */
        memcpy(GP(text + 0x800), fs + 20, sizeof fs - 20); /* then the rest, NUL-ended */
        gw64(strings, text);
        v = call("_glCreateShader", 0x8b31, 0, 0, 0, 0);
        call("_glShaderSource", v, 1, strings, 0, 0);
        call("_glCompileShader", v, 0, 0, 0, 0);
        call("_glGetShaderiv", v, 0x8b81, status, 0, 0);
        assert(v && gr32(status) == 1);
        gw64(strings, text + 0x400), gw64(strings + 8, text + 0x800);
        gw32(lengths, 20), gw32(lengths + 4, 0xffffffffu); /* -1: up to the NUL */
        f = call("_glCreateShader", 0x8b30, 0, 0, 0, 0);
        call("_glShaderSource", f, 2, strings, lengths, 0);
        call("_glCompileShader", f, 0, 0, 0, 0);
        call("_glGetShaderiv", f, 0x8b81, status, 0, 0);
        assert(f && gr32(status) == 1);
        prog = call("_glCreateProgram", 0, 0, 0, 0, 0);
        call("_glAttachShader", prog, v, 0, 0, 0);
        call("_glAttachShader", prog, f, 0, 0, 0);
        call("_glLinkProgram", prog, 0, 0, 0, 0);
        call("_glGetProgramiv", prog, 0x8b82, status, 0, 0);
        assert(prog && gr32(status) == 1);
        memcpy(GP(name), "tint", 5);
        u64 tint = call("_glGetUniformLocation", prog, name, 0, 0, 0);
        memcpy(GP(name), "p", 2);
        assert((i32)tint >= 0 && call("_glGetAttribLocation", prog, name, 0, 0, 0) == 0);
        call("_glUseProgram", prog, 0, 0, 0, 0);
        arg_f(0, 0.5f), arg_f(1, 0.25f), arg_f(2, 1), arg_f(3, 2);
        call("_glUniform4f", tint, 0, 0, 0, 0);
        call("_glGetUniformfv", prog, tint, out, 0, 0);
        {
            float back[4];
            memcpy(back, GP(out), sizeof back);
            assert(back[0] == 0.5f && back[1] == 0.25f && back[2] == 1 && back[3] == 2);
        }
        /* a shader that does not compile: its log in guest memory, its line in the runtime's log */
        memcpy(GP(text), "#version 330 core\nvoid main() { nothing; }\n", 44);
        gw64(strings, text);
        v = call("_glCreateShader", 0x8b31, 0, 0, 0, 0);
        call("_glShaderSource", v, 1, strings, 0, 0);
        call("_glCompileShader", v, 0, 0, 0, 0);
        call("_glGetShaderiv", v, 0x8b81, status, 0, 0);
        assert(gr32(status) == 0);
        call("_glGetShaderInfoLog", v, 64, status, out, 0);
        assert(!strncmp((const char*)GP(out), "ERROR: 2:", 9) && gr32(status) == strlen((const char*)GP(out)));
        assert(!strncmp(logged, "gl layer: a shader does not compile: ERROR: 2:", 46));
    }

    /* a buffer of immutable storage in guest memory: mapped persistently, written through the map's address, read
       back; a vertex array whose attribute reads the array buffer bound */
    {
        const u64 names = SCRATCH + 0x3000, out = SCRATCH + 0x3100;
        u64 at, b;
        call("_glCreateBuffers", 1, names, 0, 0, 0);
        b = gr32(names);
        call("_glNamedBufferStorage", b, 4096, 0, 0x2 | 0x40 | 0x80, 0); /* write, persistent, coherent */
        at = call("_glMapNamedBufferRange", b, 0, 4096, 0x2 | 0x40 | 0x80, 0);
        assert(b && at >= HEAP && at + 4096 <= GUEST);
        memcpy(GP(at + 100), "in guest memory", 16);
        call("_glGetNamedBufferSubData", b, 100, 16, out, 0);
        assert(!strcmp((const char*)GP(out), "in guest memory"));
        assert(call("_glUnmapNamedBuffer", b, 0, 0, 0, 0) == 1 && call("_glUnmapNamedBuffer", b, 0, 0, 0, 0) == 0);
        call("_glGenVertexArrays", 1, names, 0, 0, 0);
        vao = gr32(names);
        call("_glBindVertexArray", vao, 0, 0, 0, 0);
        call("_glCreateBuffers", 1, names, 0, 0, 0);
        ab = gr32(names);
        call("_glBindBuffer", 0x8892, ab, 0, 0, 0);
        call("_glBufferData", 0x8892, 64, 0, 0x88e8, 0); /* GL_DYNAMIC_DRAW */
        /* bytes the runtime writes into it: the GPU's caches are told, as they see only the game's own writes */
        memcpy(GP(out), "sixteen bytes...", 16);
        call("_glBufferSubData", 0x8892, 8, 16, out, 0);
        assert(told_written(snx_gl_buffer((u32)ab)->data + 8, 16));
        cpu.x[5] = 32;                                   /* the pointer: an offset in the array buffer */
        call("_glVertexAttribPointer", 2, 3, 0x1406, 0, 12);
        call("_glEnableVertexAttribArray", 2, 0, 0, 0, 0);
        call("_glGetVertexAttribiv", 2, 0x889f, out, 0, 0); /* GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING */
        assert(vao && gr32(out) == ab);
        call("_glGetVertexAttribiv", 2, 0x8622, out, 0, 0); /* _ENABLED */
        assert(gr32(out) == 1);
        cpu.x[5] = 0; /* the program's p: attribute 0, three floats from the array buffer's start */
        call("_glVertexAttribPointer", 0, 3, 0x1406, 0, 12);
        call("_glEnableVertexAttribArray", 0, 0, 0, 0, 0);
    }

    /* a texture: glTexImage2D's ninth argument on the stack, its texels kept in guest memory, a parameter read back */
    {
        const u64 names = SCRATCH + 0x3200, out = SCRATCH + 0x3300, pixels = SCRATCH + 0x3400, sp = SCRATCH + 0x3800;
        const SnxGlTexture* x;
        u32 t;
        for (int i = 0; i < 16; i++) guest[pixels + (u64)i] = (u8)(0x10 + i);
        call("_glGenTextures", 1, names, 0, 0, 0);
        t = gr32(names);
        call("_glBindTexture", 0x0de1, t, 0, 0, 0);
        cpu.x[5] = 0, cpu.x[6] = 0x1908, cpu.x[7] = 0x1401; /* border, GL_RGBA, GL_UNSIGNED_BYTE */
        cpu.x[31] = sp;
        gw64(sp, pixels);
        call("_glTexImage2D", 0x0de1, 0, 0x8058, 2, 2); /* GL_RGBA8, 2x2 */
        x = snx_gl_texture(t);
        assert(x && x->levels == 2 && x->texels >= HEAP && x->texels + x->texels_size <= GUEST);
        /* its storage NVN's layout of the same texture, the texels sent to the GPU's copy into it */
        {
            NvnTexture n;
            NvnTexStorage st;
            assert(gl_nvn_texture(t, &n) && n.d.format == NVN_FORMAT_RGBA8 && n.d.target == NVN_TEXTURE_TARGET_2D);
            assert(n.d.width == 2 && n.d.height == 2 && n.d.levels == 2 && n.gpu == gl_gpu_address(x->texels));
            assert(n.gpu && gl_gpu_address(x->texels + 16) == n.gpu + 16 && !gl_gpu_address(0x10));
            nvn_tex_storage(&n.d, &st);
            assert(x->texels_size == st.size && !(x->texels % st.align));
            assert(g_upload.count == 1 && g_upload.t.gpu == n.gpu && g_upload.v.base_level == 0);
            assert(g_upload.r.width == 2 && g_upload.r.height == 2 && g_upload.r.depth == 1 && g_upload.row == 8);
            assert(!memcmp(g_upload.bytes, GP(pixels), 16));
        }
        /* 3-byte texels whose client rows are 4-aligned: the GPU's copy takes them as tight rows */
        call("_glGenTextures", 1, names + 4, 0, 0, 0);
        call("_glBindTexture", 0x0de1, gr32(names + 4), 0, 0, 0);
        cpu.x[5] = 0, cpu.x[6] = 0x1907, cpu.x[7] = 0x1401; /* border, GL_RGB, GL_UNSIGNED_BYTE */
        call("_glTexImage2D", 0x0de1, 0, 0x8051, 2, 2);     /* GL_RGB8 */
        assert(g_upload.count == 2 && g_upload.row == 6 && g_upload.t.d.format == NVN_FORMAT_RGB8);
        assert(!memcmp(g_upload.bytes, GP(pixels), 6) && !memcmp(g_upload.bytes + 6, GP(pixels + 8), 6));
        call("_glBindTexture", 0x0de1, t, 0, 0, 0);
        call("_glGenerateMipmap", 0x0de1, 0, 0, 0, 0); /* level 1 from level 0, by the GPU */
        assert(g_levels.count == 1 && g_levels.base == 0 && g_levels.top == 1 && snx_gl_texture(t)->defined == 3);
        call("_glTexParameteri", 0x0de1, 0x2801, 0x2600, 0, 0); /* GL_TEXTURE_MIN_FILTER, GL_NEAREST */
        call("_glGetTexParameteriv", 0x0de1, 0x2801, out, 0, 0);
        assert(gr32(out) == 0x2600);
        /* GL's texture swizzle in the NVN view: a luminance and alpha atlas in RG (R, R, R, G), then the identity */
        {
            static const u32 la[4] = {0x1903, 0x1903, 0x1903, 0x1904};
            NvnTexture n;
            for (u32 c = 0; c < 4; c++) call("_glTexParameteri", 0x0de1, 0x8e42 + c, la[c], 0, 0);
            assert(gl_nvn_texture(t, &n) &&
                   n.d.swizzle == (NVN_TEXTURE_SWIZZLE_R | NVN_TEXTURE_SWIZZLE_R << 3 | NVN_TEXTURE_SWIZZLE_R << 6 |
                                   NVN_TEXTURE_SWIZZLE_G << 9 | NVN_TEX_SWIZZLE_SET));
            for (u32 c = 0; c < 4; c++) call("_glTexParameteri", 0x0de1, 0x8e42 + c, 0x1903 + c, 0, 0);
            assert(gl_nvn_texture(t, &n) && n.d.swizzle == 0);
        }
        /* a framebuffer of that texture: none attached, then complete */
        call("_glGenFramebuffers", 1, names, 0, 0, 0);
        call("_glBindFramebuffer", 0x8d40, gr32(names), 0, 0, 0);
        assert(call("_glCheckFramebufferStatus", 0x8d40, 0, 0, 0, 0) == 0x8cd7); /* _INCOMPLETE_MISSING_ATTACHMENT */
        call("_glFramebufferTexture2D", 0x8d40, 0x8ce0, 0x0de1, t, 0);
        assert(call("_glCheckFramebufferStatus", 0x8d40, 0, 0, 0, 0) == 0x8cd5); /* GL_FRAMEBUFFER_COMPLETE */
        /* its clear: the renderer's, of the texture's level 0, laid out as NVN's at its GPU address */
        g_drawn.nclear = 0;
        call("_glClear", 0x4000, 0, 0, 0, 0);
        {
            NvnTexture n;
            assert(gl_nvn_texture(t, &n) && g_drawn.nclear == 1 && g_drawn.ds.tgt.count == 1);
            assert(g_drawn.ds.tgt.color[0].gva == n.gpu && !g_drawn.ds.tgt.depth.gva &&
                   g_drawn.ds.dyn.clip_height == 2);
            assert(g_surface_view.base_level == 0 && g_surface_view.levels == 1 && g_surface_view.layers == 1);
        }
        /* a depth renderbuffer: its storage laid out as NVN's DEPTH24_STENCIL8 in the layer's memory, the depth the
           framebuffer's clear and draws take */
        call("_glGenRenderbuffers", 1, names + 12, 0, 0, 0);
        call("_glBindRenderbuffer", 0x8d41, gr32(names + 12), 0, 0, 0);
        call("_glRenderbufferStorage", 0x8d41, 0x88f0, 2, 2, 0); /* GL_DEPTH24_STENCIL8, 2x2 */
        call("_glFramebufferRenderbuffer", 0x8d40, 0x821a, 0x8d41, gr32(names + 12),
             0); /* GL_DEPTH_STENCIL_ATTACHMENT */
        assert(call("_glCheckFramebufferStatus", 0x8d40, 0, 0, 0, 0) == 0x8cd5);
        g_drawn.nclear = 0;
        call("_glClear", 0x100 | 0x400, 0, 0, 0, 0); /* depth and stencil */
        {
            const SnxGlRenderbuffer* rb = snx_gl_renderbuffer(gr32(names + 12));
            NvnTexture n;
            NvnTexStorage st;
            assert(gl_nvn_renderbuffer(gr32(names + 12), &n) && n.d.format == NVN_FORMAT_DEPTH24_STENCIL8);
            nvn_tex_storage(&n.d, &st);
            assert(rb->storage && rb->storage_size == st.size && !(rb->storage % st.align));
            assert(g_drawn.nclear == 1 && g_drawn.clear[0].aspects == 3 && !g_drawn.clear[0].color_mask);
            assert(g_drawn.ds.tgt.depth.gva == gl_gpu_address(rb->storage) && n.gpu == g_drawn.ds.tgt.depth.gva);
        }
        call("_glBindFramebuffer", 0x8d40, 0, 0, 0, 0);

        /* a draw of the program: its two stages given to the renderer once, as host programs the draw state names;
           attribute 0 from the array buffer at its GPU address, each stage's default block a copy in the ring */
        call("_glUseProgram", prog, 0, 0, 0, 0);
        call("_glDrawArrays", 4, 0, 3, 0, 0); /* GL_TRIANGLES */
        {
            const DsState* d = &g_drawn.ds;
            const SnxGlProgram* p = snx_gl_program((u32)prog);
            float tint[4];
            assert(g_drawn.draws == 1 && g_drawn.programs == 2 && g_drawn.stage[0] == ST_VS &&
                   g_drawn.stage[1] == ST_FS);
            assert(g_drawn.si[0].in_mask == 1 && !g_drawn.si[1].ntex);
            assert(d->prog.on == (1u << 1 | 1u << 5) && d->prog.code[1] == (DS_HOST_PROGRAM | 100) &&
                   d->prog.code[5] == (DS_HOST_PROGRAM | 101));
            assert(g_drawn.call.topology == 4 && !g_drawn.call.indexed && g_drawn.call.first == 0 &&
                   g_drawn.call.count == 3 && g_drawn.call.instances == 1);
            assert(d->vfmt.stream_on == 1 && d->vfmt.stream[0] == 0 && d->vfmt.offset[0] == 0 &&
                   d->vfmt.stride[0] == 12);
            assert(d->vfmt.size[0] == 0x02 && d->vfmt.type[0] == 7 && !d->vfmt.size[2]); /* R32_G32_B32 float */
            assert(d->vbuf.addr[0] == gl_gpu_address(snx_gl_buffer((u32)ab)->data) && d->vbuf.size[0] == 64);
            assert(d->bind.cb_size[0][0] == p->default_size[0] && d->bind.cb_size[4][0] == p->default_size[1]);
            memcpy(tint, gpu_bytes(d->bind.cb_addr[4][0]), sizeof tint);
            assert(tint[0] == 0.5f && tint[1] == 0.25f && tint[2] == 1 && tint[3] == 2);
            /* the ring's copies the GPU's caches are told of: its addresses come round again with new bytes */
            assert(told_written(gpu_cpu(d->bind.cb_addr[4][0]), p->default_size[1]) &&
                   told_written(gpu_cpu(d->bind.cb_addr[0][0]), p->default_size[0]));
            assert(d->tgt.color[0].gva == window.gva && d->tgt.count == 1); /* the image after two swaps */
            assert(d->dyn.window_origin == DS_ORIGIN_FLIP_Y && d->rkey.provoking_last && d->rkey.depth_zero_one);
            assert(d->vp.scale[0][0] == 640 && d->vp.scale[0][1] == 360 && d->vp.translate[0][1] == 360);
            assert(d->vp.scale[0][2] == 0.5f && d->vp.translate[0][2] == 0.5f); /* GL's depth from -1 to 1 */
        }
        /* attribute 0 without its array: its generic value, (0, 0, 0, 1), one 16-byte vertex in the ring every vertex
           reads, at stride 0 */
        call("_glDisableVertexAttribArray", 0, 0, 0, 0, 0);
        call("_glDrawArrays", 4, 0, 3, 0, 0);
        {
            const DsState* d = &g_drawn.ds;
            const float one[4] = {0, 0, 0, 1};
            assert(g_drawn.draws == 2 && d->vfmt.stream_on == 1u << 16 && d->vfmt.stream[0] == 16);
            assert(d->vfmt.size[0] == 0x01 && d->vfmt.type[0] == 7 && d->vfmt.stride[16] == 0 &&
                   d->vbuf.size[16] == 16);
            assert(!memcmp(gpu_bytes(d->vbuf.addr[16]), one, sizeof one));
        }
        call("_glEnableVertexAttribArray", 0, 0, 0, 0, 0);
        /* an indexed draw: its 16-bit indices from the element array buffer, at the offset given */
        call("_glCreateBuffers", 1, names + 8, 0, 0, 0);
        call("_glBindBuffer", 0x8893, gr32(names + 8), 0, 0, 0); /* GL_ELEMENT_ARRAY_BUFFER */
        call("_glBufferData", 0x8893, 16, 0, 0x88e4, 0);         /* GL_STATIC_DRAW */
        call("_glDrawElements", 4, 3, 0x1403, 2, 0);             /* GL_UNSIGNED_SHORT, from byte 2 */
        {
            const u64 at = gl_gpu_address(snx_gl_buffer(gr32(names + 8))->data);
            assert(g_drawn.draws == 3 && g_drawn.programs == 2 && g_drawn.call.indexed == 1 && g_drawn.call.count == 3);
            assert(g_drawn.ds.index.addr == at + 2 && g_drawn.ds.index.end == at + 15 && g_drawn.ds.index.format == 1);
        }
        /* attributes bound to locations other than the module's (glBindAttribLocation): the input the module has at
           location 0 (a) reads GL attribute 1, b at 1 reads GL attribute 0 */
        {
            const u64 text = SCRATCH + 0x4000, strings = SCRATCH + 0x4f00, status = SCRATCH + 0x4f10;
            static const char vs2[] =
                "#version 330 core\nin vec4 a;\nin vec4 b;\nvoid main() { gl_Position = a + b; }\n";
            static const char fs2[] = "#version 330 core\nout vec4 o;\nvoid main() { o = vec4(1.0); }\n";
            u64 bound = call("_glCreateProgram", 0, 0, 0, 0, 0);
            for (int i = 0; i < 2; i++) {
                u64 sh = call("_glCreateShader", i ? 0x8b30 : 0x8b31, 0, 0, 0, 0);
                memcpy(GP(text), i ? fs2 : vs2, strlen(i ? fs2 : vs2) + 1);
                gw64(strings, text);
                call("_glShaderSource", sh, 1, strings, 0, 0);
                call("_glCompileShader", sh, 0, 0, 0, 0);
                call("_glAttachShader", bound, sh, 0, 0, 0);
            }
            memcpy(GP(text), "b\0a", 4);
            call("_glBindAttribLocation", bound, 0, text, 0, 0);
            call("_glBindAttribLocation", bound, 1, text + 2, 0, 0);
            call("_glLinkProgram", bound, 0, 0, 0, 0);
            call("_glGetProgramiv", bound, 0x8b82, status, 0, 0);
            assert(gr32(status) == 1);
            cpu.x[5] = 0; /* GL attribute 0 (b): the array buffer from its start; attribute 1 (a): from byte 32 */
            call("_glVertexAttribPointer", 0, 4, 0x1406, 0, 0);
            call("_glEnableVertexAttribArray", 0, 0, 0, 0, 0);
            cpu.x[5] = 32;
            call("_glVertexAttribPointer", 1, 2, 0x1406, 0, 0);
            call("_glEnableVertexAttribArray", 1, 0, 0, 0, 0);
            call("_glUseProgram", bound, 0, 0, 0, 0);
            call("_glDrawArrays", 4, 0, 3, 0, 0);
            const DsVertexFormat* f = &g_drawn.ds.vfmt;
            const u64 data = gl_gpu_address(snx_gl_buffer((u32)ab)->data);
            assert(g_drawn.ds.vbuf.addr[f->stream[0]] == data + 32 && f->size[0] == 0x04); /* a: 2 floats */
            assert(g_drawn.ds.vbuf.addr[f->stream[1]] == data && f->size[1] == 0x01);      /* b: 4 floats */
            call("_glDisableVertexAttribArray", 1, 0, 0, 0, 0);
            call("_glUseProgram", prog, 0, 0, 0, 0);
            cpu.x[5] = 0;
            call("_glVertexAttribPointer", 0, 3, 0x1406, 0, 12);
        }
        /* a texture sampled: its TIC entry at its name in the pool, its sampler state's TSC entry, the handle of both
           in the fragment stage's texture bank */
        {
            /* the renderer started after the layer and split its texture sets: the next link takes its rules */
            g_spirv_translate.split_sets = 1;
            u64 sampled =
                program("#version 330 core\nlayout(location = 0) in vec4 p;\nvoid main() { gl_Position = p; }\n",
                        "#version 330 core\nuniform sampler2D s;\nout vec4 o;\n"
                        "void main() { o = texture(s, vec2(0.5)); }\n");
            const DsBindings* b = &g_drawn.ds.bind;
            u32 handle, tic[8], tsc[8], zero[8] = {0};
            call("_glActiveTexture", 0x84c0, 0, 0, 0, 0); /* GL_TEXTURE0 */
            call("_glBindTexture", 0x0de1, t, 0, 0, 0);
            call("_glUseProgram", sampled, 0, 0, 0, 0);
            call("_glDrawArrays", 4, 0, 3, 0, 0);
            assert(g_drawn.draws == 5 && g_drawn.programs == 6 && g_drawn.si[5].ntex == 1);
            assert(b->tex_bank == 17 && b->tex_max >= t && b->cb_size[4][17] >= 4 && b->cb_addr[4][17]);
            memcpy(&handle, gpu_bytes(b->cb_addr[4][17] + g_drawn.si[5].tex[0].cbuf_off), 4);
            assert(handle == (t | t << 20));
            memcpy(tic, gpu_bytes(b->tex_pool + 32ull * t), sizeof tic);
            memcpy(tsc, gpu_bytes(b->smp_pool + 32ull * t), sizeof tsc);
            assert(memcmp(tic, zero, sizeof tic) && memcmp(tsc, zero, sizeof tsc));
            assert(told_written(gpu_cpu(b->tex_pool + 32ull * t), 32) &&
                   told_written(gpu_cpu(b->smp_pool + 32ull * t), 32));
            assert(told_written(gpu_cpu(b->cb_addr[4][17]), 4));
            {
                const SnxGlProgram* sp = snx_gl_program((u32)sampled);
                const u32* w = snx_gl_program_spirv(sp, 1);
                u32 sampler_set = 0;
                for (u32 i = 5; i < sp->spirv_words[1] && (w[i] >> 16); i += w[i] >> 16)
                    if ((w[i] & 0xffff) == 71 && w[i + 2] == 34 && w[i + 3] != 1) sampler_set = w[i + 3];
                assert(sampler_set == 6); /* the fragment stage's set 1, its textures' five after */
            }
            g_spirv_translate.split_sets = 0;
        }
    }

    /* the point size and polygon mode a GL game sets, read back by glGet */
    {
        const u64 out = SCRATCH + 0x3a00;
        arg_f(0, 3);
        call("_glPointSize", 0, 0, 0, 0, 0);
        call("_glPolygonMode", 0x0408, 0x1b01, 0, 0, 0); /* GL_FRONT_AND_BACK, GL_LINE */
        call("_glGetIntegerv", 0x0b11, out, 0, 0, 0);
        assert(gr32(out) == 3);
        call("_glGetIntegerv", 0x0b40, out, 0, 0, 0);
        assert(gr32(out) == 0x1b01 && gr32(out + 4) == 0x1b01);
        call("_glPolygonMode", 0x0408, 0x1b02, 0, 0, 0);
    }
    /* a fence after the draws: signaled once the GPU thread finished them, here at once */
    {
        u64 sync = call("_glFenceSync", 0x9117, 0, 0, 0, 0);                         /* GL_SYNC_GPU_COMMANDS_COMPLETE */
        assert(sync && call("_glClientWaitSync", sync, 1, 1000000, 0, 0) == 0x911a); /* GL_ALREADY_SIGNALED */
    }

    /* a buffer a draw handed to a GPU thread that lags still reads: the draw reads the bytes it was called with, the
       write over them takes the buffer a new range, as GL's orphaning, and nothing waits: the GPU thread is held here,
       so a wait would never end, and nothing but the draws goes to it */
    {
        const u64 data = snx_gl_buffer((u32)ab)->data, from = SCRATCH + 0x3b00;
        u64 moved;
        u8 before[12];
        memcpy(before, GP(data), sizeof before);
        call("_glUseProgram", prog, 0, 0, 0, 0);
        call("_glBindBuffer", 0x8892, ab, 0, 0, 0);
        cpu.x[5] = 0;
        call("_glVertexAttribPointer", 0, 3, 0x1406, 0, 12);
        call("_glEnableVertexAttribArray", 0, 0, 0, 0, 0);
        g_lagging.held = 1;
        call("_glDrawArrays", 4, 0, 3, 0, 0);
        memset(GP(from), 0x5a, 12);
        g_written.n = 0;
        call("_glBufferSubData", 0x8892, 0, 12, from, 0);
        moved = snx_gl_buffer((u32)ab)->data;
        assert(moved != data && !memcmp(GP(data), before, sizeof before) && GP(moved)[0] == 0x5a &&
               GP(moved)[11] == 0x5a);
        assert(told_written(moved, 12) && g_lagging.n == 1);
        call("_glBufferData", 0x8892, 64, 0, 0x88e8, 0); /* no draw reads the new range yet: kept, zeros */
        call("_glBufferSubData", 0x8892, 4, 4, from, 0);
        assert(snx_gl_buffer((u32)ab)->data == moved && !GP(moved)[0] && GP(moved)[4] == 0x5a && g_lagging.n == 1);
        gpu_run();
        assert(!memcmp(g_drawn.vertex, before, sizeof before) && g_drawn.ds.vbuf.addr[0] == gl_gpu_address(data));
        /* the element array buffer too: the draw's indices kept where it reads them */
        {
            const u64 name = SCRATCH + 0x3c00, indices_in = SCRATCH + 0x3c10;
            u64 indices;
            memset(GP(indices_in), 1, 16);
            call("_glCreateBuffers", 1, name, 0, 0, 0);
            call("_glBindBuffer", 0x8893, gr32(name), 0, 0, 0);
            call("_glBufferData", 0x8893, 16, indices_in, 0x88e4, 0); /* GL_STATIC_DRAW */
            indices = snx_gl_buffer(gr32(name))->data;
            g_lagging.held = 1;
            call("_glDrawElements", 4, 3, 0x1403, 2, 0);
            call("_glBufferSubData", 0x8893, 2, 4, from, 0);
            assert(snx_gl_buffer(gr32(name))->data != indices && GP(indices)[2] == 1 && g_lagging.n == 1);
            assert(GP(snx_gl_buffer(gr32(name))->data)[2] == 0x5a);
            gpu_run();
            assert(g_drawn.ds.index.addr == gl_gpu_address(indices) + 2);
        }
        /* a texture deleted while its upload waits for the GPU thread: its storage held until the upload is done, the
           next texture's elsewhere */
        {
            u32 t = 0, u = 0;
            u64 kept;
            snx_gl_gen_textures((u32)ctx, 1, &t);
            snx_gl_bind_texture((u32)ctx, 0x0de1, t);
            g_lagging.held = 1;
            snx_gl_tex_image((u32)ctx, 0x0de1, 0, 0x8058, 64, 64, 1, 0x1908, 0x1401, SCRATCH + 0x10000, 0, 0);
            kept = snx_gl_texture(t)->texels;
            snx_gl_delete_textures((u32)ctx, 1, &t);
            snx_gl_gen_textures((u32)ctx, 1, &u);
            snx_gl_bind_texture((u32)ctx, 0x0de1, u);
            snx_gl_tex_image((u32)ctx, 0x0de1, 0, 0x8058, 64, 64, 1, 0x1908, 0x1401, 0, 0, 0);
            assert(kept && g_lagging.n >= 1 && snx_gl_texture(u)->texels && snx_gl_texture(u)->texels != kept);
            gpu_run();
            snx_gl_delete_textures((u32)ctx, 1, &u);
        }
    }

    /* a function without a handler stops the game, naming it; the driver never ran */
    assert(!strcmp(stops("_glDispatchCompute"),
                   "gl layer: the game called glDispatchCompute, which the GL layer doesn't implement yet"));
    assert(!strcmp(stops("eglCopyBuffers"),
                   "gl layer: the game called eglCopyBuffers, which the GL layer doesn't implement yet"));
    assert(!driver_ran);
    /* the layer's memory past the kernel's region, which these 48 MiB of textures alive at once and the surfaces
       would overrun; then textures made and deleted and a buffer given new data, frame after frame, that take no more */
    {
        u32 tex[12], buffer = 0;
        u64 taken;
        (void)snx_gl_get_error((u32)ctx); /* what the steps above left */
        snx_gl_gen_textures((u32)ctx, 12, tex);
        for (int i = 0; i < 12; i++) {
            snx_gl_bind_texture((u32)ctx, 0x0de1, tex[i]);
            snx_gl_tex_image((u32)ctx, 0x0de1, 0, 0x8058, 1024, 1024, 1, 0x1908, 0x1401, 0, 0, 0); /* 4 MiB */
            assert(!snx_gl_get_error((u32)ctx) && snx_gl_texture(tex[i])->texels);
        }
        snx_gl_delete_textures((u32)ctx, 12, tex);
        snx_gl_gen_buffers((u32)ctx, 1, &buffer);
        snx_gl_bind_buffer((u32)ctx, 0x8892, buffer);
        taken = gl_guest_pages_taken();
        for (int frame = 0; frame < 60; frame++) {
            u32 t = 0;
            snx_gl_gen_textures((u32)ctx, 1, &t);
            snx_gl_bind_texture((u32)ctx, 0x0de1, t);
            snx_gl_tex_image((u32)ctx, 0x0de1, 0, 0x8058, 1024, 512, 1, 0x1908, 0x1401, 0, 0, 0);
            snx_gl_buffer_data((u32)ctx, buffer, frame & 1 ? 1 << 20 : 3 << 19, NULL, 0x88e0); /* GL_STREAM_DRAW */
            snx_gl_delete_textures((u32)ctx, 1, &t);
        }
        assert(!snx_gl_get_error((u32)ctx) && gl_guest_pages_taken() == taken);
    }
    puts("gl_layer: ok");
    return 0;
}
