/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Shaders and pipelines. A guest shader is translated directly to SPIR-V and kept as a module.
   Every stage has a descriptor set of the same bindings, one set per stage
   (shader_vk_set). Pipelines are made per PipeKey: the shaders, vertex layout, attachment formats and blend state;
   everything else is dynamic state. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/shader/shader.h"
#include "common/utilities/spirv_emit.h" /* SPV_CAPABILITY_COOPERATIVE_MATRIX_KHR, the scanner's */
#include "nx/gpu/shader/sm86.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "common/gpu/vk/vk_common.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/shader/advanced_blend_spirv.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/vk/descriptor_heaps.h"
#include "nx/gpu/vk/capture.h"
#include "nx/gpu/vk/lookup_ways.h"
#include "nx/gpu/vk/pipeline_file.h"
#include "nx/gpu/vk/pipeline_parts.h"
#include "nx/gpu/vk/internal_spv.h"
#include "nx/gpu/vk/tess_passthrough.h"
#include "nx/gpu/vk/texture_arrays.h"
#include "nx/gpu/renderer.h"
#include "nx/gpu/shader/shipped.h"
#include "nx/gpu/shipped_pass.h"
#include "nx/runtime/platform/tracy.h"
#include "common/utilities/build_info.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "common/utilities/spirv_opt.h"
#include <sys/stat.h>

/* SH_CODE (vk.h), the bytes of a program vk_shader_get reads from its SPH. The SPH has no size, so shader_hash ends the
   program at its EXIT or the zero padding after it, and NVN packs programs into one pool: the window has to reach past
   the largest program's end (FINAL FANTASY RESONANCE has one of 70,728 bytes, an Ounce game one of 149,120), without
   going on into the next program */

/* the shader of each module, for the log line of a pipeline slow to make (pipe_make): its program's hash, its stage
   and its words of SPIR-V. A table of its own, under its own lock, as the workers that make pipelines do not read the
   GPU thread's tables. Full, a module goes unnamed */
#define MODINFO_N 16384u
typedef struct {
    u64 mod, hash;
    u32 stage, words;
} ModInfo;
/* the shader last found for an address and key, VK_SHA_WAYS per set (lookup_ways.h). It is checked against memory
   once per submit: unchanged by write tracking since then, or else by the hash of its code */
typedef struct {
    u64 gva, gva_a, gw;
    u64 shader_generation;
    u32 mask[26], epoch, wseen;
    int stage;
    VkSh* s;
} ShA;
struct PipeLib;
typedef struct Pipe {
    PipeKey k;
    u64 h;
    /* once ready, the best one made (atomic): the link of its parts, then the optimized one, with pipeline libraries */
    VkPipeline p;
    int ready;         /* set by the worker after p, with release order */
    struct PipeLib* lib[4]; /* with pipeline libraries, its parts (vertex input, pre-rasterization, fragment shader,
                               fragment output: VK_PART_* by bit), else NULL */
    /* p is the optimized pipeline (atomic, release after p). The link it took the place of stays, as every pipeline
       does for the run: the draws keep the handles they had (render.c's g_draw), and a link freed once gave its handle
       to a new pipeline, which the headless test's watch then took for the link */
    int opt;
    VkPipelineLayout layout;
    VkShaderModule fs; /* k.fs, or the empty fragment shader */
    int prewarm;       /* one of the file's, made before a draw asked for it */
    int queued;        /* in the workers' queue (g_pipeline.pj_lock): 1 a draw's, 2 the file's, 3 to optimize */
    /* a draw's: when it was queued, and whether every worker was busy then with one or more on an optimized
       pipeline (g_pipeline.pj_lock), for the stats of its wait for a worker */
    u64 queued_ns;
    int behind_opt;
    u32 standin;       /* its index + 1 in the stand-ins' table (vk_pipeline_get_deferred), 0 for none yet */
    struct Pipe *next, *job;
} Pipe;
#define PIPEH 4096
/* a shader of the shipped file: its key's words, the clip word of the key the pass gave it (ship_clip), and where its
   record starts (at 0: a free slot) */
typedef struct {
    u64 hash, qh, qh_a, at;
    u32 clen, clen_a, stage, clip;
} ShipIdx;
typedef union {
    PipeViKey vi;
    PipePrKey pr;
    PipeFsKey fs;
    PipeFoKey fo;
} PipePartKey;
typedef struct PipeLib {
    PipeKey src;       /* the whole key it is made from (pipe_info): its own state alone (vk_pipe_part_src) */
    VkShaderModule fs; /* a fragment shader part's program, or the empty one */
    PipePartKey key;   /* its key, zeroed past the part's own */
    u32 part;          /* VK_PART_* */
    u64 h;
    VkPipeline p;
    int state; /* 0 to make, 1 being made, 2 made (g_pipeline.pj_lock) */
    struct PipeLib* next;
} PipeLib;
#define PLIBH 4096
/* a shipped program's part to make into the pipeline cache ahead of its draws (ship_warm): with the part warm, a draw
   that meets the same program links it in about a millisecond instead of waiting seconds for its compile. The module is
   made from the shipped SPIR-V and destroyed by the worker once the part is in the cache. On the lowest queue, after
   the optimized pipelines */
typedef struct WarmItem {
    PipeKey src;       /* the part's state alone, as a draw's vk_pipe_part_src builds it: for a shipped program only its
                          indexed bit */
    VkShaderModule fs; /* a fragment part's module, also in src.fs; VK_NULL_HANDLE for a pre-rasterization part */
    u32 part;          /* VK_PART_PR or VK_PART_FS */
    struct WarmItem* next;
} WarmItem;
/* The file's state */
static struct {
    /* The caches live in shader_cache/ of the game's cache folder (g_cachedir), wherever the runtime was started
       from */
    char cache_dir[1100], pcache_path[1200];
    char driver_tag[SNX_VK_DRIVER_TAG]; /* the driver's, whose files of the cache these are (snx_vk_driver_tag) */
    int pcache_dirty;        /* pipelines made since the cache was last written (atomic: it is saved outside vk_lock) */
    u64 pcache_used;         /* plat_ns when a pipeline last went into the cache (atomic) */
    VkPipelineLayout playouts[1u << 5]; /* by the sets with texture arrays, made when first used */
    PMutex playout_lock;                /* making one: the file's reading thread asks for them too */
    VkPipelineCache pcache;
    PMutex psave; /* vk_pipeline_save's: one save at a time */
    /* the draw state and stage whose bound constant buffers the translator reads, for the vk_shader_get call running */
    const DsState* sh_ds;
    int sh_bind;
    VkCbReader rec_read;
    VkCbReads* rec;
    int rec_failed;
    /* vk_corpus_dir's answer once asked for, and the folder it made for it */
    char corpus_dir[sizeof g_exedir + sizeof "captures/shaders"];
    const char* corpus_at;
    int corpus_asked;
    /* The translator's reader (g_shader_cb_read) and what rec_read records into are one for the process: the GPU
       thread's draws and dispatches and the pass over the programs the game ships, on the shader files' thread
       (ship_program), translate and hash one at a time */
    PMutex xlate_lock;
    ModInfo modinfo[MODINFO_N];
    PMutex modinfo_lock;
    VkSh* shh[1024];
    ShA sha[VK_SHA_WAYS << VK_SHA_SET_BITS];
    /* for the stats line: pipelines made, the GPU thread's time waiting for new ones, shaders translated and the time
       their translation and modules took (all on the GPU thread) */
    u64 pipes_new, sh_new, sh_ns;
    u64 pipe_wait_ns;       /* the GPU thread's and the recording thread's waits, added atomically */
    u64 pipe_make_ns;       /* the workers' time making pipelines, added atomically */
    u64 pipe_hits;          /* pipelines the pipeline cache had */
    VkShaderModule null_fs; /* an empty fragment shader for depth-only passes, made when first asked for */
    Pipe* pipeh[PIPEH];
    /* The shaders and pipelines of earlier runs (pipeline_file.c), so that each run doesn't translate the same shaders
       and make the same pipelines again as views come into sight: one game made 440 pipelines and 500 shaders every
       session, each pipeline 3.4 ms on NVIDIA even from the driver's cache and 125 ms without, its draws skipped
       meanwhile. A thread of its own reads the file at startup, makes the modules and queues the pipelines to the
       workers behind any draw's; the first draw puts what it made in the tables (pf_merge), where vk_shader_get finds a
       shader only as it finds one made this run, by its code hashed in guest memory, its key and its constant buffer
       reads */
    struct {
        PFile file;
        char path[1400];
        int on;      /* the files are used (not when shaders are recorded, which wants every one translated) */
        PMutex lock; /* loaded, and done's */
        PCond done;
        int loaded;         /* the reading thread is done with the main file (lock) */
        int merged;         /* what it made is in shh and pipeh (vk_lock) */
        VkSh *sh, *sh_tail; /* its shaders, in the file's order, linked by next until merged */
        Pipe* pipes[PIPEH]; /* its pipelines by key hash, linked by next until merged */
        u32 nsh, npipe;
        u32 ncs, ncs_failed; /* the compute pipelines of the file made, and those that could not be */
        /* The shader programs the game ships (pf_ship_pass), translated before a draw asks for them, in a file of their
           own (ship, <game>.vkshipped) that is read as they are used: a game ships thousands, and a session uses a few
           of them. Its PF_SCAN records say what the pass read. The reading thread alone writes the index and the scans'
           set, and the draws read the index once ship_ready says it no longer changes */
        PFile ship;
        char ship_path[1400];
        /* the files the pass found nothing in (SHIP_NONE), kept across builds and drivers in a file of their own (scan,
           <game>.vkscan) under the walk's version, which an update of either leaves: the ship file's are started over
           */
        PFile scan;
        char scan_path[1400];
        ShipIdx* idx; /* its shaders by hash, open addressing (icap a power of two) */
        u32 icap, nidx;
        u64* scans; /* the PF_SCAN records, scan_key's, open addressing (scap a power of two) */
        u32 scap, nscans;
        PMutex note_lock; /* the pass's lines, for the screen: the latest, round; and the file it looks in */
        char note[RENDERER_NOTES][RENDERER_NOTE_SIZE];
        u32 nnote;                        /* the lines so far */
        char walk_at[RENDERER_FILE_SIZE]; /* the file or segment the pass looks in now (ShipSink's at) */
        /* atomics, written by the reading thread */
        ShipProgress progress;
        u32 ship_new;                   /* the pass's new shaders */
        int scanning;                   /* the pass runs */
        int ship_ready;                 /* the pass is over, or won't run: the draws may read the index (release) */
        u32 shaders_total, pipes_total; /* nsh and npipe once the file was read */
        char apart[64];
        /* atomics, written by the GPU thread and the pipeline workers */
        u64 made, hits; /* the file's pipelines made, and those the pipeline cache had */
        u32 ship_used;  /* the shipped file's shaders a draw found */
        u32 ship_said;  /* ship_used as vk_pipeline_save last logged it (psave) */
    } pf;
    PMutex pj_lock;
    PCond pj_work, pj_done;
    int pj_started, pj_threads, pj_busy; /* pj_busy: the workers making a pipeline now */
    struct {
        /* the table: the GPU thread adds the parts of a draw's pipelines, the file's reading thread those of the file's
           */
        PMutex lock;
        PipeLib* h[PLIBH];
        VkPipelineLayout layouts[2][1u << 5]; /* the parts' own: [0] before rasterization, [1] fragment, by arrays */
        /* atomics: parts made by VK_PART_* bit, those the pipeline cache had and their ns; the links and the optimized
           pipelines made, their ns, and the optimized ones the cache had */
        u64 made[4], hits[4], ns[4];
        u64 links, link_ns, opts, opt_ns, opt_hits;
        /* the shipped programs' parts warmed into the pipeline cache ahead of their draws (ship_warm): queued, made,
           the cache hits, their ns */
        u64 warm_queued, warm_made, warm_hit, warm_ns;
        /* the draws' pipelines' waits for a worker: how many, their ns, the ns of those queued while every worker was
           busy and one or more made an optimized pipeline, and the longest (pj_lock) */
        u64 job_waits, job_wait_ns, job_behind_opt_ns, job_wait_max_ns;
        int opt_busy_max; /* the most workers that made optimized pipelines at once (pj_lock) */
    } plib;
    /* the workers' queues, and the wait for one pipeline to be ready. The file's pipelines a draw asked for are made
       first, in the order asked for; the rest of the file's (pf_load_pipe) after */
    Pipe *pj_head, *pj_tail;         /* a draw's */
    Pipe *pw_head, *pw_tail;         /* the file's */
    Pipe *po_head, *po_tail;         /* the optimized pipelines to make after their fast links, last */
    int po_busy;                     /* the workers making an optimized pipeline now (pj_lock) */
    WarmItem *warm_head, *warm_tail; /* last: the workers make these only when nothing else waits */
    int warm_busy;                   /* the workers warming a part now (pj_lock) */
    /* the stand-ins' table (pipe_standin), by pages of PSTAND_PAGE */
    Pipe** pstand[1024];
    u32 npstand;
    /* The last entries pipe_entry looked up: a draw that changes pipeline mostly goes back to one of a few, and the
       hash of a key this size was two thirds of the hashes of one game's draws. A pipe is never freed */
    Pipe* recent[4];
    u32 recent_next;
    /* the reads a shader of a file being rebuilt was translated with, which its translation again answers from (the
       reading thread's alone). A read it didn't make answers that the buffer isn't bound: the shader then keeps that
       read, and a draw whose buffers hold more translates its own (vk_shader_get checks every read) */
    const VkCbReads* replay;
    /* the constants of the program the pass translates now (ship_read), the reading thread's alone */
    const u8* ship_consts;
    size_t ship_nconsts;
} g_pipeline = {.playout_lock = PMUTEX_INIT,
                .psave = PMUTEX_INIT,
                .xlate_lock = PMUTEX_INIT,
                .modinfo_lock = PMUTEX_INIT,
                .pj_lock = PMUTEX_INIT,
                .plib = {.lock = PMUTEX_INIT}};
static void pcache_note(void);
#if defined(SNX_TRACY)
/* the stages' names, for the shader translate zone's text */
static const char* const vk_tracy_stage[] = {"vert", "tess ctrl", "tess eval", "geom", "frag", "comp", "task", "mesh"};
#endif

VkPipelineLayout g_vk_playout;
VkDescriptorSetLayout g_vk_dslayout[2][5];
u32 g_vk_stage_sets;
int g_vk_ftz; /* SH_FTZ_*: how translations flush FP32 subnormals (vk_pipeline_init) */
static void pf_start_load(void);
static void pf_merge(void);
static void pf_shader_record(const VkSh* s, const u32* spv, size_t nw, const u8* code, u32 ncode, const ShXfb* xfb);
static u32 src_bytes(u32 mlen);
static void pf_ship_found(VkSh* s);
static VkSh* pf_ship_take(u64 h, int stage, const u32 mask[26], u32 len, u64 qh, u32 len_a, u64 qh_a);
/* the bindings (DsBindings' stage) a program of stage reads: an Ounce's task and mesh programs have their own */
static int sh_bind_stage(int stage) {
    return stage == ST_TS ? DS_TASK : stage == ST_MS ? DS_MESH : stage <= ST_FS ? stage : 0;
}

static int sh_cb_read(u32 idx, u32 off, u32* out) {
    if (idx >= 18 || !g_pipeline.sh_ds) return 0;
    DsCbBlock in = ds_cb_inline(g_pipeline.sh_ds, g_pipeline.sh_bind, idx);
    u64 a = g_pipeline.sh_ds->bind.cb_addr[g_pipeline.sh_bind][idx];
    u32 sz = g_pipeline.sh_ds->bind.cb_size[g_pipeline.sh_bind][idx];
    u32 limit = in.bytes ? in.size : sz ? sz : 65536;
    if ((!a && !in.bytes) || limit < 4 || off > limit - 4) return 0;
    *out = in.bytes ? ds_inline_word(in, off) : gmem_r32(a + off);
    return 1;
}
static int rec_read(u32 idx, u32 off, u32* out) {
    int ok = g_pipeline.rec_read(idx, off, out);
    VkCbReads* reads = g_pipeline.rec;
    if (reads->n % 16 == 0) {
        VkCbRead* grown = realloc(reads->r, sizeof *grown * (reads->n + 16));
        if (!grown) {
            g_pipeline.rec_failed = 1;
            return ok;
        }
        reads->r = grown;
    }
    reads->r[reads->n++] = (VkCbRead){idx, off, (u32)ok, ok ? *out : 0};
    return ok;
}
const char* vk_corpus_dir(void) {
    char* dir = g_pipeline.corpus_dir;
    const char* at;
    if (g_pipeline.corpus_asked) return g_pipeline.corpus_at;
    at = g_settings.shader_corpus[0] ? g_settings.shader_corpus : NULL;
    if (!at && g_settings.shader_captures) { /* a phone's game has no environment to set */
        snprintf(dir, sizeof g_pipeline.corpus_dir, "%scaptures", g_exedir);
        plat_mkdir(dir);
        snprintf(dir, sizeof g_pipeline.corpus_dir, "%scaptures/shaders", g_exedir);
        plat_mkdir(dir);
        at = dir;
    }
    g_pipeline.corpus_at = at;
    g_pipeline.corpus_asked = 1;
    return at;
}

const char* vk_spirv_unavailable(const u32* spv, size_t words) {
    for (size_t at = 5; at < words;) { /* the capabilities come first */
        u32 width = spv[at] >> 16;
        if (!width || at + width > words || (spv[at] & 0xffffu) != 17) break;
        if (width == 2 && spv[at + 1] == 10 && !g_vk.feat.shaderFloat64) return "shaderFloat64 is unavailable";
        if (width == 2 && spv[at + 1] == 11 && !g_vk.feat.shaderInt64) return "shaderInt64 is unavailable";
        if (width == 2 && spv[at + 1] == 9 && !g_shader_float16) return "shaderFloat16 is unavailable";
        if (width == 2 && spv[at + 1] == 2 && !g_vk.feat.geometryShader) return "geometryShader is unavailable";
        if (width == 2 && spv[at + 1] == 3 && !g_vk.feat.tessellationShader) return "tessellationShader is unavailable";
        if (width == 2 && spv[at + 1] == 25 && !g_vk.feat.shaderImageGatherExtended)
            return "shaderImageGatherExtended is unavailable";
        if (width == 2 && spv[at + 1] == 27 && !g_vk.feat.shaderStorageImageMultisample)
            return "shaderStorageImageMultisample is unavailable";
        if (width == 2 && spv[at + 1] == 42 && !g_vk.feat.shaderResourceMinLod)
            return "shaderResourceMinLod is unavailable";
        if (width == 2 && spv[at + 1] == 54 && !g_vk.has_geometry_streams)
            return "geometryStreams is unavailable";
        if (width == 2 && spv[at + 1] == 32 && !g_vk.feat.shaderClipDistance)
            return "shaderClipDistance is unavailable";
        if (width == 2 && spv[at + 1] == 35 && !g_vk.feat.sampleRateShading) return "sampleRateShading is unavailable";
        if (width == 2 && spv[at + 1] == 55 && !g_vk.feat.shaderStorageImageReadWithoutFormat)
            return "shaderStorageImageReadWithoutFormat is unavailable";
        if (width == 2 && spv[at + 1] == 56 && !g_vk.feat.shaderStorageImageWriteWithoutFormat)
            return "shaderStorageImageWriteWithoutFormat is unavailable";
        if (width == 2 && spv[at + 1] == 5378 && !g_vk.has_fragment_interlock)
            return "fragmentShaderPixelInterlock is unavailable";
        if (width == 2 && spv[at + 1] == 4427 && !g_vk.has_shader_draw_parameters)
            return "shaderDrawParameters is unavailable";
        if (width == 2 && spv[at + 1] == 5055 && !g_shader_clock_scopes) return "shaderClock is unavailable";
        if (width == 2 && spv[at + 1] == 5283 && !g_vk.has_mesh) return "meshShader is unavailable";
        if (width == 2 && spv[at + 1] == 6912 && !g_shader_mixed_dot)
            return "shaderMixedFloatDotProductFloat16AccFloat32 is unavailable";
        if (width == 2 && (spv[at + 1] == SPV_CAPABILITY_COOPERATIVE_MATRIX_KHR || spv[at + 1] == 5345 || spv[at + 1] == 5346) && !g_shader_hmma_coop)
            return "cooperativeMatrix or the Vulkan memory model is unavailable";
        if (width == 2 && spv[at + 1] == 6033 && !g_shader_float_atomic_add)
            return "shaderBufferFloat32AtomicAdd is unavailable";
        at += width;
    }
    return NULL;
}

/* An SM86 program in max_bytes at code (a graphics program from its header on, a compute one after the 80 zero bytes
   of the compute path): 1 and *p, or 0 when no program starts there */
static int sm86_program(const u8* code, u32 max_bytes, Sm86Program* p) {
    return sm86_find_programs(code, max_bytes, p, 1) == 1;
}
static u32* sm86_translate(const u8* code, u32 max_bytes, int stage, ShaderInfo* si, size_t* words) {
    Sm86Program p;
    u32 sph[SM86_SPH_BYTES / 4];
    if (!sm86_program(code, max_bytes, &p) || !p.complete) {
        snprintf(si->first_unsupported, sizeof si->first_unsupported,
                 "no complete SM86 program in %#x bytes (words %08x %08x %08x %08x)", max_bytes, snx_ld_le32(code),
                 snx_ld_le32(code + 4), snx_ld_le32(code + 8), snx_ld_le32(code + 12));
        si->unsupported = 1;
        return NULL;
    }
    if (stage != ST_CS) {
        if (p.code == p.header) {
            snprintf(si->first_unsupported, sizeof si->first_unsupported, "an SM86 graphics program with no header");
            si->unsupported = 1;
            return NULL;
        }
        for (u32 i = 0; i < SM86_SPH_BYTES / 4; i++) sph[i] = snx_ld_le32(code + p.header + 4 * i);
    }
    return sm86_to_spirv_ftz(code + p.code, p.size, stage, stage == ST_CS ? NULL : sph, si, words, g_vk_ftz);
}
u32* vk_translate_spirv(const u8* code, u32 max_bytes, int stage, ShaderInfo* si, size_t* words, VkCbReader read,
                        VkCbReads* reads) {
    pm_lock(&g_pipeline.xlate_lock);
    int (*old)(u32, u32, u32*) = g_shader_cb_read;
    g_pipeline.rec_read = read;
    g_pipeline.rec = reads;
    g_pipeline.rec_failed = 0;
    g_shader_cb_read = rec_read;
    const VkShaderStageFlagBits stages[] = {VK_SHADER_STAGE_VERTEX_BIT,
                                            VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                            VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                            VK_SHADER_STAGE_GEOMETRY_BIT,
                                            VK_SHADER_STAGE_FRAGMENT_BIT,
                                            VK_SHADER_STAGE_COMPUTE_BIT,
                                            VK_SHADER_STAGE_TASK_BIT_EXT,
                                            VK_SHADER_STAGE_MESH_BIT_EXT};
    VkShaderStageFlagBits shader_stage = stages[stage];
    u32* result = g_shader_sm86 ? sm86_translate(code, max_bytes, stage, si, words)
                                : shader_to_spirv(code, max_bytes, stage, si, words, g_vk_ftz, g_vk_preserve_infnan,
                                                  g_vk_preserve_infnan64, (g_vk_subgroup_stages & shader_stage) != 0);
    g_shader_cb_read = old;
    g_pipeline.rec = NULL;
    int failed = g_pipeline.rec_failed;
    pm_unlock(&g_pipeline.xlate_lock);
    if (failed) {
        free(result);
        return NULL;
    }
    if (result) {
        const char* reason = vk_spirv_unavailable(result, *words);
        if (reason) {
            snprintf(si->first_unsupported, sizeof si->first_unsupported, "%s", reason);
            si->unsupported = 1;
            free(result);
            return NULL;
        }
        /* the shared optimizer, after the translator: what an instruction that dominates already computed, then
           the registers, predicates and flags that no later block reads, and what was made only for them */
        *words = spv_opt_values(result, *words);
        *words = spv_opt_dead_code(result, *words);
    }
    return result;
}

