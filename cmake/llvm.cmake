# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# LLVM's C API is required by the recompiler on every platform. SNX_LLVM_CONFIG selects an llvm-config that runs
# on the build machine and describes the LLVM development files for the target, including when cross-compiling.
include("${CMAKE_CURRENT_LIST_DIR}/llvm_components.cmake")
# scarletnx --mcp's A64 disassembler (src/nx/runtime/driver/disasm.c), where LLVM has the AArch64 target. A release
# leaves it out: it is for working on the runtime, and a static LLVM then links no AArch64 code into a host of another
option(SNX_A64_DISASM "Build scarletnx --mcp's A64 disassembler, where LLVM has the AArch64 target" ON)
# LLVM 22's -O2 makes a lane of A32's VRSHL and VQSHL wrong on a host with AVX-512 (#2914): its x86 machine scheduler, on
# the 128-bit lanes its vectorizer makes of them. 19, 21 and 23.1.6 make them right. The configure stops on it, but with
# this option, and a build that takes it says so in its log when LLVM starts (src/common/cpu/recompiler/llvm/ir.c)
option(SNX_ACCEPT_LLVM22_MISCOMPILE
       "Build with LLVM 22, whose -O2 makes A32's VRSHL and VQSHL wrong on an AVX-512 host (#2914)" OFF)
function(snx_llvm_refuse_miscompile)
    string(REGEX MATCH "^[0-9]+" major "${SNX_LLVM_VERSION}")
    if(NOT major STREQUAL "22")
        return()
    endif()
    set(text "LLVM ${SNX_LLVM_VERSION} makes wrong code: at -O2, on a host with AVX-512, a lane of A32's VRSHL and "
             "VQSHL comes out wrong in a 32-bit program's blocks (#2914). LLVM 23.1.6 makes them right: point "
             "SNX_LLVM_CONFIG or LLVM_DIR at it")
    if(NOT SNX_ACCEPT_LLVM22_MISCOMPILE)
        message(FATAL_ERROR ${text} ", or configure with -DSNX_ACCEPT_LLVM22_MISCOMPILE=ON to build with LLVM "
                            "${SNX_LLVM_VERSION} and this defect.")
    endif()
    get_property(said GLOBAL PROPERTY SNX_LLVM_MISCOMPILE_SAID)
    if(NOT said) # llvm.cmake is read once in each folder that needs LLVM
        set_property(GLOBAL PROPERTY SNX_LLVM_MISCOMPILE_SAID ON)
        message(WARNING "SNX_ACCEPT_LLVM22_MISCOMPILE=ON: " ${text} ". The recompiler says so in its log.")
    endif()
endfunction()
# a folder of the sysroot as the program sees it on the target: a cross build's LLVM folder is under CMAKE_SYSROOT,
# which a run path leaves out (src/3ds, src/vita)
function(snx_target_path output path)
    string(LENGTH "${CMAKE_SYSROOT}" length)
    string(LENGTH "${path}" path_length)
    if(length AND path_length GREATER length)
        string(SUBSTRING "${path}" 0 ${length} start)
        string(SUBSTRING "${path}" ${length} 1 next)
        if(start STREQUAL CMAKE_SYSROOT AND next STREQUAL "/")
            string(SUBSTRING "${path}" ${length} -1 path)
        endif()
    endif()
    set(${output} "${path}" PARENT_SCOPE)
endfunction()
if(ANDROID OR IOS OR (LLVM_DIR AND NOT SNX_LLVM_CONFIG))
    # Android's and iOS's LLVM (scripts/android-llvm.sh, scripts/apple-llvm.sh) have an llvm-config that only runs on the
    # device, and the Mac's from scripts/apple-llvm.sh and Linux's from scripts/linux-llvm.sh none: their CMake package
    # instead, named by LLVM_DIR, as pkgsrc's in a NetBSD cross build (docs/hosts/netbsd.md), whose llvm-config runs on NetBSD
    find_package(LLVM REQUIRED CONFIG)
    if(LLVM_LINK_LLVM_DYLIB)
        # a package whose own tools link its one shared library (pkgsrc's, a distribution's) built it with every target,
        # which LLVMInitializeAllTargets (the tests' elf_relocate, llvm_lowering) needs: the program links it, as the
        # llvm-config route links a distribution's
        set(SNX_LLVM_LIBS LLVM)
        set(SNX_LLVM_DISASM_LIBS "")
        set(SNX_LLVM_A64_DISASM ${SNX_A64_DISASM})
        if(NOT "AArch64" IN_LIST LLVM_TARGETS_TO_BUILD)
            set(SNX_LLVM_A64_DISASM OFF)
        endif()
        set(SNX_LLVM_SHARED ON)
    else()
        llvm_map_components_to_libnames(SNX_LLVM_LIBS ${SNX_LLVM_COMPONENTS})
        if(SNX_A64_DISASM AND "AArch64" IN_LIST LLVM_TARGETS_TO_BUILD AND TARGET LLVMAArch64Disassembler)
            llvm_map_components_to_libnames(SNX_LLVM_DISASM_LIBS aarch64disassembler aarch64desc aarch64info)
            set(SNX_LLVM_A64_DISASM ON)
        endif()
        set(SNX_LLVM_SHARED OFF) # their libraries, each static
    endif()
    set(SNX_LLVM_VERSION "${LLVM_PACKAGE_VERSION}")
    set(SNX_LLVM_INCLUDE_DIR "${LLVM_INCLUDE_DIRS}")
    set(SNX_LLVM_LIBRARY_DIR "${LLVM_LIBRARY_DIRS}")
    set(SNX_LLVM_SYSTEM_LIBS "")
    message(STATUS "LLVM backend: LLVM ${SNX_LLVM_VERSION} (${LLVM_DIR})")
    snx_llvm_refuse_miscompile()
    return()
