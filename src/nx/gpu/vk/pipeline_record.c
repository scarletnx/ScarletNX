/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/vk/pipeline_record.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/gpu/vk/vk_common.h"
#include "common/platform/thread.h"
#include "common/platform/time.h"
#include "common/utilities/bits.h"
#include "common/utilities/common.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "snx_settings.h"

#include "nx/gpu/vk/pipeline_record_format.inc"

/* a handle's entry: a module's SPIR-V hash, or the text of what made the object (its create info's members) */
typedef struct {
    u64 key; /* the handle; 0 for a free slot */
    u64 hash;
    char* text;
    int dead; /* destroyed: its slot keeps the probe going */
} RecEntry;
typedef struct {
    RecEntry* e;
    size_t cap, used;
} RecMap;
typedef struct {
    char* p;
    size_t n, cap;
    int bad; /* out of memory: the text is not whole */
} RecBuf;

static struct {
    PMutex lock;
    int on;
    char dir[1024];
    char run[40]; /* this run's part of a record's folder name */
    atomic_uint seq;
    RecMap modules, samplers, set_layouts, layouts, libraries;
    PFN_vkCreateDevice create_device;
    PFN_vkCreateShaderModule create_module;
    PFN_vkDestroyShaderModule destroy_module;
    PFN_vkCreateSampler create_sampler;
    PFN_vkDestroySampler destroy_sampler;
    PFN_vkCreateDescriptorSetLayout create_set_layout;
    PFN_vkDestroyDescriptorSetLayout destroy_set_layout;
    PFN_vkCreatePipelineLayout create_layout;
    PFN_vkDestroyPipelineLayout destroy_layout;
    PFN_vkCreateGraphicsPipelines driver_make, make;
    PFN_vkDestroyPipeline destroy_pipeline;
} g_pipeline_record = {.lock = PMUTEX_INIT};

/* ---- the maps, g_pipeline_record.lock held */
static RecEntry* map_find(const RecMap* m, u64 key) {
    if (!m->cap || !key) return NULL;
    for (size_t i = (size_t)(key * SNX_FNV_PRIME) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (!m->e[i].key) return NULL;
        if (m->e[i].key == key && !m->e[i].dead) return &m->e[i];
    }
}
static void map_drop(RecMap* m, u64 key) {
    RecEntry* e = map_find(m, key);
    if (!e) return;
    free(e->text);
    e->text = NULL;
    e->dead = 1;
}
/* key's entry, emptied of what it held (a handle made again after a destroy); NULL without memory */
static RecEntry* map_put(RecMap* m, u64 key) {
    map_drop(m, key);
    if ((m->used + 1) * 2 > m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 256;
        RecEntry* e = (RecEntry*)calloc(cap, sizeof *e);
        if (!e) return NULL;
        size_t used = 0;
        for (size_t i = 0; i < m->cap; i++) {
            if (!m->e[i].key || m->e[i].dead) continue;
            size_t j = (size_t)(m->e[i].key * SNX_FNV_PRIME) & (cap - 1);
            while (e[j].key) j = (j + 1) & (cap - 1);
            e[j] = m->e[i];
            used++;
        }
        free(m->e);
        m->e = e, m->cap = cap, m->used = used;
    }
    size_t i = (size_t)(key * SNX_FNV_PRIME) & (m->cap - 1);
    while (m->e[i].key) i = (i + 1) & (m->cap - 1);
    m->e[i] = (RecEntry){.key = key};
    m->used++;
    return &m->e[i];
}

