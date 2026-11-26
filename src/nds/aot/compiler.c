/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The offline compiler, part of the driver, after the 3DS's (3ds/aot/compiler.c): the blocks discovery found in the
   image's regions, compiled in chunks on a worker per thread by the host's native back end (cpu/arm/aot.h), an index of
   them for each region, and the helpers they call, linked by the backend's own linker into the module the run loads. */
#include "nds/aot/nds_module.h"
#include "common/cpu/arm/aot.h"
#include "common/cpu/recompiler/rtlib/rtlib_object.h"
#include "common/cpu/recompiler/object/link.h"
#include "common/cpu/recompiler/mach/native.h"
#include "common/cpu/recompiler/worker_threads.h"
#include "common/cpu/recompiler/x64/x64_elf.h"
#include "common/platform/file.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK_ENTRIES 512u
typedef struct {
    uint32_t region, index; /* the slot in the region's table of its mode */
    uint8_t mode, length;
} Entry;

static void name(char out[48], const Entry* e) {
    snprintf(out, 48, "nds_%u_%c_%u", e->region, e->mode ? 't' : 'a', e->index);
}

/* The entries' blocks into an object (cpu/arm/aot.h). A block the backend ends sooner than discovery planned goes on as a
   block of its own at the instruction after. *made gets every block that was made, with the length the backend gave it
   (*made_count of them, the caller's to free), for the index */
static bool chunk(SnxArmAot* aot, const NdsRom* rom, const Entry* entries, unsigned count, SnxLinkInput* output,
                  Entry** made, unsigned* made_count, char* error, size_t size) {
    unsigned capacity = 0;
    const SnxArmCodegenOptions options = {SNX_ARM_CODEGEN_OPTIONS_ABI, SNX_ARM_PROFILE_ARM946E, SNX_ARM_MEMORY_FLAT, 0,
                                          0};
    *made = NULL;
    *made_count = 0;
    for (unsigned i = 0; i < count; ++i) {
        Entry e = entries[i];
        const NdsRegion* r = &rom->regions[e.region];
        while (e.length) {
            unsigned width = e.mode ? 2 : 4;
            char block[48];
            name(block, &e);
            size_t consumed = 0;
            if (snx_arm_aot_block(aot, block, r->data + (size_t)e.index * width, e.length, r->address + e.index * width,
                                  e.mode != 0, &options, &consumed, error, size))
                return false;
            if (*made_count == capacity) {
                Entry* grown = realloc(*made, (capacity = capacity ? capacity * 2 : 64) * sizeof *grown);
                if (!grown) {
                    snprintf(error, size, "out of memory for static entries");
                    return false;
                }
                *made = grown;
            }
            (*made)[(*made_count)++] = (Entry){e.region, e.index, e.mode, (uint8_t)consumed};
            e.index += (unsigned)consumed;
            e.length = (uint8_t)(e.length - consumed);
        }
    }
    return snx_arm_aot_objects(aot, output, error, size) >= 0;
}

/* The module's descriptor, its regions' address the linker's */
static NdsModule describe(const NdsRom* rom) {
    return (NdsModule){.magic = NDS_MODULE_MAGIC,
                       .abi = NDS_MODULE_ABI,
                       .arm_abi = SNX_ARM_ABI,
                       .cpu_size = sizeof(SnxArmCPU),
                       .pointer_size = sizeof(void*),
                       .profile = SNX_ARM_PROFILE_ARM946E,
                       .region_count = rom->region_count,
                       .identity = rom->identity};
}
/* The index (native.h's snx_native_data): each region's two tables of blocks and of their lengths, the regions, and the
   descriptor the run reads, the object's one global. entries are sorted by region, mode and slot. 0, or -1 with the
   reason in error */
