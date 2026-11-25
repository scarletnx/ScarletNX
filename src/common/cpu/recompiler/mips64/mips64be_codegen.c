/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The mips64 back end's guest functions for a big-endian MIPS64 (n64) host: mips64_codegen.c with MIPS_BE 1 */
#define MIPS_BE 1
#define snx_mips64_available snx_mips64be_available
#define snx_mips64_object snx_mips64be_object
#define snx_mips64_desc snx_mips64be_desc
#include "common/cpu/recompiler/mips64/mips64be_codegen.h"
#include "mips64_codegen.c"