/* ---- the text */
[[gnu::format(printf, 2, 3)]] static void put(RecBuf* b, const char* fmt, ...) {
    va_list a, c;
    va_start(a, fmt);
    va_copy(c, a);
    int len = vsnprintf(NULL, 0, fmt, a);
    va_end(a);
    if (len < 0 || b->bad) {
        b->bad = 1;
        va_end(c);
        return;
    }
    if (b->n + (size_t)len + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + (size_t)len + 1) cap *= 2;
        char* p = (char*)realloc(b->p, cap);
        if (!p) {
            b->bad = 1;
            va_end(c);
            return;
        }
        b->p = p, b->cap = cap;
    }
    vsnprintf(b->p + b->n, b->cap - b->n, fmt, c);
    va_end(c);
    b->n += (size_t)len;
}
static void indent(RecBuf* b, int depth) { put(b, "%*s", depth, ""); }
/* text, a create info's members as put_struct wrote them, as a block at depth */
static void paste(RecBuf* b, const char* text, int depth) {
    put(b, "{\n");
    for (const char* s = text; *s;) {
        const char* end = strchr(s, '\n');
        size_t len = end ? (size_t)(end - s) : strlen(s);
        indent(b, depth + 1);
        put(b, "%.*s\n", (int)len, s);
        s += len + (end != NULL);
    }
    indent(b, depth);
    put(b, "}\n");
}
static void put_struct(RecBuf* b, const RecDesc* d, const void* s, int depth);
/* a handle's block from map, or unrecorded */
static void put_handle(RecBuf* b, const RecMap* m, u64 h, int depth) {
    const RecEntry* e = map_find(m, h);
    if (e && e->text)
        paste(b, e->text, depth);
    else
        put(b, "unrecorded\n");
}
static void put_chain(RecBuf* b, const void* next, int depth) {
    for (const VkBaseInStructure* p = (const VkBaseInStructure*)next; p; p = p->pNext) {
        const RecDesc* d = rec_next_desc(p->sType);
        indent(b, depth);
        if (!d) {
            put(b, "next unrecorded %d\n", (int)p->sType);
            continue;
        }
        put(b, "next %s {\n", d->name);
        put_struct(b, d, p, depth + 1);
        indent(b, depth);
        put(b, "}\n");
    }
    indent(b, depth);
    put(b, "next -\n");
}
static void put_field(RecBuf* b, const RecField* f, const unsigned char* s, int depth) {
    const void* at = s + f->off;
    const void* ptr = NULL;
    u32 count = 0;
    if (f->kind == RK_STR || (f->kind >= RK_PTR && f->kind != RK_FEATS && f->kind != RK_NULLH))
        memcpy(&ptr, at, sizeof ptr);
    if (f->kind == RK_ARR || f->kind == RK_U32S || f->kind == RK_F32S || f->kind == RK_STRS ||
        f->kind == RK_SETLAYOUTS || f->kind == RK_SAMPLERS || f->kind == RK_LIBS || f->kind == RK_FEEDBACKN)
        memcpy(&count, s + f->off2, sizeof count);
    if (f->kind == RK_NEXT) {
        put_chain(b, *(const void* const*)at, depth);
        return;
    }
    indent(b, depth);
    put(b, "%s", f->name);
    switch (f->kind) {
        case RK_U32: put(b, " %u\n", *(const u32*)at); break;
        case RK_I32: put(b, " %d\n", *(const int32_t*)at); break;
        case RK_F32: put(b, " %.9g\n", (double)*(const float*)at); break;
        case RK_U64: put(b, " %llu\n", (unsigned long long)*(const u64*)at); break;
        case RK_SIZE: put(b, " %zu\n", *(const size_t*)at); break;
        case RK_STR: put(b, " %s\n", ptr ? (const char*)ptr : "-"); break;
        case RK_F32N:
            for (int i = 0; i < f->n; i++) put(b, " %.9g", (double)((const float*)at)[i]);
            put(b, "\n");
            break;
        case RK_U32N:
            for (int i = 0; i < f->n; i++) put(b, " %u", ((const u32*)at)[i]);
            put(b, "\n");
            break;
        case RK_SUB:
            put(b, " {\n");
            put_struct(b, f->desc, at, depth + 1);
            indent(b, depth);
            put(b, "}\n");
            break;
        case RK_PTR:
            if (!ptr) {
                put(b, " -\n");
                break;
            }
            put(b, " {\n");
            put_struct(b, f->desc, ptr, depth + 1);
            indent(b, depth);
            put(b, "}\n");
            break;
        case RK_ARR:
            put(b, " %u%s\n", count, ptr ? "" : " -");
            for (u32 i = 0; ptr && i < count; i++) {
                indent(b, depth + 1);
                put(b, "{\n");
                put_struct(b, f->desc, (const unsigned char*)ptr + (size_t)i * f->desc->size, depth + 2);
                indent(b, depth + 1);
                put(b, "}\n");
            }
            break;
        case RK_U32S:
            put(b, " %u", count);
            if (!ptr) put(b, " -");
            for (u32 i = 0; ptr && i < count; i++) put(b, " %u", ((const u32*)ptr)[i]);
            put(b, "\n");
            break;
        case RK_F32S:
            put(b, " %u", count);
            if (!ptr) put(b, " -");
            for (u32 i = 0; ptr && i < count; i++) put(b, " %.9g", (double)((const float*)ptr)[i]);
            put(b, "\n");
            break;
        case RK_BYTES: {
            size_t size;
            memcpy(&size, s + f->off2, sizeof size);
            put(b, " %zu", size);
            if (!ptr)
                put(b, " -");
            else if (size)
                put(b, " ");
            for (size_t i = 0; ptr && i < size; i++) put(b, "%02x", ((const unsigned char*)ptr)[i]);
            put(b, "\n");
            break;
        }
        case RK_STRS:
            put(b, " %u\n", count);
            for (u32 i = 0; ptr && i < count; i++) {
                indent(b, depth + 1);
                put(b, "%s\n", ((const char* const*)ptr)[i]);
            }
            break;
        case RK_MODULE: {
            u64 h = *(const u64*)at;
            const RecEntry* e = h ? map_find(&g_pipeline_record.modules, h) : NULL;
            if (!h)
                put(b, " -\n");
            else if (e)
                put(b, " %016llx\n", (unsigned long long)e->hash);
            else
                put(b, " unrecorded\n");
            break;
        }
        case RK_CODE: {
            size_t size;
            memcpy(&size, s + f->off2, sizeof size);
            put(b, " %016llx\n", (unsigned long long)snx_fnv1a(SNX_FNV_BASIS, ptr, ptr ? size : 0));
            break;
        }
        case RK_LAYOUT: {
            u64 h = *(const u64*)at;
            if (!h) {
                put(b, " -\n");
                break;
            }
            put(b, " ");
            put_handle(b, &g_pipeline_record.layouts, h, depth);
            break;
        }
        case RK_SETLAYOUTS:
            put(b, " %u\n", count);
            for (u32 i = 0; ptr && i < count; i++) {
                indent(b, depth + 1);
                put_handle(b, &g_pipeline_record.set_layouts, ((const u64*)ptr)[i], depth + 1);
            }
            break;
        case RK_SAMPLERS:
            if (!ptr) {
                put(b, " -\n");
                break;
            }
            put(b, " %u\n", count);
            for (u32 i = 0; i < count; i++) {
                indent(b, depth + 1);
                put_handle(b, &g_pipeline_record.samplers, ((const u64*)ptr)[i], depth + 1);
            }
            break;
        case RK_LIBS:
            put(b, " %u\n", count);
            for (u32 i = 0; ptr && i < count; i++) {
                indent(b, depth + 1);
                put_handle(b, &g_pipeline_record.libraries, ((const u64*)ptr)[i], depth + 1);
            }
            break;
        case RK_NULLH: put(b, *(const u64*)at ? " unrecorded\n" : " -\n"); break;
        case RK_MASK: {
            u32 samples;
            memcpy(&samples, s + f->off2, sizeof samples);
            u32 words = (samples + 31) / 32;
            if (!ptr) {
                put(b, " -\n");
                break;
            }
            put(b, " %u", words);
            for (u32 i = 0; i < words; i++) put(b, " %u", ((const u32*)ptr)[i]);
            put(b, "\n");
            break;
        }
        case RK_FEEDBACK1: put(b, " %s\n", ptr ? "out" : "-"); break;
        case RK_FEEDBACKN: put(b, " %s\n", ptr ? "out" : "-"); break;
        default: put(b, " unrecorded\n"); break;
    }
}
static void put_struct(RecBuf* b, const RecDesc* d, const void* s, int depth) {
    for (int i = 0; i < d->count; i++) {
        const RecField* f = &d->fields[i];
        if (f->kind == RK_FEATS) {
            indent(b, depth);
            put(b, "%s ", f->name);
            for (size_t o = f->off; o + sizeof(VkBool32) <= d->size; o += sizeof(VkBool32)) {
                VkBool32 v;
                memcpy(&v, (const unsigned char*)s + o, sizeof v);
                put(b, "%c", v ? '1' : '0');
            }
            put(b, "\n");
            continue;
        }
        put_field(b, f, (const unsigned char*)s, depth);
    }
}
/* d's members of s as a text of its own, for a handle's entry; NULL without memory */
static char* struct_text(const RecDesc* d, const void* s) {
    RecBuf b = {0};
    put_struct(&b, d, s, 0);
    if (b.bad) {
        free(b.p);
        return NULL;
    }
    if (b.n && b.p[b.n - 1] == '\n') b.p[--b.n] = 0; /* paste puts each line's own */
    return b.p ? b.p : snx_strdup("");
}

