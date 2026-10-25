/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_direct's NVN objects (gpu/nvn/nvn_object.c): the device, memory pools, buffers, textures, samplers, programs
   and state objects, and their builders. What the command buffer and queue files share with it */
#ifndef SNX_NVN_OBJECT_H
#define SNX_NVN_OBJECT_H
#include "nx/gpu/nvn/nvn_obj.h"
#include "nx/memory/mem.h"

/* an NVN object of type T at guest address a, in place in the game's memory */
#define NVN_OBJ(T, a) ((T*)(void*)GP(a))

/* a new serial for a texture, sampler or program: above 0, never reused */
u64 nvn_serial(void);
/* the storage of a memory pool object (its guest address, the GPU address in phase 1), 0 for none */
u64 nvn_pool_memory(u64 pool);

/* the game's driver resolved nvnDeviceGetInteger to func: its answers are read from its code (gpu/nvn/nvn_driver.c)
   before the game can ask */
void nvn_device_driver(u64 func);

#endif
