# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# svcgen on its own: a cross build (cmake/svcgen.cmake) builds it with this project for the machine it runs on
cmake_minimum_required(VERSION 3.25)
project(svcgen LANGUAGES C)
add_executable(svcgen svcgen.c)
# strict ISO C23, as the main build's strict C
set_target_properties(svcgen PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
