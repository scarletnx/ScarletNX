/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* module_lib.h: a library's module joins the list, its CPU hooks are kept, and their handlers run with the guest CPU */
#include "nx/nn/module_lib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/platform/dl.h"
#include "common/utilities/common.h"
#include <stdarg.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/nvn/nvn_cmd.h"
#include "nx/gpu/renderer.h"
#include "nx/nn/hook.h"
#include "nx/hle/services/vi.h"
#include "nx/memory/mem.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"

/* a library's module as the runtime's, in one allocation that starts with it */
typedef struct {
    HookModule module;
    HookModuleCode code[SNX_HOOK_CODES];
} LibModule;

/* the CPU hooks of the libraries' modules, in the order they were loaded */
static struct {
    HookLibCpu list[64];
    u32 count;
} g_cpu;

static int named(const char* name) {
    const HookModule* m;
    for (u32 i = 0; (m = hook_module_at(i)) != NULL; i++)
        if (!strcmp(m->name, name)) return 1;
    return 0;
}

/* the library at path, its module's name name: its module added, or the reason it is refused (closed then) */
static const char* take(const char* path, const char* name, char* why, size_t size) {
    void* library;
    const SnxHookModule* m;
    const char* refused;
    if (named(name)) {
        snprintf(why, size, "another module is named %s", name);
        return why;
    }
    if ((refused = snx_hook_lib_open(path, name, hook_module_lib_host(), &library, &m, why, size)) != NULL)
        return refused;
    unsigned ncpu = snx_hook_lib_cpu_count(m);
    LibModule* own = NULL;
    int index = -1;
    if (g_cpu.count + ncpu > sizeof g_cpu.list / sizeof g_cpu.list[0])
        refused = "the libraries have more CPU hooks than the runtime holds (64)";
    else if (!(own = calloc(1, sizeof *own)))
        refused = "no memory for it";
    else {
        /* the runtime's module of it: copies of its code checks, its strings the library's (it stays loaded) */
        for (u32 i = 0; i < m->ncode; i++)
            own->code[i] = (HookModuleCode){m->code[i].module, m->code[i].offset, m->code[i].bytes};
        own->module =
            (HookModule){m->name, m->what, m->title, m->module, m->build_id, m->ncode ? own->code : NULL, m->ncode};
        if ((index = hook_module_add(&own->module)) < 0) refused = "the list of modules is full";
    }
    if (refused) {
        free(own);
        plat_dl_close(library);
        return refused;
    }
    for (unsigned k = 0; k < ncpu; k++) {
        HookLibCpu* l = &g_cpu.list[g_cpu.count++];
        l->module = (u32)index;
        l->owner = own->module.name;
        l->in = m->module;
        l->offset = m->cpu_hooks[k].offset;
        l->hook = &m->cpu_hooks[k];
        l->key[0] = 0; /* hook_install's, from the game's module */
    }
    LOG("hooks: library %s: module %s (hook ABI %u.%u), %u CPU hook%s\n", path, name, m->abi_major, m->abi_minor, ncpu,
        ncpu == 1 ? "" : "s");
    return NULL;
}

int hook_module_lib_load(const char* path, int (*off)(const char* name)) {
    const char* file = snx_hook_lib_file(path);
    char name[SNX_HOOK_LIB_NAME + 1], why[256];
    const char* refused = why;
    if (!snx_hook_lib_is_library(file))
        snprintf(why, sizeof why, "it isn't a library");
    else if (snx_hook_lib_name(file, name, why, sizeof why)) {
        if (off && off(name)) {
            LOG("hooks: module %s is off (hooks=0 or hooks_off): its library %s isn't loaded\n", name, path);
            return 0;
        }
        refused = take(path, name, why, sizeof why);
    }
    if (!refused) return 1;
    LOG("hooks: library %s refused: %s\n", path, refused);
    return 0;
}

u32 hook_module_libs_load_dir(const char* folder, int (*off)(const char* name)) {
    char** files;
    size_t count;
    u32 added = 0;
    if (snx_list_dir(folder, &files, &count)) return 0; /* no folder: no module */
    for (size_t i = 0; i < count; i++) {
        if (!snx_hook_lib_is_library(files[i])) continue;
        char* path = snx_path_join(folder, files[i]);
        added += (u32)hook_module_lib_load(path, off);
        free(path);
    }
    snx_free_strings(files, count);
    return added;
}

