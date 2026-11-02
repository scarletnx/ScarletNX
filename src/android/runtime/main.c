/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "android/runtime/main.h"
#include "android/dex/translate.h"
#include "android/res/asset_jni.h"
#include "android/db/sqlite_jni.h"
#include "android/hwui/render.h"
#include "android/hwui/text.h"
#include "android/res/xml_jni.h"
#include "android/runtime/app.h"
#include "android/runtime/jvm.h"
#include "common/utilities/common.h"
#include "common/utilities/zip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

static const char usage[] =
    "Usage: scarletnx-android [options] APK\n"
    "Runs an Android app: its DEX code translated to JVM classes, with ScarletNX's own Android framework.\n"
    "\n"
    "  --translate=DIR   Translate every class of the APK ahead of time into DIR, a .class file for each, and stop\n"
    "  -J OPTION         An option for the JVM (-J-Xmx2g)\n"
    "\n"
    "JAVA_HOME names the JDK (21 or later) to run on; without it, the one the build found.\n";

typedef struct {
    unsigned warnings;
} Translation;
static void warn(void* context, const char* message) {
    Translation* t = context;
    if (t->warnings++ < 50) fprintf(stderr, "scarletnx-android: %s\n", message);
}

/* every class of every DEX file of the APK (classes.dex, classes2.dex and on) into folder */
static int translate_apk(const char* apk, const char* folder) {
    char error[512];
    SnxZip* zip = snx_zip_open(apk, error, sizeof error);
    if (!zip) {
        fprintf(stderr, "scarletnx-android: %s\n", error);
        return 1;
    }
    /* the DEX files, each read whole */
    enum { MAX_DEX = 100 };
    Dex dexes[MAX_DEX];
    unsigned char* bytes[MAX_DEX];
    uint32_t count = 0;
    int status = 0;
    for (unsigned n = 1; n <= MAX_DEX; ++n) {
        char name[32];
        if (n == 1)
            snprintf(name, sizeof name, "classes.dex");
        else
            snprintf(name, sizeof name, "classes%u.dex", n);
        uint32_t index;
        if (!snx_zip_find(zip, name, &index)) break;
        size_t size;
        bytes[count] = snx_zip_read(zip, index, &size, error, sizeof error);
        if (!bytes[count] || !dex_open(&dexes[count], bytes[count], size, error, sizeof error)) {
            fprintf(stderr, "scarletnx-android: %s: %s\n", name, error);
            free(bytes[count]);
            status = 1;
            break;
        }
        ++count;
    }
    snx_zip_close(zip);
    Translation t = {0};
    DexTranslateOptions options = {NULL, NULL, warn};
    options.context = &t;
    DexProgram* program = status ? NULL : dex_program_open(dexes, count, &options, error, sizeof error);
    if (!status && !program) {
        fprintf(stderr, "scarletnx-android: %s\n", error);
        status = 1;
    }
    unsigned classes = 0, failures = 0;
    for (uint32_t d = 0; program && d < count; ++d)
        for (uint32_t i = 0; i < dexes[d].class_count; ++i) {
            char class_name[1024];
            size_t class_size;
            unsigned char* file = NULL;
            if (dex_class_name(&dexes[d], i, class_name, sizeof class_name))
                file = dex_translate_class(program, d, i, &class_size, error, sizeof error);
            else
                snprintf(error, sizeof error, "class def %u has a bad name", (unsigned)i);
            if (!file) {
                fprintf(stderr, "scarletnx-android: %s\n", error);
                ++failures;
                continue;
            }
            size_t length = strlen(class_name);
            char* relative = malloc(length + 7);
            char* path = NULL;
            if (relative) {
                memcpy(relative, class_name, length);
                memcpy(relative + length, ".class", 7);
                path = snx_path_join(folder, relative);
            }
            char* parent = path ? snx_dirname(path) : NULL;
            if (!parent || snx_mkdirs(parent) || snx_write_file(path, file, class_size)) {
                fprintf(stderr, "scarletnx-android: cannot write %s\n", path ? path : class_name);
                ++failures;
            } else {
                ++classes;
            }
            free(parent);
            free(path);
            free(relative);
            free(file);
        }
    dex_program_close(program);
    for (uint32_t d = 0; d < count; ++d) {
        dex_close(&dexes[d]);
        free(bytes[d]);
    }
    if (status) return status;
    printf("%u classes translated, %u failed, %u methods that throw VerifyError\n", classes, failures, t.warnings);
    return failures ? 1 : 0;
}

/* the run on the JVM's own thread: HotSpot wants a thread other than the process's first, as the java launcher
   gives it */
