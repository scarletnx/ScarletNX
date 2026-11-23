/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Hook module libraries' CPU hooks (hle/hook/module_lib.c, hook.c's lib_adapter, nx/loader/hook_libs.c), of no game,
   on a 64-bit game stubbed to one module and a dispatch table as tests/hook_install.c has it. The libraries are built
   by the test suite from tests/hook_lib_cpu_mod.c.
   - The recompile's side: snx_hook_libs_route adds each library's CPU hooks to the route list by build ID and offset,
     and says why a library routes nothing. Resolved against a module of the game (the loader's route_addresses), a
     hook given by the ID's first 8 bytes (hx_cpu, in upper case) and one given by the whole ID (hx_cpu_full) take the
     same key, that of the module's whole ID, which the runtime looks them up by. A whole ID doesn't match another
     build's.
   - hook_install with the libraries' folder: the module of the game's title and build applies, and each of its CPU
     hooks the module's route list names takes its function's slot. The one whose key the list leaves out isn't
     installed, and the log says to recompile. The module of another title installs nothing, and a library with CPU
     hooks but no build ID, or one at an offset that isn't 4-aligned, is refused.
   - A call through a slot runs the handler with the CPU as the call left it: one that leaves the function's own code
     out (guest memory read and written), one around it (run_original once, the registers before and after, the
     entry, main's base, a vector register), and one that records an upscale, whose request reaches nvn_cmd.c's
     nvn_cmd_record_upscale with the fields and flags as the module gave them. Each returns to the caller's x30.
   - Outside a handler the host's CPU functions give 0 and change nothing.
   - A mod's change of a hooked function's first instruction turns that hook off, its calls then running the game's
     code through the adapter's invalid path, and leaves the other hooks */
#include "nx/hle/hook/hook.c"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/nvn/nvn_cmd.h"
#include "nx/gpu/renderer.h"

#ifndef HX_LIBS
#error HX_LIBS names the folder the test's libraries are built in
#endif

#define CODE 0x10000u
#define DATA 0x1800u
#define RET  0x9000u

/* tests/hook/install.hook's handlers, which no symbol of this test's game reaches */
u64 test_copy(CPU* c, u64 dst, u64 src, u64 n) { return (void)c, (void)src, (void)n, dst; }
u32 test_answer(CPU* c, u32 a) { return (void)c, a; }

/* ---- the game: one module, every slot holding the same own code, x0 + 100, which returns to x30 */
static unsigned own_runs;
static void own_code(CPU* RESTRICT c, u64 entry) {
    (void)entry;
    own_runs++;
    c->x[0] += 100;
    c->pc = c->x[30];
}
static _Atomic GuestFn slots[0x400];
int kernel_symbol(u32 i, u64* addr, const char** name) { return (void)i, (void)addr, (void)name, 0; }
static const SnxModInfo modules[1] = {{.name = "main",
                                       .base = CODE,
                                       .size = 0x1000,
                                       .build_id = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x23,
                                                    0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67},
                                       .text_size = 0x1000}};
static SnxModule module = {.nmods = 1, .mods = modules, .title_id = 0x0100000000abc000ull};
const SnxModule* g_mod = &module;
const SnxArmModule* g_arm_mod;
_Atomic GuestFn* g_disp = slots;
u64 g_code_lo = CODE, g_code_hi = CODE + 0x1000;
static u8 guest[0x20000];
u8* g_mem = guest;
Hle g_hle;
HleHost g_host;
Settings g_settings;
const SnxRoutes* g_routes;
const HookModule* const g_hook_modules[] = {NULL};

/* ---- the runtime, as far as hook.c and the libraries' host reach it */
u64 kmem_alloc(u64 size, u32 state, u32 perm) { return (void)size, (void)state, (void)perm, 0; }
void mem_query(u64 addr, MemBlock* b) {
    memset(b, 0, sizeof *b);
    b->addr = addr, b->state = MS_CODE, b->perm = PERM_R | PERM_X;
}
int mem_user_mapped(u64 addr, u64 size) { return addr < sizeof guest && size <= sizeof guest - addr; }
GuestFn nce_slot(GuestFn f) { return f; }
void nce_slot_taken(u64 addr, const char* what) { (void)addr, (void)what; }
void jit_slot_taken(u64 addr, GuestFn fn) { (void)addr, (void)fn; }
/* the JIT's code of what the game put at a function: its own code's */
GuestFn jit_code_now(u64 pc) { return (void)pc, own_code; }
_Atomic SnxArmBlock* arm_slot(u32 addr, int thumb) { return (void)addr, (void)thumb, NULL; }
SnxArmBlock arm_block(u32 addr, int thumb) { return (void)addr, (void)thumb, NULL; }
SnxArmCPU* arm_cpu(CPU* c) { return (void)c, NULL; }
void arm_run(SnxArmCPU* c, SnxArmBlock first, u32 stop_at) { (void)c, (void)first, (void)stop_at; }
int linker_links(void) { return 0; }
int hook_verify_call(CPU* c, u64 entry, GuestFn original, GuestFn handler, const HookVerify* v, u64 scratch,
                     u64 scratch_size, HookVerifyResult* r) {
    (void)c, (void)entry, (void)original, (void)handler, (void)v, (void)scratch, (void)scratch_size, (void)r;
    return HOOK_AGREE;
}
const char* hook_verify_text(int what) { return (void)what, ""; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
GuestFn hook_ext_skyline(const char* symbol, u32 sdk, const char** handler) {
    return (void)symbol, (void)sdk, (void)handler, NULL;
}
const char* hook_family_unready(const char* family) { return (void)family, NULL; }
void fatal(const char* fmt, ...) {
    fprintf(stderr, "fatal: %s\n", fmt);
    exit(1);
}
/* options.c's, which the host's setting read reaches */
int snx_setting_text(const char* key, char* out, size_t size) { return (void)key, (void)out, (void)size, 0; }
/* nvn_cmd.c's, which the host's record_upscale reaches */
static NvnUpscaleRequest g_request;
static u64 g_request_cb;
static unsigned g_requests;
int nvn_cmd_record_upscale(CPU* c, u64 cb, const NvnUpscaleRequest* r) {
    (void)c;
    g_request = *r;
    g_request_cb = cb;
    g_requests++;
    return 1;
}
/* the renderer's multiplier, which the host's 1.1 request reaches, and gsv.c's display: a TV's */
static float g_scale_taken;
static float scale_request(float scale) { return g_scale_taken = scale; }
static const Renderer g_test_renderer = {.scale_request = scale_request};
const Renderer* g_renderer = &g_test_renderer;
void vi2_buffer_max_size(u32 size[2]);
void vi2_buffer_max_size(u32 size[2]) { size[0] = 3840, size[1] = 2160; }

static char g_log[16384];
void logf_(int lvl, const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    size_t n = strlen(g_log);
    va_list b;
    va_copy(b, a);
    vsnprintf(g_log + n, sizeof g_log - n, fmt, b);
    va_end(b);
    if (getenv("SNX_TEST_LOG")) vprintf(fmt, a);
    va_end(a);
}
static int said(const char* text) { return strstr(g_log, text) != NULL; }
static char g_said[4096];
static void say(const char* line) {
    size_t n = strlen(g_said);
    snprintf(g_said + n, sizeof g_said - n, "%s\n", line);
}

/* the function at main+offset called through its slot, x0 to x3 its arguments, from RET */
static void call(CPU* c, u64 offset, u64 a0, u64 a1, u64 a2, u64 a3) {
    c->x[0] = a0, c->x[1] = a1, c->x[2] = a2, c->x[3] = a3, c->x[30] = RET, c->pc = 0;
    slots[offset >> 2](c, CODE + offset);
}

int main(void) {
    static CPU c;
    c.mem = guest;
    c.disp = slots;
    for (unsigned i = 0; i < sizeof slots / sizeof slots[0]; i++) slots[i] = own_code;

    /* the recompile's side: the folder's libraries' CPU hooks join the route list by build ID and offset (hx_cpu_title
       names hx_cpu's four again, which are listed once) */
    size_t before = snx_hook_route_build_count();
    assert(snx_hook_libs_route(HX_LIBS "/cpu", say) == 9);
    assert(snx_hook_route_build_count() == before + 5);
    assert(strstr(g_said, "module hx_cpu, 4 CPU hooks routed") &&
           strstr(g_said, "module hx_cpu_full, 1 CPU hook routed"));
    assert(snx_hook_libs_route(HX_LIBS "/cpu_bad", say) == 0);
    assert(strstr(g_said,
                  "hx_cpu_nobuild" HX_SUFFIX
                  ": no CPU hook routed: no build ID of 2 to 64 hex digits says which build its CPU hooks are in"));
    assert(strstr(g_said, "hx_cpu_odd" HX_SUFFIX ": no CPU hook routed: its CPU hook 0 has no name, no handler, or an "
                          "offset that isn't 4-aligned (102)"));
    /* resolved against the game's module as the loader does it: each one under the module's whole ID, the prefix's
       and the whole ID's alike */
    char key[SNX_HOOK_KEY], first[SNX_HOOK_KEY];
    u64 offset;
    for (size_t i = before; i < before + 5; i++) {
        assert(snx_hook_route_build_at(i, modules[0].build_id, &offset));
        assert(snx_hook_build_key(modules[0].build_id, offset, key, sizeof key));
        assert(!strncmp(key, "0123456789abcdef0123456789abcdef01234567:", 41));
    }
    snx_hook_route_add_build("0123456789abcdef0123456789abcdef01234567",
                             0x100); /* hx_cpu's first hook by the whole ID */
    assert(snx_hook_route_build_count() == before + 6);
    assert(snx_hook_route_build_at(before, modules[0].build_id, &offset) && offset == 0x100);
    assert(snx_hook_build_key(modules[0].build_id, offset, first, sizeof first));
    assert(snx_hook_route_build_at(before + 5, modules[0].build_id, &offset) && offset == 0x100);
    assert(snx_hook_build_key(modules[0].build_id, offset, key, sizeof key) && !strcmp(key, first));
    assert(!strcmp(first, "0123456789abcdef0123456789abcdef01234567:100"));
    unsigned char other[32];
    memcpy(other, modules[0].build_id, sizeof other);
    other[19] = 0x66; /* another build: its first 8 bytes the same, its whole ID not */
    assert(snx_hook_route_build_at(before, other, &offset) && !snx_hook_route_build_at(before + 5, other, &offset));
    static const unsigned char tail[32] = {0xab, 0xcd}, none[32];
    assert(snx_hook_build_key(tail, 0x40, key, sizeof key) && !strcmp(key, "abcd:40"));
    assert(snx_hook_build_key(none, 0, key, sizeof key) && !strcmp(key, "00:0"));
    assert(snx_hook_build_hex("ABcd") && !snx_hook_build_hex("abc") && !snx_hook_build_hex("zz") &&
           !snx_hook_build_hex(""));

    /* the game was recompiled with the keys of skip, around, upscale and full, not unrouted's */
    static const char* const routed[] = {
        "0123456789abcdef0123456789abcdef01234567:100", "0123456789abcdef0123456789abcdef01234567:200",
        "0123456789abcdef0123456789abcdef01234567:400", "0123456789abcdef0123456789abcdef01234567:500"};
    static u64 hashes[4];
    for (int i = 0; i < 4; i++) hashes[i] = snx_hook_route_hash(routed[i]);
    qsort(hashes, 4, sizeof hashes[0], snx_compare_u64);
    static SnxRoutes routes = {SNX_ROUTES_VERSION, 4, 0, hashes, 0, 0};
    g_routes = &routes;

    /* the refusals of the libraries that route nothing */
    g_settings.native_hooks = 1;
    assert(!hook_module_lib_load(HX_LIBS "/cpu_bad/hx_cpu_nobuild" HX_SUFFIX, NULL));
    assert(said("hx_cpu_nobuild" HX_SUFFIX
                " refused: no build ID of 2 to 64 hex digits says which build its CPU hooks are in"));
    assert(!hook_module_lib_load(HX_LIBS "/cpu_bad/hx_cpu_odd" HX_SUFFIX, NULL));
    assert(said("refused: its CPU hook 0 has no name, no handler, or an offset that isn't 4-aligned"));

    /* hook_install with the folder: hx_cpu applies, hx_cpu_title doesn't */
    g_log[0] = 0;
    snprintf(g_settings.hook_modules_folder, sizeof g_settings.hook_modules_folder, "%s", HX_LIBS "/cpu");
    hook_install();
    assert(said(": module hx_cpu (hook ABI 1.2), 4 CPU hooks\n"));
    assert(said(": module hx_cpu_full (hook ABI 1.2), 1 CPU hook\n"));
    assert(hook_lib_cpu_count() == 9);
    assert(said("hook: 0123456789abcdef0123456789abcdef01234567:100 at 10100 (main+100) runs module hx_cpu's skip\n"));
    assert(
        said("hook: 0123456789abcdef0123456789abcdef01234567:200 at 10200 (main+200) runs module hx_cpu's around\n"));
    assert(
        said("hook: 0123456789abcdef0123456789abcdef01234567:400 at 10400 (main+400) runs module hx_cpu's upscale\n"));
    assert(said(
        "hook: 0123456789abcdef0123456789abcdef01234567:500 at 10500 (main+500) runs module hx_cpu_full's full\n"));
    assert(said("module hx_cpu's CPU hook unrouted isn't installed: the game was recompiled without its key "
                "0123456789abcdef0123456789abcdef01234567:300 in the route list"));
    assert(!said("hx_cpu_title's"));
    assert(slots[0x100 >> 2] == lib_adapter && slots[0x200 >> 2] == lib_adapter && slots[0x400 >> 2] == lib_adapter &&
           slots[0x500 >> 2] == lib_adapter);
    assert(slots[0x300 >> 2] == own_code);
    const char* handler = NULL;
    assert(!strcmp(hook_at(CODE + 0x200, &handler), "0123456789abcdef0123456789abcdef01234567:200") &&
           !strcmp(handler, "around"));
    assert(hook_site("0123456789abcdef0123456789abcdef01234567:400") == CODE + 0x400);

    /* skip: the function's own code left out, guest memory read and written */
    snx_st_le32(guest + DATA, 1000);
    call(&c, 0x100, 5, DATA, 0, 0);
    assert(c.x[0] == 1010 && guest[DATA + 4] == 0x5a && c.pc == RET && own_runs == 0);
    /* skip with its word unmapped: nothing changed, the registers as the call left them */
    call(&c, 0x100, 5, sizeof guest, 0, 0);
    assert(c.x[0] == 5 && c.pc == RET && own_runs == 0);
    /* around: (6 + 1 + 100) * 3, its own code run once, the entry, main's base and v1 */
    memset(&c.v[1], 9, 16);
    g_log[0] = 0;
    call(&c, 0x200, 6, 0, 0, 0);
    assert(own_runs == 1 && c.x[0] == 321 && c.x[2] == CODE + 0x200 && c.x[3] == CODE && c.x[4] == 9);
    assert(((const u8*)&c.v[1])[15] == 7 && c.pc == RET);
    assert(said("ran the function at 10200's own code twice in one call"));
    /* full, the hook of the whole ID */
    call(&c, 0x500, 1, 0, 0, 0);
    assert(c.x[0] == 0xf011 && c.pc == RET && own_runs == 1);
    /* unrouted: its function's own code */
    call(&c, 0x300, 1, 0, 0, 0);
    assert(c.x[0] == 101 && own_runs == 2);
    /* upscale: the request as the module gave it */
    call(&c, 0x400, 0x3000, 0x3100, 0x3200, 0x3300);
    assert(g_requests == 1 && c.x[0] == 1 && g_request_cb == 0x3000 && c.pc == RET);
    assert(g_request.tex[NVN_UPSCALE_COLOR] == 0x3100 && g_request.tex[NVN_UPSCALE_OUTPUT] == 0x3200 &&
           g_request.view[NVN_UPSCALE_OUTPUT] == 0x3300 && !g_request.view[NVN_UPSCALE_COLOR] &&
           !g_request.tex[NVN_UPSCALE_DEPTH] && !g_request.tex[NVN_UPSCALE_MOTION]);
    assert(g_request.render[0] == 64 && g_request.render[1] == 36 && g_request.out[0] == 128 &&
           g_request.out[1] == 72 && g_request.out_base[0] == 2 && g_request.out_base[1] == 3 &&
           g_request.pre_exposure == 1 && !g_request.reset);
    assert(g_request.flags == (GPU_UPSCALE_RANGE_GIVEN | GPU_UPSCALE_SPATIAL_OK | GPU_UPSCALE_MODULE));
    /* a texture past guest memory: not recorded, 0 */
    call(&c, 0x400, 0x3000, sizeof guest, 0x3200, 0);
    assert(g_requests == 1 && c.x[0] == 0);

    /* the host's CPU functions outside a handler */
    const SnxHookHost* h = hook_module_lib_host();
    SnxHookCpu idle = {&c, CODE, 0, 0, 0};
    u8 v[16];
    u32 word = 1;
    c.x[5] = 55;
    assert(h->abi_minor == SNX_HOOK_ABI_MINOR && h->size == sizeof *h);
    assert(!h->reg(&idle, 5) && !h->entry(&idle) && !h->read(&idle, DATA, &word, 4) && !word);
    h->set_reg(&idle, 5, 1);
    h->run_original(&idle);
    assert(c.x[5] == 55 && own_runs == 2 && !h->write(&idle, DATA, &word, 4));
    h->vreg(&idle, 1, v);
    assert(!v[0]);
    SnxHookRecordedUpscale u = {0};
    u.size = sizeof u;
    assert(!h->record_upscale(&idle, 0x3000, &u) && g_requests == 1);
    /* 1.1: the display, and a multiplier the renderer takes while the user's resolution is 1, its default. The
       user's other one stays, and one out of 0.25 to 16 is not asked of the renderer */
    uint32_t display[2] = {0, 0}, target[2] = {3840, 2160}, render[2] = {1920, 1080};
    assert(h->display_size(display) && display[0] == 3840 && display[1] == 2160);
    g_settings.resolution_scale = 1;
    assert(h->resolution_scale(&idle, 2, target, render) == 2 && g_scale_taken == 2);
    g_scale_taken = 0;
    g_settings.resolution_scale = 1.5f;
    assert(h->resolution_scale(&idle, 2, target, render) == 1.5f && g_scale_taken == 0);
    g_settings.resolution_scale = 1;
    assert(h->resolution_scale(&idle, 100, NULL, NULL) == 1 && g_scale_taken == 0);
    assert(h->module_base("main") == CODE && !h->module_base("other"));

    /* a mod changed the first instruction of skip's function: that hook is off and the game's code there runs (the
       JIT's, as hook_invalidated takes it), through the adapter all the libraries' hooks share. The others stay */
    g_log[0] = 0;
    snx_st_le32(guest + CODE + 0x100, 0x14000000u);
    hook_restore(CODE, 0x1000);
    assert(said("which module hx_cpu's CPU hook skip answered: a mod's hook"));
    assert(slots[0x100 >> 2] == lib_adapter && slots[0x200 >> 2] == lib_adapter && slots[0x500 >> 2] == lib_adapter);
    call(&c, 0x100, 5, DATA, 0, 0);
    assert(c.x[0] == 105 && own_runs == 3 && c.pc == RET);
    call(&c, 0x500, 1, 0, 0, 0);
    assert(c.x[0] == 0xf011 && own_runs == 3);
    puts("hook_lib_cpu: passed");
    return 0;
}
