# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# areatexgen on its own: a cross build (cmake/areatex.cmake) builds it with this project for the machine it runs on
cmake_minimum_required(VERSION 3.25)
project(areatexgen LANGUAGES C)
add_executable(areatexgen areatexgen.c)
# strict ISO C23, as the main build's strict C
set_target_properties(areatexgen PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
# every rounding of the texture's arithmetic as AreaTex.py's (areatexgen.c)
target_compile_options(areatexgen PRIVATE -ffp-contract=off)
if(NOT WIN32)
    target_link_libraries(areatexgen PRIVATE m)
endif()
