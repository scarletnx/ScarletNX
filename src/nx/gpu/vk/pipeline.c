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
#include "common/utilities/spirv_emit.h" /* SPV_CAPABILITY_COOPERATIVE_MATRIX_KHR, the scanner's; the version */
#include "nx/gpu/shader/sm86.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "common/gpu/vk/vk_common.h"
#include "common/gpu/vk/extension_bans.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/shader/advanced_blend_spirv.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/vk/descriptor_heaps.h"
#include "nx/gpu/vk/capture.h"
#include "nx/gpu/vk/lookup_ways.h"
#include "nx/gpu/vk/pipeline_file.h"
#include "nx/gpu/vk/pipeline_parts.h"
#include "nx/gpu/vk/shading_rate.h"
#include "nx/gpu/vk/pipeline_stats.h"
#include "nx/gpu/vk/shader_objects.h"
#include "nx/gpu/vk/cb_fold.h"
#include "nx/gpu/vk/cb_windows.h"
#include "nx/gpu/vk/cb_dynamic.h"
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
#include <stdatomic.h>

/* SH_CODE (vk.h), the bytes of a program vk_shader_get reads from its SPH. The SPH has no size, so shader_hash ends the
   program at its EXIT or the zero padding after it, and NVN packs programs into one pool: the window has to reach past
   the largest program's end (FINAL FANTASY RESONANCE has one of 70,728 bytes, an Ounce game one of 149,120), without
   going on into the next program. Another Ounce game has fragment programs longer than 512 KiB, whose calls go
   to functions past 41,000 instructions: in a window of that size they had no end, and its shader pass left 13 of
   them untranslated. The SM86 reader takes the whole window for each program it looks up (vk_shader_read_code), and
   the shader pass a window for each program of its files: with 1 MiB, and those programs translated too, that game's
   pass took 12.1 s for 10,570 programs, where it took 9.7 to 10.1 s for 10,556 */

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
    u32 mask[SH_KEY_WORDS], epoch, wseen;
    int stage;
    VkSh* s;
    /* whether s folds as many constant buffer words as any translation of its program (sh_fold_top), as the tables
       were at g_pipeline.fold_gen: one that does and holds is the draw's without a look at the others */
    u32 fold_gen;
    int fold_top;
} ShA;
struct PipeLib;
typedef struct Pipe {
    PipeKey k;
    u64 h;
    /* once ready, the best one made (atomic): the link of its parts, then the optimized one, with pipeline libraries */
    _Atomic VkPipeline p;
    _Atomic int ready;      /* set by the worker after p, with release order */
    struct PipeLib* lib[4]; /* with pipeline libraries, its parts (vertex input, pre-rasterization, fragment shader,
                               fragment output: VK_PART_* by bit), else NULL */
    /* p is the optimized pipeline (atomic, release after p). The link it took the place of stays, as every pipeline
       does for the run: the draws keep the handles they had (render.c's g_draw), and a link freed once gave its handle
       to a new pipeline, which the headless test's watch then took for the link */
    _Atomic int opt;
    VkPipelineLayout layout;
    VkShaderModule fs; /* k.fs, or the empty fragment shader */
    int prewarm;       /* one of the file's, made before a draw asked for it */
    int queued;        /* in the workers' queue (g_pipeline.pj_lock): 1 a draw's, 2 the file's, 3 to optimize */
    /* a draw's: when it was queued, and whether every worker was busy then with one or more on an optimized
       pipeline (g_pipeline.pj_lock), for the stats of its wait for a worker */
    u64 queued_ns;
    int behind_opt;
    u32 standin;       /* its index + 1 in the stand-ins' table (vk_pipeline_get_deferred), 0 for none yet */
    /* drawn with shader objects (shader_objects.c): sh, its stages' objects, and p only a mark that they are made.
       Written before ready, read after it (atomic): a key whose objects the device refuses is made as a pipeline */
    _Atomic int so;
    VkShaderEXT sh[5];
    /* taken by vk_so_takes when the entry was made, never changed: the draws get its stand-in, and bind their sets
       with its layout of no independent sets (vk_pipeline_layout), which a pipeline made in place of refused objects
       has too */
    int so_layout;
    /* with pipeline libraries, its link's use: the presents it was bound in (the GPU thread, under vk_lock), and the
       ask for its optimized pipeline once they reached PIPE_OPT_PRESENTS (pj_lock), so that a worker queues it after
       its link (vk_pipeline_bound) */
    u32 used_present, used_presents;
    int opt_wanted;
    struct Pipe *next, *job;
} Pipe;
/* p of a Pipe drawn with shader objects once they are made: not a handle, never bound (vk_pipeline_bind) */
#define PIPE_SO_MADE ((VkPipeline)(uintptr_t)1)
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
/* A translation that folds the words a program's draws found SH_CB_FOLD_STABLE submits in a row (cb_fold.h), made on a
   pipeline worker while the draws go on with the translation they have (from): the program as a lookup reads it, and
   copies of the words the translation reads, so that the worker reads no guest memory and no draw state. Its pipelines
   for the keys of from's draws (twins) are made before a draw takes it, so the draw that does finds them made. In one
   block: the struct, nrec records (the reads, the loads' words, the others'), SH_CODE bytes of code */
/* made or warmed at once: past them, one made whose from was not drawn in SH_CB_FOLD_STABLE submits is dropped to make
   room, else the draws that would queue one count on */
#define SH_FOLD_JOBS  64
#define SH_FOLD_TWINS 16 /* the keys of from's draws whose pipelines one is made with before a draw takes it */
enum { SH_FOLD_QUEUED = 1, SH_FOLD_MAKING, SH_FOLD_MADE };
typedef struct {
    int state;  /* SH_FOLD_* (pj_lock) */
    int warm;   /* its module is made, and its twins are being made (the GPU thread's) */
    int mode;   /* VkCbFold.mode */
    u64 seq;    /* the order of the queue: a worker takes the oldest */
    VkSh* from; /* the translation whose draws found the words */
    u64 h, qh;
    u32 mask[SH_KEY_WORDS];
    u32 len, mlen;
    /* the records after the struct: the reads from's translation made (VK_CB_READ or VK_CB_UNBOUND), the words the
       draw had at its loads (VK_CB_READ, where it could read them), and the others' (VK_CB_FOLD_AGREE), nother[i] of
       each, then the code */
    u32 nrec, nreads, nloads, nothers, nother[SH_CB_FOLD_VARIANTS];
    /* a read the translation made that from's did not (the worker's until made): the translation is dropped */
    int missed;
    u32 missed_idx, missed_off;
    /* made: the translation (NULL for none), its SPIR-V and the worker's time */
    VkSh* made;
    u32* spv;
    size_t nw;
    u64 ns;
    /* warming: its twins' pipelines, the submit when the last was queued, the one it began warming in, and the last
       with a draw of from (from the queue on) */
    Pipe* twins[SH_FOLD_TWINS];
    u32 ntwins;
    u64 twin_submit, warm_submit, seen_submit;
} ShFold;
/* The file's state */
static struct {
    /* The caches live in shader_cache/ of the game's cache folder (g_cachedir), wherever the runtime was started
       from */
    char cache_dir[1100], pcache_path[1200];
    int bc7_cache;                      /* the BC7 cache is still to open (vk_pipeline_bc7_cache) */
    char driver_tag[SNX_VK_DRIVER_TAG]; /* the driver's, whose files of the cache these are (snx_vk_driver_tag) */
    _Atomic int
        pcache_dirty;        /* pipelines made since the cache was last written (atomic: it is saved outside vk_lock) */
    _Atomic u64 pcache_used; /* plat_ns when a pipeline last went into the cache (atomic) */
    /* by independent sets ([0], with pipeline libraries) or not ([1], for shader objects, playout), and by the sets
       with texture arrays: made when first used */
    _Atomic VkPipelineLayout playouts[2][1u << 5];
    PMutex playout_lock;                /* making one: the file's reading thread asks for them too */
    VkPipelineCache pcache;
    PMutex psave; /* vk_pipeline_save's: one save at a time */
    /* the draw state and stage whose bound constant buffers the translator reads, for the vk_shader_get call running */
    const DsState* sh_ds;
    int sh_bind;
    VkCbReader rec_read;
    VkCbReads* rec;
    int rec_failed;
    const VkCbFold* fold; /* how the translation running folds its constant buffer words (fold_read) */
    _Atomic int fold_off; /* vk_cb_fold_switch: no translation folds (atomic: the workers translate too) */
    u32 fold_gen;         /* counts the shaders put in shh, for ShA.fold_top */
    /* the translations that fold a program's draws' words, by slot (VkSh.fold_job - 1): the GPU thread takes and frees
       a slot under pj_lock, where the workers look for one queued (ShFold.state), fold_queued of them */
    ShFold* fold_jobs[SH_FOLD_JOBS];
    u32 fold_queued;
    u64 fold_seq;
    /* for the stats line: the pipelines made for them before a draw took one, and those not put in the tables */
    u64 fold_twins, fold_dropped;
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
    u64 sh_folds;           /* of sh_new, the translations made beside one that held, to fold more of a draw's words */
    _Atomic u64 pipe_wait_ns; /* the GPU thread's and the recording thread's waits, added atomically */
    _Atomic u64 pipe_make_ns; /* the workers' time making pipelines, added atomically */
    _Atomic u64 pipe_hits;    /* pipelines the pipeline cache had */
    /* Pipelines made in under PIPE_FAST_NS, the file's apart (pf.fast). A driver may leave the creation feedback's
       cache hit bit clear for every pipeline: NVIDIA's (driver d4-10de-00002684) made 963 pipelines of earlier runs in
       about 1.5 s, where one it compiles takes 80 to 130 ms, and set the bit for none. One made this fast came from a
       cache, the application's or the driver's own */
    _Atomic u64 pipe_fast;
    /* the draws' pipelines made as shader objects (vk_so_make), which no pipeline cache serves: their binaries of
       earlier runs do. One game's draws were all shader objects, and its stats line read "0 from the cache" */
    _Atomic u64 pipe_so;
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
        _Atomic u32 ship_new;   /* the pass's new shaders */
        _Atomic int scanning;   /* the pass runs */
        _Atomic int ship_ready; /* the pass is over, or won't run: the draws may read the index (release) */
        _Atomic u32 shaders_total, pipes_total; /* nsh and npipe once the file was read */
        _Atomic int cold; /* vk_pipeline_cache_cold's: -1 while it is read, then whether it held nothing */
        char apart[64];
        /* atomics, written by the GPU thread and the pipeline workers */
        _Atomic u64 made, hits; /* the file's pipelines made, and those the pipeline cache had */
        _Atomic u64 fast;       /* of them, those made in under PIPE_FAST_NS (pipe_make) */
        _Atomic u32 ship_used;  /* the shipped file's shaders a draw found */
        u32 ship_said;  /* ship_used as vk_pipeline_save last logged it (psave) */
    } pf;
    PMutex pj_lock;
    PCond pj_work, pj_done;
    int pj_started, pj_threads, pj_busy; /* pj_busy: the workers making a pipeline now */
    int held; /* a test's hold (vk_pipeline_hold): the workers take no job */
    struct {
        /* the table: the GPU thread adds the parts of a draw's pipelines, the file's reading thread those of the file's
           */
        PMutex lock;
        PipeLib* h[PLIBH];
        _Atomic VkPipelineLayout
            layouts[2][1u << 5]; /* the parts' own: [0] before rasterization, [1] fragment, by arrays */
        /* atomics: parts made by VK_PART_* bit, those the pipeline cache had and their ns; the links and the optimized
           pipelines made, their ns, and the optimized ones the cache had */
        _Atomic u64 made[4], hits[4], ns[4];
        _Atomic u64 links, link_ns, opts, opt_ns, opt_hits;
        /* the optimized links asked of the pipeline cache alone before a fast link (gpl_cached), their ns, and those
           it had */
        _Atomic u64 cached_tries, cached_ns, cached;
        /* the shipped programs' parts warmed into the pipeline cache ahead of their draws (ship_warm): queued, made,
           the cache hits, their ns */
        _Atomic u64 warm_queued, warm_made, warm_hit, warm_ns;
        /* the draws' pipelines' waits for a worker: how many, their ns, the ns of those queued while every worker was
           busy and one or more made an optimized pipeline, and the longest (pj_lock) */
        u64 job_waits, job_wait_ns, job_behind_opt_ns, job_wait_max_ns;
        int opt_busy_max; /* the most workers that made optimized pipelines at once (pj_lock) */
        u64 opt_asked;    /* the links whose optimized pipeline was asked for (pipe_want_opt; pj_lock) */
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
    u8 replay_word[4]; /* the word of them replay_bytes last gave */
    /* the constants of the program the pass translates now (ship_read), the reading thread's alone */
    const u8* ship_consts;
    size_t ship_nconsts;
} g_pipeline = {.playout_lock = PMUTEX_INIT,
                .psave = PMUTEX_INIT,
                .xlate_lock = PMUTEX_INIT,
                .modinfo_lock = PMUTEX_INIT,
                .pj_lock = PMUTEX_INIT,
                .plib = {.lock = PMUTEX_INIT}};
/* a pipeline worker's: the fold it translates, whose copies its reads answer from (fold_job_read), and the word of
   them fold_job_bytes last gave */
static _Thread_local struct {
    ShFold* fold;
    u8 word[4];
} t_pipeline;
static void pcache_note(void);
void vk_pipeline_bc7_cache(void) {
    if (!g_pipeline.bc7_cache) return;
    vk_astc_cache_open();
    g_pipeline.bc7_cache = 0;
}
#if defined(SNX_TRACY)
/* the stages' names, for the shader translate zone's text */
static const char* const vk_tracy_stage[] = {"vert", "tess ctrl", "tess eval", "geom", "frag", "comp", "task", "mesh"};
#endif

VkPipelineLayout g_vk_playout;
u64 g_vk_dslayout_hash = SNX_FNV_BASIS;
VkDescriptorSetLayout g_vk_dslayout[2][10];
u32 g_vk_stage_sets;
int g_vk_ftz; /* SH_FTZ_*: how translations flush FP32 subnormals (vk_pipeline_init) */
static void pf_start_load(void);
static void pf_merge(void);
static void pf_shader_record(const VkSh* s, const u32* spv, size_t nw, const u8* code, u32 ncode, const ShXfb* xfb);
static u32 src_bytes(u32 mlen);
static void pf_ship_found(VkSh* s);
static VkSh* pf_ship_take(u64 h, int stage, const u32 mask[SH_KEY_WORDS], u32 len, u64 qh, u32 len_a, u64 qh_a);
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
/* the bytes sh_cb_read reads, for the words a translation folds or holds folded (VkCbBytes): NULL as well where
   reading them would land what the GPU holds of them (a result kept there), which a draw that loads the words never
   does */
