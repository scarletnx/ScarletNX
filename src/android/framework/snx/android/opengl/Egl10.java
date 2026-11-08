// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.opengl;

import java.util.HashMap;
import javax.microedition.khronos.egl.EGL10;
import javax.microedition.khronos.egl.EGL11;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.egl.EGLContext;
import javax.microedition.khronos.egl.EGLDisplay;
import javax.microedition.khronos.egl.EGLSurface;
import javax.microedition.khronos.opengles.GL;

/**
 * JSR 239's EGL over the runtime's EGL (src/android/bridge/hle_egl.c), the one that an app's native code calls too:
 * each object holds the native handle, so a context or surface made here is current for the native GLES calls of
 * the same thread. The Java objects of one handle are one object, as Android's are.
 */
public final class Egl10 implements EGL11 {
    private static final class Display extends EGLDisplay { final long h; Display(long h) { this.h = h; } }
    private static final class Config extends EGLConfig { final long h; Config(long h) { this.h = h; } }
    private static final class Surface extends EGLSurface { final long h; Surface(long h) { this.h = h; } }
    private static final class Context extends EGLContext {
        final long h;
        Context(long h) { this.h = h; }
        @Override public GL getGL() { return Gl.INSTANCE; }
    }

    private static final HashMap<Long, Object> objects = new HashMap<>();

    @SuppressWarnings("unchecked")
    private static synchronized <T> T object(long h, java.util.function.LongFunction<T> make) {
        if (h == 0) return null;
        return (T) objects.computeIfAbsent(h, k -> make.apply(k));
    }
    private static synchronized void forget(long h) { objects.remove(h); }
    private static long h(EGLDisplay d) { return d instanceof Display x ? x.h : 0; }
    private static long h(EGLConfig c) { return c instanceof Config x ? x.h : 0; }
    private static long h(EGLSurface s) { return s instanceof Surface x ? x.h : 0; }
    private static long h(EGLContext c) { return c instanceof Context x ? x.h : 0; }

    @Override public EGLDisplay eglGetDisplay(Object native_display) { return object(nativeDisplay(), Display::new); }
    @Override public boolean eglInitialize(EGLDisplay display, int[] major_minor) { return nativeInitialize(h(display), major_minor); }
    @Override public boolean eglTerminate(EGLDisplay display) { return nativeTerminate(h(display)); }
    @Override public String eglQueryString(EGLDisplay display, int name) { return nativeQueryString(h(display), name); }
    @Override public int eglGetError() { return nativeError(); }

    @Override public boolean eglChooseConfig(EGLDisplay display, int[] attrib_list, EGLConfig[] configs, int config_size, int[] num_config) {
        if (num_config == null || (configs != null && configs.length < config_size)) throw new IllegalArgumentException();
        long[] found = new long[configs != null ? config_size : 0];
        int n = nativeChooseConfig(h(display), attrib_list, configs != null ? found : null);
        if (n < 0) return false;
        num_config[0] = n;
        for (int i = 0; configs != null && i < Math.min(n, config_size); ++i) configs[i] = object(found[i], Config::new);
        return true;
    }
    @Override public boolean eglGetConfigs(EGLDisplay display, EGLConfig[] configs, int config_size, int[] num_config) {
        return eglChooseConfig(display, new int[] {EGL_NONE}, configs, config_size, num_config);
    }
    @Override public boolean eglGetConfigAttrib(EGLDisplay display, EGLConfig config, int attribute, int[] value) {
        return nativeConfigAttrib(h(display), h(config), attribute, value);
    }

    @Override public EGLContext eglCreateContext(EGLDisplay display, EGLConfig config, EGLContext share_context, int[] attrib_list) {
        return object(nativeCreateContext(h(display), h(config), h(share_context), attrib_list), Context::new);
    }
    @Override public boolean eglDestroyContext(EGLDisplay display, EGLContext context) {
        long c = h(context);
        boolean ok = nativeDestroyContext(h(display), c);
        if (ok) forget(c);
        return ok;
    }
    @Override public boolean eglQueryContext(EGLDisplay display, EGLContext context, int attribute, int[] value) {
        return nativeQueryContext(h(display), h(context), attribute, value);
    }

