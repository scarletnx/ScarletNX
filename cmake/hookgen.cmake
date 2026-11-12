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

# snx_hook_patches(<var>): the game patches, src/nx/patches/*.hook in name order, each a family of address hooks for
# one game's builds (docs/platforms/nx/hooks.md) whose handlers are the .c files beside them. A new file reconfigures the build
function(snx_hook_patches var)
    file(GLOB found CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/nx/patches/*.hook")
    list(SORT found)
    set(${var} ${found} PARENT_SCOPE)
endfunction()

# snx_hook_sdk(<var>): the SDK functions that the runtime answers natively, src/nx/nn/*.hook in name order, one
# spec for each SDK library whose handlers are the .c files beside them (fs.hook, fs.c). A new file reconfigures
# the build
function(snx_hook_sdk var)
    file(GLOB found CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/nx/nn/*.hook")
    list(SORT found)
    set(${var} ${found} PARENT_SCOPE)
endfunction()

# snx_hookgen(<target> [SPEC <file>...] [PREFIX <name>] [NAME <path>] [A32 | A32_MARKED]): generates NAME.h (the handlers' declarations,
# and the hooked functions' PFN...PROC types for a spec with an include line, which hookgen finds under src/) and
# NAME.inc (the adapters and the table, which hle/hook/hook.c includes for the hooks) into the build folder and
# gives <target> their include root. SPEC is src/nx/hle/hook/hooks.hook, the native SDK's (src/nx/nn/*.hook,
# snx_hook_sdk) and the game patches' address hooks (src/nx/patches/*.hook, snx_hook_patches) unless a test names its own, NAME is hle/hook/hooks.hook, and PREFIX (hook)
# names the tables: gpu/nvn/nvn.hook, the NVN functions the runtime implements, is a second spec with its own. A32 adds
# an adapter for a 32-bit program to every hook (hookgen --a32), A32_MARKED a 32-bit program's own table, of the hooks
# declared a32 and of those declared for it alone (hookgen --a32-marked)
function(snx_hookgen target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "A32;A32_MARKED" "PREFIX;NAME" "SPEC")
    set(a32)
    if(arg_A32)
        set(a32 --a32)
    elseif(arg_A32_MARKED)
        set(a32 --a32-marked)
    endif()
    snx_hook_patches(patches)
    snx_hook_sdk(sdk)
    set(spec "${PROJECT_SOURCE_DIR}/src/nx/hle/hook/hooks.hook" ${sdk} ${patches})
    if(arg_SPEC)
        set(spec ${arg_SPEC})
    endif()
    set(prefix hook)
    if(arg_PREFIX)
        set(prefix "${arg_PREFIX}")
    endif()
    set(name hle/hook/hooks.hook)
    if(arg_NAME)
        set(name "${arg_NAME}")
    endif()
    get_filename_component(dir "${name}" DIRECTORY)
    set(out "${CMAKE_CURRENT_BINARY_DIR}/hookgen/${target}")
    add_custom_command(OUTPUT "${out}/${name}.h" "${out}/${name}.inc"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}/${dir}"
        COMMAND snx-hookgen --prefix "${prefix}" ${a32} -I "${PROJECT_SOURCE_DIR}/src" --header "${out}/${name}.h"
                --out "${out}/${name}.inc" ${spec}
        DEPENDS snx-hookgen ${spec}
        COMMENT "Generating ${name}" VERBATIM)
    target_sources(${target} PRIVATE "${out}/${name}.h" "${out}/${name}.inc")
    target_include_directories(${target} PUBLIC "${out}")
endfunction()

# snx_hook_routes(<target> [SPECS <file>...]): generates nx/loader/hook_routes.inc, the symbols that the specs name
# (hookgen --routes), which nx/loader/hook_routes.c includes, and gives <target> its include root. The specs are
# hle/hook/hooks.hook, the native SDK's, gpu/nvn/nvn.hook and the game patches unless a test names its own
function(snx_hook_routes target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SPECS")
    snx_hook_patches(patches)
    snx_hook_sdk(sdk)
    set(specs "${PROJECT_SOURCE_DIR}/src/nx/hle/hook/hooks.hook" ${sdk} "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn.hook"
              ${patches})
    if(arg_SPECS)
        set(specs ${arg_SPECS})
    endif()
    set(out "${CMAKE_CURRENT_BINARY_DIR}/hookgen/${target}")
    add_custom_command(OUTPUT "${out}/nx/loader/hook_routes.inc"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}/nx/loader"
        COMMAND snx-hookgen -I "${PROJECT_SOURCE_DIR}/src" --routes "${out}/nx/loader/hook_routes.inc" ${specs}
        DEPENDS snx-hookgen ${specs}
        COMMENT "Generating nx/loader/hook_routes.inc" VERBATIM)
    target_sources(${target} PRIVATE "${out}/nx/loader/hook_routes.inc")
    target_include_directories(${target} PRIVATE "${out}")
endfunction()
