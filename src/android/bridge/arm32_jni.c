/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* JNI for the 32-bit bridge (arm32.h). The guest's JNIEnv and JavaVM are tables of 32-bit export addresses in its
   window. A reference or an ID of the host's JVM is 64 bits, so the guest gets a 32-bit handle for each: a local
   reference is an index into the thread's list, which a native method's return or PopLocalFrame shortens, a global or
   a weak one an index into a table, and a method or a field an index into the table of IDs, with the method's
   signature, by which the calls with variable arguments read them */
#define _GNU_SOURCE
#include "android/bridge/arm32.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define R(n) (c->r[n])

static uint32_t rd32(uint32_t a) { return snx_ld_le32(arm32_mem + a); }
static void wr32(uint32_t a, uint32_t v) { snx_st_le32(arm32_mem + a, v); }
static uint32_t word(const SnxArmCPU* c, unsigned n) { return n < 4 ? c->r[n] : rd32(c->r[13] + 4 * (n - 4)); }
static uint64_t word64(const SnxArmCPU* c, unsigned n) {
    n = (n + 1) & ~1u;
    return word(c, n) | (uint64_t)word(c, n + 1) << 32;
}

static struct {
    bool started;
    JavaVM* vm;
    uint32_t env, vm_guest; /* the guest's JNIEnv and JavaVM: each a word, the address of its table */
    pthread_mutex_t lock;   /* the globals, the IDs, the natives */
    jobject* globals;
    uint32_t global_count, global_capacity, global_free; /* the free slots linked through their index + 1 */
    struct {
        void* host;
        char* signature;
    }* ids;
    uint32_t id_count, id_capacity;
    struct {
        char* key;
        uint32_t fn;
    }* natives;
    uint32_t native_count, native_capacity;
    jmethodID class_get_name, for_name, signature_of, class_for_name;
    jclass runtime;
} j = {.lock = PTHREAD_MUTEX_INITIALIZER};

static _Thread_local JNIEnv* t_env;
static _Thread_local struct {
    jobject* items;
    uint32_t count, capacity;
} t_locals;
static _Thread_local struct {
    uint32_t* marks;
    uint32_t count, capacity;
} t_frames;

static _Thread_local bool t_attached;
/* the thread's host JNIEnv: a thread that the guest made is attached the first time, as a daemon, which the JVM's end
   does not wait for */
static JNIEnv* env_of(void) {
    if (!t_env && !(*j.vm)->AttachCurrentThreadAsDaemon(j.vm, (void**)&t_env, NULL)) t_attached = true;
    return t_env;
}
void arm32_jni_thread_end(void) {
    if (t_attached) (*j.vm)->DetachCurrentThread(j.vm);
    t_attached = false;
    t_env = NULL;
    t_locals.count = 0;
    t_frames.count = 0;
}

/* ---- references ---- */

enum { REF_LOCAL = 1, REF_GLOBAL = 2, REF_WEAK = 3 };
static jobject host_ref(uint32_t h) {
    if (!h) return NULL;
    uint32_t i = (h >> 2) - 1;
    if ((h & 3) == REF_LOCAL) return i < t_locals.count ? t_locals.items[i] : NULL;
    pthread_mutex_lock(&j.lock);
    jobject o = i < j.global_count ? j.globals[i] : NULL;
    pthread_mutex_unlock(&j.lock);
    return o;
}
static uint32_t local_ref(jobject o) {
    if (!o) return 0;
    if (t_locals.count == t_locals.capacity) {
        uint32_t n = t_locals.capacity ? t_locals.capacity * 2 : 256;
        jobject* items = realloc(t_locals.items, n * sizeof *items);
        if (!items) abort();
        t_locals.items = items;
        t_locals.capacity = n;
    }
    t_locals.items[t_locals.count++] = o;
    return t_locals.count << 2 | REF_LOCAL;
}
static uint32_t global_ref(jobject o, unsigned kind) {
    if (!o) return 0;
    pthread_mutex_lock(&j.lock);
    uint32_t i;
    if (j.global_free) {
        i = j.global_free - 1;
        j.global_free = (uint32_t)(uintptr_t)j.globals[i];
    } else {
        if (j.global_count == j.global_capacity) {
            uint32_t n = j.global_capacity ? j.global_capacity * 2 : 256;
            jobject* g = realloc(j.globals, n * sizeof *g);
            if (!g) abort();
            j.globals = g;
            j.global_capacity = n;
        }
        i = j.global_count++;
    }
    j.globals[i] = o;
    pthread_mutex_unlock(&j.lock);
    return (i + 1) << 2 | kind;
}
static jobject global_drop(uint32_t h) {
    uint32_t i = (h >> 2) - 1;
    pthread_mutex_lock(&j.lock);
    jobject o = NULL;
    if (i < j.global_count) {
        o = j.globals[i];
        j.globals[i] = (jobject)(uintptr_t)j.global_free;
        j.global_free = i + 1;
    }
    pthread_mutex_unlock(&j.lock);
    return o;
}

/* ---- IDs ---- */

