# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The Android app driver (docs/ANDROID_APPS.md): an APK's DEX code translated to JVM classes, run by a JVM beside
# ScarletNX's own android.* and the HLE of Android's userland, which scarletnx runs as scarletnx-android
# (src/multicall).
#
#   snx_android_dex         the DEX reader and its translator to class files (dex/)
#   snx_android_program     the command line (runtime/main.c), the JVM host and the natives
#   snx_android_framework   ScarletNX's android.* and the runtime's Java side (framework/), a jar beside the program
set(snx_android_c_options -pedantic-errors -Wall -Wextra -Werror -Werror=missing-prototypes
                          -Werror=missing-variable-declarations)
add_library(snx_android_dex STATIC dex/dex.c dex/classfile.c dex/translate.c)
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

add_library(snx_android_program STATIC runtime/main.c runtime/jvm.c runtime/app.c res/axml.c res/xml_jni.c)
target_link_libraries(snx_android_program PUBLIC snx_android_dex snx_zip ${CMAKE_DL_LIBS})
target_include_directories(snx_android_program SYSTEM PUBLIC "${SNX_JAVA_HOME}/include"
                           "${SNX_JAVA_HOME}/include/${snx_jni_system}")
target_compile_definitions(snx_android_program PRIVATE SNX_JAVA_HOME="${SNX_JAVA_HOME}")
set_target_properties(snx_android_program PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx_android_program PRIVATE ${snx_android_c_options})
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(snx_android_program PUBLIC Threads::Threads)
endif()

file(GLOB_RECURSE snx_android_java CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/framework/*.java")
set(CMAKE_JAVA_COMPILE_FLAGS --release 21 -encoding UTF-8 -Xlint:-options -nowarn)
add_jar(snx_android_framework SOURCES ${snx_android_java} OUTPUT_NAME scarletnx-android-framework
        OUTPUT_DIR "${PROJECT_BINARY_DIR}")
add_dependencies(snx_android_program snx_android_framework)