/* an SM86 program's key: its bytes up to its end, len (0 with no program there: the bytes of a header, so each such
   program keeps a key of its own) */
static u64 sm86_hash(const u8* code, u32 max_bytes, u32 len) {
    return snx_fnv1a(SNX_FNV_BASIS, code, len ? len : max_bytes < SM86_SPH_BYTES ? max_bytes : SM86_SPH_BYTES);
}
/* shader_hash with the jump tables of BRX read through read, as the translation reads them: without a reader the
   length stops at the first EXIT and leaves out code only a jump table reaches */
u64 vk_shader_hash(const u8* code, u32 max_bytes, u32* out_len, VkCbReader read) {
    if (g_shader_sm86) { /* the program's bytes up to its end, from the start of code (its header, the zero bytes) */
        Sm86Program p;
        /* with no program there, the bytes of a header: each such program keeps a key of its own */
        u32 len = sm86_program(code, max_bytes, &p) ? p.code + p.size : 0;
        *out_len = len;
        return sm86_hash(code, max_bytes, len);
    }
    pm_lock(&g_pipeline.xlate_lock);
    int (*old)(u32, u32, u32*) = g_shader_cb_read;
    g_shader_cb_read = read;
    u64 h = shader_hash(code, max_bytes, out_len);
    g_shader_cb_read = old;
    pm_unlock(&g_pipeline.xlate_lock);
    return h;
}
/* VertexA a and VertexB b (SH_CODE bytes each) merged into m, sizing them with shader_hash through read as the
   translation would: 1 when merged */
static int sh_merge_dual(const u8* a, const u8* b, u8* m, VkCbReader read) {
    pm_lock(&g_pipeline.xlate_lock);
    int (*old)(u32, u32, u32*) = g_shader_cb_read;
    g_shader_cb_read = read;
    int merged = shader_merge_dual(a, b, m, SH_CODE);
    g_shader_cb_read = old;
    pm_unlock(&g_pipeline.xlate_lock);
    return merged;
}
int vk_cb_reads_hold(const VkCbReads* reads, VkCbReader read) {
    for (u32 i = 0; i < reads->n; i++) {
        const VkCbRead* c = &reads->r[i];
        u32 v = 0;
        int ok = read(c->idx, c->off, &v);
        if ((u32)ok != c->ok || (ok && v != c->v)) return 0;
    }
    return 1;
}

u32 g_vk_buffer_image_slots;

VkDescriptorType vk_cbuf_type(u32 slot) {
    return slot < g_shader_uniform_cbufs ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
}
/* Each stage sees one set, with the 18 constant buffer slots and the uniform buffers of the renderer in it, and
   GeForce devices give a stage 15. The slots past what a device's limits hold are storage buffers, over the same
   memory, which those devices allow many more of */
static VkDescriptorType binding_type(u32 i);
static void cbufs_fit_limits(void) {
    const VkPhysicalDeviceLimits* l = &g_vk.props.limits;
    u32 fit = l->maxPerStageDescriptorUniformBuffers, set = l->maxDescriptorSetUniformBuffers / g_vk_stage_sets;
    /* the uniform buffers past the constant buffers: SNX_VKB_RS, SNX_VKB_FMT and SNX_VKB_ROOT, and SNX_VKB_TEXFUNCS
       and SNX_VKB_TEXMAP with the texture arrays */
    u32 own = 0;
    for (u32 i = SNX_VKB_SSBO; i < SNX_VKB_END; i++)
        if (binding_type(i) == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && (i < SNX_VKB_TEXARRAY || g_shader_texture_arrays))
            own++;
    if (set < fit) fit = set;
    fit = fit > own ? fit - own : 0;
    if (fit >= 18) return;
    /* a constant buffer's offset meets the uniform buffers' alignment: it must meet the storage buffers' too */
    if (l->minUniformBufferOffsetAlignment % l->minStorageBufferOffsetAlignment) return;
    g_shader_uniform_cbufs = fit;
    LOG("vulkan: this device's limits hold %u of the 18 constant buffers as uniform buffers, the others are storage "
        "buffers\n",
        fit);
}

/* descriptors of each type in one set: the binding ranges in shader.h */
static VkDescriptorType binding_type(u32 i) {
    if (i == SNX_VKB_SMAP || i == SNX_VKB_CAPTURE) return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if (i == SNX_VKB_TEXARRAY || i == SNX_VKB_TEXDEPTH) return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    if (i == SNX_VKB_TEXFUNCS || i == SNX_VKB_TEXMAP) return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    if (i >= SNX_VKB_IMGBUF) return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    return i < SNX_VKB_SSBO  ? vk_cbuf_type(i - SNX_VKB_CB)
           : i < SNX_VKB_TEX ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
           : i < SNX_VKB_IMG ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
           : i < SNX_VKB_RS  ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                             : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}
/* The bindings from SNX_VKB_TEXARRAY on are in the arrays layouts alone (vk_pipeline_init), and only where the device
   has texture arrays: a pool counts them for array_sets of its sets. A driver may make a pool's memory for all it can
   hold when the pool is made: Qualcomm's took 470 MiB for 1024 sets that could all hold the arrays (792 bytes a
   combined image sampler), where most sets have none */
void vk_pool_sizes(VkDescriptorPoolSize* sz, u32 sets, u32 array_sets) {
    static const VkDescriptorType ty[VK_POOL_TYPES] = {
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE};
    int k;
    for (k = 0; k < VK_POOL_TYPES; k++) {
        u32 n = ty[k] == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ? 2 : 0; /* auxiliary source/validity fetches */
        u32 arrays = 0, i;
        for (i = 0; i < SNX_VKB_END; i++) {
            if (binding_type(i) != ty[k]) continue;
            if (i < SNX_VKB_TEXARRAY)
                n++;
            else if (g_shader_texture_arrays)
                arrays += (i == SNX_VKB_TEXARRAY || i == SNX_VKB_TEXDEPTH) ? g_shader_tex_array : 1;
        }
        sz[k] = (VkDescriptorPoolSize){ty[k], n * sets + arrays * array_sets};
    }
}

/* The stages that see set s: a layout for each set, the same bindings, each visible to the stage that uses the set
   (shader_vk_set), within the per-stage limits a device has, which all stages seeing every set would multiply by
   five. A mesh program takes the vertex stage's set and a task program the tessellation control stage's, and the two
   mesh stages see both sets, where the device has them. With each mesh stage seeing only its own set, one game's task
   programs on an RTX 2080 read constants at addresses that no memory backs (Xid 31, GPCCLIENT_GCC, at the same task
   instruction in each run), and the GPU hung (Xid 109) in most runs. Eden's layouts give each binding the stage of
   its program, in one set */
VkShaderStageFlags vk_set_stages(int s) {
    static const VkShaderStageFlags vis[5] = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
                                              VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                              VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                              VK_SHADER_STAGE_GEOMETRY_BIT};
    if (s < 0 || s >= 5) return 0;
    return vis[s] | (s == shader_vk_set(ST_MS) || s == shader_vk_set(ST_TS) ? g_vk.mesh_stages : 0);
}
u32 vk_texture_array_elements(void) { return g_shader_texture_arrays ? g_shader_tex_array : 0; }
int vk_dslayout_arrays(VkDescriptorSetLayout layout) {
    for (u32 s = 0; s < 5; s++)
        if (layout && layout == g_vk_dslayout[1][s]) return g_shader_texture_arrays;
    return 0;
}
int vk_graphics_playout_is(VkPipelineLayout layout) {
    for (u32 i = 0; i < sizeof g_pipeline.playouts / sizeof g_pipeline.playouts[0]; i++)
        if (layout && __atomic_load_n(&g_pipeline.playouts[i], __ATOMIC_ACQUIRE) == layout) return 1;
    return 0;
}
VkPipelineLayout vk_playout(u32 arrays) {
    VkPipelineLayout* l = &g_pipeline.playouts[arrays & ((1u << 5) - 1)];
    VkPipelineLayout made = __atomic_load_n(l, __ATOMIC_ACQUIRE);
    if (made) return made;
    pm_lock(&g_pipeline.playout_lock);
    if (!*l) {
        VkDescriptorSetLayout sets[5];
        for (u32 s = 0; s < g_vk_stage_sets; s++) sets[s] = g_vk_dslayout[(arrays >> s) & 1][s];
        /* push constants: viewport/depth pc[0..3], fragment alpha test pc[4..6], sprite T flip pc[7], scaled textures
           pc[8..9], and of those the ones of the draw's size pc[24..25] */
        VkPushConstantRange pc = {VK_PUSH_STAGES, 0, 128};
        /* with pipeline libraries, a link of parts whose layouts have only their own sets (part_layout) */
        VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                         .flags = g_vk.has_gpl ? VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT : 0,
                                         .setLayoutCount = g_vk_stage_sets,
                                         .pSetLayouts = sets,
                                         .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &pc};
        if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &made))) made = VK_NULL_HANDLE;
        __atomic_store_n(l, made, __ATOMIC_RELEASE);
    }
    made = *l;
    pm_unlock(&g_pipeline.playout_lock);
    return made;
}

static void plib_init(void);
void vk_pipeline_init(void) {
    pm_lock_announce(&g_pipeline.playout_lock, "pipeline layout");
    plib_init();
    pm_lock_announce(&g_pipeline.xlate_lock, "shader translate");
    /* the tessellation and geometry stages' sets come after the first two, where the device binds that many */
    g_vk_stage_sets = g_vk.props.limits.maxBoundDescriptorSets >= 5 ? 5 : 2;
    /* A compact array holds the unique handles selected by all indexed tables: SNX_TEX_ARRAY of them, or as many as
       the device binds beside a stage's own SH_MAXTEX (texture_arrays.h), the line saying why where fewer */
    {
        const VkPhysicalDeviceLimits* l = &g_vk.props.limits;
        SnxVkTextureLimits tl = {l->maxPerStageDescriptorSamplers, l->maxPerStageDescriptorSampledImages,
                                 l->maxDescriptorSetSamplers, l->maxDescriptorSetSampledImages};
        char line[640];
        g_shader_tex_array =
            snx_vk_texture_array_say(line, sizeof line, g_shader_texture_arrays, &tl, SH_MAXTEX, g_shader_tex_array);
        g_shader_texture_arrays = g_shader_tex_array != 0;
        if (line[0]) LOG("%s\n", line);
    }
    cbufs_fit_limits();
    for (int s = 0; s < 5; s++) {
        VkDescriptorSetLayoutBinding b[SNX_VKB_END];
        u32 count = 0;
        u32 image_slots = g_vk.props.limits.maxPerStageDescriptorStorageImages;
        u32 buffer_slots;
        u32 i;
        u32 primary = image_slots < 16 ? image_slots : 16;
        if (primary > g_vk.props.limits.maxDescriptorSetStorageImages / 2)
            primary = g_vk.props.limits.maxDescriptorSetStorageImages / 2;
        u32 budget = s < 2 ? primary : (g_vk.props.limits.maxDescriptorSetStorageImages - 2 * primary) / 3;
        if (image_slots > budget) image_slots = budget;
        /* storage texel buffers count against the storage image limits, so buffer images get what images leave */
        buffer_slots = image_slots;
        if (image_slots > SNX_VKB_RS - SNX_VKB_IMG) image_slots = SNX_VKB_RS - SNX_VKB_IMG;
        buffer_slots -= image_slots;
        if (buffer_slots > SNX_VKB_CAPTURE - SNX_VKB_IMGBUF) buffer_slots = SNX_VKB_CAPTURE - SNX_VKB_IMGBUF;
        static const int shader_stage[5] = {ST_VS, ST_FS, ST_TCS, ST_TES, ST_GS};
        g_vk.image_slots[shader_stage[s]] = image_slots;
        g_vk.buffer_image_slots[shader_stage[s]] = buffer_slots;
        if (!s) g_vk_buffer_image_slots = buffer_slots;
        for (i = 0; i < SNX_VKB_END; i++) {
            if (i == SNX_VKB_CAPTURE && (s != 1 || !vk_capture_tiles_supported())) continue;
            /* Each graphics stage has storage bindings within both per-stage and whole-pipeline limits. */
            if (i >= SNX_VKB_IMG && i < SNX_VKB_RS && i - SNX_VKB_IMG >= image_slots) continue;
            if (i >= SNX_VKB_TEXARRAY && !g_shader_texture_arrays) continue;
            if (i >= SNX_VKB_IMGBUF && i < SNX_VKB_CAPTURE && i - SNX_VKB_IMGBUF >= buffer_slots) continue;
            b[count++] = (VkDescriptorSetLayoutBinding){
                .binding = i,
                .descriptorType = binding_type(i),
                .descriptorCount = (i == SNX_VKB_TEXARRAY || i == SNX_VKB_TEXDEPTH) ? g_shader_tex_array : 1,
                .stageFlags = vk_set_stages(s)};
        }
        for (int arrays = 0; arrays < 2; arrays++) {
            u32 n = count;
            while (!arrays && n && b[n - 1].binding >= SNX_VKB_TEXARRAY) n--; /* the arrays are the last bindings */
            VkDescriptorSetLayoutCreateInfo dl = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .flags = g_vk.has_descriptor_buffers ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT : 0,
                .bindingCount = n,
                .pBindings = b};
            if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &dl, NULL, &g_vk_dslayout[arrays][s])))
                fatal("vulkan: cannot create shader descriptor layout");
            vk_descriptors_layout(g_vk_dslayout[arrays][s], b, n);
        }
    }
    /* A compute dispatch does not bind the five graphics sets. Give it the full per-stage image budget instead
       of reusing the vertex layout's share of the whole graphics pipeline budget. */
    {
        const VkPhysicalDeviceLimits* limits = &g_vk.props.limits;
        u32 total = limits->maxPerStageDescriptorStorageImages < limits->maxDescriptorSetStorageImages
                        ? limits->maxPerStageDescriptorStorageImages : limits->maxDescriptorSetStorageImages;
        u32 images = total < 8 ? total : 8;
        g_vk_buffer_image_slots = total - images < 8 ? total - images : 8;
        VkDescriptorSetLayoutBinding bindings[SNX_VKB_TEXARRAY];
        u32 count = 0;
        for (u32 i = 0; i < SNX_VKB_TEXARRAY; i++) {
            if (i == SNX_VKB_CAPTURE) continue;
            if (i >= SNX_VKB_IMG && i < SNX_VKB_RS && i - SNX_VKB_IMG >= images) continue;
            if (i >= SNX_VKB_IMGBUF && i - SNX_VKB_IMGBUF >= g_vk_buffer_image_slots) continue;
            bindings[count++] = (VkDescriptorSetLayoutBinding){i, binding_type(i), 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
        }
        VkDescriptorSetLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                .bindingCount = count, .pBindings = bindings};
        if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &info, NULL, &g_vk.cs_dslayout)))
            fatal("vulkan: cannot create compute descriptor layout");
    }
    {
        VkDescriptorPoolSize n[VK_POOL_TYPES];
        vk_pool_sizes(n, 1, 1); /* a set with the arrays */
        const VkPhysicalDeviceLimits* l = &g_vk.props.limits;
        if (n[0].descriptorCount > l->maxPerStageDescriptorUniformBuffers ||
            n[1].descriptorCount > l->maxPerStageDescriptorStorageBuffers ||
            n[2].descriptorCount > l->maxPerStageDescriptorSamplers ||
            n[2].descriptorCount > l->maxPerStageDescriptorSampledImages ||
            n[3].descriptorCount > l->maxPerStageDescriptorStorageImages ||
            n[0].descriptorCount + n[1].descriptorCount + n[2].descriptorCount + n[3].descriptorCount + 8 >
                l->maxPerStageResources)
            WARN("vulkan: a stage's descriptor set (%u uniform and %u storage buffers, %u textures, %u images) is "
                 "past this GPU's per-stage limits (%u, %u, %u, %u): pipelines may fail\n",
                 n[0].descriptorCount, n[1].descriptorCount, n[2].descriptorCount, n[3].descriptorCount,
                 l->maxPerStageDescriptorUniformBuffers, l->maxPerStageDescriptorStorageBuffers,
                 l->maxPerStageDescriptorSamplers, l->maxPerStageDescriptorStorageImages);
    }
    g_vk_playout = vk_playout(0);
    {
        VkPushConstantRange pc = {VK_PUSH_STAGES, 0, 80};
        VkPipelineLayoutCreateInfo compute = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                              .setLayoutCount = 1,
                                              .pSetLayouts = &g_vk.cs_dslayout,
                                              .pushConstantRangeCount = 1,
                                              .pPushConstantRanges = &pc};
        if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &compute, NULL, &g_vk.cs_playout)))
            fatal("vulkan: cannot create compute pipeline layout");
    }
    if (!g_vk_playout) fatal("vulkan: cannot create shader pipeline layout");
    snprintf(g_pipeline.cache_dir, sizeof g_pipeline.cache_dir, "%sshader_cache", g_cachedir);
    plat_mkdir(g_pipeline.cache_dir);
    /* the BC7 blocks of earlier ASTC encodes, where ASTC textures are held as BC7 (astc.c) */
    if (vk_astc_bc7()) {
        char bc7[1200];
        snprintf(bc7, sizeof bc7, "%s/%s.vkbc7", g_pipeline.cache_dir, g_exename);
        snx_path_native(bc7);
        vk_astc_cache_open(bc7);
    }
    /* each driver's files apart: a game run with the system's driver, then a package's (gpu_driver), then the
       system's again keeps the first one's */
    snx_vk_driver_tag(g_pipeline.driver_tag, sizeof g_pipeline.driver_tag, g_vk.driver_id, g_vk.props.vendorID,
                      g_vk.props.deviceID, g_settings.gpu_driver);
    snx_vk_driver_path(g_pipeline.pcache_path, sizeof g_pipeline.pcache_path, g_pipeline.cache_dir, "vk_pipelines",
                       g_pipeline.driver_tag, ".bin");
    snx_path_native(g_pipeline.pcache_path);
    /* the driver's compiled pipelines from earlier runs, from a file of this device's and driver build's
       (snx_vk_pipeline_cache_new). Without a file of this driver's, the one every driver wrote before each had its
       own, left where it is */
    PFileMap data = {0};
    /* vk_pipeline_cache=0 reads neither file */
    char* legacy = g_settings.vulkan_pipeline_cache ? snx_format("%s/vk_pipelines.bin", g_pipeline.cache_dir) : NULL;
    if (legacy) snx_path_native(legacy);
    const char* from = g_settings.vulkan_pipeline_cache && !plat_file_map(&data, g_pipeline.pcache_path) && data.data
                           ? g_pipeline.pcache_path
                           : NULL;
    if (!from && legacy && !plat_file_map(&data, legacy) && data.data) {
        from = legacy;
        LOG("vulkan: %s, the file every driver shared before each had its own, stays where it is, read as the start "
            "of %s, which is not there yet\n",
            legacy, g_pipeline.pcache_path);
    }
    size_t loaded = 0;
    char why[512];
    g_pipeline.pcache = snx_vk_pipeline_cache_new(g_vk.dev, &g_vk.props, from ? (const void*)data.data : NULL,
                                                  (size_t)data.size, &loaded, why, sizeof why);
    if (why[0]) LOG("vulkan: %s: %s\n", from ? from : g_pipeline.pcache_path, why);
    if (loaded) LOG("vulkan: pipeline cache loaded (%zu KB)\n", loaded >> 10);
    free(legacy);
    plat_file_unmap(&data); /* the cache copied it, and a mapped file can't be replaced on Windows */
    /* how shaders get the console's FP32 flush to zero, settled before the reading thread translates any */
    if (g_vk_flush_denorm)
        g_vk_ftz = SH_FTZ_MODE;
    else if (g_vk_preserve_infnan && vk_ftz_native()) {
        g_vk_ftz = SH_FTZ_NATIVE;
        LOG("vulkan: this GPU flushes FP32 subnormals as the console's FTZ instructions do, so shaders are translated "
            "without a flush of their own\n");
    } else {
        g_vk_ftz = SH_FTZ_EMULATE;
        WARN("vulkan: FP32 denormal flush mode unavailable; graphics shaders use per-instruction FTZ fallback\n");
    }
    pf_start_load();
}

/* ------------------------------------------------------------ shaders */

static u64 modinfo_bits(VkShaderModule m) { /* mod_bits, which comes later */
    u64 v = 0;
    memcpy(&v, &m, sizeof m);
    return v;
}
static u32 modinfo_slot(u64 bits) { return (u32)((bits * 0x9e3779b97f4a7c15ull) >> 50) & (MODINFO_N - 1); }
static void modinfo_put(VkShaderModule m, u64 hash, int stage, size_t words) {
    const u64 bits = modinfo_bits(m);
    if (!bits) return;
    pm_lock(&g_pipeline.modinfo_lock);
    for (u32 n = 0, i = modinfo_slot(bits); n < MODINFO_N; n++, i = (i + 1) & (MODINFO_N - 1))
        if (!g_pipeline.modinfo[i].mod || g_pipeline.modinfo[i].mod == bits) {
            g_pipeline.modinfo[i] = (ModInfo){bits, hash, (u32)stage, words > 0xffffffffu ? 0xffffffffu : (u32)words};
            break;
        }
    pm_unlock(&g_pipeline.modinfo_lock);
}
static int modinfo_get(VkShaderModule m, ModInfo* out) {
    const u64 bits = modinfo_bits(m);
    int found = 0;
    if (!bits) return 0;
    pm_lock(&g_pipeline.modinfo_lock);
    for (u32 n = 0, i = modinfo_slot(bits); n < MODINFO_N && g_pipeline.modinfo[i].mod && !found;
         n++, i = (i + 1) & (MODINFO_N - 1))
        if (g_pipeline.modinfo[i].mod == bits) {
            *out = g_pipeline.modinfo[i];
            found = 1;
        }
    pm_unlock(&g_pipeline.modinfo_lock);
    return found;
}

