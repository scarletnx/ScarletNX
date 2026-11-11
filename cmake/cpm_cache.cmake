# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
#   snx_cpm_cache_sweep()
#
# Removes the empty folders that failed downloads left in CPM_SOURCE_CACHE. CPM.cmake takes a package's folder there
# for the package as soon as it exists, and a download makes the folder before it fetches anything: after one that
# failed (a 504, a connection cut, other bytes than the pinned ones) every later configure took the empty folder and
# downloaded nothing. Each is removed under the lock CPM holds while it fills a folder, the cmake.lock beside it, so
# never while another configure downloads into it. Called before the first package is added, which downloads it again.
function(snx_cpm_cache_sweep)
    if(NOT CPM_SOURCE_CACHE)
        return()
    endif()
    # where CPM puts a package: <package>/<hash>, or <package>/<hash>/<name> with CPM_USE_NAMED_CACHE_DIRECTORIES
    set(depth "*/*")
    if(CPM_USE_NAMED_CACHE_DIRECTORIES)
        set(depth "*/*/*")
    endif()
    file(GLOB folders LIST_DIRECTORIES TRUE "${CPM_SOURCE_CACHE}/${depth}")
    foreach(folder IN LISTS folders)
        if(NOT IS_DIRECTORY "${folder}")
            continue()
        endif()
        file(GLOB inside LIST_DIRECTORIES TRUE "${folder}/*")
        if(inside)
            continue()
        endif()
        get_filename_component(parent "${folder}" DIRECTORY)
        file(LOCK "${parent}/cmake.lock")
        # looked at again with the lock held: a download that was filling it has ended by now
        file(GLOB inside LIST_DIRECTORIES TRUE "${folder}/*")
        if(IS_DIRECTORY "${folder}" AND NOT inside)
            file(REMOVE_RECURSE "${folder}")
            message(STATUS "CPM cache: ${folder} was empty, left by a download that failed. It is removed, and its "
                           "package is downloaded again.")
        endif()
        file(LOCK "${parent}/cmake.lock" RELEASE)
    endforeach()
endfunction()
