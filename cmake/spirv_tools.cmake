# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# SPIRV-Tools' spirv-opt, spirv-val and spirv-dis, as the executables snx-spirv-opt, snx-spirv-val and snx-spirv-dis:
# the shaders' optimizer and validator (cmake/glsl.cmake) and the tests' checks of the SPIR-V the build writes. They are
# KhronosGroup's release that cpmfile.json pins, built from its source with the SPIRV-Headers commit its DEPS names
# (spirv_headers_revision), for the machine doing the build (a cross build's too: nothing links them), as Release
# whatever the build's type, and kept in SNX_DEPS_CACHE when it is set (cmake/deps_cache.cmake): every machine optimizes
# and validates with the same release, whatever its distribution packages.
#
# SNX_SPIRV_OPT, SNX_SPIRV_VAL and SNX_SPIRV_DIS name other tools; one not named is the one beside the tool before it
# (spirv-val beside spirv-opt, spirv-dis beside spirv-val) when there is one. SPIRV-Tools_FORCE_SYSTEM and
# CPMUTIL_FORCE_SYSTEM (CPMUtil's options, cmake/packages.cmake) take the system's tools on PATH instead, which
# spirv-opt --version must say are the pinned release or a newer one, as CPM_USE_LOCAL_PACKAGES did where they are,
# the pinned ones elsewhere; SPIRV-Tools_FORCE_BUNDLED and CPMUTIL_FORCE_BUNDLED keep the pinned ones.
include_guard(GLOBAL)
if(NOT COMMAND snx_cpmfile)
    message(FATAL_ERROR "cmake/spirv_tools.cmake needs CPMUtil and the pins, cmake/packages.cmake: include that first")
endif()
snx_cpmfile(snx_spirv_tools_version spirv-tools version)
string(REGEX REPLACE "^v" "" snx_spirv_tools_version "${snx_spirv_tools_version}")

# A configure from before the pin wrote the tools it found on PATH into these keys: glsl.cmake its spirv-opt and
# spirv-val, with its own search's result beside each, and the tests their find_program's, with that command's
# help text. Such a value was found, not given, and the pinned release replaces it
foreach(snx_spirv_tool opt val dis)
    string(TOUPPER "${snx_spirv_tool}" snx_spirv_key)
    get_property(snx_spirv_help CACHE SNX_SPIRV_${snx_spirv_key} PROPERTY HELPSTRING)
    # if() takes AND and OR as they come, left to right: the parentheses group them
    if(DEFINED CACHE{SNX_SPIRV_${snx_spirv_key}}
       AND (snx_spirv_help STREQUAL "Path to a program."
            OR (DEFINED CACHE{snx_spirv_${snx_spirv_tool}_path}
                AND "$CACHE{SNX_SPIRV_${snx_spirv_key}}" STREQUAL "$CACHE{snx_spirv_${snx_spirv_tool}_path}")))
        message(STATUS "SNX_SPIRV_${snx_spirv_key}=$CACHE{SNX_SPIRV_${snx_spirv_key}} was found on PATH by a "
                       "configure from before SPIRV-Tools was pinned, not given: the build uses the pinned "
                       "v${snx_spirv_tools_version} now, and -DSNX_SPIRV_${snx_spirv_key}=... names another")
        unset(SNX_SPIRV_${snx_spirv_key} CACHE)
    endif()
    unset(snx_spirv_${snx_spirv_tool}_path CACHE)
endforeach()

set(SNX_SPIRV_OPT "" CACHE FILEPATH "spirv-opt to optimize shaders with (empty: the pinned SPIRV-Tools)")
set(SNX_SPIRV_VAL "" CACHE FILEPATH
    "spirv-val to validate shaders with (empty: the one beside SNX_SPIRV_OPT, else the pinned SPIRV-Tools)")
