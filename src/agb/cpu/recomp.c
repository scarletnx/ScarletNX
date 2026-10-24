/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "agb/cpu/recomp.h"
#include "agb/board/board.h"
#include "common/utilities/common.h"
#include "common/cpu/arm/llvm.h"
#include "agb/cpu/runtime.h"
#ifdef SNX_AGB_JIT
#include "common/cpu/arm/exec.h"
#endif
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define AGB_RECOMP_ABI 5   /* accesses bound by the window and its guard, not by 4 GiB */
#define AGB_BLOCK_MAX  256 /* words compiled per block at most */

/* the ARM7TDMI's rules (arm.h) on the flat window: the A57 default would run the ROM as ARMv8-A */
static const SnxArmCodegenOptions agb_codegen = {SNX_ARM_CODEGEN_OPTIONS_ABI, SNX_ARM_PROFILE_ARM7TDMI,
                                                 SNX_ARM_MEMORY_FLAT};

void agb_recomp_init(AgbRecomp* recomp, const char* base, const char* cc) {
    memset(recomp, 0, sizeof(*recomp));
    recomp->base = (char*)malloc(strlen(base) + 1);
    if (recomp->base) strcpy(recomp->base, base);
    recomp->cc = cc;
}

static void close_module(void* handle) {
#ifdef _WIN32
    FreeLibrary((HMODULE)handle);
#else
    dlclose(handle);
#endif
}

/* The modules of the blocks compiled at run time go away with their files: the cached module takes their
   blocks when the run is saved. */
static void close_patches(AgbRecomp* recomp) {
    char* path = recomp->base ? (char*)malloc(strlen(recomp->base) + 32) : NULL;
    for (size_t i = 0; i < recomp->patch_count; ++i) {
        close_module(recomp->patches[i]);
        if (path) {
            sprintf(path, "%s.%zu.so", recomp->base, i + 1);
            remove(path);
        }
    }
    free(path);
    free(recomp->patches);
    recomp->patches = NULL;
    recomp->patch_count = 0;
}

void agb_recomp_free(AgbRecomp* recomp) {
    close_patches(recomp);
#ifdef SNX_AGB_JIT
    snx_arm_exec_free(recomp->exec);
#endif
    if (recomp->handle) close_module(recomp->handle);
    free(recomp->blocks);
    free(recomp->base);
    memset(recomp, 0, sizeof(*recomp));
}

static void block_name(char* out, size_t size, size_t index) { snprintf(out, size, "snx_agb_%zu", index); }

/* FNV-1a over size bytes, from hash on: what tells one ROM, or one block's code, from another */
static uint64_t fnv(uint64_t hash, const void* bytes, size_t size) {
    const uint8_t* p = (const uint8_t*)bytes;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ p[i]) * 0x100000001B3u;
    return hash;
}
#define AGB_FNV_BASIS 0xCBF29CE484222325u

static uint64_t rom_hash(const AgbBoard* board) {
    return board->has_cartridge ? fnv(AGB_FNV_BASIS, board->cartridge.data, board->cartridge.size) : AGB_FNV_BASIS;
}

/* The BIOS and the cartridge windows hold the same code for the whole run; the game writes over the rest */
static int fixed(uint32_t pc) {
    uint32_t region = pc >> 24 & 0x0F;
    return region == 0x00 || (region >= 0x08 && region <= 0x0D);
}

/* the hash of the count words from pc on, as memory holds them now */
static uint64_t code_hash(AgbBoard* board, uint32_t pc, int thumb, size_t count) {
    if (thumb) {
        uint16_t words[AGB_BLOCK_MAX];
        for (size_t j = 0; j < count; ++j) words[j] = agb_read16(board, pc + (uint32_t)j * 2);
        return fnv(AGB_FNV_BASIS, words, count * sizeof(*words));
    } else {
        uint32_t words[AGB_BLOCK_MAX];
        for (size_t j = 0; j < count; ++j) words[j] = agb_read32(board, pc + (uint32_t)j * 4);
        return fnv(AGB_FNV_BASIS, words, count * sizeof(*words));
    }
}

static void* module_symbol(void* handle, size_t index) {
    char name[32];
    block_name(name, sizeof(name), index);
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)handle, name);
#else
    return dlsym(handle, name);
#endif
}

/* ---- block discovery ---- */

