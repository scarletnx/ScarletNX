/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The offline compiler, part of the driver: the extracted image's static blocks to a native module. Never creates an
   execution engine. */
#include "3ds/aot/ctr_compile.h"
#include "3ds/aot/ctr_module.h"
#include "3ds/aot/ctr_discover.h"
#include "common/cpu/arm/llvm.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/llvm/link.h"
#include "common/cpu/recompiler/workers.h"
#include "common/utilities/common.h"
#include "common/platform/file.h"
#include "common/utilities/endian.h"
#include "snx_settings.h"
#include <llvm-c/Core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(SNX_ARM_CODEGEN_OPTIONS_ABI == SNX_CTR_MODULE_CODEGEN_ABI, "codegen ABI");
_Static_assert(SNX_ARM_MEMORY_CHECKED == SNX_CTR_MODULE_CHECKED_MODE, "memory ABI");
_Static_assert(SNX_ARM_MEMORY_HOSTED == SNX_CTR_MODULE_HOSTED_MODE, "memory ABI");
#define CHUNK_ENTRIES 512u
#define OBJECT_LIMIT  (1024u * 1024u * 1024u) /* a commercial title's 330000 blocks take about 300 MiB */
#define MODULE_LIMIT  (1024u * 1024u * 1024u)
typedef struct {
    unsigned mode, index, length;
} Entry;
/* a path's file name */
static void name(char out[40], unsigned mode, unsigned index) {
    snprintf(out, 40, "ctr_%c_%u", mode ? 't' : 'a', index);
}
static LLVMValueRef table(LLVMModuleRef mod, const char* id, LLVMValueRef* functions, unsigned count) {
    LLVMTypeRef ptr = LLVMPointerTypeInContext(LLVMGetModuleContext(mod), 0);
    LLVMValueRef result = LLVMAddGlobal(mod, LLVMArrayType(ptr, count), id);
    LLVMSetInitializer(result, LLVMConstArray(ptr, functions, count));
    LLVMSetGlobalConstant(result, 1);
    LLVMSetLinkage(result, LLVMInternalLinkage);
    return result;
}
static bool descriptor(LLVMModuleRef mod, LLVMTargetDataRef layout, const SnxCtrImage* image, LLVMValueRef arm,
                       LLVMValueRef thumb) {
    LLVMContextRef c = LLVMGetModuleContext(mod);
    LLVMTypeRef i8 = LLVMInt8TypeInContext(c), i32 = LLVMInt32TypeInContext(c);
    LLVMTypeRef types[18];
    LLVMValueRef values[18], hash[32];
    types[0] = LLVMInt64TypeInContext(c);
    values[0] = LLVMConstInt(types[0], SNX_CTR_MODULE_MAGIC, 0);
    uint32_t integers[] = {SNX_CTR_MODULE_ABI,
                           sizeof(SnxCtrModule),
                           sizeof(SnxArmCPU),
                           sizeof(SnxArmCheckedCPU),
                           SNX_ARM_ABI,
                           SNX_ARM_CHECKED_ABI,
                           SNX_ARM_CODEGEN_OPTIONS_ABI,
                           SNX_ARM_PROFILE_ARM11,
                           snx_ctr_module_memory(),
                           sizeof(void*),
                           image->segment[0].address,
                           image->segment[0].bytes,
                           image->segment[0].bytes / 4,
                           image->segment[0].bytes / 2};
    const size_t offsets[] = {offsetof(SnxCtrModule, magic),           offsetof(SnxCtrModule, abi),
                              offsetof(SnxCtrModule, descriptor_size), offsetof(SnxCtrModule, cpu_size),
                              offsetof(SnxCtrModule, checked_size),    offsetof(SnxCtrModule, arm_abi),
                              offsetof(SnxCtrModule, checked_abi),     offsetof(SnxCtrModule, codegen_abi),
                              offsetof(SnxCtrModule, profile),         offsetof(SnxCtrModule, memory_mode),
                              offsetof(SnxCtrModule, pointer_size),    offsetof(SnxCtrModule, text_address),
                              offsetof(SnxCtrModule, text_bytes),      offsetof(SnxCtrModule, arm_count),
                              offsetof(SnxCtrModule, thumb_count),     offsetof(SnxCtrModule, image_identity),
                              offsetof(SnxCtrModule, arm_blocks),      offsetof(SnxCtrModule, thumb_blocks)};
    for (unsigned i = 0; i < 14; ++i) {
        types[i + 1] = i32;
        values[i + 1] = LLVMConstInt(i32, integers[i], 0);
    }
    types[15] = LLVMArrayType(i8, 32);
    for (unsigned i = 0; i < 32; ++i) hash[i] = LLVMConstInt(i8, image->identity[i], 0);
    values[15] = LLVMConstArray(i8, hash, 32);
    types[16] = types[17] = LLVMPointerTypeInContext(c, 0);
    values[16] = arm;
    values[17] = thumb;
    LLVMTypeRef structure = LLVMStructTypeInContext(c, types, 18, 0);
    if (LLVMABISizeOfType(layout, structure) != sizeof(SnxCtrModule)) return false;
    for (unsigned i = 0; i < 18; ++i)
        if (LLVMOffsetOfElement(layout, structure, i) != offsets[i]) return false;
    LLVMValueRef global = LLVMAddGlobal(mod, structure, "snx_ctr_module_v1");
    LLVMSetInitializer(global, LLVMConstNamedStruct(structure, values, 18));
    LLVMSetGlobalConstant(global, 1);
    return true;
}
/* a module made for this machine as the backend makes the Switch's (ir.c): the target, CPU and ABI the helpers
   (armlib.c) are compiled for, which the module is linked with */
