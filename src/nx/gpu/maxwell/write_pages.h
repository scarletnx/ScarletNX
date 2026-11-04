/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The generation of the last write to each 4K page, for telling a range changed without hashing it again */
#ifndef SNX_GPU_WRITE_PAGES_H
#define SNX_GPU_WRITE_PAGES_H
#include "common/cpu/aarch64/cpu.h"

/* Every page under 2^40 (all GPU and guest addresses) in a two-level table like the GMMU's: leaves of 4096 pages
   (16 MiB) made at the first write in them, each with its latest generation so a lookup skips the leaves nothing wrote
   since. Zeroed is empty. Writers are serialized by the caller; readers take no lock (write_pages.c) */
typedef struct WritePageLeaf WritePageLeaf;
typedef struct {
    WritePageLeaf* leaf[1u << 16];
    u64 overflow; /* the latest write the table couldn't hold (out of range, no memory), read as covering every page */
} WritePages;
void write_pages_stamp(WritePages* t, u64 a, u64 size, u64 gen);
/* nonzero if a page of [a,a+size) was stamped with a generation after since */
int write_pages_since(const WritePages* t, u64 since, u64 a, u64 size);
/* fn(ctx, at, n) for each run of [a,a+size) on pages stamped after since, in order, clipped to the range: pages that
   touch make one run. The whole range at once where the overflow stands for every page */
void write_pages_runs(const WritePages* t, u64 since, u64 a, u64 size, void (*fn)(void* ctx, u64 at, u64 n), void* ctx);

#endif
