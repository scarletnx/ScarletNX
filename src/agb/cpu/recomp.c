/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "agb/cpu/recomp.h"
#include "agb/cpu/agb_cpu.h"
#include "agb/board/board.h"
#include "common/utilities/common.h"
#include "common/platform/file.h"
#include "common/utilities/bits.h"
#include "common/cpu/arm/llvm.h"
#include "common/cpu/arm/exec.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/llvm/link.h"
#include "common/platform/dl.h"
#include <llvm-c/Core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AGB_RECOMP_ABI 10  /* a map's block names its module function, or none ("-") for a hint */
#define AGB_BLOCK_MAX  256 /* words compiled per block at most */

/* the ARM7TDMI's rules (arm.h) on the flat window: the A57 default would run the ROM as ARMv8-A. Its loads and stores
   count their bus cycles into the machine's CPU (agb_cpu.h), whose first member is the one the blocks get */
static const SnxArmCodegenOptions agb_codegen = {SNX_ARM_CODEGEN_OPTIONS_ABI, SNX_ARM_PROFILE_ARM7TDMI,
                                                 SNX_ARM_MEMORY_FLAT, offsetof(AgbCpu, bus_costs),
                                                 offsetof(AgbCpu, bus_cycles)};
_Static_assert(offsetof(AgbCpu, arm) == 0, "the blocks' CPU is the machine's first member");

void agb_recomp_init(AgbRecomp* recomp, const char* base) {
    *recomp = (AgbRecomp){0};
    recomp->base = (char*)malloc(strlen(base) + 1);
    if (recomp->base) strcpy(recomp->base, base);
    recomp->opt = '2';
}

void agb_recomp_free(AgbRecomp* recomp) {
    snx_arm_exec_free(recomp->exec);
    if (recomp->handle) plat_dl_close(recomp->handle);
    free(recomp->blocks);
    free(recomp->base);
    free(recomp->keep_ir);
    *recomp = (AgbRecomp){0};
}

static char* block_name(size_t index) { return snx_format("snx_agb_%zu", index); }

/* FNV-1a (snx_fnv1a) tells one ROM, or one block's code, from another */
static uint64_t rom_hash(const AgbBoard* board) {
    return board->has_cartridge    ? snx_fnv1a(SNX_FNV_BASIS, board->cartridge.data, board->cartridge.size)
           : board->multiboot_size ? board->multiboot_hash
                                   : SNX_FNV_BASIS;
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
        return snx_fnv1a(SNX_FNV_BASIS, words, count * sizeof(*words));
    } else {
        uint32_t words[AGB_BLOCK_MAX];
        for (size_t j = 0; j < count; ++j) words[j] = agb_read32(board, pc + (uint32_t)j * 4);
        return snx_fnv1a(SNX_FNV_BASIS, words, count * sizeof(*words));
    }
}

static void* module_symbol(void* handle, size_t index) {
    char* name = block_name(index);
    void* symbol = name ? plat_dl_sym(handle, name) : NULL;
    free(name);
    return symbol;
}

/* ---- block discovery ---- */

/* The blocks' index: an open addressing table of block indices, twice the blocks' capacity, carved after them from
   their allocation. Every dispatch finds its block here, in a probe or two whatever the number of blocks, where a scan
   of thousands cost the run nearly all its time. Index 0 is none, so a 0 slot is empty: the project's rule */
static uint32_t* slots(const AgbRecomp* recomp) { return (uint32_t*)(recomp->blocks + recomp->capacity); }

static size_t slot_of(const AgbRecomp* recomp, uint32_t pc, int thumb) {
    uint64_t key = pc ^ (uint32_t)thumb; /* a Thumb pc is even and an ARM pc a multiple of 4: bit 0 is free */
    return (size_t)((key * 0x9E3779B97F4A7C15ull) >> 32) & (recomp->capacity * 2 - 1);
}

static void index_put(AgbRecomp* recomp, size_t i) {
    uint32_t* slot = slots(recomp);
    size_t at = slot_of(recomp, recomp->blocks[i].pc, recomp->blocks[i].thumb);
    while (slot[at]) at = (at + 1) & (recomp->capacity * 2 - 1);
    slot[at] = (uint32_t)i;
}

