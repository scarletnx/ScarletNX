/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A java.io.FileDescriptor's host descriptor (runtime/fds.h): its private int fd, which JNI reads and sets */
#include "android/runtime/fds.h"
#include "android/runtime/jvm.h"
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#define dup _dup
#define close _close
#else
#include <unistd.h>
#endif

static jfieldID fd_field(JNIEnv* env, jclass* cls) {
    *cls = (*env)->FindClass(env, "java/io/FileDescriptor");
    return *cls ? (*env)->GetFieldID(env, *cls, "fd", "I") : NULL;
}

/* of(FileDescriptor): its descriptor, -1 for none (a Windows file has a handle instead) */
static jint JNICALL fds_of(JNIEnv* env, jclass unused, jobject fd) {
    (void)unused;
    jclass cls;
    jfieldID f = fd ? fd_field(env, &cls) : NULL;
    return f ? (*env)->GetIntField(env, fd, f) : -1;
}
/* wrap(int): a FileDescriptor of the descriptor, which owns it from now on */
static jobject JNICALL fds_wrap(JNIEnv* env, jclass unused, jint fd) {
    (void)unused;
    jclass cls;
    jfieldID f = fd_field(env, &cls);
    jmethodID init = f ? (*env)->GetMethodID(env, cls, "<init>", "()V") : NULL;
    jobject o = init ? (*env)->NewObject(env, cls, init) : NULL;
    if (o) (*env)->SetIntField(env, o, f, fd);
    return o;
}
/* dup(int): a second descriptor of the same open file, or -errno */
static jint JNICALL fds_dup(JNIEnv* env, jclass unused, jint fd) {
    (void)env;
    (void)unused;
    int r = dup(fd);
    return r < 0 ? -errno : r;
}
/* close(int): 0 or -errno */
static jint JNICALL fds_close(JNIEnv* env, jclass unused, jint fd) {
    (void)env;
    (void)unused;
    return close(fd) < 0 ? -errno : 0;
}
/* size(int): the file's size, -1 for a descriptor that is not a regular file */
static jlong JNICALL fds_size(JNIEnv* env, jclass unused, jint fd) {
    (void)env;
    (void)unused;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) return -1;
    return (jlong)st.st_size;
}

bool android_fds_register(JNIEnv* env) {
    static const JNINativeMethod methods[] = {
        {(char*)"of", (char*)"(Ljava/io/FileDescriptor;)I", (void*)fds_of},
        {(char*)"wrap", (char*)"(I)Ljava/io/FileDescriptor;", (void*)fds_wrap},
        {(char*)"dup", (char*)"(I)I", (void*)fds_dup},
        {(char*)"close", (char*)"(I)I", (void*)fds_close},
        {(char*)"size", (char*)"(I)J", (void*)fds_size},
    };
    return android_jvm_register(env, "snx/android/os/Fds", methods, (int)(sizeof methods / sizeof *methods));
}