static const u8* sh_cb_bytes(u32 idx, u32 off, u32 size) {
    if (idx >= 18 || !g_pipeline.sh_ds) return NULL;
    DsCbBlock in = ds_cb_inline(g_pipeline.sh_ds, g_pipeline.sh_bind, idx);
    u64 a = g_pipeline.sh_ds->bind.cb_addr[g_pipeline.sh_bind][idx];
    u32 sz = g_pipeline.sh_ds->bind.cb_size[g_pipeline.sh_bind][idx];
    u32 limit = in.bytes ? in.size : sz ? sz : 65536;
    if ((!a && !in.bytes) || limit < size || off > limit - size) return NULL;
    return in.bytes ? in.bytes + off : gmem_cpu_bytes(a + off, size);
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
/* the translator's question of a load at a constant place (g_shader_cb_fold), for the translation running */
static int fold_read(u32 idx, u32 off, u32* out) {
    return vk_cb_fold_note(g_pipeline.rec, g_pipeline.fold, idx, off, out, &g_pipeline.rec_failed);
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
        if (!width || at + width > words || (spv[at] & 0xffffu) != SPV_OP_CAPABILITY) break;
        if (width == 2 && spv[at + 1] == SPV_CAPABILITY_FLOAT64 && !g_vk.feat.shaderFloat64)
            return "shaderFloat64 is unavailable";
        if (width == 2 && spv[at + 1] == SPV_CAPABILITY_INT64 && !g_vk.feat.shaderInt64)
            return "shaderInt64 is unavailable";
        if (width == 2 && spv[at + 1] == SPV_CAPABILITY_FLOAT16 && !g_spirv_translate.float16)
            return "shaderFloat16 is unavailable";
        if (width == 2 && spv[at + 1] == SPV_CAPABILITY_GEOMETRY && !g_vk.feat.geometryShader)
            return "geometryShader is unavailable";
        if (width == 2 && spv[at + 1] == SPV_CAPABILITY_TESSELLATION && !g_vk.feat.tessellationShader)
            return "tessellationShader is unavailable";
        if (width == 2 && spv[at + 1] == SPV_CAPABILITY_IMAGE_GATHER_EXTENDED && !g_vk.feat.shaderImageGatherExtended)
            return "shaderImageGatherExtended is unavailable";
        if (width == 2 && spv[at + 1] == SPV_CAPABILITY_STORAGE_IMAGE_MULTISAMPLE &&
            !g_vk.feat.shaderStorageImageMultisample)
            return "shaderStorageImageMultisample is unavailable";
        if (width == 2 && spv[at + 1] == SPV_CAPABILITY_MIN_LOD && !g_vk.feat.shaderResourceMinLod)
            return "shaderResourceMinLod is unavailable";
        if (width == 2 && spv[at + 1] == SPV_CAPABILITY_GEOMETRY_STREAMS && !g_vk.has_geometry_streams)
            return "geometryStreams is unavailable";
        if (width == 2 && spv[at + 1] == SPV_CAPABILITY_CLIP_DISTANCE && !g_vk.feat.shaderClipDistance)
            return "shaderClipDistance is unavailable";
        if (width == 2 && spv[at + 1] == SPV_CAPABILITY_SAMPLE_RATE_SHADING && !g_vk.feat.sampleRateShading)
            return "sampleRateShading is unavailable";
        if (width == 2 && spv[at + 1] == SPV_CAPABILITY_STORAGE_IMAGE_READ_WITHOUT_FORMAT &&
            !g_vk.feat.shaderStorageImageReadWithoutFormat)
            return "shaderStorageImageReadWithoutFormat is unavailable";
        if (width == 2 && spv[at + 1] == SPV_CAPABILITY_STORAGE_IMAGE_WRITE_WITHOUT_FORMAT &&
            !g_vk.feat.shaderStorageImageWriteWithoutFormat)
            return "shaderStorageImageWriteWithoutFormat is unavailable";
        if (width == 2 && spv[at + 1] == SPV_CAPABILITY_FRAGMENT_SHADER_PIXEL_INTERLOCK_EXT &&
            !g_vk.has_fragment_interlock)
            return "fragmentShaderPixelInterlock is unavailable";
        if (width == 2 && spv[at + 1] == SPV_CAPABILITY_DRAW_PARAMETERS && !g_vk.has_shader_draw_parameters)
            return "shaderDrawParameters is unavailable";
        if (width == 2 && spv[at + 1] == SPV_CAPABILITY_SHADER_CLOCK_KHR && !g_spirv_translate.clock_scopes)
            return "shaderClock is unavailable";
        if (width == 2 && spv[at + 1] == SPV_CAPABILITY_MESH_SHADING_EXT && !g_vk.has_mesh)
            return "meshShader is unavailable";
        if (width == 2 && spv[at + 1] == SPV_CAPABILITY_DOT_PRODUCT_FLOAT16_ACC_FLOAT32_VALVE &&
            !g_spirv_translate.mixed_dot)
            return "shaderMixedFloatDotProductFloat16AccFloat32 is unavailable";
        if (width == 2 &&
            (spv[at + 1] == SPV_CAPABILITY_COOPERATIVE_MATRIX_KHR ||
             spv[at + 1] == SPV_CAPABILITY_VULKAN_MEMORY_MODEL ||
             spv[at + 1] == SPV_CAPABILITY_VULKAN_MEMORY_MODEL_DEVICE_SCOPE) &&
            !g_spirv_translate.hmma_coop)
            return "cooperativeMatrix or the Vulkan memory model is unavailable";
        if (width == 2 && spv[at + 1] == SPV_CAPABILITY_ATOMIC_FLOAT32_ADD_EXT && !g_spirv_translate.float_atomic_add)
            return "shaderBufferFloat32AtomicAdd is unavailable";
        if (width == 2 && spv[at + 1] == SPV_CAPABILITY_SHADING_RATE_NV && g_vk.vrs != VK_VRS_NV)
            return "shadingRateImage is unavailable";
        if (width == 2 && spv[at + 1] == SPV_CAPABILITY_FRAGMENT_SHADING_RATE_KHR && g_vk.vrs != VK_VRS_KHR)
            return "attachmentFragmentShadingRate is unavailable";
        at += width;
    }
    return NULL;
}

/* An SM86 program in max_bytes at code (a graphics program from its header on, a compute one after the 80 zero bytes
   of the compute path): 1 and *p, or 0 when no program starts there */
static int sm86_program(const u8* code, u32 max_bytes, Sm86Program* p) {
    return sm86_find_programs(code, max_bytes, p, 1) == 1;
}
static u32* sm86_translate(const u8* code, u32 max_bytes, int stage, ShaderInfo* si, size_t* words, int lanes) {
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
    return sm86_to_spirv_ftz(code + p.code, p.size, stage, stage == ST_CS ? NULL : sph, si, words, g_vk_ftz, lanes);
}
const char* vk_lane_note(char out[32], const ShaderInfo* si) {
    static const char lanes[] = "subgroup ballot/shuffle unavailable";
    out[0] = 0;
    if (!strncmp(si->first_unsupported, lanes, sizeof lanes - 1))
        snprintf(out, 32, " (%u lanes of 32)", g_vk_subgroup_lanes);
    return out;
}
u32* vk_translate_spirv(const u8* code, u32 max_bytes, int stage, ShaderInfo* si, size_t* words, VkCbReader read,
                        VkCbReads* reads) {
    return vk_translate_spirv_fold(code, max_bytes, stage, si, words, read, reads, NULL);
}
u32* vk_translate_spirv_fold(const u8* code, u32 max_bytes, int stage, ShaderInfo* si, size_t* words, VkCbReader read,
                             VkCbReads* reads, const VkCbFold* fold) {
    pm_lock(&g_pipeline.xlate_lock);
    int (*old)(u32, u32, u32*) = g_shader_cb_read;
    g_pipeline.rec_read = read;
    g_pipeline.rec = reads;
    g_pipeline.rec_failed = 0;
    g_shader_cb_read = rec_read;
    /* fragment programs, of the translator that asks: a vertex program gains a fifth of it for the same cost */
    int folds = fold && fold->mode != VK_CB_FOLD_NONE && read && reads && stage == ST_FS && !g_shader_sm86;
    g_pipeline.fold = fold;
    g_shader_cb_fold = folds ? fold_read : NULL;
    const VkShaderStageFlagBits stages[] = {VK_SHADER_STAGE_VERTEX_BIT,
                                            VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                            VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                            VK_SHADER_STAGE_GEOMETRY_BIT,
                                            VK_SHADER_STAGE_FRAGMENT_BIT,
                                            VK_SHADER_STAGE_COMPUTE_BIT,
                                            VK_SHADER_STAGE_TASK_BIT_EXT,
                                            VK_SHADER_STAGE_MESH_BIT_EXT};
    VkShaderStageFlagBits shader_stage = stages[stage];
    /* a task or mesh program runs in pipelines that ask for subgroups of 32 (mesh_choose), or not at all */
    int lanes = stage == ST_TS || stage == ST_MS || (g_vk_subgroup_stages & shader_stage) ? SM86_LANES_WARP
                : g_vk_quad_stages & shader_stage                                        ? SM86_LANES_QUAD
                                                                                          : 0;
    u32* result = g_shader_sm86 ? sm86_translate(code, max_bytes, stage, si, words, lanes)
                                : shader_to_spirv(code, max_bytes, stage, si, words, g_vk_ftz, g_vk_preserve_infnan,
                                                  g_vk_preserve_infnan64, (g_vk_subgroup_stages & shader_stage) != 0);
    g_shader_cb_read = old;
    g_shader_cb_fold = NULL;
    g_pipeline.fold = NULL;
    g_pipeline.rec = NULL;
    if (folds) vk_cb_reads_sort(reads);
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
u32 g_vk_buffer_image_slots;

VkDescriptorType vk_cbuf_type(u32 slot) {
    return slot < g_spirv_translate.uniform_cbufs ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                  : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
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
        if (binding_type(i) == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
            (i < SNX_VKB_TEXARRAY || g_spirv_translate.texture_arrays))
            own++;
    if (set < fit) fit = set;
    fit = fit > own ? fit - own : 0;
    if (fit >= 18) return;
    /* a constant buffer's offset meets the uniform buffers' alignment: it must meet the storage buffers' too */
    if (l->minUniformBufferOffsetAlignment % l->minStorageBufferOffsetAlignment) return;
    g_spirv_translate.uniform_cbufs = fit;
    LOG("vulkan: this device's limits hold %u of the 18 constant buffers as uniform buffers, the others are storage "
        "buffers\n",
        fit);
}

/* the push constant bytes of a draw's layouts: the window offsets (cb_windows) and the texture words' offsets
   (tex_table) past the 128 bytes that every layout has */
u32 vk_push_bytes(void) {
    return g_spirv_translate.tex_table    ? SH_PUSH_BYTES_TABLE
           : g_spirv_translate.cb_windows ? SH_PUSH_BYTES_WINDOWS
                                          : 128;
}
/* the bindings of a stage's textures, which split sets (ShCaps.split_sets) put in a set of their own (shader_vk_tex_set) */
int vk_binding_is_texture(u32 i) {
    return (i >= SNX_VKB_TEX && i < SNX_VKB_TEX + SH_MAXTEX) || i == SNX_VKB_TEXARRAY || i == SNX_VKB_TEXFUNCS ||
           i == SNX_VKB_TEXDEPTH || i == SNX_VKB_TEXMAP;
}
/* descriptors of each type in one set: the binding ranges in shader.h */
static VkDescriptorType binding_type(u32 i) {
    if (i == SNX_VKB_SMAP || i == SNX_VKB_CAPTURE || i == SNX_VKB_MESHBUF || i == SNX_VKB_DRAW)
        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
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
u32 vk_pool_sizes(VkDescriptorPoolSize* sz, u32 sets, u32 array_sets) {
    static const VkDescriptorType ty[VK_POOL_TYPES - 1] = {
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,   VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE};
    int k;
    for (k = 0; k < VK_POOL_TYPES - 1; k++) {
        u32 n = ty[k] == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ? 2 : 0; /* auxiliary source/validity fetches */
        u32 arrays = 0, i;
        for (i = 0; i < SNX_VKB_END; i++) {
            if (binding_type(i) != ty[k]) continue;
            if (i < SNX_VKB_TEXARRAY)
                n++;
            else if (g_spirv_translate.texture_arrays)
                arrays += (i == SNX_VKB_TEXARRAY || i == SNX_VKB_TEXDEPTH) ? g_spirv_translate.tex_array : 1;
        }
        sz[k] = (VkDescriptorPoolSize){ty[k], n * sets + arrays * array_sets};
    }
    /* the dynamic constant buffers of the vertex or the fragment set, whichever has more (cb_dynamic.h): a set of any
       layout takes its uniform buffers from the count above, which counts those slots too. A pool has no size of a
       type with none (VUID-VkDescriptorPoolSize-descriptorCount-00302) */
    const u32 v = snx_popcount64(g_cb_dynamic.mask[0]), f = snx_popcount64(g_cb_dynamic.mask[1]);
    sz[k] = (VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, (v > f ? v : f) * sets};
    return sz[k].descriptorCount ? VK_POOL_TYPES : VK_POOL_TYPES - 1;
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
    /* set 2 is a task program's, which the compute-task path dispatches as a compute program (sm86.h) */
    return vis[s] | (s == shader_vk_set(ST_MS) || s == shader_vk_set(ST_TS) ? g_vk.mesh_stages : 0) |
           (s == shader_vk_set(ST_TS) ? VK_SHADER_STAGE_COMPUTE_BIT : 0);
}
u32 vk_texture_array_elements(void) { return g_spirv_translate.texture_arrays ? g_spirv_translate.tex_array : 0; }
int vk_dslayout_arrays(VkDescriptorSetLayout layout) {
    for (u32 s = 0; s < 10; s++)
        if (layout && layout == g_vk_dslayout[1][s]) return g_spirv_translate.texture_arrays;
    return 0;
}
int vk_graphics_playout_is(VkPipelineLayout layout) {
    for (u32 j = 0; j < 2; j++)
        for (u32 i = 0; i < sizeof g_pipeline.playouts[j] / sizeof g_pipeline.playouts[j][0]; i++)
            if (layout && atomic_load_explicit(&g_pipeline.playouts[j][i], memory_order_acquire) == layout) return 1;
    return 0;
}
static struct Pipe* standin_pipe(VkPipeline h);
/* the layout of a draw pipeline's sets (vk_playout), or with independent 0 one without
   VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT, which the sets of a draw with shader objects are bound with:
   sets bound through a layout of independent sets are not compatible with a shader object's set layouts */
static VkPipelineLayout playout(u32 arrays, int independent) {
    _Atomic VkPipelineLayout* l = &g_pipeline.playouts[independent ? 0 : 1][arrays & ((1u << 5) - 1)];
    VkPipelineLayout made = atomic_load_explicit(l, memory_order_acquire);
    if (made) return made;
    pm_lock(&g_pipeline.playout_lock);
    if (!atomic_load_explicit(&(*l), memory_order_relaxed)) {
        VkDescriptorSetLayout sets[11];
        u32 nsets = g_spirv_translate.split_sets ? 10 : g_vk_stage_sets;
        for (u32 s = 0; s < nsets; s++) sets[s] = g_vk_dslayout[(arrays >> (s % 5)) & 1][s];
        if (g_spirv_translate.tex_table) sets[nsets++] = vk_table_layout(); /* at table_set, after the stages' */
        /* push constants: viewport/depth pc[0..3], fragment alpha test pc[4..6], sprite T flip pc[7], scaled textures
           pc[8..9], and of those the ones of the draw's size pc[24..25] */
        VkPushConstantRange pc = {VK_PUSH_STAGES, 0, vk_push_bytes()};
        /* with pipeline libraries, a link of parts whose layouts have only their own sets (part_layout) */
        VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                         .flags = independent ? VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT : 0,
                                         .setLayoutCount = nsets,
                                         .pSetLayouts = sets,
                                         .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &pc};
        if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &made))) made = VK_NULL_HANDLE;
        atomic_store_explicit(l, made, memory_order_release);
    }
    made = atomic_load_explicit(&(*l), memory_order_relaxed);
    pm_unlock(&g_pipeline.playout_lock);
    return made;
}
VkPipelineLayout vk_playout(u32 arrays) { return playout(arrays, g_vk.has_gpl); }
/* the layout of a compute dispatch's sets: the same, without the independent sets flag, which only a graphics pipeline
   library's parts may use (VUID-VkComputePipelineCreateInfo-layout-12396) */
VkPipelineLayout vk_playout_compute(u32 arrays) { return playout(arrays, 0); }
VkPipelineLayout vk_pipeline_layout(VkPipeline h, u32 arrays) {
    Pipe* p = standin_pipe(h);
    return p ? p->layout : vk_playout(arrays);
}