static LLVMModuleRef new_module(SnxIr* ir) {
    LLVMModuleRef m = LLVMModuleCreateWithNameInContext("ctr-static-aot-v1", ir->ctx);
    LLVMSetTarget(m, LLVMGetTarget(ir->mod));
    LLVMSetDataLayout(m, LLVMGetDataLayoutStr(ir->mod));
    return m;
}
/* the module's IR as the backend made it, before its passes, as dir/id.ll (--keep-ir) */
static bool save_ir(LLVMModuleRef mod, const char* dir, const char* id, char* error, size_t size) {
    char *text = LLVMPrintModuleToString(mod), *file = snx_format("%s.ll", id);
    char* path = file ? snx_path_join(dir, file) : NULL;
    bool ok = path && !snx_write_file(path, text, strlen(text));
    if (!ok) {
        char why[256];
        snprintf(error, size, "cannot save the IR of %s in %s: %s", id, dir, snx_os_error(why, sizeof why));
    }
    LLVMDisposeMessage(text);
    free(file);
    free(path);
    return ok;
}
/* the module's object, *output's, from the backend's passes at the -O letter opt */
static bool write_object(SnxIr* ir, LLVMModuleRef mod, SnxLinkInput* output, char opt, char* error, size_t size) {
    unsigned char* object = NULL;
    bool ok = !snx_ir_module_object(ir, mod, opt, &object, &output->size, error, size);
    output->data = object;
    return ok;
}
/* The entries' blocks into one object, at the -O letter opt, its IR kept in the folder keep_ir names as id.ll. A block
   the backend ends sooner than discovery planned (at an MSR to the CPSR, which it ends blocks on) goes on as a block of
   its own at the instruction after, appended to *rest (*rest_count of them, the caller's to free) for the index */
