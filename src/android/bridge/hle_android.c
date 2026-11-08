/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* libandroid as HLE: the native window (the Abstract of a SurfaceView's surface: the host window), the looper, the
   sensors (none) and the assets of the app's APKs. Each object the guest gets is a host structure, opaque to it, as
   the NDK's are */
#include "android/bridge/bridge.h"
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"
#include "android/bridge/jni.h"
#include "android/bridge/hle_assets.h"
#include "android/bridge/host.h"
#include <errno.h>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#endif
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static const AndroidAssets* assets;

/* a function with nothing to do here, 0 its result */
static void zero_result(CPU* c, void* data) {
    (void)data;
    X(0) = 0;
}
void hle_android_set_assets(const AndroidAssets* a) {
    assets = a;
}

/* ---- ANativeWindow: the SurfaceView's surface, which is the host window at the view's size ---- */

typedef struct {
    uint32_t magic; /* '_wnd', as the platform's */
    int32_t refs;
    int32_t width, height;            /* the view's */
    int32_t buffer_width, buffer_height, format; /* setBuffersGeometry's, 0 for the window's */
} NativeWindow;
static NativeWindow window = {0x5f776e64, 1, 0, 0, 0, 0, 1};
/* a SurfaceTexture's surface: a window of its own, which shows nowhere (a Vulkan surface of it is headless) */
static NativeWindow texture_window = {0x5f776e64, 1, 1, 1, 0, 0, 1};
bool hle_android_is_texture_window(uint64_t w) { return w == (uint64_t)(uintptr_t)&texture_window; }
static NativeWindow* window_of(uint64_t w) { return hle_android_is_texture_window(w) ? &texture_window : &window; }
static pthread_mutex_t window_lock = PTHREAD_MUTEX_INITIALIZER;

uint64_t hle_android_window(int* width, int* height) {
    pthread_mutex_lock(&window_lock);
    *width = window.buffer_width ? window.buffer_width : window.width;
    *height = window.buffer_height ? window.buffer_height : window.height;
    pthread_mutex_unlock(&window_lock);
    return (uint64_t)(uintptr_t)&window;
}

/* ANativeWindow_fromSurface(env, surface): the window, with the surface's size (Surface.width and height) */
void* hle_android_window_from_surface(JNIEnv* env, jobject surface) {
    jclass cls = surface ? (*env)->GetObjectClass(env, surface) : NULL;
    jmethodID is_window = cls ? (*env)->GetMethodID(env, cls, "isWindow", "()Z") : NULL;
    if (!is_window || !(*env)->CallBooleanMethod(env, surface, is_window)) {
        (*env)->ExceptionClear(env);
        jmethodID is_texture = cls ? (*env)->GetMethodID(env, cls, "isTexture", "()Z") : NULL;
        bool texture = is_texture && (*env)->CallBooleanMethod(env, surface, is_texture);
        (*env)->ExceptionClear(env);
        if (!texture) return NULL;
        __atomic_add_fetch(&texture_window.refs, 1, __ATOMIC_RELAXED);
        return &texture_window;
    }
    jint w = (*env)->CallIntMethod(env, surface, (*env)->GetMethodID(env, cls, "width", "()I"));
    jint h = (*env)->CallIntMethod(env, surface, (*env)->GetMethodID(env, cls, "height", "()I"));
    pthread_mutex_lock(&window_lock);
    window.width = w;
    window.height = h;
    ++window.refs;
    pthread_mutex_unlock(&window_lock);
    return &window;
}
static void window_from_surface(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(uintptr_t)hle_android_window_from_surface(bridge_jni_host(X(0)), (jobject)(uintptr_t)X(1));
}
static void window_acquire(CPU* c, void* data) {
    (void)data;
    __atomic_add_fetch(&window_of(X(0))->refs, 1, __ATOMIC_RELAXED);
}
static void window_release(CPU* c, void* data) {
    (void)data;
    __atomic_sub_fetch(&window_of(X(0))->refs, 1, __ATOMIC_RELAXED);
}
/* a window's size: its buffers', else its own */
static void window_size(uint64_t handle, int* w, int* h) {
    if (!hle_android_is_texture_window(handle)) {
        hle_android_window(w, h);
        return;
    }
    pthread_mutex_lock(&window_lock);
    *w = texture_window.buffer_width ? texture_window.buffer_width : texture_window.width;
    *h = texture_window.buffer_height ? texture_window.buffer_height : texture_window.height;
    pthread_mutex_unlock(&window_lock);
}
static void window_width(CPU* c, void* data) {
    (void)data;
    int w, h;
    window_size(X(0), &w, &h);
    X(0) = (uint64_t)(int64_t)w;
}
static void window_height(CPU* c, void* data) {
    (void)data;
    int w, h;
    window_size(X(0), &w, &h);
    X(0) = (uint64_t)(int64_t)h;
}
static void window_format(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(int64_t)window_of(X(0))->format;
}
/* setBuffersGeometry(window, width, height, format): the size the buffers have, scaled to the window; 0 keeps each */
static void window_set_geometry(CPU* c, void* data) {
    (void)data;
    NativeWindow* w = window_of(X(0));
    pthread_mutex_lock(&window_lock);
    w->buffer_width = (int32_t)X(1);
    w->buffer_height = (int32_t)X(2);
    if (X(3)) w->format = (int32_t)X(3);
    pthread_mutex_unlock(&window_lock);
    X(0) = 0;
}

/* ---- ALooper: an epoll set (host.h's) for each thread that prepares one. No guest function polls it here, so a callback the
   guest adds never runs: the sensor queues it carries have no events ---- */

typedef struct {
    int epoll;
    int32_t refs;
} Looper;
static _Thread_local Looper* t_looper;

static void looper_prepare(CPU* c, void* data) {
    (void)data;
    if (!t_looper) {
        t_looper = calloc(1, sizeof *t_looper);
        if (t_looper) {
            t_looper->epoll = host_epoll_create(true);
            t_looper->refs = 1;
        }
    }
    X(0) = (uint64_t)(uintptr_t)t_looper;
}
static void looper_for_thread(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(uintptr_t)t_looper;
}
static void looper_add_fd(CPU* c, void* data) {
    (void)data;
    Looper* l = PTR(0);
    /* ALOOPER_EVENT_INPUT 1 and OUTPUT 2 as EPOLLIN and EPOLLOUT */
    uint32_t events = (X(3) & 1 ? 0x1u : 0) | (X(3) & 2 ? 0x4u : 0);
    X(0) = l && host_epoll_ctl(l->epoll, 1 /* EPOLL_CTL_ADD */, (int)X(1), events, X(2)) == 0 ? 1 : (uint64_t)-1;
}
static void looper_remove_fd(CPU* c, void* data) {
    (void)data;
    Looper* l = PTR(0);
    X(0) = l && host_epoll_ctl(l->epoll, 2 /* EPOLL_CTL_DEL */, (int)X(1), 0, 0) == 0 ? 1 : 0;
}

/* ---- ASensorManager: a device without sensors (Off in docs/android/INTERFACES.md) ---- */

static uint64_t sensor_manager, sensor_queue;
static void sensor_get_instance(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(uintptr_t)&sensor_manager;
}
static void sensor_create_queue(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(uintptr_t)&sensor_queue;
}
static void sensor_list(CPU* c, void* data) {
    (void)data;
    uint64_t none = 0;
    if (X(1)) memcpy(PTR(1), &none, sizeof none);
    X(0) = 0;
}
static void minus_one(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)-1;
}

