# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# An access to an _Atomic object names its order (atomic_*_explicit): a plain read, write or ++ of one compiles without a
# word and is sequentially consistent, the costliest order (a locked instruction on x86, ldar/stlr on AArch64). Clang's
# -Watomic-implicit-seq-cst finds those, and every C source of this project's src/ builds with it as an error, whatever
# target compiles it. Tests keep C's plain accesses, and third-party packages (CPMUtil's, under .cache/cpm or the build
# tree) their own flags.
include_guard(GLOBAL)

function(snx_atomic_explicit_dir dir)
    get_directory_property(targets DIRECTORY "${dir}" BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if(NOT type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
            continue()
        endif()
        get_target_property(sources ${target} SOURCES)
        get_target_property(target_dir ${target} SOURCE_DIR)
        foreach(source IN LISTS sources)
            if(source MATCHES "^\\$<") # a generator expression's sources have no properties of their own here
                continue()
            endif()
            cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${target_dir}" NORMALIZE)
            cmake_path(IS_PREFIX PROJECT_SOURCE_DIR "${source}" NORMALIZE ours)
            if(ours AND source MATCHES "^${PROJECT_SOURCE_DIR}/src/" AND NOT source MATCHES "/(\\.cache|_deps)/")
                set_property(SOURCE "${source}" TARGET_DIRECTORY ${target} APPEND PROPERTY COMPILE_OPTIONS
                             "$<$<AND:$<COMPILE_LANGUAGE:C>,$<C_COMPILER_ID:Clang,AppleClang>>:-Werror=atomic-implicit-seq-cst>")
            endif()
        endforeach()
    endforeach()
    get_directory_property(subdirs DIRECTORY "${dir}" SUBDIRECTORIES)
    foreach(subdir IN LISTS subdirs)
        cmake_path(IS_PREFIX PROJECT_SOURCE_DIR "${subdir}" NORMALIZE ours)
        cmake_path(IS_PREFIX PROJECT_BINARY_DIR "${subdir}" NORMALIZE generated)
        if(ours AND NOT generated AND NOT subdir MATCHES "/\\.cache/")
            snx_atomic_explicit_dir("${subdir}")
        endif()
    endforeach()
endfunction()

# Called at the end of the top-level CMakeLists.txt, once every target is defined
function(snx_atomic_explicit)
    snx_atomic_explicit_dir("${PROJECT_SOURCE_DIR}")
endfunction()
