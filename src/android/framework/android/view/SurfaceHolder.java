// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.graphics.Canvas;
import android.graphics.Rect;

/** The holder of a surface: its callbacks and its size */
public interface SurfaceHolder {
    @Deprecated int SURFACE_TYPE_NORMAL = 0;
    @Deprecated int SURFACE_TYPE_HARDWARE = 1;
    @Deprecated int SURFACE_TYPE_GPU = 2;
    @Deprecated int SURFACE_TYPE_PUSH_BUFFERS = 3;

    interface Callback {
        void surfaceCreated(SurfaceHolder holder);
        void surfaceChanged(SurfaceHolder holder, int format, int width, int height);
        void surfaceDestroyed(SurfaceHolder holder);
    }
    interface Callback2 extends Callback {
        void surfaceRedrawNeeded(SurfaceHolder holder);
        default void surfaceRedrawNeededAsync(SurfaceHolder holder, Runnable drawingFinished) {
            surfaceRedrawNeeded(holder);
            drawingFinished.run();
        }
    }

    void addCallback(Callback callback);
    void removeCallback(Callback callback);
    boolean isCreating();
    @Deprecated void setType(int type);
    void setFixedSize(int width, int height);
    void setSizeFromLayout();
    void setFormat(int format);
    void setKeepScreenOn(boolean screenOn);
    Canvas lockCanvas();
    Canvas lockCanvas(Rect dirty);
    void unlockCanvasAndPost(Canvas canvas);
    Rect getSurfaceFrame();
    Surface getSurface();
}