/* the program's own bytes: what the GPU holds beside them on their pages stays there */
static int code_hash(u64 gva, u32 len, u64* hash) {
    u8* t = malloc(len ? len : 1);
    if (!t) return 0;
    if (gmem_read_code_cpu(gva, t, len) < len) gmem_read_code(gva, t, len);
    *hash = vk_hash(t, len);
    free(t);
    return 1;
}
/* Read only the prefix needed to find the program's end. A 128 KiB coherence request can otherwise land an
   unrelated storage image beside a tiny shader and wait for the GPU. Hash against the full zero-padded window so
   forward branch targets still prevent an early EXIT from ending the scan. Reaching a prefix boundary grows it. */
void vk_shader_read_code(u64 gva, u8* code, u32* len, VkCbReader read) {
    /* an SM86 program's end is found by decoding it (sm86_find_programs), so a prefix too short to hold it would be
       decoded again at each doubling: its window whole at once, as feat/ounce reads it */
    u32 prefix = g_shader_sm86 ? SH_CODE : 256;
    memset(code, 0, SH_CODE);
    for (;;) {
        u64 cpu = gmem_read_code_cpu(gva, code, prefix);
        if (prefix < SH_CODE && shader_code_full_window(code, prefix)) {
            /* BRX proof depends on incoming branches that can be beyond the prefix. Keep its full-window path. */
            prefix = SH_CODE;
            continue;
        }
        vk_shader_hash(code, SH_CODE, len, read);
        if (gmem_code_cut(*len, cpu, prefix)) {
            gmem_read_code(gva, code, prefix);
            if (prefix < SH_CODE && shader_code_full_window(code, prefix)) {
                /* The coherent read can expose BRX that the CPU view did not contain. */
                prefix = SH_CODE;
                continue;
            }
            vk_shader_hash(code, SH_CODE, len, read);
        }
        if (prefix == SH_CODE || *len + 16 < prefix) return;
        prefix *= 2;
    }
}
static int code_same(const ShA* ca, u64 gva, u32 len, u64 qh) {
    gmem_need(gva, len); /* pending results over the code land first, as the write tracking then sees them */
    if (!gpu_mem_dirty_since(ca->gw, gva, len) && vk_written_since(gmem_host(gva, len), len, ca->wseen) == 0) return 1;
    u64 hash;
    return code_hash(gva, len, &hash) && hash == qh;
}

/* what a shader's key words (vk_shader_get's mask) tell its translation, into si zeroed */
static void sh_key_info(ShaderInfo* si, int stage, const u32 mask[26], const ShXfb* xfb) {
    u64 ints = mask[0] | (u64)mask[1] << 32, sints = mask[2] | (u64)mask[3] << 32;
    memset(si, 0, sizeof *si);
    si->flow_base = mask[20];
    si->advanced_blend = mask[21] | (u64)mask[22] << 32;
    si->advanced_blend_mask = mask[23];
    si->coverage_to_color = (mask[23] & SH_COVERAGE_TO_COLOR) ? 1 + ((mask[23] >> SH_COVERAGE_OUTPUT_SHIFT) & 7) : 0;
    si->coverage_early_tests = si->coverage_to_color && (mask[23] & SH_BLEND_EARLY_TESTS);
    si->img_int_mask = mask[24] & 255;
    si->img_sint_mask = mask[25];
    if (stage == ST_FS) {
        si->late_depth = (mask[24] >> 8) & SH_FS_LATE_DEPTH;
        si->capture_outputs = (mask[24] >> (8 + SH_FS_CAPTURE_SHIFT)) & 7;
        si->varying = mask[24] >> (8 + SH_FS_VARYING_SHIFT);
    }
    if (stage == ST_FS) {
        si->tex_int_mask = ints;
        si->tex_sint_mask = sints;
        si->rt_int_mask = mask[4];
        si->rt_sint_mask = mask[5];
        si->tex_dc_color_mask = mask[7] | (u64)mask[8] << 32;
        si->tex_dc_color_funcs = mask[9];
        si->in_unfed_mask = mask[10];
        si->early_fragment_tests = mask[11] & 1;
        si->tex_minmax_mask = mask[11] >> 1 & ((1u << SH_MINMAX_SLOTS) - 1);
        si->tex_minmax_max = mask[11] >> (1 + SH_MINMAX_SLOTS) & ((1u << SH_MINMAX_SLOTS) - 1);
        si->tex_rect_mask = mask[12] | (u64)mask[13] << 32;
        shader_query_hint_unpack(mask + 14, si->tex_query_hint);
        si->prim_mask = mask[6]; /* the generics the draw's mesh program writes per primitive (vk_shader_mesh) */
    } else if (stage == ST_TS || stage == ST_MS) {
        /* vk_shader_mesh's words: the per-primitive SPH (word 3) is read from the code (sh_mesh_sph) */
        si->mesh.threads = (u16)mask[0];
        si->mesh.payload = (u16)(mask[0] >> 16);
        si->mesh.max_vertices = (u16)mask[1];
        si->mesh.max_primitives = (u16)(mask[1] >> 16);
        si->mesh.has_task = (u8)mask[2];
        si->mesh.primitive_vertices = (u8)(mask[2] >> 8);
        si->tex_rect_mask = mask[12] | (u64)mask[13] << 32;
        shader_query_hint_unpack(mask + 14, si->tex_query_hint);
    } else {
        si->attr_int_mask = ints;
        si->attr_sint_mask = sints;
        if (stage == ST_VS) si->position_copy = mask[24] >> 8;
        /* a vertex program's integer textures, in the words of the fragment stage's dc_funcs and unfed */
        si->tex_int_mask = mask[9];
        si->tex_sint_mask = mask[10];
        si->rt_int_mask = mask[4]; /* the tessellation or geometry primitive */
        si->clip_enable = mask[6];
        if (stage == ST_GS) {
            memcpy(si->gp_skip_mask, mask + 12, sizeof si->gp_skip_mask);
        } else {
            si->tex_rect_mask = mask[12] | (u64)mask[13] << 32;
            shader_query_hint_unpack(mask + 14, si->tex_query_hint);
        }
        if (xfb) si->xfb = *xfb;
    }
}
/* a mesh program's per-primitive SPH, at the offset that its key's word 3 gives (vk_shader_mesh), into si. The offset
   counts from the start of the program's code area, its own SPH included, as code holds it: a code area of 0xe00
   bytes has it at 0xd00, one of 0x4900 bytes at 0x4800 (docs/platforms/nx/ounce.md) */
static void sh_mesh_sph(ShaderInfo* si, int stage, const u8* code, const u32 mask[26]) {
    if (stage != ST_MS || !mask[3] || mask[3] > SH_CODE - sizeof si->mesh.prim_sph) return;
    const u8* at = code + mask[3];
    for (u32 i = 0; i < sizeof si->mesh.prim_sph / 4; i++) si->mesh.prim_sph[i] = snx_ld_le32(at + 4 * i);
}
/* s for its key, from code (SH_CODE bytes: the merged program, for VertexA and VertexB) whose whole code hashes to h,
   each part with its length and hash, translated: its SPIR-V (malloc'd, nw words), the constant buffers the
   translation read through read in s->cbr. NULL when it can't be (si.first_unsupported says why). shipped: a program
   the game ships, translated before any draw (ship_make), which fails when its translation read a constant buffer
   word that its own constants don't hold: a draw's lookup checks the reads against its own buffers, which a read
   that failed never matches */
static u32* sh_spirv(VkSh* s, const u8* code, u64 h, int stage, const u32 mask[26], u32 len, u64 qh, u32 len_a,
                     u64 qh_a, const ShXfb* xfb, VkCbReader read, int shipped, size_t* nw) {
    s->shipped = shipped;
    s->hash = h;
    s->stage = stage;
    memcpy(s->mask, mask, sizeof s->mask);
    s->clen = len;
    s->qh = qh;
    s->clen_a = len_a;
    s->qh_a = qh_a;
    sh_key_info(&s->si, stage, mask, xfb);
    sh_mesh_sph(&s->si, stage, code, mask);
    *nw = 0;
    SNX_ZONE(z_translate, "shader translate");
    SNX_ZONE_TEXT(z_translate, "%s hash %08x",
                  stage >= 0 && stage < (int)(sizeof vk_tracy_stage / sizeof *vk_tracy_stage) ? vk_tracy_stage[stage]
                                                                                              : "stage?",
                  (unsigned)h);
    u32* spv = vk_translate_spirv(code, SH_CODE, stage, &s->si, nw, read, &s->cbr);
    SNX_ZONE_VALUE(z_translate, *nw);
    SNX_ZONE_END(z_translate);
    for (int i = 0; i < s->si.ntex; i++) s->indexed |= (s->si.tex[i].flags & SNX_TEX_INDEXED) != 0;
    int unread = 0;
    for (u32 i = 0; shipped && i < s->cbr.n; i++) unread |= !s->cbr.r[i].ok;
    if (spv && unread) {
        free(spv);
        spv = NULL;
        snprintf(s->si.first_unsupported, sizeof s->si.first_unsupported, "a jump table outside the program constants");
    }
    if (spv && s->si.nimg) {
        /* by the stage whose set the program takes: a mesh program the vertex stage's, a task program the
           tessellation control stage's (vk_set_stages) */
        u32 image_slots = g_vk.image_slots[stage == ST_MS ? ST_VS : stage == ST_TS ? ST_TCS : stage];
        int needs_unformatted = 0;
        for (int i = 0; i < s->si.nimg; i++) needs_unformatted |= s->si.img[i].kind != 1;
        /* All stages before fragment shading use vertexPipelineStoresAndAtomics. */
        if ((stage != ST_FS && !g_vk.feat.vertexPipelineStoresAndAtomics) ||
            (u32)s->si.nimg > image_slots || (needs_unformatted && !g_vk.feat.shaderStorageImageWriteWithoutFormat)) {
            free(spv);
            spv = NULL;
            snprintf(s->si.first_unsupported, sizeof s->si.first_unsupported,
                     "storage image stage, feature or descriptor limit");
        }
    }
    return spv;
}
/* A draw's shader (sh_spirv's arguments, and mlen, the bytes of code the translation reads): translated, its module
   made, kept in the shader file with its program. One that couldn't be translated comes back failed; NULL when out
   of memory */
static VkSh* sh_make(const u8* code, u32 mlen, u64 h, int stage, const u32 mask[26], u32 len, u64 qh, u32 len_a,
                     u64 qh_a, const ShXfb* xfb, VkCbReader read) {
    size_t nw;
    VkSh* s = calloc(1, sizeof *s);
    if (!s) return NULL;
    SNX_ZONE(z, "shader translate");
    u32* spv = sh_spirv(s, code, h, stage, mask, len, qh, len_a, qh_a, xfb, read, 0, &nw);
    SNX_ZONE_END(z);
    if (spv) {
        VkShaderModuleCreateInfo mi = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = spv};
        if (!VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &s->mod))) {
            s->failed = 1;
            snprintf(s->si.first_unsupported, sizeof s->si.first_unsupported, "vkCreateShaderModule failed");
        } else {
            modinfo_put(s->mod, h, stage, nw);
            pf_shader_record(s, spv, nw, code, src_bytes(mlen), xfb);
        }
        free(spv);
    } else
        s->failed = 1;
    /* an Ounce program's texture handles that it loads at addresses that differ from invocation to invocation: no
       texture each (SH_PROBE_NONE), which the log says once a program */
    /* TEX.B instructions that choose among the constant buffer handles that reach them by the handle's value: one
       equal to none samples zeros, which the log says once a program */
    if (!s->failed && s->si.chosen_tex)
        LOG_LIMIT(64,
                  "vulkan: program %016llx chooses the texture of %u TEX.B%s when it runs, among the constant buffer "
                  "handles that reach it: a handle equal to none of them samples (0, 0, 0, 0) (%s)\n",
                  (unsigned long long)h, s->si.chosen_tex, s->si.chosen_tex == 1 ? "" : "s", s->si.chosen_why);
    if (!s->failed && s->si.memh_none)
        LOG_LIMIT(64, "vulkan: program %016llx samples %u texture%s as none, whose handles it loads from memory at "
                      "addresses no draw gives (%s)\n",
                  (unsigned long long)h, s->si.memh_none, s->si.memh_none == 1 ? "" : "s", s->si.memh_why);
    /* a mesh program's outputs against maxMeshOutputMemorySize, which a pipeline must not go past: such a program
       fails, and its draws are skipped with the reason (mesh_shaders) */
    if (stage == ST_MS && !s->failed) {
        ShaderInfo* si = &s->si;
        u64 bytes = sm86_mesh_output_bytes(si->sph, &si->mesh, g_vk.mesh_vertex_granule, g_vk.mesh_primitive_granule);
        int over = g_vk.mesh_output_bytes_max && bytes > g_vk.mesh_output_bytes_max;
        LOG("vulkan: mesh program %016llx: %u vertices, %u primitives, generics %#x (%#x per primitive), %llu "
            "output bytes of %u%s\n",
            (unsigned long long)h, si->mesh.max_vertices, si->mesh.max_primitives, si->out_mask, si->prim_mask,
            (unsigned long long)bytes, g_vk.mesh_output_bytes_max, over ? ": too many, its draws are skipped" : "");
        if (over) {
            s->failed = 1;
            snprintf(si->first_unsupported, sizeof si->first_unsupported,
                     "%llu bytes of mesh outputs, past the device's maxMeshOutputMemorySize of %u",
                     (unsigned long long)bytes, g_vk.mesh_output_bytes_max);
        }
    }
    return s;
}
VkSh* vk_shader_get(const DsState* ds, u64 gva, u64 gva_a, int stage, u64 int_mask, u64 sint_mask, u32 rt_int,
                    u32 rt_sint, u32 clip, u64 dc_color, u32 dc_funcs, u32 unfed, u64 rect, const ShXfb* xfb,
                    const u8* query_hint, u64 advanced_blend, u32 advanced_mask, u32 img_int, u32 img_sint) {
    return vk_shader_get_ex(ds, gva, gva_a, stage, int_mask, sint_mask, rt_int, rt_sint, clip, dc_color, dc_funcs,
                            unfed, rect, xfb, query_hint, advanced_blend, advanced_mask, img_int, img_sint, 0, 0);
}
VkSh* vk_shader_get_ex(const DsState* ds, u64 gva, u64 gva_a, int stage, u64 int_mask, u64 sint_mask, u32 rt_int,
                       u32 rt_sint, u32 clip, u64 dc_color, u32 dc_funcs, u32 unfed, u64 rect, const ShXfb* xfb,
                       const u8* query_hint, u64 advanced_blend, u32 advanced_mask, u32 img_int, u32 img_sint,
                       u32 fs_config, u32 minmax) {
    /* a vertex program's is its position's copy (ShaderInfo.position_copy) */
    if ((fs_config & ~SH_FS_CONFIG_MASK) || (fs_config && stage != ST_FS && (stage != ST_VS || fs_config > 32)))
        return NULL;
    if ((minmax >> 2 * SH_MINMAX_SLOTS) || (minmax && stage != ST_FS)) return NULL;
    const DsPrograms* p = ds ? &ds->prog : NULL;
    pf_merge();
    g_pipeline.sh_ds = ds;
    g_pipeline.sh_bind = sh_bind_stage(stage);
    u32 mask[26] = {(u32)int_mask,
                    (u32)(int_mask >> 32),
                    (u32)sint_mask,
                    (u32)(sint_mask >> 32),
                    rt_int,
                    rt_sint,
                    clip,
                    (u32)dc_color,
                    (u32)(dc_color >> 32),
                    dc_funcs,
                    unfed};
    mask[24] = (img_int & 255) | fs_config << 8;
    mask[25] = img_sint;
    if (stage == ST_FS && p) mask[11] = p->early_z;
    /* the min/max textures above the early tests' bit, in a word every key has */
    if (stage == ST_FS) mask[11] |= minmax << 1;
    if (stage == ST_FS) {
        mask[21] = (u32)advanced_blend;
        mask[22] = (u32)(advanced_blend >> 32);
        mask[23] = advanced_mask;
    }
    if (stage == ST_GS && p) memcpy(mask + 12, p->gs_skip, 8 * sizeof(u32));
    /* the rectangle textures, and the dimensions of the textures only a TXQ names (ShaderInfo.tex_query_hint,
       3 bits a slot), in words only the geometry stage's key uses otherwise */
    int queried = 0;
    if (stage != ST_GS) {
        mask[12] = (u32)rect;
        mask[13] = (u32)(rect >> 32);
        if (query_hint) shader_query_hint_pack(query_hint, mask + 14);
        for (int i = 14; i < 14 + SH_QUERY_WORDS; i++) queried |= mask[i] != 0;
    }
    /* a capture's layout, in the words only the fragment stage's key uses otherwise */
    if (stage != ST_FS && xfb) {
        u64 xh = vk_hash(xfb, sizeof *xfb);
        mask[7] = (u32)xh | 1;
        mask[8] = (u32)(xh >> 32);
    }
    u32 mix = (u32)vk_hash(mask, 12 * sizeof(u32));
    mix ^= (u32)vk_hash(mask + 21, 5 * sizeof(u32));
    if (stage == ST_GS) mix ^= (u32)vk_hash(mask + 12, 8 * sizeof(u32));
    if (stage != ST_GS && rect) mix ^= (u32)vk_hash(mask + 12, 2 * sizeof(u32));
    if (queried) mix ^= (u32)vk_hash(mask + 14, SH_QUERY_WORDS * sizeof(u32));
    u64 hk = (gva * 0x9E3779B97F4A7C15ull) ^ (gva_a * 0xC2B2AE3D27D4EB4Full) ^ ((u64)mix << 24) ^ (u64)stage;
    ShA* set = &g_pipeline.sha[vk_sha_set(hk) * VK_SHA_WAYS];
    ShA* ca = NULL;
    u32 epoch = vk_ww_epoch(); /* before the code is checked or read */
    for (int w = 0; w < VK_SHA_WAYS && !ca; w++)
        if (set[w].s && set[w].gva == gva && set[w].gva_a == gva_a && set[w].stage == stage &&
            !memcmp(set[w].mask, mask, 20 * sizeof *mask) && !memcmp(set[w].mask + 21, mask + 21, 5 * sizeof *mask))
            ca = &set[w];
    if (ca) {
        VkSh* s = ca->s;
        int same = (ca->epoch == epoch && ca->shader_generation == g_vk.shader_generation) ||
                   (code_same(ca, gva, s->clen, s->qh) && (!gva_a || code_same(ca, gva_a, s->clen_a, s->qh_a)));
        if (same && (ca->epoch != epoch || ca->shader_generation != g_vk.shader_generation)) {
            ca->shader_generation = g_vk.shader_generation;
            ca->epoch = epoch;
            ca->wseen = vk_ww_now();
            ca->gw = gpu_mem_generation();
        }
        if (same && vk_cb_reads_hold(&s->cbr, sh_cb_read)) return s;
    } else { /* an empty way, else the one used longest ago */
        ca = &set[0];
        for (int w = 1; w < VK_SHA_WAYS && ca->s; w++)
            if (!set[w].s || (i32)(set[w].epoch - ca->epoch) < 0) ca = &set[w];
    }
    u32 wseen = vk_ww_now();
    u64 gw = gpu_mem_generation();
    u8* code = malloc(SH_CODE);
    if (!code) return NULL;
    u32 len, len_a = 0;
    u64 qh, qh_a = 0;
    vk_shader_read_code(gva, code, &len, sh_cb_read);
    qh = vk_hash(code, len);
    if (gva_a) { /* VertexA + VertexB: one merged program */
        u8* a = malloc(SH_CODE);
        if (!a) {
            free(code);
            return NULL;
        }
        vk_shader_read_code(gva_a, a, &len_a, sh_cb_read);
        qh_a = vk_hash(a, len_a);
        u8* m = malloc(SH_CODE);
        if (!m) {
            free(a);
            free(code);
            return NULL;
        }
        if (sh_merge_dual(a, code, m, sh_cb_read)) {
            free(code);
            code = m;
        } else {
            free(m);
            WARN_LIMIT(8, "vulkan: VertexA %llx and VertexB %llx couldn't be merged: VertexB runs alone\n",
                       (unsigned long long)gva_a, (unsigned long long)gva);
        }
        free(a);
    }
    /* an SM86 program, never merged, is hashed as vk_shader_read_code hashed it: its decode once */
    u32 mlen = len;
    u64 h = g_shader_sm86 && !gva_a ? sm86_hash(code, SH_CODE, len) : vk_shader_hash(code, SH_CODE, &mlen, sh_cb_read);
    mask[20] = shader_absolute_flow(code, mlen) ? (u32)gva : 0;
    VkSh* s = NULL;
    /* one made for the same program: its whole code hashes the same (the merged program's, and each part's with its
       length, which ShA's checks compare memory with later), with the same key and constant buffer reads. Shaders of
       earlier runs (pf_merge) are found only so */
    for (VkSh* q = g_pipeline.shh[h & 1023]; q && !s; q = q->next)
        if (q->hash == h && q->stage == stage && !memcmp(q->mask, mask, sizeof mask) && q->clen == len && q->qh == qh &&
            q->clen_a == len_a && q->qh_a == qh_a && vk_cb_reads_hold(&q->cbr, sh_cb_read))
            s = q;
    if (s && s->shipped == 1) pf_ship_found(s);
    if (!s && (s = pf_ship_take(h, stage, mask, len, qh, len_a, qh_a)) != NULL) {
        s->next = g_pipeline.shh[h & 1023];
        g_pipeline.shh[h & 1023] = s;
    }
    if (!s) {
        if (vk_corpus_dir()) { /* the key's words as sh_make takes them into si, which the corpus keeps */
            ShaderInfo si;
            sh_key_info(&si, stage, mask, xfb);
            sh_mesh_sph(&si, stage, code, mask);
            shader_corpus_save(vk_corpus_dir(), code, mlen, SH_CODE, stage, &si, h ^ mix);
        }
        u64 t0 = plat_ns();
        s = sh_make(code, mlen, h, stage, mask, len, qh, len_a, qh_a, xfb, sh_cb_read);
        if (!s) {
            free(code);
            return NULL;
        }
        if (s->failed)
            WARN("vulkan: shader %016llx (stage %d) not translated: %s\n", (unsigned long long)h, stage,
                 s->si.first_unsupported[0] ? s->si.first_unsupported : "unsupported feature or float controls");
        g_pipeline.sh_new++;
        g_pipeline.sh_ns += plat_ns() - t0;
        s->next = g_pipeline.shh[h & 1023];
        g_pipeline.shh[h & 1023] = s;
    }
    free(code);
    ca->gva = gva;
    ca->gva_a = gva_a;
    memcpy(ca->mask, mask, sizeof mask);
    ca->stage = stage;
    ca->epoch = epoch;
    ca->wseen = wseen;
    ca->gw = gw;
    ca->shader_generation = g_vk.shader_generation;
    ca->s = s;
    return s;
}

VkSh* vk_shader_mesh(const DsState* ds, u64 gva, int stage, const DsMeshKey* k) {
    int mesh = stage == ST_MS;
    u32 w0 = mesh ? k->mesh_threads | (u32)k->mesh_payload << 16 : k->task_threads | (u32)k->task_payload << 16;
    u32 w1 = mesh ? k->max_vertices | (u32)k->max_primitives << 16 : 0;
    u32 w2 = mesh ? k->has_task | (u32)k->primitive_vertices << 8 : 0, w3 = mesh ? k->prim_sph : 0;
    return vk_shader_get(ds, gva, 0, stage, w0 | (u64)w1 << 32, w2 | (u64)w3 << 32, 0, 0, 0, 0, 0, 0, 0, NULL, NULL, 0,
                         0, 0, 0);
}
/* whether the constant buffer words s's translation read (BRX jump tables) still hold in ds's bound buffers, as
   vk_shader_get checks them on each hit */
