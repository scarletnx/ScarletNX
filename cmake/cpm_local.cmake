# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# CPM_USE_LOCAL_PACKAGES, CPM's own option (OFF by default, also read from the environment): a package the system has
# is taken from it with find_package, at the pinned version or newer, and the pinned source is downloaded for the
# rest. CPM asks find_package for every package it adds, and one it finds is left without a source folder, so the
# option reaches only the packages cmake/dependencies.cmake adds with snx_cpm_local_package, the ones that link as
# their own package installs them. Every other CPMAddPackage keeps the pinned source: a target of ours compiles it
# with its own flags, a change of ours goes on it, or it is data.
include_guard(GLOBAL)
set(snx_cpm_use_local_packages "${CPM_USE_LOCAL_PACKAGES}")
set(CPM_USE_LOCAL_PACKAGES OFF)

# CPMAddPackage with CPM_USE_LOCAL_PACKAGES as it was given. A macro: CPMAddPackage sets the package's variables in
# its caller's scope
macro(snx_cpm_local_package)
    set(CPM_USE_LOCAL_PACKAGES "${snx_cpm_use_local_packages}")
    CPMAddPackage(${ARGN})
    set(CPM_USE_LOCAL_PACKAGES OFF)
endmacro()

# The registry (vk.xml) a system's Vulkan-Headers package installs beside its headers, in share/vulkan/registry of
# the same prefix: the Vulkan settings are checked against it (cmake/settings.cmake)
function(snx_local_vulkan_registry out)
    get_target_property(include Vulkan::Headers INTERFACE_INCLUDE_DIRECTORIES)
    list(GET include 0 include)
    cmake_path(SET registry NORMALIZE "${include}/../share/vulkan/registry/vk.xml")
    if(NOT EXISTS "${registry}")
        message(FATAL_ERROR "CPM_USE_LOCAL_PACKAGES=${snx_cpm_use_local_packages} took the system's Vulkan headers in "
                            "${include}, whose registry ${registry} is not a file. Install the package that has it, "
                            "or configure with -DCPM_USE_LOCAL_PACKAGES=OFF for the pinned headers.")
    endif()
    set(${out} "${registry}" PARENT_SCOPE)
endfunction()