/* ---- the commands, in the driver's place */
static void keep_text(RecMap* m, u64 handle, const RecDesc* d, const void* info) {
    pm_lock(&g_pipeline_record.lock);
    RecEntry* e = map_put(m, handle);
    if (e) e->text = struct_text(d, info);
    pm_unlock(&g_pipeline_record.lock);
}
static void drop_handle(RecMap* m, u64 handle) {
    pm_lock(&g_pipeline_record.lock);
    map_drop(m, handle);
    pm_unlock(&g_pipeline_record.lock);
}
static void write_why(const char* path, const void* data, size_t size, const char* what) {
    char why[512];
    if (snx_vk_write_replace(path, data, size, why, sizeof why))
        WARN_LIMIT(16, "vulkan: pipeline_records: %s can't be written (%s): %s goes unrecorded\n", path, why, what);
}
static VkResult VKAPI_CALL rec_module(VkDevice dev, const VkShaderModuleCreateInfo* info,
                                      const VkAllocationCallbacks* alloc, VkShaderModule* out) {
    VkResult r = g_pipeline_record.create_module(dev, info, alloc, out);
    if (r != VK_SUCCESS) return r;
    u64 hash = snx_fnv1a(SNX_FNV_BASIS, info->pCode, info->codeSize);
    char name[64];
    snprintf(name, sizeof name, "spirv/%016llx.spv", (unsigned long long)hash);
    char* path = snx_path_join(g_pipeline_record.dir, name);
    if (path && !snx_is_file(path)) write_why(path, info->pCode, info->codeSize, "a shader module's SPIR-V");
    free(path);
    pm_lock(&g_pipeline_record.lock);
    RecEntry* e = map_put(&g_pipeline_record.modules, (u64)*out);
    if (e) e->hash = hash;
    pm_unlock(&g_pipeline_record.lock);
    return r;
}
static void VKAPI_CALL rec_module_gone(VkDevice dev, VkShaderModule m, const VkAllocationCallbacks* alloc) {
    drop_handle(&g_pipeline_record.modules, (u64)m);
    g_pipeline_record.destroy_module(dev, m, alloc);
}
static VkResult VKAPI_CALL rec_sampler(VkDevice dev, const VkSamplerCreateInfo* info,
                                       const VkAllocationCallbacks* alloc, VkSampler* out) {
    VkResult r = g_pipeline_record.create_sampler(dev, info, alloc, out);
    if (r == VK_SUCCESS) keep_text(&g_pipeline_record.samplers, (u64)*out, &d_sampler, info);
    return r;
}
static void VKAPI_CALL rec_sampler_gone(VkDevice dev, VkSampler s, const VkAllocationCallbacks* alloc) {
    drop_handle(&g_pipeline_record.samplers, (u64)s);
    g_pipeline_record.destroy_sampler(dev, s, alloc);
}
static VkResult VKAPI_CALL rec_set_layout(VkDevice dev, const VkDescriptorSetLayoutCreateInfo* info,
                                          const VkAllocationCallbacks* alloc, VkDescriptorSetLayout* out) {
    VkResult r = g_pipeline_record.create_set_layout(dev, info, alloc, out);
    if (r == VK_SUCCESS) keep_text(&g_pipeline_record.set_layouts, (u64)*out, &d_set_layout, info);
    return r;
}
static void VKAPI_CALL rec_set_layout_gone(VkDevice dev, VkDescriptorSetLayout l, const VkAllocationCallbacks* alloc) {
    drop_handle(&g_pipeline_record.set_layouts, (u64)l);
    g_pipeline_record.destroy_set_layout(dev, l, alloc);
}
static VkResult VKAPI_CALL rec_layout(VkDevice dev, const VkPipelineLayoutCreateInfo* info,
                                      const VkAllocationCallbacks* alloc, VkPipelineLayout* out) {
    VkResult r = g_pipeline_record.create_layout(dev, info, alloc, out);
    if (r == VK_SUCCESS) keep_text(&g_pipeline_record.layouts, (u64)*out, &d_layout, info);
    return r;
}
static void VKAPI_CALL rec_layout_gone(VkDevice dev, VkPipelineLayout l, const VkAllocationCallbacks* alloc) {
    drop_handle(&g_pipeline_record.layouts, (u64)l);
    g_pipeline_record.destroy_layout(dev, l, alloc);
}
static void VKAPI_CALL rec_pipeline_gone(VkDevice dev, VkPipeline p, const VkAllocationCallbacks* alloc) {
    drop_handle(&g_pipeline_record.libraries, (u64)p);
    g_pipeline_record.destroy_pipeline(dev, p, alloc);
}
/* whether info makes a library (its flags, or a chained VkPipelineCreateFlags2CreateInfo's, which take their place) */
static int makes_library(const VkGraphicsPipelineCreateInfo* info) {
    for (const VkBaseInStructure* p = (const VkBaseInStructure*)info->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO)
            return (((const VkPipelineCreateFlags2CreateInfo*)p)->flags & VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR) != 0;
    return (info->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) != 0;
}
/* the record's own copies of device.txt and of each SPIR-V it names, so its folder goes elsewhere whole */
static void keep_whole(const char* folder, const char* text) {
    char* from = snx_path_join(g_pipeline_record.dir, "device.txt");
    char* to = snx_path_join(folder, "device.txt");
    if (from && to && snx_copy_file(from, to))
        WARN("vulkan: pipeline_records: %s can't be copied to %s: the record reads it from the folder above\n", from,
             to);
    free(from);
    free(to);
    for (const char* s = text; *s; s++) {
        if (s != text && s[-1] != '\n' && s[-1] != ' ') continue;
        size_t skip = !strncmp(s, "module ", 7) ? 7 : !strncmp(s, "pCode ", 6) ? 6 : 0;
        char hash[17];
        if (!skip || sscanf(s + skip, "%16[0-9a-f]", hash) != 1 || strlen(hash) != 16) continue;
        char name[64];
        snprintf(name, sizeof name, "spirv/%s.spv", hash);
        char* spv = snx_path_join(g_pipeline_record.dir, name);
        char* copy = snx_path_join(folder, name + 6);
        if (spv && copy && !snx_is_file(copy) && snx_copy_file(spv, copy))
            WARN("vulkan: pipeline_records: %s can't be copied to %s: the record reads it from the folder above\n", spv,
                 copy);
        free(spv);
        free(copy);
    }
}
static VkResult VKAPI_CALL rec_make(VkDevice dev, VkPipelineCache cache, uint32_t count,
                                    const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* alloc,
                                    VkPipeline* out) {
    char oe[160];
    RecBuf b = {0};
    char** texts = (char**)calloc(count ? count : 1, sizeof *texts);
    int whole = texts != NULL;
    put(&b, "pipelines %u\n", count);
    pm_lock(&g_pipeline_record.lock);
    for (u32 i = 0; whole && i < count; i++) {
        texts[i] = struct_text(&d_graphics, &infos[i]);
        whole = texts[i] != NULL;
        if (!whole) break;
        put(&b, "VkGraphicsPipelineCreateInfo ");
        paste(&b, texts[i], 0);
    }
    pm_unlock(&g_pipeline_record.lock);
    char name[80];
    snprintf(name, sizeof name, "%s-%u", g_pipeline_record.run,
             atomic_fetch_add_explicit(&g_pipeline_record.seq, 1, memory_order_relaxed));
    char* folder = snx_path_join(g_pipeline_record.dir, name);
    char* text_path = folder ? snx_path_join(folder, "pipeline.txt") : NULL;
    int written = 0;
    if (b.bad || !whole || !text_path)
        WARN_LIMIT(16, "vulkan: pipeline_records: no memory for a pipeline's record: it goes unrecorded\n");
    else if (snx_mkdirs(folder))
        WARN_LIMIT(16, "vulkan: pipeline_records: %s can't be made (%s): this pipeline goes unrecorded\n", folder,
                   snx_os_error(oe, sizeof oe));
    else {
        write_why(text_path, b.p, b.n, "a pipeline's record");
        written = snx_is_file(text_path);
    }
    char was[PLAT_THREAD_NOTE_SIZE];
    snprintf(was, sizeof was, "%s", plat_thread_note_get());
    if (written) plat_thread_note("%s, its record in %s", was[0] ? was : "making a graphics pipeline", folder);
    VkResult r = g_pipeline_record.make(dev, cache, count, infos, alloc, out);
    if (was[0])
        plat_thread_note("%s", was);
    else
        plat_thread_note_clear();
    const char* what = count == 1 ? "a graphics pipeline" : "graphics pipelines";
    if (r < 0 && written) {
        keep_whole(folder, b.p);
        WARN("vulkan: the driver failed %s with %s (%d): the record is in %s\n", what, snx_vk_result_str(r), (int)r,
             folder);
    } else if (r < 0) {
        WARN("vulkan: the driver failed %s with %s (%d), and its record could not be written\n", what,
             snx_vk_result_str(r), (int)r);
    } else {
        /* a library's block, for the records of the links made of it */
        pm_lock(&g_pipeline_record.lock);
        for (u32 i = 0; whole && i < count; i++) {
            if (!makes_library(&infos[i]) || !out[i]) continue;
            RecEntry* e = map_put(&g_pipeline_record.libraries, (u64)out[i]);
            if (e) e->text = texts[i], texts[i] = NULL;
        }
        pm_unlock(&g_pipeline_record.lock);
        if (written && snx_remove_tree(folder))
            WARN_LIMIT(16, "vulkan: pipeline_records: %s can't be taken away (%s): the driver made its pipeline\n",
                       folder, snx_os_error(oe, sizeof oe));
    }
    for (u32 i = 0; texts && i < count; i++) free(texts[i]);
    free(texts);
    free(folder);
    free(text_path);
    free(b.p);
    return r;
}
static VkResult VKAPI_CALL rec_device(VkPhysicalDevice phys, const VkDeviceCreateInfo* info,
                                      const VkAllocationCallbacks* alloc, VkDevice* out) {
    VkResult r = g_pipeline_record.create_device(phys, info, alloc, out);
    if (r != VK_SUCCESS) return r;
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(phys, &p);
    RecBuf b = {0};
    put(&b, "vendorID %u\ndeviceID %u\ndriverVersion %u\napiVersion %u\ndeviceName %s\nVkDeviceCreateInfo {\n",
        p.vendorID, p.deviceID, p.driverVersion, p.apiVersion, p.deviceName);
    put_struct(&b, &d_device, info, 1);
    put(&b, "}\n");
    char* path = snx_path_join(g_pipeline_record.dir, "device.txt");
    if (b.bad || !path)
        WARN("vulkan: pipeline_records: no memory for device.txt: the records cannot make their device again\n");
    else
        write_why(path, b.p, b.n, "the device");
    free(path);
    free(b.p);
    return r;
}

