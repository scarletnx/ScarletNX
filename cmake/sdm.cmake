# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The Intel SDM, Volume 2 (2A-2D, order number 325383, revision 093US), which the x86-64 back end's instruction
# selection is derived from as the build runs (tools/sailgen/README.md, "The x86-64 back end"): pdftotext's text of
# the PDF, cut into its instruction pages by sailgen --sdm with the errata's overrides (tools/sailgen/sdm/), then
# sailgen --isel matches the back end's primitives (tools/sailgen/x64.ops) with the pages' pseudocode. The manual is not
# kept in the tree: pass its PDF with -DSNX_SDM_PDF=, or it is downloaded once into the build tree; either way its
# SHA-256 must be 093US's, so every build derives from the same text. Off unless SNX_SAIL_X64 is set
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/sailgen.cmake")

option(SNX_SAIL_X64 "Derive the x86-64 back end's instruction selection from the Intel SDM (needs pdftotext)" OFF)
set(SNX_SDM_PDF "" CACHE FILEPATH "Intel SDM Vol. 2 PDF, order no. 325383-093US (downloaded if empty)")
set(snx_sdm_url "https://cdrdv2.intel.com/v1/dl/getContent/671110")
set(snx_sdm_sha256 d137a7887787bbb06c1a8b8bafd51958d8d2e16b4dff987a03226c845cca1516)

# snx_sdm_isel(<target>): generates x64_isel.inc (the encoders of the selected forms and each primitive's candidates)
# and x64_isel.report (what was selected, and the forms the selection couldn't read, with why) into the build folder,
# and gives <target> its include root
function(snx_sdm_isel target)
    set(pdf "${SNX_SDM_PDF}")
    if(NOT pdf)
        set(pdf "${PROJECT_BINARY_DIR}/sdm/325383-093US.pdf")
        if(NOT EXISTS "${pdf}")
            message(STATUS "Downloading the Intel SDM Vol. 2 (325383-093US)")
            file(DOWNLOAD "${snx_sdm_url}" "${pdf}" STATUS st TLS_VERIFY ON)
            list(GET st 0 rc)
            if(rc)
                file(REMOVE "${pdf}")
                message(FATAL_ERROR "SDM download failed (${st}); pass -DSNX_SDM_PDF=<325383-093US.pdf>")
            endif()
        endif()
    endif()
    file(SHA256 "${pdf}" got)
    if(NOT got STREQUAL snx_sdm_sha256)
        message(FATAL_ERROR "${pdf} isn't SDM Vol. 2 revision 325383-093US (SHA-256 ${got}); Intel's download link "
                            "serves the latest revision: pass the 093US PDF with -DSNX_SDM_PDF=")
    endif()
    find_program(SNX_PDFTOTEXT pdftotext REQUIRED)
    set(dir "${PROJECT_SOURCE_DIR}/tools/sailgen")
    file(GLOB overrides CONFIGURE_DEPENDS "${dir}/sdm/*.sdm")
    set(overlay_args)
    foreach(f ${overrides})
        list(APPEND overlay_args --overlay "${f}")
    endforeach()
    set(out "${CMAKE_CURRENT_BINARY_DIR}/sailgen")
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
