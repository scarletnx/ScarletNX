/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/android.h"
#include "gui/model.h"
#include "common/utilities/common.h"
#include "gui/i18n.h"
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void gui_android_start(void) {
    const char* files = SDL_GetAndroidInternalStoragePath();
    if (files) setenv("HOME", files, 1);
    if (files) chdir(files); /* then the data folder, gui_android_own_folder */
}

const char* gui_android_own_folder(void) {
    static char folder[1024];
    const char* files = SDL_GetAndroidInternalStoragePath();
    if (!files || snprintf(folder, sizeof folder, "%s/" SNX_PORTABLE_DIR, files) >= (int)sizeof folder) return NULL;
    mkdir(folder, 0770);
    if (access(folder, W_OK) || chdir(folder)) return NULL;
    mkdir("recompiled", 0770);
    return folder;
}

char* gui_android_document_file(const char* uri) {
    SDL_IOStream* io = SDL_IOFromFile(uri, "rb");
    long fd =
        io ? (long)SDL_GetNumberProperty(SDL_GetIOProperties(io), SDL_PROP_IOSTREAM_FILE_DESCRIPTOR_NUMBER, -1) : -1;
    char link[64], path[4096], *file;
    ssize_t n = -1;
    if (fd >= 0) {
        snprintf(link, sizeof link, "/proc/self/fd/%ld", fd);
        n = readlink(link, path, sizeof path - 1);
    }
    if (io) SDL_CloseIO(io);
    if (n <= 0 || path[0] != '/') return NULL;
    path[n] = 0;
    /* the provider may open it on the storage's own file system, which is the user's shared storage */
    if (!strncmp(path, "/data/media/", 12)) {
        file = (char*)malloc((size_t)n + 32);
        if (file) snprintf(file, (size_t)n + 32, "/storage/emulated/%s", path + 12);
    } else
        file = strdup(path);
    if (file && access(file, R_OK)) {
        free(file);
        file = NULL;
    }
    return file;
}

const char* gui_android_libraries(void) {
    static char folder[1024];
    Dl_info info;
    char* slash;
    if (!folder[0] && dladdr((void*)gui_android_libraries, &info) && info.dli_fname &&
        strlen(info.dli_fname) < sizeof folder) {
        strcpy(folder, info.dli_fname);
        slash = strrchr(folder, '/');
        if (slash) *slash = 0;
    }
    return folder[0] ? folder : NULL;
}

/* what the exception thrown says (its toString), in out, the exception cleared: "" when none was */
static void thrown_text(JNIEnv* env, char* out, size_t size) {
    jthrowable thrown = (*env)->ExceptionOccurred(env);
    jclass kind;
    jmethodID said;
    jstring text = NULL;
    const char* chars = NULL;
    out[0] = 0;
    if (!thrown) return;
    (*env)->ExceptionClear(env);
    kind = (*env)->GetObjectClass(env, thrown);
    said = kind ? (*env)->GetMethodID(env, kind, "toString", "()Ljava/lang/String;") : NULL;
    if (said) text = (jstring)(*env)->CallObjectMethod(env, thrown, said);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env); /* toString's own: the class's name is all there is */
    if (text) chars = (*env)->GetStringUTFChars(env, text, NULL);
    snprintf(out, size, "%s", chars ? chars : snx_tr("an exception that says nothing"));
    if (chars) (*env)->ReleaseStringUTFChars(env, text, chars);
    if (text) (*env)->DeleteLocalRef(env, text);
    if (kind) (*env)->DeleteLocalRef(env, kind);
    (*env)->DeleteLocalRef(env, thrown);
}
/* MainActivity's method, with its arguments (none for a "()V" one, which leaves them unread); what it threw, if it did,
   in the message */
static int call(const char* method, const char* signature, const jvalue* args, char* message, size_t size) {
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    jobject activity = (jobject)SDL_GetAndroidActivity();
    jclass activity_class;
    jmethodID id;
    char thrown[512];
    if (!env || !activity) {
        snprintf(message, size, "%s", snx_tr("No Android activity to ask."));
        return 0;
    }
    activity_class = (*env)->GetObjectClass(env, activity);
    id = (*env)->GetMethodID(env, activity_class, method, signature);
    if (id) (*env)->CallVoidMethodA(env, activity, id, args);
    thrown_text(env, thrown, sizeof thrown); /* GetMethodID's NoSuchMethodError too */
    (*env)->DeleteLocalRef(env, activity_class);
    (*env)->DeleteLocalRef(env, activity);
    if (id && !thrown[0]) return 1;
    if (thrown[0])
        snprintf(message, size, snx_tr("MainActivity.%s did not run: %s"), method, thrown);
    else
        snprintf(message, size, snx_tr("MainActivity.%s did not run."), method);
    return 0;
}

/* MainActivity's method that answers UTF-8 text as a byte[], with its arguments: the text, owned; NULL with why in
   message when it did not answer */
