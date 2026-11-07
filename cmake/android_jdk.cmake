# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
# The JDK the Android app driver builds with: javac for its framework and the JDK's include/jni.h. A JDK without
# include/ (Android Studio's own runtime) or a javac below 21 cannot build it.
#
#   snx_android_jdk(JAVA_HOME JAVAC VERSION HOME_VAR WHY_VAR)
#
# JAVA_HOME is SNX_JAVA_HOME when the build was given one, else empty; JAVAC is the javac find_package(Java) found
# (empty or ...-NOTFOUND when none); VERSION its version. HOME_VAR gets the JDK's folder when it can build the driver,
# else empty, and WHY_VAR then says why, naming what was looked at.
function(snx_android_jdk java_home javac version home_var why_var)
    set(home "")
    set(why "")
    if(java_home)
        set(home "${java_home}")
    elseif(javac AND NOT javac MATCHES "-NOTFOUND$")
        get_filename_component(real "${javac}" REALPATH)
        get_filename_component(home "${real}/../.." ABSOLUTE)
        if(version VERSION_LESS 21)
            set(why "the javac found, ${real}, is version ${version}, below 21")
            set(home "")
        endif()
    else()
        set(why "no javac was found (set JAVA_HOME or SNX_JAVA_HOME to a JDK 21 or later)")
    endif()
    if(home AND NOT EXISTS "${home}/include/jni.h")
        set(why "the JDK at ${home} has no include/jni.h")
        set(home "")
    endif()
    set(${home_var} "${home}" PARENT_SCOPE)
    set(${why_var} "${why}" PARENT_SCOPE)
endfunction()
