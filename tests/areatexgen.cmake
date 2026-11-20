# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# tools/areatexgen against the SMAA distribution's Textures/AreaTex.h and SearchTex.h (iryoku/smaa 71c806a838, their
# CRLF line ends made LF), whose SHA-256s are below: each header it writes and the one the build wrote, byte for byte,
# and a second run that leaves the file as it was. Then an output it cannot write, and a run with no output named, a
# --search with none and an option it does not know, each refused with its reason.
#
# usage: cmake -DAREATEXGEN=<areatexgen> -DBUILT_AREA=<the build's AreaTex.h> -DBUILT_SEARCH=<its SearchTex.h>
#              -DWORK=<folder> -P areatexgen.cmake
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

# name: the header's file name; built: the build's; expected: the distribution's SHA-256; the rest: areatexgen's
# options before the output
function(check_texture name built expected)
    set(out "${WORK}/${name}")
    execute_process(COMMAND "${AREATEXGEN}" ${ARGN} "${out}" RESULT_VARIABLE result ERROR_VARIABLE errors)
    if(result)
        message(FATAL_ERROR "areatexgen ${ARGN} failed (${result}): ${errors}")
    endif()
    file(SHA256 "${out}" first)
    if(NOT first STREQUAL expected)
        message(FATAL_ERROR "areatexgen ${ARGN} wrote ${out} with the SHA-256 ${first}, not the distribution's "
                            "${expected}")
    endif()
    file(SHA256 "${built}" got)
    if(NOT got STREQUAL expected)
        message(FATAL_ERROR "the build's ${built} has the SHA-256 ${got}, not the distribution's ${expected}")
    endif()
    file(TIMESTAMP "${out}" before "%Y-%m-%dT%H:%M:%S.%f")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1.1)
    execute_process(COMMAND "${AREATEXGEN}" ${ARGN} "${out}" RESULT_VARIABLE result ERROR_VARIABLE errors)
    file(TIMESTAMP "${out}" after "%Y-%m-%dT%H:%M:%S.%f")
    file(SHA256 "${out}" second)
    if(result OR NOT before STREQUAL after OR NOT second STREQUAL expected)
        message(FATAL_ERROR "a second run of areatexgen ${ARGN} (${result}) rewrote the same header (${before}, "
                            "${after}) or another one (${second}): ${errors}")
    endif()
endfunction()
check_texture(AreaTex.h "${BUILT_AREA}" f2838043c41d10df08eb77f44ced37f0fe0b2af2bfba48b71e0806b8d89a4142)
check_texture(SearchTex.h "${BUILT_SEARCH}" 34bbd4ff9f6fd39b583c2f5a450d815fa2f380a50d06008e411278e6f6a4d911 --search)

execute_process(COMMAND "${AREATEXGEN}" "${WORK}/missing/AreaTex.h" RESULT_VARIABLE result ERROR_VARIABLE errors)
string(FIND "${errors}" "areatexgen: cannot write ${WORK}/missing/AreaTex.h" at)
if(NOT result EQUAL 1 OR at EQUAL -1)
    message(FATAL_ERROR "areatexgen with a folder that does not exist ended ${result}, without naming it: ${errors}")
endif()
foreach(args "" "--search" "--bogus;${WORK}/x.h")
    execute_process(COMMAND "${AREATEXGEN}" ${args} RESULT_VARIABLE result ERROR_VARIABLE errors)
    if(NOT result EQUAL 2 OR NOT errors MATCHES "usage: areatexgen OUT\\.h .*, or areatexgen --search OUT\\.h")
        message(FATAL_ERROR "areatexgen ${args} ended ${result}, without its usage: ${errors}")
    endif()
endforeach()
if(EXISTS "${WORK}/x.h")
    message(FATAL_ERROR "areatexgen --bogus wrote ${WORK}/x.h")
endif()
message(STATUS "areatexgen passed")
