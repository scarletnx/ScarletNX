# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
#   cmake -DSRC=<source dir> -DCPM=<CPM.cmake> -DWORK=<scratch dir> [-DGENERATOR=<generator> -DMAKE=<its program>]
#         -P tests/cpm_cache_test.cmake
#
# What a failed download leaves in CPM_SOURCE_CACHE (cmake/cpm_cache.cmake). The empty folder: a configure whose
# download fails leaves one, for a package that is one file and for an archive, in both of CPM's cache layouts; the next
# configure removes it, says which, and downloads the package; a folder with its package stays as it is. The file cut
# short: a package of one file (snx_cpm_file) is checked on every configure, and one that is not the pinned file stops
# it, with both hashes and the folder to delete. And cmake/dependencies.cmake sweeps the cache before its first package
# and adds every package of one file that way. Offline: the packages are files of this folder.
foreach(var SRC CPM WORK)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "cpm_cache_test.cmake needs -D${var}=...")
    endif()
endforeach()
function(fail what)
    message(FATAL_ERROR "cpm_cache: ${what}")
endfunction()

file(REMOVE_RECURSE "${WORK}")
file(WRITE "${WORK}/payload/one.txt" "the whole file\n")
file(WRITE "${WORK}/payload/arc-1/a.c" "int a;\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar czf arc.tar.gz arc-1 WORKING_DIRECTORY "${WORK}/payload"
                RESULT_VARIABLE failed)
if(failed)
    fail("could not make the archive")
endif()
file(SHA256 "${WORK}/payload/one.txt" one_sha256)
file(SHA256 "${WORK}/payload/arc.tar.gz" arc_sha256)
# how a project adds each: CPM's own call for the one file and for the archive, snx_cpm_file for the one file
set(served "${WORK}/served")
set(one_file one.txt)
set(one_add "CPMAddPackage(NAME one VERSION 1 URL \"${served}/one.txt\" URL_HASH SHA256=${one_sha256}
              DOWNLOAD_ONLY YES DOWNLOAD_NO_EXTRACT YES)")
set(arc_file arc.tar.gz)
set(arc_add "CPMAddPackage(NAME arc VERSION 1 URL \"${served}/arc.tar.gz\" URL_HASH SHA256=${arc_sha256}
              DOWNLOAD_ONLY YES)")
set(checked_add "snx_cpm_file(checked 1 \"${served}/one.txt\" ${one_sha256})")
set(generator "")
if(GENERATOR)
    set(generator -G "${GENERATOR}" "-DCMAKE_MAKE_PROGRAM=${MAKE}")
endif()

