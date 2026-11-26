# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The x86-64 back end (tools/sailgen/README.md, "The x86-64 back end"): whether it is built, and the emitters sailgen
# generates for it. Its instruction selection, tools/sailgen/sdm/x64_isel.inc, is derived from the Intel SDM, Volume 2
# (2A-2D, order number 325383, revision 093US) and is in the tree: a build needs neither the manual nor pdftotext.
# tools/sailgen/sdm/isel.py makes it again, which the test sailgen_sdm_isel holds it to where the manual's PDF is
# (snx_sdm_pdf)
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/sailgen.cmake")

# on by default where the back end's code runs: every x86-64 system, Android's APK of it among them
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(snx_sail_x64_default ON)
else()
    set(snx_sail_x64_default OFF)
endif()
option(SNX_SAIL_X64 "Build the x86-64 back end: without it LLVM makes every function" ${snx_sail_x64_default})
set(SNX_X64 "${SNX_SAIL_X64}" CACHE INTERNAL "the x86-64 back end is built (SNX_SAIL_X64)")
# in the log: a tree keeps the SNX_SAIL_X64 it was first configured with
if(SNX_X64)
    set(snx_x64_built "built")
else()
    set(snx_x64_built "not built")
endif()
message(STATUS "x86-64 back end: ${snx_x64_built} for ${CMAKE_SYSTEM_PROCESSOR} (SNX_SAIL_X64=${SNX_SAIL_X64})")

# the file path, of the package cache: downloaded from url where it is not there, and the revision's that isel.py makes
# the selection from (its SHA-256), or the configure stops: with what came, what was expected and what to do
function(snx_sdm_download url path)
    file(STRINGS "${PROJECT_SOURCE_DIR}/tools/sailgen/sdm/isel.py" lines REGEX "^(REVISION|SHA256) = '.*'$")
    foreach(line ${lines})
        string(REGEX REPLACE "^([A-Z0-9]+) = '(.*)'$" "\\1;\\2" pair "${line}")
        list(GET pair 0 name)
        list(GET pair 1 ${name})
    endforeach()
    if(NOT REVISION OR NOT SHA256)
        message(FATAL_ERROR "tools/sailgen/sdm/isel.py names no REVISION and SHA256 of the Intel SDM")
    endif()
    string(CONCAT todo
           "Pass that revision's PDF with -DSNX_SDM_PDF=, or move to the one Intel serves: the version and hash of "
           "cpmfile.json's intel-sdm-vol2, REVISION and SHA256 of tools/sailgen/sdm/isel.py, and x64_isel.inc "
           "made again")
    set(expected "revision ${REVISION} of the Intel SDM, Volume 2, whose PDF's SHA-256 is ${SHA256}")
    get_filename_component(dir "${path}" DIRECTORY)
    file(LOCK "${dir}.lock" GUARD FUNCTION)
    if(EXISTS "${path}")
        file(SHA256 "${path}" got)
        if(got STREQUAL SHA256)
            return()
        endif()
        message(FATAL_ERROR "${path} isn't ${expected}: its SHA-256 is ${got}. ${todo}")
    endif()
    # without the pin's SHA-512, at which CMake would stop before this can say what to do: snx_package_file, after it,
    # checks that
    message(STATUS "${url} has to give ${expected}. If it doesn't: ${todo}")
    cpm_download("${url}" "${path}.part")
    file(SHA256 "${path}.part" got)
    if(NOT got STREQUAL SHA256)
        message(FATAL_ERROR "${url} gives a file whose SHA-256 is ${got}, not ${expected}. ${todo}")
    endif()
    file(RENAME "${path}.part" "${path}")
endfunction()