    /** A window surface of a SurfaceView's surface (a SurfaceHolder, the Surface itself or the view), the host window */
    @Override public EGLSurface eglCreateWindowSurface(EGLDisplay display, EGLConfig config, Object native_window, int[] attrib_list) {
        android.view.Surface s = null;
        if (native_window instanceof android.view.SurfaceHolder holder) s = holder.getSurface();
        else if (native_window instanceof android.view.Surface surface) s = surface;
        else if (native_window instanceof android.view.SurfaceView view) s = view.getHolder().getSurface();
        if (s == null || !s.isWindow()) throw new IllegalArgumentException("Make sure the SurfaceView or associated SurfaceHolder has a valid Surface");
        return object(nativeCreateWindowSurface(h(display), h(config), s.width(), s.height(), attrib_list), Surface::new);
    }
    @Override public EGLSurface eglCreatePbufferSurface(EGLDisplay display, EGLConfig config, int[] attrib_list) {
        return object(nativeCreatePbufferSurface(h(display), h(config), attrib_list), Surface::new);
    }
    /** No pixmaps on Android either */
    @Override public EGLSurface eglCreatePixmapSurface(EGLDisplay display, EGLConfig config, Object native_pixmap, int[] attrib_list) {
        throw new UnsupportedOperationException("eglCreatePixmapSurface");
    }
    @Override public boolean eglDestroySurface(EGLDisplay display, EGLSurface surface) {
        long s = h(surface);
        boolean ok = nativeDestroySurface(h(display), s);
        if (ok) forget(s);
        return ok;
    }
    @Override public boolean eglQuerySurface(EGLDisplay display, EGLSurface surface, int attribute, int[] value) {
        return nativeQuerySurface(h(display), h(surface), attribute, value);
    }

    @Override public boolean eglMakeCurrent(EGLDisplay display, EGLSurface draw, EGLSurface read, EGLContext context) {
        return nativeMakeCurrent(h(display), h(draw), h(read), h(context));
    }
    @Override public EGLContext eglGetCurrentContext() { return object(nativeCurrent(0), Context::new); }
    @Override public EGLDisplay eglGetCurrentDisplay() { return object(nativeCurrent(1), Display::new); }
    @Override public EGLSurface eglGetCurrentSurface(int readdraw) { return object(nativeCurrent(readdraw == EGL_READ ? 3 : 2), Surface::new); }

    @Override public boolean eglSwapBuffers(EGLDisplay display, EGLSurface surface) { return nativeSwapBuffers(h(display), h(surface)); }
    @Override public boolean eglCopyBuffers(EGLDisplay display, EGLSurface surface, Object native_pixmap) { return false; }
    @Override public boolean eglWaitGL() { return true; }
    @Override public boolean eglWaitNative(int engine, Object bindTarget) { return true; }

    private static native long nativeDisplay();
    private static native boolean nativeInitialize(long display, int[] version);
    private static native boolean nativeTerminate(long display);
    private static native String nativeQueryString(long display, int name);
    private static native int nativeError();
    /** The configs that match, sorted as EGL sorts them, into out (null: the count alone); -1 for an error */
    private static native int nativeChooseConfig(long display, int[] attributes, long[] out);
    private static native boolean nativeConfigAttrib(long display, long config, int attribute, int[] value);
    private static native long nativeCreateContext(long display, long config, long share, int[] attributes);
    private static native boolean nativeDestroyContext(long display, long context);
    private static native boolean nativeQueryContext(long display, long context, int attribute, int[] value);
    private static native long nativeCreateWindowSurface(long display, long config, int width, int height, int[] attributes);
    private static native long nativeCreatePbufferSurface(long display, long config, int[] attributes);
    private static native boolean nativeDestroySurface(long display, long surface);
    private static native boolean nativeQuerySurface(long display, long surface, int attribute, int[] value);
    private static native boolean nativeMakeCurrent(long display, long draw, long read, long context);
    /** The calling thread's current context (0), display (1), draw surface (2) or read surface (3) */
    private static native long nativeCurrent(int which);
    private static native boolean nativeSwapBuffers(long display, long surface);
}
