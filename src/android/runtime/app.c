/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "android/runtime/app.h"
#include "android/dex/translate.h"
#include "android/runtime/jvm.h"
#include "common/utilities/zip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_DEX = 100 };

static struct {
    char* path;
    SnxZip* zip; /* the Java side reads it one call at a time (Apk's synchronized methods) */
    Dex dexes[MAX_DEX];
    unsigned char* bytes[MAX_DEX];
    uint32_t dex_count;
    DexProgram* program;
} app;

static void warn(void* context, const char* message) {
    (void)context;
    fprintf(stderr, "scarletnx-android: translation: %s\n", message);
}

bool android_app_open(const char* apk, char* error, size_t error_size) {
    size_t n = strlen(apk) + 1;
    app.path = malloc(n);
    if (!app.path) {
        snprintf(error, error_size, "out of memory");
        return false;
    }
    memcpy(app.path, apk, n);
    app.zip = snx_zip_open(apk, error, error_size);
    if (!app.zip) return false;
    for (unsigned i = 1; i <= MAX_DEX; ++i) {
        char name[32];
        if (i == 1)
            snprintf(name, sizeof name, "classes.dex");
        else
            snprintf(name, sizeof name, "classes%u.dex", i);
        uint32_t index;
        if (!snx_zip_find(app.zip, name, &index)) break;
        size_t size;
        unsigned char* bytes = snx_zip_read(app.zip, index, &size, error, error_size);
        if (!bytes) return false;
        char why[256];
        if (!dex_open(&app.dexes[app.dex_count], bytes, size, why, sizeof why)) {
            snprintf(error, error_size, "%s: %s", name, why);
            free(bytes);
            return false;
        }
        app.bytes[app.dex_count++] = bytes;
    }
    if (!app.dex_count) {
        snprintf(error, error_size, "%s has no classes.dex", apk);
        return false;
    }
    DexTranslateOptions options = {NULL, NULL, warn};
    app.program = dex_program_open(app.dexes, app.dex_count, &options, error, error_size);
    return app.program != NULL;
}

void android_app_close(void) {
    dex_program_close(app.program);
    for (uint32_t i = 0; i < app.dex_count; ++i) {
        dex_close(&app.dexes[i]);
        free(app.bytes[i]);
    }
    snx_zip_close(app.zip);
    free(app.path);
    memset(&app, 0, sizeof app);
}

/* AppClassLoader.translate: "a.b.C" as the class file of La/b/C; */
static jbyteArray JNICALL translate(JNIEnv* env, jclass unused, jstring name) {
    (void)unused;
    const char* text = (*env)->GetStringUTFChars(env, name, NULL);
    if (!text) return NULL;
    size_t n = strlen(text);
    char* desc = malloc(n + 3);
    if (!desc) {
        (*env)->ReleaseStringUTFChars(env, name, text);
        return NULL;
    }
    desc[0] = 'L';
    for (size_t i = 0; i < n; ++i) desc[i + 1] = text[i] == '.' ? '/' : text[i];
    desc[n + 1] = ';';
    desc[n + 2] = 0;
    (*env)->ReleaseStringUTFChars(env, name, text);
    uint32_t dex, index;
    jbyteArray result = NULL;
    if (dex_program_find(app.program, desc, &dex, &index)) {
        char error[512];
        size_t size;
        unsigned char* file = dex_translate_class(app.program, dex, index, &size, error, sizeof error);
        if (!file) {
            fprintf(stderr, "scarletnx-android: %s\n", error);
        } else {
            result = (*env)->NewByteArray(env, (jsize)size);
            if (result) (*env)->SetByteArrayRegion(env, result, 0, (jsize)size, (const jbyte*)file);
            free(file);
        }
    }
    free(desc);
    return result;
}

static jstring JNICALL apk_path(JNIEnv* env, jclass unused) {
    (void)unused;
    return (*env)->NewStringUTF(env, app.path);
}
static jbyteArray JNICALL apk_read(JNIEnv* env, jclass unused, jstring name) {
    (void)unused;
    const char* text = (*env)->GetStringUTFChars(env, name, NULL);
    if (!text) return NULL;
    uint32_t index;
    jbyteArray result = NULL;
    if (snx_zip_find(app.zip, text, &index)) {
        char error[512];
        size_t size;
        unsigned char* data = snx_zip_read(app.zip, index, &size, error, sizeof error);
        if (!data) {
            fprintf(stderr, "scarletnx-android: %s\n", error);
        } else {
            result = (*env)->NewByteArray(env, (jsize)size);
            if (result) (*env)->SetByteArrayRegion(env, result, 0, (jsize)size, (const jbyte*)data);
            free(data);
        }
    }
    (*env)->ReleaseStringUTFChars(env, name, text);
    return result;
}
static jobjectArray JNICALL apk_list(JNIEnv* env, jclass unused) {
    (void)unused;
    jclass string = (*env)->FindClass(env, "java/lang/String");
    if (!string) return NULL;
    uint32_t count = snx_zip_count(app.zip);
    jobjectArray names = (*env)->NewObjectArray(env, (jsize)count, string, NULL);
    for (uint32_t i = 0; names && i < count; ++i) {
        jstring s = (*env)->NewStringUTF(env, snx_zip_name(app.zip, i));
        if (!s) return NULL;
        (*env)->SetObjectArrayElement(env, names, (jsize)i, s);
        (*env)->DeleteLocalRef(env, s);
    }
    return names;
}

bool android_app_register(JNIEnv* env) {
    static const JNINativeMethod loader[] = {
        {(char*)"translate", (char*)"(Ljava/lang/String;)[B", (void*)translate},
    };
    static const JNINativeMethod apk[] = {
        {(char*)"path", (char*)"()Ljava/lang/String;", (void*)apk_path},
        {(char*)"nativeRead", (char*)"(Ljava/lang/String;)[B", (void*)apk_read},
        {(char*)"nativeList", (char*)"()[Ljava/lang/String;", (void*)apk_list},
    };
    return android_jvm_register(env, "snx/dalvik/AppClassLoader", loader, 1) &&
           android_jvm_register(env, "snx/android/Apk", apk, 3);
}
