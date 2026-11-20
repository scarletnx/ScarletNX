# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# A cross build builds the generators the build runs (hookgen, decodetree, swipc) for the machine doing it, as imported
# targets, and a test that runs one runs it there: not through the target's emulator, which a target takes from
# CMAKE_CROSSCOMPILING_EMULATOR unless it says otherwise. A small project that says it is a cross build with an
# emulator includes the three modules and adds a test of each tool: no test's command names the emulator. Run by
# ctest as host_tools_emulator, with -DSRC, -DWORK, -DGENERATOR and -DMAKE (the build's)
file(REMOVE_RECURSE "${WORK}")
file(WRITE "${WORK}/project/CMakeLists.txt" "cmake_minimum_required(VERSION 3.28)
project(host_tools_emulator_test LANGUAGES NONE)
enable_testing()
set(PROJECT_SOURCE_DIR \"${SRC}\") # where the modules find the tools' sources
set(CMAKE_CROSSCOMPILING TRUE)
set(CMAKE_CROSSCOMPILING_EMULATOR the-emulator-of-the-target)
foreach(tool hookgen decodetree swipc)
    include(\"${SRC}/cmake/\${tool}.cmake\")
    add_test(NAME runs_\${tool} COMMAND snx-\${tool})
endforeach()
")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${WORK}/project" -B "${WORK}/build" -G "${GENERATOR}"
                        "-DCMAKE_MAKE_PROGRAM=${MAKE}"
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the project did not configure (exit ${result}):\n${out}")
endif()
file(STRINGS "${WORK}/build/CTestTestfile.cmake" tests REGEX "^add_test")
list(LENGTH tests count)
if(NOT count EQUAL 3)
    message(FATAL_ERROR "the project has ${count} tests, not the 3 it adds:\n${tests}")
endif()
foreach(test IN LISTS tests)
    if(test MATCHES "the-emulator-of-the-target")
        message(FATAL_ERROR "a host tool's test runs through the target's emulator: ${test}")
    endif()
endforeach()
file(REMOVE_RECURSE "${WORK}")
