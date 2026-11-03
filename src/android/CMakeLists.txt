# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The Android app driver (docs/android/README.md): an APK's DEX code translated to JVM classes, run by a JVM beside
# ScarletNX's own android.* and the HLE of Android's userland, which scarletnx runs as scarletnx-android
# (src/multicall).
#
#   snx_android_dex         the DEX reader and its translator to class files (dex/)
#   snx_android_program     the command line (runtime/main.c), the JVM host and the natives
#   snx_android_framework   ScarletNX's android.* and the runtime's Java side (framework/), a jar beside the program
# the JIT compiles an app's native code at run time with the LLVM the build links: LLVM 19's code generator crashes on
# some of an engine's (a trap in its DAG combiner), so the Android driver is built with LLVM 22 (docs/android/README.md)
if(SNX_LLVM_VERSION)
    string(REGEX MATCH "^[0-9]+" snx_android_llvm_major "${SNX_LLVM_VERSION}")
    if(snx_android_llvm_major AND snx_android_llvm_major LESS 22)
        message(WARNING "The Android driver is built with LLVM ${SNX_LLVM_VERSION}: build it with LLVM 22 "
                        "(-DSNX_LLVM_CONFIG=), as LLVM ${snx_android_llvm_major}'s code generator crashes on some "
                        "apps' native code (docs/android/README.md)")
    endif()
endif()
set(snx_android_c_options -pedantic-errors -Wall -Wextra -Werror -Werror=missing-prototypes
                          -Werror=missing-variable-declarations)
add_library(snx_android_dex STATIC dex/dex.c dex/classfile.c dex/translate.c dex/annotations.c)
target_link_libraries(snx_android_dex PUBLIC scarletnx-common)
set_target_properties(snx_android_dex PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx_android_dex PRIVATE ${snx_android_c_options})

# The JDK: javac for the framework, and jni.h. The program loads libjvm at run time (runtime/jvm.c), from JAVA_HOME
# or this JDK: javac's own, or SNX_JAVA_HOME
find_package(Java 21 COMPONENTS Development REQUIRED)
include(UseJava)
get_filename_component(snx_javac "${Java_JAVAC_EXECUTABLE}" REALPATH)
get_filename_component(snx_javac_home "${snx_javac}/../.." ABSOLUTE)
set(SNX_JAVA_HOME "${snx_javac_home}" CACHE PATH "The JDK (21 or later) whose jni.h the Android driver builds with")
if(WIN32)
    set(snx_jni_system win32)
elseif(APPLE)
    set(snx_jni_system darwin)
else()
    string(TOLOWER "${CMAKE_SYSTEM_NAME}" snx_jni_system)
endif()
if(NOT EXISTS "${SNX_JAVA_HOME}/include/jni.h")
    message(FATAL_ERROR "The Android driver needs a JDK 21 or later: ${SNX_JAVA_HOME} has no include/jni.h. Set "
                        "SNX_JAVA_HOME to one, or SNX_BUILD_ANDROID=OFF")
endif()

add_library(snx_android_program STATIC runtime/main.c runtime/jvm.c runtime/app.c runtime/install.c res/axml.c
                                       res/xml_jni.c res/arsc.c
                                       res/asset_jni.c db/sqlite_jni.c hwui/text.c hwui/image.c hwui/path.c
                                       hwui/render.c "${volk_SOURCE_DIR}/volk.c")
# FFmpeg decodes and encodes the images of BitmapFactory and Bitmap.compress (hwui/image.c)
target_link_libraries(snx_android_program PUBLIC snx_android_dex snx_zip snx_sqlite snx_harfbuzz snx_vk_common
                      FFmpeg::avcodec FFmpeg::swscale SDL3::SDL3 Vulkan::Headers ${CMAKE_DL_LIBS})
if(NOT WIN32)
    target_link_libraries(snx_android_program PUBLIC m)
endif()
target_compile_definitions(snx_android_program PRIVATE VK_NO_PROTOTYPES)
set_source_files_properties("${volk_SOURCE_DIR}/volk.c" TARGET_DIRECTORY snx_android_program PROPERTIES
                            COMPILE_OPTIONS "${snx_third_party_flags}")
