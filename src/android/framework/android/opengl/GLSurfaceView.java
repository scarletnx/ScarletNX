// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.opengl;

import android.content.Context;
import android.util.AttributeSet;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import java.util.ArrayList;
import javax.microedition.khronos.egl.EGL10;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.egl.EGLContext;
import javax.microedition.khronos.egl.EGLDisplay;
import javax.microedition.khronos.egl.EGLSurface;
import javax.microedition.khronos.opengles.GL;
import javax.microedition.khronos.opengles.GL10;

/**
 * A SurfaceView that a Renderer draws into with GLES, on a thread of its own (the GL thread): it makes the EGL
 * display, context and window surface (the runtime's EGL, snx.android.opengl.Egl10, the one the app's native GLES
 * calls see current), calls onSurfaceCreated once for each context, onSurfaceChanged at each size, then onDrawFrame
 * and a swap for each frame, continuously or when asked. Events queued with queueEvent run on the GL thread before
 * the next frame. onPause stops drawing and gives up the surface, and the context too unless it is kept
 * (setPreserveEGLContextOnPause); onResume starts again, with onSurfaceCreated for a new context.
 */
public class GLSurfaceView extends SurfaceView implements SurfaceHolder.Callback2 {
    public static final int RENDERMODE_WHEN_DIRTY = 0, RENDERMODE_CONTINUOUSLY = 1;
    public static final int DEBUG_CHECK_GL_ERROR = 1, DEBUG_LOG_GL_CALLS = 2;

    private static final int EGL_CONTEXT_CLIENT_VERSION = 0x3098, EGL_OPENGL_ES2_BIT = 4,
        EGL_OPENGL_ES3_BIT_KHR = 0x40;

    public interface Renderer {
        void onSurfaceCreated(GL10 gl, EGLConfig config);
        void onSurfaceChanged(GL10 gl, int width, int height);
        void onDrawFrame(GL10 gl);
    }
    public interface EGLConfigChooser {
        EGLConfig chooseConfig(EGL10 egl, EGLDisplay display);
    }
    public interface EGLContextFactory {
        EGLContext createContext(EGL10 egl, EGLDisplay display, EGLConfig eglConfig);
        void destroyContext(EGL10 egl, EGLDisplay display, EGLContext context);
    }
    public interface EGLWindowSurfaceFactory {
        EGLSurface createWindowSurface(EGL10 egl, EGLDisplay display, EGLConfig config, Object nativeWindow);
        void destroySurface(EGL10 egl, EGLDisplay display, EGLSurface surface);
    }
    public interface GLWrapper {
        GL wrap(GL gl);
    }

    private final Object lock = new Object();
    private final ArrayList<Runnable> events = new ArrayList<>();
    private Renderer renderer;
    private EGLConfigChooser configChooser;
    private EGLContextFactory contextFactory;
    private EGLWindowSurfaceFactory surfaceFactory;
    private GLWrapper wrapper;
    private GLThread thread;
    private int clientVersion, debugFlags, renderMode = RENDERMODE_CONTINUOUSLY, width, height;
    private boolean preserveContext, hasSurface, paused, sizeChanged = true, renderRequested = true, exiting;

    public GLSurfaceView(Context context) {
        super(context);
        getHolder().addCallback(this);
    }
    public GLSurfaceView(Context context, AttributeSet attrs) {
        super(context, attrs);
        getHolder().addCallback(this);
    }

    private void checkRenderThreadState() {
        if (thread != null) throw new IllegalStateException("setRenderer has already been called for this instance.");
    }

    public void setGLWrapper(GLWrapper glWrapper) { wrapper = glWrapper; }
    public void setDebugFlags(int debugFlags) { this.debugFlags = debugFlags; }
    public int getDebugFlags() { return debugFlags; }
    public void setPreserveEGLContextOnPause(boolean preserveOnPause) { preserveContext = preserveOnPause; }
    public boolean getPreserveEGLContextOnPause() { return preserveContext; }

