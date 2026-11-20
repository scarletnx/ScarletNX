# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The key of libcurl's cached build (cmake/curl.cmake, SNX_DEPS_CACHE) is the same in every build tree: two trees of
# one small project that includes the module, with the pins and the cache's functions stood in for, record the key
# they would look the build up by, and it holds neither tree's folder. Run by ctest as curl_cache_key, with -DSRC,
# -DWORK, -DGENERATOR and -DMAKE (the build's)
file(REMOVE_RECURSE "${WORK}")
file(WRITE "${WORK}/project/CMakeLists.txt" "cmake_minimum_required(VERSION 3.28)
project(curl_cache_key_test LANGUAGES NONE)
set(curl_SOURCE_DIR \"${WORK}/curl-source\")
set(CMAKE_AR \"${CMAKE_COMMAND}\") # the archiver and its index, found by path: a program that is there
set(CMAKE_RANLIB \"${CMAKE_COMMAND}\")
set(snx_curl_hash a-source-hash)
set(snx_openssl_hash an-openssl-hash)
set(snx_openssl_prefix \"${WORK}/openssl\")
add_library(OpenSSL::SSL INTERFACE IMPORTED)
add_library(Threads::Threads INTERFACE IMPORTED)
# the cache's lookup: its key's texts written down, and a build found, so that none is made
function(snx_deps_cached name out)
    file(WRITE \"\${CMAKE_BINARY_DIR}/key.txt\" \"\${ARGN}\")
    set(\${out}_FOUND TRUE PARENT_SCOPE)
    set(\${out}_DIR \"${WORK}/found\" PARENT_SCOPE)
endfunction()
include(\"${SRC}/cmake/curl.cmake\")
")
foreach(tree one two)
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${WORK}/project" -B "${WORK}/${tree}" -G "${GENERATOR}"
                            "-DCMAKE_MAKE_PROGRAM=${MAKE}"
                    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "the project did not configure in ${tree} (exit ${result}):\n${out}")
    endif()
    file(READ "${WORK}/${tree}/key.txt" key_${tree})
endforeach()
if(NOT key_one MATCHES "configure: .*CURL_USE_OPENSSL")
    message(FATAL_ERROR "the key does not hold curl's configure arguments:\n${key_one}")
endif()
string(FIND "${key_one}" "${WORK}/one" at)
if(NOT key_one STREQUAL key_two OR at GREATER -1)
    message(FATAL_ERROR "two build trees look libcurl up by different keys:\n${key_one}\n${key_two}")
endif()
file(REMOVE_RECURSE "${WORK}")
