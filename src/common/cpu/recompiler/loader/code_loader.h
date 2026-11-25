/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The JIT's loader: the LLVM backend's relocatable objects (snx_ir_object's, the runtime library's) placed in this
   process's memory and linked there, where the AOT path links them into a module file for dlopen (link_elf.c,
   link_coff.c). ELF objects are read and relocated by elf_object.c, as link_elf.c reads and relocates them, and
   Windows' COFF objects by code_loader_coff.c.

   A loader owns one code arena: address space reserved once and given back only when the loader is freed. It is at
   most 2 GiB, so a 32-bit displacement reaches from any byte of it to any other: code loaded later calls the runtime
   library loaded first directly. The arena is three regions, each filled front to back with the loads packed one
   after another (a load's code from a new cache line, its other parts as their sections align):
     [code, stubs: read-execute] [read-only data, GOT: read-only] [data, bss: read-write]
   The first two are shared memory mapped twice: the region in the arena, which code runs from and which never
   becomes writable, and a read-write view elsewhere that the loader writes through, with every address computed for
   the arena. So a load goes next to code other threads are running, on the same pages, with no page writable and
   executable in one view (W^X) and no protection change per load (macOS has one view of them: below). The data region
   is private memory, made read-write a page at a time as loads reach it.

   A load's symbols resolve to the load's own definitions first, then to the loader's table: the globals of earlier
   loads and the names given to snx_code_define (the runtime's functions and data, anywhere in the address space). A
   call to a table symbol out of its displacement's reach (x86-64's 2 GiB, AArch64's 128 MiB) goes through a stub in
   the load, and a GOT reference gets a slot in the load's read-only GOT, which holds the full address.

   Threads: one call at a time per loader, which the caller serializes. Code from earlier loads keeps running during a
   load, which writes only bytes no earlier load uses, and an address comes out of snx_code_load only once its code is
   written and the instruction cache flushed: the caller can hand it to other threads (with release ordering) as is.
   x86-64 keeps instruction fetch coherent with stores from every core, checked on physical addresses (Intel SDM vol.
   3A, 11.6), so through the other view too, and no thread can jump to the new bytes before it loads their address.
   AArch64 needs more: the instruction cache is invalidated at the executable view's addresses, and every core of the
   process synchronizes its instruction stream (membarrier's PRIVATE_EXPEDITED_SYNC_CORE) before the address goes out,
   since a core may have fetched those bytes before they were written. An AArch64 Linux kernel without it (before
   4.16) gets no loader. XNU has no membarrier: on macOS each load stops every other thread of the process off its core
   (thread_get_state), and a thread comes back through a context switch, which XNU synchronizes.

   x86-64 and AArch64 ELF hosts with memfd_create (Linux) or SHM_ANON (FreeBSD), and x86-64 Windows 10 (1803) and
   later, whose objects are COFF. There the views are a pagefile-backed section (CreateFileMapping, with
   PAGE_EXECUTE_READWRITE as its maximum protection) mapped twice: with MapViewOfFile3 into a placeholder cut out of the
   arena's reservation (VirtualAlloc2 with MEM_RESERVE_PLACEHOLDER, VirtualFree with MEM_PRESERVE_PLACEHOLDER), and
   read-write anywhere. A COFF load's function table (.pdata) is registered with RtlAddFunctionTable, the arena's start
   its image base, so exceptions and longjmp unwind through JIT code as through a module's.

   PowerPC64 (ELFv1 and ELFv2) loads keep one TOC for the whole arena in r2, as the AOT linker's module keeps one
   (link_elf.c), so guest code jumps from one load into another with r2 as it is. Loaded code may call nothing outside
   the arena directly: the runtime's functions have a TOC of their own, which only pointer calls (RtAPI) switch to.
   The runtime JIT's arena shares the TOC of the AOT module its units call and jump into (snx_code_loader_new_toc):
   the arena is placed within 32-bit reach of the module's TOC, which every load keeps in r2 instead, so a guest's
   jump between the module and a load, either way, leaves r2 as the code it reaches wants it.

   macOS on Apple silicon loads ELF objects too, which the arm64 back end and the IR backend (snx_ir_new_jit) write
   there with Darwin's calling convention. Memory becomes executable there only as MAP_JIT memory, which can't be
   shared or placed: the code and read-only data are one MAP_JIT mapping, which each thread sees either writable or
   executable
   (pthread_jit_write_protect_np). The loading thread sees it writable while it writes a load, every other thread
   executable, so no thread can write and run it at once, and nothing loaded may run on the loading thread during
   snx_code_load (the resolver included). Not iOS, where an app may not make memory executable */
#ifndef SNX_RECOMPILER_LOADER_CODE_LOADER_H
#define SNX_RECOMPILER_LOADER_CODE_LOADER_H
#include "common/utilities/common.h"
#include "common/cpu/recompiler/object/link.h"

typedef struct SnxCodeLoader SnxCodeLoader;

/* what the refusal of an arena's JIT memory of size bytes says, for the system's error number (errno's): the size, the
   system's reason and what macOS asks of the app */
void snx_code_jit_refused(size_t size, int number, char* error, size_t error_size);

/* the machines of Windows' x86-64 and ARM64 COFF objects (IMAGE_FILE_MACHINE_AMD64, IMAGE_FILE_MACHINE_ARM64), for
   snx_code_loader_new_machine */
#define SNX_CODE_COFF_X64      0x8664u
#define SNX_CODE_COFF_ARM64    0xAA64u
#define snx_code_coff(machine) ((machine) == SNX_CODE_COFF_X64 || (machine) == SNX_CODE_COFF_ARM64)

/* the arena's size when snx_code_loader_new gets 0 */
#define SNX_CODE_ARENA_DEFAULT ((size_t)1 << 30)

/* Reserves an arena of reserve bytes (0 for SNX_CODE_ARENA_DEFAULT, at most 2 GiB), five eighths of it code, two
   eighths read-only data and the rest data. NULL with the reason in error */
SnxCodeLoader* snx_code_loader_new(size_t reserve, char* error, size_t error_size);
/* PowerPC64: an arena whose loads keep toc in r2, the TOC of the code outside the arena that they call and jump to
   directly (an AOT module's functions, the table's names): placed within 32-bit reach of toc, at most reserve bytes
   (0 for SNX_CODE_ARENA_DEFAULT), less when no such room is free. Code that uses no TOC at all may be given any
   address near it, which the arena's own code then keeps in r2. NULL with the reason in error. Other hosts have no
   TOC and get snx_code_loader_new's arena */
SnxCodeLoader* snx_code_loader_new_toc(size_t reserve, SnxU64 toc, char* error, size_t error_size);
/* The same for objects of another machine (an ELF e_machine: 62 x86-64, 183 AArch64; or SNX_CODE_COFF_X64,
   SNX_CODE_COFF_ARM64), for tests of the loader on a host that cannot run them: their code is placed and relocated, and
   must never run */
SnxCodeLoader* snx_code_loader_new_machine(size_t reserve, unsigned machine, char* error, size_t error_size);
/* An arena of the regions' own sizes in bytes (code, read-only data, data), each rounded up to whole pages and to
   64 KiB (Windows' allocation granularity, where views of a section go), at most 2 GiB in all: for a game's code
   loaded whole, which snx_code_need sizes, rather than a total in fixed shares. NULL with the reason in error */
SnxCodeLoader* snx_code_loader_new_regions(const size_t size[3], char* error, size_t error_size);
/* The same for objects of another machine, as snx_code_loader_new_machine */
SnxCodeLoader* snx_code_loader_new_regions_machine(const size_t size[3], unsigned machine, char* error,
                                                   size_t error_size);
/* What snx_code_load of count ELF objects (of any one machine) takes at most of each region (code, read-only data,
   data, in that order), from their section headers and relocations: their sections with every alignment, a stub or
   veneer for each symbol an object calls, and a GOT slot for each it calls or reaches through the GOT. 0, or -1 with
   the reason in error */
int snx_code_need(const SnxLinkInput* inputs, size_t count, size_t need[3], char* error, size_t error_size);
/* Gives the arena back: nothing may run the loaded code any more */
void snx_code_loader_free(SnxCodeLoader* loader);
/* Defines name at address for the loads that follow. 0, or -1 when the table has the name already */
int snx_code_define(SnxCodeLoader* loader, const char* name, SnxU64 address);
/* Names neither a load nor the table defines are asked of resolver(user, name): their address, 0 for none. For names
   too many to define one by one (the runtime JIT's snx_aot_<address>, an AOT module's functions) */
void snx_code_set_resolver(SnxCodeLoader* loader, SnxU64 (*resolver)(void* user, const char* name), void* user);
/* 1 and *address when name is in the table: an earlier load's global or a defined name */
int snx_code_lookup(const SnxCodeLoader* loader, const char* name, SnxU64* address);
/* Loads count objects as one unit: they resolve each other's globals, which join the table for later loads.
   addresses[i] receives names[i]'s address, a global of the unit or of the table. 0, or -1 with the reason in error
   and the loader as it was before the call */
int snx_code_load(SnxCodeLoader* loader, const SnxLinkInput* inputs, size_t count, const char* const* names,
                  size_t name_count, SnxU64* addresses, char* error, size_t error_size);
/* The same, but the unit's globals stay its own: they don't join the table, and may repeat a name an earlier load
   has (each unit of the GNU ABI runtime's JIT steps defines a descriptor of the same name). Not COFF */
int snx_code_load_private(SnxCodeLoader* loader, const SnxLinkInput* inputs, size_t count, const char* const* names,
                          size_t name_count, SnxU64* addresses, char* error, size_t error_size);
/* In a forked child: the arena's code and read-only data copied into memory of the child's own, at the same
   addresses, where they were shared memory shared with the parent too (each process's later loads would write into
   the other's arena). 0, or -1 with the reason in error. Linux and the BSDs */
int snx_code_loader_unshare(SnxCodeLoader* loader, char* error, size_t error_size);
/* the bytes of the arena loads use, the three regions together, alignment padding included */
size_t snx_code_used(const SnxCodeLoader* loader);

#endif
