# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
# libadrenotools and its liblinkernsbypass (both BSD-2-Clause, Billy Laws), which load a Vulkan driver package the
# player added (src/nx/runtime/platform/gpu_driver_android.c) in place of an Adreno GPU's own driver on Android arm64.
# Built here from their sources, as the three libraries the app's libraries folder carries: libadrenotools.so
# (driver.cpp with linkernsbypass), which a run opens only once it picked such a driver, so linkernsbypass's constructor
# (it makes the linker's pages writable to reach its namespaces) never runs with the system's driver; and the hooks it
# loads by name from that folder, libhook_impl.so and libmain_hook.so. Their CMakeLists would build the file
# redirection and GPU mapping hooks too, which ScarletNX does not use. The hook loads a package whose soname the
# process has already (Qualcomm's own drivers on Android 16) from a renamed copy (cmake/adrenotools-unique-soname.patch),
# so it builds from a patched copy of the release in the build tree. This file is included into the top-level scope:
# its variables carry the snx_adrenotools_ prefix.
set(snx_adrenotools_src "${PROJECT_BINARY_DIR}/_deps/libadrenotools-patched")
set(snx_linkerns_src "${linkernsbypass_SOURCE_DIR}")
include("${PROJECT_SOURCE_DIR}/cmake/patched_copy.cmake")
snx_patched_copy(SOURCE "${libadrenotools_SOURCE_DIR}" DEST "${snx_adrenotools_src}"
                 PATCHES "${PROJECT_SOURCE_DIR}/cmake/adrenotools-unique-soname.patch"
                 FILES src/hook/hook_impl.cpp 4599d573849138e6302790f94aed1c70bb22baf69e206ce54a2d09bad0840932)
set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${PROJECT_SOURCE_DIR}/cmake/adrenotools-unique-soname.patch")

add_library(snx_linkernsbypass STATIC "${snx_linkerns_src}/android_linker_ns.cpp"
            "${snx_linkerns_src}/elf_soname_patcher.cpp")
target_include_directories(snx_linkernsbypass PUBLIC "${snx_linkerns_src}")
target_link_libraries(snx_linkernsbypass PUBLIC android dl)
set_target_properties(snx_linkernsbypass PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(snx_adrenotools SHARED "${snx_adrenotools_src}/src/driver.cpp")
target_include_directories(snx_adrenotools PUBLIC "${snx_adrenotools_src}/include" PRIVATE "${snx_adrenotools_src}")
target_link_libraries(snx_adrenotools PRIVATE snx_linkernsbypass log)
set_target_properties(snx_adrenotools PROPERTIES OUTPUT_NAME adrenotools)

# the names driver.cpp and hook_impl.cpp open them by
add_library(snx_adrenotools_hook_impl SHARED "${snx_adrenotools_src}/src/hook/hook_impl.cpp")
target_include_directories(snx_adrenotools_hook_impl PRIVATE "${snx_adrenotools_src}/include")
target_link_libraries(snx_adrenotools_hook_impl PRIVATE snx_linkernsbypass log)
set_target_properties(snx_adrenotools_hook_impl PROPERTIES OUTPUT_NAME hook_impl CXX_VISIBILITY_PRESET hidden)

add_library(snx_adrenotools_main_hook SHARED "${snx_adrenotools_src}/src/hook/main_hook.c")
target_link_options(snx_adrenotools_main_hook PRIVATE -Wl,-z,global)
target_link_libraries(snx_adrenotools_main_hook PRIVATE snx_adrenotools_hook_impl)
set_target_properties(snx_adrenotools_main_hook PROPERTIES OUTPUT_NAME main_hook)

snx_quiet(snx_linkernsbypass snx_adrenotools snx_adrenotools_hook_impl snx_adrenotools_main_hook)
include(GNUInstallDirs)
install(TARGETS snx_adrenotools snx_adrenotools_hook_impl snx_adrenotools_main_hook
        LIBRARY DESTINATION lib/${ANDROID_ABI})
install(FILES "${snx_adrenotools_src}/LICENSE" DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/scarletnx/libadrenotools)
install(FILES "${snx_linkerns_src}/LICENSE"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/scarletnx/liblinkernsbypass)
