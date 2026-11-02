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

static void* open_jvm(char* error, size_t error_size) {
    const char* homes[2] = {getenv("JAVA_HOME"), NULL};
#ifdef SNX_JAVA_HOME
    homes[1] = SNX_JAVA_HOME;
#endif
    char tried[1024] = "";
    for (size_t i = 0; i < 2; ++i) {
        if (!homes[i] || !homes[i][0]) continue;
        char* path = snx_path_join(homes[i], jvm_path);
        if (!path) continue;
        void* library = snx_is_file(path) ? snx_library_open(path, error, error_size) : NULL;
        size_t used = strlen(tried);
        snprintf(tried + used, sizeof tried - used, "%s%s", used ? ", " : "", path);
        free(path);
        if (library) return library;
    }
    snprintf(error, error_size, "no JVM: set JAVA_HOME to a JDK 21 or later (tried: %s)",
             tried[0] ? tried : "JAVA_HOME is not set");
    return NULL;
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
