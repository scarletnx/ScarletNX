/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* android.content.res.AssetManager's natives, over arsc.c: the HLE of libandroidfw's AssetManager2 and Theme. The
   Java side calls them holding the AssetManager's lock */
#include "android/res/asset_jni.h"
#include "android/res/arsc.h"
#include "android/res/xml_jni.h"
#include "android/runtime/jvm.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static struct {
    jfieldID type, data, cookie, resource, changing, density, string, source;
} typed_value;

static ArscAssets* assets_of(jlong h) {
    return (ArscAssets*)(intptr_t)h;
}
static ArscTheme* theme_of(jlong h) {
    return (ArscTheme*)(intptr_t)h;
}

/* a string of a pool as a Java string */
static jstring pool_string(JNIEnv* env, const ResStringPool* pool, uint32_t index) {
    long n = pool ? res_string16(pool, index, NULL, 0) : -1;
    if (n < 0) return NULL;
    uint16_t* units = malloc(((size_t)n + 1) * sizeof *units);
    if (!units) return NULL;
    res_string16(pool, index, units, (size_t)n);
    jstring s = (*env)->NewString(env, units, (jsize)n);
    free(units);
    return s;
}
/* a value into a TypedValue; its cookie, -1 for none */
static jint fill(JNIEnv* env, ArscAssets* a, const ArscValue* v, jobject out) {
    (*env)->SetIntField(env, out, typed_value.type, v->value.type);
    (*env)->SetIntField(env, out, typed_value.data, (jint)v->value.data);
    (*env)->SetIntField(env, out, typed_value.cookie, v->cookie);
    (*env)->SetIntField(env, out, typed_value.resource, (jint)v->resid);
    (*env)->SetIntField(env, out, typed_value.changing, (jint)v->flags);
    (*env)->SetIntField(env, out, typed_value.density, v->density);
    jstring s = v->value.type == RES_TYPE_STRING ? pool_string(env, arsc_strings(a, v->cookie), v->value.data) : NULL;
    (*env)->SetObjectField(env, out, typed_value.string, s);
    if (s) (*env)->DeleteLocalRef(env, s);
    return v->cookie;
}