set(SNX_SPIRV_DIS "" CACHE FILEPATH
    "spirv-dis the tests disassemble with (empty: the one beside spirv-val, else the pinned SPIRV-Tools)")
foreach(snx_spirv_tool opt val dis)
    string(TOUPPER "${snx_spirv_tool}" snx_spirv_key)
    if(SNX_SPIRV_${snx_spirv_key} AND NOT EXISTS "${SNX_SPIRV_${snx_spirv_key}}")
        message(FATAL_ERROR "SNX_SPIRV_${snx_spirv_key}=${SNX_SPIRV_${snx_spirv_key}} is not a file. Name a "
                            "spirv-${snx_spirv_tool}, or leave it empty for the pinned SPIRV-Tools "
                            "v${snx_spirv_tools_version}.")
    endif()
endforeach()

# Each tool's path, and where it came from for the configure's line. The tools run on the machine doing the build
set(snx_spirv_exe "${CMAKE_HOST_EXECUTABLE_SUFFIX}")
foreach(snx_spirv_tool opt val dis)
    string(TOUPPER "${snx_spirv_tool}" snx_spirv_key)
    set(snx_spirv_${snx_spirv_tool} "${SNX_SPIRV_${snx_spirv_key}}")
    set(snx_spirv_${snx_spirv_tool}_from "SNX_SPIRV_${snx_spirv_key}")
endforeach()
foreach(snx_spirv_pair opt:val val:dis)
    string(REPLACE ":" ";" snx_spirv_pair "${snx_spirv_pair}")
    list(GET snx_spirv_pair 0 snx_spirv_before)
    list(GET snx_spirv_pair 1 snx_spirv_tool)
    if(snx_spirv_${snx_spirv_before} AND NOT snx_spirv_${snx_spirv_tool})
        get_filename_component(snx_spirv_dir "${snx_spirv_${snx_spirv_before}}" DIRECTORY)
        if(EXISTS "${snx_spirv_dir}/spirv-${snx_spirv_tool}${snx_spirv_exe}")
            set(snx_spirv_${snx_spirv_tool} "${snx_spirv_dir}/spirv-${snx_spirv_tool}${snx_spirv_exe}")
            set(snx_spirv_${snx_spirv_tool}_from "beside spirv-${snx_spirv_before}")
        endif()
    endif()
endforeach()

# Whether the system's tools are asked for, in CPMUtil's order: SPIRV-Tools' own option first, then the global ones,
# then CPM_USE_LOCAL_PACKAGES, which a configure from before CPMUtil gave and which takes the system's where they fit
option(SPIRV-Tools_FORCE_SYSTEM "Take spirv-opt, spirv-val and spirv-dis from PATH, at the pinned SPIRV-Tools or newer"
       OFF)
option(SPIRV-Tools_FORCE_BUNDLED "Build the pinned SPIRV-Tools' spirv-opt, spirv-val and spirv-dis" OFF)
set(snx_spirv_system "")
if(SPIRV-Tools_FORCE_SYSTEM)
    set(snx_spirv_system SPIRV-Tools_FORCE_SYSTEM)
elseif(SPIRV-Tools_FORCE_BUNDLED)
elseif(CPMUTIL_FORCE_SYSTEM)
    set(snx_spirv_system CPMUTIL_FORCE_SYSTEM)
elseif(CPMUTIL_FORCE_BUNDLED)
elseif(snx_cpm_use_local_packages) # as cmake/packages.cmake read it
    set(snx_spirv_system CPM_USE_LOCAL_PACKAGES)
    snx_cpm_note(SPIRV-Tools "${snx_cpm_use_local_packages_from} is CPM.cmake's option, which CPMUtil's replace: "
                             "read for SPIRV-Tools as SPIRV-Tools_FORCE_SYSTEM=ON where spirv-opt, spirv-val and "
                             "spirv-dis on PATH are its pinned release or newer, the pinned ones otherwise. Configure "
                             "with -DSPIRV-Tools_FORCE_SYSTEM=ON instead.")