int vk_pipeline_record_start(void) {
    char oe[160];
    char* spirv = snx_path_join(g_settings.pipeline_records, "spirv");
    if (!spirv || snx_mkdirs(spirv)) {
        WARN("vulkan: pipeline_records: %s can't be made (%s): no pipeline records\n",
             spirv ? spirv : g_settings.pipeline_records, snx_os_error(oe, sizeof oe));
        free(spirv);
        return -1;
    }
    free(spirv);
    snprintf(g_pipeline_record.dir, sizeof g_pipeline_record.dir, "%s", g_settings.pipeline_records);
    snprintf(g_pipeline_record.run, sizeof g_pipeline_record.run, "%llx", (unsigned long long)plat_ns());
    g_pipeline_record.create_device = vkCreateDevice;
    vkCreateDevice = rec_device;
    g_pipeline_record.on = 1;
    LOG("vulkan: pipeline_records: each graphics pipeline is written to %s before the driver makes it, and kept "
        "there when the driver fails it or the run ends inside it\n",
        g_pipeline_record.dir);
    return 0;
}
void vk_pipeline_record_device(void) {
    if (!g_pipeline_record.on) return;
    g_pipeline_record.create_module = vkCreateShaderModule, vkCreateShaderModule = rec_module;
    g_pipeline_record.destroy_module = vkDestroyShaderModule, vkDestroyShaderModule = rec_module_gone;
    g_pipeline_record.create_sampler = vkCreateSampler, vkCreateSampler = rec_sampler;
    g_pipeline_record.destroy_sampler = vkDestroySampler, vkDestroySampler = rec_sampler_gone;
    g_pipeline_record.create_set_layout = vkCreateDescriptorSetLayout, vkCreateDescriptorSetLayout = rec_set_layout;
    g_pipeline_record.destroy_set_layout = vkDestroyDescriptorSetLayout;
    vkDestroyDescriptorSetLayout = rec_set_layout_gone;
    g_pipeline_record.create_layout = vkCreatePipelineLayout, vkCreatePipelineLayout = rec_layout;
    g_pipeline_record.destroy_layout = vkDestroyPipelineLayout, vkDestroyPipelineLayout = rec_layout_gone;
    g_pipeline_record.driver_make = g_pipeline_record.make = vkCreateGraphicsPipelines;
    vkCreateGraphicsPipelines = rec_make;
    g_pipeline_record.destroy_pipeline = vkDestroyPipeline, vkDestroyPipeline = rec_pipeline_gone;
}
PFN_vkCreateGraphicsPipelines vk_pipeline_record_inner(PFN_vkCreateGraphicsPipelines make) {
    PFN_vkCreateGraphicsPipelines was = g_pipeline_record.make;
    g_pipeline_record.make = make ? make : g_pipeline_record.driver_make;
    return was;
}
