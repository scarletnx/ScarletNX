# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Where cmake/packages.cmake takes a package from: CPMUtil's options, the CPM.cmake options they replace, and what it
# says. A small project with a cpmfile.json of its own adds three packages, whose archives the test serves from a
# folder: "local", pinned unless the system's is asked for (bundled, with a min_version, as LZ4's and the Vulkan
# headers' entries are), "first", the system's when there is one (SDL3's), and "pinned", added with
# FORCE_BUNDLED_PACKAGE ON (every other package). A prefix has the first two, "local" giving a Vulkan::Headers with and
# without its registry. Run by ctest as cpmutil_options, with -DSRC and -DWORK
# the policies of the CMake the build needs: a script has none of its own, and before CMake 4 an unset CMP0054 read
# if(kind STREQUAL "arc") as the variable arc
cmake_minimum_required(VERSION 3.28)
if(NOT DEFINED SRC OR NOT DEFINED WORK)
    message(FATAL_ERROR "cpmutil_options_test.cmake needs -DSRC=... -DWORK=...")
endif()
file(REMOVE_RECURSE "${WORK}")
set(prefix "${WORK}/prefix")
foreach(name local first)
    file(WRITE "${prefix}/share/cmake/snxtest${name}/snxtest${name}ConfigVersion.cmake"
         "set(PACKAGE_VERSION 1.3.0)\nset(PACKAGE_VERSION_COMPATIBLE TRUE)\n")
endforeach()
# found twice in a configure, as an exported package's guard allows: once to see whether it is there
file(WRITE "${prefix}/share/cmake/snxtestlocal/snxtestlocalConfig.cmake" "if(NOT TARGET Vulkan::Headers)\n"
     "    add_library(Vulkan::Headers INTERFACE IMPORTED)\n"
     "    set_target_properties(Vulkan::Headers PROPERTIES INTERFACE_INCLUDE_DIRECTORIES \"${prefix}/include\")\n"
     "endif()\n")
file(WRITE "${prefix}/share/cmake/snxtestfirst/snxtestfirstConfig.cmake" "")
file(MAKE_DIRECTORY "${prefix}/include")

# each package's archive, served from a folder, and the cpmfile that pins it; a file URL is file:///C:/... on Windows
set(url "file://${WORK}/served")
if(NOT WORK MATCHES "^/")
    set(url "file:///${WORK}/served")
endif()
set(entries "")
foreach(name local first pinned)
    file(WRITE "${WORK}/payload/snxtest${name}-1.2.0/file.txt" "the pinned source\n")
    file(MAKE_DIRECTORY "${WORK}/served")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar czf "${WORK}/served/${name}.tar.gz" "snxtest${name}-1.2.0"
                    WORKING_DIRECTORY "${WORK}/payload" RESULT_VARIABLE failed)
    if(failed)
        message(FATAL_ERROR "cpmutil_options: could not make ${name}'s archive")
    endif()
    file(SHA512 "${WORK}/served/${name}.tar.gz" hash)
    set(fields "\"url\": \"${url}/${name}.tar.gz\", \"version\": \"1.2.0\", \"hash\": \"${hash}\"")
    if(name STREQUAL "local")
        string(APPEND fields ", \"min_version\": \"1.2.0\", \"bundled\": true")
    elseif(name STREQUAL "first")
        string(APPEND fields ", \"min_version\": \"1.2.0\"")
    endif()
    list(APPEND entries "\"snxtest${name}\": {${fields}}")
