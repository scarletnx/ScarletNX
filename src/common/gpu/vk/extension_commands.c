/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The extensions' commands looked up and asked of a device (extension_commands.h) */
#include "common/gpu/vk/extension_commands.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int by_name(const void* key, const void* entry) {
    return strcmp((const char*)key, ((const SnxVkExtensionEntry*)entry)->name);
}
const SnxVkExtensionEntry* snx_vk_extension_entry(const char* name) {
    return (const SnxVkExtensionEntry*)bsearch(name, snx_vk_extension_table, snx_vk_extension_table_count,
                                               sizeof *snx_vk_extension_table, by_name);
}

/* ---- the registry's depends ---- */

#define DEPENDS_DEPTH 32 /* parentheses within parentheses: the registry's go two deep */
typedef struct {
    const char* at;
    int (*holds)(const char* name, size_t n, void* ctx);
    void* ctx;
    int bad;
    unsigned depth;
} Depends;
static int depends_expr(Depends* d);
static int depends_term(Depends* d) {
    if (*d->at == '(') {
        d->at++;
        if (++d->depth > DEPENDS_DEPTH) d->bad = 1;
        int v = d->bad ? 0 : depends_expr(d);
        d->depth--;
        if (*d->at != ')')
            d->bad = 1;
        else
            d->at++;
        return v;
    }
    const char* name = d->at;
    while ((*d->at >= 'a' && *d->at <= 'z') || (*d->at >= 'A' && *d->at <= 'Z') || (*d->at >= '0' && *d->at <= '9') ||
           *d->at == '_' || *d->at == ':')
        d->at++;
    if (d->at == name) {
        d->bad = 1;
        return 0;
    }
    return d->holds(name, (size_t)(d->at - name), d->ctx) != 0;
}
static int depends_expr(Depends* d) {
    int v = depends_term(d);
    while (!d->bad && (*d->at == ',' || *d->at == '+')) {
        char op = *d->at++;
        int w = depends_term(d);
        v = op == ',' ? v || w : v && w;
    }
    return v;
}
int snx_vk_depends(const char* expr, int (*holds)(const char* name, size_t n, void* ctx), void* ctx) {
    Depends d = {expr, holds, ctx, 0, 0};
    int v = depends_expr(&d);
    return d.bad || *d.at ? -1 : v;
}

/* ---- the hide list ---- */

/* the hide list's next name from *at: its start and length, 0 past the last */
static size_t hide_next(const char** at, const char** name) {
    const char* p = *at;
    while (*p == ',' || *p == ' ' || *p == '\t') p++;
    *name = p;
    while (*p && *p != ',' && *p != ' ' && *p != '\t') p++;
    *at = p;
    return (size_t)(p - *name);
}
int snx_vk_hide_has(const char* hide, const char* command) {
    size_t n, len = strlen(command);
    const char* name;
    while (hide && (n = hide_next(&hide, &name)))
        if (n == len && !strncmp(name, command, n)) return 1;
    return 0;
}