static uint32_t id_of(void* host, const char* signature) {
    if (!host) return 0;
    pthread_mutex_lock(&j.lock);
    for (uint32_t i = 0; i < j.id_count; ++i)
        if (j.ids[i].host == host) {
            pthread_mutex_unlock(&j.lock);
            return i + 1;
        }
    if (j.id_count == j.id_capacity) {
        uint32_t n = j.id_capacity ? j.id_capacity * 2 : 256;
        void* ids = realloc(j.ids, n * sizeof *j.ids);
        if (!ids) abort();
        j.ids = ids;
        j.id_capacity = n;
    }
    j.ids[j.id_count].host = host;
    j.ids[j.id_count].signature = signature ? strdup(signature) : NULL;
    uint32_t id = ++j.id_count;
    pthread_mutex_unlock(&j.lock);
    return id;
}
static void* host_id(uint32_t id, const char** signature) {
    pthread_mutex_lock(&j.lock);
    void* host = id && id <= j.id_count ? j.ids[id - 1].host : NULL;
    if (signature) *signature = id && id <= j.id_count && j.ids[id - 1].signature ? j.ids[id - 1].signature : "()V";
    pthread_mutex_unlock(&j.lock);
    return host;
}

/* the next type of a signature's parameters (L for a reference), and the place past it */
static char next_type(const char** p) {
    const char* s = *p;
    if (!*s || *s == ')') return 0;
    if (*s == '[') {
        while (*s == '[') ++s;
        if (*s == 'L') s = strchr(s, ';');
        *p = s ? s + 1 : "";
        return 'L';
    }
    if (*s == 'L') {
        s = strchr(s, ';');
        *p = s ? s + 1 : "";
        return 'L';
    }
    *p = s + 1;
    return *s;
}

/* a call's arguments for the method of signature: from the guest's variable arguments (promoted: a float is a
   double), its va_list, or its jvalue array */
enum { ARGS_VARIADIC, ARGS_VA_LIST, ARGS_ARRAY };
static unsigned read_args(const char* signature, int mode, Arm32Args* a, uint32_t array, jvalue* out, unsigned max) {
    const char* p = strchr(signature, '(');
    p = p ? p + 1 : "";
    unsigned n = 0;
    for (char t; n < max && (t = next_type(&p)); ++n) {
        jvalue v = {0};
        if (mode == ARGS_ARRAY) {
            uint32_t at = array + 8 * n;
            uint64_t bits;
            memcpy(&bits, arm32_mem + at, 8);
            switch (t) {
            case 'Z': v.z = (jboolean)(bits & 0xff); break;
            case 'B': v.b = (jbyte)bits; break;
            case 'C': v.c = (jchar)bits; break;
            case 'S': v.s = (jshort)bits; break;
            case 'I': v.i = (jint)bits; break;
            case 'J': v.j = (jlong)bits; break;
            case 'F': memcpy(&v.f, &bits, 4); break;
            case 'D': memcpy(&v.d, &bits, 8); break;
            default: v.l = host_ref((uint32_t)bits); break;
            }
        } else {
            switch (t) {
            case 'Z': v.z = (jboolean)(arm32_arg(a) & 0xff); break;
            case 'B': v.b = (jbyte)arm32_arg(a); break;
            case 'C': v.c = (jchar)arm32_arg(a); break;
            case 'S': v.s = (jshort)arm32_arg(a); break;
            case 'I': v.i = (jint)arm32_arg(a); break;
            case 'J': v.j = (jlong)arm32_arg64(a); break;
            case 'F': {
                uint64_t bits = arm32_arg64(a);
                double d;
                memcpy(&d, &bits, 8);
                v.f = (jfloat)d;
                break;
            }
            case 'D': {
                uint64_t bits = arm32_arg64(a);
                memcpy(&v.d, &bits, 8);
                break;
            }
            default: v.l = host_ref(arm32_arg(a)); break;
            }
        }
        out[n] = v;
    }
    return n;
}

/* a result of type t into r0 (and r1) */
static void set_result(SnxArmCPU* c, char t, jvalue v) {
    switch (t) {
    case 'Z': R(0) = v.z ? 1 : 0; break;
    case 'B': R(0) = (uint32_t)(int32_t)v.b; break;
    case 'C': R(0) = v.c; break;
    case 'S': R(0) = (uint32_t)(int32_t)v.s; break;
    case 'I': R(0) = (uint32_t)v.i; break;
    case 'J':
        R(0) = (uint32_t)(uint64_t)v.j;
        R(1) = (uint32_t)((uint64_t)v.j >> 32);
        break;
    case 'F': memcpy(&R(0), &v.f, 4); break;
    case 'D': {
        uint64_t bits;
        memcpy(&bits, &v.d, 8);
        R(0) = (uint32_t)bits;
        R(1) = (uint32_t)(bits >> 32);
        break;
    }
    case 'L': R(0) = local_ref(v.l); break;
    default: break;
    }
}

/* ---- the calls of methods ---- */

static const char TYPES[] = "LZBCSIJFDV"; /* Object, Boolean, Byte, Char, Short, Int, Long, Float, Double, Void */
enum { CALL_VIRTUAL, CALL_NONVIRTUAL, CALL_STATIC, CALL_NEW };

