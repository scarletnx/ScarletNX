# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The recompiler, a static library for each layer, each over the ones before it:
#   snx_recompiler_base   the A64 decoder, the function discovery and the host calls (src/common/CMakeLists.txt)
#   snx_recompiler_core   the native back ends (mach/ and a folder a machine), what links and loads their objects
#                         (object/, loader/) and the runtime library's objects (rtlib/). No LLVM: the tests
#                         runtime_architecture and recompiler_core_symbols hold it to that
#   snx_llvm              the LLVM backend (llvm/), where the build has it (SNX_WITH_LLVM), and the fallback it is
#                         (fallback.h): it makes what the native back ends leave. snx_fallback_none refuses it instead
#   snx_recompiler_jit    what makes the code a running program reaches (jit/, cpu/arm/exec.c), and the AArch32 blocks
#                         of the AOT compilers (cpu/arm/aot.c)
#   scarletnx-recompiler  the library (snx_recompiler.h, docs/dev/library.md): an archive of all their objects
# A build without LLVM has the core, snx_fallback_none, the AArch32 executor and AOT blocks and the JIT cache's reader
# alone: the others still call it

# The runtime library every module links (rtlib.c): the helpers, compiled here with this Clang and the flags toolchain.c
# gives generated C
set(snx_cpu "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64")
add_subdirectory("${PROJECT_SOURCE_DIR}/src/common/cpu/arm" arm-backend)
file(GLOB snx_rtlib_sources CONFIGURE_DEPENDS "${snx_cpu}/*.h" "${snx_cpu}/*.c"
    "${snx_cpu}/helpers/*.c" "${snx_cpu}/helpers/*.h" "${PROJECT_SOURCE_DIR}/src/common/runtime/interface.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/x64/x64_helpers.c" "${PROJECT_SOURCE_DIR}/src/common/utilities/bits.h")
if(CMAKE_CROSSCOMPILING)
    # it runs during the build: a cross build makes it for the machine doing that, with that machine's compiler
    find_program(SNX_HOST_CC NAMES clang cc REQUIRED)
    set(snx_rtlib_gen "${CMAKE_CURRENT_BINARY_DIR}/snx-rtlib-gen-host")
    add_custom_command(OUTPUT "${snx_rtlib_gen}"
        COMMAND "${SNX_HOST_CC}" -std=gnu23 -O1 -w -I "${PROJECT_SOURCE_DIR}/src"
            "${CMAKE_CURRENT_SOURCE_DIR}/rtlib/rtlib_gen.c" -o "${snx_rtlib_gen}"
        DEPENDS rtlib/rtlib_gen.c VERBATIM)
else()
    add_executable(snx-rtlib-gen rtlib/rtlib_gen.c)
    set(snx_rtlib_gen snx-rtlib-gen)
endif()
# No -march=native: the recompiler builds modules for the machine it runs on, which is often not the one that built
# it (the Windows zip and the AppImage come from CI runners). The LLVM backend compiles the library's bitcode
# (llvm/CMakeLists.txt) for each module with the module's target machine, the host CPU as for the generated code; the
# object below is the one the runtime JIT loads (jit_service.c). A CI runner's -march=native library used AVX-512, an
# illegal instruction on a Ryzen 3000 the first time a module called a helper using it. Android's is the phone's ABI
# baseline, which Clang targets by default
if(ANDROID)
    set(snx_rtlib_flags --target=${CMAKE_C_COMPILER_TARGET} --sysroot=${CMAKE_SYSROOT})
elseif(APPLE) # the architecture, SDK and oldest system version (cmake/apple.cmake): iOS's are not this Mac's
    # Apple's headers fortify an optimized compile on their own, which makes memcpy, memmove and memset macros for the
    # __*_chk functions: libc.inc's definitions would not parse, and a module links no C library to have those
    set(snx_rtlib_flags ${SNX_APPLE_FLAGS} -D_FORTIFY_SOURCE=0)
else() # a Linux cross build's target and sysroot, and the CPU baseline (cmake/cross.cmake)
    set(snx_rtlib_flags ${SNX_TARGET_FLAGS})
endif()
list(APPEND snx_rtlib_flags -std=gnu23 -O2 -fvisibility=hidden -w -fno-semantic-interposition
    -foptimize-sibling-calls -fno-stack-protector)
# its own flags, which no target carries (cmake/banned_flags.cmake checks those)
include("${PROJECT_SOURCE_DIR}/cmake/banned_flags.cmake")
if(SNX_TRAPV)
    list(APPEND snx_rtlib_flags ${SNX_TRAPV_FLAGS})
