/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The offline compiler, part of the driver: the image's blocks to a native module, as the 3DS driver's is
   (3ds/aot/compiler.c). The blocks are found by following the code from what is known to be code: module_start, the
   code pointers the relocations wrote, and the hints of earlier runs. The host's native back end makes each block as it
   is found, which says where it ends and so where the next ones are (cpu/arm/successors.h). A block it refuses fails
   the recompile, named with the reason */
#include "vita/aot/module.h"
#include "common/cpu/arm/aot.h"
#include "common/cpu/arm/successors.h"
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

/* the most blocks one block goes on to, a branch's target and the code after it */
#define SUCCESSORS   2u

typedef struct {
    uint32_t key, units;
    uint32_t next[SUCCESSORS]; /* the keys it may go on to */
    uint32_t next_count;
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
/* one thread of discovery's: its native back end, whose blocks go in the module's object */
typedef struct {
    SnxNativeArm* native;
    SnxArmAotCount count;
} Finder;
/* a key of a round: the units its block covers of the count it was given */
typedef struct {
    uint32_t key;
    size_t units, count;
} Found;
/* a round: the keys queued before it, shared out to the threads */
typedef struct {
    const Discovery* d;
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
    const int made = thumb ? snx_native_thumb_block(f->native, NULL, code, k->count, k->key & ~1u, &k->units,
                                                    chain_next, (void*)d->image)
                           : snx_native_arm_block(f->native, NULL, code, k->count, k->key & ~1u, &k->units, chain_next,
                                                  (void*)d->image);
    return made ? NULL : snx_native_arm_why(f->native);
}
/* the block at k->key, by the native back end: false with the reason in error where it refuses it */
static bool find(Finder* f, const Round* r, Found* k, char* error, size_t error_size) {
    const char* why = native_block(f, r->d, k);
    if (why) {
        snprintf(error, error_size, "the %s back end made no %s block at 0x%08x: %s", snx_native_name(),
                 k->key & 1 ? "Thumb" : "ARM", (unsigned)(k->key & ~1u), why);
        return false;
    }
    ++f->count.native;
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
   blocks are *native's on return (NULL for none), how many in *count */
static bool discover(Discovery* d, const char* hints_path, unsigned threads, bool quiet, SnxNativeArm** native,
                     SnxArmAotCount* count, char* error, size_t error_size) {
    const VitaImage* image = d->image;
    size_t hint_count = 0;
    uint32_t* hints = vita_hints_read_file(hints_path, &hint_count);
    Round r = {.d = d, .finders = calloc(threads, sizeof *r.finders)};
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
    for (unsigned i = 0; ok && i < threads; ++i)
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
            *b = (Block){.key = k->key, .units = (uint32_t)k->units};
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
/* The index (native.h's snx_native_data): the sorted keys, each one's block, and the descriptor (VitaModule) the runtime
   looks up, the object's one global. 1, or 0 with the reason in error */
static bool index_object(const VitaImage* image, const Block* blocks, size_t count, SnxLinkInput* output, char* error,
                         size_t size) {
    unsigned pointer = 0;
    SnxXElf* o = snx_native_data(&pointer);
    if (!o) {
        snprintf(error, size, "this host's native back end (%s) makes no module index", snx_native_name());
        return false;
    }
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
    return !rc;
}
/* The module: the native back end's blocks, the index and the helpers they call (armlib.c), linked by the backend's own
   linker into the shared library the runtime loads (load.c), which exports the descriptor alone */
int vita_aot_compile(const char* folder, const VitaImage* image, const SnxCliRecompile* asked, char* error,
                     size_t error_size) {
    Discovery d = {.image = image};
    SnxArmAotCount made = {0};
    SnxNativeArm* native = NULL;
    SnxLinkInput objects[3] = {{.name = "armlib"}, {.name = "index"}, {.name = "native blocks"}};
    SnxLinkOutput linked = {NULL, 0};
    char *target = NULL, *staged = NULL, *recorded = NULL, note[160];
    Stub* stubs = NULL;
    const char* exports[1] = {"snx_vita_module_v1"};
    int result = 1;
    bool quiet = asked && asked->quiet;
    if (error_size) error[0] = 0;
    if (!snx_native_arm_available()) {
        if (!snx_native_floor(error, error_size))
            snprintf(error, error_size, "this host's native back end (%s) makes no AArch32 blocks", snx_native_name());
        goto done;
    }
    /* on the threads the settings aot_threads and aot_threads_most give, as the Switch's recompile (snx_aot_threads) */
    const unsigned jobs = snx_aot_threads(g_settings.aot_threads, g_settings.aot_threads_most, note, sizeof note);
    if (note[0]) fprintf(stderr, "scarletnx-vita: %s\n", note);
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
        if (!discover(&d, recorded, jobs, quiet, &native, &made, error, error_size)) goto done;
    }
    snx_arm_aot_report(&made, quiet ? NULL : stdout);
    if (d.count) qsort(d.blocks, d.count, sizeof *d.blocks, by_key); /* a NULL of none is no argument of it */
    target = vita_game_file(folder, VITA_MODULE_SUFFIX);
    staged = target ? snx_format("%s.tmp", target) : NULL;
    if (!staged) {
        snprintf(error, error_size, "out of memory");
        goto done;
    }
    if (made.native) {
        unsigned char* object = NULL;
        if (snx_native_arm_object(native, &object, &objects[2].size, NULL)) {
            snprintf(error, error_size, "%s back end: out of memory for the object of %zu blocks", snx_native_name(),
                     made.native);
            goto done;
        }
        objects[2].data = object;
    }
    if (!quiet) puts("Compiled 1/1 chunks of blocks"); /* the progress line the launcher reads */
    if (!index_object(image, d.blocks, d.count, &objects[1], error, error_size)) goto done;
    objects[0].data = (unsigned char*)snx_armlib_object, objects[0].size = (size_t)snx_armlib_object_size;
    if (snx_link(objects, objects[2].data ? 3 : 2, exports, 1, snx_path_leaf(target), &linked, error, error_size))
        goto done;
    if (snx_write_file(staged, linked.data, linked.size) || plat_replace_file(staged, target)) {
        char why[256];
        snprintf(error, error_size, "cannot write %s: %s", target, snx_os_error(why, sizeof why));
        (void)remove(staged);
        goto done;
    }
    if (!quiet) printf("Compiled %zu blocks into %s\n", d.count, target);
    result = 0;
done:
    for (unsigned i = 1; i < 3; ++i) free((void*)objects[i].data);
    free(stubs);
    free(linked.data);
    free(staged);
    free(target);
    free(recorded);
    free(d.blocks);
    free(d.set);
    free(d.work);
    snx_native_arm_free(native);
    return result;
}
