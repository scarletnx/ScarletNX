// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package javax.microedition.khronos.egl;

import javax.microedition.khronos.opengles.GL;

/** A context, and the EGL of the process (the runtime's, snx.android.opengl.Egl10). */
public abstract class EGLContext {
    private static final EGL EGL_INSTANCE = new snx.android.opengl.Egl10();

    public static EGL getEGL() { return EGL_INSTANCE; }
    public abstract GL getGL();
}