endif()
set(snx_llvm_names llvm-config)
foreach(version RANGE 30 17 -1)
    list(APPEND snx_llvm_names llvm-config-${version} llvm-config${version})
endforeach()
find_program(SNX_LLVM_CONFIG NAMES ${snx_llvm_names} DOC "llvm-config of the LLVM the LLVM backend links")
if(NOT SNX_LLVM_CONFIG)
    message(FATAL_ERROR "LLVM's development files are required by the recompiler on every platform. Install them "
                        "or point SNX_LLVM_CONFIG at an llvm-config for the target LLVM installation.")
endif()

function(snx_llvm_query output)
    execute_process(COMMAND "${SNX_LLVM_CONFIG}" ${ARGN} OUTPUT_VARIABLE value
        OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE result ERROR_VARIABLE error)
    if(NOT "${result}" STREQUAL "0")
        message(FATAL_ERROR "LLVM is required, but ${SNX_LLVM_CONFIG} ${ARGN} failed (${result}): ${error}")
    endif()
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

snx_llvm_query(SNX_LLVM_VERSION --version)
snx_llvm_query(SNX_LLVM_INCLUDE_DIR --includedir)
snx_llvm_query(SNX_LLVM_LIBRARY_DIR --libdir)
snx_llvm_query(SNX_LLVM_LIBS --libs ${SNX_LLVM_COMPONENTS})
snx_llvm_query(SNX_LLVM_SYSTEM_LIBS --system-libs ${SNX_LLVM_COMPONENTS})
if(NOT SNX_LLVM_VERSION OR NOT EXISTS "${SNX_LLVM_INCLUDE_DIR}/llvm-c/Core.h" OR
   NOT IS_DIRECTORY "${SNX_LLVM_LIBRARY_DIR}" OR NOT SNX_LLVM_LIBS)
    message(FATAL_ERROR "LLVM's development files are required, but ${SNX_LLVM_CONFIG} reports an incomplete "
                        "installation. Check its headers and libraries or select another SNX_LLVM_CONFIG.")
endif()
separate_arguments(SNX_LLVM_LIBS UNIX_COMMAND "${SNX_LLVM_LIBS}")
# a distribution's libLLVM, whose own dependencies the program's search path has to find (src/3ds)
execute_process(COMMAND "${SNX_LLVM_CONFIG}" --shared-mode OUTPUT_VARIABLE snx_llvm_mode OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(snx_llvm_mode STREQUAL "shared")
    set(SNX_LLVM_SHARED ON)
else()
    set(SNX_LLVM_SHARED OFF)
endif()
separate_arguments(SNX_LLVM_SYSTEM_LIBS UNIX_COMMAND "${SNX_LLVM_SYSTEM_LIBS}")

function(snx_llvm_windows_libraries output)
    # With -static, MinGW's linker will not find libLLVM-23.dll.a through
    # -lLLVM-23. Pass the exact import library instead, without changing the
    # linkage of the toolchain or SDL. llvm-config --libfiles is not a substitute:
    # on Windows it reports the runtime DLL in bin/, not its import library.
    set(CMAKE_FIND_LIBRARY_PREFIXES "lib" "")
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".dll.a" ".a" ".lib")
    set(libraries "")
    foreach(library IN LISTS ARGN)
        if(library MATCHES "^-l(.+)$")
            set(name "${CMAKE_MATCH_1}")
            set(resolved "resolved-NOTFOUND")
            find_library(resolved NAMES "${name}" PATHS "${SNX_LLVM_LIBRARY_DIR}"
                         NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH NO_CACHE)
            if(NOT resolved)
                message(FATAL_ERROR "LLVM is required, but ${library} from ${SNX_LLVM_CONFIG} has no library in "
                                    "${SNX_LLVM_LIBRARY_DIR}. Install the target LLVM development libraries.")
            endif()
            list(APPEND libraries "${resolved}")
        else()
            list(APPEND libraries "${library}")
        endif()
    endforeach()
    set(${output} "${libraries}" PARENT_SCOPE)
endfunction()
snx_llvm_query(snx_llvm_targets --targets-built)
if(SNX_A64_DISASM AND snx_llvm_targets MATCHES "(^| )AArch64( |$)")
    snx_llvm_query(SNX_LLVM_DISASM_LIBS --libs aarch64disassembler aarch64desc aarch64info)
    separate_arguments(SNX_LLVM_DISASM_LIBS UNIX_COMMAND "${SNX_LLVM_DISASM_LIBS}")
    set(SNX_LLVM_A64_DISASM ON)
endif()
if(WIN32)
    snx_llvm_windows_libraries(SNX_LLVM_LIBS ${SNX_LLVM_LIBS})
    if(SNX_LLVM_A64_DISASM)
        snx_llvm_windows_libraries(SNX_LLVM_DISASM_LIBS ${SNX_LLVM_DISASM_LIBS})
    endif()
endif()
message(STATUS "LLVM backend: LLVM ${SNX_LLVM_VERSION} (${SNX_LLVM_CONFIG})")
snx_llvm_refuse_miscompile()
