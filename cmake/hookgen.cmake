# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# hookgen (tools/hookgen/hookgen.c) generates the hooks' handler declarations, adapters and table from
# src/nx/hle/hook/hooks.hook. It runs during the build, so a cross build also builds it for the machine doing the
# build, with that machine's C compiler (SNX_HOST_CC), as svcgen.cmake does for svcgen
include_guard(GLOBAL)
if(CMAKE_CROSSCOMPILING)
    include("${CMAKE_CURRENT_LIST_DIR}/host_cc.cmake")
    include(ExternalProject)
    set(snx_hookgen_dir "${CMAKE_BINARY_DIR}/hookgen-host")
    set(snx_hookgen_exe "${snx_hookgen_dir}/bin/hookgen")
    if(CMAKE_HOST_WIN32)
        string(APPEND snx_hookgen_exe ".exe")
    endif()
    ExternalProject_Add(snx_hookgen_host
        SOURCE_DIR "${PROJECT_SOURCE_DIR}/tools/hookgen"
        BINARY_DIR "${snx_hookgen_dir}"
        # no -Werror: the host compiler may not be clang (SNX_HOST_CC)
        CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=${SNX_HOST_CC}"
                   "-DCMAKE_C_FLAGS=-pedantic-errors -Wall -Wextra"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${snx_hookgen_dir}/bin"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=${snx_hookgen_dir}/bin"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release
        BUILD_ALWAYS 1
        BUILD_BYPRODUCTS "${snx_hookgen_exe}"
        INSTALL_COMMAND "")
    add_executable(snx-hookgen IMPORTED GLOBAL)
    set_target_properties(snx-hookgen PROPERTIES IMPORTED_LOCATION "${snx_hookgen_exe}")
    add_dependencies(snx-hookgen snx_hookgen_host)
else()
    add_executable(snx-hookgen "${PROJECT_SOURCE_DIR}/tools/hookgen/hookgen.c")
    set_target_properties(snx-hookgen PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
                                                 RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_compile_options(snx-hookgen PRIVATE -pedantic-errors -Wall -Wextra -Werror)
endif()

# snx_hookgen(<target> [SPEC <file>]): generates hle/hook/hooks.hook.h (the handlers' declarations) and
# hle/hook/hooks.hook.inc (the adapters and the table, which hle/hook/hook.c includes) into the build folder and gives
# <target> their include root. SPEC is src/nx/hle/hook/hooks.hook unless a test names its own
function(snx_hookgen target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "SPEC" "")
    set(spec "${PROJECT_SOURCE_DIR}/src/nx/hle/hook/hooks.hook")
    if(arg_SPEC)
        set(spec "${arg_SPEC}")
    endif()
    set(out "${CMAKE_CURRENT_BINARY_DIR}/hookgen/${target}")
    add_custom_command(OUTPUT "${out}/hle/hook/hooks.hook.h" "${out}/hle/hook/hooks.hook.inc"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}/hle/hook"
        COMMAND snx-hookgen --header "${out}/hle/hook/hooks.hook.h" --out "${out}/hle/hook/hooks.hook.inc" "${spec}"
        DEPENDS snx-hookgen "${spec}"
        COMMENT "Generating the hooks" VERBATIM)
    target_sources(${target} PRIVATE "${out}/hle/hook/hooks.hook.h" "${out}/hle/hook/hooks.hook.inc")
    target_include_directories(${target} PUBLIC "${out}")
endfunction()
