/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The offline compiler, part of the driver, after the 3DS's (3ds/aot/compiler.c): the blocks discovery found in the
   image's regions, compiled in chunks on a worker per thread, an index of them for each region, and the helpers they
   call, linked by the backend's own linker into the module the run loads. */
#include "nds/aot/nds_module.h"
#include "common/cpu/arm/llvm.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/llvm/link.h"
#include "common/cpu/recompiler/workers.h"
#include "common/platform/file.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
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
static LLVMModuleRef new_module(SnxIr* ir) {
    LLVMModuleRef m = LLVMModuleCreateWithNameInContext("nds-static-aot-v1", ir->ctx);
    LLVMSetTarget(m, LLVMGetTarget(ir->mod));
    LLVMSetDataLayout(m, LLVMGetDataLayoutStr(ir->mod));
    return m;
}
static bool write_object(SnxIr* ir, LLVMModuleRef mod, SnxLinkInput* output, char opt, char* error, size_t size) {
    unsigned char* object = NULL;
    bool ok = !snx_ir_module_object(ir, mod, opt, &object, &output->size, error, size);
    output->data = object;
    return ok;
}

/* The entries' blocks into one object. A block the backend ends sooner than discovery planned goes on as a block of
   its own at the instruction after, appended to *rest for the index */
static bool chunk(SnxIr* ir, const NdsRom* rom, const Entry* entries, unsigned count, SnxLinkInput* output,
                  Entry** rest, unsigned* rest_count, char* error, size_t size) {
    LLVMModuleRef mod = new_module(ir);
    bool ok = false;
    unsigned capacity = 0;
    SnxArmCodegenOptions options = {SNX_ARM_CODEGEN_OPTIONS_ABI, SNX_ARM_PROFILE_ARM946E, SNX_ARM_MEMORY_FLAT, 0, 0};
    *rest = NULL;
    *rest_count = 0;
    for (unsigned i = 0; i < count; ++i) {
        Entry e = entries[i];
        const NdsRegion* r = &rom->regions[e.region];
        while (e.length) {
            unsigned width = e.mode ? 2 : 4;
            uint32_t pc = r->address + e.index * width;
            uint32_t words[NDS_BLOCK_UNITS];
            uint16_t halves[NDS_BLOCK_UNITS];
            for (unsigned j = 0; j < e.length; ++j) {
                const unsigned char* unit = r->data + (e.index + j) * width;
                words[j] = width == 2 ? snx_ld_le16(unit) : snx_ld_le32(unit);
                halves[j] = (uint16_t)words[j];
            }
            char id[48];
            name(id, &e);
            size_t consumed = 0;
            LLVMValueRef f = e.mode ? snx_thumb_llvm_block_ex(mod, id, halves, e.length, pc, &consumed, &options)
                                    : snx_arm_llvm_block_ex(mod, id, words, e.length, pc, &consumed, &options);
            if (!f || !consumed || consumed > e.length) {
                snprintf(error, size, "the backend refused the block at 0x%08x (%zu of %u)", pc, consumed, e.length);
                goto done;
            }
            LLVMSetVisibility(f, LLVMHiddenVisibility);
            if (*rest_count == capacity) {
                Entry* grown = realloc(*rest, (capacity = capacity ? capacity * 2 : 64) * sizeof *grown);
                if (!grown) {
                    snprintf(error, size, "out of memory for static entries");
                    goto done;
                }
                *rest = grown;
            }
            /* every block that was made, with the length the backend gave it */
            (*rest)[(*rest_count)++] = (Entry){e.region, e.index, e.mode, (uint8_t)consumed};
            e.index += (unsigned)consumed;
            e.length = (uint8_t)(e.length - consumed);
        }
    }
    ok = write_object(ir, mod, output, '1', error, size);
done:
    LLVMDisposeModule(mod);
    return ok;
}

static LLVMValueRef constant_array(LLVMModuleRef mod, const char* id, LLVMTypeRef element, LLVMValueRef* values,
                                   unsigned count) {
    LLVMValueRef g = LLVMAddGlobal(mod, LLVMArrayType(element, count), id);
    LLVMSetInitializer(g, LLVMConstArray(element, values, count));
    LLVMSetGlobalConstant(g, 1);
    LLVMSetLinkage(g, LLVMInternalLinkage);
    return g;
}

/* The index: each region's two tables of blocks and of their lengths, the regions, and the descriptor the run reads.
   entries are sorted by region, mode and slot */