static void plib_init(void);
void vk_pipeline_init(void) {
    pm_lock_announce(&g_pipeline.playout_lock, "pipeline layout");
    pm_lock_announce(&g_pipeline.pj_lock, "pipeline jobs"); /* game threads queueing work, workers taking it */
    pm_lock_announce(&g_pipeline.modinfo_lock, "shader modules"); /* the module table's readers and builders */
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
        g_spirv_translate.tex_array = snx_vk_texture_array_say(line, sizeof line, g_spirv_translate.texture_arrays, &tl,
                                                               SH_MAXTEX, g_spirv_translate.tex_array);
        g_spirv_translate.texture_arrays = g_spirv_translate.tex_array != 0;
        if (line[0]) LOG("%s\n", line);
    }
    cbufs_fit_limits();
    snprintf(g_pipeline.cache_dir, sizeof g_pipeline.cache_dir, "%sshader_cache", g_cachedir);
    plat_mkdir(g_pipeline.cache_dir);
    /* which constant buffers the vertex and fragment sets bind as dynamic uniform buffers: from the moves the game's
       last run kept there, before the set layouts that give them that type */
    vk_cb_dynamic_choose(g_pipeline.cache_dir);
    /* a stage's textures in a set of their own (shader_vk_tex_set): with plain sets, where all five stage sets are and
       the device binds ten. Descriptor buffers and heaps keep one set a stage */
    g_spirv_translate.split_sets = g_vk_stage_sets == 5 && g_vk.props.limits.maxBoundDescriptorSets >= 10 &&
                                   !g_vk.has_descriptor_buffers && !g_vk.has_descriptor_heap_support;
    LOG("vulkan: %s\n", g_spirv_translate.split_sets ? "each stage's textures are in a descriptor set of their own"
                                                       : "one descriptor set a stage, its textures with its buffers");
    /* the vertex and fragment programs' constant buffers through windows of the staging ring, their offsets in push
       constants past the 128 bytes every layout had (cb_windows.h) */
    {
        char ban[1200] = "", line[2048];
        if (g_vk.cb_windows_ban) vk_ban_why(g_vk.cb_windows_ban, ban, sizeof ban);
        const SnxVkCbWindowDevice wd = {g_vk.props.limits.maxPushConstantsSize, g_vk.props.limits.maxUniformBufferRange,
                                        g_spirv_translate.uniform_cbufs, g_vk.has_descriptor_heap_support,
                                        ban[0] ? ban : NULL};
        g_spirv_translate.cb_windows =
            snx_vk_cb_windows_say(line, sizeof line, &wd, SH_PUSH_BYTES_WINDOWS, g_settings.constant_buffer_windows);
        LOG("%s\n", line);
    }
    /* SM86 programs' plain textures through the texture table (table.c), a set after the stages' sets: where the
       constant buffers are in windows (the words take one), the sets are plain ones, and the device pushes the
       words' offsets and binds one more set */
    g_spirv_translate.tex_table = 0;
    if (g_shader_sm86) {
        const u32 table_set = g_spirv_translate.split_sets ? 10 : g_vk_stage_sets;
        char why[160] = "";
        const char* no = !g_spirv_translate.cb_windows ? "constant buffers are not bound as windows"
                         : g_vk.has_descriptor_buffers || g_vk.has_descriptor_heap_support
                             ? "descriptor buffers or heaps hold the sets"
                         : g_vk.props.limits.maxPushConstantsSize < SH_PUSH_BYTES_TABLE
                             ? "the device pushes fewer than 240 bytes"
                         : g_vk.props.limits.maxBoundDescriptorSets <= table_set ? "the device binds fewer sets"
                                                                                 : NULL;
        if (!no && vk_table_init(why, sizeof why)) {
            g_spirv_translate.tex_table = 1;
            g_spirv_translate.table_set = table_set;
        }
        LOG("vulkan: SM86 textures %s%s\n",
            g_spirv_translate.tex_table ? "through the texture table, one word a texture"
                                        : "in descriptor sets of their own: no texture table, as ",
            g_spirv_translate.tex_table ? ""
            : no                        ? no
                                        : why);
    }
    /* SM86 programs' global memory by device address, in the texture table's records: where the device gives
       addresses (has_gbuf_address) and the buffers come from the GPU mirror or the ring, not memory shared with the
       CPU */
    g_spirv_translate.gbuf_address = 0;
    if (g_shader_sm86) {
        const char* no = !g_spirv_translate.tex_table ? "no texture table"
                         : !g_vk.has_gbuf_address     ? "the device has no buffer device address or 64-bit integers"
                         : vk_uma()                   ? "guest memory is shared with the CPU"
                         : !vk_table_scratch_init()   ? "no scratch memory"
                                                      : NULL;
        g_spirv_translate.gbuf_address = !no;
        LOG("vulkan: SM86 global memory %s%s\n", no ? "through storage buffer descriptors, as " : "by device address",
            no ? no : "");
    }
    /* SM86 vertex programs fetch their own attributes by device address, from the words of their records: with global
       memory by address (the record, the scratch word and buffers with addresses), and with draw parameters, which
       give the first instance that an attribute stepped per instance counts from */
    g_spirv_translate.vertex_fetch = 0;
    if (g_shader_sm86) {
        const char* no = !g_spirv_translate.gbuf_address      ? "global memory is not read by device address"
                         : !g_spirv_translate.draw_parameters ? "the device has no draw parameters"
                                                              : NULL;
        g_spirv_translate.vertex_fetch = !no;
        LOG("vulkan: SM86 vertex attributes %s%s\n", no ? "through vertex input, as " : "fetched by the vertex program",
            no ? no : "");
    }
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
            if (i >= SNX_VKB_TEXARRAY && !g_spirv_translate.texture_arrays) continue;
            if (i >= SNX_VKB_IMGBUF && i < SNX_VKB_CAPTURE && i - SNX_VKB_IMGBUF >= buffer_slots) continue;
            b[count++] = (VkDescriptorSetLayoutBinding){
                .binding = i,
                /* the vertex and fragment sets' dynamic constant buffers (cb_dynamic.h) */
                .descriptorType = s < 2 && i < SNX_VKB_CB + VK_CB_DYNAMIC_SLOTS ? vk_cb_dynamic_type(s, i - SNX_VKB_CB)
                                                                                : binding_type(i),
                .descriptorCount = (i == SNX_VKB_TEXARRAY || i == SNX_VKB_TEXDEPTH) ? g_spirv_translate.tex_array : 1,
                .stageFlags = vk_set_stages(s)};
        }
        /* split: the buffers' set s, the same with the arrays or without, and the textures' set s + 5 */
        VkDescriptorSetLayoutBinding tex[SNX_VKB_END];
        u32 ntex = 0;
        if (g_spirv_translate.split_sets) {
            u32 kept = 0;
            for (u32 k = 0; k < count; k++)
                if (vk_binding_is_texture(b[k].binding))
                    tex[ntex++] = b[k];
                else
                    b[kept++] = b[k];
            count = kept;
        }
        for (int arrays = 0; arrays < 2; arrays++) {
            VkDescriptorSetLayoutBinding* list = g_spirv_translate.split_sets ? tex : b;
            u32 n = g_spirv_translate.split_sets ? ntex : count;
            while (!arrays && n && list[n - 1].binding >= SNX_VKB_TEXARRAY) n--; /* the arrays are the last bindings */
            for (int k = 0; k < (g_spirv_translate.split_sets ? 2 : 1); k++) {
                /* k 1: the buffers' set of a split stage, with no arrays either way */
                VkDescriptorSetLayoutBinding* l = k ? b : list;
                u32 ln = k ? count : n;
                int set = g_spirv_translate.split_sets && !k ? s + 5 : s;
                VkDescriptorSetLayoutCreateInfo dl = {
                    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                    .flags = g_vk.has_descriptor_buffers ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT : 0,
                    .bindingCount = ln,
                    .pBindings = l};
                if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &dl, NULL, &g_vk_dslayout[arrays][set])))
                    fatal("vulkan: cannot create shader descriptor layout");
                /* what the layout is made of, for the key of a shader object's binary (shader_objects.c): a layout
                   another build or setting makes is another program to the driver, with the same SPIR-V */
                u64 h =
                    snx_fnv_step(snx_fnv_step(snx_fnv_step(g_vk_dslayout_hash, (u64)set), (u64)arrays), dl.flags);
                for (u32 i = 0; i < ln; i++) {
                    h = snx_fnv_step(h, l[i].binding);
                    h = snx_fnv_step(h, (u64)l[i].descriptorType);
                    h = snx_fnv_step(h, l[i].descriptorCount);
                    h = snx_fnv_step(h, l[i].stageFlags);
                }
                g_vk_dslayout_hash = h;
                vk_descriptors_layout(g_vk_dslayout[arrays][set], l, ln);
            }
        }
    }
    /* a layout with the table is another program to the driver (shader_objects.c) */
    if (g_spirv_translate.tex_table)
        g_vk_dslayout_hash = snx_fnv_step(snx_fnv_step(g_vk_dslayout_hash, 0x7AB1Eu), g_spirv_translate.table_set);
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
    /* the BC7 blocks of earlier ASTC encodes, where ASTC textures are held as BC7 (astc.c): started now, or by
       astc_mode's Auto once it holds them as BC7 (vk_astc_auto_present) */
    g_pipeline.bc7_cache = 1;
    if (vk_astc_bc7()) vk_pipeline_bc7_cache();
    { /* the files earlier versions kept that cache in: said once a run, and left where they are */
        char old[1400];
        snprintf(old, sizeof old, "%s/%s.vkbc7", g_pipeline.cache_dir, g_exename);
        snx_path_native(old);
        vk_bc7_cache_old_file(old);
        snprintf(old, sizeof old, "%s/%s.vkbc7.idx", g_pipeline.cache_dir, g_exename);
        snx_path_native(old);
        vk_bc7_cache_old_file(old);
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
    if (snx_tracy_attached()) {
        char msg[96];
        snprintf(msg, sizeof msg, "pipeline cache: %zu bytes read%s", loaded, from ? "" : " (no file)");
        SNX_MSG(msg, strlen(msg));
    }
    SNX_PLOT("vk pipe cache read bytes", (double)loaded);
    vk_so_init();
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
    /* the way of HMMA, which the pipeline file's key holds, settled before the reading thread translates any */
    vk_hmma_pick(g_pipeline.cache_dir, g_pipeline.driver_tag);
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
static void sh_key_info(ShaderInfo* si, int stage, const u32 mask[SH_KEY_WORDS], const ShXfb* xfb) {
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
        si->predraw_slots = mask[26] | (u64)mask[27] << 32;
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
        si->mesh.payload_buf = (u16)((mask[2] >> 16) & 1); /* the compute-task path (sm86.h) */
        si->mesh.payload_stride = (u16)(mask[4] & 0x7fff);
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
            si->rt_sint_mask = mask[5]; /* the generics the stage before writes */
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
static void sh_mesh_sph(ShaderInfo* si, int stage, const u8* code, const u32 mask[SH_KEY_WORDS]) {
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
static u32* sh_spirv(VkSh* s, const u8* code, u64 h, int stage, const u32 mask[SH_KEY_WORDS], u32 len, u64 qh,
                     u32 len_a, u64 qh_a, const ShXfb* xfb, VkCbReader read, const VkCbFold* fold, int shipped,
                     size_t* nw) {
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
    u32* spv = vk_translate_spirv_fold(code, SH_CODE, stage, &s->si, nw, read, &s->cbr,
                                       atomic_load_explicit(&g_pipeline.fold_off, memory_order_relaxed) ? NULL : fold);
    s->nfold = vk_cb_fold_count(&s->cbr);
    SNX_ZONE_VALUE(z_translate, *nw);
    SNX_ZONE_TEXT(z_translate, "%s %08x, %u spv words, %s",
                  stage >= 0 && stage < (int)(sizeof vk_tracy_stage / sizeof *vk_tracy_stage) ? vk_tracy_stage[stage]
                                                                                              : "stage?",
                  (unsigned)h, (unsigned)*nw, spv ? "made" : "rejected");
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
/* a task program of the compute-task path keeps its SPIR-V, which its compute pipeline is made from (render.c) */
static void sh_keep_compute(u32** keep, u32* n, const u32* spv, u32 nw) {
    *keep = malloc(nw * sizeof **keep);
    if (!*keep) return;
    memcpy(*keep, spv, nw * sizeof **keep);
    *n = nw;
}
static VkSh* sh_make(const u8* code, u32 mlen, u64 h, int stage, const u32 mask[SH_KEY_WORDS], u32 len, u64 qh,
                     u32 len_a, u64 qh_a, const ShXfb* xfb, VkCbReader read, const VkCbFold* fold) {
    size_t nw;
    VkSh* s = calloc(1, sizeof *s);
    if (!s) return NULL;
    SNX_ZONE(z, "shader translate");
    u32* spv = sh_spirv(s, code, h, stage, mask, len, qh, len_a, qh_a, xfb, read, fold, 0, &nw);
    SNX_ZONE_END(z);
    if (spv) {
        VkShaderModuleCreateInfo mi = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = spv};
        if (!VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &s->mod))) {
            s->failed = 1;
            snprintf(s->si.first_unsupported, sizeof s->si.first_unsupported, "vkCreateShaderModule failed");
        } else {
            SNX_MEM_ALLOC_D((const void*)(uintptr_t)s->mod, 0, "shader modules");
            modinfo_put(s->mod, h, stage, nw);
            vk_so_keep_spirv(s->mod, spv, nw);
            pf_shader_record(s, spv, nw, code, src_bytes(mlen), xfb);
            if (stage == ST_TS && s->si.mesh.payload_buf) sh_keep_compute(&s->spv, &s->nspv, spv, (u32)nw);
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
    /* a program whose branches and loops aren't SPIR-V's own runs them in the translator's dispatch loop, one switch
       inside one loop, which costs the driver and the GPU about as much as the program's code: why, once a program */
    if (!s->failed && s->si.flow_why[0])
        LOG_LIMIT(64, "vulkan: program %016llx runs its control flow in a dispatch loop: %s\n", (unsigned long long)h,
                  s->si.flow_why);
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
/* A program's translations (cb_fold.h): the tables hold one for each set of constant buffer words it was made to
   hold, and a draw takes the one that holds most of those whose words its buffers still have */
static int sh_same_program(const VkSh* q, u64 h, int stage, const u32 mask[SH_KEY_WORDS], u32 len, u64 qh, u32 len_a,
                           u64 qh_a) {
    return q->hash == h && q->stage == stage && !memcmp(q->mask, mask, sizeof q->mask) && q->clen == len &&
           q->qh == qh && q->clen_a == len_a && q->qh_a == qh_a;
}
static VkSh* sh_holder(u64 h, int stage, const u32 mask[SH_KEY_WORDS], u32 len, u64 qh, u32 len_a, u64 qh_a) {
    VkSh* best = NULL;
    for (VkSh* q = g_pipeline.shh[h & 1023]; q; q = q->next)
        if (sh_same_program(q, h, stage, mask, len, qh, len_a, qh_a) && (!best || q->nfold > best->nfold) &&
            vk_cb_folds_hold(&q->cbr, sh_cb_read, sh_cb_bytes))
            best = q;
    return best;
}
static int sh_fold_top(const VkSh* s) {
    for (const VkSh* q = g_pipeline.shh[s->hash & 1023]; q; q = q->next)
        if (q->nfold > s->nfold && sh_same_program(q, s->hash, s->stage, s->mask, s->clen, s->qh, s->clen_a, s->qh_a))
            return 0;
    return 1;
}
/* how a translation of the program made now folds: by the count of those a draw's words made (the shipped pass's is
   not one: it knew the program's own constants alone), whose reads go in others for the one that folds what they
   agree on */
static void sh_fold_plan(u64 h, int stage, const u32 mask[SH_KEY_WORDS], u32 len, u64 qh, u32 len_a, u64 qh_a,
                         VkCbFold* fold, const VkCbReads* others[SH_CB_FOLD_VARIANTS]) {
    u32 made = 0;
    for (const VkSh* q = g_pipeline.shh[h & 1023]; q; q = q->next)
        if (sh_same_program(q, h, stage, mask, len, qh, len_a, qh_a) && q->nfold && !q->shipped) {
            if (made < SH_CB_FOLD_VARIANTS) others[made] = &q->cbr;
            made++;
        }
    *fold = (VkCbFold){.mode = vk_cb_fold_mode(made),
                       .bytes = sh_cb_bytes,
                       .others = others,
                       .nothers = made < SH_CB_FOLD_VARIANTS ? made : SH_CB_FOLD_VARIANTS};
}
/* s's draws are asked again whether a translation that folds more of their words is to be made (vk_shader_fold_draw):
   another translation of the program stopped holding, its words changed */
static void sh_fold_ask(VkSh* s) {
    if (s->fold_job) return; /* one is being made from them */
    s->fold_asked = 0;
    s->fold_stable = (VkCbStable){0};
}

VkSh* vk_shader_get(const DsState* ds, u64 gva, u64 gva_a, int stage, u64 int_mask, u64 sint_mask, u32 rt_int,
                    u32 rt_sint, u32 clip, u64 dc_color, u32 dc_funcs, u32 unfed, u64 rect, const ShXfb* xfb,
                    const u8* query_hint, u64 advanced_blend, u32 advanced_mask, u32 img_int, u32 img_sint) {
    return vk_shader_get_ex(ds, gva, gva_a, stage, int_mask, sint_mask, rt_int, rt_sint, clip, dc_color, dc_funcs,
                            unfed, rect, xfb, query_hint, advanced_blend, advanced_mask, img_int, img_sint, 0, 0, 0, 0);
}
VkSh* vk_shader_get_ex(const DsState* ds, u64 gva, u64 gva_a, int stage, u64 int_mask, u64 sint_mask, u32 rt_int,
                       u32 rt_sint, u32 clip, u64 dc_color, u32 dc_funcs, u32 unfed, u64 rect, const ShXfb* xfb,
                       const u8* query_hint, u64 advanced_blend, u32 advanced_mask, u32 img_int, u32 img_sint,
                       u32 fs_config, u32 minmax, u64 predraw, int early) {
    /* a vertex program's is its position's copy (ShaderInfo.position_copy) */
    if ((fs_config & ~SH_FS_CONFIG_MASK) || (fs_config && stage != ST_FS && (stage != ST_VS || fs_config > 32)))
        return NULL;
    if ((minmax >> 2 * SH_MINMAX_SLOTS) || (minmax && stage != ST_FS)) return NULL;
    if ((predraw || early) && stage != ST_FS) return NULL;
    const DsPrograms* p = ds ? &ds->prog : NULL;
    pf_merge();
    g_pipeline.sh_ds = ds;
    g_pipeline.sh_bind = sh_bind_stage(stage);
    u32 mask[SH_KEY_WORDS] = {(u32)int_mask,
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
    if (stage == ST_FS) mask[11] |= minmax << 1 | (early ? 1u : 0u);
    if (stage == ST_FS) {
        mask[21] = (u32)advanced_blend;
        mask[22] = (u32)(advanced_blend >> 32);
        mask[23] = advanced_mask;
        mask[26] = (u32)predraw;
        mask[27] = (u32)(predraw >> 32);
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
    mix ^= (u32)vk_hash(mask + 21, (SH_KEY_WORDS - 21) * sizeof(u32));
    if (stage == ST_GS) mix ^= (u32)vk_hash(mask + 12, 8 * sizeof(u32));
    if (stage != ST_GS && rect) mix ^= (u32)vk_hash(mask + 12, 2 * sizeof(u32));
    if (queried) mix ^= (u32)vk_hash(mask + 14, SH_QUERY_WORDS * sizeof(u32));
    u64 hk = (gva * 0x9E3779B97F4A7C15ull) ^ (gva_a * 0xC2B2AE3D27D4EB4Full) ^ ((u64)mix << 24) ^ (u64)stage;
    ShA* set = &g_pipeline.sha[vk_sha_set(hk) * VK_SHA_WAYS];
    ShA* ca = NULL;
    u32 epoch = vk_ww_epoch(); /* before the code is checked or read */
    for (int w = 0; w < VK_SHA_WAYS && !ca; w++)
        if (set[w].s && set[w].gva == gva && set[w].gva_a == gva_a && set[w].stage == stage &&
            !memcmp(set[w].mask, mask, 20 * sizeof *mask) && !memcmp(set[w].mask + 21, mask + 21, (SH_KEY_WORDS - 21) * sizeof *mask))
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
        if (same) {
            /* the first time a translation's words stop holding, the program's others are asked again */
            int again = 0, held = vk_cb_folds_hold(&s->cbr, sh_cb_read, sh_cb_bytes);
            if (ca->fold_gen != g_pipeline.fold_gen) {
                ca->fold_top = sh_fold_top(s);
                ca->fold_gen = g_pipeline.fold_gen;
            }
            if (!held) {
                again = !s->fold_broke;
                s->fold_broke = 1;
            }
            if (!held || !ca->fold_top) {
                s = sh_holder(s->hash, s->stage, s->mask, s->clen, s->qh, s->clen_a, s->qh_a);
                if (s) ca->fold_top = sh_fold_top(s);
            }
            /* a translation that folds more of the draws' words is made beside them, never here: the draw takes the
               one that holds, and the draws count their words again when the program's changed */
            if (s) {
                if (again) sh_fold_ask(s);
                ca->s = s;
                return s;
            }
            /* none holds: one is made from the program's code, below */
        }
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
    /* one made for the same program: its whole code hashes the same (the merged program's, and each part's with its
       length, which ShA's checks compare memory with later), with the same key and constant buffer reads. Shaders of
       earlier runs (pf_merge) are found only so */
    VkSh* s = sh_holder(h, stage, mask, len, qh, len_a, qh_a);
    if (s && s->shipped == 1) pf_ship_found(s);
    if (!s && (s = pf_ship_take(h, stage, mask, len, qh, len_a, qh_a)) != NULL) {
        s->next = g_pipeline.shh[h & 1023];
        g_pipeline.shh[h & 1023] = s;
        g_pipeline.fold_gen++;
    }
    if (!s) {
        if (vk_corpus_dir()) { /* the key's words as sh_make takes them into si, which the corpus keeps */
            ShaderInfo si;
            sh_key_info(&si, stage, mask, xfb);
            sh_mesh_sph(&si, stage, code, mask);
            shader_corpus_save(vk_corpus_dir(), code, mlen, SH_CODE, stage, &si, h ^ mix);
        }
        u64 t0 = plat_ns();
        /* folding none of the draw's words: those its draws keep finding are folded beside them, later
           (vk_shader_fold_draw) */
        const VkCbFold loads = {.mode = VK_CB_FOLD_LOADS};
        s = sh_make(code, mlen, h, stage, mask, len, qh, len_a, qh_a, xfb, sh_cb_read, &loads);
        if (!s) {
            free(code);
            return NULL;
        }
        if (s->failed)
            WARN("vulkan: shader %016llx (stage %d) not translated: %s%s\n", (unsigned long long)h, stage,
                 s->si.first_unsupported[0] ? s->si.first_unsupported : "unsupported feature or float controls",
                 vk_lane_note((char[32]){0}, &s->si));
        g_pipeline.sh_new++;
        g_pipeline.sh_ns += plat_ns() - t0;
        s->next = g_pipeline.shh[h & 1023];
        g_pipeline.shh[h & 1023] = s;
        g_pipeline.fold_gen++;
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
    ca->fold_gen = g_pipeline.fold_gen;
    ca->fold_top = sh_fold_top(s);
    return s;
}

VkSh* vk_shader_mesh(const DsState* ds, u64 gva, int stage, const DsMeshKey* k, u32 payload_stride, int task_compute) {
    int mesh = stage == ST_MS;
    u32 w0 = mesh ? k->mesh_threads | (u32)k->mesh_payload << 16 : k->task_threads | (u32)k->task_payload << 16;
    u32 w1 = mesh ? k->max_vertices | (u32)k->max_primitives << 16 : 0;
    u32 w2 = mesh ? k->has_task | (u32)k->primitive_vertices << 8 | (u32)(task_compute && k->has_task) << 16
                  : (u32)(task_compute ? 1 : 0) << 16;
    u32 w3 = mesh ? k->prim_sph : 0;
    /* the compute-task path's slot, and the path itself (ShMesh.payload_stride, pipeline.c's sh_key_info) */
    u32 w4 = task_compute ? (payload_stride & 0x7fffu) : 0;
    return vk_shader_get(ds, gva, 0, stage, w0 | (u64)w1 << 32, w2 | (u64)w3 << 32, w4, 0, 0, 0, 0, 0, 0, NULL, NULL,
                         0, 0, 0, 0);
}
u32 vk_shader_fold_gen(void) { return g_pipeline.fold_gen; }
int vk_shader_fold_top(const VkSh* s) { return sh_fold_top(s); }
int vk_shader_chosen(const DsState* ds, const VkSh* s) {
    g_pipeline.sh_ds = ds;
    g_pipeline.sh_bind = sh_bind_stage(s->stage);
    return sh_holder(s->hash, s->stage, s->mask, s->clen, s->qh, s->clen_a, s->qh_a) == s;
}
/* whether the constant buffer words s's translation read (BRX jump tables) still hold in ds's bound buffers, as
   vk_shader_get checks them on each hit */
int vk_shader_cb_hold(const DsState* ds, const VkSh* s) {
    g_pipeline.sh_ds = ds;
    g_pipeline.sh_bind = sh_bind_stage(s->stage);
    return vk_cb_folds_hold(&s->cbr, sh_cb_read, sh_cb_bytes);
}

VkShaderModule vk_module_spv(const u32* spv, size_t nw) {
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &m)))
        SNX_MEM_ALLOC_D((const void*)(uintptr_t)m, 0, "shader modules");
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
        if (op == SPV_OP_DECORATE && count == 4 && spv[i + 2] == SPV_DECORATION_LOCATION) {
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
    if (!g_pipeline.null_fs) {
        g_pipeline.null_fs = vk_module_spv(snx_null_fs_spv, snx_null_fs_spv_size / sizeof *snx_null_fs_spv);
        if (g_pipeline.null_fs) /* for the shader objects of draws without a fragment program */
            vk_so_keep_spirv(g_pipeline.null_fs, snx_null_fs_spv, snx_null_fs_spv_size / sizeof *snx_null_fs_spv);
    }
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
    return vk_conservative_dilation(conservative, g_vk.conservative_extra, NULL, 0);
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
    VkPipelineRasterizationDepthClipStateCreateInfoEXT depth_clip; /* with k->depth_clip */
    VkPipelineCreationFeedback made;
    VkPipelineCreationFeedbackCreateInfo fi;
    PipeDynIn dyn;
    VkPipelineCreateFlags flags; /* the descriptor buffers', the feedback loop's and the shading rate's */
    VkVrsPipe vrs;               /* an Ounce's shading rate image (shading_rate.c) */
} PipeInfo;
/* the color targets' blend states of k (the first k->ncolor of cba), as a pipeline and the shader objects' bind
   (vk_so_bind) give them: the device's lacks of dual-source and independent blending taken in, and an advanced
   equation in place of each operation */
void vk_pipe_blend(const PipeKey* k, VkPipelineColorBlendAttachmentState cba[8]) {
    int changed = 0;
    for (u32 i = 0; i < k->ncolor; i++) {
        VkPipelineColorBlendAttachmentState* a = &cba[i];
        *a = (VkPipelineColorBlendAttachmentState){.blendEnable = k->blend[i].en,
                                                   .srcColorBlendFactor = (VkBlendFactor)k->blend[i].sc,
                                                   .dstColorBlendFactor = (VkBlendFactor)k->blend[i].dc,
                                                   .colorBlendOp = (VkBlendOp)k->blend[i].cop,
                                                   .srcAlphaBlendFactor = (VkBlendFactor)k->blend[i].sa,
                                                   .dstAlphaBlendFactor = (VkBlendFactor)k->blend[i].da,
                                                   .alphaBlendOp = (VkBlendOp)k->blend[i].aop,
                                                   /* no fragment shader: its outputs would be undefined; a
                                                      dynamic blend's mark is no component */
                                                   .colorWriteMask = k->fs ? k->blend[i].mask & 15u : 0};
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
            changed |= memcmp(&cba[i], &cba[0], sizeof cba[0]) != 0;
            cba[i] = cba[0];
        }
    if (changed) {
        LOG_LIMIT(
            1, "vulkan: blending that needs dual-source or independent blend, which this GPU lacks, is simplified\n");
    }
    if (k->advanced.operation) {
        /* NVN advanced modes select color output zero only. Other bound attachments must have no writes. */
        for (u32 i = 0; i < k->ncolor; i++)
            cba[i].colorBlendOp = cba[i].alphaBlendOp = (VkBlendOp)k->advanced.operation;
        if (k->ncolor) cba[0].blendEnable = VK_TRUE;
    }
}
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
                                                             o->binding_maps[o->nst], SNX_VKB_END, k->cb_addr[set]),
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
    o->depth_clip = (VkPipelineRasterizationDepthClipStateCreateInfoEXT){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT,
        .pNext = k->provoking_last ? (void*)&o->provoking
                 : k->conservative ? (void*)&o->conservative
                                   : NULL,
        .depthClipEnable = k->depth_clip == 2};
    o->rs = (VkPipelineRasterizationStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                     .pNext = k->depth_clip       ? (void*)&o->depth_clip
                                                              : k->provoking_last ? (void*)&o->provoking
                                                              : k->conservative   ? (void*)&o->conservative
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
    vk_pipe_blend(k, o->cba);
    o->advanced = (VkPipelineColorBlendAdvancedStateCreateInfoEXT){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_ADVANCED_STATE_CREATE_INFO_EXT,
        .srcPremultiplied = k->advanced.src_premultiplied,
        .dstPremultiplied = VK_TRUE,
        .blendOverlap = (VkBlendOverlapEXT)k->advanced.overlap};
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
    /* Strict vertex bounds for each draw pipeline. Per-pipeline selection needs robustBufferAccess2 to be
       supported, not enabled: on a device without robustBufferAccess (which the device-wide feature needs, device.c),
       vertex inputs still get it. */
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
                         .vertex_input = g_vk.vertex_input_dynamic && !k->fetch,
                         .tess = k->tes != VK_NULL_HANDLE,
                         .mesh = k->mesh != 0,
                         .clamp_range = g_vk.has_depth_clamp_range,
                         .rate_palette = g_vk.vrs == VK_VRS_NV,
                         .blend = vk_pipe_blend_dynamic(k)};
    o->flags = (g_vk.has_descriptor_buffers ? VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT : 0) |
               (k->feedback ? VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT : 0) | vk_vrs_pipeline_flags() |
               vk_pipeline_stats_flags();
    /* each draw pipeline reads the shading rate image, which shades each pixel where none is bound */
    vk_vrs_pipeline(&o->vrs, &o->vp, &o->fi);
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
/* a pipeline made alone where the driver needs it (snx_vk_compile_serial) */
#define COMPILED(call)                                                                                                 \
    do {                                                                                                               \
        int entered_ = snx_vk_compile_enter();                                                                         \
        call;                                                                                                          \
        snx_vk_compile_leave(entered_);                                                                                \
    } while (0)
static void pipe_note(const char* what, u64 h, const VkShaderModule modules[5], const PipeKey* k);
/* gi, a pipeline, a part or a link, made of descriptor heaps (PipeKey.heaps). The driver reads the flags of a chained
   VkPipelineCreateFlags2CreateInfo (flags2) in place of gi->flags, so flags2 is gi->flags with the heaps' bit for the
   descriptor buffers' one: a pipeline of heaps is made with the flags it has with sets (a cached link's
   FAIL_ON_PIPELINE_COMPILE_REQUIRED, the statistics', the shading rate's). The stages' mappings take the place of a
   layout */
static void pipe_heaps(VkGraphicsPipelineCreateInfo* gi, VkPipelineCreateFlags2CreateInfo* flags2) {
    *flags2 = (VkPipelineCreateFlags2CreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
        .pNext = gi->pNext,
        .flags = ((VkPipelineCreateFlags2)gi->flags & ~VK_PIPELINE_CREATE_2_DESCRIPTOR_BUFFER_BIT_EXT) |
                 VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT};
    gi->pNext = flags2;
    gi->flags = 0;
    gi->layout = VK_NULL_HANDLE;
}
static VkPipeline pipe_create(const Pipe* e, int* hit) {
    SNX_ZONE(z, "pipeline whole make"); /* no libraries: the driver compiles every stage */
    u64 t0 = plat_ns();
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
    VkPipelineCreateFlags2CreateInfo flags2;
    if (e->k.heaps) pipe_heaps(&gi, &flags2);
    VkPipeline p = VK_NULL_HANDLE;
    const VkShaderModule modules[5] = {e->k.vs, e->k.tcs, e->k.tes, e->k.gs, e->fs};
    pipe_note("making the graphics pipeline", e->h, modules, &e->k);
    COMPILED(VK_OK(vkCreateGraphicsPipelines(g_vk.dev, g_pipeline.pcache, 1, &gi, NULL, &p)));
    plat_thread_note_clear();
    *hit = (o.made.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0;
    SNX_ZONE_TEXT(z, "whole, %u modules, hash %016llx, cache %s", o.nst, (unsigned long long)e->h,
                  *hit ? "hit" : "a new compile");
    SNX_ZONE_VALUE(z, *hit);
    SNX_ZONE_END(z);
    SNX_PLOT("vk pipe whole make ms", (double)(plat_ns() - t0) / 1e6);
    SNX_PLOT("vk pipe whole cache hit", (double)*hit);
    if (*hit)
        atomic_fetch_add_explicit(e->prewarm ? &g_pipeline.pf.hits : &g_pipeline.pipe_hits, 1, memory_order_relaxed);
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
    _Atomic VkPipelineLayout* l = &g_pipeline.plib.layouts[fs][arrays & ((1u << 5) - 1)];
    VkPipelineLayout made = atomic_load_explicit(l, memory_order_acquire);
    if (made) return made;
    pm_lock(&g_pipeline.playout_lock);
    if (!atomic_load_explicit(&(*l), memory_order_relaxed)) {
        VkDescriptorSetLayout sets[11] = {0};
        u32 n = g_spirv_translate.split_sets ? (fs ? 7 : 10) : fs ? 2 : g_vk_stage_sets;
        for (u32 s = 0; s < n; s++)
            if ((s % 5 == 1) == fs) sets[s] = g_vk_dslayout[(arrays >> (s % 5)) & 1][s];
        /* the table: both parts read it (table_set, after every stage's set) */
        if (g_spirv_translate.tex_table) {
            n = g_spirv_translate.table_set + 1;
            sets[g_spirv_translate.table_set] = vk_table_layout();
        }
        /* vk_playout's: a link's parts take its push constants */
        VkPushConstantRange pc = {VK_PUSH_STAGES, 0, vk_push_bytes()};
        VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                         .flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT,
                                         .setLayoutCount = n,
                                         .pSetLayouts = sets,
                                         .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &pc};
        if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &made))) made = VK_NULL_HANDLE;
        atomic_store_explicit(l, made, memory_order_release);
    }
    made = atomic_load_explicit(&(*l), memory_order_relaxed);
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
       a pipeline of sample locations or coverage to color, state the parts' keys (vk_pipe_split) do not hold yet. One
       of descriptor heaps is linked: made whole, each new pipeline on a device that takes heaps (an integrated GPU's,
       vk_descriptors_auto) compiled every stage again, and a battle's new effects waited 100 to 560 ms a frame */
    if (!g_vk.has_gpl || p->k.mesh || p->k.sample_locations.samples || p->k.coverage_to_color) return;
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
/* The crash report's note of a pipeline the driver makes now (plat_thread_note), set right before the call and cleared
   right after it: a driver that crashes compiling one says only its own offset, which names no shader */
static void pipe_note(const char* what, u64 h, const VkShaderModule modules[5], const PipeKey* k) {
    char stages[320], state[PLAT_THREAD_NOTE_SIZE];
    stages_text(modules, stages, sizeof stages);
    vk_pipe_key_text(k, state, sizeof state);
    plat_thread_note("%s %016llx%s; %s", what, (unsigned long long)h, stages, state);
}
void vk_so_note(VkShaderModule mod, VkShaderStageFlagBits stage) {
    VkShaderModule modules[5] = {0};
    char stages[320];
    modules[stage == VK_SHADER_STAGE_VERTEX_BIT                    ? 0
            : stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT    ? 1
            : stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT ? 2
            : stage == VK_SHADER_STAGE_GEOMETRY_BIT                ? 3
                                                                   : 4] = mod;
    stages_text(modules, stages, sizeof stages);
    plat_thread_note("making a shader object%s", stages);
}
static void slow_say(const char* what, const VkShaderModule modules[5], u64 ns, int hit) {
    char stages[320];
    if (ns < PIPE_SLOW_NS) return;
    stages_text(modules, stages, sizeof stages);
    LOG_LIMIT(64, "vulkan: %s took %llu ms to make (%s the pipeline cache)%s\n", what,
              (unsigned long long)(ns / 1000000), hit ? "from" : "not in", stages);
}
/* e's driver statistics (pipeline_stats), p being its whole or optimized pipeline, by its stages' program hashes */
static void pipe_stats(VkPipeline p, const Pipe* e, const char* what) {
    if (!g_vk.has_exec_stats || !p) return;
    const VkShaderModule modules[5] = {e->k.vs, e->k.tcs, e->k.tes, e->k.gs, e->fs};
    u64 programs[5] = {0};
    for (int s = 0; s < 5; s++) {
        ModInfo mi;
        if (modules[s] && modinfo_get(modules[s], &mi)) programs[s] = mi.hash;
    }
    vk_pipeline_stats_write(p, what, programs);
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
                                                (o.flags & VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT) |
                                                vk_vrs_pipeline_flags() | vk_pipeline_stats_flags(),
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
    /* descriptor heaps: every part has the heaps' flag (the link's parts agree), the stage parts their stages' mappings
       (pipe_info) */
    VkPipelineCreateFlags2CreateInfo flags2;
    if (l->src.heaps) pipe_heaps(&gi, &flags2);
    VkPipeline p = VK_NULL_HANDLE;
    const u32 i = l->part == VK_PART_VI ? 0 : l->part == VK_PART_PR ? 1 : l->part == VK_PART_FS ? 2 : 3;
    const VkShaderModule modules[5] = {i == 1 ? l->src.vs : VK_NULL_HANDLE, i == 1 ? l->src.tcs : VK_NULL_HANDLE,
                                       i == 1 ? l->src.tes : VK_NULL_HANDLE, i == 1 ? l->src.gs : VK_NULL_HANDLE,
                                       i == 2 ? l->fs : VK_NULL_HANDLE};
    char what[64];
    snprintf(what, sizeof what, "making the %s pipeline library", g_part_names[i]);
    pipe_note(what, l->h, modules, &l->src);
    COMPILED(VK_OK(vkCreateGraphicsPipelines(g_vk.dev, cache, 1, &gi, NULL, &p)));
    plat_thread_note_clear();
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
    SNX_ZONE(z, "pipeline library make");
    int hit = 0;
    if (i == 1 || i == 2) pcache_note(); /* before the driver, as pipe_make */
    VkPipeline p = lib_create(l, &hit);
    [[maybe_unused]] u32 nm = i == 1 ? !!l->src.vs + !!l->src.tcs + !!l->src.tes + !!l->src.gs : i == 2 ? !!l->fs : 0;
    SNX_ZONE_TEXT(z, "%s library, %u modules, hash %016llx, cache %s", g_part_names[i], nm,
                  (unsigned long long)l->h, hit ? "hit" : "a new compile");
    SNX_ZONE_VALUE(z, hit);
    SNX_ZONE_END(z);
    u64 ns = plat_ns() - t0;
    SNX_PLOT("vk pipe lib make ms", (double)ns / 1e6);
    SNX_PLOT("vk pipe lib cache hit", (double)hit);
    char what[64];
    snprintf(what, sizeof what, "a %s pipeline library", g_part_names[i]);
    const VkShaderModule modules[5] = {i == 1 ? l->src.vs : VK_NULL_HANDLE, i == 1 ? l->src.tcs : VK_NULL_HANDLE,
                                       i == 1 ? l->src.tes : VK_NULL_HANDLE, i == 1 ? l->src.gs : VK_NULL_HANDLE,
                                       i == 2 ? l->fs : VK_NULL_HANDLE};
    slow_say(what, modules, ns, hit);
    if (i == 1 || i == 2) pcache_note();
    atomic_fetch_add_explicit(&g_pipeline.plib.made[i], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_pipeline.plib.ns[i], ns, memory_order_relaxed);
    if (hit) atomic_fetch_add_explicit(&g_pipeline.plib.hits[i], 1, memory_order_relaxed);
    pm_lock(&g_pipeline.pj_lock);
    l->p = p;
    l->state = 2;
    if (g_pipeline.pj_started) pc_broadcast(&g_pipeline.pj_done);
    pm_unlock(&g_pipeline.pj_lock);
    return p;
}
/* how gpl_link links: fast, optimized, or optimized from the pipeline cache alone */
enum { GPL_FAST, GPL_LTO, GPL_CACHED };
/* e's parts linked: fast, without link time optimization, for the draws to wait on; or optimized (GPL_LTO), into the
   pipeline cache, for them to take when it is ready; or optimized only if the cache has it (GPL_CACHED), else
   VK_NULL_HANDLE. The parts are made already */
static VkPipeline gpl_link(const Pipe* e, int mode, int* hit) {
    const int lto = mode != GPL_FAST;
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
                 (lto ? VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT : 0) |
                 (mode == GPL_CACHED ? VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT : 0) |
                 vk_vrs_pipeline_flags() | vk_pipeline_stats_flags(),
        .layout = e->layout};
    /* descriptor heaps: the heaps' flag as the parts have it; the mappings are the stage parts' */
    VkPipelineCreateFlags2CreateInfo flags2;
    if (e->k.heaps) pipe_heaps(&gi, &flags2);
    VkPipeline p = VK_NULL_HANDLE;
    const VkShaderModule modules[5] = {e->k.vs, e->k.tcs, e->k.tes, e->k.gs, e->fs};
    pipe_note(mode == GPL_CACHED ? "taking the optimized graphics pipeline from the pipeline cache"
              : lto              ? "making the optimized graphics pipeline, from libraries,"
                                 : "linking the graphics pipeline",
              e->h, modules, &e->k);
    VkResult r = VK_SUCCESS;
    COMPILED(r = vkCreateGraphicsPipelines(g_vk.dev, lto ? g_pipeline.pcache : VK_NULL_HANDLE, 1, &gi, NULL, &p));
    plat_thread_note_clear();
    /* VK_PIPELINE_COMPILE_REQUIRED (GPL_CACHED's miss) is a success code with no pipeline: not VK_OK's to log */
    if (r != VK_PIPELINE_COMPILE_REQUIRED) VK_OK(r);
    if (r != VK_SUCCESS) p = VK_NULL_HANDLE;
    *hit = (made.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0;
    return p;
}
/* e's optimized link from the pipeline cache alone: VK_NULL_HANDLE when the driver would have to compile it. A run
   after the first finds the optimized pipelines of the last one there. The draws take them from their first frame,
   and no worker makes them again behind the fast links (PIPE_OPT_WORKERS). A miss costs the driver's lookup only */
static VkPipeline gpl_cached(const Pipe* e) {
    if (!g_vk.has_cache_control || !g_pipeline.pcache || g_vk.gpl_hold) return VK_NULL_HANDLE; /* a test's hold */
    u64 t0 = plat_ns();
    SNX_ZONE(z, "pipeline from the cache");
    int hit = 0;
    VkPipeline p = gpl_link(e, GPL_CACHED, &hit);
    SNX_ZONE_TEXT(z, "hash %016llx, %s", (unsigned long long)e->h,
                  p ? "hit: the driver has the optimized link" : "miss: it would have to compile");
    SNX_ZONE_VALUE(z, p ? 1 : 0);
    SNX_ZONE_END(z);
    u64 ns = plat_ns() - t0; /* one time for the statistic and the plot, so they cannot disagree */
    SNX_PLOT("vk pipe cached lookup ms", (double)ns / 1e6);
    SNX_PLOT("vk pipe cached hit", (double)(p ? 1 : 0));
    atomic_fetch_add_explicit(&g_pipeline.plib.cached_tries, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_pipeline.plib.cached_ns, ns, memory_order_relaxed);
    if (p) atomic_fetch_add_explicit(&g_pipeline.plib.cached, 1, memory_order_relaxed);
    return p;
}
/* e's parts, made or waited for, and their fast link, or the optimized link when the cache has it (*opt):
   VK_NULL_HANDLE when one can't be made */
static VkPipeline gpl_fast(const Pipe* e, int* hit, int* opt) {
    for (u32 i = 0; i < 4; i++)
        if (!lib_get(e->lib[i])) return VK_NULL_HANDLE;
    VkPipeline cached = gpl_cached(e);
    if (cached) {
        *hit = *opt = 1;
        return cached;
    }
    u64 t0 = plat_ns();
    SNX_ZONE(z, "pipeline link");
    VkPipeline p = gpl_link(e, GPL_FAST, hit);
    SNX_ZONE_TEXT(z, "hash %016llx, %s", (unsigned long long)e->h,
                  *hit ? "driver cache hit on the fast link" : "cache miss: the driver compiled it");
    SNX_ZONE_VALUE(z, *hit);
    SNX_ZONE_END(z);
    u64 ns = plat_ns() - t0; /* one time for the statistic and the plot, so they cannot disagree */
    SNX_PLOT("vk pipe fast link ms", (double)ns / 1e6);
    SNX_PLOT("vk pipe fast link cache hit", (double)*hit);
    atomic_fetch_add_explicit(&g_pipeline.plib.links, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_pipeline.plib.link_ns, ns, memory_order_relaxed);
    return p;
}
/* on a worker, after e's fast link: the optimized pipeline, which the draws take from then on (vk_pipeline_resolve
   and the others read e->p) */
static void pipe_optimize(Pipe* e) {
    u64 t0 = plat_ns();
    SNX_ZONE(z, "pipeline optimize");
    int hit = 0;
    pcache_note(); /* before the driver, as pipe_make */
    VkPipeline o = gpl_link(e, GPL_LTO, &hit);
    SNX_ZONE_TEXT(z, "hash %016llx, %s", (unsigned long long)e->h,
                  !o ? "failed: the fast link stays" : hit ? "driver cache hit" : "a new optimized compile");
    SNX_ZONE_VALUE(z, hit);
    SNX_ZONE_END(z);
    u64 ns = plat_ns() - t0;
    SNX_PLOT("vk pipe optimize ms", (double)ns / 1e6);
    SNX_PLOT("vk pipe optimize cache hit", (double)hit);
    const VkShaderModule modules[5] = {e->k.vs, e->k.tcs, e->k.tes, e->k.gs, e->fs};
    slow_say("an optimized pipeline, in the background,", modules, ns, hit);
    pcache_note();
    atomic_fetch_add_explicit(&g_pipeline.plib.opts, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_pipeline.plib.opt_ns, ns, memory_order_relaxed);
    if (hit) atomic_fetch_add_explicit(&g_pipeline.plib.opt_hits, 1, memory_order_relaxed);
    if (!o) { /* the fast link stays */
        char stages[320];
        stages_text(modules, stages, sizeof stages);
        LOG_LIMIT(16, "vulkan: an optimized pipeline could not be made: its link without optimization stays%s\n",
                  stages);
        return;
    }
    pipe_stats(o, e, "optimized");
    atomic_store_explicit(&e->p, o, memory_order_release);
    atomic_store_explicit(&e->opt, 1, memory_order_release);
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
        made[i] = atomic_load_explicit(&g_pipeline.plib.made[i], memory_order_relaxed);
        hits[i] = atomic_load_explicit(&g_pipeline.plib.hits[i], memory_order_relaxed);
        make_ms[i] = atomic_load_explicit(&g_pipeline.plib.ns[i], memory_order_relaxed) / 1000000;
    }
    *links = atomic_load_explicit(&g_pipeline.plib.links, memory_order_relaxed);
    *link_ms = atomic_load_explicit(&g_pipeline.plib.link_ns, memory_order_relaxed) / 1000000;
    *opts = atomic_load_explicit(&g_pipeline.plib.opts, memory_order_relaxed);
    *opt_ms = atomic_load_explicit(&g_pipeline.plib.opt_ns, memory_order_relaxed) / 1000000;
}
void vk_pipeline_lib_cached_stats(u64* tries, u64* cached, u64* ms) {
    *tries = atomic_load_explicit(&g_pipeline.plib.cached_tries, memory_order_relaxed);
    *cached = atomic_load_explicit(&g_pipeline.plib.cached, memory_order_relaxed);
    *ms = atomic_load_explicit(&g_pipeline.plib.cached_ns, memory_order_relaxed) / 1000000;
}
void vk_pipeline_lib_warm_stats(u64* made, u64* hit, u64* ms) {
    *made = atomic_load_explicit(&g_pipeline.plib.warm_made, memory_order_relaxed);
    *hit = atomic_load_explicit(&g_pipeline.plib.warm_hit, memory_order_relaxed);
    *ms = atomic_load_explicit(&g_pipeline.plib.warm_ns, memory_order_relaxed) / 1000000;
}
#define PIPE_FAST_NS 5000000ull /* a pipeline made faster than this came from a cache (g_pipeline.pipe_fast) */
/* e's pipeline, made here. One a draw asked for is kept in the file (pf_pipe_record): once made when the GPU thread
   makes it (record), and when it queues it for a worker otherwise, as the file's tables are the GPU thread's */
static VkPipeline pipe_make(Pipe* e, int record, int* opt) {
    SNX_ZONE_T(z_pipe, "pipeline make", "hash %016llx", (unsigned long long)e->h);
    /* Rename the open zone with the pipeline's own identity, so a make of one program reads by name on the timeline
       without expanding its text; the client copies the formatted name at the call. */
    SNX_ZONE_NAME_F(z_pipe, "pipeline make %016llx", (unsigned long long)e->h);
    u64 t0 = plat_ns();
    SNX_ZONE(z, "pipeline make compile"); /* the outer "pipeline make" also covers shader objects and takes no callstack */
    /* A new-area pipeline can arrive after a long quiet spell. Mark it before entering the driver, so a periodic
       cache snapshot doesn't mistake an in-flight compile for idleness and contend for the driver's cache lock. */
    int so = atomic_load_explicit(&e->so, memory_order_relaxed);
    if (!so) pcache_note();
    int hit = 0;
    *opt = 0;
    VkPipeline p = VK_NULL_HANDLE;
    /* shader objects go to no pipeline cache: making them leaves it as it is */
    if (atomic_load_explicit(&e->so, memory_order_relaxed)) {
        if (vk_so_make(&e->k, e->fs, e->sh))
            p = PIPE_SO_MADE;
        else /* made as a pipeline, whole: it has no parts */
            atomic_store_explicit(&e->so, 0, memory_order_relaxed);
    }
    if (!p) p = e->lib[0] ? gpl_fast(e, &hit, opt) : pipe_create(e, &hit);
    const VkShaderModule pm[5] = {e->k.vs, e->k.tcs, e->k.tes, e->k.gs, e->fs};
    u32 nm = 0;
    for (int s = 0; s < 5; s++) nm += !!pm[s];
    const char* path = p == PIPE_SO_MADE ? "shader objects" : e->lib[0] ? "link of libraries" : "whole compile";
    SNX_ZONE_TEXT(z, "%s, %u modules, hash %016llx, cache %s", path, nm, (unsigned long long)e->h,
                  hit ? "hit" : "miss");
    SNX_ZONE_VALUE(z, hit);
    SNX_ZONE_END(z);
    SNX_ZONE_TEXT(z_pipe, "%s, %u modules, cache %s", path, nm, hit ? "hit" : "miss");
    SNX_ZONE_VALUE(z_pipe, p == PIPE_SO_MADE ? 3 : e->lib[0] ? 2 : 1); /* 1 whole, 2 libraries, 3 objects */
    (void)nm;
    (void)path;
    /* Only a compile the driver really did gets a callstack: taking one on each cache hit is costly, and a hit's
       caller is not the one that made the shader */
    if (p && p != PIPE_SO_MADE && !hit) {
        SNX_ZONE_CS(z_cs, "pipeline make compile callstack", 4); /* four frames: the draw's path into the driver */
        /* the compile that earned a call stack also says which pipeline it made, without a name change */
        SNX_ZONE_VALUE(z_cs, e->h);
        SNX_ZONE_TEXT(z_cs, "%s, %u modules, hash %016llx", path, nm, (unsigned long long)e->h);
    }
    if (p != PIPE_SO_MADE && (!e->lib[0] || *opt))
        pipe_stats(p, e, *opt ? "optimized" : "whole"); /* a fast link runs unoptimized code */
    u64 ns = plat_ns() - t0; /* one time for the log, the statistic and the plot, so they cannot disagree */
    if (p == PIPE_SO_MADE) {
        if (!e->prewarm) atomic_fetch_add_explicit(&g_pipeline.pipe_so, 1, memory_order_relaxed);
        const VkShaderModule modules[5] = {e->k.vs, e->k.tcs, e->k.tes, e->k.gs, e->fs};
        slow_say("a draw's shader objects", modules, ns, 0);
    } else {
        if (p && ns < PIPE_FAST_NS)
            atomic_fetch_add_explicit(e->prewarm ? &g_pipeline.pf.fast : &g_pipeline.pipe_fast, 1,
                                      memory_order_relaxed);
        pipe_slow_say(e, ns, hit);
        pcache_note();
    }
    if (e->prewarm)
        atomic_fetch_add_explicit(&g_pipeline.pf.made, 1, memory_order_relaxed);
    else {
        atomic_fetch_add_explicit(&g_pipeline.pipe_make_ns, ns, memory_order_relaxed);
        SNX_PLOT("vk pipe make ms", (double)ns / 1e6);
        SNX_PLOT("vk pipe make cache hit", (double)hit);
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
static void sh_fold_make(ShFold* f);
static ShFold* fold_take(void);
/* on a worker: make a shipped program's part into the pipeline cache, then drop the module and the part pipeline (a
   draw's own module later hits the cache by its SPIR-V, which is the cache's key) */
static void warm_run(WarmItem* w) {
    PipeLib lib = {.src = w->src, .fs = w->fs, .part = w->part};
    int hit = 0;
    u64 t0 = plat_ns();
    VkPipeline p = lib_create(&lib, &hit);
    if (p) SNX_MEM_ALLOC_D((const void*)(uintptr_t)p, 0, "pipelines");
    u64 ns = plat_ns() - t0;
    atomic_fetch_add_explicit(&g_pipeline.plib.warm_ns, ns, memory_order_relaxed);
    pcache_note();
    atomic_fetch_add_explicit(&g_pipeline.plib.warm_made, 1, memory_order_relaxed);
    if (hit) atomic_fetch_add_explicit(&g_pipeline.plib.warm_hit, 1, memory_order_relaxed);
    if (p) {
        SNX_MEM_FREE_D((const void*)(uintptr_t)p, "pipelines");
        vkDestroyPipeline(g_vk.dev, p, NULL);
    }
    VkShaderModule mod = w->part == VK_PART_PR ? w->src.vs : w->fs;
    if (mod) {
        SNX_MEM_FREE_D((const void*)(uintptr_t)mod, "shader modules");
        vkDestroyShaderModule(g_vk.dev, mod, NULL);
    }
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
        while (g_pipeline.held || (!g_pipeline.pj_head && !g_pipeline.fold_queued && !g_pipeline.pw_head &&
               !(g_pipeline.po_head && g_pipeline.po_busy < PIPE_OPT_WORKERS) &&
               !(g_pipeline.warm_head && g_pipeline.warm_busy < WARM_WORKERS)))
            pc_wait(&g_pipeline.pj_work, &g_pipeline.pj_lock, -1);
        if (!g_pipeline.pj_head && g_pipeline.fold_queued) { /* after the draws' pipelines, before the file's */
            ShFold* f = fold_take();
            g_pipeline.pj_busy++;
            pm_unlock(&g_pipeline.pj_lock);
            sh_fold_make(f);
            pm_lock(&g_pipeline.pj_lock);
            g_pipeline.pj_busy--;
            f->state = SH_FOLD_MADE;
            pc_broadcast(&g_pipeline.pj_done);
            pm_unlock(&g_pipeline.pj_lock);
            continue;
        }
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
        int opt;
        p = pipe_make(e, 0, &opt);
        pm_lock(&g_pipeline.pj_lock);
        g_pipeline.pj_busy--;
        atomic_store_explicit(&e->p, p, memory_order_release);
        atomic_store_explicit(&e->opt, opt, memory_order_release);
        atomic_store_explicit(&e->ready, 1, memory_order_release);
        /* a test's hold: the link stays; else its optimized pipeline once its draws asked for it (vk_pipeline_bound) */
        if (p && e->lib[0] && !opt && !g_vk.gpl_hold && e->opt_wanted) pipe_queue(e, 3);
        pc_broadcast(&g_pipeline.pj_done);
        pm_unlock(&g_pipeline.pj_lock);
    }
}
void vk_pipeline_hold(int on) {
    pm_lock(&g_pipeline.pj_lock);
    g_pipeline.held = on;
    if (!on) pc_broadcast(&g_pipeline.pj_work);
    pm_unlock(&g_pipeline.pj_lock);
}
void vk_pipeline_quiesce(void) {
    if (g_pipeline.pf
            .on) { /* the reading thread queues the file's pipelines, then may translate the shipped programs */
        pm_lock(&g_pipeline.pf.lock);
        while (!g_pipeline.pf.loaded) pc_wait(&g_pipeline.pf.done, &g_pipeline.pf.lock, -1);
        pm_unlock(&g_pipeline.pf.lock);
        while (!atomic_load_explicit(&g_pipeline.pf.ship_ready, memory_order_acquire)) plat_sleep_ns(1000000);
    }
    pm_lock(&g_pipeline.pj_lock);
    /* the optimized pipelines still to make are left: the fast links serve until the end */
    for (Pipe* e = g_pipeline.po_head; e; e = e->job) e->queued = 0;
    g_pipeline.po_head = g_pipeline.po_tail = NULL;
    while (g_pipeline.pj_threads &&
           (g_pipeline.pj_head || g_pipeline.pw_head || g_pipeline.fold_queued || g_pipeline.pj_busy))
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
    while (!atomic_load_explicit(&e->ready, memory_order_relaxed)) {
        u64 now = plat_ns();
        if (ns >= 0 && now >= until) break;
        pc_wait(&g_pipeline.pj_done, &g_pipeline.pj_lock, ns < 0 ? -1 : (i64)(until - now));
    }
    pm_unlock(&g_pipeline.pj_lock);
    SNX_ZONE_END(z);
    return atomic_load_explicit(&e->ready, memory_order_acquire) ? atomic_load_explicit(&e->p, memory_order_acquire)
                                                                 : VK_NULL_HANDLE;
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
    atomic_fetch_add_explicit(&g_pipeline.pipe_wait_ns, plat_ns() - t0, memory_order_relaxed);
    return got;
}
/* k's entry, found or added (NULL with no memory): *found says whether it was there */
static Pipe* pipe_entry(const PipeKey* k, int* found) {
    SNX_ZONE_V(z, "pipeline entry lookup", 0); /* 0 new, 1 in the recent four, 2 in the hash table */
    pf_merge();
    for (u32 i = 0; i < 4 && g_pipeline.recent[i]; i++)
        if (!memcmp(&g_pipeline.recent[i]->k, k, sizeof *k)) {
            *found = 1;
            SNX_ZONE_VALUE(z, 1);
            return g_pipeline.recent[i];
        }
    u64 h = vk_hash(k, sizeof *k);
    u32 b = (u32)(h ^ (h >> 32)) & (PIPEH - 1);
    for (Pipe* p = g_pipeline.pipeh[b]; p; p = p->next)
        if (p->h == h && !memcmp(&p->k, k, sizeof *k)) {
            *found = 1;
            SNX_ZONE_VALUE(z, 2);
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
    if (vk_so_takes(k, p->fs)) {
        atomic_store_explicit(&p->so, p->so_layout = 1, memory_order_relaxed);
        p->layout = playout(k->arrays, 0); /* a pipeline made in its place takes it too */
    } else
        pipe_parts(p);
    p->next = g_pipeline.pipeh[b];
    g_pipeline.pipeh[b] = p;
    g_pipeline.pipes_new++;
    g_pipeline.recent[g_pipeline.recent_next++ & 3] = p;
    return p;
}
/* the handle a draw gets for p once made: its pipeline, or for shader objects a stand-in, which vk_pipeline_bind binds
   as the objects (VK_NULL_HANDLE for none made, or no stand-in) */
static VkPipeline pipe_handle(Pipe* p, VkPipeline made) {
    return made && p->so_layout ? pipe_standin(p) : made;
}
/* a pipeline made here, on the thread that asks for it: with pipeline libraries its fast link, and the optimized one
   queued for the workers (made here too without them) */
static VkPipeline pipe_make_here(Pipe* p, int record) {
    u64 t0 = plat_ns();
    int opt;
    VkPipeline made = pipe_make(p, record, &opt);
    atomic_store_explicit(&p->p, made, memory_order_release);
    atomic_store_explicit(&p->opt, opt, memory_order_release);
    atomic_store_explicit(&p->ready, 1, memory_order_release);
    atomic_fetch_add_explicit(&g_pipeline.pipe_wait_ns, plat_ns() - t0, memory_order_relaxed);
    if (made && p->lib[0] && !opt && !g_vk.gpl_hold) { /* a test's hold: the link stays */
        pm_lock(&g_pipeline.pj_lock);
        int queued = pipe_workers() != 0;
        if (!p->opt_wanted) g_pipeline.plib.opt_asked++;
        p->opt_wanted = 1;
        if (queued) pipe_queue(p, 3);
        pm_unlock(&g_pipeline.pj_lock);
        if (!queued) pipe_optimize(p);
    }
    return made;
}
/* p's optimized pipeline asked for: queued now when its link is made, else by the worker that makes the link
   (pipe_worker). Nothing for a pipeline of no parts, an optimized one, one the cache had, or one asked before */
static void pipe_want_opt(Pipe* p) {
    if (!p->lib[0] || p->so_layout || atomic_load_explicit(&p->opt, memory_order_acquire)) return;
    pm_lock(&g_pipeline.pj_lock);
    if (!p->opt_wanted) {
        p->opt_wanted = 1;
        g_pipeline.plib.opt_asked++;
        if (atomic_load_explicit(&p->ready, memory_order_relaxed) &&
            atomic_load_explicit(&p->p, memory_order_relaxed) && !atomic_load_explicit(&p->opt, memory_order_relaxed) &&
            !g_vk.gpl_hold && !p->queued && pipe_workers())
            pipe_queue(p, 3);
    }
    pm_unlock(&g_pipeline.pj_lock);
}
void vk_pipeline_bound(VkPipeline h) {
    Pipe* p = standin_pipe(h);
    /* used_present is the present + 1, so that 0 is none */
    if (!p || !p->lib[0] || p->used_presents >= PIPE_OPT_PRESENTS || p->used_present == g_vk_present + 1u) return;
    p->used_present = g_vk_present + 1u;
    if (++p->used_presents == PIPE_OPT_PRESENTS) pipe_want_opt(p);
}
u64 vk_pipeline_opt_asked(void) {
    pm_lock(&g_pipeline.pj_lock);
    u64 n = g_pipeline.plib.opt_asked;
    pm_unlock(&g_pipeline.pj_lock);
    return n;
}
/* the pipeline for k: made here when new, waited for when one of the file's is still being made */
VkPipeline vk_pipeline_get(const PipeKey* k) {
    int found;
    Pipe* p = pipe_entry(k, &found);
    if (!p) return VK_NULL_HANDLE;
    if (found) {
        if (!atomic_load_explicit(&p->ready, memory_order_acquire)) {
            if (p->prewarm) pipe_promote(p);
            (void)pipe_wait_here(p);
        }
        /* its draws bind the pipeline's own handle, not a stand-in that vk_pipeline_bound counts the presents of: a
           link a worker made (the file's) is optimized from its first draw, as pipe_make_here does */
        pipe_want_opt(p);
        return pipe_handle(p, atomic_load_explicit(&p->p, memory_order_acquire));
    }
    return pipe_handle(p, pipe_make_here(p, 1));
}
/* the key a draw's pipeline is looked up by: with the blend as dynamic state, one pipeline for every blend of a draw's
   state, the draw setting its blend (render.c), into *dyn; else k. vk_pipeline_get's callers bind theirs with the blend
   in it */
static const PipeKey* pipe_drawn_key(const PipeKey* k, PipeKey* dyn) {
    return g_vk.dyn_blend && vk_pipe_blend_key(k, dyn) ? dyn : k;
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
    PipeKey dyn;
    k = pipe_drawn_key(k, &dyn);
    if (!vk_rec_threaded()) return vk_pipeline_get(k);
    int found;
    Pipe* p = pipe_entry(k, &found);
    if (!p) return VK_NULL_HANDLE;
    if (found) {
        if (atomic_load_explicit(&p->ready, memory_order_acquire)) {
            VkPipeline made = atomic_load_explicit(&p->p, memory_order_acquire);
            if (p->so_layout) return pipe_handle(p, made);
            if (!p->lib[0] || !made || atomic_load_explicit(&p->opt, memory_order_acquire)) return made;
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
        if (!queued) return pipe_handle(p, pipe_make_here(p, 0)); /* no worker: made here */
    }
    VkPipeline s = pipe_standin(p);
    return s ? s : pipe_handle(p, pipe_wait_here(p));
}
/* the entry a stand-in names, NULL for a pipeline's own handle */
static struct Pipe* standin_pipe(VkPipeline h) {
    u64 v = (u64)(uintptr_t)h;
    if ((v & ~(u64)0xFFFFFFFFull) != PIPE_STANDIN) return NULL;
    u32 i = (u32)v - 1;
    return g_pipeline.pstand[i / PSTAND_PAGE][i % PSTAND_PAGE];
}
/* what h names once p is ready: the pipeline, or for shader objects h itself (VK_NULL_HANDLE when none was made) */
static VkPipeline standin_made(Pipe* p, VkPipeline h) {
    VkPipeline made = atomic_load_explicit(&p->p, memory_order_acquire);
    return made && atomic_load_explicit(&p->so, memory_order_relaxed) ? h : made;
}
/* on the recording thread: the pipeline a handle from vk_pipeline_get_deferred names, waited for while a worker makes
   it. VK_NULL_HANDLE when it couldn't be made */
VkPipeline vk_pipeline_resolve(VkPipeline h) {
    Pipe* p = standin_pipe(h);
    if (!p) return h;
    if (atomic_load_explicit(&p->ready, memory_order_acquire)) return standin_made(p, h);
    u64 t0 = plat_ns();
    (void)pipe_wait(p, -1);
    atomic_fetch_add_explicit(&g_pipeline.pipe_wait_ns, plat_ns() - t0, memory_order_relaxed);
    return standin_made(p, h);
}
/* the pipeline a handle from vk_pipeline_get_deferred names once it is ready, else the handle: without waiting */
VkPipeline vk_pipeline_ready(VkPipeline h) {
    Pipe* p = standin_pipe(h);
    return p && atomic_load_explicit(&p->ready, memory_order_acquire) ? standin_made(p, h) : h;
}
/* the pipeline h names without waiting for it: VK_NULL_HANDLE with *pending set while a worker still makes it, and
   VK_NULL_HANDLE alone when it couldn't be made */
VkPipeline vk_pipeline_now(VkPipeline h, int* pending) {
    Pipe* p = standin_pipe(h);
    *pending = p && !atomic_load_explicit(&p->ready, memory_order_acquire);
    return *pending ? VK_NULL_HANDLE : p ? standin_made(p, h) : h;
}
void vk_pipeline_bind(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline h) {
    Pipe* p = standin_pipe(h);
    if (p && atomic_load_explicit(&p->so, memory_order_relaxed))
        vk_so_bind(cb, &p->k, p->sh);
    else
        vkCmdBindPipeline(cb, bp, p ? atomic_load_explicit(&p->p, memory_order_acquire) : h);
}
/* ------------------------------------------------------------ folds made beside the draws (ShFold) */
/* a fold's records from at on (the reads, the loads' words, the others'), and its code after them */
static VkCbRead* fold_rec(ShFold* f, u32 at) { return (VkCbRead*)(void*)(f + 1) + at; }
static u8* fold_code(ShFold* f) { return (u8*)fold_rec(f, f->nrec); }
static const VkCbRead* fold_copy(const VkCbRead* r, u32 n, u32 idx, u32 off) {
    for (u32 i = 0; i < n; i++)
        if (r[i].idx == idx && r[i].off == off) return &r[i];
    return NULL;
}
/* on a worker, the reader of a fold's translation: what from's translation read there. A read it did not make is the
   fold's to drop, as nothing says what the draws hold there */
static int fold_job_read(u32 idx, u32 off, u32* out) {
    ShFold* f = t_pipeline.fold;
    const VkCbRead* c = fold_copy(fold_rec(f, 0), f->nreads, idx, off);
    if (!c) {
        if (!f->missed) {
            f->missed_idx = idx;
            f->missed_off = off;
        }
        f->missed = 1;
        return 0;
    }
    *out = c->v;
    return c->ok == VK_CB_READ;
}
/* and its bytes (VkCbBytes): the word the draw had at a load, NULL where it had none that it could read */
static const u8* fold_job_bytes(u32 idx, u32 off, u32 size) {
    ShFold* f = t_pipeline.fold;
    const VkCbRead* c = size == 4 ? fold_copy(fold_rec(f, f->nreads), f->nloads, idx, off) : NULL;
    if (!c) return NULL;
    snx_st_le32(t_pipeline.word, c->v);
    return t_pipeline.word;
}
/* on a worker, with no lock held: f translated from its copies, into f->made (NULL when it can't be) */
static void sh_fold_make(ShFold* f) {
    VkCbReads others[SH_CB_FOLD_VARIANTS];
    const VkCbReads* other[SH_CB_FOLD_VARIANTS];
    u32 at = f->nreads + f->nloads;
    for (u32 i = 0; i < f->nothers; i++) {
        others[i] = (VkCbReads){fold_rec(f, at), f->nother[i]};
        other[i] = &others[i];
        at += f->nother[i];
    }
    const VkCbFold fold = {.mode = f->mode, .bytes = fold_job_bytes, .others = other, .nothers = f->nothers};
    VkSh* s = calloc(1, sizeof *s);
    size_t nw = 0;
    u64 t0 = plat_ns();
    t_pipeline.fold = f;
    u32* spv =
        s ? sh_spirv(s, fold_code(f), f->h, ST_FS, f->mask, f->len, f->qh, 0, 0, NULL, fold_job_read, &fold, 0, &nw)
          : NULL;
    t_pipeline.fold = NULL;
    f->ns = plat_ns() - t0;
    if (spv && !f->missed && s->nfold) {
        s->fold_asked = 1; /* it folds what the draws' words allow */
        f->made = s;
        f->spv = spv;
        f->nw = nw;
        return;
    }
    if (!s)
        WARN_LIMIT(
            8,
            "vulkan: no memory for fragment program %016llx's fold of its draws' words: the translation that folds "
            "none draws them\n",
            (unsigned long long)f->h);
    else if (f->missed)
        WARN_LIMIT(
            8,
            "vulkan: fragment program %016llx's fold of its draws' words read c[%u][%#x], which their translation "
            "did not: dropped, that one draws them\n",
            (unsigned long long)f->h, f->missed_idx, f->missed_off);
    else if (!spv)
        WARN_LIMIT(
            8,
            "vulkan: fragment program %016llx's fold of its draws' words failed (%s): the translation that folds "
            "none draws them\n",
            (unsigned long long)f->h,
            s->si.first_unsupported[0] ? s->si.first_unsupported : "unsupported feature or float controls");
    else
        WARN_LIMIT(8,
                   "vulkan: fragment program %016llx's fold of its draws' words folded none of the %u they had at its "
                   "loads: dropped\n",
                   (unsigned long long)f->h, f->nloads);
    free(spv);
    if (s) free(s->cbr.r);
    free(s);
}
/* the queued fold a worker takes, the oldest (pj_lock) */
static ShFold* fold_take(void) {
    ShFold* oldest = NULL;
    for (u32 i = 0; i < SH_FOLD_JOBS; i++) {
        ShFold* f = g_pipeline.fold_jobs[i];
        if (f && f->state == SH_FOLD_QUEUED && (!oldest || f->seq < oldest->seq)) oldest = f;
    }
    oldest->state = SH_FOLD_MAKING;
    g_pipeline.fold_queued--;
    return oldest;
}
/* whether a fold of s's program is being made or warmed: one at a time, so that the next one's mode counts it */
static int sh_fold_pending(const VkSh* s) {
    for (u32 i = 0; i < SH_FOLD_JOBS; i++) {
        const ShFold* f = g_pipeline.fold_jobs[i];
        if (f && sh_same_program(f->from, s->hash, s->stage, s->mask, s->clen, s->qh, s->clen_a, s->qh_a)) return 1;
    }
    return 0;
}
/* a fold of s's program for fold's words, which the draw state's buffers (sh_ds) hold now, queued to the workers:
   the program read from gva again, and copies of what its translation reads. With no worker it is made here, and
   put in the tables at once, as a draw's translation was before. Nothing when no slot is free (s's draws count on) */
static void sh_fold_end(ShFold* f, int publish, int again);
static int sh_fold_adopt(ShFold* f);
/* a free slot for a fold, SH_FOLD_JOBS for none: where every one is taken, a made fold whose from no draw took in
   SH_CB_FOLD_STABLE submits is dropped for it (its from's draws count again when they come) */
static u32 sh_fold_slot(void) {
    const u64 submit = vk_submit_number();
    u32 slot = 0;
    while (slot < SH_FOLD_JOBS && g_pipeline.fold_jobs[slot]) slot++;
    for (u32 i = 0; i < SH_FOLD_JOBS && slot == SH_FOLD_JOBS; i++) {
        ShFold* f = g_pipeline.fold_jobs[i];
        pm_lock(&g_pipeline.pj_lock);
        int made = f->warm || f->state == SH_FOLD_MADE;
        pm_unlock(&g_pipeline.pj_lock);
        if (made && submit - f->seen_submit >= SH_CB_FOLD_STABLE) {
            sh_fold_end(f, 0, 1);
            slot = i;
        }
    }
    return slot;
}
static void sh_fold_queue(VkSh* s, u64 gva, const VkCbFold* fold) {
    u32 slot = sh_fold_slot(), nrec = s->cbr.n;
    if (slot == SH_FOLD_JOBS) return;
    for (u32 i = 0; i < fold->nothers; i++) nrec += fold->others[i]->n;
    if ((u64)nrec > (SIZE_MAX - sizeof(ShFold) - SH_CODE) / sizeof(VkCbRead)) return;
    ShFold* f = calloc(1, sizeof *f + nrec * sizeof(VkCbRead) + SH_CODE);
    if (!f) return;
    f->nrec = nrec;
    u8* code = fold_code(f);
    u32 len = 0;
    vk_shader_read_code(gva, code, &len, sh_cb_read);
    u64 qh = vk_hash(code, len);
    u32 mlen = len;
    u64 h = vk_shader_hash(code, SH_CODE, &mlen, sh_cb_read);
    /* the code at gva is no longer s's program: its draws are as they are */
    if (h != s->hash || len != s->clen || qh != s->qh || s->clen_a) {
        free(f);
        s->fold_asked = 1;
        return;
    }
    VkCbRead* r = fold_rec(f, 0);
    for (u32 i = 0; i < s->cbr.n; i++)
        if (s->cbr.r[i].ok <= VK_CB_READ) r[f->nreads++] = s->cbr.r[i];
    VkCbRead* l = fold_rec(f, f->nreads);
    for (u32 i = 0; i < s->cbr.n; i++) {
        const VkCbRead* c = &s->cbr.r[i];
        const u8* p = c->ok == VK_CB_LOAD ? sh_cb_bytes(c->idx, c->off, 4) : NULL;
        if (p) l[f->nloads++] = (VkCbRead){c->idx, c->off, VK_CB_READ, snx_ld_le32(p)};
    }
    VkCbRead* o = fold_rec(f, f->nreads + f->nloads);
    for (u32 i = 0; i < fold->nothers; i++) {
        if (fold->others[i]->n) memcpy(o, fold->others[i]->r, fold->others[i]->n * sizeof *o);
        f->nother[i] = fold->others[i]->n;
        o += f->nother[i];
    }
    f->nothers = fold->nothers;
    f->mode = fold->mode;
    f->from = s;
    f->h = h;
    f->qh = qh;
    f->len = len;
    f->mlen = mlen;
    f->seen_submit = vk_submit_number();
    memcpy(f->mask, s->mask, sizeof f->mask);
    s->fold_asked = 1;
    s->fold_job = (u8)(slot + 1);
    pm_lock(&g_pipeline.pj_lock);
    int workers = pipe_workers();
    g_pipeline.fold_jobs[slot] = f;
    if (workers) {
        f->state = SH_FOLD_QUEUED;
        f->seq = ++g_pipeline.fold_seq;
        g_pipeline.fold_queued++;
        pc_signal(&g_pipeline.pj_work);
    }
    pm_unlock(&g_pipeline.pj_lock);
    if (workers) return;
    sh_fold_make(f);
    if (sh_fold_adopt(f))
        sh_fold_end(f, 1, 0);
    else
        sh_fold_end(f, 0, 0);
}
/* on the GPU thread, f made: its module, as a draw's translation's is made and kept (0 when it can't be). Its
   pipelines are made from the draws' keys next */
static int sh_fold_adopt(ShFold* f) {
    VkSh* s = f->made;
    if (!s) return 0;
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = f->nw * 4, .pCode = f->spv};
    if (!VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &s->mod))) {
        WARN_LIMIT(8,
                   "vulkan: vkCreateShaderModule failed for fragment program %016llx's fold of its draws' words: the "
                   "translation that folds none draws them\n",
                   (unsigned long long)s->hash);
        return 0;
    }
    modinfo_put(s->mod, s->hash, s->stage, f->nw);
    vk_so_keep_spirv(s->mod, f->spv, f->nw);
    g_pipeline.sh_new++;
    g_pipeline.sh_folds++;
    g_pipeline.sh_ns += f->ns;
    f->warm = 1;
    f->warm_submit = vk_submit_number();
    return 1;
}
/* f's end: its translation put in the tables (publish), where the next lookups give it to the draws whose words it
   holds, and kept for the next run with its twins' pipelines; or dropped, and from's draws count their words again
   (again) or go on as they are. A dropped translation's module stays, as every module does for the run: a twin's
   pipeline may be made from it */
static void sh_fold_end(ShFold* f, int publish, int again) {
    VkSh* from = f->from;
    VkSh* s = f->made;
    if (publish) {
        pf_shader_record(s, f->spv, f->nw, fold_code(f), src_bytes(f->mlen), NULL);
        for (u32 i = 0; i < f->ntwins; i++) pf_pipe_record(&f->twins[i]->k);
        s->next = g_pipeline.shh[s->hash & 1023];
        g_pipeline.shh[s->hash & 1023] = s;
        g_pipeline.fold_gen++;
    } else {
        if (s) free(s->cbr.r);
        free(s);
        g_pipeline.fold_dropped++;
        from->fold_asked = !again;
        from->fold_stable = (VkCbStable){0};
    }
    free(f->spv);
    pm_lock(&g_pipeline.pj_lock);
    g_pipeline.fold_jobs[from->fold_job - 1] = NULL;
    pm_unlock(&g_pipeline.pj_lock);
    from->fold_job = 0;
    free(f);
}
/* a draw of from, with key k, while its fold is made or warmed: the fold's module once made, then its pipeline with
   k's state where the fold holds the draw's words, made by the workers beside the draws. The fold goes in the tables
   once every one of them is made, a submit after the last was queued, so a draw that takes it waits for none; a twin
   that could not be made keeps it out. One whose words no draw had in SH_CB_FOLD_STABLE submits is dropped */
static void sh_fold_warm(VkSh* from, const PipeKey* k) {
    ShFold* f = g_pipeline.fold_jobs[from->fold_job - 1];
    f->seen_submit = vk_submit_number();
    if (!f->warm) {
        pm_lock(&g_pipeline.pj_lock);
        int made = f->state == SH_FOLD_MADE;
        pm_unlock(&g_pipeline.pj_lock);
        if (!made) return;
        if (!sh_fold_adopt(f)) {
            sh_fold_end(f, 0, 0);
            return;
        }
    }
    VkSh* s = f->made;
    u64 submit = vk_submit_number();
    if (k && f->ntwins < SH_FOLD_TWINS && vk_cb_folds_hold(&s->cbr, sh_cb_read, sh_cb_bytes)) {
        /* the key the draw that takes the fold looks its pipeline up by (pipe_drawn_key), or the twin is one no draw
           finds and that draw makes its pipeline after all */
        PipeKey with_fold = *k, dyn;
        with_fold.fs = s->mod;
        PipeKey twin = *pipe_drawn_key(&with_fold, &dyn);
        u32 i = 0;
        while (i < f->ntwins && memcmp(&f->twins[i]->k, &twin, sizeof twin)) i++;
        if (i == f->ntwins) {
            int had = 0;
            Pipe* p = pipe_entry(&twin, &had);
            if (p && !had) {
                pm_lock(&g_pipeline.pj_lock);
                int queued = pipe_workers() != 0;
                if (queued) pipe_queue(p, 2);
                pm_unlock(&g_pipeline.pj_lock);
                if (!queued) (void)pipe_make_here(p, 0);
            }
            if (p) {
                f->twins[f->ntwins++] = p;
                f->twin_submit = submit;
                g_pipeline.fold_twins++;
            }
        }
    }
    int ready = 1, made = 1;
    for (u32 i = 0; i < f->ntwins; i++) {
        int r = atomic_load_explicit(&f->twins[i]->ready, memory_order_acquire);
        ready &= r;
        made &= !r || atomic_load_explicit(&f->twins[i]->p, memory_order_acquire) != VK_NULL_HANDLE;
    }
    if (f->ntwins && ready && submit != f->twin_submit) {
        if (!made)
            LOG_LIMIT(8,
                      "vulkan: a pipeline of fragment program %016llx's fold of its draws' words could not be made: "
                      "dropped, the translation that folds none draws them\n",
                      (unsigned long long)s->hash);
        sh_fold_end(f, made, 0);
    } else if (!f->ntwins && submit - f->warm_submit >= SH_CB_FOLD_STABLE)
        sh_fold_end(f, 0, 1);
}
void vk_shader_fold_draw(const DsState* ds, VkSh* s, u64 gva, const PipeKey* k) {
    if (!s->fold_job && (s->fold_asked || s->failed || s->stage != ST_FS)) return;
    g_pipeline.sh_ds = ds;
    g_pipeline.sh_bind = sh_bind_stage(ST_FS);
    if (s->fold_job) {
        sh_fold_warm(s, k);
        return;
    }
    u64 submit = vk_submit_number();
    /* this submit's first draw was counted: no word read for the others */
    if (s->fold_stable.same && s->fold_stable.submit == submit) return;
    if (atomic_load_explicit(&g_pipeline.fold_off, memory_order_relaxed) || sh_fold_pending(s)) return;
    VkCbFold fold;
    const VkCbReads* others[SH_CB_FOLD_VARIANTS];
    sh_fold_plan(s->hash, s->stage, s->mask, s->clen, s->qh, s->clen_a, s->qh_a, &fold, others);
    switch (vk_cb_stable_note(&s->fold_stable, submit, vk_cb_fold_words(&s->cbr, &fold))) {
        case VK_CB_STABLE_NONE: s->fold_asked = 1; break; /* none would fold more: as it is, until its words change */
        case VK_CB_STABLE_NOW:
            s->fold_stable = (VkCbStable){0};
            sh_fold_queue(s, gva, &fold);
            break;
        default: break;
    }
}
void vk_shader_fold_stats(u64* made, u64* twins, u64* dropped) {
    *made = g_pipeline.sh_folds;
    *twins = g_pipeline.fold_twins;
    *dropped = g_pipeline.fold_dropped;
}
void vk_pipeline_stats(u64* pipes, u64* hits, u64* make_ms, u64* wait_ms, u64* shaders, u64* shader_ms) {
    *pipes = g_pipeline.pipes_new;
    *hits = atomic_load_explicit(&g_pipeline.pipe_hits, memory_order_relaxed);
    *make_ms = atomic_load_explicit(&g_pipeline.pipe_make_ns, memory_order_relaxed) / 1000000;
    *wait_ms = atomic_load_explicit(&g_pipeline.pipe_wait_ns, memory_order_relaxed) / 1000000;
    *shaders = g_pipeline.sh_new;
    *shader_ms = g_pipeline.sh_ns / 1000000;
}

int vk_pipeline_cache_cold(void) {
    return g_pipeline.pf.on ? atomic_load_explicit(&g_pipeline.pf.cold, memory_order_acquire) : 0;
}
u64 vk_shader_folds(void) { return g_pipeline.sh_folds; }
void vk_cb_fold_switch(int on) { atomic_store_explicit(&g_pipeline.fold_off, !on, memory_order_relaxed); }

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
VkSh* vk_shader_generated_find(u64 hash, int stage, const u32 mask[SH_KEY_WORDS]) {
    pf_merge();
    for (VkSh* s = g_pipeline.shh[hash & 1023]; s; s = s->next)
        if (s->hash == hash && s->stage == stage && !memcmp(s->mask, mask, sizeof s->mask)) return s;
    return NULL;
}
void vk_shader_generated_record(VkSh* s, const u32* spv, size_t words, const u8* source, u32 bytes) {
    pf_shader_record(s, spv, words, source, bytes, NULL);
    s->next = g_pipeline.shh[s->hash & 1023];
    g_pipeline.shh[s->hash & 1023] = s;
    g_pipeline.fold_gen++;
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
    vk_cb_reads_sort(&s->cbr); /* as vk_cb_folds_hold reads them, whatever wrote the record */
    s->nfold = vk_cb_fold_count(&s->cbr);
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
    vk_so_keep_spirv(s->mod, spv, r->nspv);
    if (s->stage == ST_TS && s->si.mesh.payload_buf) sh_keep_compute(&s->spv, &s->nspv, spv, r->nspv);
    if (!s->mod) {
        free(s->spv);
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
    for (u32 i = 0; i < g_pipeline.replay->n; i++) {
        const VkCbRead* c = &g_pipeline.replay->r[i];
        /* a load that stayed a load read nothing the record kept: the translation again leaves it one */
        if (c->idx == idx && c->off == off && c->ok != VK_CB_LOAD) {
            *out = c->v;
            return c->ok != VK_CB_UNBOUND;
        }
    }
    return 0;
}
/* a word replay_read holds, as bytes (VkCbBytes): the folds of the translation the record was made from */
static const u8* replay_bytes(u32 idx, u32 off, u32 size) {
    u32 v = 0;
    if (size != 4 || !replay_read(idx, off, &v)) return NULL;
    snx_st_le32(g_pipeline.replay_word, v);
    return g_pipeline.replay_word;
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
    spv = sh_spirv(s, window, r->hash, (int)r->stage, r->mask, r->clen, r->qh, r->clen_a, r->qh_a, xfb, replay_read,
                   &(VkCbFold){.mode = VK_CB_FOLD_VALUES, .bytes = replay_bytes}, 0, &nw);
    g_pipeline.replay = NULL;
    if (!spv || vk_spirv_unavailable(spv, nw) || !(s->mod = vk_module_spv(spv, nw))) goto fail;
    modinfo_put(s->mod, r->hash, (int)r->stage, nw);
    vk_so_keep_spirv(s->mod, spv, nw);
    if (s->stage == ST_TS && s->si.mesh.payload_buf) sh_keep_compute(&s->spv, &s->nspv, spv, (u32)nw);
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
    u32 arrays = 0, attrs = 0, in = 0, fetch = 0;
    for (int i = 0; i < 5; i++) {
        const VkSh* sh = NULL;
        if (!mods[i]) continue;
        for (const VkSh* q = g_pipeline.pf.sh; q && !sh; q = q->next)
            if (q->mod == mods[i]) sh = q;
        if (!sh) return 0;
        if (sh->indexed) arrays |= 1u << shader_vk_set(sh->stage);
        if (i == 0) in = sh->si.in_mask, fetch = sh->si.vertex_fetch;
    }
    for (u32 i = 0; i < k->vi.nattr; i++) attrs |= 1u << k->vi.attr[i].loc;
    /* with dynamic vertex input a key holds no layout, which the draws give, and a program that fetches its own
       attributes has none */
    return arrays == k->arrays && fetch == k->fetch &&
           (g_vk.vertex_input_dynamic || fetch ? !k->vi.nattr && !k->vi.nbind : attrs == in);
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
    if (vk_so_takes(&p->k, p->fs)) {
        atomic_store_explicit(&p->so, p->so_layout = 1, memory_order_relaxed);
        p->layout = playout(p->k.arrays, 0); /* a pipeline made in its place takes it too */
    } else
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
    u32 arrays = !g_spirv_translate.texture_arrays              ? 0u
                 : g_spirv_translate.tex_array == SNX_TEX_ARRAY ? 1u
                                                                : 1u + g_spirv_translate.tex_array;
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
                  (u32)g_spirv_translate.split_sets,
                  (u32)g_spirv_translate.cb_windows,
                  (u32)g_spirv_translate.tex_table,
                  (u32)g_spirv_translate.gbuf_address,
                  (u32)g_spirv_translate.vertex_fetch,
                  (u32)g_spirv_translate.stage_point_size,
                  (u32)g_spirv_translate.output_layer,
                  (u32)g_spirv_translate.geometry,
                  (u32)g_spirv_translate.float16,
                  (u32)g_spirv_translate.half_rounding,
                  (u32)g_spirv_translate.i2f_nearest,
                  (u32)g_spirv_translate.geometry_streams,
                  (u32)g_spirv_translate.geometry_streams_lines_triangles,
                  (u32)g_spirv_translate.tessellation,
                  (u32)g_spirv_translate.xfb_stores,
                  (u32)g_vk.has_xfb,
                  (u32)g_spirv_translate.clock_scopes,
                  (u32)g_vk_flush_denorm,
                  (u32)g_vk_preserve_infnan,
                  (u32)g_vk_preserve_infnan64,
                  (u32)g_vk_subgroup_stages,
                  (u32)g_vk.has_provoking_last,
                  (u32)g_vk.has_conservative,
                  g_vk_stage_sets,
                  g_vk_buffer_image_slots,
                  g_spirv_translate.uniform_cbufs,
                  d->limits.maxPerStageDescriptorStorageImages,
                  d->limits.maxDescriptorSetStorageImages,
                  (u32)g_vk.has_mixed_samples,
                  (u32)g_vk.has_descriptor_heap_support,
                  (u32)g_vk.has_descriptor_buffers,
                  (u32)g_spirv_translate.dynamic_texture_offsets,
                  g_vk.feat13.maintenance4,
                  (u32)g_vk.has_depth_range_unrestricted,
                  /* how a fragment program reads its fragment's size (S2R 0x84) */
                  (u32)g_spirv_translate.shading_rate,
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
            " %s%s driver=%u api=%u spirv=%x uuid=%s ftz=%d quad=%d,%d mixed_dot=%d hmma_coop=%d hmma_shapes=%u "
            "hmma_pairs=%d hmma_stage=%d half_flush=%d hmma_f16=%d hmma_astat=%d sizes=%u,%u vertex_bounds=%d "
            "robust2=%d%d%d",
            snx_build_info(), exe, d->driverVersion, d->apiVersion, (unsigned)spv_emit_version(), uuid, g_vk_ftz,
            g_spirv_translate.quad, g_spirv_translate.quad_compute, g_spirv_translate.mixed_dot,
            g_spirv_translate.hmma_coop, g_spirv_translate.hmma_coop_shapes, g_spirv_translate.hmma_pairs,
            g_spirv_translate.hmma_stage, g_spirv_translate.half_flush, g_spirv_translate.hmma_f16,
            g_spirv_translate.hmma_astat, (unsigned)sizeof(ShaderInfo), (unsigned)sizeof(PfShader), g_vk.has_vertex_robustness2,
            g_vk.robust_buffer2, g_vk.robust_image2, g_vk.null_descriptor);
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
    atomic_fetch_add_explicit(&g_pipeline.pf.ship_used, 1, memory_order_relaxed);
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
static int ship_key_only_clip(const u32 mask[SH_KEY_WORDS]) {
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
static VkSh* pf_ship_take(u64 h, int stage, const u32 mask[SH_KEY_WORDS], u32 len, u64 qh, u32 len_a, u64 qh_a) {
    const ShipIdx* e;
    const u32 *spv, *cbr;
    PfShader r;
    u64 check = 0;
    u8* owned;
    VkSh* s;
    if (!g_pipeline.pf.on || !atomic_load_explicit(&g_pipeline.pf.ship_ready, memory_order_acquire) ||
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
    if (!vk_cb_folds_hold(&s->cbr, sh_cb_read, sh_cb_bytes)) {
        vk_so_forget_spirv(s->mod); /* a later module may get its handle */
        SNX_MEM_FREE_D((const void*)(uintptr_t)s->mod, "shader modules");
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
/* the same constants as bytes (VkCbBytes), for the words the translation folds */
static const u8* ship_bytes(u32 idx, u32 off, u32 size) {
    if (idx != 1 || g_pipeline.ship_nconsts < size || off > g_pipeline.ship_nconsts - size) return NULL;
    return g_pipeline.ship_consts + off;
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
    /* a part is a library, which a device without VK_EXT_graphics_pipeline_library never promised: there every draw
       compiles whole, and a part made for it is a create the driver was not given (MoltenVK reads the null
       rasterization state of a fragment part and ends its pipeline worker) */
    if (!g_vk.has_gpl) return;
    /* draws with shader objects make no parts: their objects are made for their draws */
    if (g_vk.has_shader_objects) return;
    /* a control program shares the pre-rasterization part with the vertex program, so it is warmed with its draw */
    if (stage != ST_VS && stage != ST_FS) return;
    VkShaderModule mod = vk_module_spv(spv, nw);
    if (!mod) return;
    WarmItem* w = calloc(1, sizeof *w);
    if (!w) {
        SNX_MEM_FREE_D((const void*)(uintptr_t)mod, "shader modules");
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
        SNX_MEM_FREE_D((const void*)(uintptr_t)mod, "shader modules");
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
    atomic_fetch_add_explicit(&g_pipeline.plib.warm_queued, 1, memory_order_relaxed);
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
    u32 mask[SH_KEY_WORDS] = {0};
    VkSh s = {0};
    size_t nw;
    u64 at = 0;
    const ShipIdx* e;
    mask[6] = ship_clip(stage);
    e = idx_slot(h, stage, mask[6], len, qh, len_a, qh_a);
    if (e && e->at) return SHIP_KNOWN;
    /* of the words a draw will hold, the pass knows the program's own constants */
    u32* spv = sh_spirv(&s, code, h, stage, mask, len, qh, len_a, qh_a, NULL, ship_read,
                        &(VkCbFold){.mode = VK_CB_FOLD_VALUES, .bytes = ship_bytes}, 1, &nw);
    int translated = spv != NULL;
    if (spv) at = pf_shader_into(&g_pipeline.pf.ship, &s, 0, spv, nw, NULL, 0, NULL);
    if (translated && nw >= SHIP_WARM_WORDS) ship_warm(stage, s.indexed, spv, nw);
    free(spv);
    free(s.cbr.r);
    /* An allocation failing inside the translator comes back as a program it can't translate, with no reason given,
       and is counted as failed, for this build and driver. Taking every failure without a reason as one to try again
       would walk the files again on every run of a game with a program that fails so */
    if (!translated) {
        LOG("vulkan: %s %016llx not translated: %s%s\n", shipped_stage_name(stage), (unsigned long long)h,
            s.si.first_unsupported[0] ? s.si.first_unsupported : "unsupported feature or float controls",
            vk_lane_note((char[32]){0}, &s.si));
        return SHIP_FAILED;
    }
    if (!at) return SHIP_STOP;
    idx_add(h, stage, mask[6], len, qh, len_a, qh_a, at);
    atomic_fetch_add_explicit(&g_pipeline.pf.ship_new, 1, memory_order_relaxed);
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
        atomic_store_explicit(&g_pipeline.pf.scanning, 1, memory_order_relaxed);
        int rc = ship_walk(&sink, &g_pipeline.pf.progress, g_settings.prepare_shaders == 2, g_ounce);
        atomic_store_explicit(&g_pipeline.pf.scanning, 0, memory_order_relaxed);
        pf_close(&g_pipeline.pf.scan);
        LOG("vulkan: the game's own shaders: %u translated new, %u in %s, in %llu ms%s; %llu large ones queued to warm "
            "their parts\n",
            atomic_load_explicit(&g_pipeline.pf.ship_new, memory_order_relaxed), g_pipeline.pf.nidx,
            g_pipeline.pf.ship_path, (unsigned long long)((plat_ns() - t0) / 1000000),
            rc > 0   ? " (stopped: the file takes no more)"
            : rc < 0 ? " (a later run reads what wasn't)"
                     : "",
            (unsigned long long)atomic_load_explicit(&g_pipeline.plib.warm_queued, memory_order_relaxed));
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
    p->files = atomic_load_explicit(&g_pipeline.pf.progress.files, memory_order_relaxed);
    p->files_seen = atomic_load_explicit(&g_pipeline.pf.progress.files_seen, memory_order_relaxed);
    p->programs = atomic_load_explicit(&g_pipeline.pf.progress.programs, memory_order_relaxed);
    p->programs_done = atomic_load_explicit(&g_pipeline.pf.progress.programs_done, memory_order_relaxed);
    p->shipped = atomic_load_explicit(&g_pipeline.pf.ship_new, memory_order_relaxed);
    p->scanning = atomic_load_explicit(&g_pipeline.pf.scanning, memory_order_relaxed);
    p->shaders = atomic_load_explicit(&g_pipeline.pf.shaders_total, memory_order_relaxed);
    p->pipes = atomic_load_explicit(&g_pipeline.pf.pipes_total, memory_order_relaxed);
    p->pipes_made = (u32)atomic_load_explicit(&g_pipeline.pf.made, memory_order_relaxed);
    p->loaded = loaded;
    p->done =
        loaded && atomic_load_explicit(&g_pipeline.pf.ship_ready, memory_order_acquire) && p->pipes_made >= p->pipes;
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
void vk_pipeline_file_save(void) { pf_save(&g_pipeline.pf.file); }
void vk_pipeline_file_saving(u64* total, u64* packed, u64* kept) {
    pf_saving(&g_pipeline.pf.file, total, packed, kept);
}
void vk_pipeline_file_save_stop(void) { pf_save_stop(&g_pipeline.pf.file); }
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
    g_pipeline.pf.file.pack = PF_PACK_UNITS;
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
    atomic_store_explicit(&g_pipeline.pf.shaders_total, g_pipeline.pf.nsh, memory_order_relaxed);
    atomic_store_explicit(&g_pipeline.pf.pipes_total, g_pipeline.pf.npipe, memory_order_relaxed);
    atomic_store_explicit(&g_pipeline.pf.cold, !g_pipeline.pf.nsh && !g_pipeline.pf.npipe, memory_order_release);
    pm_lock(&g_pipeline.pf.lock);
    g_pipeline.pf.loaded = 1;
    pc_broadcast(&g_pipeline.pf.done);
    pm_unlock(&g_pipeline.pf.lock);
    if (arg && g_settings.prepare_shaders) pf_ship_pass(); /* translating needs this thread's stack */
    ship_release();
    atomic_store_explicit(&g_pipeline.pf.ship_ready, 1,
                          memory_order_release); /* the index and the view no longer change */
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
    pm_lock_announce(&g_pipeline.pf.lock, "pipeline files"); /* the load thread and the game threads reading files */
    pc_init(&g_pipeline.pf.done);
    if (vk_corpus_dir()) {
        LOG("vulkan: shaders are recorded (shader_corpus or shader_captures), so every one is translated and none is "
            "kept for the next run\n");
        return;
    }
    atomic_store_explicit(&g_pipeline.pf.cold, -1, memory_order_relaxed); /* before a present can ask (pf_load) */
    g_pipeline.pf.on = 1;
    vk_so_open(g_pipeline.cache_dir, g_pipeline.driver_tag); /* the key of the shader file is settled now */
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
        g_pipeline.fold_gen++;
    }
    g_pipeline.pf.sh_tail = NULL;
    for (u32 b = 0; b < PIPEH; b++)
        while ((p = g_pipeline.pf.pipes[b]) != NULL) {
            g_pipeline.pf.pipes[b] = p->next;
            p->next = g_pipeline.pipeh[b];
            g_pipeline.pipeh[b] = p;
        }
}
void vk_pipeline_fast_stats(u64* fast, u64* early_fast, u64* so) {
    *so = atomic_load_explicit(&g_pipeline.pipe_so, memory_order_relaxed);
    *fast = atomic_load_explicit(&g_pipeline.pipe_fast, memory_order_relaxed);
    *early_fast = atomic_load_explicit(&g_pipeline.pf.fast, memory_order_relaxed);
}
void vk_pipeline_prewarm_stats(u64* made, u64* hits, u64* left) {
    *made = atomic_load_explicit(&g_pipeline.pf.made, memory_order_relaxed);
    *hits = atomic_load_explicit(&g_pipeline.pf.hits, memory_order_relaxed);
    *left = g_pipeline.pf.merged && g_pipeline.pf.npipe > *made ? g_pipeline.pf.npipe - *made
                                                                : 0; /* npipe is settled once merged */
}