endif()
snx_check_banned_flags("The runtime library's flags (snx_rtlib_flags)" "${snx_rtlib_flags}")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    list(APPEND snx_rtlib_flags -mcx16)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    # atomics inline: Clang would call libgcc's or compiler-rt's, and the module links neither
    list(APPEND snx_rtlib_flags -mno-outline-atomics)
endif()
if(WIN32)
    # no __chkstk, which the module doesn't link: the library's own frames stay under a page (its snx_stack_probe, and
    # on AArch64 its __chkstk, are for the generated functions' larger ones)
    list(APPEND snx_rtlib_flags -mno-stack-arg-probe)
else()
    list(APPEND snx_rtlib_flags -fPIC)
endif()
# The object is the JIT loader's (code_loader.c), which reads ELF: on macOS and iOS too, where the triple's object format
# says so and the code stays Darwin's (ir.c's host_triple). The bitcode keeps the Mach-O triple of the AOT modules
set(snx_rtlib_object_flags "")
set(snx_rtlib_object_base ${snx_rtlib_flags})
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    if(IOS)
        # the triple carries the version: Clang refuses -miphoneos-version-min (SNX_APPLE_FLAGS') beside an -elf one
        list(FILTER snx_rtlib_object_base EXCLUDE REGEX "^-m(iphoneos|ios-simulator)-version-min=")
        if(SNX_IOS_SIMULATOR) # the simulator's loader runs it, as MAP_JIT memory like the Mac's
            set(snx_rtlib_object_flags --target=arm64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}-simulator-elf)
        else()
            set(snx_rtlib_object_flags --target=arm64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}-elf)
        endif()
    else()
        set(snx_rtlib_object_flags --target=arm64-apple-macos${CMAKE_OSX_DEPLOYMENT_TARGET}-elf)
    endif()
endif()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/rtlib${CMAKE_C_OUTPUT_EXTENSION}"
    COMMAND "${CMAKE_C_COMPILER}" ${snx_rtlib_object_base} ${snx_rtlib_object_flags} -I "${CMAKE_CURRENT_BINARY_DIR}"
        -I "${PROJECT_SOURCE_DIR}/src" -c "${CMAKE_CURRENT_SOURCE_DIR}/rtlib/rtlib.c"
        -o "${CMAKE_CURRENT_BINARY_DIR}/rtlib${CMAKE_C_OUTPUT_EXTENSION}"
    DEPENDS rtlib/rtlib.c rtlib/libc.inc ${snx_rtlib_sources} VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/rtlib_object.c"
    COMMAND "${snx_rtlib_gen}" embed "${CMAKE_CURRENT_BINARY_DIR}/rtlib${CMAKE_C_OUTPUT_EXTENSION}"
        "${CMAKE_CURRENT_BINARY_DIR}/rtlib_object.c" snx_rtlib_object
    DEPENDS "${snx_rtlib_gen}" "${CMAKE_CURRENT_BINARY_DIR}/rtlib${CMAKE_C_OUTPUT_EXTENSION}" VERBATIM)
