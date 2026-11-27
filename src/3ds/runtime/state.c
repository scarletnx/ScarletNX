/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A 3DS game's save state, in sections each with the schema hash of what it holds:

     PROCESS  the SnxCtrProcess as it is, the pages' owners after it, but for what the running process keeps (the
              RomFS views, the filesystem tree, the memory and its space, the CPU's links to them, the services' host
              objects, the run-time compiler, the hooks) and for the times (TIME): the kernel objects and handles, each
              thread with its registers, the services' state
     TIME     the times the process keeps by the host's clock (each thread's wake and each timer's signal, the vblank,
              the audio frames, the sampling of the inputs and cameras, the hourly time entry, the tick count's
              origin), from the save's now: a load counts them from its own
     MAPS     the address space's mappings, in runs of pages with their physical address and permissions
     MEMORY   the board's RAM (FCRAM, VRAM, the DSP's), read through write tracking: what the GPU keeps newer than
              memory lands there first
     FILES    the files and archives open, by their paths in the filesystem tree, and the DLC contents opened
     LDR      the CROs loaded, and the AOT blocks a CRO's relocation made stale
     PICA     the GPU's command processor, with the command list it ran last
     DSP      the DSP (audio/dsp.c)

   The structs go as their bytes, as the Switch's g_hle does: a state whose types had other fields has another schema,
   and is refused. The game's identity is its title ID's, its segments', its name's, the model's and its memory's */
#include "3ds/runtime/state.h"
#include "3ds/runtime/internal.h"
#include "3ds/aot/ctr_module.h"
#include "3ds/board/clock.h"
#include "3ds/gpu/pica.h"
#include "3ds/gpu/vk/vk.h"
#include "3ds/gpu/vk/vk_render.h"
#include "3ds/services/dsp.h"
#include "3ds/services/frd.h"
#include "3ds/services/fs.h"
#include "3ds/services/ldr.h"
#include "3ds/services/soc.h"
#include "3ds/services/uds.h"
#include "common/filesystem/tree.h"
#include "common/platform/time.h"
#include "common/utilities/bits.h"
#include "common/utilities/build_info.h"
#include "common/utilities/common.h"
#include "common/utilities/state_header.h"
#include "common/utilities/state_file.h"
#include "common/utilities/state_io.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PAGE       4096u
#define SPACE      (1ull << 32)
#define NO_TIME    INT64_MIN /* a time the process doesn't keep (0 in it) */
#define FOREVER    INT64_MAX /* a thread asleep for good (UINT64_MAX in it) */
#define TIMES_MOST (CTR_OBJECTS + 16)
#define PATH_MOST  4096u
#define DLC_MOST   (sizeof((SnxCtrProcess*)0)->dlc_views / sizeof((SnxCtrProcess*)0)->dlc_views[0])
#define RUNS_MOST  (1u << 20)       /* a space's runs: one page each at most */
#define LDR_MOST   (1u << 16)       /* CROs loaded */
#define LIST_MOST  (16u << 20) / 4u /* the PICA's command list, in words (gpu/pica.c's LIST_LIMIT) */

/* the game: its title ID, its segments, its name, the console model and the memory its limits give it */
static uint64_t identity(const SnxCtrProcess* p) {
    uint32_t model = (uint32_t)snx_ctr_memory_profile(p->memory)->model;
    uint64_t h = snx_fnv1a(SNX_FNV_BASIS, &p->title_id, sizeof p->title_id);
    h = snx_fnv1a(h, p->segment, sizeof p->segment);
    h = snx_fnv1a(h, p->name, sizeof p->name);
    h = snx_fnv1a(h, &model, sizeof model);
    return snx_fnv1a(h, &p->app_limit, sizeof p->app_limit);
}
/* the PROCESS section's bytes: the struct and the pages' owners after it */
static size_t process_bytes(const SnxCtrProcess* p) { return sizeof *p + p->app_limit / PAGE; }

/* the schema hashes: each struct's walk and size, and the elements of the arrays of structs it holds */
static uint64_t process_schema(const SnxCtrProcess* p) {
    uint64_t h = SNX_FNV_BASIS ^ sizeof *p;
    __builtin_dump_struct(p, snx_state_schema_out, &h);
    __builtin_dump_struct(&p->cpu.state.cpu, snx_state_schema_out, &h);
    __builtin_dump_struct(&p->objects[0], snx_state_schema_out, &h);
    __builtin_dump_struct(&p->objects[0].data.thread, snx_state_schema_out, &h);
    __builtin_dump_struct(&p->objects[0].data.file, snx_state_schema_out, &h);
    __builtin_dump_struct(&p->objects[0].data.shared, snx_state_schema_out, &h);
    __builtin_dump_struct(&p->objects[0].data.timer, snx_state_schema_out, &h);
    __builtin_dump_struct(&p->handles[0], snx_state_schema_out, &h);
    __builtin_dump_struct(&p->shared_maps[0], snx_state_schema_out, &h);
    __builtin_dump_struct(&p->aliases[0], snx_state_schema_out, &h);
    __builtin_dump_struct(&p->self_archives[0], snx_state_schema_out, &h);
    __builtin_dump_struct(&p->gsp, snx_state_schema_out, &h);
    __builtin_dump_struct(&p->y2r, snx_state_schema_out, &h);
    __builtin_dump_struct(&p->cam.camera[0], snx_state_schema_out, &h);
    __builtin_dump_struct(&p->cam.port[0], snx_state_schema_out, &h);
    __builtin_dump_struct(&p->csnd, snx_state_schema_out, &h);
    __builtin_dump_struct(&p->hid, snx_state_schema_out, &h);
    return h;
}
static uint64_t pica_schema(void) {
    static const SnxCtrPica pica;
    uint64_t h = SNX_FNV_BASIS ^ sizeof pica;
    __builtin_dump_struct(&pica, snx_state_schema_out, &h);
    __builtin_dump_struct(&pica.vs, snx_state_schema_out, &h);
    __builtin_dump_struct(&pica.geometry, snx_state_schema_out, &h);
    return h;
}
static uint64_t ldr_schema(void) {
    static const SnxCtrLdrModule module;
    uint64_t h = SNX_FNV_BASIS ^ sizeof module;
    __builtin_dump_struct(&module, snx_state_schema_out, &h);
    return h;
}