/* ---- AAssetManager: the app's assets, read through the driver (app.c) ---- */

typedef struct {
    unsigned char* data; /* the whole asset */
    size_t size, at;
    char name[512];
} Asset;
typedef struct {
    char** names;
    size_t count, next;
} AssetDir;
static uint64_t asset_manager;

static void assets_from_java(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(uintptr_t)&asset_manager;
}
static void asset_open(CPU* c, void* data) {
    (void)data;
    size_t size;
    unsigned char* bytes = assets ? assets->read(STR(1), &size) : NULL;
    if (!bytes) {
        X(0) = 0;
        return;
    }
    Asset* a = calloc(1, sizeof *a);
    if (!a) {
        free(bytes);
        X(0) = 0;
        return;
    }
    a->data = bytes;
    a->size = size;
    snprintf(a->name, sizeof a->name, "%s", STR(1));
    X(0) = (uint64_t)(uintptr_t)a;
}
static void asset_close(CPU* c, void* data) {
    (void)data;
    Asset* a = PTR(0);
    if (a) free(a->data);
    free(a);
}
static void asset_read(CPU* c, void* data) {
    (void)data;
    Asset* a = PTR(0);
    size_t n = X(2) < a->size - a->at ? X(2) : a->size - a->at;
    memcpy(PTR(1), a->data + a->at, n);
    a->at += n;
    X(0) = n;
}
static void asset_seek(CPU* c, void* data) {
    (void)data;
    Asset* a = PTR(0);
    int64_t offset = (int64_t)X(1), base = (int)X(2) == SEEK_CUR ? (int64_t)a->at
                                            : (int)X(2) == SEEK_END ? (int64_t)a->size : 0;
    int64_t to = base + offset;
    if (to < 0 || to > (int64_t)a->size) {
        X(0) = (uint64_t)-1;
        return;
    }
    a->at = (size_t)to;
    X(0) = (uint64_t)to;
}
static void asset_length(CPU* c, void* data) {
    (void)data;
    X(0) = ((Asset*)PTR(0))->size;
}
static void asset_remaining(CPU* c, void* data) {
    (void)data;
    const Asset* a = PTR(0);
    X(0) = a->size - a->at;
}
static void asset_buffer(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(uintptr_t)((Asset*)PTR(0))->data;
}
/* openFileDescriptor64(asset, &start, &length): a stored asset's APK, opened, with its place in it; -1 for a deflated
   one, as Android says for a compressed asset */
