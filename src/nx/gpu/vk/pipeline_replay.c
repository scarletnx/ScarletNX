/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A pipeline record (pipeline_record.h) read back and its pipelines made again, on a device of the replay's own: its
   commands are looked up for it, so volk's, the renderer's, stay as they are */
#include "nx/gpu/vk/pipeline_record.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/gpu/vk/vk_common.h"
#include "common/utilities/bits.h"
#include "common/utilities/common.h"

#include "nx/gpu/vk/pipeline_record_format.inc"

typedef struct RecChunk {
    struct RecChunk* next;
    size_t used, cap;
    max_align_t data[];
} RecChunk;
/* the objects made for the pipelines, a kind each, destroyed at the end */
enum { RO_MODULE, RO_SAMPLER, RO_SET_LAYOUT, RO_LAYOUT, RO_PIPELINE, RO_KINDS };
typedef struct {
    u64* h;
    u64* hash; /* RO_MODULE's SPIR-V hashes, to make each module once */
    size_t n, cap;
} RecObjects;
typedef struct {
    char* text; /* the file being read, its lines cut in place */
    char* at;
    const char* file;
    int line;
    const char* dir;
    char* why;
    size_t why_n;
    int failed;
    RecChunk* arena;
    VkDevice dev;
    RecObjects made[RO_KINDS];
    PFN_vkCreateShaderModule create_module;
    PFN_vkDestroyShaderModule destroy_module;
    PFN_vkCreateSampler create_sampler;
    PFN_vkDestroySampler destroy_sampler;
    PFN_vkCreateDescriptorSetLayout create_set_layout;
    PFN_vkDestroyDescriptorSetLayout destroy_set_layout;
    PFN_vkCreatePipelineLayout create_layout;
    PFN_vkDestroyPipelineLayout destroy_layout;
    PFN_vkCreateGraphicsPipelines create_pipelines;
    PFN_vkDestroyPipeline destroy_pipeline;
} RecReader;