static void call(SnxArmCPU* c, int kind, char t, int mode) {
    JNIEnv* e = env_of();
    unsigned mid_word = kind == CALL_NONVIRTUAL ? 3 : 2;
    const char* signature;
    jmethodID m = host_id(word(c, mid_word), &signature);
    jobject target = host_ref(R(1));
    jclass clazz = kind == CALL_NONVIRTUAL ? host_ref(R(2)) : NULL;
    jvalue args[64];
    Arm32Args a;
    uint32_t array = 0;
    if (mode == ARGS_VARIADIC) arm32_args(&a, c, mid_word + 1);
    else if (mode == ARGS_VA_LIST) arm32_args_va(&a, word(c, mid_word + 1));
    else array = word(c, mid_word + 1);
    read_args(signature, mode, &a, array, args, 64);
    jvalue r = {0};
    if (kind == CALL_NEW) {
        r.l = (*e)->NewObjectA(e, target, m, args);
        set_result(c, 'L', r);
        return;
    }
#define CALL_KIND(Type, field) \
    if (kind == CALL_VIRTUAL) field (*e)->Call##Type##MethodA(e, target, m, args); \
    else if (kind == CALL_NONVIRTUAL) field (*e)->CallNonvirtual##Type##MethodA(e, target, clazz, m, args); \
    else field (*e)->CallStatic##Type##MethodA(e, target, m, args);
    switch (t) {
    case 'L': CALL_KIND(Object, r.l =) break;
    case 'Z': CALL_KIND(Boolean, r.z =) break;
    case 'B': CALL_KIND(Byte, r.b =) break;
    case 'C': CALL_KIND(Char, r.c =) break;
    case 'S': CALL_KIND(Short, r.s =) break;
    case 'I': CALL_KIND(Int, r.i =) break;
    case 'J': CALL_KIND(Long, r.j =) break;
    case 'F': CALL_KIND(Float, r.f =) break;
    case 'D': CALL_KIND(Double, r.d =) break;
    default: CALL_KIND(Void, ) break;
    }
#undef CALL_KIND
    set_result(c, t, r);
}

/* ---- fields ---- */

static void get_field(SnxArmCPU* c, bool is_static, char t) {
    JNIEnv* e = env_of();
    jobject o = host_ref(R(1));
    jfieldID f = host_id(R(2), NULL);
    jvalue r = {0};
#define GET(Type, field) \
    field is_static ? (*e)->GetStatic##Type##Field(e, o, f) : (*e)->Get##Type##Field(e, o, f);
    switch (t) {
    case 'L': GET(Object, r.l =) break;
    case 'Z': GET(Boolean, r.z =) break;
    case 'B': GET(Byte, r.b =) break;
    case 'C': GET(Char, r.c =) break;
    case 'S': GET(Short, r.s =) break;
    case 'I': GET(Int, r.i =) break;
    case 'J': GET(Long, r.j =) break;
    case 'F': GET(Float, r.f =) break;
    default: GET(Double, r.d =) break;
    }
#undef GET
    set_result(c, t, r);
}
static void set_field(SnxArmCPU* c, bool is_static, char t) {
    JNIEnv* e = env_of();
    jobject o = host_ref(R(1));
    jfieldID f = host_id(R(2), NULL);
    uint32_t w = R(3);
    uint64_t wide = word64(c, 3);
#define SET(Type, value) \
    if (is_static) (*e)->SetStatic##Type##Field(e, o, f, value); \
    else (*e)->Set##Type##Field(e, o, f, value);
    switch (t) {
    case 'L': SET(Object, host_ref(w)) break;
    case 'Z': SET(Boolean, (jboolean)(w & 0xff)) break;
    case 'B': SET(Byte, (jbyte)w) break;
    case 'C': SET(Char, (jchar)w) break;
    case 'S': SET(Short, (jshort)w) break;
    case 'I': SET(Int, (jint)w) break;
    case 'J': SET(Long, (jlong)wide) break;
    case 'F': {
        jfloat v;
        memcpy(&v, &w, 4);
        SET(Float, v)
        break;
    }
    default: {
        jdouble v;
        memcpy(&v, &wide, 8);
        SET(Double, v)
        break;
    }
    }
#undef SET
}

/* ---- arrays ---- */

static const char PRIMS[] = "ZBCSIJFD";
static unsigned prim_size(char t) { return t == 'Z' || t == 'B' ? 1 : t == 'C' || t == 'S' ? 2 : t == 'J' || t == 'D' ? 8 : 4; }

static void region(JNIEnv* e, char t, bool set, jarray a, jsize start, jsize n, void* buffer) {
#define REGION(Type, ctype) \
    if (set) (*e)->Set##Type##ArrayRegion(e, a, start, n, (const ctype*)buffer); \
    else (*e)->Get##Type##ArrayRegion(e, a, start, n, (ctype*)buffer);
    switch (t) {
    case 'Z': REGION(Boolean, jboolean) break;
    case 'B': REGION(Byte, jbyte) break;
    case 'C': REGION(Char, jchar) break;
    case 'S': REGION(Short, jshort) break;
    case 'I': REGION(Int, jint) break;
    case 'J': REGION(Long, jlong) break;
    case 'F': REGION(Float, jfloat) break;
    default: REGION(Double, jdouble) break;
    }
#undef REGION
}
static jarray new_array(JNIEnv* e, char t, jsize n) {
    switch (t) {
    case 'Z': return (*e)->NewBooleanArray(e, n);
    case 'B': return (*e)->NewByteArray(e, n);
    case 'C': return (*e)->NewCharArray(e, n);
    case 'S': return (*e)->NewShortArray(e, n);
    case 'I': return (*e)->NewIntArray(e, n);
    case 'J': return (*e)->NewLongArray(e, n);
    case 'F': return (*e)->NewFloatArray(e, n);
    default: return (*e)->NewDoubleArray(e, n);
    }
}
/* the guest's copy of an array's elements */
static uint32_t elements(JNIEnv* e, char t, jarray a, uint32_t is_copy) {
    jsize n = (*e)->GetArrayLength(e, a);
    uint32_t p = arm32_malloc((uint32_t)n * prim_size(t) + 8);
    if (p) region(e, t, false, a, 0, n, arm32_mem + p);
    if (is_copy) arm32_mem[is_copy] = 1;
    return p;
}
static void release_elements(JNIEnv* e, char t, jarray a, uint32_t p, uint32_t mode) {
    if (!p) return;
    if (mode != 2 /* JNI_ABORT */) region(e, t, true, a, 0, (*e)->GetArrayLength(e, a), arm32_mem + p);
    if (mode != 1 /* JNI_COMMIT */) arm32_free(p);
}
/* the type of a primitive array's elements, by its class's name ("[I") */
static char array_type(JNIEnv* e, jarray a) {
    jclass cls = (*e)->GetObjectClass(e, a);
    jstring name = cls ? (*e)->CallObjectMethod(e, cls, j.class_get_name) : NULL;
    const char* s = name ? (*e)->GetStringUTFChars(e, name, NULL) : NULL;
    char t = s && s[0] == '[' && s[1] ? s[1] : 'B';
    if (s) (*e)->ReleaseStringUTFChars(e, name, s);
    if (name) (*e)->DeleteLocalRef(e, name);
    if (cls) (*e)->DeleteLocalRef(e, cls);
    return t;
}
/* the critical copies a thread holds, with their types: ReleasePrimitiveArrayCritical gives no type */
static _Thread_local struct {
    uint32_t address;
    char type;
} t_critical[16];

