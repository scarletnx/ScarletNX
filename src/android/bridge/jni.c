/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* JNI between the JVM and the guest's libraries (docs/android/NATIVE.md, "JNI"). The guest's JNIEnv and JavaVM are
   tables of export addresses: each calls the host JVM's own function with the thread's host JNIEnv, and references
   and IDs pass as they are, a guest pointer being a host pointer. The calls with variable arguments read them by the
   method's signature, which GetMethodID recorded. Java calls the guest through NativeBridge.invoke*, which the
   translator makes each native method of the app call */
#define _GNU_SOURCE
#include "android/bridge/args.h"
#include "android/bridge/bridge.h"
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"
#include "android/bridge/jni.h"
#include "android/bridge/fault.h"
#include "android/bridge/linker.h"
#include "android/bridge/native.h"
#include "android/runtime/jvm.h"
#include "common/utilities/bits.h"
#include "common/utilities/common.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the JNINativeInterface's functions, by index (jni.h, JNI 1.6 and Android's), the kind of each one's result
   (hle_call's), and whether it takes variable arguments (... or a va_list) */
typedef struct {
    const char* name;
    char kind;
    unsigned char variadic;
} JniFunction;
enum { JNI_FUNCTIONS = 233 };
static const JniFunction functions[JNI_FUNCTIONS] = {
    {"reserved", 'V', 0}, {"reserved", 'V', 0}, {"reserved", 'V', 0}, {"reserved", 'V', 0}, {"GetVersion", 'I', 0},
    {"DefineClass", 'I', 0}, {"FindClass", 'I', 0}, {"FromReflectedMethod", 'I', 0}, {"FromReflectedField", 'I', 0},
    {"ToReflectedMethod", 'I', 0}, {"GetSuperclass", 'I', 0}, {"IsAssignableFrom", 'I', 0},
    {"ToReflectedField", 'I', 0}, {"Throw", 'I', 0}, {"ThrowNew", 'I', 0}, {"ExceptionOccurred", 'I', 0},
    {"ExceptionDescribe", 'V', 0}, {"ExceptionClear", 'V', 0}, {"FatalError", 'V', 0}, {"PushLocalFrame", 'I', 0},
    {"PopLocalFrame", 'I', 0}, {"NewGlobalRef", 'I', 0}, {"DeleteGlobalRef", 'V', 0}, {"DeleteLocalRef", 'V', 0},
    {"IsSameObject", 'I', 0}, {"NewLocalRef", 'I', 0}, {"EnsureLocalCapacity", 'I', 0}, {"AllocObject", 'I', 0},
    {"NewObject", 'I', 1}, {"NewObjectV", 'I', 1}, {"NewObjectA", 'I', 0}, {"GetObjectClass", 'I', 0},
    {"IsInstanceOf", 'I', 0}, {"GetMethodID", 'I', 0}, {"CallObjectMethod", 'I', 1}, {"CallObjectMethodV", 'I', 1},
    {"CallObjectMethodA", 'I', 0}, {"CallBooleanMethod", 'I', 1}, {"CallBooleanMethodV", 'I', 1},
    {"CallBooleanMethodA", 'I', 0}, {"CallByteMethod", 'I', 1}, {"CallByteMethodV", 'I', 1},
    {"CallByteMethodA", 'I', 0}, {"CallCharMethod", 'I', 1}, {"CallCharMethodV", 'I', 1}, {"CallCharMethodA", 'I', 0},
    {"CallShortMethod", 'I', 1}, {"CallShortMethodV", 'I', 1}, {"CallShortMethodA", 'I', 0},
    {"CallIntMethod", 'I', 1}, {"CallIntMethodV", 'I', 1}, {"CallIntMethodA", 'I', 0}, {"CallLongMethod", 'I', 1},
    {"CallLongMethodV", 'I', 1}, {"CallLongMethodA", 'I', 0}, {"CallFloatMethod", 'F', 1},
    {"CallFloatMethodV", 'F', 1}, {"CallFloatMethodA", 'F', 0}, {"CallDoubleMethod", 'D', 1},
    {"CallDoubleMethodV", 'D', 1}, {"CallDoubleMethodA", 'D', 0}, {"CallVoidMethod", 'V', 1},
    {"CallVoidMethodV", 'V', 1}, {"CallVoidMethodA", 'V', 0}, {"CallNonvirtualObjectMethod", 'I', 1},
    {"CallNonvirtualObjectMethodV", 'I', 1}, {"CallNonvirtualObjectMethodA", 'I', 0},
    {"CallNonvirtualBooleanMethod", 'I', 1}, {"CallNonvirtualBooleanMethodV", 'I', 1},
    {"CallNonvirtualBooleanMethodA", 'I', 0}, {"CallNonvirtualByteMethod", 'I', 1},
    {"CallNonvirtualByteMethodV", 'I', 1}, {"CallNonvirtualByteMethodA", 'I', 0},
    {"CallNonvirtualCharMethod", 'I', 1}, {"CallNonvirtualCharMethodV", 'I', 1},
    {"CallNonvirtualCharMethodA", 'I', 0}, {"CallNonvirtualShortMethod", 'I', 1},
    {"CallNonvirtualShortMethodV", 'I', 1}, {"CallNonvirtualShortMethodA", 'I', 0},
    {"CallNonvirtualIntMethod", 'I', 1}, {"CallNonvirtualIntMethodV", 'I', 1}, {"CallNonvirtualIntMethodA", 'I', 0},
    {"CallNonvirtualLongMethod", 'I', 1}, {"CallNonvirtualLongMethodV", 'I', 1},
    {"CallNonvirtualLongMethodA", 'I', 0}, {"CallNonvirtualFloatMethod", 'F', 1},
    {"CallNonvirtualFloatMethodV", 'F', 1}, {"CallNonvirtualFloatMethodA", 'F', 0},
    {"CallNonvirtualDoubleMethod", 'D', 1}, {"CallNonvirtualDoubleMethodV", 'D', 1},
    {"CallNonvirtualDoubleMethodA", 'D', 0}, {"CallNonvirtualVoidMethod", 'V', 1},
    {"CallNonvirtualVoidMethodV", 'V', 1}, {"CallNonvirtualVoidMethodA", 'V', 0}, {"GetFieldID", 'I', 0},
    {"GetObjectField", 'I', 0}, {"GetBooleanField", 'I', 0}, {"GetByteField", 'I', 0}, {"GetCharField", 'I', 0},
    {"GetShortField", 'I', 0}, {"GetIntField", 'I', 0}, {"GetLongField", 'I', 0}, {"GetFloatField", 'F', 0},
    {"GetDoubleField", 'D', 0}, {"SetObjectField", 'V', 0}, {"SetBooleanField", 'V', 0}, {"SetByteField", 'V', 0},
    {"SetCharField", 'V', 0}, {"SetShortField", 'V', 0}, {"SetIntField", 'V', 0}, {"SetLongField", 'V', 0},
    {"SetFloatField", 'V', 0}, {"SetDoubleField", 'V', 0}, {"GetStaticMethodID", 'I', 0},
    {"CallStaticObjectMethod", 'I', 1}, {"CallStaticObjectMethodV", 'I', 1}, {"CallStaticObjectMethodA", 'I', 0},
    {"CallStaticBooleanMethod", 'I', 1}, {"CallStaticBooleanMethodV", 'I', 1}, {"CallStaticBooleanMethodA", 'I', 0},
    {"CallStaticByteMethod", 'I', 1}, {"CallStaticByteMethodV", 'I', 1}, {"CallStaticByteMethodA", 'I', 0},
    {"CallStaticCharMethod", 'I', 1}, {"CallStaticCharMethodV", 'I', 1}, {"CallStaticCharMethodA", 'I', 0},
    {"CallStaticShortMethod", 'I', 1}, {"CallStaticShortMethodV", 'I', 1}, {"CallStaticShortMethodA", 'I', 0},
    {"CallStaticIntMethod", 'I', 1}, {"CallStaticIntMethodV", 'I', 1}, {"CallStaticIntMethodA", 'I', 0},
    {"CallStaticLongMethod", 'I', 1}, {"CallStaticLongMethodV", 'I', 1}, {"CallStaticLongMethodA", 'I', 0},
    {"CallStaticFloatMethod", 'F', 1}, {"CallStaticFloatMethodV", 'F', 1}, {"CallStaticFloatMethodA", 'F', 0},
    {"CallStaticDoubleMethod", 'D', 1}, {"CallStaticDoubleMethodV", 'D', 1}, {"CallStaticDoubleMethodA", 'D', 0},
    {"CallStaticVoidMethod", 'V', 1}, {"CallStaticVoidMethodV", 'V', 1}, {"CallStaticVoidMethodA", 'V', 0},
    {"GetStaticFieldID", 'I', 0}, {"GetStaticObjectField", 'I', 0}, {"GetStaticBooleanField", 'I', 0},
    {"GetStaticByteField", 'I', 0}, {"GetStaticCharField", 'I', 0}, {"GetStaticShortField", 'I', 0},
    {"GetStaticIntField", 'I', 0}, {"GetStaticLongField", 'I', 0}, {"GetStaticFloatField", 'F', 0},
    {"GetStaticDoubleField", 'D', 0}, {"SetStaticObjectField", 'V', 0}, {"SetStaticBooleanField", 'V', 0},
    {"SetStaticByteField", 'V', 0}, {"SetStaticCharField", 'V', 0}, {"SetStaticShortField", 'V', 0},
    {"SetStaticIntField", 'V', 0}, {"SetStaticLongField", 'V', 0}, {"SetStaticFloatField", 'V', 0},
    {"SetStaticDoubleField", 'V', 0}, {"NewString", 'I', 0}, {"GetStringLength", 'I', 0}, {"GetStringChars", 'I', 0},
    {"ReleaseStringChars", 'V', 0}, {"NewStringUTF", 'I', 0}, {"GetStringUTFLength", 'I', 0},
    {"GetStringUTFChars", 'I', 0}, {"ReleaseStringUTFChars", 'V', 0}, {"GetArrayLength", 'I', 0},
    {"NewObjectArray", 'I', 0}, {"GetObjectArrayElement", 'I', 0}, {"SetObjectArrayElement", 'V', 0},
    {"NewBooleanArray", 'I', 0}, {"NewByteArray", 'I', 0}, {"NewCharArray", 'I', 0}, {"NewShortArray", 'I', 0},
    {"NewIntArray", 'I', 0}, {"NewLongArray", 'I', 0}, {"NewFloatArray", 'I', 0}, {"NewDoubleArray", 'I', 0},
    {"GetBooleanArrayElements", 'I', 0}, {"GetByteArrayElements", 'I', 0}, {"GetCharArrayElements", 'I', 0},
    {"GetShortArrayElements", 'I', 0}, {"GetIntArrayElements", 'I', 0}, {"GetLongArrayElements", 'I', 0},
    {"GetFloatArrayElements", 'I', 0}, {"GetDoubleArrayElements", 'I', 0}, {"ReleaseBooleanArrayElements", 'V', 0},
    {"ReleaseByteArrayElements", 'V', 0}, {"ReleaseCharArrayElements", 'V', 0}, {"ReleaseShortArrayElements", 'V', 0},
    {"ReleaseIntArrayElements", 'V', 0}, {"ReleaseLongArrayElements", 'V', 0}, {"ReleaseFloatArrayElements", 'V', 0},
    {"ReleaseDoubleArrayElements", 'V', 0}, {"GetBooleanArrayRegion", 'V', 0}, {"GetByteArrayRegion", 'V', 0},
    {"GetCharArrayRegion", 'V', 0}, {"GetShortArrayRegion", 'V', 0}, {"GetIntArrayRegion", 'V', 0},
    {"GetLongArrayRegion", 'V', 0}, {"GetFloatArrayRegion", 'V', 0}, {"GetDoubleArrayRegion", 'V', 0},
    {"SetBooleanArrayRegion", 'V', 0}, {"SetByteArrayRegion", 'V', 0}, {"SetCharArrayRegion", 'V', 0},
    {"SetShortArrayRegion", 'V', 0}, {"SetIntArrayRegion", 'V', 0}, {"SetLongArrayRegion", 'V', 0},
    {"SetFloatArrayRegion", 'V', 0}, {"SetDoubleArrayRegion", 'V', 0}, {"RegisterNatives", 'I', 0},
    {"UnregisterNatives", 'I', 0}, {"MonitorEnter", 'I', 0}, {"MonitorExit", 'I', 0}, {"GetJavaVM", 'I', 0},
    {"GetStringRegion", 'V', 0}, {"GetStringUTFRegion", 'V', 0}, {"GetPrimitiveArrayCritical", 'I', 0},
    {"ReleasePrimitiveArrayCritical", 'V', 0}, {"GetStringCritical", 'I', 0}, {"ReleaseStringCritical", 'V', 0},
    {"NewWeakGlobalRef", 'I', 0}, {"DeleteWeakGlobalRef", 'V', 0}, {"ExceptionCheck", 'I', 0},
    {"NewDirectByteBuffer", 'I', 0}, {"GetDirectBufferAddress", 'I', 0}, {"GetDirectBufferCapacity", 'I', 0},
    {"GetObjectRefType", 'I', 0},
};

/* the guest's JNIEnv of a thread: its table, and the host's JNIEnv it stands for */
typedef struct {
    uint64_t table;
    JNIEnv* host;
} GuestEnv;
/* the guest's JavaVM */
typedef struct {
    uint64_t table;
} GuestVm;

static struct {
    JavaVM* vm;
    uint64_t env_table[JNI_FUNCTIONS];
    uint64_t vm_table[8];
    GuestVm guest_vm;
    pthread_mutex_t lock;
    /* the parameters of each method ID (one letter each: ZBCSIJFDL), open addressing by the ID */
    struct Method {
        jmethodID id;
        char* params;
    }* methods;
    size_t method_capacity, method_count;
    /* the guest's native methods: "Lpkg/Class;.name(signature)" to the guest function */
    struct Native {
        char* key;
        uint64_t fn;
    }* natives;
    size_t native_capacity, native_count;
    bool started;
} j = {.lock = PTHREAD_MUTEX_INITIALIZER};

static _Thread_local GuestEnv* t_env;
/* the app's native methods under way on this thread (NativeBridge's invoke), and the loads of its libraries (their
   constructors and JNI_OnLoad): FindClass looks in the app's class loader within them, as ART's in the loader of the
   class whose native method runs. A thread with none, one the app attached, has the system class loader alone */
static _Thread_local unsigned t_app_calls;

/* ---- the guest's JNIEnv of this thread ---- */

static GuestEnv* guest_env(JNIEnv* host) {
    if (!t_env) {
        t_env = snx_alloc(sizeof *t_env);
        t_env->table = (uint64_t)(uintptr_t)j.env_table;
    }
    t_env->host = host;
    return t_env;
}
static JNIEnv* host_env(uint64_t guest) {
    return ((const GuestEnv*)(uintptr_t)guest)->host;
}
JNIEnv* bridge_jni_host(uint64_t guest_env) {
    return host_env(guest_env);
}
uint64_t bridge_jni_env(JNIEnv* host) { return (uint64_t)(uintptr_t)guest_env(host); }

/* ---- method signatures ---- */

/* a signature's parameters as one letter each, and its result's letter */
static char* params_of(const char* sig, char* result) {
    size_t n = strlen(sig);
    char* out = snx_alloc(n + 1);
    size_t k = 0;
    const char* p = sig[0] == '(' ? sig + 1 : sig;
    while (*p && *p != ')') {
        char t = *p;
        while (*p == '[') ++p;
        if (*p == 'L') {
            p = strchr(p, ';');
            if (!p) break;
        }
        ++p;
        out[k++] = t == '[' ? 'L' : t;
    }
    out[k] = 0;
    if (result) *result = p && *p == ')' ? (p[1] == '[' ? 'L' : p[1]) : 'V';
    return out;
}

static size_t method_slot(jmethodID id) {
    size_t mask = j.method_capacity - 1, i = (size_t)(((uintptr_t)id >> 3) * 0x9e3779b97f4a7c15ull) & mask;
    while (j.methods[i].id && j.methods[i].id != id) i = (i + 1) & mask;
    return i;
}
static void record_method(jmethodID id, const char* sig) {
    if (!id || !sig) return;
    pthread_mutex_lock(&j.lock);
    if ((j.method_count + 1) * 2 > j.method_capacity) {
        struct Method* old = j.methods;
        size_t capacity = j.method_capacity;
        j.method_capacity = capacity ? capacity * 2 : 4096;
        j.methods = calloc(j.method_capacity, sizeof *j.methods);
        for (size_t i = 0; i < capacity; ++i)
            if (old[i].id) j.methods[method_slot(old[i].id)] = old[i];
        free(old);
    }
    size_t i = method_slot(id);
    if (!j.methods[i].id) {
        j.methods[i].id = id;
        j.methods[i].params = params_of(sig, NULL);
        ++j.method_count;
    }
    pthread_mutex_unlock(&j.lock);
}
static const char* method_params(jmethodID id) {
    pthread_mutex_lock(&j.lock);
    const char* p = j.method_capacity ? j.methods[method_slot(id)].params : NULL;
    pthread_mutex_unlock(&j.lock);
    return p;
}

/* ---- the JNIEnv functions ---- */

/* the parameters of a JNIEnv function with a floating one, for hle_call (Windows passes by position): the four
   Set<Type>Field functions whose value is a jfloat or a jdouble. The others take integers and pointers alone */
static const char* jni_params(const char* name) {
    size_t n = strlen(name);
    bool set = !strncmp(name, "Set", 3);
    if (set && n > 10 && !strcmp(name + n - 10, "FloatField")) return "IIIF";
    if (set && n > 11 && !strcmp(name + n - 11, "DoubleField")) return "IIID";
    return NULL;
}

/* GetVersion: Android's JNI, 1.6, where the host's JVM gives its own (JDK 25's is 24) */
static void jni_get_version(CPU* c, void* data) {
    (void)data;
    X(0) = JNI_VERSION_1_6;
}

static void jni_generic(CPU* c, void* data) {
    unsigned i = (unsigned)(uintptr_t)data;
    JNIEnv* env = host_env(X(0));
    X(0) = (uint64_t)(uintptr_t)env;
    hle_call(c, ((void**)*env)[i], functions[i].kind, jni_params(functions[i].name));
}

/* GetMethodID and GetStaticMethodID: the ID's signature recorded for the calls with variable arguments */
/* Class.forName(String) loads with its caller's loader, which for a JNI call is the loader of the native method that
   runs: on Android the app's own class, here NativeBridge's (the framework's). The guest gets
   snx.dalvik.Runtime.forName(String) in its place, which loads with the app's loader, by GetStaticMethodID or by
   FromReflectedMethod of the Method (as Unity's ReflectionHelper finds it) */
static jmethodID remap_method(JNIEnv* env, jmethodID id) {
    static jmethodID for_name, replacement;
    if (!id) return id;
    if (!__atomic_load_n(&replacement, __ATOMIC_ACQUIRE)) {
        jclass class_class = (*env)->FindClass(env, "java/lang/Class");
        jclass runtime = (*env)->FindClass(env, "snx/dalvik/Runtime");
        const char* sig = "(Ljava/lang/String;)Ljava/lang/Class;";
        jmethodID a = class_class ? (*env)->GetStaticMethodID(env, class_class, "forName", sig) : NULL;
        jmethodID b = runtime ? (*env)->GetStaticMethodID(env, runtime, "forName", sig) : NULL;
        if (!a || !b) {
            (*env)->ExceptionClear(env);
            return id;
        }
        __atomic_store_n(&for_name, a, __ATOMIC_RELAXED);
        __atomic_store_n(&replacement, b, __ATOMIC_RELEASE);
    }
    return id == __atomic_load_n(&for_name, __ATOMIC_RELAXED) ? replacement : id;
}

/* GetMethodID and GetStaticMethodID: the ID's signature recorded for the calls with variable arguments */
static void jni_get_method(CPU* c, void* data) {
    const char* sig = STR(3);
    uint64_t env_arg = X(0);
    jni_generic(c, data);
    X(0) = (uint64_t)(uintptr_t)remap_method(host_env(env_arg), (jmethodID)(uintptr_t)X(0));
    record_method((jmethodID)(uintptr_t)X(0), sig);
}

/* FromReflectedMethod: the ID of a Method or Constructor, remapped as GetStaticMethodID's. Its signature is recorded
   from the object, for the calls with variable arguments */
static void jni_from_reflected(CPU* c, void* data) {
    uint64_t env_arg = X(0);
    JNIEnv* env = host_env(env_arg);
    jobject method = (jobject)(uintptr_t)X(1);
    jni_generic(c, data);
    jmethodID id = remap_method(env, (jmethodID)(uintptr_t)X(0));
    X(0) = (uint64_t)(uintptr_t)id;
    if (id && !method_params(id)) {
        /* the JNI signature of the reflected method, from snx.dalvik.Runtime.signature(Executable) */
        jclass runtime = (*env)->FindClass(env, "snx/dalvik/Runtime");
        jmethodID sig_of = runtime ? (*env)->GetStaticMethodID(env, runtime, "signature",
                                                                "(Ljava/lang/reflect/Executable;)Ljava/lang/String;")
                                   : NULL;
        jstring sig = sig_of ? (*env)->CallStaticObjectMethod(env, runtime, sig_of, method) : NULL;
        const char* chars = sig ? (*env)->GetStringUTFChars(env, sig, NULL) : NULL;
        if (chars) {
            record_method(id, chars);
            (*env)->ReleaseStringUTFChars(env, sig, chars);
        }
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }
}

/* Call<Type>Method, CallNonvirtual..., CallStatic... and NewObject with ... or a va_list: the arguments read by the
   method's signature into jvalues, then the A form, which is two entries (...) or one (V) after */
static void jni_variadic(CPU* c, void* data) {
    unsigned i = (unsigned)(uintptr_t)data;
    const char* name = functions[i].name;
    bool va = name[strlen(name) - 1] == 'V';
    bool nonvirtual = !strncmp(name, "CallNonvirtual", 14);
    unsigned id_at = nonvirtual ? 3 : 2;
    jmethodID id = (jmethodID)(uintptr_t)X(id_at);
    const char* params = method_params(id);
    if (!params) {
        fprintf(stderr, "scarletnx-android: JNI %s of a method ID the bridge didn't see made\n", name);
        abort();
    }
    GuestArgs a;
    if (va) args_from_va_list(&a, X(id_at + 1));
    else args_from_registers(&a, c, id_at + 1, 0);
    size_t n = strlen(params);
    jvalue* values = alloca((n ? n : 1) * sizeof *values);
    for (size_t k = 0; k < n; ++k) {
        switch (params[k]) {
        case 'Z': values[k].z = (jboolean)args_int(&a); break;
        case 'B': values[k].b = (jbyte)args_int(&a); break;
        case 'C': values[k].c = (jchar)args_int(&a); break;
        case 'S': values[k].s = (jshort)args_int(&a); break;
        case 'I': values[k].i = (jint)args_int(&a); break;
        case 'J': values[k].j = (jlong)args_int(&a); break;
        case 'F': values[k].f = (jfloat)args_double(&a); break; /* a variadic float comes as a double */
        case 'D': values[k].d = args_double(&a); break;
        default: values[k].l = (jobject)(uintptr_t)args_int(&a); break;
        }
    }
    JNIEnv* env = host_env(X(0));
    X(0) = (uint64_t)(uintptr_t)env;
    X(id_at + 1) = (uint64_t)(uintptr_t)values;
    unsigned a_form = i + (va ? 1 : 2);
    hle_call(c, ((void**)*env)[a_form], functions[a_form].kind, NULL);
}

/* FindClass: the host's, which looks in the loader of the native method that runs (the framework's here), then within
   the app's native methods the app's class loader, as Android's looks in the loader of the class that loaded the
   library. Elsewhere ART's system class loader, which has no class of the app, throws ClassNotFoundException where
   the host's JVM throws NoClassDefFoundError */
static void jni_find_class(CPU* c, void* data) {
    (void)data;
    JNIEnv* env = host_env(X(0));
    const char* name = STR(1);
    jclass found = (*env)->FindClass(env, name);
    if (!found && (*env)->ExceptionCheck(env) && !t_app_calls) {
        (*env)->ExceptionClear(env);
        char* dotted = snx_format("%s", name);
        for (char* p = dotted; *p; ++p)
            if (*p == '/') *p = '.';
        char* text = snx_format("Didn't find class \"%s\" in the system class loader (no native method of the app "
                                "runs on this thread)",
                                dotted);
        jclass not_found = (*env)->FindClass(env, "java/lang/ClassNotFoundException");
        if (not_found) (*env)->ThrowNew(env, not_found, text);
        free(text);
        free(dotted);
    } else if (!found && (*env)->ExceptionCheck(env)) {
        jthrowable pending = (*env)->ExceptionOccurred(env);
        (*env)->ExceptionClear(env);
        jclass app = (*env)->FindClass(env, "snx/android/app/App");
        jmethodID loader_of = app ? (*env)->GetStaticMethodID(env, app, "classLoader", "()Ljava/lang/ClassLoader;") : NULL;
        jobject loader = loader_of ? (*env)->CallStaticObjectMethod(env, app, loader_of) : NULL;
        jclass class_class = (*env)->FindClass(env, "java/lang/Class");
        jmethodID for_name = (*env)->GetStaticMethodID(env, class_class, "forName",
                                                       "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
        char* dotted = snx_format("%s", name);
        for (char* p = dotted; *p; ++p)
            if (*p == '/') *p = '.';
        jstring s = (*env)->NewStringUTF(env, dotted);
        free(dotted);
        found = loader && s ? (*env)->CallStaticObjectMethod(env, class_class, for_name, s, JNI_FALSE, loader) : NULL;
        if (!found) {
            /* the first failure is the one the guest sees */
            (*env)->ExceptionClear(env);
            (*env)->Throw(env, pending);
        }
    }
    X(0) = (uint64_t)(uintptr_t)found;
}

/* the name of a class ("java/lang/String") through the host's JNI, the caller's to free */
static char* class_name(JNIEnv* env, jclass clazz) {
    jclass class_class = (*env)->FindClass(env, "java/lang/Class");
    jmethodID get_name = (*env)->GetMethodID(env, class_class, "getName", "()Ljava/lang/String;");
    jstring s = (*env)->CallObjectMethod(env, clazz, get_name);
    const char* text = s ? (*env)->GetStringUTFChars(env, s, NULL) : NULL;
    char* out = text ? snx_format("%s", text) : NULL;
    if (text) (*env)->ReleaseStringUTFChars(env, s, text);
    for (char* p = out; p && *p; ++p)
        if (*p == '.') *p = '/';
    return out;
}

static void add_native(const char* key, uint64_t fn) {
    pthread_mutex_lock(&j.lock);
    for (size_t i = 0; i < j.native_count; ++i)
        if (!strcmp(j.natives[i].key, key)) {
            j.natives[i].fn = fn;
            pthread_mutex_unlock(&j.lock);
            return;
        }
    if (j.native_count == j.native_capacity) {
        j.native_capacity = j.native_capacity ? j.native_capacity * 2 : 256;
        j.natives = snx_realloc(j.natives, j.native_capacity * sizeof *j.natives);
    }
    j.natives[j.native_count].key = snx_format("%s", key);
    j.natives[j.native_count++].fn = fn;
    pthread_mutex_unlock(&j.lock);
}

/* RegisterNatives(env, clazz, methods, count): JNINativeMethod is { name, signature, fnPtr } on both */
static void jni_register_natives(CPU* c, void* data) {
    (void)data;
    JNIEnv* env = host_env(X(0));
    char* cls = class_name(env, (jclass)(uintptr_t)X(1));
    const uint64_t* m = PTR(2);
    for (int64_t k = 0; cls && k < (int32_t)X(3); ++k) {
        char* key = snx_format("L%s;.%s%s", cls, (const char*)(uintptr_t)m[3 * k], (const char*)(uintptr_t)m[3 * k + 1]);
        add_native(key, m[3 * k + 2]);
        free(key);
    }
    free(cls);
    X(0) = 0;
}
static void jni_unregister_natives(CPU* c, void* data) {
    (void)data;
    X(0) = 0;
}
static void jni_get_java_vm(CPU* c, void* data) {
    (void)data;
    uint64_t vm = (uint64_t)(uintptr_t)&j.guest_vm;
    memcpy(PTR(1), &vm, sizeof vm);
    X(0) = 0;
}

/* ---- strings in ART's modified UTF-8, where HotSpot's is CESU-8: a supplementary character (a surrogate pair) is
   UTF-8's 4 bytes, not 6, and NUL C0 80 and a lone surrogate its 3 bytes in both ---- */

/* n UTF-16 units as ART's modified UTF-8 into out (none: counted): its length, without a NUL */
static size_t art_utf8(const jchar* units, size_t n, unsigned char* out) {
    size_t at = 0;
    for (size_t i = 0; i < n; ++i) {
        uint32_t u = units[i];
        if (u >= 0xd800 && u < 0xdc00 && i + 1 < n && units[i + 1] >= 0xdc00 && units[i + 1] < 0xe000) {
            uint32_t cp = 0x10000 + ((u - 0xd800) << 10) + (units[++i] - 0xdc00u);
            if (out) {
                out[at] = (unsigned char)(0xf0 | cp >> 18);
                out[at + 1] = (unsigned char)(0x80 | (cp >> 12 & 0x3f));
                out[at + 2] = (unsigned char)(0x80 | (cp >> 6 & 0x3f));
                out[at + 3] = (unsigned char)(0x80 | (cp & 0x3f));
            }
            at += 4;
        } else if (u && u < 0x80) {
            if (out) out[at] = (unsigned char)u;
            at += 1;
        } else if (u < 0x800) {
            if (out) {
                out[at] = (unsigned char)(0xc0 | u >> 6);
                out[at + 1] = (unsigned char)(0x80 | (u & 0x3f));
            }
            at += 2;
        } else {
            if (out) {
                out[at] = (unsigned char)(0xe0 | u >> 12);
                out[at + 1] = (unsigned char)(0x80 | (u >> 6 & 0x3f));
                out[at + 2] = (unsigned char)(0x80 | (u & 0x3f));
            }
            at += 3;
        }
    }
    return at;
}
/* NewStringUTF(env, bytes): a 4-byte sequence a surrogate pair, as ART reads it, where HotSpot's ends the string at
   its second byte */
static void jni_new_string_utf(CPU* c, void* data) {
    (void)data;
    JNIEnv* env = host_env(X(0));
    const unsigned char* b = PTR(1);
    if (!b) {
        X(0) = 0;
        return;
    }
    size_t n = strlen((const char*)b), k = 0;
    jchar* units = malloc((n ? n : 1) * sizeof *units); /* a unit for each byte at most */
    if (!units) {
        jclass oom = (*env)->FindClass(env, "java/lang/OutOfMemoryError");
        if (oom) (*env)->ThrowNew(env, oom, "NewStringUTF: no memory for the string's characters");
        X(0) = 0;
        return;
    }
    for (size_t i = 0; i < n;) {
        uint32_t one = b[i++];
        if (!(one & 0x80)) {
            units[k++] = (jchar)one;
            continue;
        }
        uint32_t two = i < n ? b[i++] & 0x3fu : 0;
        if (!(one & 0x20)) {
            units[k++] = (jchar)((one & 0x1f) << 6 | two);
            continue;
        }
        uint32_t three = i < n ? b[i++] & 0x3fu : 0;
        if (!(one & 0x10)) {
            units[k++] = (jchar)((one & 0x0f) << 12 | two << 6 | three);
            continue;
        }
        uint32_t four = i < n ? b[i++] & 0x3fu : 0;
        uint32_t cp = (one & 0x07) << 18 | two << 12 | three << 6 | four;
        units[k++] = (jchar)((cp >> 10) + 0xd7c0);
        units[k++] = (jchar)((cp & 0x3ff) + 0xdc00);
    }
    X(0) = (uint64_t)(uintptr_t)(*env)->NewString(env, units, (jsize)k);
    free(units);
}
/* GetStringUTFLength(env, string) */
static void jni_get_string_utf_length(CPU* c, void* data) {
    (void)data;
    JNIEnv* env = host_env(X(0));
    jstring s = (jstring)(uintptr_t)X(1);
    jsize n = (*env)->GetStringLength(env, s);
    const jchar* units = (*env)->GetStringChars(env, s, NULL);
    X(0) = units ? art_utf8(units, (size_t)n, NULL) : 0;
    if (units) (*env)->ReleaseStringChars(env, s, units);
}
/* GetStringUTFChars(env, string, is_copy): a copy of the bridge's, which ReleaseStringUTFChars frees */
static void jni_get_string_utf_chars(CPU* c, void* data) {
    (void)data;
    JNIEnv* env = host_env(X(0));
    jstring s = (jstring)(uintptr_t)X(1);
    jsize n = (*env)->GetStringLength(env, s);
    const jchar* units = (*env)->GetStringChars(env, s, NULL);
    unsigned char* out = units ? malloc(art_utf8(units, (size_t)n, NULL) + 1) : NULL;
    if (out) out[art_utf8(units, (size_t)n, out)] = 0;
    if (units) (*env)->ReleaseStringChars(env, s, units);
    if (units && !out) {
        jclass oom = (*env)->FindClass(env, "java/lang/OutOfMemoryError");
        if (oom) (*env)->ThrowNew(env, oom, "GetStringUTFChars: no memory for the string's bytes");
    }
    if (out && X(2)) *(jboolean*)PTR(2) = JNI_TRUE;
    X(0) = (uint64_t)(uintptr_t)out;
}
static void jni_release_string_utf_chars(CPU* c, void* data) {
    (void)data;
    free(PTR(2));
}
/* GetStringUTFRegion(env, string, start, length, buffer): with a NUL after, as HotSpot's */
static void jni_get_string_utf_region(CPU* c, void* data) {
    (void)data;
    JNIEnv* env = host_env(X(0));
    jsize start = (jsize)X(2), length = (jsize)X(3);
    jchar* units = malloc((length > 0 ? (size_t)length : 1) * sizeof *units);
    if (!units) {
        jclass oom = (*env)->FindClass(env, "java/lang/OutOfMemoryError");
        if (oom) (*env)->ThrowNew(env, oom, "GetStringUTFRegion: no memory for the string's characters");
        return;
    }
    (*env)->GetStringRegion(env, (jstring)(uintptr_t)X(1), start, length, units);
    if (!(*env)->ExceptionCheck(env)) {
        unsigned char* out = PTR(4);
        out[art_utf8(units, (size_t)length, out)] = 0;
    }
    free(units);
}

/* ---- the JavaVM functions ---- */

static void vm_destroy(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(int64_t)JNI_ERR;
}
static void vm_attach(CPU* c, void* data) {
    bool daemon = data != NULL;
    JNIEnv* env;
    jint r = daemon ? (*j.vm)->AttachCurrentThreadAsDaemon(j.vm, (void**)&env, PTR(2))
                    : (*j.vm)->AttachCurrentThread(j.vm, (void**)&env, PTR(2));
    if (r == JNI_OK) {
        uint64_t g = (uint64_t)(uintptr_t)guest_env(env);
        memcpy(PTR(1), &g, sizeof g);
    }
    X(0) = (uint64_t)(int64_t)r;
}
static void vm_detach(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(int64_t)(*j.vm)->DetachCurrentThread(j.vm);
}
static void vm_get_env(CPU* c, void* data) {
    (void)data;
    JNIEnv* env;
    jint r = (*j.vm)->GetEnv(j.vm, (void**)&env, (jint)X(2));
    if (r == JNI_OK) {
        uint64_t g = (uint64_t)(uintptr_t)guest_env(env);
        memcpy(PTR(1), &g, sizeof g);
    }
    X(0) = (uint64_t)(int64_t)r;
}

static void make_tables(void) {
    for (unsigned i = 0; i < JNI_FUNCTIONS; ++i) {
        const char* name = functions[i].name;
        BridgeHandler h = functions[i].variadic ? jni_variadic : jni_generic;
        if (!strcmp(name, "GetMethodID") || !strcmp(name, "GetStaticMethodID")) h = jni_get_method;
        else if (!strcmp(name, "FromReflectedMethod")) h = jni_from_reflected;
        else if (!strcmp(name, "FindClass")) h = jni_find_class;
        else if (!strcmp(name, "RegisterNatives")) h = jni_register_natives;
        else if (!strcmp(name, "UnregisterNatives")) h = jni_unregister_natives;
        else if (!strcmp(name, "GetJavaVM")) h = jni_get_java_vm;
        if (!strcmp(name, "GetVersion")) h = jni_get_version;
        if (!strcmp(name, "NewStringUTF")) h = jni_new_string_utf;
        if (!strcmp(name, "GetStringUTFLength")) h = jni_get_string_utf_length;
        if (!strcmp(name, "GetStringUTFChars")) h = jni_get_string_utf_chars;
        if (!strcmp(name, "ReleaseStringUTFChars")) h = jni_release_string_utf_chars;
        if (!strcmp(name, "GetStringUTFRegion")) h = jni_get_string_utf_region;
        char* export = snx_format("JNIEnv::%s#%u", name, i);
        j.env_table[i] = i < 4 ? 0 : bridge_export(export, h, (void*)(uintptr_t)i);
        free(export);
    }
    j.vm_table[3] = bridge_export("JavaVM::DestroyJavaVM", vm_destroy, NULL);
    j.vm_table[4] = bridge_export("JavaVM::AttachCurrentThread", vm_attach, NULL);
    j.vm_table[5] = bridge_export("JavaVM::DetachCurrentThread", vm_detach, NULL);
    j.vm_table[6] = bridge_export("JavaVM::GetEnv", vm_get_env, NULL);
    j.vm_table[7] = bridge_export("JavaVM::AttachCurrentThreadAsDaemon", vm_attach, (void*)1);
    j.guest_vm.table = (uint64_t)(uintptr_t)j.vm_table;
}

/* ---- NativeBridge's natives ---- */

/* the JNI name of a method, short (Java_pkg_Class_name) or long (with __ and the parameters) */
static void mangle(char* out, size_t size, size_t* at, const char* s, const char* end) {
    for (; s < end && *s && *at + 8 < size; ++s) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '/') out[(*at)++] = '_';
        else if (ch == '_') *at += (size_t)snprintf(out + *at, size - *at, "_1");
        else if (ch == ';') *at += (size_t)snprintf(out + *at, size - *at, "_2");
        else if (ch == '[') *at += (size_t)snprintf(out + *at, size - *at, "_3");
        else if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) out[(*at)++] = (char)ch;
        else *at += (size_t)snprintf(out + *at, size - *at, "_0%04x", ch);
    }
    out[*at] = 0;
}
/* the guest function of a native method's key, "Lpkg/Class;.name(signature)": registered, or found by its JNI name */
static uint64_t native_function(const char* key) {
    pthread_mutex_lock(&j.lock);
    for (size_t i = 0; i < j.native_count; ++i)
        if (!strcmp(j.natives[i].key, key)) {
            uint64_t fn = j.natives[i].fn;
            pthread_mutex_unlock(&j.lock);
            return fn;
        }
    pthread_mutex_unlock(&j.lock);
    const char* dot = strstr(key, ";.");
    const char* paren = dot ? strchr(dot, '(') : NULL;
    if (!dot || !paren) return 0;
    char name[2048];
    size_t at = (size_t)snprintf(name, sizeof name, "Java_");
    mangle(name, sizeof name, &at, key + 1, dot);
    name[at++] = '_';
    mangle(name, sizeof name, &at, dot + 2, paren);
    uint64_t fn = linker_symbol(NULL, name);
    if (!fn) {
        at += (size_t)snprintf(name + at, sizeof name - at, "__");
        mangle(name, sizeof name, &at, paren + 1, strchr(paren, ')'));
        fn = linker_symbol(NULL, name);
    }
    if (fn) add_native(key, fn);
    return fn;
}

