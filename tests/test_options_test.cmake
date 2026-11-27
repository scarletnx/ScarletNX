# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# A test option without the tests stops the configure and is named with its value, and the presets that build for this
# machine leave the tests out (cmake/test_options.cmake, CMakePresets.json). Run by ctest as test_options, with -DSRC
# and -DWORK
cmake_minimum_required(VERSION 3.28)
if(NOT DEFINED SRC OR NOT DEFINED WORK)
    message(FATAL_ERROR "test_options_test.cmake needs -DSRC=... -DWORK=...")
endif()
file(REMOVE_RECURSE "${WORK}")
file(WRITE "${WORK}/src/CMakeLists.txt"
     "cmake_minimum_required(VERSION 3.28)\nproject(options LANGUAGES NONE)\ninclude(\"${SRC}/cmake/test_options.cmake\")\n")

# configure(<name> <text the stop has to say, or empty where it goes through> <cmake args>...)
function(configure name says)
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${WORK}/src" -B "${WORK}/${name}" ${ARGN}
                    RESULT_VARIABLE failed OUTPUT_VARIABLE out ERROR_VARIABLE out)
    string(REGEX REPLACE "[ \n]+" " " out "${out}")
    if(says STREQUAL "")
        if(failed)
            message(FATAL_ERROR "${name}: the configure stopped: ${out}")
        endif()
    elseif(NOT failed)
        message(FATAL_ERROR "${name}: the configure went through and should have said \"${says}\"")
    else()
        string(FIND "${out}" "${says}" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "${name}: the configure stopped without saying \"${says}\": ${out}")
        endif()
    endif()
endfunction()
configure(option_on "SNX_TEST_VULKAN=ON needs the tests, which this configure leaves out (BUILD_TESTING=OFF)"
          -DBUILD_TESTING=OFF -DSNX_TEST_VULKAN:BOOL=ON)
configure(path_given "SNX_TEST_AMBERLITH=/some/probe.nro needs the tests"
          -DBUILD_TESTING=OFF -DSNX_TEST_AMBERLITH=/some/probe.nro)
configure(old_name_given "SNX_TEST_AMBERLITH=/some/probe.nro needs the tests"
          -DBUILD_TESTING=OFF -DSNX_TEST_EDENPROBE=/some/probe.nro)
configure(tests_on "" -DBUILD_TESTING=ON -DSNX_TEST_VULKAN:BOOL=ON -DSNX_TEST_AMBERLITH=/some/probe.nro)
configure(options_off "" -DBUILD_TESTING=OFF -DSNX_TEST_VULKAN:BOOL=OFF -DSNX_TEST_AMBERLITH=)
# the old name, SNX_TEST_EDENPROBE, read as SNX_TEST_AMBERLITH and said
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${WORK}/src" -B "${WORK}/old_name" -DBUILD_TESTING=ON
                        -DSNX_TEST_EDENPROBE=/some/probe.nro
                RESULT_VARIABLE failed OUTPUT_VARIABLE out ERROR_VARIABLE out)
string(REGEX REPLACE "[ \n]+" " " out "${out}")
string(FIND "${out}" "SNX_TEST_EDENPROBE is the old name of SNX_TEST_AMBERLITH: SNX_TEST_AMBERLITH=/some/probe.nro is taken"
       said)
file(STRINGS "${WORK}/old_name/CMakeCache.txt" kept REGEX "^SNX_TEST_AMBERLITH:")
if(failed OR said EQUAL -1 OR NOT kept STREQUAL "SNX_TEST_AMBERLITH:STRING=/some/probe.nro")
    message(FATAL_ERROR "old_name: the configure took SNX_TEST_EDENPROBE as \"${kept}\" (exit ${failed}): ${out}")
endif()
configure(program_found "" -DBUILD_TESTING=OFF -DSNX_TEST_PKG_CONFIG:FILEPATH=/usr/bin/pkg-config)

file(READ "${SRC}/CMakePresets.json" json)
string(JSON n LENGTH "${json}" configurePresets)
math(EXPR last "${n} - 1")
set(seen "")
foreach(i RANGE ${last})
    string(JSON name GET "${json}" configurePresets ${i} name)
    if(name STREQUAL "default" OR name STREQUAL "tracy")
        list(APPEND seen ${name})
        string(JSON inherits ERROR_VARIABLE none GET "${json}" configurePresets ${i} inherits)
        string(JSON tests ERROR_VARIABLE none GET "${json}" configurePresets ${i} cacheVariables BUILD_TESTING)
        if(name STREQUAL "default" AND NOT tests STREQUAL "OFF")
            message(FATAL_ERROR "the preset default has BUILD_TESTING \"${tests}\", not \"OFF\"")
        endif()
        if(name STREQUAL "tracy" AND (NOT inherits STREQUAL "default" OR NOT tests MATCHES "NOTFOUND$"))
            message(FATAL_ERROR "the preset tracy inherits \"${inherits}\" and has BUILD_TESTING \"${tests}\": it takes "
                                "default's")
        endif()
    endif()
endforeach()
if(NOT seen STREQUAL "default;tracy")
    message(FATAL_ERROR "CMakePresets.json has the presets \"${seen}\" of default and tracy")
endif()
message("test_options ok")