/* ---- natives ---- */

static char* class_name(JNIEnv* e, jclass cls) {
    jstring name = (*e)->CallObjectMethod(e, cls, j.class_get_name);
    const char* s = name ? (*e)->GetStringUTFChars(e, name, NULL) : NULL;
    char* copy = s ? strdup(s) : NULL;
    if (s) (*e)->ReleaseStringUTFChars(e, name, s);
    if (name) (*e)->DeleteLocalRef(e, name);
    for (char* p = copy; p && *p; ++p)
        if (*p == '.') *p = '/';
    return copy;
}
static void add_native(const char* key, uint32_t fn) {
    pthread_mutex_lock(&j.lock);
    for (uint32_t i = 0; i < j.native_count; ++i)
        if (!strcmp(j.natives[i].key, key)) {
            j.natives[i].fn = fn;
            pthread_mutex_unlock(&j.lock);
            return;
        }
    if (j.native_count == j.native_capacity) {
        uint32_t n = j.native_capacity ? j.native_capacity * 2 : 64;
        void* natives = realloc(j.natives, n * sizeof *j.natives);
        if (!natives) abort();
        j.natives = natives;
        j.native_capacity = n;
    }
    j.natives[j.native_count].key = strdup(key);
    j.natives[j.native_count++].fn = fn;
    pthread_mutex_unlock(&j.lock);
}

/* Class.forName(String) loads with its caller's loader, which for a JNI call is the loader of the native method that
   runs: on Android the app's own class, here NativeBridge's (the framework's). The guest gets
   snx.dalvik.Runtime.forName(String) in its place, which loads with the app's loader, as jni.c gives it */
static jmethodID remap_method(jmethodID m) { return m && m == j.class_for_name && j.for_name ? j.for_name : m; }

/* ---- the JNIEnv's functions ---- */

static const char* const names[] = {
    [5] = "DefineClass", [7] = "FromReflectedMethod", [8] = "FromReflectedField", [9] = "ToReflectedMethod",
    [12] = "ToReflectedField", [18] = "FatalError"};

static void unimplemented(SnxArmCPU* c, unsigned i) {
    fprintf(stderr, "scarletnx-android: arm32: JNI function %u (%s) is not there yet\n", i,
            i < sizeof names / sizeof *names && names[i] ? names[i] : "?");
    R(0) = 0;
}