int vk_shader_cb_hold(const DsState* ds, const VkSh* s) {
    g_pipeline.sh_ds = ds;
    g_pipeline.sh_bind = sh_bind_stage(s->stage);
    return vk_cb_reads_hold(&s->cbr, sh_cb_read);
}

VkShaderModule vk_module_spv(const u32* spv, size_t nw) {
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = spv};
    VkShaderModule m = VK_NULL_HANDLE;
    VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &m));
    return m;
}
VkShaderModule vk_module_spv_location(const u32* source, size_t nw, u32 location) {
    u32* spv = malloc(nw * sizeof *spv);
    if (!spv) return VK_NULL_HANDLE;
    memcpy(spv, source, nw * sizeof *spv);
    int found = 0;
    for (size_t i = 5; i < nw;) {
        u32 count = spv[i] >> 16, op = spv[i] & 0xffffu;
        if (!count || i + count > nw) break;
        if (op == 71 && count == 4 && spv[i + 2] == 30) {
            spv[i + 3] = location;
            found++;
        }
        i += count;
    }
    VkShaderModule result = found == 1 ? vk_module_spv(spv, nw) : VK_NULL_HANDLE;
    free(spv);
    return result;
}
/* an empty fragment shader for depth-only passes */
static VkShaderModule null_fs(void) {
    if (!g_pipeline.null_fs)
        g_pipeline.null_fs = vk_module_spv(snx_null_fs_spv, snx_null_fs_spv_size / sizeof *snx_null_fs_spv);
    return g_pipeline.null_fs;
}

/* ------------------------------------------------------------ pipelines */
/* A draw's new pipeline is made on the worker threads (pipeline_threads, snx_vk_pipeline_workers): the GPU thread hands
   the recording thread a stand-in for it and decodes on, and the recording thread waits for the pipeline at its bind
   (vk_pipeline_resolve; without that thread, vk_record=0, the draw waits on the GPU thread). The shader file's
   pipelines (pf_merge) are made on the workers from startup, and a draw that asks for one before then waits for it
   too, at the head of the workers' queue. Skipping a draw whose pipeline was still being made, as Eden's asynchronous
   shaders do, left
   one game's menus over a black backdrop and its loading screen without its map whenever their one draw was
   skipped, and failed 190 of edenprobe's cases: a draw is never skipped for its pipeline. What a worker needs that is
   made lazily, the layout and the empty fragment shader, is made on the GPU thread first */

static void pf_pipe_record(const PipeKey* k);

/* conservative rasterization overestimates, as the 3D engine's does, by the dilation asked for (PipeKey.conservative:
   1 plus quarter pixels) up to the device's most */
float vk_conservative_extra(u32 conservative) {
    float extra = conservative > 1 ? (float)(conservative - 1) * 0.25f : 0.0f;
    return extra < g_vk.conservative_extra ? extra : g_vk.conservative_extra;
}
/* Everything a pipeline of a key is made of, filled in place by pipe_info (its members point at each other). A whole
   pipeline takes all of it (pipe_create), a library part what that part holds (lib_create). A part is made from the
   key of the first pipeline that needed it, and holds only what its own key has of it (vk_pipe_split): the rest is
   another part's, or dynamic state, which the part ignores */
typedef struct {
    VkPipelineShaderStageCreateInfo st[5];
    u32 nst, npr; /* the stages, the first npr of them before rasterization and then the fragment one */
    /* the fragment module's specialization constants (k->fs_spec), by SpecId */
    u32 spec_values[SH_SPEC_COUNT];
    VkSpecializationMapEntry spec_entries[SH_SPEC_COUNT];
    VkSpecializationInfo spec;
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo warp; /* a mesh pipeline's mesh and task stages' */
    /* with descriptor heaps (k->heaps), each stage's mapping of its set's bindings */
    VkShaderDescriptorSetAndBindingMappingInfoEXT mappings[5];
    VkDescriptorSetAndBindingMappingEXT binding_maps[5][SNX_VKB_END];
    VkPipelineTessellationStateCreateInfo ts;
    VkVertexInputBindingDescription vb[33];
    VkVertexInputBindingDivisorDescriptionEXT dv[33];
    VkVertexInputAttributeDescription va[32];
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvi;
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationConservativeStateCreateInfoEXT conservative;
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provoking;
    VkPipelineRasterizationStateCreateInfo rs;
    VkSampleMask sample_mask;
    float coverage_table[16];
    VkPipelineCoverageModulationStateCreateInfoNV coverage;
    VkSampleLocationEXT positions[16];
    VkPipelineSampleLocationsStateCreateInfoEXT locations; /* with k->sample_locations */
    VkPipelineCoverageToColorStateCreateInfoNV to_color;   /* with k->coverage_to_color */
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState cba[8];
    VkPipelineColorBlendAdvancedStateCreateInfoEXT advanced; /* with a native advanced equation (k->advanced) */
    VkPipelineColorBlendStateCreateInfo cb;
    VkFormat cf[8];
    VkSampleCountFlagBits color_samples[8];
    VkAttachmentSampleCountInfoNV attachment_samples; /* with mixed samples (coverage.raster_samples) */
    VkPipelineRenderingCreateInfo ri;
    VkPipelineRobustnessCreateInfoEXT robustness; /* with has_vertex_robustness2 */
    VkPipelineCreationFeedback made;
    VkPipelineCreationFeedbackCreateInfo fi;
    PipeDynIn dyn;
    VkPipelineCreateFlags flags; /* the descriptor buffers' and the feedback loop's */
} PipeInfo;
static void pipe_info(const PipeKey* k, VkShaderModule fs, PipeInfo* o) {
    static const VkShaderStageFlagBits bits[5] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                                  VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                                  VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    /* a mesh pipeline's mesh and task shaders, in the vertex and tessellation control slots: their programs' lanes
       are a warp's, so their subgroups are 32 invocations, whole (sm86.h) */
    static const VkShaderStageFlagBits mesh_bits[5] = {VK_SHADER_STAGE_MESH_BIT_EXT, VK_SHADER_STAGE_TASK_BIT_EXT, 0, 0,
                                                       VK_SHADER_STAGE_FRAGMENT_BIT};
    const VkShaderModule modules[5] = {k->vs, k->tcs, k->tes, k->gs, fs};
    memset(o, 0, sizeof *o);
    o->warp = (VkPipelineShaderStageRequiredSubgroupSizeCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = 32};
    for (u32 s = 0; s < 5; s++) {
        if (!modules[s]) continue;
        o->st[o->nst] = (VkPipelineShaderStageCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                        .stage = k->mesh ? mesh_bits[s] : bits[s],
                                                        .module = modules[s],
                                                        .pName = "main"};
        if (k->mesh && s < 2) {
            o->st[o->nst].pNext = &o->warp;
            /* a workgroup of fewer invocations than a warp is one subgroup, its lanes the first ones */
            if (k->mesh & PIPE_MESH_WHOLE(s ? ST_TS : ST_MS))
                o->st[o->nst].flags = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
        }
        if (k->heaps) {
            u32 set = (u32)shader_vk_set((int)s);
            o->mappings[o->nst] = (VkShaderDescriptorSetAndBindingMappingInfoEXT){
                .sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
                .pNext = o->st[o->nst].pNext, /* a mesh stage's required subgroup size */
                .mappingCount = vk_descriptor_heaps_mapping(g_vk_dslayout[(k->arrays >> set) & 1][set], set,
                                                             o->binding_maps[o->nst], SNX_VKB_END),
                .pMappings = o->binding_maps[o->nst]};
            o->st[o->nst].pNext = &o->mappings[o->nst];
        }
        if (s == 4 && k->fs_spec) {
            /* a constant the module does not declare changes nothing in it, so each is given whatever the module */
            vk_pipe_fs_spec_values(k->fs_spec, o->spec_values);
            for (u32 id = 0; id < SH_SPEC_COUNT; id++)
                o->spec_entries[id] = (VkSpecializationMapEntry){id, id * (u32)sizeof(u32), sizeof(u32)};
            o->spec = (VkSpecializationInfo){SH_SPEC_COUNT, o->spec_entries, sizeof o->spec_values, o->spec_values};
            o->st[o->nst].pSpecializationInfo = &o->spec;
        }
        o->nst++;
        if (s < 4) o->npr = o->nst;
    }
    o->ts = (VkPipelineTessellationStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
                                                    .patchControlPoints = k->patch};
    /* the key's layout: none with dynamic vertex input, which the draws set */
    const VertexLayout* l = &k->vi;
    u32 ndv = 0;
    for (u32 i = 0; i < l->nbind; i++) {
        o->vb[i] = (VkVertexInputBindingDescription){.binding = i,
                                                     .stride = l->bind[i].stride, /* 0: dynamic */
                                                     .inputRate = l->bind[i].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE
                                                                                     : VK_VERTEX_INPUT_RATE_VERTEX};
        if (l->bind[i].divisor > 1 && g_vk.has_divisor)
            o->dv[ndv++] = (VkVertexInputBindingDivisorDescriptionEXT){i, l->bind[i].divisor};
    }
    for (u32 i = 0; i < l->nattr; i++)
        o->va[i] = (VkVertexInputAttributeDescription){l->attr[i].loc, l->attr[i].binding,
                                                       (VkFormat)l->attr[i].format, l->attr[i].offset};
    o->dvi = (VkPipelineVertexInputDivisorStateCreateInfoEXT){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
        .vertexBindingDivisorCount = ndv,
        .pVertexBindingDivisors = o->dv};
    o->vi = (VkPipelineVertexInputStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                   .pNext = ndv ? &o->dvi : NULL,
                                                   .vertexBindingDescriptionCount = l->nbind,
                                                   .pVertexBindingDescriptions = o->vb,
                                                   .vertexAttributeDescriptionCount = l->nattr,
                                                   .pVertexAttributeDescriptions = o->va};
    o->ia = (VkPipelineInputAssemblyStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                     .topology = (VkPrimitiveTopology)k->topo_class};
    o->vp = (VkPipelineViewportStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    o->conservative = (VkPipelineRasterizationConservativeStateCreateInfoEXT){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT,
        .conservativeRasterizationMode = VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT,
        .extraPrimitiveOverestimationSize = vk_conservative_extra(k->conservative)};
    o->provoking = (VkPipelineRasterizationProvokingVertexStateCreateInfoEXT){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
        .pNext = k->conservative ? &o->conservative : NULL,
        .provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT};
    o->rs = (VkPipelineRasterizationStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                     .pNext = k->provoking_last ? (void*)&o->provoking
                                                              : k->conservative ? (void*)&o->conservative
                                                                                : NULL,
                                                     .depthClampEnable = k->depth_clamp,
                                                     .polygonMode = (VkPolygonMode)k->polygon,
                                                     .lineWidth = 1.0f};
    /* a multisampled draw's samples: those of its targets, the ones the mask leaves, alpha to coverage and to one.
       A program that reads its sample (SampleId) or interpolates at it is shaded per sample, as the console shades
       it: SPIR-V's rules make that so whatever sampleShadingEnable says */
    o->sample_mask = ~k->sample_off;
    u32 coverage_count =
        k->coverage.raster_samples ? vk_coverage_table(&k->coverage, k->samples, o->coverage_table) : 0;
    o->coverage = (VkPipelineCoverageModulationStateCreateInfoNV){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COVERAGE_MODULATION_STATE_CREATE_INFO_NV,
        .coverageModulationMode = (VkCoverageModulationModeNV)k->coverage.mode,
        .coverageModulationTableEnable = k->coverage.table_enable != 0,
        .coverageModulationTableCount = k->coverage.table_enable ? coverage_count : 0,
        .pCoverageModulationTable = o->coverage_table};
    u32 raster_samples = k->coverage.raster_samples ? k->coverage.raster_samples : k->samples;
    o->locations = (VkPipelineSampleLocationsStateCreateInfoEXT){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT,
        .pNext = k->coverage.raster_samples ? &o->coverage : NULL,
        .sampleLocationsEnable = k->sample_locations.samples != 0,
        .sampleLocationsInfo = vk_sample_locations_info(&k->sample_locations, o->positions)};
    o->to_color = (VkPipelineCoverageToColorStateCreateInfoNV){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COVERAGE_TO_COLOR_STATE_CREATE_INFO_NV,
        .pNext = k->sample_locations.samples ? (void*)&o->locations
                 : k->coverage.raster_samples ? (void*)&o->coverage
                                              : NULL,
        .coverageToColorEnable = k->coverage_to_color != 0,
        .coverageToColorLocation = k->coverage_to_color ? k->coverage_to_color - 1 : 0};
    o->ms = (VkPipelineMultisampleStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = k->coverage_to_color          ? (void*)&o->to_color
                 : k->sample_locations.samples ? (void*)&o->locations
                 : k->coverage.raster_samples  ? (void*)&o->coverage
                                               : NULL,
        .rasterizationSamples = raster_samples ? (VkSampleCountFlagBits)raster_samples : VK_SAMPLE_COUNT_1_BIT,
        .pSampleMask = &o->sample_mask,
        .sampleShadingEnable = k->sample_shading != 0,
        .minSampleShading = k->sample_shading ? 1.0f : 0.0f,
        .alphaToCoverageEnable = k->alpha_to_coverage != 0,
        .alphaToOneEnable = k->alpha_to_one != 0};
    o->ds = (VkPipelineDepthStencilStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    int changed = 0;
    for (u32 i = 0; i < k->ncolor; i++) {
        VkPipelineColorBlendAttachmentState* a = &o->cba[i];
        *a = (VkPipelineColorBlendAttachmentState){.blendEnable = k->blend[i].en,
                                                   .srcColorBlendFactor = (VkBlendFactor)k->blend[i].sc,
                                                   .dstColorBlendFactor = (VkBlendFactor)k->blend[i].dc,
                                                   .colorBlendOp = (VkBlendOp)k->blend[i].cop,
                                                   .srcAlphaBlendFactor = (VkBlendFactor)k->blend[i].sa,
                                                   .dstAlphaBlendFactor = (VkBlendFactor)k->blend[i].da,
                                                   .alphaBlendOp = (VkBlendOp)k->blend[i].aop,
                                                   /* no fragment shader: its outputs would be undefined */
                                                   .colorWriteMask = k->fs ? k->blend[i].mask : 0};
        if (!g_vk.feat.dualSrcBlend) { /* the second source's factors become the first's */
            VkBlendFactor* f[4] = {&a->srcColorBlendFactor, &a->dstColorBlendFactor, &a->srcAlphaBlendFactor,
                                   &a->dstAlphaBlendFactor};
            for (int j = 0; j < 4; j++) {
                VkBlendFactor b = *f[j];
                *f[j] = b == VK_BLEND_FACTOR_SRC1_COLOR             ? VK_BLEND_FACTOR_SRC_COLOR
                        : b == VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR ? VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR
                        : b == VK_BLEND_FACTOR_SRC1_ALPHA           ? VK_BLEND_FACTOR_SRC_ALPHA
                        : b == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
                                                                    : b;
                changed |= *f[j] != b;
            }
        }
    }
    if (!g_vk.feat.independentBlend) /* every attachment blends as the first */
        for (u32 i = 1; i < k->ncolor; i++) {
            changed |= memcmp(&o->cba[i], &o->cba[0], sizeof o->cba[0]) != 0;
            o->cba[i] = o->cba[0];
        }
    if (changed) {
        LOG_LIMIT(
            1, "vulkan: blending that needs dual-source or independent blend, which this GPU lacks, is simplified\n");
    }
    o->advanced = (VkPipelineColorBlendAdvancedStateCreateInfoEXT){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_ADVANCED_STATE_CREATE_INFO_EXT,
        .srcPremultiplied = k->advanced.src_premultiplied,
        .dstPremultiplied = VK_TRUE,
        .blendOverlap = (VkBlendOverlapEXT)k->advanced.overlap};
    if (k->advanced.operation) {
        /* NVN advanced modes select color output zero only. Other bound attachments must have no writes. */
        for (u32 i = 0; i < k->ncolor; i++)
            o->cba[i].colorBlendOp = o->cba[i].alphaBlendOp = (VkBlendOp)k->advanced.operation;
        if (k->ncolor) o->cba[0].blendEnable = VK_TRUE;
    }
    /* a logic operation takes the place of blending on integer and normalized targets; float and sRGB ones pass
       their color through, as the 3D engine's LOGIC_OP does */
    o->cb = (VkPipelineColorBlendStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                  .pNext = k->advanced.operation ? &o->advanced : NULL,
                                                  .logicOpEnable = k->logic_op != 0,
                                                  .logicOp = k->logic_op ? (VkLogicOp)(k->logic_op - 1)
                                                                         : VK_LOGIC_OP_COPY,
                                                  .attachmentCount = k->ncolor,
                                                  .pAttachments = o->cba};
    for (u32 i = 0; i < k->ncolor; i++) o->cf[i] = (VkFormat)k->color[i];
    for (u32 i = 0; i < 8; i++) o->color_samples[i] = (VkSampleCountFlagBits)(k->samples ? k->samples : 1);
    o->attachment_samples = (VkAttachmentSampleCountInfoNV){.sType = VK_STRUCTURE_TYPE_ATTACHMENT_SAMPLE_COUNT_INFO_NV,
                                                            .colorAttachmentCount = k->ncolor,
                                                            .pColorAttachmentSamples = o->color_samples,
                                                            .depthStencilAttachmentSamples =
                                                                k->depth || k->stencil
                                                                    ? (VkSampleCountFlagBits)(raster_samples ? raster_samples : 1)
                                                                    : 0};
    o->ri = (VkPipelineRenderingCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                            .pNext = k->coverage.raster_samples ? &o->attachment_samples : NULL,
                                            .colorAttachmentCount = k->ncolor,
                                            .pColorAttachmentFormats = o->cf,
                                            .depthAttachmentFormat = (VkFormat)k->depth,
                                            .stencilAttachmentFormat = (VkFormat)k->stencil};
    /* Select strict vertex bounds without enabling robustness2 globally for
       storage/uniform buffers or compute pipelines. Per-pipeline selection
       requires the feature to be supported, not enabled device-wide. */
    o->robustness = (VkPipelineRobustnessCreateInfoEXT){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT,
        .vertexInputs = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT};
    /* whether the pipeline cache had it, for the stats line (core in Vulkan 1.3) */
    o->fi = (VkPipelineCreationFeedbackCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
                                                   .pPipelineCreationFeedback = &o->made};
    /* the states every draw pipeline takes as dynamic, and those of the device and the key (pipeline_parts.c): the
       feedback loop's without its pipeline flag (k->feedback), a binding's stride where vkCmdBindVertexBuffers2 sets
       it (a draw without vertex buffers never calls it, so a pipeline without bindings leaves it out), the vertex
       layout with dynamic vertex input, and with pipeline libraries the pre-rasterization state of g_vk.dyn_pr */
    o->dyn = (PipeDynIn){.dyn_pr = g_vk.dyn_pr,
                         .feedback_dynamic = g_vk.has_feedback_dynamic,
                         .stride = l->nbind && g_vk.has_dynamic_stride,
                         .vertex_input = g_vk.vertex_input_dynamic,
                         .tess = k->tes != VK_NULL_HANDLE,
                         .mesh = k->mesh != 0};
    o->flags = (g_vk.has_descriptor_buffers ? VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT : 0) |
               (k->feedback ? VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT : 0);
}
/* the creation feedback after next in a chain: the robustness of vertex inputs where the device has it, then the
   feedback */
static void* pipe_tail(PipeInfo* o, int robust) {
    if (robust && g_vk.has_vertex_robustness2) {
        o->robustness.pNext = &o->fi;
        return &o->robustness;
    }
    return &o->fi;
}
static VkPipeline pipe_create(const Pipe* e, int* hit) {
    PipeInfo o;
    pipe_info(&e->k, e->fs, &o);
    VkDynamicState dyn[VK_PIPE_DYN_MAX];
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = vk_pipe_dynamic(VK_PART_ALL, &o.dyn, dyn),
                                           .pDynamicStates = dyn};
    if (o.ri.pNext)
        o.attachment_samples.pNext = pipe_tail(&o, 1);
    else
        o.ri.pNext = pipe_tail(&o, 1);
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .pNext = &o.ri,
                                       .flags = o.flags,
                                       .stageCount = o.nst,
                                       .pStages = o.st,
                                       .pVertexInputState = e->k.mesh ? NULL : &o.vi,
                                       .pInputAssemblyState = e->k.mesh ? NULL : &o.ia,
                                       .pTessellationState = e->k.tes && !e->k.mesh ? &o.ts : NULL,
                                       .pViewportState = &o.vp,
                                       .pRasterizationState = &o.rs,
                                       .pMultisampleState = &o.ms,
                                       .pDepthStencilState = &o.ds,
                                       .pColorBlendState = &o.cb,
                                       .pDynamicState = &dy,
                                       .layout = e->layout};
    /* descriptor heaps (k->heaps): the stages' mappings take the place of a layout, and the flags go in flags2 */
    VkPipelineCreateFlags2CreateInfo flags2 = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
        .pNext = gi.pNext,
        .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT |
                 (e->k.feedback ? VK_PIPELINE_CREATE_2_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT : 0)};
    if (e->k.heaps) {
        gi.pNext = &flags2;
        gi.flags = 0;
        gi.layout = VK_NULL_HANDLE;
    }
    VkPipeline p = VK_NULL_HANDLE;
    VK_OK(vkCreateGraphicsPipelines(g_vk.dev, g_pipeline.pcache, 1, &gi, NULL, &p));
    *hit = (o.made.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0;
    if (*hit) __atomic_add_fetch(e->prewarm ? &g_pipeline.pf.hits : &g_pipeline.pipe_hits, 1, __ATOMIC_RELAXED);
    return p;
}
/* ------------------------------------------------------------ pipeline libraries */
/* With graphics pipeline libraries (g_vk.has_gpl, docs/layers/gpu/pipeline-libraries.md), a pipeline is the link of
   four parts: vertex input, the stages before rasterization, the fragment stage and the fragment output. Each part is
   made once for its key (vk_pipe_split), so a new state of known shaders costs a link of a few milliseconds where a
   whole pipeline compiled every stage again: one Ounce vertex program of 1.7 MB of SPIR-V took 4 to 10 s for each
   pipeline. The draws wait for the link only, and a worker makes the optimized pipeline after it (pipe_optimize),
   which the draws take when it is ready */
static const char* const g_part_names[4] = {"vertex input", "pre-rasterization", "fragment shader", "fragment output"};
static void plib_init(void) {
    pm_lock_announce(&g_pipeline.plib.lock, "pipeline libraries");
    pc_init(&g_pipeline.pj_work);
    pc_init(&g_pipeline.pj_done);
}

