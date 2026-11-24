# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
#   cmake -DSRC=<source dir> -DWORK=<scratch dir> -P tests/cpmutil_cache_test.cmake
#
# What CPMUtil (cmake/cpmutil) and snx_package_file (cmake/packages.cmake) keep in CPM_SOURCE_CACHE, served from a
# folder of the test: an archive, extracted into <package>/<version>, and a package of one file, kept as downloaded
# there. A download that fails says the URL and the status; a folder a fetch cut short is fetched again; a file of one
# download that is not the pinned one is downloaded again, or refused in a <package>_CUSTOM_DIR, with both hashes; a
# configure waits for another one's fetch of the same package instead of removing it, and takes what it fetched;
# another package's fetch goes on beside it; a package's patches are applied, and stop the configure where one does not
# apply without fuzz; and a patched package's folder is named by its patches (their bytes in order), so two sets of
# them have each its own folder in a cache, while a package with no patches keeps <package>/<version>.
# CPMUtil's 5 s between tries is left out: each failure would take 30 s
# the policies of the CMake the build needs: a script has none of its own, and before CMake 4 an unset CMP0054 read
# if(kind STREQUAL "arc") as the variable arc
cmake_minimum_required(VERSION 3.28)
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
file(WRITE "${WORK}/payload/pat-1/text.txt" "one\ntwo\nthree\nfour\nfive\nsix\nseven\n")
file(WRITE "${WORK}/payload/pat-1/more.txt" "eight\n")
foreach(kind arc pat)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar czf ${kind}.tar.gz ${kind}-1 WORKING_DIRECTORY "${WORK}/payload"
                    RESULT_VARIABLE failed)
    if(failed)
        message(FATAL_ERROR "cpmutil_cache: could not make the archive ${kind}.tar.gz")
    endif()
endforeach()
file(SHA512 "${WORK}/payload/one.txt" one_hash)
file(SHA512 "${WORK}/payload/arc.tar.gz" arc_hash)
file(SHA512 "${WORK}/payload/pat.tar.gz" pat_hash)
set(served "${WORK}/served")
# a file URL of that folder: file:///C:/... on Windows
set(url "file://${served}")
if(NOT served MATCHES "^/")
    set(url "file:///${served}")
