/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_BRIDGE_H
#define SNX_ANDROID_BRIDGE_H
#include "common/cpu/aarch64/cpu.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The native bridge's core (docs/android/NATIVE.md): the code window that holds the guest libraries, its dispatch
   table and the JIT that fills it, each thread's CPU state, calls from the host into guest code, and the addresses of
   host functions that guest code calls. Guest memory is the host's: every CPU has mem 0 */

bool bridge_init(const char* cache, char* error, size_t error_size);
/* the -O letter of the JIT's code ('0', '1', '2', '3', 's', 'z'), before bridge_init: '2' by default. -O0 compiles
   several times faster, for a test of the JIT itself; the cache keeps each level's code apart */
bool bridge_set_optimization(char level);
/* each JIT unit and its time on stderr (--jit-verbose), before bridge_init */
void bridge_set_verbose(bool verbose);
/* the LLVM IR of each unit the JIT compiles saved in folder (--jit-keep-ir), before bridge_init: a unit that LLVM
   fails on can then be compiled again by itself */
void bridge_set_keep_ir(const char* folder);

/* size bytes of the code window for a library, aligned to align (a power of two): its guest address, 0 when the
   window is full */
uint64_t bridge_window_reserve(size_t size, size_t align);
/* guest code written at address (in the window, page-aligned) from the host's bytes: readable, and compiled when first
   reached */
void bridge_map_code(uint64_t address, const void* code, size_t size);
/* code that a library maps at [address, address + size): compiled when first reached */
void bridge_add_code(uint64_t address, size_t size);

/* A host function that guest code calls: handler runs with the caller's CPU (its arguments in the registers), then
   the call returns to x30. data is the handler's own. The same name gives the same address */
typedef void (*BridgeHandler)(CPU* c, void* data);
uint64_t bridge_export(const char* name, BridgeHandler handler, void* data);
/* the export at address can wait (a lock, a sleep, a read): a guest signal runs its handler while it waits */
void bridge_export_blocks(uint64_t address);
/* the name of an export's address, NULL for another address */
const char* bridge_export_name(uint64_t address);

/* the calling thread's CPU state, made with its guest stack and TLS the first time */
CPU* bridge_cpu(void);
/* the guest function at fn called with up to 8 integer arguments on this thread: its x0 */
uint64_t bridge_call(uint64_t fn, const uint64_t* args, unsigned count);

/* for the guest's signals (hle_signal.c): the address a handler returns to, which ends its run; the run of a handler
   from c's state, true at that return and false when it left through the end of a host call (c is at its end); and
   the code at address compiled ahead of its first call */
uint64_t bridge_sigreturn(void);
/* the guest address whose compiled code starts nearest below host address host (a function's or a block leader's,
   in any unit the JIT made): the guest code that a host fault at host is in. 0 when none is below it. For a crash
   report: it reads the units without the lock */
uint64_t bridge_guest_of_host(uintptr_t host);
bool bridge_run_handler(CPU* c);
void bridge_prepare(uint64_t address);

/* the calling thread's guest stack: its lowest address and its size */
void bridge_stack(uint64_t* base, size_t* size);
/* the calling thread's state gone (its guest stack freed): as a thread that ran guest code ends */
void bridge_thread_end(void);

/* the guest's stack guard (bionic's __stack_chk_guard, TLS slot 5) */
uint64_t bridge_stack_guard(void);
#endif