# armlib.c: what a 32-bit module links instead (the AArch32 blocks' VFP, SIMD and crypto helpers)
file(GLOB snx_armlib_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/*.h"
    "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/*_helpers.c")
# in the host's object format, the modules' (Mach-O on Apple, which rtlib's object is not): what a 32-bit module links.
# The AArch32 JIT loads it on powerpc64 alone (cpu/arm/exec.c)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/armlib${CMAKE_C_OUTPUT_EXTENSION}"
    COMMAND "${CMAKE_C_COMPILER}" ${snx_rtlib_flags} -I "${PROJECT_SOURCE_DIR}/src"
        -c "${CMAKE_CURRENT_SOURCE_DIR}/rtlib/armlib.c"
        -o "${CMAKE_CURRENT_BINARY_DIR}/armlib${CMAKE_C_OUTPUT_EXTENSION}"
    DEPENDS rtlib/armlib.c rtlib/libc.inc ${snx_armlib_sources} ${snx_rtlib_sources} VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/armlib_object.c"
    COMMAND "${snx_rtlib_gen}" embed "${CMAKE_CURRENT_BINARY_DIR}/armlib${CMAKE_C_OUTPUT_EXTENSION}"
        "${CMAKE_CURRENT_BINARY_DIR}/armlib_object.c" snx_armlib_object
    DEPENDS "${snx_rtlib_gen}" "${CMAKE_CURRENT_BINARY_DIR}/armlib${CMAKE_C_OUTPUT_EXTENSION}" VERBATIM)

add_library(snx_recompiler_core STATIC object/link.c object/link_elf.c object/link_coff.c object/link_macho.c
    object/elf_object.c object/objects_pack.c loader/code_loader.c loader/code_loader_coff.c loader/gdb_jit.c
    "${CMAKE_CURRENT_BINARY_DIR}/rtlib_object.c" "${CMAKE_CURRENT_BINARY_DIR}/armlib_object.c")
set_target_properties(snx_recompiler_core PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
# The object loader in strict ISO C23 (RFC #142), after the target's -std
set_source_files_properties(object/elf_object.c loader/code_loader.c loader/code_loader_coff.c loader/gdb_jit.c
                            PROPERTIES COMPILE_OPTIONS "-std=c23;-pedantic-errors")
target_compile_options(snx_recompiler_core PRIVATE -Wall -Wextra -Werror=missing-prototypes)
target_compile_definitions(snx_recompiler_core PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(snx_recompiler_core PUBLIC snx_recompiler_base snx_arm_decode Threads::Threads)
# the x86-64 back end (src/common/cpu/recompiler/x64): its functions' code without LLVM, from the A64 emitters sailgen
# generates with the instruction selection derived from the Intel SDM that the tree has (tools/sailgen/sdm/x64_isel.inc,
# cmake/sdm.cmake); the default code generator where it is built, LLVM making what it can't
target_sources(snx_recompiler_core PRIVATE x64/x64_codegen.c x64/x64_cache.c x64/x64_elf.c x64/x64_arm.c)
include("${PROJECT_SOURCE_DIR}/cmake/sdm.cmake")
if(SNX_X64)
    snx_sailgen_a64_x64(snx_recompiler_core)
    # and the AArch32 blocks' (x64_arm.c), from the AArch32 Sail's emitters
    snx_sailgen_a32_x64(snx_recompiler_core)
    target_compile_definitions(snx_recompiler_core PRIVATE SNX_HAVE_X64=1)
endif()
# the arm64 back end (src/common/cpu/recompiler/arm64): the same functions in A64, from the emitters sailgen
# generates with the A64 Sail's encoders; built on every host (its code is arm64's, which the tests run under
# Unicorn; not on Windows on another machine), the default code generator on arm64's ELF hosts and Windows
target_sources(snx_recompiler_core PRIVATE arm64/arm64_codegen.c arm64/arm64_arm.c mach/native.c)
snx_sailgen_a64_arm64(snx_recompiler_core)
snx_sailgen_a32_arm64(snx_recompiler_core)
# the riscv64 back end (src/common/cpu/recompiler/rv64): the same functions in RV64, from the emitters sailgen
# generates with its RISC layer over LLVM's RISC-V records (checked in: tools/sailgen/td); built on every host
target_sources(snx_recompiler_core PRIVATE rv64/rv64_codegen.c rv64/rv64_arm.c)
snx_sailgen_a64_risc(snx_recompiler_core rv64)
snx_sailgen_a32_risc(snx_recompiler_core rv64)
# and the powerpc64 back end (src/common/cpu/recompiler/ppc64), over LLVM's PowerPC records
target_sources(snx_recompiler_core PRIVATE ppc64/ppc64_codegen.c ppc64/ppc64_arm.c)
snx_sailgen_a64_risc(snx_recompiler_core ppc64)
snx_sailgen_a32_risc(snx_recompiler_core ppc64)
# and the mips64 back end (src/common/cpu/recompiler/mips64), over LLVM's MIPS records
target_sources(snx_recompiler_core PRIVATE mips64/mips64_codegen.c mips64/mips64_arm.c)
snx_sailgen_a64_risc(snx_recompiler_core mips64)
snx_sailgen_a32_risc(snx_recompiler_core mips64)
# the big-endian hosts' powerpc64 and mips64 back ends: the same sources with PPC_BE and MIPS_BE (their emitters' too)
target_sources(snx_recompiler_core PRIVATE ppc64/ppc64be_codegen.c ppc64/ppc64be_arm.c mips64/mips64be_codegen.c
               mips64/mips64be_arm.c)
# and the s390x back end (src/common/cpu/recompiler/s390x), over LLVM's SystemZ records
target_sources(snx_recompiler_core PRIVATE s390x/s390x_codegen.c s390x/s390x_arm.c)
snx_sailgen_a64_risc(snx_recompiler_core s390x)
snx_sailgen_a32_risc(snx_recompiler_core s390x)
# and the loongarch64 back end (src/common/cpu/recompiler/la64), over LLVM's LoongArch records
target_sources(snx_recompiler_core PRIVATE la64/la64_codegen.c la64/la64_arm.c)
snx_sailgen_a64_risc(snx_recompiler_core la64)
snx_sailgen_a32_risc(snx_recompiler_core la64)

# no fallback: each thing asked of it is an error that names it. The tests link it in every build
add_library(snx_fallback_none STATIC EXCLUDE_FROM_ALL fallback_none.c)
set_target_properties(snx_fallback_none PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx_fallback_none PRIVATE -pedantic-errors -Wall -Wextra -Werror=missing-prototypes)
target_link_libraries(snx_fallback_none PUBLIC snx_recompiler_core)

add_library(snx_recompiler_jit STATIC "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/exec.c" jit/jit_cache.c
                                      "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/aot.c")
set_target_properties(snx_recompiler_jit PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
target_compile_options(snx_recompiler_jit PRIVATE -Wall -Wextra -Werror=missing-prototypes)
target_compile_definitions(snx_recompiler_jit PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(snx_recompiler_jit PUBLIC snx_recompiler_core snx_arm_rt)
if(NOT SNX_WITH_LLVM)
    target_link_libraries(snx_recompiler_jit PUBLIC snx_fallback_none)
    return()
endif()
add_subdirectory(llvm)
# these call LLVM themselves
target_sources(snx_recompiler_jit PRIVATE jit/jit_service.c jit/jit_cache_key.c)
target_link_libraries(snx_recompiler_jit PUBLIC snx_llvm)

# The recompiler as a library (snx_recompiler.h): the backends and the function discovery behind one C interface,
# without the Switch runtime. One archive holds everything of this tree the interface needs (the objects of the
# libraries above), so a consumer links it and the external libraries (LLVM) and nothing else. A tools-only build
# (-DSNX_TOOLS_ONLY=ON) has it too. SNX_RECOMPILER_SHARED also builds the shared library, which needs the parts
# compiled as position-independent code.
option(SNX_RECOMPILER_SHARED "Also build the recompiler as a shared library (libscarletnx-recompiler.so)" OFF)
# installed with the tools (the recompiler-only configuration); a full build's install, which the AppImage and the CI
# artifacts are made from, keeps to the emulator unless asked
option(SNX_RECOMPILER_INSTALL "Install the recompiler library, its headers and its CMake package" ${SNX_TOOLS_ONLY})
include(GNUInstallDirs)
# the core alone: the backends, and what they take of src/common (snx_recompiler_base: the A64 decoder, the function
# discovery, allocation, files, clocks), never the rest of common or the platform. Beside them it links LLVM and the
# system's libraries alone (snx_recompiler_external): recompiler_symbols (tests/CMakeLists.txt) fails on any other
set(snx_recompiler_parts snx_recompiler_base snx_recompiler_core snx_llvm snx_recompiler_jit snx_arm_llvm snx_arm_decode
                         snx_arm_rt)
set(snx_recompiler_external ${SNX_LLVM_LIBS} ${SNX_LLVM_SYSTEM_LIBS} Threads::Threads ${CMAKE_DL_LIBS})
set(snx_recompiler_defines _CRT_SECURE_NO_WARNINGS)
if(WIN32)
    list(APPEND snx_recompiler_external advapi32)
else()
    list(APPEND snx_recompiler_external m)
endif()
# <scarletnx/snx_recompiler.h> in this tree, as an installed package has it
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/include/scarletnx/snx_recompiler.h" CONTENT
     "/* Generated by src/common/cpu/recompiler/CMakeLists.txt. Do not edit. */\n#include \"common/cpu/recompiler/snx_recompiler.h\"\n")
set(snx_recompiler_objects)
foreach(part IN LISTS snx_recompiler_parts)
    list(APPEND snx_recompiler_objects "$<TARGET_OBJECTS:${part}>")
endforeach()
if(SNX_RECOMPILER_SHARED)
    set_target_properties(${snx_recompiler_parts} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set(snx_recompiler_kinds STATIC SHARED)
else()
    set(snx_recompiler_kinds STATIC)
endif()
# The archive is installed for other projects' linkers, which need machine code and not this Clang's bitcode: its parts
# and the archive compile without ThinLTO (cmake/lto.cmake). The programs that link the parts get no LTO across them
snx_no_lto(${snx_recompiler_parts})
foreach(kind IN LISTS snx_recompiler_kinds)
    if(kind STREQUAL STATIC)
        set(target scarletnx-recompiler)
    else()
        set(target scarletnx-recompiler-shared)
    endif()
    add_library(${target} ${kind} library.c ${snx_recompiler_objects})
    set_target_properties(${target} PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS ON
        OUTPUT_NAME scarletnx-recompiler)
    add_dependencies(${target} ${snx_recompiler_parts})
    snx_no_lto(${target})
    # installed, the headers' layout under the prefix; in this tree (a project that adds ScarletNX with CPM or
    # add_subdirectory), <scarletnx/snx_recompiler.h> as well, and the headers it includes from src/
    target_include_directories(${target} PUBLIC "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
                                                "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/scarletnx>"
                                                "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>"
                                                "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>")
    # the parts' headers, as their own targets publish them (library.c is built against the objects, not the targets)
    foreach(part IN LISTS snx_recompiler_parts)
        target_include_directories(${target} PRIVATE "$<TARGET_PROPERTY:${part},INTERFACE_INCLUDE_DIRECTORIES>")
    endforeach()
    target_include_directories(${target} SYSTEM PRIVATE "${SNX_LLVM_INCLUDE_DIR}")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror=missing-prototypes)
    target_compile_definitions(${target} PRIVATE ${snx_recompiler_defines})
    target_link_directories(${target} PUBLIC "${SNX_LLVM_LIBRARY_DIR}")
    if(kind STREQUAL STATIC)
        target_link_libraries(${target} PUBLIC ${snx_recompiler_external})
    else()
        # the interface is the header's: SNX_RECOMPILER_API marks it (what a Windows DLL exports), and on ELF the
        # version script keeps the parts' own symbols inside
        set_target_properties(${target} PROPERTIES C_VISIBILITY_PRESET hidden)
        target_compile_definitions(${target} PRIVATE SNX_RECOMPILER_EXPORTS)
        target_link_libraries(${target} PRIVATE ${snx_recompiler_external})
        if(NOT WIN32 AND NOT APPLE)
            target_link_options(${target} PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/snx_recompiler.map")
            set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/snx_recompiler.map")
        endif()
    endif()
endforeach()
# the names an installed package gives (ScarletNXRecompilerConfig.cmake), for a project that builds this tree with its own
add_library(ScarletNX::recompiler ALIAS scarletnx-recompiler)
if(SNX_RECOMPILER_SHARED)
    add_library(ScarletNX::recompiler-shared ALIAS scarletnx-recompiler-shared)
endif()
# `cmake --install` puts the library and the headers it needs under the prefix, with a CMake package:
# find_package(ScarletNXRecompiler) gives ScarletNX::recompiler (and ScarletNX::recompiler-shared when built)
if(NOT SNX_RECOMPILER_INSTALL)
    return()
endif()
install(TARGETS scarletnx-recompiler EXPORT ScarletNXRecompiler ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(SNX_RECOMPILER_SHARED)
    install(TARGETS scarletnx-recompiler-shared EXPORT ScarletNXRecompiler
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
install(EXPORT ScarletNXRecompiler NAMESPACE ScarletNX:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ScarletNXRecompiler
    FILE ScarletNXRecompilerTargets.cmake)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ScarletNXRecompilerConfig.cmake"
    "include(CMakeFindDependencyMacro)\n"
    "set(THREADS_PREFER_PTHREAD_FLAG ON)\n"
    "find_dependency(Threads)\n"
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/ScarletNXRecompilerTargets.cmake\")\n"
    "add_library(ScarletNX::recompiler ALIAS ScarletNX::scarletnx-recompiler)\n"
    "if(TARGET ScarletNX::scarletnx-recompiler-shared)\n"
    "    add_library(ScarletNX::recompiler-shared ALIAS ScarletNX::scarletnx-recompiler-shared)\n"
    "endif()\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ScarletNXRecompilerConfig.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ScarletNXRecompiler)
# the headers, in the layout they include each other with: their paths from src/ (common/cpu/aarch64/cpu.h)
set(snx_recompiler_include "${CMAKE_INSTALL_INCLUDEDIR}/scarletnx")
install(FILES snx_recompiler.h DESTINATION ${snx_recompiler_include})
install(FILES "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu.h"
        DESTINATION ${snx_recompiler_include}/common/cpu/aarch64)
install(FILES "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/arm.h" DESTINATION ${snx_recompiler_include}/common/cpu/arm)
install(FILES "${PROJECT_SOURCE_DIR}/src/common/runtime/interface.h"
        DESTINATION ${snx_recompiler_include}/common/runtime)
# cpu.h includes it
install(FILES "${PROJECT_SOURCE_DIR}/src/common/utilities/bits.h"
        DESTINATION ${snx_recompiler_include}/common/utilities)
