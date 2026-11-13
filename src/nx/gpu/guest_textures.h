/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The guest's live texture objects, as a front end that has them makes and ends them (NVN's textures): the GPU
   addresses of their storage, counted, as more than one object can be over one address. The renderer has no part in
   an object's life: its texture cache reads them here to measure how much it holds for objects the game has ended.
   A front end without texture objects (a game that writes its own command buffers) adds none. */
#ifndef SNX_GPU_GUEST_TEXTURES_H
#define SNX_GPU_GUEST_TEXTURES_H
#include "common/cpu/aarch64/cpu.h"

typedef struct {
    u64 gva, size; /* the storage's first GPU address, and the bytes of the largest object there */
    u32 refs;      /* the live objects at that address */
} GpuTexture;

/* a texture object made over size bytes at gva, and one at gva ended. From any thread */
void gpu_texture_add(u64 gva, u64 size);
void gpu_texture_remove(u64 gva);
/* the addresses with a live object copied to out, cap at most, in no order: how many there are */
u32 gpu_textures(GpuTexture* out, u32 cap);
/* whether an object is live at gva */
int gpu_texture_live(u64 gva);
/* the addresses whose last object ended since the last call, into out, cap at most: how many it gave. The rest stay
   for the next call. More than the list holds between two calls are dropped: the cache then learns nothing of those */
u32 gpu_textures_ended(u64* out, u32 cap);
/* the objects added since the start: 0 while no front end has texture objects */
u64 gpu_textures_added(void);
#endif
