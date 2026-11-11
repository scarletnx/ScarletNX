# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# CPM_USE_LOCAL_PACKAGES reaches the packages added with snx_cpm_local_package and no other (cmake/cpm_local.cmake):
# a small project adds three packages a prefix has, the second with it and the ones around it without, and the
# prefix's Vulkan::Headers with and without its registry. Run by ctest as cpm_local, with -DSRC, -DCPM (CPM.cmake)
# and -DWORK
file(REMOVE_RECURSE "${WORK}")
set(prefix "${WORK}/prefix")
foreach(name local pinned after)
    file(WRITE "${prefix}/share/cmake/snxtest${name}/snxtest${name}Config.cmake"
         "add_library(Vulkan::Headers INTERFACE IMPORTED)\n"
         "set_target_properties(Vulkan::Headers PROPERTIES INTERFACE_INCLUDE_DIRECTORIES \"${prefix}/include\")\n")
    file(WRITE "${prefix}/share/cmake/snxtest${name}/snxtest${name}ConfigVersion.cmake"
         "set(PACKAGE_VERSION 1.3.0)\nset(PACKAGE_VERSION_COMPATIBLE TRUE)\n")
    file(WRITE "${WORK}/source-${name}/file.txt" "the pinned source\n")
endforeach()
file(MAKE_DIRECTORY "${prefix}/include")
file(WRITE "${WORK}/project/CMakeLists.txt" "cmake_minimum_required(VERSION 3.25)
project(cpm_local_test LANGUAGES NONE)
include(\"${CPM}\")
include(\"${SRC}/cmake/cpm_local.cmake\")
CPMAddPackage(NAME snxtestpinned VERSION 1.2.0 SOURCE_DIR \"${WORK}/source-pinned\" DOWNLOAD_ONLY YES)
snx_cpm_local_package(NAME snxtestlocal VERSION 1.2.0 SOURCE_DIR \"${WORK}/source-local\" DOWNLOAD_ONLY YES)
CPMAddPackage(NAME snxtestafter VERSION 1.2.0 SOURCE_DIR \"${WORK}/source-after\" DOWNLOAD_ONLY YES)
set(registry \"\")
if(TARGET Vulkan::Headers)
    snx_local_vulkan_registry(registry)
endif()
message(STATUS \"RESULT local=[\${snxtestlocal_ADDED}|\${snxtestlocal_SOURCE_DIR}] \"
               \"pinned=[\${snxtestpinned_ADDED}|\${snxtestpinned_SOURCE_DIR}] \"
               \"after=[\${snxtestafter_ADDED}|\${snxtestafter_SOURCE_DIR}] registry=[\${registry}] \"
               \"option=[\${CPM_USE_LOCAL_PACKAGES}]\")
")

# configures the project with ARGN and checks its result line, or that it fails saying `expected`
function(check label expected)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=CPM_USE_LOCAL_PACKAGES --unset=CPM_LOCAL_PACKAGES_ONLY
                            --unset=CPM_SOURCE_CACHE "${CMAKE_COMMAND}" -S "${WORK}/project" -B "${WORK}/${label}"
                            "-DCMAKE_PREFIX_PATH=${prefix}" ${ARGN}
                    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
    string(REGEX REPLACE "\n *" " " flat "${out}") # CMake wraps an error's text
    string(FIND "${flat}" "${expected}" at)
    if(at LESS 0)
        message(FATAL_ERROR "${label}: the configure (exit ${result}) did not say \"${expected}\":\n${out}")
    endif()
endfunction()

set(pinned "pinned=[YES|${WORK}/source-pinned] after=[YES|${WORK}/source-after]")
set(all_pinned "RESULT local=[YES|${WORK}/source-local] ${pinned} registry=[]")
# without the option, and with it off, all come from their pinned source
check(unset "${all_pinned} option=[OFF]")
check(off "${all_pinned} option=[OFF]" -DCPM_USE_LOCAL_PACKAGES=OFF)
# with it, the one added with snx_cpm_local_package is the prefix's, said with the version found; the others are
# not looked for. The headers the prefix's package gives have no registry beside them: refused, with the path looked at
set(registry "${prefix}/share/vulkan/registry/vk.xml")
string(CONCAT refused "CPM_USE_LOCAL_PACKAGES=ON took the system's Vulkan headers in ${prefix}/include, whose "
                      "registry ${registry} is not a file.")
check(on-no-registry "${refused}" -DCPM_USE_LOCAL_PACKAGES=ON)
file(WRITE "${registry}" "<registry/>\n")
check(on "RESULT local=[|] ${pinned} registry=[${registry}] option=[OFF]" -DCPM_USE_LOCAL_PACKAGES=ON)
check(on-version "Using local package snxtestlocal@1.3.0" -DCPM_USE_LOCAL_PACKAGES=ON)
# a prefix whose package is older than the pin: the pinned source
file(WRITE "${prefix}/share/cmake/snxtestlocal/snxtestlocalConfigVersion.cmake"
     "set(PACKAGE_VERSION 1.1.0)\nset(PACKAGE_VERSION_COMPATIBLE FALSE)\n")
check(on-older "${all_pinned} option=[OFF]" -DCPM_USE_LOCAL_PACKAGES=ON)
file(REMOVE_RECURSE "${WORK}")
