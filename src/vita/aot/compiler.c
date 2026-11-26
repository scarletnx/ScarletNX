/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The offline compiler, part of the driver: the image's blocks to a native module, as the 3DS driver's is
   (3ds/aot/compiler.c). The blocks are found by following the code from what is known to be code: module_start, the
   code pointers the relocations wrote, and the hints of earlier runs. The host's native back end makes each block as it
   is found, which says where it ends and so where the next ones are (cpu/arm/successors.h); a block it refuses, or every
   block with no native back end on the host or with its IR kept, the fallback (recompiler/fallback.h) learns the same
   of, and makes in chunks, in parallel */
#include "vita/aot/module.h"
#include "common/cpu/arm/aot.h"
#include "common/cpu/arm/successors.h"
#include "common/cpu/recompiler/fallback.h"
#include "common/cpu/recompiler/rtlib/rtlib_object.h"
#include "common/cpu/recompiler/object/link.h"
#include "common/cpu/recompiler/mach/native.h"
#include "common/cpu/recompiler/worker_threads.h"
#include "common/cpu/recompiler/x64/x64_elf.h"
#include "common/utilities/common.h"
#include "common/platform/file.h"
#include "common/utilities/endian.h"
#include "snx_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK_BLOCKS 512u
/* the most blocks one block goes on to, a branch's target and the code after it */
#define SUCCESSORS   2u

typedef struct {
    uint32_t key, units;
    uint32_t next[SUCCESSORS]; /* the keys it may go on to */
    uint32_t next_count;
    bool native; /* the native back end made it, else the fallback makes it in a chunk */
} Block;
/* an import's stub, whose svc block calls the import directly (stub_block, the native back end's) */
typedef struct Stub {
    uint32_t key, import;
} Stub;
typedef struct {
    const VitaImage* image;
    const Stub* stubs; /* the imports' stubs, by key */
    size_t stub_count;
    Block* blocks;
    size_t count, capacity;
    uint32_t* set; /* every key ever queued: open addressing, 0 empty (no code is at address 0) */
    size_t set_count, set_capacity;
    uint32_t* work;
    size_t work_count, work_capacity;
    bool failed;
} Discovery;

/* the block's function: the native back end's own names (mach_arm.inc's name_of), which its blocks go on to */
static void block_name(char out[24], uint32_t key) {
    snprintf(out, 24, "%c_%08x", key & 1 ? 'T' : 'A', (unsigned)(key & ~1u));
}
/* the code from address to the end of its executable segment, and how many units of it (at most the block limit) */
static const unsigned char* code_at(const VitaImage* image, uint32_t address, int thumb, size_t* units) {
    const VitaSegment* s = vita_image_segment(image, address, thumb ? 2 : 4);
    *units = 0;
    if (!s || !(s->flags & 1)) return NULL;
    *units = (s->address + s->size - address) / (thumb ? 2 : 4);
    if (*units > VITA_BLOCK_LIMIT) *units = VITA_BLOCK_LIMIT;
    return s->bytes + (address - s->address);
}
static bool insert(Discovery* d, uint32_t key) {
    if ((d->set_count + 1) * 2 > d->set_capacity) {
        size_t capacity = d->set_capacity ? d->set_capacity * 2 : 4096;
        uint32_t* set = calloc(capacity, sizeof *set);
        if (!set) {
            d->failed = true;
            return false;
        }
        for (size_t j = 0; j < d->set_capacity; ++j)
            if (d->set[j]) {
                size_t i = (size_t)(d->set[j] * 0x9E3779B1u) & (capacity - 1);
                while (set[i]) i = (i + 1) & (capacity - 1);
                set[i] = d->set[j];
            }
        free(d->set);
        d->set = set, d->set_capacity = capacity;
    }
    size_t i = (size_t)(key * 0x9E3779B1u) & (d->set_capacity - 1);
    while (d->set[i]) {
        if (d->set[i] == key) return false;
        i = (i + 1) & (d->set_capacity - 1);
    }
    d->set[i] = key;
    ++d->set_count;
    return true;
}
static void queue(void* context, uint32_t address, int thumb) {
    Discovery* d = context;
    size_t units;
    if (address & (thumb ? 1u : 3u) || !code_at(d->image, address, thumb, &units) || !units) return;
    uint32_t key = address | (thumb ? 1u : 0u);
    if (!insert(d, key)) return;
    if (d->work_count == d->work_capacity) {
        size_t capacity = d->work_capacity ? d->work_capacity * 2 : 1024;
        uint32_t* work = realloc(d->work, capacity * sizeof *work);
        if (!work) {
            d->failed = true;
            return;
        }
        d->work = work, d->work_capacity = capacity;
    }
    d->work[d->work_count++] = key;
}
static void queue_pointer(Discovery* d, uint32_t pointer) { queue(d, pointer & ~1u, (int)(pointer & 1)); }
/* the block compiler's input: the guest's little-endian code as host integers, the caller's to free */
static void* units_of(const unsigned char* code, size_t count, int thumb) {
    if (thumb) {
        uint16_t* h = malloc(count * sizeof *h);
        for (size_t i = 0; h && i < count; ++i) h[i] = snx_ld_le16(code + i * 2);
        return h;
    }
    uint32_t* w = malloc(count * sizeof *w);
    for (size_t i = 0; w && i < count; ++i) w[i] = snx_ld_le32(code + i * 4);
    return w;
}
/* The block at key of count units, by the fallback, which why left it to: into its object as name, or with no name its
   length alone. 0 with *consumed; -1 with the fallback's reason in error, -2 for code the image lacks or no memory */
