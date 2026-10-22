# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# hookgen on its own: a cross build (cmake/hookgen.cmake) builds it with this project for the machine it runs on
cmake_minimum_required(VERSION 3.25)
project(hookgen LANGUAGES C)
add_executable(hookgen hookgen.c)
# strict ISO C99, as the main build's strict C
set_target_properties(hookgen PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