static bool start(JNIEnv* env, jstring library_dir) {
    if (j.started) return true;
    (*env)->GetJavaVM(env, &j.vm);
    const char* dir = library_dir ? (*env)->GetStringUTFChars(env, library_dir, NULL) : NULL;
    char error[512];
    bool ok = android_native_start(dir, error, sizeof error);
    if (dir) (*env)->ReleaseStringUTFChars(env, library_dir, dir);
    if (!ok) {
        fprintf(stderr, "scarletnx-android: the native bridge doesn't start: %s\n", error);
        return false;
    }
    make_tables();
    j.started = true;
    return true;
}

/* nativeLoad(path, libraryDir): null when the library loaded and its JNI_OnLoad ran, else dlerror's text */
static jstring JNICALL native_load(JNIEnv* env, jclass unused, jstring path, jstring library_dir) {
    (void)unused;
    if (!start(env, library_dir)) return (*env)->NewStringUTF(env, "dlopen failed: the native bridge doesn't start");
    const char* p = (*env)->GetStringUTFChars(env, path, NULL);
    char error[1024];
    guest_env(env);
    /* the JVM's handlers are in place by now: the guest's crash report goes before them */
    fault_install();
    ++t_app_calls;
    GuestLibrary* l = linker_load(p, error, sizeof error);
    --t_app_calls;
    (*env)->ReleaseStringUTFChars(env, path, p);
    if (!l) {
        char text[1100];
        snprintf(text, sizeof text, "dlopen failed: %s", error);
        return (*env)->NewStringUTF(env, text);
    }
    uint64_t onload = 0;
    {
        /* JNI_OnLoad of this library itself */
        uint64_t a = linker_symbol(l, "JNI_OnLoad");
        GuestLibrary* owner = a ? linker_owner(a) : NULL;
        onload = owner == l ? a : 0;
    }
    if (onload) {
        uint64_t args[2] = {(uint64_t)(uintptr_t)&j.guest_vm, 0};
        ++t_app_calls;
        jint version = (jint)bridge_call(onload, args, 2);
        --t_app_calls;
        if ((*env)->ExceptionCheck(env)) return NULL;
        if (version != JNI_VERSION_1_2 && version != JNI_VERSION_1_4 && version != JNI_VERSION_1_6) {
            char text[256];
            snprintf(text, sizeof text, "JNI_ERR returned from JNI_OnLoad in \"%s\"", linker_name(l));
            return (*env)->NewStringUTF(env, text);
        }
    }
    return NULL;
}

