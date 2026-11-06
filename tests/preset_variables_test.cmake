# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
#   cmake -DSRC=<source dir> -P tests/preset_variables_test.cmake
#
# Every SNX_ cache variable a configure preset of CMakePresets.json sets is one the build reads: its name stands
# outside a comment in a CMakeLists.txt or a cmake/ file. A variable no code reads changes nothing in the build it
# names, and CMake only says so in a warning at the end of that preset's configure.
if(NOT DEFINED SRC)
    message(FATAL_ERROR "preset_variables_test.cmake needs -DSRC=...")
endif()
file(READ "${SRC}/CMakePresets.json" json)
# the project's own: a build folder or a package cache inside the checkout holds other projects' CMakeLists.txt
file(GLOB_RECURSE lists "${SRC}/cmake/*.cmake" "${SRC}/src/CMakeLists.txt" "${SRC}/tools/CMakeLists.txt"
     "${SRC}/tests/CMakeLists.txt" "${SRC}/scripts/CMakeLists.txt")
list(APPEND lists "${SRC}/CMakeLists.txt")
set(code "")
foreach(path IN LISTS lists)
    file(STRINGS "${path}" lines)
    foreach(line IN LISTS lines)
        string(REGEX REPLACE "#.*" "" line "${line}") # a comment names what it describes, which needn't exist
        string(APPEND code " ${line}")
    endforeach()
endforeach()

set(failed 0)
set(checked 0)
string(JSON presets LENGTH "${json}" configurePresets)
math(EXPR last "${presets} - 1")
foreach(i RANGE ${last})
    string(JSON name GET "${json}" configurePresets ${i} name)
    string(JSON variables ERROR_VARIABLE none GET "${json}" configurePresets ${i} cacheVariables)
    if(none)
        continue()
    endif()
    string(JSON count LENGTH "${variables}")
    if(count EQUAL 0)
        continue()
    endif()
    math(EXPR count "${count} - 1")
    foreach(j RANGE ${count})
        string(JSON variable MEMBER "${variables}" ${j})
        if(NOT variable MATCHES "^SNX_")
            continue()
        endif()
        math(EXPR checked "${checked} + 1")
        if(NOT code MATCHES "[^A-Za-z0-9_]${variable}[^A-Za-z0-9_]")
            message(SEND_ERROR "preset ${name} sets ${variable}, which no CMakeLists.txt or cmake/ file reads")
            set(failed 1)
        endif()
    endforeach()
endforeach()
if(failed)
    message(FATAL_ERROR "preset_variables: see the errors above")
endif()
message(STATUS "preset_variables: the ${checked} SNX_ variables the presets set are each read by the build")