endif()

# The system's tools, all three from one place, when they are asked for and SNX_SPIRV_OPT and SNX_SPIRV_VAL leave one to
# find. Their version is spirv-opt's --version ("SPIRV-Tools v2026.3 ..."): no distribution's SPIRV-Tools package
# installs a CMake version file find_package could compare
if(snx_spirv_system AND NOT (snx_spirv_opt AND snx_spirv_val))
    find_program(snx_spirv_local NAMES spirv-opt NO_CACHE NO_CMAKE_FIND_ROOT_PATH)
    set(snx_spirv_local_why "")
    if(NOT snx_spirv_local)
        set(snx_spirv_local_why "no spirv-opt on PATH")
    else()
        execute_process(COMMAND "${snx_spirv_local}" --version OUTPUT_VARIABLE snx_spirv_local_out
                        ERROR_VARIABLE snx_spirv_local_out RESULT_VARIABLE snx_spirv_local_rc)
        string(STRIP "${snx_spirv_local_out}" snx_spirv_local_out)
        get_filename_component(snx_spirv_dir "${snx_spirv_local}" DIRECTORY)
        if(NOT snx_spirv_local_rc EQUAL 0 OR NOT snx_spirv_local_out MATCHES "SPIRV-Tools v([0-9]+\\.[0-9]+)")
            string(CONCAT snx_spirv_local_why "${snx_spirv_local} --version printed \"${snx_spirv_local_out}\" "
                                              "(status ${snx_spirv_local_rc}), which names no SPIRV-Tools version")
        elseif(CMAKE_MATCH_1 VERSION_LESS snx_spirv_tools_version)
            string(CONCAT snx_spirv_local_why "${snx_spirv_local} is SPIRV-Tools v${CMAKE_MATCH_1}, older than the "
                                              "pinned v${snx_spirv_tools_version}")
        else()
            set(snx_spirv_local_version "${CMAKE_MATCH_1}")
            foreach(snx_spirv_tool val dis)
                if(NOT EXISTS "${snx_spirv_dir}/spirv-${snx_spirv_tool}${snx_spirv_exe}")
                    string(CONCAT snx_spirv_local_why "${snx_spirv_local} (v${snx_spirv_local_version}) has no "
                                                      "spirv-${snx_spirv_tool} beside it")
                endif()
            endforeach()
        endif()
    endif()
    if(snx_spirv_local_why AND snx_spirv_system STREQUAL "CPM_USE_LOCAL_PACKAGES")
        message(STATUS "CPM_USE_LOCAL_PACKAGES=ON: ${snx_spirv_local_why}, so the build builds the pinned SPIRV-Tools "
                       "v${snx_spirv_tools_version}")
    elseif(snx_spirv_local_why)
        message(FATAL_ERROR "${snx_spirv_system}=ON: ${snx_spirv_local_why}. Put SPIRV-Tools "
                            "v${snx_spirv_tools_version} or newer on PATH, name the tools with SNX_SPIRV_OPT and "
                            "SNX_SPIRV_VAL, or configure with -D${snx_spirv_system}=OFF for the pinned release.")
    else()
        foreach(snx_spirv_tool opt val dis)
            if(NOT snx_spirv_${snx_spirv_tool})
                set(snx_spirv_${snx_spirv_tool} "${snx_spirv_dir}/spirv-${snx_spirv_tool}${snx_spirv_exe}")
                set(snx_spirv_${snx_spirv_tool}_from "the system's v${snx_spirv_local_version}, ${snx_spirv_system}")
            endif()
        endforeach()
    endif()
endif()

