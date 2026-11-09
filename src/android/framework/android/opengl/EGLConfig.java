// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.opengl;

/** A config of EGL14: the runtime's EGL handle. */
public class EGLConfig extends EGLObjectHandle {
    EGLConfig(long handle) { super(handle); }
    @Override public boolean equals(Object o) { return o instanceof EGLConfig e && e.getNativeHandle() == getNativeHandle(); }
}
