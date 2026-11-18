/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The console's GL layer (src/nx/gpu/gl/: gl_loader.c, gl_egl.c, gl_calls.c, gl_host_nx.c), with the runtime stubbed
   to a GL driver module's exports, a dispatch table, guest memory and a renderer that records its presents. With
   gl_layer=0 no slot changes. With 1, every egl and _gl export takes a handler, and the calls go through the slots as a
   game's do: EGL's display, config (matched, refused for a size it hasn't, EGL_BAD_ATTRIBUTE for an attribute it
   doesn't know), a 4.5 core debug context (a 2.1 one refused with EGL_BAD_MATCH), a window surface the size of the
   handheld display, eglMakeCurrent setting the viewport to it, glGetString's version in guest memory, eglGetProcAddress
   answering a GL name with its export; glClear's color in the image, then in the scissor alone, rows from GL's bottom;
   eglSwapBuffers presenting that image (RGBA8888, pitch-linear) and drawing into the other next; a program of
   desktop GLSL 3.30 core and 1.20 compiled, linked and its uniforms written and read through the slots with the
   guest's pointers, and a shader refused with its line in the log; a buffer of immutable storage mapped persistently
   at an address of guest memory, written through it and read back, a vertex array's attribute reading the array
   buffer bound; a texture given texels from a pointer passed on the stack, kept in guest memory, and a parameter of
   it read back, and a framebuffer complete once that texture is attached; a GL and an EGL function without a handler stopping the game, the message naming the function and
   gl_layer's value */
#include "common/gpu/gl/gl_state.h"
#include "common/gpu/gl/gl_texture.h"
#include "common/platform/thread.h"
#include "nx/gpu/gl/gl_layer.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/hook/hook.h"
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
#define GUEST   (48u << 20)

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
                                      "_glScissor",
                                      "_glDrawArrays",
                                      "eglQuerySurface",
                                      "eglDestroySurface",
                                      "_glCreateShader",
                                      "_glShaderSource",
                                      "_glCompileShader",
                                      "_glGetShaderiv",
                                      "_glGetShaderInfoLog",
                                      "_glCreateProgram",
                                      "_glAttachShader",
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
                                      "_glVertexAttribPointer",
                                      "_glEnableVertexAttribArray",
                                      "_glGetVertexAttribiv",
                                      "_glGenTextures",
                                      "_glBindTexture",
                                      "_glTexImage2D",
                                      "_glTexParameteri",
                                      "_glGetTexParameteriv",
                                      "_glGenFramebuffers",
                                      "_glBindFramebuffer",
                                      "_glFramebufferTexture2D",
                                      "_glCheckFramebufferStatus"};
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
u64 kernel_find_symbol_exact(const char* name) {
    for (size_t i = 0; i < EXPORTS; i++)
        if (!strcmp(exports[i], name)) return at(name);
    return 0;
}
int mem_user_string(char* dst, u64 addr, u64 cap) {
    for (u64 i = 0; i < cap; i++)
        if (!(dst[i] = (char)guest[addr + i])) return (int)(i + 1);
    return 0;
}
static u64 heap = HEAP;
u64 kmem_alloc(u64 size, u32 state, u32 perm) {
    u64 a = heap;
    (void)state, (void)perm;
    if (heap + size > GUEST) return 0;
    heap += (size + 0xfff) & ~0xfffull;
    return a;
}
void nce_slot_taken(u64 addr, const char* what) { (void)addr, (void)what; }
void jit_slot_taken(u64 addr, GuestFn fn) { (void)addr, (void)fn; }
int hook_routed(const char* name) { return (void)name, 1; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void present_pace(u32 interval, int slot) { (void)interval, (void)slot; }
void gpu_call(void (*fn)(void*), void* arg, const u32* chans, u32 nchans) {
    (void)chans, (void)nchans;
    fn(arg); /* the GPU thread's turn, at once */
}
static struct {
    unsigned count;
    u64 addr;
    u32 w, h, pitch, layout, fmt;
} shown;
static void present_buffer(u64 cpu_addr, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4],
                           u32 transform, u64 released_ns, u64 period_ns, u32 shown_syncpt) {
    (void)bhl, (void)crop, (void)transform, (void)released_ns, (void)period_ns, (void)shown_syncpt;
    shown.count++, shown.addr = cpu_addr, shown.w = w, shown.h = h, shown.pitch = pitch, shown.layout = layout;
    shown.fmt = fmt;
}
static Renderer renderer = {.present_buffer = present_buffer};
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
static const u8* pixel(u64 image, u32 w, u32 h, u32 x, u32 y_from_bottom) {
    return GP(image + ((u64)(h - 1 - y_from_bottom) * w + x) * 4);
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

    /* gl_layer=0: the driver keeps every slot */
    memcpy(before, slots, sizeof slots);
    gl_loader_init();
    assert(!memcmp(before, slots, sizeof slots));

    g_settings.gl_layer = 1;
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
    /* a window surface is the display's size, docked or handheld, whatever the settings' defaults on the host */
    g_settings.tv_mode = 1;
    {
        u64 docked = call("eglCreateWindowSurface", 1, 1, 0, 0, 0);
        assert(docked && call("eglQuerySurface", 1, docked, 0x3057, SCRATCH + 0x500, 0) == 1); /* EGL_WIDTH */
        assert(call("eglQuerySurface", 1, docked, 0x3056, SCRATCH + 0x504, 0) == 1);           /* EGL_HEIGHT */
        assert(gr32(SCRATCH + 0x500) == 1920 && gr32(SCRATCH + 0x504) == 1080);
        assert(call("eglDestroySurface", 1, docked, 0, 0, 0) == 1);
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

    /* glClear: the whole image, then the scissor's 10x10 at GL's bottom left */
    arg_f(0, 0.25f), arg_f(1, 0.5f), arg_f(2, 1), arg_f(3, 1);
    call("_glClearColor", 0, 0, 0, 0, 0);
    call("_glClear", 0x4000, 0, 0, 0, 0);
    u64 first = 0;
    {
        u32 w, h;
        gl_host_surface_size((u32)surface, &w, &h);
        assert(w == 1280 && h == 720);
    }
    call("_glEnable", 0x0c11, 0, 0, 0, 0); /* GL_SCISSOR_TEST */
    call("_glScissor", 0, 0, 10, 10, 0);
    arg_f(0, 1), arg_f(1, 0), arg_f(2, 0), arg_f(3, 1);
    call("_glClearColor", 0, 0, 0, 0, 0);
    call("_glClear", 0x4000, 0, 0, 0, 0);
    assert(call("eglSwapBuffers", 1, surface, 0, 0, 0) == 1);
    assert(shown.count == 1 && g_frames == 1 && shown.w == 1280 && shown.h == 720 && shown.pitch == 1280 * 4);
    assert(shown.layout == 0 && shown.fmt == 1);
    first = shown.addr;
    {
        const u8* red = pixel(first, 1280, 720, 9, 9);
        const u8* blue = pixel(first, 1280, 720, 10, 9);
        const u8* top = pixel(first, 1280, 720, 0, 10);
        assert(red[0] == 255 && red[1] == 0 && red[2] == 0 && red[3] == 255);
        assert(blue[0] == 64 && blue[1] == 128 && blue[2] == 255 && blue[3] == 255);
        assert(top[0] == 64 && top[2] == 255);
    }
    assert(call("eglSwapBuffers", 1, surface, 0, 0, 0) == 1);
    assert(shown.count == 2 && shown.addr != first); /* the other image */

    /* a program through the slots: its shaders' text from the guest's array of pointers, a 3.30 core vertex shader
       and a 1.20 fragment shader (the f suffix, a precision statement), linked; a uniform written and read back */
    {
        static const char vs[] = "#version 330 core\nlayout(location = 0) in vec4 p;\nuniform mat4 m;\n"
                                 "void main() { gl_Position = m * p; }\n";
        static const char fs[] = "#version 120\nprecision mediump float;\nuniform vec4 tint;\n"
                                 "void main() { gl_FragColor = tint * 0.5f; }\n";
        const u64 text = SCRATCH + 0x1000, strings = SCRATCH + 0x2000, lengths = SCRATCH + 0x2100;
        const u64 status = SCRATCH + 0x2200, out = SCRATCH + 0x2300, name = SCRATCH + 0x2400;
        u64 v, f, prog;
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
        u64 at, b, vao, ab;
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
        cpu.x[5] = 32;                                   /* the pointer: an offset in the array buffer */
        call("_glVertexAttribPointer", 2, 3, 0x1406, 0, 12);
        call("_glEnableVertexAttribArray", 2, 0, 0, 0, 0);
        call("_glGetVertexAttribiv", 2, 0x889f, out, 0, 0); /* GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING */
        assert(vao && gr32(out) == ab);
        call("_glGetVertexAttribiv", 2, 0x8622, out, 0, 0); /* _ENABLED */
        assert(gr32(out) == 1);
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
        assert(!memcmp(GP(x->texels), GP(pixels), 16));
        call("_glTexParameteri", 0x0de1, 0x2801, 0x2600, 0, 0); /* GL_TEXTURE_MIN_FILTER, GL_NEAREST */
        call("_glGetTexParameteriv", 0x0de1, 0x2801, out, 0, 0);
        assert(gr32(out) == 0x2600);
        /* a framebuffer of that texture: none attached, then complete */
        call("_glGenFramebuffers", 1, names, 0, 0, 0);
        call("_glBindFramebuffer", 0x8d40, gr32(names), 0, 0, 0);
        assert(call("_glCheckFramebufferStatus", 0x8d40, 0, 0, 0, 0) == 0x8cd7); /* _INCOMPLETE_MISSING_ATTACHMENT */
        call("_glFramebufferTexture2D", 0x8d40, 0x8ce0, 0x0de1, t, 0);
        assert(call("_glCheckFramebufferStatus", 0x8d40, 0, 0, 0, 0) == 0x8cd5); /* GL_FRAMEBUFFER_COMPLETE */
        /* its clear is drawing, which the layer does not do yet: said, and neither of the window's images is green */
        arg_f(0, 0), arg_f(1, 1), arg_f(2, 0), arg_f(3, 1);
        call("_glClearColor", 0, 0, 0, 0, 0);
        call("_glClear", 0x4000, 0, 0, 0, 0);
        assert(strstr(logged, "glClear of framebuffer") && strstr(logged, "isn't drawn yet"));
        assert(pixel(first, 1280, 720, 0, 0)[1] != 255 && pixel(shown.addr, 1280, 720, 0, 0)[1] != 255);
        call("_glBindFramebuffer", 0x8d40, 0, 0, 0, 0);
    }

    /* a function without a handler stops the game, naming it and gl_layer's value; the driver never ran */
    assert(!strcmp(stops("_glDrawArrays"),
                   "gl layer: the game called glDrawArrays, which the GL layer (gl_layer=1) doesn't implement yet"));
    assert(!strcmp(stops("eglCopyBuffers"),
                   "gl layer: the game called eglCopyBuffers, which the GL layer (gl_layer=1) doesn't implement yet"));
    assert(!driver_ran);
    puts("gl_layer: ok");
    return 0;
}