typedef struct {
    const char* framework;
    char** options;
    size_t option_count;
    int status;
} Run;
static void run_app(Run* run) {
    char error[1024];
    AndroidJvm jvm;
    if (!android_jvm_start(&jvm, run->framework, (const char* const*)run->options, run->option_count, error,
                           sizeof error)) {
        fprintf(stderr, "scarletnx-android: %s\n", error);
        run->status = 1;
        return;
    }
    JNIEnv* env = jvm.env;
    run->status = 1;
    if (!android_app_register(env) || !android_xml_register(env) || !android_assets_register(env) ||
        !android_sqlite_register(env) || !android_text_register(env) ||
        !android_render_register(env)) return;
    jclass main_class = (*env)->FindClass(env, "snx/android/Main");
    jmethodID main = main_class ? (*env)->GetStaticMethodID(env, main_class, "main", "([Ljava/lang/String;)V") : NULL;
    jclass string = (*env)->FindClass(env, "java/lang/String");
    jobjectArray args = string ? (*env)->NewObjectArray(env, 0, string, NULL) : NULL;
    if (!main || !args) {
        android_jvm_exception(env);
        fprintf(stderr, "scarletnx-android: the framework (%s) has no snx.android.Main\n", run->framework);
        return;
    }
    (*env)->CallStaticVoidMethod(env, main_class, main, args);
    run->status = android_jvm_exception(env) ? 1 : 0;
    /* the app's other threads keep the JVM until they end, as the java launcher's DestroyJavaVM waits */
    (*jvm.vm)->DestroyJavaVM(jvm.vm);
}
#ifdef _WIN32
static DWORD WINAPI run_thread(LPVOID run) {
    run_app(run);
    return 0;
}
#else
static void* run_thread(void* run) {
    run_app(run);
    return NULL;
}
#endif

/* the framework's jar: SNX_ANDROID_FRAMEWORK, or the one beside the program */
static char* framework_jar(const char* argv0) {
    const char* named = getenv("SNX_ANDROID_FRAMEWORK");
    if (named && named[0]) {
        size_t n = strlen(named) + 1;
        char* copy = malloc(n);
        if (copy) memcpy(copy, named, n);
        return copy;
    }
    char* exe = snx_executable(argv0);
    char* folder = exe ? snx_dirname(exe) : NULL;
    char* jar = folder ? snx_path_join(folder, "scarletnx-android-framework.jar") : NULL;
    free(folder);
    free(exe);
    return jar;
}

static int run_apk(const char* argv0, const char* apk, char** options, size_t option_count) {
    char error[1024];
    if (!android_app_open(apk, error, sizeof error)) {
        fprintf(stderr, "scarletnx-android: %s\n", error);
        android_app_close();
        return 1;
    }
    char* jar = framework_jar(argv0);
    if (!jar || !snx_is_file(jar)) {
        fprintf(stderr, "scarletnx-android: no framework jar at %s\n", jar ? jar : "(none)");
        free(jar);
        android_app_close();
        return 1;
    }
    /* the fonts beside the jar (the build copies them there); an option the user gives comes later and wins */
    char* folder = snx_dirname(jar);
    char* fonts = folder ? snx_path_join(folder, "android-fonts") : NULL;
    char* fonts_option = fonts ? snx_format("-Dsnx.android.fonts=%s", fonts) : NULL;
    char** all = calloc(option_count + 1, sizeof *all);
    free(folder);
    free(fonts);
    if (!fonts_option || !all) {
        free(fonts_option);
        free(all);
        free(jar);
        android_app_close();
        return 1;
    }
    all[0] = fonts_option;
    for (size_t i = 0; i < option_count; ++i) all[i + 1] = options[i];
    Run run = {jar, all, option_count + 1, 1};
#ifdef _WIN32
    HANDLE thread = CreateThread(NULL, 16u << 20, run_thread, &run, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (thread) {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
#else
    pthread_attr_t attr;
    pthread_t thread;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, (size_t)16 << 20);
    if (!pthread_create(&thread, &attr, run_thread, &run)) pthread_join(thread, NULL);
    pthread_attr_destroy(&attr);
#endif
    free(all);
    free(fonts_option);
    free(jar);
    android_app_close();
    return run.status;
}

int snx_android_main(int argc, char** argv) {
    const char *apk = NULL, *translate = NULL;
    char** options = calloc((size_t)argc + 1, sizeof *options);
    size_t option_count = 0;
    if (!options) return 1;
    for (int i = 1; i < argc; ++i) {
        if (!strncmp(argv[i], "--translate=", 12))
            translate = argv[i] + 12;
        else if (!strcmp(argv[i], "-J") && i + 1 < argc)
            options[option_count++] = argv[++i];
        else if (!strncmp(argv[i], "-J", 2) && argv[i][2])
            options[option_count++] = argv[i] + 2;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            fputs(usage, stdout);
            free(options);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "scarletnx-android: unknown option %s\n%s", argv[i], usage);
            free(options);
            return 2;
        } else {
            apk = argv[i];
        }
    }
    int status;
    if (!apk) {
        fputs(usage, stderr);
        status = 2;
    } else {
        status = translate ? translate_apk(apk, translate) : run_apk(argv[0], apk, options, option_count);
    }
    free(options);
    return status;
}