/* a part's layout: the sets of its own stages, the others none. A pre-rasterization part and a fragment part then
   link whatever texture arrays the other has (PipeKey.arrays), where one layout for both would put the fragment
   program's bit in the vertex program's key and compile it again for each kind of fragment program it meets */
static VkPipelineLayout part_layout(u32 part, u32 arrays) {
    int fs = part == VK_PART_FS;
    VkPipelineLayout* l = &g_pipeline.plib.layouts[fs][arrays & ((1u << 5) - 1)];
    VkPipelineLayout made = __atomic_load_n(l, __ATOMIC_ACQUIRE);
    if (made) return made;
    pm_lock(&g_pipeline.playout_lock);
    if (!*l) {
        VkDescriptorSetLayout sets[5] = {0};
        u32 n = fs ? 2 : g_vk_stage_sets;
        for (u32 s = 0; s < n; s++)
            if ((s == 1) == fs) sets[s] = g_vk_dslayout[(arrays >> s) & 1][s];
        VkPushConstantRange pc = {VK_PUSH_STAGES, 0, 128}; /* vk_playout's: a link's parts take its push constants */
        VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                         .flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT,
                                         .setLayoutCount = n,
                                         .pSetLayouts = sets,
                                         .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &pc};
        if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &made))) made = VK_NULL_HANDLE;
        __atomic_store_n(l, made, __ATOMIC_RELEASE);
    }
    made = *l;
    pm_unlock(&g_pipeline.playout_lock);
    return made;
}
/* the part of key (n bytes of it) for a pipeline of src, found or added (g_pipeline.plib.lock) */
static PipeLib* lib_find(u32 part, const PipePartKey* key, size_t n, const PipeKey* src, VkShaderModule fs) {
    u64 h = vk_hash(key, n) ^ part;
    u32 b = (u32)(h ^ (h >> 32)) & (PLIBH - 1);
    for (PipeLib* l = g_pipeline.plib.h[b]; l; l = l->next)
        if (l->h == h && l->part == part && !memcmp(&l->key, key, n)) return l;
    PipeLib* l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->src = *src;
    l->fs = fs;
    memcpy(&l->key, key, n);
    l->part = part;
    l->h = h;
    l->next = g_pipeline.plib.h[b];
    g_pipeline.plib.h[b] = l;
    return l;
}
/* p's parts, with pipeline libraries: none when one can't be had, and p is made whole then */
static void pipe_parts(Pipe* p) {
    /* a mesh pipeline is made whole: it has no vertex input part, and the parts take no mesh or task stage yet. So is
       a pipeline of descriptor heaps, sample locations or coverage to color, state the parts' keys (vk_pipe_split)
       do not hold yet */
    if (!g_vk.has_gpl || p->k.mesh || p->k.heaps || p->k.sample_locations.samples || p->k.coverage_to_color) return;
    const PipeSplitDevice d = {.dyn_pr = g_vk.dyn_pr, .feedback_dynamic = g_vk.has_feedback_dynamic};
    PipePartKey k[4];
    memset(k, 0, sizeof k);
    vk_pipe_split(&p->k, p->fs, &d, &k[0].vi, &k[1].pr, &k[2].fs, &k[3].fo);
    static const size_t size[4] = {sizeof(PipeViKey), sizeof(PipePrKey), sizeof(PipeFsKey), sizeof(PipeFoKey)};
    int all = 1;
    pm_lock(&g_pipeline.plib.lock);
    for (u32 i = 0; i < 4; i++) {
        PipeKey src; /* the part's own state alone (vk_pipe_part_src), whatever pipeline asks for it first */
        vk_pipe_part_src(1u << i, &k[i], &src);
        all &= (p->lib[i] = lib_find(1u << i, &k[i], size[i], &src, i == 2 ? p->fs : VK_NULL_HANDLE)) != NULL;
    }
    pm_unlock(&g_pipeline.plib.lock);
    if (!all) memset(p->lib, 0, sizeof p->lib);
}
/* a pipeline or a part that took a second or more to make: its stages' programs and SPIR-V, to find the shaders the
   driver is slow on (their programs are in the shader corpus, shader_corpus, by these hashes) */
#define PIPE_SLOW_NS 1000000000ull
/* the programs of modules (vertex, tessellation control and evaluation, geometry, fragment) for a log line */
static void stages_text(const VkShaderModule modules[5], char* stages, size_t n) {
    static const char* const names[5] = {"vs", "tcs", "tes", "gs", "fs"};
    size_t at = 0;
    stages[0] = 0;
    for (int s = 0; s < 5; s++) {
        ModInfo mi;
        if (!modules[s] || at >= n) continue;
        if (modinfo_get(modules[s], &mi))
            at += (size_t)snprintf(stages + at, n - at, ", %s %016llx (%u KB of SPIR-V)", names[s],
                                   (unsigned long long)mi.hash, mi.words / 256);
        else
            at += (size_t)snprintf(stages + at, n - at, ", %s (a runtime shader)", names[s]);
    }
}
static void slow_say(const char* what, const VkShaderModule modules[5], u64 ns, int hit) {
    char stages[320];
    if (ns < PIPE_SLOW_NS) return;
    stages_text(modules, stages, sizeof stages);
    LOG_LIMIT(64, "vulkan: %s took %llu ms to make (%s the pipeline cache)%s\n", what,
              (unsigned long long)(ns / 1000000), hit ? "from" : "not in", stages);
}
static void pipe_slow_say(const Pipe* e, u64 ns, int hit) {
    const VkShaderModule modules[5] = {e->k.vs, e->k.tcs, e->k.tes, e->k.gs, e->fs};
    slow_say(e->lib[0] ? "a pipeline, linked from libraries," : "a pipeline", modules, ns, hit);
}
/* l made: a library of its part, from what pipe_info makes of its key, holding only that part's state */
static VkPipeline lib_create(const PipeLib* l, int* hit) {
    PipeInfo o;
    pipe_info(&l->src, l->fs, &o);
    VkDynamicState dyn[VK_PIPE_DYN_MAX];
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = vk_pipe_dynamic(l->part, &o.dyn, dyn),
                                           .pDynamicStates = dyn};
    VkGraphicsPipelineLibraryCreateInfoEXT gl = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
                                                 .flags = l->part == VK_PART_VI ? VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT
                                                          : l->part == VK_PART_PR ? VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT
                                                          : l->part == VK_PART_FS ? VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT
                                                                                  : VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT};
    /* the feedback loop's flag, without its dynamic state, on the parts that read and write the attachment */
    VkPipelineCreateFlags feedback = o.flags & VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .pNext = &gl,
                                       .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR |
                                                VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT |
                                                (o.flags & VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT),
                                       .pDynamicState = &dy};
    VkPipelineCache cache = VK_NULL_HANDLE; /* the interfaces cost microseconds: nothing worth a cache entry */
    switch (l->part) {
    case VK_PART_VI:
        gl.pNext = pipe_tail(&o, 1);
        gi.pVertexInputState = &o.vi;
        gi.pInputAssemblyState = &o.ia;
        break;
    case VK_PART_PR:
        gl.pNext = pipe_tail(&o, 1);
        gi.stageCount = o.npr;
        gi.pStages = o.st;
        gi.pTessellationState = l->src.tes ? &o.ts : NULL;
        gi.pViewportState = &o.vp;
        gi.pRasterizationState = &o.rs;
        gi.layout = part_layout(VK_PART_PR, l->src.arrays & VK_ARRAYS_PR);
        cache = g_pipeline.pcache;
        break;
    case VK_PART_FS:
        gl.pNext = pipe_tail(&o, 0);
        gi.flags |= feedback;
        gi.stageCount = o.nst - o.npr;
        gi.pStages = o.st + o.npr;
        gi.pDepthStencilState = &o.ds;
        /* with sample shading only, as the output part's: the two must be the same then */
        gi.pMultisampleState = l->src.sample_shading ? &o.ms : NULL;
        gi.layout = part_layout(VK_PART_FS, l->src.arrays & VK_ARRAYS_FS);
        cache = g_pipeline.pcache;
        break;
    default:
        gl.pNext = &o.ri;
        if (o.ri.pNext)
            o.attachment_samples.pNext = pipe_tail(&o, 0);
        else
            o.ri.pNext = pipe_tail(&o, 0);
        gi.flags |= feedback;
        gi.pColorBlendState = &o.cb;
        gi.pMultisampleState = &o.ms;
        break;
    }
    VkPipeline p = VK_NULL_HANDLE;
    VK_OK(vkCreateGraphicsPipelines(g_vk.dev, cache, 1, &gi, NULL, &p));
    *hit = (o.made.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0;
    return p;
}
/* l's library, made here unless a thread makes it already, then waited for: VK_NULL_HANDLE when it can't be made */
static VkPipeline lib_get(PipeLib* l) {
    pm_lock(&g_pipeline.pj_lock);
    while (l->state == 1) pc_wait(&g_pipeline.pj_done, &g_pipeline.pj_lock, -1);
    if (l->state == 2) {
        pm_unlock(&g_pipeline.pj_lock);
        return l->p;
    }
    l->state = 1;
    pm_unlock(&g_pipeline.pj_lock);
    u32 i = l->part == VK_PART_VI ? 0 : l->part == VK_PART_PR ? 1 : l->part == VK_PART_FS ? 2 : 3;
    u64 t0 = plat_ns();
    SNX_ZONE_T(z, "pipeline library make", "%s, hash %016llx", g_part_names[i], (unsigned long long)l->h);
    int hit = 0;
    if (i == 1 || i == 2) pcache_note(); /* before the driver, as pipe_make */
    VkPipeline p = lib_create(l, &hit);
    SNX_ZONE_END(z);
    u64 ns = plat_ns() - t0;
    char what[64];
    snprintf(what, sizeof what, "a %s pipeline library", g_part_names[i]);
    const VkShaderModule modules[5] = {i == 1 ? l->src.vs : VK_NULL_HANDLE, i == 1 ? l->src.tcs : VK_NULL_HANDLE,
                                       i == 1 ? l->src.tes : VK_NULL_HANDLE, i == 1 ? l->src.gs : VK_NULL_HANDLE,
                                       i == 2 ? l->fs : VK_NULL_HANDLE};
    slow_say(what, modules, ns, hit);
    if (i == 1 || i == 2) pcache_note();
    __atomic_add_fetch(&g_pipeline.plib.made[i], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_pipeline.plib.ns[i], ns, __ATOMIC_RELAXED);
    if (hit) __atomic_add_fetch(&g_pipeline.plib.hits[i], 1, __ATOMIC_RELAXED);
    pm_lock(&g_pipeline.pj_lock);
    l->p = p;
    l->state = 2;
    if (g_pipeline.pj_started) pc_broadcast(&g_pipeline.pj_done);
    pm_unlock(&g_pipeline.pj_lock);
    return p;
}
/* e's parts linked: fast, without link time optimization, for the draws to wait on; or optimized (lto), into the
   pipeline cache, for them to take when it is ready. The parts are made already */
static VkPipeline gpl_link(const Pipe* e, int lto, int* hit) {
    VkPipeline libs[4];
    for (u32 i = 0; i < 4; i++) libs[i] = e->lib[i]->p;
    VkPipelineCreationFeedback made = {0};
    VkPipelineCreationFeedbackCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
                                               .pPipelineCreationFeedback = &made};
    /* with mixed samples, the targets' samples as the fragment output part has them, for the link too */
    const u32 raster_samples = e->k.coverage.raster_samples;
    VkSampleCountFlagBits color_samples[8];
    for (u32 i = 0; i < 8; i++) color_samples[i] = (VkSampleCountFlagBits)(e->k.samples ? e->k.samples : 1);
    VkAttachmentSampleCountInfoNV attachment_samples = {.sType = VK_STRUCTURE_TYPE_ATTACHMENT_SAMPLE_COUNT_INFO_NV,
                                                        .pNext = &fi,
                                                        .colorAttachmentCount = e->k.ncolor,
                                                        .pColorAttachmentSamples = color_samples,
                                                        .depthStencilAttachmentSamples =
                                                            e->k.depth || e->k.stencil ? (VkSampleCountFlagBits)raster_samples
                                                                                       : 0};
    VkPipelineLibraryCreateInfoKHR li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
                                         .pNext = e->k.coverage.raster_samples ? (void*)&attachment_samples : (void*)&fi,
                                         .libraryCount = 4,
                                         .pLibraries = libs};
    VkGraphicsPipelineCreateInfo gi = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &li,
        .flags = (g_vk.has_descriptor_buffers ? VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT : 0) |
                 (e->k.feedback ? VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT : 0) |
                 (lto ? VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT : 0),
        .layout = e->layout};
    VkPipeline p = VK_NULL_HANDLE;
    VK_OK(vkCreateGraphicsPipelines(g_vk.dev, lto ? g_pipeline.pcache : VK_NULL_HANDLE, 1, &gi, NULL, &p));
    *hit = (made.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0;
    return p;
}
/* e's parts, made or waited for, and their fast link: VK_NULL_HANDLE when one can't be made */
static VkPipeline gpl_fast(const Pipe* e, int* hit) {
    for (u32 i = 0; i < 4; i++)
        if (!lib_get(e->lib[i])) return VK_NULL_HANDLE;
    u64 t0 = plat_ns();
    SNX_ZONE(z, "pipeline link");
    VkPipeline p = gpl_link(e, 0, hit);
    SNX_ZONE_END(z);
    __atomic_add_fetch(&g_pipeline.plib.links, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_pipeline.plib.link_ns, plat_ns() - t0, __ATOMIC_RELAXED);
    return p;
}
/* on a worker, after e's fast link: the optimized pipeline, which the draws take from then on (vk_pipeline_resolve
   and the others read e->p) */
static void pipe_optimize(Pipe* e) {
    u64 t0 = plat_ns();
    SNX_ZONE_T(z, "pipeline optimize", "hash %016llx", (unsigned long long)e->h);
    int hit = 0;
    pcache_note(); /* before the driver, as pipe_make */
    VkPipeline o = gpl_link(e, 1, &hit);
    SNX_ZONE_END(z);
    u64 ns = plat_ns() - t0;
    const VkShaderModule modules[5] = {e->k.vs, e->k.tcs, e->k.tes, e->k.gs, e->fs};
    slow_say("an optimized pipeline, in the background,", modules, ns, hit);
    pcache_note();
    __atomic_add_fetch(&g_pipeline.plib.opts, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_pipeline.plib.opt_ns, ns, __ATOMIC_RELAXED);
    if (hit) __atomic_add_fetch(&g_pipeline.plib.opt_hits, 1, __ATOMIC_RELAXED);
    if (!o) { /* the fast link stays */
        char stages[320];
        stages_text(modules, stages, sizeof stages);
        LOG_LIMIT(16, "vulkan: an optimized pipeline could not be made: its link without optimization stays%s\n",
                  stages);
        return;
    }
    __atomic_store_n(&e->p, o, __ATOMIC_RELEASE);
    __atomic_store_n(&e->opt, 1, __ATOMIC_RELEASE);
}
void vk_pipeline_lib_waits(u64* waits, u64* wait_ms, u64* behind_opt_ms, u64* max_ms, int* opt_workers) {
    pm_lock(&g_pipeline.pj_lock);
    *opt_workers = g_pipeline.plib.opt_busy_max;
    *waits = g_pipeline.plib.job_waits;
    *wait_ms = g_pipeline.plib.job_wait_ns / 1000000;
    *behind_opt_ms = g_pipeline.plib.job_behind_opt_ns / 1000000;
    *max_ms = g_pipeline.plib.job_wait_max_ns / 1000000;
    pm_unlock(&g_pipeline.pj_lock);
}
void vk_pipeline_lib_stats(u64 made[4], u64 hits[4], u64 make_ms[4], u64* links, u64* link_ms, u64* opts,
                           u64* opt_ms) {
    for (int i = 0; i < 4; i++) {
        made[i] = __atomic_load_n(&g_pipeline.plib.made[i], __ATOMIC_RELAXED);
        hits[i] = __atomic_load_n(&g_pipeline.plib.hits[i], __ATOMIC_RELAXED);
        make_ms[i] = __atomic_load_n(&g_pipeline.plib.ns[i], __ATOMIC_RELAXED) / 1000000;
    }
    *links = __atomic_load_n(&g_pipeline.plib.links, __ATOMIC_RELAXED);
    *link_ms = __atomic_load_n(&g_pipeline.plib.link_ns, __ATOMIC_RELAXED) / 1000000;
    *opts = __atomic_load_n(&g_pipeline.plib.opts, __ATOMIC_RELAXED);
    *opt_ms = __atomic_load_n(&g_pipeline.plib.opt_ns, __ATOMIC_RELAXED) / 1000000;
}
void vk_pipeline_lib_warm_stats(u64* made, u64* hit, u64* ms) {
    *made = __atomic_load_n(&g_pipeline.plib.warm_made, __ATOMIC_RELAXED);
    *hit = __atomic_load_n(&g_pipeline.plib.warm_hit, __ATOMIC_RELAXED);
    *ms = __atomic_load_n(&g_pipeline.plib.warm_ns, __ATOMIC_RELAXED) / 1000000;
}
/* e's pipeline, made here. One a draw asked for is kept in the file (pf_pipe_record): once made when the GPU thread
   makes it (record), and when it queues it for a worker otherwise, as the file's tables are the GPU thread's */
static VkPipeline pipe_make(const Pipe* e, int record) {
    SNX_ZONE_T(z_pipe, "pipeline make", "hash %016llx", (unsigned long long)e->h);
    u64 t0 = plat_ns();
    SNX_ZONE(z, "pipeline make");
    /* A new-area pipeline can arrive after a long quiet spell. Mark it before entering the driver, so a periodic
       cache snapshot doesn't mistake an in-flight compile for idleness and contend for the driver's cache lock. */
    pcache_note();
    int hit = 0;
    VkPipeline p = e->lib[0] ? gpl_fast(e, &hit) : pipe_create(e, &hit);
    SNX_ZONE_END(z);
    pipe_slow_say(e, plat_ns() - t0, hit);
    pcache_note();
    if (e->prewarm)
        __atomic_add_fetch(&g_pipeline.pf.made, 1, __ATOMIC_RELAXED);
    else {
        __atomic_add_fetch(&g_pipeline.pipe_make_ns, plat_ns() - t0, __ATOMIC_RELAXED);
        if (p && record) pf_pipe_record(&e->k);
    }
    return p;
}

/* a shipped program's part this large or larger (SPIR-V) takes about a second or more to make, so it is warmed */
#define SHIP_WARM_WORDS 65536u /* 256 KiB of SPIR-V */
/* As the optimized pipelines (PIPE_OPT_WORKERS), a warm part's compile is seconds and no draw can take it back once a
   worker has begun it. One worker warms, the others stay free for the draws' new pipelines */
#define WARM_WORKERS 1
/* The most workers that make optimized pipelines at once. An optimized link compiles every stage again: 4 to 5 s
   for a pipeline of a 1.7 MB vertex program on NVIDIA, which no draw can take back once a worker has begun it. A
   first run queues hundreds of them behind the parts, and with no bound they took every worker while a new
   shader's parts waited (docs/layers/gpu/pipeline-libraries.md, "Results"). One worker optimizes, the others stay free */
#define PIPE_OPT_WORKERS 1
static void pipe_queue(Pipe* e, int queue);
/* on a worker: make a shipped program's part into the pipeline cache, then drop the module and the part pipeline (a
   draw's own module later hits the cache by its SPIR-V, which is the cache's key) */