    public void setRenderer(Renderer renderer) {
        checkRenderThreadState();
        if (configChooser == null) configChooser = new ComponentSizeChooser(8, 8, 8, 0, 16, 0);
        if (contextFactory == null) contextFactory = new DefaultContextFactory();
        if (surfaceFactory == null) surfaceFactory = new DefaultWindowSurfaceFactory();
        this.renderer = renderer;
        thread = new GLThread();
        thread.start();
    }
    public void setEGLContextFactory(EGLContextFactory factory) {
        checkRenderThreadState();
        contextFactory = factory;
    }
    public void setEGLWindowSurfaceFactory(EGLWindowSurfaceFactory factory) {
        checkRenderThreadState();
        surfaceFactory = factory;
    }
    public void setEGLConfigChooser(EGLConfigChooser configChooser) {
        checkRenderThreadState();
        this.configChooser = configChooser;
    }
    public void setEGLConfigChooser(boolean needDepth) {
        setEGLConfigChooser(new ComponentSizeChooser(8, 8, 8, 0, needDepth ? 16 : 0, 0));
    }
    public void setEGLConfigChooser(int redSize, int greenSize, int blueSize, int alphaSize, int depthSize, int stencilSize) {
        setEGLConfigChooser(new ComponentSizeChooser(redSize, greenSize, blueSize, alphaSize, depthSize, stencilSize));
    }
    public void setEGLContextClientVersion(int version) {
        checkRenderThreadState();
        clientVersion = version;
    }

    public void setRenderMode(int renderMode) {
        if (renderMode != RENDERMODE_WHEN_DIRTY && renderMode != RENDERMODE_CONTINUOUSLY)
            throw new IllegalArgumentException("renderMode");
        synchronized (lock) {
            this.renderMode = renderMode;
            lock.notifyAll();
        }
    }
    public int getRenderMode() {
        synchronized (lock) { return renderMode; }
    }
    public void requestRender() {
        synchronized (lock) {
            renderRequested = true;
            lock.notifyAll();
        }
    }
    public void queueEvent(Runnable r) {
        if (r == null) throw new IllegalArgumentException("r must not be null");
        synchronized (lock) {
            events.add(r);
            lock.notifyAll();
        }
    }

    public void onPause() {
        synchronized (lock) {
            paused = true;
            lock.notifyAll();
        }
    }
    public void onResume() {
        synchronized (lock) {
            paused = false;
            renderRequested = true;
            lock.notifyAll();
        }
    }