void hook_module_libs_load(int (*off)(const char* name)) {
    static int loaded;
    if (loaded) return;
    loaded = 1;
    char* folder = snx_hook_libs_folder(g_settings.hook_modules_folder);
    if (!folder) return;
    u32 added = hook_module_libs_load_dir(folder, off);
    if (added) LOG("hooks: %u module%s from the libraries in %s\n", added, added == 1 ? "" : "s", folder);
    free(folder);
}

/* ---- the CPU hooks */
u32 hook_lib_cpu_count(void) { return g_cpu.count; }

HookLibCpu* hook_lib_cpu(u32 i) { return i < g_cpu.count ? &g_cpu.list[i] : NULL; }

void hook_lib_cpu_run(u32 i, CPU* c, u64 entry) {
    SnxHookCpu cpu = {c, entry, 1, 0, i};
    g_cpu.list[i].hook->handler(hook_module_lib_host(), &cpu);
    cpu.active = 0;
}

static uint64_t hook_lib_cpu_reg(const SnxHookCpu* cpu, uint32_t n) {
    return cpu && cpu->active && n < 32 ? cpu->c->x[n] : 0;
}

static void hook_lib_cpu_set_reg(SnxHookCpu* cpu, uint32_t n, uint64_t value) {
    if (cpu && cpu->active && n < 32) cpu->c->x[n] = value;
}

static void hook_lib_cpu_vreg(const SnxHookCpu* cpu, uint32_t n, uint8_t out[16]) {
    if (cpu && cpu->active && n < 32)
        memcpy(out, &cpu->c->v[n], 16);
    else
        memset(out, 0, 16);
}

static void hook_lib_cpu_set_vreg(SnxHookCpu* cpu, uint32_t n, const uint8_t in[16]) {
    if (cpu && cpu->active && n < 32) memcpy(&cpu->c->v[n], in, 16);
}

static uint64_t hook_lib_cpu_entry(const SnxHookCpu* cpu) { return cpu && cpu->active ? cpu->entry : 0; }

static int hook_lib_cpu_read(const SnxHookCpu* cpu, uint64_t addr, void* out, uint64_t size) {
    if (!cpu || !cpu->active || addr + size < addr || !mem_user_mapped(addr, size)) {
        memset(out, 0, size);
        return 0;
    }
    if (size) memcpy(out, hook_host(cpu->c, addr), size);
    return 1;
}

static int hook_lib_cpu_write(SnxHookCpu* cpu, uint64_t addr, const void* in, uint64_t size) {
    if (!cpu || !cpu->active || addr + size < addr || !mem_user_mapped(addr, size)) return 0;
    if (size) memcpy(hook_host(cpu->c, addr), in, size);
    return 1;
}

static void hook_lib_cpu_run_original(SnxHookCpu* cpu) {
    if (!cpu || !cpu->active) return;
    if (cpu->ran) {
        LOG_LIMIT(4,
                  "hooks: a CPU hook's handler ran the function at %llx's own code twice in one call: once is "
                  "all it runs\n",
                  (unsigned long long)cpu->entry);
        return;
    }
    cpu->ran = 1;
    hook_run_original(cpu->c, cpu->entry);
}

static uint64_t hook_lib_cpu_module_base(const char* module) {
    if (!module) module = "main";
    for (u32 i = 0; g_mod && i < g_mod->nmods; i++)
        if (g_mod->mods[i].name && !strcmp(g_mod->mods[i].name, module)) return g_mod->mods[i].base;
    return 0;
}

/* ---- the host a library's entry point and handlers get, the same for every library and for the run */
[[gnu::format(printf, 2, 3)]] static void host_log(int level, const char* format, ...);
static void host_log(int level, const char* format, ...) {
    char line[512];
    va_list a;
    va_start(a, format);
    vsnprintf(line, sizeof line, format, a);
    va_end(a);
    size_t n = strlen(line);
    while (n && line[n - 1] == '\n') line[--n] = 0;
    if (level == SNX_HOOK_LOG_WARN)
        WARN("hooks: %s\n", line);
    else
        LOG("hooks: %s\n", line);
}

