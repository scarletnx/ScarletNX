/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* hook_libs.h: the hook module libraries' files, entry points, versions and CPU hooks */
#include "nx/loader/hook_libs.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/platform/dl.h"
#include "common/utilities/ascii.h"
#include "common/utilities/common.h"
#include "nx/loader/hook_routes.h"

/* the suffix of the system's libraries, as the toolchains name a shared module */
#if defined(_WIN32)
#define LIB_SUFFIX ".dll"
#elif defined(__APPLE__)
#define LIB_SUFFIX ".dylib"
#else
#define LIB_SUFFIX ".so"
#endif

typedef const SnxHookModule* (*Entry)(const SnxHookHost* host);

char* snx_hook_libs_folder(const char* setting) {
    char *data, *folder;
    if (setting && setting[0]) return snx_strdup(setting);
    data = snx_user_dir(SNX_USER_DATA);
    folder = data ? snx_path_join(data, SNX_HOOK_LIBS_FOLDER) : NULL;
    free(data);
    return folder;
}

const char* snx_hook_lib_file(const char* path) {
    const char *file = path, *p;
    for (p = path; *p; p++)
        if (snx_is_any_separator(*p)) file = p + 1;
    return file;
}

int snx_hook_lib_is_library(const char* file) {
    size_t n = strlen(file), s = sizeof LIB_SUFFIX - 1;
    return n > s && !strcmp(file + n - s, LIB_SUFFIX);
}

int snx_hook_lib_name(const char* file, char name[SNX_HOOK_LIB_NAME + 1], char* why, size_t size) {
    size_t n = strlen(file) - (sizeof LIB_SUFFIX - 1), i;
    if (n > SNX_HOOK_LIB_NAME) {
        snprintf(why, size, "its name is longer than %d characters", SNX_HOOK_LIB_NAME);
        return 0;
    }
    for (i = 0; i < n; i++)
        if (!snx_is_lower(file[i]) && !snx_is_digit(file[i]) && file[i] != '_') {
            snprintf(why, size,
                     "its name has a character other than a to z, 0 to 9 and _ (a module's name is its "
                     "word in hooks_off)");
            return 0;
        }
    memcpy(name, file, n);
    name[n] = 0;
    return 1;
}

unsigned snx_hook_lib_cpu_count(const SnxHookModule* m) { return m->ncpu_hooks; }

/* why m's code checks and CPU hooks can't be taken, or NULL */
static const char* cpu_refused(const SnxHookModule* m, char* why, size_t size) {
    unsigned n = snx_hook_lib_cpu_count(m), i;
    if (!n || n > SNX_HOOK_CPU_HOOKS || !m->cpu_hooks) {
        snprintf(why, size, "it has %u CPU hooks (1 to %d)", n, SNX_HOOK_CPU_HOOKS);
        return why;
    }
    if (!m->build_id || !snx_hook_build_hex(m->build_id)) {
        snprintf(why, size, "no build ID of 2 to 64 hex digits says which build its CPU hooks are in");
        return why;
    }
    if (m->ncode > SNX_HOOK_CODES || (m->ncode && !m->code)) {
        snprintf(why, size, "it has %u code checks (at most %d)", m->ncode, SNX_HOOK_CODES);
        return why;
    }
    for (i = 0; i < m->ncode; i++)
        if (!m->code[i].bytes) {
            snprintf(why, size, "its code check %u has no bytes", i);
            return why;
        }
    for (i = 0; i < n; i++) {
        const SnxHookCpuHook* h = &m->cpu_hooks[i];
        if (!h->name || !h->handler || (h->offset & 3)) {
            snprintf(why, size, "its CPU hook %u has no name, no handler, or an offset that isn't 4-aligned (%llx)", i,
                     (unsigned long long)h->offset);
            return why;
        }
    }
    return NULL;
}

