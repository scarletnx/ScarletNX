/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The pipeline a Vita draw takes (gpu/render.c, vita_render_pipeline_key), on a Vulkan device, in a scene with no
   draw recorded. A program released and another made in its place, its shader made where the first one's was freed,
   takes a pipeline of its own, not the first one's: the key names a shader by its program, not by its address. A
   fragment program that samples a texture takes a shader for each format of it, found again for a format it had,
   and one whose texture it does not sample changes nothing. The GXP programs are the translator's smallest: a
   header, and one with a cube sampler (vita_usse_cube.c's). Skips (77) without a Vulkan device */
#include "vita/gpu/gxm.h"
#include "vita/gpu/gxp.h"
#include "vita/gpu/render.h"
#include "vita/gpu/vk.h"
#include "vita/runtime/process.h"
#include "vita/runtime/watch.h"
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
static VitaProcess* process_make(void) {
    VitaProcess* p = calloc(1, sizeof *p);
    if (!p) return NULL;
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
    key[3] = vita_render_pipeline_key(p, c, 0);
    printf("formats: keys %016llx, %016llx, %016llx with texture 1's another, %016llx back\n",
           (unsigned long long)key[0], (unsigned long long)key[1], (unsigned long long)key[2],
           (unsigned long long)key[3]);
    CHECK(key[0] && key[1] && key[0] != key[1], "texture 0 of another format takes the first format's shader");
    CHECK(key[2] == key[1], "a texture the program does not sample changed its shader");
    CHECK(key[3] == key[0], "texture 0's first format back took another shader");
}

int main(void) {
    char why[512];
    if (!vita_vk_init(NULL, why, sizeof why)) {
        printf("SKIP no Vulkan device (%s)\n", why);
        return 77;
    }
    VitaProcess* p = process_make();
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
    vita_vk_free();
    printf("%s\n", g_failures ? "FAIL" : "ok");
    return g_failures ? 1 : 0;
}
