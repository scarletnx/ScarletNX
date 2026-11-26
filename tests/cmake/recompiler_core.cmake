# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The recompiler's core (snx_recompiler_core, src/common/cpu/recompiler/CMakeLists.txt) without LLVM, whatever the build
# has: tests/CMakeLists.txt includes it in every build of the tests, SNX_WITH_LLVM=OFF's too

# what the core's archive needs from outside it: the decoders and the host calls (snx_recompiler_base, snx_arm_decode)
# and the system's libraries, no LLVM (tests/recompiler_symbols.cmake). Its program links every object of the archive
# with those, named here rather than taken from the core's own link line
add_executable(snx-test-recompiler-core-symbols EXCLUDE_FROM_ALL
               "${PROJECT_SOURCE_DIR}/tests/recompiler_symbols.c")
set_target_properties(snx-test-recompiler-core-symbols PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx-test-recompiler-core-symbols PRIVATE -pedantic-errors -Wall -Wextra -Werror)
add_dependencies(snx-test-recompiler-core-symbols snx_recompiler_core)
target_link_libraries(snx-test-recompiler-core-symbols PRIVATE
                      "$<LINK_LIBRARY:WHOLE_ARCHIVE,$<TARGET_FILE:snx_recompiler_core>>" snx_recompiler_base
                      snx_arm_decode Threads::Threads ${CMAKE_DL_LIBS})
if(NOT WIN32)
    target_link_libraries(snx-test-recompiler-core-symbols PRIVATE m)
endif()
snx_test_sanitize(snx-test-recompiler-core-symbols) # the sanitizers' runtime, which the archive's objects call there
add_test(NAME recompiler_core_symbols COMMAND "${CMAKE_COMMAND}" "-DBUILD=${PROJECT_BINARY_DIR}"
         -DTARGET=snx-test-recompiler-core-symbols "-DARCHIVE=$<TARGET_FILE:snx_recompiler_core>" "-DNM=${CMAKE_NM}"
         "-DCONFIG=$<CONFIG>" "-DLIBRARIES=snx_recompiler_base's, snx_arm_decode's or the system's"
         "-DRULE=The recompiler's core builds and links without LLVM (src/common/cpu/recompiler/CMakeLists.txt)"
         -P "${PROJECT_SOURCE_DIR}/tests/recompiler_symbols.cmake")

# SNX_WITH_LLVM (cmake/with_llvm.cmake): which builds it stops
add_test(NAME with_llvm_option COMMAND "${CMAKE_COMMAND}" "-DSCRIPT=${PROJECT_SOURCE_DIR}/cmake/with_llvm.cmake"
         -P "${PROJECT_SOURCE_DIR}/tests/with_llvm_test.cmake")

# what makes the code the native back end leaves (recompiler/fallback.h), and the AArch32 executor and the JIT over it:
# none, in a program that links no LLVM, and LLVM's where the build has it. exec.c is in the test, its loads counted
# and refused, and the JIT's sources beside it with the cache's key of each (jit_cache_key.c, jit_cache_key_none.c)
set(snx_fallback_tests none)
if(SNX_WITH_LLVM)
    list(APPEND snx_fallback_tests llvm)
endif()
foreach(fallback IN LISTS snx_fallback_tests)
    set(target snx-test-recompiler-fallback-${fallback})
    set(jit "${PROJECT_SOURCE_DIR}/src/common/cpu/recompiler/jit")
    add_executable(${target} "${PROJECT_SOURCE_DIR}/tests/recompiler_fallback.c" "${jit}/jit_service.c"
                             "${jit}/jit_cache.c")
    snx_test_target(${target} gnu23)
    target_link_libraries(${target} PRIVATE Threads::Threads)
    if(fallback STREQUAL none)
        target_sources(${target} PRIVATE "${jit}/jit_cache_key_none.c")
        target_link_libraries(${target} PRIVATE snx_fallback_none snx_arm_rt)
        if(NOT WIN32)
            target_link_libraries(${target} PRIVATE m)
        endif()
    else()
        target_sources(${target} PRIVATE "${jit}/jit_cache_key.c")
        target_link_libraries(${target} PRIVATE snx_llvm)
    endif()
    add_test(NAME recompiler_fallback_${fallback} COMMAND ${target} WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()
