/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sceGxm's scenes on the host's Vulkan device (vk.h). The memory the game maps for the GPU (sceGxmMapMemory) is
   imported as buffers: a scene's color surface is an image loaded from its memory when the scene begins and stored
   back when it ends, vertices and indices are read where the game wrote them, textures are copied from their memory
   once a scene, all on the GPU. Where the device can't import it, each buffer is a copy of a memory block: the pages
   the game wrote are copied in before the GPU reads them, what the GPU wrote goes back once its submission is done.
   Each program becomes a SPIR-V shader (usse.h) at its first draw. The uniform buffers
   a draw reads are copied into a staging ring of the submission's, as the console's reserved ring space is the
   game's to reuse once the draw is made. A scene is submitted when it ends, and the next is recorded while the GPU
   draws it: the game waits for a scene's notifications (hle/gxm.c writes them once its submission is done) before it
   writes what the scene reads, as on the console */
#include "vita/gpu/render.h"
#include "vita/gpu/gxp.h"
#include "vita/gpu/usse.h"
#include "vita/gpu/vk.h"
#include "common/gpu/vk/jobs.h"
#include "common/gpu/vk/prepared.h"
#include "common/gpu/vk/vk_common.h"
#include "common/platform/time.h"
#include "common/utilities/build_info.h"
#include "vita/runtime/watch.h"
/* compiled from swizzle.comp, picture.comp, yuv.comp and rgb24.comp by the build (cmake/glsl.cmake) */
#include "vita/gpu/picture_spv.h"
#include "vita/gpu/rgb24_spv.h"
#include "vita/gpu/swizzle_spv.h"
#include "vita/gpu/yuv_spv.h"
#include "common/utilities/endian.h"
#include "common/utilities/bits.h"
#include "snx_settings.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMPORTS   (GXM_MAPPINGS + 16u)
#define SURFACES  16u
#define TEXTURES  256u
#define PIPELINES 256u
#define STAGING   (4u << 20)
#define LINEAR    (16u << 20) /* a slot's room for textures' rows made linear on the GPU */
#define MIRROR_STEP 4096u       /* a copy is filled this many bytes at a time, a bit for each the GPU writes */

typedef struct {
    uint32_t address, size;
    VkBuffer buffer;
    VkDeviceMemory memory;
    /* where the device cannot import host memory the buffer is a host visible copy of the range instead. mirror is
       its mapping (NULL when the range is imported in place), [begin, end) what of the range it holds so far, since and
       epoch the watch generation and epoch it matches */
    unsigned char* mirror;
    uint32_t begin, end;
    uint64_t since, epoch;
} Import;
/* rows bytes of a copy the GPU writes, stride apart, stored to guest memory once the submission holding the write is
   done: the bytes between the rows are the game's, which may write them meanwhile */
typedef struct {
    uint32_t import; /* imports' */
    uint32_t address, row, stride, rows;
    uint64_t serial;
} Writeback;
typedef struct {
    uint32_t address, width, height, stride, format;
    uint32_t type; /* a color surface's SceGxmColorSurfaceType (GXM_COLOR_SURFACE_*) */
    bool depth;
    VkFormat vk_format; /* its image's: a color surface's color_format, a depth surface's D16, D32 or one with stencil */
    bool stencil;       /* a depth surface's image has stencil (an S8D24 surface's, as vk.h's stencil_format) */
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
    VkImageLayout layout;
    /* past a scale of 1 (the resolution setting), a color surface's image a scene draws to, that many times larger:
       image above keeps its memory's size, the larger one made from it (each pixel a square) when a scene loads the
       memory, and it made from the larger one (filtered) when the scene stores it. A depth surface's own image is the
       larger one, its memory going through samples */
    VkImage big;
    VkImageView big_view;
    VkDeviceMemory big_memory;
    VkImageLayout big_layout;
    bool filtered; /* the format filters: the larger image's store averages its pixels */
    /* a color surface's image holds its memory as of generation since (vita/runtime/watch.h): its last store, which
       a scene that begins on it needn't load again while the memory wasn't written after */
    bool synced;
    uint64_t since;
    /* a multisampled D16 depth surface's samples, as its memory holds them (depth_memory), made at its first load or
       store */
    VkImage samples;
    VkDeviceMemory samples_memory;
} Surface;
typedef struct {
    uint32_t address, format, width, height, stride, type, levels;
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
    VkImageLayout layout;
    /* the image holds its memory as of generation since (vita/runtime/watch.h): copied again once the memory was
       written after; the scene it was drawn with last, the oldest going when the cache is full */
    bool synced;
    uint64_t since, used;
    /* the watch epoch (vita_watch_epoch) plus one at which its memory was last found unchanged: a draw at the same
       epoch needn't look at the pages again. 0: not found so */
    uint64_t clean;
    /* the scene and draw count at which it was last copied from the color surface being drawn (texture_for), so a
       draw that samples it again with nothing drawn between copies it no more */
    uint64_t snapshot_scene, snapshot_draws;
    /* its image's size over its own: the scale of the surface drawn larger it was copied from, else 1 */
    uint32_t scale;
} Texture;
/* swizzle.comp's, yuv.comp's, picture.comp's and rgb24.comp's push constants, member for member */
typedef struct {
    uint32_t words, elements, src_base, src_at, es, width, wl, k, tiled, tiles_x, reverse, dst_base;
} SwizzleParams;
typedef struct {
    uint32_t pixels, width, height, y_at, c_at, stride, packed, first_v, rv, gu, gv, bu;
} YuvParams;
typedef struct {
    uint32_t words, luma, pitch, width, height, u_at, v_at, packed, vu;
} PictureParams;
typedef struct {
    uint32_t words, src_at, dst_at, width, height, stride, pack, bgr;
} Rgb24Params;
/* a program's shader for what the translator was told of it; the program's others after it, one per set of the formats
   of the textures it samples */
typedef struct Shader {
    VkShaderModule module;
    UsseShader meta; /* without its words */
    bool failed;
    uint64_t code; /* its program's bytes, read at the program's first draw */
    uint64_t id;   /* those and the hints it was made for, which a pipeline's key names it by */
    uint32_t prep; /* its record's number in the prepared file plus 1, 0 for none */
    struct Shader* next;
} Shader;
typedef struct {
    uint64_t key;
    VkPipeline pipeline;
    uint8_t job;    /* while a worker makes it (async_pipelines), its job's index plus 1 */
    uint8_t better; /* while a worker makes the optimized one of a pipeline linked fast, its job's index plus 1 */
} Pipeline;
/* what goes once the work recorded before it is done: a pipeline, or an image with its view and memory */
typedef struct {
    VkPipeline pipeline;
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
    uint64_t serial;
} Buried;
/* what a pipeline is made of, all of it its own, so that a worker can make it while the draws go on */
typedef struct {
    VkVertexInputBindingDescription bindings[GXM_STREAMS];
    VkVertexInputAttributeDescription attributes[GXM_ATTRIBUTES];
    unsigned binding_count, attribute_count;
    VkShaderModule vs, fs;
    VkPrimitiveTopology topology;
    VkPipelineRasterizationStateCreateInfo raster;
    VkPipelineDepthStencilStateCreateInfo depth;
    VkPipelineColorBlendAttachmentState blend;
    VkPipelineColorBlendStateCreateFlags blend_flags;
    VkFormat color, depth_format, stencil_format;
    VkPipelineCache cache;
    VkPipeline made;           /* a job's, once made */
    uint32_t vs_prep, fs_prep; /* its shaders' records in the prepared file plus 1 (Shader.prep) */
    VkPipeline linked[SNX_VK_GPL_PARTS]; /* a fast link's parts, for its optimized one: the job links those alone */
} PipeDesc;
/* a pipeline of the prepared file (prepare_shaders): its key, its shaders by their records' numbers, and what it is
   made of, its handles none in the file */
typedef struct {
    uint64_t key;
    uint32_t vs, fs;
    PipeDesc d;
} PrepPipe;
/* what the graphics bind point of the open recording holds, so a draw that changes none of it records nothing for it.
   Forgotten (zeroed) when a scene begins and when its rendering resumes: a submit may have put a new command buffer
   under it, which holds none of this */
typedef struct {
    bool dynamic; /* viewport to push hold the last draw's */
    VkViewport viewport;
    VkRect2D scissor;
    uint32_t stencil[3]; /* reference, compare mask, write mask */
    UssePush push;
    VkPipeline pipeline;
    VkDescriptorSet set; /* and what it was written with: */
    VkDescriptorImageInfo images[USSE_TEXTURES];
    VkDeviceSize ranges[2]; /* its two uniform buffers' sizes */
    VkImageView input;      /* its color input attachment */
    VkBuffer vertex[GXM_STREAMS];
    VkDeviceSize vertex_at[GXM_STREAMS];
    VkBuffer index;
    VkDeviceSize index_at;
    VkIndexType index_type;
} Bound;
struct VitaRender {
    VitaVk* vk;
    VkPipelineCache cache; /* the driver's pipelines kept between runs (vk_pipeline_cache), VK_NULL_HANDLE for none */
    char cache_path[1200];
    unsigned char* mem; /* the guest's memory */
    Import imports[IMPORTS];
    Writeback* writeback; /* the GPU's writes to copied guest memory waiting for their submission */
    unsigned writebacks, writeback_room;
    Surface surfaces[SURFACES];
    Texture textures[TEXTURES];
    Pipeline pipelines[PIPELINES];
    /* async_pipelines: the workers that make the new pipelines while the draws that need them are skipped
       (common/gpu/vk/jobs.h), each job's pipeline, and the draws skipped so far */
    SnxVkJobs pool;
    PipeDesc jobs[SNX_VK_JOBS];
    uint64_t skipped;
    /* prepare_shaders: the file of the shaders and pipelines made (common/gpu/vk/prepared.h), its shader records, by
       whose numbers a pipeline's names its shaders; the shaders read from it (prepared_count of them, by record), each
       until a program takes it; the pipelines of earlier runs to make, on the workers (common/gpu/vk/jobs.h's feed);
       and what the run says of them */
    SnxPrepFile prep;
    uint32_t prep_shaders, prepared_count;
    Shader** prepared;
    size_t prepared_room;
    PrepPipe* prep_pipes;
    size_t prep_pipe_room;
    SnxVkFeed feed;
    VitaPrepared counts;
    /* pipeline_libraries: the parts made so far, the pipelines linked fast and optimized (the workers' too) and the
       fast links replaced, and the fast links an optimized one replaced, each kept until the work recorded before
       it is done (the serial of the recording open then) */
    SnxVkGplParts parts;
    _Atomic unsigned gpl_linked, gpl_optimized;
    unsigned gpl_replaced;
    _Atomic int gpl_failed;
    Buried buried[64];
    unsigned buried_count;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkSampler samplers[2][2][4]; /* min/mag linear, mip, address mode */
    struct {
        VkBuffer buffer;
        VkDeviceMemory memory;
        unsigned char* map;
        VkDeviceSize used;
    } staging[VITA_VK_SLOTS];
    /* the compute shaders, a buffer read and a buffer written each (swizzle.comp: textures' rows made linear,
       picture.comp: decoded pictures laid out, yuv.comp: YUV textures made RGBA, rgb24.comp: 24-bit color surfaces
       to and from RGBA rows), and the device memory their rows go to, a ring a slot */
    VkDescriptorSetLayout swizzle_set;
    VkPipelineLayout swizzle_layout;
    VkPipeline swizzle, picture, yuv, rgb24;
    struct {
        VkBuffer buffer;
        VkDeviceMemory memory;
        VkDeviceSize used;
    } linear[VITA_VK_SLOTS];
    /* the scale the scenes are drawn at, the resolution setting's whole step from 1 to 8: a color surface's drawn image,
       its viewports and scissors that much larger, its memory at the console's size */
    uint32_t scale;
    /* the scene being recorded, and the scale it is drawn at: scale, or 1 for a surface the device can't scale */
    bool rendering;
    Surface *color, *depth;
    uint32_t width, height, drawn;
    uint64_t scene;
    uint64_t draws; /* the draws recorded so far */
    unsigned logged;           /* each reason reported at least once: a bit each */
    uint64_t logged_value[32]; /* the value once_value last logged for each bit, so a different one still prints */
    /* the framebuffer the window shows (vita_render_show): its address and generation, and the vblanks since */
    uint32_t shown;
    uint64_t shown_since;
    unsigned shown_skips;
    Bound bound;
};

static struct VitaRender* render_of(VitaProcess* p) { return p->gxm ? p->gxm->render : NULL; }
/* the color target's layout while it is rendered to: one its input attachment reads take too when the device reads it
   in place (VK_KHR_dynamic_rendering_local_read) */
static VkImageLayout color_layout(const struct VitaRender* r) {
    return r->vk->local_read ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}
/* a reason logged once for each distinct value: the bit's first call, and the first call after a different value,
   print; a repeat of the same value returns before touching the format string, as the plain bit did before. A
   caller with nothing that varies (a message with no %, truly one reason) passes 0 for value */
[[gnu::format(printf, 4, 5)]] static void once_value(struct VitaRender* r, unsigned bit, uint64_t value,
                                                     const char* format, ...);
static void once_value(struct VitaRender* r, unsigned bit, uint64_t value, const char* format, ...) {
    if ((r->logged & (1u << bit)) && r->logged_value[bit] == value) return;
    r->logged |= 1u << bit;
    r->logged_value[bit] = value;
    va_list list;
    va_start(list, format);
    printf("gxm: ");
    vprintf(format, list);
    putchar('\n');
    va_end(list);
}
/* once_value for a message with nothing that varies: the same reason every time it is reached */
[[gnu::format(printf, 3, 4)]] static void once(struct VitaRender* r, unsigned bit, const char* format, ...);
static void once(struct VitaRender* r, unsigned bit, const char* format, ...) {
    if (r->logged & (1u << bit)) return;
    r->logged |= 1u << bit;
    va_list list;
    va_start(list, format);
    printf("gxm: ");
    vprintf(format, list);
    putchar('\n');
    va_end(list);
}
static void retire(unsigned slot, uint64_t serial);
static void submitting(void);
static void rendering_pause(VkCommandBuffer cb);
static void rendering_resume(struct VitaRender* r, VkCommandBuffer cb);
static bool color_copy(struct VitaRender* r, VkCommandBuffer cb, const Surface* s, const Import* m, VkDeviceSize at,
                       bool store);
static void color_written(struct VitaRender* r, const Import* m, const Surface* s);
static struct VitaRender* g_retiring; /* the renderer whose staging the device's retire frees */

/* ---- guest memory as buffers ---- */
static uint32_t import_index(const struct VitaRender* r, const Import* m) { return (uint32_t)(m - r->imports); }
/* the bytes from a write's first to its last, the ones between its rows with them */
static uint64_t writeback_span(const Writeback* w) { return (uint64_t)(w->rows - 1) * w->stride + w->row; }
/* the bits [from, to) set */
static void bits_set(uint64_t* bits, uint32_t from, uint32_t to) {
    for (uint32_t i = from / 64; i * 64 < to; ++i) {
        uint32_t first = from > i * 64 ? from - i * 64 : 0, end = to - i * 64;
        bits[i] |= (end < 64 ? (1ull << end) - 1 : ~0ull) & ~0ull << first;
    }
}
/* the bytes of [at, at + size) of a copy, size at most MIRROR_STEP, that the GPU writes and guest memory does not hold
   yet: a bit each set in bits, which start clear. False when there are none */
static bool mirror_newer(const struct VitaRender* r, const Import* m, uint64_t at, uint32_t size,
                         uint64_t bits[MIRROR_STEP / 64]) {
    uint32_t import = import_index(r, m);
    uint64_t end = at + size;
    bool any = false;
    for (unsigned i = 0; i < r->writebacks; ++i) {
        const Writeback* w = &r->writeback[i];
        if (w->import != import || w->address >= end || w->address + writeback_span(w) <= at) continue;
        any = true;
        for (uint64_t y = at > w->address ? (at - w->address) / w->stride : 0; y < w->rows; ++y) {
            uint64_t from = w->address + y * w->stride, to = from + w->row;
            if (from >= end) break;
            if (to > at) bits_set(bits, (uint32_t)(from > at ? from - at : 0), (uint32_t)(to < end ? to - at : size));
        }
    }
    return any;
}
/* [begin, end) of guest memory into a copy, but the bytes the GPU writes that are newer there */
static void mirror_fill(const struct VitaRender* r, const Import* m, uint64_t begin, uint64_t end) {
    unsigned char* to = m->mirror + (begin - m->address);
    const unsigned char* from = r->mem + begin;
    for (uint64_t at = begin; at < end;) {
        uint64_t newer[MIRROR_STEP / 64] = {0};
        uint32_t size = end - at < MIRROR_STEP ? (uint32_t)(end - at) : MIRROR_STEP;
        bool some = r->writebacks && mirror_newer(r, m, at, size, newer);
        for (uint32_t i = 0; i < size;) {
            uint32_t start = i;
            if (!some) i = size;
            while (i < size && !(newer[i / 64] >> (i % 64) & 1)) ++i;
            if (i > start) memcpy(to + (at - begin) + start, from + (at - begin) + start, i - start);
            while (i < size && newer[i / 64] >> (i % 64) & 1) i += newer[i / 64] == ~0ull && !(i % 64) ? 64 : 1;
        }
        at += size;
    }
}
/* the pages of [begin, end) the CPU wrote since the copy was brought up to date, copied. False when one of them can be
   written again with no generation counted: it was while this read it, or it can't be protected */
static bool mirror_update(const struct VitaRender* r, const Import* m, uint64_t begin, uint64_t end) {
    uint64_t page = vita_watch_page();
    bool settled = true;
    for (uint64_t at = begin, next; at < end; at = next) {
        uint64_t stop = at; /* a run of pages written */
        for (next = at; next < end && stop == next;) {
            next = (next / page + 1) * page < end ? (next / page + 1) * page : end;
            if (vita_watch_changed((uint32_t)stop, (uint32_t)(next - stop), m->since)) stop = next;
        }
        if (stop == at) continue;
        /* protected before it is read, so a write after that faults and counts */
        uint64_t since = vita_watch_protect((uint32_t)at, (uint32_t)(stop - at));
        mirror_fill(r, m, at, stop);
        if (vita_watch_changed((uint32_t)at, (uint32_t)(stop - at), since)) settled = false;
    }
    return settled;
}
/* The copy brought up to date with the pages the CPU wrote since it was: those each generation since wrote, or every
   page when the copy is not known to be up to date at any, or one of them is not known any more */
static void mirror_sync(const struct VitaRender* r, Import* m) {
    uint64_t now = vita_watch_epoch(), known = m->epoch;
    if (!m->mirror || now == m->epoch) return;
    bool settled = true;
    while (known < now) {
        uint64_t begin, end;
        if (!vita_watch_wrote(known + 1, &begin, &end)) break;
        ++known;
        begin = begin > m->begin ? begin : m->begin, end = end < m->end ? end : m->end;
        if (begin < end) settled = mirror_update(r, m, begin, end) && settled;
    }
    if (known != now) settled = mirror_update(r, m, m->begin, m->end);
    m->since = now;
    m->epoch = settled ? now : UINT64_MAX;
}
/* The copy made to hold [address, address + size) too, and what lies between it and what it held. What is new to it is
   copied whole: a page protected before, and not written since the watch began, looks unchanged */
static void mirror_cover(const struct VitaRender* r, Import* m, uint32_t address, uint32_t size) {
    uint32_t end = address + size, begin = m->begin, held = m->end;
    if (!m->mirror || (address >= begin && end <= held)) return;
    if (begin == held) begin = held = address; /* it held nothing */
    uint32_t parts[2][2] = {{address < begin ? address : begin, begin}, {held, end > held ? end : held}};
    for (unsigned i = 0; i < 2; ++i) {
        if (parts[i][0] == parts[i][1]) continue;
        uint64_t since = vita_watch_protect(parts[i][0], parts[i][1] - parts[i][0]);
        mirror_fill(r, m, parts[i][0], parts[i][1]);
        if (m->begin == m->end) m->since = since;
    }
    m->begin = parts[0][0], m->end = parts[1][1];
    m->epoch = UINT64_MAX;
}
/* the GPU writes rows of row bytes of the copy, stride apart, from address on. Its submission is the open recording's,
   which is the next one */