static void warm_run(WarmItem* w) {
    PipeLib lib = {.src = w->src, .fs = w->fs, .part = w->part};
    int hit = 0;
    u64 t0 = plat_ns();
    VkPipeline p = lib_create(&lib, &hit);
    u64 ns = plat_ns() - t0;
    __atomic_add_fetch(&g_pipeline.plib.warm_ns, ns, __ATOMIC_RELAXED);
    pcache_note();
    __atomic_add_fetch(&g_pipeline.plib.warm_made, 1, __ATOMIC_RELAXED);
    if (hit) __atomic_add_fetch(&g_pipeline.plib.warm_hit, 1, __ATOMIC_RELAXED);
    if (p) vkDestroyPipeline(g_vk.dev, p, NULL);
    VkShaderModule mod = w->part == VK_PART_PR ? w->src.vs : w->fs;
    if (mod) vkDestroyShaderModule(g_vk.dev, mod, NULL);
    free(w);
}
static void pipe_worker(void* low) {
    SNX_THREAD_NAME("Vulkan pipelines");
    if (low) { /* one past the first two (snx_vk_pipeline_low_workers) */
        char why[256];
        if (snx_thread_priority_low(why, sizeof why))
            WARN_LIMIT(1,
                       "vulkan: a pipeline worker asked for %s, the system's low priority: %s (said for the first "
                       "one refused)\n",
                       snx_thread_priority_low_name(), why);
    }
    for (;;) {
        Pipe* e;
        VkPipeline p;
        pm_lock(&g_pipeline.pj_lock);
        while (!g_pipeline.pj_head && !g_pipeline.pw_head &&
               !(g_pipeline.po_head && g_pipeline.po_busy < PIPE_OPT_WORKERS) &&
               !(g_pipeline.warm_head && g_pipeline.warm_busy < WARM_WORKERS))
            pc_wait(&g_pipeline.pj_work, &g_pipeline.pj_lock, -1);
        if (!g_pipeline.pj_head && !g_pipeline.pw_head &&
            !(g_pipeline.po_head && g_pipeline.po_busy < PIPE_OPT_WORKERS)) {
            WarmItem* w = g_pipeline.warm_head;
            g_pipeline.warm_head = w->next;
            if (!g_pipeline.warm_head) g_pipeline.warm_tail = NULL;
            g_pipeline.warm_busy++;
            pm_unlock(&g_pipeline.pj_lock);
            warm_run(w);
            pm_lock(&g_pipeline.pj_lock);
            g_pipeline.warm_busy--;
            if (g_pipeline.warm_head) pc_signal(&g_pipeline.pj_work); /* the next warm part, to a worker that waits */
            pm_unlock(&g_pipeline.pj_lock);
            continue;
        }
        Pipe **head = g_pipeline.pj_head   ? &g_pipeline.pj_head
                      : g_pipeline.pw_head ? &g_pipeline.pw_head
                                           : &g_pipeline.po_head,
             **tail = g_pipeline.pj_head   ? &g_pipeline.pj_tail
                      : g_pipeline.pw_head ? &g_pipeline.pw_tail
                                           : &g_pipeline.po_tail;
        e = *head;
        *head = e->job;
        if (!*head) *tail = NULL;
        int optimize = e->queued == 3;
        if (e->queued == 1) {
            u64 waited = plat_ns() - e->queued_ns;
            g_pipeline.plib.job_waits++;
            g_pipeline.plib.job_wait_ns += waited;
            if (e->behind_opt) g_pipeline.plib.job_behind_opt_ns += waited;
            if (waited > g_pipeline.plib.job_wait_max_ns) g_pipeline.plib.job_wait_max_ns = waited;
        }
        e->queued = 0;
        g_pipeline.pj_busy++;
        g_pipeline.po_busy += optimize;
        if (g_pipeline.po_busy > g_pipeline.plib.opt_busy_max) g_pipeline.plib.opt_busy_max = g_pipeline.po_busy;
        pm_unlock(&g_pipeline.pj_lock);
        if (optimize) {
            pipe_optimize(e);
            pm_lock(&g_pipeline.pj_lock);
            g_pipeline.pj_busy--;
            g_pipeline.po_busy--;
            pc_broadcast(&g_pipeline.pj_done);
            if (g_pipeline.po_head)
                pc_signal(&g_pipeline.pj_work); /* the next optimized pipeline, to a worker that waits */
            pm_unlock(&g_pipeline.pj_lock);
            continue;
        }
        p = pipe_make(e, 0);
        pm_lock(&g_pipeline.pj_lock);
        g_pipeline.pj_busy--;
        __atomic_store_n(&e->p, p, __ATOMIC_RELEASE);
        __atomic_store_n(&e->ready, 1, __ATOMIC_RELEASE);
        if (p && e->lib[0] && !g_vk.gpl_hold) pipe_queue(e, 3); /* a test's hold: the link stays */
        pc_broadcast(&g_pipeline.pj_done);
        pm_unlock(&g_pipeline.pj_lock);
    }
}
void vk_pipeline_quiesce(void) {
    if (g_pipeline.pf
            .on) { /* the reading thread queues the file's pipelines, then may translate the shipped programs */
        pm_lock(&g_pipeline.pf.lock);
        while (!g_pipeline.pf.loaded) pc_wait(&g_pipeline.pf.done, &g_pipeline.pf.lock, -1);
        pm_unlock(&g_pipeline.pf.lock);
        while (!__atomic_load_n(&g_pipeline.pf.ship_ready, __ATOMIC_ACQUIRE)) plat_sleep_ns(1000000);
    }
    pm_lock(&g_pipeline.pj_lock);
    /* the optimized pipelines still to make are left: the fast links serve until the end */
    for (Pipe* e = g_pipeline.po_head; e; e = e->job) e->queued = 0;
    g_pipeline.po_head = g_pipeline.po_tail = NULL;
    while (g_pipeline.pj_threads && (g_pipeline.pj_head || g_pipeline.pw_head || g_pipeline.pj_busy))
        pc_wait(&g_pipeline.pj_done, &g_pipeline.pj_lock, -1);
    pm_unlock(&g_pipeline.pj_lock);
}
/* the workers, started the first time one is needed: how many there are (g_pipeline.pj_lock) */
static int pipe_workers(void) {
    if (!g_pipeline.pj_started) {
        int cpus = plat_cpu_count(), n = snx_vk_pipeline_workers(g_settings.pipeline_threads, cpus);
        int low = snx_vk_pipeline_low_workers(n), started[2] = {0, 0};
        char why[256] = "";
        g_pipeline.pj_started = 1;
        for (int i = 0; i < n; i++) {
            int lowered = i >= n - low;
            if (!plat_thread(pipe_worker, (void*)(uintptr_t)lowered, (size_t)1 << 20))
                started[lowered]++;
            else
                snx_os_error(why, sizeof why);
        }
        g_pipeline.pj_threads = started[0] + started[1];
        LOG("vulkan: pipeline_threads=%d%s: %d workers make the new pipelines, of %d CPUs: %d at the system's normal "
            "priority, %d at its low one (%s)\n",
            g_settings.pipeline_threads, g_settings.pipeline_threads ? "" : " (Auto)", g_pipeline.pj_threads, cpus,
            started[0], started[1], snx_thread_priority_low_name());
        if (g_pipeline.pj_threads < n)
            WARN("vulkan: %d of the %d pipeline workers did not start: %s\n", n - g_pipeline.pj_threads, n, why);
    }
    return g_pipeline.pj_threads;
}
/* e at the end of a queue (g_pipeline.pj_lock): 1 a draw's, 2 the file's, 3 to optimize */
static void pipe_queue(Pipe* e, int queue) {
    Pipe **head = queue == 1   ? &g_pipeline.pj_head
                  : queue == 2 ? &g_pipeline.pw_head
                               : &g_pipeline.po_head,
         **tail = queue == 1   ? &g_pipeline.pj_tail
                  : queue == 2 ? &g_pipeline.pw_tail
                               : &g_pipeline.po_tail;
    e->job = NULL;
    if (*tail)
        (*tail)->job = e;
    else
        *head = e;
    *tail = e;
    e->queued = queue;
    if (queue == 1) {
        e->queued_ns = plat_ns();
        e->behind_opt = g_pipeline.pj_busy >= g_pipeline.pj_threads && g_pipeline.po_busy > 0;
    }
    pc_signal(&g_pipeline.pj_work);
}
/* a pipeline of the file that a draw asks for before a worker took it goes to the draws' queue: 1 when it did */
static int pipe_promote(Pipe* e) {
    int moved = 0;
    pm_lock(&g_pipeline.pj_lock);
    if (e->queued == 2) {
        Pipe *q, *before = NULL;
        for (q = g_pipeline.pw_head; q != e; q = q->job) before = q;
        if (before)
            before->job = e->job;
        else
            g_pipeline.pw_head = e->job;
        if (g_pipeline.pw_tail == e) g_pipeline.pw_tail = before;
        pipe_queue(e, 1);
        moved = 1;
    }
    pm_unlock(&g_pipeline.pj_lock);
    return moved;
}
/* e's pipeline once ready, waiting at most ns for it (a negative ns: until it is) */
static VkPipeline pipe_wait(Pipe* e, i64 ns) {
    u64 until = ns < 0 ? 0 : plat_ns() + (u64)ns;
    SNX_ZONE(z, "pipeline wait");
    pm_lock(&g_pipeline.pj_lock);
    while (!e->ready) {
        u64 now = plat_ns();
        if (ns >= 0 && now >= until) break;
        pc_wait(&g_pipeline.pj_done, &g_pipeline.pj_lock, ns < 0 ? -1 : (i64)(until - now));
    }
    pm_unlock(&g_pipeline.pj_lock);
    SNX_ZONE_END(z);
    return __atomic_load_n(&e->ready, __ATOMIC_ACQUIRE) ? __atomic_load_n(&e->p, __ATOMIC_ACQUIRE) : VK_NULL_HANDLE;
}
/* Stand-ins for pipelines still being made, which the GPU thread hands the recording thread in their place
   (vk_pipeline_get_deferred): PIPE_STANDIN in the top bits, which no handle has (record.c's REC_STANDIN tells the same
   of descriptor sets), and the pipeline's index in this table, pages the GPU thread adds before a stand-in naming
   them goes through the ring. Pipelines live as long as the run, so a stand-in does too */
#define PIPE_STANDIN 0x5EC6000000000000ull
#define PSTAND_PAGE  1024
static VkPipeline pipe_standin(Pipe* p) {
    if (!p->standin) {
        u32 page = g_pipeline.npstand / PSTAND_PAGE;
        if (page >= sizeof g_pipeline.pstand / sizeof *g_pipeline.pstand) return VK_NULL_HANDLE;
        if (!g_pipeline.pstand[page] && !(g_pipeline.pstand[page] = calloc(PSTAND_PAGE, sizeof **g_pipeline.pstand)))
            return VK_NULL_HANDLE;
        g_pipeline.pstand[page][g_pipeline.npstand % PSTAND_PAGE] = p;
        p->standin = ++g_pipeline.npstand;
    }
    return (VkPipeline)(uintptr_t)(PIPE_STANDIN | p->standin);
}
/* the GPU thread's wait for p, made by a worker */
static VkPipeline pipe_wait_here(Pipe* p) {
    u64 t0 = plat_ns();
    gpu_wait_note("a pipeline being made", p->h);
    VkPipeline got = pipe_wait(p, -1);
    gpu_wait_note(NULL, 0);
    __atomic_add_fetch(&g_pipeline.pipe_wait_ns, plat_ns() - t0, __ATOMIC_RELAXED);
    return got;
}
/* k's entry, found or added (NULL with no memory): *found says whether it was there */
static Pipe* pipe_entry(const PipeKey* k, int* found) {
    pf_merge();
    for (u32 i = 0; i < 4 && g_pipeline.recent[i]; i++)
        if (!memcmp(&g_pipeline.recent[i]->k, k, sizeof *k)) {
            *found = 1;
            return g_pipeline.recent[i];
        }
    u64 h = vk_hash(k, sizeof *k);
    u32 b = (u32)(h ^ (h >> 32)) & (PIPEH - 1);
    for (Pipe* p = g_pipeline.pipeh[b]; p; p = p->next)
        if (p->h == h && !memcmp(&p->k, k, sizeof *k)) {
            *found = 1;
            g_pipeline.recent[g_pipeline.recent_next++ & 3] = p;
            return p;
        }
    Pipe* p = calloc(1, sizeof *p);
    *found = 0;
    if (!p) return NULL;
    p->k = *k;
    p->h = h;
    p->layout = vk_playout(k->arrays);
    p->fs = k->fs ? k->fs : null_fs();
    pipe_parts(p);
    p->next = g_pipeline.pipeh[b];
    g_pipeline.pipeh[b] = p;
    g_pipeline.pipes_new++;
    g_pipeline.recent[g_pipeline.recent_next++ & 3] = p;
    return p;
}
/* a pipeline made here, on the thread that asks for it: with pipeline libraries its fast link, and the optimized one
   queued for the workers (made here too without them) */
static VkPipeline pipe_make_here(Pipe* p, int record) {
    u64 t0 = plat_ns();
    VkPipeline made = pipe_make(p, record);
    __atomic_store_n(&p->p, made, __ATOMIC_RELEASE);
    __atomic_store_n(&p->ready, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_pipeline.pipe_wait_ns, plat_ns() - t0, __ATOMIC_RELAXED);
    if (made && p->lib[0] && !g_vk.gpl_hold) { /* a test's hold: the link stays */
        pm_lock(&g_pipeline.pj_lock);
        int queued = pipe_workers() != 0;
        if (queued) pipe_queue(p, 3);
        pm_unlock(&g_pipeline.pj_lock);
        if (!queued) pipe_optimize(p);
    }
    return made;
}
/* the pipeline for k: made here when new, waited for when one of the file's is still being made */
VkPipeline vk_pipeline_get(const PipeKey* k) {
    int found;
    Pipe* p = pipe_entry(k, &found);
    if (!p) return VK_NULL_HANDLE;
    if (found) {
        if (__atomic_load_n(&p->ready, __ATOMIC_ACQUIRE)) {
            return __atomic_load_n(&p->p, __ATOMIC_ACQUIRE);
        }
        if (p->prewarm) pipe_promote(p);
        return pipe_wait_here(p);
    }
    return pipe_make_here(p, 1);
}
/* A draw's pipeline when its commands go through the recording thread: the pipeline if it is ready, otherwise a
   stand-in, with the pipeline made on a worker in the order draws asked for them. The GPU thread goes on with the
   frame and the recording thread waits for each pipeline when it reaches its bind (vk_pipeline_resolve). A burst of
   new pipelines (a first run makes hundreds as views come into sight, 87 ms each on one driver without its cache)
   is made by the workers side by side while the frame is decoded, where the GPU thread made them one after another
   and stopped the game for 45 s once. No draw is skipped for them. With pipeline libraries a pipeline whose optimized
   one is still being made is a stand-in too, so that a draw that keeps the handle binds the optimized one once it is
   there */
VkPipeline vk_pipeline_get_deferred(const PipeKey* k) {
    if (!vk_rec_threaded()) return vk_pipeline_get(k);
    int found;
    Pipe* p = pipe_entry(k, &found);
    if (!p) return VK_NULL_HANDLE;
    if (found) {
        if (__atomic_load_n(&p->ready, __ATOMIC_ACQUIRE)) {
            VkPipeline made = __atomic_load_n(&p->p, __ATOMIC_ACQUIRE);
            if (!p->lib[0] || !made || __atomic_load_n(&p->opt, __ATOMIC_ACQUIRE)) return made;
        } else if (p->prewarm)
            pipe_promote(p);
    } else {
        int queued = 0;
        pf_pipe_record(&p->k);
        pm_lock(&g_pipeline.pj_lock);
        if (pipe_workers()) {
            pipe_queue(p, 1);
            queued = 1;
        }
        pm_unlock(&g_pipeline.pj_lock);
        if (!queued) return pipe_make_here(p, 0); /* no worker: made here */
    }
    VkPipeline s = pipe_standin(p);
    return s ? s : pipe_wait_here(p);
}
/* on the recording thread: the pipeline a handle from vk_pipeline_get_deferred names, waited for while a worker makes
   it. VK_NULL_HANDLE when it couldn't be made */
VkPipeline vk_pipeline_resolve(VkPipeline h) {
    u64 v = (u64)(uintptr_t)h;
    if ((v & ~(u64)0xFFFFFFFFull) != PIPE_STANDIN) return h;
    u32 i = (u32)v - 1;
    Pipe* p = g_pipeline.pstand[i / PSTAND_PAGE][i % PSTAND_PAGE];
    if (__atomic_load_n(&p->ready, __ATOMIC_ACQUIRE)) return __atomic_load_n(&p->p, __ATOMIC_ACQUIRE);
    u64 t0 = plat_ns();
    VkPipeline got = pipe_wait(p, -1);
    __atomic_add_fetch(&g_pipeline.pipe_wait_ns, plat_ns() - t0, __ATOMIC_RELAXED);
    return got;
}
/* the pipeline a handle from vk_pipeline_get_deferred names once it is ready, else the handle: without waiting */
VkPipeline vk_pipeline_ready(VkPipeline h) {
    u64 v = (u64)(uintptr_t)h;
    if ((v & ~(u64)0xFFFFFFFFull) != PIPE_STANDIN) return h;
    u32 i = (u32)v - 1;
    Pipe* p = g_pipeline.pstand[i / PSTAND_PAGE][i % PSTAND_PAGE];
    return __atomic_load_n(&p->ready, __ATOMIC_ACQUIRE) ? __atomic_load_n(&p->p, __ATOMIC_ACQUIRE) : h;
}
/* the pipeline h names without waiting for it: VK_NULL_HANDLE with *pending set while a worker still makes it, and
   VK_NULL_HANDLE alone when it couldn't be made */
VkPipeline vk_pipeline_now(VkPipeline h, int* pending) {
    VkPipeline p = vk_pipeline_ready(h);
    *pending = (((u64)(uintptr_t)p) & ~(u64)0xFFFFFFFFull) == PIPE_STANDIN;
    return *pending ? VK_NULL_HANDLE : p;
}
void vk_pipeline_stats(u64* pipes, u64* hits, u64* make_ms, u64* wait_ms, u64* shaders, u64* shader_ms) {
    *pipes = g_pipeline.pipes_new;
    *hits = __atomic_load_n(&g_pipeline.pipe_hits, __ATOMIC_RELAXED);
    *make_ms = __atomic_load_n(&g_pipeline.pipe_make_ns, __ATOMIC_RELAXED) / 1000000;
    *wait_ms = __atomic_load_n(&g_pipeline.pipe_wait_ns, __ATOMIC_RELAXED) / 1000000;
    *shaders = g_pipeline.sh_new;
    *shader_ms = g_pipeline.sh_ns / 1000000;
}

#include "nx/gpu/vk/pipeline_cache_save.inc"

/* ------------------------------------------------------------ the shaders and pipelines of earlier runs */
/* a module's handle as the file knows it, and back (a pointer, or a 64-bit integer on 32-bit hosts) */
static u64 mod_bits(VkShaderModule m) {
    u64 v = 0;
    memcpy(&v, &m, sizeof m);
    return v;
}
static VkShaderModule mod_handle(u64 v) {
    VkShaderModule m = VK_NULL_HANDLE;
    memcpy(&m, &v, sizeof m);
    return m;
}
typedef char pf_cbr_words[sizeof(VkCbRead) == 16 ? 1 : -1]; /* the file's reads are VkCbRead's four words */

/* the bytes of a program kept with its shader (PF_SOURCE): mlen, the program as shader_hash sizes it, which is what
   the translation reads, with the words after it that a lookahead past its last instruction would read */
static u32 src_bytes(u32 mlen) {
    u32 n = ((mlen + 7u) & ~7u) + 64u;
    return n < SH_CODE ? n : SH_CODE;
}
/* s's record, of spv (nw words), into pf for module (0 for none): where it starts in the file, 0 when not written.
   With code (its first ncode bytes, src_bytes) and xfb, the program it was translated from goes before it
   (PF_SOURCE), which a later build or driver translates again (pf_load_source) */
static u64 pf_shader_into(PFile* pf, const VkSh* s, u64 module, const u32* spv, size_t nw, const u8* code, u32 ncode,
                          const ShXfb* xfb) {
    PfShader r = {0};
    PfSource src = {0};
    if (!g_pipeline.pf.on || nw > 0xFFFFFFFFu) return 0;
    if (code) {
        src.hash = s->hash;
        src.qh = s->qh;
        src.qh_a = s->qh_a;
        src.stage = (u32)s->stage;
        src.clen = s->clen;
        src.clen_a = s->clen_a;
        src.ncbr = s->cbr.n;
        src.ncode = ncode;
        src.flags = xfb ? PF_SRC_XFB : 0;
        if (s->mask[0] == 0x54435350u) src.flags |= PF_SRC_TESS_PASS;
        memcpy(src.mask, s->mask, sizeof src.mask);
    }
    r.hash = s->hash;
    r.qh = s->qh;
    r.qh_a = s->qh_a;
    r.stage = (u32)s->stage;
    r.clen = s->clen;
    r.clen_a = s->clen_a;
    r.flags = s->shipped ? PF_SHIPPED : 0;
    r.ncbr = s->cbr.n;
    r.nspv = (u32)nw;
    memcpy(r.mask, s->mask, sizeof r.mask);
    memcpy(&r.si, &s->si, sizeof r.si);
    return pf_shader(pf, module, &r, (const u32*)(const void*)s->cbr.r, spv, code ? &src : NULL, code, xfb);
}
/* s, made this run from spv translated from code (ncode bytes of it kept, with xfb): kept for the next one */
static void pf_shader_record(const VkSh* s, const u32* spv, size_t nw, const u8* code, u32 ncode, const ShXfb* xfb) {
    pf_shader_into(&g_pipeline.pf.file, s, mod_bits(s->mod), spv, nw, code, ncode, xfb);
}
VkSh* vk_shader_generated_find(u64 hash, int stage, const u32 mask[26]) {
    pf_merge();
    for (VkSh* s = g_pipeline.shh[hash & 1023]; s; s = s->next)
        if (s->hash == hash && s->stage == stage && !memcmp(s->mask, mask, sizeof s->mask)) return s;
    return NULL;
}
void vk_shader_generated_record(VkSh* s, const u32* spv, size_t words, const u8* source, u32 bytes) {
    pf_shader_record(s, spv, words, source, bytes, NULL);
    s->next = g_pipeline.shh[s->hash & 1023];
    g_pipeline.shh[s->hash & 1023] = s;
}
/* a draw's pipeline, made: kept for the next run, its modules by their shaders' records (any thread) */
static void pf_pipe_record(const PipeKey* k) {
    PipeKey c;
    u64 m[5];
    if (!g_pipeline.pf.on) return;
    m[0] = mod_bits(k->vs);
    m[1] = mod_bits(k->tcs);
    m[2] = mod_bits(k->tes);
    m[3] = mod_bits(k->gs);
    m[4] = mod_bits(k->fs);
    memcpy(&c, k, sizeof c);
    c.vs = c.tcs = c.tes = c.gs = c.fs = VK_NULL_HANDLE;
    pf_pipe(&g_pipeline.pf.file, m, &c, sizeof c);
}

/* s after the file's shaders, which pf_merge puts in the tables (the reading thread, before loaded) */
static void pf_sh_append(VkSh* s) {
    if (g_pipeline.pf.sh_tail)
        g_pipeline.pf.sh_tail->next = s;
    else
        g_pipeline.pf.sh = s;
    g_pipeline.pf.sh_tail = s;
    g_pipeline.pf.nsh++;
}
/* a shader record (its reads and SPIR-V after it) made into a VkSh as vk_shader_get makes one, without translating:
   NULL when it can't be */
static VkSh* sh_from_record(const PfShader* r, const u32* cbr, const u32* spv) {
    VkSh* s;
    if (r->si.ntex < 0 || r->si.ntex > SH_MAXTEX || vk_spirv_unavailable(spv, r->nspv)) return NULL;
    s = calloc(1, sizeof *s);
    if (!s) return NULL;
    if (r->ncbr) {
        s->cbr.r = malloc(r->ncbr * sizeof *s->cbr.r);
        if (!s->cbr.r) {
            free(s);
            return NULL;
        }
        memcpy(s->cbr.r, cbr, r->ncbr * sizeof *s->cbr.r);
        s->cbr.n = r->ncbr;
    }
    s->hash = r->hash;
    s->shipped = (r->flags & PF_SHIPPED) != 0;
    s->stage = (int)r->stage;
    memcpy(s->mask, r->mask, sizeof s->mask);
    memcpy(&s->si, &r->si, sizeof s->si);
    s->clen = r->clen;
    s->qh = r->qh;
    s->clen_a = r->clen_a;
    s->qh_a = r->qh_a;
    for (int i = 0; i < s->si.ntex; i++) s->indexed |= (s->si.tex[i].flags & SNX_TEX_INDEXED) != 0;
    s->mod = vk_module_spv(spv, r->nspv);
    modinfo_put(s->mod, s->hash, s->stage, r->nspv);
    if (!s->mod) {
        free(s->cbr.r);
        free(s);
        return NULL;
    }
    return s;
}
/* on the reading thread: a shader of the file */
static u64 pf_load_shader(void* ctx, const PfShader* r, const u32* cbr, const u32* spv) {
    VkSh* s = sh_from_record(r, cbr, spv);
    (void)ctx;
    if (!s) return 0;
    pf_sh_append(s);
    return mod_bits(s->mod);
}
static int replay_read(u32 idx, u32 off, u32* out) {
    for (u32 i = 0; i < g_pipeline.replay->n; i++)
        if (g_pipeline.replay->r[i].idx == idx && g_pipeline.replay->r[i].off == off) {
            *out = g_pipeline.replay->r[i].v;
            return (int)g_pipeline.replay->r[i].ok;
        }
    return 0;
}
/* on the reading thread, in a file of another build or driver being rebuilt: the program a shader was translated
   from, translated again with this build's translator as vk_shader_get would, and recorded in the new file */