static void jni_function(SnxArmCPU* c, void* data) {
    unsigned i = (unsigned)(uintptr_t)data;
    JNIEnv* e = env_of();
    if (i >= 34 && i <= 63) {
        call(c, CALL_VIRTUAL, TYPES[(i - 34) / 3], (int)((i - 34) % 3));
        return;
    }
    if (i >= 64 && i <= 93) {
        call(c, CALL_NONVIRTUAL, TYPES[(i - 64) / 3], (int)((i - 64) % 3));
        return;
    }
    if (i >= 114 && i <= 143) {
        call(c, CALL_STATIC, TYPES[(i - 114) / 3], (int)((i - 114) % 3));
        return;
    }
    if (i >= 28 && i <= 30) {
        call(c, CALL_NEW, 'L', (int)(i - 28));
        return;
    }
    if (i >= 95 && i <= 103) return get_field(c, false, TYPES[i - 95]);
    if (i >= 104 && i <= 112) return set_field(c, false, TYPES[i - 104]);
    if (i >= 145 && i <= 153) return get_field(c, true, TYPES[i - 145]);
    if (i >= 154 && i <= 162) return set_field(c, true, TYPES[i - 154]);
    if (i >= 175 && i <= 182) {
        R(0) = local_ref(new_array(e, PRIMS[i - 175], (jsize)R(1)));
        return;
    }
    if (i >= 183 && i <= 190) {
        R(0) = elements(e, PRIMS[i - 183], host_ref(R(1)), R(2));
        return;
    }
    if (i >= 191 && i <= 198) {
        release_elements(e, PRIMS[i - 191], host_ref(R(1)), R(2), R(3));
        return;
    }
    if (i >= 199 && i <= 214) {
        bool set = i >= 207;
        uint32_t buffer = word(c, 4);
        region(e, PRIMS[(i - 199) % 8], set, host_ref(R(1)), (jsize)R(2), (jsize)R(3), arm32_host(buffer));
        return;
    }
    switch (i) {
    case 4: R(0) = JNI_VERSION_1_6; break;
    case 6: { /* FindClass: the app's classes by its loader when the JVM's own finds none */
        const char* name = (const char*)arm32_host(R(1));
        jclass cls = (*e)->FindClass(e, name);
        if (!cls) {
            (*e)->ExceptionClear(e);
            char* dotted = strdup(name);
            for (char* p = dotted; p && *p; ++p)
                if (*p == '/') *p = '.';
            jstring s = dotted ? (*e)->NewStringUTF(e, dotted) : NULL;
            free(dotted);
            cls = s ? (*e)->CallStaticObjectMethod(e, j.runtime, j.for_name, s) : NULL;
            if (s) (*e)->DeleteLocalRef(e, s);
            if (!cls && (*e)->ExceptionCheck(e)) {
                /* NoClassDefFoundError, as Android's FindClass throws */
                (*e)->ExceptionClear(e);
                jclass error = (*e)->FindClass(e, "java/lang/NoClassDefFoundError");
                if (error) (*e)->ThrowNew(e, error, name);
            }
        }
        R(0) = local_ref(cls);
        break;
    }
    case 7: { /* FromReflectedMethod: its signature from the method itself */
        jobject method = host_ref(R(1));
        jstring sig = (*e)->CallStaticObjectMethod(e, j.runtime, j.signature_of, method);
        const char* s = sig ? (*e)->GetStringUTFChars(e, sig, NULL) : NULL;
        R(0) = id_of(remap_method((*e)->FromReflectedMethod(e, method)), s);
        if (s) (*e)->ReleaseStringUTFChars(e, sig, s);
        break;
    }
    case 8: R(0) = id_of((*e)->FromReflectedField(e, host_ref(R(1))), NULL); break;
    case 9: R(0) = local_ref((*e)->ToReflectedMethod(e, host_ref(R(1)), host_id(R(2), NULL), (jboolean)R(3))); break;
    case 10: R(0) = local_ref((*e)->GetSuperclass(e, host_ref(R(1)))); break;
    case 11: R(0) = (*e)->IsAssignableFrom(e, host_ref(R(1)), host_ref(R(2))); break;
    case 12: R(0) = local_ref((*e)->ToReflectedField(e, host_ref(R(1)), host_id(R(2), NULL), (jboolean)R(3))); break;
    case 13: R(0) = (uint32_t)(*e)->Throw(e, host_ref(R(1))); break;
    case 14: R(0) = (uint32_t)(*e)->ThrowNew(e, host_ref(R(1)), (const char*)arm32_host(R(2))); break;
    case 15: R(0) = local_ref((*e)->ExceptionOccurred(e)); break;
    case 16: (*e)->ExceptionDescribe(e); break;
    case 17: (*e)->ExceptionClear(e); break;
    case 18:
        fprintf(stderr, "scarletnx-android: arm32: FatalError: %s\n", (const char*)arm32_host(R(1)));
        abort();
    case 19: /* PushLocalFrame */
        if (t_frames.count == t_frames.capacity) {
            uint32_t n = t_frames.capacity ? t_frames.capacity * 2 : 16;
            uint32_t* marks = realloc(t_frames.marks, n * sizeof *marks);
            if (!marks) abort();
            t_frames.marks = marks;
            t_frames.capacity = n;
        }
        t_frames.marks[t_frames.count++] = t_locals.count;
        R(0) = (uint32_t)(*e)->PushLocalFrame(e, (jint)R(1));
        break;
    case 20: { /* PopLocalFrame(result) */
        jobject result = host_ref(R(1));
        if (t_frames.count) t_locals.count = t_frames.marks[--t_frames.count];
        R(0) = local_ref((*e)->PopLocalFrame(e, result));
        break;
    }
    case 21: R(0) = global_ref((*e)->NewGlobalRef(e, host_ref(R(1))), REF_GLOBAL); break;
    case 22:
        if (R(1) && (R(1) & 3) == REF_GLOBAL) (*e)->DeleteGlobalRef(e, global_drop(R(1)));
        break;
    case 23:
        if (R(1) && (R(1) & 3) == REF_LOCAL) {
            uint32_t k = (R(1) >> 2) - 1;
            if (k < t_locals.count && t_locals.items[k]) {
                (*e)->DeleteLocalRef(e, t_locals.items[k]);
                t_locals.items[k] = NULL;
                if (k + 1 == t_locals.count) --t_locals.count;
            }
        }
        break;
    case 24: R(0) = (*e)->IsSameObject(e, host_ref(R(1)), host_ref(R(2))); break;
    case 25: R(0) = local_ref((*e)->NewLocalRef(e, host_ref(R(1)))); break;
    case 26: R(0) = (uint32_t)(*e)->EnsureLocalCapacity(e, (jint)R(1)); break;
    case 27: R(0) = local_ref((*e)->AllocObject(e, host_ref(R(1)))); break;
    case 31: R(0) = local_ref((*e)->GetObjectClass(e, host_ref(R(1)))); break;
    case 32: R(0) = (*e)->IsInstanceOf(e, host_ref(R(1)), host_ref(R(2))); break;
    case 33:
    case 113: {
        const char* name = (const char*)arm32_host(R(2));
        const char* sig = (const char*)arm32_host(R(3));
        jmethodID m = i == 33 ? (*e)->GetMethodID(e, host_ref(R(1)), name, sig)
                              : (*e)->GetStaticMethodID(e, host_ref(R(1)), name, sig);
        R(0) = id_of(remap_method(m), sig);
        break;
    }
    case 94:
    case 144: {
        const char* name = (const char*)arm32_host(R(2));
        const char* sig = (const char*)arm32_host(R(3));
        jfieldID f = i == 94 ? (*e)->GetFieldID(e, host_ref(R(1)), name, sig)
                             : (*e)->GetStaticFieldID(e, host_ref(R(1)), name, sig);
        R(0) = id_of(f, sig);
        break;
    }
    case 163: R(0) = local_ref((*e)->NewString(e, (const jchar*)arm32_host(R(1)), (jsize)R(2))); break;
    case 164: R(0) = (uint32_t)(*e)->GetStringLength(e, host_ref(R(1))); break;
    case 165: /* GetStringChars */
    case 224: { /* GetStringCritical */
        jstring s = host_ref(R(1));
        jsize n = (*e)->GetStringLength(e, s);
        uint32_t p = arm32_malloc((uint32_t)n * 2 + 2);
        if (p) {
            (*e)->GetStringRegion(e, s, 0, n, (jchar*)(arm32_mem + p));
            snx_st_le16(arm32_mem + p + 2 * (uint32_t)n, 0);
        }
        if (R(2)) arm32_mem[R(2)] = 1;
        R(0) = p;
        break;
    }
    case 166:
    case 170:
    case 225: arm32_free(R(2)); break;
    case 167: R(0) = local_ref((*e)->NewStringUTF(e, (const char*)arm32_host(R(1)))); break;
    case 168: R(0) = (uint32_t)(*e)->GetStringUTFLength(e, host_ref(R(1))); break;
    case 169: { /* GetStringUTFChars */
        jstring s = host_ref(R(1));
        const char* text = s ? (*e)->GetStringUTFChars(e, s, NULL) : NULL;
        R(0) = text ? arm32_strdup(text) : 0;
        if (text) (*e)->ReleaseStringUTFChars(e, s, text);
        if (R(2)) arm32_mem[R(2)] = 1;
        break;
    }
    case 171: R(0) = (uint32_t)(*e)->GetArrayLength(e, host_ref(R(1))); break;
    case 172: R(0) = local_ref((*e)->NewObjectArray(e, (jsize)R(1), host_ref(R(2)), host_ref(R(3)))); break;
    case 173: R(0) = local_ref((*e)->GetObjectArrayElement(e, host_ref(R(1)), (jsize)R(2))); break;
    case 174: (*e)->SetObjectArrayElement(e, host_ref(R(1)), (jsize)R(2), host_ref(R(3))); break;
    case 215: { /* RegisterNatives(clazz, methods, count): {name, signature, fn} each */
        char* cls = class_name(e, host_ref(R(1)));
        for (uint32_t k = 0; cls && k < R(3); ++k) {
            uint32_t m = R(2) + 12 * k;
            char* key = snx_format("L%s;.%s%s", cls, (const char*)arm32_host(rd32(m)),
                                   (const char*)arm32_host(rd32(m + 4)));
            if (key) add_native(key, rd32(m + 8));
            free(key);
        }
        free(cls);
        R(0) = 0;
        break;
    }
    case 216: R(0) = 0; break;
    case 217: R(0) = (uint32_t)(*e)->MonitorEnter(e, host_ref(R(1))); break;
    case 218: R(0) = (uint32_t)(*e)->MonitorExit(e, host_ref(R(1))); break;
    case 219:
        wr32(R(1), j.vm_guest);
        R(0) = 0;
        break;
    case 220: /* GetStringRegion(str, start, len, buf) */
        (*e)->GetStringRegion(e, host_ref(R(1)), (jsize)R(2), (jsize)R(3), (jchar*)arm32_host(word(c, 4)));
        break;
    case 221:
        (*e)->GetStringUTFRegion(e, host_ref(R(1)), (jsize)R(2), (jsize)R(3), (char*)arm32_host(word(c, 4)));
        break;
    case 222: { /* GetPrimitiveArrayCritical: a copy, as GetArrayElements */
        jarray a = host_ref(R(1));
        char t = array_type(e, a);
        uint32_t p = elements(e, t, a, R(2));
        for (unsigned k = 0; k < 16; ++k)
            if (!t_critical[k].address) {
                t_critical[k].address = p;
                t_critical[k].type = t;
                break;
            }
        R(0) = p;
        break;
    }
    case 223: { /* ReleasePrimitiveArrayCritical(array, carray, mode) */
        char t = 0;
        for (unsigned k = 0; k < 16; ++k)
            if (t_critical[k].address == R(2)) {
                t = t_critical[k].type;
                if (R(3) != 1) t_critical[k].address = 0;
                break;
            }
        if (!t) t = array_type(e, host_ref(R(1)));
        release_elements(e, t, host_ref(R(1)), R(2), R(3));
        break;
    }
    case 226: R(0) = global_ref((*e)->NewWeakGlobalRef(e, host_ref(R(1))), REF_WEAK); break;
    case 227:
        if (R(1) && (R(1) & 3) == REF_WEAK) (*e)->DeleteWeakGlobalRef(e, global_drop(R(1)));
        break;
    case 228: R(0) = (*e)->ExceptionCheck(e) ? 1 : 0; break;
    case 229: /* NewDirectByteBuffer(address, jlong capacity) */
        R(0) = local_ref((*e)->NewDirectByteBuffer(e, arm32_host(R(1)), (jlong)word64(c, 2)));
        break;
    case 230: { /* GetDirectBufferAddress: the guest sees only a buffer in its window */
        uint8_t* p = (*e)->GetDirectBufferAddress(e, host_ref(R(1)));
        if (p && (p < arm32_mem || p >= arm32_mem + (UINT64_C(1) << 32))) {
            fprintf(stderr, "scarletnx-android: arm32: GetDirectBufferAddress of a buffer outside the guest\n");
            p = NULL;
        }
        R(0) = arm32_guest(p);
        break;
    }
    case 231: {
        jlong n = (*e)->GetDirectBufferCapacity(e, host_ref(R(1)));
        R(0) = (uint32_t)n;
        R(1) = (uint32_t)((uint64_t)n >> 32);
        break;
    }
    case 232: R(0) = R(1) ? (R(1) & 3) : 0; break;
    default: unimplemented(c, i); break;
    }
}