/* the index again from the blocks, after they moved: grown, dropped or compacted */
static void index_build(AgbRecomp* recomp) {
    if (!recomp->capacity) return;
    memset(slots(recomp), 0, recomp->capacity * 2 * sizeof(uint32_t));
    for (size_t i = 1; i <= recomp->count; ++i) index_put(recomp, i);
}

/* whether block is the one at pc in that ISA: every search compares through here, which counts it (compared) */
static int block_is(AgbRecomp* recomp, const AgbBlock* block, uint32_t pc, int thumb) {
    ++recomp->compared;
    return block->pc == pc && block->thumb == (uint8_t)thumb;
}

static AgbBlock* find(AgbRecomp* recomp, uint32_t pc, int thumb) {
    if (!recomp->capacity) return NULL;
    const uint32_t* slot = slots(recomp);
    for (size_t at = slot_of(recomp, pc, thumb); slot[at]; at = (at + 1) & (recomp->capacity * 2 - 1)) {
        AgbBlock* block = &recomp->blocks[slot[at]];
        if (block_is(recomp, block, pc, thumb)) return block;
    }
    return NULL;
}

/* Takes a block out: an index names a block's function only from a module's emit to its open, which every block
   past it already went through */
static void drop(AgbRecomp* recomp, AgbBlock* block) {
    size_t i = (size_t)(block - recomp->blocks);
    memmove(block, block + 1, (recomp->count - i) * sizeof(*block));
    --recomp->count;
    recomp->dirty = 1;
    index_build(recomp);
}

