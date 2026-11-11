# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# settingsgen (tools/settingsgen/settingsgen.c) generates the settings from src/nx/runtime/settings.txt: snx_settings.h
# (struct Settings, g_settings), their tables and LLVM's (its pipelines' passes, read from what the LLVM install's opt
# prints for -O0 to -Oz, and the options its C API sets, read from its headers) and the Vulkan extensions' (checked
# against the Khronos registry, vk.xml of the Vulkan-Headers the renderer builds with). It runs during the build, so a
# cross build also builds it for the machine doing the build (SNX_HOST_CC), as svcgen.cmake does for svcgen.
#
#   snx_settings          g_settings, the launcher's and the recompiler's tables, ini.c: the launcher, the driver
#   snx_settings_runtime  the game's .ini's tables, a file each, and its sections: the set functions are the runtime's
#   SNX_SETTINGS_LLVM     settings_llvm.c, for the LLVM backend's side of the driver
include_guard(GLOBAL)
if(CMAKE_CROSSCOMPILING)
    include("${CMAKE_CURRENT_LIST_DIR}/host_cc.cmake")
    include(ExternalProject)
    set(snx_settingsgen_dir "${PROJECT_BINARY_DIR}/settingsgen-host")
    set(snx_settingsgen_exe "${snx_settingsgen_dir}/bin/settingsgen")
    if(CMAKE_HOST_WIN32)
        string(APPEND snx_settingsgen_exe ".exe")
    endif()
    ExternalProject_Add(snx_settingsgen_host
        SOURCE_DIR "${PROJECT_SOURCE_DIR}/tools/settingsgen"
        BINARY_DIR "${snx_settingsgen_dir}"
        # no -Werror: the host compiler may not be clang (SNX_HOST_CC)
        CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=${SNX_HOST_CC}"
                   "-DCMAKE_C_FLAGS=-pedantic-errors -Wall -Wextra"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${snx_settingsgen_dir}/bin"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=${snx_settingsgen_dir}/bin"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release
        BUILD_ALWAYS 1
        BUILD_BYPRODUCTS "${snx_settingsgen_exe}"
        INSTALL_COMMAND "")
    add_executable(snx-settingsgen IMPORTED GLOBAL)
    set_target_properties(snx-settingsgen PROPERTIES IMPORTED_LOCATION "${snx_settingsgen_exe}")
    add_dependencies(snx-settingsgen snx_settingsgen_host)
