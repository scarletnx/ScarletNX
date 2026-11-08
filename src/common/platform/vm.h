/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host's address space, the same for every driver: reservations of addresses with no memory, and ranges of them
   committed, protected, decommitted and released. A driver's own needs stay on top (the NX runtime's written-page
   tracking and alias pool, the 3DS's views of shared memory objects into Windows placeholders). Sizes and addresses
   are whole host pages unless said */
#ifndef SNX_PLATFORM_VM_H
#define SNX_PLATFORM_VM_H
#include <stddef.h>
#include <stdint.h>

/* plat_mem_protect's access: none, or PLAT_MEM_READ with PLAT_MEM_WRITE or PLAT_MEM_EXEC or both */
enum { PLAT_MEM_NONE = 0, PLAT_MEM_READ = 1, PLAT_MEM_WRITE = 2, PLAT_MEM_EXEC = 4 };

size_t plat_mem_page(void); /* the host's page */
/* size bytes of addresses, inaccessible until committed, at an address that is a multiple of align (a power of two; 0
   or the host's page: no more than the page). POSIX maps align more than it needs and gives the rest back; Windows
   reserves at its 64 KiB allocation granularity, and above that frees and takes an aligned range again until no other
   thread took it meanwhile. NULL with the reason in errno */
void* plat_mem_reserve(uint64_t size, uint64_t align);
/* the same at exactly address: NULL, with EEXIST where any of the range is mapped. Linux's MAP_FIXED_NOREPLACE,
   FreeBSD's MAP_FIXED with MAP_EXCL, else the address as a hint, given back when the system put it elsewhere */
void* plat_mem_reserve_at(uint64_t address, uint64_t size);
/* [p, p + size) of a reservation becomes fresh zero pages, readable and writable, whatever was there (pages, a view
   of a file): 0, or -1. Pages cost nothing before they are touched */
int plat_mem_commit(void* p, uint64_t size);
/* reserved and committed at once, anywhere: fresh zero pages that cost nothing before they are touched. NULL with
   errno */
void* plat_mem_alloc(uint64_t size);
/* [p, p + size) inaccessible again, whatever was there, its pages given back to the system, and zero when committed
   next: 0, or -1 */
int plat_mem_decommit(void* p, uint64_t size);
/* the access of [p, p + size), whose contents stay: 0, or -1. Windows commits reserved pages first for an access other
   than none (VirtualProtect changes only committed ones), and keeps them committed for none */
int plat_mem_protect(void* p, uint64_t size, unsigned access);
/* a whole reservation (or plat_mem_alloc's block), by its first address and size */
void plat_mem_release(void* p, uint64_t size);
#endif
