# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
#   cmake -DSRC=<source dir> -DOPENSSL=<OpenSSL's source> -P tests/openssl_targets_test.cmake
#
# Every Configure target cmake/openssl.cmake names is one the pinned OpenSSL defines in its Configurations/*.conf: a
# name it does not know stops Configure ("This system (...) is not supported") on that platform alone, where no other
# job builds it.
foreach(var IN ITEMS SRC OPENSSL)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "openssl_targets_test.cmake needs -D${var}=...")
    endif()
endforeach()

file(STRINGS "${SRC}/cmake/openssl.cmake" sets REGEX "set\\(snx_openssl_target [^ \"]+\\)")
set(named "")
foreach(line IN LISTS sets)
    string(REGEX MATCH "set\\(snx_openssl_target ([^ \"]+)\\)" _ "${line}")
    list(APPEND named "${CMAKE_MATCH_1}")
endforeach()
list(LENGTH named count)
# the targets the file named when this test was written: fewer means it no longer reads the file as it is written
if(count LESS 16)
    message(FATAL_ERROR "openssl_targets: ${count} targets read from cmake/openssl.cmake, 16 expected (${named})")
endif()

file(GLOB confs "${OPENSSL}/Configurations/*.conf")
if(NOT confs)
    message(FATAL_ERROR "openssl_targets: no Configurations/*.conf in ${OPENSSL}")
endif()
set(defined "")
foreach(conf IN LISTS confs)
    file(STRINGS "${conf}" heads REGEX "^ *\"[^\"]+\" *=> *{")
    foreach(head IN LISTS heads)
        string(REGEX MATCH "\"([^\"]+)\"" _ "${head}")
        list(APPEND defined "${CMAKE_MATCH_1}")
    endforeach()
endforeach()

set(failed 0)
foreach(target IN LISTS named)
    if(target IN_LIST defined)
        message(STATUS "${target}: an OpenSSL target")
    else()
        message(SEND_ERROR "${target}, named in cmake/openssl.cmake, is no target of the OpenSSL in ${OPENSSL}")
        set(failed 1)
    endif()
endforeach()
if(failed)
    message(FATAL_ERROR "openssl_targets: see the errors above")
endif()