static int host_setting(const char* key, char* out, uint32_t size) {
    return key && out && size ? snx_setting_text(key, out, size) : 0;
}

/* the ABI's upscale flags as the runtime's: what the ABI doesn't name is dropped */
static u32 flags_in(uint32_t flags) {
    static const struct {
        uint32_t lib;
        u32 own;
    } k_flags[] = {
        {SNX_HOOK_UPSCALE_HDR, GPU_UPSCALE_HDR},
        {SNX_HOOK_UPSCALE_RENDER_MOTION, GPU_UPSCALE_RENDER_MOTION},
        {SNX_HOOK_UPSCALE_JITTERED_MOTION, GPU_UPSCALE_JITTERED_MOTION},
        {SNX_HOOK_UPSCALE_INVERTED_DEPTH, GPU_UPSCALE_INVERTED_DEPTH},
        {SNX_HOOK_UPSCALE_AUTO_EXPOSURE, GPU_UPSCALE_AUTO_EXPOSURE},
        {SNX_HOOK_UPSCALE_REPROJECTED_MOTION, GPU_UPSCALE_REPROJECTED_MOTION},
        {SNX_HOOK_UPSCALE_RUNTIME_JITTER, GPU_UPSCALE_RUNTIME_JITTER},
        {SNX_HOOK_UPSCALE_RANGE_GIVEN, GPU_UPSCALE_RANGE_GIVEN},
        {SNX_HOOK_UPSCALE_SPATIAL_OK, GPU_UPSCALE_SPATIAL_OK},
        {SNX_HOOK_UPSCALE_COPY_OK, GPU_UPSCALE_COPY_OK},
    };
    u32 own = 0;
    for (size_t i = 0; i < sizeof k_flags / sizeof k_flags[0]; i++)
        if (flags & k_flags[i].lib) own |= k_flags[i].own;
    return own;
}

/* an upscale a CPU hook's handler records into the game's command buffer (nvn_cmd.c) */
static int host_record_upscale(SnxHookCpu* cpu, uint64_t command_buffer, const SnxHookRecordedUpscale* upscale) {
    const SnxHookTexture* t[NVN_UPSCALE_TEXTURES];
    NvnUpscaleRequest r = {0};
    /* 1.1's fields at least: 1.2's reprojection where the module's size has it */
    if (!cpu || !cpu->active || !upscale || upscale->size < offsetof(SnxHookRecordedUpscale, reprojection)) return 0;
    const SnxHookRecordedUpscale* x = upscale;
    int reprojection =
        upscale->size >= sizeof(SnxHookRecordedUpscale) && (x->flags & SNX_HOOK_UPSCALE_REPROJECTED_MOTION);
    t[NVN_UPSCALE_COLOR] = &x->color, t[NVN_UPSCALE_OUTPUT] = &x->output, t[NVN_UPSCALE_DEPTH] = &x->depth;
    t[NVN_UPSCALE_MOTION] = &x->motion, t[NVN_UPSCALE_EXPOSURE] = &x->exposure, t[NVN_UPSCALE_REACTIVE] = &x->reactive;
    /* the game's objects, read where they are: each in mapped memory */
    if (!mem_user_mapped(command_buffer, 8)) return 0;
    for (u32 i = 0; i < NVN_UPSCALE_TEXTURES; i++) {
        if ((t[i]->texture && !mem_user_mapped(t[i]->texture, NVN_TEX_SNAP)) ||
            (t[i]->view && !mem_user_mapped(t[i]->view, sizeof(NvnView)))) {
            LOG_LIMIT(4,
                      "hooks: a CPU hook's upscale names a texture or view at %llx, which isn't mapped: not "
                      "recorded\n",
                      (unsigned long long)(t[i]->texture ? t[i]->texture : t[i]->view));
            return 0;
        }
        r.tex[i] = t[i]->texture, r.view[i] = t[i]->view;
    }
    for (u32 i = 0; i < 2; i++) {
        r.render[i] = x->render[i], r.out[i] = x->out[i], r.render_max[i] = x->render_max[i];
        r.out_base[i] = x->out_base[i], r.jitter[i] = x->jitter[i], r.motion_scale[i] = x->motion_scale[i];
    }
    r.pre_exposure = x->pre_exposure;
    r.reset = x->reset != 0;
    r.flags = flags_in(x->flags) | GPU_UPSCALE_MODULE;
    if (reprojection)
        memcpy(r.reprojection, x->reprojection, sizeof r.reprojection);
    else
        r.flags &= ~(u32)GPU_UPSCALE_REPROJECTED_MOTION;
    return nvn_cmd_record_upscale(cpu->c, command_buffer, &r);
}

