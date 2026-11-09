/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "android/bridge/bridge.h"
#include "android/bridge/hle_proc.h"
#include "android/bridge/hle_vulkan.h"
#include "android/bridge/linker.h"
#include "android/bridge/hle_signal.h"
#include "android/runtime/audio.h"
#include "android/runtime/main.h"
#include "android/dex/translate.h"
#include "android/res/asset_jni.h"
#include "android/db/sqlite_jni.h"
#include "android/bridge/jni.h"
#include "android/bridge/native.h"
#include "android/bridge/hle_assets.h"
#include "common/utilities/ini_lines.h"
#include "android/hwui/image.h"
#include "android/hwui/render.h"
#include "android/hwui/text.h"
#include "android/res/xml_jni.h"
#include "android/runtime/app.h"
#include "android/runtime/fds.h"
#include "android/runtime/install.h"
#include "android/runtime/jvm.h"
#include "common/utilities/cli.h"
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

/* the common options scarletnx-android takes (common/utilities/cli.h): an APK is installed and run as it is, so of
   the rest only a run without a user's: --headless and --shot, whose frames --shot-frame names */
static const unsigned android_cli = SNX_CLI_BIT(SNX_CLI_HEADLESS) | SNX_CLI_BIT(SNX_CLI_SHOT) | SNX_CLI_BIT(SNX_CLI_HELP);
static void usage(FILE* out) {
    fputs(
    "Usage: scarletnx-android [options] APK\n"
    "Runs an Android app: its DEX code translated to JVM classes, with ScarletNX's own Android framework. APK is\n"
    "an APK, or a bundle of a base APK and its splits (XAPK, APKS), which is installed into the data folder first.\n"
    "\n",
        out);
    snx_cli_help(out, android_cli);
    fputs(
    "Android options:\n"
    "  --translate=DIR        Translate every class of the APK ahead of time into DIR, a .class file for each,\n"
    "                         and stop\n"
    "  -J OPTION              An option for the JVM (-J-Xmx2g)\n"
    "  --jit-opt=LEVEL        LLVM's optimization of the native code the JIT compiles: 0, 1, 2 (the default),\n"
    "                         3, s or z. 0 compiles several times faster, to test the JIT itself\n"
    "  --framework=JAR        The framework's jar, in place of the one beside the program\n"
    "  --java-home=DIR        The JDK to run on (21 or later), in place of the one the build found\n"
    "\n"
    "A run without a user, as the Switch runtime's --script and --shot do it:\n"
    "  --shot-frame=N,M       The frames --shot saves, 60 when not given. On Android, --shot saves each of them\n"
    "                         as a binary PPM, %ld in PATH the frame's number\n"
    "  --taps=SCRIPT          \"60:300,672 90:...\": a finger down at frame 60 at (300, 672), up 3 frames later\n"
    "  --type=TEXT            '120:2+3\\n|300:x': text typed from frame 120 on, as key presses (\\n is Enter)\n"
    "  --key-frames=N         The frames between typed keys, 4 when not given\n"
    "  --pick=PATH            What the system's file pickers answer: the file, or for a folder picker its folder\n"
    "\n"
    "Diagnostics:\n"
    "  --trace-frames         Each frame the app's windows make, on stderr\n"
    "  --log-level=L          The least android.util.Log level printed: V, D (the default), I, W, E or A\n"
    "  --log-signals          Each guest signal action, send and delivery\n"
    "  --sample               Every 5 s, each guest thread's pc, x30 and HLE call, and the Vulkan presents\n"
    "  --jit-verbose          Each unit the JIT compiles, before and after, with its time\n"
    "  --jit-keep-ir=DIR      The LLVM IR of each unit the JIT compiles, saved in DIR\n"
    "  --linker-verbose       Each native library and its load address\n"
    "  --native-test LIB [SYMBOL...]\n"
    "                         Load an AArch64 library through the native bridge and call each function\n",
        out);
}

typedef struct {
    unsigned warnings;
} Translation;
static void warn(void* context, const char* message) {
    Translation* t = context;
    if (t->warnings++ < 50) fprintf(stderr, "scarletnx-android: %s\n", message);
}

/* every class of every DEX file of the APK (classes.dex, classes2.dex and on) into folder */
static int translate_apk(const char* path, const char* folder) {
    char error[512];
    AndroidInstall install;
    if (!android_install(path, &install, error, sizeof error)) {
        fprintf(stderr, "scarletnx-android: %s\n", error);
        return 1;
    }
    SnxZip* zip = snx_zip_open(install.base, error, sizeof error);
    android_install_free(&install);
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
        !android_sqlite_register(env) || !android_text_register(env) || !android_image_register(env) ||
        !android_render_register(env) || !android_bridge_register(env) || !android_egl_register(env) ||
        !android_audio_register(env) || !android_fds_register(env))
        return;
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

