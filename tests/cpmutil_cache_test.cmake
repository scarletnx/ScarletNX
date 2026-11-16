# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
#   cmake -DSRC=<source dir> -DWORK=<scratch dir> -P tests/cpmutil_cache_test.cmake
#
# What CPMUtil (cmake/cpmutil) and snx_package_file (cmake/packages.cmake) keep in CPM_SOURCE_CACHE, served from a
# folder of the test: an archive, extracted into <package>/<version>, and a package of one file, kept as downloaded
# there. A download that fails says the URL and the status; a folder a fetch cut short is fetched again; a file of one
# download that is not the pinned one is downloaded again, or refused in a <package>_CUSTOM_DIR, with both hashes; and
# a configure waits for another one's fetch of the same package instead of removing it, and takes what it fetched.
# CPMUtil's 5 s between tries is left out: each failure would take 30 s
if(NOT DEFINED SRC OR NOT DEFINED WORK)
    message(FATAL_ERROR "cpmutil_cache_test.cmake needs -DSRC=... -DWORK=...")
endif()
function(fail what)
    set_property(GLOBAL APPEND PROPERTY failed "${what}")
    message(SEND_ERROR "cpmutil_cache: ${what}")
endfunction()

file(REMOVE_RECURSE "${WORK}")
file(WRITE "${WORK}/payload/one.txt" "the whole file\n")
file(WRITE "${WORK}/payload/arc-1/a.c" "int a;\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar czf arc.tar.gz arc-1 WORKING_DIRECTORY "${WORK}/payload"
                RESULT_VARIABLE failed)
if(failed)
    message(FATAL_ERROR "cpmutil_cache: could not make the archive")
endif()
file(SHA512 "${WORK}/payload/one.txt" one_hash)
file(SHA512 "${WORK}/payload/arc.tar.gz" arc_hash)
set(served "${WORK}/served")
# a file URL of that folder: file:///C:/... on Windows
set(url "file://${served}")
if(NOT served MATCHES "^/")
    set(url "file:///${served}")
