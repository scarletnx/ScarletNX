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
/* the flags of the device the game made last (the runtime has one) */
u32 nvn_device_flags(void);
/* the device the game made last (the runtime has one), 0 before: what the builders that don't keep one answer */
u64 nvn_device(void);
u64 nvn_pool_gpu(u64 pool);

/* Around a wait for the host inside an NVN call: the game's thread off its guest core and stopped as far as a game
   pause goes, as rt_miss waits for a compile. An NVN call comes straight from the game's code, with no SVC to give the
   core up, and on the console a thread waiting for the GPU leaves its core to the others: one game's main thread held
   its core through the frame's wait for its fence, and a lower-priority loading thread on that core never finished */
void nvn_wait_begin(void);
void nvn_wait_end(void);

/* the transform feedback layout of the program SetShaders gave serial, 0 when it captures nothing */
int nvn_program_xfb(u64 serial, ShXfb* x);
/* the game's driver resolved nvnDeviceGetInteger to func: its answers are read from its code (gpu/nvn/nvn_driver.c)
   before the game can ask */
void nvn_device_driver(u64 func);
/* the NVN API version the game's driver answers (nvn_api_pack), 0 until nvn_device_driver read it from the driver's
   code, or when it couldn't */
u32 nvn_device_api(void);

#endif
