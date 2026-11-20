# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# ScarletNX's version is the sum of these numbers (cmake/version.cmake), each <file>:<name>, a "#define <name> <number>"
# in that file: what the code a recompiler made and the runtime that runs it must agree on. Each only rises, so the
# version rises by one with every bump of one of them
set(SNX_VERSION_PARTS
    src/common/cpu/aarch64/cpu.h:SNX_ABI # an A64 game's modules
    src/nx/loader/module_abi.h:SNX_ROUTES_VERSION # their route list
    src/nx/loader/nro_slots.h:SNX_NROS_VERSION # their table of NROs
    src/nx/loader/nro_slots.h:SNX_NRO_IMPORTS_VERSION # an NRO's imports
    src/common/cpu/recompiler/variants.h:SNX_VARIANTS_VERSION # their variants
    src/nx/loader/module_abi.h:SNX_ARM_MODULE_VERSION # a 32-bit game's module
    src/common/cpu/arm/arm.h:SNX_ARM_ABI # SnxArmCPU, which the 32-bit, 3DS, GBA, DS and Vita code share
    src/common/cpu/arm/checked.h:SNX_ARM_CHECKED_ABI # the 3DS's checked memory
    src/common/cpu/arm/llvm.h:SNX_ARM_CODEGEN_OPTIONS_ABI # the AArch32 code generator's options
    src/3ds/aot/ctr_module.h:SNX_CTR_MODULE_ABI # a 3DS game's module
    src/3ds/aot/ctr_module.h:SNX_CTR_MODULE_CODEGEN_ABI
    src/agb/cpu/recomp.c:AGB_RECOMP_ABI # a GBA game's map and module
    src/nds/aot/nds_module.h:NDS_MODULE_ABI # a DS game's module
    src/vita/aot/module.h:VITA_MODULE_ABI) # a Vita game's module
# The numbers in src/ named as those are (a #define of a name ending in _ABI, or in _VERSION with a number) that the
# version leaves out, each with why: none makes a game recompile. tests/version_census.cmake fails on such a number in
# neither list
set(SNX_VERSION_LEFT_OUT
    src/nx/loader/module_abi.h:SNX_ARM_MODULE_ABI # SNX_ARM_MODULE_VERSION and SNX_ARM_ABI, both counted
    src/common/cpu/recompiler/llvm/ir.h:SNX_IR_OBJECT_VERSION # the unit cache's objects, compiled again
    src/nx/gpu/vk/pipeline_file.c:PF_VERSION # the pipeline file, made again
    src/nx/gpu/vk/pipeline.c:PF_GUEST_VERSION # the same
    src/nx/gpu/vk/bc7_cache.c:BC7_FILE_VERSION # the BC7 cache, started over
    src/nx/gpu/vk/vk.h:VK_BC7_ENCODER_VERSION # the same
    src/nx/gpu/shader/corpus.c:CORPUS_VERSION # the shader corpus, started over
    src/nx/gpu/gpu.c:GPU_STATE_VERSION # a save state's GPU section, refused saying why
    src/nx/gpu/nvn/nvn_save.c:NVN_STATE_VERSION # a save state's NVN section, the same
    src/common/utilities/setup.h:SNX_SETUP_VERSION # the first start's steps, asked again
    src/nx/hle/services/am.c:NACP_DISPLAY_VERSION # the console's
    src/nx/nn/ngc.c:NGC_CONTENT_VERSION # the console's
    src/nx/gpu/video/nvdec.c:VP8_MAX_VERSION # VP8's
    src/glslc/internal.h:GLSLC_MF_VERSION # GLSL's __VERSION__
    src/common/cpu/recompiler/llvm/link_macho.c:LC_BUILD_VERSION) # Mach-O's load command
