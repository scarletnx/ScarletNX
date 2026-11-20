# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# tools/areatexgen against the SMAA distribution's Textures/AreaTex.h (iryoku/smaa 71c806a838, its CRLF line ends made
# LF), whose SHA-256 is below: the header it writes and the one the build wrote, byte for byte. Then a second run that
# leaves the file as it was, an output it cannot write and a run with no output named, each refused with its reason.
#
# usage: cmake -DAREATEXGEN=<areatexgen> -DBUILT=<the build's AreaTex.h> -DWORK=<folder> -P areatexgen.cmake
set(expected f2838043c41d10df08eb77f44ced37f0fe0b2af2bfba48b71e0806b8d89a4142)
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

execute_process(COMMAND "${AREATEXGEN}" "${WORK}/AreaTex.h" RESULT_VARIABLE result ERROR_VARIABLE errors)
if(result)
    message(FATAL_ERROR "areatexgen failed (${result}): ${errors}")
endif()
file(SHA256 "${WORK}/AreaTex.h" first)
if(NOT first STREQUAL expected)
    message(FATAL_ERROR "areatexgen wrote ${WORK}/AreaTex.h with the SHA-256 ${first}, not the distribution's "
                        "${expected}")
endif()
file(SHA256 "${BUILT}" built)
if(NOT built STREQUAL expected)
    message(FATAL_ERROR "the build's ${BUILT} has the SHA-256 ${built}, not the distribution's ${expected}")
endif()

file(TIMESTAMP "${WORK}/AreaTex.h" before "%Y-%m-%dT%H:%M:%S.%f")
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1.1)
execute_process(COMMAND "${AREATEXGEN}" "${WORK}/AreaTex.h" RESULT_VARIABLE result ERROR_VARIABLE errors)
file(TIMESTAMP "${WORK}/AreaTex.h" after "%Y-%m-%dT%H:%M:%S.%f")
file(SHA256 "${WORK}/AreaTex.h" second)
if(result OR NOT before STREQUAL after OR NOT second STREQUAL expected)
    message(FATAL_ERROR "a second run (${result}) rewrote the same header (${before}, ${after}) or another one "
                        "(${second}): ${errors}")
endif()

execute_process(COMMAND "${AREATEXGEN}" "${WORK}/missing/AreaTex.h" RESULT_VARIABLE result ERROR_VARIABLE errors)
string(FIND "${errors}" "areatexgen: cannot write ${WORK}/missing/AreaTex.h" at)
if(NOT result EQUAL 1 OR at EQUAL -1)
    message(FATAL_ERROR "areatexgen with a folder that does not exist ended ${result}, without naming it: ${errors}")
endif()
execute_process(COMMAND "${AREATEXGEN}" RESULT_VARIABLE result ERROR_VARIABLE errors)
if(NOT result EQUAL 2 OR NOT errors MATCHES "usage: areatexgen OUT\\.h")
    message(FATAL_ERROR "areatexgen with no output ended ${result}, without its usage: ${errors}")
endif()
message(STATUS "areatexgen passed")
