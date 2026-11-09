// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.opengl;

/** A surface of EGL14: the runtime's EGL handle. */
public class EGLSurface extends EGLObjectHandle {
    EGLSurface(long handle) { super(handle); }
    @Override public boolean equals(Object o) { return o instanceof EGLSurface e && e.getNativeHandle() == getNativeHandle(); }
}
