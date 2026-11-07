# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
#   cmake -DSRC=<source dir> -DWORK=<scratch dir> -P tests/android_jdk_test.cmake
#
# Which JDK cmake/android_jdk.cmake takes for the Android app driver, and the reason it gives when it takes none: a
# JDK with include/jni.h, one without it (Android Studio's own runtime), a javac below 21, no javac, and SNX_JAVA_HOME
# over the javac found.
if(NOT DEFINED SRC OR NOT DEFINED WORK)
    message(FATAL_ERROR "android_jdk_test.cmake needs -DSRC=... -DWORK=...")
endif()
include("${SRC}/cmake/android_jdk.cmake")

file(REMOVE_RECURSE "${WORK}")
file(WRITE "${WORK}/full/bin/javac" "")
file(WRITE "${WORK}/full/include/jni.h" "")
file(WRITE "${WORK}/runtime/bin/javac" "")
file(WRITE "${WORK}/other/include/jni.h" "")
set(failed 0)
function(expect label java_home javac version want_home want_why)
    snx_android_jdk("${java_home}" "${javac}" "${version}" home why)
    if(NOT home STREQUAL want_home OR NOT why MATCHES "${want_why}")
        message(SEND_ERROR "${label}: took '${home}' ('${why}'), not '${want_home}' ('${want_why}')")
        set(failed 1 PARENT_SCOPE)
    endif()
endfunction()
expect("a JDK with jni.h" "" "${WORK}/full/bin/javac" 21.0.5 "${WORK}/full" "^$")
expect("a newer JDK" "" "${WORK}/full/bin/javac" 25.0.4 "${WORK}/full" "^$")
expect("a runtime without include/" "" "${WORK}/runtime/bin/javac" 25.0.4 "" "${WORK}/runtime has no include/jni.h")
expect("a javac below 21" "" "${WORK}/full/bin/javac" 17.0.12 "" "version 17.0.12, below 21")
expect("no javac" "" "Java_JAVAC_EXECUTABLE-NOTFOUND" "" "" "no javac was found")
expect("SNX_JAVA_HOME over javac" "${WORK}/other" "${WORK}/runtime/bin/javac" 25.0.4 "${WORK}/other" "^$")
expect("SNX_JAVA_HOME without jni.h" "${WORK}/runtime" "${WORK}/full/bin/javac" 21.0.5 ""
       "${WORK}/runtime has no include/jni.h")
if(failed)
    message(FATAL_ERROR "android_jdk.cmake took the wrong JDK")
endif()
message("android_jdk.cmake takes a JDK 21 or later with include/jni.h, and names why it takes none")