/* invoke(key, self, prims, refs): the native method's guest function called with the JNIEnv, self (the object, or
   the class of a static method) and its parameters as AAPCS64 passes them. prims holds the primitive ones in order,
   each as its bits (a float's or a double's raw bits), and refs the references. The result as bits (invokeJ), a
   reference (invokeL) or none */
static uint64_t invoke(JNIEnv* env, jstring key, jobject self, jlongArray prims, jobjectArray refs, char* result) {
    const char* k = (*env)->GetStringUTFChars(env, key, NULL);
    uint64_t fn = native_function(k);
    char* params = params_of(strchr(k, '('), result);
    if (!fn) {
        char text[1200];
        snprintf(text, sizeof text, "No implementation found for %s", k);
        (*env)->ReleaseStringUTFChars(env, key, k);
        free(params);
        jclass error = (*env)->FindClass(env, "java/lang/UnsatisfiedLinkError");
        if (error) (*env)->ThrowNew(env, error, text);
        return 0;
    }
    (*env)->ReleaseStringUTFChars(env, key, k);
    CPU* c = bridge_cpu();
    size_t np = prims ? (size_t)(*env)->GetArrayLength(env, prims) : 0;
    jlong* pv = alloca((np ? np : 1) * sizeof *pv);
    if (np) (*env)->GetLongArrayRegion(env, prims, 0, (jsize)np, pv);
    /* the registers and the stack slots of the parameters, in AAPCS64's order */
    uint64_t saved[32];
    memcpy(saved, c->x, sizeof saved);
    V128 saved_v[8];
    memcpy(saved_v, c->v, sizeof saved_v);
    uint64_t stack[64];
    unsigned nx = 2, nv = 0, ns = 0;
    size_t pi = 0, ri = 0;
    c->x[0] = (uint64_t)(uintptr_t)guest_env(env);
    c->x[1] = (uint64_t)(uintptr_t)self;
    for (const char* p = params; *p && ns < 64; ++p) {
        uint64_t v;
        bool floating = *p == 'F' || *p == 'D';
        if (*p == 'L') v = (uint64_t)(uintptr_t)(*env)->GetObjectArrayElement(env, refs, (jsize)ri++);
        else v = (uint64_t)(pi < np ? pv[pi++] : 0);
        if (floating && nv < 8) {
            memset(&c->v[nv], 0, sizeof c->v[nv]);
            memcpy(&c->v[nv++].d[0], &v, 8);
        } else if (!floating && nx < 8) {
            c->x[nx++] = v;
        } else {
            stack[ns++] = v;
        }
    }
    free(params);
    uint64_t sp = c->x[31];
    if (ns) {
        c->x[31] = (sp - 8 * ns) & ~(uint64_t)15;
        memcpy((void*)(uintptr_t)c->x[31], stack, 8 * ns);
    }
    ++t_app_calls;
    bridge_call(fn, NULL, 0);
    --t_app_calls;
    c->x[31] = sp;
    uint64_t r = c->x[0];
    switch (*result) {
    case 'Z': r &= 0xff; r = r != 0; break;
    case 'B': r = (uint64_t)(int64_t)(int8_t)r; break;
    case 'C': r &= 0xffff; break;
    case 'S': r = (uint64_t)(int64_t)(int16_t)r; break;
    case 'I': r = (uint64_t)(int64_t)(int32_t)r; break;
    case 'F': r = c->v[0].s[0]; break;
    case 'D': r = c->v[0].d[0]; break;
    default: break;
    }
    memcpy(c->x, saved, sizeof saved);
    memcpy(c->v, saved_v, sizeof saved_v);
    return r;
}
static jlong JNICALL invoke_j(JNIEnv* env, jclass unused, jstring key, jobject self, jlongArray prims,
                              jobjectArray refs) {
    (void)unused;
    char result;
    return (jlong)invoke(env, key, self, prims, refs, &result);
}
static jobject JNICALL invoke_l(JNIEnv* env, jclass unused, jstring key, jobject self, jlongArray prims,
                                jobjectArray refs) {
    (void)unused;
    char result;
    return (jobject)(uintptr_t)invoke(env, key, self, prims, refs, &result);
}
static void JNICALL invoke_v(JNIEnv* env, jclass unused, jstring key, jobject self, jlongArray prims,
                             jobjectArray refs) {
    (void)unused;
    char result;
    invoke(env, key, self, prims, refs, &result);
}

bool android_bridge_register(JNIEnv* env) {
    static const JNINativeMethod methods[] = {
        {(char*)"nativeLoad", (char*)"(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", (void*)native_load},
        {(char*)"invokeJ", (char*)"(Ljava/lang/String;Ljava/lang/Object;[J[Ljava/lang/Object;)J", (void*)invoke_j},
        {(char*)"invokeL",
         (char*)"(Ljava/lang/String;Ljava/lang/Object;[J[Ljava/lang/Object;)Ljava/lang/Object;", (void*)invoke_l},
        {(char*)"invokeV", (char*)"(Ljava/lang/String;Ljava/lang/Object;[J[Ljava/lang/Object;)V", (void*)invoke_v},
    };
    return android_jvm_register(env, "snx/android/nativebridge/NativeBridge", methods,
                                (int)(sizeof methods / sizeof *methods));
}