# the renderer's shaders (hwui/render.c), compiled at build time (cmake/glsl.cmake)
include("${PROJECT_SOURCE_DIR}/cmake/glsl.cmake")
snx_glsl_spirv(snx_android_program SOURCE hwui/draw.vert NAME android_draw_vert_spv HEADER android/hwui/draw_vert_spv.h)
snx_glsl_spirv(snx_android_program SOURCE hwui/draw.frag NAME android_draw_frag_spv HEADER android/hwui/draw_frag_spv.h)
snx_glsl_spirv(snx_android_program SOURCE hwui/texture.frag NAME android_texture_frag_spv
               HEADER android/hwui/texture_frag_spv.h)
target_include_directories(snx_android_program SYSTEM PUBLIC "${SNX_JAVA_HOME}/include"
                           "${SNX_JAVA_HOME}/include/${snx_jni_system}")
target_compile_definitions(snx_android_program PRIVATE SNX_JAVA_HOME="${SNX_JAVA_HOME}")
set_target_properties(snx_android_program PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx_android_program PRIVATE ${snx_android_c_options})
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(snx_android_program PUBLIC Threads::Threads)
endif()

# The native bridge (bridge/, docs/android/NATIVE.md): the app's AArch64 libraries run through the recompiler library,
# which another driver's tree may have configured already. cpu.h is written in the GNU dialect of C
if(NOT TARGET scarletnx-recompiler)
    include("${PROJECT_SOURCE_DIR}/cmake/llvm.cmake")
    add_subdirectory("${PROJECT_SOURCE_DIR}/src/common/cpu/recompiler/llvm" android-llvm-backend)
    add_subdirectory("${PROJECT_SOURCE_DIR}/src/common/cpu/recompiler" android-recompiler-library)
endif()
add_library(snx_android_bridge STATIC bridge/bridge.c bridge/host.c bridge/linker.c bridge/hle.c bridge/hle_libc.c bridge/args.c
                                      bridge/native.c bridge/hle_posix.c bridge/hle_thread.c bridge/hle_sync.c bridge/bionic_sync.c bridge/hle_signal.c bridge/fault.c bridge/hle_proc.c bridge/hle_dl.c bridge/hle_code.c
                                      bridge/hle_sles.c bridge/hle_media.c bridge/jni.c
                                      bridge/hle_android.c bridge/hle_vulkan.c
                                      bridge/hle_egl.c)
# the guest's signals, threads and futexes: the GNU ABI runtime's kernel (src/gnuabi/kernel), here too without that
# runtime
if(NOT TARGET scarletnx-gnuabi-kernel)
    add_subdirectory("${PROJECT_SOURCE_DIR}/src/gnuabi/kernel" android-gnuabi-kernel)
endif()
# scarletnx-net: the network stack's constants as the host's (common/network/net_host.h), for the guest's sockets,
# which are the host's own. Windows: winpthreads for bionic's threads, Winsock for its sockets
target_link_libraries(snx_android_bridge PUBLIC scarletnx-common scarletnx-recompiler scarletnx-gnuabi-kernel
                      scarletnx-net SDL3::SDL3 Vulkan::Headers)