else()
    add_executable(snx-settingsgen "${PROJECT_SOURCE_DIR}/tools/settingsgen/settingsgen.c")
    set_target_properties(snx-settingsgen PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON
                                                     C_EXTENSIONS OFF RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
    target_compile_options(snx-settingsgen PRIVATE -pedantic-errors -Wall -Wextra -Werror)
endif()

set(snx_settings_out "${PROJECT_BINARY_DIR}/settingsgen")
file(MAKE_DIRECTORY "${snx_settings_out}")
set(snx_settings_source "${PROJECT_SOURCE_DIR}/src/nx/runtime/settings.txt")
set(snx_settings_spec "${snx_settings_source}")
# A phone's JIT compiles at O0 (guest_jit.c): any other level breaks it there, so its level is no setting of a phone's
# and the spec has no row or key for it
if(ANDROID OR IOS)
    file(READ "${snx_settings_spec}" snx_settings_text)
    string(REGEX REPLACE "# SNX_PHONE_BEGIN[^#]*# SNX_PHONE_END\r?\n" "" snx_settings_text "${snx_settings_text}")
    if(snx_settings_text MATCHES "SNX_PHONE_BEGIN")
        message(FATAL_ERROR "settings.txt: a phone block has no end marker")
    endif()
    set(snx_settings_spec "${snx_settings_out}/settings.txt")
    file(WRITE "${snx_settings_spec}" "${snx_settings_text}")
endif()
set(snx_settings_args "")

# LLVM's: the headers of the LLVM the backend links, and the pipelines its opt prints (llvm_opt.cmake). An opt of
# another version, even in its last number, prints pipelines that are not this LLVM's: it is refused, said, and gives
# no pass toggles (the passes are LLVM's own then), as none does (a cross build's target opt cannot run here)
if(SNX_BUILD_NX)
    include("${CMAKE_CURRENT_LIST_DIR}/llvm.cmake")
    list(APPEND snx_settings_args --llvm-include "${SNX_LLVM_INCLUDE_DIR}")
    string(REGEX MATCH "^[0-9]+" snx_llvm_major "${SNX_LLVM_VERSION}")
    set(snx_opt_hints "")
    if(SNX_LLVM_CONFIG)
        execute_process(COMMAND "${SNX_LLVM_CONFIG}" --bindir OUTPUT_VARIABLE snx_llvm_bindir
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        list(APPEND snx_opt_hints "${snx_llvm_bindir}")
    endif()
    if(LLVM_TOOLS_BINARY_DIR)
        list(APPEND snx_opt_hints "${LLVM_TOOLS_BINARY_DIR}")
    endif()
    find_program(SNX_LLVM_OPT NAMES opt opt-${snx_llvm_major} HINTS ${snx_opt_hints}
                 DOC "opt of the LLVM the backend links, which prints its pipelines for the LLVM settings")
    # the target the backend makes code for, LLVMGetDefaultTargetTriple of the LLVM it links (ir.c): the pipelines
    # carry the passes a target adds (AArch64's loop-idiom-vectorize), which the host's opt would not print on its own
    set(snx_llvm_triple "")
    if(SNX_LLVM_CONFIG)
        execute_process(COMMAND "${SNX_LLVM_CONFIG}" --host-target OUTPUT_VARIABLE snx_llvm_triple
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    elseif(LLVM_TARGET_TRIPLE)
        set(snx_llvm_triple "${LLVM_TARGET_TRIPLE}")
    elseif(LLVM_HOST_TRIPLE)
        set(snx_llvm_triple "${LLVM_HOST_TRIPLE}")
    endif()
    set(snx_pipelines "${snx_settings_out}/pipelines.txt")
    set(snx_pipelines_text "")
    if(SNX_LLVM_OPT AND snx_llvm_triple STREQUAL "")
        message(WARNING "LLVM settings: the LLVM's target triple is unknown (no llvm-config --host-target, no "
                        "LLVM_TARGET_TRIPLE), so LLVM Internals has no pass toggles")
    elseif(SNX_LLVM_OPT)
        include("${CMAKE_CURRENT_LIST_DIR}/llvm_opt.cmake")
        snx_llvm_pipelines("${SNX_LLVM_OPT}" "${SNX_LLVM_VERSION}" "${snx_llvm_triple}" "${snx_settings_out}"
                           snx_pipelines_text snx_opt_why)
        if(NOT snx_opt_why STREQUAL "")
            message(WARNING "LLVM settings: ${snx_opt_why}")
        endif()
    endif()
    if(snx_pipelines_text STREQUAL "")
        message(STATUS "LLVM settings: no opt of LLVM ${SNX_LLVM_VERSION} runs here, so LLVM Internals has no pass "
                       "toggles")
    else()
        message(STATUS "LLVM settings: the passes of ${SNX_LLVM_OPT}'s pipelines")
    endif()
    file(CONFIGURE OUTPUT "${snx_pipelines}" CONTENT "${snx_pipelines_text}")
    list(APPEND snx_settings_args --pipelines "${snx_pipelines}")
endif()

# the Vulkan extensions' settings checked against the Khronos registry, which gives their help. A build without the
# Vulkan-Headers (the tools alone) makes the same settings, unchecked and with less help
set(snx_vk_registry "")
if(DEFINED Vulkan-Headers_SOURCE_DIR AND EXISTS "${Vulkan-Headers_SOURCE_DIR}/registry/vk.xml")
    set(snx_vk_registry "${Vulkan-Headers_SOURCE_DIR}/registry/vk.xml")
elseif(DEFINED snx_vulkan_local_registry) # the system's headers' (CPM_USE_LOCAL_PACKAGES)
    set(snx_vk_registry "${snx_vulkan_local_registry}")
endif()
if(snx_vk_registry)
    list(APPEND snx_settings_args --vk-registry "${snx_vk_registry}")
endif()

# a file for each game's table, runtime or vulkan (settings_<name>.c, settingsgen's table_file), named from
# settings.txt's table lines
file(STRINGS "${snx_settings_spec}" snx_settings_tables REGEX "^table ")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${snx_settings_source}")
set(snx_settings_runtime_files settings_runtime.c)
foreach(line IN LISTS snx_settings_tables)
    # nested: if() evaluates a parenthesized condition before the MATCHES that sets CMAKE_MATCH_<n>
    if(line MATCHES "^table +([A-Za-z0-9_]+) *([a-z]*)")
        set(scope "${CMAKE_MATCH_2}")
        string(REGEX REPLACE "^g_" "" name "${CMAKE_MATCH_1}")
        string(REGEX REPLACE "_ini$" "" name "${name}")
        if(scope STREQUAL "" OR scope STREQUAL "runtime" OR scope STREQUAL "vulkan")
            list(APPEND snx_settings_runtime_files "settings_${name}.c")
        endif()
    endif()
endforeach()
list(TRANSFORM snx_settings_runtime_files PREPEND "${snx_settings_out}/")
set(snx_settings_files snx_settings.h settings.c settings_launch.c settings_llvm.c)
list(TRANSFORM snx_settings_files PREPEND "${snx_settings_out}/" OUTPUT_VARIABLE snx_settings_outputs)
list(APPEND snx_settings_outputs ${snx_settings_runtime_files})
add_custom_command(OUTPUT ${snx_settings_outputs}
    COMMAND snx-settingsgen --out "${snx_settings_out}" ${snx_settings_args} "${snx_settings_spec}"
    DEPENDS snx-settingsgen "${snx_settings_spec}" $<$<BOOL:${SNX_BUILD_NX}>:${snx_pipelines}> ${snx_vk_registry}
    COMMENT "Generating the settings" VERBATIM)
add_custom_target(snx_settings_generated DEPENDS ${snx_settings_outputs})

add_library(snx_settings STATIC "${snx_settings_out}/settings.c" "${snx_settings_out}/settings_launch.c"
                                "${PROJECT_SOURCE_DIR}/src/nx/runtime/ini.c")
set_target_properties(snx_settings PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS ON) # ini.c: __atomic
target_compile_options(snx_settings PRIVATE -Wall -Wextra -Werror=missing-prototypes -Werror=missing-variable-declarations)
target_include_directories(snx_settings PUBLIC "${snx_settings_out}" "${PROJECT_SOURCE_DIR}/src")
# the tables' rows a needs block gates resolve by the build's own macros (ini.h's INI_NEEDS_): the library that compiles
# them needs the same definitions the runtime and the launcher take, so the flag is set exactly where the feature is
# absent. Private: only this library's own compile resolves it, and a consumer that reads the tables' rows (the launcher,
# a test) takes the flag from the text the runtime prints rather than from a definition. Nothing here links the client.
if(SNX_TRACY)
    target_compile_definitions(snx_settings PRIVATE SNX_TRACY=1)
endif()
add_dependencies(snx_settings snx_settings_generated)
# a library, so a program links only the tables it uses (a test, one module's), and their set functions
add_library(snx_settings_runtime STATIC ${snx_settings_runtime_files})
set_target_properties(snx_settings_runtime PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
if(SNX_TRACY)
    target_compile_definitions(snx_settings_runtime PRIVATE SNX_TRACY=1)
endif()
target_link_libraries(snx_settings_runtime PUBLIC snx_settings)
set(SNX_SETTINGS_LLVM "${snx_settings_out}/settings_llvm.c")