[[gnu::format(printf, 2, 3)]] static int fail(RecReader* r, const char* fmt, ...) {
    if (r->failed) return -1;
    r->failed = 1;
    int w = snprintf(r->why, r->why_n, "%s:%d: ", r->file, r->line);
    va_list a;
    va_start(a, fmt);
    if (w >= 0 && (size_t)w < r->why_n) vsnprintf(r->why + w, r->why_n - (size_t)w, fmt, a);
    va_end(a);
    return -1;
}
static void* take(RecReader* r, size_t size) {
    size = (size + sizeof(max_align_t) - 1) / sizeof(max_align_t) * sizeof(max_align_t);
    if (!r->arena || r->arena->used + size > r->arena->cap) {
        size_t cap = size > 65536 ? size : 65536;
        RecChunk* c = (RecChunk*)malloc(sizeof *c + cap);
        if (!c) {
            fail(r, "no memory");
            return NULL;
        }
        c->next = r->arena, c->used = 0, c->cap = cap;
        r->arena = c;
    }
    void* p = (unsigned char*)r->arena->data + r->arena->used;
    r->arena->used += size;
    memset(p, 0, size);
    return p;
}
/* the next line that holds anything: its first word in *name, the rest after one space in *rest ("" for none) */
static int next_line(RecReader* r, char** name, char** rest) {
    while (*r->at) {
        char* s = r->at;
        char* end = strchr(s, '\n');
        if (end) *end = 0;
        r->at = end ? end + 1 : s + strlen(s);
        r->line++;
        size_t len = strlen(s);
        if (len && s[len - 1] == '\r') s[--len] = 0;
        while (*s == ' ') s++;
        if (!*s) continue;
        char* space = strchr(s, ' ');
        if (space) *space = 0;
        *name = s;
        *rest = space ? space + 1 : s + strlen(s);
        return 1;
    }
    return 0;
}
static int expect(RecReader* r, const char* want, char** rest) {
    char* name;
    if (!next_line(r, &name, rest)) return fail(r, "the record ends where %s was to be", want);
    if (strcmp(name, want)) return fail(r, "%s was to be here, not \"%s\"", want, name);
    return 0;
}
static int closes(RecReader* r) {
    char *name, *rest;
    if (!next_line(r, &name, &rest) || strcmp(name, "}")) return fail(r, "a \"}\" was to end the block here");
    return 0;
}
static int opens(RecReader* r, const char* rest) {
    return strcmp(rest, "{") ? fail(r, "a block (\"{\") was to start here, not \"%s\"", rest) : 0;
}
static int keep(RecReader* r, int kind, u64 h, u64 hash) {
    RecObjects* o = &r->made[kind];
    if (o->n == o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 16;
        u64* nh = (u64*)realloc(o->h, cap * sizeof *nh);
        if (nh) o->h = nh;
        u64* nhash = nh ? (u64*)realloc(o->hash, cap * sizeof *nhash) : NULL;
        if (nhash) o->hash = nhash;
        if (!nh || !nhash) return fail(r, "no memory");
        o->cap = cap;
    }
    o->h[o->n] = h, o->hash[o->n] = hash;
    o->n++;
    return 0;
}
/* the SPIR-V of hash from the record's folder, else the spirv folder beside it */
static unsigned char* spirv_of(RecReader* r, const char* hash, size_t* size) {
    char name[64];
    snprintf(name, sizeof name, "%s.spv", hash);
    char* path = snx_path_join(r->dir, name);
    unsigned char* data = path ? snx_read_file(path, size) : NULL;
    if (!data) {
        char* up = snx_path_join(r->dir, "..");
        snprintf(name, sizeof name, "spirv/%s.spv", hash);
        char* there = up ? snx_path_join(up, name) : NULL;
        data = there ? snx_read_file(there, size) : NULL;
        free(up);
        free(there);
    }
    if (!data) fail(r, "the SPIR-V %s is neither in %s nor in its folder's spirv", hash, r->dir);
    free(path);
    return data;
}
static int get_struct(RecReader* r, const RecDesc* d, void* s);
/* a block holding d's members, made into an object of kind (RO_LAYOUT...) in *out */
static int get_object(RecReader* r, const RecDesc* d, int kind, const char* rest, u64* out);
static int get_chain(RecReader* r, void* s) {
    VkBaseOutStructure* last = (VkBaseOutStructure*)s;
    for (;;) {
        char* rest;
        if (expect(r, "next", &rest)) return -1;
        if (!strcmp(rest, "-")) return 0;
        if (!strncmp(rest, "unrecorded", 10))
            return fail(r, "a structure the recorder does not know (sType %s): the record cannot make it again",
                        rest + 10);
        char* brace = strrchr(rest, ' ');
        if (!brace || strcmp(brace, " {")) return fail(r, "\"next %s\" is not \"next Name {\"", rest);
        *brace = 0;
        const RecDesc* d = rec_next_named(rest);
        if (!d) return fail(r, "%s is no structure the replay knows", rest);
        VkBaseOutStructure* node = (VkBaseOutStructure*)take(r, d->size);
        if (!node || get_struct(r, d, node) || closes(r)) return -1;
        last->pNext = node;
        last = node;
    }
}
static int count_of(RecReader* r, const char* rest, u32* n, int* none) {
    char* end;
    unsigned long v = strtoul(rest, &end, 10);
    if (end == rest || v > 0xffffffffu) return fail(r, "\"%s\" is not a count", rest);
    *n = (u32)v;
    while (*end == ' ') end++;
    *none = !strncmp(end, "-", 1);
    return 0;
}
static int get_field(RecReader* r, const RecField* f, unsigned char* s, const RecDesc* in) {
    void* at = s + f->off;
    if (f->kind == RK_NEXT) return get_chain(r, s);
    char* rest;
    if (expect(r, f->name, &rest)) return -1;
    u32 n = 0;
    int none = 0;
    void* ptr = NULL;
    char* end;
    switch (f->kind) {
        case RK_U32:
            *(u32*)at = (u32)strtoul(rest, &end, 10);
            return end == rest ? fail(r, "%s: no number", f->name) : 0;
        case RK_I32:
            *(int32_t*)at = (int32_t)strtol(rest, &end, 10);
            return end == rest ? fail(r, "%s: no number", f->name) : 0;
        case RK_F32: *(float*)at = strtof(rest, &end); return end == rest ? fail(r, "%s: no number", f->name) : 0;
        case RK_U64: *(u64*)at = strtoull(rest, &end, 10); return end == rest ? fail(r, "%s: no number", f->name) : 0;
        case RK_SIZE:
            *(size_t*)at = (size_t)strtoull(rest, &end, 10);
            return end == rest ? fail(r, "%s: no number", f->name) : 0;
        case RK_STR: {
            if (!strcmp(rest, "-")) return 0;
            char* copy = (char*)take(r, strlen(rest) + 1);
            if (!copy) return -1;
            strcpy(copy, rest);
            memcpy(at, &copy, sizeof copy);
            return 0;
        }
        case RK_F32N:
        case RK_U32N:
            for (int i = 0; i < f->n; i++) {
                if (f->kind == RK_F32N)
                    ((float*)at)[i] = strtof(rest, &end);
                else
                    ((u32*)at)[i] = (u32)strtoul(rest, &end, 10);
                if (end == rest) return fail(r, "%s: %d numbers were to be here", f->name, f->n);
                rest = end;
            }
            return 0;
        case RK_SUB: return opens(r, rest) || get_struct(r, f->desc, at) || closes(r) ? -1 : 0;
        case RK_PTR:
            if (!strcmp(rest, "-")) return 0;
            if (opens(r, rest) || !(ptr = take(r, f->desc->size)) || get_struct(r, f->desc, ptr) || closes(r))
                return -1;
            memcpy(at, &ptr, sizeof ptr);
            return 0;
        case RK_ARR:
            if (count_of(r, rest, &n, &none)) return -1;
            memcpy(s + f->off2, &n, sizeof n);
            if (none || !n) return 0;
            if (!(ptr = take(r, (size_t)n * f->desc->size))) return -1;
            for (u32 i = 0; i < n; i++) {
                char *name, *more;
                if (!next_line(r, &name, &more) || strcmp(name, "{"))
                    return fail(r, "%s's element %u was to start here", f->name, i);
                if (get_struct(r, f->desc, (unsigned char*)ptr + (size_t)i * f->desc->size) || closes(r)) return -1;
            }
            memcpy(at, &ptr, sizeof ptr);
            return 0;
        case RK_U32S:
        case RK_F32S:
        case RK_MASK:
            if (!strcmp(rest, "-")) return 0;
            if (count_of(r, rest, &n, &none)) return -1;
            if (f->kind != RK_MASK) memcpy(s + f->off2, &n, sizeof n);
            if (none || !n) return 0;
            if (!(ptr = take(r, (size_t)n * 4))) return -1;
            strtoul(rest, &end, 10);
            rest = end;
            for (u32 i = 0; i < n; i++) {
                if (f->kind == RK_F32S)
                    ((float*)ptr)[i] = strtof(rest, &end);
                else
                    ((u32*)ptr)[i] = (u32)strtoul(rest, &end, 10);
                if (end == rest) return fail(r, "%s: %u values were to be here", f->name, n);
                rest = end;
            }
            memcpy(at, &ptr, sizeof ptr);
            return 0;
        case RK_BYTES: {
            size_t size = (size_t)strtoull(rest, &end, 10);
            if (end == rest) return fail(r, "%s: no size", f->name);
            memcpy(s + f->off2, &size, sizeof size);
            while (*end == ' ') end++;
            if (*end == '-' || !size) return 0;
            if (strlen(end) != size * 2 || !(ptr = take(r, size)))
                return fail(r, "%s: %zu bytes were to be here", f->name, size);
            for (size_t i = 0; i < size; i++) {
                unsigned v;
                if (sscanf(end + 2 * i, "%2x", &v) != 1) return fail(r, "%s: byte %zu is not hex", f->name, i);
                ((unsigned char*)ptr)[i] = (unsigned char)v;
            }
            memcpy(at, &ptr, sizeof ptr);
            return 0;
        }
        case RK_STRS: {
            if (count_of(r, rest, &n, &none)) return -1;
            memcpy(s + f->off2, &n, sizeof n);
            if (!n) return 0;
            char** list = (char**)take(r, (size_t)n * sizeof *list);
            if (!list) return -1;
            for (u32 i = 0; i < n; i++) {
                char *name, *more;
                if (!next_line(r, &name, &more)) return fail(r, "%s: %u names were to be here", f->name, n);
                if (*more) more[-1] = ' '; /* a name with a space, whole */
                if (!(list[i] = (char*)take(r, strlen(name) + 1))) return -1;
                strcpy(list[i], name);
            }
            memcpy(at, &list, sizeof list);
            return 0;
        }
        case RK_MODULE: {
            if (!strcmp(rest, "-")) return 0;
            if (!strcmp(rest, "unrecorded"))
                return fail(r, "a stage's shader module was made before the recorder: its SPIR-V is not in the record");
            u64 hash = strtoull(rest, &end, 16);
            RecObjects* o = &r->made[RO_MODULE];
            for (size_t i = 0; i < o->n; i++)
                if (o->hash[i] == hash) {
                    memcpy(at, &o->h[i], sizeof(u64));
                    return 0;
                }
            size_t size;
            unsigned char* code = spirv_of(r, rest, &size);
            if (!code) return -1;
            VkShaderModuleCreateInfo mi = {
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = (const uint32_t*)code};
            VkShaderModule m;
            VkResult v = r->create_module(r->dev, &mi, NULL, &m);
            free(code);
            if (v != VK_SUCCESS)
                return fail(r, "the driver made no shader module of %s: %s", rest, snx_vk_result_str(v));
            memcpy(at, &m, sizeof m);
            return keep(r, RO_MODULE, (u64)m, hash);
        }
        case RK_CODE: {
            size_t size;
            unsigned char* code = spirv_of(r, rest, &size);
            if (!code) return -1;
            void* copy = take(r, size);
            if (copy) memcpy(copy, code, size);
            free(code);
            if (!copy) return -1;
            memcpy(s + f->off2, &size, sizeof size);
            memcpy(at, &copy, sizeof copy);
            return 0;
        }
        case RK_LAYOUT:
            if (!strcmp(rest, "-")) return 0;
            return get_object(r, &d_layout, RO_LAYOUT, rest, (u64*)at);
        case RK_SETLAYOUTS:
        case RK_SAMPLERS:
        case RK_LIBS: {
            if (f->kind == RK_SAMPLERS && !strcmp(rest, "-")) return 0;
            if (count_of(r, rest, &n, &none)) return -1;
            if (f->kind != RK_SAMPLERS) memcpy(s + f->off2, &n, sizeof n);
            if (!n) return 0;
            u64* list = (u64*)take(r, (size_t)n * sizeof *list);
            if (!list) return -1;
            const RecDesc* d = f->kind == RK_SETLAYOUTS ? &d_set_layout
                               : f->kind == RK_SAMPLERS ? &d_sampler
                                                        : &d_graphics;
            int kind = f->kind == RK_SETLAYOUTS ? RO_SET_LAYOUT : f->kind == RK_SAMPLERS ? RO_SAMPLER : RO_PIPELINE;
            for (u32 i = 0; i < n; i++) {
                char *name, *more;
                if (!next_line(r, &name, &more)) return fail(r, "%s: %u blocks were to be here", f->name, n);
                if (*more) more[-1] = ' ';
                if (get_object(r, d, kind, name, &list[i])) return -1;
            }
            memcpy(at, &list, sizeof list);
            return 0;
        }
        case RK_NULLH: return strcmp(rest, "-") ? fail(r, "%s names an object the record does not hold", f->name) : 0;
        case RK_FEATS: {
            size_t o = f->off;
            for (const char* c = rest; *c == '0' || *c == '1'; c++, o += sizeof(VkBool32)) {
                if (o + sizeof(VkBool32) > in->size) return fail(r, "%s: more features than %s has", f->name, in->name);
                VkBool32 v = *c == '1';
                memcpy(s + o, &v, sizeof v);
            }
            return 0;
        }
        case RK_FEEDBACK1:
            if (!strcmp(rest, "-")) return 0;
            if (!(ptr = take(r, sizeof(VkPipelineCreationFeedback)))) return -1;
            memcpy(at, &ptr, sizeof ptr);
            return 0;
        case RK_FEEDBACKN:
            if (!strcmp(rest, "-")) return 0;
            memcpy(&n, s + f->off2, sizeof n);
            if (!(ptr = take(r, (size_t)(n ? n : 1) * sizeof(VkPipelineCreationFeedback)))) return -1;
            memcpy(at, &ptr, sizeof ptr);
            return 0;
        default: return fail(r, "%s is a member the replay cannot read", f->name);
    }
}
static int get_struct(RecReader* r, const RecDesc* d, void* s) {
    if (d->stype) ((VkBaseOutStructure*)s)->sType = d->stype;
    for (int i = 0; i < d->count; i++)
        if (get_field(r, &d->fields[i], (unsigned char*)s, d)) return -1;
    return 0;
}
static int get_object(RecReader* r, const RecDesc* d, int kind, const char* rest, u64* out) {
    if (!strcmp(rest, "unrecorded"))
        return fail(r, "an object made before the recorder, or one it does not know: the record does not hold it");
    void* info;
    if (opens(r, rest) || !(info = take(r, d->size)) || get_struct(r, d, info) || closes(r)) return -1;
    VkResult v;
    switch (kind) {
        case RO_SAMPLER: v = r->create_sampler(r->dev, (const VkSamplerCreateInfo*)info, NULL, (VkSampler*)out); break;
        case RO_SET_LAYOUT:
            v = r->create_set_layout(r->dev, (const VkDescriptorSetLayoutCreateInfo*)info, NULL,
                                     (VkDescriptorSetLayout*)out);
            break;
        case RO_LAYOUT:
            v = r->create_layout(r->dev, (const VkPipelineLayoutCreateInfo*)info, NULL, (VkPipelineLayout*)out);
            break;
        default:
            v = r->create_pipelines(r->dev, VK_NULL_HANDLE, 1, (const VkGraphicsPipelineCreateInfo*)info, NULL,
                                    (VkPipeline*)out);
            break;
    }
    if (v != VK_SUCCESS) return fail(r, "the driver made no %s: %s (%d)", d->name, snx_vk_result_str(v), (int)v);
    return keep(r, kind, *out, 0);
}
/* path's text, else the one of the same name in the folder above dir; NULL said in why */
static char* read_text(RecReader* r, const char* name) {
    char* path = snx_path_join(r->dir, name);
    size_t size;
    unsigned char* data = path ? snx_read_file(path, &size) : NULL;
    if (!data) {
        char* up = snx_path_join(r->dir, "..");
        char* there = up ? snx_path_join(up, name) : NULL;
        data = there ? snx_read_file(there, &size) : NULL;
        free(up);
        free(there);
    }
    free(path);
    char* text = data ? (char*)realloc(data, size + 1) : NULL;
    if (!text) {
        free(data);
        snprintf(r->why, r->why_n, "%s is neither in %s nor in the folder above it", name, r->dir);
        r->failed = 1;
        return NULL;
    }
    text[size] = 0;
    return text;
}
/* a window's extension: the replay makes no window, and its instance has no surface extension */
static int window_extension(const char* name) {
    static const char* const prefixes[] = {"VK_KHR_swapchain",
                                           "VK_EXT_swapchain",
                                           "VK_KHR_present",
                                           "VK_EXT_present",
                                           "VK_EXT_full_screen_exclusive",
                                           "VK_EXT_hdr_metadata",
                                           "VK_KHR_incremental_present",
                                           "VK_GOOGLE_display_timing",
                                           "VK_KHR_display_swapchain",
                                           "VK_EXT_display_control",
                                           "VK_KHR_shared_presentable_image",
                                           "VK_AMD_display_native_hdr"};
    for (size_t i = 0; i < sizeof prefixes / sizeof *prefixes; i++)
        if (!strncmp(name, prefixes[i], strlen(prefixes[i]))) return 1;
    return 0;
}
static void destroy_all(RecReader* r) {
    for (size_t i = r->made[RO_PIPELINE].n; i-- > 0;)
        r->destroy_pipeline(r->dev, (VkPipeline)r->made[RO_PIPELINE].h[i], NULL);
    for (size_t i = r->made[RO_LAYOUT].n; i-- > 0;)
        r->destroy_layout(r->dev, (VkPipelineLayout)r->made[RO_LAYOUT].h[i], NULL);
    for (size_t i = r->made[RO_SET_LAYOUT].n; i-- > 0;)
        r->destroy_set_layout(r->dev, (VkDescriptorSetLayout)r->made[RO_SET_LAYOUT].h[i], NULL);
    for (size_t i = r->made[RO_SAMPLER].n; i-- > 0;)
        r->destroy_sampler(r->dev, (VkSampler)r->made[RO_SAMPLER].h[i], NULL);
    for (size_t i = r->made[RO_MODULE].n; i-- > 0;)
        r->destroy_module(r->dev, (VkShaderModule)r->made[RO_MODULE].h[i], NULL);
    for (int k = 0; k < RO_KINDS; k++) {
        free(r->made[k].h);
        free(r->made[k].hash);
    }
}
#define DEVICE_FN(r, gdpa, name, field) (r)->field = (PFN_##name)(gdpa)((r)->dev, #name)
int vk_pipeline_replay(VkInstance instance, const char* dir, VkResult* made, char* why, size_t n) {
    RecReader r = {.dir = dir, .why = why, .why_n = n, .file = "device.txt"};
    why[0] = 0;
    VkInstance own = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    char *device_text = NULL, *pipeline_text = NULL;
    PFN_vkDestroyDevice destroy_device = NULL;
    PFN_vkDestroyInstance destroy_instance = NULL;
    int ok = -1;
    /* the files first: a folder that holds no record is said before any driver is opened */
    if (!(device_text = read_text(&r, "device.txt")) || !(pipeline_text = read_text(&r, "pipeline.txt"))) goto done;
    if (!vkGetInstanceProcAddr) {
        snprintf(why, n, "no Vulkan loader (volkInitialize was not done, or found none)");
        goto done;
    }
    if (!instance) {
        PFN_vkCreateInstance create = (PFN_vkCreateInstance)vkGetInstanceProcAddr(NULL, "vkCreateInstance");
        VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                 .pApplicationName = "snx pipeline replay",
                                 .apiVersion = VK_API_VERSION_1_3};
        VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
        VkResult v = create ? create(&ii, NULL, &own) : VK_ERROR_INITIALIZATION_FAILED;
        if (v != VK_SUCCESS) {
            snprintf(why, n, "no Vulkan 1.3 instance: %s (%d)", snx_vk_result_str(v), (int)v);
            goto done;
        }
        instance = own;
    }