endforeach()
list(JOIN entries ", " entries)
file(WRITE "${WORK}/project/cpmfile.json" "{${entries}}\n")
file(WRITE "${WORK}/project/CMakeLists.txt" "cmake_minimum_required(VERSION 3.28)
project(cpmutil_options_test LANGUAGES NONE)
include(\"${SRC}/cmake/packages.cmake\")
snx_cpm_options(snxtestlocal)
AddJsonPackage(NAME snxtestlocal DOWNLOAD_ONLY)
snx_cpm_options(snxtestfirst)
AddJsonPackage(NAME snxtestfirst DOWNLOAD_ONLY)
AddJsonPackage(NAME snxtestpinned DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
set(registry \"\")
if(TARGET Vulkan::Headers)
    snx_system_vulkan_registry(registry)
endif()
message(STATUS \"RESULT local=[\${snxtestlocal_ADDED}|\${snxtestlocal_SOURCE_DIR}] \"
               \"first=[\${snxtestfirst_ADDED}|\${snxtestfirst_SOURCE_DIR}] \"
               \"pinned=[\${snxtestpinned_ADDED}|\${snxtestpinned_SOURCE_DIR}] registry=[\${registry}]\")
")

# configures the project in the build tree <label> with ARGS (and ENV), and checks it said each of SAYS and none of NOT,
# and that it failed when FAIL is given, else passed
function(check label)
    cmake_parse_arguments(arg "FAIL" "" "SAYS;NOT;ARGS;ENV" ${ARGN})
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=CPM_USE_LOCAL_PACKAGES --unset=CPM_SOURCE_CACHE
                            ${arg_ENV} "${CMAKE_COMMAND}" -S "${WORK}/project" -B "${WORK}/${label}"
                            "-DCMAKE_PREFIX_PATH=${prefix}" "-DCPM_SOURCE_CACHE=${WORK}/cache" ${arg_ARGS}
                    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
    string(REPLACE "${WORK}" "WORK" out "${out}")
    string(REGEX REPLACE "\n *" " " flat "${out}") # CMake wraps an error's text
    if((arg_FAIL AND result EQUAL 0) OR (NOT arg_FAIL AND NOT result EQUAL 0))
        message(SEND_ERROR "${label}: the configure exited ${result}:\n${out}")
        set_property(GLOBAL APPEND PROPERTY failed "${label}")
        return()
    endif()
    foreach(want IN LISTS arg_SAYS)
        string(FIND "${flat}" "${want}" at)
        if(at LESS 0)
            message(SEND_ERROR "${label}: the configure did not say \"${want}\":\n${out}")
            set_property(GLOBAL APPEND PROPERTY failed "${label}")
        endif()
    endforeach()
    foreach(unwanted IN LISTS arg_NOT)
        string(FIND "${flat}" "${unwanted}" at)
        if(at GREATER_EQUAL 0)
            message(SEND_ERROR "${label}: the configure said \"${unwanted}\":\n${out}")
            set_property(GLOBAL APPEND PROPERTY failed "${label}")
        endif()
    endforeach()
endfunction()

# The texts name WORK for the test's folder; a pinned package is in the cache, at its name and version
set(local_pinned "local=[YES|WORK/cache/snxtestlocal/1.2.0]")
set(local_system "local=[NO|]")
set(first_pinned "first=[YES|WORK/cache/snxtestfirst/1.2.0]")
set(first_system "first=[NO|]")
set(pinned "pinned=[YES|WORK/cache/snxtestpinned/1.2.0]")
set(old "is CPM.cmake's option, which CPMUtil's replace")

# nothing asked: "local" pinned, "first" the system's, "pinned" pinned
check(default SAYS "RESULT ${local_pinned} ${first_system} ${pinned} registry=[]" NOT "${old}")
# the system's headers, without the registry beside them: refused, with the option that took them and the path
set(registry "${prefix}/share/vulkan/registry/vk.xml")
string(REPLACE "${WORK}" "WORK" shown_registry "${registry}")
string(CONCAT no_registry "CPMUTIL_FORCE_SYSTEM=ON took the system's Vulkan headers in WORK/prefix/include, whose "
                          "registry ${shown_registry} is not a file.")
check(all-system-no-registry FAIL SAYS "${no_registry}" ARGS -DCPMUTIL_FORCE_SYSTEM=ON)
file(WRITE "${registry}" "<registry/>\n")
# CPMUtil's options: the system's for one package and for every one it may give, the latter said with the packages
# it leaves pinned; the pinned ones for one package and for every one
check(local-system SAYS "RESULT ${local_system} ${first_system} ${pinned} registry=[${shown_registry}]"
      ARGS -Dsnxtestlocal_FORCE_SYSTEM=ON)
check(all-system ARGS -DCPMUTIL_FORCE_SYSTEM=ON
      SAYS "RESULT ${local_system} ${first_system} ${pinned} registry=[${shown_registry}]"
           "CPMUTIL_FORCE_SYSTEM=ON takes from the system the packages it may give, and leaves these pinned"
           "): snxtestpinned@1.2.0")
check(first-bundled SAYS "RESULT ${local_pinned} ${first_pinned} ${pinned}" ARGS -Dsnxtestfirst_FORCE_BUNDLED=ON)
check(all-bundled SAYS "RESULT ${local_pinned} ${first_pinned} ${pinned}" ARGS -DCPMUTIL_FORCE_BUNDLED=ON)
# the system's asked for a package added with FORCE_BUNDLED_PACKAGE ON: refused, with the version it is
check(pinned-system FAIL ARGS -Dsnxtestpinned_FORCE_SYSTEM=ON
      SAYS "snxtestpinned_FORCE_SYSTEM=ON, and snxtestpinned is the pinned 1.2.0: the build compiles it")

# CPM.cmake's CPM_USE_LOCAL_PACKAGES, from a configure before CPMUtil: the system's "local" where find_package finds it,
# said with the options that replace it, once per build tree; from the environment too
string(CONCAT local_note "CPM_USE_LOCAL_PACKAGES=ON ${old}: snxtestlocal is the system's, which find_package finds at "
                         "1.2.0 or newer (snxtestlocal_FORCE_SYSTEM=ON). Configure with -Dsnxtestlocal_FORCE_SYSTEM=ON")
check(old-local SAYS "RESULT ${local_system} ${first_system} ${pinned}" "${local_note}"
      ARGS -DCPM_USE_LOCAL_PACKAGES=ON)
check(old-local SAYS "RESULT ${local_system} ${first_system} ${pinned}" NOT "${old}")
string(REPLACE "CPM_USE_LOCAL_PACKAGES=ON" "CPM_USE_LOCAL_PACKAGES=ON in the environment" env_note "${local_note}")
check(old-local-env SAYS "RESULT ${local_system}" "${env_note}" ENV CPM_USE_LOCAL_PACKAGES=ON)
# CPMUtil's own option, given too, wins, and the old one is said to give way to it
check(old-local-bundled SAYS "RESULT ${local_pinned}"
      "CPM_USE_LOCAL_PACKAGES=ON ${old}, and snxtestlocal_FORCE_BUNDLED=ON says where snxtestlocal comes from instead."
      ARGS -DCPM_USE_LOCAL_PACKAGES=ON -Dsnxtestlocal_FORCE_BUNDLED=ON)
# CPM_DOWNLOAD_<package>: that package's pinned source
check(old-download SAYS "RESULT ${local_pinned} ${first_pinned} ${pinned}"
      "CPM_DOWNLOAD_snxtestfirst=ON ${old}: read as snxtestfirst_FORCE_BUNDLED=ON, the pinned 1.2.0."
      ARGS -DCPM_DOWNLOAD_snxtestfirst=ON)

# a prefix whose packages are older than the pins: CPM_USE_LOCAL_PACKAGES keeps "local" pinned, saying why, "first" is
# pinned, and the system's "local" asked for stops the configure
foreach(name local first)
    file(WRITE "${prefix}/share/cmake/snxtest${name}/snxtest${name}ConfigVersion.cmake"
         "set(PACKAGE_VERSION 1.1.0)\nset(PACKAGE_VERSION_COMPATIBLE FALSE)\n")
endforeach()
check(old-local-older SAYS "RESULT ${local_pinned} ${first_pinned} ${pinned}"
      "snxtestlocal is the pinned 1.2.0: find_package finds no snxtestlocal 1.2.0 or newer."
      ARGS -DCPM_USE_LOCAL_PACKAGES=ON)
check(local-system-older FAIL SAYS "[CPMUtil] snxtestlocal not found via find_package(snxtestlocal 1.2.0)"
      ARGS -Dsnxtestlocal_FORCE_SYSTEM=ON)

get_property(failed GLOBAL PROPERTY failed)
if(failed)
    message(FATAL_ERROR "cpmutil_options: ${failed} failed (the configures are in ${WORK})")
endif()
file(REMOVE_RECURSE "${WORK}")
