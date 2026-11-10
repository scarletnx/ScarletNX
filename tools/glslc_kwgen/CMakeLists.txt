# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# glslc_kwgen on its own: a cross build (src/glslc/CMakeLists.txt) builds it with this project for the machine it runs
# on, with src/glslc/builtins.c, whose keyword table it hashes
cmake_minimum_required(VERSION 3.25)
project(glslc_kwgen LANGUAGES C)
add_executable(glslc_kwgen glslc_kwgen.c "${CMAKE_CURRENT_SOURCE_DIR}/../../src/glslc/builtins.c")
target_include_directories(glslc_kwgen PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
set_target_properties(glslc_kwgen PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
