/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_JVM_H
#define SNX_ANDROID_JVM_H
#include <jni.h>
#include <stdbool.h>
#include <stddef.h>

/* The JVM the app runs on: OpenJDK's libjvm, loaded at run time and started through its invocation API. JAVA_HOME
   names the JDK; without it, the one the build found */
typedef struct {
    void* library;
    JavaVM* vm;
    JNIEnv* env;
} AndroidJvm;

/* a JVM with class_path (the framework's jar) on the calling thread, its options after the class path's. False with
   error filled */
bool android_jvm_start(AndroidJvm* jvm, const char* class_path, const char* const* options, size_t option_count,
                       char* error, size_t error_size);
/* the pending exception, printed with its stack and cleared; false when there was none */
bool android_jvm_exception(JNIEnv* env);
/* natives for a class (internal name), from a table; false with the exception printed */
bool android_jvm_register(JNIEnv* env, const char* class_name, const JNINativeMethod* methods, int count);
#endif
