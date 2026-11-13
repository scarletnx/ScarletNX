# SPDX-License-Identifier: MPL-2.0
# Copyright 2026 the mGBA contributors
# Copyright 2026 the ScarletNX contributors
# The AGB (Game Boy Advance) board runtime: the board emulation scarletnx-agb runs GBA ROMs on. The board
# library is independent of the NX and GNU ABI runtimes; the executable is its system type entry point.
#
#   scarletnx-agb-runtime   the board: cartridge, memory map, devices, and the AOT recompiler
#   snx_agb_program         the runner: the board in a window, and its command line, which scarletnx runs as
#                           scarletnx-agb (src/multicall)

# The ARM and Thumb recompiler of src/common/cpu/arm, the same one the other system types use.
if(NOT TARGET snx_arm_llvm)
    include("${PROJECT_SOURCE_DIR}/cmake/llvm.cmake")
    add_subdirectory("${PROJECT_SOURCE_DIR}/src/common/cpu/arm" agb-arm-backend)
endif()

add_library(scarletnx-agb STATIC board/board.c board/memory.c board/dma.c board/timers.c board/save.c board/gpio.c
                                 cartridge/cartridge.c bios/bios.c ppu/ppu.c apu/apu.c cpu/memmap.c cpu/recomp.c
                                 cpu/cpu.c)
set_target_properties(scarletnx-agb PROPERTIES OUTPUT_NAME scarletnx-agb-runtime C_STANDARD 23 C_STANDARD_REQUIRED ON)
# The LLVM backend (snx_llvm, which the Switch driver's tree, the GNU ABI runtime's or the top level configures):
# the module's objects and their helpers (armlib.c) linked by its linker, and the blocks a run reaches compiled in
# this process by its executor (exec.c), as the 3DS and the Vita do
target_link_libraries(scarletnx-agb PUBLIC scarletnx-common snx_arm_llvm snx_llvm)
target_compile_options(scarletnx-agb PRIVATE -Wall -Wextra -Werror)

# the runner: the board in a window, with sound and the keypad (runtime/host.c), SDL here and never in the board. Its
# command line goes beside every GBA game (recompiled/<name>/<name>, its .ini saying system=gba)
add_library(snx_agb_program STATIC runtime/host.c runtime/main.c)
target_link_libraries(snx_agb_program PUBLIC scarletnx-agb SDL3::SDL3 snx_window_icon snx_pad_sdl snx_build_info)
set_target_properties(snx_agb_program PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
include("${PROJECT_SOURCE_DIR}/tests/cmake/agb.cmake") # its tests