static int native_index(const NdsRom* rom, const Entry* entries, unsigned count, SnxLinkInput* output, char* error,
                        size_t size) {
    unsigned pointer = 0, cursor = 0;
    SnxXElf* o = snx_native_data(&pointer);
    if (!o) {
        snprintf(error, size, "this host's native back end (%s) makes no module index", snx_native_name());
        return -1;
    }
    NdsModuleRegion* regions = calloc(rom->region_count ? rom->region_count : 1, sizeof *regions);
    unsigned char* object = NULL;
    int rc = -1;
    if (!regions) goto done;
    for (unsigned r = 0; r < rom->region_count; ++r) {
        const NdsRegion* region = &rom->regions[r];
        regions[r].address = region->address;
        regions[r].size = region->size;
        regions[r].hash = region->hash;
        for (unsigned mode = 0; mode < 2; ++mode) {
            const size_t n = region->size / (mode ? 2 : 4);
            uint8_t* units = calloc(n ? n : 1, 1);
            if (!units) goto done;
            const size_t at = snx_xelf_put(o, SNX_ELF_RELRO, NULL, n * sizeof(SnxArmBlock), sizeof(SnxArmBlock));
            for (; cursor < count && entries[cursor].region == r && entries[cursor].mode == mode; ++cursor) {
                const Entry* e = &entries[cursor];
                char id[48];
                if (e->index >= n || units[e->index]) continue;
                units[e->index] = e->length;
                name(id, e);
                snx_xelf_reloc(o, SNX_ELF_RELRO, at + e->index * sizeof(SnxArmBlock), id, pointer, 0);
            }
            char id[48];
            snprintf(id, sizeof id, "nds_%u_%s", r, mode ? "thumb" : "arm");
            snx_xelf_symbol(o, id, SNX_ELF_RELRO, at, n * sizeof(SnxArmBlock), SNX_ELF_LOCAL);
            const size_t lengths = snx_xelf_put(o, SNX_ELF_RODATA, units, n, 1);
            snprintf(id, sizeof id, "nds_%u_%s_units", r, mode ? "thumb" : "arm");
            snx_xelf_symbol(o, id, SNX_ELF_RODATA, lengths, n, SNX_ELF_LOCAL);
            free(units);
        }
    }
    const size_t list =
        snx_xelf_put(o, SNX_ELF_RELRO, regions, rom->region_count * sizeof *regions, alignof(NdsModuleRegion));
    snx_xelf_symbol(o, "nds_regions", SNX_ELF_RELRO, list, rom->region_count * sizeof *regions, SNX_ELF_LOCAL);
    for (unsigned r = 0; r < rom->region_count; ++r) {
        static const struct {
            size_t at;
            const char* table;
        } tables[] = {{offsetof(NdsModuleRegion, arm), "arm"},
                      {offsetof(NdsModuleRegion, thumb), "thumb"},
                      {offsetof(NdsModuleRegion, arm_units), "arm_units"},
                      {offsetof(NdsModuleRegion, thumb_units), "thumb_units"}};
        for (size_t k = 0; k < sizeof tables / sizeof *tables; ++k) {
            char id[48];
            snprintf(id, sizeof id, "nds_%u_%s", r, tables[k].table);
            snx_xelf_reloc(o, SNX_ELF_RELRO, list + r * sizeof *regions + tables[k].at, id, pointer, 0);
        }
    }
    const NdsModule m = describe(rom);
    const size_t at = snx_xelf_put(o, SNX_ELF_RELRO, &m, sizeof m, alignof(NdsModule));
    snx_xelf_symbol(o, NDS_MODULE_SYMBOL, SNX_ELF_RELRO, at, sizeof m, SNX_ELF_GLOBAL);
    snx_xelf_reloc(o, SNX_ELF_RELRO, at + offsetof(NdsModule, regions), "nds_regions", pointer, 0);
    if (!snx_xelf_write(o, &object, &output->size)) rc = 0;
    output->data = object;
done:
    if (rc < 0) snprintf(error, size, "out of memory for the module's index");
    free(regions);
    snx_xelf_free(o);
    return rc;
}