/* the times the process keeps by the host's clock, in the TIME section's order: their places in p */
static size_t times(SnxCtrProcess* p, uint64_t** out) {
    size_t n = 0;
    for (unsigned i = 0; i < CTR_OBJECTS; ++i) {
        CtrObject* o = &p->objects[i];
        if (o->type == CTR_OBJ_THREAD)
            out[n++] = &o->data.thread.wake_ns;
        else if (o->type == CTR_OBJ_TIMER)
            out[n++] = &o->data.timer.deadline;
    }
    out[n++] = &p->gsp.next_vblank_ns;
    out[n++] = &p->dsp.next_frame_ns;
    out[n++] = &p->y2r.done_ns;
    out[n++] = &p->irrst.next_ns;
    for (unsigned i = 0; i < 2; ++i) out[n++] = &p->cam.port[i].vsync_ns, out[n++] = &p->cam.port[i].done_ns;
    out[n++] = &p->csnd.frame_start_ns;
    out[n++] = &p->csnd.next_frame_ns;
    out[n++] = &p->csnd.next_fade_ns;
    out[n++] = &p->hid.next_pad_ns;
    out[n++] = &p->hid.next_accelerometer_ns;
    out[n++] = &p->hid.next_gyroscope_ns;
    out[n++] = &p->time_update_ns;
    return n;
}
/* the TIME section past the deadlines: the tick count's origin and the cameras' last vsyncs, from the save's now */
typedef struct {
    double origin;          /* seconds */
    int64_t vsync_us[2][5]; /* microseconds */
} Clock;

/* what a state can't hold, which the game holds now: 0, or -1 with it in why */
static int savable(SnxCtrProcess* p, char* why, size_t whysize) {
    unsigned sockets = snx_ctr_soc_open(p);
    if (p->stopped[0])
        snprintf(why, whysize, "the game stopped: %s", p->stopped);
    else if (sockets)
        snprintf(why, whysize, "the game has %u socket%s open, which a state can't keep", sockets,
                 sockets == 1 ? "" : "s");
    else if (p->uds)
        snprintf(why, whysize, "the game has local wireless (nwm::UDS) open, which a state can't keep");
    else if (snx_ctr_frd_waiting(p))
        snprintf(why, whysize, "a login of the game's to its friends server is on its way: a state can't keep it");
    else if (p->cro_transaction)
        snprintf(why, whysize, "an ldr:ro command is half done: a state is saved between two dispatches");
    else if (p->gsp.pica && p->gsp.pica->immediate_given)
        snprintf(why, whysize,
                 "the GPU holds immediate-mode vertices it hasn't drawn yet: a state is saved between two "
                 "command lists");
    else
        return 0;
    return -1;
}

static void strip_cpu(SnxArmCPU* cpu) {
    cpu->mem = NULL;
    cpu->mem_size = 0;
    cpu->io = NULL;
    cpu->rt = NULL;
    cpu->exclusive_valid = 0;
}
/* the process as a state holds it: what the running process keeps zeroed, and the times (TIME holds them) */
static void strip(SnxCtrProcess* c) {
    c->romfs = c->update_romfs = NULL;
    c->source = NULL;
    for (size_t i = 0; i < DLC_MOST; ++i) c->dlc_views[i].view = NULL;
    c->tree = NULL;
    c->sd = c->save = NULL;
    c->memory = NULL;
    c->space = c->cpu.space = NULL;
    c->cpu.state.context = NULL;
    c->cpu.state.access = NULL;
    strip_cpu(&c->cpu.state.cpu);
    c->cpu.reservation_size = 0; /* an exclusive monitor doesn't outlive a load, as it doesn't a context switch */
    for (unsigned i = 0; i < CTR_OBJECTS; ++i) {
        CtrObject* o = &c->objects[i];
        if (o->type == CTR_OBJ_THREAD) strip_cpu(&o->data.thread.context);
        if (o->type == CTR_OBJ_FILE) o->data.file.host = o->data.file.node = NULL;
    }
    for (unsigned i = 0; i < CTR_SELF_ARCHIVES; ++i) c->self_archives[i].root = NULL;
    c->gsp.pica = NULL;
    c->ldr_modules = NULL;
    c->cro_transaction = NULL;
    c->aot_stale = NULL;
    c->dsp.core = NULL;
    c->uds = NULL;
    c->soc = NULL;
    c->frd = NULL;
    c->compile = NULL;
    c->compiler = NULL;
    c->flush = c->reclaim = NULL;
    c->hooks = NULL;
    c->ldr_capacity = 0; /* the running process's allocation's, not the game's */
    uint64_t* fields[TIMES_MOST];
    for (size_t i = 0, n = times(c, fields); i < n; ++i) *fields[i] = 0;
    c->clock_origin = 0;
    memset(c->cam.port[0].vsync_us, 0, sizeof c->cam.port[0].vsync_us);
    memset(c->cam.port[1].vsync_us, 0, sizeof c->cam.port[1].vsync_us);
}

/* node's path from the tree's root, as fs_tree_lookup takes it: 0, or -1 when it is longer than size or out of the
   tree (a file deleted while open). The tree's lock is held */
