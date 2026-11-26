/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The pipeline a Vita draw takes (gpu/render.c, vita_render_pipeline_key), on a Vulkan device, in a scene with no
   draw recorded. A program released and another made in its place, its shader made where the first one's was freed,
   takes a pipeline of its own, not the first one's: the key names a shader by its program, not by its address. A
   fragment program that samples a texture takes a shader for each format of it, found again for a format it had,
   and one whose texture it does not sample changes nothing; its bytes are read at its first draw alone, not at each.
   prepare_shaders: a draw's shaders and pipeline go into the prepared file named for the driver, and a second run
   makes them from it, before its first draw (2) or on the workers (1, collected as a scene begins), so its draw
   makes no pipeline of its own; packed with compress_prepared_shaders; a record changed on disk ends the file there. The GXP programs are the translator's smallest: a
   header, and one with a cube sampler (vita_usse_cube.c's). Skips (77) without a Vulkan device */
#include "vita/gpu/gxm.h"
#include "vita/gpu/gxp.h"
#include "vita/gpu/render.h"
#include "vita/gpu/vk.h"
#include "vita/runtime/process.h"
#include "vita/runtime/watch.h"
#include "snx_settings.h"
#include "common/gpu/vk/prepared.h"
#include "common/gpu/vk/vk_common.h"
#include "common/platform/file.h"
#include "common/platform/vm.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_failures;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            ++g_failures;                                                                                              \
            fprintf(stderr, "FAIL line %d: ", __LINE__);                                                               \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

/* a process with its memory, its sceGxm state and the renderer, and nothing else of a game's */
static VitaProcess* process_make(const char* folder) {
    VitaProcess* p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->folder = folder ? snx_strdup(folder) : NULL; /* where the renderer keeps its files */
    p->user = (VitaRegion){VITA_USER_BASE, VITA_USER_END, VITA_USER_BASE};
    p->cdram = (VitaRegion){VITA_CDRAM_BASE, VITA_CDRAM_END, VITA_CDRAM_BASE};
    p->mem = plat_mem_reserve(VITA_SPACE + 0x10000, 0);
    if (plat_mem_page() > 0x1000u) p->page_refs = calloc(VITA_SPACE / plat_mem_page(), sizeof *p->page_refs);
    p->gxm = calloc(1, sizeof *p->gxm);
    if (!p->mem || !p->gxm || (plat_mem_page() > 0x1000u && !p->page_refs) || !vita_watch_init(p) ||
        !vita_render_init(p))
        return NULL;
    return p;
}
static void process_free(VitaProcess* p) {
    vita_render_free(p);
    vita_watch_free();
    plat_mem_release(p->mem, VITA_SPACE + 0x10000);
    free(p->page_refs);
    free(p->gxm);
    free(p->folder);
    free(p);
}
/* the smallest GXP the translator takes at at: its header, a vertex program's or a fragment one's, and mark in a byte
   no field reads, so that two of them are two programs */
static uint32_t program(VitaProcess* p, uint32_t at, bool fragment, unsigned char mark) {
    unsigned char* g = vita_ptr(p, at);
    memset(g, 0, 160);
    snx_st_le32(g, GXP_MAGIC);
    snx_st_le32(g + 8, 160);
    snx_st_le32(g + GXP_FLAGS, fragment ? GXP_FLAG_FRAGMENT : 0);
    g[156] = mark;
    return at;
}

/* two vertex programs, the second made after the first was released: two keys */
static void check_released(VitaProcess* p, GxmContext* c, uint32_t code) {
    VitaGxm* g = p->gxm;
    g->vertex[0] = (GxmVertexProgram){.references = 1, .program = program(p, code, false, 1)};
    g->fragment[0] =
        (GxmFragmentProgram){.references = 1, .program = program(p, code + 0x100, true, 0), .blend_info = 0xf};
    c->vertex_program = GXM_HANDLE_VERTEX;
    c->fragment_program = GXM_HANDLE_FRAGMENT;
    uint64_t first = vita_render_pipeline_key(p, c, 0);
    const uintptr_t freed = (uintptr_t)g->vertex[0].shader; /* its address alone, read before the free */
    /* as sceGxmShaderPatcherReleaseVertexProgram's last release does, then a creation in the same slot */
    vita_render_shader_free(p, g->vertex[0].shader);
    g->vertex[0] = (GxmVertexProgram){.references = 1, .program = program(p, code + 0x200, false, 2)};
    uint64_t second = vita_render_pipeline_key(p, c, 0);
    printf("released: keys %016llx then %016llx, the second program's shader %s where the first one's was freed\n",
           (unsigned long long)first, (unsigned long long)second,
           (uintptr_t)g->vertex[0].shader == freed ? "made" : "not made");
    CHECK(first && second && first != second, "the second program takes the first one's pipeline (key %016llx)",
          (unsigned long long)second);
}

/* vita_usse_cube.c's fragment program at at: texture 0 sampled through a cube sampler */
static uint32_t sampling(VitaProcess* p, uint32_t at) {
    unsigned char* g = vita_ptr(p, at);
    memset(g, 0, 216);
    snx_st_le32(g, GXP_MAGIC);
    snx_st_le32(g + 8, 216);
    snx_st_le32(g + GXP_FLAGS, GXP_FLAG_FRAGMENT);
    snx_st_le32(g + GXP_PARAMETER_COUNT, 1);
    snx_st_le32(g + GXP_PARAMETERS, 144); /* parameter record at 184 */
    snx_st_le32(g + GXP_PRIMARY_COUNT, 1);
    snx_st_le32(g + GXP_PRIMARY, 112); /* instruction at 176 */
    snx_st_le32(g + GXP_DEPENDENT_SAMPLER_COUNT, 1);
    snx_st_le32(g + GXP_DEPENDENT_SAMPLERS, 36); /* dependent sampler at 168 */
    snx_st_le32(g + GXP_CONTAINER_COUNT, 1);
    snx_st_le32(g + GXP_CONTAINERS, 12); /* container at 160 */
    snx_st_le16(g + 160, 19);
    snx_st_le16(g + 164, 1);
    snx_st_le16(g + 168, 0);
    snx_st_le16(g + 170, 1);
    snx_st_le64(g + 176, 0xe0000800c0000080ull); /* SMP, dim=3, sampler read from SA register 2 */
    g[184 + 4] = GXP_CATEGORY_SAMPLER;
    g[184 + 7] = 0x10; /* cube sampler */
    snx_st_le32(g + 184 + 8, 1);
    return at;
}
/* a texture's base format, SceGxmTextureFormat's bits 24 to 28, in its second word */
static void texture_format(GxmContext* c, unsigned i, uint32_t base) { c->textures[i].w[1] = base << 24; }
/* the sampling program's keys: texture 0 of two formats, two; then texture 1, which it does not sample, of another,
   and texture 0 back to the first */
static void check_formats(VitaProcess* p, GxmContext* c, uint32_t code) {
    VitaGxm* g = p->gxm;
    vita_render_shader_free(p, g->fragment[0].shader);
    g->fragment[0] = (GxmFragmentProgram){.references = 1, .program = sampling(p, code + 0x300), .blend_info = 0xf};
    uint64_t key[4];
    texture_format(c, 0, 0x00);
    key[0] = vita_render_pipeline_key(p, c, 0);
    texture_format(c, 0, 0x0C);
    key[1] = vita_render_pipeline_key(p, c, 0);
    texture_format(c, 1, 0x0C);
    key[2] = vita_render_pipeline_key(p, c, 0);
    texture_format(c, 0, 0x00);
    /* a byte of the program changed after its first draw: its shaders stay the ones its first draw made, read once */
    ((unsigned char*)vita_ptr(p, code + 0x300))[212] ^= 0x5A;
    key[3] = vita_render_pipeline_key(p, c, 0);
    printf("formats: keys %016llx, %016llx, %016llx with texture 1's another, %016llx back\n",
           (unsigned long long)key[0], (unsigned long long)key[1], (unsigned long long)key[2],
           (unsigned long long)key[3]);
    CHECK(key[0] && key[1] && key[0] != key[1], "texture 0 of another format takes the first format's shader");
    CHECK(key[2] == key[1], "a texture the program does not sample changed its shader");
    CHECK(key[3] == key[0],
          "texture 0's first format back took another shader, or the program's bytes were read again");
}

/* a scene on a 16 x 16 surface, the sampling program over the first vertex program, texture 0 of the first format */
static uint32_t scene_begin(VitaProcess* p) {
    uint32_t surface = vita_alloc(p, &p->cdram, 0x40000, 0x40000), code = vita_alloc(p, &p->user, 0x1000, 0x1000);
    vita_render_map(p, surface, 0x40000);
    GxmContext* c = &p->gxm->context;
    p->gxm->targets[0] = (GxmTarget){.width = 16, .height = 16};
    c->has_color = true, c->target = 0;
    c->color = (GxmColorSurface){.width = 16, .height = 16, .stride = 16, .data = surface, .type = 0};
    vita_render_begin_scene(p, c);
    return code;
}
static void programs(VitaProcess* p, uint32_t code) {
    GxmContext* c = &p->gxm->context;
    p->gxm->vertex[0] = (GxmVertexProgram){.references = 1, .program = program(p, code, false, 1)};
    p->gxm->fragment[0] =
        (GxmFragmentProgram){.references = 1, .program = sampling(p, code + 0x300), .blend_info = 0xf};
    c->vertex_program = GXM_HANDLE_VERTEX;
    c->fragment_program = GXM_HANDLE_FRAGMENT;
    texture_format(c, 0, 0x00);
}
/* the records of the prepared file at path: how many, and how many packed */
static void records_of(const char* path, unsigned* records, unsigned* packed) {
    size_t size = 0;
    unsigned char* d = snx_read_file(path, &size);
    uint32_t type, len, key = 0;
    *records = *packed = 0;
    if (d && size >= 16) memcpy(&key, d + 12, 4);
    for (size_t at = (16 + (size_t)key + 7) & ~(size_t)7; d && snx_prep_record(d, at, size, &type, &len);
         at += SNX_PREP_REC + len) {
        ++*records;
        *packed += (type & SNX_PREP_PACKED) != 0;
    }
    free(d);
}
/* the third record's check changed, the first two kept */
static void change_third(const char* path) {
    size_t size = 0;
    unsigned char* d = snx_read_file(path, &size);
    uint32_t key = 0, len;
    if (d && size >= 16) memcpy(&key, d + 12, 4);
    size_t at = (16 + (size_t)key + 7) & ~(size_t)7;
    for (int i = 0; d && i < 2 && at + 8 <= size; ++i) {
        memcpy(&len, d + at + 4, 4);
        at += SNX_PREP_REC + len;
    }
    if (d && at + 8 < size) d[at + 8] ^= 0x5A;
    if (d) CHECK(!snx_write_file(path, d, size), "cannot write %s", path);
    free(d);
}
/* two runs on a folder of their own each round: the first's draw kept, the second's made from it */
static void check_prepared(void) {
    char* base = snx_temp_dir(NULL);
    char* folder = base ? snx_path_join(base, "prepared") : NULL;
    free(base);
    if (!folder) {
        CHECK(false, "no temporary folder");
        return;
    }
    VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
    vkGetPhysicalDeviceProperties2(vita_vk()->phys, &props);
    char tag[SNX_VK_DRIVER_TAG], dir[1100], path[1300];
    snx_vk_driver_tag(tag, sizeof tag, (uint32_t)driver.driverID, props.properties.vendorID, props.properties.deviceID,
                      NULL);
    snprintf(dir, sizeof dir, "%s/shader_cache", folder);
    snx_vk_driver_path(path, sizeof path, dir, "vk_prepared", tag, ".bin");
    for (int round = 0; round < 3; ++round) {
        /* 0: made before the first draw, unpacked; 1: on the workers, packed; 2: the same file with a byte changed */
        const int mode = round == 1 ? 1 : 2;
        (void)snx_remove_tree(folder);
        (void)plat_mkdir(folder);
        g_settings.prepare_shaders = 2;
        g_settings.compress_prepared_shaders = round == 1;
        VitaProcess* p = process_make(folder);
        if (!p) {
            CHECK(false, "no process");
            break;
        }
        programs(p, scene_begin(p));
        bool a = vita_render_pipeline_made(p, &p->gxm->context, 0);
        VitaPrepared one = vita_render_prepared(p);
        vita_render_wait(p, vita_render_end_scene(p, &p->gxm->context));
        process_free(p);
        unsigned records, packed;
        records_of(path, &records, &packed);
        if (round == 2) change_third(path);
        g_settings.prepare_shaders = mode;
        p = process_make(folder);
        if (!p) {
            CHECK(false, "no second process");
            break;
        }
        VitaPrepared two = vita_render_prepared(p);
        if (mode == 1) vita_render_pipelines_wait(p); /* the workers' pipelines, collected as the scene begins */
        programs(p, scene_begin(p));
        bool b = vita_render_pipeline_made(p, &p->gxm->context, 0);
        VitaPrepared after = vita_render_prepared(p);
        vita_render_wait(p, vita_render_end_scene(p, &p->gxm->context));
        process_free(p);
        printf("prepared: prepare_shaders=%d, compress %d%s: %u records (%u packed), then %u shaders and %u pipelines "
               "read, %u made, %u drawn\n",
               mode, round == 1, round == 2 ? ", a record changed" : "", records, packed, two.shaders, two.pipelines,
               after.made, after.drawn);
        CHECK(a && b && !one.shaders && !one.pipelines && one.drawn == 1 && records == 3 &&
                  (round == 1 ? packed >= 1 : !packed),
              "the first run made %u pipelines and kept %u records, %u packed", one.drawn, records, packed);
        if (round < 2)
            CHECK(two.shaders == 2 && two.pipelines == 1 && (mode == 2 ? two.made == 1 : !two.made) &&
                      after.made == 1 && !after.drawn,
                  "the second run read %u shaders and %u pipelines, made %u, and its draw made %u", two.shaders,
                  two.pipelines, after.made, after.drawn);
        else /* the shaders kept, the pipeline record cut: the draw makes its own */
            CHECK(two.shaders == 2 && !two.pipelines && after.drawn == 1,
                  "with a record changed the second run read %u shaders and %u pipelines, and its draw made %u",
                  two.shaders, two.pipelines, after.drawn);
    }
    g_settings.prepare_shaders = 0;
    (void)snx_remove_tree(folder);
    free(folder);
}

int main(void) {
    char why[512];
    if (!vita_vk_init(NULL, why, sizeof why)) {
        printf("SKIP no Vulkan device (%s)\n", why);
        return 77;
    }
    g_settings.prepare_shaders = 0; /* the first checks' process keeps no file: it has no folder */
    VitaProcess* p = process_make(NULL);
    if (!p) {
        fprintf(stderr, "FAIL no process\n");
        return 1;
    }
    uint32_t surface = vita_alloc(p, &p->cdram, 0x40000, 0x40000), code = vita_alloc(p, &p->user, 0x1000, 0x1000);
    vita_render_map(p, surface, 0x40000);
    GxmContext* c = &p->gxm->context;
    p->gxm->targets[0] = (GxmTarget){.width = 16, .height = 16};
    c->has_color = true, c->target = 0;
    c->color = (GxmColorSurface){.width = 16, .height = 16, .stride = 16, .data = surface, .type = 0};
    vita_render_begin_scene(p, c);
    check_released(p, c, code);
    check_formats(p, c, code);
    vita_render_wait(p, vita_render_end_scene(p, c));
    process_free(p);
    check_prepared();
    vita_vk_free();
    printf("%s\n", g_failures ? "FAIL" : "ok");
    return g_failures ? 1 : 0;
}
