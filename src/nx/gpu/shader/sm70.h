/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The SM70+ shader frontend: the 128-bit instructions of Volta to Ada (SM 7.0 to 8.9), which the GPU of the Switch 2
   runs, decoded (sm70_decode.c) and translated to SPIR-V (sm70_translate.c). It shares ShaderInfo, the descriptor
   layout (shader.h) and the structured control flow (structure.c) with the Maxwell translator, and nothing of the
   Maxwell decoder.

   An instruction is two little-endian 64-bit words, lo (bits 0-63) and hi (bits 64-127). Bits 0-11 are the opcode.
   An ALU instruction has a 9-bit opcode in bits 0-8 and its form in bits 9-11. The rest, as Mesa's NAK encodes it
   (sm70_encode.rs, MIT):
     12-14 the guard predicate (7 is PT), 15 its negation, 16-23 the destination register (255 is RZ),
     24-31 source a (neg 72, abs 73), 32-63 source b or c (a register at 32-39 with neg 63 and abs 62, a 32-bit
     immediate, a constant buffer: offset 38-53, bank 54-58, or a uniform register at 32-37 for a bindless one with
     bit 91), 64-71 a register for b or c (neg 75, abs 74).
     105-108 the stall cycles, 109 yield, 110-112 the barrier the instruction sets on its write, 113-115 the one it
     sets on its read (7: none), 116-121 the barriers it waits for, 122-125 the operand reuse flags.
   A graphics program starts with its 128-byte shader program header (SPH version 4, from SM 7.3). Its first 80
   bytes have the layout of Maxwell's SPH (ShaderInfo.sph). A compute program has no header. */
#ifndef SNX_SM70_H
#define SNX_SM70_H
#include "nx/gpu/shader/shader.h"
#include <stddef.h>

enum { SM70_K_FIX = 0, SM70_K_ALU = 1, SM70_K_UALU = 2 };
/* the opcodes, SM70_<NAME> from sm70_opcodes.def, after SM70_NONE (an unknown one) */
enum {
    SM70_NONE = 0,
#define SM70_OP(name, code, kind, forms, display) SM70_##name,
#include "nx/gpu/shader/sm70_opcodes.def"
#undef SM70_OP
    SM70__COUNT
};
typedef struct {
    u16 code; /* 12 bits for SM70_K_FIX, 9 for the ALU kinds */
    u8 kind, forms;
    const char* name;
} Sm70OpInfo;
/* by opcode, SM70_NONE's first */
extern const Sm70OpInfo sm70_ops[SM70__COUNT];

#define SM70_RZ  255u /* the zero register */
#define SM70_URZ 63u  /* the uniform zero register before SM 10.0 */
#define SM70_PT  7u   /* the true predicate */

enum { SM70_SRC_NONE, SM70_SRC_REG, SM70_SRC_UREG, SM70_SRC_IMM, SM70_SRC_CBUF };
/* an ALU source */
typedef struct {
    u8 kind;
    u8 reg; /* SM70_SRC_REG, SM70_SRC_UREG; for a bindless cbuf the uniform register of its handle */
    u8 neg, abs;
    u8 bank, bindless; /* SM70_SRC_CBUF */
    u32 value;         /* the immediate's bits, or the cbuf's byte offset */
} Sm70Src;
/* the scheduling of an instruction, which the translation does not need */
typedef struct {
    u8 stall, yield, wr_bar, rd_bar, wait_mask, reuse;
} Sm70Ctrl;
typedef struct {
    u64 lo, hi;
    u16 op;  /* SM70_<NAME>, SM70_NONE when the decoder does not know the encoding */
    u8 form; /* bits 9-11 */
    u8 pred; /* the guard predicate, SM70_PT for none */
    u8 pred_not;
    u8 dst;          /* bits 16-23: a register, or for the uniform ALU a uniform register (6 bits) */
    Sm70Src a, b, c; /* for the ALU kinds: a, then b and c by the form */
    Sm70Ctrl ctrl;
} Sm70Insn;

/* bits lo to lo + n - 1 of the instruction, n at most 32 */
static inline u32 sm70_field(const Sm70Insn* in, unsigned lo, unsigned n) {
    u64 v;
    if (lo >= 64)
        v = in->hi >> (lo - 64);
    else if (lo + n <= 64)
        v = in->lo >> lo;
    else
        v = (in->lo >> lo) | (in->hi << (64 - lo));
    return (u32)(v & ((1ull << n) - 1));
}

/* Decodes the instruction lo, hi into *out: 1 when the opcode is known, 0 when it is not (out->op is SM70_NONE, the
   rest is filled as far as the common fields go) */
int sm70_decode(u64 lo, u64 hi, Sm70Insn* out);
/* the instruction index that a BRA, BSSY or another relative branch at index idx goes to (each instruction is 16
   bytes). The offset is relative to the next instruction. Outside 0 to 0xfffffffe it returns 0xffffffff */
u32 sm70_branch_target(const Sm70Insn* in, u32 idx);
/* the instruction words of a program as host numbers: 2 * count words, lo then hi */
void sm70_load(const u8* code, u32 count, u64* words);

/* the program stage (ST_VS to ST_FS) that an SPH word 0 names, or -1 when the word is not an SPH of version 3 or 4 */
int sm70_sph_stage(u32 sph0);
#define SM70_SPH_BYTES 128u

/* A program in a block of shader memory: where its header and its code are (byte offsets from the block's start),
   the bytes of code up to and with the BRA to itself that follows its last EXIT, and its stage (ST_CS without a
   header). complete is 0 for a program that the block ends before its end */
typedef struct {
    u32 header, code, size;
    int stage, complete;
} Sm70Program;
/* Finds the programs in size bytes of shader memory, one after the other, with the padding between them (zero or NOP
   instructions) skipped: at most max into out, and returns how many it found */
int sm70_find_programs(const u8* data, u32 size, Sm70Program* out, int max);

/* Translates an SM70+ program to SPIR-V. code is the program's instructions (bytes, a multiple of 16), sph its
   header's 32 words for a graphics stage (NULL for ST_CS). si holds the key state as for shader_to_spirv (cs_local
   and shared_size for compute; zero picks 1x1x1 and 16 KiB) and gets what translation found: the constant buffers,
   textures, global buffers, attributes. Returns malloc'd words, their count in *word_count, or NULL with
   si->unsupported and si->first_unsupported set */
u32* sm70_to_spirv(const u8* code, u32 bytes, int stage, const u32* sph, ShaderInfo* si, size_t* word_count);
/* Debug: with the environment variable SNX_SM70_DUMP naming a folder, the bytes (and the 32 SPH words, NULL for
   none) go to DIR/sm70_<kind>_<hash>.bin (and .sph), and why to DIR/sm70_<kind>_<hash>.txt. sm70_to_spirv writes each
   program it refuses, sm70_find_programs each block without a complete program. The files are game data: keep
   them out of the repository */
void sm70_debug_dump(const char* kind, const u8* bytes, u32 size, const u32* sph, const char* why);
#endif
