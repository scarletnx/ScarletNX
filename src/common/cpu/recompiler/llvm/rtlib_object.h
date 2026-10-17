/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime library's object (rtlib.c compiled when the recompiler is built, embedded by snx-rtlib-gen into the
   generated rtlib_object.c): linked into every AOT module (module_llvm.c) and loaded once into the JIT's code arena
   (jit_service.c) */
#ifndef SNX_RECOMPILER_LLVM_RTLIB_OBJECT_H
#define SNX_RECOMPILER_LLVM_RTLIB_OBJECT_H

extern const unsigned char snx_rtlib_object[];
extern const unsigned long snx_rtlib_object_size;

#endif
