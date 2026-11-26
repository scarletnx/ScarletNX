/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What an AArch32 block is made for, whichever compiler makes it (llvm.h's, the native back ends' of
   recompiler/mach/mach_arm.inc): the guest's CPU (arm.h's profiles), how its code reaches guest memory, and what it
   records as it runs */
#ifndef SNX_ARM_CODEGEN_H
#define SNX_ARM_CODEGEN_H
#include "common/cpu/arm/arm.h"

/* FLAT reads and writes cpu->mem directly, within cpu->mem_size.
   CHECKED (checked.h) calls the runtime's bus for every fetch and every transfer. It makes integer transfers only, and
   the ARM11's VFP and exclusives; the others stop with BACKEND_UNSUPPORTED, never access the flat pointer.
   Every instruction is fetched through the bus before executing, including
   condition-failed instructions. Permission faults report PREFETCH_ABORT;
   changed bytes report CODE_CHANGED without executing the stale instruction.
   The runtime must return to its dispatcher on either exit, never re-enter
   stale code or silently compile at runtime.
   The ARM7TDMI and the ARM946E take flat memory only: their bus hook reads cpu->io and cpu->mem.
   HOSTED is flat memory for a runtime that owns the whole process, as the 3DS and Vita runtimes do. A block of this
   mode assumes three things, and a block of the other modes assumes none of them:
   - the mapping covers all 2^32 guest addresses and is followed by pages that fault on the host (at least 256 bytes
     of them), so an access that leaves the mapping is a host fault and gets no range check;
   - while a block runs, the SnxArmCPU is reached only through the pointer the block got, so the compiler keeps
     registers and flags in host registers across guest loads and stores (LLVM's cpu argument is noalias);
   - the dispatch loop has seen to the entry state: r15 is the block's address, CPSR.T is its instruction set, ITSTATE
     is clear (snx_arm_itstate), and exit is SNX_ARM_OK. The block checks and clears none of these.
   CPSR.E is never set by a hosted block: SETEND BE and an MSR that sets E stop with SNX_ARM_ENDIAN_UNSUPPORTED
   instead, so no entry has to look at it. */
typedef enum { SNX_ARM_MEMORY_FLAT, SNX_ARM_MEMORY_CHECKED, SNX_ARM_MEMORY_HOSTED } SnxArmMemoryMode;
#define SNX_ARM_CODEGEN_OPTIONS_ABI 1u
/* Versioned opt-in code generation. NULL means the original A57/flat ABI. */
typedef struct SnxArmCodegenOptions {
    uint32_t abi;
    SnxArmProfile profile;
    SnxArmMemoryMode memory;
    /* the ARM7TDMI's trace, for a runtime that times its blocks itself: where, past the pointer its blocks get, it
       keeps a uint32_t per instruction of the longest block it compiles and a bit per instruction after it (skipped).
       An instruction's slot, by its offset from the block's start (halfwords in Thumb, words in ARM), gets its load's
       or store's address (a multiple one's lowest), or its multiplier (ARM's Rs, Thumb's Rd); the bit is set when a
       conditional instruction whose time depends on running did not run (any but a data processing one with no
       register shift that leaves the PC alone). 0 traces nothing; another profile takes none */
    uint32_t trace, skipped;
} SnxArmCodegenOptions;
/* 1 for options a block compiler takes: a profile on memory it runs on, a trace only the ARM7TDMI's */
static inline int snx_arm_codegen_options_valid(const SnxArmCodegenOptions* o) {
    return !o || (o->abi == SNX_ARM_CODEGEN_OPTIONS_ABI &&
                  (o->profile == SNX_ARM_PROFILE_A57 || o->profile == SNX_ARM_PROFILE_ARM11 ||
                   o->profile == SNX_ARM_PROFILE_A9 ||
                   ((o->profile == SNX_ARM_PROFILE_ARM7TDMI || o->profile == SNX_ARM_PROFILE_ARM946E) &&
                    o->memory == SNX_ARM_MEMORY_FLAT)) &&
                  (o->memory == SNX_ARM_MEMORY_FLAT || o->memory == SNX_ARM_MEMORY_CHECKED ||
                   o->memory == SNX_ARM_MEMORY_HOSTED) &&
                  ((!o->trace && !o->skipped) ||
                   (o->profile == SNX_ARM_PROFILE_ARM7TDMI && o->trace && o->skipped && o->trace != o->skipped)));
}
/* Options for hosted blocks that go on to the next by themselves, for a runtime whose dispatch loop runs one thread's
   blocks a slice at a time (the Vita's). The native back ends take these where options.abi says so; the fallback's go
   on by what it adds to them (fallback.h's snx_fallback_arm_chain). poll, lookup and direct are where the runtime keeps
   a thing past the pointer its blocks get, 0 for none of it:
   - poll: an int32_t, the steps back (to a block at or before the one that ends) the slice may still take. A block counts
     its own down and returns to the loop where they run out, so every loop of blocks ends the slice;
   - lookup (with poll alone): a pointer to a table of SNX_ARM_LOOKUP_SLOTS SnxArmLookups, or null. A target that is none
     of the block's successors (an indirect branch's) is run from there, its step back counted the same way; a target the
     table lacks returns to the loop, as every target does with no table;
   - direct: the block is an import's stub, an SVC alone, and this a pointer to a function, or null. It is called with the
     block's pointer and import in the SVC's place (R15 past it), and the block goes on past the SVC where it returns
     nonzero, else to the loop. With no function it is the SVC, import its syndrome */
