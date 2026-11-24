/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The mips64 back end's AArch32 blocks for a big-endian MIPS64 (n64) host: mips64_arm.c with MIPS_BE 1 */
#define MIPS_BE 1
#define SnxMips64Arm SnxMips64beArm
#define SnxMips64ArmNext SnxMips64beArmNext
#define snx_mips64_arm_available snx_mips64be_arm_available
#define snx_mips64_arm_new snx_mips64be_arm_new
#define snx_mips64_arm_free snx_mips64be_arm_free
#define snx_mips64_arm_block snx_mips64be_arm_block
#define snx_mips64_arm_why snx_mips64be_arm_why
#define snx_mips64_arm_object snx_mips64be_arm_object
#define snx_mips64_arm_limit snx_mips64be_arm_limit
#define snx_mips64_arm_absorb snx_mips64be_arm_absorb
#define snx_mips64_arm_entry snx_mips64be_arm_entry
#define snx_mips64_available snx_mips64be_available
#include "common/cpu/recompiler/mips64/mips64be_arm.h"
#include "mips64_arm.c"
