# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# cmake -DWORDS= -DOUT= -DNAME= -DHEADER= -DSOURCE= -P spirv_source.cmake: the SPIR-V module WORDS (its bytes as
# spirv-opt writes them, little-endian words) into the C file that holds them (cmake/glsl.cmake)
file(READ "${WORDS}" hex HEX)
string(LENGTH "${hex}" length)
math(EXPR rest "${length} % 8")
if(length EQUAL 0 OR NOT rest EQUAL 0)
    message(FATAL_ERROR "${WORDS} is no SPIR-V module: ${length} hex digits")
endif()
# each word's four bytes, low first, as one number; then eight of them a line
string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1u, " text "${hex}")
string(REGEX REPLACE "((0x........u, ){8})" "\\1\n" text "${text}")
string(REGEX REPLACE ", \n" ",\n" text "${text}")
file(WRITE "${OUT}.part" "/* Generated from ${SOURCE} by glslang and spirv-opt (cmake/glsl.cmake). Do not edit. */
#include \"${HEADER}\"
// clang-format off
const u32 ${NAME}[] = {
${text}};
// clang-format on
const u64 ${NAME}_size = sizeof ${NAME};
")
file(RENAME "${OUT}.part" "${OUT}")