static u64 pf_load_source(void* ctx, const PfSource* r, const u32* cbr, const u8* code, const ShXfb* xfb) {
    VkCbRead* reads = r->ncbr ? malloc(r->ncbr * sizeof *reads) : NULL;
    VkCbReads replay = {reads, r->ncbr};
    u8* window = calloc(1, SH_CODE);
    VkSh* s = calloc(1, sizeof *s);
    u32* spv = NULL;
    size_t nw = 0;
    (void)ctx;
    if ((r->ncbr && !reads) || !window || !s || r->ncode > SH_CODE) goto fail;
    if (r->flags & PF_SRC_TESS_PASS) {
        if (r->flags != PF_SRC_TESS_PASS || r->stage != ST_TCS || r->ncbr || r->ncode != 24 ||
            snx_ld_le32(code) != 0x54435350u || snx_ld_le32(code + 20) != 1) goto fail;
        u32 vertices = snx_ld_le32(code + 4), attributes = snx_ld_le32(code + 8);
        u32 point = snx_ld_le32(code + 12), clips = snx_ld_le32(code + 16);
        if (point > 1) goto fail;
        spv = vk_tess_passthrough_spirv_full(vertices, attributes, (int)point, clips, &nw);
        if (!spv || vk_spirv_unavailable(spv, nw) || !(s->mod = vk_module_spv(spv, nw))) goto fail;
        s->hash = r->hash;
        s->stage = s->si.stage = ST_TCS;
        memcpy(s->mask, r->mask, sizeof s->mask);
        s->si.in_mask = s->si.out_mask = attributes;
        s->si.reads_point_size = s->si.writes_point_size = (int)point;
        s->si.clip_input_mask = s->si.uses_clip_mask = clips;
        pf_shader_into(&g_pipeline.pf.file, s, mod_bits(s->mod), spv, nw, code, r->ncode, NULL);
        free(spv);
        free(window);
        free(reads);
        pf_sh_append(s);
        return mod_bits(s->mod);
    }
    if (r->ncbr) memcpy(reads, cbr, r->ncbr * sizeof *reads);
    memcpy(window, code, r->ncode);
    g_pipeline.replay = &replay;
    spv = sh_spirv(s, window, r->hash, (int)r->stage, r->mask, r->clen, r->qh, r->clen_a, r->qh_a, xfb, replay_read, 0,
                   &nw);
    g_pipeline.replay = NULL;
    if (!spv || vk_spirv_unavailable(spv, nw) || !(s->mod = vk_module_spv(spv, nw))) goto fail;
    modinfo_put(s->mod, r->hash, (int)r->stage, nw);
    pf_shader_into(&g_pipeline.pf.file, s, mod_bits(s->mod), spv, nw, window, r->ncode, xfb);
    free(spv);
    free(window);
    free(reads);
    pf_sh_append(s);
    return mod_bits(s->mod);
fail:
    free(spv);
    free(window);
    free(reads);
    if (s) free(s->cbr.r);
    free(s);
    return 0;
}
/* In a rebuild, a pipeline record keeps the earlier build's key with the modules of the new translation. Two of its
   fields come from that translation: the sets with texture arrays, from each stage's indexed flag, and the attributes,
   one for each one the vertex shader reads. A translator that came to index a shader's textures would have its
   pipeline made with the layout without the arrays, which its module declares, and the record written into the new
   file as it was. 1 when both fields are what the rebuilt shaders say; a record they don't hold for is dropped, and a
   draw that needs its pipeline makes it from its own key */
static int pf_key_holds(const PipeKey* k) {
    const VkShaderModule mods[5] = {k->vs, k->tcs, k->tes, k->gs, k->fs};
    u32 arrays = 0, attrs = 0, in = 0;
    for (int i = 0; i < 5; i++) {
        const VkSh* sh = NULL;
        if (!mods[i]) continue;
        for (const VkSh* q = g_pipeline.pf.sh; q && !sh; q = q->next)
            if (q->mod == mods[i]) sh = q;
        if (!sh) return 0;
        if (sh->indexed) arrays |= 1u << shader_vk_set(sh->stage);
        if (i == 0) in = sh->si.in_mask;
    }
    for (u32 i = 0; i < k->vi.nattr; i++) attrs |= 1u << k->vi.attr[i].loc;
    /* with dynamic vertex input a key holds no layout, which the draws give */
    return arrays == k->arrays && (g_vk.vertex_input_dynamic ? !k->vi.nattr && !k->vi.nbind : attrs == in);
}
void vk_pf_cspipe(u64 key, const u32 launch[5], const VkCbReads* cbr, const void* vkey, u32 nkey, const u8* code,
                  u32 ncode) {
    PfCsPipe c = {key, launch[0], launch[1], launch[2], launch[3], launch[4], cbr->n, nkey, ncode};
    if (g_pipeline.pf.on) pf_cspipe(&g_pipeline.pf.file, &c, (const u32*)(const void*)cbr->r, vkey, code);
}
/* on the reading thread: a compute pipeline of the file, made now, before the game starts (vk_pipeline_prepare's done
   waits for the file). Built here, its driver's compile is the prepare screen's rather than a dispatch's, which held
   the GPU thread and every thread waiting on it for seconds after an update of the runtime or the driver */
static void pf_load_cspipe(void* ctx, const PfCsPipe* c, const u32* cbr, const u8* key, const u8* code) {
    const u32 launch[5] = {c->lx, c->ly, c->lz, c->shm, c->flow_base};
    if (vk_cs_pipe_load(launch, cbr, c->ncbr, key, c->nkey, code, c->ncode, ((const PfLoad*)ctx)->rebuild))
        g_pipeline.pf.ncs++;
    else
        g_pipeline.pf.ncs_failed++;
}
/* on the reading thread: a pipeline of the file, whose shaders were made, queued to the workers behind the draws'. In
   a file being rebuilt it is recorded in the new one, whose shader records its modules have */
static void pf_load_pipe(void* ctx, const u64 m[5], const void* key, u32 keylen) {
    Pipe* p;
    if (keylen != sizeof p->k || !m[0]) return;
    p = calloc(1, sizeof *p);
    if (!p) return;
    memcpy(&p->k, key, sizeof p->k);
    if (p->k.vi.nattr > 32 || p->k.vi.nbind > 33 || p->k.ncolor > 8) { /* pipe_create's arrays */
        free(p);
        return;
    }
    p->k.vs = mod_handle(m[0]);
    p->k.tcs = mod_handle(m[1]);
    p->k.tes = mod_handle(m[2]);
    p->k.gs = mod_handle(m[3]);
    p->k.fs = mod_handle(m[4]);
    if (((const PfLoad*)ctx)->rebuild && !pf_key_holds(&p->k)) {
        free(p);
        return;
    }
    p->h = vk_hash(&p->k, sizeof p->k);
    u32 bucket = (u32)(p->h ^ (p->h >> 32)) & (PIPEH - 1);
    for (Pipe* q = g_pipeline.pf.pipes[bucket]; q; q = q->next) /* the file has it twice */
        if (q->h == p->h && !memcmp(&q->k, &p->k, sizeof p->k)) {
            free(p);
            return;
        }
    p->layout = vk_playout(p->k.arrays);
    p->fs = p->k.fs ? p->k.fs : null_fs();
    pipe_parts(p);
    p->prewarm = 1;
    if (((const PfLoad*)ctx)->rebuild) pf_pipe_record(&p->k);
    pm_lock(&g_pipeline.pj_lock);
    if (!pipe_workers()) { /* made when a draw asks for it, then */
        pm_unlock(&g_pipeline.pj_lock);
        free(p);
        return;
    }
    pipe_queue(p, 2);
    pm_unlock(&g_pipeline.pj_lock);
    p->next = g_pipeline.pf.pipes[bucket]; /* the workers don't use next */
    g_pipeline.pf.pipes[bucket] = p;
    g_pipeline.pf.npipe++;
}

/* PF_GUEST_VERSION: what a PF_SOURCE or PF_PIPE record means, beyond the sizes the key holds. Raised when a PipeKey
   field or a key word (vk_shader_get's mask) comes to mean something else, so that the records of an earlier build
   are dropped rather than made into pipelines and shaders of the wrong state. 7: a PipeKey holds the fragment
   module's specialization constants (fs_spec), which a record of 6 has nowhere: it would be made as a pipeline of the
   default constants, whatever alpha function and origin its program was drawn with */
#define PF_GUEST_VERSION 7
/* The file's key: whatever changes the SPIR-V translation makes or what a PipeKey means. First its guest part, which
   the PF_SOURCE and PF_PIPE records depend on (pf_open): the device; the features and limits the translator,
   vk_shader_get and pipe_create consult; the sizes of what those records hold whole. Then the rest, which only the
   SPIR-V and ShaderInfo of the PF_SHADER records depend on: the build (commit, compiler, host), and for a tree with
   changes, which has one commit for every build of it, the runtime's own file; the driver. The guest part's length
   goes in *guest */
static void pf_key(char* out, size_t n, size_t* guest) {
    const VkPhysicalDeviceProperties* d = &g_vk.props;
    const char* commit = snx_build_commit();
    size_t len = strlen(commit);
    char exe[64] = "", uuid[33];
    /* 0 no texture arrays, 1 arrays of SNX_TEX_ARRAY (every file's word before arrays came smaller), else 1 and the
       elements */
    u32 arrays = !g_shader_texture_arrays ? 0u : g_shader_tex_array == SNX_TEX_ARRAY ? 1u : 1u + g_shader_tex_array;
    u32 caps[] = {(u32)g_vk.has_divisor,
                  (u32)g_vk.has_dynamic_stride,
                  (u32)g_vk.has_shader_draw_parameters,
                  (u32)g_vk.has_feedback,
                  (u32)g_vk.has_feedback_dynamic,
                  g_vk.advanced_blend_caps.available,
                  g_vk.advanced_blend_caps.coherent,
                  g_vk.advanced_blend_caps.max_attachments,
                  g_vk.advanced_blend_caps.nonpremultiplied_source,
                  g_vk.advanced_blend_caps.correlated_overlap,
                  g_vk.advanced_blend_caps.all_operations,
                  (u32)g_vk.has_fragment_interlock,
                  arrays,
                  (u32)g_shader_stage_point_size,
                  (u32)g_shader_output_layer,
                  (u32)g_shader_geometry,
                  (u32)g_shader_float16,
                  (u32)g_shader_half_rounding,
                  (u32)g_shader_i2f_nearest,
                  (u32)g_shader_geometry_streams,
                  (u32)g_shader_geometry_streams_lines_triangles,
                  (u32)g_shader_tessellation,
                  (u32)g_shader_xfb_stores,
                  (u32)g_vk.has_xfb,
                  (u32)g_shader_clock_scopes,
                  (u32)g_vk_flush_denorm,
                  (u32)g_vk_preserve_infnan,
                  (u32)g_vk_preserve_infnan64,
                  (u32)g_vk_subgroup_stages,
                  (u32)g_vk.has_provoking_last,
                  (u32)g_vk.has_conservative,
                  g_vk_stage_sets,
                  g_vk_buffer_image_slots,
                  g_shader_uniform_cbufs,
                  d->limits.maxPerStageDescriptorStorageImages,
                  d->limits.maxDescriptorSetStorageImages,
                  (u32)g_vk.has_mixed_samples,
                  (u32)g_vk.has_descriptor_heap_support,
                  (u32)g_vk.has_descriptor_buffers,
                  (u32)g_shader_dynamic_texture_offsets,
                  g_vk.feat13.maintenance4,
                  (u32)g_vk.has_depth_range_unrestricted,
                  (u32)g_vk.vertex_input_dynamic};
    /* the last word, dynamic vertex input, only where it is on: its keys hold no vertex layout (PipeKey.vi), and the
       files of every device without it keep the key they have */
    size_t ncaps = sizeof caps / sizeof *caps - !g_vk.vertex_input_dynamic;
    /* a tree with changes builds under one commit name: the runtime's own bytes tell those builds apart. Not its
       modification time, which a copy of the same runtime (each game has one) changes */
    if (len >= 6 && !strcmp(commit + len - 6, "-dirty")) {
        char* self = snx_executable("scarletnx");
        PFileMap m;
        if (self && !plat_file_map(&m, self) && m.data) {
            snprintf(exe, sizeof exe, " exe=%016llx", (unsigned long long)vk_hash(m.data, (size_t)m.size));
            plat_file_unmap(&m);
        }
        free(self);
    }
    for (int i = 0; i < 16; i++) snprintf(uuid + 2 * i, 3, "%02x", d->pipelineCacheUUID[i]);
    int g =
        snprintf(out, n, "guest=%d gpu=%04x:%04x features=%016llx caps=%016llx sizes=%u,%u,%u,%u;", PF_GUEST_VERSION,
                 d->vendorID, d->deviceID, (unsigned long long)vk_hash(&g_vk.feat, sizeof g_vk.feat),
                 (unsigned long long)vk_hash(caps, ncaps * sizeof *caps), (unsigned)sizeof(PipeKey),
                 (unsigned)sizeof(PfSource), (unsigned)sizeof(PfPipe), (unsigned)sizeof(ShXfb));
    *guest = g > 0 && (size_t)g < n ? (size_t)g : 0;
    if (*guest)
        snprintf(
            out + *guest, n - *guest,
            " %s%s driver=%u api=%u uuid=%s ftz=%d quad=%d,%d mixed_dot=%d hmma_coop=%d hmma_pairs=%d hmma_stage=%d half_flush=%d "
            "sizes=%u,%u vertex_bounds=%d",
            snx_build_info(), exe, d->driverVersion, d->apiVersion, uuid, g_vk_ftz, g_shader_quad,
            g_shader_quad_compute, g_shader_mixed_dot, g_shader_hmma_coop, g_shader_hmma_pairs, g_shader_hmma_stage,
            g_shader_half_flush, (unsigned)sizeof(ShaderInfo), (unsigned)sizeof(PfShader), g_vk.has_vertex_robustness2);
}

void vk_pipeline_file_key(char* out, size_t n, size_t* guest) { pf_key(out, n, guest); }

/* ---- the shaders of the programs the game ships (gpu/shipped_pass.c finds them), translated before a draw asks */
#define SHIP_MAX (4ull << 30) /* the shipped file's limit: one game's 2,700 programs made 245 MB of SPIR-V */
/* the scan records (shipped_pass.h's SHIP_FILE and SHIP_ALL) as a set, by what and id mixed: never 0, a free slot */
static u64 scan_key(u64 what, u64 id) {
    u64 k = (id ^ what * 0x9E3779B97F4A7C15ull) * 0xBF58476D1CE4E5B9ull;
    return k ? k : 1;
}
static u64* scan_slot(u64 k) {
    u32 i = (u32)(k >> 32) & (g_pipeline.pf.scap - 1);
    while (g_pipeline.pf.scans[i] && g_pipeline.pf.scans[i] != k) i = (i + 1) & (g_pipeline.pf.scap - 1);
    return &g_pipeline.pf.scans[i];
}
static void scan_add(u64 what, u64 id) {
    u64 k = scan_key(what, id), *e;
    if ((g_pipeline.pf.nscans + 1) * 2 > g_pipeline.pf.scap) { /* at most half full */
        u32 cap = g_pipeline.pf.scap ? g_pipeline.pf.scap * 2 : 1024, old = g_pipeline.pf.scap, i;
        u64 *was = g_pipeline.pf.scans, *grown = calloc(cap, sizeof *grown);
        if (!grown) return;
        g_pipeline.pf.scans = grown;
        g_pipeline.pf.scap = cap;
        for (i = 0; i < old; i++)
            if (was[i]) *scan_slot(was[i]) = was[i];
        free(was);
    }
    e = scan_slot(k);
    if (!*e) {
        *e = k;
        g_pipeline.pf.nscans++;
    }
}
static int scan_has(u64 what, u64 id) { return g_pipeline.pf.scap && *scan_slot(scan_key(what, id)); }
/* a line of the pass: in the log, and among the latest the screen shows under its steps */
static void pf_note(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    LOG("vulkan: %s\n", line); /* whole; the screen's, as much as its row holds */
    pm_lock(&g_pipeline.pf.note_lock);
    snprintf(g_pipeline.pf.note[g_pipeline.pf.nnote++ % RENDERER_NOTES], sizeof g_pipeline.pf.note[0], "%s", line);
    pm_unlock(&g_pipeline.pf.note_lock);
}
static void pf_ship_found(VkSh* s) {
    s->shipped = 2;
    __atomic_add_fetch(&g_pipeline.pf.ship_used, 1, __ATOMIC_RELAXED);
}

/* the index's slot for a key: its entry, or the free one where it would go (NULL with no index) */
static ShipIdx* idx_slot(u64 hash, int stage, u32 clip, u32 len, u64 qh, u32 len_a, u64 qh_a) {
    u32 i;
    if (!g_pipeline.pf.icap) return NULL;
    for (i = (u32)(hash * 0x9E3779B97F4A7C15ull >> 32) & (g_pipeline.pf.icap - 1); g_pipeline.pf.idx[i].at;
         i = (i + 1) & (g_pipeline.pf.icap - 1)) {
        const ShipIdx* e = &g_pipeline.pf.idx[i];
        if (e->hash == hash && e->stage == (u32)stage && e->clip == clip && e->clen == len && e->qh == qh &&
            e->clen_a == len_a && e->qh_a == qh_a)
            break;
    }
    return &g_pipeline.pf.idx[i];
}
static void idx_add(u64 hash, int stage, u32 clip, u32 len, u64 qh, u32 len_a, u64 qh_a, u64 at) {
    ShipIdx* e;
    if ((g_pipeline.pf.nidx + 1) * 2 > g_pipeline.pf.icap) { /* at most half full */
        u32 cap = g_pipeline.pf.icap ? g_pipeline.pf.icap * 2 : 4096, old = g_pipeline.pf.icap, i;
        ShipIdx *was = g_pipeline.pf.idx, *grown = calloc(cap, sizeof *grown);
        if (!grown) return;
        g_pipeline.pf.idx = grown;
        g_pipeline.pf.icap = cap;
        for (i = 0; i < old; i++)
            if (was[i].at)
                *idx_slot(was[i].hash, (int)was[i].stage, was[i].clip, was[i].clen, was[i].qh, was[i].clen_a,
                          was[i].qh_a) = was[i];
        free(was);
    }
    e = idx_slot(hash, stage, clip, len, qh, len_a, qh_a);
    if (e->at) return; /* the file has it twice */
    *e = (ShipIdx){hash, qh, qh_a, at, len, len_a, (u32)stage, clip};
    g_pipeline.pf.nidx++;
}
/* The key's clip word the pass translates a stage with, as the draws ask for it (render.c): the clip distances the 3D
   engine enables, all eight (0xff) in the NVN games tried, for the stages that can be the last before rasterization
   (docs/layers/gpu/nvn.md); SNX_SH_NOT_LAST for tessellation control, which never is; none for fragment */
static u32 ship_clip(int stage) { return stage == ST_TCS ? SNX_SH_NOT_LAST : stage == ST_FS ? 0 : 0xFFu; }
/* a key whose words are all zero but the clip word: the keys the pass translates with */
static int ship_key_only_clip(const u32 mask[26]) {
    for (int i = 0; i < 26; i++)
        if (i != 6 && mask[i]) return 0;
    return 1;
}
/* a shader record of the shipped file (len bytes at p) into r, its constant buffer reads (*cbr, the program's own
   constants that its jump tables were read from) and its SPIR-V: NULL unless sh_from_record can make it (a stage a
   draw binds, its words within it). They are 8-byte aligned in the view: a record, its header and its fixed part are
   multiples of 8 */
static const u32* ship_record(const u8* p, u32 len, PfShader* r, const u32** cbr) {
    if (!p || len < sizeof *r) return NULL;
    memcpy(r, p, sizeof *r);
    if (r->stage > ST_FS || r->nspv < 5 || (u64)r->ncbr * 16 + (u64)r->nspv * 4 > len - sizeof *r) return NULL;
    *cbr = (const u32*)(const void*)(p + sizeof *r);
    return (const u32*)(const void*)(p + sizeof *r + (size_t)r->ncbr * 16);
}
/* on the reading thread: a record of the shipped file, into the index (a shader) or the scans' set */
static void pf_ship_rec(void* ctx, u32 type, const u8* p, u32 len, u64 at) {
    PfShader r;
    const u32* cbr;
    (void)ctx;
    if (type == PF_SCAN && len == 16) {
        u64 w[2];
        memcpy(w, p, sizeof w);
        scan_add(w[0], w[1]);
    } else if (type == PF_SHADER && ship_record(p, len, &r, &cbr) && ship_key_only_clip(r.mask))
        idx_add(r.hash, (int)r.stage, r.mask[6], r.clen, r.qh, r.clen_a, r.qh_a, at);
    else if (type == PF_ZSHADER && len >= sizeof r) { /* its PfShader as it is: the rest is unpacked once a draw asks */
        memcpy(&r, p, sizeof r);
        if (r.stage <= ST_FS && ship_key_only_clip(r.mask))
            idx_add(r.hash, (int)r.stage, r.mask[6], r.clen, r.qh, r.clen_a, r.qh_a, at);
    }
}
/* on the reading thread: a record of the scan file, into the scans' set */
static void pf_scan_rec(void* ctx, u32 type, const u8* p, u32 len, u64 at) {
    u64 w[2];
    (void)ctx;
    (void)at;
    if (type != PF_SCAN || len != sizeof w) return;
    memcpy(w, p, sizeof w);
    if (w[0] == SHIP_NONE) scan_add(w[0], w[1]);
}
/* the shipped file's record that starts at at, into r, its check, its reads and its SPIR-V: NULL when it isn't one.
   A packed one's are in *owned, for the caller to free once the module is made */
static const u32* ship_payload(u64 at, PfShader* r, u64* check, const u32** cbr, u8** owned) {
    u32 n = 0;
    const u8* p = pf_payload(&g_pipeline.pf.ship, at, &n, check, owned);
    return ship_record(p, n, r, cbr);
}
/* on the reading thread: a PF_SHIPREF of the main file, a shader of the shipped file that an earlier run's draw found,
   made into a module as the main file's own shaders are */
static u64 pf_load_shipref(void* ctx, u64 at, u64 check) {
    PfShader r;
    u64 c = 0;
    u8* owned;
    const u32* cbr;
    const u32* spv = ship_payload(at, &r, &c, &cbr, &owned);
    VkSh* s = spv && c == check ? sh_from_record(&r, cbr, spv) : NULL;
    (void)ctx;
    free(owned); /* sh_from_record copied the reads */
    if (!s) return 0;
    pf_sh_append(s);
    return mod_bits(s->mod);
}
/* A draw's shader the game ships, from the shipped file (vk_shader_get, after the tables had none, vk_lock), once the
   pass is done with it (ship_ready): a key of zeros but its clip word, the way the pass translated it (ship_clip). Its
   record is made into a module now, and named in the main file by a PF_SHIPREF, so that the pipelines made with it
   are kept too. One whose jump tables the pass read from the program's constants is taken only while the draw's
   constant buffer 1 holds the same words, as every shader with reads is. NULL when the file has none */
static VkSh* pf_ship_take(u64 h, int stage, const u32 mask[26], u32 len, u64 qh, u32 len_a, u64 qh_a) {
    const ShipIdx* e;
    const u32 *spv, *cbr;
    PfShader r;
    u64 check = 0;
    u8* owned;
    VkSh* s;
    if (!g_pipeline.pf.on || !__atomic_load_n(&g_pipeline.pf.ship_ready, __ATOMIC_ACQUIRE) ||
        !ship_key_only_clip(mask) || !(e = idx_slot(h, stage, mask[6], len, qh, len_a, qh_a)) || !e->at)
        return NULL;
    spv = ship_payload(e->at, &r, &check, &cbr, &owned);
    /* the record there is the one indexed, unless another process rewrote the file before it was mapped again */
    s = !spv || r.hash != h || r.stage != (u32)stage || r.clen != len || r.qh != qh || r.clen_a != len_a ||
                r.qh_a != qh_a || memcmp(r.mask, mask, sizeof r.mask)
            ? NULL
            : sh_from_record(&r, cbr, spv);
    free(owned); /* sh_from_record copied the reads */
    if (!s) return NULL;
    if (!vk_cb_reads_hold(&s->cbr, sh_cb_read)) {
        vkDestroyShaderModule(g_vk.dev, s->mod, NULL);
        free(s->cbr.r);
        free(s);
        return NULL;
    }
    pf_shipref(&g_pipeline.pf.file, mod_bits(s->mod), e->at, check);
    pf_ship_found(s);
    return s;
}