static bool index_object(SnxIr* ir, const NdsRom* rom, const Entry* entries, unsigned count, SnxLinkInput* output,
                         char* error, size_t size) {
    LLVMContextRef c = ir->ctx;
    LLVMModuleRef mod = new_module(ir);
    LLVMTargetDataRef layout = LLVMGetModuleDataLayout(mod);
    LLVMTypeRef ptr = LLVMPointerTypeInContext(c, 0), i8 = LLVMInt8TypeInContext(c), i32 = LLVMInt32TypeInContext(c);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(c), ft = LLVMFunctionType(LLVMVoidTypeInContext(c), &ptr, 1, 0);
    LLVMTypeRef region_fields[] = {i32, i32, i64, ptr, ptr, ptr, ptr};
    LLVMTypeRef region_type = LLVMStructTypeInContext(c, region_fields, 7, 0);
    LLVMValueRef* regions = calloc(rom->region_count ? rom->region_count : 1, sizeof *regions);
    unsigned cursor = 0;
    bool ok = false;
    if (!regions || LLVMABISizeOfType(layout, region_type) != sizeof(NdsModuleRegion)) {
        snprintf(error, size, "incompatible native descriptor layout");
        goto done;
    }
    for (unsigned r = 0; r < rom->region_count; ++r) {
        const NdsRegion* region = &rom->regions[r];
        LLVMValueRef tables[2], lengths[2];
        for (unsigned mode = 0; mode < 2; ++mode) {
            unsigned n = region->size / (mode ? 2 : 4);
            LLVMValueRef* functions = malloc((size_t)(n ? n : 1) * sizeof *functions);
            LLVMValueRef* units = malloc((size_t)(n ? n : 1) * sizeof *units);
            if (!functions || !units) {
                free(functions);
                free(units);
                snprintf(error, size, "out of memory for the module's index");
                goto done;
            }
            LLVMValueRef none = LLVMConstNull(ptr), zero = LLVMConstInt(i8, 0, 0);
            for (unsigned i = 0; i < n; ++i) functions[i] = none, units[i] = zero;
            while (cursor < count && entries[cursor].region == r && entries[cursor].mode == mode) {
                const Entry* e = &entries[cursor++];
                char id[48];
                name(id, e);
                if (e->index >= n || LLVMGetNamedFunction(mod, id)) continue;
                functions[e->index] = LLVMAddFunction(mod, id, ft);
                LLVMSetVisibility(functions[e->index], LLVMHiddenVisibility);
                units[e->index] = LLVMConstInt(i8, e->length, 0);
            }
            char id[48];
            snprintf(id, sizeof id, "nds_%u_%s", r, mode ? "thumb" : "arm");
            tables[mode] = constant_array(mod, id, ptr, functions, n);
            snprintf(id, sizeof id, "nds_%u_%s_units", r, mode ? "thumb" : "arm");
            lengths[mode] = constant_array(mod, id, i8, units, n);
            free(functions);
            free(units);
        }
        LLVMValueRef values[] = {LLVMConstInt(i32, region->address, 0), LLVMConstInt(i32, region->size, 0),
                                 LLVMConstInt(i64, region->hash, 0),    tables[0], tables[1], lengths[0], lengths[1]};
        regions[r] = LLVMConstNamedStruct(region_type, values, 7);
    }
    {
        LLVMValueRef list = constant_array(mod, "nds_regions", region_type, regions, rom->region_count);
        LLVMTypeRef fields[] = {i64, i32, i32, i32, i32, i32, i32, i64, ptr};
        LLVMTypeRef type = LLVMStructTypeInContext(c, fields, 9, 0);
        LLVMValueRef values[] = {LLVMConstInt(i64, NDS_MODULE_MAGIC, 0),
                                 LLVMConstInt(i32, NDS_MODULE_ABI, 0),
                                 LLVMConstInt(i32, SNX_ARM_ABI, 0),
                                 LLVMConstInt(i32, sizeof(SnxArmCPU), 0),
                                 LLVMConstInt(i32, sizeof(void*), 0),
                                 LLVMConstInt(i32, SNX_ARM_PROFILE_ARM946E, 0),
                                 LLVMConstInt(i32, rom->region_count, 0),
                                 LLVMConstInt(i64, rom->identity, 0),
                                 list};
        if (LLVMABISizeOfType(layout, type) != sizeof(NdsModule) ||
            LLVMOffsetOfElement(layout, type, 8) != offsetof(NdsModule, regions) ||
            LLVMOffsetOfElement(layout, type, 7) != offsetof(NdsModule, identity)) {
            snprintf(error, size, "incompatible native descriptor layout");
            goto done;
        }
        LLVMValueRef global = LLVMAddGlobal(mod, type, NDS_MODULE_SYMBOL);
        LLVMSetInitializer(global, LLVMConstNamedStruct(type, values, 9));
        LLVMSetGlobalConstant(global, 1);
    }
    ok = write_object(ir, mod, output, '0', error, size);
done:
    free(regions);
    LLVMDisposeModule(mod);
    return ok;
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
    SnxLinkInput* objects;
    unsigned count, chunks, next, finished;
    bool failed;
    Entry* made; /* every block the chunks made */
    unsigned made_count;
    uint64_t output_bytes;
    char error[512];
    SnxWorkLock lock;
} Work;