# a configure of a project with one package, as cmake/dependencies.cmake adds them, in a build tree of its own
set(configures 0)
function(configure kind cache named)
    math(EXPR configures "${configures} + 1")
    set(configures ${configures} PARENT_SCOPE)
    set(project "${WORK}/project-${kind}")
    file(WRITE "${project}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.25)
project(cpm_cache_test NONE)
include(\"${CPM}\")
include(\"${SRC}/cmake/cpm_cache.cmake\")
snx_cpm_cache_sweep()
${${kind}_add}
file(WRITE \"\${CMAKE_BINARY_DIR}/folder.txt\" \"\${${kind}_SOURCE_DIR}\")
")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${project}" -B "${WORK}/build${configures}" ${generator}
                            "-DCPM_SOURCE_CACHE=${cache}" "-DCPM_USE_NAMED_CACHE_DIRECTORIES=${named}"
                    RESULT_VARIABLE failed OUTPUT_VARIABLE out ERROR_VARIABLE out)
    set(failed "${failed}" PARENT_SCOPE)
    set(out "${out}" PARENT_SCOPE)
    set(folder "" PARENT_SCOPE)
    if(EXISTS "${WORK}/build${configures}/folder.txt")
        file(READ "${WORK}/build${configures}/folder.txt" folder)
        set(folder "${folder}" PARENT_SCOPE)
    endif()
endfunction()

foreach(named OFF ON)
    foreach(kind one arc)
        set(cache "${WORK}/cache-${kind}-${named}")
        set(where "${kind}, CPM_USE_NAMED_CACHE_DIRECTORIES=${named}")
        # the download fails: nothing is served
        file(REMOVE_RECURSE "${WORK}/served")
        configure(${kind} "${cache}" ${named})
        if(NOT failed)
            fail("${where}: a configure whose download failed went on:\n${out}")
        endif()
        if(named)
            file(GLOB left LIST_DIRECTORIES TRUE "${cache}/${kind}/*/${kind}")
        else()
            file(GLOB left LIST_DIRECTORIES TRUE "${cache}/${kind}/*")
            list(FILTER left EXCLUDE REGEX "\\.(lock|hash)$")
        endif()
        list(LENGTH left count)
        file(GLOB inside LIST_DIRECTORIES TRUE "${left}/*")
        if(NOT count EQUAL 1 OR NOT IS_DIRECTORY "${left}" OR inside)
            fail("${where}: the failed download left [${left}] holding [${inside}], not one empty folder: this test "
                 "is about that folder")
        endif()
        # the server has it again: that folder is removed, said so, and the package downloaded
        file(COPY "${WORK}/payload/${${kind}_file}" DESTINATION "${WORK}/served")
        configure(${kind} "${cache}" ${named})
        if(failed)
            fail("${where}: the configure after a failed download fails:\n${out}")
        endif()
        if(NOT folder STREQUAL left)
            fail("${where}: the package is in ${folder}, and the failed download left ${left}")
        endif()
        string(FIND "${out}" "${left} was empty" said)
        if(said LESS 0)
            fail("${where}: nothing says ${left} was removed:\n${out}")
        endif()
        if(kind STREQUAL "one")
            set(got "")
            if(EXISTS "${folder}/one.txt")
                file(SHA256 "${folder}/one.txt" got)
            endif()
            if(NOT got STREQUAL one_sha256)
                fail("${where}: ${folder}/one.txt has the SHA-256 [${got}] after the next configure, not ${one_sha256}")
            endif()
        elseif(NOT EXISTS "${folder}/a.c")
            fail("${where}: ${folder} has no a.c after the next configure")
        endif()
        # a folder with its package: kept, a file added to it too, and nothing said
        file(WRITE "${folder}/marker" "")
        configure(${kind} "${cache}" ${named})
        string(FIND "${out}" "was empty" said)
        if(failed OR NOT EXISTS "${folder}/marker" OR said GREATER_EQUAL 0)
            fail("${where}: a folder with its package was not left as it was:\n${out}")
        endif()
    endforeach()
endforeach()

# a package of one file, added by snx_cpm_file: taken while its file is the pinned one
set(cache "${WORK}/cache-checked")
file(COPY "${WORK}/payload/one.txt" DESTINATION "${served}")
configure(checked "${cache}" OFF)
if(failed OR NOT EXISTS "${folder}/one.txt")
    fail("a package of one file is not added by snx_cpm_file:\n${out}")
endif()
set(package "${folder}")
# its file cut short in the cache: the configure stops, with the file, its hash, the pinned one and the folder
file(WRITE "${package}/one.txt" "the who")
file(SHA256 "${package}/one.txt" cut)
configure(checked "${cache}" OFF)
if(NOT failed)
    fail("a file cut short in the cache was taken for the package")
endif()
foreach(said "${package}/one.txt" "${cut}" "${one_sha256}" "Delete" "${package}")
    string(FIND "${out}" "${said}" at)
    if(at LESS 0)
        fail("a file cut short: the message lacks [${said}]:\n${out}")
    endif()
endforeach()
# no file at all, in a folder that is not empty: stopped too
file(REMOVE "${package}/one.txt")
file(WRITE "${package}/other" "")
configure(checked "${cache}" OFF)
string(FIND "${out}" "has no one.txt" at)
if(NOT failed OR at LESS 0)
    fail("a folder without the package's file was taken for the package:\n${out}")
endif()
# what the message says to do: the folder deleted, and the next configure downloads the file
file(REMOVE_RECURSE "${package}")
configure(checked "${cache}" OFF)
set(got "")
if(EXISTS "${package}/one.txt")
    file(SHA256 "${package}/one.txt" got)
endif()
if(failed OR NOT got STREQUAL one_sha256)
    fail("after its folder is deleted, the package's file has the SHA-256 [${got}], not ${one_sha256}:\n${out}")
endif()

# cmake/dependencies.cmake: CPM included, then the sweep, then the first package
file(READ "${SRC}/cmake/dependencies.cmake" dependencies)
string(FIND "${dependencies}" "include(\"\${cpm_file}\")" at_cpm)
string(FIND "${dependencies}" "\nsnx_cpm_cache_sweep()\n" at_sweep)
string(FIND "${dependencies}" "CPMAddPackage(" at_package)
if(at_cpm LESS 0 OR at_sweep LESS at_cpm OR at_package LESS at_sweep)
    fail("cmake/dependencies.cmake does not call snx_cpm_cache_sweep() between including CPM (at ${at_cpm}) and its "
         "first package (at ${at_package}): the call is at ${at_sweep}")
endif()
# and its packages of one file, and the tests' vectors, are checked on every configure
string(FIND "${dependencies}" "DOWNLOAD_NO_EXTRACT" at)
if(at GREATER_EQUAL 0)
    fail("cmake/dependencies.cmake adds a package of one file with CPMAddPackage and DOWNLOAD_NO_EXTRACT (at ${at}), "
         "which checks the file only when it downloads it: snx_cpm_file adds it")
endif()
file(READ "${SRC}/tests/CMakeLists.txt" tests)
string(FIND "${tests}" "snx_cpm_file_check(\"\${\${package}_SOURCE_DIR}\" \"\${file}\" \${sha256})" at)
if(at LESS 0)
    fail("tests/CMakeLists.txt does not check its VP9 vectors with snx_cpm_file_check on every configure")
endif()
message(STATUS "cpm_cache: ${configures} configures, the failed downloads' folders removed, a file cut short refused")
