// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.util.AttributeSet;
import java.util.ArrayList;

/**
 * A view whose surface an app draws into itself, with its own GPU API (the Abstract of SurfaceFlinger's layers in
 * docs/android/INTERFACES.md): its surface is the host window, at the view's size. The surface exists while the view is
 * attached and laid out, as Android's callbacks give it. The view itself draws nothing, the hole Android punches.
 */
public class SurfaceView extends View {
    private final Surface surface = new Surface();
    private final ArrayList<SurfaceHolder.Callback> callbacks = new ArrayList<>();
    private final Rect frame = new Rect();
    private int format = PixelFormat.RGB_565, fixedWidth = -1, fixedHeight = -1;
    private boolean attached, created;

    public SurfaceView(Context context) { this(context, null); }
    public SurfaceView(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public SurfaceView(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public SurfaceView(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
    }

    private final SurfaceHolder holder = new SurfaceHolder() {
        @Override public void addCallback(Callback callback) { synchronized (callbacks) { if (!callbacks.contains(callback)) callbacks.add(callback); } }
        @Override public void removeCallback(Callback callback) { synchronized (callbacks) { callbacks.remove(callback); } }
        @Override public boolean isCreating() { return false; }
        @Override @Deprecated public void setType(int type) {}
        @Override public void setFixedSize(int width, int height) {
            fixedWidth = width;
            fixedHeight = height;
            update();
        }
        @Override public void setSizeFromLayout() {
            fixedWidth = fixedHeight = -1;
            update();
        }
        @Override public void setFormat(int f) {
            format = f;
            update();
        }
        @Override public void setKeepScreenOn(boolean screenOn) {}
        @Override public Canvas lockCanvas() { return surface.lockCanvas(null); }
        @Override public Canvas lockCanvas(Rect dirty) { return surface.lockCanvas(dirty); }
        @Override public void unlockCanvasAndPost(Canvas canvas) { surface.unlockCanvasAndPost(canvas); }
        @Override public Rect getSurfaceFrame() { return new Rect(frame); }
        @Override public Surface getSurface() { return surface; }
    };

    public SurfaceHolder getHolder() { return holder; }
    public void setZOrderOnTop(boolean onTop) {}
    public void setZOrderMediaOverlay(boolean isMediaOverlay) {}
    public void setSecure(boolean isSecure) {}
    public Surface getSurfaceControl() { return surface; }

    private SurfaceHolder.Callback[] callbacks() {
        synchronized (callbacks) { return callbacks.toArray(new SurfaceHolder.Callback[0]); }
    }

    /** the surface made, sized or destroyed by the view's state, with the callbacks in Android's order */
    private void update() {
        int w = fixedWidth > 0 ? fixedWidth : getWidth(), h = fixedHeight > 0 ? fixedHeight : getHeight();
        boolean want = attached && getVisibility() == VISIBLE && w > 0 && h > 0;
        if (!want) {
            if (created) {
                for (SurfaceHolder.Callback c : callbacks()) c.surfaceDestroyed(holder);
                surface.setWindow(false, 0, 0);
                created = false;
            }
            return;
        }
        boolean changed = !created || frame.width() != w || frame.height() != h;
        frame.set(0, 0, w, h);
        surface.setWindow(true, w, h);
        if (!created) {
            created = true;
            for (SurfaceHolder.Callback c : callbacks()) c.surfaceCreated(holder);
        }
        if (changed) {
            for (SurfaceHolder.Callback c : callbacks()) c.surfaceChanged(holder, format, w, h);
            for (SurfaceHolder.Callback c : callbacks())
                if (c instanceof SurfaceHolder.Callback2 c2) c2.surfaceRedrawNeeded(holder);
        }
    }

    @Override protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        attached = true;
    }
    @Override protected void onDetachedFromWindow() {
        attached = false;
        update();
        super.onDetachedFromWindow();
    }
    @Override protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        /* after the layout pass, as Android's ViewRootImpl reports a surface once the window is laid out */
        post(this::update);
    }
    @Override protected void onWindowVisibilityChanged(int visibility) {
        super.onWindowVisibilityChanged(visibility);
        post(this::update);
    }
    @Override public void draw(Canvas canvas) {}
    @Override protected void onDraw(Canvas canvas) {}
}
