// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.opengl;

/** A display of EGL14: the runtime's EGL handle. */
public class EGLDisplay extends EGLObjectHandle {
    EGLDisplay(long handle) { super(handle); }
    @Override public boolean equals(Object o) { return o instanceof EGLDisplay e && e.getNativeHandle() == getNativeHandle(); }
}
