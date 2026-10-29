/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_FONT_H
#define SNX_CTR_FONT_H
#include <stdbool.h>
#include <stdint.h>
struct SnxCtrProcess;
/* APT's shared font, as GetSharedFont hands it over: the stand-in system title's RomFS
   (cmake/ctr_system_archives.cmake, cbf_std.bcfnt.lz) loaded once into a shared memory block of the process, its CFNT
   pointers relocated for the address it is mapped at, and a handle to that block. The first call loads it, every call
   adds a handle. False when the stand-in is malformed or the block cannot be made. */
bool snx_ctr_font_shared(struct SnxCtrProcess* p, uint32_t* address, uint32_t* handle);
#endif