/* the hints: a region's index and pc | 1 a line, in hex. *count pairs, the caller's to free */
static uint32_t* read_hints(const char* path, size_t* count) {
    FILE* f = path ? fopen(path, "r") : NULL;
    uint32_t* hints = NULL;
    size_t capacity = 0;
    unsigned region, pc;
    *count = 0;
    while (f && fscanf(f, "%x %x", &region, &pc) == 2) {
        if (*count == capacity) {
            uint32_t* grown = realloc(hints, (capacity = capacity ? capacity * 2 : 1024) * 2 * sizeof *grown);
            if (!grown) break;
            hints = grown;
        }
        hints[*count * 2] = region, hints[*count * 2 + 1] = pc;
        ++*count;
    }
    if (f) fclose(f);
    return hints;
}

typedef struct {
    const NdsRom* rom;
    const Entry* entries;
    SnxLinkInput* objects; /* chunk i's is objects[i], no data for one with no blocks */
    unsigned count, chunks, next, finished;
    bool failed;
    Entry* made; /* every block the chunks made */
    unsigned made_count;
    SnxArmAotCount blocks; /* who made them */
    char error[512];
    SnxWorkLock lock;
} Work;

static void worker(void* arg) {
    Work* w = arg;
    char error[512];
    SnxArmAot* aot = snx_arm_aot_new(error, sizeof error);
    bool ok = aot != NULL;
    for (;;) {
        snx_work_lock(&w->lock);
        if (!ok && !w->failed) {
            w->failed = true;
            snprintf(w->error, sizeof w->error, "%s", error);
        }
        unsigned i = w->failed ? w->chunks : w->next++;
        snx_work_unlock(&w->lock);
        if (i >= w->chunks) break;
        unsigned begin = i * CHUNK_ENTRIES, n = w->count - begin;
        if (n > CHUNK_ENTRIES) n = CHUNK_ENTRIES;
        Entry* made = NULL;
        unsigned made_count = 0;
        ok = chunk(aot, w->rom, w->entries + begin, n, &w->objects[i], &made, &made_count, error, sizeof error);
        snx_work_lock(&w->lock);
        Entry* merged = made_count ? realloc(w->made, (w->made_count + made_count) * sizeof *merged) : w->made;
        if (ok && made_count && !merged) {
            ok = false;
            snprintf(error, sizeof error, "out of memory for static entries");
        }
        if (ok) {
            w->made = merged;
            if (made_count) memcpy(w->made + w->made_count, made, made_count * sizeof *made);
            w->made_count += made_count;
            if (!(++w->finished % 16) || w->finished == w->chunks)
                printf("ARM9 AOT: %u/%u chunks of static blocks emitted\n", w->finished, w->chunks);
        }
        snx_work_unlock(&w->lock);
        free(made);
    }
    if (aot) {
        snx_work_lock(&w->lock);
        snx_arm_aot_add(&w->blocks, snx_arm_aot_count(aot));
        snx_work_unlock(&w->lock);
    }
    snx_arm_aot_free(aot);
}

static int by_place(const void* a, const void* b) {
    const Entry *x = a, *y = b;
    if (x->region != y->region) return x->region < y->region ? -1 : 1;
    if (x->mode != y->mode) return x->mode < y->mode ? -1 : 1;
    return (x->index > y->index) - (x->index < y->index);
}