static AgbBlock* find(AgbRecomp* recomp, uint32_t pc, int thumb) {
    /* index 0 is none: the project's rule for indices */
    for (size_t i = 1; i <= recomp->count; ++i)
        if (recomp->blocks[i].pc == pc && recomp->blocks[i].thumb == (uint8_t)thumb) return &recomp->blocks[i];
    return NULL;
}

/* Takes a block out: an index names a block's function only from a module's emit to its open, which every block
   past it already went through */
static void drop(AgbRecomp* recomp, AgbBlock* block) {
    size_t i = (size_t)(block - recomp->blocks);
    memmove(block, block + 1, (recomp->count - i) * sizeof(*block));
    --recomp->count;
    recomp->dirty = 1;
}

static AgbBlock* add(AgbRecomp* recomp, uint32_t pc, int thumb) {
    AgbBlock* block = find(recomp, pc, thumb);
    if (block) return block;
    if (recomp->count + 1 >= recomp->capacity) {
        size_t capacity = recomp->capacity ? recomp->capacity * 2 : 257;
        AgbBlock* grown = (AgbBlock*)realloc(recomp->blocks, capacity * sizeof(*grown));
        if (!grown) return NULL;
        recomp->blocks = grown;
        recomp->capacity = capacity;
    }
    block = &recomp->blocks[++recomp->count];
    memset(block, 0, sizeof(*block));
    block->pc = pc;
    block->thumb = (uint8_t)thumb;
    recomp->dirty = 1;
    return block;
}

static int32_t thumb_target(uint32_t pc, uint16_t hw1, uint16_t hw2, unsigned op) {
    if (op == BR_B) return (int32_t)(pc + 4 + (uint32_t)((int32_t)((uint32_t)(hw1 & 0x7FF) << 21) >> 20));
    if (op == BR_BCOND) return (int32_t)(pc + 4 + (uint32_t)((int32_t)((uint32_t)(hw1 & 0xFF) << 24) >> 23));
    if (op == BR_CBZ || op == BR_CBNZ)
        return (int32_t)(pc + 4 + ((((hw1 >> 9) & 1) << 5) | ((hw1 >> 3) & 31)) * 2);
    /* the Thumb32 forms spread the offset over both halfwords */
    {
        unsigned s = hw1 >> 10 & 1, i1 = (hw2 >> 13 & 1) ^ 1 ^ s, i2 = (hw2 >> 11 & 1) ^ 1 ^ s;
        uint32_t raw = s << 24 | i1 << 23 | i2 << 22 | (hw1 & 0x3FF) << 12 | (hw2 & 0x7FF) << 1;
        return (int32_t)(pc + 4 + (uint32_t)((int32_t)(raw << 7) >> 7));
    }
}

/* The walk mirrors the recompiler's block ends: a control transfer (conditional ones too), a trap, or an
   undecodable instruction. Successors become block starts of their own. The walk is an explicit work
   list: the recursive form could chase a branch chain tail first without ever growing the stack. */
