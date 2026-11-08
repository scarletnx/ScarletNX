// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.opengl;

import snx.android.opengl.Egl10;

/**
 * EGL 1.4 of android.opengl over the runtime's EGL (src/android/bridge/hle_egl.c), as Egl10 is JSR 239's: the
 * objects hold the native handles, one object for each handle, and the arrays are read and written at their offsets
 * as Android's do.
 */
public class EGL14 {
    public static final int EGL_SUCCESS = 0x3000;
    public static final int EGL_NOT_INITIALIZED = 0x3001;
    public static final int EGL_BAD_ACCESS = 0x3002;
    public static final int EGL_BAD_ALLOC = 0x3003;
    public static final int EGL_BAD_ATTRIBUTE = 0x3004;
    public static final int EGL_BAD_CONFIG = 0x3005;
    public static final int EGL_BAD_CONTEXT = 0x3006;
    public static final int EGL_BAD_CURRENT_SURFACE = 0x3007;
    public static final int EGL_BAD_DISPLAY = 0x3008;
    public static final int EGL_BAD_MATCH = 0x3009;
    public static final int EGL_BAD_NATIVE_PIXMAP = 0x300A;
    public static final int EGL_BAD_NATIVE_WINDOW = 0x300B;
    public static final int EGL_BAD_PARAMETER = 0x300C;
    public static final int EGL_BAD_SURFACE = 0x300D;
    public static final int EGL_BUFFER_SIZE = 0x3020;
    public static final int EGL_ALPHA_SIZE = 0x3021;
    public static final int EGL_BLUE_SIZE = 0x3022;
    public static final int EGL_GREEN_SIZE = 0x3023;
    public static final int EGL_RED_SIZE = 0x3024;
    public static final int EGL_DEPTH_SIZE = 0x3025;
    public static final int EGL_STENCIL_SIZE = 0x3026;
    public static final int EGL_CONFIG_CAVEAT = 0x3027;
    public static final int EGL_CONFIG_ID = 0x3028;
    public static final int EGL_LEVEL = 0x3029;
    public static final int EGL_MAX_PBUFFER_HEIGHT = 0x302A;
    public static final int EGL_MAX_PBUFFER_PIXELS = 0x302B;
    public static final int EGL_MAX_PBUFFER_WIDTH = 0x302C;
    public static final int EGL_NATIVE_RENDERABLE = 0x302D;
    public static final int EGL_NATIVE_VISUAL_ID = 0x302E;
    public static final int EGL_NATIVE_VISUAL_TYPE = 0x302F;
    public static final int EGL_SAMPLES = 0x3031;
    public static final int EGL_SAMPLE_BUFFERS = 0x3032;
    public static final int EGL_SURFACE_TYPE = 0x3033;
    public static final int EGL_TRANSPARENT_TYPE = 0x3034;
    public static final int EGL_TRANSPARENT_BLUE_VALUE = 0x3035;
    public static final int EGL_TRANSPARENT_GREEN_VALUE = 0x3036;
    public static final int EGL_TRANSPARENT_RED_VALUE = 0x3037;
    public static final int EGL_NONE = 0x3038;
    public static final int EGL_LUMINANCE_SIZE = 0x303D;
    public static final int EGL_ALPHA_MASK_SIZE = 0x303E;
    public static final int EGL_COLOR_BUFFER_TYPE = 0x303F;
    public static final int EGL_RENDERABLE_TYPE = 0x3040;
    public static final int EGL_SLOW_CONFIG = 0x3050;
    public static final int EGL_NON_CONFORMANT_CONFIG = 0x3051;
    public static final int EGL_TRANSPARENT_RGB = 0x3052;
    public static final int EGL_RGB_BUFFER = 0x308E;
    public static final int EGL_LUMINANCE_BUFFER = 0x308F;
    public static final int EGL_VENDOR = 0x3053;
    public static final int EGL_VERSION = 0x3054;
    public static final int EGL_EXTENSIONS = 0x3055;
    public static final int EGL_HEIGHT = 0x3056;
    public static final int EGL_WIDTH = 0x3057;
    public static final int EGL_LARGEST_PBUFFER = 0x3058;
    public static final int EGL_RENDER_BUFFER = 0x3086;
    public static final int EGL_COLORSPACE = 0x3087;
    public static final int EGL_ALPHA_FORMAT = 0x3088;
    public static final int EGL_HORIZONTAL_RESOLUTION = 0x3090;
    public static final int EGL_VERTICAL_RESOLUTION = 0x3091;
    public static final int EGL_PIXEL_ASPECT_RATIO = 0x3092;
    public static final int EGL_SINGLE_BUFFER = 0x3085;
    public static final int EGL_CORE_NATIVE_ENGINE = 0x305B;
    public static final int EGL_DRAW = 0x3059;
    public static final int EGL_READ = 0x305A;
    public static final int EGL_DONT_CARE = -1;
    public static final int EGL_PBUFFER_BIT = 0x01;
    public static final int EGL_PIXMAP_BIT = 0x02;
    public static final int EGL_WINDOW_BIT = 0x04;
    public static final int EGL_FALSE = 0;
    public static final int EGL_TRUE = 1;
    public static final int EGL_DEFAULT_DISPLAY = 0;
    public static final int EGL_CONTEXT_LOST = 0x300E;
    public static final int EGL_BIND_TO_TEXTURE_RGB = 0x3039;
    public static final int EGL_BIND_TO_TEXTURE_RGBA = 0x303A;
    public static final int EGL_MIN_SWAP_INTERVAL = 0x303B;
    public static final int EGL_MAX_SWAP_INTERVAL = 0x303C;
    public static final int EGL_MATCH_NATIVE_PIXMAP = 0x3041;
    public static final int EGL_CONFORMANT = 0x3042;
    public static final int EGL_CLIENT_APIS = 0x308D;
    public static final int EGL_TEXTURE_FORMAT = 0x3080;
    public static final int EGL_TEXTURE_TARGET = 0x3081;
    public static final int EGL_MIPMAP_TEXTURE = 0x3082;
    public static final int EGL_MIPMAP_LEVEL = 0x3083;
    public static final int EGL_BACK_BUFFER = 0x3084;
    public static final int EGL_VG_COLORSPACE = 0x3087;
    public static final int EGL_VG_ALPHA_FORMAT = 0x3088;
    public static final int EGL_SWAP_BEHAVIOR = 0x3093;
    public static final int EGL_MULTISAMPLE_RESOLVE = 0x3099;
    public static final int EGL_CONTEXT_CLIENT_TYPE = 0x3097;
    public static final int EGL_CONTEXT_CLIENT_VERSION = 0x3098;
    public static final int EGL_NO_TEXTURE = 0x305C;
    public static final int EGL_TEXTURE_RGB = 0x305D;
    public static final int EGL_TEXTURE_RGBA = 0x305E;
    public static final int EGL_TEXTURE_2D = 0x305F;
    public static final int EGL_VG_COLORSPACE_sRGB = 0x3089;
    public static final int EGL_VG_COLORSPACE_LINEAR = 0x308A;
    public static final int EGL_VG_ALPHA_FORMAT_NONPRE = 0x308B;
    public static final int EGL_VG_ALPHA_FORMAT_PRE = 0x308C;
    public static final int EGL_DISPLAY_SCALING = 10000;
    public static final int EGL_BUFFER_PRESERVED = 0x3094;
    public static final int EGL_BUFFER_DESTROYED = 0x3095;
    public static final int EGL_OPENVG_IMAGE = 0x3096;
    public static final int EGL_MULTISAMPLE_RESOLVE_DEFAULT = 0x309A;
    public static final int EGL_MULTISAMPLE_RESOLVE_BOX = 0x309B;
    public static final int EGL_OPENGL_ES_API = 0x30A0;
    public static final int EGL_OPENVG_API = 0x30A1;
    public static final int EGL_OPENGL_API = 0x30A2;
    public static final int EGL_VG_COLORSPACE_LINEAR_BIT = 0x0020;
    public static final int EGL_VG_ALPHA_FORMAT_PRE_BIT = 0x0040;
    public static final int EGL_MULTISAMPLE_RESOLVE_BOX_BIT = 0x0200;
    public static final int EGL_SWAP_BEHAVIOR_PRESERVED_BIT = 0x0400;
    public static final int EGL_OPENGL_ES_BIT = 0x0001;
    public static final int EGL_OPENVG_BIT = 0x0002;
    public static final int EGL_OPENGL_ES2_BIT = 0x0004;
    public static final int EGL_OPENGL_BIT = 0x0008;
    public static final int EGL_UNKNOWN = -1;

