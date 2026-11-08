/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "android/runtime/app.h"
#include "android/runtime/install.h"
#include "android/dex/translate.h"
#include "android/runtime/jvm.h"
#include "common/utilities/zip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_DEX = 100 };

static struct {
    const AndroidInstall* install;
    /* the base APK, then each split: the Java side reads them one call at a time (Apk's synchronized methods) */
    SnxZip* zips[1 + ANDROID_MAX_SPLITS];
    unsigned zip_count;
    Dex dexes[MAX_DEX];
    unsigned char* bytes[MAX_DEX];
    uint32_t dex_count;
    DexProgram* program;
} app;

static void warn(void* context, const char* message) {
    (void)context;
    fprintf(stderr, "scarletnx-android: translation: %s\n", message);
}

/* the APK of the app that holds a member, its index in *index */
static SnxZip* find(const char* name, uint32_t* index) {
    for (unsigned i = 0; i < app.zip_count; ++i)
        if (snx_zip_find(app.zips[i], name, index)) return app.zips[i];
    return NULL;
}

bool android_app_open(const AndroidInstall* install, char* error, size_t error_size) {
    app.install = install;
    for (unsigned i = 0; i <= install->split_count; ++i) {
        const char* apk = i ? install->splits[i - 1] : install->base;
        app.zips[i] = snx_zip_open(apk, error, error_size);
        if (!app.zips[i]) return false;
        app.zip_count = i + 1;
    }
    const char* apk = install->base;
    for (unsigned i = 1; i <= MAX_DEX; ++i) {
        char name[32];
        if (i == 1)
            snprintf(name, sizeof name, "classes.dex");
        else
            snprintf(name, sizeof name, "classes%u.dex", i);
        uint32_t index;
        if (!snx_zip_find(app.zips[0], name, &index)) break;
        size_t size;
        unsigned char* bytes = snx_zip_read(app.zips[0], index, &size, error, error_size);
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
    for (unsigned i = 0; i < app.zip_count; ++i) snx_zip_close(app.zips[i]);
    memset(&app, 0, sizeof app);
}

/* ---- the app's assets for the native bridge (hle_android.c) ---- */

static unsigned char* asset_read(const char* name, size_t* size) {
    uint32_t index;
    char path[1024];
    snprintf(path, sizeof path, "assets/%s", name);
    SnxZip* zip = find(path, &index);
    if (!zip) return NULL;
    char error[256];
    return snx_zip_read(zip, index, size, error, sizeof error);
}
static bool asset_locate(const char* name, const char** apk, uint64_t* offset, uint64_t* size) {
    uint32_t index;
    char path[1024];
    snprintf(path, sizeof path, "assets/%s", name);
    SnxZip* zip = find(path, &index);
    if (!zip || !snx_zip_stored_at(zip, index, offset)) return false;
    *size = snx_zip_size(zip, index);
    for (unsigned i = 0; i < app.zip_count; ++i)
        if (app.zips[i] == zip) *apk = i ? app.install->splits[i - 1] : app.install->base;
    return true;
}
/* the names directly in an asset folder ("" the top): each once, the caller's to free with the array */
static char** asset_list(const char* dir, size_t* count) {
    char prefix[1024];
    snprintf(prefix, sizeof prefix, dir[0] ? "assets/%s/" : "assets/", dir);
    size_t n = strlen(prefix), have = 0, capacity = 64;
    char** names = malloc(capacity * sizeof *names);
    for (unsigned z = 0; names && z < app.zip_count; ++z)
        for (uint32_t i = 0; i < snx_zip_count(app.zips[z]); ++i) {
            const char* m = snx_zip_name(app.zips[z], i);
            if (strncmp(m, prefix, n)) continue;
            const char* rest = m + n;
            const char* slash = strchr(rest, '/');
            size_t len = slash ? (size_t)(slash - rest) : strlen(rest);
            if (!len) continue;
            bool seen = false;
            for (size_t k = 0; k < have && !seen; ++k) seen = strlen(names[k]) == len && !strncmp(names[k], rest, len);
            if (seen) continue;
            if (have == capacity) {
                capacity *= 2;
                char** grown = realloc(names, capacity * sizeof *names);
                if (!grown) break;
                names = grown;
            }
            names[have] = malloc(len + 1);
            if (!names[have]) break;
            memcpy(names[have], rest, len);
            names[have++][len] = 0;
        }
    *count = have;
    return names;
}
const AndroidAssets* android_app_assets(void) {
    static const AndroidAssets assets = {asset_read, asset_locate, asset_list};
    return &assets;
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
    return (*env)->NewStringUTF(env, app.install->base);
}
static jobjectArray strings(JNIEnv* env, char* const* list, unsigned count) {
    jclass string = (*env)->FindClass(env, "java/lang/String");
    jobjectArray out = string ? (*env)->NewObjectArray(env, (jsize)count, string, NULL) : NULL;
    for (unsigned i = 0; out && i < count; ++i) {
        jstring s = (*env)->NewStringUTF(env, list[i]);
        if (!s) return NULL;
        (*env)->SetObjectArrayElement(env, out, (jsize)i, s);
        (*env)->DeleteLocalRef(env, s);
    }
    return out;
}
static jobjectArray JNICALL split_paths(JNIEnv* env, jclass unused) {
    (void)unused;
    return strings(env, app.install->splits, app.install->split_count);
}
static jobjectArray JNICALL split_names(JNIEnv* env, jclass unused) {
    (void)unused;
    return strings(env, app.install->split_names, app.install->split_count);
}
static jstring JNICALL library_dir(JNIEnv* env, jclass unused) {
    (void)unused;
    return (*env)->NewStringUTF(env, app.install->library_dir);
}
static jbyteArray JNICALL apk_read(JNIEnv* env, jclass unused, jstring name) {
    (void)unused;
    const char* text = (*env)->GetStringUTFChars(env, name, NULL);
    if (!text) return NULL;
    uint32_t index;
    jbyteArray result = NULL;
    SnxZip* zip = find(text, &index);
    if (zip) {
        char error[512];
        size_t size;
        unsigned char* data = snx_zip_read(zip, index, &size, error, sizeof error);
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
/* whether a member is stored, not compressed: what a file descriptor can read in place */
static jboolean JNICALL apk_stored(JNIEnv* env, jclass unused, jstring name) {
    (void)unused;
    const char* text = (*env)->GetStringUTFChars(env, name, NULL);
    if (!text) return JNI_FALSE;
    uint32_t index;
    uint64_t offset;
    SnxZip* zip = find(text, &index);
    jboolean stored = zip && snx_zip_stored_at(zip, index, &offset) ? JNI_TRUE : JNI_FALSE;
    (*env)->ReleaseStringUTFChars(env, name, text);
    return stored;
}
static jobjectArray JNICALL apk_list(JNIEnv* env, jclass unused) {
    (void)unused;
    jclass string = (*env)->FindClass(env, "java/lang/String");
    if (!string) return NULL;
    /* the members of every APK of the app, the base's first */
    uint32_t count = 0;
    for (unsigned z = 0; z < app.zip_count; ++z) count += snx_zip_count(app.zips[z]);
    jobjectArray names = (*env)->NewObjectArray(env, (jsize)count, string, NULL);
    uint32_t at = 0;
    for (unsigned z = 0; names && z < app.zip_count; ++z)
        for (uint32_t i = 0; i < snx_zip_count(app.zips[z]); ++i) {
            jstring s = (*env)->NewStringUTF(env, snx_zip_name(app.zips[z], i));
            if (!s) return NULL;
            (*env)->SetObjectArrayElement(env, names, (jsize)at++, s);
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
        {(char*)"nativeStored", (char*)"(Ljava/lang/String;)Z", (void*)apk_stored},
        {(char*)"splitPaths", (char*)"()[Ljava/lang/String;", (void*)split_paths},
        {(char*)"splitNames", (char*)"()[Ljava/lang/String;", (void*)split_names},
        {(char*)"libraryDir", (char*)"()Ljava/lang/String;", (void*)library_dir},
    };
    return android_jvm_register(env, "snx/dalvik/AppClassLoader", loader, 1) &&
           android_jvm_register(env, "snx/android/Apk", apk, (int)(sizeof apk / sizeof *apk));
}