static void discover(AgbRecomp* recomp, AgbBoard* board, uint32_t pc, int thumb) {
    struct Work {
        uint32_t pc;
        int thumb;
    } work[1024];
    size_t count = 0, budget = 65536; /* the walk ends when the list empties or the budget runs out */
    work[count].pc = pc;
    work[count++].thumb = thumb;
    while (count > 0) {
        struct Work start = work[count - 1];
        --count;
        if (find(recomp, start.pc, start.thumb) || !add(recomp, start.pc, start.thumb)) continue;
        if (!--budget) return; /* whatever is left is compiled when it is first dispatched */
#define PUSH(to_pc, to_thumb)                                                                                        \
    do {                                                                                                             \
        if (count < sizeof(work) / sizeof(work[0])) {                                                                \
            work[count].pc = (uint32_t)(to_pc);                                                                      \
            work[count++].thumb = (to_thumb);                                                                        \
        }                                                                                                            \
    } while (0)
        if (start.thumb) {
            for (uint32_t p = start.pc, n = 0; n < AGB_BLOCK_MAX / 2; ++n) {
                uint16_t hw1 = agb_read16(board, p), hw2 = agb_read16(board, p + 2);
                SnxThumb t;
                unsigned len = snx_thumb_decode_profile(agb_codegen.profile, hw1, hw2, &t);
                if (!len) break;
                if (t.shape == TH_BRANCH) {
                    int32_t target = thumb_target(p, hw1, hw2, t.op);
                    int fallthrough = t.op == BR_BL || t.op == BR_BLX_IMM || t.op == BR_BCOND || t.op == BR_CBZ ||
                                      t.op == BR_CBNZ;
                    if (t.op == BR_B || t.op == BR_BCOND || t.op == BR_CBZ || t.op == BR_CBNZ || t.op == BR_BL ||
                        t.op == BR_BLX_IMM || t.op == BR_BW)
                        PUSH(target, t.op != BR_BLX_IMM);
                    if (fallthrough) PUSH(p + len * 2, 1);
                    break;
                }
                if (t.shape == TH_TABLE || t.shape == TH_IT) break;
                p += len * 2;
            }
        } else {
            for (uint32_t p = start.pc, n = 0; n < AGB_BLOCK_MAX; ++n, p += 4) {
                uint32_t w = agb_read32(board, p);
                SnxA32Kind kind = snx_a32_decode_profile(agb_codegen.profile, w);
                unsigned cond = w >> 28;
                if (kind == SNX_A32_BRANCH) {
                    uint32_t target = p + 8 + (uint32_t)((int32_t)(w << 8) >> 6);
                    PUSH(target, cond == 15); /* BLX lands in Thumb */
                    if ((w & (1u << 24)) || cond != 14) PUSH(p + 4, 0);
                    break;
                }
                if (kind == SNX_A32_BLX_IMM) {
                    uint32_t target = p + 8 + (uint32_t)((int32_t)(w << 8) >> 6) + (w >> 24 & 1) * 2;
                    PUSH(target & ~(uint32_t)3, 1);
                    break;
                }
                if (kind == SNX_A32_BX || kind == SNX_A32_SVC || kind == SNX_A32_BKPT || kind == SNX_A32_UNDEFINED) {
                    if (kind != SNX_A32_BX) PUSH(p + 4, 0);
                    break;
                }
                if ((kind == SNX_A32_DATA && (w >> 12 & 15) == 15) || (kind == SNX_A32_MULTIPLE && (w & 0x8000)) ||
                    (kind == SNX_A32_SINGLE && (w >> 12 & 15) == 15)) {
                    /* a write to the PC ends the block, and conditional ones continue after it */
                    if (cond != 14) PUSH(p + 4, 0);
                    break;
                }
            }
        }
#undef PUSH
    }
}

/* ---- linking ---- */

