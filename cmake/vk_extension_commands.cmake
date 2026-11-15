# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The device commands each Vulkan device extension requires (generated/vk_extension_commands.c, which
# src/common/gpu/vk/extension_commands.h declares), written by tools/vkcmdgen from the Khronos registry of the
# Vulkan-Headers the build has: the pinned ones' vk.xml, or the system's (CPM_USE_LOCAL_PACKAGES). It runs during the
# build, so a cross build also builds it for the machine doing the build (SNX_HOST_CC), as settings.cmake does for
# settingsgen
include_guard(GLOBAL)
if(DEFINED Vulkan-Headers_SOURCE_DIR AND EXISTS "${Vulkan-Headers_SOURCE_DIR}/registry/vk.xml")
    set(snx_vk_commands_registry "${Vulkan-Headers_SOURCE_DIR}/registry/vk.xml")
elseif(DEFINED snx_vulkan_local_registry)
    set(snx_vk_commands_registry "${snx_vulkan_local_registry}")
else()
    message(FATAL_ERROR "The Vulkan renderer's command table needs the Khronos registry (vk.xml) of the Vulkan-Headers "
                        "it builds with, and neither Vulkan-Headers_SOURCE_DIR (${Vulkan-Headers_SOURCE_DIR}) nor the "
                        "system's headers give one")
endif()
# for the generator's test (tests/vkcmdgen.cmake), which runs it on this registry too
set_property(GLOBAL PROPERTY snx_vk_commands_registry "${snx_vk_commands_registry}")
if(CMAKE_CROSSCOMPILING)
    include("${CMAKE_CURRENT_LIST_DIR}/host_cc.cmake")
    include(ExternalProject)
    set(snx_vkcmdgen_dir "${PROJECT_BINARY_DIR}/vkcmdgen-host")
    set(snx_vkcmdgen_exe "${snx_vkcmdgen_dir}/bin/vkcmdgen")
    if(CMAKE_HOST_WIN32)
        string(APPEND snx_vkcmdgen_exe ".exe")
    endif()
    ExternalProject_Add(snx_vkcmdgen_host
        SOURCE_DIR "${PROJECT_SOURCE_DIR}/tools/vkcmdgen"
        BINARY_DIR "${snx_vkcmdgen_dir}"
        # no -Werror: the host compiler may not be clang (SNX_HOST_CC)
        CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=${SNX_HOST_CC}"
                   "-DCMAKE_C_FLAGS=-pedantic-errors -Wall -Wextra"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${snx_vkcmdgen_dir}/bin"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=${snx_vkcmdgen_dir}/bin"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release
        BUILD_ALWAYS 1
        BUILD_BYPRODUCTS "${snx_vkcmdgen_exe}"
        INSTALL_COMMAND "")
    add_executable(snx-vkcmdgen IMPORTED GLOBAL)
    set_target_properties(snx-vkcmdgen PROPERTIES IMPORTED_LOCATION "${snx_vkcmdgen_exe}")
    add_dependencies(snx-vkcmdgen snx_vkcmdgen_host)
else()
    add_executable(snx-vkcmdgen "${PROJECT_SOURCE_DIR}/tools/vkcmdgen/vkcmdgen.c")
    set_target_properties(snx-vkcmdgen PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON
                                                  C_EXTENSIONS OFF RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
    target_compile_options(snx-vkcmdgen PRIVATE -pedantic-errors -Wall -Wextra -Werror)
endif()
set(snx_vk_extension_commands_c "${PROJECT_BINARY_DIR}/generated/vk_extension_commands.c")
add_custom_command(OUTPUT "${snx_vk_extension_commands_c}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROJECT_BINARY_DIR}/generated"
    COMMAND snx-vkcmdgen "${snx_vk_commands_registry}" "${snx_vk_extension_commands_c}"
    DEPENDS snx-vkcmdgen "${snx_vk_commands_registry}"
    COMMENT "Generating the Vulkan extensions' commands" VERBATIM)