static void asset_open_fd(CPU* c, void* data) {
    (void)data;
    const Asset* a = PTR(0);
    const char* apk;
    uint64_t offset, size;
    if (!assets || !assets->locate(a->name, &apk, &offset, &size)) {
        X(0) = (uint64_t)-1;
        return;
    }
#ifdef _WIN32
    int fd = _open(apk, _O_RDONLY | _O_BINARY | _O_NOINHERIT);
#else
    int fd = open(apk, O_RDONLY | O_CLOEXEC);
#endif
    if (fd >= 0) {
        memcpy(PTR(1), &offset, sizeof offset);
        memcpy(PTR(2), &size, sizeof size);
    }
    X(0) = (uint64_t)(int64_t)fd;
}
static void asset_open_dir(CPU* c, void* data) {
    (void)data;
    AssetDir* d = calloc(1, sizeof *d);
    if (d && assets) d->names = assets->list(STR(1), &d->count);
    X(0) = (uint64_t)(uintptr_t)d;
}
static void asset_dir_next(CPU* c, void* data) {
    (void)data;
    AssetDir* d = PTR(0);
    X(0) = d && d->next < d->count ? (uint64_t)(uintptr_t)d->names[d->next++] : 0;
}
static void asset_dir_close(CPU* c, void* data) {
    (void)data;
    AssetDir* d = PTR(0);
    if (!d) return;
    for (size_t i = 0; i < d->count; ++i) free(d->names[i]);
    free(d->names);
    free(d);
}

const HleSymbol hle_android[] = {
    {"ANativeWindow_fromSurface", window_from_surface, NULL},
    {"ANativeWindow_acquire", window_acquire, NULL},
    {"ANativeWindow_release", window_release, NULL},
    {"ANativeWindow_getWidth", window_width, NULL},
    {"ANativeWindow_getHeight", window_height, NULL},
    {"ANativeWindow_getFormat", window_format, NULL},
    {"ANativeWindow_setBuffersGeometry", window_set_geometry, NULL},
    {"ALooper_prepare", looper_prepare, NULL},
    {"ALooper_forThread", looper_for_thread, NULL},
    {"ALooper_acquire", zero_result, NULL},
    {"ALooper_release", zero_result, NULL},
    {"ALooper_addFd", looper_add_fd, NULL},
    {"ALooper_removeFd", looper_remove_fd, NULL},
    {"ASensorManager_getInstance", sensor_get_instance, NULL},
    {"ASensorManager_getInstanceForPackage", sensor_get_instance, NULL},
    {"ASensorManager_getDefaultSensor", zero_result, NULL},
    {"ASensorManager_getSensorList", sensor_list, NULL},
    {"ASensorManager_createEventQueue", sensor_create_queue, NULL},
    {"ASensorManager_destroyEventQueue", zero_result, NULL},
    {"ASensorEventQueue_enableSensor", minus_one, NULL},
    {"ASensorEventQueue_disableSensor", minus_one, NULL},
    {"ASensorEventQueue_setEventRate", minus_one, NULL},
    {"ASensorEventQueue_hasEvents", zero_result, NULL},
    {"ASensorEventQueue_getEvents", zero_result, NULL},
    {"AAssetManager_fromJava", assets_from_java, NULL},
    {"AAssetManager_open", asset_open, NULL},
    {"AAssetManager_openDir", asset_open_dir, NULL},
    {"AAssetDir_getNextFileName", asset_dir_next, NULL},
    {"AAssetDir_close", asset_dir_close, NULL},
    {"AAsset_close", asset_close, NULL},
    {"AAsset_read", asset_read, NULL},
    {"AAsset_seek", asset_seek, NULL},
    {"AAsset_seek64", asset_seek, NULL},
    {"AAsset_getLength", asset_length, NULL},
    {"AAsset_getLength64", asset_length, NULL},
    {"AAsset_getRemainingLength", asset_remaining, NULL},
    {"AAsset_getRemainingLength64", asset_remaining, NULL},
    {"AAsset_getBuffer", asset_buffer, NULL},
    {"AAsset_openFileDescriptor64", asset_open_fd, NULL},
    {"AAsset_openFileDescriptor", asset_open_fd, NULL},
    {"", NULL, NULL},
};