static void worker(void* arg) {
    Work* w = arg;
    char error[512] = "";
    SnxIr* ir = snx_ir_new("nds-static-aot", '1', error, sizeof error);
    bool ok = ir != NULL;
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
        ok = chunk(ir, w->rom, w->entries + begin, n, &w->objects[i], &made, &made_count, error, sizeof error);
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
            w->output_bytes += w->objects[i].size;
            if (!(++w->finished % 16) || w->finished == w->chunks)
                printf("ARM9 AOT: %u/%u chunks of static blocks emitted\n", w->finished, w->chunks);
        }
        snx_work_unlock(&w->lock);
        free(made);
    }
    snx_ir_free(ir);
}

static int by_place(const void* a, const void* b) {
    const Entry *x = a, *y = b;
    if (x->region != y->region) return x->region < y->region ? -1 : 1;
    if (x->mode != y->mode) return x->mode < y->mode ? -1 : 1;
    return (x->index > y->index) - (x->index < y->index);
}

int nds_aot_compile(const NdsRom* rom, const char* path, const char* hints_path, int threads, char* error,
                    size_t error_size) {
    int result = 1;
    NdsPlan* plans = NULL;
    SnxIr* ir = NULL;
    SnxLinkInput* objects = NULL;
    SnxLinkOutput linked = {NULL, 0};
    unsigned char* armlib = NULL;
    size_t armlib_size = 0, hint_count = 0;
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
    if (snx_ir_init(error, error_size) || !(ir = snx_ir_new("nds-static-aot", '1', error, error_size))) goto done;
    files = (count + CHUNK_ENTRIES - 1) / CHUNK_ENTRIES + 2; /* the helpers, the index, the chunks */
    objects = calloc(files, sizeof *objects);
    if (!objects) {
        snprintf(error, error_size, "out of memory for native chunks");
        goto done;
    }
    objects[0].name = "armlib", objects[1].name = "index";
    for (unsigned i = 2; i < files; ++i) objects[i].name = "blocks";
    work = (Work){.rom = rom, .entries = entries, .objects = objects + 2, .count = count, .chunks = files - 2};
    {
        char note[160];
        unsigned jobs = snx_aot_threads(threads, 0, note, sizeof note);
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
    qsort(work.made, work.made_count, sizeof *work.made, by_place);
    if (!index_object(ir, rom, work.made, work.made_count, &objects[1], error, error_size)) goto done;
    if (snx_ir_armlib_object('1', &armlib, &armlib_size, error, error_size)) goto done;
    objects[0].data = armlib, objects[0].size = armlib_size;
    if (snx_link(objects, files, exports, 1, snx_path_leaf(path), &linked, error, error_size)) goto done;
    if (snx_write_file(staged, linked.data, linked.size) || plat_replace_file(staged, path)) {
        char why[256];
        snprintf(error, error_size, "cannot write %s: %s", path, snx_os_error(why, sizeof why));
        (void)remove(staged);
        goto done;
    }
    printf("Compiled ARM9 static module: %u blocks in %u objects, %zu bytes\n", work.made_count, files, linked.size);
    result = 0;
done:
    if (objects)
        for (unsigned i = 1; i < files; ++i) free((void*)objects[i].data);
    free(objects);
    free(armlib);
    free(linked.data);
    free(entries);
    free(work.made);
    free(staged);
    snx_ir_free(ir);
    nds_plans_free(plans, rom->region_count);
    return result;
}