static jlong JNICALL create(JNIEnv* env, jclass unused) {
    (void)env;
    (void)unused;
    return (jlong)(intptr_t)arsc_create();
}
static void JNICALL destroy(JNIEnv* env, jclass unused, jlong h) {
    (void)env;
    (void)unused;
    arsc_destroy(assets_of(h));
}
static jint JNICALL add_table(JNIEnv* env, jclass unused, jlong h, jbyteArray data) {
    (void)unused;
    jsize size = (*env)->GetArrayLength(env, data);
    jbyte* bytes = (*env)->GetByteArrayElements(env, data, NULL);
    if (!bytes) return 0;
    char error[128];
    int32_t cookie = arsc_add(assets_of(h), (const unsigned char*)bytes, (size_t)size, error, sizeof error);
    (*env)->ReleaseByteArrayElements(env, data, bytes, JNI_ABORT);
    return cookie;
}
static void JNICALL set_configuration(JNIEnv* env, jclass unused, jlong h, jstring language, jstring country,
                                      jint density, jint sdk, jint orientation, jint ui_mode, jint width_dp,
                                      jint height_dp, jint smallest_dp, jint screen_layout, jint touchscreen,
                                      jint keyboard, jint navigation) {
    (void)unused;
    ArscConfig c = {{0}, {0}, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const char* l = language ? (*env)->GetStringUTFChars(env, language, NULL) : NULL;
    const char* k = country ? (*env)->GetStringUTFChars(env, country, NULL) : NULL;
    if (l) {
        strncpy(c.language, l, 2);
        (*env)->ReleaseStringUTFChars(env, language, l);
    }
    if (k) {
        strncpy(c.country, k, 2);
        (*env)->ReleaseStringUTFChars(env, country, k);
    }
    c.density = (uint16_t)density;
    c.sdk = (uint16_t)sdk;
    c.orientation = (uint8_t)orientation;
    c.ui_mode = (uint8_t)ui_mode;
    c.width_dp = (uint16_t)width_dp;
    c.height_dp = (uint16_t)height_dp;
    c.smallest_width_dp = (uint16_t)smallest_dp;
    c.screen_layout = (uint8_t)screen_layout;
    c.touchscreen = (uint8_t)touchscreen;
    c.keyboard = (uint8_t)keyboard;
    c.navigation = (uint8_t)navigation;
    arsc_set_config(assets_of(h), &c);
}
static jint JNICALL get_value(JNIEnv* env, jclass unused, jlong h, jint resid, jobject out, jboolean resolve) {
    (void)unused;
    ArscValue v;
    if (!arsc_get(assets_of(h), (uint32_t)resid, &v)) return -1;
    if (resolve && !arsc_resolve_reference(assets_of(h), &v)) return -1;
    return fill(env, assets_of(h), &v, out);
}
static jstring JNICALL pooled_string(JNIEnv* env, jclass unused, jlong h, jint cookie, jint index) {
    (void)unused;
    return pool_string(env, arsc_strings(assets_of(h), cookie), (uint32_t)index);
}
/* a bag's entries, ARSC_STYLE_ENTRIES + 1 ints each: the key, then the entry as attribute retrieval writes it */
static jintArray JNICALL get_bag(JNIEnv* env, jclass unused, jlong h, jint resid) {
    (void)unused;
    const uint32_t* keys;
    const ArscValue* values;
    uint32_t count;
    if (!arsc_bag(assets_of(h), (uint32_t)resid, &keys, &values, &count)) return NULL;
    enum { STRIDE = ARSC_STYLE_ENTRIES + 1 };
    jint* flat = malloc(((size_t)count * STRIDE + 1) * sizeof *flat);
    if (!flat) return NULL;
    for (uint32_t i = 0; i < count; ++i) {
        ArscValue v = values[i];
        arsc_resolve_reference(assets_of(h), &v);
        jint* e = flat + (size_t)i * STRIDE;
        e[0] = (jint)keys[i];
        e[1] = v.value.type;
        e[2] = (jint)v.value.data;
        e[3] = v.cookie;
        e[4] = (jint)v.resid;
        e[5] = (jint)v.flags;
        e[6] = v.density;
        e[7] = resid;
    }
    jintArray result = (*env)->NewIntArray(env, (jsize)(count * STRIDE));
    if (result) (*env)->SetIntArrayRegion(env, result, 0, (jsize)(count * STRIDE), flat);
    free(flat);
    return result;
}
static jstring JNICALL resource_name(JNIEnv* env, jclass unused, jlong h, jint resid) {
    (void)unused;
    char package[128], type[128], entry[512], name[800];
    if (!arsc_name(assets_of(h), (uint32_t)resid, package, sizeof package, type, sizeof type, entry, sizeof entry))
        return NULL;
    snprintf(name, sizeof name, "%s:%s/%s", package, type, entry);
    return (*env)->NewStringUTF(env, name);
}
static jint JNICALL identifier(JNIEnv* env, jclass unused, jlong h, jstring name, jstring type, jstring package) {
    (void)unused;
    const char* n = (*env)->GetStringUTFChars(env, name, NULL);
    const char* t = type ? (*env)->GetStringUTFChars(env, type, NULL) : NULL;
    const char* p = package ? (*env)->GetStringUTFChars(env, package, NULL) : NULL;
    jint id = n && t ? (jint)arsc_identifier(assets_of(h), p, t, n) : 0;
    if (n) (*env)->ReleaseStringUTFChars(env, name, n);
    if (t) (*env)->ReleaseStringUTFChars(env, type, t);
    if (p) (*env)->ReleaseStringUTFChars(env, package, p);
    return id;
}

static jlong JNICALL theme_create(JNIEnv* env, jclass unused, jlong h) {
    (void)env;
    (void)unused;
    return (jlong)(intptr_t)arsc_theme_create(assets_of(h));
}
static void JNICALL theme_destroy(JNIEnv* env, jclass unused, jlong t) {
    (void)env;
    (void)unused;
    arsc_theme_destroy(theme_of(t));
}
static void JNICALL theme_apply(JNIEnv* env, jclass unused, jlong t, jint style, jboolean force) {
    (void)env;
    (void)unused;
    arsc_theme_apply(theme_of(t), (uint32_t)style, force != 0);
}
static void JNICALL theme_copy(JNIEnv* env, jclass unused, jlong dst, jlong src) {
    (void)env;
    (void)unused;
    arsc_theme_copy(theme_of(dst), theme_of(src));
}
static void JNICALL theme_clear(JNIEnv* env, jclass unused, jlong t) {
    (void)env;
    (void)unused;
    arsc_theme_clear(theme_of(t));
}
static jint JNICALL theme_get(JNIEnv* env, jclass unused, jlong t, jint attr, jobject out, jboolean resolve) {
    (void)unused;
    ArscValue v;
    if (!arsc_theme_get(theme_of(t), (uint32_t)attr, &v)) return -1;
    if (resolve && !arsc_resolve_reference(arsc_theme_assets(theme_of(t)), &v)) return -1;
    return fill(env, arsc_theme_assets(theme_of(t)), &v, out);
}
static jintArray JNICALL theme_styles(JNIEnv* env, jclass unused, jlong t) {
    (void)unused;
    uint32_t count = arsc_theme_styles(theme_of(t), NULL, NULL, 0);
    uint32_t* styles = malloc(((size_t)count + 1) * sizeof *styles);
    bool* forced = malloc(((size_t)count + 1) * sizeof *forced);
    jint* flat = malloc(((size_t)count * 2 + 1) * sizeof *flat);
    jintArray result = NULL;
    if (styles && forced && flat) {
        arsc_theme_styles(theme_of(t), styles, forced, count);
        for (uint32_t i = 0; i < count; ++i) {
            flat[2 * i] = (jint)styles[i];
            flat[2 * i + 1] = forced[i];
        }
        result = (*env)->NewIntArray(env, (jsize)(2 * count));
        if (result) (*env)->SetIntArrayRegion(env, result, 0, (jsize)(2 * count), flat);
    }
    free(styles);
    free(forced);
    free(flat);
    return result;
}

/* the attributes, the output arrays and their sizes checked against each other */
typedef struct {
    jint* attrs;
    jint* values;
    jint* indices;
    jsize count;
} Retrieval;
static bool begin(JNIEnv* env, Retrieval* r, jintArray attrs, jintArray values, jintArray indices) {
    *r = (Retrieval){NULL, NULL, NULL, 0};
    r->count = (*env)->GetArrayLength(env, attrs);
    if ((*env)->GetArrayLength(env, values) < r->count * ARSC_STYLE_ENTRIES ||
        (indices && (*env)->GetArrayLength(env, indices) < r->count + 1))
        return false;
    r->attrs = (*env)->GetIntArrayElements(env, attrs, NULL);
    r->values = (*env)->GetIntArrayElements(env, values, NULL);
    r->indices = indices ? (*env)->GetIntArrayElements(env, indices, NULL) : NULL;
    return r->attrs && r->values && (!indices || r->indices);
}
static void end(JNIEnv* env, Retrieval* r, jintArray attrs, jintArray values, jintArray indices) {
    if (r->attrs) (*env)->ReleaseIntArrayElements(env, attrs, r->attrs, JNI_ABORT);
    if (r->values) (*env)->ReleaseIntArrayElements(env, values, r->values, 0);
    if (r->indices) (*env)->ReleaseIntArrayElements(env, indices, r->indices, 0);
}
static void JNICALL apply_style(JNIEnv* env, jclass unused, jlong t, jlong xml, jint def_attr, jint def_res,
                                jintArray attrs, jintArray values, jintArray indices) {
    (void)unused;
    Retrieval r;
    if (begin(env, &r, attrs, values, indices))
        arsc_apply_style(theme_of(t), android_xml_parser(xml), (uint32_t)def_attr, (uint32_t)def_res,
                         (const uint32_t*)r.attrs, (uint32_t)r.count, r.values, r.indices);
    end(env, &r, attrs, values, indices);
}
static jboolean JNICALL resolve_attrs(JNIEnv* env, jclass unused, jlong t, jint def_attr, jint def_res,
                                      jintArray in, jintArray attrs, jintArray values, jintArray indices) {
    (void)unused;
    Retrieval r;
    bool ok = begin(env, &r, attrs, values, indices);
    jint* in_values = ok && in ? (*env)->GetIntArrayElements(env, in, NULL) : NULL;
    jsize in_count = in_values ? (*env)->GetArrayLength(env, in) : 0;
    if (ok)
        ok = arsc_resolve_attrs(theme_of(t), (uint32_t)def_attr, (uint32_t)def_res, in_values, (uint32_t)in_count,
                                (const uint32_t*)r.attrs, (uint32_t)r.count, r.values, r.indices);
    if (in_values) (*env)->ReleaseIntArrayElements(env, in, in_values, JNI_ABORT);
    end(env, &r, attrs, values, indices);
    return ok;
}
static jboolean JNICALL retrieve_attributes(JNIEnv* env, jclass unused, jlong h, jlong xml, jintArray attrs,
                                            jintArray values, jintArray indices) {
    (void)unused;
    Retrieval r;
    bool ok = begin(env, &r, attrs, values, indices);
    if (ok)
        ok = arsc_retrieve_attributes(assets_of(h), android_xml_parser(xml), (const uint32_t*)r.attrs,
                                      (uint32_t)r.count, r.values, r.indices);
    end(env, &r, attrs, values, indices);
    return ok;
}

bool android_assets_register(JNIEnv* env) {
    jclass tv = (*env)->FindClass(env, "android/util/TypedValue");
    if (!tv) return android_jvm_exception(env), false;
    typed_value.type = (*env)->GetFieldID(env, tv, "type", "I");
    typed_value.data = (*env)->GetFieldID(env, tv, "data", "I");
    typed_value.cookie = (*env)->GetFieldID(env, tv, "assetCookie", "I");
    typed_value.resource = (*env)->GetFieldID(env, tv, "resourceId", "I");
    typed_value.changing = (*env)->GetFieldID(env, tv, "changingConfigurations", "I");
    typed_value.density = (*env)->GetFieldID(env, tv, "density", "I");
    typed_value.string = (*env)->GetFieldID(env, tv, "string", "Ljava/lang/CharSequence;");
    (*env)->DeleteLocalRef(env, tv);
    if (android_jvm_exception(env)) return false;
    static const JNINativeMethod methods[] = {
        {(char*)"nativeCreate", (char*)"()J", (void*)create},
        {(char*)"nativeDestroy", (char*)"(J)V", (void*)destroy},
        {(char*)"nativeAddTable", (char*)"(J[B)I", (void*)add_table},
        {(char*)"nativeSetConfiguration", (char*)"(JLjava/lang/String;Ljava/lang/String;IIIIIIIIIII)V",
         (void*)set_configuration},
        {(char*)"nativeGetResourceValue", (char*)"(JILandroid/util/TypedValue;Z)I", (void*)get_value},
        {(char*)"nativeGetPooledString", (char*)"(JII)Ljava/lang/String;", (void*)pooled_string},
        {(char*)"nativeGetBag", (char*)"(JI)[I", (void*)get_bag},
        {(char*)"nativeGetResourceName", (char*)"(JI)Ljava/lang/String;", (void*)resource_name},
        {(char*)"nativeGetResourceIdentifier", (char*)"(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;)I",
         (void*)identifier},
        {(char*)"nativeThemeCreate", (char*)"(J)J", (void*)theme_create},
        {(char*)"nativeThemeDestroy", (char*)"(J)V", (void*)theme_destroy},
        {(char*)"nativeThemeApplyStyle", (char*)"(JIZ)V", (void*)theme_apply},
        {(char*)"nativeThemeCopy", (char*)"(JJ)V", (void*)theme_copy},
        {(char*)"nativeThemeClear", (char*)"(J)V", (void*)theme_clear},
        {(char*)"nativeThemeGetAttributeValue", (char*)"(JILandroid/util/TypedValue;Z)I", (void*)theme_get},
        {(char*)"nativeThemeGetStyles", (char*)"(J)[I", (void*)theme_styles},
        {(char*)"nativeApplyStyle", (char*)"(JJII[I[I[I)V", (void*)apply_style},
        {(char*)"nativeResolveAttrs", (char*)"(JII[I[I[I[I)Z", (void*)resolve_attrs},
        {(char*)"nativeRetrieveAttributes", (char*)"(JJ[I[I[I)Z", (void*)retrieve_attributes},
    };
    return android_jvm_register(env, "android/content/res/AssetManager", methods,
                                (int)(sizeof methods / sizeof *methods));
}
