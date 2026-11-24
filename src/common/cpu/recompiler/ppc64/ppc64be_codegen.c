/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The ppc64 back end's guest functions for a big-endian PowerPC64 (ELFv2) host: ppc64_codegen.c with PPC_BE 1 */
#define PPC_BE 1
#define snx_ppc64_available snx_ppc64be_available
#define snx_ppc64_object snx_ppc64be_object
#define snx_ppc64_desc snx_ppc64be_desc
#include "common/cpu/recompiler/ppc64/ppc64be_codegen.h"
#include "ppc64_codegen.c"