static char* ask(const char* method, const char* signature, const jvalue* args, char* message, size_t size) {
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    jobject activity = (jobject)SDL_GetAndroidActivity();
    jclass activity_class;
    jmethodID id;
    jbyteArray answer = NULL;
    char thrown[512], *text = NULL;
    if (!env || !activity) {
        snprintf(message, size, "%s", snx_tr("No Android activity to ask."));
        return NULL;
    }
    activity_class = (*env)->GetObjectClass(env, activity);
    id = (*env)->GetMethodID(env, activity_class, method, signature);
    if (id) answer = (jbyteArray)(*env)->CallObjectMethodA(env, activity, id, args);
    thrown_text(env, thrown, sizeof thrown); /* GetMethodID's NoSuchMethodError too */
    if (answer && !thrown[0]) {
        jsize n = (*env)->GetArrayLength(env, answer);
        text = (char*)snx_alloc((size_t)n + 1);
        (*env)->GetByteArrayRegion(env, answer, 0, n, (jbyte*)text);
        text[n] = 0;
    } else if (thrown[0])
        snprintf(message, size, snx_tr("MainActivity.%s did not run: %s"), method, thrown);
    else
        snprintf(message, size, snx_tr("MainActivity.%s did not run."), method);
    if (answer) (*env)->DeleteLocalRef(env, answer);
    (*env)->DeleteLocalRef(env, activity_class);
    (*env)->DeleteLocalRef(env, activity);
    return text;
}

char* gui_android_exit_reason(long long pid, long long started) {
    char why[640], *text;
    jvalue args[2];
    args[0].i = (jint)pid;
    args[1].j = (jlong)started * 1000;
    text = ask("exitReason", "(IJ)[B", args, why, sizeof why);
    return text ? text : snx_format("the system's reason is unknown: %s", why);
}

int gui_android_play(const char* const* argv, char* message, size_t size) {
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    jclass string;
    jobjectArray args;
    jvalue arg;
    int n = 0, i, ok;
    if (!env) return call("play", "", NULL, message, size);
    while (argv[n]) ++n;
    string = (*env)->FindClass(env, "java/lang/String");
    args = (*env)->NewObjectArray(env, n, string, NULL);
    for (i = 0; i < n; ++i) {
        jstring s = (*env)->NewStringUTF(env, argv[i]);
        (*env)->SetObjectArrayElement(env, args, i, s);
        (*env)->DeleteLocalRef(env, s);
    }
    arg.l = args;
    ok = call("play", "([Ljava/lang/String;)V", &arg, message, size);
    (*env)->DeleteLocalRef(env, args);
    (*env)->DeleteLocalRef(env, string);
    return ok;
}

int gui_android_background(char* message, size_t size) { return call("toBack", "()V", NULL, message, size); }
int gui_android_open_folder(const char* path, char* message, size_t size) {
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    jvalue arg;
    int ok;
    if (!env) return call("openFolder", "", NULL, message, size);
    arg.l = (*env)->NewStringUTF(env, path);
    ok = call("openFolder", "(Ljava/lang/String;)V", &arg, message, size);
    (*env)->DeleteLocalRef(env, arg.l);
    return ok;
}

static SDL_DialogFileCallback folder_done;
static void* folder_data;
int gui_android_pick_folder(SDL_DialogFileCallback done, void* data, char* message, size_t size) {
    folder_done = done;
    folder_data = data;
    return call("pickFolder", "()V", NULL, message, size);
}
/* MainActivity's answer from the picker: the tree's URI as UTF-8, NULL for none picked */
JNIEXPORT void JNICALL Java_org_scarletnx_MainActivity_folderPicked(JNIEnv* env, jclass activity, jbyteArray uri);
JNIEXPORT void JNICALL Java_org_scarletnx_MainActivity_folderPicked(JNIEnv* env, jclass activity, jbyteArray uri) {
    const char* list[2] = {NULL, NULL};
    char *text = NULL, *path = NULL;
    jsize n = uri ? (*env)->GetArrayLength(env, uri) : 0;
    (void)activity;
    if (!folder_done) return;
    if (uri && (text = (char*)malloc((size_t)n + 1))) {
        (*env)->GetByteArrayRegion(env, uri, 0, n, (jbyte*)text);
        text[n] = 0;
        path = gui_document_path(text);
        list[0] = path ? path : text;
    }
    folder_done(folder_data, list, -1);
    free(path);
    free(text);
}

/* text as a byte[], which MainActivity reads as UTF-8: NewStringUTF aborts on anything else than modified UTF-8 */
static jbyteArray bytes(JNIEnv* env, const char* text) {
    jsize n = (jsize)strlen(text);
    jbyteArray array = (*env)->NewByteArray(env, n);
    if (array) (*env)->SetByteArrayRegion(env, array, 0, n, (const jbyte*)text);
    return array;
}

void gui_android_running(const char* game, const char* step, int percent) {
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    char message[64];
    jvalue args[3];
    if (!env) return;
    args[0].l = bytes(env, game);
    args[1].l = bytes(env, step);
    args[2].i = percent;
    if (args[0].l && args[1].l)
        call("running", "([B[BI)V", args, message, sizeof message);
    else
        (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, args[0].l);
    (*env)->DeleteLocalRef(env, args[1].l);
}

void gui_android_ran(const char* result) {
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    char message[64];
    jvalue arg;
    if (!env) return;
    arg.l = result ? bytes(env, result) : NULL;
    if (arg.l || !result)
        call("ran", "([B)V", &arg, message, sizeof message);
    else
        (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, arg.l);
}