    public static final EGLDisplay EGL_NO_DISPLAY = new EGLDisplay(0);
    public static final EGLContext EGL_NO_CONTEXT = new EGLContext(0);
    public static final EGLSurface EGL_NO_SURFACE = new EGLSurface(0);

    public EGL14() {}

    private static final java.util.HashMap<Long, EGLObjectHandle> objects = new java.util.HashMap<>();
    @SuppressWarnings("unchecked")
    private static synchronized <T extends EGLObjectHandle> T object(long h, java.util.function.LongFunction<T> make, T none) {
        if (h == 0) return none;
        return (T) objects.computeIfAbsent(h, make::apply);
    }
    private static synchronized void forget(long h) { objects.remove(h); }
    private static long h(EGLObjectHandle o) { return o != null ? o.getNativeHandle() : 0; }
    private static int[] window(int[] array, int offset) {
        if (array == null) return null;
        if (offset < 0 || offset > array.length) throw new IllegalArgumentException("offset < 0 || offset > length");
        return offset == 0 ? array : java.util.Arrays.copyOfRange(array, offset, array.length);
    }

    public static int eglGetError() { return Egl10.nativeError(); }
    public static EGLDisplay eglGetDisplay(int display_id) { return object(Egl10.nativeDisplay(), EGLDisplay::new, EGL_NO_DISPLAY); }
    public static EGLDisplay eglGetDisplay(long display_id) { return eglGetDisplay((int) display_id); }
    public static boolean eglInitialize(EGLDisplay dpy, int[] major, int majorOffset, int[] minor, int minorOffset) {
        int[] version = new int[2];
        if (!Egl10.nativeInitialize(h(dpy), version)) return false;
        if (major != null) major[majorOffset] = version[0];
        if (minor != null) minor[minorOffset] = version[1];
        return true;
    }
    public static boolean eglTerminate(EGLDisplay dpy) { return Egl10.nativeTerminate(h(dpy)); }
    public static String eglQueryString(EGLDisplay dpy, int name) { return Egl10.nativeQueryString(h(dpy), name); }
    public static boolean eglGetConfigs(EGLDisplay dpy, EGLConfig[] configs, int configsOffset, int config_size, int[] num_config, int num_configOffset) {
        return eglChooseConfig(dpy, new int[] {EGL_NONE}, 0, configs, configsOffset, config_size, num_config, num_configOffset);
    }
    public static boolean eglChooseConfig(EGLDisplay dpy, int[] attrib_list, int attrib_listOffset, EGLConfig[] configs, int configsOffset,
                                          int config_size, int[] num_config, int num_configOffset) {
        if (num_config == null || (configs != null && configs.length - configsOffset < config_size)) throw new IllegalArgumentException();
        long[] found = new long[configs != null ? config_size : 0];
        int n = Egl10.nativeChooseConfig(h(dpy), window(attrib_list, attrib_listOffset), configs != null ? found : null);
        if (n < 0) return false;
        num_config[num_configOffset] = configs != null ? Math.min(n, config_size) : n;
        for (int i = 0; configs != null && i < Math.min(n, config_size); ++i) configs[configsOffset + i] = object(found[i], EGLConfig::new, null);
        return true;
    }
    public static boolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, int attribute, int[] value, int offset) {
        int[] v = new int[1];
        if (!Egl10.nativeConfigAttrib(h(dpy), h(config), attribute, v)) return false;
        value[offset] = v[0];
        return true;
    }
    /** A window surface of a SurfaceView's surface (a SurfaceHolder, the Surface itself or the view), the host window */
    public static EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, Object win, int[] attrib_list, int offset) {
        android.view.Surface s = null;
        if (win instanceof android.view.SurfaceHolder holder) s = holder.getSurface();
        else if (win instanceof android.view.Surface surface) s = surface;
        else if (win instanceof android.view.SurfaceView view) s = view.getHolder().getSurface();
        if (s == null || !s.isWindow()) throw new IllegalArgumentException("Make sure the SurfaceView or associated SurfaceHolder has a valid Surface");
        return object(Egl10.nativeCreateWindowSurface(h(dpy), h(config), s.width(), s.height(), window(attrib_list, offset)), EGLSurface::new, EGL_NO_SURFACE);
    }
    public static EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, int[] attrib_list, int offset) {
        return object(Egl10.nativeCreatePbufferSurface(h(dpy), h(config), window(attrib_list, offset)), EGLSurface::new, EGL_NO_SURFACE);
    }
    public static EGLSurface eglCreatePixmapSurface(EGLDisplay dpy, EGLConfig config, int pixmap, int[] attrib_list, int offset) {
        throw new UnsupportedOperationException("eglCreatePixmapSurface");
    }
    public static boolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
        boolean ok = Egl10.nativeDestroySurface(h(dpy), h(surface));
        if (ok) forget(h(surface));
        return ok;
    }
    public static boolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface, int attribute, int[] value, int offset) {
        int[] v = new int[1];
        if (!Egl10.nativeQuerySurface(h(dpy), h(surface), attribute, v)) return false;
        value[offset] = v[0];
        return true;
    }
    public static EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context, int[] attrib_list, int offset) {
        return object(Egl10.nativeCreateContext(h(dpy), h(config), h(share_context), window(attrib_list, offset)), EGLContext::new, EGL_NO_CONTEXT);
    }
    public static boolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
        boolean ok = Egl10.nativeDestroyContext(h(dpy), h(ctx));
        if (ok) forget(h(ctx));
        return ok;
    }
    public static boolean eglQueryContext(EGLDisplay dpy, EGLContext ctx, int attribute, int[] value, int offset) {
        int[] v = new int[1];
        if (!Egl10.nativeQueryContext(h(dpy), h(ctx), attribute, v)) return false;
        value[offset] = v[0];
        return true;
    }
    public static boolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
        return Egl10.nativeMakeCurrent(h(dpy), h(draw), h(read), h(ctx));
    }
    public static EGLContext eglGetCurrentContext() { return object(Egl10.nativeCurrent(0), EGLContext::new, EGL_NO_CONTEXT); }
    public static EGLDisplay eglGetCurrentDisplay() { return object(Egl10.nativeCurrent(1), EGLDisplay::new, EGL_NO_DISPLAY); }
    public static EGLSurface eglGetCurrentSurface(int readdraw) {
        return object(Egl10.nativeCurrent(readdraw == EGL_READ ? 3 : 2), EGLSurface::new, EGL_NO_SURFACE);
    }
    public static boolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) { return Egl10.nativeSwapBuffers(h(dpy), h(surface)); }
    /** OpenGL ES is the only client API, as on Android */
    public static boolean eglBindAPI(int api) { return api == EGL_OPENGL_ES_API; }
    public static int eglQueryAPI() { return EGL_OPENGL_ES_API; }
    public static boolean eglReleaseThread() {
        Egl10.nativeMakeCurrent(Egl10.nativeCurrent(1), 0, 0, 0);
        return true;
    }
    public static boolean eglSwapInterval(EGLDisplay dpy, int interval) { return true; }
    public static boolean eglWaitClient() { return true; }
    public static boolean eglWaitGL() { return true; }
    public static boolean eglWaitNative(int engine) { return true; }
    public static boolean eglSurfaceAttrib(EGLDisplay dpy, EGLSurface surface, int attribute, int value) { return true; }
    public static boolean eglBindTexImage(EGLDisplay dpy, EGLSurface surface, int buffer) { return false; }
    public static boolean eglReleaseTexImage(EGLDisplay dpy, EGLSurface surface, int buffer) { return false; }
    public static boolean eglCopyBuffers(EGLDisplay dpy, EGLSurface surface, int target) { return false; }
}