static int tree_path(const FsTree* tree, const FsNode* node, char* out, size_t size) {
    size_t at = size - 1;
    out[at] = 0;
    for (const FsNode* n = node; n != tree->root; n = n->parent) {
        if (!n || !n->parent || !n->name) return -1;
        size_t length = strlen(n->name);
        if (length + 1 > at) return -1;
        at -= length;
        memcpy(out + at, n->name, length);
        if (n->parent != tree->root) out[--at] = '/';
    }
    memmove(out, out + at, size - at);
    return 0;
}
/* what FILES keeps of a slot: an object's index, or CTR_OBJECTS + an archive's */
typedef struct {
    uint32_t slot, host, length;
} FileEntry;
static FsNode* slot_node(const SnxCtrProcess* p, uint32_t slot) {
    return slot < CTR_OBJECTS ? (FsNode*)p->objects[slot].data.file.node : p->self_archives[slot - CTR_OBJECTS].root;
}
static bool slot_open(const SnxCtrProcess* p, uint32_t slot) {
    if (slot >= CTR_OBJECTS) return p->self_archives[slot - CTR_OBJECTS].root != NULL;
    const CtrObject* o = &p->objects[slot];
    return o->type == CTR_OBJ_FILE && o->data.file.node;
}
/* FILES written: each slot with a node, its path; then the DLC contents opened. 0, or -1 with why */
static int files_save(SnxCtrProcess* p, StateIo* io, char* why, size_t whysize) {
    char* path = malloc(PATH_MOST);
    uint32_t count = 0;
    for (uint32_t slot = 0; slot < CTR_OBJECTS + CTR_SELF_ARCHIVES; ++slot) count += slot_open(p, slot);
    int failed = !path || io_put32(io, count);
    if (count) pm_lock(&p->tree->lock);
    for (uint32_t slot = 0; !failed && slot < CTR_OBJECTS + CTR_SELF_ARCHIVES; ++slot) {
        if (!slot_open(p, slot)) continue;
        FsNode *node = slot_node(p, slot), *found = NULL;
        if (tree_path(p->tree, node, path, PATH_MOST) || fs_tree_lookup(p->tree, NULL, path, &found) || found != node) {
            snprintf(why, whysize, "the game has a file or folder open (%s) that was deleted or moved since",
                     node->name ? node->name : "?");
            failed = -1;
            break;
        }
        FileEntry e = {slot, slot < CTR_OBJECTS && p->objects[slot].data.file.host, (uint32_t)strlen(path)};
        failed = io_put(io, &e, sizeof e) || io_put(io, path, e.length);
    }
    if (count) pm_unlock(&p->tree->lock);
    failed = failed || io_put32(io, p->dlc_view_count);
    for (unsigned i = 0; !failed && i < p->dlc_view_count; ++i)
        failed = io_put64(io, p->dlc_views[i].title) || io_put32(io, p->dlc_views[i].index);
    free(path);
    return failed ? -1 : 0;
}
/* what a load opens before anything changes: each slot's node and host file, and the running process's view of each
   DLC content the state's had open (1 + its dlc_views entry) */
