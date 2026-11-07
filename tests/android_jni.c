/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* JNI as the native bridge gives it to an app's code (src/android/bridge/jni.c), on the host's JVM: the guest's JNIEnv
   functions called as guest code calls them, through the table's export and a host call into guest code
   (bridge_call), against what ART gives on Android 16. Its arguments: the framework's jar and the JDK to run */
#include "android/bridge/bridge.h"
#include "android/bridge/jni.h"
#include "android/runtime/jvm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the JNINativeInterface's functions the test calls, by index */
enum {
    GET_VERSION = 4,
    FIND_CLASS = 6,
    EXCEPTION_OCCURRED = 15,
    EXCEPTION_CLEAR = 17,
};

static struct {
    JNIEnv* env;
    uint64_t genv; /* the guest's JNIEnv* of the main thread */
    int failures;
} g_t;

static void expect_int(const char* what, int64_t got, int64_t want) {
    if (got != want) {
        fprintf(stderr, "android_jni: %s: got %lld (0x%llx), want %lld (0x%llx)\n", what, (long long)got,
                (unsigned long long)got, (long long)want, (unsigned long long)want);
        g_t.failures++;
    }
}
static void stop(const char* what) {
    fprintf(stderr, "android_jni: %s\n", what);
    if (g_t.env && (*g_t.env)->ExceptionCheck(g_t.env)) android_jvm_exception(g_t.env);
    exit(1);
}

/* the guest's JNIEnv function index called with genv and up to four arguments, as guest code calls it */
static uint64_t jni(uint64_t genv, unsigned index, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    uint64_t table, fn;
    memcpy(&table, (const void*)(uintptr_t)genv, sizeof table);
    memcpy(&fn, (const void*)(uintptr_t)(table + 8ull * index), sizeof fn);
    const uint64_t args[5] = {genv, a, b, c, d};
    return bridge_call(fn, args, 5);
}
#define P(p) ((uint64_t)(uintptr_t)(p))

/* ---- the version ---- */

static void test_version(void) {
    expect_int("GetVersion", (int64_t)jni(g_t.genv, GET_VERSION, 0, 0, 0, 0), JNI_VERSION_1_6);
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s FRAMEWORK_JAR JAVA_HOME\n", argv[0]);
        return 2;
    }
    android_jvm_set_home(argv[2]);
    AndroidJvm jvm = {0};
    char error[512];
    if (!android_jvm_start(&jvm, argv[1], NULL, 0, error, sizeof error)) stop(error);
    g_t.env = jvm.env;
    if (!android_bridge_register(g_t.env)) stop("NativeBridge's natives do not register");
    /* the bridge starts, with its JNI tables, at the first library a NativeBridge.load asks for: one that is not
       there here */
    jclass bridge = (*g_t.env)->FindClass(g_t.env, "snx/android/nativebridge/NativeBridge");
    jmethodID load = bridge ? (*g_t.env)->GetStaticMethodID(g_t.env, bridge, "nativeLoad",
                                                            "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")
                            : NULL;
    if (!load) stop("no NativeBridge.nativeLoad");
    jstring missing = (*g_t.env)->NewStringUTF(g_t.env, "android_jni_no_such_library.so");
    if (!(*g_t.env)->CallStaticObjectMethod(g_t.env, bridge, load, missing, NULL)) stop("a missing library loaded");
    g_t.genv = bridge_jni_env(g_t.env);

    test_version();
    if (g_t.failures) {
        fprintf(stderr, "android_jni: %d failed\n", g_t.failures);
        return 1;
    }
    puts("android_jni: all passed");
    return 0;
}
