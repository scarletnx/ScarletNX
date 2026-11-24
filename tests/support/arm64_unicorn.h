/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* arm64 code run under Unicorn (QEMU's AArch64, its "max" CPU) on any host, for the arm64 back end's tests: the
   test's own memory is mapped into Unicorn at the same addresses (armuc_map: page-aligned allocations, armuc_alloc's),
   so the code and the test's C share every pointer; a call to a native function goes to its trap, a word of a page
   after the code whose hook calls it with X0 to X5 and puts its result in X0. An ELF AArch64 relocatable object (the
   back end's) is loaded into the code region, its relocations written by the recompiler's own snx_elf_relocate, an
   undefined symbol a trap of the native function registered under its name */
#ifndef SNX_TEST_ARM64_UNICORN_H
#define SNX_TEST_ARM64_UNICORN_H
#include <stddef.h>
#include <stdint.h>

typedef struct ArmUc ArmUc;
/* Unicorn with a region of code_cap bytes for code (and its traps after it): NULL where Unicorn has no AArch64 */
ArmUc* armuc_new(size_t code_cap);
void armuc_free(ArmUc* u);
/* size bytes of page-aligned memory mapped at its own address (zeroed), NULL on failure */
void* armuc_alloc(ArmUc* u, size_t size);
/* native function fn under name (what an object's undefined symbol names): its trap's address */
uint64_t armuc_native(ArmUc* u, const char* name, void* fn);
/* bytes of code copied into the region: their address (NULL out of room) */
uint64_t armuc_code(ArmUc* u, const void* bytes, size_t size);
/* an ELF AArch64 relocatable object loaded into the region: 0, -1 with why in error */
int armuc_load(ArmUc* u, const unsigned char* object, size_t size, char* error, size_t error_size);
/* the loaded objects' symbol's address, 0 for none */
uint64_t armuc_symbol(ArmUc* u, const char* name);
/* the code at fn run with X0 and X1 (a GuestFn's state and entry) until it returns: 0, -1 where Unicorn stopped
   (why on stderr) */
int armuc_call(ArmUc* u, uint64_t fn, uint64_t x0, uint64_t x1, uint64_t* result);

#endif