typedef struct {
    FsNode* node[CTR_OBJECTS + CTR_SELF_ARCHIVES];
    FILE* host[CTR_OBJECTS];
    uint32_t dlc[DLC_MOST], dlc_count;
} Files;
static void files_drop(Files* f) {
    if (!f) return;
    for (unsigned i = 0; i < CTR_OBJECTS; ++i)
        if (f->host[i]) fclose(f->host[i]);
    for (unsigned i = 0; i < CTR_OBJECTS + CTR_SELF_ARCHIVES; ++i)
        if (f->node[i]) fs_tree_release(f->node[i]);
    free(f);
}
/* FILES read and opened against the running process's tree, for the state's process c: the files, or NULL with why */
static Files* files_open(SnxCtrProcess* p, const SnxCtrProcess* c, StateIo* io, char* why, size_t whysize) {
    Files* f = calloc(1, sizeof *f);
    char* path = malloc(PATH_MOST);
    uint32_t count = 0;
    bool failed = !f || !path || io_get32(io, &count) || count > CTR_OBJECTS + CTR_SELF_ARCHIVES;
    if (failed) snprintf(why, whysize, "its FILES section is damaged or cut short");
    if (!failed && count && !p->tree) {
        snprintf(why, whysize, "the state's game had %u files open, and this run has no filesystem mounted", count);
        failed = true;
    }
    for (uint32_t i = 0; !failed && i < count; ++i) {
        FileEntry e;
        if (io_get(io, &e, sizeof e) || e.slot >= CTR_OBJECTS + CTR_SELF_ARCHIVES || e.length >= PATH_MOST ||
            io_get(io, path, e.length) || f->node[e.slot] ||
            (e.slot < CTR_OBJECTS && c->objects[e.slot].type != CTR_OBJ_FILE) || (e.host && e.slot >= CTR_OBJECTS)) {
            snprintf(why, whysize, "its FILES section is damaged or cut short");
            failed = true;
            break;
        }
        path[e.length] = 0;
        FsNode* node = NULL;
        FILE* host = NULL;
        pm_lock(&p->tree->lock);
        FsTreeResult rc = fs_tree_lookup(p->tree, NULL, path, &node);
        if (!rc) {
            fs_tree_retain(node);
            f->node[e.slot] = node; /* released by files_drop when the load is refused */
            if (e.host && !(host = fs_tree_open(p->tree, node, c->objects[e.slot].data.file.writable ? "r+b" : "rb")))
                rc = FS_TREE_IO;
            f->host[e.slot] = host;
        }
        pm_unlock(&p->tree->lock);
        if (rc) {
            snprintf(why, whysize, "the game had a file open that can't be opened again: %s %s", path,
                     fs_tree_why(p->tree, rc));
            failed = true;
        }
    }
    if (!failed && (io_get32(io, &f->dlc_count) || f->dlc_count > DLC_MOST)) {
        snprintf(why, whysize, "its FILES section is damaged or cut short");
        failed = true;
    }
    for (uint32_t i = 0; !failed && i < f->dlc_count; ++i) {
        uint64_t title = 0;
        uint32_t index = 0;
        if (io_get64(io, &title) || io_get32(io, &index)) {
            snprintf(why, whysize, "its FILES section is damaged or cut short");
            failed = true;
        } else if (!(f->dlc[i] = snx_ctr_fs_dlc_view(p, title, index))) {
            snprintf(why, whysize,
                     "the game had its DLC content %016" PRIX64 ":%" PRIu32 " open, which this run can't open", title,
                     index);
            failed = true;
        }
    }
    for (unsigned i = 0; !failed && i < CTR_OBJECTS; ++i) {
        const CtrObject* o = &c->objects[i];
        if (o->type == CTR_OBJ_FILE && o->data.file.kind == CTR_FILE_VIEW && o->data.file.dlc_view > f->dlc_count) {
            snprintf(why, whysize, "its FILES section is damaged: a file reads the DLC view %" PRIu32 " of %" PRIu32,
                     o->data.file.dlc_view, f->dlc_count);
            failed = true;
        }
    }
    free(path);
    if (failed) {
        files_drop(f);
        return NULL;
    }
    return f;
}
/* the running game's files and archives closed, before the state's take their places */
static void files_close(SnxCtrProcess* p) {
    for (unsigned i = 0; i < CTR_OBJECTS; ++i) {
        CtrObject* o = &p->objects[i];
        if (o->type != CTR_OBJ_FILE) continue;
        if (o->data.file.kind == CTR_FILE_HOST && o->data.file.host) fclose((FILE*)o->data.file.host);
        if (o->data.file.node) fs_tree_release((FsNode*)o->data.file.node);
        o->data.file.host = o->data.file.node = NULL;
    }
    for (unsigned i = 0; i < CTR_SELF_ARCHIVES; ++i) {
        if (p->self_archives[i].root) fs_tree_release(p->self_archives[i].root);
        p->self_archives[i].root = NULL;
    }
}
static void files_install(SnxCtrProcess* p, Files* f) {
    for (unsigned i = 0; i < CTR_OBJECTS; ++i) {
        CtrObject* o = &p->objects[i];
        if (o->type != CTR_OBJ_FILE) continue;
        o->data.file.node = f->node[i];
        o->data.file.host = f->host[i];
        if (o->data.file.kind == CTR_FILE_VIEW && o->data.file.dlc_view)
            o->data.file.dlc_view = f->dlc[o->data.file.dlc_view - 1];
        f->node[i] = NULL;
        f->host[i] = NULL;
    }
    for (unsigned i = 0; i < CTR_SELF_ARCHIVES; ++i) {
        p->self_archives[i].root = f->node[CTR_OBJECTS + i];
        f->node[CTR_OBJECTS + i] = NULL;
    }
    files_drop(f);
}

/* the space's mappings: runs of pages whose physical addresses follow on in one board region, at one permission */
typedef struct {
    uint32_t va, pa, size, permissions;
} Run;
static Run* mappings(const SnxCtrProcess* p, uint32_t* count) {
    const SnxCtrProfile* profile = snx_ctr_memory_profile(p->memory);
    Run* runs = NULL;
    size_t n = 0, capacity = 0;
    for (uint64_t va = 0; va < SPACE; va += PAGE) {
        uint32_t pa;
        unsigned permissions;
        if (snx_ctr_query(p->space, (uint32_t)va, &pa, &permissions)) continue;
        Run* last = n ? &runs[n - 1] : NULL;
        if (last && (uint64_t)last->va + last->size == va && (uint64_t)last->pa + last->size == pa &&
            last->permissions == permissions && snx_ctr_physical_region(profile, last->pa, last->size + PAGE)) {
            last->size += PAGE;
            continue;
        }
        if (n == capacity) {
            capacity = capacity ? capacity * 2 : 64;
            Run* grown = realloc(runs, capacity * sizeof *runs);
            if (!grown) {
                free(runs);
                return NULL;
            }
            runs = grown;
        }
        runs[n++] = (Run){(uint32_t)va, pa, PAGE, permissions};
    }
    *count = (uint32_t)n;
    return runs ? runs : malloc(sizeof *runs);
}
/* MAPS read: the runs, in order and apart, or NULL */
static Run* runs_read(StateIo* io, uint32_t* count) {
    if (io_get32(io, count) || *count > RUNS_MOST) return NULL;
    Run* runs = malloc((*count ? *count : 1) * sizeof *runs);
    if (!runs || io_get(io, runs, (uint64_t)*count * sizeof *runs)) {
        free(runs);
        return NULL;
    }
    uint64_t end = 0;
    for (uint32_t i = 0; i < *count; ++i) {
        const Run* r = &runs[i];
        if (r->va < end || !r->size || (r->va | r->pa | r->size) % PAGE || (uint64_t)r->va + r->size > SPACE) {
            free(runs);
            return NULL;
        }
        end = (uint64_t)r->va + r->size;
    }
    return runs;
}
/* The space's mappings made the state's: the pages that differ unmapped, then the state's runs mapped where nothing
   is. 0, or -1 with why */