/* Emits the blocks from index first on into <path>.so. */
static int emit_module(AgbRecomp* recomp, AgbBoard* board, const char* path, size_t first, char* error,
                       size_t error_size) {
    static int initialized;
    char *object = NULL, *runtime = NULL, *library = NULL, *message = NULL, *triple;
    const char* args[10];
    LLVMContextRef ctx = NULL;
    LLVMModuleRef module = NULL;
    LLVMTargetMachineRef machine = NULL;
    LLVMTargetDataRef layout = NULL;
    LLVMPassBuilderOptionsRef options = NULL;
    LLVMTargetRef target;
    size_t n = 0;
    int result = -1;
    if (!initialized) {
        LLVMInitializeNativeTarget();
        LLVMInitializeNativeAsmPrinter();
        initialized = 1;
    }
    ctx = LLVMContextCreate();
    module = LLVMModuleCreateWithNameInContext("agb-code", ctx);
    triple = LLVMGetDefaultTargetTriple();
    if (LLVMGetTargetFromTriple(triple, &target, &message)) goto done;
    machine = LLVMCreateTargetMachine(target, triple, "generic", "", LLVMCodeGenLevelDefault, LLVMRelocPIC,
                                      LLVMCodeModelDefault);
    LLVMSetTarget(module, triple);
    layout = LLVMCreateTargetDataLayout(machine);
    LLVMSetModuleDataLayout(module, layout);
    for (size_t i = first; i <= recomp->count; ++i) {
        AgbBlock* block = &recomp->blocks[i];
        char name[32];
        size_t consumed = 0;
        LLVMValueRef fn;
        block_name(name, sizeof(name), i);
        if (block->thumb) {
            uint16_t words[AGB_BLOCK_MAX];
            for (uint32_t j = 0; j < AGB_BLOCK_MAX; ++j) words[j] = agb_read16(board, block->pc + j * 2);
            fn = snx_thumb_llvm_block_ex(module, name, words, AGB_BLOCK_MAX, block->pc, &consumed, &agb_codegen);
        } else {
            uint32_t words[AGB_BLOCK_MAX];
            for (uint32_t j = 0; j < AGB_BLOCK_MAX; ++j) words[j] = agb_read32(board, block->pc + j * 4);
            fn = snx_arm_llvm_block_ex(module, name, words, AGB_BLOCK_MAX, block->pc, &consumed, &agb_codegen);
        }
        if (!fn || !consumed) {
            snprintf(error, error_size, "the ARM recompiler rejected the block at 0x%08X", block->pc);
            goto done;
        }
        block->instructions = (uint16_t)(consumed > 0xFFFF ? 0xFFFF : consumed);
        block->code = code_hash(board, block->pc, block->thumb, block->instructions);
    }
    if (LLVMVerifyModule(module, LLVMReturnStatusAction, &message)) goto done;
    LLVMDisposeMessage(message); /* an empty one on success, still allocated */
    message = NULL;
    options = LLVMCreatePassBuilderOptions();
    {
        LLVMErrorRef pass = LLVMRunPasses(module, "default<O2>", machine, options);
        if (pass) {
            char* failure = LLVMGetErrorMessage(pass);
            snprintf(error, error_size, "%.300s", failure);
            LLVMDisposeErrorMessage(failure);
            goto done;
        }
    }
    if (LLVMVerifyModule(module, LLVMReturnStatusAction, &message)) goto done;
    LLVMDisposeMessage(message); /* an empty one on success, still allocated */
    message = NULL;
    object = (char*)malloc(strlen(path) + 8);
    runtime = (char*)malloc(strlen(path) + 8);
    library = (char*)malloc(strlen(path) + 8);
    if (!object || !runtime || !library) {
        snprintf(error, error_size, "out of memory");
        goto done;
    }
    sprintf(object, "%s.obj", path);
    sprintf(runtime, "%s.rt.a", path);
    sprintf(library, "%s.so", path);
    if (LLVMTargetMachineEmitToFile(machine, module, object, LLVMObjectFile, &message)) goto done;
    /* the blocks call the runtime library (snx_arm_undefined for what the board interprets): the module links it */
    if (snx_write_file(runtime, agb_arm_runtime, agb_arm_runtime_size)) {
        snprintf(error, error_size, "cannot write %s", runtime);
        goto done;
    }
    args[n++] = recomp->cc;
    args[n++] = "-shared";
    args[n++] = "-o";
    args[n++] = library;
    args[n++] = object;
    args[n++] = runtime;
#ifndef _WIN32
    args[n++] = "-lm";
#endif
    args[n] = NULL;
    {
        char* output = NULL;
        if (snx_run(args, NULL, NULL, &output)) {
            snprintf(error, error_size, "linking the recompiled ROM failed: %.300s", output ? output : "");
            free(output);
            goto done;
        }
        free(output);
    }
    remove(object);
    remove(runtime);
    result = 0;
done:
    if (result && message && !error[0]) snprintf(error, error_size, "%.300s", message);
    LLVMDisposeMessage(message);
    free(object);
    free(runtime);
    free(library);
    if (options) LLVMDisposePassBuilderOptions(options);
    if (layout) LLVMDisposeTargetData(layout);
    if (machine) LLVMDisposeTargetMachine(machine);
    if (module) LLVMDisposeModule(module);
    if (ctx) LLVMContextDispose(ctx);
    free(triple);
    return result;
}

/* Loads <path>.so and takes the blocks from index first on out of it. */
static void* open_module(AgbRecomp* recomp, const char* path, size_t first, char* error, size_t error_size) {
    size_t length = strlen(path);
    char* library = (char*)malloc(length + 12);
    void* handle;
    if (!library) return NULL;
    /* a name without a directory would not be looked up in the working directory */
    if (strchr(path, '/') || strchr(path, '\\'))
        sprintf(library, "%s.so", path);
    else
        sprintf(library, "./%s.so", path);
#ifdef _WIN32
    handle = (void*)LoadLibraryA(library);
#else
    handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
#endif
    free(library);
    if (!handle) {
#ifndef _WIN32
        const char* why = dlerror();
        snprintf(error, error_size, "cannot load the recompiled ROM module: %s", why ? why : "unknown");
#else
        snprintf(error, error_size, "cannot load the recompiled ROM module");
#endif
        return NULL;
    }
    for (size_t i = first; i <= recomp->count; ++i) recomp->blocks[i].fn = (SnxArmBlock)module_symbol(handle, i);
    return handle;
}

