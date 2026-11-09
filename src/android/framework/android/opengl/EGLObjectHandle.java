// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.opengl;

/** An EGL object's native handle. */
public abstract class EGLObjectHandle {
    private final long handle;

    protected EGLObjectHandle(long handle) { this.handle = handle; }
    @Deprecated protected EGLObjectHandle(int handle) { this.handle = handle; }
    @Deprecated public int getHandle() { return (int) handle; }
    public long getNativeHandle() { return handle; }
    @Override public int hashCode() { return Long.hashCode(handle); }
}
