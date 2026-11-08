/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "android/runtime/jvm.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef jint(JNICALL* CreateJavaVM)(JavaVM** vm, void** env, void* args);

/* libjvm in a JDK, by its system's layout */
#if defined(_WIN32)
static const char jvm_path[] = "bin/server/jvm.dll";
#elif defined(__APPLE__)
static const char jvm_path[] = "lib/server/libjvm.dylib";
#else
static const char jvm_path[] = "lib/server/libjvm.so";
#endif

static const char* java_home;
void android_jvm_set_home(const char* home) {
    java_home = home;
}

static void* open_jvm(char* error, size_t error_size) {
    /* --java-home's alone when it is given: a JDK named that does not load is said, never passed over for the build's */
    const char* home = java_home && java_home[0] ? java_home : NULL;
#ifdef SNX_JAVA_HOME
    if (!home) home = SNX_JAVA_HOME;
#endif
    static const char where[] =
        "set the launcher's Settings > Launch > Java home, or give --java-home=DIR, to a JDK 21 "
        "or later";
    if (!home) {
        snprintf(error, error_size, "no JVM: the build found no JDK; %s", where);
        return NULL;
    }
    char* path = snx_path_join(home, jvm_path);
    if (!path) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    char why[512] = "";
    void* library = snx_is_file(path) ? snx_library_open(path, why, sizeof why) : NULL;
    if (!library) snprintf(error, error_size, "no JVM at %s (%s): %s", path, why[0] ? why : "no such file", where);
    free(path);
    return library;
}

bool android_jvm_start(AndroidJvm* jvm, const char* class_path, const char* const* options, size_t option_count,
                       char* error, size_t error_size) {
    *jvm = (AndroidJvm){0};
    jvm->library = open_jvm(error, error_size);
    if (!jvm->library) return false;
    CreateJavaVM create = (CreateJavaVM)(size_t)snx_library_symbol(jvm->library, "JNI_CreateJavaVM");
    if (!create) {
        snprintf(error, error_size, "the JVM library has no JNI_CreateJavaVM");
        return false;
    }
    char* class_option = snx_format("-Djava.class.path=%s", class_path);
    JavaVMOption* list = calloc(option_count + 3, sizeof *list);
    if (!class_option || !list) {
        free(class_option);
        free(list);
        snprintf(error, error_size, "out of memory");
        return false;
    }
    size_t count = 0;
    list[count++].optionString = class_option;
    /* the natives this program registers, without the JVM's warning for each module that binds natives */
    list[count++].optionString = (char*)"--enable-native-access=ALL-UNNAMED";
    for (size_t i = 0; i < option_count; ++i) list[count++].optionString = (char*)options[i];
    JavaVMInitArgs args = {0};
    args.version = JNI_VERSION_21;
    args.nOptions = (jint)count;
    args.options = list;
    args.ignoreUnrecognized = JNI_FALSE;
    void* env = NULL;
    jint status = create(&jvm->vm, &env, &args);
    free(class_option);
    free(list);
    if (status != JNI_OK) {
        snprintf(error, error_size, "the JVM did not start (JNI_CreateJavaVM: %d)", (int)status);
        return false;
    }
    jvm->env = env;
    return true;
}

bool android_jvm_exception(JNIEnv* env) {
    if (!(*env)->ExceptionCheck(env)) return false;
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    return true;
}

bool android_jvm_register(JNIEnv* env, const char* class_name, const JNINativeMethod* methods, int count) {
    jclass c = (*env)->FindClass(env, class_name);
    if (!c) {
        android_jvm_exception(env);
        fprintf(stderr, "scarletnx-android: the framework has no class %s\n", class_name);
        return false;
    }
    jint status = (*env)->RegisterNatives(env, c, methods, count);
    (*env)->DeleteLocalRef(env, c);
    if (status != JNI_OK) {
        android_jvm_exception(env);
        fprintf(stderr, "scarletnx-android: the natives of %s do not match its class\n", class_name);
        return false;
    }
    return true;
}