    @Override public void surfaceCreated(SurfaceHolder holder) {
        synchronized (lock) {
            hasSurface = true;
            lock.notifyAll();
        }
    }
    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        synchronized (lock) {
            hasSurface = false;
            lock.notifyAll();
        }
    }
    @Override public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
        synchronized (lock) {
            width = w;
            height = h;
            sizeChanged = true;
            renderRequested = true;
            lock.notifyAll();
        }
    }
    @Override public void surfaceRedrawNeeded(SurfaceHolder holder) { requestRender(); }

    @Override protected void onDetachedFromWindow() {
        super.onDetachedFromWindow();
    }

    /** The first config whose depth and stencil are at least the sizes asked, and whose colors are those asked. */
    private class ComponentSizeChooser implements EGLConfigChooser {
        private final int red, green, blue, alpha, depth, stencil;

        ComponentSizeChooser(int red, int green, int blue, int alpha, int depth, int stencil) {
            this.red = red;
            this.green = green;
            this.blue = blue;
            this.alpha = alpha;
            this.depth = depth;
            this.stencil = stencil;
        }

        @Override public EGLConfig chooseConfig(EGL10 egl, EGLDisplay display) {
            int[] spec = {EGL10.EGL_RED_SIZE, red, EGL10.EGL_GREEN_SIZE, green, EGL10.EGL_BLUE_SIZE, blue,
                EGL10.EGL_ALPHA_SIZE, alpha, EGL10.EGL_DEPTH_SIZE, depth, EGL10.EGL_STENCIL_SIZE, stencil,
                EGL10.EGL_RENDERABLE_TYPE, clientVersion >= 3 ? EGL_OPENGL_ES3_BIT_KHR : clientVersion == 2 ? EGL_OPENGL_ES2_BIT : 1,
                EGL10.EGL_NONE};
            int[] count = new int[1];
            if (!egl.eglChooseConfig(display, spec, null, 0, count) || count[0] <= 0)
                throw new IllegalArgumentException("No configs match configSpec");
            EGLConfig[] configs = new EGLConfig[count[0]];
            egl.eglChooseConfig(display, spec, configs, configs.length, count);
            for (EGLConfig config : configs) {
                if (attribute(egl, display, config, EGL10.EGL_DEPTH_SIZE) < depth
                    || attribute(egl, display, config, EGL10.EGL_STENCIL_SIZE) < stencil)
                    continue;
                if (attribute(egl, display, config, EGL10.EGL_RED_SIZE) == red
                    && attribute(egl, display, config, EGL10.EGL_GREEN_SIZE) == green
                    && attribute(egl, display, config, EGL10.EGL_BLUE_SIZE) == blue
                    && attribute(egl, display, config, EGL10.EGL_ALPHA_SIZE) == alpha)
                    return config;
            }
            return configs[0];
        }

        private int attribute(EGL10 egl, EGLDisplay display, EGLConfig config, int name) {
            int[] value = new int[1];
            return egl.eglGetConfigAttrib(display, config, name, value) ? value[0] : 0;
        }
    }

    private class DefaultContextFactory implements EGLContextFactory {
        @Override public EGLContext createContext(EGL10 egl, EGLDisplay display, EGLConfig config) {
            int[] attributes = clientVersion != 0 ? new int[] {EGL_CONTEXT_CLIENT_VERSION, clientVersion, EGL10.EGL_NONE} : null;
            return egl.eglCreateContext(display, config, EGL10.EGL_NO_CONTEXT, attributes);
        }
        @Override public void destroyContext(EGL10 egl, EGLDisplay display, EGLContext context) {
            egl.eglDestroyContext(display, context);
        }
    }

    private static class DefaultWindowSurfaceFactory implements EGLWindowSurfaceFactory {
        @Override public EGLSurface createWindowSurface(EGL10 egl, EGLDisplay display, EGLConfig config, Object nativeWindow) {
            return egl.eglCreateWindowSurface(display, config, nativeWindow, null);
        }
        @Override public void destroySurface(EGL10 egl, EGLDisplay display, EGLSurface surface) {
            egl.eglDestroySurface(display, surface);
        }
    }

    /** The GL thread: EGL's objects, made when there is a surface and the view is not paused, and the frames. */
    private class GLThread extends Thread {
        private EGL10 egl;
        private EGLDisplay display;
        private EGLConfig config;
        private EGLContext context;
        private EGLSurface surface;
        private GL10 gl;

        GLThread() { super("GLThread"); }

        @Override public void run() {
            try {
                loop();
            } catch (InterruptedException e) {
                // the view is gone
            } finally {
                releaseSurface();
                releaseContext();
            }
        }

        private void releaseSurface() {
            if (surface != null) {
                egl.eglMakeCurrent(display, EGL10.EGL_NO_SURFACE, EGL10.EGL_NO_SURFACE, EGL10.EGL_NO_CONTEXT);
                surfaceFactory.destroySurface(egl, display, surface);
                surface = null;
            }
        }
        private void releaseContext() {
            if (context != null) {
                contextFactory.destroyContext(egl, display, context);
                context = null;
            }
        }

        private void loop() throws InterruptedException {
            boolean createdContext = false;
            while (true) {
                Runnable event = null;
                boolean draw = false, changed = false;
                int w, h;
                synchronized (lock) {
                    while (true) {
                        if (exiting) return;
                        if (!events.isEmpty()) {
                            event = events.remove(0);
                            break;
                        }
                        boolean ready = hasSurface && !paused && width > 0 && height > 0;
                        if (!ready && surface != null) releaseSurface();
                        if (paused && !preserveContext && context != null) {
                            releaseContext();
                            createdContext = false;
                        }
                        if (ready && (renderMode == RENDERMODE_CONTINUOUSLY || renderRequested || sizeChanged)) {
                            draw = true;
                            changed = sizeChanged;
                            sizeChanged = false;
                            renderRequested = false;
                            break;
                        }
                        lock.wait();
                    }
                    w = width;
                    h = height;
                }
                if (event != null) {
                    event.run();
                    continue;
                }
                if (egl == null) {
                    egl = (EGL10) EGLContext.getEGL();
                    display = egl.eglGetDisplay(EGL10.EGL_DEFAULT_DISPLAY);
                    if (!egl.eglInitialize(display, new int[2])) throw new RuntimeException("eglInitialize failed");
                    config = configChooser.chooseConfig(egl, display);
                }
                if (context == null) {
                    context = contextFactory.createContext(egl, display, config);
                    if (context == null) throw new RuntimeException("createContext failed: " + egl.eglGetError());
                    GL g = context.getGL();
                    if (wrapper != null) g = wrapper.wrap(g);
                    gl = g instanceof GL10 gl10 ? gl10 : null;
                    createdContext = true;
                }
                if (surface == null) {
                    surface = surfaceFactory.createWindowSurface(egl, display, config, getHolder());
                    if (surface == null) throw new RuntimeException("createWindowSurface failed: " + egl.eglGetError());
                    changed = true;
                }
                if (!egl.eglMakeCurrent(display, surface, surface, context))
                    throw new RuntimeException("eglMakeCurrent failed: " + egl.eglGetError());
                if (createdContext) {
                    renderer.onSurfaceCreated(gl, config);
                    createdContext = false;
                    changed = true;
                }
                if (changed) renderer.onSurfaceChanged(gl, w, h);
                if (draw) {
                    renderer.onDrawFrame(gl);
                    egl.eglSwapBuffers(display, surface);
                }
            }
        }
    }
}
