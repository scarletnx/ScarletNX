# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
# The Android app's manifest says it is a game, so the system's game mode and the makers' game launchers treat it as
# one. SRC is the source tree.
file(READ "${SRC}/dist/android/src/main/AndroidManifest.xml" manifest)
string(REGEX MATCH "<application[^>]*>" application "${manifest}")
if(application STREQUAL "")
    message(FATAL_ERROR "dist/android/src/main/AndroidManifest.xml has no <application> tag")
endif()
foreach(attribute "android:appCategory=\"game\"" "android:isGame=\"true\"")
    string(FIND "${application}" "${attribute}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "the app's <application> has no ${attribute}: ${application}")
    endif()
endforeach()