/* Links every known block into the cached module. */
static int relink(AgbRecomp* recomp, AgbBoard* board, char* error, size_t error_size) {
    close_patches(recomp);
    if (recomp->handle) {
        close_module(recomp->handle);
        recomp->handle = NULL;
    }
    for (size_t i = 1; i <= recomp->count; ++i) recomp->blocks[i].fn = NULL;
    if (emit_module(recomp, board, recomp->base, 1, error, error_size)) return -1;
    recomp->handle = open_module(recomp, recomp->base, 1, error, error_size);
    if (!recomp->handle) return -1;
    recomp->dirty = 0;
    return 0;
}

#ifndef SNX_AGB_JIT
/* Compiles the blocks from index first on into a module of their own. Linking everything again for each
   block a run reaches would cost the whole ROM's compile per block. */
static int patch(AgbRecomp* recomp, AgbBoard* board, size_t first, char* error, size_t error_size) {
    char* path = (char*)malloc(strlen(recomp->base) + 32);
    void** grown = (void**)realloc(recomp->patches, (recomp->patch_count + 1) * sizeof(*grown));
    void* handle = NULL;
    if (grown) recomp->patches = grown;
    if (!path || !grown) {
        free(path);
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    sprintf(path, "%s.%zu", recomp->base, recomp->patch_count + 1);
    if (!emit_module(recomp, board, path, first, error, error_size))
        handle = open_module(recomp, path, first, error, error_size);
    free(path);
    if (!handle) return -1;
    recomp->patches[recomp->patch_count++] = handle;
    return 0;
}
#endif

#ifdef SNX_AGB_JIT
/* The block compiled in this process by the backend's executor (exec.c), at its first dispatch: no module file and no
   link, which cost a run about a second for each batch of blocks it reached */
static int compile_block(AgbRecomp* recomp, AgbBoard* board, AgbBlock* block, char* error, size_t error_size) {
    size_t consumed = 0;
    if (!recomp->exec && !(recomp->exec = snx_arm_exec_new(error, error_size))) return -1;
    if (block->thumb) {
        uint16_t words[AGB_BLOCK_MAX];
        for (uint32_t j = 0; j < AGB_BLOCK_MAX; ++j) words[j] = agb_read16(board, block->pc + j * 2);
        block->fn =
            snx_arm_exec_compile(recomp->exec, words, AGB_BLOCK_MAX, block->pc, 1, agb_codegen.profile, 0, &consumed);
    } else {
        uint32_t words[AGB_BLOCK_MAX];
        for (uint32_t j = 0; j < AGB_BLOCK_MAX; ++j) words[j] = agb_read32(board, block->pc + j * 4);
        block->fn =
            snx_arm_exec_compile(recomp->exec, words, AGB_BLOCK_MAX, block->pc, 0, agb_codegen.profile, 0, &consumed);
    }
    if (!block->fn || !consumed) {
        snprintf(error, error_size, "the ARM recompiler rejected the block at 0x%08X", block->pc);
        return -1;
    }
    block->instructions = (uint16_t)(consumed > 0xFFFF ? 0xFFFF : consumed);
    block->code = code_hash(board, block->pc, block->thumb, block->instructions);
    ++recomp->compiled;
    recomp->dirty = 1;
    return 0;
}
#endif

/* The cache beside the ROM: the module plus a map of the blocks it holds, under the ROM's hash. */
static int write_cache(AgbRecomp* recomp, const AgbBoard* board) {
    size_t length = strlen(recomp->base);
    char* map_path = (char*)malloc(length + 8);
    FILE* file;
    if (!map_path) return -1;
    sprintf(map_path, "%s.map", recomp->base);
    file = fopen(map_path, "w");
    free(map_path);
    if (!file) return -1;
    fprintf(file, "snx-agb-code %d %u %016llX\n", AGB_RECOMP_ABI, SNX_ARM_ABI, (unsigned long long)rom_hash(board));
    for (size_t i = 1; i <= recomp->count; ++i)
        fprintf(file, "%08X %u %u %016llX snx_agb_%zu\n", recomp->blocks[i].pc, recomp->blocks[i].thumb,
                recomp->blocks[i].instructions, (unsigned long long)recomp->blocks[i].code, i);
    fclose(file);
    return 0;
}

int agb_recomp_load_cache(AgbRecomp* recomp, AgbBoard* board, char* error, size_t error_size) {
    size_t length = strlen(recomp->base);
    char* map_path = (char*)malloc(length + 8);
    char line[128];
    FILE* file;
    int abi = 0;
    unsigned arm_abi = 0;
    unsigned long long rom = 0, here;
    if (!map_path) return -1;
    sprintf(map_path, "%s.map", recomp->base);
    file = fopen(map_path, "r");
    if (!file) {
        free(map_path);
        return -1;
    }
    if (!fgets(line, sizeof(line), file) || sscanf(line, "snx-agb-code %d %u", &abi, &arm_abi) != 2 ||
        abi != AGB_RECOMP_ABI || arm_abi != SNX_ARM_ABI) {
        snprintf(error, error_size, "%s is another build's (ABI %d and %u, this one's %d and %u)", map_path, abi,
                 arm_abi, AGB_RECOMP_ABI, SNX_ARM_ABI);
        free(map_path);
        fclose(file);
        return -1;
    }
    here = (unsigned long long)rom_hash(board);
    if (sscanf(line, "snx-agb-code %*d %*u %llX", &rom) != 1 || rom != here) {
        snprintf(error, error_size, "%s is another ROM's (%016llX, this one %016llX)", map_path, rom, here);
        free(map_path);
        fclose(file);
        return -1;
    }
    free(map_path);
    while (fgets(line, sizeof(line), file)) {
        unsigned pc, thumb, instructions;
        unsigned long long code;
        if (sscanf(line, "%X %u %u %llX", &pc, &thumb, &instructions, &code) != 4 || !instructions) continue;
        if (!add(recomp, pc, (int)thumb)) {
            fclose(file);
            return -1;
        }
        recomp->blocks[recomp->count].instructions = (uint16_t)instructions;
        recomp->blocks[recomp->count].code = (uint64_t)code;
    }
    fclose(file);
    recomp->dirty = 0;
    recomp->handle = open_module(recomp, recomp->base, 1, error, error_size);
    return recomp->handle ? 0 : -1;
}

int agb_recomp_build(AgbRecomp* recomp, AgbBoard* board, char* error, size_t error_size) {
    if (!recomp->base || !recomp->count) discover(recomp, board, 0x080000C0, 0);
    if (!recomp->count) {
        snprintf(error, error_size, "no code found at the ROM entry");
        return -1;
    }
    if (recomp->dirty || !recomp->handle)
        if (relink(recomp, board, error, error_size)) return -1;
    write_cache(recomp, board);
    return 0;
}

const AgbBlock* agb_recomp_lookup(AgbRecomp* recomp, AgbBoard* board, uint32_t pc, int thumb, char* error,
                                  size_t error_size) {
    AgbBlock* block = find(recomp, pc, thumb);
    size_t first;
    if (block && block->instructions && !fixed(pc) && code_hash(board, pc, thumb, block->instructions) != block->code) {
        /* the game wrote over the code the block was built from, this run or the one that cached it */
        drop(recomp, block);
        block = NULL;
    }
    first = recomp->count + 1;
    if (block && block->fn && block->instructions) return block;
    if (!block) {
        /* a block the discovery step did not reach, a branch through a register or the instruction after one
           the board interpreted: the code it reaches comes with it */
        discover(recomp, board, pc, thumb);
        if (recomp->count < first) {
            snprintf(error, error_size, "out of memory");
            return NULL;
        }
#ifndef SNX_AGB_JIT
        if (patch(recomp, board, first, error, error_size)) return NULL;
        recomp->compiled += recomp->count + 1 - first;
#endif
        block = find(recomp, pc, thumb);
    }
#ifdef SNX_AGB_JIT
    /* this one alone: the others discovery found wait for their own dispatch, and the saved module for all of them */
    if (block && !block->fn && compile_block(recomp, board, block, error, error_size)) return NULL;
#else
    /* a block the cached module lacks: link everything again */
    else if (!block->fn && relink(recomp, board, error, error_size))
        return NULL;
#endif
    block = find(recomp, pc, thumb);
    if (!block || !block->fn || !block->instructions) {
        if (!error[0]) snprintf(error, error_size, "no block at 0x%08X", pc);
        return NULL;
    }
    return block;
}

int agb_recomp_save(AgbRecomp* recomp, AgbBoard* board, char* error, size_t error_size) {
    if (!recomp->dirty) return 0;
#ifdef SNX_AGB_JIT
    {
        /* the blocks the run ran: the ones discovery found that never did would cost the module's optimized compile
           (about 15 ms each), and a run that reaches them compiles them then */
        size_t kept = 0;
        for (size_t i = 1; i <= recomp->count; ++i)
            if (recomp->blocks[i].instructions) recomp->blocks[++kept] = recomp->blocks[i];
        recomp->count = kept;
    }
#endif
    if (relink(recomp, board, error, error_size)) return -1;
    return write_cache(recomp, board);
}