/* What the pass hands each program to (a ShipSink's, on the reading thread). A program without a draw has no constant
   buffer but its own constants, which its draws bind as buffer 1 (shipped_programs finds them in a BNSH): the
   compiler's jump tables of BRX are read from them, and a translation that reads anything else fails (sh_spirv's
   shipped). g_pipeline.ship_consts: the constants of the program the pass translates now, the reading thread's alone */
static int ship_read(u32 idx, u32 off, u32* out) {
    if (idx != 1 || g_pipeline.ship_nconsts < 4 || off > g_pipeline.ship_nconsts - 4) return 0;
    *out = snx_ld_le32(g_pipeline.ship_consts + off);
    return 1;
}
/* SH_CODE bytes from p (avail of them), as vk_shader_get reads a program from guest memory: zeros past them */
static u8* ship_window(const u8* p, size_t avail) {
    u8* code = calloc(1, SH_CODE);
    if (code) memcpy(code, p, avail < SH_CODE ? avail : SH_CODE);
    return code;
}
/* a shipped program large enough that its part takes about a second to make: make that part into the pipeline cache now,
   on the lowest worker queue, so a draw that meets the program links it (a millisecond) instead of compiling it
   (seconds). The program's indexed bit and its clip word are the ship pass's guesses, so a draw whose key differs
   still translates and compiles its own, as now (docs/layers/gpu/pipeline-libraries.md, step 8) */
static void ship_warm(int stage, int indexed, const u32* spv, u32 nw) {
    VkShaderModule mod = vk_module_spv(spv, nw);
    if (!mod) return;
    WarmItem* w = calloc(1, sizeof *w);
    if (!w) {
        vkDestroyShaderModule(g_vk.dev, mod, NULL);
        return;
    }
    if (stage == ST_VS) {
        w->part = VK_PART_PR;
        w->src.vs = mod;
        w->src.arrays = indexed ? (1u << shader_vk_set(ST_VS)) : 0;
    } else { /* ST_FS: the pass makes no other stage */
        w->part = VK_PART_FS;
        w->src.fs = mod;
        w->src.arrays = indexed ? (1u << shader_vk_set(ST_FS)) : 0;
        w->fs = mod;
    }
    pm_lock(&g_pipeline.pj_lock);
    if (!pipe_workers()) { /* no workers: the parts are made when the draws ask, as now */
        pm_unlock(&g_pipeline.pj_lock);
        vkDestroyShaderModule(g_vk.dev, mod, NULL);
        free(w);
        return;
    }
    w->next = NULL;
    if (g_pipeline.warm_tail)
        g_pipeline.warm_tail->next = w;
    else
        g_pipeline.warm_head = w;
    g_pipeline.warm_tail = w;
    __atomic_add_fetch(&g_pipeline.plib.warm_queued, 1, __ATOMIC_RELAXED);
    pc_signal(&g_pipeline.pj_work);
    pm_unlock(&g_pipeline.pj_lock);
}
/* the shader of code (hashing to h, as vk_shader_get hashes it) a draw asks for first: float attributes, textures
   and outputs, no capture, not early-Z (a fragment shader's first lookup, render.c), and the clip word of ship_clip.
   Translated into the shipped file unless it has it: SHIP_NEW, SHIP_KNOWN, SHIP_FAILED, or SHIP_STOP when the file
   takes no more */
static int ship_make(const u8* code, u64 h, int stage, u32 len, u64 qh, u32 len_a, u64 qh_a) {
    /* Absolute calls name the draw's loaded GPU address, which shipped source discovery does not yet know. */
    if (shader_absolute_flow(code, len)) return SHIP_SKIPPED;
    u32 mask[26] = {0};
    VkSh s = {0};
    size_t nw;
    u64 at = 0;
    const ShipIdx* e;
    mask[6] = ship_clip(stage);
    e = idx_slot(h, stage, mask[6], len, qh, len_a, qh_a);
    if (e && e->at) return SHIP_KNOWN;
    u32* spv = sh_spirv(&s, code, h, stage, mask, len, qh, len_a, qh_a, NULL, ship_read, 1, &nw);
    int translated = spv != NULL;
    if (spv) at = pf_shader_into(&g_pipeline.pf.ship, &s, 0, spv, nw, NULL, 0, NULL);
    if (translated && nw >= SHIP_WARM_WORDS) ship_warm(stage, s.indexed, spv, nw);
    free(spv);
    free(s.cbr.r);
    /* An allocation failing inside the translator comes back as a program it can't translate, with no reason given,
       and is counted as failed, for this build and driver. Taking every failure without a reason as one to try again
       would walk the files again on every run of a game with a program that fails so */
    if (!translated) {
        LOG("vulkan: %s %016llx not translated: %s\n", shipped_stage_name(stage), (unsigned long long)h,
            s.si.first_unsupported[0] ? s.si.first_unsupported : "unsupported feature or float controls");
        return SHIP_FAILED;
    }
    if (!at) return SHIP_STOP;
    idx_add(h, stage, mask[6], len, qh, len_a, qh_a, at);
    __atomic_add_fetch(&g_pipeline.pf.ship_new, 1, __ATOMIC_RELAXED);
    return SHIP_NEW;
}
/* A VertexA with the VertexB after it, merged as a draw that binds the two merges them (vk_shader_get): the VertexB is
   the first SPH of one in the 256-byte slots past A's code, where NVN's compiler puts it (docs/layers/gpu/nvn.md). A
   VertexA without one, or that won't merge, is SHIP_SKIPPED: no draw asks for it alone */
static int ship_dual(const u8* sph, size_t avail) {
    u8 *a = ship_window(sph, avail), *b = NULL, *m = NULL;
    u32 len_a = 0, len = 0, mlen;
    size_t at;
    int rc = SHIP_SKIPPED;
    u64 qh_a;
    if (!a) return SHIP_LACKED;
    vk_shader_hash(a, SH_CODE, &len_a, ship_read);
    qh_a = vk_hash(a, len_a);
    for (at = ((size_t)len_a + 255) & ~(size_t)255; at < SH_CODE && avail >= 0x50 && at <= avail - 0x50; at += 256)
        if (shipped_sph_stage(sph + at) == ST_VS) break;
    if (at < SH_CODE && avail >= 0x50 && at <= avail - 0x50) {
        b = ship_window(sph + at, avail - at);
        m = malloc(SH_CODE);
        if (!b || !m)
            rc = SHIP_LACKED;
        else {
            vk_shader_hash(b, SH_CODE, &len, ship_read);
            if (sh_merge_dual(a, b, m, ship_read)) {
                u64 h = vk_shader_hash(m, SH_CODE, &mlen, ship_read);
                rc = ship_make(m, h, ST_VS, len, vk_hash(b, len), len_a, qh_a);
            }
        }
    }
    free(m);
    free(b);
    free(a);
    return rc;
}
/* whether the Ounce program from its header at sph reads or writes the ISBE (sm86_uses_isbe) */
static int ship_isbe(const u8* sph, size_t avail) {
    Sm86Program p;
    u32 size = avail < SH_CODE ? (u32)avail : SH_CODE;
    return sm86_find_programs(sph, size, &p, 1) == 1 && sm86_uses_isbe(sph + p.code, p.size);
}
static int ship_program(void* ctx, const u8* sph, size_t avail, int stage, const u8* consts, size_t nconsts) {
    u8* code;
    u32 len;
    u64 h;
    int rc;
    (void)ctx;
    /* a VertexA and its VertexB share one code area, whose constants the draws bind for the vertex stage */
    g_pipeline.ship_consts = consts;
    g_pipeline.ship_nconsts = nconsts;
    /* an Ounce game's (SM86): its programs as a draw reads them, from their header. A VertexA, which no draw of the
       Ounce path pairs, is left to its draws */
    if (stage == SHIPPED_OUNCE_VERTEX_A) return SHIP_SKIPPED;
    /* a tessellation control program that runs no invocation: draws take the passthrough in its place */
    if (stage == (ST_TCS | SHIPPED_OUNCE) && sm86_tcs_empty(snx_ld_le32(sph + 8))) return SHIP_SKIPPED;
    /* a task or mesh program has the SPH of a vertex or evaluation program, and reads and writes the ISBE. Only its
       draw's control section gives its stage and the fields of its translation (ShaderInfo.mesh) */
    if ((stage == (ST_VS | SHIPPED_OUNCE) || stage == (ST_TES | SHIPPED_OUNCE)) && ship_isbe(sph, avail))
        return SHIP_SKIPPED;
    stage &= ~SHIPPED_OUNCE;
    if (stage == SHIPPED_VERTEX_A) return ship_dual(sph, avail);
    /* their keys hold the draw's state (geometry's input primitive and passthrough words, the evaluation's domain), so
       no key made without a draw is one a draw asks for */
    if (stage == ST_GS || stage == ST_TES) return SHIP_SKIPPED;
    if (!(code = ship_window(sph, avail))) return SHIP_LACKED;
    h = vk_shader_hash(code, SH_CODE, &len, ship_read);
    rc = ship_make(code, h, stage, len, vk_hash(code, len), 0, 0);
    free(code);
    return rc;
}
static int ship_scanned(void* ctx, u32 what, u64 id) {
    (void)ctx;
    return scan_has(what, id);
}
static void ship_mark(void* ctx, u32 what, u64 id) {
    (void)ctx;
    pf_scan(what == SHIP_NONE ? &g_pipeline.pf.scan : &g_pipeline.pf.ship, what, id);
    scan_add(what, id);
}
/* the shipped file takes no more (a write failed, or it is full): the pass's translations would be thrown away. The
   pass alone appends to it, so its f is this thread's to read */
static int ship_stopped(void* ctx) {
    (void)ctx;
    return !g_pipeline.pf.ship.f;
}
static void ship_note(void* ctx, const char* line, int shown) {
    (void)ctx;
    if (shown)
        pf_note("%s", line);
    else
        LOG("vulkan: %s\n", line);
}
static void ship_at(void* ctx, const char* name) {
    (void)ctx;
    pm_lock(&g_pipeline.pf.note_lock);
    snprintf(g_pipeline.pf.walk_at, sizeof g_pipeline.pf.walk_at, "%s", name);
    pm_unlock(&g_pipeline.pf.note_lock);
}
/* on the reading thread, once the files were read: the programs the game ships translated into the shipped file. The
   draws translate their own meanwhile, and find these once it ends (ship_ready) */
static void pf_ship_pass(void) {
    static const ShipSink sink = {NULL, ship_scanned, ship_mark, ship_program, ship_stopped, ship_note, ship_at};
    u64 t0 = plat_ns();
    pf_unseal(&g_pipeline.pf.ship);
    /* a file that takes no more (its limit, which the smallest record would pass): every program would be counted,
       read and translated only to be thrown away, on every run */
    if (g_pipeline.pf.ship.f && g_pipeline.pf.ship.size + PF_REC + sizeof(PfShader) + 32 >
                                    (g_pipeline.pf.ship.max ? g_pipeline.pf.ship.max : PF_MAX))
        LOG("vulkan: %s is full, so the game's own shaders aren't looked for\n", g_pipeline.pf.ship_path);
    else if (g_pipeline.pf.ship.f) {
        char key[64];
        snprintf(key, sizeof key, "walk=%s", ship_walk_version());
        pf_open_mapped(&g_pipeline.pf.scan, g_pipeline.pf.scan_path, key, pf_scan_rec, NULL);
        pf_unseal(&g_pipeline.pf.scan);
        __atomic_store_n(&g_pipeline.pf.scanning, 1, __ATOMIC_RELAXED);
        int rc = ship_walk(&sink, &g_pipeline.pf.progress, g_settings.prepare_shaders == 2, g_ounce);
        __atomic_store_n(&g_pipeline.pf.scanning, 0, __ATOMIC_RELAXED);
        pf_close(&g_pipeline.pf.scan);
        LOG("vulkan: the game's own shaders: %u translated new, %u in %s, in %llu ms%s; %llu large ones queued to warm "
            "their parts\n",
            __atomic_load_n(&g_pipeline.pf.ship_new, __ATOMIC_RELAXED), g_pipeline.pf.nidx, g_pipeline.pf.ship_path,
            (unsigned long long)((plat_ns() - t0) / 1000000),
            rc > 0   ? " (stopped: the file takes no more)"
            : rc < 0 ? " (a later run reads what wasn't)"
                     : "",
            (unsigned long long)__atomic_load_n(&g_pipeline.plib.warm_queued, __ATOMIC_RELAXED));
    } /* else pf_open_mapped or pf_unseal said why the file can't be written */
    pf_seal(&g_pipeline.pf.ship); /* mapped again with the records appended, for the draws to read */
}
void vk_pipeline_prepare(RendererPrepare* p) {
    int loaded;
    *p = (RendererPrepare){0};
    if (!g_pipeline.pf.on) {
        p->done = 1;
        return;
    }
    pm_lock(&g_pipeline.pf.lock);
    loaded = g_pipeline.pf.loaded;
    pm_unlock(&g_pipeline.pf.lock);
    p->files = __atomic_load_n(&g_pipeline.pf.progress.files, __ATOMIC_RELAXED);
    p->files_seen = __atomic_load_n(&g_pipeline.pf.progress.files_seen, __ATOMIC_RELAXED);
    p->programs = __atomic_load_n(&g_pipeline.pf.progress.programs, __ATOMIC_RELAXED);
    p->programs_done = __atomic_load_n(&g_pipeline.pf.progress.programs_done, __ATOMIC_RELAXED);
    p->shipped = __atomic_load_n(&g_pipeline.pf.ship_new, __ATOMIC_RELAXED);
    p->scanning = __atomic_load_n(&g_pipeline.pf.scanning, __ATOMIC_RELAXED);
    p->shaders = __atomic_load_n(&g_pipeline.pf.shaders_total, __ATOMIC_RELAXED);
    p->pipes = __atomic_load_n(&g_pipeline.pf.pipes_total, __ATOMIC_RELAXED);
    p->pipes_made = (u32)__atomic_load_n(&g_pipeline.pf.made, __ATOMIC_RELAXED);
    p->loaded = loaded;
    p->done = loaded && __atomic_load_n(&g_pipeline.pf.ship_ready, __ATOMIC_ACQUIRE) && p->pipes_made >= p->pipes;
    pm_lock(&g_pipeline.pf.note_lock);
    p->notes = g_pipeline.pf.nnote < RENDERER_NOTES ? (int)g_pipeline.pf.nnote : RENDERER_NOTES;
    for (int i = 0; i < p->notes; i++) /* oldest first */
        memcpy(p->note[i], g_pipeline.pf.note[(g_pipeline.pf.nnote - (u32)p->notes + (u32)i) % RENDERER_NOTES],
               sizeof p->note[i]);
    memcpy(p->file, g_pipeline.pf.walk_at, sizeof p->file);
    pm_unlock(&g_pipeline.pf.note_lock);
}

/* the reading thread (arg non-NULL), or vk_pipeline_init's when there is none, which doesn't run the pass: the
   shipped file first, whose records the main file's PF_SHIPREFs name, then the main file, then the pass */
static void pf_load(void* arg) {
    PfLoad load = {pf_load_shader, pf_load_pipe, pf_load_shipref, pf_load_source, NULL, 0, pf_load_cspipe};
    char key[1024];
    size_t guest;
    u32 loaded[2] = {0, 0};
    u64 t0 = plat_ns();
    load.ctx = &load; /* pf_load_pipe reads rebuild */
    SNX_ZONE(z_load, "pipeline file load");
    pf_key(key, sizeof key, &guest);
    g_pipeline.pf.ship.max = SHIP_MAX;
    g_pipeline.pf.ship.pack = g_settings.compress_prepared_shaders ? PF_PACK_SHADERS : 0;
    g_pipeline.pf.file.pack = g_settings.compress_shader_cache ? PF_PACK_UNITS : 0;
    {
        FILE* there = fopen(g_pipeline.pf.path, "rb");
        if (there)
            fclose(there);
        else
            LOG("vulkan: no shader cache of %s (driver %s) yet: %s is made\n", g_vk.driver_name, g_pipeline.driver_tag,
                g_pipeline.pf.path);
    }
    pf_open_mapped(&g_pipeline.pf.ship, g_pipeline.pf.ship_path, key, pf_ship_rec, NULL);
    pf_open(&g_pipeline.pf.file, g_pipeline.pf.path, key, guest, &load, loaded);
    LOG("vulkan: %u shaders and %u pipelines of earlier runs with %s (driver %s) loaded from %s in %llu ms, the "
        "pipelines to be made in the background, and %u compute pipelines made (%u could not be); %u shaders of the "
        "game's own programs in %s\n",
        g_pipeline.pf.nsh, g_pipeline.pf.npipe, g_vk.driver_name, g_pipeline.driver_tag, g_pipeline.pf.path,
        (unsigned long long)((plat_ns() - t0) / 1000000), g_pipeline.pf.ncs, g_pipeline.pf.ncs_failed,
        g_pipeline.pf.nidx, g_pipeline.pf.ship_path);
    __atomic_store_n(&g_pipeline.pf.shaders_total, g_pipeline.pf.nsh, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pipeline.pf.pipes_total, g_pipeline.pf.npipe, __ATOMIC_RELAXED);
    pm_lock(&g_pipeline.pf.lock);
    g_pipeline.pf.loaded = 1;
    pc_broadcast(&g_pipeline.pf.done);
    pm_unlock(&g_pipeline.pf.lock);
    if (arg && g_settings.prepare_shaders) pf_ship_pass(); /* translating needs this thread's stack */
    ship_release();
    __atomic_store_n(&g_pipeline.pf.ship_ready, 1, __ATOMIC_RELEASE); /* the index and the view no longer change */
}
static void pf_load_thread(void* arg) {
    SNX_THREAD_NAME("pipeline load");
    pf_load(arg);
}
/* at the end of vk_pipeline_init: the files are read on a thread of its own, so startup doesn't wait for them */
static void pf_start_load(void) {
    pm_init(&g_pipeline.pf.file.lock);
    pm_init(&g_pipeline.pf.ship.lock);
    pm_init(&g_pipeline.pf.scan.lock);
    pm_init(&g_pipeline.pf.note_lock);
    pm_init(&g_pipeline.pf.lock);
    pc_init(&g_pipeline.pf.done);
    if (vk_corpus_dir()) {
        LOG("vulkan: shaders are recorded (shader_corpus or shader_captures), so every one is translated and none is "
            "kept for the next run\n");
        return;
    }
    g_pipeline.pf.on = 1;
    null_fs(); /* made here, before the reading thread uses it */
    snx_vk_driver_path(g_pipeline.pf.path, sizeof g_pipeline.pf.path, g_pipeline.cache_dir, g_exename,
                       g_pipeline.driver_tag, ".vkshaders");
    snx_vk_driver_path(g_pipeline.pf.ship_path, sizeof g_pipeline.pf.ship_path, g_pipeline.cache_dir, g_exename,
                       g_pipeline.driver_tag, ".vkshipped");
    snprintf(g_pipeline.pf.scan_path, sizeof g_pipeline.pf.scan_path, "%s/%s.vkscan", g_pipeline.cache_dir, g_exename);
    snx_path_native(g_pipeline.pf.path);
    snx_path_native(g_pipeline.pf.ship_path);
    snx_path_native(g_pipeline.pf.scan_path);
    /* the files every driver shared before each had its own: a copy, the first time this driver runs, which its
       driver's next run finds as its own, and another starts over. Both, as the main file's records point into the
       shipped one by where its records start */
    for (int k = 0; k < 2; k++) {
        const char* path = k ? g_pipeline.pf.ship_path : g_pipeline.pf.path;
        char* legacy = snx_format("%s/%s%s", g_pipeline.cache_dir, g_exename, k ? ".vkshipped" : ".vkshaders");
        char why[256];
        u64 bytes;
        if (!legacy) continue;
        snx_path_native(legacy);
        int r = pf_adopt(path, legacy, &bytes, why, sizeof why);
        if (r > 0)
            LOG("vulkan: %s, the file every driver shared before each had its own, stays where it is, copied (%llu "
                "KB) as the start of %s, which was not there\n",
                legacy, (unsigned long long)(bytes >> 10), path);
        else if (r < 0)
            WARN("vulkan: cannot copy %s to %s (%s): %s of %s starts empty\n", legacy, path, why,
                 k ? "the shaders of the game's own programs" : "the shader cache", g_vk.driver_name);
        free(legacy);
    }
    if (g_settings.prepare_shaders) ship_snapshot(); /* before the game's first thread writes its data */
    /* the translator's recursion wants the GPU thread's stack */
    if (plat_thread(pf_load_thread, &g_pipeline.pf, (size_t)16 << 20)) pf_load(NULL); /* no thread: read here */
}
/* the reading thread's shaders and pipelines into the tables, waiting for it the first time a draw needs them
   (vk_lock). Not for the pass, which may run on long after */
static void pf_merge(void) {
    VkSh* s;
    Pipe* p;
    if (g_pipeline.pf.merged) return;
    g_pipeline.pf.merged = 1;
    if (!g_pipeline.pf.on) return;
    gpu_wait_note("the shaders and pipelines of earlier runs read", 0);
    pm_lock(&g_pipeline.pf.lock);
    while (!g_pipeline.pf.loaded) pc_wait(&g_pipeline.pf.done, &g_pipeline.pf.lock, -1);
    pm_unlock(&g_pipeline.pf.lock);
    gpu_wait_note(NULL, 0);
    /* in the file's order, so the last made is found first, as in the run that made them */
    while ((s = g_pipeline.pf.sh) != NULL) {
        g_pipeline.pf.sh = s->next;
        s->next = g_pipeline.shh[s->hash & 1023];
        g_pipeline.shh[s->hash & 1023] = s;
    }
    g_pipeline.pf.sh_tail = NULL;
    for (u32 b = 0; b < PIPEH; b++)
        while ((p = g_pipeline.pf.pipes[b]) != NULL) {
            g_pipeline.pf.pipes[b] = p->next;
            p->next = g_pipeline.pipeh[b];
            g_pipeline.pipeh[b] = p;
        }
}
void vk_pipeline_prewarm_stats(u64* made, u64* hits, u64* left) {
    *made = __atomic_load_n(&g_pipeline.pf.made, __ATOMIC_RELAXED);
    *hits = __atomic_load_n(&g_pipeline.pf.hits, __ATOMIC_RELAXED);
    *left = g_pipeline.pf.merged && g_pipeline.pf.npipe > *made ? g_pipeline.pf.npipe - *made
                                                                : 0; /* npipe is settled once merged */
}