static void mirror_written(struct VitaRender* r, const Import* m, uint32_t address, uint32_t row, uint32_t stride,
                           uint32_t rows) {
    if (!m || !m->mirror || !row || !rows) return;
    if ((uint64_t)(address - m->address) + (uint64_t)(rows - 1) * stride + row > m->size) {
        once_value(r, 24, address,
                   "the GPU's write at 0x%08" PRIx32 " ends past its memory: it is not stored to guest memory",
                   address);
        return;
    }
    if (rows == 1 || stride == row) row *= rows, stride = row, rows = 1;
    uint32_t import = import_index(r, m);
    uint64_t serial = r->vk->ring.submitted + 1;
    for (unsigned i = 0; i < r->writebacks; ++i) {
        Writeback* w = &r->writeback[i];
        if (w->import != import || w->serial != serial) continue;
        if (w->address == address && w->row == row && w->stride == stride && w->rows == rows) return;
        uint64_t end = (uint64_t)address + row, w_end = (uint64_t)w->address + w->row;
        if (rows != 1 || w->rows != 1 || address > w_end || w->address > end) continue;
        w->address = address < w->address ? address : w->address; /* one range with the one it touches */
        w->row = w->stride = (uint32_t)((end > w_end ? end : w_end) - w->address);
        return;
    }
    if (r->writebacks == r->writeback_room) {
        unsigned room = r->writeback_room ? r->writeback_room * 2 : 64;
        Writeback* grown = realloc(r->writeback, room * sizeof *grown);
        if (!grown) {
            once_value(r, 23, address,
                       "out of memory: the GPU's write at 0x%08" PRIx32 " is not stored to guest memory", address);
            return;
        }
        r->writeback = grown, r->writeback_room = room;
    }
    r->writeback[r->writebacks++] = (Writeback){import, address, row, stride, rows, serial};
}
/* A write of the GPU's told to the watch. The copies that were up to date stay so: no other memory changed, and the copy
   the write is in has it */
