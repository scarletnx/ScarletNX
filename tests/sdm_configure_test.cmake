# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Where the x86-64 back end can't be made the configure stops and says why, and only SNX_SAIL_X64=OFF goes without it
# (cmake/sdm.cmake). A small project takes sdm.cmake with the pinned manual's SHA-256 replaced by a file of the test's,
# beside an empty sailgen.cmake. Run by ctest as sdm_configure, with -DSRC and -DWORK
cmake_minimum_required(VERSION 3.28)
if(NOT DEFINED SRC OR NOT DEFINED WORK)
    message(FATAL_ERROR "sdm_configure_test.cmake needs -DSRC=... -DWORK=...")
endif()
file(REMOVE_RECURSE "${WORK}")
file(WRITE "${WORK}/manual.pdf" "the manual\n")
file(WRITE "${WORK}/other.pdf" "another revision\n")
file(SHA256 "${WORK}/manual.pdf" sha)
file(SHA256 "${WORK}/other.pdf" other)
file(READ "${SRC}/cmake/sdm.cmake" text)
string(REGEX MATCH "set\\(snx_sdm_sha256 [0-9a-f]+\\)" pin "${text}")
if(pin STREQUAL "")
    message(FATAL_ERROR "cmake/sdm.cmake has no set(snx_sdm_sha256 ...) to replace")
endif()
string(REPLACE "${pin}" "set(snx_sdm_sha256 ${sha})" text "${text}")
file(WRITE "${WORK}/src/cmake/sdm.cmake" "${text}")
file(WRITE "${WORK}/src/cmake/sailgen.cmake" "")
file(WRITE "${WORK}/src/CMakeLists.txt" "cmake_minimum_required(VERSION 3.28)\nproject(sdm LANGUAGES NONE)\n"
     "include(cmake/sdm.cmake)\nmessage(STATUS \"SNX_X64 is \${SNX_X64}\")\n")

# configure(<name> <stops|goes> <text the output has to have> <cmake args>...)
function(configure name want says)
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${WORK}/src" -B "${WORK}/${name}" ${ARGN}
                    RESULT_VARIABLE failed OUTPUT_VARIABLE out ERROR_VARIABLE out)
    string(REGEX REPLACE "[ \n]+" " " out "${out}")
    if(failed AND want STREQUAL "goes")
        message(FATAL_ERROR "${name}: the configure stopped: ${out}")
    elseif(NOT failed AND want STREQUAL "stops")
        message(FATAL_ERROR "${name}: the configure went through: ${out}")
    endif()
    string(FIND "${out}" "${says}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "${name}: the configure didn't say \"${says}\": ${out}")
    endif()
endfunction()
# a find_program is not run again over a value that is no -NOTFOUND: a program the project is told there is none of, or
# one it is told there is
set(with "-DSNX_PDFTOTEXT=${CMAKE_COMMAND}")
configure(no_pdftotext stops "pdftotext (poppler's) is missing: install it (docs/dev/build.md). -DSNX_SAIL_X64=OFF"
          -DSNX_SAIL_X64=ON "-DSNX_SDM_PDF=${WORK}/manual.pdf" -DSNX_PDFTOTEXT=OFF)
configure(other_revision stops "isn't revision 325383-093US of the Intel SDM (its SHA-256 is ${other}"
          -DSNX_SAIL_X64=ON "-DSNX_SDM_PDF=${WORK}/other.pdf" "${with}")
configure(off goes "SNX_X64 is OFF" -DSNX_SAIL_X64=OFF "-DSNX_SDM_PDF=${WORK}/other.pdf" -DSNX_PDFTOTEXT=OFF)
configure(made goes "SNX_X64 is ON" -DSNX_SAIL_X64=ON "-DSNX_SDM_PDF=${WORK}/manual.pdf" "${with}")
message("sdm_configure ok")
