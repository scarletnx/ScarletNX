// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.opengl;

/** A context of EGL14: the runtime's EGL handle. */
public class EGLContext extends EGLObjectHandle {
    EGLContext(long handle) { super(handle); }
    @Override public boolean equals(Object o) { return o instanceof EGLContext e && e.getNativeHandle() == getNativeHandle(); }
}