/* ---- the JavaVM's functions ---- */

static void vm_function(SnxArmCPU* c, void* data) {
    unsigned i = (unsigned)(uintptr_t)data;
    switch (i) {
    case 3: R(0) = 0; break; /* DestroyJavaVM */
    case 4:                  /* AttachCurrentThread(vm, JNIEnv**, args) */
    case 7:
        env_of();
        if (R(1)) wr32(R(1), j.env);
        R(0) = 0;
        break;
    case 5: R(0) = 0; break; /* DetachCurrentThread: the thread's host JNIEnv stays */
    case 6:                  /* GetEnv(vm, void**, version) */
        env_of();
        if (R(1)) wr32(R(1), j.env);
        R(0) = 0;
        break;
    default: R(0) = (uint32_t)-1; break;
    }
}

static bool start(JNIEnv* e, const char* library_dir) {
    if (j.started) return true;
    char error[512];
    if (!arm32_init(error, sizeof error)) {
        fprintf(stderr, "scarletnx-android: the 32-bit bridge doesn't start: %s\n", error);
        return false;
    }
    arm32_set_library_dir(library_dir);
    (*e)->GetJavaVM(e, &j.vm);
    jclass class_class = (*e)->FindClass(e, "java/lang/Class");
    j.class_get_name = class_class ? (*e)->GetMethodID(e, class_class, "getName", "()Ljava/lang/String;") : NULL;
    j.class_for_name = class_class ? (*e)->GetStaticMethodID(e, class_class, "forName",
                                                            "(Ljava/lang/String;)Ljava/lang/Class;")
                                   : NULL;
    jclass runtime = (*e)->FindClass(e, "snx/dalvik/Runtime");
    j.runtime = runtime ? (*e)->NewGlobalRef(e, runtime) : NULL;
    j.for_name = runtime ? (*e)->GetStaticMethodID(e, runtime, "forName", "(Ljava/lang/String;)Ljava/lang/Class;")
                         : NULL;
    j.signature_of = runtime ? (*e)->GetStaticMethodID(e, runtime, "signature",
                                                       "(Ljava/lang/reflect/Executable;)Ljava/lang/String;")
                             : NULL;
    if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
    if (!j.class_get_name || !j.for_name) {
        fprintf(stderr, "scarletnx-android: the 32-bit bridge doesn't find Class.getName or Runtime.forName\n");
        return false;
    }
    /* the tables, in the guest's memory: 233 functions, and the JavaVM's 8 */
    uint32_t env_table = arm32_malloc(233 * 4), vm_table = arm32_malloc(8 * 4);
    for (unsigned i = 0; i < 233; ++i) {
        char name[48];
        snprintf(name, sizeof name, "JNIEnv#%u", i);
        wr32(env_table + 4 * i, i < 4 ? 0 : arm32_export(name, jni_function, (void*)(uintptr_t)i));
    }
    for (unsigned i = 0; i < 8; ++i) {
        char name[48];
        snprintf(name, sizeof name, "JavaVM#%u", i);
        wr32(vm_table + 4 * i, i < 3 ? 0 : arm32_export(name, vm_function, (void*)(uintptr_t)i));
    }
    j.env = arm32_malloc(4);
    j.vm_guest = arm32_malloc(4);
    wr32(j.env, env_table);
    wr32(j.vm_guest, vm_table);
    j.started = true;
    return true;
}

