/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The ppc64 back end's AArch32 blocks for a big-endian PowerPC64 (ELFv2) host: ppc64_arm.c with PPC_BE 1 */
#define PPC_BE 1
#define SnxPpc64Arm SnxPpc64beArm
#define SnxPpc64ArmNext SnxPpc64beArmNext
#define snx_ppc64_arm_available snx_ppc64be_arm_available
#define snx_ppc64_arm_new snx_ppc64be_arm_new
#define snx_ppc64_arm_free snx_ppc64be_arm_free
#define snx_ppc64_arm_options   snx_ppc64be_arm_options
#define snx_ppc64_arm_block snx_ppc64be_arm_block
#define snx_ppc64_arm_thumb_block snx_ppc64be_arm_thumb_block
#define snx_ppc64_arm_why snx_ppc64be_arm_why
#define snx_ppc64_arm_object snx_ppc64be_arm_object
#define snx_ppc64_arm_limit snx_ppc64be_arm_limit
#define snx_ppc64_arm_absorb snx_ppc64be_arm_absorb
#define snx_ppc64_arm_entry snx_ppc64be_arm_entry
#define snx_ppc64_available snx_ppc64be_available
#include "common/cpu/recompiler/ppc64/ppc64be_arm.h"
#include "ppc64_arm.c"