static int remap(SnxCtrProcess* p, const Run* runs, uint32_t n, char* why, size_t whysize) {
    uint64_t from = 0, length = 0;
    uint32_t k = 0;
    for (uint64_t va = 0; va <= SPACE; va += PAGE) {
        bool drop = false;
        uint32_t pa;
        unsigned permissions;
        if (va < SPACE && !snx_ctr_query(p->space, (uint32_t)va, &pa, &permissions)) {
            while (k < n && (uint64_t)runs[k].va + runs[k].size <= va) ++k;
            bool kept = k < n && runs[k].va <= va && runs[k].pa + (uint32_t)(va - runs[k].va) == pa &&
                        runs[k].permissions == permissions;
            drop = !kept;
        }
        if (drop && length && from + length == va && length < (1ull << 31)) {
            length += PAGE;
            continue;
        }
        if (length && snx_ctr_unmap(p->space, (uint32_t)from, (uint32_t)length)) {
            snprintf(why, whysize, "the running game's pages at 0x%08" PRIx64 " (%" PRIu64 " bytes) don't unmap", from,
                     length);
            return -1;
        }
        from = va;
        length = drop ? PAGE : 0;
    }
    for (uint32_t i = 0; i < n; ++i) {
        const Run* r = &runs[i];
        uint32_t start = 0, size = 0;
        for (uint64_t offset = 0; offset <= r->size; offset += PAGE) {
            if (offset < r->size &&
                snx_ctr_query(p->space, r->va + (uint32_t)offset, NULL, NULL) == SNX_CTR_MEM_UNMAPPED) {
                if (!size) start = (uint32_t)offset;
                size += PAGE;
                continue;
            }
            SnxCtrMemResult result =
                size ? snx_ctr_map(p->space, r->va + start, r->pa + start, size, r->permissions) : SNX_CTR_MEM_OK;
            if (result) {
                snprintf(why, whysize,
                         "the state's pages at 0x%08" PRIx32 " (0x%08" PRIx32 ", %" PRIu32 " bytes) don't map (%d)",
                         r->va + start, r->pa + start, size, (int)result);
                return -1;
            }
            size = 0;
        }
    }
    return 0;
}

/* the board's RAM regions, each with its host memory, one after another */
static uint64_t ram_bytes(const SnxCtrMemory* memory) {
    const SnxCtrProfile* profile = snx_ctr_memory_profile(memory);
    uint64_t total = 0;
    for (size_t i = 0; i < profile->region_count; ++i)
        if (snx_ctr_memory_region(memory, i)) total += profile->regions[i].size;
    return total;
}
enum { CHUNK = 1u << 20 };
/* MEMORY written, each region read through write tracking: 0, or -1 */
static int memory_save(SnxCtrMemory* memory, StateWriter* w) {
    const SnxCtrProfile* profile = snx_ctr_memory_profile(memory);
    unsigned char* buffer = malloc(CHUNK);
    int failed = !buffer;
    for (size_t i = 0; !failed && i < profile->region_count; ++i) {
        const SnxCtrRegion* r = &profile->regions[i];
        if (!snx_ctr_memory_region(memory, i)) continue;
        for (uint32_t done = 0; !failed && done < r->size;) {
            uint32_t k = r->size - done < CHUNK ? r->size - done : CHUNK;
            failed = snx_ctr_phys_read(memory, r->base + done, buffer, k) != SNX_CTR_MEM_OK || sw_put(w, buffer, k);
            done += k;
        }
    }
    free(buffer);
    return failed ? -1 : 0;
}
/* the MEMORY section read through, its checksums checked, and written into the board's RAM through write tracking
   when into (the renderer then drops what it kept of each page): 0, or -1 */
static int memory_read(StateReader* r, SnxCtrMemory* memory, bool into) {
    const SnxCtrProfile* profile = snx_ctr_memory_profile(memory);
    unsigned char* buffer = malloc(CHUNK);
    if (!buffer || sr_begin(r, "MEMORY", NULL) || sr_left(r) != ram_bytes(memory)) {
        free(buffer);
        return -1;
    }
    int failed = 0;
    for (size_t i = 0; !failed && i < profile->region_count; ++i) {
        const SnxCtrRegion* region = &profile->regions[i];
        if (!snx_ctr_memory_region(memory, i)) continue;
        for (uint32_t done = 0; !failed && done < region->size;) {
            uint32_t k = region->size - done < CHUNK ? region->size - done : CHUNK;
            failed = sr_get(r, buffer, k) ||
                     (into && snx_ctr_phys_write(memory, region->base + done, buffer, k) != SNX_CTR_MEM_OK);
            done += k;
        }
    }
    free(buffer);
    return failed ? -1 : 0;
}

/* the AOT stale bit map's words, for the static text's size (aot/dispatch.c's windows of 128 bytes) */
static size_t stale_words(const SnxCtrProcess* p) { return ((((size_t)p->segment[0].bytes + 127) >> 7) + 31) / 32; }
/* the CROs and the stale map as a load stages them */
typedef struct {
    SnxCtrLdrModule* modules;
    size_t count;
    uint32_t* stale;
} Ldr;
static int ldr_save(const SnxCtrProcess* p, StateIo* io) {
    return io_put64(io, p->ldr_count) || io_put(io, p->ldr_modules, p->ldr_count * sizeof *p->ldr_modules) ||
           io_put32(io, p->aot_stale != NULL) ||
           io_put(io, p->aot_stale, p->aot_stale ? stale_words(p) * sizeof *p->aot_stale : 0);
}
static int ldr_read(const SnxCtrProcess* p, StateIo* io, Ldr* ldr) {
    uint64_t count = 0;
    uint32_t stale = 0;
    *ldr = (Ldr){0};
    if (io_get64(io, &count) || count > LDR_MOST) return -1;
    ldr->count = (size_t)count;
    if (count && (!(ldr->modules = malloc(ldr->count * sizeof *ldr->modules)) ||
                  io_get(io, ldr->modules, ldr->count * sizeof *ldr->modules)))
        return -1;
    if (io_get32(io, &stale)) return -1;
    if (stale && (!(ldr->stale = malloc(stale_words(p) * sizeof *ldr->stale)) ||
                  io_get(io, ldr->stale, stale_words(p) * sizeof *ldr->stale)))
        return -1;
    return 0;
}

