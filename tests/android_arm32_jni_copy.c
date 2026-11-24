/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A 32-bit guest's copy of a Java array's elements (src/android/bridge/arm32_jni.c, included whole for its statics):
   the array is the 64-bit VM's, and can hold more bytes than the guest's 4 GiB window. Its size is then no 32 bits,
   where it wrapped to a small block that the whole array was copied into: no copy is made, as when the window is
   full, and none of the array is read. A JNIEnv of the two functions the copy calls stands in for the VM. And the
   arrays of a thread's references end with the thread */
#include "android/bridge/arm32_jni.c"
#include <assert.h>
#include <stdio.h>

static struct {
    jsize length;   /* what GetArrayLength answers */
    unsigned reads; /* the regions asked of the array */
} g_t;

static jsize JNICALL array_length(JNIEnv* env, jarray array) {
    (void)env, (void)array;
    return g_t.length;
}
static void JNICALL long_region(JNIEnv* env, jlongArray array, jsize start, jsize count, jlong* out) {
    (void)env, (void)array, (void)start, (void)count, (void)out;
    g_t.reads++;
}

int main(void) {
    static const typeof(*(JNIEnv)0) functions = {.GetArrayLength = array_length, .GetLongArrayRegion = long_region};
    JNIEnv env = &functions;
    char error[256];
    if (!arm32_init(error, sizeof error)) {
        fprintf(stderr, "android_arm32_jni_copy: the 32-bit bridge does not start: %s\n", error);
        return 1;
    }
    /* 2^29 longs are 4 GiB: with the copy's 8 bytes more, 8 in 32 bits */
    g_t.length = 0x20000000;
    assert(elements(&env, 'J', NULL, 0) == 0 && g_t.reads == 0);
    /* an array that fits is copied whole */
    g_t.length = 4;
    uint32_t copy = elements(&env, 'J', NULL, 0);
    assert(copy && g_t.reads == 1);
    arm32_free(copy);
    /* the thread's end frees the arrays of its local references and frames */
    assert(local_ref((jobject)(uintptr_t)&g_t) && t_locals.items);
    arm32_jni_thread_end();
    assert(!t_locals.items && !t_locals.capacity && !t_frames.marks);
    puts("android_arm32_jni_copy: ok");
    return 0;
}