/* the framework's jar: --framework's, or the one beside the program */
static char* framework_jar(const char* argv0, const char* named) {
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

/* ScarletNX's Network setting (settings.txt's network: 1 online, 0 offline with loopback only, the default), as the
   launcher keeps it for every run in settings.ini of its config folder */
static bool network_online(void) {
    char* data = snx_user_data_dir();
    char* config = data ? snx_app_kind_dir(data, SNX_USER_CONFIG) : NULL;
    char* path = config ? snx_path_join(config, "settings.ini") : NULL;
    free(data);
    free(config);
    size_t size;
    char* text = path ? (char*)snx_read_file(path, &size) : NULL;
    free(path);
    bool online = false;
    if (text) {
        SnxIni ini;
        char *key, *value;
        snx_ini_start(&ini, text);
        while (snx_ini_next(&ini, &key, &value))
            if (!strcmp(key, "network")) online = atoi(value) == 1;
        free(text);
    }
    return online;
}

static int run_apk(const char* argv0, const char* apk, const char* framework, char** options, size_t option_count) {
    char error[1024];
    AndroidInstall install;
    if (!android_install(apk, &install, error, sizeof error)) {
        fprintf(stderr, "scarletnx-android: %s\n", error);
        return 1;
    }
    hle_proc_set_app(install.package);
    hle_android_set_assets(android_app_assets());
    hle_vulkan_set_host(android_render_host());
    if (!android_app_open(&install, error, sizeof error)) {
        fprintf(stderr, "scarletnx-android: %s\n", error);
        android_app_close();
        android_install_free(&install);
        return 1;
    }
    char* jar = framework_jar(argv0, framework);
    if (!jar || !snx_is_file(jar)) {
        fprintf(stderr, "scarletnx-android: no framework jar at %s\n", jar ? jar : "(none)");
        free(jar);
        android_app_close();
        android_install_free(&install);
        return 1;
    }
    /* the fonts beside the jar (the build copies them there); an option the user gives comes later and wins */
    char* folder = snx_dirname(jar);
    char* fonts = folder ? snx_path_join(folder, "android-fonts") : NULL;
    char* fonts_option = fonts ? snx_format("-Dsnx.android.fonts=%s", fonts) : NULL;
    /* the app's data folder, which the install made, and whether it has the network (on both sides of the bridge) */
    bool online = network_online();
    hle_set_network(online);
    fprintf(stderr, "scarletnx-android: the network is %s (the Network setting)\n", online ? "on" : "off");
    char* data_option = snx_format("-Dsnx.android.data=%s", install.data_dir);
    char* network_option = snx_format("-Dsnx.android.network=%d", online ? 1 : 0);
    char** all = calloc(option_count + 3, sizeof *all);
    free(folder);
    free(fonts);
    if (!fonts_option || !data_option || !network_option || !all) {
        free(fonts_option);
        free(data_option);
        free(network_option);
        free(all);
        free(jar);
        android_app_close();
        android_install_free(&install);
        return 1;
    }
    all[0] = fonts_option;
    all[1] = data_option;
    all[2] = network_option;
    for (size_t i = 0; i < option_count; ++i) all[i + 3] = options[i];
    Run run = {jar, all, option_count + 3, 1};
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
    free(data_option);
    free(network_option);
    free(jar);
    android_app_close();
    android_install_free(&install);
    return run.status;
}

/* a value option's value: argv's after "--name=", NULL for another option */
static const char* value_of(const char* arg, const char* name) {
    size_t n = strlen(name);
    return !strncmp(arg, name, n) && arg[n] == '=' ? arg + n + 1 : NULL;
}

/* the options that the framework's Java side reads, as system properties: --name=value (or --name, true) into
   -Dsnx.android.<property>=value */
static const struct {
    const char* option;
    const char* property;
} java_options[] = {
    {"--trace-frames", "trace_frames"}, {"--taps", "taps"},           {"--type", "type"},
    {"--key-frames", "key_frames"},     {"--log-level", "log_level"},
    {"--pick", "pick"},
};

static void free_options(char** options, char** owned, size_t owned_count) {
    for (size_t k = 0; k < owned_count; ++k) free(owned[k]);
    free(owned);
    free(options);
}

int snx_android_main(int argc, char** argv) {
    const char *apk = NULL, *translate = NULL, *framework = NULL, *shot = NULL, *shot_frames = NULL;
    bool headless = false, log_signals = false, sample = false;
    char** options = calloc((size_t)argc + 1, sizeof *options);
    char** owned = calloc((size_t)argc + 1, sizeof *owned); /* the -D options made here, freed at the end */
    size_t option_count = 0, owned_count = 0;
    if (!options || !owned) {
        free(options);
        free(owned);
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        const char* value;
        bool java = false;
        char error[256];
        SnxCliArg common;
        /* --keys, the text to type before it was --type: the other drivers' --keys names a key file */
        if (!strcmp(argv[i], "--keys") || !strncmp(argv[i], "--keys=", 7)) {
            fputs("scarletnx-android: the text to type is --type=TEXT now. --keys names a key file, which an Android "
                  "app has none of\n",
                  stderr);
            free_options(options, owned, owned_count);
            return 2;
        }
        /* the common options first: --headless is the Java side's too, as -Dsnx.android.headless=true */
        int id = snx_cli_match(argc, argv, &i, android_cli, "scarletnx-android", &common, error, sizeof error);
        if (id < 0 || id == SNX_CLI_HELP) {
            if (id < 0)
                fprintf(stderr, "%s\n", error);
            else
                usage(stdout);
            free_options(options, owned, owned_count);
            return id < 0 ? 2 : 0;
        }
        if (id == SNX_CLI_HEADLESS && !headless) {
            char* d = snx_format("-Dsnx.android.headless=true");
            if (d) options[option_count++] = owned[owned_count++] = d;
            headless = true;
        } else if (id == SNX_CLI_SHOT)
            shot = common.value;
        if (id) continue;
        for (size_t k = 0; k < sizeof java_options / sizeof *java_options && !java; ++k) {
            const char* v = !strcmp(argv[i], java_options[k].option) ? "true" : value_of(argv[i], java_options[k].option);
            if (!v) continue;
            char* d = snx_format("-Dsnx.android.%s=%s", java_options[k].property, v);
            if (d) options[option_count++] = owned[owned_count++] = d;
            java = true;
        }
        if (java) continue;
        if ((value = value_of(argv[i], "--framework"))) {
            framework = value;
            continue;
        }
        if ((value = value_of(argv[i], "--java-home"))) {
            android_jvm_set_home(value);
            continue;
        }
        if ((value = value_of(argv[i], "--shot-frame"))) {
            shot_frames = value;
            continue;
        }
        if (!strcmp(argv[i], "--log-signals") || !strcmp(argv[i], "--sample")) {
            log_signals |= argv[i][2] == 'l';
            sample |= argv[i][2] == 's';
            signals_set_options(log_signals, sample);
            continue;
        }
        if ((value = value_of(argv[i], "--jit-keep-ir"))) {
            bridge_set_keep_ir(value);
            continue;
        }
        if (!strcmp(argv[i], "--jit-verbose")) {
            bridge_set_verbose(true);
            continue;
        }
        if (!strcmp(argv[i], "--linker-verbose")) {
            linker_set_verbose(true);
            continue;
        }
        if (!strcmp(argv[i], "--native-test") && i + 1 < argc) {
            /* a library and the functions to call, through the native bridge */
            int status = android_native_test(argv[i + 1], (const char* const*)argv + i + 2, (unsigned)(argc - i - 2));
            free_options(options, owned, owned_count);
            return status;
        }
        if (!strncmp(argv[i], "--translate=", 12))
            translate = argv[i] + 12;
        else if (!strncmp(argv[i], "--jit-opt=", 10)) {
            if (strlen(argv[i]) != 11 || !bridge_set_optimization(argv[i][10])) {
                fprintf(stderr, "scarletnx-android: --jit-opt takes 0, 1, 2, 3, s or z, not %s\n", argv[i] + 10);
                free_options(options, owned, owned_count);
                return 2;
            }
        }
        else if (!strcmp(argv[i], "-J") && i + 1 < argc)
            options[option_count++] = argv[++i];
        else if (!strncmp(argv[i], "-J", 2) && argv[i][2])
            options[option_count++] = argv[i] + 2;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "scarletnx-android: unknown option %s\n", argv[i]);
            usage(stderr);
            free_options(options, owned, owned_count);
            return 2;
        } else {
            apk = argv[i];
        }
    }
    /* the frames to save, for the Java side to keep presenting a still window until the last (snx.android.ui.Window) */
    if (shot && option_count < (size_t)argc) {
        char* d = snx_format("-Dsnx.android.shot_frames=%s", shot_frames ? shot_frames : "60");
        if (d) options[option_count++] = owned[owned_count++] = d;
    }
    int status;
    if (!apk) {
        usage(stderr);
        status = 2;
    } else {
        android_render_set_options(headless, shot, shot_frames);
        android_audio_set_headless(headless);
        hle_vulkan_set_shot(shot, shot_frames);
        status = translate ? translate_apk(apk, translate) : run_apk(argv[0], apk, framework, options, option_count);
    }
    free_options(options, owned, owned_count);
    return status;
}