/* the PICA, present or not, its bytes with the list and the reason it stopped out, then the list's words */
static int pica_save(const SnxCtrPica* pica, StateIo* io) {
    if (io_put32(io, pica != NULL) || !pica) return pica ? -1 : 0;
    SnxCtrPica copy;
    memcpy(&copy, pica, sizeof copy);
    copy.list = NULL;
    copy.stop_reason = NULL;
    return io_put(io, &copy, sizeof copy) || io_put32(io, pica->list != NULL) ||
           io_put(io, pica->list, pica->list ? (uint64_t)pica->list_words * 4 : 0);
}
/* the PICA read into one of its own: 0 with it (NULL for none), or -1 */
static int pica_read(StateIo* io, SnxCtrPica** out) {
    uint32_t present = 0, list = 0;
    *out = NULL;
    if (io_get32(io, &present)) return -1;
    if (!present) return 0;
    SnxCtrPica* pica = malloc(sizeof *pica);
    if (!pica || io_get(io, pica, sizeof *pica)) {
        free(pica);
        return -1;
    }
    pica->list = NULL;
    pica->stop_reason = NULL;
    if (io_get32(io, &list) || pica->list_words > LIST_MOST || pica->list_index > pica->list_words ||
        (list && (!(pica->list = malloc(pica->list_words ? (size_t)pica->list_words * 4 : 4)) ||
                  io_get(io, pica->list, (uint64_t)pica->list_words * 4)))) {
        free(pica->list);
        free(pica);
        return -1;
    }
    *out = pica;
    return 0;
}
static void pica_free(SnxCtrPica* pica) {
    if (pica) snx_ctr_pica_free(pica);
    free(pica);
}

static int writer_put(void* w, const void* p, u64 n) { return sw_put((StateWriter*)w, p, n); }
static int reader_get(void* r, void* p, u64 n) { return sr_get((StateReader*)r, p, n); }

int snx_ctr_state_save(SnxCtrProcess* p, const char* path, const uint8_t* thumb, char* why, size_t whysize) {
    uint64_t t0 = plat_ns();
    why[0] = 0;
    if (savable(p, why, whysize)) return -1;
    SnxStateHeader h = {0};
    h.format = SNX_STATE_FORMAT;
    h.abi = SNX_CTR_STATE_VERSION;
    h.title_id = identity(p);
    h.created = (uint64_t)time(NULL);
    h.guest_ns = (uint64_t)((snx_ctr_clock_seconds() - p->clock_origin) * 1e9);
    h.thumb_w = thumb ? SNX_STATE_THUMB_W : 0;
    h.thumb_h = thumb ? SNX_STATE_THUMB_H : 0;
    snprintf(h.runtime, sizeof h.runtime, "%s", snx_build_info());
    snprintf(h.game, sizeof h.game, "%016" PRIX64, p->title_id);
    char* folder = snx_dirname(path);
    if (folder && folder[0] && snx_mkdirs(folder)) {
        char os[160];
        snprintf(why, whysize, "its folder %s can't be made: %s", folder, snx_os_error(os, sizeof os));
        free(folder);
        return -1;
    }
    free(folder);
    size_t bytes = process_bytes(p);
    SnxCtrProcess* c = malloc(bytes);
    uint32_t nruns = 0;
    Run* runs = mappings(p, &nruns);
    if (!c || !runs) {
        snprintf(why, whysize, "out of memory for a copy of the process (%zu bytes)", bytes);
        free(c);
        free(runs);
        return -1;
    }
    StateWriter* w = sw_open(path, &h, thumb, why, whysize);
    StateIo io = {w, writer_put, NULL};
    int r = -1;
    if (w) {
        /* the times from one now, the deadlines as offsets from it */
        double now_s = snx_ctr_clock_seconds();
        uint64_t now = (uint64_t)(now_s * 1e9);
        uint64_t* fields[TIMES_MOST];
        int64_t offsets[TIMES_MOST];
        size_t nt = times(p, fields);
        for (size_t i = 0; i < nt; ++i)
            offsets[i] = !*fields[i] ? NO_TIME : *fields[i] == UINT64_MAX ? FOREVER : (int64_t)(*fields[i] - now);
        Clock clock = {.origin = p->clock_origin - now_s};
        for (unsigned i = 0; i < 2; ++i)
            for (unsigned j = 0; j < p->cam.port[i].vsync_count && j < 5; ++j)
                clock.vsync_us[i][j] = p->cam.port[i].vsync_us[j] - (int64_t)(now / 1000);
        memcpy(c, p, bytes); /* its padding too, which goes in the file */
        strip(c);
        int failed = sw_begin(w, "PROCESS", process_schema(p)) || sw_put(w, c, bytes) || sw_end(w) ||
                     sw_begin(w, "TIME", 1) || io_put32(&io, (uint32_t)nt) ||
                     sw_put(w, offsets, nt * sizeof *offsets) || sw_put(w, &clock, sizeof clock) || sw_end(w) ||
                     sw_begin(w, "MAPS", 1) || io_put32(&io, nruns) || sw_put(w, runs, nruns * sizeof *runs) ||
                     sw_end(w) || sw_begin(w, "MEMORY", 1) || memory_save(p->memory, w) || sw_end(w) ||
                     sw_begin(w, "FILES", 1) || files_save(p, &io, why, whysize) || sw_end(w) ||
                     sw_begin(w, "LDR", ldr_schema()) || ldr_save(p, &io) || sw_end(w) ||
                     sw_begin(w, "PICA", pica_schema()) || pica_save(p->gsp.pica, &io) || sw_end(w) ||
                     sw_begin(w, "DSP", snx_ctr_dsp_state_schema()) || io_put32(&io, p->dsp.core != NULL) ||
                     (p->dsp.core && snx_ctr_dsp_state_save(p->dsp.core, &io)) || sw_end(w);
        if (!failed) {
            r = sw_finish(w, why, whysize);
            w = NULL;
        }
    }
    if (w) {
        sw_abort(w);
        if (!why[0]) snprintf(why, whysize, "the file couldn't be written");
    }
    free(c);
    free(runs);
    if (!r) printf("Saved the state %s in %.1f ms\n", path, (double)(plat_ns() - t0) / 1e6);
    return r;
}