# snx_sdm_pdf(<pdf> <pdftotext>): what sailgen_sdm_isel derives the selection again from, for the tests alone: the PDF
# of SNX_SDM_PDF, else the CPM cache's (cpmfile.json's intel-sdm-vol2, a path whether or not it is there), and
# pdftotext, empty for none. With SNX_TEST_SDM pdftotext has to be there, and the PDF is downloaded into that cache
function(snx_sdm_pdf pdf pdftotext)
    set(SNX_SDM_PDF "" CACHE FILEPATH "PDF of the Intel SDM Vol. 2, 325383-093US, for the test sailgen_sdm_isel")
    find_program(SNX_PDFTOTEXT pdftotext)
    set(path "${SNX_SDM_PDF}")
    if(SNX_TEST_SDM AND NOT SNX_PDFTOTEXT)
        message(FATAL_ERROR "SNX_TEST_SDM=${SNX_TEST_SDM} needs pdftotext (poppler's), which is missing: install it "
                            "(docs/dev/build.md)")
    endif()
    if(NOT path)
        get_json_object(intel-sdm-vol2)
        set(JSON_NAME intel-sdm-vol2)
        parse_object("${object}")
        get_cache_path(${package} "${version}" dir)
        get_filename_component(name "${url}" NAME)
        set(path "${dir}/${name}")
        if(SNX_TEST_SDM)
            snx_sdm_download("${url}" "${path}")
            snx_package_file(intel-sdm-vol2) # cpmfile.json's SHA-512 of it too
        endif()
    endif()
    set(${pdf} "${path}" PARENT_SCOPE)
    if(SNX_PDFTOTEXT)
        set(${pdftotext} "${SNX_PDFTOTEXT}" PARENT_SCOPE)
    else()
        set(${pdftotext} "" PARENT_SCOPE)
    endif()
endfunction()

# snx_sailgen_a64_x64(<target>): generates a64_x64.inc (the A64 emitters of a64_x64.roots, with sailgen's x86-64 back
# end at its top: snx_sailgen_machine_backends' files) into the build folder, and gives <target> its include root and
# that of the instruction selection it includes (x64_isel.inc)
function(snx_sailgen_a64_x64 target)
    set(dir "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/sail")
    set(profile "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/decode/a64.profile")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/sailgen")
    snx_sailgen_machine_backends(x64 a64 files args)
    add_custom_command(OUTPUT "${out}/a64_x64.inc"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
        COMMAND snx-sailgen --generate "${dir}/a64_x64.roots" ${args} --profile "${profile}" -o "${out}/a64_x64.inc"
                "${dir}/a64.sail" --overlay "${dir}/nx.sail"
        DEPENDS snx-sailgen "${dir}/a64_x64.roots" "${dir}/a64.sail" "${dir}/nx.sail" ${files} "${profile}"
        COMMENT "Generating the A64 emitters' x86-64 code from Sail" VERBATIM)
    target_sources(${target} PRIVATE "${out}/a64_x64.inc")
    target_include_directories(${target} PRIVATE "${out}" "${PROJECT_SOURCE_DIR}/tools/sailgen/sdm")
endfunction()

# snx_sailgen_a32_x64(<target>): generates a32_x64.inc (the AArch32 emitters of a32.roots, LLVM's own, with sailgen's
# x86-64 back end at its top: snx_sailgen_machine_backends' files) into the build folder, and gives <target> its include
# root and that of the instruction selection it includes (x64_isel.inc)
function(snx_sailgen_a32_x64 target)
    set(dir "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/sail")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/sailgen")
    snx_sailgen_machine_backends(x64 a32 files args)
    add_custom_command(OUTPUT "${out}/a32_x64.inc"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
        COMMAND snx-sailgen --generate "${dir}/a32.roots" ${args} --profile "${dir}/a32.profile"
                -o "${out}/a32_x64.inc" "${dir}/a32.sail" --overlay "${dir}/arm.sail"
        DEPENDS snx-sailgen "${dir}/a32.roots" "${dir}/a32.sail" "${dir}/arm.sail" "${dir}/a32.profile" ${files}
        COMMENT "Generating the AArch32 emitters' x86-64 code from Sail" VERBATIM)
    target_sources(${target} PRIVATE "${out}/a32_x64.inc")
    target_include_directories(${target} PRIVATE "${out}" "${PROJECT_SOURCE_DIR}/tools/sailgen/sdm")
endfunction()