# The pinned release, for spirv-opt or spirv-val when neither a name nor the system gave it, and for spirv-dis with them
if(NOT snx_spirv_opt OR NOT snx_spirv_val)
    AddJsonPackage(NAME spirv-tools DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
    AddJsonPackage(NAME spirv-headers DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
    snx_cpmfile(snx_spirv_tools_hash spirv-tools hash)
    snx_cpmfile(snx_spirv_headers_hash spirv-headers hash)

    # The machine doing the build's compilers: a cross build's are the target's (cmake/host_cc.cmake)
    if(CMAKE_CROSSCOMPILING)
        include("${CMAKE_CURRENT_LIST_DIR}/host_cc.cmake")
        if(CMAKE_HOST_APPLE)
            find_program(SNX_HOST_CXX NAMES clang++ PATHS /usr/bin NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
        endif()
        find_program(SNX_HOST_CXX NAMES clang++ c++ NO_CMAKE_FIND_ROOT_PATH)
        if(NOT SNX_HOST_CXX)
            message(FATAL_ERROR "A cross build builds SPIRV-Tools v${snx_spirv_tools_version} for this machine, which "
                                "needs a C++ compiler for it (clang++ or c++). Install one, set SNX_HOST_CXX to its "
                                "path, or name the tools with SNX_SPIRV_OPT and SNX_SPIRV_VAL.")
        endif()
        set(snx_spirv_cc "${SNX_HOST_CC}")
        set(snx_spirv_cxx "${SNX_HOST_CXX}")
    else()
        set(snx_spirv_cc "${CMAKE_C_COMPILER}")
        set(snx_spirv_cxx "${CMAKE_CXX_COMPILER}")
    endif()
    # SPIRV-Tools' configure generates its grammar tables with Python
    if(NOT Python3_EXECUTABLE)
        find_package(Python3 COMPONENTS Interpreter)
    endif()
    if(NOT Python3_EXECUTABLE)
        message(FATAL_ERROR "Building SPIRV-Tools v${snx_spirv_tools_version} (spirv-opt, spirv-val) needs Python 3, "
                            "which was not found: install it, or name the tools with SNX_SPIRV_OPT and SNX_SPIRV_VAL.")
    endif()

    # no -Werror: a newer compiler's new warnings would stop the build. The tests' and spirv-fuzz's own dependencies
    # are not in the release archive
    set(snx_spirv_args
        -DCMAKE_BUILD_TYPE=Release
        "-DCMAKE_C_COMPILER=${snx_spirv_cc}"
        "-DCMAKE_CXX_COMPILER=${snx_spirv_cxx}"
        "-DPython3_EXECUTABLE=${Python3_EXECUTABLE}"
        "-DSPIRV-Headers_SOURCE_DIR=${SPIRV-Headers_SOURCE_DIR}"
        -DSPIRV_WERROR=OFF
        -DSPIRV_SKIP_TESTS=ON
        -DSPIRV_BUILD_FUZZER=OFF
        -DSPIRV_TOOLS_BUILD_STATIC=ON
        -DBUILD_SHARED_LIBS=OFF
        -DSKIP_SPIRV_TOOLS_INSTALL=ON)
    # Keyed by the build machine's compiler, not the target's: every target built here runs the same tools. Python by
    # its version, not its path: the trees of one CI job run it from a venv or from the system
    execute_process(COMMAND "${Python3_EXECUTABLE}" --version OUTPUT_VARIABLE snx_spirv_python
                    ERROR_VARIABLE snx_spirv_python)
    string(STRIP "${snx_spirv_python}" snx_spirv_python)
    set(snx_spirv_key_args ${snx_spirv_args})
    list(FILTER snx_spirv_key_args EXCLUDE REGEX "^-DPython3_EXECUTABLE=")
    snx_deps_cached(spirv-tools snx_spirv_tools_cache HOST "${snx_spirv_cc}" "source: ${snx_spirv_tools_hash}"
                    "headers: ${snx_spirv_headers_hash}" "configure: ${snx_spirv_key_args}"
                    "python: ${snx_spirv_python}")
    if(snx_spirv_tools_cache_FOUND)
        set(snx_spirv_prefix "${snx_spirv_tools_cache_DIR}")
    else()
        set(snx_spirv_prefix "${PROJECT_BINARY_DIR}/spirv-tools")
        set(snx_spirv_built "")
        foreach(snx_spirv_tool opt val dis)
            list(APPEND snx_spirv_built "${snx_spirv_prefix}/bin/spirv-${snx_spirv_tool}${snx_spirv_exe}")
        endforeach()
        # the compiler cache, as the build's compiles have it (cmake/compiler_cache.cmake); a launcher with arguments
        # is a list, whose ; ExternalProject would split
        set(snx_spirv_launcher "")
        foreach(snx_spirv_lang C CXX)
            if(CMAKE_${snx_spirv_lang}_COMPILER_LAUNCHER)
                string(REPLACE ";" "|" snx_spirv_word "${CMAKE_${snx_spirv_lang}_COMPILER_LAUNCHER}")
                list(APPEND snx_spirv_launcher "-DCMAKE_${snx_spirv_lang}_COMPILER_LAUNCHER=${snx_spirv_word}")
            endif()
        endforeach()
        include(ExternalProject)
        snx_deps_binary_dir("${PROJECT_BINARY_DIR}/spirv-tools-build" "${SPIRV-Tools_SOURCE_DIR}")
        # Only the three tools are built, straight into the prefix: SPIRV-Tools' install would want every target
        ExternalProject_Add(snx_spirv_tools_build
            SOURCE_DIR "${SPIRV-Tools_SOURCE_DIR}"
            BINARY_DIR "${PROJECT_BINARY_DIR}/spirv-tools-build"
            LIST_SEPARATOR |
            CMAKE_ARGS ${snx_spirv_args} ${snx_spirv_launcher}
                       "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${snx_spirv_prefix}/bin"
                       "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=${snx_spirv_prefix}/bin"
            BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release
                          --target spirv-opt spirv-val spirv-dis
            INSTALL_COMMAND ""
            BUILD_BYPRODUCTS ${snx_spirv_built}
            LOG_CONFIGURE ON
            LOG_BUILD ON
            LOG_OUTPUT_ON_FAILURE ON)
        snx_deps_publish(snx_spirv_tools_build spirv-tools "${snx_spirv_prefix}" "${snx_spirv_tools_cache_DIR}")
    endif()
    foreach(snx_spirv_tool opt val dis)
        if(NOT snx_spirv_${snx_spirv_tool})
            set(snx_spirv_${snx_spirv_tool} "${snx_spirv_prefix}/bin/spirv-${snx_spirv_tool}${snx_spirv_exe}")
            set(snx_spirv_${snx_spirv_tool}_from "pinned v${snx_spirv_tools_version}")
        endif()
    endforeach()
endif()

set(snx_spirv_line "")
foreach(snx_spirv_tool opt val dis)
    if(NOT snx_spirv_${snx_spirv_tool})
        string(APPEND snx_spirv_line " spirv-${snx_spirv_tool}: none beside ${snx_spirv_val}, so the tests that need "
                                     "it are left out.")
        continue()
    endif()
    add_executable(snx-spirv-${snx_spirv_tool} IMPORTED GLOBAL)
    set_target_properties(snx-spirv-${snx_spirv_tool} PROPERTIES IMPORTED_LOCATION "${snx_spirv_${snx_spirv_tool}}")
    if(TARGET snx_spirv_tools_build AND snx_spirv_${snx_spirv_tool}_from MATCHES "^pinned")
        add_dependencies(snx-spirv-${snx_spirv_tool} snx_spirv_tools_build)
    endif()
    string(APPEND snx_spirv_line " spirv-${snx_spirv_tool}: ${snx_spirv_${snx_spirv_tool}} "
                                 "(${snx_spirv_${snx_spirv_tool}_from}).")
endforeach()
message(STATUS "SPIRV-Tools:${snx_spirv_line}")