/* the section tagged tag begun, with the schema this runtime writes: 0, or -1 with why */
static int begin(StateReader* r, const char* tag, uint64_t schema, char* why, size_t whysize) {
    uint64_t had = 0;
    if (sr_begin(r, tag, &had)) {
        snprintf(why, whysize, "it has no %s section", tag);
        return -1;
    }
    if (had != schema) {
        snprintf(why, whysize,
                 "it holds the 3DS's %s in another layout (schema %016" PRIX64 ", this ScarletNX's %016" PRIX64 ")",
                 tag, had, schema);
        return -1;
    }
    return 0;
}
/* a section that must have been read whole: 0, or -1 with why */
static int ended(StateReader* r, const char* tag, char* why, size_t whysize) {
    if (!sr_left(r)) return 0;
    snprintf(why, whysize, "its %s section holds %" PRIu64 " bytes past what this ScarletNX reads of it", tag,
             (uint64_t)sr_left(r));
    return -1;
}
/* what a load can't do over the running game: 0, or -1 with why */
static int loadable(SnxCtrProcess* p, char* why, size_t whysize) {
    if (snx_ctr_frd_waiting(p))
        snprintf(why, whysize,
                 "a login of the running game's to its friends server is on its way: a state is loaded "
                 "once it answered");
    else if (p->cro_transaction)
        snprintf(why, whysize, "an ldr:ro command is half done: a state is loaded between two dispatches");
    else
        return 0;
    return -1;
}
/* a load that failed past the point of no return: the game ends, with why as its run's last line */
static void end_game(SnxCtrProcess* p, const char* path, const char* fatal) {
    fprintf(stderr, "3ds: the save state %s was loaded only in part, and the game can't go on: %s\n", path, fatal);
    snprintf(p->stopped, sizeof p->stopped, "the save state %s was loaded only in part: %s", path, fatal);
    for (unsigned i = 0; i < CTR_OBJECTS; ++i)
        if (p->objects[i].type == CTR_OBJ_THREAD) p->objects[i].data.thread.dead = 1;
}