int nds_aot_compile(const NdsRom* rom, const char* path, const char* hints_path, int threads, int most,
                    SnxArmAotCount* blocks_made, char* error, size_t error_size) {
    int result = 1;
    NdsPlan* plans = NULL;
    SnxLinkInput* objects = NULL;
    SnxLinkOutput linked = {NULL, 0};
    size_t hint_count = 0;
    char* staged = snx_format("%s.tmp", path);
    const char* exports[1] = {NDS_MODULE_SYMBOL};
    Entry* entries = NULL;
    unsigned count = 0, files = 0;
    uint64_t planned = 0;
    Work work = {0};
    if (error_size) error[0] = 0;
    uint32_t* hints = read_hints(hints_path, &hint_count);
    if (hint_count) printf("Seeding ARM9 discovery with %zu blocks earlier runs compiled at run time\n", hint_count);
    bool discovered = staged && nds_discover(rom, hints, hint_count, &plans, &planned);
    free(hints);
    if (!discovered || planned > 0x7FFFFFFFu) {
        snprintf(error, error_size, "out of memory for static discovery");
        goto done;
    }
    entries = calloc(planned ? (size_t)planned : 1, sizeof *entries);
    if (!entries) {
        snprintf(error, error_size, "out of memory for static entries");
        goto done;
    }
    for (unsigned r = 0; r < rom->region_count; ++r) {
        const NdsRegion* region = &rom->regions[r];
        for (unsigned mode = 0; mode < 2; ++mode) {
            unsigned width = mode ? 2 : 4;
            for (uint32_t i = 0; i < region->size / width; ++i) {
                unsigned length = nds_plan_length(region, &plans[r], region->address + i * width, !!mode);
                if (length && count < planned) entries[count++] = (Entry){r, i, (uint8_t)mode, (uint8_t)length};
            }
        }
    }
    {
        uint64_t bytes = 0, covered = 0;
        unsigned thumb = 0;
        for (unsigned r = 0; r < rom->region_count; ++r) bytes += rom->regions[r].size;
        for (unsigned i = 0; i < count; ++i) {
            thumb += entries[i].mode;
            covered += entries[i].length * (entries[i].mode ? 2u : 4u);
        }
        printf("Discovered ARM9 static coverage: %u blocks (%u ARM, %u Thumb) over %llu of %llu bytes in %u regions\n",
               count, count - thumb, thumb, (unsigned long long)covered, (unsigned long long)bytes, rom->region_count);
    }
    work = (Work){.rom = rom,
                  .entries = entries,
                  .count = count,
                  .chunks = (count + CHUNK_ENTRIES - 1) / CHUNK_ENTRIES};
    files = 2 + work.chunks; /* the helpers, the index, one for each chunk */
    objects = calloc(files, sizeof *objects);
    if (!objects) {
        snprintf(error, error_size, "out of memory for native chunks");
        goto done;
    }
    objects[0].name = "armlib", objects[1].name = "index";
    work.objects = objects + 2;
    {
        char note[160];
        unsigned jobs = snx_aot_threads(threads, most, note, sizeof note);
        if (note[0]) fprintf(stderr, "scarletnx-nds: %s\n", note);
        if (jobs > work.chunks) jobs = work.chunks;
        if (jobs < 1) jobs = 1;
        printf("ARM9 AOT: %u chunks of static blocks on %u threads\n", work.chunks, jobs);
        snx_work_lock_init(&work.lock);
        snx_run_workers(jobs, worker, &work);
        snx_work_lock_destroy(&work.lock);
    }
    if (work.failed) {
        snprintf(error, error_size, "%s", work.error);
        goto done;
    }
    snx_arm_aot_report(&work.blocks, stdout);
    if (blocks_made) *blocks_made = work.blocks;
    qsort(work.made, work.made_count, sizeof *work.made, by_place);
    if (native_index(rom, work.made, work.made_count, &objects[1], error, error_size)) goto done;
    objects[0].data = (unsigned char*)snx_armlib_object, objects[0].size = (size_t)snx_armlib_object_size;
    unsigned linking = 2; /* the objects there are */
    for (unsigned i = 2; i < files; ++i)
        if (objects[i].data) {
            const SnxLinkInput object = objects[i];
            objects[i] = (SnxLinkInput){0};
            objects[linking++] = object;
        }
    if (snx_link(objects, linking, exports, 1, snx_path_leaf(path), &linked, error, error_size)) goto done;
    if (snx_write_file(staged, linked.data, linked.size) || plat_replace_file(staged, path)) {
        char why[256];
        snprintf(error, error_size, "cannot write %s: %s", path, snx_os_error(why, sizeof why));
        (void)remove(staged);
        goto done;
    }
    printf("Compiled ARM9 static module: %u blocks in %u objects, %zu bytes\n", work.made_count, linking, linked.size);
    result = 0;
done:
    if (objects)
        for (unsigned i = 1; i < files; ++i) free((void*)objects[i].data);
    free(objects);
    free(linked.data);
    free(entries);
    free(work.made);
    free(staged);
    nds_plans_free(plans, rom->region_count);
    return result;
}
