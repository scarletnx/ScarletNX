// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

import android.os.Handler;

/**
 * A stream of images from a producer (a camera's preview, a video's frames) into a GLES external texture. No producer
 * here gives it images yet: updateTexImage leaves the texture as it is, the transform is the identity and no frame
 * becomes available.
 */
public class SurfaceTexture {
    public interface OnFrameAvailableListener {
        void onFrameAvailable(SurfaceTexture surfaceTexture);
    }

    public static class OutOfResourcesException extends Exception {
        public OutOfResourcesException() {}
        public OutOfResourcesException(String name) { super(name); }
    }

    private int texName;
    private boolean released, attached;
    private int width = 1, height = 1;
    private OnFrameAvailableListener listener;

    public SurfaceTexture(int texName) { this(texName, false); }
    public SurfaceTexture(int texName, boolean singleBufferMode) {
        this.texName = texName;
        attached = true;
    }
    public SurfaceTexture(boolean singleBufferMode) { attached = false; }

    public void setOnFrameAvailableListener(OnFrameAvailableListener listener) { this.listener = listener; }
    public void setOnFrameAvailableListener(OnFrameAvailableListener listener, Handler handler) { this.listener = listener; }
    public void setDefaultBufferSize(int width, int height) {
        this.width = width;
        this.height = height;
    }
    public void updateTexImage() {
        if (released) throw new IllegalStateException("updateTexImage on a released SurfaceTexture");
        if (!attached) throw new IllegalStateException("updateTexImage on a SurfaceTexture not attached to a context");
    }
    public void releaseTexImage() {}
    public void detachFromGLContext() {
        if (!attached) throw new RuntimeException("Error during detachFromGLContext (see logcat for details)");
        attached = false;
    }
    public void attachToGLContext(int texName) {
        if (attached) throw new RuntimeException("Error during attachToGLContext (see logcat for details)");
        this.texName = texName;
        attached = true;
    }
    public void getTransformMatrix(float[] mtx) {
        if (mtx.length != 16) throw new IllegalArgumentException();
        java.util.Arrays.fill(mtx, 0);
        mtx[0] = mtx[5] = mtx[10] = mtx[15] = 1;
    }
    public long getTimestamp() { return 0; }
    public long getDataSpace() { return 0; }
    public void release() { released = true; }
    public boolean isReleased() { return released; }
}
