/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* module.h: whether each hook module applies, by its title, its module's build ID and the code it checks */
#include "nx/hle/hook/module.h"
#include <stdio.h>
#include <string.h>
#include "nx/runtime/platform/log.h"

enum { MODULES = 64 };
static u8 g_applies[MODULES]; /* by hook_module_at's index */
static struct {
    const HookModule* list[MODULES];
    u32 count;
} g_added; /* hook_module_add's */

static u32 compiled_in(void) {
    u32 n = 0;
    while (g_hook_modules[n]) n++;
    return n;
}

const HookModule* hook_module_at(u32 i) {
    u32 n = compiled_in();
    return i < n ? g_hook_modules[i] : i - n < g_added.count ? g_added.list[i - n] : NULL;
}

int hook_module_add(const HookModule* m) {
    u32 n = compiled_in();
    if (n + g_added.count >= MODULES) return -1;
    g_added.list[g_added.count++] = m;
    return (int)(n + g_added.count - 1);
}
static _Thread_local char g_why[192];

static const SnxModInfo* find(const SnxModule* mod, const char* name) {
    if (!name) name = "main";
    for (u32 i = 0; i < mod->nmods; i++)
        if (mod->mods[i].name && !strcmp(mod->mods[i].name, name)) return &mod->mods[i];
    return NULL;
}

static int hex(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}

/* whether the build ID begins with the hex of id (at most 32 bytes, an even count of digits) */
static int build_is(const u8 build_id[32], const char* id) {
    u32 n = 0;
    for (; id[0] && id[1]; id += 2, n++) {
        int hi = hex(id[0]), lo = hex(id[1]);
        if (n == 32 || hi < 0 || lo < 0 || build_id[n] != (u8)(hi << 4 | lo)) return 0;
    }
    return !id[0] && n > 0;
}

/* whether the bytes at p (avail of them in the module's code) are those pattern gives, *n the pattern's bytes */
static int code_is(const u8* p, u64 avail, const char* pattern, u32* n, u32* bad) {
    *n = 0;
    for (const char* s = pattern; *s;) {
        if (*s == ' ') {
            s++;
            continue;
        }
        if (!s[1]) return -1; /* half a byte: the pattern is malformed */
        if (*n >= avail) return 0;
        if (s[0] != '?' || s[1] != '?') {
            int hi = hex(s[0]), lo = hex(s[1]);
            if (hi < 0 || lo < 0) return -1;
            if (p[*n] != (u8)(hi << 4 | lo)) {
                *bad = *n;
                return 0;
            }
        }
        (*n)++;
        s += 2;
    }
    return *n > 0 ? 1 : -1;
}

const char* hook_module_refusal(const HookModule* m, const SnxModule* mod, const u8* mem) {
    if (m->title && mod->title_id != m->title) {
        snprintf(g_why, sizeof g_why, "it is for title %016llx, not %016llx", (unsigned long long)m->title,
                 (unsigned long long)mod->title_id);
        return g_why;
    }
    const SnxModInfo* own = find(mod, m->module);
    if ((m->build_id || m->ncode) && !own) {
        snprintf(g_why, sizeof g_why, "the game has no module %s", m->module ? m->module : "main");
        return g_why;
    }
    if (m->build_id && !build_is(own->build_id, m->build_id)) {
        snprintf(g_why, sizeof g_why, "%s is another build than %s", own->name, m->build_id);
        return g_why;
    }
    for (u32 i = 0; i < m->ncode; i++) {
        const HookModuleCode* c = &m->code[i];
        const SnxModInfo* in = c->module ? find(mod, c->module) : own;
        if (!in) {
            snprintf(g_why, sizeof g_why, "check %u names module %s, which the game doesn't have", i, c->module);
            return g_why;
        }
        u32 n, bad = 0;
        u64 end = (u64)in->text_off + in->text_size;
        int is = c->offset >= in->text_off && c->offset < end
                     ? code_is(mem + in->base + c->offset, end - c->offset, c->bytes, &n, &bad)
                     : 0;
        if (is < 0) {
            snprintf(g_why, sizeof g_why, "check %u's bytes \"%s\" aren't hex pairs and ??", i, c->bytes);
            return g_why;
        }
        if (!is) {
            if (c->offset < in->text_off || c->offset >= end)
                snprintf(g_why, sizeof g_why, "check %u at %s+%llx lies outside its code", i, in->name,
                         (unsigned long long)c->offset);
            else
                snprintf(g_why, sizeof g_why, "check %u at %s+%llx differs at byte %u (%02x there)", i, in->name,
                         (unsigned long long)c->offset, bad, mem[in->base + c->offset + bad]);
            return g_why;
        }
    }
    return NULL;
}

void hook_modules_select(const SnxModule* mod, const u8* mem, int (*off)(const char* name)) {
    const HookModule* m;
    memset(g_applies, 0, sizeof g_applies);
    for (u32 i = 0; (m = hook_module_at(i)) != NULL; i++) {
        if (i == MODULES) {
            WARN("hooks: more than %d modules: %s and those after it aren't applied\n", MODULES, m->name);
            break;
        }
        if (off && off(m->name)) {
            LOG("hooks: module %s is off (hooks=0 or hooks_off)\n", m->name);
            continue;
        }
        const char* why = hook_module_refusal(m, mod, mem);
        if (why) {
            /* the title says nothing to log: most modules are for another game */
            if (!m->title || mod->title_id == m->title)
                LOG("hooks: module %s doesn't apply, and leaves the game as it is: %s\n", m->name, why);
            continue;
        }
        g_applies[i] = 1;
        LOG("hooks: module %s applies (%s)\n", m->name, m->what ? m->what : "");
    }
}

int hook_module_applies(u32 i) { return i < MODULES && g_applies[i]; }

int hook_module_family_refused(const char* family) {
    for (u32 i = 0; g_hook_modules[i] && i < MODULES; i++)
        if (!strcmp(g_hook_modules[i]->name, family)) return !g_applies[i];
    return 0; /* a family of no module: hooks_off alone says */
}
