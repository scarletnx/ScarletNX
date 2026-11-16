# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
#   snx_patched_copy(SOURCE <dir> DEST <dir> PATCHES <file>... FILES <name> <sha256>...)
#
# DEST: a copy of SOURCE with PATCHES applied by patch -p1, as CPMUtil's patches are applied. A package with patches is
# kept in CPM_SOURCE_CACHE as released, so a folder given with <package>_CUSTOM_DIR (a Flatpak manifest's sources, a
# distribution's offline build) works too. patch(1) applies a hunk whose context moved or changed without an error, so
# each file the patches touch is in FILES with the SHA-256 it has patched; one that has another makes the copy again (a
# patch edited since, a copy left half patched) and, after that, stops the configure.
#
# The one way a package is patched here: CPMUtil's patches leave a <package>_CUSTOM_DIR unpatched. FFmpeg uses it, for
# NVDEC's VP9 jobs, and FidelityFX-FSR2 for its device memory. hactool, a fork of ours since #166, is in the tree
# (src/nx/runtime/driver/hactool).
function(snx_patched_copy)
    cmake_parse_arguments(arg "" "SOURCE;DEST" "PATCHES;FILES" ${ARGN})
    set(stale FALSE)
    set(pairs ${arg_FILES})
    while(pairs)
        list(POP_FRONT pairs name hash)
        set(have "")
        if(EXISTS "${arg_DEST}/${name}")
            file(SHA256 "${arg_DEST}/${name}" have)
        endif()
        if(NOT have STREQUAL hash)
            set(stale TRUE)
        endif()
    endwhile()
    if(NOT stale)
        return()
    endif()
    # CPMUtil's search (cpm_find_program): on PATH, or the one Git for Windows carries
    find_package(Git QUIET)
    if(GIT_EXECUTABLE)
        get_filename_component(git_dir "${GIT_EXECUTABLE}" DIRECTORY)
    endif()
    find_program(PATCH_EXECUTABLE patch HINTS "${git_dir}/../usr/bin" "${git_dir}/../../usr/bin")
    if(NOT PATCH_EXECUTABLE)
        message(FATAL_ERROR "patch(1) is needed to patch ${arg_SOURCE}: install it, or Git for Windows")
    endif()
    # BusyBox's patch (the one w64devkit carries, so bootstrap.ps1 stages it) has no -F and takes no fuzz anyway
    execute_process(COMMAND "${PATCH_EXECUTABLE}" --help OUTPUT_VARIABLE patch_help ERROR_VARIABLE patch_help)
    if(patch_help MATCHES "BusyBox")
        set(patch_fuzz)
    else()
        set(patch_fuzz -F0)
    endif()
    file(REMOVE_RECURSE "${arg_DEST}")
    file(COPY "${arg_SOURCE}/" DESTINATION "${arg_DEST}")
    foreach(patch IN LISTS arg_PATCHES)
        # -F0: with its default fuzz, FreeBSD's patch applies a hunk whose removed lines differ
        execute_process(COMMAND "${PATCH_EXECUTABLE}" -p1 ${patch_fuzz} -i "${patch}" WORKING_DIRECTORY "${arg_DEST}"
                        RESULT_VARIABLE failed OUTPUT_VARIABLE said ERROR_VARIABLE said)
        if(failed)
            message(FATAL_ERROR "${patch} does not apply to ${arg_SOURCE}:\n${said}")
        endif()
    endforeach()
    set(pairs ${arg_FILES})
    while(pairs)
        list(POP_FRONT pairs name hash)
        file(SHA256 "${arg_DEST}/${name}" have)
        if(NOT have STREQUAL hash)
            message(FATAL_ERROR "${arg_DEST}/${name} (SHA-256 ${have}) is not what the patches make of ${arg_SOURCE}. "
                                "After a bump of it or a patch edit, check the patched files and update their hashes")
        endif()
    endwhile()
endfunction()
