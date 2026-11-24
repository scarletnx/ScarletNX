# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The Intel SDM, Volume 2 (2A-2D, order number 325383, revision 093US), which the x86-64 back end's instruction
# selection is derived from as the build runs (tools/sailgen/README.md, "The x86-64 back end"): pdftotext's text of
# the PDF, cut into its instruction pages by sailgen --sdm with the errata's overrides (tools/sailgen/sdm/), then
# sailgen --isel matches the back end's primitives (tools/sailgen/x64.ops) with the pages' pseudocode. The manual is not
# kept in the tree: pass its PDF with -DSNX_SDM_PDF=, or it is downloaded once into the CPM cache (cpmfile.json's
# intel-sdm-vol2); either way its SHA-256 must be 093US's, so every build derives from the same text. Off unless SNX_SAIL_X64 is set
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/sailgen.cmake")

# on by default where the back end runs (x86-64, not Windows: its objects are COFF); without the manual or pdftotext
# it is left off, with a warning, and LLVM makes every function
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$" AND NOT WIN32)
    set(snx_sail_x64_default ON)
else()
    set(snx_sail_x64_default OFF)
endif()
option(SNX_SAIL_X64 "Derive the x86-64 back end's instruction selection from the Intel SDM (needs pdftotext)"
       ${snx_sail_x64_default})
set(SNX_SDM_PDF "" CACHE FILEPATH "Intel SDM Vol. 2 PDF, order no. 325383-093US (downloaded if empty)")
set(snx_sdm_sha256 d137a7887787bbb06c1a8b8bafd51958d8d2e16b4dff987a03226c845cca1516)

# the manual and pdftotext, found once: where either is missing (no PDF given and no download, a revision other than
# 093US, no pdftotext) the x86-64 back end is left out with a warning, and LLVM makes every function
set(SNX_X64 OFF CACHE INTERNAL "the x86-64 back end is built (SNX_SAIL_X64 and the manual found)")
if(SNX_SAIL_X64)
    set(snx_sdm_why "")
    set(pdf "${SNX_SDM_PDF}")
    if(NOT pdf)
        # pinned in cpmfile.json (intel-sdm-vol2) by its SHA-512, kept in the CPM cache: Intel's link serves the latest
        # revision, so a newer one there stops the configure until the entry moves to it, or -DSNX_SDM_PDF= names 093US
        snx_package_file(intel-sdm-vol2)
        set(pdf "${IntelSDM_SOURCE_DIR}/671110")
    endif()
    if(NOT snx_sdm_why)
        file(SHA256 "${pdf}" got)
        if(NOT got STREQUAL snx_sdm_sha256)
            set(snx_sdm_why "${pdf} isn't revision 325383-093US (Intel's link serves the latest): pass the 093US PDF "
                            "with -DSNX_SDM_PDF=")
        endif()
    endif()
    find_program(SNX_PDFTOTEXT pdftotext)
    if(NOT snx_sdm_why AND NOT SNX_PDFTOTEXT)
        set(snx_sdm_why "pdftotext (poppler) is missing")
    endif()
    if(snx_sdm_why)
        message(WARNING "The x86-64 back end is left out, LLVM makes every function: the Intel SDM's ${snx_sdm_why}")
    else()
        set(SNX_X64 ON CACHE INTERNAL "the x86-64 back end is built (SNX_SAIL_X64 and the manual found)")
        set(SNX_SDM_PDF_FOUND "${pdf}" CACHE INTERNAL "the Intel SDM's PDF the build derives from")
    endif()
endif()

# snx_sdm_isel(<target>): generates x64_isel.inc (the encoders of the selected forms and each primitive's candidates)
# and x64_isel.report (what was selected, and the forms the selection couldn't read, with why) into the build folder's
# sdm/, and gives <target> its include root. A folder's rules are made once: another target of the folder (or the
# same one, for A64's and AArch32's emitters) takes the same files
function(snx_sdm_isel target)
    set(pdf "${SNX_SDM_PDF_FOUND}")
    set(dir "${PROJECT_SOURCE_DIR}/tools/sailgen")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/sdm")
    get_property(made DIRECTORY PROPERTY SNX_SDM_ISEL_MADE)
    if(made)
        get_target_property(sources ${target} SOURCES)
        if(NOT "${out}/x64_isel.inc" IN_LIST sources)
            target_sources(${target} PRIVATE "${out}/x64_isel.inc")
            target_include_directories(${target} PRIVATE "${out}")
        endif()
        return()
    endif()
    set_property(DIRECTORY PROPERTY SNX_SDM_ISEL_MADE ON)
    file(GLOB overrides CONFIGURE_DEPENDS "${dir}/sdm/*.sdm")
    set(overlay_args)
    foreach(f ${overrides})
        list(APPEND overlay_args --overlay "${f}")
    endforeach()
    add_custom_command(OUTPUT "${out}/sdm.txt"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
        COMMAND "${SNX_PDFTOTEXT}" -layout "${pdf}" "${out}/sdm.txt"
        DEPENDS "${pdf}" COMMENT "pdftotext: the Intel SDM Vol. 2" VERBATIM)
    add_custom_command(OUTPUT "${out}/sdm.pages"
        COMMAND snx-sailgen --sdm "${out}/sdm.txt" -o "${out}/sdm.pages" ${overlay_args}
        DEPENDS snx-sailgen "${out}/sdm.txt" ${overrides} COMMENT "The SDM's instruction pages" VERBATIM)
    add_custom_command(OUTPUT "${out}/x64_isel.inc" "${out}/x64_isel.report"
        COMMAND snx-sailgen --isel "${out}/sdm.pages" --ops "${dir}/x64.ops" -o "${out}/x64_isel.inc"
                --report "${out}/x64_isel.report"
        DEPENDS snx-sailgen "${out}/sdm.pages" "${dir}/x64.ops"
        COMMENT "Deriving the x86-64 instruction selection from the SDM" VERBATIM)
    target_sources(${target} PRIVATE "${out}/x64_isel.inc")
    target_include_directories(${target} PRIVATE "${out}")
endfunction()

# snx_sailgen_a64_x64(<target>): generates a64_x64.inc (the A64 emitters of a64_x64.roots, with sailgen's x86-64 back
# end at its top: snx_sailgen_machine_backends' files) into the build folder, with the instruction selection it
# includes (snx_sdm_isel), and gives <target> its include root
function(snx_sailgen_a64_x64 target)
    snx_sdm_isel(${target})
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
    target_include_directories(${target} PRIVATE "${out}")
endfunction()

# snx_sailgen_a32_x64(<target>): generates a32_x64.inc (the AArch32 emitters of a32.roots, LLVM's own, with sailgen's
# x86-64 back end at its top: snx_sailgen_machine_backends' files) into the build folder, with the instruction
# selection it includes (snx_sdm_isel), and gives <target> its include root
function(snx_sailgen_a32_x64 target)
    snx_sdm_isel(${target})
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
    target_include_directories(${target} PRIVATE "${out}")
endfunction()