static bool chunk(SnxIr* ir, const SnxCtrImage* image, const Entry* entries, unsigned count, SnxLinkInput* output,
                  char opt, const char* keep_ir, const char* id, Entry** rest, unsigned* rest_count, char* error,
                  size_t size) {
    LLVMModuleRef mod = new_module(ir);
    bool ok = false;
    unsigned capacity = 0;
    SnxArmCodegenOptions options = {SNX_ARM_CODEGEN_OPTIONS_ABI, SNX_ARM_PROFILE_ARM11,
                                    (SnxArmMemoryMode)snx_ctr_module_memory(), 0, 0};
    *rest = NULL;
    *rest_count = 0;
    for (unsigned i = 0; i < count; ++i) {
        Entry e = entries[i];
        while (e.length) {
            unsigned width = e.mode ? 2 : 4;
            uint32_t pc = image->segment[0].address + e.index * width;
            uint32_t words[SNX_CTR_AOT_BLOCK_WORDS];
            uint16_t halves[SNX_CTR_AOT_BLOCK_WORDS];
            for (unsigned j = 0; j < e.length; ++j) {
                const unsigned char* unit = image->code + e.index * width + j * width;
                words[j] = width == 2 ? snx_ld_le16(unit) : snx_ld_le32(unit);
                halves[j] = (uint16_t)words[j];
            }
            char id[40];
            name(id, e.mode, e.index);
            size_t consumed = 0;
            LLVMValueRef f = e.mode ? snx_thumb_llvm_block_ex(mod, id, halves, e.length, pc, &consumed, &options)
                                    : snx_arm_llvm_block_ex(mod, id, words, e.length, pc, &consumed, &options);
            if (!f || !consumed || consumed > e.length) {
                snprintf(error, size, "static discovery/backend termination mismatch at 0x%08x (%zu/%u)", pc,
                         consumed, e.length);
                goto done;
            }
            LLVMSetVisibility(f, LLVMHiddenVisibility);
            if (e.index != entries[i].index) {
                if (*rest_count == capacity) {
                    Entry* grown = realloc(*rest, (capacity = capacity ? capacity * 2 : 16) * sizeof *grown);
                    if (!grown) {
                        snprintf(error, size, "out of memory for static entries");
                        goto done;
                    }
                    *rest = grown;
                }
                (*rest)[(*rest_count)++] = (Entry){e.mode, e.index, (unsigned)consumed};
            }
            e.index += (unsigned)consumed;
            e.length -= (unsigned)consumed;
        }
    }
    ok = (!keep_ir || save_ir(mod, keep_ir, id, error, size)) && write_object(ir, mod, output, opt, error, size);
done:
    LLVMDisposeModule(mod);
    return ok;
}
static bool index_object(SnxIr* ir, const SnxCtrImage* image, const Entry* entries, unsigned count,
                         SnxLinkInput* output, const char* keep_ir, char* error, size_t size) {
    LLVMContextRef c = ir->ctx;
    LLVMModuleRef mod = new_module(ir);
    LLVMTypeRef ptr = LLVMPointerTypeInContext(c, 0), ft = LLVMFunctionType(LLVMVoidTypeInContext(c), &ptr, 1, 0);
    LLVMValueRef tables[2];
    unsigned cursor = 0;
    bool ok = false;
    for (unsigned mode = 0; mode < 2; ++mode) {
        unsigned n = image->segment[0].bytes / (mode ? 2 : 4);
        LLVMValueRef* functions = malloc((size_t)n * sizeof *functions);
        if (!functions) {
            snprintf(error, size, "out of memory for static index");
            goto done;
        }
        for (unsigned i = 0; i < n; ++i) functions[i] = LLVMConstNull(ptr);
        while (cursor < count && entries[cursor].mode == mode) {
            const Entry* e = entries + cursor++;
            char id[40];
            name(id, mode, e->index);
            functions[e->index] = LLVMAddFunction(mod, id, ft);
            LLVMSetVisibility(functions[e->index], LLVMHiddenVisibility);
        }
        tables[mode] = table(mod, mode ? "ctr_thumb" : "ctr_arm", functions, n);
        free(functions);
    }
    if (!descriptor(mod, LLVMGetModuleDataLayout(mod), image, tables[0], tables[1])) {
        snprintf(error, size, "incompatible native descriptor layout");
        goto done;
    }
    ok = (!keep_ir || save_ir(mod, keep_ir, "index", error, size)) && write_object(ir, mod, output, '0', error, size);
done:
    LLVMDisposeModule(mod);
    return ok;
}
/* A title's hints (snx_ctr_jit_save_hints), at path: the blocks earlier runs compiled at run time, one pc a line in
   hex, | 1 for Thumb. *count of them, the caller's to free; none (NULL) without the file */
