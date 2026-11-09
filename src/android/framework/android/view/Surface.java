// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** A window's surface: its handle in the runtime's renderer. */
public class Surface implements android.os.Parcelable {
    public static final int ROTATION_0 = 0, ROTATION_90 = 1, ROTATION_180 = 2, ROTATION_270 = 3;
    private boolean valid = true;

    /** a SurfaceView's: the host window, at the view's size (ANativeWindow_fromSurface reads these) */
    private boolean window;
    private int width, height;

    public Surface() {}
    /** a surface whose frames go to the texture */
    public Surface(android.graphics.SurfaceTexture surfaceTexture) {
        if (surfaceTexture == null) throw new IllegalArgumentException("surfaceTexture must not be null");
        texture = true;
        width = height = 1;
    }
    /** a SurfaceTexture's, or a SurfaceView's on a display that shows nowhere: ANativeWindow_fromSurface gives it a
        window of its own, which shows nowhere */
    private boolean texture;
    /** a SurfaceView's surface on a display that shows nowhere (a virtual display's), at the view's size */
    void setNowhere(boolean valid, int width, int height) {
        window = false;
        texture = valid;
        this.width = width;
        this.height = height;
        this.valid = valid;
    }
    boolean isTexture() { return texture && valid; }
    void setWindow(boolean window, int width, int height) {
        this.window = window;
        this.width = width;
        this.height = height;
        valid = window;
    }
    public boolean isWindow() { return window; }
    public int width() { return width; }
    public int height() { return height; }
    public boolean isValid() { return valid; }
    public void release() { valid = false; }
    /** A canvas whose drawing goes nowhere: no consumer takes this surface's frames */
    public android.graphics.Canvas lockHardwareCanvas() { return new android.graphics.Canvas(); }
    public android.graphics.Canvas lockCanvas(android.graphics.Rect inOutDirty) { return new android.graphics.Canvas(); }
    public void unlockCanvasAndPost(android.graphics.Canvas canvas) {}
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeValue(this); }

    public static class OutOfResourcesException extends RuntimeException {
        public OutOfResourcesException() {}
        public OutOfResourcesException(String name) { super(name); }
    }
}