static int emit(SnxFallbackArm* f, const VitaImage* image, uint32_t key, size_t count, const char* name,
                const char* why, size_t* consumed, char* error, size_t error_size) {
    size_t available;
    int thumb = (int)(key & 1);
    const unsigned char* code = code_at(image, key & ~1u, thumb, &available);
    *consumed = 0;
    if (!code || count > available) return -2;
    void* input = units_of(code, count, thumb);
    if (!input) return -2;
    /* hosted (codegen.h): the run loop is the only caller, and the guest's 4 GiB window is followed by pages that fault
       (VITA_SPACE) */
    const SnxArmCodegenOptions options = {SNX_ARM_CODEGEN_OPTIONS_ABI, SNX_ARM_PROFILE_A9, SNX_ARM_MEMORY_HOSTED, 0, 0};
    const int rc =
        name ? snx_fallback_arm_block(f, name, input, count, key & ~1u, thumb, &options, why, consumed, error,
                                      error_size)
             : snx_fallback_arm_length(f, input, count, key & ~1u, thumb, &options, why, consumed, error, error_size);
    free(input);
    return rc;
}
static int stub_order(const void* a, const void* b) {
    uint32_t x = ((const Stub*)a)->key, y = ((const Stub*)b)->key;
    return (x > y) - (x < y);
}
/* the stubs sorted by key, the ones two imports share left out. The caller frees them */
static Stub* stubs_make(const VitaImage* image, size_t* count) {
    Stub* all = malloc((image->import_count ? image->import_count : 1) * sizeof *all);
    size_t n = 0;
    *count = 0;
    if (!all) return NULL;
    for (uint32_t i = 0; i < image->import_count; ++i) all[n++] = (Stub){image->imports[i].stub, i};
    qsort(all, n, sizeof *all, stub_order);
    size_t kept = 0;
    for (size_t i = 0; i < n; ++i) {
        bool shared = (i && all[i - 1].key == all[i].key) || (i + 1 < n && all[i + 1].key == all[i].key);
        if (!shared) all[kept++] = all[i];
    }
    *count = kept;
    return all;
}
/* the import whose stub is at key, UINT32_MAX for none */
static uint32_t stub_import(const Stub* stubs, size_t count, uint32_t key) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (stubs[mid].key == key) return stubs[mid].import;
        if (stubs[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return UINT32_MAX;
}

/* ---- discovery ---- */

/* the keys a block may go on to, as discovery queues them (found): a native block jumps to the ones it knows */
typedef struct {
    const VitaImage* image;
    uint32_t* keys;
    size_t count;
} Next;
static void next_found(void* context, uint32_t address, int thumb) {
    Next* n = context;
    size_t units;
    if (address & (thumb ? 1u : 3u) || !code_at(n->image, address, thumb, &units) || !units) return;
    uint32_t key = address | (thumb ? 1u : 0u);
    for (size_t i = 0; i < n->count; ++i)
        if (n->keys[i] == key) return;
    if (n->count < SUCCESSORS) n->keys[n->count++] = key;
}
static size_t chain_next(void* user, uint32_t key, size_t units, size_t count, uint32_t keys[2]) {
    Next n = {user, keys, 0};
    size_t available;
    const unsigned char* code = code_at(n.image, key & ~1u, (int)(key & 1), &available);
    snx_arm_successors(SNX_ARM_PROFILE_A9, key & ~1u, code, units, count, (int)(key & 1), next_found, &n);
    return n.count;
}
/* a block's successors as discovery meets them, queued and kept on the block for chaining */
typedef struct {
    Discovery* d;
    Next* next;
} Queued;
static void found(void* context, uint32_t address, int thumb) {
    Queued* q = context;
    queue(q->d, address, thumb);
    next_found(q->next, address, thumb);
}
/* one thread of discovery's: its native back end, whose blocks go in the module's object, and the fallback that learns
   the length of the blocks that leaves, made for the first */
typedef struct {
    SnxNativeArm* native;
    SnxFallbackArm* fallback;
    SnxArmAotCount count;
} Finder;
/* a key of a round: the units its block covers of the count it was given, and who made it */
typedef struct {
    uint32_t key;
    size_t units, count;
    bool native;
} Found;
/* a round: the keys queued before it, shared out to the threads */
typedef struct {
    const Discovery* d;
    const char* every; /* why the fallback makes every block, NULL where the native back end makes them */
    Found* found;
    size_t count, next;
    Finder* finders;
    unsigned started; /* the threads': each takes the next finder */
    bool failed;
    char error[512];
    SnxWorkLock lock;
} Round;
/* the keys a thread takes at a time */
#define ROUND_KEYS 16u

/* the SVC of an import's stub (loader/image.c's imports): "svc #index" */
static bool is_svc(const unsigned char* code, int thumb) {
    return thumb ? (snx_ld_le16(code) & 0xFF00u) == 0xDF00u : (snx_ld_le32(code) & 0xFF000000u) == 0xEF000000u;
}
/* the block at k->key, made by the native back end into f's blocks: NULL, or why it refuses it */
static const char* native_block(Finder* f, const Discovery* d, Found* k) {
    const int thumb = (int)(k->key & 1);
    const uint32_t import = stub_import(d->stubs, d->stub_count, k->key);
    const unsigned char* code = code_at(d->image, k->key & ~1u, thumb, &k->count);
    /* hosted (codegen.h): the run loop and the other blocks are the only callers, and the guest's 4 GiB window is
       followed by pages that fault (VITA_SPACE). It goes on to the next by itself (VitaThread's poll, lookup, direct) */
    SnxArmChainedOptions options = {
        .options = {SNX_ARM_CHAINED_OPTIONS_ABI, SNX_ARM_PROFILE_A9, SNX_ARM_MEMORY_HOSTED, 0, 0},
        .poll = VITA_POLL_OFFSET,
        .lookup = VITA_LOOKUP_OFFSET};
    if (import != UINT32_MAX && is_svc(code, thumb)) options.direct = VITA_DIRECT_OFFSET, options.import = import;
    if (snx_native_arm_options(f->native, &options.options)) return "options the back end does not take";
    k->native = thumb ? snx_native_thumb_block(f->native, NULL, code, k->count, k->key & ~1u, &k->units, chain_next,
                                               (void*)d->image)
                      : snx_native_arm_block(f->native, NULL, code, k->count, k->key & ~1u, &k->units, chain_next,
                                             (void*)d->image);
    return k->native ? NULL : snx_native_arm_why(f->native);
}
/* the block at k->key, by the native back end, or by the fallback for where the block compiler ends it: false with the
   reason in error where neither takes it */
static bool find(Finder* f, const Round* r, Found* k, char* error, size_t error_size) {
    const char* why = f->native ? native_block(f, r->d, k) : r->every;
    if (k->native) {
        ++f->count.native;
        return true;
    }
    if (!f->fallback && !(f->fallback = snx_fallback_arm_new("vita-aot", '0', 0))) {
        snprintf(error, error_size, "out of memory for the block at 0x%08x", (unsigned)k->key);
        return false;
    }
    (void)code_at(r->d->image, k->key & ~1u, (int)(k->key & 1), &k->count);
    const int rc = emit(f->fallback, r->d->image, k->key, k->count, NULL, why, &k->units, error, error_size);
    if (rc == -1 && !snx_fallback_available()) return false; /* none's, which says why */
    if (rc || !k->units) {
        snprintf(error, error_size, "the block compiler refused the block at 0x%08x, of %zu units at most (%s)",
                 (unsigned)k->key, k->count, why);
        return false;
    }
    snx_arm_aot_refused(&f->count, why, k->key);
    return true;
}
static void find_worker(void* arg) {
    Round* r = arg;
    char error[512];
    snx_work_lock(&r->lock);
    Finder* f = &r->finders[r->started++];
    for (;;) {
        const size_t first = r->failed ? r->count : r->next;
        const size_t end = r->count - first > ROUND_KEYS ? first + ROUND_KEYS : r->count;
        r->next = end;
        snx_work_unlock(&r->lock);
        if (first == end) return;
        bool ok = true;
        for (size_t i = first; ok && i < end; ++i) ok = find(f, r, &r->found[i], error, sizeof error);
        snx_work_lock(&r->lock);
        if (!ok && !r->failed) {
            r->failed = true;
            snprintf(r->error, sizeof r->error, "%s", error);
        }
    }
}
/* the blocks the image's entries reach, and the hints' (the blocks runs compiled at run time) at the path hints names,
   in rounds: each round's are made on the threads, then what follows them is queued for the next, in their order. The
   native ones are *native's on return (NULL for none), who made them all in *count */
static bool discover(Discovery* d, const char* hints_path, unsigned threads, const char* every, bool quiet,
                     SnxNativeArm** native, SnxArmAotCount* count, char* error, size_t error_size) {
    const VitaImage* image = d->image;
    size_t hint_count = 0;
    uint32_t* hints = vita_hints_read_file(hints_path, &hint_count);
    Round r = {.d = d, .every = every, .finders = calloc(threads, sizeof *r.finders)};
    bool ok = r.finders != NULL;
    if (!ok) snprintf(error, error_size, "out of memory discovering the code on %u threads", threads);
    snx_work_lock_init(&r.lock);
    if (hint_count && !quiet)
        printf("Seeding discovery with %zu blocks earlier runs compiled at run time\n", hint_count);
    queue_pointer(d, image->module_start);
    if (image->module_stop) queue_pointer(d, image->module_stop);
    for (size_t i = 0; i < image->seed_count; ++i) queue_pointer(d, image->seeds[i]);
    for (size_t i = 0; i < hint_count; ++i) queue_pointer(d, hints[i]);
    free(hints);
    for (unsigned i = 0; ok && !every && i < threads; ++i)
        if (!(r.finders[i].native = snx_native_arm_new())) {
            snprintf(error, error_size, "%s back end: out of memory for thread %u's blocks", snx_native_name(), i);
            ok = false;
        }
    while (ok && d->work_count && !d->failed) {
        r.count = d->work_count;
        Found* found_keys = realloc(r.found, r.count * sizeof *r.found);
        if (!found_keys) {
            d->failed = true;
            break;
        }
        r.found = found_keys;
        for (size_t i = 0; i < r.count; ++i) r.found[i] = (Found){.key = d->work[i]};
        d->work_count = r.next = 0;
        r.started = 0;
        /* a thread's start costs more than a few blocks */
        snx_run_workers(r.count < 4 * ROUND_KEYS ? 1 : threads, find_worker, &r);
        if (r.failed) {
            snprintf(error, error_size, "%s", r.error);
            ok = false;
            break;
        }
        for (size_t i = 0; i < r.count && !d->failed; ++i) {
            const Found* k = &r.found[i];
            size_t available;
            if (d->count == d->capacity) {
                size_t capacity = d->capacity ? d->capacity * 2 : 4096;
                Block* grown = realloc(d->blocks, capacity * sizeof *grown);
                if (!grown) {
                    d->failed = true;
                    break;
                }
                d->blocks = grown, d->capacity = capacity;
            }
            Block* b = &d->blocks[d->count++];
            *b = (Block){.key = k->key, .units = (uint32_t)k->units, .native = k->native};
            Next n = {image, b->next, 0};
            Queued q = {d, &n};
            snx_arm_successors(SNX_ARM_PROFILE_A9, k->key & ~1u,
                               code_at(image, k->key & ~1u, (int)(k->key & 1), &available), k->units, k->count,
                               (int)(k->key & 1), found, &q);
            b->next_count = (uint32_t)n.count;
        }
    }
    for (unsigned i = 0; r.finders && i < threads; ++i) {
        Finder* f = &r.finders[i];
        snx_arm_aot_add(count, &f->count);
        snx_fallback_arm_free(f->fallback);
        if (!i) {
            *native = f->native; /* the caller's */
            continue;
        }
        if (ok && f->native && snx_native_arm_absorb(*native, f->native)) d->failed = true;
        snx_native_arm_free(f->native);
    }
    free(r.finders);
    free(r.found);
    snx_work_lock_destroy(&r.lock);
    if (ok && d->failed) {
        snprintf(error, error_size, "out of memory discovering the code");
        ok = false;
    }
    if (ok && !quiet) printf("Discovered %zu blocks\n", d->count);
    return ok;
}
static int by_key(const void* a, const void* b) {
    uint32_t x = ((const Block*)a)->key, y = ((const Block*)b)->key;
    return (x > y) - (x < y);
}

/* the module's IR as the backend made it, before its passes, as dir/id.ll (--keep-ir) */
static bool save_ir(const char* text, const char* dir, const char* id, char* error, size_t size) {
    char *file = snx_format("%s.ll", id), *path = file ? snx_path_join(dir, file) : NULL;
    bool ok = path && !snx_write_file(path, text, strlen(text));
    if (!ok) {
        char why[256];
        snprintf(error, size, "cannot save the IR of %s in %s: %s", id, dir, snx_os_error(why, sizeof why));
    }
    free(file);
    free(path);
    return ok;
}

/* ---- the chunks of the fallback's blocks ---- */

/* shared out to a worker per host core, each with a fallback of its own */
typedef struct {
    const VitaImage* image;
    const Block* blocks;  /* the fallback's, by key */
    const size_t* starts; /* chunk i is blocks starts[i] to starts[i + 1] */
    const Block* all; /* the module's, what a block may go on to, the fallback's and the native back end's, by key */
    size_t all_count;
    char opt;             /* the -O letter */
    const char* keep_ir;  /* where each chunk's IR goes, NULL for nowhere */
    bool quiet;
    SnxLinkInput* objects; /* chunk i's object */
    const Stub* stubs;     /* the import stubs by key */
    size_t stub_count;
    unsigned chunks, next;
    unsigned finished, steps; /* of the module's objects, the native back end's among them, for the lines */
    bool failed;
    char error[512];
    SnxWorkLock lock;
} Work;
/* Block chaining (recompiler/fallback.h's snx_fallback_arm_chain): a block that ends where another block of the module
   starts goes on to it by a tail call, instead of returning to the runtime's loop to look it up, counting down the
   thread's poll for one at or before its start; a branch to no successor goes through the runtime's block table */
static bool has_block(const Block* blocks, size_t count, uint32_t key) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (blocks[mid].key == key) return true;
        if (blocks[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return false;
}
static bool chunk(SnxFallbackArm* f, Work* w, unsigned index, char* error, size_t size) {
    const size_t begin = w->starts[index], end = w->starts[index + 1];
    for (size_t i = begin; i < end; ++i) {
        char name[24];
        size_t consumed = 0;
        const uint32_t key = w->blocks[i].key;
        block_name(name, key);
        const int rc = emit(f, w->image, key, w->blocks[i].units, name, "discovery left it to the fallback", &consumed,
                            error, size);
        if (rc == -1 && !snx_fallback_available()) return false; /* none's, which says why */
        if (rc || consumed != w->blocks[i].units) {
            snprintf(error, size, "the block at 0x%08x compiled differently from its discovery", (unsigned)key);
            return false;
        }
    }
    /* every block of the chunk exists now, so a successor in it is found by name, and one in another chunk is a
       declaration the linker resolves */
    const SnxArmChainedOptions chained = {
        .options = {SNX_ARM_CHAINED_OPTIONS_ABI, SNX_ARM_PROFILE_A9, SNX_ARM_MEMORY_HOSTED, 0, 0},
        .poll = VITA_POLL_OFFSET,
        .lookup = VITA_LOOKUP_OFFSET};
    for (size_t i = begin; i < end; ++i) {
        uint32_t next[SUCCESSORS], key = w->blocks[i].key;
        char name[24], names[SUCCESSORS][24];
        const char* named[SUCCESSORS];
        size_t n = 0;
        SnxArmChainedOptions options = chained;
        uint32_t import = w->blocks[i].units == 1 ? stub_import(w->stubs, w->stub_count, key) : UINT32_MAX;
        if (import != UINT32_MAX) {
            /* an import's stub, which the block compiler made the svc block of, is replaced by the direct call */
            const uint32_t after_key = ((key & ~1u) + (key & 1 ? 2u : 4u)) | (key & 1);
            options.direct = VITA_DIRECT_OFFSET, options.import = import;
            if (has_block(w->all, w->all_count, after_key)) next[n++] = after_key;
        } else
            for (uint32_t k = 0; k < w->blocks[i].next_count; ++k)
                if (has_block(w->all, w->all_count, w->blocks[i].next[k])) next[n++] = w->blocks[i].next[k];
        for (size_t k = 0; k < n; ++k) block_name(names[k], next[k]), named[k] = names[k];
        block_name(name, key);
        if (snx_fallback_arm_chain(f, name, key, &options, next, named, n, error, size)) return false;
    }
    char id[24], *text = NULL;
    snprintf(id, sizeof id, "blocks_%03u", index);
    bool ok =
        !w->keep_ir || (!snx_fallback_arm_ir(f, &text, error, size) && save_ir(text, w->keep_ir, id, error, size));
    unsigned char* object = NULL;
    ok = ok && !snx_fallback_arm_object(f, &object, &w->objects[index].size, error, size);
    w->objects[index].data = object;
    free(text);
    return ok;
}
static void worker(void* arg) {
    Work* w = arg;
    char error[512] = "out of memory for a maker of blocks";
    SnxFallbackArm* f = snx_fallback_arm_new("vita-aot", w->opt, 0);
    bool ok = f && !snx_fallback_arm_start(f, error, sizeof error);
    for (;;) {
        snx_work_lock(&w->lock);
        if (!ok && !w->failed) {
            w->failed = true;
            snprintf(w->error, sizeof w->error, "%s", error);
        }
        unsigned i = w->failed ? w->chunks : w->next++;
        snx_work_unlock(&w->lock);
        if (i >= w->chunks) break;
        ok = chunk(f, w, i, error, sizeof error);
        snx_work_lock(&w->lock);
        if (ok) ++w->finished;
        if (ok && !w->quiet) printf("Compiled %u/%u chunks of blocks\n", w->finished, w->steps);
        snx_work_unlock(&w->lock);
    }
    snx_fallback_arm_free(f);
}
static bool compile_chunks(Work* w, unsigned jobs) {
    if (jobs > w->chunks) jobs = w->chunks;
    if (!w->quiet) printf("Compiling %u chunks of blocks on %u threads\n", w->chunks, jobs);
    snx_work_lock_init(&w->lock);
    snx_run_workers(jobs, worker, w);
    snx_work_lock_destroy(&w->lock);
    return !w->failed;
}

/* ---- the index ---- */

/* the descriptor, its tables' addresses the linker's */
static VitaModule describe(const VitaImage* image, size_t count) {
    VitaModule m = {.magic = VITA_MODULE_MAGIC,
                    .abi = VITA_MODULE_ABI,
                    .descriptor_size = sizeof(VitaModule),
                    .cpu_size = sizeof(SnxArmCPU),
                    .arm_abi = SNX_ARM_ABI,
                    .pointer_size = sizeof(void*),
                    .count = (uint32_t)count};
    memcpy(m.identity, image->identity, sizeof m.identity);
    return m;
}
/* The index without LLVM (native.h's snx_native_data): the sorted keys, each one's block, and the descriptor
   (VitaModule) the runtime looks up, the object's one global. 1, 0 where this host's native back end has no objects, -1
   with the reason in error */
static int native_index(const VitaImage* image, const Block* blocks, size_t count, SnxLinkInput* output, char* error,
                        size_t size) {
    unsigned pointer = 0;
    SnxXElf* o = snx_native_data(&pointer);
    if (!o) return 0;
    const VitaModule m = describe(image, count);
    const size_t n = count ? count : 1; /* a module of none has an empty slot */
    const size_t keys = snx_xelf_put(o, SNX_ELF_RODATA, NULL, n * sizeof(uint32_t), sizeof(uint32_t));
    const size_t table = snx_xelf_put(o, SNX_ELF_RELRO, NULL, n * sizeof(SnxArmBlock), sizeof(SnxArmBlock));
    unsigned char *key_bytes = snx_xelf_bytes(o, SNX_ELF_RODATA), *object = NULL;
    int rc = -1;
    if (key_bytes) { /* it has them */
        for (size_t i = 0; i < count; ++i) {
            char name[24];
            memcpy(key_bytes + keys + i * sizeof(uint32_t), &blocks[i].key, sizeof(uint32_t));
            block_name(name, blocks[i].key);
            snx_xelf_reloc(o, SNX_ELF_RELRO, table + i * sizeof(SnxArmBlock), name, pointer, 0);
        }
        snx_xelf_symbol(o, "vita_keys", SNX_ELF_RODATA, keys, n * sizeof(uint32_t), SNX_ELF_LOCAL);
        snx_xelf_symbol(o, "vita_blocks", SNX_ELF_RELRO, table, n * sizeof(SnxArmBlock), SNX_ELF_LOCAL);
        const size_t at = snx_xelf_put(o, SNX_ELF_RELRO, &m, sizeof m, alignof(VitaModule));
        snx_xelf_symbol(o, "snx_vita_module_v1", SNX_ELF_RELRO, at, sizeof m, SNX_ELF_GLOBAL);
        snx_xelf_reloc(o, SNX_ELF_RELRO, at + offsetof(VitaModule, keys), "vita_keys", pointer, 0);
        snx_xelf_reloc(o, SNX_ELF_RELRO, at + offsetof(VitaModule, blocks), "vita_blocks", pointer, 0);
        rc = snx_xelf_write(o, &object, &output->size);
        output->data = object;
    }
    snx_xelf_free(o);
    if (rc) snprintf(error, size, "out of memory for the module's index");
    return rc ? -1 : 1;
}
/* the same by the fallback, where the host has no native back end or its IR is kept */
typedef struct {
    const Block* blocks;
    size_t count;
} Listed;
static int listed_name(void* user, size_t i, char* out, size_t size) {
    const Listed* l = user;
    if (i >= l->count) return 0;
    char id[24];
    block_name(id, l->blocks[i].key);
    snprintf(out, size, "%s", id);
    return 1;
}
static bool fallback_index(const VitaImage* image, const Block* blocks, size_t count, SnxLinkInput* output, char opt,
                           const char* keep_ir, char* error, size_t size) {
    SnxFallbackData* d =
        snx_fallback_data_new("vita-aot", "vita-aot", opt, keep_ir ? snx_arm_aot_kept : snx_arm_aot_none);
    const size_t n = count ? count : 1; /* a module of none has an empty slot */
    uint32_t* keys = calloc(n, sizeof *keys);
    bool ok = d && keys;
    if (!ok) snprintf(error, size, "out of memory for the module's index");
    for (size_t i = 0; ok && i < count; ++i) keys[i] = blocks[i].key;
    Listed listed = {blocks, count};
    ok = ok && !snx_fallback_data_integers(d, "vita_keys", 32, n, keys, error, size) &&
         !snx_fallback_data_blocks(d, "vita_blocks", n, listed_name, &listed, error, size);
    const VitaModule m = describe(image, count);
#define FIELD(field)                                                                                                   \
    { .bits = 32, .value = m.field, .offset = offsetof(VitaModule, field) }
    const SnxFallbackField fields[] = {
        {.bits = 64, .value = m.magic, .offset = offsetof(VitaModule, magic)},
        FIELD(abi),
        FIELD(descriptor_size),
        FIELD(cpu_size),
        FIELD(arm_abi),
        FIELD(pointer_size),
        FIELD(count),
        {.bytes = m.identity, .count = sizeof m.identity, .offset = offsetof(VitaModule, identity)},
        {.global = "vita_keys", .offset = offsetof(VitaModule, keys)},
        {.global = "vita_blocks", .offset = offsetof(VitaModule, blocks)}};
#undef FIELD
    ok = ok && !snx_fallback_data_descriptor(d, "snx_vita_module_v1", fields, sizeof fields / sizeof *fields,
                                             sizeof(VitaModule), error, size);
    char* text = NULL;
    ok = ok &&
         (!keep_ir || (!snx_fallback_data_ir(d, &text, error, size) && save_ir(text, keep_ir, "index", error, size)));
    unsigned char* object = NULL;
    ok = ok && !snx_fallback_data_object(d, &object, &output->size, error, size);
    output->data = object;
    free(text);
    free(keys);
    snx_fallback_data_free(d);
    return ok;
}
static bool index_object(const VitaImage* image, const Block* blocks, size_t count, SnxLinkInput* output, char opt,
                         const char* keep_ir, char* error, size_t size) {
    const int made = keep_ir ? 0 : native_index(image, blocks, count, output, error, size);
    return made > 0 || (!made && fallback_index(image, blocks, count, output, opt, keep_ir, error, size));
}
/* where each chunk starts in blocks, + count after the last, into starts (count + 1 of them at most): chunk
   instructions to a chunk, whole blocks, or CHUNK_BLOCKS blocks for 0. The chunks' count */
static unsigned cut_chunks(const Block* blocks, size_t count, int chunk, size_t* starts) {
    unsigned chunks = 0;
    size_t at = 0;
    while (at < count) {
        starts[chunks++] = at;
        if (chunk) {
            size_t instructions = 0;
            do instructions += blocks[at++].units;
            while (at < count && instructions < (size_t)chunk);
        } else
            at = count - at > CHUNK_BLOCKS ? at + CHUNK_BLOCKS : count;
    }
    starts[chunks] = count;
    return chunks;
}
/* The module: the native back end's blocks and the chunks of the fallback's, the index and the helpers they call
   (armlib.c), linked by the backend's own linker into the shared library the runtime loads (load.c), which exports the
   descriptor alone */
int vita_aot_compile(const char* folder, const VitaImage* image, const SnxCliRecompile* asked, char* error,
                     size_t error_size) {
    Discovery d = {.image = image};
    Work work = {0};
    SnxArmAotCount made = {0};
    SnxNativeArm* native = NULL;
    SnxLinkInput* objects = NULL;
    SnxLinkOutput linked = {NULL, 0};
    Block* llvm = NULL;
    size_t *starts = NULL, llvm_count = 0;
    char *target = NULL, *staged = NULL, *kept = NULL, *recorded = NULL, note[160];
    Stub* stubs = NULL;
    const char* exports[1] = {"snx_vita_module_v1"};
    unsigned files = 0;
    int result = 1;
    const char opt = g_settings.optimization_level[0]; /* the setting opt's, which -O sets */
    bool quiet = asked && asked->quiet;
    if (error_size) error[0] = 0;
    /* on the threads the settings aot_threads and aot_threads_most give, as the Switch's recompile (snx_aot_threads) */
    const unsigned jobs = snx_aot_threads(g_settings.aot_threads, g_settings.aot_threads_most, note, sizeof note);
    if (note[0]) fprintf(stderr, "scarletnx-vita: %s\n", note);
    /* --keep-ir's folder, <title ID>_build, as a Switch game's */
    if (asked && asked->keep_ir) {
        kept = vita_game_file(folder, "_build");
        if (!kept || snx_mkdirs(kept)) {
            char why[256];
            snprintf(error, error_size, "cannot make the folder of the IR --keep-ir keeps, %s: %s",
                     kept ? kept : "(out of memory)", snx_os_error(why, sizeof why));
            goto done;
        }
    }
    stubs = stubs_make(image, &d.stub_count);
    if (!stubs && image->import_count) {
        snprintf(error, error_size, "out of memory for %u imports' stubs", (unsigned)image->import_count);
        goto done;
    }
    d.stubs = stubs;
    if (asked && asked->jit_only) { /* a module of no blocks: the run-time compiler compiles every one it reaches */
        if (!quiet) puts("No code ahead of time (--jit-only): the run-time compiler compiles every block");
    } else {
        /* the blocks earlier runs compiled at run time: the game's own record, or another --jit-cache names */
        recorded = asked && asked->jit_cache ? snx_strdup(asked->jit_cache) : vita_game_file(folder, VITA_HINTS_SUFFIX);
        if (asked && asked->jit_cache && !snx_is_file(recorded)) {
            snprintf(error, error_size,
                     "--jit-cache %s: no such file, a list of the blocks a run compiled (<title ID>%s)",
                     asked->jit_cache, VITA_HINTS_SUFFIX);
            goto done;
        }
        const char* every = kept ? snx_arm_aot_kept : snx_native_arm_available() ? NULL : snx_arm_aot_none;
        if (!discover(&d, recorded, jobs, every, quiet, &native, &made, error, error_size)) goto done;
    }
    snx_arm_aot_report(&made, "scarletnx-vita", quiet);
    if (d.count) qsort(d.blocks, d.count, sizeof *d.blocks, by_key); /* a NULL of none is no argument of it */
    target = vita_game_file(folder, VITA_MODULE_SUFFIX);
    staged = target ? snx_format("%s.tmp", target) : NULL;
    llvm = malloc((made.llvm + 1) * sizeof *llvm);
    starts = malloc((made.llvm + 1) * sizeof *starts);
    if (!staged || !llvm || !starts) {
        snprintf(error, error_size, "out of memory");
        goto done;
    }
    for (size_t i = 0; i < d.count; ++i)
        if (!d.blocks[i].native) {
            /* the entry the native blocks' jumps to it take */
            if (native && snx_native_arm_entry(native, d.blocks[i].key)) {
                snprintf(error, error_size, "%s back end: out of memory", snx_native_name());
                goto done;
            }
            llvm[llvm_count++] = d.blocks[i];
        }
    const unsigned chunks = cut_chunks(llvm, llvm_count, asked ? asked->chunk : 0, starts);
    files = chunks + 3; /* the helpers, the index, the native blocks, the fallback's chunks */
    objects = calloc(files, sizeof *objects);
    if (!objects) {
        snprintf(error, error_size, "out of memory");
        goto done;
    }
    objects[0].name = "armlib", objects[1].name = "index", objects[2].name = "native blocks";
    for (unsigned i = 3; i < files; ++i) objects[i].name = "blocks";
    work = (Work){.image = image,
                  .blocks = llvm,
                  .starts = starts,
                  .all = d.blocks,
                  .all_count = d.count,
                  .opt = opt,
                  .keep_ir = kept,
                  .quiet = quiet,
                  .objects = objects + 3,
                  .chunks = chunks,
                  .steps = chunks + (made.native != 0),
                  .stubs = stubs,
                  .stub_count = d.stub_count};
    if (made.native) {
        unsigned char* object = NULL;
        if (snx_native_arm_object(native, &object, &objects[2].size, NULL)) {
            snprintf(error, error_size, "%s back end: out of memory for the object of %zu blocks", snx_native_name(),
                     made.native);
            goto done;
        }
        objects[2].data = object;
        ++work.finished;
        if (!quiet) printf("Compiled %u/%u chunks of blocks\n", work.finished, work.steps);
    }
    if (chunks && !compile_chunks(&work, jobs)) {
        snprintf(error, error_size, "%s", work.error);
        goto done;
    }
    if (!index_object(image, d.blocks, d.count, &objects[1], opt, kept, error, error_size)) goto done;
    objects[0].data = (unsigned char*)snx_armlib_object, objects[0].size = (size_t)snx_armlib_object_size;
    unsigned linking = 2; /* the objects there are */
    for (unsigned i = 2; i < files; ++i)
        if (objects[i].data) {
            const SnxLinkInput object = objects[i];
            objects[i] = (SnxLinkInput){0};
            objects[linking++] = object;
        }
    if (snx_link(objects, linking, exports, 1, snx_path_leaf(target), &linked, error, error_size)) goto done;
    if (snx_write_file(staged, linked.data, linked.size) || plat_replace_file(staged, target)) {
        char why[256];
        snprintf(error, error_size, "cannot write %s: %s", target, snx_os_error(why, sizeof why));
        (void)remove(staged);
        goto done;
    }
    if (!quiet) printf("Compiled %zu blocks into %s\n", d.count, target);
    result = 0;
done:
    if (objects)
        for (unsigned i = 1; i < files; ++i) free((void*)objects[i].data);
    free(objects);
    free(stubs);
    free(linked.data);
    free(staged);
    free(target);
    free(llvm);
    free(starts);
    free(kept);
    free(recorded);
    free(d.blocks);
    free(d.set);
    free(d.work);
    snx_native_arm_free(native);
    return result;
}
