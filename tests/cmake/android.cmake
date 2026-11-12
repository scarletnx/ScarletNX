# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The tests of the Android runtime (src/android), included by src/android/CMakeLists.txt where they were: they run in
# its scope, so its variables hold here, and a relative path is from src/android
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
    target_link_libraries(snx-test-android-host-anon PRIVATE scarletnx-platform Threads::Threads) # plat_mem_protect
    if(WIN32)
        target_link_libraries(snx-test-android-host-anon PRIVATE ws2_32)
    endif()
    target_compile_options(snx-test-android-host-anon PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME android_host_anon COMMAND snx-test-android-host-anon)
    # the files of an APK the driver reads before its code runs, broken ones among them
    add_executable(snx-test-android-inputs "${PROJECT_SOURCE_DIR}/tests/android_inputs.c" res/arsc.c res/axml.c
                                           runtime/install.c)
    target_include_directories(snx-test-android-inputs PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(snx-test-android-inputs PRIVATE snx_zip)
    set_target_properties(snx-test-android-inputs PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(snx-test-android-inputs PRIVATE ${snx_android_c_options} -UNDEBUG)
    add_test(NAME android_inputs COMMAND snx-test-android-inputs)
    # bionic's C library as the bridge's HLE gives it to an app's code (bridge/hle_libc.c, hle_posix.c, args.c),
    # against what bionic gives on Android 16
    add_executable(snx-test-android-bionic "${PROJECT_SOURCE_DIR}/tests/android_bionic.c")
    if(APPLE)
        target_link_libraries(snx-test-android-bionic PRIVATE snx_android_program snx_android_bridge)
    else()
        target_link_libraries(snx-test-android-bionic PRIVATE
                              "$<LINK_GROUP:RESCAN,snx_android_program,snx_android_bridge>")
    endif()
    set_target_properties(snx-test-android-bionic PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(snx-test-android-bionic PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME android_bionic COMMAND snx-test-android-bionic)
    # the 32-bit bridge (bridge/arm32*.c, which Windows leaves out): its loader on ELF32 libraries the test writes, and
    # bionic's functions as 32-bit guest code calls them
    if(NOT WIN32)
        add_executable(snx-test-android-arm32 "${PROJECT_SOURCE_DIR}/tests/android_arm32.c")
        if(APPLE)
            target_link_libraries(snx-test-android-arm32 PRIVATE snx_android_program snx_android_bridge)
        else()
            target_link_libraries(snx-test-android-arm32 PRIVATE
                                  "$<LINK_GROUP:RESCAN,snx_android_program,snx_android_bridge>")
        endif()
        set_target_properties(snx-test-android-arm32 PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
        target_compile_options(snx-test-android-arm32 PRIVATE -Wall -Wextra -Werror -UNDEBUG)
        add_test(NAME android_arm32 COMMAND snx-test-android-arm32)
        set_tests_properties(android_arm32 PROPERTIES TIMEOUT 60)
    endif()
    # JNI as the bridge gives it to an app's code (bridge/jni.c), on the JVM of javac's JDK in the test's process: on
    # Linux, where it runs, and not in a sanitizer build, whose runtime has not been tried under HotSpot
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT SNX_TEST_SANITIZERS)
        add_executable(snx-test-android-jni "${PROJECT_SOURCE_DIR}/tests/android_jni.c")
        target_link_libraries(snx-test-android-jni PRIVATE
                              "$<LINK_GROUP:RESCAN,snx_android_program,snx_android_bridge>")
        set_target_properties(snx-test-android-jni PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
        target_compile_options(snx-test-android-jni PRIVATE -Wall -Wextra -Werror -UNDEBUG)
        add_dependencies(snx-test-android-jni snx_android_framework)
        add_test(NAME android_jni COMMAND snx-test-android-jni $<TARGET_PROPERTY:snx_android_framework,JAR_FILE>
                                          "${snx_javac_home}")
    endif()
    # the framework's members as an app's code links to them (a Java source-file program over the jar)
    add_test(NAME android_framework_api COMMAND "${Java_JAVA_EXECUTABLE}"
             "${PROJECT_SOURCE_DIR}/tests/android_framework_api.java" $<TARGET_PROPERTY:snx_android_framework,JAR_FILE>)
    # the framework's behaviour as an app sees it on Android 16, where it needs none of the driver's natives
    add_test(NAME android_framework COMMAND "${Java_JAVA_EXECUTABLE}" -cp
             $<TARGET_PROPERTY:snx_android_framework,JAR_FILE> "${PROJECT_SOURCE_DIR}/tests/android_framework.java")
    # GLES 2 and 3 over Vulkan (bridge/gles2*.c): draws on the host's Vulkan (lavapipe) read back and compared, and
    # again under Khronos validation
    add_executable(snx-test-android-gles2 "${PROJECT_SOURCE_DIR}/tests/android_gles2.c" "${volk_SOURCE_DIR}/volk.c")
    set_source_files_properties("${volk_SOURCE_DIR}/volk.c" TARGET_DIRECTORY snx-test-android-gles2 PROPERTIES
                                COMPILE_OPTIONS "${snx_third_party_flags}")
    # the program and the bridge call each other: the program's archive again after the bridge's. Apple's linker goes
    # back over the archives itself, and CMake has no RESCAN group for it
    if(APPLE)
        target_link_libraries(snx-test-android-gles2 PRIVATE snx_android_program snx_android_bridge)
    else()
        target_link_libraries(snx-test-android-gles2 PRIVATE
                              "$<LINK_GROUP:RESCAN,snx_android_program,snx_android_bridge>")
    endif()
    target_compile_definitions(snx-test-android-gles2 PRIVATE VK_NO_PROTOTYPES)
    set_target_properties(snx-test-android-gles2 PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(snx-test-android-gles2 PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME android_gles2 COMMAND snx-test-android-gles2)
    set_tests_properties(android_gles2 PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT WIN32 AND Python3_Interpreter_FOUND)
        add_test(NAME android_gles2_validation COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/3ds_vk_validation.py" $<TARGET_FILE:snx-test-android-gles2>)
        set_tests_properties(android_gles2_validation PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    endif()
endif()

if(COMMAND snx_sanitizer_path) # tests/CMakeLists.txt: this folder's tests
    snx_test_sanitize(snx-test-bionic-sync snx-test-android-image snx-test-android-host-anon snx-test-android-gles2
                      snx-test-android-inputs snx-test-android-bionic snx-test-android-arm32)
    snx_sanitizer_path()
endif()