#define SNX_ARM_CHAINED_OPTIONS_ABI 2u
typedef struct {
    SnxArmCodegenOptions options; /* abi SNX_ARM_CHAINED_OPTIONS_ABI, memory SNX_ARM_MEMORY_HOSTED */
    uint32_t poll, lookup, direct, import;
} SnxArmChainedOptions;
/* at a target's address shifted right once; key is the address with bit 0 set for Thumb, 0 for an empty slot */
typedef struct {
    uint32_t key;
    SnxArmBlock block;
} SnxArmLookup;
#define SNX_ARM_LOOKUP_SLOTS 65536u /* a power of two */
/* o's options as they are without the chaining: 1 for ones the native back ends take */
static inline int snx_arm_chained_options_valid(const SnxArmChainedOptions* o) {
    SnxArmCodegenOptions plain = o->options;
    plain.abi = SNX_ARM_CODEGEN_OPTIONS_ABI;
    return o->options.abi == SNX_ARM_CHAINED_OPTIONS_ABI && snx_arm_codegen_options_valid(&plain) &&
           plain.memory == SNX_ARM_MEMORY_HOSTED && (o->poll || !o->lookup);
}
/* an ARM7TDMI instruction whose time depends on whether its condition held (SnxArmCodegenOptions' skipped): any but a
   data processing one with an immediate or an immediate shift that leaves the PC alone */
static inline int snx_arm_timed_condition(uint32_t w) {
    int plain = (w & 0x0C000000) == 0 && ((w & 1u << 25) || !(w & 0x10));
    return !plain || (w >> 12 & 15) == 15;
}

/* The bus hook (SnxArmCPU.io) on the ARM7TDMI: the BIOS, the I/O window, the palette, VRAM, OAM and the save memory
   have bus rules a plain access cannot follow (the BIOS's open bus, mirrors finer than a page, byte write
   widths). The runtime registers a hook the way the A64 side registers a fetch callback; without one every
   access is direct. The regions are the AGB board's, by the address's top byte: 0x00 to 0x01 the BIOS, 0x04 to 0x07 the
   I/O window and video memory, 0x0D the EEPROM a cartridge may have there, 0x0E and 0x0F the save memory. An address
   past the console's 28 bits goes to the hook as well: nothing answers there, which the board gives as the open bus.
   The ARM946E's hook sees the registers' window alone, 0x04. */
#define SNX_ARM_IO_REGIONS                                                                                             \
    (1u << 0x00 | 1u << 0x01 | 1u << 0x04 | 1u << 0x05 | 1u << 0x06 | 1u << 0x07 | 1u << 0x0D | 1u << 0x0E | 1u << 0x0F)
/* Stores reach the hook from the cartridge's ROM, 0x08 to 0x0C, as well: the flat window holds the image there, and the
   console's ROM takes no writes, so a game's store into a table of its own leaves the table as it was */
#define SNX_ARM_IO_STORE_REGIONS (SNX_ARM_IO_REGIONS | 1u << 0x08 | 1u << 0x09 | 1u << 0x0A | 1u << 0x0B | 1u << 0x0C)
/* the save memory, whose 8 bit bus takes an LDM's or STM's addresses as they are; below it they are words' */
#define SNX_ARM_SAVE_MEMORY      0x0E000000u
#endif