static void gpu_wrote(struct VitaRender* r, uint32_t address, uint32_t size) {
    uint64_t before = vita_watch_epoch();
    vita_watch_written(address, size);
    uint64_t now = vita_watch_epoch();
    if (r->vk->host_import || now != before + 1) return; /* no copies, or a write of the game's came between */
    for (unsigned i = 0; i < IMPORTS; ++i)
        if (r->imports[i].mirror && r->imports[i].epoch == before) r->imports[i].since = r->imports[i].epoch = now;
}
/* the writes of submissions up to done (of one import's copy, or of all for IMPORTS) stored to guest memory */
static void mirror_drain(struct VitaRender* r, uint64_t done, uint32_t only) {
    unsigned kept = 0;
    for (unsigned i = 0; i < r->writebacks; ++i) {
        Writeback w = r->writeback[i];
        if (w.serial > done || (only != IMPORTS && w.import != only)) {
            r->writeback[kept++] = w;
            continue;
        }
        const Import* m = &r->imports[w.import];
        const unsigned char* from = m->mirror + (w.address - m->address);
        if (vita_watch_store(w.address, from, w.row, w.stride, w.rows)) continue;
        for (size_t y = 0; y < w.rows; ++y) memcpy(r->mem + w.address + y * w.stride, from + y * w.stride, w.row);
    }
    r->writebacks = kept;
}
static bool mirror_range(struct VitaRender* r, uint32_t address, uint32_t size, Import* out) {
    VitaVk* v = r->vk;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = size,
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    Import m = {.address = address, .size = size, .epoch = UINT64_MAX};
    if (vkCreateBuffer(v->device, &bi, NULL, &m.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(v->device, m.buffer, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size};
    ai.memoryTypeIndex = vita_vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                                     VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) {
        once(r, 25, "the device has no cached memory the CPU shares with it: what the GPU writes is read back slowly");
        ai.memoryTypeIndex = vita_vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    void* map = NULL;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(v->device, &ai, NULL, &m.memory) != VK_SUCCESS) {
        vkDestroyBuffer(v->device, m.buffer, NULL);
        return false;
    }
    if (vkBindBufferMemory(v->device, m.buffer, m.memory, 0) != VK_SUCCESS ||
        vkMapMemory(v->device, m.memory, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) {
        vkDestroyBuffer(v->device, m.buffer, NULL);
        vkFreeMemory(v->device, m.memory, NULL);
        return false;
    }
    m.mirror = map;
    *out = m;
    return true;
}
static bool import_range(struct VitaRender* r, VitaProcess* p, uint32_t address, uint32_t size, Import* out) {
    VitaVk* v = r->vk;
    if (!v->host_import) return mirror_range(r, address, size, out);
    VkExternalMemoryBufferCreateInfo external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                                 .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .pNext = &external,
                             .size = size,
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    vita_watch_release(address, size); /* the driver takes the pages as they are, writable */
    void* host = vita_ptr(p, address);
    VkMemoryHostPointerPropertiesEXT pointer = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (((uintptr_t)host | size) & (v->host_alignment - 1)) return false;
    if (vkGetMemoryHostPointerPropertiesEXT(v->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host,
                                            &pointer) != VK_SUCCESS)
        return false;
    if (vkCreateBuffer(v->device, &bi, NULL, &out->buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(v->device, out->buffer, &req);
    VkImportMemoryHostPointerInfoEXT info = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                             .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                             .pHostPointer = host};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .pNext = &info,
                               .allocationSize = size,
                               .memoryTypeIndex = vita_vk_memory_type(req.memoryTypeBits & pointer.memoryTypeBits,
                                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(v->device, &ai, NULL, &out->memory) != VK_SUCCESS ||
        vkBindBufferMemory(v->device, out->buffer, out->memory, 0) != VK_SUCCESS) {
        vkDestroyBuffer(v->device, out->buffer, NULL);
        if (out->memory) vkFreeMemory(v->device, out->memory, NULL);
        *out = (Import){0};
        return false;
    }
    out->address = address, out->size = size;
    return true;
}
static bool holds(uint32_t base, uint32_t length, uint32_t address, uint32_t size) {
    return address >= base && (uint64_t)address + size <= (uint64_t)base + length;
}
/* the import that holds [address, address + size), NULL for none */
static Import* import_of(struct VitaRender* r, uint32_t address, uint32_t size) {
    for (unsigned i = 0; i < IMPORTS; ++i) {
        Import* m = &r->imports[i];
        if (m->size && holds(m->address, m->size, address, size)) return m;
    }
    return NULL;
}
/* what a copy is of for [address, address + size): the memory block or the module's segment that holds it, so no two
   copies hold the same memory */
static void mirror_unit(const VitaProcess* p, uint32_t* address, uint32_t* size) {
    for (unsigned i = 0; i < VITA_ALLOCATIONS; ++i) {
        const VitaAllocation* a = &p->allocations[i];
        if (!a->size || !holds(a->address, a->size, *address, *size)) continue;
        *address = a->address, *size = a->size;
        return;
    }
    for (unsigned i = 0; i < p->image.segment_count; ++i) {
        const VitaSegment* g = &p->image.segment[i];
        uint32_t length = (g->size + 0xfffu) & ~0xfffu; /* its pages, as vita_mapped takes it */
        if (!holds(g->address, length, *address, *size)) continue;
        *address = g->address, *size = length;
        return;
    }
}
/* whether a mapping other than the one at address is in the copy */
static bool mirror_mapped(const VitaProcess* p, const Import* m, uint32_t address) {
    for (unsigned i = 0; i < GXM_MAPPINGS; ++i) {
        uint32_t at = p->gxm->mappings[i].address;
        if (p->gxm->mappings[i].size && at != address && at >= m->address && at - m->address < m->size) return true;
    }
    return false;
}
void vita_render_map(VitaProcess* p, uint32_t address, uint32_t size) {
    struct VitaRender* r = render_of(p);
    if (!r) return;
    uint32_t base = address, length = size;
    if (!r->vk->host_import) mirror_unit(p, &base, &length);
    Import* m = r->vk->host_import ? NULL : import_of(r, base, length);
    for (unsigned i = 0; i < IMPORTS && !m; ++i)
        if (!r->imports[i].size) {
            m = &r->imports[i];
            if (!import_range(r, p, base, length, m)) {
                if (r->vk->host_import)
                    once_value(r, 0, address, "the device cannot import guest memory at 0x%08" PRIx32, address);
                else
                    once_value(r, 0, address, "the device has no room for a copy of guest memory at 0x%08" PRIx32,
                               address);
                return;
            }
        }
    if (m) mirror_cover(r, m, address, size);
}
static void import_free(struct VitaRender* r, Import* m) {
    mirror_drain(r, UINT64_MAX, import_index(r, m)); /* the GPU is idle here */
    vkDestroyBuffer(r->vk->device, m->buffer, NULL);
    vkFreeMemory(r->vk->device, m->memory, NULL);
    *m = (Import){0};
}
/* The scene being drawn may read the memory in the work recorded so far, not submitted until it ends: a movie player
   unmaps and frees its frames from one thread while another draws the last one. That work runs now, while the memory
   is there, and the scene goes on after it, before the buffer it reads goes */
static void settle(VitaProcess* p, struct VitaRender* r) {
    bool rendering = r->rendering;
    if (rendering) rendering_pause(vita_vk_commands());
    vita_vk_submit();
    vita_render_idle(p, true);
    if (rendering) rendering_resume(r, vita_vk_commands());
}
void vita_render_freed(VitaProcess* p, uint32_t address, uint32_t size) {
    struct VitaRender* r = render_of(p);
    Import* m = r && !r->vk->host_import ? import_of(r, address, size) : NULL;
    if (!m) return;
    settle(p, r);
    import_free(r, m);
}
void vita_render_unmap(VitaProcess* p, uint32_t address) {
    struct VitaRender* r = render_of(p);
    if (!r) return;
    settle(p, r);
    for (unsigned i = 0; i < IMPORTS; ++i) {
        Import* m = &r->imports[i];
        if (!m->size) continue;
        if (m->mirror ? address >= m->address && address - m->address < m->size && !mirror_mapped(p, m, address)
                      : m->address == address)
            import_free(r, m);
    }
}
/* the buffer holding [address, address + size) and the address's offset there; a range the game didn't map for the
   GPU (a framebuffer the CPU drew) is imported whole, as its memory block */
static const Import* buffer_of(struct VitaRender* r, VitaProcess* p, uint32_t address, uint32_t size, VkDeviceSize* at) {
    Import* m = import_of(r, address, size);
    for (unsigned i = 0; i < VITA_ALLOCATIONS && !m; ++i) {
        const VitaAllocation* a = &p->allocations[i];
        if (!a->size || !holds(a->address, a->size, address, size)) continue;
        for (unsigned j = 0; j < IMPORTS && !m; ++j)
            if (!r->imports[j].size) {
                if (!import_range(r, p, a->address, a->size, &r->imports[j])) return NULL;
                m = &r->imports[j];
            }
    }
    if (!m) return NULL;
    if (m->mirror && (address < m->begin || (uint64_t)address + size > m->end)) mirror_cover(r, m, m->address, m->size);
    mirror_sync(r, m);
    *at = address - m->address;
    return m;
}

/* ---- images ---- */
static bool image_make(struct VitaRender* r, VkFormat format, uint32_t width, uint32_t height, uint32_t levels,
                       VkImageUsageFlags usage, VkImageAspectFlags aspect, VkComponentMapping components, bool cube,
                       VkImage* image, VkImageView* view, VkDeviceMemory* memory) {
    VitaVk* v = r->vk;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = format,
                            .extent = {width, height, 1},
                            .mipLevels = levels,
                            .arrayLayers = cube ? 6 : 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(v->device, &ii, NULL, image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(v->device, *image, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex = vita_vk_memory_type(req.memoryTypeBits,
                                                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = vita_vk_memory_type(req.memoryTypeBits, 0);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
                                .format = format,
                                .components = components,
                                .subresourceRange = {aspect, 0, levels, 0, cube ? 6u : 1u}};
    if (vkAllocateMemory(v->device, &ai, NULL, memory) != VK_SUCCESS ||
        vkBindImageMemory(v->device, *image, *memory, 0) != VK_SUCCESS)
        return false;
    if (!view) return true; /* an image only copied to and from */
    vi.image = *image;
    return vkCreateImageView(v->device, &vi, NULL, view) == VK_SUCCESS;
}
static void barrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from,
                               .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = image,
                               .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}
static void memory_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}
/* SceGxmColorFormat as a color surface's image format and the bytes of its pixels: the A8B8G8R8 family (the bytes R,
   G, B, A), U8U8U8 in its BGR and RGB swizzles (three bytes, R first for BGR, in an R8G8B8A8 image: color_copy) and
   U16_R (one 16-bit unsigned normalized channel, the program's first output component, as an attachment takes it
   unswizzled). False for the others, not drawn yet */
static bool color_format(uint32_t format, VkFormat* vk, unsigned* bytes) {
    if ((format & 0xffff0000u) == 0x00000000u) {
        *vk = VK_FORMAT_R8G8B8A8_UNORM, *bytes = 4;
        return true;
    }
    if ((format & ~GXM_COLOR_SWIZZLE3_RGB) == 0x10000000u) { /* SCE_GXM_COLOR_FORMAT_U8U8U8_BGR, _RGB */
        *vk = VK_FORMAT_R8G8B8A8_UNORM, *bytes = 3;
        return true;
    }
    if ((format & 0xffff0000u) == 0x01000000u) { /* SCE_GXM_COLOR_FORMAT_F16F16F16F16_*: half floats, R, G, B, A in
                                                     memory, which the game's HDR passes draw into and sample */
        *vk = VK_FORMAT_R16G16B16A16_SFLOAT, *bytes = 8;
        return true;
    }
    if (format == 0x40800000u) { /* SCE_GXM_COLOR_FORMAT_U16_R */
        *vk = VK_FORMAT_R16_UNORM, *bytes = 2;
        return true;
    }
    return false;
}
/* a surface's images and memory freed, the slot empty again */
static void surface_drop(VkDevice d, Surface* s) {
    if (s->view) vkDestroyImageView(d, s->view, NULL);
    if (s->image) vkDestroyImage(d, s->image, NULL);
    if (s->memory) vkFreeMemory(d, s->memory, NULL);
    if (s->big_view) vkDestroyImageView(d, s->big_view, NULL);
    if (s->big) vkDestroyImage(d, s->big, NULL);
    if (s->big_memory) vkFreeMemory(d, s->big_memory, NULL);
    if (s->samples) vkDestroyImage(d, s->samples, NULL);
    if (s->samples_memory) vkFreeMemory(d, s->samples_memory, NULL);
    *s = (Surface){0};
}
/* the surface for a color surface's memory (or a depth surface), made at its first scene */
static Surface* surface(struct VitaRender* r, uint32_t address, uint32_t width, uint32_t height, uint32_t stride,
                        uint32_t format, uint32_t type, bool depth) {
    Surface* free_slot = NULL;
    for (unsigned i = 0; i < SURFACES; ++i) {
        Surface* s = &r->surfaces[i];
        if (s->image && s->address == address && s->width == width && s->height == height && s->stride == stride &&
            s->format == format && s->type == type && s->depth == depth)
            return s;
        if (!s->image && !free_slot) free_slot = s;
    }
    if (!free_slot) return NULL;
    Surface* s = free_slot;
    VkComponentMapping identity = {0};
    /* a D16 depth surface (format: its GXM_DEPTH_* format) as D16, which its memory holds, an S8D24 one with its
       stencil, else D32 */
    bool stencil = depth && format == GXM_DEPTH_S8D24 && r->vk->stencil_format;
    VkFormat vk = depth && format == GXM_DEPTH_D16 && r->vk->depth16_blit ? VK_FORMAT_D16_UNORM
                  : stencil                                               ? r->vk->stencil_format
                                                                          : VK_FORMAT_D32_SFLOAT;
    unsigned bytes;
    if (!depth && !color_format(format, &vk, &bytes)) return NULL;
    uint32_t scale = depth ? r->scale : 1;
    bool ok = depth ? image_make(r, vk, width * scale, height * scale, 1,
                                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0), identity,
                                 false, &s->image, &s->view, &s->memory)
                    : image_make(r, vk, width, height, 1,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     (r->vk->local_read ? VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT : 0),
                                 VK_IMAGE_ASPECT_COLOR_BIT, identity, false, &s->image, &s->view, &s->memory);
    if (ok && !depth && r->scale > 1) {
        /* drawn larger where the device blits the format both ways, else at 1x */
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(r->vk->phys, vk, &fp);
        VkFormatFeatureFlags both = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        if ((fp.optimalTilingFeatures & both) == both) {
            ok = image_make(r, vk, width * r->scale, height * r->scale, 1,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                (r->vk->local_read ? VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT : 0),
                            VK_IMAGE_ASPECT_COLOR_BIT, identity, false, &s->big, &s->big_view, &s->big_memory);
            s->big_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            s->filtered = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
        } else
            once_value(r, 23, (uint64_t)vk,
                       "a color surface of Vulkan format %u is drawn at 1x, not at resolution=%u: the device can't "
                       "blit it",
                       (unsigned)vk, (unsigned)r->scale);
    }
    if (!ok) {
        surface_drop(r->vk->device, s);
        return NULL;
    }
    s->address = address, s->width = width, s->height = height, s->stride = stride, s->format = format;
    s->type = type, s->depth = depth, s->vk_format = vk, s->stencil = stencil;
    s->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return s;
}
/* the view a scene draws a color surface through: its larger image's past a scale of 1 */
static VkImageView drawn_view(const Surface* s) { return s->big ? s->big_view : s->view; }
/* A color surface's larger image made from its image (up: nearest, each pixel a square of the scale's side) or its
   image from the larger one (down: filtered where the format filters, which averages a square's pixels at 2x, else
   nearest), the source in TRANSFER_SRC and the destination in TRANSFER_DST */
static void color_blit(const struct VitaRender* r, VkCommandBuffer cb, const Surface* s, bool down) {
    int32_t w = (int32_t)s->width, h = (int32_t)s->height, bw = w * (int32_t)r->scale, bh = h * (int32_t)r->scale;
    VkImageBlit region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .srcOffsets = {{0, 0, 0}, {down ? bw : w, down ? bh : h, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .dstOffsets = {{0, 0, 0}, {down ? w : bw, down ? h : bh, 1}}};
    vkCmdBlitImage(cb, down ? s->big : s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down ? s->image : s->big,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   down && s->filtered ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}
/* the color surface's image made from its larger one, left in TRANSFER_SRC for a store, the larger one's layout as it
   was. Nothing to do at 1x */
static void color_down(const struct VitaRender* r, VkCommandBuffer cb, Surface* s) {
    if (!s->big) return;
    barrier(cb, s->big, VK_IMAGE_ASPECT_COLOR_BIT, s->big_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cb, s->image, VK_IMAGE_ASPECT_COLOR_BIT, s->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    color_blit(r, cb, s, true);
    barrier(cb, s->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cb, s->big, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->big_layout);
    s->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}
/* a depth surface's aspects, and its layout while it is drawn to */
static VkImageAspectFlags depth_aspects(const Surface* s) {
    return VK_IMAGE_ASPECT_DEPTH_BIT | (s->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}
static VkImageLayout depth_layout(const Surface* s) {
    return s->stencil ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
}
/* the scene's depth and stencil attachments, loaded (load, c unused) or cleared to the surface's background depth
   and stencil (the low byte of its background control word) */
static void depth_attachments(const struct VitaRender* r, const GxmContext* c, bool load,
                              VkRenderingAttachmentInfo* depth, VkRenderingAttachmentInfo* stencil) {
    *depth = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                         .imageView = r->depth->view,
                                         .imageLayout = depth_layout(r->depth),
                                         .loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR,
                                         .storeOp = VK_ATTACHMENT_STORE_OP_STORE, /* a texture's copy restarts the pass */
                                         .clearValue.depthStencil = {load ? 0.0f : c->depth.background_depth,
                                                                     load ? 0 : c->depth.background_control & 0xffu}};
    *stencil = *depth;
}

/* the prepared shaders kept between runs (prepare_shaders): the file read as the renderer starts, and the pipelines
   its workers made collected as a scene begins */
static void prepared_open(struct VitaRender* r, VitaProcess* p);
static void prep_feed(struct VitaRender* r, bool all);

/* ---- the renderer ---- */
#define COMPUTE_PUSH 64u /* the compute shaders' push constants, the largest */
static bool compute_pipeline(struct VitaRender* r, const u32* words, size_t size, VkPipeline* out) {
    return snx_vk_compute_pipeline(r->vk->device, VK_NULL_HANDLE, r->swizzle_layout, words, size, out) == VK_SUCCESS;
}
bool vita_render_init(VitaProcess* p) {
    VitaVk* v = vita_vk();
    if (!p->gxm || !v) return false;
    struct VitaRender* r = calloc(1, sizeof *r);
    if (!r) return false;
    r->vk = v;
    r->mem = p->mem;
    char scaled[160];
    r->scale = (uint32_t)snx_vk_whole_scale(g_settings.resolution_scale, 8, scaled, sizeof scaled);
    if (scaled[0]) fprintf(stderr, "scarletnx-vita: %s\n", scaled);
    /* the pipelines an earlier run made, as the Switch's and the 3DS's keep them (vk_pipeline_cache) */
    if (g_settings.vulkan_pipeline_cache && p->folder) {
        char why[512];
        size_t loaded = 0;
        if (snx_vk_cache_file(v->phys, p->folder, "vk_pipelines", r->cache_path, sizeof r->cache_path, why, sizeof why))
            fprintf(stderr, "scarletnx-vita: no pipeline cache kept between runs: %s\n", why);
        else {
            r->cache = snx_vk_pipeline_cache_load(v->device, &v->properties, r->cache_path, &loaded, why, sizeof why);
            if (why[0]) fprintf(stderr, "scarletnx-vita: %s: %s\n", r->cache_path, why);
            if (loaded)
                fprintf(stderr, "scarletnx-vita: the pipeline cache of earlier runs loaded (%zu KB)\n", loaded >> 10);
        }
    }
    VkDescriptorSetLayoutBinding bindings[3 + USSE_TEXTURES];
    for (unsigned i = 0; i < 2; ++i)
        bindings[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1,
                                                     i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT, NULL};
    for (unsigned i = 0; i < USSE_TEXTURES; ++i)
        bindings[2 + i] = (VkDescriptorSetLayoutBinding){USSE_TEXTURE_BINDING + i,
                                                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                         VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    /* the pixel's color for the programs that blend themselves */
    bindings[2 + USSE_TEXTURES] = (VkDescriptorSetLayoutBinding){USSE_COLOR_BINDING, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
                                                                 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo si = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 3 + USSE_TEXTURES,
                                          .pBindings = bindings};
    VkPushConstantRange push[2] = {{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(UssePush)},
                                   {VK_SHADER_STAGE_FRAGMENT_BIT, USSE_FRAGMENT_PUSH, 4}};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &r->set_layout,
                                     .pushConstantRangeCount = 2,
                                     .pPushConstantRanges = push};
    bool ok = vkCreateDescriptorSetLayout(v->device, &si, NULL, &r->set_layout) == VK_SUCCESS &&
              vkCreatePipelineLayout(v->device, &li, NULL, &r->layout) == VK_SUCCESS;
    for (unsigned s = 0; ok && s < VITA_VK_SLOTS; ++s) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .size = STAGING,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        ok = vkCreateBuffer(v->device, &bi, NULL, &r->staging[s].buffer) == VK_SUCCESS;
        VkMemoryRequirements req;
        if (ok) vkGetBufferMemoryRequirements(v->device, r->staging[s].buffer, &req);
        /* the shaders read their uniforms from here at each draw: in the device's own memory when the host can write
           it (a discrete device's BAR window), so the GPU reads them at its memory's speed and not across the bus */
        static const VkMemoryPropertyFlags wanted[2] = {
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
        for (unsigned w = 0; ok && w < 2 && !r->staging[s].memory; ++w) {
            VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                       .allocationSize = req.size,
                                       .memoryTypeIndex = vita_vk_memory_type(req.memoryTypeBits, wanted[w])};
            VkDeviceMemory memory = VK_NULL_HANDLE;
            if (ai.memoryTypeIndex != UINT32_MAX && vkAllocateMemory(v->device, &ai, NULL, &memory) == VK_SUCCESS)
                r->staging[s].memory = memory;
        }
        ok = ok && r->staging[s].memory &&
             vkBindBufferMemory(v->device, r->staging[s].buffer, r->staging[s].memory, 0) == VK_SUCCESS &&
             vkMapMemory(v->device, r->staging[s].memory, 0, STAGING, 0, (void**)&r->staging[s].map) == VK_SUCCESS;
    }
    for (unsigned lin = 0; ok && lin < 2; ++lin)
        for (unsigned mip = 0; ok && mip < 2; ++mip)
            for (unsigned mode = 0; ok && mode < 4; ++mode) {
                static const VkSamplerAddressMode modes[4] = {
                    VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
                    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER};
                /* the Switch's anisotropy on a mipmapped texture filtered linearly, as the 3DS's */
                float anisotropy = lin && mip ? v->anisotropy : 0;
                VkSamplerCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                          .anisotropyEnable = anisotropy > 0,
                                          .maxAnisotropy = anisotropy,
                                          .magFilter = lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                          .minFilter = lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                          .mipmapMode =
                                              mip ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                          .addressModeU = modes[mode],
                                          .addressModeV = modes[mode],
                                          .addressModeW = modes[mode],
                                          .maxLod = VK_LOD_CLAMP_NONE,
                                          .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
                ok = vkCreateSampler(v->device, &ci, NULL, &r->samplers[lin][mip][mode]) == VK_SUCCESS;
            }
    /* the deswizzle: two storage buffers, its parameters, its pipeline; its linear rows' rings */
    ok = ok &&
         snx_vk_compute_layouts(v->device, 2, 0, COMPUTE_PUSH, &r->swizzle_set, &r->swizzle_layout) == VK_SUCCESS &&
         compute_pipeline(r, vita_swizzle_spv, (size_t)vita_swizzle_spv_size, &r->swizzle) &&
         compute_pipeline(r, vita_picture_spv, (size_t)vita_picture_spv_size, &r->picture) &&
         compute_pipeline(r, vita_yuv_spv, (size_t)vita_yuv_spv_size, &r->yuv) &&
         compute_pipeline(r, vita_rgb24_spv, (size_t)vita_rgb24_spv_size, &r->rgb24);
    for (unsigned s = 0; ok && s < VITA_VK_SLOTS; ++s) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .size = LINEAR,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        ok = vkCreateBuffer(v->device, &bi, NULL, &r->linear[s].buffer) == VK_SUCCESS;
        VkMemoryRequirements req;
        if (ok) vkGetBufferMemoryRequirements(v->device, r->linear[s].buffer, &req);
        VkMemoryAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = ok ? req.size : 0,
            .memoryTypeIndex = ok ? vita_vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) : 0};
        ok = ok && vkAllocateMemory(v->device, &ai, NULL, &r->linear[s].memory) == VK_SUCCESS &&
             vkBindBufferMemory(v->device, r->linear[s].buffer, r->linear[s].memory, 0) == VK_SUCCESS;
    }
    p->gxm->render = r;
    g_retiring = r;
    vita_vk_on_retire(retire);
    if (!v->host_import) vita_vk_on_submit(submitting);
    if (!ok) {
        printf("gxm: cannot set up the renderer: scenes are not drawn\n");
        vita_render_free(p);
        return false;
    }
    prepared_open(r, p); /* with the layout and the driver's cache its pipelines are made with */
    return true;
}
static void unbury(VkDevice d, const Buried* b) {
    if (b->pipeline) vkDestroyPipeline(d, b->pipeline, NULL);
    if (b->view) vkDestroyImageView(d, b->view, NULL);
    if (b->image) vkDestroyImage(d, b->image, NULL);
    if (b->memory) vkFreeMemory(d, b->memory, NULL);
}
/* b kept until the recording open now is done, or gone at once after a wait for the device when there is no room */
static void bury(struct VitaRender* r, Buried b) {
    if (r->buried_count == sizeof r->buried / sizeof *r->buried) {
        vkDeviceWaitIdle(r->vk->device);
        unbury(r->vk->device, &b);
        return;
    }
    b.serial = r->vk->ring.submitted + 1;
    r->buried[r->buried_count++] = b;
}
static void retire(unsigned slot, uint64_t serial) {
    if (!g_retiring) return;
    struct VitaRender* r = g_retiring;
    for (unsigned i = 0; i < r->buried_count;)
        if (r->buried[i].serial <= serial) {
            unbury(r->vk->device, &r->buried[i]);
            r->buried[i] = r->buried[--r->buried_count];
        } else
            ++i;
    mirror_drain(g_retiring, serial, IMPORTS);
    g_retiring->staging[slot].used = 0, g_retiring->linear[slot].used = 0;
}
/* the GPU reads memory when its work runs: the copies take what the CPU wrote since the work was recorded */
static void submitting(void) {
    for (unsigned i = 0; g_retiring && i < IMPORTS; ++i) mirror_sync(g_retiring, &g_retiring->imports[i]);
}
void vita_render_shader_free(VitaProcess* p, void* shader) {
    struct VitaRender* r = render_of(p);
    for (Shader *s = shader, *next; s; s = next) {
        next = s->next;
        if (r && s->module) vkDestroyShaderModule(r->vk->device, s->module, NULL);
        free(s);
    }
}
void vita_render_free(VitaProcess* p) {
    struct VitaRender* r = render_of(p);
    if (!r) return;
    VkDevice d = r->vk->device;
    /* the workers stopped, every pipeline begun made, and the ones no draw collected freed */
    snx_vk_jobs_stop(&r->pool, NULL);
    for (unsigned i = 0; i < SNX_VK_JOBS; ++i)
        if (snx_vk_jobs_done(&r->pool, i) && r->jobs[i].made) vkDestroyPipeline(d, r->jobs[i].made, NULL);
    if (r->skipped)
        fprintf(stderr, "scarletnx-vita: async_pipelines=%d skipped %llu draw%s while their pipelines were made\n",
                g_settings.async_pipelines, (unsigned long long)r->skipped, r->skipped == 1 ? "" : "s");
    if (r->feed.count && g_settings.prepare_shaders == 1)
        fprintf(
            stderr,
            "scarletnx-vita: prepare_shaders=1 made %u of the %zu pipelines of earlier runs before the game ended\n",
            r->counts.made, r->feed.count);
    snx_prep_close(&r->prep);
    vkDeviceWaitIdle(d);
    vita_vk_on_retire(NULL);
    vita_vk_on_submit(NULL);
    g_retiring = NULL;
    for (unsigned i = 0; i < GXM_PROGRAMS; ++i) {
        vita_render_shader_free(p, p->gxm->vertex[i].shader);
        p->gxm->vertex[i].shader = NULL;
    }
    for (unsigned i = 0; i < GXM_FRAGMENT_PROGRAMS; ++i) {
        vita_render_shader_free(p, p->gxm->fragment[i].shader);
        p->gxm->fragment[i].shader = NULL;
    }
    for (unsigned i = 0; i < PIPELINES; ++i)
        if (r->pipelines[i].pipeline) vkDestroyPipeline(d, r->pipelines[i].pipeline, NULL);
    for (unsigned i = 0; i < r->buried_count; ++i) unbury(d, &r->buried[i]);
    if (r->vk->gpl.on)
        fprintf(stderr,
                "scarletnx-vita: pipeline_libraries=%d linked %u pipeline%s fast and %u optimized, from %zu parts (%u "
                "reused)\n",
                g_settings.pipeline_libraries, atomic_load_explicit(&r->gpl_linked, memory_order_relaxed),
                atomic_load_explicit(&r->gpl_linked, memory_order_relaxed) == 1 ? "" : "s",
                atomic_load_explicit(&r->gpl_optimized, memory_order_relaxed), r->parts.count, r->parts.reused);
    snx_vk_gpl_parts_free(&r->parts, d);             /* after the pipelines linked from them */
    for (uint32_t i = 0; i < r->prepared_count; ++i) /* the file's shaders no program took */
        if (r->prepared[i]) {
            if (r->prepared[i]->module) vkDestroyShaderModule(d, r->prepared[i]->module, NULL);
            free(r->prepared[i]);
        }
    free(r->prepared);
    free(r->prep_pipes);
    if (r->cache) {
        char why[512];
        size_t saved = 0;
        if (snx_vk_pipeline_cache_save(d, r->cache, r->cache_path, &saved, why, sizeof why))
            fprintf(stderr, "scarletnx-vita: the pipeline cache could not be written to %s: %s\n", r->cache_path, why);
        else
            fprintf(stderr, "scarletnx-vita: the pipeline cache written (%zu KB)\n", saved >> 10);
        vkDestroyPipelineCache(d, r->cache, NULL);
    }
    for (unsigned i = 0; i < SURFACES; ++i) surface_drop(d, &r->surfaces[i]);
    for (unsigned i = 0; i < TEXTURES; ++i)
        if (r->textures[i].image) {
            vkDestroyImageView(d, r->textures[i].view, NULL);
            vkDestroyImage(d, r->textures[i].image, NULL);
            vkFreeMemory(d, r->textures[i].memory, NULL);
        }
    for (unsigned i = 0; i < IMPORTS; ++i)
        if (r->imports[i].size) import_free(r, &r->imports[i]);
    for (unsigned s = 0; s < VITA_VK_SLOTS; ++s) {
        if (r->staging[s].buffer) vkDestroyBuffer(d, r->staging[s].buffer, NULL);
        if (r->staging[s].memory) vkFreeMemory(d, r->staging[s].memory, NULL);
        if (r->linear[s].buffer) vkDestroyBuffer(d, r->linear[s].buffer, NULL);
        if (r->linear[s].memory) vkFreeMemory(d, r->linear[s].memory, NULL);
    }
    if (r->swizzle) vkDestroyPipeline(d, r->swizzle, NULL);
    if (r->picture) vkDestroyPipeline(d, r->picture, NULL);
    if (r->yuv) vkDestroyPipeline(d, r->yuv, NULL);
    if (r->rgb24) vkDestroyPipeline(d, r->rgb24, NULL);
    if (r->swizzle_layout) vkDestroyPipelineLayout(d, r->swizzle_layout, NULL);
    if (r->swizzle_set) vkDestroyDescriptorSetLayout(d, r->swizzle_set, NULL);
    for (unsigned i = 0; i < 16; ++i) {
        VkSampler s = (&r->samplers[0][0][0])[i];
        if (s) vkDestroySampler(d, s, NULL);
    }
    if (r->layout) vkDestroyPipelineLayout(d, r->layout, NULL);
    if (r->set_layout) vkDestroyDescriptorSetLayout(d, r->set_layout, NULL);
    free(r->writeback);
    free(r);
    p->gxm->render = NULL;
}

/* ---- scenes ---- */
/* The scene's D16 depth surface as its memory holds it, when the game loads or stores it (mask: GXM_DEPTH_FORCE_LOAD
   or _STORE) and the GPU reaches it: rows of the stride's samples, 16 bits each, a pixel's samples side by side (2x1
   for SCE_GXM_MULTISAMPLE_2X, 2x2 for 4X). Its buffer and offset, and the sample grid's factors; NULL for none. A
   multisampled one is made from (or into) the pixels' depth through the surface's sample image, each pixel's depth its
   samples' */
static const Import* depth_memory(struct VitaRender* r, VitaProcess* p, const GxmContext* c, uint32_t mask,
                                  VkDeviceSize* at, uint32_t* fx, uint32_t* fy) {
    uint32_t control = c->depth.control, stride = control >> GXM_DEPTH_STRIDE_SHIFT;
    if (!r->depth || r->depth->vk_format != VK_FORMAT_D16_UNORM || !(control & mask) || (control & GXM_DEPTH_TILED))
        return NULL;
    uint32_t multisample = p->gxm->targets[c->target].multisample;
    *fx = multisample ? 2 : 1, *fy = multisample == 2 ? 2 : 1;
    if (stride < r->depth->width * *fx) return NULL;
    const Import* m = buffer_of(r, p, c->depth.depth, stride * 2 * r->depth->height * *fy, at);
    if (!m) {
        once_value(r, 20, c->depth.depth, "the depth surface at 0x%08" PRIx32 " is not in memory the GPU can reach",
                   c->depth.depth);
        return NULL;
    }
    /* the memory's samples go through an image of their own when they aren't the drawn image's pixels: several a
       pixel, or the scene drawn larger */
    if ((*fx > 1 || *fy > 1 || r->scale > 1) && !r->depth->samples) {
        VkComponentMapping identity = {0};
        Surface* s = r->depth;
        if (!image_make(r, VK_FORMAT_D16_UNORM, s->width * *fx, s->height * *fy, 1,
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                        identity, false, &s->samples, NULL, &s->samples_memory))
            return NULL;
    }
    return m;
}
/* the blit of one depth image's whole extent onto another's, nearest: a pixel's depth to its samples, or a sample
   to its pixel */
static void depth_blit(VkCommandBuffer cb, VkImage from, uint32_t fw, uint32_t fh, VkImage to, uint32_t tw,
                       uint32_t th) {
    VkImageBlit region = {.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                          .srcOffsets = {{0, 0, 0}, {(int32_t)fw, (int32_t)fh, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                          .dstOffsets = {{0, 0, 0}, {(int32_t)tw, (int32_t)th, 1}}};
    vkCmdBlitImage(cb, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   VK_FILTER_NEAREST);
}
/* the bytes of the color surface's memory, 0 for a format not drawn yet */
static uint32_t color_bytes(const GxmContext* c) {
    VkFormat vk;
    unsigned bytes;
    return color_format(c->color.format, &vk, &bytes) ? c->color.stride * bytes * c->color.height : 0;
}
void vita_render_begin_scene(VitaProcess* p, GxmContext* c) {
    struct VitaRender* r = render_of(p);
    if (!r) return;
    if (r->feed.next < r->feed.count || r->feed.making) prep_feed(r, false);
    ++r->scene;
    r->rendering = false;
    r->color = r->depth = NULL;
    if (!c->has_color) {
        once(r, 1, "a scene without a color surface is not drawn");
        return;
    }
    if (!color_bytes(c)) {
        once_value(r, 2, c->color.format, "color format 0x%08" PRIx32 " is not drawn yet", c->color.format);
        return;
    }
    const GxmTarget* target = &p->gxm->targets[c->target];
    r->width = c->color.width < target->width ? c->color.width : target->width;
    r->height = c->color.height < target->height ? c->color.height : target->height;
    r->color = surface(r, c->color.data, c->color.width, c->color.height, c->color.stride, c->color.format,
                       c->color.type, false);
    VkDeviceSize at;
    const Import* m = r->color ? buffer_of(r, p, c->color.data, color_bytes(c), &at) : NULL;
    if (!m) {
        once_value(r, 3, c->color.data, "the color surface at 0x%08" PRIx32 " is not in memory the GPU can reach",
                   c->color.data);
        r->color = NULL;
        return;
    }
    if (c->has_depth && c->depth.depth)
        r->depth = surface(r, c->depth.depth, c->color.width, c->color.height, 0,
                           c->depth.control & GXM_DEPTH_FORMAT_MASK, 0, true);
    VkCommandBuffer cb = vita_vk_commands();
    /* the surface's memory into the image: what the CPU or other surfaces left there, unless the image still holds
       what it stored last. Drawn larger, the image is made into the larger one, which the scene draws to */
    Surface* s = r->color;
    r->drawn = s->big ? r->scale : 1;
    if (!s->synced || vita_watch_changed(s->address, color_bytes(c), s->since)) {
        barrier(cb, s->image, VK_IMAGE_ASPECT_COLOR_BIT, s->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        color_copy(r, cb, s, m, at, false);
        if (s->big) {
            barrier(cb, s->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            s->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier(cb, s->big, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            color_blit(r, cb, s, false);
            barrier(cb, s->big, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, color_layout(r));
        } else
            barrier(cb, s->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, color_layout(r));
    } else if (s->big)
        barrier(cb, s->big, VK_IMAGE_ASPECT_COLOR_BIT, s->big_layout, color_layout(r));
    else
        barrier(cb, s->image, VK_IMAGE_ASPECT_COLOR_BIT, s->layout, color_layout(r));
    if (s->big)
        s->big_layout = color_layout(r);
    else
        s->layout = color_layout(r);
    /* the depth: what its memory holds when the game loads it (force load), else the background depth */
    bool depth_loaded = false;
    if (r->depth) {
        uint32_t fx, fy;
        VkDeviceSize depth_at;
        const Import* dm = depth_memory(r, p, c, GXM_DEPTH_FORCE_LOAD, &depth_at, &fx, &fy);
        Surface* d = r->depth;
        if (dm) {
            bool sampled = fx * fy > 1 || r->scale > 1;
            VkImage into = sampled ? d->samples : d->image;
            barrier(cb, into, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy load = {.bufferOffset = depth_at,
                                      .bufferRowLength = c->depth.control >> GXM_DEPTH_STRIDE_SHIFT,
                                      .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                                      .imageExtent = {d->width * fx, d->height * fy, 1}};
            vkCmdCopyBufferToImage(cb, dm->buffer, into, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &load);
            if (sampled) {
                barrier(cb, d->samples, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                barrier(cb, d->image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                depth_blit(cb, d->samples, d->width * fx, d->height * fy, d->image, d->width * r->drawn,
                           d->height * r->drawn);
            }
            barrier(cb, d->image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, depth_layout(d));
            depth_loaded = true;
        } else
            barrier(cb, d->image, depth_aspects(d), VK_IMAGE_LAYOUT_UNDEFINED, depth_layout(d));
        d->layout = depth_layout(d);
    }
    VkRenderingAttachmentInfo color = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                       .imageView = drawn_view(r->color),
                                       .imageLayout = color_layout(r),
                                       .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                       .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingAttachmentInfo depth, stencil;
    if (r->depth) depth_attachments(r, c, depth_loaded, &depth, &stencil);
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                          .renderArea = {{0, 0}, {r->width * r->drawn, r->height * r->drawn}},
                          .layerCount = 1,
                          .colorAttachmentCount = 1,
                          .pColorAttachments = &color,
                          .pDepthAttachment = r->depth ? &depth : NULL,
                          .pStencilAttachment = r->depth && r->depth->stencil ? &stencil : NULL};
    vkCmdBeginRendering(cb, &ri);
    r->rendering = true;
    r->bound = (Bound){0};
}
uint64_t vita_render_end_scene(VitaProcess* p, GxmContext* c) {
    struct VitaRender* r = render_of(p);
    if (!r) return 0;
    if (r->rendering) {
        VkCommandBuffer cb = vita_vk_commands();
        vkCmdEndRendering(cb);
        /* the image back into the surface's memory, which the display and the game read */
        VkDeviceSize at;
        const Import* m = buffer_of(r, p, c->color.data, color_bytes(c), &at);
        if (r->color->big) {
            barrier(cb, r->color->big, VK_IMAGE_ASPECT_COLOR_BIT, color_layout(r),
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            r->color->big_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            color_down(r, cb, r->color);
        } else
            barrier(cb, r->color->image, VK_IMAGE_ASPECT_COLOR_BIT, color_layout(r),
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        r->color->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        r->color->synced = m && color_copy(r, cb, r->color, m, at, true);
        if (r->color->synced) {
            /* the store is the memory's newest write, which the textures on it and the other surfaces take */
            color_written(r, m, r->color);
            gpu_wrote(r, r->color->address, color_bytes(c));
            r->color->since = vita_watch_protect(r->color->address, color_bytes(c));
        }
        /* the depth into its memory when the game keeps it (force store), which it reads as a texture */
        uint32_t fx, fy;
        VkDeviceSize depth_at;
        const Import* dm = depth_memory(r, p, c, GXM_DEPTH_FORCE_STORE, &depth_at, &fx, &fy);
        if (dm) {
            Surface* d = r->depth;
            bool sampled = fx * fy > 1 || r->scale > 1;
            barrier(cb, d->image, VK_IMAGE_ASPECT_DEPTH_BIT, d->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            d->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            if (sampled) {
                barrier(cb, d->samples, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                depth_blit(cb, d->image, d->width * r->drawn, d->height * r->drawn, d->samples, d->width * fx,
                           d->height * fy);
                barrier(cb, d->samples, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            }
            memory_barrier(cb); /* after every earlier access to the memory, as a color store */
            VkBufferImageCopy store = {.bufferOffset = depth_at,
                                       .bufferRowLength = c->depth.control >> GXM_DEPTH_STRIDE_SHIFT,
                                       .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                                       .imageExtent = {d->width * fx, d->height * fy, 1}};
            vkCmdCopyImageToBuffer(cb, sampled ? d->samples : d->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   dm->buffer, 1, &store);
            uint32_t depth_stride = (c->depth.control >> GXM_DEPTH_STRIDE_SHIFT) * 2;
            mirror_written(r, dm, c->depth.depth, d->width * fx * 2, depth_stride, d->height * fy);
            gpu_wrote(r, c->depth.depth, depth_stride * d->height * fy);
        }
        memory_barrier(cb);
        r->rendering = false;
    }
    return vita_vk_submit();
}
bool vita_render_done(VitaProcess* p, uint64_t serial) {
    struct VitaRender* r = render_of(p);
    return !r || vita_vk_poll() >= serial;
}
bool vita_render_wait(VitaProcess* p, uint64_t serial) {
    struct VitaRender* r = render_of(p);
    return !r || vita_vk_wait(serial);
}
bool vita_render_idle(VitaProcess* p, bool wait) {
    struct VitaRender* r = render_of(p);
    if (!r) return true;
    if (wait) return vita_vk_wait(r->vk->ring.submitted);
    return vita_vk_poll() >= r->vk->ring.submitted;
}
bool vita_render_pending(VitaProcess* p) {
    struct VitaRender* r = render_of(p);
    return r && vita_vk_poll() < r->vk->ring.submitted;
}
uint64_t vita_render_finish(VitaProcess* p) {
    struct VitaRender* r = render_of(p);
    if (!r || !r->vk->ring.recording) return r ? r->vk->ring.submitted : 0;
    VkCommandBuffer cb = vita_vk_commands();
    if (r->rendering) rendering_pause(cb);
    memory_barrier(cb);
    uint64_t serial = vita_vk_submit();
    if (r->rendering) rendering_resume(r, vita_vk_commands());
    return serial;
}
/* the color surface drawn larger that holds the framebuffer at address (pitch pixels a row, width by height of them) as
   its memory does, unwritten since its store: NULL for none, or when it is the one being drawn */
static Surface* drawn_larger(struct VitaRender* r, uint32_t address, uint32_t pitch, uint32_t width, uint32_t height) {
    for (unsigned i = 0; i < SURFACES; ++i) {
        Surface* s = &r->surfaces[i];
        VkFormat vk;
        unsigned bytes;
        if (!s->big || s == r->color || !s->synced || s->address != address || s->type != GXM_COLOR_SURFACE_LINEAR ||
            s->stride != pitch || s->width < width || s->height < height || !color_format(s->format, &vk, &bytes) ||
            bytes != 4)
            continue;
        if (!vita_watch_changed(address, pitch * 4 * height, s->since)) return s;
    }
    return NULL;
}
bool vita_render_show(VitaProcess* p, uint32_t address, uint32_t pitch, uint32_t width, uint32_t height) {
    struct VitaRender* r = render_of(p);
    if (!r) return false;
    /* nothing new to show: the window keeps it, but for a present every half second (a swapchain made anew) */
    uint32_t bytes = pitch * 4 * height;
    if (address == r->shown && !vita_watch_changed(address, bytes, r->shown_since) && ++r->shown_skips < 30)
        return true;
    VkDeviceSize at;
    const Import* m = buffer_of(r, p, address, bytes, &at);
    if (!m) {
        once_value(r, 21, address, "the display's framebuffer at 0x%08" PRIx32 " is not in memory the GPU can reach",
                   address);
        return false;
    }
    uint64_t since = vita_watch_protect(address, bytes);
    /* the present's own commands (a copy, a blit) can't go inside the next scene's render pass, which the game may
       already have open (begin_scene) while this, the already-finished frame, is still what gets shown. The present
       submits, which ends the recording cb points to, so resume takes a fresh one rather than reuse cb (as
       vita_render_finish does around its own submit) */
    if (r->rendering) rendering_pause(vita_vk_commands());
    /* a framebuffer a scene drew larger shows its larger image, which the memory was made from at its store */
    Surface* big = drawn_larger(r, address, pitch, width, height);
    if (big) {
        VkCommandBuffer cb = vita_vk_commands();
        barrier(cb, big->big, VK_IMAGE_ASPECT_COLOR_BIT, big->big_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        big->big_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    bool shown = big ? vita_vk_present(VK_NULL_HANDLE, 0, 0, big->big, width * r->scale, height * r->scale)
                     : vita_vk_present(m->buffer, at, pitch, VK_NULL_HANDLE, width, height);
    if (r->rendering) rendering_resume(r, vita_vk_commands());
    if (shown) r->shown = address, r->shown_since = since, r->shown_skips = 0;
    return true;
}
uint64_t vita_render_submitted(VitaProcess* p) {
    struct VitaRender* r = render_of(p);
    return r ? r->vk->ring.submitted : 0;
}

/* ---- draws ---- */
/* a shader's id: its program's bytes (code: as many as its header says it has, hashed once a program, not at each
   draw) and the hints, zeroed before they are set, with the formats of the textures sampled (the mask's) alone, the
   others telling the translator nothing */
static uint64_t shader_id(uint64_t code, const UsseHints* hints, uint32_t sampled) {
    UsseHints h = *hints;
    for (unsigned i = 0; i < USSE_TEXTURES; ++i)
        if (!(sampled & (1u << i))) h.texture_formats[i] = 0;
    return snx_fnv1a(code, &h, sizeof h);
}
/* ---- the prepared shaders kept between runs (prepare_shaders) ---- */

/* the file's records: a shader (PrepShader, then its program's bytes) and a pipeline (PrepPipe) */
enum { PREP_SHADER = 1, PREP_PIPE };
#define PREP_MAGIC   "SNXVITPS"
#define PREP_VERSION 1
typedef struct {
    UsseHints hints; /* the formats of the textures it does not sample zeroed */
    uint32_t size, pad;
} PrepShader;
/* a shader of the file's that a program of code, drawn with hints, is made into, taken from the list: NULL for none */
static Shader* prepared_take(struct VitaRender* r, uint64_t code, const UsseHints* hints) {
    for (uint32_t i = 0; i < r->prepared_count; ++i) {
        Shader* s = r->prepared[i];
        if (s && s->code == code && s->id == shader_id(code, hints, s->meta.sampler_mask)) {
            r->prepared[i] = NULL;
            return s;
        }
    }
    return NULL;
}
static void prep_add_shader(struct VitaRender* r, const void* gxp, uint32_t size, const UsseHints* hints, Shader* s) {
    if (!r->prep.f) return;
    PrepShader h = {.hints = *hints, .size = size};
    for (unsigned i = 0; i < USSE_TEXTURES; ++i)
        if (!(s->meta.sampler_mask & (1u << i))) h.hints.texture_formats[i] = 0;
    if (!snx_prep_add(&r->prep, PREP_SHADER, &h, sizeof h, gxp, size)) s->prep = ++r->prep_shaders;
}
/* whether key is a pipeline of the file's, made or still to make */
static bool prep_listed(const struct VitaRender* r, uint64_t key) {
    for (size_t i = 0; i < r->feed.count; ++i)
        if (r->prep_pipes[i].key == key) return true;
    return false;
}
/* a pipeline a draw made: into the file, its shaders named by their records, unless the file has it */
static void prep_add_pipe(struct VitaRender* r, uint64_t key, const PipeDesc* d) {
    ++r->counts.drawn;
    if (!r->prep.f || !d->vs_prep || !d->fs_prep || prep_listed(r, key)) return;
    PrepPipe k = {.key = key, .vs = d->vs_prep - 1, .fs = d->fs_prep - 1, .d = *d};
    k.d.vs = k.d.fs = VK_NULL_HANDLE, k.d.cache = VK_NULL_HANDLE, k.d.made = VK_NULL_HANDLE;
    k.d.vs_prep = k.d.fs_prep = 0;
    memset(k.d.linked, 0, sizeof k.d.linked);
    (void)snx_prep_add(&r->prep, PREP_PIPE, &k, sizeof k, NULL, 0);
}
static bool grow(void** items, size_t* room, size_t count, size_t size) {
    if (count < *room) return true;
    size_t n = *room ? *room * 2 : 16;
    void* p = realloc(*items, n * size);
    if (!p) return false;
    *items = p;
    *room = n;
    return true;
}
/* one record read: its shader made from its program, or its pipeline listed; false for one this build cannot read,
   which ends the file */
static bool prep_each(void* ctx, uint32_t type, const uint8_t* payload, uint32_t len) {
    struct VitaRender* r = ctx;
    if (type == PREP_SHADER && len >= sizeof(PrepShader)) {
        PrepShader h;
        memcpy(&h, payload, sizeof h);
        if (h.size > len - sizeof h ||
            !grow((void**)&r->prepared, &r->prepared_room, r->prepared_count, sizeof *r->prepared))
            return false;
        Shader* s = calloc(1, sizeof *s);
        if (!s) return false;
        const uint8_t* gxp = payload + sizeof h;
        char error[256];
        UsseShader out;
        s->code = snx_fnv1a(SNX_FNV_BASIS, gxp, h.size);
        s->prep = ++r->prep_shaders;
        if (usse_translate(gxp, h.size, &h.hints, r->vk->preserve_infnan, &out, error, sizeof error)) {
            VkShaderModuleCreateInfo mi = {
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = out.count * 4, .pCode = out.words};
            if (vkCreateShaderModule(r->vk->device, &mi, NULL, &s->module) != VK_SUCCESS) s->module = VK_NULL_HANDLE;
            free(out.words);
            out.words = NULL;
            s->meta = out;
        }
        s->failed = !s->module;
        s->id = shader_id(s->code, &h.hints, s->meta.sampler_mask);
        r->prepared[r->prepared_count++] = s;
        ++r->counts.shaders;
        return true;
    }
    if (type == PREP_PIPE && len >= sizeof(PrepPipe)) {
        PrepPipe k;
        memcpy(&k, payload, sizeof k);
        if (k.vs >= r->prepared_count || k.fs >= r->prepared_count) return false;
        ++r->counts.pipelines;
        const Shader *vs = r->prepared[k.vs], *fs = r->prepared[k.fs];
        if (!vs->module || !fs->module) return true; /* one of a program not translated: none, as a draw of it */
        if (!grow((void**)&r->prep_pipes, &r->prep_pipe_room, r->feed.count, sizeof *r->prep_pipes)) return false;
        k.d.vs = vs->module, k.d.fs = fs->module, k.d.cache = r->cache;
        r->prep_pipes[r->feed.count++] = k;
        return true;
    }
    return false;
}
/* a pipeline of the file's, made: at its key, unless a draw made it first or the table is full */
static void prep_adopt(struct VitaRender* r, uint64_t key, VkPipeline made) {
    if (!made) return;
    unsigned slot = (unsigned)(key % PIPELINES);
    for (unsigned probe = 0; probe < PIPELINES; ++probe, slot = (slot + 1) % PIPELINES) {
        Pipeline* e = &r->pipelines[slot];
        if (e->key == key && (e->pipeline || e->job)) break;
        if (!e->pipeline && !e->job) {
            *e = (Pipeline){.key = key, .pipeline = made};
            ++r->counts.made;
            return;
        }
    }
    vkDestroyPipeline(r->vk->device, made, NULL);
}
static void prep_fill(void* owner, unsigned job, size_t item) {
    struct VitaRender* r = owner;
    r->jobs[job] = r->prep_pipes[item].d;
}
static void prep_made(void* owner, unsigned job, size_t item) {
    struct VitaRender* r = owner;
    prep_adopt(r, r->prep_pipes[item].key, r->jobs[job].made);
    r->jobs[job] = (PipeDesc){0};
}
static void prep_feed(struct VitaRender* r, bool all) {
    snx_vk_jobs_feed(&r->pool, &r->feed, all, prep_fill, prep_made);
}
static void job_make(void* owner, unsigned job);
static VkPipeline make_pipe(struct VitaRender* r, const PipeDesc* d, bool optimize, VkPipeline linked[]);
/* the workers, as the draws' pipelines and the prepared ones share them: how many there are */
static unsigned pipe_workers(struct VitaRender* r) {
    r->pool.make = job_make, r->pool.owner = r, r->pool.who = "scarletnx-vita", r->pool.thread = "Vita pipelines";
    return snx_vk_jobs_start(&r->pool, g_settings.pipeline_threads);
}
static void prepared_open(struct VitaRender* r, VitaProcess* p) {
    if (!g_settings.prepare_shaders || !p->folder) return;
    char path[1200], why[512], key[256];
    r->prep.who = "scarletnx-vita";
    if (snx_vk_cache_file(r->vk->phys, p->folder, "vk_prepared", path, sizeof path, why, sizeof why)) {
        fprintf(stderr, "scarletnx-vita: prepare_shaders=%d keeps no shaders between runs: %s\n",
                g_settings.prepare_shaders, why);
        return;
    }
    /* the records are the game's programs and the renderer's state as this build lays them out */
    snprintf(key, sizeof key, "guest=%d sizes=%zu,%zu; %s", PREP_VERSION, sizeof(UsseHints), sizeof(PipeDesc),
             snx_build_commit());
    r->prep.pack = g_settings.compress_prepared_shaders;
    const uint64_t t0 = plat_ns();
    (void)snx_prep_open(&r->prep, path, PREP_MAGIC, PREP_VERSION, key, prep_each, r);
    if (!r->counts.shaders && !r->counts.pipelines) return;
    const bool workers = r->feed.count && pipe_workers(r);
    const bool now = r->feed.count && (g_settings.prepare_shaders == 2 || !workers);
    if (workers)
        prep_feed(r, now); /* every one, or the first ones to the workers */
    else if (now)
        for (; r->feed.next < r->feed.count; ++r->feed.next) {
            PrepPipe* k = &r->prep_pipes[r->feed.next];
            prep_adopt(r, k->key, make_pipe(r, &k->d, true, NULL));
        }
    if (now || !r->feed.count)
        fprintf(stderr,
                "scarletnx-vita: %s: %u shaders and %u pipelines of earlier runs, %u of the pipelines made before the "
                "first draw in %.0f ms%s\n",
                path, r->counts.shaders, r->counts.pipelines, r->counts.made, (double)(plat_ns() - t0) / 1e6,
                now && g_settings.prepare_shaders == 1 ? " (prepare_shaders=1, but no pipeline worker started)" : "");
    else
        fprintf(stderr,
                "scarletnx-vita: %s: %u shaders and %u pipelines of earlier runs, the pipelines made on the workers "
                "while the game runs (prepare_shaders=1)\n",
                path, r->counts.shaders, r->counts.pipelines);
}
void vita_render_pipelines_wait(VitaProcess* p) {
    struct VitaRender* r = render_of(p);
    if (r) snx_vk_jobs_wait(&r->pool, false, NULL);
}
VitaLibraries vita_render_libraries(VitaProcess* p) {
    struct VitaRender* r = render_of(p);
    if (!r) return (VitaLibraries){0};
    SDL_LockMutex(r->parts.lock);
    VitaLibraries l = {.linked = atomic_load_explicit(&r->gpl_linked, memory_order_relaxed),
                       .optimized = atomic_load_explicit(&r->gpl_optimized, memory_order_relaxed),
                       .parts = (unsigned)r->parts.count,
                       .reused = r->parts.reused,
                       .replaced = r->gpl_replaced};
    SDL_UnlockMutex(r->parts.lock);
    return l;
}
VitaPrepared vita_render_prepared(VitaProcess* p) {
    const struct VitaRender* r = render_of(p);
    return r ? r->counts : (VitaPrepared){0};
}

/* The program's shader for hints: the slot's, a list each program keeps (its first shader), one made for the formats
   of the textures it samples, which the first one's translation says, or taken from the prepared file's; a new one
   made, put after the others and into the file */
static Shader* shader_for(struct VitaRender* r, VitaProcess* p, void** slot, uint32_t program, const UsseHints* hints) {
    Shader* first = *slot;
    if (first) {
        const uint64_t id = shader_id(first->code, hints, first->meta.sampler_mask);
        for (Shader* s = first; s; s = s->next)
            if (s->id == id) return s;
    }
    const uint64_t code =
        first ? first->code : snx_fnv1a(SNX_FNV_BASIS, vita_ptr(p, program), snx_ld_le32(vita_ptr(p, program + 8)));
    Shader* s = prepared_take(r, code, hints);
    const bool made = s != NULL;
    if (!s && !(s = calloc(1, sizeof *s))) return NULL;
    s->code = code;
    if (first) {
        Shader* last = first;
        while (last->next) last = last->next;
        last->next = s;
    } else
        *slot = s;
    if (made) return s;
    char error[256];
    UsseShader out;
    if (g_settings.vita_shader_corpus[0] &&
        !usse_corpus_write(g_settings.vita_shader_corpus, vita_ptr(p, program), snx_ld_le32(vita_ptr(p, program + 8)),
                           hints))
        once(r, 15, "cannot write the shader corpus in %s", g_settings.vita_shader_corpus);
    if (!usse_translate(vita_ptr(p, program), snx_ld_le32(vita_ptr(p, program + 8)), hints, r->vk->preserve_infnan, &out,
                        error, sizeof error)) {
        printf("gxm: the program at 0x%08" PRIx32 " is not drawn: %s\n", program, error);
        s->failed = true;
        s->id = shader_id(s->code, hints, first ? first->meta.sampler_mask : 0);
        return s;
    }
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = out.count * 4,
                                   .pCode = out.words};
    if (vkCreateShaderModule(r->vk->device, &mi, NULL, &s->module) != VK_SUCCESS) {
        printf("gxm: the device refuses the shader of the program at 0x%08" PRIx32 "\n", program);
        s->failed = true;
    }
    free(out.words);
    out.words = NULL;
    s->meta = out;
    s->id = shader_id(s->code, hints, out.sampler_mask);
    if (s->module) prep_add_shader(r, vita_ptr(p, program), snx_ld_le32(vita_ptr(p, program + 8)), hints, s);
    return s;
}
/* SceGxmAttributeFormat and a component count as a vertex format */
static VkFormat attribute_format(unsigned format, unsigned n) {
    static const VkFormat formats[10][4] = {
        {VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT},
        {VK_FORMAT_R8_SINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8A8_SINT},
        {VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16A16_UINT},
        {VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16A16_SINT},
        {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM},
        {VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM},
        {VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM},
        {VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM},
        {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT},
        {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT}};
    /* UNTYPED (10): its 32-bit words, which the shader takes as they are (usse.c) */
    static const VkFormat untyped[4] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT,
                                        VK_FORMAT_R32G32B32A32_UINT};
    if (format == 10 && n >= 1 && n <= 4) return untyped[n - 1];
    return format < 10 && n >= 1 && n <= 4 ? formats[format][n - 1] : VK_FORMAT_UNDEFINED;
}
/* SceGxmBlendFactor and SceGxmBlendFunc as Vulkan's */
static VkBlendFactor blend_factor(unsigned f) {
    static const VkBlendFactor factors[] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_DST_ALPHA /* DST_ALPHA_SATURATE */};
    return f < sizeof factors / sizeof *factors ? factors[f] : VK_BLEND_FACTOR_ONE;
}
static VkBlendOp blend_op(unsigned f) {
    static const VkBlendOp ops[] = {VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT,
                                    VK_BLEND_OP_MIN, VK_BLEND_OP_MAX};
    return f < sizeof ops / sizeof *ops ? ops[f] : VK_BLEND_OP_ADD;
}
/* the pipeline d describes, made now: VK_NULL_HANDLE where the device refuses it */
/* the pipeline d describes, whole or linked from its parts (pipeline_libraries), optimized or fast, linked then the
   parts a fast one took (for its optimized one): VK_NULL_HANDLE where the device refuses it */
static VkPipeline make_pipe(struct VitaRender* r, const PipeDesc* d, bool optimize, VkPipeline linked[]) {
    const VkDevice device = r->vk->device;
    const VkPipelineLayout layout = r->layout;
    VkPipelineShaderStageCreateInfo stages[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                  .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                                  .module = d->vs,
                                                  .pName = "main"},
                                                 {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                  .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                  .module = d->fs,
                                                  .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo input = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                  .vertexBindingDescriptionCount = d->binding_count,
                                                  .pVertexBindingDescriptions = d->bindings,
                                                  .vertexAttributeDescriptionCount = d->attribute_count,
                                                  .pVertexAttributeDescriptions = d->attributes};
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = d->topology};
    VkPipelineViewportStateCreateInfo viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineMultisampleStateCreateInfo multisample = {.sType =
                                                            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendStateCreateInfo blending = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                    .flags = d->blend_flags,
                                                    .attachmentCount = 1,
                                                    .pAttachments = &d->blend};
    VkDynamicState states[5] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK};
    VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                .dynamicStateCount = 5,
                                                .pDynamicStates = states};
    VkPipelineRenderingCreateInfo rendering = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                               .colorAttachmentCount = 1,
                                               .pColorAttachmentFormats = &d->color,
                                               .depthAttachmentFormat = d->depth_format,
                                               .stencilAttachmentFormat = d->stencil_format};
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .pNext = &rendering,
                                       .stageCount = 2,
                                       .pStages = stages,
                                       .pVertexInputState = &input,
                                       .pInputAssemblyState = &assembly,
                                       .pViewportState = &viewport,
                                       .pRasterizationState = &d->raster,
                                       .pMultisampleState = &multisample,
                                       .pDepthStencilState = &d->depth,
                                       .pColorBlendState = &blending,
                                       .pDynamicState = &dynamic,
                                       .layout = layout};
    VkPipeline pipeline = VK_NULL_HANDLE, parts[SNX_VK_GPL_PARTS];
    if (r->vk->gpl.on) {
        VkResult made = snx_vk_gpl_make(&r->parts, device, d->cache, &gi, optimize, linked ? linked : parts, &pipeline);
        if (made != VK_SUCCESS) {
            if (!atomic_exchange_explicit(&r->gpl_failed, 1, memory_order_relaxed))
                fprintf(stderr,
                        "scarletnx-vita: a pipeline from libraries failed (VkResult %d): made whole (said for the "
                        "first)\n",
                        (int)made);
            pipeline = VK_NULL_HANDLE;
            if (linked) linked[0] = VK_NULL_HANDLE;
        } else
            atomic_fetch_add_explicit(optimize ? &r->gpl_optimized : &r->gpl_linked, 1, memory_order_relaxed);
    }
    if (!pipeline && vkCreateGraphicsPipelines(device, d->cache, 1, &gi, NULL, &pipeline) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return pipeline;
}
/* a job's pipeline made, on a worker */
static void job_make(void* owner, unsigned job) {
    struct VitaRender* r = owner;
    PipeDesc* d = &r->jobs[job];
    if (d->linked[0]) {
        if (snx_vk_gpl_optimize(r->vk->device, r->cache, d->linked, r->layout, &d->made) == VK_SUCCESS)
            atomic_fetch_add_explicit(&r->gpl_optimized, 1, memory_order_relaxed);
    } else
        d->made = make_pipe(r, d, true, NULL); /* a worker's: optimized at once */
}
/* a fast link an optimized pipeline replaced: destroyed once the work recorded so far is done (retire) */
static void bury_pipeline(struct VitaRender* r, VkPipeline p) { bury(r, (Buried){.pipeline = p}); }
/* what picks a draw's pipeline: its shaders by their programs (a Shader freed and another made where it was is another
   program's), then the state the pipeline bakes in */
static uint64_t pipeline_key(const struct VitaRender* r, const GxmContext* c, const GxmVertexProgram* vp,
                             const Shader* vs, const GxmFragmentProgram* fp, const Shader* fs,
                             VkPrimitiveTopology topology) {
    uint64_t key = SNX_FNV_BASIS;
    key = snx_fnv_step(key, vs->id), key = snx_fnv_step(key, fs->id),
    key = snx_fnv_step(key, fp->blend_info | (uint64_t)fp->blend << 32);
    key = snx_fnv_step(key, topology), key = snx_fnv_step(key, c->cull_mode), key = snx_fnv_step(key, r->depth != NULL);
    key = snx_fnv_step(key, r->color->vk_format), key = snx_fnv_step(key, r->depth ? r->depth->vk_format : 0);
    key = snx_fnv_step(key, c->depth_func), key = snx_fnv_step(key, c->depth_write);
    bool stencil = r->depth && r->depth->stencil;
    if (stencil)
        key = snx_fnv_step(key, c->stencil_func[0] | (uint64_t)c->stencil_ops[0][0] << 32 |
                                    (uint64_t)c->stencil_ops[0][1] << 40 | (uint64_t)c->stencil_ops[0][2] << 48);
    for (unsigned i = 0; i < vp->stream_count; ++i) key = snx_fnv_step(key, vp->streams[i].stride | (uint64_t)vp->streams[i].index_source << 16);
    for (unsigned i = 0; i < vp->attribute_count; ++i) {
        const GxmAttribute* a = &vp->attributes[i];
        key = snx_fnv_step(key, a->stream | (uint64_t)a->offset << 8 | (uint64_t)a->format << 24 | (uint64_t)a->components << 32 |
                            (uint64_t)a->reg << 40);
    }
    return key;
}
/* The pipeline of a draw. Without async_pipelines it is made now. With it a worker makes a new one while the draws
   that need it are skipped: VK_NULL_HANDLE and *making until it is done; VK_NULL_HANDLE alone where it cannot be */
static VkPipeline pipeline_for(struct VitaRender* r, GxmContext* c, const GxmVertexProgram* vp, Shader* vs,
                               const GxmFragmentProgram* fp, Shader* fs, VkPrimitiveTopology topology, bool* making) {
    const uint64_t key = pipeline_key(r, c, vp, vs, fp, fs, topology);
    const bool stencil = r->depth && r->depth->stencil;
    unsigned slot = (unsigned)(key % PIPELINES);
    *making = false;
    for (unsigned probe = 0; probe < PIPELINES; ++probe, slot = (slot + 1) % PIPELINES) {
        Pipeline* e = &r->pipelines[slot];
        if (e->key == key && e->job) {
            if (!snx_vk_jobs_take(&r->pool, e->job - 1u)) return *making = true, VK_NULL_HANDLE;
            e->pipeline = r->jobs[e->job - 1].made;
            if (e->pipeline) prep_add_pipe(r, key, &r->jobs[e->job - 1]);
            e->job = 0;
            if (!e->pipeline) { /* the device refused it: the slot free again, the draws after it asking again */
                e->key = 0;
                once(r, 6, "the device refuses a pipeline");
            }
            return e->pipeline;
        }
        if (e->key == key && e->pipeline) {
            if (e->better && snx_vk_jobs_take(&r->pool, e->better - 1u)) { /* the optimized one, in the link's place */
                if (r->jobs[e->better - 1].made) {
                    bury_pipeline(r, e->pipeline);
                    e->pipeline = r->jobs[e->better - 1].made;
                    ++r->gpl_replaced;
                }
                r->jobs[e->better - 1] = (PipeDesc){0};
                e->better = 0;
            }
            return e->pipeline;
        }
        if (!e->pipeline && !e->job) break;
    }
    if (r->pipelines[slot].pipeline || r->pipelines[slot].job) {
        once(r, 4, "the pipeline cache is full");
        return VK_NULL_HANDLE;
    }
    if (r->feed.making && prep_listed(r, key)) { /* one of the file's: a worker may have made it since */
        prep_feed(r, false);
        if (r->pipelines[slot].key == key && r->pipelines[slot].pipeline) return r->pipelines[slot].pipeline;
    }
    PipeDesc d = {.vs = vs->module,
                  .fs = fs->module,
                  .topology = topology,
                  .cache = r->cache,
                  .vs_prep = vs->prep,
                  .fs_prep = fs->prep};
    for (unsigned i = 0; i < vp->stream_count; ++i)
        d.bindings[d.binding_count++] = (VkVertexInputBindingDescription){
            i, vp->streams[i].stride,
            vp->streams[i].index_source & 1 ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
    for (unsigned i = 0; i < vp->attribute_count; ++i) {
        int location = vs->meta.attribute_location[i];
        if (location < 0) continue;
        const GxmAttribute* a = &vp->attributes[i];
        VkFormat format = attribute_format(a->format, a->components);
        if (format == VK_FORMAT_UNDEFINED) {
            once_value(r, 5, a->format, "vertex attribute format %u is not drawn yet", a->format);
            return VK_NULL_HANDLE;
        }
        d.attributes[d.attribute_count++] =
            (VkVertexInputAttributeDescription){(uint32_t)location, a->stream, format, a->offset};
    }
    /* SceGxmCullMode: none, clockwise, counter-clockwise; the viewport flips y, so a face's winding is the console's */
    d.raster = (VkPipelineRasterizationStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = r->vk->depth_clamp, /* the console clips no primitive by its depth, it clamps it */
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = c->cull_mode ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE,
        .frontFace = c->cull_mode == 1 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f};
    static const VkCompareOp compares[8] = {VK_COMPARE_OP_NEVER,         VK_COMPARE_OP_LESS,
                                            VK_COMPARE_OP_EQUAL,         VK_COMPARE_OP_LESS_OR_EQUAL,
                                            VK_COMPARE_OP_GREATER,       VK_COMPARE_OP_NOT_EQUAL,
                                            VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
    /* SceGxmStencilOp: keep, zero, replace, increment and decrement (saturating), invert, increment and decrement
       (wrapping); the function in bits 25 to 27, in the depth functions' order. Both faces take the front's, as they do
       without two-sided mode (sceGxmSetTwoSidedEnable, not taken yet); the reference and masks are dynamic */
    static const VkStencilOp ops[8] = {VK_STENCIL_OP_KEEP,
                                       VK_STENCIL_OP_ZERO,
                                       VK_STENCIL_OP_REPLACE,
                                       VK_STENCIL_OP_INCREMENT_AND_CLAMP,
                                       VK_STENCIL_OP_DECREMENT_AND_CLAMP,
                                       VK_STENCIL_OP_INVERT,
                                       VK_STENCIL_OP_INCREMENT_AND_WRAP,
                                       VK_STENCIL_OP_DECREMENT_AND_WRAP};
    VkStencilOpState face = {.failOp = ops[c->stencil_ops[0][0] & 7],
                             .passOp = ops[c->stencil_ops[0][2] & 7],
                             .depthFailOp = ops[c->stencil_ops[0][1] & 7],
                             .compareOp = compares[(c->stencil_func[0] >> 25) & 7]};
    d.depth = (VkPipelineDepthStencilStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = r->depth != NULL,
        .depthWriteEnable = r->depth != NULL && c->depth_write == 0, /* SCE_GXM_DEPTH_WRITE_ENABLED is 0 */
        .depthCompareOp = compares[(c->depth_func >> 22) & 7],
        .stencilTestEnable = stencil,
        .front = face,
        .back = face};
    uint32_t b = fp->blend_info;
    d.blend = (VkPipelineColorBlendAttachmentState){
        .blendEnable = fp->blend,
        .srcColorBlendFactor = blend_factor((b >> 16) & 15),
        .dstColorBlendFactor = blend_factor((b >> 20) & 15),
        .colorBlendOp = blend_op((b >> 8) & 15),
        .srcAlphaBlendFactor = blend_factor((b >> 24) & 15),
        .dstAlphaBlendFactor = blend_factor((b >> 28) & 15),
        .alphaBlendOp = blend_op((b >> 12) & 15),
        /* SceGxmColorMask: A, R, G, B in bits 0 to 3 */
        .colorWriteMask = (b & 2 ? VK_COLOR_COMPONENT_R_BIT : 0) | (b & 4 ? VK_COLOR_COMPONENT_G_BIT : 0) |
                          (b & 8 ? VK_COLOR_COMPONENT_B_BIT : 0) | (b & 1 ? VK_COLOR_COMPONENT_A_BIT : 0)};
    /* a program reading its pixel's color reads what the draw's earlier primitives wrote there, when the device can */
    d.blend_flags = fs->meta.reads_color && r->vk->raster_order
                        ? VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT
                        : 0;
    d.color = r->color->vk_format;
    d.depth_format = r->depth ? r->depth->vk_format : VK_FORMAT_UNDEFINED;
    d.stencil_format = stencil ? r->depth->vk_format : VK_FORMAT_UNDEFINED;
    if (g_settings.async_pipelines && pipe_workers(r)) {
        int job = snx_vk_jobs_free(&r->pool);
        if (job >= 0) { /* with every job in hand the pipeline is asked for again at the next draw */
            r->jobs[job] = d;
            r->pipelines[slot] = (Pipeline){.key = key, .job = (uint8_t)(job + 1)};
            snx_vk_jobs_queue(&r->pool, (unsigned)job);
        }
        *making = true;
        return VK_NULL_HANDLE;
    }
    VkPipeline linked[SNX_VK_GPL_PARTS] = {0};
    VkPipeline pipeline = make_pipe(r, &d, false, linked);
    if (!pipeline) {
        once(r, 6, "the device refuses a pipeline");
        return VK_NULL_HANDLE;
    }
    r->pipelines[slot].key = key, r->pipelines[slot].pipeline = pipeline;
    prep_add_pipe(r, key, &d);
    /* a fast link: its optimized one made on a worker where one is free, which replaces it once made */
    int job = linked[0] && pipe_workers(r) ? snx_vk_jobs_free(&r->pool) : -1;
    if (job >= 0) {
        r->jobs[job] = (PipeDesc){0};
        memcpy(r->jobs[job].linked, linked, sizeof linked);
        r->pipelines[slot].better = (uint8_t)(job + 1);
        snx_vk_jobs_queue(&r->pool, (unsigned)job);
    }
    return pipeline;
}
/* SceGxmTextureFormat's U8U8U8U8 (four bytes, R in the first for ABGR), U8U8U8 (three bytes expanded to RGBA8),
   U8 (one) and UBC1 to UBC3 (BC1 to BC3, 4x4 blocks of 8 or 16 bytes) with their swizzles as an image format and the
   view's mapping: the bytes an element (a texel, or a block) takes, and 4 for a block's side (1 for a texel). False
   for the formats not drawn yet */
static bool texture_format(const VitaVk* v, uint32_t format, VkFormat* vk, unsigned* bytes, unsigned* block,
                           VkComponentMapping* map) {
    unsigned base = format & 0x9f000000u, swizzle = (format >> 12) & 7;
    const VkComponentSwizzle R = VK_COMPONENT_SWIZZLE_R, G = VK_COMPONENT_SWIZZLE_G, B = VK_COMPONENT_SWIZZLE_B,
                             A = VK_COMPONENT_SWIZZLE_A, O = VK_COMPONENT_SWIZZLE_ONE, Z = VK_COMPONENT_SWIZZLE_ZERO;
    static const VkComponentMapping swizzle4[8] = {{R, G, B, A}, {B, G, R, A}, {A, B, G, R}, {G, B, A, R},
                                                   {R, G, B, O}, {B, G, R, O}, {A, B, G, O}, {G, B, A, O}};
    *block = 1;
    if (base == 0x0c000000u) { /* the bytes read as R, G, B, A: ABGR has them in that order */
        *vk = VK_FORMAT_R8G8B8A8_UNORM, *bytes = 4, *map = swizzle4[swizzle];
        return true;
    }
    if (base == 0x98000000u && swizzle < 2) { /* three-byte RGB with the swizzle's implied alpha of one */
        *vk = VK_FORMAT_R8G8B8A8_UNORM, *bytes = 3;
        *map = swizzle == 0 ? (VkComponentMapping){R, G, B, O} : (VkComponentMapping){B, G, R, O};
        return true;
    }
    if (base == 0x1b000000u) { /* F16F16F16F16: a half-float color surface sampled */
        *vk = VK_FORMAT_R16G16B16A16_SFLOAT, *bytes = 8, *map = swizzle4[swizzle];
        return true;
    }
    if (base >= 0x85000000u && base <= 0x87000000u && v->bc) { /* UBC1, UBC2, UBC3: the blocks decode to R, G, B, A */
        static const VkFormat formats[3] = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK,
                                            VK_FORMAT_BC3_UNORM_BLOCK};
        *vk = formats[(base - 0x85000000u) >> 24], *bytes = base == 0x85000000u ? 8 : 16, *block = 4;
        *map = swizzle4[swizzle];
        return true;
    }
    if (base == 0x00000000u || base == 0x09000000u) { /* U8 and U16, SWIZZLE1: R, 000R, 111R, RRRR, 0RRR, 1RRR,
                                                           R000, R111 (the letters A, B, G, R) */
        static const VkComponentMapping maps[8] = {{R, Z, Z, O}, {R, Z, Z, Z}, {R, O, O, O}, {R, R, R, R},
                                                   {R, R, R, Z}, {R, R, R, O}, {Z, Z, Z, R}, {O, O, O, R}};
        *vk = base ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM, *bytes = base ? 2 : 1, *map = maps[swizzle];
        return true;
    }
    return false;
}
/* push constants of size bytes (at most 128, which every device takes), all 32-bit words: the device reads them as it
   reads memory, little-endian, so a big-endian host swaps them */
static void push_constants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, const void* data,
                           uint32_t size) {
    uint32_t words[32];
    memcpy(words, data, size);
    snx_le32_array(words, size / 4);
    vkCmdPushConstants(cb, layout, stages, 0, size, words);
}
/* One of the compute shaders on source (from base, which the storage buffer alignment allows) into destination's
   [at, at + size), with its push constants and count invocations, between barriers: the writes before it (the
   guest's, the GPU's) and the reads after. False without a descriptor set */
static bool compute(struct VitaRender* r, VkCommandBuffer cb, VkPipeline pipeline, VkBuffer source, VkDeviceSize base,
                    VkBuffer destination, VkDeviceSize at, VkDeviceSize size, const void* params, uint32_t params_size,
                    uint32_t count) {
    VkDescriptorSet set = vita_vk_set(r->swizzle_set);
    if (!set) return false;
    VkDescriptorBufferInfo infos[2] = {{source, base, VK_WHOLE_SIZE}, {destination, at, size}};
    snx_vk_storage_buffers(r->vk->device, set, infos, 2);
    memory_barrier(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, r->swizzle_layout, 0, 1, &set, 0, NULL);
    push_constants(cb, r->swizzle_layout, VK_SHADER_STAGE_COMPUTE_BIT, params, params_size);
    vkCmdDispatch(cb, (count + 63) / 64, 1, 1);
    memory_barrier(cb);
    return true;
}
static VkDeviceSize storage_align(const struct VitaRender* r) {
    VkDeviceSize align = r->vk->properties.limits.minStorageBufferOffsetAlignment;
    return align < 16 ? 16 : align;
}
/* Temporary rows in the slot's ring. Callers restore used after recording consumers; barriers order reuse. */
static bool linear_room(struct VitaRender* r, VkDeviceSize size, VkDeviceSize* at) {
    unsigned slot = vita_vk_slot();
    VkDeviceSize align = storage_align(r);
    *at = (r->linear[slot].used + align - 1) / align * align;
    if (*at + size > LINEAR) return false;
    r->linear[slot].used = *at + size;
    return true;
}
/* A swizzled or tiled texture's columns x rows elements of bytes each, at offset in the imported buffer, made linear
   rows on the GPU (swizzle.comp) in the slot's ring: where they are, ready to be copied. False without room */
static bool deswizzle(struct VitaRender* r, VkCommandBuffer cb, VkBuffer source, VkDeviceSize offset, bool tiled,
                      uint32_t columns, uint32_t rows, unsigned bytes, VkBuffer* out, VkDeviceSize* out_offset) {
    uint32_t elements = columns * rows, words = (uint32_t)(((uint64_t)elements * bytes + 3) / 4);
    VkDeviceSize at, size = (VkDeviceSize)words * 4, align = storage_align(r), base = offset / align * align;
    if ((bytes != 3 && (offset & 3)) || !linear_room(r, size, &at)) return false;
    unsigned es = bytes == 3 ? 5 : 0, wl = 0, k = 0;
    while (bytes != 3 && (1u << es) < bytes) ++es;
    while ((1u << wl) < columns) ++wl;
    uint32_t smaller = columns < rows ? columns : rows;
    while ((1u << k) < smaller) ++k;
    SwizzleParams params = {words, elements, (uint32_t)((offset - base) / 4), (uint32_t)((offset - base) & 3),
                            es, columns, wl, k, tiled, (columns + 31) >> 5, 0, 0};
    VkBuffer ring = r->linear[vita_vk_slot()].buffer;
    if (!compute(r, cb, r->swizzle, source, base, ring, at, size, &params, sizeof params, words)) return false;
    *out = ring, *out_offset = at;
    return true;
}
/* the words a swizzled or tiled 32-bit color surface's memory takes: a tiled one's whole tiles */
static uint32_t laid_out_words(const Surface* s) {
    return s->type == GXM_COLOR_SURFACE_TILED ? (((s->width + 31) >> 5) * ((s->height + 31) >> 5)) << 10
                                              : s->width * s->height;
}
/* A swizzled or tiled 32-bit color surface's memory into its image, or its image into the memory (store): rows in the
   slot's ring between them, made from the memory or laid out into it by swizzle.comp. A game renders one and reads it
   as a swizzled texture (one draws its character's pencil sketch so). False without room, or past the import */
static bool laid_out_copy(struct VitaRender* r, VkCommandBuffer cb, const Surface* s, const Import* m, VkDeviceSize at,
                          bool store) {
    unsigned slot = vita_vk_slot();
    VkDeviceSize used = r->linear[slot].used;
    bool tiled = s->type == GXM_COLOR_SURFACE_TILED;
    VkBufferImageCopy copy = {.bufferRowLength = s->width,
                              .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {s->width, s->height, 1}};
    if (!store) {
        VkBuffer rows;
        if (!deswizzle(r, cb, m->buffer, at, tiled, s->width, s->height, 4, &rows, &copy.bufferOffset)) {
            r->linear[slot].used = used;
            return false;
        }
        vkCmdCopyBufferToImage(cb, rows, s->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        r->linear[slot].used = used;
        return true;
    }
    uint32_t words = s->width * s->height, wl = 0, k = 0, smaller = s->width < s->height ? s->width : s->height;
    while ((1u << wl) < s->width) ++wl;
    while ((1u << k) < smaller) ++k;
    /* the memory's words from an aligned base */
    VkDeviceSize rows, align = storage_align(r), base = at / align * align;
    VkDeviceSize size = at - base + (VkDeviceSize)laid_out_words(s) * 4;
    if ((at & 3) || base + size > m->size || !linear_room(r, (VkDeviceSize)words * 4, &rows)) {
        r->linear[slot].used = used;
        return false;
    }
    VkBuffer ring = r->linear[slot].buffer;
    copy.bufferOffset = rows;
    vkCmdCopyImageToBuffer(cb, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ring, 1, &copy);
    SwizzleParams params = {words, words, 0, 0, 2, s->width, wl, k, tiled, (s->width + 31) >> 5, 1,
                            (uint32_t)((at - base) / 4)};
    bool ok = compute(r, cb, r->swizzle, ring, rows, m->buffer, base, size, &params, sizeof params, words);
    r->linear[slot].used = used;
    return ok;
}
/* whether a color surface's memory is laid out by swizzle.comp: 32-bit pixels in tiles, or in Morton order on sides
   that are powers of two */
static bool color_laid_out(const Surface* s, unsigned bytes) {
    bool pow2 = !(s->width & (s->width - 1)) && !(s->height & (s->height - 1));
    return bytes == 4 && (s->type == GXM_COLOR_SURFACE_TILED || (s->type != GXM_COLOR_SURFACE_LINEAR && pow2));
}
/* A color surface's memory (at in m) into its image (in TRANSFER_DST), or the image (in TRANSFER_SRC) into its memory
   (store). A swizzled or tiled one is laid out on the GPU (laid_out_copy). A 24-bit surface goes through R8G8B8A8 rows in the slot's ring, made from its memory or made its memory by
   rgb24.comp, which keeps the bytes of the rows' padding. False when the ring has no room for them */
static bool color_copy(struct VitaRender* r, VkCommandBuffer cb, const Surface* s, const Import* m, VkDeviceSize at,
                       bool store) {
    VkFormat vk;
    unsigned bytes;
    if (!color_format(s->format, &vk, &bytes)) return false;
    /* a store after every earlier access to the memory: the surface's own load, the presenter's copy of it, a texture
       read from it (the image's barrier orders only the image's accesses for the validation layer) */
    if (store) memory_barrier(cb);
    if (s->type != GXM_COLOR_SURFACE_LINEAR) {
        if (color_laid_out(s, bytes)) return laid_out_copy(r, cb, s, m, at, store);
        once_value(r, 22, ((uint64_t)s->type << 32) | bytes,
                   "a color surface of type 0x%08" PRIx32 " and %u-byte pixels is drawn as rows", s->type, bytes);
    }
    VkBufferImageCopy copy = {.bufferOffset = at,
                              .bufferRowLength = s->stride,
                              .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {s->width, s->height, 1}};
    if (bytes != 3) {
        if (store)
            vkCmdCopyImageToBuffer(cb, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m->buffer, 1, &copy);
        else
            vkCmdCopyBufferToImage(cb, m->buffer, s->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        return true;
    }
    uint32_t pixels = s->width * s->height;
    VkDeviceSize rows, size = (VkDeviceSize)pixels * 4, align = storage_align(r), base = at / align * align;
    /* the words from base to the surface's end, inside the import (an allocation's pages) */
    uint32_t words = (uint32_t)((at - base + (VkDeviceSize)s->stride * 3 * s->height + 3) / 4);
    unsigned slot = vita_vk_slot();
    VkDeviceSize used = r->linear[slot].used;
    if (base + (VkDeviceSize)words * 4 > m->size || !linear_room(r, size, &rows)) {
        once_value(r, 18, ((uint64_t)s->width << 32) | s->height,
                   "a 24-bit color surface of %ux%u is not drawn: no room for its rows", (unsigned)s->width,
                   (unsigned)s->height);
        return false;
    }
    VkBuffer ring = r->linear[slot].buffer;
    bool bgr = (s->format & GXM_COLOR_SWIZZLE3_RGB) != 0;
    copy.bufferOffset = rows, copy.bufferRowLength = s->width;
    bool ok;
    if (!store) {
        Rgb24Params params = {pixels, (uint32_t)(at - base), 0, s->width, s->height, s->stride * 3, 0, bgr};
        ok = compute(r, cb, r->rgb24, m->buffer, base, ring, rows, size, &params, sizeof params, pixels);
        if (ok) vkCmdCopyBufferToImage(cb, ring, s->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    } else {
        vkCmdCopyImageToBuffer(cb, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ring, 1, &copy);
        Rgb24Params params = {words, 0, (uint32_t)(at - base), s->width, s->height, s->stride, 1, bgr};
        ok = compute(r, cb, r->rgb24, ring, rows, m->buffer, base, (VkDeviceSize)words * 4, &params, sizeof params,
                     words);
    }
    r->linear[slot].used = used;
    return ok;
}
/* a color surface's store (color_copy) recorded: its rows, or its whole memory where they are not rows */
static void color_written(struct VitaRender* r, const Import* m, const Surface* s) {
    VkFormat vk;
    unsigned bytes;
    if (!color_format(s->format, &vk, &bytes)) return;
    uint32_t stride = s->stride * bytes, whole = laid_out_words(s) * 4;
    if (color_laid_out(s, bytes))
        mirror_written(r, m, s->address, whole, whole, 1);
    else
        mirror_written(r, m, s->address, s->width * bytes, stride, s->height);
}
/* A YUV420 texture (format base 0x90000000 P2, 0x91000000 P3; its swizzle YUV or YVU, CSC0 or CSC1) at offset in the
   imported buffer, its Y rows stride bytes and its chroma after them, made R8G8B8A8 rows on the GPU (yuv.comp) in the
   slot's ring. CSC0 is taken for BT.601 and CSC1 for BT.709, both limited range: SceGxm's names say no more */
static bool yuv_to_rgba(struct VitaRender* r, VkCommandBuffer cb, VkBuffer source, VkDeviceSize offset, uint32_t format,
                        uint32_t width, uint32_t height, uint32_t stride, VkBuffer* out, VkDeviceSize* out_offset) {
    static const uint32_t matrices[2][4] = {{409, 100, 208, 516}, {459, 55, 136, 541}}; /* rv, gu, gv, bu */
    unsigned swizzle = (format >> 12) & 7;
    const uint32_t* m = matrices[(swizzle >> 1) & 1];
    uint32_t pixels = width * height;
    VkDeviceSize at, size = (VkDeviceSize)pixels * 4, align = storage_align(r), base = offset / align * align;
    if (!linear_room(r, size, &at)) return false;
    YuvParams params = {pixels, width, height, (uint32_t)(offset - base), (uint32_t)(offset - base) + stride * height,
                        stride, (format & 0x9f000000u) == 0x90000000u, swizzle & 1, m[0], m[1], m[2], m[3]};
    VkBuffer ring = r->linear[vita_vk_slot()].buffer;
    if (!compute(r, cb, r->yuv, source, base, ring, at, size, &params, sizeof params, pixels)) return false;
    *out = ring, *out_offset = at;
    return true;
}
/* A three-byte texture level into an RGBA8 row buffer (rgb24.comp): its source stride is bytes, its alpha is one */
static bool rgb24_to_rgba(struct VitaRender* r, VkCommandBuffer cb, VkBuffer source, VkDeviceSize offset,
                          uint32_t width, uint32_t height, uint32_t stride, VkBuffer* out, VkDeviceSize* out_offset) {
    uint32_t pixels = width * height;
    VkDeviceSize at, size = (VkDeviceSize)pixels * 4, align = storage_align(r), base = offset / align * align;
    if (!linear_room(r, size, &at)) return false;
    Rgb24Params params = {pixels, (uint32_t)(offset - base), 0, width, height, stride, 0, 0};
    VkBuffer ring = r->linear[vita_vk_slot()].buffer;
    if (!compute(r, cb, r->rgb24, source, base, ring, at, size, &params, sizeof params, pixels)) return false;
    *out = ring, *out_offset = at;
    return true;
}
/* The scene's rendering ended for work that can't run inside it (copies, dispatches), and begun again after it on
   what it drew so far */
static void rendering_pause(VkCommandBuffer cb) { vkCmdEndRendering(cb); }
static void rendering_resume(struct VitaRender* r, VkCommandBuffer cb) {
    VkRenderingAttachmentInfo color = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                       .imageView = drawn_view(r->color),
                                       .imageLayout = color_layout(r),
                                       .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                       .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingAttachmentInfo depth, stencil;
    if (r->depth) depth_attachments(r, NULL, true, &depth, &stencil);
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                          .renderArea = {{0, 0}, {r->width * r->drawn, r->height * r->drawn}},
                          .layerCount = 1,
                          .colorAttachmentCount = 1,
                          .pColorAttachments = &color,
                          .pDepthAttachment = r->depth ? &depth : NULL,
                          .pStencilAttachment = r->depth && r->depth->stencil ? &stencil : NULL};
    vkCmdBeginRendering(cb, &ri);
    r->bound = (Bound){0}; /* cb may be a new command buffer, and the work between did bind things of its own */
}
static uint32_t round_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
static uint32_t power_of_two(uint32_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}
/* One mip level of a texture as the guest lays it out (Vita3K's renderer/src/texture/cache.cpp): its texel size, its
   elements (texels, or blocks) a row and a column, the bytes from one of its rows to the next once linear, and where
   it starts from the texture's data. The levels follow each other, each taking its size rounded up to powers of two
   and to the layout's alignment (8 texels wide for linear rows, 32x32 for tiles, a block for a compressed format) */
typedef struct {
    uint32_t width, height, columns, rows, stride, offset, size;
} Level;
static unsigned texture_levels(const GxmTexture* x, uint32_t type, unsigned bytes, unsigned block, Level* out,
                               unsigned most) {
    uint32_t width = gxm_texture_width(x), height = gxm_texture_height(x), smaller = width < height ? width : height;
    unsigned count = gxm_texture_mip_count(x), fit = 0;
    while (smaller >> fit) ++fit; /* the levels down to a texel on the smaller side */
    if (count > fit) count = fit;
    if (count > most) count = most;
    bool linear = type == GXM_TEXTURE_LINEAR || type == GXM_TEXTURE_LINEAR_STRIDED;
    uint32_t align_w = type == GXM_TEXTURE_LINEAR && block < 8 ? 8 : type == GXM_TEXTURE_TILED ? 32 : block;
    uint32_t align_h = type == GXM_TEXTURE_TILED ? 32 : block;
    /* a single level of linear rows takes its own size, not powers of two */
    bool tight = linear && gxm_bits(x->w[0], 17, 4) == 15;
    uint32_t lw = tight ? width : power_of_two(width), lh = tight ? height : power_of_two(height), offset = 0;
    for (unsigned i = 0; i < count; ++i) {
        Level* l = &out[i];
        l->width = width, l->height = height;
        l->columns = (width + block - 1) / block, l->rows = (height + block - 1) / block;
        l->stride = type == GXM_TEXTURE_LINEAR_STRIDED ? gxm_texture_stride(x)
                    : type == GXM_TEXTURE_LINEAR       ? round_up(width, align_w) / block * bytes
                                                       : l->columns * bytes;
        l->offset = offset;
        /* what the level's bytes span: a tiled one whole tiles, a strided one its rows */
        l->size = type == GXM_TEXTURE_TILED ? (((l->columns + 31) >> 5) * ((l->rows + 31) >> 5) << 10) * bytes
                  : linear                  ? l->stride * l->rows
                                            : l->columns * l->rows * bytes;
        offset += round_up(lw, align_w) / block * (round_up(lh, align_h) / block) * bytes;
        width = width > 1 ? width / 2 : 1, height = height > 1 ? height / 2 : 1;
        lw = lw > 1 ? lw / 2 : 1, lh = lh > 1 ? lh / 2 : 1;
    }
    return count;
}
/* Vita cube faces contain the whole mip chain, with the next face aligned independently of how many mips are sampled. */
static uint32_t cube_face_stride(const GxmTexture* x, unsigned bytes, unsigned block) {
    uint32_t width = power_of_two(gxm_texture_width(x)), height = power_of_two(gxm_texture_height(x));
    uint32_t offset = 0;
    unsigned raw_mips = gxm_bits(x->w[0], 17, 4);
    do {
        offset += round_up(width, block) / block * (round_up(height, block) / block) * bytes;
        if (raw_mips == 15) break;
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
    } while (width > 1 || height > 1);
    uint32_t face_alignment = 4;
    if (raw_mips != 15) {
        unsigned bpp = bytes * 8 / (block * block);
        bool align_2k = (gxm_texture_width(x) >= 32 && gxm_texture_height(x) >= 32 && (bpp <= 8 || block > 1)) ||
                        (gxm_texture_width(x) >= 16 && gxm_texture_height(x) >= 16 && (bpp == 16 || bpp == 32)) ||
                        (gxm_texture_width(x) >= 8 && gxm_texture_height(x) >= 8 && bpp == 64);
        if (align_2k) face_alignment = 2048;
    }
    return round_up(offset, face_alignment);
}
#define LEVELS 13u /* 4096 texels on a side */
/* whether [address, address + size) overlaps the color surface's memory */
static bool overlaps_color(const struct VitaRender* r, uint32_t address, uint32_t size) {
    VkFormat vk;
    unsigned bytes;
    if (!r->color || !color_format(r->color->format, &vk, &bytes)) return false;
    uint64_t start = r->color->address, end = start + (uint64_t)r->color->stride * bytes * r->color->height;
    return address < end && (uint64_t)address + size > start;
}
/* the color image into the surface's memory, between barriers, its layout as it was (rendering paused) */
static void color_write_back(struct VitaRender* r, VitaProcess* p, VkCommandBuffer cb) {
    Surface* s = r->color;
    VkFormat vk;
    unsigned bytes;
    VkDeviceSize at;
    const Import* m = color_format(s->format, &vk, &bytes)
                          ? buffer_of(r, p, s->address, s->stride * bytes * s->height, &at)
                          : NULL;
    if (!m) return;
    bool stored;
    if (s->big) { /* the image is the memory's, made from the larger one drawn to */
        color_down(r, cb, s);
        stored = color_copy(r, cb, s, m, at, true);
    } else {
        barrier(cb, s->image, VK_IMAGE_ASPECT_COLOR_BIT, s->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        stored = color_copy(r, cb, s, m, at, true);
        barrier(cb, s->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->layout);
    }
    memory_barrier(cb);
    if (stored) color_written(r, m, s);
    gpu_wrote(r, s->address, s->stride * bytes * s->height); /* the scene's later stores write it again */
}
bool vita_render_transfer_copy(VitaProcess* p, uint32_t width, uint32_t height, uint32_t bytes_per_pixel,
                               uint32_t src_address, uint32_t src_x, uint32_t src_y, int32_t src_stride,
                               uint32_t dst_address, uint32_t dst_x, uint32_t dst_y, int32_t dst_stride) {
    struct VitaRender* r = render_of(p);
    if (!r || !width || !height) return true;
    if (!bytes_per_pixel || src_stride <= 0 || dst_stride <= 0) return false;

    uint64_t row_bytes = (uint64_t)width * bytes_per_pixel;
    uint64_t src_row_end = ((uint64_t)src_x + width) * bytes_per_pixel;
    uint64_t dst_row_end = ((uint64_t)dst_x + width) * bytes_per_pixel;
    uint64_t src_start = (uint64_t)src_y * (uint32_t)src_stride + (uint64_t)src_x * bytes_per_pixel;
    uint64_t dst_start = (uint64_t)dst_y * (uint32_t)dst_stride + (uint64_t)dst_x * bytes_per_pixel;
    uint64_t src_span = (uint64_t)(height - 1) * (uint32_t)src_stride + row_bytes;
    uint64_t dst_span = (uint64_t)(height - 1) * (uint32_t)dst_stride + row_bytes;
    uint64_t src_begin = (uint64_t)src_address + src_start;
    uint64_t dst_begin = (uint64_t)dst_address + dst_start;
    if (row_bytes > UINT32_MAX || src_row_end > (uint32_t)src_stride || dst_row_end > (uint32_t)dst_stride ||
        src_begin > UINT32_MAX || dst_begin > UINT32_MAX || src_span > UINT32_MAX || dst_span > UINT32_MAX ||
        ((src_begin | dst_begin | row_bytes | (uint32_t)src_stride | (uint32_t)dst_stride) & 3))
        return false;

    VkDeviceSize src_at, dst_at;
    const Import* src = buffer_of(r, p, (uint32_t)src_begin, (uint32_t)src_span, &src_at);
    const Import* dst = buffer_of(r, p, (uint32_t)dst_begin, (uint32_t)dst_span, &dst_at);
    if (!src || !dst) return false;

    bool same = src->buffer == dst->buffer;
    bool overlap = same && src_at < dst_at + dst_span && dst_at < src_at + src_span;
    VkCommandBuffer cb = vita_vk_commands();
    bool rendering = r->rendering;
    if (rendering) {
        rendering_pause(cb);
        if (overlaps_color(r, (uint32_t)src_begin, (uint32_t)src_span)) color_write_back(r, p, cb);
    }
    memory_barrier(cb);

    if (overlap) {
        if (row_bytes > LINEAR / height) {
            if (rendering) rendering_resume(r, cb);
            return false;
        }
        unsigned slot = vita_vk_slot();
        VkDeviceSize old_used = r->linear[slot].used, scratch_at;
        if (!linear_room(r, row_bytes * height, &scratch_at)) {
            r->linear[slot].used = old_used;
            if (rendering) rendering_resume(r, cb);
            return false;
        }
        VkBuffer scratch = r->linear[slot].buffer;
        for (uint32_t y = 0; y < height; ++y) {
            VkBufferCopy copy = {.srcOffset = src_at + (VkDeviceSize)y * (uint32_t)src_stride,
                                 .dstOffset = scratch_at + (VkDeviceSize)y * row_bytes,
                                 .size = row_bytes};
            vkCmdCopyBuffer(cb, src->buffer, scratch, 1, &copy);
        }
        memory_barrier(cb);
        for (uint32_t y = 0; y < height; ++y) {
            VkBufferCopy copy = {.srcOffset = scratch_at + (VkDeviceSize)y * row_bytes,
                                 .dstOffset = dst_at + (VkDeviceSize)y * (uint32_t)dst_stride,
                                 .size = row_bytes};
            vkCmdCopyBuffer(cb, scratch, dst->buffer, 1, &copy);
        }
        memory_barrier(cb);
        r->linear[slot].used = old_used;
    } else if (src_stride == (int32_t)row_bytes && dst_stride == (int32_t)row_bytes) {
        VkBufferCopy copy = {.srcOffset = src_at, .dstOffset = dst_at, .size = row_bytes * height};
        vkCmdCopyBuffer(cb, src->buffer, dst->buffer, 1, &copy);
    } else {
        for (uint32_t y = 0; y < height; ++y) {
            VkBufferCopy copy = {.srcOffset = src_at + (VkDeviceSize)y * (uint32_t)src_stride,
                                 .dstOffset = dst_at + (VkDeviceSize)y * (uint32_t)dst_stride,
                                 .size = row_bytes};
            vkCmdCopyBuffer(cb, src->buffer, dst->buffer, 1, &copy);
        }
    }
    memory_barrier(cb);
    mirror_written(r, dst, (uint32_t)dst_begin, (uint32_t)row_bytes, (uint32_t)dst_stride, height);
    gpu_wrote(r, (uint32_t)dst_begin, (uint32_t)dst_span);
    if (rendering) rendering_resume(r, cb);
    return true;
}
/* the color surface whose image holds the memory a texture reads as it is now: the same address, size and format,
   stored to its memory by a scene that ended, and the memory not written since */
static const Surface* surface_holding(const struct VitaRender* r, uint32_t address, uint32_t width, uint32_t height,
                                      VkFormat vk, uint32_t stride) {
    for (unsigned i = 0; i < SURFACES; ++i) {
        const Surface* s = &r->surfaces[i];
        VkFormat format;
        unsigned bytes;
        if (!s->image || s->depth || s == r->color || !s->synced || s->type != GXM_COLOR_SURFACE_LINEAR ||
            s->address != address || s->width != width || s->height != height || s->vk_format != vk ||
            !color_format(s->format, &format, &bytes) || s->stride * bytes != stride)
            continue;
        if (!vita_watch_changed(s->address, s->stride * bytes * s->height, s->since)) return s;
    }
    return NULL;
}
/* t's image at scale (a copy of a surface drawn larger, or 1 for its memory's), made again where it has another: the
   old one kept until the work recorded with it is done. False when the device makes none */
static bool texture_scaled(struct VitaRender* r, Texture* t, VkFormat vk, VkComponentMapping map, bool cube,
                           unsigned levels, uint32_t scale) {
    if (t->scale == scale) return true;
    bury(r, (Buried){.image = t->image, .view = t->view, .memory = t->memory});
    t->image = VK_NULL_HANDLE, t->view = VK_NULL_HANDLE, t->memory = VK_NULL_HANDLE;
    r->bound = (Bound){0}; /* its set may hold the view that went */
    if (!image_make(r, vk, t->width * scale, t->height * scale, levels,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, map, cube,
                    &t->image, &t->view, &t->memory)) {
        *t = (Texture){0};
        return false;
    }
    t->scale = scale, t->layout = VK_IMAGE_LAYOUT_UNDEFINED, t->synced = false;
    return true;
}
static Texture* texture_for(struct VitaRender* r, VitaProcess* p, VkCommandBuffer cb, const GxmTexture* x) {
    uint32_t type = gxm_texture_type(x), format = gxm_texture_format(x), data = gxm_texture_data(x);
    uint32_t width = gxm_texture_width(x), height = gxm_texture_height(x);
    VkFormat vk;
    unsigned bytes, block;
    VkComponentMapping map;
    bool linear = type == GXM_TEXTURE_LINEAR || type == GXM_TEXTURE_LINEAR_STRIDED;
    bool cube = type == GXM_TEXTURE_CUBE;
    if (!linear && type != GXM_TEXTURE_SWIZZLED && type != GXM_TEXTURE_TILED && !cube) {
        once_value(r, 7, ((uint64_t)type << 32) | format,
                   "texture layout %u (format 0x%08" PRIx32 ", %ux%u) is not drawn yet", type, format, (unsigned)width,
                   (unsigned)height);
        return NULL;
    }
    if (cube && width != height) {
        once_value(r, 18, data, "cube texture at 0x%08" PRIx32 " is not square", data);
        return NULL;
    }
    /* a YUV420 one (a movie's frames) is made RGBA on the GPU: its Y bytes are the elements of a level of its own,
       the chroma after them */
    bool yuv = (format & 0x9e000000u) == 0x90000000u;
    bool rgb24 = (format & 0x9f000000u) == 0x98000000u;
    if (yuv && type == GXM_TEXTURE_LINEAR) {
        const VkComponentSwizzle I = VK_COMPONENT_SWIZZLE_IDENTITY;
        vk = VK_FORMAT_R8G8B8A8_UNORM, bytes = 1, block = 1, map = (VkComponentMapping){I, I, I, I};
    } else if (yuv) {
        once_value(r, 17, type, "a YUV texture of layout %u is not drawn yet", type);
        return NULL;
    } else if (!texture_format(r->vk, format, &vk, &bytes, &block, &map)) {
        once_value(r, 8, format, "texture format 0x%08" PRIx32 " is not drawn yet", format);
        return NULL;
    }
    Level levels[LEVELS];
    unsigned count = texture_levels(x, type, bytes, block, levels, yuv ? 1 : LEVELS);
    if (yuv) levels[0].size = levels[0].stride * levels[0].rows * 3 / 2;
    uint32_t face_stride = cube ? cube_face_stride(x, bytes, block) : 0;
    uint32_t stride = levels[0].stride;
    Texture *t = NULL, *free_slot = NULL;
    for (unsigned i = 0; i < TEXTURES && !t; ++i) {
        Texture* c = &r->textures[i];
        if (c->image && c->address == data && c->format == format && c->width == width && c->height == height &&
            c->stride == stride && c->type == type && c->levels == count)
            t = c;
        else if (!c->image && !free_slot)
            free_slot = c;
    }
    if (!t) {
        if (!free_slot) { /* the oldest goes */
            free_slot = &r->textures[0];
            for (unsigned i = 1; i < TEXTURES; ++i)
                if (r->textures[i].used < free_slot->used) free_slot = &r->textures[i];
            vita_vk_wait(r->vk->ring.submitted);
            r->bound = (Bound){0}; /* its set may hold the view that goes */
            vkDestroyImageView(r->vk->device, free_slot->view, NULL);
            vkDestroyImage(r->vk->device, free_slot->image, NULL);
            vkFreeMemory(r->vk->device, free_slot->memory, NULL);
            *free_slot = (Texture){0};
        }
        t = free_slot;
        if (!image_make(r, vk, width, height, count, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, map, cube, &t->image, &t->view, &t->memory))
            return NULL;
        t->address = data, t->format = format, t->width = width, t->height = height, t->stride = stride;
        t->type = type, t->levels = count;
        t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        t->snapshot_scene = t->snapshot_draws = 0;
        t->scale = 1;
    }
    /* Its memory again once a scene, the CPU may have changed it; and at each draw while it overlaps the color surface
       being drawn, with what the scene drew so far written back first. The console's GPU draws a scene a tile at a
       time and writes each tile back as it finishes it, so a draw reads what the scene drew in the tiles before: a
       game reads a pass's result back in the same scene (one blurs a region another pass of the scene drew), from
       memory it may have used for something else before the scene began */
    const Level* last = &levels[count - 1];
    uint64_t span64 = (cube ? (uint64_t)face_stride * 5 : 0) + last->offset + last->size;
    if (span64 > UINT32_MAX) {
        once_value(r, 19, data, "texture at 0x%08" PRIx32 " is too large", data);
        return NULL;
    }
    uint32_t span = (uint32_t)span64;
    bool drawn_over = r->rendering && overlaps_color(r, data, span);
    /* The texture is the color surface being drawn, whole and in the same format (a game blurs or filters a surface
       by drawing from it onto itself): the image copied to the texture's on the GPU, where the memory's round trip
       would write the surface back to guest memory and read it again for each draw. Its memory is not written here;
       the scene's end does that. The copy is once for the draws between two draws */
    if (drawn_over && type == GXM_TEXTURE_LINEAR && !cube && !yuv && !rgb24 && count == 1 &&
        r->color->type == GXM_COLOR_SURFACE_LINEAR && data == r->color->address && width == r->color->width &&
        height == r->color->height && vk == r->color->vk_format && stride == r->color->stride * bytes) {
        if (t->snapshot_scene != r->scene || t->snapshot_draws != r->draws) {
            /* drawn larger, the larger image whole, the texture as large */
            const Surface* s = r->color;
            VkImage from = s->big ? s->big : s->image;
            VkImageLayout layout = s->big ? s->big_layout : s->layout;
            if (!texture_scaled(r, t, vk, map, false, 1, r->drawn)) return NULL;
            VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .extent = {width * r->drawn, height * r->drawn, 1}};
            rendering_pause(cb);
            barrier(cb, from, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            barrier(cb, t->image, VK_IMAGE_ASPECT_COLOR_BIT, t->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdCopyImage(cb, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            barrier(cb, from, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout);
            barrier(cb, t->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            rendering_resume(r, cb);
            t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            t->snapshot_scene = r->scene, t->snapshot_draws = r->draws;
        }
        t->synced = false; /* it holds the surface, not its memory: the next scene reads the memory again */
        t->clean = 0;
        t->used = r->scene;
        return t;
    }
    /* read before the pages are, so a write that lands meanwhile makes the next draw look again */
    uint64_t epoch = vita_watch_epoch() + 1;
    if (!t->synced || drawn_over || (t->clean != epoch && vita_watch_changed(data, span, t->since))) {
        t->clean = 0;
        /* A scene's result read by a later scene (a game's passes draw each from the one before): the surface's image
           copied to the texture's on the GPU, where the memory's copy would read back what the scene's end just wrote */
        const Surface* source = !drawn_over && type == GXM_TEXTURE_LINEAR && !cube && !yuv && !rgb24 && count == 1
                                  ? surface_holding(r, data, width, height, vk, stride)
                                  : NULL;
        if (source) {
            /* drawn larger, its larger image, which its store made the memory from */
            uint32_t k = source->big ? r->scale : 1;
            VkImage from = source->big ? source->big : source->image;
            VkImageLayout layout = source->big ? source->big_layout : source->layout;
            if (!texture_scaled(r, t, vk, map, false, 1, k)) return NULL;
            VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .extent = {width * k, height * k, 1}};
            rendering_pause(cb);
            barrier(cb, from, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            barrier(cb, t->image, VK_IMAGE_ASPECT_COLOR_BIT, t->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdCopyImage(cb, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            barrier(cb, from, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout);
            barrier(cb, t->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            rendering_resume(r, cb);
            t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            t->synced = true;
            t->since = source->since; /* the memory it holds is the surface's, unwritten since its store */
            t->used = r->scene;
            return t;
        }
        VkDeviceSize at;
        const Import* m = buffer_of(r, p, data, span, &at);
        if (!m) {
            once_value(r, 9, data, "texture memory at 0x%08" PRIx32 " is not mapped for the GPU", data);
            return NULL;
        }
        if (!texture_scaled(r, t, vk, map, cube, count, 1)) return NULL; /* its memory's size again */
        rendering_pause(cb);
        if (drawn_over) color_write_back(r, p, cb);
        /* each level from the guest's bytes, a swizzled or tiled one made linear rows first */
        VkBufferImageCopy copies[LEVELS * 6];
        VkBuffer from[LEVELS * 6];
        bool complete = true;
        unsigned slot = vita_vk_slot();
        VkDeviceSize used = r->linear[slot].used;
        unsigned faces = cube ? 6 : 1;
        for (unsigned face = 0; face < faces; ++face)
        for (unsigned i = 0; i < count; ++i) {
            unsigned copy = face * count + i;
            const Level* l = &levels[i];
            VkDeviceSize offset = at + (VkDeviceSize)face * face_stride + l->offset;
            from[copy] = m->buffer;
            if (yuv) {
                if (!yuv_to_rgba(r, cb, m->buffer, offset, format, l->width, l->height, l->stride, &from[copy], &offset)) {
                    once_value(r, 16, format,
                               "no room to make a texture's rows linear (format 0x%08" PRIx32 ", %ux%u)", format,
                               (unsigned)width, (unsigned)height);
                    from[copy] = VK_NULL_HANDLE, complete = false;
                }
                copies[copy] = (VkBufferImageCopy){.bufferOffset = offset,
                                                .bufferRowLength = l->width,
                                                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, face, 1},
                                                .imageExtent = {l->width, l->height, 1}};
                continue;
            }
            if (rgb24) {
                if (!linear && !deswizzle(r, cb, m->buffer, offset, type == GXM_TEXTURE_TILED, l->columns, l->rows,
                                          bytes, &from[copy], &offset)) {
                    once_value(r, 16, format,
                               "no room to make a texture's rows linear (format 0x%08" PRIx32 ", %ux%u)", format,
                               (unsigned)width, (unsigned)height);
                    from[copy] = VK_NULL_HANDLE, complete = false;
                    continue;
                }
                if (!rgb24_to_rgba(r, cb, from[copy], offset, l->width, l->height, l->stride, &from[copy], &offset)) {
                    once_value(r, 16, format,
                               "no room to expand a texture's RGB bytes (format 0x%08" PRIx32 ", %ux%u)", format,
                               (unsigned)width, (unsigned)height);
                    from[copy] = VK_NULL_HANDLE, complete = false;
                }
                copies[copy] = (VkBufferImageCopy){.bufferOffset = offset,
                                                .bufferRowLength = l->width,
                                                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, face, 1},
                                                .imageExtent = {l->width, l->height, 1}};
                continue;
            }
            if (!linear && !deswizzle(r, cb, m->buffer, offset, type == GXM_TEXTURE_TILED, l->columns, l->rows, bytes,
                                      &from[copy], &offset)) {
                once_value(r, 16, format,
                           "no room to make a texture's rows linear (format 0x%08" PRIx32 ", %ux%u)", format,
                           (unsigned)width, (unsigned)height);
                from[copy] = VK_NULL_HANDLE, complete = false;
            }
            copies[copy] = (VkBufferImageCopy){.bufferOffset = offset,
                                            .bufferRowLength = l->stride / bytes * block,
                                            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, face, 1},
                                            .imageExtent = {l->width, l->height, 1}};
        }
        barrier(cb, t->image, VK_IMAGE_ASPECT_COLOR_BIT, t->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        for (unsigned i = 0; i < faces * count; ++i)
            if (from[i])
                vkCmdCopyBufferToImage(cb, from[i], t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copies[i]);
            else
                complete = false;
        r->linear[slot].used = used;
        barrier(cb, t->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        t->synced = complete;
        if (complete) t->since = vita_watch_protect(data, span);
        rendering_resume(r, cb);
        if (!complete) return NULL;
    } else
        t->clean = epoch;
    t->used = r->scene;
    return t;
}
/* a program's uniform buffers (UsseShader.buffers) from guest memory into the staging ring: the offset there, or
   UINT32_MAX without room */
static uint32_t stage_uniforms(struct VitaRender* r, VitaProcess* p, const UsseShader* meta, const uint32_t* buffers) {
    unsigned slot = vita_vk_slot();
    VkDeviceSize align = r->vk->properties.limits.minStorageBufferOffsetAlignment;
    VkDeviceSize at = (r->staging[slot].used + align - 1) / align * align, size = meta->buffer_words * 4u;
    if (!size) size = 4;
    if (at + size > STAGING) return UINT32_MAX;
    unsigned char* to = r->staging[slot].map + at;
    memset(to, 0, size);
    for (unsigned i = 0; i < meta->buffer_count; ++i) {
        uint32_t address = buffers[meta->buffers[i].container];
        if (address && vita_mapped(p, address, meta->buffers[i].words * 4))
            memcpy(to + meta->buffers[i].offset * 4, vita_ptr(p, address), meta->buffers[i].words * 4);
    }
    r->staging[slot].used = at + size;
    return (uint32_t)at;
}
/* stream i's vertex buffer, unless the recording already holds it there */
static void bind_vertex(Bound* b, VkCommandBuffer cb, unsigned i, VkBuffer buffer, VkDeviceSize at) {
    if (b->vertex[i] == buffer && b->vertex_at[i] == at) return;
    vkCmdBindVertexBuffers(cb, i, 1, &buffer, &at);
    b->vertex[i] = buffer, b->vertex_at[i] = at;
}
/* a draw recorded into the scene, or why it isn't */
/* SceGxmPrimitiveType: triangles, lines, points, a strip, a fan (bits 26 to 28) */
static const VkPrimitiveTopology topologies[6] = {
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,  VK_PRIMITIVE_TOPOLOGY_LINE_LIST,    VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
/* a draw's shaders, each made for what the translator is told of it: the vertex program's attributes, the fragment
   program's texture formats (NULL for one that cannot be) */
static void draw_shaders(struct VitaRender* r, VitaProcess* p, const GxmContext* c, GxmVertexProgram* vp,
                         GxmFragmentProgram* fp, Shader** vs, Shader** fs) {
    UsseHints vh = {0}, fh = {0};
    for (unsigned i = 0; i < vp->attribute_count && i < USSE_ATTRIBUTES; ++i) {
        vh.attributes[i].reg = vp->attributes[i].reg;
        vh.attributes[i].format = vp->attributes[i].format;
        vh.attributes[i].components = vp->attributes[i].components;
    }
    vh.attribute_count = vp->attribute_count;
    for (unsigned i = 0; i < USSE_TEXTURES; ++i) fh.texture_formats[i] = gxm_texture_format(&c->textures[i]);
    *vs = shader_for(r, p, &vp->shader, vp->program, &vh);
    *fs = shader_for(r, p, &fp->shader, fp->program, &fh);
}
uint64_t vita_render_pipeline_key(VitaProcess* p, GxmContext* c, uint32_t primitive) {
    struct VitaRender* r = render_of(p);
    if (!r || !r->rendering || primitive >> 26 >= 6) return 0;
    GxmVertexProgram* vp = &p->gxm->vertex[(c->vertex_program - GXM_HANDLE_VERTEX) / GXM_PROGRAM_HANDLE];
    GxmFragmentProgram* fp = &p->gxm->fragment[(c->fragment_program - GXM_HANDLE_FRAGMENT) / GXM_PROGRAM_HANDLE];
    Shader *vs, *fs;
    draw_shaders(r, p, c, vp, fp, &vs, &fs);
    if (!vs || !fs || vs->failed || fs->failed) return 0;
    return pipeline_key(r, c, vp, vs, fp, fs, topologies[primitive >> 26]);
}
uint32_t vita_render_drawn_scale(VitaProcess* p) {
    struct VitaRender* r = render_of(p);
    return r ? r->drawn : 0;
}
bool vita_render_pipeline_made(VitaProcess* p, GxmContext* c, uint32_t primitive) {
    struct VitaRender* r = render_of(p);
    if (!r || !r->rendering || primitive >> 26 >= 6) return false;
    GxmVertexProgram* vp = &p->gxm->vertex[(c->vertex_program - GXM_HANDLE_VERTEX) / GXM_PROGRAM_HANDLE];
    GxmFragmentProgram* fp = &p->gxm->fragment[(c->fragment_program - GXM_HANDLE_FRAGMENT) / GXM_PROGRAM_HANDLE];
    Shader *vs, *fs;
    draw_shaders(r, p, c, vp, fp, &vs, &fs);
    bool making;
    return vs && fs && !vs->failed && !fs->failed &&
           pipeline_for(r, c, vp, vs, fp, fs, topologies[primitive >> 26], &making) != VK_NULL_HANDLE;
}
static const char* draw(VitaProcess* p, GxmContext* c, uint32_t primitive, uint32_t index_format, uint32_t indices,
                        uint32_t count, uint32_t instances) {
    struct VitaRender* r = render_of(p);
    if (!r || !r->rendering) return "no scene being drawn";
    VitaGxm* g = p->gxm;
    GxmVertexProgram* vp = &g->vertex[(c->vertex_program - GXM_HANDLE_VERTEX) / GXM_PROGRAM_HANDLE];
    GxmFragmentProgram* fp = &g->fragment[(c->fragment_program - GXM_HANDLE_FRAGMENT) / GXM_PROGRAM_HANDLE];
    Shader *vs, *fs;
    draw_shaders(r, p, c, vp, fp, &vs, &fs);
    if (!vs || !fs || vs->failed || fs->failed) return "a program not translated";
    unsigned kind = primitive >> 26;
    if (kind >= 6) {
        once_value(r, 10, primitive, "primitive type 0x%08" PRIx32, primitive);
        return "its primitive type";
    }
    VkCommandBuffer cb = vita_vk_commands();
    /* the textures first: copying one ends the rendering for a moment */
    VkDescriptorImageInfo images[USSE_TEXTURES];
    for (unsigned i = 0; i < USSE_TEXTURES; ++i) {
        images[i] = (VkDescriptorImageInfo){0};
        if (!(fs->meta.sampler_mask & (1u << i))) continue;
        Texture* t = texture_for(r, p, cb, &c->textures[i]);
        if (!t) return "a texture";
        /* a strided texture's min and mip filter bits are its stride's: its min filter is its mag filter */
        bool strided = gxm_texture_type(&c->textures[i]) == GXM_TEXTURE_LINEAR_STRIDED;
        unsigned linear = (!strided && gxm_texture_min_filter(&c->textures[i]) != 0) ||
                          gxm_texture_mag_filter(&c->textures[i]) != 0;
        unsigned mode = gxm_texture_uaddr(&c->textures[i]);
        unsigned mip = strided ? 0 : gxm_texture_mip_filter(&c->textures[i]);
        images[i] = (VkDescriptorImageInfo){r->samplers[linear][mip][mode == 0 ? 0 : mode == 1 ? 1 : 2], t->view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    bool making;
    VkPipeline pipeline = pipeline_for(r, c, vp, vs, fp, fs, topologies[kind], &making);
    if (!pipeline) return making ? (++r->skipped, "its pipeline in the making (async_pipelines)") : "no pipeline";
    uint32_t vertex_at = stage_uniforms(r, p, &vs->meta, c->vertex_uniform);
    uint32_t fragment_at = stage_uniforms(r, p, &fs->meta, c->fragment_uniform);
    if (vertex_at == UINT32_MAX || fragment_at == UINT32_MAX) {
        once(r, 11, "the uniform staging ring is full");
        return "the uniform staging ring full";
    }
    if (fs->meta.reads_color && !r->vk->local_read) {
        once(r, 19, "a fragment program reads its pixel's color, which this device can't give it in place");
        return "its program reads its pixel's color";
    }
    unsigned slot = vita_vk_slot();
    VkDescriptorBufferInfo buffers[2] = {{r->staging[slot].buffer, 0, vs->meta.buffer_words ? vs->meta.buffer_words * 4 : 4},
                                         {r->staging[slot].buffer, 0, fs->meta.buffer_words ? fs->meta.buffer_words * 4 : 4}};
    VkDescriptorImageInfo own = {VK_NULL_HANDLE, drawn_view(r->color), color_layout(r)};
    /* The set of the draw before, when it holds what this one reads: the buffers are the staging ring's, whose offsets
       are dynamic, so a run of draws on the same textures and program sizes (a sprite batch) shares one set and
       allocates and writes none */
    Bound* b = &r->bound;
    VkImageView input = fs->meta.reads_color ? own.imageView : VK_NULL_HANDLE;
    bool same = b->set && b->input == input && b->ranges[0] == buffers[0].range && b->ranges[1] == buffers[1].range;
    for (unsigned i = 0; same && i < USSE_TEXTURES; ++i)
        same = b->images[i].sampler == images[i].sampler && b->images[i].imageView == images[i].imageView;
    VkDescriptorSet set = same ? b->set : vita_vk_set(r->set_layout);
    if (!set) {
        once(r, 12, "out of descriptor sets");
        return "out of descriptor sets";
    }
    if (!same) {
        VkWriteDescriptorSet writes[3 + USSE_TEXTURES];
        unsigned n = 0;
        if (fs->meta.reads_color)
            writes[n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                 .dstSet = set,
                                                 .dstBinding = USSE_COLOR_BINDING,
                                                 .descriptorCount = 1,
                                                 .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
                                                 .pImageInfo = &own};
        for (unsigned i = 0; i < 2; ++i)
            writes[n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                 .dstSet = set,
                                                 .dstBinding = i,
                                                 .descriptorCount = 1,
                                                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
                                                 .pBufferInfo = &buffers[i]};
        for (unsigned i = 0; i < USSE_TEXTURES; ++i)
            if (images[i].imageView)
                writes[n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                     .dstSet = set,
                                                     .dstBinding = USSE_TEXTURE_BINDING + i,
                                                     .descriptorCount = 1,
                                                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                     .pImageInfo = &images[i]};
        vkUpdateDescriptorSets(r->vk->device, n, writes, 0, NULL);
        b->set = set, b->input = input, b->ranges[0] = buffers[0].range, b->ranges[1] = buffers[1].range;
        for (unsigned i = 0; i < USSE_TEXTURES; ++i) b->images[i] = images[i];
    }
    uint32_t offsets[2] = {vertex_at, fragment_at};
    if (b->pipeline != pipeline) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        b->pipeline = pipeline;
    }
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->layout, 0, 1, &set, 2, offsets);
    /* without rasterization-order access, the color the earlier draws wrote is made visible to this one's reads, which
       is as far as Vulkan orders them (overlapping primitives of one draw read each other's colors undefined) */
    if (fs->meta.reads_color && !r->vk->raster_order) {
        VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                              .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_DEPENDENCY_BY_REGION_BIT, 1, &mb, 0, NULL, 0, NULL);
    }
    /* the viewport: GXM's maps [-1, 1] by its offsets and scales (y down for a negative scale), Vulkan's by an origin
       and size, which may be negative; the depth transform is the shader's (UssePush) */
    VkViewport viewport = {0, 0, (float)r->width, (float)r->height, 0, 1};
    UssePush push = {c->viewport_enabled ? 1.0f : 0.0f, (float)r->width, (float)r->height, 0.0f, 1.0f};
    if (c->viewport_enabled) {
        viewport.x = c->viewport_offset[0] - c->viewport_scale[0];
        viewport.width = 2 * c->viewport_scale[0];
        viewport.y = c->viewport_offset[1] - c->viewport_scale[1];
        viewport.height = 2 * c->viewport_scale[1];
        push.z_offset = c->viewport_offset[2], push.z_scale = c->viewport_scale[2];
    }
    VkRect2D scissor = {{0, 0}, {r->width, r->height}};
    /* SceGxmRegionClipMode: none, all (0x40000000: nothing drawn), outside (0x80000000: drawing inside the rectangle
       only), inside (drawn whole, as Vita3K does) */
    if (c->region_clip_mode == 0x40000000u) return "its region clip";
    if (c->region_clip_mode == 0x80000000u) {
        uint32_t x0 = c->clip_x0 < r->width ? c->clip_x0 : r->width, y0 = c->clip_y0 < r->height ? c->clip_y0 : r->height;
        uint32_t x1 = c->clip_x1 + 1 < r->width ? c->clip_x1 + 1 : r->width;
        uint32_t y1 = c->clip_y1 + 1 < r->height ? c->clip_y1 + 1 : r->height;
        scissor = (VkRect2D){{(int32_t)x0, (int32_t)y0}, {x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0}};
    }
    /* the scene drawn larger: the viewport and the scissor in its pixels, the shaders' window coordinates (UssePush)
       still the console's */
    float k = (float)r->drawn;
    viewport.x *= k, viewport.y *= k, viewport.width *= k, viewport.height *= k;
    scissor.offset.x *= (int32_t)r->drawn, scissor.offset.y *= (int32_t)r->drawn;
    scissor.extent.width *= r->drawn, scissor.extent.height *= r->drawn;
    /* sceGxmSetFrontStencilRef's reference and sceGxmSetFrontStencilFunc's compare and write masks */
    uint32_t stencil[3] = {c->stencil_ref[0] & 0xffu, c->stencil_masks[0][0] & 0xffu, c->stencil_masks[0][1] & 0xffu};
    /* each of these only when it differs from what the recording holds from the draw before */
    if (!b->dynamic || memcmp(&b->viewport, &viewport, sizeof viewport)) {
        vkCmdSetViewport(cb, 0, 1, &viewport);
        b->viewport = viewport;
    }
    if (!b->dynamic || memcmp(&b->scissor, &scissor, sizeof scissor)) {
        vkCmdSetScissor(cb, 0, 1, &scissor);
        b->scissor = scissor;
    }
    if (!b->dynamic || memcmp(b->stencil, stencil, sizeof stencil)) {
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, stencil[0]);
        vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, stencil[1]);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, stencil[2]);
        memcpy(b->stencil, stencil, sizeof stencil);
    }
    if (!b->dynamic || memcmp(&b->push, &push, sizeof push)) {
        push_constants(cb, r->layout, VK_SHADER_STAGE_VERTEX_BIT, &push, sizeof push);
        b->push = push;
    }
    if (!b->dynamic) { /* the scale the pixel's position a fragment program reads is divided by (usse.h) */
        uint32_t word;
        memcpy(&word, &k, sizeof word);
        snx_le32_array(&word, 1);
        vkCmdPushConstants(cb, r->layout, VK_SHADER_STAGE_FRAGMENT_BIT, USSE_FRAGMENT_PUSH, sizeof word, &word);
    }
    b->dynamic = true;
    VkBuffer set_buffer = VK_NULL_HANDLE;
    VkDeviceSize set_at = 0;
    for (unsigned i = 0; i < vp->stream_count; ++i) {
        VkDeviceSize at = 0;
        const Import* m = c->streams[i] ? buffer_of(r, p, c->streams[i], 1, &at) : NULL;
        if (!m && !c->streams[i] && set_buffer) {
            /* a stream the game never set (its program declares more streams than it reads from): the pipeline wants
               a buffer bound for it, and nothing the program reads is in it */
            bind_vertex(b, cb, i, set_buffer, set_at);
            continue;
        }
        if (!m) {
            once_value(r, 13, ((uint64_t)i << 32) | c->streams[i],
                       "vertex stream %u at 0x%08" PRIx32 " is not mapped for the GPU", i, c->streams[i]);
            return "a vertex stream unmapped";
        }
        bind_vertex(b, cb, i, m->buffer, at);
        set_buffer = m->buffer, set_at = at;
    }
    VkDeviceSize at = 0;
    const Import* m = buffer_of(r, p, indices, count * (index_format ? 4u : 2u), &at);
    if (!m) {
        once_value(r, 14, indices, "indices at 0x%08" PRIx32 " are not mapped for the GPU", indices);
        return "its indices unmapped";
    }
    VkIndexType index_type = index_format ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    if (b->index != m->buffer || b->index_at != at || b->index_type != index_type) {
        vkCmdBindIndexBuffer(cb, m->buffer, at, index_type);
        b->index = m->buffer, b->index_at = at, b->index_type = index_type;
    }
    vkCmdDrawIndexed(cb, count, instances, 0, 0, 0);
    ++r->draws;
    return NULL;
}
void vita_render_draw(VitaProcess* p, GxmContext* c, uint32_t primitive, uint32_t index_format, uint32_t indices,
                      uint32_t count, uint32_t instances) {
    const char* skipped = draw(p, c, primitive, index_format, indices, count, instances);
    if (!g_settings.vita_gxm_trace) return;
    VitaGxm* g = p->gxm;
    uint32_t vp = g->vertex[(c->vertex_program - GXM_HANDLE_VERTEX) / GXM_PROGRAM_HANDLE].program;
    const GxmFragmentProgram* f = &g->fragment[(c->fragment_program - GXM_HANDLE_FRAGMENT) / GXM_PROGRAM_HANDLE];
    printf("gxm: draw %u indices, primitive %u, program 0x%08" PRIx32 "/0x%08" PRIx32 ", depth 0x%08" PRIx32
           " write %u, cull %u, z %g %+g, blend %u 0x%08" PRIx32 "%s%s\n",
           (unsigned)count, (unsigned)(primitive >> 26), vp, f->program, c->depth_func, (unsigned)c->depth_write,
           (unsigned)c->cull_mode, c->viewport_scale[2], c->viewport_offset[2], (unsigned)f->blend, f->blend_info,
           skipped ? " skipped: " : "", skipped ? skipped : "");
    /* the textures its fragment program samples: where, its size, format and layout, its min, mag and mip filters */
    const Shader* fs = f->shader;
    for (unsigned i = 0; fs && i < USSE_TEXTURES; ++i)
        if (fs->meta.sampler_mask & (1u << i))
            printf("gxm:   texture %u at 0x%08" PRIx32 " %ux%u format 0x%08" PRIx32 " layout %u, filters %u %u %u\n", i,
                   gxm_texture_data(&c->textures[i]), (unsigned)gxm_texture_width(&c->textures[i]),
                   (unsigned)gxm_texture_height(&c->textures[i]), gxm_texture_format(&c->textures[i]),
                   (unsigned)gxm_texture_type(&c->textures[i]), (unsigned)gxm_texture_min_filter(&c->textures[i]),
                   (unsigned)gxm_texture_mag_filter(&c->textures[i]), (unsigned)gxm_texture_mip_filter(&c->textures[i]));
}

/* ---- decoded pictures ---- */
bool vita_render_picture(VitaProcess* p, uint32_t address, uint32_t pitch, uint32_t pixel_type, uint32_t width,
                         uint32_t height, const uint8_t* const planes[3], const int linesize[3]) {
    struct VitaRender* r = render_of(p);
    if (!r || (pixel_type != 0x10 && pixel_type != 0x20) || (width | height) & 1 || pitch & 15 || pitch < width)
        return false;
    uint32_t cw = width / 2, ch = height / 2, y_bytes = width * height, c_bytes = cw * ch;
    uint32_t total = pitch * height * 3 / 2, words = total / 4;
    VkDeviceSize at;
    const Import* m = buffer_of(r, p, address, total, &at);
    VkDeviceSize align = storage_align(r);
    if (!m || at % align) {
        once_value(r, 18, address,
                   "a decoded picture at 0x%08" PRIx32 " is not mapped for the GPU, or not aligned for it", address);
        return false;
    }
    /* the planes' rows staged as they are, each exactly its width, in the recording's slot: begun first, as beginning
       it waits for the slot's last submission and frees its staging, which the GPU may be reading until then */
    VkCommandBuffer cb = vita_vk_commands();
    unsigned slot = vita_vk_slot();
    VkDeviceSize from = (r->staging[slot].used + align - 1) / align * align, size = y_bytes + 2 * (VkDeviceSize)c_bytes;
    if (from + size > STAGING) {
        once(r, 11, "the uniform staging ring is full");
        return false;
    }
    unsigned char* to = r->staging[slot].map + from;
    for (uint32_t y = 0; y < height; ++y) memcpy(to + y * width, planes[0] + (size_t)y * (size_t)linesize[0], width);
    for (unsigned k = 1; k < 3; ++k)
        for (uint32_t y = 0; y < ch; ++y)
            memcpy(to + y_bytes + (k - 1) * c_bytes + y * cw, planes[k] + (size_t)y * (size_t)linesize[k], cw);
    r->staging[slot].used = from + size;
    PictureParams params = {words, pitch * height, pitch, width, height, y_bytes, y_bytes + c_bytes,
                            pixel_type == 0x20, pixel_type == 0x20};
    if (r->rendering) rendering_pause(cb);
    bool ok = compute(r, cb, r->picture, r->staging[slot].buffer, from, m->buffer, at, total, &params, sizeof params, words);
    if (ok) mirror_written(r, m, address, total, total, 1);
    gpu_wrote(r, address, total);
    if (r->rendering) rendering_resume(r, cb);
    return ok;
}