uint32_t arm32_jni_vm(void) { return j.vm_guest; }
jobject arm32_jni_ref(uint32_t handle) { return host_ref(handle); }

/* NativeActivity's glue. ANativeActivity on 32-bit ARM: callbacks, vm, env, clazz, internalDataPath,
   externalDataPath, sdkVersion, instance, assetManager, obbPath (40 bytes); its callbacks, 16 functions. The library
   sets the callbacks in the function that it is called with */
uint32_t arm32_jni_activity_create(JNIEnv* e, jobject activity, const char* function, const char* internal,
                                   const char* external, int sdk) {
    if (!arm32_active() || !j.started) return 0;
    uint32_t fn = arm32_symbol(NULL, function);
    if (!fn) return 0;
    uint32_t a = arm32_malloc(40), callbacks = arm32_malloc(64);
    if (!a || !callbacks) return 0;
    memset(arm32_mem + a, 0, 40);
    memset(arm32_mem + callbacks, 0, 64);
    JNIEnv* saved = t_env;
    t_env = e;
    wr32(a, callbacks);
    wr32(a + 4, j.vm_guest);
    wr32(a + 8, j.env);
    wr32(a + 12, global_ref((*e)->NewGlobalRef(e, activity), REF_GLOBAL));
    wr32(a + 16, internal ? arm32_strdup(internal) : 0);
    wr32(a + 20, external ? arm32_strdup(external) : 0);
    wr32(a + 24, (uint32_t)sdk);
    wr32(a + 36, external ? arm32_strdup(external) : 0);
    uint32_t mark = t_locals.count;
    uint32_t args[3] = {a, 0, 0};
    arm32_call(fn, args, 3, NULL);
    t_locals.count = mark;
    t_env = saved;
    return a;
}

