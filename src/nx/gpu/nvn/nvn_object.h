/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_direct's NVN objects (gpu/nvn/nvn_object.c): the device, memory pools, buffers, textures, samplers, programs
   and state objects, and their builders. What the command buffer and queue files share with it */
#ifndef SNX_NVN_OBJECT_H
#define SNX_NVN_OBJECT_H
#include "nx/gpu/nvn/nvn_obj.h"
#include "nx/memory/mem.h"
#include "nx/gpu/shader/shader.h"

/* an NVN object of type T at guest address a, in place in the game's memory */
#define NVN_OBJ(T, a) ((T*)(void*)GP(a))

/* a new serial for a texture, sampler or program: above 0, never reused */
u64 nvn_serial(void);
/* a memory pool object's storage: its guest address (0 for none), and its GPU address */
u64 nvn_pool_memory(u64 pool);
u64 nvn_pool_gpu(u64 pool);

/* the transform feedback layout of the program SetShaders gave serial, 0 when it captures nothing */
int nvn_program_xfb(u64 serial, ShXfb* x);
/* the game's driver resolved nvnDeviceGetInteger to func: its answers are read from its code (gpu/nvn/nvn_driver.c)
   before the game can ask */
void nvn_device_driver(u64 func);
/* the NVN API version the game's driver answers (nvn_api_pack), 0 until nvn_device_driver read it from the driver's
   code, or when it couldn't */
u32 nvn_device_api(void);

#endif