static AgbBlock* add(AgbRecomp* recomp, uint32_t pc, int thumb) {
    AgbBlock* block = find(recomp, pc, thumb);
    if (block) return block;
    if (recomp->count + 1 >= recomp->capacity) {
        /* a power of two, the index's twice as many slots after the blocks in the same allocation */
        size_t capacity = recomp->capacity ? recomp->capacity * 2 : 256, each = sizeof(AgbBlock) + 2 * sizeof(uint32_t);
        AgbBlock* grown;
        if (capacity > UINT32_MAX / 2 || capacity > (size_t)-1 / each) return NULL;
        if (!(grown = (AgbBlock*)realloc(recomp->blocks, capacity * each))) return NULL;
        recomp->blocks = grown;
        recomp->capacity = capacity;
        index_build(recomp);
    }
    block = &recomp->blocks[++recomp->count];
    *block = (AgbBlock){0};
    block->pc = pc;
    block->thumb = (uint8_t)thumb;
    recomp->dirty = 1;
    index_put(recomp, recomp->count);
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
            uint32_t prefix_at = 1, prefix = 0; /* the last BL prefix's address (none: odd) and the LR it sets */
            for (uint32_t p = start.pc, n = 0; n < AGB_BLOCK_MAX / 2; ++n) {
                uint16_t hw1 = agb_read16(board, p), hw2 = agb_read16(board, p + 2);
                SnxThumb t;
                unsigned len = snx_thumb_decode_profile(agb_codegen.profile, hw1, hw2, &t);
                if (!len) break;
                if (t.shape == TH_LEGACY_BL) { /* the ARM7TDMI's BL, two halves */
                    if (!t.op) {
                        prefix = p + 4 + (uint32_t)((int32_t)((uint32_t)t.imm << 21) >> 9);
                        prefix_at = p;
                        p += 2;
                        continue;
                    }
                    /* the suffix calls through LR: known here when the prefix is just before it, else at run time */
                    if (prefix_at + 2 == p) PUSH(prefix + (t.imm << 1), 1);
                    PUSH(p + 2, 1);
                    break;
                }
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


/* Emits every block into <base>.so: compiled for this machine as the backend compiles the Switch's
   32-bit modules (module_arm.c), with the helpers they call (armlib.c), and linked by the backend's own linker, which
   exports each block's function by its name */
static int emit_module(AgbRecomp* recomp, AgbBoard* board, char* error, size_t error_size) {
    size_t count = recomp->count, object_size = 0, armlib_size = 0;
    unsigned char *object = NULL, *armlib = NULL;
    char **names = (char**)calloc(count ? count : 1, sizeof(*names)), *library = snx_format("%s.so", recomp->base), *tmp = NULL;
    SnxIr* ir = NULL;
    LLVMModuleRef module = NULL;
    SnxLinkInput inputs[2];
    SnxLinkOutput linked = {NULL, 0};
    int result = -1;
    if (!names || !library) {
        snprintf(error, error_size, "out of memory");
        goto done;
    }
    if (snx_ir_init(error, error_size) || !(ir = snx_ir_new("agb-code", recomp->opt, error, error_size))) goto done;
    module = LLVMModuleCreateWithNameInContext("agb-code", ir->ctx);
    LLVMSetTarget(module, LLVMGetTarget(ir->mod));
    LLVMSetDataLayout(module, LLVMGetDataLayoutStr(ir->mod));
    for (size_t i = 1; i <= recomp->count; ++i) {
        AgbBlock* block = &recomp->blocks[i];
        char* name = names[i - 1] = block_name(i);
        size_t consumed = 0;
        LLVMValueRef fn;
        if (!name) {
            snprintf(error, error_size, "out of memory");
            goto done;
        }
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
    if (recomp->keep_ir) { /* the module's IR as the backend made it, before its passes */
        char *text = LLVMPrintModuleToString(module), *kept = snx_path_join(recomp->keep_ir, "blocks.ll");
        int failed = !kept || snx_write_file(kept, text, strlen(text));
        if (failed) {
            char why[256];
            snprintf(error, error_size, "cannot save the IR of blocks in %s: %s", recomp->keep_ir,
                     snx_os_error(why, sizeof why));
        }
        LLVMDisposeMessage(text);
        free(kept);
        if (failed) goto done;
    }
    if (snx_ir_module_object(ir, module, recomp->opt, &object, &object_size, error, error_size) ||
        snx_ir_armlib_object(recomp->opt, &armlib, &armlib_size, error, error_size))
        goto done;
    inputs[0].data = armlib, inputs[0].size = armlib_size, inputs[0].name = "armlib";
    inputs[1].data = object, inputs[1].size = object_size, inputs[1].name = "blocks";
    if (snx_link(inputs, 2, (const char* const*)names, count, snx_path_leaf(library), &linked, error, error_size))
        goto done;
    tmp = snx_format("%s.tmp", library);
    if (!tmp || snx_write_file(tmp, linked.data, linked.size) || plat_replace_file(tmp, library)) {
        snprintf(error, error_size, "cannot write %s", library);
        if (tmp) remove(tmp);
        goto done;
    }
    result = 0;
done:
    if (result && !error[0]) snprintf(error, error_size, "linking the recompiled ROM failed");
    if (names)
        for (size_t i = 0; i < count; ++i) free(names[i]);
    free(names);
    free(library);
    free(tmp);
    free(object);
    free(armlib);
    free(linked.data);
    if (module) LLVMDisposeModule(module);
    snx_ir_free(ir);
    return result;
}

/* Loads <base>.so and takes the blocks out of it. */
static void* open_module(AgbRecomp* recomp, char* error, size_t error_size) {
    const char* path = recomp->base;
    /* a name without a directory would not be looked up in the working directory */
    char* library = strchr(path, '/') || strchr(path, '\\') ? snx_format("%s.so", path) : snx_format("./%s.so", path);
    if (!library) return NULL;
    void* handle = plat_dl_open(library);
    if (!handle) snprintf(error, error_size, "cannot load %s: %s", library, plat_dl_error());
    free(library);
    if (!handle) return NULL;
    for (size_t i = 1; i <= recomp->count; ++i)
        recomp->blocks[i].fn =
            recomp->blocks[i].symbol ? (SnxArmBlock)module_symbol(handle, recomp->blocks[i].symbol) : NULL;
    return handle;
}

/* the module file's size and time now, which saving compares with the ones its symbols came from */
static void stamp(const AgbRecomp* recomp, unsigned long long* size, unsigned long long* time) {
    char* library = snx_format("%s.so", recomp->base);
    *size = library ? snx_file_size(library) : 0;
    *time = library ? snx_file_time(library) : 0;
    free(library);
}

/* Links every known block into the cached module. */
static int relink(AgbRecomp* recomp, AgbBoard* board, char* error, size_t error_size) {
    if (recomp->handle) {
        plat_dl_close(recomp->handle);
        recomp->handle = NULL;
    }
    for (size_t i = 1; i <= recomp->count; ++i) {
        recomp->blocks[i].fn = NULL;
        recomp->blocks[i].symbol = (uint32_t)i; /* emit_module names each block's function by its index */
        recomp->blocks[i].hint = 0;
    }
    if (emit_module(recomp, board, error, error_size)) return -1;
    recomp->handle = open_module(recomp, error, error_size);
    if (!recomp->handle) return -1;
    stamp(recomp, &recomp->module_size, &recomp->module_time);
    recomp->dirty = 0;
    return 0;
}

/* The block compiled in this process by the backend's executor (exec.c), at its first dispatch: no module file and no
   link, which cost a run about a second for each batch of blocks it reached */
static int compile_block(AgbRecomp* recomp, AgbBoard* board, AgbBlock* block, char* error, size_t error_size) {
    size_t consumed = 0;
    if (!recomp->exec && !(recomp->exec = snx_arm_exec_new(error, error_size))) return -1;
    if (block->thumb) {
        uint16_t words[AGB_BLOCK_MAX];
        for (uint32_t j = 0; j < AGB_BLOCK_MAX; ++j) words[j] = agb_read16(board, block->pc + j * 2);
        block->fn = snx_arm_exec_compile_ex(recomp->exec, words, AGB_BLOCK_MAX, block->pc, 1, &agb_codegen, &consumed);
    } else {
        uint32_t words[AGB_BLOCK_MAX];
        for (uint32_t j = 0; j < AGB_BLOCK_MAX; ++j) words[j] = agb_read32(board, block->pc + j * 4);
        block->fn = snx_arm_exec_compile_ex(recomp->exec, words, AGB_BLOCK_MAX, block->pc, 0, &agb_codegen, &consumed);
    }
    if (!block->fn || !consumed) {
        snprintf(error, error_size, "the ARM recompiler rejected the block at 0x%08X", block->pc);
        return -1;
    }
    block->instructions = (uint16_t)(consumed > 0xFFFF ? 0xFFFF : consumed);
    block->code = code_hash(board, block->pc, block->thumb, block->instructions);
    if (!block->hint) { /* a hint the map named is kept already */
        block->hint = 1;
        ++recomp->compiled;
        recomp->dirty = 1;
    }
    return 0;
}

/* The cache beside the ROM: the module plus a map of the blocks it holds, under the ROM's hash. */
static int write_cache(AgbRecomp* recomp, const AgbBoard* board) {
    size_t length = strlen(recomp->base);
    char* map_path = (char*)malloc(length + 8);
    if (!map_path) return -1;
    sprintf(map_path, "%s.map", recomp->base);
    FILE* file = fopen(map_path, "w");
    free(map_path);
    if (!file) return -1;
    fprintf(file, "snx-agb-code %d %u %016llX\n", AGB_RECOMP_ABI, SNX_ARM_ABI, (unsigned long long)rom_hash(board));
    for (size_t i = 1; i <= recomp->count; ++i) {
        const AgbBlock* block = &recomp->blocks[i];
        fprintf(file, "%08X %u %u %016llX ", block->pc, block->thumb, block->instructions,
                (unsigned long long)block->code);
        if (block->symbol)
            fprintf(file, "snx_agb_%u\n", (unsigned)block->symbol);
        else
            fputs("-\n", file);
    }
    fclose(file);
    return 0;
}

int agb_recomp_load_map(AgbRecomp* recomp, AgbBoard* board, char* error, size_t error_size) {
    char* map_path = snx_format("%s.map", recomp->base);
    FILE* file = map_path ? fopen(map_path, "r") : NULL;
    if (!file) {
        free(map_path);
        return -1;
    }
    fclose(file);
    int loaded = agb_recomp_load_map_from(recomp, board, map_path, error, error_size);
    free(map_path);
    return loaded;
}
int agb_recomp_load_map_from(AgbRecomp* recomp, AgbBoard* board, const char* path, char* error, size_t error_size) {
    char line[128];
    int abi = 0;
    unsigned arm_abi = 0;
    unsigned long long rom = 0;
    const char* map_path = path;
    FILE* file = fopen(map_path, "r");
    if (!file) {
        char why[256];
        snprintf(error, error_size, "cannot read %s: %s", map_path, snx_os_error(why, sizeof why));
        return -1;
    }
    if (!fgets(line, sizeof(line), file) || sscanf(line, "snx-agb-code %d %u", &abi, &arm_abi) != 2 ||
        abi != AGB_RECOMP_ABI || arm_abi != SNX_ARM_ABI) {
        snprintf(error, error_size, "%s is another build's (ABI %d and %u, this one's %d and %u)", map_path, abi,
                 arm_abi, AGB_RECOMP_ABI, SNX_ARM_ABI);
        fclose(file);
        return -1;
    }
    unsigned long long here = (unsigned long long)rom_hash(board);
    if (sscanf(line, "snx-agb-code %*d %*u %llX", &rom) != 1 || rom != here) {
        snprintf(error, error_size, "%s is another ROM's (%016llX, this one %016llX)", map_path, rom, here);
        fclose(file);
        return -1;
    }
    while (fgets(line, sizeof(line), file)) {
        unsigned pc, thumb, instructions, symbol = 0;
        unsigned long long code;
        char name[32];
        if (sscanf(line, "%X %u %u %llX %31s", &pc, &thumb, &instructions, &code, name) != 5 || !instructions) continue;
        if (strcmp(name, "-") && sscanf(name, "snx_agb_%u", &symbol) != 1) continue;
        AgbBlock* block = add(recomp, pc, (int)thumb);
        if (!block) {
            fclose(file);
            snprintf(error, error_size, "out of memory");
            return -1;
        }
        block->instructions = (uint16_t)instructions;
        block->code = (uint64_t)code;
        block->symbol = symbol;
        block->hint = !symbol;
    }
    fclose(file);
    recomp->dirty = 0;
    return 0;
}

int agb_recomp_load_cache(AgbRecomp* recomp, AgbBoard* board, char* error, size_t error_size) {
    if (agb_recomp_load_map(recomp, board, error, error_size)) return -1;
    recomp->handle = open_module(recomp, error, error_size);
    if (recomp->handle) stamp(recomp, &recomp->module_size, &recomp->module_time);
    return recomp->handle ? 0 : -1;
}

int agb_recomp_build(AgbRecomp* recomp, AgbBoard* board, char* error, size_t error_size) {
    if (!recomp->base || !recomp->count) discover(recomp, board, agb_board_entry(board), 0);
    if (!recomp->count) {
        snprintf(error, error_size, "no code found at the entry 0x%08X", agb_board_entry(board));
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
    if (block && block->instructions && !fixed(pc) && code_hash(board, pc, thumb, block->instructions) != block->code) {
        /* the game wrote over the code the block was built from, this run or the one that cached it */
        drop(recomp, block);
        block = NULL;
    }
    size_t first = recomp->count + 1;
    if (block && block->fn && block->instructions) return block;
    if (!block) {
        /* a block the discovery step did not reach, a branch through a register or the instruction after one
           the board interpreted: the code it reaches comes with it */
        discover(recomp, board, pc, thumb);
        if (recomp->count < first) {
            snprintf(error, error_size, "out of memory");
            return NULL;
        }
        block = find(recomp, pc, thumb);
    }
    /* this one alone: the others discovery found wait for their own dispatch, and the saved module for all of them */
    if (block && !block->fn && compile_block(recomp, board, block, error, error_size)) return NULL;
    block = find(recomp, pc, thumb);
    if (!block || !block->fn || !block->instructions) {
        if (!error[0]) snprintf(error, error_size, "no block at 0x%08X", pc);
        return NULL;
    }
    return block;
}

int agb_recomp_save(AgbRecomp* recomp, AgbBoard* board, char* error, size_t error_size) {
    if (!recomp->dirty) return 0;
    {
        /* the blocks the run ran: the ones discovery found that never did would cost the module's optimized compile
           (about 15 ms each), and a run that reaches them compiles them then */
        size_t kept = 0;
        for (size_t i = 1; i <= recomp->count; ++i)
            if (recomp->blocks[i].instructions) recomp->blocks[++kept] = recomp->blocks[i];
        recomp->count = kept;
        index_build(recomp);
    }
    {
        /* a recompile in another process since this run loaded the module: its functions are numbered its own
           way, and every block is a hint the next run compiles again */
        unsigned long long size, time;
        stamp(recomp, &size, &time);
        if (size != recomp->module_size || time != recomp->module_time)
            for (size_t i = 1; i <= recomp->count; ++i) {
                recomp->blocks[i].symbol = 0;
                recomp->blocks[i].hint = 1;
            }
    }
    /* the map alone, at once: compiling every block into the module, which grows with the game, waits for a
       recompile (-r), which starts from them */
    if (write_cache(recomp, board)) {
        char why[256];
        snprintf(error, error_size, "cannot write %s.map: %s", recomp->base, snx_os_error(why, sizeof why));
        return -1;
    }
    recomp->dirty = 0;
    return 0;
}