/* the edit distance from a's na bytes to b: insertions, deletions and substitutions, for the nearest names */
static size_t distance(const char* a, size_t na, const char* b) {
    size_t row[129], nb = strlen(b);
    if (na > 128) na = 128;
    if (nb > 128) nb = 128;
    for (size_t j = 0; j <= nb; j++) row[j] = j;
    for (size_t i = 1; i <= na; i++) {
        size_t diagonal = row[0];
        row[0] = i;
        for (size_t j = 1; j <= nb; j++) {
            size_t up = row[j], best = diagonal + (a[i - 1] != b[j - 1]);
            if (up + 1 < best) best = up + 1;
            if (row[j - 1] + 1 < best) best = row[j - 1] + 1;
            diagonal = up;
            row[j] = best;
        }
    }
    return row[nb];
}
/* how many commands the table lists, an extension's each */
static uint32_t command_count(void) {
    if (!snx_vk_extension_table_count) return 0;
    const SnxVkExtensionEntry* last = &snx_vk_extension_table[snx_vk_extension_table_count - 1];
    return last->first + last->count;
}
int snx_vk_hide_check(const char* hide, char* err, size_t err_n) {
    const char *name, *at = hide;
    size_t n;
    uint32_t commands = command_count();
    while (at && (n = hide_next(&at, &name))) {
        uint32_t i = 0;
        while (i < commands &&
               (strlen(snx_vk_extension_commands[i].name) != n || strncmp(snx_vk_extension_commands[i].name, name, n)))
            i++;
        if (i < commands) continue;
        /* the three nearest, each once (a command several extensions require is listed for each) */
        const char* near[3] = {NULL, NULL, NULL};
        size_t far[3] = {(size_t)-1, (size_t)-1, (size_t)-1};
        for (i = 0; i < commands; i++) {
            const char* c = snx_vk_extension_commands[i].name;
            size_t d = distance(name, n, c);
            int seen = 0;
            for (int k = 0; k < 3; k++) seen |= near[k] && !strcmp(near[k], c);
            if (seen) continue;
            for (int k = 0; k < 3; k++)
                if (d < far[k]) {
                    for (int m = 2; m > k; m--) near[m] = near[m - 1], far[m] = far[m - 1];
                    near[k] = c;
                    far[k] = d;
                    break;
                }
        }
        snprintf(err, err_n,
                 "vk_debug_hide_commands=%s: %.*s is no device command a Vulkan extension requires; the nearest: %s, "
                 "%s, %s",
                 hide, (int)n, name, near[0] ? near[0] : "-", near[1] ? near[1] : "-", near[2] ? near[2] : "-");
        return 0;
    }
    return 1;
}

/* ---- a device's check ---- */

typedef struct {
    const char* const* enabled;
    uint32_t n, api;
} Device;
/* a name of a depends, as the device holds it: a version up to its own, an extension it was made with */
static int device_holds(const char* name, size_t n, void* ctx) {
    const Device* d = (const Device*)ctx;
    char text[32], tail;
    unsigned major, minor;
    if (n < sizeof text) {
        memcpy(text, name, n);
        text[n] = 0;
        if (sscanf(text, "VK_VERSION_%u_%u%c", &major, &minor, &tail) == 2)
            return d->api >= VK_MAKE_API_VERSION(0, major, minor, 0);
    }
    for (uint32_t i = 0; i < d->n; i++)
        if (strlen(d->enabled[i]) == n && !strncmp(d->enabled[i], name, n)) return 1;
    return 0;
}
int snx_vk_commands_lacking(const char* const* enabled, uint32_t n, uint32_t api, PFN_vkGetDeviceProcAddr get,
                            VkDevice dev, const char* hide, SnxVkLacking* out, uint32_t max, char* err, size_t err_n) {
    Device device = {enabled, n, api};
    int count = 0;
    for (uint32_t i = 0; i < n; i++) {
        const SnxVkExtensionEntry* e = snx_vk_extension_entry(enabled[i]);
        if (!e) {
            snprintf(err, err_n,
                     "the device is made with %s, which the command table (the Khronos registry the build has) has "
                     "not as a device extension",
                     enabled[i]);
            return -1;
        }
        for (uint32_t k = 0; k < e->count; k++) {
            const SnxVkExtensionCommand* c = &snx_vk_extension_commands[e->first + k];
            int holds = c->depends ? snx_vk_depends(c->depends, device_holds, &device) : 1;
            if (holds < 0) {
                snprintf(err, err_n, "the command table's depends of %s for %s, \"%s\", does not parse", c->name,
                         e->name, c->depends);
                return -1;
            }
            if (!holds) continue;
            int hidden = snx_vk_hide_has(hide, c->name), said = 0;
            PFN_vkVoidFunction fn = get(dev, c->name);
            if (fn && !hidden) continue;
            /* listed again under another condition that holds too: one line */
            for (int j = 0; j < count && (uint32_t)j < max; j++)
                said |= out[j].extension == enabled[i] && !strcmp(out[j].command, c->name);
            if (said) continue;
            if ((uint32_t)count < max) out[count] = (SnxVkLacking){enabled[i], c->name, fn != NULL};
            count++;
        }
    }
    return count;
}
