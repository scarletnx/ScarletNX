/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_HOST_MEMORY_H
#define SNX_HOST_MEMORY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* The host's side of a guest's flat address space (the 3DS's src/3ds/memory, the GBA's src/agb/cpu/memmap.c): the
   guest's RAM as host memory objects that can be mapped at several host addresses at once, a reservation the guest's
   addresses are offsets into, and pages of the objects mapped there as views, so every guest address of a page (a
   mirror, an alias) is the same memory. Linux, Android and NetBSD use a memfd, the other POSIX systems an unlinked
   shm_open object, Windows a pagefile section mapped into placeholders (Windows 10 1803). */

#define SNX_HOST_READ  1u
#define SNX_HOST_WRITE 2u
#define SNX_HOST_EXEC  4u /* read: blocks are compiled, never run there */

typedef struct {
    intptr_t handle;     /* the object: a file descriptor, or a Windows section HANDLE */
    unsigned char* view; /* all of it mapped once, readable and writable */
    uint64_t size;
} SnxHostMemory;

/* the size a view's address, offset and length are whole numbers of: the host's page; 0 where views cannot be made
   (a Windows older than 10 1803) */
size_t snx_host_page(void);
bool snx_host_memory_new(SnxHostMemory* memory, uint64_t size);
void snx_host_memory_free(SnxHostMemory* memory);
/* An object to take another's place (src/3ds/memory's snx_ctr_memory_adopt): its size bytes copied from bytes,
   through a mapping of its own, whatever memory's view shows. False when the object does not map */
bool snx_host_memory_fill(const SnxHostMemory* memory, const void* bytes);
/* The object's handle closed, its view not unmapped: another object's mapping over that view holds it from then on */
void snx_host_memory_close(SnxHostMemory* memory);
/* size bytes, inaccessible; NULL on failure */
unsigned char* snx_host_reserve(uint64_t size);
void snx_host_release(unsigned char* base, uint64_t size);
/* at, offset and size in pages; permissions SNX_HOST_READ, SNX_HOST_WRITE, SNX_HOST_EXEC */
bool snx_host_map(const SnxHostMemory* memory, unsigned char* base, uint64_t at, uint64_t offset, uint64_t size,
                  unsigned permissions);
/* private memory of zeros, readable and writable, in place of what the reservation holds there */
bool snx_host_commit(unsigned char* base, uint64_t at, uint64_t size);
bool snx_host_protect(unsigned char* base, uint64_t at, uint64_t size, unsigned permissions);
/* back to reserved and inaccessible */
bool snx_host_unmap(unsigned char* base, uint64_t at, uint64_t size);
#endif
