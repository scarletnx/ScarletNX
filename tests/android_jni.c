/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* JNI as the native bridge gives it to an app's code (src/android/bridge/jni.c), on the host's JVM: the guest's JNIEnv
   functions called as guest code calls them, through the table's export and a host call into guest code
   (bridge_call), against what ART gives on Android 16. Its arguments: the framework's jar and the JDK to run */
#include "android/bridge/bridge.h"
#include "android/bridge/jni.h"
#include "android/runtime/jvm.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the JNINativeInterface's functions the test calls, by index */
enum {
    GET_VERSION = 4,
    FIND_CLASS = 6,
    EXCEPTION_OCCURRED = 15,
    EXCEPTION_CLEAR = 17,
    REGISTER_NATIVES = 215,
    GET_JAVA_VM = 219,
};
/* the JNIInvokeInterface's */
enum { ATTACH_CURRENT_THREAD = 4, DETACH_CURRENT_THREAD = 5 };

static struct {
    JNIEnv* env;
    uint64_t genv; /* the guest's JNIEnv* of the main thread */
    uint64_t vm;   /* the guest's JavaVM* */
    char app_class[128]; /* a class in the app's class loader alone, its internal name */
    int failures;
} g_t;

static void expect_int(const char* what, int64_t got, int64_t want) {
    if (got != want) {
        fprintf(stderr, "android_jni: %s: got %lld (0x%llx), want %lld (0x%llx)\n", what, (long long)got,
                (unsigned long long)got, (long long)want, (unsigned long long)want);
        g_t.failures++;
    }
}
static void expect_text(const char* what, const char* got, const char* want) {
    if (strcmp(got, want)) {
        fprintf(stderr, "android_jni: %s: got \"%s\", want \"%s\"\n", what, got, want);
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

/* the class name of the exception pending on the guest's genv, cleared, or "none" */
static void pending(uint64_t genv, JNIEnv* env, char* out, size_t size) {
    jthrowable t = (jthrowable)(uintptr_t)jni(genv, EXCEPTION_OCCURRED, 0, 0, 0, 0);
    if (!t) {
        snprintf(out, size, "none");
        return;
    }
    jni(genv, EXCEPTION_CLEAR, 0, 0, 0, 0);
    jclass of = (*env)->GetObjectClass(env, t);
    jclass class_class = (*env)->FindClass(env, "java/lang/Class");
    jmethodID get_name = (*env)->GetMethodID(env, class_class, "getName", "()Ljava/lang/String;");
    jstring name = (*env)->CallObjectMethod(env, of, get_name);
    const char* text = name ? (*env)->GetStringUTFChars(env, name, NULL) : NULL;
    snprintf(out, size, "%s", text ? text : "?");
    if (text) (*env)->ReleaseStringUTFChars(env, name, text);
}

/* ---- the version ---- */

static void test_version(void) {
    expect_int("GetVersion", (int64_t)jni(g_t.genv, GET_VERSION, 0, 0, 0, 0), JNI_VERSION_1_6);
}

/* ---- FindClass ---- */

/* the app's class loader (snx.android.app.App's), with a class in it alone: a proxy class it defines */
static void make_app_class(JNIEnv* env) {
    jclass url = (*env)->FindClass(env, "java/net/URL");
    jclass url_loader = (*env)->FindClass(env, "java/net/URLClassLoader");
    jclass loader_class = (*env)->FindClass(env, "java/lang/ClassLoader");
    jclass proxy = (*env)->FindClass(env, "java/lang/reflect/Proxy");
    jclass class_class = (*env)->FindClass(env, "java/lang/Class");
    jclass app = (*env)->FindClass(env, "snx/android/app/App");
    if (!url || !url_loader || !loader_class || !proxy || !class_class || !app)
        stop("a class of the JDK or App is missing");
    jobject system = (*env)->CallStaticObjectMethod(
        env, loader_class,
        (*env)->GetStaticMethodID(env, loader_class, "getSystemClassLoader", "()Ljava/lang/ClassLoader;"));
    jobject loader = (*env)->NewObject(
        env, url_loader, (*env)->GetMethodID(env, url_loader, "<init>", "([Ljava/net/URL;Ljava/lang/ClassLoader;)V"),
        (*env)->NewObjectArray(env, 0, url, NULL), system);
    jobjectArray interfaces = (*env)->NewObjectArray(env, 1, class_class, (*env)->FindClass(env, "java/lang/Runnable"));
    jobject made = (*env)->CallStaticObjectMethod(
        env, proxy,
        (*env)->GetStaticMethodID(env, proxy, "getProxyClass",
                                  "(Ljava/lang/ClassLoader;[Ljava/lang/Class;)Ljava/lang/Class;"),
        loader, interfaces);
    if (!made) stop("no proxy class");
    jstring name =
        (*env)->CallObjectMethod(env, made, (*env)->GetMethodID(env, class_class, "getName", "()Ljava/lang/String;"));
    const char* text = (*env)->GetStringUTFChars(env, name, NULL);
    snprintf(g_t.app_class, sizeof g_t.app_class, "%s", text);
    (*env)->ReleaseStringUTFChars(env, name, text);
    for (char* p = g_t.app_class; *p; ++p)
        if (*p == '.') *p = '/';
    (*env)->CallStaticVoidMethod(
        env, app,
        (*env)->GetStaticMethodID(
            env, app, "set",
            "(Ljava/lang/ClassLoader;Landroid/app/AppComponentFactory;Landroid/content/res/Resources;)V"),
        loader, NULL, NULL);
    if ((*env)->ExceptionCheck(env)) stop("App.set threw");
}

/* FindClass of the app's class and of the framework's on a guest JNIEnv: "found" or the exception's class */
static void find_both(uint64_t genv, JNIEnv* env, char* out, size_t size) {
    char app[128], framework[128];
    if (jni(genv, FIND_CLASS, P(g_t.app_class), 0, 0, 0))
        snprintf(app, sizeof app, "found");
    else
        pending(genv, env, app, sizeof app);
    if (jni(genv, FIND_CLASS, P("android/app/Activity"), 0, 0, 0))
        snprintf(framework, sizeof framework, "found");
    else
        pending(genv, env, framework, sizeof framework);
    snprintf(out, size, "app's %s, framework's %s", app, framework);
}

/* a thread of the app's own, attached through the guest's JavaVM */
static void* attached(void* out) {
    uint64_t table, genv = 0;
    memcpy(&table, (const void*)(uintptr_t)g_t.vm, sizeof table);
    uint64_t attach, detach;
    memcpy(&attach, (const void*)(uintptr_t)(table + 8 * ATTACH_CURRENT_THREAD), sizeof attach);
    memcpy(&detach, (const void*)(uintptr_t)(table + 8 * DETACH_CURRENT_THREAD), sizeof detach);
    const uint64_t args[3] = {g_t.vm, P(&genv), 0};
    if (bridge_call(attach, args, 3)) {
        snprintf(out, 128, "AttachCurrentThread failed");
        return NULL;
    }
    find_both(genv, bridge_jni_host(genv), out, 128);
    bridge_call(detach, args, 1);
    bridge_thread_end();
    return NULL;
}

/* the app's native method that FindClass runs in: an export the guest's RegisterNatives binds */
static void native_find(CPU* c, void* data) {
    (void)data;
    c->x[0] = jni(c->x[0], FIND_CLASS, P(g_t.app_class), 0, 0, 0);
}

/* the app's class is found from a native method of the app (as from JNI_OnLoad), the system class loader's alone
   elsewhere: on a thread the app attached, and on one where no native method of the app runs. ART throws
   ClassNotFoundException for the app's class there */
static void test_find_class(void) {
    make_app_class(g_t.env);
    char got[128];
    find_both(g_t.genv, g_t.env, got, sizeof got);
    expect_text("FindClass with no native method of the app", got,
                "app's java.lang.ClassNotFoundException, framework's found");
    pthread_t t;
    if (pthread_create(&t, NULL, attached, got)) stop("no thread");
    pthread_join(t, NULL);
    expect_text("FindClass on a thread the app attached", got,
                "app's java.lang.ClassNotFoundException, framework's found");

    /* a native method of the app, bound by the guest's RegisterNatives and called as the translator's code calls it */
    JNIEnv* env = g_t.env;
    jclass object = (*env)->FindClass(env, "java/lang/Object");
    const uint64_t method[3] = {P("findAppClass"), P("()Ljava/lang/Class;"),
                                bridge_export("android_jni::find", native_find, NULL)};
    expect_int("RegisterNatives", (int64_t)jni(g_t.genv, REGISTER_NATIVES, P(object), P(method), 1, 0), 0);
    jclass bridge = (*env)->FindClass(env, "snx/android/nativebridge/NativeBridge");
    jmethodID invoke = (*env)->GetStaticMethodID(
        env, bridge, "invokeL", "(Ljava/lang/String;Ljava/lang/Object;[J[Ljava/lang/Object;)Ljava/lang/Object;");
    jstring key = (*env)->NewStringUTF(env, "Ljava/lang/Object;.findAppClass()Ljava/lang/Class;");
    jobject found = (*env)->CallStaticObjectMethod(env, bridge, invoke, key, object, NULL, NULL);
    if ((*env)->ExceptionCheck(env)) {
        android_jvm_exception(env);
        found = NULL;
    }
    expect_int("FindClass in a native method of the app finds the app's class", found != NULL, 1);
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
    jni(g_t.genv, GET_JAVA_VM, P(&g_t.vm), 0, 0, 0);

    test_version();
    test_find_class();
    if (g_t.failures) {
        fprintf(stderr, "android_jni: %d failed\n", g_t.failures);
        return 1;
    }
    puts("android_jni: all passed");
    return 0;
}