#define INSTANCE_FN(name) PFN_##name name##_ = (PFN_##name)vkGetInstanceProcAddr(instance, #name)
    INSTANCE_FN(vkEnumeratePhysicalDevices);
    INSTANCE_FN(vkGetPhysicalDeviceProperties);
    INSTANCE_FN(vkCreateDevice);
    INSTANCE_FN(vkGetDeviceProcAddr);
    INSTANCE_FN(vkDestroyInstance);
#undef INSTANCE_FN
    destroy_instance = vkDestroyInstance_;
    r.text = r.at = device_text;
    char *rest, *name;
    u32 vendor = 0, device = 0;
    if (expect(&r, "vendorID", &rest)) goto done;
    vendor = (u32)strtoul(rest, NULL, 10);
    if (expect(&r, "deviceID", &rest)) goto done;
    device = (u32)strtoul(rest, NULL, 10);
    if (expect(&r, "driverVersion", &rest) || expect(&r, "apiVersion", &rest) || expect(&r, "deviceName", &rest))
        goto done;
    char recorded_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    snprintf(recorded_name, sizeof recorded_name, "%s", rest);
    if (expect(&r, "VkDeviceCreateInfo", &rest) || opens(&r, rest)) goto done;
    VkDeviceCreateInfo* di = (VkDeviceCreateInfo*)take(&r, sizeof *di);
    if (!di || get_struct(&r, &d_device, di) || closes(&r)) goto done;
    /* the window's extensions, and their features' structures, left out */
    u32 kept = 0;
    for (u32 i = 0; i < di->enabledExtensionCount; i++) {
        if (window_extension(di->ppEnabledExtensionNames[i]))
            printf("replay: %s left out of the device: a window's, and the replay makes none\n",
                   di->ppEnabledExtensionNames[i]);
        else
            ((const char**)di->ppEnabledExtensionNames)[kept++] = di->ppEnabledExtensionNames[i];
    }
    di->enabledExtensionCount = kept;
    for (VkBaseOutStructure* p = (VkBaseOutStructure*)di; p && p->pNext;) {
        const RecDesc* d = rec_next_desc(p->pNext->sType);
        if (d && (strstr(d->name, "Present") || strstr(d->name, "Swapchain"))) {
            printf("replay: %s left out of the device's features: its extension is a window's\n", d->name);
            p->pNext = p->pNext->pNext;
        } else {
            p = p->pNext;
        }
    }
    u32 count = 0;
    vkEnumeratePhysicalDevices_(instance, &count, NULL);
    VkPhysicalDevice* list = count ? (VkPhysicalDevice*)take(&r, count * sizeof *list) : NULL;
    if (!list) {
        if (!r.failed) snprintf(why, n, "the instance has no physical device");
        goto done;
    }
    vkEnumeratePhysicalDevices_(instance, &count, list);
    VkPhysicalDevice phys = list[0];
    VkPhysicalDeviceProperties p;
    int found = 0;
    for (u32 i = 0; i < count && !found; i++) {
        vkGetPhysicalDeviceProperties_(list[i], &p);
        if (p.vendorID == vendor && p.deviceID == device) phys = list[i], found = 1;
    }
    if (!found) {
        vkGetPhysicalDeviceProperties_(phys, &p);
        printf("replay: the record's GPU, %s (vendor %#x, device %#x), is not here: on %s (vendor %#x, device %#x)\n",
               recorded_name, vendor, device, p.deviceName, p.vendorID, p.deviceID);
    }
    VkResult v = vkCreateDevice_(phys, di, NULL, &dev);
    if (v != VK_SUCCESS) {
        snprintf(why, n, "the device of device.txt was not made: %s (%d)", snx_vk_result_str(v), (int)v);
        goto done;
    }
    r.dev = dev;
    PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr_;
    destroy_device = (PFN_vkDestroyDevice)gdpa(dev, "vkDestroyDevice");
    DEVICE_FN(&r, gdpa, vkCreateShaderModule, create_module);
    DEVICE_FN(&r, gdpa, vkDestroyShaderModule, destroy_module);
    DEVICE_FN(&r, gdpa, vkCreateSampler, create_sampler);
    DEVICE_FN(&r, gdpa, vkDestroySampler, destroy_sampler);
    DEVICE_FN(&r, gdpa, vkCreateDescriptorSetLayout, create_set_layout);
    DEVICE_FN(&r, gdpa, vkDestroyDescriptorSetLayout, destroy_set_layout);
    DEVICE_FN(&r, gdpa, vkCreatePipelineLayout, create_layout);
    DEVICE_FN(&r, gdpa, vkDestroyPipelineLayout, destroy_layout);
    DEVICE_FN(&r, gdpa, vkCreateGraphicsPipelines, create_pipelines);
    DEVICE_FN(&r, gdpa, vkDestroyPipeline, destroy_pipeline);
    r.file = "pipeline.txt", r.line = 0;
    r.text = r.at = pipeline_text;
    u32 pipelines;
    int none;
    if (expect(&r, "pipelines", &rest) || count_of(&r, rest, &pipelines, &none)) goto done;
    if (!pipelines) {
        snprintf(why, n, "pipeline.txt holds no pipeline");
        goto done;
    }
    VkGraphicsPipelineCreateInfo* infos = (VkGraphicsPipelineCreateInfo*)take(&r, pipelines * sizeof *infos);
    VkPipeline* out = (VkPipeline*)take(&r, pipelines * sizeof *out);
    if (!infos || !out) goto done;
    for (u32 i = 0; i < pipelines; i++)
        if (expect(&r, "VkGraphicsPipelineCreateInfo", &rest) || opens(&r, rest) ||
            get_struct(&r, &d_graphics, &infos[i]) || closes(&r))
            goto done;
    if (next_line(&r, &name, &rest)) {
        fail(&r, "\"%s\" after the last pipeline", name);
        goto done;
    }
    printf("replay: the driver makes the %u graphics pipeline%s of %s\n", pipelines, pipelines == 1 ? "" : "s", dir);
    fflush(stdout);
    *made = r.create_pipelines(dev, VK_NULL_HANDLE, pipelines, infos, NULL, out);
    for (u32 i = 0; *made >= 0 && i < pipelines; i++)
        if (out[i]) keep(&r, RO_PIPELINE, (u64)out[i], 0);
    ok = 0;
done:
    if (dev) {
        destroy_all(&r);
        destroy_device(dev, NULL);
    }
    while (r.arena) {
        RecChunk* next = r.arena->next;
        free(r.arena);
        r.arena = next;
    }
    free(device_text);
    free(pipeline_text);
    if (own && destroy_instance) destroy_instance(own, NULL);
    if (ok && !why[0]) snprintf(why, n, "the record could not be read");
    return ok;
}
