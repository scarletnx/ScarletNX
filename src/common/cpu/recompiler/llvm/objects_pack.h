/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A module's relocatable objects in one file, for the host that loads them with the JIT loader (code_loader.h) instead
   of linking them: "snx-objs", their count, then each one's size and bytes, sizes and count as 8-byte little-endian
   numbers. The recompile of a game for iOS writes one (docs/hosts/apple.md), and the app loads it */
#ifndef SNX_RECOMPILER_LLVM_OBJECTS_PACK_H
#define SNX_RECOMPILER_LLVM_OBJECTS_PACK_H
#include "common/cpu/recompiler/llvm/link.h"

/* The pack of count inputs (their data and size), malloc'd, or NULL when out of memory */
unsigned char* snx_objects_pack(const SnxLinkInput* inputs, size_t count, size_t* size);
/* The same pack written to path a record at a time, so that the objects are never in memory twice. An input whose
   data is NULL is read when its turn comes by load(arg, i, &size, why, why_size): malloc'd, of the input's size, and
   freed once written, or NULL with the reason in why. written(arg, i), when given, is called once input i's record is
   written or has failed, so that the caller may free the input. 0 with the file's size in *size, or -1 with the reason
   in error: the file and the system's reason for a write, the input and load's reason for a read. The file is left as
   far as it got */
int snx_objects_pack_write(const SnxLinkInput* inputs, size_t count, const char* path, size_t* size,
                           unsigned char* (*load)(void* arg, size_t index, size_t* size, char* why, size_t why_size),
                           void (*written)(void* arg, size_t index), void* arg, char* error, size_t error_size);
/* The inputs of the pack at data (size bytes), whose data point into it, malloc'd with their count in *count; each
   name is name. NULL when it is not a pack, is cut short or is out of memory */
SnxLinkInput* snx_objects_unpack(const unsigned char* data, size_t size, const char* name, size_t* count);

#endif