static uint32_t* read_hints(const char* path, size_t* count) {
    FILE* f = path ? fopen(path, "r") : NULL;
    uint32_t* hints = NULL;
    size_t capacity = 0;
    unsigned value;
    *count = 0;
    while (f && fscanf(f, "%x", &value) == 1) {
        if (*count == capacity) {
            uint32_t* grown = realloc(hints, (capacity = capacity ? capacity * 2 : 1024) * sizeof *grown);
            if (!grown) break;
            hints = grown;
        }
        hints[(*count)++] = value;
    }
    if (f) fclose(f);
    return hints;
}
/* a chunk's place for its progress line: the addresses its blocks span and how many there are */
static void chunk_label(const SnxCtrImage* image, const Entry* entries, unsigned count, char* out, size_t size) {
    uint32_t lo = UINT32_MAX, hi = 0;
    for (unsigned i = 0; i < count; ++i) {
        unsigned width = entries[i].mode ? 2 : 4;
        uint32_t pc = image->segment[0].address + entries[i].index * width;
        if (pc < lo) lo = pc;
        if (pc + entries[i].length * width > hi) hi = pc + entries[i].length * width;
    }
    if (lo > hi) lo = hi = 0;
    snprintf(out, size, "0x%08X-0x%08X, %u block%s", (unsigned)lo, (unsigned)hi, count, count == 1 ? "" : "s");
}
/* The chunks shared out to a worker per host core, each with an SnxIr of its own: a commercial title's few hundred
   thousand blocks take an hour on one */
typedef struct {
    const SnxCtrImage* image;
    const Entry* entries;
    const unsigned* starts; /* chunk i is entries starts[i] to starts[i + 1] */
    SnxLinkInput* objects;  /* chunk i's object is objects[i] */
    unsigned count, chunks, next, finished;
    char opt;            /* the -O letter */
    const char* keep_ir; /* where each chunk's IR goes, NULL for nowhere */
    bool quiet, failed;
    Entry* rest; /* the blocks the backend split off, every chunk's */
    unsigned rest_count;
    uint64_t output_bytes;
    char error[512];
    SnxWorkLock lock;
} Work;
static void worker(void* arg) {
    Work* w = arg;
    char error[512] = "";
    SnxIr* ir = snx_ir_new("ctr-static-aot", w->opt, error, sizeof error);
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
        unsigned begin = w->starts[i], n = w->starts[i + 1] - begin;
        Entry* rest = NULL;
        unsigned rest_count = 0;
        char id[24];
        snprintf(id, sizeof id, "blocks_%03u", i);
        double start = snx_seconds();
        ok = chunk(ir, w->image, w->entries + begin, n, &w->objects[i], w->opt, w->keep_ir, id, &rest, &rest_count,
                   error, sizeof error);
        double seconds = snx_seconds() - start;
        snx_work_lock(&w->lock);
        Entry* merged = rest_count ? realloc(w->rest, (w->rest_count + rest_count) * sizeof *merged) : w->rest;
        if (ok && rest_count && !merged) {
            ok = false;
            snprintf(error, sizeof error, "out of memory for static entries");
        }
        if (ok) {
            w->rest = merged;
            if (rest_count) memcpy(w->rest + w->rest_count, rest, rest_count * sizeof *rest);
            w->rest_count += rest_count;
            w->output_bytes += w->objects[i].size;
            /* a line for each chunk, as the Switch's recompile gives one for each unit, + the index */
            char label[64];
            chunk_label(w->image, w->entries + begin, n, label, sizeof label);
            ++w->finished;
            if (!w->quiet) snx_report_unit(w->finished, w->chunks + 1, id, label, 0, seconds);
        }
        snx_work_unlock(&w->lock);
        free(rest);
    }
    snx_ir_free(ir);
}
/* on the threads the settings aot_threads and aot_threads_most give, as the Switch's recompile (snx_aot_threads) */
static bool compile_chunks(Work* w) {
    char note[160];
    unsigned jobs = snx_aot_threads(g_settings.aot_threads, g_settings.aot_threads_most, note, sizeof note);
    if (note[0]) fprintf(stderr, "scarletnx-3ds: %s\n", note);
    if (jobs > w->chunks) jobs = w->chunks;
    if (jobs < 1) jobs = 1;
    if (!w->quiet) printf("ARM11 AOT: %u chunks of static blocks on %u threads\n", w->chunks, jobs);
    snx_work_lock_init(&w->lock);
    snx_run_workers(jobs, worker, w);
    snx_work_lock_destroy(&w->lock);
    return !w->failed;
}
static int by_place(const void* a, const void* b) {
    const Entry *x = a, *y = b;
    if (x->mode != y->mode) return x->mode < y->mode ? -1 : 1;
    return (x->index > y->index) - (x->index < y->index);
}
/* where each chunk starts in entries, + count after the last, into starts (count + 1 of them at most): chunk instructions
   to a chunk, whole blocks, or CHUNK_ENTRIES blocks for 0. The chunks' count */