/* the display's largest picture, as the game is told it (1.1) */
static int host_display_size(uint32_t out[2]) {
    u32 size[2];
    if (!out) return 0;
    vi2_buffer_max_size(size);
    out[0] = size[0], out[1] = size[1];
    return 1;
}

/* A module's resolution multiplier (1.1), from a CPU hook's handler or from its load (cpu NULL): the user's resolution
   setting wins where it isn't 1, its default, and the renderer takes it until it makes its first render target. The
   multiplier in effect after the call, said in the log */
static float host_resolution_scale(SnxHookCpu* cpu, float scale, const uint32_t target[2], const uint32_t render[2]) {
    static const uint32_t none[2] = {0, 0};
    /* a CPU hook's handler names its module, and a module asking at its load (snx_hook_module, cpu NULL) is said so */
    char who[96];
    if (cpu && cpu->active && cpu->hook < g_cpu.count)
        snprintf(who, sizeof who, "module %s", g_cpu.list[cpu->hook].owner);
    else
        snprintf(who, sizeof who, "a module at its load");
    float user = g_settings.resolution_scale, now;
    if (!target) target = none;
    if (!render) render = none;
    if (!(scale >= 0.25f && scale <= 16.0f)) {
        WARN("hooks: %s asks for the resolution multiplier %g, out of 0.25 to 16: not taken\n", who, (double)scale);
        return user;
    }
    if (user != 1.0f) {
        LOG("hooks: %s asks for the resolution multiplier %g (target %ux%u, the game renders %ux%u), but "
            "resolution=%g, the user's, stays\n",
            who, (double)scale, target[0], target[1], render[0], render[1], (double)user);
        return user;
    }
    if (!g_renderer || !g_renderer->scale_request) {
        LOG("hooks: %s asks for the resolution multiplier %g, which this renderer doesn't take\n", who, (double)scale);
        return 1.0f;
    }
    now = g_renderer->scale_request(scale);
    if (now == scale)
        LOG("hooks: %s sets the resolution multiplier to %g (target %ux%u, the game renders %ux%u)\n", who,
            (double)scale, target[0], target[1], render[0], render[1]);
    else
        WARN("hooks: %s asks for the resolution multiplier %g too late: the renderer has made its render "
             "targets at %g\n",
             who, (double)scale, (double)now);
    return now;
}

static const SnxHookHost k_host = {.abi_major = SNX_HOOK_ABI_MAJOR,
                                   .abi_minor = SNX_HOOK_ABI_MINOR,
                                   .size = sizeof(SnxHookHost),
                                   .log = host_log,
                                   .setting = host_setting,
                                   .reg = hook_lib_cpu_reg,
                                   .set_reg = hook_lib_cpu_set_reg,
                                   .vreg = hook_lib_cpu_vreg,
                                   .set_vreg = hook_lib_cpu_set_vreg,
                                   .entry = hook_lib_cpu_entry,
                                   .read = hook_lib_cpu_read,
                                   .write = hook_lib_cpu_write,
                                   .run_original = hook_lib_cpu_run_original,
                                   .module_base = hook_lib_cpu_module_base,
                                   .record_upscale = host_record_upscale,
                                   .display_size = host_display_size,
                                   .resolution_scale = host_resolution_scale};

const SnxHookHost* hook_module_lib_host(void) { return &k_host; }