endif()
set(cache "${WORK}/cache")
file(WRITE "${WORK}/project/cpmfile.json" "{
    \"arc\": {\"url\": \"${url}/arc.tar.gz\", \"version\": \"1\", \"hash\": \"${arc_hash}\"},
    \"one\": {\"url\": \"${url}/one.txt\", \"version\": \"1\", \"hash\": \"${one_hash}\"}
}
")
# KIND (arc or one) added as cmake/dependencies.cmake adds it, after WAIT names a file to appear (the other fetch's
# lock taken); its folder written to folder.txt
file(WRITE "${WORK}/project/CMakeLists.txt" "cmake_minimum_required(VERSION 3.31)
project(cpmutil_cache_test LANGUAGES NONE)
include(\"${SRC}/cmake/packages.cmake\")
macro(sleep time)
endmacro()
if(WAIT)
    foreach(i RANGE 200)
        if(EXISTS \"\${WAIT}\")
            break()
        endif()
        execute_process(COMMAND \"\${CMAKE_COMMAND}\" -E sleep 0.1)
    endforeach()
endif()
if(KIND STREQUAL \"arc\")
    AddJsonPackage(NAME arc DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
else()
    snx_package_file(one)
endif()
file(WRITE \"\${CMAKE_BINARY_DIR}/folder.txt\" \"\${\${KIND}_SOURCE_DIR}\")
")

# configures the project with KIND and ARGN in a build tree of its own; failed, out and folder for the caller
set(configures 0)
function(configure kind)
    math(EXPR configures "${configures} + 1")
    set(configures ${configures} PARENT_SCOPE)
    set(build "${WORK}/build${configures}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=CPM_SOURCE_CACHE "${CMAKE_COMMAND}" -S "${WORK}/project"
                            -B "${build}" "-DCPM_SOURCE_CACHE=${cache}" "-DKIND=${kind}" ${ARGN}
                    RESULT_VARIABLE failed OUTPUT_VARIABLE out ERROR_VARIABLE out)
    string(REGEX REPLACE "\n *" " " out "${out}") # CMake wraps an error's text
    set(failed "${failed}" PARENT_SCOPE)
    set(out "${out}" PARENT_SCOPE)
    set(folder "" PARENT_SCOPE)
    if(EXISTS "${build}/folder.txt")
        file(READ "${build}/folder.txt" folder)
        set(folder "${folder}" PARENT_SCOPE)
    endif()
endfunction()
function(says label)
    foreach(want IN LISTS ARGN)
        string(FIND "${out}" "${want}" at)
        if(at LESS 0)
            fail("${label}: the configure did not say [${want}]:\n${out}")
        endif()
    endforeach()
endfunction()
function(sha512 out file)
    set(have "")
    if(EXISTS "${file}")
        file(SHA512 "${file}" have)
    endif()
    set(${out} "${have}" PARENT_SCOPE)
endfunction()

foreach(kind arc one)
    set(package "${cache}/${kind}/1")
    file(REMOVE_RECURSE "${served}")
    # the download fails: stopped, saying the URL and the status
    configure(${kind})
    if(NOT failed)
        fail("${kind}: a configure whose download failed went on:\n${out}")
    endif()
    says(${kind}-failed "Download of ${url}/${kind}" "failed 6 times, the last with status")
    # served now: downloaded, into <package>/<version>
    if(kind STREQUAL "arc")
        file(COPY "${WORK}/payload/arc.tar.gz" DESTINATION "${served}")
    else()
        file(COPY "${WORK}/payload/one.txt" DESTINATION "${served}")
    endif()
    configure(${kind})
    if(failed OR NOT folder STREQUAL package)
        fail("${kind}: the package is in [${folder}], not ${package}:\n${out}")
    endif()
    file(GLOB parts "${cache}/${kind}/*.part" "${package}/*.part")
    if(parts)
        fail("${kind}: the download left [${parts}] behind")
    endif()
    # no longer served: taken from the cache
    file(REMOVE_RECURSE "${served}")
    configure(${kind})
    if(failed OR NOT folder STREQUAL package)
        fail("${kind}: with nothing served, the cached package is not taken:\n${out}")
    endif()
endforeach()
file(COPY "${WORK}/payload/arc.tar.gz" "${WORK}/payload/one.txt" DESTINATION "${served}")

# the archive's folder as a fetch cut short leaves it, extracted in part and without the key CPMUtil writes last:
# fetched again
set(arc "${cache}/arc/1")
file(REMOVE "${arc}/.cpm_patch_key" "${arc}/a.c")
file(WRITE "${arc}/half" "")
configure(arc)
if(failed OR NOT EXISTS "${arc}/a.c" OR EXISTS "${arc}/half")
    fail("arc: a folder a fetch cut short was not fetched again:\n${out}")
endif()

# the one file cut short, or another: downloaded again, and the configure says so
set(one "${cache}/one/1/one.txt")
file(WRITE "${one}" "the who")
configure(one)
sha512(got "${one}")
if(failed OR NOT got STREQUAL one_hash)
    fail("one: a file cut short has the SHA-512 [${got}] after the next configure, not ${one_hash}:\n${out}")
endif()
says(one-again "Downloading one@1 from ${url}/one.txt")
# a folder of the packager's: taken while its file is the pinned one, refused with both hashes otherwise
file(COPY "${WORK}/payload/one.txt" DESTINATION "${WORK}/custom")
configure(one "-Done_CUSTOM_DIR=${WORK}/custom")
if(failed OR NOT folder STREQUAL "${WORK}/custom")
    fail("one: one_CUSTOM_DIR=${WORK}/custom is not where the package is taken from, [${folder}] is:\n${out}")
endif()
file(WRITE "${WORK}/custom/one.txt" "another file\n")
file(SHA512 "${WORK}/custom/one.txt" other)
configure(one "-Done_CUSTOM_DIR=${WORK}/custom")
if(NOT failed)
    fail("one: another file in one_CUSTOM_DIR was taken:\n${out}")
endif()
says(one-custom "one_CUSTOM_DIR=${WORK}/custom: ${WORK}/custom/one.txt has the SHA-512 [${other}], and"
                "cpmfile.json pins ${one_hash}")

# Two configures fetching one package: the second waits while the first holds the package's lock, then takes what it
# fetched (the holder's own file) instead of removing it and downloading; nothing is served, so a fetch would fail.
# The holder takes the lock, says so in a file, waits 2 s, writes the package as a fetch does and lets go
file(REMOVE_RECURSE "${served}")
foreach(kind arc one)
    file(REMOVE_RECURSE "${cache}/${kind}" "${WORK}/locked")
    if(kind STREQUAL "arc")
        set(write "file(WRITE \"${cache}/arc/1/from-holder\" \"\")\nfile(WRITE \"${cache}/arc/1/.cpm_patch_key\" none)")
        set(lock "${cache}/arc/1.lock")
        set(made "${cache}/arc/1/from-holder")
    else()
        set(write "file(COPY \"${WORK}/payload/one.txt\" DESTINATION \"${cache}/one/1\")")
        set(lock "${cache}/one/1.lock")
        set(made "${cache}/one/1/one.txt")
    endif()
    file(WRITE "${WORK}/holder-${kind}.cmake" "file(LOCK \"${lock}\")
file(WRITE \"${WORK}/locked\" \"\")
execute_process(COMMAND \"\${CMAKE_COMMAND}\" -E sleep 2)
${write}
file(LOCK \"${lock}\" RELEASE)
")
    math(EXPR configures "${configures} + 1")
    execute_process(COMMAND "${CMAKE_COMMAND}" -P "${WORK}/holder-${kind}.cmake"
                    COMMAND "${CMAKE_COMMAND}" -E env --unset=CPM_SOURCE_CACHE "${CMAKE_COMMAND}" -S "${WORK}/project"
                            -B "${WORK}/build${configures}" "-DCPM_SOURCE_CACHE=${cache}" "-DKIND=${kind}"
                            "-DWAIT=${WORK}/locked"
                    RESULTS_VARIABLE results OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if(NOT results STREQUAL "0;0" OR NOT EXISTS "${made}")
        fail("${kind}: the holder and a configure that waited for its fetch exited [${results}], and ${made} is not "
             "what the holder wrote:\n${out}")
    endif()
endforeach()

get_property(failed GLOBAL PROPERTY failed)
if(failed)
    message(FATAL_ERROR "cpmutil_cache: ${failed}")
endif()
message(STATUS "cpmutil_cache: ${configures} configures, the failures said, the cut packages fetched again, the "
               "fetches of one package one at a time")
file(REMOVE_RECURSE "${WORK}")
