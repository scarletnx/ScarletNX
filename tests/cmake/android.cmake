# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The tests of the Android runtime (src/android), included by src/android/CMakeLists.txt where they were: they run in
# its scope, so its variables hold here, and a relative path is from src/android
# bionic's mutexes, conditions, rwlocks, barriers and semaphores (bridge/bionic_sync.c) on the GNU ABI kernel's futex
if(BUILD_TESTING AND NOT WIN32)
    add_executable(snx-test-bionic-sync "${PROJECT_SOURCE_DIR}/tests/bionic_sync.c" bridge/bionic_sync.c)
    target_link_libraries(snx-test-bionic-sync PRIVATE scarletnx-gnuabi-kernel)
    set_target_properties(snx-test-bionic-sync PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
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
    set_target_properties(snx-test-android-image PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(snx-test-android-image PRIVATE ${snx_android_c_options} -UNDEBUG)
    add_test(NAME android_image COMMAND snx-test-android-image)
    # SoundPool's voices in the mixer (runtime/audio.c) through its natives, on SDL's dummy audio driver: a seek past
    # the sound's end, before its start and to no number
    add_executable(snx-test-android-audio "${PROJECT_SOURCE_DIR}/tests/android_audio.c" runtime/audio.c)
    target_include_directories(snx-test-android-audio PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_include_directories(snx-test-android-audio SYSTEM PRIVATE "${SNX_JAVA_HOME}/include"
                               "${SNX_JAVA_HOME}/include/${snx_jni_system}")
    target_link_libraries(snx-test-android-audio PRIVATE FFmpeg::avformat FFmpeg::avcodec FFmpeg::swresample
                          SDL3::SDL3)
    if(NOT WIN32)
        target_link_libraries(snx-test-android-audio PRIVATE m)
    endif()
    set_target_properties(snx-test-android-audio PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(snx-test-android-audio PRIVATE ${snx_android_c_options} -UNDEBUG)
    add_test(NAME android_audio COMMAND snx-test-android-audio)
    set_tests_properties(android_audio PROPERTIES TIMEOUT 60)
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
    set_target_properties(snx-test-android-inputs PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(snx-test-android-inputs PRIVATE ${snx_android_c_options} -UNDEBUG)
    add_test(NAME android_inputs COMMAND snx-test-android-inputs)
    # the DEX reader's values as a file may nest them, past what a compiler writes and what a stack holds (dex/dex.c)
    add_executable(snx-test-android-dex "${PROJECT_SOURCE_DIR}/tests/android_dex.c")
    target_include_directories(snx-test-android-dex PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(snx-test-android-dex PRIVATE snx_android_dex)
    set_target_properties(snx-test-android-dex PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(snx-test-android-dex PRIVATE ${snx_android_c_options} -UNDEBUG)
    add_test(NAME android_dex COMMAND snx-test-android-dex)
    # bionic's C library as the bridge's HLE gives it to an app's code (bridge/hle_libc.c, hle_posix.c, args.c),
    # against what bionic gives on Android 16
    add_executable(snx-test-android-bionic "${PROJECT_SOURCE_DIR}/tests/android_bionic.c")
    if(APPLE)
        target_link_libraries(snx-test-android-bionic PRIVATE snx_android_program snx_android_bridge)
    else()
        target_link_libraries(snx-test-android-bionic PRIVATE
                              "$<LINK_GROUP:RESCAN,snx_android_program,snx_android_bridge>")
    endif()
    set_target_properties(snx-test-android-bionic PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    target_compile_options(snx-test-android-bionic PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME android_bionic COMMAND snx-test-android-bionic)
    # GLES 1.1's texture names, which an app chooses (bridge/hle_gles.c): the ones the table has no room for
    add_executable(snx-test-android-gles1 "${PROJECT_SOURCE_DIR}/tests/android_gles1.c")
    if(APPLE)
        target_link_libraries(snx-test-android-gles1 PRIVATE snx_android_program snx_android_bridge)
    else()
        target_link_libraries(snx-test-android-gles1 PRIVATE
                              "$<LINK_GROUP:RESCAN,snx_android_program,snx_android_bridge>")
    endif()
    set_target_properties(snx-test-android-gles1 PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    target_compile_options(snx-test-android-gles1 PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME android_gles1 COMMAND snx-test-android-gles1)
    # a 32-bit guest's copy of a Java array longer than its window (bridge/arm32_jni.c, included for its statics):
    # where the bridge has its 32-bit side, which Windows has not (src/android/CMakeLists.txt)
    if(NOT WIN32)
        add_executable(snx-test-android-arm32-jni-copy "${PROJECT_SOURCE_DIR}/tests/android_arm32_jni_copy.c")
        target_include_directories(snx-test-android-arm32-jni-copy PRIVATE "${PROJECT_SOURCE_DIR}/src")
        target_include_directories(snx-test-android-arm32-jni-copy SYSTEM PRIVATE "${SNX_JAVA_HOME}/include"
                                   "${SNX_JAVA_HOME}/include/${snx_jni_system}")
        if(APPLE)
            target_link_libraries(snx-test-android-arm32-jni-copy PRIVATE snx_android_program snx_android_bridge)
        else()
            target_link_libraries(snx-test-android-arm32-jni-copy PRIVATE
                                  "$<LINK_GROUP:RESCAN,snx_android_program,snx_android_bridge>")
        endif()
        set_target_properties(snx-test-android-arm32-jni-copy PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
        target_compile_options(snx-test-android-arm32-jni-copy PRIVATE -UNDEBUG)
        add_test(NAME android_arm32_jni_copy COMMAND snx-test-android-arm32-jni-copy)
    endif()
    # what the bridge keeps for a thread ends with it, and an EGL context another thread has current stays (bridge.c,
    # hle_egl.c, hle_android.c), and the same for a 32-bit guest's (bridge/arm32_thread.c, included for its statics)
    if(NOT WIN32)
        add_executable(snx-test-android-thread-end "${PROJECT_SOURCE_DIR}/tests/android_thread_end.c")
        if(APPLE)
            target_link_libraries(snx-test-android-thread-end PRIVATE snx_android_program snx_android_bridge)
        else()
            target_link_libraries(snx-test-android-thread-end PRIVATE
                                  "$<LINK_GROUP:RESCAN,snx_android_program,snx_android_bridge>")
        endif()
        set_target_properties(snx-test-android-thread-end PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
        target_compile_options(snx-test-android-thread-end PRIVATE -Wall -Wextra -Werror -UNDEBUG)
        add_test(NAME android_thread_end COMMAND snx-test-android-thread-end)
        set_tests_properties(android_thread_end PROPERTIES TIMEOUT 60)
        add_executable(snx-test-android-thread-state "${PROJECT_SOURCE_DIR}/tests/android_thread_state.c")
        target_include_directories(snx-test-android-thread-state PRIVATE "${PROJECT_SOURCE_DIR}/src")
        target_include_directories(snx-test-android-thread-state SYSTEM PRIVATE "${SNX_JAVA_HOME}/include"
                                   "${SNX_JAVA_HOME}/include/${snx_jni_system}")
        if(APPLE)
            target_link_libraries(snx-test-android-thread-state PRIVATE snx_android_program snx_android_bridge)
        else()
            target_link_libraries(snx-test-android-thread-state PRIVATE
                                  "$<LINK_GROUP:RESCAN,snx_android_program,snx_android_bridge>")
        endif()
        set_target_properties(snx-test-android-thread-state PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
        target_compile_options(snx-test-android-thread-state PRIVATE -UNDEBUG)
        add_test(NAME android_thread_state COMMAND snx-test-android-thread-state)
        set_tests_properties(android_thread_state PROPERTIES TIMEOUT 60)
        add_executable(snx-test-android-arm32-threads "${PROJECT_SOURCE_DIR}/tests/android_arm32_threads.c")
        target_include_directories(snx-test-android-arm32-threads PRIVATE "${PROJECT_SOURCE_DIR}/src")
        target_include_directories(snx-test-android-arm32-threads SYSTEM PRIVATE "${SNX_JAVA_HOME}/include"
                                   "${SNX_JAVA_HOME}/include/${snx_jni_system}")
        if(APPLE)
            target_link_libraries(snx-test-android-arm32-threads PRIVATE snx_android_program snx_android_bridge)
        else()
            target_link_libraries(snx-test-android-arm32-threads PRIVATE
                                  "$<LINK_GROUP:RESCAN,snx_android_program,snx_android_bridge>")
        endif()
        set_target_properties(snx-test-android-arm32-threads PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
        target_compile_options(snx-test-android-arm32-threads PRIVATE -UNDEBUG)
        add_test(NAME android_arm32_threads COMMAND snx-test-android-arm32-threads)
        set_tests_properties(android_arm32_threads PROPERTIES TIMEOUT 60)
    endif()
    # a style applied with no array of indices, over assets with no table (res/arsc.c)
    add_executable(snx-test-android-arsc-style "${PROJECT_SOURCE_DIR}/tests/android_arsc_style.c")
    target_include_directories(snx-test-android-arsc-style PRIVATE "${PROJECT_SOURCE_DIR}/src")
    if(APPLE)
        target_link_libraries(snx-test-android-arsc-style PRIVATE snx_android_program snx_android_bridge)
    else()
        target_link_libraries(snx-test-android-arsc-style PRIVATE
                              "$<LINK_GROUP:RESCAN,snx_android_program,snx_android_bridge>")
    endif()
    set_target_properties(snx-test-android-arsc-style PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    target_compile_options(snx-test-android-arsc-style PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME android_arsc_style COMMAND snx-test-android-arsc-style)
    # GLES 1.1's samplers, one for each pair of a texture's filters and wraps: hle_gles.c included for its static,
    # vkCreateSampler stood in for
    add_executable(snx-test-android-gles1-samplers "${PROJECT_SOURCE_DIR}/tests/android_gles1_samplers.c")
    if(APPLE)
        target_link_libraries(snx-test-android-gles1-samplers PRIVATE snx_android_program snx_android_bridge)
    else()
        target_link_libraries(snx-test-android-gles1-samplers PRIVATE
                              "$<LINK_GROUP:RESCAN,snx_android_program,snx_android_bridge>")
    endif()
    set_target_properties(snx-test-android-gles1-samplers PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    target_compile_options(snx-test-android-gles1-samplers PRIVATE -UNDEBUG)
    add_test(NAME android_gles1_samplers COMMAND snx-test-android-gles1-samplers)
    # the bridge's start where no recompiler is made for the host, snx_recompiler_new wrapped at link time; and
    # a lookup in the linker's libraries while another thread's load is held in its read, libc's pread wrapped
    if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|FreeBSD)$")
        add_executable(snx-test-android-linker-lock "${PROJECT_SOURCE_DIR}/tests/android_linker_lock.c")
        target_link_libraries(snx-test-android-linker-lock PRIVATE
                              "$<LINK_GROUP:RESCAN,snx_android_program,snx_android_bridge>" Threads::Threads)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux") # glibc's pread is pread64 under 64-bit offsets
            target_link_options(snx-test-android-linker-lock PRIVATE "LINKER:--wrap=pread,--wrap=pread64")
        else()
            target_link_options(snx-test-android-linker-lock PRIVATE "LINKER:--wrap=pread")
        endif()
        set_target_properties(snx-test-android-linker-lock PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
        target_compile_options(snx-test-android-linker-lock PRIVATE -Wall -Wextra -Werror -UNDEBUG)
        add_test(NAME android_linker_lock COMMAND snx-test-android-linker-lock)
        set_tests_properties(android_linker_lock PROPERTIES TIMEOUT 60)
        # libraries whose symbol hash tables hold no bucket, made by the test, loaded and looked up in
        add_executable(snx-test-android-linker-hash "${PROJECT_SOURCE_DIR}/tests/android_linker_hash.c")
        target_link_libraries(snx-test-android-linker-hash PRIVATE
                              "$<LINK_GROUP:RESCAN,snx_android_program,snx_android_bridge>")
        set_target_properties(snx-test-android-linker-hash PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
        target_compile_options(snx-test-android-linker-hash PRIVATE -Wall -Wextra -Werror -UNDEBUG)
        add_test(NAME android_linker_hash COMMAND snx-test-android-linker-hash)
        set_tests_properties(android_linker_hash PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        add_executable(snx-test-android-bridge-start "${PROJECT_SOURCE_DIR}/tests/android_bridge_start.c")
        target_link_libraries(snx-test-android-bridge-start PRIVATE
                              "$<LINK_GROUP:RESCAN,snx_android_program,snx_android_bridge>")
        target_link_options(snx-test-android-bridge-start PRIVATE "LINKER:--wrap=snx_recompiler_new")
        set_target_properties(snx-test-android-bridge-start PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
        target_compile_options(snx-test-android-bridge-start PRIVATE -Wall -Wextra -Werror -UNDEBUG)
        add_test(NAME android_bridge_start COMMAND snx-test-android-bridge-start)
    endif()
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
        set_target_properties(snx-test-android-arm32 PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
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
        set_target_properties(snx-test-android-jni PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
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
    set_target_properties(snx-test-android-gles2 PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    target_compile_options(snx-test-android-gles2 PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        # the driver's command line: the Android group's settings of the launcher's Settings, no app and no JVM
        add_test(NAME android_driver COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/android_driver.py"
                 "${PROJECT_BINARY_DIR}/scarletnx-android${CMAKE_EXECUTABLE_SUFFIX}")
        set_tests_properties(android_driver PROPERTIES TIMEOUT 120)
    endif()
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
                      snx-test-android-inputs snx-test-android-bionic snx-test-android-arm32 snx-test-android-dex
                      snx-test-android-bridge-start snx-test-android-audio snx-test-android-gles1
                      snx-test-android-linker-lock snx-test-android-linker-hash
                      snx-test-android-gles1-samplers snx-test-android-arsc-style
                      snx-test-android-arm32-jni-copy
                      snx-test-android-thread-end snx-test-android-thread-state snx-test-android-arm32-threads)
    snx_sanitizer_path()
endif()
