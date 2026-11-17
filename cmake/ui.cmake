# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# uigen (tools/uigen/uigen.c) generates the launcher's scenes from src/gui/ui.txt: gui/ui.h and gui/ui.c. It runs
# during the build, so a cross build also builds it for the machine doing the build, with that machine's C compiler
# (SNX_HOST_CC), as svcgen.cmake does for svcgen.
include_guard(GLOBAL)
if(CMAKE_CROSSCOMPILING)
    include("${CMAKE_CURRENT_LIST_DIR}/host_cc.cmake")
    include(ExternalProject)
    set(snx_uigen_dir "${PROJECT_BINARY_DIR}/uigen-host")
    set(snx_uigen_exe "${snx_uigen_dir}/bin/uigen")
    if(CMAKE_HOST_WIN32)
        string(APPEND snx_uigen_exe ".exe")
    endif()
    ExternalProject_Add(snx_uigen_host
        SOURCE_DIR "${PROJECT_SOURCE_DIR}/tools/uigen"
        BINARY_DIR "${snx_uigen_dir}"
        # no -Werror: the host compiler may not be clang (SNX_HOST_CC)
        CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=${SNX_HOST_CC}"
                   "-DCMAKE_C_FLAGS=-pedantic-errors -Wall -Wextra"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${snx_uigen_dir}/bin"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=${snx_uigen_dir}/bin"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release
        BUILD_ALWAYS 1
        BUILD_BYPRODUCTS "${snx_uigen_exe}"
        INSTALL_COMMAND "")
    add_executable(snx-uigen IMPORTED GLOBAL)
    set_target_properties(snx-uigen PROPERTIES IMPORTED_LOCATION "${snx_uigen_exe}")
    add_dependencies(snx-uigen snx_uigen_host)
else()
    add_executable(snx-uigen "${PROJECT_SOURCE_DIR}/tools/uigen/uigen.c")
    set_target_properties(snx-uigen PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
                                               RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
    target_compile_options(snx-uigen PRIVATE -pedantic-errors -Wall -Wextra -Werror)
endif()
