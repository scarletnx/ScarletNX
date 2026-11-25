/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The settings of the process that the LLVM backend's code takes (ir.h), which the runtime and the recompile hand it
   from the player's, with no header of LLVM's */
#ifndef SNX_RECOMPILER_LLVM_IR_SETTINGS_H
#define SNX_RECOMPILER_LLVM_IR_SETTINGS_H
#include "common/utilities/common.h"

/* LLVM's SLP vectorizer on or off for every module this process makes (the setting slp_vectorizer): an option of
   LLVM's command line, so once per process, from one thread, before any module is made */
void snx_ir_slp_vectorizer(int on);
/* whether it runs: LLVM's own, on, until snx_ir_slp_vectorizer. The caches' keys carry it */
int snx_ir_slp(void);
/* Whether the generated code keeps FPSR's cumulative flags (IOC, DZC, OFC, UFC, IXC and QC) as the console raises
   them (the setting fpsr_flags, on by default): off, the FP fast paths give the console's results and nothing else,
   the host's IEEE arithmetic wherever the result is not a NaN, the flags only where the helpers run, and saturating
   instructions leave QC alone. Once per process, before
   any module is made; the caches' keys carry it */
void snx_ir_fpsr_flags(int exact);
int snx_ir_fpsr_exact(void);
/* the settings ir_stats (each code unit's static IR counts in a recompile's log) and ir_dump_pc (hex: the function
   there printed, its IR before and after the promotion), for the SnxIr made after */
void snx_ir_diagnostics(int stats, const char* dump_pc);
int snx_ir_stats_on(void); /* the setting ir_stats is on: the recompile reports each unit's IR (LLVM's alone) */
/* the function ir_dump_pc names, 0 for none: LLVM makes it, whose IR is printed, and the x86-64 back end not */
SnxU64 snx_ir_dump_pc(void);
/* Whether the guest's loads and stores are volatile (the setting volatile_memory, off by default): on, LLVM keeps
   each where the guest has it (ir_state.c's ir_rd). Once per process, before any module is made; the caches' keys
   carry it */
void snx_ir_volatile_memory(int on);
int snx_ir_volatile(void);
/* Whether guest calls follow AAPCS64 at BL and BLR sites (the setting abi_contract, off by default): on, promotion
   keeps the callee-saved registers in their host locations across a call and does not store the caller-saved ones
   that are dead after it. Once per process, before any module is made; the caches' keys carry it */
void snx_ir_abi_contract(int on);
int snx_ir_abi_contract_on(void);
/* the setting ir_branch_profile: the file ir.h's ir_profile_branch reads, once, at the first branch of a module made after
   (NULL or "": none). Before any module is made */
void snx_ir_branch_profile(const char* path);
#endif
