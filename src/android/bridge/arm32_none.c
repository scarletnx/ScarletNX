/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 32-bit native bridge on Windows (arm32.h): not there. Its files take bionic's ILP32 layouts from the host's POSIX
   ones (struct stat, struct tm, the sockets' and the resource limits' headers), which Windows does not have, so the
   build leaves them out and an app's 32-bit library is refused when it loads */
#include "android/bridge/arm32.h"
#include "common/utilities/common.h"

bool arm32_active(void) { return false; }

char* arm32_jni_load(JNIEnv* env, const char* path, const char* library_dir) {
    (void)env, (void)library_dir;
    return snx_format("dlopen failed: \"%s\" is a 32-bit ARM library, which the native bridge does not run on Windows",
                      path);
}

uint64_t arm32_jni_invoke(JNIEnv* env, const char* key, jobject self, jlongArray prims, jobjectArray refs, char* result,
                          bool* found) {
    (void)env, (void)key, (void)self, (void)prims, (void)refs, (void)result;
    *found = false;
    return 0;
}

jobject arm32_jni_allocate(JNIEnv* env, jint capacity, jlongArray address) {
    (void)env, (void)capacity, (void)address;
    return NULL;
}

void arm32_free(uint32_t address) { (void)address; }

uint32_t arm32_jni_activity_create(JNIEnv* env, jobject activity, const char* function, const char* internal,
                                   const char* external, int sdk) {
    (void)env, (void)activity, (void)function, (void)internal, (void)external, (void)sdk;
    return 0;
}

void arm32_jni_activity_call(JNIEnv* env, uint32_t activity, int callback, uint32_t argument) {
    (void)env, (void)activity, (void)callback, (void)argument;
}

uint32_t arm32_input_queue_new(void) { return 0; }

void arm32_input_motion(uint32_t queue, const int32_t* fields, int64_t down, int64_t time, float xprec, float yprec,
                        unsigned count, const int32_t* ids, const int32_t* tools, const float* axes) {
    (void)queue, (void)fields, (void)down, (void)time, (void)xprec, (void)yprec, (void)count, (void)ids, (void)tools,
        (void)axes;
}

void arm32_input_key(uint32_t queue, const int32_t* fields, int64_t down, int64_t time) {
    (void)queue, (void)fields, (void)down, (void)time;
}
