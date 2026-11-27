/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime library's object (rtlib.c compiled when the recompiler is built, embedded by snx-rtlib-gen into the
   generated rtlib_object.c): linked into every AOT module (module_a64.c) and loaded once into the JIT's code arena
   (jit_service.c). And armlib.c's in the host's object format (armlib_object.c): what a 32-bit module links
   (module_arm.c, the consoles' AOT) and, on powerpc64, the AArch32 JIT loads (cpu/arm/exec.c) */
#ifndef SNX_RECOMPILER_RTLIB_RTLIB_OBJECT_H
#define SNX_RECOMPILER_RTLIB_RTLIB_OBJECT_H

extern const unsigned char snx_rtlib_object[];
extern const unsigned long snx_rtlib_object_size;
extern const unsigned char snx_armlib_object[];
extern const unsigned long snx_armlib_object_size;

#endif