void arm32_jni_activity_call(JNIEnv* e, uint32_t activity, int callback, uint32_t argument) {
    if (!activity || callback < 0 || callback >= 16) return;
    uint32_t fn = rd32(rd32(activity) + 4 * (uint32_t)callback);
    if (!fn) return;
    JNIEnv* saved = t_env;
    t_env = e;
    uint32_t mark = t_locals.count;
    uint32_t args[2] = {activity, argument};
    arm32_call(fn, args, 2, NULL);
    t_locals.count = mark;
    t_env = saved;
}

jobject arm32_jni_allocate(JNIEnv* e, jint capacity, jlongArray address) {
    if (!arm32_active() || capacity < 0) return NULL;
    uint32_t a = arm32_malloc((uint32_t)capacity ? (uint32_t)capacity : 1);
    if (!a) return NULL;
    memset(arm32_mem + a, 0, (size_t)capacity);
    jlong g = a;
    (*e)->SetLongArrayRegion(e, address, 0, 1, &g);
    return (*e)->NewDirectByteBuffer(e, arm32_mem + a, capacity);
}
JNIEnv* arm32_jni_env(void) { return env_of(); }

char* arm32_jni_load(JNIEnv* e, const char* path, const char* library_dir) {
    if (!start(e, library_dir)) return strdup("dlopen failed: the 32-bit native bridge doesn't start");
    JNIEnv* saved = t_env;
    t_env = e;
    uint32_t mark = t_locals.count;
    char error[1024];
    Arm32Library* l = arm32_load(path, error, sizeof error);
    char* result = NULL;
    if (!l) {
        result = snx_format("dlopen failed: %s", error);
    } else {
        uint32_t onload = arm32_symbol(l, "JNI_OnLoad");
        if (onload) {
            uint32_t args[2] = {j.vm_guest, 0};
            uint32_t version = arm32_call(onload, args, 2, NULL);
            if (!(*e)->ExceptionCheck(e) && version != JNI_VERSION_1_2 && version != JNI_VERSION_1_4 &&
                version != JNI_VERSION_1_6)
                result = snx_format("JNI_ERR returned from JNI_OnLoad in \"%s\"", arm32_library_name(l));
        }
    }
    t_locals.count = mark;
    t_env = saved;
    return result;
}

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
static uint32_t native_function(const char* key) {
    pthread_mutex_lock(&j.lock);
    for (uint32_t i = 0; i < j.native_count; ++i)
        if (!strcmp(j.natives[i].key, key)) {
            uint32_t fn = j.natives[i].fn;
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
    uint32_t fn = arm32_symbol(NULL, name);
    if (!fn) {
        at += (size_t)snprintf(name + at, sizeof name - at, "__");
        mangle(name, sizeof name, &at, paren + 1, strchr(paren, ')'));
        fn = arm32_symbol(NULL, name);
    }
    if (fn) add_native(key, fn);
    return fn;
}

uint64_t arm32_jni_invoke(JNIEnv* e, const char* key, jobject self, jlongArray prims, jobjectArray refs,
                          char* result, bool* found) {
    const char* paren = strchr(key, '(');
    const char* close = paren ? strchr(paren, ')') : NULL;
    *result = close ? close[1] : 'V';
    if (*result == '[') *result = 'L';
    uint32_t fn = j.started ? native_function(key) : 0;
    *found = fn != 0;
    if (!fn) return 0;
    JNIEnv* saved = t_env;
    t_env = e;
    uint32_t mark = t_locals.count, frames = t_frames.count;
    jsize np = prims ? (*e)->GetArrayLength(e, prims) : 0;
    jlong pv[64] = {0};
    if (np) (*e)->GetLongArrayRegion(e, prims, 0, np < 64 ? np : 64, pv);
    /* the words of the arguments, as AAPCS lays them out: a long or a double at an even word */
    uint32_t words[160];
    unsigned n = 0, pi = 0, ri = 0;
    words[n++] = j.env;
    words[n++] = local_ref(self);
    const char* p = paren ? paren + 1 : "";
    for (char t; n < 156 && (t = next_type(&p));) {
        if (t == 'L') {
            words[n++] = local_ref((*e)->GetObjectArrayElement(e, refs, (jsize)ri++));
        } else if (t == 'J' || t == 'D') {
            uint64_t v = (uint64_t)(pi < 64 ? pv[pi] : 0);
            ++pi;
            if (n & 1) words[n++] = 0;
            words[n++] = (uint32_t)v;
            words[n++] = (uint32_t)(v >> 32);
        } else {
            words[n++] = (uint32_t)(pi < 64 ? pv[pi] : 0);
            ++pi;
        }
    }
    uint32_t high = 0;
    uint32_t r0 = arm32_call(fn, words, n, &high);
    uint64_t r = r0;
    switch (*result) {
    case 'Z': r = (r0 & 0xff) != 0; break;
    case 'B': r = (uint64_t)(int64_t)(int8_t)r0; break;
    case 'C': r = r0 & 0xffff; break;
    case 'S': r = (uint64_t)(int64_t)(int16_t)r0; break;
    case 'I': r = (uint64_t)(int64_t)(int32_t)r0; break;
    case 'J':
    case 'D': r = r0 | (uint64_t)high << 32; break;
    case 'L': {
        jobject o = host_ref(r0);
        r = (uint64_t)(uintptr_t)(o ? (*e)->NewLocalRef(e, o) : NULL);
        break;
    }
    default: break;
    }
    t_locals.count = mark;
    t_frames.count = frames;
    t_env = saved;
    return r;
}