endif()
set(cache "${WORK}/cache")
# the pins, the one of pat with its patches: a JSON array's contents
function(write_pins patches)
    file(WRITE "${WORK}/project/cpmfile.json" "{
    \"arc\": {\"url\": \"${url}/arc.tar.gz\", \"version\": \"1\", \"hash\": \"${arc_hash}\"},
    \"one\": {\"url\": \"${url}/one.txt\", \"version\": \"1\", \"hash\": \"${one_hash}\"},
    \"pat\": {\"url\": \"${url}/pat.tar.gz\", \"version\": \"1\", \"hash\": \"${pat_hash}\",
             \"patches\": [${patches}]}
}
")
endfunction()
write_pins("\"0001-four.patch\"")
# KIND (arc or one) added as cmake/dependencies.cmake adds it, after WAIT names a file to appear (the other fetch's
# lock taken), its download paused with PAUSE; its folder written to folder.txt
file(WRITE "${WORK}/project/CMakeLists.txt" "cmake_minimum_required(VERSION 3.28)
project(cpmutil_cache_test LANGUAGES NONE)
include(\"${SRC}/cmake/packages.cmake\")
macro(sleep time)
endmacro()
# PAUSE: the download, its package's lock held, says so in LOCKED and waits for PAUSE to appear
if(PAUSE)
    function(cpm_download)
        file(WRITE \"\${LOCKED}\" \"\")
        foreach(i RANGE 300)
            if(EXISTS \"\${PAUSE}\")
                break()
            endif()
            execute_process(COMMAND \"\${CMAKE_COMMAND}\" -E sleep 0.1)
        endforeach()
        if(NOT EXISTS \"\${PAUSE}\")
            message(FATAL_ERROR \"\${PAUSE} did not appear while \${KIND}'s fetch held its lock\")
        endif()
        _cpm_download(\${ARGV})
    endfunction()
endif()
if(WAIT)
    foreach(i RANGE 200)
        if(EXISTS \"\${WAIT}\")
            break()
        endif()
        execute_process(COMMAND \"\${CMAKE_COMMAND}\" -E sleep 0.1)
    endforeach()
endif()
if(KIND STREQUAL \"arc\" OR KIND STREQUAL \"pat\")
    AddJsonPackage(NAME \${KIND} DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
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

# A package's patches (.patch/<key>/, cpmfile.json's "patches"): applied to what the fetch extracts, in a folder named
# by the version and by the first 10 characters of the SHA-512 of their bytes, in the order the list gives; one that does
# not apply as it is written, without fuzz, stops the configure, naming it and the folder, and leaves the folder to be
# fetched again
function(patch_four line context)
    file(WRITE "${WORK}/project/.patch/pat/0001-four.patch" "--- a/text.txt\n+++ b/text.txt\n@@ -1,7 +1,7 @@\n"
               " ${context}\n two\n three\n-four\n+${line}\n five\n six\n seven\n")
endfunction()
# the folder the patches ARGN (names in .patch/pat) give
function(patched_folder out)
    set(bytes "")
    foreach(name IN LISTS ARGN)
        file(READ "${WORK}/project/.patch/pat/${name}" content)
        string(APPEND bytes "${content}")
    endforeach()
    string(SHA512 key "${bytes}")
    string(SUBSTRING "${key}" 0 10 key)
    set(${out} "${cache}/pat/1-${key}" PARENT_SCOPE)
endfunction()
function(text_of out folder)
    set(text "")
    if(EXISTS "${folder}/text.txt")
        file(READ "${folder}/text.txt" text)
    endif()
    set(${out} "${text}" PARENT_SCOPE)
endfunction()
file(COPY "${WORK}/payload/pat.tar.gz" DESTINATION "${served}")
# a cache made before the folder was named by the patches, the version alone its name: read by none and left as it is
file(WRITE "${cache}/pat/1/text.txt" "an older version's\n")
file(WRITE "${cache}/pat/1/.cpm_patch_key" "none")
set(sets "")
foreach(line FOUR Four)
    patch_four(${line} one)
    configure(pat)
    patched_folder(want 0001-four.patch)
    text_of(text "${folder}")
    if(failed OR NOT folder STREQUAL want OR NOT text STREQUAL "one\ntwo\nthree\n${line}\nfive\nsix\nseven\n" OR
       NOT EXISTS "${want}/.cpm_patch_key")
        fail("pat: the patch that writes ${line} gave [${text}] in [${folder}], not ${want}:\n${out}")
    endif()
    list(APPEND sets "${want}")
endforeach()
text_of(text "${cache}/pat/1")
if(NOT text STREQUAL "an older version's\n")
    fail("pat: the folder named by the version alone reads [${text}]")
endif()
# each set of patches has kept its own, and is taken from there with nothing served: a branch that goes back
list(REMOVE_DUPLICATES sets)
list(LENGTH sets distinct)
if(NOT distinct EQUAL 2)
    fail("pat: two sets of patches are in [${sets}]")
endif()
file(REMOVE_RECURSE "${served}")
foreach(line FOUR Four)
    patch_four(${line} one)
    configure(pat)
    patched_folder(want 0001-four.patch)
    text_of(text "${folder}")
    if(failed OR NOT folder STREQUAL want OR NOT text STREQUAL "one\ntwo\nthree\n${line}\nfive\nsix\nseven\n")
        fail("pat: with nothing served, the patches that write ${line} are not in ${want}, [${folder}] is:\n${out}")
    endif()
endforeach()
file(COPY "${WORK}/payload/pat.tar.gz" DESTINATION "${served}")
# two patches in the other order are another set, and the same bytes under another name are the same set
file(WRITE "${WORK}/project/.patch/pat/0002-more.patch" "--- a/more.txt\n+++ b/more.txt\n@@ -1 +1 @@\n-eight\n+EIGHT\n")
patch_four(FOUR one)
foreach(order "0001-four.patch;0002-more.patch" "0002-more.patch;0001-four.patch")
    list(JOIN order "\", \"" names)
    write_pins("\"${names}\"")
    configure(pat)
    patched_folder(want ${order})
    text_of(text "${folder}")
    set(more "")
    if(EXISTS "${folder}/more.txt")
        file(READ "${folder}/more.txt" more)
    endif()
    if(failed OR NOT folder STREQUAL want OR NOT text STREQUAL "one\ntwo\nthree\nFOUR\nfive\nsix\nseven\n" OR
       NOT more STREQUAL "EIGHT\n")
        fail("pat: the patches [${order}] gave [${text}${more}] in [${folder}], not ${want}:\n${out}")
    endif()
    list(APPEND sets "${want}")
endforeach()
list(REMOVE_DUPLICATES sets)
list(LENGTH sets distinct)
if(NOT distinct EQUAL 4)
    fail("pat: four sets of patches are in [${sets}]")
endif()
file(RENAME "${WORK}/project/.patch/pat/0002-more.patch" "${WORK}/project/.patch/pat/0003-more.patch")
write_pins("\"0001-four.patch\", \"0003-more.patch\"")
configure(pat)
list(GET sets 2 want)
if(failed OR NOT folder STREQUAL want)
    fail("pat: a patch renamed with the same bytes is in [${folder}], not ${want}:\n${out}")
endif()
write_pins("\"0001-four.patch\"")
# its first line of context another: a fuzz of 1 would apply it. The folder it is left in is fetched again by the next
# configure, which fails the same, and the others' stay
patch_four(FOUR uno)
patched_folder(pat 0001-four.patch)
foreach(again 0 1)
    file(WRITE "${pat}/half" "")
    configure(pat)
    if(NOT failed OR EXISTS "${pat}/.cpm_patch_key" OR EXISTS "${pat}/half")
        fail("pat: a patch that applies only with fuzz was applied, or its folder is not fetched again:\n${out}")
    endif()
    says(pat-fuzz "${WORK}/project/.patch/pat/0001-four.patch does not apply to ${pat}:")
endforeach()
patch_four(FOUR one)
configure(pat)
patched_folder(pat 0001-four.patch)
if(failed OR NOT folder STREQUAL pat OR NOT EXISTS "${pat}/.cpm_patch_key")
    fail("pat: a patch that applies, after one that did not, is in [${folder}], not ${pat}:\n${out}")
endif()
# a folder of the packager's for a patched package, which CPMUtil would take unpatched: refused, with the patches
configure(pat "-Dpat_CUSTOM_DIR=${WORK}/payload/pat-1")
if(NOT failed)
    fail("pat: pat_CUSTOM_DIR was taken unpatched:\n${out}")
endif()
says(pat-custom "pat_CUSTOM_DIR=${WORK}/payload/pat-1: CPMUtil takes that folder as it is, and cpmfile.json patches"
                "pat with 0001-four.patch (.patch/pat).")

# A fetch locks its own package alone: a configure fetches one package while another configure is inside its fetch of
# the other one (PAUSE: its download waits until the first configure has written its folder, or 30 s, and then fails).
# A lock that kept other packages out would have the first configure wait for the paused one, which waits for it
file(COPY "${WORK}/payload/arc.tar.gz" "${WORK}/payload/one.txt" DESTINATION "${served}")
foreach(kind arc one)
    if(kind STREQUAL "arc")
        set(other one)
    else()
        set(other arc)
    endif()
    file(REMOVE_RECURSE "${cache}/${kind}" "${cache}/${other}" "${WORK}/locked")
    math(EXPR configures "${configures} + 2")
    math(EXPR paused "${configures} - 1")
    set(build "${WORK}/build${configures}")
    # side by side, as a pipe: the paused one second, as it ends last (its output would find the pipe closed)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=CPM_SOURCE_CACHE "${CMAKE_COMMAND}" -S "${WORK}/project"
                            -B "${build}" "-DCPM_SOURCE_CACHE=${cache}" "-DKIND=${kind}" "-DWAIT=${WORK}/locked"
                    COMMAND "${CMAKE_COMMAND}" -E env --unset=CPM_SOURCE_CACHE "${CMAKE_COMMAND}" -S "${WORK}/project"
                            -B "${WORK}/build${paused}" "-DCPM_SOURCE_CACHE=${cache}" "-DKIND=${other}"
                            "-DPAUSE=${build}/folder.txt" "-DLOCKED=${WORK}/locked"
                    RESULTS_VARIABLE results OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if(NOT results STREQUAL "0;0" OR NOT EXISTS "${cache}/${kind}/1" OR NOT EXISTS "${cache}/${other}/1")
        fail("${kind}: fetched while ${other}'s fetch held its lock, the two configures exited [${results}]:\n${out}")
    endif()
endforeach()

get_property(failed GLOBAL PROPERTY failed)
if(failed)
    message(FATAL_ERROR "cpmutil_cache: ${failed}")
endif()
message(STATUS "cpmutil_cache: ${configures} configures, the failures said, the cut packages fetched again, the "
               "fetches of one package one at a time, and of two packages side by side")
file(REMOVE_RECURSE "${WORK}")