static unsigned cut_chunks(const Entry* entries, unsigned count, int chunk, unsigned* starts) {
    unsigned chunks = 0, at = 0;
    while (at < count) {
        starts[chunks++] = at;
        if (chunk) {
            size_t instructions = 0;
            do instructions += entries[at++].length;
            while (at < count && instructions < (size_t)chunk);
        } else
            at = count - at > CHUNK_ENTRIES ? at + CHUNK_ENTRIES : count;
    }
    starts[chunks] = count;
    return chunks;
}
/* The module: the blocks' chunks, the index and the helpers they call (armlib.c), linked by the backend's own linker
   into the shared library the runtime loads, which exports the descriptor alone */
int snx_ctr_aot_compile(const char* extracted, const SnxCtrSource* game, const SnxCliRecompile* asked, char* error,
                        size_t error_size) {
    int result = 1;
    SnxCtrImage image = {0}, check = {0};
    SnxCtrPlan plan = {0};
    SnxIr* ir = NULL;
    SnxLinkInput* objects = NULL;
    SnxLinkOutput linked = {NULL, 0};
    unsigned char* armlib = NULL;
    size_t armlib_size = 0;
    char *directory = NULL, *target = NULL, *staged = NULL, *previous = NULL, *kept = NULL, *recorded = NULL;
    const char* exports[1] = {"snx_ctr_module_v1"};
    Entry* entries = NULL;
    unsigned count = 0, files = 0, *starts = NULL;
    Work work = {0};
    const char opt = g_settings.optimization_level[0]; /* the setting opt's, which -O sets */
    bool quiet = asked && asked->quiet;
    if (error_size) error[0] = 0;
    if (!(game ? snx_ctr_image_from_source(game, &image, error, error_size)
              : snx_ctr_image_load(extracted, &image, error, error_size)))
        goto done;
    if (image.segment[0].bytes > SNX_CTR_AOT_TEXT_LIMIT) {
        snprintf(error, error_size, "static AOT text limit is %u bytes", SNX_CTR_AOT_TEXT_LIMIT);
        result = 3;
        goto done;
    }
    directory = snx_absolute(extracted);
    if (!directory) {
        char why[256];
        snprintf(error, error_size, "cannot resolve %s: %s", extracted, snx_os_error(why, sizeof why));
        goto done;
    }
    target = snx_ctr_game_file(directory, SNX_CTR_MODULE_SUFFIX);
    staged = target ? snx_format("%s.tmp", target) : NULL;
    if (!staged) {
        snprintf(error, error_size, "out of memory");
        goto done;
    }
    /* --keep-ir's folder, <title ID>_build, as a Switch game's */
    if (asked && asked->keep_ir) {
        kept = snx_ctr_game_file(directory, "_build");
        if (!kept || snx_mkdirs(kept)) {
            char why[256];
            snprintf(error, error_size, "cannot make the folder of the IR --keep-ir keeps, %s: %s",
                     kept ? kept : "(out of memory)", snx_os_error(why, sizeof why));
            goto done;
        }
    }
    bool exhaustive = image.segment[0].bytes <= SNX_CTR_AOT_EXHAUSTIVE_LIMIT;
    unsigned capacity = image.segment[0].bytes / 4 + image.segment[0].bytes / 2;
    if (asked && asked->jit_only) { /* a module of no blocks: the run-time compiler compiles every one it reaches */
        if (!quiet) puts("No code ahead of time (--jit-only): the run-time compiler compiles every block");
        entries = calloc(1, sizeof *entries);
    } else {
        /* the blocks earlier runs compiled at run time: the title's own record, or another --jit-cache names */
        size_t hint_count = 0;
        recorded =
            asked && asked->jit_cache ? snx_strdup(asked->jit_cache) : snx_ctr_game_file(directory, SNX_CTR_HINTS);
        if (asked && asked->jit_cache && !snx_is_file(recorded)) {
            snprintf(error, error_size,
                     "--jit-cache %s: no such file, a list of the blocks a run compiled (<title ID>%s)",
                     asked->jit_cache, SNX_CTR_HINTS);
            goto done;
        }
        uint32_t* hints = read_hints(recorded, &hint_count);
        if (hint_count && !quiet)
            printf("Seeding ARM11 discovery with %zu blocks earlier runs compiled at run time\n", hint_count);
        bool discovered = exhaustive || snx_ctr_discover(&image, hints, hint_count, &plan, error, error_size);
        free(hints);
        if (!discovered) {
            result = 3;
            goto done;
        }
        entries = calloc(exhaustive ? capacity : plan.entries, sizeof *entries);
    }
    if (!entries) {
        snprintf(error, error_size, "out of memory for static entries");
        goto done;
    }
    for (unsigned mode = 0; !(asked && asked->jit_only) && mode < 2; ++mode) {
        unsigned width = mode ? 2 : 4;
        for (unsigned i = 0; i < image.segment[0].bytes / width; ++i) {
            unsigned length = exhaustive ? 1 : snx_ctr_plan_length(&plan, image.segment[0].address + i * width, !!mode);
            if (length) entries[count++] = (Entry){mode, i, length};
        }
    }
    if (!quiet)
        printf("Discovered ARM11 static coverage: %u blocks%s\n", count,
               exhaustive ? " (exhaustive tiny image)" : " (conservative CFG)");
    if (snx_ir_init(error, error_size) || !(ir = snx_ir_new("ctr-static-aot", opt, error, error_size))) goto done;
    starts = malloc(((size_t)count + 1) * sizeof *starts);
    files = starts ? cut_chunks(entries, count, asked ? asked->chunk : 0, starts) + 2 : 0; /* helpers, index, chunks */
    objects = files ? calloc(files, sizeof *objects) : NULL;
    if (!objects) {
        snprintf(error, error_size, "out of memory for native chunks");
        goto done;
    }
    objects[0].name = "armlib", objects[1].name = "index";
    for (unsigned i = 2; i < files; ++i) objects[i].name = "blocks";
    work = (Work){.image = &image,
                  .entries = entries,
                  .starts = starts,
                  .objects = objects + 2,
                  .count = count,
                  .chunks = files - 2,
                  .opt = opt,
                  .keep_ir = kept,
                  .quiet = quiet};
    if (!compile_chunks(&work)) {
        snprintf(error, error_size, "%s", work.error);
        goto done;
    }
    /* the index of every block, the ones the backend split off with the planned ones */
    if (work.rest_count) {
        Entry* all = realloc(entries, (count + work.rest_count) * sizeof *all);
        if (!all) {
            snprintf(error, error_size, "out of memory for static entries");
            goto done;
        }
        entries = all;
        memcpy(entries + count, work.rest, work.rest_count * sizeof *entries);
        count += work.rest_count;
        qsort(entries, count, sizeof *entries, by_place);
    }
    double start = snx_seconds();
    if (!index_object(ir, &image, entries, count, &objects[1], kept, error, error_size)) goto done;
    if (!quiet) {
        char label[48];
        snprintf(label, sizeof label, "the module's index, %u block%s", count, count == 1 ? "" : "s");
        snx_report_unit(work.chunks + 1, work.chunks + 1, "index", label, 0, snx_seconds() - start);
    }
    if (snx_ir_armlib_object(opt, &armlib, &armlib_size, error, error_size)) goto done;
    objects[0].data = armlib, objects[0].size = armlib_size;
    uint64_t output_bytes = work.output_bytes + objects[1].size;
    if (output_bytes > OBJECT_LIMIT || armlib_size > OBJECT_LIMIT - output_bytes) {
        snprintf(error, error_size, "native object output limit exceeded including semantic helpers");
        result = 3;
        goto done;
    }
    if (snx_link(objects, files, exports, 1, snx_path_leaf(target), &linked, error, error_size)) goto done;
    if (linked.size > MODULE_LIMIT) {
        snprintf(error, error_size, "native module output limit exceeded");
        result = 3;
        goto done;
    }
    /* the objects go before the image is read again: a commercial title's are as large as the module */
    for (unsigned i = 1; i < files; ++i) {
        free((void*)objects[i].data);
        objects[i].data = NULL;
    }
    if (!(game ? snx_ctr_image_from_source(game, &check, error, error_size)
              : snx_ctr_image_load(directory, &check, error, error_size)))
        goto done; /* its own error says why */
    if (memcmp(image.identity, check.identity, 32)) {
        snprintf(error, error_size, "extracted image changed during compilation");
        goto done;
    }
    /* A module from an earlier compile is replaced only once the new one is complete, by a rename over it, so a
       failed publish leaves what was there. On Windows a DLL a running copy of the game has loaded cannot be
       replaced. */
    /* a module Android can map: its text is loaded executable, which its file must allow there */
    if (snx_write_file(staged, linked.data, linked.size) || snx_make_executable(staged) ||
        plat_replace_file(staged, target)) {
        char why[256];
        snprintf(error, error_size, "cannot write %s: %s", target, snx_os_error(why, sizeof why));
        (void)remove(staged);
        goto done;
    }
    /* an older version's module, in native/ */
    previous = snx_path_join(directory, "native");
    if (previous && snx_is_dir(previous)) (void)snx_remove_tree(previous);
    if (!quiet) printf("Compiled ARM11 static module: %u blocks in %u objects\n", count, files);
    result = 0;
done:
    if (objects)
        for (unsigned i = 1; i < files; ++i) free((void*)objects[i].data);
    free(objects);
    free(armlib);
    free(linked.data);
    free(entries);
    free(starts);
    free(kept);
    free(recorded);
    free(work.rest);
    free(staged);
    free(target);
    free(previous);
    free(directory);
    snx_ir_free(ir);
    snx_ctr_plan_free(&plan);
    snx_ctr_image_free(&check);
    snx_ctr_image_free(&image);
    return result;
}