find_package(Threads REQUIRED)
target_link_libraries(snx_android_bridge PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(snx_android_bridge PUBLIC ws2_32)
endif()
target_include_directories(snx_android_bridge PUBLIC "${volk_SOURCE_DIR}")
target_compile_definitions(snx_android_bridge PRIVATE VK_NO_PROTOTYPES)
# the blend of the renderer's layer over the guest's presents (bridge/hle_vulkan.c)
snx_glsl_spirv(snx_android_bridge SOURCE bridge/compose.vert NAME android_compose_vert_spv
               HEADER android/bridge/compose_vert_spv.h)
snx_glsl_spirv(snx_android_bridge SOURCE bridge/compose.frag NAME android_compose_frag_spv
               HEADER android/bridge/compose_frag_spv.h)
target_include_directories(snx_android_bridge SYSTEM PUBLIC "${SNX_JAVA_HOME}/include"
                           "${SNX_JAVA_HOME}/include/${snx_jni_system}")
set_target_properties(snx_android_bridge PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
target_compile_options(snx_android_bridge PRIVATE -Wall -Wextra -Werror -Werror=missing-prototypes
                                                  -Werror=missing-variable-declarations)
target_link_libraries(snx_android_program PUBLIC snx_android_bridge)

file(GLOB_RECURSE snx_android_java CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/framework/*.java")
set(CMAKE_JAVA_COMPILE_FLAGS --release 21 -encoding UTF-8 -Xlint:-options -nowarn)
# META-INF/services names the offline name resolver (snx.android.net.OfflineResolver) to the JDK
add_jar(snx_android_framework SOURCES ${snx_android_java}
        RESOURCES NAMESPACE "META-INF/services"
                  "${CMAKE_CURRENT_SOURCE_DIR}/framework/META-INF/services/java.net.spi.InetAddressResolverProvider"
        OUTPUT_NAME scarletnx-android-framework OUTPUT_DIR "${PROJECT_BINARY_DIR}")
add_dependencies(snx_android_program snx_android_framework)

# the font the framework draws with, beside the jar (the host names the folder in snx.android.fonts)
add_custom_command(OUTPUT "${PROJECT_BINARY_DIR}/android-fonts/NotoSansJP-Subset.ttf"
                   COMMAND "${CMAKE_COMMAND}" -E copy "${NotoSansJPSubset_SOURCE_DIR}/NotoSansJP-Subset.ttf"
                           "${PROJECT_BINARY_DIR}/android-fonts/NotoSansJP-Subset.ttf"
                   DEPENDS "${NotoSansJPSubset_SOURCE_DIR}/NotoSansJP-Subset.ttf")
add_custom_target(snx_android_fonts DEPENDS "${PROJECT_BINARY_DIR}/android-fonts/NotoSansJP-Subset.ttf")
add_dependencies(snx_android_program snx_android_fonts)

# bionic's mutexes, conditions, rwlocks, barriers and semaphores (bridge/bionic_sync.c) on the GNU ABI kernel's futex
if(BUILD_TESTING AND NOT WIN32)
    add_executable(snx-test-bionic-sync "${PROJECT_SOURCE_DIR}/tests/bionic_sync.c" bridge/bionic_sync.c)
    target_link_libraries(snx-test-bionic-sync PRIVATE scarletnx-gnuabi-kernel)
    set_target_properties(snx-test-bionic-sync PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(snx-test-bionic-sync PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME android_bionic_sync COMMAND snx-test-bionic-sync)
    set_tests_properties(android_bionic_sync PROPERTIES TIMEOUT 120)
endif()
if(BUILD_TESTING)
    # the image codec of BitmapFactory and Bitmap.compress over FFmpeg (hwui/image.c)
    add_executable(snx-test-android-image "${PROJECT_SOURCE_DIR}/tests/android_image.c" hwui/image.c)
    target_include_directories(snx-test-android-image PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_include_directories(snx-test-android-image SYSTEM PRIVATE "${SNX_JAVA_HOME}/include"
                               "${SNX_JAVA_HOME}/include/${snx_jni_system}")
    target_link_libraries(snx-test-android-image PRIVATE FFmpeg::avcodec FFmpeg::swscale)
    set_target_properties(snx-test-android-image PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(snx-test-android-image PRIVATE ${snx_android_c_options} -UNDEBUG)
    add_test(NAME android_image COMMAND snx-test-android-image)
    # the record of the guest's anonymous mappings and its MADV_DONTNEED, which FreeBSD, macOS and Windows take
    add_executable(snx-test-android-host-anon "${PROJECT_SOURCE_DIR}/tests/android_host_anon.c" bridge/host.c)
    target_include_directories(snx-test-android-host-anon PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(snx-test-android-host-anon PRIVATE Threads::Threads)
    if(WIN32)
        target_link_libraries(snx-test-android-host-anon PRIVATE ws2_32)
    endif()
    target_compile_options(snx-test-android-host-anon PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME android_host_anon COMMAND snx-test-android-host-anon)
endif()
