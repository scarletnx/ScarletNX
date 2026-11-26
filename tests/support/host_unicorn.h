/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A native back end's code run under Unicorn on any host, for the back ends' tests: arm64, riscv64, ... (QEMU's CPU
   of that architecture). The test's own memory is mapped into Unicorn at the same addresses (hostuc_map, and
   hostuc_alloc's page-aligned allocations), so the code and the test's C share every pointer; a call to a native
   function goes to its trap, a word of a page after the code whose hook calls it with the ABI's first six
   arguments and puts its result in the ABI's result register (two of them for a native of 128 bits). An ELF
   relocatable object of the architecture (the back end's) is loaded into the code region, its relocations written by
   the recompiler's own snx_elf_relocate, an undefined symbol a trap of the native function registered under its name.
   A big-endian architecture's code reads the structures the test shares with it as its own numbers: the test's
   swapper (hostuc_swapper) turns them over each time the code starts or a trap's native function returns to it, and
   back each time it stops or calls one; its objects' tables are in its order (read here turned over), their
   relocations' fields written in it. PowerPC64 runs as POWER9
   in 64-bit, hypervisor and little-endian mode (MSR), which Unicorn 2.1.4 needs patching for (its PPC64 models and
   MSR's HV): hostuc_new gives NULL where the MSR it set doesn't hold. MIPS64 runs as MIPS64R2 with Unicorn's virtual
   TLB, every address its own (the kernel's segments would want them under 2 GiB). Unicorn has no LoongArch: its
   code runs in la64_interp.c's interpreter, on the same memory and the same traps. s390x runs as a z196 in 64-bit
   addressing mode (the PSW's EA and BA); a native function's sixth argument is the ABI's stack slot (160(r15)).

   hostuc_call marks what a callee of AAPCS64 keeps on arm64 (X19 to X29, the low halves of V8 to V15, SP), and X18, the
   platform's on some, and fails where one came back changed.

   HOSTUC_ARM64_WIN is arm64's code for Windows: its COFF objects are loaded by the production loader
   (code_loader_coff.c), its arena mapped into Unicorn. A native function leaves every register a callee may change
   changed. The objects' function table is held to what Windows does with it
   (arm64_unwinder.h): where a native function is called, and with hostuc_step at every instruction, the frames are
   unwound by it back to the entry, every register to its mark.

   HOSTUC_X64 is x86-64's System V code under Unicorn's x86-64, of the processor hostuc_new_cpu names (QEMU's models:
   a Nehalem's, a qemu64's...), whose CPUID the code's features are read from (hostuc_cpuid): an instruction the
   processor lacks stops it. A call's return address is pushed, and a trap's RET goes back to it */
#ifndef SNX_TEST_HOST_UNICORN_H
#define SNX_TEST_HOST_UNICORN_H
#include <stddef.h>
#include <stdint.h>

typedef enum {
    HOSTUC_ARM64,
    HOSTUC_RV64,
    HOSTUC_PPC64LE,
    HOSTUC_MIPS64EL,
    HOSTUC_LA64,
    HOSTUC_PPC64,  /* big-endian, ELFv2 */
    HOSTUC_MIPS64, /* big-endian */
    HOSTUC_S390X,
    HOSTUC_ARM64_WIN, /* arm64's code for Windows, in COFF objects */
    HOSTUC_WIN64,     /* x86-64's code for Windows, run by this x86-64 host itself (host_win64.c, with no Unicorn) */
    HOSTUC_X64        /* x86-64's System V code, on an older processor's model */
} HostUcArch;
typedef struct HostUc HostUc;
/* Unicorn with a region of code_cap bytes for code (and its traps after it): NULL where Unicorn has none of arch */
HostUc* hostuc_new(HostUcArch arch, size_t code_cap);
/* the same with the processor named cpu (HOSTUC_X64's: qemu64, kvm64, phenom, Opteron_G1 to G3, Conroe, Penryn,
   Nehalem, Westmere), NULL for the architecture's own; NULL, said with the names, for a name Unicorn has none of */
HostUc* hostuc_new_cpu(HostUcArch arch, size_t code_cap, const char* cpu);
/* HOSTUC_X64's: CPUID's leaf (subleaf 0) on the processor, EAX, EBX, ECX and EDX into r: 0, -1 */
int hostuc_cpuid(HostUc* u, unsigned leaf, unsigned r[4]);
void hostuc_free(HostUc* u);
const char* hostuc_name(HostUcArch arch);
/* the test's own memory of size bytes at p (page-aligned) mapped at its address: 0, -1 */
int hostuc_map(HostUc* u, void* p, size_t size);
/* page-aligned memory of size bytes (zeroed), NULL on failure: below 4 GiB where there is room, as Unicorn's MIPS64 raises
   an exception for an LL or LLD of an address above it */
void* hostuc_pages(size_t size);
/* size bytes of page-aligned memory mapped at its own address (zeroed), NULL on failure */
void* hostuc_alloc(HostUc* u, size_t size);
/* native function fn under name (what an object's undefined symbol names): its trap's address. wide: fn returns
   unsigned __int128, both halves into the result registers */
uint64_t hostuc_native(HostUc* u, const char* name, void* fn);
typedef struct {
    uint64_t lo, hi;
} HostUcPair;
uint64_t hostuc_native_wide(HostUc* u, const char* name, void* fn);
/* a trap's address for fn under name where one is registered, else 0 */
uint64_t hostuc_trap_of(HostUc* u, const char* name);
/* bytes of code copied into the region: their address (0 out of room) */
uint64_t hostuc_code(HostUc* u, const void* bytes, size_t size);
/* the code at [p, p + size) changed: Unicorn's translations of it dropped */
void hostuc_changed(HostUc* u, const void* p, size_t size);
/* an ELF (for Windows COFF) relocatable object of the architecture loaded: 0, -1 with why in error */
int hostuc_load(HostUc* u, const unsigned char* object, size_t size, char* error, size_t error_size);
/* name defined at address for the objects loaded after (data of the test's an object names), as their own symbols
   are; 0, -1 out of memory */
int hostuc_define(HostUc* u, const char* name, uint64_t address);
/* the loaded objects' symbol's address, 0 for none */
uint64_t hostuc_symbol(HostUc* u, const char* name);
/* 1 for a big-endian architecture */
int hostuc_big(HostUcArch arch);
/* swap(user): the structures the test shares with a big-endian architecture's code turned over (its own inverse) */
void hostuc_swapper(HostUc* u, void (*swap)(void* user), void* user);
/* the code at fn run with the first two arguments a0 and a1 until it returns: 0 and its result, -1 where Unicorn
   stopped (why on stderr) */
int hostuc_call(HostUc* u, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t* result);
/* HOSTUC_WIN64's and HOSTUC_ARM64_WIN's: the code's frames unwound by its function table at every instruction (1), not
   where it calls a native function alone; and how many states were unwound, *failures of them wrongly (hostuc_call fails
   with them). Others have no function table: none */
void hostuc_step(HostUc* u, int on);
size_t hostuc_unwound(const HostUc* u, size_t* failures);
/* HOSTUC_ARM64_WIN's function table (arm64_unwinder.h): the words of the code of the entry pc is in, which starts at
   *begin, 0 where none is; a frame at *state's pc made its caller's, NULL, or what can't be */
uint32_t hostuc_table_entry(const HostUc* u, uint64_t pc, uint64_t* begin);
struct Arm64State;
const char* hostuc_unwind_frame(const HostUc* u, struct Arm64State* state);

#endif
