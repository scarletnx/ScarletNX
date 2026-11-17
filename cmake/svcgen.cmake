# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# svcgen (tools/svcgen/svcgen.c) generates the SVC dispatch, each SVC's registers in both ABIs and the tables by
# number, from src/nx/hle/svc/hos.svc. It runs during the build, so a cross build also builds it for the machine doing
# the build, with that machine's C compiler (SNX_HOST_CC), as swipc.cmake does for swipc
include_guard(GLOBAL)
if(CMAKE_CROSSCOMPILING)
    include("${CMAKE_CURRENT_LIST_DIR}/host_cc.cmake")
    include(ExternalProject)
    set(snx_svcgen_dir "${PROJECT_BINARY_DIR}/svcgen-host")
    set(snx_svcgen_exe "${snx_svcgen_dir}/bin/svcgen")
    if(CMAKE_HOST_WIN32)
        string(APPEND snx_svcgen_exe ".exe")
    endif()
    ExternalProject_Add(snx_svcgen_host
        SOURCE_DIR "${PROJECT_SOURCE_DIR}/tools/svcgen"
        BINARY_DIR "${snx_svcgen_dir}"
        # no -Werror: the host compiler may not be clang (SNX_HOST_CC)
        CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=${SNX_HOST_CC}"
                   "-DCMAKE_C_FLAGS=-pedantic-errors -Wall -Wextra"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${snx_svcgen_dir}/bin"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=${snx_svcgen_dir}/bin"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release
        BUILD_ALWAYS 1
        BUILD_BYPRODUCTS "${snx_svcgen_exe}"
        INSTALL_COMMAND "")
    add_executable(snx-svcgen IMPORTED GLOBAL)
    set_target_properties(snx-svcgen PROPERTIES IMPORTED_LOCATION "${snx_svcgen_exe}")
    add_dependencies(snx-svcgen snx_svcgen_host)
else()
    add_executable(snx-svcgen "${PROJECT_SOURCE_DIR}/tools/svcgen/svcgen.c")
    set_target_properties(snx-svcgen PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
                                                RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
    target_compile_options(snx-svcgen PRIVATE -pedantic-errors -Wall -Wextra -Werror)
endif()

# snx_svcgen(<target>): generates hle/svc/hos.svc.inc, which hle/svc/svc.c includes, into the build folder and gives
# <target> its include root
function(snx_svcgen target)
    set(spec "${PROJECT_SOURCE_DIR}/src/nx/hle/svc/hos.svc")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/svcgen")
    add_custom_command(OUTPUT "${out}/hle/svc/hos.svc.inc"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}/hle/svc"
        COMMAND snx-svcgen --out "${out}/hle/svc/hos.svc.inc" "${spec}"
        DEPENDS snx-svcgen "${spec}"
        COMMENT "Generating the SVC dispatch" VERBATIM)
    target_sources(${target} PRIVATE "${out}/hle/svc/hos.svc.inc")
    target_include_directories(${target} PUBLIC "${out}")
endfunction()