int snx_ctr_state_load(SnxCtrProcess* p, const char* path, char* why, size_t whysize) {
    uint64_t t0 = plat_ns();
    SnxStateHeader h;
    why[0] = 0;
    if (loadable(p, why, whysize)) return -1;
    StateReader* r = sr_open(path, &h, why, whysize);
    if (!r) return -1;
    StateIo io = {r, NULL, reader_get};
    size_t bytes = process_bytes(p);
    SnxCtrProcess* c = malloc(bytes);
    int64_t offsets[TIMES_MOST];
    Clock clock;
    uint32_t nt = 0, nruns = 0, dsp_present = 0;
    Run* runs = NULL;
    Files* files = NULL;
    Ldr ldr = {0};
    SnxCtrPica* pica = NULL;
    SnxCtrDsp* dsp = NULL;
    int refused = 1;
    if (!c)
        snprintf(why, whysize, "out of memory for the state's process (%zu bytes)", bytes);
    else if (h.title_id != identity(p))
        snprintf(why, whysize,
                 "it is another game's: its identity is %016" PRIX64 ", this game's (%016" PRIX64 ") %016" PRIX64,
                 h.title_id, p->title_id, identity(p));
    else if (h.abi != SNX_CTR_STATE_VERSION)
        snprintf(why, whysize, "it is a 3DS state of version %u, and this ScarletNX reads version %u", (unsigned)h.abi,
                 (unsigned)SNX_CTR_STATE_VERSION);
    else if (begin(r, "PROCESS", process_schema(p), why, whysize))
        ;
    else if (sr_left(r) != bytes)
        snprintf(why, whysize, "its PROCESS section holds %" PRIu64 " bytes, not the %zu this ScarletNX's holds",
                 (uint64_t)sr_left(r), bytes);
    else if (sr_get(r, c, bytes))
        snprintf(why, whysize, "its PROCESS section is damaged: its data or its checksum doesn't read back");
    else if (!begin(r, "TIME", 1, why, whysize)) {
        uint64_t* fields[TIMES_MOST];
        size_t want = times(c, fields);
        if (io_get32(&io, &nt) || nt != want || sr_get(r, offsets, nt * sizeof *offsets) ||
            sr_get(r, &clock, sizeof clock) || ended(r, "TIME", why, whysize))
            snprintf(why, whysize, "its TIME section is damaged or cut short: %" PRIu32 " times for %zu", nt, want);
        else if (begin(r, "MAPS", 1, why, whysize))
            ;
        else if (!(runs = runs_read(&io, &nruns)) || ended(r, "MAPS", why, whysize))
            snprintf(why, whysize, "its MAPS section is damaged or cut short");
        else if (memory_read(r, p->memory, false))
            snprintf(why, whysize,
                     "its MEMORY section is damaged or cut short: it doesn't read back as the board's %" PRIu64
                     " bytes of RAM",
                     ram_bytes(p->memory));
        else if (begin(r, "FILES", 1, why, whysize) || !(files = files_open(p, c, &io, why, whysize)) ||
                 ended(r, "FILES", why, whysize))
            ;
        else if (begin(r, "LDR", ldr_schema(), why, whysize))
            ;
        else if (ldr_read(c, &io, &ldr) || ldr.count != c->ldr_count || ended(r, "LDR", why, whysize))
            snprintf(why, whysize, "its LDR section is damaged or cut short");
        else if (begin(r, "PICA", pica_schema(), why, whysize))
            ;
        else if (pica_read(&io, &pica) || ended(r, "PICA", why, whysize))
            snprintf(why, whysize, "its PICA section is damaged or cut short");
        else if (begin(r, "DSP", snx_ctr_dsp_state_schema(), why, whysize))
            ;
        else if (io_get32(&io, &dsp_present))
            snprintf(why, whysize, "its DSP section is damaged or cut short");
        else if (dsp_present && !(dsp = snx_ctr_dsp_state_open(p, &io, why, whysize)))
            ;
        else if (!ended(r, "DSP", why, whysize))
            refused = 0;
    }
    if (refused) {
        sr_close(r);
        free(c);
        free(runs);
        files_drop(files);
        free(ldr.modules);
        free(ldr.stale);
        pica_free(pica);
        snx_ctr_dsp_free(dsp);
        if (!why[0]) snprintf(why, whysize, "it couldn't be read");
        return -1;
    }
    /* everything read and checked: past here nothing of the running game is kept, and a failure ends the game. What the
       GPU still runs is done first; the services that hold host objects let them go while the objects they hold are
       still the running game's */
    (void)snx_ctr_vk_finish();
    snx_ctr_soc_close(p);
    snx_ctr_uds_close(p);
    files_close(p);
    snx_ctr_dsp_close(p);
    pica_free(p->gsp.pica);
    p->gsp.pica = NULL;
    char fatal[512] = "";
    if (!remap(p, runs, nruns, fatal, sizeof fatal) && memory_read(r, p->memory, true))
        snprintf(fatal, sizeof fatal, "its RAM didn't read back the second time (the disk?)");
    sr_close(r);
    free(runs);
    /* the process: the state's, the running one's own parts kept */
    SnxArmCPU registers = c->cpu.state.cpu;
    const SnxArmCPU* live = &p->cpu.state.cpu;
    c->cpu = p->cpu;
    c->cpu.state.cpu = registers;
    c->cpu.state.cpu.mem = live->mem;
    c->cpu.state.cpu.mem_size = live->mem_size;
    c->cpu.state.cpu.io = live->io;
    c->cpu.state.cpu.rt = live->rt;
    c->cpu.state.cpu.exclusive_valid = 0;
    c->cpu.reservation_size = 0;
    for (unsigned i = 0; i < CTR_OBJECTS; ++i) {
        CtrThread* t = &c->objects[i].data.thread;
        if (c->objects[i].type != CTR_OBJ_THREAD) continue;
        t->context.mem = live->mem;
        t->context.mem_size = live->mem_size;
        t->context.io = live->io;
        t->context.rt = live->rt;
        t->context.exclusive_valid = 0;
    }
    c->romfs = p->romfs, c->update_romfs = p->update_romfs, c->source = p->source;
    memcpy(c->dlc_views, p->dlc_views, sizeof c->dlc_views);
    c->dlc_view_count = p->dlc_view_count;
    c->tree = p->tree, c->sd = p->sd, c->save = p->save;
    c->memory = p->memory, c->space = p->space;
    c->ldr_modules = p->ldr_modules, c->ldr_capacity = p->ldr_capacity;
    c->aot_stale = p->aot_stale;
    c->uds = p->uds, c->soc = p->soc, c->frd = p->frd;
    c->compile = p->compile, c->compiler = p->compiler, c->flush = p->flush, c->reclaim = p->reclaim;
    c->hooks = p->hooks;
    memcpy(p, c, bytes);
    free(c);
    files_install(p, files);
    /* the CROs and the stale blocks: the state's lists in the running process's allocations */
    if (ldr.count > p->ldr_capacity) {
        free(p->ldr_modules);
        p->ldr_modules = ldr.modules;
        p->ldr_capacity = ldr.count;
        ldr.modules = NULL;
    } else if (ldr.count) {
        memcpy(p->ldr_modules, ldr.modules, ldr.count * sizeof *ldr.modules);
    }
    p->ldr_count = ldr.count;
    free(ldr.modules);
    free(p->aot_stale);
    p->aot_stale = ldr.stale;
    p->gsp.pica = pica;
    p->dsp.core = dsp;
    p->y2r.serial = 0; /* the conversion's GPU work was in memory at the save */
    /* the save data as it is now: a state never takes it back */
    if (p->tree) {
        pm_lock(&p->tree->lock);
        if (fs_tree_lookup(p->tree, NULL, "savedata", &p->save) || !p->save->is_dir) p->save = NULL;
        pm_unlock(&p->tree->lock);
    }
    double now_s = snx_ctr_clock_seconds();
    uint64_t now = (uint64_t)(now_s * 1e9);
    uint64_t* fields[TIMES_MOST];
    size_t have = times(p, fields);
    for (size_t i = 0; i < have && i < nt; ++i) {
        int64_t at = (int64_t)now + offsets[i];
        *fields[i] = offsets[i] == NO_TIME ? 0 : offsets[i] == FOREVER ? UINT64_MAX : at > 0 ? (uint64_t)at : 1;
    }
    p->clock_origin = now_s + clock.origin;
    for (unsigned i = 0; i < 2; ++i)
        for (unsigned j = 0; j < p->cam.port[i].vsync_count && j < 5; ++j)
            p->cam.port[i].vsync_us[j] = (int64_t)(now / 1000) + clock.vsync_us[i][j];
    /* the code may be another's where CROs were loaded: the blocks compiled and the dispatcher's cache go */
    snx_ctr_dispatch_forget();
    if (p->flush) p->flush(p->compiler);
    snx_ctr_vk_render_forget();
    if (fatal[0]) {
        end_game(p, path, fatal);
        return 0;
    }
    printf("Loaded the state %s in %.1f ms\n", path, (double)(plat_ns() - t0) / 1e6);
    return 0;
}