const char* snx_hook_lib_open(const char* path, const char* name, const SnxHookHost* host, void** library,
                              const SnxHookModule** module, char* why, size_t size) {
    void *lib, *symbol;
    Entry entry;
    const SnxHookModule* m;
    const char* refused = why;
    *library = NULL;
    *module = NULL;
    if (!(lib = plat_dl_open(path))) {
        snprintf(why, size, "it doesn't load: %s", plat_dl_error());
        return why;
    }
    if (!(symbol = plat_dl_sym(lib, SNX_HOOK_ENTRY))) {
        plat_dl_close(lib);
        return "it has no " SNX_HOOK_ENTRY;
    }
    memcpy(&entry, &symbol, sizeof entry); /* a function's address as dlsym gives it */
    m = entry(host);
    if (!m)
        snprintf(why, size, "its %s refused this runtime (hook ABI %d.%d)", SNX_HOOK_ENTRY, SNX_HOOK_ABI_MAJOR,
                 SNX_HOOK_ABI_MINOR);
    else if (m->abi_major != SNX_HOOK_ABI_MAJOR || m->abi_minor > SNX_HOOK_ABI_MINOR)
        snprintf(why, size, "it is built for hook ABI %u.%u, and the runtime has %d.%d (%s)", m->abi_major,
                 m->abi_minor, SNX_HOOK_ABI_MAJOR, SNX_HOOK_ABI_MINOR,
                 m->abi_major != SNX_HOOK_ABI_MAJOR ? "another major" : "a newer minor than the runtime's");
    else if (!m->name || strcmp(m->name, name))
        snprintf(why, size, "its module is named %s, not as its file", m->name ? m->name : "(none)");
    else
        refused = cpu_refused(m, why, size);
    if (refused) {
        plat_dl_close(lib);
        return refused;
    }
    *library = lib;
    *module = m;
    return NULL;
}

/* ---- the recompile's */
static void (*g_say)(const char* line);

static void say_log(int level, const char* format, ...) {
    char line[512];
    va_list a;
    (void)level;
    va_start(a, format);
    vsnprintf(line, sizeof line, format, a);
    va_end(a);
    if (g_say) g_say(line);
}

size_t snx_hook_libs_route(const char* folder, void (*say)(const char* line)) {
    /* the log alone: a module's entry point may log, and no handler of it runs here */
    static const SnxHookHost k_describe = {
        .abi_major = SNX_HOOK_ABI_MAJOR, .abi_minor = SNX_HOOK_ABI_MINOR, .size = sizeof(SnxHookHost), .log = say_log};
    char** files;
    size_t count, i, added = 0;
    g_say = say;
    if (snx_list_dir(folder, &files, &count)) return 0; /* no folder: no module */
    for (i = 0; i < count; i++) {
        char name[SNX_HOOK_LIB_NAME + 1], why[256], line[512];
        const char* refused = why;
        const SnxHookModule* m = NULL;
        void* lib = NULL;
        char* path;
        unsigned k, n;
        if (!snx_hook_lib_is_library(files[i])) continue;
        path = snx_path_join(folder, files[i]);
        if (snx_hook_lib_name(files[i], name, why, sizeof why))
            refused = snx_hook_lib_open(path, name, &k_describe, &lib, &m, why, sizeof why);
        if (refused) {
            snprintf(line, sizeof line, "hook module library %s: no CPU hook routed: %s", path, refused);
            if (say) say(line);
            free(path);
            continue;
        }
        n = snx_hook_lib_cpu_count(m);
        for (k = 0; k < n; k++) snx_hook_route_add_build(m->build_id, m->cpu_hooks[k].offset), added++;
        snprintf(line, sizeof line, "hook module library %s: module %s, %u CPU hook%s routed", path, name, n,
                 n == 1 ? "" : "s");
        if (say) say(line);
        plat_dl_close(lib); /* the build IDs are copies: nothing of it is used after */
        free(path);
    }
    snx_free_strings(files, count);
    return added;
}
